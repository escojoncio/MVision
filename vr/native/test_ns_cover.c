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


/* ---- huecos del mundo: techo sin modelar (volcado d0003 de a31: arriba 50 %, abajo 0 %) ---- */
static void add_tri(NsRTri *T, size_t *n, const float a[3], const float b[3], const float c[3]) {
    memset(&T[*n], 0, sizeof T[*n]);
    memcpy(T[*n].v[0].p, a, 12); memcpy(T[*n].v[1].p, b, 12); memcpy(T[*n].v[2].p, c, 12);
    (*n)++;
}
static void add_quad(NsRTri *T, size_t *n, const float a[3], const float b[3], const float c[3], const float d[3]) { add_tri(T, n, a, b, c); add_tri(T, n, a, c, d); }
static void test_holes(void) {
    const float eye[3] = {0, 1600, 0};
    NsRTri T[16]; size_t n = 0; NsCoverHoles H;
    const float S = 20000.0f;
    /* sala cerrada: seis caras de un cubo de 20 m alrededor del ojo */
    float v[8][3]; for (int i = 0; i < 8; i++) { v[i][0] = (i & 1 ? S : -S); v[i][1] = (i & 2 ? S : -S); v[i][2] = (i & 4 ? S : -S); }
    const int F[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};   /* z-, z+, y-, y+, x-, x+ */
    for (int f = 0; f < 6; f++) add_quad(T, &n, v[F[f][0]], v[F[f][1]], v[F[f][2]], v[F[f][3]]);
    ns_cover_holes(T, n, 0, eye, &H);
    CHECK(H.rays == NC_HOLE_AZ * NC_HOLE_EL && H.miss == 0, "sala cerrada sin huecos: miss=%u de %u", H.miss, H.rays);
    /* sin la cara superior (y+): el techo que el juego no modela */
    n = 0; for (int f = 0; f < 6; f++) if (f != 3) add_quad(T, &n, v[F[f][0]], v[F[f][1]], v[F[f][2]], v[F[f][3]]);
    ns_cover_holes(T, n, 0, eye, &H);
    CHECK(H.miss > 0 && H.miss_dn == 0, "sin techo: nada por debajo del horizonte (abajo=%u)", H.miss_dn);
    CHECK(H.band[0] == (uint32_t)(H.rays / 12), "zenit 100 %% sin geometría: %u de %u", H.band[0], H.rays / 12);
    CHECK(H.band[11] == 0 && H.band[6] == 0, "nadir y franja bajo el horizonte cubiertos");
    CHECK(H.miss_up > 0 && H.miss_up < H.rays / 2, "techo parcial: la mitad superior no está vacía del todo (%u de %u)", H.miss_up, H.rays / 2);
    {   /* el cielo (los últimos triángulos) NO es geometría: no tapa el techo que falta; se cuenta aparte */
        const uint32_t miss0 = H.miss; const size_t n0 = n;
        const float a[3] = {-1e6f, 150000, -1e6f}, b[3] = {1e6f, 150000, -1e6f}, c[3] = {1e6f, 150000, 1e6f}, d[3] = {-1e6f, 150000, 1e6f};
        NsRTri T2[16]; memcpy(T2, T, sizeof T); add_quad(T2, &n, a, b, c, d);
        ns_cover_holes(T2, n, 2, eye, &H);
        CHECK(H.miss == miss0 && H.sky == miss0 && miss0 > 0, "el cielo no tapa huecos: sin_geometria=%u (antes %u) tapados_por_cielo=%u", H.miss, miss0, H.sky);
        NsCoverResult r; memset(&r, 0, sizeof r); r.holes = H; char buf[4096];
        ns_cover_format(&r, 5, buf, sizeof buf);
        CHECK(strstr(buf, "solo_cielo="), "formato con cielo: %s", buf);
        n = n0;
    }
    /* solo suelo infinito: toda la mitad superior es hueco, la inferior no (el rayo horizontal no toca) */
    n = 0; { const float a[3] = {-1e6f, 0, -1e6f}, b[3] = {1e6f, 0, -1e6f}, c[3] = {1e6f, 0, 1e6f}, d[3] = {-1e6f, 0, 1e6f}; add_quad(T, &n, a, b, c, d); }
    ns_cover_holes(T, n, 0, eye, &H);
    CHECK(H.miss_up == H.rays / 2 && H.miss_dn == 0, "solo suelo: arriba=%u abajo=%u", H.miss_up, H.miss_dn);
    /* geometría a menos de 1 unidad del ojo no cuenta; sin triángulos no se mide */
    ns_cover_holes(NULL, 0, 0, eye, &H);
    CHECK(H.rays == 0, "sin triángulos: sin medida");
    /* texto de la línea [huecos] */
    {
        NsCoverResult r; memset(&r, 0, sizeof r);
        ns_cover_holes(T, n, 0, eye, &r.holes);
        char buf[4096];
        size_t len = ns_cover_format(&r, 77, buf, sizeof buf);
        CHECK(len > 0 && strstr(buf, "[huecos] fr=77") && strstr(buf, "sin_geometria=50.0%") && strstr(buf, "arriba 100.0%, abajo 0.0%"), "formato: %s", buf);
        memset(&r.holes, 0, sizeof r.holes);
        len = ns_cover_format(&r, 77, buf, sizeof buf);
        CHECK(!strstr(buf, "[huecos]"), "sin medida no hay línea");
    }
}

/* «pose» nunca con dueño estático: la misma textura y UV en una pared del ESCENARIO a 3000 unidades no es otra pose, es geometría que falta ahí (antes contaba como pose y no como ausente). */
static void test_pose_static_owner(void) {
    Nat N = {0};
    nat_quad(&N, -500, -500, 4000, 1000, 0, 0xAAAA0004u, 9, 1, 2);
    for (size_t i = 0; i < N.n; i++) N.s[i].iflags = NS_IF_STATIC;
    float pv[4][3] = {{2500, -500, 4000}, {3500, -500, 4200}, {2500, 500, 4000}, {3500, 500, 4200}};
    float puv[4][2] = {{0, 0}, {63, 0}, {0, 63}, {63, 63}};
    GpuOp pose = list_quad(pv, puv, 0, 1, 128, TX, CLX);
    NsCoverResult r; run(&N, &pose, 1, &r, NULL);
    CHECK(cz(&r, NC_POSE) == 0 && cz(&r, NC_ABSENT) == 2, "dueño estático: pose=%u ausente=%u", cz(&r, NC_POSE), cz(&r, NC_ABSENT));
    free(N.t); free(N.s);
}

/* ---- verificador geométrico ---- */
static GpuOp vop(GpuOp o, uint32_t pkt) { o.pkt_addr = 0x80000000u | pkt; return o; }
static NsVerifOwner vown(uint32_t lo, uint32_t n, uint32_t key, uint32_t piece, int cls) { NsVerifOwner w; memset(&w, 0, sizeof w); w.lo = lo; w.hi = lo + n; w.key = key; w.piece = (uint16_t)piece; w.psize = 40; w.cls = (uint8_t)cls; return w; }
static void test_verif(void) {
    NsVerifCam id; memset(&id, 0, sizeof id); id.A[0] = id.A[4] = id.A[8] = 1.0f;
    NsVerif v;
    /* 1. escenario presente / ausente: sin categoría pose, por distancia geométrica y no por textura */
    {
        Nat N = {0};
        nat_quad(&N, -500, -500, 4000, 1000, 0, 0x80100000u, 0, 1, 2);
        NsVerifOwner own[2] = {vown(0x100000, 400, 0x80100000u, 0, NV_STATIC), vown(0x101000, 400, 0x80100000u, 0, NV_STATIC)};
        GpuOp ok = vop(list_of_quad(-500, -500, 4000, 1000, 0, 1, 128), 0x100000);
        float pv[4][3] = {{2500, -500, 4000}, {3500, -500, 4200}, {2500, 500, 4000}, {3500, 500, 4200}};
        float puv[4][2] = {{0, 0}, {63, 0}, {0, 63}, {63, 63}};
        GpuOp far = vop(list_quad(pv, puv, 0, 1, 128, TX, CLX), 0x101000);   /* misma textura y UV, 3000 unidades al lado */
        GpuOp ops[2] = {ok, far};
        CHECK(ns_verif_core(N.t, N.s, N.n, NULL, 0, own, 2, &id, 1, 1.0f, ops, 1, 0, &v) == 0 && v.valid && v.tris_gte == 2 && v.missing == 0, "presente: tris=%u faltan=%u", v.tris_gte, v.missing);
        CHECK(ns_verif_core(N.t, N.s, N.n, NULL, 0, own, 2, &id, 1, 1.0f, ops, 2, 0, &v) == 0 && v.tris_gte == 4 && v.missing == 2 && v.cls[NV_STATIC] == 2 && v.moved == 0, "ausente estático: faltan=%u estatico=%u otra_pose=%u", v.missing, v.cls[NV_STATIC], v.moved);
        CHECK(v.worst_key == 0x80100000u && v.worst_n == 2 && v.worst_cls == NV_STATIC, "peor dueño %08x n=%u", v.worst_key, v.worst_n);
        /* sin dueño en las colas y sin muestras de escenario que validen la cámara: no se puede verificar */
        far.pkt_addr = 0x80300000u;
        CHECK(ns_verif_core(N.t, N.s, N.n, NULL, 0, own, 2, &id, 1, 1.0f, &far, 1, 0, &v) == 0 && v.missing == 0 && v.unverified == 2 && v.tris_gte == 0, "sin dueño: sin_verificar=%u", v.unverified);
        /* vértice de vista ambiguo: no se mide */
        far.v[1].vflags = GPU_VF_AMBIG;
        CHECK(ns_verif_core(N.t, N.s, N.n, NULL, 0, own, 2, &id, 1, 1.0f, &far, 1, 0, &v) == 0 && v.missing == 0 && v.ambig == 2 && v.tris_gte == 0, "ambiguo: %u", v.ambig);
        char buf[600];
        size_t w = ns_verif_format(&v, 7, buf, sizeof buf);
        CHECK(w > 0 && strstr(buf, "[verif] fr=7 tris_gte=") && strstr(buf, "sin_equivalente=") && strstr(buf, "mundo_en_2d=") && strstr(buf, "sortonly=") && strstr(buf, "fuera_grupo="), "formato: %s", buf);
        free(N.t); free(N.s);
    }
    /* 2. la lista se emitió con OTRA cámara (corte entre fotogramas): se recupera con los vértices de dueño estático y no falta nada */
    {
        Nat N = {0};
        uint32_t key = 0x80100000u; NsVerifOwner own[16]; GpuOp ops[16]; int no = 0;
        const double yaw = 0.7, pitch = 0.25, c = cos(yaw), sn = sin(yaw), cp = cos(pitch), sp = sin(pitch);
        const double R[9] = {c, 0, -sn, -sn * sp, cp, -c * sp, sn * cp, sp, c * cp}, T[3] = {300, -200, 2500};   /* mundo -> vista de la lista */
        for (int k = 0; k < 12; k++) {   /* doce caras del escenario en tres piezas, a distintas profundidades y alturas (no coplanares) */
            const float x0 = -3000.0f + 700.0f * (float)(k % 4), y0 = -900.0f + 650.0f * (float)(k / 4), zc = 3000.0f + 450.0f * (float)k, sz = 500.0f + 30.0f * (float)k;
            nat_quad(&N, x0, y0, zc, sz, 0, key, (uint32_t)(k / 4), (uint32_t)k, 2);
            float wv[4][3] = {{x0, y0, zc}, {x0 + sz, y0, zc}, {x0, y0 + sz, zc}, {x0 + sz, y0 + sz, zc}}, lv[4][3];
            for (int q = 0; q < 4; q++) for (int r = 0; r < 3; r++) lv[q][r] = (float)(R[3 * r] * wv[q][0] + R[3 * r + 1] * wv[q][1] + R[3 * r + 2] * wv[q][2] + T[r]);
            float uv[4][2] = {{0, 0}, {63, 0}, {0, 63}, {63, 63}};
            own[no] = vown(0x100000u + 0x100u * (uint32_t)k, 0x28, key, (uint32_t)(k / 4), NV_STATIC);
            ops[no] = vop(list_quad(lv, uv, 0, 1, 128, TX, CLX), own[no].lo); no++;
        }
        for (size_t i = 0; i < N.n; i++) N.s[i].iflags = NS_IF_STATIC;
        CHECK(ns_verif_core(N.t, N.s, N.n, NULL, 0, own, (size_t)no, &id, 1, 1.0f, ops, (uint32_t)no, 0, &v) == 0 && v.tris_gte == 24 && v.cam == -1 && v.cam_in > 0.99f && v.cam_distinct && v.missing == 0 && v.unverified == 24 && v.only_fit == 24,
              "cámara ajustada lejos de la de la app: se recupera, pero se marca DISTINTA y nada cuenta como equivalente (ni como ausente: sin verificar): tris=%u faltan=%u cam=%d explica=%.2f", v.tris_gte, v.missing, v.cam, v.cam_in);
        /* una cara sin dueño CON la cámara validada por el escenario (candidata buena): «otros» */
        {
            float wv[4][3] = {{5000, 0, 9000}, {5600, 0, 9000}, {5000, 600, 9000}, {5600, 600, 9000}}, lv[4][3], uv[4][2] = {{0, 0}, {63, 0}, {0, 63}, {63, 63}};
            for (int q = 0; q < 4; q++) for (int r = 0; r < 3; r++) lv[q][r] = (float)(R[3 * r] * wv[q][0] + R[3 * r + 1] * wv[q][1] + R[3 * r + 2] * wv[q][2] + T[r]);
            NsVerifCam gd; for (int k = 0; k < 9; k++) gd.A[k] = (float)R[k]; for (int k = 0; k < 3; k++) gd.t[k] = (float)T[k];
            ops[no] = vop(list_quad(lv, uv, 0, 1, 128, TX, CLX), 0x300000);
            CHECK(ns_verif_core(N.t, N.s, N.n, NULL, 0, own, (size_t)no, &gd, 1, 1.0f, ops, (uint32_t)no + 1, 0, &v) == 0 && v.missing == 2 && v.cls[NV_OTHER] == 2 && v.no_owner == 2 && v.unverified == 0, "sin dueño: otros=%u", v.cls[NV_OTHER]);
        }
        /* con la cámara buena entre las candidatas no se ajusta nada */
        NsVerifCam good; for (int k = 0; k < 9; k++) good.A[k] = (float)R[k]; for (int k = 0; k < 3; k++) good.t[k] = (float)T[k];
        NsVerifCam two[2] = {id, good};
        CHECK(ns_verif_core(N.t, N.s, N.n, NULL, 0, own, (size_t)no, two, 2, 1.0f, ops, (uint32_t)no, 0, &v) == 0 && v.missing == 0 && v.cam == 1, "cámara candidata 1: faltan=%u cam=%d", v.missing, v.cam);
        /* y una cara que el nativo NO tiene sigue faltando con la cámara recuperada */
        {
            float wv[4][3] = {{5000, 0, 9000}, {5600, 0, 9000}, {5000, 600, 9000}, {5600, 600, 9000}}, lv[4][3], uv[4][2] = {{0, 0}, {63, 0}, {0, 63}, {63, 63}};
            for (int q = 0; q < 4; q++) for (int r = 0; r < 3; r++) lv[q][r] = (float)(R[3 * r] * wv[q][0] + R[3 * r + 1] * wv[q][1] + R[3 * r + 2] * wv[q][2] + T[r]);
            own[no] = vown(0x100000u + 0x100u * 12u, 0x28, key, 2, NV_STATIC);
            ops[no] = vop(list_quad(lv, uv, 0, 1, 128, TX, CLX), own[no].lo); no++;
            CHECK(ns_verif_core(N.t, N.s, N.n, NULL, 0, own, (size_t)no, &id, 1, 1.0f, ops, (uint32_t)no, 0, &v) == 0 && v.missing == 2 && v.cls[NV_STATIC] == 2 && v.unverified == 24 && v.only_fit == 24 && v.cam == -1, "cara ausente con cámara ajustada: faltan=%u (solo con la ajustada=%u) cam=%d", v.missing, v.only_fit, v.cam);
            CHECK(ns_verif_core(N.t, N.s, N.n, NULL, 0, own, (size_t)no, two, 2, 1.0f, ops, (uint32_t)no, 0, &v) == 0 && v.missing == 2 && v.cls[NV_STATIC] == 2 && v.cam == 1 && !v.cam_distinct, "cara ausente con la cámara buena: faltan=%u cam=%d", v.missing, v.cam);
        }
        free(N.t); free(N.s);
    }
    /* 3. dueño dinámico en otra pose (animación de un fotograma a otro): cubierto si su pieza tiene un triángulo congruente; ausente si la pieza no está en el nativo */
    {
        Nat N = {0};
        nat_quad(&N, -500, -500, 4000, 1000, 0, 0x80100000u, 0, 1, 2);            /* escenario */
        nat_quad(&N, 2000, 0, 3000, 300, 0, 0x80120000u, 3, 1, 2);               /* pieza 3 de un objeto dinámico */
        NsVerifOwner own[2] = {vown(0x110000, 400, 0x80120000u, 3, NV_DYNAMIC), vown(0x111000, 400, 0x80120000u, 5, NV_DYNAMIC)};
        GpuOp mv = vop(list_of_quad(2400, 150, 3100, 300, 0, 1, 128), 0x110000);   /* la misma cara, desplazada 400 */
        CHECK(ns_verif_core(N.t, N.s, N.n, NULL, 0, own, 2, &id, 1, 1.0f, &mv, 1, 0, &v) == 0 && v.missing == 0 && v.moved == 2, "dinámico movido: faltan=%u otra_pose=%u", v.missing, v.moved);
        GpuOp gone = vop(list_of_quad(2400, 150, 3100, 300, 0, 1, 128), 0x111000);  /* pieza 5: no existe en el nativo */
        CHECK(ns_verif_core(N.t, N.s, N.n, NULL, 0, own, 2, &id, 1, 1.0f, &gone, 1, 0, &v) == 0 && v.missing == 2 && v.cls[NV_DYNAMIC] == 2, "dinámico sin pieza: faltan=%u", v.missing);
        GpuOp other = vop(list_of_quad(2400, 150, 3100, 777, 0, 1, 128), 0x110000);  /* otra forma: la pieza no tiene esa cara */
        CHECK(ns_verif_core(N.t, N.s, N.n, NULL, 0, own, 2, &id, 1, 1.0f, &other, 1, 0, &v) == 0 && v.missing == 2 && v.cls[NV_DYNAMIC] == 2, "dinámico con otra geometría: faltan=%u", v.missing);
        free(N.t); free(N.s);
    }
    /* 4. primitivas: de mundo sin elemento nativo = falta; con elemento del mismo paquete = cubierta; solo ordenación (cielo) sin generar = falta, generada = cubierta; fuera de grupo */
    {
        Nat N = {0};
        nat_quad(&N, -500, -500, 4000, 1000, 0, 0x80100000u, 0, 1, 2);
        NsVerifOwner own[3] = {vown(0x120000, 400, 0x80130000u, 0, NV_PRIM), vown(0x121000, 400, 0x80131000u, 0, NV_SORTONLY), vown(0x122000, 400, 0x80132000u, 0, NV_OFFGROUP)};
        GpuOp pr = vop(list_of_quad(6000, 0, 5000, 200, 1, 1, 128), 0x120000 + 40);   /* paquete 1 de la primitiva */
        CHECK(ns_verif_core(N.t, N.s, N.n, NULL, 0, own, 3, &id, 1, 1.0f, &pr, 1, 0, &v) == 0 && v.missing == 2 && v.cls[NV_PRIM] == 2, "primitiva sin elemento: prim=%u", v.cls[NV_PRIM]);
        NsVerifFxTri ft[2]; memset(ft, 0, sizeof ft);
        ft[0].key = 0x80130000u; ft[0].idx = 1; ft[0].p[0][0] = 9000; ft[0].p[1][0] = 9100; ft[0].p[2][1] = 100; ft[0].p[0][2] = ft[0].p[1][2] = ft[0].p[2][2] = 5000;   /* su elemento, en otro sitio (se movió) */
        CHECK(ns_verif_core(N.t, N.s, N.n, ft, 1, own, 3, &id, 1, 1.0f, &pr, 1, 0, &v) == 0 && v.missing == 0 && v.moved == 2, "primitiva con elemento: faltan=%u", v.missing);
        GpuOp sky = vop(list_of_quad(-160, -128, 1, 32, 0, 1, 128), 0x121000);
        for (int k = 0; k < 4; k++) sky.v[k].vvalid = 0;                                 /* solo ordenación: sin GTE */
        CHECK(ns_verif_core(N.t, N.s, N.n, NULL, 0, own, 3, &id, 1, 1.0f, &sky, 1, 0, &v) == 0 && v.missing == 2 && v.cls[NV_SORTONLY] == 2 && v.tris_gte == 0, "cielo sin generar: sortonly=%u", v.cls[NV_SORTONLY]);
        ft[1].key = 0x80131000u; ft[1].nogeo = 1;
        CHECK(ns_verif_core(N.t, N.s, N.n, ft, 2, own, 3, &id, 1, 1.0f, &sky, 1, 0, &v) == 0 && v.missing == 0, "cielo generado: faltan=%u", v.missing);
        sky.pkt_addr = 0x80400000u;
        CHECK(ns_verif_core(N.t, N.s, N.n, ft, 2, own, 3, &id, 1, 1.0f, &sky, 1, 0, &v) == 0 && v.missing == 0 && v.no_view_2d == 1, "HUD sin vista: 2D=%u", v.no_view_2d);
        GpuOp og = vop(list_of_quad(6000, 3000, 5000, 200, 0, 1, 128), 0x122000);
        CHECK(ns_verif_core(N.t, N.s, N.n, NULL, 0, own, 3, &id, 1, 1.0f, &og, 1, 0, &v) == 0 && v.missing == 2 && v.cls[NV_OFFGROUP] == 2, "fuera de grupo: %u", v.cls[NV_OFFGROUP]);
        /* muestreo acotado */
        GpuOp many[64]; for (int k = 0; k < 64; k++) many[k] = vop(list_of_quad(-500, -500, 4000, 1000, 0, 1, 128), 0x122000);   /* con dueño: se miden */
        CHECK(ns_verif_core(N.t, N.s, N.n, NULL, 0, own, 3, &id, 1, 1.0f, many, 64, 16, &v) == 0 && v.step == 8 && v.tris_gte == 16 && v.unverified == 0 && v.missing == 0, "muestreo: paso=%u medidos=%u", v.step, v.tris_gte);
        free(N.t); free(N.s);
    }
    /* 5. dueños desde la RAM: piezas de objetos (estático / dinámico / fuera de grupo, extensión tras la pieza cabecera) y primitivas (mundo / solo ordenación / pegada a la cámara) */
    {
        uint8_t *ram = (uint8_t *)calloc(1, 0x200000);
#define W16(a, x) do { uint32_t a_ = (a) & 0x1FFFFF; ram[a_] = (uint8_t)(x); ram[a_ + 1] = (uint8_t)((x) >> 8); } while (0)
#define W32(a, x) do { W16(a, (x) & 0xFFFF); W16((a) + 2, (uint32_t)(x) >> 16); } while (0)
        const uint32_t CH = 0x800B2B7Cu, Q = 0x800B2110u;
        W32(0x800ACAF8u, 1); W16(CH + 0x52, 6); W16(CH + 0x54, 3); W16(CH + 0x56, 3); W32(CH + 0x58, Q);
        const uint32_t o0 = 0x80150000u, o1 = 0x80151000u, o2 = 0x80152000u, def0 = 0x80140000u;
        W32(Q, o0); W32(Q + 4, o1); W32(Q + 8, o2);
        W32(def0 + 4, 2);                                                           /* la definición tiene 2 modelos: pieza 0 y su extensión (índice 1) */
        W32(o0 + 0x24, def0); W32(o0 + 0x28, 0x57); W16(o0 + 0x2C, 1); W16(o0 + 0x2E, 1);   /* escenario, grupo activo, 1 pieza */
        W16(o0 + 0x48 + 0x52, 3); W32(o0 + 0x48 + 0x54, 0x80160000u); W32(o0 + 0x48 + 0x48, o0 + 0x48 + 0x5C);
        W16(o0 + 0x48 + 0x5C + 0x52, 2);                                            /* extensión: 2 paquetes a continuación de los 3 de la cabecera */
        W32(o1 + 0x28, 0x0D); W16(o1 + 0x2C, 1); W16(o1 + 0x2E, 1); W16(o1 + 0x48 + 0x52, 4); W32(o1 + 0x48 + 0x58, 0x80161000u);   /* dinámico, búfer 1 */
        W32(o2 + 0x28, 0x57); W16(o2 + 0x2C, 4); W16(o2 + 0x2E, 1); W16(o2 + 0x48 + 0x52, 1); W32(o2 + 0x48 + 0x54, 0x80162000u);   /* escenario de otro grupo */
        const uint32_t p0 = 0x80170000u, p1 = 0x80171000u, p2 = 0x80172000u;
        W32(Q + 12, p0); W32(Q + 16, p1); W32(Q + 20, p2);
        W32(p0 + 0x24, 18); W16(p0 + 0x2A, 5); W16(p0 + 0x30, 40); W32(p0 + 0x40, 0x80180000u);            /* primitiva de mundo */
        W32(p1 + 0x24, 18 | 0x800); W16(p1 + 0x2A, 99); W16(p1 + 0x30, 40); W32(p1 + 0x40, 0x80181000u);   /* solo ordenación */
        W32(p2 + 0x24, 18 | 0x200); W16(p2 + 0x2A, 2); W16(p2 + 0x30, 40); W32(p2 + 0x40, 0x80182000u);    /* pegada a la cámara */
        NsVerifOwner own[32];
        size_t n = ns_verif_owners(ram, own, 32);
        CHECK(n == 7, "dueños: %zu", n);
        int ok = n == 7;
        for (size_t i = 1; i < n && ok; i++) ok = own[i - 1].lo <= own[i].lo;
        CHECK(ok, "dueños ordenados");
        struct { uint32_t lo, len, key; int piece, cls, screen; } want[7] = {
            {0x160000, 3 * 52, o0, 0, NV_STATIC, 0}, {0x160000 + 3 * 52, 2 * 52, o0, 1, NV_STATIC, 0}, {0x161000, 4 * 52, o1, 0, NV_DYNAMIC, 0}, {0x162000, 52, o2, 0, NV_OFFGROUP, 0},
            {0x180000, 200, p0, 0, NV_PRIM, 0}, {0x181000, 99 * 40, p1, 0, NV_SORTONLY, 0}, {0x182000, 80, p2, 0, NV_PRIM, 1}};
        for (int k = 0; k < 7 && n == 7; k++)
            CHECK(own[k].lo == want[k].lo && own[k].hi == want[k].lo + want[k].len && own[k].key == want[k].key && own[k].piece == want[k].piece && own[k].cls == want[k].cls && own[k].screen == want[k].screen,
                  "dueño %d: %x..%x key=%08x pieza=%u cls=%u pantalla=%u", k, own[k].lo, own[k].hi, own[k].key, own[k].piece, own[k].cls, own[k].screen);
        CHECK(ns_verif_owners(ram, own, 2) == 2 && ns_verif_owners(NULL, own, 2) == 0, "tope y argumentos");
        free(ram);
    }
}

/* ---- verificador: no se autoengaña (casos de la revisión adversarial) ---- */
static void vh_tri(Nat *N, const float a[3], const float b[3], const float c[3], uint32_t key, uint32_t piece, uint32_t mfl, uint32_t ifl) {
    float p[3][3], uv[3][2] = {{0, 0}, {63, 0}, {0, 63}};
    memcpy(p[0], a, 12); memcpy(p[1], b, 12); memcpy(p[2], c, 12);
    nat_push(N, p, uv, PAGE, CLUT, key, piece, 0, 0, mfl, 2, 128);
    N->s[N->n - 1].iflags = ifl;
}
/* operación de la lista: el triángulo de mundo (a,b,c) visto con la cámara mundo -> vista (R, T) */
static GpuOp vh_op(const float a[3], const float b[3], const float c[3], const float *R, const float *T, uint32_t pkt) {
    GpuOp o; memset(&o, 0, sizeof o);
    o.type = GPU_OP_POLY; o.nv = 3; o.pkt_addr = pkt;
    const float *P[3] = {a, b, c};
    for (int k = 0; k < 3; k++) {
        o.v[k].vvalid = 1;
        o.v[k].vx = R[0] * P[k][0] + R[1] * P[k][1] + R[2] * P[k][2] + T[0]; o.v[k].vy = R[3] * P[k][0] + R[4] * P[k][1] + R[5] * P[k][2] + T[1]; o.v[k].vz = R[6] * P[k][0] + R[7] * P[k][1] + R[8] * P[k][2] + T[2];
    }
    return o;
}
static void test_verif_honest(void) {
    /* escenario: 12 triángulos de un cubo irregular de ~1 m, una pieza estática */
    float P[8][3]; for (int i = 0; i < 8; i++) { P[i][0] = (i & 1) ? 1100 : 0; P[i][1] = (i & 2) ? 900 : 0; P[i][2] = (i & 4) ? 1300 : 0; P[i][0] += 37.f * (float)i; P[i][2] += 4000; }
    static const int F[12][3] = {{0,1,3},{0,3,2},{4,5,7},{4,7,6},{0,1,5},{0,5,4},{2,3,7},{2,7,6},{0,2,6},{0,6,4},{1,3,7},{1,7,5}};
    Nat N = {0};
    for (int i = 0; i < 12; i++) vh_tri(&N, P[F[i][0]], P[F[i][1]], P[F[i][2]], 0x80150000u, 0, 0, NS_IF_STATIC);
    NsVerifOwner own[2] = {vown(0x100000, 14 * 52, 0x80150000u, 0, NV_STATIC), vown(0x110000, 52, 0x80160000u, 0, NV_DYNAMIC)};
    own[0].psize = own[1].psize = 52;
    NsVerifCam id; memset(&id, 0, sizeof id); id.A[0] = id.A[4] = id.A[8] = 1.0f;
    const float I9[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, Z3[3] = {0, 0, 0};
    GpuOp ops[16]; NsVerif v; char buf[1024];
    /* (a) la lista se emitió con una cámara a 40 grados y varios metros de la de la app (la última candidata): la ajustada la recupera, pero NO vale como equivalencia */
    {
        const float a = 40.0f * 3.14159265f / 180.0f, R[9] = {cosf(a), 0, sinf(a), 0, 1, 0, -sinf(a), 0, cosf(a)}, T[3] = {-3000, 400, 5000};
        for (int i = 0; i < 12; i++) ops[i] = vh_op(P[F[i][0]], P[F[i][1]], P[F[i][2]], R, T, 0x80100000u + 52u * (uint32_t)i);
        CHECK(ns_verif_core(N.t, N.s, N.n, NULL, 0, own, 1, &id, 1, 1.0f, ops, 12, 0, &v) == 0 && v.cam == -1 && v.cam_distinct == 1, "cámara distinta: cam=%d distinta=%d", v.cam, v.cam_distinct);
        CHECK(v.cam_dist > 5000.0f && v.cam_dist < 6500.0f && fabsf(v.cam_giro - 40.0f) < 1.0f && v.app_in < 0.05f, "distancia=%.0f giro=%.2f app_explica=%.2f", v.cam_dist, v.cam_giro, v.app_in);
        CHECK(v.missing == 0 && v.unverified == 12 && v.only_fit == 12, "solo casan con la ajustada: sin verificar, no ausentes: faltan=%u sin_verificar=%u solo_ajustada=%u", v.missing, v.unverified, v.only_fit);
        ns_verif_format(&v, 1, buf, sizeof buf);
        CHECK(strstr(buf, "camara=DISTINTA") && strstr(buf, "cam_dist=") && strstr(buf, "cam_giro=") && strstr(buf, "app_explica=0%") && strstr(buf, "solo_ajustada=12") && strstr(buf, "sin_equivalente=0 ") && strstr(buf, "sin_verificar=12 ok=no"), "formato: %s", buf);
        /* la cámara ajustada coincide con una cámara REGISTRADA con la lista (candidata 0, a 50 unidades de la real: no explica las muestras pero la respalda): no es distinta aunque la de la app (la
         * última) esté lejos; la distancia y el giro que se informan son los de la app */
        NsVerifCam two[2]; two[1] = id;
        memcpy(two[0].A, R, sizeof two[0].A); two[0].t[0] = T[0] + 50; two[0].t[1] = T[1]; two[0].t[2] = T[2];
        CHECK(ns_verif_core(N.t, N.s, N.n, NULL, 0, own, 1, two, 2, 1.0f, ops, 12, 0, &v) == 0 && v.cam == -1 && v.cam_distinct == 0 && v.missing == 0 && v.only_fit == 0 && v.cam_dist > 5000.0f && v.app_in < 0.05f,
              "ajustada junto a la cámara de la lista: cam=%d distinta=%d faltan=%u dist_app=%.0f", v.cam, v.cam_distinct, v.missing, v.cam_dist);
        /* giro de 2 grados sin moverse del sitio: por encima de NV_CAM_ROT_TOL aunque la distancia sea 0 */
        const float g2 = 2.0f * 3.14159265f / 180.0f, R3[9] = {cosf(g2), 0, sinf(g2), 0, 1, 0, -sinf(g2), 0, cosf(g2)};
        for (int i = 0; i < 12; i++) ops[i] = vh_op(P[F[i][0]], P[F[i][1]], P[F[i][2]], R3, Z3, 0x80100000u + 52u * (uint32_t)i);
        CHECK(ns_verif_core(N.t, N.s, N.n, NULL, 0, own, 1, &id, 1, 1.0f, ops, 12, 0, &v) == 0 && v.cam == -1 && v.cam_distinct == 1 && v.cam_dist < NV_CAM_DIST_TOL && fabsf(v.cam_giro - 2.0f) < 0.2f, "giro de 2 grados: distinta=%d dist=%.0f giro=%.2f", v.cam_distinct, v.cam_dist, v.cam_giro);
        for (int i = 0; i < 12; i++) ops[i] = vh_op(P[F[i][0]], P[F[i][1]], P[F[i][2]], R, T, 0x80100000u + 52u * (uint32_t)i);
        /* la misma lista con la cámara apenas movida (un fotograma de diferencia): se ajusta, no es distinta y no falta nada */
        const float b = 0.2f * 3.14159265f / 180.0f, R2[9] = {cosf(b), 0, sinf(b), 0, 1, 0, -sinf(b), 0, cosf(b)}, T2[3] = {12, -5, 9};
        for (int i = 0; i < 12; i++) ops[i] = vh_op(P[F[i][0]], P[F[i][1]], P[F[i][2]], R2, T2, 0x80100000u + 52u * (uint32_t)i);
        CHECK(ns_verif_core(N.t, N.s, N.n, NULL, 0, own, 1, &id, 1, 1.0f, ops, 12, 0, &v) == 0 && v.cam_distinct == 0 && v.missing == 0 && v.only_fit == 0 && v.cam_giro < 0.5f, "cámara casi igual: cam=%d distinta=%d faltan=%u giro=%.2f", v.cam, v.cam_distinct, v.missing, v.cam_giro);
        ns_verif_format(&v, 1, buf, sizeof buf);
        CHECK(strstr(buf, " ok=si "), "formato limpio: %s", buf);
    }
    /* lista con la cámara de la app para el resto */
    for (int i = 0; i < 12; i++) ops[i] = vh_op(P[F[i][0]], P[F[i][1]], P[F[i][2]], I9, Z3, 0x80100000u + 52u * (uint32_t)i);
    /* (b) pieza dinámica SOLDADA que el nativo tiene a 50 m de donde la dibuja la lista: no es «otra pose» */
    {
        const float q0[3] = {50000, 0, 0}, q1[3] = {50010, 0, 0}, q2[3] = {50000, 7, 0}, l0[3] = {-2000, 0, 4000}, l1[3] = {-1500, 0, 4000}, l2[3] = {-2000, 600, 4000};
        vh_tri(&N, q0, q1, q2, 0x80160000u, 0, NS_MF_WELDED, 0);
        ops[12] = vh_op(l0, l1, l2, I9, Z3, 0x80110000u);
        CHECK(ns_verif_core(N.t, N.s, N.n, NULL, 0, own, 2, &id, 1, 1.0f, ops, 13, 0, &v) == 0 && v.cam_distinct == 0 && v.missing == 1 && v.cls[NV_DYNAMIC] == 1 && v.moved == 0, "soldada lejos: faltan=%u dinamico=%u otra_pose=%u", v.missing, v.cls[NV_DYNAMIC], v.moved);
        /* la misma pieza soldada a 20 cm (animación de un fotograma): otra pose */
        const float m0[3] = {49800, 0, 0}, m1[3] = {50300, 30, 0}, m2[3] = {49800, 300, 40};
        ops[12] = vh_op(m0, m1, m2, I9, Z3, 0x80110000u);
        CHECK(ns_verif_core(N.t, N.s, N.n, NULL, 0, own, 2, &id, 1, 1.0f, ops, 13, 0, &v) == 0 && v.missing == 0 && v.moved == 1, "soldada cerca: faltan=%u otra_pose=%u", v.missing, v.moved);
    }
    /* (c) polígono del escenario sin posición de vista: se cuenta aparte */
    {
        ops[13] = ops[0]; ops[13].pkt_addr = 0x80100000u + 52u * 13u; for (int k = 0; k < 3; k++) ops[13].v[k].vvalid = 0;
        CHECK(ns_verif_core(N.t, N.s, N.n, NULL, 0, own, 2, &id, 1, 1.0f, ops, 14, 0, &v) == 0 && v.no_view_static == 1, "sin vista estático: %u", v.no_view_static);
        ns_verif_format(&v, 2, buf, sizeof buf);
        CHECK(strstr(buf, "sin_vista_estatico=1"), "formato: %s", buf);
    }
    /* (d) lista sin dueños y cámara que no explica nada: no se puede verificar (ni equivalente ni ausente) */
    {
        for (int i = 0; i < 12; i++) ops[i].pkt_addr = 0;
        CHECK(ns_verif_core(N.t, N.s, N.n, NULL, 0, own, 0, &id, 1, 1.0f, ops, 12, 0, &v) == 0 && v.unverified == 12 && v.tris_gte == 0 && v.missing == 0, "sin verificar: %u tris=%u faltan=%u", v.unverified, v.tris_gte, v.missing);
        CHECK(ns_verif_core(NULL, NULL, 0, NULL, 0, own, 0, &id, 1, 1.0f, ops, 12, 0, &v) == 0 && v.unverified == 12 && v.missing == 0, "nativo vacío sin dueños: sin verificar=%u", v.unverified);
        ns_verif_format(&v, 4, buf, sizeof buf);
        CHECK(strstr(buf, "sin_verificar=12"), "formato: %s", buf);
    }
    free(N.t); free(N.s);
}

/* ---- [descartes]: descartes por triángulo del modo ENGINE (réplica del sombreador en el anfitrión) y nativo frente a la lista en la ventana del motor ---- */
static GpuOp wc_op(const float a[3], const float b[3], const float c[3]) {   /* polígono 3D de la lista con la cámara identidad (x, y en px = 320 * v / z) */
    const float I9[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, Z3[3] = {0, 0, 0};
    GpuOp o = vh_op(a, b, c, I9, Z3, 0x80100000u);
    for (int k = 0; k < 3; k++) { o.v[k].x = (int16_t)(320.0f * o.v[k].vx / o.v[k].vz); o.v[k].y = (int16_t)(320.0f * o.v[k].vy / o.v[k].vz); }
    return o;
}
static void test_discards(void) {
    Nat N = {0};
    NsGpuEngCam c; memset(&c, 0, sizeof c);
    c.x[0] = 1; c.y[1] = 1; c.fwd[2] = 1; c.h = 320; c.hx = 320; c.hy = 320;
    const float f0[3] = {-500, 300, 1000}, f1[3] = {500, 300, 1000}, f2[3] = {0, 300, 3000};       /* suelo delante */
    const float b0[3] = {-500, 300, -3000}, b1[3] = {500, 300, -3000}, b2[3] = {0, 300, -1000};    /* suelo detrás de la cámara */
    const float g0[3] = {-40000, 0, 20000}, g1[3] = {40000, 0, 20000}, g2[3] = {0, 100, 30000};    /* enorme y lejano: lo pierde la tarjeta; rescatado fuera de la ventana */
    const float s0[3] = {300, -400, 94}, s1[3] = {900, 500, 120}, s2[3] = {400, 600, 300};         /* enorme y pegado a la cámara (zmax < H): descartado entero */
    vh_tri(&N, f0, f1, f2, 0x80150000u, 0, NS_MF_TWOSIDED, NS_IF_STATIC);
    vh_tri(&N, b0, b1, b2, 0x80150000u, 0, NS_MF_TWOSIDED, NS_IF_STATIC);
    vh_tri(&N, g0, g1, g2, 0x80150000u, 0, NS_MF_TWOSIDED, NS_IF_STATIC);
    vh_tri(&N, s0, s1, s2, 0x80150000u, 0, NS_MF_TWOSIDED, NS_IF_STATIC);
    const int f_back = ns_gpu_tri_back(f0, f1, f2, c.eye);                                         /* dinámico de una sola cara con los dos sentidos: uno es reverso */
    vh_tri(&N, f0, f_back ? f2 : f1, f_back ? f1 : f2, 0x80160000u, 0, 0, NS_IF_GBOUND);           /* [4] anverso */
    vh_tri(&N, f0, f_back ? f1 : f2, f_back ? f2 : f1, 0x80160000u, 0, 0, NS_IF_GBOUND);           /* [5] reverso */
    NsDiscards d;
    ns_discards_core(N.t, N.s, N.n, &c, NULL, 0, 0, &d);
    CHECK(d.split == 1 && d.size == 0 && d.drawn == 4 && d.drawn_static == 3, "«Original»: el enorme y cercano de un objeto sin GBOUND se parte: partidas=%u tamano=%u dibujados=%u", d.split, d.size, d.drawn);
    ns_discards_core(N.t, N.s, N.n, &c, NULL, 0, NSG_ENG_OFF_SPLIT, &d);
    CHECK(d.split == 0 && d.sent == 6 && d.behind == 1 && d.size == 1 && d.back == 1 && d.extra == 1 && d.drawn == 3 && d.drawn_static == 2 && d.drawn_dyn == 1 && ns_gpu_tri_oversize((const float (*)[3])(const float[3][3]){{300, -400, 160}, {900, 500, 160}, {400, 600, 300}}, 320, 320),
          "enviados=%u detras=%u tamano=%u reverso=%u extra=%u dibujados=%u (%u + %u)", d.sent, d.behind, d.size, d.back, d.extra, d.drawn, d.drawn_static, d.drawn_dyn);
    ns_discards_core(N.t, N.s, N.n, &c, NULL, 0, NSG_ENG_OFF_BEHIND | NSG_ENG_OFF_SIZE | NSG_ENG_OFF_BACK | NSG_ENG_OFF_SPLIT, &d);
    CHECK(d.behind == 0 && d.size == 0 && d.back == 0 && d.extra == 0 && d.drawn == 6, "reglas apagadas: dibujados=%u", d.drawn);
    /* la segunda mitad de un cuadrilátero llega de ns_adapt como (B,D,C) y el sombreador la dibuja como (B,C,D): marcado como segunda mitad, el ANVERSO [4] pasa a ser el reverso y al revés */
    N.s[4].half = 1;
    ns_discards_core(N.t, N.s, N.n, &c, NULL, 0, NSG_ENG_OFF_SPLIT, &d);
    CHECK(d.back == 2 && d.drawn_dyn == 0, "segunda mitad: el orden del sombreador invierte el sentido: reverso=%u dinamicos=%u", d.back, d.drawn_dyn);
    N.s[5].half = 1;
    ns_discards_core(N.t, N.s, N.n, &c, NULL, 0, NSG_ENG_OFF_SPLIT, &d);
    CHECK(d.back == 1 && d.drawn_dyn == 1, "las dos como segunda mitad: reverso=%u dinamicos=%u", d.back, d.drawn_dyn);
    N.s[4].half = N.s[5].half = 0;
    ns_discards_core(N.t, N.s, N.n, &c, NULL, 0, NSG_ENG_OFF_SPLIT, &d);
    {   /* el rescate del reverso lo decide cada pase con su ojo (ns_vertex): una pared de canto entre los dos ojos solo la rescata uno */
        const float w0[3] = {10, -200, 2000}, w1[3] = {10, 200, 2000}, w2[3] = {10, 0, 4000};
        const float eyes[2][3] = {{-30, 0, 0}, {30, 0, 0}}, same[2][3] = {{30, 0, 0}, {30, 0, 0}};
        const int wb = ns_gpu_tri_back(w0, w1, w2, c.eye);
        Nat E = {0}; NsDiscards e;
        vh_tri(&E, w0, wb ? w1 : w2, wb ? w2 : w1, 0x80160000u, 0, 0, NS_IF_GBOUND);               /* reverso para el motor y para el ojo izquierdo, anverso para el derecho */
        ns_discards_core(E.t, E.s, E.n, &c, eyes, 2, 0, &e);
        CHECK(e.extra == 1 && e.extra_one == 1 && e.back == 0, "rescatado por un solo ojo: extra=%u un_ojo=%u reverso=%u", e.extra, e.extra_one, e.back);
        ns_discards_core(E.t, E.s, E.n, &c, same, 2, 0, &e);
        CHECK(e.extra == 1 && e.extra_one == 0, "rescatado por los dos ojos: extra=%u un_ojo=%u", e.extra, e.extra_one);
        ns_discards_core(E.t, E.s, E.n, &c, eyes, 1, 0, &e);
        CHECK(e.extra == 0 && e.extra_one == 0 && e.back == 1, "con el ojo que lo ve por detrás: reverso=%u", e.back);
        free(E.t); free(E.s);
    }
    {   /* quién se parte lo dice la bandera GBOUND del objeto, no si es escenario: el enorme y cercano de un dinámico sin GBOUND se parte; el de un objeto GBOUND (aunque sea escenario), no */
        const float k0[3] = {2000, 300, 10}, k1[3] = {-2000, 300, 10}, k2[3] = {0, 300, 4000};
        Nat K = {0}; NsDiscards e; NsWinCmp w;
        vh_tri(&K, k0, k1, k2, 0x80160000u, 0, NS_MF_TWOSIDED, 0);
        ns_discards_core(K.t, K.s, K.n, &c, NULL, 0, 0, &e);
        ns_wincmp(K.t, K.s, K.n, &c, 0, NULL, 0, &w);
        CHECK(e.split == 1 && e.extra == 0 && w.split > 1000, "dinámico sin GBOUND: partidas=%u extra=%u celdas=%u", e.split, e.extra, w.split);
        K.s[0].iflags = NS_IF_GBOUND | NS_IF_STATIC;
        ns_discards_core(K.t, K.s, K.n, &c, NULL, 0, 0, &e);
        ns_wincmp(K.t, K.s, K.n, &c, 0, NULL, 0, &w);
        CHECK(e.split == 0 && e.extra == 1 && w.split == 0, "objeto GBOUND: no se parte: partidas=%u extra=%u celdas=%u", e.split, e.extra, w.split);
        free(K.t); free(K.s);
    }
    /* nativo frente a la lista en la ventana del motor */
    {
        NsWinCmp w; GpuOp ops[2];
        ops[0] = wc_op(f0, f1, f2);                                                                /* la lista dibuja el suelo de delante: el nativo también ([0] y [4]) */
        ns_wincmp(N.t, N.s, N.n, &c, 0, ops, 1, &w);
        CHECK(w.cells == 160 * 112 && w.cover == 0 && w.extra == 0 && w.missing == 0, "igual que la lista: tapa=%u de_mas=%u falta=%u", w.cover, w.extra, w.missing);
        {   /* un suelo del escenario que pasa bajo la cámara: «Original» lo parte y lo dibuja; no cuenta como tapa ni de más (la lista parcheada no lo tiene), va en `split` */
            const float k0[3] = {2000, 300, 10}, k1[3] = {-2000, 300, 10}, k2[3] = {0, 300, 4000};
            Nat K = {0};
            vh_tri(&K, k0, k1, k2, 0x80150000u, 0, NS_MF_TWOSIDED, NS_IF_STATIC);
            ns_wincmp(K.t, K.s, K.n, &c, 0, ops, 0, &w);
            CHECK(w.split > 1000 && w.extra == 0 && w.cover == 0, "cara partida: partidas=%u de_mas=%u", w.split, w.extra);
            ns_wincmp(K.t, K.s, K.n, &c, NSG_ENG_OFF_SPLIT, ops, 0, &w);
            CHECK(w.split == 0 && w.extra == 0, "«Recortadas»: ni partida ni dentro de la ventana: partidas=%u de_mas=%u", w.split, w.extra);
            free(K.t); free(K.s);
        }
        const float n0[3] = {-100, -50, 500}, n1[3] = {100, -50, 500}, n2[3] = {0, 100, 500};      /* un triángulo cercano que el juego NO dibuja, delante del suelo */
        Nat M = {0};
        for (size_t i = 0; i < N.n; i++) { float p[3][3]; for (int k = 0; k < 3; k++) memcpy(p[k], N.t[i].v[k].p, 12); vh_tri(&M, p[0], p[1], p[2], N.s[i].key, 0, N.s[i].mflags, N.s[i].iflags); }
        vh_tri(&M, n0, n1, n2, 0x80170000u, 0, NS_MF_TWOSIDED, NS_IF_GBOUND);
        ns_wincmp(M.t, M.s, M.n, &c, 0, ops, 1, &w);
        CHECK(w.cover > 0 && w.extra > 1000 && w.missing == 0, "nativo de más y tapando: tapa=%u de_mas=%u", w.cover, w.extra);
        ops[1] = wc_op(n0, n1, n2);
        ns_wincmp(N.t, N.s, N.n, &c, 0, ops, 2, &w);
        CHECK(w.cover == 0 && w.extra == 0 && w.missing > 1000, "falta en el nativo: falta=%u", w.missing);
        ns_wincmp(N.t, N.s, N.n, &c, 0, ops, 1, &w);
        CHECK(w.extra == 0, "lo rescatado no cuenta dentro de la ventana: de_mas=%u", w.extra);
        ns_wincmp(N.t, N.s, N.n, &c, NSG_ENG_OFF_SIZE, ops, 1, &w);                                /* con el tope apagado los enormes [2] y [3] se dibujan en la ventana */
        CHECK(w.extra > 0, "regla apagada: de_mas=%u", w.extra);
        d.published = 9; char buf[400];
        ns_wincmp(M.t, M.s, M.n, &c, 0, ops, 1, &w);
        size_t len = ns_discards_format(&d, 31.4f, &w, NSG_ENG_OFF_SPLIT, buf, sizeof buf);
        CHECK(len > 0 && strstr(buf, "[descartes] caras=recortadas publicados=9 enviados=6 detras=1 tamano=1 reverso=1 extra=1 (un_ojo=0) partidas=0 dibujados=3 (escenario=2 dinamicos=1) salto=31% | lista: tapa=") && strstr(buf, " partidas=") && strstr(buf, " de_mas=") && strstr(buf, " falta=0.0%"), "formato: %s", buf);
        ns_discards_format(&d, 0, NULL, 0, buf, sizeof buf);
        CHECK(strstr(buf, "[descartes] caras=original ") && strstr(buf, "| lista: sin_medida"), "formato sin lista: %s", buf);
        free(M.t); free(M.s);
    }
    free(N.t); free(N.s);
}

int main(void) {
    test_match();
    test_split_diag();
    test_subdiv_and_other_tex();
    test_billboard_pose_absent();
    test_pose_static_owner();
    test_verif();
    test_verif_honest();
    test_discards();
    test_reverse();
    test_color_shift_degenerate();
    test_camera_and_format();
    test_many();
    test_run_and_glue();
    test_screen_ys();
    test_holes();
    if (fails) { printf("%d FALLOS\n", fails); return 1; }
    printf("OK: todas las pruebas de ns_cover pasan\n");
    return 0;
}
