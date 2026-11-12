/* Pruebas sintéticas de ns_fx: RAM construida a mano (sin datos del juego) con proyección de referencia independiente. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ns_fx.h"
#include "ns_fx_export.h"

/* Nombres neutros de actor: la prueba instala su propia tabla (hash -> papel) con ns_fx_set_actor_table, sin depender de la tabla de producción. */
#define NAME_SMOKE "actor_a.bin"
#define NAME_SHADOW "actor_b.bin"
#define NAME_WTVIEW "actor_c.bin"
#define NAME_FADEIO "actor_d.bin"
#define NAME_GASEFCT "actor_e.bin"
static void install_test_table(void) {
    static NsFxActorEntry t[5];
    t[0].hash = ns_fx_name_hash(NAME_SMOKE); t[0].code = NSFX_ROLE_SMOKE;
    t[1].hash = ns_fx_name_hash(NAME_SHADOW); t[1].code = NSFX_ROLE_SHADOW_OBJ;
    t[2].hash = ns_fx_name_hash(NAME_WTVIEW); t[2].code = NSFX_AK_WATER;
    t[3].hash = ns_fx_name_hash(NAME_FADEIO); t[3].code = NSFX_AK_FADE;
    t[4].hash = ns_fx_name_hash(NAME_GASEFCT); t[4].code = NSFX_AK_GAS;
    ns_fx_set_actor_table(t, 5);
}
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
    /* Cuadrilátero texturizado de mundo (vertices) con paquete bien proyectado en el búfer 0 */
    TPrim a = mkprim(18, 0, 1, 4096, t0, 0);
    int P[4][3] = {{500, 800, 2000}, {1500, 800, 2000}, {500, 100, 2600}, {1500, 100, 2600}};
    double sz[4];
    for (int v = 0; v < 4; v++) put_pos(&a, v, P[v][0], P[v][1], P[v][2]);
    for (int b = 0; b < 2; b++) {
        uint32_t pk = a.pk[b]; w16(pk, 1234); w8(pk + 7, 0x2E);
        for (int v = 0; v < 4; v++) { double w[3] = {P[v][0], P[v][1], P[v][2]}, sx, sy; ref_proj(w, &sx, &sy, &sz[v]); pk_xy(pk, 18, v, (int)lround(sx), (int)lround(sy)); pk_uv(pk, 18, v, 10 * v, 5 * v); }
        w16(pk + 14, 0x7C10); w16(pk + 22, 0x2A | (1 << 5) | (1 << 7)); /* tpage: abr 1, tp 1 */
    }
    /* billboard (rectángulo) con rect centrado 600x400 */
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
    /* sprite con tamaño en píxeles (rectángulo, rect 8,8 -> esquina = proyección - 8) */
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
        CHECK(ne == 2 && trunc == 0, "export n=%u (el sprite sin página no se exporta)", ne);
        CHECK(ef[0].kind == NS_EK_QUAD && ef[0].nv == 4 && (ef[0].flags & NS_EF_TEXTURED) && ef[0].tpage == q->tpage && NEAR(ef[0].p[2][2], 2600, 0.01), "export quad");
        CHECK(ef[1].kind == NS_EK_BILLBOARD && (ef[1].flags & NS_EF_WORLD_SIZE) && NEAR(ef[1].ref_h, 320, 1e-6) && NEAR(ef[1].box[0], b->off[0], 1e-4) && NEAR(ef[1].box[3], b->off[3], 1e-4) && NEAR(ef[1].box[0], -300, 0.01) && NEAR(ef[1].box[3], 200 / NS_OVERSCAN_Y, 0.1), "export billboard box en mundo (%.2f %.2f)", ef[1].box[0], ef[1].box[3]);
        { NsFxScene *sm = s; NsFxItem *sprite = &sm->items[2]; uint16_t sv = sprite->flags;
          sprite->flags = (uint16_t)((sv & ~NSFX_F_TPAGE_UNKNOWN) | NSFX_F_LEARNED);   /* con la página aprendida sí */
          ne = ns_fx_export(s, 1, ef, 8, &trunc); sprite->flags = sv;
          CHECK(ne == 3 && (ef[2].flags & NS_EF_WORLD_SIZE) && NEAR(ef[2].box[0], c->off[0], 1e-3) && NEAR(ef[2].box[2], c->off[2], 1e-3) && NEAR(ef[2].box[1], c->off[1], 1e-3) && NEAR(ef[2].box[3], c->off[3], 1e-3), "export sprite: tamaño en mundo = el de la proyección (%.2f %.2f %.2f %.2f)", ef[2].box[0], ef[2].box[1], ef[2].box[2], ef[2].box[3]); }
        ne = ns_fx_export(s, 1, ef, 1, &trunc); CHECK(ne == 1 && trunc == 1, "export truncado");
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

/* Máscara de grupos visibles: se lee de la dirección real del motor (0x800ACAF8, la de ns_extract / vr_fire / vr_activity). Una primitiva con grupo del mapa actual se extrae; la de otro mapa no. */
static void test_map_group(void) {
    reset();
    w32(NSFX_A_MAP, 0); w32(0x800ACAF8u, 0x4);
    int t0[3] = {0, 0, 0};
    TPrim a = mkprim(18, 0, 1, 4096, t0, 0x4);
    int P[4][3] = {{500, 800, 2000}, {1500, 800, 2000}, {500, 100, 2600}, {1500, 100, 2600}};
    for (int v = 0; v < 4; v++) put_pos(&a, v, P[v][0], P[v][1], P[v][2]);
    for (int b = 0; b < 2; b++) {
        uint32_t pk = a.pk[b]; w16(pk, 1234); w8(pk + 7, 0x2E);
        for (int v = 0; v < 4; v++) { double w[3] = {P[v][0], P[v][1], P[v][2]}, sx, sy, z; ref_proj(w, &sx, &sy, &z); pk_xy(pk, 18, v, (int)lround(sx), (int)lround(sy)); }
    }
    TPrim mp = mkprim(14, 0, 1, 4096, t0, 0x2); (void)mp;
    queue_commit(0);
    int r; NsFxScene *s = run(0, &r);
    CHECK(r == 0 && s->cur_map == 0x4, "mapa actual leído de 0x800ACAF8: 0x%X", s->cur_map);
    CHECK(s->stats.n_wrong_map == 1 && s->n_items == 1 && s->items[0].prim == a.addr, "grupo del mapa actual extraído, el de otro mapa no: otro_mapa=%u elementos=%u", s->stats.n_wrong_map, s->n_items);
    ns_fx_scene_destroy(s);
}

/* ns_fx_covers_packet: garantía de cobertura nativa de un paquete del motor (GpuOp.pkt_addr). Solo los paquetes de una primitiva de la cola con un elemento que se exporta al renderer. */
static void test_covers_packet(void) {
    reset();
    ns_fx_learn_reset();
    int t0[3] = {0, 0, 0};
    TPrim a = mkprim(18, 0, 2, 4096, t0, 0x1);   /* dos cuadriláteros de mundo, grupo del mapa actual */
    int P[4][3] = {{500, 800, 2000}, {1500, 800, 2000}, {500, 100, 2600}, {1500, 100, 2600}};
    for (int j = 0; j < 2; j++) {
        for (int v = 0; v < 4; v++) put_pos(&a, j * 4 + v, P[v][0] + 300 * j, P[v][1], P[v][2]);
        for (int b = 0; b < 2; b++) {
            uint32_t pk = a.pk[b] + 40u * (uint32_t)j; w16(pk, 1234); w8(pk + 7, 0x2C);
            for (int v = 0; v < 4; v++) { double w[3] = {P[v][0] + 300 * j, P[v][1], P[v][2]}, sx, sy, z; ref_proj(w, &sx, &sy, &z); pk_xy(pk, 18, v, (int)lround(sx), (int)lround(sy)); }
        }
    }
    TPrim sp = mkprim(6, 0x400, 1, 4096, t0, 0);   /* sprite texturizado: su paquete no trae la página de textura */
    put_pos(&sp, 0, 900, 300, 1200); put_rect(&sp, 8, 8, 16, 16);
    {
        double w[3] = {900, 300, 1200}, sx, sy, z; ref_proj(w, &sx, &sy, &z);
        for (int b = 0; b < 2; b++) { uint32_t pk = sp.pk[b]; w16(pk, 5); w8(pk + 7, 0x66); w16(pk + 8, (int)lround(sx) - 8); w16(pk + 10, (int)lround(sy) - 8); w8(pk + 12, 16); w8(pk + 13, 32); w16(pk + 14, 0x1234); w16(pk + 16, 16); w16(pk + 18, 16); }
    }
    TPrim so = mkprim(18, 0x800, 1, 4096, t0, 0);    /* solo ordenación (fondo de cielo, HUD): ns_fx no la extrae */
    TPrim inv = mkprim(18, 0x100, 1, 4096, t0, 0);   /* invisible */
    TPrim mp = mkprim(18, 0, 1, 4096, t0, 0x2);      /* de otro mapa */
    for (int b = 0; b < 2; b++) { w16(so.pk[b], 7); w16(inv.pk[b], 7); w16(mp.pk[b], 7); }
    queue_commit(0);
    int r; NsFxScene *s = run(0, &r);
    CHECK(r == 0 && s->n_items == 3, "escena: 2 cuadriláteros + 1 sprite (%u)", s->n_items);
    CHECK(ns_fx_covers_packet(RAM, s, a.pk[0]) == 1 && ns_fx_covers_packet(RAM, s, a.pk[0] + 40) == 1, "paquetes 0 y 1 de la primitiva extraída (búfer 0): cubiertos");
    CHECK(ns_fx_covers_packet(RAM, s, a.pk[1]) == 1 && ns_fx_covers_packet(RAM, s, a.pk[1] + 40) == 1, "los mismos en el otro búfer de paquetes: cubiertos");
    CHECK(ns_fx_covers_packet(RAM, s, a.pk[0] + 4) == 0 && ns_fx_covers_packet(RAM, s, a.pk[0] + 44) == 0, "dirección que no es el inicio de un paquete: no");
    CHECK(ns_fx_covers_packet(RAM, s, so.pk[0]) == 0 && ns_fx_covers_packet(RAM, s, so.pk[1]) == 0, "primitiva de solo ordenación: no");
    CHECK(ns_fx_covers_packet(RAM, s, inv.pk[0]) == 0 && ns_fx_covers_packet(RAM, s, mp.pk[0]) == 0, "invisible / de otro mapa: no");
    CHECK(ns_fx_covers_packet(RAM, s, 0x80180000u) == 0 && ns_fx_covers_packet(RAM, s, 0) == 0 && ns_fx_covers_packet(RAM, s, a.addr) == 0, "dirección fuera de los paquetes de la cola: no");
    CHECK(ns_fx_covers_packet(NULL, s, a.pk[0]) == 0 && ns_fx_covers_packet(RAM, NULL, a.pk[0]) == 0, "sin RAM o sin escena: no");
    CHECK((s->items[2].flags & NSFX_F_TPAGE_UNKNOWN) && ns_fx_covers_packet(RAM, s, sp.pk[0]) == 0, "sprite texturizado sin página de textura conocida (no se exporta): no");
    {   /* con la página aprendida de la GPU el sprite se exporta: cubierto */
        GpuOp op; memset(&op, 0, sizeof op);
        op.type = GPU_OP_RECT; op.flags = GPU_F_TEXTURED | GPU_F_SEMI; op.rw = 16; op.rh = 16; op.tx = 704; op.ty = 256; op.tp = 1; op.abr = 2; op.clx = 16 * 3; op.cly = 500;
        GpuDrawList dl; memset(&dl, 0, sizeof dl); dl.ops = &op; dl.nops = 1; dl.disp.w = 320; dl.disp.h = 240;
        int32_t item[1] = {2};
        CHECK(ns_fx_learn(s, &dl, item) == 1, "aprende la página del sprite");
        ns_fx_scene_destroy(s);
        s = run(0, &r);
        CHECK(r == 0 && s->n_items == 3 && !(s->items[2].flags & NSFX_F_TPAGE_UNKNOWN) && ns_fx_covers_packet(RAM, s, sp.pk[0]) == 1 && ns_fx_covers_packet(RAM, s, sp.pk[1]) == 1, "sprite con la página aprendida: cubierto");
    }
    {   /* una primitiva retirada de la cola (la lista es de un fotograma anterior): sus paquetes dejan de estar cubiertos */
        NQ = 1; queue_commit(0);   /* solo queda `a` en la cola; la escena `s` es la de antes */
        CHECK(ns_fx_covers_packet(RAM, s, a.pk[0]) == 1 && ns_fx_covers_packet(RAM, s, sp.pk[0]) == 0, "primitiva que ya no está en la cola: no");
    }
    ns_fx_scene_destroy(s);
    ns_fx_learn_reset();
}

static void test_lines_and_oncamera(void) {
    reset();
    int t0[3] = {0, 0, 0};
    TPrim l = mkprim(22, 0, 1, 4096, t0, 0);   /* LINE_GT2 haz */
    put_pos(&l, 0, 0, 0, 1000); put_pos(&l, 1, 800, 400, 5000);
    for (int b = 0; b < 2; b++) {
        double w0[3] = {0, 0, 1000}, w1[3] = {800, 400, 5000}, sx0, sy0, z0, sx1, sy1, z1; ref_proj(w0, &sx0, &sy0, &z0); ref_proj(w1, &sx1, &sy1, &z1);
        uint32_t pk = l.pk[b]; w16(pk, 10); w8(pk + 7, 0x3E);
        w16(pk + 8, (int)lround(sx0)); w16(pk + 10, (int)lround(sy0)); w16(pk + 20, (int)lround(sx1)); w16(pk + 22, (int)lround(sy1));
    }
    TPrim pl = mkprim(2, 0, 1, 4096, t0, 0);   /* polilínea: 3 segmentos */
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
    CHECK(r == 0 && s->n_items == 5, "n_items=%u (haz 1 + polilínea 3 + vista 1)", s->n_items);
    CHECK(s->items[0].shape == NSFX_SH_RIBBON && s->items[0].role == NSFX_ROLE_BEAM, "haz -> cinta");
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
    CHECK(s->stats.chk_bad == 0, "reproyección haz/polilínea/vista: mal=%u de %u (max %.2f)", s->stats.chk_bad, s->stats.chk_n, s->stats.chk_max_px);
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
    /* sombra de objeto */
    uint32_t sh = actor_add(4, NAME_SHADOW, 0x100);
    uint32_t objs = alloc(0x48 + 0x5C + 64), pk0 = alloc(52), pk1 = alloc(52);
    w32(sh + 0x28, objs); w32(sh + 0x90, 1);
    w16(objs + 0, 1024); w16(objs + 8, 4096); w16(objs + 16, 512);   /* la matriz del objeto lleva la escala: x 1024 -> media 1000, z 512 -> media 500 */ w32(objs + 20, 2000); w32(objs + 24, 0); w32(objs + 28, 5000);
    w32(objs + 0x28, 0); w16(objs + 0x2C, 1); w16(objs + 0x2E, 1);
    w16(objs + 0x48, 1024); w16(objs + 0x48 + 16, 512);   /* la de la pieza (misma escala) no se aplica otra vez */
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
    w32(NSFX_A_STATUS, 4);
    queue_commit(0);
    int r; NsFxScene *s = run(0, &r);
    CHECK(r == 0 && s->n_items == 2, "n_items=%u", s->n_items);
    CHECK(s->items[0].role == NSFX_ROLE_SMOKE, "papel por actor dueño: %s", ns_fx_role_name(s->items[0].role));
    CHECK(s->n_shadow_objs == 1 && s->shadow_objs[0] == objs, "objeto de sombra reclamado");
    const NsFxItem *q = &s->items[1];
    CHECK(q->role == NSFX_ROLE_SHADOW_OBJ && (q->flags & NSFX_F_FROM_OBJ) && q->shape == NSFX_SH_QUAD, "sombra de objeto");
    for (int v = 0; v < 4; v++) CHECK(NEAR(q->pos[v][0], CW[v][0], 0.6) && NEAR(q->pos[v][1], CW[v][1], 0.6) && NEAR(q->pos[v][2], CW[v][2], 0.6), "esquina %d de la sombra en el orden del paquete (%.1f,%.1f,%.1f)", v, q->pos[v][0], q->pos[v][1], q->pos[v][2]);
    CHECK(q->abr == 2 && q->clut == 0x4455 && q->raise == 150 && q->uv[2][0] == 16, "estado de la sombra abr=%u clut=0x%X", q->abr, q->clut);
    CHECK(s->screen.water.active && s->screen.water.present && NEAR(s->screen.water.tint[0], 100 / 128.0, 1e-6) && NEAR(s->screen.water.box_max[2], 9000, 0), "agua");
    CHECK(s->screen.water.wave_rows == 4 && s->screen.water.bar_rows == 6 && s->screen.water.phase == 777, "parámetros del agua");
    CHECK(s->screen.fade.active && !s->screen.fade.white && NEAR(s->screen.fade.amount, 0.4, 1e-6), "fundido %.3f", s->screen.fade.amount);
    CHECK(s->screen.mode_a == 1 && s->screen.mode_b == 0 && s->screen.status == 4, "modo de visión a");
    CHECK(s->screen.gas.active && NEAR(s->screen.gas.intensity, 1.0, 1e-6) && NEAR(s->screen.gas.col_inner[0], 30 / 255.0, 1e-3) && NEAR(s->screen.gas.col_outer[0], 100 / 255.0, 1e-3) && NEAR(s->screen.gas.aspect, 3205 / 4096.0, 1e-4), "gas activo");
    ns_fx_scene_destroy(s);
    /* el motor no emite la sombra en otro fotograma: se usa la textura de la última emitida (no página 0 / modo supuesto) */
    w16(objs + 0x48 + 0x52, 0);
    s = run(0, &r);
    {
        const NsFxItem *z = &s->items[1];
        CHECK(z->role == NSFX_ROLE_SHADOW_OBJ && (z->flags & NSFX_F_LEARNED) && !(z->flags & NSFX_F_TPAGE_UNKNOWN) && z->clut == 0x4455 && z->abr == 2 && z->uv[0][0] + z->uv[1][0] + z->uv[2][0] + z->uv[3][0] == 48 && z->tpage != 0, "sombra no emitida con la textura aprendida (tpage=0x%X clut=0x%X flags=0x%X)", z->tpage, z->clut, z->flags);
        CHECK(NEAR((z->pos[0][0] + z->pos[3][0]) * 0.5, 2000, 0.6) && NEAR((z->pos[0][2] + z->pos[3][2]) * 0.5, 5000, 0.6) && NEAR(fabsf(z->pos[3][0] - z->pos[0][0]), 2000, 0.6), "geometría de la no emitida (centro y lado)");
    }
    ns_fx_scene_destroy(s);
    w16(objs + 0x48 + 0x52, 1);
    w32(NSFX_A_STATUS, 8);   /* modo b: el motor no dibuja el anillo */
    s = run(0, &r);
    CHECK(s->screen.gas.present && !s->screen.gas.active && s->screen.mode_b == 1, "gas apagado con el modo b");
    ns_fx_scene_destroy(s);
}

static void test_learn(void) {
    reset();
    memset(LASTNODE, 0, sizeof LASTNODE);
    int t0[3] = {0, 0, 0};
    TPrim sp = mkprim(6, 0x400, 1, 4096, t0, 0);
    put_pos(&sp, 0, 900, 300, 1200); put_rect(&sp, 8, 8, 16, 16);
    {
        double w[3] = {900, 300, 1200}, sx, sy, z; ref_proj(w, &sx, &sy, &z);
        for (int b = 0; b < 2; b++) { uint32_t pk = sp.pk[b]; w16(pk, 5); w8(pk + 7, 0x66); w16(pk + 8, (int)lround(sx) - 8); w16(pk + 10, (int)lround(sy) - 8); w8(pk + 12, 16); w8(pk + 13, 32); w16(pk + 14, 0x1234); w16(pk + 16, 16); w16(pk + 18, 16); }
    }
    TPrim sp2 = mkprim(6, 0x400, 1, 4096, t0, 0);   /* otra primitiva del mismo tipo: sin pareja en la lista, hereda lo aprendido por papel */
    put_pos(&sp2, 0, -900, 300, 1200); put_rect(&sp2, 8, 8, 16, 16);
    {
        double w[3] = {-900, 300, 1200}, sx, sy, z; ref_proj(w, &sx, &sy, &z);
        for (int b = 0; b < 2; b++) { uint32_t pk = sp2.pk[b]; w16(pk, 5); w8(pk + 7, 0x66); w16(pk + 8, (int)lround(sx) - 8); w16(pk + 10, (int)lround(sy) - 8); w8(pk + 12, 16); w8(pk + 13, 32); w16(pk + 14, 0x2345); w16(pk + 16, 16); w16(pk + 18, 16); }
    }
    queue_commit(0);
    ns_fx_learn_reset();
    int r; NsFxScene *s = run(0, &r);
    CHECK(r == 0 && s->n_items == 2 && (s->items[0].flags & NSFX_F_TPAGE_UNKNOWN) && (s->items[0].flags & NSFX_F_ABR_UNKNOWN) && !(s->items[0].flags & NSFX_F_LEARNED), "sprite sin página ni modo (flags 0x%X)", s->items[0].flags);
    GpuOp op; memset(&op, 0, sizeof op);
    op.type = GPU_OP_RECT; op.flags = GPU_F_TEXTURED | GPU_F_SEMI; op.rw = 16; op.rh = 16; op.tx = 704; op.ty = 256; op.tp = 1; op.abr = 2; op.clx = 16 * 3; op.cly = 500;
    GpuDrawList dl; memset(&dl, 0, sizeof dl); dl.ops = &op; dl.nops = 1; dl.disp.w = 320; dl.disp.h = 240;
    int32_t item[1] = {0};
    CHECK(ns_fx_learn(s, &dl, item) == 1, "aprende una operación");
    ns_fx_scene_destroy(s);
    s = run(0, &r);
    {
        const NsFxItem *it = &s->items[0];
        CHECK(r == 0 && (it->flags & NSFX_F_LEARNED) && !(it->flags & (NSFX_F_TPAGE_UNKNOWN | NSFX_F_ABR_UNKNOWN)), "aplicado (flags 0x%X)", it->flags);
        const NsFxItem *i2 = &s->items[1];
        CHECK(s->items[0].prim != i2->prim && (i2->flags & NSFX_F_LEARNED) && !(i2->flags & NSFX_F_TPAGE_UNKNOWN) && i2->tpage == it->tpage && i2->abr == 2 && i2->clut == 0x2345, "primitiva nueva con lo aprendido por papel (flags 0x%X tpage 0x%X clut 0x%X)", i2->flags, i2->tpage, i2->clut);
        CHECK(it->tpage == (11 | (1 << 4) | (2 << 5) | (1 << 7)) && it->abr == 2 && it->tp == 1 && it->clut == 0x1234, "página 0x%X abr %u clut 0x%X", it->tpage, it->abr, it->clut);
    }
    ns_fx_scene_destroy(s);
    ns_fx_learn_reset();
    s = run(0, &r);
    CHECK(r == 0 && (s->items[0].flags & NSFX_F_TPAGE_UNKNOWN) && (s->items[1].flags & NSFX_F_TPAGE_UNKNOWN) && !(s->items[1].flags & NSFX_F_LEARNED), "sin lo aprendido sigue desconocida");
    ns_fx_scene_destroy(s);
}

/* Identidad por DIRECCIÓN del paquete: un efecto animado (anime.c) reescribe u,v y color del paquete cada fotograma, así que el contenido de la RAM no es el de la lista (medido en a31 d0003/d0005);
 * la casación por contenido lo dejaba en 2D y el relleno lo contaba como «otra textura». Con GpuOp.pkt_addr el paquete es el del elemento aunque el contenido difiera, en cualquiera de los dos búferes. */
static void test_classify_by_address(void) {
    reset();
    ns_fx_learn_reset();
    int t0[3] = {0, 0, 0};
    TPrim a = mkprim(18, 0, 2, 4096, t0, 0x1);
    int P[4][3] = {{500, 800, 2000}, {1500, 800, 2000}, {500, 100, 2600}, {1500, 100, 2600}};
    int SX[2][4], SY[2][4];
    for (int j = 0; j < 2; j++) {
        for (int v = 0; v < 4; v++) put_pos(&a, j * 4 + v, P[v][0] + 300 * j, P[v][1], P[v][2]);
        for (int b = 0; b < 2; b++) {
            uint32_t pk = a.pk[b] + 40u * (uint32_t)j; w16(pk, 1234); w8(pk + 7, 0x2C);
            for (int v = 0; v < 4; v++) { double w[3] = {P[v][0] + 300 * j, P[v][1], P[v][2]}, sx, sy, z; ref_proj(w, &sx, &sy, &z); SX[j][v] = (int)lround(sx); SY[j][v] = (int)lround(sy); pk_xy(pk, 18, v, SX[j][v], SY[j][v]); }
        }
    }
    TPrim so = mkprim(18, 0x800, 1, 4096, t0, 0);   /* solo ordenación: no es un elemento */
    for (int b = 0; b < 2; b++) { w16(so.pk[b], 7); w8(so.pk[b] + 7, 0x2C); }
    queue_commit(0);
    int r; NsFxScene *s = run(0, &r);
    CHECK(r == 0 && s->n_items == 2, "escena: 2 cuadriláteros (%u)", s->n_items);
    enum { N = 4 };
    GpuOp ops[N]; memset(ops, 0, sizeof ops);
    for (int k = 0; k < N; k++) {
        int j = k == 1 ? 1 : 0;
        ops[k].type = GPU_OP_POLY; ops[k].nv = 4; ops[k].flags = GPU_F_TEXTURED | GPU_F_QUAD; ops[k].tp = 1; ops[k].tx = 640;
        for (int v = 0; v < 4; v++) { ops[k].v[v].x = (int16_t)SX[j][v]; ops[k].v[v].y = (int16_t)SY[j][v]; ops[k].v[v].u = (uint8_t)(9 + 20 * v); ops[k].v[v].v = 77; ops[k].v[v].r = 3; ops[k].v[v].g = 5; ops[k].v[v].b = 7; }
    }
    ops[0].pkt_addr = 0x80000000u | (a.pk[0] & 0x1FFFFF);                  /* elemento 0, búfer 0, u,v,rgb distintos a los de la RAM */
    ops[1].pkt_addr = 0x80000000u | ((a.pk[1] + 40) & 0x1FFFFF);           /* elemento 1, el OTRO búfer */
    ops[2].pkt_addr = 0x80000000u | (so.pk[0] & 0x1FFFFF);                 /* paquete de ordenación 2D: no es un elemento */
    ops[3].pkt_addr = 0;                                                    /* sin dirección: solo vale el contenido (no coincide) */
    GpuDrawList dl; memset(&dl, 0, sizeof dl); dl.ops = ops; dl.nops = N; dl.disp.w = 320; dl.disp.h = 240;
    uint8_t cls[N]; int32_t item[N];
    CHECK(ns_fx_classify(RAM, s, &dl, cls, NULL, item, NULL) == 0, "classify");
    CHECK(cls[0] == NSFX_OP_FX && item[0] == 0, "elemento 0 por dirección aunque u,v,rgb difieran (%d/%d)", cls[0], item[0]);
    CHECK(cls[1] == NSFX_OP_FX && item[1] == 1, "elemento 1 por dirección en el otro búfer (%d/%d)", cls[1], item[1]);
    CHECK(cls[2] != NSFX_OP_FX, "dirección de un paquete que no es elemento: no es efecto (%d)", cls[2]);
    CHECK(cls[3] == NSFX_OP_2D, "sin dirección y con contenido distinto: 2D (%d)", cls[3]);
    ns_fx_scene_destroy(s);
    ns_fx_learn_reset();
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
        w16(pk + 14, 0x1111); w16(pk + 22, 0x1A | (1 << 5) | (1 << 7));
    }
    TPrim so = mkprim(18, 0x800, 1, 4096, t0, 0);   /* de ordenación 2D (HUD): sus paquetes son 2D */
    for (int b = 0; b < 2; b++) { uint32_t pk = so.pk[b]; w16(pk, 1); w8(pk + 7, 0x2C); pk_xy(pk, 18, 0, -150, -100); pk_xy(pk, 18, 1, -100, -100); pk_xy(pk, 18, 2, -150, -60); pk_xy(pk, 18, 3, -100, -60); }
    /* objeto de malla con un paquete cuadrilátero degradado emitido */
    uint32_t obj = alloc(0x48 + 0x5C), mpk = alloc(52 * 2);
    w16(obj + 0x2E, 1); w16(obj + 0x48 + 0x52, 1); w32(obj + 0x48 + 0x54, mpk); w32(obj + 0x48 + 0x58, mpk + 52);
    w16(mpk, 9); w8(mpk + 7, 0x3C);
    { int xs[4] = {20, 60, 20, 60}, ys[4] = {20, 20, 60, 60}; for (int v = 0; v < 4; v++) { w16(mpk + 8 + 12 * v, xs[v]); w16(mpk + 10 + 12 * v, ys[v]); } }
    { uint32_t wt = actor_add(5, NAME_WTVIEW, 0x60);   /* agua y fundido activos: justifican los efectos de pantalla */
      w16(wt + 0x28, -5000); w16(wt + 0x2A, -5000); w16(wt + 0x2C, -9000); w16(wt + 0x30, 5000); w16(wt + 0x32, 5000); w16(wt + 0x34, 9000); w32(wt + 0x40, 1);
      uint32_t fd = actor_add(6, NAME_FADEIO, 0x40); w32(fd + 0x34, 0); w32(fd + 0x30, 102); }
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
    { int xs[4] = {20, 60, 20, 60}, ys[4] = {20, 20, 60, 60}; for (int v = 0; v < 4; v++) { ops[k].v[v].x = (int16_t)(xs[v] + dox); ops[k].v[v].y = (int16_t)(ys[v] + doy); ops[k].v[v].vvalid = 1; } } ops[k].proj_h = 320; k++;   /* 1: malla (paquete del objeto) */
    ops[k].type = GPU_OP_POLY; ops[k].nv = 4; ops[k].flags = GPU_F_TEXTURED | GPU_F_QUAD; ops[k].dox = (int16_t)dox; ops[k].doy = (int16_t)doy;
    { int xs[4] = {-150, -100, -150, -100}, ys[4] = {-100, -100, -60, -60}; for (int v = 0; v < 4; v++) { ops[k].v[v].x = (int16_t)(xs[v] + dox); ops[k].v[v].y = (int16_t)(ys[v] + doy); } } k++;   /* 2: 2D de ordenación */
    ops[k].type = GPU_OP_RECT; ops[k].flags = GPU_F_TEXTURED; ops[k].rw = 24; ops[k].rh = 8; ops[k].v[0].x = 10; ops[k].v[0].y = 12; ops[k].tp = 0; k++;           /* 3: texto/HUD */
    ops[k].type = GPU_OP_RECT; ops[k].flags = GPU_F_SEMI; ops[k].rw = 320; ops[k].rh = 240; ops[k].v[0].x = 0; ops[k].v[0].y = 0; k++;                           /* 4: fundido */
    ops[k].type = GPU_OP_RECT; ops[k].flags = GPU_F_TEXTURED; ops[k].tp = 2; ops[k].rw = 160; ops[k].rh = 2; ops[k].tx = 0; ops[k].ty = 0; ops[k].cx1 = 0; ops[k].cx2 = 319; ops[k].cy1 = 0; ops[k].cy2 = 239; ops[k].v[0].x = 0; ops[k].v[0].y = 40; k++;   /* 5: tira de copia de pantalla */
    ops[k].type = GPU_OP_RECT; ops[k].rw = 320; ops[k].rh = 6; ops[k].v[0].x = 0; ops[k].v[0].y = 0; ops[k].cx1 = 0; ops[k].cx2 = 319; ops[k].cy1 = 0; ops[k].cy2 = 239; k++;                                                         /* 6: barra */
    ops[k].type = GPU_OP_FILL; k++;                                                                                                                                /* 7: evento */
    ops[k].type = GPU_OP_POLY; ops[k].nv = 3; ops[k].flags = GPU_F_GOURAUD; for (int v = 0; v < 3; v++) { ops[k].v[v].x = (int16_t)(10 + 10 * v); ops[k].v[v].y = (int16_t)(100 + 7 * v); ops[k].v[v].vvalid = 1; } ops[k].proj_h = 320; k++; /* 8: 3D sin paquete (subdividido) */
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
    /* anillo de gas: 16 polígonos degradados semitransparentes alrededor; solo vale con el anillo activo en la RAM */
    NsFxScene *sg; { uint32_t gs = actor_add(7, NAME_GASEFCT, 0x90); w32(gs + 0x40, 1); w32(gs + 0x4C, 4096); w32(gs + 0x58, 2); w32(gs + 0x5C, 80); w32(gs + 0x64, 3205); int rr; sg = run(0, &rr); }
    GpuOp *ring = (GpuOp *)calloc(16, sizeof(GpuOp)); GpuDrawList d2; memset(&d2, 0, sizeof d2); d2.ops = ring; d2.nops = 16; d2.disp.w = 320; d2.disp.h = 240;
    for (int i = 0; i < 16; i++) { double a0 = i * 0.392699, a1 = (i + 1) * 0.392699; ring[i].type = GPU_OP_POLY; ring[i].nv = 4; ring[i].dox = 160; ring[i].doy = 120; ring[i].flags = GPU_F_GOURAUD | GPU_F_SEMI | GPU_F_QUAD;
        double xs[4] = {80 * cos(a0), 80 * cos(a1), 230 * cos(a0), 230 * cos(a1)}, ys[4] = {60 * sin(a0), 60 * sin(a1), 170 * sin(a0), 170 * sin(a1)};
        for (int v = 0; v < 4; v++) { ring[i].v[v].x = (int16_t)(160 + xs[v] / 2.2); ring[i].v[v].y = (int16_t)(120 + ys[v] / 2.2); } }
    uint8_t c2[16], s2[16]; NsFxClsStats st2;
    CHECK(sg->screen.gas.active, "gas activo");
    CHECK(ns_fx_classify(NULL, sg, &d2, c2, s2, NULL, &st2) == 0 && st2.sub[NSFX_SC_RING] == 16 && c2[0] == NSFX_OP_SCREEN, "anillo detectado (%u)", st2.sub[NSFX_SC_RING]);
    CHECK(ns_fx_classify(NULL, s, &d2, c2, s2, NULL, &st2) == 0 && st2.sub[NSFX_SC_RING] == 0 && c2[0] == NSFX_OP_2D, "sin gas activo el mismo dibujo es 2D");
    ns_fx_scene_destroy(sg);
    free(ring); free(ops); ns_fx_scene_destroy(s);
}


/* ================================================================================== pruebas de seguridad de la clasificación (el HUD no desaparece) */
static void op_rect(GpuOp *o, int x, int y, int w, int h, int flags, int tp) {
    memset(o, 0, sizeof *o); o->type = GPU_OP_RECT; o->v[0].x = (int16_t)x; o->v[0].y = (int16_t)y; o->rw = (int16_t)w; o->rh = (int16_t)h; o->flags = (uint16_t)flags; o->tp = (uint8_t)tp;
    o->cx1 = 0; o->cx2 = 319; o->cy1 = 0; o->cy2 = 239;
}
static void op_quad(GpuOp *o, const int xy[4][2], int flags, int clean3d) {
    memset(o, 0, sizeof *o); o->type = GPU_OP_POLY; o->nv = 4; o->flags = (uint16_t)(flags | GPU_F_QUAD);
    for (int v = 0; v < 4; v++) { o->v[v].x = (int16_t)xy[v][0]; o->v[v].y = (int16_t)xy[v][1]; if (clean3d) o->v[v].vvalid = 1; }
    if (clean3d) o->proj_h = 320;
    o->cx1 = 0; o->cx2 = 319; o->cy1 = 0; o->cy2 = 239;
}
/* escena de efectos: agua, fundido y gas activos (status = estado de la palabra global), o vacía */
static NsFxScene *mkscene(int with_effects, uint32_t status) {
    reset(); memset(LASTNODE, 0, sizeof LASTNODE);
    if (with_effects) {
        uint32_t wt = actor_add(5, NAME_WTVIEW, 0x60);
        w16(wt + 0x28, -5000); w16(wt + 0x2A, -5000); w16(wt + 0x2C, -9000); w16(wt + 0x30, 5000); w16(wt + 0x32, 5000); w16(wt + 0x34, 9000); w32(wt + 0x40, 1);
        uint32_t fd = actor_add(6, NAME_FADEIO, 0x40); w32(fd + 0x34, 0); w32(fd + 0x30, 102);
        uint32_t gs = actor_add(7, NAME_GASEFCT, 0x90); w32(gs + 0x40, 1); w32(gs + 0x4C, 4096); w32(gs + 0x58, 2); w32(gs + 0x5C, 80); w32(gs + 0x64, 3205);
    }
    w32(NSFX_A_STATUS, status);
    queue_commit(0);
    int r; return run(0, &r);
}
static void make_ring(GpuOp *ring, int n, int one_quadrant) {
    for (int i = 0; i < n; i++) {
        double a0 = (one_quadrant ? 0.1 : 0.0) + i * (one_quadrant ? 0.05 : 6.283185 / n), a1 = a0 + (one_quadrant ? 0.05 : 6.283185 / n);
        memset(&ring[i], 0, sizeof ring[i]); ring[i].type = GPU_OP_POLY; ring[i].nv = 4; ring[i].flags = GPU_F_GOURAUD | GPU_F_SEMI | GPU_F_QUAD; ring[i].dox = 160; ring[i].doy = 120;
        double xs[4] = {80 * cos(a0), 80 * cos(a1), 230 * cos(a0), 230 * cos(a1)}, ys[4] = {60 * sin(a0), 60 * sin(a1), 170 * sin(a0), 170 * sin(a1)};
        for (int v = 0; v < 4; v++) { ring[i].v[v].x = (int16_t)(160 + xs[v] / 2.2); ring[i].v[v].y = (int16_t)(120 + ys[v] / 2.2); }
    }
}
static int cls1(const NsFxScene *sc, GpuOp *ops, int n, uint8_t *sub_out) {
    GpuDrawList dl; memset(&dl, 0, sizeof dl); dl.ops = ops; dl.nops = (uint32_t)n; dl.disp.w = 320; dl.disp.h = 240;
    uint8_t cls[64], sub[64]; if (n > 64) n = 64;
    if (ns_fx_classify(RAM, sc, &dl, cls, sub, NULL, NULL) != 0) return -1;
    if (sub_out) *sub_out = sub[0];
    return cls[0];
}
static void test_stays_2d(void) {
    NsFxScene *none = mkscene(0, 0), *fx = mkscene(1, 0), *modes = mkscene(0, 4);
    CHECK(!fx->screen.water.active || fx->screen.fade.active, "escena de efectos");
    GpuOp o[64]; int xy[4][2] = {{10, 10}, {60, 10}, {10, 40}, {60, 40}}; int n = 0; (void)n;
    struct { const char *name; NsFxScene *sc; } sc2[2] = {{"sin efectos", none}, {"con efectos", fx}};
    for (int k = 0; k < 2; k++) {
        NsFxScene *sc = sc2[k].sc; const char *nm = sc2[k].name;
        op_rect(&o[0], 10, 12, 24, 8, GPU_F_TEXTURED, 0);                CHECK(cls1(sc, o, 1, NULL) == NSFX_OP_2D, "%s: tira de texto", nm);
        op_rect(&o[0], 0, 100, 320, 1, 0, 0);                            CHECK(cls1(sc, o, 1, NULL) == NSFX_OP_2D, "%s: separador de 1 fila", nm);
        op_rect(&o[0], 0, 100, 320, 2, 0, 0);                            CHECK(cls1(sc, o, 1, NULL) == NSFX_OP_2D, "%s: separador de 2 filas", nm);
        op_rect(&o[0], 0, 0, 320, 240, 0, 0);                            CHECK(cls1(sc, o, 1, NULL) == NSFX_OP_2D, "%s: fondo opaco de pantalla completa", nm);
        op_rect(&o[0], 0, 0, 320, 6, GPU_F_SEMI, 0);                     CHECK(cls1(sc, o, 1, NULL) == NSFX_OP_2D, "%s: barra semitransparente", nm);
        op_rect(&o[0], 0, 0, 320, 6, GPU_F_GOURAUD, 0);                  CHECK(cls1(sc, o, 1, NULL) == NSFX_OP_2D, "%s: barra degradada", nm);
        op_rect(&o[0], 0, 0, 320, 240, GPU_F_SEMI | GPU_F_TEXTURED, 0);  CHECK(cls1(sc, o, 1, NULL) == NSFX_OP_2D, "%s: pantalla completa texturizada", nm);
        op_rect(&o[0], 0, 40, 160, 2, GPU_F_TEXTURED, 2);
        o[0].cx2 = 100; CHECK(cls1(sc, o, 1, NULL) == NSFX_OP_2D, "%s: relectura fuera del área de dibujo", nm);
        op_quad(&o[0], xy, GPU_F_GOURAUD, 0);                            CHECK(cls1(sc, o, 1, NULL) == NSFX_OP_2D, "%s: polígono sin posición de vista", nm);
        op_quad(&o[0], xy, GPU_F_GOURAUD, 1); o[0].v[2].vflags = 1;      CHECK(cls1(sc, o, 1, NULL) == NSFX_OP_2D, "%s: vértice ambiguo", nm);
        op_quad(&o[0], xy, GPU_F_GOURAUD, 1); o[0].v[0].vflags = 2;      CHECK(cls1(sc, o, 1, NULL) == NSFX_OP_2D, "%s: vértice saturado", nm);
        op_quad(&o[0], xy, GPU_F_GOURAUD, 1); o[0].v[3].vflags = 4;      CHECK(cls1(sc, o, 1, NULL) == NSFX_OP_2D, "%s: vértice detrás", nm);
        op_quad(&o[0], xy, GPU_F_GOURAUD, 1); o[0].proj_h = 0;           CHECK(cls1(sc, o, 1, NULL) == NSFX_OP_2D, "%s: sin proyección", nm);
        memset(&o[0], 0, sizeof o[0]); o[0].type = GPU_OP_LINE; o[0].nv = 2; o[0].v[1].x = 300; CHECK(cls1(sc, o, 1, NULL) == NSFX_OP_2D, "%s: línea suelta", nm);
        op_quad(&o[0], xy, GPU_F_GOURAUD | GPU_F_SEMI, 0);               CHECK(cls1(sc, o, 1, NULL) == NSFX_OP_2D, "%s: indicador degradado semitransparente", nm);
    }
    /* sin fundido ni modos de visión: el recubrimiento semitransparente de pantalla completa es 2D */
    op_rect(&o[0], 0, 0, 320, 240, GPU_F_SEMI, 0);
    CHECK(cls1(none, o, 1, NULL) == NSFX_OP_2D && cls1(modes, o, 1, NULL) == NSFX_OP_SCREEN, "recubrimiento: 2D sin estado, pantalla con modo de visión");
    { uint8_t sb; CHECK(cls1(fx, o, 1, &sb) == NSFX_OP_SCREEN && sb == NSFX_SC_COVER, "recubrimiento con fundido activo"); }
    op_rect(&o[0], 0, 40, 160, 2, GPU_F_TEXTURED, 2);
    CHECK(cls1(none, o, 1, NULL) == NSFX_OP_2D, "relectura sin agua: 2D");
    { uint8_t sb; CHECK(cls1(fx, o, 1, &sb) == NSFX_OP_SCREEN && sb == NSFX_SC_COPY, "relectura con agua"); }
    op_rect(&o[0], 0, 100, 320, 1, 0, 0);
    { uint8_t sb; CHECK(cls1(none, o, 1, NULL) == NSFX_OP_2D && cls1(modes, o, 1, &sb) == NSFX_OP_SCREEN && sb == NSFX_SC_SCAN, "líneas de pantalla: solo con modo de visión"); }
    op_rect(&o[0], 0, 0, 320, 6, 0, 0);
    { uint8_t sb; CHECK(cls1(none, o, 1, NULL) == NSFX_OP_2D && cls1(fx, o, 1, &sb) == NSFX_OP_SCREEN && sb == NSFX_SC_BARS, "barras: solo con agua"); }
    /* anillo: exactamente 16, repartidos; con otro recuento o sin simetría, 2D */
    GpuOp ring[24]; GpuDrawList dl; memset(&dl, 0, sizeof dl); dl.disp.w = 320; dl.disp.h = 240;
    uint8_t c[24], sb[24]; NsFxClsStats st;
    int counts[5] = {1, 15, 16, 17, 20};
    for (int k = 0; k < 5; k++) {
        make_ring(ring, counts[k], 0); dl.ops = ring; dl.nops = (uint32_t)counts[k];
        ns_fx_classify(RAM, fx, &dl, c, sb, NULL, &st);
        CHECK(st.sub[NSFX_SC_RING] == (counts[k] == 16 ? 16u : 0u), "anillo con %d polígonos -> %u", counts[k], st.sub[NSFX_SC_RING]);
    }
    make_ring(ring, 16, 1); dl.ops = ring; dl.nops = 16; ns_fx_classify(RAM, fx, &dl, c, sb, NULL, &st);
    CHECK(st.sub[NSFX_SC_RING] == 0, "anillo sin simetría radial -> 2D");
    make_ring(ring, 16, 0); ns_fx_classify(RAM, none, &dl, c, sb, NULL, &st);
    CHECK(st.sub[NSFX_SC_RING] == 0 && st.n_2d == 16, "anillo sin gas activo -> 2D");
    ns_fx_scene_destroy(none); ns_fx_scene_destroy(fx); ns_fx_scene_destroy(modes);
}

/* Un paquete de ordenación 2D idéntico al de un elemento visible: gana 2D (C3). Un paquete ambiguo de una primitiva no extraída no es FX_PACK. */
static void test_sortonly_wins(void) {
    reset(); memset(LASTNODE, 0, sizeof LASTNODE);
    int t0[3] = {0, 0, 0};
    TPrim bb = mkprim(18, 0x400, 1, 4096, t0, 0);
    put_pos(&bb, 0, 1200, 700, 1500); put_rect(&bb, 300, 200, 600, 400);
    int xs[4] = {-40, 40, -40, 40}, ys[4] = {-30, -30, 30, 30};
    TPrim so = mkprim(18, 0x800, 1, 4096, t0, 0);
    TPrim hid = mkprim(18, 0x100, 1, 4096, t0, 0);
    for (int b = 0; b < 2; b++) {
        uint32_t pk = bb.pk[b]; w16(pk, 77); w8(pk + 7, 0x2E); for (int v = 0; v < 4; v++) { pk_xy(pk, 18, v, xs[v], ys[v]); pk_uv(pk, 18, v, (v & 1) ? 63 : 0, (v & 2) ? 63 : 0); }
        w16(pk + 22, 0x1A | (1 << 5) | (1 << 7));
        pk = so.pk[b]; w16(pk, 1); w8(pk + 7, 0x2E); for (int v = 0; v < 4; v++) { pk_xy(pk, 18, v, xs[v], ys[v]); pk_uv(pk, 18, v, (v & 1) ? 63 : 0, (v & 2) ? 63 : 0); }
        w16(pk + 22, 0x1A | (1 << 5) | (1 << 7));
        pk = hid.pk[b]; w16(pk, 50); w8(pk + 7, 0x2E); for (int v = 0; v < 4; v++) { pk_xy(pk, 18, v, 100 + xs[v], 50 + ys[v]); pk_uv(pk, 18, v, (v & 1) ? 63 : 0, (v & 2) ? 63 : 0); }
        w16(pk + 22, 0x1A | (1 << 5) | (1 << 7));
    }
    queue_commit(0);
    int r; NsFxScene *s = run(0, &r);
    GpuOp o[2]; GpuDrawList dl; memset(&dl, 0, sizeof dl); dl.ops = o; dl.nops = 2; dl.disp.w = 320; dl.disp.h = 240;
    for (int k = 0; k < 2; k++) {
        memset(&o[k], 0, sizeof o[k]); o[k].type = GPU_OP_POLY; o[k].nv = 4; o[k].flags = GPU_F_TEXTURED | GPU_F_SEMI | GPU_F_QUAD; o[k].tp = 1; o[k].proj_h = 320;
        for (int v = 0; v < 4; v++) { o[k].v[v].x = (int16_t)(xs[v] + (k ? 100 : 0)); o[k].v[v].y = (int16_t)(ys[v] + (k ? 50 : 0)); o[k].v[v].u = (uint8_t)((v & 1) ? 63 : 0); o[k].v[v].v = (uint8_t)((v & 2) ? 63 : 0); o[k].v[v].vvalid = 1; }
    }
    uint8_t c[2]; int32_t it[2];
    CHECK(ns_fx_classify(RAM, s, &dl, c, NULL, it, NULL) == 0, "classify");
    CHECK(c[0] == NSFX_OP_2D, "sortonly idéntico al elemento: 2D (%d)", c[0]);
    CHECK(c[1] == NSFX_OP_FX_PACK, "paquete limpio de primitiva invisible: FX_PACK (%d)", c[1]);
    o[1].v[1].vflags = 1;   /* ambiguo: nunca FX_PACK */
    ns_fx_classify(RAM, s, &dl, c, NULL, it, NULL);
    CHECK(c[1] == NSFX_OP_2D, "FX_PACK ambiguo -> 2D (%d)", c[1]);
    o[1].v[1].vflags = 0; o[1].flags &= (uint16_t)~GPU_F_SEMI;   /* distinto modo de mezcla: no casa */
    ns_fx_classify(RAM, s, &dl, c, NULL, it, NULL);
    CHECK(c[1] != NSFX_OP_FX_PACK && c[1] != NSFX_OP_FX, "no casa con otro modo de mezcla (%d)", c[1]);
    /* sin RAM: el sortonly no se ve, pero el elemento sigue casando solo con contenido exacto; la clase es candidata y la máscara la devuelve a la GPU */
    uint8_t g[2]; CHECK(ns_fx_classify(NULL, s, &dl, c, NULL, it, NULL) == 0, "classify sin RAM");
    CHECK(ns_fx_gpu_mask(c, NULL, 2, g) == 2 && g[0] && g[1], "nada confirmado: todo por la GPU");
    ns_fx_scene_destroy(s);
}

static void test_gpu_mask(void) {
    uint8_t cls[8] = {NSFX_OP_EVENT, NSFX_OP_2D, NSFX_OP_MESH, NSFX_OP_MESH_GUESS, NSFX_OP_FX, NSFX_OP_FX_PACK, NSFX_OP_SCREEN, NSFX_OP_MAINT};
    uint8_t conf[8] = {1, 1, 1, 0, 1, 0, 1, 1}, g[8];
    uint32_t k = ns_fx_gpu_mask(cls, conf, 8, g);
    CHECK(g[0] == 1 && g[1] == 1 && g[7] == 1, "evento, 2D y mantenimiento siempre por la GPU aunque se marquen confirmados");
    CHECK(g[2] == 0 && g[4] == 0 && g[6] == 0, "candidatos confirmados: el renderer los cubre");
    CHECK(g[3] == 1 && g[5] == 1, "candidatos sin confirmar: la GPU");
    CHECK(k == 5, "operaciones por la GPU: %u", k);
    CHECK(ns_fx_gpu_mask(cls, NULL, 8, g) == 8, "confirmed == NULL: todo por la GPU");
}

/* Fuzzer estructurado: sin efectos activos solo se sale de 2D/EVENT por coincidencia exacta; con efectos, cada SCREEN cumple su condición geométrica. */
static uint32_t rng_s = 777;
static uint32_t rnd(void) { rng_s = rng_s * 1664525u + 1013904223u; return rng_s >> 8; }
static void test_fuzz(void) {
    for (int mode = 0; mode < 2; mode++) {
        NsFxScene *s = mkscene(mode, mode ? 4 : 0);
        enum { N = 64 };
        GpuOp ops[N]; GpuDrawList dl; memset(&dl, 0, sizeof dl); dl.ops = ops; dl.nops = N; dl.disp.w = 320; dl.disp.h = 240;
        uint8_t cls[N], sub[N]; int32_t item[N]; NsFxClsStats st;
        for (int it = 0; it < 400; it++) {
            for (int i = 0; i < N; i++) {
                GpuOp *o = &ops[i]; memset(o, 0, sizeof *o);
                int t = (int)(rnd() % 6);
                o->type = (uint8_t)(t == 0 ? GPU_OP_RECT : t == 1 ? GPU_OP_LINE : t == 5 ? GPU_OP_FILL : GPU_OP_POLY);
                o->nv = (uint8_t)(o->type == GPU_OP_LINE ? 2 : 3 + (rnd() & 1)); o->flags = (uint16_t)(rnd() & (GPU_F_SEMI | GPU_F_TEXTURED | GPU_F_GOURAUD));
                o->rw = (int16_t)(rnd() % 400); o->rh = (int16_t)(rnd() % 260); o->tp = (uint8_t)(rnd() & 3);
                o->cx1 = 0; o->cx2 = 319; o->cy1 = 0; o->cy2 = 239; o->dox = (int16_t)((rnd() & 1) ? 160 : 0); o->doy = (int16_t)((rnd() & 1) ? 120 : 0);
                for (int v = 0; v < 4; v++) { o->v[v].x = (int16_t)((int)(rnd() % 400) - 40); o->v[v].y = (int16_t)((int)(rnd() % 300) - 30); o->v[v].vvalid = (uint8_t)(rnd() & 1); o->v[v].vflags = (uint8_t)((rnd() % 4) ? (rnd() & 7) : 0); }
                if (rnd() & 1) o->proj_h = 320;
            }
            CHECK(ns_fx_classify(RAM, s, &dl, cls, sub, item, &st) == 0, "classify");
            uint32_t nring = 0;
            for (int i = 0; i < N; i++) {
                const GpuOp *o = &ops[i];
                if (cls[i] == NSFX_OP_EVENT) CHECK(o->type == GPU_OP_FILL, "EVENT solo para eventos de VRAM");
                if (cls[i] == NSFX_OP_MESH_GUESS || cls[i] == NSFX_OP_FX_PACK) { int ok = o->type == GPU_OP_POLY && o->proj_h > 0; for (int v = 0; ok && v < o->nv; v++) ok = o->v[v].vvalid && o->v[v].vflags == 0; CHECK(ok, "candidato 3D requiere posición limpia"); }
                if (cls[i] == NSFX_OP_FX || cls[i] == NSFX_OP_MESH) CHECK(0, "sin elementos ni mallas coincidentes no puede haber FX/MESH");
                if (cls[i] == NSFX_OP_SCREEN) {
                    CHECK(mode == 1, "SCREEN sin ningún efecto activo (sub %d)", sub[i]);
                    int sb = sub[i];
                    if (sb == NSFX_SC_COPY) CHECK(o->tp >= 2 && (o->flags & GPU_F_TEXTURED), "COPY exige textura de 15 bits");
                    if (sb == NSFX_SC_COVER) CHECK((o->flags & GPU_F_SEMI) && !(o->flags & GPU_F_TEXTURED), "COVER exige semitransparente sin textura");
                    if (sb == NSFX_SC_BARS) CHECK(!(o->flags & (GPU_F_SEMI | GPU_F_TEXTURED | GPU_F_GOURAUD)), "BARS exige opaco liso");
                    if (sb == NSFX_SC_RING) nring++;
                }
            }
            CHECK(nring == 0 || nring == 16, "anillo parcial: %u", nring);
            uint32_t sum = st.n_event + st.n_2d + st.n_mesh + st.n_mesh_guess + st.n_fx + st.n_fx_pack + st.n_screen + st.n_maint;
            CHECK(sum == N, "recuento coherente");
        }
        ns_fx_scene_destroy(s);
    }
}

/* Límites del gas: valores extremos en la RAM se recortan. */
static void test_gas_limits(void) {
    reset(); memset(LASTNODE, 0, sizeof LASTNODE);
    uint32_t gs = actor_add(7, NAME_GASEFCT, 0x90);
    w32(gs + 0x40, 1); w32(gs + 0x4C, 0x7FFFFFFF); w32(gs + 0x58, 0x7777); w32(gs + 0x5C, (uint32_t)-5000); w32(gs + 0x64, 0x7FFFFFFF);
    w16(gs + 0x2C, 0x7FFF); w16(gs + 0x2E, -1); w16(gs + 0x34, 0x7FFF); w16(gs + 0x36, -30000); w32(gs + 0x44, 0xFFFFFFF0u); w32(gs + 0x50, 0x7FFFFFFF); w32(gs + 0x54, 0x7FFFFFFF);
    queue_commit(0);
    int r; NsFxScene *s = run(0, &r); const NsFxGas *g = &s->screen.gas;
    CHECK(r == 0 && g->intensity >= 0 && g->intensity <= 1.0f, "intensidad recortada %.3f", g->intensity);
    CHECK(g->aspect >= 0 && g->aspect <= 2.0f, "aspecto recortado %.3f", g->aspect);
    CHECK(g->abr >= 0 && g->abr <= 3, "modo de mezcla recortado %d", g->abr);
    for (int c = 0; c < 3; c++) CHECK(g->col_inner[c] >= 0 && g->col_inner[c] <= 1 && g->col_outer[c] >= 0 && g->col_outer[c] <= 1, "color recortado");
    CHECK(isfinite(g->inner_px) && isfinite(g->pulse_amp) && g->inner_px >= 0, "radio finito %.2f", g->inner_px);
    ns_fx_scene_destroy(s);
}

/* Rendimiento: peor caso acotado (todas las entradas colisionan; muchas operaciones). */
static void test_perf(void) {
    reset(); memset(LASTNODE, 0, sizeof LASTNODE);
    int t0[3] = {0, 0, 0};
    enum { NP = 3000, NOPS = 20000 };
    TPrim a = mkprim(18, 0, NP, 4096, t0, 0);
    for (int j = 0; j < NP; j++) {
        for (int v = 0; v < 4; v++) put_pos(&a, j * 4 + v, 500 + 100 * (v & 1), 300 + 100 * (v >> 1), 2000);
        for (int b = 0; b < 2; b++) { uint32_t pk = a.pk[b] + 40u * (uint32_t)j; w16(pk, 100); w8(pk + 7, 0x2E); for (int v = 0; v < 4; v++) pk_xy(pk, 18, v, (v & 1) * 30 - 15, (v >> 1) * 30 - 15); w16(pk + 22, 1 << 7); }
    }
    queue_commit(0);
    int r; NsFxScene *s = run(0, &r);
    GpuOp *ops = (GpuOp *)calloc(NOPS, sizeof(GpuOp)); GpuDrawList dl; memset(&dl, 0, sizeof dl); dl.ops = ops; dl.nops = NOPS; dl.disp.w = 320; dl.disp.h = 240;
    for (int i = 0; i < NOPS; i++) { ops[i].type = GPU_OP_POLY; ops[i].nv = 4; ops[i].flags = GPU_F_TEXTURED | GPU_F_QUAD; ops[i].tp = 0; for (int v = 0; v < 4; v++) { ops[i].v[v].x = (int16_t)((v & 1) * 30 - 15); ops[i].v[v].y = (int16_t)((v >> 1) * 30 - 15); } }
    uint8_t *c = (uint8_t *)malloc(NOPS); NsFxClsStats st;
    clock_t t = clock();
    CHECK(ns_fx_classify(RAM, s, &dl, c, NULL, NULL, &st) == 0, "classify");
    double secs = (double)(clock() - t) / CLOCKS_PER_SEC;
    CHECK(secs < 2.0, "tiempo del peor caso %.3f s", secs); printf("  perf: %.3f s desbordado=%u items=%u\n", secs, st.idx_overflow, s->n_items);
    CHECK(r == 0 && (st.idx_overflow == 1 ? st.n_2d == NOPS : 1), "desbordamiento del índice -> todo 2D (n_2d=%u)", st.n_2d);
    free(c); free(ops); ns_fx_scene_destroy(s);
}

/* RAM nula: se respeta el contrato (solo elementos). */
static void test_null_ram(void) {
    reset(); memset(LASTNODE, 0, sizeof LASTNODE);
    int t0[3] = {0, 0, 0};
    TPrim bb = mkprim(18, 0x400, 1, 4096, t0, 0);
    put_pos(&bb, 0, 1200, 700, 1500); put_rect(&bb, 300, 200, 600, 400);
    int xs[4] = {-40, 40, -40, 40}, ys[4] = {-30, -30, 30, 30};
    for (int b = 0; b < 2; b++) { uint32_t pk = bb.pk[b]; w16(pk, 77); w8(pk + 7, 0x2E); for (int v = 0; v < 4; v++) { pk_xy(pk, 18, v, xs[v], ys[v]); pk_uv(pk, 18, v, (v & 1) ? 63 : 0, (v & 2) ? 63 : 0); } w16(pk + 22, 0x1A | (1 << 5) | (1 << 7)); }
    queue_commit(0);
    int r; NsFxScene *s = run(0, &r);
    GpuOp o[2]; GpuDrawList dl; memset(&dl, 0, sizeof dl); dl.ops = o; dl.nops = 2; dl.disp.w = 320; dl.disp.h = 240;
    memset(o, 0, sizeof o);
    for (int k = 0; k < 2; k++) {
        o[k].type = GPU_OP_POLY; o[k].nv = 4; o[k].flags = GPU_F_TEXTURED | GPU_F_SEMI | GPU_F_QUAD; o[k].tp = 1;
        for (int v = 0; v < 4; v++) { o[k].v[v].x = (int16_t)(xs[v] + (k ? 7 : 0)); o[k].v[v].y = (int16_t)ys[v]; o[k].v[v].u = (uint8_t)((v & 1) ? 63 : 0); o[k].v[v].v = (uint8_t)((v & 2) ? 63 : 0); }
    }
    uint8_t c[2]; int32_t it[2];
    CHECK(ns_fx_classify(NULL, s, &dl, c, NULL, it, NULL) == 0, "classify sin RAM");
    CHECK(c[0] == NSFX_OP_FX && it[0] == 0 && c[1] == NSFX_OP_2D, "sin RAM: coincide el elemento (%d) y lo demás es 2D (%d)", c[0], c[1]);
    ns_fx_scene_destroy(s);
}

/* A2: cartel y cinta orientados con los ejes y la posición de la cabeza; A3: tamaño en píxeles con profundidad ínfima. */
static void test_head_and_px(void) {
    reset(); memset(LASTNODE, 0, sizeof LASTNODE);
    int t0[3] = {0, 0, 0};
    TPrim bb = mkprim(18, 0x400, 1, 4096, t0, 0);
    put_pos(&bb, 0, 1200, 700, 1500); put_rect(&bb, 300, 200, 600, 400);
    TPrim fp = mkprim(18, 0x2000, 1, 4096, t0, 0);   /* paquetes de formato libre ya proyectados, con profundidad diminuta */
    for (int b = 0; b < 2; b++) {
        uint32_t pk = bb.pk[b]; w16(pk, 77); w8(pk + 7, 0x2E); for (int v = 0; v < 4; v++) pk_xy(pk, 18, v, (v & 1) * 20 - 10, (v >> 1) * 20 - 10);
        pk = fp.pk[b]; w16(pk, 3); w8(pk + 7, 0x2E); for (int v = 0; v < 4; v++) pk_xy(pk, 18, v, (v & 1) * 20 - 10, (v >> 1) * 20 - 10);
    }
    queue_commit(0);
    int r; NsFxScene *s = run(0, &r);
    CHECK(r == 0 && s->n_items >= 1, "items=%u", s->n_items);
    const NsFxItem *b = &s->items[0];
    NsFxXform x = ns_fx_xform_default();
    float hr[3] = {0, 0, 1}, hu[3] = {0, 1, 0}, c[4][3];
    ns_fx_billboard_corners(b, &x, &s->cam, hr, hu, c);
    double dx = c[1][0] - c[0][0], dy = c[1][1] - c[0][1], dz = c[1][2] - c[0][2];
    CHECK(NEAR(sqrt(dx * dx + dy * dy + dz * dz), (b->off[2] - b->off[0]) * 0.001, 1e-5) && NEAR(dz, (b->off[2] - b->off[0]) * 0.001, 1e-5), "el borde superior sigue el eje derecho de la cabeza");
    double vx = c[2][0] - c[0][0], vy = c[2][1] - c[0][1], vz = c[2][2] - c[0][2];
    CHECK(NEAR(vz, 0, 1e-6) && NEAR(vx, 0, 1e-6) && NEAR(fabs(vy), fabs(b->off[3] - b->off[1]) * 0.001, 1e-5), "el borde izquierdo sigue el eje vertical de la cabeza");
    { float c2[4][3]; ns_fx_billboard_corners(b, &x, &s->cam, hr, hu, c2); CHECK(memcmp(c, c2, sizeof c) == 0, "misma orientación en ambas llamadas (una por fotograma)"); }
    int found = 0;
    for (uint32_t i = 0; i < s->n_items; i++) {
        const NsFxItem *p = &s->items[i];
        if (!(p->flags & NSFX_F_PXSIZE)) continue;
        found++;
        CHECK(isfinite(p->off[0]) && p->off[2] - p->off[0] > 0.0f && p->off[2] - p->off[0] < 5.0f, "tamaño en píxeles con z diminuta: el de su profundidad real, sin inflar a una de referencia (%.2f)", p->off[2] - p->off[0]);
    }
    CHECK(found >= 1, "elemento de tamaño en píxeles");
    ns_fx_scene_destroy(s);
}

/* M4/M5/M3: líneas degeneradas y polígonos con menos de 3 vértices no generan basura en el índice. */
static void test_degenerate(void) {
    reset(); memset(LASTNODE, 0, sizeof LASTNODE);
    int t0[3] = {0, 0, 0};
    TPrim ln = mkprim(3, 0, 4, 4096, t0, 0);   /* líneas */
    for (int j = 0; j < 4; j++) { put_pos(&ln, j * 2, 100, 100, 2000); put_pos(&ln, j * 2 + 1, 100, 100, 2000); for (int b = 0; b < 2; b++) { uint32_t pk = ln.pk[b] + (uint32_t)(PSZ[3] * j); w16(pk, 50); w8(pk + 7, 0x40); } }
    queue_commit(0);
    int r; NsFxScene *s = run(0, &r);
    GpuOp o[1]; GpuDrawList dl; memset(&dl, 0, sizeof dl); dl.ops = o; dl.nops = 1; dl.disp.w = 320; dl.disp.h = 240;
    memset(o, 0, sizeof o); o[0].type = GPU_OP_LINE; o[0].nv = 2;
    uint8_t c[1];
    CHECK(r == 0 && ns_fx_classify(RAM, s, &dl, c, NULL, NULL, NULL) == 0 && c[0] != NSFX_OP_MESH, "líneas degeneradas: sin casar con basura (%d)", c[0]);
    ns_fx_scene_destroy(s);
}


/* Clase 23 (formato libre) y paquetes propios: la forma sale del comando del paquete; se reconstruyen a la profundidad del paquete y casan exactos con la GPU. */
static TPrim mkfree(int n, int psz, int flags) {
    int t0[3] = {0, 0, 0};
    TPrim p = mkprim(23, flags, n, 4096, t0, 0);
    p.pk[0] = alloc((uint32_t)(psz * n)); p.pk[1] = alloc((uint32_t)(psz * n));
    w16(p.addr + 0x30, psz); w16(p.addr + 0x32, 4); w16(p.addr + 0x34, 8); w16(p.addr + 0x36, 8);
    w32(p.addr + 0x40, p.pk[0]); w32(p.addr + 0x44, p.pk[1]);
    return p;
}
static void test_free_kind(void) {
    reset(); memset(LASTNODE, 0, sizeof LASTNODE);
    TPrim f = mkfree(6, 52, 0);
    const int SZ = 1500, PS = 52;
    static const int xs[4] = {-40, 30, -35, 40}, ys[4] = {-30, -28, 25, 33};
    for (int b = 0; b < 2; b++) {
        uint32_t base = f.pk[b];
        for (int j = 0; j < 6; j++) { uint32_t pk = base + (uint32_t)(PS * j); w16(pk, SZ); }
        uint32_t pk;
        /* 0: cuadrilátero texturizado semitransparente (0x2E): xy@8 uvclut@12 xy@16 uvtpage@20 ... */
        pk = base; w8(pk + 7, 0x2E);
        for (int v = 0; v < 4; v++) { w16(pk + 8 + 8u * (uint32_t)v, xs[v]); w16(pk + 10 + 8u * (uint32_t)v, ys[v]); w8(pk + 12 + 8u * (uint32_t)v, 10 + v); w8(pk + 13 + 8u * (uint32_t)v, 20 + v); }
        w16(pk + 14, 0x7C10); w16(pk + 22, 0x1A | (1 << 5) | (1 << 7));
        /* 1: triángulo plano (0x20) */
        pk = base + (uint32_t)PS; w8(pk + 7, 0x20); w8(pk + 4, 90); w8(pk + 5, 91); w8(pk + 6, 92);
        for (int v = 0; v < 3; v++) { w16(pk + 8 + 4u * (uint32_t)v, xs[v] + 3); w16(pk + 10 + 4u * (uint32_t)v, ys[v] + 3); }
        /* 2: cuadrilátero degradado (0x38) */
        pk = base + (uint32_t)(2 * PS); w8(pk + 7, 0x38);
        for (int v = 0; v < 4; v++) { w16(pk + 8 + 8u * (uint32_t)v, xs[v] - 5); w16(pk + 10 + 8u * (uint32_t)v, ys[v] - 5); }
        w8(pk + 4, 10); w8(pk + 12, 40); w8(pk + 20, 80); w8(pk + 28, 120);
        /* 3: línea plana (0x42, semitransparente) */
        pk = base + (uint32_t)(3 * PS); w8(pk + 7, 0x42); w8(pk + 4, 200);
        w16(pk + 8, -20); w16(pk + 10, -10); w16(pk + 12, 25); w16(pk + 14, 12);
        /* 4: sprite 8x8 (0x76) */
        pk = base + (uint32_t)(4 * PS); w8(pk + 7, 0x76); w16(pk + 8, 5); w16(pk + 10, -6); w8(pk + 12, 32); w8(pk + 13, 48); w16(pk + 14, 0x7C20);
        /* 5: rectángulo de tamaño libre (0x62) */
        pk = base + (uint32_t)(5 * PS); w8(pk + 7, 0x62); w16(pk + 8, -10); w16(pk + 10, 4); w16(pk + 12, 14); w16(pk + 14, 6);
    }
    queue_commit(0);
    int r; NsFxScene *s = run(0, &r);
    CHECK(r == 0 && s->n_items == 6 && s->stats.n_free == 1, "clase 23: items=%u libres=%u", s->n_items, s->stats.n_free);
    if (s->n_items == 6) {
        static const int shape[6] = {NSFX_SH_QUAD, NSFX_SH_TRI, NSFX_SH_QUAD, NSFX_SH_RIBBON, NSFX_SH_BILLBOARD, NSFX_SH_BILLBOARD};
        for (int k = 0; k < 6; k++) CHECK(s->items[k].shape == shape[k] && s->items[k].kind == 23, "elemento %d forma=%d clase=%d", k, s->items[k].shape, s->items[k].kind);
        const NsFxItem *q = &s->items[0];
        CHECK((q->flags & NSFX_F_TEXTURED) && (q->flags & NSFX_F_SEMI) && q->abr == 1 && q->tp == 1 && q->clut == 0x7C10 && q->uv[2][0] == 12 && q->uv[2][1] == 22, "textura del cuadrilátero libre (abr=%d tp=%d clut=%04x)", q->abr, q->tp, q->clut);
        CHECK(!(s->items[1].flags & NSFX_F_TEXTURED) && s->items[1].rgb[0][0] == 90, "triángulo plano con su color");
        CHECK((s->items[2].flags & NSFX_F_GOURAUD) && s->items[2].rgb[0][0] == 10 && s->items[2].rgb[1][0] == 40 && s->items[2].rgb[2][0] == 80 && s->items[2].rgb[3][0] == 120, "degradado: un color por vértice");
        CHECK((s->items[3].flags & NSFX_F_SEMI) && s->items[3].width[0] > 0 && s->items[3].pcls == 0, "línea libre: cinta");
        CHECK((s->items[4].flags & NSFX_F_TEXTURED) && (s->items[4].flags & NSFX_F_TPAGE_UNKNOWN) && s->items[4].sw == 8 && s->items[4].sh == 8, "sprite 8x8 libre");
        CHECK(!(s->items[5].flags & NSFX_F_TEXTURED) && s->items[5].sw == 14 && s->items[5].sh == 6, "rectángulo libre de tamaño del paquete");
        /* reproyección: la posición de mundo reconstruida vuelve a las coordenadas del paquete (factor vertical incluido) */
        for (int v = 0; v < 4; v++) {
            double w[3] = {q->pos[v][0], q->pos[v][1], q->pos[v][2]}, sx, sy, z;
            CHECK(ref_proj(w, &sx, &sy, &z) && NEAR(sx, xs[v], 0.6) && NEAR(sy, ys[v], 0.6) && NEAR(z, SZ, 1.0), "reproyección v%d: (%.2f %.2f) esperado (%d %d)", v, sx, sy, xs[v], ys[v]);
        }
        /* casación exacta con la GPU: el cuadrilátero libre y el triángulo plano salen como FX */
        GpuOp o[3]; GpuDrawList dl; memset(&dl, 0, sizeof dl); memset(o, 0, sizeof o); dl.ops = o; dl.nops = 3; dl.disp.w = 320; dl.disp.h = 240;
        o[0].type = GPU_OP_POLY; o[0].nv = 4; o[0].flags = GPU_F_TEXTURED | GPU_F_SEMI | GPU_F_QUAD; o[0].tp = 1;
        for (int v = 0; v < 4; v++) { o[0].v[v].x = (int16_t)xs[v]; o[0].v[v].y = (int16_t)ys[v]; o[0].v[v].u = (uint8_t)(10 + v); o[0].v[v].v = (uint8_t)(20 + v); }
        o[1].type = GPU_OP_POLY; o[1].nv = 3;
        for (int v = 0; v < 3; v++) { o[1].v[v].x = (int16_t)(xs[v] + 3); o[1].v[v].y = (int16_t)(ys[v] + 3); o[1].v[v].r = 90; o[1].v[v].g = 91; o[1].v[v].b = 92; }
        o[2].type = GPU_OP_LINE; o[2].nv = 2; o[2].flags = GPU_F_SEMI; o[2].v[0].x = -20; o[2].v[0].y = -10; o[2].v[1].x = 25; o[2].v[1].y = 12; o[2].v[0].r = 200;
        uint8_t c[3]; int32_t it[3];
        CHECK(ns_fx_classify(RAM, s, &dl, c, NULL, it, NULL) == 0 && c[0] == NSFX_OP_FX && it[0] == 0 && c[1] == NSFX_OP_FX && it[1] == 1 && c[2] == NSFX_OP_FX && it[2] == 3,
              "casan exactos: %d/%d/%d items %d/%d/%d", c[0], c[1], c[2], it[0], it[1], it[2]);
    }
    ns_fx_scene_destroy(s);
    /* comando que no dibuja (ni 0): sin elemento; polilínea con terminador: sin elemento; paquete sin profundidad: no emitido */
    reset(); memset(LASTNODE, 0, sizeof LASTNODE);
    TPrim g = mkfree(3, 52, 0);
    for (int b = 0; b < 2; b++) {
        w16(g.pk[b], 1500); w8(g.pk[b] + 7, 0x00);
        w16(g.pk[b] + 52, 1500); w8(g.pk[b] + 52 + 7, 0x4A);
        w16(g.pk[b] + 104, 0); w8(g.pk[b] + 104 + 7, 0x2C);
    }
    queue_commit(0);
    s = run(0, &r);
    CHECK(r == 0 && s->n_items == 0, "paquetes libres sin dibujo interpretable: %u elementos", s->n_items);
    ns_fx_scene_destroy(s);
    /* línea con la bandera de paquetes propios en una clase normal de línea: cinta en el plano del paquete */
    reset(); memset(LASTNODE, 0, sizeof LASTNODE);
    int t0[3] = {0, 0, 0};
    TPrim ln = mkprim(3, 0x2000, 1, 4096, t0, 0);
    for (int b = 0; b < 2; b++) { uint32_t pk = ln.pk[b]; w16(pk, 800); w8(pk + 7, 0x52); pk_xy(pk, 3, 0, -30, -5); pk_xy(pk, 3, 1, 44, 17); w8(pk + 4, 30); w8(pk + 12, 60); }
    queue_commit(0);
    s = run(0, &r);
    CHECK(r == 0 && s->n_items == 1 && s->items[0].shape == NSFX_SH_RIBBON, "línea de paquetes propios: items=%u", s->n_items);
    if (s->n_items == 1) {
        for (int e = 0; e < 2; e++) { double w[3] = {s->items[0].pos[e][0], s->items[0].pos[e][1], s->items[0].pos[e][2]}, sx, sy, z; ref_proj(w, &sx, &sy, &z); CHECK(NEAR(sx, e ? 44 : -30, 0.6) && NEAR(sy, e ? 17 : -5, 0.6), "extremo %d (%.2f %.2f)", e, sx, sy); }
        CHECK(s->items[0].rgb[0][0] == 30 && s->items[0].rgb[1][0] == 60, "colores de los extremos");
    }
    ns_fx_scene_destroy(s);
}

/* casación por pantalla: destello grande (vértices saturados, color animado y 1-2 px de desfase) -> FX; el HUD (sin posición de vista) y lo distinto, no */
static void test_classify_screen(void) {
    NsFxScene *sc = ns_fx_scene_create(4);
    if (!sc) { CHECK(0, "escena"); return; }
    sc->cam.y_scale = 1.0f; sc->n_items = 1;
    NsFxItem *it = &sc->items[0]; memset(it, 0, sizeof *it);
    it->shape = NSFX_SH_BILLBOARD; it->nv = 4; it->flags = NSFX_F_SEMI | NSFX_F_TEXTURED; it->abr = 1; it->tpage = 0x1A | (1 << 5) | (1 << 7); it->tp = 1; it->ptp = 1; it->clut = 0x3c70;
    it->pcls = 2; it->pnv = 4; it->pu0 = 64; it->pv0 = 96; it->sz = 5000; it->rgb[0][0] = it->rgb[0][1] = it->rgb[0][2] = 72;
    static const int16_t sx[4] = {-694, -607, -694, -607}, sy[4] = {-141, -141, -54, -54};
    for (int v = 0; v < 4; v++) { it->sxy[v][0] = sx[v]; it->sxy[v][1] = sy[v]; }
    GpuOp ops[3]; memset(ops, 0, sizeof ops);
    GpuDrawList dl; memset(&dl, 0, sizeof dl); dl.ops = ops; dl.nops = 3;
    for (int k = 0; k < 3; k++) {
        GpuOp *o = &ops[k];
        o->type = GPU_OP_POLY; o->nv = 4; o->flags = GPU_F_TEXTURED | GPU_F_SEMI | GPU_F_QUAD; o->abr = 1; o->tp = 1; o->tx = 640; o->ty = 256; o->dox = 160; o->doy = 128;
        for (int v = 0; v < 4; v++) {
            o->v[v].x = (int16_t)(sx[v] + 160 + (k == 0 ? 1 : 0)); o->v[v].y = (int16_t)(sy[v] + 128 + (k == 0 ? -1 : 0));
            o->v[v].u = (uint8_t)(64 + 31 * (v & 1)); o->v[v].v = (uint8_t)(96 + 31 * (v >> 1)); o->v[v].r = o->v[v].g = o->v[v].b = 64;
            o->v[v].vvalid = k != 1; o->v[v].vx = (v & 1) ? 1000.0f : -1000.0f; o->v[v].vy = (v >> 1) ? 1000.0f : -1000.0f; o->v[v].vz = 3656.0f; o->v[v].vflags = v == 1 || v == 2 ? GPU_VF_DERIVED : 0;
        }
    }
    ops[2].v[0].x = (int16_t)(ops[2].v[0].x + 40);   /* otra posición: no es el mismo paquete */
    uint8_t cls[3] = {NSFX_OP_2D, NSFX_OP_2D, NSFX_OP_2D}; int32_t item[3] = {-1, -1, -1};
    uint32_t ch = ns_fx_classify_near(sc, NULL, &dl, cls, item);
    CHECK(ch == 1 && cls[0] == NSFX_OP_FX && item[0] == 0, "destello por pantalla: cls=%d item=%d cambiadas=%u", cls[0], item[0], ch);
    CHECK(cls[1] == NSFX_OP_2D, "sin posición de vista (HUD): 2D");
    CHECK(cls[2] == NSFX_OP_2D, "elemento ya reclamado / otra posición: 2D");
    ns_fx_scene_destroy(sc);
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

/* casación tolerante: por posición (elemento movido pocas unidades) y por familia (movido mucho, vértices saturados); nunca sin vista (HUD) */
static void test_classify_near(void) {
    NsFxScene *sc = ns_fx_scene_create(8);
    CHECK(sc != NULL, "escena");
    if (!sc) return;
    sc->cam.y_scale = 1.0f;
    sc->n_items = 2;
    for (int k = 0; k < 2; k++) {
        NsFxItem *it = &sc->items[k];
        memset(it, 0, sizeof *it);
        it->shape = NSFX_SH_QUAD; it->nv = 4; it->flags = NSFX_F_SEMI | NSFX_F_TEXTURED; it->abr = 1;
        it->tpage = 0x15 | (1 << 5); it->clut = 0x3d7e; it->prim = 0x80100000u + (uint32_t)k * 64; it->idx = (uint16_t)k;
        for (int v = 0; v < 4; v++) { float x = (float)(k * 5000 + (v & 1) * 100), y = 0, z = 1000.0f + (float)(v >> 1) * 100; it->vpos[v][0] = x; it->vpos[v][1] = y; it->vpos[v][2] = z; }
    }
    GpuOp ops[3];
    memset(ops, 0, sizeof ops);
    GpuDrawList dl;
    memset(&dl, 0, sizeof dl);
    dl.ops = ops; dl.nops = 3;
    uint8_t cls[3] = {NSFX_OP_2D, NSFX_OP_2D, NSFX_OP_2D};
    int32_t item[3] = {-1, -1, -1};
    for (int i = 0; i < 3; i++) {
        GpuOp *o = &ops[i];
        o->type = GPU_OP_POLY; o->nv = 4; o->flags = GPU_F_TEXTURED | GPU_F_SEMI; o->abr = 1; o->tx = 5 * 64; o->ty = 256; o->tp = 0; o->cly = 0x3d7e >> 6;
        o->clx = (0x3d7e & 63) * 16;
        for (int v = 0; v < 4; v++) { o->v[v].vvalid = i < 2; o->v[v].vx = (float)(i * 5000 + (v & 1) * 100) + (i == 0 ? 10.0f : 0.0f); o->v[v].vy = 0; o->v[v].vz = 1000.0f + (float)(v >> 1) * 100; }
    }
    /* op 1: saturado (ninguna esquina comparable) y movido: solo por familia; op 2: sin vista (HUD): jamás */
    for (int v = 0; v < 4; v++) { ops[1].v[v].vflags = GPU_VF_AMBIG; ops[1].v[v].vx += 4000.0f; }
    uint32_t ch = ns_fx_classify_near(sc, NULL, &dl, cls, item);
    CHECK(ch == 2, "cambiadas=%u", ch);
    CHECK(cls[0] == NSFX_OP_FX && item[0] == 0, "por posición cls=%d item=%d", cls[0], item[0]);
    CHECK(cls[1] == NSFX_OP_FX && item[1] == 1, "por familia cls=%d item=%d", cls[1], item[1]);
    CHECK(cls[2] == NSFX_OP_2D && item[2] == -1, "sin vista queda 2D");
    CHECK(ns_fx_item_key(&sc->items[0]) != ns_fx_item_key(&sc->items[1]) && ns_fx_item_key(&sc->items[0]) != 0, "claves distintas y no nulas");
    ns_fx_scene_destroy(sc);
}


/* Tamaño en mundo de los sprites de tamaño en píxeles: el de la z REAL del motor al emitirse, aprendido por clave; sin emisión no se inventa una profundidad */
static void spr_packet(const TPrim *sp, int sz, double wx, double wy, double wz) {
    double w[3] = {wx, wy, wz}, sx, sy, z; ref_proj(w, &sx, &sy, &z);
    for (int b = 0; b < 2; b++) { uint32_t pk = sp->pk[b]; w16(pk, sz); w8(pk + 7, 0x66); w16(pk + 8, (int)lround(sx) - 8); w16(pk + 10, (int)lround(sy) - 8); w8(pk + 12, 16); w8(pk + 13, 32); w16(pk + 14, 0x1234); w16(pk + 16, 16); w16(pk + 18, 16); }
}
static void test_learned_size(void) {
    reset(); ns_fx_learn_reset();
    int t0[3] = {0, 0, 0};
    TPrim sp = mkprim(6, 0x400, 1, 4096, t0, 0), sp2 = mkprim(6, 0x400, 1, 4096, t0, 0);
    put_pos(&sp, 0, 900, 300, 1200); put_rect(&sp, 8, 8, 16, 16);
    put_pos(&sp2, 0, -900, 300, 1200); put_rect(&sp2, 8, 8, 16, 16);
    spr_packet(&sp, 5, 900, 300, 1200);          /* emitido */
    spr_packet(&sp2, 0, -900, 300, 1200);        /* NO emitido y nunca visto */
    queue_commit(0);
    int r; NsFxScene *s = run(0, &r);
    const NsFxItem *a = NULL, *b2 = NULL;
    for (uint32_t i = 0; i < s->n_items; i++) { if (s->items[i].prim == sp.addr) a = &s->items[i]; if (s->items[i].prim == sp2.addr) b2 = &s->items[i]; }
    CHECK(r == 0 && a && b2, "dos sprites extraídos");
    if (!a || !b2) { ns_fx_scene_destroy(s); return; }
    double w[3] = {900, 300, 1200}, v[3]; ref_view(w, v);
    float w1 = a->off[2] - a->off[0];
    CHECK(!(a->flags & NSFX_F_NOSIZE) && !(a->flags & NSFX_F_ENGINE_CULLED) && NEAR(w1, 16 * v[2] / G_H, 0.5), "emitido: ancho en mundo = px * z / H con la z real (%.2f vs %.2f)", w1, 16 * v[2] / G_H);
    CHECK((b2->flags & NSFX_F_ENGINE_CULLED) && (b2->flags & NSFX_F_NOSIZE), "no emitido y sin tamaño aprendido: NOSIZE (flags 0x%X)", b2->flags);
    for (uint32_t i = 0; i < s->n_items; i++) s->items[i].flags &= (uint16_t)~NSFX_F_TPAGE_UNKNOWN;   /* la página no es lo que se prueba */
    { NsEffect ef[8]; int tr; uint32_t n = ns_fx_export(s, 1, ef, 8, &tr); int has2 = 0;
      for (uint32_t i = 0; i < n; i++) if (ef[i].src_key == sp2.addr) has2 = 1;
      CHECK(!has2, "el sprite sin tamaño no se exporta (nunca se infla con una profundidad de referencia)"); }
    ns_fx_scene_destroy(s);
    /* siguiente fotograma: el motor ya no lo emite (sz = 0) y la cámara/posición dan otra z: el tamaño en mundo sigue siendo el aprendido */
    put_pos(&sp, 0, 900, 300, 3600); spr_packet(&sp, 0, 900, 300, 3600);
    queue_commit(0);
    s = run(0, &r); a = NULL;
    for (uint32_t i = 0; i < s->n_items; i++) if (s->items[i].prim == sp.addr) a = &s->items[i];
    CHECK(a && (a->flags & NSFX_F_ENGINE_CULLED) && !(a->flags & NSFX_F_NOSIZE) && NEAR(a->off[2] - a->off[0], w1, 1e-3), "no emitido: tamaño en mundo aprendido de su emisión (%.2f vs %.2f)", a ? a->off[2] - a->off[0] : -1.0, w1);
    for (uint32_t i = 0; i < s->n_items; i++) s->items[i].flags &= (uint16_t)~NSFX_F_TPAGE_UNKNOWN;
    { NsEffect ef[8]; int tr; uint32_t n = ns_fx_export(s, 1, ef, 8, &tr), ok = 0;
      for (uint32_t i = 0; i < n; i++) if (ef[i].src_key == sp.addr && (ef[i].flags & NS_EF_WORLD_SIZE) && NEAR(ef[i].box[2] - ef[i].box[0], w1, 1e-3)) ok = 1;
      CHECK(ok, "exportado con el tamaño aprendido y NS_EF_WORLD_SIZE"); }
    ns_fx_scene_destroy(s);
    /* tras reiniciar el aprendizaje ya no hay tamaño */
    ns_fx_learn_reset(); s = run(0, &r); a = NULL;
    for (uint32_t i = 0; i < s->n_items; i++) if (s->items[i].prim == sp.addr) a = &s->items[i];
    CHECK(a && (a->flags & NSFX_F_NOSIZE), "sin aprendizaje: NOSIZE");
    ns_fx_scene_destroy(s);
}

int main(void) {
    install_test_table();
    RAM = (uint8_t *)calloc(NSFX_RAM_SIZE, 1);
    test_polygon_and_billboard();
    test_filters_and_oneside();
    test_map_group();
    test_covers_packet();
    test_classify_by_address();
    test_lines_and_oncamera();
    test_actors_and_screen();
    test_classify();
    test_learn();
    test_xform();
    test_stays_2d();
    test_sortonly_wins();
    test_gpu_mask();
    test_fuzz();
    test_gas_limits();
    test_perf();
    test_null_ram();
    test_head_and_px();
    test_degenerate();
    test_garbage();
    test_classify_near();
    test_free_kind();
    test_classify_screen();
    test_learned_size();
    free(RAM);
    printf("%s: %d comprobaciones, %d fallos\n", g_fail ? "FALLA" : "OK", g_checks, g_fail);
    return g_fail != 0;
}
