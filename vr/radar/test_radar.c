/* Pruebas sintéticas de vr_radar (sin datos del juego): marcas, separación, conos ampliados, anclaje al antebrazo, mirada.
 *   test_radar                 ejecuta las pruebas
 *   test_radar --ref DIR [ppm] escribe DIR/wrist_ref.ppm: referencia por software del quad en un brazo sintético visto desde el ojo
 *                              (ppm opcional: textura del radar; por defecto DIR/radar_06438_ext.ppm o un patrón procedural) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glob.h>
#include "cpu.h"
#include "sys.h"
#include "vr_radar.h"
#include "vr_glove.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fprintf(stderr, "FALLO %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)
#define NEAR(a, b, e) (fabs((double)(a) - (double)(b)) <= (e))

/* ------------------------------------------------------------------------------------------------ marcas */
static void t_marks(void) {
    vrradar_reset();
    vrradar_on_mark(VRRADAR_MARK_END, 5);                       /* fin sin inicio: se ignora */
    CHECK(!vrradar_seq_is_radar(4), "fin sin inicio");
    vrradar_on_mark(VRRADAR_MARK_BEGIN, 10);
    vrradar_on_mark(VRRADAR_MARK_END, 20);
    CHECK(!vrradar_seq_is_radar(9) && vrradar_seq_is_radar(10) && vrradar_seq_is_radar(19) && !vrradar_seq_is_radar(20), "rango [10,20)");
    vrradar_on_mark(VRRADAR_MARK_BEGIN, 30);
    vrradar_on_mark(VRRADAR_MARK_BEGIN, 40);                    /* un inicio nuevo sustituye al abierto */
    vrradar_on_mark(VRRADAR_MARK_END, 50);
    CHECK(!vrradar_seq_is_radar(35) && vrradar_seq_is_radar(45), "inicio doble");
    vrradar_on_mark(VRRADAR_MARK_BEGIN, 60);
    vrradar_on_mark(VRRADAR_MARK_END, 60);                      /* vacío: no se registra */
    CHECK(!vrradar_seq_is_radar(60), "rango vacío");
    vrradar_on_mark(0x123456, 70);                              /* id ajeno */
    uint32_t lo[64], hi[64];
    CHECK(vrradar_ranges(lo, hi, 64) == 2, "n rangos");
    for (uint32_t i = 0; i < 100; i++) { vrradar_on_mark(VRRADAR_MARK_BEGIN, 1000 + 100 * i); vrradar_on_mark(VRRADAR_MARK_END, 1010 + 100 * i); }
    CHECK(!vrradar_seq_is_radar(1000) && vrradar_seq_is_radar(1000 + 100 * 99 + 5) && vrradar_seq_is_radar(1000 + 100 * 70 + 5), "anillo");
    vrradar_reset();
    vrradar_on_mark(VRRADAR_MARK_BEGIN, 0xFFFFFFF0u);           /* envoltura del contador */
    vrradar_on_mark(VRRADAR_MARK_END, 0x10u);
    CHECK(vrradar_seq_is_radar(0xFFFFFFF8u) && vrradar_seq_is_radar(3) && !vrradar_seq_is_radar(0x10u) && !vrradar_seq_is_radar(0xFFFFFFE0u), "envoltura");
}

/* ------------------------------------------------------------------------------------------------ lista sintética */
#define MAXO 64
static GpuOp ops[MAXO];
static uint32_t nops, seqc;

static GpuOp *mk(int type) {
    GpuOp *o = &ops[nops++];
    memset(o, 0, sizeof *o);
    o->type = (uint8_t)type;
    o->seq = seqc++;
    o->cx2 = 1023; o->cy2 = 511;
    return o;
}
static void rgb(GpuVtx *v, int r, int g, int b) { v->r = (uint8_t)r; v->g = (uint8_t)g; v->b = (uint8_t)b; }
static GpuOp *mk_rect(int x, int y, int w, int h, int r, int g, int b, int ox, int oy, int semi) {
    GpuOp *o = mk(GPU_OP_RECT);
    o->v[0].x = (int16_t)x; o->v[0].y = (int16_t)y; o->rw = (int16_t)w; o->rh = (int16_t)h;
    rgb(&o->v[0], r, g, b);
    o->dox = (int16_t)ox; o->doy = (int16_t)oy;
    o->flags = (uint16_t)(semi ? GPU_F_SEMI : 0);
    o->abr = 1;
    return o;
}
static GpuOp *mk_cone(const float p[4][2], const uint8_t m[3], const uint8_t f[3], int ox, int oy) {
    GpuOp *o = mk(GPU_OP_POLY);
    o->nv = 4;
    o->flags = GPU_F_GOURAUD | GPU_F_SEMI | GPU_F_QUAD;
    o->abr = 1;
    o->dox = (int16_t)ox; o->doy = (int16_t)oy;
    for (int i = 0; i < 4; i++) {
        o->v[i].x = (int16_t)p[i][0]; o->v[i].y = (int16_t)p[i][1];
        const uint8_t *c = i == 1 ? m : f;
        rgb(&o->v[i], c[0], c[1], c[2]);
    }
    return o;
}
static GpuOp *mk_line(int x0, int y0, int x1, int y1, int r, int g, int b, int ox, int oy) {
    GpuOp *o = mk(GPU_OP_LINE);
    o->nv = 2;
    o->v[0].x = (int16_t)x0; o->v[0].y = (int16_t)y0; o->v[1].x = (int16_t)x1; o->v[1].y = (int16_t)y1;
    rgb(&o->v[0], r, g, b); rgb(&o->v[1], r, g, b);
    o->dox = (int16_t)ox; o->doy = (int16_t)oy;
    return o;
}

static const uint8_t SOLD_M[3] = {0x00, 0x80, 0x80}, SOLD_F[3] = {0x00, 0x00, 0x10};
static const uint8_t ALRT_M[3] = {0xA0, 0x00, 0x00}, ALRT_F[3] = {0x10, 0x00, 0x00};
static const uint8_t CAM_M[3] = {0xA0, 0xA0, 0x00}, CAM_F[3] = {0x08, 0x08, 0x00};
static const uint8_t PLY_M[3] = {0x00, 0xA0, 0x48}, PLY_F[3] = {0, 0, 0};

static GpuDrawList L;
static uint32_t idx_cone_first;
#define CX 269
#define CY 42

static void build_list(int with_cam_player) {
    nops = 0; seqc = 100;
    GpuOp *o = mk(GPU_OP_FILL); o->rx = 0; o->ry = 0; o->rw = 320; o->rh = 240;                       /* evento HUD */
    o = mk_rect(10, 10, 20, 8, 200, 200, 200, 0, 0, 0);                                                 /* HUD */
    mk(GPU_OP_UPLOAD)->rw = 1;                                                                          /* evento */
    vrradar_reset();
    vrradar_on_mark(VRRADAR_MARK_BEGIN, seqc);
    mk_rect(CX - 34, CY - 26, 69, 52, 0x18, 0x18, 0, CX, CY, 1)->abr = 0;                               /* fondo (semi) */
    mk_line(-30, -10, 20, -10, 0x00, 0xA0, 0x48, CX, CY);                                               /* pared */
    mk_line(-30, 0, 20, 12, 0x00, 0xA0, 0x48, CX, CY);
    mk(GPU_OP_UPLOAD)->rw = 1;                                                                          /* evento entre medias */
    idx_cone_first = nops;
    {
        /* A, O, F, B: el origen está dentro de la ventana */
        float p[4][2] = {{-5, -22}, {-20, 4}, {10, 10}, {18, -12}};
        for (int i = 0; i < 4; i++) { p[i][0] += CX; p[i][1] += CY; }
        mk_cone(p, SOLD_M, SOLD_F, CX, CY);                                                             /* vértices con el offset ya sumado */
    }
    {
        float p[4][2] = {{-15, 15}, {-25, 20}, {-10, 25}, {-30, 28}};
        for (int i = 0; i < 4; i++) { p[i][0] += CX; p[i][1] += CY; }
        mk_cone(p, ALRT_M, ALRT_F, CX, CY);                                                               /* soldado en alerta */
    }
    if (with_cam_player) {
        float p[4][2] = {{0, 0}, {5, 5}, {10, 0}, {5, -8}};
        for (int i = 0; i < 4; i++) { p[i][0] += CX; p[i][1] += CY; }
        mk_cone(p, CAM_M, CAM_F, CX, CY);                                                                 /* cámara de vigilancia */
        mk_cone(p, PLY_M, PLY_F, CX, CY);                                                                 /* cono del jugador */
    }
    mk_rect(0, 0, 1, 1, 0xC8, 0xC8, 0xC8, CX, CY, 0);                                                   /* punto del jugador */
    mk_rect(CX - 35, CY - 27, 71, 1, 0, 0, 0, 0, 0, 0);                                                 /* marco: 4 rects con el entorno base */
    mk_rect(CX - 35, CY - 27, 1, 54, 0, 0, 0, 0, 0, 0);
    mk_rect(CX + 35, CY - 27, 1, 54, 0, 0, 0, 0, 0, 0);
    mk_rect(CX - 35, CY + 26, 71, 1, 0, 0, 0, 0, 0, 0);
    vrradar_on_mark(VRRADAR_MARK_END, seqc);
    mk_rect(100, 100, 8, 8, 255, 255, 255, 0, 0, 0);                                                    /* HUD tras el radar */
    memset(&L, 0, sizeof L);
    L.ops = ops; L.nops = nops; L.cap_ops = MAXO;
    L.disp.w = 320; L.disp.h = 240;
    (void)o;
}

static double tri_area(const VrVtx *a) {
    return 0.5 * fabs((a[1].pos[0] - a[0].pos[0]) * (a[2].pos[1] - a[0].pos[1]) - (a[1].pos[1] - a[0].pos[1]) * (a[2].pos[0] - a[0].pos[0]));
}

static void t_split_build(void) {
    build_list(1);
    VrRadarSplit s;
    CHECK(vrradar_split(&L, &s) == 0 && s.present, "split");
    uint32_t nrad = 0, ev = 0;
    for (uint32_t i = 0; i < L.nops; i++) { if (ops[i].type == GPU_OP_UPLOAD || ops[i].type == GPU_OP_FILL) ev++; else if (vrradar_seq_is_radar(ops[i].seq)) nrad++; }
    CHECK(s.n_ops == nrad && nrad == 1 + 2 + 4 + 1 + 4 + 0 + 0 + 0, "ops radar %u", s.n_ops);   /* fondo, 2 líneas, 4 conos, punto, 4 marco... (ver abajo) */
    CHECK(s.hud->nops + nrad == L.nops, "hud + radar == total");
    CHECK(s.rect[0] == CX - 35 && s.rect[1] == CY - 27 && s.rect[2] == 71 && s.rect[3] == 54, "rect %d,%d,%d,%d", s.rect[0], s.rect[1], s.rect[2], s.rect[3]);
    CHECK(s.center[0] == CX && s.center[1] == CY, "centro");
    uint32_t hud_ev = 0;
    for (uint32_t i = 0; i < s.hud->nops; i++) { int t = s.hud->ops[i].type; if (t == GPU_OP_UPLOAD || t == GPU_OP_FILL) hud_ev++; CHECK(!(t == GPU_OP_RECT && vrradar_seq_is_radar(s.hud->ops[i].seq)), "radar en hud"); }
    CHECK(hud_ev == ev, "eventos conservados");
    for (uint32_t i = 0; i < s.radar->nops; i++) { const GpuOp *o = &s.radar->ops[i]; CHECK(!o->v[0].vvalid && !o->v[1].vvalid, "vvalid del radar"); }

    /* sin percepción: conos tal cual, sin extra */
    VrRadarPerc off = {0, 1.0f, 0.6f, 0.5f};
    vrradar_set_perception(&off);
    VrRadarScene a;
    CHECK(vrradar_build(&s, &a) == 0 && a.present, "build");
    CHECK(a.n_cones == 2 && a.n_cones_ext == 0 && a.n_ext_tris == 0, "conos de soldado %u (cámara y jugador no cuentan), ext %u", a.n_cones, a.n_cones_ext);
    uint32_t nv0 = a.nv;
    /* ev_end en índices de la lista HUD */
    CHECK(a.nb >= 2, "al menos 2 lotes (hay un evento entre medias)");
    uint32_t hud_idx_ev3 = 0;
    {   /* índice HUD del tercer evento (el que cae entre las primitivas del radar) */
        uint32_t k = 0;
        for (uint32_t i = 0; i < s.hud->nops; i++) { int t = s.hud->ops[i].type; if (t == GPU_OP_UPLOAD || t == GPU_OP_FILL) { if (++k == 3) { hud_idx_ev3 = i; break; } } }
    }
    CHECK(a.b[0].ev_end == hud_idx_ev3, "ev_end primer lote %u (esperado %u)", a.b[0].ev_end, hud_idx_ev3);
    CHECK(a.b[a.nb - 1].ev_end == s.hud->nops, "ev_end último lote %u (esperado %u)", a.b[a.nb - 1].ev_end, s.hud->nops);
    for (uint32_t i = 1; i < a.nb; i++) CHECK(a.b[i].ev_end >= a.b[i - 1].ev_end, "ev_end monótono");

    /* percepción VR: corona = (m^2 - 1) x área del cono */
    VrRadarPerc vr = {1, 2.4f, 0.6f, 0.5f};
    vrradar_set_perception(&vr);
    VrRadarScene b;
    CHECK(vrradar_build(&s, &b) == 0, "build vr");
    CHECK(b.n_cones == 2 && b.n_cones_ext == 2 && b.n_ext_tris > 0, "ext: %u conos, %u tris", b.n_cones_ext, b.n_ext_tris);
    CHECK(b.nv == nv0 + b.n_ext_tris * 3 && b.nb == a.nb + 1, "nv/nb ext");
    /* cono 1 y 2: áreas esperadas */
    double expect = 0;
    {
        float p1[4][2] = {{-5, -22}, {-20, 4}, {10, 10}, {18, -12}}, p2[4][2] = {{-15, 15}, {-25, 20}, {-10, 25}, {-30, 28}};
        float (*pp[2])[2] = {p1, p2};
        for (int k = 0; k < 2; k++) {
            float (*p)[2] = pp[k];
            double a1 = 0.5 * fabs((p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) - (p[1][1] - p[0][1]) * (p[2][0] - p[0][0]));
            double a2 = 0.5 * fabs((p[2][0] - p[1][0]) * (p[3][1] - p[1][1]) - (p[2][1] - p[1][1]) * (p[3][0] - p[1][0]));
            expect += (2.4 * 2.4 - 1.0) * (a1 + a2);
        }
    }
    uint32_t eb = 0xFFFFFFFFu, cb = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < b.nb; i++) if (b.b[i].count == b.n_ext_tris * 3 && b.b[i].kind == 2 && b.b[i].blend == 1 && eb == 0xFFFFFFFFu) eb = i;
    CHECK(eb != 0xFFFFFFFFu, "lote ext");
    double fill = 0;
    if (eb != 0xFFFFFFFFu) {
        for (uint32_t k = 0; k + 2 < b.b[eb].count; k += 3) { double ar = tri_area(&b.v[b.b[eb].first + k]); if (ar > 0.51) fill += ar; }
        CHECK(NEAR(fill / expect, 1.0, 1e-3), "área de la corona %.2f (esperada %.2f)", fill, expect);
        /* el lote ext va antes del primer lote que contiene el primer cono (vértice O con el color principal) */
        for (uint32_t i = 0; i < b.nb; i++) {
            if (i == eb) continue;
            for (uint32_t k = 0; k < b.b[i].count; k++) {
                const VrVtx *v = &b.v[b.b[i].first + k];
                if ((v->rgba & 0xFFFFFF) == 0x808000u << 0 && cb == 0xFFFFFFFFu) cb = i;
            }
        }
        CHECK(cb != 0xFFFFFFFFu && eb < cb, "el lote ext (%u) va antes del lote del cono (%u)", eb, cb);
        /* colores: el relleno usa el color del cono; todos los vértices ext dentro del lienzo razonable y sin NaN */
        for (uint32_t k = 0; k < b.b[eb].count; k++) {
            const VrVtx *v = &b.v[b.b[eb].first + k];
            CHECK(isfinite(v->pos[0]) && isfinite(v->pos[1]) && v->pad == 0xFFFF && (v->flags & VR_F_2D) && (v->flags & GPU_F_SEMI), "vértice ext inválido");
        }
    }
    /* sin periferia: el área total de relleno es la misma */
    VrRadarPerc nop = {1, 2.4f, 1.0f, 0.5f};
    vrradar_set_perception(&nop);
    VrRadarScene c;
    CHECK(vrradar_build(&s, &c) == 0, "build sin periferia");
    CHECK(c.n_ext_tris < b.n_ext_tris, "sin periferia hay menos triángulos (%u vs %u)", c.n_ext_tris, b.n_ext_tris);
    /* ORIGINAL / range_mult <= 1: nada */
    VrRadarPerc o1 = {1, 1.0f, 0.6f, 0.5f};
    vrradar_set_perception(&o1);
    VrRadarScene d;
    CHECK(vrradar_build(&s, &d) == 0 && d.n_ext_tris == 0 && d.nv == nv0, "range_mult 1: nada extra");
    vrradar_set_perception(&off);
    CHECK(vrradar_build(&s, &d) == 0 && d.n_ext_tris == 0 && d.nv == nv0, "ORIGINAL: nada extra");
    /* radar oculto: sin marcas -> sin radar y la lista pasa tal cual */
    vrradar_reset();
    VrRadarSplit s2;
    CHECK(vrradar_split(&L, &s2) == 0 && !s2.present && s2.hud == &L && !s2.radar, "sin marcas: hud == lista");
    VrRadarScene e;
    CHECK(vrradar_build(&s2, &e) == 0 && !e.present, "build sin radar");
    /* lista nula / argumentos */
    CHECK(vrradar_split(NULL, &s2) < 0, "split NULL");
    CHECK(vrradar_build(NULL, &e) == 0 && !e.present, "build NULL");
    vrradar_set_perception(&vr);
    vrradar_set_perception(NULL);
    CHECK(vrradar_get_perception().enabled == 1, "set NULL no cambia");
}

/* estado de interferencia/alerta: sprite texturizado y sin conos -> mode 1 */
static void t_state_mode(void) {
    build_list(0);
    /* sustituye los conos por un sprite texturizado */
    for (uint32_t i = 0; i < nops; i++)
        if (ops[i].type == GPU_OP_POLY) { ops[i].type = GPU_OP_RECT; ops[i].flags = GPU_F_TEXTURED | GPU_F_SEMI; ops[i].rw = 8; ops[i].rh = 8; ops[i].v[0].x = (int16_t)(CX - 10); ops[i].v[0].y = (int16_t)(CY - 12); }
    L.nops = nops;
    VrRadarSplit s;
    VrRadarPerc vr = {1, 2.4f, 0.6f, 0.5f};
    vrradar_set_perception(&vr);
    CHECK(vrradar_split(&L, &s) == 0 && s.present, "split estado");
    VrRadarScene a;
    CHECK(vrradar_build(&s, &a) == 0 && a.mode == 1 && a.n_cones == 0 && a.n_ext_tris == 0, "modo estado sin conos");
}

/* ------------------------------------------------------------------------------------------------ nuevos (revisión adversarial) */
static int in_tri(double px, double py, const float a[2], const float b[2], const float c[2]) {
    double d1 = (px - b[0]) * (a[1] - b[1]) - (a[0] - b[0]) * (py - b[1]);
    double d2 = (px - c[0]) * (b[1] - c[1]) - (b[0] - c[0]) * (py - c[1]);
    double d3 = (px - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (py - a[1]);
    int neg = (d1 < -1e-9) || (d2 < -1e-9) || (d3 < -1e-9), pos = (d1 > 1e-9) || (d2 > 1e-9) || (d3 > 1e-9);
    return !(neg && pos);
}
/* Un solo cono de soldado: la corona queda FUERA del cono original y DENTRO del cono escalado respecto al origen. */
static void t_corona_placement(void) {
    build_list(1);
    /* el segundo cono de soldado deja de serlo (otro color): solo queda el primero */
    for (uint32_t i = idx_cone_first + 1; i < nops; i++) if (ops[i].type == GPU_OP_POLY && ops[i].v[1].r == 0xA0 && ops[i].v[1].g == 0x00) ops[i].v[1].r = 0x33;
    /* cono realista: A y B a ambos lados del eje O-F (el cono sintético de build_list tiene A y B del mismo lado) */
    {
        GpuOp *o = &ops[idx_cone_first];
        const float q[4][2] = {{6, -11}, {-20, 4}, {10, 4}, {6, 19}};
        for (int i = 0; i < 4; i++) { o->v[i].x = (int16_t)(CX + q[i][0]); o->v[i].y = (int16_t)(CY + q[i][1]); }
    }
    VrRadarSplit s;
    CHECK(vrradar_split(&L, &s) == 0 && s.present, "split corona");
    const float m = 2.4f;
    VrRadarPerc vr = {1, m, 0.6f, 0.5f};
    vrradar_set_perception(&vr);
    VrRadarScene b;
    CHECK(vrradar_build(&s, &b) == 0 && b.n_cones == 1 && b.n_cones_ext == 1, "un cono: %u / %u", b.n_cones, b.n_cones_ext);
    float A[2] = {6 + CX, -11 + CY}, O[2] = {-20 + CX, 4 + CY}, F[2] = {10 + CX, 4 + CY}, B[2] = {6 + CX, 19 + CY};
    float mA[2], mF[2], mB[2];
    for (int k = 0; k < 2; k++) { mA[k] = O[k] + (A[k] - O[k]) * m; mF[k] = O[k] + (F[k] - O[k]) * m; mB[k] = O[k] + (B[k] - O[k]) * m; }
    unsigned nfill = 0, bad_in = 0, bad_out = 0;
    for (uint32_t i = 0; i < b.nb; i++) {
        if (b.b[i].blend != 1 || b.b[i].count != b.n_ext_tris * 3) continue;
        for (uint32_t k = 0; k + 2 < b.b[i].count; k += 3) {
            const VrVtx *t = &b.v[b.b[i].first + k];
            if (tri_area(t) <= 0.51) continue;               /* los puntos del borde miden 0,5 */
            double cx = (t[0].pos[0] + t[1].pos[0] + t[2].pos[0]) / 3.0, cy = (t[0].pos[1] + t[1].pos[1] + t[2].pos[1]) / 3.0;
            nfill++;
            if (in_tri(cx, cy, A, O, F) || in_tri(cx, cy, O, F, B)) bad_in++;
            if (!(in_tri(cx, cy, mA, O, mF) || in_tri(cx, cy, O, mF, mB))) bad_out++;
        }
    }
    CHECK(nfill > 0 && bad_in == 0, "%u triángulos de corona dentro del cono original (de %u)", bad_in, nfill);
    CHECK(bad_out == 0, "%u triángulos de corona fuera del cono escalado", bad_out);
}

/* periph_near > 1: franja núcleo hasta n*L; el área total de relleno no cambia y n >= range_mult equivale a "sin periferia". */
static void t_periph_near(void) {
    build_list(1);
    VrRadarSplit s;
    CHECK(vrradar_split(&L, &s) == 0 && s.present, "split periph_near");
    VrRadarScene r0, r1, r2, r3;
    VrRadarPerc base = {1, 2.4f, 0.6f, 0.5f}, mid = {1, 2.4f, 0.6f, 1.5f}, big = {1, 2.4f, 0.6f, 2.4f}, nop = {1, 2.4f, 1.0f, 0.5f};
    double area[4] = {0, 0, 0, 0};
    unsigned nfill[4] = {0, 0, 0, 0};
    VrRadarPerc *pp[4] = {&base, &mid, &big, &nop};
    VrRadarScene *rr[4] = {&r0, &r1, &r2, &r3};
    for (int k = 0; k < 4; k++) {
        vrradar_set_perception(pp[k]);
        CHECK(vrradar_build(&s, rr[k]) == 0, "build periph_near %d", k);
        for (uint32_t i = 0; i < rr[k]->nb; i++) {
            if (rr[k]->b[i].blend != 1 || rr[k]->b[i].count != rr[k]->n_ext_tris * 3) continue;
            for (uint32_t t = 0; t + 2 < rr[k]->b[i].count; t += 3) { double ar = tri_area(&rr[k]->v[rr[k]->b[i].first + t]); if (ar > 0.51) { area[k] += ar; nfill[k]++; } }
        }
    }
    CHECK(NEAR(area[1] / area[0], 1.0, 1e-3) && NEAR(area[2] / area[0], 1.0, 1e-3) && NEAR(area[3] / area[0], 1.0, 1e-3), "área de relleno invariante: %.2f %.2f %.2f %.2f", area[0], area[1], area[2], area[3]);
    CHECK(r2.n_ext_tris == r3.n_ext_tris, "periph_near >= range_mult == sin periferia (%u vs %u)", r2.n_ext_tris, r3.n_ext_tris);
    CHECK(nfill[1] == nfill[0] + 8 && nfill[2] == nfill[3], "franja núcleo extra: %u triángulos de relleno (base %u, sin periferia %u)", nfill[1], nfill[0], nfill[3]);
    CHECK(nfill[3] == nfill[0] - 8, "sin periferia: 2 triángulos menos por lado y cono (%u vs %u)", nfill[3], nfill[0]);
}

/* range_scale (menú de alcance): la IA ve hasta L x range_scale x range_mult, y el radar dibuja eso (área de relleno = (s^2 m^2 - 1) x área del cono). */
static void t_range_scale(void) {
    build_list(1);
    VrRadarSplit s;
    CHECK(vrradar_split(&L, &s) == 0 && s.present, "split range_scale");
    s.rect[0] = -4000; s.rect[1] = -4000; s.rect[2] = 8000; s.rect[3] = 8000;   /* aquí se mide la escala pura: sin límite de ventana (ver t_range_fit) */
    VrRadarPerc p1 = {1, 2.4f, 0.6f, 0.5f, 1.0f}, p0 = {1, 2.4f, 0.6f, 0.5f, 0.0f}, p15 = {1, 2.4f, 0.6f, 0.5f, 1.5f};
    VrRadarPerc *pp[3] = {&p1, &p0, &p15};
    double area[3] = {0, 0, 0};
    for (int k = 0; k < 3; k++) {
        VrRadarScene r;
        vrradar_set_perception(pp[k]);
        CHECK(vrradar_build(&s, &r) == 0 && r.n_cones_ext == 2, "build range_scale %d", k);
        for (uint32_t i = 0; i < r.nb; i++) {
            if (r.b[i].blend != 1 || r.b[i].count != r.n_ext_tris * 3) continue;
            for (uint32_t t = 0; t + 2 < r.b[i].count; t += 3) { double ar = tri_area(&r.v[r.b[i].first + t]); if (ar > 0.51) area[k] += ar; }
        }
    }
    CHECK(NEAR(area[1] / area[0], 1.0, 1e-6), "range_scale 0 == 1: %.2f vs %.2f", area[1], area[0]);
    double want = (1.5 * 1.5 * 2.4 * 2.4 - 1.0) / (2.4 * 2.4 - 1.0);
    CHECK(NEAR(area[2] / area[0], want, 1e-3), "corona con range_scale 1,5: razón de áreas %.3f (esperada %.3f)", area[2] / area[0], want);
}

/* range_scale con el alcance ampliado fuera de la ventana (71x54 px): la corona no puede quedar fuera de lo que el radar muestra (si no, el borde de la sospecha y
 * de la visión desaparece y solo queda un relleno tenue). Se reduce el factor hasta que quepa, sin bajar nunca del de x1 ni pasar de range_scale. */
static void t_range_fit(void) {
    build_list(0);
    static const float c0[4][2] = {{3, -6}, {0, 0}, {6, 0}, {3, 6}}, c1[4][2] = {{-17, 7}, {-20, 10}, {-14, 10}, {-17, 13}};
    for (int c = 0; c < 2; c++) {
        GpuOp *op = &ops[idx_cone_first + (uint32_t)c];
        for (int i = 0; i < 4; i++) { op->v[i].x = (int16_t)((c ? c1[i][0] : c0[i][0]) + CX); op->v[i].y = (int16_t)((c ? c1[i][1] : c0[i][1]) + CY); }
    }
    VrRadarSplit s;
    CHECK(vrradar_split(&L, &s) == 0 && s.present, "split range_fit");
    const float rs[3] = {1.0f, 1.5f, 2.0f};
    double ext[3] = {0, 0, 0}, over[3] = {-1e9, -1e9, -1e9};
    for (int k = 0; k < 3; k++) {
        VrRadarScene r;
        VrRadarPerc p = {1, 2.4f, 0.6f, 0.5f, rs[k]};
        vrradar_set_perception(&p);
        CHECK(vrradar_build(&s, &r) == 0 && r.n_cones_ext == 2, "build range_fit %d", k);
        for (uint32_t i = 0; i < r.nb; i++) {
            if (r.b[i].blend != 1 || r.b[i].count != r.n_ext_tris * 3) continue;
            for (uint32_t t = 0; t < r.b[i].count; t++) {
                const VrVtx *v = &r.v[r.b[i].first + t];
                double ex = fmax(fabs(v->pos[0] - CX) - 35.0, fabs(v->pos[1] - CY) - 27.0), d = fmax(fabs(v->pos[0] - CX), fabs(v->pos[1] - CY));
                if (ex > over[k]) over[k] = ex;
                if (d > ext[k]) ext[k] = d;
            }
        }
        CHECK(k == 0 || over[k] <= 2.5, "range_scale %.1f: la corona se sale %.1f px de la ventana del radar", rs[k], over[k]);
    }
    CHECK(ext[1] >= ext[0] - 1e-6 && ext[2] >= ext[1] - 1e-6, "la corona no se encoge al subir range_scale: %.1f %.1f %.1f", ext[0], ext[1], ext[2]);
    CHECK(ext[2] > ext[0] + 4.0, "con range_scale 2 la corona es mayor que con x1: %.1f vs %.1f", ext[2], ext[0]);
}

/* La numeración de seq de la captura vuelve a 0 al reencenderla: los rangos viejos no deben marcar primitivas nuevas. */
static void t_seq_restart(void) {
    vrradar_reset();
    vrradar_on_mark(VRRADAR_MARK_BEGIN, 5000); vrradar_on_mark(VRRADAR_MARK_END, 5100);
    CHECK(vrradar_seq_is_radar(5050), "rango antes del reinicio");
    vrradar_on_mark(VRRADAR_MARK_BEGIN, 40);               /* captura reencendida: seq vuelve atrás */
    vrradar_on_mark(VRRADAR_MARK_END, 90);
    CHECK(!vrradar_seq_is_radar(5050) && vrradar_seq_is_radar(60), "rangos viejos descartados tras el reinicio de seq");
    uint32_t lo[8], hi[8];
    CHECK(vrradar_ranges(lo, hi, 8) == 1 && lo[0] == 40 && hi[0] == 90, "un solo rango");
}

/* ------------------------------------------------------------------------------------------------ marcas en la OT (HLE) */
uint8_t emu_ram[EMU_RAM_SIZE];
void hle_vrradar_reset(void);
void vrradar_hle_enable(int on);
void vrradar_hle_stats(unsigned *calls, unsigned *marked, unsigned *fail);
void hle_vrradar_draw(Cpu *c);
static uint32_t kheap = 0x1000;
uint32_t bios_kalloc(uint32_t n) { n = (n + 7) & ~7u; uint32_t a = kheap; kheap += n; memset(&emu_ram[a], 0, n); return 0x80000000u | a; }
static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, emu_ram + (a & 0x1FFFFC), 4); return v; }
static void wr32(uint32_t a, uint32_t v) { memcpy(emu_ram + (a & 0x1FFFFC), &v, 4); }
static int orig_nprim, orig_calls;
/* "función original": enlaza orig_nprim primitivas de 4 palabras de carga en la ranura a1 (como el encadenado de primitivas: al principio de la ranura) y
 * destruye a1 (la HLE debe haberlo guardado). */
int rt_orig(Cpu *c, const char *name) {
    (void)name;
    uint32_t ot = c->r[5];
    orig_calls++;
    for (int i = 0; i < orig_nprim; i++) {
        uint32_t p = 0x100000u + 32u * (uint32_t)i;
        wr32(p, (rd32(ot) & 0xFFFFFF) | (4u << 24));
        wr32(p + 4, 0x20000000u | (uint32_t)(i + 1)); wr32(p + 8, 0); wr32(p + 12, 0); wr32(p + 16, 0);
        wr32(ot, (rd32(ot) & 0xFF000000u) | p);
    }
    c->r[5] = 0xDEAD;
    return 1;
}
/* Recorrido idéntico a gpu_draw_list (runtime/gpu.c): palabras de carga en orden; -1 si no termina. */
static int walk_ot(uint32_t addr, uint32_t *out, int max) {
    int n = 0;
    addr &= 0x1FFFFC;
    for (int guard = 0; guard < 4096; guard++) {
        uint32_t h = rd32(addr);
        unsigned k = h >> 24;
        for (unsigned i = 0; i < k; i++) { if (n >= max) return -2; out[n++] = rd32(addr + 4 + 4 * i); }
        if (h & 0x800000) return n;
        addr = h & 0x1FFFFC;
    }
    return -1;
}
static void t_ot_marks(void) {
    Cpu c;
    memset(&c, 0, sizeof c);
    hle_vrradar_reset(); vrradar_hle_enable(1); vrradar_reset();
    const uint32_t ot = 0x1000A0;
    for (int round = 0; round < 11; round++) {            /* > 4: se reutilizan los pares de marcas */
        memset(emu_ram + 0x100000, 0, 0x2000);
        for (int i = 0; i < 4; i++) wr32(ot + 4 * i, i < 3 ? (ot + 4 * (i + 1)) & 0xFFFFFF : 0xFFFFFF);
        wr32(0x101000, (rd32(ot + 4) & 0xFFFFFF) | (1u << 24)); wr32(0x101004, 0x21000000u); wr32(ot + 4, 0x101000);   /* primitiva previa del HUD */
        wr32(0x102000, (rd32(ot + 8) & 0xFFFFFF) | (1u << 24)); wr32(0x102004, 0x22000000u); wr32(ot + 8, 0x102000);   /* y otra en la ranura siguiente */
        orig_nprim = 1 + round % 4;
        c.r[5] = ot + 4; c.r[4] = 0x800B0000u;
        hle_vrradar_draw(&c);
        uint32_t w[64];
        int n = walk_ot(ot, w, 64), np = orig_nprim, exp = 1 + np * 4 + 1 + 1 + 1;
        CHECK(n == exp, "ronda %d: %d palabras (esperadas %d)", round, n, exp);
        if (n != exp) continue;
        CHECK(w[0] == (0x04000000u | VRRADAR_MARK_BEGIN), "marca de inicio primera");
        for (int i = 0; i < np; i++) CHECK(w[1 + 4 * i] == (0x20000000u | (uint32_t)(np - i)), "orden de dibujo inverso al de alta");
        CHECK(w[1 + 4 * np] == (0x04000000u | VRRADAR_MARK_END), "marca de fin justo tras la primera primitiva dada de alta");
        CHECK(w[2 + 4 * np] == 0x21000000u && w[3 + 4 * np] == 0x22000000u, "primitivas ajenas conservadas y en orden");
        CHECK(c.r[5] == 0xDEAD, "registros del original intactos");
        /* las marcas delimitan EXACTAMENTE las primitivas del radar (como lo hace la GPU con el contador seq) */
        vrradar_reset();
        uint32_t seq = 0, radar_seq_lo = 0, radar_seq_hi = 0;
        for (int i = 0; i < n; ) {
            uint32_t cmd = w[i] >> 24;
            if (cmd == 0x04) { vrradar_on_mark(w[i] & 0xFFFFFF, seq); if ((w[i] & 0xFFFFFF) == VRRADAR_MARK_BEGIN) radar_seq_lo = seq; else radar_seq_hi = seq; i++; }
            else { seq++; i += cmd == 0x20 ? 4 : 1; }
        }
        CHECK(radar_seq_hi - radar_seq_lo == (uint32_t)np && radar_seq_lo == 0, "rango de seq = %d primitivas (inicio %u fin %u)", np, radar_seq_lo, radar_seq_hi);
        for (uint32_t q = 0; q < seq; q++) CHECK(vrradar_seq_is_radar(q) == (q < (uint32_t)np), "seq %u", q);
    }
    /* sin primitivas nuevas: la OT no se toca */
    for (int i = 0; i < 4; i++) wr32(ot + 4 * i, i < 3 ? (ot + 4 * (i + 1)) & 0xFFFFFF : 0xFFFFFF);
    unsigned a, b0, f, b1;
    vrradar_hle_stats(&a, &b0, &f);
    orig_nprim = 0; c.r[5] = ot + 4;
    hle_vrradar_draw(&c);
    vrradar_hle_stats(&a, &b1, &f);
    CHECK(b1 == b0 && rd32(ot + 4) == ((ot + 8) & 0xFFFFFF), "sin primitivas nuevas: sin marcas ni cambios");
    /* ranura vacía antes (H0 = terminador) y cabecera con byte de tamaño propio */
    wr32(ot + 4, 0xFFFFFF);
    orig_nprim = 2; c.r[5] = ot + 4;
    hle_vrradar_draw(&c);
    uint32_t w[64];
    CHECK(walk_ot(ot + 4, w, 64) == 1 + 8 + 1 && w[9] == (0x04000000u | VRRADAR_MARK_END), "H0 = terminador");
    /* desactivada: pasa directo */
    vrradar_hle_enable(0);
    wr32(ot + 4, 0xFFFFFF); orig_nprim = 2; c.r[5] = ot + 4;
    hle_vrradar_draw(&c);
    CHECK(walk_ot(ot + 4, w, 64) == 8, "HLE desactivada: sin marcas");
    vrradar_hle_enable(1);
    unsigned fl; vrradar_hle_stats(&a, &b1, &fl);
    CHECK(fl == 0, "sin cadenas inesperadas");
    vrradar_reset();
}

/* ------------------------------------------------------------------------------------------------ anclaje */
static float dotv(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void crossv(float o[3], const float a[3], const float b[3]) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static float lenv(const float a[3]) { return sqrtf(dotv(a, a)); }

static void t_anchor(void) {
    VrRadarWristCfg cfg = vrradar_wrist_cfg_default();
    VrRadarArm arm = {{-0.20f, 1.22f, -0.20f}, {0.10f, 1.30f, -0.38f}, {0.0f, 1.0f, 0.3f}};
    VrRadarQuad q;
    CHECK(vrradar_wrist_quad(&arm, &cfg, 71.0f / 54.0f, &q) && q.valid, "quad válido");
    float a[3] = {arm.elbow[0] - arm.wrist[0], arm.elbow[1] - arm.wrist[1], arm.elbow[2] - arm.wrist[2]};
    float al = lenv(a); a[0] /= al; a[1] /= al; a[2] /= al;
    CHECK(NEAR(lenv(q.normal), 1, 1e-5) && NEAR(lenv(q.right), 1, 1e-5) && NEAR(lenv(q.up), 1, 1e-5), "ejes unitarios");
    CHECK(NEAR(dotv(q.normal, a), 0, 1e-5) && NEAR(dotv(q.right, q.normal), 0, 1e-5) && NEAR(dotv(q.up, q.right), 0, 1e-5), "ejes ortogonales");
    float cr[3]; crossv(cr, q.right, q.up);
    CHECK(NEAR(dotv(cr, q.normal), 1, 1e-5), "mano derecha: right x up = normal");
    CHECK(NEAR(dotv(q.right, a), -1, 1e-5), "derecha de la imagen = hacia la mano");
    float d[3] = {q.center[0] - arm.wrist[0], q.center[1] - arm.wrist[1], q.center[2] - arm.wrist[2]};
    CHECK(NEAR(dotv(d, a), cfg.along_m, 1e-5) && NEAR(dotv(d, q.normal), cfg.lift_m, 1e-5), "posición: %.4f a lo largo, %.4f sobre", dotv(d, a), dotv(d, q.normal));
    CHECK(dotv(q.normal, arm.dorsal) > 0, "normal hacia el dorso");
    CHECK(q.half_w * 2 >= 0.0899 && NEAR(q.half_w * 2, cfg.width_m, 1e-6) && NEAR(q.half_w / q.half_h, 71.0 / 54.0, 1e-4), "tamaño >= 9 cm (%.4f), proporción del radar", q.half_w * 2);
    float w[3] = {q.corners[1][0] - q.corners[0][0], q.corners[1][1] - q.corners[0][1], q.corners[1][2] - q.corners[0][2]};
    CHECK(NEAR(lenv(w), cfg.width_m, 1e-5), "esquinas");
    CHECK(NEAR(q.model[0], q.right[0] * cfg.width_m, 1e-5) && NEAR(q.model[12], q.center[0], 1e-6) && q.model[15] == 1.0f, "modelo");
    /* giro de 90° */
    VrRadarWristCfg c2 = cfg; c2.rot90 = 1;
    VrRadarQuad q2;
    CHECK(vrradar_wrist_quad(&arm, &c2, 71.0f / 54.0f, &q2) && NEAR(dotv(q2.right, q.up), 1, 1e-5) && NEAR(dotv(q2.up, q.right), -1, 1e-5), "rot90");
    c2.rot90 = 4;
    CHECK(vrradar_wrist_quad(&arm, &c2, 71.0f / 54.0f, &q2) && NEAR(dotv(q2.right, q.right), 1, 1e-5), "rot90 x4 = identidad");
    /* alto explícito */
    c2 = cfg; c2.height_m = 0.06f;
    CHECK(vrradar_wrist_quad(&arm, &c2, 1.0f, &q2) && NEAR(q2.half_h, 0.03, 1e-6), "alto explícito");
    /* degenerados */
    VrRadarArm bad = arm;
    bad.elbow[0] = bad.wrist[0]; bad.elbow[1] = bad.wrist[1]; bad.elbow[2] = bad.wrist[2];
    CHECK(!vrradar_wrist_quad(&bad, &cfg, 1.3f, &q) && !q.valid, "hueso nulo");
    bad = arm; bad.dorsal[0] = a[0]; bad.dorsal[1] = a[1]; bad.dorsal[2] = a[2];
    CHECK(!vrradar_wrist_quad(&bad, &cfg, 1.3f, &q), "dorso paralelo al hueso");
    bad = arm; bad.dorsal[1] = NAN;
    CHECK(!vrradar_wrist_quad(&bad, &cfg, 1.3f, &q), "NaN");
    bad = arm; bad.wrist[2] = INFINITY;
    CHECK(!vrradar_wrist_quad(&bad, &cfg, 1.3f, &q), "inf");
    c2 = cfg; c2.width_m = 0;
    CHECK(!vrradar_wrist_quad(&arm, &c2, 1.3f, &q), "ancho 0");
    CHECK(!vrradar_wrist_quad(NULL, &cfg, 1.3f, &q) && !vrradar_wrist_quad(&arm, &cfg, 1.3f, NULL), "NULL");
    CHECK(vrradar_wrist_quad(&arm, NULL, 0.0f, &q) && q.valid, "cfg NULL y aspecto 0 usan valores por defecto");

    /* mando */
    float grip[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.1f, 1.2f, -0.3f, 1};
    VrRadarArm ca;
    CHECK(vrradar_arm_from_controller(grip, &ca) && NEAR(ca.wrist[0], 0.1, 1e-6) && NEAR(ca.elbow[2], -0.3 + 0.27, 1e-6) && NEAR(ca.dorsal[1], 1, 1e-6), "arm del mando");
    VrRadarQuad qc;
    CHECK(vrradar_wrist_quad(&ca, &cfg, 1.3f, &qc) && NEAR(qc.normal[1], 1, 1e-5) && qc.center[1] > 1.2f + cfg.lift_m - 1e-4, "quad del mando (hacia arriba)");
    /* hueso invisible 1:1 con la pose real: agarre girado (X 30 grados, luego Y 40 grados) y desplazado. Marco del agarre: -Z dedos, +Y dorso, +Z hacia el codo.
     * Hueso = origen + 0,13 m hacia el codo (+Z) + 0,04 m sobre el dorso (+Y); normal del panel = +Y (lejos de la palma). Valores literales a propósito. */
    {
        float cx = cosf(0.5235988f), sx = sinf(0.5235988f), cy = cosf(0.6981317f), sy = sinf(0.6981317f);
        float ex[3] = {1, 0, 0}, ey[3] = {0, cx, sx}, ez[3] = {0, -sx, cx}, X[3], Y[3], Z[3];
        float *src[3] = {ex, ey, ez}, *dst[3] = {X, Y, Z};
        for (int k = 0; k < 3; k++) { dst[k][0] = cy * src[k][0] + sy * src[k][2]; dst[k][1] = src[k][1]; dst[k][2] = -sy * src[k][0] + cy * src[k][2]; }
        float rg[16] = {X[0], X[1], X[2], 0, Y[0], Y[1], Y[2], 0, Z[0], Z[1], Z[2], 0, -0.25f, 1.10f, -0.40f, 1};
        VrRadarArm ra; VrRadarQuad rq; VrRadarWristCfg dc = vrradar_wrist_cfg_default();
        CHECK(NEAR(dc.along_m, 0.13, 1e-6) && NEAR(dc.lift_m, 0.065, 1e-6), "hueso: 0,13 m al codo (muñeca, antes de la mano) y 0,065 m sobre el dorso por defecto");
        CHECK(dc.lift_m >= 0.063f, "hueso: lift_m %.4f por encima del radio máximo del antebrazo del guante (55 mm + holgura)", dc.lift_m);
        CHECK(vrradar_arm_from_controller(rg, &ra) && vrradar_wrist_quad(&ra, &dc, 71.0f / 54.0f, &rq), "hueso: cuadro del agarre girado");
        for (int i = 0; i < 3; i++) {
            float want = rg[12 + i] + 0.13f * Z[i] + 0.065f * Y[i];
            CHECK(NEAR(rq.center[i], want, 1e-5), "hueso: centro[%d] %.5f esperado %.5f", i, rq.center[i], want);
            CHECK(NEAR(rq.normal[i], Y[i], 1e-5), "hueso: normal[%d] %.5f esperado %.5f", i, rq.normal[i], Y[i]);
        }
    }
    grip[5] = NAN;
    CHECK(!vrradar_arm_from_controller(grip, &ca), "mando NaN");

    /* panel fijo al dorso del antebrazo (no gira hacia los ojos): normal = dorsal ortogonalizada al eje, aunque apunte hacia abajo (caso real: normal (-0,17 -0,93 0,30)) */
    {
        VrRadarArm ea = {{0.10f, 0.89f, -0.16f + 0.27f}, {0.10f, 0.89f, -0.16f}, {-0.17f, -0.93f, 0.30f}};   /* codo = muñeca + 0,27 m hacia +Z */
        float ax[3] = {0, 0, 1};
        VrRadarQuad dq;
        VrRadarWristCfg ec = vrradar_wrist_cfg_default();
        CHECK(vrradar_wrist_quad(&ea, &ec, 71.0f / 54.0f, &dq) && dq.valid, "dorso: quad válido");
        float d[3] = {dq.center[0] - ea.wrist[0], dq.center[1] - ea.wrist[1], dq.center[2] - ea.wrist[2]};
        float dn[3] = {-0.17f, -0.93f, 0.0f}, dl = lenv(dn); dn[0] /= dl; dn[1] /= dl;
        CHECK(NEAR(lenv(dq.normal), 1, 1e-5) && NEAR(dotv(dq.normal, ax), 0, 1e-5), "dorso: normal unitaria y perpendicular al antebrazo");
        CHECK(NEAR(dotv(dq.normal, dn), 1, 1e-5) && dotv(dq.normal, (float[3]){0, 1, 0}) < 0, "dorso: la normal es el dorso ortogonalizado (apunta hacia abajo aquí)");
        CHECK(NEAR(dotv(d, ax), ec.along_m, 1e-5) && NEAR(dotv(d, dq.normal), ec.lift_m, 1e-5), "dorso: %.3f m a lo largo, %.3f m sobre el antebrazo", dotv(d, ax), dotv(d, dq.normal));
        CHECK(NEAR(ec.along_m, 0.13, 1e-6) && NEAR(ec.lift_m, 0.065, 1e-6), "dorso: valores por defecto 0,13 / 0,065");
        float cr2[3]; crossv(cr2, dq.right, dq.up);
        CHECK(NEAR(dotv(cr2, dq.normal), 1, 1e-5) && NEAR(dotv(dq.right, ax), -1, 1e-5), "dorso: ejes de mano derecha, derecha = hacia la mano");
        VrRadarQuad sq;
        CHECK(!vrradar_wrist_quad(NULL, &ec, 1.3f, &sq) && !vrradar_wrist_quad(&ea, &ec, 1.3f, NULL), "dorso: NULL");
    }

    /* mirada */
    VrRadarQuad g;
    VrRadarArm flat = {{0, 1.2f, 0.27f}, {0, 1.2f, 0}, {0, 1, 0}};   /* antebrazo horizontal hacia -Z, dorso arriba */
    CHECK(vrradar_wrist_quad(&flat, &cfg, 1.3f, &g), "quad plano");
    float eye[3] = {g.center[0], g.center[1] + 0.4f, g.center[2]}, fwd[3] = {0, -1, 0};
    CHECK(NEAR(vrradar_gaze(&g, eye, fwd, &cfg), cfg.gaze_boost, 1e-5), "mirándolo de frente: brillo máximo");
    float fwd2[3] = {0, 0, -1};
    CHECK(vrradar_gaze(&g, eye, fwd2, &cfg) == 0.0f, "mirando a otro lado: 0");
    float eye3[3] = {g.center[0], g.center[1] - 0.4f, g.center[2]}, fwd3[3] = {0, 1, 0};
    CHECK(vrradar_gaze(&g, eye3, fwd3, &cfg) == 0.0f, "quad de espaldas: 0");
    float fwd4[3] = {0.5f, -0.866f, 0};   /* 30° de desvío: cos 0,866 entre 0,75 y 0,93 */
    float gz = vrradar_gaze(&g, eye, fwd4, &cfg);
    CHECK(gz > 0 && gz < cfg.gaze_boost, "transición suave (%.3f)", gz);
    CHECK(vrradar_gaze(NULL, eye, fwd, &cfg) == 0 && vrradar_gaze(&g, NULL, fwd, &cfg) == 0, "gaze NULL");
    float eye0[3] = {g.center[0], g.center[1], g.center[2]};
    CHECK(vrradar_gaze(&g, eye0, fwd, &cfg) == 0, "ojo en el centro");
}

/* ------------------------------------------------------------------------------------------------ referencia por software del quad */
typedef struct { int w, h; unsigned char *p; } Tex;
static Tex load_ppm(const char *path) {
    Tex t = {0, 0, NULL};
    FILE *f = fopen(path, "rb");
    if (!f) return t;
    int mx;
    if (fscanf(f, "P6 %d %d %d", &t.w, &t.h, &mx) != 3 || t.w <= 0 || t.h <= 0 || t.w > 4096 || t.h > 4096) { fclose(f); t.w = t.h = 0; return t; }
    fgetc(f);
    t.p = (unsigned char *)malloc((size_t)t.w * t.h * 3);
    if (fread(t.p, 3, (size_t)t.w * t.h, f) != (size_t)t.w * t.h) { free(t.p); t.p = NULL; t.w = t.h = 0; }
    fclose(f);
    return t;
}
static Tex proc_tex(void) {
    Tex t = {284, 216, NULL};
    t.p = (unsigned char *)calloc((size_t)t.w * t.h, 3);
    for (int y = 0; y < t.h; y++) for (int x = 0; x < t.w; x++) {
        unsigned char *p = &t.p[((size_t)y * t.w + x) * 3];
        int g = (x % 16 == 0 || y % 16 == 0) ? 90 : 12;
        if (abs(x - 142) + abs(y - 108) < 4) g = 255;
        p[1] = (unsigned char)g; p[2] = (unsigned char)(g / 2);
    }
    return t;
}

static int ref_image(const char *dir, const char *texpath) {
    char path[600];
    Tex tex = {0, 0, NULL};
    if (texpath) tex = load_ppm(texpath);
    if (!tex.p) { snprintf(path, sizeof path, "%s/radar_06438_ext.ppm", dir); tex = load_ppm(path); }
    if (!tex.p) tex = proc_tex();
    VrRadarWristCfg cfg = vrradar_wrist_cfg_default();
    VrRadarArm arm = {{-0.20f, 1.22f, -0.20f}, {0.10f, 1.30f, -0.38f}, {0.0f, 1.0f, 0.35f}};
    VrRadarQuad q;
    if (!vrradar_wrist_quad(&arm, &cfg, 71.0f / 54.0f, &q)) return 1;
    /* cámara: cabeza a 1,60 m mirando hacia el antebrazo (hacia -Z y abajo) */
    float eye[3] = {0.0f, 1.60f, 0.0f}, tgt[3] = {-0.02f, 1.28f, -0.30f};
    float f[3] = {tgt[0] - eye[0], tgt[1] - eye[1], tgt[2] - eye[2]};
    float fl = lenv(f); f[0] /= fl; f[1] /= fl; f[2] /= fl;
    float wup[3] = {0, 1, 0}, r[3], u[3];
    crossv(r, f, wup); { float l = lenv(r); r[0] /= l; r[1] /= l; r[2] /= l; }
    crossv(u, r, f);
    float gz = vrradar_gaze(&q, eye, f, &cfg);
    int W = 960, H = 720;
    float tanh_ = tanf(35.0f * 3.14159265f / 180.0f);
    unsigned char *img = (unsigned char *)malloc((size_t)W * H * 3);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        float sx = ((x + 0.5f) / W * 2 - 1) * tanh_ * W / H, sy = (1 - (y + 0.5f) / H * 2) * tanh_;
        float d[3] = {f[0] + r[0] * sx + u[0] * sy, f[1] + r[1] * sx + u[1] * sy, f[2] + r[2] * sx + u[2] * sy};
        float dl = lenv(d); d[0] /= dl; d[1] /= dl; d[2] /= dl;
        float col[3] = {0.10f + 0.06f * (1 - (float)y / H), 0.11f + 0.06f * (1 - (float)y / H), 0.14f + 0.06f * (1 - (float)y / H)};   /* fondo */
        float best = 1e9f;
        /* antebrazo: cilindro de radio 3,5 cm entre codo y muñeca + manga */
        float ax[3] = {arm.wrist[0] - arm.elbow[0], arm.wrist[1] - arm.elbow[1], arm.wrist[2] - arm.elbow[2]};
        float axl = lenv(ax); ax[0] /= axl; ax[1] /= axl; ax[2] /= axl;
        float oc[3] = {eye[0] - arm.elbow[0], eye[1] - arm.elbow[1], eye[2] - arm.elbow[2]};
        float dperp[3], operp[3];
        float dd = dotv(d, ax), od = dotv(oc, ax);
        for (int i = 0; i < 3; i++) { dperp[i] = d[i] - ax[i] * dd; operp[i] = oc[i] - ax[i] * od; }
        float A = dotv(dperp, dperp), B = 2 * dotv(dperp, operp), C = dotv(operp, operp) - 0.035f * 0.035f;
        float disc = B * B - 4 * A * C;
        if (disc > 0 && A > 1e-9f) {
            float t = (-B - sqrtf(disc)) / (2 * A);
            if (t > 0) {
                float along = od + dd * t;
                if (along > 0 && along < axl) {
                    float pnt[3] = {eye[0] + d[0] * t, eye[1] + d[1] * t, eye[2] + d[2] * t};
                    float n[3] = {pnt[0] - arm.elbow[0] - ax[0] * along, pnt[1] - arm.elbow[1] - ax[1] * along, pnt[2] - arm.elbow[2] - ax[2] * along};
                    float nl = lenv(n); n[0] /= nl; n[1] /= nl; n[2] /= nl;
                    float lam = 0.25f + 0.6f * fmaxf(0, n[1] * 0.7f + n[2] * 0.3f + 0.2f);
                    col[0] = 0.30f * lam; col[1] = 0.28f * lam; col[2] = 0.22f * lam; best = t;
                }
            }
        }
        /* marco y pantalla: dos rectángulos en planos paralelos (marco algo por detrás) */
        for (int pass = 0; pass < 2; pass++) {
            float off = pass == 0 ? q.bezel_back : 0.0f;
            float pc[3] = {q.center[0] - q.normal[0] * off, q.center[1] - q.normal[1] * off, q.center[2] - q.normal[2] * off};
            float den = dotv(d, q.normal);
            if (fabsf(den) < 1e-6f) continue;
            float ppo[3] = {pc[0] - eye[0], pc[1] - eye[1], pc[2] - eye[2]};
            float t = dotv(ppo, q.normal) / den;
            if (t <= 0 || t >= best + 1e-5f) continue;
            float hit[3] = {eye[0] + d[0] * t - pc[0], eye[1] + d[1] * t - pc[1], eye[2] + d[2] * t - pc[2]};
            float lx = dotv(hit, q.right), ly = dotv(hit, q.up);
            if (pass == 0) {
                if (fabsf(lx) <= q.half_w + q.bezel && fabsf(ly) <= q.half_h + q.bezel) { col[0] = 0.02f; col[1] = 0.025f; col[2] = 0.03f; best = t; }
            } else if (fabsf(lx) <= q.half_w && fabsf(ly) <= q.half_h) {
                float uu = (lx / q.half_w * 0.5f + 0.5f), vv = (0.5f - ly / q.half_h * 0.5f);
                int tx = (int)(uu * tex.w), ty = (int)(vv * tex.h);
                if (tx >= tex.w) tx = tex.w - 1;
                if (ty >= tex.h) ty = tex.h - 1;
                const unsigned char *p = &tex.p[((size_t)ty * tex.w + tx) * 3];
                float k = 1.0f + gz;
                for (int c = 0; c < 3; c++) { float v = p[c] / 255.0f * k; col[c] = v > 1 ? 1 : v; }
                best = t;
            }
        }
        unsigned char *o = &img[((size_t)y * W + x) * 3];
        for (int c = 0; c < 3; c++) o[c] = (unsigned char)(col[c] * 255.0f + 0.5f);
    }
    snprintf(path, sizeof path, "%s/wrist_ref.ppm", dir);
    FILE *fo = fopen(path, "wb");
    if (!fo) { perror(path); return 1; }
    fprintf(fo, "P6\n%d %d\n255\n", W, H);
    fwrite(img, 3, (size_t)W * H, fo);
    fclose(fo);
    fprintf(stderr, "[ref] %s (brillo por mirada %.3f)\n", path, gz);
    free(img); free(tex.p);
    return 0;
}

/* distancia del punto p al segmento a-b */
static float seg_dist3(const float p[3], const float a[3], const float b[3]) {
    float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, ap[3] = {p[0] - a[0], p[1] - a[1], p[2] - a[2]};
    float t = dotv(ap, ab) / dotv(ab, ab); t = t < 0 ? 0 : (t > 1 ? 1 : t);
    float d[3] = {ap[0] - ab[0] * t, ap[1] - ab[1] * t, ap[2] - ab[2] * t};
    return lenv(d);
}
/* El panel va sobre el antebrazo dibujado (IK), no sobre el eje del agarre (inclinado ~35 grados respecto al antebrazo); el mando solo es reserva */
static void t_pick_arm(void) {
    VrRadarWristCfg cfg = vrradar_wrist_cfg_default();
    VrRadarArm ik = {{0.10f, 1.00f, -0.30f + 0.27f}, {0.10f, 1.00f, -0.30f}, {0.0f, 1.0f, 0.0f}};   /* antebrazo horizontal, codo a +Z */
    for (int sg = -1; sg <= 1; sg += 2) {
        float th = sg * 0.6108652f, c = cosf(th), s = sinf(th);       /* Rx(+-35 grados): el eje +Z del agarre se separa del antebrazo */
        float grip[16] = {1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, ik.wrist[0], ik.wrist[1], ik.wrist[2], 1};
        VrRadarArm pa, ca; VrRadarQuad pq, cq;
        CHECK(vrradar_pick_arm(grip, &ik, &pa) == 1 && vrradar_wrist_quad(&pa, &cfg, 71.0f / 54.0f, &pq), "pick: IK válida primero (sg=%d)", sg);
        float dp = seg_dist3(pq.center, ik.wrist, ik.elbow);
        CHECK(dp <= cfg.lift_m + 0.01f, "pick: centro a %.4f m del segmento codo-muñeca (max %.3f)", dp, cfg.lift_m + 0.01f);
        CHECK(NEAR(pq.center[0], ik.wrist[0], 1e-4) && NEAR(dotv((float[3]){pq.center[0] - ik.wrist[0], pq.center[1] - ik.wrist[1], pq.center[2] - ik.wrist[2]}, (float[3]){0, 0, 1}), cfg.along_m, 1e-4), "pick: along_m 0,13 desde la muñeca IK");
        if (sg < 0) {     /* ruta del mando sola: el panel sale del antebrazo (flota) */
            CHECK(vrradar_arm_from_controller(grip, &ca) && vrradar_wrist_quad(&ca, &cfg, 71.0f / 54.0f, &cq), "pick: quad del mando");
            float dc = seg_dist3(cq.center, ik.wrist, ik.elbow);
            CHECK(dc > cfg.lift_m + 0.01f, "pick: la ruta del mando flota a %.4f m (debe superar %.3f)", dc, cfg.lift_m + 0.01f);
        }
        /* IK nula o inválida -> mando */
        VrRadarArm zero = ik; memcpy(zero.elbow, zero.wrist, sizeof zero.elbow);
        VrRadarArm nanik = ik; nanik.dorsal[1] = NAN;
        CHECK(vrradar_pick_arm(grip, NULL, &pa) == 2 && NEAR(pa.elbow[2], ik.wrist[2] + 0.27f * c, 1e-5), "pick: IK nula -> mando");
        CHECK(vrradar_pick_arm(grip, &zero, &pa) == 2, "pick: IK de hueso nulo -> mando");
        CHECK(vrradar_pick_arm(grip, &nanik, &pa) == 2, "pick: IK con NaN -> mando");
        CHECK(vrradar_pick_arm(NULL, &ik, &pa) == 1 && NEAR(pa.elbow[2], ik.elbow[2], 1e-6), "pick: sin mando -> IK");
        grip[5] = NAN;
        CHECK(vrradar_pick_arm(grip, &nanik, &pa) == 0 && vrradar_pick_arm(NULL, NULL, &pa) == 0 && vrradar_pick_arm(grip, &ik, NULL) == 0, "pick: nada válido -> 0");
    }
}


/* ------------------------------------------------------------------------------------------------ visibilidad: solo cuando la muñeca mira a la cara */
static void eye_at(float e[3], float deg) { float r = deg * 3.14159265f / 180.0f; e[0] = sinf(r); e[1] = 0; e[2] = cosf(r); }   /* ojo a 1 m del panel, a `deg` de su normal (0,0,1) */
static void t_visibility(void) {
    const float n[3] = {0, 0, 1}, c[3] = {0, 0, 0};
    float e[3];
    eye_at(e, 0);
    CHECK(NEAR(vrradar_visibility(n, c, e, 0.0f, 0.075f), 0.5, 1e-6), "aparece en 0,15 s: a mitad de fundido 0,5");
    CHECK(NEAR(vrradar_visibility(n, c, e, 0.5f, 0.075f), 1.0, 1e-6) && NEAR(vrradar_visibility(n, c, e, 0.5f, 5.0f), 1.0, 1e-6), "llega a 1 y no pasa");
    CHECK(NEAR(vrradar_visibility(n, c, e, 0.0f, 0.15f), 1.0, 1e-6), "fundido de 0,15 s");
    CHECK(NEAR(vrradar_visibility(n, c, e, 0.3f, 0.0f), 0.3, 1e-6), "dt 0: sin cambio");
    CHECK(VRRADAR_VIS_ON_DEG == 60.0f && VRRADAR_VIS_OFF_DEG == 75.0f && VRRADAR_VIS_FADE_S == 0.15f, "umbrales: aparece < 60, desaparece > 75, fundido 0,15 s");
    eye_at(e, 59.0f); CHECK(vrradar_visibility(n, c, e, 0.0f, 1.0f) == 1.0f, "59 grados: aparece");
    eye_at(e, 61.0f); CHECK(vrradar_visibility(n, c, e, 0.0f, 1.0f) == 0.0f, "61 grados desde oculto: sigue oculto (histéresis)");
    eye_at(e, 74.0f);
    CHECK(vrradar_visibility(n, c, e, 1.0f, 1.0f) == 1.0f, "74 grados desde visible: sigue visible");
    CHECK(vrradar_visibility(n, c, e, 0.0f, 1.0f) == 0.0f, "74 grados desde oculto: sigue oculto");
    eye_at(e, 76.0f);
    CHECK(vrradar_visibility(n, c, e, 1.0f, 1.0f) == 0.0f, "76 grados: desaparece");
    CHECK(NEAR(vrradar_visibility(n, c, e, 1.0f, 0.075f), 0.5, 1e-6), "desaparece en 0,15 s");
    CHECK(NEAR(vrradar_visibility(n, c, e, 0.05f, 0.075f), 0.0, 1e-6), "no baja de 0");
    e[0] = 0; e[1] = 0; e[2] = -1; CHECK(vrradar_visibility(n, c, e, 1.0f, 1.0f) == 0.0f, "ojo detrás del panel: oculto");
    /* barrido de ángulo 0 -> 90 -> 0 a 90 Hz: aparece al cruzar 60 bajando, se mantiene hasta 75 subiendo, y no reaparece hasta bajar de 60 */
    float v = 0; int off_at_up = -1, on_at_down = -1;
    for (int i = 0; i <= 900; i++) {
        float deg = i <= 450 ? i * 0.2f : (900 - i) * 0.2f;   /* 0 -> 90 -> 0, 0,2 grados por paso */
        eye_at(e, deg);
        float pv = v; v = vrradar_visibility(n, c, e, v, 1.0f / 90.0f);
        CHECK(v >= 0.0f && v <= 1.0f && fabsf(v - pv) <= 1.0f / 90.0f / VRRADAR_VIS_FADE_S + 1e-6f, "paso de fundido acotado");
        if (i <= 450 && pv >= 0.999f && v < 0.999f && off_at_up < 0) off_at_up = i;
        if (i > 450 && pv <= 0.001f && v > 0.001f && on_at_down < 0) on_at_down = i;
    }
    CHECK(off_at_up >= 0 && off_at_up * 0.2f >= 74.9f && off_at_up * 0.2f <= 76.0f, "subiendo: empieza a apagarse pasados 75 grados (paso %d = %.1f)", off_at_up, off_at_up * 0.2f);
    CHECK(on_at_down >= 0 && (900 - on_at_down) * 0.2f <= 60.0f + 1e-3f, "bajando: reaparece al bajar de 60 grados (paso %d)", on_at_down);
    /* entradas no válidas = fuera de la zona */
    eye_at(e, 0);
    float nan3[3] = {NAN, 0, 1}, z3[3] = {0, 0, 0};
    CHECK(vrradar_visibility(nan3, c, e, 1.0f, 1.0f) == 0.0f && vrradar_visibility(z3, c, e, 1.0f, 1.0f) == 0.0f && vrradar_visibility(n, c, c, 1.0f, 1.0f) == 0.0f, "normal NaN / nula / ojo sobre el panel: oculto");
    CHECK(vrradar_visibility(NULL, c, e, 1.0f, 1.0f) == 0.0f && vrradar_visibility(n, NULL, e, 1.0f, 1.0f) == 0.0f && vrradar_visibility(n, c, NULL, 1.0f, 1.0f) == 0.0f, "NULL: oculto");
    CHECK(vrradar_visibility(n, c, e, NAN, 1.0f) == 1.0f && vrradar_visibility(n, c, e, 7.0f, 0.0f) == 1.0f && vrradar_visibility(n, c, e, -3.0f, 0.0f) == 0.0f, "prev fuera de rango / NaN se acota");
    CHECK(vrradar_visibility(n, c, e, 0.0f, NAN) == 0.0f && vrradar_visibility(n, c, e, 0.0f, -1.0f) == 0.0f, "dt no válido: sin cambio");
}

/* Dorso REAL de la mano = +Y del marco de mano = agarre girado por hand_rot_offset (roll +45 grados alrededor del eje Z local, izquierda; -45 derecha), no +Y del agarre (45 grados desviado: el radar quedaba
 * de lado y casi nunca mirando a la cara). La componente a lo largo del hueso se descarta. */
static void t_dorsal_offset(void) {
    const float I[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};       /* agarre sin girar: X derecha, Y arriba, Z hacia atrás (hacia el codo) */
    const float bone[3] = {0, 0, 1};                                  /* dirección del antebrazo (cualquier sentido): hacia atrás */
    float ql[4] = {0, 0, sinf(22.5f * 3.14159265f / 180.0f), cosf(22.5f * 3.14159265f / 180.0f)}, qr[4] = {0, 0, -ql[2], ql[3]}, o[3];
    CHECK(vrradar_dorsal_from_grip(I, ql, bone, o) && NEAR(o[0], -0.70710678, 1e-5) && NEAR(o[1], 0.70710678, 1e-5) && NEAR(o[2], 0, 1e-5), "dorso izquierda: agarre girado +45 (%.3f %.3f %.3f)", o[0], o[1], o[2]);
    CHECK(vrradar_dorsal_from_grip(I, qr, bone, o) && NEAR(o[0], 0.70710678, 1e-5) && NEAR(o[1], 0.70710678, 1e-5), "dorso derecha: agarre girado -45");
    CHECK(vrradar_dorsal_from_grip(I, NULL, bone, o) && NEAR(o[0], 0, 1e-6) && NEAR(o[1], 1, 1e-6), "sin offset (NULL): +Y del agarre");
    float qi[4] = {0, 0, 0, 1};
    CHECK(vrradar_dorsal_from_grip(I, qi, bone, o) && NEAR(o[1], 1, 1e-6), "offset identidad: +Y del agarre");
    /* ortogonal al hueso aunque éste vaya inclinado, y unitario */
    float tilt[3] = {0, 0.5f, 0.8660254f}, o2[3];
    CHECK(vrradar_dorsal_from_grip(I, ql, tilt, o2) && NEAR(lenv(o2), 1, 1e-5) && NEAR(dotv(o2, tilt), 0, 1e-5), "dorso ortogonal al hueso inclinado y unitario");
    /* agarre girado en el mundo: gira con él (Y del agarre = (1,0,0)) */
    const float G[16] = {0,1,0,0, -1,0,0,0, 0,0,1,0, 0.3f,1.1f,-0.4f,1};   /* rotación de +90 grados sobre Z: X->Y, Y->-X */
    CHECK(vrradar_dorsal_from_grip(G, qi, bone, o) && NEAR(o[0], -1, 1e-6) && NEAR(o[1], 0, 1e-6), "agarre girado: el dorso gira con él");
    /* entradas no válidas */
    float bad[16]; memcpy(bad, I, sizeof bad); bad[5] = NAN;
    float zero[3] = {0, 0, 0}, along_y[3] = {0, 1, 0};
    CHECK(!vrradar_dorsal_from_grip(bad, ql, bone, o) && !vrradar_dorsal_from_grip(NULL, ql, bone, o) && !vrradar_dorsal_from_grip(I, ql, zero, o) && !vrradar_dorsal_from_grip(I, ql, bone, NULL), "entradas no válidas: 0");
    CHECK(!vrradar_dorsal_from_grip(I, qi, along_y, o), "dorso paralelo al hueso: 0 (degenerado)");
    /* el caso del registro: agarre inclinado 45 grados hacia el lado del pulgar (su +Y ya no es el dorso), ojo 35 cm sobre la muñeca: el dorso con el offset mira más al ojo que +Y del agarre */
    float gtilt[16] = {1,0,0,0, 0.7071068f,0.7071068f,0,0, 0,0,1,0, 0,1,0,1}, d_old[3], d_new[3];
    memcpy(d_old, &gtilt[4], sizeof d_old);   /* normal antigua: Y del agarre */
    CHECK(vrradar_dorsal_from_grip(gtilt, ql, bone, d_new), "dorso del agarre inclinado");
    float eye[3] = {0.0f, 1.35f, 0.0f}, to[3] = {eye[0] - 0, eye[1] - 1.0f, eye[2] - 0}, tl = lenv(to);
    for (int i = 0; i < 3; i++) to[i] /= tl;
    CHECK(dotv(d_new, to) > dotv(d_old, to) + 0.1f, "el dorso con el offset mira más al ojo (%.3f) que +Y del agarre (%.3f)", dotv(d_new, to), dotv(d_old, to));
}

/* El panel queda fijo al dorso del antebrazo (no gira hacia el ojo) y la visibilidad mide el ángulo dorso-ojo: de espaldas a la cabeza no se ve nunca */
static void t_dorsal_visibility(void) {
    VrRadarWristCfg cfg = vrradar_wrist_cfg_default();
    VrRadarArm arm = {{0.0f, 1.0f, -0.6f}, {0.0f, 1.0f, -0.3f}, {0.0f, 1.0f, 0.0f}};   /* antebrazo horizontal hacia -Z (codo atrás), dorso arriba */
    const float eyes[4][3] = {{0.0f, 1.6f, -0.3f}, {0.0f, 0.4f, -0.45f}, {0.0f, 1.0f, 0.5f}, {0.5f, 1.0f, -0.45f}};   /* encima, DEBAJO, delante a la altura del brazo, al lado */
    const float want[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 4; i++) {
        VrRadarQuad q;
        CHECK(vrradar_wrist_quad_mesh(&arm, &cfg, 71.0f / 54.0f, NULL, 0, &q) && q.valid, "dorso %d: quad válido", i);
        CHECK(NEAR(q.normal[0], 0, 1e-5) && NEAR(q.normal[1], 1, 1e-5) && NEAR(q.normal[2], 0, 1e-5), "dorso %d: la normal es el dorso (%.3f %.3f %.3f), no gira al ojo", i, q.normal[0], q.normal[1], q.normal[2]);
        float v = vrradar_visibility(q.normal, q.center, eyes[i], 1.0f, 1.0f);
        CHECK(v == want[i], "dorso %d: visibilidad %.2f esperada %.2f", i, v, want[i]);
    }
}

/* ------------------------------------------------------------------------------------------------ el panel no atraviesa el antebrazo */
static void sub3(float o[3], const float a[3], const float b[3]) { for (int i = 0; i < 3; i++) o[i] = a[i] - b[i]; }
/* distancia de p al triángulo abc (punto más próximo, Ericson) */
static float dist_pt_tri(const float p[3], const float a[3], const float b[3], const float c[3]) {
    float ab[3], ac[3], ap[3]; sub3(ab, b, a); sub3(ac, c, a); sub3(ap, p, a);
    float d1 = dotv(ab, ap), d2 = dotv(ac, ap), q[3];
    if (d1 <= 0 && d2 <= 0) { memcpy(q, a, sizeof q); }
    else {
        float bp[3]; sub3(bp, p, b); float d3 = dotv(ab, bp), d4 = dotv(ac, bp);
        float cp[3]; sub3(cp, p, c); float d5 = dotv(ab, cp), d6 = dotv(ac, cp);
        float vc = d1 * d4 - d3 * d2, vb = d5 * d2 - d1 * d6, va = d3 * d6 - d5 * d4;
        if (d3 >= 0 && d4 <= d3) memcpy(q, b, sizeof q);
        else if (vc <= 0 && d1 >= 0 && d3 <= 0) { float t = d1 / (d1 - d3); for (int i = 0; i < 3; i++) q[i] = a[i] + t * ab[i]; }
        else if (d6 >= 0 && d5 <= d6) memcpy(q, c, sizeof q);
        else if (vb <= 0 && d2 >= 0 && d6 <= 0) { float t = d2 / (d2 - d6); for (int i = 0; i < 3; i++) q[i] = a[i] + t * ac[i]; }
        else if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) { float t = (d4 - d3) / ((d4 - d3) + (d5 - d6)); for (int i = 0; i < 3; i++) q[i] = b[i] + t * (c[i] - b[i]); }
        else { float den = 1.0f / (va + vb + vc), v = vb * den, w = vc * den; for (int i = 0; i < 3; i++) q[i] = a[i] + ab[i] * v + ac[i] * w; }
    }
    float d[3]; sub3(d, p, q);
    return lenv(d);
}
/* distancia de p a la recta (w, a unitario) */
static float dist_axis(const float p[3], const float w[3], const float a[3]) {
    float d[3]; sub3(d, p, w);
    float t = dotv(d, a), r[3] = {d[0] - a[0] * t, d[1] - a[1] * t, d[2] - a[2] * t};
    return lenv(r);
}
static void basis_perp(const float a[3], float e1[3], float e2[3]) {
    float t[3] = {fabsf(a[0]) < 0.8f ? 1.0f : 0.0f, fabsf(a[0]) < 0.8f ? 0.0f : 1.0f, 0.0f};
    float d = dotv(t, a);
    for (int i = 0; i < 3; i++) e1[i] = t[i] - a[i] * d;
    float l = lenv(e1); for (int i = 0; i < 3; i++) e1[i] /= l;
    crossv(e2, a, e1);
}
/* Para normales giradas 0..360 grados alrededor del eje: el centro del panel (y sus esquinas) quedan fuera de la malla, a >= skin_m de ella */
static void t_mesh_clearance(void) {
    VrRadarWristCfg cfg = vrradar_wrist_cfg_default();
    CHECK(NEAR(cfg.skin_m, 0.008, 1e-9), "holgura por defecto 8 mm");
    /* (a) cilindro de radio 0,055 (64 vértices por anillo, 4 anillos) y elipse 0,055 x 0,035: la elevación sigue el radio de cada dirección */
    VrRadarArm arm = {{0.30f * 0.27f + 0.10f, 1.20f + 0.20f * 0.27f, -0.30f + 0.90f * 0.27f}, {0.10f, 1.20f, -0.30f}, {0, 1, 0}};
    float a[3]; sub3(a, arm.elbow, arm.wrist); { float l = lenv(a); for (int i = 0; i < 3; i++) a[i] /= l; }
    float e1[3], e2[3]; basis_perp(a, e1, e2);
    for (int shape = 0; shape < 2; shape++) {
        const float rx = 0.055f, ry = shape ? 0.035f : 0.055f;
        float V[4 * 64 * 3];
        for (int k = 0; k < 4; k++) for (int j = 0; j < 64; j++) {
            float th = 6.2831853f * (float)j / 64.0f, st = 0.09f * (float)k;
            for (int i = 0; i < 3; i++) V[(k * 64 + j) * 3 + i] = arm.wrist[i] + a[i] * st + e1[i] * rx * cosf(th) + e2[i] * ry * sinf(th);
        }
        float lo = 1e9f, hi = 0; int ok = 1;
        for (int d = 0; d < 360; d += 5) {
            float th = (float)d * 3.14159265f / 180.0f, nn[3];
            VrRadarArm ad = arm;      /* el dorso gira 0..360 alrededor del eje: el panel sigue al dorso */
            for (int i = 0; i < 3; i++) { nn[i] = e1[i] * cosf(th) + e2[i] * sinf(th); ad.dorsal[i] = nn[i]; }
            VrRadarQuad q;
            ok &= vrradar_wrist_quad_mesh(&ad, &cfg, 71.0f / 54.0f, V, 4 * 64, &q) && q.valid && dotv(q.normal, nn) > 0.9999f;
            float dc = dist_axis(q.center, arm.wrist, a);
            if (d % 90 == 0) CHECK(NEAR(dc, (d % 180 == 0 ? rx : ry) + cfg.skin_m, 1e-5), "forma %d ángulo %d: centro a %.5f del eje, esperado radio + holgura %.5f", shape, d, dc, (d % 180 == 0 ? rx : ry) + cfg.skin_m);
            /* fuera de la superficie lisa: elipse x²/rx² + y²/ry² > 1 en el plano perpendicular */
            float rel[3]; sub3(rel, q.center, arm.wrist);
            float x = dotv(rel, e1), y = dotv(rel, e2);
            CHECK(x * x / (rx * rx) + y * y / (ry * ry) > 1.0f, "forma %d ángulo %d: centro dentro de la sección", shape, d);
            for (int k = 0; k < 4; k++) CHECK(dist_axis(q.corners[k], arm.wrist, a) >= dc - 1e-5f, "forma %d ángulo %d: esquina %d más cerca del eje que el centro", shape, d, k);
            lo = fminf(lo, dc); hi = fmaxf(hi, dc);
        }
        CHECK(ok, "forma %d: quad válido en todos los ángulos", shape);
        if (shape == 1) CHECK(hi - lo > 0.015f, "elipse: la elevación cambia con el ángulo (%.4f..%.4f)", lo, hi);
        else CHECK(hi - lo < 1e-3f, "cilindro: elevación constante (%.4f..%.4f)", lo, hi);
    }
    /* sin malla: lift_m fijo, como vrradar_wrist_quad */
    { VrRadarQuad q0, q1;
      CHECK(vrradar_wrist_quad(&arm, &cfg, 1.3f, &q0) && vrradar_wrist_quad_mesh(&arm, &cfg, 1.3f, NULL, 0, &q1) && NEAR(dist_axis(q1.center, arm.wrist, a), dist_axis(q0.center, arm.wrist, a), 1e-6) && NEAR(dist_axis(q0.center, arm.wrist, a), cfg.lift_m, 1e-5), "sin malla: lift_m");
      float junk[3] = {NAN, 0, 0}; CHECK(vrradar_wrist_quad_mesh(&arm, &cfg, 1.3f, junk, 1, &q1) && NEAR(dist_axis(q1.center, arm.wrist, a), cfg.lift_m, 1e-5), "malla no finita: lift_m");
      float tiny[3] = {arm.wrist[0], arm.wrist[1], arm.wrist[2]}; CHECK(vrradar_wrist_quad_mesh(&arm, &cfg, 1.3f, tiny, 1, &q1) && NEAR(dist_axis(q1.center, arm.wrist, a), cfg.lift_m, 1e-5), "radio < 1 cm: lift_m");
      CHECK(!vrradar_wrist_quad_mesh(NULL, &cfg, 1.3f, NULL, 0, &q1) && !vrradar_wrist_quad_mesh(&arm, &cfg, 1.3f, NULL, 0, NULL), "NULL"); }
    /* (b) malla real del antebrazo del guante (volcado d0001 de mv2, como test_glove): el panel nunca la atraviesa */
    static uint8_t RAM[0x200000]; static uint16_t VRAM[1024 * 512];
    glob_t gl; memset(&gl, 0, sizeof gl);
    const char *env = getenv("MV_DUMP");
    char rp[512], vp[512];
    if (env) { snprintf(rp, sizeof rp, "%s/ram.bin", env); snprintf(vp, sizeof vp, "%s/vram.bin", env); }
    else if (glob("/home/claude/mv2/Dumps/d0001_*/ram.bin", 0, NULL, &gl) == 0 && gl.gl_pathc > 0) { snprintf(rp, sizeof rp, "%s", gl.gl_pathv[0]); snprintf(vp, sizeof vp, "%.*svram.bin", (int)(strlen(gl.gl_pathv[0]) - 7), gl.gl_pathv[0]); }
    else { fprintf(stderr, "  (sin volcado real: malla real omitida, solo el cilindro)\n"); globfree(&gl); return; }
    globfree(&gl);
    FILE *f = fopen(rp, "rb"), *fv = fopen(vp, "rb");
    if (!f || !fv || fread(RAM, 1, sizeof RAM, f) != sizeof RAM || fread(VRAM, 1, sizeof VRAM, fv) != sizeof VRAM) { fprintf(stderr, "  (volcado ilegible: malla real omitida)\n"); if (f) fclose(f); if (fv) fclose(fv); return; }
    fclose(f); fclose(fv);
    #define R32(o) ((uint32_t)RAM[(o)] | ((uint32_t)RAM[(o) + 1] << 8) | ((uint32_t)RAM[(o) + 2] << 16) | ((uint32_t)RAM[(o) + 3] << 24))
    uint32_t ob = R32(0xACBB0), ps = R32(ob & 0x1FFFFF);
    static const int PI_[2][3] = {{7, 8, 9}, {2, 3, 4}};
    uint32_t md[2][3];
    for (int s = 0; s < 2; s++) for (int k = 0; k < 3; k++) md[s][k] = R32((ps + 0x48 + 0x5C * (uint32_t)PI_[s][k] + 0x40) & 0x1FFFFF);
    #undef R32
    VrGlove *g = vrglove_create();
    CHECK(vrglove_build(g, RAM, VRAM, md, 0) == 1, "guante real construido");
    if (!g->valid) { vrglove_destroy(g); return; }
    VrGlovePose pz; memset(&pz, 0, sizeof pz);
    for (int k = 0; k < 3; k++) pz.R[k][0] = pz.R[k][4] = pz.R[k][8] = 1;
    pz.upm = 1010.7f;
    pz.org[1][1] = -0.260f * 1000.0f / 1010.7f; pz.org[1][2] = -0.050f * 1000.0f / 1010.7f;
    pz.org[2][1] = pz.org[1][1] - 0.249f * 1000.0f / 1010.7f; pz.org[2][2] = pz.org[1][2] + 0.047f * 1000.0f / 1010.7f;
    VrGloveOut *o = calloc(1, sizeof *o);
    for (int s = 0; s < 2; s++) {
        CHECK(vrglove_pose(g, s, &pz, o) == 0 && o->nv[1] >= 8, "pose real del antebrazo (lado %d)", s);
        const VrGlovePiece *P = &g->p[s][1];
        VrRadarArm ra; memcpy(ra.elbow, pz.org[1], sizeof ra.elbow); memcpy(ra.wrist, pz.org[2], sizeof ra.wrist); ra.dorsal[0] = 1; ra.dorsal[1] = 0; ra.dorsal[2] = 0;
        float aa[3]; sub3(aa, ra.elbow, ra.wrist); { float l = lenv(aa); for (int i = 0; i < 3; i++) aa[i] /= l; }
        float b1[3], b2[3]; basis_perp(aa, b1, b2);
        float V[VRGLOVE_MAXOUT * 3];
        for (uint32_t j = 0; j < o->nv[1]; j++) memcpy(&V[j * 3], o->v[1][j].pos, 3 * sizeof(float));
        float dmin = 1e9f, rmin = 1e9f, rmax = 0;
        for (int d = 0; d < 360; d += 3) {
            float th = (float)d * 3.14159265f / 180.0f;
            for (int i = 0; i < 3; i++) ra.dorsal[i] = b1[i] * cosf(th) + b2[i] * sinf(th);   /* el dorso gira 0..360 alrededor del eje */
            VrRadarQuad q;
            CHECK(vrradar_wrist_quad_mesh(&ra, &cfg, 71.0f / 54.0f, V, o->nv[1], &q) && q.valid, "lado %d ángulo %d: quad", s, d);
            float pts[5][3]; memcpy(pts[0], q.center, sizeof pts[0]); for (int k = 0; k < 4; k++) memcpy(pts[k + 1], q.corners[k], sizeof pts[0]);
            for (int k = 0; k < 5; k++) for (uint32_t t = 0; t + 2 < P->nidx_out; t += 3) {
                float dd = dist_pt_tri(pts[k], o->v[1][P->idx[t]].pos, o->v[1][P->idx[t + 1]].pos, o->v[1][P->idx[t + 2]].pos);
                dmin = fminf(dmin, dd);
            }
            float rc = dist_axis(q.center, ra.wrist, aa); rmin = fminf(rmin, rc); rmax = fmaxf(rmax, rc);
        }
        CHECK(dmin >= cfg.skin_m - 1e-4f, "lado %d: el panel (centro y esquinas) queda a %.4f m de la malla real (mínimo %.4f)", s, dmin, cfg.skin_m);
        printf("  antebrazo real lado %d: elevación del centro %.3f..%.3f m según el ángulo, distancia mínima panel-malla %.4f m\n", s, rmin, rmax, dmin);
        (void)P;
    }
    free(o); vrglove_destroy(g);
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--ref")) return ref_image(argv[2], argc > 3 ? argv[3] : NULL);
    t_marks();
    t_split_build();
    t_state_mode();
    t_corona_placement();
    t_periph_near();
    t_range_scale();
    t_range_fit();
    t_seq_restart();
    t_ot_marks();
    t_anchor();
    t_pick_arm();
    t_visibility();
    t_dorsal_visibility();
    t_dorsal_offset();
    t_mesh_clearance();
    fprintf(stderr, "test_radar: %d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
