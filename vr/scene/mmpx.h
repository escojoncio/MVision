#pragma once
/* MMPX 2x: ampliación de píxel art que conserva el estilo (M. McGuire y M. Gagiu, «MMPX Style-Preserving Pixel Art Magnification», JCGT 2021).
 * Cada píxel de salida es SIEMPRE la copia de un píxel de la entrada (nunca una mezcla): no crea colores ni alfas nuevos, así que vale igual para color directo,
 * para alfa premultiplicado y para la capa 2D codificada de vr_layer2d.h. Reglas (sobre el entorno de 3x3 más 8 vecinos a distancia 2..3): pendiente 1:1 (diagonales),
 * intersecciones (no romper líneas de 1 px ni esquinas) y pendiente 2:1. La comparación de píxeles es por igualdad exacta de los 4 bytes; la «luminosidad» que decide qué
 * lado gana es (r + g + b + 1) * (256 - alfa), como la referencia.
 * Reglas tomadas de la implementación de referencia MMPX.glc de los autores (Copyright 2020 Morgan McGuire & Mara Gagiu, licencia MIT, https://opensource.org/licenses/MIT). C99 puro. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* src: w x h píxeles RGBA8 (4 bytes por píxel, filas de arriba abajo; alfa 0 = transparente: su color se ignora y sale como 0,0,0,0). dst: 2w x 2h píxeles RGBA8 (no puede solaparse con src).
 * Fuera de la imagen se repite el borde. No hace nada si w o h <= 0. Para texturas del mundo y cualquier imagen RGBA de alfa directo. */
void mmpx_scale2x(const uint8_t *src, int w, int h, uint8_t *dst);

/* Lo mismo para una imagen de `planes` planos RGBA8 apilados en vertical (src: w x h*planes; dst: 2w x 2h*planes), como la capa 2D empaquetada de vr_layer2d.h (color premultiplicado, resta, tope):
 * dos píxeles son iguales solo si lo son en TODOS los planos, la luminosidad sale del plano 0 y cada píxel de salida copia el mismo píxel de origen en todos los planos (los planos no se desalinean).
 * Aquí el alfa 0 NO se trata aparte (en premultiplicado un píxel aditivo lleva alfa 0 y color). Es lo que hace en la GPU el núcleo xr_mmpx2 de XRShaders.metal. */
void mmpx_scale2x_planes(const uint8_t *src, int w, int h, int planes, uint8_t *dst);

#ifdef __cplusplus
}
#endif
