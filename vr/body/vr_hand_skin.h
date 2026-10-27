/* vr_hand_skin: mano del personaje con skin lineal sobre el esqueleto OpenXR de 25 articulaciones de vr_hand_asset (mano genérica de WebXR Input Profiles, MIT). C99, matemática pura.
 *
 * Sustituye a la mano procedural de huesos rígidos: la malla es una sola superficie continua (sin dedos de palo al cerrar) que se deforma en la CPU (skin lineal, <= 4 huesos por vértice).
 *
 * Marco de la mano = el de la pieza mano del jugador (unidades del motor; origen en la muñeca del modelo; dedos -Y; pulgar +Z; palma +X en la mano DERECHA y -X en la izquierda). La
 * izquierda es el ESPEJO EXACTO en X de la derecha (posiciones y normales con x -> -x; quien dibuja invierte el orden de los triángulos; las poses se conjugan con el espejo).
 * Tamaño: a la escala del BRAZO DEL JUEGO. El asset (mano real de ~190 mm) se escala por VRHS_FIT_K y se desplaza VRHS_FIT_O para que el ancho de su puño (pulgar-meñique) y su centro
 * coincidan con el anillo final del antebrazo del modelo del jugador (medido en el volcado: x -20..35, z -49..18 a 22 unidades sobre la muñeca); las proporciones no cambian.
 * Todas las salidas (vrhs_pose, vrhs_skin, centro de agarre, yema, nudillos) van ya ajustadas. VRHS_UNIT_M = metros por unidad con units_per_m = 1000.
 *
 * Pose: cierre por dedo (curl[0..3] índice, corazón, anular, meñique; curl[4] pulgar; 0 = reposo del asset, 1 = puño) y separación del pulgar (-1 pegado .. +1 abierto). Cada
 * articulación gira alrededor de los ejes de su marco de REPOSO (convención OpenXR: -Z hacia la punta, +Y dorso, X = eje de flexión); el ángulo es lineal con el cierre. Sin estado. */
#pragma once
#include "vr_hand_asset.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VRHS_J VRHA_NJ
#define VRHS_UNIT_M 0.001f       /* metros por unidad del marco de la mano (units_per_m = 1000) */
#define VRHS_FIT_K 1.27f         /* escala asset -> brazo del juego: ancho del puño del asset 52,6 mm -> 67 unidades del anillo final del antebrazo */
extern const float VRHS_FIT_O[3];   /* desplazamiento asset -> pieza mano: centra el puño en el anillo del antebrazo (8,3; 0; -14,2) */
#define VRHS_HANDLE_R_MM 17.0f   /* radio del mango de referencia del centro de agarre (mando, pistola) */
#define VRHS_HANDLE_TILT_DEG 30.0f   /* el mango cruza la palma en diagonal (agarre de fuerza): del meñique hacia la base del índice, inclinado hacia los dedos */

enum {
    VRHS_WRIST = 0,
    VRHS_THUMB_MC = 1, VRHS_THUMB_PROX = 2, VRHS_THUMB_DIST = 3, VRHS_THUMB_TIP = 4,
    VRHS_INDEX_MC = 5, VRHS_INDEX_PROX = 6, VRHS_INDEX_MID = 7, VRHS_INDEX_DIST = 8, VRHS_INDEX_TIP = 9,
    VRHS_MIDDLE_MC = 10, VRHS_RING_MC = 15, VRHS_LITTLE_MC = 20      /* + 1 proximal, + 2 media, + 3 distal, + 4 punta */
};

/* Pose: xf[j] = articulación j -> marco de la mano (3x4 fila-mayor, v' = R v + t, mm). side +1 derecha, -1 izquierda. Entradas acotadas (no finitas = 0). */
void vrhs_pose(const float curl[5], float thumb_spread, int side, float xf[VRHS_J][12]);
/* Matrices de skin (xf[j] * inversa de reposo): las que mueven los vértices de reposo de la mano DERECHA a la pose. Para side -1 se deshace el espejo de xf. */
void vrhs_skin_mats(const float xf[VRHS_J][12], int side, float sk[VRHS_J][12]);
/* Vértices (VRHA_NV) en el marco de la mano: posición (mm) y normal unitaria HACIA FUERA; nrm puede ser NULL. Para side -1, espejo X. */
void vrhs_skin(const float xf[VRHS_J][12], int side, float (*pos)[3], float (*nrm)[3]);
/* Centro de agarre (marco de la mano, mm): sobre el eje de un mango de radio VRHS_HANDLE_R_MM apoyado en la palma, a la altura del dedo corazón (65 % de su metacarpo). Es el punto
 * del mando (pose de agarre) y el punto de empuñadura de las armas (vr_grip). */
void vrhs_grip_center(int side, float out[3]);
/* Eje del mango en el marco de la mano (unitario, hacia el lado del pulgar): +Z inclinado VRHS_HANDLE_TILT_DEG hacia los dedos. */
void vrhs_handle_axis(int side, float out[3]);
/* Yema del índice (cara palmar de la falange distal) con la pose xf. */
void vrhs_index_pad(const float xf[VRHS_J][12], float out[3]);
/* Punto de golpe: nudillos (centro de los vértices más avanzados hacia -Y) con el puño cerrado. */
void vrhs_fist_point(int side, float out[3]);

#ifdef __cplusplus
}
#endif
