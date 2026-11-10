/* ns_hook: ver ns_hook.h. Se ejecuta en el hilo de emulación; los contadores y el estado que lee la interfaz (otro hilo) son atómicos. */
#include "ns_hook.h"
#include <stdatomic.h>
#include <string.h>

static struct {
    NsBridge *br;
    _Atomic(NsExtractFn) ex; void *ex_user; NsMeshStore *store;
    NsCtxFn ctx; void *ctx_user;
    NsGateFn gate; void *gate_user;
    NsStoreResetFn sreset; void *sreset_user;
    NsSceneTapFn tap; void *tap_user;
    NsFinalizeFn fin; void *fin_user;
    uint32_t vbl;
    int pending;
    NsPlayerMap map;
    uint32_t refresh_parts;
    int ik_done, hide_head;
    int wrap_mark;   /* ns_capture (envoltura de la etapa «pantalla») corrió desde el último ns_capture_backup_due */
    NsIkIdleFn idle; void *idle_user;
    struct {
        _Atomic uint32_t captures, published, replaced, extract_fail, ik_refresh, player_bound, ik_expected, ik_missed, skipped_gate, last_valid, ik_idle, cap_wrap, cap_backup, cap_dyn;
        _Atomic uint64_t last_pub_ns;
    } st;
} H;

static void clear_stats(void) {
    atomic_store(&H.st.captures, 0); atomic_store(&H.st.published, 0); atomic_store(&H.st.replaced, 0); atomic_store(&H.st.extract_fail, 0);
    atomic_store(&H.st.ik_refresh, 0); atomic_store(&H.st.player_bound, 0); atomic_store(&H.st.ik_expected, 0); atomic_store(&H.st.ik_missed, 0);
    atomic_store(&H.st.ik_idle, 0); atomic_store(&H.st.skipped_gate, 0); atomic_store(&H.st.last_valid, 0); atomic_store(&H.st.last_pub_ns, 0);
    atomic_store(&H.st.cap_wrap, 0); atomic_store(&H.st.cap_backup, 0); atomic_store(&H.st.cap_dyn, 0);
}
static void inc(_Atomic uint32_t *c) { atomic_fetch_add_explicit(c, 1, memory_order_relaxed); }

void ns_hook_init(NsBridge *b) { H.br = b; H.pending = 0; H.wrap_mark = 0; memset(&H.map, 0, sizeof H.map); clear_stats(); }

void ns_hook_reset(void) {
    H.pending = 0; H.wrap_mark = 0;
    memset(&H.map, 0, sizeof H.map);
    clear_stats();
    if (H.sreset && H.store) H.sreset(H.store, H.sreset_user);
    if (H.br) {   /* escena vacía: el consumidor deja de usar la del ejecutable anterior (valid = 0 => GPU) y la interpolación se corta (game_frame retrocede) */
        NsScene *s = ns_bridge_begin(H.br);
        if (s) { s->t_ns = ns_now_ns(); ns_bridge_publish(H.br, H.store); }
    }
}
void ns_hook_set_extractor(NsExtractFn fn, void *user, NsMeshStore *store) { H.ex_user = user; H.store = store; atomic_store(&H.ex, fn); }
int ns_hook_has_extractor(void) { return atomic_load(&H.ex) != NULL; }
void ns_hook_set_context(NsCtxFn fn, void *user) { H.ctx = fn; H.ctx_user = user; }
void ns_hook_set_gate(NsGateFn fn, void *user) { H.gate_user = user; H.gate = fn; }
void ns_hook_set_store_reset(NsStoreResetFn fn, void *user) { H.sreset_user = user; H.sreset = fn; }
void ns_hook_set_scene_tap(NsSceneTapFn fn, void *user) { H.tap_user = user; H.tap = fn; }
void ns_hook_set_finalize(NsFinalizeFn fn, void *user) { H.fin_user = user; H.fin = fn; }
void ns_hook_set_frame(uint32_t v) { H.vbl = v; }
void ns_hook_set_ik_idle(NsIkIdleFn fn, void *user) { H.idle_user = user; H.idle = fn; }
void ns_hook_stats(NsHookStats *s) {
    if (!s) return;
    s->captures = atomic_load(&H.st.captures); s->published = atomic_load(&H.st.published); s->replaced = atomic_load(&H.st.replaced);
    s->extract_fail = atomic_load(&H.st.extract_fail); s->ik_refresh = atomic_load(&H.st.ik_refresh); s->player_bound = atomic_load(&H.st.player_bound);
    s->ik_expected = atomic_load(&H.st.ik_expected); s->ik_missed = atomic_load(&H.st.ik_missed); s->skipped_gate = atomic_load(&H.st.skipped_gate);
    s->last_valid = atomic_load(&H.st.last_valid); s->last_pub_ns = atomic_load(&H.st.last_pub_ns);
    s->ik_idle = atomic_load(&H.st.ik_idle);
    s->cap_wrap = atomic_load(&H.st.cap_wrap); s->cap_backup = atomic_load(&H.st.cap_backup); s->cap_dyn = atomic_load(&H.st.cap_dyn);
}

static void capture(const uint8_t *ram, const uint8_t *scratch, int kind) {   /* kind: 0 envoltura estándar, 1 respaldo, 2 envoltura dinámica (etapa en overlay) */
    if (!H.br || !ram) return;
    if (H.gate && !H.gate(H.gate_user)) {      /* modo gpu / renderer no listo: ni se captura ni se extrae */
        inc(&H.st.skipped_gate);
        H.pending = 0;
        atomic_store(&H.st.last_valid, 0);
        return;
    }
    uint64_t t = ns_now_ns();
    if (H.pending) inc(&H.st.replaced);
    NsScene *s = ns_bridge_begin(H.br);   /* idempotente: si había una pendiente se reutiliza la misma ranura limpia */
    if (!s) return;
    H.pending = 0;
    inc(&H.st.captures);
    inc(kind == 1 ? &H.st.cap_backup : kind == 2 ? &H.st.cap_dyn : &H.st.cap_wrap);
    NsExtractFn ex = atomic_load(&H.ex);
    if (ex) {
        if (ex(ram, scratch, s, H.ex_user) < 0) { inc(&H.st.extract_fail); return; }
    }
    s->t_ns = t;
    if (!s->game_frame) s->game_frame = atomic_load(&H.st.captures);
    if (!s->vblank) s->vblank = H.vbl;
    NsCtxOut o;
    memset(&o, 0, sizeof o);
    if (H.ctx) H.ctx(s, &o, H.ctx_user);
    memset(&H.map, 0, sizeof H.map);
    H.refresh_parts = 0; H.ik_done = 0; H.hide_head = o.hide_head;
    s->valid &= ~(NS_SV_PLAYER_OK | NS_SV_PLAYER_PRESENT);   /* lo deciden este enganche, nunca el extractor */
    int present = o.player_key ? ns_player_find(s, o.player_key) : -1;
    if (present >= 0) s->valid |= NS_SV_PLAYER_PRESENT;
    int pb = o.player_key ? ns_player_bind(&H.map, s, ram, o.player_key) : 0;
    int head_ok = 0;
    if (pb > 0) {
        inc(&H.st.player_bound);
        head_ok = 1;
        if (o.hide_head) head_ok = ns_player_hide_part(&H.map, s, NS_PART_HEAD) > 0;
        for (int k = 0; k < NS_PLAYER_PARTS; k++) if (o.hide_parts & (1u << k)) ns_player_hide_part(&H.map, s, k);
        H.refresh_parts = o.refresh_parts;
        int arms_ok = 1;
        if (o.refresh_parts) {   /* debe haber al menos una pieza de brazo enlazada para que la IK tenga efecto */
            arms_ok = 0;
            for (int k = 0; k < NS_PLAYER_PARTS; k++) if ((o.refresh_parts & (1u << k)) && H.map.part[k].n) arms_ok = 1;
            inc(&H.st.ik_expected);
        }
        /* Apto: cabeza oculta; si no hay IK que esperar, ya; si la hay, se marca en ns_hook_post_ik cuando llegue. */
        if (head_ok && arms_ok && !o.refresh_parts) s->valid |= NS_SV_PLAYER_OK;
        else if (!(head_ok && arms_ok)) { H.refresh_parts = 0; if (o.refresh_parts) inc(&H.st.ik_missed); }
    }
    /* El jugador está en la escena (el motor lo oculta en ciertas cámaras, pero se extrae) y no se pudo enlazar o ocultar su cabeza: ninguna de sus piezas se dibuja (nunca el cuerpo entero con la cabeza dentro de la vista VR) */
    if (present >= 0 && o.hide_head && !head_ok) {
        const NsInstance *pi = &s->inst[present];
        if ((uint64_t)pi->first_piece + pi->n_pieces <= s->n_piece)
            for (uint32_t k = 0; k < pi->n_pieces; k++) s->piece[pi->first_piece + k].flags |= NS_PF_HIDDEN;
    }
    H.pending = 1;
}

void ns_capture(const uint8_t *ram, const uint8_t *scratch) {
    H.wrap_mark = 1;
    capture(ram, scratch, 0);
}
void ns_capture_dyn(const uint8_t *ram, const uint8_t *scratch) {
    H.wrap_mark = 1;
    capture(ram, scratch, 2);
}
void ns_capture_backup_begin(void) { H.wrap_mark = 0; }
int ns_capture_backup_due(void) { int due = !H.wrap_mark; H.wrap_mark = 0; return due; }
void ns_capture_backup(const uint8_t *ram, const uint8_t *scratch) { capture(ram, scratch, 1); }

void ns_hook_post_ik(const uint8_t *ram) {
    if (!H.pending || !H.br || !H.map.bound || !H.refresh_parts || H.ik_done) return;
    NsScene *s = ns_bridge_current(H.br);
    if (s && ns_player_refresh(&H.map, s, ram, H.refresh_parts) > 0) {
        H.ik_done = 1;
        inc(&H.st.ik_refresh);
        s->valid |= NS_SV_PLAYER_OK;
    }
}

void ns_hook_tick(const uint8_t *ram) {
    (void)ram;
    if (!H.pending || !H.br) return;
    NsScene *s = ns_bridge_current(H.br);
    if (!s) { H.pending = 0; return; }
    if (H.refresh_parts && !H.ik_done) {
        if (H.idle && H.idle(H.idle_user)) { inc(&H.st.ik_idle); s->valid |= NS_SV_PLAYER_OK; }   /* capa VR sin mandos con seguimiento: brazos de la animación del juego, escena apta */
        else inc(&H.st.ik_missed);                                                                  /* se esperaba la pose IK y no llegó: la escena sale sin PLAYER_OK */
    }
    if (H.fin && H.map.bound && (s->valid & NS_SV_PLAYER_OK)) H.fin(s, H.store, &H.map, H.fin_user);
    atomic_store(&H.st.last_valid, s->valid);
    if (H.tap) H.tap(s, H.store, H.tap_user);   /* antes de publicar: el observador ve la escena que verá el consumidor */
    ns_bridge_publish(H.br, H.store);
    atomic_store(&H.st.last_pub_ns, ns_now_ns());
    inc(&H.st.published);
    H.pending = 0;
    memset(&H.map, 0, sizeof H.map);
}

int ns_hook_native_ready(uint64_t now_ns, uint64_t max_age_ns) {
    uint64_t t = atomic_load(&H.st.last_pub_ns);
    return atomic_load(&H.st.published) && ns_scene_native_ok(atomic_load(&H.st.last_valid)) && now_ns >= t && now_ns - t <= max_age_ns;
}
