#include "ns_bridge.h"
#include "ns_pub.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static NsScene g_sc[3];
static const NsMeshStore *g_store[3];
static NsPub g_pub;
static atomic_uint g_seq;
static atomic_uint g_pub_seq;
static int g_ready;

/* El almacén real lo implementa el extractor; estos enlaces débiles permiten enlazar la app antes de que exista (sin mallas no se dibuja nada). */
__attribute__((weak)) const NsMesh *ns_store_get(const NsMeshStore *st, uint64_t id) { (void)st; (void)id; return NULL; }
__attribute__((weak)) const NsBaked *ns_store_get_baked(const NsMeshStore *st, uint64_t mesh_id, uint32_t ver) { (void)st; (void)mesh_id; (void)ver; return NULL; }
__attribute__((weak)) uint32_t ns_store_epoch(const NsMeshStore *st) { (void)st; return 0; }
__attribute__((weak)) void ns_store_ack(NsMeshStore *st, uint32_t epoch) { (void)st; (void)epoch; }

static void slot_free(NsScene *s) {
    free(s->inst); free(s->piece); free(s->lights); free(s->effects);
    memset(s, 0, sizeof *s);
}

void mv_native_shutdown(void) {
    g_ready = 0;
    for (int i = 0; i < 3; i++) { slot_free(&g_sc[i]); g_store[i] = NULL; }
    memset(&g_pub, 0, sizeof g_pub);
    atomic_store(&g_seq, 0u);
    atomic_store(&g_pub_seq, 0u);
}

int mv_native_init(uint32_t ci, uint32_t cp, uint32_t cl, uint32_t ce) {
    mv_native_shutdown();
    if (!ci) ci = NS_MIN_INST;
    if (!cp) cp = NS_MIN_PIECE;
    if (!cl) cl = NS_MIN_LIGHT;
    if (!ce) ce = NS_MIN_EFFECT;
    for (int i = 0; i < 3; i++) {
        NsScene *s = &g_sc[i];
        s->inst = (NsInstance *)calloc(ci, sizeof(NsInstance));
        s->piece = (NsPiece *)calloc(cp, sizeof(NsPiece));
        s->lights = (NsLight *)calloc(cl, sizeof(NsLight));
        s->effects = (NsEffect *)calloc(ce, sizeof(NsEffect));
        if (!s->inst || !s->piece || !s->lights || !s->effects) { mv_native_shutdown(); return -1; }
        s->cap_inst = ci; s->cap_piece = cp; s->cap_light = cl; s->cap_effect = ce;
        s->size = (uint32_t)sizeof(NsScene); s->version = NS_VERSION;
    }
    ns_pub_init(&g_pub, &g_sc[0], &g_sc[1], &g_sc[2]);
    g_ready = 1;
    return 0;
}

NsScene *mv_native_begin(void) {
    if (!g_ready) return NULL;
    NsScene *s = g_pub.slot[g_pub.back];
    NsInstance *a = s->inst; NsPiece *b = s->piece; NsLight *c = s->lights; NsEffect *d = s->effects;
    uint32_t ci = s->cap_inst, cp = s->cap_piece, cl = s->cap_light, ce = s->cap_effect, gen = s->slot_gen;
    memset(s, 0, sizeof *s);
    s->inst = a; s->piece = b; s->lights = c; s->effects = d;
    s->cap_inst = ci; s->cap_piece = cp; s->cap_light = cl; s->cap_effect = ce;
    s->size = (uint32_t)sizeof(NsScene); s->version = NS_VERSION; s->slot_gen = gen;
    return ns_pub_begin(&g_pub);   /* sube slot_gen */
}

void mv_native_publish(NsScene *sc, const NsMeshStore *store) {
    if (!g_ready || !sc || sc != g_pub.slot[g_pub.back]) return;
    int idx = g_pub.back;
    sc->seq = atomic_fetch_add(&g_seq, 1u) + 1u;
    g_store[idx] = store;
    ns_pub_publish(&g_pub);                      /* release: slot, store y mallas visibles */
    atomic_store(&g_pub_seq, sc->seq);
}

const NsScene *mv_native_acquire(const NsMeshStore **store) {
    if (!g_ready) return NULL;
    NsScene *s = ns_pub_acquire(&g_pub);
    if (!s) return NULL;
    if (s->size != (uint32_t)sizeof(NsScene) || s->version != NS_VERSION) return NULL;
    if (store) { int idx = (int)(s - g_sc); *store = g_store[idx]; }
    return s;
}

void mv_native_release(const NsScene *sc) { (void)sc; }

void mv_native_ack(const NsMeshStore *store, uint32_t epoch) { if (store) ns_store_ack((NsMeshStore *)(uintptr_t)store, epoch); }

uint32_t mv_native_seq(void) { return atomic_load(&g_pub_seq); }
