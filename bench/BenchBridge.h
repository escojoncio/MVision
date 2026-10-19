/* Puente C -> Swift del banco de render: el mismo que la app + los datos sintéticos de bench/c/bench_host.c. */
#include "Bridging-Header.h"   /* app/Sources/Bridging-Header.h */

#include "c/native_synth.h"   /* escena nativa sintética (la misma del banco del renderer nativo) */
void bench_set_native_marks(int on);   /* la lista de la GPU marca sus batches 3D (skip_if_native) y declara native_3d */
void bench_make_native_e2e_scene(const uint16_t *vram);   /* escena de extremo a extremo: un cuadrilátero rojo enorme delante de la cabeza (0.3, 1.6, 0.5) */
void bench_make_pattern(void);   /* fotograma 2D 320x240 de prueba (pantalla de cine) */
void bench_touch_frame(void);
void bench_make_scene(void);     /* escena VR sintética: suelo, cubos, sprite 2D */
void bench_scene_post(void);     /* la entrega como escena nueva en el siguiente mv_host_scene */
void bench_enable_radar(int on);
void bench_enable_weapon(int on);
void bench_set_cine_camera(int on);   /* game_view de la escena = cámara del juego a 1,6 m mirando a -Z (pantalla 3D) */
void bench_set_radar_occluder(int on, float x, float y, float z, float size);   /* cubo ciano opcional delante del radar de muñeca (se emite en bench_make_scene) */
void bench_smooth_config(int enabled, float lead_s);   /* fluidez: vr_smooth con ese adelanto (s); <0 = el de defecto */
void bench_smooth_set_time(double t, double turn_req_deg);
void bench_publish_geom(double ox_units, double oz_units, double turn_deg);   /* misma geometría que bench_publish_motion pero sin muestra para el suavizador (referencia ideal) */
void bench_publish_motion(double t_wall, double game_s, double ox_units, double oz_units, double turn_deg);   /* escena nueva: personaje en O, giro del stick turn_deg */
