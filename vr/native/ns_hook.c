/* ns_hook: ver ns_hook.h. Todo en el hilo de emulación (las estadísticas se leen desde otros hilos sin garantías de coherencia entre campos). */
#include "ns_hook.h"
#include <string.h>

static struct {
    NsBridge *br;
    NsExtractFn ex; void *ex_user; const NsMeshStore *store;
    NsCtxFn ctx; void *ctx_user;
    uint32_t vbl;
    int pending;
    NsPlayerMap map;
    uint32_t refresh_parts;
    NsHookStats st;
} H;

void ns_hook_init(NsBridge *b) { H.br = b; H.pending = 0; memset(&H.map, 0, sizeof H.map); }
void ns_hook_reset(void) { H.pending = 0; memset(&H.map, 0, sizeof H.map); memset(&H.st, 0, sizeof H.st); }
void ns_hook_set_extractor(NsExtractFn fn, void *user, const NsMeshStore *store) { H.ex = fn; H.ex_user = user; H.store = store; }
int ns_hook_has_extractor(void) { return H.ex != NULL; }
void ns_hook_set_context(NsCtxFn fn, void *user) { H.ctx = fn; H.ctx_user = user; }
void ns_hook_set_frame(uint32_t v) { H.vbl = v; }
void ns_hook_stats(NsHookStats *s) { if (s) *s = H.st; }

void ns_capture(const uint8_t *ram, const uint8_t *scratch) {
    if (!H.br || !ram) return;
    uint64_t t = ns_now_ns();
    if (H.pending) H.st.replaced++;
    NsScene *s = ns_bridge_begin(H.br);   /* idempotente: si había una pendiente se reutiliza la misma ranura limpia */
    if (!s) return;
    H.pending = 0;
    H.st.captures++;
    if (H.ex) {
        if (H.ex(ram, scratch, s, H.ex_user) < 0) { H.st.extract_fail++; return; }
    }
    s->t_ns = t;
    if (!s->game_frame) s->game_frame = H.st.captures;
    if (!s->vblank) s->vblank = H.vbl;
    NsCtxOut o;
    memset(&o, 0, sizeof o);
    if (H.ctx) H.ctx(s, &o, H.ctx_user);
    memset(&H.map, 0, sizeof H.map);
    H.refresh_parts = 0;
    if (o.player_key && ns_player_bind(&H.map, s, ram, o.player_key) > 0) {
        H.st.player_bound++;
        if (o.hide_head) ns_player_hide_part(&H.map, s, NS_PART_HEAD);
        H.refresh_parts = o.refresh_parts;
    }
    H.pending = 1;
}

void ns_hook_post_ik(const uint8_t *ram) {
    if (!H.pending || !H.br || !H.map.bound || !H.refresh_parts) return;
    NsScene *s = ns_bridge_current(H.br);
    if (s && ns_player_refresh(&H.map, s, ram, H.refresh_parts) > 0) H.st.ik_refresh++;
}

void ns_hook_tick(const uint8_t *ram) {
    (void)ram;
    if (!H.pending || !H.br) return;
    NsScene *s = ns_bridge_current(H.br);
    if (!s) { H.pending = 0; return; }
    H.st.last_valid = s->valid;
    H.st.last_pub_ns = ns_now_ns();
    ns_bridge_publish(H.br, H.store);
    H.st.published++;
    H.pending = 0;
    memset(&H.map, 0, sizeof H.map);
}

int ns_hook_native_ready(uint64_t now_ns, uint64_t max_age_ns) {
    const uint32_t need = NS_SV_GAME | NS_SV_LISTS_OK | NS_SV_TOVR_OK;
    return H.st.published && (H.st.last_valid & need) == need && now_ns >= H.st.last_pub_ns && now_ns - H.st.last_pub_ns <= max_age_ns;
}
