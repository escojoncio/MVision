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
