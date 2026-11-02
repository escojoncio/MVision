/* w_laser: ver w_laser.h. */
#include <math.h>
#include <string.h>
#include "w_laser.h"
#include "wcore.h"

static int pose_ok(const WPose *p) {
    if (!p) return 0;
    if (!isfinite(p->p.x) || !isfinite(p->p.y) || !isfinite(p->p.z)) return 0;
    if (!isfinite(p->q.x) || !isfinite(p->q.y) || !isfinite(p->q.z) || !isfinite(p->q.w)) return 0;
    return p->q.x * p->q.x + p->q.y * p->q.y + p->q.z * p->q.z + p->q.w * p->q.w > 1e-12f;
}

int wlaser_raised(const WPose *muzzle) {
    if (!pose_ok(muzzle)) return 0;
    WVec3 f = wquat_rotate(wquat_normalize(muzzle->q), wv(0, 0, -1));
    return f.y >= WLASER_MIN_FWD_Y;
}

float wlaser_length(float hit_m) {
    if (!isfinite(hit_m) || hit_m < 0.0f) return WLASER_MAX_M;
    float l = hit_m - WLASER_SURF_GAP_M;
    return l < 0.0f ? 0.0f : (l > WLASER_MAX_M ? WLASER_MAX_M : l);
}

static void put(VrHandsVertex *v, WVec3 p, WVec3 n) {
    v->pos[0] = p.x; v->pos[1] = p.y; v->pos[2] = p.z;
    v->nrm[0] = n.x; v->nrm[1] = n.y; v->nrm[2] = n.z;
    v->uv[0] = 0; v->uv[1] = 0;
    v->rgba[0] = 255; v->rgba[1] = 0; v->rgba[2] = 0; v->rgba[3] = 255;
}

int wlaser_build(const WPose *muzzle, float length_m, WLaserMesh *out) {
    if (!out || !pose_ok(muzzle) || !isfinite(length_m) || length_m < WLASER_MIN_M) return 0;
    float len = length_m > WLASER_MAX_M ? WLASER_MAX_M : length_m;
    WQuat q = wquat_normalize(muzzle->q);
    WVec3 r = wquat_rotate(q, wv(1, 0, 0)), u = wquat_rotate(q, wv(0, 1, 0)), f = wquat_rotate(q, wv(0, 0, -1));
    WVec3 o = muzzle->p;
    /* prisma: anillo cercano (0..3, en la boca) y lejano (4..7, a `len`), esquinas (-1,-1) (1,-1) (1,1) (-1,1) en (derecha, arriba); normal = hacia la esquina */
    static const int sr[4] = {-1, 1, 1, -1}, su[4] = {-1, -1, 1, 1};
    for (int k = 0; k < 2; k++) {
        WVec3 c = wv_add(o, wv_scale(f, k ? len : 0.0f));
        for (int i = 0; i < 4; i++) {
            WVec3 d = wv_add(wv_scale(r, (float)sr[i]), wv_scale(u, (float)su[i]));
            put(&out->v[4 * k + i], wv_add(c, wv_scale(d, WLASER_HALF_W)), wv_scale(d, 0.70710678f));
        }
    }
    uint16_t *ix = out->idx;
    for (int i = 0; i < 4; i++) {
        int j = (i + 1) & 3;
        *ix++ = (uint16_t)i; *ix++ = (uint16_t)j; *ix++ = (uint16_t)(4 + j);
        *ix++ = (uint16_t)i; *ix++ = (uint16_t)(4 + j); *ix++ = (uint16_t)(4 + i);
    }
    /* punto: octaedro con la punta lejana en el extremo del rayo (centro a `h` antes) */
    float h = WLASER_DOT_ANG * len;
    if (h < WLASER_DOT_MIN) h = WLASER_DOT_MIN;
    WVec3 c = wv_add(o, wv_scale(f, len - h));
    WVec3 ax[6] = {r, wv_scale(r, -1.0f), u, wv_scale(u, -1.0f), f, wv_scale(f, -1.0f)};
    for (int i = 0; i < 6; i++) put(&out->v[8 + i], wv_add(c, wv_scale(ax[i], h)), ax[i]);
    for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) for (int c2 = 0; c2 < 2; c2++) {
        *ix++ = (uint16_t)(8 + a); *ix++ = (uint16_t)(10 + b); *ix++ = (uint16_t)(12 + c2);
    }
    return 1;
}

void wlaser_mesh(const WLaserMesh *m, VrHandsMesh *out) {
    memset(out, 0, sizeof *out);
    out->mesh_id = WLASER_MESH_ID;
    out->tex_id = 0;
    out->model[0] = out->model[5] = out->model[10] = out->model[15] = 1.0f;
    out->verts = m->v; out->nverts = WLASER_NV;
    out->idx = m->idx; out->nidx = WLASER_NI;
    out->flags = VRH_MF_DYNAMIC;
}
