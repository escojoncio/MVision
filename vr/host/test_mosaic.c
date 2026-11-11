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
    int r = vrmosaic_build(ops, n, mk, NULL, EYE, M, NULL, 0, &m);
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
    vrmosaic_build(ops, n, mk, NULL, EYE, M, NULL, 0, &m);
    CHECK(m.rgb[0][0] == ((5 << 3) | (5 >> 2)), "modulación por el color del vértice: %u", m.rgb[0][0]);
    ops[first].v[0].r = ops[first].v[0].g = ops[first].v[0].b = 128;
    /* superficie inclinada bajo el mosaico: la profundidad sale interpolada en el píxel (1/z lineal) */
    ops[1] = quad3d(-300, -400, 300, 400, 1500, 3000);
    vrmosaic_build(ops, n, mk, NULL, EYE, M, NULL, 0, &m);
    {   float u = (50.0f / m.depth * HX + 0.0f);   /* comprobación de coherencia: el punto a esa profundidad en el rayo del píxel debe estar sobre el plano inclinado z = 2250 + 2,5 x */
        float xv = u * m.depth / HX;
        CHECK(fabsf(m.depth - (2250.0f + 2.5f * xv)) < 40.0f && m.depth > 2000.0f && m.depth < 2700.0f, "superficie inclinada: profundidad %.1f en x de vista %.1f", m.depth, xv); }
    ops[1] = quad3d(-300, -400, 300, 400, 2000, 2000);
    /* sin nada 3D bajo el mosaico (solo geometría lejos del centro): sin parche */
    GpuOp keep = ops[1], keep0 = ops[0];
    ops[1] = quad3d(900, 600, 1200, 900, 2000, 2000); ops[0] = quad3d(-3000, 1500, -2000, 2000, 6000, 6000);
    CHECK(vrmosaic_build(ops, n, mk, NULL, EYE, M, NULL, 0, &m) == 0 && m.n == 0, "sin superficie debajo: sin parche");
    ops[1] = keep; ops[0] = keep0;
    /* celdas sin texel resuelto (o transparente) no se emiten; sin celdas, nada */
    for (uint32_t i = first; i < n; i++) ops[i].fbk = 0;
    CHECK(vrmosaic_build(ops, n, mk, NULL, EYE, M, NULL, 0, &m) == 0, "sin realimentación resuelta: sin parche");
    /* como en los volcados reales: la captura no resuelve el texel (la celda se pisa su propia textura); el color sale de la VRAM de cierre de la lista, en el centro de cada celda */
    {
        uint16_t *vram = calloc(1024 * 512, sizeof *vram);
        for (uint32_t i = first; i < n; i++) {
            ops[i].cx1 = 320; ops[i].cy1 = 0; ops[i].cx2 = 639; ops[i].cy2 = 255;   /* búfer de dibujo en x = 320 (CX = 480) */
            const int cxp = (ops[i].v[0].x + ops[i].v[3].x) / 2, cyp = (ops[i].v[0].y + ops[i].v[3].y) / 2, k = (int)(i - first);
            vram[cyp * 1024 + cxp] = (uint16_t)((1 + k) | ((31 - k) << 5) | (7 << 10) | 0x8000u);
        }
        CHECK(vrmosaic_build(ops, n, mk, vram, EYE, M, NULL, 0, &m) == 16 && fabsf(m.depth - 2000.0f) < 1.0f, "celdas sin resolver + VRAM de cierre: 16 celdas (%d)", m.n);
        CHECK(m.rgb[0][0] == ((1 << 3) | (1 >> 2)) && m.rgb[0][1] == ((31 << 3) | (31 >> 2)) && m.rgb[0][2] == ((7 << 3) | (7 >> 2)), "color de la celda 0 leído de la VRAM: %u %u %u", m.rgb[0][0], m.rgb[0][1], m.rgb[0][2]);
        CHECK(m.rgb[15][0] == ((16 << 3) | (16 >> 2)) && m.rgb[15][1] == ((16 << 3) | (16 >> 2)), "color de la celda 15 leído de la VRAM: %u %u", m.rgb[15][0], m.rgb[15][1]);
        ops[first].flags = 0;
        CHECK(vrmosaic_build(ops, n, mk, vram, EYE, M, NULL, 0, &m) == 15, "una primitiva sin textura del actor no es celda (%d)", m.n);
        ops[first].flags = GPU_F_TEXTURED;
        /* una celda resuelta por la captura como transparente sigue siendo celda (no estropea el parche) pero no se emite */
        const uint16_t keep_fbt = ops[first].fbt;
        ops[first].fbk = 1; ops[first].fbt = 0;
        CHECK(vrmosaic_is_cell(&ops[first], vram) && vrmosaic_cell_empty(&ops[first]) && vrmosaic_build(ops, n, mk, vram, EYE, M, NULL, 0, &m) == 15, "celda transparente: celda válida, sin emitir (%d)", m.n);
        ops[first].fbk = 0; ops[first].fbt = keep_fbt;
        free(vram);
    }
    for (uint32_t i = first; i < n; i++) ops[i].fbk = 1;
    memset(mk, 0, sizeof mk);
    CHECK(vrmosaic_build(ops, n, mk, NULL, EYE, M, NULL, 0, &m) == 0, "lista sin mosaico");
    CHECK(vrmosaic_build(NULL, 0, mk, NULL, EYE, M, NULL, 0, &m) == 0 && vrmosaic_build(ops, n, NULL, NULL, EYE, M, NULL, 0, &m) == 0, "entradas nulas");
    /* vértices saturados / detrás de la cámara no entran en la medida ni en la profundidad */
    for (uint32_t i = first; i < n; i++) mk[i] = 1;
    GpuOp bad = quad3d(-300, -400, 300, 400, 500, 500);
    for (int k = 0; k < 4; k++) bad.v[k].vflags = GPU_VF_SAT;
    ops[n++] = bad;
    vrmosaic_build(ops, n, mk, NULL, EYE, M, NULL, 0, &m);
    CHECK(m.n == 16 && fabsf(m.depth - 2000.0f) < 1.0f, "un polígono con vértices saturados no cuenta (%.1f)", m.depth);
    /* una columna tapa el centro (d0013): el personaje, a 5000, queda detrás de una pared a 1300 que cubre la mitad izquierda del mosaico. Sin objetivo conocido cada celda se pega a lo que la tapa
     * (pared a la izquierda, personaje a la derecha); con objetivo (punto dinámico en el rayo del centro, a 5000) NINGUNA celda va a la profundidad de la pared: todas sobre el personaje, y el test
     * de profundidad del nativo las oculta tras ella. Nunca un plano a media sala. */
    {
        GpuOp o2[N]; uint8_t k2[N]; uint32_t n2 = 0; memset(k2, 0, sizeof k2);
        o2[n2++] = quad3d(-3000, -2000, 3000, 2000, 6500, 6500);
        o2[n2++] = quad3d(-1500, -800, 1500, 800, 5000, 5000);   /* personaje */
        const int cx2 = (int)lrintf(CX + HX * 50.0f / 5000.0f), cy2 = (int)lrintf(CY + HY * 100.0f / 5000.0f);   /* el mosaico, 4x4 de 6 px, sobre el punto de vista (50, 100, 5000) */
        o2[n2++] = quad3d(-3000, -2000, 12.0f, 2000, 1300, 1300);   /* pared cercana: cubre x de vista < 12 a 1300, o sea hasta el píxel central: la mitad izquierda de la rejilla */
        for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) { k2[n2] = 1; o2[n2++] = cell(cx2 - 12 + 6 * i, cy2 - 12 + 6 * j, 6, (uint16_t)(10 | (12 << 5) | (20 << 10))); }
        VrMosaic m2;
        CHECK(vrmosaic_build(o2, n2, k2, NULL, EYE, M, NULL, 0, &m2) == 16, "d0013 sin objetivo: 16 celdas");
        int near_wall = 0, near_char = 0;
        for (int c = 0; c < 16; c++) { float zc = 0; for (int a = 0; a < 3; a++) { float d = m2.p[c][0][a] - EYE[a]; zc += d * d; } zc = sqrtf(zc);   /* distancia al ojo: aproxima la profundidad (rayos casi axiales) */
                                       near_wall += zc < 2000.0f; near_char += zc > 4000.0f; }
        CHECK(near_wall >= 6 && near_char >= 6, "sin objetivo: cada celda sobre lo que la tapa (pared %d, personaje %d)", near_wall, near_char);
        /* punto dinámico del personaje: en el rayo del centro, a 5000 de profundidad (mundo = ojo + M * vista) */
        float pt[1][3];
        { const float v[3] = {50.0f, 100.0f, 5000.0f};   /* vista (50, 100, 5000): se proyecta en el píxel central de la rejilla */
          for (int a = 0; a < 3; a++) pt[0][a] = EYE[a] + M[3 * a] * v[0] + M[3 * a + 1] * v[1] + M[3 * a + 2] * v[2]; }
        CHECK(vrmosaic_build(o2, n2, k2, NULL, EYE, M, (const float (*)[3])pt, 1, &m2) == 16, "d0013 con objetivo: 16 celdas");
        int wall = 0;
        for (int c = 0; c < 16; c++) { float zc = 0; for (int a = 0; a < 3; a++) { float d = m2.p[c][0][a] - EYE[a]; zc += d * d; } wall += sqrtf(zc) < 4000.0f; }
        CHECK(wall == 0 && fabsf(m2.depth - 5000.0f) < 5.0f, "con objetivo: ninguna celda sobre la pared (%d), profundidad %.1f", wall, m2.depth);
        /* objetivo lejos del rayo (otro personaje a un lado, a 2000 uds): no cuenta, se usa lo de más cerca bajo cada celda */
        float far_pt[1][3];
        { const float v[3] = {2500.0f, 100.0f, 5000.0f}; for (int a = 0; a < 3; a++) far_pt[0][a] = EYE[a] + M[3 * a] * v[0] + M[3 * a + 1] * v[1] + M[3 * a + 2] * v[2]; }
        vrmosaic_build(o2, n2, k2, NULL, EYE, M, (const float (*)[3])far_pt, 1, &m2);
        wall = 0;
        for (int c = 0; c < 16; c++) { float zc = 0; for (int a = 0; a < 3; a++) { float d = m2.p[c][0][a] - EYE[a]; zc += d * d; } wall += sqrtf(zc) < 2000.0f; }
        CHECK(m2.n == 16 && wall >= 6, "objetivo fuera del rayo: se ignora (%d celdas sobre la pared)", wall);
        /* un semitransparente (agua, cristal) por delante no tapa: las celdas van al personaje */
        o2[2].flags |= GPU_F_SEMI;
        vrmosaic_build(o2, n2, k2, NULL, EYE, M, NULL, 0, &m2);
        wall = 0;
        for (int c = 0; c < 16; c++) { float zc = 0; for (int a = 0; a < 3; a++) { float d = m2.p[c][0][a] - EYE[a]; zc += d * d; } wall += sqrtf(zc) < 4000.0f; }
        CHECK(m2.n == 16 && wall == 0, "un semitransparente no tapa (%d celdas delante)", wall);
        /* con objetivo pero sin ninguna superficie opaca bajo las celdas por detrás del tapante: van a la profundidad del objetivo */
        o2[2].flags &= ~GPU_F_SEMI; o2[1] = quad3d(900, 600, 1200, 900, 5000, 5000); o2[0] = quad3d(900, 600, 1200, 900, 6500, 6500);
        vrmosaic_build(o2, n2, k2, NULL, EYE, M, (const float (*)[3])pt, 1, &m2);
        CHECK(m2.n == 16 && fabsf(m2.depth - 5000.0f) < 5.0f, "solo hay lo que tapa: celdas a la profundidad del objetivo (%.1f)", m2.depth);
    }
    printf("test_mosaic: %d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
