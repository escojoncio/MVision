#pragma once
/* ns_sink: piezas de objetos dinámicos HUNDIDAS bajo un suelo del escenario que el motor ordena «por detrás» (pies de los personajes).
 *
 * El problema (medido en un volcado del visor): en una pasarela los soldados pisan 0,12-0,32 m por debajo de la cara superior del suelo. El motor no tiene búfer de profundidad: ordena por
 * z media y a las mallas de suelo les pone un sesgo de orden NEGATIVO (NsMesh.depth_bias = -250..-1000 unidades de profundidad: «dibújame detrás»), así que las botas se pintan encima. El
 * renderer nativo usa búfer de profundidad y el suelo se comía los pies y media espinilla.
 *
 * Qué NO sirve (probado): adelantar todos los objetos dinámicos un margen fijo recorta el cuerpo propio contra el plano cercano y saca personajes y puertas por delante de las paredes; alejar
 * los suelos a la escala real del sesgo deja ver por los bordes de las pasarelas la pared de debajo.
 *
 * Qué se hace: por pieza y solo si hace falta. Una pieza de un cuerpo articulado (personaje) con algún vértice por DEBAJO de una cara de suelo con sesgo negativo, dentro de su contorno (planta XZ) y a menos de ese sesgo
 * de ella, recibe un adelanto de orden a lo largo de la línea de visión (el `raise` del renderer: no cambia su posición en pantalla) de lo justo para asomar: hundimiento / sen(elevación del
 * ojo sobre el suelo), con el propio sesgo del motor como tope y dejando siempre 0,3 m hasta el ojo. Vista desde debajo del suelo no se toca. El adelanto viaja en NsPiece.flags
 * (NS_PF_LIFT_*) y ns_gpu_make_piece lo pasa a NsGpuPiece.raise. Sin dependencias de plataforma; hilo del productor (captura). */
#include "ns_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NS_PF_LIFT_SHIFT 8          /* NsPiece.flags bits 8-15: adelanto de orden de la pieza, en pasos de NS_LIFT_UNIT unidades del motor (0 = ninguno) */
#define NS_PF_LIFT_MASK 0xFF00u
#define NS_LIFT_UNIT 8.0f
#define NS_SINK_EYE_KEEP 300.0f     /* unidades del motor que el adelanto deja siempre entre la pieza y el ojo */
#define NS_SINK_MIN_SIN 0.2f        /* seno mínimo de la elevación del ojo (a ras de suelo el adelanto necesario se dispara: se acota) */
#define NS_SINK_MAX 450.0f          /* hundimiento máximo (unidades) que se trata como «pisar por debajo del suelo» (medido: hasta 322 en pies, ~150 en cuerpos caídos); más hondo es otra planta y no se toca */
#define NS_SINK_MIN_PIECES 8        /* solo cuerpos articulados (personajes: 16 piezas). Puertas, ascensores y objetos de una pieza se hunden en el suelo a propósito y adelantarlos los sacaría por delante del marco o la pared donde se guardan */

typedef struct { uint32_t floor_tris, pieces_tested, pieces_lifted; float max_sink, max_lift; } NsSinkStats;

/* Marca en sc->piece[].flags el adelanto de las piezas dinámicas hundidas. `st`: almacén de mallas de la escena. Devuelve cuántas piezas marcó. `stats` puede ser NULL. */
int ns_sink_apply(NsScene *sc, const NsMeshStore *st, NsSinkStats *stats);
/* Adelanto (unidades del motor) que lleva una pieza. */
static inline float ns_sink_lift_of(uint16_t piece_flags) { return (float)((piece_flags & NS_PF_LIFT_MASK) >> NS_PF_LIFT_SHIFT) * NS_LIFT_UNIT; }

#ifdef __cplusplus
}
#endif
