/* test_mmpx: mmpx_scale2x (mmpx.h) frente a los casos del artículo: zonas planas, líneas de 1 px, diagonal 1:1, pendiente 2:1, esquinas y transparencia. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mmpx.h"

static int fails;
static uint32_t col(char c) { return c == '#' ? 0xFF000000u : c == '.' ? 0xFFFFFFFFu : c == 'o' ? 0xFF0000FFu : 0u; }   /* ' ' = transparente */

/* amplía `in` (filas de texto) y lo compara con `want` (el doble de filas y columnas) */
static void pattern(const char *name, int w, int h, const char *const *in, const char *const *want) {
    uint8_t *s = malloc((size_t)w * h * 4), *d = malloc((size_t)w * h * 16);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { uint32_t c = col(in[y][x]); memcpy(s + ((size_t)y * w + x) * 4, &c, 4); }
    mmpx_scale2x(s, w, h, d);
    int bad = 0;
    for (int y = 0; y < 2 * h; y++) for (int x = 0; x < 2 * w; x++) { uint32_t c; memcpy(&c, d + ((size_t)y * 2 * w + x) * 4, 4); if (c != col(want[y][x])) bad++; }
    if (bad) {
        fails++; printf("FALLO %s: %d píxeles distintos\n", name, bad);
        for (int y = 0; y < 2 * h; y++) { for (int x = 0; x < 2 * w; x++) { uint32_t c; memcpy(&c, d + ((size_t)y * 2 * w + x) * 4, 4); putchar(c == col('#') ? '#' : c == col('.') ? '.' : c == col('o') ? 'o' : c ? '?' : ' '); } putchar('\n'); }
    }
    free(s); free(d);
}

int main(void) {
    {   static const char *in[] = {"....", "....", "...."}, *want[] = {"........", "........", "........", "........", "........", "........"};
        pattern("plano", 4, 3, in, want); }
    {   static const char *in[] = {"......", "......", "######", "......", "......"};
        static const char *want[] = {"............", "............", "............", "............", "############", "############", "............", "............", "............", "............"};
        pattern("linea horizontal de 1 px", 6, 5, in, want); }
    {   static const char *in[] = {"..#..", "..#..", "..#..", "..#.."};
        static const char *want[] = {"....##....", "....##....", "....##....", "....##....", "....##....", "....##....", "....##....", "....##...."};
        pattern("linea vertical de 1 px", 5, 4, in, want); }
    {   static const char *in[] = {"#.....", ".#....", "..#...", "...#..", "....#.", ".....#"};
        static const char *want[] = {"##..........", "#.#.........", ".###........", "..###.......", "...###......", "....###.....", ".....###....", "......###...", ".......###..", "........###.", ".........#.#", "..........##"};
        pattern("diagonal 1:1 oscura sobre claro", 6, 6, in, want); }
    {   static const char *in[] = {".#####", "#.####", "##.###", "###.##", "####.#", "#####."};
        static const char *want[] = {"..##########", ".#.#########", "#...########", "##...#######", "###...######", "####...#####", "#####...####", "######...###", "#######...##", "########...#", "#########.#.", "##########.."};
        pattern("diagonal 1:1 clara sobre oscuro", 6, 6, in, want); }
    {   static const char *in[] = {"........", "##......", "..##....", "....##..", "......##", "........"};
        static const char *want[] = {"................", "................", "####............", "######..........", "..######........", "....######......", "......######....", "........######..", "..........######", "............####", "................", "................"};
        pattern("pendiente 2:1", 8, 6, in, want); }
    {   static const char *in[] = {"......", ".####.", ".####.", ".####.", ".####.", "......"};
        static const char *want[] = {"............", "............", "..########..", "..########..", "..########..", "..########..", "..########..", "..########..", "..########..", "..########..", "............", "............"};
        pattern("esquinas de un rectangulo", 6, 6, in, want); }
    {   static const char *in[] = {"......", ".#....", ".#....", ".#....", ".####.", "......"};
        static const char *want[] = {"............", "............", "..##........", "..##........", "..##........", "..##........", "..##........", "..##........", "..########..", "..########..", "............", "............"};
        pattern("esquina en L de 1 px", 6, 6, in, want); }
    {   static const char *in[] = {"      ", "  oo  ", " oooo ", " oooo ", "  oo  ", "      "};
        static const char *want[] = {"            ", "            ", "     oo     ", "   oooooo   ", "   oooooo   ", "  oooooooo  ", "  oooooooo  ", "   oooooo   ", "   oooooo   ", "     oo     ", "            ", "            "};
        pattern("sprite sobre transparente", 6, 6, in, want); }
    {   /* transparente con color oculto distinto: es el mismo «color» (no crea bordes) y sale 0,0,0,0 */
        uint8_t s[2 * 2 * 4] = {10, 20, 30, 0, 200, 100, 50, 0, 1, 2, 3, 0, 90, 80, 70, 0}, d[4 * 4 * 4];
        memset(d, 0xEE, sizeof d);
        mmpx_scale2x(s, 2, 2, d);
        int bad = 0; for (size_t i = 0; i < sizeof d; i++) bad += d[i] != 0;
        if (bad) { fails++; printf("FALLO transparente con color oculto: %d bytes distintos de 0\n", bad); } }
    {   /* ruido: cada píxel de salida es uno de los 9 del entorno de su píxel de origen (nunca un color nuevo) y no se lee fuera */
        enum { W = 37, H = 23 };
        uint8_t *s = malloc(W * H * 4), *d = malloc(W * H * 16);
        uint32_t seed = 12345u; int bad = 0;
        for (int i = 0; i < W * H; i++) { seed = seed * 1664525u + 1013904223u; uint32_t c = 0xFF000000u | (0x404040u * ((seed >> 28) & 3)); if (((seed >> 20) & 7) == 0) c = 0; memcpy(s + i * 4, &c, 4); }
        mmpx_scale2x(s, W, H, d);
        for (int y = 0; y < 2 * H; y++) for (int x = 0; x < 2 * W; x++) {
            int ok = 0;
            for (int dy = -1; dy <= 1 && !ok; dy++) for (int dx = -1; dx <= 1; dx++) {
                int sx = x / 2 + dx, sy = y / 2 + dy;
                sx = sx < 0 ? 0 : sx >= W ? W - 1 : sx; sy = sy < 0 ? 0 : sy >= H ? H - 1 : sy;
                if (!memcmp(d + ((size_t)y * 2 * W + x) * 4, s + ((size_t)sy * W + sx) * 4, 4)) { ok = 1; break; }
            }
            bad += !ok;
        }
        if (bad) { fails++; printf("FALLO ruido: %d píxeles de salida que no son copia de un vecino\n", bad); }
        free(s); free(d); }
    {   uint8_t one[4] = {1, 2, 3, 255}, d[16]; mmpx_scale2x(one, 1, 1, d);   /* 1x1 */
        for (int i = 0; i < 4; i++) if (memcmp(d + i * 4, one, 4)) { fails++; printf("FALLO 1x1\n"); break; }
        mmpx_scale2x(one, 0, 1, d); mmpx_scale2x(NULL, 1, 1, d); }
    {   /* planos apilados (capa 2D empaquetada): con 1 plano y sin transparentes es mmpx_scale2x; con 3, los planos van juntos (cada píxel de salida copia UN píxel de origen entero) y dos píxeles
         * iguales en el plano 0 pero distintos en otro NO son iguales (no se funden) */
        enum { W = 19, H = 13 };
        uint8_t *s3 = malloc(W * H * 4 * 3), *d3 = malloc(W * H * 16 * 3), *d1 = malloc(W * H * 16), *d1p = malloc(W * H * 16);
        uint32_t seed = 99u; int bad = 0, split = 0;
        for (int i = 0; i < W * H; i++) {
            seed = seed * 1664525u + 1013904223u;
            uint32_t a = 0xFF000000u | (0x505050u * ((seed >> 28) & 3)), b = 0xFF000000u | ((seed >> 20) & 1 ? 0x101010u : 0u), g = 0xFFFFFFFFu;
            memcpy(s3 + i * 4, &a, 4); memcpy(s3 + (W * H + i) * 4, &b, 4); memcpy(s3 + (2 * W * H + i) * 4, &g, 4);
        }
        mmpx_scale2x(s3, W, H, d1); mmpx_scale2x_planes(s3, W, H, 1, d1p);
        if (memcmp(d1, d1p, W * H * 16)) { fails++; printf("FALLO planos: con 1 plano opaco no coincide con mmpx_scale2x\n"); }
        mmpx_scale2x_planes(s3, W, H, 3, d3);
        for (int y = 0; y < 2 * H; y++) for (int x = 0; x < 2 * W; x++) {
            int ok = 0;
            for (int dy = -1; dy <= 1 && !ok; dy++) for (int dx = -1; dx <= 1; dx++) {
                int sx = x / 2 + dx, sy = y / 2 + dy;
                sx = sx < 0 ? 0 : sx >= W ? W - 1 : sx; sy = sy < 0 ? 0 : sy >= H ? H - 1 : sy;
                int all = 1;
                for (int p = 0; p < 3; p++) all &= !memcmp(d3 + ((size_t)(p * 2 * H + y) * 2 * W + x) * 4, s3 + ((size_t)(p * H + sy) * W + sx) * 4, 4);
                if (all) { ok = 1; break; }
            }
            bad += !ok;
            split += memcmp(d3 + ((size_t)y * 2 * W + x) * 4, d1 + ((size_t)y * 2 * W + x) * 4, 4) != 0;
        }
        if (bad) { fails++; printf("FALLO planos: %d píxeles de salida cuyos 3 planos no salen del mismo píxel de origen\n", bad); }
        if (!split) { fails++; printf("FALLO planos: el plano 1 no influye en la igualdad\n"); }
        uint32_t add[9], dd[36]; for (int i = 0; i < 9; i++) add[i] = i == 4 ? 0x00404040u : 0u;   /* aditivo premultiplicado (alfa 0 con color): no es transparente */
        mmpx_scale2x_planes((const uint8_t *)add, 3, 3, 1, (uint8_t *)dd);
        if (dd[2 * 6 + 2] != 0x00404040u || dd[3 * 6 + 3] != 0x00404040u) { fails++; printf("FALLO planos: el píxel aditivo de alfa 0 se pierde\n"); }
        free(s3); free(d3); free(d1); free(d1p); }
    printf(fails ? "test_mmpx: %d FALLOS\n" : "test_mmpx: OK\n", fails);
    return fails != 0;
}
