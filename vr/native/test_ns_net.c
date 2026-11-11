/* Pruebas sintéticas (sin datos del juego) de la red de seguridad de vértices (ns_gpu_net_*, réplica en C de NativeShaders.metal) y de las soldaduras encadenadas:
 *  1. posición de mundo con soldaduras encadenadas (hijo -> padre -> abuelo) idéntica a la del adaptador de la referencia (ns_adapt);
 *  2. pose coherente: la red no colapsa nada ni marca ninguna pieza;
 *  3. padre "a medio actualizar" (matriz de otro instante, 5 m más allá): la soldadura se marca inconsistente; sin NOWELD la red colapsa los triángulos de la costura y con NOWELD no queda nada estirado;
 *  4. matriz corrupta (escala, NaN, traslación enorme): causa MAT;
 *  5. vértice basura en el almacén de GPU: los triángulos que lo usan se colapsan (y solo ésos); el escenario (NS_IF_STATIC: agua, cielo) nunca se toca;
 *  6. mallas de la mano articulada propia en todas sus poses: 0 colapsos; con la matriz de la manopla corrupta: MAT;
 *  7. interpolación de matrices (ns_xf_lerp): sin escalas ni traslaciones fuera del rango de los extremos;
 *  8. sesgo `raise` de las piezas dinámicas (mínimo K, max y no suma; negativo y escenario intactos);
 *  9. brazo estirado por la IK (hasta 2,5 veces): no es una matriz corrupta;
 * 10. reversos de las caras de una sola cara en el mundo en primera persona: se dibujan pero pierden el empate con el anverso que comparte su plano (dos hojas espalda con espalda);
 * 11. tamaño máximo del hardware con la cámara del juego: el triángulo de más de 1023 x 511 px de la pantalla del motor no se dibuja;
 * 12. una pieza sin malla (modelo nulo o sin caras: mesh_id = 0) no es una «malla ausente»: no retiene la escena. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ns_gpu.h"
#include "ns_adapt.h"
#include "ns_extract.h"
#include "ns_bridge.h"
#include "vr_hand_skin.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: %s ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng_s = 12345u;
static float rnd(void) { rng_s = rng_s * 1664525u + 1013904223u; return (float)(rng_s >> 8) / 16777216.0f; }
static float rr(float a, float b) { return a + (b - a) * rnd(); }

/* ---- escena sintética: 3 piezas encadenadas A (raíz) <- B <- C; cada malla es un prisma de 8 vértices; B y C llevan vértices soldados ---- */
typedef struct { NsVert v[8]; NsNorm n[8]; NsFace f[6]; NsMesh m; } Prism;
static void make_prism(Prism *p, float sx, float sy, float sz, uint64_t salt) {
    memset(p, 0, sizeof *p);
    int i = 0;
    for (int ix = 0; ix < 2; ix++) for (int iy = 0; iy < 2; iy++) for (int iz = 0; iz < 2; iz++) {
        p->v[i].x = (int16_t)(ix ? sx : -sx); p->v[i].y = (int16_t)(iy ? sy : -sy); p->v[i].z = (int16_t)(iz ? sz : -sz); p->v[i].weld = 0xFF;
        p->n[i].x = 2365; p->n[i].y = 2365; p->n[i].z = 2365;
        i++;
    }
    static const uint8_t quads[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
    for (int f = 0; f < 6; f++) for (int k = 0; k < 4; k++) { p->f[f].v[k] = quads[f][k]; p->f[f].n[k] = quads[f][k]; }
    p->m.n_verts = 8; p->m.n_norms = 8; p->m.n_faces = 6; p->m.verts = p->v; p->m.norms = p->n; p->m.faces = p->f;
    p->m.flags = 0;
    p->m.id = ns_mesh_content_hash(&p->m) ^ salt;
}
static void set_xf(NsXf *x, float yaw, float tx, float ty, float tz) {
    float c = cosf(yaw), s = sinf(yaw);
    float m[12] = {c, 0, s, tx, 0, 1, 0, ty, -s, 0, c, tz};
    memcpy(x->m, m, sizeof m);
}

typedef struct {
    NsScene sc; NsMeshStore *st; Prism pr[3];
    NsGpuVert *pool; NsGpuCorner *cor; NsGpuPiece tb[3];
    uint32_t vb[3], tb0[3], nt[3];
} W;

static void build(W *w, int is_static, float stale_parent_dx) {
    memset(w, 0, sizeof *w);
    ns_scene_alloc_exact(&w->sc, 2, 3, 4, 4);
    w->st = ns_store_create();
    make_prism(&w->pr[0], 200, 300, 150, 1);
    make_prism(&w->pr[1], 180, 280, 140, 2);
    make_prism(&w->pr[2], 160, 260, 130, 3);
    /* soldaduras: 4 vértices (0..3, cara -X) de B a vértices (4..7) de A; 4 de C a vértices de B que a su vez están soldados (encadenado) */
    for (int k = 0; k < 4; k++) { w->pr[1].v[k].weld = (uint8_t)(4 + k); }
    for (int k = 0; k < 4; k++) { w->pr[2].v[k].weld = (uint8_t)k; }          /* C -> vértices 0..3 de B, que están soldados a A */
    for (int i = 1; i < 3; i++) { w->pr[i].m.flags |= NS_MF_WELDED; w->pr[i].m.id = ns_mesh_content_hash(&w->pr[i].m); }
    for (int i = 0; i < 3; i++) ns_store_put(w->st, &w->pr[i].m);
    NsInstance *in = &w->sc.inst[0];
    memset(in, 0, sizeof *in);
    in->key = 0x80100000u; in->first_piece = 0; in->n_pieces = 3; in->light = -1; in->flags = (uint16_t)(is_static ? NS_IF_STATIC : 0);
    /* pose: A en el origen; B colgada de A de modo que sus vértices soldados coinciden con los de A (-X de B = +X de A); C encadenada a B */
    NsXf wa, wb, wc;
    set_xf(&wa, 0.0f, 10000, 2000, -5000);
    set_xf(&wb, 0.0f, 10000 + 200 + 180, 2000, -5000);        /* B: su cara -X (x = -180) cae en x = 10000 + 200 = cara +X de A */
    set_xf(&wc, 0.0f, 10000 + 200 + 180 + 180 + 160, 2000, -5000);
    wa.m[3] += 0; wb.m[3] += stale_parent_dx; wc.m[3] += stale_parent_dx;   /* B y C con otra posición (padre a medio actualizar respecto de A) */
    NsXf *xs[3] = {&wa, &wb, &wc};
    for (int i = 0; i < 3; i++) {
        NsPiece *p = &w->sc.piece[i];
        memset(p, 0, sizeof *p);
        p->world = *xs[i]; p->mesh_id = w->pr[i].m.id; p->parent = (int16_t)(i - 1);
    }
    w->sc.n_inst = 1; w->sc.n_piece = 3;
    w->sc.units_per_m = 1000.0f;
    uint32_t nvt = 0, ntt = 0;
    w->pool = (NsGpuVert *)calloc(64, sizeof(NsGpuVert)); w->cor = (NsGpuCorner *)calloc(3 * 64, sizeof(NsGpuCorner));
    for (int i = 0; i < 3; i++) {
        w->vb[i] = nvt; w->tb0[i] = ntt; w->nt[i] = ns_gpu_tri_count(&w->pr[i].m);
        ns_gpu_pack(&w->pr[i].m, NULL, w->pool + nvt, w->cor + 3 * ntt, nvt, NULL, NULL);
        nvt += w->pr[i].m.n_verts; ntt += w->nt[i];
        int pg = i > 0 ? i - 1 : -1;
        ns_gpu_make_piece(&w->sc, in, &w->sc.piece[i], 0, 0, w->vb[i], pg, pg >= 0 ? 8u : 0u, &w->tb[i]);
        w->tb[i].rest_r = ns_gpu_mesh_radius(&w->pr[i].m);
    }
}
static void destroy(W *w) { free(w->pool); free(w->cor); ns_scene_free(&w->sc); }

static uint32_t net_all(W *w, float mpu, NsGpuNetTris *acc) {
    uint32_t n = 0;
    for (int i = 0; i < 3; i++) n += ns_gpu_net_tris(w->pool, w->cor + 3 * w->tb0[i], w->nt[i], w->tb, (uint32_t)i, mpu, acc);
    return n;
}

/* 1. posición de mundo con soldaduras encadenadas = ns_adapt */
static void test_chain(void) {
    W w; build(&w, 0, 0.0f);
    NsAdaptOpts opt; memset(&opt, 0, sizeof opt);
    NsAdaptSrc *src = NULL; opt.src_out = &src;
    NsRTri *tr = NULL; NsAdaptStats as;
    size_t nt = ns_adapt_scene(&w.sc, w.st, &opt, &tr, &as);
    CHECK(nt > 0, "adapt devolvió %zu", nt);
    CHECK(as.welded_verts > 0, "sin soldaduras resueltas");
    int cmp = 0; double maxd = 0;
    for (size_t t = 0; t < nt; t++) {
        const NsMesh *m = &w.pr[src[t].piece].m;
        const NsFace *f = &m->faces[src[t].face];
        static const int qa[2][3] = {{0, 1, 3}, {1, 3, 2}};
        const int tri = (f->flags & NS_FF_TRI) != 0;
        for (int c = 0; c < 3; c++) {
            int ci = tri ? c : qa[src[t].half][c];
            float p[3];
            ns_gpu_vert_world(w.pool, w.tb, src[t].piece, w.vb[src[t].piece] + f->v[ci], p);
            for (int a = 0; a < 3; a++) { double d = fabs(p[a] - tr[t].v[c].p[a]); if (d > maxd) maxd = d; }
            cmp++;
        }
    }
    CHECK(maxd < 1e-3, "posición de mundo distinta de la referencia: %.4f unidades", maxd);
    printf("  encadenadas: %d vértices comparados con ns_adapt, error máx. %.2e unidades, soldaduras resueltas %zu\n", cmp, maxd, as.welded_verts);
    /* la cadena tiene de verdad dos niveles: un vértice de C cae donde el vértice (soldado) de B, que cae donde el de A */
    float pc[3], pa[3];
    ns_gpu_vert_world(w.pool, w.tb, 2, w.vb[2] + 0, pc);
    ns_gpu_vert_world(w.pool, w.tb, 0, w.vb[0] + 4, pa);
    CHECK(fabsf(pc[0] - pa[0]) < 1e-3f && fabsf(pc[1] - pa[1]) < 1e-3f && fabsf(pc[2] - pa[2]) < 1e-3f, "cadena de 2 niveles: C(%.1f %.1f %.1f) != A(%.1f %.1f %.1f)", pc[0], pc[1], pc[2], pa[0], pa[1], pa[2]);
    free(tr); free(src); ns_store_destroy(w.st); destroy(&w);
}

/* 2. pose coherente: nada colapsa */
static void test_coherent(void) {
    W w; build(&w, 0, 0.0f);
    NsGpuNetTris acc; memset(&acc, 0, sizeof acc);
    CHECK(net_all(&w, 0.001f, &acc) == 0, "pose coherente colapsada: lejos=%u arista=%u", acc.bad_far, acc.bad_edge);
    for (int i = 0; i < 3; i++) { NsGpuNetPiece np; CHECK(ns_gpu_net_piece(w.pool, w.tb, (uint32_t)i, 8, 0.001f, &np) == 0, "pieza %d marcada (escala %.2f salto %.2f m)", i, np.max_scale, np.max_gap_m); }
    CHECK(acc.tris == 3 * 12, "triángulos vigilados %u", acc.tris);
    ns_store_destroy(w.st); destroy(&w);
}

/* 3. padre a medio actualizar */
static void test_stale_parent(void) {
    W w; build(&w, 0, 5000.0f);   /* B y C 5 m más allá de A (1 unidad = 1 mm en esta escena: mpu = 0,001) */
    NsGpuNetPiece np;
    uint32_t c1 = ns_gpu_net_piece(w.pool, w.tb, 1, 8, 0.001f, &np);
    CHECK((c1 & NSG_NET_WELD) != 0, "B: soldadura no detectada (salto %.2f m)", np.max_gap_m);
    CHECK(np.max_gap_m > 4.0f && np.max_gap_m < 6.0f, "salto medido %.2f m", np.max_gap_m);
    CHECK(np.n_welded == 4, "soldados de B: %u", np.n_welded);
    CHECK(ns_gpu_net_piece(w.pool, w.tb, 0, 8, 0.001f, &np) == 0, "A marcada");
    NsGpuNetTris acc; memset(&acc, 0, sizeof acc);
    uint32_t n_before = net_all(&w, 0.001f, &acc);
    CHECK(n_before > 0, "sin NOWELD la costura estirada debía colapsar triángulos");
    CHECK(acc.bad_edge > 0, "causa arista: %u", acc.bad_edge);
    printf("  padre a medio actualizar: sin NOWELD %u triángulos colapsados (arista %u, lejos %u, peor exceso %.2f m, peor lejanía %.2f m)\n", n_before, acc.bad_edge, acc.bad_far, acc.worst_edge_m, acc.worst_far_m);
    /* con NOWELD (lo que hace el renderer): la pieza se dibuja rígida y no queda nada estirado */
    w.tb[1].flags |= NSG_PF_NOWELD;
    w.tb[2].flags |= NSG_PF_NOWELD;
    memset(&acc, 0, sizeof acc);
    CHECK(net_all(&w, 0.001f, &acc) == 0, "con NOWELD aún colapsa %u", acc.bad_edge + acc.bad_far);
    float p[3], own[3];
    ns_gpu_vert_world(w.pool, w.tb, 1, w.vb[1] + 0, p);
    own[0] = w.tb[1].w[0] * w.pool[w.vb[1]].p[0] + w.tb[1].w[1] * w.pool[w.vb[1]].p[1] + w.tb[1].w[2] * w.pool[w.vb[1]].p[2] + w.tb[1].w[3];
    CHECK(fabsf(p[0] - own[0]) < 1e-3f, "NOWELD no usa la posición propia");
    ns_store_destroy(w.st); destroy(&w);
}

/* 4. matriz corrupta */
static void test_bad_matrix(void) {
    W w; build(&w, 0, 0.0f);
    NsGpuNetPiece np;
    w.tb[0].w[0] = 3.0f;                                  /* columna de norma 3 */
    CHECK((ns_gpu_net_piece(w.pool, w.tb, 0, 8, 0.001f, &np) & NSG_NET_MAT) != 0, "escala 3 no detectada (%.2f)", np.max_scale);
    w.tb[0].w[0] = 1.0f; w.tb[0].w[5] = NAN;
    CHECK((ns_gpu_net_piece(w.pool, w.tb, 0, 8, 0.001f, &np) & NSG_NET_MAT) != 0, "NaN no detectado");
    w.tb[0].w[5] = 1.0f; w.tb[0].w[11] = 3.0e9f;
    CHECK((ns_gpu_net_piece(w.pool, w.tb, 0, 8, 0.001f, &np) & NSG_NET_MAT) != 0, "traslación enorme no detectada");
    w.tb[0].w[11] = -5000.0f;
    CHECK(ns_gpu_net_piece(w.pool, w.tb, 0, 8, 0.001f, &np) == 0, "matriz sana marcada");
    /* una matriz de sombra aplastada (escala 0,06) es legítima */
    w.tb[0].w[0] = 0.06f; w.tb[0].w[10] = 0.06f;
    CHECK((ns_gpu_net_piece(w.pool, w.tb, 0, 8, 0.001f, &np) & NSG_NET_MAT) == 0, "escala pequeña marcada");
    ns_store_destroy(w.st); destroy(&w);
}

/* 5. vértice basura en el almacén de la GPU; el escenario no se toca */
static void test_garbage_vertex(void) {
    for (int is_static = 0; is_static < 2; is_static++) {
        W w; build(&w, is_static, 0.0f);
        /* el vértice 5 de la malla A (basura: lejos) */
        w.pool[w.vb[0] + 5].p[0] = 30000.0f; w.pool[w.vb[0] + 5].p[1] = -30000.0f; w.pool[w.vb[0] + 5].p[2] = 30000.0f;
        uint32_t users = 0;
        for (uint32_t t = 0; t < w.nt[0]; t++) for (int k = 0; k < 3; k++) if (w.cor[3 * (w.tb0[0] + t) + (uint32_t)k].v == w.vb[0] + 5) { users++; break; }
        NsGpuNetTris acc; memset(&acc, 0, sizeof acc);
        uint32_t n = ns_gpu_net_tris(w.pool, w.cor + 3 * w.tb0[0], w.nt[0], w.tb, 0, 0.001f, &acc);
        if (is_static) CHECK(n == 0 && acc.tris == 0, "escenario vigilado: %u", n);
        else {
            CHECK(n == users && users > 0, "colapsados %u, usan el vértice %u", n, users);
            CHECK(acc.bad_far >= users && acc.worst_far_m > 20.0f, "lejos=%u peor %.1f m", acc.bad_far, acc.worst_far_m);
            printf("  vértice basura: %u de %u triángulos colapsados (lejos=%u, arista=%u, peor lejanía %.1f m)\n", n, w.nt[0], acc.bad_far, acc.bad_edge, acc.worst_far_m);
        }
        /* sin radio de reposo (rest_r = 0) no hay referencia de lejanía: la prueba de arista compara con el mismo vértice basura del almacén y no ve nada (el radio sale de la malla de la CPU, no del almacén) */
        if (!is_static) {
            w.tb[0].rest_r = 0.0f; memset(&acc, 0, sizeof acc);
            n = ns_gpu_net_tris(w.pool, w.cor + 3 * w.tb0[0], w.nt[0], w.tb, 0, 0.001f, &acc);
            CHECK(n == 0 && acc.bad_far == 0, "rest_r=0: n=%u far=%u edge=%u", n, acc.bad_far, acc.bad_edge);
        }
        ns_store_destroy(w.st); destroy(&w);
    }
}

/* 6. mano propia con skin (vr_hand_skin) en todas sus poses: ninguna pieza ni triángulo es rechazado por la red de seguridad */
static void test_hand(void) {
    static float pos[VRHA_NV][3];
    for (int side = -1; side <= 1; side += 2) {
        long tris = 0, bad = 0; float worst = 0;
        for (int gi = 0; gi <= 10; gi++) {
            float xf[VRHS_J][12], c[5];
            for (int i = 0; i < 5; i++) c[i] = (float)gi / 10.0f;
            vrhs_pose(c, 0.0f, side, xf);
            vrhs_skin(xf, side, pos, NULL);
            for (int k = 0; k < VRHA_NP; k++) {
                const VrHaPiece *pc = &vrha_piece[k];
                static NsVert v[256]; static NsFace f[VRHA_NT]; NsNorm n[1] = {{0, 0, 4096, 0}};
                memset(f, 0, sizeof f);
                for (int i = 0; i < pc->nv; i++) { const float *p = pos[pc->v0 + i]; v[i].x = (int16_t)lrintf(p[0]); v[i].y = (int16_t)lrintf(p[1]); v[i].z = (int16_t)lrintf(p[2]); v[i].weld = 0xFF; v[i].pad = 0; }
                for (int t = 0; t < pc->nt; t++) { const uint8_t *tr = vrha_tri[pc->t0 + t]; f[t].v[0] = tr[0]; f[t].v[1] = tr[2]; f[t].v[2] = tr[1]; f[t].v[3] = tr[1]; f[t].flags = NS_FF_TRI; }
                NsMesh m; memset(&m, 0, sizeof m);
                m.n_verts = pc->nv; m.n_norms = 1; m.n_faces = pc->nt; m.verts = v; m.norms = n; m.faces = f;
                uint32_t nt = ns_gpu_tri_count(&m);
                static NsGpuVert pv[256]; static NsGpuCorner pcn[3 * VRHA_NT];
                ns_gpu_pack(&m, NULL, pv, pcn, 0, NULL, NULL);
                /* matriz de la manopla (brazo en otra parte del mundo, 1 unidad = 1 mm) */
                NsGpuPiece P; memset(&P, 0, sizeof P);
                const float gl[12] = {0.8f, 0.0f, 0.6f, 12345.0f, 0.0f, 1.0f, 0.0f, -2000.0f, -0.6f, 0.0f, 0.8f, 777.0f};
                memcpy(P.w, gl, sizeof gl);
                P.flags = NSG_PF_DRAW | NSG_PF_NET; P.parent = -1; P.rest_r = ns_gpu_mesh_radius(&m);
                NsGpuNetTris acc; memset(&acc, 0, sizeof acc);
                uint32_t nb = ns_gpu_net_tris(pv, pcn, nt, &P, 0, 0.001f, &acc);
                tris += nt; bad += nb; if (acc.worst_far_m > worst) worst = acc.worst_far_m;
                NsGpuNetPiece np;
                CHECK(ns_gpu_net_piece(pv, &P, 0, m.n_verts, 0.001f, &np) == 0, "pieza %d cierre %d: causas %u escala %.3f", k, gi, np.causes, np.max_scale);
                if (gi == 5 && k == 1) {   /* la matriz de la manopla corrupta: la pieza no se dibuja */
                    P.w[0] *= 5.0f; P.w[4] *= 5.0f; P.w[8] *= 5.0f;
                    CHECK((ns_gpu_net_piece(pv, &P, 0, m.n_verts, 0.001f, &np) & NSG_NET_MAT) != 0, "manopla corrupta no detectada (escala %.2f)", np.max_scale);
                }
            }
        }
        CHECK(bad == 0, "mano %s: %ld de %ld triángulos colapsados", side > 0 ? "derecha" : "izquierda", bad, tris);
        printf("  mano %s: %ld triángulos en 11 poses x %d piezas, 0 colapsados esperados (%ld), peor lejanía al origen %.3f m\n", side > 0 ? "derecha" : "izquierda", tris, VRHA_NP, bad, worst);
    }
}

/* 7. interpolación de matrices acotada */
static void test_lerp(void) {
    long n = 0, bad = 0; float worst = 0;
    for (int it = 0; it < 20000; it++) {
        NsXf a, b, o;
        float sa = it % 5 == 0 ? rr(0.05f, 1.0f) : 1.0f, sb = it % 7 == 0 ? rr(0.05f, 1.0f) : 1.0f;
        float ya = rr(-3.2f, 3.2f), yb = it % 3 == 0 ? ya + rr(3.0f, 3.3f) : rr(-3.2f, 3.2f), pa = rr(-1.5f, 1.5f), pb = rr(-1.5f, 1.5f);
        for (int k = 0; k < 2; k++) {
            float yaw = k ? yb : ya, pit = k ? pb : pa, s = k ? sb : sa, cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pit), sp = sinf(pit);
            float m[12] = {cy * s, sy * sp * 1.0f, sy * cp, rr(-30000, 30000), 0, cp, -sp, rr(-3000, 3000), -sy * s, cy * sp, cy * cp, rr(-30000, 30000)};
            memcpy((k ? &b : &a)->m, m, sizeof m);
        }
        float t = rnd();
        ns_xf_lerp(&a, &b, t, &o);
        float mx = 0, mn_all = 0;
        for (int c = 0; c < 3; c++) {
            float na = sqrtf(a.m[c] * a.m[c] + a.m[4 + c] * a.m[4 + c] + a.m[8 + c] * a.m[8 + c]), nb = sqrtf(b.m[c] * b.m[c] + b.m[4 + c] * b.m[4 + c] + b.m[8 + c] * b.m[8 + c]);
            float no = sqrtf(o.m[c] * o.m[c] + o.m[4 + c] * o.m[4 + c] + o.m[8 + c] * o.m[8 + c]);
            float hi = fmaxf(na, nb) * 1.02f + 0.02f;
            if (!(no <= hi)) bad++;
            if (no > mx) mx = no;
            (void)mn_all;
        }
        for (int i = 0; i < 3; i++) {
            float lo = fminf(a.m[4 * i + 3], b.m[4 * i + 3]) - 1.0f, hi = fmaxf(a.m[4 * i + 3], b.m[4 * i + 3]) + 1.0f;
            if (!(o.m[4 * i + 3] >= lo && o.m[4 * i + 3] <= hi)) bad++;
        }
        for (int i = 0; i < 12; i++) if (!isfinite(o.m[i])) bad++;
        if (mx > worst) worst = mx;
        n++;
    }
    CHECK(bad == 0, "interpolación fuera de rango: %ld de %ld", bad, n);
    printf("  interpolación: %ld pares aleatorios (rotaciones de hasta 180 grados, escalas 0,05..1), 0 fuera de rango esperados (%ld), norma máx. de columna %.3f\n", n, bad, worst);
}

/* 8. el sesgo de orden llega tal cual (ni el escenario ni los objetos dinámicos reciben adelantos propios: un adelanto global de los dinámicos los sacaba por delante de las paredes y recortaba
 * el cuerpo propio contra el plano cercano) y la CLUT térmica solo se marca con la visión térmica activa en objetos que la admiten */
static void test_raise_and_ir(void) {
    W w; build(&w, 0, 0.0f);
    NsGpuPiece o;
    NsInstance *in = &w.sc.inst[0];
    in->flags = 0;
    ns_gpu_make_piece(&w.sc, in, &w.sc.piece[0], 0, 0, 0, -1, 0, &o);
    CHECK(o.raise == 0.0f, "dinámica sin sesgo: raise=%f", o.raise);
    ns_gpu_make_piece(&w.sc, in, &w.sc.piece[0], 0, 1000, 0, -1, 0, &o);
    CHECK(o.raise == 1000.0f, "dinámica con sesgo: tal cual, raise=%f", o.raise);
    ns_gpu_make_piece(&w.sc, in, &w.sc.piece[0], 0, -500, 0, -1, 0, &o);
    CHECK(o.raise == -500.0f, "sesgo negativo tal cual, raise=%f", o.raise);
    in->flags = NS_IF_STATIC;
    ns_gpu_make_piece(&w.sc, in, &w.sc.piece[0], 0, -750, 0, -1, 0, &o);
    CHECK(o.raise == -750.0f, "estática conserva su sesgo: raise=%f", o.raise);
    /* visión térmica */
    in->flags = NS_IF_THERMAL_OK;
    ns_gpu_make_piece(&w.sc, in, &w.sc.piece[0], 0, 0, 0, -1, 0, &o);
    CHECK(!(o.flags & NSG_PF_IRCLUT), "sin visión térmica no hay CLUT térmica");
    w.sc.valid |= NS_SV_FX; w.sc.screen.flags = NS_SFX_MODE_A;
    ns_gpu_make_piece(&w.sc, in, &w.sc.piece[0], 0, 0, 0, -1, 0, &o);
    CHECK(!(o.flags & NSG_PF_IRCLUT), "visión nocturna: sin CLUT térmica");
    w.sc.screen.flags = NS_SFX_MODE_B;
    ns_gpu_make_piece(&w.sc, in, &w.sc.piece[0], 0, 0, 0, -1, 0, &o);
    CHECK((o.flags & NSG_PF_IRCLUT) != 0, "visión térmica + objeto que la admite: CLUT térmica");
    in->flags = 0;
    ns_gpu_make_piece(&w.sc, in, &w.sc.piece[0], 0, 0, 0, -1, 0, &o);
    CHECK(!(o.flags & NSG_PF_IRCLUT), "objeto sin textura térmica: su CLUT");
    w.sc.valid &= ~NS_SV_FX; in->flags = NS_IF_THERMAL_OK;
    ns_gpu_make_piece(&w.sc, in, &w.sc.piece[0], 0, 0, 0, -1, 0, &o);
    CHECK(!(o.flags & NSG_PF_IRCLUT), "sin efectos de pantalla válidos (NS_SV_FX) no se lee el modo");
    destroy(&w);
}

/* 9. la IK alarga los huesos del brazo hasta 2,5 veces (vr_body.c) sobre matrices del motor de escala ~1,003: la pieza se dibuja */
static void test_ik_stretch(void) {
    W w; build(&w, 0, 0.0f);
    NsGpuNetPiece np;
    for (float s = 1.0f; s <= 2.5001f; s += 0.25f) {
        w.tb[0].w[0] = s * 1.0034f;                       /* hueso alargado a lo largo de su eje */
        CHECK((ns_gpu_net_piece(w.pool, w.tb, 0, 8, 0.001f, &np) & NSG_NET_MAT) == 0, "brazo estirado x%.2f tratado como matriz corrupta (escala %.2f)", s, np.max_scale);
    }
    w.tb[0].w[0] = 3.0f;
    CHECK((ns_gpu_net_piece(w.pool, w.tb, 0, 8, 0.001f, &np) & NSG_NET_MAT) != 0, "escala 3 no detectada");
    ns_store_destroy(w.st); destroy(&w);
}

/* 10. reversos: dos hojas de una cara en el mismo plano, espalda con espalda, teseladas distinto (una pared vista por un lado y por el otro). Sin descarte de reversos (primera persona) el
 * anverso debe ganar SIEMPRE la prueba de profundidad (Z invertida, compare >=), se dibuje antes o después y con el error de redondeo que sea. */
static void test_back_faces(void) {
    const uint32_t one = NSG_CF_ONESIDED | NSG_CF_TEX, two = NSG_CF_TEX;
    CHECK(ns_gpu_back_z(NSG_CULL_NONE, one, 1) < 1.0f, "reverso de una cara sin descarte: debe alejarse (%f)", ns_gpu_back_z(NSG_CULL_NONE, one, 1));
    CHECK(ns_gpu_back_z(NSG_CULL_NONE, one, 0) == 1.0f, "anverso: sin tocar");
    CHECK(ns_gpu_back_z(NSG_CULL_NONE, two, 1) == 1.0f, "malla de doble cara: sin tocar");
    CHECK(ns_gpu_back_z(NSG_CULL_ALL, one, 1) == 1.0f, "con descarte de reversos (pantalla 3D) no se dibuja: nada que alejar");
    CHECK(ns_gpu_back_z(NSG_CULL_SEMI, one, 1) < 1.0f, "bajo el agua los reversos opacos se siguen dibujando: se alejan");
    CHECK(ns_gpu_back_z(NSG_CULL_SEMI, one | NSG_CF_SEMI, 1) == 1.0f, "bajo el agua los semitransparentes de una cara se descartan");
    /* pared en x = 0: la hoja A mira a +X (la ve el ojo), la hoja B a -X; el ojo en +X. Profundidad de recorte z/w con Z invertida y lejano infinito: cerca / distancia */
    const float eye[3] = {1500, 1600, 300};
    const float a0[3] = {0, 0, 0}, a1[3] = {0, 0, 4000}, a2[3] = {0, 3000, 0};          /* sentido tal que A es anverso desde +X */
    const int a_back = ns_gpu_tri_back(a0, a1, a2, eye), b_back = ns_gpu_tri_back(a0, a2, a1, eye);
    CHECK(a_back != b_back, "las dos hojas no pueden mirar al mismo lado");
    long ties = 0, lost = 0, n = 0;
    for (int i = 0; i < 20000; i++) {
        const float d = rr(0.12f, 60.0f);                 /* distancia de vista (m) del mismo punto de la pared por las dos hojas */
        const float noise = 1.0f + rr(-2e-5f, 2e-5f);     /* redondeo distinto de las dos teselaciones */
        const float za = (0.1f / d) * ns_gpu_back_z(NSG_CULL_NONE, one, a_back), zb = (0.1f / (d * noise)) * ns_gpu_back_z(NSG_CULL_NONE, one, b_back);
        const float front = a_back ? zb : za, back = a_back ? za : zb;
        n++; ties += front == back; lost += !(front > back);
    }
    CHECK(lost == 0, "el anverso pierde o empata la profundidad en %ld de %ld muestras (empates %ld)", lost, n, ties);
}

/* 11. tamaño máximo del hardware (vista del motor: x derecha, y abajo, z delante; h = píxeles por unidad de tangente) */
static void test_oversize(void) {
    const float H = 161.0f, HY = 161.0f * 66.0f / 64.0f;
    /* rejilla de un conducto a 13 cm de la cámara (medido en un volcado): 945 px de alto en la pantalla del motor */
    const float grate[3][3] = {{402, -85, 111}, {105, 329, 97}, {1, -388, 160}};
    CHECK(ns_gpu_tri_oversize(grate, H, HY) == 1, "triángulo de 945 px de alto: el hardware no lo dibuja");
    const float wall[3][3] = {{-2000, -1500, 4000}, {2000, -1500, 4000}, {-2000, 1500, 4000}};      /* 161 x 124 px */
    CHECK(ns_gpu_tri_oversize(wall, H, HY) == 0, "pared lejana descartada");
    const float wide[3][3] = {{-700, 0, 200}, {700, 0, 200}, {0, 100, 200}};                         /* 1127 px de ancho, 83 de alto */
    CHECK(ns_gpu_tri_oversize(wide, H, HY) == 1, "más de 1023 px de ancho: no se dibuja");
    const float tall[3][3] = {{0, -330, 200}, {10, 330, 200}, {100, 0, 200}};                        /* 548 px de alto */
    CHECK(ns_gpu_tri_oversize(tall, H, HY) == 1, "más de 511 px de alto: no se dibuja");
    const float fits[3][3] = {{-600, -300, 200}, {600, -300, 200}, {0, 300, 200}};                    /* 966 x 498 px */
    CHECK(ns_gpu_tri_oversize(fits, H, HY) == 0, "966 x 498 px cabe");
    /* las coordenadas saturan a -1024..1023 antes de medir: un vértice a 5000 px cuenta como 1023 */
    const float sat[3][3] = {{100, 0, 200}, {6300, 0, 200}, {100, 100, 200}};                         /* x: 80 y 5071 -> 1023: 943 px de ancho */
    CHECK(ns_gpu_tri_oversize(sat, H, HY) == 0, "ancho medido tras saturar");
}

/* 11b. la GTE de la cámara del juego satura la división en 2,0 cuando z <= H/2: el motor proyecta ese vértice como si estuviera en el plano z = H/2 (x e y de la vista intactas) y dibuja el polígono
 * igualmente (medido en volcados de cinemáticas: polígonos de la lista con vértices a z = 79, 28, -625 de H = 320). Solo no dibuja el que no tiene ningún vértice delante de la cámara. */
static void test_gte_clamp(void) {
    const float H = 320.0f, HY = 320.0f * 66.0f / 64.0f, zc = 0.5f * H;
    float out[3][3];
    /* un vértice a z = 79 (< 160): se mueve al plano z = 160 conservando x e y; los otros dos, intactos */
    const float one[3][3] = {{-298, -187, 246}, {348, 152, 1187}, {-300, 100, 79}};
    CHECK(ns_gpu_gte_tri(one, zc, H, HY, out) == NSG_GTE_DRAW, "un vértice cercano: se dibuja (antes se descartaba el triángulo entero)");
    CHECK(out[2][0] == -300 && out[2][1] == 100 && out[2][2] == zc, "vértice a z=79 -> (x, y, H/2): %f %f %f", out[2][0], out[2][1], out[2][2]);
    CHECK(out[0][2] == 246 && out[1][2] == 1187 && out[0][0] == -298 && out[1][1] == 152, "los vértices delante de H/2 no se tocan");
    /* un vértice detrás de la cámara (z < 0) con otros delante: también se proyecta con z = H/2 */
    const float beh[3][3] = {{100, 50, 800}, {-200, 40, 900}, {300, -30, -625}};
    CHECK(ns_gpu_gte_tri(beh, zc, H, HY, out) == NSG_GTE_DRAW && out[2][2] == zc && out[2][0] == 300, "vértice detrás de la cámara: z = H/2");
    /* ningún vértice delante (z <= 0 los tres): el motor lo descarta (código de pantalla común) */
    const float none[3][3] = {{100, 50, -10}, {-200, 40, 0}, {300, -30, -625}};
    CHECK(ns_gpu_gte_tri(none, zc, H, HY, out) == NSG_GTE_BEHIND, "los tres vértices detrás: no se dibuja");
    /* z entre 0 y H/2 los tres: se dibujan todos en el plano H/2 (la proyección es la de ese plano) */
    const float near3[3][3] = {{10, 10, 100}, {-10, 10, 50}, {0, -10, 5}};
    CHECK(ns_gpu_gte_tri(near3, zc, H, HY, out) == NSG_GTE_DRAW && out[0][2] == zc && out[1][2] == zc && out[2][2] == zc, "tres vértices entre 0 y H/2: se proyectan en H/2");
    /* el tamaño máximo del hardware se mide DESPUÉS del ajuste: x = 2000 a z = 10 cae en 2000 * 320 / 160 = 4000 px (satura a 1023); con otro vértice en x = -2000: 2047 px de ancho -> no se dibuja */
    const float big[3][3] = {{2000, 0, 10}, {-2000, 0, 10}, {0, 10, 400}};
    CHECK(ns_gpu_gte_tri(big, zc, H, HY, out) == NSG_GTE_OVERSIZE, "más de 1023 px de ancho tras el ajuste: lo descarta el hardware");
    /* sin ajuste (todos delante): igual que ns_gpu_tri_oversize */
    const float ok3[3][3] = {{-600, -300, 400}, {600, -300, 400}, {0, 300, 400}};
    CHECK(ns_gpu_gte_tri(ok3, zc, H, HY, out) == NSG_GTE_DRAW && out[0][2] == 400, "triángulo normal: se dibuja intacto");
}

/* 12. piezas sin malla */
static void test_empty_piece(void) {
    W w; build(&w, 0, 0.0f);
    CHECK(ns_gpu_scene_misses(&w.sc, w.st) == 0, "escena completa con mallas ausentes");
    w.sc.piece[2].mesh_id = 0; w.sc.piece[2].flags |= NS_PF_HIDDEN;       /* como la deja el extractor con un modelo nulo o sin caras */
    CHECK(ns_gpu_piece_has_mesh(&w.sc.piece[2]) == 0 && ns_gpu_piece_has_mesh(&w.sc.piece[1]) == 1, "pieza sin malla mal reconocida");
    CHECK(ns_gpu_scene_misses(&w.sc, w.st) == 0, "una pieza sin malla cuenta como malla ausente (retendría la escena)");
    w.sc.piece[1].mesh_id ^= 0x5555u;                                     /* malla que el almacén no tiene: ausente de verdad */
    CHECK(ns_gpu_scene_misses(&w.sc, w.st) == 1, "malla ausente de verdad no contada");
    ns_store_destroy(w.st); destroy(&w);
}

int main(void) {
    printf("test_ns_net: red de seguridad de vértices\n");
    test_chain();
    test_coherent();
    test_stale_parent();
    test_bad_matrix();
    test_garbage_vertex();
    test_hand();
    test_lerp();
    test_raise_and_ir();
    test_ik_stretch();
    test_back_faces();
    test_oversize();
    test_gte_clamp();
    test_empty_piece();
    printf("%d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
