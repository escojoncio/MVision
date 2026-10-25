/* Pruebas de vr_glove: RAM/VRAM sintéticas (quad, UV real, color de CLUT, alfa 0 en 0x0000, soldadura, hash) y, si existen volcados reales del juego
 * (/home/claude/mv2/Dumps/d0001_* /{ram,vram}.bin; o MV_DUMP=ruta), la mano real: 28 vértices / 22 caras, sin NaN, tamaño de mano, puño.
 * make -C vr/hands test / test-san */
#include "vr_glove.h"
#include <glob.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { printf("  FALLO l.%d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
#define NEAR(a, b, e) (fabsf((a) - (b)) < (e))

#define RAMSZ 0x200000
static uint8_t RAM[RAMSZ];
static uint16_t VRAM[1024 * 512];

static void w8(uint32_t a, uint8_t v) { RAM[a & 0x1FFFFF] = v; }
static void w16(uint32_t a, int v) { RAM[a & 0x1FFFFF] = (uint8_t)v; RAM[(a + 1) & 0x1FFFFF] = (uint8_t)(v >> 8); }
static void w32(uint32_t a, uint32_t v) { w16(a, (int)(v & 0xFFFF)); w16(a + 2, (int)(v >> 16)); }

/* ---------------------------------------------------------------- modelo sintético */
#define MODELS 0x80100000u       /* 6 modelos de 0x58 */
#define DATA   0x80110000u
#define TEXTAB 0x800B30E0u
static uint32_t g_data;          /* siguiente dirección libre de datos */
static uint32_t alloc(uint32_t n) { uint32_t a = g_data; g_data += (n + 7u) & ~7u; return a; }

typedef struct { int nv, nf; int16_t v[16][4]; uint8_t vi[8][4], ni[8][4]; uint8_t uv[8][8]; uint16_t tex[8]; int nn; int16_t n[8][4]; uint32_t flags; } SynModel;

static void put_model(uint32_t m, const SynModel *s) {
    memset(RAM + (m & 0x1FFFFF), 0, 0x58);
    w32(m, s->flags); w32(m + 4, (uint32_t)s->nf);
    if (!s->nf) return;
    uint32_t pv = alloc(8u * (uint32_t)s->nv), pvi = alloc(4u * (uint32_t)s->nf), pn = alloc(8u * (uint32_t)(s->nn ? s->nn : 1)), pni = alloc(4u * (uint32_t)s->nf);
    uint32_t puv = alloc(8u * (uint32_t)s->nf), ptx = alloc(2u * (uint32_t)s->nf);
    w32(m + 0x34, (uint32_t)s->nv); w32(m + 0x38, pv); w32(m + 0x3C, pvi); w32(m + 0x40, (uint32_t)s->nn); w32(m + 0x44, pn); w32(m + 0x48, pni); w32(m + 0x4C, puv); w32(m + 0x50, ptx);
    for (int i = 0; i < s->nv; i++) for (int k = 0; k < 4; k++) w16(pv + 8u * (uint32_t)i + 2u * (uint32_t)k, s->v[i][k]);
    for (int i = 0; i < s->nn; i++) for (int k = 0; k < 4; k++) w16(pn + 8u * (uint32_t)i + 2u * (uint32_t)k, s->n[i][k]);
    for (int f = 0; f < s->nf; f++) {
        for (int k = 0; k < 4; k++) { w8(pvi + 4u * (uint32_t)f + (uint32_t)k, s->vi[f][k]); w8(pni + 4u * (uint32_t)f + (uint32_t)k, s->ni[f][k]); }
        for (int k = 0; k < 8; k++) w8(puv + 8u * (uint32_t)f + (uint32_t)k, s->uv[f][k]);
        w16(ptx + 2u * (uint32_t)f, s->tex[f]);
    }
}
static void put_tex(int slot, int id, int tpage, int clut, int ox, int oy, int w, int h) {
    uint32_t e = TEXTAB + 12u * (uint32_t)(id % 512);      /* sondeo de tex_find: la entrada vive en id % 512 (sin colisiones en estas pruebas) */
    (void)slot;
    w16(e, id); w16(e + 4, tpage); w16(e + 6, clut); w8(e + 8, (uint8_t)ox); w8(e + 9, (uint8_t)oy); w8(e + 10, (uint8_t)w); w8(e + 11, (uint8_t)h);
}
static uint16_t clut_color(int i) { return i == 0 ? 0 : (uint16_t)((i & 31) | (((2 * i) & 31) << 5) | (((31 - i) & 31) << 10)); }
static void rgb5(uint16_t c, uint8_t o[3]) { int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31; o[0] = (uint8_t)((r << 3) | (r >> 2)); o[1] = (uint8_t)((g << 3) | (g >> 2)); o[2] = (uint8_t)((b << 3) | (b >> 2)); }

/* Textura 4 bits: página (tx = 64, ty = 0), CLUT en (48, 480); región ox=16, oy=8, w=7 (8 texels), h=3 (4 filas); texel (u,v) = índice (u - 16 + v - 8) % 16 */
#define TPAGE 0x0001
#define CLUTW ((480 << 6) | 3)
static void setup_vram(void) {
    memset(VRAM, 0, sizeof VRAM);
    for (int i = 0; i < 16; i++) VRAM[480 * 1024 + 48 + i] = clut_color(i);
    for (int v = 8; v < 12; v++)
        for (int u = 16; u < 24; u++) {
            int idx = ((u - 16) + (v - 8)) % 16;
            uint16_t *w = &VRAM[v * 1024 + 64 + (u >> 2)];
            *w = (uint16_t)((*w & ~(15 << ((u & 3) * 4))) | (idx << ((u & 3) * 4)));
        }
}
static void uv4(SynModel *s, int f, int u0, int v0, int u1, int v1, int u2, int v2, int u3, int v3) {
    int a[8] = {u0, v0, u1, v1, u2, v2, u3, v3};
    for (int k = 0; k < 8; k++) s->uv[f][k] = (uint8_t)a[k];
}

static uint32_t MD[2][3];
static void setup_empty(void) {
    memset(RAM, 0, sizeof RAM);
    g_data = DATA;
    SynModel e; memset(&e, 0, sizeof e);
    for (int s = 0; s < 2; s++) for (int k = 0; k < 3; k++) { MD[s][k] = MODELS + 0x58u * (uint32_t)(s * 3 + k); put_model(MD[s][k], &e); }
}
static uint8_t px_at(const VrGlove *g, float u, float v, int c) {
    int x = (int)floorf(u * (float)g->aw), y = (int)floorf(v * (float)g->ah);
    return g->atlas[((size_t)y * g->aw + (size_t)x) * 4 + (size_t)c];
}

static void test_quad(void) {
    setup_empty(); setup_vram();
    put_tex(5, 0x1234, TPAGE, CLUTW, 16, 8, 7, 3);
    SynModel q; memset(&q, 0, sizeof q);
    q.nv = 4; q.nf = 1; q.nn = 1; q.flags = 0x1;
    int16_t pos[4][3] = {{0, 0, 0}, {100, 0, 0}, {100, -50, 0}, {0, -50, 0}};
    for (int i = 0; i < 4; i++) { q.v[i][0] = pos[i][0]; q.v[i][1] = pos[i][1]; q.v[i][2] = pos[i][2]; q.v[i][3] = -1; }
    q.n[0][2] = 4096;
    for (int k = 0; k < 4; k++) q.vi[0][k] = (uint8_t)k;
    q.tex[0] = 0x1234;
    uv4(&q, 0, 0, 0, 255, 0, 255, 255, 0, 255);
    put_model(MD[1][0], &q);
    VrGlove *g = vrglove_create();
    CHECK(vrglove_build(g, RAM, VRAM, MD, 0) == 1 && g->valid && g->id != 0, "build sintético");
    const VrGlovePiece *P = &g->p[1][0];
    CHECK(P->nv == 4 && P->nf == 1 && P->nout == 4 && P->nidx_out == 6, "quad: nv=%d nf=%d nout=%u nidx=%u", P->nv, P->nf, P->nout, P->nidx_out);
    static const uint16_t want[6] = {0, 1, 3, 1, 2, 3};      /* (A,B,D), (B,C,D) */
    for (int i = 0; i < 6; i++) CHECK(P->idx[i] == want[i], "quad -> 2 triángulos: idx[%d]=%d", i, P->idx[i]);
    CHECK(g->p[0][0].nout == 0 && g->p[1][1].nout == 0, "piezas vacías");
    /* UV real u' = ((u * (w + 1)) >> 8) + ox  (w=7, h=3, ox=16, oy=8): 0 -> 16 / 8, 255 -> 23 / 11 */
    const int eu[4] = {16, 23, 23, 16}, ev[4] = {8, 8, 11, 11};
    for (int k = 0; k < 4; k++) {
        CHECK(P->out_u[k] == eu[k] && P->out_t[k] == ev[k], "UV real esquina %d: (%d,%d) esperado (%d,%d)", k, P->out_u[k], P->out_t[k], eu[k], ev[k]);
        /* el texel del atlas en esa UV es el del VRAM: índice (u-16 + v-8) % 16 -> color de la CLUT (alfa 0 si índice 0) */
        int idx = ((eu[k] - 16) + (ev[k] - 8)) % 16;
        float tu = P->out_uv[k][0], tv = P->out_uv[k][1];
        CHECK(tu > 0 && tu < 1 && tv > 0 && tv < 1, "UV del atlas en 0..1");
        if (idx == 0) CHECK(px_at(g, tu, tv, 3) == 0, "0x0000 -> alfa 0 (esquina %d)", k);
        else {
            uint8_t c[3]; rgb5(clut_color(idx), c);
            CHECK(px_at(g, tu, tv, 0) == c[0] && px_at(g, tu, tv, 1) == c[1] && px_at(g, tu, tv, 2) == c[2] && px_at(g, tu, tv, 3) == 255, "color de CLUT esquina %d idx %d: %d,%d,%d,%d", k, idx, px_at(g, tu, tv, 0), px_at(g, tu, tv, 1), px_at(g, tu, tv, 2), px_at(g, tu, tv, 3));
        }
    }
    /* texel (16,8) tiene índice 0 -> transparente; (17,8) idx 1 opaco */
    {
        float cx = (1.0f + 0.0f + 0.5f) / (float)g->aw, cy = (4.0f + 0.0f + 0.5f) / (float)g->ah;      /* región: ax=1, ay=4 (tras el bloque blanco 3x3) */
        CHECK(px_at(g, cx, cy, 3) == 0, "texel (ox,oy) con índice 0 transparente");
        CHECK(px_at(g, cx + 1.0f / (float)g->aw, cy, 3) == 255, "texel contiguo opaco");
    }
    /* la malla en el espacio: pieza con identidad y upm 1000 -> vértices en metros */
    VrGlovePose pz; memset(&pz, 0, sizeof pz);
    for (int k = 0; k < 3; k++) { pz.R[k][0] = pz.R[k][4] = pz.R[k][8] = 1; }
    pz.upm = 1000.0f; pz.org[1][1] = 5.0f;
    VrGloveOut *o = calloc(1, sizeof *o);
    CHECK(vrglove_pose(g, 1, &pz, o) == 0 && o->nv[0] == 4 && o->nv[1] == 0, "pose");
    CHECK(NEAR(o->v[0][1].pos[0], 0.1f, 1e-6f) && NEAR(o->v[0][2].pos[1], -0.05f, 1e-6f) && NEAR(o->v[0][1].nrm[2], 1.0f, 1e-6f), "vértices en metros y normal");
    CHECK(o->v[0][0].rgba[0] == 255 && o->v[0][0].rgba[3] == 255 && NEAR(o->v[0][0].uv[0], P->out_uv[0][0], 1e-7f), "color del vértice (255) y UV");
    /* rotación 90 grados en Y: x -> -z */
    pz.R[0][0] = 0; pz.R[0][2] = 1; pz.R[0][6] = -1; pz.R[0][8] = 0;
    CHECK(vrglove_pose(g, 1, &pz, o) == 0 && NEAR(o->v[0][1].pos[2], -0.1f, 1e-6f) && NEAR(o->v[0][1].nrm[0], 1.0f, 1e-6f), "pose girada");
    /* hash: igual -> 0; un color de CLUT usado cambia -> reconstruye; un texel usado también; un píxel ajeno no */
    uint64_t h0 = g->hash; uint32_t id0 = g->id;
    CHECK(vrglove_build(g, RAM, VRAM, MD, h0) == 0, "mismo hash -> 0");
    VRAM[300 * 1024 + 10] ^= 0x7FFF;
    VrGlove *g2 = vrglove_create();
    CHECK(vrglove_build(g2, RAM, VRAM, MD, h0) == 0 && g2->hash == h0, "un píxel ajeno no cambia el hash");
    VRAM[480 * 1024 + 48 + 3] ^= 0x0421;
    CHECK(vrglove_build(g2, RAM, VRAM, MD, h0) == 1 && g2->hash != h0 && g2->id != id0, "fila CLUT usada cambia -> reconstruye");
    { uint8_t c[3]; rgb5((uint16_t)(clut_color(3) ^ 0x0421), c); /* el texel (19,8) tiene idx 3 */
      float tu = ((float)g2->p[1][0].out_uv[0][0]), tv = g2->p[1][0].out_uv[0][1]; (void)tu; (void)tv;
      float x = (1.0f + 3.0f + 0.5f) / (float)g2->aw, y = (4.0f + 0.0f + 0.5f) / (float)g2->ah;
      CHECK(px_at(g2, x, y, 0) == c[0] && px_at(g2, x, y, 1) == c[1], "atlas nuevo con el color nuevo"); }
    uint64_t h1 = g2->hash;
    VRAM[480 * 1024 + 48 + 3] ^= 0x0421;
    VRAM[9 * 1024 + 64 + 1] ^= 0x1111;      /* texel usado (u 20..23, v 9) */
    CHECK(vrglove_build(g2, RAM, VRAM, MD, h1) == 1 && g2->hash != h1, "texel usado cambia -> reconstruye");
    /* errores */
    CHECK(vrglove_build(NULL, RAM, VRAM, MD, 0) == -1 && vrglove_build(g2, NULL, VRAM, MD, 0) == -1 && vrglove_build(g2, RAM, NULL, MD, 0) == -1, "argumentos");
    uint32_t bad[2][3]; memcpy(bad, MD, sizeof bad); bad[1][2] = 0x12345;
    CHECK(vrglove_build(g2, RAM, VRAM, bad, 0) == -2 && !g2->valid, "modelo ilegible");
    CHECK(vrglove_pose(g2, 1, &pz, o) == -1, "pose con malla no válida");
    VrGlovePose nanp = pz; nanp.org[0][0] = NAN;
    CHECK(vrglove_pose(g, 1, &nanp, o) == -1 && vrglove_pose(g, 2, &pz, o) == -1 && vrglove_pose(g, 1, &pz, NULL) == -1, "pose inválida");
    free(o); vrglove_destroy(g); vrglove_destroy(g2);
}

static void test_tri_and_modes(void) {
    setup_empty(); setup_vram();
    /* triángulo (C == D) y textura de 8 bits / 15 bits */
    VRAM[200 * 1024 + 7] = 0x03E0;                       /* CLUT 8 bits en (0, 200): entrada 7 = verde */
    VRAM[100 * 1024 + 128 + 0] = 0x0007;                 /* página (tx = 128, mode 1): texel (0, 0) = índice 7 */
    VRAM[256 + 50 * 0 + 0] = 0;
    put_tex(1, 0x0AAA, (2 << 0) | (1 << 7) | (1 << 4), (200 << 6) | 0, 0, 0, 1, 1);    /* tx = 128, ty = 256, 8 bits */
    VRAM[(256 + 0) * 1024 + 128] = 0x0007;
    VRAM[(256 + 0) * 1024 + 129] = 0x0007;
    put_tex(2, 0x0BBB, 0x0003 | (2 << 7), 0, 4, 2, 0, 0);   /* 15 bits, tx = 192: texel (4, 2) */
    VRAM[2 * 1024 + 192 + 4] = 0x7C1F;                    /* magenta */
    SynModel t; memset(&t, 0, sizeof t);
    t.nv = 3; t.nf = 2; t.nn = 1; t.flags = 0x1;
    for (int i = 0; i < 3; i++) { t.v[i][0] = (int16_t)(10 * i); t.v[i][1] = (int16_t)(5 * i); t.v[i][2] = 0; }
    t.n[0][1] = 4096;
    t.vi[0][0] = 0; t.vi[0][1] = 1; t.vi[0][2] = 2; t.vi[0][3] = 2;          /* triángulo */
    t.vi[1][0] = 0; t.vi[1][1] = 1; t.vi[1][2] = 9; t.vi[1][3] = 2;          /* índice fuera de rango: cara descartada */
    t.tex[0] = 0x0AAA; t.tex[1] = 0x0BBB;
    uv4(&t, 0, 0, 0, 255, 0, 0, 255, 0, 255);
    put_model(MD[0][2], &t);
    VrGlove *g = vrglove_create();
    CHECK(vrglove_build(g, RAM, VRAM, MD, 0) == 1, "build tri");
    const VrGlovePiece *P = &g->p[0][2];
    CHECK(P->nout == 3 && P->nidx_out == 3 && P->idx[0] == 0 && P->idx[1] == 1 && P->idx[2] == 2, "triángulo: nout=%u nidx=%u", P->nout, P->nidx_out);
    float tu = P->out_uv[0][0], tv = P->out_uv[0][1];
    CHECK(px_at(g, tu, tv, 1) == 255 && px_at(g, tu, tv, 0) == 0 && px_at(g, tu, tv, 3) == 255, "8 bits: CLUT 8 bits (verde)");
    vrglove_destroy(g);
    /* cara con 15 bits */
    setup_empty(); setup_vram();
    put_tex(2, 0x0BBB, 0x0003 | (2 << 7), 0, 4, 2, 0, 0);
    VRAM[2 * 1024 + 192 + 4] = 0x7C1F;
    memset(&t, 0, sizeof t);
    t.nv = 3; t.nf = 1; t.nn = 0; t.flags = 0x1;
    t.vi[0][0] = 0; t.vi[0][1] = 1; t.vi[0][2] = 2; t.vi[0][3] = 2; t.tex[0] = 0x0BBB;
    put_model(MD[0][2], &t);
    g = vrglove_create();
    CHECK(vrglove_build(g, RAM, VRAM, MD, 0) == 1, "build 15 bits");
    P = &g->p[0][2];
    tu = P->out_uv[0][0]; tv = P->out_uv[0][1];
    CHECK(px_at(g, tu, tv, 0) == 255 && px_at(g, tu, tv, 1) == 0 && px_at(g, tu, tv, 2) == 255, "15 bits directo (magenta): %d,%d,%d", px_at(g, tu, tv, 0), px_at(g, tu, tv, 1), px_at(g, tu, tv, 2));
    VrGlovePose pz; memset(&pz, 0, sizeof pz);
    for (int k = 0; k < 3; k++) pz.R[k][0] = pz.R[k][4] = pz.R[k][8] = 1;
    pz.upm = 1000.0f;
    VrGloveOut *o = calloc(1, sizeof *o);
    CHECK(vrglove_pose(g, 0, &pz, o) == 0 && o->nv[2] == 3 && NEAR(o->v[2][0].nrm[1], 1.0f, 1e-6f), "sin normales: arriba");
    free(o); vrglove_destroy(g);
}

/* Soldadura: un vértice marcado del antebrazo toma la posición ya transformada del vértice del padre (brazo) que indica su campo */
static void test_weld(void) {
    setup_empty(); setup_vram();
    SynModel a, f;
    memset(&a, 0, sizeof a); memset(&f, 0, sizeof f);
    a.nv = 8; a.nf = 1; a.nn = 0; a.flags = 0x1;
    for (int i = 0; i < 8; i++) { a.v[i][0] = (int16_t)(100 * i); a.v[i][1] = (int16_t)(-10 * i); a.v[i][2] = (int16_t)(3 * i); }
    a.vi[0][0] = 4; a.vi[0][1] = 5; a.vi[0][2] = 6; a.vi[0][3] = 7;        /* cara 0 del padre: esquina 2 (C) -> posición 3 del paquete -> índice vi[3] = 7 */
    put_model(MD[1][0], &a);
    f.nv = 4; f.nf = 1; f.nn = 0; f.flags = 0x10001;
    for (int i = 0; i < 4; i++) { f.v[i][0] = (int16_t)(7 * i); f.v[i][1] = 0; f.v[i][2] = 0; f.v[i][3] = -1; }
    f.v[0][3] = (int16_t)(8 + 52 * 0 + 12 * 2);                             /* cara 0, esquina 2 */
    f.vi[0][0] = 0x80 | 0; f.vi[0][1] = 1; f.vi[0][2] = 2; f.vi[0][3] = 3;
    put_model(MD[1][1], &f);
    VrGlove *g = vrglove_create();
    CHECK(vrglove_build(g, RAM, VRAM, MD, 0) == 1, "build weld");
    CHECK(g->p[1][1].weld[0] == 7 && g->p[1][1].weld[1] == 0xFF && g->p[1][0].weld[7] == 0xFF, "soldadura resuelta: weld[0]=%d", g->p[1][1].weld[0]);
    VrGlovePose pz; memset(&pz, 0, sizeof pz);
    for (int k = 0; k < 3; k++) { pz.R[k][0] = pz.R[k][4] = pz.R[k][8] = 1; }
    pz.upm = 1000.0f;
    pz.org[0][0] = 1.0f; pz.org[1][1] = 2.0f;                                 /* el antebrazo se aparta del brazo: la soldadura cierra la junta */
    pz.R[1][4] = 0; pz.R[1][5] = -1; pz.R[1][7] = 1; pz.R[1][8] = 0;          /* giro 90 grados en X */
    VrGloveOut *o = calloc(1, sizeof *o);
    CHECK(vrglove_pose(g, 1, &pz, o) == 0, "pose weld");
    const VrGlovePiece *PA = &g->p[1][0], *PF = &g->p[1][1];
    int ja = -1, jf = -1, jf1 = -1;
    for (uint32_t j = 0; j < PA->nout; j++) if (PA->out_v[j] == 7) ja = (int)j;
    for (uint32_t j = 0; j < PF->nout; j++) { if (PF->out_v[j] == 0) jf = (int)j; if (PF->out_v[j] == 1) jf1 = (int)j; }
    CHECK(ja >= 0 && jf >= 0 && jf1 >= 0, "vértices encontrados");
    for (int c = 0; c < 3; c++) CHECK(NEAR(o->v[0][ja].pos[c], o->v[1][jf].pos[c], 1e-6f), "junta cerrada eje %d: %g vs %g", c, o->v[0][ja].pos[c], o->v[1][jf].pos[c]);
    CHECK(NEAR(o->v[1][jf].pos[0], 1.0f + 0.7f, 1e-5f), "posición = vértice del brazo (%g)", o->v[1][jf].pos[0]);
    /* el vértice 1 (no soldado) sí va con la pose del antebrazo: (7,0,0) mm girado 90 grados en X + org (0,2,0) */
    CHECK(NEAR(o->v[1][jf1].pos[0], 0.007f, 1e-6f) && NEAR(o->v[1][jf1].pos[1], 2.0f, 1e-6f), "vértice libre del antebrazo con su pose");
    free(o); vrglove_destroy(g);
}

/* ---------------------------------------------------------------- volcados reales */
static int load(const char *path, void *dst, size_t n) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t r = fread(dst, 1, n, f);
    fclose(f);
    return r == n;
}
static void bbox(const VrGloveOut *o, int k, float lo[3], float hi[3]) {
    for (int c = 0; c < 3; c++) { lo[c] = 1e30f; hi[c] = -1e30f; }
    for (uint32_t j = 0; j < o->nv[k]; j++) for (int c = 0; c < 3; c++) { if (o->v[k][j].pos[c] < lo[c]) lo[c] = o->v[k][j].pos[c]; if (o->v[k][j].pos[c] > hi[c]) hi[c] = o->v[k][j].pos[c]; }
}
static int finite_out(const VrGloveOut *o) {
    for (int k = 0; k < 3; k++) for (uint32_t j = 0; j < o->nv[k]; j++) {
        const VrHandsVertex *v = &o->v[k][j];
        for (int c = 0; c < 3; c++) if (!isfinite(v->pos[c]) || !isfinite(v->nrm[c])) return 0;
        if (!isfinite(v->uv[0]) || !isfinite(v->uv[1]) || v->uv[0] < 0 || v->uv[0] > 1 || v->uv[1] < 0 || v->uv[1] > 1) return 0;
    }
    return 1;
}
/* ---- mano real: quiralidad, pulgar conectado, puño (el modelo del juego trae la mano derecha con el pulgar abierto ~39 grados y los dedos curvados: se construye como espejo de la izquierda) */
static int uf_find2(int *p, int i) { while (p[i] != i) { p[i] = p[p[i]]; i = p[i]; } return i; }
/* is_thumb[i] = vértice de una componente conexa por caras que no es la mayor (el pulgar es una caja suelta); is_main = la mayor */
static void split_groups(const VrGlovePiece *P, int is_thumb[], int is_main[]) {
    int uf[VRGLOVE_MAXV], cnt[VRGLOVE_MAXV] = {0};
    for (int i = 0; i < P->nv; i++) uf[i] = i;
    for (int f = 0; f < P->nf; f++) for (int k = 0; k < 4; k++) {
        int a = uf_find2(uf, P->vidx[4 * f + k] & 0x7F), b = uf_find2(uf, P->vidx[4 * f + ((k + 1) & 3)] & 0x7F);
        if (a != b) uf[b] = a;
    }
    for (int i = 0; i < P->nv; i++) cnt[uf_find2(uf, i)]++;
    int mr = 0;
    for (int i = 0; i < P->nv; i++) if (cnt[uf_find2(uf, i)] > cnt[mr]) mr = uf_find2(uf, i);
    for (int i = 0; i < P->nv; i++) { is_main[i] = uf_find2(uf, i) == mr; is_thumb[i] = !is_main[i]; }
}
/* posición de salida (m) del vértice i de la pieza k de o */
static const float *vpos(const VrGlovePiece *P, const VrGloveOut *o, int k, int i) {
    for (uint32_t j = 0; j < P->nout; j++) if (P->out_v[j] == i) return o->v[k][j].pos;
    return NULL;
}
static float seg_dist(const float p[3], const float a[3], const float b[3]) {
    float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, ap[3] = {p[0] - a[0], p[1] - a[1], p[2] - a[2]};
    float l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2], t = l2 > 0 ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / l2 : 0;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    float d[3] = {ap[0] - t * ab[0], ap[1] - t * ab[1], ap[2] - t * ab[2]};
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}
/* Distancia (mm del modelo) de los vértices de la raíz del pulgar (los del extremo de la muñeca) al anillo de la base de la palma (aristas entre los vértices no soldados de la mano principal con la y más
 * cercana a la muñeca). Devuelve el máximo de los vértices de la raíz. */
static float thumb_root_gap(const VrGlovePiece *P) {
    int th[VRGLOVE_MAXV], mn[VRGLOVE_MAXV], ring[VRGLOVE_MAXV] = {0};
    split_groups(P, th, mn);
    int ymax_m = -32768, ymax_t = -32768;
    for (int i = 0; i < P->nv; i++) {
        if (mn[i] && P->weld[i] == 0xFF && P->v[4 * i + 1] > ymax_m) ymax_m = P->v[4 * i + 1];
        if (th[i] && P->v[4 * i + 1] > ymax_t) ymax_t = P->v[4 * i + 1];
    }
    for (int i = 0; i < P->nv; i++) ring[i] = mn[i] && P->weld[i] == 0xFF && P->v[4 * i + 1] >= ymax_m - 3;
    float worst = 0;
    for (int i = 0; i < P->nv; i++) {
        if (!th[i] || P->v[4 * i + 1] < ymax_t - 3) continue;
        float p[3] = {P->v[4 * i], P->v[4 * i + 1], P->v[4 * i + 2]}, best = 1e30f;
        for (int f = 0; f < P->nf; f++) for (int k = 0; k < 4; k++) {
            int a = P->vidx[4 * f + k] & 0x7F, b = P->vidx[4 * f + ((k + 1) & 3)] & 0x7F;
            if (!ring[a] || !ring[b] || a == b) continue;
            float pa[3] = {P->v[4 * a], P->v[4 * a + 1], P->v[4 * a + 2]}, pb[3] = {P->v[4 * b], P->v[4 * b + 1], P->v[4 * b + 2]};
            float d = seg_dist(p, pa, pb);
            if (d < best) best = d;
        }
        if (best > worst) worst = best;
    }
    return worst;
}
static float signed_volume(const VrGlovePiece *P) {      /* de las caras tal como las dibuja el renderer (ordenadas): cambia de signo si el orden de los índices se invierte */
    float vol = 0;
    for (uint32_t t = 0; t + 2 < P->nidx_out; t += 3) {
        float A[3], B[3], C[3];
        int ia = P->out_v[P->idx[t]], ib = P->out_v[P->idx[t + 1]], ic = P->out_v[P->idx[t + 2]];
        for (int k = 0; k < 3; k++) { A[k] = P->v[4 * ia + k]; B[k] = P->v[4 * ib + k]; C[k] = P->v[4 * ic + k]; }
        vol += (A[0] * (B[1] * C[2] - B[2] * C[1]) - A[1] * (B[0] * C[2] - B[2] * C[0]) + A[2] * (B[0] * C[1] - B[1] * C[0])) / 6.0f;
    }
    return vol;
}
static void thumb_dir(const VrGlovePiece *P, const VrGloveOut *o, float d[3]) {
    int th[VRGLOVE_MAXV], mn[VRGLOVE_MAXV];
    split_groups(P, th, mn);
    int ymax = -32768, ymin = 32767;
    for (int i = 0; i < P->nv; i++) if (th[i]) { if (P->v[4 * i + 1] > ymax) ymax = P->v[4 * i + 1]; if (P->v[4 * i + 1] < ymin) ymin = P->v[4 * i + 1]; }
    float r[3] = {0, 0, 0}, t[3] = {0, 0, 0}; int nr = 0, nt = 0;
    for (int i = 0; i < P->nv; i++) {
        if (!th[i]) continue;
        const float *p = vpos(P, o, 2, i);
        if (!p) continue;
        if (P->v[4 * i + 1] >= ymax - 3) { for (int k = 0; k < 3; k++) r[k] += p[k]; nr++; }
        if (P->v[4 * i + 1] <= ymin + 6) { for (int k = 0; k < 3; k++) t[k] += p[k]; nt++; }
    }
    for (int k = 0; k < 3; k++) d[k] = (nr && nt) ? t[k] / (float)nt - r[k] / (float)nr : 0;
    float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (l > 0) for (int k = 0; k < 3; k++) d[k] /= l;
}
static void test_real_hands(const VrGlove *g) {
    const VrGlovePiece *L = &g->p[0][2], *R = &g->p[1][2];
    int thL[VRGLOVE_MAXV], mnL[VRGLOVE_MAXV], thR[VRGLOVE_MAXV], mnR[VRGLOVE_MAXV];
    split_groups(L, thL, mnL); split_groups(R, thR, mnR);
    /* (a) quiralidad en el marco local: el pulgar sale hacia lados opuestos de la palma (la derecha es el espejo X de la izquierda) */
    float off[2];
    for (int s = 0; s < 2; s++) {
        const VrGlovePiece *P = &g->p[s][2];
        const int *th = s ? thR : thL, *mn = s ? mnR : mnL;
        float tc = 0, pc = 0; int nt = 0, np = 0;
        for (int i = 0; i < P->nv; i++) { if (th[i]) { tc += P->v[4 * i]; nt++; } else if (mn[i] && P->weld[i] == 0xFF) { pc += P->v[4 * i]; np++; } }
        off[s] = tc / (float)nt - pc / (float)np;
    }
    printf("  pulgar - palma (x local): izq %.1f der %.1f mm\n", off[0], off[1]);
    CHECK(off[0] * off[1] < 0 && fabsf(fabsf(off[0]) - fabsf(off[1])) < 3.0f, "quiralidad: el pulgar debe salir hacia lados opuestos y a igual distancia: %.1f / %.1f", off[0], off[1]);
    CHECK(R->nv == L->nv && R->nf == L->nf && R->nn == L->nn, "derecha e izquierda con la misma topología");
    float dev = 0, ndev = 0;
    for (int i = 0; i < L->nv && i < R->nv; i++) {
        dev = fmaxf(dev, fabsf((float)R->v[4 * i] + (float)L->v[4 * i])); dev = fmaxf(dev, fabsf((float)R->v[4 * i + 1] - (float)L->v[4 * i + 1])); dev = fmaxf(dev, fabsf((float)R->v[4 * i + 2] - (float)L->v[4 * i + 2]));
    }
    for (int i = 0; i < L->nn && i < R->nn; i++) ndev = fmaxf(ndev, fabsf((float)R->n[4 * i] + (float)L->n[4 * i]));
    printf("  desviación derecha vs espejo de la izquierda: %.1f mm (normales %.0f)\n", dev, ndev);
    CHECK(dev <= 1.0f && ndev <= 1.0f, "la mano derecha es el espejo de la izquierda: desviación %.1f mm, normales %.1f", dev, ndev);
    float vl = signed_volume(L), vr = signed_volume(R);
    CHECK(vl != 0 && fabsf(vl - vr) <= 0.01f * fabsf(vl), "el orden de los índices se invierte con el espejo (winding): volumen con signo %.0f / %.0f", vl, vr);
    CHECK(L->nout == R->nout && L->nidx_out == R->nidx_out, "misma salida de vértices/índices");
    int su = 0, st = 0, su2 = 0, st2 = 0;
    for (uint32_t j = 0; j < L->nout && j < R->nout; j++) { su += L->out_u[j]; st += L->out_t[j]; su2 += R->out_u[j]; st2 += R->out_t[j]; }
    CHECK(su == su2 && st == st2, "mismas UV (atlas) en ambas manos");
    /* en el espacio del usuario con el marco de mano de vr_body (hand_map: izquierda pulgar +Z local -> +X, derecha -> -X; palma hacia -Y): mirando el dorso desde arriba con los dedos hacia delante, el
     * pulgar de la izquierda queda a la DERECHA de la palma y el de la derecha a la izquierda; el pulgar queda en el lado de la palma (por debajo) */
    {
        static const float MAP[2][9] = {{0, 0, 1, 1, 0, 0, 0, 1, 0}, {0, 0, -1, -1, 0, 0, 0, 1, 0}};
        VrGlovePose pz; memset(&pz, 0, sizeof pz);
        pz.upm = 1010.7f;
        VrGloveOut *o = calloc(1, sizeof *o);
        for (int s = 0; s < 2; s++) {
            for (int k = 0; k < 3; k++) memcpy(pz.R[k], MAP[s], sizeof MAP[s]);
            CHECK(vrglove_pose(g, s, &pz, o) == 0, "pose con el marco de mano");
            const VrGlovePiece *P = &g->p[s][2];
            const int *th = s ? thR : thL, *mn = s ? mnR : mnL;
            float tc[3] = {0, 0, 0}, pc[3] = {0, 0, 0}; int nt = 0, np = 0;
            for (int i = 0; i < P->nv; i++) {
                const float *p = vpos(P, o, 2, i);
                if (!p) continue;
                if (th[i]) { for (int k = 0; k < 3; k++) tc[k] += p[k]; nt++; } else if (mn[i] && P->weld[i] == 0xFF) { for (int k = 0; k < 3; k++) pc[k] += p[k]; np++; }
            }
            float dx = tc[0] / (float)nt - pc[0] / (float)np, dy = tc[1] / (float)nt - pc[1] / (float)np;
            printf("  mano %s en el marco de VR: pulgar-palma dx %.1f dy %.1f mm\n", s ? "derecha" : "izquierda", dx * 1000.0f, dy * 1000.0f);
            CHECK(s == 0 ? dx > 0.015f : dx < -0.015f, "lado %d: el pulgar debe quedar a la %s de la palma mirando el dorso (dx %.3f)", s, s == 0 ? "derecha" : "izquierda", dx);
            CHECK(dy < 0.0f, "lado %d: el pulgar queda del lado de la palma (dy %.3f)", s, dy);
        }
        free(o);
    }
    /* (b) pulgar conectado a la base de la palma: en la mano buena (izquierda) la raíz del pulgar queda a ~6 mm del anillo de la base de la palma */
    float gl = thumb_root_gap(L), gr = thumb_root_gap(R);
    printf("  raíz del pulgar a la base de la palma: izq %.1f der %.1f mm\n", gl, gr);
    CHECK(gl < 8.0f && gr < 8.0f && fabsf(gl - gr) < 1.5f, "pulgar conectado a la base de la palma: izq %.1f der %.1f mm (< 8)", gl, gr);
    /* (c) puño: longitud <= 70 % de la abierta y el pulgar gira como mucho 35 grados respecto a la abierta */
    {
        VrGlovePose pz; memset(&pz, 0, sizeof pz);
        for (int k = 0; k < 3; k++) { pz.R[k][0] = pz.R[k][4] = pz.R[k][8] = 1; }
        pz.upm = 1010.7f;
        pz.org[1][1] = -0.260f * 1000.0f / 1010.7f; pz.org[1][2] = -0.050f * 1000.0f / 1010.7f;     /* pose de reposo apilada (las soldaduras de la muñeca se cierran contra el antebrazo) */
        pz.org[2][1] = pz.org[1][1] - 0.249f * 1000.0f / 1010.7f; pz.org[2][2] = pz.org[1][2] + 0.047f * 1000.0f / 1010.7f;
        VrFistParams fp; vrfist_params_default(&fp);
        pz.fist = &fp; pz.fist_min = 0.03f;
        for (int s = 0; s < 2; s++) {
            VrGloveOut *o = calloc(1, sizeof *o), *f = calloc(1, sizeof *f);      /* la caché del puño es de un lado: una instancia por lado */
            const VrGlovePiece *P = &g->p[s][2];
            pz.grip = 0.0f; CHECK(vrglove_pose(g, s, &pz, o) == 0, "abierta");
            pz.grip = 1.0f; CHECK(vrglove_pose(g, s, &pz, f) == 0 && f->fist_valid && finite_out(f), "puño");
            float lo[3], hi[3], lof[3], hif[3];
            bbox(o, 2, lo, hi); bbox(f, 2, lof, hif);
            float ratio = (hif[1] - lof[1]) / (hi[1] - lo[1]);
            float d0[3], d1[3];
            thumb_dir(P, o, d0); thumb_dir(P, f, d1);
            float c = d0[0] * d1[0] + d0[1] * d1[1] + d0[2] * d1[2];
            float ang = acosf(c > 1 ? 1 : (c < -1 ? -1 : c)) * 180.0f / 3.14159265f;
            printf("  mano %s: puño/abierta %.2f de longitud, pulgar gira %.1f grados\n", s ? "derecha" : "izquierda", ratio, ang);
            CHECK(ratio <= 0.70f, "lado %d: el puño debe medir <= 70 %% de la mano abierta: %.2f", s, ratio);
            CHECK(ang <= 35.5f && ang > 5.0f, "lado %d: el pulgar gira como mucho 35 grados (y se mueve): %.1f", s, ang);
            free(o); free(f);
        }
    }
    /* (d) sin NaN y soldaduras cerradas, abierta y en puño, en ambos lados, y el anillo de la mano espejada coincide con el del antebrazo (sin costura abierta) */
    {
        VrGlovePose pz; memset(&pz, 0, sizeof pz);
        for (int k = 0; k < 3; k++) { pz.R[k][0] = pz.R[k][4] = pz.R[k][8] = 1; }
        pz.upm = 1010.7f;
        pz.org[1][1] = -0.260f * 1000.0f / 1010.7f; pz.org[1][2] = -0.050f * 1000.0f / 1010.7f;
        pz.org[2][1] = pz.org[1][1] - 0.249f * 1000.0f / 1010.7f; pz.org[2][2] = pz.org[1][2] + 0.047f * 1000.0f / 1010.7f;
        VrFistParams fp; vrfist_params_default(&fp);
        pz.fist = &fp; pz.fist_min = 0.03f;
        VrGloveOut *o = calloc(1, sizeof *o);
        for (int s = 0; s < 2; s++) for (int gi = 0; gi < 2; gi++) {
            pz.grip = (float)gi;
            CHECK(vrglove_pose(g, s, &pz, o) == 0 && finite_out(o), "lado %d grip %d finito", s, gi);
            const VrGlovePiece *PH = &g->p[s][2], *PF = &g->p[s][1];
            int closed = 0, total = 0;
            float ring_gap = 0;
            for (int i = 0; i < PH->nv; i++) {
                if (PH->weld[i] == 0xFF) continue;
                const float *ph = vpos(PH, o, 2, i), *pf = vpos(PF, o, 1, PH->weld[i]);
                if (!ph || !pf) continue;
                float d = 0; for (int c = 0; c < 3; c++) d = fmaxf(d, fabsf(ph[c] - pf[c]));
                total++; if (d < 1e-6f) closed++;
                /* hueco entre la posición propia del vértice de la mano (sin soldar) y el del antebrazo */
                const float *Rm = pz.R[2], *O = pz.org[2]; const int16_t *sv = PH->v;
                float lx = sv[4 * i] / pz.upm, ly = sv[4 * i + 1] / pz.upm, lz = sv[4 * i + 2] / pz.upm;
                float raw[3] = {O[0] + Rm[0] * lx + Rm[1] * ly + Rm[2] * lz, O[1] + Rm[3] * lx + Rm[4] * ly + Rm[5] * lz, O[2] + Rm[6] * lx + Rm[7] * ly + Rm[8] * lz};
                for (int c = 0; c < 3; c++) ring_gap = fmaxf(ring_gap, fabsf(raw[c] - pf[c]) * 1000.0f);
            }
            CHECK(total >= 4 && closed == total, "lado %d grip %d: soldaduras cerradas %d de %d", s, gi, closed, total);
            if (gi == 0) printf("  mano %s: anillo de la muñeca vs antebrazo, hueco máx %.1f mm\n", s ? "derecha" : "izquierda", ring_gap);
            if (gi == 0) CHECK(ring_gap < 8.0f, "lado %d: la mano no encaja con su antebrazo (hueco %.1f mm)", s, ring_gap);
        }
        free(o);
    }
}

static void test_real(void) {
    char ram_p[512], vram_p[512];
    const char *env = getenv("MV_DUMP");
    glob_t gl; memset(&gl, 0, sizeof gl);
    if (env) { snprintf(ram_p, sizeof ram_p, "%s/ram.bin", env); snprintf(vram_p, sizeof vram_p, "%s/vram.bin", env); }
    else if (glob("/home/claude/mv2/Dumps/d0001_*/ram.bin", 0, NULL, &gl) == 0 && gl.gl_pathc > 0) {
        snprintf(ram_p, sizeof ram_p, "%s", gl.gl_pathv[0]);
        snprintf(vram_p, sizeof vram_p, "%.*svram.bin", (int)(strlen(gl.gl_pathv[0]) - 7), gl.gl_pathv[0]);
    } else { printf("  (sin volcado real: se omite)\n"); globfree(&gl); return; }
    globfree(&gl);
    if (!load(ram_p, RAM, RAMSZ) || !load(vram_p, VRAM, sizeof VRAM)) { printf("  (volcado ilegible: se omite)\n"); return; }
    /* modelos del jugador: 0x800ACBB0 -> objeto -> conjunto de piezas (+0x48 + 0x5C * i, model* en +0x40), brazo/antebrazo/mano I = 7,8,9 y D = 2,3,4 */
    uint32_t ob = (uint32_t)RAM[0xACBB0] | ((uint32_t)RAM[0xACBB1] << 8) | ((uint32_t)RAM[0xACBB2] << 16) | ((uint32_t)RAM[0xACBB3] << 24);
    uint32_t ps = (uint32_t)RAM[ob & 0x1FFFFF] | ((uint32_t)RAM[(ob & 0x1FFFFF) + 1] << 8) | ((uint32_t)RAM[(ob & 0x1FFFFF) + 2] << 16) | ((uint32_t)RAM[(ob & 0x1FFFFF) + 3] << 24);
    static const int PI_[2][3] = {{7, 8, 9}, {2, 3, 4}};
    uint32_t md[2][3];
    for (int s = 0; s < 2; s++) for (int k = 0; k < 3; k++) {
        uint32_t a = (ps + 0x48 + 0x5C * (uint32_t)PI_[s][k] + 0x40) & 0x1FFFFF;
        md[s][k] = (uint32_t)RAM[a] | ((uint32_t)RAM[a + 1] << 8) | ((uint32_t)RAM[a + 2] << 16) | ((uint32_t)RAM[a + 3] << 24);
        CHECK(md[s][k] == 0x8013E818u + 0x58u * (uint32_t)PI_[s][k], "modelo lado %d pieza %d en %08x", s, k, md[s][k]);
    }
    VrGlove *g = vrglove_create();
    int rc = vrglove_build(g, RAM, VRAM, md, 0);
    CHECK(rc == 1, "build real rc=%d", rc);
    if (rc != 1) { vrglove_destroy(g); return; }
    static const int NV[3] = {23, 12, 28}, NF[3] = {22, 8, 22};
    int welded[2][3] = {{0}};
    for (int s = 0; s < 2; s++) for (int k = 0; k < 3; k++) {
        const VrGlovePiece *P = &g->p[s][k];
        CHECK(P->nv == NV[k] && P->nf == NF[k], "pieza lado %d/%d: %dv/%df esperado %dv/%df", s, k, P->nv, P->nf, NV[k], NF[k]);
        CHECK(P->nout > 0 && P->nout <= VRGLOVE_MAXOUT && P->nidx_out >= 3 * (uint32_t)P->nf && P->nidx_out % 3 == 0, "salida lado %d/%d: %u vértices %u índices", s, k, P->nout, P->nidx_out);
        for (uint32_t i = 0; i < P->nidx_out; i++) CHECK(P->idx[i] < P->nout, "índice fuera de rango");
        for (int i = 0; i < P->nv; i++) if (P->weld[i] != 0xFF) welded[s][k]++;
    }
    for (int s = 0; s < 2; s++) CHECK(welded[s][0] == 0 && welded[s][1] > 0 && welded[s][2] > 0, "soldaduras reales lado %d: antebrazo %d, mano %d", s, welded[s][1], welded[s][2]);
    CHECK(g->atlas && g->aw >= 3 && g->ah >= 3 && g->ah <= VRGLOVE_MAXATLAS_H, "atlas %ux%u", g->aw, g->ah);
    int opaque = 0, transp = 0;
    for (size_t i = 0; i < (size_t)g->aw * g->ah; i++) { if (g->atlas[4 * i + 3]) opaque++; else transp++; }
    CHECK(opaque > 20, "atlas con texels opacos (%d, transparentes %d)", opaque, transp);
    if (getenv("MV_GLOVE_PPM")) {
        FILE *f = fopen(getenv("MV_GLOVE_PPM"), "wb");
        if (f) { fprintf(f, "P6\n%u %u\n255\n", g->aw, g->ah); for (size_t i = 0; i < (size_t)g->aw * g->ah; i++) { uint8_t *p = g->atlas + 4 * i; uint8_t c[3] = {p[3] ? p[0] : 255, p[3] ? p[1] : 0, p[3] ? p[2] : 255}; fwrite(c, 1, 3, f); } fclose(f); }
    }
    /* pose de reposo: piezas apiladas como en el modelo (brazo en el hombro, codo a (0,-260,-50), muñeca a (0,-249,47) del codo; upm 1010,7) */
    VrGlovePose pz; memset(&pz, 0, sizeof pz);
    for (int k = 0; k < 3; k++) { pz.R[k][0] = pz.R[k][4] = pz.R[k][8] = 1; }
    pz.upm = 1010.7f;
    pz.org[1][1] = -0.260f / 1.0f * 1000.0f / 1010.7f; pz.org[1][2] = -0.050f * 1000.0f / 1010.7f;
    pz.org[2][1] = pz.org[1][1] - 0.249f * 1000.0f / 1010.7f; pz.org[2][2] = pz.org[1][2] + 0.047f * 1000.0f / 1010.7f;
    VrFistParams fp; vrfist_params_default(&fp);
    for (int s = 0; s < 2; s++) {
        VrGloveOut *o = calloc(1, sizeof *o);
        pz.fist = &fp; pz.fist_min = 0.03f;
        pz.grip = 0.0f;
        CHECK(vrglove_pose(g, s, &pz, o) == 0 && finite_out(o), "pose real abierta (lado %d)", s);
        float lo[3], hi[3];
        bbox(o, 2, lo, hi);
        float ext[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
        float mx = fmaxf(ext[0], fmaxf(ext[1], ext[2]));
        CHECK(mx > 0.08f && mx < 0.20f && ext[0] > 0.04f && ext[2] > 0.04f, "mano abierta lado %d: %.3f x %.3f x %.3f m", s, ext[0], ext[1], ext[2]);
        printf("  mano %s abierta: %.3f x %.3f x %.3f m\n", s ? "derecha" : "izquierda", ext[0], ext[1], ext[2]);
        float lo1[3], hi1[3];
        bbox(o, 1, lo1, hi1);
        CHECK(hi1[1] - lo1[1] > 0.20f && hi1[1] - lo1[1] < 0.30f, "antebrazo lado %d: largo %.3f m", s, hi1[1] - lo1[1]);
        /* puño: misma malla deformada, finita, más corta a lo largo de los dedos que abierta; grip por debajo del mínimo = abierta */
        VrGloveOut open = *o;
        pz.grip = 0.01f;
        CHECK(vrglove_pose(g, s, &pz, o) == 0 && memcmp(o->v[2], open.v[2], sizeof(VrHandsVertex) * open.nv[2]) == 0, "grip bajo el mínimo = abierta");
        pz.grip = 1.0f;
        CHECK(vrglove_pose(g, s, &pz, o) == 0 && finite_out(o), "puño finito (lado %d)", s);
        float lof[3], hif[3];
        bbox(o, 2, lof, hif);
        printf("  mano %s en puño: %.3f x %.3f x %.3f m\n", s ? "derecha" : "izquierda", hif[0] - lof[0], hif[1] - lof[1], hif[2] - lof[2]);
        CHECK(o->fist_valid, "puño reconocido (lado %d)", s);
        CHECK(hif[1] - lof[1] < 0.75f * ext[1], "el puño es más corto que la mano abierta: %.3f vs %.3f", hif[1] - lof[1], ext[1]);
        int moved = 0;
        for (uint32_t j = 0; j < o->nv[2]; j++) if (fabsf(o->v[2][j].pos[1] - open.v[2][j].pos[1]) > 0.01f) moved++;
        CHECK(moved > 10, "vértices movidos por el puño: %d", moved);
        /* la caché del puño: mismo agarre cuantizado -> mismo resultado; los demás tramos no cambian */
        VrGloveOut again = *o;
        CHECK(vrglove_pose(g, s, &pz, o) == 0 && memcmp(o->v[2], again.v[2], sizeof(VrHandsVertex) * again.nv[2]) == 0 && memcmp(o->v[0], open.v[0], sizeof(VrHandsVertex) * open.nv[0]) == 0, "caché del puño estable");
        free(o);
    }
    /* las uniones de la pose de reposo se cierran: los vértices soldados de la mano coinciden con los del antebrazo */
    {
        VrGloveOut *o = calloc(1, sizeof *o);
        pz.grip = 0.0f;
        CHECK(vrglove_pose(g, 1, &pz, o) == 0, "pose reposo");
        const VrGlovePiece *PH = &g->p[1][2], *PF = &g->p[1][1];
        int closed = 0, total = 0;
        for (int i = 0; i < PH->nv; i++) {
            if (PH->weld[i] == 0xFF) continue;
            for (uint32_t j = 0; j < PH->nout; j++) if (PH->out_v[j] == i) {
                for (uint32_t m = 0; m < PF->nout; m++) if (PF->out_v[m] == PH->weld[i]) {
                    float d = 0; for (int c = 0; c < 3; c++) d = fmaxf(d, fabsf(o->v[2][j].pos[c] - o->v[1][m].pos[c]));
                    total++; if (d < 1e-6f) closed++;
                    break;
                }
                break;
            }
        }
        CHECK(total > 0 && closed == total, "juntas de la mano cerradas: %d de %d", closed, total);
        free(o);
    }
    test_real_hands(g);
    /* hash estable entre dos lecturas de los mismos datos */
    uint64_t h0 = g->hash;
    VrGlove *g2 = vrglove_create();
    CHECK(vrglove_build(g2, RAM, VRAM, md, h0) == 0, "hash real estable");
    vrglove_destroy(g2); vrglove_destroy(g);
}

int main(void) {
    test_quad();
    test_tri_and_modes();
    test_weld();
    test_real();
    if (fails) { printf("FALLO vr_glove: %d de %d comprobaciones\n", fails, checks); return 1; }
    printf("OK vr_glove: %d comprobaciones\n", checks);
    return 0;
}
