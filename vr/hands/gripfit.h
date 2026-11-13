/* gripfit: colisión mano-arma para ajustar y comprobar las poses de agarre por arma (vr_hand_pose.c: HP_W_*, vr_grip.c). Solo cabecera, sin dependencias salvo vr_hand_skin; lo usan
 * gripfit_gen.c (generador offline de la tabla) y test_gripfit.c (comprobación con STAGE.DIR). Unidades: m para el arma y el mundo, mm para la mano y las distancias.
 *   - Arma/obstáculo = lista de triángulos. Distancia con signo: la sin signo al triángulo más cercano; el signo, para el arma, por paridad de rayos (mayoría de 3 ejes) y, para la otra
 *     mano, por la normal de la cara más cercana (la malla de la mano está orientada hacia fuera).
 *   - Una "parte" de la mano es el hueso dominante de cada vértice: 0 palma/metacarpos/muñeca, 1 + 3 f + (0 proximal, 1 media, 2 distal) por dedo f (índice..meñique), 13 y 14 pulgar proximal y distal. */
#pragma once
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "../body/vr_hand_skin.h"

#define GF_NPART 15
typedef struct { float (*t)[3][3]; float (*bb)[6]; float (*fn)[3]; int n; float flip; } GfTris;   /* flip: +1 si las normales de cara (b-a)x(c-a) miran hacia fuera, -1 si hacia dentro */

static inline float gf_dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline void gf_sub(const float *a, const float *b, float *o) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static inline void gf_cross(const float *a, const float *b, float *o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }

/* src: 3 n puntos (m). flip: invierte la normal de las caras (se calcula sola con nrm si no es NULL: nrm = normal hacia fuera del primer vértice de cada cara, n puntos de 3 en 3 como src). */
static inline void gf_build(GfTris *T, const float (*src)[3], int n) {
    T->n = n; T->flip = 1.0f;
    T->t = (float (*)[3][3])malloc(sizeof(float[3][3]) * (size_t)(n ? n : 1));
    T->bb = (float (*)[6])malloc(sizeof(float[6]) * (size_t)(n ? n : 1));
    T->fn = (float (*)[3])malloc(sizeof(float[3]) * (size_t)(n ? n : 1));
    for (int i = 0; i < n; i++) {
        float e1[3], e2[3];
        for (int k = 0; k < 3; k++) memcpy(T->t[i][k], src[3 * i + k], 12);
        for (int a = 0; a < 3; a++) {
            float lo = T->t[i][0][a], hi = lo;
            for (int k = 1; k < 3; k++) { if (T->t[i][k][a] < lo) lo = T->t[i][k][a]; if (T->t[i][k][a] > hi) hi = T->t[i][k][a]; }
            T->bb[i][a] = lo; T->bb[i][3 + a] = hi;
        }
        gf_sub(T->t[i][1], T->t[i][0], e1); gf_sub(T->t[i][2], T->t[i][0], e2); gf_cross(e1, e2, T->fn[i]);
        float l = sqrtf(gf_dot(T->fn[i], T->fn[i])); if (l > 1e-12f) for (int a = 0; a < 3; a++) T->fn[i][a] /= l;
    }
}
static inline void gf_free(GfTris *T) { free(T->t); free(T->bb); free(T->fn); memset(T, 0, sizeof *T); }

/* Orienta flip de un conjunto de triángulos cerrados: la cara que contiene el vértice más extremo en cada eje debe mirar hacia fuera (voto de los 6 extremos). */
static inline void gf_orient_outward(GfTris *T) {
    int vote = 0;
    for (int ax = 0; ax < 3; ax++) for (int sg = -1; sg <= 1; sg += 2) {
        float best = -1e9f; int bi = 0;
        for (int i = 0; i < T->n; i++) for (int k = 0; k < 3; k++) if (sg * T->t[i][k][ax] > best) { best = sg * T->t[i][k][ax]; bi = i; }
        float c = T->fn[bi][ax] * (float)sg; if (fabsf(c) > 0.3f) vote += c > 0 ? 1 : -1;
    }
    T->flip = vote >= 0 ? 1.0f : -1.0f;
}
/* punto más cercano del triángulo (a, b, c) a p -> q */
static inline void gf_closest(const float *p, const float *a, const float *b, const float *c, float *q) {
    float ab[3], ac[3], ap[3]; gf_sub(b, a, ab); gf_sub(c, a, ac); gf_sub(p, a, ap);
    float d1 = gf_dot(ab, ap), d2 = gf_dot(ac, ap);
#define GF_Q(x, y, z) { q[0] = (x); q[1] = (y); q[2] = (z); return; }
    if (d1 <= 0 && d2 <= 0) GF_Q(a[0], a[1], a[2]);
    float bp[3]; gf_sub(p, b, bp); float d3 = gf_dot(ab, bp), d4 = gf_dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) GF_Q(b[0], b[1], b[2]);
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) { float v = d1 / (d1 - d3); GF_Q(a[0] + v * ab[0], a[1] + v * ab[1], a[2] + v * ab[2]); }
    float cp[3]; gf_sub(p, c, cp); float d5 = gf_dot(ab, cp), d6 = gf_dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) GF_Q(c[0], c[1], c[2]);
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) { float w = d2 / (d2 - d6); GF_Q(a[0] + w * ac[0], a[1] + w * ac[1], a[2] + w * ac[2]); }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) { float w = (d4 - d3) / ((d4 - d3) + (d5 - d6)); GF_Q(b[0] + w * (c[0] - b[0]), b[1] + w * (c[1] - b[1]), b[2] + w * (c[2] - b[2])); }
    float den = 1.0f / (va + vb + vc), v = vb * den, w = vc * den;
    GF_Q(a[0] + ab[0] * v + ac[0] * w, a[1] + ab[1] * v + ac[1] * w, a[2] + ab[2] * v + ac[2] * w);
#undef GF_Q
}

/* ¿el rayo desde p en el eje ax (+) cruza el triángulo i? (Möller sobre ejes alineados: proyección 2D) */
static inline int gf_ray_hit(const GfTris *T, int i, const float *p, int ax) {
    const int u = (ax + 1) % 3, v = (ax + 2) % 3;
    const float *bb = T->bb[i];
    if (p[u] < bb[u] || p[u] > bb[3 + u] || p[v] < bb[v] || p[v] > bb[3 + v] || bb[3 + ax] < p[ax]) return 0;
    const float (*t)[3] = T->t[i];
    float e0u = t[1][u] - t[0][u], e0v = t[1][v] - t[0][v], e1u = t[2][u] - t[0][u], e1v = t[2][v] - t[0][v];
    float det = e0u * e1v - e0v * e1u;
    if (fabsf(det) < 1e-14f) return 0;
    float pu = p[u] - t[0][u], pv = p[v] - t[0][v];
    float s = (pu * e1v - pv * e1u) / det, r = (e0u * pv - e0v * pu) / det;
    if (s < 0 || r < 0 || s + r > 1) return 0;
    float h = t[0][ax] + s * (t[1][ax] - t[0][ax]) + r * (t[2][ax] - t[0][ax]);
    return h > p[ax];
}
static inline int gf_inside_parity(const GfTris *T, const float *p) {
    int votes = 0;
    for (int ax = 0; ax < 3; ax++) { int c = 0; for (int i = 0; i < T->n; i++) c += gf_ray_hit(T, i, p, ax); votes += (c & 1); }
    return votes >= 2;
}
/* Distancia con signo (m; < 0 dentro) al conjunto; si la distancia sin signo supera cut devuelve +dist sin calcular el signo. normal_sign: signo por la cara más cercana en vez de la paridad. */
static inline float gf_sd(const GfTris *T, const float *p, float cut, int normal_sign) {
    float best = 1e9f; int bi = -1; float bq[3] = {0, 0, 0};
    for (int i = 0; i < T->n; i++) {
        const float *bb = T->bb[i];
        if (p[0] < bb[0] - cut || p[0] > bb[3] + cut || p[1] < bb[1] - cut || p[1] > bb[4] + cut || p[2] < bb[2] - cut || p[2] > bb[5] + cut) continue;
        float q[3], d[3]; gf_closest(p, T->t[i][0], T->t[i][1], T->t[i][2], q); gf_sub(p, q, d);
        float dd = gf_dot(d, d);
        if (dd < best * best) { best = sqrtf(dd); bi = i; memcpy(bq, q, 12); }
    }
    if (bi < 0 || best > cut) return best > 1e8f ? cut * 4.0f : best;
    if (normal_sign) {   /* en una arista o un vértice empatan varias caras: el signo lo da la que mira más de frente al punto (|cos| máximo); con una sola cara arbitraria el signo sale mal en las aristas vivas */
        const float tol = best + 1e-5f; float bc = -1.0f, sg = 1.0f;
        for (int i = 0; i < T->n; i++) {
            const float *bb = T->bb[i];
            if (p[0] < bb[0] - tol || p[0] > bb[3] + tol || p[1] < bb[1] - tol || p[1] > bb[4] + tol || p[2] < bb[2] - tol || p[2] > bb[5] + tol) continue;
            float q[3], d[3]; gf_closest(p, T->t[i][0], T->t[i][1], T->t[i][2], q); gf_sub(p, q, d);
            const float dd = sqrtf(gf_dot(d, d));
            if (dd > tol) continue;
            if (dd < 1e-9f) return 0.0f;
            const float c = gf_dot(d, T->fn[i]) * T->flip / dd;
            if (fabsf(c) > bc) { bc = fabsf(c); sg = c < 0 ? -1.0f : 1.0f; }
        }
        return sg * best;
    }
    return gf_inside_parity(T, p) ? -best : best;
}

/* parte de cada vértice de la malla de la mano (hueso de mayor peso) */
static inline void gf_parts(int part[VRHA_NV]) {
    for (int i = 0; i < VRHA_NV; i++) {
        int bj = vrha_joint[i][0]; float bw = vrha_weight[i][0];
        for (int k = 1; k < 4; k++) if (vrha_weight[i][k] > bw) { bw = vrha_weight[i][k]; bj = vrha_joint[i][k]; }
        int id = 0;
        if (bj >= VRHS_INDEX_MC) { int f = (bj - VRHS_INDEX_MC) / 5, k = (bj - VRHS_INDEX_MC) % 5; if (k >= 1) id = 1 + 3 * f + (k > 3 ? 2 : k - 1); }
        else if (bj == VRHS_THUMB_PROX) id = 13; else if (bj == VRHS_THUMB_DIST || bj == VRHS_THUMB_TIP) id = 14;
        part[i] = id;
    }
}
/* triángulos de la mano en el mundo a partir de los vértices w (m), orientados hacia fuera (side -1: el espejo invierte el orden) */
static inline int gf_hand_tris(const float (*w)[3], int side, float (*out)[3]) {
    static int orient = 0;   /* +1 si las caras de la malla de reposo (mano derecha) miran hacia fuera con el orden del asset, -1 si no */
    if (!orient) {
        static float pos[VRHA_NV][3], nrm[VRHA_NV][3]; float xf[VRHS_J][12]; vrhs_pose(NULL, 0.0f, 1, xf); vrhs_skin(xf, 1, pos, nrm);
        double acc = 0;
        for (int pc = 0; pc < VRHA_NP; pc++) for (int t = 0; t < vrha_piece[pc].nt; t++) {
            const uint8_t *ix = vrha_tri[vrha_piece[pc].t0 + t]; const int v0 = vrha_piece[pc].v0;
            float e1[3], e2[3], n[3]; gf_sub(pos[v0 + ix[1]], pos[v0 + ix[0]], e1); gf_sub(pos[v0 + ix[2]], pos[v0 + ix[0]], e2); gf_cross(e1, e2, n);
            acc += gf_dot(n, nrm[v0 + ix[0]]) > 0 ? 1 : -1;
        }
        orient = acc >= 0 ? 1 : -1;
    }
    int n = 0;
    for (int pc = 0; pc < VRHA_NP; pc++) {
        const VrHaPiece *P = &vrha_piece[pc];
        for (int t = 0; t < P->nt; t++) {
            const uint8_t *ix = vrha_tri[P->t0 + t];
            int o[3] = {0, 1, 2}; if ((side < 0) != (orient < 0)) { o[1] = 2; o[2] = 1; }
            for (int k = 0; k < 3; k++) memcpy(out[3 * n + k], w[P->v0 + ix[o[k]]], 12);
            n++;
        }
    }
    return n;
}
