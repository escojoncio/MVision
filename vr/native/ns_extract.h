#pragma once
/* ns_extract: EXTRACTOR de la escena nativa. Función pura: a partir de una instantánea de la RAM del motor (2 MiB, dirección base
 * 0x80000000) rellena una NsScene y un almacén de mallas según el contrato de ns_types.h. Estructuras y desplazamientos: docs/escena_nativa.md
 * (§3 objetos, §5 texturas, §6 luces, §10-§11 algoritmo). Sin dependencias de plataforma (C99 + stdatomic), sin globales, sin E/S.
 *
 * Reglas de la extracción
 *   - SIN culling: no se mira la cámara, ni la distancia, ni el estado de visibilidad del último fotograma. Sólo se descartan (con contador): objetos ocultos por el juego (flag 0x80), objetos de un
 *     grupo de mapa no activo (group_id != 0 && !(group_id & grupo_actual)), punteros nulos/fuera de RAM, objetos repetidos en la cola y mallas
 *     inconsistentes. Las piezas sin caras se emiten ocultas (NS_PF_HIDDEN, malla 0) para conservar los índices de pieza que usan los padres.
 *   - Toda lectura de RAM está acotada (la RAM puede estar corrupta o ser hostil): punteros fuera de rango, tamaños absurdos, índices fuera de tabla,
 *     bucles en la cola/cadenas de extensión y capacidades agotadas se contabilizan en NsExtractStats y nunca provocan accesos fuera de límites.
 *   - Colores: el mapa (flag PAINT) entrega su array de colores precalculados en NsMesh.paint (se lee, no se recalcula). Los objetos dinámicos NO usan los
 *     paquetes (fuera de cámara llevan colores viejos): se entregan normales + NsLight de la instancia (matrices del objeto leídas de RAM) y el consumidor
 *     sombrea (ns_light_color / ns_shade_normal; ver ns_adapt.c).
 *   - Capacidad: las tablas de la escena CRECEN (realloc) en vez de truncar; nunca se pierde mapa por falta de capacidad (truncated sólo si falla la memoria).
 *     Por eso los punteros sc->inst/piece/lights pueden cambiar entre llamadas: leer siempre tras ns_extract.
 *   - Hash de malla: se calcula una vez por (dirección del modelo, generación del almacén). Al cambiar de zona hay que llamar a ns_store_clear. El color precalculado
 *     no entra en el hash ni en el id (dos instancias con la misma definición comparten malla y color; en el mapa cada pieza tiene su definición).
 *   - id de malla = (dirección de la definición << 32) | hash32 del contenido (contrato).
 */
#include "ns_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NS_RAM_SIZE 0x200000u

typedef struct NsExtractStats {
    uint32_t queue_n;              /* entradas leídas de la cola del canal principal */
    uint32_t q_clamped;            /* la cuenta de objetos estaba fuera de 0..256 y se recortó */
    uint32_t drop_null, drop_badptr, drop_dup;           /* puntero nulo / fuera de RAM / objeto repetido (bucle) en la cola */
    uint32_t drop_skipped;         /* objetos omitidos a petición (NsExtractOpts.skip_objs) */
    uint32_t drop_invisible, drop_group;                 /* flag INVISIBLE / grupo de mapa no activo */
    uint32_t drop_badobj;          /* cabecera inconsistente (n_models absurdo, piezas fuera de RAM) */
    uint32_t n_inst, n_pieces, n_hidden_pieces, n_ext_pieces;
    uint32_t ext_bad;              /* cadenas de extensión rotas, fuera del objeto o con bucle */
    uint32_t mesh_new, mesh_ref;   /* mallas añadidas al almacén / ya existentes */
    uint32_t drop_bad_model;       /* pieza con modelo fuera de RAM, tablas fuera de RAM o tamaños absurdos */
    uint32_t faces_seen, faces_kept;                     /* caras leídas / caras guardadas (suma sobre mallas únicas decodificadas) */
    uint32_t faces_bad_vindex;     /* cara descartada: índice de vértice >= n_verts */
    uint32_t faces_bad_nindex;     /* índice de normal fuera de tabla (se sustituye por 0) */
    uint32_t faces_tri, faces_quad, faces_degenerate;    /* triángulos (C==D), cuadriláteros, con vértices repetidos distintos de C==D */
    uint32_t faces_no_tex;         /* texid 0 o no encontrada en la tabla (cara plana) */
    uint32_t faces_paint;          /* caras con color precalculado */
    uint32_t welded_verts;         /* vértices soldados resueltos / sin resolver (weld_bad) */
    uint32_t weld_bad;
    uint32_t n_lights;
    uint32_t oob_reads;            /* lecturas de RAM rechazadas por estar fuera de rango */
    uint32_t truncated;            /* = scene->truncated */
} NsExtractStats;

/* ---- almacén de mallas (implementa ns_store_get/ns_store_epoch de ns_types.h) ---- */
NsMeshStore *ns_store_create(void);
void ns_store_destroy(NsMeshStore *st);
void ns_store_clear(NsMeshStore *st);                 /* vacía el almacén y sube la época */
uint32_t ns_store_count(const NsMeshStore *st);       /* mallas almacenadas */

/* ---- escena ---- */
int ns_scene_alloc(NsScene *sc, uint32_t cap_inst, uint32_t cap_piece, uint32_t cap_light, uint32_t cap_effect);   /* 0 = bien */
void ns_scene_free(NsScene *sc);
#define NS_DEF_CAP_INST 512u
#define NS_DEF_CAP_PIECE 8192u
#define NS_DEF_CAP_LIGHT 512u
#define NS_DEF_CAP_EFFECT 0u

/* Extrae la escena del canal principal. `ram` de ram_size bytes (2 MiB; se rechaza otro tamaño mayor que NS_RAM_SIZE... un volcado más corto se
 * trata como RAM más pequeña: las direcciones fuera de ella cuentan como punteros malos). `scratch` (memoria rápida de 1 KiB) es opcional y de
 * momento no hace falta para la escena (se admite para que la firma no cambie cuando se usen los restos de cálculo). La escena y el almacén deben
 * estar inicializados (ns_scene_alloc, ns_store_create); la escena se reinicia en cada llamada (conserva capacidades y el contador `seq`, que sube).
 * Devuelve 0 si la escena se leyó, -1 si el canal principal no es legible (la escena queda vacía y valid = 0). stats puede ser NULL. */
typedef struct {
    const uint32_t *skip_objs;   /* direcciones de objetos del motor que NO se extraen (los pinta otro módulo: sombras de personaje de ns_fx) */
    uint32_t n_skip;
} NsExtractOpts;
int ns_extract_ex(NsScene *sc, NsMeshStore *st, const uint8_t *ram, size_t ram_size, const uint8_t *scratch, size_t scratch_size, const NsExtractOpts *opt, NsExtractStats *stats);
int ns_extract(NsScene *sc, NsMeshStore *st, const uint8_t *ram, size_t ram_size, const uint8_t *scratch, size_t scratch_size, NsExtractStats *stats);

#ifdef __cplusplus
}
#endif
