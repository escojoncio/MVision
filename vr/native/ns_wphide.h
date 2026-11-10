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

/* Clave de instancia (NsInstance.key) del actor del arma equipada, con las mismas comprobaciones de la RAM que ns_wphide_apply; 0 = sin arma o punteros no válidos. */
uint32_t ns_wphide_key(const uint8_t *ram);

/* on = 0: no hace nada. Devuelve cuántas piezas se marcaron ocultas (0 si no hay arma, los punteros no son válidos o el arma no está en la escena). */
int ns_wphide_apply(NsScene *sc, const uint8_t *ram, int on);

/* Cuelga el arma del juego de otra mano: el juego la coloca respecto a la mano de la ANIMACIÓN (Ha = Ra | ta, mundo del motor) y aquí se lleva a la mano que la capa VR dibuja (Hd = Rd | td, p. ej. la de la IK):
 * cada pieza W -> Hd * inv(Ha) * W, con la relación mano-arma del juego intacta. mirror = 1 (Hd es la mano IZQUIERDA y Ha la derecha): la relación se refleja en el eje lateral de la pieza mano (la pieza izquierda
 * es la derecha con X invertida). R fila-mayor 3x3 ortonormal; t en unidades del motor. Devuelve las piezas movidas (0 si no hay arma en la escena, argumentos o punteros no válidos: no toca nada). */
int ns_wphide_carry(NsScene *sc, const uint8_t *ram, const float Ra[9], const float ta[3], const float Rd[9], const float td[3], int mirror);

/* Traslación (unidades del motor) de la primera pieza de la instancia del arma equipada, tal como queda en la escena (tras ns_wphide_carry): dónde ve el jugador el objeto de la mano.
 * 1 = ok; 0 = sin arma en la escena, argumentos o punteros no válidos, o traslación no finita (p no se toca). */
int ns_wphide_pos(const NsScene *sc, const uint8_t *ram, float p[3]);

/* 1 si la escena contiene una instancia con esa clave (NsInstance.key) y sus piezas caben en la lista; 0 si no (clave 0 incluida). */
int ns_wphide_has(const NsScene *sc, uint32_t key);

#ifdef __cplusplus
}
#endif
