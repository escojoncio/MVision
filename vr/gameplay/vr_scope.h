/* vr_scope: geometría de la mira óptica del francotirador del juego (retícula y abertura) y su campo de visión.
 *
 * Medido del original (listas de dibujo del intérprete, edición europea):
 *  - Con la mira el juego sube la distancia de pantalla de 320 a 3000 (aumento 3000/320 = 9,375x) y dibuja sobre el lienzo de
 *    320 x 256 (centro 160,128) dos cosas: una abertura casi circular (114 polígonos de marco que cierran la imagen fuera de ella:
 *    celdas abiertas de x 32..285 y 2..253 del lienzo, es decir, radio ~127 x ~125 px con esquinas algo cuadradas) y la retícula, que son
 *    primitivas 2D negras de 1 px (cruz fina de +-64 px) y cuatro postes gruesos (~5 px) desde 67 px hasta el borde del lienzo.
 *  - Aquí se reproduce esa geometría en una textura cuadrada que cubre el lienzo en vertical (256 px de lienzo = el lado), con
 *    transparencia en el interior de la abertura: se pone encima de la imagen aumentada que renderiza la app desde el visor del
 *    arma (un segundo pase de escena con campo vertical vrscope_fov_y). No se usan los gráficos del juego (la retícula del juego
 *    sigue en su panel 2D plano, que la app oculta mientras se mira por la mira VR).
 * Solo C puro, sin datos del juego. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VRSCOPE_CANVAS_W 320.0f
#define VRSCOPE_CANVAS_H 256.0f
#define VRSCOPE_BASE_H   320.0f     /* distancia de pantalla normal del juego */
#define VRSCOPE_GAME_H   3000.0f    /* con la mira */
#define VRSCOPE_ZOOM     (VRSCOPE_GAME_H / VRSCOPE_BASE_H)

/* Campo vertical (rad) de la textura cuadrada para un aumento dado (relativo a la vista normal del juego). */
float vrscope_fov_y(float zoom);
/* Lienzo -> coordenada de la textura (0..1) y al revés. La textura cubre el lienzo en vertical y 256 px en horizontal (de x=32 a x=288). */
void vrscope_canvas_to_uv(float cx, float cy, float *u, float *v);
/* 1 si el punto del lienzo está dentro de la abertura. */
int vrscope_inside(float cx, float cy);
/* 1 si el punto del lienzo está sobre la retícula. */
int vrscope_on_reticle(float cx, float cy);
/* Dibuja abertura + retícula en una textura RGBA cuadrada (R | G<<8 | B<<16 | A<<24): fuera de la abertura negro opaco; dentro, transparente
 * salvo la retícula (casi negra, opaca). size 16..2048; fuera de rango no hace nada. */
void vrscope_draw_overlay(uint32_t *rgba, int size);

#ifdef __cplusplus
}
#endif
