#pragma once
/* Rasterizador de REFERENCIA del renderer de alta resolución.
 * Consume GpuDrawList (runtime/gpu.h) y dibuja a escala N (1..8) con supermuestreo KxK, muestreando
 * texturas de una copia de la VRAM nativa que se actualiza con los eventos en orden. Es el modelo exacto
 * que se porta a render/metal/ps1_gpu.metal. Ver render/README.md. */
#include <stdint.h>
#include "gpu.h"

typedef struct RefR RefR;

enum {
    REF_HQ = 1,        /* color de 8 bits por canal, sin tramado, mezcla en 8 bits (defecto si N*K > 1) */
    REF_EXACT5 = 2,    /* fuerza canal de 5 bits y tramado aunque N*K > 1 (solo depuración) */
    REF_FLOAT_VTX = 4, /* usa GpuVtx.fx/fy cuando los 3 vértices los traen (ruta subpíxel futura) */
};

typedef struct {
    int scale;   /* resolución de salida respecto al nativo: 1..8 */
    int ssaa;    /* supermuestreo lineal por píxel de salida: 1 (sin AA), 2 (=4 muestras), 3, 4 */
    unsigned flags;
} RefOpts;

RefR *ref_create(const RefOpts *o);
void ref_destroy(RefR *r);

/* Aplica una lista capturada. hires = 0: solo se actualiza la VRAM nativa (rápido; descarta el estado
 * de alta resolución, que se reconstruye replicando la nativa). hires = 1: además se dibuja a escala. */
void ref_apply(RefR *r, const GpuDrawList *l, int hires);

/* VRAM nativa 1024x512 que mantiene el modelo (debe coincidir con gpu_vram del oráculo). */
const uint16_t *ref_vram(const RefR *r);

/* Imagen del área de display a escala N: RGB8 (propiedad de r, válida hasta la siguiente llamada).
 * Misma lectura que gpu_dump_ppm (ignora d->enabled). Modo 24 bits: se replica la nativa. */
const uint8_t *ref_present(RefR *r, const GpuDisplay *d, int *w, int *h);

/* Número de tiles de alta resolución reservados (diagnóstico). */
int ref_tiles(const RefR *r);
