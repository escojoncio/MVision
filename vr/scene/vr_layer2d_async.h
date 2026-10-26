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
/* Opciones del visor: raster NATIVO fijo (escala 1, suavizado original = rasterizado exacto de la GPU original, 24 bits conservados en vídeo). El escalado y el filtro de píxel art (xBR) los hace la GPU del renderer
 * sobre esa capa 1x; la re-rasterización a x2..x6 de la CPU y la escala adaptativa ya no intervienen en la imagen. Equivale a vl2a_options(A, 1, VL_SMOOTH_ORIGINAL). */
void vl2a_options_native(VrL2Async *A);
/* Troceo del 2D (interruptor global, APAGADO por defecto): con 0 la capa 2D es UNA sola imagen completa (todo lo que el juego dibuja en 2D: HUD, radar del juego, menús de objetos/armas, pausa, créditos, textos) y el
 * trabajador nunca calcula la ventana del selector (vl2d_set_menu(0): sin rect de menú, sin omitir créditos/vida/texto de la capa). Con 1 vuelve el comportamiento anterior (rect del menú sobre la mano, filtro de
 * créditos por región). Lo lee también vrhost.c (separación del radar, vrradar_split) y el visor (mv_vrhost_l2d_split): una sola fuente de verdad. Cualquier hilo. */
void vl2a_set_split(int on);
int vl2a_split(void);
/* Etiqueta de la vista vigente ((view_kind << 8) | view_reason de vr_view.h; la fija el hilo de emulación antes de vl2a_submit): cada trabajo la guarda y la capa publicada la conserva. */
void vl2a_set_view(VrL2Async *A, int view);
/* Hilo de emulación: encola la lista (copia). op_dest (nops bytes) NULL = escena fallida: solo eventos. full: VL_PANEL / VL_FULL / VL_PANEL_VERIFIED (vr_layer2d.h; el verificado se publica como panel, full = 0). reset != 0: olvida la VRAM espejo antes. Devuelve 1 si se encoló. */
int vl2a_submit(VrL2Async *A, const GpuDrawList *l, const uint8_t *op_dest, int full, int reset);
/* Consumidor: -1 = no hay capa utilizable (apagada, sin resultado o con retraso excesivo); 0 = la misma que la última adquirida; 1 = nueva (px/w/h/full/seq válidos hasta vl2a_release). */
int vl2a_acquire(VrL2Async *A, const uint8_t **px, int *w, int *h, int *full, uint32_t *seq);
void vl2a_release(VrL2Async *A);
/* Consumidor, entre vl2a_acquire (r >= 0) y vl2a_release: modo (VL_PANEL / VL_FULL / VL_PANEL_VERIFIED) y etiqueta de vista (vl2a_set_view) del trabajo que produjo la capa adquirida. 0 si no hay capa. */
int vl2a_acquire_info(VrL2Async *A, int *mode, int *view);
/* Consumidor, entre vl2a_acquire (r >= 0) y vl2a_release: rect UV [u0, v0, u1, v1] (0..1) de la ventana de menú de la capa adquirida (primitivas nuevas respecto a la lista previa sin menú, vl2d_menu_rect);
 * solo hay rect si el trabajo era de la vista selector (VRV_R_SELECTOR). 0 si no hay. */
int vl2a_acquire_menu_rect(VrL2Async *A, float uv[4]);
/* ¿Vale la capa (mode, layer_view) para la vista vigente cur_view? (función pura). La capa debe ser de la misma clase de vista (kind = view >> 8: una capa retrasada de los créditos, kind motor, no entra en el
 * mundo) y, con overlay (ventana 2D del menú sobre el mundo, hudTexture = VL_PANEL), solo VL_PANEL. */
int vl2a_layer_accept(int mode, int layer_view, int cur_view, int overlay);
/* Estadísticas: escala actual, ms del último dibujo del trabajador, ms máximos de vl2a_submit, retraso en listas de la última capa publicada. */
void vl2a_stats(VrL2Async *A, int *scale, double *work_ms, double *submit_ms_max, uint32_t *lag);
/* Línea de log «[capa2d] ...» con los contadores desde la llamada anterior (los pone a cero). Devuelve la longitud escrita. */
int vl2a_report(VrL2Async *A, char *buf, int n);
/* Estado de la escala adaptativa: últimos 8 tiempos de trabajo (media móvil) y trabajos seguidos con la media bajo el umbral de subida. A cero = estado inicial. */
typedef struct { double ms[8]; int n, i, fast; } Vl2aScale;
/* Paso de la escala adaptativa: devuelve la escala siguiente para un trabajo de `ms`. Baja (cur - 1) solo si la media de los últimos 8 trabajos supera 12 ms; sube (cur + 1, hasta target) tras 15 trabajos
 * seguidos en que la media permitiría la escala siguiente; un pico aislado apenas mueve la media y no reinicia la cuenta. Al cambiar de escala se vacía la media. */
int vl2a_scale_step(int cur, int target, double ms, Vl2aScale *st);
/* Pruebas: espera a que la cola quede vacía. */
void vl2a_flush(VrL2Async *A);

#ifdef __cplusplus
}
#endif
