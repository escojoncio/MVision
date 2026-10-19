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
#include "vr_body.h"
#include "vr_camera.h"
#include "vr_heading.h"
#include "vr_usermap.h"
#include "vr_fire.h"
#include "vr_radar.h"
#include "vr_radar_vrp.h"
#include "vr_scene.h"
#include "vr_view.h"
#include "vrai.h"
#include "vrgame.h"

void hle_vrcam_reset(void);   /* vr_camera_hle.c */
void hle_vrradar_reset(void); /* vr/radar/vr_radar_hle.c */
void hle_vrfire_reset(void);  /* vr/gameplay/vr_fire_hle.c */
#define RADAR_DRAW_ADDR 0x8003bd9cu   /* vr/radar/vr_radar_names.json (edición europea) */
#define BODY_SORT_ADDR 0x80018304u    /* vr/body/vr_names_body.json (fin del dibujado del canal 3D: vrbody_restore) */

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

/* Funciones del motor que crean el proyectil del jugador (vr/gameplay/vr_fire_names.json; make -C vr/host check). */
static const struct { const char *name; uint32_t addr; } FIRE_SITES[] = {
    {"vrfire_bullet", 0x800779a8u},
    {"vrfire_flash_a", 0x8005e6dcu},
    {"vrfire_flash_b", 0x8005e358u},
    {"vrfire_rmissile", 0x8006e128u},
    {"vrfire_amissile", 0x8006eca0u},
    {"vrfire_mine", 0x8006c4b4u},
    {"vrfire_charge", 0x8006b69cu},
    {"vrfire_grenade", 0x8006af98u},
    {"vrfire_hide", 0x80018c8cu},
    {"vrfire_aalock", 0x80071fc8u},
    {"vrfire_aaview", 0x800711acu},
};
#define NFIRE (sizeof FIRE_SITES / sizeof FIRE_SITES[0])
static int body_ok, fire_ok;

/* Huella del código del buscador antiaéreo (edición europea): prólogo de cada función y las dos llamadas internas que usa el sustituto
 * (aaview desde el paso; carga de H del canal desde aaview). Sin ella no se enlazan (las direcciones fijas no se verifican por firma). */
static int aa_code_ok(void) {
    static const struct { uint32_t addr, word; } W[] = {
        {0x80071fc8u, 0x27BDFEA0u},   /* paso: addiu sp,sp,-352 */
        {0x80072010u, 0x0C01C46Bu},   /* paso: jal aaview */
        {0x800711acu, 0x27BDFFE8u},   /* aaview: addiu sp,sp,-24 */
        {0x800711c8u, 0x0C005E06u},   /* aaview: jal (carga de H y rectángulo del canal) */
    };
    for (unsigned i = 0; i < sizeof W / sizeof *W; i++) {
        uint32_t w;
        memcpy(&w, emu_ram + (W[i].addr & (EMU_RAM_SIZE - 1u)), 4);
        if (w != W[i].word) return 0;
    }
    return 1;
}

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
    uint8_t frm[VR_FRAME_W * VR_FRAME_H];    /* copia de VrFilter.frame (máscara de visión: la memoria de vr_scene se reutiliza en la siguiente lista) */
    VrVtx *rv;       size_t rv_cap;          /* escena del radar de muñeca (copia propia; ev_end en índices de `list`) */
    VrBatch *rb;     size_t rb_cap;
    VrState *rst;    size_t rst_cap;
    VrRadarScene rs;
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
static VrView view;                      /* vista vigente: mundo / cine (vr_view.h) */
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
    if (sc->filter.frame_on && sc->filter.frame && sc->filter.frame_w == VR_FRAME_W && sc->filter.frame_h == VR_FRAME_H) {
        memcpy(s->frm, sc->filter.frame, sizeof s->frm);
        s->sc.filter.frame = s->frm;
    } else {
        s->sc.filter.frame_on = 0; s->sc.filter.frame = NULL; s->sc.filter.frame_w = s->sc.filter.frame_h = 0;
    }
    return 0;
}

/* Copia la escena del radar (memoria interna de vr_radar, reutilizada en la siguiente llamada) al slot. Sus ev_end/src_op ya vienen en
 * índices de la lista HUD (split.hud) y por eso se reescriben igual que los del HUD con el prefijo de eventos `pre` (fill_slot). */
static int fill_radar(Slot *s, const VrRadarScene *r, size_t nops_hud) {
    memset(&s->rs, 0, sizeof s->rs);
    if (!r || !r->present || !r->v || !r->b) return 0;
    if (!grow((void **)&s->rv, &s->rv_cap, r->nv, sizeof *s->rv) || !grow((void **)&s->rb, &s->rb_cap, r->nb, sizeof *s->rb) ||
        !grow((void **)&s->rst, &s->rst_cap, r->nstates, sizeof *s->rst))
        return -1;
    memcpy(s->rv, r->v, r->nv * sizeof *s->rv);
    memcpy(s->rb, r->b, r->nb * sizeof *s->rb);
    if (r->nstates) memcpy(s->rst, r->states, r->nstates * sizeof *s->rst);
    for (size_t i = 0; i < r->nb; i++) {
        VrBatch *b = &s->rb[i];
        b->ev_end = pre[b->ev_end < nops_hud ? b->ev_end : nops_hud];
        b->src_op = pre[b->src_op < nops_hud ? b->src_op : nops_hud];
    }
    s->rs = *r;
    s->rs.v = s->rv; s->rs.b = s->rb; s->rs.states = r->nstates ? s->rst : NULL;
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
#ifndef MV_NO_USER_MAP     /* (solo pruebas A/B: sin la correspondencia mundo -> usuario, como antes de §49) */
            vrcam_scene_user_map(&o, r, org[1]);
#endif
        }
    }
    /* Radar de muñeca: se separa del HUD (la escena del HUD se ensambla con la lista SIN sus primitivas) y su propia escena se
     * ensambla y copia ANTES de vr_scene_build (que reutiliza sus búferes). Sin marcas del radar (el juego lo oculta/apaga) no hay nada. */
    VrRadarSplit rsplit;
    VrRadarScene rscene;
    memset(&rscene, 0, sizeof rscene);
    const GpuDrawList *hl = l;
    if (vrradar_split(l, &rsplit) == 0 && rsplit.present) {
        hl = rsplit.hud;
        if (vrradar_build(&rsplit, &rscene) < 0) memset(&rscene, 0, sizeof rscene);
    }
    uint64_t t0 = mono_ns();
    VrScene sc;
    int rc = vr_scene_build(hl, &bs, &o, &sc);
    /* Solo recubrimientos (fundidos/destellos) sin tiras ni copias de pantalla (menús, vídeos, transiciones): el post-proceso solo cubre la vista 3D y no el
     * panel 2D, así que se reconstruye sin filtro y el fundido va al panel como antes. Con tiras (efecto submarino) se conserva el filtro. */
    /* Las gafas (líneas de exploración) y la máscara de visión (máscara de gas, caja) no son fundidos: se conserva el filtro (clase 3b) para que su tinte, sus líneas y su marco
     * cubran toda la vista; también un destello/fundido durante la partida (hay 3D y el jugador está en escena) se aplica sobre la vista en vez de en el panel. */
    int keep3b = sc.filter.n_scan > 0 || sc.filter.frame_on || (sc.has_3d && sc.world_valid);
    if (rc == 0 && o.screen_filter && sc.filter.active && sc.filter.n_strips == 0 && sc.filter.n_polys == 0 && !keep3b) {
        VrSceneOpts o2 = o;
        o2.screen_filter = 0;
        rc = vr_scene_build(hl, &bs, &o2, &sc);
    }
    double ms = (double)(mono_ns() - t0) * 1e-6;
    memcpy(bs.eye_pos, save_pos, sizeof save_pos);
    memcpy(bs.eye_axes, save_axes, sizeof save_axes);

    int ok = rc == 0 && fill_slot(&slot[si], hl, &sc) == 0 && fill_radar(&slot[si], &rscene, hl->nops) == 0;
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
        vrradar_reset();                /* la numeración de seq de la captura vuelve a 0: los rangos de marcas viejos ya no valen */
        cap_on = 0;
    }
}

/* ------------------------------------------------------------------------------------------------ gancho de frame */
static void capture_set(int on) {
    if (on == cap_on) return;
    if (on) {
        gpu_capture_hiprec(1);
        gpu_capture_enable(1);          /* la primera lista trae vram_init */
        vrradar_reset();                /* idem: seq vuelve a 0 */
        pthread_mutex_lock(&MX); stats.cap_resyncs++; pthread_mutex_unlock(&MX);
    } else {
        gpu_capture_enable(0);          /* libera y vacía la cola de captura */
        vrradar_reset();
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
    {
        VrViewIn vin;
        memset(&vin, 0, sizeof vin);
        vin.game = (uint8_t)core;
        vin.in_control = (uint8_t)g.in_control;
        vin.cam_active = (uint8_t)(g.cam_active != 0);
        vin.cinematic = core && bs.cinematic; vin.comms = core && bs.comms; vin.paused = core && bs.paused;
        vin.menu_open = core && bs.menu_open; vin.stopped = core && bs.stopped;
        g.view_mode = vrview_step(&view, &vin);
        g.view_reason = view.reason;
        g.cinematic = vin.cinematic; g.comms = vin.comms; g.paused = vin.paused; g.menu_open = vin.menu_open; g.stopped = vin.stopped;
        g.item = core ? bs.item : -1;
        g.alert_mode = core ? bs.alert_mode : 0;
        g.ammo = -1; g.ammo_max = -1;
        if (core && bs.weapon >= 0 && bs.weapon < BRIDGE_N_WEAPONS) { g.ammo = bs.weapon_ammo[bs.weapon]; g.ammo_max = bs.weapon_max[bs.weapon]; }
    }
    pthread_mutex_lock(&MX);
    memcpy(&pub_bs, &bs, sizeof bs);
    pub_valid = 1;
    pub_gs = g;
    pthread_mutex_unlock(&MX);
}

static void (*test_cb)(void *cpu, uint32_t frame);
void mv_vrhost_set_test_hook(void (*cb)(void *cpu, uint32_t frame)) { test_cb = cb; }

static void frame_hook(Cpu *c, uint32_t f) {
    float turn = mv_vr_take_turn();
    if (turn != 0.0f) vrhead_turn(turn);          /* gira la vista y, en vrhead_frame, el rumbo del personaje */
    if (mv_vr_take_recenter()) vrcam_recenter();
    bridge_read(emu_ram, &bs);
    publish_state();
    vrcam_frame(emu_ram, &bs);
    vrhead_frame(emu_ram, &bs, bs.moving);
    vrai_frame(&bs, f);
    if (test_cb) test_cb(c, f);
    vrgame_frame_end();

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
    vrview_reset(&view);
    hle_vrcam_reset();
    hle_vrradar_reset();
    vrradar_reset();
    hle_vrfire_reset();
    vrgame_reset();
    body_ok = fire_ok = 0;
    {   /* cuerpo: arranca DESACTIVADO; solo se activa si la HLE que restaura las matrices (vrbody_sort) se enlaza (on_bound) */
        VrBodyConfig bc;
        vrbody_config_default(&bc);
        bc.enabled = 0;
        vrbody_init(&bc);
        vrbody_attach_vrcam();
    }
    vrgame_set_enabled(0, 0);
#ifdef MV_NOGEN
    hle_bind_declare_fixed("vrradar_draw");
    hle_bind_declare_fixed("vrbody_sort");
    for (unsigned i = 0; i < NFIRE; i++) hle_bind_declare_fixed(FIRE_SITES[i].name);
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
    cfg.calibrate = 1;                            /* altura/escala del usuario al recentrar (vr_calib.h) */
    vrcam_init(&cfg);
    vrhead_init(NULL);
    pthread_mutex_lock(&MX); stats.cam_ok = vr_ok; pthread_mutex_unlock(&MX);
    if (vr_ok) vrai_on_exe_loaded();   /* IA de enemigos y cámaras: solo con la edición esperada; los parches se instalan en on_bound */
}

static void on_bound(void) {
    if (!vr_ok) return;     /* las direcciones fijas son de la edición esperada del ejecutable: sin esa comprobación no se engancha nada */
    /* Cámara primero: si falla, no se engancha nada más (las demás HLE dependen de la vista de la cámara VR). */
    for (unsigned i = 0; i < NSITES; i++) {
        if (hle_bind_fixed(SITES[i].name, SITES[i].addr) < 0) {
            fprintf(stderr, "[vrhost] no se pudo enlazar %s: sin cámara VR\n", SITES[i].name);
            vr_ok = 0;   /* head_fn devuelve 0: el motor sigue con su cámara (el parche de recorte solo amplía) */
            pthread_mutex_lock(&MX); stats.cam_ok = 0; pthread_mutex_unlock(&MX);
            return;
        }
    }
    if (hle_bind_fixed("vrradar_draw", RADAR_DRAW_ADDR) < 0) fprintf(stderr, "[vrhost] no se pudo enlazar vrradar_draw: el radar seguirá en el HUD\n");
    /* Cuerpo: vrbody_apply (en vrcam_trans) y vrbody_restore (en vrbody_sort) van en pareja; sin la segunda no se activa la primera. */
    if (hle_bind_fixed("vrbody_sort", BODY_SORT_ADDR) < 0) {
        fprintf(stderr, "[vrhost] no se pudo enlazar vrbody_sort: sin IK de los brazos\n");
    } else {
        vrbody_set_enabled(1);
        body_ok = 1;
    }
    /* Disparo: todas o ninguna (con unas sí y otras no, el arma VR dispararía a medias). */
    fire_ok = 1;
    for (unsigned i = 0; i < NFIRE; i++) {
        if (!strncmp(FIRE_SITES[i].name, "vrfire_aa", 9) && !aa_code_ok()) {
            fprintf(stderr, "[vrhost] el código del buscador antiaéreo no coincide: sin arma VR\n");
            fire_ok = 0;
            break;
        }
        if (hle_bind_fixed(FIRE_SITES[i].name, FIRE_SITES[i].addr) < 0) {
            fprintf(stderr, "[vrhost] no se pudo enlazar %s: sin arma VR\n", FIRE_SITES[i].name);
            fire_ok = 0;
        }
    }
    vrgame_set_enabled(body_ok, fire_ok);
    if (vrai_on_bound() < 0) fprintf(stderr, "[vrhost] IA de enemigos no instalada\n");
}

static void on_start(void) {
    /* La captura se enciende cuando aparece un consumidor de escenas (frame_hook). */
    cap_on = 0;
    gpu_capture_set_mark_hook(vrradar_on_mark);   /* runtime_mark.patch */
    sopts = vr_scene_opts_default();
    sopts.screen_filter = 1;   /* el renderer de la app aplica la clase 3 (post-proceso por ojo: onda, tinte y recubrimiento; RenderRG.swift) */
}

/* Mando que ve el juego (hilo de emulación, cada VBlank, antes de aplicarlo): arma VR, disparo y recarga (vrgame.c). `bs` es el puente
 * del VBlank anterior. */
static uint16_t pad_hook(uint16_t buttons) {
    return vrgame_pad(buttons, &bs, emu_ram);
}

/* Radar de muñeca: copia de la percepción activa (cono original + cono ampliado más tenue; nada con el ajuste original). */
static void radar_perception(const VrpConfig *c) { vrradar_set_vrp(c); }

void mv_vrhost_set_ai(int perception, int cameras, int activity) {
    VraiOptions o;
    o.perception = perception; o.cameras = cameras; o.activity = activity;
    vrai_set_options(&o);
}

void mv_vrhost_install(void) {
    vrai_set_perception_listener(radar_perception);
    MvHostHooks h;
    memset(&h, 0, sizeof h);
    h.exe_loaded = on_exe_loaded;
    h.bound = on_bound;
    h.start = on_start;
    h.frame = frame_hook;
    h.pad = pad_hook;
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
        out->view_mode = VRV_CINEMA; out->view_reason = VRV_R_NO_GAME; out->ammo = out->ammo_max = out->item = -1;
    }
    pthread_mutex_unlock(&MX);
}

/* Escena del radar de la última escena entregada por mv_host_scene (misma validez: hasta la siguiente llamada). 0 si esa escena no
 * traía radar (el juego lo oculta, lo apaga o hay cinemática): entonces no hay quad en el brazo. */
int mv_host_radar(VrRadarScene *out) {
    int ok;
    pthread_mutex_lock(&MX);
    ok = held >= 0 && slot[held].rs.present;
    if (ok && out) *out = slot[held].rs;
    pthread_mutex_unlock(&MX);
    return ok;
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
