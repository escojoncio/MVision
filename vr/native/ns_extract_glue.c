/* Registro del extractor real en el enganche (ns_hook): vrhost llama a ns_extract_register() en mv_vrhost_install (tiene una definición débil vacía; ésta la sustituye).
 * Crea el almacén de mallas, registra la extracción de la RAM y el reinicio del almacén al cambiar de ejecutable. Hilo: el de instalación; después el enganche
 * llama a ns_extract_ex en el hilo de emulación (único productor). */
#include "ns_extract.h"
#include "ns_hook.h"
#include <stdatomic.h>

static NsMeshStore *g_store;

/* Contadores acumulados de ns_extract_ex (para el registro de diagnóstico, 1 vez por segundo). Los escribe el hilo de emulación y los lee el de render o el mismo
 * hilo: atómicos relajados. last_* = la última escena. */
static struct {
    _Atomic uint32_t calls, mesh_new, memo_stale, zone_changes, epoch_opened, store_full, drop_group, drop_hidden, drop_other, queue_sum;
    _Atomic uint32_t last_queue, last_inst, last_static, last_pieces, last_hidden_pieces, last_drop_group, last_drop_hidden, last_drop_other;
} G;

static void bump(_Atomic uint32_t *c, uint32_t v) { atomic_fetch_add_explicit(c, v, memory_order_relaxed); }

static int glue_extract(const uint8_t *ram, const uint8_t *scratch, NsScene *out, void *user) {
    NsExtractStats st;
    int rc = ns_extract_ex(out, (NsMeshStore *)user, ram, NS_RAM_SIZE, scratch, scratch ? 1024u : 0u, NULL, &st);
    uint32_t other = st.drop_null + st.drop_badptr + st.drop_dup + st.drop_badobj + st.drop_bad_model;
    bump(&G.calls, 1); bump(&G.mesh_new, st.mesh_new); bump(&G.memo_stale, st.memo_stale); bump(&G.zone_changes, st.zone_changes);
    bump(&G.epoch_opened, st.epoch_opened); bump(&G.store_full, st.store_full); bump(&G.drop_group, st.drop_group);
    bump(&G.drop_hidden, st.drop_hidden_flag); bump(&G.drop_other, other); bump(&G.queue_sum, st.queue_n);
    atomic_store_explicit(&G.last_queue, st.queue_n, memory_order_relaxed);
    atomic_store_explicit(&G.last_inst, st.n_inst, memory_order_relaxed);
    atomic_store_explicit(&G.last_static, st.n_static, memory_order_relaxed);
    atomic_store_explicit(&G.last_pieces, st.n_pieces, memory_order_relaxed);
    atomic_store_explicit(&G.last_hidden_pieces, st.n_hidden_pieces, memory_order_relaxed);
    atomic_store_explicit(&G.last_drop_group, st.drop_group, memory_order_relaxed);
    atomic_store_explicit(&G.last_drop_hidden, st.drop_hidden_flag, memory_order_relaxed);
    atomic_store_explicit(&G.last_drop_other, other, memory_order_relaxed);
    return rc;
}
static void glue_reset(NsMeshStore *st, void *user) { (void)user; ns_store_reset(st); }

void ns_extract_register(void) {
    if (!g_store) g_store = ns_store_create();
    if (!g_store) return;
    ns_hook_set_extractor(glue_extract, g_store, g_store);
    ns_hook_set_store_reset(glue_reset, NULL);
}

void ns_extract_glue_stats(NsExtractGlueStats *o) {
    if (!o) return;
    o->calls = atomic_load_explicit(&G.calls, memory_order_relaxed); o->mesh_new = atomic_load_explicit(&G.mesh_new, memory_order_relaxed);
    o->memo_stale = atomic_load_explicit(&G.memo_stale, memory_order_relaxed); o->zone_changes = atomic_load_explicit(&G.zone_changes, memory_order_relaxed);
    o->epoch_opened = atomic_load_explicit(&G.epoch_opened, memory_order_relaxed); o->store_full = atomic_load_explicit(&G.store_full, memory_order_relaxed);
    o->drop_group = atomic_load_explicit(&G.drop_group, memory_order_relaxed); o->drop_hidden = atomic_load_explicit(&G.drop_hidden, memory_order_relaxed);
    o->drop_other = atomic_load_explicit(&G.drop_other, memory_order_relaxed); o->queue_sum = atomic_load_explicit(&G.queue_sum, memory_order_relaxed);
    o->last_queue = atomic_load_explicit(&G.last_queue, memory_order_relaxed); o->last_inst = atomic_load_explicit(&G.last_inst, memory_order_relaxed);
    o->last_static = atomic_load_explicit(&G.last_static, memory_order_relaxed); o->last_pieces = atomic_load_explicit(&G.last_pieces, memory_order_relaxed);
    o->last_hidden_pieces = atomic_load_explicit(&G.last_hidden_pieces, memory_order_relaxed);
    o->last_drop_group = atomic_load_explicit(&G.last_drop_group, memory_order_relaxed); o->last_drop_hidden = atomic_load_explicit(&G.last_drop_hidden, memory_order_relaxed);
    o->last_drop_other = atomic_load_explicit(&G.last_drop_other, memory_order_relaxed);
}
