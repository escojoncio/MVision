/* vr_grip: ver vr_grip.h. */
#include "vr_grip.h"
#include <string.h>

/* Zócalos (marco de la mano -> marco de empuñadura, m con 1 unidad de la mano = 1 mm; mano de vr_hand_skin a tamaño real, VRHS_FIT_K = 1). Ver vr_grip.h. Base: H = [x, a, x * a] con a = eje del mango en la mano (vrhs_handle_axis,
 * cruza la palma en diagonal) y x = normal de la palma; t = -R * centro de agarre (vrhs_grip_center).
 *   MAIN_YAW: apretón de manos (a -> +Y, palma -> -X) girado -17 grados de cabeceo (sobre X) y -35 de guiñada (sobre Y): la yema del índice con el gatillo suelto (HP_GRIP_PISTOL) cae en TRIG, el
 *             gatillo de la pistola primitiva de test_grip.c (63 mm por delante y 32 por encima del centro del mango).
 *   MAIN:     apretón de manos sin giro (colocables).
 *   SPHERE:   como MAIN con el centro del objeto 15 unidades por delante de la palma (granada de ~32 mm de radio en el hueco de la mano, HP_GRIP_SPHERE).
 *   SUPPORT_*: MEDIDOS en las ANIMACIONES de Snake (no inventados): OAR "snake" (init, 0x992d 'o'; formato game/motion.c) con el esqueleto del KMD "snake" (init, 0x992d 'k'), acción de apuntar de cada arma
 *             (sna_init.c: SocomAttack.aim = 42, FamasAttack.aim = 5, StingerAttack/NikitaAttack.aim = 74, PSG1Attack.aim = 44), último fotograma del movimiento, RotMatrixZYX por hueso. Validado con
 *             el volcado de RAM a33 (PSG1 apuntando): hombros, codos y mano izquierda coinciden con el volcado en <= 5 mm. La mano derecha (parte 4) lleva el arma con la misma matriz, así que el marco del
 *             arma es el del hueso con (x, y, z) del motor -> (-x, z, y). Cada zócalo es la orientación medida de la mano izquierda (parte 9) en ese marco (el hueso de la mano nueva y el de Snake
 *             comparten ejes: dedos -Y, palma +X en la derecha), llevada a la mano derecha canónica con el espejo; la posición pone el centro de agarre de la mano nueva (HP_GRIP_SUPPORT) en WP_SUPPORT.
 *             Herramientas: /tmp/w57 (oar.py, fk.py, an4.c; no van al repo). Sin confirmar: el punto WP_SUPPORT del FAMAS/Stinger/Nikita queda a 70-140 mm de la palma estimada de Snake (ver bitácora).
 *   El codo (elbow_main / elbow_support): dirección del codo medida en la misma pose, en el marco del torso de Snake (x hacia fuera de su lado, y arriba, z delante), para vrbody_set_pole.
 */
/* RESERVA SIN USO: SUP_PIST, SUP_RIFLE, SUP_LAUNCH, SUP_SNIPER, MAIN_YAW, MAIN_PIST, TRIG_PIST y TRIG solo son el respaldo de los FIT_<ARMA>_* (#ifndef más abajo); con vr_grip_fit.inc no se usan. No borrar. */
#define SUP_PIST    {{0.09158f, -0.00660f, 0.05918f}, {-0.18441f, -0.65841f, -0.72515f, 0.08153f}}
#define SUP_RIFLE   {{0.07210f, -0.08201f, 0.00316f}, {0.78183f, 0.61331f, 0.09187f, 0.06449f}}
#define SUP_LAUNCH  {{0.09908f, -0.04383f, -0.01398f}, {-0.35963f, -0.27085f, -0.85976f, 0.24106f}}
#define SUP_SNIPER  {{0.05474f, -0.07318f, 0.05985f}, {0.73084f, 0.57621f, 0.15323f, 0.33223f}}
#define MAIN_YAW  {{0.00397f, -0.00208f, 0.10915f}, {-0.27978f, 0.59370f, 0.74639f, 0.11021f}}
#define MAIN_PIST {{0.05843f, -0.06337f, 0.12371f}, {0.01279f, 0.61352f, 0.77839f, 0.13242f}}   /* pistola: mano a 20 grados de la de Snake (hueso con ejes F), palma a 3 mm de la geometria del mango y yema del indice a 0 mm del gatillo (TRIG_PIST) */
#define MAIN      {{0.05480f, -0.02408f, 0.06670f}, {0.00000f, 0.50000f, 0.86603f, 0.00000f}}   /* t = -R * centro de agarre con la mano a tamaño real (VRHS_FIT_K = 1) */
#define SPHERE    {{0.06980f, -0.02408f, 0.06670f}, {0.00000f, 0.50000f, 0.86603f, 0.00000f}}
#define NONE_POSE {{0, 0, 0}, {0, 0, 0, 1}}
#define TRIG_PIST {0.00000f, -0.00260f, -0.02952f}
#define TRIG      {0.00001f, 0.03165f, -0.06241f}

/* Armas ajustadas contra la malla real (gripfit_gen.c): FIT_<ARMA>_{GRIP,SUP,TRIG,POSE,SUPPOSE}; el zócalo de la mano que empuña va respecto al punto WP_GRIP del modelo y el de apoyo (reflejado) respecto a WP_SUPPORT. */
#include "vr_grip_fit.inc"
#ifndef FIT_SOCOM_TURN
#define FIT_SOCOM_TURN {0, 0, 0, 1}
#endif
#ifndef FIT_SOCOM_GRIP   /* arma sin ajuste: zócalos y poses de antes */
#define FIT_SOCOM_GRIP MAIN_PIST
#define FIT_SOCOM_SUP SUP_PIST
#define FIT_SOCOM_TRIG TRIG_PIST
#define FIT_SOCOM_POSE HP_GRIP_PISTOL
#define FIT_SOCOM_SUPPOSE HP_GRIP_SUPPORT
#endif
#ifndef FIT_FAMAS_TURN
#define FIT_FAMAS_TURN {0, 0, 0, 1}
#endif
#ifndef FIT_FAMAS_GRIP   /* arma sin ajuste: zócalos y poses de antes */
#define FIT_FAMAS_GRIP MAIN_YAW
#define FIT_FAMAS_SUP SUP_RIFLE
#define FIT_FAMAS_TRIG TRIG
#define FIT_FAMAS_POSE HP_GRIP_RIFLE
#define FIT_FAMAS_SUPPOSE HP_GRIP_SUPPORT
#endif
#ifndef FIT_PSG1_TURN
#define FIT_PSG1_TURN {0, 0, 0, 1}
#endif
#ifndef FIT_PSG1_GRIP   /* arma sin ajuste: zócalos y poses de antes */
#define FIT_PSG1_GRIP MAIN_YAW
#define FIT_PSG1_SUP SUP_SNIPER
#define FIT_PSG1_TRIG TRIG
#define FIT_PSG1_POSE HP_GRIP_RIFLE
#define FIT_PSG1_SUPPOSE HP_GRIP_SUPPORT
#endif
#ifndef FIT_STINGER_TURN
#define FIT_STINGER_TURN {0, 0, 0, 1}
#endif
#ifndef FIT_STINGER_GRIP   /* arma sin ajuste: zócalos y poses de antes */
#define FIT_STINGER_GRIP MAIN_YAW
#define FIT_STINGER_SUP SUP_LAUNCH
#define FIT_STINGER_TRIG TRIG
#define FIT_STINGER_POSE HP_GRIP_RIFLE
#define FIT_STINGER_SUPPOSE HP_GRIP_SUPPORT
#endif
#ifndef FIT_NIKITA_TURN
#define FIT_NIKITA_TURN {0, 0, 0, 1}
#endif
#ifndef FIT_NIKITA_GRIP   /* arma sin ajuste: zócalos y poses de antes */
#define FIT_NIKITA_GRIP MAIN_YAW
#define FIT_NIKITA_SUP SUP_LAUNCH
#define FIT_NIKITA_TRIG TRIG
#define FIT_NIKITA_POSE HP_GRIP_RIFLE
#define FIT_NIKITA_SUPPOSE HP_GRIP_SUPPORT
#endif
#ifndef FIT_FRAG_POSE   /* granadas sin ajuste: pose genérica de esfera */
#define FIT_FRAG_POSE HP_GRIP_SPHERE
#endif
#ifndef FIT_STUN_POSE
#define FIT_STUN_POSE HP_GRIP_SPHERE
#endif
#ifndef FIT_CHAFF_POSE
#define FIT_CHAFF_POSE HP_GRIP_SPHERE
#endif

/* ids de game_weapon_id (tabla del puente): 0 pistola, 1 fusil, 2 granada, 3 guiado, 4 antiaéreo, 5 mina, 6 adhesivo con mando, 7 y 8 variantes de granada, 9 francotirador. */
static const VrGripEntry TABLE[] = {
    {0, FIT_SOCOM_POSE, FIT_SOCOM_SUPPOSE, 1, FIT_SOCOM_GRIP, FIT_SOCOM_TRIG, FIT_SOCOM_SUP,   {0.532f, -0.288f, 0.796f}, {0.566f, -0.776f, 0.280f}, FIT_SOCOM_TURN},
    {1, FIT_FAMAS_POSE, FIT_FAMAS_SUPPOSE, 1, FIT_FAMAS_GRIP, FIT_FAMAS_TRIG, FIT_FAMAS_SUP,  {0.580f, -0.619f, 0.529f}, {0.232f, -0.969f, 0.079f}, FIT_FAMAS_TURN},
    {2, FIT_FRAG_POSE,  -1,              0, SPHERE,     {0, 0, 0},    NONE_POSE, {0, 0, 0}, {0, 0, 0}, {0, 0, 0, 1}},
    {3, FIT_NIKITA_POSE, FIT_NIKITA_SUPPOSE, 1, FIT_NIKITA_GRIP, FIT_NIKITA_TRIG, FIT_NIKITA_SUP, {0.649f, -0.625f, 0.433f}, {0.266f, -0.964f, -0.009f}, FIT_NIKITA_TURN},
    {4, FIT_STINGER_POSE, FIT_STINGER_SUPPOSE, 1, FIT_STINGER_GRIP, FIT_STINGER_TRIG, FIT_STINGER_SUP, {0.649f, -0.625f, 0.433f}, {0.266f, -0.964f, -0.009f}, FIT_STINGER_TURN},
    {5, HP_GRIP_PLACED,  -1,              0, MAIN,     {0, 0, 0},    NONE_POSE, {0, 0, 0}, {0, 0, 0}, {0, 0, 0, 1}},
    {6, HP_GRIP_PLACED,  -1,              0, MAIN,     {0, 0, 0},    NONE_POSE, {0, 0, 0}, {0, 0, 0}, {0, 0, 0, 1}},
    {7, FIT_STUN_POSE,  -1,              0, SPHERE,     {0, 0, 0},    NONE_POSE, {0, 0, 0}, {0, 0, 0}, {0, 0, 0, 1}},
    {8, FIT_CHAFF_POSE,  -1,              0, SPHERE,     {0, 0, 0},    NONE_POSE, {0, 0, 0}, {0, 0, 0}, {0, 0, 0, 1}},
    {9, FIT_PSG1_POSE, FIT_PSG1_SUPPOSE, 1, FIT_PSG1_GRIP, FIT_PSG1_TRIG, FIT_PSG1_SUP, {0.655f, 0.122f, 0.746f}, {0.143f, -0.111f, 0.983f}, FIT_PSG1_TURN},
};
#define N_TABLE ((int)(sizeof TABLE / sizeof TABLE[0]))

/* Objeto del juego colgado de la mano con desplazamiento propio (granadas): ver vr_grip_obj.inc. Los ids que no están aquí conservan la relación mano-objeto del juego. */
#include "vr_grip_obj.inc"
static const struct { int game_weapon_id; float R[9]; float t[3]; } OBJ_TABLE[] = {
    {2, OBJ_SNAKE}, {7, OBJ_SNAKE}, {8, OBJ_SNAKE},
};
int vrgrip_object_offset(int game_weapon_id, float R[9], float t[3]) {
    for (int i = 0; i < (int)(sizeof OBJ_TABLE / sizeof OBJ_TABLE[0]); i++) {
        if (OBJ_TABLE[i].game_weapon_id != game_weapon_id) continue;
        if (R) memcpy(R, OBJ_TABLE[i].R, sizeof OBJ_TABLE[i].R);
        if (t) memcpy(t, OBJ_TABLE[i].t, sizeof OBJ_TABLE[i].t);
        return 1;
    }
    return 0;
}

const VrGripEntry *vrgrip_find(int game_weapon_id) {
    for (int i = 0; i < N_TABLE; i++) if (TABLE[i].game_weapon_id == game_weapon_id) return &TABLE[i];
    return NULL;
}
int vrgrip_count(void) { return N_TABLE; }
const VrGripEntry *vrgrip_at(int i) { return (i >= 0 && i < N_TABLE) ? &TABLE[i] : NULL; }

WPose vrgrip_mirror(WPose p) {
    p.p.x = -p.p.x;
    p.q.y = -p.q.y; p.q.z = -p.q.z;
    return p;
}

WPose vrgrip_weapon_from_hand(WPose handle, WPose socket, int side) {
    return wpose_mul(handle, side < 0 ? vrgrip_mirror(socket) : socket);
}
