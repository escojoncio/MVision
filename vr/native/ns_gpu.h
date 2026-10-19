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

/* Ordena todos los triángulos semitransparentes de atrás adelante (clave = profundidad del centroide; empate = orden de entrada: el mismo criterio del
 * rasterizador de referencia). Escribe como mucho cap entradas en out/abr_out y devuelve cuántas. */
size_t ns_gpu_sort_semi(const NsGpuSemiSrc *src, size_t nsrc, NsGpuOrder *out, uint8_t *abr_out, size_t cap);

#ifdef __cplusplus
}
#endif
