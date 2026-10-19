#pragma once
/* vr_layer2d: capa 2D (HUD, menús, textos, créditos, radio, fundidos) rasterizada con la aritmética EXACTA de la GPU original.
 *
 * Por qué existe. La capa 2D salía de los batches kind 2 de vr_scene dibujados por un shader (render/metal/vr_scene.metal) a 4x con filtro
 * bilineal, sin tramado, en 8 bits continuos, con rects y líneas convertidos a triángulos y los texels semitransparentes en dos pasadas por
 * batch (se pierde el orden entre primitivas). Y sobre un panel transparente de alfa premultiplicado no existe el modo B-F (los fundidos y
 * sombras sustractivos se perdían) ni el B+F real (aproximado). Todo eso hacía que textos, créditos y menús no coincidieran con el emulador.
 *
 * Qué hace. Reproduce EN ORDEN, a 1x y sobre una VRAM espejo (eventos de VRAM de las listas, como el renderer), las primitivas que vr_scene
 * mandó al panel (VrScene.op_dest == 2), con el mismo código que gpu.c: reglas de arista y relleno, interpolación entera, ventana de textura,
 * texels de 4/8/15 bits con CLUT, texel 0 transparente, bit 15 como interruptor de la semitransparencia, modulación (texel * color) >> 4,
 * tramado 4x4, canal de 5 bits, 4 modos de mezcla, máscara, recorte por primitiva y primitivas de realimentación ya resueltas (fbk/fbt).
 * El destino no es la VRAM sino una capa con transmitancia (lo que queda del mundo que hay debajo):
 *     composición = clamp( c + t * mundo - d )     (c = color acumulado en 5 bits, t = fracción de mundo que se ve 0..256, d = lo que se resta del mundo)
 * Con t = 0 (opaco) el píxel es el de la GPU original bit a bit. Sobre fondo negro (mundo = 0) la composición es la imagen de la GPU original
 * (pruebas: test_layer2d.c contra gpu.c). Mezcla: B/2+F/2 -> c=(c+F)>>1, t/=2; B+F -> c+=F; B-F -> c-=F si ya es opaco, si no d+=F; B+F/4 -> c+=F>>2.
 *
 * Salida: RGBA8 NO premultiplicado (como hudTexture) de la zona mostrada (disp.w x disp.h), filas de arriba abajo. Lo opaco es exacto (5 -> 8 bits como
 * la salida de vídeo); lo semitransparente se codifica como alfa premultiplicado equivalente (el panel se compone con "over"): el sustractivo sobre el
 * mundo se aproxima por oscurecimiento (1 - d); lo aditivo puro (alfa 0) como alfa = canal máximo. C99 puro, sin dependencias de plataforma. */
#include <stdint.h>
#include "gpu.h"
#include "vr_scene.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t c[3];      /* color acumulado: 5 bits por canal con escala 1, 8 bits con escala > 1 */
    uint8_t m;         /* bit de máscara (bit 15) del último píxel escrito */
    uint16_t t;        /* transmitancia del mundo, 0 (opaco) .. 256 (sin cubrir) */
    uint16_t d[3];     /* resta pendiente sobre el mundo, en cuartos de nivel */
} VrLayerPx;

/* Suavizado del muestreo de texturas (y del vídeo ampliado): 0 original (vecino más próximo), 1 nítido (píxel art: corrección de diagonales tipo EPX/Scale2x
 * sobre el texel de VRAM), 2 suave (bilineal). Solo actúa con escala > 1. */
enum { VL_SMOOTH_ORIGINAL = 0, VL_SMOOTH_SHARP = 1, VL_SMOOTH_SOFT = 2 };

typedef struct VrLayer2D VrLayer2D;

VrLayer2D *vl2d_new(void);
void vl2d_free(VrLayer2D *L);
/* Olvida la VRAM espejo: hasta la siguiente lista con vram_init no hay capa (vl2d_run devuelve 0 y rgba NULL). */
void vl2d_reset(VrLayer2D *L);
/* Escala interna (1..6; defecto 4) y suavizado (VL_SMOOTH_*; defecto nítido). Escala 1 = rasterizado exacto de 5 bits con tramado (idéntico a la GPU original bit a bit);
 * escala > 1 = muestras de 8 bits sin tramado, geometría a alta resolución con el mismo relleno (muestreo en el centro de cada submuestra). */
void vl2d_set_options(VrLayer2D *L, int scale, int smooth);

/* Procesa UNA lista (todas, en orden, aunque la escena falle: la VRAM espejo debe ver todos los eventos). sc == NULL, o sin sc->op_dest, solo aplica los
 * eventos. Con escena dibuja las primitivas con op_dest == 2 en su orden original, intercaladas con los eventos de VRAM; con `full` != 0 dibuja TODAS las
 * primitivas sobre el área mostrada de la VRAM espejo ampliada (pantalla de cine: el fotograma entero a alta resolución, vídeo incluido; la salida es opaca).
 * `l` debe ser la lista con la que se ensambló sc. Devuelve 1 si hay capa nueva (vl2d_rgba / vl2d_px válidos hasta la siguiente llamada), 0 si no. */
int vl2d_run(VrLayer2D *L, const GpuDrawList *l, const VrScene *sc, int full);

/* Capa RGBA8 no premultiplicada de la última llamada con capa; w x h = (zona mostrada) * escala. NULL si no hay. */
const uint8_t *vl2d_rgba(const VrLayer2D *L, int *w, int *h);
/* Estado interno por píxel (pruebas): mismas dimensiones. NULL si no hay capa. */
const VrLayerPx *vl2d_px(const VrLayer2D *L, int *w, int *h);
/* VRAM espejo 1024x512 (pruebas). */
const uint16_t *vl2d_vram(const VrLayer2D *L);

/* Composición de un píxel de la capa de ESCALA 1 sobre un mundo de 5 bits por canal (bg5) -> 5 bits (out5). */
void vl2d_compose5(const VrLayerPx *p, const uint8_t bg5[3], uint8_t out5[3]);
/* Codificación a RGBA8 no premultiplicado (la que usa vl2d_run). hq = 1 con color de 8 bits (escala > 1). */
void vl2d_encode(const VrLayerPx *p, int hq, uint8_t rgba[4]);

#ifdef __cplusplus
}
#endif
