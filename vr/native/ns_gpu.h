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

/* Accesores para Swift (los arrays fijos de C llegan como tuplas) */
static inline const float *ns_scene_to_vr(const NsScene *s) { return s->to_vr; }
static inline const float *ns_scene_cam_rot(const NsScene *s) { return s->cam.rot; }
static inline const float *ns_scene_cam_eye(const NsScene *s) { return s->cam.eye; }

#ifdef __cplusplus
}
#endif
