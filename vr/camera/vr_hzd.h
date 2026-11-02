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

/* Lectura de rayos contra las mismas paredes (láser de la pistola, vr/weapons/w_laser.h). Vive en vr_hzd_ray.c (unidad propia, sin vr_camera.c: la usa vr/host). */
#define VRHZD_SKIP_SIGHT 4u      /* máscara de paredes ignoradas con la que el juego hace su prueba de línea de visión (enemigos y cámaras; callers de 0x80028D14) */
/* Primer impacto del rayo o + t*d (mundo del motor, unidades del motor; d se normaliza) con las paredes activas de colisión del juego cuya franja vertical contenga el punto de
 * impacto, t en (0, max_units]. Se ignoran las paredes con (bandera & skip) != 0: es la misma comparación de la función de línea de visión del juego, que recibe esa máscara en la pila
 * (a3 & 4 activa las paredes, a3 & 8 los objetos; los objetos no se leen aquí). Devuelve t en unidades del motor, o -1 si no hay impacto en el alcance / sin datos HZD / entradas no
 * finitas. Solo lee la RAM. */
float vrhzd_raycast(const uint8_t *ram, const float o[3], const float d[3], float max_units, unsigned skip);

#ifdef __cplusplus
}
#endif
