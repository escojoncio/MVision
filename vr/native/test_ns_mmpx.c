/* Prueba del filtro de texturas MMPX del mundo 3D: réplica en C de ns_mmpx (NativeShaders.metal, que la sigue línea a línea) frente a mmpx_scale2x (vr/scene/mmpx.c).
 * Reglas de MMPX.glc (Copyright 2020 Morgan McGuire & Mara Gagiu, licencia MIT). El texel de VRAM se compara por su valor de 16 bits ya decodificado (página, CLUT 4/8 bits, 15 bits):
 * 0 = transparente (el descarte de ns_fs). Para pasarlo a mmpx_scale2x se codifica sin pérdida en RGBA8: R = (r5 << 3) | bit15, G = g5 << 3, B = b5 << 3, alfa 255 (alfa 0 y color 0 si el texel es 0);
 * así la «luminosidad» de mmpx.c ((r + g + b + 1) * (256 - alfa)) coincide con la de ns_mmpx_luma. Sin ventana de textura (como ns_texel); coordenadas de la página con envoltura a 256. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mmpx.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; if (fails < 20) { printf("FALLO %s:%d: %s ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint16_t vram[1024 * 512];

/* ---- réplica de ns_texel (NativeShaders.metal) ---- */
static uint32_t ns_texel(uint32_t tpage, uint32_t clut, int u, int v) {
    int tx = (int)(tpage & 15u) * 64, ty = (int)((tpage >> 4) & 1u) * 256;
    uint32_t tp = (tpage >> 7) & 3u;
    int clx = (int)(clut & 63u) * 16, cly = (int)((clut >> 6) & 511u);
    u &= 255; v &= 255;
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

/* ---- réplica de ns_mmpx (misma secuencia que el sombreador) ---- */
static uint32_t ns_mm_luma(uint32_t t) {
    return t == 0u ? 256u : (((t & 31u) + ((t >> 5) & 31u) + ((t >> 10) & 31u)) * 8u + (t >> 15) + 1u);
}
static int ns_mm_eq2(uint32_t b, uint32_t a0, uint32_t a1) { return b == a0 && b == a1; }
static int ns_mm_eq3(uint32_t b, uint32_t a0, uint32_t a1, uint32_t a2) { return b == a0 && b == a1 && b == a2; }
static int ns_mm_eq4(uint32_t b, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) { return b == a0 && b == a1 && b == a2 && b == a3; }
static int ns_mm_any3(uint32_t b, uint32_t a0, uint32_t a1, uint32_t a2) { return b == a0 || b == a1 || b == a2; }
static int ns_mm_none2(uint32_t b, uint32_t a0, uint32_t a1) { return b != a0 && b != a1; }
static int ns_mm_none4(uint32_t b, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) { return b != a0 && b != a1 && b != a2 && b != a3; }

#define NS_TAP(dx, dy) ns_texel(tpage, clut, bu + (dx), bv + (dy))
static uint32_t ns_mmpx(uint32_t tpage, uint32_t clut, float uvx, float uvy) {
    const float fux = uvx + 1.0e-4f, fuy = uvy + 1.0e-4f;
    const float bfx = floorf(fux), bfy = floorf(fuy);
    const int bu = (int)bfx, bv = (int)bfy;
    const float frx = fux - bfx, fry = fuy - bfy;
    const uint32_t A = NS_TAP(-1, -1), B = NS_TAP(0, -1), C = NS_TAP(1, -1);
    const uint32_t D = NS_TAP(-1, 0), E = NS_TAP(0, 0), F = NS_TAP(1, 0);
    const uint32_t G = NS_TAP(-1, 1), H = NS_TAP(0, 1), I = NS_TAP(1, 1);
    uint32_t J = E, K = E, L = E, M = E;
    if (!ns_mm_eq4(E, A, B, C, D) || !ns_mm_eq4(E, F, G, H, I)) {
        const uint32_t P = NS_TAP(0, -2), S = NS_TAP(0, 2), Q = NS_TAP(-2, 0), R = NS_TAP(2, 0);
        const uint32_t Bl = ns_mm_luma(B), Dl = ns_mm_luma(D), El = ns_mm_luma(E), Fl = ns_mm_luma(F), Hl = ns_mm_luma(H);
        /* pendiente 1:1 */
        if ((D == B && D != H && D != F) && (El >= Dl || E == A) && ns_mm_any3(E, A, C, G) && (El < Dl || A != D || E != P || E != Q)) J = D;
        if ((B == F && B != D && B != H) && (El >= Bl || E == C) && ns_mm_any3(E, A, C, I) && (El < Bl || C != B || E != P || E != R)) K = B;
        if ((H == D && H != F && H != B) && (El >= Hl || E == G) && ns_mm_any3(E, A, G, I) && (El < Hl || G != H || E != S || E != Q)) L = H;
        if ((F == H && F != B && F != D) && (El >= Fl || E == I) && ns_mm_any3(E, C, G, I) && (El < Fl || I != H || E != R || E != S)) M = F;
        /* intersecciones */
        if ((E != F && ns_mm_eq4(E, C, I, D, Q) && ns_mm_eq2(F, B, H)) && F != NS_TAP(3, 0)) { K = F; M = F; }
        if ((E != D && ns_mm_eq4(E, A, G, F, R) && ns_mm_eq2(D, B, H)) && D != NS_TAP(-3, 0)) { J = D; L = D; }
        if ((E != H && ns_mm_eq4(E, G, I, B, P) && ns_mm_eq2(H, D, F)) && H != NS_TAP(0, 3)) { L = H; M = H; }
        if ((E != B && ns_mm_eq4(E, A, C, H, S) && ns_mm_eq2(B, D, F)) && B != NS_TAP(0, -3)) { J = B; K = B; }
        if (Bl < El && ns_mm_eq4(E, G, H, I, S) && ns_mm_none4(E, A, D, C, F)) { J = B; K = B; }
        if (Hl < El && ns_mm_eq4(E, A, B, C, P) && ns_mm_none4(E, D, G, I, F)) { L = H; M = H; }
        if (Fl < El && ns_mm_eq4(E, A, D, G, Q) && ns_mm_none4(E, B, C, I, H)) { K = F; M = F; }
        if (Dl < El && ns_mm_eq4(E, C, F, I, R) && ns_mm_none4(E, B, A, G, H)) { J = D; L = D; }
        /* pendiente 2:1 */
        if (H != B) {
            if (H != A && H != E && H != C) {
                if (ns_mm_eq3(H, G, F, R) && ns_mm_none2(H, D, NS_TAP(2, -1))) L = M;
                if (ns_mm_eq3(H, I, D, Q) && ns_mm_none2(H, F, NS_TAP(-2, -1))) M = L;
            }
            if (B != I && B != G && B != E) {
                if (ns_mm_eq3(B, A, F, R) && ns_mm_none2(B, D, NS_TAP(2, 1))) J = K;
                if (ns_mm_eq3(B, C, D, Q) && ns_mm_none2(B, F, NS_TAP(-2, 1))) K = J;
            }
        }
        if (F != D) {
            if (D != I && D != E && D != C) {
                if (ns_mm_eq3(D, A, H, S) && ns_mm_none2(D, B, NS_TAP(1, 2))) J = L;
                if (ns_mm_eq3(D, G, B, P) && ns_mm_none2(D, H, NS_TAP(1, -2))) L = J;
            }
            if (F != E && F != A && F != G) {
                if (ns_mm_eq3(F, C, H, S) && ns_mm_none2(F, B, NS_TAP(-1, 2))) K = M;
                if (ns_mm_eq3(F, I, B, P) && ns_mm_none2(F, H, NS_TAP(-1, -2))) M = K;
            }
        }
    }
    return frx < 0.5f ? (fry < 0.5f ? J : L) : (fry < 0.5f ? K : M);
}
#undef NS_TAP

/* ---- prueba ---- */
static uint32_t rng = 12345u;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static void enc(uint32_t t, uint8_t *o) {
    if (!t) { memset(o, 0, 4); return; }
    o[0] = (uint8_t)(((t & 31u) << 3) | (t >> 15)); o[1] = (uint8_t)(((t >> 5) & 31u) << 3); o[2] = (uint8_t)(((t >> 10) & 31u) << 3); o[3] = 255;
}

/* Imagen de píxel art de 256x256 índices (0..nc-1): celdas de 8x8 con rayas, diagonales, pendientes 2:1, ruido y relleno liso */
static void make_art(uint8_t *img, int nc) {
    for (int cy = 0; cy < 32; cy++)
        for (int cx = 0; cx < 32; cx++) {
            int kind = (int)(rnd() % 8u);
            int c0 = (int)(rnd() % (uint32_t)nc), c1 = (int)(rnd() % (uint32_t)nc), c2 = (int)(rnd() % (uint32_t)nc);
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    int c = c0;
                    switch (kind) {
                    case 1: c = x == 3 ? c1 : c0; break;
                    case 2: c = y == 4 ? c1 : c0; break;
                    case 3: c = x > y ? c1 : c0; break;
                    case 4: c = (x + y) == 7 ? c1 : c0; break;
                    case 5: c = (x >> 1) > y ? c1 : (y > 5 ? c2 : c0); break;
                    case 6: c = (int)(rnd() % (uint32_t)nc); break;
                    case 7: c = ((x ^ y) & 4) ? c1 : (x == y ? c2 : c0); break;
                    default: break;
                    }
                    img[(cy * 8 + y) * 256 + cx * 8 + x] = (uint8_t)c;
                }
        }
}

static uint32_t rcolor(void) { uint32_t c = rnd() & 0x7FFFu; return c ? c | ((rnd() & 1u) << 15) : 0x0421u; }

/* Una modalidad: bpp 4/8/15. Prepara la VRAM, ejecuta ns_mmpx en las 4 mitades de cada texel de la página entera y lo compara con mmpx_scale2x sobre la página repetida 3x3 (la envoltura a 256) */
static void run(int bpp, uint32_t tpage, int ny) {
    memset(vram, 0, sizeof vram);
    uint32_t clutv = 0;
    const int nc = bpp == 4 ? 7 : (bpp == 8 ? 12 : 6);
    uint8_t *img = malloc(256 * 256);
    make_art(img, nc);
    int tx = (int)(tpage & 15u) * 64, ty = (int)((tpage >> 4) & 1u) * 256;
    if (bpp != 15) {
        /* CLUT con color 0 transparente, colores repetidos (dos índices distintos con el mismo color: la igualdad es por texel, no por índice) y bit15 variado */
        int clx = 512, cly = 500 + ny;
        clutv = (uint32_t)(clx / 16) | (uint32_t)cly << 6;
        int n = bpp == 4 ? 16 : 256;
        for (int i = 0; i < n; i++) vram[cly * 1024 + clx + i] = (uint16_t)rcolor();
        vram[cly * 1024 + clx + 0] = 0;
        vram[cly * 1024 + clx + 3] = vram[cly * 1024 + clx + 2];
        vram[cly * 1024 + clx + 5] = (uint16_t)(vram[cly * 1024 + clx + 4] ^ 0x8000u);   /* mismo color, otro bit15 */
        for (int v = 0; v < 256; v++)
            for (int u = 0; u < 256; u++) {
                int i = img[v * 256 + u];
                int y = (ty + v) & 511;
                if (bpp == 4) vram[y * 1024 + ((tx + (u >> 2)) & 1023)] |= (uint16_t)(i << ((u & 3) * 4));
                else vram[y * 1024 + ((tx + (u >> 1)) & 1023)] |= (uint16_t)(i << ((u & 1) * 8));
            }
    } else {
        uint16_t pal[16];
        for (int i = 0; i < nc; i++) pal[i] = (uint16_t)rcolor();
        pal[0] = 0; pal[2] = pal[1];
        for (int v = 0; v < 256; v++)
            for (int u = 0; u < 256; u++) vram[((ty + v) & 511) * 1024 + ((tx + u) & 1023)] = pal[img[v * 256 + u]];
    }
    /* página repetida 3x3 como RGBA8 -> mmpx_scale2x */
    const int W = 768;
    uint8_t *src = malloc((size_t)W * W * 4), *dst = malloc((size_t)W * 2 * W * 2 * 4);
    for (int y = 0; y < W; y++)
        for (int x = 0; x < W; x++) enc(ns_texel(tpage, clutv, x, y), src + ((size_t)y * W + x) * 4);
    mmpx_scale2x(src, W, W, dst);
    int changed = 0, transp = 0;
    for (int v = 0; v < 256; v++)
        for (int u = 0; u < 256; u++)
            for (int q = 0; q < 4; q++) {
                const float fx = (q & 1) ? 0.75f : 0.25f, fy = (q >> 1) ? 0.75f : 0.25f;
                uint32_t got = ns_mmpx(tpage, clutv, (float)u + fx, (float)v + fy);
                uint8_t want[4];
                memcpy(want, dst + ((size_t)(2 * (v + 256) + (q >> 1)) * (size_t)(2 * W) + (size_t)(2 * (u + 256) + (q & 1))) * 4, 4);
                uint8_t gotb[4];
                enc(got, gotb);
                CHECK(!memcmp(want, gotb, 4), "bpp=%d u=%d v=%d q=%d got=%04x", bpp, u, v, q, got);
                if (got != ns_texel(tpage, clutv, u, v)) changed++;
                if (!got) transp++;
            }
    CHECK(changed > 2000, "cobertura baja: solo %d subtexeles cambian (bpp=%d)", changed, bpp);
    CHECK(transp > 0, "ninguna transparencia (bpp=%d)", bpp);
    printf("bpp=%d: %d subtexeles comparados, %d distintos del texel origen, %d transparentes\n", bpp, 256 * 256 * 4, changed, transp);
    free(img); free(src); free(dst);
}

int main(void) {
    run(4, 0u | (0u << 4) | (0u << 7), 0);           /* 4 bits: página 0 (x 0..63), arriba */
    run(8, 2u | (0u << 4) | (1u << 7), 1);           /* 8 bits: página 2 (x 128..255) */
    run(15, 4u | (1u << 4) | (2u << 7), 2);          /* 15 bits: página 4, mitad inferior (y 256..511) */
    /* la fracción exacta 0,5 cae en el subtexel derecho/inferior; 0,4999 y 0,9999 no cambian de texel */
    CHECK(ns_mmpx(0u, (32u) | (500u << 6), 10.5f, 10.5f) == ns_mmpx(0u, (32u) | (500u << 6), 10.75f, 10.75f), "0,5 no es el cuadrante M");
    printf("%d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
