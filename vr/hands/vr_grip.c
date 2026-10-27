/* vr_grip: ver vr_grip.h. */
#include "vr_grip.h"

/* Zócalos (hand-frame -> marco de empuñadura, m). Ver vr_grip.h; calculados con la mano por defecto:
 *   MAIN_YAW: R = Ry(-19,00 grados) * [[-1,0,0],[0,0,1],[0,1,0]] (apretón de manos), t = -R * centro de agarre.
 *   MAIN:     lo mismo sin guiñada (granada, colocables).
 *   SUPPORT:  palma hacia arriba bajo el guardamanos, pulgar hacia la boca, t = -R * centro de agarre. */
#define MAIN_YAW  {{0.00118f, 0.00000f, 0.07081f}, {-0.11671f, 0.69741f, 0.69741f, 0.11671f}}
#define MAIN      {{0.02416f, 0.00000f, 0.06656f}, {0.00000f, 0.70711f, 0.70711f, 0.00000f}}
#define SUPPORT   {{0.06656f, -0.02416f, 0.00000f}, {0.70711f, 0.70711f, 0.00000f, 0.00000f}}
#define NONE_POSE {{0, 0, 0}, {0, 0, 0, 1}}
#define TRIG      {0.00000f, 0.03240f, -0.06200f}

/* ids de game_weapon_id (tabla del puente): 0 pistola, 1 fusil, 2 granada, 3 guiado, 4 antiaéreo, 5 mina, 6 adhesivo con mando, 7 y 8 variantes de granada, 9 francotirador. */
static const VrGripEntry TABLE[] = {
    {0, HP_GRIP_PISTOL,  HP_GRIP_SUPPORT, 1, MAIN_YAW, TRIG,         SUPPORT},
    {1, HP_GRIP_RIFLE,   HP_GRIP_SUPPORT, 1, MAIN_YAW, TRIG,         SUPPORT},
    {2, HP_GRIP_SPHERE,  -1,              0, MAIN,     {0, 0, 0},    NONE_POSE},
    {3, HP_GRIP_RIFLE,   HP_GRIP_SUPPORT, 1, MAIN_YAW, TRIG,         SUPPORT},
    {4, HP_GRIP_RIFLE,   HP_GRIP_SUPPORT, 1, MAIN_YAW, TRIG,         SUPPORT},
    {5, HP_GRIP_PLACED,  -1,              0, MAIN,     {0, 0, 0},    NONE_POSE},
    {6, HP_GRIP_PLACED,  -1,              0, MAIN,     {0, 0, 0},    NONE_POSE},
    {7, HP_GRIP_SPHERE,  -1,              0, MAIN,     {0, 0, 0},    NONE_POSE},
    {8, HP_GRIP_SPHERE,  -1,              0, MAIN,     {0, 0, 0},    NONE_POSE},
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
