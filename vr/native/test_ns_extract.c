/* Pruebas sintéticas del extractor (ns_extract) y del adaptador (ns_adapt): RAM construida a mano, sin datos del juego. */
#include "ns_adapt.h"
#include "ns_extract.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint8_t *ram;
static uint32_t arena;
static void w8(uint32_t a, uint8_t v) { ram[a & 0x1FFFFF] = v; }
static void w16(uint32_t a, uint32_t v) { ram[a & 0x1FFFFF] = (uint8_t)v; ram[(a & 0x1FFFFF) + 1] = (uint8_t)(v >> 8); }
static void w32(uint32_t a, uint32_t v) { w16(a, v & 0xFFFF); w16(a + 2, v >> 16); }
static uint32_t rd32_(uint32_t a) { uint32_t v; memcpy(&v, ram + (a & 0x1FFFFF), 4); return v; }
static uint32_t alloc(uint32_t n) { uint32_t a = arena; arena += (n + 15u) & ~15u; return a; }
static void wmat(uint32_t a, int s, int32_t tx, int32_t ty, int32_t tz) {   /* rotación identidad por s/4096 + traslación */
    for (int i = 0; i < 9; i++) w16(a + 2u * (uint32_t)i, (i % 4 == 0) ? (uint32_t)s : 0u);
    w32(a + 0x14, (uint32_t)tx); w32(a + 0x18, (uint32_t)ty); w32(a + 0x1C, (uint32_t)tz);
}
#define CHAN 0x800B2B7Cu
#define QUEUE 0x800B2110u
#define LIGHTG 0x8009E55Cu

/* modelo con nv vértices, nf caras; devuelve la dirección de las tablas en out[] */
typedef struct { uint32_t v, vi, n, ni, uv, tx; } Tabs;
static Tabs model(uint32_t m, uint32_t flag, int nf, int nv, int nn, int parent) {
    Tabs t;
    t.v = alloc((uint32_t)nv * 8); t.vi = alloc((uint32_t)nf * 4); t.n = alloc((uint32_t)(nn ? nn : 1) * 8); t.ni = alloc((uint32_t)nf * 4); t.uv = alloc((uint32_t)nf * 8); t.tx = alloc((uint32_t)nf * 2);
    w32(m, flag); w32(m + 4, (uint32_t)nf); w32(m + 0x2C, (uint32_t)parent); w32(m + 0x30, 0xFFFFFFFFu); w32(m + 0x34, (uint32_t)nv);
    w32(m + 0x38, t.v); w32(m + 0x3C, t.vi); w32(m + 0x40, (uint32_t)nn); w32(m + 0x44, t.n); w32(m + 0x48, t.ni); w32(m + 0x4C, t.uv); w32(m + 0x50, t.tx);
    return t;
}
static void vert(Tabs *t, int i, int x, int y, int z, int pad) { w16(t->v + 8u * (uint32_t)i, (uint32_t)x); w16(t->v + 8u * (uint32_t)i + 2, (uint32_t)y); w16(t->v + 8u * (uint32_t)i + 4, (uint32_t)z); w16(t->v + 8u * (uint32_t)i + 6, (uint32_t)pad); }
static void face(Tabs *t, int f, int a, int b, int c, int d, int na) {
    w8(t->vi + 4u * (uint32_t)f, (uint8_t)a); w8(t->vi + 4u * (uint32_t)f + 1, (uint8_t)b); w8(t->vi + 4u * (uint32_t)f + 2, (uint8_t)c); w8(t->vi + 4u * (uint32_t)f + 3, (uint8_t)d);
    for (int k = 0; k < 4; k++) w8(t->ni + 4u * (uint32_t)f + (uint32_t)k, (uint8_t)na);
    const uint8_t uv[8] = {0, 0, 255, 0, 255, 255, 0, 255};
    for (int k = 0; k < 8; k++) w8(t->uv + 8u * (uint32_t)f + (uint32_t)k, uv[k]);
    w16(t->tx + 2u * (uint32_t)f, 0x1234);
}
static void queue_set(int n, const uint32_t *objs) {
    w16(CHAN + 0x56, (uint32_t)n); w32(CHAN + 0x58, QUEUE);
    for (int i = 0; i < n; i++) w32(QUEUE + 4u * (uint32_t)i, objs[i]);
}
static uint32_t object(uint32_t flag, int gid, int nm, uint32_t def, uint32_t light) {
    uint32_t o = alloc(0x48 + 0x5C * (uint32_t)(nm > 0 && nm < 64 ? nm + 2 : 4));
    wmat(o, 4096, 0, 0, 0);
    w32(o + 0x24, def); w32(o + 0x28, flag); w16(o + 0x2C, (uint32_t)gid); w16(o + 0x2E, (uint32_t)nm); w32(o + 0x34, light);
    return o;
}
static uint32_t piece(uint32_t o, int k) { return o + 0x48 + 0x5Cu * (uint32_t)k; }

static NsScene sc; static NsMeshStore *st;
static uint32_t O0, O1, O2, O3, O4, O5, O6, O7, O8, O9, O10, O11, DEF0, MODEL7_V;

static void build(void) {
    memset(ram, 0, NS_RAM_SIZE);
    arena = 0x80130000u;
    /* tabla de texturas: id 0x1234 */
    uint32_t te = 0x800B30E0u + 12u * (0x1234u % 512u);
    w16(te, 0x1234); w16(te + 4, 0x0105); w16(te + 6, 0x7800); w8(te + 8, 16); w8(te + 9, 32); w8(te + 10, 31); w8(te + 11, 15);
    /* luz global: dir principal (0,-4096,0), color 560, ambiente 51 */
    w16(LIGHTG + 2, (uint32_t)-4096 & 0xFFFF);
    w16(LIGHTG + 32, 560); w16(LIGHTG + 38, 560); w16(LIGHTG + 44, 560);   /* col[c][0] */
    for (int c = 0; c < 3; c++) w16(0x800AC530u + 2u * (uint32_t)c, 51);
    w32(0x800ACAF8u, 1);
    wmat(CHAN + 0x30, 4106, 100, 200, 300);
    w16(CHAN + 0x50, 320);

    /* objeto 0: mapa (texturizado|precalculado|translúcido|una matriz), 2 piezas */
    uint32_t def0 = alloc(0x20 + 0x58 * 2); w32(def0, 2); w32(def0 + 4, 2);
    uint32_t o0 = object(0x57, 1, 2, def0, LIGHTG); O0 = o0; DEF0 = def0;
    for (int k = 0; k < 2; k++) {
        uint32_t m = def0 + 0x20 + 0x58u * (uint32_t)k;
        Tabs t = model(m, 0x203, 2, 4, 1, -1);
        vert(&t, 0, 0, 0, 0, 0xFFFF); vert(&t, 1, 1000, 0, 0, 0xFFFF); vert(&t, 2, 1000, 0, 1000, 0xFFFF); vert(&t, 3, 0, 0, 1000, 0xFFFF);
        w16(t.n, 0); w16(t.n + 2, (uint32_t)-4096 & 0xFFFF); w16(t.n + 4, 0);
        face(&t, 0, 0, 1, 2, 3, 0);
        face(&t, 1, 0, 1, 2, 2, 0);   /* triángulo */
        uint32_t rg = alloc(2 * 16);
        for (int i = 0; i < 8; i++) { w8(rg + 4u * (uint32_t)i, (uint8_t)(10 + i)); w8(rg + 4u * (uint32_t)i + 1, (uint8_t)(20 + i)); w8(rg + 4u * (uint32_t)i + 2, (uint8_t)(30 + i)); w8(rg + 4u * (uint32_t)i + 3, 0x3C); }
        w32(piece(o0, k) + 0x40, m); w32(piece(o0, k) + 0x44, rg); wmat(piece(o0, k), 4096, 0, 0, 0);
    }
    /* objeto 1: dinámico articulado: pieza 0 raíz y pieza 1 hija con vértice soldado al 3 del padre; luz propia + ambiente propio */
    uint32_t def1 = alloc(0x20 + 0x58 * 2); w32(def1, 2); w32(def1 + 4, 2);
    uint32_t lp = alloc(64);
    w16(lp + 2, (uint32_t)-4096 & 0xFFFF); w32(lp + 0x14, 90); w32(lp + 0x18, 80); w32(lp + 0x1C, 70);
    w16(lp + 32, 800); w16(lp + 38, 700); w16(lp + 44, 600);
    uint32_t o1 = object(0x32D, 1, 2, def1, lp); O1 = o1;
    {
        uint32_t m0 = def1 + 0x20, m1 = def1 + 0x20 + 0x58;
        Tabs t0 = model(m0, 0x10001, 1, 4, 1, -1);
        vert(&t0, 0, 0, 0, 0, 0xFFFF); vert(&t0, 1, 100, 0, 0, 0xFFFF); vert(&t0, 2, 100, 100, 0, 0xFFFF); vert(&t0, 3, 0, 100, 0, 0xFFFF);
        w16(t0.n, 0); w16(t0.n + 2, 4096); w16(t0.n + 4, 0);
        face(&t0, 0, 0, 1, 2, 3, 0);
        Tabs t1 = model(m1, 0x10001, 1, 4, 1, 0);
        vert(&t1, 0, 5, 5, 5, 0xFFFF); vert(&t1, 1, 5, 6, 5, 8 + 24); vert(&t1, 2, 6, 6, 5, 0xFFFF); vert(&t1, 3, 6, 5, 7, 0xFFFF);   /* vértice 1: soldado (cara 0, esquina D del padre) */
        w16(t1.n, 0); w16(t1.n + 2, 4096); w16(t1.n + 4, 0);
        face(&t1, 0, 0x80 | 1, 2, 3, 0, 0);
        wmat(piece(o1, 0), 4096, 1000, 0, 0); wmat(piece(o1, 1), 4096, 2000, 500, 0);
        w32(piece(o1, 0) + 0x40, m0); w32(piece(o1, 1) + 0x40, m1);
    }
    /* objeto 2: oculto; objeto 3: grupo no activo; objeto 4: nm absurdo; objeto 5: modelo con tablas fuera de RAM + extensión a sí misma */
    uint32_t o2 = object(0x57 | 0x80, 1, 1, def0, LIGHTG);
    uint32_t o3 = object(0x57, 4, 1, def0, LIGHTG);
    uint32_t o4 = object(0x57, 1, 5000, def0, LIGHTG);
    uint32_t def5 = alloc(0x20 + 0x58); w32(def5, 1); w32(def5 + 4, 1);
    uint32_t o5 = object(0x57, 1, 1, def5, LIGHTG); O5 = o5; O2 = o2; O3 = o3; O4 = o4;
    w32(def5 + 0x20, 1); w32(def5 + 0x24, 3); w32(def5 + 0x34, 4); w32(def5 + 0x38, 0x80200000u); w32(def5 + 0x3C, 0x80100000u);
    w32(piece(o5, 0) + 0x40, def5 + 0x20); w32(piece(o5, 0) + 0x48, piece(o5, 0));
    /* objeto 6: pieza 0 (modelo de o0) con extensión (índice 1) válida */
    uint32_t o6 = object(0x57, 1, 1, def0, LIGHTG); O6 = o6;
    w32(piece(o6, 0) + 0x40, def0 + 0x20); w32(piece(o6, 0) + 0x44, 0); wmat(piece(o6, 0), 4096, 5, 6, 7);
    w32(piece(o6, 0) + 0x48, piece(o6, 1)); w32(piece(o6, 1) + 0x40, def0 + 0x20 + 0x58);
    /* objeto 7: modelo SIN vértices soldados con 200 vértices e índices >= 0x80; sesgo de profundidad positivo en el flag y guardado en la pieza */
    {
        uint32_t def7 = alloc(0x20 + 0x58); w32(def7, 1); w32(def7 + 4, 1);
        O7 = object(0x1, 1, 1, def7, 0);
        Tabs t = model(def7 + 0x20, 0x101, 2, 200, 0, -1);
        for (int i = 0; i < 200; i++) vert(&t, i, i, 2 * i, 3 * i, 0xFFFF);
        MODEL7_V = t.v;
        face(&t, 0, 0x90, 0x91, 0x92, 0x93, 0);
        face(&t, 1, 0x7F, 0x80, 0xC7, 0xC7, 0);
        w32(piece(O7, 0) + 0x40, def7 + 0x20); w16(piece(O7, 0) + 0x50, 1000); wmat(piece(O7, 0), 4096, 0, 0, 0);
    }
    /* objeto 8: un contador por cara: índice de vértice malo, índice de normal malo, textura desconocida, repetición de vértices */
    {
        uint32_t def8 = alloc(0x20 + 0x58); w32(def8, 1); w32(def8 + 4, 1);
        O8 = object(0x1, 1, 1, def8, 0);
        Tabs t = model(def8 + 0x20, 0x1, 4, 4, 1, -1);
        vert(&t, 0, 0, 0, 0, 0xFFFF); vert(&t, 1, 100, 0, 0, 0xFFFF); vert(&t, 2, 100, 100, 0, 0xFFFF); vert(&t, 3, 0, 100, 0, 0xFFFF);
        face(&t, 0, 0, 1, 2, 9, 0);
        face(&t, 1, 0, 1, 2, 3, 5);
        face(&t, 2, 0, 1, 2, 3, 0); w16(t.tx + 4, 0x4321);
        face(&t, 3, 0, 0, 1, 2, 0);
        w32(piece(O8, 0) + 0x40, def8 + 0x20); wmat(piece(O8, 0), 4096, 0, 0, 0);
    }
    /* objeto 9: soldadura imposible (desplazamiento < 8) */
    {
        uint32_t def9 = alloc(0x20 + 0x58 * 2); w32(def9, 2); w32(def9 + 4, 2);
        O9 = object(0x1, 1, 2, def9, 0);
        Tabs t0 = model(def9 + 0x20, 0x1, 1, 4, 0, -1);
        for (int i = 0; i < 4; i++) vert(&t0, i, i, 0, 0, 0xFFFF);
        face(&t0, 0, 0, 1, 2, 3, 0);
        Tabs t1 = model(def9 + 0x20 + 0x58, 0x10001, 1, 4, 0, 0);
        for (int i = 0; i < 4; i++) vert(&t1, i, i, 1, 0, i == 1 ? 5 : 0xFFFF);
        face(&t1, 0, 0x81, 2, 3, 0, 0);
        for (int k = 0; k < 2; k++) { w32(piece(O9, k) + 0x40, def9 + 0x20 + 0x58u * (uint32_t)k); wmat(piece(O9, k), 4096, 0, 0, 0); }
    }
    /* objeto 10: puntero de modelo nulo */
    O10 = object(0x1, 1, 1, 0, 0); wmat(piece(O10, 0), 4096, 0, 0, 0);
    /* objeto 11: una pieza, una cara plana, translúcido e iluminado (aspecto de sombra) */
    {
        uint32_t def11 = alloc(0x20 + 0x58); w32(def11, 1); w32(def11 + 4, 1);
        O11 = object(0xD, 1, 1, def11, LIGHTG);
        Tabs t = model(def11 + 0x20, 0x2, 1, 4, 1, -1);
        vert(&t, 0, -50, 0, -50, 0xFFFF); vert(&t, 1, 50, 0, -50, 0xFFFF); vert(&t, 2, 50, 0, 50, 0xFFFF); vert(&t, 3, -50, 0, 50, 0xFFFF);
        face(&t, 0, 0, 1, 2, 3, 0);
        w32(piece(O11, 0) + 0x40, def11 + 0x20); wmat(piece(O11, 0), 4096, 0, 0, 0);
    }
    uint32_t q[16] = {o0, o1, o2, o3, o4, 0x70000000u, 0, o0, o5, o6, 0x80300000u + 0x1FFFF0u};
    queue_set(11, q);
}

static void fresh(void) {
    ns_store_destroy(st); st = ns_store_create();
    build();
}
static void q1(uint32_t a) { queue_set(1, &a); }
static void q2(uint32_t a, uint32_t b) { uint32_t v[2] = {a, b}; queue_set(2, v); }
static const NsInstance *find_inst(const NsScene *s, uint32_t obj) {
    for (uint32_t i = 0; i < s->n_inst; i++) if (s->inst[i].key == (0x80000000u | (obj & 0x1FFFFFFFu))) return &s->inst[i];
    return NULL;
}
/* invariantes de toda escena extraída: cada pieza visible tiene malla en el almacén y los padres son índices válidos de la instancia */
static int invariants(const NsScene *s, const NsMeshStore *store) {
    int ok = 1;
    for (uint32_t i = 0; i < s->n_inst; i++) {
        const NsInstance *in = &s->inst[i];
        if ((uint64_t)in->first_piece + in->n_pieces > s->n_piece) { ok = 0; continue; }
        for (uint32_t k = 0; k < in->n_pieces; k++) {
            const NsPiece *p = &s->piece[in->first_piece + k];
            if (p->parent >= (int)in->n_pieces) ok = 0;
            if (!(p->flags & NS_PF_HIDDEN)) {
                const NsMesh *m = ns_store_get(store, p->mesh_id);
                if (!m) ok = 0;
                else if (p->baked_ver && !ns_store_get_baked(store, p->mesh_id, p->baked_ver)) ok = 0;
            }
        }
        if (in->light >= (int)s->n_light) ok = 0;
    }
    return ok;
}

int main(void) {
    ram = (uint8_t *)calloc(1, NS_RAM_SIZE);
    st = ns_store_create();
    CHECK(ns_scene_alloc(&sc, 1, 1, 1, 0) == 0);   /* se sube a los mínimos del contrato */
    CHECK(sc.cap_inst >= NS_MIN_INST && sc.cap_piece >= NS_MIN_PIECE && sc.cap_light >= NS_MIN_LIGHT && sc.cap_effect >= NS_MIN_EFFECT);
    build();
    NsExtractStats es;
    int rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
    CHECK(rc == 0);
    CHECK(sc.size == sizeof(NsScene) && sc.version == NS_VERSION && sc.seq == 1);
    CHECK(sc.truncated == 0);
    CHECK((sc.valid & NS_SV_MAP_OK) && (sc.valid & NS_SV_LISTS_OK) && (sc.valid & NS_SV_GAME));
    CHECK(es.queue_n == 11);
    CHECK(es.drop_hidden_flag == 1);
    CHECK(es.drop_group == 1);
    CHECK(es.drop_badobj == 1);
    CHECK(es.drop_dup == 1);
    CHECK(es.drop_null == 1);
    CHECK(es.drop_badptr == 2);
    CHECK(es.drop_bad_model == 1);
    CHECK(es.ext_bad == 1);
    CHECK(sc.n_inst == 4);   /* objetos 0, 1, 5 (modelo roto: pieza oculta) y 6 */
    CHECK(es.n_ext_pieces == 1);
    CHECK(es.n_static == 3);
    CHECK(sc.inst[0].flags & NS_IF_STATIC);   /* el escenario va primero */
    CHECK(!(sc.inst[3].flags & NS_IF_STATIC) && sc.inst[3].key == (0x80000000u | (O1 & 0x1FFFFFFFu)));
    CHECK(sc.cam.h == 320.0f && fabsf(sc.cam.eye[0] - 100.0f) < 1e-3f && fabsf(sc.cam.rot[0] - 4106.0f / 4096.0f) < 1e-4f);
    CHECK(sc.cam.overscan_y == NS_OVERSCAN_Y);
    CHECK(sc.group_mask == 1);
    CHECK(sc.mesh_epoch == 1 && sc.zone_gen == 1);
    CHECK(invariants(&sc, st));

    /* mapa */
    const NsInstance *i0 = find_inst(&sc, O0);
    CHECK(i0 && i0->n_pieces == 2 && (i0->flags & NS_IF_STATIC) && !(i0->flags & NS_IF_LIT) && i0->light == -1);
    const NsMesh *m0 = i0 ? ns_store_get(st, sc.piece[i0->first_piece].mesh_id) : NULL;
    const NsBaked *b0 = i0 ? ns_store_get_baked(st, sc.piece[i0->first_piece].mesh_id, sc.piece[i0->first_piece].baked_ver) : NULL;
    CHECK(m0 && m0->n_faces == 2 && m0->n_verts == 4 && (m0->flags & NS_MF_BAKED) && b0 && b0->n_colors == 8);
    if (m0 && b0) {
        CHECK(!(m0->faces[0].flags & NS_FF_TRI) && (m0->faces[1].flags & NS_FF_TRI));
        CHECK((m0->faces[0].flags & NS_FF_BAKED_OK) && sc.piece[i0->first_piece].baked_ver >= 1);
        CHECK(m0->faces[0].tpage == 0x0105 && m0->faces[0].clut == 0x7800 && m0->faces[0].texid == 0x1234);
        CHECK(m0->faces[0].u[2] == 47 && m0->faces[0].t[2] == 47 && m0->faces[0].u[0] == 16 && m0->faces[0].t[0] == 32);
        CHECK(b0->rgba[0] == (10u | (20u << 8) | (30u << 16) | 0xFF000000u));
        CHECK(m0->bbox[3] == 1000 && m0->bbox[5] == 1000);
        CHECK(m0->flags & NS_MF_BLEND);
        CHECK(m0->id == ns_mesh_content_hash(m0));
    }
    /* dinámico con soldadura (sólo el vértice marcado queda soldado; los demás conservan su posición) */
    const NsInstance *i1 = find_inst(&sc, O1);
    CHECK(i1 && i1->n_pieces == 2 && (i1->flags & NS_IF_LIT) && (i1->flags & NS_IF_OWN_AMBIENT) && i1->light >= 0);
    if (i1 && i1->light >= 0) {
        const NsLight *L = &sc.lights[i1->light];
        CHECK(L->amb[0] == 90 && L->amb[1] == 80 && L->amb[2] == 70 && L->dir[0][1] == -4096 && L->col[0][0] == 800 && L->col[1][0] == 700 && L->col[2][0] == 600);
    }
    const NsPiece *pc = &sc.piece[i1->first_piece + 1];
    const NsMesh *mc = ns_store_get(st, pc->mesh_id);
    CHECK(mc && pc->parent == 0 && (mc->flags & NS_MF_WELDED) && mc->verts[1].weld == 3 && mc->verts[0].weld == 0xFF && mc->verts[2].weld == 0xFF && mc->verts[3].weld == 0xFF);
    CHECK(es.welded_verts == 1 && es.weld_bad == 0);
    CHECK(pc->baked_ver == 0);

    /* adaptador */
    NsRTri *tr = NULL; NsAdaptStats as;
    NsAdaptOpts ao; memset(&ao, 0, sizeof ao); ao.no_tint = 1;
    size_t nt = ns_adapt_scene(&sc, st, &ao, &tr, &as);
    CHECK(nt > 0 && tr);
    CHECK(as.welded_verts == 1);
    CHECK(as.faces_baked >= 4 && as.faces_dyn == 2);
    /* el vértice soldado de la pieza hija se coloca en la posición del vértice 3 del padre transformada con el padre: (1000, 100, 0) */
    int found = 0;
    for (size_t i = 0; i < nt; i++) for (int v = 0; v < 3; v++) if (fabsf(tr[i].v[v].p[0] - 1000.0f) < 1e-3f && fabsf(tr[i].v[v].p[1] - 100.0f) < 1e-3f && fabsf(tr[i].v[v].p[2]) < 1e-3f) found++;
    CHECK(found >= 1);
    /* división exacta de la primera cara del mapa (A=(0,0,0) B=(1000,0,0) C=(1000,0,1000) D=(0,0,1000)): (A,B,D) y (B,D,C), con el color de cada esquina */
    {
        static const float P[2][3][3] = {{{0, 0, 0}, {1000, 0, 0}, {0, 0, 1000}}, {{1000, 0, 0}, {0, 0, 1000}, {1000, 0, 1000}}};
        static const int COL[2][3] = {{0, 1, 3}, {1, 3, 2}};   /* esquina de cada vértice del triángulo */
        for (int t = 0; t < 2; t++) for (int v = 0; v < 3; v++) {
            for (int k = 0; k < 3; k++) CHECK(fabsf(tr[t].v[v].p[k] - P[t][v][k]) < 1e-3f);
            CHECK(tr[t].v[v].r == 10 + COL[t][v] && tr[t].v[v].g == 20 + COL[t][v] && tr[t].v[v].b == 30 + COL[t][v]);
        }
    }
    free(tr);
    ao.cull_eye = (float[3]){0, 1000, 0};
    nt = ns_adapt_scene(&sc, st, &ao, &tr, &as);
    CHECK(as.drop_backface > 0);
    free(tr);

    /* segunda extracción: mismos ids, nada nuevo en el almacén, sin recodificar (memoria) */
    uint32_t cnt = ns_store_count(st);
    uint64_t id0 = sc.piece[0].mesh_id;
    rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
    CHECK(rc == 0 && ns_store_count(st) == cnt && sc.piece[0].mesh_id == id0 && es.mesh_new == 0 && sc.seq == 2 && es.baked_new == 0 && es.baked_ref >= 2 && es.zone_changes == 0);

    /* sombras: lista del módulo de efectos y heurística */
    {
        uint32_t skip[1] = {O1};
        NsExtractOpts xo; memset(&xo, 0, sizeof xo); xo.shadow_objs = skip; xo.n_shadow = 1;
        rc = ns_extract_ex(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &xo, &es);
        CHECK(rc == 0 && es.shadow_omitted == 1 && sc.n_shadow_omitted == 1 && sc.n_inst == 3 && !find_inst(&sc, O1));
        q2(O11, O1);
        rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
        CHECK(rc == 0 && sc.n_inst == 2 && sc.n_shadow_omitted == 0);
        memset(&xo, 0, sizeof xo); xo.detect_shadows = 1;
        rc = ns_extract_ex(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &xo, &es);
        CHECK(rc == 0 && sc.n_inst == 1 && sc.n_shadow_omitted == 1 && es.shadow_omitted == 1 && find_inst(&sc, O1));
    }
    /* cola con bucle, cuenta fuera de rango, RAM corta */
    {
        uint32_t objs[256];
        for (int i = 0; i < 256; i++) objs[i] = O0;
        queue_set(256, objs);
        rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
        CHECK(rc == 0 && sc.n_inst == 1 && es.drop_dup == 255);
        w16(CHAN + 0x56, 9999);
        rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
        CHECK(rc == 0 && es.q_clamped == 1 && sc.n_inst == 1);
        w16(CHAN + 0x56, (uint32_t)-5);
        rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
        CHECK(rc == 0 && es.q_clamped == 1 && sc.n_inst == 0);
        w16(CHAN + 0x56, 4); w32(CHAN + 0x58, 0x80300000u);
        rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
        CHECK(rc == 0 && sc.n_inst == 0 && es.drop_badptr >= 1);
        fresh();
        rc = ns_extract(&sc, st, ram, 0x1000, NULL, 0, &es);
        CHECK(rc == -1 && sc.n_inst == 0 && sc.valid == 0);
        rc = ns_extract(&sc, st, NULL, 0, NULL, 0, &es);
        CHECK(rc == -1);
    }

    /* modelo sin vértices soldados: índices de 8 bits completos (>= 0x80) y sesgo de profundidad del flag */
    fresh();
    q1(O7);
    rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
    CHECK(rc == 0 && sc.n_inst == 1 && es.faces_bad_vindex == 0);
    {
        const NsMesh *m7 = ns_store_get(st, sc.piece[0].mesh_id);
        CHECK(m7 && m7->n_verts == 200 && !(m7->flags & NS_MF_WELDED));
        if (m7) {
            CHECK(m7->faces[0].v[0] == 0x90 && m7->faces[0].v[1] == 0x91 && m7->faces[0].v[2] == 0x92 && m7->faces[0].v[3] == 0x93);
            CHECK(m7->faces[1].v[0] == 0x7F && m7->faces[1].v[1] == 0x80 && m7->faces[1].v[2] == 0xC7 && (m7->faces[1].flags & NS_FF_TRI));
            CHECK(m7->depth_bias == 1000 && (m7->flags & NS_MF_BIAS_UP));
        }
        CHECK(es.bias_mismatch == 0);
        w16(piece(O7, 0) + 0x50, 0);
        rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
        CHECK(es.bias_mismatch == 1);
    }
    /* un contador por motivo */
    fresh();
    q1(O8);
    rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
    CHECK(rc == 0 && es.faces_seen == 4);
    CHECK(es.faces_bad_vindex == 1);
    CHECK(es.faces_bad_nindex == 1);
    CHECK(es.faces_no_tex == 1);
    CHECK(es.faces_degenerate == 1);
    CHECK(es.faces_quad == 3 && es.faces_tri == 0);
    {
        const NsMesh *m8 = ns_store_get(st, sc.piece[0].mesh_id);
        CHECK(m8 && m8->n_faces == 4 && m8->faces[0].v[0] == 0 && m8->faces[0].v[3] == 0);   /* la cara mala se conserva degenerada: no se pierde la alineación */
    }
    fresh();
    q1(O9);
    rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
    CHECK(rc == 0 && es.weld_bad == 1 && es.welded_verts == 0);
    fresh();
    q1(O10);
    rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
    CHECK(rc == 0 && sc.n_inst == 1 && es.null_model == 1 && (sc.piece[0].flags & NS_PF_HIDDEN) && sc.piece[0].mesh_id == 0 && es.n_hidden_pieces == 1);

    /* color precalculado versionado: mismo contenido = misma versión; contenido nuevo = versión nueva con el mismo mesh_id */
    fresh();
    q1(O0);
    rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
    CHECK(es.baked_new >= 1 && es.baked_new + es.baked_ref == 2);   /* las dos piezas comparten malla y color: una versión */
    uint32_t v1 = sc.piece[0].baked_ver; uint64_t mid = sc.piece[0].mesh_id;
    rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
    CHECK(es.baked_new == 0 && es.baked_ref == 2 && sc.piece[0].baked_ver == v1);
    {
        uint32_t rg = rd32_(piece(O0, 0) + 0x44);
        w8(rg, 99);
        rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
        CHECK(es.baked_new == 1 && sc.piece[0].baked_ver != v1 && sc.piece[0].mesh_id == mid && es.mesh_new == 0 && es.zone_changes == 0);
        const NsBaked *bn = ns_store_get_baked(st, mid, sc.piece[0].baked_ver), *bo = ns_store_get_baked(st, mid, v1);
        CHECK(bn && bo && (bn->rgba[0] & 255) == 99 && (bo->rgba[0] & 255) == 10);
        w8(rg, 10);   /* vuelve el color anterior: se reutiliza la versión antigua */
        rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
        CHECK(es.baked_new == 0 && sc.piece[0].baked_ver == v1);
    }

    /* contenido que cambia en la misma dirección: dinámico = recodifica (id nuevo); escenario = cambio de zona (generación y época) */
    fresh();
    q1(O7);
    rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
    uint64_t idA = sc.piece[0].mesh_id;
    w16(MODEL7_V, 7777);
    rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
    CHECK(es.memo_stale == 1 && sc.piece[0].mesh_id != idA && es.zone_changes == 0 && sc.zone_gen == 1);
    {   /* regresión (puño de la mano): un vértice INTERIOR de una tabla pequeña (ni los primeros ni los últimos bytes) también invalida la memoria */
        uint64_t idC = sc.piece[0].mesh_id;
        rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
        CHECK(es.memo_stale == 0 && sc.piece[0].mesh_id == idC);                    /* sin cambios: se reutiliza */
        w16(MODEL7_V + 8u * 100u + 2u, 1234);
        rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
        CHECK(es.memo_stale == 1 && sc.piece[0].mesh_id != idC && es.zone_changes == 0);
        (void)rc;
    }
    fresh();
    q1(O0);
    rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
    uint64_t idB = sc.piece[0].mesh_id;
    CHECK(sc.mesh_epoch == 1 && sc.zone_gen == 1);
    {
        uint32_t m0 = DEF0 + 0x20, m1 = DEF0 + 0x20 + 0x58;
        uint32_t mv = rd32_(m0 + 0x38);
        /* regresión (época que subía en cada escena): vértices del escenario que se animan en el sitio (misma estructura) recodifican la malla pero NO son un cambio de zona */
        w16(mv + 2, 4242); w16(rd32_(m1 + 0x38) + 2, 4141);
        rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
        CHECK(es.zone_changes == 0 && sc.zone_gen == 1 && sc.mesh_epoch == 1 && es.memo_stale >= 1 && sc.piece[0].mesh_id != idB);
        w16(mv + 2, 4343);
        rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
        CHECK(es.zone_changes == 0 && sc.zone_gen == 1 && sc.mesh_epoch == 1);
        /* ... y la misma RAM extraída dos veces no cambia nada */
        rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
        CHECK(es.zone_changes == 0 && es.memo_stale == 0 && es.mesh_new == 0 && sc.mesh_epoch == 1);
        /* cambio ESTRUCTURAL de modelos conocidos del escenario (tabla de índices de caras): cambio de zona */
        uint32_t vi0 = rd32_(m0 + 0x3C), vi1 = rd32_(m1 + 0x3C);
        #define SWAPVI(vi) do { uint8_t t_ = ram[(vi) & 0x1FFFFF]; ram[(vi) & 0x1FFFFF] = ram[((vi) & 0x1FFFFF) + 1]; ram[((vi) & 0x1FFFFF) + 1] = t_; } while (0)
        CHECK(ram[vi0 & 0x1FFFFF] != ram[(vi0 & 0x1FFFFF) + 1] && ram[vi1 & 0x1FFFFF] != ram[(vi1 & 0x1FFFFF) + 1]);
        SWAPVI(vi0); SWAPVI(vi1);
        rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
        CHECK(es.zone_changes == 1 && es.epoch_opened == 1 && sc.zone_gen == 2 && sc.mesh_epoch == 2 && sc.piece[0].mesh_id != idB);
        CHECK(ns_store_get(st, idB) != NULL);                 /* la época anterior sigue legible hasta que el consumidor reconoce la nueva */
        CHECK(ns_store_get(st, sc.piece[0].mesh_id) != NULL);
        ns_store_ack(st, 2);
        CHECK(ns_store_get(st, idB) == NULL);                 /* reconocida la época 2 se libera la 1 */
        /* nuevo cambio sin reconocer la época 3: se sigue en la actual */
        SWAPVI(vi0); SWAPVI(vi1);
        rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
        CHECK(es.zone_changes == 1 && es.epoch_opened == 1 && sc.mesh_epoch == 3);
        SWAPVI(vi0); SWAPVI(vi1);
        rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
        CHECK(es.zone_changes == 1 && es.epoch_opened == 0 && sc.mesh_epoch == 3 && sc.zone_gen == 4);
        #undef SWAPVI
        CHECK(invariants(&sc, st));
    }
    /* vértices del escenario animados durante muchas escenas: la época no sube sin parar (solo por presión del almacén) y el almacén queda acotado */
    {
        fresh();
        q1(O0);
        rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
        ns_store_ack(st, sc.mesh_epoch);
        uint32_t mv = rd32_(DEF0 + 0x20 + 0x38), mv1 = rd32_(DEF0 + 0x20 + 0x58 + 0x38);
        size_t one = ns_store_bytes(st);
        ns_store_set_compact(st, one * 6);
        uint32_t e0 = sc.mesh_epoch, g0 = sc.zone_gen, comp = 0, zc = 0, maxb = 0;
        for (int i = 0; i < 300; i++) {
            w16(mv + 2, 3000 + i); w16(mv1 + 2, 5000 + i);
            rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
            comp += es.compactions; zc += es.zone_changes;
            CHECK(ns_store_get(st, sc.piece[0].mesh_id) != NULL);
            ns_store_ack(st, sc.mesh_epoch);
            if (ns_store_bytes(st) > maxb) maxb = (uint32_t)ns_store_bytes(st);
        }
        CHECK(zc == 0 && sc.zone_gen == g0);
        CHECK(comp >= 1 && sc.mesh_epoch - e0 == comp && comp < 150);
        CHECK(maxb < one * 16);
        ns_store_set_compact(st, 0);
        (void)rc;
    }
    /* sustitución total de las definiciones del escenario = cambio de zona */
    fresh();
    q1(O0);
    rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
    q1(O5);   /* otra definición estática */
    rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
    CHECK(es.zone_changes == 1 && sc.zone_gen == 2);
    q1(O5);
    rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
    CHECK(es.zone_changes == 0 && sc.zone_gen == 2);

    /* tope del almacén: el escenario no se descarta (piezas sin malla ocultas, mapa incompleto); lo dinámico sí */
    fresh();
    ns_store_set_limit(st, 1);
    q2(O1, O0);
    rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
    CHECK(rc == 0 && es.store_full > 0 && (sc.valid & NS_SV_STORE_FULL) && (sc.valid & NS_SV_MAP_INCOMPLETE) && !(sc.valid & NS_SV_LISTS_OK));
    CHECK(sc.n_inst == 1 && (sc.inst[0].flags & NS_IF_STATIC) && sc.n_dropped_inst == 1 && (sc.piece[0].flags & NS_PF_HIDDEN));
    CHECK(invariants(&sc, st));
    ns_store_set_limit(st, NS_STORE_DEFAULT_LIMIT);
    rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
    CHECK(sc.n_inst == 2 && !(sc.valid & NS_SV_STORE_FULL) && !(sc.valid & NS_SV_MAP_INCOMPLETE));

    /* truncado por capacidad: el escenario tiene prioridad sobre lo dinámico; si el escenario no cabe, MAP_INCOMPLETE */
    {
        NsScene s2;
        fresh();
        CHECK(ns_scene_alloc_exact(&s2, 1, 8, 4, 1) == 0);
        q2(O1, O0);
        rc = ns_extract(&s2, st, ram, NS_RAM_SIZE, NULL, 0, &es);
        CHECK(rc == 0 && s2.n_inst == 1 && (s2.inst[0].flags & NS_IF_STATIC) && s2.n_dropped_inst == 1 && s2.n_dropped_piece == 2 && (s2.truncated & NS_TR_INST) && !(s2.valid & NS_SV_MAP_INCOMPLETE));
        q2(O0, O6);
        rc = ns_extract(&s2, st, ram, NS_RAM_SIZE, NULL, 0, &es);
        CHECK(rc == 0 && s2.n_inst == 1 && (s2.valid & NS_SV_MAP_INCOMPLETE) && (s2.truncated & NS_TR_INST) && s2.n_dropped_inst == 1);
        ns_scene_free(&s2);
        CHECK(ns_scene_alloc_exact(&s2, 8, 2, 4, 1) == 0);   /* 2 piezas: cabe un objeto de 2 piezas, el dinámico siguiente no */
        q2(O0, O1);
        rc = ns_extract(&s2, st, ram, NS_RAM_SIZE, NULL, 0, &es);
        CHECK(s2.n_inst == 1 && s2.n_piece == 2 && (s2.truncated & NS_TR_PIECE) && s2.n_dropped_inst == 1 && s2.n_dropped_piece == 2 && !(s2.valid & NS_SV_MAP_INCOMPLETE));
        CHECK(invariants(&s2, st));
        ns_scene_free(&s2);
        CHECK(ns_scene_alloc_exact(&s2, 8, 8, 4, 1) == 0);
        NsExtractOpts xo; memset(&xo, 0, sizeof xo); xo.max_pieces = 3;   /* tope global por llamada */
        q2(O0, O1);
        rc = ns_extract_ex(&s2, st, ram, NS_RAM_SIZE, NULL, 0, &xo, &es);
        CHECK(s2.n_piece == 2 && s2.n_dropped_inst == 1 && (s2.truncated & NS_TR_PIECE));
        ns_scene_free(&s2);
    }

    /* fuzz: la escena válida corrompida al azar; ningún acceso inválido (ASan/UBSan) e invariantes siempre */
    {
        unsigned seed = 12345u;
        long ok = 0, inv = 0;
        for (int it = 0; it < 300; it++) {
            fresh();
            for (int k = 0; k < 40 + it; k++) {
                seed = seed * 1664525u + 1013904223u;
                uint32_t a = 0x130000u + (seed >> 8) % 0x6000u;
                if (it % 3 == 0) a = 0x110000u + (seed >> 8) % 0x20000u;
                seed = seed * 1664525u + 1013904223u;
                ram[a] = (uint8_t)(seed >> 16);
            }
            rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
            NsRTri *t2 = NULL; NsAdaptStats a2;
            ns_adapt_scene(&sc, st, &ao, &t2, &a2); free(t2);
            rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);   /* segunda llamada: memoria, firmas y épocas */
            ok += rc == 0; inv += invariants(&sc, st);
        }
        CHECK(ok == 300);
        CHECK(inv == 300);
    }
    /* RAM totalmente aleatoria con el canal apuntando a la cola */
    for (int it = 0; it < 50; it++) {
        unsigned seed = 777u + (unsigned)it;
        for (size_t i = 0; i < NS_RAM_SIZE; i++) { seed = seed * 1664525u + 1013904223u; ram[i] = (uint8_t)(seed >> 24); }
        w16(CHAN + 0x56, 40); w32(CHAN + 0x58, QUEUE);
        rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
        NsRTri *t2 = NULL; NsAdaptStats a2;
        ns_adapt_scene(&sc, st, &ao, &t2, &a2); free(t2);
        CHECK(rc == 0 && invariants(&sc, st));
    }
    ns_scene_free(&sc); ns_store_destroy(st); free(ram);
    if (fails) { printf("%d fallos\n", fails); return 1; }
    printf("test_ns_extract: todo correcto\n");
    return 0;
}
