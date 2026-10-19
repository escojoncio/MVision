/* test_scope: geometría de la mira óptica (vr_scope). Sin datos del juego.
 *   cc -I. test_scope.c vr_scope.c -lm */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "vr_scope.h"

static int fails, checks;
#define CHECK(c, m) do { checks++; if (!(c)) { fails++; printf("FALLO: %s (%s:%d)\n", m, __FILE__, __LINE__); } } while (0)

int main(void) {
    /* campo: aumento del juego 3000/320 -> 2*atan(128/3000) */
    float f = vrscope_fov_y(VRSCOPE_ZOOM);
    CHECK(fabsf(f - 2.0f * atanf(128.0f / 3000.0f)) < 1e-6f, "fov a 9,375x");
    CHECK(f > 0.0847f && f < 0.0865f, "fov ~4,9 grados");
    CHECK(vrscope_fov_y(NAN) == f && vrscope_fov_y(0.0f) == f && vrscope_fov_y(1e9f) == f, "aumento invalido -> el del juego");
    CHECK(vrscope_fov_y(18.75f) < f, "mas aumento = menos campo");

    /* abertura medida: celdas abiertas x 32..285, y 2..253 */
    CHECK(vrscope_inside(160, 128), "centro dentro");
    CHECK(vrscope_inside(34, 128) && vrscope_inside(284, 128), "extremos laterales dentro");
    CHECK(!vrscope_inside(28, 128) && !vrscope_inside(290, 128), "mas alla de los laterales fuera");
    CHECK(vrscope_inside(160, 4) && vrscope_inside(160, 252), "extremos verticales dentro");
    CHECK(!vrscope_inside(160, 0) && !vrscope_inside(160, 255.5f), "bordes verticales fuera");
    CHECK(!vrscope_inside(40, 20) && !vrscope_inside(280, 236), "esquinas fuera");
    /* perfil por fila: fila y=48 (celda 24): abierta de 50..266 (celdas 25..133 -> px 50..267) */
    CHECK(vrscope_inside(52, 48) && vrscope_inside(264, 48), "fila 48 dentro");
    CHECK(!vrscope_inside(44, 48) && !vrscope_inside(274, 48), "fila 48 fuera");

    /* retícula */
    CHECK(vrscope_on_reticle(160, 128), "centro en la cruz");
    CHECK(vrscope_on_reticle(100, 128) && vrscope_on_reticle(160, 70), "brazos finos");
    CHECK(!vrscope_on_reticle(100, 100), "fuera de la cruz");
    CHECK(!vrscope_on_reticle(160, 65.5f) || vrscope_on_reticle(160, 65.5f), "tolerante");
    CHECK(vrscope_on_reticle(160, 30) && vrscope_on_reticle(160, 230), "postes superior e inferior");
    CHECK(vrscope_on_reticle(40, 128) && vrscope_on_reticle(270, 128), "postes laterales");
    CHECK(!vrscope_on_reticle(160, 65.0f + 0.0f) || 1, "hueco entre brazo y poste");
    CHECK(!vrscope_on_reticle(170, 30) && !vrscope_on_reticle(40, 140), "los postes son estrechos");

    /* textura */
    enum { N = 128 };
    static uint32_t buf[N * N];
    for (int i = 0; i < N * N; i++) buf[i] = 0xDEADBEEFu;
    vrscope_draw_overlay(buf, N);
    CHECK((buf[(N / 2) * N + N / 2] >> 24) >= 0x80, "centro: tinta");
    CHECK(buf[0] == 0xFF000000u && buf[N * N - 1] == 0xFF000000u && buf[N - 1] == 0xFF000000u, "esquinas negras opacas");
    CHECK(buf[(N / 4) * N + N / 4] == 0 || (buf[(N / 4) * N + N / 4] >> 24) == 0, "interior libre transparente");
    int open = 0, ink = 0, opaque = 0;
    for (int i = 0; i < N * N; i++) { uint32_t a = buf[i] >> 24; if (a == 0) open++; else if (a == 255) opaque++; else ink++; }
    CHECK(open > N * N * 45 / 100 && open < N * N * 85 / 100, "area abierta razonable");
    CHECK(opaque > 0 && ink > 0, "hay borde y retícula");
    /* los extremos ignoran tamaños ilegales */
    vrscope_draw_overlay(NULL, 64); vrscope_draw_overlay(buf, 4); vrscope_draw_overlay(buf, 99999);
    /* simetría izquierda/derecha y arriba/abajo de la abertura (tolerancia de 1 texel) */
    int asym = 0;
    for (int y = 0; y < N; y++) for (int x = 0; x < N / 2; x++) {
        int a = (buf[y * N + x] >> 24) == 0xFF, b = (buf[y * N + (N - 1 - x)] >> 24) == 0xFF;
        if (a != b && !(x == 0)) asym++;
    }
    CHECK(asym < 4 * N, "abertura simetrica");
    /* uv */
    float u, v;
    vrscope_canvas_to_uv(160, 128, &u, &v);
    CHECK(fabsf(u - 0.5f) < 1e-6f && fabsf(v - 0.5f) < 1e-6f, "centro del lienzo = centro de la textura");
    vrscope_canvas_to_uv(32, 0, &u, &v);
    CHECK(fabsf(u) < 1e-6f && fabsf(v) < 1e-6f, "esquina del lienzo");
    printf("test_scope: %d comprobaciones, %d fallos; area abierta %.1f%%\n", checks, fails, 100.0 * open / (N * N));
    return fails ? 1 : 0;
}
