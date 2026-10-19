/* vrgame: ver vrgame.h. */
#include "vrgame.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "bridge.h"
#include "cdrom.h"
#include "disc_img.h"
#include "host.h"
#include "vr_body.h"
#include "vr_camera.h"
#include "vr_fire.h"
#include "vr_hands.h"
#include "vr_pose.h"
#include "vr_reload.h"
#include "wcut.h"
#include "wload.h"

#define PAD_L1 (1u << 10)
#define PAD_R1 (1u << 11)
#define PAD_SQUARE (1u << 15)

/* Modelo del arma en el archivo de stages del usuario: stage residente y id del modelo de la pistola con supresor (vr/weapons/wcut_tables.c). */
#define WPN_STAGE "init"
#define WPN_MODEL 0x36f6u
#define WPN_GAME_ID 0            /* índice de arma del puente que corresponde a esa arma VR (game_weapon_id de la pistola) */

static uint64_t mono_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* ============================================================================================================== matemática */
static void q_mul(const float a[4], const float b[4], float o[4]) {
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
static void q_norm(float q[4]) {
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(l > 1e-6f) || !isfinite(l)) { q[0] = q[1] = q[2] = 0; q[3] = 1; return; }
    for (int i = 0; i < 4; i++) q[i] /= l;
}
static void q_rot(const float q[4], const float v[3], float o[3]) {   /* v + 2w(u x v) + 2 u x (u x v) */
    float t[3] = {2 * (q[1] * v[2] - q[2] * v[1]), 2 * (q[2] * v[0] - q[0] * v[2]), 2 * (q[0] * v[1] - q[1] * v[0])};
    o[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    o[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    o[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}
static void q_conj(const float q[4], float o[4]) { o[0] = -q[0]; o[1] = -q[1]; o[2] = -q[2]; o[3] = q[3]; }
/* matriz 4x4 columna-mayor (columnas = ejes) -> cuaternión */
static void q_from_m16(const float m[16], float q[4]) {
#define R(r, c) m[4 * (c) + (r)]
    float tr = R(0, 0) + R(1, 1) + R(2, 2);
    if (tr > 0) {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s; q[0] = (R(2, 1) - R(1, 2)) / s; q[1] = (R(0, 2) - R(2, 0)) / s; q[2] = (R(1, 0) - R(0, 1)) / s;
    } else if (R(0, 0) > R(1, 1) && R(0, 0) > R(2, 2)) {
        float s = sqrtf(1.0f + R(0, 0) - R(1, 1) - R(2, 2)) * 2.0f;
        q[3] = (R(2, 1) - R(1, 2)) / s; q[0] = 0.25f * s; q[1] = (R(0, 1) + R(1, 0)) / s; q[2] = (R(0, 2) + R(2, 0)) / s;
    } else if (R(1, 1) > R(2, 2)) {
        float s = sqrtf(1.0f + R(1, 1) - R(0, 0) - R(2, 2)) * 2.0f;
        q[3] = (R(0, 2) - R(2, 0)) / s; q[0] = (R(0, 1) + R(1, 0)) / s; q[1] = 0.25f * s; q[2] = (R(1, 2) + R(2, 1)) / s;
    } else {
        float s = sqrtf(1.0f + R(2, 2) - R(0, 0) - R(1, 1)) * 2.0f;
        q[3] = (R(1, 0) - R(0, 1)) / s; q[0] = (R(0, 2) + R(2, 0)) / s; q[1] = (R(1, 2) + R(2, 1)) / s; q[2] = 0.25f * s;
    }
#undef R
    q_norm(q);
}
static void m16_from_pq(const float p[3], const float q[4], float m[16]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y + z * w);     m[2] = 2 * (x * z - y * w);     m[3] = 0;
    m[4] = 2 * (x * y - z * w);     m[5] = 1 - 2 * (x * x + z * z); m[6] = 2 * (y * z + x * w);     m[7] = 0;
    m[8] = 2 * (x * z + y * w);     m[9] = 2 * (y * z - x * w);     m[10] = 1 - 2 * (x * x + y * y); m[11] = 0;
    m[12] = p[0]; m[13] = p[1]; m[14] = p[2]; m[15] = 1;
}
static int finite_n(const float *v, int n) { for (int i = 0; i < n; i++) if (!isfinite(v[i])) return 0; return 1; }

/* ============================================================================================================== buzón de manos */
typedef struct { int valid; float pos[3], rot[4]; float trigger, grip; int low, up, stick; uint64_t t_ns; } HandSlot;
typedef struct { int valid; float p[3], q[4]; } GripPose;

static pthread_mutex_t HM = PTHREAD_MUTEX_INITIALIZER;   /* manos, opciones, hápticos, estado, publicación del arma */
static HandSlot hand_in[2];
static int opt_right = 1, opt_reload = WR_MANUAL;
static float goff_p[3] = {0, 0, 0}, goff_q[4] = {0, 0, 0, 1};
static float hap_out[2];
static MvVrGameStatus pub_status;
static MvVrForearm pub_fore[2];
static int pub_fore_ok[2];
static GripPose pub_sim_hand;      /* mano del arma con la que se simuló el último frame publicado (compensación de latencia) */
static int pub_sim_valid;

#define HAND_MAX_AGE_NS 300000000ull    /* mano sin actualizar más de 0,3 s = sin seguimiento */

void mv_vrgame_set_hand(int32_t side, const MvVrHand *h) {
    if (!h || side < 0 || side > 1) return;
    HandSlot s;
    memset(&s, 0, sizeof s);
    s.valid = h->valid && finite_n(h->pos, 3) && finite_n(h->rot, 4);
    if (s.valid) {
        memcpy(s.pos, h->pos, sizeof s.pos); memcpy(s.rot, h->rot, sizeof s.rot); q_norm(s.rot);
        s.trigger = isfinite(h->trigger) ? h->trigger : 0; s.grip = isfinite(h->grip) ? h->grip : 0;
        s.low = h->low != 0; s.up = h->up != 0; s.stick = h->stick != 0;
    }
    s.t_ns = mono_ns();
    pthread_mutex_lock(&HM);
    hand_in[side] = s;
    pthread_mutex_unlock(&HM);
}
void mv_vrgame_clear_hands(void) {
    pthread_mutex_lock(&HM);
    memset(hand_in, 0, sizeof hand_in);
    pthread_mutex_unlock(&HM);
}
void mv_vrgame_set_options(int32_t right_handed, int32_t reload_mode) {
    pthread_mutex_lock(&HM);
    opt_right = right_handed != 0;
    opt_reload = reload_mode == 1 ? WR_SIMPLIFIED : reload_mode == 2 ? WR_AUTOMATIC : WR_MANUAL;
    pthread_mutex_unlock(&HM);
}
void mv_vrgame_set_grip_offset(const float p[3], const float q[4]) {
    if (!p || !q || !finite_n(p, 3) || !finite_n(q, 4)) return;
    pthread_mutex_lock(&HM);
    memcpy(goff_p, p, sizeof goff_p); memcpy(goff_q, q, sizeof goff_q); q_norm(goff_q);
    pthread_mutex_unlock(&HM);
}

/* Pose del mando -> marco del agarre (vr/body). Con HM tomado. */
static GripPose grip_locked(const HandSlot *s, uint64_t now) {
    GripPose g;
    memset(&g, 0, sizeof g);
    g.q[3] = 1;
    if (!s->valid || now - s->t_ns > HAND_MAX_AGE_NS) return g;
    float off[3];
    q_rot(s->rot, goff_p, off);
    for (int i = 0; i < 3; i++) g.p[i] = s->pos[i] + off[i];
    q_mul(s->rot, goff_q, g.q);
    q_norm(g.q);
    g.valid = 1;
    return g;
}

/* Manos para la IK de los brazos (hilo de emulación, etapa de dibujado). */
int vrpose_get_hands(VrHandPose out[2]) {
    uint64_t now = mono_ns();
    int n = 0;
    pthread_mutex_lock(&HM);
    for (int i = 0; i < 2; i++) {
        GripPose g = grip_locked(&hand_in[i], now);
        memset(&out[i], 0, sizeof out[i]);
        if (!g.valid) continue;
        out[i].valid = 1;
        memcpy(out[i].pos, g.p, sizeof g.p); memcpy(out[i].rot, g.q, sizeof g.q);
        n++;
    }
    pthread_mutex_unlock(&HM);
    return n;
}

int32_t mv_vrgame_hand_matrix(int32_t side, float m[16]) {
    if (!m || side < 0 || side > 1) return 0;
    pthread_mutex_lock(&HM);
    GripPose g = grip_locked(&hand_in[side], mono_ns());
    pthread_mutex_unlock(&HM);
    if (!g.valid) return 0;
    m16_from_pq(g.p, g.q, m);
    return 1;
}
int32_t mv_vrgame_forearm(int32_t side, MvVrForearm *out) {
    if (!out || side < 0 || side > 1) return 0;
    pthread_mutex_lock(&HM);
    int ok = pub_fore_ok[side];
    if (ok) *out = pub_fore[side];
    pthread_mutex_unlock(&HM);
    return ok;
}
void mv_vrgame_take_haptics(float *left, float *right) {
    pthread_mutex_lock(&HM);
    if (left) *left = hap_out[0];
    if (right) *right = hap_out[1];
    hap_out[0] = hap_out[1] = 0;
    pthread_mutex_unlock(&HM);
}
void mv_vrgame_status(MvVrGameStatus *out) {
    if (!out) return;
    pthread_mutex_lock(&HM);
    *out = pub_status;
    pthread_mutex_unlock(&HM);
}

/* ============================================================================================================== carga del arma */
enum { LD_IDLE = 0, LD_LOADING = 1, LD_READY = 2, LD_FAILED = 3 };

static struct {
    _Atomic int state;
    char path[1024];
    int is_dir, disc;
    pthread_t th;
    int th_started;
    /* resultado (los escribe el hilo de carga antes de publicar state = READY) */
    WCutSet cs;
    WeaponDef *def;
    WeaponState *st;
    VrHandsRig *rig;
} LD;

static void *load_thread(void *arg) {
    (void)arg;
    int rc = -1;
    size_t len = 0;
    WLoaded m;
    memset(&m, 0, sizeof m);
    WCutSet cs;
    memset(&cs, 0, sizeof cs);
    uint8_t *arch = disc_load_stage_archive(LD.path, LD.is_dir, &len);
    if (!arch) { fprintf(stderr, "[vrgame] arma VR: el disco no tiene el archivo de modelos legible\n"); goto fail; }
    WLoadOpts o;
    wload_default_opts(&o);
    o.resident = WPN_STAGE;
    o.upscale = 2;
    rc = wload_model(arch, len, WPN_STAGE, (uint16_t)WPN_MODEL, &o, &m);
    free(arch);
    if (rc) { fprintf(stderr, "[vrgame] arma VR: wload_model=%d\n", rc); goto fail; }
    const WCutModel *cm = wcut_find((uint16_t)WPN_MODEL);
    if (!cm || !cm->weapon) { wload_free(&m); fprintf(stderr, "[vrgame] arma VR: sin tabla de corte\n"); goto fail; }
    rc = wcut_run(&m, cm, &cs);
    wload_free(&m);
    if (rc) { fprintf(stderr, "[vrgame] arma VR: wcut_run=%d\n", rc); goto fail; }
    const WeaponDef *base = NULL;
    for (int i = 0; i < w_registry_count; i++) if (w_registry[i] && !strcmp(w_registry[i]->id, cm->weapon)) base = w_registry[i];
    if (!base) { fprintf(stderr, "[vrgame] arma VR: arma no registrada\n"); goto fail2; }
    WeaponDef *def = (WeaponDef *)malloc(sizeof *def);
    WeaponState *st = (WeaponState *)calloc(1, base->state_size);
    if (!def || !st) { free(def); free(st); goto fail2; }
    *def = *base;
    st->def = def;
    def->init(st);                                        /* init pone s->def = &w_pist: se corrige abajo */
    st->def = def;
    weapon_bind(st, cs.meshes, cs.nmeshes, cs.parts, cs.nparts);
    /* El agarre derivado del modelo pasa a ser el del arma: dibujo (vr_hands) y lógica (puntos de interacción, boca) coinciden. */
    WPose g;
    if (vrhands_derive_grip(st, &g) == 0) def->points[WP_GRIP] = g;
    VrHandsConfig hc;
    vrhands_default_config(&hc);
    hc.grip_mode = VRH_GRIP_DEF;
    VrHandsRig *rig = vrhands_create(&hc);
    if (!rig || vrhands_bind(rig, st, cs.atlas, cs.atlas_w, cs.atlas_h)) {
        if (rig) vrhands_destroy(rig);
        free(def); free(st);
        goto fail2;
    }
    LD.cs = cs; LD.def = def; LD.st = st; LD.rig = rig;
    atomic_store_explicit(&LD.state, LD_READY, memory_order_release);
    fprintf(stderr, "[vrgame] arma VR lista (%d piezas)\n", cs.nmeshes);
    return NULL;
fail2:
    wcut_free(&cs);
fail:
    atomic_store_explicit(&LD.state, LD_FAILED, memory_order_release);
    return NULL;
}

/* Hilo de emulación. */
static void load_start(void) {
    if (atomic_load(&LD.state) != LD_IDLE) return;
    if (LD.th_started) { pthread_join(LD.th, NULL); LD.th_started = 0; }
    if (cd_current_source(LD.path, sizeof LD.path, &LD.is_dir) != 0) { atomic_store(&LD.state, LD_FAILED); return; }
    LD.disc = cd_current_disc();
    atomic_store(&LD.state, LD_LOADING);
    if (pthread_create(&LD.th, NULL, load_thread, NULL) != 0) { atomic_store(&LD.state, LD_FAILED); return; }
    LD.th_started = 1;
}

/* ============================================================================================================== simulación del arma */
static struct {
    int body_ok, fire_ok;
    int inited;
    uint64_t last_ns;
    int active;                 /* modo arma VR del VBlank anterior */
    int prev_mag_in;            /* mag_inserted del paso anterior (flanco "cargador insertado") */
    int prev_state;
    int fire_edge;              /* lo pone el callback fire() dentro de update */
    WPose fire_pose;
    int boot_done;              /* la primera toma de munición del arma (arranque) no cuenta como cargador de la cintura */
    WGameBridge br;
    VrReloadOut ro;             /* última salida de la recarga */
    uint32_t shots, arms;
    int last_disc;
    int logged;
} G;

static void publish_off(void) {
    vrhands_post_clear();
    pthread_mutex_lock(&HM);
    pub_sim_valid = 0;
    pthread_mutex_unlock(&HM);
}

void vrgame_reset(void) {
    G.inited = 0; G.active = 0; G.prev_mag_in = 0; G.prev_state = VRR_OFF; G.fire_edge = 0;
    G.boot_done = 0; G.last_ns = 0;
    memset(&G.ro, 0, sizeof G.ro);
    G.ro.sync_rounds = G.ro.sync_chambered = -1;
    publish_off();      /* un arma publicada antes de reiniciar el motor no debe quedarse dibujada */
}
void vrgame_set_enabled(int body_ok, int fire_ok) { G.body_ok = body_ok; G.fire_ok = fire_ok; }

static int cb_ammo_reserve(int id) { return vrreload_ammo_reserve(id); }
static void cb_ammo_take(int id, int n) {
    if (!G.boot_done) return;     /* el cargador con el que arranca el arma es el del juego (se sincroniza): no sale de la cintura */
    vrreload_ammo_take(id, n);
}
static void cb_fire(int id, WPose mz) { (void)id; G.fire_edge = 1; G.fire_pose = mz; G.shots++; }
static void cb_haptic(int hand, float strength, float seconds) {
    (void)seconds;
    int right;
    pthread_mutex_lock(&HM);
    right = opt_right;
    int side = hand == 0 ? (right ? 1 : 0) : (right ? 0 : 1);   /* hand 0 = mano del arma */
    if (isfinite(strength) && strength > hap_out[side]) hap_out[side] = strength > 1 ? 1 : strength;
    pthread_mutex_unlock(&HM);
}

static void init_once(void) {
    if (G.inited) return;
    G.inited = 1;
    VrFireCfg fc;
    vrfire_cfg_default(&fc);
    vrfire_init(&fc);
    VrReloadCfg rc;
    vrreload_cfg_default(&rc);
    vrreload_init(&rc);
    memset(&G.br, 0, sizeof G.br);
    G.br.ammo_reserve = cb_ammo_reserve;
    G.br.ammo_take = cb_ammo_take;
    G.br.fire = cb_fire;
    G.br.haptic = cb_haptic;
    G.ro.sync_rounds = G.ro.sync_chambered = -1;
}

static void head_pose(WPose *h) {
    float m[16];
    memset(h, 0, sizeof *h);
    h->q.w = 1;
    if (!mv_vr_get_head(m, NULL) || !finite_n(m, 16)) return;
    float q[4];
    q_from_m16(m, q);
    h->p.x = m[12]; h->p.y = m[13]; h->p.z = m[14];
    h->q.x = q[0]; h->q.y = q[1]; h->q.z = q[2]; h->q.w = q[3];
}
static WPose wpose_of(const GripPose *g) {
    WPose w;
    w.p.x = g->p[0]; w.p.y = g->p[1]; w.p.z = g->p[2];
    w.q.x = g->q[0]; w.q.y = g->q[1]; w.q.z = g->q[2]; w.q.w = g->q[3];
    return w;
}

uint16_t vrgame_pad(uint16_t pad, const BridgeState *bs, uint8_t *ram) {
    init_once();
    uint64_t now = mono_ns();
    float dt = G.last_ns ? (float)((double)(now - G.last_ns) * 1e-9) : 0.02f;
    G.last_ns = now;
    if (!(dt > 0.001f)) dt = 0.001f;
    if (dt > 0.1f) dt = 0.1f;

    int cam = vrcam_active();
    int core = bs && (bs->valid & BRIDGE_OK_ALL_CORE) == BRIDGE_OK_ALL_CORE && bs->has_player;

    /* manos (una sola toma coherente) */
    HandSlot hs[2];
    GripPose gp[2];
    int right, rmode;
    pthread_mutex_lock(&HM);
    right = opt_right; rmode = opt_reload;
    for (int i = 0; i < 2; i++) { hs[i] = hand_in[i]; gp[i] = grip_locked(&hand_in[i], now); }
    pthread_mutex_unlock(&HM);

    /* cuerpo: agarre de cada mano y mano dominante (la IK de los brazos lee las poses por vrpose_get_hands) */
    vrbody_set_dominant(right ? 1 : 0);
    for (int i = 0; i < 2; i++) vrbody_set_grip(i, gp[i].valid ? hs[i].grip : 0.0f);

    if (atomic_load(&LD.state) == LD_FAILED && cd_current_disc() != LD.disc) {   /* otro disco: se vuelve a intentar */
        if (LD.th_started) { pthread_join(LD.th, NULL); LD.th_started = 0; }
        atomic_store(&LD.state, LD_IDLE);
    }
    if (core && bs->weapon == WPN_GAME_ID && G.fire_ok && cam) load_start();   /* pide el modelo la primera vez que hace falta */
    int ready = atomic_load_explicit(&LD.state, memory_order_acquire) == LD_READY;
    if (ready && LD.th_started) { pthread_join(LD.th, NULL); LD.th_started = 0; }   /* el hilo ya terminó (solo falta recogerlo) */

    int w = right ? 1 : 0, o = 1 - w;   /* w: mando que empuña el arma; o: el otro */
    int equipped = core && bs->weapon == WPN_GAME_ID;
    int mode = ready && G.fire_ok && cam && gp[w].valid && core && (equipped || G.ro.busy);

    /* la recarga corre siempre (si no hay arma VR vuelve a reposo y suelta su contabilidad) */
    VrReloadGame rg;
    vrreload_read_game(bs, &rg);
    VrReloadIn ri;
    memset(&ri, 0, sizeof ri);
    ri.weapon_id = mode ? WPN_GAME_ID : -1;
    ri.mag_in_edge = 0;
    ri.ready = 0;
    if (ready && mode) {
        int mi = LD.st->mag_inserted;
        ri.mag_in_edge = mi && !G.prev_mag_in;
        ri.ready = mi && LD.st->chambered;
        G.prev_mag_in = mi;
    } else {
        G.prev_mag_in = 0;
    }
    VrReloadOut ro;
    vrreload_frame(&rg, &ri, &ro);

    if (!mode) {
        if (G.active) {
            publish_off();
            /* al volver a mostrarla se reinicia el estado de la pistola (cargador del juego otra vez) */
            if (ready) { LD.def->init(LD.st); LD.st->def = LD.def; }
            G.boot_done = 0;
        }
        G.active = 0;
        vrbody_set_armed(0);
        vrfire_set_hide(0);
        VrFireGame fg;
        vrfire_read_game(ram, bs, &fg);
        VrFireIn fi;
        memset(&fi, 0, sizeof fi);
        VrFireOut fo;
        vrfire_frame(&fg, &fi, &fo);
        G.ro = ro;
        uint16_t out = pad;
        if (ro.busy) out = (uint16_t)(out & ~ro.pad_mask);
        out |= ro.pad_force;     /* una maniobra a medias termina aunque el arma ya no esté equipada */
        pthread_mutex_lock(&HM);
        pub_status.loader = atomic_load(&LD.state);
        pub_status.active = 0; pub_status.mag_rounds = pub_status.chambered = pub_status.mag_inserted = 0;
        pub_status.game_mag = pub_status.game_reserve = 0;
        pub_status.reload_state = ro.state;
        pub_status.shots = G.shots;
        pthread_mutex_unlock(&HM);
        return out;
    }

    /* ---- arma VR mostrada y mandando ---- */
    WeaponState *st = LD.st;
    const WeaponDef *def = LD.def;
    G.active = 1;
    vrbody_set_armed(1);
    vrfire_set_hide(1);

    /* tope/sincronía del cargador VR con el del juego (vr_reload.h): solo con cargador dentro y fuera de la espera de recarga */
    if (ro.state == VRR_EMPTY && G.prev_state != VRR_EMPTY) { st->chambered = 0; st->mag_rounds = 0; }   /* el juego vació: el VR también */
    if (ro.state == VRR_READY && st->mag_inserted) {
        if (ro.sync_rounds >= 0) { if (ro.sync_force || st->mag_rounds > ro.sync_rounds) st->mag_rounds = ro.sync_rounds; }
        if (ro.sync_chambered >= 0) { if (ro.sync_force || st->chambered > ro.sync_chambered) st->chambered = ro.sync_chambered; }
    }
    G.prev_state = ro.state;

    WInput in;
    memset(&in, 0, sizeof in);
    head_pose(&in.head);
    in.hand[0] = wpose_of(&gp[w]);
    if (gp[o].valid) in.hand[1] = wpose_of(&gp[o]);
    else { in.hand[1].p.x = 0; in.hand[1].p.y = -10; in.hand[1].p.z = 0; in.hand[1].q.w = 1; }   /* sin mano libre: lejos de todo punto de interacción */
    in.trigger[0] = hs[w].trigger;
    in.grip[0] = hs[w].grip;
    if (gp[o].valid) { in.trigger[1] = hs[o].trigger; in.grip[1] = hs[o].grip; }
    in.buttons[0] = (hs[w].low ? WB_A : 0u) | (hs[w].up ? WB_B : 0u) | (hs[w].stick ? WB_STICK : 0u);
    if (gp[o].valid) in.buttons[1] = (hs[o].low ? WB_A : 0u) | (hs[o].up ? WB_B : 0u) | (hs[o].stick ? WB_STICK : 0u);
    in.dt = dt;
    /* el gatillo solo actúa cuando el disparo puede salir (recarga del juego en curso, cargador del juego vacío...: sin disparos sin proyectil) */
    if (!(ro.state == VRR_READY && ro.fire_allowed)) in.trigger[0] = 0;
    in.support_held = gp[o].valid && in.grip[1] > 0.5f && wcore_hand_dist(st, &in, 1, WP_SUPPORT) < 0.07f;

    G.fire_edge = 0;
    def->update(st, &in, (WReloadMode)rmode, &G.br);
    G.boot_done = 1;      /* la primera actualización es la del arranque del arma */

    /* publicación de las mallas */
    VrHandsPose hp;
    memset(&hp, 0, sizeof hp);
    hp.valid = 1;
    memcpy(hp.pos, gp[w].p, sizeof hp.pos); memcpy(hp.rot, gp[w].q, sizeof hp.rot);
    VrHandsFrame vf;
    int nb = vrhands_build(LD.rig, st, &hp, &vf);   /* fuera del cerrojo: el hilo de render no espera a la construcción de las mallas */
    pthread_mutex_lock(&HM);
    if (nb > 0) {
        vrhands_post(&vf);
        pub_sim_hand = gp[w];
        pub_sim_valid = 1;
    } else {
        vrhands_post_clear();
        pub_sim_valid = 0;
    }
    pthread_mutex_unlock(&HM);

    /* disparo */
    VrFireGame fg;
    vrfire_read_game(ram, bs, &fg);
    VrFireMap map;
    vrfire_map_from_cam(&map, vrcam_last_record());
    VrFireIn fi;
    memset(&fi, 0, sizeof fi);
    fi.active = bs->in_control && !ro.busy && map.valid && (ro.fire_allowed || ro.state == VRR_READY || ro.state == VRR_OFF);
    {
        WPose W = vrhands_weapon_world(LD.rig, st, &hp);
        fi.muzzle = wpose_mul(W, def->points[WP_MUZZLE]);
    }
    fi.fire = G.fire_edge && ro.fire_allowed;
    fi.fire_pose = G.fire_pose;
    fi.map = &map;
    VrFireOut fo;
    vrfire_frame(&fg, &fi, &fo);

    /* mando que ve el juego: gatillo y empuñaduras ya no son del juego (lo que hacen lo hace el arma VR) */
    /* Solo con el jugador al mando: en menús, pausa, comunicador y escenas el mando pasa tal cual (el gatillo VR no se traduce ahí).
     * El agarre de la mano libre (L1) solo se retira mientras sujeta el arma con las dos manos: lejos del arma abre las ventanas del juego. */
    uint16_t out = pad;
    if (bs->in_control) {
        out = (uint16_t)(out & ~(PAD_SQUARE | PAD_R1));
        if (in.support_held) out = (uint16_t)(out & ~PAD_L1);
    }
    if (ro.busy) out = (uint16_t)(out & ~ro.pad_mask);
    out |= ro.pad_force;
    if (fo.engaged && fo.square) out |= PAD_SQUARE;
    G.ro = ro;

    pthread_mutex_lock(&HM);
    pub_status.loader = LD_READY;
    pub_status.active = 1;
    pub_status.mag_rounds = st->mag_rounds; pub_status.chambered = st->chambered; pub_status.mag_inserted = st->mag_inserted;
    pub_status.game_mag = ro.show_mag; pub_status.game_reserve = ro.show_reserve;
    pub_status.reload_state = ro.state;
    pub_status.shots = G.shots;
    pthread_mutex_unlock(&HM);
    return out;
}

void vrgame_frame_end(void) {
    MvVrForearm f[2];
    int ok[2];
    for (int i = 0; i < 2; i++) {
        VrBodyForearm b;
        ok[i] = G.body_ok && vrbody_forearm_pose(i, &b);
        if (ok[i]) { memcpy(f[i].elbow, b.elbow, sizeof b.elbow); memcpy(f[i].wrist, b.wrist, sizeof b.wrist); memcpy(f[i].dorsal, b.dorsal, sizeof b.dorsal); }
    }
    VrBodyStats bst;
    vrbody_stats(&bst);
    pthread_mutex_lock(&HM);
    for (int i = 0; i < 2; i++) { pub_fore_ok[i] = ok[i]; if (ok[i]) pub_fore[i] = f[i]; }
    pub_status.arms_driven = bst.arms_driven;
    pub_status.loader = atomic_load(&LD.state);
    pthread_mutex_unlock(&HM);
}

/* ============================================================================================================== lado de render */
/* Compensación de latencia: el arma se simula en el hilo del motor a la cadencia del VBlank y su frame trae la mano de ese instante.
 * Aquí, al pedirlo el renderer, se mueve todo el frame (las piezas del arma, no los objetos sueltos) con el movimiento rígido de la
 * mano desde entonces hasta ahora: D = H_ahora * inv(H_sim). Se ignora si el salto es incoherente (recentrado, pérdida de seguimiento). */
int mv_host_hands(VrHandsFrame *out) {
    static VrHandsMesh buf[VRH_MAX_MESHES];
    int rc;
    uint64_t now = mono_ns();
    pthread_mutex_lock(&HM);
    rc = vrhands_fetch(out);
    if (rc && pub_sim_valid && out->meshes && out->nmeshes > 0 && out->nmeshes <= VRH_MAX_MESHES) {
        int w = opt_right ? 1 : 0;
        GripPose hn = grip_locked(&hand_in[w], now), hs = pub_sim_hand;
        if (hn.valid && hs.valid) {
            float qc[4], qd[4], pd[3], rp[3];
            q_conj(hs.q, qc);
            q_mul(hn.q, qc, qd);            /* Rd = Rn * Rs^-1 */
            q_norm(qd);
            q_rot(qd, hs.p, rp);
            for (int i = 0; i < 3; i++) pd[i] = hn.p[i] - rp[i];
            float dmove = sqrtf((hn.p[0] - hs.p[0]) * (hn.p[0] - hs.p[0]) + (hn.p[1] - hs.p[1]) * (hn.p[1] - hs.p[1]) + (hn.p[2] - hs.p[2]) * (hn.p[2] - hs.p[2]));
            float ang = 2.0f * acosf(fminf(1.0f, fabsf(qd[3])));
            if (dmove < 0.35f && ang < 1.2f) {
                float D[16];
                m16_from_pq(pd, qd, D);
                for (int k = 0; k < out->nmeshes; k++) {
                    buf[k] = out->meshes[k];
                    if (buf[k].flags & VRH_MF_HELD) continue;
                    const float *M = out->meshes[k].model;
                    for (int c = 0; c < 4; c++)
                        for (int r = 0; r < 4; r++)
                            buf[k].model[4 * c + r] = D[r] * M[4 * c + 0] + D[4 + r] * M[4 * c + 1] + D[8 + r] * M[4 * c + 2] + D[12 + r] * M[4 * c + 3];
                }
                out->meshes = buf;
            }
        }
    }
    pthread_mutex_unlock(&HM);
    return rc;
}
