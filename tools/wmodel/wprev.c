#include "wprev.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

WImage wimg_new(int w, int h) { WImage i = {w, h, calloc((size_t)w * h, 3)}; return i; }
void wimg_free(WImage *im) { free(im->rgb); im->rgb = NULL; }
void wimg_blit(WImage *d, const WImage *s, int x0, int y0) {
    for (int y = 0; y < s->h; y++) {
        if (y + y0 < 0 || y + y0 >= d->h) continue;
        for (int x = 0; x < s->w; x++) {
            if (x + x0 < 0 || x + x0 >= d->w) continue;
            memcpy(d->rgb + ((size_t)(y + y0) * d->w + x + x0) * 3, s->rgb + ((size_t)y * s->w + x) * 3, 3);
        }
    }
}
int wimg_write_ppm(const WImage *im, const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", im->w, im->h);
    fwrite(im->rgb, 3, (size_t)im->w * im->h, f); fclose(f); return 0;
}

/* ---- PNG ---- */
static uint32_t crc_tab[256];
static uint32_t crc32_(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t k = i; for (int j = 0; j < 8; j++) k = (k & 1) ? 0xEDB88320u ^ (k >> 1) : k >> 1; crc_tab[i] = k; }
    c = ~c; while (n--) c = crc_tab[(c ^ *p++) & 255] ^ (c >> 8); return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE *f, const char *t, const uint8_t *d, uint32_t n) {
    uint8_t b[4]; be32(b, n); fwrite(b, 1, 4, f);
    fwrite(t, 1, 4, f); if (n) fwrite(d, 1, n, f);
    uint32_t c = crc32_(0, (const uint8_t *)t, 4); if (n) c = crc32_(c, d, n);
    be32(b, c); fwrite(b, 1, 4, f);
}
int wimg_write_png(const WImage *im, const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ih[13]; be32(ih, im->w); be32(ih + 4, im->h); ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0;
    chunk(f, "IHDR", ih, 13);
    size_t rowb = (size_t)im->w * 3 + 1, raw = rowb * im->h;
    uint8_t *r = malloc(raw);
    for (int y = 0; y < im->h; y++) { r[y * rowb] = 0; memcpy(r + y * rowb + 1, im->rgb + (size_t)y * im->w * 3, (size_t)im->w * 3); }
    size_t nblk = (raw + 65534) / 65535;
    size_t zn = 2 + raw + nblk * 5 + 4;
    uint8_t *z = malloc(zn), *q = z;
    *q++ = 0x78; *q++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw; i++) { a = (a + r[i]) % 65521; b = (b + a) % 65521; }
    for (size_t o = 0; o < raw; o += 65535) {
        size_t n = raw - o < 65535 ? raw - o : 65535;
        *q++ = (o + n >= raw); *q++ = n & 255; *q++ = n >> 8; *q++ = ~n & 255; *q++ = (~n >> 8) & 255;
        memcpy(q, r + o, n); q += n;
    }
    be32(q, (b << 16) | a); q += 4;
    chunk(f, "IDAT", z, (uint32_t)(q - z));
    chunk(f, "IEND", NULL, 0);
    free(r); free(z); fclose(f); return 0;
}

/* ---- cámara ---- */
int wprev_cam(const WeaponState *s, WPose ww, const char *view, float radius, WCam *c) {
    (void)s;
    WVec3 ctr = wpose_apply(ww, wv(0, 0.02f, -0.07f));
    WVec3 R = wquat_rotate(ww.q, wv(1, 0, 0)), U = wquat_rotate(ww.q, wv(0, 1, 0)), F = wquat_rotate(ww.q, wv(0, 0, -1));
    float fov = 28.0f, d = radius / tanf(fov * 0.5f * 3.14159265f / 180.0f);
    WVec3 dir; WVec3 up = U;
    if (!strcmp(view, "side")) dir = R;
    else if (!strcmp(view, "left")) dir = wv_scale(R, -1);
    else if (!strcmp(view, "top")) { dir = U; up = wv_scale(F, 1); }
    else if (!strcmp(view, "under")) { dir = wv_scale(U, -1); up = F; }
    else if (!strcmp(view, "front")) dir = F;
    else if (!strcmp(view, "rear")) dir = wv_scale(F, -1);
    else if (!strcmp(view, "three")) dir = wv_norm(wv_add(wv_add(wv_scale(R, 0.7f), wv_scale(F, 0.55f)), wv_scale(U, 0.45f)));
    else if (!strcmp(view, "three2")) dir = wv_norm(wv_add(wv_add(wv_scale(R, 0.7f), wv_scale(F, -0.55f)), wv_scale(U, 0.45f)));
    else return -1;
    c->eye = wv_add(ctr, wv_scale(dir, d)); c->target = ctr; c->up = up; c->fov_y_deg = fov;
    return 0;
}

/* ---- rasterizador ---- */
typedef struct { float x, y, z, r, g, b, u, v; } RV;
typedef struct { int w, h; float *zb; uint8_t *col; int tex; } Buf;   /* tex: 1 = la malla en curso lleva textura (r,g,b = luz) */
/* atlas global opcional (RGBA8, v = 0 arriba); ganancia para ver modelos muy oscuros */
static const uint8_t *g_atlas; static int g_aw, g_ah; static float g_gain = 1.0f;
static int g_ccw;   /* 1 = triangulos antihorarios hacia fuera (salida de wcut); 0 = horarios (salida cruda de wload / OBJ extraidos) */
static int g_two;   /* 1 = sin descarte de caras traseras (cortes en seccion) */
void wprev_set_twosided(int on) { g_two = on ? 1 : 0; }
void wprev_set_winding(int ccw_outward) { g_ccw = ccw_outward ? 1 : 0; }
void wprev_set_atlas(const uint8_t *rgba, int w, int h, float gain) { g_atlas = rgba; g_aw = w; g_ah = h; g_gain = gain > 0 ? gain : 1.0f; }
typedef struct { WVec3 pos, R, U, Fd; float f; int w, h; WVec3 L1, L2; } View;

static void raster_tri(Buf *B, const View *V, RV a, RV b, RV c, int cull) {
    /* proyección */
    float ia = 1 / a.z, ib = 1 / b.z, ic = 1 / c.z;
    float cx = V->w * 0.5f, cy = V->h * 0.5f;
    float ax = cx + V->f * a.x * ia, ay = cy - V->f * a.y * ia;
    float bx = cx + V->f * b.x * ib, by = cy - V->f * b.y * ib;
    float qx = cx + V->f * c.x * ic, qy = cy - V->f * c.y * ic;
    float area = (bx - ax) * (qy - ay) - (by - ay) * (qx - ax);   /* >0 = horario en pantalla (y abajo) = antihorario real */
    if (area == 0) return;
    if (cull && !g_two && (g_ccw ? area > 0 : area < 0)) return;
    int x0 = (int)floorf(fminf(ax, fminf(bx, qx))), x1 = (int)ceilf(fmaxf(ax, fmaxf(bx, qx)));
    int y0 = (int)floorf(fminf(ay, fminf(by, qy))), y1 = (int)ceilf(fmaxf(ay, fmaxf(by, qy)));
    if (x0 < 0) { x0 = 0; }
    if (y0 < 0) { y0 = 0; }
    if (x1 > B->w) { x1 = B->w; }
    if (y1 > B->h) { y1 = B->h; }
    float inv = 1 / area;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
        float px = x + 0.5f, py = y + 0.5f;
        float w0 = ((bx - px) * (qy - py) - (by - py) * (qx - px)) * inv;
        float w1 = ((qx - px) * (ay - py) - (qy - py) * (ax - px)) * inv;
        float w2 = 1 - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        float iw = w0 * ia + w1 * ib + w2 * ic;
        size_t k = (size_t)y * B->w + x;
        if (iw <= B->zb[k]) continue;
        float r = (w0 * a.r * ia + w1 * b.r * ib + w2 * c.r * ic) / iw;
        float g = (w0 * a.g * ia + w1 * b.g * ib + w2 * c.g * ic) / iw;
        float bl = (w0 * a.b * ia + w1 * b.b * ib + w2 * c.b * ic) / iw;
        if (B->tex) {   /* r,g,b = luz * 255: se multiplica por el texel (vecino mas proximo) */
            float u = (w0 * a.u * ia + w1 * b.u * ib + w2 * c.u * ic) / iw, vv = (w0 * a.v * ia + w1 * b.v * ib + w2 * c.v * ic) / iw;
            int tx = (int)(u * g_aw), ty = (int)(vv * g_ah);
            tx = tx < 0 ? 0 : (tx >= g_aw ? g_aw - 1 : tx); ty = ty < 0 ? 0 : (ty >= g_ah ? g_ah - 1 : ty);
            const uint8_t *tp = g_atlas + ((size_t)ty * g_aw + tx) * 4;
            if (tp[3] < 128) continue;   /* texel transparente: no escribe */
            float gl = r / 255.0f * g_gain;
            r = tp[0] * gl; g = tp[1] * gl; bl = tp[2] * gl;
        }
        B->zb[k] = iw;
        B->col[k * 3] = (uint8_t)fminf(255, r); B->col[k * 3 + 1] = (uint8_t)fminf(255, g); B->col[k * 3 + 2] = (uint8_t)fminf(255, bl);
    }
}

static void clip_draw(Buf *B, const View *V, RV t[3], int cull) {
    const float N = 0.02f;
    RV in[4], o[4]; int n = 3, m = 0;
    memcpy(in, t, sizeof(RV) * 3);
    for (int i = 0; i < n; i++) {
        RV p = in[i], q = in[(i + 1) % n];
        int pi = p.z >= N, qi = q.z >= N;
        if (pi) o[m++] = p;
        if (pi != qi) {
            float u = (N - p.z) / (q.z - p.z); RV r;
            r.x = p.x + (q.x - p.x) * u; r.y = p.y + (q.y - p.y) * u; r.z = N;
            r.r = p.r + (q.r - p.r) * u; r.g = p.g + (q.g - p.g) * u; r.b = p.b + (q.b - p.b) * u;
            r.u = p.u + (q.u - p.u) * u; r.v = p.v + (q.v - p.v) * u;
            if (m < 4) o[m++] = r;
        }
    }
    for (int i = 1; i + 1 < m; i++) raster_tri(B, V, o[0], o[i], o[i + 1], cull);
}

static void draw_mesh(Buf *B, const View *V, const WMesh *me, WPose P) {
    if (!me || me->nv == 0 || me->ni == 0) return;
    B->tex = (me->texture && g_atlas) ? 1 : 0;
    /* descarte rápido: pieza fuera de vista (centro lejano) */
    WVec3 c0 = wpose_apply(P, wv(me->v[0].pos[0], me->v[0].pos[1], me->v[0].pos[2]));
    if (wv_len(wv_sub(c0, V->pos)) > 20.0f) return;
    for (uint32_t t = 0; t + 2 < me->ni; t += 3) {
        RV r[3];
        for (int k = 0; k < 3; k++) {
            const WVertex *v = &me->v[me->idx[t + k]];
            WVec3 w = wpose_apply(P, wv(v->pos[0], v->pos[1], v->pos[2]));
            WVec3 n = wquat_rotate(P.q, wv(v->nrm[0], v->nrm[1], v->nrm[2]));
            WVec3 d = wv_sub(w, V->pos);
            r[k].x = wv_dot(d, V->R); r[k].y = wv_dot(d, V->U); r[k].z = wv_dot(d, V->Fd);
            float d1 = fmaxf(0, wv_dot(n, V->L1)), d2 = fmaxf(0, wv_dot(n, V->L2));
            float up = 0.5f + 0.5f * wv_dot(n, V->U);
            float l = 0.55f + 0.15f * up + 0.70f * d1 + 0.25f * d2;
            if (B->tex) { r[k].r = r[k].g = r[k].b = 255.0f * l; r[k].u = v->uv[0]; r[k].v = v->uv[1]; }
            else { r[k].r = v->rgba[0] * l; r[k].g = v->rgba[1] * l; r[k].b = v->rgba[2] * l; r[k].u = r[k].v = 0; }
        }
        if (r[0].z < 0.02f && r[1].z < 0.02f && r[2].z < 0.02f) continue;
        clip_draw(B, V, r, 1);
    }
}

void wprev_render(const WeaponState *s, WPose ww, const WCam *cam, WImage *img, int ss) {
    if (ss < 1) ss = 1;
    Buf B = {img->w * ss, img->h * ss, NULL, NULL, 0};
    B.zb = calloc((size_t)B.w * B.h, sizeof(float));
    B.col = malloc((size_t)B.w * B.h * 3);
    for (int y = 0; y < B.h; y++) {          /* fondo degradado */
        float t = (float)y / B.h;
        uint8_t r = (uint8_t)(86 - 36 * t), g = (uint8_t)(94 - 38 * t), b = (uint8_t)(108 - 40 * t);
        for (int x = 0; x < B.w; x++) { size_t k = ((size_t)y * B.w + x) * 3; B.col[k] = r; B.col[k + 1] = g; B.col[k + 2] = b; }
    }
    View V; V.pos = cam->eye;
    V.Fd = wv_norm(wv_sub(cam->target, cam->eye));
    V.R = wv_norm(wv_cross(V.Fd, cam->up)); V.U = wv_cross(V.R, V.Fd);
    V.w = B.w; V.h = B.h; V.f = (B.h * 0.5f) / tanf(cam->fov_y_deg * 0.5f * 3.14159265f / 180.0f);
    V.L1 = wv_norm(wv_add(wv_add(wv_scale(V.R, -0.45f), wv_scale(V.U, 0.75f)), wv_scale(V.Fd, -0.55f)));
    V.L2 = wv_norm(wv_add(wv_add(wv_scale(V.R, 0.7f), wv_scale(V.U, -0.1f)), wv_scale(V.Fd, -0.4f)));
    int nm, np; const WMesh *ms = weapon_meshes(s, &nm); (void)weapon_parts(s, &np);
    WPose *pp = malloc(sizeof(WPose) * (np + 1));
    np = wcore_part_poses(s, ww, pp, np);
    for (int i = 0; i < np; i++) {
        int mi = weapon_part_mesh(s, i);          /* índice directo o por nombre (mesh_names) */
        if (mi >= 0 && mi < nm) draw_mesh(&B, &V, &ms[mi], pp[i]);
    }
    for (int i = 0; i < W_MAX_HELD; i++)
        if (s->held[i].active && s->held[i].mesh >= 0 && s->held[i].mesh < nm)
            draw_mesh(&B, &V, &ms[s->held[i].mesh], s->held[i].world);
    free(pp);
    /* reducción por caja (ss x ss) */
    for (int y = 0; y < img->h; y++) for (int x = 0; x < img->w; x++) {
        int acc[3] = {0, 0, 0};
        for (int j = 0; j < ss; j++) for (int i = 0; i < ss; i++) {
            size_t k = ((size_t)(y * ss + j) * B.w + x * ss + i) * 3;
            acc[0] += B.col[k]; acc[1] += B.col[k + 1]; acc[2] += B.col[k + 2];
        }
        size_t o = ((size_t)y * img->w + x) * 3;
        for (int c = 0; c < 3; c++) img->rgb[o + c] = (uint8_t)(acc[c] / (ss * ss));
    }
    free(B.zb); free(B.col);
}
