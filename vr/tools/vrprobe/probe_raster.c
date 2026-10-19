#include "probe_raster.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void pr_eye_from_head(const float h[16], int side, float ipd, PrEye *e) {
    /* ejes de la cabeza en el mundo: X = h[0..2], Y = h[4..6], Z = h[8..10]; posición del ojo = pos + X * side * ipd/2 */
    for (int i = 0; i < 3; i++) {
        e->R[0 + i] = h[0 + i];       /* fila 0 = eje X en el mundo -> pc.x = X . (pw - p) */
        e->R[3 + i] = h[4 + i];
        e->R[6 + i] = h[8 + i];
        e->p[i] = h[12 + i] + h[0 + i] * (float)side * 0.5f * ipd;
    }
}

void pr_buf_init(PrBuf *b, int w, int h) { b->w = w; b->h = h; b->z = malloc((size_t)w * (size_t)h * sizeof(float)); pr_buf_clear(b); }
void pr_buf_free(PrBuf *b) { free(b->z); b->z = NULL; }
void pr_buf_clear(PrBuf *b) { for (int i = 0; i < b->w * b->h; i++) b->z[i] = 1e30f; }

static void to_cam(const PrEye *e, const float *v, float *o) {
    float d[3] = {v[0] - e->p[0], v[1] - e->p[1], v[2] - e->p[2]};
    for (int r = 0; r < 3; r++) o[r] = e->R[3 * r] * d[0] + e->R[3 * r + 1] * d[1] + e->R[3 * r + 2] * d[2];
}

int pr_project(const PrCam *c, const PrEye *e, const float p[3], float *px, float *py, float *depth) {
    float q[3];
    to_cam(e, p, q);
    float d = -q[2];
    if (d < c->near) return 0;
    *px = (q[0] / (d * c->tx) * 0.5f + 0.5f) * (float)c->w;
    *py = (0.5f - q[1] / (d * c->ty) * 0.5f) * (float)c->h;
    *depth = d;
    return 1;
}

typedef struct { float x, y, iz; } SV;

static int raster_sv(PrBuf *b, const SV *a, const SV *bb, const SV *cc) {
    float minx = fminf(a->x, fminf(bb->x, cc->x)), maxx = fmaxf(a->x, fmaxf(bb->x, cc->x));
    float miny = fminf(a->y, fminf(bb->y, cc->y)), maxy = fmaxf(a->y, fmaxf(bb->y, cc->y));
    if (maxx < 0 || maxy < 0 || minx > (float)b->w || miny > (float)b->h) return 0;
    float area = (bb->x - a->x) * (cc->y - a->y) - (bb->y - a->y) * (cc->x - a->x);
    int x0 = (int)floorf(minx - 0.5f), x1 = (int)ceilf(maxx - 0.5f), y0 = (int)floorf(miny - 0.5f), y1 = (int)ceilf(maxy - 0.5f);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > b->w - 1) x1 = b->w - 1;
    if (y1 > b->h - 1) y1 = b->h - 1;
    int n = 0;
    float zmin = fminf(a->iz, fminf(bb->iz, cc->iz));
    (void)zmin;
    if (fabsf(area) < 1e-9f || (maxx - minx < 1.0f && maxy - miny < 1.0f)) {
        /* más pequeño que un píxel: se marca el píxel del centro (si no, el ruido de triángulos diminutos domina las diferencias) */
        int px = (int)floorf(0.5f * (minx + maxx)), py = (int)floorf(0.5f * (miny + maxy));
        if (px < 0 || py < 0 || px >= b->w || py >= b->h) return 0;
        float z = 1.0f / fmaxf((a->iz + bb->iz + cc->iz) * (1.0f / 3.0f), 1e-9f);
        float *p = &b->z[py * b->w + px];
        if (z < *p) *p = z;
        return 1;
    }
    float ia = 1.0f / area;
    for (int y = y0; y <= y1; y++) {
        float fy = (float)y + 0.5f;
        for (int x = x0; x <= x1; x++) {
            float fx = (float)x + 0.5f;
            float w0 = ((bb->x - fx) * (cc->y - fy) - (bb->y - fy) * (cc->x - fx)) * ia;
            float w1 = ((cc->x - fx) * (a->y - fy) - (cc->y - fy) * (a->x - fx)) * ia;
            float w2 = 1.0f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float iz = w0 * a->iz + w1 * bb->iz + w2 * cc->iz;
            float z = 1.0f / fmaxf(iz, 1e-9f);
            float *p = &b->z[y * b->w + x];
            if (z < *p) *p = z;
            n++;
        }
    }
    return n ? n : 0;
}

int pr_tri(PrBuf *b, const PrCam *c, const PrEye *e, const float *v) {
    float q[4][3];
    for (int i = 0; i < 3; i++) to_cam(e, v + 3 * i, q[i]);
    /* descarte rápido: todo detrás o todo fuera del mismo lado */
    float d0 = -q[0][2], d1 = -q[1][2], d2 = -q[2][2];
    if (d0 < c->near && d1 < c->near && d2 < c->near) return 0;
    if (q[0][0] > c->tx * d0 && q[1][0] > c->tx * d1 && q[2][0] > c->tx * d2) return 0;
    if (q[0][0] < -c->tx * d0 && q[1][0] < -c->tx * d1 && q[2][0] < -c->tx * d2) return 0;
    if (q[0][1] > c->ty * d0 && q[1][1] > c->ty * d1 && q[2][1] > c->ty * d2) return 0;
    if (q[0][1] < -c->ty * d0 && q[1][1] < -c->ty * d1 && q[2][1] < -c->ty * d2) return 0;
    /* recorte contra el plano cercano (Sutherland-Hodgman) */
    float poly[4][3];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const float *A = q[i], *B = q[(i + 1) % 3];
        float da = -A[2] - c->near, db = -B[2] - c->near;
        if (da >= 0) { memcpy(poly[n++], A, sizeof poly[0]); }
        if ((da >= 0) != (db >= 0)) {
            float t = da / (da - db);
            poly[n][0] = A[0] + (B[0] - A[0]) * t; poly[n][1] = A[1] + (B[1] - A[1]) * t; poly[n][2] = A[2] + (B[2] - A[2]) * t;
            n++;
        }
    }
    if (n < 3) return 0;
    SV s[4];
    for (int i = 0; i < n; i++) {
        float d = -poly[i][2];
        if (d < c->near) d = c->near;
        s[i].x = (poly[i][0] / (d * c->tx) * 0.5f + 0.5f) * (float)c->w;
        s[i].y = (0.5f - poly[i][1] / (d * c->ty) * 0.5f) * (float)c->h;
        s[i].iz = 1.0f / d;
    }
    int r = raster_sv(b, &s[0], &s[1], &s[2]);
    if (n == 4) r += raster_sv(b, &s[0], &s[2], &s[3]);
    return r;
}

/* ------------------------------------------------------------------------------------------------ PNG */
static uint32_t crc_tab[256];
static void crc_init(void) {
    if (crc_tab[1]) return;
    for (uint32_t n = 0; n < 256; n++) { uint32_t c = n; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_tab[n] = c; }
}
static uint32_t crc_upd(uint32_t c, const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 255] ^ (c >> 8); return c; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t h[8];
    put32(h, n); memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    uint32_t c = crc_upd(0xFFFFFFFFu, (const uint8_t *)type, 4);
    if (n) { fwrite(data, 1, n, f); c = crc_upd(c, data, n); }
    uint8_t t[4];
    put32(t, c ^ 0xFFFFFFFFu);
    fwrite(t, 1, 4, f);
}
int pr_write_png(const char *path, int w, int h, const uint8_t *rgb) {
    crc_init();
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    put32(ihdr, (uint32_t)w); put32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    size_t raw = (size_t)h * (1 + (size_t)w * 3);
    uint8_t *buf = malloc(raw);
    if (!buf) { fclose(f); return -2; }
    for (int y = 0; y < h; y++) { buf[(size_t)y * (1 + (size_t)w * 3)] = 0; memcpy(buf + (size_t)y * (1 + (size_t)w * 3) + 1, rgb + (size_t)y * w * 3, (size_t)w * 3); }
    /* zlib: cabecera 78 01, bloques stored de hasta 65535 bytes, Adler-32 */
    size_t nblk = (raw + 65534) / 65535;
    size_t zl = 2 + raw + nblk * 5 + 4;
    uint8_t *z = malloc(zl);
    if (!z) { free(buf); fclose(f); return -2; }
    size_t o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw; i++) { a = (a + buf[i]) % 65521u; b = (b + a) % 65521u; }
    for (size_t pos = 0; pos < raw;) {
        size_t n = raw - pos > 65535 ? 65535 : raw - pos;
        z[o++] = pos + n >= raw ? 1 : 0;
        z[o++] = (uint8_t)(n & 255); z[o++] = (uint8_t)(n >> 8); z[o++] = (uint8_t)(~n & 255); z[o++] = (uint8_t)((~n >> 8) & 255);
        memcpy(z + o, buf + pos, n); o += n; pos += n;
    }
    put32(z + o, (b << 16) | a); o += 4;
    chunk(f, "IDAT", z, (uint32_t)o);
    chunk(f, "IEND", NULL, 0);
    free(z); free(buf);
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------------------------------------ dibujo mínimo */
void pr_rect(uint8_t *rgb, int w, int h, int x, int y, int rw, int rh, uint8_t r, uint8_t g, uint8_t b) {
    for (int j = y; j < y + rh; j++) {
        if (j < 0 || j >= h) continue;
        for (int i = x; i < x + rw; i++) { if (i < 0 || i >= w) continue; uint8_t *p = rgb + ((size_t)j * w + i) * 3; p[0] = r; p[1] = g; p[2] = b; }
    }
}
void pr_line(uint8_t *rgb, int w, int h, int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        if (x0 >= 0 && y0 >= 0 && x0 < w && y0 < h) { uint8_t *p = rgb + ((size_t)y0 * w + x0) * 3; p[0] = r; p[1] = g; p[2] = b; }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
/* Fuente 3x5: cada glifo son 5 filas de 3 bits (bit 2 = izquierda). */
static const struct { char c; uint8_t r[5]; } FONT[] = {
    {'0', {7, 5, 5, 5, 7}}, {'1', {2, 6, 2, 2, 7}}, {'2', {7, 1, 7, 4, 7}}, {'3', {7, 1, 7, 1, 7}}, {'4', {5, 5, 7, 1, 1}},
    {'5', {7, 4, 7, 1, 7}}, {'6', {7, 4, 7, 5, 7}}, {'7', {7, 1, 1, 1, 1}}, {'8', {7, 5, 7, 5, 7}}, {'9', {7, 5, 7, 1, 7}},
    {'.', {0, 0, 0, 0, 2}}, {'%', {5, 1, 2, 4, 5}}, {'-', {0, 0, 7, 0, 0}}, {':', {0, 2, 0, 2, 0}}, {'/', {1, 1, 2, 4, 4}}, {' ', {0, 0, 0, 0, 0}},
    {'A', {2, 5, 7, 5, 5}}, {'B', {6, 5, 6, 5, 6}}, {'C', {3, 4, 4, 4, 3}}, {'D', {6, 5, 5, 5, 6}}, {'E', {7, 4, 6, 4, 7}}, {'F', {7, 4, 6, 4, 4}},
    {'G', {3, 4, 5, 5, 3}}, {'H', {5, 5, 7, 5, 5}}, {'I', {7, 2, 2, 2, 7}}, {'L', {4, 4, 4, 4, 7}}, {'M', {5, 7, 7, 5, 5}}, {'N', {6, 5, 5, 5, 5}},
    {'O', {2, 5, 5, 5, 2}}, {'P', {6, 5, 6, 4, 4}}, {'R', {6, 5, 6, 5, 5}}, {'S', {3, 4, 2, 1, 6}}, {'T', {7, 2, 2, 2, 2}}, {'U', {5, 5, 5, 5, 7}},
    {'V', {5, 5, 5, 5, 2}}, {'X', {5, 5, 2, 5, 5}}, {'Y', {5, 5, 2, 2, 2}}, {'Z', {7, 1, 2, 4, 7}}, {'W', {5, 5, 7, 7, 5}}, {'K', {5, 5, 6, 5, 5}}, {'J', {1, 1, 1, 5, 2}}, {'Q', {2, 5, 5, 7, 3}},
};
void pr_text(uint8_t *rgb, int w, int h, int x, int y, int scale, const char *s, uint8_t r, uint8_t g, uint8_t b) {
    for (; *s; s++, x += 4 * scale) {
        char c = *s >= 'a' && *s <= 'z' ? (char)(*s - 32) : *s;
        for (unsigned k = 0; k < sizeof FONT / sizeof FONT[0]; k++) {
            if (FONT[k].c != c) continue;
            for (int row = 0; row < 5; row++) for (int col = 0; col < 3; col++)
                if (FONT[k].r[row] & (4 >> col)) pr_rect(rgb, w, h, x + col * scale, y + row * scale, scale, scale, r, g, b);
            break;
        }
    }
}
