/* vr_fire: disparo VR con las armas del propio juego (no se reimplementan balas ni proyectiles).
 *
 * Idea. El juego crea la bala/proyectil cuando el jugador pulsa su botón de ataque. La capa VR:
 *   1) pulsa/suelta ese botón del mando virtual cuando el arma VR dispara (cadencia, cargador, modos: lógica de
 *      w_pist/w_gunc; aquí solo se traduce "el arma VR ha disparado" en pulsaciones del botón del juego);
 *   2) sustituye (HLE, vr_fire_hle.c) los ARGUMENTOS de las funciones que crean el proyectil: la matriz de
 *      origen/orientación sale de la boca del arma VR (WP_MUZZLE en mundo) en vez de la mano del modelo del jugador;
 *   3) oculta el modelo del arma del propio juego (solo se ve el arma VR).
 *
 * Cómo dispara el juego (edición europea, medido; direcciones en vr_fire_names.json):
 *  - Botón: cuadrado. Cada arma tiene en su tabla de tipo un bit "dispara mientras se mantiene" (bit 0: fusil de
 *    asalto) o "dispara al SOLTAR" (resto: pistola, francotirador, lanzamisiles...). Desde reposo, pulsar cuadrado
 *    levanta el arma (estado "apuntando", ~17 VBlanks = 0,34 s); en ese estado soltar cuadrado dispara 1 bala unos
 *    3 VBlanks después. Un soltar antes de estar apuntando queda en cola y dispara al llegar. Mantener cuadrado
 *    mantiene el estado (no camina); soltar sin más pulsación lo devuelve a reposo unos 10 ticks después. SOLTAR
 *    DISPARA SIEMPRE (consume munición del juego aunque nadie lo pida), de ahí el modo ARM_ON_RAISE y su "disparo de
 *    desarmado".
 *  - El mando se muestrea una vez por tick (2 VBlanks): toda pulsación y todo soltar duran >= 2 VBlanks.
 *    Medido: ciclos de 2 VBlanks soltado + 2 pulsado disparan 1 bala cada ciclo (12 por segundo con 50 Hz).
 *  - La bala (y el proyectil de los lanzamisiles, la mina y la carga adhesiva) se crea con una matriz de 32 B
 *    (matriz de 32 B) donde el eje local -Y es el DELANTE. Las balas son instantáneas (trazo): el origen del trazo
 *    es matriz.t + R*(0,400,0) = 400 unidades DETRÁS de t, así que para que el trazo parta de la boca se pasa
 *    t = boca + 400*delante. El resplandor del disparo parte de t + 130*delante (pistola) / t (fusil).
 *  - La granada se crea con (posición, velocidad por tick, mecha, tipo, modelo); se sustituyen posición y velocidad.
 *    Medido en el código del juego: su actor suma la velocidad a la posición cada tick y resta 16 unidades/tick a la vertical (VRFIRE_GAME_G); el lanzamiento por
 *    defecto sale con (0, 128, 150) unidades/tick en el marco del jugador (197 unidades/tick = 4,9 m/s, a 40 grados).
 *
 * Unidades/ejes. Espacio del usuario (tracking): metros, Y arriba, delante = -Z local de la pose. Mundo del motor:
 * ~1 unidad = 1 mm, Y arriba, guiñada 0 = +Z, 1024 = +X. Correspondencia (la de la cámara VR, vr_camera):
 *   mundo = ojo + upm * R_y(theta_play + 180°) * (p_usuario - p_ref).
 * Todo se llama desde el hilo del emulador (gancho de frame y funciones del motor sustituidas). No es reentrante.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "bridge.h"
#include "weapon.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- correspondencia usuario -> motor (se rellena desde el último registro de cámara) ---- */
typedef struct VrFireMap {
    int valid;               /* 1 si hay cámara VR activa y los demás campos son válidos */
    int32_t eye[3];          /* ojo del motor (unidades) */
    float theta_play_deg;    /* giro espacio de usuario -> mundo */
    float p_ref[3];          /* posición de la cabeza (m, espacio de usuario) a la que corresponde `eye` */
    float units_per_m;       /* unidades del motor por metro (1000) */
} VrFireMap;

/* Matriz del motor (MAT16 de 32 B): m[3][3] fila-mayor en 1.12 (4096 = 1,0), relleno, t[3] en unidades. */
typedef struct VrFireMat {
    int16_t m[9];
    int32_t t[3];
} VrFireMat;

/* Posición/dirección de usuario -> mundo del motor (float). dir no se escala. */
void vrfire_pos_to_world(const VrFireMap *map, const float p[3], float out[3]);
/* Inversas: mundo del motor (unidades) -> espacio de usuario (m). */
void vrfire_pos_from_world(const VrFireMap *map, const float w[3], float out[3]);
void vrfire_dir_from_world(const VrFireMap *map, const float d[3], float out[3]);
void vrfire_dir_to_world(const VrFireMap *map, const float d[3], float out[3]);
/* Matriz del motor para una pose de arma (boca): eje -Y local = delante de la pose (-Z local de WPose), col. 0 =
 * izquierda (arriba x delante), col. 2 = delante x izquierda. t = boca + shift*delante (shift en unidades; 400 para
 * las balas). Devuelve 0 si la correspondencia o la pose no son válidas. */
int vrfire_make_matrix(const VrFireMap *map, const WPose *muzzle, float shift, VrFireMat *out);
/* Escribe/lee la matriz en la RAM del invitado (32 B, little endian). */
void vrfire_store_mat(uint8_t *ram, uint32_t addr, const VrFireMat *m);
void vrfire_load_mat(const uint8_t *ram, uint32_t addr, VrFireMat *m);
/* Dirección de avance (unitaria, mundo del motor) de una matriz del motor: -columna 1. */
void vrfire_mat_forward(const VrFireMat *m, float out[3]);

/* ---- configuración ---- */
enum { VRFIRE_ARM_ON_TRIGGER = 0,  /* pulsa cuadrado al disparar (latencia del primer disparo ~0,4 s, sin disparos sobrantes) */
       VRFIRE_ARM_ON_RAISE = 1 };  /* mantiene cuadrado con el arma levantada (primer disparo ~3 VBlanks; bloquea el andar y
                                    * al bajar el arma el juego dispara un tiro sobrante que se suprime, ver vrfire_stats) */

typedef struct VrFireCfg {
    int arm_mode;            /* VRFIRE_ARM_* (defecto ON_TRIGGER) */
    int press_vbl;           /* VBlanks mínimos pulsado (>= 2: el mando se muestrea cada 2) */
    int release_vbl;         /* VBlanks mínimos soltado (>= 2) */
    int req_window;          /* VBlanks de vida de una petición de disparo (defecto 60) */
    int arm_timeout;         /* VBlanks esperando "apuntando" antes de soltar igualmente (defecto 40) */
    float bullet_shift;      /* unidades que se adelanta t de la bala sobre la boca (400) */
    float flash_shift_a;     /* resplandor de pistola: t = boca - esto*delante (130) */
    int suppress_disarm;     /* 1: suprimir el proyectil del disparo de desarmado (defecto) */
} VrFireCfg;
void vrfire_cfg_default(VrFireCfg *c);

/* ---- estado del juego que necesita la lógica de pulsación (leído de la RAM) ---- */
typedef struct VrFireGame {
    int ok;                  /* lectura válida */
    int weapon;              /* arma equipada (índice del puente), -1 ninguna */
    int armed;               /* el arma del jugador está en el estado "apuntando/disparando" */
    int hold_fires;          /* la tabla de tipo dice "dispara mientras se mantiene" */
    int mag, mag_max;        /* cargador del juego */
    int total;               /* munición total del arma (incluye el cargador) */
} VrFireGame;
void vrfire_read_game(const uint8_t *ram, const BridgeState *bs, VrFireGame *g);

/* ---- entrada por VBlank ---- */
typedef struct VrFireIn {
    int active;              /* el jugador es controlable y el arma VR equipada es la del juego */
    WPose muzzle;            /* pose de WP_MUZZLE en espacio de usuario (delante = -Z local) */
    int ready;               /* arma levantada/apuntando (solo para ARM_ON_RAISE o con `raise`) */
    int raise;               /* este VBlank se trata como ARM_ON_RAISE aunque la configuración sea ON_TRIGGER (lanzador antiaéreo: el visor
                              * levantado mantiene el botón para que el juego tenga su mira y su buscador; ver vr_aa.h) */
    int fire;                /* flanco: el arma VR acaba de disparar (WGameBridge.fire) */
    WPose fire_pose;         /* pose de la boca en el instante de ese disparo (la bala sale con ella, no con la actual) */
    int fire_life;           /* con `fire`: VBlanks de vida de esa petición si superan a req_window (0 = el de la configuración; tope 600). Armas cuyo
                              * proyectil lo crea el juego tras una animación larga (colocar una mina agachándose, ~70 VBlanks) */
    int trigger_held;        /* gatillo mantenido y modo automático (armas que disparan mientras se mantiene) */
    const VrFireMap *map;    /* correspondencia usuario -> motor del frame (NULL/invalid: no se sustituye nada) */
} VrFireIn;
typedef struct VrFireOut {
    int square;              /* 1: el botón de ataque del juego debe estar pulsado este VBlank */
    int engaged;             /* la capa controla el botón (si 0, el botón sigue el mapa normal del mando) */
} VrFireOut;

void vrfire_init(const VrFireCfg *c);
const VrFireCfg *vrfire_config(void);
/* Una vez por VBlank, antes de enviar el mando al juego. */
void vrfire_frame(const VrFireGame *g, const VrFireIn *in, VrFireOut *out);

/* ---- lo que usan los sustitutos HLE (vr_fire_hle.c) ---- */
enum { VRFIRE_PASS = 0,    /* llamar al original con los argumentos del juego */
       VRFIRE_SUBST = 1,   /* llamar con la matriz sustituida */
       VRFIRE_SKIP = -1 }; /* no crear el proyectil (disparo de desarmado) */
/* Se llama cuando el juego va a crear un proyectil del jugador. kind: 0 bala, 1 misil, 2 mina/carga. Rellena `m`
 * (origen de la bala: boca + bullet_shift*delante; para el resto: la boca). */
int vrfire_take_projectile(int kind, VrFireMat *m);
/* Resplandor del disparo: matriz para AN_*Flash tras una bala sustituida (0 = ninguna). style 0: pistola, 1: fusil. */
int vrfire_take_flash(int style, VrFireMat *m);
/* Lanzamiento de granada: posición y velocidad del motor (unidades; v por tick) para la última pose de lanzamiento. */
int vrfire_set_throw(const float pos_user[3], const float vel_user_mps[3], float tick_s);
int vrfire_take_throw(int16_t pos[3], int16_t vel[3]);
/* Granada en la mano: el anfitrión da en cada escena nativa la posición (unidades del motor) del objeto del juego que lleva la mano VR (ns_wphide_pos tras el carry). Si hay una de las últimas
 * VBlanks, el actor de la granada lanzada nace ahí (sin adelantar la trayectoria: se ve salir de la mano) con la velocidad del gesto; si no, la trayectoria adelantada de siempre. NULL / no finito: se ignora. */
void vrfire_set_throw_origin(const float p_engine[3]);
/* Ocultar el modelo de la mano cuando el volador existe: tras crearse el actor se espera a verlo en la escena nativa. vrfire_flier_key() = clave de instancia (NsInstance.key) del objeto del actor
 * mientras se espera (0 si no); el anfitrión llama a vrfire_flier_seen() en la escena que ya lo contiene; desde entonces vrfire_held_hidden() = 1 durante una ventana corta (el modelo de la mano
 * de esa escena y las siguientes se oculta, no se lleva a la mano), sin hueco (se oculta justo cuando el volador está) ni duplicado. */
uint32_t vrfire_flier_key(void);
void vrfire_flier_seen(void);
int vrfire_held_hidden(void);
/* Diagnóstico por stderr de un lanzamiento: original frente a sustituido y validez del mapa (ver vr_fire.c). */
void vrfire_log_grenade(const int16_t o_pos[3], const int16_t o_vel[3], int subst, const int16_t pos[3], const int16_t vel[3]);
/* Seguimiento diagnóstico del actor de la granada creada (lo arma vrfire_probe(3, ..., actor)): una línea [granada] cada 0,25 s durante 3 s con su posición, velocidad,
 * mecha y lo que decide si la escena nativa lo dibuja (oculto, grupo, cola). Una vez por VBlank, con la RAM del juego y el tiempo real transcurrido. 1 = escribió una línea. */
int vrfire_grenade_track(const uint8_t *ram, float dt_s);

typedef struct VrFireStats {
    uint32_t requests, expired;          /* peticiones de disparo recibidas / caducadas sin proyectil */
    uint32_t subst, passed, skipped;     /* proyectiles sustituidos / dejados / suprimidos */
    uint32_t presses, releases;          /* pulsaciones y soltados del botón */
    uint32_t arm_timeouts;
} VrFireStats;
void vrfire_stats(VrFireStats *s);
/* Sonda de pruebas: los sustitutos la llaman tras ejecutar el original (result = valor devuelto, normalmente el puntero
 * al actor creado). NULL por defecto. */
typedef void (*VrFireProbe)(int kind, int decision, uint32_t result, const VrFireMat *m);
void vrfire_set_probe(VrFireProbe f);
void vrfire_probe(int kind, int decision, uint32_t result, const VrFireMat *m);
/* Dirección del último actor de misil creado (0 = ninguno) y nº de misiles creados desde el arranque. */
uint32_t vrfire_last_missile(uint32_t *serial);
/* Último proyectil sustituido (para pruebas). */
const VrFireMat *vrfire_last_subst(void);

/* ---- modelo del arma del juego (ocultar) ---- */
/* Pone a 0 el límite de pantalla de los modelos del arma que lleva el jugador, tras la etapa de recorte: no se dibuja.
 * Devuelve el nº de modelos ocultados (0 si no hay arma o los punteros no son válidos). */
int vrfire_hide_weapon(uint8_t *ram);
/* El anfitrión lo activa mientras la capa VR manda (cámara VR activa y arma VR mostrada); por defecto desactivado. */
void vrfire_set_hide(int on);
int vrfire_hide_enabled(void);

/* Utilidad de integración: correspondencia desde el último registro de la cámara VR (vr_camera.h). */
struct VrCamRecord;
void vrfire_map_from_cam(VrFireMap *m, const struct VrCamRecord *r);

#ifdef __cplusplus
}
#endif
