#pragma once
/* ns_fxcmp: MEDIDA de los efectos nativos frente a la lista de la GPU del mismo fotograma.
 *
 * Para cada operación de la lista casada con un elemento de ns_fx (exacta o tolerante) se construye, con la MISMA función que dibuja el renderer
 * (ns_gpu_fx_build: carteles con caja en píxeles a la profundidad de la cámara del juego, cintas, cuadriláteros), la geometría nativa vista desde la cámara con
 * la que se emitió la lista, y se compara con lo que la GPU dibujó:
 *   posición  distancia entre el centro nativo y el de la operación (mm de mundo VR y píxeles de la pantalla del motor);
 *   tamaño    lado equivalente (raíz del área) nativo / lista, en el espacio de vista (polígonos con posición de vista) o en píxeles (rectángulos);
 *   mezcla    semitransparencia, modo de mezcla, textura, página y CLUT del elemento frente a los de la operación (lo que el renderer realmente usará);
 *   rol       papel del elemento (informativo).
 * Además cuenta los elementos que el motor no emitió (ENGINE_CULLED) pero cuyo centro cae DENTRO de la pantalla de la cámara de la lista (no se explican por el
 * frustum: el renderer los dibujaría igualmente) y los elementos fiables que ninguna operación de la lista reclama.
 *
 * Lo usan la app (ns_cover.c, una vez por segundo, en el registro) y la herramienta de escritorio nscover. Sin dependencias de plataforma. */
#include <stddef.h>
#include <stdint.h>
#include "gpu.h"
#include "ns_fx.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { NFC_BAD_POS = 1, NFC_BAD_SIZE = 2, NFC_BAD_BLEND = 4, NFC_BAD_TEX = 8, NFC_BAD_PAGE = 16 };
#define NFC_WORST 5

typedef struct {
    int32_t op, item;
    uint8_t role, shape, near_match, culled, how;    /* how: 0 = vista (polígono con posición de vista), 1 = pantalla (rectángulo / sin vista) */
    uint8_t abr_it, abr_op, semi_it, semi_op, tex_it, tex_op, unk;   /* unk: el elemento no trae página (1) y/o modo de mezcla (2) y el renderer usa un valor supuesto */
    uint16_t tpage_it, tpage_op, clut_it, clut_op;
    uint32_t prim; uint16_t idx;
    uint32_t bad;                                    /* NFC_BAD_* */
    float pos_mm, pos_px, vtx_mm;                    /* vtx_mm: máximo por vértice (solo cuadriláteros/triángulos con la misma numeración) */
    float size_it, size_op, ratio;                   /* how 0: mm; how 1: px (raíz de ancho*alto) */
    float score;
} NsFxCmpRow;

typedef struct {
    uint32_t n_ops_fx;          /* operaciones de la lista casadas con un elemento (FX) */
    uint32_t n_cmp, n_view, n_screen, n_noface, n_nomeas, n_clip;   /* medidas hechas (por vista / por pantalla), omitidas por cara oculta, sin medida posible; n_clip: polígonos que el motor recortó (vértices saturados o a menos de 64 del plano cercano), no comparables */
    uint32_t n_near;            /* de las medidas, casadas por la pasada tolerante (no exacta) */
    float pos_mean_mm, pos_p90_mm, pos_max_mm, pos_mean_px;
    float ratio_mean;           /* media geométrica nativo/lista del tamaño */
    uint32_t pos_bad, size_bad, blend_bad, tex_bad, page_bad;
    uint32_t unk_page, unk_abr; /* medidas con página / modo de mezcla supuestos (el renderer no los conoce) */
    uint32_t n_items, n_items_culled, n_culled_inview;  /* elementos extraídos / no emitidos por el motor / de esos, con el centro dentro de la pantalla de la cámara de la lista */
    uint32_t n_orphan_semi3d, n_orphan_rect_semi;       /* operaciones semitransparentes de la lista (polígono con posición de vista / rectángulo texturizado) que ni son efecto ni malla: sprites del motor sin extraer */
    uint32_t n_items_reliable, n_items_unclaimed;       /* elementos fiables (emitidos, con profundidad) / de ellos, sin ninguna operación de la lista */
    uint32_t by_role[NSFX_ROLE_COUNT];
    uint32_t n_worst;
    NsFxCmpRow worst[NFC_WORST];
    float mm_per_unit;
} NsFxCmpResult;

/* sc: escena extraída (ya con ns_fx_resolve_items aplicado si se quiere medir lo resuelto; sin él se mide lo que el renderer usa hoy).
 * lc: cámara con la que se emitió la lista (NULL = la de la RAM). to_vr: 3x4 mundo del motor -> VR (m) solo para la escala a mm (NULL = 1 mm por unidad).
 * cls/item: salida de ns_fx_classify (+ classify_near); exact (opcional): copia de cls tras la clasificación exacta, para marcar las casadas por la pasada tolerante.
 * Devuelve 0, o <0 si faltan argumentos. */
int ns_fxcmp_run(const NsFxScene *sc, const NsFxListCam *lc, const float *to_vr, const GpuDrawList *dl, const uint8_t *cls, const int32_t *item, const uint8_t *exact, NsFxCmpResult *out);
size_t ns_fxcmp_format(const NsFxCmpResult *r, uint32_t frame, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
