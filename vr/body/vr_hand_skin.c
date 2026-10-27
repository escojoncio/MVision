/* vr_hand_skin: ver vr_hand_skin.h. */
#include "vr_hand_skin.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DEG 0.017453292519943295f

/* Ángulos con el cierre al máximo (grados), por dedo (índice, corazón, anular, meñique): metacarpo (ahuecar la palma), MCP, PIP y DIP. Flexión = giro sobre -X local. */
static const float FINGER_DEG[4][4] = {
    {0.0f, 80.0f, 100.0f, 62.0f},
    {0.0f, 84.0f, 102.0f, 64.0f},
    {3.0f, 86.0f, 102.0f, 62.0f},
    {7.0f, 88.0f, 98.0f, 60.0f},
};
/* Separación de los dedos (giro sobre Y local de la falange proximal, grados) que se cierra al flexionar: convergen en el puño. */
static const float FINGER_CONVERGE_DEG[4] = {-4.0f, 0.0f, 4.0f, 9.0f};
/* Pulgar con el cierre al máximo: metacarpo (flexión sobre -X y oposición sobre +Y), proximal y distal. Separación: giro del metacarpo sobre -Z (alejarse de la palma). */
static const float THUMB_MC_FLEX = 34.0f, THUMB_MC_OPP = 30.0f, THUMB_PROX = 38.0f, THUMB_DIST = 52.0f, THUMB_SPREAD = 22.0f;

static float c01(float x) { return (x == x && x - x == 0.0f) ? (x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x)) : 0.0f; }
static float cpm1(float x) { return (x == x && x - x == 0.0f) ? (x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x)) : 0.0f; }

static void xf_mul(float *o, const float *a, const float *b) {   /* o = a * b (3x4) */
    float r[12];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) r[4 * i + j] = a[4 * i] * b[j] + a[4 * i + 1] * b[4 + j] + a[4 * i + 2] * b[8 + j];
        r[4 * i + 3] = a[4 * i] * b[3] + a[4 * i + 1] * b[7] + a[4 * i + 2] * b[11] + a[4 * i + 3];
    }
    memcpy(o, r, sizeof r);
}
static void xf_inv(float *o, const float *a) {   /* inversa de una transformación rígida */
    float r[12];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r[4 * i + j] = a[4 * j + i];
    for (int i = 0; i < 3; i++) r[4 * i + 3] = -(r[4 * i] * a[3] + r[4 * i + 1] * a[7] + r[4 * i + 2] * a[11]);
    memcpy(o, r, sizeof r);
}
static void rot_axis(float *o, const float ax[3], float ang) {   /* 3x4 sin traslación */
    float c = cosf(ang), s = sinf(ang), t = 1.0f - c, x = ax[0], y = ax[1], z = ax[2];
    float r[12] = {t * x * x + c, t * x * y - s * z, t * x * z + s * y, 0, t * x * y + s * z, t * y * y + c, t * y * z - s * x, 0, t * x * z - s * y, t * y * z + s * x, t * z * z + c, 0};
    memcpy(o, r, sizeof r);
}
static void mirror_xf(float *o, const float *a) {   /* S a S, S = diag(-1, 1, 1) */
    static const float sg[12] = {1, -1, -1, -1, -1, 1, 1, 1, -1, 1, 1, 1};
    for (int k = 0; k < 12; k++) o[k] = a[k] * sg[k];
}

void vrhs_pose(const float curl[5], float thumb_spread, int side, float xf[VRHS_J][12]) {
    float c[5];
    for (int i = 0; i < 5; i++) c[i] = curl ? c01(curl[i]) : 0.0f;
    const float sp = cpm1(thumb_spread);
    static const float AX[3] = {1, 0, 0}, AY[3] = {0, 1, 0}, AZ[3] = {0, 0, 1};
    for (int j = 0; j < VRHS_J; j++) {
        float loc[12], r[12], r2[12];
        const int p = vrha_parent[j];
        if (p < 0) memcpy(loc, vrha_bind[j], sizeof loc);
        else xf_mul(loc, vrha_ibm[p], vrha_bind[j]);                 /* local de reposo respecto al padre */
        memset(r, 0, sizeof r); r[0] = r[5] = r[10] = 1;
        if (j >= VRHS_INDEX_MC) {
            const int f = (j - VRHS_INDEX_MC) / 5, k = (j - VRHS_INDEX_MC) % 5;   /* k: 0 metacarpo, 1 proximal, 2 media, 3 distal, 4 punta */
            if (k <= 3) {
                rot_axis(r, AX, -FINGER_DEG[f][k] * DEG * c[f]);
                if (k == 1 && FINGER_CONVERGE_DEG[f] != 0.0f) { rot_axis(r2, AY, FINGER_CONVERGE_DEG[f] * DEG * c[f]); xf_mul(r, r2, r); }
            }
        } else if (j == VRHS_THUMB_MC) {
            rot_axis(r, AX, -THUMB_MC_FLEX * DEG * c[4]);
            rot_axis(r2, AY, THUMB_MC_OPP * DEG * c[4]); xf_mul(r, r2, r);
            rot_axis(r2, AZ, -THUMB_SPREAD * DEG * sp); xf_mul(r, r2, r);
        } else if (j == VRHS_THUMB_PROX) rot_axis(r, AX, -THUMB_PROX * DEG * c[4]);
        else if (j == VRHS_THUMB_DIST) rot_axis(r, AX, -THUMB_DIST * DEG * c[4]);
        xf_mul(loc, loc, r);
        if (p < 0) memcpy(xf[j], loc, sizeof loc);
        else xf_mul(xf[j], xf[p], loc);
    }
    if (side < 0) for (int j = 0; j < VRHS_J; j++) mirror_xf(xf[j], xf[j]);
}

void vrhs_skin_mats(const float xf[VRHS_J][12], int side, float sk[VRHS_J][12]) {
    for (int j = 0; j < VRHS_J; j++) {
        float a[12];
        if (side < 0) mirror_xf(a, xf[j]); else memcpy(a, xf[j], sizeof a);
        xf_mul(sk[j], a, vrha_ibm[j]);
    }
}

void vrhs_skin(const float xf[VRHS_J][12], int side, float (*pos)[3], float (*nrm)[3]) {
    float sk[VRHS_J][12];
    vrhs_skin_mats(xf, side, sk);
    const float sx = side < 0 ? -1.0f : 1.0f;
    for (int i = 0; i < VRHA_NV; i++) {
        float m[12] = {0};
        for (int k = 0; k < 4; k++) {
            const float w = vrha_weight[i][k];
            if (w <= 0.0f) continue;
            const float *s = sk[vrha_joint[i][k]];
            for (int q = 0; q < 12; q++) m[q] += w * s[q];
        }
        const float *v = vrha_pos[i];
        float p[3];
        for (int a = 0; a < 3; a++) p[a] = m[4 * a] * v[0] + m[4 * a + 1] * v[1] + m[4 * a + 2] * v[2] + m[4 * a + 3];
        pos[i][0] = sx * p[0]; pos[i][1] = p[1]; pos[i][2] = p[2];
        if (nrm) {
            const float *n = vrha_nrm[i];
            float o[3];
            for (int a = 0; a < 3; a++) o[a] = m[4 * a] * n[0] + m[4 * a + 1] * n[1] + m[4 * a + 2] * n[2];
            float l = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
            if (!(l > 1e-6f)) { o[0] = n[0]; o[1] = n[1]; o[2] = n[2]; l = 1.0f; }
            nrm[i][0] = sx * o[0] / l; nrm[i][1] = o[1] / l; nrm[i][2] = o[2] / l;
        }
    }
}

void vrhs_handle_axis(int side, float out[3]) {
    const float th = VRHS_HANDLE_TILT_DEG * DEG;
    out[0] = 0.0f; out[1] = -sinf(th); out[2] = cosf(th);
    (void)side;   /* el espejo X no cambia el eje (x = 0) */
}

void vrhs_grip_center(int side, float out[3]) {
    /* altura: 65 % del metacarpo del corazón (de su base a los nudillos); z: la del dedo corazón. x: superficie de la palma (vértices con cara palmar) a lo largo del eje del mango
     * entre el meñique y el índice, más el radio del mango */
    const float *mc = vrha_bind[VRHS_MIDDLE_MC], *pp = vrha_bind[VRHS_MIDDLE_MC + 1];
    const float y0 = 0.35f * mc[7] + 0.65f * pp[7], z0 = pp[11], tn = tanf(VRHS_HANDLE_TILT_DEG * DEG);
    float xs = -1e9f;
    for (int i = 0; i < VRHA_NV; i++) {
        const float *v = vrha_pos[i];
        if (vrha_nrm[i][0] < 0.3f || v[2] < z0 - 40.0f || v[2] > z0 + 20.0f) continue;
        if (fabsf(v[1] - (y0 - (v[2] - z0) * tn)) < 8.0f && v[0] > xs) xs = v[0];
    }
    out[0] = (side < 0 ? -1.0f : 1.0f) * (xs + VRHS_HANDLE_R_MM); out[1] = y0; out[2] = z0;
}

void vrhs_index_pad(const float xf[VRHS_J][12], float out[3]) {
    const float *d = xf[VRHS_INDEX_DIST], *t = xf[VRHS_INDEX_TIP];
    for (int a = 0; a < 3; a++) out[a] = 0.4f * d[4 * a + 3] + 0.6f * t[4 * a + 3] - 6.0f * d[4 * a + 1];   /* -6 mm sobre el +Y local (dorso): cara palmar */
}

void vrhs_fist_point(int side, float out[3]) {
    float fp[3];
    {
        float (*pos)[3] = (float (*)[3])malloc(sizeof(float[VRHA_NV][3]));
        if (!pos) { out[0] = out[1] = out[2] = 0.0f; return; }
        float xf[VRHS_J][12];
        const float curl[5] = {1, 1, 1, 1, 1};
        vrhs_pose(curl, 0.0f, 1, xf);
        vrhs_skin(xf, 1, pos, NULL);
        float ymin = 1e9f;
        for (int i = 0; i < VRHA_NV; i++) if (pos[i][1] < ymin) ymin = pos[i][1];
        float s[3] = {0, 0, 0}; int n = 0;
        for (int i = 0; i < VRHA_NV; i++) if (pos[i][1] < ymin + 8.0f) { for (int a = 0; a < 3; a++) s[a] += pos[i][a]; n++; }
        for (int a = 0; a < 3; a++) fp[a] = s[a] / (float)n;
        free(pos);
    }
    out[0] = side < 0 ? -fp[0] : fp[0]; out[1] = fp[1]; out[2] = fp[2];
}
