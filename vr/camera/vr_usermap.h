/* vr_usermap: correspondencia mundo del juego -> espacio recentrado del usuario para vr_scene (VrSceneOpts.user_map_*).
 *
 * UNICA definición de la corrección mundo -> usuario (la usan vr/host, test_camera y camtest): con la cámara VR el motor dibuja con el ojo en
 * p_ref y el espacio del usuario girado theta + 180 grados respecto al mundo (W = ojo + upm * T (P - p_ref), T = R_y(theta + 180)); es la
 * inversa exacta de lo que hace vrcam_delta. `floor_y` = y del origen de la escena (suelo, unidades del motor) que se pasó en origin_game[1];
 * `o->scale` debe estar ya fijada. Solo cabecera (static inline). */
#pragma once
#include <math.h>
#include <string.h>
#include "vr_camera.h"
#include "vr_scene.h"

static inline void vrcam_scene_user_map(VrSceneOpts *o, const VrCamRecord *r, float floor_y) {
    float th = (r->theta_play_deg + 180.0f) * (3.14159265f / 180.0f), cs = cosf(th), sn = sinf(th);
    const float R[9] = {cs, 0, -sn, 0, 1, 0, sn, 0, cs};
    float upm = r->units_per_m > 1.0f ? r->units_per_m : 1000.0f;
    o->user_map_on = 1;
    memcpy(o->user_R, R, sizeof R);
    o->user_k = 1.0f / (o->scale * upm);
    o->user_eye_h_m = ((float)r->eye[1] - floor_y) / upm;
    memcpy(o->user_p_ref, r->p_ref, sizeof o->user_p_ref);
}

/* Mapa mundo del motor -> espacio recentrado del usuario para el render NATIVO (3x4 fila-mayor, metros). UNICA definición, la misma regla que aplica vr_scene_build
 * con user_map_on a los vértices de la lista de la GPU (la reflexión de Z de vr_scene se cancela entre la vista->mundo y el mapa de usuario):
 *   P = R * (k * (W - origen)) + p_ref - R * (0, eye_h, 0),   k = user_k * scale = 1 / unidades-por-metro.
 * `o` debe traer user_map_* ya fijados (vrcam_scene_user_map) y `org` es el origen del mundo VR (origin_game de la lista: bajo el ojo VR de ese fotograma).
 * Devuelve 0 si la escala no es válida. La prueba vr/camera/test_native_coh.c comprueba que el nativo y la lista de la GPU caen en el mismo sitio. */
static inline int vrcam_native_to_vr(const VrSceneOpts *o, const float org[3], float to_vr[12]) {
    float k = o->user_k * o->scale;
    if (!(k > 0.0f) || !isfinite(k)) return 0;
    for (int i = 0; i < 3; i++) {
        float c = o->user_p_ref[i] - o->user_R[3 * i + 1] * o->user_eye_h_m, a = 0.0f;
        for (int j = 0; j < 3; j++) { to_vr[4 * i + j] = o->user_R[3 * i + j] * k; a += to_vr[4 * i + j] * org[j]; }
        to_vr[4 * i + 3] = c - a;
    }
    return 1;
}
