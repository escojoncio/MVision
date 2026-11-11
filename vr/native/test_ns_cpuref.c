/* Pruebas sintéticas de ns_cpuref (sin datos del juego). */
#include "ns_cpuref.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint16_t *vram;
static uint16_t rgb15(int r, int g, int b, int s) { return (uint16_t)(r | g << 5 | b << 10 | (s ? 0x8000 : 0)); }

static NsRVert V(float x, float y, float z, float u, float v, int r, int g, int b) {
    NsRVert o; memset(&o, 0, sizeof o); o.p[0] = x; o.p[1] = y; o.p[2] = z; o.u = u; o.v = v; o.r = (uint8_t)r; o.g = (uint8_t)g; o.b = (uint8_t)b; return o;
}
/* quad en el plano z = z (mirando a +Z) [x0,x1]x[y0,y1] con uv [u0,u1]x[v0,v1]; escribe 2 triángulos en t[0..1] */
static void quad(NsRTri *t, float x0, float y0, float x1, float y1, float z, float u0, float v0, float u1, float v1, int col, uint8_t flags, uint16_t tpage, uint16_t clut, int abr) {
    NsRVert a = V(x0, y0, z, u0, v0, col, col, col), b = V(x1, y0, z, u1, v0, col, col, col), c = V(x1, y1, z, u1, v1, col, col, col), d = V(x0, y1, z, u0, v1, col, col, col);
    memset(t, 0, 2 * sizeof *t);
    t[0].v[0] = a; t[0].v[1] = b; t[0].v[2] = c; t[1].v[0] = a; t[1].v[1] = c; t[1].v[2] = d;
    for (int i = 0; i < 2; i++) { t[i].flags = flags; t[i].tpage = tpage; t[i].clut = clut; t[i].abr = (uint8_t)abr; }
}
static void setcol(NsRTri *t, int n, int r, int g, int b) { for (int i = 0; i < n; i++) for (int k = 0; k < 3; k++) { t[i].v[k].r = (uint8_t)r; t[i].v[k].g = (uint8_t)g; t[i].v[k].b = (uint8_t)b; } }

static NsRCam cam0(int w, int h) {   /* en el origen mirando a +Z, FOV 90 */
    NsRCam c; float p[3] = {0, 0, 0}; ns_cam_yaw_pitch(&c, p, 0, 0, 90, w, h); c.znear = 0.5f; return c;
}
static void px(const NsRBuf *b, int x, int y, float *o) { memcpy(o, &b->c[((size_t)y * b->w + x) * 3], 3 * sizeof(float)); }
static NsRBuf newbuf(int w, int h) { NsRBuf b; ns_buf_alloc(&b, w, h); ns_buf_clear(&b, 0, 0, 0); return b; }
static void render(const NsRTri *t, size_t n, const NsRCam *c, NsRBuf *b, uint32_t opts) { NsRScene s = {t, n, vram}; ns_render_view(&s, c, opts, b, NULL); }

static void test_texel(void) {
    /* 4 bpp: página x=64 (tpage&15 = 1), y=0; CLUT en (0,480) */
    uint16_t tp4 = 1, cl4 = (uint16_t)(0 | (480 << 6));
    for (int i = 0; i < 16; i++) vram[480 * 1024 + i] = rgb15(i, 2 * i > 31 ? 31 : 2 * i, 5, 0);
    vram[0 * 1024 + 64] = 0x3210;   /* texels 0..3 de la fila 0: índices 0,1,2,3 */
    for (int u = 0; u < 4; u++) CHECK(ns_sample_texel(vram, tp4, cl4, 0, u, 0) == vram[480 * 1024 + u]);
    /* 8 bpp */
    uint16_t tp8 = (uint16_t)(2 | (1 << 7)), cl8 = (uint16_t)(0 | (481 << 6));
    for (int i = 0; i < 256; i++) vram[481 * 1024 + i] = rgb15(i & 31, (i >> 3) & 31, 1, 0);
    vram[3 * 1024 + 128] = 0x07C1;   /* fila 3, x=128: texels 0,1 = 0xC1, 0x07 */
    CHECK(ns_sample_texel(vram, tp8, cl8, 0, 0, 3) == vram[481 * 1024 + 0xC1]);
    CHECK(ns_sample_texel(vram, tp8, cl8, 0, 1, 3) == vram[481 * 1024 + 0x07]);
    /* 15 bpp, página y=256 */
    uint16_t tp15 = (uint16_t)(0 | (1 << 4) | (2 << 7));
    vram[(256 + 5) * 1024 + 7] = 0x1234;
    CHECK(ns_sample_texel(vram, tp15, 0, 0, 7, 5) == 0x1234);
    CHECK(ns_sample_texel(vram, tp15, 0, 0, 7 + 256, 5 + 256) == 0x1234);   /* envoltura a 256 */
    /* ventana de textura: máscara 24 (mx=3), ox=2 -> u' = (u & ~24) | 16 */
    vram[(256 + 0) * 1024 + 20] = 0x4321;
    uint32_t tw = 3u | (0u << 8) | (2u << 16);
    CHECK(ns_sample_texel(vram, tp15, 0, tw, 4, 0) == 0x4321);   /* 4 -> (4&~24)|16 = 20 */
    CHECK(ns_sample_texel(vram, tp15, 0, tw, 12, 0) == 0x4321);  /* 12 -> 20 */
}

static void test_textured_quad(void) {
    /* damero 4 bpp 16x16: índice 0 = transparente (pone 0 en CLUT[0]), índice 1 = rojo, 2 = verde */
    uint16_t tp = 1, cl = (uint16_t)(0 | (480 << 6));
    vram[480 * 1024 + 0] = 0; vram[480 * 1024 + 1] = rgb15(31, 0, 0, 0); vram[480 * 1024 + 2] = rgb15(0, 20, 0, 0);
    for (int y = 0; y < 16; y++) for (int xw = 0; xw < 4; xw++) {
        uint16_t w = 0;
        for (int n = 0; n < 4; n++) { int u = xw * 4 + n; int idx = (u < 8) ? (y < 8 ? 1 : 0) : (y < 8 ? 2 : 1); w |= (uint16_t)(idx << (4 * n)); }
        vram[y * 1024 + 64 + xw] = w;
    }
    NsRTri t[2]; NsRBuf b = newbuf(64, 64); NsRCam c = cam0(64, 64);
    /* quad de 64x64 px: a z=32 con FOV 90 -> x,y en [-32,32] llena la pantalla; uv 0..16 -> 4 px por texel */
    quad(t, -32, -32, 32, 32, 32, 0, 0, 16, 16, 128, NS_TF_TEXTURED, tp, cl, 0);
    render(t, 2, &c, &b, 0);
    float p[3];
    px(&b, 8, 8, p);   CHECK(p[0] == 31 && p[1] == 0 && p[2] == 0);       /* rojo (u<8, v<8) */
    px(&b, 40, 8, p);  CHECK(p[0] == 0 && p[1] == 20 && p[2] == 0);       /* verde */
    px(&b, 8, 40, p);  CHECK(p[0] == 0 && p[1] == 0 && p[2] == 0 && b.iz[40 * 64 + 8] == 0);   /* texel 0 transparente: sigue el fondo y hueco */
    px(&b, 40, 40, p); CHECK(p[0] == 31 && p[1] == 0);
    /* modulación: 64 = mitad (texel*64/128), 255 = casi el doble saturado, RAW ignora el color */
    ns_buf_clear(&b, 0, 0, 0); setcol(t, 2, 64, 64, 64); render(t, 2, &c, &b, 0); px(&b, 8, 8, p); CHECK(p[0] == 15);   /* floor(31*64/128) */
    ns_buf_clear(&b, 0, 0, 0); setcol(t, 2, 255, 255, 255); render(t, 2, &c, &b, 0); px(&b, 40, 8, p); CHECK(p[1] == 31);   /* 20*255/128 = 39 -> 31 */
    ns_buf_clear(&b, 0, 0, 0); setcol(t, 2, 10, 10, 10); t[0].flags = t[1].flags = NS_TF_TEXTURED | NS_TF_RAW; render(t, 2, &c, &b, 0); px(&b, 8, 8, p); CHECK(p[0] == 31);
    ns_buf_free(&b);
}

static void test_gouraud(void) {
    NsRTri t[2]; NsRBuf b = newbuf(64, 64); NsRCam c = cam0(64, 64);
    quad(t, -32, -32, 32, 32, 32, 0, 0, 0, 0, 128, 0, 0, 0, 0);
    t[0].v[0].r = t[0].v[0].g = t[0].v[0].b = 0;   /* a (arriba izq.) negro */
    t[0].v[1].r = t[0].v[1].g = t[0].v[1].b = 255; /* b (arriba der.) blanco */
    t[1].v[0] = t[0].v[0]; t[1].v[1] = t[0].v[2]; /* mantiene c */
    render(t, 1, &c, &b, NS_O_FLOAT);
    float l[3], r[3];
    px(&b, 14, 3, l); px(&b, 44, 3, r);
    CHECK(l[0] < r[0]);   /* crece hacia el vértice blanco */
    CHECK(fabsf(l[0] - l[1]) < 1e-3f);
    /* sin textura: el color 128 -> 16 de 31 */
    quad(t, -32, -32, 32, 32, 32, 0, 0, 0, 0, 128, 0, 0, 0, 0);
    ns_buf_clear(&b, 0, 0, 0); render(t, 2, &c, &b, 0); px(&b, 10, 10, l); CHECK(l[0] == 16);
    ns_buf_free(&b);
}

static void test_z_and_semi(void) {
    NsRBuf b = newbuf(64, 64); NsRCam c = cam0(64, 64);
    NsRTri t[4];
    /* z: cercano rojo (z=20) y lejano verde (z=40); se dibuja en ambos órdenes */
    for (int ord = 0; ord < 2; ord++) {
        quad(t, -10, -10, 10, 10, 20, 0, 0, 0, 0, 128, 0, 0, 0, 0); setcol(t, 2, 255, 0, 0);
        quad(t + 2, -30, -30, 30, 30, 40, 0, 0, 0, 0, 128, 0, 0, 0, 0); setcol(t + 2, 2, 0, 255, 0);
        NsRTri u[4]; if (ord) { memcpy(u, t + 2, 2 * sizeof *t); memcpy(u + 2, t, 2 * sizeof *t); } else memcpy(u, t, sizeof u);
        ns_buf_clear(&b, 0, 0, 0); render(u, 4, &c, &b, 0);
        float p[3]; px(&b, 32, 32, p); CHECK(p[0] == 31 && p[1] == 0);   /* centro: el cercano */
        px(&b, 10, 10, p); CHECK(p[0] == 0 && p[1] == 31);               /* fuera del cercano: el lejano */
    }
    /* cuatro modos sobre fondo gris 16, color semitransparente 80 -> 10 */
    int expect[4] = {13, 26, 6, 18};
    for (int m = 0; m < 4; m++) {
        quad(t, -32, -32, 32, 32, 40, 0, 0, 0, 0, 128, 0, 0, 0, 0);
        quad(t + 2, -32, -32, 32, 32, 20, 0, 0, 0, 0, 80, NS_TF_SEMI, 0, 0, m);
        ns_buf_clear(&b, 0, 0, 0); render(t, 4, &c, &b, 0);
        float p[3]; px(&b, 32, 32, p);
        CHECK(p[0] == (float)expect[m] && p[1] == p[0]);
    }
    /* orden de semitransparentes: dos láminas (modo 0) de colores distintos; el resultado no depende del orden de entrada */
    float res[2][3];
    for (int ord = 0; ord < 2; ord++) {
        NsRTri s[6];
        quad(s, -32, -32, 32, 32, 40, 0, 0, 0, 0, 128, 0, 0, 0, 0);
        quad(s + 2, -32, -32, 32, 32, 30, 0, 0, 0, 0, 255, NS_TF_SEMI, 0, 0, 0); setcol(s + 2, 2, 255, 0, 0);
        quad(s + 4, -32, -32, 32, 32, 20, 0, 0, 0, 0, 255, NS_TF_SEMI, 0, 0, 0); setcol(s + 4, 2, 0, 0, 255);
        NsRTri u[6]; memcpy(u, s, sizeof u);
        if (ord) { memcpy(u + 2, s + 4, 2 * sizeof *s); memcpy(u + 4, s + 2, 2 * sizeof *s); }
        ns_buf_clear(&b, 0, 0, 0); render(u, 6, &c, &b, 0); px(&b, 32, 32, res[ord]);
    }
    CHECK(res[0][0] == res[1][0] && res[0][1] == res[1][1] && res[0][2] == res[1][2]);
    /* fondo 16 -> roja(31) mod 0: (23, 8, 8); luego azul: (11, 4, 19) */
    CHECK(res[0][0] == 11 && res[0][1] == 4 && res[0][2] == 19);
    /* semitransparente detrás de lo opaco no se ve */
    NsRTri s[4];
    quad(s, -32, -32, 32, 32, 40, 0, 0, 0, 0, 255, NS_TF_SEMI, 0, 0, 1); setcol(s, 2, 255, 255, 255);
    quad(s + 2, -32, -32, 32, 32, 20, 0, 0, 0, 0, 128, 0, 0, 0, 0);
    ns_buf_clear(&b, 0, 0, 0); render(s, 4, &c, &b, 0); float p[3]; px(&b, 32, 32, p); CHECK(p[0] == 16);
    /* texturizada semitransparente: bit 15 = 0 opaco, = 1 mezcla (modo 1, aditivo) */
    uint16_t tp = (uint16_t)(2 | (2 << 7));   /* 15 bpp, página x=128 */
    vram[0 * 1024 + 128] = rgb15(10, 10, 10, 0); vram[0 * 1024 + 129] = rgb15(10, 10, 10, 1);
    NsRTri q[4];
    quad(q, -32, -32, 32, 32, 40, 0, 0, 0, 0, 128, 0, 0, 0, 0);
    quad(q + 2, -32, -32, 32, 32, 20, 0.5f, 0.5f, 0.5f, 0.5f, 128, NS_TF_TEXTURED | NS_TF_SEMI, tp, 0, 1);   /* texel (0,0): opaco */
    ns_buf_clear(&b, 0, 0, 0); render(q, 4, &c, &b, 0); px(&b, 10, 32, p); CHECK(p[0] == 10);   /* reemplaza al fondo */
    for (int i = 2; i < 4; i++) for (int k = 0; k < 3; k++) q[i].v[k].u = 1.5f;                  /* texel (1,0): semitransparente */
    ns_buf_clear(&b, 0, 0, 0); render(q, 4, &c, &b, 0); px(&b, 10, 32, p); CHECK(p[0] == 26);    /* aditivo: 16 + 10 */
    ns_buf_free(&b);
}

static void test_clip_and_cam(void) {
    NsRBuf b = newbuf(64, 64); NsRCam c = cam0(64, 64);
    /* lámina vertical que atraviesa el plano cercano: z de -50 a 50, x=[-10,10], en y=+8 (debajo del eje); no debe romper nada */
    NsRTri t[2]; memset(t, 0, sizeof t);
    t[0].v[0] = V(-10, 8, -50, 0, 0, 128, 128, 128); t[0].v[1] = V(10, 8, -50, 0, 0, 128, 128, 128); t[0].v[2] = V(10, 8, 50, 0, 0, 128, 128, 128);
    t[1].v[0] = V(-10, 8, -50, 0, 0, 128, 128, 128); t[1].v[1] = V(10, 8, 50, 0, 0, 128, 128, 128); t[1].v[2] = V(-10, 8, 50, 0, 0, 128, 128, 128);
    render(t, 2, &c, &b, 0);
    int n = 0; for (int i = 0; i < 64 * 64; i++) n += b.iz[i] > 0;
    CHECK(n > 200);
    float p[3]; px(&b, 32, 40, p); CHECK(p[0] == 16);
    px(&b, 32, 10, p); CHECK(b.iz[10 * 64 + 32] == 0);
    /* cámara del motor: A = I, t = 0, H = 200, centro (160,120) en 320x240; vértice (100,-50,200) -> (260,70) */
    float A[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, tt[3] = {0, 0, 0};
    NsRCam g; ns_cam_game(&g, A, tt, 200, 160, 120, 320, 240, 320, 240); g.znear = 1;
    NsRBuf b2 = newbuf(320, 240);
    NsRTri q[2]; quad(q, 100 - 20, -50 - 10, 100 + 20, -50 + 10, 200, 0, 0, 0, 0, 128, 0, 0, 0, 0);
    render(q, 2, &g, &b2, 0);
    int x0 = 9999, x1 = -1, y0 = 9999, y1 = -1;
    for (int y = 0; y < 240; y++) for (int x = 0; x < 320; x++) if (b2.iz[y * 320 + x] > 0) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
    CHECK(x0 == 240 && x1 == 279 && y0 == 60 && y1 == 79);   /* (260±20, 70±10) */
    /* yaw 90: el punto (10,0,0) cae delante */
    NsRCam y; float o[3] = {0, 0, 0}; ns_cam_yaw_pitch(&y, o, 90, 0, 90, 64, 64);
    CHECK(fabsf(y.A[6] * 10 - 10) < 1e-3f);   /* z_ojo del punto (10,0,0) = 10 */
    NsRCam u; ns_cam_yaw_pitch(&u, o, 0, 90, 90, 64, 64);   /* mirar arriba = -Y */
    CHECK(fabsf(u.A[7] * -10 - 10) < 1e-3f);                 /* z_ojo de (0,-10,0) = 10 */
    ns_buf_free(&b); ns_buf_free(&b2);
}

/* caja envolvente: 6 caras de colores distintos alrededor del origen */
static size_t make_box(NsRTri *t, float r, int skip_face) {
    static const int col[6][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}, {0, 255, 255}, {255, 0, 255}};   /* +x rojo, -x verde, +y azul, -y amarillo, +z cian, -z magenta */
    size_t n = 0;
    for (int f = 0; f < 6; f++) {
        if (f == skip_face) continue;
        int ax = f / 2; float sg = (f & 1) ? -r : r;
        int a1 = (ax + 1) % 3, a2 = (ax + 2) % 3;
        float c[4][2] = {{-r, -r}, {r, -r}, {r, r}, {-r, r}};
        NsRVert vv[4];
        for (int k = 0; k < 4; k++) { float p[3]; p[ax] = sg; p[a1] = c[k][0]; p[a2] = c[k][1]; vv[k] = V(p[0], p[1], p[2], 0, 0, col[f][0], col[f][1], col[f][2]); }
        memset(&t[n], 0, 2 * sizeof *t);
        t[n].v[0] = vv[0]; t[n].v[1] = vv[1]; t[n].v[2] = vv[2]; t[n + 1].v[0] = vv[0]; t[n + 1].v[1] = vv[2]; t[n + 1].v[2] = vv[3]; n += 2;
    }
    return n;
}
static void px8(const uint8_t *im, int w, int x, int y, int *o) { for (int k = 0; k < 3; k++) o[k] = im[((size_t)y * w + x) * 3 + k]; }

static void test_pano(void) {
    NsRTri t[12]; size_t n = make_box(t, 100, -1);
    NsRScene s = {t, n, vram};
    float o[3] = {0, 0, 0}; NsRCam pose; ns_cam_yaw_pitch(&pose, o, 0, 0, 90, 64, 64); pose.znear = 0.5f;
    int W = 256, H = 128; NsRBuf pano = newbuf(W, H); NsRStats st;
    ns_render_pano(&s, &pose, 0.5f, 0, &pano, &st);
    CHECK(st.hole_frac == 0);
    uint8_t *rgb = malloc((size_t)W * H * 3); ns_buf_to_rgb8(&pano, rgb, 1, 0xFF00FF);
    int c[3];
    px8(rgb, W, W / 2, H / 2, c);           CHECK(c[0] == 0 && c[1] == 255 && c[2] == 255);      /* delante (+Z): cian */
    px8(rgb, W, W * 3 / 4, H / 2, c);       CHECK(c[0] == 255 && c[1] == 0 && c[2] == 0);        /* lon +90 = +X (derecha): rojo */
    px8(rgb, W, W / 4, H / 2, c);           CHECK(c[0] == 0 && c[1] == 255 && c[2] == 0);        /* izquierda -X: verde */
    px8(rgb, W, 0, H / 2, c);               CHECK(c[0] == 255 && c[1] == 0 && c[2] == 255);      /* atrás (-Z): magenta */
    px8(rgb, W, W / 2, 2, c);               CHECK(c[0] == 255 && c[1] == 255 && c[2] == 0);      /* arriba = -Y: amarillo */
    px8(rgb, W, W / 2, H - 3, c);           CHECK(c[0] == 0 && c[1] == 0 && c[2] == 255);        /* abajo = +Y: azul */
    /* cubemap en cruz: tamaño y que sea ejecutable */
    NsRBuf faces[6]; for (int f = 0; f < 6; f++) { ns_buf_alloc(&faces[f], 32, 32); }
    ns_render_cube(&s, &pose, 32, 0.5f, 0, faces, &st);
    uint8_t *cr = malloc((size_t)128 * 96 * 3); ns_cube_cross_rgb8(faces, 0xFF00FF, 1, cr);
    px8(cr, 128, 32 + 16, 32 + 16, c); CHECK(c[2] == 255 && c[0] == 0);   /* cara delante en (1,1) */
    px8(cr, 128, 64 + 16, 32 + 16, c); CHECK(c[0] == 255 && c[1] == 0);   /* derecha */
    px8(cr, 128, 0, 0, c); CHECK(c[0] == 0 && c[1] == 0 && c[2] == 0);    /* esquina sin cara */
    for (int f = 0; f < 6; f++) ns_buf_free(&faces[f]);
    free(cr); free(rgb); ns_buf_free(&pano);
    /* sin la cara de atrás (-z, índice 5): huecos exactamente donde falta */
    n = make_box(t, 100, 5);
    pano = newbuf(W, H); ns_render_pano(&s, &pose, 0.5f, 0, &pano, &st);
    s.ntris = n;
    ns_buf_clear(&pano, 0, 0, 0); ns_render_pano(&s, &pose, 0.5f, 0, &pano, &st);
    CHECK(st.hole_frac > 0.05 && st.hole_frac < 0.25);
    CHECK(pano.iz[(H / 2) * W + 0] == 0);              /* atrás: hueco */
    CHECK(pano.iz[(H / 2) * W + W / 2] > 0);           /* delante: hay */
    ns_buf_free(&pano);
}

static void test_io(void) {
    uint8_t im[4 * 3 * 3]; for (int i = 0; i < 36; i++) im[i] = (uint8_t)(i * 7);
    CHECK(ns_write_ppm("/tmp/ns_t.ppm", 4, 3, im) == 0);
    int w, h; uint8_t *r = ns_read_ppm("/tmp/ns_t.ppm", &w, &h);
    CHECK(r && w == 4 && h == 3 && memcmp(r, im, 36) == 0); free(r);
    CHECK(ns_write_png("/tmp/ns_t.png", 4, 3, im) == 0);
    uint8_t b2[36]; memcpy(b2, im, 36); b2[0] += 10; b2[5] += 100;
    NsRDiff d; ns_diff_rgb8(im, b2, 4, 3, 20, NULL, &d);
    CHECK(d.npix == 12 && fabs(d.frac_over - 1.0 / 12) < 1e-9 && fabs(d.mae - 110.0 / 36) < 1e-9);
    remove("/tmp/ns_t.ppm");
}

static unsigned g_seed = 12345;
static float rnd(void) { g_seed = g_seed * 1664525u + 1013904223u; return (float)(g_seed >> 8) / 16777216.0f; }
#define RND() rnd()
static void test_perf(void) {
    size_t N = 100000;
    NsRTri *t = calloc(N, sizeof *t);
    uint16_t tp = 1, cl = (uint16_t)(480 << 6);
    for (int i = 0; i < 16; i++) vram[480 * 1024 + i] = rgb15(i * 2, 31 - i, i, i == 15);
    for (size_t i = 0; i < N; i++) {
        float cx = (RND() - 0.5f) * 1000, cy = (RND() - 0.5f) * 400, cz = (RND() - 0.5f) * 1000;
        for (int k = 0; k < 3; k++) {
            float dx = (RND() - 0.5f) * 30, dy = (RND() - 0.5f) * 30, dz = (RND() - 0.5f) * 30, u = RND() * 31, v = RND() * 31;
            t[i].v[k] = V(cx + dx, cy + dy, cz + dz, u, v, 100 + (int)(RND() * 60), 128, 128);
        }
        t[i].flags = NS_TF_TEXTURED | ((i % 10 == 0) ? NS_TF_SEMI : 0); t[i].tpage = tp; t[i].clut = cl; t[i].abr = (uint8_t)(i & 3);
    }
    NsRScene s = {t, N, vram};
    float o[3] = {0, 0, 0}; NsRCam pose; ns_cam_yaw_pitch(&pose, o, 0, 0, 90, 64, 64);
    NsRBuf pano = newbuf(2048, 1024); NsRStats st;
    ns_render_pano(&s, &pose, 1.0f, 0, &pano, &st);
    printf("rendimiento: panorama 2048x1024, %zu tris: %.0f ms (dibujados %zu, huecos %.1f%%)\n", N, st.ms, st.tris_drawn, st.hole_frac * 100);
    CHECK(st.ms < 5000);
    ns_buf_free(&pano); free(t);
}

/* Descarte cercano del motor con su cámara (NsRCam.zreject; réplica del de ns_vertex con U.misc.y): el triángulo con ALGÚN vértice a z <= zreject no se dibuja entero; 0 = sin descarte. */
static int covered(const NsRBuf *b) { int n = 0; for (int i = 0; i < b->w * b->h; i++) n += b->iz[i] > 0; return n; }
static void test_near_reject(void) {
    NsRBuf b = newbuf(64, 64); NsRCam c = cam0(64, 64);
    /* lámina inclinada: de z = 30 (abajo) a z = 300 (arriba), delante de la cámara */
    NsRTri t[2]; memset(t, 0, sizeof t);
    t[0].v[0] = V(-20, 20, 30, 0, 0, 128, 128, 128); t[0].v[1] = V(20, 20, 30, 0, 0, 128, 128, 128); t[0].v[2] = V(20, -200, 300, 0, 0, 128, 128, 128);
    t[1].v[0] = V(-20, 20, 30, 0, 0, 128, 128, 128); t[1].v[1] = V(20, -200, 300, 0, 0, 128, 128, 128); t[1].v[2] = V(-20, -200, 300, 0, 0, 128, 128, 128);
    render(t, 2, &c, &b, 0);
    const int full = covered(&b);
    CHECK(full > 100);                                    /* sin descarte se dibuja entera */
    c.zreject = 100; ns_buf_clear(&b, 0, 0, 0); render(t, 2, &c, &b, 0);
    CHECK(covered(&b) == 0);                              /* los dos triángulos tienen un vértice a z = 30 <= 100: ninguno se dibuja (ni la parte lejana) */
    c.zreject = 29.5f; ns_buf_clear(&b, 0, 0, 0); render(t, 2, &c, &b, 0);
    CHECK(covered(&b) == full);                           /* todos los vértices por encima del umbral: igual que sin descarte */
    c.zreject = 30; ns_buf_clear(&b, 0, 0, 0); render(t, 2, &c, &b, 0);
    CHECK(covered(&b) == 0);                              /* z == umbral también se descarta (<=) */
    /* un triángulo entero más allá del umbral no se toca aunque otro de la escena se descarte */
    NsRTri u[4]; memcpy(u, t, sizeof t); quad(u + 2, -10, -10, 10, 10, 150, 0, 0, 0, 0, 128, 0, 0, 0, 0);
    c.zreject = 100; ns_buf_clear(&b, 0, 0, 0); render(u, 4, &c, &b, 0);
    NsRBuf far = newbuf(64, 64); NsRCam c0 = cam0(64, 64); render(u + 2, 2, &c0, &far, 0);
    CHECK(covered(&b) == covered(&far) && covered(&far) > 0);
    ns_buf_free(&b); ns_buf_free(&far);
}

int main(void) {
    vram = calloc(1024 * 512, 2);
    test_texel(); test_textured_quad(); test_gouraud(); test_z_and_semi(); test_clip_and_cam(); test_near_reject(); test_pano(); test_io(); test_perf();
    free(vram);
    if (fails) { printf("%d fallos\n", fails); return 1; }
    printf("test_ns_cpuref: OK\n");
    return 0;
}
