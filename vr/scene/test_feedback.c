/* Prueba sintética de vr_feedback (efectos de realimentación de pantalla) con las primitivas medidas en volcados del visor (blurpure.c, blur.c, focus.c). make -C vr/scene test */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_feedback.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FALLO %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define NEAR(a, b, e) (fabs((double)(a) - (double)(b)) <= (e))

static GpuOp ops[16];
static uint16_t *vram;

/* búfer de dibujo en (bx, 0) de 320x256; centro de proyección (bx + 160, 128), como la versión PAL del juego */
static void clip(GpuOp *o, int bx) { o->cx1 = (int16_t)bx; o->cy1 = 0; o->cx2 = (int16_t)(bx + 319); o->cy2 = 255; }
static GpuOp tri3d(int bx) {
    GpuOp o; memset(&o, 0, sizeof o);
    o.type = GPU_OP_POLY; o.nv = 3; o.flags = GPU_F_TEXTURED; o.tp = 0; o.tx = 640; o.ty = 0; clip(&o, bx);
    o.proj_h = 320.0f; o.dox = (int16_t)(bx + 160); o.doy = 128;
    return o;
}
static GpuOp sprite(int bx, int x, int y, int w, int h, int tx, int u, int v, unsigned flags, int abr, int col) {
    GpuOp o; memset(&o, 0, sizeof o);
    o.type = GPU_OP_RECT; o.nv = 1; o.flags = (uint16_t)(GPU_F_TEXTURED | flags); o.abr = (uint8_t)abr; o.tp = 2; o.tx = (uint16_t)tx; o.ty = 0; clip(&o, bx);
    o.v[0].x = (int16_t)x; o.v[0].y = (int16_t)y; o.v[0].u = (uint8_t)u; o.v[0].v = (uint8_t)v; o.v[0].r = o.v[0].g = o.v[0].b = (uint8_t)col;
    o.rw = (int16_t)w; o.rh = (int16_t)h;
    return o;
}
static GpuOp quad(int bx, int tx, const int xy[4][2], const int uv[4][2]) {
    GpuOp o; memset(&o, 0, sizeof o);
    o.type = GPU_OP_POLY; o.nv = 4; o.flags = GPU_F_TEXTURED | GPU_F_SEMI | GPU_F_QUAD; o.abr = 0; o.tp = 2; o.tx = (uint16_t)tx; o.ty = 0; clip(&o, bx);
    for (int k = 0; k < 4; k++) { o.v[k].x = (int16_t)xy[k][0]; o.v[k].y = (int16_t)xy[k][1]; o.v[k].u = (uint8_t)uv[k][0]; o.v[k].v = (uint8_t)uv[k][1]; o.v[k].r = o.v[k].g = o.v[k].b = 128; }
    return o;
}
static void mask(int x0, int w, int on) { for (int y = 0; y < 256; y++) for (int x = x0; x < x0 + w; x++) vram[y * 1024 + x] = on ? 0x8000u | 0x1234u : 0x1234u; }

int main(void) {
    vram = calloc(1024 * 512, sizeof *vram);
    VrFeedback f;
    int org[2];

    /* ---- blurpure.c (d0014): 2 sprites de 160x184 con el OTRO búfer, (B+F)/2, color 120, todo con bit de máscara ---- */
    memset(ops, 0, sizeof ops);
    ops[0] = tri3d(320);
    ops[1] = sprite(320, 320, 27, 160, 184, 0, 0, 27, GPU_F_SEMI | GPU_F_MASK_SET, 0, 120);
    ops[2] = sprite(320, 480, 27, 160, 184, 128, 32, 27, GPU_F_SEMI | GPU_F_MASK_SET, 0, 120);
    mask(0, 320, 1);
    CHECK(vrfb_op_source(&ops[0], NULL, NULL) == VRFB_NONE, "una textura normal no es realimentación");
    CHECK(vrfb_op_source(&ops[1], NULL, org) == VRFB_PREV && org[0] == 0 && org[1] == 0, "sprite con el otro búfer: fotograma anterior (origen %d,%d)", org[0], org[1]);
    CHECK(vrfb_build(ops, 3, NULL, NULL, vram, &f) == VRFB_PREV && f.n == 2, "blurpure: fotograma anterior, 2 primitivas (modo %d n %d)", f.mode, f.n);
    CHECK(NEAR(f.a, 0.5, 1e-6) && NEAR(f.b[0], 0.5 * 120 / 128, 1e-6) && NEAR(f.b[2], 0.46875, 1e-6), "blurpure: (B+F)/2 con color 120/128 (a %f b %f)", f.a, f.b[0]);
    CHECK(NEAR(f.m[0], 1, 1e-4) && NEAR(f.m[1], 0, 1e-4) && NEAR(f.m[2], 0, 1e-4) && NEAR(f.m[3], 0, 1e-4) && NEAR(f.m[4], 1, 1e-4) && NEAR(f.m[5], 0, 1e-4) && f.err_px < 0.01f, "blurpure: sin deformar (err %f)", f.err_px);
    CHECK(NEAR(f.rect[0], 0, 1e-5) && NEAR(f.rect[2], 1, 1e-5) && NEAR(f.rect[1], 0.5 + (27 - 128) / 240.0, 1e-5) && NEAR(f.rect[3], 0.5 + (211 - 128) / 240.0, 1e-5), "blurpure: zona = filas 27..211 (%f..%f)", f.rect[1], f.rect[3]);
    /* la fuente sin bit de máscara: el sprite semitransparente pinta opaco */
    mask(0, 320, 0);
    CHECK(vrfb_build(ops, 3, NULL, NULL, vram, &f) == VRFB_PREV && f.a == 0.0f && NEAR(f.b[1], 120 / 128.0, 1e-6), "sin bit de máscara: opaco (a %f b %f)", f.a, f.b[1]);
    /* sin VRAM se da por mezclado */
    CHECK(vrfb_build(ops, 3, NULL, NULL, NULL, &f) == VRFB_PREV && NEAR(f.a, 0.5, 1e-6), "sin VRAM: mezclado");
    /* búfer de dibujo en x = 0 (d0010): la fuente es el de x = 320 */
    ops[0] = tri3d(0);
    ops[1] = sprite(0, 0, 27, 160, 184, 320, 0, 27, GPU_F_SEMI | GPU_F_MASK_SET, 0, 120);
    ops[2] = sprite(0, 160, 27, 160, 184, 448, 32, 27, GPU_F_SEMI | GPU_F_MASK_SET, 0, 120);
    mask(320, 320, 1);
    CHECK(vrfb_op_source(&ops[2], NULL, org) == VRFB_PREV && org[0] == 320, "el otro búfer a la derecha (origen %d)", org[0]);
    CHECK(vrfb_build(ops, 3, NULL, NULL, vram, &f) == VRFB_PREV && NEAR(f.m[0], 1, 1e-4) && NEAR(f.m[2], 0, 1e-4) && NEAR(f.a, 0.5, 1e-6), "blurpure con los búferes al revés: mismo modelo");
    /* otros modos de mezcla */
    ops[1].abr = ops[2].abr = 1; CHECK(vrfb_build(ops, 3, NULL, NULL, vram, &f) && f.a == 1.0f && NEAR(f.b[0], 120 / 128.0, 1e-6), "B+F");
    ops[1].abr = ops[2].abr = 2; CHECK(vrfb_build(ops, 3, NULL, NULL, vram, &f) && f.a == 1.0f && NEAR(f.b[0], -120 / 128.0, 1e-6), "B-F");
    ops[1].abr = ops[2].abr = 3; CHECK(vrfb_build(ops, 3, NULL, NULL, vram, &f) && f.a == 1.0f && NEAR(f.b[0], 0.25 * 120 / 128.0, 1e-6), "B+F/4");
    ops[1].abr = 0; CHECK(vrfb_build(ops, 3, NULL, NULL, vram, &f) == VRFB_NONE && f.n == 2, "mezclas distintas en la misma lista: sin modelo (siguen en el panel)");
    ops[2].abr = 0; ops[2].v[0].g = 90; CHECK(vrfb_build(ops, 3, NULL, NULL, vram, &f) == VRFB_NONE, "colores distintos: sin modelo");
    ops[2].v[0].g = 120;
    /* máscara de candidatas: solo se consideran las marcadas */
    { uint8_t cand[3] = {0, 1, 0}; CHECK(vrfb_build(ops, 3, cand, NULL, vram, &f) == VRFB_PREV && f.n == 1 && NEAR(f.rect[2], 0.5, 1e-5), "una sola candidata: media imagen (n %d, u1 %f)", f.n, f.rect[2]); }
    { uint8_t cand[3] = {0, 0, 0}; CHECK(vrfb_build(ops, 3, cand, NULL, vram, &f) == VRFB_NONE && f.n == 0, "sin candidatas"); }

    /* centro de proyección en los registros de la GTE (OFX = 160, OFY = 120) y el origen de la pantalla en el desplazamiento de dibujo: misma imagen, mismo modelo */
    {
        GpuOp t = tri3d(0); t.dox = 0; t.doy = 8; t.proj_ox = 160.0f; t.proj_oy = 120.0f;      /* pantalla del motor en (0, 8) del búfer: su centro, el píxel (160, 128) */
        ops[0] = t;
        CHECK(vrfb_build(ops, 3, NULL, NULL, vram, &f) == VRFB_PREV && NEAR(f.rect[0], 0, 1e-5) && NEAR(f.rect[2], 1, 1e-5) && NEAR(f.rect[1], 0.5 + (27 - 128) / 240.0, 1e-5), "OFX/OFY en la GTE: misma zona (%f..%f, v0 %f)", f.rect[0], f.rect[2], f.rect[1]);
        t.proj_ox = 100.0f; ops[0] = t;                                                           /* centro de proyección descentrado: la ventana sigue siendo [0,320] de la pantalla */
        CHECK(vrfb_build(ops, 3, NULL, NULL, vram, &f) == VRFB_PREV && NEAR(f.rect[0], 0, 1e-5) && NEAR(f.rect[2], 1, 1e-5), "OFX descentrado: la zona no se desplaza (%f..%f)", f.rect[0], f.rect[2]);
        ops[0] = tri3d(0);
    }

    /* ---- blur.c (d0012): rejilla 2x2 con el otro búfer, fuente algo menor que el destino y el vértice central desplazado ---- */
    memset(ops, 0, sizeof ops);
    ops[0] = tri3d(320);
    { const int xy[4][2] = {{476, 128}, {640, 128}, {475, 211}, {640, 211}}, uv[4][2] = {{35, 128}, {189, 128}, {35, 208}, {189, 208}}; ops[1] = quad(320, 128, xy, uv); }
    { const int xy[4][2] = {{320, 126}, {476, 128}, {320, 211}, {475, 211}}, uv[4][2] = {{2, 128}, {162, 128}, {2, 208}, {162, 208}}; ops[2] = quad(320, 0, xy, uv); }
    { const int xy[4][2] = {{478, 27}, {640, 27}, {476, 128}, {640, 128}}, uv[4][2] = {{35, 29}, {189, 29}, {35, 127}, {189, 127}}; ops[3] = quad(320, 128, xy, uv); }
    { const int xy[4][2] = {{320, 27}, {478, 27}, {320, 126}, {476, 128}}, uv[4][2] = {{2, 29}, {162, 29}, {2, 127}, {162, 127}}; ops[4] = quad(320, 0, xy, uv); }
    mask(0, 320, 1);
    CHECK(vrfb_build(ops, 5, NULL, NULL, vram, &f) == VRFB_PREV && f.n == 4, "blur.c: fotograma anterior, 4 polígonos");
    CHECK(f.m[0] > 0.97f && f.m[0] < 0.995f && f.m[4] > 0.96f && f.m[4] < 0.99f && fabsf(f.m[1]) < 0.02f && fabsf(f.m[3]) < 0.02f, "blur.c: la fuente es menor que el destino (ampliación): %f %f", f.m[0], f.m[4]);
    CHECK(f.err_px > 1.0f && f.err_px < VRFB_MAX_ERR_PX && NEAR(f.a, 0.5, 1e-6) && NEAR(f.b[0], 0.5, 1e-6), "blur.c: la deformación cabe en el ajuste (err %f px)", f.err_px);
    /* el centro de la imagen (160, 119 px del búfer) se muestrea cerca de sí mismo */
    { float u = 0.5f, v = 0.5f + (119 - 128) / 240.0f, qu = f.m[0] * u + f.m[1] * v + f.m[2], qv = f.m[3] * u + f.m[4] * v + f.m[5];
      CHECK(fabsf(qu - u) * 320 < 8 && fabsf(qv - v) * 240 < 8, "blur.c: el centro apenas se mueve (%f, %f px)", (qu - u) * 320, (qv - v) * 240); }
    /* una rejilla rota (un vértice a 60 px de su sitio) no cabe en el modelo */
    ops[1].v[0].x = 420; CHECK(vrfb_build(ops, 5, NULL, NULL, vram, &f) == VRFB_NONE && f.err_px > VRFB_MAX_ERR_PX, "deformación grande: sin modelo (err %f)", f.err_px);

    /* ---- focus.c (d0015): 2 sprites de 160x256 con el MISMO búfer, 2 px a la izquierda; la fuente casi no tiene bit de máscara ---- */
    memset(ops, 0, sizeof ops);
    ops[0] = tri3d(320);
    ops[1] = sprite(320, 318, 0, 160, 256, 320, 0, 0, GPU_F_SEMI, 0, 128);
    ops[2] = sprite(320, 478, 0, 160, 256, 448, 32, 0, GPU_F_SEMI, 0, 128);
    mask(320, 320, 0);
    CHECK(vrfb_op_source(&ops[1], NULL, org) == VRFB_SELF && org[0] == 320, "sprite con el búfer que se dibuja: mismo fotograma");
    CHECK(vrfb_build(ops, 3, NULL, NULL, vram, &f) == VRFB_SELF && f.n == 2 && f.a == 0.0f && f.b[0] == 1.0f, "focus: mismo fotograma, opaco (modo %d a %f)", f.mode, f.a);
    CHECK(NEAR(f.m[0], 1, 1e-4) && NEAR(f.m[2], 2 / 320.0, 1e-5) && NEAR(f.m[4], 1, 1e-4) && NEAR(f.m[5], 0, 1e-5), "focus: fuente 2 px a la derecha del destino (%f)", f.m[2] * 320);
    mask(320, 320, 1);
    CHECK(vrfb_build(ops, 3, NULL, NULL, vram, &f) == VRFB_SELF && NEAR(f.a, 0.5, 1e-6), "focus con bit de máscara: imagen doble al 50 %%");

    /* ---- lo que no es este efecto ---- */
    { GpuOp o = sprite(320, 400, 100, 32, 32, 0, 80, 100, GPU_F_SEMI, 0, 128);     /* sprite pequeño que muestrea la pantalla */
      CHECK(vrfb_op_source(&o, NULL, NULL) == VRFB_PREV, "un sprite pequeño también muestrea el búfer");
      memset(ops, 0, sizeof ops); ops[0] = tri3d(320); ops[1] = o;
      CHECK(vrfb_build(ops, 2, NULL, NULL, vram, &f) == VRFB_NONE && f.n == 1, "pero no cubre la imagen: sin modelo"); }
    { const int xy[4][2] = {{391, 122}, {397, 122}, {391, 128}, {397, 128}}, uv[4][2] = {{73, 122}, {73, 122}, {73, 122}, {73, 122}};   /* celda del mosaico: un solo texel */
      GpuOp o = quad(320, 320, xy, uv); CHECK(vrfb_op_source(&o, NULL, NULL) == VRFB_NONE, "celda de un solo texel (mosaico): no"); }
    { GpuOp o = sprite(320, 320, 27, 160, 184, 0, 0, 27, GPU_F_SEMI, 0, 120); o.fbk = 1; CHECK(vrfb_op_source(&o, NULL, NULL) == VRFB_NONE, "resuelta por la captura: no"); }
    { GpuOp o = sprite(320, 320, 27, 160, 184, 0, 0, 27, GPU_F_SEMI, 0, 120); o.tp = 0; CHECK(vrfb_op_source(&o, NULL, NULL) == VRFB_NONE, "textura con paleta: no"); }
    { GpuOp o = sprite(320, 320, 27, 160, 184, 640, 0, 27, GPU_F_SEMI, 0, 120); CHECK(vrfb_op_source(&o, NULL, NULL) == VRFB_NONE, "fuente fuera de los búferes: no"); }
    { GpuOp o = sprite(320, 320, 27, 160, 184, 256, 0, 27, GPU_F_SEMI, 0, 120); CHECK(vrfb_op_source(&o, NULL, NULL) == VRFB_NONE, "fuente a caballo entre los dos búferes: no"); }
    { GpuOp o = sprite(320, 320, 27, 160, 184, 0, 0, 27, 0, 0, 120); o.flags = GPU_F_SEMI; CHECK(vrfb_op_source(&o, NULL, NULL) == VRFB_NONE, "sin textura: no"); }
    /* el otro búfer dicho por el anfitrión (área mostrada al cerrar la lista anterior) manda sobre la deducción */
    { GpuOp o = sprite(320, 320, 27, 160, 184, 640, 0, 27, GPU_F_SEMI, 0, 120); const int other[4] = {640, 0, 320, 256}, same[4] = {320, 0, 320, 256}, none[4] = {0, 0, 0, 0};
      CHECK(vrfb_op_source(&o, other, org) == VRFB_PREV && org[0] == 640, "otro búfer dado: fuente en él");
      CHECK(vrfb_op_source(&o, same, NULL) == VRFB_NONE && vrfb_op_source(&o, none, NULL) == VRFB_NONE, "otro búfer no válido (solapa el de dibujo o vacío): se deduce, y 640 no es la pareja de 320");
      GpuOp p = sprite(320, 320, 27, 160, 184, 0, 0, 27, GPU_F_SEMI, 0, 120);
      CHECK(vrfb_op_source(&p, other, NULL) == VRFB_NONE, "con el otro búfer en 640, el de x = 0 no es fuente de realimentación"); }
    /* búferes apilados en vertical (0,0) y (0,256) */
    { GpuOp o = sprite(0, 0, 27, 160, 184, 0, 0, 27, GPU_F_SEMI, 0, 120); o.ty = 256; CHECK(vrfb_op_source(&o, NULL, org) == VRFB_PREV && org[1] == 256, "pareja vertical"); }
    CHECK(vrfb_op_source(NULL, NULL, NULL) == VRFB_NONE && vrfb_build(NULL, 0, NULL, NULL, NULL, &f) == VRFB_NONE && f.n == 0, "entradas vacías");

    /* ---- ondulación del agua de la intro (wt_view.c, volcados auto01/auto02 de Archivo_33): tiras de 160 x 2 que releen el MISMO búfer con la fila de origen v = y + rsin(i * 128) / 1024 ---- */
    {
        static GpuOp w[300];
        memset(w, 0, sizeof w);
        w[0] = tri3d(320);
        int n = 1;
        for (int i = 0, y = 6; y < 218; y += 2, i++) {
            const int off = (int)(4096.0 * sin(i * 128 * 2.0 * 3.14159265358979 / 4096.0)) / 1024;
            w[n++] = sprite(320, 320, y, 160, 2, 320, 0, y + off, 0, 0, 82);
            w[n++] = sprite(320, 480, y, 160, 2, 448, 32, y + off, 0, 0, 82);
        }
        mask(320, 320, 0);
        CHECK(vrfb_op_source(&w[1], NULL, org) == VRFB_SELF && vrfb_is_strip(&w[1]) && !vrfb_is_strip(&w[0]), "tira de la onda: mismo búfer");
        CHECK(vrfb_build(w, (uint32_t)n, NULL, NULL, vram, &f) == VRFB_SELF && f.n == n - 1, "onda: modelo SELF con todas las tiras (modo %d n %d)", f.mode, f.n);
        const float amp = sqrtf(f.wave[0] * f.wave[0] + f.wave[1] * f.wave[1]) * VRFB_IMG_H;
        CHECK(amp > 3.0f && amp < 4.5f && f.err_px <= VRFB_WAVE_MAX_ERR_PX, "onda: amplitud ~4 filas y residuo corto (amp %f, err %f)", amp, f.err_px);
        CHECK(NEAR(f.m[0], 1, 1e-3) && NEAR(f.m[4], 1, 1e-2) && f.a == 0.0f && NEAR(f.b[0], 82 / 128.0, 1e-6), "onda: afín casi identidad, opaco, tinte 82/128 (a %f b %f)", f.a, f.b[0]);
        /* con una tira de 1 fila (la fuente a 1 fila de distancia): también vale; un sprite ancho de 1 fila no se descarta, uno diminuto sí */
        { GpuOp t = sprite(320, 320, 20, 160, 1, 320, 0, 19, 0, 0, 82); CHECK(vrfb_op_source(&t, NULL, NULL) == VRFB_SELF && vrfb_is_strip(&t), "tira de 1 fila: SELF");
          GpuOp u = sprite(320, 320, 20, 8, 1, 320, 0, 19, 0, 0, 82); CHECK(vrfb_op_source(&u, NULL, NULL) == VRFB_NONE && !vrfb_is_strip(&u), "sprite diminuto de 1 fila: no"); }
        /* la afín sola (focus) no lleva onda */
        CHECK(f.wave[0] != 0.0f || f.wave[1] != 0.0f, "onda ajustada");
        /* con otra realimentación en la misma lista, sin restringir a las tiras no hay modelo (el anfitrión las separa con vrfb_is_strip) */
        w[n] = sprite(320, 320, 27, 160, 184, 0, 0, 27, GPU_F_SEMI | GPU_F_MASK_SET, 0, 120);
        CHECK(vrfb_build(w, (uint32_t)n + 1, NULL, NULL, vram, &f) == VRFB_NONE, "tiras + desenfoque sin separar: sin modelo");
        static uint8_t mk[300];
        for (int i = 0; i <= n; i++) mk[i] = vrfb_is_strip(&w[i]);
        CHECK(vrfb_build(w, (uint32_t)n + 1, mk, NULL, vram, &f) == VRFB_SELF && f.n == n - 1, "tiras separadas del desenfoque: modelo de la onda (n %d)", f.n);
        /* un desplazamiento constante (focus con tiras) no es onda: afín exacta, sin ondulación */
        memset(w, 0, sizeof w); w[0] = tri3d(320); n = 1;
        for (int y = 6; y < 218; y += 2) w[n++] = sprite(320, 320, y, 160, 2, 320, 2, y, 0, 0, 128);
        CHECK(vrfb_build(w, (uint32_t)n, NULL, NULL, vram, &f) == VRFB_SELF && f.wave[0] == 0.0f && f.wave[1] == 0.0f && NEAR(f.m[2], 2 / 320.0, 1e-5), "tiras sin onda: solo desplazamiento");
    }

    free(vram);
    printf("test_feedback: %d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
