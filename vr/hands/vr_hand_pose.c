/* vr_hand_pose: ver vr_hand_pose.h. */
#include "vr_hand_pose.h"
#include <math.h>

typedef struct { float curl[5]; float spread; float pulled; } PoseDef;   /* pulled: cierre del índice con el gatillo a fondo; < 0 = sin gatillo */

static const PoseDef TABLE[HP_COUNT] = {
    [HP_OPEN]         = {{0.00f, 0.00f, 0.00f, 0.00f, 0.00f},  0.50f, -1.0f},
    [HP_RELAX]        = {{0.15f, 0.20f, 0.26f, 0.32f, 0.12f},  0.00f, -1.0f},
    [HP_FIST]         = {{1.00f, 1.00f, 1.00f, 1.00f, 1.00f},  0.00f, -1.0f},
    [HP_POINT]        = {{0.00f, 1.00f, 1.00f, 1.00f, 0.75f},  0.00f, -1.0f},
    [HP_TRIGGER]      = {{0.22f, 1.00f, 1.00f, 1.00f, 0.60f},  0.00f, 0.85f},
    [HP_GRIP_PISTOL]  = {{0.30f, 0.60f, 0.55f, 0.50f, 0.60f}, -0.20f, 0.60f},
    [HP_GRIP_RIFLE]   = {{0.22f, 0.70f, 0.60f, 0.50f, 0.75f}, -0.30f, 0.42f},
    [HP_GRIP_SUPPORT] = {{0.62f, 0.68f, 0.72f, 0.72f, 0.22f},  0.30f, -1.0f},
    [HP_GRIP_SPHERE]  = {{0.50f, 0.52f, 0.55f, 0.60f, 0.40f},  0.20f, -1.0f},
    [HP_GRIP_PLACED]  = {{0.45f, 0.50f, 0.55f, 0.55f, 0.20f},  0.30f, -1.0f},
    [HP_GRIP_BOX]     = {{0.60f, 0.62f, 0.64f, 0.66f, 0.45f},  0.10f, -1.0f},
};

static float fin(float x) { return x == x && x - x == 0.0f; }
static float c01(float x) { return fin(x) ? (x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x)) : 0.0f; }
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

int vrhandpose_has_trigger(int pose) { return pose >= 0 && pose < HP_COUNT && TABLE[pose].pulled >= 0.0f; }

void vrhandpose_curls(const VrHandPoseIn *in, VrHandCurls *out) {
    int item = (in && in->item_pose >= 0 && in->item_pose < HP_COUNT) ? in->item_pose : HP_RELAX;
    float g = in ? c01(in->grip) : 0.0f, tr = in ? c01(in->trigger) : 0.0f, tb = in ? c01(in->thumb_btn) : 0.0f, fb = in ? c01(in->fist_btn) : 0.0f;
    int target = item;
    if (item == HP_RELAX) { target = HP_FIST; if (fb > g) g = fb; }     /* sin objeto: el agarre y el botón de puño cierran el puño */
    const PoseDef *rel = &TABLE[HP_RELAX], *tg = &TABLE[target];
    for (int i = 0; i < 5; i++) out->curl[i] = lerpf(rel->curl[i], tg->curl[i], g);
    out->thumb_spread = lerpf(rel->spread, tg->spread, g);
    if (tg->pulled >= 0.0f) out->curl[0] = lerpf(out->curl[0], tg->pulled, tr);
    out->curl[4] = lerpf(out->curl[4], 0.0f, tb);                       /* el pulgar se levanta hacia los botones de cara */
    out->thumb_spread = lerpf(out->thumb_spread, 0.6f, tb);
}

void vrhandpose_blend(int side, const VrHandPoseIn *in, float xf[VRHS_J][12]) {
    VrHandCurls c;
    vrhandpose_curls(in, &c);
    vrhs_pose(c.curl, c.thumb_spread, side, xf);
}

void vrhandpose_index_tip(const float xf[VRHS_J][12], float out[3]) { vrhs_index_pad(xf, out); }
