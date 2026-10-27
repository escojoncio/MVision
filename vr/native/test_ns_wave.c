/* Onda de pantalla bajo el agua (post-proceso por ojo, ns_screen_wave_*): desplazamiento horizontal por fila de la IMAGEN FINAL del ojo, no de las texturas. Sintética, sin datos del juego.
 * Regla: la onda mueve toda la escena por filas (una arista vertical recta sale ondulada, sea cual sea su textura); la profundidad no interviene. */
#include "ns_screen.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_ok;
#define CHECK(c, ...) do { if (c) g_ok++; else { g_fail++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int near(float a, float b, float e) { return fabsf(a - b) <= e; }

enum { W = 128, H = 96 };
static const float TWO_PI = 6.2831853f;

/* Fórmula pura: dx(fila) = amp * (ancho del ojo / 1888) * sin(freq * fila + fase) */
static void test_formula(void) {
    const float ww[4] = { 12.0f, TWO_PI / 32.0f, 0.7f, 0.0f };
    CHECK(near(ns_screen_wave_dx(ww, 0.0f, NS_WAVE_REF_W), 12.0f * sinf(0.7f), 1e-4f), "fila 0 = amp*sin(fase)");
    CHECK(near(ns_screen_wave_dx(ww, 5.0f, NS_WAVE_REF_W), 12.0f * sinf(TWO_PI * 5.0f / 32.0f + 0.7f), 1e-4f), "fila 5");
    CHECK(near(ns_screen_wave_dx(ww, 5.0f, NS_WAVE_REF_W), ns_screen_wave_dx(ww, 37.0f, NS_WAVE_REF_W), 1e-3f), "se repite cada periodo (32 filas)");
    CHECK(near(ns_screen_wave_dx(ww, 5.0f, NS_WAVE_REF_W / 2.0f), 0.5f * ns_screen_wave_dx(ww, 5.0f, NS_WAVE_REF_W), 1e-4f), "escala con el ancho del ojo");
    float mx = 0; for (int r = -200; r <= 200; r++) { float d = fabsf(ns_screen_wave_dx(ww, (float)r, NS_WAVE_REF_W)); if (d > mx) mx = d; }
    CHECK(mx <= 12.0f + 1e-4f && mx > 11.0f, "acotada por la amplitud (%f)", mx);
    const float off[4] = { 0, 0, 0, 0 };
    CHECK(ns_screen_wave_dx(off, 3.0f, NS_WAVE_REF_W) == 0.0f, "sin amplitud no hay onda");
    CHECK(near(ns_screen_wave_row(50.0f, 50.0f, 80.0f, 320.0f), 0.0f, 1e-6f) && near(ns_screen_wave_row(130.0f, 50.0f, 80.0f, 320.0f), 320.0f, 1e-3f), "fila del motor: 0 en el centro óptico, H a una tangente");
}

/* Imagen con una arista vertical recta entre los texels 63 y 64 (R = 255 a la derecha; cruce a 127,5 en 63,5): tras la onda la arista de cada fila debe estar en 63,5 - dx(fila), sin mirar la textura. */
static void test_whole_image(void) {
    static uint8_t src[W * H * 3], dst[W * H * 3];
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { uint8_t v = x >= 64 ? 255 : 0; src[(y * W + x) * 3] = v; src[(y * W + x) * 3 + 1] = (uint8_t)(x * 2); src[(y * W + x) * 3 + 2] = v; }
    const float cy = H * 0.5f, fy = H * 0.6f, Hm = 160.0f;
    /* amplitud tal que el eye de W px mida 3 px: amp_ref * W/1888 = 3 */
    const float ww[4] = { 3.0f * NS_WAVE_REF_W / (float)W, TWO_PI / 16.0f, 0.4f, 0.0f };
    ns_screen_wave_rgb8(ww, src, dst, W, H, cy, fy, Hm);
    int bad = 0, moved = 0; float lo = 1e9f, hi = -1e9f;
    for (int y = 0; y < H; y++) {
        float e = ns_screen_wave_dx(ww, ns_screen_wave_row((float)y + 0.5f, cy, fy, Hm), (float)W);
        /* posición de la arista: el 255 del canal R se interpola; borde donde el valor cruza 127,5 (interpolación lineal entre píxeles: error < 0,1 px en el tramo de la rampa) */
        float edge = -1;
        for (int x = 0; x + 1 < W; x++) { float a = dst[(y * W + x) * 3], b = dst[(y * W + x + 1) * 3]; if (a < 127.5f && b >= 127.5f) { edge = (float)x + 1.0f - (b - 127.5f) / (b - a); break; } }
        float expect = 63.5f - e;
        if (edge < 0 || fabsf(edge - expect) > 0.12f) { bad++; if (bad < 4) printf("  fila %d: arista %f esperada %f\n", y, edge, expect); }
        if (fabsf(e) > 0.5f) moved++;
        if (expect < lo) lo = expect;
        if (expect > hi) hi = expect;
    }
    CHECK(bad == 0, "la arista recta ondula como la fórmula en cada fila (%d filas mal)", bad);
    CHECK(moved > H / 3 && hi - lo > 4.0f, "la imagen entera se mueve por filas (filas movidas %d, recorrido %.2f px)", moved, hi - lo);
    /* el canal G (degradado horizontal 2*x, independiente de la arista) se desplaza igual: no hay nada por textura */
    int badg = 0;
    for (int y = 0; y < H; y++) {
        float e = ns_screen_wave_dx(ww, ns_screen_wave_row((float)y + 0.5f, cy, fy, Hm), (float)W);
        for (int x = 12; x < W - 12; x++) if (fabsf(dst[(y * W + x) * 3 + 1] - 2.0f * ((float)x + e)) > 0.6f) badg++;
    }
    CHECK(badg == 0, "el degradado también se desplaza por fila (%d px mal)", badg);
    /* amplitud 0: copia exacta */
    const float off[4] = { 0, 0, 0, 0 };
    ns_screen_wave_rgb8(off, src, dst, W, H, cy, fy, Hm);
    CHECK(memcmp(src, dst, sizeof src) == 0, "sin onda la imagen no cambia");
}

int main(void) {
    test_formula();
    test_whole_image();
    printf("test_ns_wave: %d ok, %d fallos\n", g_ok, g_fail);
    return g_fail != 0;
}
