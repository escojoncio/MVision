/* vr_reload: la recarga VR ES la del juego. No hay gesto de cargador, maniobra del mando ni cuenta propia: el arma VR (pistola, fusil, francotirador) refleja SIEMPRE el cargador del juego (GM_Magazine).
 *
 * Cómo recarga el original (decompilación, sna_init.c): con el arma levantada (sub_80057BF0, estado de ataque) y GM_Magazine == 0 con GM_MagazineMax != 0 y GM_Weapons[arma] > 0 pasa a su función de
 * recarga (sna_80057A90), que en su primer tick fija GM_Magazine = min(GM_MagazineMax, GM_Weapons[arma]) (el francotirador siempre 5, sna_init.c:6098-6106). Se llega a ese estado pulsando el botón de
 * ataque con el cargador vacío; el último disparo lo deja ya en él (recarga solo en el fotograma siguiente). Esta capa hace tres cosas, una vez por VBlank (vrreload_frame):
 *   1) ESPEJO. `set_held` (>= 0) dice cuántas balas (cargador + recámara) debe tener el arma VR para igualar el cargador del juego; el anfitrión llama a WeaponDef.make_ready. Se adelanta al evento
 *      (cargador del juego sube: recarga, recogida) o a un arma recién mostrada; si el arma VR tiene MÁS que el juego se corrige ya; si tiene menos sin que el juego haya subido (un disparo VR que el
 *      juego aún no ha gastado, o que no llegó a hacer) se espera REFLECT_SETTLE_VBL VBlanks desde el último disparo VR (más que la vida de una petición de disparo, vr_fire.h req_window).
 *   2) PULSAR DISPARO CON EL CARGADOR VACÍO: flanco de gatillo VR con el cargador del juego a 0 y reserva > 0 -> `pad_force` mantiene el botón de ataque hasta ver el cargador del juego > 0 (o
 *      PRESS_MAX_VBL) y lo suelta: soltar con el juego ya recargando no dispara (el disparo de las armas de una mano ocurre al soltar en el estado de ataque, no en la recarga).
 *   3) RECOGIDA CON EL ARMA SECA: recoger munición solo suma a GM_Weapons[arma] (item.c add_ammo) y no toca GM_Magazine; con el cargador a 0 y el total subido se pide `load_mag` (lo que haría el propio
 *      juego al recargar) y el espejo deja el arma VR lista.
 * `busy` (pulsación + REFLECT_LOCK_VBL tras una recarga del juego): el juego está recargando; el disparo VR no sale y vr_fire no controla el botón. La duración de la animación de recarga del juego
 * no está medida (REFLECT_LOCK_VBL es una estimación).
 *
 * La munición mostrada es la del juego: reserva = total - cargador. No es reentrante; se llama desde el hilo del emulador.
 */
#pragma once
#include <stdint.h>
#include "bridge.h"
#include "weapon.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Estado del juego (puente) */
typedef struct VrReloadGame {
    int ok;                  /* jugador controlable y lectura válida */
    int weapon;              /* arma equipada (índice del puente), -1 ninguna */
    int mag, mag_max;        /* cargador del juego */
    int total;               /* munición total del arma equipada (incluye el cargador) */
    int menu;                /* menú/pausa abierto */
} VrReloadGame;
void vrreload_read_game(const BridgeState *bs, VrReloadGame *g);

typedef struct VrReloadIn {
    int weapon_id;           /* game_weapon_id del arma VR de cargador mostrada (-1 ninguna) */
    int trigger_edge;        /* flanco: el gatillo VR (crudo) acaba de pasar de suelto a apretado */
    int vr_held;             /* balas del arma VR: cargador montado + recámara */
    int vr_shot;             /* el arma VR ha disparado en este VBlank */
} VrReloadIn;

typedef struct VrReloadOut {
    uint16_t pad_force;      /* bits del mando que se pulsan este VBlank (botón de ataque con el cargador vacío) */
    int busy;                /* el juego recarga o se le provoca la recarga: ni disparo VR ni control de vr_fire sobre el botón */
    int fire_allowed;        /* el disparo VR puede salir (cargador del juego > 0 y no recargando) */
    int set_held;            /* >= 0: balas (cargador + recámara) que el arma VR debe tener ya = cargador del juego; -1 = no tocar */
    int show_mag, show_reserve;   /* munición del juego para la pantalla */
    int load_mag;            /* > 0: munición recogida con el arma seca: el anfitrión escribe este valor en GM_Magazine = min(GM_MagazineMax, GM_Weapons[arma]) (francotirador: 5) */
} VrReloadOut;

typedef struct VrReloadStats {
    uint32_t presses, loads, syncs;   /* pulsaciones de ataque con el cargador vacío, cargas por recogida y correcciones del arma VR (set_held) */
} VrReloadStats;

void vrreload_init(void);
void vrreload_frame(const VrReloadGame *g, const VrReloadIn *in, VrReloadOut *out);
void vrreload_stats(VrReloadStats *s);
int vrreload_pickup_pending(void);   /* 1: se recogió munición con el arma seca y el cargador del juego aún no se ha cargado (diagnóstico) */
void vrreload_cancel_press(VrReloadOut *out);   /* anula la pulsación del ataque en curso y rehace out->pad_force/busy/fire_allowed (gatillo bloqueado por la zona de menú) */

/* Reserva mostrada y que ven las armas (WGameBridge.ammo_reserve): total del juego - cargador del juego, del arma equipada; 0 de otra arma. */
int vrreload_ammo_reserve(int weapon_id);

#ifdef __cplusplus
}
#endif
