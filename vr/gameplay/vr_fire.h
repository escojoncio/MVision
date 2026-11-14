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
enum { VRFIRE_ARM_ON_TRIGGER = 0,  /* pulsa cuadrado al disparar (primer disparo ~0,4 s; ~0,1 s con vrfire_skip_raise; sin disparos sobrantes) */
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

/* Anula todo disparo pedido que aún no ha producido proyectil (la mano entró en la zona de menú: el gatillo ya no es del arma). Las peticiones sin soltar desaparecen y el botón se suelta como un desarmado;
 * las ya soltadas (el juego va a crear la bala) y la cola de la ráfaga automática se suprimen cuando el juego cree el proyectil (misma ventana que el disparo de desarmado, si suppress_disarm).
 * Una vez por VBlank antes de vrfire_frame, también sin peticiones (no hace nada). Devuelve cuántas peticiones anuló. */
int vrfire_cancel_pending(void);

/* Salta el levantado del arma del juego. Pistola, fusil de asalto y lanzamisiles guiado comparten función de estado (decompilación: sna_init.c sna_gun_800540D0): tras pulsar el ataque cuenta 8 ticks
 * (16 VBlanks) antes de pasar al estado que dispara (sub_80057BF0); el contador (field_910; edición europea: bloque del jugador + 0x90C) lo incrementa el juego en cada tick. Con una petición de disparo
 * en curso y el botón ya pulsado por esta capa, en cuanto el juego ha arrancado el ataque (contador 1..7, arma aún sin apuntar) se escribe 8: el tick siguiente cambia de estado y el otro dispara
 * (~5 VBlanks de la petición a la bala en vez de ~21), y vrfire_frame suelta el botón sin esperar a «apuntando» (el soltar queda en cola en el propio juego). Las demás armas no se tocan (su función
 * de estado usa el contador para otra cosa o no lo tiene). Una vez por VBlank, antes de vrfire_frame. Devuelve 1 si escribió. */
int vrfire_skip_raise(uint8_t *ram);

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
float vrfire_throw_mps(void);      /* diagnóstico: velocidad (m/s) guardada por el último vrfire_set_throw, ya en unidades del motor y vuelta a metros */
/* Granada en la mano: el anfitrión da en cada escena nativa la posición (unidades del motor) del objeto del juego que lleva la mano VR (ns_wphide_pos tras el carry). Si hay una de las últimas
 * VBlanks, el actor de la granada lanzada nace ahí (sin adelantar la trayectoria: se ve salir de la mano) con la velocidad del gesto; si no, la trayectoria adelantada de siempre. NULL / no finito: se ignora. */
void vrfire_set_throw_origin(const float p_engine[3]);
/* Validación del siguiente vrfire_set_throw_origin (se consume con él): body = posición (motor) del cuerpo del jugador; wall_between = hay una pared de colisión entre el cuerpo y el origen (lo calcula el anfitrión con el
 * rayo HZD). Con pared, o con el origen a más de 1,5 m del cuerpo en horizontal, el origen se descarta (y el anterior también): el actor nace con la trayectoria adelantada. NULL / no finito: sin validar. */
void vrfire_set_throw_body(const float body_engine[3], int wall_between);
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
    uint32_t lat_last, lat_max;          /* VBlanks entre la petición de disparo (gatillo) y el proyectil sustituido: el último y el máximo (la espera es sobre todo el levantar del arma del juego) */
    uint32_t cancelled;                  /* peticiones de disparo anuladas por vrfire_cancel_pending */
} VrFireStats;
void vrfire_stats(VrFireStats *s);
/* Sonda de pruebas: los sustitutos la llaman tras ejecutar el original (result = valor devuelto, normalmente el puntero
 * al actor creado). NULL por defecto. */
typedef void (*VrFireProbe)(int kind, int decision, uint32_t result, const VrFireMat *m);
void vrfire_set_probe(VrFireProbe f);
void vrfire_probe(int kind, int decision, uint32_t result, const VrFireMat *m);
/* Dirección del último actor de misil creado (0 = ninguno) y nº de misiles creados desde el arranque. */
uint32_t vrfire_last_missile(uint32_t *serial);
/* Misil guiado del juego (bullet/rmissile.c NewRMissile, Nikita). Dos arreglos del arranque, ambos solo para ese misil (el antiaéreo no los usa):
 *  1) vrfire_level_matrix: GM_ConfigControlMatrix saca la guiñada del misil con DG_MatrixRotYXZ(matriz) y SOLO usa rot.vy (GV_DirVec2(rot.vy...)). El juego original le pasa siempre RotMatrixYXZ(-1024, guiñada, 0) (cañón
 *     nivelado: punto de bloqueo del cardán, donde vy es exactamente la guiñada). Con la matriz de una mano real (cabeceo y balanceo de unos grados) la descomposición YXZ sale del bloqueo y vy ya no es la guiñada del cañón
 *     (con el cañón casi nivelado, unos grados de balanceo de la muñeca dan errores de decenas de grados, hasta 90). Se sustituye por la matriz nivelada con la MISMA guiñada y la misma posición. Devuelve 0 (sin tocar) si el
 *     cañón apunta casi vertical.
 *  2) vrfire_missile_settle: el actor nace en GM_PlayerPosition (los pies de Snake, ctrl->mov) y "planea" hasta la posición de la matriz durante 8 ticks (field_110 = 8; Act: NearExp2 = mitad de la distancia restante por tick). El juego
 *     explota el misil si en un tick sube 200 o más (rmissile_act_helper_8006C114: pos.y - prev.y >= 200). El original lanza a +320 sobre los pies (rcm.c: mt2.t[1] = GM_PlayerPosition.vy + 320; el primer paso del planeo es 160); la boca
 *     del lanzador VR queda a 0,8-1,7 m (paso de 400-850): el misil explotaba al nacer junto a Snake (bola de fuego sobre él). Se coloca el actor ya en su posición de lanzamiento (ctrl->mov y posición del tick anterior = field_100)
 *     y se deja el contador del planeo como está (8): con mov == field_100 el planeo da paso 0, y ponerlo a 1 hacía que el tick 1 fijase target.side = 0 con el blanco aún en los pies de Snake (explosión en su cara; el original mantiene side = 1 durante 7 ticks). Solo toca el actor si el contador vale 1..16 (el valor de creación es 8). Devuelve 1 si lo modificó; info (opcional) recibe lo que había. */
typedef struct VrFireMissileBirth { int16_t home[3], start[3]; int glide; int first_dy; } VrFireMissileBirth;   /* first_dy: primer paso vertical del planeo que habría hecho el juego (mitad de la distancia vertical) */
int vrfire_level_matrix(VrFireMat *m);
int vrfire_missile_settle(uint8_t *ram, uint32_t actor, VrFireMissileBirth *info);
/* Diagnóstico del actor del misil (log [nikita]): contador de planeo (field_110), y del blanco (Work.target): bando, vida (vital) y centro. Los offsets del blanco (+0x120) salen del layout de la decompilación, sin medir en el juego (unverified). Devuelve 0 si el puntero no vale. */
typedef struct VrFireMissileDiag { int glide, side, vital; int16_t center[3]; } VrFireMissileDiag;
int vrfire_missile_diag(const uint8_t *ram, uint32_t actor, VrFireMissileDiag *d);
#define VRFIRE_MIS_MAX_STEP_Y 200   /* paso vertical por tick (unidades del motor) a partir del cual el juego explota el misil */
/* Último proyectil sustituido (para pruebas). */
const VrFireMat *vrfire_last_subst(void);

/* ---- C4 (carga adhesiva) colocada directamente, SIN la animación de Snake ----
 * El juego solo crea la carga cuando su animación de colocar (cuerpo entero: acción 55/56, o la superposición action2 49/54 contra una pared; decompilación sna_init.c sna_80057118 / sna_80057378) llega al fotograma
 * de soltarla: el actor lo crea weapon/bomb.c con NewBakudan (bullet/bakudan.c; edición europea 0x8006B69C = vrfire_charge) cuando el jugador pone field_914_trigger = 3, gasta GM_Weapons[WP_C4] (BA_VARS + 2 * (17 + 6)),
 * suena SE_C4_PUT (0x31) en control.mov y respeta el límite de 16 cargas (contador 0x800A0604). Aquí se hace lo mismo sin pasar por esa animación: el anfitrión pide la colocación con la pose ya calculada
 * (vrfire_c4_request: la matriz del motor se calcula en ese instante, con la correspondencia de ese VBlank) y un gancho que corre dentro del juego (hle_vrfire_c4_pump, vr_fire_hle.c; se llama desde la construcción
 * de la cámara, en la fase de actores) la consume con vrfire_c4_pump: comprueba arma/munición/límite, crea el actor con las operaciones del invitado y solo si existe gasta la unidad y suena. No toca el estado del jugador.
 * Una sola petición a la vez; caduca a los VRFIRE_C4_LIFE VBlanks (vrfire_frame la envejece). La carga adherida a un enemigo sigue haciéndola el propio juego (necesita su blanco y su animación). */
#define VRFIRE_BA_C4_COUNT 0x800A0604u   /* nº de cargas vivas (bakudan_count; NewBakudan lo compara con 16) */
#define VRFIRE_C4_LIFE 50
typedef struct VrFireC4Ops {
    uint32_t (*create)(void *user, const VrFireMat *m);          /* NewBakudan(m, NULL, 0, jugador, 0) en el invitado: devuelve el actor o 0 */
    void (*sound)(void *user, uint32_t pos_addr, int se_id);     /* GM_SeSet(pos, id); NULL = sin sonido */
    void *user;
} VrFireC4Ops;
int vrfire_c4_request(const VrFireMap *map, const WPose *pose);  /* 1 si queda pendiente (mapa y pose válidos, ninguna otra pendiente) */
int vrfire_c4_pending(void);
int vrfire_c4_take(VrFireMat *m);                                /* saca la petición sin comprobar el juego (pruebas); 1 si había */
int vrfire_c4_pump(uint8_t *ram, const VrFireC4Ops *ops);        /* 1 si se creó la carga */

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
