#pragma once
/* ns_wphide: oculta en la escena nativa el arma propia del juego (el modelo que el motor dibuja en la mano de Snake) cuando la capa VR dibuja la suya.
 * Con la lista de la GPU se oculta escribiendo +0x4C = 0 en los modelos del arma (vrfire_hide_weapon); el extractor nativo NO lee ese contador y el arma
 * no está en NsPlayerMap (no es parte del cuerpo del jugador), así que salía como una «pistola fantasma» detrás de la de VR. Aquí se localiza la instancia
 * por IDENTIDAD (NsInstance.key = 0x80000000 | dirección física del conjunto de piezas del actor del arma) y se marcan todas sus piezas NS_PF_HIDDEN.
 * Ruta en la RAM (misma que vr/gameplay/vr_fire.c): *BA_PLAYER_CTRL = bloque de control; actor del arma = *(ctrl - 0x20 + 0x904); conjunto de piezas = *(actor + 0x20).
 * Sin dependencias del runtime: recibe la RAM como puntero (2 MB). No escribe en la RAM. */
#include <stdint.h>
#include "ns_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NS_WPHIDE_PLAYER_CTRL 0x800ACB84u   /* = BA_PLAYER_CTRL (vr/bridge/bridge_addrs.h): puntero al bloque de control del jugador */
#define NS_WPHIDE_FROM_CTRL   0x20u         /* bloque del jugador = puntero de control - 0x20 */
#define NS_WPHIDE_WEAPON_ACTOR 0x904u       /* actor del arma equipada en el bloque del jugador (0 = ninguna) */
#define NS_WPHIDE_WA_OBJS     0x20u         /* actor del arma: conjunto de piezas del modelo */

/* on = 0: no hace nada. Devuelve cuántas piezas se marcaron ocultas (0 si no hay arma, los punteros no son válidos o el arma no está en la escena). */
int ns_wphide_apply(NsScene *sc, const uint8_t *ram, int on);

#ifdef __cplusplus
}
#endif
