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
    uint32_t o0 = object(0x57, 1, 2, def0, LIGHTG);
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
    uint32_t o1 = object(0x32D, 1, 2, def1, lp);
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
    uint32_t o5 = object(0x57, 1, 1, def5, LIGHTG);
    w32(def5 + 0x20, 1); w32(def5 + 0x24, 3); w32(def5 + 0x34, 4); w32(def5 + 0x38, 0x80200000u); w32(def5 + 0x3C, 0x80100000u);
    w32(piece(o5, 0) + 0x40, def5 + 0x20); w32(piece(o5, 0) + 0x48, piece(o5, 0));
    /* objeto 6: pieza 0 (modelo de o0) con extensión (índice 1) válida */
    uint32_t o6 = object(0x57, 1, 1, def0, LIGHTG);
    w32(piece(o6, 0) + 0x40, def0 + 0x20); w32(piece(o6, 0) + 0x44, 0); wmat(piece(o6, 0), 4096, 5, 6, 7);
    w32(piece(o6, 0) + 0x48, piece(o6, 1)); w32(piece(o6, 1) + 0x40, def0 + 0x20 + 0x58);
    uint32_t q[16] = {o0, o1, o2, o3, o4, 0x70000000u, 0, o0, o5, o6, 0x80300000u + 0x1FFFF0u};
    queue_set(11, q);
}

int main(void) {
    ram = (uint8_t *)calloc(1, NS_RAM_SIZE);
    st = ns_store_create();
    CHECK(ns_scene_alloc(&sc, 1, 1, 1, 0) == 0);   /* capacidades mínimas: deben crecer sin truncar */
    build();
    NsExtractStats es;
    int rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
    CHECK(rc == 0);
    CHECK(sc.truncated == 0);
    CHECK(sc.valid & NS_SV_MAP_OK);
    CHECK(es.queue_n == 11);
    CHECK(es.drop_invisible == 1 && es.drop_group == 1 && es.drop_badobj == 1 && es.drop_dup == 1 && es.drop_null == 1 && es.drop_badptr == 2);
    CHECK(es.drop_bad_model == 1 && es.ext_bad == 1);
    CHECK(sc.n_inst == 4);   /* objetos 0, 1, 5 (modelo roto: pieza oculta) y 6 */
    CHECK(es.n_ext_pieces == 1);
    CHECK(sc.cam.h == 320.0f && fabsf(sc.cam.eye[0] - 100.0f) < 1e-3f && fabsf(sc.cam.rot[0] - 4106.0f / 4096.0f) < 1e-4f);
    CHECK(sc.group_mask == 1);

    /* mapa */
    const NsInstance *i0 = &sc.inst[0];
    CHECK(i0->n_pieces == 2 && (i0->flags & NS_IF_STATIC) && !(i0->flags & NS_IF_SHADE) && i0->light == -1);
    const NsMesh *m0 = ns_store_get(st, sc.piece[i0->first_piece].mesh_id);
    CHECK(m0 && m0->n_faces == 2 && m0->n_verts == 4 && m0->paint && (m0->flags & NS_MF_PAINT));
    if (m0) {
        CHECK((m0->id >> 32) == 0x80130000u || (m0->id >> 32) != 0);
        CHECK(!(m0->faces[0].flags & NS_FF_TRI) && (m0->faces[1].flags & NS_FF_TRI));
        CHECK(m0->faces[0].tpage == 0x0105 && m0->faces[0].clut == 0x7800 && m0->faces[0].texid == 0x1234);
        CHECK(m0->faces[0].u[2] == 47 && m0->faces[0].t[2] == 47 && m0->faces[0].u[0] == 16 && m0->faces[0].t[0] == 32);
        CHECK(m0->paint[0] == (10u | (20u << 8) | (30u << 16) | 0xFF000000u));
        CHECK(m0->bbox[3] == 1000 && m0->bbox[5] == 1000);
        CHECK(m0->flags & NS_MF_TRANS);
    }
    /* dinámico con soldadura */
    const NsInstance *i1 = &sc.inst[1];
    CHECK(i1->n_pieces == 2 && (i1->flags & NS_IF_SHADE) && (i1->flags & NS_IF_AMBIENT) && i1->light >= 0);
    if (i1->light >= 0) {
        const NsLight *L = &sc.lights[i1->light];
        CHECK(L->amb[0] == 90 && L->amb[1] == 80 && L->amb[2] == 70 && L->dir[0][1] == -4096 && L->col[0][0] == 800 && L->col[1][0] == 700 && L->col[2][0] == 600);
    }
    const NsPiece *pc = &sc.piece[i1->first_piece + 1];
    const NsMesh *mc = ns_store_get(st, pc->mesh_id);
    CHECK(mc && pc->parent == 0 && mc->verts[1].weld == 3 && mc->verts[0].weld == 0xFF && es.welded_verts == 1 && es.weld_bad == 0);
    CHECK(mc && mc->paint == NULL);

    /* adaptador */
    NsRTri *tr = NULL; NsAdaptStats as;
    NsAdaptOpts ao; memset(&ao, 0, sizeof ao); ao.no_tint = 1;
    size_t nt = ns_adapt_scene(&sc, st, &ao, &tr, &as);
    CHECK(nt > 0 && tr);
    CHECK(as.welded_verts == 1);
    CHECK(as.faces_paint >= 4 && as.faces_dyn == 2);
    /* el vértice soldado de la pieza hija se coloca en la posición del vértice 3 del padre transformada con el padre: (1000, 100, 0) */
    int found = 0;
    for (size_t i = 0; i < nt; i++) for (int v = 0; v < 3; v++) if (fabsf(tr[i].v[v].p[0] - 1000.0f) < 1e-3f && fabsf(tr[i].v[v].p[1] - 100.0f) < 1e-3f && fabsf(tr[i].v[v].p[2]) < 1e-3f) found++;
    CHECK(found >= 1);
    /* colores: cara del mapa (precalculada) tal cual x exposición 1 */
    CHECK(tr[0].v[0].r == 10 && tr[0].v[0].g == 20 && tr[0].v[0].b == 30);
    free(tr);
    ao.cull_eye = (float[3]){0, 1000, 0};
    nt = ns_adapt_scene(&sc, st, &ao, &tr, &as);
    CHECK(as.drop_backface > 0);
    free(tr);

    /* segunda extracción: mismos ids, nada nuevo en el almacén, sin recodificar (memoria por dirección) */
    uint32_t cnt = ns_store_count(st);
    uint64_t id0 = sc.piece[0].mesh_id;
    rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
    CHECK(rc == 0 && ns_store_count(st) == cnt && sc.piece[0].mesh_id == id0 && es.mesh_new == 0 && sc.seq == 2);

    /* omisión de objetos a petición */
    {
        uint32_t skip[1] = {sc.inst[1].key};
        NsExtractOpts xo = {skip, 1};
        rc = ns_extract_ex(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &xo, &es);
        CHECK(rc == 0 && es.drop_skipped == 1 && sc.n_inst == 3);
    }
    /* cambiar el color precalculado NO cambia el id (el hash lo excluye) */
    {
        uint32_t m = 0x80130000u;   /* sin tocar: sólo se comprueba que ns_store_clear invalida y sube la época */
        (void)m;
        uint32_t ep = ns_store_epoch(st);
        ns_store_clear(st);
        CHECK(ns_store_epoch(st) == ep + 1 && ns_store_count(st) == 0 && ns_store_get(st, id0) == NULL);
        rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
        CHECK(rc == 0 && es.mesh_new > 0 && sc.piece[0].mesh_id == id0 && sc.mesh_epoch == ep + 1);
    }
    /* cola con bucle: la misma entrada repetida 256 veces y cuenta fuera de rango */
    {
        uint32_t objs[256];
        for (int i = 0; i < 256; i++) objs[i] = sc.inst[0].key;
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
    }
    /* RAM más corta que 2 MiB y nula */
    {
        build();
        rc = ns_extract(&sc, st, ram, 0x1000, NULL, 0, &es);
        CHECK(rc == -1 && sc.n_inst == 0 && sc.valid == 0);
        rc = ns_extract(&sc, st, NULL, 0, NULL, 0, &es);
        CHECK(rc == -1);
    }
    /* fuzz: RAM aleatoria con el escenario válido corrompido en bytes al azar; no debe fallar ningún acceso (lo vigilan ASan/UBSan) */
    {
        unsigned seed = 12345u;
        long ok = 0;
        for (int it = 0; it < 300; it++) {
            build();
            for (int k = 0; k < 40 + it; k++) {
                seed = seed * 1664525u + 1013904223u;
                uint32_t a = 0x130000u + (seed >> 8) % 0x4000u;
                if (it % 3 == 0) a = 0x110000u + (seed >> 8) % 0x20000u;
                seed = seed * 1664525u + 1013904223u;
                ram[a] = (uint8_t)(seed >> 16);
            }
            ns_store_clear(st);
            rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
            NsRTri *t2 = NULL; NsAdaptStats a2;
            size_t n2 = ns_adapt_scene(&sc, st, &ao, &t2, &a2);
            (void)n2; free(t2);
            ok += rc == 0;
        }
        CHECK(ok == 300);
    }
    /* RAM totalmente aleatoria con el canal apuntando a la cola */
    for (int it = 0; it < 50; it++) {
        unsigned seed = 777u + (unsigned)it;
        for (size_t i = 0; i < NS_RAM_SIZE; i++) { seed = seed * 1664525u + 1013904223u; ram[i] = (uint8_t)(seed >> 24); }
        w16(CHAN + 0x56, 40); w32(CHAN + 0x58, QUEUE);
        ns_store_clear(st);
        rc = ns_extract(&sc, st, ram, NS_RAM_SIZE, NULL, 0, &es);
        NsRTri *t2 = NULL; NsAdaptStats a2;
        ns_adapt_scene(&sc, st, &ao, &t2, &a2); free(t2);
        CHECK(rc == 0);
    }
    ns_scene_free(&sc); ns_store_destroy(st); free(ram);
    if (fails) { printf("%d fallos\n", fails); return 1; }
    printf("test_ns_extract: todo correcto\n");
    return 0;
}
