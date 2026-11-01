/* vr_body: los brazos del personaje que se ven al mirar hacia abajo siguen las manos del usuario (IK de dos huesos por
 * brazo) con el modelo ORIGINAL del jugador. C99; el solver es matemática pura, el pegamento lee/escribe la RAM del
 * juego (EXE principal, edición europea) y está acotado a las matrices de 6 piezas del modelo.
 *
 * MODELO DEL JUGADOR EN RAM (verificado en partida; detalle y direcciones en bitacora.md y en vr_body.c):
 *   puntero de cuerpo 0x800ACBB0 -> objeto {+0 puntero al conjunto de piezas}; conjunto: nº de piezas s16 +0x2E (16),
 *   modo de visibilidad s16 +0x32 (0 = fuera de pantalla), piezas de 0x5C bytes desde +0x48. Pieza: world (matriz 3x3 s16 + t) +0x00 (s16 m[3][3],
 *   4096 = 1, pad, s32 t[3] en +0x14), screen (matriz igual) +0x20 (= vista x world), model* +0x40, modo de visibilidad s16 +0x4C.
 *   Modelo de pieza: t (origen respecto al padre, mm) +0x20, parent s32 +0x2C. Mundo: Y arriba, derecho;
 *   el personaje mira a +Z, su izquierda es +X; unidad = 1 mm.
 *   Piezas: 0 cadera, 1 torso, 2/3/4 = brazo/antebrazo/mano DERECHOS (los del arma), 5 cuello, 6 cabeza, 7/8/9 =
 *   brazo/antebrazo/mano IZQUIERDOS, 10-12 pierna derecha, 13-15 pierna izquierda. La mano es UNA pieza (manopla abierta, sin variante de puño en el juego): con el agarre cerrado su malla se deforma en un puño (vr_fist.h).
 *   Huesos: hombro = origen de la pieza 2 (7); codo = origen de la 3 (8): (0,-260,-50); muñeca = origen de la 4 (9):
 *   (0,-249,47) respecto al codo. Longitudes 264,8 y 253,4 mm. La pieza mano mide ~125 mm bajo la muñeca.
 *
 * CUÁNDO. El motor anima (ángulos -> matrices world/screen de todas las piezas) en la etapa 1 del dibujado
 * (pantalla), calcula visibilidad (2), transforma vértices (3, 0x8001e840 = sustituto vrcam_trans), ilumina (4) y
 * ordena (7). vrbody_apply se llama al principio de la etapa 3 (después de que el juego haya animado y antes de
 * transformar), sustituye world+screen de las 3 piezas de cada brazo y deja la copia original; vrbody_restore (tras
 * el dibujado, 0x80018304) las devuelve a su valor, de modo que la lógica del juego (ataques, armas, efectos,
 * objetos enganchados a la mano, enemigos) nunca ve la pose IK: solo la ve el dibujado. Ver el diff en el informe.
 *
 * ENTRADA. vrpose_get_hands (vr_pose.h) en el espacio de la cabeza (m, Y arriba, -Z delante). La relación espacio de
 * usuario <-> mundo del motor la da VrBodyView (vrbody_attach_vrcam() la toma de vr/camera: ojo del motor, p_ref y
 * giro del registro de cámara del frame): W = ojo + u * T (P - p_ref), T = R_y(theta + 180 grados).
 *
 * No es reentrante ni multihilo: se llama desde el hilo del emulador (sustitutos del motor). */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "vr_pose.h"
#include "vr_fist.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- solver (matemática pura) ----------------------- */
/* Vectores float[3]; matrices 3x3 float[9] fila-mayor m[3*fila+col]; las COLUMNAS son los ejes locales en el mundo
 * (v_mundo = M * v_local). Unidades: mm (el solver no depende de la escala). */

typedef struct VrArmRig {
    float side;              /* +1 brazo izquierdo del personaje (hombro hacia +X del torso), -1 derecho */
    float elbow_off[3];      /* origen del codo en el marco del brazo superior (mm) */
    float wrist_off[3];      /* origen de la muñeca en el marco del antebrazo (mm) */
} VrArmRig;

typedef struct VrArmLimits {
    float elbow_min_deg;     /* ángulo interior mínimo del codo (flexión máxima = 180 - esto); defecto 32 */
    float reach_margin;      /* fracción de L1+L2 que se usa como alcance máximo; defecto 0.999 */
    float shoulder_back;     /* componente "hacia delante" mínima del brazo superior (marco del torso, unitario); defecto -0.80 */
    float shoulder_cross;    /* componente "hacia fuera" mínima (negativa = cruza el pecho); defecto -0.85 */
    float wrist_swing_deg;   /* SIN USO (la mano lleva siempre la orientación del mando); se conserva por compatibilidad */
    float forearm_share;     /* parte del giro (pronación) que asume el antebrazo, 0..1; defecto 1 (el antebrazo gira con la mano: brazo y mano continuos) */
    float forearm_twist_deg; /* giro máximo del antebrazo sobre su eje; defecto 180 (sin tope efectivo: el exceso sobre +-100 grados lo absorbe el giro del codo, swivel) */
    float hand_twist_deg;    /* SIN USO (la mano absorbe todo el giro sobrante); se conserva por compatibilidad */
    float wrist_flex_deg, wrist_ext_deg;   /* flexión (palma) y extensión (dorso) máximas de la muñeca; defecto 80 / 70 */
    float wrist_rad_deg, wrist_uln_deg;    /* desviación radial (pulgar) y cubital máximas; defecto 20 / 30 */
    float swivel_max_deg;    /* giro máximo del codo alrededor de la recta hombro-muñeca respecto al codo polar para respetar esos límites; defecto 90 (0 = codo polar fijo) */
} VrArmLimits;

enum {
    VRARM_STRETCHED = 1,       /* objetivo fuera de alcance: brazo extendido, mano en el punto alcanzable más cercano */
    VRARM_TOO_CLOSE = 2,       /* objetivo más cerca que la flexión máxima del codo */
    VRARM_SHOULDER_LIMIT = 4,  /* se aplicó el límite del hombro */
    VRARM_WRIST_LIMIT = 8,     /* la muñeca quedó fuera de sus límites aun girando el codo, o el giro del antebrazo quedó recortado por forearm_twist_deg; la mano nunca se limita */
    VRARM_POLE_FALLBACK = 16   /* el vector polar era paralelo al brazo: se usó una alternativa */
};

typedef struct VrArmIn {
    float shoulder[3];       /* origen del hombro (mm, mundo) */
    float torso[9];          /* marco del torso: columnas X izquierda, Y arriba, Z delante */
    float wrist[3];          /* objetivo de la muñeca (mm, mundo) */
    float hand[9];           /* orientación objetivo de la pieza mano (columnas = ejes locales de la mano, en mundo) */
    float pole[3];           /* hacia dónde apunta el codo (mundo; se proyecta perpendicular al brazo) */
    int has_prev;            /* 1: tau_prev/tau_f_prev/swivel_prev son los del cálculo anterior de este brazo (giro del antebrazo y codo continuos, sin saltos) */
    float tau_prev, tau_f_prev;   /* VrArmOut.tau y .tau_f del cálculo anterior (rad) */
    float swivel_prev;       /* VrArmOut.swivel del cálculo anterior (grados) */
} VrArmIn;

typedef struct VrArmOut {
    float elbow[3], wrist[3];            /* posiciones resultantes (mm, mundo) */
    float R_upper[9], R_fore[9], R_hand[9]; /* orientación de las piezas brazo, antebrazo y mano */
    float reach;                         /* distancia pedida hombro->muñeca / (L1 + L2) */
    float err_mm;                        /* distancia entre el objetivo de muñeca y la muñeca resultante */
    float tau, tau_f;                    /* giro (rad) mano respecto al antebrazo, desenvuelto de forma continua, y la parte que asume el antebrazo (con su límite) */
    float swivel;                        /* giro del codo (grados) alrededor de la recta hombro-muñeca respecto al codo polar, elegido para la muñeca y el giro del antebrazo */
    float stretch;                       /* escala de ambos huesos a lo largo de su eje (>= 1): objetivo más lejos que el alcance => brazo estirado, muñeca en el objetivo */
    unsigned flags;                      /* VRARM_* */
} VrArmOut;

void vrbody_limits_default(VrArmLimits *l);
/* Alarga la matriz de un hueso (R de rotación pura que lleva `off` a la dirección del hueso) a lo largo de su eje local: R' = R (I + (s-1) l l^T), l = off/|off|. s <= 1 no hace nada.
 * Es la que usan el apply y la ruta de la mano viva para que la malla alcance al hueso siguiente cuando el brazo se estira. */
void vrbody_stretch_bone(float R[9], const float off[3], float s);
/* 0 = bien; -1 = entrada no finita o rig inválido (out sin tocar). Longitud de huesos: |elbow_off|, |wrist_off|. */
int vrbody_solve_arm(const VrArmRig *rig, const VrArmLimits *lim, const VrArmIn *in, VrArmOut *out);

/* Utilidades de matemática (expuestas para pruebas) */
void vrbody_quat_to_mat(const float q[4], float m[9]);                 /* q = x,y,z,w; se normaliza (no finita -> identidad) */
void vrbody_mat_to_quat(const float m[9], float q[4]);                 /* m ortonormal (se ortonormaliza antes) */
void vrbody_orthonormalize(const float m[9], float out[9]);

/* ------------------------------------------------------------------ vista y configuración ----------------------- */
typedef struct VrBodyView {
    int active;              /* 1: la cámara VR manda este frame (si no, no se toca nada) */
    float eye[3];            /* ojo del motor (mundo, unidades del motor) */
    float p_ref[3];          /* posición de la cabeza (m, espacio del usuario) a la que corresponde `eye` */
    float theta_deg;         /* giro espacio de usuario -> mundo del motor: T = R_y(theta + 180 grados) */
    float units_per_m;       /* 1000 */
    float gaze_yaw_deg;      /* guiñada de la mirada REAL del usuario en el mundo del motor (misma escala que el rumbo del cuerpo: 0 = +Z, + hacia +X); la usa torso_follow */
    float shift[3];          /* desplazamiento (mundo del motor) con que la cámara apartó el ojo del modelo (paredes, gateo): todo el cuerpo del jugador se dibuja desplazado igual para no quedar por delante del ojo; 0 = sin empuje */
} VrBodyView;
typedef int (*VrBodyViewFn)(VrBodyView *out);    /* devuelve 1 y rellena `out` si hay vista; 0 si no */
void vrbody_set_view_fn(VrBodyViewFn fn);
/* Usa la cámara VR de vr/camera como fuente de la vista (vr_body_cam.c). Llamar una vez tras vrbody_init. */
void vrbody_attach_vrcam(void);

typedef struct VrBodyConfig {
    int enabled;                 /* 1 (defecto) */
    int dominant;                /* mano que empuña el arma: 1 derecha (defecto), 0 izquierda */
    /* piezas del modelo [0] = izquierda, [1] = derecha: hombro, codo, muñeca */
    int part_arm[2][3];          /* {{7,8,9},{2,3,4}} */
    int part_torso;              /* 1 */
    int part_neck;               /* 5: hijo del torso; su origen (cuello) queda fijo al enderezar el torso (torso_follow) */
    int min_parts;               /* 16: modelos con menos piezas se ignoran */
    /* mano */
    float grip_to_wrist_mm;      /* SIN USO: la muñeca sale del centro de agarre de la mano nueva (vrhs_grip_center, a tamaño real: mm * units_per_m / 1000); se conserva por compatibilidad */
    float hand_rot_offset[2][4]; /* cuaternión x,y,z,w aplicado a la pose de cada mano (marco del mando -> marco de mano de vr_pose.h). Defecto: giro de hand_roll_deg sobre el eje de los dedos
                                  * (izquierda +, derecha -): el marco de AGARRE de los mandos del visor es el del dispositivo (Y arriba, -Z delante, X lateral; medido: inclinado 35 grados y NO está
                                  * reflejado entre mandos), mientras que el de vr_pose.h es el de la mano plana con la palma abajo (+Y = dorso). Con identidad la mano sale plana "sobre las piernas". */
    float hand_roll_deg;         /* giro de agarre -> mano (defecto 45): al sujetar los mandos la mano queda girada ~45 grados vista desde arriba (palma hacia dentro y abajo); 90 = apretón de manos, 0 = plana */
    float hand_pitch_deg;        /* CABECEO fijo agarre -> mano (grados, giro sobre el eje lateral X del agarre; positivo = los dedos suben). Defecto -35: el marco de agarre del mando de seguimiento está inclinado 35 grados
                                  * respecto al apuntado (medido en el mando de seguimiento: Y=(0,0.82,0.57), Z=(0,-0.57,0.82) en el marco del dispositivo, apuntado = Z del dispositivo), así que con el mando apuntando horizontal los
                                  * dedos del agarre miran 35 grados arriba y se bajan 35. Independiente del alabeo (hand_roll_deg, que gira DESPUÉS sobre el eje de los dedos). */
    int hand_pitch_auto;         /* 1 (defecto): si el mando entrega el apuntado (VrHandPose.aim_valid) el cabeceo se calcula de la propia relación agarre/apuntado (deja la mano alineada con el apuntado); 0 = siempre hand_pitch_deg */
    float curl_deg;              /* giro RÍGIDO de la pieza mano con el agarre al máximo; defecto 0 (el cierre lo hace el puño deformado: fist_enabled) */
    int fist_enabled;            /* 1 (defecto): con el agarre cerrado la malla de la mano del personaje se deforma en un puño (vr_fist); 0 = mano rígida */
    float fist_min_grip;         /* agarre (ya suavizado y fundido) a partir del cual se deforma la malla; defecto 0,03 */
    VrFistParams fist;           /* forma del puño (ángulos de las falanges, pulgar...) */
    int own_hands;               /* 0 (defecto): 1 = las piezas brazo/antebrazo/mano de cada lado MOVIDO por la IK (2,3,4 / 7,8,9) se ocultan (visibilidad +0x4C = 0) porque el anfitrión dibuja manos propias; restore las devuelve */
    int torso_follow;            /* 1 (defecto): cuerpo estilo VRIK. La pelvis tiene guiñada propia (zona muerta torso_deadzone_deg respecto a la mirada; al andar se alinea con la dirección de marcha) y arrastra a las piernas del juego (su
                                  * animación de locomoción, intacta, rotada rígidamente con la pelvis); el pecho (torso y cuello fijo) se tuerce hacia la cabeza una fracción del desvío, sin inclinación, para que los hombros no hereden la pelvis del reposo */
    float torso_yaw_tau_s;       /* constante del filtro de la guiñada de la pelvis y el pecho; defecto 0.25 */
    float torso_deadzone_deg;    /* zona muerta de la pelvis: mirada y pelvis no se separan más de esto (grados); la pelvis (con las piernas del juego) solo se arrastra al salir de ella; defecto 40 */
    float torso_chest_share;     /* fracción del desvío mirada-pelvis que el pecho se tuerce hacia la cabeza (columna repartida entre cadera y pecho); defecto 0.5 */
    float torso_chest_max_deg;   /* torsión máxima del pecho respecto a la pelvis; defecto 30 */
    float torso_hand_share;      /* peso (0..1) de la dirección media de las manos, si están delante de la mirada, en la orientación objetivo del torso; defecto 0.3 */
    float torso_walk_mps;        /* velocidad horizontal (m/s) de la cadera del juego a partir de la cual la pelvis se alinea con la dirección de marcha; defecto 0.3 */
    float near_hide_m;           /* 0,30 (defecto; 0 = desactivado): se ocultan (visibilidad +0x4C = 0, restaurable) las piezas del jugador, salvo piernas y pies (10-15) y los brazos que la IK mueve, cuyo centro en el
                                  * mundo queda DELANTE del ojo (proyección sobre la mirada horizontal > 0) y a menos de esta distancia (m): el cuerpo que se inclina al atacar no tapa la vista */
    float near_hide_vtx_m;       /* 0,12 (defecto; 0 = desactivado): ... o con algún vértice a menos de esta distancia (m) del ojo, esté donde esté el centro */
    int force_arm_visible;       /* 1 (defecto): si el motor dejó a 0 la visibilidad de una pieza del brazo movida por la IK, se fuerza durante la transformación (brazos siempre dibujados) */
    /* codo */
    float pole[3];               /* dirección del codo en el marco del torso para el brazo izquierdo: x = hacia fuera, y = arriba, z = delante; el derecho se refleja en x. Defecto (0.45,-1,-0.35) */
    VrArmLimits limits;
    /* tiempo */
    float smooth_tau_s;          /* constante del suavizado de las manos (0 = sin suavizado); defecto 0.02 */
    float frame_dt_s;            /* duración de un frame del motor (0.02 = 50 Hz) */
    float blend_s;               /* tiempo de fundido con la animación original al ganar/perder seguimiento; defecto 0.25 */
    /* agarre de un enemigo (vrbody_set_grab) */
    float grab_blend_s;          /* tiempo de ida (y vuelta, algo más lenta) de las manos al cuello del enemigo; defecto 0.2 */
    float grab_spread_mm;        /* separación lateral de cada mano respecto al centro del cuello; defecto 75 */
    float grab_roll_deg;         /* giro de la palma al rodear el cuello (izquierda +, derecha -); defecto 60 */
    float grab_max_dist_mm;      /* si el cuello está más lejos que esto del hombro, esa mano no se pega; defecto 1100 */
    /* golpe sintético (vrbody_set_jab: golpe con el botón sin gesto) */
    float jab_s;                 /* duración total del golpe; defecto 0.28 */
    float jab_reach_mm;          /* cuánto avanza la mano respecto a donde está el mando; defecto 450 */
    float max_hand_jump_m;       /* salto de pose entre frames que se considera glitch y se suaviza como mucho a esta velocidad (0 = sin límite); defecto 3 m */
} VrBodyConfig;

void vrbody_config_default(VrBodyConfig *c);
void vrbody_init(const VrBodyConfig *c);     /* reinicia estado (fundidos, filtros) y fija la configuración (copia) */
const VrBodyConfig *vrbody_config(void);

void vrbody_set_grip(int side, float v);     /* side 0 izquierda / 1 derecha; v 0 (abierta) .. 1 (cerrada) */
void vrbody_set_dominant(int side);          /* 0 izquierda / 1 derecha */
void vrbody_set_armed(int on);               /* la mano dominante empuña un arma: su corrección agarre -> mano es la FIJA (vrbody_hand_offset, sin cabeceo automático por el apuntado); la pose de los dedos la decide vr_hand_pose según el objeto */
void vrbody_set_enabled(int on);
/* El anfitrión suprime el puño deformado de la malla del motor (las manos nuevas de ns_hand ya hacen el puño). Va aparte de cfg.fist_enabled (ajuste del usuario, no se modifica). */
void vrbody_set_fist_suppressed(int on);
/* Estado de la mano en el último apply (hilo de emulación): agarre efectivo 0..1 y marco de mundo de la pieza mano tras la IK (R fila-mayor 3x3, t en mm del motor). 0 si ese
 * brazo no se manejó. side 0 izquierda / 1 derecha. R y t pueden ser NULL. */
int vrbody_hand_state(int side, float *grip, float R[9], float t[3]);
/* Punto de golpe (nudillos del puño de la mano articulada, constante en el marco de la mano) en el mundo del motor, mm; 0 si el brazo no se manejó en el último apply. */
int vrbody_fist_point(int side, float out[3]);

/* Agarre de un enemigo (gestos de vr/input): mientras `active`, las DOS manos del personaje se pegan con una transición suave a los lados del cuello del enemigo
 * (la izquierda a su lado `left` del cuello, la derecha al otro; dedos hacia el cuello, palma girada grab_roll_deg) y vuelven al mando al soltar. `neck` y `left` en el
 * MUNDO del motor (neck en unidades del motor = mm; left unitario horizontal, hacia la izquierda del personaje mirando al enemigo). Llamar cada VBlank mientras se
 * sujeta; si dejan de llegar (3 apply sin refrescar) se suelta. NULL o active = 0: soltar. Sin vista VR no hace nada. */
typedef struct VrBodyGrab { int active; float neck[3]; float left[3]; } VrBodyGrab;
void vrbody_set_grab(const VrBodyGrab *g);
/* Golpe sintético de una mano del personaje (side 0 izquierda del personaje / 1 derecha; dir = dirección horizontal del golpe en el MUNDO del motor, unitaria): la mano
 * sale hacia dir jab_reach_mm y vuelve en jab_s con el puño cerrado, para que el golpe con el BOTÓN (sin gesto físico) también mueva la mano y no solo el codo. Sin vista VR o sin
 * seguimiento de esa mano no hace nada. Un golpe nuevo reinicia el anterior. */
void vrbody_set_jab(int side, const float dir[3]);
/* Cambia el giro de agarre -> mano (grados, -180..180; ver VrBodyConfig.hand_roll_deg) sin reiniciar el estado. Para calibrar en el visor. */
void vrbody_set_hand_roll(float deg);
/* Cabeceo fijo (hand_pitch_deg) y modo automático por el apuntado (hand_pitch_auto); no reinician el estado. */
void vrbody_set_hand_pitch(float deg);
void vrbody_set_hand_pitch_auto(int on);
/* Cabeceo (grados, giro sobre el eje X del agarre) que alinea los dedos del agarre con el apuntado: atan2(ay, -az) con a = apuntado(-Z) en el marco del agarre. 1 = bien; 0 = apuntado
 * inservible (cuaterniones no finitos o apuntado casi perpendicular a los dedos): se usa el cabeceo fijo. */
int vrbody_pitch_from_aim(const float grip_q[4], const float aim_q[4], float *deg);
/* Calibración "Calibrar manos": con los brazos rectos al frente y los mandos sujetos de forma natural, toma la orientación ACTUAL de cada agarre y guarda la corrección fija
 * (cuaternión agarre -> mano) que deja la mano del personaje con los dedos horizontales al frente y el dorso girado hand_roll_deg hacia fuera. Tiene PRIORIDAD sobre hand_pitch_deg/hand_pitch_auto/hand_roll_deg de esa mano
 * (cambiarlos después no la afecta) hasta vrbody_clear_calibration. Devuelve cuántas manos se calibraron (0..2; una mano sin seguimiento o con los dedos apuntando casi vertical no se calibra). */
int vrbody_calibrate_hands(void);
void vrbody_clear_calibration(void);
/* Persistencia: obtener (devuelve 1 si hay calibración) / restaurar la corrección de un lado (0 izq, 1 der). */
int vrbody_get_calibration(int side, float q[4]);
void vrbody_set_calibration(int side, const float q[4]);   /* rechaza datos no finitos o con la norma fuera de 0,5..2 */
/* Corrección fija agarre -> mano de un lado (cuaternión x,y,z,w): la calibrada si la hay (devuelve 1) o cabeceo fijo hand_pitch_deg seguido del alabeo hand_rot_offset (devuelve 0). Con la mano armada es la misma
 * que usa vrbody_apply para dibujar la mano; el arma cuelga del agarre del mando con ella (mano = agarre * este desvío). */
int vrbody_hand_offset(int side, float q[4]);
/* Giro extra agarre -> mano de un lado (cuaternión x,y,z,w), aplicado ANTES del desvío fijo/calibrado en vrbody_hand_offset y al dibujar esa mano (offset total = q * desvío): lo calcula vrgame para que la boca del arma apunte donde el mando
 * (VrHandPose.aim). NULL, no finito o norma fuera de 0,5..2: se desactiva (identidad). No sobrevive a vrbody_init. */
void vrbody_set_aim_fix(int side, const float q[4]);
/* Pura (sin estado): corrección agarre -> mano que deja los dedos horizontales y el dorso a roll_signed_deg (izq +, der -) del cenit. 1 = bien; 0 = dedos casi verticales. */
int vrbody_calib_offset(const float grip_q[4], float roll_signed_deg, float out_q[4]);

/* ------------------------------------------------------------------ pegamento sobre la RAM ----------------------- */
/* Sustituye las matrices de los brazos para el frame en curso (etapa de transformación). Devuelve el nº de brazos
 * modificados (0 si no hay vista VR, el modelo no es el esperado, el objeto está fuera de pantalla o no hay manos).
 * Escribe en la RAM del juego world+screen (64 B) de las piezas de los brazos, y guarda su valor original. */
/* Además (con al menos una mano seguida): activa MF_TWO_SIDED (0x400 en model[0]) en los modelos de las 16 piezas; con torso_follow reescribe world+screen de la pelvis y sus piernas, el torso y el cuello;
 * con own_hands oculta las tres piezas de cada brazo movido por la IK (el lado no movido conserva la animación del juego). Todo se devuelve en vrbody_restore. */
int  vrbody_apply(uint8_t *ram);
/* Tiempo (s) emulado desde el apply anterior: vrbody_apply se llama una vez por frame DIBUJADO del juego, no por VBlank. El anfitrión lo fija antes de cada apply
 * (VBlanks transcurridos / vblank_hz, al menos uno); se acota a 0.2 s. Se usa en la velocidad de marcha y en todos los filtros (suavizado de mano, fundido, pelvis, pecho).
 * 0 o no finito = vuelve a VrBodyConfig.frame_dt_s. */
void vrbody_set_dt(float dt_s);
/* Devuelve esas piezas a su valor original (idempotente; sin efecto si no hay nada pendiente). */
void vrbody_restore(uint8_t *ram);
void vrbody_prefist(uint8_t *ram);   /* puño antes de la captura nativa (usa el agarre del último apply) */
/* 1 si hay matrices IK escritas sin restaurar (diagnóstico). */
int  vrbody_pending(void);
/* 1 si la RAM del juego tiene algo escrito por esta capa sin restaurar (matrices IK, visibilidad, banderas de modelo o malla de mano deformada en puño): lo que se lea de la RAM
 * en ese instante no es el modelo en reposo. Lo comprueba quien lee mallas del jugador desde otro punto (vr/hands/vr_glove). */
int  vrbody_ram_dirty(void);
/* Activa/desactiva own_hands sin reiniciar el estado (el anfitrión lo activa solo cuando sus manos propias están listas). */
void vrbody_set_own_hands(int on);
/* Direcciones de modelo (model*) de las piezas brazo, antebrazo y mano de cada lado: models[0] = izquierdo {7,8,9}, models[1] = derecho {2,3,4} (cfg.part_arm).
 * 1 = las 6 legibles con la jerarquía esperada; 0 = no (models sin definir). */
int  vrbody_player_models(const uint8_t *ram, uint32_t models[2][3]);

typedef struct VrBodyStats {
    uint32_t applies, arms_driven, restores;
    uint32_t skip_disabled, skip_noview, skip_model, skip_offscreen, skip_nohands;
    uint32_t grab_applies;       /* brazos movidos con una mano pegada al enemigo (att > 0) */
    uint32_t jab_applies;        /* brazos movidos con un golpe sintético en curso */
    uint32_t fist_applies, fist_skipped, fist_pre;   /* mallas de mano deformadas en puño / mallas no reconocidas (se dejan rígidas) */
    uint32_t torso_applies;      /* frames con el torso girado hacia la mirada */
    uint32_t hidden_pieces;      /* piezas ocultas por own_hands (acumulado) */
    uint32_t near_hidden;        /* piezas ocultas por estar delante y pegadas al ojo (near_hide_m / near_hide_vtx_m; acumulado) */
    uint32_t two_sided;          /* modelos pasados a doble cara (acumulado) */
    float torso_yaw, torso_gaze; /* guiñada del pecho y de la mirada (grados) en el último frame con torso aplicado */
    float pelvis_yaw;            /* guiñada de la pelvis (grados) en ese frame */
    float stretch_sum, stretch_max;   /* suma (de los brazos estirados) y máximo del factor de estirado */
    uint32_t stretched, too_close, shoulder_limit, wrist_limit, bad_solve, stale_dropped, vis_forced;
} VrBodyStats;
void vrbody_stats(VrBodyStats *s);

/* Antebrazo tras la IK en el espacio del usuario (metros): codo, muñeca y dirección unitaria hacia el lado +Y del marco de AGARRE (arriba con el pulgar arriba; no es el dorso anatómico: ver hand_roll_deg). Lo usa el radar de
 * muñeca (vr/radar). side 0 izquierda / 1 derecha. 1 = válida (brazo movido por la IK en el último apply y fundido > 0,3);
 * 0 = no (el llamador cae al mando). Mismo layout que VrRadarArm (vr_radar.h). */
typedef struct { float elbow[3], wrist[3], dorsal[3]; } VrBodyForearm;
int vrbody_forearm_pose(int side, VrBodyForearm *out);

/* Estado de un brazo del último apply para REPETIR la IK con la mano viva (a más cadencia que el motor): todo en el espacio del usuario (m; orientaciones
 * en ese mismo espacio, no en el del motor). `rig` y `lim` en unidades del motor; upm = unidades del motor por metro. wrist/hand son el objetivo ya fundido y
 * grip_p/grip_q la pose del agarre (filtrada) con la que se calculó: la mano viva desplaza ese objetivo con el movimiento rígido de agarre simulado -> agarre vivo.
 * 1 = válido (brazo movido por la IK en el último apply); 0 = no. */
typedef struct VrBodyArmLive {
    VrArmRig rig; VrArmLimits lim;
    float upm;
    float shoulder[3];           /* hombro del torso enderezado */
    float torso[9];              /* marco del torso */
    float pole[3];               /* hacia dónde apunta el codo (unitario) */
    float wrist[3];              /* objetivo de la muñeca */
    float hand[9];               /* orientación objetivo de la pieza mano */
    float grip_p[3], grip_q[4];  /* agarre (filtrado) del que sale el objetivo */
    float grip;                  /* agarre efectivo del apply (ya multiplicado por el fundido) */
    float grip_floor;            /* mínimo del agarre mientras golpea (1) */
    float fade;                  /* peso de fundido 0..1 (suavizado) */
    int has_prev; float tau_prev, tau_f_prev, swivel_prev;   /* giro del antebrazo y del codo del apply (ver VrArmIn) */
} VrBodyArmLive;
int vrbody_arm_live(int side, VrBodyArmLive *out);
/* Brazo resuelto (espacio del usuario, m): origen de las piezas brazo, antebrazo y mano y sus orientaciones (columnas = ejes locales de la pieza). */
typedef struct VrBodyArmPose {
    float shoulder[3], elbow[3], wrist[3];
    float R_upper[9], R_fore[9], R_hand[9];   /* R_upper y R_fore ya alargadas (vrbody_stretch_bone) con `stretch` */
    float err_mm;
    float stretch;               /* >= 1: factor de estirado de los huesos */
    unsigned flags;              /* VRARM_* */
} VrBodyArmPose;
/* Pura. Repite la IK con el agarre vivo (live_p, live_q; cuaternión x,y,z,w): muñeca y mano del objetivo se mueven con D = vivo * inv(agarre simulado), el hombro, el torso y el
 * codo polar son los del apply. Con el agarre vivo = grip_p/grip_q reproduce la solución del apply. 0 = bien; -1 = entrada no finita o IK inválida. */
int vrbody_arm_resolve(const VrBodyArmLive *a, const float live_p[3], const float live_q[4], VrBodyArmPose *out);

/* Marco EFECTIVO de la mano del personaje tras la IK, en el espacio del usuario (m): 4x4 columna-mayor (columnas = ejes x, y, z y origen) en el MISMO marco que el
 * agarre entregado por vrpose_get_hands (sin hand_rot_offset ni el giro de cierre de los dedos: -Z hacia los dedos) y el origen en el centro de la palma que resulta de la muñeca resuelta. Si el
 * objetivo era alcanzable y está dentro de los límites coincide con la pose pedida; si no, la diferencia muestra dónde actuaron los límites (depuración visual).
 * 1 = válido (brazo movido por la IK en el último apply); 0 = no. */
int vrbody_debug_hand_frame(int side, float m[16]);

/* Gancho de diagnóstico: se llama al final de vrbody_apply (con las matrices IK ya escritas) si se escribió algo. Para
 * herramientas de validación; NULL por defecto. */
typedef void (*VrBodyHook)(uint8_t *ram);
void vrbody_set_apply_hook(VrBodyHook fn);

/* Diagnóstico de la última aplicación de cada brazo (pruebas/herramientas). */
typedef struct VrBodyDebug {
    int driven;                  /* este brazo se modificó en el último apply */
    float w;                     /* peso de fundido 0..1 */
    float target_wrist[3], target_hand[9]; /* objetivos (mundo, mm) tras suavizado y fundido */
    float grip;                  /* agarre efectivo */
    VrArmOut sol;
} VrBodyDebug;
const VrBodyDebug *vrbody_debug(int side);

#ifdef __cplusplus
}
#endif
