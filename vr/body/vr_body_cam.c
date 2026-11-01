/* Fuente de la vista de vr_body basada en vr/camera: ojo del motor, referencia de cabeza y giro del registro de la
 * cámara construida en este frame (la misma con la que el motor va a dibujar el cuerpo). */
#include <math.h>
#include "vr_body.h"
#include "../camera/vr_camera.h"

static int cam_view(VrBodyView *out) {
    if (!vrcam_active()) return 0;
    const VrCamRecord *r = vrcam_last_record();
    if (!r || !r->active) return 0;
    out->active = 1;
    for (int i = 0; i < 3; i++) { out->eye[i] = (float)r->eye[i]; out->p_ref[i] = r->p_ref[i]; }
    out->theta_deg = r->theta_play_deg;
    for (int i = 0; i < 3; i++) out->shift[i] = r->eye_shift[i];
    out->units_per_m = r->units_per_m;
    out->gaze_yaw_deg = vrcam_view_yaw_deg();       /* mirada real del usuario (la usa torso_follow) */
    return 1;
}

void vrbody_attach_vrcam(void) { vrbody_set_view_fn(cam_view); }
