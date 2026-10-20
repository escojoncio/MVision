/* vr_hand_mesh: mano articulada propia del personaje, generada por código (C99, matemática pura, sin datos del juego).
 *
 * Sustituye a la "manopla" del motor en primera persona: en vez de deformar una malla rígida de 22 caras (vr_fist), la mano es un esqueleto de 16 huesos RÍGIDOS,
 * cada uno con su malla cerrada, y el puño es solo una pose (ángulos de las articulaciones): el grosor de los dedos no cambia nunca.
 *
 *   hueso 0        palma (raíz, con volumen: puño de la manga, centro y nudillos)
 *   huesos 1..12   índice, corazón, anular, meñique: falange proximal, media y distal (3 cada uno)
 *   huesos 13..15  pulgar: metacarpo y dos falanges
 *
 * Marco de la mano = marco local de la pieza mano del modelo del jugador (mm; dedos hacia -Y, pulgar hacia +Z; la palma mira a +X en la mano DERECHA y a -X en la
 * izquierda). Se construye siempre la mano derecha canónica y la izquierda es su ESPEJO EXACTO en X (vértices con x -> -x; las caras invierten el orden A,D,C,B
 * para conservar el sentido; las poses se conjugan con el espejo).
 *
 * Cada hueso tiene su malla en su marco local (origen en la articulación proximal, el tubo se extiende hacia -Y): la pose da 12 flotantes por hueso (3x4 fila-mayor,
 * hueso local -> marco de la mano). Las mallas llevan la convención del motor: caras A,B,C,D en perímetro con cross(B-A, D-A) hacia el INTERIOR y normales
 * interiores por vértice (4096 = 1), triángulos (A,B,D) y (B,C,D).
 *
 * Tamaño: la malla se ajusta a la manopla original (VrHandDims: puño de la manga y largo medidos en la malla de la manopla). Número de triángulos: ~590.
 * Textura: cada cara lleva un rol (palma, dorso, dedo, articulación) y parámetros (su, sv) 0..255 por esquina; quien dibuja los proyecta a la región de la textura de la
 * manopla (ns_hand). */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VRHAND_BONES 16
#define VRHAND_MAX_V 32          /* vértices por hueso */
#define VRHAND_MAX_F 24          /* caras por hueso */

enum { VRHAND_PALM = 0, VRHAND_INDEX = 1, VRHAND_MIDDLE = 4, VRHAND_RING = 7, VRHAND_LITTLE = 10, VRHAND_THUMB = 13 };   /* primer hueso de cada grupo */
enum { VRHAND_ROLE_PALM = 0, VRHAND_ROLE_BACK = 1, VRHAND_ROLE_FINGER = 2, VRHAND_ROLE_JOINT = 3, VRHAND_ROLES = 4 };

/* Medidas de la manopla del motor en el marco de la mano, mano derecha canónica (para la izquierda se refleja en X antes de medir). */
typedef struct {
    float cuff_y;                /* plano del puño de la manga (22) */
    float cuff_c[2];             /* centro x, z del puño (7,5; -15,5) */
    float cuff_h[2];             /* semiejes x, z del puño (27,5; 33,5) */
    float tip_y;                 /* punta de los dedos (-125) */
} VrHandDims;

/* Ángulos de la pose (grados). Dedos: MCP, PIP, DIP abiertos y cerrados por dedo (índice, corazón, anular, meñique). Pulgar: base con flexión y aducción, MCP, IP. */
typedef struct {
    float open_deg[3];
    float closed_deg[4][3];
    float thumb_open[4];         /* cmc_flex, cmc_add, mcp, ip */
    float thumb_closed[4];
    float thumb_start;           /* agarre a partir del cual el pulgar empieza a cerrarse (cierra después de los dedos) */
} VrHandPoseCfg;

typedef struct {
    int parent;                  /* hueso padre (-1 raíz) */
    float pivot[3];              /* articulación proximal en el marco local del padre, en reposo */
    float rest[9];               /* rotación de reposo relativa al padre (columnas = ejes locales en el marco del padre), fila-mayor */
    float len;                   /* longitud del tubo hasta la articulación distal */
    float radius;                /* radio del ecuador distal */
} VrHandBone;

typedef struct {
    uint8_t n_verts, n_faces;
    int16_t v[VRHAND_MAX_V][3];      /* marco local del hueso (mm) */
    int16_t n[VRHAND_MAX_V][3];      /* normal interior unitaria por vértice, 4096 = 1 */
    uint8_t f[VRHAND_MAX_F][4];      /* A,B,C,D (perímetro) */
    uint8_t role[VRHAND_MAX_F];      /* VRHAND_ROLE_* */
    uint8_t su[VRHAND_MAX_F][4], sv[VRHAND_MAX_F][4];   /* parámetros de textura 0..255 por esquina */
} VrHandBoneMesh;

typedef struct {
    int mirror;                  /* 0 mano derecha, 1 izquierda (espejo exacto) */
    VrHandDims dims;             /* medidas con las que se construyó (derecha canónica) */
    VrHandPoseCfg pose;          /* ángulos de la pose */
    VrHandBone bone[VRHAND_BONES];
    VrHandBoneMesh mesh[VRHAND_BONES];
} VrHand;

void vrhand_dims_default(VrHandDims *d);
/* Mide la manopla: xyz = vértices (stride_i16 = int16 por vértice, 4 en el modelo del motor y en NsVert), side +1 si la palma mira a +X (derecha) o -1 (izquierda).
 * 1 = bien; 0 = malla inservible (d recibe los valores por defecto). */
int vrhand_dims_from_verts(const int16_t *xyz, int nv, int stride_i16, int side, VrHandDims *d);
void vrhand_pose_cfg_default(VrHandPoseCfg *p);
/* Construye la mano. side +1 derecha, -1 izquierda. dims NULL = por defecto. 0 = bien. */
int vrhand_build(VrHand *h, const VrHandDims *dims, int side);

/* Pose: xf[k] = hueso k local -> marco de la mano (3x4 fila-mayor, v' = R v + t). grip 0 (relajada) .. 1 (puño); fuera de rango o no finito se acota. */
void vrhand_pose(const VrHand *h, float grip, float xf[VRHAND_BONES][12]);
/* Con un cierre por dedo (curl[0..3] = índice..meñique, curl[4] = pulgar, cada uno 0..1). */
void vrhand_pose_curls(const VrHand *h, const float curl[5], float xf[VRHAND_BONES][12]);

/* Triángulos totales de la mano (suma de las mallas: 1 por cara triangular, 2 por cuadrilátero). */
int vrhand_tri_count(const VrHand *h);
/* Punto de golpe en el marco de la mano: centro de los vértices de los dedos más avanzados hacia -Y (las yemas con la mano abierta, los nudillos con el puño). */
void vrhand_fist_point(const VrHand *h, const float xf[VRHAND_BONES][12], float out[3]);
/* Centro de la palma (marco de la mano): el punto de agarre del mando. */
void vrhand_palm_center(const VrHand *h, float out[3]);

/* Colisión de la pose (cápsulas de los huesos entre sí y contra la palma): la mayor penetración en mm. */
typedef struct {
    float finger_finger;         /* entre dedos distintos (solo falanges; no las contiguas de un mismo dedo) */
    float finger_palm;           /* falanges contra la palma */
    float thumb_finger;          /* pulgar contra los demás dedos */
    float thumb_palm;            /* pulgar contra la palma */
    float tip_palm_gap;          /* distancia mínima de la yema del dedo medio a la superficie de la palma (mm; <= 0 toca) */
    float thumb_over;            /* x de la punta del pulgar menos x máxima de las falanges medias de índice y corazón (> 0: el pulgar queda por fuera) */
} VrHandCollision;
void vrhand_collision(const VrHand *h, const float xf[VRHAND_BONES][12], VrHandCollision *out);

#ifdef __cplusplus
}
#endif
