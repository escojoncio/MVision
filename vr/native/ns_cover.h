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
 *   POSE       la misma textura+UV existe en un objeto DINÁMICO del nativo pero en OTRO sitio: pieza/animación/IK distinta (la lista manda). Nunca con dueño estático (el escenario no posa): eso lo
 *              mide el verificador geométrico ([verif], más abajo), que además recupera la cámara con la que se emitió la lista
 *   OTHERTEX   hay superficie nativa en esa posición pero con otra textura/CLUT (capa distinta, material mal leído)
 *   PARTIAL    alguna esquina sobre superficie nativa de la misma textura y otras no (polígono recortado por pantalla/plano cercano, o cobertura parcial)
 *   ABSENT     nada parecido en el nativo: geometría ausente del extractor
 * La cobertura = (MATCH + SPLIT + SUBDIV) / triángulos 3D válidos de la lista: lo que el nativo representa (aunque teselado distinto). */
#include <stddef.h>
#include <stdint.h>
#include "gpu.h"
#include "ns_gpu.h"
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
    float ys;                  /* factor vertical de la vista de la lista (66/64 con la cámara del propio juego, 1 con la de la capa VR); 0 = medirlo de la RAM (ns_extract_screen_ys) */
    const float *prev_A, *prev_t;   /* cámara mundo -> vista (9 + 3) de la RAM del fotograma ANTERIOR (la de la lista cuando no hay registro de cámara: cinemáticas), o NULL: candidata del verificador */
    uint32_t max_detail;       /* máximo de triángulos sin pareja que se clasifican a fondo (def. 4096: con menos, lo no clasificado contaba como AUSENTE y la cobertura salía subestimada); el resto cuenta como ABSENT sin dueño */
} NsCoverCfg;
void ns_cover_cfg_default(NsCoverCfg *c);

/* Huecos del MUNDO: rayos desde un punto (en el espacio de mundo de las NsRTri, Y arriba) repartidos como un panorama equirectangular (NC_HOLE_AZ x NC_HOLE_EL, uniforme en
 * azimut y elevación, igual que el panorama de nshito1) contra TODOS los triángulos nativos, por las dos caras (el visor dibuja los reversos). Un rayo sin ningún impacto = dirección
 * en la que el visor no tiene nada que dibujar (techo o cielo que el juego no modela, o geometría ausente). Independiente de la cámara del juego y de lo que dibujó la lista. */
#define NC_HOLE_AZ 48
#define NC_HOLE_EL 24
typedef struct {
    uint32_t rays, miss;       /* rayos lanzados / sin impacto (0 rayos = no medido) */
    uint32_t band[12];         /* sin impacto por franja de elevación de 15 grados, de arriba (zenit) a abajo; cada franja tiene rays / 12 rayos */
    uint32_t miss_up, miss_dn; /* sin impacto por encima / por debajo del horizonte */
    uint32_t sky;              /* rayos sin geometría que caen en el fondo de cielo (incluidos en miss) */
    float eye[3];
} NsCoverHoles;
/* Los ÚLTIMOS `nsky` triángulos son el fondo de cielo (ns_fx): no son geometría del escenario y no tapan huecos; solo se cuenta en `sky` cuántos rayos sin geometría caen en él. */
void ns_cover_holes(const NsRTri *tris, size_t ntris, size_t nsky, const float eye[3], NsCoverHoles *out);

/* ---------------------------------------------------------------------------------------------------------------- verificador geométrico [verif]
 * Cada triángulo que el motor sometió a la GTE (GpuOp con posición de vista en todos sus vértices) se lleva al MUNDO con la cámara con la que se emitió la lista y debe tener superficie nativa
 * (mallas + efectos) a distancia GEOMÉTRICA <= tolerancia en sus tres esquinas y su centro. No se mira la textura. El dueño de cada triángulo sale de la dirección de su paquete
 * (GpuOp.pkt_addr) contra las colas reales del canal: piezas de objetos (estático / dinámico / de un grupo de mapa no activo) y primitivas.
 *   - Cámara: la lista es de un fotograma ANTES que la RAM y en las cinemáticas la cámara cambia entre fotogramas (cortes): se puntúan las cámaras candidatas (lista, RAM) con los vértices de
 *     dueño estático y, si ninguna explica la lista, se AJUSTA una rígida (hipótesis por triángulo congruente de la propia pieza dueña + consenso + refinado). `cam` dice cuál se usó.
 *   - Dueño estático: no hay categoría «pose»: o hay superficie nativa ahí o cuenta como sin equivalente.
 *   - Dueño dinámico o primitiva que no casa en posición absoluta (animación de un fotograma a otro): vale si la pieza dueña existe en el nativo y tiene un triángulo CONGRUENTE (mismas aristas;
 *     una pieza soldada se deforma y no lo tiene: vale si su superficie nativa está a menos de NV_MOVE_TOL de las tres esquinas) o, en primitivas, un elemento nativo de ese mismo paquete; se cuenta en `moved`. Si no, sin equivalente.
 *   - SORTONLY: polígonos de la lista sin posición de vista cuyo paquete es de una primitiva de solo ordenación de la cola (cielo): sin equivalente salvo que el nativo genere esa primitiva. */
enum { NV_STATIC = 0, NV_DYNAMIC, NV_PRIM, NV_SORTONLY, NV_OFFGROUP, NV_OTHER, NV_N };
extern const char *const ns_verif_cls_name[NV_N];
typedef struct { uint32_t lo, hi; uint32_t key; uint16_t piece, psize; uint8_t cls, screen; } NsVerifOwner;   /* [lo, hi): direcciones físicas de los paquetes; key = objeto o primitiva; piece = pieza dentro de la instancia nativa (objetos); screen = 1: primitiva pegada a la cámara (coordenadas de pantalla) */
typedef struct { float p[3][3]; uint32_t key; uint16_t idx; uint16_t nogeo; } NsVerifFxTri;            /* triángulo de un efecto nativo en el mundo; nogeo = 1: solo cuenta como «generado» (cielo anclado al ojo), no como superficie */
typedef struct { float A[9], t[3]; } NsVerifCam;                                                       /* mundo -> vista SIN el factor vertical: e = A p + t */
typedef struct {
    int valid;
    uint32_t tris_gte;         /* triángulos GTE medidos (con muestreo: los examinados) */
    uint32_t missing;          /* sin equivalente = suma de cls[] */
    uint32_t cls[NV_N];        /* por dueño: estático, dinámico, primitiva, solo-ordenación, fuera de grupo, sin dueño en las colas */
    uint32_t moved;            /* dueño dinámico/primitiva presente en el nativo pero en otra pose que la de la lista (un fotograma de diferencia): cubiertos */
    uint32_t ambig;            /* triángulos con algún vértice de vista ambiguo o derivado: no se miden */
    uint32_t no_view_2d;       /* polígonos sin posición de vista y sin dueño en las colas (HUD, menús) */
    uint32_t no_owner;         /* triángulos GTE cuyo paquete no pertenece a ninguna cola del canal (subdivisión, otros renderers); con equivalente o sin él */
    uint32_t world_in_2d;      /* operaciones de la lista (polígonos, líneas, sprites) cuyo paquete es de una primitiva de MUNDO de la cola (ni solo-ordenación ni pegada a la cámara) y que el nativo no
                                * garantiza (ns_fx_covers_packet, el criterio del anfitrión): siguen saliendo por la vía 2D (panel). Lo rellena ns_cover_run; debe ser 0 */
    uint32_t step;             /* muestreo: 1 de cada `step` */
    int cam;                   /* cámara usada: índice de la candidata, o -1 = ajustada */
    float cam_in;              /* fracción de vértices de dueño estático que la cámara usada explica (0 si no había dueños estáticos) */
    float cam_A[9], cam_t[3];  /* la cámara usada (mundo -> vista sin el factor vertical) */
    uint32_t cam_pts;
    uint32_t worst_key, worst_n; uint8_t worst_cls;
    /* Honradez de la cámara. La ÚLTIMA candidata es la de la app (NsScene.cam: la de la RAM, con la que el visor dibuja); las anteriores, cámaras conocidas de la lista (su registro, la de la RAM del
     * fotograma anterior). Una cámara AJUSTADA que dista de TODAS las candidatas más de NV_CAM_DIST_TOL / NV_CAM_ROT_TOL es «DISTINTA»: no la respalda ninguna cámara conocida y puede estar
     * absorbiendo un nativo mal colocado, así que lo que solo casa con ella no se da por equivalente ni por ausente: cuenta en `unverified` (y en `only_fit`). cam_dist / cam_giro / app_in se dan
     * siempre frente a la de la app. El 0 de `missing` solo vale con unverified = 0 y no_view_static = 0 (`ok=si` en la línea). */
    int cam_distinct;
    float cam_dist, cam_giro;  /* entre la cámara usada y la de la app: distancia entre ojos (unidades del motor) y giro (grados) */
    float app_in;              /* fracción de las muestras de dueño estático que explica la cámara de la app SIN ajustar */
    uint32_t only_fit;         /* triángulos que solo casan con la cámara ajustada DISTINTA (incluidos en unverified) */
    uint32_t no_view_static;   /* triángulos de polígonos de dueño ESTÁTICO sin posición de vista: no se pudieron medir */
    uint32_t unverified;       /* ni equivalentes ni ausentes: sin dueño en las colas con una cámara que no explica la lista (cam_in < 0,5 o sin muestras), o que solo casan con la ajustada DISTINTA */
    double ms;
} NsVerif;
#define NV_CAM_DIST_TOL 60.0f    /* unidades del motor */
#define NV_CAM_ROT_TOL 0.5f      /* grados */
#define NV_MOVE_TOL 600.0f       /* una pieza soldada «en otra pose» debe estar a menos de esto (unidades del motor) de donde la dibuja la lista */
/* tris/src: mallas nativas en el mundo (ns_adapt_scene con src_out); fx: triángulos de efectos; own: dueños ORDENADOS por lo; cams: candidatas en orden de preferencia; ys: factor vertical de la
 * vista de la lista; max_tris: tope de triángulos medidos (0 = 6000; por encima se muestrea). 0 = bien. */
int ns_verif_core(const NsRTri *tris, const NsAdaptSrc *src, size_t ntris, const NsVerifFxTri *fx, size_t nfx, const NsVerifOwner *own, size_t nown,
                  const NsVerifCam *cams, int ncam, float ys, const GpuOp *ops, uint32_t nops, uint32_t max_tris, NsVerif *out);
/* Dueños a partir de la RAM: piezas (y extensiones) de los objetos de chanl->queue[0..objs_index) y primitivas de [prim_index..queue_size). Devuelve cuántos (<= cap), ordenados. */
size_t ns_verif_owners(const uint8_t *ram, NsVerifOwner *out, size_t cap);
size_t ns_verif_format(const NsVerif *v, uint32_t frame, char *buf, size_t cap);

/* ---------------------------------------------------------------------------------------------------------------- descartes del modo ENGINE [descartes]
 * Cuántos triángulos de la escena nativa descarta ENTEROS el sombreador con la cámara del motor (pantalla 3D), con la misma regla (ns_gpu_engine_tri) y sin depender de la GPU:
 *   published = triángulos de todas las piezas con malla de la escena; sent = los que se envían a dibujar (piezas visibles, ns_adapt_scene); behind / size / back = descartados por cada regla;
 *   extra = descartados por el motor pero dibujados en el campo extra de la pantalla 3D por algún ojo (extra_one = de ellos, los de reverso que solo rescata uno de los ojos:
 *   cada pase decide con su ojo, como ns_vertex); split = caras de objetos sin GBOUND que el motor original parte, dibujadas con su geometría real («Caras
 *   cercanas: Original»); drawn = sent - descartados (drawn_static del escenario + drawn_dyn del resto; incluye extra y split).
 * `eyes` = los `neyes` ojos de la vista en el mundo del motor (ns_discards_count: el del motor -+ ns_gpu_engine_sep() / 2). `off` = reglas apagadas (NSG_ENG_OFF_*). */
typedef struct { uint32_t published, sent, behind, size, back, extra, split, drawn, drawn_static, drawn_dyn, extra_one; } NsDiscards;
void ns_discards_core(const NsRTri *tris, const NsAdaptSrc *src, size_t ntris, const NsGpuEngCam *cam, const float (*eyes)[3], int neyes, uint32_t off, NsDiscards *out);
void ns_discards_count(const NsScene *sc, const NsMeshStore *st, uint32_t off, NsDiscards *out);
/* Lo que el nativo presenta en la VENTANA del motor (+-160 x +-112 px, celdas de 2 px, desde el ojo del motor y con su regla) frente a lo que el motor dibujó de verdad (polígonos 3D de la lista,
 * sin los que la tarjeta pierde por tamaño): cells = celdas de la ventana; cover («tapa») = las dos cubren y el nativo queda a menos de 1/1,3 de la profundidad de la lista; extra («de más») = solo
 * el nativo; missing («falta») = solo la lista. Las caras PARTIDAS (NSG_ENG_SPLIT) no entran en esa comparación: la lista es la del juego con el parche sin-subdivisión, que no las tiene, y aquí
 * se dibujan a propósito; split = celdas donde una cara partida es lo más cercano del nativo. `cam` debe ser la cámara con la que se emitió la lista. */
typedef struct { uint32_t cells, cover, extra, missing, split; } NsWinCmp;
void ns_wincmp(const NsRTri *tris, const NsAdaptSrc *src, size_t ntris, const NsGpuEngCam *cam, uint32_t off, const GpuOp *ops, uint32_t nops, NsWinCmp *out);
/* Piezas de ns_wincmp (herramientas): máscaras de 1/z de NS_WIN_W x NS_WIN_H celdas, a cero antes de llenarlas. ns_winmask_tri: un triángulo ya en pantalla (x px, y px, 1/z);
 * ns_winmask_native: lo que dibuja la regla (iz) y, aparte, las caras partidas recortadas por z >= NS_WIN_NEAR (iz_split, o NULL); ns_winmask_list: la lista; ns_winmask_cmp: a frente a b. */
#define NS_WIN_W 160
#define NS_WIN_H 112
#define NS_WIN_NEAR 50.0f   /* plano cercano de la pantalla 3D (0,05 m) en unidades del motor */
void ns_winmask_tri(float *iz, const float s[3][3]);
void ns_winmask_native(const NsRTri *tris, const NsAdaptSrc *src, size_t ntris, const NsGpuEngCam *cam, uint32_t off, float *iz, float *iz_split);
void ns_winmask_list(const GpuOp *ops, uint32_t nops, float h, float *iz);
void ns_winmask_cmp(const float *a, const float *b, NsWinCmp *out);
/* `[descartes] caras=original|recortadas publicados= enviados= detras= tamano= reverso= extra= (un_ojo=) partidas= dibujados= (escenario= dinamicos=) salto=N% | lista: tapa=N% de_mas=N% falta=N% partidas=N%`;
 * jump_pct = mayor cambio de descartes entre dos escenas consecutivas del último segundo; win = última comparación con la lista (NULL o cells = 0: `lista: sin_medida`); off = reglas apagadas. */
size_t ns_discards_format(const NsDiscards *d, float jump_pct, const NsWinCmp *win, uint32_t off, char *buf, size_t cap);
/* Lo mismo para el anfitrión (vrhost.c no incluye esta cabecera y tiene definiciones débiles vacías): los NS_DISCARDS_WORDS contadores en el orden de NsDiscards, con las reglas apagadas vigentes
 * (ns_gpu_engine_off) y los ojos reales de la vista; win = los NS_WINCMP_WORDS de NsWinCmp de la última medida del trabajador de cobertura (ns_cover_glue_win). */
#define NS_DISCARDS_WORDS 11
#define NS_WINCMP_WORDS 5
void ns_discards_scene(const NsScene *sc, const NsMeshStore *st, uint32_t out[NS_DISCARDS_WORDS]);
size_t ns_discards_line(const uint32_t d[NS_DISCARDS_WORDS], float jump_pct, const uint32_t win[NS_WINCMP_WORDS], char *buf, size_t cap);
int ns_cover_glue_win(uint32_t win[NS_WINCMP_WORDS], uint32_t *frame);   /* ns_cover_glue.c: 1 si hay medida; frame = fotograma de su lista (el de ns_cover_glue_submit) */

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
    float ys;                                          /* factor vertical de la vista de la lista que se usó (NsCoverCfg.ys o medido de la RAM; 1 si no se pudo medir) */
    NsVerif verif;                                     /* verificador geométrico (verif.valid = 0 si no se midió) */
    NsWinCmp win;                                      /* nativo frente a la lista en la ventana del motor (ns_wincmp); win.cells = 0 si no se midió */
    NsCoverHoles holes;                                /* huecos del mundo nativo desde el ojo del juego (ns_cover_holes); holes.rays = 0 si no se midió */
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
