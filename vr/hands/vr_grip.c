/* vr_grip: ver vr_grip.h. */
#include "vr_grip.h"

/* Zócalos (marco de la mano -> marco de empuñadura, m; mano de vr_hand_skin a tamaño real). Ver vr_grip.h. Base: H = [x, a, x * a] con a = eje del mango en la mano (vrhs_handle_axis,
 * cruza la palma en diagonal) y x = normal de la palma; t = -R * centro de agarre (vrhs_grip_center).
 *   MAIN_YAW: apretón de manos (a -> +Y, palma -> -X) girado -16 grados de cabeceo (sobre X) y -13 de guiñada (sobre Y): la yema del índice con el gatillo suelto (HP_GRIP_PISTOL) cae en TRIG, el
 *             gatillo de la pistola primitiva de test_grip.c (63 mm por delante y 32 por encima del centro del mango).
 *   MAIN:     apretón de manos sin giro (colocables).
 *   SPHERE:   como MAIN con el centro del objeto 12 mm por delante de la palma (granada de ~32 mm de radio en el hueco de la mano, HP_GRIP_SPHERE).
 *   SUPPORT:  palma hacia arriba bajo el guardamanos, eje del mango a lo largo del cañón (hacia la boca), dedos hacia -X. */
#define MAIN_YAW  {{0.03192f, -0.01609f, 0.07586f}, {-0.10496f, 0.61170f, 0.78295f, 0.04241f}}
#define MAIN      {{0.04651f, -0.03637f, 0.05955f}, {0.00000f, 0.50000f, 0.86603f, 0.00000f}}
#define SPHERE    {{0.05851f, -0.03637f, 0.05955f}, {0.00000f, 0.50000f, 0.86603f, 0.00000f}}
#define SUPPORT   {{0.05955f, -0.04651f, 0.03637f}, {0.68301f, 0.68301f, 0.18301f, 0.18301f}}
#define NONE_POSE {{0, 0, 0}, {0, 0, 0, 1}}
#define TRIG      {0.00017f, 0.03195f, -0.06314f}

/* ids de game_weapon_id (tabla del puente): 0 pistola, 1 fusil, 2 granada, 3 guiado, 4 antiaéreo, 5 mina, 6 adhesivo con mando, 7 y 8 variantes de granada, 9 francotirador. */
static const VrGripEntry TABLE[] = {
    {0, HP_GRIP_PISTOL,  HP_GRIP_SUPPORT, 1, MAIN_YAW, TRIG,         SUPPORT},
    {1, HP_GRIP_RIFLE,   HP_GRIP_SUPPORT, 1, MAIN_YAW, TRIG,         SUPPORT},
    {2, HP_GRIP_SPHERE,  -1,              0, SPHERE,     {0, 0, 0},    NONE_POSE},
    {3, HP_GRIP_RIFLE,   HP_GRIP_SUPPORT, 1, MAIN_YAW, TRIG,         SUPPORT},
    {4, HP_GRIP_RIFLE,   HP_GRIP_SUPPORT, 1, MAIN_YAW, TRIG,         SUPPORT},
    {5, HP_GRIP_PLACED,  -1,              0, MAIN,     {0, 0, 0},    NONE_POSE},
    {6, HP_GRIP_PLACED,  -1,              0, MAIN,     {0, 0, 0},    NONE_POSE},
    {7, HP_GRIP_SPHERE,  -1,              0, SPHERE,     {0, 0, 0},    NONE_POSE},
    {8, HP_GRIP_SPHERE,  -1,              0, SPHERE,     {0, 0, 0},    NONE_POSE},
    {9, HP_GRIP_RIFLE,   HP_GRIP_SUPPORT, 1, MAIN_YAW, TRIG,         SUPPORT},
};
#define N_TABLE ((int)(sizeof TABLE / sizeof TABLE[0]))

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
