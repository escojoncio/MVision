/* Onda del agua del sombreador (ns_fs) sobre el rasterizador de referencia (ns_ref_wave). Sintética, sin datos del juego.
 * Regla: la onda es un desplazamiento suave de lo ya dibujado; nunca mueve el muestreo más de 1 texel (a más, en una cara lejana mezcla texturas vecinas y deja basura). */
#include "ns_cpuref.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_ok;
#define CHECK(c, ...) do { if (c) g_ok++; else { g_fail++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { W = 64, H = 64 };

/* Cuadrado de 8 px de lado a distancia 100 (fx = 100): uv 8..24 en 8 px = 2 texels por píxel. Textura en degradado de 16 bits (página 0, directa): R5 = u. */
static float render(const NsRWave *wv, float *r5) {
    uint16_t *vram = calloc(1024 * 512, 2);
    for (int v = 0; v < 256; v++) for (int u = 0; u < 32; u++) vram[v * 1024 + u] = (uint16_t)((u & 31) | (1 << 5));
    NsRTri t[2]; memset(t, 0, sizeof t);
    const float x[4] = { -4, 4, -4, 4 }, y[4] = { -4, -4, 4, 4 }, uu[4] = { 8, 24, 8, 24 }, vv[4] = { 8, 8, 24, 24 };
    const int idx[2][3] = { { 0, 1, 2 }, { 1, 2, 3 } };
    for (int k = 0; k < 2; k++) {
        for (int j = 0; j < 3; j++) { NsRVert *p = &t[k].v[j]; int i = idx[k][j]; p->p[0] = x[i]; p->p[1] = y[i]; p->p[2] = 100; p->u = uu[i]; p->v = vv[i]; p->r = p->g = p->b = 128; }
        t[k].tpage = 2u << 7; t[k].flags = NS_TF_TEXTURED | NS_TF_RAW;
    }
    NsRScene s = { t, 2, vram };
    const float A[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, T[3] = { 0, 0, 0 };
    NsRCam c; ns_cam_game(&c, A, T, 100, 32, 32, W, H, W, H);
    NsRBuf b; ns_buf_alloc(&b, W, H); ns_buf_clear(&b, 0, 0, 0);
    ns_ref_wave = wv; ns_render_view(&s, &c, 0, &b, NULL); ns_ref_wave = NULL;
    int n = 0;
    for (int i = 0; i < W * H; i++) { r5[i] = b.iz[i] > 0 ? b.c[i * 3] : -1.0f; if (b.iz[i] > 0) n++; }
    ns_buf_free(&b); free(vram);
    return (float)n;
}

int main(void) {
    static float a[W * H], b[W * H];
    NsRWave w; memset(&w, 0, sizeof w);
    w.min[0] = w.min[1] = w.min[2] = -1e6f; w.max[0] = w.max[1] = w.max[2] = 1e6f; w.all = 1;
    w.amp = 4.0f; w.freq = 0.5f; w.phase = 0.7f;
    float n0 = render(NULL, a), n1 = render(&w, b);
    CHECK(n0 > 40 && n0 == n1, "la onda no cambia la cobertura (%g / %g)", n0, n1);
    float maxd = 0; int changed = 0;
    for (int i = 0; i < W * H; i++) if (a[i] >= 0 && b[i] >= 0) { float d = fabsf(a[i] - b[i]); if (d > maxd) maxd = d; if (d > 0) changed++; }
    CHECK(changed > 4, "la onda mueve algo (%d px)", changed);
    CHECK(maxd <= 1.0f, "desplazamiento del muestreo <= 1 texel (R5 = u): max %.1f", maxd);
    printf("test_ns_wave: %d ok, %d fallos\n", g_ok, g_fail);
    return g_fail != 0;
}
