/* Pruebas sintéticas de ns_cover (sin datos del juego): emparejamiento, causas, comprobación inversa, color/posición y formato. */
#include "ns_cover.h"
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
static void nat_push(Nat *N, const float p[3][3], const float uv[3][2], uint16_t page, uint16_t clut, uint32_t key, uint32_t piece, uint32_t face, uint32_t half, uint32_t mflags, int32_t vis, uint8_t rgb) {
    if (N->n == N->cap) {
        N->cap = N->cap ? N->cap * 2 : 16;
        N->t = (NsRTri *)realloc(N->t, N->cap * sizeof *N->t);
        N->s = (NsAdaptSrc *)realloc(N->s, N->cap * sizeof *N->s);
    }
    NsRTri *T = &N->t[N->n]; NsAdaptSrc *S = &N->s[N->n++];
    memset(T, 0, sizeof *T); memset(S, 0, sizeof *S);
    for (int c = 0; c < 3; c++) {
        memcpy(T->v[c].p, p[c], sizeof T->v[c].p);
        T->v[c].u = uv[c][0]; T->v[c].v = uv[c][1];
        T->v[c].r = T->v[c].g = T->v[c].b = rgb;
    }
    T->tpage = page; T->clut = clut; T->flags = NS_TF_TEXTURED;
    S->key = key; S->piece_rel = piece; S->face = face; S->half = half; S->mesh_id = 0xC0DE0000u + piece; S->mflags = mflags; S->vis = vis; S->inst = 0; S->piece = piece;
}

/* cuadrilátero A,B,C,D (perímetro) en el plano z = zc: lado s, esquina (x0,y0). UV del plano. Diagonal B-D (hardware) o A-C. */
static void nat_quad(Nat *N, float x0, float y0, float zc, float s, int diag_ac, uint32_t key, uint32_t piece, uint32_t face, int32_t vis) {
    float A[3] = {x0, y0, zc}, B[3] = {x0 + s, y0, zc}, C[3] = {x0 + s, y0 + s, zc}, D[3] = {x0, y0 + s, zc};
    float ua[2] = {0, 0}, ub[2] = {63, 0}, uc[2] = {63, 63}, ud[2] = {0, 63};
    float P[3][3], U[3][2];
#define SET(i, V, UV) do { memcpy(P[i], V, 12); memcpy(U[i], UV, 8); } while (0)
    if (!diag_ac) {   /* (A,B,D) y (B,D,C): lo que emite ns_adapt */
        SET(0, A, ua); SET(1, B, ub); SET(2, D, ud); nat_push(N, P, U, PAGE, CLUT, key, piece, face, 0, 0, vis, 128);
        SET(0, B, ub); SET(1, D, ud); SET(2, C, uc); nat_push(N, P, U, PAGE, CLUT, key, piece, face, 1, 0, vis, 128);   /* (B,D,C) como ns_adapt */
    } else {          /* (A,B,C) y (A,C,D) */
        SET(0, A, ua); SET(1, B, ub); SET(2, C, uc); nat_push(N, P, U, PAGE, CLUT, key, piece, face, 0, 0, vis, 128);
        SET(0, A, ua); SET(1, C, uc); SET(2, D, ud); nat_push(N, P, U, PAGE, CLUT, key, piece, face, 0, 0, vis, 128);
    }
#undef SET
}

/* operación de la lista: cuadrilátero con las 4 esquinas dadas en orden de hardware (v0..v3 = A,B,D,C) */
static GpuOp list_quad(const float v[4][3], const float uv[4][2], int semi, int textured, uint8_t rgb, int page_tx, int clx) {
    GpuOp o; memset(&o, 0, sizeof o);
    o.type = GPU_OP_POLY; o.nv = 4;
    o.flags = (uint16_t)(GPU_F_GOURAUD | (textured ? GPU_F_TEXTURED : 0) | (semi ? GPU_F_SEMI : 0) | GPU_F_QUAD);
    o.tx = (uint16_t)page_tx; o.ty = TY; o.clx = (uint16_t)clx; o.cly = CLY; o.tp = 0;
    o.proj_h = 160; o.dox = 160; o.doy = 128;
    for (int k = 0; k < 4; k++) {
        GpuVtx *g = &o.v[k];
        g->vvalid = 1; g->vx = v[k][0]; g->vy = v[k][1]; g->vz = v[k][2];
        g->u = (uint8_t)uv[k][0]; g->v = (uint8_t)uv[k][1]; g->r = g->g = g->b = rgb;
        g->fvalid = 1; g->fx = 160 + 160 * v[k][0] / v[k][2]; g->fy = 128 + 160 * v[k][1] / v[k][2];
        g->x = (int16_t)g->fx; g->y = (int16_t)g->fy;
    }
    return o;
}
/* el mismo cuadrilátero que nat_quad, en orden de hardware A,B,D,C */
static GpuOp list_of_quad(float x0, float y0, float zc, float s, int semi, int textured, uint8_t rgb) {
    float v[4][3] = {{x0, y0, zc}, {x0 + s, y0, zc}, {x0, y0 + s, zc}, {x0 + s, y0 + s, zc}};
    float uv[4][2] = {{0, 0}, {63, 0}, {0, 63}, {63, 63}};
    return list_quad(v, uv, semi, textured, rgb, TX, CLX);
}

static NsCoverCam cam_id(void) { NsCoverCam c; memset(&c, 0, sizeof c); c.A[0] = c.A[4] = c.A[8] = 1.0f; return c; }
static void run(const Nat *N, const GpuOp *ops, uint32_t nops, NsCoverResult *r, const NsCoverCfg *cfg) {
    NsCoverCam cam = cam_id();
    int rc = ns_cover_core(N->t, N->s, N->n, &cam, ops, nops, cfg, r);
    CHECK(rc == 0, "rc=%d", rc);
}
static uint32_t cz(const NsCoverResult *r, int c) { return r->cause[c][0] + r->cause[c][1]; }

static void test_match(void) {
    Nat N = {0};
    nat_quad(&N, -500, -500, 4000, 1000, 0, 0xAAAA0001u, 3, 7, 2);
    GpuOp ops[1] = {list_of_quad(-500, -500, 4000, 1000, 0, 1, 128)};
    NsCoverResult r; run(&N, ops, 1, &r, NULL);
    CHECK(r.list_tris == 2 && cz(&r, NC_MATCH) == 2, "tris=%u match=%u", r.list_tris, cz(&r, NC_MATCH));
    CHECK(fabsf(r.cover_pct - 100.0f) < 1e-3f && fabsf(r.exact_pct - 100.0f) < 1e-3f, "cover=%.1f", r.cover_pct);
    CHECK(r.tex_page == 0 && r.tex_clut == 0 && r.tex_uv == 0, "tex %u %u %u", r.tex_page, r.tex_clut, r.tex_uv);
    CHECK(r.col_n == 2 && r.col_bad == 0 && r.shift_bad == 0, "col %u/%u shift %u", r.col_n, r.col_bad, r.shift_bad);
    CHECK(r.nat_used == 2 && r.rev[NR_EXTRA] == 0, "used=%u extra=%u", r.nat_used, r.rev[NR_EXTRA]);
    /* tolerancia: una esquina 10 unidades corrida sigue emparejando; 400 no */
    ops[0].v[1].vx += 10; ops[0].v[1].fx = 160 + 160 * ops[0].v[1].vx / ops[0].v[1].vz;
    run(&N, ops, 1, &r, NULL);
    CHECK(cz(&r, NC_MATCH) == 2, "con 10 de error: match=%u", cz(&r, NC_MATCH));
    ops[0].v[1].vx += 400;
    run(&N, ops, 1, &r, NULL);
    CHECK(cz(&r, NC_MATCH) < 2, "con 410 de error no debe emparejar");
    free(N.t); free(N.s);
}

static void test_split_diag(void) {
    Nat N = {0};
    nat_quad(&N, -500, -500, 4000, 1000, 1, 0xAAAA0002u, 1, 4, 2);   /* diagonal A-C: (A,B,C),(A,C,D) */
    GpuOp ops[1] = {list_of_quad(-500, -500, 4000, 1000, 0, 1, 128)};   /* hardware: (A,B,D),(B,D,C) */
    NsCoverResult r; run(&N, ops, 1, &r, NULL);
    CHECK(cz(&r, NC_SPLIT) == 2 && cz(&r, NC_MATCH) == 0, "split=%u match=%u", cz(&r, NC_SPLIT), cz(&r, NC_MATCH));
    CHECK(r.cover_pct > 99.0f && r.exact_pct < 1.0f, "cover=%.1f exact=%.1f", r.cover_pct, r.exact_pct);
    CHECK(r.nat_used == 2, "used=%u", r.nat_used);
    free(N.t); free(N.s);
}

static void test_subdiv_and_other_tex(void) {
    Nat N = {0};
    nat_quad(&N, -4000, -4000, 6000, 8000, 0, 0xAAAA0003u, 0, 0, 2);   /* cara enorme */
    /* subdivisión del motor: un trozo interior, mismos textura y CLUT, vértices que no son los de la cara */
    GpuOp ops[2] = {list_of_quad(-1000, -1000, 6000, 1000, 0, 1, 128), list_of_quad(1000, 1000, 6000, 1000, 0, 1, 128)};
    NsCoverResult r; run(&N, ops, 2, &r, NULL);
    CHECK(cz(&r, NC_SUBDIV) == 4, "subdiv=%u", cz(&r, NC_SUBDIV));
    CHECK(r.cover_pct > 99.0f, "cover=%.1f", r.cover_pct);
    /* misma posición con otra textura: OTHERTEX, no cubierto */
    GpuOp o2 = list_of_quad(-1000, -1000, 6000, 1000, 1, 1, 128);
    o2.tx = 320; o2.clx = 992;
    run(&N, &o2, 1, &r, NULL);
    CHECK(cz(&r, NC_OTHERTEX) == 2 && r.cause[NC_OTHERTEX][1] == 2, "othertex=%u", cz(&r, NC_OTHERTEX));
    CHECK(r.cover_pct < 1.0f, "cover=%.1f", r.cover_pct);
    CHECK(r.n_worst >= 1 && r.worst[0].cause == NC_OTHERTEX && r.worst[0].n == 2 && r.worst[0].key == 0xAAAA0003u && r.worst[0].semi == 1, "peor: n=%u causa=%d", r.n_worst, r.n_worst ? r.worst[0].cause : -1);
    CHECK(r.worst[0].tpage == ((320 / 64) | (1 << 4)) && r.worst[0].clut == ((992 / 16) | (CLY << 6)), "tpage=%04x clut=%04x", r.worst[0].tpage, r.worst[0].clut);
    free(N.t); free(N.s);
}

static void test_billboard_pose_absent(void) {
    Nat N = {0};
    nat_quad(&N, -500, -500, 4000, 1000, 0, 0xAAAA0004u, 9, 1, 2);   /* la cara con uv 0..63 */
    /* sprite: rectángulo a z constante lejos de todo */
    GpuOp spr = list_of_quad(8000, 8000, 3000, 300, 1, 0, 8);
    /* pose distinta: mismas textura y UV que la cara nativa, pero 3000 unidades más allá */
    float pv[4][3] = {{2500, -500, 4000}, {3500, -500, 4200}, {2500, 500, 4000}, {3500, 500, 4200}};
    float puv[4][2] = {{0, 0}, {63, 0}, {0, 63}, {63, 63}};
    GpuOp pose = list_quad(pv, puv, 0, 1, 128, TX, CLX);
    /* ausente: textura que el nativo no tiene y esquinas a distinta profundidad lejos de todo */
    float v[4][3] = {{-9000, -9000, 2000}, {-8000, -9000, 2500}, {-9000, -8000, 3000}, {-8000, -8000, 3500}};
    float uv[4][2] = {{10, 10}, {20, 10}, {10, 20}, {20, 20}};
    GpuOp abs_ = list_quad(v, uv, 0, 1, 128, 128, 800);
    GpuOp ops[3] = {spr, pose, abs_};
    NsCoverResult r; run(&N, ops, 3, &r, NULL);
    CHECK(cz(&r, NC_BILLBOARD) == 2, "sprite=%u", cz(&r, NC_BILLBOARD));
    CHECK(cz(&r, NC_POSE) == 2, "pose=%u", cz(&r, NC_POSE));
    CHECK(cz(&r, NC_ABSENT) == 2, "ausente=%u", cz(&r, NC_ABSENT));
    CHECK(r.nat_used == 2, "la pareja de la pose cuenta como usada: %u", r.nat_used);
    int found = 0;
    for (uint32_t i = 0; i < r.n_worst; i++) if (r.worst[i].cause == NC_POSE) { found = 1; CHECK(r.worst[i].key == 0xAAAA0004u && r.worst[i].piece == 9, "dueño de la pose %08x/%u", r.worst[i].key, r.worst[i].piece); }
    CHECK(found, "grupo POSE en peores");
    for (uint32_t i = 0; i < r.n_worst; i++) if (r.worst[i].cause == NC_ABSENT) CHECK(r.worst[i].key == 0xAAAA0004u, "ausente: dueño más cercano %08x", r.worst[i].key);
    CHECK(r.cover_pct < 1.0f, "cover=%.1f", r.cover_pct);
    free(N.t); free(N.s);
}

static void test_reverse(void) {
    Nat N = {0};
    nat_quad(&N, -500, -500, 4000, 1000, 0, 0xB0000001u, 0, 0, 2);              /* emparejada con la lista */
    nat_quad(&N, -500, -500, -3000, 1000, 0, 0xB0000002u, 0, 0, 2);             /* detrás */
    nat_quad(&N, 90000, -500, 4000, 1000, 0, 0xB0000003u, 0, 0, 2);             /* fuera de pantalla */
    nat_quad(&N, 2000, -500, 4000, 1000, 0, 0xB0000007u, 0, 0, 0);              /* objeto que el motor no vio (vis 0) pero en pantalla */
    nat_quad(&N, -500, 1500, 4000, 1000, 0, 0xB0000008u, 0, 0, 2);              /* visible de más: sin pareja, en pantalla, orientada al frente */
    /* reverso: misma cara con las esquinas en el otro sentido (A,D,B,...) -> orientación contraria */
    {
        float A[3] = {-500, 2500, 4000}, B[3] = {500, 2500, 4000}, D[3] = {-500, 3500, 4000};
        float P[3][3], U[3][2] = {{0, 0}, {63, 0}, {0, 63}};
        memcpy(P[0], A, 12); memcpy(P[1], D, 12); memcpy(P[2], B, 12);   /* (A,D,B): sentido contrario a (A,B,D) */
        nat_push(&N, P, U, PAGE, CLUT, 0xB0000009u, 0, 0, 0, 0, 2, 128);
    }
    GpuOp ops[1] = {list_of_quad(-500, -500, 4000, 1000, 0, 1, 128)};
    NsCoverResult r; run(&N, ops, 1, &r, NULL);
    CHECK(r.rev[NR_BEHIND] == 2, "detras=%u", r.rev[NR_BEHIND]);
    CHECK(r.rev[NR_OFF] == 2, "fuera=%u", r.rev[NR_OFF]);
    CHECK(r.rev[NR_HIDDEN] == 2, "objeto_no_visible=%u", r.rev[NR_HIDDEN]);
    CHECK(r.rev[NR_EXTRA] == 2, "de_mas=%u", r.rev[NR_EXTRA]);
    CHECK(r.rev[NR_BACK] == 1, "reverso=%u", r.rev[NR_BACK]);
    CHECK(r.n_extra == 1 && r.extra[0].key == 0xB0000008u && r.extra[0].n == 2, "grupo de más: n=%u key=%08x", r.n_extra, r.n_extra ? r.extra[0].key : 0);
    /* doble cara: el reverso ya no se descarta */
    N.t[N.n - 1].v[0].p[0] += 0;   /* (no-op) */
    N.s[N.n - 1].mflags = NS_MF_TWOSIDED;
    run(&N, ops, 1, &r, NULL);
    CHECK(r.rev[NR_BACK] == 0 && r.rev[NR_EXTRA] == 3, "doble cara: reverso=%u de_mas=%u", r.rev[NR_BACK], r.rev[NR_EXTRA]);
    free(N.t); free(N.s);
}

static void test_color_shift_degenerate(void) {
    Nat N = {0};
    nat_quad(&N, -100, -100, 800, 200, 0, 0xC0000001u, 2, 5, 2);
    /* color de la lista distinto (200 frente a 128) y una esquina desplazada 20 unidades (dentro de la tolerancia; a 800 de profundidad son 4 px) */
    GpuOp ops[3];
    ops[0] = list_of_quad(-100, -100, 800, 200, 0, 1, 200);
    ops[0].v[3].vx += 20;
    /* degenerada: dos esquinas iguales */
    ops[1] = list_of_quad(0, 0, 4000, 1000, 0, 1, 128);
    ops[1].v[1] = ops[1].v[0]; ops[1].v[2] = ops[1].v[0]; ops[1].v[3] = ops[1].v[0];
    /* sin posición de vista: no se compara */
    ops[2] = list_of_quad(0, 0, 4000, 1000, 0, 1, 128);
    ops[2].v[2].vvalid = 0;
    NsCoverResult r; run(&N, ops, 3, &r, NULL);
    CHECK(r.list_degenerate == 2 && r.list_no_view == 1 && r.list_polys3d == 2, "degen=%u sinvista=%u polys=%u", r.list_degenerate, r.list_no_view, r.list_polys3d);
    CHECK(r.col_bad >= 1 && r.col_mean > 24.0f, "color: malos=%u medio=%.1f", r.col_bad, r.col_mean);
    CHECK(r.shift_bad >= 1, "posicion: desplazados=%u", r.shift_bad);
    CHECK(r.n_color >= 1 && r.color[0].key == 0xC0000001u && r.n_shift >= 1 && r.shift[0].key == 0xC0000001u, "grupos color/pos");
    free(N.t); free(N.s);
}

static void test_camera_and_format(void) {
    /* cámara real: mundo desplazado (ojo en (1000,0,0)); la lista está en vista */
    Nat N = {0};
    nat_quad(&N, 500, -500, 4000, 1000, 0, 0xD0000001u, 0, 0, 2);
    NsCoverCam cam = cam_id(); cam.t[0] = -1000.0f;   /* e = p - (1000,0,0) */
    GpuOp ops[1] = {list_of_quad(-500, -500, 4000, 1000, 0, 1, 128)};
    NsCoverResult r;
    CHECK(ns_cover_core(N.t, N.s, N.n, &cam, ops, 1, NULL, &r) == 0 && cz(&r, NC_MATCH) == 2, "cámara trasladada: match=%u", cz(&r, NC_MATCH));
    char buf[4096];
    size_t n = ns_cover_format(&r, 77, buf, sizeof buf);
    CHECK(n > 0 && strstr(buf, "[cobertura] fr=77") && strstr(buf, "cubierto=100.0%"), "formato: %s", buf);
    CHECK(strstr(buf, "SIN_CUBRIR") && strstr(buf, "nativo:"), "formato: faltan campos");
    /* búfer minúsculo: nunca escribe fuera y deja el terminador */
    char tiny[16]; memset(tiny, 'x', sizeof tiny);
    n = ns_cover_format(&r, 1, tiny, sizeof tiny - 1);
    CHECK(n < sizeof tiny && tiny[n] == 0 && tiny[15] == 'x', "búfer pequeño n=%zu", n);
    /* sin nativo y sin lista */
    CHECK(ns_cover_core(NULL, NULL, 0, &cam, NULL, 0, NULL, &r) == 0 && r.cover_pct == 100.0f, "vacío");
    CHECK(ns_cover_core(NULL, NULL, 0, NULL, NULL, 0, NULL, &r) != 0, "sin cámara = error");
    free(N.t); free(N.s);
}

static void test_many(void) {
    /* escala: 4000 caras nativas y 600 de la lista (todas con pareja); debe ser rápido y exacto */
    Nat N = {0};
    GpuOp *ops = (GpuOp *)calloc(600, sizeof(GpuOp));
    for (int i = 0; i < 4000; i++) nat_quad(&N, (float)((i % 80) * 700 - 28000), (float)((i / 80) * 700 - 17000), 8000 + (float)(i % 7) * 50, 600, 0, 0xE0000000u + (uint32_t)(i / 100), (uint32_t)(i % 100), (uint32_t)i, 2);
    for (int i = 0; i < 600; i++) { int k = i * 6; ops[i] = list_of_quad((float)((k % 80) * 700 - 28000), (float)((k / 80) * 700 - 17000), 8000 + (float)(k % 7) * 50, 600, 0, 1, 128); }
    NsCoverResult r; run(&N, ops, 600, &r, NULL);
    CHECK(r.list_tris == 1200 && cz(&r, NC_MATCH) == 1200, "match=%u de %u", cz(&r, NC_MATCH), r.list_tris);
    CHECK(r.ms < 2000.0, "ms=%.1f", r.ms);
    free(ops); free(N.t); free(N.s);
}

/* ns_cover_run con una RAM sin escena (menú, vídeo): error limpio, sin caer; y el enlace/hilo de la app (ns_cover_glue) con la misma RAM: nunca bloquea ni imprime. */
void ns_cover_glue_post_ik(const uint8_t *ram);
void ns_cover_glue_submit(const uint8_t *ram, const uint8_t *scratch, const GpuOp *ops, uint32_t nops, uint32_t frame);
void ns_cover_glue_submit_cam(const uint8_t *ram, const uint8_t *scratch, const GpuOp *ops, uint32_t nops, uint32_t frame, const float *eye, const float *M);
size_t ns_cover_glue_poll(char *buf, size_t cap);
static void test_run_and_glue(void) {
    uint8_t *ram = (uint8_t *)calloc(1, 0x200000);
    GpuOp ops[1] = {list_of_quad(-500, -500, 4000, 1000, 0, 1, 128)};
    NsCover *c = ns_cover_create();
    NsCoverResult r;
    CHECK(c != NULL, "create");
    ns_cover_run(c, ram, NULL, ops, 1, NULL, &r);   /* RAM vacía: no cae; el resultado queda sin nativos */
    CHECK(r.nat_tris == 0, "RAM sin escena = cero triángulos nativos");
    CHECK(ns_cover_run(NULL, ram, NULL, ops, 1, NULL, &r) != 0 && ns_cover_run(c, NULL, NULL, ops, 1, NULL, &r) != 0, "argumentos nulos");
    ns_cover_destroy(c); ns_cover_destroy(NULL);
    setenv("MV_COVER_N", "1", 1);
    char buf[256];
    for (int i = 0; i < 40; i++) { ns_cover_glue_post_ik(ram); ns_cover_glue_submit(ram, NULL, ops, 1, (uint32_t)i); }
    struct timespec ts = {0, 50 * 1000 * 1000};
    nanosleep(&ts, NULL);
    ns_cover_glue_poll(buf, sizeof buf);   /* puede haber texto o no según el hilo; solo debe ser seguro */
    CHECK(ns_cover_glue_poll(buf, sizeof buf) == 0, "el texto se consume una vez");
    { float eye[3] = {0, 0, 0}, M[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
      for (int i = 0; i < 40; i++) { ns_cover_glue_post_ik(ram); ns_cover_glue_submit_cam(ram, NULL, ops, 1, (uint32_t)i, eye, M); }
      nanosleep(&ts, NULL); ns_cover_glue_poll(buf, sizeof buf); }   /* con la cámara de la lista: seguro */
    {   /* ns_cover_run con la cámara de la lista: no cae con RAM sin escena */
      NsCover *c2 = ns_cover_create(); NsCoverCfg cfg; float eye[3] = {0, 0, 0}, M[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
      ns_cover_cfg_default(&cfg); cfg.list_eye = eye; cfg.list_M = M;
      NsCoverResult r2; ns_cover_run(c2, ram, NULL, ops, 1, &cfg, &r2); CHECK(r2.nat_tris == 0, "cámara de la lista, RAM sin escena");
      ns_cover_destroy(c2); }
    ns_cover_glue_submit(ram, NULL, ops, 0, 1);   /* lista vacía: ignorada */
    ns_cover_glue_submit(NULL, NULL, ops, 1, 1);
    free(ram);
}

/* Factor vertical de la vista de la lista (cámara del propio juego: matrices de pantalla con la fila Y x 66/64): la RAM sintética lleva un objeto con un cuadrilátero en |y| grande; ns_cover_run lo mide de las
 * matrices de pantalla de la pieza (+0x20) y la lista (con vy = ys * y nativo) debe quedar cubierta; con ys = 1 forzado no. */
static void ram_w16(uint8_t *ram, uint32_t a, uint32_t v) { ram[a & 0x1FFFFF] = (uint8_t)v; ram[(a & 0x1FFFFF) + 1] = (uint8_t)(v >> 8); }
static void ram_w32(uint8_t *ram, uint32_t a, uint32_t v) { ram_w16(ram, a, v & 0xFFFF); ram_w16(ram, a + 2, v >> 16); }
static void ram_mat(uint8_t *ram, uint32_t a, float ys) { for (int i = 0; i < 9; i++) ram_w16(ram, a + 2u * (uint32_t)i, (i % 4 == 0) ? (i == 4 ? (uint32_t)(4096.0f * ys) : 4096u) : 0u); }
static void test_screen_ys(void) {
    for (int mode = 0; mode < 2; mode++) {   /* 0: cámara del juego (66/64); 1: capa VR (sin factor) */
        const float ys = mode ? 1.0f : 66.0f / 64.0f;
        uint8_t *ram = (uint8_t *)calloc(1, 0x200000);
        const uint32_t CH = 0x800B2B7Cu, Q = 0x800B2110u, O = 0x80130000u, DEF = 0x80131000u, M = DEF + 0x20, V = 0x80132000u, VI = 0x80132100u, NI = 0x80132200u, UV = 0x80132300u, TXI = 0x80132400u, NRM = 0x80132500u;
        uint32_t te = 0x800B30E0u + 12u * (0x1234u % 512u);
        ram_w16(ram, te, 0x1234); ram_w16(ram, te + 4, 0x0105); ram_w16(ram, te + 6, 0x7800);
        ram[(te + 8) & 0x1FFFFF] = 16; ram[(te + 9) & 0x1FFFFF] = 32; ram[(te + 10) & 0x1FFFFF] = 31; ram[(te + 11) & 0x1FFFFF] = 15;
        ram_mat(ram, CH + 0x10, 1.0f); ram_mat(ram, CH + 0x30, 1.0f); ram_w16(ram, CH + 0x50, 160);
        ram_w16(ram, CH + 0x56, 1); ram_w32(ram, CH + 0x58, Q); ram_w32(ram, Q, O);
        ram_w32(ram, 0x800ACAF8u, 1);
        ram_mat(ram, O, 1.0f); ram_w32(ram, O + 0x24, DEF); ram_w32(ram, O + 0x28, 0x1); ram_w16(ram, O + 0x2C, 1); ram_w16(ram, O + 0x2E, 1);
        ram_w32(ram, DEF, 1); ram_w32(ram, DEF + 4, 1);
        uint32_t P0 = O + 0x48;
        ram_mat(ram, P0, 1.0f); ram_mat(ram, P0 + 0x20, ys); ram_w32(ram, P0 + 0x40, M);
        const int16_t vx[4][3] = {{-500, 2800, 4000}, {500, 2800, 4000}, {500, 3800, 4000}, {-500, 3800, 4000}};   /* A,B,C,D */
        ram_w32(ram, M, 0x2); ram_w32(ram, M + 4, 1); ram_w32(ram, M + 0x2C, 0xFFFFFFFFu); ram_w32(ram, M + 0x30, 0xFFFFFFFFu); ram_w32(ram, M + 0x34, 4);
        ram_w32(ram, M + 0x38, V); ram_w32(ram, M + 0x3C, VI); ram_w32(ram, M + 0x40, 1); ram_w32(ram, M + 0x44, NRM); ram_w32(ram, M + 0x48, NI); ram_w32(ram, M + 0x4C, UV); ram_w32(ram, M + 0x50, TXI);
        for (int i = 0; i < 4; i++) for (int k = 0; k < 3; k++) ram_w16(ram, V + 8u * (uint32_t)i + 2u * (uint32_t)k, (uint32_t)(uint16_t)vx[i][k]);
        for (int k = 0; k < 4; k++) ram[(VI + (uint32_t)k) & 0x1FFFFF] = (uint8_t)k;
        const uint8_t uvb[8] = {0, 0, 255, 0, 255, 255, 0, 255};
        for (int k = 0; k < 8; k++) ram[(UV + (uint32_t)k) & 0x1FFFFF] = uvb[k];
        ram_w16(ram, TXI, 0x1234);
        /* lista: el mismo cuadrilátero en orden de hardware A,B,D,C con vy = ys * y */
        float v[4][3], uv[4][2] = {{16, 32}, {47, 32}, {16, 47}, {47, 47}};
        const int hw[4] = {0, 1, 3, 2};
        for (int k = 0; k < 4; k++) { v[k][0] = vx[hw[k]][0]; v[k][1] = ys * vx[hw[k]][1]; v[k][2] = vx[hw[k]][2]; }
        GpuOp op = list_quad(v, uv, 0, 1, 128, TX, CLX);
        NsCover *c = ns_cover_create();
        NsCoverResult r; NsCoverCfg cfg;
        ns_cover_cfg_default(&cfg);
        CHECK(ns_cover_run(c, ram, NULL, &op, 1, &cfg, &r) == 0, "run");
        CHECK(r.list_tris == 2 && cz(&r, NC_MATCH) == 2 && fabsf(r.ys - ys) < 1e-3f, "auto ys=%.4f modo %d: match=%u de %u", r.ys, mode, cz(&r, NC_MATCH), r.list_tris);
        if (mode == 0) {
            cfg.ys = 1.0f;
            CHECK(ns_cover_run(c, ram, NULL, &op, 1, &cfg, &r) == 0 && r.ys == 1.0f && cz(&r, NC_MATCH) == 0, "ys=1 forzado con lista a 66/64: match=%u", cz(&r, NC_MATCH));
        }
        ns_cover_destroy(c); free(ram);
    }
}

int main(void) {
    test_match();
    test_split_diag();
    test_subdiv_and_other_tex();
    test_billboard_pose_absent();
    test_reverse();
    test_color_shift_degenerate();
    test_camera_and_format();
    test_many();
    test_run_and_glue();
    test_screen_ys();
    if (fails) { printf("%d FALLOS\n", fails); return 1; }
    printf("OK: todas las pruebas de ns_cover pasan\n");
    return 0;
}
