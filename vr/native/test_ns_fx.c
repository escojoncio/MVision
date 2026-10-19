/* Pruebas sintéticas de ns_fx: RAM construida a mano (sin datos del juego) con proyección de referencia independiente. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ns_fx.h"
#include "ns_fx_export.h"

/* Los nombres de actor que reconoce el extractor solo existen como hash; aquí se guardan ofuscados (XOR 0x5A) para que la prueba pueda escribirlos en la RAM sintética. */
static const unsigned char ENC_SMOKE[] = {0x29, 0x37, 0x35, 0x31, 0x3F, 0x74, 0x39};
static const unsigned char ENC_SHADOW[] = {0x29, 0x32, 0x3B, 0x3E, 0x35, 0x2D, 0x74, 0x39};
static const unsigned char ENC_WTVIEW[] = {0x2D, 0x2E, 0x05, 0x2C, 0x33, 0x3F, 0x2D, 0x74, 0x39};
static const unsigned char ENC_FADEIO[] = {0x3C, 0x3B, 0x3E, 0x3F, 0x33, 0x35, 0x74, 0x39};
static const unsigned char ENC_GASEFCT[] = {0x3D, 0x3B, 0x29, 0x05, 0x3F, 0x3C, 0x39, 0x2E, 0x74, 0x39};
static char NAMEBUF[5][16];
static const char *unxor(const unsigned char *e, size_t n, int slot) { for (size_t i = 0; i < n; i++) NAMEBUF[slot][i] = (char)(e[i] ^ 0x5A); NAMEBUF[slot][n] = 0; return NAMEBUF[slot]; }
#define NAME_SMOKE unxor(ENC_SMOKE, sizeof ENC_SMOKE, 0)
#define NAME_SHADOW unxor(ENC_SHADOW, sizeof ENC_SHADOW, 1)
#define NAME_WTVIEW unxor(ENC_WTVIEW, sizeof ENC_WTVIEW, 2)
#define NAME_FADEIO unxor(ENC_FADEIO, sizeof ENC_FADEIO, 3)
#define NAME_GASEFCT unxor(ENC_GASEFCT, sizeof ENC_GASEFCT, 4)
static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define NEAR(a, b, t) (fabs((double)(a) - (double)(b)) <= (t))

static uint8_t *RAM;
static void w8(uint32_t a, int v) { RAM[a & 0x1FFFFF] = (uint8_t)v; }
static void w16(uint32_t a, int v) { w8(a, v & 255); w8(a + 1, (v >> 8) & 255); }
static void w32(uint32_t a, uint32_t v) { w16(a, (int)(v & 0xFFFF)); w16(a + 2, (int)(v >> 16)); }
static void wstr(uint32_t a, const char *s) { do w8(a++, *s); while (*s++); }

/* ---- cámara de referencia: ojo E, giro yaw; vista: x der, y abajo, z adelante; mundo: Y arriba ---- */
static double G_E[3] = {1000, 500, -3000}, G_YAW = 0.5236, G_H = 320, G_YS = 66.0 / 64.0;
static double G_R[3][3];
static void cam_setup(void) {
    double c = cos(G_YAW), s = sin(G_YAW);
    /* mundo -> vista: v = D * Ry(w - E), D = diag(1,-1,1) */
    double Ry[3][3] = {{c, 0, -s}, {0, 1, 0}, {s, 0, c}};
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) G_R[i][j] = (i == 1 ? -1.0 : 1.0) * Ry[i][j];
    uint32_t ch = NSFX_A_CHANL;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) w16(ch + 0x10 + 2 * (3 * i + j), (int)lround(G_R[i][j] * 4096.0));
    for (int i = 0; i < 3; i++) { double t = -(G_R[i][0] * G_E[0] + G_R[i][1] * G_E[1] + G_R[i][2] * G_E[2]); w32(ch + 0x10 + 20 + 4 * i, (uint32_t)(int32_t)lround(t)); }
    w16(ch + 0x50, (int)G_H);
}
static void ref_view(const double w[3], double v[3]) {
    for (int i = 0; i < 3; i++) v[i] = G_R[i][0] * (w[0] - G_E[0]) + G_R[i][1] * (w[1] - G_E[1]) + G_R[i][2] * (w[2] - G_E[2]);
    v[1] *= G_YS;
}
static int ref_proj(const double w[3], double *sx, double *sy, double *z) {
    double v[3]; ref_view(w, v); *z = v[2]; *sx = *sy = 0;
    if (v[2] < 1) return 0;
    *sx = G_H * v[0] / v[2]; *sy = G_H * v[1] / v[2]; return 1;
}

/* ---- primitivas ---- */
static uint32_t HEAP;
static uint32_t alloc(uint32_t n) { uint32_t a = HEAP; HEAP += (n + 15) & ~15u; return a; }
static uint32_t QUEUE, NQ;
static void queue_add(uint32_t p) { w32(QUEUE + 4 * NQ, p); NQ++; }
static void queue_commit(uint32_t nobj) { uint32_t ch = NSFX_A_CHANL; w16(ch + 0x52, (int)NQ); w16(ch + 0x54, (int)nobj); w16(ch + 0x56, (int)nobj); w32(ch + 0x58, QUEUE); }

static const int PSZ[24] = {16, 24, 28, 20, 32, 40, 20, 16, 16, 16, 12, 12, 12, 20, 24, 28, 36, 32, 40, 40, 52, 40, 52, 12};
static const int VERTS[24] = {2, 3, 4, 2, 3, 4, 1, 1, 1, 1, 1, 1, 1, 3, 4, 3, 4, 3, 4, 3, 4, 2, 2, 1};
static const int VSTEP[24] = {4, 4, 4, 8, 8, 8, 0, 0, 0, 0, 0, 0, 0, 4, 4, 8, 8, 8, 8, 12, 12, 8, 12, 0};
typedef struct { uint32_t addr, pos, rect, pk[2]; int n, kind, type; } TPrim;
/* raíz = identidad salvo `scale` y traslación t */
static TPrim mkprim(int kind, int flags, int n, int scale4096, const int t[3], int group) {
    TPrim p; memset(&p, 0, sizeof p);
    p.kind = kind; p.n = n; p.type = kind | flags;
    p.addr = alloc(0x54); p.pos = alloc(8 * 4 * (uint32_t)n); p.rect = alloc(8);
    p.pk[0] = alloc((uint32_t)(PSZ[kind] * n)); p.pk[1] = alloc((uint32_t)(PSZ[kind] * n));
    uint32_t a = p.addr;
    w16(a + 0, scale4096); w16(a + 8, scale4096); w16(a + 16, scale4096);
    for (int i = 0; i < 3; i++) w32(a + 20 + 4 * i, (uint32_t)t[i]);
    w32(a + 0x20, 0); w32(a + 0x24, (uint32_t)p.type); w16(a + 0x28, group); w16(a + 0x2A, n); w16(a + 0x2E, 100);
    w16(a + 0x30, PSZ[kind]); w16(a + 0x32, VERTS[kind]); w16(a + 0x34, 8); w16(a + 0x36, VSTEP[kind]);
    w32(a + 0x38, p.pos); w32(a + 0x3C, p.rect); w32(a + 0x40, p.pk[0]); w32(a + 0x44, p.pk[1]);
    queue_add(a);
    return p;
}
static void put_pos(const TPrim *p, int i, int x, int y, int z) { w16(p->pos + 8 * (uint32_t)i, x); w16(p->pos + 8 * (uint32_t)i + 2, y); w16(p->pos + 8 * (uint32_t)i + 4, z); }
static void put_rect(const TPrim *p, int x, int y, int w, int h) { w16(p->rect, x); w16(p->rect + 2, y); w16(p->rect + 4, w); w16(p->rect + 6, h); }
static void pk_xy(uint32_t b, int kind, int v, int x, int y) { w16(b + 8 + (uint32_t)(VSTEP[kind] * v), x); w16(b + 8 + (uint32_t)(VSTEP[kind] * v) + 2, y); }
static void pk_uv(uint32_t b, int kind, int v, int u, int vv) { w8(b + 12 + (uint32_t)(VSTEP[kind] * v), u); w8(b + 13 + (uint32_t)(VSTEP[kind] * v), vv); }

static NsFxScene *run(int buf, int *ret) {
    NsFxScene *s = ns_fx_scene_create(0); NsFxOpts o = ns_fx_opts_default(); o.buffer = buf;
    *ret = ns_fx_extract(RAM, &o, s);
    return s;
}
static void reset(void) {
    memset(RAM, 0, NSFX_RAM_SIZE); HEAP = 0x80100000u; QUEUE = alloc(4096); NQ = 0;
    cam_setup(); w32(NSFX_A_MAP, 0x1); w32(NSFX_A_CLOCK, 0);
}

/* nodo de actor en la lista `lv` (cabeza + 4 = primero) */
static uint32_t LASTNODE[9];
static uint32_t actor_add(int lv, const char *file, uint32_t size) {
    uint32_t base = NSFX_A_ACTOR_LISTS + 68u * (uint32_t)lv, endn = base + 32u;
    uint32_t node = alloc(size), name = alloc(32);
    wstr(name, file); w32(node + 4, endn); w32(node + 8, 0x80012345u); w32(node + 20, name);
    if (!LASTNODE[lv]) w32(base + 4, node); else w32(LASTNODE[lv] + 4, node);
    LASTNODE[lv] = node;
    return node;
}

static void test_polygon_and_billboard(void) {
    reset();
    int t0[3] = {0, 0, 0};
    /* FT4 de mundo (vertices) con paquete bien proyectado en el búfer 0 */
    TPrim a = mkprim(18, 0, 1, 4096, t0, 0);
    int P[4][3] = {{500, 800, 2000}, {1500, 800, 2000}, {500, 100, 2600}, {1500, 100, 2600}};
    double sz[4];
    for (int v = 0; v < 4; v++) put_pos(&a, v, P[v][0], P[v][1], P[v][2]);
    for (int b = 0; b < 2; b++) {
        uint32_t pk = a.pk[b]; w16(pk, 1234); w8(pk + 7, 0x2E);
        for (int v = 0; v < 4; v++) { double w[3] = {P[v][0], P[v][1], P[v][2]}, sx, sy; ref_proj(w, &sx, &sy, &sz[v]); pk_xy(pk, 18, v, (int)lround(sx), (int)lround(sy)); pk_uv(pk, 18, v, 10 * v, 5 * v); }
        w16(pk + 14, 0x7C10); w16(pk + 22, 0x2A | (1 << 5) | (1 << 7)); /* tpage: abr 1, tp 1 */
    }
    /* billboard (RECTANGLE, FT4) con rect centrado 600x400 */
    TPrim bb = mkprim(18, 0x400, 1, 4096, t0, 0);
    double cw[3] = {1200, 700, 1500}; put_pos(&bb, 0, 1200, 700, 1500); put_rect(&bb, 300, 200, 600, 400);
    {
        double sx, sy, z; ref_proj(cw, &sx, &sy, &z);
        for (int b = 0; b < 2; b++) {
            uint32_t pk = bb.pk[b]; w16(pk, 77); w8(pk + 7, 0x2E);
            int l = (int)lround(sx - 300 * G_H / z), r = (int)lround(sx + 300 * G_H / z), tp = (int)lround(sy - 200 * G_H / z), bt = (int)lround(sy + 200 * G_H / z);
            pk_xy(pk, 18, 0, l, tp); pk_xy(pk, 18, 1, r, tp); pk_xy(pk, 18, 2, l, bt); pk_xy(pk, 18, 3, r, bt);
            for (int v = 0; v < 4; v++) pk_uv(pk, 18, v, (v & 1) ? 63 : 0, (v & 2) ? 63 : 0);
            w16(pk + 22, 0x1A | (3 << 5));
        }
    }
    /* sprite con tamaño en píxeles (SPRT, RECTANGLE, rect 8,8 -> esquina = proyección - 8) */
    TPrim sp = mkprim(6, 0x400, 1, 4096, t0, 0);
    put_pos(&sp, 0, 900, 300, 1200); put_rect(&sp, 8, 8, 16, 16);
    {
        double w[3] = {900, 300, 1200}, sx, sy, z; ref_proj(w, &sx, &sy, &z);
        for (int b = 0; b < 2; b++) { uint32_t pk = sp.pk[b]; w16(pk, 5); w8(pk + 7, 0x66); w16(pk + 8, (int)lround(sx) - 8); w16(pk + 10, (int)lround(sy) - 8); w8(pk + 12, 16); w8(pk + 13, 32); w16(pk + 14, 0x1234); w16(pk + 16, 16); w16(pk + 18, 16); }
    }
    queue_commit(0);
    int r; NsFxScene *s = run(-1, &r);
    CHECK(r == 0 && s->cam.valid, "extracción r=%d valid=%d", r, s->cam.valid);
    CHECK(s->n_items == 3, "n_items=%u", s->n_items);
    CHECK(s->cam.det_neg == 1, "mundo zurdo por el espejo en Y de vista");
    CHECK(s->buffer == 0, "búfer %d", s->buffer);
    CHECK(s->stats.chk_n >= 6 && s->stats.chk_bad == 0 && s->stats.chk_max_px < 1.5f, "reproyección n=%u mal=%u max=%.2f", s->stats.chk_n, s->stats.chk_bad, s->stats.chk_max_px);
    const NsFxItem *q = &s->items[0];
    CHECK(q->shape == NSFX_SH_QUAD && q->nv == 4 && (q->flags & NSFX_F_TEXTURED) && (q->flags & NSFX_F_SEMI), "quad flags=0x%X", q->flags);
    CHECK(q->tpage == (0x2A | 32 | 128) && q->abr == 1 && q->tp == 1 && q->clut == 0x7C10, "tpage=0x%X abr=%u tp=%u clut=0x%X", q->tpage, q->abr, q->tp, q->clut);
    for (int v = 0; v < 4; v++) CHECK(NEAR(q->pos[v][0], P[v][0], 0.01) && NEAR(q->pos[v][1], P[v][1], 0.01) && NEAR(q->pos[v][2], P[v][2], 0.01), "pos mundo vértice %d", v);
    CHECK(q->uv[3][0] == 30 && q->uv[3][1] == 15, "uv");
    const NsFxItem *b = &s->items[1];
    CHECK(b->shape == NSFX_SH_BILLBOARD, "billboard");
    CHECK(NEAR(b->off[0], -300, 0.01) && NEAR(b->off[2], 300, 0.01) && NEAR(b->off[1], -200 / G_YS, 0.01) && NEAR(b->off[3], 200 / G_YS, 0.01), "off %f %f %f %f", b->off[0], b->off[1], b->off[2], b->off[3]);
    CHECK(b->uv[1][0] == 63 && b->uv[2][1] == 63, "uv billboard");
    CHECK(NEAR(b->pos[0][0], 1200, 0.1) && NEAR(b->pos[0][1], 700, 0.1) && NEAR(b->pos[0][2], 1500, 0.1), "centro mundo del billboard");
    /* esquinas VR por ojo orientadas a la cámara del motor: proyectadas con la cámara del motor deben coincidir con el paquete */
    {
        NsFxXform x = ns_fx_xform_default(); x.mirror_z = 0; x.scale = 1.0f;   /* sin escala ni espejo: VR = mundo del motor */
        float right[3] = {s->cam.axes[0][0], s->cam.axes[1][0], s->cam.axes[2][0]}, down[3] = {s->cam.axes[0][1], s->cam.axes[1][1], s->cam.axes[2][1]}, up[3] = {-down[0], -down[1], -down[2]};
        float cr[4][3]; ns_fx_billboard_corners(b, &x, &s->cam, right, up, cr);
        double sx0, sy0, z0; double w0[3] = {cr[0][0], cr[0][1], cr[0][2]}; ref_proj(w0, &sx0, &sy0, &z0);
        double sx3, sy3, z3; double w3[3] = {cr[3][0], cr[3][1], cr[3][2]}; ref_proj(w3, &sx3, &sy3, &z3);
        uint32_t pk = bb.pk[0];
        int px0 = (int16_t)(RAM[(pk & 0x1FFFFF) + 8] | (RAM[(pk & 0x1FFFFF) + 9] << 8)), py0 = (int16_t)(RAM[(pk & 0x1FFFFF) + 10] | (RAM[(pk & 0x1FFFFF) + 11] << 8));
        int px3 = (int16_t)(RAM[(pk & 0x1FFFFF) + 32] | (RAM[(pk & 0x1FFFFF) + 33] << 8)), py3 = (int16_t)(RAM[(pk & 0x1FFFFF) + 34] | (RAM[(pk & 0x1FFFFF) + 35] << 8));
        CHECK(NEAR(sx0, px0, 1.5) && NEAR(sy0, py0, 1.5) && NEAR(sx3, px3, 1.5) && NEAR(sy3, py3, 1.5), "esquinas VR re-proyectadas (%.1f,%.1f)/(%d,%d) (%.1f,%.1f)/(%d,%d)", sx0, sy0, px0, py0, sx3, sy3, px3, py3);
    }
    const NsFxItem *c = &s->items[2];
    CHECK(c->shape == NSFX_SH_BILLBOARD && (c->flags & NSFX_F_PXSIZE) && (c->flags & NSFX_F_TPAGE_UNKNOWN), "sprite px flags=0x%X", c->flags);
    {
        double w[3] = {900, 300, 1200}, v[3]; ref_view(w, v);
        CHECK(NEAR(c->off[0], -8 * v[2] / G_H, 0.5) && NEAR(c->off[2], 8 * v[2] / G_H, 0.5), "tamaño en píxeles -> mundo (%.2f)", c->off[2]);
    }
    CHECK(c->uv[0][0] == 16 && c->uv[0][1] == 32 && c->uv[3][0] == 31 && c->uv[3][1] == 47 && c->clut == 0x1234, "uv/clut sprite");
    {   /* adaptador al contrato */
        NsEffect ef[8]; int trunc = 9;
        uint32_t ne = ns_fx_export(s, 1, ef, 8, &trunc);
        CHECK(ne == 3 && trunc == 0, "export n=%u", ne);
        CHECK(ef[0].kind == NS_EK_QUAD && ef[0].nv == 4 && (ef[0].flags & NS_EF_TEXTURED) && ef[0].tpage == q->tpage && NEAR(ef[0].p[2][2], 2600, 0.01), "export quad");
        CHECK(ef[1].kind == NS_EK_BILLBOARD && NEAR(ef[1].ref_h, 320, 1e-6) && NEAR(ef[1].box[0], -300 * 320.0 / b->vpos[0][2], 0.01) && NEAR(ef[1].box[3], 200 * 320.0 / b->vpos[0][2], 0.01), "export billboard box (%.2f %.2f)", ef[1].box[0], ef[1].box[3]);
        CHECK(NEAR(ef[2].box[0], -8, 0.01) && NEAR(ef[2].box[2], 8, 0.01) && NEAR(ef[2].box[1], -8, 0.05) && NEAR(ef[2].box[3], 8, 0.05), "export sprite en px (%.2f %.2f %.2f %.2f)", ef[2].box[0], ef[2].box[1], ef[2].box[2], ef[2].box[3]);
        ne = ns_fx_export(s, 1, ef, 2, &trunc); CHECK(ne == 2 && trunc == 1, "export truncado");
    }
    ns_fx_scene_destroy(s);

    /* búfer: solo el 1 trae paquetes buenos, el reloj apunta al 0 -> se elige el 1 */
    memset(RAM + (a.pk[0] & 0x1FFFFF), 0, 40); memset(RAM + (bb.pk[0] & 0x1FFFFF), 0, 40); memset(RAM + (sp.pk[0] & 0x1FFFFF), 0, 20);
    w16(a.pk[0], 99); /* paquete con basura */
    pk_xy(a.pk[0], 18, 0, 1, 1);
    s = run(-1, &r); CHECK(s->buffer == 1, "selección de búfer por reproyección: %d", s->buffer);
    ns_fx_scene_destroy(s);
    w32(NSFX_A_CLOCK, 1); s = run(-1, &r); CHECK(s->buffer == 1 && s->stats.chk_bad == 0, "búfer del reloj"); ns_fx_scene_destroy(s);
}

static void test_filters_and_oneside(void) {
    reset();
    int t0[3] = {0, 0, 0};
    TPrim a = mkprim(18, 0x1000, 2, 4096, t0, 0);   /* ONESIDE: dos quads con arrollamiento opuesto */
    int Q[2][4][3] = {{{0, 0, 2000}, {1000, 0, 2000}, {0, 1000, 2000}, {1000, 1000, 2000}}, {{0, 0, 2000}, {0, 1000, 2000}, {1000, 0, 2000}, {1000, 1000, 2000}}};
    for (int j = 0; j < 2; j++) {
        for (int v = 0; v < 4; v++) put_pos(&a, j * 4 + v, Q[j][v][0], Q[j][v][1], Q[j][v][2]);
        for (int b = 0; b < 2; b++) {
            uint32_t pk = a.pk[b] + 40u * (uint32_t)j; double sx[4], sy[4], z[4]; int cull;
            for (int v = 0; v < 4; v++) { double w[3] = {Q[j][v][0], Q[j][v][1], Q[j][v][2]}; ref_proj(w, &sx[v], &sy[v], &z[v]); }
            cull = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]) <= 0;   /* NormalClip <= 0 -> el motor lo descarta */
            w16(pk, cull ? 0 : 500); w8(pk + 7, 0x2C);
            for (int v = 0; v < 4; v++) { pk_xy(pk, 18, v, (int)lround(sx[v]), (int)lround(sy[v])); }
        }
    }
    /* invisible, otro mapa, solo orden */
    TPrim inv = mkprim(14, 0x100, 1, 4096, t0, 0); (void)inv;
    TPrim mp = mkprim(14, 0, 1, 4096, t0, 0x2); (void)mp;
    TPrim so = mkprim(14, 0x800, 1, 4096, t0, 0); (void)so;
    queue_commit(0);
    int r; NsFxScene *s = run(0, &r);
    CHECK(r == 0 && s->stats.n_invisible == 1 && s->stats.n_wrong_map == 1 && s->stats.n_sortonly == 1, "filtros inv=%u mapa=%u orden=%u", s->stats.n_invisible, s->stats.n_wrong_map, s->stats.n_sortonly);
    CHECK(s->n_items == 2, "n_items=%u", s->n_items);
    float eye[3] = {s->cam.eye[0], s->cam.eye[1], s->cam.eye[2]};
    CHECK(NEAR(eye[0], G_E[0], 0.6) && NEAR(eye[1], G_E[1], 0.6) && NEAR(eye[2], G_E[2], 0.6), "ojo del motor (%.1f,%.1f,%.1f)", eye[0], eye[1], eye[2]);
    for (int j = 0; j < 2; j++) {
        uint16_t szj = (uint16_t)(RAM[(a.pk[0] & 0x1FFFFF) + 40 * j] | (RAM[(a.pk[0] & 0x1FFFFF) + 40 * j + 1] << 8));
        CHECK(s->items[j].shape == NSFX_SH_QUAD && (s->items[j].flags & NSFX_F_ONESIDE), "una cara");
        CHECK(ns_fx_item_faces(&s->items[j], eye) == (szj != 0), "cara %d: motor %s, ns_fx %d", j, szj ? "dibuja" : "descarta", ns_fx_item_faces(&s->items[j], eye));
        CHECK(((s->items[j].flags & NSFX_F_ENGINE_CULLED) != 0) == (szj == 0), "marca de descarte del motor");
    }
    CHECK(ns_fx_item_faces(&s->items[0], eye) != ns_fx_item_faces(&s->items[1], eye), "caras opuestas");
    /* desde detrás del plano el resultado se invierte */
    float behind[3] = {eye[0], eye[1], eye[2] + 8000};
    CHECK(ns_fx_item_faces(&s->items[0], behind) != ns_fx_item_faces(&s->items[0], eye), "cara trasera visible desde el otro lado");
    ns_fx_scene_destroy(s);
}

static void test_lines_and_oncamera(void) {
    reset();
    int t0[3] = {0, 0, 0};
    TPrim l = mkprim(22, 0, 1, 4096, t0, 0);   /* LINE_GT2 láser */
    put_pos(&l, 0, 0, 0, 1000); put_pos(&l, 1, 800, 400, 5000);
    for (int b = 0; b < 2; b++) {
        double w0[3] = {0, 0, 1000}, w1[3] = {800, 400, 5000}, sx0, sy0, z0, sx1, sy1, z1; ref_proj(w0, &sx0, &sy0, &z0); ref_proj(w1, &sx1, &sy1, &z1);
        uint32_t pk = l.pk[b]; w16(pk, 10); w8(pk + 7, 0x3E);
        w16(pk + 8, (int)lround(sx0)); w16(pk + 10, (int)lround(sy0)); w16(pk + 20, (int)lround(sx1)); w16(pk + 22, (int)lround(sy1));
    }
    TPrim pl = mkprim(2, 0, 1, 4096, t0, 0);   /* LINE_F4: 3 segmentos */
    int PL[4][3] = {{0, 0, 3000}, {300, 0, 3000}, {300, 300, 3000}, {600, 300, 3000}};
    for (int v = 0; v < 4; v++) put_pos(&pl, v, PL[v][0], PL[v][1], PL[v][2]);
    for (int b = 0; b < 2; b++) { w16(pl.pk[b], 5); w8(pl.pk[b] + 7, 0x42); for (int v = 0; v < 4; v++) { double w[3] = {PL[v][0], PL[v][1], PL[v][2]}, sx, sy, z; ref_proj(w, &sx, &sy, &z); pk_xy(pl.pk[b], 2, v, (int)lround(sx), (int)lround(sy)); } }
    /* ON_CAMERA: posición en espacio de vista (sin factor vertical), billboard a 2000 de profundidad */
    TPrim oc = mkprim(18, 0x400 | 0x200, 1, 4096, t0, 0);
    put_pos(&oc, 0, 100, 50, 2000); put_rect(&oc, 20, 20, 40, 40);
    for (int b = 0; b < 2; b++) { uint32_t pk = oc.pk[b]; w16(pk, 2000); int cx = (int)lround(320.0 * 100 / 2000), cy = (int)lround(320.0 * 50 / 2000), hh = (int)lround(320.0 * 20 / 2000);
        pk_xy(pk, 18, 0, cx - hh, cy - hh); pk_xy(pk, 18, 1, cx + hh, cy - hh); pk_xy(pk, 18, 2, cx - hh, cy + hh); pk_xy(pk, 18, 3, cx + hh, cy + hh); }
    queue_commit(0);
    int r; NsFxScene *s = run(0, &r);
    CHECK(r == 0 && s->n_items == 5, "n_items=%u (láser 1 + polilínea 3 + vista 1)", s->n_items);
    CHECK(s->items[0].shape == NSFX_SH_RIBBON && s->items[0].role == NSFX_ROLE_LASER, "láser -> cinta");
    {
        double w0[3] = {0, 0, 1000}, v0[3]; ref_view(w0, v0);
        CHECK(NEAR(s->items[0].width[0], 1.5 * v0[2] / G_H, 0.05), "anchura 1,5 px en el mundo: %.3f", s->items[0].width[0]);
    }
    CHECK(s->items[1].shape == NSFX_SH_RIBBON && s->items[1].sub == 0 && s->items[3].sub == 2, "segmentos de la polilínea");
    CHECK(NEAR(s->items[2].pos[0][0], 300, 0.01) && NEAR(s->items[2].pos[1][1], 300, 0.01), "segmento 1 de la polilínea");
    {   /* la cinta mira al ojo: el lado es perpendicular a dirección y a la línea de visión */
        NsFxXform x = ns_fx_xform_default(); float eyev[3]; ns_fx_to_vr(&x, &s->cam, s->cam.eye, eyev);
        float co[4][3];
        CHECK(ns_fx_ribbon_corners(&s->items[0], &x, &s->cam, eyev, co) == 1, "cinta válida");
        float a[3], b[3]; ns_fx_to_vr(&x, &s->cam, s->items[0].pos[0], a); ns_fx_to_vr(&x, &s->cam, s->items[0].pos[1], b);
        float side[3] = {co[1][0] - co[0][0], co[1][1] - co[0][1], co[1][2] - co[0][2]}, dir[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, vw[3] = {eyev[0] - a[0], eyev[1] - a[1], eyev[2] - a[2]};
        float d1 = side[0] * dir[0] + side[1] * dir[1] + side[2] * dir[2], d2 = side[0] * vw[0] + side[1] * vw[1] + side[2] * vw[2];
        CHECK(fabsf(d1) < 1e-6f && fabsf(d2) < 1e-6f, "lado perpendicular %g %g", d1, d2);
        float sl = sqrtf(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
        CHECK(NEAR(sl, s->items[0].width[0] * x.scale, 1e-6), "anchura del lado");
        float ce[3] = {co[0][0], co[0][1], co[0][2]}; (void)ce;
    }
    const NsFxItem *c = &s->items[4];
    CHECK(c->shape == NSFX_SH_BILLBOARD && (c->flags & NSFX_F_VIEWLOCKED), "pegado a la cámara");
    /* el centro en mundo, visto con la cámara, está en vista (100, 50/ys, 2000)*/
    {
        double w[3] = {c->pos[0][0], c->pos[0][1], c->pos[0][2]}, v[3]; ref_view(w, v);
        CHECK(NEAR(v[0], 100, 0.5) && NEAR(v[1], 50, 0.5) && NEAR(v[2], 2000, 0.5), "vista (%.2f,%.2f,%.2f)", v[0], v[1], v[2]);
    }
    CHECK(s->stats.chk_bad == 0, "reproyección láser/polilínea/vista: mal=%u de %u (max %.2f)", s->stats.chk_bad, s->stats.chk_n, s->stats.chk_max_px);
    ns_fx_scene_destroy(s);
}

static void test_actors_and_screen(void) {
    reset();
    memset(LASTNODE, 0, sizeof LASTNODE);
    int t0[3] = {0, 0, 0};
    /* primitiva dueña de papel: actor NAME_SMOKE con puntero a la primitiva */
    TPrim a = mkprim(18, 0x400, 1, 4096, t0, 0);
    put_pos(&a, 0, 100, 100, 3000); put_rect(&a, 50, 50, 100, 100);
    for (int b = 0; b < 2; b++) { w16(a.pk[b], 9); w8(a.pk[b] + 7, 0x2E); }
    uint32_t sm = actor_add(3, NAME_SMOKE, 0x80); w32(sm + 0x30, a.addr);
    /* sombra de personaje */
    uint32_t sh = actor_add(4, NAME_SHADOW, 0x100);
    uint32_t objs = alloc(0x48 + 0x5C + 64), pk0 = alloc(52), pk1 = alloc(52);
    w32(sh + 0x28, objs); w32(sh + 0x90, 1);
    w16(objs + 0, 4096); w16(objs + 8, 4096); w16(objs + 16, 4096); w32(objs + 20, 2000); w32(objs + 24, 0); w32(objs + 28, 5000);
    w32(objs + 0x28, 0); w16(objs + 0x2C, 1); w16(objs + 0x2E, 1);
    w16(objs + 0x48, 1024); w16(objs + 0x48 + 16, 512);   /* escala x 1024 -> media 1000, z 512 -> media 500 */
    w16(objs + 0x48 + 0x52, 1); w32(objs + 0x48 + 0x54, pk0); w32(objs + 0x48 + 0x58, pk1); w16(objs + 0x48 + 0x50, 150);
    double CW[4][3] = {{3000, 0, 5500}, {1000, 0, 5500}, {3000, 0, 4500}, {1000, 0, 4500}};   /* orden de paquete: (+,+) (-,+) (+,-) (-,-) en (x,z) */
    for (int b = 0; b < 2; b++) {
        uint32_t pk = b ? pk1 : pk0; w16(pk, 33); w8(pk + 7, 0x3E);
        for (int v = 0; v < 4; v++) { double sx, sy, z; ref_proj(CW[v], &sx, &sy, &z); w16(pk + 8 + 12 * v, (int)lround(sx)); w16(pk + 10 + 12 * v, (int)lround(sy)); w8(pk + 12 + 12 * v, v * 8); w8(pk + 13 + 12 * v, 3); }
        w16(pk + 14, 0x4455); w16(pk + 26, 0x0A | (2 << 5));
    }
    /* agua: caja y ojo del motor dentro */
    uint32_t wt = actor_add(5, NAME_WTVIEW, 0x60);
    w16(wt + 0x28, -5000); w16(wt + 0x2A, -5000); w16(wt + 0x2C, -9000); w16(wt + 0x30, 5000); w16(wt + 0x32, 5000); w16(wt + 0x34, 9000);
    w8(wt + 0x3C, 100); w8(wt + 0x3D, 128); w8(wt + 0x3E, 120); w32(wt + 0x40, 1); w32(wt + 0x48, 777);
    /* fundido a negro */
    uint32_t fd = actor_add(6, NAME_FADEIO, 0x40); w32(fd + 0x34, 0); w32(fd + 0x30, 102);
    /* gas */
    uint32_t gs = actor_add(7, NAME_GASEFCT, 0x90);
    w32(gs + 0x40, 1); w32(gs + 0x4C, 4096); w32(gs + 0x44, 0); w32(gs + 0x50, 0); w32(gs + 0x54, 128); w32(gs + 0x58, 2); w32(gs + 0x5C, 80); w32(gs + 0x64, 3205);
    w16(gs + 0x2C, 30); w16(gs + 0x2E, 30); w16(gs + 0x30, 0); w16(gs + 0x34, 100); w16(gs + 0x36, 100); w16(gs + 0x38, 60);
    w32(NSFX_A_GAME_STATUS, 4);
    queue_commit(0);
    int r; NsFxScene *s = run(0, &r);
    CHECK(r == 0 && s->n_items == 2, "n_items=%u", s->n_items);
    CHECK(s->items[0].role == NSFX_ROLE_SMOKE, "papel por actor dueño: %s", ns_fx_role_name(s->items[0].role));
    CHECK(s->n_shadow_objs == 1 && s->shadow_objs[0] == objs, "objeto de sombra reclamado");
    const NsFxItem *q = &s->items[1];
    CHECK(q->role == NSFX_ROLE_SHADOW_CHAR && (q->flags & NSFX_F_FROM_OBJ) && q->shape == NSFX_SH_QUAD, "sombra de personaje");
    for (int v = 0; v < 4; v++) CHECK(NEAR(q->pos[v][0], CW[v][0], 0.6) && NEAR(q->pos[v][1], CW[v][1], 0.6) && NEAR(q->pos[v][2], CW[v][2], 0.6), "esquina %d de la sombra en el orden del paquete (%.1f,%.1f,%.1f)", v, q->pos[v][0], q->pos[v][1], q->pos[v][2]);
    CHECK(q->abr == 2 && q->clut == 0x4455 && q->raise == 150 && q->uv[2][0] == 16, "estado de la sombra abr=%u clut=0x%X", q->abr, q->clut);
    CHECK(s->screen.water.active && s->screen.water.present && NEAR(s->screen.water.tint[0], 100 / 128.0, 1e-6) && NEAR(s->screen.water.box_max[2], 9000, 0), "agua");
    CHECK(s->screen.water.wave_rows == 4 && s->screen.water.bar_rows == 6 && s->screen.water.phase == 777, "parámetros del agua");
    CHECK(s->screen.fade.active && !s->screen.fade.white && NEAR(s->screen.fade.amount, 0.4, 1e-6), "fundido %.3f", s->screen.fade.amount);
    CHECK(s->screen.nvg == 1 && s->screen.thermal == 0, "gafas nocturnas");
    CHECK(s->screen.gas.active && NEAR(s->screen.gas.intensity, 1.0, 1e-6) && NEAR(s->screen.gas.col_inner[0], 30 / 255.0, 1e-3) && NEAR(s->screen.gas.col_outer[0], 100 / 255.0, 1e-3) && NEAR(s->screen.gas.aspect, 3205 / 4096.0, 1e-4), "gas activo");
    ns_fx_scene_destroy(s);
    w32(NSFX_A_GAME_STATUS, 8);   /* gafas térmicas: el motor no dibuja el anillo */
    s = run(0, &r);
    CHECK(s->screen.gas.present && !s->screen.gas.active && s->screen.thermal == 1, "gas apagado con gafas térmicas");
    ns_fx_scene_destroy(s);
}

static void test_classify(void) {
    reset();
    memset(LASTNODE, 0, sizeof LASTNODE);
    int t0[3] = {0, 0, 0};
    TPrim bb = mkprim(18, 0x400, 1, 4096, t0, 0);
    put_pos(&bb, 0, 1200, 700, 1500); put_rect(&bb, 300, 200, 600, 400);
    double cw[3] = {1200, 700, 1500}, sx, sy, z; ref_proj(cw, &sx, &sy, &z);
    int bl = (int)lround(sx - 300 * G_H / z), br = (int)lround(sx + 300 * G_H / z), bt = (int)lround(sy - 200 * G_H / z), bb_ = (int)lround(sy + 200 * G_H / z);
    for (int b = 0; b < 2; b++) {
        uint32_t pk = bb.pk[b]; w16(pk, 77); w8(pk + 7, 0x2E);
        pk_xy(pk, 18, 0, bl, bt); pk_xy(pk, 18, 1, br, bt); pk_xy(pk, 18, 2, bl, bb_); pk_xy(pk, 18, 3, br, bb_);
        for (int v = 0; v < 4; v++) pk_uv(pk, 18, v, (v & 1) ? 63 : 0, (v & 2) ? 63 : 0);
        w16(pk + 14, 0x1111); w16(pk + 22, 0x1A | (1 << 5));
    }
    TPrim so = mkprim(18, 0x800, 1, 4096, t0, 0);   /* de ordenación 2D (HUD): sus paquetes son 2D */
    for (int b = 0; b < 2; b++) { uint32_t pk = so.pk[b]; w16(pk, 1); w8(pk + 7, 0x2C); pk_xy(pk, 18, 0, -150, -100); pk_xy(pk, 18, 1, -100, -100); pk_xy(pk, 18, 2, -150, -60); pk_xy(pk, 18, 3, -100, -60); }
    /* objeto de malla con un paquete GT4 emitido */
    uint32_t obj = alloc(0x48 + 0x5C), mpk = alloc(52 * 2);
    w16(obj + 0x2E, 1); w16(obj + 0x48 + 0x52, 1); w32(obj + 0x48 + 0x54, mpk); w32(obj + 0x48 + 0x58, mpk + 52);
    w16(mpk, 9); w8(mpk + 7, 0x3C);
    { int xs[4] = {20, 60, 20, 60}, ys[4] = {20, 20, 60, 60}; for (int v = 0; v < 4; v++) { w16(mpk + 8 + 12 * v, xs[v]); w16(mpk + 10 + 12 * v, ys[v]); } }
    w32(QUEUE, obj); NQ = 1;
    /* reordenar: el objeto va primero en la cola; las primitivas que mkprim añadió quedan detrás */
    uint32_t pa = bb.addr, pb = so.addr; w32(QUEUE + 4, pa); w32(QUEUE + 8, pb); NQ = 3;
    queue_commit(1);
    int r; NsFxScene *s = run(0, &r);
    CHECK(r == 0 && s->n_items == 1, "n_items=%u", s->n_items);
    enum { N = 11 };
    GpuOp *ops = (GpuOp *)calloc(N, sizeof(GpuOp));
    int dox = 160, doy = 120, k = 0;
    ops[k].type = GPU_OP_POLY; ops[k].nv = 4; ops[k].flags = GPU_F_TEXTURED | GPU_F_SEMI | GPU_F_QUAD; ops[k].dox = (int16_t)dox; ops[k].doy = (int16_t)doy;
    { int xs[4] = {bl, br, bl, br}, ys[4] = {bt, bt, bb_, bb_}; for (int v = 0; v < 4; v++) { ops[k].v[v].x = (int16_t)(xs[v] + dox); ops[k].v[v].y = (int16_t)(ys[v] + doy); ops[k].v[v].u = (uint8_t)((v & 1) ? 63 : 0); ops[k].v[v].v = (uint8_t)((v & 2) ? 63 : 0); } }
    ops[k].tp = 1; ops[k].abr = 1; ops[k].tx = 640; ops[k].ty = 0; ops[k].clx = 16 * 5; ops[k].cly = 480; k++;                    /* 0: efecto */
    ops[k].type = GPU_OP_POLY; ops[k].nv = 4; ops[k].flags = GPU_F_TEXTURED | GPU_F_QUAD; ops[k].dox = (int16_t)dox; ops[k].doy = (int16_t)doy;
    { int xs[4] = {20, 60, 20, 60}, ys[4] = {20, 20, 60, 60}; for (int v = 0; v < 4; v++) { ops[k].v[v].x = (int16_t)(xs[v] + dox); ops[k].v[v].y = (int16_t)(ys[v] + doy); ops[k].v[v].vvalid = 1; } } k++;   /* 1: malla (paquete del objeto) */
    ops[k].type = GPU_OP_POLY; ops[k].nv = 4; ops[k].flags = GPU_F_TEXTURED | GPU_F_QUAD; ops[k].dox = (int16_t)dox; ops[k].doy = (int16_t)doy;
    { int xs[4] = {-150, -100, -150, -100}, ys[4] = {-100, -100, -60, -60}; for (int v = 0; v < 4; v++) { ops[k].v[v].x = (int16_t)(xs[v] + dox); ops[k].v[v].y = (int16_t)(ys[v] + doy); } } k++;   /* 2: 2D de ordenación */
    ops[k].type = GPU_OP_RECT; ops[k].flags = GPU_F_TEXTURED; ops[k].rw = 24; ops[k].rh = 8; ops[k].v[0].x = 10; ops[k].v[0].y = 12; ops[k].tp = 0; k++;           /* 3: texto/HUD */
    ops[k].type = GPU_OP_RECT; ops[k].flags = GPU_F_SEMI; ops[k].rw = 320; ops[k].rh = 240; ops[k].v[0].x = 0; ops[k].v[0].y = 0; k++;                           /* 4: fundido */
    ops[k].type = GPU_OP_RECT; ops[k].flags = GPU_F_TEXTURED; ops[k].tp = 2; ops[k].rw = 160; ops[k].rh = 2; ops[k].tx = 0; ops[k].ty = 0; ops[k].v[0].x = 0; ops[k].v[0].y = 40; k++;   /* 5: tira de copia de pantalla */
    ops[k].type = GPU_OP_RECT; ops[k].rw = 320; ops[k].rh = 6; ops[k].v[0].x = 0; ops[k].v[0].y = 0; k++;                                                         /* 6: barra */
    ops[k].type = GPU_OP_FILL; k++;                                                                                                                                /* 7: evento */
    ops[k].type = GPU_OP_POLY; ops[k].nv = 3; ops[k].flags = GPU_F_GOURAUD; for (int v = 0; v < 3; v++) { ops[k].v[v].x = (int16_t)(10 + 10 * v); ops[k].v[v].y = (int16_t)(100 + 7 * v); ops[k].v[v].vvalid = 1; } k++; /* 8: 3D sin paquete (subdividido) */
    ops[k].type = GPU_OP_POLY; ops[k].nv = 3; ops[k].flags = GPU_F_GOURAUD; ops[k].v[0].x = 200; ops[k].v[0].y = 200; ops[k].v[1].x = 250; ops[k].v[1].y = 200; ops[k].v[2].x = 200; ops[k].v[2].y = 230; k++;   /* 9: 2D */
    ops[k].type = GPU_OP_RECT; ops[k].flags = GPU_F_TEXTURED; ops[k].rw = 8; ops[k].rh = 8; ops[k].v[0].x = 100; ops[k].v[0].y = 100; k++;   /* 10: rect 2D que no coincide */
    GpuDrawList dl; memset(&dl, 0, sizeof dl); dl.ops = ops; dl.nops = N; dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 240;
    uint8_t cls[N], sub[N]; int32_t item[N]; NsFxClsStats st;
    CHECK(ns_fx_classify(RAM, s, &dl, cls, sub, item, &st) == 0, "classify");
    CHECK(cls[0] == NSFX_OP_FX && item[0] == 0, "0 efecto -> FX (%d)", cls[0]);
    CHECK(cls[1] == NSFX_OP_MESH, "1 malla (%d)", cls[1]);
    CHECK(cls[2] == NSFX_OP_2D, "2 paquete de ordenación 2D (%d)", cls[2]);
    CHECK(cls[3] == NSFX_OP_2D, "3 texto (%d)", cls[3]);
    CHECK(cls[4] == NSFX_OP_SCREEN && sub[4] == NSFX_SC_COVER, "4 fundido (%d/%d)", cls[4], sub[4]);
    CHECK(cls[5] == NSFX_OP_SCREEN && sub[5] == NSFX_SC_COPY, "5 copia de pantalla (%d/%d)", cls[5], sub[5]);
    CHECK(cls[6] == NSFX_OP_SCREEN && sub[6] == NSFX_SC_BARS, "6 barra (%d/%d)", cls[6], sub[6]);
    CHECK(cls[7] == NSFX_OP_EVENT, "7 evento");
    CHECK(cls[8] == NSFX_OP_MESH_GUESS, "8 malla subdividida (%d)", cls[8]);
    CHECK(cls[9] == NSFX_OP_2D && cls[10] == NSFX_OP_2D, "9/10 2D (%d %d)", cls[9], cls[10]);
    CHECK(st.n_ops == N && st.n_fx == 1 && st.n_mesh == 1 && st.n_screen == 3 && st.n_2d == 4 && st.n_event == 1 && st.n_mesh_guess == 1, "estadísticas fx=%u mesh=%u scr=%u 2d=%u ev=%u", st.n_fx, st.n_mesh, st.n_screen, st.n_2d, st.n_event);
    CHECK(st.n_fx_items_hit == 1 && st.n_fx_items_missing == 0, "elementos casados %u perdidos %u", st.n_fx_items_hit, st.n_fx_items_missing);
    /* completar la página de textura del sprite desde la primitiva de la GPU */
    s->items[0].flags |= NSFX_F_TPAGE_UNKNOWN; s->items[0].tpage = 0;
    CHECK(ns_fx_resolve_items(s, &dl, item) == 1 && s->items[0].tpage == (10 | (0 << 4) | (1 << 5) | (1 << 7)) && s->items[0].clut == (5 | (480 << 6)), "resolución de página 0x%X clut 0x%X", s->items[0].tpage, s->items[0].clut);
    /* un elemento que el motor emitió y no está en la lista -> perdido */
    ops[0].type = GPU_OP_FILL;
    ns_fx_classify(RAM, s, &dl, cls, sub, item, &st);
    CHECK(st.n_fx_items_missing == 1 && st.n_fx_items_vis == 1, "perdido %u", st.n_fx_items_missing);
    /* anillo de gas: 16 polígonos degradados semitransparentes alrededor */
    GpuOp *ring = (GpuOp *)calloc(16, sizeof(GpuOp)); GpuDrawList d2; memset(&d2, 0, sizeof d2); d2.ops = ring; d2.nops = 16; d2.disp.w = 320; d2.disp.h = 240;
    for (int i = 0; i < 16; i++) { double a0 = i * 0.392699, a1 = (i + 1) * 0.392699; ring[i].type = GPU_OP_POLY; ring[i].nv = 4; ring[i].flags = GPU_F_GOURAUD | GPU_F_SEMI | GPU_F_QUAD;
        double xs[4] = {80 * cos(a0), 80 * cos(a1), 230 * cos(a0), 230 * cos(a1)}, ys[4] = {60 * sin(a0), 60 * sin(a1), 170 * sin(a0), 170 * sin(a1)};
        for (int v = 0; v < 4; v++) { ring[i].v[v].x = (int16_t)(160 + xs[v] / 2.2); ring[i].v[v].y = (int16_t)(120 + ys[v] / 2.2); } }
    uint8_t c2[16], s2[16]; NsFxClsStats st2;
    CHECK(ns_fx_classify(NULL, s, &d2, c2, s2, NULL, &st2) == 0 && st2.sub[NSFX_SC_RING] == 16 && c2[0] == NSFX_OP_SCREEN, "anillo detectado (%u)", st2.sub[NSFX_SC_RING]);
    free(ring); free(ops); ns_fx_scene_destroy(s);
}

static void test_xform(void) {
    NsFxCam cam; memset(&cam, 0, sizeof cam); cam.det_neg = 1;
    NsFxXform x = ns_fx_xform_default(); x.origin[0] = 1000; x.origin[1] = 0; x.origin[2] = -2000;
    float w[3] = {3000, 500, 1000}, o[3];
    ns_fx_to_vr(&x, &cam, w, o);
    CHECK(NEAR(o[0], 2.0, 1e-6) && NEAR(o[1], 0.5, 1e-6) && NEAR(o[2], -3.0, 1e-6), "mundo -> VR con espejo en Z (%.3f %.3f %.3f)", o[0], o[1], o[2]);
    x.mirror_z = 0; ns_fx_to_vr(&x, &cam, w, o); CHECK(NEAR(o[2], 3.0, 1e-6), "sin espejo");
    x.mirror_z = -1; cam.det_neg = 0; ns_fx_to_vr(&x, &cam, w, o); CHECK(NEAR(o[2], 3.0, 1e-6), "automático sin espejo");
    /* espacio del usuario: giro de 90 grados alrededor de Y, k = 2, altura de ojo 1,6 m, p_ref = (1,0,0) */
    x.user_map_on = 1; x.user_k = 2.0f; x.user_eye_h_m = 1.6f; x.user_p_ref[0] = 1;
    float R[9] = {0, 0, 1, 0, 1, 0, -1, 0, 0}; memcpy(x.user_R, R, sizeof R);
    ns_fx_to_vr(&x, &cam, w, o);
    /* p = (2,0.5,3) -> q = (4,1,6) -> R*q = (6,1,-4) - (0,1.6,0)... - R[:,1]*eye_h = (0,1,0)*1.6 */
    CHECK(NEAR(o[0], 7.0, 1e-5) && NEAR(o[1], 1.0 - 1.6, 1e-5) && NEAR(o[2], -4.0, 1e-5), "mapa de usuario (%.3f %.3f %.3f)", o[0], o[1], o[2]);
    float d[3] = {1, 0, 0}, od[3]; ns_fx_dir_to_vr(&x, &cam, d, od);
    CHECK(NEAR(od[0], 0, 1e-6) && NEAR(od[2], -2.0, 1e-6), "dirección con k");
    CHECK(NEAR(ns_fx_len_scale(&x), 0.002, 1e-9), "escala lineal");
}

static void test_garbage(void) {
    /* RAM aleatoria y valores límite: no debe fallar ni escribir fuera (ASan/UBSan) */
    uint32_t seed = 12345;
    for (int it = 0; it < 40; it++) {
        for (uint32_t i = 0; i < NSFX_RAM_SIZE; i++) { seed = seed * 1664525u + 1013904223u; RAM[i] = (uint8_t)(seed >> 24); }
        if (it & 1) { cam_setup(); }
        if (it % 4 == 3) { /* cola con punteros válidos a basura */
            uint32_t ch = NSFX_A_CHANL; w16(ch + 0x52, 200); w16(ch + 0x54, 0); w32(ch + 0x58, 0x80100000u);
            for (int i = 0; i < 200; i++) w32(0x80100000u + 4 * i, 0x80100800u + 0x60u * i);
        }
        int r; NsFxScene *s = run(-1, &r);
        CHECK(r == 0 || r == -2, "retorno %d", r);
        GpuOp *ops = (GpuOp *)calloc(64, sizeof(GpuOp)); GpuDrawList dl; memset(&dl, 0, sizeof dl); dl.ops = ops; dl.nops = 64; dl.disp.w = 320; dl.disp.h = 240;
        for (int i = 0; i < 64; i++) { seed = seed * 1664525u + 1013904223u; ops[i].type = (uint8_t)((seed >> 8) % 8); ops[i].nv = (uint8_t)((seed >> 12) % 6); ops[i].flags = (uint16_t)(seed >> 16); ops[i].rw = (int16_t)(seed >> 3); ops[i].rh = (int16_t)(seed >> 5); ops[i].tp = (uint8_t)((seed >> 20) & 3); }
        uint8_t cls[64]; NsFxClsStats st;
        CHECK(ns_fx_classify(RAM, s, &dl, cls, NULL, NULL, &st) == 0, "classify basura");
        free(ops); ns_fx_scene_destroy(s);
    }
    int r; NsFxScene *s = ns_fx_scene_create(2);   /* capacidad pequeña: desbordamiento controlado */
    reset(); int t0[3] = {0, 0, 0}; TPrim a = mkprim(14, 0, 5, 4096, t0, 0);
    for (int j = 0; j < 5; j++) { for (int b = 0; b < 2; b++) { w16(a.pk[b] + 24 * (uint32_t)j, 5); } for (int v = 0; v < 4; v++) put_pos(&a, j * 4 + v, v * 100, v * 50, 2000); }
    queue_commit(0); NsFxOpts o = ns_fx_opts_default(); o.buffer = 0; r = ns_fx_extract(RAM, &o, s);
    CHECK(r == 0 && s->n_items == 2 && s->stats.n_overflow >= 1, "desbordamiento: n=%u over=%u", s->n_items, s->stats.n_overflow);
    CHECK(ns_fx_extract(NULL, &o, s) == -1 && ns_fx_extract(RAM, &o, NULL) == -1, "argumentos nulos");
    ns_fx_scene_destroy(s);
}

int main(void) {
    RAM = (uint8_t *)calloc(NSFX_RAM_SIZE, 1);
    test_polygon_and_billboard();
    test_filters_and_oneside();
    test_lines_and_oncamera();
    test_actors_and_screen();
    test_classify();
    test_xform();
    test_garbage();
    free(RAM);
    printf("%s: %d comprobaciones, %d fallos\n", g_fail ? "FALLA" : "OK", g_checks, g_fail);
    return g_fail != 0;
}
