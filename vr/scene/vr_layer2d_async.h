#pragma once
/* vr_layer2d_async: la capa 2D fiel (vr_layer2d.h) en un hilo trabajador propio, para que ni la emulación ni el render esperen a la rasterización.
 *
 * Flujo. El hilo de emulación llama a vl2a_submit una vez por lista: copia lo mínimo (ops, datos de subida, op_dest, VRAM inicial si la hay) en una cola circular
 * y vuelve (memcpy, sin trabajo de píxeles). El trabajador procesa la cola EN ORDEN (la VRAM espejo debe ver todos los eventos) y publica el resultado en un búfer de
 * un anillo de 3 (publicado / en uso por el consumidor / libre para escribir). El consumidor (hilo de render) pide con vl2a_acquire la última capa publicada; no espera
 * nunca. Si el trabajador va por detrás, las listas antiguas de la cola se procesan SOLO con sus eventos (barato) y se dibuja la más reciente: el retraso de la capa
 * es de una lista salvo sobrecarga, y con más de L2A_STALE listas de retraso acquire devuelve -1 (el renderer vuelve al dibujo por shader).
 * La escala adaptativa mide el coste del trabajador (no el de la emulación). C11 + pthreads. */
#include <stdint.h>
#include "gpu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VrL2Async VrL2Async;

VrL2Async *vl2a_new(void);
void vl2a_free(VrL2Async *A);
/* Escala objetivo (1..6) y suavizado (VL_SMOOTH_*); cualquier hilo. */
void vl2a_options(VrL2Async *A, int scale, int smooth);
/* Hilo de emulación: encola la lista (copia). op_dest (nops bytes) NULL = escena fallida: solo eventos. full: ver vl2d_run. reset != 0: olvida la VRAM espejo antes. Devuelve 1 si se encoló. */
int vl2a_submit(VrL2Async *A, const GpuDrawList *l, const uint8_t *op_dest, int full, int reset);
/* Consumidor: -1 = no hay capa utilizable (apagada, sin resultado o con retraso excesivo); 0 = la misma que la última adquirida; 1 = nueva (px/w/h/full/seq válidos hasta vl2a_release). */
int vl2a_acquire(VrL2Async *A, const uint8_t **px, int *w, int *h, int *full, uint32_t *seq);
void vl2a_release(VrL2Async *A);
/* Estadísticas: escala actual, ms del último dibujo del trabajador, ms máximos de vl2a_submit, retraso en listas de la última capa publicada. */
void vl2a_stats(VrL2Async *A, int *scale, double *work_ms, double *submit_ms_max, uint32_t *lag);
/* Línea de log «[capa2d] ...» con los contadores desde la llamada anterior (los pone a cero). Devuelve la longitud escrita. */
int vl2a_report(VrL2Async *A, char *buf, int n);
/* Pruebas: espera a que la cola quede vacía. */
void vl2a_flush(VrL2Async *A);

#ifdef __cplusplus
}
#endif
