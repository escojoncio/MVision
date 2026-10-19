/* Pruebas de escritorio (sin GPU, sin datos del juego) del empaquetado del renderer nativo:
 *  1. emulación en C de ns_vertex (NativeShaders.metal) sobre los búferes de ns_gpu.c, comparada triángulo a triángulo con el adaptador independiente de la
 *     referencia (posiciones VR, colores de vértice con luz/niebla/color precalculado/welded);
 *  2. orden de los semitransparentes frente a una ordenación de fuerza bruta;
 *  3. puente de triple búfer (una hebra y con productor/consumidor concurrentes). */
#include "native_synth.h"
#include "ns_bridge.h"
#include "ns_gpu.h"
#include <math.h>
#include <stdatomic.h>
#include <pthread.h>
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
    const NsMeshStore *store = NULL;
    const NsScene *sc = mv_native_acquire(&store);
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
    mv_native_release(sc);
}

/* ---------------- 2. orden de los semitransparentes ---------------- */
typedef struct { float d; uint32_t seq, piece, tri; } Bf;
static int bf_cmp(const void *a, const void *b) {
    const Bf *x = (const Bf *)a, *y = (const Bf *)b;
    if (x->d != y->d) return x->d > y->d ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
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
    size_t m = ns_gpu_sort_semi(src, NP, out, ab, NP * NT);
    CHECK(m == n);
    int ok = 1;
    for (size_t i = 0; i < n && i < m; i++) if (out[i].piece != bf[i].piece || out[i].tri != bf[i].tri) ok = 0;
    CHECK(ok);
    CHECK(ns_gpu_sort_semi(src, NP, out, ab, 17) == 17);   /* capacidad */
    CHECK(ns_gpu_sort_semi(src, 0, out, ab, 10) == 0);
    free(bf); free(out); free(ab);
}

/* ---------------- 3. puente ---------------- */
static atomic_int stop;
static void *producer(void *arg) {
    (void)arg;
    uint32_t n = 0;
    while (!atomic_load(&stop)) {
        NsScene *s = mv_native_begin();
        if (!s) continue;
        n++;
        s->game_frame = n;
        s->n_inst = 8;
        for (uint32_t i = 0; i < 8; i++) { s->inst[i].key = n * 7u + i; s->inst[i].n_pieces = (uint16_t)(n & 0xFFFF); }
        mv_native_publish(s, (const NsMeshStore *)(uintptr_t)n);
        if ((n & 63) == 0) usleep(50);
    }
    return NULL;
}
static void test_bridge(void) {
    CHECK(mv_native_init(16, 16, 4, 4) == 0);
    CHECK(mv_native_seq() == 0);
    const NsMeshStore *st = NULL;
    CHECK(mv_native_acquire(&st) == NULL);
    NsScene *a = mv_native_begin();
    CHECK(a && a->cap_inst == 16 && a->n_inst == 0 && a->version == NS_VERSION);
    mv_native_publish(a, (const NsMeshStore *)(uintptr_t)11);
    CHECK(mv_native_seq() == 1);
    const NsScene *r = mv_native_acquire(&st);
    CHECK(r == a && (uintptr_t)st == 11 && r->seq == 1);
    /* la escena tomada no se reutiliza hasta la siguiente acquire: el productor nunca recibe esa ranura */
    for (int i = 0; i < 10; i++) {
        NsScene *b = mv_native_begin();
        CHECK(b != NULL && b != r);
        mv_native_publish(b, NULL);
    }
    CHECK(r->seq == 1);                       /* intacta */
    mv_native_release(r);
    CHECK(r->size == sizeof(NsScene) && r->slot_gen >= 1);
    CHECK(mv_native_init(16, 16, 4, 4) == 0);   /* reinit */
    /* concurrente */
    pthread_t th; atomic_store(&stop, 0);
    pthread_create(&th, NULL, producer, NULL);
    uint32_t last = 0; long reads = 0, torn = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 > 400) break;
        const NsScene *s = mv_native_acquire(&st);
        if (!s) continue;
        uint32_t f = s->game_frame;
        int ok = s->n_inst == 8 && s->game_frame == s->seq && s->inst[7].key == f * 7u + 7u && s->inst[0].n_pieces == (uint16_t)(f & 0xFFFF) && (uintptr_t)st == f;
        if (!ok) torn++;
        if (f < last) torn++;
        last = f; reads++;
        mv_native_release(s);
    }
    atomic_store(&stop, 1);
    pthread_join(th, NULL);
    printf("puente: %ld lecturas, última escena %u, corrompidas %ld\n", reads, last, torn);
    CHECK(torn == 0 && reads > 1000);
    mv_native_shutdown();
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    char p[1024];
    snprintf(p, sizeof p, "%s/scene_test.nsy", dir);
    CHECK(ns_synth_write(p) == 0);
    CHECK(ns_synth_load(p) == 0);
    test_emulation();
    test_sort();
    test_bridge();
    printf(fails ? "FALLOS: %d\n" : "OK: todas las pruebas pasan\n", fails);
    return fails ? 1 : 0;
}
