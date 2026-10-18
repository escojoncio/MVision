/* Prueba aleatoria: el modelo de referencia (ref_raster) debe reproducir la VRAM del oráculo (gpu.c)
 * bit a bit tras una secuencia aleatoria de comandos GP0 que cubre todos los tipos de primitiva, modos de
 * textura, semitransparencias, máscara, ventana de textura, tramado, volteo, polilíneas, fills, subidas y copias.
 * Además comprueba invariantes de la ruta de alta resolución. Uso: ref_selftest [iteraciones] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu.h"
#include "ref_raster.h"

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static int rr(int a, int b) { return a + (int)(rnd() % (uint32_t)(b - a + 1)); }
static uint32_t xy(int x, int y) { return (uint32_t)(x & 0x7FF) | (uint32_t)(y & 0x7FF) << 16; }

static void rand_vram(void) {
    gpu_gp0(0xA0000000); gpu_gp0(0); gpu_gp0(1024 | 512u << 16);
    for (int i = 0; i < 1024 * 512 / 2; i++) {
        uint32_t a = rnd(), b = rnd();
        /* mezcla de ceros (transparencia) y bit15 variado */
        if (rnd() % 5 == 0) a &= 0xFFFF0000u; if (rnd() % 5 == 0) b &= 0x0000FFFFu;
        gpu_gp0(a ^ (b << 16) ^ (b >> 16));
    }
}

static void env(void) {
    int tp = rr(0, 2), abr = rr(0, 3);
    gpu_gp0(0xE1000000 | (uint32_t)rr(0, 15) | (uint32_t)rr(0, 1) << 4 | (uint32_t)abr << 5 | (uint32_t)tp << 7 |
            (uint32_t)(rnd() & 1) << 9 | (uint32_t)(rnd() % 8 == 0) << 11 | (uint32_t)(rnd() & 1) << 12 | (uint32_t)(rnd() & 1) << 13);
    if (rnd() % 3 == 0) gpu_gp0(0xE2000000 | (uint32_t)rr(0, 31) | (uint32_t)rr(0, 31) << 5 | (uint32_t)rr(0, 31) << 10 | (uint32_t)rr(0, 31) << 15);
    else gpu_gp0(0xE2000000);
    int x1 = rr(0, 300), y1 = rr(0, 200);
    gpu_gp0(0xE3000000 | (uint32_t)x1 | (uint32_t)y1 << 10);
    gpu_gp0(0xE4000000 | (uint32_t)(x1 + rr(1, 500)) | (uint32_t)(y1 + rr(1, 300)) << 10);
    gpu_gp0(0xE5000000 | (uint32_t)(rr(-100, 100) & 0x7FF) | (uint32_t)(rr(-100, 100) & 0x7FF) << 11);
    gpu_gp0(0xE6000000 | (rnd() % 4 == 0) | (uint32_t)(rnd() % 4 == 0) << 1);
}

static uint32_t uvw(int u, int v, uint32_t hi) { return (uint32_t)(u & 255) | (uint32_t)(v & 255) << 8 | hi << 16; }

static void poly(void) {
    int quad = rr(0, 1), gour = rr(0, 1), tex = rr(0, 1), semi = rr(0, 1), raw = rr(0, 1);
    uint32_t op = 0x20 | gour << 4 | quad << 3 | tex << 2 | semi << 1 | raw;
    int n = quad ? 4 : 3, cx = rr(0, 380), cy = rr(0, 250), sz = rr(2, 120);
    uint32_t clut = (uint32_t)(rr(0, 63) | rr(0, 400) << 6);
    uint32_t tpg = (uint32_t)(rr(0, 15) | rr(0, 1) << 4 | rr(0, 3) << 5 | rr(0, 2) << 7);
    gpu_gp0(op << 24 | (rnd() & 0xFFFFFF));
    for (int i = 0; i < n; i++) {
        if (i && gour) gpu_gp0(rnd() & 0xFFFFFF);
        gpu_gp0(xy(cx + rr(-sz, sz), cy + rr(-sz, sz)));
        if (tex) gpu_gp0(uvw(rr(0, 255), rr(0, 255), i == 0 ? clut : i == 1 ? tpg : 0));
    }
}

static void rect(void) {
    int sz = rr(0, 3), tex = rr(0, 1), semi = rr(0, 1), raw = rr(0, 1);
    uint32_t op = 0x60 | sz << 3 | tex << 2 | semi << 1 | raw;
    gpu_gp0(op << 24 | (rnd() & 0xFFFFFF));
    gpu_gp0(xy(rr(-20, 380), rr(-20, 250)));
    if (tex) gpu_gp0(uvw(rr(0, 255), rr(0, 255), (uint32_t)(rr(0, 63) | rr(0, 400) << 6)));
    if (sz == 0) gpu_gp0((uint32_t)rr(1, 90) | (uint32_t)rr(1, 70) << 16);
}

static void line(void) {
    int gour = rr(0, 1), semi = rr(0, 1), poly = rr(0, 1);
    uint32_t op = 0x40 | gour << 4 | poly << 3 | semi << 1;
    gpu_gp0(op << 24 | (rnd() & 0xFFFFFF));
    gpu_gp0(xy(rr(-20, 380), rr(-20, 250)));
    int n = poly ? rr(1, 4) : 1;
    for (int i = 0; i < n; i++) {
        if (gour) gpu_gp0(rnd() & 0xFFFFFF);
        gpu_gp0(xy(rr(-20, 380), rr(-20, 250)));
    }
    if (poly) gpu_gp0(0x55555555);
}

static void vram_ops(void) {
    switch (rr(0, 3)) {
    case 0: gpu_gp0(0x02000000 | (rnd() & 0xFFFFFF)); gpu_gp0((uint32_t)rr(0, 900) | (uint32_t)rr(0, 450) << 16);
            gpu_gp0((uint32_t)rr(1, 120) | (uint32_t)rr(1, 80) << 16); break;
    case 1: gpu_gp0(0x80000000); gpu_gp0((uint32_t)rr(0, 900) | (uint32_t)rr(0, 450) << 16);
            gpu_gp0((uint32_t)rr(0, 900) | (uint32_t)rr(0, 450) << 16); gpu_gp0((uint32_t)rr(1, 100) | (uint32_t)rr(1, 60) << 16); break;
    case 2: { /* copia solapada */
        int x = rr(0, 800), y = rr(0, 400), w = rr(8, 100), h = rr(8, 60);
        gpu_gp0(0x80000000); gpu_gp0((uint32_t)x | (uint32_t)y << 16);
        gpu_gp0((uint32_t)(x + rr(-6, 6)) | (uint32_t)(y + rr(-6, 6)) << 16); gpu_gp0((uint32_t)w | (uint32_t)h << 16); break; }
    default: { int w = rr(1, 40), h = rr(1, 30);
        gpu_gp0(0xA0000000); gpu_gp0((uint32_t)rr(0, 900) | (uint32_t)rr(0, 450) << 16); gpu_gp0((uint32_t)w | (uint32_t)h << 16);
        for (int i = 0; i < (w * h + 1) / 2; i++) gpu_gp0(rnd()); break; }
    }
}

static int check(RefR *r, const char *what, int it) {
    const uint16_t *rv = ref_vram(r);
    int bad = 0, first = -1;
    for (int i = 0; i < 1024 * 512; i++) if (rv[i] != ((const uint16_t *)gpu_vram)[i]) { if (first < 0) first = i; bad++; }
    if (bad) fprintf(stderr, "FALLO %s it=%d: %d palabras (primera x=%d y=%d ref=%04x oraculo=%04x)\n", what, it, bad, first & 1023,
                     first >> 10, rv[first], ((const uint16_t *)gpu_vram)[first]);
    return bad;
}

int main(int argc, char **argv) {
    int iters = argc > 1 ? atoi(argv[1]) : 300, fails = 0;
    RefOpts o1 = {1, 1, 0};
    RefR *r = ref_create(&o1);
    long counts[7] = {0};
    for (int it = 0; it < iters; it++) {
        gpu_reset();
        gpu_capture_enable(0);
        gpu_capture_enable(1);
        rand_vram();
        for (int k = 0; k < 60; k++) {
            if (rnd() % 4 == 0) env();
            switch (rr(0, 9)) {
            case 0: case 1: case 2: case 3: poly(); break;
            case 4: case 5: rect(); break;
            case 6: line(); break;
            default: vram_ops(); break;
            }
        }
        gpu_gp1(0x05000000 | (uint32_t)(((it & 1) + 1) << 10)); /* cambio de display: cierra la lista */
        const GpuDrawList *l = gpu_capture_frame();
        if (!l) { fprintf(stderr, "FALLO: sin lista it=%d\n", it); fails++; continue; }
        for (uint32_t i = 0; i < l->nops; i++) counts[l->ops[i].type]++;
        ref_apply(r, l, 1);
        fails += check(r, "1x", it) != 0;
        /* VRAM 1x exacta tras reaplicar solo nativo */
    }
    printf("selftest 1x: %d iteraciones, %d fallos (ops: poly=%ld rect=%ld line=%ld fill=%ld vcopy=%ld upload=%ld)\n", iters, fails,
           counts[0], counts[1], counts[2], counts[3], counts[4], counts[5]);
    ref_destroy(r);

    /* Invariante de alta resolución: rects/subidas/fills/copias sin modulación ni mezcla -> la imagen NxN promediada
     * debe ser idéntica a la 1x (replicación exacta). */
    int hfails = 0;
    for (int N = 2; N <= 4; N += 2) {
        RefOpts a = {1, 1, 0}, b = {N, N == 2 ? 2 : 1, REF_HQ};
        RefR *ra = ref_create(&a), *rb = ref_create(&b);
        for (int it = 0; it < 40; it++) {
            gpu_reset(); gpu_capture_enable(0); gpu_capture_enable(1);
            rand_vram();
            gpu_gp0(0xE3000000); gpu_gp0(0xE4000000 | 639 | 479u << 10);
            gpu_gp0(0xE6000000); gpu_gp0(0xE1000000 | (uint32_t)rr(0, 15) | (uint32_t)rr(0, 2) << 7);
            for (int k = 0; k < 30; k++) {
                if (rr(0, 1)) { /* sprite crudo opaco-o-no, sin semitransparencia */
                    gpu_gp0(0x65000000 | (rnd() & 0xFFFFFF)); gpu_gp0(xy(rr(0, 300), rr(0, 200)));
                    gpu_gp0(uvw(rr(0, 255), rr(0, 255), (uint32_t)(rr(0, 63) | rr(0, 400) << 6))); gpu_gp0(xy(rr(1, 60), rr(1, 40)));
                } else vram_ops();
            }
            gpu_gp1(0x05000000 | (uint32_t)(((it & 1) + 1) << 10));
            const GpuDrawList *l = gpu_capture_frame();
            ref_apply(ra, l, 1); ref_apply(rb, l, 1);
            GpuDisplay d = {0, 0, 320, 240, 0, 0, 0, 1};
            int w1, h1, wn, hn;
            const uint8_t *i1 = ref_present(ra, &d, &w1, &h1), *in = ref_present(rb, &d, &wn, &hn);
            /* el fill en HQ usa 8 bits del color de 24: se excluye comparando solo con fills en 5 bits -> tolerancia 7 */
            int bad = 0;
            for (int y = 0; y < hn; y++)
                for (int x = 0; x < wn; x++)
                    for (int c = 0; c < 3; c++) {
                        int dd = abs((int)in[((size_t)y * wn + x) * 3 + c] - (int)i1[((size_t)(y / N) * w1 + x / N) * 3 + c]);
                        if (dd > 7) bad++;
                    }
            if (bad) { fprintf(stderr, "FALLO hires N=%d it=%d: %d muestras fuera de tolerancia\n", N, it, bad); hfails++; }
        }
        ref_destroy(ra); ref_destroy(rb);
    }
    printf("selftest hires (replicacion de sprites/fills/subidas/copias, N=2 ssaa2 y N=4): %d fallos\n", hfails);
    return fails || hfails;
}
