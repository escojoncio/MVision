/* Genera RAM sintéticas (2 MiB) para probar ns_lightcheck sin datos reales.
 *   test_ns_lightcheck_gen ok.bin borde.bin
 * Direcciones de los globales: luz 0x80100000, ambiente 0x80100100, luces fijas 0x80100200.
 * ok.bin: un objeto dinámico y un objeto precalculado cuyos colores se calculan con ns_light (cadena completa);
 * borde.bin: objeto con punteros a los últimos bytes de la RAM (lecturas que se saldrían del búfer). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ns_light.h"

static uint8_t *R;
#define P(x) (0x80000000u | (uint32_t)(x))
static void w16(uint32_t a, int v) { uint16_t x = (uint16_t)v; memcpy(R + (a & 0x1FFFFF), &x, 2); }
static void w32(uint32_t a, uint32_t v) { memcpy(R + (a & 0x1FFFFF), &v, 4); }
static void w8(uint32_t a, int v) { R[a & 0x1FFFFF] = (uint8_t)v; }
static void wmat(uint32_t a, const int16_t m[3][3]) { for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) w16(a + (i * 3 + j) * 2, m[i][j]); }

static const int16_t ID[3][3] = {{4096, 0, 0}, {0, 4096, 0}, {0, 0, 4096}};
static const int16_t DIR[3][3] = {{0, -4096, 0}, {0, 0, 0}, {0, 0, 0}};
static const int16_t COL[3][3] = {{560, 4096, 4096}, {560, 4096, 4096}, {560, 4096, 4096}};

static void globals(void) {
    wmat(0x100000, DIR); wmat(0x100020, COL);
    for (int i = 0; i < 3; i++) w16(0x100100 + 2 * i, 51);
    w32(0x100200, 1); w32(0x100204, P(0x100300));                   /* una lista fija con 1 luz */
    w16(0x100308, 1000); w16(0x10030A, 2000); w8(0x10030C, 200); w8(0x10030D, 100); w8(0x10030E, 50);
}
/* Objeto genérico: cabecera + 1 pieza + modelo. */
static void object(uint32_t A, uint32_t flag, uint32_t light, uint32_t model) {
    wmat(A, ID);
    w32(A + 0x24, P(A + 0x200)); w32(A + 0x28, flag); w16(A + 0x2E, 1); w32(A + 0x34, light ? P(light) : 0);
    uint32_t o = A + 0x48;
    wmat(o, ID); w32(o + 0x40, P(A + 0x220));
    (void)model;
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    R = calloc(1, 0x200000 + 8);
    NsLightEnv env; int16_t amb = 51;
    for (int i = 0; i < 3; i++) { env.main_dir[i] = DIR[0][i]; env.main_col[i] = COL[i][0]; env.ambient[i] = amb; }
    NsPointLight pl = {{0, 0, 0}, 1000, 2000, {200, 100, 50}};
    NsLightList lst = {&pl, 1};

    globals();
    /* dinámico */
    {
        uint32_t A = 0x110000, LP = 0x110300, PK = 0x110400, M = A + 0x220;
        object(A, 0x09, LP, M);
        wmat(LP, DIR); wmat(LP + 32, COL);
        uint32_t o = A + 0x48;
        w16(o + 0x52, 1); w32(o + 0x54, P(PK));
        w32(M + 4, 1); w32(M + 0x40, 2); w32(M + 0x44, P(0x110500)); w32(M + 0x48, P(0x110540));
        int16_t nm[2][3] = {{0, -4096, 0}, {2896, -2896, 0}};
        for (int n = 0; n < 2; n++) for (int k = 0; k < 3; k++) w16(0x110500 + n * 8 + 2 * k, nm[n][k]);
        const int ni[4] = {0, 1, 1, 0};
        for (int k = 0; k < 4; k++) w8(0x110540 + k, ni[k]);
        w32(PK, 1);
        NsLight L; memcpy(L.cmat, COL, sizeof COL);
        ns_light_to_local(DIR, ID, L.lmat);
        for (int i = 0; i < 3; i++) L.bg[i] = amb * 16;
        static const int src[4] = {0, 1, 3, 2};
        for (int sl = 0; sl < 4; sl++) {
            uint8_t c[3]; ns_light_color(&L, nm[ni[src[sl]]], c);
            for (int a = 0; a < 3; a++) w8(PK + 4 + sl * 12 + a, c[a]);
        }
    }
    /* precalculado */
    {
        uint32_t B = 0x120000, M = B + 0x220, RG = 0x120600;
        object(B, 0x02, 0, M);
        uint32_t o = B + 0x48;
        w32(o + 0x44, P(RG));
        w32(M + 4, 1); w32(M + 0x34, 4); w32(M + 0x38, P(0x120400)); w32(M + 0x3C, P(0x120440));
        w32(M + 0x40, 1); w32(M + 0x44, P(0x120460)); w32(M + 0x48, P(0x120480));
        int16_t vv[4][3] = {{100, -50, 0}, {500, -50, 300}, {900, -1000, 0}, {-3000, 0, 2500}};
        for (int v = 0; v < 4; v++) { for (int k = 0; k < 3; k++) w16(0x120400 + v * 8 + 2 * k, vv[v][k]); w8(0x120440 + v, v); w8(0x120480 + v, 0); }
        w16(0x120460, 0); w16(0x120462, -4096); w16(0x120464, 0);
        for (int v = 0; v < 4; v++) {
            int32_t T[3] = {0, 0, 0}, wp[3]; uint8_t c[3];
            ns_light_world_pos(ID, T, vv[v], wp);
            int16_t n[3] = {0, -4096, 0};
            ns_light_preshade_vertex(&env, &lst, 1, wp, ID, n, c);
            for (int a = 0; a < 3; a++) w8(RG + v * 4 + a, c[a]);
        }
    }
    FILE *f = fopen(argv[1], "wb"); if (!f) return 1;
    fwrite(R, 1, 0x200000, f); fclose(f);

    /* borde: se parte de las globales y un objeto con punteros al final de la RAM */
    memset(R, 0, 0x200000 + 8);
    globals();
    {
        uint32_t C = 0x130000, M = C + 0x220;
        object(C, 0x09, 0x110300, M);
        wmat(0x110300, DIR); wmat(0x110320, COL);
        uint32_t o = C + 0x48;
        w16(o + 0x52, 1); w32(o + 0x54, 0x801FFFF0); w32(o + 0x48, 0x801FFFFE);
        w32(M + 4, 1); w32(M + 0x40, 2); w32(M + 0x44, 0x801FFFF8); w32(M + 0x48, 0x801FFFFC);
        w32(0x801FFFF0, 1);
        /* y un precalculado cuyos arrays terminan en el último byte */
        uint32_t B = 0x140000, M2 = B + 0x220;
        object(B, 0x02, 0, M2);
        w32(B + 0x48 + 0x44, 0x801FFFFE);
        w32(M2 + 4, 4000); w32(M2 + 0x34, 4); w32(M2 + 0x38, 0x801FFFF8); w32(M2 + 0x3C, 0x801FFFFF);
        w32(M2 + 0x40, 2); w32(M2 + 0x44, 0x801FFFFC); w32(M2 + 0x48, 0x801FFFFF);
    }
    f = fopen(argv[2], "wb"); if (!f) return 1;
    fwrite(R, 1, 0x200000, f); fclose(f);
    free(R);
    return 0;
}
