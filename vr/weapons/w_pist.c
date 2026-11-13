/* Pistola semiautomática con supresor ("pist"). Lógica de mano, corredera y cargador mostrado.
 *
 * Convenciones (ver también wcore.c):
 *  - Mano 0 = mano del arma. La mano libre (1) solo sujeta WP_SUPPORT (agarre a dos manos, copa bajo la empuñadura).
 *  - Canales usados: WCH_SLIDE (m, hacia atrás +Z), WCH_TRIGGER (0..1), WCH_HAMMER (0..1 amartillado),
 *    WCH_MAG (m a lo largo del eje del cargador hacia abajo; 50 = oculto), WCH_ROUND (1 = recámara vacía,
 *    la pieza se aparta 100 m), WCH_CUSTOM0 (1 = sin supresor, pieza apartada).
 *  - Supresor: no se monta ni se desmonta en el VR. Lo decide el juego: WInput.game_flags & WGF_SUPP (visible y con
 *    la punta como boca) o no (pieza apartada, boca del cañón). Nunca es un objeto suelto.
 *  - Objetos sueltos (WeaponState.held): 1 = vaina expulsada en cada disparo.
 *  - Munición: NO hay recarga VR (ni cargador que se tira, ni corredera que se monta a mano). Las balas del arma
 *    (mag_rounds + chambered) son un reflejo del cargador del juego (GM_Magazine): el anfitrión (vrgame.c +
 *    vr/gameplay/vr_reload) las fija con make_ready cuando el cargador del juego cambia, y el juego recarga solo
 *    como en el original. Disparar mueve una bala del cargador a la recámara (ciclo de la corredera) y deja la corredera
 *    retenida atrás con el cargador y la recámara vacíos hasta que el anfitrión la iguala al juego.
 *  - Equipar con el gatillo apretado no dispara: hay que soltarlo una vez. */
#include "wcore.h"
/* Sin meshes compilados: los modelos se enlazan en ejecución con weapon_bind() por nombre de hueco
 * (pist_mesh_names). Las cotas de abajo son el "layout" del arma (m, espacio arma), medidas en el modelo original y escaladas por W_PIST_K (weapon.h). */
#define PIST_BY (0.063f * W_PIST_K)   /* eje del cañón = centro del asiento del supresor (wcut_tables.c AXIS_Y) */
#define PIST_MAG_CY (-0.022291f * W_PIST_K)
#define PIST_MAG_CZ (0.007675f * W_PIST_K)
#define PIST_MAG_QX -0.173648f   /* inclinación del mango medida en el modelo: 20 grados (ver wcut_tables.c MAG_UY) */
#define PIST_MAG_QW 0.984808f
#define PIST_MAG_UY 0.939693f
#define PIST_MAG_UZ -0.342020f
#define PIST_WELL_Y (-0.075f * W_PIST_K)
#define PIST_WELL_Z (0.021501f * W_PIST_K)
#define PIST_SUPP_Z (-0.21964f * W_PIST_K)   /* asiento del supresor = boca del cañón (asoma 2,64 mm de la corredera: wcut_tables.c PROT_MM) */
/* boca del cañón (a ras de la cara frontal de la corredera del modelo) y punta del supresor: salen del modelo cortado (wcut) */
#define PIST_MUZZLE_Z (-0.21964f * W_PIST_K)
#define PIST_SUPP_TIP_Z (-0.37364f * W_PIST_K)
#define PIST_TRIG_Y (0.0337f * W_PIST_K)    /* bisagra del gatillo = pivote de wcut_tables.c PIST_TRIGGER */
#define PIST_TRIG_Z (-0.1243f * W_PIST_K)
#define PIST_HAM_Y (0.050f * W_PIST_K)
#define PIST_HAM_Z (0.042f * W_PIST_K)
#define PIST_CHAMBER_Z (-0.057f * W_PIST_K)
#define PIST_SLIDE_TRAVEL (0.042f * W_PIST_K)
#define PIST_EJECT_X (0.014f * W_PIST_K)
#define PIST_EJECT_Z (-0.010f * W_PIST_K)
#define PIST_CHARGE_Z (0.020f * W_PIST_K)
#define PIST_EYE_Z (0.055f * W_PIST_K)
/* Punto de la mano de apoyo (agarre a dos manos en copa): bajo el mango, donde acaba el armazón del modelo real (STAGE.DIR, modelo 21b6 cortado: mango x +-11 mm, y -29..40 mm, z -57..-17 mm
 * detrás del guardamonte; cargador generado y -36 mm) y a la altura del guardamonte delante del mango (z -75 mm). El centro de agarre de la mano de apoyo cae aquí (zócalo de apoyo de vr_grip: FIT_SOCOM_SUP en vr_grip_fit.inc). */
#define PIST_SUP_X (-0.011f * W_PIST_K)      /* sobre la geometría real (<= 10 mm): vértice del armazón bajo el mango, lado izquierdo (test_grip.c test_real_support) */
#define PIST_SUP_Y (-0.036f * W_PIST_K)
#define PIST_SUP_Z (-0.079f * W_PIST_K)
const float w_pist_slide_travel = PIST_SLIDE_TRAVEL;   /* recorrido de la corredera: lo usa wcut_tables.c (SLIDE_TRAVEL_MM) y lo comprueba test_wcut */
const float w_pist_supp_seat_z = PIST_SUPP_Z;
const float w_pist_bare_muzzle_z = PIST_MUZZLE_Z;   /* sin supresor (lo comprueba test_wcut contra la boca del cañón) */
enum { PIST_MESH_FRAME, PIST_MESH_SLIDE, PIST_MESH_BARREL, PIST_MESH_SUPP, PIST_MESH_TRIGGER,
       PIST_MESH_HAMMER, PIST_MESH_MAG, PIST_MESH_ROUND, PIST_MESH_CASE };
static const char *const pist_mesh_names[] = {"frame", "slide", "barrel", "supp", "trigger", "hammer", "mag", "round", "case", NULL};
#include <string.h>
#include <math.h>

extern const WeaponDef w_pist;
#define PIST_ID 0
enum { P_FRAME, P_BARREL, P_SLIDE, P_TRIGGER, P_HAMMER, P_MAG, P_ROUND, P_SUPP, P_COUNT };
enum { H_MAG, H_CASE, H_ROUND };        /* índices de WeaponState.held: solo se usa H_CASE (los demás quedan inactivos) */
enum { FM_NONE, FM_FALL, FM_REST };
enum { PH_NORMAL, PH_CYCLING, PH_LOCKED, PH_NOMAG };

#define TRAVEL PIST_SLIDE_TRAVEL
#define CYC_BACK 0.024f      /* s hasta el final del retroceso */
#define CYC_TOTAL 0.060f     /* ciclo completo ~60 ms */

static const WPart k_parts[P_COUNT] = {
    {"frame", -1, {{0, 0, 0}, {0, 0, 0, 1}}, PIST_MESH_FRAME, WCH_NONE, {0, 0, 0}, 0},
    {"barrel", 0, {{0, 0, 0}, {0, 0, 0, 1}}, PIST_MESH_BARREL, WCH_NONE, {0, 0, 0}, 0},
    {"slide", 0, {{0, 0, 0}, {0, 0, 0, 1}}, PIST_MESH_SLIDE, WCH_SLIDE, {0, 0, 1}, 0},
    {"trigger", 0, {{0, PIST_TRIG_Y, PIST_TRIG_Z}, {0, 0, 0, 1}}, PIST_MESH_TRIGGER, WCH_TRIGGER, {-0.35f, 0, 0}, 1},
    {"hammer", 0, {{0, PIST_HAM_Y, PIST_HAM_Z}, {0, 0, 0, 1}}, PIST_MESH_HAMMER, WCH_HAMMER, {0.75f, 0, 0}, 1},
    {"mag", 0, {{0, PIST_MAG_CY, PIST_MAG_CZ}, {PIST_MAG_QX, 0, 0, PIST_MAG_QW}}, PIST_MESH_MAG, WCH_MAG,
        {0, -PIST_MAG_UY, -PIST_MAG_UZ}, 0},
    {"round", 0, {{0, PIST_BY, PIST_CHAMBER_Z}, {0, 0, 0, 1}}, PIST_MESH_ROUND, WCH_ROUND, {0, 100, 0}, 0},
    {"supp", P_BARREL, {{0, PIST_BY, PIST_SUPP_Z}, {0, 0, 0, 1}}, PIST_MESH_SUPP, WCH_CUSTOM0, {0, 100, 0}, 0},
};

typedef struct {
    int mode;                 /* FM_* */
    WVec3 v, w;               /* FM_FALL: velocidad lineal y angular (mundo) */
    float dur;                /* vida restante en FM_REST (<0 = permanente) */
} Free;

typedef struct {
    WeaponState b;
    float slide;              /* posición real de la corredera (m) */
    int locked, cyc, peaked, fed;
    float cyc_t;
    int hammer_cocked; float hammer;
    int trig_armed;           /* el gatillo ya pasó por reposo: puede disparar */
    Free fr[W_MAX_HELD];
    uint32_t seed;
    int shots;
} PistState;

/* ---------------- puente ---------------- */
static void snd(const WGameBridge *g, int ev) { if (g && g->sound) g->sound(PIST_ID, ev); }
static void hap(const WGameBridge *g, int hand, float str, float sec) { if (g && g->haptic) g->haptic(hand, str, sec); }

static float smooth(float u) { u = wcore_clampf(u, 0, 1); return u * u * (3 - 2 * u); }

/* ---------------- objetos sueltos (vainas) ---------------- */
static void drop_free(PistState *s, int slot, WPose world, WVec3 v, WVec3 w, float life) {
    Free *f = &s->fr[slot];
    f->mode = FM_FALL; f->v = v; f->w = w; f->dur = life;
    s->b.held[slot].world = world; s->b.held[slot].active = 1;
}
static void kill_free(PistState *s, int slot) { s->fr[slot].mode = FM_NONE; s->b.held[slot].active = 0; }

static void step_free(PistState *s, int slot, float dt, float floor_y) {
    Free *f = &s->fr[slot]; WHeld *h = &s->b.held[slot];
    if (f->mode == FM_NONE) { h->active = 0; return; }
    h->active = 1;
    if (f->mode == FM_FALL) {
        f->v.y -= 9.81f * dt;
        h->world.p = wv_add(h->world.p, wv_scale(f->v, dt));
        float wl = wv_len(f->w);
        if (wl > 1e-5f) h->world.q = wquat_normalize(wquat_mul(wquat_axis_angle(f->w, wl * dt), h->world.q));
        if (h->world.p.y <= floor_y + 0.012f) {
            h->world.p.y = floor_y + 0.012f; f->mode = FM_REST; f->v = wv(0, 0, 0); f->w = wv(0, 0, 0);
        }
    } else if (f->mode == FM_REST) {
        if (f->dur >= 0) { f->dur -= dt; if (f->dur <= 0) kill_free(s, slot); }
    }
}

/* ---------------- corredera ---------------- */
static void chamber_round(PistState *s) {
    if (!s->b.chambered && s->b.mag_inserted && s->b.mag_rounds > 0) { s->b.mag_rounds--; s->b.chambered = 1; }
}

static void eject_case(PistState *s, WPose W) {
    WPose at = {{PIST_EJECT_X, 0.070f * W_PIST_K, PIST_EJECT_Z}, W.q};
    at.q = wquat_identity();
    WPose wp = wpose_mul(W, at);
    float a = wcore_rand(&s->seed), b = wcore_rand(&s->seed), c = wcore_rand(&s->seed);
    WVec3 v = wquat_rotate(W.q, wv(2.0f + 0.4f * a, 1.2f + 0.3f * b, 0.7f + 0.3f * c));
    WVec3 w = wquat_rotate(W.q, wv(6 * c, 22 + 8 * a, 9 * b));
    drop_free(s, H_CASE, wp, v, w, 2.0f);
    s->b.held[H_CASE].mesh = PIST_MESH_CASE;
}

static void start_fire_cycle(PistState *s) { s->cyc = 1; s->cyc_t = 0; s->peaked = 0; s->fed = 0; }

/* ---------------- actualización ---------------- */
int w_pist_trig_armed(const WeaponState *ws) { return ((const PistState *)ws)->trig_armed; }   /* diagnóstico (vrgame): solo para el estado de la pistola */

/* El anfitrión iguala el arma al cargador del juego (vr_reload.h set_held): cargador montado con `mag_rounds` balas, una en la recámara, corredera cerrada. */
static void pist_make_ready(WeaponState *ws, int mag_rounds) {
    PistState *s = (PistState *)ws;
    ws->mag_inserted = 1; ws->mag_rounds = mag_rounds > 0 ? mag_rounds : 0; ws->chambered = 1;
    s->locked = 0; s->cyc = 0; s->slide = 0;
    s->hammer_cocked = 1;
}

static void pist_init(WeaponState *ws) {
    wcore_state_reset(ws, &w_pist);          /* estado entero a cero, binding conservado */
    PistState *s = (PistState *)ws;
    /* sin cargador hasta que el anfitrión lo iguale al del juego (make_ready) */
    s->hammer_cocked = 1; s->hammer = 1; s->trig_armed = 0; s->seed = 0xC0FFEEu;
    ws->held[H_CASE].mesh = PIST_MESH_CASE;
}

static void pist_update(WeaponState *ws, const WInput *in, WReloadMode rm, const WGameBridge *g) {
    PistState *s = (PistState *)ws;
    (void)rm;                                  /* no hay recarga VR: el juego recarga solo y el anfitrión iguala el arma a su cargador */
    float dt = wcore_dt(in->dt, 0.1f);
    if (dt <= 0) return;                       /* dt NaN, 0 o negativo: cuadro inválido */
    wcore_recoil_step(ws, in, dt);
    WPose W = wcore_weapon_world(ws, in);
    float floor_y = in->head.p.y - 1.65f;
    float tr = in->trigger[0];
    int supp = (in->game_flags & WGF_SUPP) != 0;   /* supresor puesto según el juego (la capa de integración lo rellena) */

    /* ---- gatillo / disparo ---- */
    if (tr < 0.3f) s->trig_armed = 1;       /* se rearma al soltar por debajo de 0,3 */
    if (tr > 0.5f && s->trig_armed) {
        s->trig_armed = 0;
        if (!s->cyc && !s->locked && s->slide < 0.002f) {
            if (s->b.chambered && s->hammer_cocked) {
                WPose mz = wpose_mul(W, ws->def->points[WP_MUZZLE]);
                if (!supp) { WPose loc = ws->def->points[WP_MUZZLE]; loc.p.z = PIST_MUZZLE_Z; mz = wpose_mul(W, loc); }
                s->b.chambered = 0; s->hammer_cocked = 0; s->shots++;
                if (g && g->fire) g->fire(PIST_ID, mz);
                snd(g, WS_FIRE);
                wcore_kick(ws, in, s->seed ^ (uint32_t)(s->shots * 2654435761u));
                hap(g, 0, supp ? 0.7f : 1.0f, 0.06f);
                if (in->support_held) hap(g, 1, 0.45f, 0.05f);
                start_fire_cycle(s);
            } else {
                s->hammer_cocked = 0; snd(g, WS_DRY); hap(g, 0, 0.15f, 0.02f);
            }
        }
    }

    /* ---- ciclo de la corredera tras disparo ---- */
    if (s->cyc) {
        s->cyc_t += dt;
        if (!s->peaked && s->cyc_t >= CYC_BACK) {
            s->peaked = 1; s->hammer_cocked = 1;
            eject_case(s, W); snd(g, WS_SLIDE_BACK);
            if (!(s->b.mag_inserted && s->b.mag_rounds > 0) && s->b.mag_inserted) {   /* cargador vacío: retén */
                s->locked = 1; s->cyc = 0; s->slide = TRAVEL;
            }
        }
        if (s->cyc) {
            if (s->cyc_t < CYC_BACK) { float u = s->cyc_t / CYC_BACK; s->slide = TRAVEL * (1 - (1 - u) * (1 - u)); }
            else {
                float u = (s->cyc_t - CYC_BACK) / (CYC_TOTAL - CYC_BACK); s->slide = TRAVEL * (1 - smooth(u));
                if (!s->fed && u > 0.45f) { s->fed = 1; chamber_round(s); }
            }
            if (s->cyc_t >= CYC_TOTAL) { s->cyc = 0; s->slide = 0; snd(g, WS_SLIDE_FWD); hap(g, 0, 0.2f, 0.02f); }
        }
    }
    /* sin balas (cargador y recámara vacíos, p. ej. el juego vació el arma): corredera retenida atrás hasta que el anfitrión la iguale al juego */
    if (!s->cyc && s->b.mag_inserted && s->b.mag_rounds == 0 && !s->b.chambered) s->locked = 1;
    if (s->locked && !s->cyc) s->slide = TRAVEL;

    /* ---- física de los objetos sueltos ---- */
    for (int i = 0; i < W_MAX_HELD; i++) step_free(s, i, dt, floor_y);

    /* ---- martillo ---- */
    {
        float tgt = s->hammer_cocked ? 1.0f : 0.0f;
        float rate = s->hammer_cocked ? 40.0f : 70.0f;
        s->hammer = wcore_approach(s->hammer, tgt, rate * dt);
    }

    /* ---- canales ---- */
    ws->channel[WCH_SLIDE] = s->slide;
    ws->channel[WCH_TRIGGER] = wcore_clampf(tr, 0, 1);
    ws->channel[WCH_HAMMER] = s->hammer;
    ws->channel[WCH_MAG] = ws->mag_inserted ? 0.0f : 50.0f;
    ws->channel[WCH_ROUND] = ws->chambered ? 0.0f : 1.0f;
    ws->channel[WCH_CUSTOM0] = supp ? 0.0f : 1.0f;
    ws->phase = s->cyc ? PH_CYCLING : s->locked ? PH_LOCKED : !ws->mag_inserted ? PH_NOMAG : PH_NORMAL;
    ws->t = s->cyc ? s->cyc_t : 0;
}

const WeaponDef w_pist = {
    .id = "pist", .game_weapon_id = PIST_ID,
    .meshes = NULL, .nmeshes = 0, .mesh_names = pist_mesh_names,
    .parts = k_parts, .nparts = P_COUNT,
    .points = {
        [WP_GRIP]    = {{0, 0, 0}, {0, 0, 0, 1}},
        [WP_SUPPORT] = {{PIST_SUP_X, PIST_SUP_Y, PIST_SUP_Z}, {0, 0, 0, 1}},   /* mano libre: copa bajo el mango (palma hacia arriba, dedos hacia delante) */
        [WP_MUZZLE]  = {{0, PIST_BY, PIST_SUPP_TIP_Z}, {0, 0, 0, 1}},
        [WP_EYE]     = {{0, 0.0875f * W_PIST_K, PIST_EYE_Z}, {0, 0, 0, 1}},
        [WP_MAGWELL] = {{0, PIST_WELL_Y, PIST_WELL_Z}, {PIST_MAG_QX, 0, 0, PIST_MAG_QW}},
        [WP_CHARGE]  = {{0, PIST_BY, PIST_CHARGE_Z}, {0, 0, 0, 1}},
        [WP_SCREEN]  = {{0, 0, 0}, {0, 0, 0, 1}},
    },
    /* k=200 (w=14 rad/s), c=24 -> zeta~0,85: pico ~2,5 cm / ~7 grados, vuelve en ~0,3 s sin oscilar */
    .recoil = {.kick_back = 0.9f, .kick_up = 4.6f, .kick_yaw_rand = 0.8f, .stiffness = 200.0f, .damping = 24.0f,
               .two_hand_factor = 0.55f},
    .mode = WF_SEMI, .rpm = 360, .mag_capacity = 12,
    .state_size = sizeof(PistState), .init = pist_init, .update = pist_update,
    .make_ready = pist_make_ready,
};
