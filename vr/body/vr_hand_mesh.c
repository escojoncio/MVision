/* vr_hand_mesh: ver vr_hand_mesh.h. */
#include "vr_hand_mesh.h"
#include <math.h>
#include <string.h>

#define PI_F 3.14159265358979f
#define DEG (PI_F / 180.0f)

/* ------------------------------------------------------------------------------------------------ matemática mínima */
static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static int isfin(float x) { return x == x && x - x == 0.0f; }
static void v3_cross(float *o, const float *a, const float *b) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static float v3_dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float v3_len(const float *a) { return sqrtf(v3_dot(a, a)); }
static void v3_norm(float *a) { float l = v3_len(a); if (l > 1e-9f) { a[0] /= l; a[1] /= l; a[2] /= l; } }

static void xf_ident(float *x) { memset(x, 0, 12 * sizeof(float)); x[0] = x[5] = x[10] = 1.0f; }
/* o = a * b (3x4); o puede ser a o b. */
static void xf_mul(float *o, const float *a, const float *b) {
    float r[12];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) r[4 * i + j] = a[4 * i] * b[j] + a[4 * i + 1] * b[4 + j] + a[4 * i + 2] * b[8 + j];
        r[4 * i + 3] = a[4 * i] * b[3] + a[4 * i + 1] * b[7] + a[4 * i + 2] * b[11] + a[4 * i + 3];
    }
    memcpy(o, r, sizeof r);
}
static void xf_from_rt(float *x, const float *R9, const float *t) {
    for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) x[4 * i + j] = R9[3 * i + j]; x[4 * i + 3] = t ? t[i] : 0.0f; }
}
static void xf_apply(const float *x, const float *p, float *o) {
    o[0] = x[0] * p[0] + x[1] * p[1] + x[2] * p[2] + x[3];
    o[1] = x[4] * p[0] + x[5] * p[1] + x[6] * p[2] + x[7];
    o[2] = x[8] * p[0] + x[9] * p[1] + x[10] * p[2] + x[11];
}
/* Rodrigues, R fila-mayor. */
static void rot_axis(float *R, const float *axis, float ang) {
    float a[3] = {axis[0], axis[1], axis[2]};
    v3_norm(a);
    float c = cosf(ang), s = sinf(ang), t = 1.0f - c;
    R[0] = t * a[0] * a[0] + c;        R[1] = t * a[0] * a[1] - s * a[2]; R[2] = t * a[0] * a[2] + s * a[1];
    R[3] = t * a[0] * a[1] + s * a[2]; R[4] = t * a[1] * a[1] + c;        R[5] = t * a[1] * a[2] - s * a[0];
    R[6] = t * a[0] * a[2] - s * a[1]; R[7] = t * a[1] * a[2] + s * a[0]; R[8] = t * a[2] * a[2] + c;
}
static void m3_mul(float *o, const float *a, const float *b) {
    float r[9];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r[3 * i + j] = a[3 * i] * b[j] + a[3 * i + 1] * b[3 + j] + a[3 * i + 2] * b[6 + j];
    memcpy(o, r, sizeof r);
}

/* ------------------------------------------------------------------------------------------------ medidas y composición */
void vrhand_dims_default(VrHandDims *d) {
    d->cuff_y = 22.0f; d->cuff_c[0] = 7.5f; d->cuff_c[1] = -15.5f; d->cuff_h[0] = 27.5f; d->cuff_h[1] = 33.5f; d->tip_y = -125.0f;
}

int vrhand_dims_from_verts(const int16_t *xyz, int nv, int stride_i16, int side, VrHandDims *d) {
    vrhand_dims_default(d);
    if (!xyz || nv < 8 || nv > 4096 || stride_i16 < 3) return 0;
    float sg = side < 0 ? -1.0f : 1.0f;
    int ymax = -32768, ymin = 32767;
    for (int i = 0; i < nv; i++) { int y = xyz[i * stride_i16 + 1]; if (y > ymax) ymax = y; if (y < ymin) ymin = y; }
    float len = (float)(ymax - ymin);
    if (!(len >= 60.0f && len <= 300.0f)) return 0;
    int n = 0, x0 = 32767, x1 = -32768, z0 = 32767, z1 = -32768;
    for (int i = 0; i < nv; i++) {
        const int16_t *p = xyz + i * stride_i16;
        if (p[1] < ymax - 2) continue;
        int x = side < 0 ? -p[0] : p[0];
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (p[2] < z0) z0 = p[2];
        if (p[2] > z1) z1 = p[2];
        n++;
    }
    float hx = 0.5f * (float)(x1 - x0), hz = 0.5f * (float)(z1 - z0);
    if (n < 3 || !(hx >= 10.0f && hx <= 60.0f && hz >= 10.0f && hz <= 70.0f)) return 0;
    (void)sg;
    d->cuff_y = (float)ymax; d->tip_y = (float)ymin;
    d->cuff_c[0] = 0.5f * (float)(x0 + x1); d->cuff_c[1] = 0.5f * (float)(z0 + z1);
    d->cuff_h[0] = hx; d->cuff_h[1] = hz;
    return 1;
}

void vrhand_pose_cfg_default(VrHandPoseCfg *p) {
    memset(p, 0, sizeof *p);
    p->open_deg[0] = 6.0f; p->open_deg[1] = 12.0f; p->open_deg[2] = 6.0f;
    static const float cl[4][3] = {{72, 82, 60}, {72, 84, 60}, {72, 84, 60}, {72, 84, 58}};
    memcpy(p->closed_deg, cl, sizeof cl);
    static const float to[4] = {0, 0, 12, 8};
    static const float tc[4] = {48, 16, 3, 5};
    memcpy(p->thumb_open, to, sizeof to);
    memcpy(p->thumb_closed, tc, sizeof tc);
    p->thumb_start = 0.35f;
}

typedef struct { float y, cx, cz, hx, hz; } PalmRing;
typedef struct {
    float Lh, sc, yk;
    PalmRing ring[3];                   /* puño, centro, nudillos */
    float fx, fz[4], fr[4], flen[4][3], fr0[4][3], fr1[4][3];
    float tp[3], tlen[3], tr0[3], tr1[3], tdir[3];
} Lay;

static void layout(const VrHandDims *dm, Lay *L) {
    VrHandDims d = *dm;
    float Lh = clampf(d.cuff_y - d.tip_y, 60.0f, 300.0f);
    L->Lh = Lh; L->sc = Lh / 147.0f;
    float sc = L->sc;
    float chx = clampf(d.cuff_h[0], 10.0f, 60.0f), chz = clampf(d.cuff_h[1], 10.0f, 70.0f);
    L->yk = d.cuff_y - 0.565f * Lh;
    float kx = clampf(d.cuff_c[0] - 13.5f * sc, -10.0f * sc, 0.0f);
    L->ring[0] = (PalmRing){d.cuff_y + 6.0f * sc, d.cuff_c[0], d.cuff_c[1], 0.95f * chx, 0.95f * chz};
    L->ring[2] = (PalmRing){L->yk + 1.5f * sc, kx, 0.0f, 0.47f * chx, 1.25f * chz};
    float y1 = d.cuff_y - 0.30f * Lh;
    float t = (L->ring[0].y - y1) / (L->ring[0].y - L->ring[2].y);
    L->ring[1] = (PalmRing){y1, L->ring[0].cx + (L->ring[2].cx - L->ring[0].cx) * t, L->ring[0].cz + (L->ring[2].cz - L->ring[0].cz) * t, 0.72f * chx, 0.87f * chz};
    /* dedos */
    float W = 2.0f * L->ring[2].hz, dsp = W / 4.0f;
    L->fx = L->ring[2].cx + 1.5f * sc;
    static const float rf[4] = {1.0f, 1.0f, 0.98f, 0.86f}, lf[4] = {0.94f, 1.0f, 0.95f, 0.80f}, seg[3] = {0.47f, 0.29f, 0.24f};
    static const float tapr0[3] = {1.0f, 0.85f, 0.73f}, tapr1[3] = {0.97f, 0.83f, 0.70f};
    for (int i = 0; i < 4; i++) {
        L->fz[i] = L->ring[2].cz + dsp * (1.5f - (float)i);
        L->fr[i] = 0.5f * dsp * 0.99f * rf[i];
        float ext = (L->yk - d.tip_y) * lf[i];
        float tube = ext - 0.643f * L->fr[i] * tapr1[2];
        for (int j = 0; j < 3; j++) { L->flen[i][j] = tube * seg[j]; L->fr0[i][j] = L->fr[i] * tapr0[j]; L->fr1[i][j] = L->fr[i] * tapr1[j]; }
    }
    /* pulgar */
    float yc = d.cuff_y - 0.27f * Lh;
    float tt = (L->ring[0].y - yc) / (L->ring[0].y - L->ring[2].y);
    float cx = L->ring[0].cx + (L->ring[2].cx - L->ring[0].cx) * tt, cz = L->ring[0].cz + (L->ring[2].cz - L->ring[0].cz) * tt;
    float hx = L->ring[0].hx + (L->ring[2].hx - L->ring[0].hx) * tt, hz = L->ring[0].hz + (L->ring[2].hz - L->ring[0].hz) * tt;
    if (tt > 0.45f) { hx = L->ring[1].hx; hz = L->ring[1].hz; }
    L->tp[0] = cx + 0.40f * hx; L->tp[1] = yc; L->tp[2] = cz + 1.0f * hz;
    float tlen[3] = {0.18f * Lh, 0.155f * Lh, 0.12f * Lh};
    static const float tr0[3] = {12.5f, 10.1f, 8.6f}, tr1[3] = {11.5f, 9.8f, 8.2f};
    for (int j = 0; j < 3; j++) { L->tlen[j] = tlen[j] - (j == 2 ? 0.643f * tr1[2] * sc : 0.0f); L->tr0[j] = tr0[j] * sc; L->tr1[j] = tr1[j] * sc; }
    L->tdir[0] = 0.05f; L->tdir[1] = -0.78f; L->tdir[2] = 0.62f; v3_norm(L->tdir);
}

/* Sección de la palma a la altura y (interpolación lineal entre los tres anillos, acotada). */
static void palm_section(const Lay *L, float y, float *cx, float *cz, float *hx, float *hz) {
    const PalmRing *a, *b;
    if (y >= L->ring[1].y) { a = &L->ring[0]; b = &L->ring[1]; } else { a = &L->ring[1]; b = &L->ring[2]; }
    float t = (a->y - y) / (a->y - b->y);
    t = clampf(t, 0.0f, 1.0f);
    *cx = a->cx + (b->cx - a->cx) * t; *cz = a->cz + (b->cz - a->cz) * t; *hx = a->hx + (b->hx - a->hx) * t; *hz = a->hz + (b->hz - a->hz) * t;
}

/* ------------------------------------------------------------------------------------------------ constructor de mallas */
typedef struct {
    int nv, nf;
    float v[VRHAND_MAX_V][3];
    uint8_t f[VRHAND_MAX_F][4], role[VRHAND_MAX_F], su[VRHAND_MAX_F][4], sv[VRHAND_MAX_F][4];
} Tmp;

static int add_ring(Tmp *t, int n, float y, float cx, float cz, float a, float b, float phase_deg) {
    int base = t->nv;
    for (int k = 0; k < n && t->nv < VRHAND_MAX_V; k++) {
        float ph = (phase_deg + 360.0f * (float)k / (float)n) * DEG;
        t->v[t->nv][0] = cx + a * cosf(ph); t->v[t->nv][1] = y; t->v[t->nv][2] = cz + b * sinf(ph);
        t->nv++;
    }
    return base;
}
static void add_face(Tmp *t, int a, int b, int c, int d, int role, const uint8_t su[4], const uint8_t sv[4]) {
    if (t->nf >= VRHAND_MAX_F) return;
    t->f[t->nf][0] = (uint8_t)a; t->f[t->nf][1] = (uint8_t)b; t->f[t->nf][2] = (uint8_t)c; t->f[t->nf][3] = (uint8_t)d;
    t->role[t->nf] = (uint8_t)role;
    memcpy(t->su[t->nf], su, 4); memcpy(t->sv[t->nf], sv, 4);
    t->nf++;
}
/* Banda entre dos anillos de n vértices (a hacia b). roles[k]: rol de la cara k (NULL = role); patch = 1: cada cara cubre toda la región (palma/dorso), si no el parámetro avanza alrededor. */
static void add_band(Tmp *t, int n, int ra, int rb, int role, const uint8_t *roles, float sv0, float sv1) {
    for (int k = 0; k < n; k++) {
        int k1 = (k + 1) % n;
        int r = roles ? roles[k] : role;
        uint8_t su[4], sv[4];
        uint8_t s0 = (uint8_t)(255.0f * (float)k / (float)n), s1 = (uint8_t)(255.0f * (float)(k + 1) / (float)n);
        uint8_t v0 = (uint8_t)(255.0f * sv0), v1 = (uint8_t)(255.0f * sv1);
        if (r == VRHAND_ROLE_PALM || r == VRHAND_ROLE_BACK) { su[0] = 0; su[1] = 255; su[2] = 255; su[3] = 0; }
        else { su[0] = s0; su[1] = s1; su[2] = s1; su[3] = s0; }
        sv[0] = v0; sv[1] = v0; sv[2] = v1; sv[3] = v1;
        add_face(t, ra + k, ra + k1, rb + k1, rb + k, r, su, sv);
    }
}
/* Tapa plana de un anillo de n vértices (n par: n/2 - 1 cuadriláteros en abanico por pares). */
static void add_cap(Tmp *t, int n, int r0, int role, float sv) {
    uint8_t su[4] = {0, 255, 255, 0}, svv[4];
    uint8_t v = (uint8_t)(255.0f * sv);
    svv[0] = svv[1] = svv[2] = svv[3] = v;
    if (n == 6) {
        add_face(t, r0 + 0, r0 + 1, r0 + 2, r0 + 3, role, su, svv);
        add_face(t, r0 + 3, r0 + 4, r0 + 5, r0 + 0, role, su, svv);
    } else {   /* 8 */
        add_face(t, r0 + 0, r0 + 1, r0 + 2, r0 + 3, role, su, svv);
        add_face(t, r0 + 3, r0 + 4, r0 + 5, r0 + 6, role, su, svv);
        add_face(t, r0 + 6, r0 + 7, r0 + 0, r0 + 3, role, su, svv);
    }
}

/* Orienta cada cara con cross(B-A, D-A) hacia el interior (frente al centro de la malla) y la deja como VrHandBoneMesh con normales suaves. */
static void finish_mesh(const Tmp *t, VrHandBoneMesh *m) {
    memset(m, 0, sizeof *m);
    m->n_verts = (uint8_t)t->nv; m->n_faces = (uint8_t)t->nf;
    float c[3] = {0, 0, 0};
    for (int i = 0; i < t->nv; i++) {
        for (int k = 0; k < 3; k++) { m->v[i][k] = (int16_t)lroundf(t->v[i][k]); c[k] += (float)m->v[i][k]; }
    }
    for (int k = 0; k < 3; k++) c[k] /= (float)(t->nv > 0 ? t->nv : 1);
    for (int fi = 0; fi < t->nf; fi++) {
        uint8_t f[4], su[4], sv[4];
        memcpy(f, t->f[fi], 4); memcpy(su, t->su[fi], 4); memcpy(sv, t->sv[fi], 4);
        const int16_t *A = m->v[f[0]], *B = m->v[f[1]], *D = m->v[f[3]];
        float e1[3] = {(float)(B[0] - A[0]), (float)(B[1] - A[1]), (float)(B[2] - A[2])}, e2[3] = {(float)(D[0] - A[0]), (float)(D[1] - A[1]), (float)(D[2] - A[2])};
        float n[3]; v3_cross(n, e1, e2);
        float fc[3] = {0, 0, 0};
        for (int k = 0; k < 4; k++) for (int j = 0; j < 3; j++) fc[j] += 0.25f * (float)m->v[f[k]][j];
        float o[3] = {fc[0] - c[0], fc[1] - c[1], fc[2] - c[2]};
        if (v3_dot(n, o) > 0.0f) {   /* apunta hacia fuera: invertir el sentido (A,D,C,B) */
            uint8_t g[4] = {f[0], f[3], f[2], f[1]}, gu[4] = {su[0], su[3], su[2], su[1]}, gv[4] = {sv[0], sv[3], sv[2], sv[1]};
            memcpy(f, g, 4); memcpy(su, gu, 4); memcpy(sv, gv, 4);
        }
        memcpy(m->f[fi], f, 4); memcpy(m->su[fi], su, 4); memcpy(m->sv[fi], sv, 4); m->role[fi] = t->role[fi];
    }
    /* normales interiores por vértice: suma de los cross de las caras que lo usan (ponderada por el área) */
    float acc[VRHAND_MAX_V][3];
    memset(acc, 0, sizeof acc);
    for (int fi = 0; fi < m->n_faces; fi++) {
        const uint8_t *f = m->f[fi];
        const int16_t *A = m->v[f[0]], *B = m->v[f[1]], *C = m->v[f[2]], *D = m->v[f[3]];
        const int16_t *tri[2][3] = {{A, B, D}, {B, C, D}};
        for (int q = 0; q < 2; q++) {
            float e1[3] = {(float)(tri[q][1][0] - tri[q][0][0]), (float)(tri[q][1][1] - tri[q][0][1]), (float)(tri[q][1][2] - tri[q][0][2])};
            float e2[3] = {(float)(tri[q][2][0] - tri[q][0][0]), (float)(tri[q][2][1] - tri[q][0][1]), (float)(tri[q][2][2] - tri[q][0][2])};
            float n[3]; v3_cross(n, e1, e2);
            for (int k = 0; k < 4; k++) for (int j = 0; j < 3; j++) acc[f[k]][j] += n[j];
        }
    }
    for (int i = 0; i < m->n_verts; i++) {
        float l = v3_len(acc[i]);
        if (l < 1e-6f) { float o[3] = {c[0] - (float)m->v[i][0], c[1] - (float)m->v[i][1], c[2] - (float)m->v[i][2]}; l = v3_len(o); if (l < 1e-6f) { o[1] = 1; l = 1; } for (int j = 0; j < 3; j++) acc[i][j] = o[j]; }
        for (int j = 0; j < 3; j++) m->n[i][j] = (int16_t)lroundf(4096.0f * acc[i][j] / l);
    }
}

/* Falange: tubo hexagonal del pivote proximal (y = 0, radio r0) al distal (y = -len, radio r1) con cúpula distal (anillo a 40 grados + tapa) y, si prox_dome, cúpula
 * proximal (nudillo); si no, tapa plana proximal (queda dentro de la cúpula del hueso anterior). */
static void build_segment(VrHandBoneMesh *m, float len, float r0, float r1, int prox_dome) {
    Tmp t; memset(&t, 0, sizeof t);
    static const uint8_t tube_roles[6] = {VRHAND_ROLE_FINGER, VRHAND_ROLE_FINGER, VRHAND_ROLE_BACK, VRHAND_ROLE_FINGER, VRHAND_ROLE_FINGER, VRHAND_ROLE_PALM};
    int rp = -1;
    if (prox_dome) rp = add_ring(&t, 6, 0.643f * r0, 0, 0, 0.766f * r0, 0.766f * r0, 30.0f);
    int a0 = add_ring(&t, 6, 0.0f, 0, 0, r0, r0, 30.0f);
    int a1 = add_ring(&t, 6, -len, 0, 0, r1, r1, 30.0f);
    int a2 = add_ring(&t, 6, -len - 0.643f * r1, 0, 0, 0.766f * r1, 0.766f * r1, 30.0f);
    add_band(&t, 6, a0, a1, 0, tube_roles, 0.0f, 1.0f);
    add_band(&t, 6, a1, a2, VRHAND_ROLE_JOINT, NULL, 1.0f, 1.0f);
    add_cap(&t, 6, a2, VRHAND_ROLE_JOINT, 1.0f);
    if (prox_dome) { add_band(&t, 6, rp, a0, VRHAND_ROLE_JOINT, NULL, 0.0f, 0.0f); add_cap(&t, 6, rp, VRHAND_ROLE_JOINT, 0.0f); }
    else add_cap(&t, 6, a0, VRHAND_ROLE_JOINT, 0.0f);
    finish_mesh(&t, m);
}

static void build_palm(const Lay *L, VrHandBoneMesh *m) {
    Tmp t; memset(&t, 0, sizeof t);
    static const uint8_t roles[8] = {VRHAND_ROLE_PALM, VRHAND_ROLE_BACK, VRHAND_ROLE_BACK, VRHAND_ROLE_BACK, VRHAND_ROLE_BACK, VRHAND_ROLE_BACK, VRHAND_ROLE_PALM, VRHAND_ROLE_PALM};
    const float k = 1.0f / cosf(22.5f * DEG);
    int r[3];
    for (int i = 0; i < 3; i++) r[i] = add_ring(&t, 8, L->ring[i].y, L->ring[i].cx, L->ring[i].cz, L->ring[i].hx * k, L->ring[i].hz * k, 22.5f);
    add_band(&t, 8, r[0], r[1], 0, roles, 0.0f, 0.5f);
    add_band(&t, 8, r[1], r[2], 0, roles, 0.5f, 1.0f);
    add_cap(&t, 8, r[0], VRHAND_ROLE_JOINT, 0.0f);
    add_cap(&t, 8, r[2], VRHAND_ROLE_JOINT, 1.0f);
    finish_mesh(&t, m);
}

int vrhand_build(VrHand *h, const VrHandDims *dims, int side) {
    if (!h) return -1;
    memset(h, 0, sizeof *h);
    VrHandDims d;
    if (dims) d = *dims; else vrhand_dims_default(&d);
    if (!isfin(d.cuff_y) || !isfin(d.tip_y) || !isfin(d.cuff_c[0]) || !isfin(d.cuff_c[1]) || !isfin(d.cuff_h[0]) || !isfin(d.cuff_h[1])) vrhand_dims_default(&d);
    h->dims = d;
    vrhand_pose_cfg_default(&h->pose);
    Lay L; layout(&d, &L);
    float I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (int k = 0; k < VRHAND_BONES; k++) { h->bone[k].parent = -1; memcpy(h->bone[k].rest, I, sizeof I); }
    build_palm(&L, &h->mesh[0]);
    h->bone[0].parent = -1; h->bone[0].len = L.Lh; h->bone[0].radius = L.ring[1].hx;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 3; j++) {
            int k = 1 + 3 * i + j;
            VrHandBone *b = &h->bone[k];
            b->parent = j == 0 ? 0 : k - 1;
            if (j == 0) { b->pivot[0] = L.fx; b->pivot[1] = L.yk; b->pivot[2] = L.fz[i]; }
            else { b->pivot[0] = 0; b->pivot[1] = -h->bone[k - 1].len; b->pivot[2] = 0; }
            b->len = L.flen[i][j]; b->radius = L.fr1[i][j];
            build_segment(&h->mesh[k], b->len, L.fr0[i][j], L.fr1[i][j], j == 0);
        }
    }
    /* pulgar: el hueso 13 mira (en reposo) a lo largo de tdir; su eje local -Y es esa dirección */
    {
        float ey[3] = {-L.tdir[0], -L.tdir[1], -L.tdir[2]};
        float ex[3] = {1, 0, 0};
        float dp = v3_dot(ex, ey);
        for (int j = 0; j < 3; j++) ex[j] -= dp * ey[j];
        v3_norm(ex);
        float ez[3]; v3_cross(ez, ex, ey);
        VrHandBone *b = &h->bone[13];
        b->parent = 0; memcpy(b->pivot, L.tp, sizeof L.tp);
        for (int i = 0; i < 3; i++) { b->rest[3 * i] = ex[i]; b->rest[3 * i + 1] = ey[i]; b->rest[3 * i + 2] = ez[i]; }
        b->len = L.tlen[0]; b->radius = L.tr1[0];
        build_segment(&h->mesh[13], b->len, L.tr0[0], L.tr1[0], 1);
        for (int j = 1; j < 3; j++) {
            VrHandBone *c = &h->bone[13 + j];
            c->parent = 12 + j; c->pivot[0] = 0; c->pivot[1] = -h->bone[12 + j].len; c->pivot[2] = 0;
            c->len = L.tlen[j]; c->radius = L.tr1[j];
            build_segment(&h->mesh[13 + j], c->len, L.tr0[j], L.tr1[j], 0);
        }
    }
    if (side < 0) {   /* espejo exacto en X: vértices, normales y orden de las caras (A,D,C,B) */
        h->mirror = 1;
        for (int k = 0; k < VRHAND_BONES; k++) {
            VrHandBoneMesh *m = &h->mesh[k];
            for (int i = 0; i < m->n_verts; i++) { m->v[i][0] = (int16_t)-m->v[i][0]; m->n[i][0] = (int16_t)-m->n[i][0]; }
            for (int fi = 0; fi < m->n_faces; fi++) {
                uint8_t f[4], su[4], sv[4];
                memcpy(f, m->f[fi], 4); memcpy(su, m->su[fi], 4); memcpy(sv, m->sv[fi], 4);
                const int ord[4] = {0, 3, 2, 1};
                for (int q = 0; q < 4; q++) { m->f[fi][q] = f[ord[q]]; m->su[fi][q] = su[ord[q]]; m->sv[fi][q] = sv[ord[q]]; }
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------ pose */
static void pose_canonical(const VrHand *h, const float curl[5], const float thumb_t, float xf[VRHAND_BONES][12]) {
    const VrHandPoseCfg *pc = &h->pose;
    xf_ident(xf[0]);
    const float zax[3] = {0, 0, 1}, xax[3] = {1, 0, 0};
    for (int i = 0; i < 4; i++) {
        float c = clampf(curl[i], 0.0f, 1.0f);
        for (int j = 0; j < 3; j++) {
            int k = 1 + 3 * i + j;
            const VrHandBone *b = &h->bone[k];
            float ang = (pc->open_deg[j] + (pc->closed_deg[i][j] - pc->open_deg[j]) * c) * DEG;
            float R[9], loc[12];
            rot_axis(R, zax, ang);
            xf_from_rt(loc, R, b->pivot);
            xf_mul(xf[k], xf[b->parent], loc);
        }
    }
    {   /* pulgar */
        const VrHandBone *b13 = &h->bone[13];
        float tt = clampf(thumb_t, 0.0f, 1.0f);
        float a[4];
        for (int q = 0; q < 4; q++) a[q] = (pc->thumb_open[q] + (pc->thumb_closed[q] - pc->thumb_open[q]) * tt) * DEG;
        float d13[3] = {-b13->rest[1], -b13->rest[4], -b13->rest[7]};   /* -(eje local Y) en el marco de la mano = dirección del hueso */
        float ah[3]; v3_cross(ah, d13, xax); v3_norm(ah);               /* eje de flexión hacia la palma (+X) */
        float Rf[9], Ra[9], J[9], Rr[9], loc[12];
        rot_axis(Rf, ah, a[0]); rot_axis(Ra, xax, a[1]);
        m3_mul(J, Ra, Rf);
        m3_mul(Rr, J, b13->rest);
        xf_from_rt(loc, Rr, b13->pivot);
        xf_mul(xf[13], xf[0], loc);
        /* eje de flexión en el marco local del hueso 13 (rest^T * ah) */
        float al[3];
        for (int i = 0; i < 3; i++) al[i] = b13->rest[i] * ah[0] + b13->rest[3 + i] * ah[1] + b13->rest[6 + i] * ah[2];
        for (int j = 1; j < 3; j++) {
            const VrHandBone *b = &h->bone[13 + j];
            float R[9], l2[12];
            rot_axis(R, al, a[1 + j]);
            xf_from_rt(l2, R, b->pivot);
            xf_mul(xf[13 + j], xf[12 + j], l2);
        }
    }
}

static void mirror_xf(float *x) {
    /* Mx * X * Mx con Mx = diag(-1, 1, 1) */
    x[1] = -x[1]; x[2] = -x[2];     /* fila 0, columnas 1 y 2 */
    x[4] = -x[4]; x[8] = -x[8];     /* filas 1 y 2, columna 0 */
    x[3] = -x[3];                   /* traslación x */
}

void vrhand_pose_curls(const VrHand *h, const float curl[5], float xf[VRHAND_BONES][12]) {
    float c[5];
    for (int i = 0; i < 5; i++) c[i] = (curl && isfin(curl[i])) ? clampf(curl[i], 0.0f, 1.0f) : 0.0f;
    pose_canonical(h, c, c[4], xf);
    if (h->mirror) for (int k = 0; k < VRHAND_BONES; k++) mirror_xf(xf[k]);
}

void vrhand_pose(const VrHand *h, float grip, float xf[VRHAND_BONES][12]) {
    float g = isfin(grip) ? clampf(grip, 0.0f, 1.0f) : 0.0f;
    float st = clampf(h->pose.thumb_start, 0.0f, 0.95f);
    float u = clampf((g - st) / (1.0f - st), 0.0f, 1.0f);
    float t = u * u * (3.0f - 2.0f * u);
    float c[5] = {g, g, g, g, t};
    vrhand_pose_curls(h, c, xf);
}

int vrhand_tri_count(const VrHand *h) {
    int n = 0;
    for (int k = 0; k < VRHAND_BONES; k++) for (int f = 0; f < h->mesh[k].n_faces; f++) {
        const uint8_t *q = h->mesh[k].f[f];
        n += (q[2] == q[3]) ? 1 : 2;
    }
    return n;
}

void vrhand_fist_point(const VrHand *h, const float xf[VRHAND_BONES][12], float out[3]) {
    float ymin = 1e30f;
    for (int k = 1; k <= 12; k++) for (int i = 0; i < h->mesh[k].n_verts; i++) {
        float p[3] = {(float)h->mesh[k].v[i][0], (float)h->mesh[k].v[i][1], (float)h->mesh[k].v[i][2]}, q[3];
        xf_apply(xf[k], p, q);
        if (q[1] < ymin) ymin = q[1];
    }
    float s[3] = {0, 0, 0}; int n = 0;
    for (int k = 1; k <= 12; k++) for (int i = 0; i < h->mesh[k].n_verts; i++) {
        float p[3] = {(float)h->mesh[k].v[i][0], (float)h->mesh[k].v[i][1], (float)h->mesh[k].v[i][2]}, q[3];
        xf_apply(xf[k], p, q);
        if (q[1] <= ymin + 8.0f * (h->dims.cuff_y - h->dims.tip_y) / 147.0f) { s[0] += q[0]; s[1] += q[1]; s[2] += q[2]; n++; }
    }
    if (n < 1) { out[0] = out[2] = 0; out[1] = h->dims.tip_y; return; }
    out[0] = s[0] / (float)n; out[1] = s[1] / (float)n; out[2] = s[2] / (float)n;
}

void vrhand_palm_center(const VrHand *h, float out[3]) {
    Lay L; layout(&h->dims, &L);
    float cx, cz, hx, hz, y = -0.374f * L.Lh;
    palm_section(&L, y, &cx, &cz, &hx, &hz);
    out[0] = h->mirror ? -cx : cx; out[1] = y; out[2] = cz;
}

/* ------------------------------------------------------------------------------------------------ colisión (cápsulas) */
static float seg_seg_dist(const float *p1, const float *q1, const float *p2, const float *q2) {
    float d1[3] = {q1[0] - p1[0], q1[1] - p1[1], q1[2] - p1[2]}, d2[3] = {q2[0] - p2[0], q2[1] - p2[1], q2[2] - p2[2]}, r[3] = {p1[0] - p2[0], p1[1] - p2[1], p1[2] - p2[2]};
    float a = v3_dot(d1, d1), e = v3_dot(d2, d2), f = v3_dot(d2, r), s, t;
    if (a <= 1e-9f && e <= 1e-9f) { s = t = 0; }
    else if (a <= 1e-9f) { s = 0; t = clampf(f / e, 0, 1); }
    else {
        float c = v3_dot(d1, r);
        if (e <= 1e-9f) { t = 0; s = clampf(-c / a, 0, 1); }
        else {
            float b = v3_dot(d1, d2), den = a * e - b * b;
            s = den > 1e-9f ? clampf((b * f - c * e) / den, 0, 1) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0) { t = 0; s = clampf(-c / a, 0, 1); } else if (t > 1) { t = 1; s = clampf((b - c) / a, 0, 1); }
        }
    }
    float c1[3] = {p1[0] + d1[0] * s, p1[1] + d1[1] * s, p1[2] + d1[2] * s}, c2[3] = {p2[0] + d2[0] * t, p2[1] + d2[1] * t, p2[2] + d2[2] * t};
    float d[3] = {c1[0] - c2[0], c1[1] - c2[1], c1[2] - c2[2]};
    return v3_len(d);
}

/* Distancia con signo (aprox.) de q a la palma, en el marco CANÓNICO (derecha). */
static float palm_sd(const Lay *L, const float *q) {
    float cx, cz, hx, hz;
    float y = clampf(q[1], L->ring[2].y, L->ring[0].y);
    palm_section(L, y, &cx, &cz, &hx, &hz);
    float dx = fabsf(q[0] - cx) - hx, dz = fabsf(q[2] - cz) - hz, dy = q[1] < L->ring[2].y ? L->ring[2].y - q[1] : (q[1] > L->ring[0].y ? q[1] - L->ring[0].y : 0.0f);
    if (dx > 0 || dz > 0 || dy > 0) { float a = dx > 0 ? dx : 0, b = dz > 0 ? dz : 0; return sqrtf(a * a + b * b + dy * dy); }
    return dx > dz ? dx : dz;
}

void vrhand_collision(const VrHand *h, const float xf_in[VRHAND_BONES][12], VrHandCollision *out) {
    memset(out, 0, sizeof *out);
    Lay L; layout(&h->dims, &L);
    float xf[VRHAND_BONES][12];
    memcpy(xf, xf_in, sizeof xf);
    if (h->mirror) for (int k = 0; k < VRHAND_BONES; k++) mirror_xf(xf[k]);   /* de vuelta al marco canónico */
    float P0[VRHAND_BONES][3], P1[VRHAND_BONES][3], R[VRHAND_BONES];
    const float z0[3] = {0, 0, 0};
    for (int k = 1; k < VRHAND_BONES; k++) {
        float e[3] = {0, -h->bone[k].len, 0};
        xf_apply(xf[k], z0, P0[k]); xf_apply(xf[k], e, P1[k]);
        R[k] = 0.93f * h->bone[k].radius;
    }
    out->finger_finger = out->finger_palm = out->thumb_finger = out->thumb_palm = -1e9f;
    for (int a = 1; a <= 12; a++) for (int b = a + 1; b <= 12; b++) {
        if ((a - 1) / 3 == (b - 1) / 3) continue;
        float pen = R[a] + R[b] - seg_seg_dist(P0[a], P1[a], P0[b], P1[b]);
        if (pen > out->finger_finger) out->finger_finger = pen;
    }
    for (int a = 13; a <= 15; a++) for (int b = 1; b <= 12; b++) {
        float pen = R[a] + R[b] - seg_seg_dist(P0[a], P1[a], P0[b], P1[b]);
        if (pen > out->thumb_finger) out->thumb_finger = pen;
    }
    for (int k = 1; k <= 15; k++) {
        static const float ts[2] = {0.5f, 1.0f};
        for (int s = (k % 3 == 1 && k <= 12) ? 1 : 0; s < 2; s++) {   /* falange proximal: solo su extremo distal (la base es el nudillo) */
            float q[3] = {P0[k][0] + (P1[k][0] - P0[k][0]) * ts[s], P0[k][1] + (P1[k][1] - P0[k][1]) * ts[s], P0[k][2] + (P1[k][2] - P0[k][2]) * ts[s]};
            float pen = R[k] - palm_sd(&L, q);
            if (k <= 12) { if (pen > out->finger_palm) out->finger_palm = pen; }
            else if (k > 13 && pen > out->thumb_palm) out->thumb_palm = pen;
        }
    }
    {   /* yema del corazón (hueso 6) contra la superficie de la palma */
        float sd = palm_sd(&L, P1[6]);
        out->tip_palm_gap = sd - R[6];
        float xr = -1e9f;
        const int mids[2] = {2, 5};
        for (int i = 0; i < 2; i++) {
            float r = R[mids[i]];
            if (P0[mids[i]][0] + r > xr) xr = P0[mids[i]][0] + r;
            if (P1[mids[i]][0] + r > xr) xr = P1[mids[i]][0] + r;
        }
        out->thumb_over = (P1[15][0] - R[15]) - xr;
    }
}
