/* camtest: validación dinámica de la cámara VR con el runtime headless y una cabeza sintética.
 *   camtest <exe_parcheado> <frames> <dir_salida>          (disco: MV_DISC; mismo guion de mando que el headless)
 * Opciones por entorno: CAM_HIDE=0 (no ocultar cabeza), CAM_HFOV=100 (grados de la vista previa), CAM_ENGINE_H=0,
 *   CAM_NOVR=1 (no activar la cámara VR: línea base), CAM_PHASE=ini fija el tiempo inicial de las fases (depuración).
 * Fases (t = frames desde que la cámara VR está activa): quieto, emparejado (guiñada alterna +-20 grados por frame),
 * giro 360 grados de guiñada y vaivén de cabeceo +-60 grados. De cada lista de dibujo capturada (geometría de alta
 * precisión) se hace una vista previa de campo ancho re-proyectando con vrcam_delta y se escribe PPM. */
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

extern jmp_buf rt_fatal_jmp;
extern int rt_fatal_armed;

/* ---- guion de mando ---- */
typedef struct { uint32_t start, dur, every; uint16_t mask; } PadEv;
static PadEv pad_ev[16];
static int npad_ev;
static int f_act = -1, cur_t = -1;
static void script_pad(uint32_t f) {
    static uint16_t last = 0xFFFF;
    uint16_t b = 0;
    if (f_act >= 0) {   /* con la cámara VR activa: solo la prueba de marcha (adelante) en dos ventanas */
        int t = cur_t;
        if ((t >= 10 && t < 50) || (t >= 1340 && t < 1380)) b |= 1u << 4;
    } else
    for (int i = 0; i < npad_ev; i++) {
        const PadEv *p = &pad_ev[i];
        if (p->every) { if (f >= p->every && f % p->every < p->dur) b |= p->mask; }
        else if (f >= p->start && f < p->start + p->dur) b |= p->mask;
    }
    if (b != last) { pad_set_state(0, b, 0x80, 0x80, 0x80, 0x80); last = b; }
}

/* ---- cabeza sintética ---- */
static float g_yaw, g_elev;   /* grados */
static int head_fn(float m[16], double *t) {
    float y = g_yaw * 3.14159265f / 180, e = g_elev * 3.14159265f / 180, cy = cosf(y), sy = sinf(y), ce = cosf(e), se = sinf(e);
    float X[3] = {cy, 0, -sy}, Y[3] = {sy * se, ce, cy * se}, Z[3] = {sy * ce, -se, cy * ce};
    for (int i = 0; i < 3; i++) { m[i] = X[i]; m[4 + i] = Y[i]; m[8 + i] = Z[i]; }
    m[3] = m[7] = m[11] = 0; m[12] = 0; m[13] = 1.6f; m[14] = 0; m[15] = 1;
    *t = 0;
    return 1;
}

/* ---- imagen ---- */
#define PW 800
#define PH 640
static uint8_t img[PW * PH * 3];
static void write_ppm(const char *dir, const char *name, const uint8_t *rgb, int w, int h) {
    char p[512]; snprintf(p, sizeof p, "%s/%s.ppm", dir, name);
    FILE *f = fopen(p, "wb");
    if (!f) { perror(p); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h); fwrite(rgb, 1, (size_t)w * h * 3, f); fclose(f);
}

/* ---- muestreo de textura PS1 desde la VRAM ---- */
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
static uint8_t *g_img, *g_painted;
static uint64_t g_pix_drawn;

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
            if (o->flags & GPU_F_RAW) { R = tr; G = tg; B = tb; }
            else { R = tr * cr / 128; G = tg * cg / 128; B = tb * cb / 128; }
        } else { R = cr; G = cg; B = cb; }
        uint8_t *q = g_img + ((size_t)y * g_w + x) * 3;
        q[0] = (uint8_t)(R > 255 ? 255 : R); q[1] = (uint8_t)(G > 255 ? 255 : G); q[2] = (uint8_t)(B > 255 ? 255 : B);
        g_painted[y * g_w + x] = 1; g_pix_drawn++;
    }
}

/* recorta contra z >= near y dibuja */
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
            PV m;
            float *pa = (float *)a, *pb = (float *)b, *pm = (float *)&m;
            for (int k = 0; k < 8; k++) pm[k] = pa[k] + (pb[k] - pa[k]) * s;
            m.z = near;
            out[n++] = m;
        }
    }
    for (int i = 1; i + 1 < n; i++) raster_tri(o, &out[0], &out[i], &out[i + 1]);
}

typedef struct { uint32_t polys, drawn, partial, behind; } RStat;

/* Re-proyecta una lista con la transformación `d` (NULL = identidad) y el modelo estenopeico (f, cx, cy, w, h). */
static int g_dbg;
static RStat render_list(const GpuDrawList *l, const VrCamDelta *d, float f, float cx, float cy, int w, int h, uint8_t *im, uint8_t *pt) {
    g_f = f; g_cx = cx; g_cy = cy; g_w = w; g_h = h; g_img = im; g_painted = pt; g_pix_drawn = 0;
    memset(im, 0, (size_t)w * h * 3); memset(pt, 0, (size_t)w * h);
    RStat s = {0};
    for (uint32_t i = 0; i < l->nops; i++) {
        const GpuOp *o = &l->ops[i];
        if (o->type != GPU_OP_POLY) continue;
        s.polys++;
        int nvalid = 0;
        for (int k = 0; k < o->nv; k++) nvalid += o->v[k].vvalid;
        if (nvalid != o->nv) {
            if (nvalid) { s.partial++;
                if (getenv("CAM_DBG") && g_dbg && (s.partial % 40) == 1) { fprintf(stderr, "  parcial op%u nv=%d dox=%d doy=%d:", i, o->nv, o->dox, o->doy); for (int k = 0; k < o->nv; k++) fprintf(stderr, " (%d,%d vv%d f%d fl%d z%.0f)", o->v[k].x - o->dox, o->v[k].y - o->doy, o->v[k].vvalid, o->v[k].fvalid, o->v[k].vflags, o->v[k].vz); fprintf(stderr, "\n"); } }
            continue; }
        PV pv[4];
        for (int k = 0; k < o->nv; k++) {
            const GpuVtx *v = &o->v[k];
            float x = v->vx, y = v->vy, z = v->vz;
            if (d) {
                float X = d->A[0] * x + d->A[1] * y + d->A[2] * z + d->b[0], Y = d->A[3] * x + d->A[4] * y + d->A[5] * z + d->b[1],
                      Z = d->A[6] * x + d->A[7] * y + d->A[8] * z + d->b[2];
                x = X; y = Y; z = Z;
            }
            pv[k] = (PV){x, y, z, v->u, v->v, (o->flags & GPU_F_GOURAUD) ? v->r : o->v[0].r, (o->flags & GPU_F_GOURAUD) ? v->g : o->v[0].g,
                         (o->flags & GPU_F_GOURAUD) ? v->b : o->v[0].b};
            if (!(o->flags & GPU_F_GOURAUD)) { pv[k].r = o->v[0].r; pv[k].g = o->v[0].g; pv[k].b = o->v[0].b; }
        }
        int allb = 1;
        for (int k = 0; k < o->nv; k++) if (pv[k].z >= 24) allb = 0;
        if (allb) { s.behind++; continue; }
        s.drawn++;
        draw_clipped(o, &pv[0], &pv[1], &pv[2]);
        if (o->nv == 4) draw_clipped(o, &pv[1], &pv[2], &pv[3]);
    }
    return s;
}

/* ---- estado de la prueba ---- */
static RefR *r1;
static const char *outdir;
static uint32_t max_frames;
static BridgeState bs;
static int hist_n;
static struct { int f; int x, z; float view; int po; } hist[20000];
static uint32_t lists_seen, pair_ok, none_rec;
static double sum_cov; static int n_cov;
static float hfov_prev = 100.0f;
static GpuDisplay last_disp;

static void phase_pose(int t, int f) {
    (void)f;
    g_yaw = g_elev = 0;
    if (t < 120) return;
    if (t < 240) { int s = (t >> 1) & 1; g_yaw = s ? 20.0f : -20.0f; g_elev = s ? 30.0f : -30.0f; return; }
    if (t < 960) { g_yaw = (float)(t - 240) * 0.5f; return; }
    if (t < 1320) { g_elev = 60.0f * sinf((float)(t - 960) * 6.2831853f / 360.0f); return; }
    g_yaw = 90.0f;
}

static double mad_img(const uint8_t *a, const uint8_t *b, int n, const uint8_t *mask) {
    double s = 0; int cnt = 0;
    for (int i = 0; i < n; i++) {
        if (mask && !mask[i]) continue;
        for (int k = 0; k < 3; k++) s += abs((int)a[3 * i + k] - (int)b[3 * i + k]);
        cnt++;
    }
    return cnt ? s / (3.0 * cnt) : -1;
}

static VrCamRecord prev_copy, cur_copy;
static float pair_yaw0;
static int dump_this(int t) {
    if (t == 60 || t == 61 || t == 130 || t == 131) return 1;
    if (t >= 240 && t < 960 && (t - 240) % 45 == 0) return 1;
    if (t >= 960 && t < 1320 && (t - 960) % 45 == 0) return 1;
    return 0;
}

/* ---- consistencia del mundo: el mismo vértice estático, visto desde dos cámaras distintas, debe dar la misma posición mundo ---- */
typedef struct { uint64_t key; float w[4][3]; int nv; } WE;
static WE *wprev, *wcur; static size_t wnp, wnc, wcap;
static double *werr; static size_t nwerr, wecap;
static int wc_inv3(const float a[9], float o[9]) {
    float c00 = a[4] * a[8] - a[5] * a[7], c01 = a[5] * a[6] - a[3] * a[8], c02 = a[3] * a[7] - a[4] * a[6];
    float det = a[0] * c00 + a[1] * c01 + a[2] * c02;
    if (fabsf(det) < 1e-6f) return 0;
    float id = 1.0f / det;
    o[0] = c00 * id; o[1] = (a[2] * a[7] - a[1] * a[8]) * id; o[2] = (a[1] * a[5] - a[2] * a[4]) * id;
    o[3] = c01 * id; o[4] = (a[0] * a[8] - a[2] * a[6]) * id; o[5] = (a[2] * a[3] - a[0] * a[5]) * id;
    o[6] = c02 * id; o[7] = (a[1] * a[6] - a[0] * a[7]) * id; o[8] = (a[0] * a[4] - a[1] * a[3]) * id;
    return 1;
}
static int wecmp(const void *a, const void *b) { uint64_t x = ((const WE *)a)->key, y = ((const WE *)b)->key; return x < y ? -1 : x > y; }
static void world_collect(const GpuDrawList *l, const VrCamRecord *r) {
    if (!wcap) { wcap = 1 << 14; wprev = malloc(wcap * sizeof(WE)); wcur = malloc(wcap * sizeof(WE)); werr = malloc((wecap = 1 << 22) * sizeof(double)); }
    float Mt[9] = {r->M[0], r->M[3], r->M[6], r->M[1], r->M[4], r->M[7], r->M[2], r->M[5], r->M[8]}, Q[9];
    if (!wc_inv3(Mt, Q)) return;
    WE *tmp = wprev; wprev = wcur; wcur = tmp; wnp = wnc; wnc = 0;
    static uint32_t occ[1 << 16]; memset(occ, 0, sizeof occ);
    for (uint32_t i = 0; i < l->nops && wnc < wcap; i++) {
        const GpuOp *o = &l->ops[i];
        if (o->type != GPU_OP_POLY) continue;
        int ok = 1; for (int k = 0; k < o->nv; k++) if (!o->v[k].vvalid || o->v[k].vz < 200) ok = 0;
        if (!ok) continue;
        uint64_t h = 1469598103934665603ull;
        #define WMIX(x) h = (h ^ (uint64_t)(x)) * 1099511628211ull
        WMIX(o->nv); WMIX(o->tp); WMIX(o->tx); WMIX(o->ty); WMIX(o->clx); WMIX(o->cly);
        for (int k = 0; k < o->nv; k++) { WMIX(o->v[k].u); WMIX(o->v[k].v); }
        uint32_t *c = &occ[(h >> 7) & 0xFFFF]; h = h * 31 + (*c)++;
        WE *e = &wcur[wnc++]; e->key = h; e->nv = o->nv;
        for (int k = 0; k < o->nv; k++) for (int a = 0; a < 3; a++) {
            float v3[3] = {o->v[k].vx, o->v[k].vy, o->v[k].vz};
            e->w[k][a] = (float)r->eye[a] + Q[3 * a] * v3[0] + Q[3 * a + 1] * v3[1] + Q[3 * a + 2] * v3[2];
        }
    }
    qsort(wcur, wnc, sizeof(WE), wecmp);
    size_t a = 0, b = 0;
    while (a < wnp && b < wnc) {
        if (wprev[a].key < wcur[b].key) a++; else if (wprev[a].key > wcur[b].key) b++;
        else {
            for (int k = 0; k < wcur[b].nv && nwerr < wecap; k++) {
                double d = 0; for (int q = 0; q < 3; q++) { double x = wprev[a].w[k][q] - wcur[b].w[k][q]; d += x * x; }
                werr[nwerr++] = sqrt(d);
            }
            a++; b++;
        }
    }
}
static int dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static void world_report(const char *tag) {
    if (!nwerr) { fprintf(stderr, "[camtest] consistencia mundo %s: sin pares\n", tag); return; }
    qsort(werr, nwerr, sizeof(double), dcmp);
    size_t lt20 = 0; for (size_t i = 0; i < nwerr; i++) if (werr[i] < 20) lt20++;
    fprintf(stderr, "[camtest] consistencia del mundo %s: %zu vértices emparejados entre frames consecutivos; error de posición mundo (unidades ~mm): mediana %.1f, p75 %.1f, p90 %.1f; %.1f%% < 20\n", tag, nwerr,
            werr[nwerr / 2], werr[nwerr * 3 / 4], werr[nwerr * 9 / 10], 100.0 * lt20 / nwerr);
}
static void process_list(const GpuDrawList *l, int t, int f) {
    lists_seen++;
    last_disp = l->disp;
    static uint8_t *pt;
    if (!pt) pt = malloc(PW * PH);
    ref_apply(r1, l, 1);
    vram = ref_vram(r1);
    uint32_t n = l->index + 1;
    uint32_t sq = 0; for (uint32_t i = 0; i < l->nops; i++) if (l->ops[i].type == GPU_OP_POLY && l->ops[i].v[0].vvalid) { sq = l->ops[i].seq; break; }
    const VrCamRecord *r = vrcam_seq_record(sq);
    if (!r && getenv("CAM_NOVR")) { static VrCamRecord dummy; r = &dummy; }
    if (!r) { none_rec++; if (getenv("CAM_DBG") && (lists_seen % 400) == 1) fprintf(stderr, "[dbg] lista %u idx=%u envios=%u\n", lists_seen, l->index, vrcam_draw_count()); return; }
    if (r->serial != cur_copy.serial) { prev_copy = cur_copy; cur_copy = *r; }
    int novr = getenv("CAM_NOVR") != NULL;
    if (!r->active && !novr) return;
    if (t < 0) return;
    pair_ok++;
    /* proyección del motor (primer polígono 3D con proyección) para el centro de la imagen del juego */
    float eh = 0, eox = 0, eoy = 0;
    for (uint32_t i = 0; i < l->nops; i++) if (l->ops[i].type == GPU_OP_POLY && l->ops[i].proj_h > 0) { eh = l->ops[i].proj_h; eox = l->ops[i].proj_ox + l->ops[i].dox; eoy = l->ops[i].proj_oy + l->ops[i].doy; break; }
    VrCamDelta d;
    float head[16]; double tt; head_fn(head, &tt);
    if (novr) { memset(&d, 0, sizeof d); d.A[0] = d.A[4] = d.A[8] = 1; }
    else if (!vrcam_delta(r, r->head, &d)) return;
    /* a) comprobación de identidad: re-proyectar con la pose del registro (y la proyección del motor) vs imagen del oráculo */
    if (t == 60 || t == 61 || t == 130 || t == 131) {
        int w = 320, h = 256;
        static uint8_t a[320 * 256 * 3], p2[320 * 256];
        g_dbg = (t == 60 || t == 600);
        RStat s = render_list(l, &d, eh, eox - l->disp.x, eoy - l->disp.y, w, h, a, p2);
        int ow, oh;
        const uint8_t *orc = ref_present(r1, &l->disp, &ow, &oh);
        double m = mad_img(a, orc, w * h, p2);
        char nm[64]; snprintf(nm, sizeof nm, "id_t%d_re", t); write_ppm(outdir, nm, a, w, h);
        snprintf(nm, sizeof nm, "id_t%d_orc", t); write_ppm(outdir, nm, orc, ow, oh);
        /* emparejamiento desplazado: registro n-1 */
        const VrCamRecord *r2 = prev_copy.serial ? &prev_copy : NULL;
        double m2 = -1;
        if (r2 && r2->active) {
            VrCamDelta d2;
            if (vrcam_delta(r2, r2->head, &d2)) {
                render_list(l, &d2, eh, eox - l->disp.x, eoy - l->disp.y, w, h, a, p2);
                m2 = mad_img(a, orc, w * h, p2);
            }
        }
        fprintf(stderr, "[camtest] t=%d (f=%d): identidad vs oráculo, error medio |d| = %.2f (emparejado con registro vecino: %.2f); polis %u dibujados %u parciales %u  H=%.0f rec: yaw=%.1f pitch=%.1f serial=%u (envío %u)\n",
                t, f, m, m2, s.polys, s.drawn, s.partial, eh, r->yaw_deg, r->pitch_deg, r->serial, n);
    }
    /* a2) emparejado: con la cabeza FIJA (guiñada 0) dos frames consecutivos cuyo registro difiere ~40 grados deben dar la misma
     * escena si el registro asociado es el correcto; con el registro vecino, no. */
    static int pair_n;
    if (t >= 130 && t < 240) world_collect(l, r);
    if (t >= 130 && t < 240 && pair_n < 2) {
        int pn = pair_n++;
        static uint8_t ia[2][PW * PH * 3], iw[2][PW * PH * 3], pa[PW * PH];
        float save_y = g_yaw, save_e = g_elev; g_yaw = g_elev = 0;
        float h0[16]; double t0; head_fn(h0, &t0);
        g_yaw = save_y; g_elev = save_e;
        VrCamDelta dd; float fp = (PW / 2.0f) / tanf(hfov_prev * 3.14159265f / 360.0f);
        if (vrcam_delta(r, h0, &dd)) render_list(l, &dd, fp, PW / 2.0f, PH / 2.0f, PW, PH, ia[pn], pa);
        const VrCamRecord *rw = prev_copy.serial ? &prev_copy : NULL;
        if (rw && vrcam_delta(rw, h0, &dd)) render_list(l, &dd, fp, PW / 2.0f, PH / 2.0f, PW, PH, iw[pn], pa);
        if (pn == 1) {
            double mc = mad_img(ia[0], ia[1], PW * PH, NULL), mw = mad_img(iw[0], iw[1], PW * PH, NULL);
            fprintf(stderr, "[camtest] emparejado (cabeza fija, frames con yaw de motor %.0f y %.0f): diferencia entre frames consecutivos con el registro correcto = %.2f, con el registro vecino = %.2f\n", pair_yaw0, r->yaw_deg, mc, mw);
            write_ppm(outdir, "pair_a0", ia[0], PW, PH); write_ppm(outdir, "pair_a1", ia[1], PW, PH); write_ppm(outdir, "pair_w1", iw[1], PW, PH);
        } else pair_yaw0 = r->yaw_deg;
    }
    /* b) vista previa de campo ancho con la pose actual de la cabeza */
    if (dump_this(t)) {
        VrCamDelta dc;
        if (novr) dc = d; else if (!vrcam_delta(r, head, &dc)) return;
        float fp = (PW / 2.0f) / tanf(hfov_prev * 3.14159265f / 360.0f);
        RStat s = render_list(l, &dc, fp, PW / 2.0f, PH / 2.0f, PW, PH, img, pt);
        uint32_t cov = 0;
        for (int i = 0; i < PW * PH; i++) cov += pt[i];
        double c = 100.0 * cov / (PW * PH);
        sum_cov += c; n_cov++;
        char nm[64]; snprintf(nm, sizeof nm, "p_t%04d", t); write_ppm(outdir, nm, img, PW, PH);
        fprintf(stderr, "[camtest] t=%4d yaw=%6.1f elev=%6.1f -> vista %.0fx%.0f cubierta %.1f%%  polis %u dibujados %u parciales(perdidos) %u detras %u  eye=(%d,%d,%d) vista motor yaw=%.1f pitch=%.1f\n",
                t, g_yaw, g_elev, hfov_prev, hfov_prev * PH / PW, c, s.polys, s.drawn, s.partial, s.behind, r->eye[0], r->eye[1], r->eye[2], r->yaw_deg, r->pitch_deg);
    }
}

static void on_frame(Cpu *c, uint32_t f) {
    (void)c;
    alarm(60);
    script_pad(f);
    static int novr;
    if (getenv("CAM_NOVR")) novr = 1;
    bridge_read(psx_ram, &bs);
    vrcam_frame(psx_ram, &bs);
    if (f_act < 0 && (vrcam_active() || (getenv("CAM_NOVR") && bs.in_control && bs.has_player && f > 6000))) f_act = (int)f + 20;
    cur_t = f_act < 0 ? -1 : (int)f - f_act;
    if (cur_t >= 0) phase_pose(cur_t, (int)f); else g_yaw = g_elev = 0;
    if (hist_n < 20000) { hist[hist_n].f = (int)f; hist[hist_n].x = bs.player_pos.x; hist[hist_n].z = bs.player_pos.z; hist[hist_n].view = vrcam_view_yaw_deg(); hist[hist_n].po = vrcam_pad_origin(); hist_n++; }
    if (cur_t == 60 || cur_t == 62) {
        const int16_t *m = (const int16_t *)(psx_ram + (0x800B2B7Cu & 0x1FFFFF) + 0x10);   /* canal principal: matriz mundo->vista */
        double n[3]; for (int r = 0; r < 3; r++) n[r] = sqrt((double)m[3 * r] * m[3 * r] + (double)m[3 * r + 1] * m[3 * r + 1] + (double)m[3 * r + 2] * m[3 * r + 2]) / 4096.0;
        fprintf(stderr, "[camtest] t=%d norma de las filas de la matriz mundo->vista (x,y,z): %.4f %.4f %.4f (activo=%d)\n", cur_t, n[0], n[1], n[2], vrcam_active());
    }
    gpu_capture_vblank();
    const GpuDrawList *l;
    while ((l = gpu_capture_frame())) process_list(l, cur_t, (int)f);
    if (f % 500 == 0) fprintf(stderr, "[camtest] frame %u modo=%s activo=%d t=%d\n", f, bridge_mode_name(&bs), vrcam_active(), cur_t);
    (void)novr;
    if (f >= max_frames || cur_t >= 1400) {
        { GteHpStats g; gte_hp_stats(&g); fprintf(stderr, "[camtest] tabla GTE: almacenados=%u desalojos=%u omitidos=%u (sf0=%u sat=%u div=%u z=%u) consultas=%u aciertos=%u max/gen=%u\n", g.stored, g.evict, g.skipped, g.skip_sf, g.skip_sat, g.skip_ovf, g.skip_z, g.lookups, g.hits, g.max_gen_stored); }
        world_report(getenv("CAM_KEEP_OVERSCAN") ? "(keep_overscan)" : "");
        VrCamStats s; vrcam_stats(&s);
        fprintf(stderr, "[camtest] FIN f=%u: listas %u, con registro VR %u, sin registro %u; envíos %u; cámaras %u (VR %u) activaciones %u desactivaciones %u recentrados %u cabeza-perdida %u; cobertura media %.1f%%\n",
                f, lists_seen, pair_ok, none_rec, vrcam_draw_count(), s.builds, s.vr_builds, s.activations, s.deactivations, s.recenters, s.head_misses, n_cov ? sum_cov / n_cov : 0);
        { /* marcha hacia delante con la mirada fija: dirección de desplazamiento frente a la mirada */
            int w[2][2] = {{20, 60}, {1350, 1390}};
            for (int k = 0; k < 2 && f_act >= 0; k++) {
                int i0 = f_act + w[k][0], i1 = f_act + w[k][1], a = -1, b = -1;
                for (int i = 0; i < hist_n; i++) { if (hist[i].f == i0) a = i; if (hist[i].f == i1) b = i; }
                if (a < 0 || b < 0) continue;
                int dx = hist[b].x - hist[a].x, dz = hist[b].z - hist[a].z;
                double dir = atan2(dx, dz) * 180 / 3.14159265; if (dir < 0) dir += 360;
                fprintf(stderr, "[camtest] marcha 'adelante' t=%d..%d: desplazamiento (%d,%d), dirección %.0f; mirada(yaw motor)=%.0f origen_mando=%d => dirección-mirada=%.0f grados\n",
                        w[k][0], w[k][1], dx, dz, dir, hist[b].view, hist[b].po, fmod(dir - hist[b].view + 720, 360) > 180 ? fmod(dir - hist[b].view + 720, 360) - 360 : fmod(dir - hist[b].view + 720, 360));
            }
        }
        exit(0);
    }
}
static void on_alarm(int s) { (void)s; rt_fatal(NULL, "sin VBlank en 60 s"); }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "uso: %s <exe> <frames> <dir_salida>\n", argv[0]); return 1; }
    const char *exe = argv[1], *disc = getenv("MV_DISC");
    max_frames = (uint32_t)strtoul(argv[2], 0, 0);
    outdir = argv[3];
    mkdir(outdir, 0755);
    if (getenv("CAM_HFOV")) hfov_prev = (float)atof(getenv("CAM_HFOV"));
    pad_ev[npad_ev++] = (PadEv){120, 6, 0, 1u << 3};
    pad_ev[npad_ev++] = (PadEv){400, 6, 0, 1u << 3};
    pad_ev[npad_ev++] = (PadEv){0, 6, 97, 1u << 13};
    pad_ev[npad_ev++] = (PadEv){0, 6, 211, 1u << 7};
    FILE *f = fopen(exe, "rb");
    if (!f) { perror(exe); return 1; }
    static uint8_t buf[PSX_RAM_SIZE + 0x800];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n < 0x800 || memcmp(buf, "PS-X EXE", 8)) { fprintf(stderr, "no es PS-X EXE\n"); return 1; }
    uint32_t pc = rd32(buf + 0x10), gp = rd32(buf + 0x14), t = rd32(buf + 0x18), sz = rd32(buf + 0x1C);
    memcpy(&psx_ram[t & 0x1FFFFF], buf + 0x800, sz);
    if (disc && !cd_open(disc)) { fprintf(stderr, "sin discos\n"); return 1; }
    RefOpts o1 = {1, 1, 0};
    r1 = ref_create(&o1);
    VrCamConfig cfg; vrcam_config_default(&cfg);
    cfg.head = getenv("CAM_NOVR") ? NULL : head_fn;
    if (getenv("CAM_HIDE")) cfg.hide_head = atoi(getenv("CAM_HIDE"));
    if (getenv("CAM_ENGINE_H")) cfg.engine_h = atoi(getenv("CAM_ENGINE_H"));
    if (getenv("CAM_KEEP_OVERSCAN")) cfg.keep_overscan = atoi(getenv("CAM_KEEP_OVERSCAN"));
    if (getenv("CAM_LAG")) cfg.draw_lag = atoi(getenv("CAM_LAG"));
    if (getenv("CAM_UP")) cfg.eye_up = (float)atof(getenv("CAM_UP"));
    if (getenv("CAM_FWD")) cfg.eye_fwd = (float)atof(getenv("CAM_FWD"));
    vrcam_init(&cfg);
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
