/* vr_hzd: el ojo de la cámara VR no puede quedar fuera del volumen que el juego considera libre (paredes de colisión del propio juego, "hzd" del mapa del jugador).
 * Causa que corrige: en el gateo el ojo es la articulación de la cabeza (~0,45 m por delante del centro del cuerpo) y el juego solo colisiona el CENTRO del cuerpo, así que en un conducto
 * (1 m de ancho) la cabeza atraviesa la pared (medido en volcado: ojo 4 uds al otro lado, ver el mundo exterior). Regla general: el centro del cuerpo está siempre en el lado libre de cada
 * pared; el ojo se empuja por la normal de la pared hasta quedar a `r` de ella en ese mismo lado (sin pedir más holgura de la que tiene el cuerpo). Solo lee la RAM.
 * Datos (EXE EU, hallados en la función de línea de visión 0x80028D14): CONTROL jugador (0x800ACB84) +0x2C -> mapa +8 -> envoltura +0 -> cabecera (+10 s16 nº grupos, +16 grupos de 24 B);
 * grupo: +2 s16 nº paredes, +8 puntero a paredes de 16 B {x1,z1,y,alto, x2,z2,y,alto} (s16), +20 bytes de banderas; máscara de grupos activos en 0x800ACB38. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Empuja (ex, ez) (unidades del motor) fuera de r unidades de cada pared activa cuya franja vertical contenga ey, en el lado del cuerpo (bx, bz).
 * Devuelve cuántas correcciones aplicó (0 = ojo libre o sin datos válidos: no cambia nada). */
int vrhzd_clamp_eye(const uint8_t *ram, int32_t bx, int32_t bz, float *ex, float *ez, float ey, float r);

#ifdef __cplusplus
}
#endif
