/* Pruebas sintéticas de ns_solid (geometría sólida de la escena nativa para la colisión de la cabeza): RAM construida a mano, sin datos del juego.
 * Se comprueba: qué entra (escenario y dinámicos, también DETRÁS de la cámara), qué no (jugador, semitransparentes, ocultos), el recorte por radio, las soldaduras,
 * el filtro de articulaciones solo para dinámicos, la malla ausente y el búfer lleno; y de extremo a extremo: la cabeza contra paredes de la escena extraída en 6 direcciones. */
#include "ns_extract.h"
#include "ns_solid.h"
#include "vr_headcol.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: %s ", __FILE__, __LINE__, #c); printf("" __VA_ARGS__); printf("\n"); } } while (0)

static uint8_t *ram;
static uint32_t arena;
static void w8(uint32_t a, uint8_t v) { ram[a & 0x1FFFFF] = v; }
static void w16(uint32_t a, uint32_t v) { ram[a & 0x1FFFFF] = (uint8_t)v; ram[(a & 0x1FFFFF) + 1] = (uint8_t)(v >> 8); }
static void w32(uint32_t a, uint32_t v) { w16(a, v & 0xFFFF); w16(a + 2, v >> 16); }
static uint32_t alloc(uint32_t n) { uint32_t a = arena; arena += (n + 15u) & ~15u; return a; }
static void wmat(uint32_t a, int s, int32_t tx, int32_t ty, int32_t tz) {
    for (int i = 0; i < 9; i++) w16(a + 2u * (uint32_t)i, (i % 4 == 0) ? (uint32_t)s : 0u);
    w32(a + 0x14, (uint32_t)tx); w32(a + 0x18, (uint32_t)ty); w32(a + 0x1C, (uint32_t)tz);
}
#define CHAN 0x800B2B7Cu
#define QUEUE 0x800B2110u
#define LIGHTG 0x8009E55Cu
typedef struct { uint32_t v, vi, n, ni, uv, tx; } Tabs;
static Tabs model(uint32_t m, uint32_t flag, int nf, int nv, int nn, int parent) {
    Tabs t;
    t.v = alloc((uint32_t)nv * 8); t.vi = alloc((uint32_t)nf * 4); t.n = alloc((uint32_t)(nn ? nn : 1) * 8); t.ni = alloc((uint32_t)nf * 4); t.uv = alloc((uint32_t)nf * 8); t.tx = alloc((uint32_t)nf * 2);
    w32(m, flag); w32(m + 4, (uint32_t)nf); w32(m + 0x2C, (uint32_t)parent); w32(m + 0x30, 0xFFFFFFFFu); w32(m + 0x34, (uint32_t)nv);
    w32(m + 0x38, t.v); w32(m + 0x3C, t.vi); w32(m + 0x40, (uint32_t)nn); w32(m + 0x44, t.n); w32(m + 0x48, t.ni); w32(m + 0x4C, t.uv); w32(m + 0x50, t.tx);
    return t;
}
static void vert(Tabs *t, int i, int x, int y, int z) { w16(t->v + 8u * (uint32_t)i, (uint32_t)x); w16(t->v + 8u * (uint32_t)i + 2, (uint32_t)y); w16(t->v + 8u * (uint32_t)i + 4, (uint32_t)z); w16(t->v + 8u * (uint32_t)i + 6, 0xFFFF); }
static void face(Tabs *t, int f, int a, int b, int c, int d) {
    w8(t->vi + 4u * (uint32_t)f, (uint8_t)a); w8(t->vi + 4u * (uint32_t)f + 1, (uint8_t)b); w8(t->vi + 4u * (uint32_t)f + 2, (uint8_t)c); w8(t->vi + 4u * (uint32_t)f + 3, (uint8_t)d);
    const uint8_t uv[8] = {0, 0, 255, 0, 255, 255, 0, 255};
    for (int k = 0; k < 8; k++) w8(t->uv + 8u * (uint32_t)f + (uint32_t)k, uv[k]);
    w16(t->tx + 2u * (uint32_t)f, 0x1234);
}
static uint32_t object(uint32_t flag, int gid, int nm, uint32_t def, uint32_t light) {
    uint32_t o = alloc(0x48 + 0x5C * (uint32_t)(nm > 0 && nm < 64 ? nm + 2 : 4));
    wmat(o, 4096, 0, 0, 0);
    w32(o + 0x24, def); w32(o + 0x28, flag); w16(o + 0x2C, (uint32_t)gid); w16(o + 0x2E, (uint32_t)nm); w32(o + 0x34, light);
    return o;
}
static uint32_t piece(uint32_t o, int k) { return o + 0x48 + 0x5Cu * (uint32_t)k; }
static void queue_set(int n, const uint32_t *objs) { w16(CHAN + 0x56, (uint32_t)n); w32(CHAN + 0x58, QUEUE); for (int i = 0; i < n; i++) w32(QUEUE + 4u * (uint32_t)i, objs[i]); }

/* objeto de UNA pieza con un cuadrado grande en el plano perpendicular a `axis` a `dist` del origen (x,y,z en unidades del motor), lado 2*half */
static uint32_t wall_obj(uint32_t flag, int axis, int dist, int half, int32_t tx, int32_t ty, int32_t tz, uint32_t model_flag) {
    uint32_t def = alloc(0x20 + 0x58); w32(def, 1); w32(def + 4, 1);
    uint32_t o = object(flag, 1, 1, def, flag & 0x8 ? 0 : LIGHTG);
    Tabs t = model(def + 0x20, model_flag, 1, 4, 1, -1);
    int p[4][3];
    int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
    for (int k = 0; k < 4; k++) { p[k][axis] = dist; p[k][a1] = (k == 0 || k == 3) ? -half : half; p[k][a2] = (k == 0 || k == 1) ? -half : half; }
    for (int k = 0; k < 4; k++) vert(&t, k, p[k][0], p[k][1], p[k][2]);
    w16(t.n, 0); w16(t.n + 2, 4096); w16(t.n + 4, 0);
    face(&t, 0, 0, 1, 2, 3);
    w32(piece(o, 0) + 0x40, def + 0x20); wmat(piece(o, 0), 4096, tx, ty, tz);
    return o;
}
static void base(void) {
    memset(ram, 0, NS_RAM_SIZE);
    arena = 0x80130000u;
    uint32_t te = 0x800B30E0u + 12u * (0x1234u % 512u);
    w16(te, 0x1234); w16(te + 4, 0x0105); w16(te + 6, 0x7800); w8(te + 8, 16); w8(te + 9, 32); w8(te + 10, 31); w8(te + 11, 15);
    w16(LIGHTG + 2, (uint32_t)-4096 & 0xFFFF);
    w16(LIGHTG + 32, 560); w16(LIGHTG + 38, 560); w16(LIGHTG + 44, 560);
    for (int c = 0; c < 3; c++) w16(0x800AC530u + 2u * (uint32_t)c, 51);
    w32(0x800ACAF8u, 1);
    wmat(CHAN + 0x30, 4096, 0, 0, 0);
    w16(CHAN + 0x50, 320);
}
static NsScene sc; static NsMeshStore *st;
static int extract(void) {
    ns_store_destroy(st); st = ns_store_create();
    NsExtractStats es;
    return ns_extract_ex(&sc, st, ram, NS_RAM_SIZE, NULL, 0, NULL, &es);
}
#define OF_MAP 0x57u    /* mapa: texturizado|precalculado|translúcido|una matriz (como las pruebas del extractor) */
#define OF_DYN 0x1u

static void test_collect(void) {
    base();
    uint32_t o_map = wall_obj(OF_MAP, 0, 1000, 3000, 0, 0, 0, 0x201);     /* escenario opaco */
    uint32_t o_dyn = wall_obj(OF_DYN, 2, -1000, 500, 0, 0, 0, 0x1);       /* dinámico, DETRÁS (z negativo) */
    uint32_t o_semi = wall_obj(OF_DYN, 1, -500, 500, 0, 0, 0, 0x2);       /* modelo semitransparente puro (0x2 = NS_MF_BLEND sin 0x200) */
    uint32_t o_hide = wall_obj(OF_MAP | 0x80, 0, -1000, 500, 0, 0, 0, 0x201);   /* oculto por el juego */
    uint32_t o_far = wall_obj(OF_DYN, 0, 9000, 500, 0, 0, 0, 0x1);        /* lejos */
    uint32_t q[8] = {o_map, o_dyn, o_semi, o_hide, o_far};
    queue_set(5, q);
    CHECK(extract() == 0 && sc.n_inst >= 3, "extrae (n_inst=%u)", sc.n_inst);
    size_t cap = 4096;
    float *tris = malloc(cap * 36); NsSolidSrc *src = malloc(cap * sizeof *src);
    NsSolidStats S;
    NsSolidOpts o; memset(&o, 0, sizeof o);
    size_t n = ns_solid_collect(&sc, st, &o, tris, src, cap, &S);
    int nstat = 0, ndyn = 0;
    for (size_t i = 0; i < n; i++) { if (src[i].is_static) nstat++; else ndyn++; }
    CHECK(S.tris_static == (unsigned)nstat && S.tris_dyn == (unsigned)ndyn && (size_t)(nstat + ndyn) == n, "recuento coherente");
    CHECK(nstat == 2, "el cuadrado del escenario son 2 triángulos (%d)", nstat);
    CHECK(ndyn == 4, "dinámicos: el de atrás y el lejano (2 triángulos cada uno) = 4; el semitransparente y el oculto, no (%d) semi_piezas=%u", ndyn, S.pieces_blend);
    CHECK(S.pieces_no_mesh == 0 && S.overflow == 0, "sin ausentes ni desbordes");
    /* triángulos de la pared del mapa: x = 1000 */
    int ok_x = 0; for (size_t i = 0; i < n; i++) if (src[i].is_static) { for (int v = 0; v < 3; v++) if (fabsf(tris[9 * i + 3 * v] - 1000.0f) < 0.5f) ok_x++; }
    CHECK(ok_x == 6, "la pared del escenario está en x=1000 (%d)", ok_x);
    /* DETRÁS: el dinámico en z=-1000 está presente aunque la cámara mire a +z */
    int back = 0; for (size_t i = 0; i < n; i++) if (!src[i].is_static && tris[9 * i + 2] < -900.0f && tris[9 * i + 2] > -1100.0f) back++;
    CHECK(back >= 2, "geometría detrás de la cámara incluida (%d)", back);
    /* incluir semitransparentes */
    o.include_blend = 1;
    size_t n2 = ns_solid_collect(&sc, st, &o, tris, NULL, cap, &S);
    CHECK(n2 == n + 2, "con include_blend entra también la malla semitransparente (%zu vs %zu)", n2, n);
    o.include_blend = 0;
    /* jugador omitido */
    o.skip_key = 0x80000000u | (o_dyn & 0x1FFFFFFFu);
    size_t n3 = ns_solid_collect(&sc, st, &o, tris, NULL, cap, &S);
    CHECK(n3 == n - 2 && S.inst_skipped == 1, "la instancia del jugador se omite (%zu vs %zu)", n3, n);
    o.skip_key = 0;
    /* radio: solo lo cercano al centro */
    o.center[0] = 0; o.center[1] = 0; o.center[2] = 0; o.radius = 3000;
    size_t n4 = ns_solid_collect(&sc, st, &o, tris, NULL, cap, &S);
    CHECK(n4 == n - 2 && S.pieces_culled >= 1, "el objeto lejano queda fuera del radio (%zu vs %zu, descartadas=%u)", n4, n, S.pieces_culled);
    /* articulaciones: filtran dinámicos pequeños, nunca el escenario */
    float j[1][3] = {{0, 0, -1000}};
    o.radius = 0; o.joints = (const float (*)[3])j; o.n_joints = 1; o.body_units = 1500;
    size_t n5 = ns_solid_collect(&sc, st, &o, tris, NULL, cap, &S);
    CHECK(S.tris_body == 2 && S.tris_static == 2 && n5 == n - 2, "el cuerpo cerca de una articulación se ignora, el escenario no (cuerpo=%u estatico=%u)", S.tris_body, S.tris_static);
    /* búfer lleno */
    o.n_joints = 0;
    size_t n6 = ns_solid_collect(&sc, st, &o, tris, NULL, 3, &S);
    CHECK(n6 == 3 && S.overflow == n - 3, "búfer lleno: se cuenta lo que no cupo (%u)", S.overflow);
    free(tris); free(src);
}

static void test_missing_mesh(void) {
    /* un modelo con el puntero fuera de RAM en el ESCENARIO no puede pasar por escena completa: MAP_INCOMPLETE => la decisión cae a la ruta GPU */
    base();
    uint32_t def = alloc(0x20 + 0x58); w32(def, 1); w32(def + 4, 1);
    uint32_t o_bad = object(OF_MAP, 1, 1, def, LIGHTG);
    w32(piece(o_bad, 0) + 0x40, 0x80F00000u); wmat(piece(o_bad, 0), 4096, 0, 0, 0);   /* modelo fuera de RAM */
    uint32_t o_ok = wall_obj(OF_MAP, 0, 1000, 3000, 0, 0, 0, 0x201);
    uint32_t q[2] = {o_bad, o_ok};
    queue_set(2, q);
    CHECK(extract() == 0, "extrae");
    CHECK(sc.valid & NS_SV_MAP_INCOMPLETE, "modelo del escenario ilegible => MAP_INCOMPLETE (valid=0x%x)", sc.valid);
    /* un objeto DINÁMICO con modelo ilegible no marca el mapa incompleto */
    base();
    def = alloc(0x20 + 0x58); w32(def, 1); w32(def + 4, 1);
    uint32_t o_dbad = object(OF_DYN, 1, 1, def, 0);
    w32(piece(o_dbad, 0) + 0x40, 0x80F00000u); wmat(piece(o_dbad, 0), 4096, 0, 0, 0);
    o_ok = wall_obj(OF_MAP, 0, 1000, 3000, 0, 0, 0, 0x201);
    uint32_t q2[2] = {o_dbad, o_ok};
    queue_set(2, q2);
    CHECK(extract() == 0 && !(sc.valid & NS_SV_MAP_INCOMPLETE), "dinámico ilegible: el escenario sigue completo");
}

/* ---- extremo a extremo: habitación cerrada en la escena nativa y la cabeza empujando contra cada pared ---- */
static void test_head_vs_native_room(void) {
    base();
    /* habitación de 2 m (±1000) x 2,6 m: seis paredes del escenario (cada una un objeto de una pieza) */
    uint32_t objs[6];
    const int dist[6] = {1000, -1000, 1300, -1300, 1000, -1000};
    for (int d = 0; d < 6; d++) objs[d] = wall_obj(OF_MAP, d / 2, dist[d], 3000, 0, 0, 0, 0x201);
    queue_set(6, objs);
    CHECK(extract() == 0 && sc.n_inst == 6, "habitación extraída (n_inst=%u)", sc.n_inst);
    size_t cap = 4096; float *tris = malloc(cap * 36);
    NsSolidOpts o; memset(&o, 0, sizeof o); NsSolidStats S;
    size_t n = ns_solid_collect(&sc, st, &o, tris, NULL, cap, &S);
    CHECK(n == 12 && S.tris_static == 12, "12 triángulos de escenario (%zu)", n);
    VrCamRecord r; memset(&r, 0, sizeof r);
    r.active = 1; r.theta_play_deg = -180.0f; r.units_per_m = 1000.0f;     /* R = identidad: usuario = mundo/1000 */
    r.eye[0] = 0; r.eye[1] = 0; r.eye[2] = 0;
    r.p_ref[0] = 0; r.p_ref[1] = 0; r.p_ref[2] = 0;
    VrHcMap m; vrhc_map_from_record(&m, &r);
    const char *nm[6] = {"+x", "-x", "+y", "-y", "+z", "-z"};
    for (int d = 0; d < 6; d++) {
        vrhc_init(NULL);
        const VrHcCfg *cf = vrhc_config();
        VrHcOut ho;
        vrhc_feed_world(tris, (int)n, 0);
        float head[3] = {0, 0, 0};
        vrhc_query(&m, head, 0.011f, &ho);
        CHECK(ho.fade == 0 && ho.corr_len < 1e-3f, "%s: en el centro libre", nm[d]);
        float sgn = (d & 1) ? -1.0f : 1.0f; int ax = d / 2;
        float wall = dist[d] / 1000.0f;
        float pushes[3] = {0.5f * wall, 0.95f * wall, 2.0f * wall};
        for (int k = 0; k < 3; k++) {
            vrhc_feed_world(tris, (int)n, 0);
            float hh[3] = {0, 0, 0}; hh[ax] = sgn * fabsf(pushes[k]);
            vrhc_query(&m, hh, 0.011f, &ho);
            float fin = fabsf(hh[ax] + ho.corr[ax]);
            CHECK(fin <= fabsf(wall) - cf->radius_m + 0.006f, "%s empuje %.2f m: la cabeza queda a %.3f m del centro (pared a %.2f, radio %.2f)", nm[d], pushes[k], fin, fabsf(wall), cf->radius_m);
            CHECK(ho.clear_m >= cf->face_m - 0.006f, "%s empuje %.2f m: cara a %.3f m de un sólido (min %.2f)", nm[d], pushes[k], ho.clear_m, cf->face_m);
            if (k == 2) CHECK(ho.fade > 0.99f, "%s: negro inmediato al atravesar (%.2f)", nm[d], ho.fade);
        }
    }
    free(tris);
}

int main(void) {
    ram = (uint8_t *)calloc(1, NS_RAM_SIZE);
    st = ns_store_create();
    if (ns_scene_alloc(&sc, 1, 1, 1, 0)) { printf("sin memoria\n"); return 1; }
    test_collect();
    test_missing_mesh();
    test_head_vs_native_room();
    printf("test_ns_solid: %d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
