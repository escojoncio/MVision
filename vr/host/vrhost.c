/* vrhost: ver vrhost.h. Todo lo que toca el estado del runtime corre en el hilo de emulación (ganchos de host_hooks.h);
 * solo la cola de escenas, el BridgeState publicado y la marca de "hay consumidor" se comparten con otros hilos. */
#include "vrhost.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "bridge.h"
#include "cpu.h"
#include "gpu.h"
#include "hlebind.h"
#include "host.h"
#include "host_hooks.h"
#include "vr_camera.h"
#include "vr_scene.h"

void hle_vrcam_reset(void);   /* vr_camera_hle.c */

/* Funciones del motor sustituidas por la capa de cámara y sus direcciones (edición europea del EXE). Deben coincidir con
 * vr/camera/vr_names.json (make -C vr/host check). Se enlazan por dirección porque no hay firma de librería para ellas. */
static const struct { const char *name; uint32_t addr; } SITES[] = {
    {"vrcam_view", 0x80017564u},
    {"vrcam_padorg", 0x80016d98u},
    {"vrcam_trans", 0x8001e840u},
    {"vrcam_draw", 0x80018110u},
    {"vrcam_overscan", 0x80017780u},
};
#define NSITES (sizeof SITES / sizeof SITES[0])

/* ------------------------------------------------------------------------------------------------ estado compartido */
#define NSLOT 6        /* 1 en manos del consumidor + 1 en construcción + hasta NQ en cola */
#define NQ 4

typedef struct {
    GpuDrawList list;                    /* copia compacta: solo los eventos de VRAM (ops) y los datos de subida */
    GpuOp *ops;      size_t ops_cap;
    uint16_t *data;  size_t data_cap;
    uint16_t *vram;  int has_vram;
    VrVtx *v;        size_t v_cap;
    VrBatch *b;      size_t b_cap;
    VrState *st;     size_t st_cap;
    VrScene sc;
} Slot;

enum { S_FREE = 0, S_BUILD, S_QUEUED, S_HELD };

static pthread_mutex_t MX = PTHREAD_MUTEX_INITIALIZER;
static Slot slot[NSLOT];
static int sstate[NSLOT];
static int queue[NQ], q_head, q_n, held = -1;
static BridgeState pub_bs;
static int pub_valid;
static MvVrGameState pub_gs;
static MvVrHostStats stats;
static _Atomic uint64_t last_poll_ns;

/* ------------------------------------------------------------------------------------------------ estado del hilo de emulación */
static BridgeState bs;                   /* último puente leído */
static int vr_ok;                        /* EXE de la edición esperada: parche aplicado y HLE de cámara enlazadas */
static int cap_on;
static VrSceneOpts sopts;
static uint32_t *pre;                    /* prefijo de eventos de VRAM por índice de op (compactación) */
static size_t pre_cap;

static uint64_t mono_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static int grow(void **p, size_t *cap, size_t need, size_t esz) {
    if (need <= *cap) return 1;
    size_t n = *cap * 2;
    if (n < need) n = need;
    if (n < 256) n = 256;
    void *q = realloc(*p, n * esz);
    if (!q) return 0;
    *p = q; *cap = n;
    return 1;
}

/* ------------------------------------------------------------------------------------------------ cola de escenas */
int mv_host_scene(VrScene *out) {
    atomic_store(&last_poll_ns, mono_ns() | 1);
    if (!out) return 0;
    pthread_mutex_lock(&MX);
    if (!q_n) { pthread_mutex_unlock(&MX); return 0; }
    if (held >= 0) sstate[held] = S_FREE;
    int i = queue[q_head];
    q_head = (q_head + 1) % NQ;
    q_n--;
    sstate[i] = S_HELD;
    held = i;
    *out = slot[i].sc;
    stats.delivered++;
    pthread_mutex_unlock(&MX);
    return 1;
}

static int take_free_slot(void) {
    int r = -1;
    pthread_mutex_lock(&MX);
    if (q_n < NQ)
        for (int i = 0; i < NSLOT; i++) if (sstate[i] == S_FREE) { sstate[i] = S_BUILD; r = i; break; }
    pthread_mutex_unlock(&MX);
    return r;
}

static void drop_queue(void) {
    pthread_mutex_lock(&MX);
    while (q_n) { sstate[queue[q_head]] = S_FREE; q_head = (q_head + 1) % NQ; q_n--; }
    pthread_mutex_unlock(&MX);
}

/* Copia la escena recién ensamblada (memoria interna de vr_scene, reutilizada en la siguiente llamada) y los eventos de VRAM
 * de la lista a memoria del slot. De la lista solo se conservan los eventos (subida / copia / relleno) compactados; ev_end y
 * src_op de los batches se reescriben a índices de esa lista compacta. Devuelve 0 o <0 sin memoria. */
static int fill_slot(Slot *s, const GpuDrawList *l, const VrScene *sc) {
    size_t nv = sc->v ? sc->nv : 0, nb = sc->b ? sc->nb : 0, ns = sc->states ? sc->nstates : 0;
    if (!grow((void **)&s->v, &s->v_cap, nv, sizeof *s->v) || !grow((void **)&s->b, &s->b_cap, nb, sizeof *s->b) ||
        !grow((void **)&s->st, &s->st_cap, ns, sizeof *s->st))
        return -1;
    if (nv) memcpy(s->v, sc->v, nv * sizeof *s->v);
    if (nb) memcpy(s->b, sc->b, nb * sizeof *s->b);
    if (ns) memcpy(s->st, sc->states, ns * sizeof *s->st);

    size_t nops = l->nops;
    if (!grow((void **)&pre, &pre_cap, nops + 1, sizeof *pre)) return -1;
    uint32_t nev = 0;
    for (size_t i = 0; i < nops; i++) {
        pre[i] = nev;
        int t = l->ops[i].type;
        if (t == GPU_OP_UPLOAD || t == GPU_OP_VCOPY || t == GPU_OP_FILL) nev++;
    }
    pre[nops] = nev;
    if (!grow((void **)&s->ops, &s->ops_cap, nev, sizeof *s->ops)) return -1;
    uint32_t k = 0;
    for (size_t i = 0; i < nops; i++) {
        int t = l->ops[i].type;
        if (t == GPU_OP_UPLOAD || t == GPU_OP_VCOPY || t == GPU_OP_FILL) s->ops[k++] = l->ops[i];
    }
    for (size_t i = 0; i < nb; i++) {
        VrBatch *b = &s->b[i];
        b->ev_end = pre[b->ev_end < nops ? b->ev_end : nops];
        b->src_op = pre[b->src_op < nops ? b->src_op : nops];
    }
    if (!grow((void **)&s->data, &s->data_cap, l->ndata, sizeof *s->data)) return -1;
    if (l->ndata) memcpy(s->data, l->data, (size_t)l->ndata * sizeof *s->data);
    s->has_vram = 0;
    if (l->vram_init) {
        size_t n = (size_t)GPU_VRAM_W * GPU_VRAM_H;
        if (!s->vram) s->vram = malloc(n * sizeof *s->vram);
        if (!s->vram) return -1;
        memcpy(s->vram, l->vram_init, n * sizeof *s->vram);
        s->has_vram = 1;
    }

    memset(&s->list, 0, sizeof s->list);
    s->list.index = l->index;
    s->list.host_frame = l->host_frame;
    s->list.reason = l->reason;
    s->list.disp = l->disp;
    s->list.ops = nev ? s->ops : NULL;
    s->list.nops = nev;
    s->list.data = l->ndata ? s->data : NULL;
    s->list.ndata = l->ndata;
    s->list.vram_init = s->has_vram ? s->vram : NULL;

    s->sc = *sc;
    s->sc.v = nv ? s->v : NULL;
    s->sc.b = nb ? s->b : NULL;
    s->sc.states = ns ? s->st : NULL;
    s->sc.src = &s->list;
    return 0;
}

/* Registro de cámara con el que se emitió la lista (por número de secuencia del primer polígono con posición de vista).
 * *has3d = 1 si la lista tiene algún polígono con posición de vista (si no, no hay registro que esperar). */
static const VrCamRecord *list_record(const GpuDrawList *l, int *has3d) {
    *has3d = 0;
    for (uint32_t i = 0; i < l->nops; i++)
        if (l->ops[i].type == GPU_OP_POLY && l->ops[i].v[0].vvalid) { *has3d = 1; return vrcam_seq_record(l->ops[i].seq); }
    return NULL;
}

static void build_one(const GpuDrawList *l, int si) {
    /* Los polígonos de una lista se transformaron con la cámara construida un frame del motor ANTES de que el VBlank lea la
     * RAM: la matriz de vista del puente puede ser ya la siguiente. El registro de cámara de la lista (vrcam) trae la correcta. */
    int has3d = 0;
    const VrCamRecord *r = (bs.valid & BRIDGE_OK_EYE) ? list_record(l, &has3d) : NULL;
    int differs = 0;
    int32_t save_pos[3];
    int16_t save_axes[3][3];
    VrSceneOpts o = sopts;
    memcpy(save_pos, bs.eye_pos, sizeof save_pos);
    memcpy(save_axes, bs.eye_axes, sizeof save_axes);
    if (r) {
        for (int i = 0; i < 3; i++) {
            if (bs.eye_pos[i] != r->eye[i]) differs = 1;
            bs.eye_pos[i] = r->eye[i];
            for (int j = 0; j < 3; j++) {
                int16_t v = (int16_t)lrintf(r->M[3 * i + j] * 4096.0f);
                if (bs.eye_axes[i][j] != v) differs = 1;
                bs.eye_axes[i][j] = v;
            }
        }
        if (r->active) {   /* origen del mundo VR = bajo el ojo VR de ese frame (no la posición del jugador de un frame después) */
            float org[3];
            vr_scene_player_origin(&bs, &o, org);
            o.origin_mode = 1;
            o.origin_game[0] = (float)r->eye[0]; o.origin_game[1] = org[1]; o.origin_game[2] = (float)r->eye[2];
        }
    }
    uint64_t t0 = mono_ns();
    VrScene sc;
    int rc = vr_scene_build(l, &bs, &o, &sc);
    double ms = (double)(mono_ns() - t0) * 1e-6;
    memcpy(bs.eye_pos, save_pos, sizeof save_pos);
    memcpy(bs.eye_axes, save_axes, sizeof save_axes);

    int ok = rc == 0 && fill_slot(&slot[si], l, &sc) == 0;
    pthread_mutex_lock(&MX);
    if (ok) {
        sstate[si] = S_QUEUED;
        queue[(q_head + q_n) % NQ] = si;
        q_n++;
        stats.built++;
        if (sc.has_3d) stats.has3d++;
        if (sc.world_valid) stats.world_valid++;
        if (r) { stats.cam_rec_found++; if (differs) stats.cam_rec_differs++; }
        else if (has3d && (bs.valid & BRIDGE_OK_EYE)) stats.cam_rec_missing++;
        stats.build_ms_avg += (ms - stats.build_ms_avg) / (double)stats.built;
        if (ms > stats.build_ms_max) stats.build_ms_max = ms;
    } else {
        sstate[si] = S_FREE;   /* no se entrega: el consumidor perdería eventos de VRAM; la siguiente resincroniza abajo */
    }
    pthread_mutex_unlock(&MX);
    if (!ok) {
        /* hueco en la secuencia de eventos de VRAM: forzar nueva instantánea completa */
        fprintf(stderr, "[vrhost] sin memoria o error al ensamblar la escena (rc=%d): se resincroniza la VRAM\n", rc);
        drop_queue();
        gpu_capture_enable(0);
        cap_on = 0;
    }
}

/* ------------------------------------------------------------------------------------------------ gancho de frame */
static void capture_set(int on) {
    if (on == cap_on) return;
    if (on) {
        gpu_capture_hiprec(1);
        gpu_capture_enable(1);          /* la primera lista trae vram_init */
        pthread_mutex_lock(&MX); stats.cap_resyncs++; pthread_mutex_unlock(&MX);
    } else {
        gpu_capture_enable(0);          /* libera y vacía la cola de captura */
        gpu_capture_hiprec(0);
        drop_queue();
    }
    cap_on = on;
}

static void publish_state(void) {
    MvVrGameState g;
    memset(&g, 0, sizeof g);
    int core = (bs.valid & BRIDGE_OK_ALL_CORE) == BRIDGE_OK_ALL_CORE && bs.has_player;
    g.game = core;
    g.in_control = core && bs.in_control;
    g.menu = !(core && bs.in_control);
    g.posture = core && bs.stance >= 0 && bs.stance <= 2 ? bs.stance : -1;
    g.cam_active = vrcam_active();
    g.first_person = bs.first_person;
    g.weapon = bs.weapon;
    g.life = bs.life; g.life_max = bs.life_max;
    pthread_mutex_lock(&MX);
    memcpy(&pub_bs, &bs, sizeof bs);
    pub_valid = 1;
    pub_gs = g;
    pthread_mutex_unlock(&MX);
}

static void frame_hook(Cpu *c, uint32_t f) {
    (void)c; (void)f;
    float turn = mv_vr_take_turn();
    if (turn != 0.0f) vrcam_turn(turn);
    if (mv_vr_take_recenter()) vrcam_recenter();
    bridge_read(psx_ram, &bs);
    publish_state();
    vrcam_frame(psx_ram, &bs);

    uint64_t lp = atomic_load(&last_poll_ns), now = mono_ns();
    int want = lp && now - lp < 1500000000ull;
    capture_set(want);
    if (!cap_on) return;
    gpu_capture_vblank();
    for (;;) {
        int si = take_free_slot();
        if (si < 0) {   /* cola propia llena: las listas esperan en la captura (si desborda, se resincroniza sola) */
            pthread_mutex_lock(&MX); stats.full_waits++; pthread_mutex_unlock(&MX);
            break;
        }
        const GpuDrawList *l = gpu_capture_frame();
        if (!l) {
            pthread_mutex_lock(&MX); sstate[si] = S_FREE; pthread_mutex_unlock(&MX);
            break;
        }
        build_one(l, si);
        if (!cap_on) break;
    }
}

/* ------------------------------------------------------------------------------------------------ enganches de arranque */
static int head_fn(float m[16], double *t) {
    if (!vr_ok) return 0;
    double age = mv_vr_head_age();
    if (age < 0.0 || age > 0.5) return 0;   /* sin espacio inmersivo (o pose caducada): la cámara del juego manda */
    return mv_vr_get_head(m, t);
}

static void on_exe_loaded(uint8_t *ram, uint32_t base, uint32_t size) {
    vr_ok = 0;
    hle_vrcam_reset();
#ifdef MV_NOGEN
    for (unsigned i = 0; i < NSITES; i++) hle_bind_declare_fixed(SITES[i].name);
    VrCamPatch p;
    vrcam_patch_default(&p);
    int n = vrcam_patch_ram(ram, &p);
    if (n < 0) {
        fprintf(stderr, "[vrhost] el ejecutable no coincide con la edición esperada: sin cámara VR (se usa la del juego)\n");
    } else {
        rt_code_invalidate(base, size);
        vr_ok = 1;
        fprintf(stderr, "[vrhost] parche de recorte aplicado (%d puntos)\n", n);
    }
#else
    (void)ram; (void)base; (void)size;   /* con código recompilado la cámara VR exige regenerarlo con el EXE parcheado: no se usa */
#endif
    VrCamConfig cfg;
    vrcam_config_default(&cfg);
    cfg.head = vr_ok ? head_fn : NULL;
    vrcam_init(&cfg);
    pthread_mutex_lock(&MX); stats.cam_ok = vr_ok; pthread_mutex_unlock(&MX);
}

static void on_bound(void) {
    if (!vr_ok) return;
    for (unsigned i = 0; i < NSITES; i++) {
        if (hle_bind_fixed(SITES[i].name, SITES[i].addr) < 0) {
            fprintf(stderr, "[vrhost] no se pudo enlazar %s: sin cámara VR\n", SITES[i].name);
            vr_ok = 0;   /* head_fn devuelve 0: el motor sigue con su cámara (el parche de recorte solo amplía) */
            pthread_mutex_lock(&MX); stats.cam_ok = 0; pthread_mutex_unlock(&MX);
            return;
        }
    }
}

static void on_start(void) {
    /* La captura se enciende cuando aparece un consumidor de escenas (frame_hook). */
    cap_on = 0;
    sopts = vr_scene_opts_default();
}

void mv_vrhost_install(void) {
    MvHostHooks h;
    memset(&h, 0, sizeof h);
    h.exe_loaded = on_exe_loaded;
    h.bound = on_bound;
    h.start = on_start;
    h.frame = frame_hook;
    mv_host_set_hooks(&h);
}

/* ------------------------------------------------------------------------------------------------ consultas */
void mv_vrhost_game_state(MvVrGameState *out) {
    if (!out) return;
    pthread_mutex_lock(&MX);
    if (pub_valid) {
        *out = pub_gs;
    } else {
        memset(out, 0, sizeof *out);
        out->posture = -1; out->menu = 1; out->weapon = -1;
    }
    pthread_mutex_unlock(&MX);
}

int mv_host_bridge(struct BridgeState *out) {
    int ok;
    pthread_mutex_lock(&MX);
    ok = pub_valid;
    if (ok && out) memcpy(out, &pub_bs, sizeof pub_bs);
    pthread_mutex_unlock(&MX);
    return ok;
}

void mv_vrhost_stats(MvVrHostStats *s) {
    if (!s) return;
    pthread_mutex_lock(&MX);
    *s = stats;
    pthread_mutex_unlock(&MX);
}
