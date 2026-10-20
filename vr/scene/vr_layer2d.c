/* vr_layer2d: ver vr_layer2d.h. Con escala 1 la aritmética de píxel, texel, triángulo, rect y línea es la de runtime/gpu.c (oráculo) tal cual: cualquier cambio
 * ahí debe pasar test_layer2d.c (compara con gpu.c bit a bit). Con escala > 1 (alta resolución) el relleno sigue la misma regla de arista pero muestreando en el
 * centro de cada submuestra, con color de 8 bits (sin tramado) y muestreo de textura filtrado (original / nítido / suave) sobre los texels de VRAM.
 * El destino no es la VRAM sino una capa con transmitancia (ver el .h). */
#include "vr_layer2d.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define VW 1024
#define VH 512

struct VrLayer2D {
    uint16_t vram[VH][VW];   /* espejo: solo eventos de VRAM (subidas, copias, rellenos) */
    int have_vram;
    const uint16_t *vend;        /* VRAM real al cerrar la lista (solo durante vl2d_run) y mapa de bloques 16x16 de VRAM dibujados por la lista: sus texels se leen de ahí */
    uint8_t drawn[32 * 64];
    VrL2Info info;
    int pad, aw;                 /* ventana visible dentro de la capa (pantalla completa): columnas [pad, pad+aw) de w, el resto negro */
    uint8_t *natm; size_t natm_cap;   /* bit 15 (máscara) de la VRAM mostrada, por píxel nativo */
    int scale, smooth;
    VrLayerPx *px; size_t px_cap;
    uint8_t *rgba; size_t rgba_cap;
    int w, h, ox, oy, valid;     /* zona mostrada en píxeles nativos y su origen en VRAM */
    int LW, LH;                  /* tamaño de la salida RGBA: w*scale, h*scale */
    int R, PW, PH;               /* escala de RASTERIZADO (1 en modo original: exacto de 5 bits y replicado; = scale en los demás) y tamaño de px: w*R, h*R */
    int cur_scale;               /* clave (R, escala, modo) con la que están inicializados px/rgba */
    int dx0, dy0, dx1, dy1;      /* rectángulo sucio (muestras, [x0,x1) [y0,y1)) de la lista anterior; vacío si dx1 <= dx0 */
    uint16_t tc[65536]; uint32_t ts[65536]; uint32_t stamp;   /* caché de texels de la primitiva en curso */
    uint32_t qc[262144]; uint32_t qs[262144];                 /* caché del modo nítido por cuarto de texel: texel que cubre | color corregido << 16 */
    uint8_t *nat; size_t nat_cap;                             /* imagen nativa descodificada de la zona mostrada (fondo de la pantalla de cine) */
};

VrLayer2D *vl2d_new(void) {
    VrLayer2D *L = (VrLayer2D *)calloc(1, sizeof(VrLayer2D));
    if (L) { L->scale = 4; L->smooth = VL_SMOOTH_ORIGINAL; L->R = 1; L->PW = 1; L->PH = 1; L->LW = 1; L->LH = 1; }
    return L;
}
void vl2d_free(VrLayer2D *L) { if (!L) return; free(L->natm); free(L->px); free(L->rgba); free(L->nat); free(L); }
void vl2d_reset(VrLayer2D *L) { if (!L) return; L->have_vram = 0; L->valid = 0; }
void vl2d_set_options(VrLayer2D *L, int scale, int smooth) {
    if (!L) return;
    L->scale = scale < 1 ? 1 : scale > 6 ? 6 : scale;
    L->smooth = smooth < 0 ? 0 : smooth > 2 ? 2 : smooth;
}

static const int dither_m[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};
static inline int ex8(int v5) { return (v5 << 3) | (v5 >> 2); }

/* ---------------- contexto de dibujo de una primitiva ---------------- */
typedef struct {
    VrLayer2D *L;
    const GpuOp *o;
    int S, hq, smooth, cmax;
    int cx1, cy1, cx2, cy2;       /* recorte en muestras (inclusivo) */
    int semi, mask_chk, mask_set, abr;
    int fixed;                    /* primitiva de realimentación: un solo texel (op->fbt) */
    int umin, umax, vmin, vmax;   /* caja de texels de la primitiva (el filtrado no sale de ella: sin sangrado entre sprites del atlas) */
} Ctx;

/* ---------------- escritura de muestra ---------------- */
/* sx, sy: muestra en VRAM (x*S+si). r,g,b: 0..255 (escala 1: valor de 8 bits previo al canal de 5; escala > 1: color final). */
static inline void lplot(Ctx *c, int sx, int sy, int r, int g, int b, int dith, int semi, int mbit) {
    if (sx < c->cx1 || sx > c->cx2 || sy < c->cy1 || sy > c->cy2) return;
    VrLayer2D *L = c->L;
    int S = c->S;
    int mx = VW * S - 1, my = VH * S - 1;
    int lx = ((sx & mx) - ((L->ox * S) & mx)) & mx;
    int ly = ((sy & my) - ((L->oy * S) & my)) & my;
    if (lx >= L->PW || ly >= L->PH || lx < L->pad * S || lx >= (L->pad + L->aw) * S) return;
    VrLayerPx *p = &L->px[(size_t)ly * L->PW + lx];
    if (c->mask_chk && p->m) return;
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    if (!c->hq) {
        if (dith) {
            int d = dither_m[sy & 3][sx & 3];
            r += d; g += d; b += d;
            r = r < 0 ? 0 : r > 255 ? 255 : r;
            g = g < 0 ? 0 : g > 255 ? 255 : g;
            b = b < 0 ? 0 : b > 255 ? 255 : b;
        }
        r >>= 3; g >>= 3; b >>= 3;
    }
    int f[3] = {r, g, b}, cmax = c->cmax;
    if (!semi) {
        for (int k = 0; k < 3; k++) { p->c[k] = (uint8_t)f[k]; p->d[k] = 0; }
        p->t = 0;
    } else {
        switch (c->abr) {
        case 0:
            for (int k = 0; k < 3; k++) { p->c[k] = (uint8_t)((p->c[k] + f[k]) >> 1); p->d[k] = (uint16_t)(p->d[k] >> 1); }
            p->t = (uint16_t)(p->t >> 1);
            break;
        case 1:
            for (int k = 0; k < 3; k++) { int v = p->c[k] + f[k]; p->c[k] = (uint8_t)(v > cmax ? cmax : v); }
            break;
        case 2:
            for (int k = 0; k < 3; k++) {
                if (p->t == 0) { int v = p->c[k] - f[k]; p->c[k] = (uint8_t)(v < 0 ? 0 : v); }   /* opaco: exacto */
                else { unsigned v = (unsigned)p->d[k] + (unsigned)f[k] * 4u; p->d[k] = (uint16_t)(v > 60000u ? 60000u : v); }
            }
            break;
        default:
            for (int k = 0; k < 3; k++) { int v = p->c[k] + (f[k] >> 2); p->c[k] = (uint8_t)(v > cmax ? cmax : v); }
            break;
        }
        if (p->t == 0) {   /* ya opaco (p. ej. tras varias mezclas B/2): la resta pendiente se aplica al color */
            for (int k = 0; k < 3; k++) {
                int v = p->c[k] - (int)((p->d[k] + 2) / 4);
                p->c[k] = (uint8_t)(v < 0 ? 0 : v); p->d[k] = 0;
            }
        }
    }
    p->m = (uint8_t)((mbit | c->mask_set) ? 1 : 0);
}

/* ---------------- texturas (gpu.c texel + caché) ---------------- */
/* Texel de VRAM: del espejo, o de la VRAM al cerrar la lista si ese bloque lo dibujó una primitiva de la lista (render a textura) */
static inline uint16_t vget(const VrLayer2D *L, int x, int y) {
    if (L->vend && L->drawn[(y >> 4) * 64 + (x >> 4)]) return L->vend[y * VW + x];
    return L->vram[y][x];
}

static inline uint16_t texel_raw(const VrLayer2D *L, const GpuOp *o, int u, int v) {
    u &= 255; v &= 255;
    u = (u & ~(o->twmx * 8)) | ((o->twox & o->twmx) * 8);
    v = (v & ~(o->twmy * 8)) | ((o->twoy & o->twmy) * 8);
    int y = (o->ty + v) & 511;
    switch (o->tp) {
    case 0: {
        uint16_t w = vget(L, (o->tx + (u >> 2)) & 1023, y);
        return vget(L, (o->clx + ((w >> ((u & 3) * 4)) & 15)) & 1023, o->cly & 511);
    }
    case 1: {
        uint16_t w = vget(L, (o->tx + (u >> 1)) & 1023, y);
        return vget(L, (o->clx + ((w >> ((u & 1) * 8)) & 255)) & 1023, o->cly & 511);
    }
    default:
        return vget(L, (o->tx + u) & 1023, y);
    }
}

static inline uint16_t texel(Ctx *c, int u, int v) {
    VrLayer2D *L = c->L;
    unsigned i = (unsigned)(((v & 255) << 8) | (u & 255));
    if (L->ts[i] == L->stamp) return L->tc[i];
    uint16_t t = texel_raw(L, c->o, u, v);
    L->ts[i] = L->stamp; L->tc[i] = t;
    return t;
}
static inline uint16_t texel_cl(Ctx *c, int u, int v) {   /* limitado a la caja de la primitiva */
    u = u < c->umin ? c->umin : u > c->umax ? c->umax : u;
    v = v < c->vmin ? c->vmin : v > c->vmax ? c->vmax : v;
    return texel(c, u, v);
}

/* texel exacto (escala 1) */
static inline void tex_plot(Ctx *c, int sx, int sy, int u, int v, int r, int g, int b, int raw, int semi, int dith) {
    uint16_t t = c->fixed ? c->o->fbt : texel(c, u, v);
    if (!t) return;
    int tr = t & 31, tg = (t >> 5) & 31, tb = (t >> 10) & 31;
    int R, G, B;
    if (raw) { R = tr << 3; G = tg << 3; B = tb << 3; dith = 0; }
    else { R = (tr * r) >> 4; G = (tg * g) >> 4; B = (tb * b) >> 4; }
    lplot(c, sx, sy, R, G, B, dith, semi && (t & 0x8000), t >> 15);
}

/* texel filtrado (alta resolución): u,v continuos (el texel i cubre [i, i+1)), r,g,b en escala 128 = neutro */
static void tex_plot_hq(Ctx *c, int sx, int sy, double u, double v, double r, double g, double b, int raw, int semi) {
    uint16_t tn;
    float er, eg, eb;
    if (c->fixed) {
        tn = c->o->fbt;
        er = (float)ex8(tn & 31); eg = (float)ex8((tn >> 5) & 31); eb = (float)ex8((tn >> 10) & 31);
    } else {
        int iu = (int)floor(u), iv = (int)floor(v);
        double fu = u - iu, fv = v - iv;
        int mode = c->smooth;
        if (mode == VL_SMOOTH_ORIGINAL) {
            tn = texel_cl(c, iu, iv);
            if (!tn) return;
            er = (float)ex8(tn & 31); eg = (float)ex8((tn >> 5) & 31); eb = (float)ex8((tn >> 10) & 31);
        } else if (mode == VL_SMOOTH_SHARP) {
            /* píxel art: corrección de diagonales tipo EPX/Scale2x sobre el cuarto de texel que cae la muestra; el color es el de un texel real */
            int qx = fu >= 0.5, qy = fv >= 0.5;
            int cu = iu < c->umin ? c->umin : iu > c->umax ? c->umax : iu, cv = iv < c->vmin ? c->vmin : iv > c->vmax ? c->vmax : iv;
            unsigned qi = (unsigned)((((cv & 255) << 8 | (cu & 255)) << 2) | (qy << 1) | qx);
            uint32_t q;
            if (c->L->qs[qi] == c->L->stamp) q = c->L->qc[qi];
            else {
                uint16_t P = texel_cl(c, iu, iv), N = texel_cl(c, iu, iv - 1), Sx = texel_cl(c, iu, iv + 1), W = texel_cl(c, iu - 1, iv), E = texel_cl(c, iu + 1, iv);
                uint16_t R = P;
                if (!qy && !qx) { if (W == N && N != E && W != Sx) R = W; }
                else if (!qy && qx) { if (N == E && N != W && E != Sx) R = E; }
                else if (qy && !qx) { if (W == Sx && W != N && Sx != E) R = W; }
                else { if (Sx == E && Sx != W && E != N) R = E; }
                q = (uint32_t)(P ? P : R) | (uint32_t)R << 16;   /* bit 15 y transparencia: los del texel que cubre la muestra (o, si es transparente y un vecino lo rellena, los del vecino) */
                c->L->qs[qi] = c->L->stamp; c->L->qc[qi] = q;
            }
            uint16_t R = (uint16_t)(q >> 16);
            tn = (uint16_t)q;
            if (!R) return;
            er = (float)ex8(R & 31); eg = (float)ex8((R >> 5) & 31); eb = (float)ex8((R >> 10) & 31);
        } else {
            double x = u - 0.5, y = v - 0.5;
            int u0 = (int)floor(x), v0 = (int)floor(y);
            double wx = x - u0, wy = y - v0;
            uint16_t t4[4] = {texel_cl(c, u0, v0), texel_cl(c, u0 + 1, v0), texel_cl(c, u0, v0 + 1), texel_cl(c, u0 + 1, v0 + 1)};
            double w4[4] = {(1 - wx) * (1 - wy), wx * (1 - wy), (1 - wx) * wy, wx * wy};
            double acc[3] = {0, 0, 0}, cov = 0;
            for (int i = 0; i < 4; i++) {
                if (!t4[i]) continue;
                acc[0] += ex8(t4[i] & 31) * w4[i]; acc[1] += ex8((t4[i] >> 5) & 31) * w4[i]; acc[2] += ex8((t4[i] >> 10) & 31) * w4[i];
                cov += w4[i];
            }
            if (cov < 0.5) return;
            er = (float)(acc[0] / cov); eg = (float)(acc[1] / cov); eb = (float)(acc[2] / cov);
            tn = t4[(wy >= 0.5 ? 2 : 0) + (wx >= 0.5 ? 1 : 0)];
            if (!tn) return;
        }
    }
    int R, G, B;
    if (raw) { R = (int)(er + 0.5f); G = (int)(eg + 0.5f); B = (int)(eb + 0.5f); }
    else { R = (int)(er * r / 128.0 + 0.5); G = (int)(eg * g / 128.0 + 0.5); B = (int)(eb * b / 128.0 + 0.5); }
    lplot(c, sx, sy, R, G, B, 0, semi && (tn & 0x8000), tn >> 15);
}

/* ---------------- triángulos ---------------- */
typedef struct { int x, y; int64_t X, Y; int r, g, b, u, v; } V;

static inline int64_t edge(const V *a, const V *b, int64_t PX, int64_t PY) {
    return (b->X - a->X) * (PY - a->Y) - (b->Y - a->Y) * (PX - a->X);
}
static inline int is_tl(const V *a, const V *b) {
    int64_t dy = b->Y - a->Y, dx = b->X - a->X;
    return dy < 0 || (dy == 0 && dx > 0);
}
static inline int64_t floordiv(int64_t a, int64_t b) { int64_t q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }

/* Mismo recorrido que gpu.c draw_tri en escala 1 (muestra = esquina del píxel); con escala > 1 la muestra es el centro de la submuestra. */
static void tri(Ctx *c, V v0, V v1, V v2, int gour, int tex, int raw, int dither) {
    int S = c->S, soff = c->hq ? 16 : 0;
    int minx = v0.x, maxx = v0.x, miny = v0.y, maxy = v0.y;
    const V *vs[3] = {&v0, &v1, &v2};
    for (int i = 1; i < 3; i++) {
        if (vs[i]->x < minx) minx = vs[i]->x;
        if (vs[i]->x > maxx) maxx = vs[i]->x;
        if (vs[i]->y < miny) miny = vs[i]->y;
        if (vs[i]->y > maxy) maxy = vs[i]->y;
    }
    if (maxx - minx >= 1024 || maxy - miny >= 512) return;   /* el hardware descarta */
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
    if (sx0 < c->cx1) sx0 = c->cx1;
    if (sy0 < c->cy1) sy0 = c->cy1;
    if (sx1 > c->cx2) sx1 = c->cx2;
    if (sy1 > c->cy2) sy1 = c->cy2;
    if (sx0 > sx1 || sy0 > sy1) return;
    int b0 = is_tl(&v1, &v2) ? 0 : -1, b1 = is_tl(&v2, &v0) ? 0 : -1, b2 = is_tl(&v0, &v1) ? 0 : -1;
    int64_t d0x = -(v2.Y - v1.Y) * 32, d1x = -(v0.Y - v2.Y) * 32, d2x = -(v1.Y - v0.Y) * 32;
    int64_t d0y = (v2.X - v1.X) * 32, d1y = (v0.X - v2.X) * 32, d2y = (v1.X - v0.X) * 32;
    int64_t PX0 = (int64_t)sx0 * 32 + soff, PY0 = (int64_t)sy0 * 32 + soff;
    int64_t r0 = edge(&v1, &v2, PX0, PY0), r1 = edge(&v2, &v0, PX0, PY0), r2 = edge(&v0, &v1, PX0, PY0);
    for (int y = sy0; y <= sy1; y++, r0 += d0y, r1 += d1y, r2 += d2y) {
        int64_t w0 = r0, w1 = r1, w2 = r2;
        for (int x = sx0; x <= sx1; x++, w0 += d0x, w1 += d1x, w2 += d2x) {
            if (w0 + b0 < 0 || w1 + b1 < 0 || w2 + b2 < 0) continue;
            if (!c->hq) {
                int r = v0.r, g = v0.g, b = v0.b;
                if (gour) {
                    r = (int)((v0.r * w0 + v1.r * w1 + v2.r * w2) / area);
                    g = (int)((v0.g * w0 + v1.g * w1 + v2.g * w2) / area);
                    b = (int)((v0.b * w0 + v1.b * w1 + v2.b * w2) / area);
                }
                if (tex) {
                    int u = (int)((v0.u * w0 + v1.u * w1 + v2.u * w2) / area);
                    int v = (int)((v0.v * w0 + v1.v * w1 + v2.v * w2) / area);
                    tex_plot(c, x, y, u, v, r, g, b, raw, c->semi, dither);
                } else {
                    lplot(c, x, y, r, g, b, dither, c->semi, 0);
                }
            } else {
                double ia = 1.0 / (double)area;
                double a0 = (double)w0 * ia, a1 = (double)w1 * ia, a2 = (double)w2 * ia;
                double r = v0.r, g = v0.g, b = v0.b;
                if (gour) { r = v0.r * a0 + v1.r * a1 + v2.r * a2; g = v0.g * a0 + v1.g * a1 + v2.g * a2; b = v0.b * a0 + v1.b * a1 + v2.b * a2; }
                if (tex) {
                    double u = v0.u * a0 + v1.u * a1 + v2.u * a2, v = v0.v * a0 + v1.v * a1 + v2.v * a2;
                    tex_plot_hq(c, x, y, u, v, r, g, b, raw, c->semi);
                } else {
                    lplot(c, x, y, (int)(r + 0.5), (int)(g + 0.5), (int)(b + 0.5), 0, c->semi, 0);
                }
            }
        }
    }
    (void)S;
}

static void make_v(const GpuOp *o, int i, int S, V *d) {
    const GpuVtx *s = &o->v[i];
    int64_t Q = 32 * (int64_t)S;
    d->x = s->x; d->y = s->y; d->X = (int64_t)s->x * Q; d->Y = (int64_t)s->y * Q;
    d->r = s->r; d->g = s->g; d->b = s->b; d->u = s->u; d->v = s->v;
}

static void do_poly(Ctx *c) {
    const GpuOp *o = c->o;
    int gour = !!(o->flags & GPU_F_GOURAUD), tex = !!(o->flags & GPU_F_TEXTURED), raw = !!(o->flags & GPU_F_RAW);
    int dither = !c->hq && !!(o->flags & GPU_F_DITHER);
    int nv = o->nv == 4 ? 4 : 3;
    V v[4];
    for (int i = 0; i < nv; i++) make_v(o, i, c->S, &v[i]);
    tri(c, v[0], v[1], v[2], gour, tex, raw, dither);
    if (nv == 4) tri(c, v[1], v[2], v[3], gour, tex, raw, dither);
}

static void do_rect(Ctx *c) {
    const GpuOp *o = c->o;
    int S = c->S;
    int tex = !!(o->flags & GPU_F_TEXTURED), raw = !!(o->flags & GPU_F_RAW);
    int flipx = !!(o->flags & GPU_F_FLIPX), flipy = !!(o->flags & GPU_F_FLIPY);
    int x0 = o->v[0].x, y0 = o->v[0].y, ww = o->rw, hh = o->rh;
    int u0 = o->v[0].u, v0 = o->v[0].v;
    int r = o->v[0].r, g = o->v[0].g, b = o->v[0].b;
    for (int j = 0; j < hh; j++) {
        int y = y0 + j;
        if ((y + 1) * S - 1 < c->cy1 || y * S > c->cy2) continue;
        for (int i = 0; i < ww; i++) {
            int x = x0 + i;
            if ((x + 1) * S - 1 < c->cx1 || x * S > c->cx2) continue;
            for (int sj = 0; sj < S; sj++)
                for (int si = 0; si < S; si++) {
                    int sx = x * S + si, sy = y * S + sj;
                    if (tex) {
                        if (!c->hq) {
                            int u = flipx ? u0 - i : u0 + i, v = flipy ? v0 - j : v0 + j;
                            tex_plot(c, sx, sy, u, v, r, g, b, raw, c->semi, 0);
                        } else {
                            double fu = (si + 0.5) / S, fv = (sj + 0.5) / S;
                            double u = flipx ? u0 - i + 1.0 - fu : u0 + i + fu, v = flipy ? v0 - j + 1.0 - fv : v0 + j + fv;
                            tex_plot_hq(c, sx, sy, u, v, r, g, b, raw, c->semi);
                        }
                    } else {
                        lplot(c, sx, sy, r, g, b, 0, c->semi, 0);
                    }
                }
        }
    }
}

static void do_line(Ctx *c) {
    const GpuOp *o = c->o;
    const GpuVtx *a = &o->v[0], *b = &o->v[1];
    int S = c->S;
    int gour = !!(o->flags & GPU_F_GOURAUD);
    int dx = b->x - a->x, dy = b->y - a->y, adx = abs(dx), ady = abs(dy);
    if (adx >= 1024 || ady >= 512) return;
    if (!c->hq) {   /* mismo DDA que el oráculo */
        int dith = !!(o->flags & GPU_F_DITHER);
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
            lplot(c, x, y, r, g, bb, dith, c->semi, 0);
        }
        return;
    }
    /* alta resolución: cinta de 1 píxel nativo de ancho con extremos prolongados 0,5 px (cubre los mismos píxeles que el DDA) */
    double ax = a->x + 0.5, ay = a->y + 0.5, bx = b->x + 0.5, by = b->y + 0.5;
    double len = sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
    double ux = len > 0 ? (bx - ax) / len : 1, uy = len > 0 ? (by - ay) / len : 0;
    int minx = (int)floor((fmin(ax, bx) - 1.0) * S), maxx = (int)ceil((fmax(ax, bx) + 1.0) * S);
    int miny = (int)floor((fmin(ay, by) - 1.0) * S), maxy = (int)ceil((fmax(ay, by) + 1.0) * S);
    if (minx < c->cx1) minx = c->cx1;
    if (miny < c->cy1) miny = c->cy1;
    if (maxx > c->cx2) maxx = c->cx2;
    if (maxy > c->cy2) maxy = c->cy2;
    for (int sy = miny; sy <= maxy; sy++)
        for (int sx = minx; sx <= maxx; sx++) {
            double px = (sx + 0.5) / S - ax, py = (sy + 0.5) / S - ay;
            double t = px * ux + py * uy, d = fabs(-px * uy + py * ux);
            if (d > 0.5 || t < -0.5 || t > len + 0.5) continue;
            double s = len > 0 ? t / len : 0;
            s = s < 0 ? 0 : s > 1 ? 1 : s;
            int r = a->r, g = a->g, bb = a->b;
            if (gour) {
                r = (int)(a->r + (b->r - a->r) * s + 0.5); g = (int)(a->g + (b->g - a->g) * s + 0.5);
                bb = (int)(a->b + (b->b - a->b) * s + 0.5);
            }
            lplot(c, sx, sy, r, g, bb, 0, c->semi, 0);
        }
}

/* ---------------- eventos de VRAM (gpu.c cmd_fill / cmd_copy / subida) ---------------- */
static void ev_fill(VrLayer2D *L, const GpuOp *o) {
    for (int j = 0; j < o->rh; j++)
        for (int i = 0; i < o->rw; i++) L->vram[(o->ry + j) & (VH - 1)][(o->rx + i) & (VW - 1)] = o->fill15;
}

static void ev_upload(VrLayer2D *L, const GpuOp *o, const GpuDrawList *l) {
    if (!l->data || o->rw <= 0 || o->rh <= 0) return;
    int mchk = !!(o->flags & GPU_F_MASK_CHK), mset = !!(o->flags & GPU_F_MASK_SET);
    for (uint32_t i = 0; i < o->data_n && i < (uint32_t)(o->rw * o->rh) && o->data_off + i < l->ndata; i++) {
        int x = (o->rx + (int)(i % (uint32_t)o->rw)) & (VW - 1), y = (o->ry + (int)(i / (uint32_t)o->rw)) & (VH - 1);
        uint16_t *p = &L->vram[y][x];
        if (mchk && (*p & 0x8000)) continue;
        *p = (uint16_t)(l->data[o->data_off + i] | (mset ? 0x8000 : 0));
    }
}

static void ev_copy(VrLayer2D *L, const GpuOp *o) {
    int mchk = !!(o->flags & GPU_F_MASK_CHK), mset = !!(o->flags & GPU_F_MASK_SET);
    uint16_t line[VW];
    int ww = o->rw > VW ? VW : o->rw;
    for (int j = 0; j < o->rh; j++) {
        int sy = (o->sy + j) & (VH - 1), dy = (o->ry + j) & (VH - 1);
        for (int i = 0; i < ww; i++) line[i] = L->vram[sy][(o->sx + i) & (VW - 1)];
        for (int i = 0; i < ww; i++) {
            uint16_t *p = &L->vram[dy][(o->rx + i) & (VW - 1)];
            if (mchk && (*p & 0x8000)) continue;
            *p = (uint16_t)(line[i] | (mset ? 0x8000 : 0));
        }
    }
}

/* ---------------- composición y codificación ---------------- */
void vl2d_compose5(const VrLayerPx *p, const uint8_t bg[3], uint8_t out[3]) {
    for (int k = 0; k < 3; k++) {
        int v4 = p->c[k] * 4 + (int)(((int)p->t * bg[k] * 4 + 128) / 256) - (int)p->d[k];
        int v = v4 < 0 ? 0 : (v4 + 2) / 4;
        out[k] = (uint8_t)(v > 31 ? 31 : v);
    }
}

void vl2d_encode(const VrLayerPx *p, int hq, uint8_t rgba[4]) {
    int cmax = hq ? 255 : 31;
    if (p->t == 0 && !p->d[0] && !p->d[1] && !p->d[2]) {   /* opaco: la salida de vídeo de la GPU original */
        if (hq) { rgba[0] = p->c[0]; rgba[1] = p->c[1]; rgba[2] = p->c[2]; }
        else { rgba[0] = (uint8_t)ex8(p->c[0]); rgba[1] = (uint8_t)ex8(p->c[1]); rgba[2] = (uint8_t)ex8(p->c[2]); }
        rgba[3] = 255;
        return;
    }
    float cov = 1.0f - (float)p->t / 256.0f;                 /* parte de la imagen que tapa lo dibujado */
    float dm = (float)(p->d[0] + p->d[1] + p->d[2]) / (3.0f * 4.0f * (float)cmax);   /* oscurecimiento medio por la resta pendiente */
    if (dm > 1.0f) dm = 1.0f;
    float keep = 1.0f - dm;
    float C[3];
    for (int k = 0; k < 3; k++) C[k] = (hq ? (float)p->c[k] : (float)ex8(p->c[k])) / 255.0f * keep;
    float a = 1.0f - (1.0f - cov) * keep;                    /* alfa premultiplicado equivalente */
    float mx = C[0] > C[1] ? C[0] : C[1];
    if (C[2] > mx) mx = C[2];
    if (a < mx) a = mx;                                      /* aditivo puro (alfa 0): alfa = canal máximo */
    if (a <= 0.0f) { rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0; return; }
    if (a > 1.0f) a = 1.0f;
    for (int k = 0; k < 3; k++) {
        float v = C[k] / a;
        rgba[k] = (uint8_t)(v <= 0.0f ? 0 : v >= 1.0f ? 255 : (int)(v * 255.0f + 0.5f));
    }
    rgba[3] = (uint8_t)(a * 255.0f + 0.5f);
}

/* ---------------- fondo de la pantalla de cine: zona mostrada de la VRAM espejo ampliada ---------------- */
static void fill_backdrop(VrLayer2D *L, const GpuDisplay *d) {
    int S = L->R, mode = L->smooth, w = L->w, h = L->h;
    size_t need = (size_t)w * h * 3;
    if (need > L->nat_cap) { uint8_t *q = (uint8_t *)realloc(L->nat, need); if (!q) return; L->nat = q; L->nat_cap = need; }
    if ((size_t)w * h > L->natm_cap) { uint8_t *q = (uint8_t *)realloc(L->natm, (size_t)w * h); if (!q) return; L->natm = q; L->natm_cap = (size_t)w * h; }
    int pad = L->pad, aw = L->aw;
    for (int y = 0; y < h; y++) {   /* imagen nativa descodificada (15 o 24 bits); fuera de la ventana visible (márgenes del rango) negro, como el hardware */
        int py = (d->y + y) & (VH - 1);
        uint8_t *o = &L->nat[(size_t)y * w * 3], *om = &L->natm[(size_t)y * w];
        memset(o, 0, (size_t)w * 3); memset(om, 0, (size_t)w);
        for (int x = pad; x < pad + aw && x < w; x++) {
            int xs = x - pad;
            if (d->depth24) { const uint8_t *ln = (const uint8_t *)L->vram[py]; for (int k = 0; k < 3; k++) o[x * 3 + k] = ln[((d->x * 2) + xs * 3 + k) & 2047]; }
            else {
                uint16_t t = L->vram[py][(d->x + xs) & (VW - 1)];
                o[x * 3] = (uint8_t)ex8(t & 31); o[x * 3 + 1] = (uint8_t)ex8((t >> 5) & 31); o[x * 3 + 2] = (uint8_t)ex8((t >> 10) & 31);
                om[x] = (uint8_t)(t >> 15);   /* bit de máscara del píxel de VRAM: una primitiva con «comprobar máscara» no lo pisa (gpu.c plot) */
            }
        }
    }
    int *xi = (int *)malloc((size_t)L->PW * 2 * sizeof(int));
    if (!xi) return;
    int *wxi = xi + L->PW;
    for (int lx = 0; lx < L->PW; lx++) {   /* columna nativa izquierda y peso (0..256) del vecino derecho */
        if (mode == VL_SMOOTH_ORIGINAL || S == 1) { xi[lx] = lx / S; wxi[lx] = 0; continue; }
        double fx = (lx + 0.5) / S - 0.5;
        int x0 = (int)floor(fx);
        double wx = fx - x0;
        if (mode == VL_SMOOTH_SHARP) { double q = (wx - 0.5) * S + 0.5; wx = q < 0 ? 0 : q > 1 ? 1 : q; }
        xi[lx] = x0; wxi[lx] = (int)(wx * 256.0 + 0.5);
    }
    for (int ly = 0; ly < L->PH; ly++) {
        int y0, wy;
        if (mode == VL_SMOOTH_ORIGINAL || S == 1) { y0 = ly / S; wy = 0; }
        else {
            double fy = (ly + 0.5) / S - 0.5;
            y0 = (int)floor(fy);
            double q = fy - y0;
            if (mode == VL_SMOOTH_SHARP) { q = (q - 0.5) * S + 0.5; q = q < 0 ? 0 : q > 1 ? 1 : q; }
            wy = (int)(q * 256.0 + 0.5);
        }
        int ya = y0 < 0 ? 0 : y0 >= h ? h - 1 : y0, yb = y0 + 1 < 0 ? 0 : y0 + 1 >= h ? h - 1 : y0 + 1;
        const uint8_t *ra = &L->nat[(size_t)ya * w * 3], *rb = &L->nat[(size_t)yb * w * 3];
        VrLayerPx *p = &L->px[(size_t)ly * L->PW];
        for (int lx = 0; lx < L->PW; lx++, p++) {
            int xa = xi[lx] < 0 ? 0 : xi[lx] >= w ? w - 1 : xi[lx], xb = xi[lx] + 1 < 0 ? 0 : xi[lx] + 1 >= w ? w - 1 : xi[lx] + 1;
            int wx = wxi[lx];
            for (int k = 0; k < 3; k++) {
                int v;
                if (!wx && !wy) v = ra[xa * 3 + k];
                else {
                    int top = ra[xa * 3 + k] * (256 - wx) + ra[xb * 3 + k] * wx, bot = rb[xa * 3 + k] * (256 - wx) + rb[xb * 3 + k] * wx;
                    v = (top * (256 - wy) + bot * wy + 32768) >> 16;
                }
                p->c[k] = (uint8_t)(S == 1 ? v >> 3 : v);
            }
            p->m = L->natm[(size_t)(ly / S) * w + lx / S]; p->t = 0; p->d[0] = p->d[1] = p->d[2] = 0;
        }
    }
    free(xi);
}

/* ---------------- lista ---------------- */
static int ensure(VrLayer2D *L, size_t npx, size_t nout) {
    if (npx > L->px_cap) {
        VrLayerPx *q = (VrLayerPx *)realloc(L->px, npx * sizeof *q);
        if (!q) return 0;
        L->px = q; L->px_cap = npx; L->cur_scale = 0;   /* el contenido ya no es válido: reinicialización completa */
    }
    if (nout * 4 > L->rgba_cap) {
        uint8_t *q = (uint8_t *)realloc(L->rgba, nout * 4);
        if (!q) return 0;
        L->rgba = q; L->rgba_cap = nout * 4; L->cur_scale = 0;
    }
    return 1;
}

static void blank(VrLayer2D *L, int x0, int y0, int x1, int y1) {   /* [x0,x1) x [y0,y1) en muestras de px: capa transparente (y su salida RGBA, k veces mayor en modo original) */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > L->PW) x1 = L->PW;
    if (y1 > L->PH) y1 = L->PH;
    if (x1 <= x0) return;
    int k = L->LW / L->PW;
    for (int y = y0; y < y1; y++) {
        VrLayerPx *p = &L->px[(size_t)y * L->PW + x0];
        for (int x = x0; x < x1; x++, p++) { memset(p, 0, sizeof *p); p->t = 256; }
        for (int j = 0; j < k; j++) memset(&L->rgba[((size_t)(y * k + j) * L->LW + (size_t)x0 * k) * 4], 0, (size_t)(x1 - x0) * k * 4);
    }
}

/* caja de una primitiva en muestras (conservadora), para el rectángulo sucio */
static void op_box(const GpuOp *o, int S, int *x0, int *y0, int *x1, int *y1) {
    int a = 1 << 20, b = 1 << 20, c = -(1 << 20), d = -(1 << 20);
    if (o->type == GPU_OP_RECT) { a = o->v[0].x; b = o->v[0].y; c = a + o->rw; d = b + o->rh; }
    else {
        int n = o->type == GPU_OP_LINE ? 2 : (o->nv == 4 ? 4 : 3);
        for (int i = 0; i < n; i++) {
            if (o->v[i].x < a) a = o->v[i].x;
            if (o->v[i].y < b) b = o->v[i].y;
            if (o->v[i].x + 1 > c) c = o->v[i].x + 1;
            if (o->v[i].y + 1 > d) d = o->v[i].y + 1;
        }
    }
    if (a < o->cx1) a = o->cx1;
    if (b < o->cy1) b = o->cy1;
    if (c > o->cx2 + 1) c = o->cx2 + 1;
    if (d > o->cy2 + 1) d = o->cy2 + 1;
    *x0 = (a - 1) * S; *y0 = (b - 1) * S; *x1 = (c + 1) * S; *y1 = (d + 1) * S;
}

int vl2d_run(VrLayer2D *L, const GpuDrawList *l, const VrScene *sc, int full) {
    if (!L || !l) return 0;
    L->valid = 0;
    /* La VRAM de partida es la REAL del emulador al inicio de la lista (vram_start, cada lista) o, si el productor no la da, la instantánea de la primera lista/resincronización
     * (vram_init) más los eventos: así una subida o un relleno que no pasen por la lista (DMA, copias, escrituras del juego) nunca dejan texels basura en fuentes y CLUT. */
    { const uint16_t *v0 = l->vram_start ? l->vram_start : l->vram_init; if (v0) { memcpy(L->vram, v0, sizeof L->vram); L->have_vram = 1; } }
    int draw = sc && sc->op_dest && sc->n_op_dest >= l->nops && L->have_vram;
    int S = L->scale;   /* escala de la SALIDA */
    int R = S;          /* escala de rasterizado */
    int w = l->disp.w > 0 ? l->disp.w : 320, h = l->disp.h > 0 ? l->disp.h : 240;
    int pad = 0, aw = 0, oxe = l->disp.x;
    if (full) {   /* pantalla completa: el MARCO del hardware (fw x h, como gpu_present_rgba): el rango horizontal puede ser mayor o menor que el nominal (créditos, códec) */
        int fw = l->disp.fw > 0 ? l->disp.fw : w;
        if (fw > VW) fw = VW;
        pad = l->disp.pad_l > 0 ? l->disp.pad_l : 0;
        aw = l->disp.aw > 0 ? l->disp.aw : fw;
        if (pad > fw) pad = fw;
        if (pad + aw > fw) aw = fw - pad;
        w = fw; oxe = l->disp.x - pad;
    }
    if (w > VW) w = VW;
    if (h > VH) h = VH;
    if (!full) aw = w;
    int ux0 = 0, uy0 = 0, ux1 = 0, uy1 = 0;   /* sucio de esta lista */
    /* Modo original: el rasterizado es el EXACTO de la GPU original (escala 1: 5 bits, tramado, mismo muestreo de texels) y la salida lo replica S x S (vecino más próximo
     * a escala entera): el texto sale idéntico en forma y color y nítido. Excepción: fotograma completo en 24 bits (vídeo), que necesita los 8 bits por canal. */
    if (L->smooth == VL_SMOOTH_ORIGINAL && !(full && l->disp.depth24)) R = 1;
    if (draw) {
        size_t nout = (size_t)w * S * h * S, npx = (size_t)w * R * h * R;
        if (nout > (size_t)6 * 1024 * 6 * 512) draw = 0;   /* tope de memoria (con escala 6 una pantalla de 640x480 son 11 Mpx) */
        else if (!ensure(L, npx, nout)) draw = 0;
    }
    if (draw) {
        int key = (R << 8) | (S << 4) | L->smooth;
        if (L->cur_scale != key || L->w != w || L->h != h || L->ox != oxe || L->oy != l->disp.y || L->pad != pad || L->aw != aw) {
            L->w = w; L->h = h; L->ox = oxe; L->oy = l->disp.y; L->pad = pad; L->aw = aw; L->LW = w * S; L->LH = h * S;
            L->R = R; L->PW = w * R; L->PH = h * R;
            L->cur_scale = key;
            L->dx0 = L->dy0 = 0; L->dx1 = L->PW; L->dy1 = L->PH;   /* reinicialización completa */
        }
        blank(L, L->dx0, L->dy0, L->dx1, L->dy1);
        L->dx0 = L->dy0 = L->dx1 = L->dy1 = 0;
        ux0 = L->PW; uy0 = L->PH; ux1 = 0; uy1 = 0;
        if (full) {
            fill_backdrop(L, &l->disp);
            ux0 = 0; uy0 = 0; ux1 = L->PW; uy1 = L->PH;
        }
    }
    L->vend = l->vram_end;
    memset(&L->info, 0, sizeof L->info);
    L->info.fw = w; L->info.h = h; L->info.aw = aw; L->info.pad = pad; L->info.scale = S; L->info.raster = R; L->info.smooth = L->smooth; L->info.full = full;
    L->info.used_start = l->vram_start != NULL; L->info.used_end = l->vram_end != NULL;
    memset(L->drawn, 0, sizeof L->drawn);
    Ctx c0;
    memset(&c0, 0, sizeof c0);
    c0.L = L; c0.S = R; c0.hq = R > 1; c0.smooth = L->smooth; c0.cmax = c0.hq ? 255 : 31;
    for (uint32_t i = 0; i < l->nops; i++) {
        const GpuOp *o = &l->ops[i];
        switch (o->type) {
        case GPU_OP_FILL: ev_fill(L, o); break;
        case GPU_OP_UPLOAD: ev_upload(L, o, l); break;
        case GPU_OP_VCOPY: ev_copy(L, o); break;
        case GPU_OP_POLY: case GPU_OP_RECT: case GPU_OP_LINE: {
            if (l->vram_end) {   /* ¿dibuja fuera de la zona mostrada? (render a textura): esos bloques de VRAM se leen de la VRAM al cerrar */
                int a, b, e, f;
                op_box(o, 1, &a, &b, &e, &f);
                if (a < o->cx1) a = o->cx1;
                if (b < o->cy1) b = o->cy1;
                if (e > o->cx2 + 1) e = o->cx2 + 1;
                if (f > o->cy2 + 1) f = o->cy2 + 1;
                if (a < 0) a = 0;
                if (b < 0) b = 0;
                if (e > VW) e = VW;
                if (f > VH) f = VH;
                int inside = a >= l->disp.x && e <= l->disp.x + l->disp.w && b >= l->disp.y && f <= l->disp.y + l->disp.h;
                if (!inside && e > a && f > b)
                    for (int by = b >> 4; by <= (f - 1) >> 4; by++) for (int bx = a >> 4; bx <= (e - 1) >> 4; bx++) L->drawn[by * 64 + bx] = 1;
            }
            if (!draw) break;
            if (!full && sc->op_dest[i] != 2) break;
            L->info.prims++;
            if (o->flags & GPU_F_MASK_CHK) L->info.mask_prims++;
            Ctx c = c0;
            c.o = o;
            c.cx1 = o->cx1 * R; c.cy1 = o->cy1 * R; c.cx2 = (o->cx2 + 1) * R - 1; c.cy2 = (o->cy2 + 1) * R - 1;
            c.semi = !!(o->flags & GPU_F_SEMI);
            c.mask_chk = !!(o->flags & GPU_F_MASK_CHK); c.mask_set = !!(o->flags & GPU_F_MASK_SET);
            c.abr = o->abr;
            if (o->fbk && o->type != GPU_OP_LINE) {   /* realimentación resuelta: un único texel (gpu.h, GpuOp.fbk) */
                if (!o->fbt) break;
                if (!(o->flags & GPU_F_TEXTURED)) break;
                c.fixed = 1;
            }
            if (o->flags & GPU_F_TEXTURED) {   /* caja de texels de la primitiva */
                int n = o->type == GPU_OP_RECT ? 1 : (o->nv == 4 ? 4 : 3);
                int umin = 255, umax = 0, vmin = 255, vmax = 0;
                if (o->type == GPU_OP_RECT) {
                    int u0 = o->v[0].u, v0 = o->v[0].v;
                    int u1 = (o->flags & GPU_F_FLIPX) ? u0 - (o->rw - 1) : u0 + (o->rw - 1), v1 = (o->flags & GPU_F_FLIPY) ? v0 - (o->rh - 1) : v0 + (o->rh - 1);
                    umin = u0 < u1 ? u0 : u1; umax = u0 < u1 ? u1 : u0; vmin = v0 < v1 ? v0 : v1; vmax = v0 < v1 ? v1 : v0;
                } else {
                    for (int k = 0; k < n; k++) {
                        if (o->v[k].u < umin) umin = o->v[k].u;
                        if (o->v[k].u > umax) umax = o->v[k].u;
                        if (o->v[k].v < vmin) vmin = o->v[k].v;
                        if (o->v[k].v > vmax) vmax = o->v[k].v;
                    }
                }
                if (umax > 255) umax = 255;
                if (vmax > 255) vmax = 255;
                if (umin < 0) umin = 0;
                if (vmin < 0) vmin = 0;
                c.umin = umin; c.umax = umax; c.vmin = vmin; c.vmax = vmax;
                L->stamp++;
                if (L->stamp == 0) { memset(L->ts, 0, sizeof L->ts); memset(L->qs, 0, sizeof L->qs); L->stamp = 1; }
            }
            if (!full) {
                int a, b, e, f;
                op_box(o, R, &a, &b, &e, &f);
                a -= L->ox * R; e -= L->ox * R; b -= L->oy * R; f -= L->oy * R;
                if (a < ux0) ux0 = a;
                if (b < uy0) uy0 = b;
                if (e > ux1) ux1 = e;
                if (f > uy1) uy1 = f;
            }
            if (o->type == GPU_OP_POLY) do_poly(&c);
            else if (o->type == GPU_OP_RECT) do_rect(&c);
            else do_line(&c);
            break;
        }
        default: break;
        }
    }
    L->vend = NULL;
    if (!draw) return 0;
    if (ux0 < 0) ux0 = 0;
    if (uy0 < 0) uy0 = 0;
    if (ux1 > L->PW) ux1 = L->PW;
    if (uy1 > L->PH) uy1 = L->PH;
    if (ux1 > ux0 && uy1 > uy0) {
        int k = L->LW / L->PW;   /* replicado de salida (1 salvo en modo original) */
        for (int y = uy0; y < uy1; y++) {
            const VrLayerPx *p = &L->px[(size_t)y * L->PW + ux0];
            uint8_t *q = &L->rgba[((size_t)y * k * L->LW + (size_t)ux0 * k) * 4];
            for (int x = ux0; x < ux1; x++, p++, q += 4 * k) {
                vl2d_encode(p, c0.hq, q);
                for (int i = 1; i < k; i++) memcpy(q + 4 * i, q, 4);
            }
            for (int j = 1; j < k; j++) memcpy(&L->rgba[((size_t)(y * k + j) * L->LW + (size_t)ux0 * k) * 4], &L->rgba[((size_t)y * k * L->LW + (size_t)ux0 * k) * 4], (size_t)(ux1 - ux0) * k * 4);
        }
        L->dx0 = ux0; L->dy0 = uy0; L->dx1 = ux1; L->dy1 = uy1;
    }
    L->valid = 1;
    return 1;
}

const uint8_t *vl2d_rgba(const VrLayer2D *L, int *w, int *h) {
    if (!L || !L->valid) return NULL;
    if (w) *w = L->LW;
    if (h) *h = L->LH;
    return L->rgba;
}
const VrLayerPx *vl2d_px(const VrLayer2D *L, int *w, int *h) {
    if (!L || !L->valid) return NULL;
    if (w) *w = L->PW;
    if (h) *h = L->PH;
    return L->px;
}
void vl2d_info(const VrLayer2D *L, VrL2Info *o) { if (o) { if (L) *o = L->info; else memset(o, 0, sizeof *o); } }
const uint16_t *vl2d_vram(const VrLayer2D *L) { return L ? &L->vram[0][0] : NULL; }
