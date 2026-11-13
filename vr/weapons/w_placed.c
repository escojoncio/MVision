/* Explosivos colocables: carga adhesiva y mina direccional.
 * Aparecen en la mano al equiparlos (sin gesto del chaleco). Se coloca soltando el gatillo (tras apretarlo a fondo): el juego resuelve la superficie y aquí
 * solo se entrega la pose (cara adhesiva en la carga; cuerpo con frente = -Z en la mina) por WL_PLACE_C / WL_PLACE_MINE. La detonación de las cargas es el
 * AGARRE del mando derecho con el C4 equipado (vrgame.c c4_remote lo convierte en el círculo que ve el juego; sin mando remoto, tapa ni botón virtuales).
 * Si el juego rechaza el evento (w5_emit devuelve 0: sin superficie al alcance) no se consume munición y el objeto sigue en la mano. */
#include "w_throw.h"
#include "wcore.h"
#include <string.h>
#include <math.h>


/* Datos ajustables (marco del arma; se sustituyen al asociar las partes del modelo original). */
/* GRIP de la carga (0xf83d, marco HELD; bloque X -92..8, Y -79..170 con antena, Z -156..-56 mm: wcut_tables.c): el centro del bloque en X, a media altura y 19 mm dentro de la cara trasera (la del detonador),
 * para que la palma quede en esa cara y los dedos rodeen el bloque (con la empunadura en el origen, fuera del bloque, la mano cogía el aire y el modelo quedaba 6-16 cm por delante). */
#define BLOCK_GRIP   {-0.042f, 0.0f, -0.075f}
/* Centro de la cara adhesiva de la carga; +Z sale de la superficie. 22,5 mm por delante de la empunadura (la misma pose de colocación respecto a la mano de antes de mover la empunadura). */
#define BLOCK_FACE   {-0.042f, 0.0f, -0.0975f}
/* Mina (0x17a1, marco HELD): cara picuda (de disparo) +X, eje largo Z, mira +Y. En la mano va girada R_y(+90) respecto al mando (zócalo FIT_CLAYMORE de vr_grip): cara picuda al frente, eje largo de
 * izquierda a derecha. El evento de colocación lleva la pose del modelo girada R_y(-90): su -Z es la cara picuda, y el anfitrión (place_project) proyecta y orienta con ese delante. */
#define MINE_FACE_Q  {0, -0.70710678f, 0, 0.70710678f}
/* Piezas esperadas (nombre -> canal). Carga (placed_a): "block","adh","det","ant"(CUSTOM1 antena). Mina (placed_b): "body","plate","sight","legl","legr"
 * (CUSTOM0 = despliegue; legr rota -Z, legl +Z). Las piezas llevan mesh = -1: se asocian por nombre a mesh_names. */
#define PART(nm, ch, ax, ay, az, rot) { nm, -1, {{0,0,0},{0,0,0,1}}, -1, ch, {ax, ay, az}, rot }
static const WPart CARGA_PARTS[] = { PART("block", WCH_NONE,0,0,0,0), PART("adh", WCH_NONE,0,0,0,0), PART("det", WCH_NONE,0,0,0,0),
    PART("ant", WCH_CUSTOM1, 0, 0.03f, 0, 0) };
static const WPart MINE_PARTS[] = { PART("body", WCH_NONE,0,0,0,0), PART("plate", WCH_NONE,0,0,0,0), PART("sight", WCH_NONE,0,0,0,0),
    PART("legr", WCH_CUSTOM0, 0,0,-1.25f, 1), PART("legl", WCH_CUSTOM0, 0,0,1.25f, 1) };
static const char *const CARGA_MESH_NAMES[] = { "block", "adh", "det", "ant", NULL };
static const char *const MINE_MESH_NAMES[] = { "body", "plate", "sight", "legl", "legr", NULL };
#define DT_MAX         0.1f
#define PLACE_TRIG     0.80f     /* gatillo a fondo = preparado para colocar */
#define PLACE_REL      0.30f     /* soltar por debajo de esto = colocar */
#define LEGS_TIME      0.40f
#define RESPAWN_T      0.40f
#define PROBE_ON_T     0.08f     /* s con superficie valida seguidos para dar el pulso */
#define PROBE_OFF_T    0.30f     /* s sin ella seguidos para volver a poder darlo (histeresis: sin vibrar en continuo) */
#define PLACE_ID_A     6       /* adhesivo con mando */
#define PLACE_ID_B     5       /* mina */

static void snd(const WGameBridge *g, int id, int ev) { if (g && g->sound) g->sound(id, ev); }
static void hap(const WGameBridge *g, int hand, float a, float sec) { if (g && g->haptic) g->haptic(hand, a, sec); }

static void placed_init_k(WeaponState *s, const WeaponDef *d, int kind) {
    WPlacedState *p = (WPlacedState *)s;
    wcore_state_reset(s, d);
    p->kind = kind;
    s->phase = WPL_PH_VEST;
    w5_hide(s, 1);
}

/* Evento de colocación con la pose actual del objeto en la mano (el de soltar el gatillo y el del sondeo). */
static WLaunch place_event(const WeaponState *s, const WInput *in, const WPlacedState *p, int id) {
    WLaunch ev; memset(&ev, 0, sizeof ev);
    ev.weapon_id = id;
    WPose w = wcore_weapon_world(s, in);
    if (p->kind == 0) {
        static const WVec3 face = BLOCK_FACE;
        ev.kind = WL_PLACE_C;
        ev.pose.p = wpose_apply(w, face); ev.pose.q = w.q;        /* +Z local sale de la superficie */
    } else {
        static const WQuat face = MINE_FACE_Q;
        ev.kind = WL_PLACE_MINE; ev.pose.p = w.p; ev.pose.q = wquat_mul(w.q, face);   /* -Z del evento = cara picuda (+X del modelo) = delante del mando */
    }
    return ev;
}

static void placed_update(WeaponState *s, const WInput *in, WReloadMode rm, const WGameBridge *g) {
    (void)rm;                                  /* sin gesto del chaleco el modo de recarga no interviene */
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
    case WPL_PH_VEST:     /* sin objeto en la mano: lo equipado en el menú del juego aparece en la mano si queda reserva (sin gesto del chaleco) */
        if (reserve > 0) {
            s->phase = WPL_PH_HELD; s->t = 0; w5_hide(s, 0);
            p->deploy = 0; p->trig_ready = 0; p->armed = 0; p->probe_on = 0; p->probe_t_on = p->probe_t_off = 0; s->channel[WCH_CUSTOM1] = 0; s->channel[WCH_CUSTOM0] = 0;
            snd(g, id, W5_S_DRAW); hap(g, 0, 0.3f, 0.04f);
            if (p->kind) snd(g, id, W5_S_LEGS);
        }
        break;
    case WPL_PH_PLACED_OUT:
        if (s->t > RESPAWN_T) { s->phase = WPL_PH_VEST; s->t = 0; }
        break;
    case WPL_PH_HELD: {
        if (p->kind == 1) {                                   /* patas abatibles: se despliegan al sacarla */
            p->deploy = wcore_approach(p->deploy, 1.0f, dt / LEGS_TIME);
            s->channel[WCH_CUSTOM0] = p->deploy;
        } else {                                              /* antena de la espoleta: se extiende al apretar */
            s->channel[WCH_CUSTOM1] = wcore_approach(s->channel[WCH_CUSTOM1], in->trigger[0] > 0.3f ? 1.0f : 0.0f, dt / 0.12f);
        }
        int ready = p->kind == 1 ? p->deploy > 0.95f : 1;
        if (ready && w5_probe(place_event(s, in, p, id))) {        /* sondeo: pulso corto al pasar de sin superficie a con superficie, una vez por entrada */
            p->probe_t_off = 0; p->probe_t_on += dt;
            if (!p->probe_on && p->probe_t_on >= PROBE_ON_T) { p->probe_on = 1; hap(g, 0, 0.35f, 0.05f); }
        } else {
            p->probe_t_on = 0; p->probe_t_off += dt;
            if (p->probe_on && p->probe_t_off >= PROBE_OFF_T) p->probe_on = 0;
        }
        if (in->trigger[0] > PLACE_TRIG && p->trig_ready && ready) p->armed = 1;     /* sacar con el gatillo apretado no prepara */
        if (in->trigger[0] >= PLACE_REL) break;
        p->trig_ready = 1;
        if (!p->armed) break;
        p->armed = 0;                                         /* soltar tras apretar a fondo = colocar */
        WLaunch ev = place_event(s, in, p, id);
        if (w5_emit(g, ev)) {
            if (g && g->ammo_take && reserve > 0) g->ammo_take(id, 1);
            snd(g, id, W5_S_PLACE); hap(g, 0, 0.7f, 0.06f);
            s->phase = WPL_PH_PLACED_OUT; s->t = 0; w5_hide(s, 1);
        } else { snd(g, id, WS_DRY); hap(g, 0, 0.2f, 0.04f); }       /* el juego no encontró superficie al alcance */
        break;
    }
    }
}

#define ID_Q {0, 0, 0, 1}
/* GRIP: el punto del modelo que la mano empuña (el zócalo FIT_CLAYMORE_GRIP de vr_grip.c pone la mano respecto a él). Mina (marco del arma; cuerpo X -68..31, Y -99..100, Z -370..-45 mm; con patas X -80..31, Y -224..100: w_laser.h WGHOST_MINE_RAW_*): la mano la coge por el MEDIO,
 * palma en la cara trasera central y dedos por encima del canto superior; GRIP = centro del canto superior, dentro del cuerpo (gripfit_gen.c, CLAYMORE). */
#define PL_PTS_G(GRIP, ...) { \
    [WP_GRIP]    = {GRIP, ID_Q}, [WP_SUPPORT] = {{0, 0, 0}, ID_Q}, [WP_MUZZLE] = {__VA_ARGS__, ID_Q}, \
    [WP_EYE]     = {{0, 0, 0}, ID_Q}, [WP_MAGWELL] = {{0, 0, 0}, ID_Q}, [WP_CHARGE] = {{0, 0, 0}, ID_Q}, \
    [WP_SCREEN]  = {{0, 0, 0}, ID_Q} }
#define PL_RECOIL { .kick_back = 0, .kick_up = 0, .kick_yaw_rand = 0, .stiffness = 0, .damping = 0, .two_hand_factor = 1 }
static void placed_init_a(WeaponState *s);
static void placed_init_b(WeaponState *s);
const WeaponDef w_placed_a = {
    .id = "placed_a", .game_weapon_id = PLACE_ID_A, .meshes = NULL, .nmeshes = 0,
    .parts = CARGA_PARTS, .nparts = 4, .points = PL_PTS_G(((WVec3)BLOCK_GRIP), ((WVec3)BLOCK_FACE)), .recoil = PL_RECOIL,
    .mode = WF_PLACED, .rpm = 0, .mag_capacity = 1, .state_size = sizeof(WPlacedState),
    .init = placed_init_a, .update = placed_update, .mesh_names = CARGA_MESH_NAMES };
const WeaponDef w_placed_b = {
    .id = "placed_b", .game_weapon_id = PLACE_ID_B, .meshes = NULL, .nmeshes = 0,
    .parts = MINE_PARTS, .nparts = 5, .points = PL_PTS_G(((WVec3){-0.015f, 0.090f, -0.205f}), ((WVec3){0, 0, -0.03f})), .recoil = PL_RECOIL,
    .mode = WF_PLACED, .rpm = 0, .mag_capacity = 1, .state_size = sizeof(WPlacedState),
    .init = placed_init_b, .update = placed_update, .mesh_names = MINE_MESH_NAMES };
/* Un init por variante: no dependen de que el llamador haya asignado s->def. */
static void placed_init_a(WeaponState *s) { placed_init_k(s, &w_placed_a, 0); }
static void placed_init_b(WeaponState *s) { placed_init_k(s, &w_placed_b, 1); }
