#pragma once
/* ns_gpu: empaquetado en C de la escena nativa para el renderer Metal (app/Sources/Native). Convierte mallas inmutables (ns_types.h) en búferes planos
 * de vértices y esquinas, construye la tabla de piezas de cada escena (matriz de mundo, luces en espacio local, niebla) y ordena de atrás adelante
 * los triángulos semitransparentes. Sin dependencias de plataforma (C99, libm): se prueba en Linux (test_ns_gpu.c). Layout GPU: ns_gpu_types.h. */
#include <stddef.h>
#include <stdint.h>
#include "ns_types.h"
#include "ns_gpu_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Nº de triángulos de una malla: 1 por cara triangular, 2 por cuadrilátero. */
uint32_t ns_gpu_tri_count(const NsMesh *m);

/* Empaqueta la malla. verts: m->n_verts entradas; corners: 3*ns_gpu_tri_count(m) entradas (índices de vértice ABSOLUTOS = vert_base + local);
 * baked = juego de colores precalculados de la pieza (NsBaked; NULL = ninguno, solo se usa con NS_MF_BAKED y caras NS_FF_BAKED_OK);
 * centroid (3 floats por triángulo, espacio local) y abr (modo de mezcla por triángulo) pueden ser NULL. Índices fuera de rango se acotan (nunca lee fuera). */
void ns_gpu_pack(const NsMesh *m, const NsBaked *baked, NsGpuVert *verts, NsGpuCorner *corners, uint32_t vert_base, float *centroid, uint8_t *abr);

/* ---- Diagnóstico: partir triángulos largos en CPU (se aplica a la malla ya empaquetada, antes de subirla a la GPU) ---- */
typedef struct {
    NsGpuVert *verts; uint32_t nverts;     /* los nv originales (mismos índices) + los añadidos (malloc) */
    NsGpuCorner *corners; uint32_t ntris;  /* 3*ntris esquinas con índices LOCALES de `verts` (malloc) */
    float *centroid;                       /* 3*ntris (NULL si la entrada no traía) */
    uint8_t *abr;                          /* ntris (NULL si la entrada no traía) */
    uint32_t n_split;                      /* triángulos originales subdivididos */
    uint32_t n_skip_weld;                  /* largos pero con algún vértice soldado (su posición real depende del padre): sin partir */
    uint32_t n_skip_cap;                   /* largos que no se partieron del todo por el tope o la profundidad */
} NsGpuSplit;

/* Parte por bisección de la arista más larga los triángulos de la malla empaquetada (verts/corners con índices locales: ns_gpu_pack con vert_base = 0) cuya arista
 * supere max_edge (unidades LOCALES de la malla). Los triángulos con algún vértice soldado no se tocan. Los vértices nuevos no llevan soldadura; normal, color
 * precalculado y texel se interpolan. Tope de seguridad: la malla resultante no pasa de max_out_tris triángulos. centroid y abr pueden ser NULL.
 * Devuelve 0 (resultado en out; liberar con ns_gpu_split_free), 1 (nada que partir: out vacío) o -1 (memoria o parámetros inválidos). */
int ns_gpu_split(const NsGpuVert *verts, uint32_t nv, const NsGpuCorner *corners, uint32_t nt, const float *centroid, const uint8_t *abr,
                 float max_edge, uint32_t max_out_tris, NsGpuSplit *out);
void ns_gpu_split_free(NsGpuSplit *s);
/* Suma `base` al índice de vértice de n esquinas (de local a absoluto en el pool). */
void ns_gpu_rebase_corners(NsGpuCorner *c, uint32_t n, uint32_t base);

/* Recuento de diagnóstico de una malla empaquetada dibujada con la matriz m (16 floats en orden de columnas: local -> espacio del ojo, rígida salvo la escala a metros):
 * triángulos con alguna arista > big_edge (m), triángulos que cruzan el plano cercano (profundidad = -z del ojo) y ambas cosas. Se acumula en acc. */
typedef struct { uint32_t tris, big, cross, big_cross; float max_edge; } NsGpuScan;
void ns_gpu_scan(const NsGpuVert *verts, const NsGpuCorner *corners, uint32_t ntri, const float m[16], float near, float big_edge, NsGpuScan *acc);

/* Pieza de la escena (solo se llama con la malla presente: mesh_flags = NsMesh.flags, depth_bias = NsMesh.depth_bias). parent_global: índice global (en NsScene.piece) del
 * padre o -1 (la malla del padre debe estar presente; parent_nverts = su nº de vértices). vert_base: base de la malla en el pool de vértices. */
void ns_gpu_make_piece(const NsScene *sc, const NsInstance *inst, const NsPiece *p, uint32_t mesh_flags, int depth_bias, uint32_t vert_base,
                       int parent_global, uint32_t parent_nverts, NsGpuPiece *out);

/* ---- Red de seguridad de vértices (réplica en C de la del shader NativeShaders.metal; la usa el renderer para contar y registrar causas, y las pruebas) ---- */
enum { NSG_NET_MAT = 1, NSG_NET_WELD = 2, NSG_NET_FAR = 4, NSG_NET_EDGE = 8 };   /* causas: matriz corrupta / soldadura inconsistente / vértice lejos de la pieza / arista desproporcionada */
typedef struct { uint32_t causes; float max_scale; float max_gap_m; uint32_t n_welded, n_gap; } NsGpuNetPiece;
typedef struct { uint32_t tris, bad_far, bad_edge; float worst_far_m, worst_edge_m; } NsGpuNetTris;
/* Radio de reposo de la malla: máx. |v| local (unidades del motor). */
float ns_gpu_mesh_radius(const NsMesh *m);
/* Posición de mundo del motor del vértice ABSOLUTO vabs de la pieza gi, con la misma semántica que ns_vertex: soldadura al vértice del padre, encadenada hasta NSG_WELD_DEPTH niveles; NSG_PF_NOWELD la ignora. */
/* Réplica en C del descarte por detrás del sombreador (ns_vertex): 1 si el triángulo (p0,p1,p2; orden de ns_gpu_pack) de una malla de una sola cara se ve por detrás desde `eye`
 * (mundo del motor): dot(p0 - eye, (p1-p0)x(p2-p0)) <= 0. Solo con cull_eye.w = 1 (cinemáticas). */
int ns_gpu_tri_back(const float p0[3], const float p1[3], const float p2[3], const float eye[3]);
void ns_gpu_vert_world(const NsGpuVert *pool, const NsGpuPiece *tbl, uint32_t gi, uint32_t vabs, float out[3]);
/* Comprobación de PIEZA (una vez por fotograma, tras rellenar la tabla): matriz corrupta (NSG_NET_MAT: no dibujar la pieza) y soldaduras cuyo vértice del padre se aleja más de NSG_NET_WELD_GAP_M de
 * la posición propia (NSG_NET_WELD: poner NSG_PF_NOWELD). nverts = nº de vértices de la malla de la pieza; mpu = metros VR por unidad del motor. Devuelve las causas. */
uint32_t ns_gpu_net_piece(const NsGpuVert *pool, const NsGpuPiece *tbl, uint32_t gi, uint32_t nverts, float mpu, NsGpuNetPiece *out);
/* Triángulos de la pieza que el shader colapsaría (solo piezas NSG_PF_NET); acumula contadores y peores valores en acc (puede ser NULL). Devuelve cuántos. corners: 3*ntri esquinas con índices absolutos. */
uint32_t ns_gpu_net_tris(const NsGpuVert *pool, const NsGpuCorner *corners, uint32_t ntri, const NsGpuPiece *tbl, uint32_t gi, float mpu, NsGpuNetTris *acc);

/* Factor de niebla f (0..1) a la profundidad z de la cámara del juego (unidades del motor): 0 sin niebla. */
float ns_gpu_fog_f(const NsFog *fog, float z);
/* Profundidad de vista de la cámara del juego de un punto del mundo del motor. */
float ns_gpu_game_z(const NsCamera *cam, const float p[3]);

/* Origen de un grupo de triángulos semitransparentes de una pieza. zrow: profundidad de vista (positiva hacia delante) de un punto local c:
 * depth = zrow[0]*c.x + zrow[1]*c.y + zrow[2]*c.z + zrow[3]. */
typedef struct {
    uint32_t piece;          /* índice de pieza */
    uint32_t tri_base;       /* primer triángulo ABSOLUTO de la malla */
    uint32_t ntri;
    const float *centroid;   /* 3*ntri */
    const uint8_t *abr;      /* ntri */
    float zrow[4];
} NsGpuSemiSrc;

/* Memoria de trabajo de la ordenación (crece y se reutiliza: sin malloc por fotograma). Inicializar a cero; liberar con ns_gpu_sort_free. */
typedef struct { void *a, *b; size_t cap; } NsGpuSortScratch;
void ns_gpu_sort_free(NsGpuSortScratch *sc);

/* Ordena todos los triángulos semitransparentes de atrás adelante (clave = profundidad del centroide; empate = orden de entrada: el mismo criterio del
 * rasterizador de referencia), con radix sort estable sobre la clave de 32 bits. Escribe como mucho cap entradas en out/abr_out y devuelve cuántas; si hay más
 * de cap se descartan los MÁS LEJANOS. scratch puede ser NULL (reserva y libera dentro: solo pruebas). */
size_t ns_gpu_sort_semi(const NsGpuSemiSrc *src, size_t nsrc, NsGpuOrder *out, uint8_t *abr_out, size_t cap, NsGpuSortScratch *scratch);
/* Igual y, si depth_out != NULL, escribe también la profundidad de cada entrada ordenada (para intercalar con los efectos). */
size_t ns_gpu_sort_semi_ex(const NsGpuSemiSrc *src, size_t nsrc, NsGpuOrder *out, uint8_t *abr_out, float *depth_out, size_t cap, NsGpuSortScratch *scratch);


/* ---- Efectos nativos (NsScene.effects): geometría por fotograma ----
 * Convierte los NsEffect (mundo del motor) en triángulos del mundo VR para el renderer: carteles (tamaño en píxeles del motor -> mundo con la profundidad de la
 * cámara del juego) y cintas orientados UNA vez por fotograma a la cabeza; cuadriláteros y triángulos de mundo (diagonal 0,1,2 / 1,2,3); polígonos de una cara con
 * test de cara por ojo (en el mundo del motor, como el motor). Salida en dos tramos del mismo búfer:
 *   [0, n_opaque)                 pase 0 (opaco): todos los no semitransparentes y, de los semitransparentes texturizados, una copia (solo se pintan sus texels con
 *                                 el bit 15 a 0)
 *   [n_opaque, n_opaque + n_semi) pase 1 (semitransparente), de atrás adelante respecto a la cabeza, en runs de igual modo de mezcla (`runs`)
 * `keys` recibe la identidad (ns_fx_item_key) de cada efecto que ha entrado en el dibujo: el renderer solo lo da por CONFIRMADO si dibuja de verdad. */
typedef struct NsGpuFxTrack NsGpuFxTrack;   /* estado del seguimiento entre fotogramas (opaco) */
NsGpuFxTrack *ns_gpu_fx_track_create(void);
void ns_gpu_fx_track_destroy(NsGpuFxTrack *t);
void ns_gpu_fx_track_reset(NsGpuFxTrack *t);   /* olvida todo (cambio de escena, recentrado) */
void ns_gpu_fx_track_request_reset(void);       /* desde cualquier hilo (recentrado): el siguiente ns_gpu_fx_build_ex con seguimiento olvida todo antes de procesar */
typedef struct {
    const float *to_vr;                 /* 12: mundo del motor -> mundo VR (m), 3x4 fila-mayor */
    const NsCamera *cam;                /* cámara del juego de la escena (profundidad de los carteles, sentido del mundo) */
    float head[3];                      /* cabeza (mundo VR, m) */
    float head_right[3], head_up[3];    /* ejes de la cabeza en el mundo VR (unitarios, ortogonales) */
    float eyes[2][3]; int n_eyes;       /* ojos (mundo VR, m): 1 o 2 */
    float px_z_min, px_z_ref;           /* profundidad mínima fiable de un tamaño en píxeles y la de referencia si no (0 = 32 y 1000) */
    const NsFog *fog;                   /* niebla de la escena (NULL = sin niebla). Cada vértice se multiplica por 1 - ns_gpu_fog_f(z de la cámara del juego), como las mallas */
    int has_depth_row;                  /* 1: los semitransparentes se ordenan por la MISMA profundidad que las mallas (depth_row sobre el centro, mundo VR) y no por la distancia a la cabeza */
    float depth_row[4];                 /* profundidad (positiva hacia delante) = depth_row[0..2] . centro + depth_row[3] */
    /* Orden de dibujo del motor. raise_m: metros VR por unidad de `depth_bias` (como las mallas: el efecto se acerca a la cabeza esa distancia; 0 = ignorar). board_pull: fracción de la
     * mitad del tamaño con la que se adelanta un CARTEL hacia la cabeza (escalado desde la cabeza: se ve igual); el motor lo pinta plano en pantalla según su profundidad y un halo
     * de 2 m en una lámpara de techo no se recorta contra el techo; 0 = sin adelantar. */
    float raise_m, board_pull;
    /* Comodidad en VR (ver ns_gpu_fx_build_ex). comfort_deg = X: diámetro angular máximo (grados) de un efecto con el centro a 0,3 m de la cabeza (0 = desactivado: ni reduce ni descarta).
     * Entre comfort_dmin_m y 0,3 m el tope sube desde 0 hasta X (por debajo de comfort_dmin_m, 0 = 0,12 m, el efecto nace "dentro de la cabeza": se descarta) y de 0,3 m a 1 m sube hasta 90 grados;
     * a más de 1 m no hay tope. Un efecto que lo supera se REDUCE alrededor de su centro (nunca se desplaza). Los pegados a la cámara del motor (NS_EF_ON_CAMERA o papel de superficie
     * de vista) no tienen emisor en el mundo: con el centro a menos de oncam_drop_m (0 = no) se descartan (cuentan como confirmados). near_log_m (0 = 0,5 m): radio del registro de cercanos. */
    float comfort_deg, comfort_dmin_m, oncam_drop_m, near_log_m;
    /* Red de seguridad extrema, independiente de comfort_deg: se descarta (confirmado) el efecto con el centro a menos de drop_inside_m de la cabeza (0 = no). Los efectos bien colocados no se tocan. */
    float drop_inside_m;
    /* Seguimiento de lo que va pegado a la cabeza (detector "sigue a la cámara"). track != NULL: cada efecto (clave ns_gpu_fx_key; los rellenos de ns_fill llevan identidad estable) se sigue entre
     * fotogramas y se compara su posición en ESPACIO DE CABEZA con la que tendría si estuviera anclado al mundo del motor. Constante en la cabeza mientras la cabeza se mueve respecto al mundo =>
     * SIGUE (se descarta, confirmado, a cualquier distancia < follow_far_m, def. 30 m; más lejos = cielo / fondo, que va con la cabeza a propósito). Cercano (< hold_near_m, 0 = sin regla) y todavía sin
     * demostrar que está anclado al mundo => retenido (no se dibuja), salvo los de vida corta (< 12 fotogramas) a menos de 15 cm, que se dibujan; ~50 fotogramas quieto en el mundo del motor sin indicio de
     * SIGUE => MUNDO. Cercano que ni sigue ni está quieto en el mundo => VAGA (se descarta). drop_oncam != 0: un NS_EF_ON_CAMERA que no es recubrimiento de pantalla (papel VIEW_OVERLAY) se descarta si
     * además SIGUE o está a menos de 1 m. Todo lo demás va anclado al mundo. */
    NsGpuFxTrack *track;
    float hold_near_m;
    float follow_far_m;   /* distancia (m) a partir de la cual un SIGUE ya no se descarta (cielo, fondos); 0 = 30 */
    int drop_oncam;
} NsGpuFxParams;
typedef struct { uint32_t start, count; uint8_t abr; } NsGpuFxRun;   /* start/count en vértices del búfer completo */
/* Un efecto semitransparente en el búfer (ya ordenado de atrás adelante): para intercalarlo con los triángulos semitransparentes de las mallas (ns_gpu_semi_merge). */
typedef struct { float depth; uint32_t start, count; uint8_t abr; } NsGpuFxSemiItem;
/* Registro de efectos cercanos a la cabeza (por fotograma, los de mayor tamaño angular). accion: 0 intacto, 1 reducido, 2 descartado por nacer dentro de la cabeza, 3 descartado por ir pegado a la cámara. */
enum { NSG_FXN_MAX = 6, NSG_FXN_KEPT = 0, NSG_FXN_SHRUNK = 1, NSG_FXN_DROP_INSIDE = 2, NSG_FXN_DROP_ONCAM = 3, NSG_FXN_DROP_FOLLOW = 4, NSG_FXN_HELD = 5 };
typedef struct {
    uint32_t src_key; uint16_t idx; uint16_t flags;   /* origen (primitiva y paquete) y NS_EF_* */
    uint8_t role, kind, action, screen;               /* papel (NSFX_ROLE_*), NS_EK_*, accion, 1 = efecto de pantalla (ON_CAMERA o papel de superficie de vista) */
    float dist_m, size_m, deg_raw, deg_after;         /* distancia centro-cabeza, mayor lado/diagonal (m), tamaño angular antes y después de la comodidad */
} NsGpuFxNear;
/* Detector "sigue a la cámara": estado de un efecto seguido. SIN_VERIFICAR = cercano aún sin demostrar que está en el mundo (retenido); SIGUE = fijo a la cabeza; VAGA = cercano e inestable. */
enum { NSG_FS_UNKNOWN = 0, NSG_FS_WORLD = 1, NSG_FS_FOLLOW = 2, NSG_FS_WANDER = 3 };
enum { NSG_FXF_MAX = 8, NSG_ORIGIN_FX = 0, NSG_ORIGIN_FILL = 1 };
typedef struct {
    uint32_t src_key; uint16_t idx; uint8_t sub, role, kind, state, origin, oncam;   /* origen: NSG_ORIGIN_* (los rellenos llevan 0xF1 en el byte alto de src_key) */
    float dist_m, size_m, hs[3];        /* distancia y mayor lado (m); posición en espacio de cabeza (m: derecha, arriba, delante) */
} NsGpuFxFollow;
typedef struct {
    uint32_t n_opaque, n_semi, n_runs, n_keys, n_sitems;
    uint32_t n_items, n_skipped;        /* efectos entrados / descartados (no finitos, degenerados) */
    uint32_t n_billboard, n_ribbon, n_poly;
    uint32_t n_near, n_near_oncam, n_near_screen;   /* con el centro a menos de near_log_m; de ellos pegados a la cámara / de pantalla */
    uint32_t n_shrunk, n_drop_inside, n_drop_oncam; /* comodidad: reducidos y descartados (los descartados cuentan como confirmados: el motor no los redibuja) */
    uint32_t n_zref;                    /* carteles de tamaño en píxeles con poca profundidad (se usó la de referencia: tamaño de mundo inflado) */
    uint32_t n_rec;                     /* entradas válidas de rec[] */
    NsGpuFxNear rec[NSG_FXN_MAX];       /* los de mayor deg_raw */
    uint32_t n_follow, n_wander, n_held, n_world;   /* seguimiento: descartados por ir fijos a la cabeza / por vagar cerca / retenidos sin verificar / verificados en el mundo */
    uint32_t n_follow_fill, n_tracked;  /* de los SIGUE, cuántos son relleno; efectos seguidos */
    uint32_t n_fol;                     /* entradas válidas de fol[] */
    NsGpuFxFollow fol[NSG_FXF_MAX];     /* los SIGUE / VAGA / retenidos más cercanos a la cabeza */
} NsGpuFxOut;
/* Acumulador de registro (1 s): cuántos y cuáles efectos cerca de la cabeza, y si el jugador andaba o giraba. */
typedef struct {
    uint32_t frames, frames_near, frames_moving, frames_moving_near;
    uint32_t max_near, max_near_oncam, max_near_screen, max_shrunk, max_zref, sum_drop_inside, sum_drop_oncam;
    float max_deg_raw, max_speed_mps, max_turn_dps;
    int has_prev; float prev_head[3], prev_right[3], prev_t;
    uint32_t n_rec; NsGpuFxNear rec[NSG_FXN_MAX];
} NsGpuFxNearAcc;
/* Suma un fotograma. head/right: cabeza y eje derecho en el mundo VR; t_s: reloj en segundos (para m/s y grados/s). Moviéndose = más de 0,3 m/s o 30 grados/s. */
void ns_gpu_fx_near_accum(NsGpuFxNearAcc *acc, const NsGpuFxOut *o, const float head[3], const float right[3], float t_s);
/* Una línea de texto (sin salto de línea) con el resumen; devuelve su longitud. Llamar después ns_gpu_fx_near_reset. */
size_t ns_gpu_fx_near_describe(const NsGpuFxNearAcc *acc, char *buf, size_t cap);
void ns_gpu_fx_near_reset(NsGpuFxNearAcc *acc);
/* Acumulador del detector (ventana de 1 s): qué efectos / rellenos iban fijos a la cabeza, con papel, origen y clave. */
typedef struct { NsGpuFxFollow f; uint32_t frames; } NsGpuFxFollowRec;
typedef struct {
    uint32_t frames, frames_follow, frames_held, max_follow, max_wander, max_held, max_follow_fill, sum_follow, sum_held, max_tracked, max_world;
    uint32_t n_rec; NsGpuFxFollowRec rec[NSG_FXF_MAX];
} NsGpuFxFollowAcc;
void ns_gpu_fx_follow_accum(NsGpuFxFollowAcc *acc, const NsGpuFxOut *o);
size_t ns_gpu_fx_follow_describe(const NsGpuFxFollowAcc *acc, char *buf, size_t cap);
void ns_gpu_fx_follow_reset(NsGpuFxFollowAcc *acc);
const char *ns_gpu_fx_role_name(int role);
/* Tope angular (grados) de la comodidad para un efecto con el centro a `d` m de la cabeza; 1e9 = sin tope; 0 = descartar. */
float ns_gpu_fx_comfort_cap_deg(float d, float x_deg, float dmin_m);
/* Capacidades: verts >= 12 * n_fx (peor caso: cartel semitransparente texturizado = 6 + 6), runs >= n_fx, keys >= n_fx. 0 = bien; <0 parámetros inválidos. */
int ns_gpu_fx_build(const NsEffect *fx, uint32_t n_fx, const NsGpuFxParams *prm, NsGpuFxVert *verts, uint32_t cap_verts, NsGpuFxRun *runs, uint32_t cap_runs,
                    uint64_t *keys, uint32_t cap_keys, NsGpuFxOut *out);
/* Igual que ns_gpu_fx_build y, si items != NULL, escribe un NsGpuFxSemiItem por efecto semitransparente en el orden de dibujo (cap_items >= n_fx). */
int ns_gpu_fx_build_ex(const NsEffect *fx, uint32_t n_fx, const NsGpuFxParams *prm, NsGpuFxVert *verts, uint32_t cap_verts, NsGpuFxRun *runs, uint32_t cap_runs,
                       uint64_t *keys, uint32_t cap_keys, NsGpuFxSemiItem *items, uint32_t cap_items, NsGpuFxOut *out);

/* ---- Una sola pasada semitransparente ordenada: mallas y efectos intercalados ----
 * Entra la lista de triángulos de malla ya ordenada (ns_gpu_sort_semi_ex: profundidad + modo por entrada, de atrás adelante) y los efectos semitransparentes (ya
 * ordenados). Sale una lista de tramos en el orden de dibujo: tipo 0 = triángulos [start, start+count) de la lista de mallas; tipo 1 = vértices del búfer de efectos.
 * A igual profundidad va primero la malla (el efecto se pinta encima). Tramos contiguos del mismo tipo y modo de mezcla se unen. Devuelve los tramos escritos
 * (si cap_runs no basta se detiene y se pone *truncated). */
typedef struct { uint32_t kind; uint32_t start, count; uint8_t abr; } NsGpuSemiRun;
size_t ns_gpu_semi_merge(const float *mesh_depth, const uint8_t *mesh_abr, size_t n_mesh, const NsGpuFxSemiItem *fx, size_t n_fx, NsGpuSemiRun *runs, size_t cap_runs,
                         int *truncated);

/* Clave de 64 bits (no nula) de un efecto: la misma que ns_fx_item_key de ns_fx.h. */
static inline uint64_t ns_gpu_fx_key(const NsEffect *e) { return ((uint64_t)e->src_key << 32) | ((uint64_t)e->idx << 8) | (uint64_t)e->sub; }

/* Accesores para Swift (los arrays fijos de C llegan como tuplas) */
static inline const float *ns_scene_to_vr(const NsScene *s) { return s->to_vr; }
static inline const float *ns_scene_cam_rot(const NsScene *s) { return s->cam.rot; }
static inline const float *ns_scene_cam_eye(const NsScene *s) { return s->cam.eye; }

#ifdef __cplusplus
}
#endif
