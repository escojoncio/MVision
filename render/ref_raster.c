/* Rasterizador de referencia (ver ref_raster.h y README.md).
 *
 * Dos destinos por operación:
 *  - NATIVO: VRAM 1024x512x16 exacta (misma aritmética de 5 bits que el oráculo de gpu.c). Es de donde se
 *    leen texturas/CLUT y lo que se compara con el oráculo.
 *  - ALTA RESOLUCIÓN: rejilla de muestras M = scale*ssaa por píxel nativo, repartida en tiles de 32x32
 *    píxeles nativos que se reservan al escribir (inicializados replicando la nativa). Una muestra por
 *    celda; el píxel nativo p ocupa las muestras [p*M, p*M+M-1].
 * Convención de muestreo: M>1 (o HQ) muestrea en el CENTRO de cada muestra, ((s+0.5)/M en píxeles nativos), así un
 * rectángulo alineado cubre exactamente su huella nativa. M==1 exacto muestrea en la ESQUINA (coordenada entera),
 * como el hardware/oráculo, y reproduce su regla de arista superior-izquierda bit a bit. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "ref_raster.h"

#define TILE 32
#define VW 1024
#define VH 512

struct RefR {
    RefOpts o;
    int M, hq, scale, K;
    uint16_t (*vram)[VW];
    uint32_t *tile[VH / TILE][VW / TILE];
    int ntiles;
    uint8_t *img; size_t img_cap;
    /* caché del último tile */
    int c_tx, c_ty; uint32_t *c_p;
};

typedef struct { int M, native, hq, centre; } Tgt; /* centre: desplazamiento de muestreo (0 esquina, 1 centro) */

typedef struct {
    RefR *R; const Tgt *T; const GpuOp *op;
    int csx1, csy1, csx2, csy2; /* recorte en muestras */
    int semi, mask_chk, mask_set, abr;
} Ctx;

static const int dither_m[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

static inline uint32_t ex8(uint32_t v5) { return (v5 << 3) | (v5 >> 2); }
static inline uint32_t expand555(uint16_t c) {
    return ex8(c & 31) | (ex8((c >> 5) & 31) << 8) | (ex8((c >> 10) & 31) << 16) | ((uint32_t)(c >> 15) << 24);
}
static inline int64_t floordiv(int64_t a, int64_t b) { int64_t q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }

/* ---------------- tiles de alta resolución ---------------- */
static uint32_t *tile_get(RefR *R, int tx, int ty, int alloc) {
    uint32_t **slot = &R->tile[ty][tx];
    if (!*slot) {
        if (!alloc) return NULL;
        int M = R->M, side = TILE * M;
        uint32_t *t = malloc((size_t)side * side * sizeof *t);
        if (!t) abort();
        for (int j = 0; j < side; j++) {
            const uint16_t *src = &R->vram[ty * TILE + j / M][tx * TILE];
            uint32_t *dst = t + (size_t)j * side;
            for (int i = 0; i < side; i++) dst[i] = expand555(src[i / M]);
        }
        *slot = t; R->ntiles++;
    }
    return *slot;
}

static void tiles_free(RefR *R) {
    for (int j = 0; j < VH / TILE; j++)
        for (int i = 0; i < VW / TILE; i++) { free(R->tile[j][i]); R->tile[j][i] = NULL; }
    R->ntiles = 0; R->c_p = NULL;
}

/* puntero a la muestra (sx,sy) (en rejilla M); NULL si no hay tile y !alloc */
static inline uint32_t *hpix(RefR *R, int sx, int sy, int alloc) {
    int M = R->M, side = TILE * M;
    int tx = sx / side, ty = sy / side;
    uint32_t *t;
    if (R->c_p && R->c_tx == tx && R->c_ty == ty) t = R->c_p;
    else {
        t = tile_get(R, tx, ty, alloc);
        if (!t) return NULL;
        R->c_tx = tx; R->c_ty = ty; R->c_p = t;
    }
    return t + (size_t)(sy - ty * side) * side + (sx - tx * side);
}
static inline uint32_t hget(RefR *R, int sx, int sy) {
    uint32_t *p = hpix(R, sx, sy, 0);
    return p ? *p : expand555(R->vram[(sy / R->M) & (VH - 1)][(sx / R->M) & (VW - 1)]);
}

/* ---------------- escritura de píxel/muestra ---------------- */
static inline void rplot(Ctx *c, int sx, int sy, int r, int g, int b, int dith, int semi, int mbit) {
    if (sx < c->csx1 || sx > c->csx2 || sy < c->csy1 || sy > c->csy2) return;
    const Tgt *T = c->T;
    int bd_r, bd_g, bd_b, bmask;
    uint16_t *p16 = NULL; uint32_t *p32 = NULL;
    if (T->native) {
        p16 = &c->R->vram[sy & (VH - 1)][sx & (VW - 1)];
        bmask = *p16 >> 15;
        bd_r = *p16 & 31; bd_g = (*p16 >> 5) & 31; bd_b = (*p16 >> 10) & 31;
    } else {
        if (sy >= VH * T->M) sy -= VH * T->M; /* y&511 del hardware (E4 admite hasta 1023) */
        p32 = hpix(c->R, sx, sy, 1);
        uint32_t d = *p32;
        bmask = (d >> 24) & 1;
        bd_r = d & 255; bd_g = (d >> 8) & 255; bd_b = (d >> 16) & 255;
        if (!T->hq) { bd_r >>= 3; bd_g >>= 3; bd_b >>= 3; }
    }
    if (c->mask_chk && bmask) return;
    int nm = mbit | c->mask_set;
    if (!T->hq) { /* canal de 5 bits, idéntico al oráculo */
        if (dith) {
            int d = dither_m[(sy / T->M) & 3][(sx / T->M) & 3];
            r += d; g += d; b += d;
        }
        r = r < 0 ? 0 : r > 255 ? 255 : r;
        g = g < 0 ? 0 : g > 255 ? 255 : g;
        b = b < 0 ? 0 : b > 255 ? 255 : b;
        r >>= 3; g >>= 3; b >>= 3;
        if (semi) {
            switch (c->abr) {
            case 0: r = (bd_r + r) >> 1; g = (bd_g + g) >> 1; b = (bd_b + b) >> 1; break;
            case 1: r += bd_r; g += bd_g; b += bd_b; break;
            case 2: r = bd_r - r; g = bd_g - g; b = bd_b - b; break;
            case 3: r = bd_r + (r >> 2); g = bd_g + (g >> 2); b = bd_b + (b >> 2); break;
            }
            r = r < 0 ? 0 : r > 31 ? 31 : r;
            g = g < 0 ? 0 : g > 31 ? 31 : g;
            b = b < 0 ? 0 : b > 31 ? 31 : b;
        }
        if (p16) *p16 = (uint16_t)(r | (g << 5) | (b << 10) | (nm ? 0x8000 : 0));
        else *p32 = ex8(r) | (ex8(g) << 8) | (ex8(b) << 16) | ((uint32_t)nm << 24);
    } else { /* 8 bits por canal, sin tramado */
        r = r < 0 ? 0 : r > 255 ? 255 : r;
        g = g < 0 ? 0 : g > 255 ? 255 : g;
        b = b < 0 ? 0 : b > 255 ? 255 : b;
        if (semi) {
            switch (c->abr) {
            case 0: r = (bd_r + r) >> 1; g = (bd_g + g) >> 1; b = (bd_b + b) >> 1; break;
            case 1: r += bd_r; g += bd_g; b += bd_b; break;
            case 2: r = bd_r - r; g = bd_g - g; b = bd_b - b; break;
            case 3: r = bd_r + (r >> 2); g = bd_g + (g >> 2); b = bd_b + (b >> 2); break;
            }
            r = r < 0 ? 0 : r > 255 ? 255 : r;
            g = g < 0 ? 0 : g > 255 ? 255 : g;
            b = b < 0 ? 0 : b > 255 ? 255 : b;
        }
        *p32 = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)nm << 24);
    }
}

/* ---------------- texturas (siempre de la VRAM nativa) ---------------- */
static inline uint16_t texel(const RefR *R, const GpuOp *o, int u, int v) {
    u &= 255; v &= 255;
    u = (u & ~(o->twmx * 8)) | ((o->twox & o->twmx) * 8);
    v = (v & ~(o->twmy * 8)) | ((o->twoy & o->twmy) * 8);
    int y = (o->ty + v) & 511;
    switch (o->tp) {
    case 0: {
        uint16_t w = R->vram[y][(o->tx + (u >> 2)) & 1023];
        return R->vram[o->cly & 511][(o->clx + ((w >> ((u & 3) * 4)) & 15)) & 1023];
    }
    case 1: {
        uint16_t w = R->vram[y][(o->tx + (u >> 1)) & 1023];
        return R->vram[o->cly & 511][(o->clx + ((w >> ((u & 1) * 8)) & 255)) & 1023];
    }
    default: return R->vram[y][(o->tx + u) & 1023];
    }
}

/* Color modulado y escritura de UNA muestra texturizada. r,g,b: 0..255 en formato 8.fb (fb bits fraccionarios). */
static inline void tex_shade(Ctx *c, int sx, int sy, int u, int v, int r, int g, int b, int fb, int raw, int dith) {
    uint16_t t = texel(c->R, c->op, u, v);
    if (!t) return;
    int tr = t & 31, tg = (t >> 5) & 31, tb = (t >> 10) & 31;
    int R_, G_, B_;
    if (!c->T->hq) { /* oráculo: sin fracción (fb=0) */
        if (raw) { R_ = tr << 3; G_ = tg << 3; B_ = tb << 3; dith = 0; }
        else { R_ = (tr * r) >> 4; G_ = (tg * g) >> 4; B_ = (tb * b) >> 4; }
    } else {
        int er = (int)ex8(tr), eg = (int)ex8(tg), eb = (int)ex8(tb);
        if (raw) { R_ = er; G_ = eg; B_ = eb; }
        else { /* 128 = neutro; r tiene fb bits fraccionarios */
            R_ = (int)(((int64_t)er * r) >> (7 + fb)); G_ = (int)(((int64_t)eg * g) >> (7 + fb));
            B_ = (int)(((int64_t)eb * b) >> (7 + fb));
        }
    }
    rplot(c, sx, sy, R_, G_, B_, dith, c->semi && (t & 0x8000), t >> 15);
}

/* ---------------- geometría ---------------- */
typedef struct { int x, y; int64_t X, Y; int r, g, b, u, v; } V;

static void make_v(const GpuOp *o, int i, const Tgt *T, unsigned flags, V *d, int use_f) {
    const GpuVtx *s = &o->v[i];
    int64_t Q = 32 * T->M;
    d->x = s->x; d->y = s->y;
    if (use_f) { d->X = llroundf(s->fx * (float)Q); d->Y = llroundf(s->fy * (float)Q); }
    else { d->X = (int64_t)s->x * Q; d->Y = (int64_t)s->y * Q; }
    d->r = s->r; d->g = s->g; d->b = s->b; d->u = s->u; d->v = s->v;
    (void)flags;
}

static inline int64_t edge(const V *a, const V *b, int64_t PX, int64_t PY) {
    return (b->X - a->X) * (PY - a->Y) - (b->Y - a->Y) * (PX - a->X);
}
static inline int is_tl(const V *a, const V *b) {
    int64_t dy = b->Y - a->Y, dx = b->X - a->X;
    return dy < 0 || (dy == 0 && dx > 0);
}

static void tri(Ctx *c, V v0, V v1, V v2, int gour, int tex, int raw, int dither) {
    const Tgt *T = c->T;
    int M = T->M, soff = T->centre ? 16 : 0;
    int minx = v0.x, maxx = v0.x, miny = v0.y, maxy = v0.y;
    const V *vs[3] = {&v0, &v1, &v2};
    for (int i = 1; i < 3; i++) {
        if (vs[i]->x < minx) minx = vs[i]->x;
        if (vs[i]->x > maxx) maxx = vs[i]->x;
        if (vs[i]->y < miny) miny = vs[i]->y;
        if (vs[i]->y > maxy) maxy = vs[i]->y;
    }
    if (maxx - minx >= 1024 || maxy - miny >= 512) return; /* el hardware descarta */
    int64_t area = edge(&v0, &v1, v2.X, v2.Y);
    if (area == 0) return;
    if (area < 0) { V t = v1; v1 = v2; v2 = t; area = -area; }
    int64_t mnX = v0.X, mxX = v0.X, mnY = v0.Y, mxY = v0.Y;
    const V *w3[3] = {&v0, &v1, &v2};
    for (int i = 1; i < 3; i++) {
        if (w3[i]->X < mnX) mnX = w3[i]->X;
        if (w3[i]->X > mxX) mxX = w3[i]->X;
        if (w3[i]->Y < mnY) mnY = w3[i]->Y;
        if (w3[i]->Y > mxY) mxY = w3[i]->Y;
    }
    int sx0 = (int)floordiv(mnX - soff, 32), sx1 = (int)floordiv(mxX - soff, 32) + 1;
    int sy0 = (int)floordiv(mnY - soff, 32), sy1 = (int)floordiv(mxY - soff, 32) + 1;
    if (sx0 < c->csx1) sx0 = c->csx1;
    if (sy0 < c->csy1) sy0 = c->csy1;
    if (sx1 > c->csx2) sx1 = c->csx2;
    if (sy1 > c->csy2) sy1 = c->csy2;
    if (sx0 > sx1 || sy0 > sy1) return;
    int b0 = is_tl(&v1, &v2) ? 0 : -1, b1 = is_tl(&v2, &v0) ? 0 : -1, b2 = is_tl(&v0, &v1) ? 0 : -1;
    /* incrementos de las funciones de arista por muestra (paso = 32 unidades) */
    int64_t d0x = -(v2.Y - v1.Y) * 32, d1x = -(v0.Y - v2.Y) * 32, d2x = -(v1.Y - v0.Y) * 32;
    int64_t d0y = (v2.X - v1.X) * 32, d1y = (v0.X - v2.X) * 32, d2y = (v1.X - v0.X) * 32;
    int64_t PX0 = (int64_t)sx0 * 32 + soff, PY0 = (int64_t)sy0 * 32 + soff;
    int64_t r0 = edge(&v1, &v2, PX0, PY0), r1 = edge(&v2, &v0, PX0, PY0), r2 = edge(&v0, &v1, PX0, PY0);
    int fb = T->hq ? 8 : 0;
    int dith = dither && !T->hq;
    for (int y = sy0; y <= sy1; y++, r0 += d0y, r1 += d1y, r2 += d2y) {
        int64_t w0 = r0, w1 = r1, w2 = r2;
        for (int x = sx0; x <= sx1; x++, w0 += d0x, w1 += d1x, w2 += d2x) {
            if (w0 + b0 < 0 || w1 + b1 < 0 || w2 + b2 < 0) continue;
            int r = v0.r, g = v0.g, b = v0.b;
            if (fb) { r <<= 8; g <<= 8; b <<= 8; }
            if (gour) {
                r = (int)(((int64_t)v0.r * w0 + (int64_t)v1.r * w1 + (int64_t)v2.r * w2) * (1 << fb) / area);
                g = (int)(((int64_t)v0.g * w0 + (int64_t)v1.g * w1 + (int64_t)v2.g * w2) * (1 << fb) / area);
                b = (int)(((int64_t)v0.b * w0 + (int64_t)v1.b * w1 + (int64_t)v2.b * w2) * (1 << fb) / area);
            }
            if (tex) {
                int u = (int)(((int64_t)v0.u * w0 + (int64_t)v1.u * w1 + (int64_t)v2.u * w2) / area);
                int v = (int)(((int64_t)v0.v * w0 + (int64_t)v1.v * w1 + (int64_t)v2.v * w2) / area);
                tex_shade(c, x, y, u, v, r, g, b, fb, raw, dith);
            } else {
                rplot(c, x, y, r >> fb, g >> fb, b >> fb, dith, c->semi, 0);
            }
        }
    }
    (void)M;
}

static void do_poly(Ctx *c, unsigned flags) {
    const GpuOp *o = c->op;
    int gour = !!(o->flags & GPU_F_GOURAUD), tex = !!(o->flags & GPU_F_TEXTURED), raw = !!(o->flags & GPU_F_RAW);
    int dither = !!(o->flags & GPU_F_DITHER);
    int nv = o->nv;
    int use_f = (flags & REF_FLOAT_VTX) && !c->T->native;
    /* El flotante solo vale si es coherente con el entero (el SXY de la GTE sale de redondear/saturar el flotante):
     * NaN, infinito o un vértice saturado (|f - entero| > 1) desbordarían la aritmética de aristas. */
    for (int i = 0; i < nv; i++) {
        const GpuVtx *s = &o->v[i];
        if (!s->fvalid || !(fabsf(s->fx - (float)s->x) <= 1.0f) || !(fabsf(s->fy - (float)s->y) <= 1.0f)) use_f = 0;
    }
    V v[4];
    for (int i = 0; i < nv; i++) make_v(o, i, c->T, flags, &v[i], use_f);
    tri(c, v[0], v[1], v[2], gour, tex, raw, dither);
    if (nv == 4) tri(c, v[1], v[2], v[3], gour, tex, raw, dither);
}

static void do_rect(Ctx *c) {
    const GpuOp *o = c->op;
    int M = c->T->M;
    int tex = !!(o->flags & GPU_F_TEXTURED), raw = !!(o->flags & GPU_F_RAW);
    int flipx = !!(o->flags & GPU_F_FLIPX), flipy = !!(o->flags & GPU_F_FLIPY);
    int x0 = o->v[0].x, y0 = o->v[0].y, ww = o->rw, hh = o->rh;
    int u0 = o->v[0].u, v0 = o->v[0].v;
    int r = o->v[0].r, g = o->v[0].g, b = o->v[0].b;
    int fb = c->T->hq ? 8 : 0;
    if (fb) { r <<= 8; g <<= 8; b <<= 8; }
    for (int j = 0; j < hh; j++) {
        int y = y0 + j;
        if (y < o->cy1 || y > o->cy2) continue;
        for (int i = 0; i < ww; i++) {
            int x = x0 + i;
            if (x < o->cx1 || x > o->cx2) continue;
            int u = flipx ? u0 - i : u0 + i, v = flipy ? v0 - j : v0 + j;
            for (int sj = 0; sj < M; sj++)
                for (int si = 0; si < M; si++) {
                    int sx = x * M + si, sy = y * M + sj;
                    if (tex) tex_shade(c, sx, sy, u, v, r, g, b, fb, raw, 0);
                    else rplot(c, sx, sy, r >> fb, g >> fb, b >> fb, 0, c->semi, 0);
                }
        }
    }
}

static void do_line(Ctx *c) {
    const GpuOp *o = c->op;
    const GpuVtx *a = &o->v[0], *b = &o->v[1];
    int gour = !!(o->flags & GPU_F_GOURAUD);
    int dx = b->x - a->x, dy = b->y - a->y, adx = abs(dx), ady = abs(dy);
    if (adx >= 1024 || ady >= 512) return;
    int dith = !!(o->flags & GPU_F_DITHER);
    if (c->T->M == 1 && !c->T->hq) { /* mismo DDA que el oráculo */
        int n = adx > ady ? adx : ady;
        for (int i = 0; i <= n; i++) {
            int x = n ? a->x + (dx * i + (dx >= 0 ? n / 2 : -n / 2)) / n : a->x;
            int y = n ? a->y + (dy * i + (dy >= 0 ? n / 2 : -n / 2)) / n : a->y;
            int r = a->r, g = a->g, bb = a->b;
            if (gour && n) {
                r = a->r + (b->r - a->r) * i / n;
                g = a->g + (b->g - a->g) * i / n;
                bb = a->b + (b->b - a->b) * i / n;
            }
            rplot(c, x, y, r, g, bb, dith, c->semi, 0);
        }
        return;
    }
    /* Alta resolución: cinta de 1 píxel nativo de ancho con extremos prolongados 0,5 px (cubre el mismo
     * píxel de cada extremo que el DDA); color lineal a lo largo del eje. */
    int M = c->T->M;
    double ax = a->x + 0.5, ay = a->y + 0.5, bx = b->x + 0.5, by = b->y + 0.5;
    double len = sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
    double ux = len > 0 ? (bx - ax) / len : 1, uy = len > 0 ? (by - ay) / len : 0;
    int minx = (int)floor((fmin(ax, bx) - 1.0) * M), maxx = (int)ceil((fmax(ax, bx) + 1.0) * M);
    int miny = (int)floor((fmin(ay, by) - 1.0) * M), maxy = (int)ceil((fmax(ay, by) + 1.0) * M);
    if (minx < c->csx1) minx = c->csx1;
    if (miny < c->csy1) miny = c->csy1;
    if (maxx > c->csx2) maxx = c->csx2;
    if (maxy > c->csy2) maxy = c->csy2;
    for (int sy = miny; sy <= maxy; sy++)
        for (int sx = minx; sx <= maxx; sx++) {
            double px = (sx + 0.5) / M - ax, py = (sy + 0.5) / M - ay;
            double t = px * ux + py * uy, d = fabs(-px * uy + py * ux);
            if (d > 0.5 || t < -0.5 || t > len + 0.5) continue;
            double s = len > 0 ? t / len : 0;
            s = s < 0 ? 0 : s > 1 ? 1 : s;
            int r = a->r, g = a->g, bb = a->b;
            if (gour) {
                r = (int)(a->r + (b->r - a->r) * s + 0.5); g = (int)(a->g + (b->g - a->g) * s + 0.5);
                bb = (int)(a->b + (b->b - a->b) * s + 0.5);
            }
            rplot(c, sx, sy, r, g, bb, 0, c->semi, 0);
        }
}

/* ---------------- operaciones de VRAM ---------------- */
static void block_set(RefR *R, int x, int y, uint32_t val) {
    int M = R->M;
    for (int j = 0; j < M; j++)
        for (int i = 0; i < M; i++) *hpix(R, x * M + i, y * M + j, 1) = val;
}
static int tile_any(RefR *R, int x, int y, int w, int h) {
    if (!R->ntiles) return 0;
    /* el rectángulo da la vuelta a la VRAM (x o y &1023/&511): conservador, hay que tratarlo como tocado */
    if ((x & 1023) + w > VW || (y & 511) + h > VH) return 1;
    for (int ty = (y & 511) / TILE; ty <= ((y + h - 1) & 511) / TILE && ty < VH / TILE; ty++)
        for (int tx = (x & 1023) / TILE; tx <= ((x + w - 1) & 1023) / TILE && tx < VW / TILE; tx++)
            if (R->tile[ty][tx]) return 1;
    return 0;
}

static void op_fill(RefR *R, const GpuOp *o, int hires) {
    uint16_t c = o->fill15;
    uint32_t hv = (R->hq)
                      ? (uint32_t)o->v[0].r | ((uint32_t)o->v[0].g << 8) | ((uint32_t)o->v[0].b << 16)
                      : expand555(c);
    for (int j = 0; j < o->rh; j++)
        for (int i = 0; i < o->rw; i++) {
            int x = (o->rx + i) & (VW - 1), y = (o->ry + j) & (VH - 1);
            R->vram[y][x] = c;
            if (hires) block_set(R, x, y, hv);
        }
}

static void op_upload(RefR *R, const GpuOp *o, const GpuDrawList *l, int hires) {
    int mchk = !!(o->flags & GPU_F_MASK_CHK), mset = !!(o->flags & GPU_F_MASK_SET);
    for (uint32_t i = 0; i < o->data_n && i < (uint32_t)(o->rw * o->rh); i++) {
        int x = (o->rx + (int)(i % o->rw)) & (VW - 1), y = (o->ry + (int)(i / o->rw)) & (VH - 1);
        uint16_t *p = &R->vram[y][x];
        if (mchk && (*p & 0x8000)) continue;
        *p = l->data[o->data_off + i] | (mset ? 0x8000 : 0);
        if (hires && R->tile[y / TILE][x / TILE]) block_set(R, x, y, expand555(*p));
    }
}

static void op_vcopy(RefR *R, const GpuOp *o, int hires) {
    int mchk = !!(o->flags & GPU_F_MASK_CHK), mset = !!(o->flags & GPU_F_MASK_SET);
    int M = R->M, ww = o->rw, hh = o->rh;
    int hr = hires && (tile_any(R, o->sx, o->sy, ww, hh) || tile_any(R, o->rx, o->ry, ww, hh));
    static uint16_t line[VW];
    uint32_t *hl = hr ? malloc((size_t)ww * M * M * sizeof *hl) : NULL; /* M filas de muestras de la fila nativa */
    for (int j = 0; j < hh; j++) {
        int sy = (o->sy + j) & (VH - 1), dy = (o->ry + j) & (VH - 1);
        for (int i = 0; i < ww; i++) line[i] = R->vram[sy][(o->sx + i) & (VW - 1)];
        if (hr)
            for (int sj = 0; sj < M; sj++)
                for (int i = 0; i < ww; i++)
                    for (int si = 0; si < M; si++)
                        hl[((size_t)sj * ww + i) * M + si] = hget(R, ((o->sx + i) & (VW - 1)) * M + si, sy * M + sj);
        for (int i = 0; i < ww; i++) {
            int dx = (o->rx + i) & (VW - 1);
            uint16_t *p = &R->vram[dy][dx];
            if (mchk && (*p & 0x8000)) continue;
            *p = line[i] | (mset ? 0x8000 : 0);
            if (hr)
                for (int sj = 0; sj < M; sj++)
                    for (int si = 0; si < M; si++) {
                        uint32_t v = hl[((size_t)sj * ww + i) * M + si];
                        if (mset) v |= 1u << 24;
                        *hpix(R, dx * M + si, dy * M + sj, 1) = v;
                    }
        }
    }
    free(hl);
}

/* ---------------- API ---------------- */
RefR *ref_create(const RefOpts *o) {
    RefR *R = calloc(1, sizeof *R);
    R->o = *o;
    R->scale = o->scale < 1 ? 1 : o->scale > 8 ? 8 : o->scale;
    R->K = o->ssaa < 1 ? 1 : o->ssaa > 4 ? 4 : o->ssaa;
    R->M = R->scale * R->K;
    R->hq = R->M > 1 ? !(o->flags & REF_EXACT5) : !!(o->flags & REF_HQ);
    R->vram = calloc(VH, sizeof *R->vram);
    return R;
}

void ref_destroy(RefR *R) {
    if (!R) return;
    tiles_free(R);
    free(R->vram); free(R->img); free(R);
}

void ref_apply(RefR *R, const GpuDrawList *l, int hires) {
    if (l->vram_init) { memcpy(R->vram, l->vram_init, (size_t)VW * VH * 2); tiles_free(R); }
    if (!hires && R->ntiles) tiles_free(R);
    Tgt TN = {1, 1, 0, 0};
    Tgt TH = {R->M, 0, R->hq, (R->M > 1 || R->hq) ? 1 : 0};
    for (uint32_t k = 0; k < l->nops; k++) {
        const GpuOp *o = &l->ops[k];
        switch (o->type) {
        case GPU_OP_POLY: case GPU_OP_RECT: case GPU_OP_LINE: {
            for (int pass = hires ? 0 : 1; pass < 2; pass++) { /* alta resolución primero, luego nativo */
                const Tgt *T = pass ? &TN : &TH;
                Ctx c = {R, T, o, 0, 0, 0, 0, 0, 0, 0, 0};
                int M = T->M;
                c.csx1 = o->cx1 * M; c.csy1 = o->cy1 * M; c.csx2 = (o->cx2 + 1) * M - 1; c.csy2 = (o->cy2 + 1) * M - 1;
                c.semi = !!(o->flags & GPU_F_SEMI); c.mask_chk = !!(o->flags & GPU_F_MASK_CHK);
                c.mask_set = !!(o->flags & GPU_F_MASK_SET); c.abr = o->abr;
                if (o->type == GPU_OP_POLY) do_poly(&c, R->o.flags);
                else if (o->type == GPU_OP_RECT) do_rect(&c);
                else do_line(&c);
            }
            break;
        }
        case GPU_OP_FILL: op_fill(R, o, hires); break;
        case GPU_OP_UPLOAD: op_upload(R, o, l, hires); break;
        case GPU_OP_VCOPY: op_vcopy(R, o, hires); break;
        default: break; /* DOWNLOAD: sin efecto */
        }
    }
}

const uint16_t *ref_vram(const RefR *R) { return &R->vram[0][0]; }
int ref_tiles(const RefR *R) { return R->ntiles; }

const uint8_t *ref_present(RefR *R, const GpuDisplay *d, int *w, int *h) {
    int S = R->scale, K = R->K, M = R->M;
    int W = d->w * S, H = d->h * S;
    size_t need = (size_t)W * H * 3;
    if (need > R->img_cap) { R->img = realloc(R->img, need); R->img_cap = need; }
    int d24 = d->depth24;
    for (int oy = 0; oy < H; oy++) {
        uint8_t *row = R->img + (size_t)oy * W * 3;
        int py = (d->y + oy / S) & (VH - 1);
        for (int ox = 0; ox < W; ox++) {
            uint8_t *o = row + ox * 3;
            int px = ox / S;
            if (d24) {
                const uint8_t *ln = (const uint8_t *)R->vram[py];
                for (int k = 0; k < 3; k++) o[k] = ln[((d->x * 2) + px * 3 + k) & 2047];
                continue;
            }
            int hx = ((d->x + px) & (VW - 1)) * M + (ox % S) * K, hy = py * M + (oy % S) * K;
            int sr = 0, sg = 0, sb = 0;
            for (int a = 0; a < K; a++)
                for (int b = 0; b < K; b++) {
                    uint32_t v = hget(R, hx + b, hy + a);
                    sr += v & 255; sg += (v >> 8) & 255; sb += (v >> 16) & 255;
                }
            int n = K * K;
            o[0] = (uint8_t)((sr + n / 2) / n); o[1] = (uint8_t)((sg + n / 2) / n); o[2] = (uint8_t)((sb + n / 2) / n);
        }
    }
    *w = W; *h = H;
    return R->img;
}
