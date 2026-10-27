/* Arrojadizas y explosivos colocables: API propia (extiende el contrato sin tocar weapon.h).
 * El lanzamiento físico y la colocación llegan al juego por dos vías equivalentes:
 *   1) un gancho opcional (w_throw_set_hook) que recibe el evento completo;
 *   2) WGameBridge.fire(), con el evento completo disponible en w_launch_last()
 *      (se rellena ANTES de llamar a fire, así que es legible dentro del callback). */
#pragma once
#include "weapon.h"

/* Tipos de evento que se entregan al juego. */
typedef enum {
    WL_THROW = 1,    /* arrojadiza lanzada: pose + velocidades + espoleta restante */
    WL_INHAND,       /* espoleta agotada con el objeto aún en la mano (explota en la mano) */
    WL_PLACE_C,      /* carga adhesiva colocada: pose de la cara adhesiva (+Z local sale de la superficie) */
    WL_DETONATE,     /* detonación remota de una carga ya colocada (pose = la de su colocación) */
    WL_PLACE_MINE    /* mina colocada: pose del cuerpo; cara frontal = -Z local */
} WLaunchKind;

typedef struct {
    WLaunchKind kind;
    int weapon_id;           /* game_weapon_id */
    WPose pose;              /* mundo: centro del objeto (WL_THROW) o cara de apoyo (colocables) */
    WVec3 lin_vel;           /* m/s, mundo, del centro del objeto */
    WVec3 ang_vel;           /* rad/s, mundo (eje * velocidad) */
    float fuse_left;         /* s de espoleta restantes (0 = no aplica) */
    float cooked;            /* s que estuvo cocinada antes del lanzamiento */
    int index;               /* WL_DETONATE: índice de la carga; resto: 0 */
    uint32_t seq;            /* contador creciente de eventos */
} WLaunch;

/* Gancho opcional; devuelve 1 si el juego aceptó el evento (p.ej. hay superficie
 * para la carga), 0 si lo rechaza. Un rechazo no consume munición ni cambia el estado del arma
 * (la arrojadiza sigue en la mano; la carga no se registra; el detonador conserva esa carga).
 * Sin gancho se asume aceptado. */
typedef int (*WLaunchHook)(void *user, const WLaunch *ev);
void w_throw_set_hook(WLaunchHook hook, void *user);
const WLaunch *w_launch_last(void);

/* Sonidos propios: están en el enum WS_* del contrato (weapon.h); estos son solo alias de lectura. */
enum { W5_S_DRAW = WS_DRAW, W5_S_PIN = WS_PIN, W5_S_LEVER = WS_LEVER, W5_S_THROW = WS_THROW,
       W5_S_PLACE = WS_PLACE, W5_S_CAP = WS_CAP, W5_S_BTN = WS_BTN, W5_S_LEGS = WS_LEGS, W5_S_EMPTY = WS_EMPTY };

/* ---------- Utilidades comunes (w_throw.c) ---------- */
/* Entrega un evento al juego (gancho o fire). Devuelve 1 si fue aceptado. */
int  w5_emit(const WGameBridge *g, WLaunch ev);
/* Oculta el arma (en el chaleco): desplaza el modelo fuera de vista vía recoil_offset. */
void w5_hide(WeaponState *s, int hidden);

/* ---------- Estimación de lanzamiento (testeable por separado) ---------- */
#define WV_HIST 24
typedef struct { float t; WPose p; } WVSample;
typedef struct { WVSample s[WV_HIST]; int n, head; float clock; } WVHist;
void wvh_reset(WVHist *h);
void wvh_push(WVHist *h, float dt, WPose p);
/* Velocidad lineal/angular de la mano ajustada por mínimos cuadrados en la ventana
 * [now - lag - win, now - lag]. La corrección por aceleración (parábola) se mezcla en parte con la recta
 * (WVH_ACCEL_BLEND). Devuelve nº de muestras usadas. dt <= 0 o NaN no se registra. */
#define WVH_ACCEL_BLEND 0.9f
int  wvh_estimate(const WVHist *h, float win, float lag, WVec3 *lin, WVec3 *ang);

/* ---------- Arrojadizas ---------- */
/* HELD: en la mano abierta. READY: mano cerrada sobre el objeto (el anfitrión mantiene el botón de ataque del juego, ver vr_fire: es el «preparar» del propio
 * juego); abrirla lanza. */
enum { WT_PH_VEST, WT_PH_HELD, WT_PH_READY, WT_PH_GONE };
typedef struct {
    WeaponState base;
    WVHist hist;
    int kind;                 /* 0 variante base, 1 y 2 variantes (espoletas distintas) */
    int primed;               /* la mano ya estuvo abierta con el objeto: cerrarla lo prepara (sacarlo con el puño cerrado no lo prepara) */
    float retry;              /* s hasta reintentar un lanzamiento que el juego rechazó */
} WThrowState;
extern const WeaponDef w_throw_frag, w_throw_b, w_throw_c;

/* ---------- Colocables ---------- */
enum { WPL_PH_VEST, WPL_PH_HELD, WPL_PH_PLACED_OUT };
typedef struct {
    WeaponState base;
    int kind;                 /* 0 carga adhesiva, 1 mina */
    float deploy;             /* mina: patas desplegadas 0..1 */
    int trig_ready;           /* el gatillo pasó por reposo tras sacar el objeto: puede prepararse */
    int armed;                /* gatillo apretado a fondo: al soltarlo se coloca */
} WPlacedState;
extern const WeaponDef w_placed_a, w_placed_b;
