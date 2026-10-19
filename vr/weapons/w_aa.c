/* Lanzamisiles antiaéreo al hombro ("aa"): tubo de un solo uso y recarga por sustitución del tubo. Lógica pura (sin plataforma).
 *
 * DECISIONES DE JUEGO (documentadas):
 *  - El fijado de blanco lo hace el JUEGO (vr/gameplay/vr_aa.h): aquí no hay buscador, cono, tiempo de fijación, tono ni guiado.
 *    El arma refleja el estado del buscador del juego (aa_feed_game_lock) en aa_lock_state/aa_lock_progress y en el háptico.
 *  - Disparar SIN bloqueo está permitido: el misil sale sin guiado (recto, el juego decide su trayectoria). Con bloqueo, el misil
 *    del juego persigue el blanco que dejó su buscador (no hay nada que guiar desde aquí).
 *  - La munición se descuenta (ammo_take) al ACOPLAR cada tubo, no al sacarlo de la espalda; el primer tubo se carga en la
 *    primera actualización. Sin reserva no hay tubo nuevo.
 *  - update() calcula la pose y avanza el resorte de retroceso (wcore_recoil_step). Sin vista auxiliar: la mira y el marco de
 *    fijación son los del HUD del juego.
 *  - Sin mallas compiladas: los modelos se enlazan en ejecución con weapon_bind() por nombre de pieza (aa_mesh_names).
 *    Equipar con el gatillo apretado no dispara: hay que soltarlo una vez. */
#include <math.h>
#include <string.h>
#include "w_aa.h"
#include "wcore.h"

/* ------------------------------------------------------------------ utilidades */
static WVec3 v3(float x, float y, float z) { WVec3 v = { x, y, z }; return v; }
static WVec3 vsub(WVec3 a, WVec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static WVec3 vadd(WVec3 a, WVec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static WVec3 vmul(WVec3 a, float k) { return v3(a.x * k, a.y * k, a.z * k); }
static float vdot(WVec3 a, WVec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static WVec3 vcross(WVec3 a, WVec3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static float vlen(WVec3 a) { return sqrtf(vdot(a, a)); }
static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

static WVec3 qrot(WQuat q, WVec3 v) {
    WVec3 u = v3(q.x, q.y, q.z);
    WVec3 t = vmul(vcross(u, v), 2.0f);
    return vadd(vadd(v, vmul(t, q.w)), vcross(u, t));
}

/* ------------------------------------------------------------------ definición */
#define POSE(x, y, z) { { x, y, z }, { 0, 0, 0, 1 } }

/* Nombre lógico de cada hueco de mesh; las piezas con el mismo nombre las usan (weapon_part_mesh) */
static const char *const aa_mesh_names[] = { "body", "tube", "trigger", "battery", "antenna", "sight_rear", "bezel", "screen", NULL };

/* Índices de piezas */
enum { PT_BODY, PT_TSLIDE, PT_TUBE, PT_TRIGGER, PT_BATT, PT_ANT, PT_SIGHT, PT_BEZEL, PT_SCREEN, PT_N };

static const WPart aa_parts[PT_N] = {
    /* name        parent rest                 mesh                  channel      axis                    rot */
    { "body",      -1,    POSE(0, 0, 0),       -1,   WCH_NONE,    { 0, 0, 0 },            0 },
    /* el tubo desliza hacia atrás 0,30 m (canal MAG) y cae (canal CUSTOM0, m hacia abajo) */
    { "tube_slide", 0,    POSE(0, 0.11f, -0.15f), -1,                WCH_MAG,     { 0, 0, AA_SLIDE_TRAVEL }, 0 },
    { "tube",      PT_TSLIDE, POSE(0, 0, 0),   -1,   WCH_CUSTOM0, { 0, -1, 0 },           0 },
    { "trigger",   0,     POSE(0, 0.035f, -0.045f), -1,   WCH_TRIGGER, { -0.35f, 0, 0 },       1 },
    { "battery",   0,     POSE(0, 0, 0),       -1,   WCH_NONE,    { 0, 0, 0 },            0 },
    /* antena IFF: plegada hacia delante; el canal LID la levanta 90 grados */
    { "antenna",   0,     POSE(-0.075f, 0.115f, -0.25f), -1,   WCH_LID, { 1.5708f, 0, 0 },    1 },
    { "sight_rear", 0,    POSE(0, 0.181f, 0.20f), -1,   WCH_SAFETY, { -1.5708f, 0, 0 },    1 },
    { "bezel",     0,     POSE(0, 0, 0),       -1,   WCH_NONE,    { 0, 0, 0 },            0 },
    { "screen",    0,     POSE(0, 0, 0),       -1,   WCH_SCREEN,  { 0, 0, 0 },            0 },
};

static void aa_init(WeaponState *s);
static void aa_update(WeaponState *s, const WInput *in, WReloadMode rm, const WGameBridge *g);

const WeaponDef w_aa = {
    .id = "aa", .game_weapon_id = 4,   /* antiaéreo, tabla del puente */
    .meshes = NULL, .nmeshes = 0, .mesh_names = aa_mesh_names,
    .parts = aa_parts, .nparts = PT_N,
    .points = {
        [WP_GRIP]    = POSE(0, -0.03f, 0.02f),
        [WP_SUPPORT] = POSE(0, 0.045f, -0.43f),     /* unidad de batería, bajo el tubo */
        [WP_MUZZLE]  = POSE(0, 0.11f, -0.98f),
        [WP_EYE]     = POSE(0, 0.165f, -0.64f),     /* origen de la cámara de la pantalla (sobre el tubo, ante la mira) */
        [WP_MAGWELL] = POSE(0, 0.11f, 0.15f),       /* centro del tubo nuevo al empezar el encaje */
        [WP_CHARGE]  = POSE(0, 0.05f, 0.05f),
        [WP_SCREEN]  = POSE(-0.115f, 0.135f, 0.150f),
    },
    .recoil = { .kick_back = 0.9f, .kick_up = 0.5f, .kick_yaw_rand = 0.15f, .stiffness = 55.0f,   /* contragolpe suave: */
                .damping = 12.6f, .two_hand_factor = 0.55f },                                       /* el tubo es pesado */
    .mode = WF_SINGLE_SHOT, .rpm = 0, .mag_capacity = 1,
    .state_size = sizeof(AAState),
    .init = aa_init, .update = aa_update,
};

/* ------------------------------------------------------------------ estado */
#define HIDE_DROP 2.5f   /* m de caída a partir de los cuales el tubo se oculta */

static void aa_init(WeaponState *s) {
    AAState *a = (AAState *)s;
    wcore_state_reset(s, &w_aa);             /* estado entero a cero, binding conservado */
    s->phase = AA_P_BOOT;
    a->seed = 0x2545F491u;
    a->trig_armed = 1;                       /* "ya apretado": con el gatillo pulsado al equipar no dispara hasta soltarlo */
}

int aa_lock_state(const WeaponState *s) { return ((const AAState *)s)->lk; }
float aa_lock_progress(const WeaponState *s) {
    const AAState *a = (const AAState *)s;
    if (a->lk == AA_LK_LOCKED) return 1.0f;
    return a->lk == AA_LK_ACQ ? clampf((float)a->gl.frames / (float)AA_BOX_FRAMES, 0, 1) : 0.0f;
}

WVec3 aa_back_zone(const WInput *in, float *radius) {
    if (radius) *radius = 0.25f;
    return vadd(in->head.p, qrot(in->head.q, v3(0.18f, -0.12f, 0.30f)));
}

/* Centro del tubo nuevo según la mano de apoyo: la mano lo agarra 0,30 m detrás del centro. */
static WPose spare_from_hand(const WInput *in) {
    WPose p;
    p.q = in->hand[1].q;
    p.p = vadd(in->hand[1].p, qrot(in->hand[1].q, v3(0, 0, -0.30f)));
    return p;
}

int aa_spare_pose(const WeaponState *s, const WInput *in, WPose *out) {
    const AAState *a = (const AAState *)s;
    if (!a->carry_valid) return 0;
    if (out) *out = spare_from_hand(in);
    return 1;
}

static void hap(const WGameBridge *g, int hand, float st, float sec) { if (g && g->haptic) g->haptic(hand, st, sec); }
static void snd(const WGameBridge *g, int ev) { if (g && g->sound) g->sound(w_aa.game_weapon_id, ev); }
static int reserve(const WGameBridge *g) { return (g && g->ammo_reserve) ? g->ammo_reserve(w_aa.game_weapon_id) : 0; }

/* Acopla el tubo nuevo (descuenta del contador del juego). Devuelve 0 si ya no hay reserva. */
static int seat_tube(AAState *a, const WGameBridge *g) {
    WeaponState *s = &a->base;
    if (reserve(g) <= 0) return 0;
    if (g->ammo_take) g->ammo_take(w_aa.game_weapon_id, 1);
    s->mag_rounds = 1; s->chambered = 1; s->mag_inserted = 1;
    s->channel[WCH_MAG] = 0; s->channel[WCH_CUSTOM0] = 0;
    s->phase = AA_P_READY; s->t = 0;
    a->carry_valid = 0;
    snd(g, WS_MAG_IN);
    hap(g, 0, 0.5f, 0.08f); hap(g, 1, 0.6f, 0.08f);
    return 1;
}

/* ------------------------------------------------------------------ visor (reflejo del buscador del juego) */
WPose aa_sight_pose(const WeaponState *s, const WInput *in) { return wcore_point_world(s, in, WP_EYE); }

void aa_feed_game_lock(WeaponState *s, const AAGameLock *g) {
    AAState *a = (AAState *)s;
    if (g) a->gl = *g; else memset(&a->gl, 0, sizeof a->gl);
}

/* Estado visible y háptico a partir del estado del buscador del juego. */
static void sight_update(AAState *a, const WGameBridge *g) {
    WeaponState *s = &a->base;
    int prev = a->lk;
    if (s->phase != AA_P_READY) a->lk = AA_LK_OFF;
    else if (!a->gl.sight) a->lk = AA_LK_STBY;
    else if (!a->gl.target) a->lk = AA_LK_SEARCH;
    else a->lk = a->gl.frames >= AA_BOX_FRAMES ? AA_LK_LOCKED : AA_LK_ACQ;
    if (a->lk == prev) return;
    if (a->lk == AA_LK_ACQ && prev != AA_LK_LOCKED) hap(g, 0, 0.25f, 0.04f);          /* el juego empieza a seguir un blanco */
    if (a->lk == AA_LK_LOCKED) { hap(g, 0, 0.8f, 0.25f); hap(g, 1, 0.6f, 0.25f); }    /* marco cerrado */
}

/* ------------------------------------------------------------------ recarga */
static void start_eject(AAState *a, const WGameBridge *g) {
    WeaponState *s = &a->base;
    s->phase = AA_P_EJECT; s->t = 0; a->drop_v = 0;
    s->mag_rounds = 0; s->chambered = 0;
    snd(g, WS_MAG_OUT); hap(g, 0, 0.5f, 0.08f);
}

static void reload_update(AAState *a, const WInput *in, WPose W, WReloadMode rm, const WGameBridge *g, float dt) {
    WeaponState *s = &a->base;
    const int B_REL = (in->buttons[0] & WB_MAG_RELEASE) != 0;
    s->t += dt;
    switch (s->phase) {
    case AA_P_BOOT:
        if (reserve(g) > 0) seat_tube(a, g); else { s->phase = AA_P_AWAIT; s->channel[WCH_CUSTOM0] = HIDE_DROP; }
        break;
    case AA_P_SPENT:
        if (rm == WR_AUTOMATIC ? s->t > 0.9f : B_REL) start_eject(a, g);
        break;
    case AA_P_EJECT: {
        float k = clampf(s->t / 0.25f, 0, 1);
        s->channel[WCH_MAG] = k;                       /* desliza hacia atrás */
        if (s->t > 0.25f) {                            /* y cae */
            a->drop_v += 9.8f * dt;
            s->channel[WCH_CUSTOM0] += a->drop_v * dt;
            if (s->channel[WCH_CUSTOM0] >= HIDE_DROP) {
                s->channel[WCH_CUSTOM0] = HIDE_DROP; s->mag_inserted = 0;
                s->phase = AA_P_AWAIT; s->t = 0;
            }
        }
        break; }
    case AA_P_AWAIT:
        if (rm == WR_MANUAL) {
            float rad; WVec3 z = aa_back_zone(in, &rad);
            int hold = in->grip[1] > 0.7f;
            if (hold && vlen(vsub(in->hand[1].p, z)) < rad && !a->dry_lat) {
                if (reserve(g) > 0) {                  /* coge el tubo de la espalda */
                    s->phase = AA_P_CARRY; s->t = 0; a->carry_valid = 1;
                    hap(g, 1, 0.4f, 0.06f);
                } else { snd(g, WS_DRY); hap(g, 1, 0.3f, 0.1f); a->dry_lat = 1; }
            }
            if (!hold) a->dry_lat = 0;              /* rearme del aviso de "sin tubos" */
        } else if (s->t > 0.4f && reserve(g) > 0) {
            s->phase = AA_P_AUTO_LOAD; s->t = 0;
            s->channel[WCH_MAG] = 1; s->channel[WCH_CUSTOM0] = 0; s->mag_inserted = 1;
        }
        break;
    case AA_P_CARRY: {
        if (in->grip[1] < 0.3f) {                      /* soltar: vuelve a la espalda */
            a->carry_valid = 0; s->phase = AA_P_AWAIT; s->t = 0; break;
        }
        WPose sp = spare_from_hand(in);
        WPose mw = wpose_mul(W, w_aa.points[WP_MAGWELL]);
        WVec3 f1 = qrot(sp.q, v3(0, 0, -1)), f0 = qrot(mw.q, v3(0, 0, -1));
        float ang = acosf(clampf(vdot(f1, f0), -1, 1));
        if (vlen(vsub(sp.p, mw.p)) < 0.10f && ang < 0.45f) {   /* alineado: empieza el encaje */
            s->phase = AA_P_INSERT; s->t = 0; a->carry_valid = 0; a->ins_hand0 = in->hand[1].p;
            s->channel[WCH_MAG] = 1; s->channel[WCH_CUSTOM0] = 0; s->mag_inserted = 1;
            hap(g, 1, 0.35f, 0.05f);
        }
        break; }
    case AA_P_INSERT: {
        WVec3 fwd = qrot(wpose_mul(W, w_aa.points[WP_MAGWELL]).q, v3(0, 0, -1));
        WVec3 d = vsub(in->hand[1].p, a->ins_hand0);
        float trav = vdot(d, fwd);
        float lat = vlen(vsub(d, vmul(fwd, trav)));
        if (in->grip[1] < 0.3f || trav < -0.08f || lat > 0.12f) {   /* se zafa: vuelve a la mano o se suelta */
            s->channel[WCH_CUSTOM0] = HIDE_DROP; s->mag_inserted = 0;
            if (in->grip[1] >= 0.3f) { s->phase = AA_P_CARRY; a->carry_valid = 1; }
            else { s->phase = AA_P_AWAIT; }
            s->t = 0; break;
        }
        s->channel[WCH_MAG] = 1.0f - clampf(trav / AA_SLIDE_TRAVEL, 0, 1);
        if (trav >= AA_SLIDE_TRAVEL * 0.97f) {
            if (!seat_tube(a, g)) { s->channel[WCH_CUSTOM0] = HIDE_DROP; s->mag_inserted = 0; s->phase = AA_P_AWAIT; }
        }
        break; }
    case AA_P_AUTO_LOAD: {
        float k = clampf(s->t / 0.6f, 0, 1);
        s->channel[WCH_MAG] = 1.0f - k * k * (3 - 2 * k);
        if (k >= 1.0f && !seat_tube(a, g)) { s->channel[WCH_CUSTOM0] = HIDE_DROP; s->mag_inserted = 0; s->phase = AA_P_AWAIT; s->t = 0; }
        break; }
    default: break;
    }
}

/* ------------------------------------------------------------------ actualización */
static void aa_update(WeaponState *s, const WInput *in, WReloadMode rm, const WGameBridge *g) {
    AAState *a = (AAState *)s;
    float dt = wcore_dt(in->dt, 0.05f);
    if (dt <= 0) return;                       /* dt NaN, 0 o negativo: cuadro inválido */
    WPose W = wcore_weapon_world(s, in);

    s->channel[WCH_TRIGGER] = in->trigger[0];
    reload_update(a, in, W, rm, g, dt);

    /* disparo: flanco del gatillo con tubo cargado */
    int armed_phase = (s->phase == AA_P_READY);
    if (in->trigger[0] > 0.85f) {
        if (!a->trig_armed) {
            a->trig_armed = 1;
            if (armed_phase && s->mag_rounds > 0) {   /* también durante el enfriado: sale sin bloqueo */
                WPose mz = wpose_mul(W, w_aa.points[WP_MUZZLE]);
                if (g && g->fire) g->fire(w_aa.game_weapon_id, mz);
                snd(g, WS_FIRE); hap(g, 0, 1.0f, 0.15f); hap(g, 1, 0.7f, 0.15f);
                a->seed = a->seed * 1664525u + 1013904223u;
                wcore_kick(s, in, a->seed);
                s->mag_rounds = 0; s->chambered = 0; s->phase = AA_P_SPENT; s->t = 0;
                a->lk = AA_LK_OFF;
            } else {
                snd(g, WS_DRY); hap(g, 0, 0.2f, 0.04f);   /* sin tubo útil */
            }
        }
    } else if (in->trigger[0] < 0.5f) {
        a->trig_armed = 0;
    }

    sight_update(a, g);

    /* canales visuales */
    float lid_target = (s->phase == AA_P_READY || s->phase == AA_P_SPENT) ? (s->phase == AA_P_READY ? 1.0f : 0.0f) : 0.0f;
    s->channel[WCH_LID] += clampf(lid_target - s->channel[WCH_LID], -dt * 3.0f, dt * 3.0f);
    s->channel[WCH_SAFETY] = 0;
    s->channel[WCH_SCREEN] = 1;

    /* sin vista auxiliar: la mira y el marco de fijación los dibuja el juego en su HUD */
    s->aux.active = 0;

    wcore_recoil_step(s, in, dt);
}
