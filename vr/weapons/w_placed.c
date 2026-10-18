/* Explosivos colocables: carga adhesiva con detonador remoto y mina direccional.
 * Se sacan del chaleco (agarre en el pecho). El juego resuelve la superficie: aquí solo se entrega la pose
 * (cara adhesiva en la carga; cuerpo con frente = -Z en la mina) por WL_PLACE_C / WL_PLACE_MINE.
 * Si el juego rechaza el evento (w5_emit devuelve 0) no se consume munición ni se registra nada. */
#include "w_throw.h"
#include "wcore.h"
#include <string.h>
#include <math.h>


/* Datos ajustables (marco del arma; se sustituyen al asociar las partes del modelo original). */
#define BLOCK_FACE   {0, 0, -0.0225f}       /* centro de la cara adhesiva de la carga; +Z sale de la superficie */
#define REM_BTN      {0, 0.0145f, 0.027f}   /* botón del mando, marco del mando */
#define REM_LID      {0, 0.0165f, 0.0345f}  /* charnela de la tapa de seguridad */
#define REM_ANT      {-0.018f, 0, -0.055f}
#define REM_BTN_TRAVEL 0.0045f
#define REM_LID_ANGLE  1.9f
/* Piezas esperadas (nombre -> canal). Carga (placed_a): "block","adh","det","ant"(CUSTOM1 antena). Mando (WHeld):
 * rem, rbtn(CUSTOM0), rlid(LID), rant. Mina (placed_b): "body","plate","sight","legl","legr"(CUSTOM0 = despliegue;
 * legr rota -Z, legl +Z). Las piezas llevan mesh = -1: se asocian por nombre a mesh_names. */
#define PART(nm, ch, ax, ay, az, rot) { nm, -1, {{0,0,0},{0,0,0,1}}, -1, ch, {ax, ay, az}, rot }
static const WPart CARGA_PARTS[] = { PART("block", WCH_NONE,0,0,0,0), PART("adh", WCH_NONE,0,0,0,0), PART("det", WCH_NONE,0,0,0,0),
    PART("ant", WCH_CUSTOM1, 0, 0.03f, 0, 0) };
static const WPart MINE_PARTS[] = { PART("body", WCH_NONE,0,0,0,0), PART("plate", WCH_NONE,0,0,0,0), PART("sight", WCH_NONE,0,0,0,0),
    PART("legr", WCH_CUSTOM0, 0,0,-1.25f, 1), PART("legl", WCH_CUSTOM0, 0,0,1.25f, 1) };
static const char *const CARGA_MESH_NAMES[] = { "block", "adh", "det", "ant", "rem", "rbtn", "rlid", "rant", NULL };
static const char *const MINE_MESH_NAMES[] = { "body", "plate", "sight", "legl", "legr", NULL };
#define CARGA_MESH_REMOTE 4       /* índice de "rem" en CARGA_MESH_NAMES; le siguen botón, tapa y antena */
#define DT_MAX         0.1f
#define GRIP_OFF       0.25f
#define PLACE_TRIG     0.80f     /* gatillo a fondo = pegar la carga / soltar la mina */
#define BTN_TRIG       0.70f
#define CAP_AUTOCLOSE  4.0f      /* la tapa se cierra sola si no se pulsa */
#define LEGS_TIME      0.40f
#define RESPAWN_T      0.40f
#define PLACE_ID_A     103
#define PLACE_ID_B     104

static void snd(const WGameBridge *g, int id, int ev) { if (g && g->sound) g->sound(id, ev); }
static void hap(const WGameBridge *g, int hand, float a, float sec) { if (g && g->haptic) g->haptic(hand, a, sec); }

static void placed_init_k(WeaponState *s, const WeaponDef *d, int kind) {
    WPlacedState *p = (WPlacedState *)s;
    wcore_state_reset(s, d);
    for (int i = 0; i < 4; i++) p->held_mesh[i] = kind == 0 ? CARGA_MESH_REMOTE + i : -1;
    p->kind = kind;
    s->phase = WPL_PH_VEST;
    w5_hide(s, 1);
}

void w_placed_forget(WPlacedState *p, int index) {
    if (index < 0 || index >= p->n_placed) return;
    for (int i = index; i < p->n_placed - 1; i++) p->placed[i] = p->placed[i + 1];
    p->n_placed--;
}

int w_placed_remote_pose(const WPlacedState *p, const WInput *in, WPose *out) {
    if (p->kind != 0 || p->n_placed <= 0) return 0;
    *out = in->hand[1];
    return 1;
}

static WPose local(WVec3 pos, WVec3 axis, float ang) {
    WPose r; r.p = pos; r.q = ang != 0.0f ? wquat_axis_angle(axis, ang) : wquat_identity(); return r;
}

/* Mando remoto como objetos sueltos (cuerpo, botón, tapa, antena) en la mano 1. */
static void sync_remote(WPlacedState *p, const WInput *in) {
    WeaponState *s = &p->base;
    WPose root;
    int vis = w_placed_remote_pose(p, in, &root);
    static const WVec3 BTN = REM_BTN, LID = REM_LID, ANT = REM_ANT;
    WPose parts[4];
    parts[0] = root;
    parts[1] = wpose_mul(root, local(wv_add(BTN, wv(0, -REM_BTN_TRAVEL * p->btn, 0)), wv(1, 0, 0), 0));
    parts[2] = wpose_mul(root, local(LID, wv(1, 0, 0), REM_LID_ANGLE * p->cap_open));
    parts[3] = wpose_mul(root, local(ANT, wv(1, 0, 0), -80.0f * 3.14159265f / 180.0f));
    for (int i = 0; i < 4; i++) {
        s->held[i].active = vis; s->held[i].mesh = p->held_mesh[i];
        if (vis) s->held[i].world = parts[i];
    }
}

static void clear_remote(WeaponState *s) { for (int i = 0; i < 4; i++) s->held[i].active = 0; }

/* Detonador: A (pulgar, mano 1) abre/cierra la tapa; con la tapa abierta, gatillo o B pulsan el botón. */
static void remote_update(WPlacedState *p, const WInput *in, const WGameBridge *g, float dt) {
    WeaponState *s = &p->base;
    int id = s->def->game_weapon_id;
    if (p->n_placed <= 0) { p->cap_open = 0; p->btn = 0; p->cap_target = 0; return; }
    uint32_t b = in->buttons[1];
    if (wcore_edge_down(p->btn_prev[1], b, WB_A)) {
        p->cap_target = !p->cap_target; p->cap_t = 0;
        snd(g, id, W5_S_CAP); hap(g, 1, 0.25f, 0.03f);
    }
    if (p->cap_target) { p->cap_t += dt; if (p->cap_t > CAP_AUTOCLOSE && p->btn < 0.1f) p->cap_target = 0; }
    p->cap_open = wcore_approach(p->cap_open, (float)p->cap_target, dt / 0.15f);
    int press = (in->trigger[1] > BTN_TRIG || (b & WB_B)) ? 1 : 0;
    int was = p->btn > 0.5f;
    if (p->cap_open > 0.9f && press) {
        p->btn = wcore_approach(p->btn, 1.0f, dt / 0.06f);
        if (!was && p->btn > 0.5f) {                 /* flanco: detonación remota de todas las cargas */
            snd(g, id, W5_S_BTN); hap(g, 1, 0.9f, 0.08f);
            int keep = 0;                                   /* las cargas que el juego rechaza se conservan */
            for (int i = 0; i < p->n_placed; i++) {
                WLaunch ev; memset(&ev, 0, sizeof ev);
                ev.kind = WL_DETONATE; ev.weapon_id = id; ev.pose = p->placed[i]; ev.index = i;
                if (!w5_emit(g, ev)) p->placed[keep++] = p->placed[i];
            }
            p->n_placed = keep; p->cap_t = 0; p->det_fired = 1;
        }
    } else p->btn = wcore_approach(p->btn, 0.0f, dt / 0.08f);
    if (p->cap_open < 0.9f && press && !was) hap(g, 1, 0.1f, 0.02f);    /* tapa de seguridad cerrada: no hace nada */
    if (p->det_fired && !press) { p->det_fired = 0; p->cap_target = 0; }  /* se cierra tras soltar el botón */
    s->channel[WCH_LID] = p->cap_open; s->channel[WCH_CUSTOM0] = p->btn;
}

static void placed_update(WeaponState *s, const WInput *in, WReloadMode rm, const WGameBridge *g) {
    WPlacedState *p = (WPlacedState *)s;
    const WeaponDef *d = s->def;
    int id = d->game_weapon_id;
    float dt = wcore_dt(in->dt, DT_MAX);
    if (dt <= 0) return;                       /* dt NaN, 0 o negativo: cuadro inválido */
    s->t += dt;
    int reserve = g && g->ammo_reserve ? g->ammo_reserve(id) : 1;
    if (reserve < 0) reserve = 0;
    s->mag_rounds = reserve;

    switch (s->phase) {
    case WPL_PH_VEST:
        if (w5_draw_gesture(in, rm, p->grip_prev)) {
            if (reserve > 0) {
                s->phase = WPL_PH_HELD; s->t = 0; w5_hide(s, 0);
                p->deploy = 0; p->trig_ready = 0; s->channel[WCH_CUSTOM1] = 0; s->channel[WCH_CUSTOM0] = p->kind ? 0.0f : s->channel[WCH_CUSTOM0];
                snd(g, id, W5_S_DRAW); hap(g, 0, 0.3f, 0.04f);
                if (p->kind) snd(g, id, W5_S_LEGS);
            } else { snd(g, id, W5_S_EMPTY); snd(g, id, WS_DRY); hap(g, 0, 0.15f, 0.03f); }
        }
        break;
    case WPL_PH_PLACED_OUT:
        if (s->t > RESPAWN_T) { s->phase = WPL_PH_VEST; s->t = 0; }
        break;
    case WPL_PH_HELD:
        if (p->kind == 1) {                                   /* patas abatibles: se despliegan al sacarla */
            p->deploy = wcore_approach(p->deploy, 1.0f, dt / LEGS_TIME);
            s->channel[WCH_CUSTOM0] = p->deploy;
        } else {                                              /* antena de la espoleta: se extiende al apretar */
            s->channel[WCH_CUSTOM1] = wcore_approach(s->channel[WCH_CUSTOM1], in->trigger[0] > 0.3f ? 1.0f : 0.0f, dt / 0.12f);
        }
        if (in->grip[0] < GRIP_OFF) {                         /* soltar sin colocar: vuelve al chaleco */
            s->phase = WPL_PH_VEST; w5_hide(s, 1); break;
        }
        if (in->trigger[0] < 0.3f) p->trig_ready = 1;       /* sacar con el gatillo apretado no coloca */
        if (in->trigger[0] > PLACE_TRIG && p->trig_ready && (p->kind == 1 ? p->deploy > 0.95f : 1)) {
            WLaunch ev; memset(&ev, 0, sizeof ev);
            ev.weapon_id = id;
            WPose w = wcore_weapon_world(s, in);
            if (p->kind == 0) {
                static const WVec3 face = BLOCK_FACE;
                ev.kind = WL_PLACE_C;
                ev.pose.p = wpose_apply(w, face); ev.pose.q = w.q;        /* +Z local sale de la superficie */
            } else {
                ev.kind = WL_PLACE_MINE; ev.pose = w;                     /* orientación = la de la mano */
            }
            if (p->kind == 0 && p->n_placed >= WPL_MAX) { snd(g, id, WS_DRY); break; }
            if (w5_emit(g, ev)) {
                if (g && g->ammo_take && reserve > 0) g->ammo_take(id, 1);
                if (p->kind == 0) p->placed[p->n_placed++] = ev.pose; else p->n_placed++;
                snd(g, id, W5_S_PLACE); hap(g, 0, 0.7f, 0.06f);
                s->phase = WPL_PH_PLACED_OUT; s->t = 0; w5_hide(s, 1);
            } else { snd(g, id, WS_DRY); hap(g, 0, 0.2f, 0.04f); }   /* el juego no encontró superficie */
        }
        break;
    }
    if (p->kind == 0) { remote_update(p, in, g, dt); sync_remote(p, in); }
    else clear_remote(s);
    p->btn_prev[0] = in->buttons[0]; p->btn_prev[1] = in->buttons[1];
    p->grip_prev = in->grip[0];
}

#define ID_Q {0, 0, 0, 1}
#define PL_PTS(MUZ) { \
    [WP_GRIP]    = {{0, 0, 0}, ID_Q}, [WP_SUPPORT] = {{0, 0, 0}, ID_Q}, [WP_MUZZLE] = {MUZ, ID_Q}, \
    [WP_EYE]     = {{0, 0, 0}, ID_Q}, [WP_MAGWELL] = {{0, 0, 0}, ID_Q}, [WP_CHARGE] = {{0, 0, 0}, ID_Q}, \
    [WP_SCREEN]  = {{0, 0, 0}, ID_Q} }
#define PL_RECOIL { .kick_back = 0, .kick_up = 0, .kick_yaw_rand = 0, .stiffness = 0, .damping = 0, .two_hand_factor = 1 }
static void placed_init_a(WeaponState *s);
static void placed_init_b(WeaponState *s);
const WeaponDef w_placed_a = {
    .id = "placed_a", .game_weapon_id = PLACE_ID_A, .meshes = NULL, .nmeshes = 0,
    .parts = CARGA_PARTS, .nparts = 4, .points = PL_PTS(BLOCK_FACE), .recoil = PL_RECOIL,
    .mode = WF_PLACED, .rpm = 0, .mag_capacity = 1, .state_size = sizeof(WPlacedState),
    .init = placed_init_a, .update = placed_update, .mesh_names = CARGA_MESH_NAMES };
const WeaponDef w_placed_b = {
    .id = "placed_b", .game_weapon_id = PLACE_ID_B, .meshes = NULL, .nmeshes = 0,
    .parts = MINE_PARTS, .nparts = 5, .points = PL_PTS(((WVec3){0, 0, -0.03f})), .recoil = PL_RECOIL,
    .mode = WF_PLACED, .rpm = 0, .mag_capacity = 1, .state_size = sizeof(WPlacedState),
    .init = placed_init_b, .update = placed_update, .mesh_names = MINE_MESH_NAMES };
/* Un init por variante: no dependen de que el llamador haya asignado s->def. */
static void placed_init_a(WeaponState *s) { placed_init_k(s, &w_placed_a, 0); }
static void placed_init_b(WeaponState *s) { placed_init_k(s, &w_placed_b, 1); }
