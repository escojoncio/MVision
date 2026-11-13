/* vr_hand_pose: poses de la mano nueva (vr/body/vr_hand_skin) a partir de la entrada analógica de un mando (gatillo, agarre, botones; sin seguimiento de dedos).
 *
 * Cada pose es una tabla de cierres por dedo (índice, corazón, anular, meñique, pulgar; 0 abierto .. 1 puño) y una separación del pulgar; el cierre de un dedo se reparte
 * entre sus articulaciones con los ángulos de vr_hand_skin (vrhs_pose). Mezcla (vrhandpose_blend):
 *   base    = lerp(HP_RELAX -> pose del objeto, agarre)
 *   índice  = lerp(base -> índice tirado de la pose, gatillo), solo si la pose tiene gatillo (HP_TRIGGER, HP_GRIP_PISTOL, HP_GRIP_RIFLE)
 *   pulgar  = lerp(base -> pulgar levantado y abierto, botón del pulgar)
 *   sin objeto (item_pose = HP_RELAX): la pose destino es HP_FIST y el botón de puño (fist_btn) la fuerza: agarre efectivo = max(agarre, fist_btn).
 *   Con objeto (HP_GRIP_*, HP_OPEN, HP_POINT, HP_FIST, HP_TRIGGER) fist_btn se ignora.
 * Todas las entradas se acotan a 0..1 (NaN/inf -> 0); una pose fuera de rango cuenta como HP_RELAX. Determinista y sin estado.
 * La yema del índice en HP_TRIGGER y HP_GRIP_* (gatillo suelto) cae en la zona de gatillo (ver test_hand_pose.c); vr_grip.c coloca el arma contando con ello. */
#pragma once
#include "vr_hand_skin.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HP_OPEN = 0, HP_RELAX, HP_FIST, HP_POINT,
    HP_TRIGGER,           /* puño con el índice libre sobre el gatillo (sin objeto en la mano) */
    HP_GRIP_PISTOL,       /* empuñadura de pistola, índice en el gatillo */
    HP_GRIP_RIFLE,        /* empuñadura de pistola de un arma larga, índice en el gatillo, pulgar rodeándola */
    HP_GRIP_SUPPORT,      /* mano de apoyo bajo el guardamanos o delante de la empuñadura */
    HP_GRIP_SPHERE,       /* granada */
    HP_GRIP_PLACED,       /* mina / C4 / detonador */
    HP_GRIP_BOX,          /* caja (cargador, objeto genérico) */
    /* Poses por arma y mano, por ángulos de articulación (tabla generada offline contra la malla del arma: vr_hand_grips.inc, ver gripfit_gen.c). _R = mano que empuña (con gatillo), _L = mano de apoyo. */
    HP_W_SOCOM_R, HP_W_SOCOM_L, HP_W_FAMAS_R, HP_W_FAMAS_L, HP_W_PSG1_R, HP_W_PSG1_L, HP_W_STINGER_R, HP_W_STINGER_L, HP_W_NIKITA_R, HP_W_NIKITA_L,
    HP_W_FRAG_R, HP_W_STUN_R, HP_W_CHAFF_R,   /* granadas (mano derecha, sin gatillo): rodean el modelo del juego que lleva la mano (grenade.c: grenade 0x3b88; aturdidora y señuelo can_gren 0x7a64) */
    HP_W_CLAYMORE_R,      /* claymore (id 5, mano derecha, sin gatillo): palma en la cara trasera central y dedos por encima del canto superior del modelo del juego 0x17a1 */
    HP_COUNT
};
#define HP_W_FIRST HP_W_SOCOM_R

typedef struct {
    int item_pose;        /* HP_*; HP_RELAX = sin objeto */
    float grip;           /* agarre analógico 0..1 */
    float trigger;        /* gatillo analógico 0..1 */
    float thumb_btn;      /* pulgar sobre un botón de cara 0..1 (levanta el pulgar) */
    float fist_btn;       /* botón de puño 0..1 (solo sin objeto) */
} VrHandPoseIn;

/* Cierres resultantes antes de aplicar el esqueleto. */
typedef struct {
    float curl[5];        /* índice, corazón, anular, meñique, pulgar: 0..1 */
    float thumb_spread;   /* -1 .. 1 (vrhs_pose) */
} VrHandCurls;

int vrhandpose_has_trigger(int pose);                                  /* 1 si el objeto de esta pose tiene gatillo (el índice sigue la entrada de gatillo) */
void vrhandpose_curls(const VrHandPoseIn *in, VrHandCurls *out);       /* la mezcla sin esqueleto; in NULL = mano relajada */
/* Lo mismo por ángulos de articulación (vrhs_pose_ang): para HP_W_* los de la tabla por arma (base = lerp(HP_RELAX -> pose, agarre); índice -> índice tirado con el gatillo en las poses _R; pulgar levantado con thumb_btn);
 * para el resto, los cierres de vrhandpose_curls convertidos (idéntico a vrhs_pose). */
void vrhandpose_angles(const VrHandPoseIn *in, float ang[VRHS_NANG]);
/* Pose de las articulaciones (vrhs_pose) de la mano side (+1 derecha, -1 izquierda). */
void vrhandpose_blend(int side, const VrHandPoseIn *in, float xf[VRHS_J][12]);
/* Yema del índice (vrhs_index_pad) en el marco de la mano (mm). */
void vrhandpose_index_tip(const float xf[VRHS_J][12], float out[3]);

#ifdef __cplusplus
}
#endif
