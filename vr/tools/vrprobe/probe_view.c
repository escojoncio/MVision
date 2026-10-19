#include "probe_view.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "gpu.h"

typedef struct { float x, y, z, u, v, r, g, b; } PV;   /* cámara (x der, y arriba, z atrás), atributos */

typedef struct PvCamX PvCamX;
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int texel(uint16_t tpage, uint16_t clut, int u, int v, uint16_t *out) {
    int bx = (tpage & 15) * 64, by = ((tpage >> 4) & 1) * 256, tp = (tpage >> 7) & 3;
    u &= 255; v &= 255;
    uint16_t t;
    if (tp == 0) {
        uint16_t w = gpu_vram[(by + v) & 511][(bx + (u >> 2)) & 1023];
        int idx = (w >> ((u & 3) * 4)) & 15;
        t = gpu_vram[(clut >> 6) & 511][((clut & 63) * 16 + idx) & 1023];
    } else if (tp == 1) {
        uint16_t w = gpu_vram[(by + v) & 511][(bx + (u >> 1)) & 1023];
        int idx = (w >> ((u & 1) * 8)) & 255;
        t = gpu_vram[(clut >> 6) & 511][((clut & 63) * 16 + idx) & 1023];
    } else t = gpu_vram[(by + v) & 511][(bx + u) & 1023];
    *out = t;
    return t != 0;
}

static void draw_tri(uint8_t *rgb, float *zb, int W, int H, const PvCam *cam, const PV a_[3], const VrVtx *src, int kind, int blend) {
    float sx[3], sy[3], iw[3];
    for (int i = 0; i < 3; i++) {
        float d = -a_[i].z;
        iw[i] = 1.0f / d;
        sx[i] = cam->cx + cam->f * a_[i].x * iw[i];
        sy[i] = cam->cy - cam->f * a_[i].y * iw[i];
    }
    float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
    if (fabsf(area) < 1e-9f) return;
    float minx = fminf(sx[0], fminf(sx[1], sx[2])), maxx = fmaxf(sx[0], fmaxf(sx[1], sx[2])), miny = fminf(sy[0], fminf(sy[1], sy[2])), maxy = fmaxf(sy[0], fmaxf(sy[1], sy[2]));
    int x0 = clampi((int)floorf(minx), 0, W - 1), x1 = clampi((int)ceilf(maxx), 0, W - 1), y0 = clampi((int)floorf(miny), 0, H - 1), y1 = clampi((int)ceilf(maxy), 0, H - 1);
    uint16_t fl = src[0].flags;
    int textured = (fl & 2) != 0, raw = (fl & 4) != 0, semi = kind == 1 || (fl & 8);
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        float px = x + 0.5f, py = y + 0.5f;
        float w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
        float w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
        float w2 = 1.0f - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        float q0 = w0 * iw[0], q1 = w1 * iw[1], q2 = w2 * iw[2], qs = q0 + q1 + q2;
        float depth = 1.0f / qs;
        float *zp = &zb[y * W + x];
        if (depth >= *zp) continue;
        float u = (q0 * a_[0].u + q1 * a_[1].u + q2 * a_[2].u) / qs, v = (q0 * a_[0].v + q1 * a_[1].v + q2 * a_[2].v) / qs;
        float cr = (q0 * a_[0].r + q1 * a_[1].r + q2 * a_[2].r) / qs, cg = (q0 * a_[0].g + q1 * a_[1].g + q2 * a_[2].g) / qs, cb = (q0 * a_[0].b + q1 * a_[1].b + q2 * a_[2].b) / qs;
        float r, g, b;
        int blend_this = semi;
        if (textured) {
            uint16_t t;
            if (!texel(src[0].tpage, src[0].clut, (int)floorf(u + 0.001f), (int)floorf(v + 0.001f), &t)) continue;
            float tr = ((t & 31) << 3) | ((t & 31) >> 2), tg = (((t >> 5) & 31) << 3) | (((t >> 5) & 31) >> 2), tb = (((t >> 10) & 31) << 3) | (((t >> 10) & 31) >> 2);
            if (raw) { r = tr; g = tg; b = tb; } else { r = fminf(255.f, tr * cr / 128.f); g = fminf(255.f, tg * cg / 128.f); b = fminf(255.f, tb * cb / 128.f); }
            if (semi && !(t & 0x8000)) blend_this = 0;
        } else { r = cr; g = cg; b = cb; }
        uint8_t *d = rgb + ((size_t)y * W + x) * 3;
        if (blend_this) {
            float B[3] = {d[0], d[1], d[2]}, F[3] = {r, g, b}, o[3];
            for (int k = 0; k < 3; k++) {
                switch (blend & 3) { case 0: o[k] = 0.5f * B[k] + 0.5f * F[k]; break; case 1: o[k] = B[k] + F[k]; break; case 2: o[k] = B[k] - F[k]; break; default: o[k] = B[k] + 0.25f * F[k]; }
                d[k] = (uint8_t)clampi((int)o[k], 0, 255);
            }
            /* semitransparente: no escribe profundidad */
        } else { d[0] = (uint8_t)clampi((int)r, 0, 255); d[1] = (uint8_t)clampi((int)g, 0, 255); d[2] = (uint8_t)clampi((int)b, 0, 255); *zp = depth; }
    }
}

void pv_cam_head(PvCam *c, const float head[16], float fov_deg, int w, int h) {
    for (int i = 0; i < 3; i++) { c->R[i] = head[i]; c->R[3 + i] = head[4 + i]; c->R[6 + i] = head[8 + i]; c->p[i] = head[12 + i]; }
    c->w = w; c->h = h; c->cx = 0.5f * w; c->cy = 0.5f * h; c->f = 0.5f * w / tanf(fov_deg * 0.5f * 0.01745329252f);
}
void pv_cam_engine(PvCam *c, const VrScene *sc, int w, int h) {
    /* cámara del motor: game_view = vista (x der, y abajo, z adelante) -> mundo; filas de R = der, arriba, atrás */
    const float *g = sc->game_view;
    float r[3] = {g[0], g[1], g[2]}, d[3] = {g[4], g[5], g[6]}, f[3] = {g[8], g[9], g[10]};
    float lr = sqrtf(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]), ld = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]), lf = sqrtf(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
    for (int i = 0; i < 3; i++) { c->R[i] = r[i] / lr; c->R[3 + i] = -d[i] / ld; c->R[6 + i] = -f[i] / lf; c->p[i] = g[12 + i]; }
    c->w = w; c->h = h; c->cx = sc->filter.cx_col * (float)w / 320.0f; c->cy = sc->filter.cy_row * (float)h / 256.0f; c->f = sc->filter.proj_h * (float)w / 320.0f;
}
void pv_render(const VrScene *sc, const PvCam *cam, uint8_t *rgb, uint8_t bg, float near_m, const uint32_t *tsrc, const uint8_t *keep, uint32_t nkeep) {
    int W = cam->w, H = cam->h;
    const float *head = NULL; (void)head;
    memset(rgb, bg, (size_t)W * H * 3);
    float *zb = malloc((size_t)W * H * sizeof(float));
    if (!zb) return;
    for (int i = 0; i < W * H; i++) zb[i] = 1e30f;
    for (uint32_t bi = 0; bi < sc->nb; bi++) {
        const VrBatch *b = &sc->b[bi];
        if (b->kind > 1) continue;
        for (uint32_t j = 0; j + 2 < b->count; j += 3) {
            if ((uint64_t)b->first + j + 2 >= sc->nv) break;
            const VrVtx *s = &sc->v[b->first + j];
            if (tsrc && keep) { uint32_t op = tsrc[(b->first + j) / 3]; if (op < nkeep && !keep[op]) continue; }
            PV p[3];
            for (int k = 0; k < 3; k++) {
                float d[3] = {s[k].pos[0] - cam->p[0], s[k].pos[1] - cam->p[1], s[k].pos[2] - cam->p[2]};
                p[k].x = cam->R[0] * d[0] + cam->R[1] * d[1] + cam->R[2] * d[2];
                p[k].y = cam->R[3] * d[0] + cam->R[4] * d[1] + cam->R[5] * d[2];
                p[k].z = cam->R[6] * d[0] + cam->R[7] * d[1] + cam->R[8] * d[2];
                p[k].u = s[k].uv[0]; p[k].v = s[k].uv[1];
                p[k].r = (float)(s[k].rgba & 255); p[k].g = (float)((s[k].rgba >> 8) & 255); p[k].b = (float)((s[k].rgba >> 16) & 255);
                if (!(s[0].flags & 1)) { p[k].r = (float)(s[0].rgba & 255); p[k].g = (float)((s[0].rgba >> 8) & 255); p[k].b = (float)((s[0].rgba >> 16) & 255); }
            }
            /* recorte contra z <= -near */
            PV poly[4]; int n = 0;
            for (int k = 0; k < 3; k++) {
                const PV *A = &p[k], *B = &p[(k + 1) % 3];
                int ia = A->z <= -near_m, ib = B->z <= -near_m;
                if (ia) poly[n++] = *A;
                if (ia != ib) {
                    float t = (-near_m - A->z) / (B->z - A->z);
                    PV *o = &poly[n++];
                    o->x = A->x + (B->x - A->x) * t; o->y = A->y + (B->y - A->y) * t; o->z = -near_m;
                    o->u = A->u + (B->u - A->u) * t; o->v = A->v + (B->v - A->v) * t;
                    o->r = A->r + (B->r - A->r) * t; o->g = A->g + (B->g - A->g) * t; o->b = A->b + (B->b - A->b) * t;
                }
            }
            for (int f = 1; f + 1 < n; f++) { PV tri[3] = {poly[0], poly[f], poly[f + 1]}; draw_tri(rgb, zb, W, H, cam, tri, s, b->kind, b->blend); }
        }
    }
    free(zb);
}
