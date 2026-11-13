/* Fusil de asalto bullpup: definición, piezas y config. La mecánica está en w_gunc.c.
 *
 * Parámetros físicos:
 *  - cadencia 750 rpm (intervalo 80 ms; la del juego en PAL); cerrojo ida y vuelta en 45 ms;
 *  - retroceso: resorte k=220, amortiguación ζ≈0,85 (c=25); por disparo 0,30 m/s hacia atrás y
 *    0,9 rad/s de cabeceo; a 12,5 disp/s eso da ~2 cm de retroceso y ~3,7 grados de subida
 *    sostenida a una mano, más hasta 0,55 rad/s extra de cabeceo según crece la ráfaga (30 disparos);
 *    con la mano de apoyo en el guardamanos se multiplica por 0,45. */
#include <string.h>
#include "w_rifle.h"

#define POSE(x, y, z) { { x, y, z }, { 0, 0, 0, 1 } }

static const WPart rifle_parts[RIFLE_PT_N] = {
    { "body",     -1, POSE(0, 0, 0), -1, WCH_NONE, { 0, 0, 0 }, 0 },
    { "trigger",  0,  POSE(0.00000f, 0.04500f, -0.03500f), -1,
      WCH_TRIGGER, { -0.40f, 0, 0 }, 1 },
    { "selector", 0,  POSE(0.02650f, 0.08500f, 0.04500f), -1,
      WCH_SAFETY, { 1.1f, 0, 0 }, 1 },
    { "mag",      0,  POSE(0.00000f, -0.04000f, 0.10500f), -1,
      WCH_MAG, { 0, -0.15000f, 0 }, 0 },
    { "charge",   0,  POSE(0.00000f, 0.15200f, 0.07000f), -1,
      WCH_CUSTOM0, { 0, 0, 0.06500f }, 0 },
    { "bolt",     0,  POSE(0.02650f, 0.09800f, -0.02000f), -1,
      WCH_BOLT, { 0, 0, 0.05000f }, 0 },
};

static const GunCfg rifle_cfg = {
    .interval = 60.0f / RIFLE_RPM, .bolt_cycle = 0.045f,
    .burst_kick = 0.55f,
    .eject_pos = { 0.03f, 0.098f, -0.02f }, .eject_vel = { 2.2f, 1.6f, 0.6f },
    .floor_drop = 1.65f, .can_select = 1, .has_safety = 0, .start_semi = 0,
};

typedef struct { GunState g; } RifleState;   /* GunState empieza por WeaponState */

static void rifle_init(WeaponState *s) {
    RifleState *r = (RifleState *)s;
    gun_init(&r->g, &w_rifle, &rifle_cfg);   /* limpia el estado entero conservando el binding */
}
static void rifle_update(WeaponState *s, const WInput *in, WReloadMode rm, const WGameBridge *g) {
    gun_update(&((RifleState *)s)->g, &rifle_cfg, in, rm, g, 0);
}

static const char *const rifle_mesh_names[] = { "body", "trigger", "selector", "mag", "charge", "bolt", "casing", NULL };

const WeaponDef w_rifle = {
    .id = "rifle", .game_weapon_id = 1,    /* fusil, tabla del puente */
    .mesh_names = rifle_mesh_names,
    .meshes = 0, .nmeshes = 0,   /* sin mallas propias: el cargador de modelos las asocia por nombre de pieza */
    .parts = rifle_parts, .nparts = RIFLE_PT_N,
    .points = {
        [WP_GRIP]    = POSE(0, 0, 0),
        [WP_SUPPORT] = POSE(0.01400f, 0.11600f, -0.23000f),
        [WP_MUZZLE]  = POSE(0.00000f, 0.09000f, -0.45500f),
        [WP_EYE]     = POSE(0.00000f, 0.21200f, 0.22500f),
        [WP_MAGWELL] = POSE(0.00000f, -0.03300f, 0.10500f),
        [WP_CHARGE]  = POSE(0.00000f, 0.15200f, 0.07600f),
        [WP_SCREEN]  = POSE(0, 0, 0),
    },
    .recoil = { .kick_back = 0.30f, .kick_up = 0.9f, .kick_yaw_rand = 0.25f,
                .stiffness = 220.0f, .damping = 25.0f, .two_hand_factor = 0.45f },
    .mode = WF_AUTO, .rpm = RIFLE_RPM, .mag_capacity = RIFLE_MAG,
    .state_size = sizeof(RifleState),
    .init = rifle_init, .update = rifle_update,
    .make_ready = gun_make_ready,
};

int rifle_casings(const WeaponState *s, WPose *out, int max) { return gun_casings(&((const RifleState *)s)->g, out, max); }
int rifle_is_semi(const WeaponState *s) { return ((const RifleState *)s)->g.semi; }
