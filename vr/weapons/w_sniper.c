/* Rifle de francotirador semiautomático con mira telescópica de render-to-texture.
 * La mecánica de disparo y recarga está en w_gunc.c.
 *
 * Parámetros físicos:
 *  - 300 rpm máx. (semiautomático: un disparo por pulsación, intervalo mínimo 200 ms), 20 balas;
 *  - retroceso: muelle k=190, ζ≈0,9 (c=24.8); por disparo 0,55 m/s hacia atrás y 1,1 rad/s de
 *    cabeceo (~4 grados de pico a una mano); con la mano de apoyo ×0,5;
 *  - mira: 4x / 6x / 10x sobre 60 grados de vista base; balanceo de respiración 0,0055 rad en cabeceo
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

/* El original tiene un único aumento: distancia de pantalla 3000 frente a 320 (9,375x). Los otros dos niveles (A) son una comodidad VR. */
static const float zoom_levels[3] = { SNIPER_ZOOM_GAME, SNIPER_ZOOM_GAME * 0.5f, SNIPER_ZOOM_GAME * 1.5f };

typedef struct {
    GunState g;
    float steady;
    float blend;              /* iris 0..1 */
    int scoped, zoom_i;
    float sw_t, tr_x, tr_y;   /* tiempo de respiración y temblor filtrado */
    float sway_yaw, sway_pitch;
    float stock, stock_goal;
    uint32_t sr;
} SniperState;

static void sniper_init(WeaponState *s) {
    SniperState *n = (SniperState *)s;
    gun_init(&n->g, &w_sniper, &sniper_cfg);   /* limpia el estado entero (incluida la extensión) */
    n->zoom_i = 0; n->stock = n->stock_goal = 0.5f; n->sr = 0xBEEF1234u;
}

void sniper_set_steady(WeaponState *s, float v) {
    ((SniperState *)s)->steady = v < 0 ? 0 : (v > 1 ? 1 : v);
}
void sniper_set_stock(WeaponState *s, float v) {
    ((SniperState *)s)->stock_goal = v < 0 ? 0 : (v > 1 ? 1 : v);
}
int sniper_scoped(const WeaponState *s) { return ((const SniperState *)s)->scoped; }
float sniper_zoom(const WeaponState *s) { return zoom_levels[((const SniperState *)s)->zoom_i]; }
void sniper_sway(const WeaponState *s, float *yaw, float *pitch) {
    const SniperState *n = (const SniperState *)s;
    if (yaw) *yaw = n->sway_yaw;
    if (pitch) *pitch = n->sway_pitch;
}

static void sniper_update(WeaponState *s, const WInput *in, WReloadMode rm, const WGameBridge *br) {
    SniperState *n = (SniperState *)s;
    float dt = wcore_dt(in->dt, 0.1f);
    if (dt <= 0) return;
    int zoom_edge = (in->buttons[0] & WB_A) && !(n->g.btn_prev[0] & WB_A);

    /* ---- proximidad del ojo a la mira (con el arma de la actualización anterior; el retroceso
     *      de este cuadro se aplica después y la diferencia es de milímetros) ---- */
    WPose ep = wcore_point_world(s, in, WP_EYE);
    WVec3 eye = wv_add(in->head.p, wquat_rotate(in->head.q, wv(0.032f, 0, 0)));
    WVec3 d = wv_sub(eye, ep.p);
    float dist = wv_len(d);
    WVec3 back = wquat_rotate(ep.q, wv(0, 0, 1));
    int cone = dist < 1e-4f || wv_dot(wv_scale(d, 1.0f / dist), back) > 0.5f;
    if (n->scoped) { if (dist > SNIPER_SCOPE_OFF_R || !cone) n->scoped = 0; }
    else if (dist < SNIPER_SCOPE_ON_R && cone) n->scoped = 1;
    n->blend = wcore_approach(n->blend, n->scoped ? 1.0f : 0.0f, dt * 8.0f);
    if (n->scoped && zoom_edge) n->zoom_i = (n->zoom_i + 1) % 3;

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
    n->sway_pitch = (pb + amp_t * 0.0012f * n->tr_y) * n->blend;
    n->sway_yaw = (yb + amp_t * 0.0012f * n->tr_x) * n->blend;
    WQuat sq = wquat_from_rotvec(wv(n->sway_pitch, n->sway_yaw, 0));

    /* ---- culata ajustable ---- */
    n->stock = wcore_approach(n->stock, n->stock_goal, dt * 0.8f);
    s->channel[WCH_CUSTOM1] = n->stock;

    /* ---- mecánica común ---- */
    gun_update(&n->g, &sniper_cfg, in, rm, br, &sq);

    /* ---- vista auxiliar ---- */
    WPose cp = wcore_point_world(s, in, WP_SCREEN);
    cp.q = wquat_normalize(wquat_mul(cp.q, sq));
    float z = zoom_levels[n->zoom_i];
    s->aux.active = n->scoped;
    s->aux.camera = cp;
    /* la textura cuadrada cubre el lienzo del juego en vertical (256 px): campo = 2 atan(128 / (320 z)) (vr_scope.h) */
    s->aux.fov_y = 2.0f * atanf(128.0f / (320.0f * z));
    s->aux.target_part = SNIPER_PT_LENS;
    s->aux.width = s->aux.height = SNIPER_VIEW_SIZE;
    s->aux.use_game_camera = 0;
    s->aux.overlay = WO_RETICLE | SNIPER_OV_VIGNETTE;
    s->channel[WCH_SCREEN] = n->blend;
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
        [WP_EYE]     = POSE(0.00000f, 0.14750f, 0.13500f),
        [WP_MAGWELL] = POSE(0.00000f, -0.02000f, -0.11500f),
        [WP_CHARGE]  = POSE(-0.03300f, 0.09200f, -0.03000f),
        [WP_SCREEN]  = POSE(0.00000f, 0.14750f, -0.20000f),
    },
    .recoil = { .kick_back = 0.55f, .kick_up = 1.1f, .kick_yaw_rand = 0.20f,
                .stiffness = 190.0f, .damping = 24.8f, .two_hand_factor = 0.5f },
    .mode = WF_SEMI, .rpm = SNIPER_RPM, .mag_capacity = SNIPER_MAG,
    .state_size = sizeof(SniperState),
    .init = sniper_init, .update = sniper_update,
};

int sniper_loose_mag(const WeaponState *s, WPose *out) { return gun_loose_mag(&((const SniperState *)s)->g, out); }
int sniper_held_mag(const WeaponState *s, const WInput *in, WPose *out) {
    return gun_held_mag(&((const SniperState *)s)->g, &sniper_cfg, in, out);
}
int sniper_casings(const WeaponState *s, WPose *out, int max) { return gun_casings(&((const SniperState *)s)->g, out, max); }

/* ---------------------------------------------------------------- retícula y viñeta */
static float smooth01(float x) { x = x < 0 ? 0 : (x > 1 ? 1 : x); return x * x * (3 - 2 * x); }

void sniper_draw_overlay(const WeaponState *s, uint32_t *rgba, int w, int h) {
    const SniperState *n = (const SniperState *)s;
    if (!rgba || w <= 0 || h <= 0) return;
    float iris = n->blend < 0 ? 0 : (n->blend > 1 ? 1 : n->blend);
    float cx = (w - 1) * 0.5f, cy = (h - 1) * 0.5f;
    float R = (w < h ? w : h) * 0.5f;
    float edge = 0.97f * (0.35f + 0.65f * iris);          /* el iris se abre al acercar el ojo */
    /* la viñeta empieza en 0,60 pero nunca más cerca del borde que 0,05: con el iris casi cerrado
     * (edge <= 0,60) el denominador sería cero o negativo */
    float vig0 = edge - 0.05f < 0.60f ? edge - 0.05f : 0.60f;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float dx = x - cx, dy = y - cy;
            float r = sqrtf(dx * dx + dy * dy) / R;
            uint32_t px = rgba[y * w + x];
            float f = 1.0f - 0.78f * smooth01((r - vig0) / (edge - vig0));   /* viñeta */
            if (r >= edge) f = 0.0f;
            else if (r > edge - 0.035f) f *= 0.35f;                            /* borde de la lente */
            float rr = (px & 255) * f, gg = ((px >> 8) & 255) * f, bb = ((px >> 16) & 255) * f;
            if (r < edge) {
                float ax = fabsf(dx), ay = fabsf(dy);
                int ink = 0;
                float gap = 0.035f * R;
                /* cruz fina con hueco central; postes gruesos a izquierda, derecha y abajo */
                if (ay < 0.8f && ax > gap && r < edge - 0.04f) ink = 1;
                if (ax < 0.8f && ay > gap && r < edge - 0.04f && dy > -R * 0.9f) ink = 1;
                if (dy >= -0.5f && ay < 2.4f && ax > 0.34f * R && r < edge - 0.04f) ink = 1;
                if (dy > 0 && ax < 2.4f && ay > 0.34f * R && r < edge - 0.04f) ink = 1;
                if (dy < 0 && ax < 2.4f && ay > 0.34f * R && r < edge - 0.04f) ink = 0;   /* sin poste superior */
                /* puntos de telémetro en cada eje, solo en el tramo fino */
                for (int i = 1; i <= 3 && !ink; i++) {
                    float o = 0.095f * i * R;
                    if (ax < 1.8f && fabsf(ay - o) < 1.8f && o < 0.34f * R) ink = 1;
                    if (ay < 1.8f && fabsf(ax - o) < 1.8f && o < 0.34f * R) ink = 1;
                }
                if (ax < 1.6f && ay < 1.6f) ink = 1;                          /* punto central */
                if (ink) { rr = 8; gg = 8; bb = 10; }
            }
            rgba[y * w + x] = (uint32_t)rr | ((uint32_t)gg << 8) | ((uint32_t)bb << 16) | 0xFF000000u;
        }
    }
}
