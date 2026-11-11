/* Prueba de vr_mosaic.h: el mosaico 2D del juego pasa a un parche del mundo sobre lo que tapa. make -C vr/host test */
#include <stdio.h>
#include <stdlib.h>
#include "vr_mosaic.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fprintf(stderr, "FALLO %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

/* Cámara de prueba: ojo en (100, 200, -300), vista = mundo girado 90 grados en Y (columnas de M: X derecha = -Z mundo, Y abajo = +Y... la que sea, basta con que sea una rotación). */
static const float EYE[3] = {100, 200, -300};
static const float M[9] = {0, 0, 1,  0, 1, 0,  -1, 0, 0};
/* Proyección del «juego»: H = 320 con factor vertical 66/64, centro (160+320, 112), es decir con el búfer de dibujo en x = 320. */
static const float HX = 320.0f, HY = 320.0f * 66.0f / 64.0f, CX = 480.0f, CY = 112.0f;
static GpuVtx vert3d(float vx, float vy, float vz) {
    GpuVtx v; memset(&v, 0, sizeof v);
    v.vx = vx; v.vy = vy; v.vz = vz; v.vvalid = 1; v.fvalid = 1;
    v.x = (int16_t)lrintf(CX + HX * vx / vz); v.y = (int16_t)lrintf(CY + HY * vy / vz);
    v.fx = v.x; v.fy = v.y; v.fz = vz;
    return v;
}
static GpuOp quad3d(float x0, float y0, float x1, float y1, float z0, float z1) {   /* cuadrilátero de vista entre (x0,y0) y (x1,y1); la profundidad va de z0 (izquierda) a z1 (derecha) */
    GpuOp o; memset(&o, 0, sizeof o);
    o.type = GPU_OP_POLY; o.nv = 4;
    o.v[0] = vert3d(x0, y0, z0); o.v[1] = vert3d(x1, y0, z1); o.v[2] = vert3d(x0, y1, z0); o.v[3] = vert3d(x1, y1, z1);
    return o;
}
static GpuOp cell(int x, int y, int s, uint16_t texel) {
    GpuOp o; memset(&o, 0, sizeof o);
    o.type = GPU_OP_POLY; o.nv = 4; o.flags = GPU_F_TEXTURED; o.fbk = 1; o.fbt = texel;
    int xs[4] = {x, x + s, x, x + s}, ys[4] = {y, y, y + s, y + s};
    for (int k = 0; k < 4; k++) { o.v[k].x = (int16_t)xs[k]; o.v[k].y = (int16_t)ys[k]; o.v[k].r = o.v[k].g = o.v[k].b = 128; }
    return o;
}

int main(void) {
    enum { N = 64 };
    GpuOp ops[N]; uint8_t mk[N]; uint32_t n = 0;
    memset(mk, 0, sizeof mk);
    /* fondo lejano (pared a 6000), suelo, y el «personaje»: un cuadrilátero a 2000 de profundidad que cubre el centro del mosaico; delante, a un lado, otro objeto más cercano que NO lo cubre */
    ops[n++] = quad3d(-3000, -2000, 3000, 2000, 6000, 6000);
    ops[n++] = quad3d(-300, -400, 300, 400, 2000, 2000);
    ops[n++] = quad3d(900, -200, 1200, 200, 1000, 1000);
    /* mosaico 4x4 de celdas de 6 px centrado en el píxel donde se proyecta el punto de vista (50, 100, 2000) */
    const int px = (int)lrintf(CX + HX * 50.0f / 2000.0f), py = (int)lrintf(CY + HY * 100.0f / 2000.0f);
    const uint32_t first = n;
    for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) { mk[n] = 1; ops[n++] = cell(px - 12 + 6 * i, py - 12 + 6 * j, 6, (uint16_t)((10 + i) | ((12 + j) << 5) | (20 << 10))); }
    VrMosaic m;
    int r = vrmosaic_build(ops, n, mk, EYE, M, &m);
    CHECK(r == 16 && m.n == 16, "16 celdas (%d)", r);
    CHECK(fabsf(m.depth - 2000.0f) < 1.0f, "profundidad de lo tapado: la del personaje, no la pared ni el objeto de al lado (%.1f)", m.depth);
    /* el centro del parche cae sobre el rayo del píxel central, a 0,97 de la profundidad */
    float c[3] = {0, 0, 0};
    for (int k = 0; k < 16; k++) for (int v = 0; v < 4; v++) for (int a = 0; a < 3; a++) c[a] += m.p[k][v][a] / 64.0f;
    const float z = 2000.0f * VRMOSAIC_NEAR, vx = 50.0f * VRMOSAIC_NEAR, vy = 100.0f * VRMOSAIC_NEAR;
    const float want[3] = {EYE[0] + M[0] * vx + M[1] * vy + M[2] * z, EYE[1] + M[3] * vx + M[4] * vy + M[5] * z, EYE[2] + M[6] * vx + M[7] * vy + M[8] * z};
    CHECK(fabsf(c[0] - want[0]) < 6.0f && fabsf(c[1] - want[1]) < 6.0f && fabsf(c[2] - want[2]) < 6.0f, "centro del parche en el mundo: (%.1f %.1f %.1f) frente a (%.1f %.1f %.1f)", c[0], c[1], c[2], want[0], want[1], want[2]);
    /* tamaño de celda en el mundo: 6 px a esa profundidad = 6 * z / H (36,4 uds en X; 35,3 en Y por el factor vertical) */
    float ex = 0, ey = 0;
    for (int a = 0; a < 3; a++) { float d = m.p[0][1][a] - m.p[0][0][a]; ex += d * d; d = m.p[0][2][a] - m.p[0][0][a]; ey += d * d; }
    CHECK(fabsf(sqrtf(ex) - 6.0f * z / HX) < 0.8f && fabsf(sqrtf(ey) - 6.0f * z / HY) < 0.8f, "celda de 6 px: %.2f x %.2f uds (esperado %.2f x %.2f)", sqrtf(ex), sqrtf(ey), 6.0f * z / HX, 6.0f * z / HY);
    /* color: el texel resuelto, celda a celda */
    CHECK(m.rgb[0][0] == ((10 << 3) | (10 >> 2)) && m.rgb[0][1] == ((12 << 3) | (12 >> 2)) && m.rgb[0][2] == ((20 << 3) | (20 >> 2)), "color de la celda 0: %u %u %u", m.rgb[0][0], m.rgb[0][1], m.rgb[0][2]);
    CHECK(m.rgb[5][0] == ((11 << 3) | (11 >> 2)) && m.rgb[5][1] == ((13 << 3) | (13 >> 2)), "color de la celda 5: %u %u", m.rgb[5][0], m.rgb[5][1]);
    /* color de vértice a media luz: la mitad */
    ops[first].v[0].r = ops[first].v[0].g = ops[first].v[0].b = 64;
    vrmosaic_build(ops, n, mk, EYE, M, &m);
    CHECK(m.rgb[0][0] == ((5 << 3) | (5 >> 2)), "modulación por el color del vértice: %u", m.rgb[0][0]);
    ops[first].v[0].r = ops[first].v[0].g = ops[first].v[0].b = 128;
    /* superficie inclinada bajo el mosaico: la profundidad sale interpolada en el píxel (1/z lineal) */
    ops[1] = quad3d(-300, -400, 300, 400, 1500, 3000);
    vrmosaic_build(ops, n, mk, EYE, M, &m);
    {   float u = (50.0f / m.depth * HX + 0.0f);   /* comprobación de coherencia: el punto a esa profundidad en el rayo del píxel debe estar sobre el plano inclinado z = 2250 + 2,5 x */
        float xv = u * m.depth / HX;
        CHECK(fabsf(m.depth - (2250.0f + 2.5f * xv)) < 40.0f && m.depth > 2000.0f && m.depth < 2700.0f, "superficie inclinada: profundidad %.1f en x de vista %.1f", m.depth, xv); }
    ops[1] = quad3d(-300, -400, 300, 400, 2000, 2000);
    /* sin nada 3D bajo el mosaico (solo geometría lejos del centro): sin parche */
    GpuOp keep = ops[1], keep0 = ops[0];
    ops[1] = quad3d(900, 600, 1200, 900, 2000, 2000); ops[0] = quad3d(-3000, 1500, -2000, 2000, 6000, 6000);
    CHECK(vrmosaic_build(ops, n, mk, EYE, M, &m) == 0 && m.n == 0, "sin superficie debajo: sin parche");
    ops[1] = keep; ops[0] = keep0;
    /* celdas sin texel resuelto (o transparente) no se emiten; sin celdas, nada */
    for (uint32_t i = first; i < n; i++) ops[i].fbk = 0;
    CHECK(vrmosaic_build(ops, n, mk, EYE, M, &m) == 0, "sin realimentación resuelta: sin parche");
    for (uint32_t i = first; i < n; i++) ops[i].fbk = 1;
    memset(mk, 0, sizeof mk);
    CHECK(vrmosaic_build(ops, n, mk, EYE, M, &m) == 0, "lista sin mosaico");
    CHECK(vrmosaic_build(NULL, 0, mk, EYE, M, &m) == 0 && vrmosaic_build(ops, n, NULL, EYE, M, &m) == 0, "entradas nulas");
    /* vértices saturados / detrás de la cámara no entran en la medida ni en la profundidad */
    for (uint32_t i = first; i < n; i++) mk[i] = 1;
    GpuOp bad = quad3d(-300, -400, 300, 400, 500, 500);
    for (int k = 0; k < 4; k++) bad.v[k].vflags = GPU_VF_SAT;
    ops[n++] = bad;
    vrmosaic_build(ops, n, mk, EYE, M, &m);
    CHECK(m.n == 16 && fabsf(m.depth - 2000.0f) < 1.0f, "un polígono con vértices saturados no cuenta (%.1f)", m.depth);
    printf("test_mosaic: %d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
