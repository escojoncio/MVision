/* Pruebas de escritorio (sin GPU, sin datos del juego) del empaquetado del renderer nativo:
 *  1. emulación en C de ns_vertex (NativeShaders.metal) sobre los búferes de ns_gpu.c, comparada triángulo a triángulo con el adaptador independiente de la
 *     referencia (posiciones VR, colores de vértice con luz/niebla/color precalculado/welded);
 *  2. orden de los semitransparentes frente a una ordenación de fuerza bruta;
 *  El puente (ns_bridge) tiene sus propias pruebas: vr/native/integra.mk. */
#include "native_synth.h"
#include "ns_bridge.h"
#include "ns_gpu.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

typedef struct { float x, y, z; } V3;
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---------------- 1. emulación del vertex shader ---------------- */
typedef struct { float tovr[12]; float fog_fwd[4]; float fog_par[4]; } Uni;

static void mul34(const float *m, const float p[3], float o[3]) {
    o[0] = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
    o[1] = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
    o[2] = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
}

/* réplica de ns_vertex (sin proyección): posición VR y color byte */
static void emu_vertex(const NsGpuVert *verts, const NsGpuCorner *c, const NsGpuPiece *pieces, int pi, const Uni *U, float pos[3], float col[3]) {
    const NsGpuPiece *P = &pieces[pi];
    const NsGpuVert *V = &verts[c->v];
    float wp[3];
    mul34(P->w, V->p, wp);
    if (V->weld != NSG_NO_WELD && P->parent >= 0 && V->weld < P->parent_nverts) {
        const NsGpuPiece *Q = &pieces[P->parent];
        const NsGpuVert *PV = &verts[Q->vert_base + V->weld];
        mul34(Q->w, PV->p, wp);
    }
    float raw[3];
    if (c->flags & NSG_CF_PAINT) {
        raw[0] = (float)(c->paint & 255u); raw[1] = (float)((c->paint >> 8) & 255u); raw[2] = (float)((c->paint >> 16) & 255u);
    } else if (P->flags & NSG_PF_SHADE) {
        float inten[3];
        for (int l = 0; l < 3; l++) inten[l] = clampf(P->l[3 * l] * c->n[0] + P->l[3 * l + 1] * c->n[1] + P->l[3 * l + 2] * c->n[2], 0.0f, NSG_INTENSITY_MAX);
        for (int ch = 0; ch < 3; ch++) raw[ch] = P->k[4 * ch + 3] + P->k[4 * ch] * inten[0] + P->k[4 * ch + 1] * inten[1] + P->k[4 * ch + 2] * inten[2];
    } else raw[0] = raw[1] = raw[2] = 128.0f;
    float s = P->fog;
    if (P->flags & NSG_PF_VFOG) {
        float z = U->fog_fwd[0] * wp[0] + U->fog_fwd[1] * wp[1] + U->fog_fwd[2] * wp[2] + U->fog_fwd[3];
        s = 1.0f - clampf((z - U->fog_par[0]) / fmaxf(U->fog_par[1], 1e-3f), 0.0f, 1.0f);
    }
    for (int k = 0; k < 3; k++) col[k] = clampf(floorf(clampf(raw[k], 0, 255) * s), 0, 255);
    mul34(U->tovr, wp, pos);
}

static void test_emulation(void) {
    NsView view; memset(&view, 0, sizeof view);
    CHECK(ns_bridge_acquire(ns_synth_bridge(), ns_now_ns(), &view) == 1);
    const NsScene *sc = view.cur;
    const NsMeshStore *store = view.store;
    CHECK(sc != NULL);
    if (!sc) return;
    NsRTri *ref = NULL;
    size_t nref = ns_synth_ref_tris(&ref);
    CHECK(nref > 300);

    /* "pool" de vértices y esquinas como los de la GPU */
    enum { MAXV = 4096, MAXT = 4096 };
    NsGpuVert *verts = (NsGpuVert *)calloc(MAXV, sizeof(NsGpuVert));
    NsGpuCorner *corners = (NsGpuCorner *)calloc(3 * MAXT, sizeof(NsGpuCorner));
    uint32_t vbase[64]; uint32_t tbase[64]; uint64_t ids[64]; uint32_t nm = 0, vcur = 0, tcur = 0;
    for (uint32_t ii = 0; ii < sc->n_inst; ii++)
        for (uint32_t k = 0; k < sc->inst[ii].n_pieces; k++) {
            uint64_t id = sc->piece[sc->inst[ii].first_piece + k].mesh_id;
            int have = 0;
            for (uint32_t j = 0; j < nm; j++) if (ids[j] == id) have = 1;
            const NsMesh *m = ns_store_get(store, id);
            if (have || !m) continue;
            ids[nm] = id; vbase[nm] = vcur; tbase[nm] = tcur;
            uint32_t nt = ns_gpu_tri_count(m);
            ns_gpu_pack(m, ns_store_get_baked(store, id, 1), verts + vcur, corners + 3 * tcur, vcur, NULL, NULL);
            vcur += m->n_verts; tcur += nt; nm++;
        }
    NsGpuPiece *pieces = (NsGpuPiece *)calloc(sc->n_piece, sizeof(NsGpuPiece));
    for (uint32_t ii = 0; ii < sc->n_inst; ii++) {
        const NsInstance *in = &sc->inst[ii];
        for (uint32_t k = 0; k < in->n_pieces; k++) {
            uint32_t gi = in->first_piece + k;
            const NsPiece *p = &sc->piece[gi];
            const NsMesh *m = ns_store_get(store, p->mesh_id);
            if (!m) continue;
            uint32_t mi = 0;
            for (uint32_t j = 0; j < nm; j++) if (ids[j] == p->mesh_id) mi = j;
            int pg = -1; uint32_t pnv = 0;
            if (p->parent >= 0 && (uint32_t)p->parent < in->n_pieces) {
                const NsMesh *pm = ns_store_get(store, sc->piece[in->first_piece + (uint32_t)p->parent].mesh_id);
                if (pm) { pg = (int)(in->first_piece + (uint32_t)p->parent); pnv = pm->n_verts; }
            }
            ns_gpu_make_piece(sc, in, p, m->flags, m->depth_bias, vbase[mi], pg, pnv, &pieces[gi]);
            if (p->flags & NS_PF_HIDDEN) pieces[gi].flags &= ~NSG_PF_DRAW;
        }
    }
    Uni U;
    memcpy(U.tovr, sc->to_vr, sizeof U.tovr);
    U.fog_fwd[0] = sc->cam.rot[2]; U.fog_fwd[1] = sc->cam.rot[5]; U.fog_fwd[2] = sc->cam.rot[8];
    U.fog_fwd[3] = -(U.fog_fwd[0] * sc->cam.eye[0] + U.fog_fwd[1] * sc->cam.eye[1] + U.fog_fwd[2] * sc->cam.eye[2]);
    U.fog_par[0] = sc->fog.start; U.fog_par[1] = sc->fog.len; U.fog_par[2] = (float)sc->fog.active; U.fog_par[3] = 0.05f / sc->units_per_m;

    size_t ti = 0; long badpos = 0, badcol = 0, maxcol = 0; double maxpos = 0;
    int semi_pieces = 0;
    for (uint32_t gi = 0; gi < sc->n_piece; gi++) {
        if (!(pieces[gi].flags & NSG_PF_DRAW)) continue;
        uint32_t mi = 0;
        for (uint32_t j = 0; j < nm; j++) if (vbase[j] == pieces[gi].vert_base) mi = j;
        const NsMesh *m = ns_store_get(store, ids[mi]);
        uint32_t nt = ns_gpu_tri_count(m);
        if (m->flags & NS_MF_BLEND) semi_pieces++;
        for (uint32_t t = 0; t < nt; t++, ti++) {
            if (ti >= nref) { CHECK(0); break; }
            for (int k = 0; k < 3; k++) {
                float pos[3], col[3];
                emu_vertex(verts, &corners[3 * (tbase[mi] + t) + (uint32_t)k], pieces, (int)gi, &U, pos, col);
                const NsRVert *rv = &ref[ti].v[k];
                for (int a = 0; a < 3; a++) { double d = fabs(pos[a] - rv->p[a]); if (d > maxpos) maxpos = d; if (d > 2e-4) badpos++; }
                const uint8_t rc[3] = {rv->r, rv->g, rv->b};
                for (int a = 0; a < 3; a++) { long d = labs((long)col[a] - (long)rc[a]); if (d > maxcol) maxcol = d; if (d > 1) badcol++; }
            }
        }
    }
    printf("emulación: %zu triángulos de referencia, %d piezas semi, error máx. posición %.2e m, color %ld niveles\n", nref, semi_pieces, maxpos, maxcol);
    CHECK(ti == nref);
    CHECK(badpos == 0);
    CHECK(badcol == 0);
    CHECK(semi_pieces == 2);
    free(verts); free(corners); free(pieces); free(ref);
}

/* ---------------- 2. orden de los semitransparentes ---------------- */
typedef struct { float d; uint32_t seq, piece, tri; } Bf;
static int bf_cmp(const void *a, const void *b) {
    const Bf *x = (const Bf *)a, *y = (const Bf *)b;
    if (x->d != y->d) return x->d > y->d ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}
/* Un cuadrilátero se parte por la diagonal B-D (la del hardware), no por A-C; un triángulo (C == D) es (A,B,C). */
static void test_quad_diagonal(void) {
    NsVert v[4] = {{0, 0, 0, 0xFF, 0}, {100, 0, 0, 0xFF, 0}, {100, 0, 100, 0xFF, 0}, {0, 50, 100, 0xFF, 0}};   /* perímetro A,B,C,D alabeado */
    NsFace f[2]; memset(f, 0, sizeof f);
    for (int k = 0; k < 4; k++) { f[0].v[k] = (uint8_t)k; f[0].u[k] = (uint8_t)(10 * k); }
    f[1] = f[0]; f[1].flags = NS_FF_TRI; f[1].v[3] = f[1].v[2];
    NsMesh m; memset(&m, 0, sizeof m);
    m.n_verts = 4; m.n_faces = 2; m.verts = v; m.faces = f;
    CHECK(ns_gpu_tri_count(&m) == 3);
    NsGpuVert gv[4]; NsGpuCorner c[9];
    ns_gpu_pack(&m, NULL, gv, c, 0, NULL, NULL);
    /* cuadrilátero: (A,B,D) y (B,C,D) -> comparten B-D */
    CHECK(c[0].v == 0 && c[1].v == 1 && c[2].v == 3);
    CHECK(c[3].v == 1 && c[4].v == 2 && c[5].v == 3);
    /* cada esquina conserva su propio UV (el de su vértice de la cara, no el de otra) */
    CHECK(c[2].u == 30 && c[3].u == 10 && c[4].u == 20 && c[5].u == 30);
    /* triángulo: (A,B,C) */
    CHECK(c[6].v == 0 && c[7].v == 1 && c[8].v == 2);
}

static void test_sort(void) {
    srand(7);
    enum { NP = 5, NT = 200 };
    static float cen[NP][NT * 3]; static uint8_t abr[NP][NT];
    NsGpuSemiSrc src[NP];
    for (int p = 0; p < NP; p++) {
        for (int t = 0; t < NT; t++) {
            for (int a = 0; a < 3; a++) cen[p][3 * t + a] = (float)(rand() % 2001 - 1000);
            if (t % 7 == 0) { cen[p][3 * t + 2] = 500.0f; }   /* empates de profundidad */
            abr[p][t] = (uint8_t)(rand() & 3);
        }
        src[p].piece = (uint32_t)p; src[p].tri_base = (uint32_t)(p * NT); src[p].ntri = NT; src[p].centroid = cen[p]; src[p].abr = abr[p];
        src[p].zrow[0] = 0; src[p].zrow[1] = 0; src[p].zrow[2] = 1; src[p].zrow[3] = (float)(p * 3);   /* profundidad = z + 3p */
    }
    Bf *bf = (Bf *)malloc(NP * NT * sizeof *bf);
    uint32_t seq = 0; size_t n = 0;
    for (int p = 0; p < NP; p++) for (int t = 0; t < NT; t++, seq++) { bf[n].d = cen[p][3 * t + 2] + (float)(p * 3); bf[n].seq = seq; bf[n].piece = (uint32_t)p; bf[n].tri = (uint32_t)(p * NT + t); n++; }
    qsort(bf, n, sizeof *bf, bf_cmp);
    NsGpuOrder *out = (NsGpuOrder *)malloc(NP * NT * sizeof *out); uint8_t *ab = (uint8_t *)malloc(NP * NT);
    NsGpuSortScratch scr; memset(&scr, 0, sizeof scr);
    size_t m = ns_gpu_sort_semi(src, NP, out, ab, NP * NT, &scr);
    CHECK(m == n);
    int ok = 1;
    for (size_t i = 0; i < n && i < m; i++) if (out[i].piece != bf[i].piece || out[i].tri != bf[i].tri) ok = 0;
    CHECK(ok);
    CHECK(ns_gpu_sort_semi(src, NP, out, ab, 17, &scr) == 17);   /* capacidad: quedan los 17 MÁS CERCANOS (el final de la lista) */
    int okc = 1;
    for (size_t i = 0; i < 17; i++) if (out[i].piece != bf[n - 17 + i].piece || out[i].tri != bf[n - 17 + i].tri) okc = 0;
    CHECK(okc);
    CHECK(ns_gpu_sort_semi(src, 0, out, ab, 10, &scr) == 0);
    CHECK(ns_gpu_sort_semi(src, NP, out, ab, NP * NT, NULL) == n);   /* sin scratch propio */
    ns_gpu_sort_free(&scr);
    free(bf); free(out); free(ab);
}

/* Diagnóstico: partir triángulos largos de la malla empaquetada y recuento de triángulos largos / que cruzan el plano cercano. */
static void test_split(void) {
    NsGpuVert v[5];
    memset(v, 0, sizeof v);
    float P[5][3] = {{0, 0, 0}, {63000, 0, 0}, {0, 0, 3000}, {0, 100, 0}, {100, 0, 0}};   /* unidades locales (1 = 1 mm) */
    for (int i = 0; i < 5; i++) { v[i].p[0] = P[i][0]; v[i].p[1] = P[i][1]; v[i].p[2] = P[i][2]; v[i].weld = NSG_NO_WELD; }
    NsGpuCorner c[9];
    memset(c, 0, sizeof c);
    uint32_t idx[9] = {0, 1, 2, 0, 3, 4, 0, 1, 2};
    for (int i = 0; i < 9; i++) { c[i].v = idx[i]; c[i].n[1] = 1.0f; c[i].u = (uint8_t)(i * 10); c[i].t = (uint8_t)(i * 20); c[i].tpage = 5; c[i].clut = 6; c[i].flags = NSG_CF_TEX | NSG_CF_PAINT; c[i].paint = 0x808080u; }
    float cen[9] = {21000, 0, 1000, 33, 33, 0, 21000, 0, 1000};
    uint8_t abr[3] = {0, 1, 2};
    NsGpuSplit sp;
    /* triángulos 0 y 2 largos (63 m), el 1 pequeño; umbral 2 m = 2000 unidades */
    int rc = ns_gpu_split(v, 5, c, 3, cen, abr, 2000.0f, 100000, &sp);
    CHECK(rc == 0);
    if (rc == 0) {
        CHECK(sp.n_split == 2 && sp.ntris > 3 && sp.nverts > 5 && sp.n_skip_cap == 0);
        float mx = 0;
        int order = 1;
        for (uint32_t t = 0; t < sp.ntris; t++) {
            for (int k = 0; k < 3; k++) {
                const NsGpuCorner *a = &sp.corners[3 * t + k], *b = &sp.corners[3 * t + (k + 1) % 3];
                CHECK(a->v < sp.nverts && a->tpage == 5 && a->clut == 6);
                float dx = sp.verts[a->v].p[0] - sp.verts[b->v].p[0], dy = sp.verts[a->v].p[1] - sp.verts[b->v].p[1], dz = sp.verts[a->v].p[2] - sp.verts[b->v].p[2];
                float e = sqrtf(dx * dx + dy * dy + dz * dz);
                if (e > mx) mx = e;
                float nl = sqrtf(a->n[0] * a->n[0] + a->n[1] * a->n[1] + a->n[2] * a->n[2]);
                CHECK(fabsf(nl - 1.0f) < 1e-3f);
                CHECK((a->paint & 255u) == 0x80u);
            }
            if (sp.abr[t] < abr[0] || sp.abr[t] > abr[2]) order = 0;
        }
        CHECK(mx <= 2000.0f + 1.0f);
        CHECK(order);
        /* el triángulo pequeño sigue ahí, en el mismo orden relativo: abr 0.. luego 1 (un solo triángulo) luego 2.. */
        uint32_t n1 = 0, first1 = 0;
        for (uint32_t t = 0; t < sp.ntris; t++) if (sp.abr[t] == 1) { n1++; first1 = t; }
        CHECK(n1 == 1);
        uint32_t before = 0; for (uint32_t t = 0; t < first1; t++) if (sp.abr[t] == 0) before++;
        CHECK(before == first1 && before > 1);
        /* los originales conservan sus índices */
        CHECK(memcmp(sp.verts, v, sizeof v) == 0);
        /* los centroides son la media de los vértices */
        int okc = 1;
        for (uint32_t t = 0; t < sp.ntris; t++) {
            float ex = 0; for (int k = 0; k < 3; k++) ex += sp.verts[sp.corners[3 * t + k].v].p[0];
            if (fabsf(sp.centroid[3 * t] - ex / 3.0f) > 1.0f) okc = 0;
        }
        CHECK(okc);
        NsGpuCorner *cc = (NsGpuCorner *)malloc(3u * sp.ntris * sizeof *cc);
        memcpy(cc, sp.corners, 3u * sp.ntris * sizeof *cc);
        ns_gpu_rebase_corners(cc, 3u * sp.ntris, 1000);
        CHECK(cc[0].v == sp.corners[0].v + 1000);
        free(cc);
        ns_gpu_split_free(&sp);
    }
    /* tope: salida <= 10 triángulos */
    rc = ns_gpu_split(v, 5, c, 3, cen, abr, 2000.0f, 10, &sp);
    CHECK(rc == 0 && sp.ntris <= 10 && sp.n_skip_cap >= 1);
    if (rc == 0) ns_gpu_split_free(&sp);
    /* sin centroides ni abr */
    rc = ns_gpu_split(v, 5, c, 3, NULL, NULL, 2000.0f, 100000, &sp);
    CHECK(rc == 0 && sp.centroid == NULL && sp.abr == NULL);
    if (rc == 0) ns_gpu_split_free(&sp);
    /* soldados: no se tocan */
    v[1].weld = 3;
    rc = ns_gpu_split(v, 5, c, 3, cen, abr, 2000.0f, 100000, &sp);
    CHECK(rc == 1 && sp.n_skip_weld == 2);
    v[1].weld = NSG_NO_WELD;
    CHECK(ns_gpu_split(v, 5, c, 3, cen, abr, 1e9f, 1000, &sp) == 1);
    CHECK(ns_gpu_split(v, 5, c, 3, cen, abr, 0.0f, 1000, &sp) == -1);
    /* recuento: matriz identidad (local = ojo, en metros), plano cercano 0,1 m */
    float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    NsGpuVert w[3]; memset(w, 0, sizeof w);
    float W[3][3] = {{0, 0, -1}, {30, 0, -80}, {0, 5, 2}};
    for (int i = 0; i < 3; i++) { w[i].p[0] = W[i][0]; w[i].p[1] = W[i][1]; w[i].p[2] = W[i][2]; w[i].weld = NSG_NO_WELD; }
    NsGpuCorner wc[3]; memset(wc, 0, sizeof wc);
    for (int i = 0; i < 3; i++) wc[i].v = (uint32_t)i;
    NsGpuScan sc; memset(&sc, 0, sizeof sc);
    ns_gpu_scan(w, wc, 1, id, 0.1f, 12.0f, &sc);
    CHECK(sc.tris == 1 && sc.big == 1 && sc.cross == 1 && sc.big_cross == 1);
}

/* Efectos (ns_gpu_fx_build): un cuadrilátero opaco, un cartel y una cinta semitransparentes -> geometría del mundo VR orientada a la cabeza (la misma que dibuja ns_vs_fx). */
static void test_fx(void) {
    NsCamera cam; memset(&cam, 0, sizeof cam);
    cam.rot[0] = cam.rot[4] = cam.rot[8] = 1; cam.h = 320; cam.overscan_y = NS_OVERSCAN_Y;
    float to_vr[12] = {0.001f, 0, 0, 0, 0, 0.001f, 0, 0, 0, 0, 0.001f, 0};
    NsEffect fx[3]; memset(fx, 0, sizeof fx);
    fx[0].kind = NS_EK_QUAD; fx[0].nv = 4; fx[0].src_key = 0x80100000u; fx[0].idx = 1;
    for (int v = 0; v < 4; v++) { fx[0].p[v][0] = (float)(v & 1) * 100; fx[0].p[v][1] = (float)(v >> 1) * 100; fx[0].p[v][2] = 3000; }
    fx[1].kind = NS_EK_BILLBOARD; fx[1].nv = 4; fx[1].flags = NS_EF_SEMI | NS_EF_TEXTURED; fx[1].abr = 1; fx[1].src_key = 0x80100000u; fx[1].idx = 2;
    fx[1].p[0][2] = 3200; fx[1].ref_h = 320; fx[1].box[0] = fx[1].box[1] = -16; fx[1].box[2] = fx[1].box[3] = 16;
    fx[2].kind = NS_EK_QUAD; fx[2].nv = 2; fx[2].flags = NS_EF_LINE | NS_EF_SEMI; fx[2].abr = 2; fx[2].src_key = 0x80100000u; fx[2].idx = 3; fx[2].width_px = 3;
    fx[2].p[0][0] = -500; fx[2].p[0][2] = 2500; fx[2].p[1][0] = 500; fx[2].p[1][2] = 2500;
    NsGpuFxParams P; memset(&P, 0, sizeof P);
    P.to_vr = to_vr; P.cam = &cam; P.head_right[0] = 1; P.head_up[1] = 1; P.n_eyes = 2; P.eyes[0][0] = -0.03f; P.eyes[1][0] = 0.03f;
    NsGpuFxVert vb[64]; NsGpuFxRun runs[4]; uint64_t keys[4]; NsGpuFxOut o;
    CHECK(ns_gpu_fx_build(fx, 3, &P, vb, 64, runs, 4, keys, 4, &o) == 0);
    CHECK(o.n_keys == 3 && o.n_billboard == 1 && o.n_ribbon == 1 && o.n_poly == 1);
    CHECK(o.n_opaque == 6 + 6 && o.n_semi == 6 + 6);   /* cuadrilátero + copia opaca del cartel texturizado; cartel + cinta */
    CHECK(o.n_runs == 2 && runs[0].abr == 1 && runs[1].abr == 2);   /* de atrás adelante: el cartel (3,2 m) y después la cinta (2,5 m) */
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    char p[1024];
    snprintf(p, sizeof p, "%s/scene_test.nsy", dir);
    CHECK(ns_synth_write(p) == 0);
    CHECK(ns_synth_load(p) == 0);
    test_emulation();
    test_quad_diagonal();
    test_sort();
    test_split();
    test_fx();
    printf(fails ? "FALLOS: %d\n" : "OK: todas las pruebas pasan\n", fails);
    return fails ? 1 : 0;
}
