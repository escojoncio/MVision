/* vr_reload: recarga VR sobre el sistema de recarga del propio juego.
 *
 * El juego recarga de dos maneras (medido, ver bitacora): (1) al VACIAR el cargador con munición en reserva recarga
 * solo; (2) al desequipar y volver a equipar el arma rápido (el equipar crea el arma con el cargador lleno: capacidad,
 * +1 si había bala en la recámara). El jugador VR hace el gesto físico (cargador fuera, otro dentro, corredera) y esta
 * capa traduce "cargador insertado" en la maniobra del mando que provoca la recarga del juego. La munición mostrada
 * es la del juego (cargador y reserva), nunca una cuenta propia.
 *
 * Máquina de estados (una vez por VBlank, vrreload_frame):
 *   READY   el cargador VR refleja al del juego (solo hacia abajo: nunca se "rellena" sin gesto). Si el juego se queda
 *           sin balas pasa a EMPTY.
 *   EMPTY   el juego ha vaciado (y recargará solo si hay reserva); el disparo VR queda BLOQUEADO (rondas VR = 0) hasta
 *           que el gesto físico termina (arma lista: cargador dentro y bala en la recámara). Entonces se copia el
 *           cargador del juego (ya recargado) a READY. Si el gesto termina y el juego sigue vacío con reserva, se
 *           provoca la recarga con la maniobra.
 *   PRESS   maniobra del mando: método VIA_R1_DOUBLE (dos toques de R1: desequipa y equipa; no abre el menú de armas)
 *           o VIA_R2_TAPS (dos toques de R2: abre brevemente el menú de armas, que refresca el arma). Cada pulsación y
 *           cada hueco duran >= 2 VBlanks (el mando se muestrea cada 2).
 *   VERIFY  espera a que el cargador del juego suba (o llegue a la capacidad) con el arma de nuevo equipada; si el arma
 *           se ha quedado desequipada (un toque se perdió) pulsa R1 una vez más; tras `max_retries` abandona (FAIL
 *           transitorio y vuelve a READY).
 * Modos de recarga (WReloadMode de w_pist): manual / simplificado / automático afectan al ARMA VR (cómo se consigue el
 * gesto); aquí solo importa el flanco "cargador insertado" y "arma lista". En automático el gesto lo hace el arma sola.
 *
 * Contabilidad: ammo_take(id, n) del arma VR (cargador cogido de la cintura = n balas fuera de la reserva) es un libro
 * VIRTUAL (no escribe en el juego): reserva mostrada = total del juego - cargador del juego - lo cogido. Al completar la
 * recarga del juego el libro se pone a 0 (el juego ya movió las balas).
 *
 * No es reentrante; se llama desde el hilo del emulador (gancho de frame).
 */
#pragma once
#include <stdint.h>
#include "bridge.h"
#include "weapon.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { VRR_VIA_R1_DOUBLE = 0, VRR_VIA_R2_TAPS = 1 } VrReloadMethod;
enum { VRR_OFF, VRR_READY, VRR_EMPTY, VRR_PRESS, VRR_VERIFY };

typedef struct VrReloadCfg {
    VrReloadMethod method;
    int taps;                /* nº de toques de la maniobra (defecto 2; con R2 basta 1, con R1 hacen falta 2: desequipar y equipar) */
    int press_vbl;           /* VBlanks pulsado por toque (>= 2) */
    int gap_vbl;             /* VBlanks soltado entre toques (>= 2) */
    int verify_vbl;          /* VBlanks esperando la recarga del juego tras la maniobra (defecto 60) */
    int max_retries;         /* nº de repeticiones de la maniobra / de re-equipar (defecto 2) */
    int empty_wait_vbl;      /* VBlanks que se espera a la recarga propia del juego tras el gesto (defecto 150) */
    int lost_weapon_vbl;     /* VBlanks con "sin arma" tolerados durante la maniobra antes de re-equipar (defecto 24) */
} VrReloadCfg;
void vrreload_cfg_default(VrReloadCfg *c);

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
    int weapon_id;           /* game_weapon_id del arma VR mostrada (-1 ninguna) */
    int mag_in_edge;         /* flanco: cargador insertado (WeaponState.mag_inserted 0 -> 1) */
    int ready;               /* el arma VR está lista para disparar (cargador dentro y bala en la recámara) */
} VrReloadIn;

typedef struct VrReloadOut {
    uint16_t pad_force;      /* bits del mando (activo alto) que se pulsan este VBlank (R1/R2) */
    uint16_t pad_mask;       /* bits que controla la capa: el filtro debe quitar los del usuario mientras `busy` */
    int state;               /* VRR_* */
    int busy;                /* maniobra en curso: no pulsar el botón de ataque ni disparar */
    int fire_allowed;        /* el disparo VR puede salir (READY y cargador del juego > 0) */
    int sync_rounds;         /* >= 0: balas del cargador VR (WeaponState.mag_rounds) que corresponden al juego; -1 = no tocar */
    int sync_chambered;      /* >= 0: bala en la recámara (WeaponState.chambered) que corresponde al juego; -1 = no tocar */
    int sync_force;          /* 0: son TOPES (mag_rounds = min(mag_rounds, sync_rounds)); 1: fijar exactamente (recarga completada) */
    int show_mag, show_reserve;   /* munición del juego para la pantalla */
} VrReloadOut;

typedef struct VrReloadStats {
    uint32_t sequences, ok, retries, fails, empties;
    int last_latency_vbl;    /* VBlanks de la última maniobra hasta ver el cargador lleno */
} VrReloadStats;

void vrreload_init(const VrReloadCfg *c);
const VrReloadCfg *vrreload_config(void);
void vrreload_frame(const VrReloadGame *g, const VrReloadIn *in, VrReloadOut *out);
void vrreload_stats(VrReloadStats *s);
int vrreload_state(void);

/* Contabilidad virtual: se enchufa en WGameBridge.ammo_reserve / ammo_take (no escribe en el juego). */
int vrreload_ammo_reserve(int weapon_id);
void vrreload_ammo_take(int weapon_id, int n);
/* Rellena los punteros de ammo_reserve/ammo_take de un WGameBridge con los anteriores. */
void vrreload_bridge_fill(WGameBridge *b);

#ifdef __cplusplus
}
#endif
