/* w_laser: puntero láser de la pistola VR. Una sola malla dinámica (prisma fino rojo desde la boca a lo largo de su eje + octaedro pequeño al final) en el espacio VR de la pose de la boca,
 * en el formato de vr/hands (VrHandsMesh, VRH_MF_DYNAMIC, sin textura): la dibuja el mismo renderer del arma (RenderWeapon.swift) sin tocar Swift ni Metal.
 *
 * Aspecto sin shader propio: el vértice es rgba = (255, 0, 0): el shader del arma modula textura blanca * rgba/128 * (ambiente + difuso * lambert) y satura a 1, así que sale rojo puro (1, 0, 0)
 * con cualquier luz (ambiente 0,78 * 255/128 > 1). No es aditivo y el renderer lo oculta tras el escenario por la profundidad (mismo pase que el mundo).
 * Longitud: WLASER_MAX_M, o hasta el primer impacto con el escenario del juego (vrgame.c lo calcula cada VBlank con vrhzd_raycast + vrhc_raycast_world + vrhit_probe (primer enemigo) y lo pasa por wlaser_length): el punto del
 * extremo queda sobre la superficie apuntada, WLASER_SURF_GAP_M por delante de ella, y el rayo se acorta como el del mod de 1997. Sin dato de impacto, 15 m.
 * El eje es el de la boca (-Z local de la pose), el mismo con el que vr_fire (vrfire_make_matrix) orienta la bala: lo que apunta el rayo es lo que dispara el arma. */
#pragma once
#include <stdint.h>
#include "weapon.h"
#include "../hands/vr_hands_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WLASER_MAX_M 15.0f           /* longitud del rayo (m) */
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

#ifdef __cplusplus
}
#endif
