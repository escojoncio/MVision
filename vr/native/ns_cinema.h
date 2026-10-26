#ifndef NS_CINEMA_H
#define NS_CINEMA_H
/* ns_cinema: cinemáticas del motor con el render nativo. La pantalla 3D de la cinemática enseña el mundo desde la CÁMARA DEL JUEGO (no desde la cabeza): se dibuja la
 * escena nativa dos veces (ojo izquierdo/derecho) en las texturas de la pantalla, con la proyección del motor (x_px = OFX + H*vx/vz, y_px = OFY + H*vy*66/64/vz).
 * Este módulo da la matriz mundo del motor -> mundo VR de esa cámara y las cámaras estéreo (réplica de XRStereo.eye en app/Sources/XRMath.swift: mantener sincronizado).
 * Convenciones: matrices de 16 floats en orden de COLUMNAS (como simd); to_vr de 12 floats fila-mayor (3x4, como NsScene.to_vr). C puro (probado en Linux). */
#include "ns_types.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Mundo del motor -> mundo VR (m) de la cámara del juego: gv * diag(1, overscan_y, 1) * (R^T (p - eye)), con R = cam->rot (vista -> mundo) y gv = VrScene.game_view
 * (vista del motor en unidades -> mundo VR en metros). 0 si los datos no son finitos o gv es singular. */
int ns_cinema_to_vr(const NsCamera *cam, const float gv[16], float out[12]);

/* (ipd 0 = pantalla de cine 3D mono: ambos ojos idénticos.) Cámara estéreo de la pantalla 3D (réplica de XRStereo.eye): vista (mundo VR -> ojo GL: x derecha, y arriba, -z delante) y tangentes (izquierda, derecha, arriba, abajo).
 * 0 si los datos no son válidos. */
int ns_cinema_eye(const float gv[16], int eye, float ipd, float conv, float tan_x, float tan_y, float view[16], float tangents[4]);

/* Tangentes base de la ventana de la pantalla del motor con la H REAL de la escena (cam->h) y su centro de proyección (cam->ofx/ofy; 0 = centro 160/120):
 * x_px = OFX + H*vx/vz  =>  izquierda = OFX/H, derecha = (320-OFX)/H, arriba = OFY/H, abajo = (240-OFY)/H. Devuelve tan_x = 160/H, tan_y = 120/H (para ns_cinema_eye) y el
 * desplazamiento del centro `shift` = {dx, -dx, dy, -dy} (tangentes) que se SUMA a las tangentes de ns_cinema_eye. 0 si H no es válida. */
int ns_cinema_window(const NsCamera *cam, float *tan_x, float *tan_y, float shift[4]);

/* Proyección de Z invertida (réplica de XRMath.projection) a partir de las tangentes. */
void ns_cinema_projection(const float tangents[4], float znear, float zfar, float out[16]);

#ifdef __cplusplus
}
#endif
#endif
