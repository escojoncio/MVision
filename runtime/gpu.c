/* GPU PS1 por software: VRAM 1024x512x16, comandos GP0/GP1, DMA canal 2 y HLE de libgpu.
 * Escrito a partir de la documentación pública del hardware. Sirve de referencia (oráculo)
 * para el renderer Metal: prioriza exactitud y claridad sobre velocidad. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "gpu.h"
#include "hle.h"

uint16_t gpu_vram[GPU_VRAM_H][GPU_VRAM_W];
GpuStats gpu_stats;

/* ---------------- estado ---------------- */
static struct {
    /* E1 */
    int tx, ty, abr, tp, dither, draw_disp, tex_off, flipx, flipy;
    uint32_t e1;
    /* E2 */
    int twmx, twmy, twox, twoy;
    uint32_t e2;
    /* E3/E4/E5 */
    int cx1, cy1, cx2, cy2, ox, oy;
    uint32_t e3, e4, e5;
    /* E6 */
    int mask_set, mask_chk;
    /* GP1 */
    int disp_off, dma_dir, dx, dy, hx1, hx2, vy1, vy2;
    uint32_t mode;
    int irq;
    uint32_t read_latch;
} g;

/* Acumulador de comandos GP0 */
static uint32_t fifo[16];
static int fifo_n, fifo_need;
static enum { ST_CMD, ST_POLYLINE, ST_UPLOAD, } st;
/* Transferencias CPU<->VRAM */
static struct { int x, y, w, h, i, n; } xfer, rd;
static int rd_active;

static inline int sext11(uint32_t v) { return (int)(v << 21) >> 21; }

static void cap_on_reset(void);

void gpu_reset(void) {
    memset(&g, 0, sizeof g);
    g.disp_off = 1;
    g.hx1 = 0x200; g.hx2 = 0x200 + 256 * 10;
    g.vy1 = 0x10; g.vy2 = 0x10 + 240;
    fifo_n = 0; st = ST_CMD; rd_active = 0;
    cap_on_reset();
}

/* ---------------- escritura de píxel ---------------- */
static const int dither_m[4][4] = {
    {-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

static inline int clip_ok(int x, int y) {
    return x >= g.cx1 && x <= g.cx2 && y >= g.cy1 && y <= g.cy2;
}

/* r,g,b en escala de 8 bits (0..255, ya modulados). semi: aplicar mezcla. mbit: bit 15 de origen. */
static inline void plot(int x, int y, int r, int gg, int b, int dith, int semi, int mbit) {
    if (!clip_ok(x, y)) return;
    if (!g.draw_disp && 0) return; /* bit 10: el hardware no evita dibujar; ignorado */
    uint16_t *p = &gpu_vram[y & 511][x & 1023];
    if (g.mask_chk && (*p & 0x8000)) return;
    if (dith) {
        int d = dither_m[y & 3][x & 3];
        r += d; gg += d; b += d;
    }
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    gg = gg < 0 ? 0 : gg > 255 ? 255 : gg;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    r >>= 3; gg >>= 3; b >>= 3;
    if (semi) {
        int br = *p & 31, bg = (*p >> 5) & 31, bb = (*p >> 10) & 31;
        switch (g.abr) {
        case 0: r = (br + r) >> 1; gg = (bg + gg) >> 1; b = (bb + b) >> 1; break;
        case 1: r += br; gg += bg; b += bb; break;
        case 2: r = br - r; gg = bg - gg; b = bb - b; break;
        case 3: r = br + (r >> 2); gg = bg + (gg >> 2); b = bb + (b >> 2); break;
        }
        r = r < 0 ? 0 : r > 31 ? 31 : r;
        gg = gg < 0 ? 0 : gg > 31 ? 31 : gg;
        b = b < 0 ? 0 : b > 31 ? 31 : b;
    }
    *p = (uint16_t)(r | (gg << 5) | (b << 10) | ((mbit | g.mask_set) ? 0x8000 : 0));
}

/* ---------------- texturas ---------------- */
typedef struct { int tx, ty, tp, cx, cy; } TexCtx;

static inline uint16_t texel(const TexCtx *t, int u, int v) {
    u &= 255; v &= 255;
    u = (u & ~(g.twmx * 8)) | ((g.twox & g.twmx) * 8);
    v = (v & ~(g.twmy * 8)) | ((g.twoy & g.twmy) * 8);
    int y = (t->ty + v) & 511;
    switch (t->tp) {
    case 0: {
        uint16_t w = gpu_vram[y][(t->tx + (u >> 2)) & 1023];
        int idx = (w >> ((u & 3) * 4)) & 15;
        return gpu_vram[t->cy & 511][(t->cx + idx) & 1023];
    }
    case 1: {
        uint16_t w = gpu_vram[y][(t->tx + (u >> 1)) & 1023];
        int idx = (w >> ((u & 1) * 8)) & 255;
        return gpu_vram[t->cy & 511][(t->cx + idx) & 1023];
    }
    default:
        return gpu_vram[y][(t->tx + u) & 1023];
    }
}

/* Fragmento texturizado: devuelve 0 si el texel es transparente (0x0000). */
static inline void tex_plot(const TexCtx *t, int x, int y, int u, int v, int r, int gg, int b,
                            int raw, int semi, int dith) {
    uint16_t c = texel(t, u, v);
    if (!c) return;
    int tr = c & 31, tg = (c >> 5) & 31, tb = (c >> 10) & 31;
    int R, G, B;
    if (raw) { R = tr << 3; G = tg << 3; B = tb << 3; dith = 0; }
    else { R = (tr * r) >> 4; G = (tg * gg) >> 4; B = (tb * b) >> 4; }
    plot(x, y, R, G, B, dith, semi && (c & 0x8000), c >> 15);
}

static void set_texpage(uint32_t v) {
    /* bits 0-8 y 11 comunes entre E1 y el campo tpage de los polígonos */
    g.tx = (v & 15) * 64;
    g.ty = ((v >> 4) & 1) * 256;
    g.abr = (v >> 5) & 3;
    g.tp = (v >> 7) & 3;
    g.tex_off = (v >> 11) & 1;
}

/* ---------------- triángulos ---------------- */
typedef struct { int x, y, r, g, b, u, v; } Vtx;

/* ---------------- captura de primitivas (ver gpu.h) ---------------- */
static struct {
    int on, verify;
    GpuDrawList cur, out, q[GPU_CAP_QDEPTH];
    int qh, qn;
    uint32_t seq, nframes, dropped;
    int need_snap, up_active, pending_close, disp_open, vb_noflip;
    uint32_t up_idx;
    int last_dx, last_dy;
    GpuFloatHook hook;
    GpuHpHook hphook;
    GpuHpResolve hpres;
} cap;

static void cap_list_reset(GpuDrawList *l) {
    l->nops = l->ndata = 0;
    free(l->vram_init); l->vram_init = NULL;
    free(l->vram_after); l->vram_after = NULL;
}

static void cap_begin(void) {
    cap_list_reset(&cap.cur);
    if (cap.need_snap) {
        cap.cur.vram_init = malloc(sizeof gpu_vram);
        if (cap.cur.vram_init) memcpy(cap.cur.vram_init, gpu_vram, sizeof gpu_vram);
        cap.need_snap = 0;
    }
}

static GpuOp *cap_new(int type) {
    GpuDrawList *l = &cap.cur;
    if (l->nops == l->cap_ops) {
        l->cap_ops = l->cap_ops ? l->cap_ops * 2 : 1024;
        l->ops = realloc(l->ops, l->cap_ops * sizeof *l->ops);
        if (!l->ops) { fprintf(stderr, "[gpu] captura: sin memoria\n"); abort(); }
    }
    GpuOp *o = &l->ops[l->nops++];
    memset(o, 0, sizeof *o);
    o->type = (uint8_t)type;
    o->seq = cap.seq++;
    o->abr = (uint8_t)g.abr; o->tp = (uint8_t)g.tp;
    o->twmx = (uint8_t)g.twmx; o->twmy = (uint8_t)g.twmy; o->twox = (uint8_t)g.twox; o->twoy = (uint8_t)g.twoy;
    o->tx = (uint16_t)g.tx; o->ty = (uint16_t)g.ty;
    o->dox = (int16_t)g.ox; o->doy = (int16_t)g.oy;
    o->cx1 = (int16_t)g.cx1; o->cy1 = (int16_t)g.cy1; o->cx2 = (int16_t)g.cx2; o->cy2 = (int16_t)g.cy2;
    o->flags = (uint16_t)((g.mask_set ? GPU_F_MASK_SET : 0) | (g.mask_chk ? GPU_F_MASK_CHK : 0));
    cap.disp_open = 0;
    return o;
}

static void cap_close(int reason) {
    if (cap.cur.nops == 0) { cap.disp_open = 0; return; }
    if (cap.up_active) { cap.pending_close = reason; return; } /* subida en curso: cerrar al terminarla */
    GpuDrawList *c = &cap.cur;
    gte_hp_frame(); /* envejece la tabla de posiciones float de la GTE (no hace nada si está apagada) */
    if (cap.qn == GPU_CAP_QDEPTH) {
        /* Nadie consume. Perder solo la más antigua dejaría una cola sin vram_init y con un hueco (VRAM del
         * consumidor divergente hasta el siguiente resincronizado): se descarta TODA la cola y también esta lista
         * (no lleva instantánea); la siguiente que se cierre trae vram_init. */
        for (int i = 0; i < cap.qn; i++) cap_list_reset(&cap.q[(cap.qh + i) % GPU_CAP_QDEPTH]);
        cap.dropped += (uint32_t)cap.qn + 1; cap.nframes++;
        cap.qh = cap.qn = 0; cap.need_snap = 1;
        cap_begin();
        cap.disp_open = 0; cap.vb_noflip = 0; cap.pending_close = 0;
        return;
    }
    c->index = cap.nframes++;
    c->host_frame = hle_frame_count();
    c->reason = reason;
    gpu_get_display(&c->disp);
    if (cap.verify) {
        c->vram_after = malloc(sizeof gpu_vram);
        if (c->vram_after) memcpy(c->vram_after, gpu_vram, sizeof gpu_vram);
    }
    GpuDrawList *slot = &cap.q[(cap.qh + cap.qn) % GPU_CAP_QDEPTH];
    GpuDrawList tmp = *slot; *slot = *c; *c = tmp;
    cap.qn++;
    cap_begin();
    cap.disp_open = 1; cap.vb_noflip = 0; cap.pending_close = 0;
}

/* Tras un cierre, las escrituras GP1 de la misma ráfaga (modo, rango vertical...) retocan el display. */
static void cap_refresh_disp(void) {
    if (cap.disp_open && cap.qn) gpu_get_display(&cap.q[(cap.qh + cap.qn - 1) % GPU_CAP_QDEPTH].disp);
}

static GpuHpInfo cap_proj;
static uint32_t cap_seq[4];
/* Datos básicos del vértice (el enlace con la GTE se resuelve después, por primitiva: cap_resolve). */
static void cap_vtx(GpuVtx *o, const Vtx *v, int tex) {
    o->x = (int16_t)v->x; o->y = (int16_t)v->y;
    o->r = (uint8_t)v->r; o->g = (uint8_t)v->g; o->b = (uint8_t)v->b;
    if (tex) { o->u = (uint8_t)v->u; o->v = (uint8_t)v->v; }
}
/* Enlace con la GTE de los nv vértices de una primitiva. La GTE produce SXY con OFX/OFY; el juego lo copia a la primitiva y el
 * GPU suma después el offset de dibujo E5: la clave es la coordenada SIN offset (v->x - g.ox). Con la alta precisión se asocia
 * la primitiva entera (gte_hp_resolve: desambigua vértices saturados por cercanía en la secuencia de salida de la GTE); cada
 * vértice sale con vvalid/vflags/vx,vy,vz y, si no está saturado ni detrás de la cámara, además fvalid/fx/fy/fz. */
static void cap_resolve(GpuVtx *o, const Vtx *v, int nv) {
    if (cap.hpres) {
        int sx[4], sy[4]; GpuHpInfo h[4]; uint8_t ok[4];
        for (int i = 0; i < nv; i++) { sx[i] = v[i].x - g.ox; sy[i] = v[i].y - g.oy; }
        cap.hpres(nv, sx, sy, h, ok);
        for (int i = 0; i < nv; i++) {
            if (!ok[i]) continue;
            o[i].vvalid = 1; o[i].vflags = h[i].flags;
            o[i].vx = h[i].vx; o[i].vy = h[i].vy; o[i].vz = h[i].vz; o[i].fz = h[i].fz;
            o[i].fx = h[i].fx + (float)g.ox; o[i].fy = h[i].fy + (float)g.oy;
            o[i].fvalid = !(h[i].flags & (GPU_VF_SAT | GPU_VF_BEHIND));
            cap_proj = h[i]; cap_seq[i] = h[i].seq;
        }
    } else if (cap.hphook) {
        for (int i = 0; i < nv; i++) {
            GpuHpInfo h;
            if (cap.hphook(v[i].x - g.ox, v[i].y - g.oy, &h)) {
                o[i].fvalid = o[i].vvalid = 1; o[i].fx = h.fx + (float)g.ox; o[i].fy = h.fy + (float)g.oy; o[i].fz = h.fz;
                o[i].vx = h.vx; o[i].vy = h.vy; o[i].vz = h.vz;
                cap_proj = h;
            }
        }
    } else if (cap.hook) {
        for (int i = 0; i < nv; i++) {
            float fx, fy, fz;
            if (cap.hook(v[i].x - g.ox, v[i].y - g.oy, &fx, &fy, &fz)) {
                o[i].fvalid = 1; o[i].fx = fx + g.ox; o[i].fy = fy + g.oy; o[i].fz = fz;
            }
        }
    }
}

/* Sprite de 4 vértices del que el juego proyectó solo dos esquinas diagonales (0,3 o 1,2) y construyó las otras dos en
 * pantalla (rectángulo alineado, o girado: son carteles de cara a la cámara). Se exige: las dos esquinas normales con seq
 * cercanos (misma rutina; una coincidencia espuria de dos vértices de HUD con seq a <=256 es del orden de 1e-4) y las otras dos
 * a distancia razonable del centro. Se les da vista a la profundidad media de las dos esquinas (GPU_VF_DERIVED). */
static void cap_derive(GpuVtx *q) {
    for (int d = 0; d < 2; d++) {
        int a = d ? 1 : 0, b = d ? 2 : 3, c1 = d ? 0 : 1, c2 = d ? 3 : 2;
        GpuVtx *A = &q[a], *B = &q[b];
        if (!A->fvalid || !B->fvalid || (A->vflags | B->vflags) & GPU_VF_AMBIG || q[c1].vvalid || q[c2].vvalid || cap_proj.h == 0.0f) continue; /* AMBIG: asociación dudosa, típica de coincidencias de HUD */
        if (cap_seq[a] - cap_seq[b] + 256u > 512u) continue;
        float za = A->vz, zb = B->vz;
        if (za <= 0 || zb <= 0 || fabsf(za - zb) > 0.5f * (za > zb ? za : zb)) continue;
        float mx = 0.5f * (A->fx + B->fx), my = 0.5f * (A->fy + B->fy);
        float lim = 1.5f * hypotf(A->fx - B->fx, A->fy - B->fy) + 4.0f;
        if (hypotf(q[c1].x - mx, q[c1].y - my) > lim || hypotf(q[c2].x - mx, q[c2].y - my) > lim) continue;
        float z = 0.5f * (za + zb);
        GpuVtx *cs[2] = {&q[c1], &q[c2]};
        for (int k = 0; k < 2; k++) {
            GpuVtx *C = cs[k];
            /* subpíxel de la esquina real que comparte la coordenada entera; si no, el entero del juego (con offset de dibujo) */
            float fx = C->x == B->x ? B->fx : C->x == A->x ? A->fx : (float)C->x;
            float fy = C->y == B->y ? B->fy : C->y == A->y ? A->fy : (float)C->y;
            C->vvalid = 1; C->vflags = GPU_VF_DERIVED; C->fvalid = 0;
            C->fx = fx; C->fy = fy; C->fz = z; C->vz = z;
            C->vx = (fx - (float)g.ox - cap_proj.ofx) * z / cap_proj.h;
            C->vy = (fy - (float)g.oy - cap_proj.ofy) * z / cap_proj.h;
        }
        return;
    }
}

static void cap_set_proj(GpuOp *o) {
    if (cap_proj.h != 0.0f) { o->proj_h = cap_proj.h; o->proj_ox = cap_proj.ofx; o->proj_oy = cap_proj.ofy; }
}

static void cap_poly(const Vtx *v, int nv, int gour, int tex, int raw, int semi, const TexCtx *t) {
    GpuOp *o = cap_new(GPU_OP_POLY);
    o->nv = (uint8_t)nv;
    o->flags |= (uint16_t)((gour ? GPU_F_GOURAUD : 0) | (tex ? GPU_F_TEXTURED : 0) | (raw ? GPU_F_RAW : 0) |
                           (semi ? GPU_F_SEMI : 0) | (nv == 4 ? GPU_F_QUAD : 0) |
                           (g.dither && (gour || (tex && !raw)) ? GPU_F_DITHER : 0));
    o->clx = (uint16_t)t->cx; o->cly = (uint16_t)t->cy;
    cap_proj.h = 0;
    for (int i = 0; i < nv; i++) cap_vtx(&o->v[i], &v[i], tex);
    cap_resolve(o->v, v, nv);
    if (nv == 4 && cap.hpres) cap_derive(o->v);
    cap_set_proj(o);
}

static void cap_line(const Vtx *a, const Vtx *b, int gour, int semi) {
    GpuOp *o = cap_new(GPU_OP_LINE);
    o->nv = 2;
    o->flags |= (uint16_t)((gour ? GPU_F_GOURAUD : 0) | (semi ? GPU_F_SEMI : 0) | (g.dither && gour ? GPU_F_DITHER : 0));
    cap_proj.h = 0;
    cap_vtx(&o->v[0], a, 0); cap_vtx(&o->v[1], b, 0);
    { Vtx ab[2] = {*a, *b}; cap_resolve(o->v, ab, 2); }
    cap_set_proj(o);
}

static void cap_rect(int x, int y, int w, int h, int r, int gg, int b, int tex, int raw, int semi, int u0, int v0,
                     const TexCtx *t) {
    GpuOp *o = cap_new(GPU_OP_RECT);
    o->nv = 1;
    o->flags |= (uint16_t)((tex ? GPU_F_TEXTURED : 0) | (raw ? GPU_F_RAW : 0) | (semi ? GPU_F_SEMI : 0) |
                           (g.flipx ? GPU_F_FLIPX : 0) | (g.flipy ? GPU_F_FLIPY : 0));
    o->v[0].x = (int16_t)x; o->v[0].y = (int16_t)y;
    o->v[0].r = (uint8_t)r; o->v[0].g = (uint8_t)gg; o->v[0].b = (uint8_t)b;
    o->v[0].u = (uint8_t)u0; o->v[0].v = (uint8_t)v0;
    o->clx = (uint16_t)t->cx; o->cly = (uint16_t)t->cy;
    o->rw = (int16_t)w; o->rh = (int16_t)h;
}

static void cap_upload_begin(int x, int y, int w, int h) {
    GpuOp *o = cap_new(GPU_OP_UPLOAD);
    o->rx = (int16_t)x; o->ry = (int16_t)y; o->rw = (int16_t)w; o->rh = (int16_t)h;
    o->data_off = cap.cur.ndata;
    cap.up_idx = cap.cur.nops - 1;
    cap.up_active = 1;
}

static void cap_upload_px(uint16_t v) {
    GpuDrawList *l = &cap.cur;
    if (l->ndata == l->cap_data) {
        l->cap_data = l->cap_data ? l->cap_data * 2 : 1 << 16;
        l->data = realloc(l->data, l->cap_data * sizeof *l->data);
        if (!l->data) { fprintf(stderr, "[gpu] captura: sin memoria\n"); abort(); }
    }
    l->data[l->ndata++] = v;
    l->ops[cap.up_idx].data_n++;
}

static void cap_upload_end(void) {
    cap.up_active = 0;
    if (cap.pending_close) cap_close(cap.pending_close);
}

/* ---------------- API de captura ---------------- */
void gpu_capture_enable(int mode) {
    if (mode <= 0) {
        if (cap.on) {
            cap_list_reset(&cap.cur); free(cap.cur.ops); free(cap.cur.data);
            cap_list_reset(&cap.out); free(cap.out.ops); free(cap.out.data);
            for (int i = 0; i < GPU_CAP_QDEPTH; i++) { cap_list_reset(&cap.q[i]); free(cap.q[i].ops); free(cap.q[i].data); }
            GpuFloatHook h = cap.hook;
            memset(&cap, 0, sizeof cap);
            cap.hook = h;
        }
        return;
    }
    cap.verify = mode >= 2;
    if (cap.on) return;
    cap.on = 1;
    cap.need_snap = 1; cap.up_active = 0;
    cap.last_dx = g.dx; cap.last_dy = g.dy;
    cap_begin();
}

const GpuDrawList *gpu_capture_frame(void) {
    if (!cap.on || !cap.qn) return NULL;
    GpuDrawList *slot = &cap.q[cap.qh];
    cap_list_reset(&cap.out);
    GpuDrawList tmp = cap.out; cap.out = *slot; *slot = tmp;
    cap.qh = (cap.qh + 1) % GPU_CAP_QDEPTH; cap.qn--;
    return &cap.out;
}

void gpu_capture_vblank(void) {
    if (!cap.on) return;
    if (!cap.cur.nops) { cap.vb_noflip = 0; return; } /* sin nada pendiente no se acumula: un frame no se parte tras estar ocioso */
    if (++cap.vb_noflip >= GPU_CAP_VB_FALLBACK) cap_close(GPU_CLOSE_VBLANK);
}
uint32_t gpu_capture_dropped(void) { return cap.dropped; }
uint32_t gpu_capture_seq(void) { return cap.seq; }
void gpu_capture_set_float_hook(GpuFloatHook h) { cap.hook = h; }
void gpu_capture_set_hp_hook(GpuHpHook h) { cap.hphook = h; }
void gpu_capture_hiprec(int on) { gte_hp_enable(on); cap.hphook = NULL; cap.hpres = on ? gte_hp_resolve : NULL; }

static void cap_display_start_changed(void) {
    if (g.dx != cap.last_dx || g.dy != cap.last_dy) {
        cap.last_dx = g.dx; cap.last_dy = g.dy;
        cap_close(GPU_CLOSE_FLIP);
    }
}

/* GP1(00h)/ResetGraph: puede llegar a mitad de una subida (el estado se anula) y devuelve la dirección de
 * display a 0 sin pasar por GP1(05h). */
static void cap_on_reset(void) {
    if (!cap.on) return;
    if (cap.up_active) cap_upload_end();
    cap.last_dx = g.dx; cap.last_dy = g.dy;
}

static inline int64_t edge(const Vtx *a, const Vtx *b, int px, int py) {
    return (int64_t)(b->x - a->x) * (py - a->y) - (int64_t)(b->y - a->y) * (px - a->x);
}
/* Regla de arista superior-izquierda (área positiva): se incluyen aristas izquierdas y superiores. */
static inline int is_tl(const Vtx *a, const Vtx *b) {
    int dy = b->y - a->y, dx = b->x - a->x;
    return dy < 0 || (dy == 0 && dx > 0);
}

static void draw_tri(Vtx v0, Vtx v1, Vtx v2, int gouraud, int textured, const TexCtx *t,
                     int raw, int semi) {
    int minx = v0.x, maxx = v0.x, miny = v0.y, maxy = v0.y;
    const Vtx *vs[3] = {&v0, &v1, &v2};
    for (int i = 1; i < 3; i++) {
        if (vs[i]->x < minx) minx = vs[i]->x;
        if (vs[i]->x > maxx) maxx = vs[i]->x;
        if (vs[i]->y < miny) miny = vs[i]->y;
        if (vs[i]->y > maxy) maxy = vs[i]->y;
    }
    if (maxx - minx >= 1024 || maxy - miny >= 512) return; /* el hardware descarta */
    int64_t area = edge(&v0, &v1, v2.x, v2.y);
    if (area == 0) return;
    if (area < 0) { Vtx tmp = v1; v1 = v2; v2 = tmp; area = -area; }
    if (minx < g.cx1) minx = g.cx1;
    if (miny < g.cy1) miny = g.cy1;
    if (maxx > g.cx2) maxx = g.cx2;
    if (maxy > g.cy2) maxy = g.cy2;
    int b0 = is_tl(&v1, &v2) ? 0 : -1, b1 = is_tl(&v2, &v0) ? 0 : -1, b2 = is_tl(&v0, &v1) ? 0 : -1;
    int dith = g.dither && (gouraud || (textured && !raw));
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            int64_t w0 = edge(&v1, &v2, x, y), w1 = edge(&v2, &v0, x, y), w2 = edge(&v0, &v1, x, y);
            if (w0 + b0 < 0 || w1 + b1 < 0 || w2 + b2 < 0) continue;
            int r = v0.r, gg = v0.g, b = v0.b;
            if (gouraud) {
                r = (int)((v0.r * w0 + v1.r * w1 + v2.r * w2) / area);
                gg = (int)((v0.g * w0 + v1.g * w1 + v2.g * w2) / area);
                b = (int)((v0.b * w0 + v1.b * w1 + v2.b * w2) / area);
            }
            if (textured) {
                int u = (int)((v0.u * w0 + v1.u * w1 + v2.u * w2) / area);
                int v = (int)((v0.v * w0 + v1.v * w1 + v2.v * w2) / area);
                tex_plot(t, x, y, u, v, r, gg, b, raw, semi, dith);
            } else {
                plot(x, y, r, gg, b, dith, semi, 0);
            }
        }
    }
}

static void cmd_poly(const uint32_t *w) {
    uint32_t op = w[0] >> 24;
    int gour = (op >> 4) & 1, quad = (op >> 3) & 1, tex = (op >> 2) & 1, semi = (op >> 1) & 1,
        raw = op & 1;
    int nv = quad ? 4 : 3;
    Vtx v[4];
    TexCtx t = {0};
    int k = 0;
    uint32_t col = w[k++] & 0xFFFFFF;
    for (int i = 0; i < nv; i++) {
        if (i > 0 && gour) col = w[k++] & 0xFFFFFF;
        uint32_t xy = w[k++];
        v[i].x = sext11(xy) + g.ox;
        v[i].y = sext11(xy >> 16) + g.oy;
        v[i].r = col & 255; v[i].g = (col >> 8) & 255; v[i].b = (col >> 16) & 255;
        v[i].u = v[i].v = 0;
        if (tex) {
            uint32_t uv = w[k++];
            v[i].u = uv & 255; v[i].v = (uv >> 8) & 255;
            if (i == 0) { t.cx = ((uv >> 16) & 63) * 16; t.cy = (uv >> 22) & 511; }
            if (i == 1) {
                uint32_t tp = uv >> 16;
                g.e1 = (g.e1 & ~0x9FFu) | (tp & 0x9FF);
                set_texpage(g.e1);
            }
        }
    }
    if (tex && g.tex_off) tex = 0; /* texturas deshabilitadas por E1 bit 11 */
    t.tx = g.tx; t.ty = g.ty; t.tp = g.tp;
    gpu_stats.poly++;
    if (cap.on) cap_poly(v, nv, gour, tex, raw, semi, &t);
    draw_tri(v[0], v[1], v[2], gour, tex, &t, raw, semi);
    if (quad) draw_tri(v[1], v[2], v[3], gour, tex, &t, raw, semi);
}

/* ---------------- líneas ---------------- */
static void draw_line(Vtx a, Vtx b, int gour, int semi) {
    int dx = b.x - a.x, dy = b.y - a.y;
    int adx = abs(dx), ady = abs(dy);
    if (adx >= 1024 || ady >= 512) return;
    int n = adx > ady ? adx : ady;
    int dith = g.dither && gour;
    for (int i = 0; i <= n; i++) {
        int x = n ? a.x + (dx * i + (dx >= 0 ? n / 2 : -n / 2)) / n : a.x;
        int y = n ? a.y + (dy * i + (dy >= 0 ? n / 2 : -n / 2)) / n : a.y;
        int r = a.r, gg = a.g, bb = a.b;
        if (gour && n) {
            r = a.r + (b.r - a.r) * i / n;
            gg = a.g + (b.g - a.g) * i / n;
            bb = a.b + (b.b - a.b) * i / n;
        }
        plot(x, y, r, gg, bb, dith, semi, 0);
    }
}

static void do_line(Vtx a, Vtx b, int gour, int semi) {
    if (cap.on) cap_line(&a, &b, gour, semi);
    draw_line(a, b, gour, semi);
}

static Vtx line_v(uint32_t col, uint32_t xy) {
    Vtx v = {sext11(xy) + g.ox, sext11(xy >> 16) + g.oy, col & 255, (col >> 8) & 255,
             (col >> 16) & 255, 0, 0};
    return v;
}

/* Estado de polilínea */
static Vtx pl_last;
static uint32_t pl_col, pl_op;
static int pl_wait_col;

/* ---------------- rectángulos ---------------- */
static void cmd_rect(const uint32_t *w) {
    uint32_t op = w[0] >> 24;
    int tex = (op >> 2) & 1, semi = (op >> 1) & 1, raw = op & 1, sz = (op >> 3) & 3;
    int k = 1;
    uint32_t col = w[0] & 0xFFFFFF;
    uint32_t xy = w[k++];
    int x0 = sext11(xy) + g.ox, y0 = sext11(xy >> 16) + g.oy;
    TexCtx t = {g.tx, g.ty, g.tp, 0, 0};
    int u0 = 0, v0 = 0;
    if (tex) {
        uint32_t uv = w[k++];
        u0 = uv & 255; v0 = (uv >> 8) & 255;
        t.cx = ((uv >> 16) & 63) * 16; t.cy = (uv >> 22) & 511;
    }
    int ww, hh;
    if (sz == 0) { uint32_t s = w[k++]; ww = s & 0x3FF; hh = (s >> 16) & 0x1FF; }
    else if (sz == 1) ww = hh = 1;
    else if (sz == 2) ww = hh = 8;
    else ww = hh = 16;
    if (g.tex_off) tex = 0;
    int r = col & 255, gg = (col >> 8) & 255, b = (col >> 16) & 255;
    gpu_stats.rect++;
    if (cap.on) cap_rect(x0, y0, ww, hh, r, gg, b, tex, raw, semi, u0, v0, &t);
    for (int j = 0; j < hh; j++) {
        int y = y0 + j;
        if (y < g.cy1 || y > g.cy2) continue;
        for (int i = 0; i < ww; i++) {
            int x = x0 + i;
            if (x < g.cx1 || x > g.cx2) continue;
            if (tex) {
                int u = g.flipx ? u0 - i : u0 + i;
                int v = g.flipy ? v0 - j : v0 + j;
                tex_plot(&t, x, y, u, v, r, gg, b, raw, semi, 0);
            } else {
                plot(x, y, r, gg, b, 0, semi, 0);
            }
        }
    }
}

/* ---------------- fill / copias ---------------- */
static void cmd_fill(const uint32_t *w) {
    uint32_t col = w[0];
    int x = w[1] & 0x3F0, y = (w[1] >> 16) & 0x1FF;
    int ww = ((w[2] & 0x3FF) + 15) & ~15, hh = (w[2] >> 16) & 0x1FF;
    uint16_t c = (uint16_t)(((col >> 3) & 31) | (((col >> 11) & 31) << 5) | (((col >> 19) & 31) << 10));
    gpu_stats.fill++;
    if (cap.on) {
        GpuOp *o = cap_new(GPU_OP_FILL);
        o->rx = (int16_t)x; o->ry = (int16_t)y; o->rw = (int16_t)ww; o->rh = (int16_t)hh;
        o->fill15 = c;
        o->v[0].r = (uint8_t)col; o->v[0].g = (uint8_t)(col >> 8); o->v[0].b = (uint8_t)(col >> 16);
    }
    for (int j = 0; j < hh; j++)
        for (int i = 0; i < ww; i++) gpu_vram[(y + j) & 511][(x + i) & 1023] = c;
}

static inline void vram_put(int x, int y, uint16_t v) {
    uint16_t *p = &gpu_vram[y & 511][x & 1023];
    if (g.mask_chk && (*p & 0x8000)) return;
    *p = v | (g.mask_set ? 0x8000 : 0);
}

static void cmd_copy(const uint32_t *w) {
    int sx = w[1] & 0x3FF, sy = (w[1] >> 16) & 0x1FF;
    int dx = w[2] & 0x3FF, dy = (w[2] >> 16) & 0x1FF;
    int ww = (((w[3] & 0xFFFF) - 1) & 0x3FF) + 1, hh = ((((w[3] >> 16) & 0xFFFF) - 1) & 0x1FF) + 1;
    gpu_stats.copy++;
    if (cap.on) {
        GpuOp *o = cap_new(GPU_OP_VCOPY);
        o->sx = (int16_t)sx; o->sy = (int16_t)sy; o->rx = (int16_t)dx; o->ry = (int16_t)dy;
        o->rw = (int16_t)ww; o->rh = (int16_t)hh;
    }
    /* copia vía buffer de línea: evita aliasing cuando se solapan */
    static uint16_t line[1024];
    for (int j = 0; j < hh; j++) {
        for (int i = 0; i < ww; i++) line[i] = gpu_vram[(sy + j) & 511][(sx + i) & 1023];
        for (int i = 0; i < ww; i++) vram_put(dx + i, dy + j, line[i]);
    }
}

static void xfer_setup(const uint32_t *w, int read) {
    int x = w[1] & 0x3FF, y = (w[1] >> 16) & 0x1FF;
    int ww = (((w[2] & 0xFFFF) - 1) & 0x3FF) + 1, hh = ((((w[2] >> 16) & 0xFFFF) - 1) & 0x1FF) + 1;
    if (read) {
        rd.x = x; rd.y = y; rd.w = ww; rd.h = hh; rd.i = 0; rd.n = ww * hh;
        rd_active = 1;
        gpu_stats.download++;
        if (cap.on) {
            GpuOp *o = cap_new(GPU_OP_DOWNLOAD);
            o->rx = (int16_t)x; o->ry = (int16_t)y; o->rw = (int16_t)ww; o->rh = (int16_t)hh;
        }
    } else {
        xfer.x = x; xfer.y = y; xfer.w = ww; xfer.h = hh; xfer.i = 0; xfer.n = ww * hh;
        st = ST_UPLOAD;
        gpu_stats.upload++;
        if (cap.on) cap_upload_begin(x, y, ww, hh);
    }
}

static void upload_px(uint16_t v) {
    if (xfer.i < xfer.n) {
        if (cap.on && cap.up_active) cap_upload_px(v);
        vram_put(xfer.x + xfer.i % xfer.w, xfer.y + xfer.i / xfer.w, v);
        xfer.i++;
    }
}

uint32_t gpu_read(void) {
    if (rd_active) {
        uint32_t v = 0;
        for (int k = 0; k < 2; k++) {
            if (rd.i < rd.n) {
                v |= (uint32_t)gpu_vram[(rd.y + rd.i / rd.w) & 511][(rd.x + rd.i % rd.w) & 1023] << (16 * k);
                rd.i++;
            }
        }
        if (rd.i >= rd.n) rd_active = 0;
        return v;
    }
    return g.read_latch;
}

/* ---------------- entorno E1-E6 ---------------- */
static void cmd_env(uint32_t w) {
    gpu_stats.env++;
    switch (w >> 24) {
    case 0xE1:
        g.e1 = w & 0x3FFF;
        set_texpage(w);
        g.dither = (w >> 9) & 1; g.draw_disp = (w >> 10) & 1;
        g.flipx = (w >> 12) & 1; g.flipy = (w >> 13) & 1;
        break;
    case 0xE2:
        g.e2 = w & 0xFFFFF;
        g.twmx = w & 31; g.twmy = (w >> 5) & 31; g.twox = (w >> 10) & 31; g.twoy = (w >> 15) & 31;
        break;
    case 0xE3: g.e3 = w & 0xFFFFF; g.cx1 = w & 0x3FF; g.cy1 = (w >> 10) & 0x3FF; break;
    case 0xE4: g.e4 = w & 0xFFFFF; g.cx2 = w & 0x3FF; g.cy2 = (w >> 10) & 0x3FF; break;
    case 0xE5: g.e5 = w & 0x3FFFFF; g.ox = sext11(w); g.oy = sext11(w >> 11); break;
    case 0xE6: g.mask_set = w & 1; g.mask_chk = (w >> 1) & 1; break;
    }
}

/* ---------------- GP0 ---------------- */
static int cmd_len(uint32_t op) {
    if (op >= 0x20 && op < 0x40) {
        int gour = (op >> 4) & 1, nv = (op & 8) ? 4 : 3, tex = (op >> 2) & 1;
        return 1 + nv * (1 + tex) + (gour ? nv - 1 : 0);
    }
    if (op >= 0x40 && op < 0x60) return (op & 0x10) ? 4 : 3; /* polilínea: inicio igual */
    if (op >= 0x60 && op < 0x80) return 2 + ((op >> 2) & 1) + (((op >> 3) & 3) == 0);
    if (op >= 0x80 && op < 0xA0) return 4;
    if (op >= 0xA0 && op < 0xE0) return 3;
    if (op == 0x02) return 3;
    return 1;
}

static void exec_cmd(const uint32_t *w) {
    uint32_t op = w[0] >> 24;
    if (op >= 0x20 && op < 0x40) cmd_poly(w);
    else if (op >= 0x40 && op < 0x60) {
        int gour = (op >> 4) & 1, semi = (op >> 1) & 1;
        Vtx a = line_v(w[0], w[1]);
        Vtx b = gour ? line_v(w[2], w[3]) : line_v(w[0], w[2]);
        gpu_stats.line++;
        do_line(a, b, gour, semi);
        if (op & 8) { st = ST_POLYLINE; pl_last = b; pl_col = gour ? w[2] : w[0]; pl_op = op; pl_wait_col = gour; }
    } else if (op >= 0x60 && op < 0x80) cmd_rect(w);
    else if (op >= 0x80 && op < 0xA0) cmd_copy(w);
    else if (op >= 0xA0 && op < 0xC0) xfer_setup(w, 0);
    else if (op >= 0xC0 && op < 0xE0) xfer_setup(w, 1);
    else if (op == 0x02) cmd_fill(w);
    else if (op >= 0xE1 && op <= 0xE6) cmd_env(w[0]);
    else if (op == 0x1F) g.irq = 1;
    /* 0x00, 0x01 (caché), 0x03 y 0xE0/0xE7+ = nop */
}

void gpu_gp0(uint32_t w) {
    switch (st) {
    case ST_UPLOAD:
        upload_px((uint16_t)w);
        upload_px((uint16_t)(w >> 16));
        if (xfer.i >= xfer.n) { st = ST_CMD; if (cap.on && cap.up_active) cap_upload_end(); }
        return;
    case ST_POLYLINE:
        if ((w & 0xF000F000u) == 0x50005000u) { st = ST_CMD; return; }
        if (pl_wait_col) { pl_col = w; pl_wait_col = 0; return; }
        {
            Vtx b = line_v(pl_col, w);
            gpu_stats.line++;
            do_line(pl_last, b, (pl_op >> 4) & 1, (pl_op >> 1) & 1);
            pl_last = b;
            if (pl_op & 0x10) pl_wait_col = 1;
        }
        return;
    case ST_CMD:
        break;
    }
    if (fifo_n == 0) fifo_need = cmd_len(w >> 24);
    fifo[fifo_n++] = w;
    if (fifo_n >= fifo_need) { fifo_n = 0; exec_cmd(fifo); }
}

/* ---------------- GP1 ---------------- */
void gpu_gp1(uint32_t w) {
    uint32_t v = w & 0xFFFFFF;
    switch ((w >> 24) & 0x3F) {
    case 0x00: gpu_reset(); break;
    case 0x01:
        if (st == ST_UPLOAD && cap.on && cap.up_active) cap_upload_end();
        fifo_n = 0; st = ST_CMD; break;
    case 0x02: g.irq = 0; break;
    case 0x03: g.disp_off = v & 1; if (cap.on) cap_refresh_disp(); break;
    case 0x04: g.dma_dir = v & 3; break;
    case 0x05:
        g.dx = v & 0x3FE; g.dy = (v >> 10) & 0x1FF;
        if (cap.on) { cap_display_start_changed(); cap_refresh_disp(); }
        break;
    case 0x06: g.hx1 = v & 0xFFF; g.hx2 = (v >> 12) & 0xFFF; if (cap.on) cap_refresh_disp(); break;
    case 0x07: g.vy1 = v & 0x3FF; g.vy2 = (v >> 10) & 0x3FF; if (cap.on) cap_refresh_disp(); break;
    case 0x08: g.mode = v; if (cap.on) cap_refresh_disp(); break;
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        switch (v & 7) {
        case 2: g.read_latch = g.e2; break;
        case 3: g.read_latch = g.e3; break;
        case 4: g.read_latch = g.e4; break;
        case 5: g.read_latch = g.e5; break;
        case 7: g.read_latch = 2; break;
        }
        break;
    }
}

uint32_t gpu_stat(void) {
    uint32_t s = (g.e1 & 0x7FF) | (g.mask_set << 11) | (g.mask_chk << 12);
    s |= ((g.mode >> 6) & 1) << 16;            /* 368 */
    s |= (g.mode & 3) << 17;                   /* hres */
    s |= ((g.mode >> 2) & 1) << 19;            /* vres */
    s |= ((g.mode >> 3) & 1) << 20;            /* PAL */
    s |= ((g.mode >> 4) & 1) << 21;            /* 24 bit */
    s |= ((g.mode >> 5) & 1) << 22;            /* entrelazado */
    s |= (uint32_t)g.disp_off << 23;
    s |= (uint32_t)g.irq << 24;
    s |= 0x1C000000u;                          /* listo para comando, VRAM->CPU y DMA */
    if (g.dma_dir == 1 || g.dma_dir == 2) s |= 1u << 25;
    s |= (uint32_t)g.dma_dir << 29;
    s |= (hle_frame_count() & 1u) << 31;       /* campo par/impar */
    return s;
}

void gpu_get_display(GpuDisplay *d) {
    static const int hres[4] = {256, 320, 512, 640};
    d->x = g.dx; d->y = g.dy;
    d->w = (g.mode & 0x40) ? 368 : hres[g.mode & 3];
    int h = g.vy2 - g.vy1;
    if (h <= 0 || h > 288) h = (g.mode & 8) ? 256 : 240;
    d->interlace = (g.mode >> 5) & 1;
    if (d->interlace && (g.mode & 4)) h *= 2;
    d->h = h;
    d->depth24 = (g.mode >> 4) & 1;
    d->pal = (g.mode >> 3) & 1;
    d->enabled = !g.disp_off;
}

/* ---------------- DMA canal 2 y listas enlazadas ---------------- */
static inline uint32_t ram32(uint32_t a) {
    uint32_t v;
    memcpy(&v, &psx_ram[a & 0x1FFFFC], 4);
    return v;
}

void gpu_draw_list(uint32_t addr) {
    gpu_stats.lists++;
    addr &= 0x1FFFFC;
    for (unsigned guard = 0; guard < 0x100000; guard++) {
        uint32_t h = ram32(addr);
        unsigned n = h >> 24;
        for (unsigned i = 0; i < n; i++) gpu_gp0(ram32(addr + 4 + 4 * i));
        if (h & 0x800000) return; /* terminador 0xFFFFFF (bit 23) */
        addr = h & 0x1FFFFC;
    }
    fprintf(stderr, "[gpu] lista enlazada sin fin (¿bucle?)\n");
}

void gpu_dma2(uint32_t madr, uint32_t bcr, uint32_t chcr) {
    uint32_t a = madr & 0x1FFFFC;
    switch ((chcr >> 9) & 3) {
    case 0: case 1: {
        uint32_t words = (chcr >> 9) & 3 ? (bcr & 0xFFFF) * (bcr >> 16) : (bcr & 0xFFFF ? bcr & 0xFFFF : 0x10000);
        for (uint32_t i = 0; i < words; i++, a += 4) {
            if (chcr & 1) gpu_gp0(ram32(a));
            else { uint32_t v = gpu_read(); memcpy(&psx_ram[a & 0x1FFFFC], &v, 4); }
        }
        break;
    }
    case 2: gpu_draw_list(madr); break;
    }
}

/* ---------------- volcado PPM ---------------- */
void gpu_dump_ppm(const char *path, int full) {
    GpuDisplay d;
    gpu_get_display(&d);
    int x0 = full ? 0 : d.x, y0 = full ? 0 : d.y, w = full ? 1024 : d.w, h = full ? 512 : d.h;
    int d24 = !full && d.depth24;
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    uint8_t *row = malloc((size_t)w * 3);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *o = row + x * 3;
            if (d24) {
                /* 24 bits: 3 bytes por píxel empaquetados en la línea de VRAM */
                const uint8_t *ln = (const uint8_t *)gpu_vram[(y0 + y) & 511];
                for (int k = 0; k < 3; k++) o[k] = ln[((x0 * 2) + x * 3 + k) & 2047];
            } else {
                uint16_t c = gpu_vram[(y0 + y) & 511][(x0 + x) & 1023];
                o[0] = (uint8_t)((c & 31) << 3 | (c & 31) >> 2);
                o[1] = (uint8_t)(((c >> 5) & 31) << 3 | ((c >> 5) & 31) >> 2);
                o[2] = (uint8_t)(((c >> 10) & 31) << 3 | ((c >> 10) & 31) >> 2);
            }
        }
        fwrite(row, 3, (size_t)w, f);
    }
    free(row);
    fclose(f);
}

/* =================== HLE de libgpu =================== */
#define V0 c->r[2]
#define A0 c->r[4]
#define A1 c->r[5]
#define A2 c->r[6]
#define A3 c->r[7]

static uint32_t drawsync_cb;
static int in_cb;
static int graph_debug;

static int16_t rd16(Cpu *c, uint32_t a) { return (int16_t)mem_r16(c, a); }

/* Callback de fin de dibujo: con GPU síncrona se invoca al terminar cada operación encolable. */
static void done(Cpu *c) {
    if (drawsync_cb && !in_cb) {
        in_cb = 1;
        hle_invoke(c, drawsync_cb, 0);
        in_cb = 0;
    }
}

void hle_ResetGraph(Cpu *c) {
    int mode = A0 & 7;
    if (mode == 0 || mode == 3 || mode == 5) {
        gpu_reset();
        gpu_gp1(0x00000000);
    } else {
        gpu_gp1(0x01000000);
    }
    if (mode != 3) drawsync_cb = 0;
    /* El estado interno de libgpu (tipo de GPU, tamaño máximo de VRAM que usan SetDrawEnv/SetDefDrawEnv
     * para recortar) lo inicializa el ResetGraph original: se ejecuta tras la parte de hardware. */
    if (mode == 0 || mode == 3 || mode == 5) {
        uint32_t keep = drawsync_cb;
        rt_orig(c, "ResetGraph");
        drawsync_cb = keep;
    }
    V0 = 0;
}
void hle_SetGraphDebug(Cpu *c) { V0 = graph_debug; graph_debug = A0; }
void hle_SetGraphQueue(Cpu *c) { V0 = 0; }
void hle_GetODE(Cpu *c) { V0 = gpu_stat() >> 31; }
void hle_DrawSync(Cpu *c) { V0 = 0; } /* cola siempre vacía */
void hle_DrawSyncCallback(Cpu *c) { V0 = drawsync_cb; drawsync_cb = A0; }
void hle_SetDispMask(Cpu *c) { gpu_gp1(0x03000000 | (A0 ? 0 : 1)); V0 = 0; }

/* RECT {short x, y, w, h} */
static void get_rect(Cpu *c, uint32_t p, int *x, int *y, int *w, int *h) {
    *x = rd16(c, p); *y = rd16(c, p + 2); *w = rd16(c, p + 4); *h = rd16(c, p + 6);
}

void hle_LoadImage(Cpu *c) {
    int x, y, w, h;
    get_rect(c, A0, &x, &y, &w, &h);
    uint32_t src = A1;
    if (w > 0 && h > 0) {
        gpu_gp0(0xA0000000);
        gpu_gp0((uint32_t)(x & 0x3FF) | (uint32_t)(y & 0x1FF) << 16);
        gpu_gp0((uint32_t)(w & 0xFFFF) | (uint32_t)(h & 0xFFFF) << 16);
        uint32_t words = ((uint32_t)w * h + 1) / 2;
        for (uint32_t i = 0; i < words; i++) gpu_gp0(mem_r32(c, src + 4 * i));
        st = ST_CMD;
    }
    V0 = 0;
    done(c);
}
void hle_LoadImage2(Cpu *c) { hle_LoadImage(c); }

void hle_StoreImage(Cpu *c) {
    int x, y, w, h;
    get_rect(c, A0, &x, &y, &w, &h);
    uint32_t dst = A1;
    if (w > 0 && h > 0) {
        gpu_gp0(0xC0000000);
        gpu_gp0((uint32_t)(x & 0x3FF) | (uint32_t)(y & 0x1FF) << 16);
        gpu_gp0((uint32_t)(w & 0xFFFF) | (uint32_t)(h & 0xFFFF) << 16);
        uint32_t words = ((uint32_t)w * h + 1) / 2;
        for (uint32_t i = 0; i < words; i++) mem_w32(c, dst + 4 * i, gpu_read());
        rd_active = 0;
    }
    V0 = 0;
    done(c);
}
void hle_StoreImage2(Cpu *c) { hle_StoreImage(c); }

void hle_MoveImage(Cpu *c) {
    int x, y, w, h;
    get_rect(c, A0, &x, &y, &w, &h);
    if (w > 0 && h > 0) {
        gpu_gp0(0x80000000);
        gpu_gp0((uint32_t)(x & 0x3FF) | (uint32_t)(y & 0x1FF) << 16);
        gpu_gp0((A1 & 0x3FF) | (A2 & 0x1FF) << 16);
        gpu_gp0((uint32_t)(w & 0xFFFF) | (uint32_t)(h & 0xFFFF) << 16);
    }
    V0 = 0;
    done(c);
}
void hle_MoveImage2(Cpu *c) { hle_MoveImage(c); }

/* ClearImage(RECT*, r, g, b): fill respetando solo el rectángulo (sin clip ni offset). */
void hle_ClearImage(Cpu *c) {
    int x, y, w, h;
    get_rect(c, A0, &x, &y, &w, &h);
    uint32_t col = (A1 & 255) | (A2 & 255) << 8 | (A3 & 255) << 16;
    gpu_gp0(0x02000000 | col);
    gpu_gp0((uint32_t)(x & 0x3FF) | (uint32_t)(y & 0x1FF) << 16);
    gpu_gp0((uint32_t)(w & 0x3FF) | (uint32_t)(h & 0x1FF) << 16);
    V0 = 0;
    done(c);
}
void hle_ClearImage2(Cpu *c) { hle_ClearImage(c); }

/* OT: entradas = puntero de 24 bits a la siguiente (longitud 0); terminador 0x00FFFFFF. */
void hle_ClearOTag(Cpu *c) {
    uint32_t ot = A0, n = A1;
    for (uint32_t i = 0; i + 1 < n; i++) mem_w32(c, ot + 4 * i, (ot + 4 * (i + 1)) & 0xFFFFFF);
    if (n) mem_w32(c, ot + 4 * (n - 1), 0x00FFFFFF);
    V0 = ot;
}
void hle_ClearOTagR(Cpu *c) {
    uint32_t ot = A0, n = A1;
    for (uint32_t i = 1; i < n; i++) mem_w32(c, ot + 4 * i, (ot + 4 * (i - 1)) & 0xFFFFFF);
    if (n) mem_w32(c, ot, 0x00FFFFFF);
    V0 = ot;
}

void hle_DrawOTag(Cpu *c) { gpu_draw_list(A0); V0 = 0; done(c); }
void hle_DrawOTag2(Cpu *c) { hle_DrawOTag(c); }

/* DrawPrim(p): un único paquete (cabecera con longitud). */
void hle_DrawPrim(Cpu *c) {
    uint32_t h = mem_r32(c, A0);
    unsigned n = h >> 24;
    for (unsigned i = 0; i < n; i++) gpu_gp0(mem_r32(c, A0 + 4 + 4 * i));
    V0 = 0;
}

/* DRAWENV: clip RECT @0, ofs[2] @8, tw RECT @12, tpage @20, dtd @22, dfe @23, isbg @24,
 * r0 g0 b0 @25..27, dr_env @28 (64 B). Total 92 B. */
static void put_drawenv(Cpu *c, uint32_t e) {
    int x, y, w, h;
    get_rect(c, e, &x, &y, &w, &h);
    int ox = rd16(c, e + 8), oy = rd16(c, e + 10);
    int twx, twy, tww, twh;
    get_rect(c, e + 12, &twx, &twy, &tww, &twh);
    uint32_t tpage = mem_r16(c, e + 20);
    int dtd = mem_r8(c, e + 22), dfe = mem_r8(c, e + 23), isbg = mem_r8(c, e + 24);
    uint32_t bg = mem_r8(c, e + 25) | mem_r8(c, e + 26) << 8 | mem_r8(c, e + 27) << 16;
    int x2 = x + w - 1, y2 = y + h - 1;
    if (x2 < x) x2 = x;
    if (y2 < y) y2 = y;
    gpu_gp0(0xE1000000 | (tpage & 0x9FF) | (dtd ? 0x200 : 0) | (dfe ? 0x400 : 0));
    /* ventana de textura: máscara y offset en unidades de 8 píxeles */
    uint32_t mx = tww ? ((-tww) & 0xFF) >> 3 : 0, my = twh ? ((-twh) & 0xFF) >> 3 : 0;
    uint32_t offx = (twx & 0xFF) >> 3, offy = (twy & 0xFF) >> 3;
    gpu_gp0(0xE2000000 | mx | my << 5 | offx << 10 | offy << 15);
    gpu_gp0(0xE3000000 | (x & 0x3FF) | (uint32_t)(y & 0x3FF) << 10);
    gpu_gp0(0xE4000000 | (x2 & 0x3FF) | (uint32_t)(y2 & 0x3FF) << 10);
    gpu_gp0(0xE5000000 | (ox & 0x7FF) | (uint32_t)(oy & 0x7FF) << 11);
    if (isbg && w > 0 && h > 0) {
        gpu_gp0(0x02000000 | bg);
        gpu_gp0((uint32_t)(x & 0x3FF) | (uint32_t)(y & 0x1FF) << 16);
        gpu_gp0((uint32_t)(w & 0x3FF) | (uint32_t)(h & 0x1FF) << 16);
    }
}

void hle_PutDrawEnv(Cpu *c) { put_drawenv(c, A0); V0 = A0; done(c); }
void hle_DrawOTagEnv(Cpu *c) { uint32_t p = A0; put_drawenv(c, A1); gpu_draw_list(p); V0 = 0; done(c); }

/* DISPENV: disp RECT @0, screen RECT @8, isinter @16, isrgb24 @17. */
void hle_PutDispEnv(Cpu *c) {
    uint32_t e = A0;
    int x, y, w, h, sx, sy, sw, sh;
    get_rect(c, e, &x, &y, &w, &h);
    get_rect(c, e + 8, &sx, &sy, &sw, &sh);
    int inter = mem_r8(c, e + 16), rgb24 = mem_r8(c, e + 17);
    uint32_t mode;
    if (w > 560) mode = 3; else if (w > 400) mode = 2; else if (w > 352) mode = 0x40; else if (w > 280) mode = 1; else mode = 0;
    if (h > 256) mode |= 4 | 0x20;
    if (inter) mode |= 0x20;
    if (rgb24) mode |= 0x10;
    mode |= 8; /* edición PAL */
    gpu_gp1(0x08000000 | mode);
    gpu_gp1(0x05000000 | (x & 0x3FF) | (uint32_t)(y & 0x1FF) << 10);
    /* rango vertical: alto visible (screen.h si se dio, si no disp.h) */
    int vh = sh ? sh : (h > 256 ? h / 2 : h);
    int vy = 0x13 + sy; /* PAL: primera línea visible aproximada */
    gpu_gp1(0x07000000 | (vy & 0x3FF) | (uint32_t)((vy + vh) & 0x3FF) << 10);
    gpu_gp1(0x06000000 | 0x260 | (uint32_t)(0x260 + 2560) << 12);
    V0 = A0;
}
