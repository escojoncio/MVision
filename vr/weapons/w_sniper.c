/* Rifle de francotirador semiautomático con mira telescópica de render-to-texture.
 * La mecánica de disparo y recarga está en w_gunc.c.
 *
 * Parámetros físicos:
 *  - 300 rpm máx. (semiautomático: un disparo por pulsación, intervalo mínimo 200 ms), 20 balas;
 *  - retroceso: resorte k=190, ζ≈0,9 (c=24.8); por disparo 0,55 m/s hacia atrás y 1,1 rad/s de
 *    cabeceo (~4 grados de pico a una mano); con la mano de apoyo ×0,5;
 *  - mira: aumento del original (distancia de pantalla 320 -> hasta 3000, ver w_sniper.h); balanceo de respiración 0,0055 rad en cabeceo
 *    y 0,0040 en guiñada (0,21 y 0,16 Hz) + temblor filtrado de 0,0012 rad; segunda mano ×0,55;
 *    tranquilizante: hasta -85 % de respiración y -95 % de temblor. */
#include <math.h>
#include <string.h>
#include "w_sniper.h"

#define POSE(x, y, z) { { x, y, z }, { 0, 0, 0, 1 } }

static const WPart sniper_parts[SNIPER_PT_N] = {
    { "body",     -1, POSE(0, 0, 0), -1, WCH_NONE, { 0, 0, 0 }, 0 },
    { "stock",    0,  POSE(0, 0, 0), -1, WCH_CUSTOM1, { 0, 0, 0.04500f }, 0 },
    { "trigger",  0,  POSE(0.00000f, 0.04500f, -0.03000f), -1,
      WCH_TRIGGER, { -0.40f, 0, 0 }, 1 },
    { "selector", 0,  POSE(-0.02150f, 0.06200f, 0.02000f), -1,
      WCH_SAFETY, { 0.9f, 0, 0 }, 1 },
    { "mag",      0,  POSE(0.00000f, -0.03500f, -0.11500f), -1,
      WCH_MAG, { 0, -0.16000f, 0 }, 0 },
    { "charge",   0,  POSE(-0.00000f, 0.09200f, -0.03000f), -1,
      WCH_CUSTOM0, { 0, 0, 0.06000f }, 0 },
    { "bolt",     0,  POSE(0.02150f, 0.09000f, -0.04000f), -1,
      WCH_BOLT, { 0, 0, 0.05000f }, 0 },
    { "lens",     0,  POSE(0.00000f, 0.14750f, 0.10020f), -1,
      WCH_SCREEN, { 0, 0, 0 }, 0 },
};

static const GunCfg sniper_cfg = {
    .interval = 60.0f / SNIPER_RPM, .bolt_cycle = 0.09f,
    .charge_travel = 0.06000f, .charge_grab_r = 0.05f,
    .magwell_r = 0.12f, .mag_align_cos = 0.82f,
    .burst_kick = 0.0f,
    .mag_len = 0.19000f, .mag_in_hand = 0.03f,
    .mag_rest = { 0.00000f, -0.03500f, -0.11500f },
    .eject_pos = { 0.022f, 0.092f, -0.04f }, .eject_vel = { 2.5f, 1.8f, 0.5f },
    .floor_drop = 1.65f, .can_select = 0, .has_safety = 1, .start_semi = 1,
};

typedef struct {
    GunState g;
    float steady;
    int scope_ticks;
    float zh, tick_acc, range, range_m;   /* distancia de pantalla H del original (320..3000), resto de tick, primer impacto del eje (unidades del motor; < 0 = ninguno) y esa distancia en m */
    WPose lens_w; int lens_w_ok;
    float sw_t, tr_x, tr_y;   /* tiempo de respiración y temblor filtrado */
    float sway_yaw, sway_pitch;
    float stock, stock_goal;
    uint32_t sr;
} SniperState;

static void sniper_init(WeaponState *s) {
    SniperState *n = (SniperState *)s;
    gun_init(&n->g, &w_sniper, &sniper_cfg);   /* limpia el estado entero (incluida la extensión) */
    n->zh = SNIPER_ZOOM_BASE_H; n->range = n->range_m = -1.0f; n->stock = n->stock_goal = 0.5f; n->sr = 0xBEEF1234u;
}

void sniper_set_steady(WeaponState *s, float v) {
    ((SniperState *)s)->steady = v < 0 ? 0 : (v > 1 ? 1 : v);
}
void sniper_set_stock(WeaponState *s, float v) {
    ((SniperState *)s)->stock_goal = v < 0 ? 0 : (v > 1 ? 1 : v);
}
float sniper_zoom(const WeaponState *s) { return ((const SniperState *)s)->zh / SNIPER_ZOOM_BASE_H; }
void sniper_set_range(WeaponState *s, float hit, float units_per_m) {
    SniperState *n = (SniperState *)s;
    n->range = hit;
    n->range_m = hit >= 0.0f && units_per_m > 0.0f ? hit / units_per_m : -1.0f;
}
int sniper_lens_world(const WeaponState *s, WPose *out) {
    const SniperState *n = (const SniperState *)s;
    if (!n->lens_w_ok || !out) return 0;
    *out = n->lens_w; return 1;
}

/* rifle.c GetZoomLimit: largo 3000 (ScopePos), recortado al primer impacto; menos de 1000 = 320 (sin zoom). */
float sniper_zoom_limit(float hit) {
    float len = SNIPER_ZOOM_MAX_H;
    if (hit >= 0.0f && hit < len) len = hit;
    return len < SNIPER_ZOOM_NEAR_H ? SNIPER_ZOOM_BASE_H : len;
}
/* rifle.c Act: por debajo del límite H += H / 3 (sin pasarse); si no, GV_NearExp4 (a +-4 salta al destino, si no un cuarto de la diferencia). */
float sniper_zoom_step(float h, float limit) {
    if (h < limit) { h += h / 3.0f; return h > limit ? limit : h; }
    float diff = limit - h;
    return (diff > -4.0f && diff < 4.0f) ? limit : h + diff / 4.0f;
}
float sniper_fov_y(float h) { return 2.0f * atanf(128.0f / h); }
float sniper_cam_ahead(float range_m) {
    if (!(range_m >= 0.0f)) return SNIPER_CAM_AHEAD;
    float a = range_m - 0.05f;
    return a < 0.0f ? 0.0f : (a > SNIPER_CAM_AHEAD ? SNIPER_CAM_AHEAD : a);
}

/* Cuaternión de giro mínimo de (0,0,-1) a a (unitario). */
static WQuat arc_from_fwd(WVec3 a) {
    WQuat q = { a.y, -a.x, 0.0f, 1.0f - a.z };
    return wquat_normalize(q);
}

int sniper_lens(const WeaponState *s, SniperLens *out) {
    int np; const WPart *pt = weapon_parts(s, &np);
    int i = wcore_part_find(s, "lens");
    out->pose = sniper_parts[SNIPER_PT_LENS].rest; out->d = 0.040f; out->model = 0;
    if (i < 0 || i >= np) return 0;
    WPose w = pt[i].rest;
    for (int k = pt[i].parent, g = 0; k >= 0 && k < np && g < 8; k = pt[k].parent, g++) w = wpose_mul(pt[k].rest, w);
    out->pose = w;
    int nm; const WMesh *ms = weapon_meshes(s, &nm);
    int mi = weapon_part_mesh(s, i);
    if (mi < 0 || mi >= nm || !ms[mi].v || !ms[mi].idx || ms[mi].nv == 0) return 0;
    const WMesh *m = &ms[mi];
    WVec3 lo = wv(1e30f, 1e30f, 1e30f), hi = wv(-1e30f, -1e30f, -1e30f), ns = wv(0, 0, 0);
    for (uint32_t k = 0; k < m->nv; k++) {
        const float *p = m->v[k].pos;
        if (p[0] < lo.x) lo.x = p[0];
        if (p[1] < lo.y) lo.y = p[1];
        if (p[2] < lo.z) lo.z = p[2];
        if (p[0] > hi.x) hi.x = p[0];
        if (p[1] > hi.y) hi.y = p[1];
        if (p[2] > hi.z) hi.z = p[2];
    }
    for (uint32_t k = 0; k + 2 < m->ni; k += 3) {
        uint32_t a = m->idx[k], b = m->idx[k + 1], c = m->idx[k + 2];
        if (a >= m->nv || b >= m->nv || c >= m->nv) continue;
        WVec3 pa = wv(m->v[a].pos[0], m->v[a].pos[1], m->v[a].pos[2]), pb = wv(m->v[b].pos[0], m->v[b].pos[1], m->v[b].pos[2]), pc = wv(m->v[c].pos[0], m->v[c].pos[1], m->v[c].pos[2]);
        ns = wv_add(ns, wv_cross(wv_sub(pb, pa), wv_sub(pc, pa)));
    }
    WVec3 ctr = wv_scale(wv_add(lo, hi), 0.5f);
    if (!isfinite(ctr.x + ctr.y + ctr.z + hi.x - lo.x + hi.y - lo.y)) return 0;
    /* eje = normal de la cara (sin importar el sentido del giro de los triángulos), orientado hacia delante (-Z); si no es casi perpendicular a Z (la placa trasera de la mira), se usa -Z */
    WVec3 ax = wquat_rotate(w.q, wv_norm(ns));
    if (ax.z > 0.0f) ax = wv_scale(ax, -1.0f);
    if (!(ax.z < -0.985f)) ax = wv(0, 0, -1);
    float d = hi.x - lo.x < hi.y - lo.y ? hi.x - lo.x : hi.y - lo.y;
    out->pose.p = wpose_apply(w, ctr);
    out->pose.q = arc_from_fwd(ax);
    out->d = d < 0.03f ? 0.03f : (d > 0.06f ? 0.06f : d);
    out->model = 1;
    return 1;
}

int sniper_lens_visible(const WPose *lens_w, const WPose *head) {
    WVec3 to = wv_sub(head->p, lens_w->p);
    float r = wv_len(to);
    if (r < 1e-4f) return 1;
    to = wv_scale(to, 1.0f / r);
    return wv_dot(to, wquat_rotate(lens_w->q, wv(0, 0, 1))) > 0.0f &&
           wv_dot(to, wquat_rotate(head->q, wv(0, 0, 1))) > SNIPER_VIEW_COS;
}
void sniper_sway(const WeaponState *s, float *yaw, float *pitch) {
    const SniperState *n = (const SniperState *)s;
    if (yaw) *yaw = n->sway_yaw;
    if (pitch) *pitch = n->sway_pitch;
}

static void sniper_update(WeaponState *s, const WInput *in, WReloadMode rm, const WGameBridge *br) {
    SniperState *n = (SniperState *)s;
    float dt = wcore_dt(in->dt, 0.1f);
    if (dt <= 0) return;

    SniperLens L; sniper_lens(s, &L);

    /* ---- zoom del original: ticks de 1/25 s (2 VBlank PAL) siempre con el arma en la mano (ver w_sniper.h) ---- */
    n->tick_acc += dt * SNIPER_TICK_HZ;
    while (n->tick_acc >= 1.0f) {
        n->tick_acc -= 1.0f;
        if (n->scope_ticks >= SNIPER_ZOOM_DELAY) n->zh = sniper_zoom_step(n->zh, sniper_zoom_limit(n->range));
        if (n->scope_ticks < 1000) n->scope_ticks++;
    }

    /* ---- balanceo ---- */
    float sup = in->support_held ? 0.55f : 1.0f;
    float amp_b = sup * (1.0f - 0.85f * n->steady);
    float amp_t = sup * (1.0f - 0.95f * n->steady);
    n->sw_t += dt;
    const float tp = 6.2831853f;
    /* Amplitudes y frecuencias medidas en el original (animación de apuntar tumbado de la cámara, 4096 = vuelta): guiñada 13 unidades pico a
     * pico (~1,1 grados) con picos espectrales en 0,39 y 0,58 Hz; cabeceo 8 unidades (~0,7 grados) en 0,19 y 0,39 Hz. */
    float pb = amp_b * SNIPER_SWAY_PITCH * (sinf(tp * 0.19f * n->sw_t) + 0.35f * sinf(tp * 0.39f * n->sw_t + 1.3f));
    float yb = amp_b * SNIPER_SWAY_YAW * (sinf(tp * 0.39f * n->sw_t + 0.7f) + 0.30f * sinf(tp * 0.58f * n->sw_t));
    float k = dt * 28.0f; if (k > 1) k = 1;
    n->tr_x += (wcore_rand(&n->sr) - n->tr_x) * k;
    n->tr_y += (wcore_rand(&n->sr) - n->tr_y) * k;
    n->sway_pitch = pb + amp_t * 0.0012f * n->tr_y;
    n->sway_yaw = yb + amp_t * 0.0012f * n->tr_x;
    WQuat sq = wquat_from_rotvec(wv(n->sway_pitch, n->sway_yaw, 0));

    /* ---- culata ajustable ---- */
    n->stock = wcore_approach(n->stock, n->stock_goal, dt * 0.8f);
    s->channel[WCH_CUSTOM1] = n->stock;

    /* ---- mecánica común (la bala sale siempre del eje óptico, en el plano de la boca, y a lo largo de él) ---- */
    {
        WVec3 axis = wquat_rotate(L.pose.q, wv(0, 0, -1));
        float t = wv_dot(wv_sub(s->def->points[WP_MUZZLE].p, L.pose.p), axis);
        n->g.sight_on = 1;
        n->g.sight.p = wv_add(L.pose.p, wv_scale(axis, t));
        n->g.sight.q = L.pose.q;
    }
    gun_update(&n->g, &sniper_cfg, in, rm, br, &sq);

    /* ---- vista auxiliar: desde el eje de la lente, adelantada ---- */
    WPose lw = wpose_mul(wcore_weapon_world(s, in), L.pose);
    n->lens_w = lw; n->lens_w_ok = 1;
    WPose cp = lw;
    cp.p = wv_add(lw.p, wquat_rotate(lw.q, wv(0, 0, -sniper_cam_ahead(n->range_m))));
    cp.q = wquat_normalize(wquat_mul(cp.q, sq));
    s->aux.active = 1;
    s->aux.camera = cp;
    /* la textura cuadrada cubre el lienzo del juego en vertical (256 px): campo = 2 atan(128 / H) (vr_scope.h) */
    s->aux.fov_y = sniper_fov_y(n->zh);
    s->aux.target_part = SNIPER_PT_LENS;
    s->aux.width = s->aux.height = SNIPER_VIEW_SIZE;
    s->aux.use_game_camera = 0;
    s->aux.overlay = WO_RETICLE;
    s->channel[WCH_SCREEN] = 1.0f;
}

static const char *const sniper_mesh_names[] = { "body", "stock", "trigger", "selector", "mag", "charge", "bolt", "lens", "casing", NULL };

const WeaponDef w_sniper = {
    .id = "sniper", .game_weapon_id = 9,   /* francotirador, tabla del puente */
    .mesh_names = sniper_mesh_names,
    .meshes = 0, .nmeshes = 0,   /* sin mallas propias: el cargador de modelos las asocia por nombre de pieza */
    .parts = sniper_parts, .nparts = SNIPER_PT_N,
    .points = {
        [WP_GRIP]    = POSE(0, 0, 0),
        [WP_SUPPORT] = POSE(0.00000f, 0.04500f, -0.33000f),
        [WP_MUZZLE]  = POSE(0.00000f, 0.08000f, -0.84000f),
        [WP_EYE]     = POSE(0, 0, 0),    /* sin uso: la mira es la pieza "lens" (sniper_lens) */
        [WP_MAGWELL] = POSE(0.00000f, -0.02000f, -0.11500f),
        [WP_CHARGE]  = POSE(-0.03300f, 0.09200f, -0.03000f),
        [WP_SCREEN]  = POSE(0, 0, 0),    /* sin uso */
    },
    .recoil = { .kick_back = 0.55f, .kick_up = 1.1f, .kick_yaw_rand = 0.20f,
                .stiffness = 190.0f, .damping = 24.8f, .two_hand_factor = 0.5f },
    .mode = WF_SEMI, .rpm = SNIPER_RPM, .mag_capacity = SNIPER_MAG,
    .state_size = sizeof(SniperState),
    .init = sniper_init, .update = sniper_update,
    .make_ready = gun_make_ready,
};

int sniper_loose_mag(const WeaponState *s, WPose *out) { return gun_loose_mag(&((const SniperState *)s)->g, out); }
int sniper_held_mag(const WeaponState *s, const WInput *in, WPose *out) {
    return gun_held_mag(&((const SniperState *)s)->g, &sniper_cfg, in, out);
}
int sniper_casings(const WeaponState *s, WPose *out, int max) { return gun_casings(&((const SniperState *)s)->g, out, max); }
