#pragma once
/* Emulador por CPU del camino HUD (kind 2) del renderer Metal: replica vr_vs_hud + vr_fs_scene (render/metal/vr_scene.metal)
 * y la mezcla/pases de PS1Renderer.encodeHud, a resolución 1x del área mostrada. Sirve para comprobar en Linux, sin Metal, que
 * lo que genera vr_scene_build se vería igual que el oráculo. Mantener sincronizado con vr_scene.metal. */
#include <stdint.h>
#include "vr_scene.h"

typedef struct { int w, h; float *rgba; } HudImg;   /* alfa premultiplicado (como hudPre) */

HudImg hud_img_new(int w, int h);
void hud_img_free(HudImg *im);
/* Aplica el evento de VRAM `o` (fill/copia/subida) sobre `vram` (1024x512 uint16). */
void hud_emu_event(const GpuDrawList *l, const GpuOp *o, uint16_t *vram);
/* Dibuja los batches kind 2 de `s` en `im` (origen = (rx,ry) en coordenadas de vértice), aplicando en orden los eventos de VRAM de
 * s->src->ops con índice < ev_end antes de cada batch (y los restantes al final). `vram` entra con el estado del INICIO de la lista.
 * im == NULL: solo aplica los eventos (mantiene la VRAM del renderer sin dibujar). */
void hud_emu_run(const VrScene *s, uint16_t *vram, HudImg *im, int rx, int ry);
/* Convierte a RGB8 sobre negro (rgb no premultiplicado * alfa, como se vería el panel sobre fondo negro). */
void hud_emu_to_rgb(const HudImg *im, uint8_t *rgb);

/* Emula el post-proceso del filtro de pantalla (vr_vs_post/vr_fs_post de vr_scene.metal, destino NO lineal): src/dst = RGB float 0..1 (w*h*3, filas de arriba abajo,
 * espacio gamma); cy_img/f_eye = fila del punto principal y focal vertical del ojo en píxeles (de la proyección: h/2*(1+P[2][1]) y h/2*P[1][1]); f = VrFilter de la escena.
 * Muestreo bilineal con clamp, igual que el sampler del shader. Las filas sin tira (rowcov=0) no se desplazan; el tinte y el recubrimiento se aplican a toda la vista. */
void hud_emu_post(const float *src, float *dst, int w, int h, float cy_img, float f_eye, const VrFilter *f);
/* Como hud_emu_post pero con el punto principal y la focal HORIZONTALES del ojo (cx_img, fx_eye, en píxeles) y con la clase 3b: varios recubrimientos en orden,
 * líneas de exploración y máscara de visión (f->frame). Replica vr_fs_post de vr_scene.metal; hud_emu_post la llama con cx_img = w/2 y fx_eye = f_eye. */
void hud_emu_post2(const float *src, float *dst, int w, int h, float cx_img, float cy_img, float fx_eye, float f_eye, const VrFilter *f);
