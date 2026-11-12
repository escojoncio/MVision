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
    uint32_t n; const NsEffect *e = ns_fill_effects(f, &n);
    CHECK(st.cls[NF_FILL_TEXFIX] == 2 && st.filled_tris == 0 && n == 0, "textura corregida: se cuenta pero NO se dibuja por defecto (copia coplanar de la malla nativa): texfix=%u relleno=%u salidas=%u", st.cls[NF_FILL_TEXFIX], st.filled_tris, n);
    CHECK(st.cover_pct < 1.0f, "textura corregida sin dibujar: no cuenta en la cobertura (%.1f%%)", st.cover_pct);
    cfg.draw_texfix = 1; ns_fill_reset(f);
    step(f, &N, &op, 1, &cam, NULL, &cfg, 1, &st);
    CHECK(st.cls[NF_FILL_TEXFIX] == 2 && st.filled_tris == 2, "draw_texfix=1: se dibuja, texfix=%u", st.cls[NF_FILL_TEXFIX]);
    CHECK(st.cover_pct > 99.0f, "textura corregida dibujada: cuenta en la cobertura (%.1f%%)", st.cover_pct);
    e = ns_fill_effects(f, &n);
    CHECK(n >= 1 && e[0].depth_bias == cfg.bias && e[0].tpage == PAGE, "bias=%d", n ? e[0].depth_bias : -99);
    cfg.draw_texfix = 0;
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
    /* M1: el reinicio (cambio de escena / de clase de vista) borra la medida de cobertura: la de la escena anterior no sobrevive */
    ns_fill_glue_reset();
    CHECK(ns_fill_glue_last(&fr, &pct) == 0, "M1: tras ns_fill_glue_reset no hay medida");
    ns_fill_glue_test_cover(&st, 1200);
    CHECK(ns_fill_glue_last(&fr, &pct) == 1 && fr == 1200 && pct >= 95, "M1: la medida siguiente la repone");
    /* j-render: escenario nativo inservible (nativo < 10 %, nativo + relleno >= 80 %) */
    CHECK(ns_fill_glue_scenery_stale() == 1, "escenario inservible: nativo %.1f%% y total %.1f%%", st.nat_pct, st.cover_pct);
    { NsFillStats ok = st; ok.nat_pct = 85.0f; ok.cover_pct = 98.0f; ns_fill_glue_test_cover(&ok, 1300);
      CHECK(ns_fill_glue_scenery_stale() == 0, "escenario que casa (85 %%): se dibuja"); }
    ns_fill_glue_reset();
    CHECK(ns_fill_glue_scenery_stale() == 0, "sin medida: no se descarta nada");
    ns_fill_glue_test_cover(&st, 1400);
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
    float v[4][3] = {{-150, -150, 100}, {150, -150, 100}, {-150, 150, 100}, {150, 150, 100}};   /* centro a 100 u del ojo, 30 cm de lado: pequeño */
    GpuOp near = list_quad(v, 0, TX, CLX);
    step(f, NULL, &near, 1, &cam, NULL, &cfg, 1, &st);
    CHECK(st.filled_tris == 0 && st.cls[NF_SKIP_NEAR] == 2, "junto a la cabeza: no se rellena (junto=%u relleno=%u)", st.cls[NF_SKIP_NEAR], st.filled_tris);
    GpuOp far = list_of(-500, -500, 4000, 1000, 0);
    step(f, NULL, &far, 1, &cam, NULL, &cfg, 2, &st);
    CHECK(st.filled_tris == 2 && st.cls[NF_SKIP_NEAR] == 0, "lejos: se rellena");
    /* M7: una pared grande (aristas > 50 cm) que pasa a 10 cm del ojo NUNCA se omite */
    float wl[4][3] = {{-300, -300, 100}, {300, -300, 100}, {-300, 300, 100}, {300, 300, 100}};
    GpuOp wall = list_quad(wl, 0, TX, CLX);
    ns_fill_reset(f); step(f, NULL, &wall, 1, &cam, NULL, &cfg, 3, &st);
    CHECK(st.cls[NF_SKIP_NEAR] == 0 && st.filled_tris == 2, "M7: pared grande a 10 cm: no se omite (junto=%u relleno=%u)", st.cls[NF_SKIP_NEAR], st.filled_tris);
    /* M7: la distancia es al punto MÁS CERCANO del triángulo, no a su centro: A=(0,0,220) a 220 u, centro del primer triángulo a 269 u (> 250) */
    float nq[4][3] = {{0, 0, 220}, {330, 0, 220}, {0, 330, 220}, {330, 330, 220}};
    GpuOp nearest = list_quad(nq, 0, TX, CLX);
    ns_fill_reset(f); step(f, NULL, &nearest, 1, &cam, NULL, &cfg, 4, &st);
    CHECK(st.cls[NF_SKIP_NEAR] == 1 && st.filled_tris == 1, "M7: punto mas cercano a 22 cm, centro a 27 cm: se omite el triangulo A,B,D y no el B,D,C (junto=%u relleno=%u)", st.cls[NF_SKIP_NEAR], st.filled_tris);
    cfg.near_skip = 0;
    ns_fill_reset(f); step(f, NULL, &near, 1, &cam, NULL, &cfg, 5, &st);
    CHECK(st.filled_tris == 2, "regla apagada: se rellena (%u)", st.filled_tris);
    ns_fill_destroy(f);
}
/* M2 / M3: cobertura. Lo omitido por cercano sale del denominador (como la pose); lo que recorta el tope de salida sale del numerador. */
static void test_cover_near_cap(void) {
    NsFill *f = ns_fill_create(); NsFillCfg cfg; ns_fill_cfg_default(&cfg); NsFillStats st; NsFillCam cam = cam_id();
    GpuOp ops[24];
    float v[4][3] = {{-150, -150, 100}, {150, -150, 100}, {-150, 150, 100}, {150, 150, 100}};
    for (int i = 0; i < 12; i++) ops[i] = list_of(-3000 + 250 * i, -500, 4000 + 40 * i, 200, 0);
    for (int i = 12; i < 24; i++) ops[i] = list_quad(v, 0, TX, CLX);
    CHECK(step(f, NULL, ops, 24, &cam, NULL, &cfg, 1, &st) == 0, "run");
    CHECK(st.cls[NF_SKIP_NEAR] == 24 && st.cover_pct > 95.0f, "M2: 24 triangulos pegados al ojo no cuentan como escena sin cubrir: cobertura %.1f%% (junto=%u)", st.cover_pct, st.cls[NF_SKIP_NEAR]);
    NsFillStats pr = st;
    /* el glue aplica la misma regla: solo escena pegada al ojo = nada que medir */
    memset(&pr, 0, sizeof pr); pr.tris = 24; pr.cls[NF_SKIP_NEAR] = 24; pr.cover_pct = 100.0f;
    uint32_t fr = 0; int pct = 0;
    ns_fill_glue_reset();
    ns_fill_glue_test_cover(&pr, 10);
    CHECK(ns_fill_glue_last(&fr, &pct) == 0, "M2: solo triangulos pegados al ojo: el glue no mide");
    ns_fill_destroy(f);
    f = ns_fill_create(); ns_fill_cfg_default(&cfg);
    cfg.max_out = 10;
    for (int i = 0; i < 24; i++) ops[i] = list_of(-3000 + 250 * i, -500, 4000 + 40 * i, 200, 0);
    CHECK(step(f, NULL, ops, 24, &cam, NULL, &cfg, 1, &st) == 0, "run tope");
    CHECK(st.cls[NF_SKIP_CAP] > 0 && st.cover_pct < 85.0f && st.cover_pct > 40.0f, "M3: el tope recorto %u rellenos: cobertura %.1f%% (no 100)", st.cls[NF_SKIP_CAP], st.cover_pct);
    ns_fill_destroy(f);
}
/* Tubería completa: relleno -> ns_gpu_fx_build (ya sin seguimiento de lo pegado a la cabeza). Mundo: dibujado y fijo con la cabeza girando. */
static void test_pipeline_follow(void) {
    NsFill *f = ns_fill_create(); NsFillCfg cfg; ns_fill_cfg_default(&cfg); NsFillStats st;
    static const float to_vr[12] = {0.001f, 0, 0, 0, 0, 0.001f, 0, 0, 0, 0, 0.001f, 0};
    NsCamera nc; memset(&nc, 0, sizeof nc); nc.rot[0] = nc.rot[4] = nc.rot[8] = 1; nc.h = 160; nc.overscan_y = 1;
    int drawn_w = 0; float devw = 0, cprev[3] = {0, 0, 0}; int have = 0;
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
        P.to_vr = to_vr; P.cam = &nc; P.n_eyes = 1;
        for (int a = 0; a < 3; a++) { P.head[a] = cam.eye[a] * 0.001f; P.head_right[a] = cam.R[3 * a]; P.head_up[a] = cam.R[3 * a + 1]; P.eyes[0][a] = P.head[a]; }
        NsGpuFxVert vb[64]; NsGpuFxRun runs[4]; uint64_t keys[4]; NsGpuFxOut o;
        CHECK(ns_gpu_fx_build(all, n, &P, vb, 64, runs, 4, keys, 4, &o) == 0, "build");
        uint32_t nverts = o.n_opaque + o.n_semi;
        if (nverts >= 6) {   /* la primera salida dibujada es la de mundo (se emite primero) */
            drawn_w++;
            float c[3] = {0, 0, 0};
            for (int k = 0; k < 6; k++) for (int a = 0; a < 3; a++) c[a] += vb[k].p[a] / 6.0f;
            if (have) { float d = sqrtf((c[0] - cprev[0]) * (c[0] - cprev[0]) + (c[1] - cprev[1]) * (c[1] - cprev[1]) + (c[2] - cprev[2]) * (c[2] - cprev[2])); if (d > devw) devw = d; }
            memcpy(cprev, c, sizeof c); have = 1;
        }
    }
    CHECK(drawn_w >= 38 && devw < 0.003f, "mundo: dibujado siempre y fijo en el mundo VR con la cabeza girando y avanzando (desviacion %.5f m, dibujado %d)", devw, drawn_w);
    ns_fill_destroy(f);
}


/* ---- calibración de la cámara de la lista contra el escenario estático del nativo (desfase RAM/lista) ---- */
/* Rejilla de 18 cuadriláteros estáticos del mundo; la lista los dibuja con la cámara VERDADERA y ns_fill_run recibe la cámara de la RAM, desplazada y girada (un tick adelantada). */
#define CAL_N 18
static void cal_world(int i, float *x0, float *y0, float *zc) { *x0 = -6000.0f + 1500.0f * (float)(i % 6); *y0 = -1500.0f + 1500.0f * (float)(i / 6); *zc = 5000.0f + 700.0f * (float)((i + i / 6) % 4); }
static NsFillCam cam_from(float yaw, float ex, float ey, float ez) { NsFillCam c = cam_yaw(yaw, ex); c.eye[1] = ey; c.eye[2] = ez; return c; }

static void test_calibration(void) {
    const NsFillCam truth = cam_from(10.0f, 300, -200, 150), ram = cam_from(10.7f, 350, -170, 190);   /* 50/30/40 u y 0,7 grados de error */
    Nat N = {0};
    GpuOp ops[CAL_N + 4];
    for (int i = 0; i < CAL_N; i++) { float x0, y0, zc; cal_world(i, &x0, &y0, &zc); nat_quad(&N, x0, y0, zc, 600, PAGE, CLUT, 0xC0000000u + (uint32_t)i); ops[i] = list_world_quad(&truth, x0, y0, zc, 600); }
    /* señuelos: cuadriláteros de la lista SIN pareja en el nativo con la misma textura y UV a ~250 u de uno que sí existe (parejas falsas que la calibración debe descartar) */
    float dx[3] = {250, -300, 280}, dz[3] = {0, 100, -150};
    for (int k = 0; k < 3; k++) { float x0, y0, zc; cal_world(2 + 5 * k, &x0, &y0, &zc); ops[CAL_N + k] = list_world_quad(&truth, x0 + dx[k], y0 + 100, zc + dz[k], 600); }
    /* el hueco que sí hay que rellenar: lejos de todo lo del nativo */
    const float hx = 1200, hy = -2600, hz = 6500, hs = 500;
    ops[CAL_N + 3] = list_world_quad(&truth, hx, hy, hz, hs);
    const float HW[4][3] = {{hx, hy, hz}, {hx + hs, hy, hz}, {hx, hy + hs, hz}, {hx + hs, hy + hs, hz}};

    NsFill *f = ns_fill_create(); NsFillCfg cfg; ns_fill_cfg_default(&cfg); NsFillStats st;
    CHECK(step(f, &N, ops, CAL_N + 4, &ram, NULL, &cfg, 1, &st) == 0, "run con calibración");
    CHECK(st.cal_state == 1, "calibración aplicada (estado %d, muestra %u, válidos %u, antes %.2f px, después %.2f px)", st.cal_state, st.cal_pairs, st.cal_inliers, st.cal_px_before, st.cal_px_after);
    CHECK(st.cal_px_before > 3.0f && st.cal_px_after < 0.5f, "desfase %.2f px -> %.2f px", st.cal_px_before, st.cal_px_after);
    CHECK(st.cal_rot_deg > 0.4f && st.cal_rot_deg < 1.0f && st.cal_dt > 30.0f && st.cal_dt < 300.0f, "corrección ajustada: rot %.2f gr, dt %.0f u", st.cal_rot_deg, st.cal_dt);
    CHECK(st.cls[NF_MATCH] == 2 * CAL_N, "todo el nativo casa tras calibrar: exacto=%u (esperado %d)", st.cls[NF_MATCH], 2 * CAL_N);
    uint32_t n; const NsEffect *e = ns_fill_effects(f, &n);
    float err = 1e9f; int found = 0;
    for (uint32_t i = 0; i < n; i++) {
        float c[3]; centroid(&e[i], c);
        if (fabsf(c[2] - (hz)) < 400 && fabsf(c[0] - (hx + hs * 0.5f)) < 600 && fabsf(c[1] - (hy + hs * 0.5f)) < 600) {
            found = 1; err = 0;
            for (int k = 0; k < e[i].nv && k < 4; k++) for (int a = 0; a < 3; a++) err = fmaxf(err, fabsf(e[i].p[k][a] - HW[k][a]));
        }
    }
    CHECK(found && err < 2.0f, "el hueco se rellena en su sitio verdadero del mundo: error %.2f u (encontrado %d)", err, found);
    /* la cámara equivalente lleva el mundo del nativo a la vista de la lista */
    NsFillCam cc = ns_fill_cam_corrected(&ram, &st);
    double A[9], Ai[9]; ns_fill_cam_maps(&cc, A, Ai);
    float ev = 0;
    for (int i = 0; i < CAL_N; i++) { float x0, y0, zc; cal_world(i, &x0, &y0, &zc); float w[3] = {x0, y0, zc}, v[3], v0[3]; world_to_view(&truth, w, v0);
        for (int r = 0; r < 3; r++) v[r] = (float)(A[3 * r] * (w[0] - cc.eye[0]) + A[3 * r + 1] * (w[1] - cc.eye[1]) + A[3 * r + 2] * (w[2] - cc.eye[2]));
        for (int a = 0; a < 3; a++) ev = fmaxf(ev, fabsf(v[a] - v0[a])); }
    CHECK(ev < 1.5f, "ns_fill_cam_corrected reproduce la vista de la lista (error %.2f u)", ev);
    char buf[2048]; size_t l = ns_fill_format_diag(&st, 7, buf, sizeof buf);
    CHECK(l > 40 && strstr(buf, "[relleno] fr=7 tris_rellenables=") && strstr(buf, "(exacto=") && strstr(buf, " pose=") && strstr(buf, "ausente=") && strstr(buf, "otra_textura=") && strstr(buf, "parcial=") && strstr(buf, "sprite=")
          && strstr(buf, " ms=") && strstr(buf, "desfase_px=") && strstr(buf, "calibracion=aplicada"), "línea de diagnóstico: %s", buf);
    ns_fill_destroy(f);

    /* sin calibrar: el mismo caso falla (la prueba distingue) */
    f = ns_fill_create(); cfg.calib = 0;
    step(f, &N, ops, CAL_N + 4, &ram, NULL, &cfg, 1, &st);
    e = ns_fill_effects(f, &n);
    float err0 = 0;
    for (uint32_t i = 0; i < n; i++) { float c[3]; centroid(&e[i], c); if (fabsf(c[2] - hz) < 400 && fabsf(c[0] - (hx + hs * 0.5f)) < 600) for (int k = 0; k < e[i].nv && k < 4; k++) for (int a = 0; a < 3; a++) err0 = fmaxf(err0, fabsf(e[i].p[k][a] - HW[k][a])); }
    CHECK(st.cal_state == 4 && st.cls[NF_MATCH] < 2 * CAL_N && err0 > 20.0f, "sin calibrar el relleno queda desplazado %.1f u y casan %u/%d (estado %d)", err0, st.cls[NF_MATCH], 2 * CAL_N, st.cal_state);
    ns_fill_destroy(f);

    /* cámara ya exacta: no se toca nada */
    f = ns_fill_create(); ns_fill_cfg_default(&cfg);
    step(f, &N, ops, CAL_N + 4, &truth, NULL, &cfg, 1, &st);
    e = ns_fill_effects(f, &n);
    err = 1e9f;
    for (uint32_t i = 0; i < n; i++) { float c[3]; centroid(&e[i], c); if (fabsf(c[2] - hz) < 400 && fabsf(c[0] - (hx + hs * 0.5f)) < 600) { err = 0; for (int k = 0; k < e[i].nv && k < 4; k++) for (int a = 0; a < 3; a++) err = fmaxf(err, fabsf(e[i].p[k][a] - HW[k][a])); } }
    CHECK(st.cal_state == 2 && st.cal_px_before < 0.5f && err < 1.0f, "cámara exacta: calibración innecesaria (estado %d, desfase %.2f px, error %.2f u)", st.cal_state, st.cal_px_before, err);
    ns_fill_destroy(f);

    /* muestra insuficiente (< 12 triángulos con pareja): no se calibra */
    f = ns_fill_create();
    step(f, &N, ops, 8, &ram, NULL, &cfg, 1, &st);
    CHECK(st.cal_state == 0 && st.cal_pairs < 12, "muestra insuficiente: sin calibrar (estado %d, muestra %u)", st.cal_state, st.cal_pairs);
    ns_fill_destroy(f);

    /* desfase absurdo (la cámara de la RAM es de otra escena): ninguna pareja a < calib_radius, no se inventa una corrección */
    f = ns_fill_create();
    NsFillCam far = cam_from(10.0f, 5300, -200, 150);
    step(f, &N, ops, CAL_N + 4, &far, NULL, &cfg, 1, &st);
    CHECK(st.cal_state != 1, "cámara de otra escena: nunca se aplica (estado %d)", st.cal_state);
    ns_fill_destroy(f);
    free(N.t); free(N.s);
}



/* FRENO y plataforma desplazada. Mismo escenario que test_calibration. */
static int count_outputs_near(const NsFill *f, float hx, float hy, float hz, float hs) {
    uint32_t n; const NsEffect *e = ns_fill_effects(f, &n); int k = 0;
    for (uint32_t i = 0; i < n; i++) { float c[3]; centroid(&e[i], c); if (fabsf(c[2] - hz) < 400 && fabsf(c[0] - (hx + hs * 0.5f)) < 600 && fabsf(c[1] - (hy + hs * 0.5f)) < 600) k++; }
    return k;
}
static void test_brake_and_platform(void) {
    const NsFillCam truth = cam_from(10.0f, 300, -200, 150), ram = cam_from(10.7f, 350, -170, 190);
    const float hx = 1200, hy = -2600, hz = 6500, hs = 500;
    NsFill *f; NsFillCfg cfg; ns_fill_cfg_default(&cfg); NsFillStats st;
    GpuOp ops[CAL_N + 1];
    Nat N = {0};
    for (int i = 0; i < CAL_N; i++) { float x0, y0, zc; cal_world(i, &x0, &y0, &zc); nat_quad(&N, x0, y0, zc, 600, PAGE, CLUT, 0xC0000000u + (uint32_t)i); ops[i] = list_world_quad(&truth, x0, y0, zc, 600); }
    ops[CAL_N] = list_world_quad(&truth, hx, hy, hz, hs);

    /* (a) tipo d0014: nativo presente (>= 64 triángulos) que NO casa con la lista (sin muestra) => cero salidas, solo diagnóstico */
    Nat M = {0};
    for (int i = 0; i < 40; i++) nat_quad(&M, 80000.0f + 1500.0f * (float)(i % 8), 80000.0f + 1500.0f * (float)(i / 8), 4000, 600, PAGE, CLUT, 0xE0000000u + (uint32_t)i);   /* 80 triángulos lejos de la vista */
    f = ns_fill_create();
    step(f, &M, ops, CAL_N + 1, &ram, NULL, &cfg, 1, &st);
    uint32_t n; ns_fill_effects(f, &n);
    CHECK(st.brake == 1 && n == 0 && st.filled_tris == 0 && st.cal_state == 0, "sin muestra con nativo presente: freno=%d salidas=%u relleno=%u estado=%d", st.brake, n, st.filled_tris, st.cal_state);
    CHECK(st.cls[NF_FILL_ABSENT] + st.cls[NF_FILL_SPRITE] + st.cls[NF_FILL_OTHERTEX] > 0 && st.cover_pct < 50.0f, "el diagnóstico sigue contando lo rellenable (%u) y la cobertura no cuenta lo frenado (%.1f%%)", st.cls[NF_FILL_ABSENT] + st.cls[NF_FILL_SPRITE] + st.cls[NF_FILL_OTHERTEX], st.cover_pct);
    char buf[2048]; ns_fill_format_diag(&st, 3, buf, sizeof buf);
    CHECK(strstr(buf, "freno=sin_muestra") != NULL, "la línea dice el motivo: %s", buf);
    ns_fill_destroy(f);
    /* mismo caso con el freno apagado: se rellena (la prueba distingue) */
    f = ns_fill_create(); cfg.brake = 0;
    step(f, &M, ops, CAL_N + 1, &ram, NULL, &cfg, 1, &st);
    ns_fill_effects(f, &n);
    CHECK(st.brake == 0 && n > 0, "sin freno se rellena: salidas=%u", n);
    ns_fill_destroy(f); cfg.brake = 1;
    /* nativo casi vacío (intro: 2 triángulos): no hay nada que duplicar, no se frena (ver test_intro_cover) */
    f = ns_fill_create();
    step(f, NULL, ops, CAL_N + 1, &ram, NULL, &cfg, 1, &st);
    ns_fill_effects(f, &n);
    CHECK(st.brake == 0 && n > 0, "nativo vacío: no se frena (freno=%d salidas=%u)", st.brake, n);
    ns_fill_destroy(f);

    /* (b) cámara buena con muestra: desfase 0 => sin freno y el relleno se mantiene */
    f = ns_fill_create();
    step(f, &N, ops, CAL_N + 1, &truth, NULL, &cfg, 1, &st);
    CHECK(st.brake == 0 && st.cal_state == 2 && count_outputs_near(f, hx, hy, hz, hs) == 1, "cámara buena: freno=%d estado=%d salidas en el hueco=%d", st.brake, st.cal_state, count_outputs_near(f, hx, hy, hz, hs));
    ns_fill_destroy(f);
    /* (b2) escena pequeña (muestra 4..11 < calib_min_tris) con cámara buena: el desfase mide 0 y NO se frena; con muestra < 4 sí; con desfase alto sigue el freno */
    Nat S2 = {0};
    for (int i = 0; i < CAL_N; i++) { float x0, y0, zc; cal_world(i, &x0, &y0, &zc); nat_quad(&S2, x0, y0, zc, 600, PAGE, CLUT, 0xC3000000u + (uint32_t)i); }
    for (int i = 0; i < 40; i++) nat_quad(&S2, 80000.0f + 1500.0f * (float)(i % 8), 80000.0f + 1500.0f * (float)(i / 8), 4000, 600, PAGE, CLUT, 0xE3000000u + (uint32_t)i);   /* nativo presente (>= 64 tris) */
    const uint32_t shortn[] = {10, 9};
    for (int k = 0; k < 2; k++) {
        f = ns_fill_create();
        step(f, &S2, ops, shortn[k], &truth, NULL, &cfg, 1, &st);
        CHECK(st.cal_pairs == shortn[k] && st.cal_state == 0 && st.brake == 0 && st.cal_px_before < 0.5f, "muestra corta %u con cámara buena: sin freno (muestra %u, estado %d, desfase %.2f px, freno %d)", shortn[k], st.cal_pairs, st.cal_state, st.cal_px_before, st.brake);
        ns_fill_destroy(f);
    }
    f = ns_fill_create();
    step(f, &S2, ops, 10, &ram, NULL, &cfg, 1, &st);
    CHECK(st.cal_pairs == 10 && st.cal_px_before > cfg.brake_px && st.brake == 2, "muestra corta con desfase alto: freno por desfase (muestra %u, desfase %.1f px, freno %d)", st.cal_pairs, st.cal_px_before, st.brake);
    ns_fill_destroy(f);
    f = ns_fill_create();
    step(f, &S2, ops, 2, &truth, NULL, &cfg, 1, &st);
    CHECK(st.cal_pairs == 2 && st.brake == 1, "muestra 2: freno sin_muestra (muestra %u, freno %d)", st.cal_pairs, st.brake);
    ns_fill_destroy(f); free(S2.t); free(S2.s);
    /* calibrable: tras corregir queda < 2 px, sin freno */
    f = ns_fill_create();
    step(f, &N, ops, CAL_N + 1, &ram, NULL, &cfg, 1, &st);
    CHECK(st.brake == 0 && st.cal_state == 1 && count_outputs_near(f, hx, hy, hz, hs) == 1, "calibrada: freno=%d estado=%d", st.brake, st.cal_state);
    ns_fill_destroy(f);

    /* (c) desfase alto que no se puede corregir (la muestra se mueve a trozos con offsets distintos): rechazada => freno por desfase */
    Nat P = {0};
    for (int i = 0; i < CAL_N; i++) { float x0, y0, zc; cal_world(i, &x0, &y0, &zc); float o = (i % 3 == 0) ? 0.0f : (i % 3 == 1) ? 160.0f : -170.0f; nat_quad(&P, x0 + o, y0, zc + (i % 2 ? o : -o), 600, PAGE, CLUT, 0xC1000000u + (uint32_t)i); }
    for (int i = 0; i < 40; i++) nat_quad(&P, 80000.0f + 1500.0f * (float)(i % 8), 80000.0f + 1500.0f * (float)(i / 8), 4000, 600, PAGE, CLUT, 0xE1000000u + (uint32_t)i);   /* nativo presente (>= 64 tris) */
    f = ns_fill_create();
    step(f, &P, ops, CAL_N + 1, &truth, NULL, &cfg, 1, &st);
    ns_fill_effects(f, &n);
    CHECK(st.cal_state != 1 && st.brake == 2 && n == 0, "desfase alto sin corrección: freno=%d estado=%d antes=%.1f px salidas=%u", st.brake, st.cal_state, st.cal_px_before, n);
    ns_fill_destroy(f); free(P.t); free(P.s);

    /* (d) plataforma/cabina un tick adelantada: una MINORÍA del escenario estático desplazada, cámara buena => la mediana es ~0, no se toca nada */
    Nat Q = {0};
    for (int i = 0; i < CAL_N; i++) { float x0, y0, zc; cal_world(i, &x0, &y0, &zc); float o = i < 5 ? 90.0f : 0.0f; nat_quad(&Q, x0 + o, y0, zc, 600, PAGE, CLUT, 0xC2000000u + (uint32_t)i); }
    f = ns_fill_create();
    step(f, &Q, ops, CAL_N + 1, &truth, NULL, &cfg, 1, &st);
    CHECK(st.cal_state == 2 && st.cal_px_before < 1.0f && count_outputs_near(f, hx, hy, hz, hs) == 1, "minoría desplazada: la cámara buena no se toca (estado %d, desfase %.2f px)", st.cal_state, st.cal_px_before);
    uint32_t nq; const NsEffect *eq = ns_fill_effects(f, &nq); float errq = 1e9f;
    for (uint32_t i = 0; i < nq; i++) { float c[3]; centroid(&eq[i], c); if (fabsf(c[2] - hz) < 400) { errq = 0; const float HW[4][3] = {{hx, hy, hz}, {hx + hs, hy, hz}, {hx, hy + hs, hz}, {hx + hs, hy + hs, hz}}; for (int k = 0; k < 4; k++) for (int a = 0; a < 3; a++) errq = fmaxf(errq, fabsf(eq[i].p[k][a] - HW[k][a])); } }
    CHECK(errq < 1.0f, "el relleno queda exactamente donde dice la lista (error %.2f u)", errq);
    ns_fill_destroy(f); free(Q.t); free(Q.s);
    free(M.t); free(M.s); free(N.t); free(N.s);
}

/* Cautelas del relleno: un semitransparente PARCIAL (la superficie nativa ya está debajo con esa textura) no se rellena (doble mezcla); el no semitransparente sí, con sesgo de profundidad 2 sobre el nativo */
static void test_partial_semi_and_bias(void) {
    Nat N = {0};
    nat_quad(&N, -500, -500, 4000, 1000, PAGE, CLUT, 0xD0000001u);
    NsFillCam cam = cam_id(); NsFillStats st; NsFillCfg cfg; ns_fill_cfg_default(&cfg);
    NsFill *f = ns_fill_create();
    GpuOp semi = list_of(-500, -500, 4000, 2000, 1);   /* cubre el nativo y se sale de él: el triángulo A,B,D es PARCIAL */
    step(f, &N, &semi, 1, &cam, NULL, &cfg, 1, &st);
    CHECK(st.cls[NF_SKIP_PARTIAL_SEMI] == 1 && st.cls[NF_FILL_PARTIAL] == 0 && st.cls[NF_FILL_ABSENT] + st.cls[NF_FILL_SPRITE] == 1, "semitransparente parcial: no se rellena (parcial_semi=%u parcial=%u ausente+sprite=%u)", st.cls[NF_SKIP_PARTIAL_SEMI], st.cls[NF_FILL_PARTIAL], st.cls[NF_FILL_ABSENT] + st.cls[NF_FILL_SPRITE]);
    ns_fill_reset(f);
    GpuOp opaque = list_of(-500, -500, 4000, 2000, 0);
    step(f, &N, &opaque, 1, &cam, NULL, &cfg, 2, &st);
    uint32_t n; const NsEffect *e = ns_fill_effects(f, &n);
    int bias2 = 0, bias0 = 0;
    for (uint32_t i = 0; i < n; i++) { bias2 += e[i].depth_bias == 2; bias0 += e[i].depth_bias == 0; }
    CHECK(st.cls[NF_FILL_PARTIAL] == 1 && st.cls[NF_SKIP_PARTIAL_SEMI] == 0 && n == 2 && bias2 == 1 && bias0 == 1, "opaco parcial: se rellena con sesgo 2 sobre el nativo (parcial=%u salidas=%u sesgo2=%d sesgo0=%d)", st.cls[NF_FILL_PARTIAL], n, bias2, bias0);
    ns_fill_destroy(f); free(N.t); free(N.s);
}

/* Modo del glue (procesos aparte: el entorno se lee una sola vez). m: 0 = sin MV_FILL (lo gobierna el interruptor de la app; defecto apagado), 1 = MV_FILL=1 (forzado), 2 = MV_FILL=0 (todo apagado).
 * El trabajador puede calcular y registrar siempre, pero ns_fill_glue_fetch solo devuelve algo si dibujar está permitido. */
static int test_glue_mode(int m) {
    NsEffect one, out[4]; memset(&one, 0, sizeof one); one.nv = 3;
    int bad = 0;
    ns_fill_glue_test_set(&one, 1, 100, 105);
    uint32_t a = ns_fill_glue_fetch(out, 4);                          /* sin tocar el interruptor */
    ns_fill_glue_set_mode(1); ns_fill_glue_test_set(&one, 1, 100, 105);
    uint32_t b = ns_fill_glue_fetch(out, 4);                          /* interruptor encendido */
    ns_fill_glue_set_mode(0); ns_fill_glue_test_set(&one, 1, 100, 105);
    uint32_t c = ns_fill_glue_fetch(out, 4);                          /* interruptor apagado */
    const uint32_t ea = m == 1, eb = m == 2 ? 0 : 1, ec = m == 1;     /* esperado */
    if (a != ea || b != eb || c != ec) { printf("FALLO modo %d: sin interruptor=%u (esp. %u) encendido=%u (esp. %u) apagado=%u (esp. %u)\n", m, a, ea, b, eb, c, ec); bad = 1; }
    return bad;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--sin-dibujo")) return test_glue_mode(argc > 2 ? atoi(argv[2]) : 0);

    setenv("MV_FILL", "1", 1);   /* los tests del hilo trabajador comprueban lo que se dibuja: modo dibujo explícito (el defecto es solo diagnóstico) */
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
    test_cover_near_cap();
    test_pipeline_follow();
    test_glue();
    test_glue_stale();
    test_calibration();
    test_partial_semi_and_bias();
    test_brake_and_platform();
    for (int m = 0; m < 3 && argc > 0 && strchr(argv[0], '/'); m++) {   /* sin MV_FILL (interruptor de la app, defecto apagado), MV_FILL=1 (forzado) y MV_FILL=0 (necesita la ruta del propio ejecutable) */
        static const char *const pre[3] = {"env -u MV_FILL", "env MV_FILL=1", "env MV_FILL=0"};
        char cmd[600];
        snprintf(cmd, sizeof cmd, "%s '%s' --sin-dibujo %d", pre[m], argv[0], m);
        int rc = system(cmd);
        CHECK(rc == 0, "modo del glue %s (rc=%d)", pre[m], rc);
    }
    if (fails) { printf("test_ns_fill: %d FALLOS\n", fails); return 1; }
    printf("test_ns_fill: OK\n");
    return 0;
}
