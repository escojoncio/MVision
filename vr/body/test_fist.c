/* Pruebas de vr_fist con una malla de mano sintética (sin datos del juego). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_fist.h"
#include "test_fist_mesh.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: %s ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static VrFistMesh as_mesh(const SynthHand *h) { VrFistMesh m = {h->nv, h->nn, h->nf, h->v, h->n, h->vidx, h->nidx}; return m; }

static void centroid(const int16_t *v, int a, int b, float c[3]) { c[0] = c[1] = c[2] = 0; for (int i = a; i < b; i++) for (int k = 0; k < 3; k++) c[k] += v[4 * i + k]; for (int k = 0; k < 3; k++) c[k] /= (float)(b - a); }

static void test_basic(void) {
    VrFistParams p; vrfist_params_default(&p);
    for (int mirror = 0; mirror < 2; mirror++) {
        SynthHand h; synth_hand(&h, mirror); VrFistMesh m = as_mesh(&h);
        float sg = mirror ? -1.0f : 1.0f;
        int16_t v[128 * 4], n[128 * 4];
        /* grip 0: idéntica */
        CHECK(vrfist_deform(&m, sg, 0.0f, &p, v, n) == 0, "grip 0");
        CHECK(memcmp(v, h.v, (size_t)h.nv * 8) == 0 && memcmp(n, h.n, (size_t)h.nn * 8) == 0, "grip 0 debe devolver la malla original");
        /* grip 1 */
        CHECK(vrfist_deform(&m, sg, 1.0f, &p, v, n) == 0, "grip 1");
        for (int i = 0; i < h.nv; i++) CHECK(v[4 * i + 3] == h.v[4 * i + 3], "el relleno se conserva");
        /* muñeca y palma (anillos 0 y 1) no se mueven */
        for (int i = 0; i < 8; i++) CHECK(v[4 * i] == h.v[4 * i] && v[4 * i + 1] == h.v[4 * i + 1] && v[4 * i + 2] == h.v[4 * i + 2], "vértice de palma %d movido", i);
        /* las puntas (anillo 3) se acercan a la palma y a la muñeca: x * signo sube y la y sube; z se conserva */
        float c0[3], c1[3];
        centroid(h.v, 12, 16, c0); centroid(v, 12, 16, c1);
        CHECK(sg * c1[0] > sg * c0[0] - 60 && c1[1] > c0[1] + 30, "punta: (%.1f,%.1f) -> (%.1f,%.1f)", c0[0], c0[1], c1[0], c1[1]);
        /* los dedos cerrados quedan por delante de la palma (lado +sg) y no por detrás del dorso */
        float lo = 1e9f;
        for (int i = 8; i < 16; i++) { float xs = sg * v[4 * i]; if (xs < lo) lo = xs; }
        CHECK(lo > -30, "dedos por detrás del dorso: %.1f", lo);
        for (int i = 8; i < 16; i++) CHECK(v[4 * i + 2] == h.v[4 * i + 2], "z de los dedos se conserva");
        /* pulgar: se acerca a la hilera de dedos (hacia -Z) sin pasar del centro de la hilera (nunca cruza hacia el meñique) */
        float t0[3], t1[3]; centroid(h.v, 16, 24, t0); centroid(v, 16, 24, t1);
        CHECK(t1[2] < t0[2] - 1.5f && t1[2] > -5, "el pulgar debe acercarse a los dedos sin cruzarlos: %.1f -> %.1f", t0[2], t1[2]);
        /* normales: módulo conservado y giran donde se mueve el vértice */
        for (int j = 0; j < h.nn; j++) {
            float a = sqrtf((float)h.n[4 * j] * h.n[4 * j] + (float)h.n[4 * j + 1] * h.n[4 * j + 1] + (float)h.n[4 * j + 2] * h.n[4 * j + 2]);
            float b = sqrtf((float)n[4 * j] * n[4 * j] + (float)n[4 * j + 1] * n[4 * j + 1] + (float)n[4 * j + 2] * n[4 * j + 2]);
            CHECK(fabsf(a - b) <= 0.01f * a + 3, "módulo de la normal %d: %.1f -> %.1f", j, a, b);
        }
        int turned = 0; for (int j = 0; j < h.nn; j++) turned += n[4 * j] != h.n[4 * j] || n[4 * j + 1] != h.n[4 * j + 1] || n[4 * j + 2] != h.n[4 * j + 2];
        CHECK(turned >= 20, "normales giradas: %d", turned);
        /* monótono: las puntas suben con el agarre */
        float prev = -1e9f;
        for (int k = 0; k <= 10; k++) {
            int16_t v2[128 * 4], n2[128 * 4];
            CHECK(vrfist_deform(&m, sg, k / 10.0f, &p, v2, n2) == 0, "grip %d", k);
            float c[3]; centroid(v2, 12, 16, c);
            CHECK(c[1] >= prev - 0.6f, "punta no monótona en k=%d: %.1f < %.1f", k, c[1], prev);
            prev = c[1];
        }
    }
    /* espejo: la malla izquierda deformada es el reflejo exacto de la derecha (±1) */
    {
        SynthHand a, b; synth_hand(&a, 0); synth_hand(&b, 1);
        VrFistMesh ma = as_mesh(&a), mb = as_mesh(&b);
        int16_t va[128 * 4], na[128 * 4], vb[128 * 4], nb[128 * 4];
        vrfist_deform(&ma, 1.0f, 0.8f, &p, va, na); vrfist_deform(&mb, -1.0f, 0.8f, &p, vb, nb);
        int worst = 0;
        for (int i = 0; i < a.nv; i++) { int dx = abs(va[4 * i] + vb[4 * i]), dy = abs(va[4 * i + 1] - vb[4 * i + 1]), dz = abs(va[4 * i + 2] - vb[4 * i + 2]); if (dx > worst) worst = dx; if (dy > worst) worst = dy; if (dz > worst) worst = dz; }
        CHECK(worst <= 1, "espejo: desviación %d", worst);
        worst = 0;
        for (int j = 0; j < a.nn; j++) { int dx = abs(na[4 * j] + nb[4 * j]), dy = abs(na[4 * j + 1] - nb[4 * j + 1]), dz = abs(na[4 * j + 2] - nb[4 * j + 2]); if (dx > worst) worst = dx; if (dy > worst) worst = dy; if (dz > worst) worst = dz; }
        CHECK(worst <= 2, "espejo de normales: desviación %d", worst);
    }
}


/* Pulgar anatómico: cadena de 2 articulaciones, termina por FUERA de los dedos doblados sobre las falanges medias del índice y el corazón */
static float dist3(const float *a, const float *b) { return sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }
static void test_thumb(void) {
    VrFistParams p; vrfist_params_default(&p);
    CHECK(fabsf(p.thumb_tilt_deg - 45.0f) < 1e-4f, "eje de la base inclinado 45 grados");
    for (int mirror = 0; mirror < 2; mirror++) {
        SynthHand h; synth_hand(&h, mirror); VrFistMesh m = as_mesh(&h);
        float sg = mirror ? -1.0f : 1.0f;
        int16_t v[128 * 4], n[128 * 4], v1[128 * 4], n1[128 * 4];
        CHECK(vrfist_deform(&m, sg, 1.0f, &p, v1, n1) == 0, "puño");
        /* finger row extents (z) y alcance en x/y de las falanges medias: anillo 2 y 3 */
        float zmax = -1e9f, zmin = 1e9f;
        for (int i = 8; i < 16; i++) { if (v1[4 * i + 2] > zmax) zmax = v1[4 * i + 2]; if (v1[4 * i + 2] < zmin) zmin = v1[4 * i + 2]; }
        float W = zmax - zmin, tipc[3], t0[3];
        centroid(v1, 20, 24, tipc); centroid(h.v, 20, 24, t0);
        float base1[3], base0[3]; centroid(v1, 16, 18, base1); centroid(h.v, 16, 18, base0);
        CHECK(tipc[2] <= zmax - 0.05f * W && tipc[2] >= zmax - 0.65f * W, "la punta queda sobre el índice y el corazón, no sobre el meñique: z=%.1f (dedos %.1f..%.1f)", tipc[2], zmin, zmax);
        CHECK(dist3(base1, base0) < 14.0f, "la base del pulgar casi no se mueve: %.1f", dist3(base1, base0));
        /* por fuera de la palma: ningún vértice del pulgar dentro del bloque de la palma (x*signo > xlo de la palma) salvo la base */
        for (int i = 16; i < 24; i++) CHECK(sg * v1[4 * i] >= 18, "pulgar dentro de la palma: x=%g", sg * v1[4 * i]);
        /* continuidad con el agarre: la punta avanza sin saltos (< 35 % del recorrido total por 0,1 de agarre) y el recorrido total es el de 0 a 1 */
        float prev[3] = {t0[0], t0[1], t0[2]}, path = 0, maxstep = 0;
        for (int k = 1; k <= 10; k++) {
            int16_t vk[128 * 4], nk[128 * 4]; vrfist_deform(&m, sg, k / 10.0f, &p, vk, nk);
            float c[3]; centroid(vk, 20, 24, c);
            float st = dist3(c, prev); path += st; if (st > maxstep) maxstep = st; prev[0] = c[0]; prev[1] = c[1]; prev[2] = c[2];
        }
        CHECK(maxstep <= 0.35f * path + 1.0f, "pulgar con saltos: paso máx %.1f de %.1f", maxstep, path);
        /* agarre parcial: el pulgar no se deforma de forma rara (aristas casi intactas) y se mueve proporcionalmente */
        for (int k = 1; k <= 9; k += 2) {
            vrfist_deform(&m, sg, k / 10.0f, &p, v, n);
            float worst = 0;
            for (int i = 16; i < 24; i++) for (int j = i + 1; j < 24; j++) {
                float a0[3] = {(float)h.v[4 * i], (float)h.v[4 * i + 1], (float)h.v[4 * i + 2]}, b0[3] = {(float)h.v[4 * j], (float)h.v[4 * j + 1], (float)h.v[4 * j + 2]};
                float a1[3] = {(float)v[4 * i], (float)v[4 * i + 1], (float)v[4 * i + 2]}, b1[3] = {(float)v[4 * j], (float)v[4 * j + 1], (float)v[4 * j + 2]};
                float e = fabsf(dist3(a1, b1) - dist3(a0, b0)) / (dist3(a0, b0) + 1e-3f);
                if (e > worst) worst = e;
            }
            CHECK(worst < 0.35f, "pulgar estirado/encogido %.2f con agarre %.1f", worst, k / 10.0f);
            float c[3]; centroid(v, 20, 24, c);
            CHECK(dist3(c, t0) <= dist3(tipc, t0) * (k / 10.0f) * 1.6f + 3.0f, "desplazamiento no proporcional con agarre %.1f", k / 10.0f);
        }
        /* a medias de camino el pulgar sigue entero y las normales conservan el módulo */
        for (int j = 0; j < h.nn; j++) {
            float a = sqrtf((float)h.n[4 * j] * h.n[4 * j] + (float)h.n[4 * j + 1] * h.n[4 * j + 1] + (float)h.n[4 * j + 2] * h.n[4 * j + 2]);
            float b = sqrtf((float)n1[4 * j] * n1[4 * j] + (float)n1[4 * j + 1] * n1[4 * j + 1] + (float)n1[4 * j + 2] * n1[4 * j + 2]);
            CHECK(fabsf(a - b) <= 0.01f * a + 3, "módulo de la normal %d en el puño", j);
        }
        /* ejes anatómicos: con el eje de la base a 0 grados (igual que los dedos) o a 90 la pose cambia: el ángulo cuenta */
        VrFistParams q = p; q.thumb_tilt_deg = 0.0f;
        int16_t vq[128 * 4], nq[128 * 4]; vrfist_deform(&m, sg, 1.0f, &q, vq, nq);
        int diff = 0; for (int i = 16; i < 24; i++) diff += abs(vq[4 * i] - v1[4 * i]) + abs(vq[4 * i + 1] - v1[4 * i + 1]) + abs(vq[4 * i + 2] - v1[4 * i + 2]);
        CHECK(diff > 0, "la inclinación del eje de la base debe influir");
    }
}

static void test_invalid(void) {
    VrFistParams p; vrfist_params_default(&p);
    SynthHand h; synth_hand(&h, 0); VrFistMesh m = as_mesh(&h);
    int16_t v[128 * 4], n[128 * 4];
    CHECK(vrfist_deform(NULL, 1, 1, &p, v, n) < 0, "NULL");
    CHECK(vrfist_deform(&m, 1, NAN, &p, v, n) < 0, "NaN grip");
    CHECK(vrfist_deform(&m, NAN, 1, &p, v, n) < 0, "NaN lado");
    CHECK(vrfist_deform(&m, 1, 1, NULL, v, n) < 0, "sin parámetros");
    VrFistMesh bad = m; bad.nv = 4; CHECK(vrfist_deform(&bad, 1, 1, &p, v, n) < 0, "pocos vértices");
    bad = m; bad.nv = 500; CHECK(vrfist_deform(&bad, 1, 1, &p, v, n) < 0, "demasiados vértices");
    uint8_t vi[128 * 4]; memcpy(vi, h.vidx, sizeof vi); vi[5] = 100;
    bad = m; bad.vidx = vi; CHECK(vrfist_deform(&bad, 1, 1, &p, v, n) == -2, "índice de vértice fuera de rango");
    uint8_t ni[128 * 4]; memcpy(ni, h.nidx, sizeof ni); ni[7] = 120;
    bad = m; bad.nidx = ni; CHECK(vrfist_deform(&bad, 1, 1, &p, v, n) == -2, "índice de normal fuera de rango");
    /* grip fuera de 0..1 se acota */
    int16_t v1[128 * 4], n1[128 * 4], v2[128 * 4], n2[128 * 4];
    vrfist_deform(&m, 1, 1.0f, &p, v1, n1); vrfist_deform(&m, 1, 7.0f, &p, v2, n2);
    CHECK(memcmp(v1, v2, (size_t)h.nv * 8) == 0, "grip > 1 se acota");
    /* sin pulgar suelto: los dedos se doblan igualmente */
    { SynthHand g; synth_hand(&g, 0); g.nv = 16; g.nf = 13; /* sin la caja del pulgar */
      VrFistMesh mg = as_mesh(&g); int r = vrfist_deform(&mg, 1, 1, &p, v, n); CHECK(r == 0, "sin pulgar r=%d", r);
      float c0[3], c1[3]; centroid(g.v, 12, 16, c0); centroid(v, 12, 16, c1); CHECK(c1[1] > c0[1] + 30, "sin pulgar: los dedos se doblan"); }
    /* una malla que no es una mano (caja de 8 vértices): se rechaza o no revienta */
    { SynthHand g; memset(&g, 0, sizeof g); for (int i = 0; i < 8; i++) { g.v[4 * i] = (int16_t)((i & 1) ? 10 : -10); g.v[4 * i + 1] = (int16_t)((i & 2) ? 10 : -10); g.v[4 * i + 2] = (int16_t)((i & 4) ? 10 : -10); }
      static const int bx[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
      for (int f = 0; f < 6; f++) for (int k = 0; k < 4; k++) { g.vidx[4 * f + k] = (uint8_t)bx[f][k]; g.nidx[4 * f + k] = (uint8_t)(4 * f + k); }
      g.nv = 8; g.nn = 24; g.nf = 6; VrFistMesh mg = as_mesh(&g);
      int r = vrfist_deform(&mg, 1, 1, &p, v, n); CHECK(r < 0, "una caja no es una mano: r=%d", r); }
}

static uint64_t rs = 88172645463325252ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }
static void test_fuzz(void) {
    VrFistParams p; vrfist_params_default(&p);
    int ok = 0;
    for (int it = 0; it < 4000; it++) {
        SynthHand h; synth_hand(&h, it & 1);
        int mode = (int)(rnd() % 4);
        for (int k = 0; k < 6; k++) {                  /* mutaciones aleatorias de vértices, normales, índices y recuentos */
            int i = (int)(rnd() % 128);
            if (mode == 0) h.v[i * 4 + (int)(rnd() % 3)] = (int16_t)rnd();
            else if (mode == 1) h.vidx[rnd() % 512] = (uint8_t)rnd();
            else if (mode == 2) h.n[i * 4 + (int)(rnd() % 3)] = (int16_t)rnd();
            else { h.nv = (int)(rnd() % 140); h.nn = (int)(rnd() % 140); h.nf = (int)(rnd() % 140); }
        }
        VrFistMesh m = as_mesh(&h);
        int16_t v[128 * 4], n[128 * 4];
        memset(v, 0x55, sizeof v); memset(n, 0x55, sizeof n);
        int r = vrfist_deform(&m, (it & 2) ? 1.0f : -1.0f, (float)(rnd() % 1001) / 1000.0f, &p, v, n);
        if (r == 0) ok++;
    }
    CHECK(ok > 500, "fuzz: demasiadas mallas rechazadas (%d)", ok);
}

int main(void) {
    test_basic(); test_thumb(); test_invalid(); test_fuzz();
    if (fails) { printf("FALLO: %d de %d\n", fails, checks); return 1; }
    printf("OK vr_fist: %d comprobaciones\n", checks);
    return 0;
}
