/* Lógica común de armas de cargador (fusil y francotirador): disparo con cadencia, cerrojo, vainas y retroceso acumulativo. C puro, sin plataforma.
 *
 * NO hay recarga VR (ni cargador que se tira y se mete, ni palanca de carga): las balas del arma (mag_rounds + chambered) son un reflejo del cargador del juego (GM_Magazine); el anfitrión
 * (vrgame.c + vr/gameplay/vr_reload) las iguala con WeaponDef.make_ready (gun_make_ready) y el juego recarga solo, como en el original. Disparar consume la bala de la recámara y pasa la
 * siguiente del cargador; con cargador y recámara vacíos el cerrojo queda atrás.
 * Cadencia: el sobrante de tiempo entre disparos se arrastra, pero nunca más de un intervalo (o un cuadro).
 * Equipar con el gatillo ya apretado no dispara: hay que soltarlo una vez.
 * Convenio de sonido/háptica: ver cada punto en gun_update. */
#pragma once
#include "weapon.h"
#include "wcore.h"

#define GUN_MAX_CASINGS 10
#define GUN_BURST_MAX 30

enum { GP_READY, GP_NOMAG, GP_EMPTY };   /* WeaponState.phase: lista / sin cargador montado / sin bala en la recámara */

typedef struct {
    float interval;          /* s entre disparos */
    float bolt_cycle;        /* s de ida y vuelta del cerrojo por disparo */
    float burst_kick;        /* rad/s extra de cabeceo a plena ráfaga (se suma al kick_up del arma) */
    WVec3 eject_pos, eject_vel;   /* espacio del arma */
    float floor_drop;        /* m desde la cabeza hasta el suelo (para el rebote de vainas) */
    int can_select;          /* WB_STICK de la mano del arma alterna auto/semi */
    int has_safety;          /* WB_STICK alterna seguro (el arma no dispara con seguro) */
    int start_semi;
} GunCfg;

typedef struct { int active; WPose pose; WVec3 v, w; float life; int rested; } GunLoose;

typedef struct {
    WeaponState base;
    float cd; int latch, dry_latch, safe, semi;
    float burst, idle_t;
    float bolt_t, bolt_pos; int bolt_locked;
    uint32_t btn_prev[2];
    uint32_t seed;
    GunLoose casing[GUN_MAX_CASINGS]; int casing_next;
    int shots;
    int equip_latch;         /* gatillo apretado al equipar: no dispara hasta soltarlo */
    int sight_on;            /* 1: el disparo sale por `sight` (pose en espacio del arma) en vez de WP_MUZZLE (mira telescópica del francotirador) */
    WPose sight;
} GunState;

void gun_init(GunState *g, const WeaponDef *d, const GunCfg *c);
/* aim: rotación extra (local) de la boca para el disparo (balanceo de mira); NULL = ninguna.
 * rm se ignora (no hay recarga VR); se conserva en la firma por el contrato WeaponDef.update. */
void gun_update(GunState *g, const GunCfg *c, const WInput *in, WReloadMode rm,
                const WGameBridge *br, const WQuat *aim);

/* WeaponDef.make_ready de las armas de este núcleo (GunState es el principio de su estado): cargador montado con `mag_rounds`, bala en la recámara, cerrojo cerrado. */
void gun_make_ready(WeaponState *s, int mag_rounds);
/* Vainas expulsadas (para el render). */
int gun_casings(const GunState *g, WPose *out, int max);
