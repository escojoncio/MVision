#pragma once
/* probe_view: render de la escena de vr_scene por CPU (texturas de la VRAM del oráculo, semitransparencia, profundidad) con una pose de cabeza dada.
 * Solo para el probador: reproduce lo que vería el visor con el campo de visión pedido, para compararlo con el fotograma real del juego. */
#include <stdint.h>
#include "vr_scene.h"
typedef struct { float p[3], R[9]; int w, h; float cx, cy, f; } PvCam;   /* R: filas = der, arriba, atrás (en el mundo); f en píxeles; imagen y hacia abajo */
/* head: 16 floats columna-mayor (X der, Y arriba, Z atrás, posición); fov en grados (cuadrado). */
void pv_cam_head(PvCam *c, const float head[16], float fov_deg, int w, int h);
/* La cámara del propio motor (VrScene.game_view/filter): w x h con el aspecto 320x256 del fotograma original. */
void pv_cam_engine(PvCam *c, const VrScene *sc, int w, int h);
/* rgb: w*h*3. fondo = (bg,bg,bg). near en m. */
/* tsrc/keep (opcionales): índice de op de origen por triángulo (vr_scene_set_tri_src) y máscara por op (0 = no dibujar). */
void pv_render(const VrScene *sc, const PvCam *cam, uint8_t *rgb, uint8_t bg, float near_m, const uint32_t *tsrc, const uint8_t *keep, uint32_t nkeep);
