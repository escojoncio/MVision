/* Núcleo de armas: matemática de poses, jerarquía de piezas y retroceso físico.
 *
 * Convención de canales (WPart): el valor del canal c = s->channel[part.channel] se aplica en el
 * marco del PADRE de la pieza, con el pivote en part.rest.p:
 *   - rotates == 0: p = rest.p + axis * c            (axis en m por unidad de canal)
 *   - rotates == 1: q = R(axis/|axis|, |axis| * c) * rest.q   (axis en rad por unidad de canal)
 * Los padres deben aparecer antes que sus hijos en def->parts. */
#include "wcore.h"
#include <string.h>

/* ---------------- cuaterniones ---------------- */
WQuat wquat_identity(void) { WQuat q = {0, 0, 0, 1}; return q; }
WQuat wquat_mul(WQuat a, WQuat b) {
    WQuat r = { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
    return r;
}
WQuat wquat_inv(WQuat q) { WQuat r = {-q.x, -q.y, -q.z, q.w}; return r; }
WQuat wquat_normalize(WQuat q) {
    float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (l < 1e-12f) return wquat_identity();
    float i = 1.0f / l; WQuat r = {q.x * i, q.y * i, q.z * i, q.w * i}; return r;
}
WVec3 wquat_rotate(WQuat q, WVec3 v) {
    WVec3 u = {q.x, q.y, q.z};
    WVec3 t = wv_scale(wv_cross(u, v), 2.0f);
    return wv_add(wv_add(v, wv_scale(t, q.w)), wv_cross(u, t));
}
WQuat wquat_axis_angle(WVec3 axis, float rad) {
    WVec3 a = wv_norm(axis);
    float h = 0.5f * rad, s = sinf(h);
    WQuat q = {a.x * s, a.y * s, a.z * s, cosf(h)};
    return q;
}
WQuat wquat_from_rotvec(WVec3 r) {
    float a = wv_len(r);
    if (a < 1e-9f) { WQuat q = {0.5f * r.x, 0.5f * r.y, 0.5f * r.z, 1.0f}; return wquat_normalize(q); }
    return wquat_axis_angle(r, a);
}
WVec3 wquat_to_rotvec(WQuat q) {
    if (q.w < 0) { q.x = -q.x; q.y = -q.y; q.z = -q.z; q.w = -q.w; }
    float s = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z);
    if (s < 1e-9f) return wv(2 * q.x, 2 * q.y, 2 * q.z);
    float ang = 2.0f * atan2f(s, q.w);
    return wv_scale(wv(q.x, q.y, q.z), ang / s);
}
WQuat wquat_slerp(WQuat a, WQuat b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; d = -d; }
    if (d > 0.9995f) {
        WQuat r = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
        return wquat_normalize(r);
    }
    float th = acosf(d), sn = sinf(th);
    float wa = sinf((1 - t) * th) / sn, wb = sinf(t * th) / sn;
    WQuat r = {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
    return r;
}

/* ---------------- poses ---------------- */
WPose wpose_identity(void) { WPose p = {{0, 0, 0}, {0, 0, 0, 1}}; return p; }
WPose wpose_mul(WPose a, WPose b) {
    WPose r; r.p = wv_add(a.p, wquat_rotate(a.q, b.p)); r.q = wquat_normalize(wquat_mul(a.q, b.q)); return r;
}
WVec3 wpose_apply(WPose a, WVec3 v) { return wv_add(a.p, wquat_rotate(a.q, v)); }
WPose wpose_inv(WPose a) {
    WPose r; r.q = wquat_inv(a.q); r.p = wv_scale(wquat_rotate(r.q, a.p), -1.0f); return r;
}
WPose wpose_lerp(WPose a, WPose b, float t) {
    WPose r; r.p = wv_lerp(a.p, b.p, t); r.q = wquat_slerp(a.q, b.q, t); return r;
}

int wcore_part_find(const WeaponState *s, const char *name) {
    int n; const WPart *p = weapon_parts(s, &n);
    if (!name) return -1;
    for (int i = 0; i < n; i++) if (p[i].name && !strcmp(p[i].name, name)) return i;
    return -1;
}

void wcore_state_reset(WeaponState *s, const WeaponDef *d) {
    const WMesh *m = s->meshes; int nm = s->nmeshes;
    const WPart *p = s->parts; int np = s->nparts;
    memset(s, 0, d->state_size);
    s->def = d;
    s->meshes = m; s->nmeshes = nm; s->parts = p; s->nparts = np;   /* el binding sobrevive al reinicio */
    s->recoil_offset.q = wquat_identity();
    for (int i = 0; i < W_MAX_HELD; i++) s->held[i].mesh = -1;
}

float wcore_rand(uint32_t *seed) {
    uint32_t x = *seed ? *seed : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;   /* xorshift32 */
    *seed = x;
    return (float)(x >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

/* ---------------- binding de modelos ---------------- */
void weapon_bind(WeaponState *s, const WMesh *m, int n, const WPart *p, int np) {
    s->meshes = m; s->nmeshes = n; s->parts = p; s->nparts = p ? np : 0;
}
const WMesh *weapon_meshes(const WeaponState *s, int *n) {
    if (s->meshes) { *n = s->nmeshes; return s->meshes; }
    *n = s->def->nmeshes; return s->def->meshes;
}
const WPart *weapon_parts(const WeaponState *s, int *n) {
    if (s->parts) { *n = s->nparts; return s->parts; }
    *n = s->def->nparts; return s->def->parts;
}
int weapon_mesh_count(const WeaponDef *d) {
    int n = 0;
    if (d && d->mesh_names) while (d->mesh_names[n]) n++;
    return n;
}
int weapon_part_mesh(const WeaponState *s, int i) {
    int n; const WPart *p = weapon_parts(s, &n);
    if (i < 0 || i >= n) return -1;
    if (p[i].mesh >= 0) return p[i].mesh;
    if (!p[i].name || !s->def->mesh_names) return -1;
    for (int k = 0; s->def->mesh_names[k]; k++) if (!strcmp(s->def->mesh_names[k], p[i].name)) return k;
    return -1;
}

/* ---------------- jerarquía de piezas ---------------- */
static WPose part_local(const WeaponState *s, const WPart *p) {
    WPose l = p->rest;
    float c = (p->channel > WCH_NONE && p->channel < WCH_COUNT) ? s->channel[p->channel] : 0.0f;
    if (c != 0.0f) {
        if (p->rotates) {
            WVec3 rv = wv_scale(p->axis, c);
            l.q = wquat_normalize(wquat_mul(wquat_from_rotvec(rv), l.q));
        } else {
            l.p = wv_add(l.p, wv_scale(p->axis, c));
        }
    }
    return l;
}

int wcore_part_poses(const WeaponState *s, WPose weapon_world, WPose *out, int max) {
    int np; const WPart *parts = weapon_parts(s, &np);
    if (np > max) np = max;
    for (int i = 0; i < np; i++) {
        const WPart *p = &parts[i];
        WPose l = part_local(s, p);
        if (p->parent < 0 || p->parent >= i) out[i] = wpose_mul(weapon_world, l);
        else out[i] = wpose_mul(out[p->parent], l);
    }
    return np;
}

/* ---------------- pose del arma ---------------- */
WPose wcore_weapon_world(const WeaponState *s, const WInput *in) {
    WPose grip = s->def->points[WP_GRIP];
    return wpose_mul(wpose_mul(in->hand[0], s->recoil_offset), wpose_inv(grip));
}

WPose wcore_point_world(const WeaponState *s, const WInput *in, WPoint p) {
    return wpose_mul(wcore_weapon_world(s, in), s->def->points[p]);
}

float wcore_hand_dist(const WeaponState *s, const WInput *in, int hand, WPoint p) {
    WPose pw = wcore_point_world(s, in, p);
    return wv_len(wv_sub(in->hand[hand & 1].p, pw.p));
}

/* ---------------- retroceso ----------------
 * Modelo: sistema masa unidad con resorte k y amortiguador c, lineal y angular, en el marco de la mano.
 *   a = -k x - c v     (x = desplazamiento / vector de rotación del arma respecto al agarre)
 * Integración semi-implícita con subpasos de <= 2 ms (estable con k de varios cientos).
 * Con damping = 2*sqrt(k)*zeta, zeta ~0,85 el retorno apenas rebasa (sin oscilar). */
void wcore_recoil_step(WeaponState *s, const WInput *in, float dt) {
    (void)in;
    const WRecoil *r = &s->def->recoil;
    dt = wcore_dt(dt, 0.1f);
    if (dt <= 0) return;
    int n = (int)ceilf(dt / 0.002f); if (n < 1) n = 1;
    float h = dt / n;
    WVec3 x = s->recoil_offset.p;
    WVec3 rv = wquat_to_rotvec(s->recoil_offset.q);
    WVec3 v = s->recoil_vel, w = s->recoil_angvel;
    for (int i = 0; i < n; i++) {
        v = wv_add(v, wv_scale(wv_add(wv_scale(x, -r->stiffness), wv_scale(v, -r->damping)), h));
        x = wv_add(x, wv_scale(v, h));
        w = wv_add(w, wv_scale(wv_add(wv_scale(rv, -r->stiffness), wv_scale(w, -r->damping)), h));
        rv = wv_add(rv, wv_scale(w, h));
    }
    s->recoil_offset.p = x;
    s->recoil_offset.q = wquat_from_rotvec(rv);
    s->recoil_vel = v; s->recoil_angvel = w;
}

void wcore_kick(WeaponState *s, const WInput *in, uint32_t seed) {
    const WeaponDef *d = s->def;
    const WRecoil *r = &d->recoil;
    float f = in->support_held ? r->two_hand_factor : 1.0f;
    /* ejes del arma pasados al marco de la mano: weapon_in_hand = grip^-1 */
    WPose wih = wpose_inv(d->points[WP_GRIP]);
    WVec3 fwd_w = wquat_rotate(d->points[WP_MUZZLE].q, wv(0, 0, -1));
    WVec3 back = wquat_rotate(wih.q, wv_scale(fwd_w, -1.0f));
    WVec3 right = wquat_rotate(wih.q, wv(1, 0, 0));
    WVec3 up = wquat_rotate(wih.q, wv(0, 1, 0));
    uint32_t sd = seed ^ 0xA5F152C3u;
    float ry = wcore_rand(&sd);
    float rb = 1.0f + 0.15f * wcore_rand(&sd);      /* variación de potencia ±15 % */
    s->recoil_vel = wv_add(s->recoil_vel, wv_scale(back, r->kick_back * f * rb));
    /* cabeceo hacia arriba: rotación + alrededor del eje derecho del arma (levanta el cañón) */
    s->recoil_angvel = wv_add(s->recoil_angvel, wv_scale(right, r->kick_up * f * rb));
    s->recoil_angvel = wv_add(s->recoil_angvel, wv_scale(up, r->kick_yaw_rand * f * ry));
}
