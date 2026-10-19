/* Prueba de vr_split (Linux): make -C vr/scene test-split */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_split.h"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FALLO: %s (%s:%d)\n", m, __FILE__, __LINE__); fails++; } } while (0)

static VrVtx mk(float x, float y, float z, uint32_t col) { VrVtx v; memset(&v, 0, sizeof v); v.pos[0] = x; v.pos[1] = y; v.pos[2] = z; v.rgba = col; v.uv[0] = x; v.uv[1] = z; v.tpage = 7; v.pad = 3; return v; }

static float area(const VrVtx *t) {
    float a[3] = {t[1].pos[0] - t[0].pos[0], t[1].pos[1] - t[0].pos[1], t[1].pos[2] - t[0].pos[2]};
    float b[3] = {t[2].pos[0] - t[0].pos[0], t[2].pos[1] - t[0].pos[1], t[2].pos[2] - t[0].pos[2]};
    float c[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    return 0.5f * sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
}

int main(void) {
    /* batch 0: un triángulo de 63 m de largo (opaco 3D); batch 1: uno pequeño; batch 2: 2D (no se toca) con un triángulo enorme */
    VrVtx v[9];
    v[0] = mk(0, 0, 0, 0xFF000000u); v[1] = mk(63, 0, 0, 0xFFFFFFFFu); v[2] = mk(0, 0, 3, 0xFF808080u);
    v[3] = mk(0, 0, 0, 0xFF000000u); v[4] = mk(1, 0, 0, 0xFF000000u); v[5] = mk(0, 0, 1, 0xFF000000u);
    v[6] = mk(0, 0, 0, 0); v[7] = mk(500, 0, 0, 0); v[8] = mk(0, 500, 0, 0);
    VrBatch b[3]; memset(b, 0, sizeof b);
    b[0].first = 0; b[0].count = 3; b[0].kind = 0;
    b[1].first = 3; b[1].count = 3; b[1].kind = 1;
    b[2].first = 6; b[2].count = 3; b[2].kind = 2;
    VrSplit s;
    int rc = vr_split_run(v, 9, b, 3, 2.0f, 100000, &s);
    CHECK(rc == 0, "hay que partir");
    CHECK(s.first[0] == 9 && s.count[0] > 3, "batch 0 reescrito tras los originales");
    CHECK(s.first[1] == 0xFFFFFFFFu && s.first[2] == 0xFFFFFFFFu, "batch pequeño y 2D intactos");
    CHECK(s.count[0] % 3 == 0 && s.n == s.count[0], "multiplo de 3");
    float total = 0, mx = 0;
    for (uint32_t k = 0; k < s.count[0]; k += 3) {
        const VrVtx *t = s.v + k;
        total += area(t);
        for (int i = 0; i < 3; i++) { const VrVtx *p = &t[i], *q = &t[(i + 1) % 3]; float e = sqrtf((p->pos[0]-q->pos[0])*(p->pos[0]-q->pos[0]) + (p->pos[1]-q->pos[1])*(p->pos[1]-q->pos[1]) + (p->pos[2]-q->pos[2])*(p->pos[2]-q->pos[2])); if (e > mx) mx = e; }
        CHECK(t[0].tpage == 7 && t[0].pad == 3, "atributos copiados");
    }
    CHECK(fabsf(total - 0.5f * 63 * 3) < 0.05f, "el área se conserva");
    CHECK(mx <= 2.0f + 1e-3f, "ninguna arista supera el umbral");
    CHECK(s.split_tris == 1 && s.cap_skipped == 0, "contadores");
    printf("partido en %u triángulos (arista máx %.3f)\n", s.count[0] / 3, mx);
    vr_split_free(&s);

    /* tope: con 10 triángulos de salida como máximo el grande queda sin partir del todo pero la salida no lo supera */
    rc = vr_split_run(v, 9, b, 3, 2.0f, 10, &s);
    CHECK(rc == 0 && s.out_tris <= 10 && s.cap_skipped == 1, "tope de salida");
    vr_split_free(&s);
    /* nada que partir */
    rc = vr_split_run(v, 9, b, 3, 100.0f, 1000, &s);
    CHECK(rc == 1, "nada que partir");
    /* parámetros inválidos */
    CHECK(vr_split_run(v, 9, b, 3, 0.0f, 1000, &s) == -1, "umbral 0");
    CHECK(vr_split_run(v, 9, b, 3, NAN, 1000, &s) == -1, "umbral NaN");

    /* recuento: vista identidad, cercano a 0.1, el triángulo 63 m cruza si tiene vértices a z<0 y z>0 (profundidad = -z) */
    float view[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    VrVtx w[3] = {mk(0, 0, -1, 0), mk(30, 0, -80, 0), mk(0, 5, 2, 0)};   /* profundidades 1, 80, -2 */
    VrScan sc; memset(&sc, 0, sizeof sc);
    vr_scan_tris(w, 3, view, 0.1f, 12.0f, &sc);
    CHECK(sc.tris == 1 && sc.big == 1 && sc.cross == 1 && sc.big_cross == 1, "recuento big/cross");
    VrVtx w2[3] = {mk(0, 0, -1, 0), mk(1, 0, -2, 0), mk(0, 1, -3, 0)};
    memset(&sc, 0, sizeof sc);
    vr_scan_tris(w2, 3, view, 0.1f, 12.0f, &sc);
    CHECK(sc.tris == 1 && sc.big == 0 && sc.cross == 0, "triángulo normal");
    printf(fails ? "vr_split: %d FALLOS\n" : "vr_split: OK\n", fails);
    return fails != 0;
}
