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

/* Unidades del motor por unidad de NsGpuPiece.raise (el renderer multiplica raise por fog_par.w = NS_GPU_RAISE_UNIT / units_per_m metros). */
#define NS_GPU_RAISE_UNIT 0.05f
/* Retraso de orden (en unidades de `raise`: 40 = 2 unidades del motor) del escenario de un grupo de mapa NO activo. Los mapas de un mismo stage comparten espacio y repiten caras en sus fronteras
 * (medido: 10-33 triángulos duplicados entre grupos en dos zonas): con todos dibujados, a igual profundidad gana el mapa activo y no parpadean. */
#define NS_GPU_OFFGROUP_BACK 40.0f
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
/* Política de descarte por detrás del sombreador (NsGpuUniforms.cull_eye.w = mode, NSG_CULL_*): 1 si una cara con esas banderas (NSG_CF_*) se descarta cuando se ve por detrás. */
int ns_gpu_cull_applies(int mode, uint32_t cface_flags);
void ns_gpu_vert_world(const NsGpuVert *pool, const NsGpuPiece *tbl, uint32_t gi, uint32_t vabs, float out[3]);
/* Réplica en C del trato de los reversos que NO se descartan (ns_vertex): factor por el que se multiplica la profundidad de recorte (Z invertida) de un triángulo. `back` = se ve por detrás desde
 * el ojo de la vista. NSG_BACK_Z para el reverso de una cara de una sola cara que el modo `mode` (NSG_CULL_*) no descarta: se dibuja, pero pierde el empate con el anverso que comparte su plano; 1 en
 * los demás casos (anversos, mallas de doble cara, y reversos que el modo descarta). */
float ns_gpu_back_z(int mode, uint32_t cface_flags, int back);
/* Réplica en C del tamaño máximo del hardware (ns_vertex, solo con la cámara del juego): 1 si el triángulo, proyectado en la pantalla del motor, mide más de NSG_HW_MAX_W px de ancho o
 * NSG_HW_MAX_H de alto y por tanto la GPU original no lo dibuja. v = vértices en la vista del motor (x derecha, y abajo, z delante; unidades del motor), hx/hy = píxeles por unidad de tangente
 * (H y H por el factor vertical). Las coordenadas saturan a NSG_HW_SXY_MIN..MAX antes de medir, como las de la GTE. Un vértice con z <= 0 no tiene proyección: devuelve 0 (lo decide el descarte cercano). */
int ns_gpu_tri_oversize(const float v[3][3], float hx, float hy);
/* Réplica en C de la proyección de la GTE para un triángulo en la vista del motor (cámara del juego): la división H/SZ satura en 2,0 cuando z <= zc (= H/2), así que un vértice con z < zc se proyecta
 * como si estuviera en el plano z = zc (x e y de la vista intactas) y el polígono se dibuja igualmente. out = vértices tras ese ajuste. NSG_GTE_BEHIND si ningún z > 0; NSG_GTE_OVERSIZE si, ya
 * ajustado, supera el tamaño máximo del hardware (ns_gpu_tri_oversize); si no, NSG_GTE_DRAW. hx/hy como en ns_gpu_tri_oversize. Es la regla del motor por triángulo entero; ns_gpu_engine_tri la aplica
 * y añade el rescate en el campo extra de la pantalla 3D. */
int ns_gpu_gte_tri(const float v[3][3], float zc, float hx, float hy, float out[3][3]);
/* Réplica en C de las reglas del modo ENGINE de ns_vertex / ns_fs (pantalla 3D con la cámara del motor, U.misc.y > 0; ver NSG_ENG_* en ns_gpu_types.h), por triángulo (p = vértices en el
 * mundo del motor, orden de ns_gpu_pack). Primero la regla del motor con su ojo, que es la del fotograma del juego: NSG_ENG_BEHIND (ningún z de vista > 0), vértices con z < H/2 movidos al plano
 * H/2, NSG_ENG_BACK (cara de una sola cara `one_sided` vista por detrás, con los vértices ya movidos) y NSG_ENG_OVERSIZE (más de 1023 x 511 px). Lo que pasa se dibuja con los vértices movidos
 * (NSG_ENG_DRAW, `draw` = esos vértices). Lo descartado por reverso o tamaño se rescata (NSG_ENG_EXTRA, `draw` = p) si no es cercano (algún vértice a z >= h): el de tamaño siempre; el de
 * reverso, si es anverso desde alguno de los `neyes` ojos (`eyes`, mundo del motor; ns_vertex decide cada vista con SU ojo: neyes = 1 da lo que dibuja ese pase). De un triángulo rescatado solo se dibujan los puntos FUERA de la ventana del motor
 * (ns_gpu_engine_win == 0). `can_split` = el objeto no es GBOUND (el motor original parte sus caras; pieza sin NSG_PF_NOSPLIT): si la regla del motor la
 * perdería por tamaño o la deformaría (algún z < H/2), su doble área saturada pasa de NSG_DIV_AREA y la media de max(z, 0) no llega a NSG_DIV_Z (NSG_DIV_Z_FAR con h > 1000), se dibuja con
 * su geometría real en todas partes (NSG_ENG_SPLIT, `draw` = p). `off` = reglas apagadas (NSG_ENG_OFF_*). `draw` puede ser NULL. */
typedef struct { float eye[3], x[3], y[3], fwd[3]; float h, hx, hy; } NsGpuEngCam;   /* ojo y ejes de la vista del motor en el mundo (derecha, abajo, delante); H; px por unidad de tangente en x e y */
int ns_gpu_engine_tri(const float p[3][3], const NsGpuEngCam *c, int one_sided, int can_split, const float (*eyes)[3], int neyes, ns_u32 off, float draw[3][3]);
/* 1 si el punto p (mundo del motor) se proyecta con el ojo del motor dentro de su ventana (z > 0, |x| <= NSG_ENG_WIN_X px, |y| <= NSG_ENG_WIN_Y px). Réplica del recorte de ns_fs (NSG_CF_WINCUT). */
int ns_gpu_engine_win(const NsGpuEngCam *c, const float p[3]);
/* Reglas del modo ENGINE apagadas desde los ajustes de la app (NSG_ENG_OFF_*; 0 = todas activas). Atómico: lo escribe la interfaz y lo leen el render y el anfitrión. */
void ns_gpu_engine_set_off(ns_u32 off);
ns_u32 ns_gpu_engine_off(void);
/* Separación de las dos cámaras de la pantalla 3D (unidades del motor; ns_cinema_stereo) con la que se dibujó la última vez: los ojos de la vista son el del motor -+ sep / 2 a lo largo de su eje X.
 * La escribe el render y la lee el anfitrión para contar los descartes con los ojos reales (línea [descartes]). Atómico. */
void ns_gpu_engine_set_sep(float units);
float ns_gpu_engine_sep(void);
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
    /* near_log_m (0 = 0,5 m): radio del registro de cercanos (diagnóstico). */
    float near_log_m;
    /* Única red de seguridad: se descarta (confirmado) el efecto con el centro a menos de drop_inside_m de la cabeza (0 = no); las superficies del entorno (agua, paneles) no cuentan. Los efectos bien
     * colocados no se tocan: el tamaño es de mundo (NS_EF_WORLD_SIZE, calculado una vez con la cámara del motor, igual para los dos ojos) y nada se encoge ni se retiene por «seguirse». Además se
     * DESCARTA siempre (confirmado) todo NS_EF_ON_CAMERA que no sea de pantalla (papel VIEW_OVERLAY): no tiene emisor en el mundo. */
    float drop_inside_m;
} NsGpuFxParams;
typedef struct { uint32_t start, count; uint8_t abr; } NsGpuFxRun;   /* start/count en vértices del búfer completo */
/* Un efecto semitransparente en el búfer (ya ordenado de atrás adelante): para intercalarlo con los triángulos semitransparentes de las mallas (ns_gpu_semi_merge). */
typedef struct { float depth; uint32_t start, count; uint8_t abr; } NsGpuFxSemiItem;
/* Registro de efectos cercanos a la cabeza (por fotograma, los de mayor tamaño angular). accion: 0 intacto, 2 descartado por nacer dentro de la cabeza (drop_inside_m), 3 descartado por ir pegado a la cámara sin ser de pantalla. */
enum { NSG_FXN_MAX = 6, NSG_FXN_KEPT = 0, NSG_FXN_DROP_INSIDE = 2, NSG_FXN_DROP_ONCAM = 3 };
typedef struct {
    uint32_t src_key; uint16_t idx; uint16_t flags;   /* origen (primitiva y paquete) y NS_EF_* */
    uint8_t role, kind, action, screen;               /* papel (NSFX_ROLE_*), NS_EK_*, accion, 1 = efecto de pantalla (ON_CAMERA o papel de superficie de vista) */
    float dist_m, size_m, deg_raw;                    /* distancia centro-cabeza, mayor lado/diagonal (m), tamaño angular */
} NsGpuFxNear;
typedef struct {
    uint32_t n_opaque, n_semi, n_runs, n_keys, n_sitems;
    uint32_t n_items, n_skipped;        /* efectos entrados / descartados (no finitos, degenerados) */
    uint32_t n_billboard, n_ribbon, n_poly;
    uint32_t n_near, n_near_oncam, n_near_screen;   /* con el centro a menos de near_log_m; de ellos pegados a la cámara / de pantalla */
    uint32_t n_drop_inside, n_drop_oncam;           /* descartados (cuentan como confirmados: el motor no los redibuja): dentro de la cabeza / ON_CAMERA que no es de pantalla */
    uint32_t n_zref;                    /* carteles de tamaño en píxeles con poca profundidad (se usó la de referencia: tamaño de mundo inflado) */
    uint32_t n_rec;                     /* entradas válidas de rec[] */
    NsGpuFxNear rec[NSG_FXN_MAX];       /* los de mayor deg_raw */
    uint32_t n_drawn;                   /* efectos que entran en la geometría de salida (ni descartados ni de una cara vistos por detrás) */
    float min_dist_m;                   /* distancia mínima centro-cabeza de esos efectos (válida si n_drawn > 0) */
} NsGpuFxOut;
/* Acumulador de registro (1 s): cuántos y cuáles efectos cerca de la cabeza, y si el jugador andaba o giraba. */
typedef struct {
    uint32_t frames, frames_near, frames_moving, frames_moving_near;
    uint32_t max_near, max_near_oncam, max_near_screen, max_zref, sum_drop_inside, sum_drop_oncam;
    float max_deg_raw, max_speed_mps, max_turn_dps;
    int has_prev; float prev_head[3], prev_right[3], prev_t;
    uint32_t n_rec; NsGpuFxNear rec[NSG_FXN_MAX];
} NsGpuFxNearAcc;
/* Suma un fotograma. head/right: cabeza y eje derecho en el mundo VR; t_s: reloj en segundos (para m/s y grados/s). Moviéndose = más de 0,3 m/s o 30 grados/s. */
void ns_gpu_fx_near_accum(NsGpuFxNearAcc *acc, const NsGpuFxOut *o, const float head[3], const float right[3], float t_s);
/* Una línea de texto (sin salto de línea) con el resumen; devuelve su longitud. Llamar después ns_gpu_fx_near_reset. */
size_t ns_gpu_fx_near_describe(const NsGpuFxNearAcc *acc, char *buf, size_t cap);
void ns_gpu_fx_near_reset(NsGpuFxNearAcc *acc);
const char *ns_gpu_fx_role_name(int role);
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
/* 1 si la pieza lleva malla (el renderer la sube; si no está en el almacén es una malla ausente); 0 si el extractor no le encontró caras: no se dibuja ni se espera. */
static inline int ns_gpu_piece_has_mesh(const NsPiece *p) { return p->mesh_id != 0; }
/* Mallas AUSENTES de una escena: piezas que llevan malla y cuyo id no está en el almacén (lo que el renderer cuenta para retener la escena). Una pieza sin malla (modelo nulo o sin caras: el
 * extractor la deja con mesh_id = 0 y NS_PF_HIDDEN) no es una ausencia: no hay nada que esperar. En línea para no atar ns_gpu.c al almacén. */
static inline uint32_t ns_gpu_scene_misses(const NsScene *sc, const NsMeshStore *st) {
    uint32_t n = 0;
    const uint32_t np = sc->n_piece < sc->cap_piece ? sc->n_piece : sc->cap_piece;
    for (uint32_t i = 0; i < np; i++) if (ns_gpu_piece_has_mesh(&sc->piece[i]) && !ns_store_get(st, sc->piece[i].mesh_id)) n++;
    return n;
}
static inline const float *ns_scene_to_vr(const NsScene *s) { return s->to_vr; }
static inline const float *ns_scene_cam_rot(const NsScene *s) { return s->cam.rot; }
static inline const float *ns_scene_cam_eye(const NsScene *s) { return s->cam.eye; }

#ifdef __cplusplus
}
#endif
