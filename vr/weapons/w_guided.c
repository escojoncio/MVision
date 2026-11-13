/* Lanzador de misiles guiados al hombro ("guided"): definición, lógica de uso y overlay del visor.
 * Marco del arma: origen en la empuñadura delantera; -Z = boca; +Z = culata; eje del tubo en y=0.10. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "w_guided.h"
#include "wcore.h"

#define AXY   0.10f          /* altura del eje del tubo */
#define MOUTH_Z 0.52f        /* boca trasera (tapa) */
#define MISSILE_LEN 0.85f
#define HAND_TO_NOSE 0.75f   /* la mano agarra cerca de la cola */
#define INS_TRAVEL 0.30f     /* recorrido de inserción guiada (m) */
#define HIDDEN_Z (-400.0f)   /* valor del canal WCH_ROUND con el tubo vacío (pieza fuera de plano lejano) */

/* ------------------------------------------------------------------ geometría / piezas */
/* GP_* = pieza lógica = índice en w_guided.parts = índice en guided_mesh_names (mismo nombre) */

#define QI {0, 0, 0, 1}
static const WPart g_parts[GP_COUNT] = {
    {"tube",    -1,        {{0, 0, 0}, QI},                        -1,    WCH_NONE,    {0, 0, 0}, 0},
    {"grip",    -1,        {{0, 0, 0}, QI},                        -1,    WCH_NONE,    {0, 0, 0}, 0},
    {"trigger", GP_GRIP,   {{0, 0.012f, -0.034f}, QI},             -1, WCH_TRIGGER, {-0.50f, 0, 0}, 1},
    {"sight",   -1,        {{0, 0.164f, 0.22f}, QI},               -1,   WCH_NONE,    {0, 0, 0}, 0},
    /* pantallita: inclinada 50 grados hacia arriba/atrás (rotación -50 sobre X) */
    {"screen",  GP_TUBE,   {{0, 0.205f, 0.02f}, {-0.42262f, 0, 0, 0.90631f}}, -1, WCH_SCREEN, {0, 0, 0}, 0},
    {"frame",   GP_TUBE,   {{0, 0.205f, 0.02f}, {-0.42262f, 0, 0, 0.90631f}}, -1, WCH_NONE, {0, 0, 0}, 0},
    /* tapa trasera: bisagra en el lado derecho, gira sobre Y */
    {"lid",     GP_TUBE,   {{0.058f, AXY, MOUTH_Z}, QI},           -1,     WCH_LID,     {0, 2.3f, 0}, 1},
    /* misil: 0 = cargado (morro asoma 0.10 m); 1 = a la altura de la boca trasera; -400 = ausente */
    {"round",   GP_TUBE,   {{0, AXY, -0.275f}, QI},                -1, WCH_ROUND,   {0, 0, 1.25f}, 0},
};
static const char *const guided_mesh_names[] = { "tube", "grip", "trigger", "sight", "screen", "frame", "lid", "round", NULL };

static void g_init(WeaponState *s);
static void g_update(WeaponState *s, const WInput *in, WReloadMode rm, const WGameBridge *g);

const WeaponDef w_guided = {
    .id = "guided", .game_weapon_id = 3,
    .meshes = NULL, .nmeshes = 0, .mesh_names = guided_mesh_names,
    .parts = g_parts, .nparts = GP_COUNT,
    .points = {
        [WP_GRIP]    = {{0, 0, 0}, QI},
        [WP_SUPPORT] = {{-0.029f, 0.045f, -0.197f}, QI},
        [WP_MUZZLE]  = {{0, AXY, -0.62f}, QI},
        [WP_EYE]     = {{-0.095f, 0.176f, 0.40f}, QI},
        [WP_MAGWELL] = {{0, AXY, MOUTH_Z}, QI},
        [WP_CHARGE]  = {{-0.045f, AXY, 0.545f}, QI},
        [WP_SCREEN]  = {{0, 0.205f, 0.02f}, QI},
    },
    .recoil = { .kick_back = 0.55f, .kick_up = 0.80f, .kick_yaw_rand = 0.12f, .stiffness = 90.0f,
                .damping = 12.0f, .two_hand_factor = 0.60f },
    .mode = WF_SINGLE_SHOT, .rpm = 0.0f, .mag_capacity = 1,
    .state_size = sizeof(GuidedState), .init = g_init, .update = g_update,
};

/* ------------------------------------------------------------------ matemática local */
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static WVec3 v3(float x, float y, float z) { WVec3 r = {x, y, z}; return r; }
static WVec3 vadd(WVec3 a, WVec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static WVec3 vsub(WVec3 a, WVec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static WVec3 vmul(WVec3 a, float k) { return v3(a.x * k, a.y * k, a.z * k); }
static float vdot(WVec3 a, WVec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static WVec3 vcross(WVec3 a, WVec3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static float vlen(WVec3 a) { return sqrtf(vdot(a, a)); }
static WVec3 vnorm(WVec3 a) { float l = vlen(a); return l > 1e-9f ? vmul(a, 1.0f / l) : v3(0, 0, -1); }
static WQuat qmul(WQuat a, WQuat b) {
    WQuat r = { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
    return r;
}
static WQuat qconj(WQuat q) { WQuat r = {-q.x, -q.y, -q.z, q.w}; return r; }
static WVec3 qrot(WQuat q, WVec3 v) {
    WVec3 u = v3(q.x, q.y, q.z);
    WVec3 t = vmul(vcross(u, v), 2.0f);
    return vadd(vadd(v, vmul(t, q.w)), vcross(u, t));
}
static WQuat qnorm(WQuat q) {
    float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (l < 1e-9f) { WQuat i = QI; return i; }
    WQuat r = {q.x / l, q.y / l, q.z / l, q.w / l}; return r;
}
static WQuat qnlerp(WQuat a, WQuat b, float t) {
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; }
    WQuat r = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
    return qnorm(r);
}
/* base ortonormal (x,y,z columnas) -> cuaternión */
static WQuat q_from_basis(WVec3 x, WVec3 y, WVec3 z) {
    float tr = x.x + y.y + z.z; WQuat q;
    if (tr > 0) { float s = sqrtf(tr + 1.0f) * 2; q.w = 0.25f * s; q.x = (y.z - z.y) / s; q.y = (z.x - x.z) / s; q.z = (x.y - y.x) / s; }
    else if (x.x > y.y && x.x > z.z) { float s = sqrtf(1.0f + x.x - y.y - z.z) * 2; q.w = (y.z - z.y) / s; q.x = 0.25f * s; q.y = (y.x + x.y) / s; q.z = (z.x + x.z) / s; }
    else if (y.y > z.z) { float s = sqrtf(1.0f + y.y - x.x - z.z) * 2; q.w = (z.x - x.z) / s; q.x = (y.x + x.y) / s; q.y = 0.25f * s; q.z = (z.y + y.z) / s; }
    else { float s = sqrtf(1.0f + z.z - x.x - y.y) * 2; q.w = (x.y - y.x) / s; q.x = (z.x + x.z) / s; q.y = (z.y + y.z) / s; q.z = 0.25f * s; }
    return qnorm(q);
}

/* ------------------------------------------------------------------ puente (seguro ante NULL) */
static int  b_reserve(const WGameBridge *g) { return (g && g->ammo_reserve) ? g->ammo_reserve(w_guided.game_weapon_id) : 0; }
static void b_take(const WGameBridge *g, int n) { if (g && g->ammo_take) g->ammo_take(w_guided.game_weapon_id, n); }
static void b_haptic(const WGameBridge *g, int h, float st, float sec) { if (g && g->haptic) g->haptic(h, st, sec); }
static void b_sound(const WGameBridge *g, int ev) { if (g && g->sound) g->sound(w_guided.game_weapon_id, ev); }

/* ------------------------------------------------------------------ estado */
static void g_init(WeaponState *s) {
    GuidedState *gs = (GuidedState *)s;
    wcore_state_reset(s, &w_guided);          /* estado entero a cero, binding conservado */
    s->mag_rounds = 1; s->chambered = 1; s->mag_inserted = 1;
    s->phase = G_IDLE; s->t = 0;
    gs->q0.w = 1;
    gs->world_scale = 1.0f;
    gs->fuel = GD_FUEL_TIME;
    gs->seed = 12345u;
    gs->prev_grip1 = 0; gs->prev_trig = 1.0f;   /* como si el gatillo ya estuviera apretado: no lanza al equipar */
    s->aux.target_part = GP_SCREEN; s->aux.width = GD_VIEW_W; s->aux.height = GD_VIEW_H; s->aux.fov_y = 1.05f;
}

void guided_set_stick(WeaponState *s, float x, float y) {
    GuidedState *gs = (GuidedState *)s; gs->stick[0] = clampf(x, -1, 1); gs->stick[1] = clampf(y, -1, 1);
}

static WVec3 pt_world(const WPose *W, WPoint p) { return wpose_apply(*W, w_guided.points[p].p); }

/* respuesta con zona muerta y curva suave: 0 dentro de la zona muerta, 1 a fondo */
static float shape(float a, float dead, float full) {
    float m = fabsf(a);
    if (m <= dead) return 0.0f;
    float u = clampf((m - dead) / (full - dead), 0, 1);
    u = u * (0.5f + 0.5f * u);
    return a < 0 ? -u : u;
}

static void finish_load(GuidedState *gs, const WGameBridge *g) {
    WeaponState *s = &gs->base;
    if (b_reserve(g) > 0) {
        b_take(g, 1);
        s->mag_rounds = 1; s->chambered = 1; s->mag_inserted = 1;
        b_sound(g, WS_MAG_IN); b_haptic(g, 1, 0.5f, 0.08f); b_haptic(g, 0, 0.3f, 0.06f);
    }
    gs->ins = 0; gs->hand_missile = 0;
}

static void start_auto(GuidedState *gs, float wait) { gs->auto_stage = GR_WAIT; gs->auto_t = -wait; }

/* zona "sobre el hombro, atrás" en el espacio de la cabeza */
static int in_back_zone(const WInput *in, WVec3 p) {
    WVec3 l = qrot(qconj(in->head.q), vsub(p, in->head.p));
    if (l.z < 0.02f) return 0;
    WVec3 c = v3(0.18f, 0.0f, 0.22f);
    return vlen(vsub(l, c)) < 0.30f;
}

static void do_launch(GuidedState *gs, const WInput *in, const WGameBridge *g, const WPose *W) {
    WeaponState *s = &gs->base;
    WPose muz = wpose_mul(*W, w_guided.points[WP_MUZZLE]);
    if (g && g->fire) g->fire(w_guided.game_weapon_id, muz);
    b_sound(g, WS_FIRE); b_haptic(g, 0, 1.0f, 0.15f); b_haptic(g, 1, 0.6f, 0.12f);
    s->mag_rounds = 0; s->chambered = 0;
    s->phase = G_FLIGHT; s->t = 0;
    gs->flight_t = 0; gs->fuel = GD_FUEL_TIME; gs->seen = 0; gs->proj_valid = 0;
    gs->q0 = in->hand[0].q; gs->launch_pos = muz.p; gs->launch_ground_y = in->head.p.y - 1.65f;
    gs->steer[0] = gs->steer[1] = gs->steer_raw[0] = gs->steer_raw[1] = 0;
    gs->dist = gs->alt = gs->speed = 0; gs->ended_by = 0;
    gs->hand_missile = 0; gs->ins = 0; gs->auto_stage = GR_NONE;
    wcore_kick(s, in, gs->seed++);
}

static void g_update(WeaponState *s, const WInput *in, WReloadMode rm, const WGameBridge *g) {
    GuidedState *gs = (GuidedState *)s;
    float dt = wcore_dt(in->dt, 0.1f);
    if (dt <= 0) return;                       /* dt NaN, 0 o negativo: cuadro inválido */
    int gid = w_guided.game_weapon_id;
    gs->frame++;
    wcore_recoil_step(s, in, dt);
    WPose W = wcore_weapon_world(s, in);

    /* --- apoyo al hombro (hombro derecho) y ojo en la mirilla --- */
    WVec3 shoulder = vadd(in->head.p, qrot(in->head.q, v3(0.17f, -0.22f, 0.05f)));
    WVec3 pad = wpose_apply(W, v3(0, 0.02f, 0.38f));
    gs->shouldered = vlen(vsub(pad, shoulder)) < GD_SHOULDER_R;
    WVec3 eye = vadd(in->head.p, qrot(in->head.q, v3(0.032f, 0, 0)));
    float de = vlen(vsub(eye, pt_world(&W, WP_EYE)));
    if (gs->eye_near) { if (de > GD_EYE_OUT) gs->eye_near = 0; } else if (de < GD_EYE_IN) gs->eye_near = 1;

    float trig = in->trigger[0], grip1 = in->grip[1];
    int trig_edge = trig > 0.5f && gs->prev_trig <= 0.5f;
    int grip_edge = grip1 >= 0.7f && gs->prev_grip1 < 0.7f;
    int btn_b_edge = (in->buttons[0] & WB_B) && !(gs->prev_btn0 & WB_B);
    s->channel[WCH_TRIGGER] = trig;
    gs->deny_t = fmaxf(0, gs->deny_t - dt);
    s->t += dt;

    if (s->phase == G_IDLE) {
        /* --- disparo --- */
        if (trig_edge) {
            if (s->mag_rounds > 0 && s->channel[WCH_LID] < 0.02f && gs->lid_target == 0 && gs->ins == 0 && gs->auto_stage == GR_NONE)
                do_launch(gs, in, g, &W);
            else { b_sound(g, WS_DRY); b_haptic(g, 0, 0.2f, 0.04f); }
        }
    }
    if (s->phase == G_IDLE) {
        /* --- recarga --- */
        int empty = s->mag_rounds == 0;
        if (rm == WR_AUTOMATIC) {
            if (empty && gs->auto_stage == GR_NONE && gs->lid_target == 0 && b_reserve(g) > 0) start_auto(gs, 0.9f);
        } else if (rm == WR_SIMPLIFIED) {
            if (empty && gs->auto_stage == GR_NONE && b_reserve(g) > 0 && grip_edge &&
                (vlen(vsub(in->hand[1].p, pt_world(&W, WP_MAGWELL))) < 0.30f || in_back_zone(in, in->hand[1].p)))
                start_auto(gs, 0.0f);
        } else if (gs->auto_stage == GR_NONE) {
            /* manual: tapa -> misil de la espalda -> inserción alineada -> cerrar tapa */
            WVec3 h1 = in->hand[1].p;
            if (grip_edge && !gs->hand_missile && vlen(vsub(h1, pt_world(&W, WP_CHARGE))) < 0.10f) {
                if (gs->lid_target == 0) {
                    if (empty) { gs->lid_target = 1; b_sound(g, WS_MAG_OUT); b_haptic(g, 1, 0.4f, 0.05f); }
                    else { gs->deny_t = 0.5f; b_haptic(g, 1, 0.3f, 0.05f); }
                } else { gs->lid_target = 0; b_sound(g, WS_BOLT); b_haptic(g, 1, 0.4f, 0.05f); }
            }
            if (gs->lid_target > 0.5f && s->channel[WCH_LID] >= 0.9f && empty) {
                if (!gs->hand_missile) {
                    if (grip_edge && in_back_zone(in, h1) && b_reserve(g) > 0) {
                        gs->hand_missile = 1; b_sound(g, WS_MAG_OUT); b_haptic(g, 1, 0.5f, 0.06f);
                    }
                } else if (grip1 < 0.4f) {
                    gs->hand_missile = 0;                 /* suelta el misil */
                } else {
                    WVec3 hl = qrot(qconj(W.q), vsub(h1, W.p));
                    WVec3 axl = qrot(qconj(W.q), qrot(in->hand[1].q, v3(0, 0, -1)));
                    WVec3 nose = vadd(hl, vmul(axl, HAND_TO_NOSE));
                    float lat = hypotf(nose.x, nose.y - AXY);
                    float tgt = 0;
                    if (axl.z < -0.88f && lat < 0.07f) tgt = clampf((MOUTH_Z - nose.z) / INS_TRAVEL, 0, 1);
                    gs->ins = tgt;
                    if (gs->ins >= 0.98f) finish_load(gs, g);
                }
            }
            if (gs->ins > 0 && !gs->hand_missile) gs->ins = fmaxf(0, gs->ins - dt * 2.0f);  /* se desliza hacia fuera */
        }
        /* etapas de recarga asistida */
        if (gs->auto_stage != GR_NONE) {
            switch (gs->auto_stage) {
            case GR_WAIT:
                gs->auto_t += dt;
                if (gs->auto_t >= 0) { gs->lid_target = 1; b_sound(g, WS_MAG_OUT); gs->auto_stage = GR_OPEN; }
                break;
            case GR_OPEN:
                if (s->channel[WCH_LID] >= 0.98f) { gs->auto_stage = GR_INSERT; gs->ins = 0; }
                break;
            case GR_INSERT:
                gs->ins += dt / 0.7f;
                if (gs->ins >= 1.0f) { finish_load(gs, g); gs->lid_target = 0; gs->auto_stage = GR_CLOSE; }
                break;
            case GR_CLOSE:
                if (s->channel[WCH_LID] <= 0.02f) { b_sound(g, WS_BOLT); gs->auto_stage = GR_NONE; }
                break;
            }
        }
    }

    /* --- vuelo --- */
    if (s->phase == G_FLIGHT) {
        gs->flight_t += dt; gs->fuel -= dt;
        WPose pp; int have = (g && g->projectile) ? g->projectile(gid, &pp) : 0;
        if (have) {
            gs->seen = 1;
            if (gs->proj_valid && dt > 1e-4f) {
                float sp = vlen(vsub(pp.p, gs->proj_pos)) / dt * gs->world_scale;
                gs->speed += (sp - gs->speed) * 0.2f;
            }
            gs->proj_prev = gs->proj_pos; gs->proj_pos = pp.p; gs->proj_valid = 1;
            gs->dist = vlen(vsub(pp.p, gs->launch_pos)) * gs->world_scale;
            gs->alt = fmaxf(0, (pp.p.y - gs->launch_ground_y) * gs->world_scale);
            s->aux.camera = pp;
        }
        /* guiado: inclinación relativa al disparo (orientación de la mano del arma) o stick */
        if (btn_b_edge) { gs->q0 = in->hand[0].q; b_haptic(g, 0, 0.2f, 0.04f); }   /* recentrar */
        WQuat rel = qmul(qconj(gs->q0), in->hand[0].q);
        WVec3 f = qrot(rel, v3(0, 0, -1));
        float yaw = atan2f(f.x, -f.z), pitch = asinf(clampf(f.y, -1, 1));
        gs->steer_raw[0] = shape(yaw, GD_STEER_DEAD, GD_STEER_FULL);
        gs->steer_raw[1] = shape(pitch, GD_STEER_DEAD, GD_STEER_FULL);
        if (fabsf(gs->stick[0]) > 0.1f || fabsf(gs->stick[1]) > 0.1f) {
            gs->steer_raw[0] = shape(gs->stick[0], 0.1f, 1.0f);
            gs->steer_raw[1] = shape(gs->stick[1], 0.1f, 1.0f);
        }
        float k = 1.0f - expf(-dt / GD_STEER_TAU);
        gs->steer[0] += (gs->steer_raw[0] - gs->steer[0]) * k;
        gs->steer[1] += (gs->steer_raw[1] - gs->steer[1]) * k;
        if (g && g->steer) g->steer(gid, gs->steer[0], gs->steer[1]);

        float nz = 0.10f + 0.20f * clampf(gs->dist / 300.0f, 0, 1);
        if (gs->fuel < 3.0f) nz += 0.25f * (1.0f - gs->fuel / 3.0f);
        nz += fmaxf(0, 1.0f - gs->flight_t / 0.5f) * 0.9f;
        gs->noise = clampf(nz, 0, 1);

        int end = 0;
        if (gs->seen && !have) end = 1;
        else if (!gs->seen && gs->flight_t > GD_CREATE_GRACE) end = 3;     /* el juego tarda ~0,65 s (32 VBlanks) en crear el misil tras el disparo */
        else if (gs->fuel <= 0) end = 2;
        if (end) {
            gs->ended_by = end;
            if (g && g->steer) g->steer(gid, 0, 0);
            s->phase = G_STATIC; s->t = 0; gs->static_t = 0;
            b_haptic(g, 0, end == 1 ? 0.7f : 0.3f, 0.25f);
        }
    } else if (s->phase == G_STATIC) {
        gs->static_t += dt;
        gs->noise = clampf(1.0f - 0.5f * gs->static_t / GD_STATIC_TIME, 0.5f, 1.0f);
        if (gs->static_t >= GD_STATIC_TIME) {
            s->phase = G_IDLE; s->t = 0; gs->proj_valid = 0; gs->noise = 0;
        }
    }

    /* --- animación de tapa, canal del misil, visor --- */
    {
        float lid = s->channel[WCH_LID], tg = gs->lid_target, step = dt / 0.35f;
        lid = lid < tg ? fminf(tg, lid + step) : fmaxf(tg, lid - step);
        s->channel[WCH_LID] = lid;
    }
    if (gs->ins > 0) s->channel[WCH_ROUND] = 1.0f - gs->ins;
    else s->channel[WCH_ROUND] = s->mag_rounds > 0 ? 0.0f : HIDDEN_Z;

    float vt = (s->phase != G_IDLE && gs->eye_near) ? 1.0f : 0.0f;
    gs->view_blend += clampf(vt - gs->view_blend, -dt / 0.25f, dt / 0.25f);

    WAuxView *a = &s->aux;
    a->active = s->phase != G_IDLE;
    a->use_game_camera = s->phase == G_FLIGHT;
    a->target_part = GP_SCREEN; a->width = GD_VIEW_W; a->height = GD_VIEW_H; a->fov_y = 1.05f;
    a->overlay = s->phase == G_FLIGHT ? (WO_RETICLE | WO_TELEMETRY | WO_NOISE | WO_SCANLINES)
               : s->phase == G_STATIC ? (WO_NOISE | WO_SCANLINES) : 0;

    /* misil en la mano libre: se publica como objeto sostenido (mesh del misil) */
    {
        WPose hm; int nm; const WMesh *ms = weapon_meshes(s, &nm);
        s->held[0].mesh = (GP_ROUND < nm && ms[GP_ROUND].nv > 0) ? GP_ROUND : -1;
        s->held[0].active = s->held[0].mesh >= 0 && guided_hand_missile_pose(s, in, &hm);
        if (s->held[0].active) s->held[0].world = hm;
    }
    gs->prev_trig = trig; gs->prev_grip1 = grip1; gs->prev_btn0 = in->buttons[0];
}

/* ------------------------------------------------------------------ servicios para el renderer */
float guided_view_panel(const WeaponState *s, const WInput *in, WPose *out, float *w_m, float *h_m) {
    const GuidedState *gs = (const GuidedState *)s;
    if (s->phase == G_IDLE) return 0.0f;
    float b = gs->view_blend, e = b * b * (3.0f - 2.0f * b);
    WPose W = wcore_weapon_world(s, in);
    WPose small = wpose_mul(W, w_guided.parts[GP_SCREEN].rest);   /* la pantalla no se mueve por canal */

    WVec3 eye = vadd(in->head.p, qrot(in->head.q, v3(0.032f, 0, 0)));
    WVec3 fwd = qrot(W.q, v3(0, 0, -1));
    WVec3 tgt = vadd(pt_world(&W, WP_EYE), fwd);
    WVec3 dir = vnorm(vsub(tgt, eye));
    WVec3 zc = vmul(dir, -1.0f);
    WVec3 up = qrot(in->head.q, v3(0, 1, 0));
    WVec3 xc = vcross(up, zc);
    xc = vnorm(vlen(xc) < 1e-4f ? v3(1, 0, 0) : xc);
    WVec3 yc = vcross(zc, xc);
    WPose big; big.p = vadd(eye, vmul(dir, 0.30f)); big.q = q_from_basis(xc, yc, zc);

    out->p = vadd(small.p, vmul(vsub(big.p, small.p), e));
    out->q = qnlerp(small.q, big.q, e);
    if (w_m) *w_m = 0.10f + (0.62f - 0.10f) * e;
    if (h_m) *h_m = 0.075f + (0.465f - 0.075f) * e;
    return e;
}

int guided_hand_missile_pose(const WeaponState *s, const WInput *in, WPose *out) {
    const GuidedState *gs = (const GuidedState *)s;
    if (!gs->hand_missile || gs->ins > 0.001f || s->phase != G_IDLE) return 0;
    out->p = vadd(in->hand[1].p, qrot(in->hand[1].q, v3(0, 0, -(MISSILE_LEN * 0.5f - 0.10f))));
    out->q = in->hand[1].q;
    return 1;
}

int guided_round_visible(const WeaponState *s) { return s->channel[WCH_ROUND] > -100.0f; }

/* ------------------------------------------------------------------ overlay del visor (software) */
typedef struct { uint8_t r, g, b; } Rgb;
static uint32_t hsh(uint32_t a, uint32_t b) {
    a ^= b * 0x9E3779B1u; a *= 0x85EBCA6Bu; a ^= a >> 13; a *= 0xC2B2AE35u; a ^= a >> 16; return a;
}
typedef struct { uint8_t *p; int w, h; } Img;
static void blend(Img *im, int x, int y, Rgb c, int a) {   /* a: 0..255 */
    if (x < 0 || y < 0 || x >= im->w || y >= im->h) return;
    uint8_t *q = im->p + 4 * ((size_t)y * im->w + x);
    q[0] = (uint8_t)((q[0] * (255 - a) + c.r * a) / 255);
    q[1] = (uint8_t)((q[1] * (255 - a) + c.g * a) / 255);
    q[2] = (uint8_t)((q[2] * (255 - a) + c.b * a) / 255);
    q[3] = 255;
}
static void rect(Img *im, int x0, int y0, int x1, int y1, Rgb c, int a) {
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) blend(im, x, y, c, a);
}
static void line(Img *im, int x0, int y0, int x1, int y1, int th, Rgb c, int a) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, er = dx + dy;
    for (;;) {
        rect(im, x0, y0, x0 + th - 1, y0 + th - 1, c, a);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * er;
        if (e2 >= dy) { er += dy; x0 += sx; }
        if (e2 <= dx) { er += dx; y0 += sy; }
    }
}
static void circle(Img *im, int cx, int cy, int r, int th, Rgb c, int a, int dashed) {
    int n = 8 + r * 4;
    for (int i = 0; i < n; i++) {
        if (dashed && (i / 2) % 2) continue;
        float t = 6.2831853f * i / n;
        rect(im, cx + (int)lroundf(cosf(t) * r), cy + (int)lroundf(sinf(t) * r),
             cx + (int)lroundf(cosf(t) * r) + th - 1, cy + (int)lroundf(sinf(t) * r) + th - 1, c, a);
    }
}

/* fuente 5x7: A-Z 0-9 y signos; filas de 5 bits en texto (legible y sin tablas opacas) */
static const char *const FONT[][2] = {
    {"A","01110 10001 10001 11111 10001 10001 10001"}, {"B","11110 10001 10001 11110 10001 10001 11110"},
    {"C","01110 10001 10000 10000 10000 10001 01110"}, {"D","11110 10001 10001 10001 10001 10001 11110"},
    {"E","11111 10000 10000 11110 10000 10000 11111"}, {"F","11111 10000 10000 11110 10000 10000 10000"},
    {"G","01110 10001 10000 10111 10001 10001 01111"}, {"H","10001 10001 10001 11111 10001 10001 10001"},
    {"I","01110 00100 00100 00100 00100 00100 01110"}, {"J","00111 00010 00010 00010 00010 10010 01100"},
    {"K","10001 10010 10100 11000 10100 10010 10001"}, {"L","10000 10000 10000 10000 10000 10000 11111"},
    {"M","10001 11011 10101 10101 10001 10001 10001"}, {"N","10001 11001 10101 10011 10001 10001 10001"},
    {"O","01110 10001 10001 10001 10001 10001 01110"}, {"P","11110 10001 10001 11110 10000 10000 10000"},
    {"Q","01110 10001 10001 10001 10101 10010 01101"}, {"R","11110 10001 10001 11110 10100 10010 10001"},
    {"S","01111 10000 10000 01110 00001 00001 11110"}, {"T","11111 00100 00100 00100 00100 00100 00100"},
    {"U","10001 10001 10001 10001 10001 10001 01110"}, {"V","10001 10001 10001 10001 10001 01010 00100"},
    {"W","10001 10001 10001 10101 10101 11011 10001"}, {"X","10001 10001 01010 00100 01010 10001 10001"},
    {"Y","10001 10001 01010 00100 00100 00100 00100"}, {"Z","11111 00001 00010 00100 01000 10000 11111"},
    {"0","01110 10001 10011 10101 11001 10001 01110"}, {"1","00100 01100 00100 00100 00100 00100 01110"},
    {"2","01110 10001 00001 00010 00100 01000 11111"}, {"3","11110 00001 00001 01110 00001 00001 11110"},
    {"4","00010 00110 01010 10010 11111 00010 00010"}, {"5","11111 10000 11110 00001 00001 10001 01110"},
    {"6","00110 01000 10000 11110 10001 10001 01110"}, {"7","11111 00001 00010 00100 01000 01000 01000"},
    {"8","01110 10001 10001 01110 10001 10001 01110"}, {"9","01110 10001 10001 01111 00001 00010 01100"},
    {".","00000 00000 00000 00000 00000 01100 01100"}, {":","00000 01100 01100 00000 01100 01100 00000"},
    {"-","00000 00000 00000 11111 00000 00000 00000"}, {"/","00001 00010 00010 00100 01000 01000 10000"},
    {"+","00000 00100 00100 11111 00100 00100 00000"},
};
static void glyph(Img *im, int x, int y, int sc, char ch, Rgb c, int shadow) {
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    for (size_t i = 0; i < sizeof(FONT) / sizeof(FONT[0]); i++) {
        if (FONT[i][0][0] != ch) continue;
        const char *p = FONT[i][1];
        for (int r = 0; r < 7; r++, p++)
            for (int cl = 0; cl < 5; cl++, p++)
                if (*p == '1') {
                    if (shadow) rect(im, x + cl * sc + 1, y + r * sc + 1, x + cl * sc + sc, y + r * sc + sc, (Rgb){0, 0, 0}, 200);
                    rect(im, x + cl * sc, y + r * sc, x + cl * sc + sc - 1, y + r * sc + sc - 1, c, 255);
                }
        return;
    }
}
static int text_w(const char *t, int sc) { return (int)strlen(t) * 6 * sc - sc; }
static void text(Img *im, int x, int y, int sc, const char *t, Rgb c) {
    for (; *t; t++, x += 6 * sc) glyph(im, x, y, sc, *t, c, 1);
}

static void frame(Img *im, float k) {
    int w = im->w, h = im->h, t = (int)fmaxf(2, 4 * k), r = (int)(8 * k);
    Rgb d = {18, 20, 20}, e = {70, 76, 72};
    rect(im, 0, 0, w - 1, t - 1, d, 255); rect(im, 0, h - t, w - 1, h - 1, d, 255);
    rect(im, 0, 0, t - 1, h - 1, d, 255); rect(im, w - t, 0, w - 1, h - 1, d, 255);
    for (int i = 0; i < r; i++) {                         /* esquinas redondeadas (recortadas) */
        int cut = (int)(r - sqrtf((float)(r * r - (r - i) * (r - i))));
        rect(im, 0, i, cut, i, d, 255); rect(im, w - 1 - cut, i, w - 1, i, d, 255);
        rect(im, 0, h - 1 - i, cut, h - 1 - i, d, 255); rect(im, w - 1 - cut, h - 1 - i, w - 1, h - 1 - i, d, 255);
    }
    rect(im, t, t, w - 1 - t, t, e, 120); rect(im, t, h - 1 - t, w - 1 - t, h - 1 - t, e, 120);
    rect(im, t, t, t, h - 1 - t, e, 120); rect(im, w - 1 - t, t, w - 1 - t, h - 1 - t, e, 120);
}

static void scanlines(Img *im, float t, float strength) {
    for (int y = 0; y < im->h; y++) {
        int dark = (y & 1) ? (int)(strength * 100) : 0;
        uint8_t *q = im->p + 4 * (size_t)y * im->w;
        for (int x = 0; x < im->w; x++, q += 4) if (dark) { q[0] = q[0] * (255 - dark) / 255; q[1] = q[1] * (255 - dark) / 255; q[2] = q[2] * (255 - dark) / 255; }
    }
    /* barra de interferencia que sube lentamente */
    int hh = im->h / 10, y0 = (int)(fmodf(t * 0.35f, 1.4f) * im->h) - hh;
    for (int y = y0; y < y0 + hh; y++) {
        if (y < 0 || y >= im->h) continue;
        float u = (float)(y - y0) / hh, a = sinf(u * 3.14159f) * 0.14f;
        uint8_t *q = im->p + 4 * (size_t)y * im->w;
        for (int x = 0; x < im->w; x++, q += 4) { for (int c = 0; c < 3; c++) q[c] = (uint8_t)fminf(255, q[c] + 255 * a * 0.4f); }
    }
}

static void noise_fx(Img *im, uint32_t frame_no, float lvl, int replace) {
    int w = im->w, h = im->h;
    for (int y = 0; y < h; y++) {
        uint8_t *q = im->p + 4 * (size_t)y * w;
        for (int x = 0; x < w; x++, q += 4) {
            int n = (int)(hsh(frame_no * 977u + (uint32_t)y, (uint32_t)x) & 255);
            if (replace) { int v = (int)(40 + n * 0.8f * lvl + 30 * (1 - lvl)); v = v > 255 ? 255 : v; q[0] = (uint8_t)(v * 0.85f); q[1] = (uint8_t)v; q[2] = (uint8_t)(v * 0.9f); q[3] = 255; }
            else { int d = (int)((n - 128) * lvl * 0.55f); for (int c = 0; c < 3; c++) { int v = q[c] + d; q[c] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); } }
        }
    }
    /* desgarros horizontales (pérdida de sincronía) */
    static uint32_t tmp[4096];
    if (w <= 4096) for (int y = 0; y < h; y++) {
        uint32_t hv = hsh(frame_no, (uint32_t)y * 31u + 7u);
        if ((hv % 1000u) < (uint32_t)(lvl * lvl * 40.0f)) {
            int sh = (int)((hv >> 10) % (uint32_t)(1 + (int)(lvl * w * 0.12f)));
            uint32_t *row = (uint32_t *)(im->p + 4 * (size_t)y * w);
            memcpy(tmp, row, (size_t)w * 4);
            for (int x = 0; x < w; x++) row[x] = tmp[(x + sh) % w];
        }
    }
}

void guided_draw_overlay(const WeaponState *s, uint32_t *rgba, int w, int h) {
    const GuidedState *gs = (const GuidedState *)s;
    Img im = {(uint8_t *)rgba, w, h};
    float k = w / 320.0f; if (k < 1) k = 1;
    int sc = (int)fmaxf(1, floorf(k + 0.5f)), th = sc;
    Rgb green = {110, 255, 130}, red = {255, 70, 60}, amber = {255, 200, 80};
    int cx = w / 2, cy = h / 2;
    char buf[48];

    if (s->phase == G_STATIC) {
        float lvl = clampf(gs->noise, 0, 1);
        noise_fx(&im, gs->frame, lvl, 1);
        scanlines(&im, gs->flight_t, 0.5f);
        if (gs->static_t > 0.15f && ((int)(gs->static_t * 6) & 1) == 0) {
            const char *m = gs->ended_by == 2 ? "SIN COMBUSTIBLE" : "SIN SENAL";
            int tw = text_w(m, sc * 2);
            rect(&im, cx - tw / 2 - 6, cy - 9 * sc, cx + tw / 2 + 6, cy + 9 * sc, (Rgb){0, 0, 0}, 160);
            text(&im, cx - tw / 2, cy - 7 * sc, sc * 2, m, (Rgb){235, 235, 235});
        }
        frame(&im, k);
        return;
    }
    if (s->phase == G_IDLE) {          /* pantallita en espera */
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            uint8_t *q = im.p + 4 * ((size_t)y * w + x); q[0] = 6; q[1] = 20; q[2] = 12; q[3] = 255;
        }
        const char *m = s->channel[WCH_LID] > 0.05f ? "TAPA ABIERTA" : (s->mag_rounds > 0 ? "LISTO" : "SIN MISIL");
        Rgb c = s->mag_rounds > 0 && s->channel[WCH_LID] <= 0.05f ? green : amber;
        int tw = text_w(m, sc * 2);
        text(&im, cx - tw / 2, cy - 7 * sc, sc * 2, m, c);
        scanlines(&im, 0, 0.5f);
        frame(&im, k);
        return;
    }

    /* --- vuelo: tinte monocromo verdoso, viñeta, ruido, scanlines --- */
    for (int y = 0; y < h; y++) {
        float dy = (y - cy) / (float)cy;
        uint8_t *q = im.p + 4 * (size_t)y * w;
        for (int x = 0; x < w; x++, q += 4) {
            float dx = (x - cx) / (float)cx;
            float lum = (q[0] * 77 + q[1] * 150 + q[2] * 29) / 256.0f;
            lum = (lum - 128) * 1.15f + 128 + 6;
            float vg = 1.0f - 0.45f * (dx * dx + dy * dy) * 0.5f;
            lum = clampf(lum * vg, 0, 255);
            q[0] = (uint8_t)(lum * 0.80f); q[1] = (uint8_t)lum; q[2] = (uint8_t)(lum * 0.84f); q[3] = 255;
        }
    }
    if (s->aux.overlay & WO_NOISE) noise_fx(&im, gs->frame, clampf(gs->noise, 0, 1), 0);
    if (s->aux.overlay & WO_SCANLINES) scanlines(&im, gs->flight_t, 0.5f);

    if (s->aux.overlay & WO_RETICLE) {
        int g0 = (int)(6 * k), g1 = (int)(18 * k);
        line(&im, cx - g1, cy, cx - g0, cy, th, green, 230); line(&im, cx + g0, cy, cx + g1, cy, th, green, 230);
        line(&im, cx, cy - g1, cx, cy - g0, th, green, 230); line(&im, cx, cy + g0, cx, cy + g1, th, green, 230);
        rect(&im, cx, cy, cx + th - 1, cy + th - 1, green, 255);
        circle(&im, cx, cy, (int)(30 * k), th, green, 150, 1);
        int bx = (int)(52 * k), by = (int)(38 * k), bl = (int)(10 * k);          /* esquinas de encuadre */
        for (int sx = -1; sx <= 1; sx += 2) for (int sy = -1; sy <= 1; sy += 2) {
            line(&im, cx + sx * bx, cy + sy * by, cx + sx * (bx - bl), cy + sy * by, th, green, 200);
            line(&im, cx + sx * bx, cy + sy * by, cx + sx * bx, cy + sy * (by - bl), th, green, 200);
        }
        for (int i = -3; i <= 3; i++) if (i) {                                   /* escala de cabeceo */
            int yy = cy + (int)(i * 14 * k), lw = (int)((i % 2 ? 4 : 7) * k);
            line(&im, cx + bx + (int)(12 * k), yy, cx + bx + (int)(12 * k) + lw, yy, th, green, 130);
            line(&im, cx - bx - (int)(12 * k), yy, cx - bx - (int)(12 * k) - lw, yy, th, green, 130);
        }
        /* cursor de orden de guiado */
        int mx = cx + (int)(gs->steer[0] * 40 * k), my = cy - (int)(gs->steer[1] * 30 * k), mr = (int)(4 * k);
        line(&im, mx - mr, my, mx, my - mr, th, amber, 255); line(&im, mx, my - mr, mx + mr, my, th, amber, 255);
        line(&im, mx + mr, my, mx, my + mr, th, amber, 255); line(&im, mx, my + mr, mx - mr, my, th, amber, 255);
    }
    if (s->aux.overlay & WO_TELEMETRY) {
        int m = (int)(10 * k), ls = 9 * sc, sc1 = sc;
        float d = gs->dist, a = gs->alt;
        snprintf(buf, sizeof buf, "DIST %04d M", (int)clampf(d, 0, 9999)); text(&im, m, m, sc1, buf, green);
        snprintf(buf, sizeof buf, "ALT  %04d M", (int)clampf(a, 0, 9999)); text(&im, m, m + ls, sc1, buf, green);
        snprintf(buf, sizeof buf, "VEL  %04d M/S", (int)clampf(gs->speed, 0, 9999)); text(&im, m, m + 2 * ls, sc1, buf, green);
        float fu = clampf(gs->fuel, 0, GD_FUEL_TIME);
        int low = fu < 3.0f, blinkoff = low && (((int)(gs->flight_t * 4)) & 1);
        snprintf(buf, sizeof buf, "COMB %04.1f S", fu);
        int tw = text_w(buf, sc1);
        if (!blinkoff) text(&im, w - m - tw, m, sc1, buf, low ? red : green);
        int bw = (int)(60 * k), bh = (int)(5 * k);                               /* barra de combustible */
        int bx0 = w - m - bw, by0 = m + ls;
        rect(&im, bx0, by0, bx0 + bw, by0 + bh, (Rgb){0, 0, 0}, 150);
        rect(&im, bx0, by0, bx0 + bw, by0, green, 200); rect(&im, bx0, by0 + bh, bx0 + bw, by0 + bh, green, 200);
        rect(&im, bx0, by0, bx0, by0 + bh, green, 200); rect(&im, bx0 + bw, by0, bx0 + bw, by0 + bh, green, 200);
        rect(&im, bx0 + 1, by0 + 1, bx0 + 1 + (int)((bw - 2) * fu / GD_FUEL_TIME), by0 + bh - 1, low ? red : green, 220);
        /* indicador de dirección (abajo) */
        int gw = (int)(70 * k), gy = h - (int)(16 * k);
        line(&im, cx - gw / 2, gy, cx + gw / 2, gy, th, green, 150);
        line(&im, cx, gy - (int)(3 * k), cx, gy + (int)(3 * k), th, green, 150);
        int gx = cx + (int)(gs->steer[0] * gw / 2);
        rect(&im, gx - (int)(2 * k), gy - (int)(3 * k), gx + (int)(2 * k), gy + (int)(3 * k), amber, 255);
        /* CAM, REC y código de tiempo */
        text(&im, m, h - m - 7 * sc1, sc1, "CAM 1", green);
        int sec = (int)gs->flight_t, ff = (int)((gs->flight_t - sec) * 25);
        snprintf(buf, sizeof buf, "%02d:%02d:%02d", sec / 60, sec % 60, ff);
        tw = text_w(buf, sc1);
        text(&im, w - m - tw, h - m - 7 * sc1, sc1, buf, green);
        if (((int)(gs->flight_t * 2)) & 1) {
            int rr = (int)(3 * k);
            rect(&im, cx - (int)(14 * k), m + 1, cx - (int)(14 * k) + 2 * rr, m + 1 + 2 * rr, red, 255);
            text(&im, cx - (int)(14 * k) + 3 * rr, m, sc1, "REC", red);
        }
    }
    frame(&im, k);
}

int guided_led_face(const WeaponState *s, WScreenFace *out) {
    if (!s || !out || s->def != &w_guided) return 0;
    int np = 0, ni = 0;
    const WPart *parts = weapon_parts(s, &np);
    const WMesh *meshes = weapon_meshes(s, &ni);
    if (!parts || !meshes) return 0;
    int pi = -1;
    for (int i = 0; i < np; i++) if (parts[i].name && !strcmp(parts[i].name, "tube")) pi = i;
    int mi = pi >= 0 ? weapon_part_mesh(s, pi) : -1;
    if (pi < 0 || parts[pi].parent != -1 || mi < 0 || mi >= ni || !meshes[mi].v || !meshes[mi].idx || meshes[mi].nv < 3 || meshes[mi].nv > 4096 || meshes[mi].ni > 65535) return 0;
    const WMesh *m = &meshes[mi];
    const WVec3 o = parts[pi].rest.p;
    /* la cara: triangulo de mayor area con normal +Z, todo en la mitad delantera (z < 0) e izquierda (x < 0) del tubo */
    float best = 0, zf = 0;
    for (uint32_t t = 0; t + 2 < m->ni; t += 3) {
        const float *a = m->v[m->idx[t]].pos, *b = m->v[m->idx[t + 1]].pos, *c = m->v[m->idx[t + 2]].pos;
        float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float nz = e1[0] * e2[1] - e1[1] * e2[0], nx = e1[1] * e2[2] - e1[2] * e2[1], ny = e1[2] * e2[0] - e1[0] * e2[2];
        float ar = 0.5f * sqrtf(nx * nx + ny * ny + nz * nz);
        if (!(ar > 1e-6f) || nz < 0.99f * 2.0f * ar) continue;
        if (fmaxf(a[0], fmaxf(b[0], c[0])) + o.x >= -0.0005f || fmaxf(a[2], fmaxf(b[2], c[2])) + o.z >= -0.0005f) continue;
        if (fabsf(a[2] - b[2]) > 0.0005f || fabsf(a[2] - c[2]) > 0.0005f) continue;
        if (ar > best) { best = ar; zf = a[2] + o.z; }
    }
    if (!(best > 0)) return 0;
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f; int any = 0;
    for (uint32_t t = 0; t + 2 < m->ni; t += 3) {
        const float *p[3] = {m->v[m->idx[t]].pos, m->v[m->idx[t + 1]].pos, m->v[m->idx[t + 2]].pos};
        float e1[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]}, e2[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
        float nz = e1[0] * e2[1] - e1[1] * e2[0], nx = e1[1] * e2[2] - e1[2] * e2[1], ny = e1[2] * e2[0] - e1[0] * e2[2];
        float ar2 = sqrtf(nx * nx + ny * ny + nz * nz);
        if (!(ar2 > 1e-9f) || nz < 0.99f * ar2) continue;
        int ok = 1;
        for (int k = 0; k < 3; k++) if (fabsf(p[k][2] + o.z - zf) > 0.0005f || p[k][0] + o.x >= -0.0005f) ok = 0;
        if (!ok) continue;
        for (int k = 0; k < 3; k++) {
            float x = p[k][0] + o.x, y = p[k][1] + o.y;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
        any = 1;
    }
    if (!any || !(x1 - x0 > 1e-3f) || !(y1 - y0 > 1e-3f)) return 0;
    /* esquina recortada por el hexagono del tubo: vertices del tubo en ese plano sobre el borde inferior (A) y el derecho (B) de la caja */
    int haveA = 0, haveB = 0; float ax = 0, by = 0;
    for (uint32_t i = 0; i < m->nv; i++) {
        float x = m->v[i].pos[0] + o.x, y = m->v[i].pos[1] + o.y, z = m->v[i].pos[2] + o.z;
        if (fabsf(z - zf) > 0.0005f) continue;
        if (fabsf(y - y0) < 0.0005f && x > x0 + 0.002f && x < x1 - 0.002f) { haveA = 1; ax = x; }
        if (fabsf(x - x1) < 0.0005f && y > y0 + 0.002f && y < y1 - 0.002f) { haveB = 1; by = y; }
    }
    const float cx = 0.5f * (x0 + x1), cy = 0.5f * (y0 + y1);
    memset(out, 0, sizeof *out);
    out->face.p = v3(cx, cy, zf + GD_LED_LIFT);
    out->face.q.x = 0; out->face.q.y = 0; out->face.q.z = 0; out->face.q.w = 1;
    const float sc = GD_LED_DIAG / sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));   /* reduccion uniforme hacia el centro: un contorno convexo reducido hacia un punto interior sigue dentro del original */
    out->w = (x1 - x0) * sc; out->h = (y1 - y0) * sc;
    if (haveA && haveB) {
        const float P[5][2] = {{x0, y0}, {ax, y0}, {x1, by}, {x1, y1}, {x0, y1}};
        out->npoly = 5;
        for (int i = 0; i < 5; i++) { out->poly[i][0] = (P[i][0] - cx) * sc; out->poly[i][1] = (P[i][1] - cy) * sc; }
    } else {
        const float P[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
        out->npoly = 4;
        for (int i = 0; i < 4; i++) { out->poly[i][0] = (P[i][0] - cx) * sc; out->poly[i][1] = (P[i][1] - cy) * sc; }
    }
    return 1;
}

int guided_led_route(const WeaponState *s, int mis_alive, int fullscreen) {
    (void)mis_alive;
    if (!s || s->phase == G_IDLE) return GLED_STANDBY;
    if (fullscreen) return GLED_FULLSCREEN;
    return s->phase == G_STATIC ? GLED_STATIC : GLED_LIVE;
}
