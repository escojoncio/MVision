/* eyeshot: sonda de pruebas (no forma parte de la app). Rasteriza por software la escena VR (VrScene en el espacio recentrado del usuario,
 * la que consume el renderer) desde una pose de cabeza dada, aplicando los eventos de VRAM en orden: sirve para ver "lo que ve el usuario"
 * (cuerpo y brazos al mirar abajo) en el banco headless. Reutiliza el rasterizador de scene_dump. Uso desde bodyrun (--shot). */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu.h"
#include "vr_scene.h"
#include "eyeshot.h"

static uint32_t crc_tab[256];
static uint32_t crc32u(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_tab[1])
        for (uint32_t i = 0; i < 256; i++) { uint32_t x = i; for (int k = 0; k < 8; k++) x = x & 1 ? 0xEDB88320u ^ (x >> 1) : x >> 1; crc_tab[i] = x; }
    c = ~c;
    while (n--) c = crc_tab[(c ^ *p++) & 255] ^ (c >> 8);
    return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE *f, const char *t, const uint8_t *d, uint32_t n) {
    uint8_t h[8]; be32(h, n); memcpy(h + 4, t, 4);
    fwrite(h, 1, 8, f); if (n) fwrite(d, 1, n, f);
    uint32_t c = crc32u(0, (const uint8_t *)t, 4); c = crc32u(c, d, n);
    uint8_t e[4]; be32(e, c); fwrite(e, 1, 4, f);
}
static void write_png(const char *path, const uint8_t *rgb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ih[13]; be32(ih, w); be32(ih + 4, h); ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0;
    chunk(f, "IHDR", ih, 13);
    size_t rawn = (size_t)h * (1 + (size_t)w * 3);
    uint8_t *raw = malloc(rawn);
    for (int y = 0; y < h; y++) { raw[(size_t)y * (1 + w * 3)] = 0; memcpy(raw + (size_t)y * (1 + w * 3) + 1, rgb + (size_t)y * w * 3, (size_t)w * 3); }
    size_t nb = (rawn + 65534) / 65535, zn = 2 + rawn + nb * 5 + 4;
    uint8_t *z = malloc(zn), *q = z;
    *q++ = 0x78; *q++ = 1;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < rawn; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < rawn; off += 65535) {
        size_t n = rawn - off < 65535 ? rawn - off : 65535;
        *q++ = off + n >= rawn; *q++ = n; *q++ = n >> 8; *q++ = ~n; *q++ = ~n >> 8;
        memcpy(q, raw + off, n); q += n;
    }
    be32(q, b << 16 | a); q += 4;
    chunk(f, "IDAT", z, (uint32_t)(q - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f); free(raw); free(z);
}

/* ---- VRAM local con aplicación de eventos en orden ---- */
static uint16_t vram[512 * 1024];
static void ev_apply(const GpuDrawList *l, const GpuOp *o) {
    switch (o->type) {
    case GPU_OP_FILL:
        for (int y = 0; y < o->rh; y++)
            for (int x = 0; x < o->rw; x++) vram[((o->ry + y) & 511) * 1024 + ((o->rx + x) & 1023)] = o->fill15;
        break;
    case GPU_OP_VCOPY: {
        static uint16_t tmp[512 * 1024];
        for (int y = 0; y < o->rh; y++)
            for (int x = 0; x < o->rw; x++) tmp[y * 1024 + x] = vram[((o->sy + y) & 511) * 1024 + ((o->sx + x) & 1023)];
        for (int y = 0; y < o->rh; y++)
            for (int x = 0; x < o->rw; x++) vram[((o->ry + y) & 511) * 1024 + ((o->rx + x) & 1023)] = tmp[y * 1024 + x];
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

/* ---- raster por software de la escena 3D ---- */
typedef struct { float e[3], r[3], u[3], f[3], focal, cx, cy; int W, H; } Cam;
typedef struct { uint8_t *rgb; float *zb; int W, H; } Img;
static Img img_new(int W, int H) {
    Img i = {calloc((size_t)W * H * 3, 1), malloc((size_t)W * H * sizeof(float)), W, H};
    for (size_t k = 0; k < (size_t)W * H; k++) { i.zb[k] = 0; i.rgb[k * 3] = 24; i.rgb[k * 3 + 1] = 24; i.rgb[k * 3 + 2] = 32; }
    return i;
}
static float dot3(const float *a, const float *b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
static void norm3(float *a) { float l = sqrtf(dot3(a, a)); if (l > 0) for (int i = 0; i < 3; i++) a[i] /= l; }
static void cross3(const float *a, const float *b, float *o) { o[0] = a[1]*b[2]-a[2]*b[1]; o[1] = a[2]*b[0]-a[0]*b[2]; o[2] = a[0]*b[1]-a[1]*b[0]; }
typedef struct { float x, y, z, u, v, r, g, b; } CV;

static uint32_t tex_sample(const VrVtx *v0, const VrState *st, float u, float vv, int *transparent, int *semi_texel) {
    int tx = (v0->tpage & 15) * 64, ty = ((v0->tpage >> 4) & 1) * 256, tp = (v0->tpage >> 7) & 3;
    int iu = (int)floorf(u) & 255, iv = (int)floorf(vv) & 255;
    if (st && (st->twmx | st->twmy)) {
        iu = (iu & ~(st->twmx * 8)) | ((st->twox & st->twmx) * 8);
        iv = (iv & ~(st->twmy * 8)) | ((st->twoy & st->twmy) * 8);
    }
    uint16_t t;
    if (tp == 0) {
        uint16_t w = vram[((ty + iv) & 511) * 1024 + ((tx + iu / 4) & 1023)];
        int idx = (w >> ((iu & 3) * 4)) & 15;
        t = vram[(((v0->clut >> 6) & 511)) * 1024 + (((v0->clut & 63) * 16 + idx) & 1023)];
    } else if (tp == 1) {
        uint16_t w = vram[((ty + iv) & 511) * 1024 + ((tx + iu / 2) & 1023)];
        int idx = (w >> ((iu & 1) * 8)) & 255;
        t = vram[(((v0->clut >> 6) & 511)) * 1024 + (((v0->clut & 63) * 16 + idx) & 1023)];
    } else {
        t = vram[((ty + iv) & 511) * 1024 + ((tx + iu) & 1023)];
    }
    *transparent = (t == 0);
    *semi_texel = (t >> 15) & 1;
    return (uint32_t)((t & 31) << 3) | (uint32_t)(((t >> 5) & 31) << 3) << 8 | (uint32_t)(((t >> 10) & 31) << 3) << 16;
}

static void raster_tri(Img *im, const Cam *c, const VrVtx *v, const VrState *st, int semi, int abr) {
    CV a[3];
    for (int i = 0; i < 3; i++) {
        float d[3] = {v[i].pos[0] - c->e[0], v[i].pos[1] - c->e[1], v[i].pos[2] - c->e[2]};
        a[i] = (CV){dot3(d, c->r), dot3(d, c->u), dot3(d, c->f), v[i].uv[0], v[i].uv[1],
                    (float)(v[i].rgba & 255), (float)((v[i].rgba >> 8) & 255), (float)((v[i].rgba >> 16) & 255)};
    }
    /* recorte contra z = near */
    const float nz = 0.05f;
    CV poly[4]; int np = 0;
    for (int i = 0; i < 3; i++) {
        CV p = a[i], q = a[(i + 1) % 3];
        int pi_ = p.z >= nz, qi = q.z >= nz;
        if (pi_) poly[np++] = p;
        if (pi_ != qi) {
            float t = (nz - p.z) / (q.z - p.z);
            CV m;
            float *pm = &m.x; const float *pp = &p.x, *pq = &q.x;
            for (int k = 0; k < 8; k++) pm[k] = pp[k] + (pq[k] - pp[k]) * t;
            m.z = nz;
            poly[np++] = m;
        }
    }
    if (np < 3) return;
    const VrVtx *v0 = &v[0];
    int textured = (v0->flags & GPU_F_TEXTURED) != 0, raw = (v0->flags & GPU_F_RAW) != 0;
    for (int tri = 0; tri + 2 < np; tri++) {
        const CV *p[3] = {&poly[0], &poly[tri + 1], &poly[tri + 2]};
        float sx[3], sy[3], iw[3];
        for (int i = 0; i < 3; i++) { iw[i] = 1.0f / p[i]->z; sx[i] = c->cx + c->focal * p[i]->x * iw[i]; sy[i] = c->cy - c->focal * p[i]->y * iw[i]; }
        float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
        if (fabsf(area) < 1e-6f) continue;
        int x0 = (int)fmaxf(0, floorf(fminf(sx[0], fminf(sx[1], sx[2])))), x1 = (int)fminf(im->W - 1, ceilf(fmaxf(sx[0], fmaxf(sx[1], sx[2]))));
        int y0 = (int)fmaxf(0, floorf(fminf(sy[0], fminf(sy[1], sy[2])))), y1 = (int)fminf(im->H - 1, ceilf(fmaxf(sy[0], fmaxf(sy[1], sy[2]))));
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                float px = x + 0.5f, py = y + 0.5f;
                float w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
                float w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
                float w2 = 1.0f - w0 - w1;
                if (w0 < -1e-5f || w1 < -1e-5f || w2 < -1e-5f) continue;
                float ww = w0 * iw[0] + w1 * iw[1] + w2 * iw[2];
                if (ww <= 0) continue;
                size_t pix = (size_t)y * im->W + x;
                if (ww <= im->zb[pix]) continue;
                float b0 = w0 * iw[0] / ww, b1 = w1 * iw[1] / ww, b2 = w2 * iw[2] / ww;
                float cr = b0 * p[0]->r + b1 * p[1]->r + b2 * p[2]->r, cg = b0 * p[0]->g + b1 * p[1]->g + b2 * p[2]->g,
                      cb = b0 * p[0]->b + b1 * p[1]->b + b2 * p[2]->b;
                float rr = cr, gg = cg, bb = cb;
                int is_semi = semi;
                if (textured) {
                    int tr, st_;
                    float u = b0 * p[0]->u + b1 * p[1]->u + b2 * p[2]->u, vv = b0 * p[0]->v + b1 * p[1]->v + b2 * p[2]->v;
                    uint32_t t = tex_sample(v0, st, u - 0.0f, vv, &tr, &st_);
                    if (tr) continue;
                    float tr_ = (float)(t & 255), tg = (float)((t >> 8) & 255), tb = (float)((t >> 16) & 255);
                    if (raw) { rr = tr_; gg = tg; bb = tb; }
                    else { rr = fminf(255, tr_ * cr / 128); gg = fminf(255, tg * cg / 128); bb = fminf(255, tb * cb / 128); }
                    is_semi = semi && st_;
                }
                uint8_t *o = &im->rgb[pix * 3];
                if (is_semi) {
                    float f[3] = {rr, gg, bb};
                    for (int k = 0; k < 3; k++) {
                        float bk = o[k], res;
                        switch (abr) { case 0: res = 0.5f * bk + 0.5f * f[k]; break; case 1: res = bk + f[k]; break;
                                       case 2: res = bk - f[k]; break; default: res = bk + 0.25f * f[k]; }
                        o[k] = (uint8_t)fminf(255, fmaxf(0, res));
                    }
                } else {
                    o[0] = (uint8_t)fminf(255, rr); o[1] = (uint8_t)fminf(255, gg); o[2] = (uint8_t)fminf(255, bb);
                    im->zb[pix] = ww;
                }
            }
    }
}

/* Dibuja los batches 3D de la escena con eventos de VRAM en orden. vram0 = VRAM al inicio de la lista. */
static void render_scene(const VrScene *s, const uint16_t *vram0, const Cam *c, Img *im) {
    memcpy(vram, vram0, sizeof vram);
    uint32_t applied = 0;
    const GpuDrawList *l = s->src;
    for (uint32_t bi = 0; bi < s->nb; bi++) {
        const VrBatch *b = &s->b[bi];
        for (; applied < b->ev_end && applied < l->nops; applied++) ev_apply(l, &l->ops[applied]);
        if (b->kind == 2) continue;
        for (uint32_t i = 0; i < b->count; i += 3) {
            const VrVtx *v = &s->v[b->first + i];
            raster_tri(im, c, v, vr_scene_state(s, v[0].pad), b->kind == 1, b->blend & 3);
        }
    }
}

/* ---- API ---- */
static uint16_t vram_start[512 * 1024];   /* VRAM al inicio de la última escena */
static uint16_t vram_now[512 * 1024];     /* VRAM tras aplicar todos los eventos */
static int have_vram;

void eyeshot_begin(const VrScene *s) {   /* llamar con cada escena nueva, en orden, ANTES de eyeshot_render de esa escena */
    const GpuDrawList *l = s->src;
    if (l && l->vram_init) { memcpy(vram_now, l->vram_init, sizeof vram_now); have_vram = 1; }
    memcpy(vram_start, vram_now, sizeof vram_start);
}
void eyeshot_end(const VrScene *s) {     /* aplica los eventos de la escena a la VRAM acumulada */
    const GpuDrawList *l = s->src;
    if (!l) return;
    memcpy(vram, vram_now, sizeof vram);
    for (uint32_t i = 0; i < l->nops; i++) ev_apply(l, &l->ops[i]);
    memcpy(vram_now, vram, sizeof vram);
}
/* Dibuja desde la pose `head` (columnas, formato mv_vr_get_head) con campo horizontal hfov y escribe un PPM. Devuelve 0 si va bien. */
int eyeshot_render(const VrScene *s, const float head[16], int W, int H, float hfov_deg, const char *ppm_path) {
    if (!have_vram && !(s->src && s->src->vram_init)) return -1;
    Cam c;
    memset(&c, 0, sizeof c);
    for (int i = 0; i < 3; i++) { c.e[i] = head[12 + i]; c.r[i] = head[i]; c.u[i] = head[4 + i]; c.f[i] = -head[8 + i]; }
    c.W = W; c.H = H; c.cx = W * 0.5f; c.cy = H * 0.5f; c.focal = (W * 0.5f) / tanf(hfov_deg * 0.5f * 3.14159265f / 180.0f);
    Img im = img_new(W, H);
    render_scene(s, vram_start, &c, &im);
    FILE *f = fopen(ppm_path, "wb");
    if (!f) { free(im.rgb); free(im.zb); return -2; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    fwrite(im.rgb, 1, (size_t)W * H * 3, f);
    fclose(f);
    free(im.rgb); free(im.zb);
    return 0;
}
