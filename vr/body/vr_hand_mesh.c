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
    static const float cl[4][3] = {{76, 88, 62}, {76, 88, 62}, {76, 88, 62}, {76, 88, 60}};
    memcpy(p->closed_deg, cl, sizeof cl);
    static const float to[4] = {0, 0, 12, 8};
    static const float tc[4] = {50, -4, 3, 5};
    memcpy(p->thumb_open, to, sizeof to);
    memcpy(p->thumb_closed, tc, sizeof tc);
    p->thumb_start = 0.35f;
}

typedef struct { float y, cx, cz, hx, hz; } PalmRing;
typedef struct {
    float Lh, sc, yk;
    PalmRing rim[2];                    /* dentro de la manga y labio del puño del guante */
    PalmRing ring[3];                   /* muñeca, centro, nudillos (también para la colisión) */
    PalmRing thenar;                    /* solo malla: abultamiento de la base del pulgar */
    float fx, fy[4], fz[4], fr[4], rj[4][4], flen[4][3];   /* fy: pivote MCP de cada dedo (nudillos en arco); rj: radio de los anillos MCP, PIP, DIP y base de la yema */
    float tp[3], tlen[3], tj[4], tdir[3], tx[3], ty[3], tz[3];   /* pulgar: pivote, largos, radios de anillo, dirección y ejes locales del metacarpo (columnas de rest) */
} Lay;

/* Sección de la palma a la altura y (interpolación lineal entre los tres anillos muñeca-centro-nudillos, acotada). */
static void palm_section(const Lay *L, float y, float *cx, float *cz, float *hx, float *hz) {
    const PalmRing *a, *b;
    if (y >= L->ring[1].y) { a = &L->ring[0]; b = &L->ring[1]; } else { a = &L->ring[1]; b = &L->ring[2]; }
    float t = (a->y - y) / (a->y - b->y);
    t = clampf(t, 0.0f, 1.0f);
    *cx = a->cx + (b->cx - a->cx) * t; *cz = a->cz + (b->cz - a->cz) * t; *hx = a->hx + (b->hx - a->hx) * t; *hz = a->hz + (b->hz - a->hz) * t;
}

/* Proporciones de una mano adulta (largo 1,0 = muñeca a yema del corazón, ~19 cm; ancho de la palma 0,447 = ~8,5 cm). La manga (puño) sale de la manopla del motor. */
static void layout(const VrHandDims *dm, Lay *L) {
    VrHandDims d = *dm;
    float Lh = clampf(d.cuff_y - d.tip_y, 60.0f, 300.0f);
    L->Lh = Lh; L->sc = Lh / 147.0f;
    float sc = L->sc;
    float chx = clampf(d.cuff_h[0], 10.0f, 60.0f), chz = clampf(d.cuff_h[1], 10.0f, 70.0f);
    L->yk = d.cuff_y - 0.53f * Lh;
    float kx = clampf(d.cuff_c[0] - 13.5f * sc, -10.0f * sc, 0.0f);
    static const float dyk[4] = {0.008f, 0.0f, 0.014f, 0.034f};      /* nudillos en arco: el corazón el más adelantado */
    const float W = 0.447f * Lh, gap = 0.012f * Lh;                  /* ancho de la palma = 4 dedos + 3 huecos */
    L->rim[0] = (PalmRing){d.cuff_y + 6.0f * sc, d.cuff_c[0], d.cuff_c[1], 0.95f * chx, 0.95f * chz};
    L->rim[1] = (PalmRing){d.cuff_y - 1.0f * sc, d.cuff_c[0], d.cuff_c[1], 1.03f * chx, 1.03f * chz};
    L->ring[0] = (PalmRing){d.cuff_y - 8.0f * sc, d.cuff_c[0], 0.4f * d.cuff_c[1], fminf(0.60f * chx, 0.105f * Lh), 0.145f * Lh};
    L->ring[2] = (PalmRing){L->yk + dyk[3] * Lh + 1.5f * sc, kx, 0.0f, 0.078f * Lh, 0.5f * W};
    float y1 = d.cuff_y - 0.38f * Lh;
    float t = (L->ring[0].y - y1) / (L->ring[0].y - L->ring[2].y);
    L->ring[1] = (PalmRing){y1, L->ring[0].cx + (L->ring[2].cx - L->ring[0].cx) * t, L->ring[0].cz + (L->ring[2].cz - L->ring[0].cz) * t, 0.088f * Lh, 0.200f * Lh};
    float y2 = d.cuff_y - 0.21f * Lh;
    t = (L->ring[0].y - y2) / (L->ring[0].y - L->ring[2].y);
    L->thenar = (PalmRing){y2, L->ring[0].cx + (L->ring[2].cx - L->ring[0].cx) * t + 0.012f * Lh, L->ring[0].cz + (L->ring[2].cz - L->ring[0].cz) * t + 0.03f * Lh, 0.108f * Lh, 0.175f * Lh};
    /* dedos: llenan el ancho de los nudillos con huecos pequeños; largo total (MCP a yema) y proporción de las tres falanges */
    L->fx = L->ring[2].cx + 1.5f * sc;
    static const float rf[4] = {1.0f, 1.0f, 0.97f, 0.90f}, ext[4] = {0.415f, 0.47f, 0.435f, 0.376f}, seg[3] = {0.47f, 0.29f, 0.24f};
    static const float rjf[4] = {1.0f, 0.92f, 0.83f, 0.74f};
    const float a = (W - 3.0f * gap) / (2.0f * (rf[0] + rf[1] + rf[2] + rf[3]));
    float zc = L->ring[2].cz + 0.5f * W;
    for (int i = 0; i < 4; i++) {
        L->fr[i] = a * rf[i];
        L->fz[i] = zc - L->fr[i];
        zc -= 2.0f * L->fr[i] + gap;
        L->fy[i] = L->yk + dyk[i] * Lh;
        for (int j = 0; j < 4; j++) L->rj[i][j] = L->fr[i] * rjf[j];
        float tube = ext[i] * Lh - 0.643f * L->rj[i][3];
        for (int j = 0; j < 3; j++) L->flen[i][j] = tube * seg[j];
    }
    /* pulgar: nace del abultamiento de la base (metacarpo dentro de la palma, cerca de la muñeca), a unos 45 grados hacia fuera */
    float yc = d.cuff_y - 0.19f * Lh;
    L->tp[0] = L->thenar.cx + 0.35f * L->thenar.hx; L->tp[1] = yc; L->tp[2] = L->thenar.cz + 0.55f * L->thenar.hz;
    float tlen[3] = {0.19f, 0.16f, 0.125f};
    static const float tjf[4] = {1.30f, 1.17f, 1.04f, 0.92f};
    for (int j = 0; j < 4; j++) L->tj[j] = L->fr[1] * tjf[j];
    for (int j = 0; j < 3; j++) L->tlen[j] = tlen[j] * Lh - (j == 2 ? 0.643f * L->tj[3] : 0.0f);
    L->tdir[0] = 0.12f; L->tdir[1] = -0.70f; L->tdir[2] = 0.70f; v3_norm(L->tdir);
    /* ejes locales del metacarpo: ty = -tdir (su -Y local es la dirección del hueso); tx (la cara de la yema, +X local) mira entre la palma y el índice (+X y -Z), ortogonalizado; tz = tx x ty */
    for (int i = 0; i < 3; i++) L->ty[i] = -L->tdir[i];
    L->tx[0] = 0.55f; L->tx[1] = 0.0f; L->tx[2] = -0.835f;
    float dp = v3_dot(L->tx, L->ty);
    for (int i = 0; i < 3; i++) L->tx[i] -= dp * L->ty[i];
    v3_norm(L->tx);
    v3_cross(L->tz, L->tx, L->ty);
}

/* ------------------------------------------------------------------------------------------------ constructor de mallas */
typedef struct {
    int nv, nf;
    float v[VRHAND_MAX_V][3];
    uint8_t f[VRHAND_MAX_F][4], role[VRHAND_MAX_F], su[VRHAND_MAX_F][4], sv[VRHAND_MAX_F][4], weld[VRHAND_MAX_V];
    float ref[VRHAND_MAX_F][3];         /* punto interior cercano a la cara (eje del tubo): fija qué lado es el exterior */
} Tmp;

static void tmp_init(Tmp *t) { memset(t, 0, sizeof *t); memset(t->weld, VRHAND_NO_WELD, sizeof t->weld); }

/* Anillo de n vértices en el plano (ex, ez) alrededor de c (ex, ez unitarios), radio r. */
static int add_ring3(Tmp *t, int n, const float *c, const float *ex, const float *ez, float r, float phase_deg) {
    int base = t->nv;
    for (int k = 0; k < n && t->nv < VRHAND_MAX_V; k++) {
        float ph = (phase_deg + 360.0f * (float)k / (float)n) * DEG, cs = r * cosf(ph), sn = r * sinf(ph);
        for (int a = 0; a < 3; a++) t->v[t->nv][a] = c[a] + cs * ex[a] + sn * ez[a];
        t->nv++;
    }
    return base;
}
/* Anillo elíptico en un plano y = const (semiejes a en X, b en Z). */
static int add_ring(Tmp *t, int n, float y, float cx, float cz, float a, float b, float phase_deg) {
    int base = t->nv;
    for (int k = 0; k < n && t->nv < VRHAND_MAX_V; k++) {
        float ph = (phase_deg + 360.0f * (float)k / (float)n) * DEG;
        t->v[t->nv][0] = cx + a * cosf(ph); t->v[t->nv][1] = y; t->v[t->nv][2] = cz + b * sinf(ph);
        t->nv++;
    }
    return base;
}
static void add_face(Tmp *t, int a, int b, int c, int d, int role, const uint8_t su[4], const uint8_t sv[4], const float *ref) {
    if (t->nf >= VRHAND_MAX_F) return;
    t->f[t->nf][0] = (uint8_t)a; t->f[t->nf][1] = (uint8_t)b; t->f[t->nf][2] = (uint8_t)c; t->f[t->nf][3] = (uint8_t)d;
    t->role[t->nf] = (uint8_t)role;
    memcpy(t->su[t->nf], su, 4); memcpy(t->sv[t->nf], sv, 4);
    memcpy(t->ref[t->nf], ref, 3 * sizeof(float));
    t->nf++;
}
/* Banda entre dos anillos de n vértices (a hacia b; ca, cb = centros de los anillos). roles[k]: rol de la cara k (NULL = role); las caras de palma/dorso cubren toda la región, si no el parámetro avanza alrededor. */
static void add_band(Tmp *t, int n, int ra, int rb, int role, const uint8_t *roles, float sv0, float sv1, const float *ca, const float *cb) {
    float ref[3] = {0.5f * (ca[0] + cb[0]), 0.5f * (ca[1] + cb[1]), 0.5f * (ca[2] + cb[2])};
    for (int k = 0; k < n; k++) {
        int k1 = (k + 1) % n;
        int r = roles ? roles[k] : role;
        uint8_t su[4], sv[4];
        uint8_t s0 = (uint8_t)(255.0f * (float)k / (float)n), s1 = (uint8_t)(255.0f * (float)(k + 1) / (float)n);
        uint8_t v0 = (uint8_t)(255.0f * sv0), v1 = (uint8_t)(255.0f * sv1);
        if (r == VRHAND_ROLE_PALM || r == VRHAND_ROLE_BACK) { su[0] = 0; su[1] = 255; su[2] = 255; su[3] = 0; }
        else { su[0] = s0; su[1] = s1; su[2] = s1; su[3] = s0; }
        sv[0] = v0; sv[1] = v0; sv[2] = v1; sv[3] = v1;
        add_face(t, ra + k, ra + k1, rb + k1, rb + k, r, su, sv, ref);
    }
}
/* Tapa plana de un anillo de n vértices (6: 2 cuadriláteros; 8: 3). other = centro del anillo del otro extremo (queda del lado interior). */
static void add_cap(Tmp *t, int n, int r0, int role, float sv, const float *other) {
    uint8_t su[4] = {0, 255, 255, 0}, svv[4];
    uint8_t v = (uint8_t)(255.0f * sv);
    svv[0] = svv[1] = svv[2] = svv[3] = v;
    if (n == 6) {
        add_face(t, r0 + 0, r0 + 1, r0 + 2, r0 + 3, role, su, svv, other);
        add_face(t, r0 + 3, r0 + 4, r0 + 5, r0 + 0, role, su, svv, other);
    } else {   /* 8 */
        add_face(t, r0 + 0, r0 + 1, r0 + 2, r0 + 3, role, su, svv, other);
        add_face(t, r0 + 3, r0 + 4, r0 + 5, r0 + 6, role, su, svv, other);
        add_face(t, r0 + 6, r0 + 7, r0 + 0, r0 + 3, role, su, svv, other);
    }
}

/* Deja la malla en int16: cada cara con cross(B-A, D-A) hacia el interior (frente a su punto de referencia) y normales suaves por vértice. */
static void finish_mesh(const Tmp *t, VrHandBoneMesh *m) {
    memset(m, 0, sizeof *m);
    memset(m->weld, VRHAND_NO_WELD, sizeof m->weld);
    m->n_verts = (uint8_t)t->nv; m->n_faces = (uint8_t)t->nf;
    float c[3] = {0, 0, 0};
    for (int i = 0; i < t->nv; i++) {
        for (int k = 0; k < 3; k++) { m->v[i][k] = (int16_t)lroundf(t->v[i][k]); c[k] += (float)m->v[i][k]; }
        m->weld[i] = t->weld[i];
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
        float o[3] = {fc[0] - t->ref[fi][0], fc[1] - t->ref[fi][1], fc[2] - t->ref[fi][2]};
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

/* Falange: tubo hexagonal de su articulación proximal (y = 0, radio r0) a la distal (y = -len, radio r1), con tapas planas en ambos extremos (la malla es cerrada).
 * El anillo proximal (vértices 0..5) se suelda al anillo de vértices weld_base..weld_base+5 del hueso padre (anillo distal de la falange anterior = 6, o collar de la palma).
 * Si tip: la punta lleva una cúpula (anillo a 40 grados + tapa) con la uña (cara dorsal, rol JOINT) y la yema (cara de la palma, rol PALM). */
static void build_phalanx(VrHandBoneMesh *m, float len, float r0, float r1, int tip, int weld_base) {
    Tmp t; tmp_init(&t);
    static const uint8_t tube_roles[6] = {VRHAND_ROLE_FINGER, VRHAND_ROLE_FINGER, VRHAND_ROLE_BACK, VRHAND_ROLE_FINGER, VRHAND_ROLE_FINGER, VRHAND_ROLE_PALM};
    static const uint8_t tip_roles[6] = {VRHAND_ROLE_FINGER, VRHAND_ROLE_FINGER, VRHAND_ROLE_JOINT, VRHAND_ROLE_FINGER, VRHAND_ROLE_FINGER, VRHAND_ROLE_PALM};
    const float c0[3] = {0, 0, 0}, c1[3] = {0, -len, 0};
    int a0 = add_ring(&t, 6, 0.0f, 0, 0, r0, r0, 30.0f);
    int a1 = add_ring(&t, 6, -len, 0, 0, r1, r1, 30.0f);
    if (weld_base >= 0) for (int k = 0; k < 6; k++) t.weld[a0 + k] = (uint8_t)(weld_base + k);
    add_band(&t, 6, a0, a1, 0, tube_roles, 0.0f, 1.0f, c0, c1);
    add_cap(&t, 6, a0, VRHAND_ROLE_JOINT, 0.0f, c1);
    if (tip) {
        const float c2[3] = {0, -len - 0.643f * r1, 0};
        int a2 = add_ring(&t, 6, c2[1], 0, 0, 0.766f * r1, 0.766f * r1, 30.0f);
        add_band(&t, 6, a1, a2, 0, tip_roles, 1.0f, 1.0f, c1, c2);
        add_cap(&t, 6, a2, VRHAND_ROLE_JOINT, 1.0f, c1);
    } else add_cap(&t, 6, a1, VRHAND_ROLE_JOINT, 1.0f, c0);
    finish_mesh(&t, m);
}

/* Palma: bandas octogonales (dentro de la manga, labio del guante, muñeca, centro, nudillos) y, para cada dedo y el pulgar, un collar hexagonal corto cuyo anillo
 * frontal es el objetivo de la soldadura del hueso hijo (collar_front[i] = primer vértice de ese anillo; 0..3 dedos, 4 pulgar). */
static void build_palm(const Lay *L, VrHandBoneMesh *m, int collar_front[5]) {
    Tmp t; tmp_init(&t);
    static const uint8_t roles[8] = {VRHAND_ROLE_PALM, VRHAND_ROLE_BACK, VRHAND_ROLE_BACK, VRHAND_ROLE_BACK, VRHAND_ROLE_BACK, VRHAND_ROLE_BACK, VRHAND_ROLE_PALM, VRHAND_ROLE_PALM};
    static const uint8_t tube_roles[6] = {VRHAND_ROLE_FINGER, VRHAND_ROLE_FINGER, VRHAND_ROLE_BACK, VRHAND_ROLE_FINGER, VRHAND_ROLE_FINGER, VRHAND_ROLE_PALM};
    const float k = 1.0f / cosf(22.5f * DEG);
    const PalmRing *pr[6] = {&L->rim[0], &L->rim[1], &L->ring[0], &L->thenar, &L->ring[1], &L->ring[2]};
    int r[6];
    float c[6][3];
    for (int i = 0; i < 6; i++) {
        r[i] = add_ring(&t, 8, pr[i]->y, pr[i]->cx, pr[i]->cz, pr[i]->hx * k, pr[i]->hz * k, 22.5f);
        c[i][0] = pr[i]->cx; c[i][1] = pr[i]->y; c[i][2] = pr[i]->cz;
    }
    static const float svs[6] = {0.0f, 0.1f, 0.2f, 0.4f, 0.65f, 1.0f};
    for (int i = 0; i < 5; i++) add_band(&t, 8, r[i], r[i + 1], 0, roles, svs[i], svs[i + 1], c[i], c[i + 1]);
    add_cap(&t, 8, r[0], VRHAND_ROLE_JOINT, 0.0f, c[1]);
    add_cap(&t, 8, r[5], VRHAND_ROLE_JOINT, 1.0f, c[4]);
    const float ex[3] = {1, 0, 0}, ez[3] = {0, 0, 1};
    for (int i = 0; i < 4; i++) {   /* collares de los dedos: delante en el pivote del MCP, detrás dentro de la palma */
        const float cf[3] = {L->fx, L->fy[i], L->fz[i]}, cb[3] = {L->fx, L->fy[i] + 1.6f * L->fr[i], L->fz[i]};
        int rb = add_ring3(&t, 6, cb, ex, ez, L->rj[i][0], 30.0f), rf = add_ring3(&t, 6, cf, ex, ez, L->rj[i][0], 30.0f);
        add_band(&t, 6, rb, rf, 0, tube_roles, 0.0f, 1.0f, cb, cf);
        add_cap(&t, 6, rb, VRHAND_ROLE_JOINT, 0.0f, cf);
        add_cap(&t, 6, rf, VRHAND_ROLE_JOINT, 1.0f, cb);
        collar_front[i] = rf;
    }
    {   /* collar del pulgar: en los ejes locales del metacarpo (el anillo es perpendicular a su dirección) */
        float cb[3], cf[3];
        for (int a = 0; a < 3; a++) { cf[a] = L->tp[a]; cb[a] = L->tp[a] + 0.8f * L->tj[0] * L->ty[a]; }
        int rb = add_ring3(&t, 6, cb, L->tx, L->tz, L->tj[0], 30.0f), rf = add_ring3(&t, 6, cf, L->tx, L->tz, L->tj[0], 30.0f);
        add_band(&t, 6, rb, rf, 0, tube_roles, 0.0f, 1.0f, cb, cf);
        add_cap(&t, 6, rb, VRHAND_ROLE_JOINT, 0.0f, cf);
        add_cap(&t, 6, rf, VRHAND_ROLE_JOINT, 1.0f, cb);
        collar_front[4] = rf;
    }
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
    int cf[5];
    build_palm(&L, &h->mesh[0], cf);
    h->bone[0].parent = -1; h->bone[0].len = L.Lh; h->bone[0].radius = L.ring[1].hx;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 3; j++) {
            int k = 1 + 3 * i + j;
            VrHandBone *b = &h->bone[k];
            b->parent = j == 0 ? 0 : k - 1;
            if (j == 0) { b->pivot[0] = L.fx; b->pivot[1] = L.fy[i]; b->pivot[2] = L.fz[i]; }
            else { b->pivot[0] = 0; b->pivot[1] = -h->bone[k - 1].len; b->pivot[2] = 0; }
            b->len = L.flen[i][j]; b->radius = L.rj[i][j + 1];
            build_phalanx(&h->mesh[k], b->len, L.rj[i][j], L.rj[i][j + 1], j == 2, j == 0 ? cf[i] : 6);
        }
    }
    /* pulgar: el hueso 13 mira (en reposo) a lo largo de tdir; su eje local -Y es esa dirección */
    {
        VrHandBone *b = &h->bone[13];
        b->parent = 0; memcpy(b->pivot, L.tp, sizeof L.tp);
        for (int i = 0; i < 3; i++) { b->rest[3 * i] = L.tx[i]; b->rest[3 * i + 1] = L.ty[i]; b->rest[3 * i + 2] = L.tz[i]; }
        b->len = L.tlen[0]; b->radius = L.tj[1];
        build_phalanx(&h->mesh[13], b->len, L.tj[0], L.tj[1], 0, cf[4]);
        for (int j = 1; j < 3; j++) {
            VrHandBone *c = &h->bone[13 + j];
            c->parent = 12 + j; c->pivot[0] = 0; c->pivot[1] = -h->bone[12 + j].len; c->pivot[2] = 0;
            c->len = L.tlen[j]; c->radius = L.tj[j + 1];
            build_phalanx(&h->mesh[13 + j], c->len, L.tj[j], L.tj[j + 1], j == 2, 6);
        }
    }
    if (side < 0) {   /* espejo exacto en X: vértices, normales y orden de las caras (A,D,C,B); las soldaduras (índices) no cambian */
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
static void pose_canonical(const VrHand *h, const float curl[5], const float thumb_t, float spread, float xf[VRHAND_BONES][12]) {
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
        a[1] -= spread * 25.0f * DEG;                                   /* aducción negativa = abducción: el pulgar se abre hacia fuera */
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

void vrhand_pose_curls_ex(const VrHand *h, const float curl[5], float thumb_spread, float xf[VRHAND_BONES][12]) {
    float c[5];
    for (int i = 0; i < 5; i++) c[i] = (curl && isfin(curl[i])) ? clampf(curl[i], 0.0f, 1.0f) : 0.0f;
    pose_canonical(h, c, c[4], isfin(thumb_spread) ? clampf(thumb_spread, -1.0f, 1.0f) : 0.0f, xf);
    if (h->mirror) for (int k = 0; k < VRHAND_BONES; k++) mirror_xf(xf[k]);
}
void vrhand_pose_curls(const VrHand *h, const float curl[5], float xf[VRHAND_BONES][12]) { vrhand_pose_curls_ex(h, curl, 0.0f, xf); }

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

void vrhand_skin(const VrHand *h, const float xf[VRHAND_BONES][12], float out[VRHAND_BONES][VRHAND_MAX_V][3]) {
    for (int k = 0; k < VRHAND_BONES; k++) {
        const VrHandBoneMesh *m = &h->mesh[k];
        for (int i = 0; i < m->n_verts; i++) {
            int par = h->bone[k].parent;
            if (m->weld[i] != VRHAND_NO_WELD && par >= 0 && par < k && m->weld[i] < h->mesh[par].n_verts) {
                memcpy(out[k][i], out[par][m->weld[i]], 3 * sizeof(float));
            } else {
                float p[3] = {(float)m->v[i][0], (float)m->v[i][1], (float)m->v[i][2]};
                xf_apply(xf[k], p, out[k][i]);
            }
        }
    }
}

float vrhand_unit_m(const VrHand *h) { return VRHAND_REF_LEN_M / clampf(h->dims.cuff_y - h->dims.tip_y, 60.0f, 300.0f); }

void vrhand_palm_center(const VrHand *h, float out[3]) {
    Lay L; layout(&h->dims, &L);
    float cx, cz, hx, hz, y = h->dims.cuff_y - 0.50f * L.Lh;
    palm_section(&L, y, &cx, &cz, &hx, &hz);
    float x = cx + hx + 0.09f * L.Lh;     /* superficie de la palma + radio de un mango de ~34 mm: el centro del puño cerrado */
    out[0] = h->mirror ? -x : x; out[1] = y; out[2] = L.ring[2].cz;   /* z: centro de los cuatro dedos */
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
