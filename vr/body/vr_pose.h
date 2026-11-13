/* vr_pose: contrato de poses de las manos del usuario para la capa del cuerpo (vr/body). C99 puro.
 *
 * ESPACIO. El mismo que la pose de la cabeza de mv_vr_get_head (runtime/host.h): metros, Y arriba, -Z delante,
 * origen recentrado. Si la cabeza y las manos no comparten origen, vr/body no puede unirlas: el integrador debe
 * entregar ambas en el mismo espacio (el de la sesión de visionOS ya recentrado).
 *
 * MARCO DE LA MANO (rot = cuaternión x,y,z,w, de marco de la mano a espacio VR; mano dextrógira como la cabeza):
 *   - origen (pos) = centro de la palma (punto de agarre, como la "grip pose" de OpenXR);
 *   - -Z = hacia donde apuntan los dedos con la mano abierta y plana (de la muñeca a los nudillos);
 *   - +Y = hacia el dorso de la mano (la palma mira a -Y);
 *   - +X = Y x Z. (Mano derecha con la palma abajo y los dedos al frente: el pulgar queda hacia -X; en la
 *     izquierda hacia +X.)
 * Si el mando que se sostiene tiene otro marco, se corrige con VrBodyConfig.hand_rot_offset (vr_body.h), no aquí.
 *
 * ÍNDICE. out[0] = mano izquierda, out[1] = mano derecha (izquierda y derecha del USUARIO, no del dominio).
 *
 * IMPLEMENTACIÓN. `vrpose_get_hands` es débil (vr_pose.c devuelve 0 = sin manos). El integrador la define en
 * runtime/host_vr.c (símbolo fuerte) a partir de VRInputPose (Swift): rellena `out` con la última pose muestreada
 * (thread-safe: se llama desde el hilo del emulador) y devuelve el nº de manos válidas (0..2). Una mano sin
 * seguimiento se marca valid = 0 (el resto de campos se ignoran); vr/body funde entonces su brazo con la
 * animación original en ~0,25 s. Poses con valores no finitos se tratan como no válidas. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int   valid;       /* 1: pose válida */
    float pos[3];      /* m */
    float rot[4];      /* cuaternión x, y, z, w (no hace falta que esté normalizado) */
    int   aim_valid;   /* 1: `aim_rot` es la orientación del APUNTADO del mando (ubicación "aim", -Z hacia donde apunta), en el mismo espacio. Con ella vr/body deduce el
                        * cabeceo fijo entre el agarre y el apuntado (el mango del mando de seguimiento está inclinado ~35 grados) y endereza la mano del personaje: con el mando apuntando
                        * horizontal, los dedos del personaje quedan horizontales. 0 = sin apuntado (se usa VrBodyConfig.hand_pitch_deg). */
    float aim_rot[4];
    /* Calibración de manos (vrbody_calibrate_hands): parte del agarre REAL del mando, nunca de una pose ya corregida. `derived` = 1: pos/rot de esta mano no son los de su mando (mano de apoyo soldada
     * al arma, que cuelga de la mano que empuña); entonces la calibración usa `raw_rot` (agarre de su propio mando, solo si raw_valid = 1) y, sin él, esa mano no se calibra. derived = 0: rot ya es el del mando. */
    int   derived;
    int   raw_valid;
    float raw_rot[4];
} VrHandPose;

/* Rellena out[0] (izquierda) y out[1] (derecha). Devuelve cuántas son válidas. Débil: por defecto 0 manos. */
int vrpose_get_hands(VrHandPose out[2]);

#ifdef __cplusplus
}
#endif
