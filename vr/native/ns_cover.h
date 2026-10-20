#pragma once
/* ns_cover: COBERTURA de la lista de la GPU por la escena nativa. La lista de dibujo de un fotograma (GpuDrawList) es lo que el motor dibujó de verdad; cada
 * polígono 3D de ella debe existir en la escena nativa extraída de la misma RAM. Este módulo empareja, en el espacio de vista del juego, cada triángulo 3D de la
 * lista con los triángulos de la escena nativa (esquina a esquina, con tolerancia que crece con la profundidad; después textura/CLUT/UV), clasifica por causa los que
 * NO tienen pareja y hace la comprobación inversa (triángulos nativos que el motor no dibujó y por qué: detrás de la cámara, fuera de pantalla, reverso, lejos).
 *
 * El MISMO código lo usan la herramienta de escritorio (tools/nscover.c, sobre un volcado) y la app (ns_cover_glue.c: cada N fotogramas, en un hilo aparte, con
 * una instantánea de la RAM y de la lista); las pruebas (test_ns_cover.c) lo ejercitan con escenas sintéticas. Sin dependencias de plataforma (C11, libm).
 *
 * Causas (por triángulo de la lista; cada cuadrilátero son 2 triángulos (v0,v1,v2) y (v1,v2,v3)):
 *   MATCH      esquinas dentro de la tolerancia de las de un triángulo nativo (cualquier orden)
 *   SPLIT      las tres esquinas son vértices de UNA cara nativa pero con otra diagonal (división del cuadrilátero distinta)
 *   SUBDIV     las tres esquinas están sobre la superficie de triángulos nativos de la MISMA textura sin ser vértices: subdivisión del motor para caras cercanas
 *   BILLBOARD  rectángulo a profundidad constante sin superficie nativa debajo: sprite generado por código (efectos), fuera de la estructura de modelos
 *   POSE       la misma textura+UV existe en el nativo pero en OTRO sitio: pieza/animación/IK distinta (la lista manda)
 *   OTHERTEX   hay superficie nativa en esa posición pero con otra textura/CLUT (capa distinta, material mal leído)
 *   PARTIAL    alguna esquina sobre superficie nativa de la misma textura y otras no (polígono recortado por pantalla/plano cercano, o cobertura parcial)
 *   ABSENT     nada parecido en el nativo: geometría ausente del extractor
 * La cobertura = (MATCH + SPLIT + SUBDIV) / triángulos 3D válidos de la lista: lo que el nativo representa (aunque teselado distinto). */
#include <stddef.h>
#include <stdint.h>
#include "gpu.h"
#include "ns_adapt.h"
#include "ns_fx.h"
#include "ns_fxcmp.h"
#include "ns_types.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { NC_MATCH = 0, NC_SPLIT, NC_SUBDIV, NC_BILLBOARD, NC_POSE, NC_OTHERTEX, NC_PARTIAL, NC_ABSENT, NC_N };
enum { NR_BEHIND = 0, NR_NEAR, NR_OFF, NR_BACK, NR_FAR, NR_HIDDEN, NR_BIG, NR_EXTRA, NR_N };   /* nativos sin pareja: detrás / cruzan el plano cercano / fuera de pantalla / reverso / más allá de la niebla / objeto que el motor consideró no visible / polígono mayor de 1023x511 px (el hardware lo descarta; el visor lo dibuja) / visibles (de más) */
extern const char *const ns_cover_cause_name[NC_N];
extern const char *const ns_cover_rev_name[NR_N];

typedef struct {
    float tol_base, tol_z;     /* tolerancia de esquina en unidades de vista: tol_base + tol_z * z (def. 20 y 0,004; tope 200) */
    float surf_tol;            /* margen extra para «sobre la superficie» (def. 80) */
    int screen_w, screen_h;    /* pantalla de referencia (def. 320x256) */
    void *dbg;                 /* FILE* opcional (herramientas): una línea por triángulo sin pareja y por nativo de más */
    const float *list_eye, *list_M;   /* cámara con la que se emitió la lista (ojo en el mundo y vista->mundo fila-mayor); NULL = la de la RAM */
    uint32_t max_detail;       /* máximo de triángulos sin pareja que se clasifican a fondo (def. 4096: con menos, lo no clasificado contaba como AUSENTE y la cobertura salía subestimada); el resto cuenta como ABSENT sin dueño */
} NsCoverCfg;
void ns_cover_cfg_default(NsCoverCfg *c);

#define NC_WORST 6
typedef struct {
    uint8_t cause, semi;
    uint32_t key;              /* NsInstance.key del dueño (nativo más probable) o 0 */
    uint32_t piece;            /* pieza del dueño dentro de su instancia */
    uint32_t mesh;             /* 32 bits bajos del id de malla del dueño */
    uint16_t tpage, clut;      /* de la lista (página sin los bits de mezcla) */
    uint32_t n;                /* triángulos del grupo */
    float zmin;                /* profundidad de vista mínima del grupo */
} NsCoverGroup;

typedef struct {
    uint32_t list_ops, list_polys3d, list_no_view;     /* operaciones de la lista / polígonos con vista válida / polígonos sin posición de vista (no se comparan) */
    uint32_t list_tris, list_degenerate;               /* triángulos 3D comparados / degenerados omitidos */
    uint32_t fx_tris, fx_water, fx_sprite, fx_other;    /* triángulos de la lista cubiertos por ELEMENTOS DE EFECTOS (ns_fx): ondas de agua / carteles y sprites / resto; ya incluidos en list_tris y en cover_pct */
    uint32_t cause[NC_N][2];                           /* por causa: [0] opacos, [1] semitransparentes */
    uint32_t tex_page, tex_clut, tex_uv;               /* entre los MATCH: página/CLUT/UV distintos */
    uint32_t col_n, col_bad;                           /* entre los MATCH con color por vértice comparable: cuántos / con error medio de color > 24 (128 = neutro) */
    uint32_t shift_n, shift_bad;                       /* entre los MATCH: error de posición medido en píxeles (error 3D * H / z); cuántos pasan de 2 px */
    float shift_mean_px;
    float col_mean;                                    /* error medio de color absoluto (niveles 0..255) */
    uint32_t nat_tris, nat_used;                       /* triángulos nativos / emparejados con algo de la lista */
    uint32_t rev[NR_N];                                /* nativos sin pareja por causa */
    float cover_pct, cover_opaque_pct, exact_pct;      /* (MATCH+SPLIT+SUBDIV) %, lo mismo solo opacos, solo MATCH % */
    uint32_t n_worst, n_extra, n_color, n_shift;
    NsCoverGroup worst[NC_WORST];                      /* peores grupos de la lista sin cubrir (por nº de triángulos) */
    NsCoverGroup extra[3];                             /* peores grupos de nativos visibles sin pareja (cause = NC_N) */
    NsCoverGroup shift[3];                             /* peores grupos de MATCH desplazados > 2 px (cause = NC_N + 2) */
    NsCoverGroup color[3];                             /* peores grupos de MATCH con color distinto (cause = NC_N + 1) */
    NsFxCmpResult fxc;                                 /* medida de los efectos nativos frente a la lista (ns_fxcmp.h); fxc_ok = 1 si se midió */
    int fxc_ok;
    double ms;
} NsCoverResult;

/* Cámara de la comparación: mundo -> vista e = A*p + t (A fila-mayor, 9 floats), como las matrices del motor. fog_far > 0: profundidad más allá de la cual el motor no dibuja. */
typedef struct { float A[9], t[3]; float fog_far; } NsCoverCam;

/* Núcleo: triángulos nativos en mundo (salida de ns_adapt_scene con src_out) frente a la lista. 0 = bien. */
int ns_cover_core(const NsRTri *tris, const NsAdaptSrc *src, size_t ntris, const NsCoverCam *cam, const GpuOp *ops, uint32_t nops, const NsCoverCfg *cfg, NsCoverResult *out);

/* Completo: extrae la escena de una instantánea de la RAM del motor (almacén propio), adapta y compara. La cámara sale de la escena extraída. 0 = bien. */
typedef struct NsCover NsCover;
NsCover *ns_cover_create(void);
void ns_cover_destroy(NsCover *c);
int ns_cover_run(NsCover *c, const uint8_t *ram, const uint8_t *scratch, const GpuOp *ops, uint32_t nops, const NsCoverCfg *cfg, NsCoverResult *out);

/* Texto para el registro: línea 1 resumen, después una línea por cada peor grupo (lista sin cubrir y nativos de más). Devuelve los bytes escritos. */
size_t ns_cover_format(const NsCoverResult *r, uint32_t frame, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
