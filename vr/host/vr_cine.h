#pragma once
/* vr_cine: planos (cortes de cámara) de las cinemáticas del motor y cámaras estéreo de la pantalla 3D.
 *
 * Planos. En una cinemática la cámara del juego la lleva el guion (la cabeza del usuario NO la toca: vrcam solo manda con control del
 * jugador). `vrcine_camera` se llama con la cámara con la que se dibujó cada lista (VrCamRecord: ojo + ejes) y decide si es un CORTE
 * (salto de posición o de orientación entre dos listas consecutivas imposible para un movimiento continuo) o continúa el plano. Cada plano
 * tiene un ORIGEN fijo (el ojo de su primer fotograma): ensamblando la escena con ese origen las coordenadas del mundo VR no cambian dentro
 * del plano aunque el personaje se mueva (con el origen "bajo el jugador por fotograma" todo el mundo se desplazaba con él).
 *
 * Estéreo. `vrcine_eye` da, para la cámara del juego (VrScene.game_view) y un ojo, la matriz de vista y las tangentes del campo de visión de
 * una pareja de cámaras PARALELAS con ventana desplazada (frustos asimétricos, sin convergencia): el plano a `conv` metros de la cámara
 * queda sin paralaje (en el plano de la pantalla), lo más cercano sale de ella y lo lejano queda detrás. Si el campo de visión de las
 * cámaras es el ángulo que la pantalla ocupa desde el usuario y la separación es su IPD, la escena se ve a escala real (ortoestereoscopía).
 * La cabeza del usuario no interviene: mover la cabeza solo cambia hacia dónde de la pantalla se mira. C99 puro; lo prueba test_cine.c;
 * XRPresent.swift (XRStereo) lo replica función por función: mantener sincronizado. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float cut_dist;        /* unidades del motor: salto de posición entre listas que se considera corte (defecto 2500) */
    float cut_deg;         /* grados: giro de la cámara entre listas que se considera corte (defecto 28) */
} VrCineConfig;

typedef struct {
    VrCineConfig cfg;
    int have;              /* hay cámara previa del plano */
    int32_t prev_eye[3];
    float prev_fwd[3], prev_up[3];
    int32_t origin[3];     /* ojo del primer fotograma del plano */
    uint32_t shot;         /* nº de plano (>= 1 tras el primero) */
    uint32_t cuts;         /* cortes detectados (sin contar el primer plano de cada cinemática) */
    int cut_now;           /* la última cámara empezó un plano nuevo */
    uint32_t cams;         /* cámaras vistas */
    float last_jump;       /* salto de posición de la última cámara (unidades) */
    float last_turn;       /* giro de la última cámara (grados) */
} VrCine;

void vrcine_config_default(VrCineConfig *c);
void vrcine_reset(VrCine *s);
/* Cámara con la que se dibujó una lista: eye (unidades), M = vista->mundo fila-mayor M[3*fila+col], columnas = ejes X derecha, Y abajo, Z delante.
 * `active` = 0 cuando la lista no es de una cinemática (se descarta el plano en curso; la siguiente empieza otro). Devuelve 1 si esta cámara
 * empieza un plano nuevo (también la primera). */
int vrcine_camera(VrCine *s, int active, const int32_t eye[3], const float M[9]);

/* ---- estéreo ---- */
typedef struct {
    float view[16];        /* mundo VR (m) -> espacio del ojo (GL: x der, y arriba, -z delante), columna-mayor */
    float tan_[4];         /* tangentes del frusto: izquierda, derecha, arriba, abajo (positivas) */
} VrCineEye;
/* game_view: VrScene.game_view (columna-mayor, vista del motor en unidades -> metros). eye: 0 izquierdo, 1 derecho. ipd y conv en metros;
 * tan_x/tan_y = semiángulo del campo de visión (tangentes) de la imagen. Devuelve 0 si game_view no es invertible. */
int vrcine_eye(const float game_view[16], int eye, float ipd, float conv, float tan_x, float tan_y, VrCineEye *out);
/* Proyección de pantalla: (x,y) en [0,1] x [0,1] (y hacia abajo) de un punto del espacio del ojo; devuelve 0 si está detrás. */
int vrcine_project(const VrCineEye *e, const float p[3], float *u, float *v, float *depth);

#ifdef __cplusplus
}
#endif
