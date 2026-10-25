/* vr_fist: ver vr_fist.h. Todo el análisis sale de la propia malla recibida (nada del juego en el código). */
#include "vr_fist.h"
#include <math.h>
#include <string.h>

#define DEG (3.14159265358979f / 180.0f)

void vrfist_params_default(VrFistParams *p) {
    p->head_deg[0] = 100.0f; p->head_deg[1] = 150.0f; p->head_deg[2] = 198.0f;
    p->joint2 = 0.45f; p->joint3 = 0.78f; p->soft = 0.12f;
    p->inner_k = 0.45f; p->outer_k = 0.9f;
    p->thumb_tilt_deg = 45.0f; p->thumb_joint = 0.5f; p->thumb_soft = 0.12f; p->thumb_base_deg = 35.0f; p->thumb_tip_deg = 0.0f;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float sstep(float x) { x = clampf(x, 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); }
static int16_t to_s16(float v) { return (int16_t)(v > 32767.0f ? 32767 : (v < -32768.0f ? -32768 : lrintf(v))); }

static int uf_find(int *p, int i) { while (p[i] != i) { p[i] = p[p[i]]; i = p[i]; } return i; }

#define STEP 0.5f
#define NSTEP 800

/* Curva de los dedos (dirección en función de t y posición integrada) para el agarre g. */
typedef struct { float a0, h1, h2, h3, w, t2, t3; int ns; } Curve;
static float curve_angle(const Curve *k, float g, float tt) {
    return k->a0 + g * ((k->h1 - k->a0) * sstep(tt / (2.0f * k->w)) + (k->h2 - k->h1) * sstep((tt - k->t2 + k->w) / (2.0f * k->w)) + (k->h3 - k->h2) * sstep((tt - k->t3 + k->w) / (2.0f * k->w)));
}
static void curve_build(const Curve *k, float g, float *Cx, float *Cy, float *Ah) {
    Cx[0] = 0; Cy[0] = 0;
    for (int i = 0; i <= k->ns; i++) {
        float tt = (float)i * STEP;
        Ah[i] = curve_angle(k, g, tt);
        if (i < k->ns) {
            float am = curve_angle(k, g, tt + 0.5f * STEP);   /* regla del punto medio con la dirección del tramo */
            Cx[i + 1] = Cx[i] + STEP * sinf(am); Cy[i + 1] = Cy[i] - STEP * cosf(am);
        }
    }
}
static void rot_axis(const float k[3], float ang, const float v[3], float o[3]) {   /* Rodrigues: v girado `ang` alrededor del eje unitario k */
    float c = cosf(ang), s = sinf(ang), d = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];
    float cr[3] = {k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2], k[0] * v[1] - k[1] * v[0]};
    for (int i = 0; i < 3; i++) o[i] = v[i] * c + cr[i] * s + k[i] * d * (1.0f - c);
}

int vrfist_deform(const VrFistMesh *m, float side_sign, float grip, const VrFistParams *prm, int16_t *out_v, int16_t *out_n) {
    if (!m || !prm || !out_v || !out_n || !m->v || !m->n || !m->vidx || !m->nidx) return -1;
    int nv = m->nv, nn = m->nn, nf = m->nf;
    if (nv < 8 || nv > VRFIST_MAX_V || nn < 1 || nn > VRFIST_MAX_V || nf < 4 || nf > VRFIST_MAX_F) return -1;
    if (!isfinite(grip) || !isfinite(side_sign)) return -1;
    float g = clampf(grip, 0.0f, 1.0f);
    float sg = side_sign < 0 ? -1.0f : 1.0f;
    memcpy(out_v, m->v, (size_t)nv * 4 * sizeof(int16_t));
    memcpy(out_n, m->n, (size_t)nn * 4 * sizeof(int16_t));

    /* conectividad y propietario de cada normal */
    int uf[VRFIST_MAX_V], owner[VRFIST_MAX_V];
    for (int i = 0; i < nv; i++) uf[i] = i;
    for (int i = 0; i < nn; i++) owner[i] = -1;
    for (int f = 0; f < nf; f++) {
        for (int k = 0; k < 4; k++) {
            int a = m->vidx[4 * f + k] & 0x7F, b = m->nidx[4 * f + k] & 0x7F;
            if (a >= nv || b >= nn) return -2;
            if (owner[b] < 0) owner[b] = a;
            int c = m->vidx[4 * f + ((k + 1) & 3)] & 0x7F;
            if (c >= nv) return -2;
            int ra = uf_find(uf, a), rc = uf_find(uf, c);
            if (ra != rc) uf[rc] = ra;
        }
    }
    if (g <= 1e-4f) return 0;                       /* mano abierta: la malla original */

    float X[VRFIST_MAX_V][3];
    for (int i = 0; i < nv; i++) { X[i][0] = sg * (float)m->v[4 * i]; X[i][1] = (float)m->v[4 * i + 1]; X[i][2] = (float)m->v[4 * i + 2]; }
    int cnt[VRFIST_MAX_V]; memset(cnt, 0, sizeof cnt);
    for (int i = 0; i < nv; i++) cnt[uf_find(uf, i)]++;
    int mainr = -1;
    for (int i = 0; i < nv; i++) if (uf_find(uf, i) == i && (mainr < 0 || cnt[i] > cnt[mainr])) mainr = i;
    if (mainr < 0 || cnt[mainr] < 8) return -3;
    int is_main[VRFIST_MAX_V], is_thumb[VRFIST_MAX_V];
    for (int i = 0; i < nv; i++) { int r = uf_find(uf, i); is_main[i] = r == mainr; is_thumb[i] = !is_main[i] && cnt[r] >= 4; }

    /* nudillos: el último anillo de vértices antes de los dedos (la mano principal; el eje de los dedos es -Y) */
    float smax = -1e9f, smin = 1e9f;
    for (int i = 0; i < nv; i++) if (is_main[i]) { float s = -X[i][1]; if (s > smax) smax = s; if (s < smin) smin = s; }
    if (!(smax > 40.0f) || !(smax - smin > 40.0f)) return -3;
    float s0 = -1e9f, cand = 0.62f * smax;
    for (int i = 0; i < nv; i++) if (is_main[i]) { float s = -X[i][1]; if (s <= cand && s > s0) s0 = s; }
    if (s0 < 0.35f * smax) s0 = 0.55f * smax;
    int nfing = 0;
    for (int i = 0; i < nv; i++) if (is_main[i] && -X[i][1] > s0 + 4.0f) nfing++;
    if (nfing < 4) return -3;

    /* eje central de los dedos: x = xm + pendiente (s - sm) por mínimos cuadrados con los vértices desde los nudillos */
    float sm = 0, xm = 0; int np = 0;
    for (int i = 0; i < nv; i++) if (is_main[i] && -X[i][1] >= s0 - 1.5f) { sm += -X[i][1]; xm += X[i][0]; np++; }
    if (np < 6) return -3;
    sm /= (float)np; xm /= (float)np;
    float sxx = 0, sxy = 0;
    for (int i = 0; i < nv; i++) if (is_main[i] && -X[i][1] >= s0 - 1.5f) { float ds = -X[i][1] - sm; sxx += ds * ds; sxy += ds * (X[i][0] - xm); }
    float slope = sxx > 1e-3f ? sxy / sxx : 0.0f;
    float a0 = atanf(slope);
    a0 = clampf(a0, -30.0f * DEG, 70.0f * DEG);
    float x0 = xm + tanf(a0) * (s0 - sm), y0 = -s0;
    float ux = sinf(a0), uy = -cosf(a0), nx_ = cosf(a0), ny_ = sinf(a0);

    float t[VRFIST_MAX_V], d[VRFIST_MAX_V], tmax = 0;
    for (int i = 0; i < nv; i++) {
        t[i] = 0; d[i] = 0;
        if (!is_main[i]) continue;
        float rx = X[i][0] - x0, ry = X[i][1] - y0;
        t[i] = rx * ux + ry * uy; d[i] = rx * nx_ + ry * ny_;
        if (t[i] > tmax) tmax = t[i];
    }
    if (!(tmax > 15.0f) || tmax > STEP * (NSTEP - 2)) return -3;
    float L = tmax;
    float w = prm->soft * L; if (w < 1.0f) w = 1.0f;
    float t2 = prm->joint2 * L, t3 = prm->joint3 * L;
    float h1 = prm->head_deg[0] * DEG, h2 = prm->head_deg[1] * DEG, h3 = prm->head_deg[2] * DEG;
    if (h1 < a0) h1 = a0;
    if (h2 < h1) h2 = h1;
    if (h3 < h2) h3 = h2;
    Curve cv = {a0, h1, h2, h3, w, t2, t3, 0};
    cv.ns = (int)(tmax / STEP) + 2;
    if (cv.ns > NSTEP) cv.ns = NSTEP;
    int ns = cv.ns;
    float Cx[NSTEP + 2], Cy[NSTEP + 2], Ah[NSTEP + 2];
    curve_build(&cv, g, Cx, Cy, Ah);
    float ink = 1.0f + g * (prm->inner_k - 1.0f), outk = 1.0f + g * (prm->outer_k - 1.0f);

    float Y[VRFIST_MAX_V][3]; float dA[VRFIST_MAX_V]; /* dA: giro del vértice (normales) */
    for (int i = 0; i < nv; i++) { Y[i][0] = X[i][0]; Y[i][1] = X[i][1]; Y[i][2] = X[i][2]; dA[i] = 0; }
    for (int i = 0; i < nv; i++) {
        if (!is_main[i] || t[i] <= 0.0f || -X[i][1] <= s0 + 1.5f) continue;     /* el anillo de los nudillos y todo lo de atrás (palma, muñeca) no se mueve */
        float pos = t[i] / STEP; int k = (int)pos; if (k > ns - 1) k = ns - 1;
        float fr = pos - (float)k; if (fr > 1) fr = 1;
        float cx = Cx[k] + (Cx[k + 1] - Cx[k]) * fr, cy = Cy[k] + (Cy[k + 1] - Cy[k]) * fr;
        float a = Ah[k] + (Ah[k + 1] - Ah[k]) * fr;
        float dd = d[i] * (d[i] > 0 ? ink : outk);
        Y[i][0] = x0 + cx + dd * cosf(a);
        Y[i][1] = y0 + cy + dd * sinf(a);
        dA[i] = a - a0;
    }

    /* pulgar: flexión propia y limitada, independiente de la de los dedos. La base gira `thumb_base_deg * grip` (35 grados) alrededor de un eje inclinado `thumb_tilt_deg` (45) respecto al eje de flexión de los
     * dedos (Z) hacia el lado de los dedos, de modo que el pulgar baja hacia la palma y se acerca a los dedos sin atravesarlos ni doblarse de forma rara; la punta puede flexionar `thumb_tip_deg * grip`
     * (0 por defecto) en el mismo plano. El movimiento es continuo y proporcional al agarre. */
    float tcz = 0, mcz = 0; int nt = 0, nm = 0;
    float tsmin = 1e9f, tsmax = -1e9f;
    for (int i = 0; i < nv; i++) {
        if (is_thumb[i]) { tcz += X[i][2]; nt++; float s = -X[i][1]; if (s < tsmin) tsmin = s; if (s > tsmax) tsmax = s; }
        else if (is_main[i]) { mcz += X[i][2]; nm++; }
    }
    float w2v[VRFIST_MAX_V]; memset(w2v, 0, sizeof w2v);
    float tph = 0, tps = 0;                 /* ángulos finales de base y punta (rad) con este agarre */
    float tax1[3] = {0, 0, 1}, tax2[3] = {0, 0, 1};
    if (nt >= 4 && nm > 0) {
        tcz /= (float)nt; mcz /= (float)nm;
        float sgz = tcz >= mcz ? 1.0f : -1.0f;
        float range = tsmax - tsmin + 1.0f;
        float B[3] = {0, 0, 0}, Tp[3] = {0, 0, 0}; int nb = 0, ntp = 0;
        for (int i = 0; i < nv; i++) {
            if (!is_thumb[i]) continue;
            float s = -X[i][1];
            if (s <= tsmin + 0.2f * range) { for (int k = 0; k < 3; k++) B[k] += X[i][k]; nb++; }
            if (s >= tsmax - 0.2f * range) { for (int k = 0; k < 3; k++) Tp[k] += X[i][k]; ntp++; }
        }
        if (nb > 0 && ntp > 0) {
            for (int k = 0; k < 3; k++) { B[k] /= (float)nb; Tp[k] /= (float)ntp; }
            float dv[3] = {Tp[0] - B[0], Tp[1] - B[1], Tp[2] - B[2]}, Lt = sqrtf(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
            if (Lt > 8.0f) {
                float dir0[3] = {dv[0] / Lt, dv[1] / Lt, dv[2] / Lt};
                /* ejes: base inclinada `thumb_tilt_deg` respecto a Z hacia el lado de los dedos */
                float tau = prm->thumb_tilt_deg * DEG;
                tax1[0] = sgz * sinf(tau); tax1[1] = 0; tax1[2] = cosf(tau);
                float jf = prm->thumb_joint < 0.2f ? 0.2f : (prm->thumb_joint > 0.8f ? 0.8f : prm->thumb_joint);
                float J0[3] = {B[0] + dv[0] * jf, B[1] + dv[1] * jf, B[2] + dv[2] * jf};
                tph = g * prm->thumb_base_deg * DEG; tps = g * prm->thumb_tip_deg * DEG;     /* flexión propia y limitada, proporcional al agarre (no sigue la curva de los dedos) */
                /* piel: segmento 1 (base -> J) gira con R1; segmento 2 (J -> punta) con R2 sobre R1; mezcla suave alrededor de J */
                float J1r[3], jr[3] = {J0[0] - B[0], J0[1] - B[1], J0[2] - B[2]}; rot_axis(tax1, tph, jr, J1r);
                float d1[3]; rot_axis(tax1, tph, dir0, d1);
                float dd = tax1[0] * d1[0] + tax1[1] * d1[1] + tax1[2] * d1[2], nl;
                for (int k = 0; k < 3; k++) tax2[k] = tax1[k] - dd * d1[k];
                nl = sqrtf(tax2[0] * tax2[0] + tax2[1] * tax2[1] + tax2[2] * tax2[2]);
                if (nl < 0.2f) { for (int k = 0; k < 3; k++) tax2[k] = tax1[k]; } else for (int k = 0; k < 3; k++) tax2[k] /= nl;
                float soft = prm->thumb_soft * Lt; if (soft < 0.5f) soft = 0.5f;
                for (int i = 0; i < nv; i++) {
                    if (!is_thumb[i]) continue;
                    float rr[3] = {X[i][0] - B[0], X[i][1] - B[1], X[i][2] - B[2]}, p1[3], p2[3];
                    float u = rr[0] * dir0[0] + rr[1] * dir0[1] + rr[2] * dir0[2];
                    rot_axis(tax1, tph, rr, p1);
                    float q[3] = {p1[0] - J1r[0], p1[1] - J1r[1], p1[2] - J1r[2]}, q2[3]; rot_axis(tax2, tps, q, q2);
                    for (int k = 0; k < 3; k++) p2[k] = J1r[k] + q2[k];
                    float w2 = sstep((u - jf * Lt) / (2.0f * soft) + 0.5f);
                    w2v[i] = w2;
                    for (int k = 0; k < 3; k++) Y[i][k] = B[k] + p1[k] + (p2[k] - p1[k]) * w2;
                }
            }
        }
    }

    for (int i = 0; i < nv; i++) {
        out_v[4 * i] = to_s16(sg * Y[i][0]); out_v[4 * i + 1] = to_s16(Y[i][1]); out_v[4 * i + 2] = to_s16(Y[i][2]);
    }

    /* normales: giran con el vértice que las usa */
    for (int j = 0; j < nn; j++) {
        int o = owner[j];
        if (o < 0) continue;
        float nx = sg * (float)m->n[4 * j], ny = (float)m->n[4 * j + 1], nz = (float)m->n[4 * j + 2];
        if (is_main[o] && dA[o] != 0.0f) {
            float c = cosf(dA[o]), s = sinf(dA[o]);
            float rx = nx * c - ny * s, ry = nx * s + ny * c; nx = rx; ny = ry;
        } else if (is_thumb[o] && (tph != 0.0f || tps != 0.0f)) {
            float n0[3] = {nx, ny, nz}, n1[3], n2[3];
            float l0 = sqrtf(nx * nx + ny * ny + nz * nz);
            rot_axis(tax1, tph, n0, n1); rot_axis(tax2, tps, n1, n2);
            float bl[3], w2 = w2v[o];
            for (int k = 0; k < 3; k++) bl[k] = n1[k] + (n2[k] - n1[k]) * w2;
            float lb = sqrtf(bl[0] * bl[0] + bl[1] * bl[1] + bl[2] * bl[2]);
            if (lb > 1e-6f && l0 > 0) { float k = l0 / lb; nx = bl[0] * k; ny = bl[1] * k; nz = bl[2] * k; }
        } else continue;
        out_n[4 * j] = to_s16(sg * nx); out_n[4 * j + 1] = to_s16(ny); out_n[4 * j + 2] = to_s16(nz);
    }
    return 0;
}
