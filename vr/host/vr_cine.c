#include "vr_cine.h"
#include <math.h>
#include <string.h>

void vrcine_config_default(VrCineConfig *c) {
    c->cut_dist = 2500.0f;
    c->cut_deg = 28.0f;
}

void vrcine_reset(VrCine *s) {
    VrCineConfig c = s->cfg;
    memset(s, 0, sizeof *s);
    s->cfg = c;
    if (!(c.cut_dist > 0)) vrcine_config_default(&s->cfg);
}

int vrcine_camera(VrCine *s, int active, const int32_t eye[3], const float M[9]) {
    if (!active) { s->have = 0; s->cut_now = 0; return 0; }
    float fwd[3] = {M[2], M[5], M[8]}, up[3] = {-M[1], -M[4], -M[7]};   /* Z delante, Y abajo -> arriba = -Y */
    s->cams++;
    int cut = !s->have;
    s->last_jump = s->last_turn = 0;
    if (s->have) {
        float dx = (float)(eye[0] - s->prev_eye[0]), dy = (float)(eye[1] - s->prev_eye[1]), dz = (float)(eye[2] - s->prev_eye[2]);
        s->last_jump = sqrtf(dx * dx + dy * dy + dz * dz);
        float d = fwd[0] * s->prev_fwd[0] + fwd[1] * s->prev_fwd[1] + fwd[2] * s->prev_fwd[2];
        float d2 = up[0] * s->prev_up[0] + up[1] * s->prev_up[1] + up[2] * s->prev_up[2];
        d = fminf(1.0f, fmaxf(-1.0f, d)); d2 = fminf(1.0f, fmaxf(-1.0f, d2));   /* fminf/fmaxf devuelven el otro argumento ante NaN */
        float a = acosf(d) * 57.2957795f, b = acosf(d2) * 57.2957795f;
        s->last_turn = a > b ? a : b;
        if (!isfinite(fwd[0] + fwd[1] + fwd[2] + up[0] + up[1] + up[2])) s->last_turn = NAN;
        if (!(s->last_jump <= s->cfg.cut_dist) || !(s->last_turn <= s->cfg.cut_deg)) cut = 1;   /* NaN => corte */
        if (cut) s->cuts++;
    }
    if (cut) {
        s->shot++;
        for (int i = 0; i < 3; i++) s->origin[i] = eye[i];
    }
    s->have = 1;
    for (int i = 0; i < 3; i++) { s->prev_eye[i] = eye[i]; s->prev_fwd[i] = fwd[i]; s->prev_up[i] = up[i]; }
    s->cut_now = cut;
    return cut;
}

/* inversa de una afín columna-mayor 3x3 + t; devuelve 0 si singular */
static int inv_affine(const float g[16], float o[12]) {   /* o: 3x3 fila-mayor inversa (9) + t' (3) */
    float a = g[0], b = g[4], c = g[8], d = g[1], e = g[5], f = g[9], gg = g[2], h = g[6], i = g[10];   /* fila r, columna c = g[c*4+r] */
    float A = e * i - f * h, B = c * h - b * i, C = b * f - c * e;
    float det = a * A + d * B + gg * C;
    if (!(fabsf(det) > 1e-18f)) return 0;
    float id = 1.0f / det;
    o[0] = A * id; o[1] = B * id; o[2] = C * id;
    o[3] = (f * gg - d * i) * id; o[4] = (a * i - c * gg) * id; o[5] = (c * d - a * f) * id;
    o[6] = (d * h - e * gg) * id; o[7] = (b * gg - a * h) * id; o[8] = (a * e - b * d) * id;
    for (int r = 0; r < 3; r++) o[9 + r] = -(o[3 * r] * g[12] + o[3 * r + 1] * g[13] + o[3 * r + 2] * g[14]);
    return 1;
}

int vrcine_eye(const float gv[16], int eye, float ipd, float conv, float tan_x, float tan_y, VrCineEye *out) {
    float inv[12];
    if (!inv_affine(gv, inv)) return 0;
    for (int i = 0; i < 16; i++) if (!isfinite(gv[i])) return 0;
    if (!(conv > 0.1f) || !(tan_x > 0.05f) || !(tan_y > 0.05f)) return 0;
    /* vista del juego (unidades) = inv; a metros GL: x, -y, -z con 0,001 m por unidad; luego el ojo a -+ipd/2 en x */
    const float s = 0.001f;
    float o = eye ? 0.5f * ipd : -0.5f * ipd;   /* posición del ojo en x de la cámara */
    float R[3][4];
    for (int r = 0; r < 3; r++) {
        float sg = r == 0 ? 1.0f : -1.0f;
        for (int c = 0; c < 3; c++) R[r][c] = sg * s * inv[3 * r + c];
        R[r][3] = sg * s * inv[9 + r];
    }
    R[0][3] -= o;
    memset(out->view, 0, sizeof out->view);
    for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) out->view[c * 4 + r] = R[r][c];
    for (int r = 0; r < 3; r++) out->view[12 + r] = R[r][3];
    out->view[15] = 1.0f;
    float sh = (0.5f * ipd) / conv;   /* desplazamiento de la ventana (tangente) */
    if (!eye) { out->tan_[0] = tan_x - sh; out->tan_[1] = tan_x + sh; }
    else { out->tan_[0] = tan_x + sh; out->tan_[1] = tan_x - sh; }
    out->tan_[2] = out->tan_[3] = tan_y;
    return 1;
}

int vrcine_project(const VrCineEye *e, const float p[3], float *u, float *v, float *depth) {
    const float *m = e->view;
    float x = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
    float y = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
    float z = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
    if (!(z < -1e-4f)) return 0;
    float tx = x / -z, ty = y / -z;
    *u = (tx + e->tan_[0]) / (e->tan_[0] + e->tan_[1]);
    *v = (e->tan_[2] - ty) / (e->tan_[2] + e->tan_[3]);
    if (depth) *depth = -z;
    return 1;
}
