#pragma once
/* vr_split: diagnóstico de la lista de la GPU. Parte en CPU los triángulos 3D cuya arista más larga supera un umbral (en metros del mundo VR) y cuenta
 * triángulos largos / que cruzan el plano cercano. Sin dependencias de plataforma (C99 + libm); se prueba en Linux (vr/scene/test_split.c). */
#include <stdint.h>
#include "vr_scene.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    VrVtx *v;                 /* vértices añadidos (malloc) */
    uint32_t n;               /* nº de vértices añadidos */
    uint32_t *first, *count;  /* por batch (nb entradas): first = nv + desplazamiento en `v` (índice absoluto del búfer combinado), count nuevo; first = 0xFFFFFFFF = sin cambio */
    uint32_t nb;
    uint32_t src_tris;        /* triángulos 3D examinados (kinds 0 y 1) */
    uint32_t split_tris;      /* triángulos originales que se subdividieron */
    uint32_t out_tris;        /* triángulos de los batches reescritos */
    uint32_t cap_skipped;     /* triángulos largos que no se partieron del todo (tope de salida o profundidad) */
} VrSplit;

/* Parte los triángulos de los batches 3D (kind 0/1) por bisección de la arista más larga hasta que ninguna supere max_edge (m). Un batch sin triángulos largos no
 * se toca. Tope de seguridad: la salida total de los batches reescritos no pasa de max_out_tris triángulos (lo que no cabe se deja sin partir).
 * Devuelve 0 si hay algo que reemplazar (liberar con vr_split_free), 1 si no hace falta nada (out vacío, nada que liberar), -1 si falta memoria o los parámetros no valen. */
int vr_split_run(const VrVtx *v, uint32_t nv, const VrBatch *b, uint32_t nb, float max_edge, uint32_t max_out_tris, VrSplit *out);
void vr_split_free(VrSplit *s);

typedef struct {
    uint32_t tris;            /* triángulos examinados */
    uint32_t big;             /* con alguna arista > big_edge (m) */
    uint32_t cross;           /* cruzan el plano cercano (algún vértice a menos de `near` de profundidad y otro a más) */
    uint32_t big_cross;       /* ambas cosas */
    float max_edge;           /* arista más larga vista (m) */
} VrScan;

/* Acumula en `acc` el recuento de la lista de vértices `v` (n múltiplo de 3; posiciones en el mundo VR). view = mundo -> ojo, 16 floats en orden de columnas;
 * la profundidad de un vértice es -z del espacio del ojo. */
void vr_scan_tris(const VrVtx *v, uint32_t n, const float view[16], float near, float big_edge, VrScan *acc);

#ifdef __cplusplus
}
#endif
