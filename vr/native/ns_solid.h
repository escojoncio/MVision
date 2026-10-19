#pragma once
/* ns_solid: geometría SÓLIDA de la escena nativa en el mundo del motor, para la colisión de la cabeza (vr/camera/vr_headcol.h). La escena nativa trae TODO el
 * escenario y los objetos dinámicos en 360 grados (no solo lo que ve la cámara del juego, como la lista de la GPU), así que la cabeza no tiene huecos detrás ni a
 * los lados. Sin dependencias de plataforma (C99, libm). Hilo: el que llama (en la app, el de emulación, justo antes de publicar la escena).
 *
 * Qué entra: todas las caras de las piezas no ocultas con malla en el almacén, del escenario (NS_IF_STATIC) y de los objetos dinámicos, cuadriláteros con la
 * diagonal del hardware ((A,B,D) y (B,D,C)); sin culling (la colisión es de doble cara). Qué NO entra: la instancia propia del jugador (skip_key), las mallas
 * semitransparentes (NS_MF_BLEND: agua, cristales de efecto) y las sombras (no se emiten). Los triángulos de objetos DINÁMICOS pequeños pegados a las articulaciones
 * del personaje se ignoran (su arma o sus manos no son un obstáculo); el escenario nunca se filtra. */
#include <stddef.h>
#include <stdint.h>
#include "ns_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float center[3];        /* mundo del motor: solo piezas/caras a menos de `radius` de aquí (radius <= 0: todo) */
    float radius;           /* unidades del motor */
    uint32_t skip_key;      /* NsInstance.key a omitir (cuerpo del jugador); 0 = ninguna */
    const float (*joints)[3]; int n_joints; float body_units;   /* articulaciones del personaje (mundo) y radio de «cuerpo»; n_joints = 0: sin filtro */
    int include_blend;      /* 1: incluir también las mallas semitransparentes (herramientas de análisis) */
} NsSolidOpts;

typedef struct {
    uint32_t inst_used, inst_skipped;       /* instancias recorridas / omitidas (jugador) */
    uint32_t pieces, pieces_hidden, pieces_blend, pieces_culled;   /* piezas vistas / ocultas por el juego / semitransparentes / fuera del radio */
    uint32_t pieces_no_mesh;                /* piezas NO ocultas con mesh_id != 0 cuya malla falta en el almacén: geometría ausente (la escena NO debe tratarse como completa) */
    uint32_t tris_static, tris_dyn, tris_body;   /* triángulos emitidos del escenario / dinámicos / descartados por ser cuerpo propio */
    uint32_t overflow;                      /* triángulos que no cupieron en el búfer (la lista está truncada) */
} NsSolidStats;

/* Origen de cada triángulo emitido (opcional, herramientas): instancia (clave), pieza dentro de la instancia, 1 = escenario. */
typedef struct { uint32_t key; uint16_t piece_rel; uint8_t is_static, pad; uint32_t mesh32; } NsSolidSrc;

/* Escribe hasta `cap` triángulos (9 floats cada uno, mundo del motor) en `tris` y devuelve cuántos. `src` puede ser NULL o tener `cap` elementos. */
size_t ns_solid_collect(const NsScene *sc, const NsMeshStore *st, const NsSolidOpts *opt, float *tris, NsSolidSrc *src, size_t cap, NsSolidStats *stats);

#ifdef __cplusplus
}
#endif
