/* Puente C -> Swift del banco de render: el mismo que la app + los datos sintéticos de bench/c/bench_host.c. */
#include "Bridging-Header.h"   /* app/Sources/Bridging-Header.h */

void bench_make_pattern(void);   /* fotograma 2D 320x240 de prueba (pantalla de cine) */
void bench_touch_frame(void);
void bench_make_scene(void);     /* escena VR sintética: suelo, cubos, sprite 2D */
void bench_scene_post(void);     /* la entrega como escena nueva en el siguiente mv_host_scene */
void bench_enable_radar(int on);
void bench_enable_weapon(int on);
void bench_set_cine_camera(int on);   /* game_view de la escena = cámara del juego a 1,6 m mirando a -Z (pantalla 3D) */
