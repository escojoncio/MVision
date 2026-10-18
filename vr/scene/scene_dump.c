/* scene_dump: herramienta de prueba de vr_scene (no forma parte de la app). Enlaza el runtime, corre el juego con un
 * guion de mando y, para los fotogramas pedidos, ensambla la escena VR, la exporta a OBJ (con UV) y la renderiza por
 * software desde (a) la cámara del juego (réplica, comparable con la imagen del oráculo), (b) dos ojos separados 64 mm
 * desde un punto de vista FIJO en el mundo VR, (c) dos ojos desde la cabeza del jugador. Compara además la posición
 * de los vértices entre dos fotogramas: el escenario no debe moverse en mundo VR cuando la cámara del juego se mueve.
 *   scene_dump <exe> <frames_max> --out DIR --at F1,F2,... [--disc R] [--pad G] [--pad-every N:B] [--scale S]
 *              [--feet UNIDADES]
 * Disco: MV_DISC. Nunca escribe en la RAM del juego. */
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "bridge.h"
#include "cdrom.h"
#include "cpu.h"
#include "gpu.h"
#include "hle.h"
#include "ref_raster.h"
#include "sys.h"
#include "vr_scene.h"

extern jmp_buf rt_fatal_jmp;
extern int rt_fatal_armed;

#define MAXT 8
static uint32_t max_frames = 8000, at[MAXT];
static int nat;
static const char *outdir = ".";
static VrSceneOpts opts;
static RefR *rv;
static BridgeState bs;

/* ---- guion de mando (igual que bridge_dump) ---- */
typedef struct { uint32_t start, dur, every; uint16_t mask; } PadEv;
static PadEv pad_ev[256];
static int npad_ev;
static const struct { const char *n; int bit; } pad_names[] = {
    {"select", 0}, {"l3", 1}, {"r3", 2}, {"start", 3}, {"up", 4}, {"right", 5}, {"down", 6}, {"left", 7},
    {"l2", 8}, {"r2", 9}, {"l1", 10}, {"r1", 11}, {"triangle", 12}, {"circle", 13}, {"cross", 14}, {"square", 15}};
static uint16_t pad_mask(const char *s, size_t len) {
    uint16_t m = 0;
    while (len) {
        size_t k = 0;
        while (k < len && s[k] != '+') k++;
        int ok = 0;
        for (size_t i = 0; i < sizeof pad_names / sizeof *pad_names; i++)
            if (strlen(pad_names[i].n) == k && !strncasecmp(s, pad_names[i].n, k)) { m |= (uint16_t)(1u << pad_names[i].bit); ok = 1; }
        if (!ok) return 0;
        s += k; len -= k;
        if (len) { s++; len--; }
    }
    return m;
}
static int pad_add_script(const char *g) {
    char *dup = strdup(g), *sv = NULL;
    for (char *tok = strtok_r(dup, ",", &sv); tok; tok = strtok_r(NULL, ",", &sv)) {
        char *e;
        uint32_t fr = (uint32_t)strtoul(tok, &e, 0);
        if (*e != ':' || npad_ev >= 256) { free(dup); return -1; }
        char *b = e + 1, *c2 = strchr(b, ':');
        uint32_t dur = 6;
        if (c2) dur = (uint32_t)strtoul(c2 + 1, 0, 0);
        uint16_t m = pad_mask(b, c2 ? (size_t)(c2 - b) : strlen(b));
        if (!m) { free(dup); return -1; }
        pad_ev[npad_ev++] = (PadEv){fr, dur, 0, m};
    }
    free(dup);
    return 0;
}
static int pad_add_every(const char *a) {
    char *e;
    uint32_t n = (uint32_t)strtoul(a, &e, 0);
    if (*e != ':' || !n || npad_ev >= 256) return -1;
    uint16_t m = pad_mask(e + 1, strlen(e + 1));
    if (!m) return -1;
    pad_ev[npad_ev++] = (PadEv){0, 6, n, m};
    return 0;
}
static void script_pad_frame(uint32_t f) {
    static uint16_t last = 0xFFFF;
    uint16_t b = 0;
    for (int i = 0; i < npad_ev; i++) {
        const PadEv *p = &pad_ev[i];
        if (p->every) { if (f >= p->every && f % p->every < p->dur) b |= p->mask; }
        else if (f >= p->start && f < p->start + p->dur) b |= p->mask;
    }
    if (b != last) { pad_set_state(0, b, 0x80, 0x80, 0x80, 0x80); last = b; }
}

/* ---- PNG mínimo ---- */
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
static void cam_look(Cam *c, const float *eye, const float *target, int W, int H, float hfov_deg) {
    memcpy(c->e, eye, 12);
    for (int i = 0; i < 3; i++) c->f[i] = target[i] - eye[i];
    norm3(c->f);
    float up[3] = {0, 1, 0};
    cross3(c->f, up, c->r); norm3(c->r);
    cross3(c->r, c->f, c->u);
    c->W = W; c->H = H; c->cx = W * 0.5f; c->cy = H * 0.5f;
    c->focal = (W * 0.5f) / tanf(hfov_deg * 0.5f * 3.14159265f / 180.0f);
}

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

/* Dibuja los batches 3D de la escena con eventos de VRAM en orden. vram debe partir del inicio del frame. */
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

/* ---- datos por fotograma pedido ---- */
typedef struct {
    uint32_t frame;
    VrScene sc;               /* punteros inválidos tras la siguiente construcción: solo se usan las copias */
    VrVtx *v3; uint32_t n3;   /* vértices 3D (copia) */
    float game_view[16], eye_vr[3], org[3], feet[3];
    int world_valid;
    Img img_fix;              /* vista fija (ojo izquierdo) */
    Img img_fixR;
    int ok;
} Cap;
static Cap caps[MAXT];
static int ncaps;
static uint16_t *vram_before;
static float anchor[3]; static int have_anchor;
static float fixed_eye[3], fixed_tgt[3]; static int have_fixed;

static int fcmp(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }
/* -1 si el mundo del juego es zurdo (det de los ejes de la cámara < 0): vr_scene espeja Z */
static float mirror_sign(void) {
    if (opts.mirror_z == 1) return -1.0f;
    if (opts.mirror_z == 0 || !(bs.valid & BRIDGE_OK_EYE)) return 1.0f;
    float R[3][3];
    for (int r = 0; r < 3; r++) for (int cc = 0; cc < 3; cc++) R[r][cc] = (float)bs.eye_axes[r][cc];
    float det = R[0][0]*(R[1][1]*R[2][2]-R[1][2]*R[2][1]) - R[0][1]*(R[1][0]*R[2][2]-R[1][2]*R[2][0]) + R[0][2]*(R[1][0]*R[2][1]-R[1][1]*R[2][0]);
    return det < 0 ? -1.0f : 1.0f;
}
static void world_stats(const Cap *c, const char *tag) {
    /* altura del escenario respecto a los pies del jugador: percentiles de y, y figura del jugador (vértices a < 0,6 m en horizontal) */
    uint32_t n = c->n3;
    float *ys = malloc((n ? n : 1) * sizeof(float));
    for (uint32_t i = 0; i < n; i++) ys[i] = c->v3[i].pos[1];
    qsort(ys, n, sizeof(float), fcmp);
    if (n) fprintf(stderr, "[%s] y mundo VR (m, pies del jugador=%.3f): min %.3f p2 %.3f p50 %.3f p98 %.3f max %.3f\n", tag, c->feet[1], ys[0],
                   ys[n / 50], ys[n / 2], ys[n - 1 - n / 50], ys[n - 1]);
    free(ys);
    float lo = 1e9f, hi = -1e9f; int cnt = 0;
    for (uint32_t i = 0; i < n; i++) {
        float dx = c->v3[i].pos[0] - c->feet[0], dz = c->v3[i].pos[2] - c->feet[2];
        if (dx * dx + dz * dz < 0.45f * 0.45f) { cnt++; if (c->v3[i].pos[1] < lo) lo = c->v3[i].pos[1]; if (c->v3[i].pos[1] > hi) hi = c->v3[i].pos[1]; }
    }
    if (cnt) {
        int hist[40] = {0};
        for (uint32_t i = 0; i < n; i++) {
            float dx = c->v3[i].pos[0] - c->feet[0], dz = c->v3[i].pos[2] - c->feet[2];
            int bin = (int)floorf((c->v3[i].pos[1] - c->feet[1]) / 0.25f) + 10;
            if (dx * dx + dz * dz < 0.45f * 0.45f && bin >= 0 && bin < 40) hist[bin]++;
        }
        fprintf(stderr, "[%s] histograma y-pies (bins de 0,25 m desde -2,5 m) de vértices a <0,45 m del jugador:", tag);
        for (int b = 0; b < 40; b++) fprintf(stderr, " %d", hist[b]);
        fprintf(stderr, "\n");
    }
    if (cnt) fprintf(stderr, "[%s] vértices a <0,45 m del jugador (x,z): %d, y de %.3f a %.3f (altura %.3f m)\n", tag, cnt, lo, hi, hi - lo);
}

static void write_obj(const VrScene *s, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# escena VR (m, Y arriba); solo 3D; vt = uv/256 (v invertida)\n");
    uint32_t n = 0;
    for (uint32_t bi = 0; bi < s->nb; bi++) {
        const VrBatch *b = &s->b[bi];
        if (b->kind == 2) continue;
        fprintf(f, "g batch%u_k%d_src%u\n", bi, b->kind, b->src_op);
        for (uint32_t i = 0; i < b->count; i++) {
            const VrVtx *v = &s->v[b->first + i];
            fprintf(f, "v %.4f %.4f %.4f\nvt %.5f %.5f\n", v->pos[0], v->pos[1], v->pos[2], v->uv[0] / 256.0f, 1.0f - v->uv[1] / 256.0f);
        }
        for (uint32_t i = 0; i < b->count; i += 3) fprintf(f, "f %u/%u %u/%u %u/%u\n", n + i + 1, n + i + 1, n + i + 2, n + i + 2, n + i + 3, n + i + 3);
        n += b->count;
    }
    fclose(f);
}

static void anaglyph(const Img *L, const Img *R, uint8_t *o) {
    for (size_t i = 0; i < (size_t)L->W * L->H; i++) {
        int gl = (L->rgb[i*3] * 30 + L->rgb[i*3+1] * 59 + L->rgb[i*3+2] * 11) / 100, gr = (R->rgb[i*3] * 30 + R->rgb[i*3+1] * 59 + R->rgb[i*3+2] * 11) / 100;
        o[i*3] = (uint8_t)gl; o[i*3+1] = (uint8_t)gr; o[i*3+2] = (uint8_t)gr;
    }
}

static int target_frame(uint32_t f) { for (int i = 0; i < nat; i++) if (at[i] == f) return 1; return 0; }

static void handle_list(uint32_t f, const GpuDrawList *l) {
    if (!vram_before) vram_before = malloc(sizeof vram);
    memcpy(vram_before, ref_vram(rv), sizeof vram);
    if (l->vram_init) memcpy(vram_before, l->vram_init, sizeof vram);
    if (ncaps >= MAXT) return;
    Cap *c = &caps[ncaps];
    memset(c, 0, sizeof *c);
    c->frame = f;
    VrSceneOpts o = opts;
    if (!have_anchor) {
        /* ancla fija del mundo VR: pies del jugador del primer fotograma pedido */
        vr_scene_player_origin(&bs, &opts, anchor);
        have_anchor = 1;
    }
    o.origin_mode = 1; memcpy(o.origin_game, anchor, 12);
    VrScene s;
    int rc = vr_scene_build(l, &bs, &o, &s);
    fprintf(stderr, "[scene] frame %u rc=%d modo=%s eye_ok=%d  ops=%u polys=%u (3D %u, 2D %u) rects=%u lineas=%u | tris3D=%u (volteados %u) tris2D=%u | "
            "descartes: parcial %u cerca %u degenerado %u enorme %u nan %u | eventos VRAM %u batches %u\n",
            f, rc, bridge_mode_name(&bs), s.world_valid, s.stats.n_ops, s.stats.n_poly, s.stats.n_poly3d, s.stats.n_poly2d, s.stats.n_rect, s.stats.n_line,
            s.stats.n_tris3d, s.stats.n_flipped, s.stats.n_tris2d, s.stats.drop_partial, s.stats.drop_near, s.stats.drop_degenerate, s.stats.drop_huge,
            s.stats.drop_nan, s.stats.n_events, s.stats.n_batches);
    {
        /* descartes con la regla ANTERIOR (3D = todos fvalid; parcial = algunos fvalid; cerca = algún vz < near en polis fvalid) para comparar */
        uint32_t o3 = 0, opart = 0, onear = 0;
        for (uint32_t i = 0; i < l->nops; i++) {
            const GpuOp *po = &l->ops[i];
            if (po->type != GPU_OP_POLY) continue;
            int nv = po->nv == 4 ? 4 : 3, nf = 0, nearf = 0;
            for (int k = 0; k < nv; k++) { nf += po->v[k].fvalid; if (po->v[k].fvalid && po->v[k].vz < o.near_units) nearf = 1; }
            if (nf == nv) { if (nearf) onear++; else o3++; } else if (nf) opart++;
        }
        fprintf(stderr, "[scene]   regla previa (fvalid): polis3D-sin-cerca %u, parciales %u, con cerca %u | nueva (vvalid): 3D %u (con BEHIND %u, tris recortados %u), "
                "parcial %u, ambig %u, derivados-descartados %u, tris-detras %u, cerca-sin-BEHIND %u\n",
                o3, opart, onear, s.stats.n_poly3d, s.stats.n_poly_behind, s.stats.n_clipped, s.stats.drop_partial, s.stats.drop_ambig,
                s.stats.drop_derived, s.stats.drop_behind, s.stats.drop_near);
    }
    fprintf(stderr, "[scene]   filtro(clase 3): activo %d  copias: polis %u rects %u  recubrimientos %u  tris %u  clear(clase 2) %u  tint (%.2f,%.2f,%.2f) filas %d\n",
            s.filter.active, s.stats.n_filter_polys, s.stats.n_filter_rects, s.stats.n_filter_cover, s.stats.n_filter_tris, s.stats.n_clear,
            s.filter.tint[0], s.filter.tint[1], s.filter.tint[2], s.filter.nrows);
    fprintf(stderr, "[scene]   jugador (juego) pos=(%d,%d,%d) yaw=%d  eye_pos=(%d,%d,%d) focal=%d  eje_x=(%d,%d,%d) eje_y=(%d,%d,%d) eje_z=(%d,%d,%d)\n",
            bs.player_pos.x, bs.player_pos.y, bs.player_pos.z, bs.ctl_rot.y, bs.eye_pos[0], bs.eye_pos[1], bs.eye_pos[2], bs.eye_focal,
            bs.eye_axes[0][0], bs.eye_axes[1][0], bs.eye_axes[2][0], bs.eye_axes[0][1], bs.eye_axes[1][1], bs.eye_axes[2][1],
            bs.eye_axes[0][2], bs.eye_axes[1][2], bs.eye_axes[2][2]);
    {
        /* eye_pos alternativa: -R * view_t (la traslación mundo->vista que usa la GTE) */
        float ep[3];
        for (int r = 0; r < 3; r++) {
            ep[r] = 0;
            for (int cc = 0; cc < 3; cc++) ep[r] -= (float)bs.eye_axes[r][cc] / 4096.0f * (float)bs.view_t[cc];
        }
        fprintf(stderr, "[scene]   view_t=(%d,%d,%d) -> ojo desde view_t = (%.0f,%.0f,%.0f) vs eye_pos (%d,%d,%d)\n", bs.view_t[0], bs.view_t[1], bs.view_t[2],
                ep[0], ep[1], ep[2], bs.eye_pos[0], bs.eye_pos[1], bs.eye_pos[2]);
    }
    if (rc) return;
    c->frame = f;
    memcpy(c->game_view, s.game_view, sizeof c->game_view); memcpy(c->eye_vr, s.eye_vr, 12); memcpy(c->org, s.origin_game, 12);
    c->world_valid = s.world_valid;
    /* pies del jugador en mundo VR (con el ancla fija: el jugador se mueve) */
    {
        float po[3];
        vr_scene_player_origin(&bs, &opts, po);
        c->feet[0] = (po[0] - anchor[0]) * opts.scale; c->feet[1] = (po[1] - anchor[1]) * opts.scale;
        c->feet[2] = mirror_sign() * (po[2] - anchor[2]) * opts.scale;
    }
    /* copia de vértices 3D */
    uint32_t n3 = 0;
    for (uint32_t bi = 0; bi < s.nb; bi++) if (s.b[bi].kind != 2) n3 += s.b[bi].count;
    c->v3 = malloc((n3 ? n3 : 1) * sizeof(VrVtx));
    for (uint32_t bi = 0, k = 0; bi < s.nb; bi++) {
        if (s.b[bi].kind == 2) continue;
        memcpy(c->v3 + k, s.v + s.b[bi].first, s.b[bi].count * sizeof(VrVtx)); k += s.b[bi].count;
    }
    c->n3 = n3;
    char tag[32]; snprintf(tag, sizeof tag, "f%u", f);
    world_stats(c, tag);
    char p[512];
    snprintf(p, sizeof p, "%s/scene_%05u.obj", outdir, f); write_obj(&s, p);

    /* (a) réplica desde la cámara del juego frente al oráculo */
    {
        GpuDisplay d = l->disp;
        int W = d.w > 0 ? d.w : 320, H = d.h > 0 ? d.h : 240;
        Cam cam;
        float zc[3] = {s.game_view[8], s.game_view[9], s.game_view[10]}, xc[3] = {s.game_view[0], s.game_view[1], s.game_view[2]},
              yc[3] = {s.game_view[4], s.game_view[5], s.game_view[6]};
        norm3(zc); norm3(xc);
        memcpy(cam.e, s.eye_vr, 12); memcpy(cam.f, zc, 12); memcpy(cam.r, xc, 12);
        for (int i = 0; i < 3; i++) cam.u[i] = -yc[i];
        norm3(cam.u);
        int S = 2; /* aumento */
        cam.W = W * S; cam.H = H * S; cam.cx = cam.W * 0.5f; cam.cy = cam.H * 0.5f;
        float Hp = bs.eye_focal > 0 ? (float)bs.eye_focal : 320.0f, pcx = d.w * 0.5f, pcy = d.h * 0.5f;
        for (uint32_t k = 0; k < l->nops; k++) { /* H y punto principal reales del dibujo (el primer polígono con float) */
            const GpuOp *po = &l->ops[k];
            if (po->type == GPU_OP_POLY && po->proj_h > 0 && po->v[0].fvalid && po->v[0].vz > 0) {
                Hp = po->proj_h;
                pcx = po->v[0].fx - Hp * po->v[0].vx / po->v[0].vz - d.x;
                pcy = po->v[0].fy - Hp * po->v[0].vy / po->v[0].vz - d.y;
                break;
            }
        }
        cam.cx = pcx * S; cam.cy = pcy * S;
        cam.focal = Hp * S;
        fprintf(stderr, "[scene]   replay: H=%.1f (bridge focal=%d) punto principal (%.1f,%.1f) en la pantalla de %dx%d\n", Hp, bs.eye_focal, pcx, pcy, W, d.h > 0 ? d.h : 240);
        Img im = img_new(cam.W, cam.H);
        render_scene(&s, vram_before, &cam, &im);
        snprintf(p, sizeof p, "%s/replay_%05u.png", outdir, f);
        write_png(p, im.rgb, im.W, im.H);
        free(im.rgb); free(im.zb);
        /* oráculo: ref 1x tras aplicar la lista */
        ref_apply(rv, l, 0);
        int w, h;
        const uint8_t *oi = ref_present(rv, &d, &w, &h);
        if (oi) { snprintf(p, sizeof p, "%s/oracle_%05u.png", outdir, f); write_png(p, oi, w, h); }
    }
    /* (b) estéreo desde un punto de vista fijo en el mundo VR (mismo en todos los fotogramas) */
    {
        float zc[3] = {s.game_view[8], s.game_view[9], s.game_view[10]};
        norm3(zc);
        if (!have_fixed) {
            /* objetivo: pecho del jugador del primer fotograma; ojo 3,5 m atrás sobre la línea de la cámara del juego */
            fixed_tgt[0] = c->feet[0]; fixed_tgt[1] = c->feet[1] + 1.0f; fixed_tgt[2] = c->feet[2];
            for (int i = 0; i < 3; i++) fixed_eye[i] = fixed_tgt[i] - zc[i] * 4.0f;
            have_fixed = 1;
        }
        const float ipd = 0.064f;
        Cam cl, cr;
        cam_look(&cl, fixed_eye, fixed_tgt, 512, 384, 70);
        cr = cl;
        for (int i = 0; i < 3; i++) { cl.e[i] = fixed_eye[i] - cl.r[i] * ipd * 0.5f; cr.e[i] = fixed_eye[i] + cr.r[i] * ipd * 0.5f; }
        Img L = img_new(512, 384), R = img_new(512, 384);
        render_scene(&s, vram_before, &cl, &L);
        render_scene(&s, vram_before, &cr, &R);
        snprintf(p, sizeof p, "%s/fixedL_%05u.png", outdir, f); write_png(p, L.rgb, L.W, L.H);
        snprintf(p, sizeof p, "%s/fixedR_%05u.png", outdir, f); write_png(p, R.rgb, R.W, R.H);
        uint8_t *an = malloc((size_t)512 * 384 * 3);
        anaglyph(&L, &R, an);
        snprintf(p, sizeof p, "%s/fixed_ana_%05u.png", outdir, f); write_png(p, an, 512, 384);
        free(an);
        c->img_fix = L; c->img_fixR = R;
        /* (c) cabeza en el jugador: 1,6 m, mirando al yaw del jugador */
        float yaw = (float)bs.ctl_rot.y * (6.28318530718f / 4096.0f);
        float dz = cosf(yaw) * mirror_sign(), dx = sinf(yaw); /* yaw 0 = +Z, 1024 = +X en el mundo del juego */
        float he[3] = {c->feet[0], c->feet[1] + 1.6f, c->feet[2]}, ht[3] = {he[0] + dx, he[1], he[2] + dz};
        Cam hl, hr;
        cam_look(&hl, he, ht, 512, 384, 90);
        hr = hl;
        for (int i = 0; i < 3; i++) { hl.e[i] = he[i] - hl.r[i] * ipd * 0.5f; hr.e[i] = he[i] + hr.r[i] * ipd * 0.5f; }
        Img HL = img_new(512, 384), HR = img_new(512, 384);
        render_scene(&s, vram_before, &hl, &HL);
        render_scene(&s, vram_before, &hr, &HR);
        uint8_t *an2 = malloc((size_t)512 * 384 * 3);
        anaglyph(&HL, &HR, an2);
        snprintf(p, sizeof p, "%s/head_ana_%05u.png", outdir, f); write_png(p, an2, 512, 384);
        snprintf(p, sizeof p, "%s/headL_%05u.png", outdir, f); write_png(p, HL.rgb, 512, 384);
        free(an2); free(HL.rgb); free(HL.zb); free(HR.rgb); free(HR.zb);
    }
    ncaps++;
}

/* ---- comparación de vértices entre fotogramas ---- */
typedef struct { int32_t x, y, z; } Q;
static uint32_t qh(Q q) { return (uint32_t)(q.x * 73856093) ^ (uint32_t)(q.y * 19349663) ^ (uint32_t)(q.z * 83492791); }
static void compare(const Cap *A, const Cap *B) {
    /* A: tabla hash de los vértices de A (celda 5 cm). Para cada vértice de B: distancia al vértice de A más cercano (hasta 30 cm). */
    const float cell = 0.05f;
    size_t cap = 1; while (cap < (size_t)A->n3 * 2 + 8) cap <<= 1;
    int32_t *tab = malloc(cap * sizeof(int32_t));
    for (size_t i = 0; i < cap; i++) tab[i] = -1;
    for (uint32_t i = 0; i < A->n3; i++) {
        Q q = {(int32_t)floorf(A->v3[i].pos[0] / cell), (int32_t)floorf(A->v3[i].pos[1] / cell), (int32_t)floorf(A->v3[i].pos[2] / cell)};
        size_t h = qh(q) & (cap - 1);
        while (tab[h] >= 0) h = (h + 1) & (cap - 1);
        tab[h] = (int32_t)i;
    }
    const float tols[5] = {0.002f, 0.01f, 0.03f, 0.10f, 0.30f};
    uint32_t hit[5] = {0, 0, 0, 0, 0};
    for (uint32_t j = 0; j < B->n3; j++) {
        const float *p = B->v3[j].pos;
        float best = 1e9f;
        int cx = (int)floorf(p[0] / cell), cy = (int)floorf(p[1] / cell), cz = (int)floorf(p[2] / cell);
        for (int dx = -6; dx <= 6; dx++) for (int dy = -6; dy <= 6; dy++) for (int dz = -6; dz <= 6; dz++) {
            Q q = {cx + dx, cy + dy, cz + dz};
            size_t h = qh(q) & (cap - 1);
            while (tab[h] >= 0) {
                const float *a = A->v3[tab[h]].pos;
                float d2 = (a[0]-p[0])*(a[0]-p[0]) + (a[1]-p[1])*(a[1]-p[1]) + (a[2]-p[2])*(a[2]-p[2]);
                if (d2 < best) best = d2;
                h = (h + 1) & (cap - 1);
            }
        }
        best = sqrtf(best);
        for (int k = 0; k < 5; k++) hit[k] += best <= tols[k];
    }
    free(tab);
    fprintf(stderr, "[estabilidad] f%u -> f%u: vértices de B (%u) con algún vértice de A a <2 mm: %.1f%%, <1 cm: %.1f%%, <3 cm: %.1f%%, <10 cm: %.1f%%, <30 cm: %.1f%%\n",
            A->frame, B->frame, B->n3, B->n3 ? 100.0 * hit[0] / B->n3 : 0.0, B->n3 ? 100.0 * hit[1] / B->n3 : 0.0, B->n3 ? 100.0 * hit[2] / B->n3 : 0.0,
            B->n3 ? 100.0 * hit[3] / B->n3 : 0.0, B->n3 ? 100.0 * hit[4] / B->n3 : 0.0);
}

/* Emparejamiento por contenido: un triángulo con la misma página/CLUT/UV/color (vértices ordenados) en dos fotogramas es el
 * mismo polígono del escenario si se dibuja entero (el juego lo trocea según la cámara: solo cuentan los que coinciden);
 * se mide cuánto se mueve en mundo VR cada vértice. Lo que se mueve de verdad (jugador, objetos) sale en la cola. */
typedef struct { uint64_t key; uint32_t idx; int8_t ord[3]; } KT;
static int ktcmp(const void *a, const void *b) { uint64_t x = ((const KT *)a)->key, y = ((const KT *)b)->key; return x < y ? -1 : x > y; }
static int vtx_lt(const VrVtx *a, const VrVtx *b) {
    if (a->uv[0] != b->uv[0]) return a->uv[0] < b->uv[0];
    if (a->uv[1] != b->uv[1]) return a->uv[1] < b->uv[1];
    return a->rgba < b->rgba;
}
static size_t build_keys(const Cap *c, KT **out) {
    KT *k = malloc((c->n3 / 3 + 1) * sizeof(KT));
    size_t n = 0;
    for (uint32_t t = 0; t + 2 < c->n3; t += 3) {
        const VrVtx *v = &c->v3[t];
        int o[3] = {0, 1, 2};
        for (int i = 0; i < 3; i++) for (int j = 0; j < 2 - i; j++) if (vtx_lt(&v[o[j + 1]], &v[o[j]])) { int x = o[j]; o[j] = o[j + 1]; o[j + 1] = x; }
        if (!vtx_lt(&v[o[0]], &v[o[1]]) || !vtx_lt(&v[o[1]], &v[o[2]])) continue; /* vértices indistinguibles: ambiguo */
        uint64_t h = 1469598103934665603ull;
        #define MX(x) h = (h ^ (uint64_t)(x)) * 1099511628211ull
        MX(v[0].tpage); MX(v[0].clut); MX(v[0].flags & 0x3FF);
        for (int i = 0; i < 3; i++) { MX((uint32_t)v[o[i]].uv[0]); MX((uint32_t)v[o[i]].uv[1]); MX(v[o[i]].rgba); }
        k[n].key = h; k[n].idx = t; for (int i = 0; i < 3; i++) k[n].ord[i] = (int8_t)o[i];
        n++;
    }
    qsort(k, n, sizeof(KT), ktcmp);
    *out = k;
    return n;
}
static void compare_keyed(const Cap *A, const Cap *B) {
    KT *ka, *kb;
    size_t na = build_keys(A, &ka), nb = build_keys(B, &kb);
    uint32_t pairs = 0, hist[6] = {0};
    const float lim[5] = {0.002f, 0.01f, 0.03f, 0.10f, 0.50f};
    for (size_t j = 0; j < nb; j++) {
        if (j && kb[j - 1].key == kb[j].key) continue;
        if (j + 1 < nb && kb[j + 1].key == kb[j].key) continue;
        KT q = kb[j];
        KT *m = bsearch(&q, ka, na, sizeof(KT), ktcmp);
        if (!m) continue;
        size_t mi = (size_t)(m - ka);
        if ((mi && ka[mi - 1].key == q.key) || (mi + 1 < na && ka[mi + 1].key == q.key)) continue;
        float d = 0;
        {   /* se excluye el jugador (se mueve de verdad): centroide a < 1 m de sus pies en horizontal */
            float cxm = 0, czm = 0;
            for (int i = 0; i < 3; i++) { cxm += B->v3[q.idx + i].pos[0] / 3; czm += B->v3[q.idx + i].pos[2] / 3; }
            float dx = cxm - B->feet[0], dz = czm - B->feet[2];
            if (dx * dx + dz * dz < 1.0f) continue;
        }
        for (int i = 0; i < 3; i++) {
            const float *pa = A->v3[m->idx + m->ord[i]].pos, *pb = B->v3[q.idx + q.ord[i]].pos;
            float dd = sqrtf((pa[0]-pb[0])*(pa[0]-pb[0]) + (pa[1]-pb[1])*(pa[1]-pb[1]) + (pa[2]-pb[2])*(pa[2]-pb[2]));
            if (dd > d) d = dd;
        }
        pairs++;
        int b = 0; while (b < 5 && d > lim[b]) b++;
        hist[b]++;
    }
    fprintf(stderr, "[estabilidad-clave] f%u -> f%u: triángulos emparejados por contenido (sin el jugador, a >1 m de sus pies) %u de %zu/%zu; máx |Δ vértice| en mundo VR: <2 mm %.1f%%, <1 cm %.1f%%, <3 cm %.1f%%, <10 cm %.1f%%, <50 cm %.1f%%, más %.1f%%\n",
            A->frame, B->frame, pairs, na, nb, pairs ? 100.0 * hist[0] / pairs : 0.0, pairs ? 100.0 * hist[1] / pairs : 0.0, pairs ? 100.0 * hist[2] / pairs : 0.0,
            pairs ? 100.0 * hist[3] / pairs : 0.0, pairs ? 100.0 * hist[4] / pairs : 0.0, pairs ? 100.0 * hist[5] / pairs : 0.0);
    free(ka); free(kb);
}

static void finish(void) {
    for (int i = 0; i + 1 < ncaps; i++) {
        compare(&caps[i], &caps[i + 1]);
        compare_keyed(&caps[i], &caps[i + 1]);
        /* movimiento de la cámara del juego y del jugador entre los dos fotogramas */
        const Cap *A = &caps[i], *B = &caps[i + 1];
        fprintf(stderr, "[estabilidad] cámara del juego en mundo VR: f%u (%.3f,%.3f,%.3f) f%u (%.3f,%.3f,%.3f) | jugador pies: (%.3f,%.3f,%.3f) -> (%.3f,%.3f,%.3f)\n",
                A->frame, A->eye_vr[0], A->eye_vr[1], A->eye_vr[2], B->frame, B->eye_vr[0], B->eye_vr[1], B->eye_vr[2], A->feet[0], A->feet[1], A->feet[2],
                B->feet[0], B->feet[1], B->feet[2]);
        /* diferencia de imágenes desde el mismo punto de vista fijo: A en rojo, B en cian (el escenario fijo sale gris) */
        const Img *a = &A->img_fix, *b = &B->img_fix;
        if (a->rgb && b->rgb && a->W == b->W) {
            uint8_t *o = malloc((size_t)a->W * a->H * 3);
            uint64_t both = 0, same = 0;
            for (size_t k = 0; k < (size_t)a->W * a->H; k++) {
                int ga = (a->rgb[k*3]*30 + a->rgb[k*3+1]*59 + a->rgb[k*3+2]*11)/100, gb = (b->rgb[k*3]*30 + b->rgb[k*3+1]*59 + b->rgb[k*3+2]*11)/100;
                o[k*3] = (uint8_t)ga; o[k*3+1] = (uint8_t)gb; o[k*3+2] = (uint8_t)gb;
                if (a->zb[k] > 0 && b->zb[k] > 0) { both++; if (abs(ga - gb) < 24) same++; }
            }
            char p[512];
            snprintf(p, sizeof p, "%s/stab_%05u_%05u.png", outdir, A->frame, B->frame);
            write_png(p, o, a->W, a->H);
            free(o);
            fprintf(stderr, "[estabilidad] imágenes desde el punto fijo: píxeles con geometría en ambos %llu, de ellos con luminancia casi igual (<24): %.1f%%\n",
                    (unsigned long long)both, both ? 100.0 * (double)same / (double)both : 0.0);
        }
    }
}

static int scan_filter;
static void on_alarm(int s) { (void)s; rt_fatal(NULL, "sin VBlank en 60 s"); }

static void on_frame(Cpu *c, uint32_t f) {
    (void)c;
    alarm(60);
    script_pad_frame(f);
    bridge_read(psx_ram, &bs);
    gpu_capture_vblank();
    const GpuDrawList *l;
    while ((l = gpu_capture_frame())) {
        if (scan_filter) {
            VrScene sc;
            if (vr_scene_build(l, &bs, &opts, &sc) == 0 && sc.filter.active)
                fprintf(stderr, "[scan] f%u modo=%s polis %u rects %u recub %u clear %u tint (%.2f,%.2f,%.2f) filas %d cover abr %d rgb (%.2f,%.2f,%.2f) disp(%d,%d,%d,%d) 3D %u\n", f, bridge_mode_name(&bs),
                        sc.stats.n_filter_polys, sc.stats.n_filter_rects, sc.stats.n_filter_cover, sc.stats.n_clear, sc.filter.tint[0], sc.filter.tint[1], sc.filter.tint[2],
                        sc.filter.nrows, sc.filter.cover_abr, sc.filter.cover_rgb[0], sc.filter.cover_rgb[1], sc.filter.cover_rgb[2], l->disp.x, l->disp.y, l->disp.w, l->disp.h, sc.stats.n_tris3d);
        }
        if (target_frame(f)) handle_list(f, l);
        else ref_apply(rv, l, 0);
    }
    if (f >= max_frames) {
        finish();
        exit(0);
    }
}
static uint32_t rd32f(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

int main(int argc, char **argv) {
    const char *exe = NULL, *disc = getenv("MV_DISC");
    opts = vr_scene_opts_default();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--disc") && i + 1 < argc) disc = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "--at") && i + 1 < argc) {
            char *dup = strdup(argv[++i]), *sv = NULL;
            for (char *t = strtok_r(dup, ",", &sv); t && nat < MAXT; t = strtok_r(NULL, ",", &sv)) at[nat++] = (uint32_t)strtoul(t, 0, 0);
            free(dup);
        } else if (!strcmp(argv[i], "--pad") && i + 1 < argc) { if (pad_add_script(argv[++i])) return 1; }
        else if (!strcmp(argv[i], "--pad-every") && i + 1 < argc) { if (pad_add_every(argv[++i])) return 1; }
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) opts.scale = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--filter") && i + 1 < argc) opts.screen_filter = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scan-filter")) scan_filter = 1, opts.screen_filter = 1;   /* una línea [scan] por lista con clase 3 activa */
        else if (!strcmp(argv[i], "--raw-eye")) opts.eye_normalize = 0;
        else if (!strcmp(argv[i], "--eye-vt")) opts.eye_from_view_t = 1;
        else if (!strcmp(argv[i], "--feet") && i + 1 < argc) opts.feet_below_pos = (float)atof(argv[++i]);
        else if (!exe) exe = argv[i];
        else max_frames = (uint32_t)strtoul(argv[i], 0, 0);
    }
    if (!exe || !nat) { fprintf(stderr, "uso: %s <exe> [frames_max] --out DIR --at F1,F2 [--pad G] [--pad-every N:B]\n", argv[0]); return 1; }
    for (int i = 0; i < nat; i++) if (at[i] > max_frames) max_frames = at[i];
    mkdir(outdir, 0755);
    FILE *f = fopen(exe, "rb");
    if (!f) { perror(exe); return 1; }
    static uint8_t buf[PSX_RAM_SIZE + 0x800];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n < 0x800 || memcmp(buf, "PS-X EXE", 8)) { fprintf(stderr, "no es PS-X EXE\n"); return 1; }
    uint32_t pc = rd32f(buf + 0x10), gp = rd32f(buf + 0x14), t = rd32f(buf + 0x18), sz = rd32f(buf + 0x1C);
    if (sz > n - 0x800 || (t & 0x1FFFFF) + sz > PSX_RAM_SIZE) { fprintf(stderr, "cabecera inválida\n"); return 1; }
    memcpy(&psx_ram[t & 0x1FFFFF], buf + 0x800, sz);
    if (disc && !cd_open(disc)) { fprintf(stderr, "sin discos\n"); return 1; }
    RefOpts ro = {1, 1, 0, 0};
    rv = ref_create(&ro);
    Cpu c = {0};
    c.r[28] = gp;
    c.r[29] = c.r[30] = 0x801FFFF0u;
    gte_reset();
    gpu_reset();
    gpu_capture_hiprec(1);
    gpu_capture_enable(1);
    rt_host_frame = on_frame;
    signal(SIGALRM, on_alarm);
    rt_fatal_armed = 1;
    if (setjmp(rt_fatal_jmp)) return 2;
    alarm(60);
    rt_call(&c, pc);
    return 0;
}
