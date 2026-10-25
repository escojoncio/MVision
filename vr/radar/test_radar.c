/* Pruebas sintéticas de vr_radar (sin datos del juego): marcas, separación, conos ampliados, anclaje al antebrazo, mirada.
 *   test_radar                 ejecuta las pruebas
 *   test_radar --ref DIR [ppm] escribe DIR/wrist_ref.ppm: referencia por software del quad en un brazo sintético visto desde el ojo
 *                              (ppm opcional: textura del radar; por defecto DIR/radar_06438_ext.ppm o un patrón procedural) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "sys.h"
#include "vr_radar.h"

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
    CHECK(NEAR(q.half_w * 2, 0.07, 1e-6) && NEAR(q.half_w / q.half_h, 71.0 / 54.0, 1e-4), "tamaño 7 cm, proporción del radar");
    float w[3] = {q.corners[1][0] - q.corners[0][0], q.corners[1][1] - q.corners[0][1], q.corners[1][2] - q.corners[0][2]};
    CHECK(NEAR(lenv(w), 0.07, 1e-5), "esquinas");
    CHECK(NEAR(q.model[0], q.right[0] * 0.07, 1e-5) && NEAR(q.model[12], q.center[0], 1e-6) && q.model[15] == 1.0f, "modelo");
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
        CHECK(NEAR(dc.along_m, 0.13, 1e-6) && NEAR(dc.lift_m, 0.04, 1e-6), "hueso: 0,13 m al codo (muñeca, antes de la mano) y 0,04 m sobre el dorso por defecto");
        CHECK(vrradar_arm_from_controller(rg, &ra) && vrradar_wrist_quad(&ra, &dc, 71.0f / 54.0f, &rq), "hueso: cuadro del agarre girado");
        for (int i = 0; i < 3; i++) {
            float want = rg[12 + i] + 0.13f * Z[i] + 0.04f * Y[i];
            CHECK(NEAR(rq.center[i], want, 1e-5), "hueso: centro[%d] %.5f esperado %.5f", i, rq.center[i], want);
            CHECK(NEAR(rq.normal[i], Y[i], 1e-5), "hueso: normal[%d] %.5f esperado %.5f", i, rq.normal[i], Y[i]);
        }
    }
    grip[5] = NAN;
    CHECK(!vrradar_arm_from_controller(grip, &ca), "mando NaN");

    /* panel de cara a los ojos: giro alrededor del eje del antebrazo aunque el +Y del agarre apunte hacia abajo (caso real: normal (-0,17 -0,93 0,30)) */
    {
        VrRadarArm ea = {{0.10f, 0.89f, -0.16f + 0.27f}, {0.10f, 0.89f, -0.16f}, {-0.17f, -0.93f, 0.30f}};   /* codo = muñeca + 0,27 m hacia +Z */
        float eye[3] = {0.0f, 1.60f, 0.10f}, ax[3] = {0, 0, 1};
        VrRadarQuad eq, dq;
        VrRadarWristCfg ec = vrradar_wrist_cfg_default();
        CHECK(vrradar_wrist_quad(&ea, &ec, 71.0f / 54.0f, &dq) && dotv(dq.normal, (float[3]){0, 1, 0}) < 0, "sin ojo: la normal del dorso apunta hacia abajo (caso base)");
        CHECK(vrradar_wrist_quad_eye(&ea, &ec, 71.0f / 54.0f, eye, &eq) && eq.valid, "quad con ojo válido");
        float d[3] = {eq.center[0] - ea.wrist[0], eq.center[1] - ea.wrist[1], eq.center[2] - ea.wrist[2]};
        float toe[3] = {eye[0] - eq.center[0], eye[1] - eq.center[1], eye[2] - eq.center[2]};
        float tl = lenv(toe); toe[0] /= tl; toe[1] /= tl; toe[2] /= tl;
        CHECK(NEAR(lenv(eq.normal), 1, 1e-5) && NEAR(dotv(eq.normal, ax), 0, 1e-5), "ojo: normal unitaria y perpendicular al antebrazo");
        CHECK(dotv(eq.normal, toe) > 0.95f, "ojo: la normal apunta a la cabeza (%.3f)", dotv(eq.normal, toe));
        CHECK(NEAR(dotv(d, ax), ec.along_m, 1e-5) && NEAR(dotv(d, eq.normal), ec.lift_m, 1e-5), "ojo: %.3f m a lo largo, %.3f m sobre el antebrazo", dotv(d, ax), dotv(d, eq.normal));
        CHECK(NEAR(ec.along_m, 0.13, 1e-6) && NEAR(ec.lift_m, 0.04, 1e-6), "ojo: valores por defecto 0,13 / 0,04");
        float cr2[3]; crossv(cr2, eq.right, eq.up);
        CHECK(NEAR(dotv(cr2, eq.normal), 1, 1e-5) && NEAR(dotv(eq.right, ax), -1, 1e-5), "ojo: ejes de mano derecha, derecha = hacia la mano");
        CHECK(dotv(eq.normal, (float[3]){0, 1, 0}) > 0, "ojo: la cara mira hacia arriba (cabeza encima)");
        /* ojo en el eje del antebrazo (singular) o NULL: vuelve al dorso */
        float eax[3] = {0.10f, 0.89f, 2.0f};
        VrRadarQuad sq;
        CHECK(vrradar_wrist_quad_eye(&ea, &ec, 1.3f, eax, &sq) && NEAR(dotv(sq.normal, dq.normal), 1, 1e-5), "ojo sobre el eje: dorso");
        CHECK(vrradar_wrist_quad_eye(&ea, &ec, 1.3f, NULL, &sq) && NEAR(dotv(sq.normal, dq.normal), 1, 1e-5), "ojo NULL: dorso");
        float enan[3] = {NAN, 1.6f, 0};
        CHECK(vrradar_wrist_quad_eye(&ea, &ec, 1.3f, enan, &sq) && NEAR(dotv(sq.normal, dq.normal), 1, 1e-5), "ojo NaN: dorso");
        /* la normal sigue a la cabeza al moverla alrededor del antebrazo */
        float eye2[3] = {0.8f, 0.89f, 0.0f};
        CHECK(vrradar_wrist_quad_eye(&ea, &ec, 1.3f, eye2, &sq) && sq.normal[0] > 0.95f, "ojo a un lado: la normal gira hacia él");
        CHECK(!vrradar_wrist_quad_eye(NULL, &ec, 1.3f, eye, &sq) && !vrradar_wrist_quad_eye(&ea, &ec, 1.3f, eye, NULL), "ojo: NULL");
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
    const float eye[3] = {0.10f, 1.60f, -0.20f};
    VrRadarArm ik = {{0.10f, 1.00f, -0.30f + 0.27f}, {0.10f, 1.00f, -0.30f}, {0.0f, 1.0f, 0.0f}};   /* antebrazo horizontal, codo a +Z */
    for (int sg = -1; sg <= 1; sg += 2) {
        float th = sg * 0.6108652f, c = cosf(th), s = sinf(th);       /* Rx(+-35 grados): el eje +Z del agarre se separa del antebrazo */
        float grip[16] = {1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, ik.wrist[0], ik.wrist[1], ik.wrist[2], 1};
        VrRadarArm pa, ca; VrRadarQuad pq, cq;
        CHECK(vrradar_pick_arm(grip, &ik, &pa) == 1 && vrradar_wrist_quad_eye(&pa, &cfg, 71.0f / 54.0f, eye, &pq), "pick: IK válida primero (sg=%d)", sg);
        float dp = seg_dist3(pq.center, ik.wrist, ik.elbow);
        CHECK(dp <= cfg.lift_m + 0.01f, "pick: centro a %.4f m del segmento codo-muñeca (max %.3f)", dp, cfg.lift_m + 0.01f);
        CHECK(NEAR(pq.center[0], ik.wrist[0], 1e-4) && NEAR(dotv((float[3]){pq.center[0] - ik.wrist[0], pq.center[1] - ik.wrist[1], pq.center[2] - ik.wrist[2]}, (float[3]){0, 0, 1}), cfg.along_m, 1e-4), "pick: along_m 0,13 desde la muñeca IK");
        if (sg < 0) {     /* ruta del mando sola: el panel sale del antebrazo (flota) */
            CHECK(vrradar_arm_from_controller(grip, &ca) && vrradar_wrist_quad_eye(&ca, &cfg, 71.0f / 54.0f, eye, &cq), "pick: quad del mando");
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

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--ref")) return ref_image(argv[2], argc > 3 ? argv[3] : NULL);
    t_marks();
    t_split_build();
    t_state_mode();
    t_corona_placement();
    t_periph_near();
    t_seq_restart();
    t_ot_marks();
    t_anchor();
    t_pick_arm();
    fprintf(stderr, "test_radar: %d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
