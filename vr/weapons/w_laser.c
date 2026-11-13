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

/* ---- fantasma de colocación (ver w_laser.h) ---- */
static void ghost_strip(WGhostMesh *o, int k, WVec3 a, WVec3 b) {
    WVec3 d = wv_sub(b, a), n = wv(-d.z, 0, d.x);
    float nl = sqrtf(n.x * n.x + n.z * n.z);
    n = nl > 1e-6f ? wv_scale(n, WGHOST_STRIP_HALF_W / nl) : wv(WGHOST_STRIP_HALF_W, 0, 0);
    const WVec3 c[4] = {wv_sub(a, n), wv_add(a, n), wv_add(b, n), wv_sub(b, n)};
    for (int i = 0; i < 4; i++) {
        VrHandsVertex *v = &o->v[4 * k + i];
        put(v, c[i], wv(0, 1, 0));
        v->rgba[0] = 0; v->rgba[1] = 255; v->rgba[2] = 0;
    }
    uint16_t *ix = &o->idx[12 * k];
    const uint16_t b0 = (uint16_t)(4 * k);
    static const uint16_t f[12] = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};     /* las dos caras */
    for (int i = 0; i < 12; i++) ix[i] = (uint16_t)(b0 + f[i]);
}

int wghost_build(const WPose *placed, float arrow_len, WGhostMesh *out) {
    if (!out || !placed || !isfinite(placed->p.x) || !isfinite(placed->p.y) || !isfinite(placed->p.z) || !isfinite(placed->q.x) || !isfinite(placed->q.y) || !isfinite(placed->q.z) || !isfinite(placed->q.w)) return 0;
    if (!(arrow_len > 0.0f) || !isfinite(arrow_len)) return 0;
    const float qn = placed->q.x * placed->q.x + placed->q.y * placed->q.y + placed->q.z * placed->q.z + placed->q.w * placed->q.w;
    if (!(qn > 1e-6f)) return 0;
    const WQuat q = wquat_normalize(placed->q);
    WVec3 f = wquat_rotate(q, wv(1, 0, 0)), r = wquat_rotate(q, wv(0, 0, 1));         /* +X del modelo (cara picuda) y +Z, tendidos en horizontal */
    f.y = 0; r.y = 0;
    const float fl = sqrtf(f.x * f.x + f.z * f.z), rl = sqrtf(r.x * r.x + r.z * r.z);
    if (!(fl > 1e-4f) || !(rl > 1e-4f)) return 0;
    f = wv_scale(f, 1.0f / fl); r = wv_scale(r, 1.0f / rl);
    const WVec3 o = wv(placed->p.x, placed->p.y + WGHOST_LIFT_M, placed->p.z);
#define GP(x, z) wv_add(o, wv_add(wv_scale(f, (x)), wv_scale(r, (z))))
    const WVec3 q4[4] = {GP(WGHOST_MINE_X1, WGHOST_MINE_Z0), GP(WGHOST_MINE_X1, WGHOST_MINE_Z1), GP(WGHOST_MINE_X0, WGHOST_MINE_Z1), GP(WGHOST_MINE_X0, WGHOST_MINE_Z0)};
    const WVec3 base = GP(WGHOST_MINE_X1, WGHOST_MINE_ZA);
#undef GP
    for (int i = 0; i < 4; i++) ghost_strip(out, i, q4[i], q4[(i + 1) & 3]);
    const WVec3 tip = wv_add(base, wv_scale(f, arrow_len));
    ghost_strip(out, 4, base, tip);
    const float hl = 0.25f * arrow_len;
    ghost_strip(out, 5, tip, wv_add(tip, wv_add(wv_scale(f, -hl), wv_scale(r, hl))));
    ghost_strip(out, 6, tip, wv_add(tip, wv_sub(wv_scale(f, -hl), wv_scale(r, hl))));
    return 1;
}

void wghost_mesh(const WGhostMesh *m, VrHandsMesh *out) {
    memset(out, 0, sizeof *out);
    out->mesh_id = WGHOST_MESH_ID;
    out->tex_id = 0;
    out->model[0] = out->model[5] = out->model[10] = out->model[15] = 1.0f;
    out->verts = m->v; out->nverts = WGHOST_NV;
    out->idx = m->idx; out->nidx = WGHOST_NI;
    out->flags = VRH_MF_DYNAMIC;
}
