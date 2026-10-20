#include "ns_cinema_ref.h"
#include "ns_cinema.h"
#include <math.h>
#include <string.h>

void ns_cine_engine_view(const NsCamera *cam, float A[9], float t[3]) {
    const float ys = (cam->overscan_y > 0.5f && cam->overscan_y < 2.0f) ? cam->overscan_y : NS_OVERSCAN_Y;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) A[i * 3 + j] = cam->rot[j * 3 + i] * (i == 1 ? ys : 1.0f);
    for (int i = 0; i < 3; i++) t[i] = -(A[i * 3] * cam->eye[0] + A[i * 3 + 1] * cam->eye[1] + A[i * 3 + 2] * cam->eye[2]);
}

void ns_cine_test_gv(const NsCamera *cam, int k, float gv[16]) {
    memset(gv, 0, 16 * sizeof(float)); gv[15] = 1;
    const float s = 0.001f;
    if (k == 0) { gv[0] = s; gv[5] = -s; gv[10] = -s; gv[13] = 1.5f; return; }
    if (k == 1 || k == 3) {
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) gv[c * 4 + r] = s * cam->rot[r * 3 + c] * ((k == 3 && r == 2) ? -1.0f : 1.0f);
        for (int r = 0; r < 3; r++) gv[12 + r] = s * cam->eye[r] * ((k == 3 && r == 2) ? -1.0f : 1.0f);
        return;
    }
    gv[2] = -s; gv[5] = -s; gv[8] = s; gv[12] = 2; gv[13] = 1.5f; gv[14] = -3;   /* x de vista -> -z VR; y abajo -> y arriba; z de vista -> +x VR */
}

static void mulv(const float m[16], const float v[4], float o[4]) { for (int r = 0; r < 4; r++) o[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * v[3]; }

int ns_cine_app_chain(const NsCamera *cam, const float gv[16], int eye, float ipd, float conv, float ua, float ub, NsRTri *tris, size_t n, int w, int h, NsRCam *c) {
    (void)ua; (void)ub;
    float t12[12];
    if (!ns_cinema_to_vr(cam, gv, t12)) return 0;
    float tx, ty, sh[4];
    if (!ns_cinema_window(cam, &tx, &ty, sh)) return 0;
    float view[16], tg[4];
    if (!ns_cinema_eye(gv, eye, ipd, conv, tx, ty, view, tg)) return 0;
    for (int i = 0; i < 4; i++) tg[i] += sh[i];
    if (!(tg[0] > 0.01f && tg[1] > 0.01f && tg[2] > 0.01f && tg[3] > 0.01f)) return 0;
    float P[16]; ns_cinema_projection(tg, 0.05f, 800.0f, P);
    const float W = (float)w, Hh = (float)h;
    memset(c, 0, sizeof *c);
    c->A[0] = 1; c->A[4] = -1; c->A[8] = -1;           /* GL (x der, y arriba, -z delante) -> ojo del rasterizador (x der, y abajo, z delante) */
    c->fx = W / (tg[0] + tg[1]); c->cx = W * tg[0] / (tg[0] + tg[1]);
    c->fy = Hh / (tg[2] + tg[3]); c->cy = Hh * tg[2] / (tg[2] + tg[3]);
    c->w = w; c->h = h; c->znear = 0.05f;
    double maxerr = 0;
    for (size_t i = 0; i < n; i++) for (int k = 0; k < 3; k++) {
        float *p = tris[i].v[k].p;
        float wv[4] = { t12[0] * p[0] + t12[1] * p[1] + t12[2] * p[2] + t12[3], t12[4] * p[0] + t12[5] * p[1] + t12[6] * p[2] + t12[7], t12[8] * p[0] + t12[9] * p[1] + t12[10] * p[2] + t12[11], 1 };
        float e[4]; mulv(view, wv, e);
        if (i % 97 == 0 && e[2] < -0.1f) {   /* punto delante: el píxel de la matriz de proyección debe igualar al pinhole */
            float cl[4]; mulv(P, e, cl);
            double px = (cl[0] / cl[3] * 0.5 + 0.5) * W, py = (0.5 - cl[1] / cl[3] * 0.5) * Hh;
            double qx = c->cx + c->fx * e[0] / -e[2], qy = c->cy + c->fy * -e[1] / -e[2];
            double d = fabs(px - qx) + fabs(py - qy); if (d > maxerr) maxerr = d;
        }
        p[0] = e[0]; p[1] = e[1]; p[2] = e[2];
    }
    return maxerr < 0.05;
}

void ns_cine_score(const uint8_t *a, const uint8_t *b, int w, int h, NsCineScore *o) {
    memset(o, 0, sizeof *o);
    const int B = 16, bw = w / B, bh = h / B;
    double sum = 0; int cnt = 0;
    double ma = 0, mb = 0, ax = 0, ay = 0, bx = 0, by = 0;
    #define LUM(p, i) (0.299 * (p)[(i) * 3] + 0.587 * (p)[(i) * 3 + 1] + 0.114 * (p)[(i) * 3 + 2])
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        double la = LUM(a, (size_t)y * w + x), lb = LUM(b, (size_t)y * w + x);
        ma += la; mb += lb; ax += la * (x + 0.5); ay += la * (y + 0.5); bx += lb * (x + 0.5); by += lb * (y + 0.5);
    }
    if (ma > 0) { o->com_a[0] = ax / ma / w; o->com_a[1] = ay / ma / h; }
    if (mb > 0) { o->com_b[0] = bx / mb / w; o->com_b[1] = by / mb / h; }
    o->com_dist = hypot(o->com_a[0] - o->com_b[0], o->com_a[1] - o->com_b[1]);
    for (int by2 = 0; by2 < bh; by2++) for (int bx2 = 0; bx2 < bw; bx2++) {
        double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0; int m = 0;
        for (int y = by2 * B; y < (by2 + 1) * B; y++) for (int x = bx2 * B; x < (bx2 + 1) * B; x++) {
            double la = LUM(a, (size_t)y * w + x), lb = LUM(b, (size_t)y * w + x);
            sa += la; sb += lb; saa += la * la; sbb += lb * lb; sab += la * lb; m++;
        }
        double va = saa - sa * sa / m, vb = sbb - sb * sb / m;
        if (va < 1e-6 && vb < 1e-6) continue;          /* bloque liso en ambas: no informa */
        double cov = sab - sa * sb / m;
        sum += (va > 1e-6 && vb > 1e-6) ? cov / sqrt(va * vb) : 0.0; cnt++;
    }
    o->block_corr = cnt ? sum / cnt : 0;
    #undef LUM
}

#include "ns_gpu.h"
size_t ns_cine_cull_back(NsRTri *tris, struct NsAdaptSrc *src, size_t n, const float eye[3]) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        const NsAdaptSrc *s = &src[i];
        if (!(s->mflags & NS_MF_TWOSIDED)) {
            const float *a = tris[i].v[0].p, *b = tris[i].v[1].p, *c = tris[i].v[2].p;
            if (s->half == 1 ? ns_gpu_tri_back(a, c, b, eye) : ns_gpu_tri_back(a, b, c, eye)) continue;
        }
        tris[o] = tris[i]; src[o] = src[i]; o++;
    }
    return o;
}
