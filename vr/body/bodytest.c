/* bodytest: validación de vr_body con el runtime headless, la cámara VR (cabeza sintética mirando hacia abajo) y poses
 * sintéticas de manos.
 *   bodytest <exe_parcheado> <frames> <dir_salida>     (disco: MV_DISC)
 * Por cada escenario (manos delante, a los lados, arriba, cruzadas, fuera de alcance, abajo, junto a la cara, una sola
 * mano, agarre abierto/cerrado, giro de muñeca) escribe en <dir_salida>:
 *   eye_<nombre>.ppm  lo que ve el usuario (lista de dibujo del juego re-proyectada con la cabeza, campo de 100 grados)
 *   ext_<nombre>.ppm  el cuerpo del jugador desde fuera (frente, lado izquierdo, arriba) y ampliado, dibujado aparte con
 *                     las matrices que el motor usa en la transformación (marcadores: hombro/codo/muñeca y objetivo)
 * y cuenta los VBlank que caen entre apply y restore (ahí un lector del anfitrión vería la pose IK; la lógica del juego no corre
 * en ese intervalo). La restauración byte a byte la comprueba test_body.
 * Variables de entorno: BT_PITCH (grados de inclinación de la cabeza hacia abajo, 55), BT_ONLY=nombre (solo ese escenario),
 * BT_NOBODY=1 (sin vr_body: línea base). */
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
#include "sys.h"
#include "ref_raster.h"
#include "vr_camera.h"
#include "vr_body.h"

extern jmp_buf rt_fatal_jmp;
extern int rt_fatal_armed;

/* ------------------------------------------------------------------ mando para llegar al juego ---- */
typedef struct { uint32_t start, dur, every; uint16_t mask; } PadEv;
static PadEv pad_ev[16];
static int npad_ev;
static int f_act = -1, cur_t = -1;
static void script_pad(uint32_t f) {
    static uint16_t last = 0xFFFF;
    uint16_t b = 0;
    if (f_act < 0)
        for (int i = 0; i < npad_ev; i++) {
            const PadEv *p = &pad_ev[i];
            if (p->every) { if (f >= p->every && f % p->every < p->dur) b |= p->mask; }
            else if (f >= p->start && f < p->start + p->dur) b |= p->mask;
        }
    if (b != last) { pad_set_state(0, b, 0x80, 0x80, 0x80, 0x80); last = b; }
}

/* ------------------------------------------------------------------ cabeza y manos sintéticas ---- */
static float g_yaw, g_elev;     /* grados; elev + = arriba */
static float g_head[3] = {0, 1.6f, 0};
static int head_fn(float m[16], double *t) {
    float y = g_yaw * 3.14159265f / 180, e = g_elev * 3.14159265f / 180, cy = cosf(y), sy = sinf(y), ce = cosf(e), se = sinf(e);
    float X[3] = {cy, 0, -sy}, Y[3] = {sy * se, ce, cy * se}, Z[3] = {sy * ce, -se, cy * ce};
    for (int i = 0; i < 3; i++) { m[i] = X[i]; m[4 + i] = Y[i]; m[8 + i] = Z[i]; }
    m[3] = m[7] = m[11] = 0; m[12] = g_head[0]; m[13] = g_head[1]; m[14] = g_head[2]; m[15] = 1;
    *t = 0;
    return 1;
}

typedef struct { int valid; float p[3]; float yaw, pitch, roll; } HandSpec;   /* grados; marco de mano: rot = Ry(yaw) Rx(pitch) Rz(roll) */
typedef struct { const char *name; HandSpec h[2]; float grip[2]; int armed; float head_pitch; } Scenario;   /* h[0] izquierda, h[1] derecha (del usuario) */
#define NS (sizeof SC / sizeof *SC)
/* Posiciones en m en el espacio de la cabeza: x derecha, y arriba (cabeza a 1,6), z atrás. Dedos = -Z de la mano. */
static const Scenario SC[] = {
    {"front",      {{1, {-0.20f, 1.20f, -0.42f}, 0, 0, 0},      {1, {0.20f, 1.20f, -0.42f}, 0, 0, 0}},        {0, 0}, 0, 55},
    {"front_fist", {{1, {-0.20f, 1.20f, -0.42f}, 0, 0, 0},      {1, {0.20f, 1.20f, -0.42f}, 0, 0, 0}},        {1, 1}, 0, 55},
    {"sides",      {{1, {-0.62f, 1.35f, -0.05f}, 90, 0, 0},     {1, {0.62f, 1.35f, -0.05f}, -90, 0, 0}},      {0, 0}, 0, 40},
    {"up",         {{1, {-0.18f, 1.95f, -0.15f}, 0, 90, 0},     {1, {0.18f, 1.95f, -0.15f}, 0, 90, 0}},       {0, 0}, 0, 20},
    {"crossed",    {{1, {0.22f, 1.18f, -0.40f}, -30, 0, 0},     {1, {-0.22f, 1.18f, -0.40f}, 30, 0, 0}},      {0, 0}, 0, 55},
    {"far",        {{1, {-0.25f, 1.50f, -1.00f}, 0, 0, 0},      {1, {0.25f, 1.50f, -1.00f}, 0, 0, 0}},        {0, 0}, 0, 35},
    {"low",        {{1, {-0.28f, 0.85f, -0.10f}, 0, -90, 0},    {1, {0.28f, 0.85f, -0.10f}, 0, -90, 0}},      {0, 0}, 0, 65},
    {"face",       {{1, {-0.10f, 1.52f, -0.24f}, 0, 70, 0},     {1, {0.10f, 1.52f, -0.24f}, 0, 70, 0}},       {0, 0}, 0, 30},
    {"onehand",    {{1, {-0.22f, 1.22f, -0.45f}, 0, 0, 0},      {0}},                                        {0, 0}, 0, 55},
    {"handshake",  {{1, {-0.18f, 1.22f, -0.45f}, 0, 0, 90},     {1, {0.18f, 1.22f, -0.45f}, 0, 0, -90}},      {0, 0}, 0, 55},
    {"armed",      {{1, {-0.12f, 1.28f, -0.40f}, 0, 0, 0},      {1, {0.14f, 1.30f, -0.50f}, 0, 12, 0}},       {0, 0}, 1, 50},
    {"rest_down",  {{1, {-0.22f, 0.95f, 0.02f}, 0, -90, 0},     {1, {0.22f, 0.95f, 0.02f}, 0, -90, 0}},       {0, 0}, 0, 70},
};
static const Scenario *g_sc;
static float g_grip_override = -1;

static void euler_quat(float yaw, float pitch, float roll, float *q) {
    /* R = Ry(yaw) Rx(pitch) Rz(roll) */
    float a[3] = {pitch, yaw, roll};
    float cy = cosf(a[1] * 0.00872665f), sy = sinf(a[1] * 0.00872665f), cx = cosf(a[0] * 0.00872665f), sx = sinf(a[0] * 0.00872665f), cz = cosf(a[2] * 0.00872665f), sz = sinf(a[2] * 0.00872665f);
    float qy[4] = {0, sy, 0, cy}, qx[4] = {sx, 0, 0, cx}, qz[4] = {0, 0, sz, cz}, t[4];
    #define QM(o, a_, b_) do { float x = a_[3] * b_[0] + a_[0] * b_[3] + a_[1] * b_[2] - a_[2] * b_[1], y = a_[3] * b_[1] - a_[0] * b_[2] + a_[1] * b_[3] + a_[2] * b_[0], \
        z = a_[3] * b_[2] + a_[0] * b_[1] - a_[1] * b_[0] + a_[2] * b_[3], w = a_[3] * b_[3] - a_[0] * b_[0] - a_[1] * b_[1] - a_[2] * b_[2]; o[0] = x; o[1] = y; o[2] = z; o[3] = w; } while (0)
    QM(t, qy, qx); QM(q, t, qz);
}
int vrpose_get_hands(VrHandPose out[2]) {       /* definición fuerte: sustituye a la débil de vr_pose.c */
    memset(out, 0, 2 * sizeof *out);
    if (!g_sc || getenv("BT_NOHANDS")) return 0;
    int n = 0;
    for (int s = 0; s < 2; s++) {
        const HandSpec *h = &g_sc->h[s];
        if (!h->valid) continue;
        out[s].valid = 1;
        for (int i = 0; i < 3; i++) out[s].pos[i] = h->p[i] + (i == 1 ? 0 : 0) + (i == 0 ? g_head[0] : (i == 2 ? g_head[2] : 0));
        euler_quat(h->yaw, h->pitch, h->roll, out[s].rot);
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ imagen y rasterizador de la lista del juego ---- */
#define PW 800
#define PH 640
static uint8_t img[PW * PH * 3];
static void write_ppm(const char *dir, const char *name, const uint8_t *rgb, int w, int h) {
    char p[512]; snprintf(p, sizeof p, "%s/%s.ppm", dir, name);
    FILE *f = fopen(p, "wb");
    if (!f) { perror(p); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h); fwrite(rgb, 1, (size_t)w * h * 3, f); fclose(f);
}
static const uint16_t *vram;
static int tex_sample(const GpuOp *o, int u, int v, uint16_t *out) {
    u &= 255; v &= 255;
    if (o->twmx || o->twmy || o->twox || o->twoy) {
        u = (u & ~(o->twmx * 8)) | ((o->twox & o->twmx) * 8);
        v = (v & ~(o->twmy * 8)) | ((o->twoy & o->twmy) * 8);
    }
    int ty = (o->ty + v) & 511;
    uint16_t t;
    if (o->tp == 0) {
        uint16_t w = vram[ty * 1024 + ((o->tx + u / 4) & 1023)];
        int idx = (w >> ((u & 3) * 4)) & 15;
        t = vram[(o->cly & 511) * 1024 + ((o->clx + idx) & 1023)];
    } else if (o->tp == 1) {
        uint16_t w = vram[ty * 1024 + ((o->tx + u / 2) & 1023)];
        int idx = (w >> ((u & 1) * 8)) & 255;
        t = vram[(o->cly & 511) * 1024 + ((o->clx + idx) & 1023)];
    } else t = vram[ty * 1024 + ((o->tx + u) & 1023)];
    *out = t;
    return t != 0;
}
typedef struct { float x, y, z, u, v, r, g, b; } PV;
static float g_f, g_cx, g_cy;
static int g_w, g_h;
static uint8_t *g_img;
static void raster_tri(const GpuOp *o, const PV *a, const PV *b, const PV *c) {
    float sx[3], sy[3], iz[3];
    const PV *p[3] = {a, b, c};
    for (int i = 0; i < 3; i++) { iz[i] = 1.0f / p[i]->z; sx[i] = g_cx + g_f * p[i]->x * iz[i]; sy[i] = g_cy + g_f * p[i]->y * iz[i]; }
    float minx = fminf(sx[0], fminf(sx[1], sx[2])), maxx = fmaxf(sx[0], fmaxf(sx[1], sx[2]));
    float miny = fminf(sy[0], fminf(sy[1], sy[2])), maxy = fmaxf(sy[0], fmaxf(sy[1], sy[2]));
    int x0 = (int)fmaxf(0, floorf(minx)), x1 = (int)fminf(g_w - 1, ceilf(maxx)), y0 = (int)fmaxf(0, floorf(miny)), y1 = (int)fminf(g_h - 1, ceilf(maxy));
    float den = (sy[1] - sy[2]) * (sx[0] - sx[2]) + (sx[2] - sx[1]) * (sy[0] - sy[2]);
    if (fabsf(den) < 1e-6f) return;
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        float px = x + 0.5f, py = y + 0.5f;
        float w0 = ((sy[1] - sy[2]) * (px - sx[2]) + (sx[2] - sx[1]) * (py - sy[2])) / den;
        float w1 = ((sy[2] - sy[0]) * (px - sx[2]) + (sx[0] - sx[2]) * (py - sy[2])) / den;
        float w2 = 1 - w0 - w1;
        const float e = -0.002f;
        if (w0 < e || w1 < e || w2 < e) continue;
        float iw = w0 * iz[0] + w1 * iz[1] + w2 * iz[2];
        if (iw <= 0) continue;
        float k0 = w0 * iz[0] / iw, k1 = w1 * iz[1] / iw, k2 = w2 * iz[2] / iw;
        float u = k0 * a->u + k1 * b->u + k2 * c->u, v = k0 * a->v + k1 * b->v + k2 * c->v;
        float cr = k0 * a->r + k1 * b->r + k2 * c->r, cg = k0 * a->g + k1 * b->g + k2 * c->g, cb = k0 * a->b + k1 * b->b + k2 * c->b;
        float R, G, B;
        if (o->flags & GPU_F_TEXTURED) {
            uint16_t t;
            if (!tex_sample(o, (int)floorf(u + 0.001f), (int)floorf(v + 0.001f), &t)) continue;
            float tr = (t & 31) * 8.0f, tg = ((t >> 5) & 31) * 8.0f, tb = ((t >> 10) & 31) * 8.0f;
            if (o->flags & GPU_F_RAW) { R = tr; G = tg; B = tb; } else { R = tr * cr / 128; G = tg * cg / 128; B = tb * cb / 128; }
        } else { R = cr; G = cg; B = cb; }
        uint8_t *q = g_img + ((size_t)y * g_w + x) * 3;
        q[0] = (uint8_t)(R > 255 ? 255 : R); q[1] = (uint8_t)(G > 255 ? 255 : G); q[2] = (uint8_t)(B > 255 ? 255 : B);
    }
}
static void draw_clipped(const GpuOp *o, const PV *t0, const PV *t1, const PV *t2) {
    const float near = 24.0f;
    PV in[3] = {*t0, *t1, *t2}, out[5];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const PV *a = &in[i], *b = &in[(i + 1) % 3];
        int ia = a->z >= near, ib = b->z >= near;
        if (ia) out[n++] = *a;
        if (ia != ib) {
            float s = (near - a->z) / (b->z - a->z);
            PV m; float *pa = (float *)a, *pb = (float *)b, *pm = (float *)&m;
            for (int k = 0; k < 8; k++) pm[k] = pa[k] + (pb[k] - pa[k]) * s;
            m.z = near; out[n++] = m;
        }
    }
    for (int i = 1; i + 1 < n; i++) raster_tri(o, &out[0], &out[i], &out[i + 1]);
}
static void render_list(const GpuDrawList *l, const VrCamDelta *d, float f, float cx, float cy, int w, int h, uint8_t *im) {
    g_f = f; g_cx = cx; g_cy = cy; g_w = w; g_h = h; g_img = im;
    memset(im, 0, (size_t)w * h * 3);
    for (uint32_t i = 0; i < l->nops; i++) {
        const GpuOp *o = &l->ops[i];
        if (o->type != GPU_OP_POLY) continue;
        int nvalid = 0;
        for (int k = 0; k < o->nv; k++) nvalid += o->v[k].vvalid;
        if (nvalid != o->nv) continue;
        unsigned uf = 0;
        for (int k = 0; k < o->nv; k++) uf |= o->v[k].vflags;
        if ((uf & GPU_VF_AMBIG) && (uf & (GPU_VF_SAT | GPU_VF_BEHIND))) continue;
        PV pv[4];
        for (int k = 0; k < o->nv; k++) {
            const GpuVtx *v = &o->v[k];
            float x = v->vx, y = v->vy, z = v->vz;
            float X = d->A[0] * x + d->A[1] * y + d->A[2] * z + d->b[0], Y = d->A[3] * x + d->A[4] * y + d->A[5] * z + d->b[1], Z = d->A[6] * x + d->A[7] * y + d->A[8] * z + d->b[2];
            pv[k] = (PV){X, Y, Z, v->u, v->v, (o->flags & GPU_F_GOURAUD) ? v->r : o->v[0].r, (o->flags & GPU_F_GOURAUD) ? v->g : o->v[0].g, (o->flags & GPU_F_GOURAUD) ? v->b : o->v[0].b};
            if (!(o->flags & GPU_F_GOURAUD)) { pv[k].r = o->v[0].r; pv[k].g = o->v[0].g; pv[k].b = o->v[0].b; }
        }
        int allb = 1;
        for (int k = 0; k < o->nv; k++) if (pv[k].z >= 24) allb = 0;
        if (allb) continue;
        draw_clipped(o, &pv[0], &pv[1], &pv[2]);
        if (o->nv == 4) draw_clipped(o, &pv[1], &pv[2], &pv[3]);
    }
}

/* ------------------------------------------------------------------ vistas externas del cuerpo ---- */
#define R_(a) (emu_ram + ((a) & 0x1FFFFF))
static uint32_t U32(uint32_t a) { uint32_t v; memcpy(&v, R_(a), 4); return v; }
static int32_t I32(uint32_t a) { int32_t v; memcpy(&v, R_(a), 4); return v; }
static int16_t S16(uint32_t a) { int16_t v; memcpy(&v, R_(a), 2); return v; }
typedef struct { float a[3], b[3], c[3]; int part; } Tri;
static Tri *tris; static int ntris;
static float PWm[16][12];       /* matriz mundo de cada pieza: R (9) + t (3) */
static int nparts_body;

static void collect_body(void) {
    uint32_t ob = U32(0x800ACBB0), pset = U32(ob);
    int n = S16(pset + 0x2E);
    nparts_body = n < 16 ? n : 16;
    if (!tris) tris = malloc(sizeof(Tri) * 4000);
    ntris = 0;
    for (int i = 0; i < nparts_body; i++) {
        uint32_t pa = pset + 0x48 + 0x5C * (uint32_t)i;
        for (int k = 0; k < 9; k++) PWm[i][k] = S16(pa + 2 * k) / 4096.0f;
        for (int k = 0; k < 3; k++) PWm[i][9 + k] = (float)I32(pa + 0x14 + 4 * k);
        uint32_t m = U32(pa + 0x40);
        int nv = I32(m + 0x34), nf = I32(m + 4);
        uint32_t vp = U32(m + 0x38), ip = U32(m + 0x3C);
        if (nv <= 0 || nv > 256 || nf <= 0 || nf > 300) continue;
        float V[256][3];
        for (int k = 0; k < nv; k++) {
            float lv[3] = {(float)S16(vp + 8 * (uint32_t)k), (float)S16(vp + 8 * (uint32_t)k + 2), (float)S16(vp + 8 * (uint32_t)k + 4)};
            for (int r = 0; r < 3; r++) V[k][r] = PWm[i][3 * r] * lv[0] + PWm[i][3 * r + 1] * lv[1] + PWm[i][3 * r + 2] * lv[2] + PWm[i][9 + r];
        }
        for (int f = 0; f < nf && ntris < 3990; f++) {
            uint32_t w = U32(ip + 4 * (uint32_t)f);
            int i0 = w & 0x7F, i1 = (w >> 8) & 0x7F, i3 = (w >> 16) & 0x7F, i2 = (w >> 24) & 0x7F;      /* cuádrica de la consola: (v0, v1, v2, v3) = (b0, b1, b3, b2) */
            if (i0 >= nv || i1 >= nv || i2 >= nv || i3 >= nv) continue;
            Tri *t = &tris[ntris++]; t->part = i; memcpy(t->a, V[i0], 12); memcpy(t->b, V[i1], 12); memcpy(t->c, V[i2], 12);
            if (i3 != i1 && i3 != i2) { t = &tris[ntris++]; t->part = i; memcpy(t->a, V[i1], 12); memcpy(t->b, V[i2], 12); memcpy(t->c, V[i3], 12); }
        }
    }
}
static const uint8_t PCOL[16][3] = {{150, 150, 150}, {110, 130, 190}, {230, 90, 80}, {240, 150, 70}, {250, 220, 90}, {120, 120, 120}, {200, 160, 140},
                                    {70, 160, 230}, {70, 200, 200}, {80, 230, 120}, {100, 100, 100}, {120, 120, 120}, {140, 140, 140}, {100, 100, 100}, {120, 120, 120}, {140, 140, 140}};
typedef struct { float sx, sy, z; } P2;
static void tri2(uint8_t *im, float *zb, int w, int h, P2 a, P2 b, P2 c, const uint8_t *col) {
    int x0 = (int)fmaxf(0, floorf(fminf(a.sx, fminf(b.sx, c.sx)))), x1 = (int)fminf(w - 1, ceilf(fmaxf(a.sx, fmaxf(b.sx, c.sx))));
    int y0 = (int)fmaxf(0, floorf(fminf(a.sy, fminf(b.sy, c.sy)))), y1 = (int)fminf(h - 1, ceilf(fmaxf(a.sy, fmaxf(b.sy, c.sy))));
    float den = (b.sy - c.sy) * (a.sx - c.sx) + (c.sx - b.sx) * (a.sy - c.sy);
    if (fabsf(den) < 1e-6f) return;
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        float px = x + 0.5f, py = y + 0.5f;
        float w0 = ((b.sy - c.sy) * (px - c.sx) + (c.sx - b.sx) * (py - c.sy)) / den, w1 = ((c.sy - a.sy) * (px - c.sx) + (a.sx - c.sx) * (py - c.sy)) / den, w2 = 1 - w0 - w1;
        if (w0 < -0.001f || w1 < -0.001f || w2 < -0.001f) continue;
        float z = w0 * a.z + w1 * b.z + w2 * c.z;
        if (z <= zb[y * w + x]) continue;
        zb[y * w + x] = z;
        uint8_t *q = im + ((size_t)y * w + x) * 3; q[0] = col[0]; q[1] = col[1]; q[2] = col[2];
    }
}
static void disc(uint8_t *im, int w, int h, float cx, float cy, float r, const uint8_t *col) {
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++) for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
        if (x < 0 || y < 0 || x >= w || y >= h) continue;
        if ((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy) <= r * r) { uint8_t *q = im + ((size_t)y * w + x) * 3; q[0] = col[0]; q[1] = col[1]; q[2] = col[2]; }
    }
}
static void line2(uint8_t *im, int w, int h, float x0, float y0, float x1, float y1, const uint8_t *col) {
    int n = (int)fmaxf(fabsf(x1 - x0), fabsf(y1 - y0)) + 1;
    for (int i = 0; i <= n; i++) { int x = (int)(x0 + (x1 - x0) * i / n), y = (int)(y0 + (y1 - y0) * i / n); if (x >= 0 && y >= 0 && x < w && y < h) { uint8_t *q = im + ((size_t)y * w + x) * 3; q[0] = col[0]; q[1] = col[1]; q[2] = col[2]; } }
}
static float REFF[9];      /* marco de referencia (columnas: izquierda, arriba, delante) */
static float MARK[8][3]; static int NMARK;         /* marcadores en mundo */
static uint8_t MCOL[8][3];

/* panel w x h: vista 0 frente, 1 lado izquierdo, 2 arriba. fit = 1: encuadre sobre el torso y los brazos */
static void ext_panel(uint8_t *im, int w, int h, int view, int fit) {
    memset(im, 235, (size_t)w * h * 3);
    float *zb = malloc(sizeof(float) * (size_t)w * h);
    for (int i = 0; i < w * h; i++) zb[i] = -1e30f;
    float Xl[3] = {REFF[0], REFF[3], REFF[6]}, Yu[3] = {REFF[1], REFF[4], REFF[7]}, Zf[3] = {REFF[2], REFF[5], REFF[8]};
    float rx[3], uy[3], dz[3];
    if (view == 0) { memcpy(rx, Xl, 12); memcpy(uy, Yu, 12); memcpy(dz, Zf, 12); }
    else if (view == 1) { for (int i = 0; i < 3; i++) { rx[i] = -Zf[i]; uy[i] = Yu[i]; dz[i] = Xl[i]; } }
    else { for (int i = 0; i < 3; i++) { rx[i] = -Xl[i]; uy[i] = Zf[i]; dz[i] = Yu[i]; } }
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    for (int t = 0; t < ntris; t++) {
        if (fit && !(tris[t].part <= 9)) continue;
        const float *pp[3] = {tris[t].a, tris[t].b, tris[t].c};
        for (int k = 0; k < 3; k++) { float X = pp[k][0] * rx[0] + pp[k][1] * rx[1] + pp[k][2] * rx[2], Y = pp[k][0] * uy[0] + pp[k][1] * uy[1] + pp[k][2] * uy[2];
            lo[0] = fminf(lo[0], X); hi[0] = fmaxf(hi[0], X); lo[1] = fminf(lo[1], Y); hi[1] = fmaxf(hi[1], Y); }
    }
    for (int m = 0; m < NMARK; m++) { float X = MARK[m][0] * rx[0] + MARK[m][1] * rx[1] + MARK[m][2] * rx[2], Y = MARK[m][0] * uy[0] + MARK[m][1] * uy[1] + MARK[m][2] * uy[2];
        if (!fit) { lo[0] = fminf(lo[0], X); hi[0] = fmaxf(hi[0], X); lo[1] = fminf(lo[1], Y); hi[1] = fmaxf(hi[1], Y); } }
    float sc = fminf((w - 16) / fmaxf(hi[0] - lo[0], 1.0f), (h - 16) / fmaxf(hi[1] - lo[1], 1.0f));
    float ox = w / 2.0f - (lo[0] + hi[0]) * 0.5f * sc, oy = h / 2.0f + (lo[1] + hi[1]) * 0.5f * sc;
    #define PROJ(p, out) do { out.sx = ((p)[0] * rx[0] + (p)[1] * rx[1] + (p)[2] * rx[2]) * sc + ox; out.sy = oy - ((p)[0] * uy[0] + (p)[1] * uy[1] + (p)[2] * uy[2]) * sc; \
                              out.z = (p)[0] * dz[0] + (p)[1] * dz[1] + (p)[2] * dz[2]; } while (0)
    for (int t = 0; t < ntris; t++) {
        P2 a, b, c; PROJ(tris[t].a, a); PROJ(tris[t].b, b); PROJ(tris[t].c, c);
        float e1[3] = {tris[t].b[0] - tris[t].a[0], tris[t].b[1] - tris[t].a[1], tris[t].b[2] - tris[t].a[2]}, e2[3] = {tris[t].c[0] - tris[t].a[0], tris[t].c[1] - tris[t].a[1], tris[t].c[2] - tris[t].a[2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) + 1e-9f;
        float lit = fabsf((n[0] * dz[0] + n[1] * dz[1] + n[2] * dz[2]) / nl) * 0.65f + 0.35f;
        uint8_t col[3]; for (int k = 0; k < 3; k++) col[k] = (uint8_t)(PCOL[tris[t].part][k] * lit);
        tri2(im, zb, w, h, a, b, c, col);
    }
    uint8_t dark[3] = {40, 40, 40};
    for (int t = 0; t < ntris; t++) {   /* contornos finos para ver las piezas */
        P2 a, b, c; PROJ(tris[t].a, a); PROJ(tris[t].b, b); PROJ(tris[t].c, c);
        (void)dark;
        (void)a; (void)b; (void)c;
    }
    for (int m = 0; m < NMARK; m++) { P2 p; PROJ(MARK[m], p); disc(im, w, h, p.sx, p.sy, 4.0f, MCOL[m]); }
    free(zb);
}
static int want_ext; static char ext_name[64]; static const char *outdir;
static void on_apply(uint8_t *ram);

/* ------------------------------------------------------------------ comprobación de que la lógica no ve la IK ---- */
static uint8_t arm_snapshot[6][0x40]; static int snap_valid;
static uint32_t pa_of(int i) { uint32_t ob = U32(0x800ACBB0), pset = U32(ob); return pset + 0x48 + 0x5C
 * (uint32_t)i; }
static const int ARMP[6] = {2, 3, 4, 7, 8, 9};
static uint32_t chk_frames, chk_during_diff, vb_pending;
static void snap_arms(uint8_t out[6][0x40]) { for (int k = 0; k < 6; k++) memcpy(out[k], R_(pa_of(ARMP[k])), 0x40); }
static void on_apply(uint8_t *ram) {
    (void)ram;
    uint8_t cur[6][0x40]; snap_arms(cur);
    if (snap_valid && memcmp(cur, arm_snapshot, sizeof cur) != 0) chk_during_diff++;
    if (want_ext) {
        want_ext = 0;
        collect_body();
        if (getenv("BT_DBG")) {
            VrBodyView vw; memset(&vw, 0, sizeof vw); const VrCamRecord *rr = vrcam_last_record();
            float tz[3] = {S16(pa_of(1) + 4) / 4096.0f, S16(pa_of(1) + 10) / 4096.0f, S16(pa_of(1) + 16) / 4096.0f};
            float tx[3] = {S16(pa_of(1) + 0) / 4096.0f, S16(pa_of(1) + 6) / 4096.0f, S16(pa_of(1) + 12) / 4096.0f};
            fprintf(stderr, "[dbg] %s theta_play=%.1f eye=(%d,%d,%d) torso_t=(%d,%d,%d) torso col0=(%.2f %.2f %.2f) col2=(%.2f %.2f %.2f) pref=(%.2f %.2f %.2f)\n", ext_name, rr->theta_play_deg,
                    (int)rr->eye[0], (int)rr->eye[1], (int)rr->eye[2], I32(pa_of(1) + 0x14), I32(pa_of(1) + 0x18), I32(pa_of(1) + 0x1c), tx[0], tx[1], tx[2], tz[0], tz[1], tz[2], rr->p_ref[0], rr->p_ref[1], rr->p_ref[2]);
        }
        float R1[9]; for (int k = 0; k < 9; k++) R1[k] = PWm[1][k];
        float x[3] = {R1[0], R1[3], R1[6]}, y[3] = {R1[1], R1[4], R1[7]}, z[3] = {R1[2], R1[5], R1[8]};
        float xn = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]); for (int i = 0; i < 3; i++) { x[i] /= xn; }
        float d = x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; for (int i = 0; i < 3; i++) y[i] -= x[i] * d;
        float yn = sqrtf(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]); for (int i = 0; i < 3; i++) y[i] /= yn;
        z[0] = x[1] * y[2] - x[2] * y[1]; z[1] = x[2] * y[0] - x[0] * y[2]; z[2] = x[0] * y[1] - x[1] * y[0];
        for (int i = 0; i < 3; i++) { REFF[3 * i] = x[i]; REFF[3 * i + 1] = y[i]; REFF[3 * i + 2] = z[i]; }
        NMARK = 0;
        for (int s = 0; s < 2; s++) {
            const VrBodyDebug *db = vrbody_debug(s);
            if (!db->driven) continue;
            int pi[3] = {s ? 2 : 7, s ? 3 : 8, s ? 4 : 9};
            for (int k = 0; k < 3 && NMARK < 8; k++) { memcpy(MARK[NMARK], &PWm[pi[k]][9], 12); MCOL[NMARK][0] = 20; MCOL[NMARK][1] = 20; MCOL[NMARK][2] = 20; NMARK++; }
            if (NMARK < 8) { memcpy(MARK[NMARK], db->target_wrist, 12); MCOL[NMARK][0] = 220; MCOL[NMARK][1] = 0; MCOL[NMARK][2] = 220; NMARK++; }
        }
        static uint8_t pan[360 * 520 * 3]; static uint8_t out[1080 * 1040 * 3];
        for (int row = 0; row < 2; row++) for (int v = 0; v < 3; v++) {
            ext_panel(pan, 360, 520, v, row);
            for (int y = 0; y < 520; y++) memcpy(out + ((size_t)(row * 520 + y) * 1080 + (size_t)v * 360) * 3, pan + (size_t)y * 360 * 3, 360 * 3);
        }
        char nm[96]; snprintf(nm, sizeof nm, "ext_%s", ext_name);
        write_ppm(outdir, nm, out, 1080, 1040);
    }
}

/* ------------------------------------------------------------------ bucle ---- */
static BridgeState bs;
static int pending_eye = -1;         /* escenario cuya imagen se hace con la próxima lista válida */
static int sc_idx = -1, sc_t0;
#define SC_DUR 44
#define SC_SETTLE 30
static uint32_t lists_seen, eye_written, restore_checks;
static int eye_done[64];
static const char *only;

static void process_list(const GpuDrawList *l, int t) {
    lists_seen++;
    static RefR *r1; static uint8_t *dummy;
    if (!r1) { RefOpts o1 = {1, 1, 0}; r1 = ref_create(&o1); dummy = NULL; }
    (void)dummy;
    ref_apply(r1, l, 1);
    vram = ref_vram(r1);
    if (pending_eye < 0 || t < 0) return;
    uint32_t sq = 0; for (uint32_t i = 0; i < l->nops; i++) if (l->ops[i].type == GPU_OP_POLY && l->ops[i].v[0].vvalid) { sq = l->ops[i].seq; break; }
    const VrCamRecord *r = vrcam_seq_record(sq);
    if (!r || !r->active) return;
    VrCamDelta d;
    if (!vrcam_delta(r, r->head, &d)) return;
    float fp = (PW / 2.0f) / tanf(100.0f * 3.14159265f / 360.0f);
    render_list(l, &d, fp, PW / 2.0f, PH / 2.0f, PW, PH, img);
    char nm[96]; snprintf(nm, sizeof nm, "eye_%s", SC[pending_eye].name);
    write_ppm(outdir, nm, img, PW, PH);
    eye_done[pending_eye] = 1; eye_written++;
    fprintf(stderr, "[bodytest] escenario %-10s t=%d: imagen del ojo escrita\n", SC[pending_eye].name, t);
    pending_eye = -1;
}

static void on_frame(Cpu *c, uint32_t f) {
    (void)c;
    alarm(90);
    script_pad(f);
    bridge_read(emu_ram, &bs);
    vrcam_frame(emu_ram, &bs);
    if (f_act < 0 && vrcam_active()) f_act = (int)f + 20;
    cur_t = f_act < 0 ? -1 : (int)f - f_act;
    /* comprobación: al principio del frame (CPU parada en el VBlank) las piezas de los brazos no pueden tener la pose IK;
     * se guarda y se compara en cada apply con la que tenía el frame anterior no es válido (la animación cambia), así que lo que
     * se comprueba es que tras el dibujado (VBlank) las matrices coinciden con world(padre) o la animación: aquí basta con ver que
     * snap_valid sigue la secuencia apply -> restore */
    if (cur_t >= 0) {
        int si = -1;
        for (unsigned k = 0; k < NS; k++) if (cur_t >= (int)(k * SC_DUR) && cur_t < (int)((k + 1) * SC_DUR)) si = (int)k;
        if (si != sc_idx) { sc_idx = si; sc_t0 = cur_t; g_sc = si >= 0 ? &SC[si] : NULL; }
        if (si >= 0) {
            const Scenario *s = &SC[si];
            g_yaw = getenv("BT_YAW") ? (float)atof(getenv("BT_YAW")) : 0; g_elev = -(getenv("BT_PITCH") ? (float)atof(getenv("BT_PITCH")) : s->head_pitch);
            vrbody_set_grip(0, s->grip[0]); vrbody_set_grip(1, s->grip[1]); vrbody_set_armed(s->armed);
            int skip = only && strcmp(only, s->name);
            if (!skip && cur_t - sc_t0 == SC_SETTLE) { pending_eye = si; want_ext = 1; snprintf(ext_name, sizeof ext_name, "%s", s->name); }
        }
    }
    if (vrbody_pending()) vb_pending++;      /* VBlank entre apply y restore: un lector del anfitrión vería la pose IK */
    gpu_capture_vblank();
    const GpuDrawList *l;
    while ((l = gpu_capture_frame())) process_list(l, cur_t);
    if (f % 500 == 0) fprintf(stderr, "[bodytest] frame %u modo=%s vrcam=%d t=%d\n", f, bridge_mode_name(&bs), vrcam_active(), cur_t);
    if (f >= (uint32_t)atoi(getenv("BT_FRAMES") ? getenv("BT_FRAMES") : "100000") || (cur_t >= (int)(NS * SC_DUR) + 10)) {
        VrBodyStats st; vrbody_stats(&st);
        fprintf(stderr, "[bodytest] FIN f=%u: imágenes %u; vr_body: applies %u brazos %u restores %u (saltos: sin vista %u, modelo %u, fuera de pantalla %u, sin manos %u) estirados %u cerca %u hombro %u muñeca %u malas %u; VBlank con la pose IK aún puesta (entre apply y restore): %u\n",
                f, eye_written, st.applies, st.arms_driven, st.restores, st.skip_noview, st.skip_model, st.skip_offscreen, st.skip_nohands, st.stretched, st.too_close, st.shoulder_limit, st.wrist_limit, st.bad_solve, vb_pending);
        exit(0);
    }
}
static void on_alarm(int s) { (void)s; rt_fatal(NULL, "sin VBlank en 90 s"); }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* Comprobación de que la lógica del juego no ve la IK: en CADA VBlank (el juego duerme entre sus etapas) las piezas de los
 * brazos deben ser las que dejó el dibujado = las originales. Se compara con la copia que guardó el último apply
 * (arm_snapshot_pre): si hay un apply sin restore, el VBlank las ve distintas. */
static uint8_t pre_apply[6][0x40]; static int have_pre;
static void on_apply_pre(void) {}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "uso: %s <exe> <frames> <dir_salida>\n", argv[0]); return 1; }
    const char *exe = argv[1], *disc = getenv("MV_DISC");
    outdir = argv[3];
    only = getenv("BT_ONLY");
    mkdir(outdir, 0755);
    pad_ev[npad_ev++] = (PadEv){120, 6, 0, 1u << 3};
    pad_ev[npad_ev++] = (PadEv){400, 6, 0, 1u << 3};
    pad_ev[npad_ev++] = (PadEv){0, 6, 97, 1u << 13};
    pad_ev[npad_ev++] = (PadEv){0, 6, 211, 1u << 7};
    FILE *f = fopen(exe, "rb");
    if (!f) { perror(exe); return 1; }
    static uint8_t buf[EMU_RAM_SIZE + 0x800];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n < 0x800 || memcmp(buf, EXE_MAGIC, 8)) { fprintf(stderr, "ejecutable no válido\n"); return 1; }
    uint32_t pc = rd32(buf + 0x10), gp = rd32(buf + 0x14), t = rd32(buf + 0x18), sz = rd32(buf + 0x1C);
    memcpy(&emu_ram[t & 0x1FFFFF], buf + 0x800, sz);
    if (disc && !cd_open(disc)) { fprintf(stderr, "sin discos\n"); return 1; }
    VrCamConfig cfg; vrcam_config_default(&cfg);
    cfg.head = head_fn;
    vrcam_init(&cfg);
    VrBodyConfig bc; vrbody_config_default(&bc);
    vrbody_init(&bc);
    vrbody_attach_vrcam();
    if (getenv("BT_NOBODY")) vrbody_set_enabled(0);
    vrbody_set_apply_hook(on_apply);
    (void)on_apply_pre; (void)pre_apply; (void)have_pre; (void)chk_frames; (void)chk_during_diff; (void)restore_checks; (void)arm_snapshot; (void)snap_valid;
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
    alarm(90);
    rt_call(&c, pc);
    return 0;
}
