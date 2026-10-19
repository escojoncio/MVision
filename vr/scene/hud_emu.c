#include "hud_emu.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define VR_VF_TEXTURED 2
#define VR_VF_RAW 4
#define VR_VF_FLIPX 128
#define VR_VF_FLIPY 256
#define VR_VF_RECT 0x4000

HudImg hud_img_new(int w, int h) {
    HudImg i = {w, h, calloc((size_t)w * h * 4, sizeof(float))};
    return i;
}
void hud_img_free(HudImg *im) { free(im->rgba); im->rgba = NULL; }

void hud_emu_event(const GpuDrawList *l, const GpuOp *o, uint16_t *vram) {
    switch (o->type) {
    case GPU_OP_FILL:
        for (int y = 0; y < o->rh; y++)
            for (int x = 0; x < o->rw; x++) vram[((o->ry + y) & 511) * 1024 + ((o->rx + x) & 1023)] = o->fill15;
        break;
    case GPU_OP_VCOPY: {
        uint16_t *tmp = malloc((size_t)o->rw * o->rh * 2 + 2);
        for (int y = 0; y < o->rh; y++)
            for (int x = 0; x < o->rw; x++) tmp[y * o->rw + x] = vram[((o->sy + y) & 511) * 1024 + ((o->sx + x) & 1023)];
        for (int y = 0; y < o->rh; y++)
            for (int x = 0; x < o->rw; x++) vram[((o->ry + y) & 511) * 1024 + ((o->rx + x) & 1023)] = tmp[y * o->rw + x];
        free(tmp);
        break;
    }
    case GPU_OP_UPLOAD: {
        uint32_t k = 0;
        for (int y = 0; y < o->rh; y++)
            for (int x = 0; x < o->rw && k < o->data_n; x++, k++)
                vram[((o->ry + y) & 511) * 1024 + ((o->rx + x) & 1023)] = l->data[o->data_off + k];
        break;
    }
    default: break;
    }
}

/* --- vr_texel de vr_scene.metal --- */
static uint32_t texel(const uint16_t *vram, uint32_t tpage, uint32_t clut, uint32_t tw, int u, int v) {
    int tx = (int)(tpage & 15u) * 64, ty = (int)((tpage >> 4) & 1u) * 256;
    uint32_t tp = (tpage >> 7) & 3u;
    int clx = (int)(clut & 63u) * 16, cly = (int)((clut >> 6) & 511u);
    u &= 255; v &= 255;
    if (tw != 0u) {
        int mx = (int)(tw & 255u), my = (int)((tw >> 8) & 255u), ox = (int)((tw >> 16) & 255u), oy = (int)((tw >> 24) & 255u);
        u = (u & ~(mx * 8)) | ((ox & mx) * 8);
        v = (v & ~(my * 8)) | ((oy & my) * 8);
    }
    int y = (ty + v) & 511;
    if (tp == 0u) {
        uint32_t w = vram[y * 1024 + ((tx + (u >> 2)) & 1023)];
        uint32_t idx = (w >> (uint32_t)((u & 3) * 4)) & 15u;
        return vram[cly * 1024 + ((clx + (int)idx) & 1023)];
    } else if (tp == 1u) {
        uint32_t w = vram[y * 1024 + ((tx + (u >> 1)) & 1023)];
        uint32_t idx = (w >> (uint32_t)((u & 1) * 8)) & 255u;
        return vram[cly * 1024 + ((clx + (int)idx) & 1023)];
    }
    return vram[y * 1024 + ((tx + u) & 1023)];
}
static void expand(uint32_t t, float *o) {
    for (int k = 0; k < 3; k++) { uint32_t c = (t >> (5 * k)) & 31u; o[k] = (float)((c << 3) | (c >> 2)) / 255.0f; }
}

typedef struct { float x, y, u, v; } PV;
extern const VrState *hud_cur_states; extern uint32_t hud_cur_nstates;

/* fragmento: devuelve 0 si se descarta; rgb en [0,1] */
static int frag(const uint16_t *vram, const VrVtx *v0, float u, float v, const float *col, uint32_t pass, float *rgb) {
    uint32_t flags = v0->flags, clut = v0->clut;
    if (flags & VR_VF_TEXTURED) {
        if (flags & VR_VF_RECT) { if (flags & VR_VF_FLIPX) u += 1.0f; if (flags & VR_VF_FLIPY) v += 1.0f; }
        extern const VrState *hud_cur_states; extern uint32_t hud_cur_nstates;
        uint32_t tw = 0;
        if (v0->pad != 0xFFFFu && v0->pad < hud_cur_nstates) {
            const VrState *st = &hud_cur_states[v0->pad];
            tw = st->twmx | ((uint32_t)st->twmy << 8) | ((uint32_t)st->twox << 16) | ((uint32_t)st->twoy << 24);
        }
        uint32_t tn = texel(vram, v0->tpage, clut, tw, (int)floorf(u), (int)floorf(v));
        if (tn == 0u) return 0;
        float tex[3]; expand(tn, tex);
        int b15 = (tn & 0x8000u) != 0u;
        if (pass == 1u && !b15) return 0;
        if (pass == 2u && b15) return 0;
        for (int k = 0; k < 3; k++) rgb[k] = (flags & VR_VF_RAW) ? tex[k] : tex[k] * col[k];
    } else {
        if (pass == 2u) return 0;
        for (int k = 0; k < 3; k++) rgb[k] = col[k] * (128.0f / 255.0f);
    }
    for (int k = 0; k < 3; k++) rgb[k] = rgb[k] < 0 ? 0 : rgb[k] > 1 ? 1 : rgb[k];
    return 1;
}

const VrState *hud_cur_states; uint32_t hud_cur_nstates;

static void blend(float *d, const float *f, int mode) {  /* d: rgba premultiplicado; f: rgb fragmento (alfa 1); mode -1 sin mezcla */
    float a = mode == 3 ? 0.25f : 0.5f;
    float na;
    float nd[3];
    switch (mode) {
    case 0: for (int k = 0; k < 3; k++) nd[k] = a * f[k] + (1 - a) * d[k]; na = a * 1.0f + (1 - a) * d[3]; break;
    case 1: for (int k = 0; k < 3; k++) nd[k] = f[k] + d[k]; na = d[3]; break;
    case 2: for (int k = 0; k < 3; k++) nd[k] = d[k] - f[k]; na = d[3]; break;
    case 3: for (int k = 0; k < 3; k++) nd[k] = a * f[k] + d[k]; na = d[3]; break;
    default: for (int k = 0; k < 3; k++) nd[k] = f[k]; na = 1.0f; break;
    }
    for (int k = 0; k < 3; k++) d[k] = nd[k] < 0 ? 0 : nd[k] > 1 ? 1 : nd[k];   /* rgba16Unorm satura */
    d[3] = na < 0 ? 0 : na > 1 ? 1 : na;
}

static float edge(float ax, float ay, float bx, float by, float px, float py) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); }

static void draw_tri(const uint16_t *vram, HudImg *im, const VrVtx *t, int rx, int ry, uint32_t pass, int mode) {
    PV p[3];
    for (int k = 0; k < 3; k++) { p[k].x = t[k].pos[0] - (float)rx; p[k].y = t[k].pos[1] - (float)ry; p[k].u = t[k].uv[0]; p[k].v = t[k].uv[1]; }
    float area = edge(p[0].x, p[0].y, p[1].x, p[1].y, p[2].x, p[2].y);
    if (area == 0) return;
    int x0 = (int)floorf(fminf(p[0].x, fminf(p[1].x, p[2].x))), x1 = (int)ceilf(fmaxf(p[0].x, fmaxf(p[1].x, p[2].x)));
    int y0 = (int)floorf(fminf(p[0].y, fminf(p[1].y, p[2].y))), y1 = (int)ceilf(fmaxf(p[0].y, fmaxf(p[1].y, p[2].y)));
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > im->w) x1 = im->w;
    if (y1 > im->h) y1 = im->h;
    /* recorte vigente de la primitiva (E3/E4, inclusivo, coordenadas de VRAM como los vértices 2D): vr_fs_scene lo aplica sobre la posición original interpolada */
    if (t[0].pad != 0xFFFFu && t[0].pad < hud_cur_nstates) {
        const VrState *st = &hud_cur_states[t[0].pad];
        int cx1 = st->cx1 - rx, cx2 = st->cx2 - rx + 1, cy1 = st->cy1 - ry, cy2 = st->cy2 - ry + 1;
        if (x0 < cx1) x0 = cx1;
        if (y0 < cy1) y0 = cy1;
        if (x1 > cx2) x1 = cx2;
        if (y1 > cy2) y1 = cy2;
    }
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = edge(p[1].x, p[1].y, p[2].x, p[2].y, px, py) / area, w1 = edge(p[2].x, p[2].y, p[0].x, p[0].y, px, py) / area,
                  w2 = 1.0f - w0 - w1;
            const float eps = -1e-6f;
            if (w0 < eps || w1 < eps || w2 < eps) continue;
            /* regla de cobertura: borde izquierdo/superior incluido (aprox.): si cae justo en el borde, solo uno de los dos triángulos */
            if (w0 == 0 || w1 == 0 || w2 == 0) {
                /* píxeles de centro exactamente en una arista compartida: cubrir solo si la arista es "superior-izquierda" */
                int skip = 0;
                for (int e = 0; e < 3 && !skip; e++) {
                    float w = e == 0 ? w0 : e == 1 ? w1 : w2;
                    if (w != 0) continue;
                    const PV *a = &p[(e + 1) % 3], *b = &p[(e + 2) % 3];
                    float dx = b->x - a->x, dy = b->y - a->y;
                    if (area < 0) { dx = -dx; dy = -dy; }
                    int top_left = (dy == 0 && dx < 0) || dy > 0;   /* convención D3D con y hacia abajo y orientación positiva */
                    if (!top_left) skip = 1;
                }
                if (skip) continue;
            }
            float u = w0 * p[0].u + w1 * p[1].u + w2 * p[2].u, v = w0 * p[0].v + w1 * p[1].v + w2 * p[2].v;
            float rgb[3];
            float col[3];
            for (int k = 0; k < 3; k++)
                col[k] = (w0 * (float)((t[0].rgba >> (8 * k)) & 255u) + w1 * (float)((t[1].rgba >> (8 * k)) & 255u) + w2 * (float)((t[2].rgba >> (8 * k)) & 255u)) / 128.0f;
            if (!frag(vram, &t[0], u, v, col, pass, rgb)) continue;
            blend(&im->rgba[((size_t)y * im->w + x) * 4], rgb, mode);
        }
}

void hud_emu_run(const VrScene *s, uint16_t *vram, HudImg *im, int rx, int ry) {
    hud_cur_states = s->states; hud_cur_nstates = s->nstates;
    const GpuDrawList *l = s->src;
    uint32_t next_ev = 0;
    for (uint32_t bi = 0; bi < s->nb; bi++) {
        const VrBatch *b = &s->b[bi];
        while (next_ev < b->ev_end && next_ev < l->nops) {
            hud_emu_event(l, &l->ops[next_ev], vram);
            next_ev++;
        }
        if (b->kind != 2 || !im) continue;
        int hastex = 0;
        for (uint32_t j = 0; j < b->count; j++) if (s->v[b->first + j].flags & VR_VF_TEXTURED) { hastex = 1; break; }
        int semi = b->blend <= 3;
        for (uint32_t j = 0; j + 2 < b->count; j += 3) {
            const VrVtx *t = &s->v[b->first + j];
            if (semi) {
                draw_tri(vram, im, t, rx, ry, hastex ? 1 : 0, b->blend);
                if (hastex) draw_tri(vram, im, t, rx, ry, 2, -1);
            } else draw_tri(vram, im, t, rx, ry, 0, -1);
        }
    }
    while (next_ev < l->nops) { hud_emu_event(l, &l->ops[next_ev], vram); next_ev++; }
}

void hud_emu_to_rgb(const HudImg *im, uint8_t *rgb) {
    for (int i = 0; i < im->w * im->h; i++) {
        const float *c = &im->rgba[i * 4];
        float a = fmaxf(c[3], fmaxf(c[0], fmaxf(c[1], c[2])));
        float o[3] = {0, 0, 0};
        if (a > 0) for (int k = 0; k < 3; k++) { float u = c[k] / a; u = u < 0 ? 0 : u > 1 ? 1 : u; o[k] = u * (a > 1 ? 1 : a); }
        for (int k = 0; k < 3; k++) rgb[i * 3 + k] = (uint8_t)(o[k] * 255.0f + 0.5f);
    }
}

/* --- vr_fs_post de vr_scene.metal (mantener sincronizado) --- */
static float post_ysrc(const VrFilter *f, float yc, float cy_img, float f_eye) {
    if (f->nrows <= 0 || !(f_eye > 1.0f) || !(f->proj_h > 0.0f)) return yc;
    float t = (yc - cy_img) / f_eye;
    float r = f->cy_row + f->proj_h * t;
    float d = 0.0f;
    if (r >= 0.0f && r < (float)VR_FILTER_ROWS) {
        float rr = r - 0.5f, fl = floorf(rr);
        int i0 = (int)fl, i1 = i0 + 1 < VR_FILTER_ROWS - 1 ? i0 + 1 : VR_FILTER_ROWS - 1;
        float fr = rr - fl;
        if (i0 < 0) i0 = 0;
        float d0 = f->rowcov[i0] ? f->dv[i0] : 0.0f, d1 = f->rowcov[i1] ? f->dv[i1] : 0.0f;
        d = d0 + (d1 - d0) * fr;
    }
    return yc + f_eye * d / f->proj_h;
}
/* mezcla del modo original m con peso w (vr_post_blend de vr_scene.metal) */
static float post_blend(float c, float f, int m, float w) {
    switch (m & 3) {
    case 0: return c + (0.5f * c + 0.5f * f - c) * w;
    case 1: return c + f * w;
    case 2: return c - f * w;
    default: return c + 0.25f * f * w;
    }
}
static float sat1(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float frac1(float v) { return v - floorf(v); }
/* vr_post_scan */
static float post_scan(const VrFilter *f, float yc, float cy_img, float f_eye) {
    if (f->n_scan <= 0 || f->scan_pitch < 1.0f || f_eye <= 1.0f || f->proj_h <= 0.0f) return 0.0f;
    float pitch = f->scan_pitch, duty = sat1(f->scan_h / pitch);
    float r = f->cy_row + f->proj_h * ((yc - cy_img) / f_eye);
    float t = frac1((r - f->scan_phase) / pitch);
    float pe = pitch * f_eye / f->proj_h, e = pitch / (pe > 1e-3f ? pe : 1e-3f), edge = e > 1e-4f ? e : 1e-4f;
    float w = sat1((duty - t) * pitch / edge + 0.5f) * sat1(t * pitch / edge + 0.5f);
    float k = sat1((3.0f - pe) * 0.5f);
    return w + (duty - w) * k;
}
/* vr_post_frame: bilineal con clamp (centro de celda i + 0,5), 1 fuera de la imagen original */
static float post_frame(const VrFilter *f, float px, float py, float cx_img, float cy_img, float fx_eye, float f_eye) {
    if (!f->frame_on || !f->frame || f->frame_w <= 0 || f->frame_h <= 0 || f_eye <= 1.0f || fx_eye <= 1.0f || f->proj_h <= 0.0f || f->area[2] <= 0 || f->area[3] <= 0) return 0.0f;
    float k = f->proj_h;
    float cx = f->cx_col + k * ((px - cx_img) / fx_eye), cy = f->cy_row + k * ((py - cy_img) / f_eye);
    float u = cx / (float)f->area[2], v = cy / (float)f->area[3];
    if (u < 0 || u > 1 || v < 0 || v > 1) return 1.0f;
    float fx = u * f->frame_w - 0.5f, fy = v * f->frame_h - 0.5f, x0f = floorf(fx), y0f = floorf(fy);
    int x0 = (int)x0f, y0 = (int)y0f, x1 = x0 + 1, y1 = y0 + 1;
    float wx = fx - x0f, wy = fy - y0f;
    x0 = x0 < 0 ? 0 : x0 > f->frame_w - 1 ? f->frame_w - 1 : x0; x1 = x1 < 0 ? 0 : x1 > f->frame_w - 1 ? f->frame_w - 1 : x1;
    y0 = y0 < 0 ? 0 : y0 > f->frame_h - 1 ? f->frame_h - 1 : y0; y1 = y1 < 0 ? 0 : y1 > f->frame_h - 1 ? f->frame_h - 1 : y1;
    float a = f->frame[y0 * f->frame_w + x0], b = f->frame[y0 * f->frame_w + x1], c = f->frame[y1 * f->frame_w + x0], d = f->frame[y1 * f->frame_w + x1];
    float top = a + (b - a) * wx, bot = c + (d - c) * wx;
    return (top + (bot - top) * wy) / 255.0f;
}
void hud_emu_post2(const float *src, float *dst, int w, int h, float cx_img, float cy_img, float fx_eye, float f_eye, const VrFilter *f) {
    for (int y = 0; y < h; y++) {
        float ys = post_ysrc(f, (float)y + 0.5f, cy_img, f_eye);
        float ty = ys - 0.5f, fy = floorf(ty), wy = ty - fy;      /* bilineal: texel i tiene su centro en i + 0,5 */
        int y0 = (int)fy, y1 = y0 + 1;
        y0 = y0 < 0 ? 0 : y0 > h - 1 ? h - 1 : y0; y1 = y1 < 0 ? 0 : y1 > h - 1 ? h - 1 : y1;
        float sw = post_scan(f, (float)y + 0.5f, cy_img, f_eye);
        for (int x = 0; x < w; x++) {
            float fs = post_frame(f, (float)x + 0.5f, (float)y + 0.5f, cx_img, cy_img, fx_eye, f_eye);
            for (int k = 0; k < 3; k++) {
                float a = src[((size_t)y0 * w + x) * 3 + k], b = src[((size_t)y1 * w + x) * 3 + k];
                float c = sat1(a + (b - a) * wy);
                c *= f->tint[k];
                int scan_pending = sw > 0.0f, nc = f->n_covers > VR_FILTER_COVERS ? VR_FILTER_COVERS : f->n_covers;
                if (nc > 0) {
                    for (int i = 0; i < nc; i++) {
                        if (scan_pending && i >= f->scan_after) { c = sat1(post_blend(c, f->scan_rgb[k], f->scan_abr, sw)); scan_pending = 0; }
                        c = sat1(post_blend(c, f->covers_rgb[i][k], f->covers_abr[i], 1.0f));
                    }
                } else if (f->cover_abr >= 0) {
                    c = post_blend(c, f->cover_rgb[k], f->cover_abr, 1.0f);   /* filtros construidos a mano sin covers[] (pruebas antiguas) */
                }
                if (scan_pending) c = sat1(post_blend(c, f->scan_rgb[k], f->scan_abr, sw));
                if (fs > 0.0f) {
                    c = c - fs < 0 ? 0 : c - fs;
                    float sm = sat1((fs - 0.85f) / 0.15f); sm = sm * sm * (3.0f - 2.0f * sm);   /* smoothstep(0,85; 1) */
                    c += f->frame_rgb[k] * sm;
                }
                dst[((size_t)y * w + x) * 3 + k] = sat1(c);
            }
        }
    }
}
void hud_emu_post(const float *src, float *dst, int w, int h, float cy_img, float f_eye, const VrFilter *f) {
    hud_emu_post2(src, dst, w, h, 0.5f * (float)w, cy_img, f_eye, f_eye, f);
}

/* ---- pase 3D por CPU (pantalla 3D) ---- */
typedef struct { float x, y, z, w, u, v, c[3]; } V3;   /* x,y en píxeles; w = 1/(-z) */

static void tri3d(const uint16_t *vram, HudImg *im, float *zb, const VrVtx *t, const V3 *p, uint32_t pass, int mode, int writez) {
    float area = edge(p[0].x, p[0].y, p[1].x, p[1].y, p[2].x, p[2].y);
    if (area == 0) return;
    int x0 = (int)floorf(fminf(p[0].x, fminf(p[1].x, p[2].x))), x1 = (int)ceilf(fmaxf(p[0].x, fmaxf(p[1].x, p[2].x)));
    int y0 = (int)floorf(fminf(p[0].y, fminf(p[1].y, p[2].y))), y1 = (int)ceilf(fmaxf(p[0].y, fmaxf(p[1].y, p[2].y)));
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > im->w) x1 = im->w; if (y1 > im->h) y1 = im->h;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = edge(p[1].x, p[1].y, p[2].x, p[2].y, px, py) / area, w1 = edge(p[2].x, p[2].y, p[0].x, p[0].y, px, py) / area, w2 = 1.0f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float iw = w0 * p[0].w + w1 * p[1].w + w2 * p[2].w;   /* 1/profundidad interpolada linealmente en pantalla */
            if (!(iw > 0)) continue;
            float depth = 1.0f / iw;
            size_t zi = (size_t)y * im->w + x;
            if (depth >= zb[zi]) continue;
            float b0 = w0 * p[0].w / iw, b1 = w1 * p[1].w / iw, b2 = w2 * p[2].w / iw;   /* corrección de perspectiva */
            float u = b0 * p[0].u + b1 * p[1].u + b2 * p[2].u, v = b0 * p[0].v + b1 * p[1].v + b2 * p[2].v;
            float col[3];
            for (int k = 0; k < 3; k++) col[k] = b0 * p[0].c[k] + b1 * p[1].c[k] + b2 * p[2].c[k];
            float rgb[3];
            if (!frag(vram, &t[0], u, v, col, pass, rgb)) continue;
            blend(&im->rgba[zi * 4], rgb, mode);
            if (writez) zb[zi] = depth;
        }
}

void hud_emu_run3d(const VrScene *s, uint16_t *vram, HudImg *im, float *zbuf, int zinit, const float view[16], const float tan_[4]) {
    hud_cur_states = s->states; hud_cur_nstates = s->nstates;
    const GpuDrawList *l = s->src;
    if (im && zinit) for (size_t i = 0; i < (size_t)im->w * im->h; i++) zbuf[i] = 1e30f;
    uint32_t next_ev = 0;
    for (uint32_t bi = 0; bi < s->nb; bi++) {
        const VrBatch *b = &s->b[bi];
        while (next_ev < b->ev_end && next_ev < l->nops) { hud_emu_event(l, &l->ops[next_ev], vram); next_ev++; }
        if (b->kind > 1 || !im) continue;
        int hastex = 0;
        for (uint32_t j = 0; j < b->count; j++) if (s->v[b->first + j].flags & VR_VF_TEXTURED) { hastex = 1; break; }
        int semi = b->kind == 1;
        for (uint32_t j = 0; j + 2 < b->count; j += 3) {
            const VrVtx *t = &s->v[b->first + j];
            V3 p[3];
            int ok = 1;
            for (int k = 0; k < 3; k++) {
                const float *q = t[k].pos;
                float x = view[0] * q[0] + view[4] * q[1] + view[8] * q[2] + view[12];
                float y = view[1] * q[0] + view[5] * q[1] + view[9] * q[2] + view[13];
                float z = view[2] * q[0] + view[6] * q[1] + view[10] * q[2] + view[14];
                if (!(z < -0.02f)) { ok = 0; break; }
                float tx = x / -z, ty = y / -z;
                p[k].x = (tx + tan_[0]) / (tan_[0] + tan_[1]) * (float)im->w;
                p[k].y = (tan_[2] - ty) / (tan_[2] + tan_[3]) * (float)im->h;
                p[k].z = z; p[k].w = 1.0f / -z;
                p[k].u = t[k].uv[0]; p[k].v = t[k].uv[1];
                for (int c = 0; c < 3; c++) p[k].c[c] = (float)((t[k].rgba >> (8 * c)) & 255u) / 128.0f;
            }
            if (!ok) continue;   /* el ensamblador ya recortó contra el plano cercano de la cámara del juego; aquí solo se descarta lo que cae detrás del ojo */
            if (semi) {
                tri3d(vram, im, zbuf, t, p, hastex ? 1 : 0, b->blend, 0);
                if (hastex) tri3d(vram, im, zbuf, t, p, 2, -1, 1);
            } else tri3d(vram, im, zbuf, t, p, 0, -1, 1);
        }
    }
    while (next_ev < l->nops) { hud_emu_event(l, &l->ops[next_ev], vram); next_ev++; }
}

/* Vista GL del ojo del motor (x der, y arriba, -z delante; metros) a partir de VrScene.game_view (columna-mayor: vértice de vista del juego (x der, y abajo, z adelante)
 * -> mundo VR): view = k * diag(1,-1,-1) * A^-1 con traslación -M t. Con ella y las tangentes del motor, hud_emu_run3d reproduce la imagen del motor. */
static int inv3(const float a[9], float o[9]) {
    float c00 = a[4] * a[8] - a[5] * a[7], c01 = a[5] * a[6] - a[3] * a[8], c02 = a[3] * a[7] - a[4] * a[6];
    float det = a[0] * c00 + a[1] * c01 + a[2] * c02;
    if (fabsf(det) < 1e-30f) return 0;
    float id = 1.0f / det;
    o[0] = c00 * id; o[1] = (a[2] * a[7] - a[1] * a[8]) * id; o[2] = (a[1] * a[5] - a[2] * a[4]) * id;
    o[3] = c01 * id; o[4] = (a[0] * a[8] - a[2] * a[6]) * id; o[5] = (a[2] * a[3] - a[0] * a[5]) * id;
    o[6] = c02 * id; o[7] = (a[1] * a[6] - a[0] * a[7]) * id; o[8] = (a[0] * a[4] - a[1] * a[3]) * id;
    return 1;
}
int hud_emu_game_view_gl(const VrScene *s, float view[16]) {
    float A[9] = {s->game_view[0], s->game_view[4], s->game_view[8], s->game_view[1], s->game_view[5], s->game_view[9], s->game_view[2], s->game_view[6], s->game_view[10]};   /* fila-mayor */
    float t[3] = {s->game_view[12], s->game_view[13], s->game_view[14]}, Ai[9];
    if (!inv3(A, Ai)) return 0;
    float k = s->scale > 0 ? s->scale : 0.001f, M[9];
    for (int c = 0; c < 3; c++) { M[c] = k * Ai[c]; M[3 + c] = -k * Ai[3 + c]; M[6 + c] = -k * Ai[6 + c]; }
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) view[c * 4 + r] = M[r * 3 + c];
    for (int r = 0; r < 3; r++) view[12 + r] = -(M[r * 3] * t[0] + M[r * 3 + 1] * t[1] + M[r * 3 + 2] * t[2]);
    view[3] = view[7] = view[11] = 0; view[15] = 1;
    return 1;
}
