#include "ns_cpuref.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------- cámaras ---------------- */
static void norm3(float *v) { float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; } }
static void cross3(const float *a, const float *b, float *o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }

void ns_cam_basis(NsRCam *c, const float pos[3], const float right[3], const float down[3], const float fwd[3], float fov_h_deg, int w, int h) {
    memset(c, 0, sizeof *c);
    for (int k = 0; k < 3; k++) { c->A[k] = right[k]; c->A[3 + k] = down[k]; c->A[6 + k] = fwd[k]; }
    for (int r = 0; r < 3; r++) c->t[r] = -(c->A[r * 3] * pos[0] + c->A[r * 3 + 1] * pos[1] + c->A[r * 3 + 2] * pos[2]);
    c->w = w; c->h = h; c->cx = 0.5f * (float)w; c->cy = 0.5f * (float)h;
    c->fx = c->fy = 0.5f * (float)w / tanf(0.5f * fov_h_deg * (float)M_PI / 180.0f);
    c->znear = 1.0f;
}

void ns_cam_yaw_pitch(NsRCam *c, const float pos[3], float yaw_deg, float pitch_deg, float fov_h_deg, int w, int h) {
    float y = yaw_deg * (float)M_PI / 180.0f, p = pitch_deg * (float)M_PI / 180.0f;
    float fwd[3] = {sinf(y) * cosf(p), -sinf(p), cosf(y) * cosf(p)};
    float up0[3] = {0, -1, 0}, right[3], down[3];
    cross3(fwd, up0, right);                       /* fwd x (-Y) = derecha con Y abajo / Z delante */
    if (right[0] * right[0] + right[1] * right[1] + right[2] * right[2] < 1e-12f) { right[0] = 1; right[1] = right[2] = 0; }
    norm3(right);
    cross3(fwd, right, down);                      /* right x down = fwd -> down = fwd x right */
    norm3(down);
    ns_cam_basis(c, pos, right, down, fwd, fov_h_deg, w, h);
}

void ns_cam_game(NsRCam *c, const float A[9], const float t[3], float H, float ofx, float ofy, int ref_w, int ref_h, int w, int h) {
    memset(c, 0, sizeof *c);
    memcpy(c->A, A, 9 * sizeof(float));
    memcpy(c->t, t, 3 * sizeof(float));
    float sx = (float)w / (float)ref_w, sy = (float)h / (float)ref_h;
    c->fx = H * sx; c->fy = H * sy; c->cx = ofx * sx; c->cy = ofy * sy;
    c->w = w; c->h = h; c->znear = 1.0f;
}

/* ---------------- búferes ---------------- */
int ns_buf_alloc(NsRBuf *b, int w, int h) {
    b->w = w; b->h = h;
    b->c = calloc((size_t)w * h * 3, sizeof(float));
    b->iz = calloc((size_t)w * h, sizeof(float));
    if (!b->c || !b->iz) { ns_buf_free(b); return -1; }
    return 0;
}
void ns_buf_free(NsRBuf *b) { free(b->c); free(b->iz); b->c = b->iz = NULL; b->w = b->h = 0; }
void ns_buf_clear(NsRBuf *b, float r5, float g5, float b5) {
    size_t n = (size_t)b->w * b->h;
    for (size_t i = 0; i < n; i++) { b->c[i * 3] = r5; b->c[i * 3 + 1] = g5; b->c[i * 3 + 2] = b5; b->iz[i] = 0; }
}

/* ---------------- muestreo de VRAM ---------------- */
uint16_t ns_sample_texel(const uint16_t *vram, uint16_t tpage, uint16_t clut, uint32_t tw, int u, int v) {
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

/* ---------------- recorte ---------------- */
typedef struct { float x, y, z, u, v, r, g, b; } CV;

static int clip_plane(const CV *in, int n, CV *out, float nx, float ny, float nz, float d) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const CV *a = &in[i], *b = &in[(i + 1) % n];
        float da = nx * a->x + ny * a->y + nz * a->z - d, db = nx * b->x + ny * b->y + nz * b->z - d;
        if (da >= 0) out[m++] = *a;
        if ((da >= 0) != (db >= 0)) {
            float t = da / (da - db);
            const float *pa = &a->x, *pb = &b->x;
            float *po = &out[m].x;
            for (int k = 0; k < 8; k++) po[k] = pa[k] + (pb[k] - pa[k]) * t;
            m++;
        }
    }
    return m;
}

/* ---------------- rasterización ---------------- */
typedef struct {
    const NsRScene *s; const NsRCam *cam; NsRBuf *buf; uint32_t opts;
    float planes[5][4];            /* nx, ny, nz, d */
    size_t px_opaque, px_semi; size_t tris_drawn;
} Ctx;

static inline float sat31(float v) { return v < 0 ? 0 : v > 31 ? 31 : v; }

static inline void blend5(float *d, const float *f, int mode, int q) {
    for (int k = 0; k < 3; k++) {
        float b = d[k], r;
        switch (mode & 3) {
        case 0: r = q ? floorf((b + f[k]) * 0.5f) : (b + f[k]) * 0.5f; break;
        case 1: r = b + f[k]; break;
        case 2: r = b - f[k]; break;
        default: r = b + (q ? floorf(f[k] * 0.25f) : f[k] * 0.25f); break;
        }
        d[k] = sat31(r);
    }
}

/* pass 0 = opaco (z-write), 1 = semitransparente (mezcla, sin z-write). v: 3 vértices en espacio del ojo (z>0). */
static int raster_tri(Ctx *cx, const NsRTri *t, const CV *v, int pass) {
    const NsRCam *cam = cx->cam;
    NsRBuf *buf = cx->buf;
    const int q = !(cx->opts & NS_O_FLOAT);
    double sx[3], sy[3], iz[3];
    for (int k = 0; k < 3; k++) {
        iz[k] = 1.0 / (double)v[k].z;
        sx[k] = cam->cx + cam->fx * v[k].x * iz[k];
        sy[k] = cam->cy + cam->fy * v[k].y * iz[k];
    }
    double area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
    if (area == 0) return 0;
    double minx = fmin(sx[0], fmin(sx[1], sx[2])), maxx = fmax(sx[0], fmax(sx[1], sx[2]));
    double miny = fmin(sy[0], fmin(sy[1], sy[2])), maxy = fmax(sy[0], fmax(sy[1], sy[2]));
    int x0 = (int)floor(minx - 0.5), x1 = (int)ceil(maxx - 0.5), y0 = (int)floor(miny - 0.5), y1 = (int)ceil(maxy - 0.5);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > buf->w - 1) x1 = buf->w - 1;
    if (y1 > buf->h - 1) y1 = buf->h - 1;
    if (x0 > x1 || y0 > y1) return 0;
    /* w_k asociada al vértice k (arista opuesta) */
    double ea[3], eb[3], ec[3];
    for (int k = 0; k < 3; k++) {
        int i = (k + 1) % 3, j = (k + 2) % 3;
        ea[k] = (sy[i] - sy[j]) / area;   /* w_k = edge(i,j,p)/area: coordenada baricéntrica, >= 0 dentro sea cual sea la orientación */
        eb[k] = (sx[j] - sx[i]) / area;
        ec[k] = (sx[i] * sy[j] - sx[j] * sy[i]) / area;
    }
    /* regla de propiedad de aristas coincidentes: cada arista se la queda un solo triángulo (coeficientes negados en el vecino) */
    int own[3];
    for (int k = 0; k < 3; k++) {
        own[k] = (ea[k] > 0) || (ea[k] == 0 && eb[k] > 0);
    }
    const uint16_t *vram = cx->s->vram;
    const int tex = (t->flags & NS_TF_TEXTURED) != 0, semi = (t->flags & NS_TF_SEMI) != 0, raw = (t->flags & NS_TF_RAW) != 0;
    int touched = 0;
    for (int y = y0; y <= y1; y++) {
        double py = y + 0.5;
        for (int x = x0; x <= x1; x++) {
            double px = x + 0.5;
            double w0 = ea[0] * px + eb[0] * py + ec[0], w1 = ea[1] * px + eb[1] * py + ec[1], w2 = ea[2] * px + eb[2] * py + ec[2];
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            if ((w0 == 0 && !own[0]) || (w1 == 0 && !own[1]) || (w2 == 0 && !own[2])) continue;
            double piz = w0 * iz[0] + w1 * iz[1] + w2 * iz[2];
            if (!(piz > 0)) continue;
            size_t pi = (size_t)y * buf->w + x;
            float zb = buf->iz[pi];
            if (pass == 0) { if ((float)piz < zb * (1.0f - 2e-6f)) continue; }
            else if (zb > 0 && (float)piz < zb * (1.0f - 2e-6f)) continue;
            double b0 = w0 * iz[0] / piz, b1 = w1 * iz[1] / piz, b2 = w2 * iz[2] / piz;
            float col[3];
            col[0] = (float)(b0 * v[0].r + b1 * v[1].r + b2 * v[2].r);
            col[1] = (float)(b0 * v[0].g + b1 * v[1].g + b2 * v[2].g);
            col[2] = (float)(b0 * v[0].b + b1 * v[1].b + b2 * v[2].b);
            if (q) for (int k = 0; k < 3; k++) col[k] = floorf(col[k] + 0.5f);
            float f[3]; int kind;
            if (tex) {
                float u = (float)(b0 * v[0].u + b1 * v[1].u + b2 * v[2].u), vv = (float)(b0 * v[0].v + b1 * v[1].v + b2 * v[2].v);
                uint16_t tn = ns_sample_texel(vram, t->tpage, t->clut, t->tw, (int)floorf(u + 1e-4f), (int)floorf(vv + 1e-4f));
                if (tn == 0) continue;
                for (int k = 0; k < 3; k++) {
                    float c5 = (float)((tn >> (5 * k)) & 31u);
                    if (!raw) { c5 = c5 * col[k] / 128.0f; if (q) c5 = floorf(c5); }
                    f[k] = sat31(c5);
                }
                kind = (semi && (tn & 0x8000u)) ? 1 : 0;
            } else {
                for (int k = 0; k < 3; k++) { float c = col[k] < 0 ? 0 : col[k] > 255 ? 255 : col[k]; f[k] = q ? floorf(c / 8.0f) : c * (31.0f / 255.0f); }
                kind = semi ? 1 : 0;
            }
            if (kind != pass) continue;
            float *d = &buf->c[pi * 3];
            if (pass == 0) { d[0] = f[0]; d[1] = f[1]; d[2] = f[2]; buf->iz[pi] = (float)piz; cx->px_opaque++; }
            else { blend5(d, f, t->abr, q); if (zb == 0) buf->iz[pi] = (float)piz; cx->px_semi++; }
            touched = 1;
        }
    }
    return touched;
}

typedef struct { float key; uint32_t idx; } SemiRef;
static int semi_cmp(const void *a, const void *b) {
    const SemiRef *x = a, *y = b;
    if (x->key != y->key) return x->key > y->key ? -1 : 1;     /* lejos primero */
    return x->idx < y->idx ? -1 : x->idx > y->idx;             /* empate: orden original */
}

/* transforma + recorta; devuelve nº de triángulos del abanico en out (hasta 6 triángulos = 8 vértices), o 0 si se descarta. */
static int prep_tri(const Ctx *cx, const NsRTri *t, CV *poly, float *zavg) {
    const NsRCam *c = cx->cam;
    CV in[12], tmp[12];
    int n = 3, allin = 1;
    float dist[3][5]; int anyout[5] = {0}, allout[5] = {1, 1, 1, 1, 1};
    for (int k = 0; k < 3; k++) {
        const float *p = t->v[k].p;
        CV *o = &in[k];
        o->x = c->A[0] * p[0] + c->A[1] * p[1] + c->A[2] * p[2] + c->t[0];
        o->y = c->A[3] * p[0] + c->A[4] * p[1] + c->A[5] * p[2] + c->t[1];
        o->z = c->A[6] * p[0] + c->A[7] * p[1] + c->A[8] * p[2] + c->t[2];
        o->u = t->v[k].u; o->v = t->v[k].v; o->r = t->v[k].r; o->g = t->v[k].g; o->b = t->v[k].b;
        if (c->zreject > 0 && o->z <= c->zreject) return 0;
        for (int pl = 0; pl < 5; pl++) {
            const float *P = cx->planes[pl];
            dist[k][pl] = P[0] * o->x + P[1] * o->y + P[2] * o->z - P[3];
            if (dist[k][pl] < 0) anyout[pl] = 1; else allout[pl] = 0;
        }
    }
    for (int pl = 0; pl < 5; pl++) { if (allout[pl]) return 0; if (anyout[pl]) allin = 0; }
    *zavg = (in[0].z + in[1].z + in[2].z) * (1.0f / 3.0f);
    if (allin) { memcpy(poly, in, 3 * sizeof(CV)); return 3; }
    CV *a = in, *b = tmp;
    for (int pl = 0; pl < 5 && n > 0; pl++) {
        if (!anyout[pl]) continue;
        const float *P = cx->planes[pl];
        n = clip_plane(a, n, b, P[0], P[1], P[2], P[3]);
        CV *s = a; a = b; b = s;
    }
    if (n < 3) return 0;
    memcpy(poly, a, (size_t)n * sizeof(CV));
    return n;
}

static void setup_planes(Ctx *cx) {
    const NsRCam *c = cx->cam;
    float gxr = 2.0f * (c->w - c->cx) / c->fx + 1.0f, gxl = 2.0f * c->cx / c->fx + 1.0f;
    float gyb = 2.0f * (c->h - c->cy) / c->fy + 1.0f, gyt = 2.0f * c->cy / c->fy + 1.0f;
    if (gxr < 1) gxr = 1;
    if (gxl < 1) gxl = 1;
    if (gyb < 1) gyb = 1;
    if (gyt < 1) gyt = 1;
    float zn = c->znear > 0 ? c->znear : 1e-3f;
    float P[5][4] = {{0, 0, 1, zn}, {-1, 0, gxr, 0}, {1, 0, gxl, 0}, {0, -1, gyb, 0}, {0, 1, gyt, 0}};
    memcpy(cx->planes, P, sizeof P);
}

static void draw_poly(Ctx *cx, const NsRTri *t, const CV *poly, int n, int pass) {
    int hit = 0;
    for (int k = 1; k + 1 < n; k++) {
        CV tri[3] = {poly[0], poly[k], poly[k + 1]};
        hit |= raster_tri(cx, t, tri, pass);
    }
    if (hit && pass == 0) cx->tris_drawn++;
}

static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6; }

static void render_ctx(Ctx *cx) {
    const NsRScene *s = cx->s;
    setup_planes(cx);
    SemiRef *semi = NULL; size_t ns = 0, cap = 0;
    CV poly[12];
    for (size_t i = 0; i < s->ntris; i++) {
        const NsRTri *t = &s->tris[i];
        float zavg;
        int n = prep_tri(cx, t, poly, &zavg);
        if (!n) continue;
        int tex = (t->flags & NS_TF_TEXTURED) != 0, sm = (t->flags & NS_TF_SEMI) != 0;
        if (!sm || tex) draw_poly(cx, t, poly, n, 0);     /* opaco (en semitransparentes texturizadas, solo texels con bit15=0) */
        if (sm) {
            if (ns == cap) { cap = cap ? cap * 2 : 1024; semi = realloc(semi, cap * sizeof *semi); }
            semi[ns].key = zavg; semi[ns].idx = (uint32_t)i; ns++;
        }
    }
    if (ns) {
        qsort(semi, ns, sizeof *semi, semi_cmp);
        for (size_t j = 0; j < ns; j++) {
            const NsRTri *t = &s->tris[semi[j].idx];
            float zavg;
            int n = prep_tri(cx, t, poly, &zavg);
            if (n) draw_poly(cx, t, poly, n, 1);
        }
    }
    free(semi);
}

static double hole_fraction(const NsRBuf *b) {
    size_t n = (size_t)b->w * b->h, h = 0;
    for (size_t i = 0; i < n; i++) h += b->iz[i] == 0;
    return n ? (double)h / (double)n : 0;
}

void ns_render_view(const NsRScene *s, const NsRCam *cam, uint32_t opts, NsRBuf *out, NsRStats *st) {
    double t0 = now_ms();
    Ctx cx; memset(&cx, 0, sizeof cx);
    cx.s = s; cx.cam = cam; cx.buf = out; cx.opts = opts;
    render_ctx(&cx);
    if (st) {
        memset(st, 0, sizeof *st);
        st->tris_in = s->ntris; st->tris_drawn = cx.tris_drawn; st->px_opaque = cx.px_opaque; st->px_semi = cx.px_semi;
        st->hole_frac = hole_fraction(out); st->ms = now_ms() - t0;
    }
}

/* ---------------- cubemap / panorama ---------------- */
/* base de cada cara (filas: derecha, abajo, delante) respecto al ojo de la pose; r x d = f */
static const float FACE[6][9] = {
    {1, 0, 0, 0, 1, 0, 0, 0, 1},       /* delante */
    {0, 0, -1, 0, 1, 0, 1, 0, 0},      /* derecha */
    {-1, 0, 0, 0, 1, 0, 0, 0, -1},     /* atrás */
    {0, 0, 1, 0, 1, 0, -1, 0, 0},      /* izquierda */
    {1, 0, 0, 0, 0, 1, 0, -1, 0},      /* arriba */
    {1, 0, 0, 0, 0, -1, 0, 1, 0},      /* abajo */
};

typedef struct { const NsRScene *s; NsRCam cam; uint32_t opts; NsRBuf *buf; NsRStats st; } FaceJob;
static void *face_main(void *p) {
    FaceJob *j = p;
    ns_buf_clear(j->buf, 0, 0, 0);
    ns_render_view(j->s, &j->cam, j->opts, j->buf, &j->st);
    return NULL;
}

void ns_render_cube(const NsRScene *s, const NsRCam *pose, int n, float znear, uint32_t opts, NsRBuf faces[6], NsRStats *st) {
    double t0 = now_ms();
    FaceJob job[6];
    for (int f = 0; f < 6; f++) {
        NsRCam *c = &job[f].cam;
        memset(c, 0, sizeof *c);
        const float *F = FACE[f];
        for (int r = 0; r < 3; r++) {
            for (int k = 0; k < 3; k++) c->A[r * 3 + k] = F[r * 3] * pose->A[k] + F[r * 3 + 1] * pose->A[3 + k] + F[r * 3 + 2] * pose->A[6 + k];
            c->t[r] = F[r * 3] * pose->t[0] + F[r * 3 + 1] * pose->t[1] + F[r * 3 + 2] * pose->t[2];
        }
        c->w = c->h = n; c->cx = c->cy = 0.5f * (float)n; c->fx = c->fy = 0.5f * (float)n; c->znear = znear;
        job[f].s = s; job[f].opts = opts; job[f].buf = &faces[f];
    }
    if (opts & NS_O_1THREAD) { for (int f = 0; f < 6; f++) face_main(&job[f]); }
    else {
        pthread_t th[6]; int ok[6];
        for (int f = 0; f < 6; f++) ok[f] = pthread_create(&th[f], NULL, face_main, &job[f]) == 0;
        for (int f = 0; f < 6; f++) { if (ok[f]) pthread_join(th[f], NULL); else face_main(&job[f]); }
    }
    if (st) {
        memset(st, 0, sizeof *st);
        st->tris_in = s->ntris;
        double h = 0;
        for (int f = 0; f < 6; f++) { st->tris_drawn += job[f].st.tris_drawn; st->px_opaque += job[f].st.px_opaque; st->px_semi += job[f].st.px_semi; h += job[f].st.hole_frac; }
        st->hole_frac = h / 6.0; st->ms = now_ms() - t0;
    }
}

void ns_cube_to_equirect(const NsRBuf faces[6], const NsRCam *pose, NsRBuf *out) {
    (void)pose;
    int n = faces[0].w, W = out->w, H = out->h;
    for (int j = 0; j < H; j++) {
        float lat = (float)M_PI * 0.5f - ((float)j + 0.5f) / (float)H * (float)M_PI;
        float cl = cosf(lat), sl = sinf(lat);
        for (int i = 0; i < W; i++) {
            float lon = ((float)i + 0.5f) / (float)W * 2.0f * (float)M_PI - (float)M_PI;
            float d[3] = {cl * sinf(lon), -sl, cl * cosf(lon)};
            float ax = fabsf(d[0]), ay = fabsf(d[1]), az = fabsf(d[2]);
            int f;
            if (az >= ax && az >= ay) f = d[2] > 0 ? 0 : 2;
            else if (ax >= ay) f = d[0] > 0 ? 1 : 3;
            else f = d[1] > 0 ? 5 : 4;
            const float *F = FACE[f];
            float ex = F[0] * d[0] + F[1] * d[1] + F[2] * d[2], ey = F[3] * d[0] + F[4] * d[1] + F[5] * d[2], ez = F[6] * d[0] + F[7] * d[1] + F[8] * d[2];
            int px = (int)floorf(0.5f * (float)n * (1.0f + ex / ez)), py = (int)floorf(0.5f * (float)n * (1.0f + ey / ez));
            if (px < 0) px = 0;
            if (py < 0) py = 0;
            if (px > n - 1) px = n - 1;
            if (py > n - 1) py = n - 1;
            size_t si = (size_t)py * n + px, di = (size_t)j * W + i;
            memcpy(&out->c[di * 3], &faces[f].c[si * 3], 3 * sizeof(float));
            out->iz[di] = faces[f].iz[si];
        }
    }
}

void ns_render_pano(const NsRScene *s, const NsRCam *pose, float znear, uint32_t opts, NsRBuf *out, NsRStats *st) {
    int n = out->w / 4;
    if (n < 16) n = 16;
    NsRBuf faces[6];
    int ok = 1;
    for (int f = 0; f < 6; f++) { memset(&faces[f], 0, sizeof faces[f]); if (ns_buf_alloc(&faces[f], n, n)) ok = 0; }
    if (ok) {
        ns_render_cube(s, pose, n, znear, opts, faces, st);
        ns_cube_to_equirect(faces, pose, out);
        if (st) st->hole_frac = hole_fraction(out);
    }
    for (int f = 0; f < 6; f++) ns_buf_free(&faces[f]);
}

/* ---------------- salida ---------------- */
void ns_buf_to_rgb8(const NsRBuf *b, uint8_t *rgb, int mark_holes, uint32_t hole_rgb) {
    size_t n = (size_t)b->w * b->h;
    for (size_t i = 0; i < n; i++) {
        if (mark_holes && b->iz[i] == 0) { rgb[i * 3] = (uint8_t)(hole_rgb >> 16); rgb[i * 3 + 1] = (uint8_t)(hole_rgb >> 8); rgb[i * 3 + 2] = (uint8_t)hole_rgb; continue; }
        for (int k = 0; k < 3; k++) {
            float c = b->c[i * 3 + k];
            c = c < 0 ? 0 : c > 31 ? 31 : c;
            rgb[i * 3 + k] = (uint8_t)(c * (255.0f / 31.0f) + 0.5f);
        }
    }
}

void ns_cube_cross_rgb8(const NsRBuf faces[6], uint32_t hole_rgb, int mark_holes, uint8_t *rgb) {
    int n = faces[0].w, W = 4 * n;
    memset(rgb, 0, (size_t)W * 3 * n * 3);
    /* disposición: fila 0: arriba en columna 1; fila 1: izquierda, delante, derecha, atrás; fila 2: abajo en columna 1 */
    static const int pos[6][2] = {{1, 1}, {2, 1}, {3, 1}, {0, 1}, {1, 0}, {1, 2}};
    uint8_t *tmp = malloc((size_t)n * n * 3);
    if (!tmp) return;
    for (int f = 0; f < 6; f++) {
        ns_buf_to_rgb8(&faces[f], tmp, mark_holes, hole_rgb);
        for (int y = 0; y < n; y++) memcpy(&rgb[(((size_t)pos[f][1] * n + y) * W + (size_t)pos[f][0] * n) * 3], &tmp[(size_t)y * n * 3], (size_t)n * 3);
    }
    free(tmp);
}

static uint32_t crc_tab[256]; static int crc_init;
static uint32_t crc32u(uint32_t c, const uint8_t *d, size_t n) {
    if (!crc_init) { for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1; crc_tab[i] = v; } crc_init = 1; }
    c = ~c;
    while (n--) c = crc_tab[(c ^ *d++) & 255] ^ (c >> 8);
    return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE *f, const char *tag, const uint8_t *d, uint32_t n) {
    uint8_t h[8]; be32(h, n); memcpy(h + 4, tag, 4);
    fwrite(h, 1, 8, f); if (n) fwrite(d, 1, n, f);
    uint32_t c = crc32u(0, h + 4, 4); c = crc32u(c, d, n);
    uint8_t t[4]; be32(t, c); fwrite(t, 1, 4, f);
}
int ns_write_png(const char *path, int w, int h, const uint8_t *rgb) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t raw = ((size_t)w * 3 + 1) * h;
    uint8_t *z = malloc(raw + (raw / 65535 + 2) * 5 + 16);
    uint8_t *img = malloc(raw);
    if (!z || !img) { fclose(f); free(z); free(img); return -1; }
    for (int y = 0; y < h; y++) { img[(size_t)y * (w * 3 + 1)] = 0; memcpy(&img[(size_t)y * (w * 3 + 1) + 1], &rgb[(size_t)y * w * 3], (size_t)w * 3); }
    size_t zn = 0; z[zn++] = 0x78; z[zn++] = 0x01;
    for (size_t o = 0; o < raw || o == 0;) {
        size_t l = raw - o > 65535 ? 65535 : raw - o;
        z[zn++] = (o + l >= raw) ? 1 : 0; z[zn++] = l & 255; z[zn++] = l >> 8; z[zn++] = (~l) & 255; z[zn++] = ((~l) >> 8) & 255;
        memcpy(&z[zn], &img[o], l); zn += l; o += l;
        if (l == 0) break;
    }
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw; i++) { a = (a + img[i]) % 65521; b = (b + a) % 65521; }
    be32(&z[zn], (b << 16) | a); zn += 4;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13]; be32(ihdr, (uint32_t)w); be32(ihdr + 4, (uint32_t)h); ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13); chunk(f, "IDAT", z, (uint32_t)zn); chunk(f, "IEND", NULL, 0);
    free(z); free(img);
    return fclose(f) == 0 ? 0 : -1;
}
int ns_write_ppm(const char *path, int w, int h, const uint8_t *rgb) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 3, (size_t)w * h, f);
    return fclose(f) == 0 ? 0 : -1;
}
uint8_t *ns_read_ppm(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    int mx = 0;
    if (fscanf(f, "P6 %d %d %d", w, h, &mx) != 3 || mx != 255 || *w <= 0 || *h <= 0 || *w > 16384 || *h > 16384) { fclose(f); return NULL; }
    fgetc(f);
    uint8_t *p = malloc((size_t)*w * *h * 3);
    if (p && fread(p, 3, (size_t)*w * *h, f) != (size_t)*w * *h) { free(p); p = NULL; }
    fclose(f);
    return p;
}

void ns_diff_rgb8(const uint8_t *a, const uint8_t *b, int w, int h, int thr, uint8_t *diff, NsRDiff *out) {
    double sum = 0; long over = 0, n = (long)w * h;
    for (long i = 0; i < n; i++) {
        int m = 0;
        for (int k = 0; k < 3; k++) {
            int d = abs((int)a[i * 3 + k] - (int)b[i * 3 + k]);
            sum += d; if (d > m) m = d;
            if (diff) diff[i * 3 + k] = (uint8_t)(d * 4 > 255 ? 255 : d * 4);
        }
        over += m > thr;
    }
    if (out) { out->mae = n ? sum / (3.0 * (double)n) : 0; out->frac_over = n ? (double)over / (double)n : 0; out->thr = thr; out->npix = n; }
}
