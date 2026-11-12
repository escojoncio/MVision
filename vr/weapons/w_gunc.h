/* Lógica común de armas de cargador con recarga manual (fusil y francotirador): disparo con
 * cadencia, cerrojo, vainas, retroceso acumulativo, cargador suelto/en la mano, palanca de carga y
 * recargas temporizadas (modos simplificado/automático). C puro, sin plataforma.
 *
 * Flujo de recarga manual (modo WR_MANUAL), todo con la mano libre (mano 1):
 *   1) acercarla al alojamiento (WP_MAGWELL) y pulsar WB_MAG_RELEASE -> el cargador sale; si la mano
 *      aprieta el agarre (>0,5) lo lleva; si no, cae al suelo (sus balas se pierden);
 *   2) cargador viejo en mano: soltar el agarre lejos de la cintura lo tira; soltarlo en la zona de
 *      la cintura lo guarda (devuelve sus balas al contador con ammo_take(id, -n));
 *   3) con el agarre apretado en la zona de la cintura se coge uno nuevo del contador del juego
 *      (ammo_take(id, n)); si no queda munición suena WS_DRY;
 *   4) insertarlo: la parte alta del cargador a menos de GUN_INSERT_R de la boca del alojamiento y
 *      alineado con su eje (cos > mag_align_cos) y con la curva hacia delante;
 *   5) cámara vacía (cerrojo atrás o sin bala): agarrar la palanca (WP_CHARGE), tirar hacia atrás
 *      >90 % de su recorrido y soltar -> encamara. Tirar de la palanca con bala encamarada la expulsa
 *      como bala VIVA (no es un disparo) y se devuelve al contador con ammo_take(id, -1), igual que
 *      hace la pistola; no se pierde ni se duplica ninguna bala.
 * Modos WR_SIMPLIFIED / WR_AUTOMATIC: WB_MAG_RELEASE (en cualquier mano) lanza la secuencia
 * temporizada completa (también partiendo sin cargador); en automático arranca sola 0,3 s después de
 * quedarse vacía y solo si hay reserva (sin reserva espera en silencio; con el botón suena WS_DRY).
 * Cadencia: el sobrante de tiempo entre disparos se arrastra, pero nunca más de un intervalo (o un cuadro).
 * Equipar con el gatillo ya apretado no dispara: hay que soltarlo una vez.
 * Convenio de sonido/háptica: ver cada punto en gun_update. */
#pragma once
#include "weapon.h"
#include "wcore.h"

#define GUN_MAX_CASINGS 10
#define GUN_BURST_MAX 30
#define GUN_INSERT_R 0.05f

enum { GP_READY, GP_NOMAG, GP_NEEDCHARGE, GP_AUTO };   /* WeaponState.phase */

typedef struct {
    float interval;          /* s entre disparos */
    float bolt_cycle;        /* s de ida y vuelta del cerrojo por disparo */
    float charge_travel;     /* m de recorrido de la palanca */
    float charge_grab_r;     /* m para agarrar la palanca */
    float magwell_r;         /* m de la mano libre al alojamiento para soltar el cargador */
    float mag_align_cos;     /* alineación mínima del eje del cargador al insertar */
    float burst_kick;        /* rad/s extra de cabeceo a plena ráfaga (se suma al kick_up del arma) */
    float mag_len;           /* longitud del cargador (m) */
    float mag_in_hand;       /* m que sobresale el cargador por encima de la mano */
    WVec3 mag_rest;          /* centro del cargador montado, espacio del arma */
    WVec3 eject_pos, eject_vel;   /* espacio del arma */
    float floor_drop;        /* m desde la cabeza hasta el suelo (para el rebote de vainas/cargadores) */
    int can_select;          /* WB_STICK de la mano del arma alterna auto/semi */
    int has_safety;          /* WB_STICK alterna seguro (el arma no dispara con seguro) */
    int start_semi;
} GunCfg;

typedef struct { int active; WPose pose; WVec3 v, w; float life; int rested; } GunLoose;

typedef struct {
    WeaponState base;
    int booted;
    float cd; int latch, dry_latch, safe, semi;
    float burst, idle_t;
    float bolt_t, bolt_pos; int bolt_locked;
    float mag_slide;
    int hand_mag, hand_mag_rounds, hand_mag_old;
    GunLoose mag_loose;
    int charge_hand, charge_pulled; float charge_z0, charge_pos;
    int seq; float seq_t; float empty_t;
    uint32_t btn_prev[2]; float grip_prev[2];
    float bump_t;
    uint32_t seed;
    GunLoose casing[GUN_MAX_CASINGS]; int casing_next;
    int shots;
    int equip_latch;         /* gatillo apretado al equipar: no dispara hasta soltarlo */
    int sight_on;            /* 1: el disparo sale por `sight` (pose en espacio del arma) en vez de WP_MUZZLE (mira telescópica del francotirador) */
    WPose sight;
} GunState;

void gun_init(GunState *g, const WeaponDef *d, const GunCfg *c);
/* aim: rotación extra (local) de la boca para el disparo (balanceo de mira); NULL = ninguna. */
void gun_update(GunState *g, const GunCfg *c, const WInput *in, WReloadMode rm,
                const WGameBridge *br, const WQuat *aim);

/* Datos para el render: cargador suelto cayendo, cargador en la mano y vainas. Devuelven 1 si hay. */
/* WeaponDef.make_ready de las armas de este núcleo (GunState es el principio de su estado): cargador montado con `mag_rounds`, bala en la recámara, cerrojo cerrado, sin secuencia en curso. */
void gun_make_ready(WeaponState *s, int mag_rounds);
int gun_loose_mag(const GunState *g, WPose *out);
int gun_held_mag(const GunState *g, const GunCfg *c, const WInput *in, WPose *out);
int gun_casings(const GunState *g, WPose *out, int max);
/* Zona de la cintura donde se cogen cargadores (mundo). */
WVec3 gun_waist_zone(const WInput *in, float *radius);
