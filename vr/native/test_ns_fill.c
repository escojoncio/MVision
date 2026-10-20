/* Pruebas sintéticas de ns_fill (sin datos del juego): casar/rellenar, cámaras con escala vertical, pegado a la cámara, pose del jugador, puertas de fiabilidad,
 * persistencia (retirada por vista, edad y corte), textura corregida, triángulos y cuadriláteros, y el hilo trabajador (ns_fill_glue). */
#include "ns_fill.h"
#include "ns_fill_glue.h"
#include "ns_gpu.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FALLO %s:%d: %s ", __FILE__, __LINE__, #c); printf("" __VA_ARGS__); printf("\n"); fails++; } } while (0)

#define TX 640
#define TY 256
#define CLX 912
#define CLY 242
#define PAGE ((TX / 64) | ((TY / 256) << 4))
#define CLUT ((CLX / 16) | (CLY << 6))

typedef struct { NsRTri *t; NsAdaptSrc *s; size_t n, cap; } Nat;
static void nat_push(Nat *N, const float p[3][3], const float uv[3][2], uint16_t page, uint16_t clut, uint32_t key) {
    if (N->n == N->cap) { N->cap = N->cap ? N->cap * 2 : 16; N->t = (NsRTri *)realloc(N->t, N->cap * sizeof *N->t); N->s = (NsAdaptSrc *)realloc(N->s, N->cap * sizeof *N->s); }
    NsRTri *T = &N->t[N->n]; NsAdaptSrc *S = &N->s[N->n++];
    memset(T, 0, sizeof *T); memset(S, 0, sizeof *S);
    for (int c = 0; c < 3; c++) { memcpy(T->v[c].p, p[c], 12); T->v[c].u = uv[c][0]; T->v[c].v = uv[c][1]; T->v[c].r = T->v[c].g = T->v[c].b = 128; }
    T->tpage = page; T->clut = clut; T->flags = NS_TF_TEXTURED;
    S->key = key; S->mesh_id = 0xC0DE0000u; S->piece = S->piece_rel = (uint32_t)N->n;
}
/* cuadrilátero en el plano z = zc de lado s (esquina x0,y0), como lo emite ns_adapt: (A,B,D) y (B,D,C) */
static void nat_quad(Nat *N, float x0, float y0, float zc, float s, uint16_t page, uint16_t clut, uint32_t key) {
    float A[3] = {x0, y0, zc}, B[3] = {x0 + s, y0, zc}, C[3] = {x0 + s, y0 + s, zc}, D[3] = {x0, y0 + s, zc};
    float ua[2] = {0, 0}, ub[2] = {63, 0}, uc[2] = {63, 63}, ud[2] = {0, 63}, P[3][3], U[3][2];
    memcpy(P[0], A, 12); memcpy(P[1], B, 12); memcpy(P[2], D, 12); memcpy(U[0], ua, 8); memcpy(U[1], ub, 8); memcpy(U[2], ud, 8); nat_push(N, P, U, page, clut, key);
    memcpy(P[0], B, 12); memcpy(P[1], D, 12); memcpy(P[2], C, 12); memcpy(U[0], ub, 8); memcpy(U[1], ud, 8); memcpy(U[2], uc, 8); nat_push(N, P, U, page, clut, key);
}

/* operación de la lista con las posiciones de vista dadas (A,B,D,C) y su pantalla proyectada con H = 160, centro (160,128) */
static GpuOp list_quad(const float v[4][3], int semi, int tx, int clx) {
    static const float uv[4][2] = {{0, 0}, {63, 0}, {0, 63}, {63, 63}};
    GpuOp o; memset(&o, 0, sizeof o);
    o.type = GPU_OP_POLY; o.nv = 4;
    o.flags = (uint16_t)(GPU_F_GOURAUD | GPU_F_TEXTURED | (semi ? GPU_F_SEMI : 0) | GPU_F_QUAD);
    o.tx = (uint16_t)tx; o.ty = TY; o.clx = (uint16_t)clx; o.cly = CLY;
    o.proj_h = 160; o.dox = 160; o.doy = 128;
    for (int k = 0; k < 4; k++) {
        GpuVtx *g = &o.v[k];
        g->vvalid = 1; g->vx = v[k][0]; g->vy = v[k][1]; g->vz = v[k][2];
        g->u = (uint8_t)uv[k][0]; g->v = (uint8_t)uv[k][1]; g->r = g->g = g->b = 128;
        g->fvalid = 1; g->fx = 160 + 160 * v[k][0] / v[k][2]; g->fy = 128 + 160 * v[k][1] / v[k][2];
        g->x = (int16_t)lroundf(g->fx); g->y = (int16_t)lroundf(g->fy);
    }
    return o;
}
static GpuOp list_of(float x0, float y0, float zc, float s, int semi) {
    float v[4][3] = {{x0, y0, zc}, {x0 + s, y0, zc}, {x0, y0 + s, zc}, {x0 + s, y0 + s, zc}};
    return list_quad(v, semi, TX, CLX);
}
static NsFillCam cam_id(void) { NsFillCam c; memset(&c, 0, sizeof c); c.R[0] = c.R[4] = c.R[8] = 1; c.ys = 1; return c; }
/* un fotograma; native puede estar vacío */
static int step(NsFill *f, const Nat *N, const GpuOp *ops, uint32_t nops, const NsFillCam *cam, const NsFillCam *alt, NsFillCfg *cfg, uint32_t frame, NsFillStats *st) {
    NsFillNative nat = {N ? N->t : NULL, N ? N->s : NULL, N ? N->n : 0, NULL};
    return ns_fill_run(f, &nat, ops, nops, NULL, cam, alt, cfg, frame, st);
}

static void test_fill_and_match(void) {
    NsFill *f = ns_fill_create(); NsFillCfg cfg; ns_fill_cfg_default(&cfg); NsFillCam cam = cam_id(); NsFillStats st;
    Nat N = {0};
    nat_quad(&N, -500, -500, 4000, 1000, PAGE, CLUT, 0xAAAA0001u);
    GpuOp ops[2] = {list_of(-500, -500, 4000, 1000, 0), list_of(3000, -500, 5000, 1000, 1)};   /* la primera existe en el nativo, la segunda no */
    CHECK(step(f, &N, ops, 2, &cam, NULL, &cfg, 1, &st) == 0, "run");
    CHECK(st.tris == 4 && st.cls[NF_MATCH] == 2 && st.filled_tris == 2, "tris=%u match=%u relleno=%u", st.tris, st.cls[NF_MATCH], st.filled_tris);
    uint32_t n; const NsEffect *e = ns_fill_effects(f, &n);
    CHECK(n >= 1 && e && e[0].kind == NS_EK_QUAD, "n=%u", n);
    if (n >= 1) {
        CHECK((e[0].flags & NS_EF_SEMI) && (e[0].flags & NS_EF_TEXTURED) && e[0].tpage == PAGE && e[0].clut == CLUT, "flags=%x tpage=%x clut=%x", e[0].flags, e[0].tpage, e[0].clut);
        float err = 0;
        for (int k = 0; k < e[0].nv && k < 4; k++) err = fmaxf(err, fabsf(e[0].p[k][2] - 5000));
        CHECK(err < 1.0f, "z mundo err=%.2f", err);
        CHECK(e[0].p[0][0] > 2999 && e[0].p[0][0] < 4001, "x mundo %.1f", e[0].p[0][0]);
    }
    CHECK(fabsf(st.cover_pct - 100.0f) < 1e-3f && st.nat_pct < 51.0f, "cover=%.1f nat=%.1f", st.cover_pct, st.nat_pct);
    ns_fill_destroy(f); free(N.t); free(N.s);
}

/* cámara con rotación y escala vertical 66/64: la desproyección debe devolver el mundo exacto */
static void test_camera_ys(void) {
    NsFill *f = ns_fill_create(); NsFillCfg cfg; ns_fill_cfg_default(&cfg); NsFillStats st;
    const float ang = 0.5f, ys = 66.0f / 64.0f;
    NsFillCam cam; memset(&cam, 0, sizeof cam);
    cam.eye[0] = 1200; cam.eye[1] = -300; cam.eye[2] = 700; cam.ys = ys;
    cam.R[0] = cosf(ang); cam.R[2] = sinf(ang); cam.R[4] = 1; cam.R[6] = -sinf(ang); cam.R[8] = cosf(ang);   /* vista->mundo */
    /* cuadrilátero en el mundo, lo proyectamos con A = diag(1,ys,1) R^T */
    float W[4][3] = {{2000, -800, 5000}, {3000, -800, 5000}, {2000, 200, 5000}, {3000, 200, 5000}}, V[4][3];
    for (int k = 0; k < 4; k++) {
        float d[3] = {W[k][0] - cam.eye[0], W[k][1] - cam.eye[1], W[k][2] - cam.eye[2]};
        for (int a = 0; a < 3; a++) V[k][a] = cam.R[0 + a] * d[0] + cam.R[3 + a] * d[1] + cam.R[6 + a] * d[2];   /* R^T d */
        V[k][1] *= ys;
    }
    GpuOp op = list_quad(V, 0, TX, CLX);
    CHECK(step(f, NULL, &op, 1, &cam, NULL, &cfg, 1, &st) == 0, "run");
    CHECK(st.filled_tris == 2, "relleno=%u", st.filled_tris);
    uint32_t n; const NsEffect *e = ns_fill_effects(f, &n);
    float err = 1e9f;
    if (n >= 1) { err = 0; for (int k = 0; k < 4; k++) for (int a = 0; a < 3; a++) err = fmaxf(err, fabsf(e[0].p[k][a] - W[k][a])); }
    CHECK(n >= 1 && err < 1.0f, "n=%u err=%.3f", n, err);
    /* sin ys el error en Y es del 3 %: confirma que la prueba distingue */
    NsFillCam c2 = cam; c2.ys = 1; ns_fill_reset(f);
    step(f, NULL, &op, 1, &c2, NULL, &cfg, 1, &st);
    e = ns_fill_effects(f, &n);
    float err2 = 0;
    if (n >= 1) for (int k = 0; k < 4; k++) for (int a = 0; a < 3; a++) err2 = fmaxf(err2, fabsf(e[0].p[k][a] - W[k][a]));
    CHECK(err2 > 5.0f || n == 0, "con ys=1 el error debe verse: %.2f", err2);
    ns_fill_destroy(f);
}

static void test_locked_alt_cam(void) {
    NsFill *f = ns_fill_create(); NsFillCfg cfg; ns_fill_cfg_default(&cfg); NsFillStats st;
    NsFillCam cam = cam_id(), alt = cam_id();
    cam.eye[0] = 2000;   /* la cámara de la lista está desplazada 2000: el nativo (con la alternativa) coincide en vista, no en mundo */
    Nat N = {0};
    /* mundo con la cámara alt (ojo 0): vista = mundo. Con la de la lista (ojo 2000) la misma vista cae en x+2000 */
    nat_quad(&N, -500, -500, 4000, 1000, PAGE, CLUT, 0xAAAA0002u);
    GpuOp op = list_of(-500, -500, 4000, 1000, 0);
    step(f, &N, &op, 1, &cam, &alt, &cfg, 1, &st);
    CHECK(st.cls[NF_LOCKED] == 2 && st.filled_tris == 0, "locked=%u relleno=%u", st.cls[NF_LOCKED], st.filled_tris);
    ns_fill_reset(f);
    step(f, &N, &op, 1, &cam, NULL, &cfg, 1, &st);
    CHECK(st.filled_tris == 2 && st.cls[NF_LOCKED] == 0, "sin alt debe rellenar: relleno=%u", st.filled_tris);
    ns_fill_destroy(f); free(N.t); free(N.s);
}

static void test_pose_player_and_texfix(void) {
    NsFill *f = ns_fill_create(); NsFillCfg cfg; ns_fill_cfg_default(&cfg); NsFillStats st; NsFillCam cam = cam_id();
    Nat N = {0};
    nat_quad(&N, 2500, -500, 4000, 1000, PAGE, CLUT, 0xBEEF0001u);   /* el jugador en otra pose, misma textura y UV */
    cfg.player_key = 0xBEEF0001u;
    GpuOp op = list_of(-500, -500, 4000, 1000, 0);
    step(f, &N, &op, 1, &cam, NULL, &cfg, 1, &st);
    CHECK(st.cls[NF_POSE_PLAYER] == 2 && st.filled_tris == 0, "pose_jugador=%u relleno=%u", st.cls[NF_POSE_PLAYER], st.filled_tris);
    CHECK(fabsf(st.cover_pct - 100.0f) < 1e-3f || st.tris - st.cls[NF_POSE_PLAYER] == 0, "la pose no cuenta en la cobertura: %.1f", st.cover_pct);
    /* otro dinámico (no es el jugador): se detecta como pose solo si el nativo lo marca dinámico; sin sc no hay dinámicos y se rellena */
    cfg.player_key = 0; ns_fill_reset(f);
    step(f, &N, &op, 1, &cam, NULL, &cfg, 1, &st);
    CHECK(st.filled_tris == 2, "sin jugador ni dinámico se rellena: %u", st.filled_tris);
    free(N.t); free(N.s); memset(&N, 0, sizeof N);
    /* textura corregida: el nativo está en el sitio pero con otra página */
    nat_quad(&N, -500, -500, 4000, 1000, (uint16_t)(PAGE + 1), CLUT, 0xAAAA0003u);
    ns_fill_reset(f);
    step(f, &N, &op, 1, &cam, NULL, &cfg, 1, &st);
    CHECK(st.cls[NF_FILL_TEXFIX] == 2 && st.filled_tris == 2, "texfix=%u", st.cls[NF_FILL_TEXFIX]);
    uint32_t n; const NsEffect *e = ns_fill_effects(f, &n);
    CHECK(n >= 1 && e[0].depth_bias == cfg.bias && e[0].tpage == PAGE, "bias=%d", n ? e[0].depth_bias : -99);
    /* si esa pareja nativa es del jugador no se corrige: MATCH normal */
    cfg.player_key = 0xAAAA0003u; ns_fill_reset(f);
    step(f, &N, &op, 1, &cam, NULL, &cfg, 1, &st);
    CHECK(st.cls[NF_FILL_TEXFIX] == 0 && st.cls[NF_MATCH] == 2, "jugador: texfix=%u match=%u", st.cls[NF_FILL_TEXFIX], st.cls[NF_MATCH]);
    ns_fill_destroy(f); free(N.t); free(N.s);
}

static void test_gates_and_2d(void) {
    NsFill *f = ns_fill_create(); NsFillCfg cfg; ns_fill_cfg_default(&cfg); NsFillStats st; NsFillCam cam = cam_id();
    GpuOp ops[3];
    ops[0] = list_of(-500, -500, 4000, 1000, 0); for (int k = 0; k < 4; k++) ops[0].v[k].vflags = GPU_VF_BEHIND;   /* la GTE marca el vértice detrás de la cámara */
    ops[1] = list_of(-500, -500, 4000, 1000, 0); for (int k = 0; k < 4; k++) ops[1].v[k].vvalid = 0;               /* 2D */
    ops[2] = list_of(-50000, -50000, 4000, 100000, 0);                                                            /* arista desproporcionada (coherente con su pantalla) */
    step(f, NULL, ops, 3, &cam, NULL, &cfg, 1, &st);
    CHECK(st.filled_tris == 0, "ninguna debe rellenarse: %u", st.filled_tris);
    CHECK(st.no_view >= 1 && st.cls[NF_SKIP_BEHIND] >= 1, "sin_vista=%u detras=%u", st.no_view, st.cls[NF_SKIP_BEHIND]);
    CHECK(st.cls[NF_SKIP_EDGE] + st.cls[NF_SKIP_FAR] + st.cls[NF_SKIP_NAN] + st.cls[NF_SKIP_INCOH] >= 1, "arista/lejos/incoh");
    /* un vértice con vista corrupta pero pantalla correcta se reconstruye (plano de los otros tres ∩ rayo) y sí se rellena */
    GpuOp rc = list_of(-500, -500, 4000, 1000, 0); rc.v[0].vx = 9e6f;
    ns_fill_reset(f); step(f, NULL, &rc, 1, &cam, NULL, &cfg, 2, &st);
    CHECK(st.recon >= 1 && st.filled_tris == 2, "reconstruido=%u relleno=%u", st.recon, st.filled_tris);
    ns_fill_destroy(f);
}

static void test_persistence(void) {
    NsFill *f = ns_fill_create(); NsFillCfg cfg; ns_fill_cfg_default(&cfg); cfg.keep_frames = 5; NsFillStats st; NsFillCam cam = cam_id();
    /* sitio dentro de la vista */
    GpuOp in = list_of(-500, -500, 4000, 1000, 0);
    step(f, NULL, &in, 1, &cam, NULL, &cfg, 1, &st);
    CHECK(st.filled_tris == 2, "relleno=%u", st.filled_tris);
    /* siguiente fotograma: el motor ya no lo dibuja y su sitio sigue en la vista (con otra operación lejos que sí tiene vista): se retira */
    GpuOp other = list_of(-100, -100, 8000, 200, 0);
    step(f, NULL, &other, 1, &cam, NULL, &cfg, 2, &st);
    CHECK(st.retired_view >= 1, "retirado por vista=%u", st.retired_view);
    /* sitio FUERA de la vista (x lejos): al girar la cabeza se conserva keep_frames, luego se retira por edad */
    ns_fill_reset(f);
    GpuOp edge = list_of(30000, -500, 4000, 1000, 0);   /* pantalla x = 160 + 160*30000/4000 = 1360: fuera de 320 */
    step(f, NULL, &edge, 1, &cam, NULL, &cfg, 10, &st);
    CHECK(st.filled_tris == 2, "relleno borde=%u", st.filled_tris);
    uint32_t n0; ns_fill_effects(f, &n0);
    step(f, NULL, &other, 1, &cam, NULL, &cfg, 12, &st);
    uint32_t n1; ns_fill_effects(f, &n1);
    CHECK(st.persisted >= 1 && n1 >= n0, "persistido=%u n0=%u n1=%u", st.persisted, n0, n1);
    step(f, NULL, &other, 1, &cam, NULL, &cfg, 40, &st);
    CHECK(st.retired_age >= 1, "retirado por edad=%u", st.retired_age);
    /* corte de cámara */
    ns_fill_reset(f);
    step(f, NULL, &edge, 1, &cam, NULL, &cfg, 50, &st);
    NsFillCam far = cam; far.eye[0] = 50000;
    step(f, NULL, &other, 1, &far, NULL, &cfg, 51, &st);
    CHECK(st.retired_cut >= 1, "retirado por corte=%u", st.retired_cut);
    ns_fill_destroy(f);
}

static void test_triangle_and_format(void) {
    NsFill *f = ns_fill_create(); NsFillCfg cfg; ns_fill_cfg_default(&cfg); NsFillStats st; NsFillCam cam = cam_id();
    GpuOp op = list_of(-500, -500, 4000, 1000, 0); op.nv = 3; op.flags &= (uint16_t)~GPU_F_QUAD;
    step(f, NULL, &op, 1, &cam, NULL, &cfg, 1, &st);
    CHECK(st.tris == 1 && st.filled_tris == 1, "tris=%u relleno=%u", st.tris, st.filled_tris);
    uint32_t n; const NsEffect *e = ns_fill_effects(f, &n);
    CHECK(n == 1 && e[0].nv == 3, "nv=%d", n ? e[0].nv : -1);
    char buf[2048]; size_t l = ns_fill_format(&st, 1, buf, sizeof buf);
    CHECK(l > 20 && l < sizeof buf, "formato len=%zu", l);
    CHECK(strstr(buf, "camara=lista") != NULL, "el registro dice qué cámara usó: %s", buf);
    st.cam_src = 2; ns_fill_format(&st, 1, buf, sizeof buf);
    CHECK(strstr(buf, "camara=RAM(sin camara en la lista)") != NULL, "cámara de la RAM por falta de registro: %s", buf);
    ns_fill_destroy(f);
}

void ns_fill_glue_test_set(const NsEffect *e, uint32_t n, uint32_t res_frame, uint32_t sub_frame);

/* Relleno fantasma: el resultado caduca si el trabajador lleva > 20 fotogramas sin ejecutar, y una lista sin 3D lo vacía. */
static void test_glue_stale(void) {
    NsEffect out[16], one; memset(&one, 0, sizeof one); one.nv = 3;
    static uint8_t ram[0x200000]; uint8_t scr[1024]; memset(scr, 0, sizeof scr);
    ns_fill_glue_test_set(&one, 1, 100, 110);
    CHECK(ns_fill_glue_fetch(out, 16) == 1, "reciente: se sirve");
    ns_fill_glue_test_set(&one, 1, 100, 120);
    CHECK(ns_fill_glue_fetch(out, 16) == 1, "20 fotogramas: aún se sirve");
    ns_fill_glue_test_set(&one, 1, 100, 121);
    CHECK(ns_fill_glue_fetch(out, 16) == 0, "21 fotogramas sin ejecución: no se sirve (relleno fantasma)");
    ns_fill_glue_test_set(&one, 1, 500, 10);
    CHECK(ns_fill_glue_fetch(out, 16) == 0, "contador retrocedido (carga de partida): no se sirve");
    ns_fill_glue_test_set(&one, 1, 100, 105);
    CHECK(ns_fill_glue_fetch(out, 16) == 1, "de nuevo vigente");
    GpuOp op2d = list_of(-500, -500, 4000, 1000, 0); op2d.v[0].vvalid = 0;
    ns_fill_glue_submit_cam(ram, scr, &op2d, 1, 106, NULL, NULL, 0);
    CHECK(ns_fill_glue_fetch(out, 16) == 0, "lista sin 3D: vaciado");
}

static void test_glue(void) {
    NsEffect out[16];
    CHECK(ns_fill_glue_fetch(out, 16) == 0, "sin trabajo: nada");
    static uint8_t ram[0x200000]; uint8_t scr[1024]; memset(scr, 0, sizeof scr);
    GpuOp op = list_of(-500, -500, 4000, 1000, 0);
    float eye[3] = {0, 0, 0}, M[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    ns_fill_glue_post_ik(ram);
    ns_fill_glue_submit_cam(ram, scr, &op, 1, 1, eye, M, 0);
    ns_fill_glue_submit_cam(ram, scr, &op, 1, 2, NULL, NULL, 1);   /* puede omitirse si el trabajador sigue ocupado: sin cola, sin bloqueo */
    struct timespec ts = {0, 50 * 1000000}; nanosleep(&ts, NULL);
    ns_fill_glue_fetch(out, 16);   /* con RAM vacía no hay escena: no debe fallar ni colgarse */
    ns_fill_glue_reset();
    CHECK(ns_fill_glue_fetch(out, 16) == 0, "tras reset: nada");
}

/* INTRO: escena 3D del motor sin partida y con el nativo vacío o en otro sitio (cobertura del nativo solo ~0 %): el relleno la repone y la cobertura total (la que decide motor / respaldo)
 * sale alta; el glue la publica con ns_fill_glue_last. La escena que el relleno NO puede reponer (sin posición fiable) mantiene la cobertura total baja. */
void ns_fill_glue_test_cover(const NsFillStats *st, uint32_t frame);
static void test_intro_cover(void) {
    NsFill *f = ns_fill_create(); NsFillCfg cfg; ns_fill_cfg_default(&cfg); NsFillCam cam = cam_id(); NsFillStats st;
    GpuOp ops[24];
    for (int i = 0; i < 24; i++) ops[i] = list_of(-3000 + 250 * i, -500, 4000 + 40 * i, 200, 0);
    Nat N = {0};
    nat_quad(&N, 90000, 90000, 4000, 1000, PAGE, CLUT, 0xAAAA0009u);   /* el nativo existe pero no está en la vista de la lista */
    CHECK(step(f, &N, ops, 24, &cam, NULL, &cfg, 1, &st) == 0, "run intro");
    CHECK(st.nat_pct < 1.0f && st.cover_pct > 95.0f, "intro: nativo %.1f%% -> nativo+relleno %.1f%%", st.nat_pct, st.cover_pct);
    uint32_t fr = 0; int pct = -1;
    ns_fill_glue_last(&fr, &pct);   /* sin medida previa no falla */
    ns_fill_glue_test_cover(&st, 813);
    CHECK(ns_fill_glue_last(&fr, &pct) == 1 && fr == 813 && pct >= 95 && pct <= 100, "glue_last: fr=%u pct=%d", fr, pct);
    /* pocas primitivas: no hay escena que medir (cover_pct vale 100 sin denominador) y no pisa la medida buena */
    NsFillStats few; memset(&few, 0, sizeof few); few.tris = 3; few.cover_pct = 100.0f;
    ns_fill_glue_test_cover(&few, 900);
    CHECK(ns_fill_glue_last(&fr, &pct) == 1 && fr == 813, "sin escena suficiente: no cuenta (fr=%u)", fr);
    /* solo pose (jugador): tampoco */
    NsFillStats po; memset(&po, 0, sizeof po); po.tris = 40; po.cls[NF_POSE_PLAYER] = 36; po.cover_pct = 100.0f;
    ns_fill_glue_test_cover(&po, 950);
    CHECK(fr == 813 && ns_fill_glue_last(&fr, &pct) == 1 && fr == 813, "solo pose: no cuenta");
    /* escena que el relleno no puede reponer (todo detrás de la cámara): cobertura total baja */
    GpuOp bad[24];
    for (int i = 0; i < 24; i++) { bad[i] = list_of(-3000 + 250 * i, -500, 4000, 200, 0); for (int k = 0; k < 4; k++) bad[i].v[k].vflags = GPU_VF_BEHIND; }
    ns_fill_reset(f);
    CHECK(step(f, NULL, bad, 24, &cam, NULL, &cfg, 2, &st) == 0, "run detras");
    CHECK(st.cover_pct < 20.0f, "tris detrás de la cámara: cobertura total %.1f%% (baja: respaldo)", st.cover_pct);
    ns_fill_destroy(f); free(N.t); free(N.s);
}

/* ---- ancla al mundo y pegado a la cabeza ---- */
/* cámara que gira `yaw` alrededor del eje Y del motor (vista -> mundo fila-mayor: columnas X derecha, Y abajo, Z delante) */
static NsFillCam cam_yaw(float yaw_deg, float ex) {
    NsFillCam c = cam_id(); float a = yaw_deg * 3.14159265f / 180.0f;
    c.R[0] = cosf(a); c.R[2] = sinf(a); c.R[6] = -sinf(a); c.R[8] = cosf(a); c.eye[0] = ex;
    return c;
}
static void world_to_view(const NsFillCam *c, const float w[3], float v[3]) {   /* v = R^T (w - eye) */
    float d[3] = {w[0] - c->eye[0], w[1] - c->eye[1], w[2] - c->eye[2]};
    for (int r = 0; r < 3; r++) v[r] = c->R[r] * d[0] + c->R[3 + r] * d[1] + c->R[6 + r] * d[2];
}
static GpuOp list_world_quad(const NsFillCam *c, float x0, float y0, float zc, float s) {   /* cuadrilátero del mundo visto desde la cámara */
    float w[4][3] = {{x0, y0, zc}, {x0 + s, y0, zc}, {x0, y0 + s, zc}, {x0 + s, y0 + s, zc}}, v[4][3];
    for (int k = 0; k < 4; k++) world_to_view(c, w[k], v[k]);
    return list_quad(v, 0, TX, CLX);
}
static void centroid(const NsEffect *e, float c[3]) {
    c[0] = c[1] = c[2] = 0;
    for (int k = 0; k < e->nv; k++) for (int a = 0; a < 3; a++) c[a] += e->p[k][a] / (float)e->nv;
}
static void test_world_anchor_and_headlock(void) {
    NsFill *fw = ns_fill_create(), *fl = ns_fill_create(); NsFillCfg cfg; ns_fill_cfg_default(&cfg); NsFillStats st;
    float cw0[3] = {0, 0, 0}, dev = 0, move_lock = 0; uint32_t key_w = 0, key_l = 0; int same_w = 1, same_l = 1; float cl0[3] = {0, 0, 0};
    /* cámara que gira 4 grados por fotograma (y avanza 20 u): el relleno del mundo no se mueve; el de la vista fija sí (el motor lo recoloca) y conserva la identidad */
    GpuOp locked_ref;
    for (int t = 0; t < 14; t++) {
        NsFillCam cam = cam_yaw(4.0f * (float)t, 20.0f * (float)t);
        GpuOp w = list_world_quad(&cam, 1500, -400, 6000, 700);
        step(fw, NULL, &w, 1, &cam, NULL, &cfg, (uint32_t)(10 + t), &st);
        uint32_t n; const NsEffect *e = ns_fill_effects(fw, &n);
        CHECK(n == 1 && e, "mundo: salidas=%u (sin estelas)", n);
        if (n == 1) {
            float c[3]; centroid(&e[0], c);
            if (t == 0) { memcpy(cw0, c, sizeof c); key_w = e[0].src_key; }
            else { float d = sqrtf((c[0] - cw0[0]) * (c[0] - cw0[0]) + (c[1] - cw0[1]) * (c[1] - cw0[1]) + (c[2] - cw0[2]) * (c[2] - cw0[2])); if (d > dev) dev = d; if (e[0].src_key != key_w) same_w = 0; }
        }
        /* pegado a la cámara: misma posición de VISTA en todos los fotogramas (sitio de vista (200,-100,1200)) */
        float v[4][3] = {{200, -100, 1200}, {600, -100, 1200}, {200, 300, 1200}, {600, 300, 1200}};
        GpuOp l = list_quad(v, 0, TX, CLX); locked_ref = l;
        step(fl, NULL, &l, 1, &cam, NULL, &cfg, (uint32_t)(10 + t), &st);
        e = ns_fill_effects(fl, &n);
        CHECK(n == 1 && e, "pegado: salidas=%u (la identidad por vista evita las estelas)", n);
        if (n == 1) {
            float c[3]; centroid(&e[0], c);
            if (t == 0) { memcpy(cl0, c, sizeof c); key_l = e[0].src_key; }
            else { float d = sqrtf((c[0] - cl0[0]) * (c[0] - cl0[0]) + (c[1] - cl0[1]) * (c[1] - cl0[1]) + (c[2] - cl0[2]) * (c[2] - cl0[2])); if (d > move_lock) move_lock = d; if (e[0].src_key != key_l) same_l = 0; }
        }
    }
    (void)locked_ref;
    CHECK(dev < 2.0f, "relleno de mundo con la cabeza girando: el sitio en el mundo es constante (desviacion %.2f u)", dev);
    CHECK(same_w, "el relleno de mundo conserva la identidad");
    CHECK(move_lock > 100.0f, "lo pegado a la vista cambia de sitio en el mundo (%.0f u): es lo que delata que sigue a la cabeza", move_lock);
    CHECK(same_l && (key_l >> 24) == 0xF1u, "lo pegado a la vista conserva la identidad estable aunque el mundo cambie");
    ns_fill_destroy(fw); ns_fill_destroy(fl);
}
static void test_near_skip(void) {
    NsFill *f = ns_fill_create(); NsFillCfg cfg; ns_fill_cfg_default(&cfg); NsFillStats st; NsFillCam cam = cam_id();
    float v[4][3] = {{-300, -300, 100}, {300, -300, 100}, {-300, 300, 100}, {300, 300, 100}};   /* centro a 100 u del ojo, 60 cm de lado */
    GpuOp near = list_quad(v, 0, TX, CLX);
    step(f, NULL, &near, 1, &cam, NULL, &cfg, 1, &st);
    CHECK(st.filled_tris == 0 && st.cls[NF_SKIP_NEAR] == 2, "junto a la cabeza: no se rellena (junto=%u relleno=%u)", st.cls[NF_SKIP_NEAR], st.filled_tris);
    GpuOp far = list_of(-500, -500, 4000, 1000, 0);
    step(f, NULL, &far, 1, &cam, NULL, &cfg, 2, &st);
    CHECK(st.filled_tris == 2 && st.cls[NF_SKIP_NEAR] == 0, "lejos: se rellena");
    cfg.near_skip = 0;
    ns_fill_reset(f); step(f, NULL, &near, 1, &cam, NULL, &cfg, 3, &st);
    CHECK(st.filled_tris == 2, "regla apagada: se rellena (%u)", st.filled_tris);
    ns_fill_destroy(f);
}
/* Tubería completa: relleno -> ns_gpu_fx_build con seguimiento. Mundo: dibujado y fijo con la cabeza girando; pegado a la vista: se detecta y se descarta. */
static void test_pipeline_follow(void) {
    NsFill *f = ns_fill_create(); NsFillCfg cfg; ns_fill_cfg_default(&cfg); NsFillStats st;
    NsGpuFxTrack *trk = ns_gpu_fx_track_create();
    static const float to_vr[12] = {0.001f, 0, 0, 0, 0, 0.001f, 0, 0, 0, 0, 0.001f, 0};
    NsCamera nc; memset(&nc, 0, sizeof nc); nc.rot[0] = nc.rot[4] = nc.rot[8] = 1; nc.h = 160; nc.overscan_y = 1;
    int drawn_w = 0, drawn_l_last = 1, follow_fill = 0; float devw = 0, cprev[3] = {0, 0, 0}; int have = 0;
    for (int t = 0; t < 40; t++) {
        NsFillCam cam = cam_yaw(3.0f * (float)t, 10.0f * (float)t);
        GpuOp ops[2];
        ops[0] = list_world_quad(&cam, 1500, -400, 6000, 700);
        float v[4][3] = {{200, -100, 1200}, {600, -100, 1200}, {200, 300, 1200}, {600, 300, 1200}};
        ops[1] = list_quad(v, 0, TX, CLX);
        step(f, NULL, ops, 2, &cam, NULL, &cfg, (uint32_t)(10 + t), &st);
        uint32_t n; const NsEffect *e = ns_fill_effects(f, &n);
        CHECK(n == 2, "salidas=%u", n);
        NsEffect all[4]; memcpy(all, e, sizeof(NsEffect) * (n < 4 ? n : 4));
        NsGpuFxParams P; memset(&P, 0, sizeof P);
        P.to_vr = to_vr; P.cam = &nc; P.track = trk; P.hold_near_m = 0.3f; P.n_eyes = 1;
        for (int a = 0; a < 3; a++) { P.head[a] = cam.eye[a] * 0.001f; P.head_right[a] = cam.R[3 * a]; P.head_up[a] = cam.R[3 * a + 1]; P.eyes[0][a] = P.head[a]; }
        NsGpuFxVert vb[64]; NsGpuFxRun runs[4]; uint64_t keys[4]; NsGpuFxOut o;
        CHECK(ns_gpu_fx_build(all, n, &P, vb, 64, runs, 4, keys, 4, &o) == 0, "build");
        uint32_t nverts = o.n_opaque + o.n_semi;
        if (t == 39) drawn_l_last = nverts >= 12;   /* si el pegado se dibujara habria dos cuadrilateros (12 vertices) */
        if (o.n_follow_fill) follow_fill = 1;
        if (nverts >= 6) {   /* la primera salida dibujada es la de mundo (se emite primero) */
            drawn_w++;
            float c[3] = {0, 0, 0};
            for (int k = 0; k < 6; k++) for (int a = 0; a < 3; a++) c[a] += vb[k].p[a] / 6.0f;
            if (have) { float d = sqrtf((c[0] - cprev[0]) * (c[0] - cprev[0]) + (c[1] - cprev[1]) * (c[1] - cprev[1]) + (c[2] - cprev[2]) * (c[2] - cprev[2])); if (d > devw) devw = d; }
            memcpy(cprev, c, sizeof c); have = 1;
        }
    }
    CHECK(drawn_w >= 38 && devw < 0.003f, "mundo: dibujado siempre y fijo en el mundo VR con la cabeza girando y avanzando (desviacion %.5f m, dibujado %d)", devw, drawn_w);
    CHECK(follow_fill && !drawn_l_last, "lo pegado a la vista (relleno) se detecta como SIGUE y se descarta");
    ns_gpu_fx_track_destroy(trk);
    ns_fill_destroy(f);
}

int main(void) {
    test_intro_cover();
    test_fill_and_match();
    test_camera_ys();
    test_locked_alt_cam();
    test_pose_player_and_texfix();
    test_gates_and_2d();
    test_persistence();
    test_triangle_and_format();
    test_world_anchor_and_headlock();
    test_near_skip();
    test_pipeline_follow();
    test_glue();
    test_glue_stale();
    if (fails) { printf("test_ns_fill: %d FALLOS\n", fails); return 1; }
    printf("test_ns_fill: OK\n");
    return 0;
}
