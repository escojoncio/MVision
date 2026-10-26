/* vr_layer2d: ver vr_layer2d.h. Con escala 1 la aritmética de píxel, texel, triángulo, rect y línea es la de runtime/gpu.c (oráculo) tal cual: cualquier cambio
 * ahí debe pasar test_layer2d.c (compara con gpu.c bit a bit). Con escala > 1 (alta resolución) el relleno sigue la misma regla de arista pero muestreando en el
 * centro de cada submuestra, con color de 8 bits (sin tramado) y muestreo de textura filtrado (original / nítido / suave) sobre los texels de VRAM.
 * El destino no es la VRAM sino una capa con transmitancia (ver el .h). */
#include "vr_layer2d.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>
#define VW 1024
#define VH 512

typedef struct { float x0, y0, x1, y1; int par, cnt, op, tot, chg; } MNew;   /* op: marco/fondo opaco (sin textura ni SEMI); par/cnt: unión-búsqueda y nº de primitivas del componente; tot/chg: celdas de 8 px que cubre (suma sobre sus primitivas) y cuántas ya cambiaban sin menú */
struct VrLayer2D {
    uint16_t vram[VH][VW];   /* espejo: solo eventos de VRAM (subidas, copias, rellenos) */
    int have_vram;
    VrL2Info info;
    int pad, aw;                 /* ventana visible dentro de la capa (pantalla completa): columnas [pad, pad+aw) de w, el resto negro */
    uint8_t *natm; size_t natm_cap;   /* bit 15 (máscara) de la VRAM mostrada, por píxel nativo */
    int scale, smooth;
    VrLayerPx *px; size_t px_cap;
    uint8_t *rgba; size_t rgba_cap;
    int w, h, ox, oy, valid;     /* zona mostrada en píxeles nativos y su origen en VRAM */
    int LW, LH;                  /* tamaño de la salida RGBA: w*scale, h*scale */
    int R, PW, PH;               /* escala de RASTERIZADO (1 en modo original: exacto de 5 bits y replicado; = scale en los demás) y tamaño de px: w*R, h*R */
    int hq8;                     /* px.c en 8 bits por canal (R > 1, o fotograma completo de 24 bits a R = 1); si no, 5 bits */
    int cur_scale;               /* clave (R, escala, modo, hq8) con la que están inicializados px/rgba */
    int dx0, dy0, dx1, dy1;      /* rectángulo sucio (muestras, [x0,x1) [y0,y1)) de la lista anterior; vacío si dx1 <= dx0 */
    uint16_t tc[65536]; uint32_t ts[65536]; uint32_t stamp;   /* caché de texels de la primitiva en curso */
    uint32_t qc[262144]; uint32_t qs[262144];                 /* caché del modo nítido por cuarto de texel: texel que cubre | color corregido << 16 */
    uint8_t *nat; size_t nat_cap;                             /* imagen nativa descodificada de la zona mostrada (fondo de la pantalla de cine) */
    uint16_t *vsnap;                                          /* copia de la VRAM espejo al empezar la lista (verificación del panel: la pasada del mundo la modifica y se restaura) */
    uint8_t *w5; size_t w5_cap;                               /* el mundo del panel (todo salvo las primitivas 2D del panel) en la zona mostrada: aw x h x 3, 5 bits por canal */
    uint8_t *vmap; size_t vmap_cap;                           /* mapa por píxel de la verificación (tocado / mundo distinto fuera / distinto) */
    int menu_on, menu_ok; float menu_uv[4];                   /* menú sobre la mano: activo / hay rect en la última lista / rect UV */
    uint8_t mchg[(VH / 8) * (VW / 8)];                        /* celdas de 8 px (px nativos de la capa) con primitivas nuevas en las últimas MCHG_LISTS listas SIN menú: radar, créditos que se desplazan, barras animadas (lista restante, 0 = no cambia) */
    int menu_n, menu_calm, menu_set;                          /* listas de crecimiento con el menú activo desde que se abrió / listas consecutivas SIN crecer el rect / rect asentado (congelado hasta cerrar) */
    int menu_have; float menu_px[4];                          /* rect conservado mientras el menú siga activo (px nativos x0,y0,x1,y1): solo crece con primitivas nuevas pegadas a él */
    uint64_t *mbase; size_t mbase_n, mbase_cap;               /* claves (ordenadas) de las primitivas del panel de la última lista sin menú */
    uint64_t *mcur; size_t mcur_n, mcur_cap;                  /* claves de la lista en curso */
    MNew *mnp; size_t mnp_n, mnp_cap;                         /* primitivas nuevas de la lista en curso (sin rect conservado): cajas en px nativos, para elegir la ventana del selector */
    int fb_hold;                                              /* listas que faltan de respaldo exacto tras una verificación fallida (anti-parpadeo) */
};

#define MENU_CALM_LISTS 8   /* listas consecutivas sin crecer el rect tras las cuales se asienta (se congela hasta cerrar el selector) */
#define MENU_GROW_MAX 60    /* máximo de listas de crecimiento por apertura: pasado esto se asienta aunque siga cambiando */
#define MENU_GROW_EPS 0.5f  /* px nativos: variación del rect que cuenta como crecimiento */
#define MENU_KEEP_PX 2.0f   /* px nativos: holgura del rect asentado para decidir si una primitiva nueva lo corta y se pinta en la capa del selector (el rect lleva ya el margen del 1 % del visor) */
#define MENU_BAND_V 0.35f   /* franja superior de la capa (vida/oxígeno y créditos de partida: hudRegions 0-1 del visor): sus primitivas nunca son la ventana del selector */

static void menu_forget(VrLayer2D *L) { L->menu_have = 0; L->menu_n = 0; L->menu_calm = 0; L->menu_set = 0; }

VrLayer2D *vl2d_new(void) {
    VrLayer2D *L = (VrLayer2D *)calloc(1, sizeof(VrLayer2D));
    if (L) { L->scale = 4; L->smooth = VL_SMOOTH_ORIGINAL; L->R = 1; L->PW = 1; L->PH = 1; L->LW = 1; L->LH = 1; }
    return L;
}
void vl2d_free(VrLayer2D *L) { if (!L) return; free(L->mbase); free(L->mcur); free(L->mnp); free(L->vsnap); free(L->w5); free(L->vmap); free(L->natm); free(L->px); free(L->rgba); free(L->nat); free(L); }
void vl2d_reset(VrLayer2D *L) { if (!L) return; L->have_vram = 0; L->valid = 0; menu_forget(L); memset(L->mchg, 0, sizeof L->mchg); }
void vl2d_set_options(VrLayer2D *L, int scale, int smooth) {
    if (!L) return;
    L->scale = scale < 1 ? 1 : scale > 6 ? 6 : scale;
    L->smooth = smooth < 0 ? 0 : smooth > 2 ? 2 : smooth;
}

void vl2d_set_menu(VrLayer2D *L, int on) { if (!L) return; L->menu_on = on != 0; if (!on) menu_forget(L); }   /* al salir del selector se olvida el rect */
int vl2d_menu_rect(const VrLayer2D *L, float uv[4]) {
    if (!L || !L->valid || !L->menu_ok) return 0;
    if (uv) memcpy(uv, L->menu_uv, sizeof L->menu_uv);
    return 1;
}

/* Rect asentado: el rect crece por unión hasta llevar MENU_CALM_LISTS listas consecutivas sin crecer (o MENU_GROW_MAX listas de crecimiento) y entonces queda fijo hasta cerrar el selector. El consumidor solo debe usar el rect de ahí en adelante. */
int vl2d_menu_settled(const VrLayer2D *L) { return L && L->menu_on && L->menu_set; }

/* clave de una primitiva del panel (FNV-1a de 64 bits) para el menú sobre la mano */
/* ox, oy: origen de la zona mostrada en VRAM: las coordenadas del paquete llevan el del búfer en que dibuja el juego (doble búfer, x = 0 / 320), la clave no */
static uint64_t op_key(const GpuOp *o, int ox, int oy) {
    uint64_t h = 1469598103934665603ull;
    uint32_t f[16]; int n = 0;
    f[n++] = o->type; f[n++] = o->flags; f[n++] = (uint16_t)(o->v[0].x - ox); f[n++] = (uint16_t)(o->v[0].y - oy);
    if (o->type == GPU_OP_RECT) { f[n++] = (uint16_t)o->rx; f[n++] = (uint16_t)o->ry; f[n++] = (uint16_t)o->rw; f[n++] = (uint16_t)o->rh; }
    else { int nv = o->type == GPU_OP_LINE ? 2 : (o->nv == 4 ? 4 : 3); for (int i = 1; i < nv; i++) { f[n++] = (uint16_t)(o->v[i].x - ox); f[n++] = (uint16_t)(o->v[i].y - oy); } }
    f[n++] = o->tx; f[n++] = o->ty; f[n++] = o->clx; f[n++] = o->cly; f[n++] = o->v[0].u; f[n++] = o->v[0].v;
    for (int i = 0; i < n; i++) for (int b = 0; b < 4; b++) { h ^= (f[i] >> (8 * b)) & 255u; h *= 1099511628211ull; }
    return h;
}
static int key_cmp(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static int key_in(const uint64_t *v, size_t n, uint64_t k) { return n && bsearch(&k, v, n, sizeof k, key_cmp) != NULL; }
static void key_push(uint64_t **v, size_t *n, size_t *cap, uint64_t k) {
    if (*n == *cap) { size_t c = *cap ? *cap * 2 : 256; uint64_t *q = (uint64_t *)realloc(*v, c * sizeof **v); if (!q) return; *v = q; *cap = c; }
    (*v)[(*n)++] = k;
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
    uint16_t (*vdst)[VW];         /* != NULL: el destino es la VRAM espejo (escala 1, 15 bits, igual que gpu.c plot), no la capa: dibujos fuera de la zona mostrada / fotograma completo */
    int umin, umax, vmin, vmax;   /* caja de texels de la primitiva (el filtrado no sale de ella: sin sangrado entre sprites del atlas) */
} Ctx;

/* ---------------- escritura de muestra ---------------- */
/* sx, sy: muestra en VRAM (x*S+si). r,g,b: 0..255 (escala 1: valor de 8 bits previo al canal de 5; escala > 1: color final). */
static inline void vplot(Ctx *c, int x, int y, int r, int g, int b, int dith, int semi, int mbit) {   /* gpu.c plot, sobre la VRAM espejo */
    uint16_t *p = &c->vdst[y & (VH - 1)][x & (VW - 1)];
    if (c->mask_chk && (*p & 0x8000)) return;
    if (dith) { int d = dither_m[y & 3][x & 3]; r += d; g += d; b += d; }
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    r >>= 3; g >>= 3; b >>= 3;
    if (semi) {
        int br = *p & 31, bg = (*p >> 5) & 31, bb = (*p >> 10) & 31;
        switch (c->abr) {
        case 0: r = (br + r) >> 1; g = (bg + g) >> 1; b = (bb + b) >> 1; break;
        case 1: r += br; g += bg; b += bb; break;
        case 2: r = br - r; g = bg - g; b = bb - b; break;
        default: r = br + (r >> 2); g = bg + (g >> 2); b = bb + (b >> 2); break;
        }
        r = r < 0 ? 0 : r > 31 ? 31 : r;
        g = g < 0 ? 0 : g > 31 ? 31 : g;
        b = b < 0 ? 0 : b > 31 ? 31 : b;
    }
    *p = (uint16_t)(r | (g << 5) | (b << 10) | ((mbit | c->mask_set) ? 0x8000 : 0));
}

static inline void lplot(Ctx *c, int sx, int sy, int r, int g, int b, int dith, int semi, int mbit) {
    if (sx < c->cx1 || sx > c->cx2 || sy < c->cy1 || sy > c->cy2) return;
    if (c->vdst) { vplot(c, sx, sy, r, g, b, dith, semi, mbit); return; }
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
/* Texel de VRAM: del espejo. Los dibujos de la lista a zonas fuera de la mostrada (render a textura) se aplican al espejo EN ORDEN (vplot), así que una textura
 * compuesta varias veces en la misma lista (mismo búfer de composición reutilizado) se lee con el contenido que tenía en cada momento. */
static inline uint16_t vget(const VrLayer2D *L, int x, int y) { return L->vram[y][x]; }

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
    uint16_t t = c->fixed ? c->o->fbt : c->vdst ? texel_raw(c->L, c->o, u, v) : texel(c, u, v);   /* con vdst sin caché: la primitiva puede leer lo que ella misma escribe (como gpu.c) */
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
/* Un evento escribe SIEMPRE en la VRAM espejo y, según `sink`, también en la capa (sus píxeles son opacos: lo que la GPU deja en la VRAM mostrada):
 *   SINK_NONE   solo el espejo (la capa se construye al final a partir del espejo, o no hay capa);
 *   SINK_FULL   fotograma completo a alta resolución: el relleno, la subida y la copia que caen en la zona visible se pintan en la capa en su orden (borrados y copias
 *               de fondo que quitan lo dibujado antes: sin esto quedan restos de fotogramas anteriores);
 *   SINK_PANEL  panel sobre el mundo: solo las copias VRAM->VRAM CUYO ORIGEN ESTÁ FUERA de la zona mostrada (texto compuesto fuera de pantalla y copiado a ella); el texel 0 es transparente
 *               (el fondo negro de la composición no tapa el mundo). Los rellenos y las subidas a la zona mostrada no se pintan en el panel (borrado del fotograma / vídeo: el mundo es el nativo). */
enum { SINK_NONE = 0, SINK_FULL = 1, SINK_PANEL = 2 };

/* un píxel de VRAM (vx,vy) con valor v15 a la capa (si cae en la ventana visible): bloque R x R, opaco */
static void px_put(VrLayer2D *L, int vx, int vy, uint16_t v15, int mchk, int mset, int skip0) {
    if (skip0 && !v15) return;
    int nx = (vx - L->ox) & (VW - 1), ny = (vy - L->oy) & (VH - 1);
    if (nx >= L->w || ny >= L->h || nx < L->pad || nx >= L->pad + L->aw) return;
    int R = L->R;
    int c5[3] = {v15 & 31, (v15 >> 5) & 31, (v15 >> 10) & 31};
    for (int j = 0; j < R; j++) {
        VrLayerPx *p = &L->px[(size_t)(ny * R + j) * L->PW + (size_t)nx * R];
        for (int i = 0; i < R; i++, p++) {
            if (mchk && p->m) continue;
            for (int k = 0; k < 3; k++) { p->c[k] = (uint8_t)(R > 1 ? ex8(c5[k]) : c5[k]); p->d[k] = 0; }
            p->t = 0;
            p->m = (uint8_t)(((v15 >> 15) | mset) ? 1 : 0);
        }
    }
}

static int rect_hit(int ax, int ay, int aw, int ah, int bx0, int by0, int bx1, int by1) { return ax < bx1 && ax + aw > bx0 && ay < by1 && ay + ah > by0; }

static void ev_apply(VrLayer2D *L, const GpuOp *o, const GpuDrawList *l, int sink) {
    int mchk = !!(o->flags & GPU_F_MASK_CHK), mset = !!(o->flags & GPU_F_MASK_SET);
    if (o->type == GPU_OP_FILL) {
        for (int j = 0; j < o->rh; j++)
            for (int i = 0; i < o->rw; i++) {
                int vx = (o->rx + i) & (VW - 1), vy = (o->ry + j) & (VH - 1);
                L->vram[vy][vx] = o->fill15;
                if (sink == SINK_FULL) px_put(L, vx, vy, o->fill15, 0, 0, 0);
            }
    } else if (o->type == GPU_OP_UPLOAD) {
        if (!l->data || o->rw <= 0 || o->rh <= 0) return;
        for (uint32_t i = 0; i < o->data_n && i < (uint32_t)(o->rw * o->rh) && o->data_off + i < l->ndata; i++) {
            int x = (o->rx + (int)(i % (uint32_t)o->rw)) & (VW - 1), y = (o->ry + (int)(i / (uint32_t)o->rw)) & (VH - 1);
            uint16_t *p = &L->vram[y][x];
            uint16_t v = l->data[o->data_off + i];
            if (sink == SINK_FULL) px_put(L, x, y, v, mchk, mset, 0);
            if (mchk && (*p & 0x8000)) continue;
            *p = (uint16_t)(v | (mset ? 0x8000 : 0));
        }
    } else if (o->type == GPU_OP_VCOPY) {
        uint16_t line[VW];
        int ww = o->rw > VW ? VW : o->rw;
        int paint = sink == SINK_FULL;
        if (sink == SINK_PANEL) {   /* origen fuera de la zona mostrada Y destino dentro: composición fuera de pantalla copiada a la pantalla */
            int wx0 = L->ox + L->pad, wx1 = wx0 + L->aw, wy0 = L->oy, wy1 = L->oy + L->h;
            paint = !rect_hit(o->sx, o->sy, ww, o->rh, wx0, wy0, wx1, wy1) && rect_hit(o->rx, o->ry, ww, o->rh, wx0, wy0, wx1, wy1);
        }
        for (int j = 0; j < o->rh; j++) {
            int sy = (o->sy + j) & (VH - 1), dy = (o->ry + j) & (VH - 1);
            for (int i = 0; i < ww; i++) line[i] = L->vram[sy][(o->sx + i) & (VW - 1)];
            for (int i = 0; i < ww; i++) {
                int dx = (o->rx + i) & (VW - 1);
                uint16_t *p = &L->vram[dy][dx];
                if (paint) px_put(L, dx, dy, line[i], mchk, mset, sink == SINK_PANEL);
                if (mchk && (*p & 0x8000)) continue;
                *p = (uint16_t)(line[i] | (mset ? 0x8000 : 0));
            }
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
static void fill_backdrop(VrLayer2D *L, const GpuDisplay *d, const uint16_t *src) {   /* src: VRAM 1024x512 (el espejo, o la real al cerrar la lista) */
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
            if (d->depth24) { const uint8_t *ln = (const uint8_t *)(src + (size_t)py * VW); for (int k = 0; k < 3; k++) o[x * 3 + k] = ln[((d->x * 2) + xs * 3 + k) & 2047]; }
            else {
                uint16_t t = src[(size_t)py * VW + ((d->x + xs) & (VW - 1))];
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
                p->c[k] = (uint8_t)(L->hq8 ? v : v >> 3);
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

/* caja de la primitiva recortada al área de dibujo y a la VRAM: [x0,x1) x [y0,y1); 0 si queda vacía */
static int prim_bbox(const GpuOp *o, int *x0, int *y0, int *x1, int *y1) {
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
    if (a < 0) a = 0;
    if (b < 0) b = 0;
    if (c > VW) c = VW;
    if (d > VH) d = VH;
    *x0 = a; *y0 = b; *x1 = c; *y1 = d;
    return c > a && d > b;
}

/* Rasteriza la primitiva (poligono/rect/linea) con el contexto c (destino ya fijado: capa o espejo) */
static void raster_prim(Ctx *c, const GpuOp *o) {
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
        c->umin = umin; c->umax = umax; c->vmin = vmin; c->vmax = vmax;
        if (!c->vdst) {
            VrLayer2D *L = c->L;
            L->stamp++;
            if (L->stamp == 0) { memset(L->ts, 0, sizeof L->ts); memset(L->qs, 0, sizeof L->qs); L->stamp = 1; }
        }
    }
    if (o->type == GPU_OP_POLY) do_poly(c);
    else if (o->type == GPU_OP_RECT) do_rect(c);
    else do_line(c);
}

/* Fidelidad del fotograma completo frente a la imagen REAL del emulador (la VRAM al cerrar la lista, que es lo que muestra el hardware): la capa a escala 1 debe ser idéntica. */
static void fidelity(VrLayer2D *L, const GpuDisplay *d, const uint16_t *vend) {
    VrL2Info *I = &L->info;
    long n = 0, bad = 0, sum = 0;
    for (int y = 0; y < L->h; y++) {
        int py = (d->y + y) & (VH - 1);
        for (int x = L->pad; x < L->pad + L->aw && x < L->w; x++, n++) {
            uint16_t t = vend[(size_t)py * VW + ((d->x + (x - L->pad)) & (VW - 1))];
            const VrLayerPx *p = &L->px[(size_t)y * L->PW + x];
            int e0 = (t & 31) - p->c[0], e1 = ((t >> 5) & 31) - p->c[1], e2 = ((t >> 10) & 31) - p->c[2];
            if (e0 | e1 | e2) { bad++; sum += abs(e0) + abs(e1) + abs(e2); }
        }
    }
    I->fid_n = (int)n; I->fid_bad = (int)bad;
    I->fid_mean_milli = n ? (int)(sum * 1000L * 255L / (31L * 3L * n)) : 0;   /* media por canal en niveles de 8 bits (x1000) */
}

/* ---------------- verificación del panel (pantalla 3D del motor + panel 2D encima) ----------------
 * La pantalla completa se verifica contra la imagen real del emulador (fidelity). El panel no puede compararse tal cual: debajo va el mundo NATIVO. Pero la imagen del hardware es, por construcción,
 *     hardware = (mundo: todas las primitivas que NO van al panel, en orden) + (primitivas del panel encima)
 * así que se reproduce el mundo SIN las primitivas del panel (misma aritmética, espejo aparte) y se compone el panel encima con vl2d_compose5: en los píxeles que el panel toca, el resultado debe ser
 * la imagen del emulador (vram_end). Si no lo es (rasterizado distinto, una primitiva clasificada fuera del panel que se pinta DESPUÉS de una del panel, mezclas que el modelo no representa),
 * ese píxel se sustituye por el del emulador (exacto) y, si son demasiados, todo el panel pasa a ser la imagen del emulador (respaldo exacto, como en pantalla completa). */
#define VERIFY_TOL 2          /* diferencia máxima por canal (niveles de 5 bits) que se acepta: redondeos de la composición de mezclas sobre un mundo que el modelo no conoce al bit */
#define VERIFY_BAD_MIN 64     /* mínimo absoluto de píxeles del panel distintos para el respaldo de los píxeles que toca el panel */
#define VERIFY_BAD_PCT 2      /* y más de este porcentaje de los píxeles que toca el panel */
#define VERIFY_HOLD 12        /* listas que se mantiene el respaldo exacto tras un fallo (~0,5 s): sin parpadeo entre panel y respaldo */

static double l2_now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1e3 + (double)t.tv_nsec * 1e-6; }

static int world_pass(VrLayer2D *L, const GpuDrawList *l, const VrScene *sc, int aw, int h, int *lost2d) {
    if (!L->vsnap) { L->vsnap = (uint16_t *)malloc(sizeof L->vram); if (!L->vsnap) return 0; }
    size_t need = (size_t)aw * h * 3;
    if (need > L->w5_cap) { uint8_t *q = (uint8_t *)realloc(L->w5, need); if (!q) return 0; L->w5 = q; L->w5_cap = need; }
    memcpy(L->vsnap, L->vram, sizeof L->vram);
    int wx0 = l->disp.x, wx1 = l->disp.x + aw, wy0 = l->disp.y, wy1 = l->disp.y + h;
    Ctx c0;
    memset(&c0, 0, sizeof c0);
    c0.L = L; c0.S = 1; c0.cmax = 31;
    *lost2d = 0;
    for (uint32_t i = 0; i < l->nops; i++) {
        const GpuOp *o = &l->ops[i];
        if (o->type == GPU_OP_FILL || o->type == GPU_OP_UPLOAD || o->type == GPU_OP_VCOPY) { ev_apply(L, o, l, SINK_NONE); continue; }
        if (o->type != GPU_OP_POLY && o->type != GPU_OP_RECT && o->type != GPU_OP_LINE) continue;
        int bx0, by0, bx1, by1;
        if (!prim_bbox(o, &bx0, &by0, &bx1, &by1)) continue;
        int hit = bx0 < wx1 && bx1 > wx0 && by0 < wy1 && by1 > wy0;
        if (hit && sc->op_dest[i] == 2) continue;   /* primitiva del panel: no es mundo */
        if (hit && sc->op_dest[i] == 0 && (o->flags & GPU_F_TEXTURED)) {   /* informativo: 2D texturado que cae en la pantalla y no va al panel (ni a ningún filtro): si el nativo no lo dibuja, se pierde */
            int n = o->type == GPU_OP_RECT ? 1 : o->type == GPU_OP_LINE ? 2 : (o->nv == 4 ? 4 : 3), v3 = 0;
            for (int k = 0; k < n; k++) v3 |= o->v[k].vvalid;
            if (!v3) (*lost2d)++;
        }
        Ctx cm = c0;
        cm.vdst = L->vram; cm.o = o;
        cm.cx1 = o->cx1; cm.cy1 = o->cy1; cm.cx2 = o->cx2; cm.cy2 = o->cy2;
        cm.semi = !!(o->flags & GPU_F_SEMI);
        cm.mask_chk = !!(o->flags & GPU_F_MASK_CHK); cm.mask_set = !!(o->flags & GPU_F_MASK_SET);
        cm.abr = o->abr;
        raster_prim(&cm, o);
    }
    for (int y = 0; y < h; y++) {
        int py = (l->disp.y + y) & (VH - 1);
        uint8_t *q = &L->w5[(size_t)y * aw * 3];
        for (int x = 0; x < aw; x++, q += 3) {
            uint16_t t = L->vram[py][(l->disp.x + x) & (VW - 1)];
            q[0] = (uint8_t)(t & 31); q[1] = (uint8_t)((t >> 5) & 31); q[2] = (uint8_t)((t >> 10) & 31);
        }
    }
    memcpy(L->vram, L->vsnap, sizeof L->vram);
    return 1;
}

/* Compone el panel (escala 1, ya pintado) sobre el mundo y lo compara con la imagen del emulador. Sustituye los píxeles del panel que difieren, o, si son demasiados (más del VERIFY_BAD_PCT % de los
 * tocados y al menos VERIFY_BAD_MIN) o aún dura el respaldo, TODOS los píxeles que toca el panel dilatados 1 px: el resto de la capa sigue transparente (el 3D del motor, que es fiable, se ve por debajo;
 * el respaldo nunca opaca la pantalla entera). Lo que se pone en cada píxel sustituido NO es la imagen del emulador tal cual (llevaría dentro el mundo PS1, que taparía el 3D del motor):
 *   - panel opaco en ese píxel (t = 0): el color del emulador (el mundo no se ve);
 *   - panel semitransparente: la contribución del panel = emulador - mundo reproducido (con recorte), como suma / resta sobre lo que haya debajo;
 *   - mundo reproducido no fiable (fuera del panel no coincide con el emulador: en más del 5 % de lo no tocado o en el entorno 3x3 del píxel): transparente.
 * Rellena la información de verificación. Devuelve 1 si se aplicó el respaldo (la capa entera se vuelve a codificar). */
static void recover_px(VrLayerPx *p, uint16_t emu, const uint8_t w[3], int reliable, int *nsub, int *ntransp) {
    const int f[3] = {emu & 31, (emu >> 5) & 31, (emu >> 10) & 31};
    p->m = (uint8_t)(emu >> 15);
    if (p->t == 0) { for (int k = 0; k < 3; k++) { p->c[k] = (uint8_t)f[k]; p->d[k] = 0; } return; }   /* opaco: el color exacto del hardware */
    int mx = 0;
    for (int k = 0; k < 3; k++) { int e = abs(f[k] - (int)w[k]); if (e > mx) mx = e; }
    p->t = 256;   /* lo de debajo (el 3D del motor) pasa entero; el panel solo suma / resta su parte */
    if (!reliable || mx <= VERIFY_TOL) { for (int k = 0; k < 3; k++) p->c[k] = p->d[k] = 0; (*ntransp)++; return; }
    for (int k = 0; k < 3; k++) {
        int dl = f[k] - (int)w[k];
        p->c[k] = (uint8_t)(dl > 0 ? (dl > 31 ? 31 : dl) : 0);
        p->d[k] = (uint8_t)(dl < 0 ? (-dl > 31 ? 31 : -dl) * 4 : 0);
    }
    (*nsub)++;
}

static int verify_panel(VrLayer2D *L, const GpuDisplay *d, const uint16_t *vend, int lost2d) {
    VrL2Info *I = &L->info;
    int aw = L->aw, h = L->h, PW = L->PW;
    const int xw = aw < L->w ? aw : L->w;   /* columnas verificadas */
    size_t need = (size_t)(xw > 0 ? xw : 1) * (h > 0 ? h : 1);
    if (need > L->vmap_cap) { uint8_t *q = (uint8_t *)realloc(L->vmap, need); if (!q) return 0; L->vmap = q; L->vmap_cap = need; }
    uint8_t *vm = L->vmap;
    memset(vm, 0, need);
    enum { VM_COV = 1, VM_OUT = 2, VM_BAD = 4 };
    long cov = 0, bad = 0, out = 0, sum = 0;
    for (int y = 0; y < h; y++) {
        int py = (d->y + y) & (VH - 1);
        const uint8_t *wq = &L->w5[(size_t)y * aw * 3];
        for (int x = 0; x < xw; x++) {
            uint16_t t = vend[(size_t)py * VW + ((d->x + x) & (VW - 1))];
            const VrLayerPx *p = &L->px[(size_t)y * PW + x];
            int f[3] = {t & 31, (t >> 5) & 31, (t >> 10) & 31}, mxd = 0, dsum = 0;
            int covered = !(p->t == 256 && !p->c[0] && !p->c[1] && !p->c[2] && !p->d[0] && !p->d[1] && !p->d[2]);
            uint8_t o5[3];
            vl2d_compose5(p, &wq[x * 3], o5);
            for (int k = 0; k < 3; k++) { int e = abs((int)o5[k] - f[k]); if (e > mxd) mxd = e; dsum += e; }
            uint8_t *m = &vm[(size_t)y * xw + x];
            if (covered) { *m |= VM_COV; cov++; if (mxd > VERIFY_TOL) { *m |= VM_BAD; bad++; sum += dsum; } }
            else if (abs((int)wq[x * 3] - f[0]) > VERIFY_TOL || abs((int)wq[x * 3 + 1] - f[1]) > VERIFY_TOL || abs((int)wq[x * 3 + 2] - f[2]) > VERIFY_TOL) { *m |= VM_OUT; out++; }
        }
    }
    I->verify = 1; I->v_cov = (int)cov; I->v_bad = (int)bad; I->v_out = (int)out; I->v_lost2d = lost2d;
    I->v_mean_milli = bad ? (int)(sum * 1000L * 255L / (31L * 3L * bad)) : 0;
    const int trigger = bad >= VERIFY_BAD_MIN && bad * 100L > cov * VERIFY_BAD_PCT;   /* relativo al panel (más de VERIFY_BAD_PCT %) con un mínimo absoluto */
    int whole = trigger || L->fb_hold > 0;
    if (trigger) L->fb_hold = VERIFY_HOLD; else if (L->fb_hold > 0) L->fb_hold--;
    if (!whole && !bad) return 0;
    long patched = 0; int nsub = 0, ntransp = 0;
    /* el mundo reproducido solo es fiable si coincide con el hardware fuera del panel: globalmente (más del 5 % de lo no tocado distinto = el modelo no entiende esta escena) y, además, en el entorno 3x3 */
    const int world_bad = out * 100L > ((long)xw * h - cov) * 5L;
    for (int y = 0; y < h; y++) {
        int py = (d->y + y) & (VH - 1);
        const uint8_t *wq = &L->w5[(size_t)y * aw * 3];
        for (int x = 0; x < xw; x++) {
            const uint8_t m = vm[(size_t)y * xw + x];
            int sel = 0, unreliable = 0;
            if (whole) {   /* píxeles tocados por el panel dilatados 1 px */
                for (int dy = -1; dy <= 1 && !sel; dy++) for (int dx = -1; dx <= 1; dx++) {
                    int yy = y + dy, xx = x + dx;
                    if (yy < 0 || yy >= h || xx < 0 || xx >= xw) continue;
                    if (vm[(size_t)yy * xw + xx] & VM_COV) { sel = 1; break; }
                }
            } else sel = (m & VM_BAD) != 0;
            if (!sel) continue;
            unreliable = world_bad;
            for (int dy = -1; dy <= 1 && !unreliable; dy++) for (int dx = -1; dx <= 1; dx++) {
                int yy = y + dy, xx = x + dx;
                if (yy < 0 || yy >= h || xx < 0 || xx >= xw) continue;
                if (vm[(size_t)yy * xw + xx] & VM_OUT) { unreliable = 1; break; }
            }
            recover_px(&L->px[(size_t)y * PW + x], vend[(size_t)py * VW + ((d->x + x) & (VW - 1))], &wq[x * 3], !unreliable, &nsub, &ntransp);
            patched++;
        }
    }
    I->v_patched = (int)patched; I->v_sub = nsub; I->v_transp = ntransp;
    if (whole) I->fallback = 1;
    return whole;
}

#define MCHG_LISTS 16       /* listas sin menú que una celda sigue contando como «ya cambiaba» */
static void mchg_mark(VrLayer2D *L, int x0, int y0, int x1, int y1) {
    for (int cy = y0 / 8; cy <= (y1 - 1) / 8; cy++)
        for (int cx = x0 / 8; cx <= (x1 - 1) / 8; cx++) L->mchg[cy * (VW / 8) + cx] = MCHG_LISTS;
}
static int mn_find(MNew *p, int i) { while (p[i].par != i) { p[i].par = p[p[i].par].par; i = p[i].par; } return i; }
/* Ventana del selector entre las primitivas nuevas (sin rect congelado): componentes conexos (hueco < 8 px entre cajas) y gana el de mayor área que tenga marco o fondo opaco (sin textura ni
 * GPU_F_SEMI); si ninguno lo tiene, el de mayor área de todos (la mano nunca recibe la capa entera). Devuelve el nº de primitivas del componente y su caja en *x0.. (0 y caja intacta si no hay nuevas). */
static int menu_pick(const VrLayer2D *L, MNew *p, size_t n, float *x0, float *y0, float *x1, float *y1) {
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            if (p[i].x0 < p[j].x1 + 8.0f && p[i].x1 > p[j].x0 - 8.0f && p[i].y0 < p[j].y1 + 8.0f && p[i].y1 > p[j].y0 - 8.0f) {
                int a = mn_find(p, (int)i), b = mn_find(p, (int)j);
                if (a != b) p[b].par = a;
            }
    for (size_t i = 0; i < n; i++) {   /* celdas de 8 px que cubre cada primitiva (antes de fundir las cajas) y cuántas ya cambiaban antes de abrir el menú */
        int r = mn_find(p, (int)i);
        for (int cy = (int)p[i].y0 / 8; cy <= ((int)p[i].y1 - 1) / 8; cy++)
            for (int cx = (int)p[i].x0 / 8; cx <= ((int)p[i].x1 - 1) / 8; cx++) { p[r].tot++; if (L->mchg[cy * (VW / 8) + cx]) p[r].chg++; }
    }
    for (size_t i = 0; i < n; i++) {   /* los datos del componente se acumulan en su raíz (las raíces no se leen como hojas: solo se escriben las raíces) */
        int r = mn_find(p, (int)i);
        if (r == (int)i) continue;
        MNew *m = &p[r];
        if (p[i].x0 < m->x0) m->x0 = p[i].x0;
        if (p[i].y0 < m->y0) m->y0 = p[i].y0;
        if (p[i].x1 > m->x1) m->x1 = p[i].x1;
        if (p[i].y1 > m->y1) m->y1 = p[i].y1;
        m->cnt += p[i].cnt; m->op |= p[i].op;
    }
    int best = -1; float ba = 0;
    for (int pass = 0; pass < 2 && best < 0; pass++)   /* pasada 0: sin los componentes que ya cambiaban (más de la mitad de sus celdas: radar, créditos que se desplazan); pasada 1 (respaldo): todos */
        for (size_t i = 0; i < n; i++) {
            if (p[i].par != (int)i || (pass == 0 && p[i].chg * 2 > p[i].tot)) continue;
            float a = (p[i].x1 - p[i].x0) * (p[i].y1 - p[i].y0);
            if (best < 0 || (p[i].op && !p[best].op) || (p[i].op == p[best].op && a > ba)) { best = (int)i; ba = a; }
        }
    if (best < 0) return 0;
    *x0 = p[best].x0; *y0 = p[best].y0; *x1 = p[best].x1; *y1 = p[best].y1;
    return p[best].cnt;
}

int vl2d_run(VrLayer2D *L, const GpuDrawList *l, const VrScene *sc, int full) {
    if (!L || !l) return 0;
    L->valid = 0; L->menu_ok = 0;
    const int verify_req = full == VL_PANEL_VERIFIED;   /* panel con verificación contra la imagen del emulador (pantalla 3D del motor + panel) */
    if (verify_req) full = 0;
    if (!verify_req) L->fb_hold = 0;
    /* La VRAM de partida es la REAL del emulador al inicio de la lista (vram_start, cada lista) o, si el productor no la da, la instantánea de la primera lista/resincronización
     * (vram_init) más los eventos: así una subida o un relleno que no pasen por la lista (DMA, copias, escrituras del juego) nunca dejan texels basura en fuentes y CLUT. */
    { const uint16_t *v0 = l->vram_start ? l->vram_start : l->vram_init; if (v0) { memcpy(L->vram, v0, sizeof L->vram); L->have_vram = 1; } }
    /* panel: hace falta la clasificación de la escena (op_dest); fotograma completo: no (dibuja TODAS las primitivas), así que también hay capa exacta si la escena no se pudo ensamblar */
    int draw = L->have_vram && (full || (sc && sc->op_dest && sc->n_op_dest >= l->nops));
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
    if (!full) { aw = l->disp.aw > 0 && l->disp.aw < w ? l->disp.aw : w; }   /* panel: rectángulo nominal, pero lo que el rango deja fuera (basura de VRAM) no se dibuja */
    if (full && !l->disp.enabled) aw = 0;   /* pantalla apagada: negro, como el fotograma normal */
    int ux0 = 0, uy0 = 0, ux1 = 0, uy1 = 0;   /* sucio de esta lista */
    /* Modo original: el rasterizado es el EXACTO de la GPU original (escala 1: 5 bits, tramado, mismo muestreo de texels) y la salida lo replica S x S (vecino más próximo
     * a escala entera): el texto sale idéntico en forma y color y nítido. Excepción: fotograma completo en 24 bits (vídeo), que necesita los 8 bits por canal. */
    if (L->smooth == VL_SMOOTH_ORIGINAL && !(full && l->disp.depth24)) R = 1;
    const int hq8 = R > 1 || (full && l->disp.depth24);   /* vídeo de 24 bits a R = 1 (raster nativo del visor): se conservan los 8 bits por canal */
    L->hq8 = hq8;
    if (draw) {
        size_t nout = (size_t)w * S * h * S, npx = (size_t)w * R * h * R;
        if (nout > (size_t)6 * 1024 * 6 * 512) draw = 0;   /* tope de memoria (con escala 6 una pantalla de 640x480 son 11 Mpx) */
        else if (!ensure(L, npx, nout)) draw = 0;
    }
    /* Fotograma completo a escala 1 («original», 15 bits): la VRAM espejo es la ÚNICA fuente de verdad. Se ejecuta la lista entera sobre ella EN ORDEN (todas las primitivas con la
     * aritmética de gpu.c, los rellenos, subidas y copias, los dibujos a zonas fuera de la mostrada que luego se usan de textura o se copian) y al final se lee la zona mostrada: es
     * exactamente lo que muestra el hardware. Antes la zona mostrada se copiaba al INICIO y las primitivas se pintaban encima: los borrados y copias de fondo (rellenos, VRAM->VRAM) que
     * quitan lo dibujado antes no se aplicaban (restos / textos duplicados), y lo compuesto fuera de pantalla se leía con el estado final de la lista. */
    int vmode = draw && full && R == 1;
    int sink = !draw || vmode ? SINK_NONE : full ? SINK_FULL : SINK_PANEL;
    /* verificación del panel: solo a escala 1 «original» de 15 bits, con la imagen real al cerrar la lista y la clasificación de la escena */
    int verify = verify_req && draw && !full && R == 1 && l->vram_end && !l->disp.depth24 && sc && sc->op_dest && sc->n_op_dest >= l->nops;
    int lost2d = 0;
    double v_t0 = verify ? l2_now_ms() : 0.0, v_ms = 0.0;
    if (verify && !world_pass(L, l, sc, aw, h, &lost2d)) verify = 0;
    if (verify) v_ms = l2_now_ms() - v_t0;
    if (draw) {
        int key = (hq8 << 12) | (R << 8) | (S << 4) | L->smooth;
        int zone = L->cur_scale != key || L->w != w || L->h != h || L->pad != pad || L->aw != aw;   /* otra zona mostrada (tamaño/escala); el cambio de búfer (ox, oy) no invalida el rect del menú: va relativo a ella */
        const int geom = L->w != w || L->h != h || L->pad != pad || L->aw != aw;   /* otro tamaño de zona (no la escala) */
        if (zone || L->ox != oxe || L->oy != l->disp.y) {
            L->w = w; L->h = h; L->ox = oxe; L->oy = l->disp.y; L->pad = pad; L->aw = aw; L->LW = w * S; L->LH = h * S;
            L->R = R; L->PW = w * R; L->PH = h * R;
            L->cur_scale = key;
            if (geom) { menu_forget(L); memset(L->mchg, 0, sizeof L->mchg); }   /* el rect conservado y las celdas que cambiaban son de otra zona mostrada (otro tamaño); la ESCALA no cuenta: rect, celdas y claves van en px nativos / UV, y la escala cambia sola bajo carga */
            L->dx0 = L->dy0 = 0; L->dx1 = L->PW; L->dy1 = L->PH;   /* reinicialización completa */
        }
        blank(L, L->dx0, L->dy0, L->dx1, L->dy1);
        L->dx0 = L->dy0 = L->dx1 = L->dy1 = 0;
        ux0 = L->PW; uy0 = L->PH; ux1 = 0; uy1 = 0;
        if (full) {
            if (!vmode) fill_backdrop(L, &l->disp, &L->vram[0][0]);
            ux0 = 0; uy0 = 0; ux1 = L->PW; uy1 = L->PH;
        }
    }
    memset(&L->info, 0, sizeof L->info);
    L->mcur_n = 0; L->mnp_n = 0;
    if (draw && !full && !L->menu_on) for (size_t i = 0; i < sizeof L->mchg; i++) if (L->mchg[i]) L->mchg[i]--;   /* lista sin menú: envejece las celdas que cambiaban */
    float mx0 = 1e9f, my0 = 1e9f, mx1 = -1e9f, my1 = -1e9f; int mnew = 0;   /* caja (px nativos) y cuenta de las primitivas del panel nuevas respecto al base */
    const int mhave = L->menu_on && L->menu_have && L->menu_set;                                  /* asentado: rect fijo hasta cerrar; antes se reelige en cada lista y crece por unión (< 8 px) */
    const int had_rect = L->menu_have; const float old_px[4] = { L->menu_px[0], L->menu_px[1], L->menu_px[2], L->menu_px[3] };
    if (mhave) { mx0 = L->menu_px[0]; my0 = L->menu_px[1]; mx1 = L->menu_px[2]; my1 = L->menu_px[3]; }
    L->info.fw = w; L->info.h = h; L->info.aw = aw; L->info.pad = pad; L->info.scale = S; L->info.raster = R; L->info.smooth = L->smooth; L->info.full = full;
    L->info.used_start = l->vram_start != NULL; L->info.used_end = l->vram_end != NULL;
    L->info.verify_req = verify_req;
    /* zona mostrada (visible) en VRAM: solo lo que cae aquí se pinta en la capa; el resto de dibujos solo actualiza la VRAM espejo (se usa luego como textura o se copia) */
    int wx0 = l->disp.x, wx1 = l->disp.x + (aw > 0 ? aw : 0), wy0 = l->disp.y, wy1 = l->disp.y + h;
    Ctx c0;
    memset(&c0, 0, sizeof c0);
    c0.L = L; c0.S = R; c0.hq = hq8; c0.smooth = L->smooth; c0.cmax = c0.hq ? 255 : 31;
    for (uint32_t i = 0; i < l->nops; i++) {
        const GpuOp *o = &l->ops[i];
        switch (o->type) {
        case GPU_OP_FILL: case GPU_OP_UPLOAD: case GPU_OP_VCOPY: ev_apply(L, o, l, sink); break;
        case GPU_OP_POLY: case GPU_OP_RECT: case GPU_OP_LINE: {
            if (!draw) break;
            int bx0, by0, bx1, by1;
            if (!prim_bbox(o, &bx0, &by0, &bx1, &by1)) break;
            int inside = bx0 >= wx0 && bx1 <= (full ? wx1 : l->disp.x + w) && by0 >= wy0 && by1 <= wy1;   /* destino enteramente en la zona mostrada (panel: el rectángulo nominal) */
            int hit = bx0 < wx1 && bx1 > wx0 && by0 < wy1 && by1 > wy0;           /* destino con alguna parte en la zona mostrada */
            if (vmode || !inside) {   /* actualiza la VRAM espejo (todo en fotograma completo; en el panel solo lo que sale de la zona mostrada: render a textura, búfer alterno) */
                Ctx cm = c0;
                cm.S = 1; cm.hq = 0; cm.smooth = 0; cm.cmax = 31;
                cm.vdst = L->vram; cm.o = o;
                cm.cx1 = o->cx1; cm.cy1 = o->cy1; cm.cx2 = o->cx2; cm.cy2 = o->cy2;
                cm.semi = !!(o->flags & GPU_F_SEMI);
                cm.mask_chk = !!(o->flags & GPU_F_MASK_CHK); cm.mask_set = !!(o->flags & GPU_F_MASK_SET);
                cm.abr = o->abr;
                int skip = 0;
                if (o->fbk && o->type != GPU_OP_LINE && !vmode) {   /* panel: la VRAM espejo no tiene lo dibujado en la zona mostrada: texel único resuelto por la captura */
                    if (!o->fbt || !(o->flags & GPU_F_TEXTURED)) skip = 1;
                    cm.fixed = 1;
                }
                if (!skip) raster_prim(&cm, o);
                if (!vmode) L->info.mirror_prims++;
            }
            if (vmode) { L->info.prims++; if (o->flags & GPU_F_MASK_CHK) L->info.mask_prims++; break; }
            if (!hit) break;                                   /* no toca la zona mostrada: no se pinta en la capa */
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
            int menu_skip = 0;   /* ventana asentada: la capa omite lo que ya había antes de abrir el menú y lo nuevo que no toca el rect (créditos, vida, texto) */
            if (!full) {
                uint64_t mk = op_key(o, L->ox, L->oy);
                key_push(&L->mcur, &L->mcur_n, &L->mcur_cap, mk);
                if (mhave) {
                    int na, nb, ne, nf;
                    op_box(o, 1, &na, &nb, &ne, &nf);
                    na -= L->ox; ne -= L->ox; nb -= L->oy; nf -= L->oy;
                    if (na < 0) na = 0;
                    if (nb < 0) nb = 0;
                    if (ne > w) ne = w;
                    if (nf > h) nf = h;
                    /* solo se omite lo que ya estaba en la base o lo que no toca el rect; una primitiva nueva que lo corte se pinta (el panel ya recorta por UV) */
                    menu_skip = key_in(L->mbase, L->mbase_n, mk) || ne <= na || nf <= nb || (float)ne <= mx0 - MENU_KEEP_PX || (float)na >= mx1 + MENU_KEEP_PX || (float)nf <= my0 - MENU_KEEP_PX || (float)nb >= my1 + MENU_KEEP_PX;
                }
                if ((L->menu_on || L->mbase_n) && !key_in(L->mbase, L->mbase_n, mk)) {   /* nueva respecto al base: con menú, candidata a ventana; sin menú, la celda «ya cambiaba» */
                    int na, nb, ne, nf;
                    op_box(o, 1, &na, &nb, &ne, &nf);
                    na -= L->ox; ne -= L->ox; nb -= L->oy; nf -= L->oy;
                    if (na < 0) na = 0;
                    if (nb < 0) nb = 0;
                    if (ne > w) ne = w;
                    if (nf > h) nf = h;
                    if (!L->menu_on) { if (ne > na && nf > nb) mchg_mark(L, na, nb, ne, nf); }
                    else if ((float)nf <= MENU_BAND_V * (float)h) { /* franja de créditos/vida: no es la ventana */ }
                    else if (ne > na && nf > nb && !mhave) {   /* sin rect conservado: se guardan todas y la ventana se elige al final (componentes conexos) */
                        if (L->mnp_n == L->mnp_cap) {
                            size_t cn = L->mnp_cap ? L->mnp_cap * 2 : 128;
                            MNew *q = (MNew *)realloc(L->mnp, cn * sizeof *q);
                            if (q) { L->mnp = q; L->mnp_cap = cn; }
                        }
                        if (L->mnp_n < L->mnp_cap) {
                            MNew *m = &L->mnp[L->mnp_n];
                            m->x0 = (float)na; m->y0 = (float)nb; m->x1 = (float)ne; m->y1 = (float)nf;
                            m->par = (int)L->mnp_n; m->cnt = 1; m->tot = m->chg = 0; m->op = !(o->flags & (GPU_F_TEXTURED | GPU_F_SEMI));
                            L->mnp_n++;
                        }
                    }
                }
                int a, b, e, f;
                op_box(o, R, &a, &b, &e, &f);
                a -= L->ox * R; e -= L->ox * R; b -= L->oy * R; f -= L->oy * R;
                if (a < ux0) ux0 = a;
                if (b < uy0) uy0 = b;
                if (e > ux1) ux1 = e;
                if (f > uy1) uy1 = f;
            }
            if (menu_skip) break;
            raster_prim(&c, o);
            break;
        }
        default: break;
        }
    }
    if (!draw) return 0;
    if (!full) {
        if (L->menu_on && !mhave) {
            mnew = menu_pick(L, L->mnp, L->mnp_n, &mx0, &my0, &mx1, &my1);
            /* La ventana se abre con animación (crece de lista en lista) y la elección de cada lista puede ser parcial: si la elegida toca el rect anterior (< 8 px) se funde con él (solo crece, nunca oscila);
             * si es otra zona (una barra animada), la sustituye. */
            if (mnew > 0 && L->menu_have && mx0 < L->menu_px[2] + 8.0f && mx1 > L->menu_px[0] - 8.0f && my0 < L->menu_px[3] + 8.0f && my1 > L->menu_px[1] - 8.0f) {
                if (L->menu_px[0] < mx0) mx0 = L->menu_px[0];
                if (L->menu_px[1] < my0) my0 = L->menu_px[1];
                if (L->menu_px[2] > mx1) mx1 = L->menu_px[2];
                if (L->menu_px[3] > my1) my1 = L->menu_px[3];
            }
        }
        if (!L->menu_on) {   /* lista sin menú: renueva el base */
            qsort(L->mcur, L->mcur_n, sizeof *L->mcur, key_cmp);
            uint64_t *t = L->mbase; size_t tc = L->mbase_cap;
            L->mbase = L->mcur; L->mbase_n = L->mcur_n; L->mbase_cap = L->mcur_cap;
            L->mcur = t; L->mcur_n = 0; L->mcur_cap = tc;
        } else if (L->mbase_n && (mnew > 0 || L->menu_have) && w > 0 && h > 0) {
            if ((mnew > 0 || mhave) && (double)(mx1 - mx0) * (double)(my1 - my0) <= 0.6 * (double)w * (double)h) { L->menu_have = 1; L->menu_px[0] = mx0; L->menu_px[1] = my0; L->menu_px[2] = mx1; L->menu_px[3] = my1; }
            if (L->menu_have) {   /* sin primitivas nuevas en esta lista (o con un crecimiento excesivo) se conserva el último rect válido */
                L->menu_ok = 1;
                L->menu_uv[0] = L->menu_px[0] / (float)w; L->menu_uv[1] = L->menu_px[1] / (float)h; L->menu_uv[2] = L->menu_px[2] / (float)w; L->menu_uv[3] = L->menu_px[3] / (float)h;
            }
        }
        if (L->menu_on && !L->menu_set) {   /* fase de crecimiento: se asienta tras MENU_CALM_LISTS listas seguidas sin crecer (o al agotar MENU_GROW_MAX) */
            int grew = 0;
            if (L->menu_have) {
                if (!had_rect) grew = 1;
                else for (int c = 0; c < 4; c++) if (fabsf(L->menu_px[c] - old_px[c]) > MENU_GROW_EPS) grew = 1;
            }
            L->menu_n++;
            if (!L->menu_have || grew) L->menu_calm = 0; else L->menu_calm++;
            if ((L->menu_have && L->menu_calm >= MENU_CALM_LISTS) || L->menu_n >= MENU_GROW_MAX) L->menu_set = 1;
        }
        L->info.menu_new = mnew; L->info.menu_base = (int)L->mbase_n;
    }
    if (vmode) {
        fill_backdrop(L, &l->disp, &L->vram[0][0]);
        if (l->vram_end && !l->disp.depth24) {   /* (24 bits: el marco no es la lectura de 15 bits) medida de fidelidad contra la imagen real; si la capa no es idéntica, se usa la imagen exacta del emulador (respaldo) */
            fidelity(L, &l->disp, l->vram_end);
            if (L->info.fid_bad > 0) { fill_backdrop(L, &l->disp, l->vram_end); L->info.fallback = 1; }
        }
    }
    if (verify) {
        double t1 = l2_now_ms();
        if (verify_panel(L, &l->disp, l->vram_end, lost2d)) { ux0 = 0; uy0 = 0; ux1 = L->PW; uy1 = L->PH; }   /* respaldo exacto: toda la capa cambia */
        L->info.v_ms = v_ms + (l2_now_ms() - t1);
    }
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

/* Comparación de una capa RGBA (escala `scale`, el pixel nativo (x,y) se toma en (x*scale, y*scale)) con una imagen de referencia RGBA de rw x rh (frame.png del volcado o el marco de la GPU). */
int vl2d_compare(const uint8_t *rgba, int w, int h, int scale, const uint8_t *ref, int rw, int rh, VrL2Diff *out) {
    if (out) memset(out, 0, sizeof *out);
    if (!rgba || !ref || scale < 1 || rw <= 0 || rh <= 0 || w < rw * scale || h < rh * scale) return 0;
    long bad = 0, sum = 0;
    int mx = 0;
    for (int y = 0; y < rh; y++)
        for (int x = 0; x < rw; x++) {
            const uint8_t *p = &rgba[((size_t)(y * scale) * w + (size_t)(x * scale)) * 4], *q = &ref[((size_t)y * rw + x) * 4];
            int d = 0;
            for (int k = 0; k < 3; k++) { int e = abs((int)p[k] - (int)q[k]); d += e; if (e > mx) mx = e; }
            if (d) bad++;
            sum += d;
        }
    if (out) {
        out->n = (long)rw * rh; out->bad = bad; out->max = mx;
        out->mean = (double)sum / (3.0 * (double)out->n);
        out->pct = 100.0 * (double)bad / (double)out->n;
    }
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
