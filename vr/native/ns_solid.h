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

/* Profundidad de vista (unidades del motor) de lo más cercano que la pantalla 3D dibuja DENTRO de la ventana del motor (+-NSG_ENG_WIN_X x +-NSG_ENG_WIN_Y px con su cámara): el escenario (también el de
 * grupos de mapa no activos, que el renderer dibuja) y los objetos dinámicos (piezas visibles, sin sombras), cara a cara y con la regla del modo ENGINE vigente (ns_gpu_engine_tri con ns_gpu_engine_off): de la cara que dibuja la regla
 * del motor, el z mínimo de su polígono (vértices ya ajustados al plano H/2) recortado por la ventana; de la cara partida («Caras cercanas: Original», geometría real), el de su polígono recortado
 * además por el plano cercano de la pantalla 3D (NS_SOLID_SCREEN_NEAR). Lo descartado y lo rescatado (solo fuera de la ventana) no cuentan. 0 si no hay nada o la cámara no es válida. La ventana
 * estéreo de la pantalla 3D (ns_cinema_depth_filter, NsCinemaDepthFilter.z_static) acota con ella el paralaje cruzado. */
#define NS_SOLID_SCREEN_NEAR 50.0f   /* plano cercano de la pantalla 3D (0,05 m de la vista estéreo, a 1000 unidades por metro) */
float ns_solid_view_near(const NsScene *sc, const NsMeshStore *st);

#ifdef __cplusplus
}
#endif
