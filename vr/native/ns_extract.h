#pragma once
/* ns_extract: EXTRACTOR de la escena nativa. Función pura: a partir de una instantánea de la RAM del motor (2 MiB, dirección base 0x80000000) rellena una
 * NsScene y un almacén de mallas según el contrato v2 de ns_types.h (protocolo de entrega incluido). Estructuras y desplazamientos: docs/escena_nativa.md.
 * Sin dependencias de plataforma (C11 + stdatomic), sin globales, sin E/S. El productor es un único hilo; el consumidor (otro hilo) sólo llama a
 * ns_store_get / ns_store_get_baked / ns_store_epoch / ns_store_ack.
 *
 * Reglas de la extracción
 *   - SIN culling: no se mira la cámara, ni la distancia, ni el estado de visibilidad del último fotograma. Sólo se descartan (con contador): objetos ocultos
 *     por el juego (flag 0x80), objetos de un grupo de mapa no activo, punteros nulos/fuera de RAM, objetos repetidos en la cola y mallas inconsistentes.
 *   - Orden y capacidades: el escenario (NS_IF_STATIC) se emite PRIMERO y nunca se descarta por capacidad; después las instancias dinámicas por orden de la
 *     cola; lo que no cabe se descarta por el final y se contabiliza (NS_TR_*, n_dropped_*, NS_SV_MAP_INCOMPLETE si ni el escenario cupo). Las tablas de la escena
 *     NO crecen: las reserva quien llama (mínimos NS_MIN_*).
 *   - Lecturas de RAM acotadas (la RAM puede ser hostil): todo puntero, tamaño, índice y cadena se valida; los fallos se cuentan en NsExtractStats.
 *   - Colores: el escenario entrega su color precalculado en NsBaked (ns_store_get_baked), versionado: se relee en cada llamada y, si cambió, se publica una
 *     versión nueva. Los objetos dinámicos NO usan los paquetes de la GPU (fuera de cámara llevan colores viejos): se entregan normales + NsLight de la
 *     instancia (matrices del objeto leídas de RAM) y el consumidor sombrea.
 *   - Mallas: id = ns_mesh_content_hash (contenido, sin dirección ni color precalculado). Una memoria por (dirección del modelo, dirección de la definición)
 *     evita recodificar; en cada acierto se valida una firma barata (cabecera del modelo, muestra de tablas, entradas de textura y cabecera del padre) y se
 *     recodifica si no coincide. El cambio de zona se detecta solo (cambio de firma de modelos del escenario o sustitución total de sus definiciones): sube
 *     zone_gen, se vacía la memoria y se intenta abrir una época nueva del almacén (si el consumidor aún no reconoció la anterior, se sigue en la actual).
 *   - Sombras: las instancias reconocidas como sombra (lista NsExtractOpts.shadow_objs del módulo de efectos, o la heurística opcional detect_shadows) no se
 *     emiten; se cuentan en NsScene.n_shadow_omitted.
 *   - Topes: el almacén tiene un límite de bytes (ns_store_set_limit). Si se alcanza, antes de rechazar se libera la época anterior si ya está reconocida;
 *     si aun así no cabe, la instancia dinámica se descarta y se marca NS_SV_STORE_FULL (el escenario nunca se descarta: sus piezas sin malla quedan ocultas y se marca
 *     NS_SV_MAP_INCOMPLETE).
 */
#include "ns_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NS_RAM_SIZE 0x200000u
#define NS_STORE_DEFAULT_LIMIT (256u * 1024u * 1024u)

typedef struct NsExtractStats {
    uint32_t queue_n;              /* entradas leídas de la cola del canal principal */
    uint32_t q_clamped;            /* la cuenta de objetos estaba fuera de 0..256 y se recortó */
    uint32_t drop_null, drop_badptr, drop_dup;           /* puntero nulo / fuera de RAM / objeto repetido (bucle) en la cola */
    uint32_t drop_hidden_flag, drop_group;               /* flag de oculto / grupo de mapa no activo */
    uint32_t drop_badobj;          /* cabecera inconsistente (nº de piezas absurdo, piezas fuera de RAM) */
    uint32_t shadow_omitted;       /* sombras no emitidas */
    uint32_t n_inst, n_static, n_pieces, n_hidden_pieces, n_ext_pieces;
    uint32_t null_model;           /* piezas con puntero de modelo nulo (ocultas) */
    uint32_t ext_bad;              /* extensiones rotas: fuera del objeto, < n_models, con bucle o demasiado profundas */
    uint32_t mesh_new, mesh_ref;   /* mallas añadidas al almacén / reutilizadas (memoria o ya presentes) */
    uint32_t memo_stale;           /* aciertos de la memoria cuya firma no coincidía: recodificados */
    uint32_t memo_full;            /* fallos al guardar en la memoria */
    uint32_t drop_bad_model;       /* pieza con modelo fuera de RAM, tablas fuera de RAM o tamaños absurdos */
    uint32_t faces_seen;           /* caras leídas en las decodificaciones de esta llamada */
    uint32_t faces_bad_vindex;     /* cara con índice de vértice >= n_verts: se guarda degenerada (v = 0) para conservar la alineación con el color */
    uint32_t faces_bad_nindex;     /* índice de normal fuera de tabla (sustituido por 0) */
    uint32_t faces_tri, faces_quad, faces_degenerate;    /* triángulos (C==D), cuadriláteros, con vértices repetidos distintos de C==D */
    uint32_t faces_no_tex;         /* texid 0 o no encontrada en la tabla (cara plana) */
    uint32_t welded_verts, weld_bad;                     /* soldaduras resueltas / sin resolver */
    uint32_t baked_new, baked_ref; /* versiones de color precalculado nuevas / reutilizadas */
    uint32_t bias_mismatch;        /* piezas cuyo sesgo de profundidad guardado no coincide con el derivado del flag del modelo (diagnóstico) */
    uint32_t zone_changes;         /* cambios de zona detectados en esta llamada */
    uint32_t epoch_opened;         /* 1 si se abrió una época nueva del almacén */
    uint32_t compactions;          /* 1 si la época se abrió por presión del almacén (sin cambio de zona) */
    uint32_t store_full;           /* mallas/colores rechazados por el tope del almacén */
    uint32_t n_lights;
    uint32_t oob_reads;            /* lecturas de RAM rechazadas por estar fuera de rango */
} NsExtractStats;

/* Contadores acumulados del extractor del enganche (ns_extract_glue.c) para el registro de diagnóstico; los acumulados solo crecen (la interfaz hace diferencias por segundo). */
typedef struct NsExtractGlueStats {
    uint32_t calls, mesh_new, memo_stale, zone_changes, epoch_opened, store_full, drop_group, drop_hidden, drop_other, queue_sum;   /* acumulados */
    uint32_t last_queue, last_inst, last_static, last_pieces, last_hidden_pieces, last_drop_group, last_drop_hidden, last_drop_other;   /* última escena */
} NsExtractGlueStats;
void ns_extract_glue_stats(NsExtractGlueStats *out);

typedef struct {
    const uint32_t *shadow_objs;   /* direcciones de objetos que son sombras (NsFxScene.shadow_objs): no se emiten */
    uint32_t n_shadow;
    int detect_shadows;            /* 1: además, heurística propia (objeto de una pieza con una cara plana translúcida iluminada) */
    uint32_t max_pieces;           /* tope global de piezas por llamada (0 = la capacidad de la escena) */
} NsExtractOpts;

/* ---- almacén de mallas (implementa el protocolo de ns_types.h «Entrega»: dos épocas vivas, ack del consumidor) ---- */
NsMeshStore *ns_store_create(void);
void ns_store_destroy(NsMeshStore *st);
void ns_store_set_limit(NsMeshStore *st, size_t bytes);   /* tope de bytes (ambas épocas); def. NS_STORE_DEFAULT_LIMIT */
/* Productor: intenta abrir la época siguiente (cambio de zona). Sólo si la anterior (e-1) ya se liberó, es decir, el consumidor reconoció la actual. Devuelve 1 si
 * se abrió; 0 si se sigue en la actual. */
int ns_store_new_epoch(NsMeshStore *st);
uint32_t ns_store_count(const NsMeshStore *st);           /* mallas vivas (ambas épocas) */
size_t ns_store_bytes(const NsMeshStore *st);
void ns_store_set_compact(NsMeshStore *st, size_t bytes);   /* umbral de bytes a partir del cual se abre época para descartar mallas obsoletas (0 = automático) */
uint32_t ns_store_zone_gen(const NsMeshStore *st);
void ns_store_reset(NsMeshStore *st);   /* cambio de ejecutable: vacía la memoria de direcciones, sube zone_gen y abre época si se puede (productor) */
int ns_store_is_stub(void);             /* siempre 0: sirve a la prueba de enlace (si el almacén fuese un sustituto débil devolvería 1) */
/* Productor: añade a la época actual una malla que NO viene de la RAM del motor (las manos propias, ns_hand). Copia los datos. Si ya está en la época actual no hace nada
 * (hay que volver a ofrecerla cada fotograma: tras abrirse una época nueva la anterior deja de contar). 1 = añadida, 0 = ya estaba, -1 = no cabe o malla inválida. */
int ns_store_put(NsMeshStore *st, const NsMesh *m);

/* ---- escena ---- */
/* Reserva las tablas con al menos NS_MIN_* de capacidad (se sube al mínimo). 0 = bien. */
int ns_scene_alloc(NsScene *sc, uint32_t cap_inst, uint32_t cap_piece, uint32_t cap_light, uint32_t cap_effect);
/* Igual pero con las capacidades EXACTAS (sólo pruebas de truncado). */
int ns_scene_alloc_exact(NsScene *sc, uint32_t cap_inst, uint32_t cap_piece, uint32_t cap_light, uint32_t cap_effect);
void ns_scene_free(NsScene *sc);

/* Extrae la escena del canal principal. `ram` de ram_size bytes (hasta NS_RAM_SIZE; lo que quede fuera cuenta como puntero malo). `scratch` (memoria rápida de
 * 1 KiB) es opcional y de momento no hace falta para la escena. La escena y el almacén deben estar inicializados; la escena se reinicia en cada llamada (conserva
 * capacidades, `seq` y `slot_gen`). Devuelve 0 si la escena se leyó, -1 si el canal principal no es legible (escena vacía, valid = 0). stats/opt pueden ser NULL. */
int ns_extract_ex(NsScene *sc, NsMeshStore *st, const uint8_t *ram, size_t ram_size, const uint8_t *scratch, size_t scratch_size, const NsExtractOpts *opt, NsExtractStats *stats);
int ns_extract(NsScene *sc, NsMeshStore *st, const uint8_t *ram, size_t ram_size, const uint8_t *scratch, size_t scratch_size, NsExtractStats *stats);

#ifdef __cplusplus
}
#endif
