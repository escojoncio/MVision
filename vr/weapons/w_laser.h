/* w_laser: puntero láser de la pistola VR. Una sola malla dinámica (prisma fino rojo desde la boca a lo largo de su eje + octaedro pequeño al final) en el espacio VR de la pose de la boca,
 * en el formato de vr/hands (VrHandsMesh, VRH_MF_DYNAMIC, sin textura): la dibuja el mismo renderer del arma (RenderWeapon.swift) sin tocar Swift ni Metal.
 *
 * Aspecto sin shader propio: el vértice es rgba = (255, 0, 0): el shader del arma modula textura blanca * rgba/128 * (ambiente + difuso * lambert) y satura a 1, así que sale rojo puro (1, 0, 0)
 * con cualquier luz (ambiente 0,78 * 255/128 > 1). No es aditivo y el renderer lo oculta tras el escenario por la profundidad (mismo pase que el mundo).
 * Longitud: WLASER_MAX_M, o hasta el primer impacto de la bala (vrgame.c lo calcula cada VBlank con vrhit_shot_dist, vr/gameplay/vr_hit.h: la MISMA prueba y el mismo alcance que la bala: primer blanco con sus volúmenes,
 * paredes HZD y suelos de colisión del juego; no la geometría de render, que no bloquea balas) y lo pasa por wlaser_length): el punto del
 * extremo queda sobre la superficie apuntada, WLASER_SURF_GAP_M por delante de ella, y el rayo se acorta como el del mod de 1997. Sin dato de impacto, WLASER_MAX_M.
 * El eje es el de la boca (-Z local de la pose), el mismo con el que vr_fire (vrfire_make_matrix) orienta la bala: lo que apunta el rayo es lo que dispara el arma. La pose de la boca que recibe es la del arma SIN el retroceso
 * visual (recoil_offset, que solo mueve el modelo): el rayo no se balancea con el disparo y la bala sale por él aunque se dispare rápido. */
#pragma once
#include <stdint.h>
#include "weapon.h"
#include "../hands/vr_hands_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WLASER_MAX_M 25.0f           /* longitud del rayo (m): el alcance de la bala de la SOCOM (VRHIT_SHOT_M_SOCOM, vr/gameplay/vr_hit.h); el láser es solo de la SOCOM */
#define WLASER_SURF_GAP_M 0.005f     /* el extremo del rayo queda tanto antes de la superficie (evita z-fighting del punto con ella) */
#define WLASER_MIN_M 0.05f           /* por debajo de esto no se dibuja */
#define WLASER_HALF_W 0.00125f       /* medio lado del prisma (m): 2,5 mm de grosor */
#define WLASER_DOT_MIN 0.004f        /* medio tamaño mínimo del punto (m) */
#define WLASER_DOT_ANG 0.0015f       /* medio tamaño del punto por metro de rayo (tamaño angular casi constante) */
#define WLASER_MIN_FWD_Y (-0.5f)     /* «levantada»: la boca no apunta más de 30 grados por debajo del horizonte (componente Y del eje >= esto) */
#define WLASER_NV 14                 /* 8 del prisma + 6 del octaedro */
#define WLASER_NI 48                 /* 8 triángulos del prisma + 8 del octaedro */
#define WLASER_MESH_ID 0x7F4C5A52u   /* fuera de los de las armas (uid * 64) y de los del guante (bit 31) */

typedef struct { VrHandsVertex v[WLASER_NV]; uint16_t idx[WLASER_NI]; } WLaserMesh;

/* 1 si el eje de la boca (-Z local) no apunta por debajo de WLASER_MIN_FWD_Y (el arma está levantada y no colgando); pose no finita o sin cuaternión: 0. */
int wlaser_raised(const WPose *muzzle);
/* Longitud del rayo (m) para un impacto a `hit_m` de la boca: hit_m - WLASER_SURF_GAP_M recortado a [0, WLASER_MAX_M]; hit_m negativo o no finito (= sin impacto / sin dato): WLASER_MAX_M. */
float wlaser_length(float hit_m);
/* Rellena `out` con el rayo de `length_m` (recortado a WLASER_MAX_M) desde la posición de la boca a lo largo de su eje. La topología es siempre la misma (WLASER_NV/WLASER_NI).
 * 0 = pose no finita, cuaternión nulo o longitud menor que WLASER_MIN_M / no finita (out sin tocar). */
int wlaser_build(const WPose *muzzle, float length_m, WLaserMesh *out);
/* Entrada de la lista del frame (vr_hands_frame.h) para esa malla: sin textura, modelo = identidad (vértices ya en el espacio VR), dinámica. Los punteros apuntan a `m`. */
void wlaser_mesh(const WLaserMesh *m, VrHandsMesh *out);

/* Fantasma de colocación de la mina (claymore): contorno plano de su huella real tendido
 * sobre el suelo en la pose de colocación `placed` (la que el anfitrión da al juego, con p.y = la altura del SUELO: el origen del modelo va WGHOST_MINE_LEGS_U uds más arriba, apoyada por las patas, y no sirve de ancla),
 * y una flecha desde el borde frontal, a la altura de la punta de la cara picuda (Z = -207 mm), hacia la cara picuda (+X de `placed`), que es la dirección de disparo. Solo cuenta la guiñada de `placed`. Una sola malla dinámica
 * (WGHOST_NV vértices, < 4096 bytes), verde, en el espacio VR; doble cara. 0 = datos no finitos, cuaternión nulo, +X o +Z del modelo verticales o arrow_len <= 0 (out sin tocar).
 * FUENTE ÚNICA de la caja del modelo 0x17a1 (body + patas): medida con wload en el marco del motor sin rotar (W_FRAME_ENGINE, uds = mm; test_throw_place_models.c la vuelve a medir) y en el marco con que el JUEGO lo dibuja ya
 * colocado (vrfire_make_matrix: col0 = izquierda = arriba x delante, col1 = -delante, col2 = arriba): X del motor -> -X de `placed`/marco del arma (WCUT_BB, HELD girado 180 grados en Z), Y del motor -> +Z de
 * `placed`, Z del motor -> arriba (las patas, 12 triángulos, ocupan Z -224..-99: hacia abajo). Todo lo demás (WGHOST_MINE_*, w_placed.c, vrgame.c) sale de estos seis números. */
#define WGHOST_MINE_RAW_X0 -31.0f    /* mm del motor, caja completa del modelo (cuerpo + patas) */
#define WGHOST_MINE_RAW_X1 80.0f
#define WGHOST_MINE_RAW_Y0 -370.0f
#define WGHOST_MINE_RAW_Y1 -45.0f
#define WGHOST_MINE_RAW_Z0 -224.0f   /* punta de las patas */
#define WGHOST_MINE_RAW_Z1 100.0f    /* mira */
#define WGHOST_MINE_X0 (-WGHOST_MINE_RAW_X1 / 1000.0f)     /* huella en el marco de `placed` (m) */
#define WGHOST_MINE_X1 (-WGHOST_MINE_RAW_X0 / 1000.0f)
#define WGHOST_MINE_LEGS_U (-WGHOST_MINE_RAW_Z0)           /* uds del motor del origen del modelo a la punta de sus patas, hacia abajo en el marco colocado */
#define WGHOST_MINE_UP_M (WGHOST_MINE_RAW_Z1 / 1000.0f)    /* altura de la mira sobre el origen (m) */
#define WGHOST_MINE_Z0 (WGHOST_MINE_RAW_Y0 / 1000.0f)
#define WGHOST_MINE_Z1 (WGHOST_MINE_RAW_Y1 / 1000.0f)
#define WGHOST_MINE_ZA ((WGHOST_MINE_RAW_Y0 + WGHOST_MINE_RAW_Y1) / 2000.0f)   /* punta de la cara picuda: centro del eje largo */
#define WGHOST_NSEG 7                /* 4 lados del rectángulo + fuste y 2 puntas de la flecha */
#define WGHOST_NV (4 * WGHOST_NSEG)
#define WGHOST_NI (12 * WGHOST_NSEG)
#define WGHOST_MESH_ID 0x7F4C5A53u
#define WGHOST_LIFT_M 0.004f         /* el contorno queda tanto por encima del suelo (evita z-fighting) */
#define WGHOST_STRIP_HALF_W 0.004f   /* medio ancho de cada tira (m) */
typedef struct { VrHandsVertex v[WGHOST_NV]; uint16_t idx[WGHOST_NI]; } WGhostMesh;
int wghost_build(const WPose *placed, float arrow_len, WGhostMesh *out);
void wghost_mesh(const WGhostMesh *m, VrHandsMesh *out);

#ifdef __cplusplus
}
#endif
