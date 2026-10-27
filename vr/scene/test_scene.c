/* Prueba sintética de vr_scene (sin datos del juego). make -C vr/scene test / test-san */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_scene.h"
#include "hud_emu.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FALLO %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
static int near_f(float a, float b, float t) { return fabsf(a - b) <= t; }

static GpuOp ops[512];
static GpuDrawList dl;

static void reset(void) { memset(ops, 0, sizeof ops); memset(&dl, 0, sizeof dl); dl.ops = ops; }
static GpuOp *add(int type) { GpuOp *o = &ops[dl.nops++]; o->type = (uint8_t)type; return o; }
static void setv3(GpuVtx *v, float x, float y, float z) { v->fvalid = 1; v->vvalid = 1; v->vx = x; v->vy = y; v->vz = z; v->fz = z; }
static GpuOp *poly3(float a[3], float b[3], float c[3], int semi) {
    GpuOp *o = add(GPU_OP_POLY);
    o->nv = 3; o->flags = (uint16_t)(GPU_F_TEXTURED | (semi ? GPU_F_SEMI : 0)); o->abr = 1; o->tp = 1; o->tx = 320; o->ty = 256; o->clx = 48; o->cly = 500;
    setv3(&o->v[0], a[0], a[1], a[2]); setv3(&o->v[1], b[0], b[1], b[2]); setv3(&o->v[2], c[0], c[1], c[2]);
    for (int i = 0; i < 3; i++) { o->v[i].u = (uint8_t)(10 * i); o->v[i].v = (uint8_t)(5 * i); o->v[i].r = 128; o->v[i].g = 100; o->v[i].b = 60; }
    return o;
}

/* cámara del motor: ejes en columnas (der, abajo, adelante) en el mundo del juego (Y arriba) */
static void set_eye(BridgeState *bs, const float R[3][3], const int32_t e[3]) {
    memset(bs, 0, sizeof *bs);
    bs->valid = BRIDGE_OK_ALL_CORE;
    bs->has_player = 1;
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) bs->eye_axes[r][c] = (int16_t)lrintf(R[r][c] * 4096.0f);
    for (int i = 0; i < 3; i++) bs->eye_pos[i] = e[i];
    bs->eye_focal = 320;
    bs->player_pos = (BVec3){100, 1104, -200};
    bs->ctl_height = 1104;
}
/* vista de un punto del mundo con una cámara (R = ejes en columnas): v = R^T (P - e) */
static void to_view(const float R[3][3], const int32_t e[3], const float P[3], float v[3]) {
    for (int c = 0; c < 3; c++) { v[c] = 0; for (int r = 0; r < 3; r++) v[c] += R[r][c] * (P[r] - (float)e[r]); }
}

static void test_basic(void) {
    /* cámara mirando a -Z, derecha +X, abajo -Y (mundo Y arriba, mano derecha) */
    const float R[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    const int32_t e[3] = {0, 3000, 500};
    BridgeState bs; set_eye(&bs, R, e);
    reset();
    float a[3] = {0, 0, 2000}, b[3] = {1000, 0, 2000}, c[3] = {0, 1000, 2000}; /* en vista: 2 m delante */
    poly3(a, b, c, 0);
    VrSceneOpts o = vr_scene_opts_default();
    o.origin_mode = 1; o.origin_game[0] = 0; o.origin_game[1] = 0; o.origin_game[2] = 0;
    VrScene s;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0, "build");
    CHECK(s.has_3d && s.nv == 3 && s.nb == 1 && s.b[0].kind == 0, "nv=%u nb=%u", s.nv, s.nb);
    CHECK(s.world_valid, "world_valid");
    /* vista (0,0,2000) -> mundo del juego e + fwd*2000 = (0,3000,500-2000) = (0,3000,-1500) -> VR (0,3,-1.5) */
    int found = 0;
    for (uint32_t i = 0; i < s.nv; i++)
        if (near_f(s.v[i].pos[0], 0, 1e-4f) && near_f(s.v[i].pos[1], 3.0f, 1e-4f) && near_f(s.v[i].pos[2], -1.5f, 1e-4f)) found++;
    CHECK(found == 1, "vértice esperado (0,3,-1,5) no aparece (%d)", found);
    CHECK(near_f(s.eye_vr[0], 0, 1e-5f) && near_f(s.eye_vr[1], 3.0f, 1e-5f) && near_f(s.eye_vr[2], 0.5f, 1e-5f), "eye_vr %.3f %.3f %.3f", s.eye_vr[0], s.eye_vr[1], s.eye_vr[2]);
    /* game_view columna-mayor: punto de vista (0,0,2000) -> VR */
    float p[3];
    for (int r = 0; r < 3; r++) p[r] = s.game_view[8 + r] * 2000 + s.game_view[12 + r];
    CHECK(near_f(p[0], 0, 1e-4f) && near_f(p[1], 3.0f, 1e-4f) && near_f(p[2], -1.5f, 1e-4f), "game_view");
    /* UV, color, clut, tpage */
    CHECK(s.v[0].clut == (48 / 16 | (500 << 6)) && (s.v[0].tpage & 15) == 5 && ((s.v[0].tpage >> 4) & 1) == 1 && ((s.v[0].tpage >> 7) & 3) == 1 &&
          ((s.v[0].tpage >> 5) & 3) == 1, "clut/tpage %04x %04x", s.v[0].clut, s.v[0].tpage);
    CHECK((s.v[0].rgba & 255) == 128 && ((s.v[0].rgba >> 24) & 255) == 255, "rgba");
    CHECK(!(s.v[0].flags & VR_F_2D), "flags 3D");
    /* orientación: la normal mira a la cámara (producto escalar con eye-p0 > 0) */
    {
        const VrVtx *v = s.v;
        float e1[3], e2[3], n[3], t[3];
        for (int i = 0; i < 3; i++) { e1[i] = v[1].pos[i] - v[0].pos[i]; e2[i] = v[2].pos[i] - v[0].pos[i]; t[i] = s.eye_vr[i] - v[0].pos[i]; }
        n[0] = e1[1]*e2[2]-e1[2]*e2[1]; n[1] = e1[2]*e2[0]-e1[0]*e2[2]; n[2] = e1[0]*e2[1]-e1[1]*e2[0];
        CHECK(n[0]*t[0] + n[1]*t[1] + n[2]*t[2] > 0, "frente hacia la cámara");
    }
    /* mundo zurdo: espejo de Z automático; el frente sigue mirando a la cámara */
    const float RL[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, 1}};
    set_eye(&bs, RL, e);
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.nv == 3, "build zurdo");
    CHECK(near_f(s.eye_vr[2], -0.5f, 1e-5f), "espejo Z eye_vr z=%.3f", s.eye_vr[2]);
    vr_scene_release();
}

static void test_stability(void) {
    /* un punto fijo del mundo visto desde dos cámaras distintas (giro + traslación) cae en el mismo punto VR */
    float ca = cosf(0.7f), sa = sinf(0.7f);
    const float R1[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    /* R2: R1 girada 0,7 rad alrededor de Y del mundo */
    float R2[3][3];
    for (int c = 0; c < 3; c++) { R2[0][c] = ca * R1[0][c] + sa * R1[2][c]; R2[1][c] = R1[1][c]; R2[2][c] = -sa * R1[0][c] + ca * R1[2][c]; }
    const int32_t e1[3] = {0, 3000, 500}, e2[3] = {1500, 2500, -900};
    const float P[3][3] = {{-500, 100, -4000}, {-300, 600, -4200}, {-700, 900, -3900}};
    VrScene s1, s2;
    float out1[9], out2[9];
    for (int k = 0; k < 2; k++) {
        const float (*R)[3] = k ? R2 : R1; const int32_t *e = k ? e2 : e1;
        BridgeState bs; set_eye(&bs, R, e);
        reset();
        float v[3][3];
        for (int i = 0; i < 3; i++) to_view(R, e, P[i], v[i]);
        poly3(v[0], v[1], v[2], 0);
        VrSceneOpts o = vr_scene_opts_default();
        o.origin_mode = 1; /* origen fijo: el mundo no se mueve con la cámara */
        VrScene *s = k ? &s2 : &s1;
        CHECK(vr_scene_build(&dl, &bs, &o, s) == 0 && s->nv == 3, "build cam %d (nv=%u)", k, s->nv);
        memcpy(k ? out2 : out1, s->v ? (float[9]){s->v[0].pos[0], s->v[0].pos[1], s->v[0].pos[2], s->v[1].pos[0], s->v[1].pos[1], s->v[1].pos[2],
                                                 s->v[2].pos[0], s->v[2].pos[1], s->v[2].pos[2]} : out1, sizeof out1);
        /* la permutación del frente puede reordenar: se compara como conjunto */
    }
    int ok = 1;
    for (int i = 0; i < 3; i++) {
        int m = 0;
        for (int j = 0; j < 3; j++)
            if (near_f(out1[i*3], out2[j*3], 2e-3f) && near_f(out1[i*3+1], out2[j*3+1], 2e-3f) && near_f(out1[i*3+2], out2[j*3+2], 2e-3f)) m = 1;
        ok &= m;
    }
    CHECK(ok, "escenario fijo entre dos cámaras (cuantización 1.12 de la matriz: 2 mm)");
    /* y coincide con la posición del mundo del juego (VR = mundo*0,001, sin espejo) */
    int ok2 = 1;
    for (int i = 0; i < 3; i++) {
        int m = 0;
        for (int j = 0; j < 3; j++)
            if (near_f(P[i][0] * 0.001f, out1[j*3], 3e-3f) && near_f(P[i][1] * 0.001f, out1[j*3+1], 3e-3f) && near_f(P[i][2] * 0.001f, out1[j*3+2], 3e-3f)) m = 1;
        ok2 &= m;
    }
    CHECK(ok2, "posición VR == mundo del juego * escala");
    vr_scene_release();
}

static void test_classes_and_events(void) {
    const float R[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    const int32_t e[3] = {0, 3000, 500};
    BridgeState bs; set_eye(&bs, R, e);
    reset();
    float a[3] = {0, 0, 2000}, b[3] = {1000, 0, 2000}, c[3] = {0, 1000, 2000};
    poly3(a, b, c, 0);                                   /* op0 3D opaco */
    { GpuOp *r = add(GPU_OP_RECT); r->v[0].x = 10; r->v[0].y = 20; r->v[0].u = 5; r->v[0].v = 6; r->rw = 16; r->rh = 8; r->flags = GPU_F_TEXTURED | GPU_F_FLIPX | GPU_F_SEMI; r->abr = 2; } /* op1 */
    poly3(a, b, c, 1);                                   /* op2 3D semi */
    { GpuOp *o = add(GPU_OP_POLY); o->nv = 4; o->flags = GPU_F_GOURAUD; /* op3 2D quad sin float */
      for (int i = 0; i < 4; i++) { o->v[i].x = (int16_t)(i & 1 ? 100 : 50); o->v[i].y = (int16_t)(i & 2 ? 90 : 40); } }
    { GpuOp *o = add(GPU_OP_UPLOAD); o->rx = 0; o->ry = 0; o->rw = 4; o->rh = 4; }   /* op4 evento */
    poly3(a, b, c, 0);                                   /* op5 3D opaco (otra época) */
    { GpuOp *o = add(GPU_OP_LINE); o->nv = 2; o->v[0].x = 0; o->v[0].y = 0; o->v[1].x = 30; o->v[1].y = 40; } /* op6 línea 2D */
    { GpuOp *o = add(GPU_OP_FILL); o->rx = 0; o->ry = 0; o->rw = 16; o->rh = 16; }   /* op7 evento */
    poly3(b, c, a, 0);                                   /* op8 */
    { GpuOp *o = add(GPU_OP_POLY); o->nv = 3; o->flags = 0; /* op9 parcialmente 3D */
      setv3(&o->v[0], 0, 0, 2000); setv3(&o->v[1], 100, 0, 2000); o->v[2].fvalid = 0; o->v[2].vvalid = 0; }
    VrSceneOpts o = vr_scene_opts_default();
    VrScene s;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0, "build");
    CHECK(s.stats.n_events == 2, "eventos %u", s.stats.n_events);
    CHECK(s.stats.drop_partial == 1, "parcial %u", s.stats.drop_partial);
    /* tris: 3D opacos op0, op5, op8 = 3; semi op2 = 1; 2D: rect 2 + quad 2 + línea 2 = 6 */
    CHECK(s.stats.n_tris3d == 4 && s.stats.n_tris2d == 6, "tris3d %u tris2d %u", s.stats.n_tris3d, s.stats.n_tris2d);
    CHECK(s.nv == 30, "nv %u", s.nv);
    /* batches por época: época A (ev_end=4): k0 [op0], k1 [op2], k2 [rect abr2 | quad opaco]; época B (ev_end=7): k0 [op5], k2 [línea]; época C (ev_end=10): k0 [op8] */
    uint32_t prev = 0;
    int kinds_ok = 1;
    for (uint32_t i = 0; i < s.nb; i++) { if (s.b[i].ev_end < prev) kinds_ok = 0; prev = s.b[i].ev_end; }
    CHECK(kinds_ok, "ev_end no decrece");
    CHECK(s.nb == 7, "nb %u", s.nb);
    if (s.nb == 7) {
        const struct { uint8_t k, bl; uint32_t ev, n, src; } ex[7] = {
            {0, 0xFF, 4, 3, 0}, {1, 1, 4, 3, 2}, {2, 2, 4, 6, 1}, {2, 0xFF, 4, 6, 3},
            {0, 0xFF, 7, 3, 5}, {2, 0xFF, 7, 6, 6}, {0, 0xFF, 10, 3, 8}};
        for (int i = 0; i < 7; i++)
            CHECK(s.b[i].kind == ex[i].k && s.b[i].blend == ex[i].bl && s.b[i].ev_end == ex[i].ev && s.b[i].count == ex[i].n && s.b[i].src_op == ex[i].src,
                  "batch %d: k%u bl%u ev%u n%u src%u", i, s.b[i].kind, s.b[i].blend, s.b[i].ev_end, s.b[i].count, s.b[i].src_op);
    }
    /* rect: flip X -> u decrece; flags */
    for (uint32_t i = 0; i < s.nb; i++)
        if (s.b[i].kind == 2 && s.b[i].src_op == 1) {
            const VrVtx *v = &s.v[s.b[i].first];
            CHECK((v[0].flags & VR_F_2D) && (v[0].flags & VR_F_RECT), "flags rect");
            float umin = 1e9f, umax = -1e9f;
            for (int k = 0; k < 6; k++) { umin = fminf(umin, v[k].uv[0]); umax = fmaxf(umax, v[k].uv[0]); }
            CHECK(near_f(umax, 5, 1e-4f) && near_f(umin, 5 - 16, 1e-4f), "u rect flip %f %f", umin, umax);
            CHECK(near_f(v[0].pos[0], 10, 1e-4f) && near_f(v[0].pos[2], 0, 1e-4f), "pos rect");
        }
    /* líneas: 2D con VR_F_LINE */
    for (uint32_t i = 0; i < s.nb; i++)
        if (s.b[i].src_op == 6) CHECK(s.v[s.b[i].first].flags & VR_F_LINE, "línea");
    /* partial_to_2d */
    o.partial_to_2d = 1;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.drop_partial == 0 && s.stats.n_poly2d == 2, "partial_to_2d (%u)", s.stats.n_poly2d);
    vr_scene_release();
}

static void test_drops(void) {
    const float R[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    const int32_t e[3] = {0, 3000, 500};
    BridgeState bs; set_eye(&bs, R, e);
    reset();
    float a[3] = {0, 0, 2000}, b[3] = {1000, 0, 2000}, c[3] = {0, 1000, 2000};
    float bh[3] = {0, 0, -500};                          /* detrás de la cámara */
    float a2[3] = {0, 0, 2000}, c2[3] = {2000, 0, 2000}; /* degenerado (colineal) */
    float h0[3] = {0, 0, 2000}, h1[3] = {500000, 0, 2000}, h2[3] = {0, 500000, 2000}; /* enorme */
    float nanp[3] = {NAN, 0, 2000};
    poly3(a, b, c, 0);
    poly3(a, b, bh, 0);
    poly3(a2, b, c2, 0);
    poly3(h0, h1, h2, 0);
    poly3(a, b, nanp, 0);
    VrSceneOpts o = vr_scene_opts_default(); o.engine_near_clip = 1;   /* comportamiento previo: recorte contra el plano del motor */
    o.coherence = 0;   /* aquí se prueban los límites propios (max_edge_m); la red de §56 va en test_coherence */
    VrScene s;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0, "build");
    CHECK(s.stats.n_tris3d == 1 && s.stats.drop_near == 1 && s.stats.drop_degenerate == 1 && s.stats.drop_huge == 1 && s.stats.drop_nan == 1,
          "3d %u near %u deg %u huge %u nan %u", s.stats.n_tris3d, s.stats.drop_near, s.stats.drop_degenerate, s.stats.drop_huge, s.stats.drop_nan);
    /* sin puente: espacio de vista, cámara en el origen, -Z adelante */
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && !s.world_valid && s.has_3d, "sin puente");
    for (uint32_t i = 0; i < s.nv; i++) CHECK(near_f(s.v[i].pos[2], -2.0f, 1e-4f), "sin puente z=%f", s.v[i].pos[2]);
    /* lista vacía, argumentos nulos, escala distinta */
    GpuDrawList empty; memset(&empty, 0, sizeof empty);
    CHECK(vr_scene_build(&empty, &bs, &o, &s) == 0 && s.nv == 0 && s.nb == 0 && !s.has_3d, "vacía");
    CHECK(vr_scene_build(NULL, &bs, &o, &s) < 0 && vr_scene_build(&empty, &bs, &o, NULL) < 0, "nulos");
    VrSceneOpts o2 = o; o2.scale = 0.002f;
    CHECK(vr_scene_build(&dl, &bs, &o2, &s) == 0 && s.stats.n_tris3d == 1 && near_f(s.scale, 0.002f, 1e-9f), "escala");
    vr_scene_release();
}


/* ---- recorte cercano (contrato §28) ---- */
static GpuOp *polyN(int nv, const float P[4][3], const uint8_t fl[4], int gour) {
    GpuOp *o = add(GPU_OP_POLY);
    o->nv = (uint8_t)nv; o->flags = (uint16_t)(GPU_F_TEXTURED | (gour ? GPU_F_GOURAUD : 0)); o->tp = 1; o->tx = 320; o->ty = 256;
    for (int i = 0; i < nv; i++) {
        setv3(&o->v[i], P[i][0], P[i][1], P[i][2]);
        o->v[i].vflags = fl[i];
        if (fl[i]) o->v[i].fvalid = 0;
        o->v[i].u = (uint8_t)(20 * i); o->v[i].v = (uint8_t)(10 * i + 3);
        o->v[i].r = (uint8_t)(40 * i + 10); o->v[i].g = 100; o->v[i].b = (uint8_t)(200 - 30 * i);
    }
    return o;
}

static void test_near_clip(void) {
    const float R[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    const int32_t e[3] = {0, 3000, 500};
    BridgeState bs; set_eye(&bs, R, e);
    VrSceneOpts o = vr_scene_opts_default(); o.engine_near_clip = 1;
    o.origin_mode = 1;
    VrScene s;
    const float zn = o.near_units;
    /* a) triángulo con 1 vértice detrás -> quad (2 triángulos); con 2 detrás -> 1 triángulo; con 3 -> 0 */
    for (int nb = 0; nb <= 3; nb++) {
        reset();
        float P[4][3] = {{0, 0, 2000}, {1000, 0, 2000}, {0, 1000, 2000}, {0, 0, 0}};
        uint8_t fl[4] = {0, 0, 0, 0};
        for (int i = 0; i < nb; i++) { P[2 - i][2] = -400 - 100 * i; fl[2 - i] = GPU_VF_BEHIND; }
        polyN(3, P, fl, 1);
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0, "build clip");
        uint32_t want = nb == 0 ? 1 : nb == 1 ? 2 : nb == 2 ? 1 : 0;
        CHECK(s.stats.n_tris3d == want && s.nv == 3 * want, "clip %d detrás: tris %u (esperado %u)", nb, s.stats.n_tris3d, want);
        CHECK(s.stats.n_poly_behind == (nb ? 1u : 0u) && s.stats.drop_behind == (nb == 3 ? 1u : 0u) && s.stats.n_clipped == (nb && nb < 3 ? 1u : 0u),
              "stats clip %d: behind %u dropb %u clipped %u", nb, s.stats.n_poly_behind, s.stats.drop_behind, s.stats.n_clipped);
        /* todo vértice emitido está delante del plano cercano (en VR: z <= -zn*escala; cámara mira a -Z desde z=0,5) */
        for (uint32_t i = 0; i < s.nv; i++) {
            float zv = (s.eye_vr[2] - s.v[i].pos[2]) / s.scale;   /* profundidad de vista */
            CHECK(zv >= zn - 0.05f, "vértice recortado a z=%f < near", zv);
        }
    }
    /* b) interpolación: arista (0,0,2000)-(0,0,-1000) cruza z=24 en s=(2000-24)/3000; uv/color lineales; posición exacta */
    {
        reset();
        float P[4][3] = {{0, 0, 2000}, {1000, 0, 2000}, {0, 1000, -1000}, {0, 0, 0}};
        uint8_t fl[4] = {0, 0, GPU_VF_BEHIND, 0};
        polyN(3, P, fl, 1);
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.nv == 6, "build interp");
        float sp = (2000.0f - zn) / 3000.0f;
        int found = 0;
        for (uint32_t i = 0; i < s.nv; i++) {
            float zv = (s.eye_vr[2] - s.v[i].pos[2]) / s.scale;
            if (!near_f(zv, zn, 0.05f)) continue;
            /* vértice en la arista 0->2: x=0, y=1000*sp (vista, abajo=+y => VR y = 3 - ...) */
            float yv = (3.0f - s.v[i].pos[1]) / s.scale;
            if (near_f(s.v[i].pos[0], 0, 1e-4f) && near_f(yv, 1000 * sp, 0.05f)) {
                found++;
                CHECK(near_f(s.v[i].uv[0], 40 * sp, 1e-3f) && near_f(s.v[i].uv[1], 3 + (23 - 3) * sp, 1e-3f), "uv interpolado %f %f", s.v[i].uv[0], s.v[i].uv[1]);
                CHECK((s.v[i].rgba & 255) == (uint32_t)lrintf(10 + (90 - 10) * sp) && ((s.v[i].rgba >> 16) & 255) == (uint32_t)lrintf(200 + (140 - 200) * sp), "color interpolado %08x", s.v[i].rgba);
            }
        }
        CHECK(found >= 1, "vértice de la arista 0-2 no aparece (%d)", found);
    }
    /* c) quad con 1 vértice detrás: dos triángulos (0,1,2),(1,2,3); el vértice 3 detrás recorta solo el segundo (1 vértice atrás -> 2 tris) */
    {
        reset();
        float P[4][3] = {{0, 0, 2000}, {1000, 0, 2000}, {0, 1000, 2000}, {1000, 1000, -300}};
        uint8_t fl[4] = {0, 0, 0, GPU_VF_BEHIND};
        polyN(4, P, fl, 0);
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 3 && s.stats.n_clipped == 1, "quad 1 detrás: tris %u clip %u", s.stats.n_tris3d, s.stats.n_clipped);
        /* con 2 detrás (2 y 3): tri0 pierde 1 vértice (->2), tri1 pierde 2 (->1): total 3 */
        reset();
        float P2[4][3] = {{0, 0, 2000}, {1000, 0, 2000}, {0, 1000, -300}, {1000, 1000, -300}};
        uint8_t fl2[4] = {0, 0, GPU_VF_BEHIND, GPU_VF_BEHIND};
        polyN(4, P2, fl2, 0);
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 3 && s.stats.n_clipped == 2, "quad 2 detrás: tris %u clip %u", s.stats.n_tris3d, s.stats.n_clipped);
    }
    /* d) sin BEHIND: la misma geometría con vz<near se descarta como antes (salida previa) */
    {
        reset();
        float P[4][3] = {{0, 0, 2000}, {1000, 0, 2000}, {0, 1000, -300}, {0, 0, 0}};
        uint8_t fl[4] = {0, 0, 0, 0};
        polyN(3, P, fl, 0);
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 0 && s.stats.drop_near == 1 && s.stats.n_clipped == 0, "sin BEHIND: drop_near");
        o.clip_all_near = 1;
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 2 && s.stats.n_clipped == 1, "clip_all_near");
        o.clip_all_near = 0;
    }
    /* e) AMBIG: solo -> aceptado; con SAT o BEHIND -> descartado; DERIVED aceptado salvo drop_derived */
    {
        const float P[4][3] = {{0, 0, 2000}, {1000, 0, 2000}, {0, 1000, 2000}, {0, 0, 0}};
        uint8_t fa[4] = {GPU_VF_AMBIG, 0, 0, 0}, fb[4] = {GPU_VF_AMBIG, GPU_VF_SAT, 0, 0}, fc[4] = {GPU_VF_AMBIG, 0, GPU_VF_BEHIND, 0},
                fd[4] = {GPU_VF_DERIVED, 0, 0, 0}, fs[4] = {GPU_VF_SAT, 0, 0, 0};
        reset(); polyN(3, P, fa, 0); CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 1 && s.stats.drop_ambig == 0, "AMBIG solo");
        reset(); polyN(3, P, fb, 0); CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 0 && s.stats.drop_ambig == 1, "AMBIG+SAT");
        reset(); { float Q[4][3] = {{0, 0, 2000}, {1000, 0, 2000}, {0, 1000, -200}, {0, 0, 0}}; polyN(3, Q, fc, 0); }
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 0 && s.stats.drop_ambig == 1, "AMBIG+BEHIND");
        reset(); polyN(3, P, fd, 0); CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 1, "DERIVED aceptado");
        o.drop_derived = 1; CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 0 && s.stats.drop_derived == 1, "DERIVED descartado"); o.drop_derived = 0;
        /* SAT usa vx,vy,vz (los x,y enteros no intervienen) */
        reset(); { GpuOp *g = polyN(3, P, fs, 0); g->v[0].x = 511; g->v[0].y = -300; }
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 1 && near_f(s.v[0].pos[0], 0, 1.01f), "SAT con vx,vy,vz");
    }
    /* f) sin BEHIND/SAT/AMBIG/DERIVED: salida idéntica bit a bit a la previa (fvalid) y vvalid sin fvalid cuenta igual */
    {
        reset();
        const float P[4][3] = {{0, 0, 2000}, {1000, 0, 2000}, {0, 1000, 2500}, {1100, 900, 2300}};
        uint8_t z[4] = {0, 0, 0, 0};
        polyN(4, P, z, 1);
        VrScene s1, s2;
        CHECK(vr_scene_build(&dl, &bs, &o, &s1) == 0 && s1.nv == 6, "build previo");
        size_t bytes = s1.nv * sizeof(VrVtx);
        VrVtx *copy = malloc(bytes); memcpy(copy, s1.v, bytes);
        VrSceneOpts o2 = o; o2.clip_all_near = 1; o2.drop_derived = 1; /* no afectan sin BEHIND/DERIVED y con vz>near */
        CHECK(vr_scene_build(&dl, &bs, &o2, &s2) == 0 && s2.nv == s1.nv && memcmp(copy, s2.v, bytes) == 0, "salida idéntica sin BEHIND");
        /* y coincide con la fórmula previa: uv enteros, color en rango */
        for (uint32_t i = 0; i < s1.nv; i++) CHECK(s1.v[i].uv[0] == floorf(s1.v[i].uv[0]) && s1.v[i].uv[1] == floorf(s1.v[i].uv[1]), "uv enteros");
        free(copy);
    }
    /* g) fuzz: vértices aleatorios con BEHIND; nunca vértices detrás del plano, sin NaN, nº de triángulos <= 2 por triángulo de entrada */
    srand(11);
    for (int it = 0; it < 3000; it++) {
        reset();
        int nv = 3 + (rand() & 1);
        float P[4][3]; uint8_t fl[4];
        for (int k = 0; k < 4; k++) {
            P[k][0] = (float)(rand() % 4001 - 2000); P[k][1] = (float)(rand() % 4001 - 2000); P[k][2] = (float)(rand() % 4001 - 1500);
            fl[k] = P[k][2] <= 0 ? GPU_VF_BEHIND : 0;
        }
        polyN(nv, P, fl, rand() & 1);
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d <= (nv == 4 ? 4u : 2u), "fuzz n=%u", s.stats.n_tris3d);
        for (uint32_t i = 0; i < s.nv; i++) {
            float zv = (s.eye_vr[2] - s.v[i].pos[2]) / s.scale;
            CHECK(isfinite(s.v[i].pos[0]) && isfinite(s.v[i].pos[1]) && zv >= zn - 0.5f, "fuzz vértice z=%f", zv);
            CHECK(isfinite(s.v[i].uv[0]) && s.v[i].uv[0] >= -0.01f && s.v[i].uv[0] <= 60.01f, "fuzz uv %f", s.v[i].uv[0]);
        }
    }
    vr_scene_release();
}


/* ---- clase 3: filtro de pantalla ---- */
static GpuOp *rect2d(int x, int y, int w, int h, int u, int v, uint16_t flags, int tp, int tx, int ty, uint8_t r, uint8_t g, uint8_t b) {
    GpuOp *o = add(GPU_OP_RECT);
    o->v[0].x = (int16_t)x; o->v[0].y = (int16_t)y; o->v[0].u = (uint8_t)u; o->v[0].v = (uint8_t)v; o->v[0].r = r; o->v[0].g = g; o->v[0].b = b;
    o->rw = (int16_t)w; o->rh = (int16_t)h; o->flags = flags; o->tp = (uint8_t)tp; o->tx = (uint16_t)tx; o->ty = (uint16_t)ty;
    o->cx1 = 0; o->cy1 = 0; o->cx2 = 319; o->cy2 = 255;
    return o;
}
static void test_filter(void) {
    const float R[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    const int32_t e[3] = {0, 3000, 500};
    BridgeState bs; set_eye(&bs, R, e);
    reset();
    dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 256;
    float a[3] = {0, 0, 2000}, b[3] = {1000, 0, 2000}, c[3] = {0, 1000, 2000};
    rect2d(0, 0, 320, 256, 0, 0, 0, 0, 0, 0, 0, 0, 0);                    /* op0 borrado opaco: clase 2 (n_clear) */
    poly3(a, b, c, 0);                                                    /* op1 3D */
    for (int i = 0; i < 20; i++) {                                         /* ops 2..41: tiras de copia de pantalla, mitades izq/der */
        rect2d(0, 6 + 2 * i, 160, 2, 0, 10 + 2 * i + (i & 1), GPU_F_TEXTURED | GPU_F_MASK_SET, 2, 0, 0, 0x40, 0x80, 0x40);
        rect2d(160, 6 + 2 * i, 160, 2, 32, 10 + 2 * i + (i & 1), GPU_F_TEXTURED | GPU_F_MASK_SET, 2, 128, 0, 0x40, 0x80, 0x40);
    }
    rect2d(16, 16, 40, 20, 0, 0, GPU_F_TEXTURED, 0, 960, 256, 128, 128, 128);                       /* op42 HUD (CLUT 4 bits): clase 2 */
    rect2d(16, 40, 40, 20, 0, 0, GPU_F_TEXTURED, 2, 640, 256, 128, 128, 128);                       /* op43 15 bits fuera de los búferes: clase 2 */
    rect2d(0, 0, 320, 256, 0, 0, GPU_F_SEMI, 0, 0, 0, 20, 30, 40)->abr = 1;                          /* op44 recubrimiento semitransparente */
    rect2d(0, 0, 100, 100, 0, 0, GPU_F_SEMI, 0, 0, 0, 20, 30, 40);                                   /* op45 semitransparente pequeño: clase 2 */
    { GpuOp *o = add(GPU_OP_POLY); o->nv = 4; o->flags = GPU_F_TEXTURED; o->tp = 2; o->tx = 0; o->ty = 0; o->cx1 = 0; o->cy1 = 0; o->cx2 = 319; o->cy2 = 255;
      for (int i = 0; i < 4; i++) { o->v[i].x = (int16_t)(i & 1 ? 300 : 20); o->v[i].y = (int16_t)(i & 2 ? 200 : 30); o->v[i].u = (uint8_t)(i & 1 ? 255 : 0); o->v[i].v = (uint8_t)(i & 2 ? 200 : 30); } } /* op46 copia como polígono 2D */
    VrSceneOpts o = vr_scene_opts_default();
    CHECK(o.screen_filter == 0 && o.drop_clear == 1, "defectos: clase 3 desactivada, clear descartado");
    VrScene s;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_filter_tris == 0 && !s.filter.active && s.stats.n_clear == 1, "por defecto no hay clase 3");
    for (uint32_t i = 0; i < s.nb; i++) CHECK(s.b[i].kind != 3, "por defecto sin batches kind 3");
    o.screen_filter = 1;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0, "build filtro");
    CHECK(s.stats.n_filter_rects == 40 && s.stats.n_filter_polys == 1 && s.stats.n_filter_cover == 1, "filtro rects %u polys %u cover %u",
          s.stats.n_filter_rects, s.stats.n_filter_polys, s.stats.n_filter_cover);
    CHECK(s.stats.n_clear == 1, "n_clear %u", s.stats.n_clear);
    CHECK(s.stats.n_filter_tris == (40 + 1 + 1) * 2 && s.stats.n_tris3d == 1, "tris filtro %u 3d %u", s.stats.n_filter_tris, s.stats.n_tris3d);
    /* 2D (clase 2): HUD, 15 bits fuera de búfer, semi pequeño = 3 rects -> 6 tris (el borrado no se emite: drop_clear) */
    CHECK(s.stats.n_tris2d == 6, "tris 2D %u", s.stats.n_tris2d);
    uint32_t k3 = 0, k2 = 0;
    for (uint32_t i = 0; i < s.nb; i++) {
        if (s.b[i].kind == 3) { k3 += s.b[i].count; for (uint32_t j = 0; j < s.b[i].count; j++) CHECK((s.v[s.b[i].first + j].flags & (VR_F_FILTER | VR_F_2D)) == (VR_F_FILTER | VR_F_2D), "flags filtro"); }
        if (s.b[i].kind == 2) { k2 += s.b[i].count; for (uint32_t j = 0; j < s.b[i].count; j++) CHECK(!(s.v[s.b[i].first + j].flags & VR_F_FILTER), "flags 2D sin filtro"); }
    }
    CHECK(k3 == 84 * 3 / 2 * 1 || k3 == (uint32_t)s.stats.n_filter_tris * 3, "vértices kind 3 %u", k3);
    CHECK(k2 == 18, "vértices kind 2 %u", k2);
    const VrFilter *f = &s.filter;
    CHECK(f->active && f->n_strips == 40 && f->n_polys == 1 && f->n_cover == 1, "resumen filtro");
    CHECK(near_f(f->tint[0], 0.5f, 1e-4f) && near_f(f->tint[1], 1.0f, 1e-4f) && near_f(f->tint[2], 0.5f, 1e-4f), "tinte %f %f %f", f->tint[0], f->tint[1], f->tint[2]);
    CHECK(f->rowcov[6] && f->rowcov[7] && !f->rowcov[5] && !f->rowcov[46] && f->nrows == 46, "filas cubiertas nrows %d", f->nrows);
    CHECK(near_f(f->dv[6], 4, 1e-4f) && near_f(f->dv[8], 4 + 1 - 0 , 1.01f) && near_f(f->du[6], 0, 1e-4f) && near_f(f->du[6 + 0], 0, 1e-4f), "dv %f %f du %f", f->dv[6], f->dv[8], f->du[6]);
    CHECK(f->cover_abr == 1 && near_f(f->cover_rgb[1], 30 / 255.0f, 1e-4f), "recubrimiento");
    /* con screen_filter=0: comportamiento anterior (todo clase 2) */
    o.screen_filter = 0; o.drop_clear = 0;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_filter_tris == 0 && !s.filter.active && s.stats.n_tris2d == 8 + 84, "screen_filter=0: tris2d %u", s.stats.n_tris2d);
    for (uint32_t i = 0; i < s.nb; i++) CHECK(s.b[i].kind != 3, "sin batches kind 3");
    /* falsos positivos: panel pequeño con su recorte (semi sobre su propio recorte), parche local que copia la pantalla, polígono texturizado semi a pantalla completa */
    reset();
    dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 256;
    { GpuOp *r = rect2d(235, 16, 69, 52, 0, 0, GPU_F_SEMI, 0, 0, 0, 0, 24, 24); r->cx1 = 235; r->cy1 = 16; r->cx2 = 303; r->cy2 = 67; }   /* minimapa */
    rect2d(90, 30, 140, 89, 0, 128, GPU_F_TEXTURED | GPU_F_SEMI, 2, 0, 0, 32, 32, 32);                                             /* parche 140x89 */
    rect2d(0, 0, 320, 256, 0, 0, GPU_F_SEMI | GPU_F_TEXTURED, 1, 128, 256, 0, 0, 0);                                                 /* semi texturizado a pantalla completa */
    rect2d(300, 0, 1, 1, 5, 5, GPU_F_TEXTURED, 2, 0, 0, 128, 128, 128);                                                             /* 1x1 con origen dentro del búfer */
    o = vr_scene_opts_default(); o.screen_filter = 1;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && !s.filter.active && s.stats.n_filter_tris == 0, "falsos positivos de clase 3: tris %u", s.stats.n_filter_tris);
    vr_scene_release();
}

/* Mezcla con el fotograma anterior (sprite semitransparente de 15 bits que lee del búfer alterno): no va al panel, con o sin screen_filter */
static void test_ghost(void) {
    const float R[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    const int32_t e[3] = {0, 3000, 500};
    BridgeState bs; set_eye(&bs, R, e);
    reset();
    dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 256;
    rect2d(0, 0, 160, 184, 0, 27, GPU_F_TEXTURED | GPU_F_SEMI, 2, 320, 0, 120, 120, 120);       /* mitad izquierda leyendo del búfer vecino (x 320): estela */
    rect2d(160, 0, 160, 184, 32, 27, GPU_F_TEXTURED | GPU_F_SEMI, 2, 448, 0, 120, 120, 120);    /* mitad derecha */
    rect2d(16, 40, 40, 20, 0, 0, GPU_F_TEXTURED, 2, 640, 256, 128, 128, 128);                   /* contenido 2D normal */
    rect2d(0, 0, 100, 60, 0, 0, GPU_F_TEXTURED | GPU_F_SEMI, 2, 320, 0, 128, 128, 128);        /* parche pequeño que lee del vecino: se conserva */
    VrSceneOpts o = vr_scene_opts_default();
    VrScene s;
    CHECK(o.drop_ghost == 1, "drop_ghost por defecto");
    for (int sf = 0; sf < 2; sf++) {
        o.screen_filter = sf;
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_filter_ghost == 2, "estela descartada (screen_filter=%d): %u", sf, s.stats.n_filter_ghost);
        CHECK(s.stats.n_tris2d == 4 && !s.filter.active, "queda el 2D normal: tris2d %u activo %d", s.stats.n_tris2d, s.filter.active);
    }
    o.drop_ghost = 0;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_filter_ghost == 0 && s.stats.n_tris2d == 8, "drop_ghost=0: comportamiento anterior");
    vr_scene_release();
}

static void test_state(void) {
    const float R[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    const int32_t e[3] = {0, 3000, 500};
    BridgeState bs; set_eye(&bs, R, e);
    reset();
    float a[3] = {0, 0, 2000}, b[3] = {1000, 0, 2000}, c[3] = {0, 1000, 2000};
    GpuOp *o1 = poly3(a, b, c, 0); o1->twmx = 3; o1->twox = 1; o1->cx2 = 319; o1->cy2 = 239;
    GpuOp *o2 = poly3(a, b, c, 0); o2->twmx = 3; o2->twox = 1; o2->cx2 = 319; o2->cy2 = 239;
    GpuOp *o3 = poly3(a, b, c, 0); (void)o3;
    VrSceneOpts o = vr_scene_opts_default();
    VrScene s;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.nstates == 2, "estados %u", s.nstates);
    CHECK(s.v[0].pad == s.v[3].pad && s.v[0].pad != s.v[6].pad, "índices de estado");
    const VrState *st = vr_scene_state(&s, s.v[0].pad);
    CHECK(st && st->twmx == 3 && st->twox == 1 && st->cx2 == 319, "estado");
    CHECK(vr_scene_state(&s, 77) == NULL, "estado fuera de rango");
    vr_scene_release();
}


static void test_gte_scale_and_validity(void) {
    /* La GTE usa N = M^T con M = eye_axes (columnas escaladas ~1,002): el mundo reconstruido debe ser exacto, sin
     * escala 1,002 respecto al ojo. Vértice de vista v = N*(P - e) -> mundo e + inv(N) v. */
    const float Rr[3][3] = {{1, 0, 0}, {0, -0.5f, -0.8660254f}, {0, 0.8660254f, -0.5f}};
    const float D[3] = {1.0024f, 1.0034f, 1.0008f};
    float M[3][3];
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) M[r][c] = Rr[r][c] * D[c];
    const int32_t e[3] = {-5800, 8188, 1807};
    BridgeState bs; set_eye(&bs, M, e);
    reset();
    const float P[3][3] = {{-5000, 0, -3000}, {-6000, 100, -2500}, {-4000, 50, -3500}};
    float v[3][3];
    for (int i = 0; i < 3; i++) {
        float Q[3] = {P[i][0] - e[0], P[i][1] - e[1], P[i][2] - e[2]};
        for (int c = 0; c < 3; c++) { v[i][c] = 0; for (int r = 0; r < 3; r++) v[i][c] += (bs.eye_axes[r][c] / 4096.0f) * Q[r]; }
    }
    poly3(v[0], v[1], v[2], 0);
    VrSceneOpts o = vr_scene_opts_default();
    o.origin_mode = 1;
    VrScene s;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.nv == 3, "build gte");
    int ok = 1;
    for (int i = 0; i < 3; i++) {
        int m = 0;
        for (uint32_t j = 0; j < 3; j++)
            if (near_f(s.v[j].pos[0], P[i][0] * 0.001f, 1e-3f) && near_f(s.v[j].pos[1], P[i][1] * 0.001f, 1e-3f) &&
                near_f(s.v[j].pos[2], P[i][2] * 0.001f, 1e-3f)) m = 1;
        ok &= m;
    }
    CHECK(ok, "mundo exacto con la matriz escalada de la GTE");
    /* eye_from_view_t coincide con eye_pos cuando la traslación es -N*e */
    for (int c = 0; c < 3; c++) { float t = 0; for (int r = 0; r < 3; r++) t -= (bs.eye_axes[r][c] / 4096.0f) * (float)e[r]; bs.view_t[c] = (int32_t)lrintf(t); }
    VrScene s2; o.eye_from_view_t = 1;
    float eye_a[3] = {s.eye_vr[0], s.eye_vr[1], s.eye_vr[2]};
    CHECK(vr_scene_build(&dl, &bs, &o, &s2) == 0 && near_f(s2.eye_vr[0], eye_a[0], 2e-3f) && near_f(s2.eye_vr[1], eye_a[1], 2e-3f) &&
          near_f(s2.eye_vr[2], eye_a[2], 2e-3f), "ojo desde view_t == eye_pos");
    o.eye_from_view_t = 0;
    /* validez del puente: sin jugador o sin ALL_CORE => world_valid 0 (la matriz del ojo se aplica igualmente) */
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.world_valid, "world_valid con puente completo");
    bs.has_player = 0;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && !s.world_valid && s.nv == 3, "sin jugador");
    bs.has_player = 1; bs.valid &= ~BRIDGE_OK_ALERT;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && !s.world_valid, "sin ALL_CORE");
    bs.valid &= ~BRIDGE_OK_EYE;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && !s.world_valid && s.nv == 3, "sin EYE");
    vr_scene_release();
}

static void test_line_coverage(void) {
    reset();
    { GpuOp *o = add(GPU_OP_LINE); o->nv = 2; o->v[0].x = 10; o->v[0].y = 20; o->v[1].x = 14; o->v[1].y = 20; }
    VrSceneOpts o = vr_scene_opts_default();
    VrScene s;
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.nv == 6, "línea");
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (uint32_t i = 0; i < s.nv; i++) {
        x0 = fminf(x0, s.v[i].pos[0]); x1 = fmaxf(x1, s.v[i].pos[0]); y0 = fminf(y0, s.v[i].pos[1]); y1 = fmaxf(y1, s.v[i].pos[1]);
    }
    /* píxeles 10..14 en x (5) y la fila 20 */
    CHECK(near_f(x0, 10, 1e-4f) && near_f(x1, 15, 1e-4f) && near_f(y0, 20, 1e-4f) && near_f(y1, 21, 1e-4f), "cobertura línea %f %f %f %f", x0, x1, y0, y1);
    vr_scene_release();
}

static void test_event_epochs(void) {
    /* aleatorio: ev_end de cada triángulo == nº de op < src_op que son eventos... en índices: primer evento con índice > op */
    srand(7);
    const float R[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    const int32_t e[3] = {0, 3000, 500};
    BridgeState bs; set_eye(&bs, R, e);
    float a[3] = {0, 0, 2000}, b[3] = {1000, 0, 2000}, c[3] = {0, 1000, 2000};
    for (int iter = 0; iter < 200; iter++) {
        reset();
        int n = 1 + rand() % 60;
        for (int i = 0; i < n; i++) {
            int k = rand() % 6;
            if (k == 0) { GpuOp *o = add(GPU_OP_FILL); o->rw = 1; o->rh = 1; }
            else if (k == 1) { GpuOp *o = add(GPU_OP_UPLOAD); o->rw = 1; o->rh = 1; }
            else if (k == 2) add(GPU_OP_DOWNLOAD);
            else if (k == 3) { GpuOp *o = add(GPU_OP_RECT); o->rw = 4; o->rh = 4; o->flags = (uint16_t)(rand() & 1 ? GPU_F_SEMI : 0); o->abr = (uint8_t)(rand() & 3); }
            else poly3(a, b, c, rand() & 1);
        }
        VrSceneOpts o = vr_scene_opts_default();
        VrScene s;
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0, "build fuzz");
        uint32_t prev = 0, total = 0;
        for (uint32_t i = 0; i < s.nb; i++) {
            const VrBatch *bt = &s.b[i];
            CHECK(bt->ev_end >= prev, "monotónico"); prev = bt->ev_end;
            total += bt->count;
            for (uint32_t j = 0; j < bt->count; j += 3) {
                /* ningún evento de índice < src (de la primitiva) puede quedar fuera, ni uno > op dentro */
                const VrVtx *v = &s.v[bt->first + j]; (void)v;
            }
            /* eventos con índice < ev_end deben ser exactamente los anteriores a la primitiva del batch */
            for (uint32_t q = 0; q < dl.nops; q++) {
                int ev = dl.ops[q].type == GPU_OP_FILL || dl.ops[q].type == GPU_OP_UPLOAD || dl.ops[q].type == GPU_OP_VCOPY;
                if (!ev) continue;
                CHECK((q < bt->ev_end) == (q < bt->src_op), "evento %u vs ev_end %u src_op %u", q, bt->ev_end, bt->src_op);
            }
        }
        CHECK(total == s.nv, "vértices en batches");
    }
    vr_scene_release();
}

/* ---- realimentación (GpuOp.fbk/fbt): sin textura, color = vértice * texel / 128, semi solo con bit 15, texel 0 descartado ---- */
static void test_feedback(void) {
    reset();
    dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 256;
    /* op0: quad texturizado semi, texel 0xC3E0 (bit15, g=31, b=16, r=0), color (0x80,0x80,0x40) */
    GpuOp *q = add(GPU_OP_POLY);
    q->nv = 4; q->flags = GPU_F_TEXTURED | GPU_F_SEMI | GPU_F_QUAD; q->abr = 2; q->tp = 2; q->cx2 = 319; q->cy2 = 255; q->fbk = 1; q->fbt = 0xC3E0;
    for (int i = 0; i < 4; i++) { q->v[i].x = (int16_t)(i & 1 ? 120 : 80); q->v[i].y = (int16_t)(i & 2 ? 108 : 100); q->v[i].u = (uint8_t)(i & 1); q->v[i].v = (uint8_t)((i >> 1) & 1);
                                  q->v[i].r = 0x80; q->v[i].g = 0x80; q->v[i].b = 0x40; }
    /* op1: igual pero texel sin bit 15 (opaco aunque el polígono sea semi) y crudo */
    GpuOp *r = add(GPU_OP_POLY);
    *r = *q; r->flags |= GPU_F_RAW; r->fbt = 0x4210; r->seq = 1;
    /* op2: texel 0 -> transparente, no se emite */
    GpuOp *z = add(GPU_OP_POLY);
    *z = *q; z->fbt = 0; z->seq = 2;
    /* op3: rect 1x1 con fbk, color 0xFF: texel 0x001F modulado a 0xF8*0xFF/128 -> satura */
    GpuOp *rc = rect2d(10, 10, 1, 1, 0, 0, GPU_F_TEXTURED, 2, 0, 0, 0xFF, 0x40, 0x80); rc->fbk = 1; rc->fbt = 0x001F;
    /* op4: sin fbk: sigue texturizado */
    rect2d(20, 20, 8, 8, 0, 0, GPU_F_TEXTURED, 2, 0, 0, 128, 128, 128);
    VrScene s; VrSceneOpts o = vr_scene_opts_default();
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0, "build realimentacion");
    CHECK(s.stats.n_fb_baked == 2 + 2 + 2 && s.stats.n_fb_clear == 1, "stats fb baked %u clear %u", s.stats.n_fb_baked, s.stats.n_fb_clear);
    CHECK(s.nv / 3 == 2 + 2 + 2 + 2, "triangulos %u", s.nv / 3);   /* 2+2 (quads) + 2 (rect fb) + 2 (rect normal); el texel 0 no emite */
    int n_semi = 0, n_opq = 0, n_tex = 0;
    for (uint32_t i = 0; i < s.nb; i++) {
        const VrBatch *b = &s.b[i];
        for (uint32_t j = 0; j < b->count; j++) {
            const VrVtx *v = &s.v[b->first + j];
            if (v->flags & GPU_F_TEXTURED) { n_tex++; continue; }
            if (b->src_op == 0 && j == 0) { /* primer triangulo del quad 0: modulado y semi (modo abr 2) */
                CHECK(b->blend == 2, "blend op0 %u", b->blend);
                /* e(t5)=expand 5->8 bits; r: 0 -> 0, g: 31 -> 255 *0x80/128 = 255, b: 16 -> 132 * 0x40/128 = 66 */
                CHECK((v->rgba & 255) == 0 && ((v->rgba >> 8) & 255) == 255 && ((v->rgba >> 16) & 255) == 66, "color op0 %08x", v->rgba);
            }
            if (b->blend == 0xFF) n_opq++; else n_semi++;
        }
    }
    for (uint32_t i = 0; i < s.nb; i++) CHECK(s.b[i].skip_if_native == 0 && !vr_batch_skipped(&s.b[i], VR_SKIP_WORLD | VR_SKIP_FX), "batch %u sin marca de nativo por defecto", i);
    CHECK(n_tex == 6, "vertices texturizados %d (el rect sin fbk)", n_tex);
    CHECK(n_semi == 6 && n_opq == 6 + 6, "semi %d opacos %d", n_semi, n_opq);
    /* op1 (crudo): el color es el texel expandido, opaco */
    for (uint32_t i = 0; i < s.nb; i++) {
        const VrBatch *b = &s.b[i];
        if (b->src_op == 1) { const VrVtx *v = &s.v[b->first]; CHECK(b->blend == 0xFF && (v->rgba & 255) == 0x84 && !(v->flags & (GPU_F_TEXTURED | GPU_F_RAW)), "op1 crudo %08x blend %u", v->rgba, b->blend); }
        if (b->src_op == 3) { const VrVtx *v = &s.v[b->first]; CHECK((v->rgba & 255) == 255 && ((v->rgba >> 8) & 255) == 0 && ((v->rgba >> 16) & 255) == 0, "op3 satura %08x", v->rgba); }
    }
    vr_scene_release();
}

static void test_ofs_fix(void) {
    /* desplazamiento de proyección por primitiva: sin cámara del motor, pos_vr = (0,001 vx, -0,001 vy, -0,001 vz) */
    reset();
    dl.disp.x = 320; dl.disp.y = 8; dl.disp.w = 320; dl.disp.h = 240;
    float a[3] = {-500, -300, 2000}, b[3] = {500, -300, 2000}, c[3] = {0, 400, 2000};
    for (int i = 0; i < 3; i++) {   /* 3 opacos con centro (480,136) = (160+320, 128+8): dox=320, doy=8, proj_ox=160, proj_oy=128 */
        GpuOp *o = poly3(a, b, c, 0);
        o->proj_h = 150.0f; o->proj_ox = 160.0f; o->proj_oy = 128.0f; o->dox = 320; o->doy = 8;
    }
    /* un opaco aislado con otro centro (no es la moda) */
    GpuOp *odd = poly3(a, b, c, 0);
    odd->proj_h = 150.0f; odd->proj_ox = 100.0f; odd->proj_oy = 128.0f; odd->dox = 320; odd->doy = 8;
    /* resplandor semitransparente: centro propio (OFX 210+dox 330 = 540, OFY 100+doy 8 = 108), vx,vy locales +-1000 a vz 3000, proj_h 150 */
    float ga[3] = {-1000, -1000, 3000}, gb[3] = {1000, -1000, 3000}, gc[3] = {0, 1000, 3000};
    GpuOp *g = poly3(ga, gb, gc, 1);
    g->proj_h = 150.0f; g->proj_ox = 210.0f; g->proj_oy = 100.0f; g->dox = 330; g->doy = 8;
    VrScene s; VrSceneOpts o = vr_scene_opts_default();
    o.coherence = 0;   /* x,y de pantalla sin rellenar en este caso sintético: la comprobación del modelo (§56) se prueba en test_coherence */
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0, "build ofs");
    CHECK(near_f(s.filter.proj_h, 150.0f, 1e-3f) && near_f(s.filter.cx_col, 160.0f, 1e-3f) && near_f(s.filter.cy_row, 128.0f, 1e-3f),
          "filter proj %.2f %.2f %.2f", s.filter.proj_h, s.filter.cx_col, s.filter.cy_row);
    CHECK(s.stats.n_ref_group == 3, "grupo modal %u", s.stats.n_ref_group);
    CHECK(s.stats.n_ofs_fix == 2, "corregidos %u (el opaco aislado y el resplandor)", s.stats.n_ofs_fix);
    /* resplandor: ex = 540-480 = 60 px, ey = 108-136 = -28 px: x += 60*3000/150 = 1200, y += -28*3000/150 = -560 */
    int found = 0;
    for (uint32_t i = 0; i < s.nv; i++) {
        const VrVtx *v = &s.v[i];
        if (!near_f(v->pos[2], -3.0f, 1e-4f)) continue;
        float vx = v->pos[0] * 1000.0f, vy = -v->pos[1] * 1000.0f;
        if ((near_f(vx, 200, 0.5f) && near_f(vy, -1560, 0.5f)) || (near_f(vx, 2200, 0.5f) && near_f(vy, -1560, 0.5f)) || (near_f(vx, 1200, 0.5f) && near_f(vy, 440, 0.5f))) found++;
    }
    CHECK(found == 3, "vértices corregidos del resplandor %d", found);
    /* un opaco del grupo modal no cambia: (-500,-300,2000) */
    int same = 0;
    for (uint32_t i = 0; i < s.nv; i++) if (near_f(s.v[i].pos[0], -0.5f, 1e-4f) && near_f(s.v[i].pos[1], 0.3f, 1e-4f) && near_f(s.v[i].pos[2], -2.0f, 1e-4f)) same++;
    CHECK(same >= 3, "polígonos normales sin cambio (%d)", same);
    /* sin opacos: el centro sale de los semitransparentes (3D) */
    reset();
    dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 256;
    g = poly3(ga, gb, gc, 1); g->proj_h = 150.0f; g->proj_ox = 210.0f; g->proj_oy = 100.0f;
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0, "build ofs semi");
    CHECK(near_f(s.filter.cx_col, 210.0f, 1e-3f) && near_f(s.filter.cy_row, 100.0f, 1e-3f) && s.stats.n_ofs_fix == 0, "ref semi %.1f %.1f fix %u", s.filter.cx_col, s.filter.cy_row, s.stats.n_ofs_fix);
    /* sin proyección: valores por defecto (centro del área) y ningún polígono corregido */
    reset();
    dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 256;
    poly3(ga, gb, gc, 0);
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0, "build ofs def");
    CHECK(near_f(s.filter.proj_h, 160.0f, 1e-3f) && near_f(s.filter.cx_col, 160.0f, 1e-3f) && near_f(s.filter.cy_row, 128.0f, 1e-3f) && s.stats.n_ofs_fix == 0, "defecto %.1f %.1f %.1f", s.filter.proj_h, s.filter.cx_col, s.filter.cy_row);
    vr_scene_release();
}

static void test_post_emu(void) {
    /* Modelo del post-proceso (hud_emu_post == vr_fs_post): imagen del ojo 200x480 cuyo valor R codifica la fila (y/h); ojo con cy_img=240, f_eye=400; motor con
     * proj_h=160, centro (160,128). Tiras en las filas original 50..59 con dv=+3, y 60..69 con dv=-2; tinte (0,5; 1; 0,5). */
    enum { W = 200, H = 480 };
    static float src[W * H * 3], dst[W * H * 3];
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { float *p = &src[((size_t)y * W + x) * 3]; p[0] = (y + 0.5f) / H; p[1] = 0.8f; p[2] = 0.4f; }
    VrFilter f; memset(&f, 0, sizeof f);
    f.active = 1; f.proj_h = 160.0f; f.cx_col = 160.0f; f.cy_row = 128.0f; f.cover_abr = -1;
    f.tint[0] = 0.5f; f.tint[1] = 1.0f; f.tint[2] = 0.5f;
    for (int i = 50; i < 60; i++) { f.rowcov[i] = 1; f.dv[i] = 3.0f; }
    for (int i = 60; i < 70; i++) { f.rowcov[i] = 1; f.dv[i] = -2.0f; }
    f.nrows = 70;
    hud_emu_post(src, dst, W, H, 240.0f, 400.0f, &f);
    /* fila de salida y con r = 128 + 160*(y+0,5-240)/400 = 55,5 -> y + 0,5 = 240 - 72,5*2,5 = 58,75: y = 58 (r = 55,4); en el interior de la tira dv=3: fuente = y+0,5 + 2,5*3 = 66,25 */
    {
        int y = 58;
        float r = 128.0f + 160.0f * ((float)y + 0.5f - 240.0f) / 400.0f;
        CHECK(r > 51.0f && r < 58.0f, "r=%.3f", r);
        float want = ((float)y + 0.5f + 400.0f * 3.0f / 160.0f) / H;
        CHECK(near_f(dst[((size_t)y * W + 7) * 3] / 0.5f, want, 1.5f / H), "interior tira dv=3: %.5f vs %.5f", dst[((size_t)y * W + 7) * 3] / 0.5f, want);
        CHECK(near_f(dst[((size_t)y * W + 7) * 3 + 1], 0.8f, 1e-5f) && near_f(dst[((size_t)y * W + 7) * 3 + 2], 0.2f, 1e-5f), "tinte G/B");
    }
    /* r = 64,x -> dv=-2: fuente = y + 0,5 - 2,5*2 */
    {
        int y = 240 + (int)((64.5f - 128.0f) * 400.0f / 160.0f);   /* r ~ 64,5 */
        float r = 128.0f + 160.0f * ((float)y + 0.5f - 240.0f) / 400.0f;
        CHECK(r > 60.6f && r < 68.9f, "r2=%.3f", r);
        float want = ((float)y + 0.5f - 400.0f * 2.0f / 160.0f) / H;
        CHECK(near_f(dst[((size_t)y * W + 3) * 3] / 0.5f, want, 1.5f / H), "interior tira dv=-2: %.5f vs %.5f", dst[((size_t)y * W + 3) * 3] / 0.5f, want);
    }
    /* fuera de las tiras (arriba y abajo del todo, r > 70 o r < 0): sin desplazamiento, solo tinte */
    for (int y = 0; y < H; y += 17) {
        float r = 128.0f + 160.0f * ((float)y + 0.5f - 240.0f) / 400.0f;
        if (r > 71.0f || r < 0.0f) CHECK(near_f(dst[((size_t)y * W) * 3] / 0.5f, (y + 0.5f) / H, 1e-5f), "fuera de tiras y=%d (r=%.1f)", y, r);
    }
    /* parámetros por ojo del Swift (encodePost): fila del punto principal = h/2*(1+P[2][1]) y focal = h/2*P[1][1] con P[1][1]=2/(u+d), P[2][1]=(u-d)/(u+d):
     * el rayo de tangente +u cae en la fila 0 y el de -d en la fila h (tangentes asimétricas) */
    {
        const float up = 0.9f, dn = 0.7f, hh = 1000.0f, sy = 2.0f / (up + dn), cyp = (up - dn) / (up + dn);
        float yc = hh * 0.5f * (1.0f + cyp), fe = hh * 0.5f * sy;
        CHECK(near_f(yc - fe * up, 0.0f, 1e-3f) && near_f(yc + fe * dn, hh, 1e-3f), "parámetros del ojo: y(+u)=%.3f y(-d)=%.3f", yc - fe * up, yc + fe * dn);
    }
    /* sin filas con datos y tinte neutro: identidad; con recubrimiento modo 1 (B+F) suma */
    memset(&f, 0, sizeof f);
    f.proj_h = 160.0f; f.cy_row = 128.0f; f.tint[0] = f.tint[1] = f.tint[2] = 1.0f; f.cover_abr = -1;
    hud_emu_post(src, dst, W, H, 240.0f, 400.0f, &f);
    int same = 1;
    for (int i = 0; i < W * H * 3; i++) if (!near_f(src[i], dst[i], 1e-6f)) same = 0;
    CHECK(same, "identidad sin filtro");
    f.cover_abr = 1; f.cover_rgb[0] = 0.25f; f.cover_rgb[1] = 0.5f; f.cover_rgb[2] = 0.0f;
    hud_emu_post(src, dst, W, H, 240.0f, 400.0f, &f);
    CHECK(near_f(dst[(100 * W) * 3 + 1], 1.0f, 1e-6f) && near_f(dst[(100 * W) * 3 + 2], 0.4f, 1e-6f), "recubrimiento aditivo");
}


/* ---- objetos equipados (clase 3b): gafas (recubrimiento + líneas de exploración), máscara de visión, varios recubrimientos ---- */
static GpuOp *poly2d_quad(int x0, int y0, int x1, int y1, uint16_t flags, int abr, const uint8_t col[4][3]) {
    GpuOp *o = add(GPU_OP_POLY);
    o->nv = 4; o->flags = flags; o->abr = (uint8_t)abr;
    int xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
    for (int k = 0; k < 4; k++) { o->v[k].x = (int16_t)xs[k]; o->v[k].y = (int16_t)ys[k]; o->v[k].r = col[k][0]; o->v[k].g = col[k][1]; o->v[k].b = col[k][2]; }
    o->cx1 = 0; o->cy1 = 0; o->cx2 = 319; o->cy2 = 255;
    return o;
}
static void test_goggles(void) {
    const float R[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    const int32_t e[3] = {0, 3000, 500};
    BridgeState bs; set_eye(&bs, R, e);
    VrSceneOpts o = vr_scene_opts_default(); o.screen_filter = 1;
    VrScene s;
    float a[3] = {0, 0, 2000}, b[3] = {1000, 0, 2000}, c[3] = {0, 1000, 2000};

    /* A) gafas de visión nocturna: tinte aditivo verde (56,120,40) + 128 líneas de 320x1 cada 2 filas, resta 64 */
    reset();
    dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 256;
    rect2d(0, 0, 320, 256, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    poly3(a, b, c, 0);
    rect2d(0, 0, 320, 256, 0, 0, GPU_F_SEMI, 0, 0, 0, 56, 120, 40)->abr = 1;
    for (int i = 0; i < 128; i++) { GpuOp *r = rect2d(0, 254 - 2 * i, 320, 1, 0, 0, GPU_F_SEMI, 0, 0, 0, 64, 64, 64); r->abr = 2; }
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0, "build gafas");
    const VrFilter *f = &s.filter;
    CHECK(f->active && f->n_cover == 1 && f->n_covers == 1 && f->covers_abr[0] == 1 && near_f(f->covers_rgb[0][1], 120 / 255.0f, 1e-4f), "cover NV");
    CHECK(s.stats.n_filter_scan == 128 && f->n_scan == 128 && near_f(f->scan_pitch, 2.0f, 1e-4f) && near_f(f->scan_phase, 0.0f, 1e-4f) && near_f(f->scan_h, 1.0f, 1e-4f) &&
          f->scan_abr == 2 && near_f(f->scan_rgb[0], 64 / 255.0f, 1e-4f), "líneas: n %d pitch %.2f phase %.2f h %.2f abr %d", f->n_scan, f->scan_pitch, f->scan_phase, f->scan_h, f->scan_abr);
    CHECK(!f->frame_on && f->frame == NULL, "sin máscara");
    CHECK(f->scan_after == 1, "las líneas van tras el 1.er recubrimiento: %d", f->scan_after);
    CHECK(s.stats.n_tris2d == 0 && s.stats.n_tris3d == 1, "las líneas no van al panel: tris2d %u", s.stats.n_tris2d);
    for (uint32_t i = 0; i < s.nb; i++) CHECK(s.b[i].kind != 2, "sin batches 2D");
    /* sin screen_filter las líneas y el tinte vuelven a la clase 2 (comportamiento anterior) */
    { VrSceneOpts o0 = vr_scene_opts_default();
      CHECK(vr_scene_build(&dl, &bs, &o0, &s) == 0 && s.stats.n_filter_scan == 0 && !s.filter.active && s.filter.n_scan == 0 && s.stats.n_tris2d == 2 * 129, "screen_filter=0: tris2d %u", s.stats.n_tris2d); }

    /* A0) objetos que solo se ven con las gafas/el tabaco (haz de un sensor: polígono 3D semitransparente cuyo color modula el juego, 0 = invisible, 128 = neutro): se emiten en los dos casos */
    for (int pass = 0; pass < 2; pass++) {
        reset();
        dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 256;
        GpuOp *beam = poly3(a, b, c, 1);
        for (int i = 0; i < 3; i++) beam->v[i].r = beam->v[i].g = beam->v[i].b = (uint8_t)(pass ? 128 : 0);
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 1 && s.nb == 1 && s.b[0].kind == 1, "haz semitransparente emitido (color %d)", pass ? 128 : 0);
        CHECK(s.nv == 3 && (s.v[0].rgba & 255) == (uint32_t)(pass ? 128 : 0), "color del haz %u", s.v[0].rgba & 255);
    }

    /* A2) mira de las gafas en primera persona: tinte, líneas y DESPUÉS un velo gris (B/2+F/2, poligono plano a pantalla completa) -> orden tinte < líneas < velo */
    reset();
    dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 256;
    poly3(a, b, c, 0);
    rect2d(0, 0, 320, 256, 0, 0, GPU_F_SEMI, 0, 0, 0, 56, 120, 40)->abr = 1;
    for (int i = 0; i < 128; i++) { GpuOp *r = rect2d(0, 254 - 2 * i, 320, 1, 0, 0, GPU_F_SEMI, 0, 0, 0, 64, 64, 64); r->abr = 2; }
    { static const uint8_t G[4][3] = {{85, 85, 85}, {85, 85, 85}, {85, 85, 85}, {85, 85, 85}}; poly2d_quad(0, 0, 320, 256, GPU_F_QUAD | GPU_F_SEMI, 0, G); }
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.filter.n_covers == 2 && s.filter.scan_after == 1 && s.filter.n_scan == 128 && s.filter.covers_abr[1] == 0, "orden tinte/líneas/velo: covers %d after %d", s.filter.n_covers, s.filter.scan_after);
    { static float src2[64 * 64 * 3], dst2[64 * 64 * 3];
      for (int i = 0; i < 64 * 64 * 3; i++) src2[i] = 0.5f;
      VrFilter ff = s.filter; ff.proj_h = 160.0f; ff.cx_col = 160.0f; ff.cy_row = 128.0f;
      hud_emu_post2(src2, dst2, 64, 64, 32.0f, 32.0f, 320.0f, 320.0f, &ff);   /* cada fila del ojo = 0,5 fila original: periodo 4 px, centro en la fila 128 original */
      /* fila 32 del ojo (+0,5): r = 128 + 0,5/2 = 128,25 -> t = fract(64,125) = 0,125: sobre línea; fila 34: r = 129,25 -> entre líneas */
      float g_on = 0.5f + 120 / 255.0f; g_on = g_on - 64 / 255.0f; g_on = 0.5f * g_on + 0.5f * (85 / 255.0f);
      float g_off = 0.5f + 120 / 255.0f; g_off = 0.5f * g_off + 0.5f * (85 / 255.0f);
      CHECK(near_f(dst2[(32 * 64 + 10) * 3 + 1], g_on, 2e-3f) && near_f(dst2[(34 * 64 + 10) * 3 + 1], g_off, 2e-3f), "velo gris tras las líneas: %.4f (%.4f) / %.4f (%.4f)", dst2[(32 * 64 + 10) * 3 + 1], g_on, dst2[(34 * 64 + 10) * 3 + 1], g_off); }

    /* B) tiras sueltas (< 8) no son un efecto de pantalla; periodo irregular: se toma el menor; fase con origen de la imagen desplazado */
    reset();
    dl.disp.x = 320; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 256;
    for (int i = 0; i < 5; i++) { GpuOp *r = rect2d(320, 100 + 3 * i, 320, 1, 0, 0, GPU_F_SEMI, 0, 0, 0, 64, 64, 64); r->abr = 2; r->cx1 = 320; r->cx2 = 639; }
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.filter.n_scan == 0, "5 tiras: n_scan %d", s.filter.n_scan);
    reset();
    dl.disp.x = 320; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 256;
    for (int i = 0; i < 20; i++) { GpuOp *r = rect2d(320, 101 + 3 * i, 320, 1, 0, 0, GPU_F_SEMI, 0, 0, 0, 40, 40, 40); r->abr = 2; r->cx1 = 320; r->cx2 = 639; }
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.filter.n_scan == 20 && near_f(s.filter.scan_pitch, 3.0f, 1e-4f) && near_f(s.filter.scan_phase, 2.0f, 1e-4f), "periodo 3 fase 2: n %d pitch %.2f phase %.2f", s.filter.n_scan, s.filter.scan_pitch, s.filter.scan_phase);

    /* C) varios recubrimientos en orden: el tinte de las gafas y un destello rojo (el último gana en cover_*; los dos van en covers_*) */
    reset();
    dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 256;
    poly3(a, b, c, 0);
    rect2d(0, 0, 320, 256, 0, 0, GPU_F_SEMI, 0, 0, 0, 56, 120, 40)->abr = 1;
    rect2d(0, 0, 320, 256, 0, 0, GPU_F_SEMI, 0, 0, 0, 90, 0, 0)->abr = 1;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.filter.n_covers == 2 && s.filter.n_cover == 2 && near_f(s.filter.covers_rgb[0][1], 120 / 255.0f, 1e-4f) &&
          near_f(s.filter.covers_rgb[1][0], 90 / 255.0f, 1e-4f) && near_f(s.filter.cover_rgb[0], 90 / 255.0f, 1e-4f), "dos recubrimientos");
    reset();
    dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 256;
    poly3(a, b, c, 0);   /* con 3D: sin él los velos pasan a la clase 2 (veil_as_2d implícito) */
    for (int i = 0; i < 6; i++) rect2d(0, 0, 320, 256, 0, 0, GPU_F_SEMI, 0, 0, 0, (uint8_t)(10 * (i + 1)), 0, 0)->abr = (uint8_t)(i & 3);
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.filter.n_covers == VR_FILTER_COVERS && near_f(s.filter.covers_rgb[0][0], 30 / 255.0f, 1e-4f) && near_f(s.filter.covers_rgb[3][0], 60 / 255.0f, 1e-4f) &&
          s.filter.covers_abr[0] == 2 && s.filter.covers_abr[3] == 1, "6 recubrimientos: se conservan los 4 últimos (n %d, [0]=%.0f abr %d)", s.filter.n_covers, s.filter.covers_rgb[0][0] * 255, s.filter.covers_abr[0]);

    /* D) máscara de visión: agujero rectangular x 100..220, y 80..180 (centro 160,130), 10 planos de restar blanco / negro opaco alrededor y 10 degradados de resta en el borde interior */
    static const uint8_t WHITE[4][3] = {{255, 255, 255}, {255, 255, 255}, {255, 255, 255}, {255, 255, 255}};
    static const uint8_t BLACK[4][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    for (int variant = 0; variant < 2; variant++) {   /* 0: planos de restar blanco (semitransparente, modo 2); 1: planos opacos negros */
        reset();
        dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 256;
        poly3(a, b, c, 0);
        uint16_t ff = (uint16_t)(variant == 0 ? GPU_F_QUAD | GPU_F_SEMI : GPU_F_QUAD);
        const uint8_t (*col)[3] = variant == 0 ? WHITE : BLACK;
        for (int i = 0; i < 4; i++) { poly2d_quad(80 * i, 0, 80 * (i + 1), 80, ff, 2, col); poly2d_quad(80 * i, 180, 80 * (i + 1), 256, ff, 2, col); }
        poly2d_quad(0, 80, 100, 180, ff, 2, col); poly2d_quad(220, 80, 320, 180, ff, 2, col);
        /* degradado interior de 10 px: 255 pegado al agujero... al borde del plano (resta total) -> 0 en el interior (arriba y abajo en 3 trozos, izq/der: 2) */
        static const uint8_t GT[4][3] = {{255, 255, 255}, {255, 255, 255}, {0, 0, 0}, {0, 0, 0}};   /* arriba: vértices 0,1 en y0 (borde), 2,3 en y1 */
        static const uint8_t GB[4][3] = {{0, 0, 0}, {0, 0, 0}, {255, 255, 255}, {255, 255, 255}};
        for (int i = 0; i < 3; i++) { poly2d_quad(100 + 40 * i, 80, 140 + 40 * i, 90, GPU_F_QUAD | GPU_F_GOURAUD | GPU_F_SEMI, 2, GT); poly2d_quad(100 + 40 * i, 170, 140 + 40 * i, 180, GPU_F_QUAD | GPU_F_GOURAUD | GPU_F_SEMI, 2, GB); }
        static const uint8_t GL[4][3] = {{255, 255, 255}, {0, 0, 0}, {255, 255, 255}, {0, 0, 0}}, GR[4][3] = {{0, 0, 0}, {255, 255, 255}, {0, 0, 0}, {255, 255, 255}};
        for (int i = 0; i < 2; i++) { poly2d_quad(100, 90 + 40 * i, 110, 130 + 40 * i, GPU_F_QUAD | GPU_F_GOURAUD | GPU_F_SEMI, 2, GL); poly2d_quad(210, 90 + 40 * i, 220, 130 + 40 * i, GPU_F_QUAD | GPU_F_GOURAUD | GPU_F_SEMI, 2, GR); }
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0, "build máscara");
        f = &s.filter;
        CHECK(f->active && f->frame_on && f->frame && f->frame_w == VR_FRAME_W && f->frame_h == VR_FRAME_H, "máscara activa (variante %d)", variant);
        CHECK(s.stats.n_filter_frame == 10 + 10 && s.stats.n_tris2d == 0 && s.stats.n_tris3d == 1, "polígonos de la máscara %u (tris2d %u)", s.stats.n_filter_frame, s.stats.n_tris2d);
        for (uint32_t i = 0; i < s.nb; i++) CHECK(s.b[i].kind != 2, "la máscara no va al panel");
        if (f->frame) {
            #define FR(px, py) f->frame[(int)((py) / 2) * VR_FRAME_W + (int)((px) / 2)]
            CHECK(FR(160, 130) == 0, "centro del agujero abierto: %d", FR(160, 130));
            CHECK(FR(10, 10) == 255 && FR(300, 250) == 255 && FR(40, 130) == 255 && FR(280, 130) == 255 && FR(160, 40) == 255 && FR(160, 220) == 255, "fuera del agujero cerrado");
            CHECK(FR(160, 81) >= 200 && FR(160, 89) <= 60 && FR(160, 85) > 60 && FR(160, 85) < 200, "degradado superior %d %d %d", FR(160, 81), FR(160, 85), FR(160, 89));
            CHECK(FR(101, 130) >= 200 && FR(109, 130) <= 60 && FR(219, 130) >= 200 && FR(211, 130) <= 60, "degradados laterales %d %d", FR(101, 130), FR(109, 130));
        }
    }
    /* sin screen_filter no se detecta; y con pocos polígonos de resta (< 8 de cada) no es una máscara (HUD normal con degradados) */
    { VrSceneOpts o0 = vr_scene_opts_default(); CHECK(vr_scene_build(&dl, &bs, &o0, &s) == 0 && !s.filter.frame_on && s.stats.n_filter_frame == 0, "screen_filter=0: sin máscara"); }
    reset();
    dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 256;
    for (int i = 0; i < 6; i++) { poly2d_quad(10 + 40 * i, 10, 40 + 40 * i, 30, GPU_F_QUAD | GPU_F_SEMI, 2, WHITE); poly2d_quad(10 + 40 * i, 40, 40 + 40 * i, 60, GPU_F_QUAD | GPU_F_GOURAUD | GPU_F_SEMI, 2, WHITE); }
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && !s.filter.frame_on && s.stats.n_filter_frame == 0 && s.stats.n_tris2d == 24, "6+6 polígonos: HUD normal (tris2d %u)", s.stats.n_tris2d);
    /* la máscara con origen de imagen desplazado (disp.x=320) cae en las mismas celdas */
    reset();
    dl.disp.x = 320; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 256;
    for (int i = 0; i < 8; i++) poly2d_quad(320 + 40 * i, 0, 320 + 40 * (i + 1), 100, GPU_F_QUAD | GPU_F_SEMI, 2, WHITE);
    for (int i = 0; i < 8; i++) poly2d_quad(320 + 40 * i, 100, 320 + 40 * (i + 1), 110, GPU_F_QUAD | GPU_F_GOURAUD | GPU_F_SEMI, 2, WHITE);
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.filter.frame_on && s.filter.frame[(50 / 2) * VR_FRAME_W + 100 / 2] == 255 && s.filter.frame[(200 / 2) * VR_FRAME_W + 100 / 2] == 0, "máscara con disp.x=320");
    vr_scene_release();
}


/* emulación del post-proceso con la clase 3b (hud_emu_post2 == vr_fs_post): gafas (tinte + líneas) y máscara de visión */
static void test_goggles_emu(void) {
    const float R[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    const int32_t e[3] = {0, 3000, 500};
    BridgeState bs; set_eye(&bs, R, e);
    VrSceneOpts o = vr_scene_opts_default(); o.screen_filter = 1;
    VrScene s;
    float a[3] = {0, 0, 2000}, b[3] = {1000, 0, 2000}, c[3] = {0, 1000, 2000};
    enum { W = 640, H = 512 };    /* ojo con el doble de resolución que la imagen del motor y su mismo campo: x_ojo = 2*x_motor, f = 320 */
    static float src[W * H * 3], dst[W * H * 3];
    for (int i = 0; i < W * H * 3; i++) src[i] = 0.5f;
    /* gafas nocturnas */
    reset();
    dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 256;
    poly3(a, b, c, 0);
    rect2d(0, 0, 320, 256, 0, 0, GPU_F_SEMI, 0, 0, 0, 56, 120, 40)->abr = 1;
    for (int i = 0; i < 128; i++) { GpuOp *r = rect2d(0, 254 - 2 * i, 320, 1, 0, 0, GPU_F_SEMI, 0, 0, 0, 64, 64, 64); r->abr = 2; }
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.filter.n_scan == 128, "build gafas (emu)");
    s.filter.proj_h = 160.0f; s.filter.cx_col = 160.0f; s.filter.cy_row = 128.0f;
    hud_emu_post2(src, dst, W, H, 320.0f, 256.0f, 320.0f, 320.0f, &s.filter);
    {   /* fila 200 del ojo -> fila original r = 128 + (200,5-256)/2 = 100,25: dentro de una línea (filas pares); fila 202 -> r = 101,25: entre líneas */
        const float *on = &dst[((size_t)200 * W + 300) * 3], *off = &dst[((size_t)202 * W + 300) * 3];
        float r0 = 0.5f + 56 / 255.0f, g0 = 0.5f + 120 / 255.0f, b0 = 0.5f + 40 / 255.0f;
        CHECK(near_f(off[0], r0, 1e-4f) && near_f(off[1], g0, 1e-4f) && near_f(off[2], b0, 1e-4f), "entre líneas: tinte aditivo %.3f %.3f %.3f", off[0], off[1], off[2]);
        CHECK(near_f(on[0], r0 - 64 / 255.0f, 1e-3f) && near_f(on[1], g0 - 64 / 255.0f, 1e-3f) && near_f(on[2], b0 - 64 / 255.0f, 1e-3f), "sobre línea: resta 64 %.3f %.3f %.3f", on[0], on[1], on[2]);
    }
    /* con las líneas más finas que ~1 px del ojo (periodo = 1 px) se usa la media (alto/periodo = 0,5): resta media 32 */
    hud_emu_post2(src, dst, 320, 256, 160.0f, 128.0f, 80.0f, 80.0f, &s.filter);
    {
        const float *p0 = &dst[((size_t)100 * 320 + 50) * 3], *p1 = &dst[((size_t)101 * 320 + 50) * 3];
        CHECK(near_f(p0[0], p1[0], 1e-4f) && near_f(p0[0], 0.5f + 56 / 255.0f - 32 / 255.0f, 2e-3f), "periodo de 2 px: media %.4f %.4f", p0[0], p1[0]);
    }
    /* sin líneas (n_scan = 0) y sin recubrimientos: identidad */
    { VrFilter f; memset(&f, 0, sizeof f); f.proj_h = 160.0f; f.cy_row = 128.0f; f.cx_col = 160.0f; f.tint[0] = f.tint[1] = f.tint[2] = 1.0f; f.cover_abr = -1;
      hud_emu_post2(src, dst, W, H, 320.0f, 256.0f, 320.0f, 320.0f, &f);
      int same = 1; for (int i = 0; i < W * H * 3; i++) if (!near_f(src[i], dst[i], 1e-6f)) same = 0;
      CHECK(same, "post2 identidad"); }
    /* máscara de visión: agujero x 100..240 (asimétrico), y 80..180 con el mismo campo; ojo 640x512 a doble resolución */
    static const uint8_t WHITE[4][3] = {{255, 255, 255}, {255, 255, 255}, {255, 255, 255}, {255, 255, 255}};
    static const uint8_t GT[4][3] = {{255, 255, 255}, {255, 255, 255}, {0, 0, 0}, {0, 0, 0}};
    reset();
    dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 256;
    poly3(a, b, c, 0);
    for (int i = 0; i < 4; i++) { poly2d_quad(80 * i, 0, 80 * (i + 1), 80, GPU_F_QUAD | GPU_F_SEMI, 2, WHITE); poly2d_quad(80 * i, 180, 80 * (i + 1), 256, GPU_F_QUAD | GPU_F_SEMI, 2, WHITE); }
    poly2d_quad(0, 80, 100, 180, GPU_F_QUAD | GPU_F_SEMI, 2, WHITE); poly2d_quad(240, 80, 320, 180, GPU_F_QUAD | GPU_F_SEMI, 2, WHITE);
    for (int i = 0; i < 3; i++) { poly2d_quad(100 + 46 * i, 80, 146 + 46 * i, 90, GPU_F_QUAD | GPU_F_GOURAUD | GPU_F_SEMI, 2, GT); poly2d_quad(100 + 46 * i, 170, 146 + 46 * i, 180, GPU_F_QUAD | GPU_F_GOURAUD | GPU_F_SEMI, 2, GT); }
    for (int i = 0; i < 2; i++) { poly2d_quad(100, 90 + 40 * i, 110, 130 + 40 * i, GPU_F_QUAD | GPU_F_GOURAUD | GPU_F_SEMI, 2, GT); poly2d_quad(230, 90 + 40 * i, 240, 130 + 40 * i, GPU_F_QUAD | GPU_F_GOURAUD | GPU_F_SEMI, 2, GT); }
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.filter.frame_on && s.filter.frame_rgb[0] == 0.0f, "build máscara (emu): interior negro sin caja");
    bs.item = 3; bs.valid |= BRIDGE_OK_EQUIP;   /* caja equipada: el interior cerrado es cartón oscuro */
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.filter.frame_on && near_f(s.filter.frame_rgb[0], 0.12f, 1e-6f) && s.filter.frame_rgb[2] > 0.0f, "build máscara (emu): interior de cartón con la caja");
    s.filter.proj_h = 160.0f; s.filter.cx_col = 160.0f; s.filter.cy_row = 128.0f;
    hud_emu_post2(src, dst, W, H, 320.0f, 256.0f, 320.0f, 320.0f, &s.filter);
    #define PX(x, y) (&dst[((size_t)(y) * W + (x)) * 3])
    CHECK(near_f(PX(320, 260)[1], 0.5f, 1e-4f), "centro abierto: %.3f", PX(320, 260)[1]);          /* canvas (160,130) */
    CHECK(near_f(PX(20, 20)[0], 0.12f, 1e-4f) && near_f(PX(600, 500)[2], 0.055f, 1e-4f) && near_f(PX(100, 260)[0], 0.12f, 1e-4f), "fuera del agujero: interior de la caja");
    CHECK(near_f(PX(2 * 225, 260)[0], 0.5f, 1e-4f) && near_f(PX(2 * 95, 260)[0], 0.12f, 1e-4f) && near_f(PX(2 * 255, 260)[0], 0.12f, 1e-4f), "agujero asimétrico: abierto en x=225, cerrado en 95 y 255");
    CHECK(near_f(PX(320, 176)[0], 0.3f, 0.03f), "degradado superior (canvas y=88: resta 0,2): %.3f", PX(320, 176)[0]);
    /* fuera de la imagen del motor (campo del ojo mayor que el del motor): resta total. Ojo 2x: la imagen del motor ocupa todo; con la misma cámara pero focal menor (160) cubre 1280x1024 virtuales */
    hud_emu_post2(src, dst, 320, 256, 160.0f, 128.0f, 80.0f, 80.0f, &s.filter);   /* focal 80: el ojo ve el doble de campo (tangente ±2): fuera de la imagen del motor = negro */
    CHECK(near_f(dst[((size_t)5 * 320 + 5) * 3], 0.12f, 1e-4f) && near_f(dst[((size_t)128 * 320 + 160) * 3], 0.5f, 1e-4f) && near_f(dst[((size_t)128 * 320 + 60) * 3], 0.12f, 1e-4f),
          "campo mayor que el del motor: negro fuera (centro %.3f)", dst[((size_t)128 * 320 + 160) * 3]);
    /* fidelidad con la resta del original sobre una escena cualquiera: out = max(c - resta, 0) en cada canal */
    {
        for (int i = 0; i < W * H * 3; i++) src[i] = 0.9f;
        hud_emu_post2(src, dst, W, H, 320.0f, 256.0f, 320.0f, 320.0f, &s.filter);
        float want = 0.9f - s.filter.frame[(int)(84 / 2) * VR_FRAME_W + (int)(160 / 2)] / 255.0f;   /* canvas (160,84): 0,9 - resta */
        CHECK(near_f(PX(320, 168)[0], want < 0 ? 0 : want, 0.12f), "resta en el degradado: %.3f vs %.3f", PX(320, 168)[0], want);
    }
    vr_scene_release();
}

static void test_recon_fate(void) {
    /* quad con 3 vértices fiables y el 4º sin posición de vista: se reconstruye (plano z=2000, rayo de pantalla) */
    reset();
    GpuOp *q = add(GPU_OP_POLY);
    q->nv = 4; q->flags = GPU_F_TEXTURED; q->tp = 1; q->tx = 320; q->ty = 256;
    q->proj_h = 150.0f; q->proj_ox = 160.0f; q->proj_oy = 128.0f;
    setv3(&q->v[0], -500, -300, 2000); setv3(&q->v[1], 500, -300, 2000); setv3(&q->v[2], -500, 400, 2000);
    q->v[3].x = 190; q->v[3].y = 158;   /* 160 + 150*400/2000, 128 + 150*400/2000 */
    /* otro quad igual pero con el 4º vértice saturado en pantalla: no se reconstruye */
    GpuOp *q2 = add(GPU_OP_POLY);
    *q2 = *q; q2->v[3].x = 2000;
    /* triángulo con un vértice detrás del plano del motor (z<0): sin recorte del motor se emite entero */
    float a[3] = {-500, 0, -300}, b[3] = {500, 0, 1500}, c[3] = {0, 500, 1500};
    poly3(a, b, c, 0);
    uint8_t fate[8];
    VrScene s; VrSceneOpts o = vr_scene_opts_default();
    vr_scene_set_fate(fate, sizeof fate);
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0, "build recon");
    CHECK(s.stats.n_recon == 1, "n_recon %u", s.stats.n_recon);
    CHECK(fate[0] == VR_FATE_3D, "destino quad reconstruido %u", fate[0]);
    CHECK(fate[1] == VR_FATE_PARTIAL, "destino quad saturado %u", fate[1]);
    CHECK(fate[2] == VR_FATE_3D && s.stats.n_unclipped == 1, "detrás: destino %u unclipped %u", fate[2], s.stats.n_unclipped);
    int found = 0;   /* vértice reconstruido (400,400,2000) -> VR (0,4; -0,4; -2) */
    for (uint32_t i = 0; i < s.nv; i++)
        if (near_f(s.v[i].pos[0], 0.4f, 1e-3f) && near_f(s.v[i].pos[1], -0.4f, 1e-3f) && near_f(s.v[i].pos[2], -2.0f, 1e-3f)) found++;
    CHECK(found >= 1, "vértice reconstruido ausente (%d)", found);
    CHECK(s.nb >= 1 && s.stats.n_tris3d == 3, "triángulos %u", s.stats.n_tris3d);
    o.recon_partial = 0;
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.stats.n_recon == 0 && fate[0] == VR_FATE_PARTIAL, "sin recon: destino %u", fate[0]);
    vr_scene_set_fate(NULL, 0);
    vr_scene_release();
}

static void test_recon_diag_ambig(void) {
    /* resplandor: solo las esquinas v0 y v3 (diagonal) tienen posición; v1 = (x3,y0), v2 = (x0,y3) a z constante */
    reset();
    GpuOp *q = add(GPU_OP_POLY);
    q->nv = 4; q->flags = GPU_F_SEMI; q->abr = 1; q->tp = 1;
    q->proj_h = 150.0f; q->proj_ox = 160.0f; q->proj_oy = 128.0f;
    setv3(&q->v[0], -1000, -1000, 555); setv3(&q->v[3], 1000, 1000, 555);
    /* AMBIG+SAT coherente (rectángulo) y no coherente (z de un vértice muy distinto) */
    GpuOp *r1 = add(GPU_OP_POLY);
    r1->nv = 4; r1->flags = GPU_F_TEXTURED; r1->tp = 1;
    setv3(&r1->v[0], -3706, -1898, 3689); setv3(&r1->v[1], -2955, -1898, 186); setv3(&r1->v[2], -2955, -1898, 3689); setv3(&r1->v[3], -3706, -1898, 186);
    for (int k = 0; k < 4; k++) r1->v[k].vflags = GPU_VF_AMBIG | GPU_VF_SAT;
    GpuOp *r2 = add(GPU_OP_POLY);
    *r2 = *r1; r2->v[1].vy = -2649; r2->v[1].vz = -502; r2->v[2].vx = -1000;
    /* cuadrilátero con v3 == v1: el primer triángulo es válido pero el segundo es degenerado -> no es coherente, se descarta */
    GpuOp *r3 = add(GPU_OP_POLY);
    *r3 = *r1; r3->v[3] = r3->v[1];
    uint8_t fate[4];
    VrScene s; VrSceneOpts o = vr_scene_opts_default();
    vr_scene_set_fate(fate, sizeof fate);
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0, "build diag");
    CHECK(s.stats.n_recon == 1 && s.stats.n_tris3d >= 4, "diag: recon %u tris %u", s.stats.n_recon, s.stats.n_tris3d);
    CHECK(fate[0] == VR_FATE_3D && fate[1] == VR_FATE_3D && fate[2] == VR_FATE_AMBIG, "destinos %u %u %u", fate[0], fate[1], fate[2]);
    CHECK(fate[3] == VR_FATE_AMBIG, "quad con 2.º triángulo degenerado: destino %u", fate[3]);
    o.recon_ambig = 0;
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && fate[1] == VR_FATE_AMBIG, "sin recon_ambig: %u", fate[1]);
    vr_scene_set_fate(NULL, 0);
    vr_scene_release();
}

/* Cartel de efectos con DOS esquinas limpias (vvalid, sin banderas) en diagonal y las otras dos dudosas (AMBIG/SAT, basura) o sin vista: las otras dos se derivan de las limpias (DERIVED)
 * en vez de descartar el polígono (parcial / AMBIG), y billboard_quad las acepta. Con 3 o 4 limpias no cambia nada. */
static GpuOp *glow_quad(const uint8_t fl[4], const uint8_t vv[4]) {
    GpuOp *q = add(GPU_OP_POLY);
    q->nv = 4; q->flags = GPU_F_SEMI; q->abr = 1; q->tp = 1;
    q->proj_h = 150.0f; q->proj_ox = 160.0f; q->proj_oy = 128.0f;
    const float X[4][3] = {{-500, -500, 555}, {7000, -3000, 9000}, {-4000, 6000, 20}, {500, 500, 555}};
    for (int k = 0; k < 4; k++) { setv3(&q->v[k], X[k][0], X[k][1], X[k][2]); q->v[k].vflags = fl[k]; q->v[k].vvalid = vv[k]; }
    return q;
}
static void glow_sxy(GpuOp *q) {   /* SXY coherente con el modelo de la GTE (solo para las esquinas con posiciones sensatas) */
    for (int k = 0; k < 4; k++) { q->v[k].x = (int16_t)lrintf(q->proj_ox + q->proj_h * q->v[k].vx / q->v[k].vz); q->v[k].y = (int16_t)lrintf(q->proj_oy + q->proj_h * q->v[k].vy / q->v[k].vz); }
}
static void test_diag_dirty(void) {
    const uint8_t on4[4] = {1, 1, 1, 1}, A_S = GPU_VF_AMBIG | GPU_VF_SAT;
    uint8_t fate[1];
    VrScene s; VrSceneOpts o = vr_scene_opts_default();
    o.billboard = 1;
    vr_scene_set_fate(fate, sizeof fate);
    /* a) v0 y v3 limpias, v1 y v2 con vista AMBIG+SAT basura: antes se descartaban (AMBIG); ahora cartel derivado */
    { const uint8_t f[4] = {0, A_S, A_S, 0}; reset(); glow_quad(f, on4); }
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && fate[0] == VR_FATE_3D && s.stats.n_recon == 1 && s.stats.n_tris3d == 2 && s.stats.n_billboard == 1 && s.stats.drop_ambig == 0,
          "diag v0,v3 limpias + 2 AMBIG: fate %u recon %u tris %u bb %u ambig %u", fate[0], s.stats.n_recon, s.stats.n_tris3d, s.stats.n_billboard, s.stats.drop_ambig);
    /* b) v0 y v3 limpias, v1 sin vista y v2 con vista dudosa: antes parcial; ahora cartel derivado */
    { const uint8_t f[4] = {0, 0, GPU_VF_SAT, 0}, v[4] = {1, 0, 1, 1}; reset(); glow_quad(f, v); }
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && fate[0] == VR_FATE_3D && s.stats.drop_partial == 0 && s.stats.n_tris3d == 2 && s.stats.n_billboard == 1,
          "diag v0,v3 limpias + 1 sin vista + 1 SAT: fate %u partial %u tris %u bb %u", fate[0], s.stats.drop_partial, s.stats.n_tris3d, s.stats.n_billboard);
    /* c) la otra diagonal (v1,v2) */
    { const uint8_t f[4] = {A_S, 0, 0, GPU_VF_BEHIND}; reset(); GpuOp *q = glow_quad(f, on4);
      setv3(&q->v[1], 500, -500, 555); setv3(&q->v[2], -500, 500, 555); q->v[0].vflags = A_S; q->v[3].vflags = GPU_VF_BEHIND; }
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && fate[0] == VR_FATE_3D && s.stats.n_recon == 1 && s.stats.n_billboard == 1, "diag v1,v2 limpias: fate %u recon %u bb %u", fate[0], s.stats.n_recon, s.stats.n_billboard);
    /* d) 3 limpias + 1 dudosa (SAT): sin relleno */
    { const uint8_t f[4] = {0, 0, GPU_VF_SAT, 0}; reset(); GpuOp *q = glow_quad(f, on4); setv3(&q->v[1], 500, -500, 555); setv3(&q->v[2], -500, 500, 555); q->v[2].vflags = GPU_VF_SAT; setv3(&q->v[3], 500, 500, 555); glow_sxy(q); }
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.stats.n_recon == 0 && s.stats.n_billboard == 0 && s.stats.n_tris3d == 2, "3 limpias: sin relleno (recon %u bb %u tris %u)", s.stats.n_recon, s.stats.n_billboard, s.stats.n_tris3d);
    /* e) 4 limpias: sin relleno */
    { const uint8_t f[4] = {0, 0, 0, 0}; reset(); GpuOp *q = glow_quad(f, on4); setv3(&q->v[1], 500, -500, 555); setv3(&q->v[2], -500, 500, 555); glow_sxy(q); }
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.stats.n_recon == 0 && s.stats.n_tris3d == 2, "4 limpias: sin relleno (recon %u tris %u)", s.stats.n_recon, s.stats.n_tris3d);
    /* f) 2 limpias NO en diagonal (v0,v1): no se rellena; el parcial/AMBIG se descarta como antes */
    { const uint8_t f[4] = {0, 0, A_S, A_S}; reset(); glow_quad(f, on4); }
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.stats.n_recon == 0 && fate[0] == VR_FATE_AMBIG, "2 limpias en arista: sin relleno (recon %u fate %u)", s.stats.n_recon, fate[0]);
    /* h) quad OPACO (sin SEMI) con 2 limpias en diagonal y las otras dos dudosas: la regla es solo de carteles de efectos (SEMI); el opaco sigue como antes (AMBIG descartado, sin relleno) */
    { const uint8_t f[4] = {0, A_S, A_S, 0}; reset(); GpuOp *q = glow_quad(f, on4); q->flags = 0; }
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.stats.n_recon == 0 && fate[0] == VR_FATE_AMBIG, "opaco, diag limpias + 2 AMBIG: sin relleno (recon %u fate %u)", s.stats.n_recon, fate[0]);
    { const uint8_t f[4] = {0, 0, GPU_VF_SAT, 0}, v[4] = {1, 0, 1, 1}; reset(); GpuOp *q = glow_quad(f, v); q->flags = 0; }
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.stats.n_recon == 0 && s.stats.drop_partial == 1 && fate[0] == VR_FATE_PARTIAL, "opaco, diag limpias + 1 sin vista + 1 SAT: parcial como antes (recon %u partial %u fate %u)", s.stats.n_recon, s.stats.drop_partial, fate[0]);
    /* g) 2 limpias en diagonal y las otras dos YA exactamente DERIVED (rellenadas antes): no se rellena de nuevo ni cuenta como RECON */
    { const uint8_t f[4] = {0, GPU_VF_DERIVED, GPU_VF_DERIVED, 0}; reset(); GpuOp *q = glow_quad(f, on4); setv3(&q->v[1], 500, -500, 555); setv3(&q->v[2], -500, 500, 555); glow_sxy(q); }
    o.drop_derived = 0;
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && fate[0] == VR_FATE_3D && s.stats.n_recon == 0 && s.stats.n_tris3d == 2,
          "diag limpias + 2 ya DERIVED: sin relleno (recon %u fate %u tris %u)", s.stats.n_recon, fate[0], s.stats.n_tris3d);
    vr_scene_set_fate(NULL, 0);
    vr_scene_release();
}

/* AMBIG junto a SAT/BEHIND en un cuadrilátero no coherente: si UN solo vértice es AMBIG y los otros tres son fiables (sin banderas, o solo SAT con una coordenada
 * sin saturar que reproduce la GTE), el paralelogramo v_a + v_b - v_opuesto lo reconstruye (si reproduce su SXY saturado); si no, se descarta. */
static GpuOp *ambig_quad(const float P[4][3], const uint8_t fl[4], const int sxy[4][2]) {
    GpuOp *q = add(GPU_OP_POLY);
    q->nv = 4; q->flags = GPU_F_TEXTURED; q->tp = 1; q->tx = 320; q->ty = 256;
    q->proj_h = 150.0f; q->proj_ox = 0.0f; q->proj_oy = 0.0f;
    for (int k = 0; k < 4; k++) {
        setv3(&q->v[k], P[k][0], P[k][1], P[k][2]);
        q->v[k].vflags = fl[k]; if (fl[k]) q->v[k].fvalid = 0;
        q->v[k].x = (int16_t)sxy[k][0]; q->v[k].y = (int16_t)sxy[k][1];
        q->v[k].u = (uint8_t)(20 * (k & 1)); q->v[k].v = (uint8_t)(20 * (k >> 1)); q->v[k].r = q->v[k].g = q->v[k].b = 128;
    }
    return q;
}
static void test_recon_ambig_quad(void) {
    VrScene s; VrSceneOpts o = vr_scene_opts_default();
    uint8_t fate[4];
    vr_scene_set_fate(fate, sizeof fate);
    /* v0=(1000,-300,1000) v1=(4000,-300,1000) v2=(1000,400,200); el v3 verdadero (4000,400,200) satura en x (0.75*4000 = 3000 > 1023). Los x,y del paquete miden <= 1023 de ancho: si no, la GPU de PS1 descartaría el cuadrilátero */
    const int sxy[4][2] = {{150, -45}, {600, -45}, {750, 300}, {1023, 300}};
    const uint8_t fj[4] = {0, 0, 0, GPU_VF_AMBIG | GPU_VF_SAT | GPU_VF_BEHIND};
    float P[4][3] = {{1000, -300, 1000}, {4000, -300, 1000}, {1000, 400, 200}, {50000, -90000, -2996}};
    /* a) un AMBIG, tres limpios: reconstruido (2 triángulos, el vértice verdadero en VR (4,0; -0,4; -0,2)) */
    reset(); ambig_quad(P, fj, sxy);
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.stats.n_tris3d == 2 && s.stats.n_recon == 1 && s.stats.drop_ambig == 0, "a) tris %u recon %u ambig %u", s.stats.n_tris3d, s.stats.n_recon, s.stats.drop_ambig);
    CHECK(fate[0] == VR_FATE_3D, "a) destino %u", fate[0]);
    int found = 0;
    for (uint32_t i = 0; i < s.nv; i++) if (near_f(s.v[i].pos[0], 4.0f, 2e-3f) && near_f(s.v[i].pos[1], -0.4f, 2e-3f) && near_f(s.v[i].pos[2], -0.2f, 2e-3f)) found++;
    CHECK(found >= 1, "a) vértice reconstruido ausente (%d)", found);
    /* b) dos AMBIG: no hay reconstrucción -> descartado */
    { uint8_t f2[4] = {0, GPU_VF_AMBIG | GPU_VF_SAT, 0, GPU_VF_AMBIG | GPU_VF_SAT | GPU_VF_BEHIND}; float Q[4][3]; memcpy(Q, P, sizeof Q); Q[1][2] = -800;
      reset(); ambig_quad(Q, f2, sxy);
      CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.stats.n_tris3d == 0 && s.stats.drop_ambig == 1 && s.stats.n_recon == 0, "b) tris %u ambig %u", s.stats.n_tris3d, s.stats.drop_ambig); }
    /* c) el paralelogramo no reproduce el SXY registrado del vértice (y=700 en vez de 300): descartado */
    { const int bad[4][2] = {{150, -45}, {600, -45}, {750, 300}, {1023, 700}};
      reset(); ambig_quad(P, fj, bad);
      CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.stats.n_tris3d == 0 && s.stats.drop_ambig == 1, "c) tris %u ambig %u", s.stats.n_tris3d, s.stats.drop_ambig); }
    /* d) uno de los tres "fiables" está detrás de la cámara (BEHIND sin AMBIG): no es fiable -> descartado */
    { uint8_t f3[4] = {0, GPU_VF_SAT | GPU_VF_BEHIND, 0, GPU_VF_AMBIG | GPU_VF_SAT}; float Q[4][3]; memcpy(Q, P, sizeof Q); Q[1][2] = -500;
      reset(); ambig_quad(Q, f3, sxy);
      CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.stats.n_tris3d == 0 && s.stats.drop_ambig == 1, "d) tris %u ambig %u", s.stats.n_tris3d, s.stats.drop_ambig); }
    /* e) un fiable solo SAT (x saturada, y sin saturar y coherente con la GTE): sí vale. v1=(10500,-300,1000) -> v3 = v1+v2-v0 = (10500,400,200) */
    { uint8_t f4[4] = {0, GPU_VF_SAT, 0, GPU_VF_AMBIG | GPU_VF_SAT}; float Q[4][3]; memcpy(Q, P, sizeof Q); Q[1][0] = 10500;
      const int e4[4][2] = {{150, -45}, {1023, -45}, {750, 300}, {1023, 300}};
      reset(); ambig_quad(Q, f4, e4);
      CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.stats.n_tris3d == 2 && s.stats.n_recon == 1, "e) tris %u recon %u", s.stats.n_tris3d, s.stats.n_recon);
      /* f) el SAT tiene las dos coordenadas saturadas: su SXY no informa, no es fiable -> descartado */
      const int g4[4][2] = {{150, -45}, {1023, 1023}, {750, 600}, {1023, 600}};   /* (1,2,3) mide <= 511 en Y: no lo descarta la GPU */
      Q[1][1] = 9000;
      reset(); ambig_quad(Q, f4, g4);
      CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.stats.n_tris3d == 0 && s.stats.drop_ambig == 1, "f) tris %u ambig %u", s.stats.n_tris3d, s.stats.drop_ambig); }
    /* g) sin recon_ambig: nunca se reconstruye */
    reset(); ambig_quad(P, fj, sxy); o.recon_ambig = 0;
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.stats.n_tris3d == 0 && s.stats.drop_ambig == 1, "g) tris %u ambig %u", s.stats.n_tris3d, s.stats.drop_ambig);
    vr_scene_set_fate(NULL, 0);
    vr_scene_release();
}

/* Correspondencia mundo -> espacio del usuario (user_map_on): el ojo del motor cae en p_ref, lo que está delante del ojo del motor queda delante
 * del usuario (-Z) con el giro theta, el suelo queda bajo la cabeza a la altura del ojo y la escala k cambia las distancias. */
static void test_user_map(void) {
    const float R[3][3] = {{-1, 0, 0}, {0, -1, 0}, {0, 0, 1}};       /* motor mirando a +Z (rumbo 0), derecha -X, abajo -Y */
    const int32_t e[3] = {1000, 1600, 2000};
    BridgeState bs; set_eye(&bs, R, e);
    float pts[3][3] = {{0, 0, 2000}, {0, 0, 3000}, {500, 300, 1000}};  /* vistas: 2 m delante a la altura del ojo; 3 m; (500 der, 300 abajo, 1000) */
    for (int th_case = 0; th_case < 3; th_case++) {
        float theta = th_case == 0 ? 0.0f : (th_case == 1 ? 90.0f : -37.0f);
        for (int sc_case = 0; sc_case < 2; sc_case++) {
            float k = sc_case ? 1.0f / 0.9f : 1.0f;
            reset();
            poly3(pts[0], pts[1], pts[2], 0);
            VrSceneOpts o = vr_scene_opts_default();
            o.origin_mode = 1; o.origin_game[0] = 1000; o.origin_game[1] = 0; o.origin_game[2] = 2000;
            VrScene base;
            CHECK(vr_scene_build(&dl, &bs, &o, &base) == 0 && base.nv == 3, "build base");
            float bp[3][3];
            for (int i = 0; i < 3; i++) memcpy(bp[i], base.v[i].pos, 12);
            float th = (theta + 180.0f) * 3.14159265f / 180.0f, cs = cosf(th), sn = sinf(th);
            o.user_map_on = 1;
            const float Rm[9] = {cs, 0, -sn, 0, 1, 0, sn, 0, cs};
            memcpy(o.user_R, Rm, sizeof Rm);
            o.user_k = k; o.user_eye_h_m = k * 1.6f;
            o.user_p_ref[0] = 0.1f; o.user_p_ref[1] = 1.04f; o.user_p_ref[2] = 0.2f;
            VrScene s;
            CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.nv == 3, "build user_map");
            int ok = 1;
            for (int i = 0; i < 3; i++) {
                float w[3] = {bp[i][0], bp[i][1] * k - k * 1.6f, bp[i][2] * k};          /* el mundo no se refleja aquí (det > 0) */
                w[0] = bp[i][0] * k;
                float P[3] = {Rm[0] * w[0] + Rm[1] * w[1] + Rm[2] * w[2] + 0.1f, Rm[3] * w[0] + Rm[4] * w[1] + Rm[5] * w[2] + 1.04f,
                              Rm[6] * w[0] + Rm[7] * w[1] + Rm[8] * w[2] + 0.2f};
                int m = 0;   /* el orden de los vértices puede cambiar (el ensamblador ordena por sentido de giro) */
                for (uint32_t j = 0; j < s.nv; j++)
                    if (near_f(s.v[j].pos[0], P[0], 2e-3f) && near_f(s.v[j].pos[1], P[1], 2e-3f) && near_f(s.v[j].pos[2], P[2], 2e-3f)) m = 1;
                ok &= m;
            }
            CHECK(ok, "user_map: fórmula (theta %.0f, k %.3f)", theta, k);
            if (!ok) for (int i = 0; i < 3; i++) fprintf(stderr, "  base %.3f %.3f %.3f -> %.3f %.3f %.3f\n", bp[i][0], bp[i][1], bp[i][2], s.v[i].pos[0], s.v[i].pos[1], s.v[i].pos[2]);
            if (th_case == 0 && sc_case == 0) {
                /* theta 0: 2 m delante del ojo y a su altura -> 2 m delante (-Z) de la cabeza, a su altura */
                int m = 0;
                for (uint32_t j = 0; j < s.nv; j++)
                    if (near_f(s.v[j].pos[0], 0.1f, 2e-3f) && near_f(s.v[j].pos[1], 1.04f, 2e-3f) && near_f(s.v[j].pos[2], -1.8f, 2e-3f)) m = 1;
                CHECK(m, "user_map: 2 m delante del ojo = 2 m delante (-Z) de la cabeza, a su altura");
            }
        }
    }
    vr_scene_release();
}


/* §56: red de seguridad de coherencia de vértices. 1 unidad de vista = 1 mm (escala por defecto 0,001). */
static void test_coherence(void) {
    VrScene s; VrSceneOpts o = vr_scene_opts_default();
    CHECK(o.coherence == 1 && o.coh_edge_m == 60.0f && o.coh_flag_edge_m == 32.0f, "coherencia: valores por defecto");
    /* limpio y razonable: se conserva */
    float a[3] = {0, 0, 5000}, b[3] = {1000, 0, 5000}, c[3] = {0, 1000, 5000};
    reset(); poly3(a, b, c, 0);
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.stats.n_tris3d == 1 && s.stats.drop_incoh == 0, "coherencia: triángulo normal intacto");
    /* un vértice a 70 m del resto (asociación equivocada): fuera y contado; con la red apagada se emite pero se cuenta */
    float far3[3] = {70000, 3000, 5000};
    reset(); poly3(a, b, far3, 0);
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.stats.n_tris3d == 0 && s.stats.drop_incoh == 1 && s.stats.incoh[VR_INC_EDGE] == 1, "coherencia: arista de 70 m descartada (%u)", s.stats.drop_incoh);
    o.coherence = 0;
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.stats.n_tris3d == 1 && s.stats.drop_incoh == 0 && s.stats.incoh[VR_INC_EDGE] == 1, "coherencia apagada: se emite y se cuenta (tris %u drop %u edge %u)", s.stats.n_tris3d, s.stats.drop_incoh, s.stats.incoh[VR_INC_EDGE]);
    o.coherence = 1;
    /* 40 m: legítimo si todos sus vértices son fiables, sospechoso si alguno es ambiguo */
    float m40[3] = {40000, 3000, 5000};
    reset(); poly3(a, b, m40, 0);
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.stats.n_tris3d == 1, "coherencia: 40 m fiable se conserva (tris %u inc %u/%u/%u/%u)", s.stats.n_tris3d, s.stats.incoh[0], s.stats.incoh[1], s.stats.incoh[2], s.stats.drop_degenerate);
    reset(); { GpuOp *p = poly3(a, b, m40, 0); p->v[2].vflags = GPU_VF_AMBIG; }
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.stats.n_tris3d == 0 && s.stats.incoh[VR_INC_EDGE] == 1, "coherencia: 40 m ambiguo descartado");
    /* vértice a más de 150 m del ojo del motor */
    float f1[3] = {160000, 0, 100}, f2[3] = {160500, 0, 100}, f3[3] = {160000, 500, 100};
    reset(); poly3(f1, f2, f3, 0);
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.stats.n_tris3d == 0 && s.stats.incoh[VR_INC_FAR] == 1, "coherencia: vértice a 160 m descartado");
    /* modelo de la GTE: proyección coherente con (x,y) -> intacto; un vértice con otra pantalla -> descartado */
    reset();
    {
        GpuOp *q = poly3(a, b, c, 0);
        q->proj_h = 150.0f; q->proj_ox = 160.0f; q->proj_oy = 128.0f;
        for (int k = 0; k < 3; k++) {
            q->v[k].x = (int16_t)lrintf(150.0f * q->v[k].vx / q->v[k].vz + 160.0f);
            q->v[k].y = (int16_t)lrintf(150.0f * q->v[k].vy / q->v[k].vz + 128.0f);
        }
    }
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.stats.n_tris3d == 1 && s.stats.chk_vtx == 3 && s.stats.drop_incoh == 0, "coherencia: modelo reproduce la pantalla (%u de %u)", s.stats.chk_bad[0], s.stats.chk_vtx);
    dl.ops[0].v[1].x += 40;
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.stats.n_tris3d == 0 && s.stats.incoh[VR_INC_MODEL] == 1 && s.stats.chk_bad[0] == 1, "coherencia: vértice que no reproduce su pantalla descartado");
    /* saturado: SXY recortado a +-0x3FF con la posición muy fuera de pantalla -> el modelo lo reproduce */
    reset();
    {
        float s1[3] = {300000, 0, 100}, s2[3] = {300500, 0, 100}, s3[3] = {300000, 500, 100};
        GpuOp *q = poly3(s1, s2, s3, 0);
        q->proj_h = 150.0f; q->proj_ox = 160.0f; q->proj_oy = 128.0f;
        for (int k = 0; k < 3; k++) { q->v[k].vflags = GPU_VF_SAT; q->v[k].x = 0x3FF; q->v[k].y = (int16_t)lrintf(150.0f * q->v[k].vy / q->v[k].vz + 128.0f); }
    }
    o.coh_far_m = 0.0f;
    (void)vr_scene_build(&dl, NULL, &o, &s);
    CHECK(s.stats.chk_bad[1] == 0, "coherencia: vértice saturado coherente (malos %u)", s.stats.chk_bad[1]);
    vr_scene_release();
}

/* ---- composición 3D + 2D con la cámara del juego (la usa el probador por fotograma): la vista GL sale de game_view y el pase 3D cae donde el motor proyectó ---- */
static void test_compose_view(void) {
    for (int lefty = 0; lefty < 2; lefty++) {
        const float R[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, lefty ? 1 : -1}};
        const int32_t e[3] = {200, 3000, 500};
        BridgeState bs; set_eye(&bs, R, e);
        reset();
        /* triángulo de color plano en espacio de vista (z = 2000, H = 320, centro (160,120)): vértices en pantalla (96,56) (224,56) (160,184) */
        float a[3] = {-400, -400, 2000}, b[3] = {400, -400, 2000}, c[3] = {0, 400, 2000};
        GpuOp *o = poly3(a, b, c, 0);
        o->flags = 0; o->proj_h = 320.0f; o->proj_ox = 160.0f; o->proj_oy = 120.0f;
        for (int k = 0; k < 3; k++) { o->v[k].r = 200; o->v[k].g = 40; o->v[k].b = 40; o->v[k].x = (int16_t)(160 + (k == 0 ? -64 : k == 1 ? 64 : 0)); o->v[k].y = (int16_t)(120 + (k == 2 ? 64 : -64)); }
        /* rect 2D opaco azul encima del centro del triángulo */
        GpuOp *r2 = add(GPU_OP_RECT);
        r2->v[0].x = 150; r2->v[0].y = 110; r2->rw = 20; r2->rh = 20; r2->cx2 = 319; r2->cy2 = 239; r2->v[0].r = 0; r2->v[0].g = 0; r2->v[0].b = 250;
        dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 240;
        VrSceneOpts so = vr_scene_opts_default();
        so.origin_mode = 1;
        VrScene s;
        CHECK(vr_scene_build(&dl, &bs, &so, &s) == 0 && s.has_3d, "build compose %d", lefty);
        enum { W = 320, H = 240 };
        HudImg i3 = hud_img_new(W, H), i2 = hud_img_new(W, H);
        static float zb[W * H];
        static uint16_t vram[1024 * 512];
        float view[16];
        CHECK(hud_emu_game_view_gl(&s, view), "vista invertible");
        const float tn[4] = {s.filter.cx_col / s.filter.proj_h, (W - s.filter.cx_col) / s.filter.proj_h, s.filter.cy_row / s.filter.proj_h, (H - s.filter.cy_row) / s.filter.proj_h};
        for (int i = 0; i < W * H; i++) i3.rgba[i * 4 + 3] = 1;
        hud_emu_run3d(&s, vram, &i3, zb, 1, view, tn);
        hud_emu_run(&s, vram, &i2, 0, 0);
        const float *p = &i3.rgba[((size_t)(100) * W + 160) * 4];      /* dentro del triángulo, fuera del rect */
        CHECK(near_f(p[0], 200.0f / 255.0f, 0.03f) && p[1] < 0.2f, "3D en (160,100) = (%.2f %.2f %.2f) lefty=%d", p[0], p[1], p[2], lefty);
        const float *q = &i3.rgba[((size_t)(60) * W + 100) * 4];       /* esquina sup. izq. del triángulo: dentro (96,56) .. */
        const float *out = &i3.rgba[((size_t)(200) * W + 30) * 4];     /* fuera */
        CHECK(near_f(out[0], 0.0f, 0.01f) && out[2] < 0.01f, "fuera del triángulo negro");
        CHECK(near_f(q[0], 200.0f / 255.0f, 0.03f), "vértice del triángulo en la posición proyectada por el motor: (100,60) = %.2f", q[0]);
        const float *z = &i3.rgba[((size_t)(190) * W + 160) * 4];      /* debajo de (160,184): fuera */
        CHECK(z[0] < 0.01f, "bajo el vértice inferior (160,190) vacío: %.2f", z[0]);
        const float *u = &i2.rgba[((size_t)(120) * W + 160) * 4];
        CHECK(near_f(u[2], 250.0f / 255.0f, 0.02f) && near_f(u[3], 1.0f, 1e-4f), "panel 2D opaco en el centro: %.2f a=%.2f", u[2], u[3]);
        CHECK(near_f(i2.rgba[((size_t)(100) * W + 160) * 4 + 3], 0.0f, 1e-4f), "panel 2D transparente fuera del rect");
        hud_img_free(&i3); hud_img_free(&i2);
        vr_scene_release();
    }
}

/* ---- recorte vigente (E3/E4) en el panel 2D: lo que el juego dibuja fuera de su caja no se ve (ventanas, cuadros de texto, letras con recorte) ---- */
static void test_clip_2d(void) {
    BridgeState bs; const float R[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}}; const int32_t e[3] = {0, 0, 0};
    set_eye(&bs, R, e);
    reset();
    /* rect rojo de 100x100 en (10,10) con recorte (30..69, 40..79) inclusivo, y otro igual SIN recorte más abajo a la derecha */
    GpuOp *a = add(GPU_OP_RECT);
    a->v[0].x = 10; a->v[0].y = 10; a->rw = 100; a->rh = 100; a->v[0].r = 250; a->v[0].g = 0; a->v[0].b = 0;
    a->cx1 = 30; a->cy1 = 40; a->cx2 = 69; a->cy2 = 79;
    GpuOp *b = add(GPU_OP_RECT);
    b->v[0].x = 150; b->v[0].y = 10; b->rw = 40; b->rh = 40; b->v[0].r = 0; b->v[0].g = 250; b->v[0].b = 0;
    b->cx1 = 0; b->cy1 = 0; b->cx2 = 319; b->cy2 = 239;
    dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 240;
    VrSceneOpts so = vr_scene_opts_default();
    VrScene s;
    CHECK(vr_scene_build(&dl, &bs, &so, &s) == 0, "build clip");
    enum { W = 320, H = 240 };
    HudImg i2 = hud_img_new(W, H);
    static uint16_t vram[1024 * 512];
    hud_emu_run(&s, vram, &i2, 0, 0);
#define PX(x, y) (&i2.rgba[((size_t)(y) * W + (x)) * 4])
    CHECK(PX(50, 60)[0] > 0.9f && PX(50, 60)[3] > 0.99f, "dentro del recorte se dibuja");
    CHECK(PX(30, 40)[3] > 0.99f && PX(69, 79)[3] > 0.99f, "bordes inclusivos del recorte");
    CHECK(PX(29, 60)[3] < 0.01f && PX(70, 60)[3] < 0.01f && PX(50, 39)[3] < 0.01f && PX(50, 80)[3] < 0.01f, "fuera del recorte no se dibuja");
    CHECK(PX(20, 20)[3] < 0.01f && PX(100, 100)[3] < 0.01f, "el resto del rect recortado (esquinas) no se dibuja");
    CHECK(PX(160, 20)[1] > 0.9f && PX(189, 49)[3] > 0.99f, "sin recorte (pantalla completa) se dibuja entero");
    hud_img_free(&i2);
    vr_scene_release();
#undef PX
}

/* El contrato que usa vr/host/vr_smooth: con user_map, un punto del mundo del juego W cae en P = c + R * (k * scale) * (W - O), c = p_ref - R col1 * eye_h, tanto con la
 * cámara del motor directa (det > 0) como reflejada (det < 0: la reflexión de z de vr_scene se cancela). */
/* ---- modo gpu: distancia al ojo, recorte cercano y 2D al panel (VrSceneOpts.eye_near_m) ---- */
static void test_gpu_eye_near(void) {
    const float R[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    const int32_t e[3] = {0, 3000, 500};
    BridgeState bs; set_eye(&bs, R, e);
    VrSceneOpts o = vr_scene_opts_default();
    o.origin_mode = 1;
    VrScene s;
    const uint8_t z4[4] = {0, 0, 0, 0};
    /* a) un triángulo a 0,1 m del ojo (100 unidades = 0,1 m con escala 0,001): fuera con eye_near_m; sin él (comportamiento previo) se emite */
    {
        reset();
        const float P[4][3] = {{0, 0, 100}, {60, 0, 100}, {0, 60, 100}, {0, 0, 0}};
        polyN(3, P, z4, 0);
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 1 && s.stats.n_near_eye == 0, "sin eye_near_m: emitido (tris %u)", s.stats.n_tris3d);
        o.eye_near_m = 0.25f;
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 0 && s.nv == 0 && s.stats.n_near_eye == 1, "a 0,1 m: descartado (tris %u, near_eye %u)", s.stats.n_tris3d, s.stats.n_near_eye);
        /* a 0,3 m (y con un vértice a 0,2 m, descartado): el límite es el vértice más cercano */
        reset();
        const float Q[4][3] = {{0, 0, 300}, {60, 0, 300}, {0, 60, 300}, {0, 0, 0}};
        polyN(3, Q, z4, 0);
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 1 && s.stats.n_near_eye == 0, "a 0,3 m: se conserva");
        reset();
        const float W[4][3] = {{0, 0, 200}, {200, 0, 400}, {0, 200, 400}, {0, 0, 0}};
        polyN(3, W, z4, 0);
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 1 && s.stats.n_near_eye == 0, "un vértice a 0,2 m y los otros lejos: se conserva (tris %u near %u)", s.stats.n_tris3d, s.stats.n_near_eye);
        /* todos los vértices dentro de eye_near_m (efecto pegado a la cara): descartado */
        reset();
        const float F[4][3] = {{0, 0, 150}, {100, 0, 200}, {0, 100, 200}, {0, 0, 0}};
        polyN(3, F, z4, 0);
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 0 && s.stats.n_near_eye == 1, "todos a < 0,25 m: descartado (near %u)", s.stats.n_near_eye);
        /* la escala de salida cuenta: con escala 0,002 los vértices a 150-200 unidades están a 0,3-0,4 m (fuera de eye_near_m) */
        VrSceneOpts o2 = o; o2.scale = 0.002f;
        CHECK(vr_scene_build(&dl, &bs, &o2, &s) == 0 && s.stats.n_tris3d == 1 && s.stats.n_near_eye == 0, "la distancia usa opts.scale");
        /* pared grande (aristas de 2 m) con un vértice a 0,1 m: NO se descarta (se ve a través de ella si se borra) */
        reset();
        const float G[4][3] = {{0, 0, 100}, {2000, 0, 100}, {0, 2000, 100}, {0, 0, 0}};
        polyN(3, G, z4, 0);
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 1 && s.stats.n_near_eye == 0, "pared grande junto al ojo: se conserva");
    }
    /* b) un triángulo a caballo del plano cercano se RECORTA (no se descarta): 2 vértices delante, 1 detrás (BEHIND) -> cuadrilátero = 2 triángulos, ninguno pegado al ojo */
    {
        reset();
        const float P[4][3] = {{0, 0, 2000}, {1000, 0, 2000}, {0, 1000, -1000}, {0, 0, 0}};
        const uint8_t fl[4] = {0, 0, GPU_VF_BEHIND, 0};
        polyN(3, P, fl, 1);
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 2 && s.stats.n_clipped == 1 && s.stats.n_near_eye == 0 && s.stats.drop_behind == 0,
              "a caballo del plano: recortado (tris %u clip %u near %u behind %u)", s.stats.n_tris3d, s.stats.n_clipped, s.stats.n_near_eye, s.stats.drop_behind);
        for (uint32_t i = 0; i < s.nv; i++) {
            float zv = (s.eye_vr[2] - s.v[i].pos[2]) / s.scale;
            CHECK(zv > 0.0f, "vértice recortado a z=%f <= 0", zv);
        }
        /* entero detrás: descartado (cuenta como detrás) */
        reset();
        const float B[4][3] = {{0, 0, -500}, {1000, 0, -500}, {0, 1000, -700}, {0, 0, 0}};
        const uint8_t fb[4] = {GPU_VF_BEHIND, GPU_VF_BEHIND, GPU_VF_BEHIND, 0};
        polyN(3, B, fb, 0);
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 0 && s.stats.drop_behind == 1, "entero detrás: descartado (behind %u)", s.stats.drop_behind);
        /* BEHIND declarado sin ningún vértice detrás del plano (dato incoherente): descartado */
        reset();
        const float G[4][3] = {{0, 0, 2000}, {1000, 0, 2000}, {0, 1000, 2000}, {0, 0, 0}};
        const uint8_t fg[4] = {GPU_VF_BEHIND, 0, 0, 0};
        polyN(3, G, fg, 0);
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 0 && s.stats.drop_behind == 1, "BEHIND incoherente: descartado (tris %u)", s.stats.n_tris3d);
        /* sin BEHIND y con un vértice detrás del plano (antes: descartado como drop_near con engine_near_clip): ahora se recorta */
        reset();
        const float N[4][3] = {{0, 0, 2000}, {1000, 0, 2000}, {0, 1000, -300}, {0, 0, 0}};
        polyN(3, N, z4, 0);
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 2 && s.stats.n_clipped == 1, "vértice detrás sin bandera: recortado");
    }
    /* b2) con eye_near_m el plano cercano NO es el de la cámara del juego (near_units): lo recorta el renderer con el de la cabeza. Solo se recorta contra z <= 0 (épsilon) */
    {
        reset();
        const float A[4][3] = {{0, 0, 10}, {2000, 0, 10}, {0, 2000, 10}, {0, 0, 0}};   /* entero entre el ojo y near_units (24) */
        polyN(3, A, z4, 0);
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 1 && s.stats.drop_behind == 0 && s.stats.n_clipped == 0,
              "z=10 < near_units: emitido sin recortar (tris %u behind %u clip %u)", s.stats.n_tris3d, s.stats.drop_behind, s.stats.n_clipped);
        reset();
        const float M[4][3] = {{0, 0, 2000}, {1000, 0, 2000}, {0, 1000, 10}, {0, 0, 0}};   /* un vértice entre el ojo y near_units */
        polyN(3, M, z4, 0);
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 1 && s.stats.n_clipped == 0, "un vértice a z=10: sin recorte (tris %u clip %u)", s.stats.n_tris3d, s.stats.n_clipped);
        reset();
        const float K[4][3] = {{0, 0, 2000}, {1000, 0, 2000}, {0, 1000, -300}, {0, 0, 0}};   /* un vértice detrás del ojo: recorte al plano del ojo, no a near_units */
        polyN(3, K, z4, 0);
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 2 && s.stats.n_clipped == 1, "vértice detrás: recortado (tris %u clip %u)", s.stats.n_tris3d, s.stats.n_clipped);
        float zmin = 1e9f;
        for (uint32_t i = 0; i < s.nv; i++) { float zv = (s.eye_vr[2] - s.v[i].pos[2]) / s.scale; if (zv < zmin) zmin = zv; }
        CHECK(zmin > 0.0f && zmin < 1.0f, "el recorte queda pegado al plano del ojo: zmin=%f", zmin);
    }
    /* c) una primitiva 2D (rect de HUD / texto) va al panel: batch kind 2, op_dest = 2, posición en píxeles de pantalla, nunca como 3D (aunque haya 3D pegado al ojo en la lista) */
    {
        reset();
        const float P[4][3] = {{0, 0, 100}, {60, 0, 100}, {0, 60, 100}, {0, 0, 0}};
        polyN(3, P, z4, 0);                                         /* op0: 3D a 0,1 m (descartado) */
        rect2d(20, 30, 16, 8, 0, 0, GPU_F_TEXTURED, 0, 320, 0, 128, 128, 128);   /* op1: 2D */
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0, "build 2D");
        int k3 = 0, k2 = 0;
        for (uint32_t i = 0; i < s.nb; i++) { if (s.b[i].kind <= 1) k3++; else if (s.b[i].kind == 2) k2++; }
        CHECK(k3 == 0 && k2 == 1 && s.stats.n_tris3d == 0 && s.stats.n_near_eye == 1, "3D=%d 2D=%d tris3d=%u near=%u", k3, k2, s.stats.n_tris3d, s.stats.n_near_eye);
        CHECK(s.op_dest && s.n_op_dest >= 2 && s.op_dest[0] == 0 && s.op_dest[1] == 2, "op_dest: 3D descartado = 0, rect = 2 (panel)");
        int ok2d = s.nv > 0;
        for (uint32_t i = 0; i < s.nv; i++) ok2d &= (s.v[i].flags & VR_F_2D) != 0 && s.v[i].pos[2] == 0.0f && s.v[i].pos[0] >= 20.0f && s.v[i].pos[0] <= 36.0f && s.v[i].pos[1] >= 30.0f && s.v[i].pos[1] <= 38.0f;
        CHECK(ok2d, "el rect 2D conserva coordenadas de pantalla y VR_F_2D");
    }
    vr_scene_release();
}

static void test_user_map_linear(void) {
    for (int mirrored = 0; mirrored < 2; mirrored++) {
        float R[3][3] = {{-1, 0, 0}, {0, -1, 0}, {0, 0, 1}};
        if (mirrored) { R[0][0] = 1; R[1][1] = 1; R[2][2] = -1; }
        const int32_t e[3] = {1000, 1600, 2000};
        BridgeState bs; set_eye(&bs, R, e);
        float pts[3][3] = {{0, 0, 2000}, {0, 0, 3000}, {500, 300, 1000}};
        reset();
        poly3(pts[0], pts[1], pts[2], 0);
        VrSceneOpts o = vr_scene_opts_default();
        o.origin_mode = 1; o.origin_game[0] = 1000; o.origin_game[1] = 0; o.origin_game[2] = 2000;
        float th = 125.0f * 3.14159265f / 180.0f, cs = cosf(th), sn = sinf(th);
        const float Rm[9] = {cs, 0, -sn, 0, 1, 0, sn, 0, cs};
        o.user_map_on = 1; memcpy(o.user_R, Rm, sizeof Rm);
        o.user_k = 1.1f; o.user_eye_h_m = 1.76f; o.user_p_ref[0] = 0.1f; o.user_p_ref[1] = 1.04f; o.user_p_ref[2] = 0.2f;
        VrScene s;
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.nv == 3 && s.user_space, "linear: build");
        float K = s.user_k * s.scale, c[3];
        for (int i = 0; i < 3; i++) c[i] = s.user_p_ref[i] - s.user_R[3 * i + 1] * s.user_eye_h;
        int found = 0;
        for (int i = 0; i < 3; i++) {
            float W[3], d[3], P[3];
            for (int r = 0; r < 3; r++) W[r] = (float)e[r] + R[r][0] * pts[i][0] + R[r][1] * pts[i][1] + R[r][2] * pts[i][2];
            for (int r = 0; r < 3; r++) d[r] = W[r] - s.origin_game[r];
            for (int r = 0; r < 3; r++) P[r] = c[r] + K * (s.user_R[3 * r] * d[0] + s.user_R[3 * r + 1] * d[1] + s.user_R[3 * r + 2] * d[2]);
            for (uint32_t j = 0; j < s.nv; j++)
                if (near_f(s.v[j].pos[0], P[0], 2e-3f) && near_f(s.v[j].pos[1], P[1], 2e-3f) && near_f(s.v[j].pos[2], P[2], 2e-3f)) { found++; break; }
        }
        CHECK(found == 3, "linear: P = c + R k scale (W - O) (espejado %d): %d de 3", mirrored, found);
    }
    vr_scene_release();
}

/* Ventana de la pantalla 3D de las cinemáticas: debe salir de la H real y del centro de la lista (no de la pantalla virtual) para registrar con el panel 2D */
static GpuOp *proj_poly(float H, float ox, float oy) {
    float a[3] = {0, 0, 2000}, b[3] = {1000, 0, 2000}, c[3] = {0, 1000, 2000};
    GpuOp *o = poly3(a, b, c, 0);
    o->proj_h = H; o->proj_ox = ox; o->proj_oy = oy;
    return o;
}
static void test_screen_window(void) {
    const float R[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    const int32_t e[3] = {0, 3000, 500};
    BridgeState bs; set_eye(&bs, R, e);
    VrSceneOpts o = vr_scene_opts_default();
    o.origin_mode = 1;
    VrScene s;
    /* créditos: H=360, centro (160,120) en un área de 320x240 -> tangentes 160/360 y 120/360 (no la ventana virtual de la pantalla) */
    reset(); dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 240;
    proj_poly(360.0f, 160.0f, 120.0f);
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.filter.proj_ok, "build ventana");
    VrScreenWindow w = vr_scene_screen_window(&s.filter);
    CHECK(w.ok && near_f(w.left, 160.0f / 360.0f, 1e-5f) && near_f(w.right, 160.0f / 360.0f, 1e-5f) &&
          near_f(w.up, 120.0f / 360.0f, 1e-5f) && near_f(w.down, 120.0f / 360.0f, 1e-5f), "ventana centrada %d %.4f %.4f %.4f %.4f", w.ok, w.left, w.right, w.up, w.down);
    /* centro desplazado (OFX/OFY) y área 320x256 en (0,8): la ventana se desplaza y la parte baja/alta se reparte según cy_row */
    reset(); dl.disp.x = 0; dl.disp.y = 8; dl.disp.w = 320; dl.disp.h = 256;
    proj_poly(300.0f, 180.0f, 100.0f);   /* cx_col = 180, cy_row = 100 - 8 = 92 */
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0, "build desplazada");
    w = vr_scene_screen_window(&s.filter);
    CHECK(w.ok && near_f(w.left, 180.0f / 300.0f, 1e-5f) && near_f(w.right, 140.0f / 300.0f, 1e-5f) &&
          near_f(w.up, 92.0f / 300.0f, 1e-5f) && near_f(w.down, 164.0f / 300.0f, 1e-5f), "ventana desplazada %d %.4f %.4f %.4f %.4f", w.ok, w.left, w.right, w.up, w.down);
    /* sin polígonos con proyección: no hay ventana (el consumidor usa la de la petición) */
    reset(); dl.disp.w = 320; dl.disp.h = 240;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && !s.filter.proj_ok && !vr_scene_screen_window(&s.filter).ok, "sin proyección: sin ventana");
    CHECK(!vr_scene_screen_window(NULL).ok, "NULL");
    /* centro óptico fuera de la imagen: no válido */
    VrFilter bad = s.filter; bad.proj_ok = 1; bad.proj_h = 360; bad.cx_col = 400; bad.cy_row = 120; bad.area[2] = 320; bad.area[3] = 240;
    CHECK(!vr_scene_screen_window(&bad).ok, "centro fuera de la imagen");
    vr_scene_release();
}

/* Línea del mundo con los dos extremos de vista -> cuadrilátero 3D con grosor en píxeles (opts.line3d_px) */
static GpuOp *line3(float x0, float y0, float z0, float x1, float y1, float z1, float H, float ox, float oy) {
    GpuOp *o = add(GPU_OP_LINE);
    o->nv = 2; o->proj_h = H; o->proj_ox = ox; o->proj_oy = oy;
    float P[2][3] = {{x0, y0, z0}, {x1, y1, z1}};
    for (int i = 0; i < 2; i++) {
        GpuVtx *v = &o->v[i];
        setv3(v, P[i][0], P[i][1], P[i][2]);
        float nf = H / P[i][2];
        v->x = (int16_t)floorf(ox + nf * P[i][0]); v->y = (int16_t)floorf(oy + nf * P[i][1]);   /* SXY que reproduce el modelo de la GTE */
        v->r = 200; v->g = 100; v->b = 50;
    }
    return o;
}
static void view_to_vr(const VrScene *s, const float v[3], float out[3]) {
    for (int r = 0; r < 3; r++) out[r] = s->game_view[0 + r] * v[0] + s->game_view[4 + r] * v[1] + s->game_view[8 + r] * v[2] + s->game_view[12 + r];
}
static void test_line3d(void) {
    const float R[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    const int32_t e[3] = {0, 3000, 500};
    BridgeState bs; set_eye(&bs, R, e);
    VrSceneOpts o = vr_scene_opts_default();
    o.origin_mode = 1;
    VrScene s;
    /* a) por defecto (line3d_px = 0): sigue yendo al plano 2D */
    reset(); dl.disp.w = 320; dl.disp.h = 240;
    line3(100, 50, 1800, 500, -100, 1800, 360, 160, 120);
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 0 && s.stats.n_line3d == 0 && s.nv == 6 && (s.v[0].flags & VR_F_2D), "línea por defecto: 2D");
    /* b) con line3d_px = 1: 2 triángulos 3D, sin VR_F_2D, el cuadrilátero rodea el segmento con grosor 1 px a esa profundidad */
    o.line3d_px = 1.0f;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 2 && s.stats.n_line3d == 1 && s.nv == 6 && s.has_3d, "línea 3D: tris %u", s.stats.n_tris3d);
    CHECK(s.nb == 1 && s.b[0].kind == 0 && s.b[0].blend == 0xFF && s.op_dest && s.op_dest[0] == 0, "línea 3D: lote opaco y op_dest 0");
    {
        int ok2d = 0;
        for (uint32_t i = 0; i < s.nv; i++) ok2d |= (s.v[i].flags & (VR_F_2D | VR_F_LINE)) != 0;
        CHECK(!ok2d, "línea 3D: sin banderas 2D");
        float u[6][3]; int nu = 0;
        for (uint32_t i = 0; i < s.nv; i++) {
            int dup = 0;
            for (int k = 0; k < nu; k++) if (near_f(u[k][0], s.v[i].pos[0], 1e-5f) && near_f(u[k][1], s.v[i].pos[1], 1e-5f) && near_f(u[k][2], s.v[i].pos[2], 1e-5f)) dup = 1;
            if (!dup && nu < 6) memcpy(u[nu++], s.v[i].pos, sizeof u[0]);
        }
        CHECK(nu == 4, "línea 3D: 4 esquinas únicas (%d)", nu);
        if (nu == 4) {
            float dmin = 1e9f, dmax = 0, mid[3] = {0, 0, 0}, want[3], mv[3] = {300, -25, 1800};
            for (int a = 0; a < 4; a++) {
                for (int c = 0; c < 3; c++) mid[c] += u[a][c] * 0.25f;
                for (int b = a + 1; b < 4; b++) {
                    float d = sqrtf((u[a][0]-u[b][0])*(u[a][0]-u[b][0]) + (u[a][1]-u[b][1])*(u[a][1]-u[b][1]) + (u[a][2]-u[b][2])*(u[a][2]-u[b][2]));
                    dmin = fminf(dmin, d); dmax = fmaxf(dmax, d);
                }
            }
            view_to_vr(&s, mv, want);
            CHECK(near_f(dmin, 1.0f / 360.0f * 1800.0f * 0.001f, 2e-4f), "grosor 1 px a 1,8 m = 5 mm (%.5f)", dmin);
            CHECK(near_f(dmax, (sqrtf(400.0f * 400.0f + 150.0f * 150.0f) + 5.0f) * 0.001f, 2e-3f), "largo = segmento + 1 px (%.5f)", dmax);
            CHECK(near_f(mid[0], want[0], 1e-4f) && near_f(mid[1], want[1], 1e-4f) && near_f(mid[2], want[2], 1e-4f), "centro del cuadrilátero = centro del segmento");
        }
        /* el frente mira a la cámara del juego (ambos triángulos) */
        for (uint32_t t = 0; t < s.nv / 3; t++) {
            const VrVtx *v = &s.v[3 * t];
            float e1[3], e2[3], n[3], tc[3];
            for (int c = 0; c < 3; c++) { e1[c] = v[1].pos[c] - v[0].pos[c]; e2[c] = v[2].pos[c] - v[0].pos[c]; tc[c] = s.eye_vr[c] - v[0].pos[c]; }
            n[0] = e1[1]*e2[2]-e1[2]*e2[1]; n[1] = e1[2]*e2[0]-e1[0]*e2[2]; n[2] = e1[0]*e2[1]-e1[1]*e2[0];
            CHECK(n[0]*tc[0] + n[1]*tc[1] + n[2]*tc[2] > 0, "línea 3D: frente hacia la cámara (tri %u)", t);
        }
    }
    /* c) semitransparente: lote semitransparente con su modo */
    reset(); dl.disp.w = 320; dl.disp.h = 240;
    { GpuOp *l = line3(100, 50, 1800, 500, -100, 1800, 360, 160, 120); l->flags = GPU_F_SEMI; l->abr = 1; }
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_line3d == 1 && s.nb == 1 && s.b[0].kind == 1 && s.b[0].blend == 1, "línea semitransparente: kind 1, blend 1");
    /* d) el SXY no reproduce el modelo (coincidencia con un vértice de HUD): se queda en 2D */
    reset(); dl.disp.w = 320; dl.disp.h = 240;
    { GpuOp *l = line3(100, 50, 1800, 500, -100, 1800, 360, 160, 120); l->v[1].x = (int16_t)(l->v[1].x + 12); }
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_line3d == 0 && s.stats.n_tris3d == 0 && (s.v[0].flags & VR_F_2D), "SXY incoherente: 2D");
    /* e) un solo extremo con posición de vista, o sin proyección: 2D */
    reset(); dl.disp.w = 320; dl.disp.h = 240;
    { GpuOp *l = line3(100, 50, 1800, 500, -100, 1800, 360, 160, 120); l->v[1].vvalid = 0; }
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_line3d == 0 && (s.v[0].flags & VR_F_2D), "un extremo sin vista: 2D");
    reset(); dl.disp.w = 320; dl.disp.h = 240;
    { GpuOp *l = line3(100, 50, 1800, 500, -100, 1800, 360, 160, 120); l->proj_h = 0; }
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_line3d == 0 && (s.v[0].flags & VR_F_2D), "sin proj_h: 2D");
    /* f) plano cercano: ambos extremos delante del plano -> fuera (detrás); uno detrás -> recortado en el plano */
    reset(); dl.disp.w = 320; dl.disp.h = 240;
    line3(100, 50, 1800, 300, -60, 3600, 360, 160, 120);
    VrSceneOpts on = o; on.near_units = 4000.0f;
    CHECK(vr_scene_build(&dl, &bs, &on, &s) == 0 && s.stats.n_line3d == 0 && s.stats.n_tris3d == 0 && s.stats.drop_behind == 1, "línea entera detrás: descartada (behind %u)", s.stats.drop_behind);
    on.near_units = 2700.0f;
    CHECK(vr_scene_build(&dl, &bs, &on, &s) == 0 && s.stats.n_line3d == 1 && s.stats.n_clipped == 1 && s.nv == 6, "línea a caballo del plano: recortada");
    for (uint32_t i = 0; i < s.nv; i++) {
        float zv = (s.eye_vr[2] - s.v[i].pos[2]) / s.scale;
        CHECK(zv >= on.near_units - 0.05f, "línea recortada: z=%f < plano cercano", zv);
    }
    /* g) pegada al ojo del motor (eye_near_m): fuera, como los triángulos pequeños */
    reset(); dl.disp.w = 320; dl.disp.h = 240;
    line3(10, 5, 200, 30, -6, 200, 360, 160, 120);   /* a 0,2 m del ojo, extremos muy próximos entre sí */
    on = o; on.eye_near_m = 0.25f;
    CHECK(vr_scene_build(&dl, &bs, &on, &s) == 0 && s.stats.n_line3d == 0 && s.stats.n_near_eye == 1, "línea pegada al ojo: descartada (near %u)", s.stats.n_near_eye);
    /* h) con eye_near_m el plano cercano no es near_units: la línea de 1800 a 3600 con near_units 2700 sale entera */
    reset(); dl.disp.w = 320; dl.disp.h = 240;
    line3(100, 50, 1800, 300, -60, 3600, 360, 160, 120);
    on = o; on.near_units = 2700.0f; on.eye_near_m = 0.25f;
    CHECK(vr_scene_build(&dl, &bs, &on, &s) == 0 && s.stats.n_line3d == 1 && s.stats.n_clipped == 0 && s.stats.drop_behind == 0, "línea con eye_near_m: sin recorte contra near_units (clip %u)", s.stats.n_clipped);
    vr_scene_release();
}

/* Cinemática con letterbox (captura: franja negra 2D desplazada ~195 filas respecto de la imagen 3D). El 3D y las bandas de la misma lista (2 TILE negros (0,0,320,27) y (0,210,320,46)) deben
 * registrar entre sí: la ventana de la lista (H=200, OFX/OFY 160/118, disp 320x256) lleva cada vértice 3D a la fila del panel 2D (v de textura = SXY.y / 256). */
static void test_cine_letterbox(void) {
    const float R[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    const int32_t e[3] = {0, 3000, 500};
    BridgeState bs; set_eye(&bs, R, e);
    VrSceneOpts o = vr_scene_opts_default();
    o.origin_mode = 1;
    reset(); dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 256;
    const float H = 200.0f, OFX = 160.0f, OFY = 118.0f;
    float Pw[3][3] = {{-400, -200, 2000}, {800, -200, 2000}, {0, 600, 2000}};
    GpuOp *po = poly3(Pw[0], Pw[1], Pw[2], 0);
    po->proj_h = H; po->proj_ox = OFX; po->proj_oy = OFY;
    int sx[3], sy[3];
    for (int k = 0; k < 3; k++) {
        sx[k] = (int)lrintf(OFX + H * Pw[k][0] / Pw[k][2]); sy[k] = (int)lrintf(OFY + H * Pw[k][1] / Pw[k][2]);   /* SXY de la GTE */
        po->v[k].x = (int16_t)sx[k]; po->v[k].y = (int16_t)sy[k];
    }
    rect2d(0, 0, 320, 27, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    rect2d(0, 210, 320, 46, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    VrScene s;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.has_3d && s.filter.proj_ok, "build letterbox");
    VrScreenWindow w = vr_scene_screen_window(&s.filter);
    CHECK(w.ok && near_f(w.left, 0.8f, 1e-5f) && near_f(w.right, 0.8f, 1e-5f) && near_f(w.up, 118.0f / 200.0f, 1e-5f) && near_f(w.down, 138.0f / 200.0f, 1e-5f),
          "ventana de la lista %d %.4f %.4f %.4f %.4f", w.ok, w.left, w.right, w.up, w.down);
    /* bandas: dos lotes kind 2 con y en [0,27) y [210,256) */
    int n2 = 0, bad2 = 0;
    float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
    for (uint32_t i = 0; i < s.nb; i++) {
        if (s.b[i].kind != 2) continue;
        for (uint32_t j = 0; j < s.b[i].count; j++) {
            float y = s.v[s.b[i].first + j].pos[1];
            int band = y < 128.0f ? 0 : 1;
            lo[band] = fminf(lo[band], y); hi[band] = fmaxf(hi[band], y);
            n2++;
            if (!(near_f(y, 0, 1e-4f) || near_f(y, 27, 1e-4f) || near_f(y, 210, 1e-4f) || near_f(y, 256, 1e-4f))) bad2++;
        }
    }
    CHECK(n2 == 12 && bad2 == 0 && near_f(lo[0], 0, 1e-4f) && near_f(hi[0], 27, 1e-4f) && near_f(lo[1], 210, 1e-4f) && near_f(hi[1], 256, 1e-4f),
          "bandas 2D: vértices %d fuera de lugar %d, banda alta [%.1f,%.1f] baja [%.1f,%.1f]", n2, bad2, lo[0], hi[0], lo[1], hi[1]);
    /* 3D: vértice VR -> vista del juego (inversa de game_view) -> ventana de la lista -> fila de la textura 3D; debe caer en SXY.y / 256 (error < 0,5 fila) */
    const float *g = s.game_view;
    float k2 = g[0] * g[0] + g[1] * g[1] + g[2] * g[2];
    int matched = 0, n3 = 0;
    for (uint32_t i = 0; i < s.nb; i++) {
        if (s.b[i].kind == 2) continue;
        for (uint32_t j = 0; j < s.b[i].count; j++) {
            const float *p = s.v[s.b[i].first + j].pos;
            float d[3] = {p[0] - g[12], p[1] - g[13], p[2] - g[14]};
            float vx = (g[0] * d[0] + g[1] * d[1] + g[2] * d[2]) / k2, vy = (g[4] * d[0] + g[5] * d[1] + g[6] * d[2]) / k2, vz = (g[8] * d[0] + g[9] * d[1] + g[10] * d[2]) / k2;
            float col = (w.left + vx / vz) / (w.left + w.right) * 320.0f, row = (w.up + vy / vz) / (w.up + w.down) * 256.0f;   /* v * 256 */
            n3++;
            for (int k = 0; k < 3; k++) if (fabsf(col - (float)sx[k]) < 0.5f && fabsf(row - (float)sy[k]) < 0.5f) { matched++; break; }
        }
    }
    CHECK(n3 == 3 && matched == 3, "3D registra con el panel: %d de %d vértices en SXY (error < 0,5 fila)", matched, n3);
    /* estadísticas del diagnóstico [geom]: un grupo (H,ofx,ofy) con la única primitiva 3D */
    CHECK(s.stats.n_ref_groups == 1 && s.stats.n_ref_total == 1 && s.stats.n_ref_group == 1, "grupos %u total %u dominante %u", s.stats.n_ref_groups, s.stats.n_ref_total, s.stats.n_ref_group);
    vr_scene_release();
}

/* Diagnóstico [geom]: grupos de centro de proyección y cuadriláteros aceptados sin coplanaridad */
static void test_geom_stats(void) {
    const float R[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    const int32_t e[3] = {0, 3000, 500};
    BridgeState bs; set_eye(&bs, R, e);
    VrSceneOpts o = vr_scene_opts_default();
    o.origin_mode = 1; o.coherence = 0;   /* SXY sin rellenar */
    VrScene s;
    reset(); dl.disp.w = 320; dl.disp.h = 256;
    proj_poly(200.0f, 160.0f, 118.0f); proj_poly(200.0f, 160.0f, 118.0f); proj_poly(200.0f, 100.0f, 118.0f);
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_ref_groups == 2 && s.stats.n_ref_total == 3 && s.stats.n_ref_group == 2,
          "grupos %u total %u dominante %u (esperado 2, 3, 2)", s.stats.n_ref_groups, s.stats.n_ref_total, s.stats.n_ref_group);
    const uint8_t z4[4] = {0, 0, 0, 0};
    const float Qc[4][3] = {{0, 0, 2000}, {1000, 0, 2000}, {0, 1000, 2000}, {1000, 1000, 2000}};
    const float Qn[4][3] = {{0, 0, 2000}, {1000, 0, 2000}, {0, 1000, 2000}, {1000, 1000, 2300}};
    reset(); polyN(4, Qc, z4, 0); polyN(4, Qn, z4, 0);
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_quad3d == 2 && s.stats.n_quad_noncoh == 1, "cuadriláteros %u no coplanares %u (esperado 2, 1)", s.stats.n_quad3d, s.stats.n_quad_noncoh);
    vr_scene_release();
}

/* Orden de dibujo del juego (OT): primitivas 3D coplanares (< 5 mm) que se solapan se desplazan hacia el ojo 0,2 mm por metro de distancia (máx. 50 m) según su orden; la posterior gana el z-fight */
static float dist_eye(const VrScene *s, const float *p) {
    return sqrtf((p[0] - s->eye_vr[0]) * (p[0] - s->eye_vr[0]) + (p[1] - s->eye_vr[1]) * (p[1] - s->eye_vr[1]) + (p[2] - s->eye_vr[2]) * (p[2] - s->eye_vr[2]));
}
static void test_coplanar(void) {
    const float R[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    const int32_t e[3] = {0, 3000, 500};
    BridgeState bs; set_eye(&bs, R, e);
    VrSceneOpts o = vr_scene_opts_default();
    o.origin_mode = 1;
    o.coplanar_m = 0.005f;
    VrSceneOpts off = o; off.coplanar_m = 0.0f;
    VrScene s0, s;
    static VrVtx base[64];
    float A[3][3] = {{-1000, -500, 2000}, {1000, -500, 2000}, {0, 1000, 2000}};
    float B[3][3] = {{-200, -100, 2000}, {200, -100, 2000}, {0, 200, 2000}};   /* dentro de A, coplanar */
    float C[3][3] = {{-50, -50, 2000}, {50, -50, 2000}, {0, 50, 2000}};        /* dentro de B */
    /* a) A, B, C coplanares y apiladas: cada una más cerca del ojo que la anterior */
    reset(); poly3(A[0], A[1], A[2], 0); poly3(B[0], B[1], B[2], 0); poly3(C[0], C[1], C[2], 0);
    CHECK(vr_scene_build(&dl, &bs, &off, &s0) == 0 && s0.nv == 9, "build base");
    memcpy(base, s0.v, 9 * sizeof(VrVtx));
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.nv == 9, "build coplanar");
    int a_same = 1, ordered = 1;
    for (int k = 0; k < 3; k++) {
        float dA = dist_eye(&s, s.v[k].pos);
        float sB = dist_eye(&s0, base[3 + k].pos) - dist_eye(&s, s.v[3 + k].pos), sC = dist_eye(&s0, base[6 + k].pos) - dist_eye(&s, s.v[6 + k].pos);   /* acercamiento de cada una */
        a_same &= near_f(dA, dist_eye(&s0, base[k].pos), 1e-6f);
        ordered &= sB > 0.0003f && sC > sB + 0.0003f;   /* ~0,4 mm por nivel a 2 m: B un nivel, C dos */
    }
    CHECK(a_same, "la primera primitiva no se mueve");
    CHECK(ordered, "la posterior queda más cerca del ojo (B 1 nivel, C 2 niveles)");
    CHECK(s.stats.n_coplanar == 3, "pares coplanares %u (esperado 3: B-A, C-A, C-B)", s.stats.n_coplanar);
    /* la posición en pantalla no cambia: el vértice se mueve por su rayo al ojo */
    {
        const float *p0 = base[3].pos, *p1 = s.v[3].pos;
        float a[3], b[3];
        for (int c = 0; c < 3; c++) { a[c] = p0[c] - s.eye_vr[c]; b[c] = p1[c] - s.eye_vr[c]; }
        float la = sqrtf(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]), lb = sqrtf(b[0]*b[0] + b[1]*b[1] + b[2]*b[2]);
        CHECK(near_f(a[0] / la, b[0] / lb, 1e-5f) && near_f(a[1] / la, b[1] / lb, 1e-5f) && near_f(a[2] / la, b[2] / lb, 1e-5f), "mismo rayo al ojo");
    }
    /* b) no coplanar (a 10 cm): sin cambio */
    float B2[3][3] = {{-200, -100, 2100}, {200, -100, 2100}, {0, 200, 2100}};
    reset(); poly3(A[0], A[1], A[2], 0); poly3(B2[0], B2[1], B2[2], 0);
    CHECK(vr_scene_build(&dl, &bs, &off, &s0) == 0 && s0.nv == 6, "build base b");
    memcpy(base, s0.v, 6 * sizeof(VrVtx));
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.nv == 6 && memcmp(base, s.v, 6 * sizeof(VrVtx)) == 0 && s.stats.n_coplanar == 0, "no coplanar: sin cambios");
    /* c) coplanar pero sin solape en el plano: sin cambio */
    float B3[3][3] = {{1500, -500, 2000}, {1900, -500, 2000}, {1700, 0, 2000}};
    reset(); poly3(A[0], A[1], A[2], 0); poly3(B3[0], B3[1], B3[2], 0);
    CHECK(vr_scene_build(&dl, &bs, &off, &s0) == 0 && s0.nv == 6, "build base c");
    memcpy(base, s0.v, 6 * sizeof(VrVtx));
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.nv == 6 && memcmp(base, s.v, 6 * sizeof(VrVtx)) == 0 && s.stats.n_coplanar == 0, "sin solape: sin cambios");
    /* d) mitades de un mismo cuadrilátero (arista compartida, misma primitiva): sin cambio */
    const uint8_t z4[4] = {0, 0, 0, 0};
    const float Q[4][3] = {{0, 0, 2000}, {1000, 0, 2000}, {0, 1000, 2000}, {1000, 1000, 2000}};
    reset(); polyN(4, Q, z4, 0);
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.nv == 6 && s.stats.n_coplanar == 0, "mitades de un cuadrilátero: sin pares (%u)", s.stats.n_coplanar);
    /* e) semitransparentes: no escriben profundidad, no se tocan */
    reset(); poly3(A[0], A[1], A[2], 1); poly3(B[0], B[1], B[2], 1);
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.nv == 6 && s.stats.n_coplanar == 0, "semitransparentes: sin pares (%u)", s.stats.n_coplanar);
    vr_scene_release();
}

/* Crédito sobre el 3D de la intro (ruta ENGINE estéreo): el borrado opaco POSTERIOR al 3D es el relleno de los créditos y se conserva (keep_late_clear); el velo semitransparente
 * de una lista sin 3D no se pierde en fx.covers (clase 3) sino que se pinta como 2D. */
static int op_has_kind2(const VrScene *s, uint32_t op) {
    return s->op_dest && op < s->n_op_dest && s->op_dest[op] == 2;
}
static void test_late_clear_veil(void) {
    const float R[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    const int32_t e[3] = {0, 3000, 500};
    BridgeState bs; set_eye(&bs, R, e);
    float a[3] = {0, 0, 2000}, b[3] = {1000, 0, 2000}, c[3] = {0, 1000, 2000};
    VrScene s;
    /* 1) [3D, TILE opaco 320x240 sin textura, glifo 4bpp] con keep_late_clear: el TILE se emite en la clase 2 */
    reset();
    dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 240;
    poly3(a, b, c, 0);                                                                       /* op0 3D */
    rect2d(0, 0, 320, 240, 0, 0, 0, 0, 0, 0, 0, 0, 0);                                       /* op1 TILE opaco sin textura */
    rect2d(16, 16, 40, 20, 0, 0, GPU_F_TEXTURED, 0, 960, 256, 128, 128, 128);               /* op2 glifo 4bpp */
    VrSceneOpts o = vr_scene_opts_default();
    CHECK(o.keep_late_clear == 0 && o.veil_as_2d == 0, "defectos: keep_late_clear y veil_as_2d desactivados");
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_clear == 1 && !op_has_kind2(&s, 1), "por defecto el TILE tardio se descarta: n_clear %u", s.stats.n_clear);
    o.keep_late_clear = 1;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.has_3d, "build keep_late_clear");
    CHECK(s.stats.n_clear == 0 && op_has_kind2(&s, 1) && op_has_kind2(&s, 2), "TILE tardio emitido: n_clear %u", s.stats.n_clear);
    /* control: el TILE ANTES del 3D (borrado de inicio de lista) se sigue descartando */
    reset();
    dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 240;
    rect2d(0, 0, 320, 240, 0, 0, 0, 0, 0, 0, 0, 0, 0);                                       /* op0 TILE */
    poly3(a, b, c, 0);                                                                       /* op1 3D */
    rect2d(16, 16, 40, 20, 0, 0, GPU_F_TEXTURED, 0, 960, 256, 128, 128, 128);               /* op2 glifo */
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_clear == 1 && !op_has_kind2(&s, 0) && op_has_kind2(&s, 2), "TILE previo al 3D sigue descartado: n_clear %u", s.stats.n_clear);
    /* sin ningun 3D no hay "tardio": se descarta */
    reset();
    dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 240;
    rect2d(0, 0, 320, 240, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_clear == 1 && !op_has_kind2(&s, 0), "TILE sin 3D se descarta: n_clear %u", s.stats.n_clear);

    /* 2) velo semitransparente negro 320x240 con screen_filter=1 en una lista SIN 3D: kind 2 y no cuenta como recubrimiento */
    reset();
    dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 240;
    rect2d(16, 16, 40, 20, 0, 0, GPU_F_TEXTURED, 0, 960, 256, 128, 128, 128);               /* op0 texto */
    rect2d(0, 0, 320, 240, 0, 0, GPU_F_SEMI, 0, 0, 0, 0, 0, 0)->abr = 2;                      /* op1 velo negro */
    o = vr_scene_opts_default(); o.screen_filter = 1;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && !s.has_3d, "build velo sin 3D");
    CHECK(op_has_kind2(&s, 1) && s.stats.n_filter_cover == 0 && s.filter.n_covers == 0 && s.filter.n_cover == 0, "velo sin 3D: kind 2 (cover %u)", s.stats.n_filter_cover);
    /* con 3D en la lista y sin veil_as_2d sigue siendo el recubrimiento de la clase 3 (comportamiento previo) */
    reset();
    dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 240;
    poly3(a, b, c, 0);
    rect2d(0, 0, 320, 240, 0, 0, GPU_F_SEMI, 0, 0, 0, 0, 0, 0)->abr = 2;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.has_3d && s.stats.n_filter_cover == 1 && s.filter.n_covers == 1 && !op_has_kind2(&s, 1), "velo con 3D: clase 3 (cover %u)", s.stats.n_filter_cover);
    /* con veil_as_2d (se compone dentro del estereo) el velo sobre el 3D tambien va como 2D y no se duplica en fx.covers */
    o.veil_as_2d = 1;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.has_3d && s.stats.n_filter_cover == 0 && s.filter.n_covers == 0 && op_has_kind2(&s, 1), "velo con veil_as_2d: kind 2 (cover %u)", s.stats.n_filter_cover);
    vr_scene_release();
}

/* La GPU de PS1 no dibuja un triángulo cuya extensión en pantalla (SXY del paquete con el offset de dibujo) supera 1023 en X o 511 en Y (el juego confía en ello).
 * Un quad son dos triángulos (0,1,2) y (1,2,3), cada uno por su cuenta. Vale para 3D con vista, 2D y líneas; un RECT no puede pasarse (w <= 1023, h <= 511). */
static void set_xy(GpuOp *o, int k, int x, int y) { o->v[k].x = (int16_t)x; o->v[k].y = (int16_t)y; }
static GpuOp *poly2d_n(int nv, uint8_t r) {
    GpuOp *o = add(GPU_OP_POLY);
    o->nv = (uint8_t)nv; o->flags = nv == 4 ? GPU_F_QUAD : 0; o->cx2 = 319; o->cy2 = 255;
    for (int i = 0; i < nv; i++) { o->v[i].r = r; o->v[i].g = 90; o->v[i].b = 40; }
    return o;
}
static void test_hw_cull(void) {
    const float R[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    const int32_t e[3] = {0, 3000, 500};
    BridgeState bs; set_eye(&bs, R, e);
    VrSceneOpts o = vr_scene_opts_default();
    o.origin_mode = 1;
    VrScene s;
    uint8_t fate[16];
    vr_scene_set_fate(fate, sizeof fate);
    const uint8_t z4[4] = {0, 0, 0, 0};
    const float P[4][3] = {{0, 0, 2000}, {1000, 0, 2000}, {0, 1000, 2000}, {1000, 1000, 2000}};
    /* a) triángulo 3D: extensión 1023x511 se dibuja; 1024 en X o 512 en Y, no (con offset de dibujo; el signo/orden de los vértices da igual) */
    static const int ext[5][2] = {{1023, 511}, {1024, 100}, {100, 512}, {1024, 512}, {-1030, 20}};
    for (int c = 0; c < 5; c++) {
        reset();
        GpuOp *p = polyN(3, P, z4, 0);
        p->dox = 320; p->doy = -8;
        set_xy(p, 0, 320 - 500, -8 - 200); set_xy(p, 1, 320 - 500 + ext[c][0], -8 - 200); set_xy(p, 2, 320 - 500, -8 - 200 + ext[c][1]);
        int cull = abs(ext[c][0]) > 1023 || ext[c][1] > 511;
        memset(fate, 0, sizeof fate);
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_hw_cull == (cull ? 1u : 0u) && s.stats.n_tris3d == (cull ? 0u : 1u) && fate[0] == (cull ? VR_FATE_HWCULL : VR_FATE_3D),
              "3D %dx%d: n_hw_cull %u tris3d %u fate %u (descarte esperado %d)", ext[c][0], ext[c][1], s.stats.n_hw_cull, s.stats.n_tris3d, fate[0], cull);
        CHECK(s.stats.n_poly == 1, "el polígono descartado por la GPU cuenta como recibido (n_poly %u)", s.stats.n_poly);
    }
    /* a2) con ventana mostrada conocida (dl.disp): solo se descarta si el bbox del triángulo la toca; fuera de ella se emite (en VR se ve más allá del encuadre) */
    static const int wx[6][2] = {{-1200, -1}, {-1200, 50}, {-1200, 0}, {319, 1500}, {320, 1500}, {-500, 600}};   /* x mín, x máx del triángulo (ventana x 0..319) */
    for (int c = 0; c < 6; c++) {
        reset();
        dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 240;
        GpuOp *p = polyN(3, P, z4, 0);
        set_xy(p, 0, wx[c][0], 100); set_xy(p, 1, wx[c][1], 100); set_xy(p, 2, wx[c][0], 200);
        int cull = wx[c][1] >= 0 && wx[c][0] < 320;
        memset(fate, 0, sizeof fate);
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_hw_cull == (cull ? 1u : 0u) && s.stats.n_tris3d == (cull ? 0u : 1u) && fate[0] == (cull ? VR_FATE_HWCULL : VR_FATE_3D),
              "3D x %d..%d con ventana 0..319: n_hw_cull %u tris3d %u (descarte esperado %d)", wx[c][0], wx[c][1], s.stats.n_hw_cull, s.stats.n_tris3d, cull);
    }
    reset();   /* en Y: ventana 0..239; el triángulo mide 600 en Y (>511) */
    dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 240;
    { GpuOp *p = polyN(3, P, z4, 0); set_xy(p, 0, 10, 239); set_xy(p, 1, 60, 239); set_xy(p, 2, 10, 839); }
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_hw_cull == 1 && s.stats.n_tris3d == 0, "3D que toca la última fila de la ventana (y 239) en Y: descartado (hw_cull %u)", s.stats.n_hw_cull);
    reset();
    dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 240;
    { GpuOp *p = polyN(3, P, z4, 0); set_xy(p, 0, 10, 240); set_xy(p, 1, 60, 240); set_xy(p, 2, 10, 840); }
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_hw_cull == 0 && s.stats.n_tris3d == 1, "3D que empieza justo debajo de la ventana en Y: se emite (hw_cull %u)", s.stats.n_hw_cull);
    /* b) quad 3D: solo el triángulo (1,2,3) pasa de 1023 -> se emite el (0,1,2) y se cuenta 1; y a la inversa */
    for (int w = 0; w < 2; w++) {
        reset();
        GpuOp *q = polyN(4, P, z4, 0);
        set_xy(q, 0, 0, 0); set_xy(q, 1, 100, 0); set_xy(q, 2, 0, 100); set_xy(q, 3, 100, 100);
        if (w == 0) set_xy(q, 3, 1100, 100);        /* (1,2,3) mide 1100 en X; (0,1,2) 100 */
        else { set_xy(q, 0, -1100, 100); }          /* (0,1,2) mide 1200 en X; (1,2,3) 100 */
        memset(fate, 0, sizeof fate);
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_hw_cull == 1 && s.stats.n_tris3d == 1 && s.nv == 3 && fate[0] == VR_FATE_3D, "quad 3D con un triángulo largo (%d): hw_cull %u tris3d %u", w, s.stats.n_hw_cull, s.stats.n_tris3d);
    }
    /* c) polígono 2D (sin vista): el triángulo largo no sale al panel; op_dest 0 si no queda ninguno */
    reset();
    { GpuOp *t = poly2d_n(3, 200); set_xy(t, 0, 10, 10); set_xy(t, 1, 1100, 12); set_xy(t, 2, 20, 90); }
    memset(fate, 0, sizeof fate);
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_hw_cull == 1 && s.nv == 0 && s.op_dest[0] == 0 && fate[0] == VR_FATE_HWCULL, "2D largo: hw_cull %u nv %u dest %u fate %u", s.stats.n_hw_cull, s.nv, s.op_dest[0], fate[0]);
    reset();
    { GpuOp *q = poly2d_n(4, 200); set_xy(q, 0, 10, 10); set_xy(q, 1, 50, 10); set_xy(q, 2, 10, 50); set_xy(q, 3, 50, 600); }   /* (1,2,3) mide 600 en Y */
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_hw_cull == 1 && s.nv == 3 && s.op_dest[0] == 2 && s.stats.n_poly2d == 1, "quad 2D con un triángulo largo: hw_cull %u nv %u dest %u", s.stats.n_hw_cull, s.nv, s.op_dest[0]);
    /* d) línea: |dx| <= 1023 y |dy| <= 511 se dibuja */
    static const int lx[4][2] = {{1023, 511}, {1024, 0}, {0, 512}, {-1100, 5}};
    for (int c = 0; c < 4; c++) {
        reset();
        GpuOp *l = add(GPU_OP_LINE);
        l->nv = 2; set_xy(l, 0, 100, 100); set_xy(l, 1, 100 + lx[c][0], 100 + lx[c][1]);
        int cull = abs(lx[c][0]) > 1023 || abs(lx[c][1]) > 511;
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_hw_cull == (cull ? 1u : 0u) && s.nv == (cull ? 0u : 6u), "línea %d,%d: hw_cull %u nv %u (descarte esperado %d)", lx[c][0], lx[c][1], s.stats.n_hw_cull, s.nv, cull);
    }
    /* e) un RECT del tamaño máximo no se descarta nunca */
    reset();
    rect2d(0, 0, 1023, 511, 0, 0, GPU_F_TEXTURED, 0, 320, 0, 10, 20, 30);
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_hw_cull == 0 && s.nv == 6, "RECT máximo: no se descarta");
    vr_scene_set_fate(NULL, 0);
    vr_scene_release();
}

/* RECT/sprite con su XY procedente de la GTE (v[0].vvalid): opts.rect3d lo emite como cartel 3D (plano perpendicular al eje de la cámara del juego a la profundidad del vértice, w,h píxeles
 * escalados por profundidad/H); sin vista, o con la opción apagada, sigue al plano 2D. */
static GpuOp *rect_hp(int w, int h, float vx, float vy, float vz, float H, float ox, float oy, uint16_t flags) {
    GpuOp *o = rect2d((int)floorf(ox + H * vx / vz), (int)floorf(oy + H * vy / vz), w, h, 10, 20, flags, 1, 320, 256, 128, 100, 60);
    o->proj_h = H; o->proj_ox = ox; o->proj_oy = oy;
    GpuVtx *v = &o->v[0];
    v->vvalid = 1; v->vx = vx; v->vy = vy; v->vz = vz; v->fvalid = 1; v->fz = vz;
    return o;
}
static int find_corner(const VrScene *s, const float want[3], float u, float v) {
    for (uint32_t i = 0; i < s->nv; i++)
        if (near_f(s->v[i].pos[0], want[0], 1e-4f) && near_f(s->v[i].pos[1], want[1], 1e-4f) && near_f(s->v[i].pos[2], want[2], 1e-4f) && near_f(s->v[i].uv[0], u, 1e-4f) && near_f(s->v[i].uv[1], v, 1e-4f)) return 1;
    return 0;
}
static void test_rect3d(void) {
    const float R[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    const int32_t e[3] = {0, 3000, 500};
    BridgeState bs; set_eye(&bs, R, e);
    VrSceneOpts o = vr_scene_opts_default();
    o.origin_mode = 1;
    VrScene s;
    const float H = 360.0f, vx = 300.0f, vy = -150.0f, vz = 1800.0f;
    /* a) por defecto (rect3d = 0): plano 2D como siempre, aunque traiga vista */
    reset(); dl.disp.w = 320; dl.disp.h = 240;
    rect_hp(16, 8, vx, vy, vz, H, 160, 120, GPU_F_TEXTURED);
    CHECK(o.rect3d == 0 && vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 0 && s.stats.n_rect3d == 0 && s.nv == 6 && (s.v[0].flags & VR_F_2D) && s.op_dest[0] == 2, "rect3d apagado: 2D");
    CHECK(s.stats.n_rect_hp == 1, "n_rect_hp cuenta el RECT con vista (%u)", s.stats.n_rect_hp);
    /* b) rect3d = 1: cartel 3D opaco; las 4 esquinas en (vx + dx*vz/H, vy + dy*vz/H, vz) con uv (u0,v0)..(u0+w,v0+h) */
    o.rect3d = 1;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_tris3d == 2 && s.stats.n_rect3d == 1 && s.stats.n_rect == 1 && s.nv == 6 && s.has_3d && s.nb == 1 && s.b[0].kind == 0 && s.b[0].blend == 0xFF, "cartel 3D: tris %u nb %u", s.stats.n_tris3d, s.nb);
    CHECK(s.op_dest[0] == 0, "el cartel 3D no va al panel (op_dest %u)", s.op_dest[0]);
    {
        int ok = 1;
        for (uint32_t i = 0; i < s.nv; i++) ok &= !(s.v[i].flags & VR_F_2D) && (s.v[i].flags & VR_F_RECT) && (s.v[i].flags & GPU_F_TEXTURED);
        CHECK(ok, "flags: 3D (sin VR_F_2D), VR_F_RECT, textura");
        static const int dxy[4][2] = {{0, 0}, {16, 0}, {0, 8}, {16, 8}};
        for (int k = 0; k < 4; k++) {
            float cv[3] = {vx + dxy[k][0] * vz / H, vy + dxy[k][1] * vz / H, vz}, want[3];
            view_to_vr(&s, cv, want);
            CHECK(find_corner(&s, want, 10.0f + dxy[k][0], 20.0f + dxy[k][1]), "esquina %d del cartel", k);
        }
        for (uint32_t t = 0; t < s.nv / 3; t++) {   /* el frente mira a la cámara del juego */
            const VrVtx *v = &s.v[3 * t];
            float e1[3], e2[3], n[3], tc[3];
            for (int c = 0; c < 3; c++) { e1[c] = v[1].pos[c] - v[0].pos[c]; e2[c] = v[2].pos[c] - v[0].pos[c]; tc[c] = s.eye_vr[c] - v[0].pos[c]; }
            n[0] = e1[1]*e2[2]-e1[2]*e2[1]; n[1] = e1[2]*e2[0]-e1[0]*e2[2]; n[2] = e1[0]*e2[1]-e1[1]*e2[0];
            CHECK(n[0]*tc[0] + n[1]*tc[1] + n[2]*tc[2] > 0, "cartel: frente hacia la cámara (tri %u)", t);
        }
        CHECK(near_f(s.v[0].rgba & 255, 128, 0.5f) && ((s.v[0].rgba >> 24) & 255) == 255, "color del rect");
    }
    /* c) semitransparente con volteo en X: lote 3D semitransparente, u decrece hacia la derecha */
    reset(); dl.disp.w = 320; dl.disp.h = 240;
    { GpuOp *r = rect_hp(16, 8, vx, vy, vz, H, 160, 120, GPU_F_TEXTURED | GPU_F_SEMI | GPU_F_FLIPX); r->abr = 2; }
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_rect3d == 1 && s.nb == 1 && s.b[0].kind == 1 && s.b[0].blend == 2, "cartel semitransparente: kind 1 blend 2");
    {
        float cv[3] = {vx + 16 * vz / H, vy, vz}, want[3];
        view_to_vr(&s, cv, want);
        CHECK(find_corner(&s, want, 10.0f - 16.0f, 20.0f) && (s.v[0].flags & GPU_F_FLIPX), "volteo X: u0 - w en la esquina derecha y bandera conservada");
    }
    /* d) sin vista (vvalid = 0), o con vista dudosa (banderas), sigue al plano 2D */
    reset(); dl.disp.w = 320; dl.disp.h = 240;
    rect_hp(16, 8, vx, vy, vz, H, 160, 120, GPU_F_TEXTURED)->v[0].vvalid = 0;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_rect3d == 0 && s.nv == 6 && (s.v[0].flags & VR_F_2D) && s.op_dest[0] == 2 && s.stats.n_rect_hp == 0, "sin vista: 2D");
    reset(); dl.disp.w = 320; dl.disp.h = 240;
    rect_hp(16, 8, vx, vy, vz, H, 160, 120, GPU_F_TEXTURED)->v[0].vflags = GPU_VF_AMBIG;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_rect3d == 0 && s.nv == 6 && (s.v[0].flags & VR_F_2D), "vista AMBIG: 2D");
    /* e) vista que no reproduce su XY (x,y del paquete lejos de la proyección): 2D, no se fía */
    reset(); dl.disp.w = 320; dl.disp.h = 240;
    rect_hp(16, 8, vx, vy, vz, H, 160, 120, GPU_F_TEXTURED)->v[0].x += 40;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_rect3d == 0 && s.nv == 6 && (s.v[0].flags & VR_F_2D), "vista incoherente con el XY: 2D");
    /* f) desplazamiento de proyección (OFX) distinto del centro del mundo: se corrige como en los polígonos */
    reset(); dl.disp.w = 320; dl.disp.h = 240;
    proj_poly(360.0f, 160.0f, 120.0f);                                   /* op0: fija el centro del mundo en (160,120) */
    rect_hp(16, 8, vx, vy, vz, H, 200, 120, GPU_F_TEXTURED);            /* op1: su OFX es 200 */
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_rect3d == 1 && s.stats.n_ofs_fix >= 1, "OFX distinto: rect3d %u ofs_fix %u", s.stats.n_rect3d, s.stats.n_ofs_fix);
    {
        float cv[3] = {vx + (200.0f - 160.0f) * vz / H, vy, vz}, want[3];
        view_to_vr(&s, cv, want);
        CHECK(find_corner(&s, want, 10.0f, 20.0f), "esquina con el OFX corregido");
    }
    /* g) detrás del plano cercano del motor con engine_near_clip: descartado y contado (no vuelve al 2D) */
    reset(); dl.disp.w = 320; dl.disp.h = 240;
    rect_hp(16, 8, 10.0f, 5.0f, 250.0f, H, 160, 120, GPU_F_TEXTURED);   /* z = 250 < near_units (300); H >= 2z no satura la división de la GTE */
    o.engine_near_clip = 1; o.near_units = 300.0f;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_rect3d == 0 && s.nv == 0 && s.stats.drop_behind == 1 && s.op_dest[0] == 0, "cartel detrás del plano cercano: descartado (behind %u nv %u)", s.stats.drop_behind, s.nv);
    /* g2) con eye_near_m el plano cercano no es near_units: el cartel a z=250 se emite */
    o.engine_near_clip = 0; o.eye_near_m = 0.10f;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_rect3d == 1 && s.stats.drop_behind == 0, "cartel entre el ojo y near_units con eye_near_m: emitido (rect3d %u behind %u)", s.stats.n_rect3d, s.stats.drop_behind);
    vr_scene_release();
}

/* Carteles (VR_F_BILLBOARD): marcado en vr_scene_build y cálculo de las esquinas por ojo (vr_billboard_corner, la misma matemática que vr_vs_scene en render/metal/vr_scene.metal). */
static double dot3d(const double *a, const double *b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
static void bb_edge(const float a[3], const float b[3], double d[3]) { for (int i = 0; i < 3; i++) d[i] = (double)b[i] - (double)a[i]; }
static void test_billboard(void) {
    /* a) ojo a partir de P*V (Z invertida, proyección asimétrica, ojo girado y desplazado) */
    {
        const double eyep[3] = {1.5, 1.7, -2.25}, th = 0.6, c = cos(th), sn = sin(th);
        double Rt[16] = {c, 0, sn, 0,  0, 1, 0, 0,  -sn, 0, c, 0,  0, 0, 0, 1}, T[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, -eyep[0], -eyep[1], -eyep[2], 1}, V[16], P[16], M[16];
        for (int cI = 0; cI < 4; cI++) for (int r = 0; r < 4; r++) { double a = 0; for (int k = 0; k < 4; k++) a += Rt[k * 4 + r] * T[cI * 4 + k]; V[cI * 4 + r] = a; }
        memset(P, 0, sizeof P);
        P[0] = 1.1; P[5] = 1.3; P[8] = 0.07; P[9] = -0.04; P[10] = 0.0001; P[11] = -1; P[14] = 0.05;
        for (int cI = 0; cI < 4; cI++) for (int r = 0; r < 4; r++) { double a = 0; for (int k = 0; k < 4; k++) a += P[k * 4 + r] * V[cI * 4 + k]; M[cI * 4 + r] = a; }
        double e[3];
        CHECK(vr_eye_from_viewproj(M, e) && near_f((float)e[0], 1.5f, 1e-6f) && near_f((float)e[1], 1.7f, 1e-6f) && near_f((float)e[2], -2.25f, 1e-6f), "ojo desde P*V: %.6f %.6f %.6f", e[0], e[1], e[2]);
        double Z[16]; memset(Z, 0, sizeof Z);
        CHECK(!vr_eye_from_viewproj(Z, e), "matriz singular");
    }
    /* b) esquinas: base de la cámara del juego en el mundo VR (derecha +X, abajo -Y, delante -Z), centro a 5 m, semiejes 0,5 x 0,3 m */
    {
        const float bb[12] = {0, 2, -5,  1, 0, 0,  0, -1, 0,  0, 0, -1};
        const float hw = 0.5f, hh = 0.3f;
        const float P[4][3] = {{-hw, 2 + hh, -5}, {hw, 2 + hh, -5}, {-hw, 2 - hh, -5}, {hw, 2 - hh, -5}};   /* arriba-izq, arriba-der, abajo-izq, abajo-der */
        float out[4][3];
        /* ojo sobre el eje (el del juego): sin cambios */
        const float e0[3] = {0, 2, 0};
        for (int k = 0; k < 4; k++) { vr_billboard_corner(bb, e0, P[k], out[k]); CHECK(near_f(out[k][0], P[k][0], 1e-5f) && near_f(out[k][1], P[k][1], 1e-5f) && near_f(out[k][2], P[k][2], 1e-5f), "ojo en el eje: esquina %d", k); }
        /* ojos girados/desplazados: el cartel queda perpendicular al rayo ojo->centro, con los mismos semiejes y la misma distancia */
        const float eyes[4][3] = {{3, 2, 0}, {-4, 4, -1}, {0.1f, 2, -3}, {2, -1, -9}};
        for (int ei = 0; ei < 4; ei++) {
            const float *e = eyes[ei];
            double f[3] = {bb[0] - e[0], bb[1] - e[1], bb[2] - e[2]}, fl = sqrt(dot3d(f, f));
            for (int i = 0; i < 3; i++) f[i] /= fl;
            for (int k = 0; k < 4; k++) vr_billboard_corner(bb, e, P[k], out[k]);
            double ctr[3] = {0, 0, 0};
            for (int k = 0; k < 4; k++) { double d[3] = {out[k][0] - bb[0], out[k][1] - bb[1], out[k][2] - bb[2]}; CHECK(fabs(dot3d(d, f)) < 1e-5, "ojo %d esquina %d fuera del plano perpendicular al rayo (%g)", ei, k, dot3d(d, f)); for (int i = 0; i < 3; i++) ctr[i] += 0.25 * out[k][i]; }
            CHECK(fabs(ctr[0] - bb[0]) < 1e-5 && fabs(ctr[1] - bb[1]) < 1e-5 && fabs(ctr[2] - bb[2]) < 1e-5, "ojo %d centro movido", ei);
            double ex[3], ey[3], ox[3], oy[3];
            bb_edge(out[0], out[1], ex); bb_edge(out[0], out[2], ey); bb_edge(P[0], P[1], ox); bb_edge(P[0], P[2], oy);
            CHECK(fabs(sqrt(dot3d(ex, ex)) - 2 * hw) < 1e-5 && fabs(sqrt(dot3d(ey, ey)) - 2 * hh) < 1e-5, "ojo %d semiejes %.5f %.5f", ei, sqrt(dot3d(ex, ex)), sqrt(dot3d(ey, ey)));
            CHECK(fabs(dot3d(ex, ey)) < 1e-5, "ojo %d ya no es un rectángulo", ei);   /* el rectángulo sigue siéndolo: sin elipse ni cizalla */
            double nx[3] = {ex[1]*ey[2] - ex[2]*ey[1], ex[2]*ey[0] - ex[0]*ey[2], ex[0]*ey[1] - ex[1]*ey[0]}, no[3] = {ox[1]*oy[2] - ox[2]*oy[1], ox[2]*oy[0] - ox[0]*oy[2], ox[0]*oy[1] - ox[1]*oy[0]};
            CHECK(dot3d(nx, f) * dot3d(no, (const double[3]){bb[9], bb[10], bb[11]}) > 0, "ojo %d cambió el sentido del giro (cara delantera)", ei);
            /* tamaño angular: el semieje horizontal sobre la distancia es el mismo que el original visto desde el eje */
            CHECK(fabs(sqrt(dot3d(ex, ex)) / fl - 2 * hw / fl) < 1e-6, "ojo %d tamaño angular", ei);
        }
        /* degenerados: ojo sobre el centro, o rayo opuesto a la normal: la esquina original */
        const float ec[3] = {0, 2, -5}, eo[3] = {0, 2, -10};
        vr_billboard_corner(bb, ec, P[1], out[0]); CHECK(near_f(out[0][0], P[1][0], 1e-6f) && near_f(out[0][2], P[1][2], 1e-6f), "ojo sobre el centro");
        vr_billboard_corner(bb, eo, P[1], out[0]); CHECK(near_f(out[0][0], P[1][0], 1e-6f) && near_f(out[0][2], P[1][2], 1e-6f), "rayo opuesto");
    }
    /* c) marcado en vr_scene_build */
    {
        const float R[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
        const int32_t e[3] = {0, 3000, 500};
        BridgeState bs; set_eye(&bs, R, e);
        VrSceneOpts o = vr_scene_opts_default(); o.origin_mode = 1;
        VrScene s;
        float P[4][3] = {{-200, -200, 2000}, {200, -200, 2000}, {-200, 200, 2000}, {200, 200, 2000}};
        uint8_t fl[4] = {0, GPU_VF_DERIVED, GPU_VF_DERIVED, 0};
        #define BBQUAD(g) reset(); { g = polyN(4, P, fl, 0); g->flags |= GPU_F_SEMI; }
        GpuOp *g;
        BBQUAD(g); (void)g;
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_billboard == 0 && s.nstates == 1, "billboard=0: nada (n %u estados %u)", s.stats.n_billboard, s.nstates);
        for (uint32_t i = 0; i < s.nv; i++) CHECK(!(s.v[i].flags & VR_F_BILLBOARD), "billboard=0: vértice %u marcado", i);
        o.billboard = 1;
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_billboard == 1 && s.nv == 6 && s.nstates == 2 && s.stats.n_tris3d == 2, "cuadrilátero 2+2: n %u nv %u estados %u", s.stats.n_billboard, s.nv, s.nstates);
        int all = 1; for (uint32_t i = 0; i < s.nv; i++) all &= (s.v[i].flags & VR_F_BILLBOARD) != 0 && s.v[i].pad == s.v[0].pad;
        CHECK(all, "los 6 vértices llevan VR_F_BILLBOARD y el mismo estado");
        const VrState *st = vr_scene_state(&s, s.v[0].pad);
        CHECK(st != NULL, "estado");
        if (st) {
            /* centro = (0, 3, -1,5) (test_basic: vista (0,0,2000)); N = adelante de la cámara del juego = -Z; R = +X; D = -Y */
            CHECK(near_f(st->bb[0], 0, 1e-4f) && near_f(st->bb[1], 3.0f, 1e-4f) && near_f(st->bb[2], -1.5f, 1e-4f), "centro %.4f %.4f %.4f", st->bb[0], st->bb[1], st->bb[2]);
            CHECK(near_f(st->bb[3], 1, 1e-5f) && near_f(st->bb[7], -1, 1e-5f) && near_f(st->bb[11], -1, 1e-5f), "ejes R(%.2f) D(%.2f) N(%.2f)", st->bb[3], st->bb[7], st->bb[11]);
            /* desde el ojo del juego y centrado en el eje: las esquinas no cambian; con el ojo 1 m a la derecha y 2 m delante, el cartel queda perpendicular al rayo */
            float o1[3], eyeg[3] = {s.eye_vr[0], s.eye_vr[1], s.eye_vr[2]};
            int same = 1; for (uint32_t i = 0; i < s.nv; i++) { vr_billboard_corner(st->bb, eyeg, s.v[i].pos, o1); same &= near_f(o1[0], s.v[i].pos[0], 1e-4f) && near_f(o1[1], s.v[i].pos[1], 1e-4f) && near_f(o1[2], s.v[i].pos[2], 1e-4f); }
            CHECK(same, "ojo del juego: sin cambios");
            float eh[3] = {eyeg[0] + 1.0f, eyeg[1], eyeg[2] - 2.0f};
            double f[3] = {st->bb[0] - eh[0], st->bb[1] - eh[1], st->bb[2] - eh[2]}, fl2 = sqrt(dot3d(f, f)); for (int i = 0; i < 3; i++) f[i] /= fl2;
            int perp = 1; for (uint32_t i = 0; i < s.nv; i++) { vr_billboard_corner(st->bb, eh, s.v[i].pos, o1); double d[3] = {o1[0] - st->bb[0], o1[1] - st->bb[1], o1[2] - st->bb[2]}; perp &= fabs(dot3d(d, f)) < 1e-4; }
            CHECK(perp, "ojo desplazado: perpendicular al rayo");
        }
        /* UV, color y mezcla intactos respecto a la salida sin cartel */
        VrScene s0; VrSceneOpts o0 = o; o0.billboard = 0;
        BBQUAD(g); CHECK(vr_scene_build(&dl, &bs, &o0, &s0) == 0 && s0.nv == 6, "referencia");
        { static VrVtx ref[8]; memcpy(ref, s0.v, 6 * sizeof ref[0]);
          CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.nv == 6, "con cartel");
          int ok = 1; for (int i = 0; i < 6; i++) ok &= !memcmp(ref[i].pos, s.v[i].pos, sizeof ref[i].pos) && ref[i].uv[0] == s.v[i].uv[0] && ref[i].uv[1] == s.v[i].uv[1] && ref[i].rgba == s.v[i].rgba && ref[i].clut == s.v[i].clut && ref[i].tpage == s.v[i].tpage && (ref[i].flags | VR_F_BILLBOARD) == s.v[i].flags;
          CHECK(ok, "posición, UV, color y mezcla sin cambios (solo el flag)"); }
        /* no se marca: opaco, una sola esquina derivada, esquina real saturada/ambigua, profundidades distintas, triángulo */
        BBQUAD(g); g->flags &= (uint16_t)~GPU_F_SEMI;
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_billboard == 0, "opaco no");
        { uint8_t f1[4] = {0, GPU_VF_DERIVED, 0, 0}; reset(); g = polyN(4, P, f1, 0); g->flags |= GPU_F_SEMI; }
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_billboard == 0, "una derivada no");
        { uint8_t f3[4] = {0, GPU_VF_DERIVED, GPU_VF_DERIVED | GPU_VF_AMBIG, 0}; reset(); g = polyN(4, P, f3, 0); g->flags |= GPU_F_SEMI; }
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_billboard == 0, "derivada con otra bandera no");
        { uint8_t f4[4] = {GPU_VF_SAT, GPU_VF_DERIVED, GPU_VF_DERIVED, 0}; reset(); g = polyN(4, P, f4, 0); g->flags |= GPU_F_SEMI; }
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_billboard == 0, "esquina real saturada no (centro no fiable)");
        { float Q[4][3]; memcpy(Q, P, sizeof Q); Q[3][2] = 2600; reset(); g = polyN(4, Q, fl, 0); g->flags |= GPU_F_SEMI; }
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_billboard == 0, "profundidades distintas no");
        { float Q[4][3]; memcpy(Q, P, sizeof Q); Q[3][2] = 2100; reset(); g = polyN(4, Q, fl, 0); g->flags |= GPU_F_SEMI; }
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_billboard == 1, "profundidades parecidas sí");
        { uint8_t f2[4] = {0, GPU_VF_DERIVED, GPU_VF_DERIVED, 0}; reset(); g = polyN(3, P, f2, 0); g->flags |= GPU_F_SEMI; }
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.stats.n_billboard == 0, "triángulo no");
        #undef BBQUAD
    }
    /* d) muchos carteles (cada uno con su estado) no agotan la tabla de estados de las primitivas normales */
    {
        enum { NBB = 600, NNORM = 12 };
        const float R[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
        const int32_t e[3] = {0, 3000, 500};
        BridgeState bs; set_eye(&bs, R, e);
        VrSceneOpts o = vr_scene_opts_default(); o.origin_mode = 1; o.billboard = 1;
        GpuOp *big = calloc(NBB + NNORM + 1, sizeof *big);
        uint8_t fl[4] = {0, GPU_VF_DERIVED, GPU_VF_DERIVED, 0};
        uint32_t nbig = 0;
        for (int k = 0; k < NBB + NNORM; k++) {
            reset();
            if (k < NNORM / 2 || k >= NBB + NNORM / 2) {   /* normales: unas antes y otras después de los carteles */
                float a[3] = {0, 0, 2000}, b[3] = {200, 0, 2000}, c[3] = {0, 200, 2000};
                GpuOp *g = poly3(a, b, c, 0);
                g->cx1 = 0; g->cy1 = 0; g->cx2 = (int16_t)(1000 + (k < NNORM / 2 ? k : k - NBB)); g->cy2 = 239;
            } else {
                int kk = k - NNORM / 2;
                float P[4][3] = {{-50, -50, 2000}, {50, -50, 2000}, {-50, 50, 2000}, {50, 50, 2000}};
                for (int j = 0; j < 4; j++) { P[j][0] += (float)(kk % 30) * 120; P[j][1] += (float)(kk / 30) * 120; }
                GpuOp *g = polyN(4, P, fl, 0); g->flags |= GPU_F_SEMI;
            }
            big[nbig++] = ops[0];
        }
        dl = (GpuDrawList){0}; dl.ops = big; dl.nops = nbig;
        VrScene s;
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0, "600 carteles: build");
        CHECK(s.stats.n_billboard == NBB, "los %d carteles se marcan (%u)", NBB, s.stats.n_billboard);
        int found[NNORM] = {0}, lost = 0;
        for (uint32_t i = 0; i < s.nv; i++) {
            if (s.v[i].flags & VR_F_BILLBOARD) continue;
            const VrState *st = s.v[i].pad == 0xFFFF ? NULL : vr_scene_state(&s, s.v[i].pad);
            if (!st) { lost++; continue; }
            int idx = st->cx2 >= 1000 && st->cx2 < 1000 + NNORM ? st->cx2 - 1000 : -1;
            if (idx >= 0) found[idx] = 1;
        }
        int nf = 0; for (int i = 0; i < NNORM; i++) nf += found[i];
        CHECK(lost == 0 && nf == NNORM, "las primitivas normales conservan su estado (sin estado %d, con su estado %d/%d)", lost, nf, NNORM);
        free(big);
    }
    vr_scene_release();
}

/* Vista de mundo (drop_world2d): (A5) un cuadrilátero de HUD (relleno de la barra de vida) con un vértice con vista por coincidencia de SXY (hp_por_valor) no debe perderse ni arrastrar al HUD anterior;
 * (A1) las bandas negras opacas de ancho de pantalla (máscara del agua) no van al panel (el nativo dibuja el mundo), el HUD pequeño sí; sin drop_world2d (cinemática) las bandas se conservan. */
static void test_world_view_hud(void) {
    const float R[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    const int32_t e[3] = {0, 3000, 500};
    BridgeState bs; set_eye(&bs, R, e);
    float a[3] = {0, 0, 2000}, b[3] = {1000, 0, 2000}, c[3] = {0, 1000, 2000};
    for (int on = 0; on < 2; on++) {
        reset(); dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 256;
        poly3(a, b, c, 0);                                                   /* op0: 3D completo */
        for (int q = 0; q < 2; q++) {                                        /* op1: relleno limpio; op2: relleno con 1 vértice con vista (falso positivo) */
            GpuOp *h = add(GPU_OP_POLY); h->nv = 4; h->flags = GPU_F_GOURAUD;
            for (int i = 0; i < 4; i++) { h->v[i].x = (int16_t)(16 + (i & 1 ? 32 : 0)); h->v[i].y = (int16_t)(17 + q * 20 + (i & 2 ? 5 : 0)); }
            if (q) setv3(&h->v[1], 10, 20, 3000);
        }
        rect2d(0, 0, 320, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0);                     /* op3: banda negra opaca superior */
        rect2d(0, 250, 320, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0);                   /* op4: banda inferior */
        rect2d(16, 28, 32, 5, 0, 0, 0, 0, 0, 0, 0, 24, 24);                  /* op5: fondo de la barra (rect pequeño) */
        VrSceneOpts o = vr_scene_opts_default();
        o.origin_mode = 1; o.drop_clear = 1; o.drop_world2d = on;
        VrScene s;
        CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.op_dest, "world_view_hud: build (%d)", on);
        if (on) {
            CHECK(s.op_dest[1] == 2 && s.op_dest[2] == 2 && s.stats.drop_partial == 0, "A5: relleno limpio %u y con vértice falso %u en el panel (esperado 2 y 2), parciales %u", s.op_dest[1], s.op_dest[2], s.stats.drop_partial);
            CHECK(s.op_dest[3] == 0 && s.op_dest[4] == 0 && s.op_dest[5] == 2, "A1: bandas %u %u (esperado 0 0), fondo de barra %u (esperado 2)", s.op_dest[3], s.op_dest[4], s.op_dest[5]);
        } else
            CHECK(s.op_dest[3] == 2 && s.op_dest[4] == 2, "cinemática (sin drop_world2d): las bandas siguen en el panel (%u %u)", s.op_dest[3], s.op_dest[4]);
    }
    vr_scene_release();
}

/* Vista de mundo: un parcial con >=2 vértices válidos tras el último 3D se descarta (no va al panel 2D); solo <=1 (coincidencia de SXY) va al HUD.
 * La banda opaca de ancho de pantalla solo se descarta si además es baja (<=10% del alto): un rect 300x40 se queda en el panel. */
static void test_world_view_partial_band(void) {
    const float R[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    const int32_t e[3] = {0, 3000, 500};
    BridgeState bs; set_eye(&bs, R, e);
    float a[3] = {0, 0, 2000}, b[3] = {1000, 0, 2000}, c[3] = {0, 1000, 2000};
    reset(); dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 256;
    poly3(a, b, c, 0);                                                       /* op0: 3D completo */
    GpuOp *h = add(GPU_OP_POLY); h->nv = 4; h->flags = GPU_F_GOURAUD;        /* op1: 2 de 4 vértices válidos */
    for (int i = 0; i < 4; i++) { h->v[i].x = (int16_t)(16 + (i & 1 ? 32 : 0)); h->v[i].y = (int16_t)(17 + (i & 2 ? 5 : 0)); }
    setv3(&h->v[0], 10, 20, 3000); setv3(&h->v[1], 20, 20, 3000);
    rect2d(10, 100, 300, 40, 0, 0, 0, 0, 0, 0, 0, 0, 0);                     /* op2: 300x40 opaco */
    rect2d(0, 250, 320, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0);                       /* op3: 320x6 opaco */
    VrSceneOpts o = vr_scene_opts_default();
    o.origin_mode = 1; o.drop_clear = 1; o.drop_world2d = 1;
    VrScene s;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0 && s.op_dest, "partial_band: build");
    CHECK(s.op_dest[1] != 2 && s.stats.drop_partial == 1, "parcial con 2 válidos no va al 2D (dest %u, parciales %u)", s.op_dest[1], s.stats.drop_partial);
    CHECK(s.op_dest[2] == 2, "rect 300x40 opaco se queda en el panel (%u)", s.op_dest[2]);
    CHECK(s.op_dest[3] == 0, "rect 320x6 sigue descartado (%u)", s.op_dest[3]);
    vr_scene_release();
}

int main(void) {
    test_billboard();
    test_diag_dirty();
    test_hw_cull();
    test_rect3d();
    test_screen_window();
    test_line3d();
    test_basic();
    test_stability();
    test_classes_and_events();
    test_drops();
    test_state();
    test_near_clip();
    test_filter();
    test_late_clear_veil();
    test_ghost();
    test_gte_scale_and_validity();
    test_line_coverage();
    test_event_epochs();
    test_feedback();
    test_ofs_fix();
    test_post_emu();
    test_goggles();
    test_goggles_emu();
    test_recon_fate();
    test_recon_diag_ambig();
    test_recon_ambig_quad();
    test_user_map();
    test_coherence();

    test_compose_view();
    test_clip_2d();

    test_user_map_linear();
    test_gpu_eye_near();
    test_cine_letterbox();
    test_geom_stats();
    test_coplanar();
    test_world_view_hud();
    test_world_view_partial_band();
    if (fails) { fprintf(stderr, "%d fallos\n", fails); return 1; }
    printf("test_scene: OK\n");
    return 0;
}
