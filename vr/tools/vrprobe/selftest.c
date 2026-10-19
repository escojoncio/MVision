/* Autoprueba del rasterizador de cobertura y del escritor de PNG del probador (sin juego). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "probe_raster.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FALLO %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

static void head(float yaw_deg, float pitch_deg, float x, float y, float z, float m[16]) {
    float yw = yaw_deg * 0.01745329252f, pt = pitch_deg * 0.01745329252f, cy = cosf(yw), sy = sinf(yw), cp = cosf(pt), sp = sinf(pt);
    float c[16] = {cy, 0, -sy, 0, sy * sp, cp, cy * sp, 0, sy * cp, -sp, cy * cp, 0, x, y, z, 1};
    memcpy(m, c, sizeof c);
}
static int count(const PrBuf *b) { int n = 0; for (int i = 0; i < b->w * b->h; i++) n += b->z[i] < 1e29f; return n; }

int main(void) {
    PrCam cam = {64, 64, tanf(50.0f * 0.01745329252f), tanf(50.0f * 0.01745329252f), 0.1f};
    PrBuf b;
    pr_buf_init(&b, 64, 64);
    float m[16];
    PrEye e;
    head(0, 0, 0, 0, 0, m);
    pr_eye_from_head(m, -1, 0.0f, &e);
    float px, py, d;
    CHECK(pr_project(&cam, &e, (float[]){0, 0, -2}, &px, &py, &d) && fabsf(px - 32) < 1e-3f && fabsf(py - 32) < 1e-3f && fabsf(d - 2) < 1e-4f, "centro %f %f %f", px, py, d);
    CHECK(!pr_project(&cam, &e, (float[]){0, 0, 2}, &px, &py, &d), "detrás proyectado");
    CHECK(pr_project(&cam, &e, (float[]){1, 0, -1}, &px, &py, &d) && px > 32, "derecha: %f", px);
    CHECK(pr_project(&cam, &e, (float[]){0, 1, -1}, &px, &py, &d) && py < 32, "arriba: %f", py);
    /* triángulo grande delante: cubre el centro */
    float t1[9] = {-5, -5, -3, 5, -5, -3, 0, 5, -3};
    CHECK(pr_tri(&b, &cam, &e, t1) > 0 && b.z[32 * 64 + 32] < 1e29f && fabsf(b.z[32 * 64 + 32] - 3) < 1e-3f, "triángulo delante: %d px, z %f", count(&b), b.z[32 * 64 + 32]);
    /* mismo triángulo con el orden de vértices invertido (doble cara): misma cobertura */
    int n1 = count(&b);
    pr_buf_clear(&b);
    float t1r[9] = {0, 5, -3, 5, -5, -3, -5, -5, -3};
    pr_tri(&b, &cam, &e, t1r);
    CHECK(count(&b) == n1, "doble cara: %d vs %d", count(&b), n1);
    /* detrás: nada */
    pr_buf_clear(&b);
    float t2[9] = {-5, -5, 3, 5, -5, 3, 0, 5, 3};
    CHECK(pr_tri(&b, &cam, &e, t2) == 0 && count(&b) == 0, "triángulo detrás dibujado");
    /* fuera del campo (lado): nada */
    float t3[9] = {20, -1, -2, 22, -1, -2, 21, 1, -2};
    CHECK(pr_tri(&b, &cam, &e, t3) == 0, "triángulo fuera del campo dibujado");
    /* suelo que atraviesa el plano cercano: recortado y visible en la parte baja */
    pr_buf_clear(&b);
    float t4[9] = {-10, -1, 5, 10, -1, 5, 0, -1, -30};
    CHECK(pr_tri(&b, &cam, &e, t4) > 0, "suelo recortado ausente");
    int low = 0, high = 0;
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) if (b.z[y * 64 + x] < 1e29f) { if (y >= 32) low++; else high++; }
    CHECK(low > 0 && high == 0, "suelo debe quedar bajo el horizonte: %d / %d", low, high);
    /* cabeza girada 90° a la izquierda (guiñada +90): el eje de mirada pasa a -X */
    pr_buf_clear(&b);
    head(90, 0, 0, 0, 0, m);
    pr_eye_from_head(m, -1, 0.0f, &e);
    CHECK(pr_project(&cam, &e, (float[]){-2, 0, 0}, &px, &py, &d) && fabsf(px - 32) < 1e-2f, "guiñada +90 no mira a -X (px %f)", px);
    /* cabeceo +90: mira a +Y */
    head(0, 90, 0, 0, 0, m);
    pr_eye_from_head(m, -1, 0.0f, &e);
    CHECK(pr_project(&cam, &e, (float[]){0, 2, 0}, &px, &py, &d) && fabsf(px - 32) < 1e-2f && fabsf(py - 32) < 1e-2f, "cabeceo +90 no mira a +Y (%f %f)", px, py);
    /* ojos: separación IPD */
    head(0, 0, 0, 1.6f, 0, m);
    PrEye el, er;
    pr_eye_from_head(m, -1, 0.064f, &el); pr_eye_from_head(m, 1, 0.064f, &er);
    CHECK(fabsf(er.p[0] - el.p[0] - 0.064f) < 1e-5f && fabsf(el.p[1] - 1.6f) < 1e-6f, "IPD %f", er.p[0] - el.p[0]);
    /* triángulo diminuto (< 1 píxel) lejano: marca un píxel */
    pr_buf_clear(&b);
    head(0, 0, 0, 0, 0, m);
    pr_eye_from_head(m, -1, 0.0f, &e);
    float t5[9] = {0, 0, -50, 0.01f, 0, -50, 0, 0.01f, -50};
    CHECK(pr_tri(&b, &cam, &e, t5) > 0 && count(&b) == 1, "triángulo diminuto: %d px", count(&b));
    pr_buf_free(&b);
    /* PNG */
    uint8_t rgb[8 * 8 * 3];
    for (int i = 0; i < 8 * 8; i++) { rgb[3 * i] = (uint8_t)(i * 4); rgb[3 * i + 1] = (uint8_t)(255 - i * 4); rgb[3 * i + 2] = 7; }
    CHECK(pr_write_png("selftest.png", 8, 8, rgb) == 0, "no se escribió el PNG");
    FILE *f = fopen("selftest.png", "rb");
    uint8_t sig[8] = {0};
    CHECK(f && fread(sig, 1, 8, f) == 8 && sig[1] == 'P' && sig[2] == 'N' && sig[3] == 'G', "firma PNG");
    if (f) fclose(f);
    fprintf(stderr, fails ? "selftest: %d FALLOS\n" : "selftest: OK\n", fails);
    return fails ? 1 : 0;
}
