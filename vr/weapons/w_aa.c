/* Lanzamisiles antiaéreo al hombro ("aa"): tubo de un solo uso, buscador infrarrojo con fijación,
 * pantalla integrada y recarga por sustitución del tubo. Lógica pura (sin plataforma).
 *
 * DECISIONES DE JUEGO (documentadas):
 *  - Disparar SIN bloqueo está permitido: el misil sale sin guiado (recto, el juego decide su
 *    trayectoria). Con bloqueo, tras el disparo se guía hacia el blanco con WGameBridge.steer
 *    mientras projectile() devuelva un misil activo.
 *  - La munición se descuenta (ammo_take) al ACOPLAR cada tubo, no al sacarlo de la espalda; el
 *    primer tubo se carga en la primera actualización. Sin reserva no hay tubo nuevo.
 *  - update() calcula la pose, avanza el muelle de retroceso (wcore_recoil_step) y rellena aux.
 *  - Sin mallas compiladas: los modelos se enlazan en ejecución con weapon_bind() por nombre de pieza
 *    (aa_mesh_names). Equipar con el gatillo apretado no dispara: hay que soltarlo una vez. */
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
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

static WVec3 qrot(WQuat q, WVec3 v) {
    WVec3 u = v3(q.x, q.y, q.z);
    WVec3 t = vmul(vcross(u, v), 2.0f);
    return vadd(vadd(v, vmul(t, q.w)), vcross(u, t));
}
static WQuat qconj(WQuat q) { WQuat r = { -q.x, -q.y, -q.z, q.w }; return r; }

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
    return a->lk == AA_LK_ACQ ? clampf(a->lock_t / AA_LOCK_TIME, 0, 1) : 0.0f;
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
    a->cool_t = 0; a->lk = AA_LK_COOL; a->lock_t = 0; a->tgt_valid = 0; a->grace = 0;
    a->carry_valid = 0;
    snd(g, WS_MAG_IN);
    hap(g, 0, 0.5f, 0.08f); hap(g, 1, 0.6f, 0.08f);
    return 1;
}

static void seeker_reset(AAState *a) { a->lock_t = 0; a->grace = 0; a->tgt_valid = 0; a->tgt_on_screen = 0; a->beep_t = 0; }

/* ------------------------------------------------------------------ buscador */
static void seeker_update(AAState *a, WPose W, const WGameBridge *g, float dt) {
    WeaponState *s = &a->base;
    WPose cam = wpose_mul(W, w_aa.points[WP_EYE]);
    WVec3 fwd = qrot(cam.q, v3(0, 0, -1));
    int prev = a->lk;

    if (s->phase != AA_P_READY) { a->lk = AA_LK_OFF; seeker_reset(a); return; }
    if (a->cool_t < AA_COOL_TIME) {
        a->cool_t += dt; a->lk = AA_LK_COOL; seeker_reset(a); return;
    }

    /* mejor candidato: el de menor ángulo al eje dentro del cono; si ya se sigue uno, se conserva
     * mientras permanezca dentro del cono ampliado (histéresis) */
    WVec3 tg[16]; int n = 0;
    if (g && g->targets) { n = g->targets(tg, 16); if (n > 16) n = 16; if (n < 0) n = 0; }
    int best = -1; float best_ang = 1e9f;
    int keep = -1; float keep_ang = 1e9f;
    for (int i = 0; i < n; i++) {
        WVec3 d = vsub(tg[i], cam.p);
        float r = vlen(d);
        if (r < AA_RANGE_MIN || r > AA_RANGE_MAX) continue;
        float ang = acosf(clampf(vdot(fwd, d) / r, -1, 1));
        if (ang < best_ang) { best_ang = ang; best = i; }
        if (a->tgt_valid) {
            float gd = vlen(vsub(tg[i], a->tgt));
            if (gd < 10.0f && ang < keep_ang) { keep_ang = ang; keep = i; }
        }
    }
    int cur = -1; float cur_ang = 0;
    if (a->tgt_valid) {
        if (keep >= 0 && keep_ang <= AA_CONE_RAD * AA_TRACK_MULT) { cur = keep; cur_ang = keep_ang; }
    } else if (best >= 0 && best_ang <= AA_CONE_RAD) { cur = best; cur_ang = best_ang; }
    (void)cur_ang;

    if (cur >= 0) {
        a->tgt = tg[cur]; a->tgt_valid = 1; a->grace = 0;
        a->tgt_range = vlen(vsub(tg[cur], cam.p));
        a->lock_t += dt;
        if (a->lock_t >= AA_LOCK_TIME) {
            if (a->lk != AA_LK_LOCKED) {
                a->lk = AA_LK_LOCKED;
                snd(g, WS_LOCKED);          /* la app mantiene el tono en bucle mientras aa_lock_state == LOCKED */
                hap(g, 0, 0.8f, 0.25f); hap(g, 1, 0.6f, 0.25f);
            }
        } else {
            a->lk = AA_LK_ACQ;
            float p = a->lock_t / AA_LOCK_TIME;
            a->beep_t -= dt;
            if (a->beep_t <= 0) {            /* tono más rápido y fuerte a medida que se acerca el bloqueo */
                snd(g, WS_LOCK_TONE);
                hap(g, 0, 0.12f + 0.35f * p, 0.04f);
                a->beep_t = lerpf(0.40f, 0.09f, p);
            }
        }
        /* posición en pantalla */
        WVec3 c = qrot(qconj(cam.q), vsub(a->tgt, cam.p));
        float th = tanf(AA_VIEW_FOV * 0.5f);
        a->ndc_x = (c.x / -c.z) / (th * ((float)AA_VIEW_W / AA_VIEW_H));
        a->ndc_y = (c.y / -c.z) / th;
        a->tgt_on_screen = c.z < 0 && fabsf(a->ndc_x) <= 1.2f && fabsf(a->ndc_y) <= 1.2f;
    } else if (a->tgt_valid) {
        a->grace += dt;
        a->tgt_on_screen = 0;
        if (a->grace > AA_GRACE_TIME) { seeker_reset(a); a->lk = AA_LK_SEARCH; }   /* perder el blanco reinicia */
    } else {
        a->lk = AA_LK_SEARCH; a->lock_t = 0; a->tgt_on_screen = 0; a->beep_t = 0;
    }
    if (prev == AA_LK_COOL && a->lk == AA_LK_SEARCH) hap(g, 0, 0.2f, 0.05f);
}

/* Guiado del misil en vuelo hacia el blanco bloqueado. */
static void guide_update(AAState *a, const WGameBridge *g, float dt) {
    if (!a->guiding) return;
    WPose p;
    if (!g || !g->projectile || !g->steer || !g->projectile(w_aa.game_weapon_id, &p)) {
        a->rel_timer += dt;              /* tolerancia: el proyectil puede aparecer un frame después */
        if (a->rel_timer > 0.3f) a->guiding = 0;
        return;
    }
    a->rel_timer = 0;
    WVec3 tg[16]; int n = g->targets ? g->targets(tg, 16) : 0; if (n > 16) n = 16;
    float bd = 40.0f; int bi = -1;
    for (int i = 0; i < n; i++) { float d = vlen(vsub(tg[i], a->guide_tgt)); if (d < bd) { bd = d; bi = i; } }
    if (bi >= 0) a->guide_tgt = tg[bi];
    WVec3 l = qrot(qconj(p.q), vsub(a->guide_tgt, p.p));
    float yaw = atan2f(l.x, -l.z), pit = atan2f(l.y, -l.z);
    g->steer(w_aa.game_weapon_id, clampf(yaw / 0.5f, -1, 1), clampf(pit / 0.5f, -1, 1));
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
                int locked = (a->lk == AA_LK_LOCKED);
                if (g && g->fire) g->fire(w_aa.game_weapon_id, mz);
                snd(g, WS_FIRE); hap(g, 0, 1.0f, 0.15f); hap(g, 1, 0.7f, 0.15f);
                a->seed = a->seed * 1664525u + 1013904223u;
                wcore_kick(s, in, a->seed);
                s->mag_rounds = 0; s->chambered = 0; s->phase = AA_P_SPENT; s->t = 0;
                if (locked) { a->guiding = 1; a->guide_tgt = a->tgt; a->rel_timer = 0; }
                a->lk = AA_LK_OFF; seeker_reset(a);
            } else {
                snd(g, WS_DRY); hap(g, 0, 0.2f, 0.04f);   /* sin tubo útil */
            }
        }
    } else if (in->trigger[0] < 0.5f) {
        a->trig_armed = 0;
    }

    seeker_update(a, W, g, dt);
    guide_update(a, g, dt);

    /* canales visuales */
    float lid_target = (s->phase == AA_P_READY || s->phase == AA_P_SPENT) ? (s->phase == AA_P_READY ? 1.0f : 0.0f) : 0.0f;
    s->channel[WCH_LID] += clampf(lid_target - s->channel[WCH_LID], -dt * 3.0f, dt * 3.0f);
    s->channel[WCH_SAFETY] = 0;
    s->channel[WCH_SCREEN] = 1;
    a->flash_t += dt;

    /* vista auxiliar: cámara en el eje del tubo, estrecha; siempre activa mientras se empuña */
    s->aux.active = 1;
    s->aux.camera = wpose_mul(W, w_aa.points[WP_EYE]);
    s->aux.fov_y = AA_VIEW_FOV;
    s->aux.target_part = PT_SCREEN;
    s->aux.width = AA_VIEW_W; s->aux.height = AA_VIEW_H;
    s->aux.use_game_camera = 0;
    s->aux.overlay = WO_RETICLE | WO_LOCK_BOX | WO_TELEMETRY;

    wcore_recoil_step(s, in, dt);
}

/* ------------------------------------------------------------------ overlay (software) */
#define PXL(r, g, b) (0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))

static void blend(uint32_t *img, int w, int h, int x, int y, int r, int g, int b, int a255) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    uint32_t o = img[y * w + x];
    int orr = o & 255, og = (o >> 8) & 255, ob = (o >> 16) & 255;
    int rr = (orr * (255 - a255) + r * a255) / 255, gg = (og * (255 - a255) + g * a255) / 255, bb = (ob * (255 - a255) + b * a255) / 255;
    img[y * w + x] = PXL(rr, gg, bb);
}
typedef struct { uint32_t *img; int w, h, r, g, b, a; } Pen;
static void dot(Pen *p, int x, int y) { blend(p->img, p->w, p->h, x, y, p->r, p->g, p->b, p->a); }
static void hline(Pen *p, int x0, int x1, int y) { for (int x = x0; x <= x1; x++) dot(p, x, y); }
static void vline(Pen *p, int x, int y0, int y1) { for (int y = y0; y <= y1; y++) dot(p, x, y); }
static void circle(Pen *p, int cx, int cy, int r) {
    int x = r, y = 0, e = 1 - r;
    while (x >= y) {
        dot(p, cx + x, cy + y); dot(p, cx - x, cy + y); dot(p, cx + x, cy - y); dot(p, cx - x, cy - y);
        dot(p, cx + y, cy + x); dot(p, cx - y, cy + x); dot(p, cx + y, cy - x); dot(p, cx - y, cy - x);
        y++; if (e < 0) e += 2 * y + 1; else { x--; e += 2 * (y - x) + 1; }
    }
}
static void brackets(Pen *p, int cx, int cy, int hw, int hh, int len) {
    int x0 = cx - hw, x1 = cx + hw, y0 = cy - hh, y1 = cy + hh;
    for (int t = 0; t < 2; t++) {
        hline(p, x0, x0 + len, y0 + t); hline(p, x1 - len, x1, y0 + t);
        hline(p, x0, x0 + len, y1 - t); hline(p, x1 - len, x1, y1 - t);
        vline(p, x0 + t, y0, y0 + len); vline(p, x1 - t, y0, y0 + len);
        vline(p, x0 + t, y1 - len, y1); vline(p, x1 - t, y1 - len, y1);
    }
}

/* Fuente 3x5 (filas de 3 bits) */
static const struct { char c; unsigned char r[5]; } FONT[] = {
    {'0',{7,5,5,5,7}},{'1',{2,6,2,2,7}},{'2',{7,1,7,4,7}},{'3',{7,1,7,1,7}},{'4',{5,5,7,1,1}},
    {'5',{7,4,7,1,7}},{'6',{7,4,7,5,7}},{'7',{7,1,1,1,1}},{'8',{7,5,7,5,7}},{'9',{7,5,7,1,7}},
    {'A',{7,5,7,5,5}},{'B',{6,5,6,5,6}},{'C',{7,4,4,4,7}},{'D',{6,5,5,5,6}},{'E',{7,4,7,4,7}},
    {'F',{7,4,6,4,4}},{'G',{7,4,5,5,7}},{'H',{5,5,7,5,5}},{'I',{7,2,2,2,7}},{'K',{5,5,6,5,5}},
    {'L',{4,4,4,4,7}},{'M',{5,7,7,5,5}},{'N',{6,5,5,5,5}},{'O',{7,5,5,5,7}},{'P',{7,5,7,4,4}},
    {'R',{6,5,6,5,5}},{'S',{7,4,7,1,7}},{'T',{7,2,2,2,2}},{'U',{5,5,5,5,7}},{'Y',{5,5,7,2,2}},
    {'-',{0,0,7,0,0}},{'.',{0,0,0,0,2}},{':',{0,2,0,2,0}},{' ',{0,0,0,0,0}},
};
static void text(Pen *p, int x, int y, int sc, const char *str) {
    for (; *str; str++, x += 4 * sc) {
        for (unsigned i = 0; i < sizeof FONT / sizeof FONT[0]; i++) if (FONT[i].c == *str)
            for (int ry = 0; ry < 5; ry++) for (int rx = 0; rx < 3; rx++)
                if (FONT[i].r[ry] & (4 >> rx))
                    for (int sy = 0; sy < sc; sy++) for (int sx = 0; sx < sc; sx++) dot(p, x + rx * sc + sx, y + ry * sc + sy);
    }
}
static void num(char *out, int v, int digits) {
    for (int i = digits - 1; i >= 0; i--) { out[i] = (char)('0' + v % 10); v /= 10; }
    out[digits] = 0;
}

void aa_draw_overlay(const WeaponState *s, uint32_t *rgba, int w, int h) {
    const AAState *a = (const AAState *)s;
    if (!rgba || w <= 0 || h <= 0) return;
    uint32_t ov = s->aux.overlay;
    Pen p = { rgba, w, h, 90, 255, 120, 235 };
    int cx = w / 2, cy = h / 2;
    float rc_f = tanf(AA_CONE_RAD) / tanf(AA_VIEW_FOV * 0.5f) * (h * 0.5f);
    int rc = (int)(rc_f + 0.5f);
    int sc = h >= 160 ? 2 : 1;
    int locked = a->lk == AA_LK_LOCKED;
    int blink = ((int)(a->flash_t * 8.0f)) & 1;

    if (ov & WO_RETICLE) {
        if (locked) { p.r = 255; p.g = 90; p.b = 60; }
        circle(&p, cx, cy, rc);
        int gap = rc / 5 + 2;
        hline(&p, cx - rc - 6, cx - gap, cy); hline(&p, cx + gap, cx + rc + 6, cy);
        vline(&p, cx, cy - rc - 6, cy - gap); vline(&p, cx, cy + gap, cy + rc + 6);
        dot(&p, cx, cy);
        for (int i = -2; i <= 2; i++) {                     /* escala de elevación */
            if (!i) continue;
            hline(&p, cx - 3, cx + 3, cy + i * rc / 3);
        }
        p.r = 90; p.g = 255; p.b = 120;
    }
    if (ov & WO_LOCK_BOX) {
        if (a->tgt_on_screen && (a->lk == AA_LK_ACQ || locked)) {
            int bx = cx + (int)(a->ndc_x * (w * 0.5f)), by = cy - (int)(a->ndc_y * (h * 0.5f));
            float pr = locked ? 1.0f : clampf(a->lock_t / AA_LOCK_TIME, 0, 1);
            int half = (int)lerpf(h * 0.22f, h * 0.07f, pr);
            if (locked) { p.r = 255; p.g = 90; p.b = 60; brackets(&p, bx, by, half, half, half / 2 + 2);
                          hline(&p, bx - 3, bx + 3, by); vline(&p, bx, by - 3, by + 3); }
            else if (blink) { p.r = 255; p.g = 220; p.b = 60; brackets(&p, bx, by, half, half, half / 2 + 1); }
            p.r = 90; p.g = 255; p.b = 120;
        }
    }
    if (ov & WO_TELEMETRY) {
        const char *st = "SRCH";
        switch (a->lk) {
        case AA_LK_OFF: st = (s->phase == AA_P_AWAIT || s->phase == AA_P_CARRY || s->phase == AA_P_INSERT || s->phase == AA_P_AUTO_LOAD) ? "NO TUBE" : (s->phase == AA_P_SPENT ? "SPENT" : "NO TUBE"); break;
        case AA_LK_COOL: st = "COOL"; break;
        case AA_LK_SEARCH: st = "SRCH"; break;
        case AA_LK_ACQ: st = "TRACK"; break;
        case AA_LK_LOCKED: st = "LOCK"; break;
        }
        if (s->phase == AA_P_EJECT || s->phase == AA_P_CARRY || s->phase == AA_P_INSERT || s->phase == AA_P_AUTO_LOAD) st = "LOAD";
        if (locked) { p.r = 255; p.g = 90; p.b = 60; } else if (a->lk == AA_LK_OFF) { p.r = 255; p.g = 200; p.b = 60; }
        text(&p, 4, 4, sc, st);
        p.r = 90; p.g = 255; p.b = 120;
        if (a->tgt_valid && (a->lk == AA_LK_ACQ || locked)) {
            char b[8]; num(b, (int)clampf(a->tgt_range, 0, 9999), 4);
            text(&p, w - 4 - 4 * sc * 6, 4, sc, "R"); text(&p, w - 4 - 4 * sc * 5, 4, sc, b);
        }
        /* barra de adquisición */
        int bw = w / 3, bx0 = cx - bw / 2, by0 = h - 8 * sc;
        hline(&p, bx0, bx0 + bw, by0); hline(&p, bx0, bx0 + bw, by0 + 3 * sc);
        vline(&p, bx0, by0, by0 + 3 * sc); vline(&p, bx0 + bw, by0, by0 + 3 * sc);
        int fill = (int)(bw * aa_lock_progress(s));
        for (int y = by0 + 1; y < by0 + 3 * sc; y++) hline(&p, bx0, bx0 + fill, y);
    }
}
