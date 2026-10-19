/* ns_bridge: ver ns_bridge.h. */
#include "ns_bridge.h"
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NONE 0xFu

struct NsBridge {
    _Atomic uint32_t state;              /* buzón | actual << 4 | anterior << 8 (4 bits cada uno, 0xF = ninguna) */
    NsBridgeCaps caps;
    NsScene slot[NS_BRIDGE_SLOTS];
    NsMeshStore *store[NS_BRIDGE_SLOTS];
    /* solo productor */
    int wr;                              /* ranura en escritura o -1 */
    int fresh_wr;                        /* la ranura de escritura se acaba de elegir (primer begin) */
    uint32_t seq;
    /* contadores (cualquier hilo) */
    _Atomic uint32_t published, dropped, acquired, polls;
    _Atomic uint64_t period_ns;
    /* solo consumidor */
    float lead_factor, max_extrap;
    uint32_t last_cur_seq;
    /* emparejamiento de instancias (consumidor) */
    int32_t *map;
    int32_t *tab_idx;
    uint32_t *tab_key;
    uint32_t tab_n, tab_shift;           /* tab_n = 1 << (32 - tab_shift) */
    uint32_t map_cur_seq, map_prev_seq;
    int map_valid;
};

uint64_t ns_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static uint32_t pick(uint32_t v, uint32_t d) { return v ? v : d; }

NsBridge *ns_bridge_create(const NsBridgeCaps *caps) {
    NsBridge *b = (NsBridge *)calloc(1, sizeof *b);
    if (!b) return NULL;
    b->caps.cap_inst = pick(caps ? caps->cap_inst : 0, 512);
    b->caps.cap_piece = pick(caps ? caps->cap_piece : 0, 4096);
    b->caps.cap_light = pick(caps ? caps->cap_light : 0, 256);
    b->caps.cap_effect = pick(caps ? caps->cap_effect : 0, 1024);
    int ok = 1;
    for (int i = 0; i < NS_BRIDGE_SLOTS && ok; i++) {
        NsScene *s = &b->slot[i];
        s->inst = (NsInstance *)calloc(b->caps.cap_inst, sizeof *s->inst);
        s->piece = (NsPiece *)calloc(b->caps.cap_piece, sizeof *s->piece);
        s->lights = (NsLight *)calloc(b->caps.cap_light, sizeof *s->lights);
        s->effects = (NsEffect *)calloc(b->caps.cap_effect, sizeof *s->effects);
        ok = s->inst && s->piece && s->lights && s->effects;
    }
    uint32_t tn = 16;
    while (tn < 2u * b->caps.cap_inst) tn <<= 1;
    b->tab_n = tn;
    b->tab_shift = 32;
    for (uint32_t t = tn; t > 1; t >>= 1) b->tab_shift--;
    b->map = (int32_t *)calloc(b->caps.cap_inst, sizeof *b->map);
    b->tab_idx = (int32_t *)calloc(tn, sizeof *b->tab_idx);
    b->tab_key = (uint32_t *)calloc(tn, sizeof *b->tab_key);
    if (!ok || !b->map || !b->tab_idx || !b->tab_key) { ns_bridge_destroy(b); return NULL; }
    atomic_init(&b->state, NONE | (NONE << 4) | (NONE << 8));
    atomic_init(&b->published, 0); atomic_init(&b->dropped, 0); atomic_init(&b->acquired, 0); atomic_init(&b->polls, 0);
    atomic_init(&b->period_ns, 40000000ull);
    b->wr = -1;
    b->lead_factor = 1.0f;
    b->max_extrap = 0.25f;
    return b;
}

void ns_bridge_destroy(NsBridge *b) {
    if (!b) return;
    for (int i = 0; i < NS_BRIDGE_SLOTS; i++) {
        free(b->slot[i].inst); free(b->slot[i].piece); free(b->slot[i].lights); free(b->slot[i].effects);
    }
    free(b->map); free(b->tab_idx); free(b->tab_key);
    free(b);
}

void ns_bridge_set_interp(NsBridge *b, float lead_factor, float max_extrap) {
    if (!b) return;
    if (lead_factor >= 0.0f && lead_factor <= 4.0f) b->lead_factor = lead_factor;
    if (max_extrap >= 0.0f && max_extrap <= 1.0f) b->max_extrap = max_extrap;
}

void ns_bridge_stats(const NsBridge *b, NsBridgeStats *s) {
    if (!b || !s) return;
    s->published = atomic_load_explicit(&b->published, memory_order_relaxed);
    s->dropped = atomic_load_explicit(&b->dropped, memory_order_relaxed);
    s->acquired = atomic_load_explicit(&b->acquired, memory_order_relaxed);
    s->polls = atomic_load_explicit(&b->polls, memory_order_relaxed);
    s->period_ms = (double)atomic_load_explicit(&b->period_ns, memory_order_relaxed) * 1e-6;
}

/* ------------------------------------------------------------------------------------------------ productor */
NsScene *ns_bridge_begin(NsBridge *b) {
    if (!b) return NULL;
    if (b->wr < 0) {
        uint32_t s = atomic_load_explicit(&b->state, memory_order_acquire);
        uint32_t used[3] = { s & 0xF, (s >> 4) & 0xF, (s >> 8) & 0xF };
        /* Con 4 ranuras y como mucho 3 ocupadas siempre queda una libre. Una ranura que no está en el estado leído no puede pasar a estarlo
         * por obra del consumidor (solo mueve buzón -> actual -> anterior -> libre), así que el estado puede estar caducado sin riesgo. */
        for (int i = 0; i < NS_BRIDGE_SLOTS; i++) {
            if ((uint32_t)i != used[0] && (uint32_t)i != used[1] && (uint32_t)i != used[2]) { b->wr = i; break; }
        }
        if (b->wr < 0) return NULL;   /* imposible por construcción */
        b->fresh_wr = 1;
    }
    NsScene *s = &b->slot[b->wr];
    NsInstance *ai = s->inst; NsPiece *ap = s->piece; NsLight *al = s->lights; NsEffect *ae = s->effects;
    uint32_t gen = s->slot_gen + (b->fresh_wr ? 1u : 0u);   /* contrato v2: generación del slot, sube en cada reescritura (begin repetido no cuenta) */
    b->fresh_wr = 0;
    memset(s, 0, sizeof *s);
    s->size = (uint32_t)sizeof *s;
    s->slot_gen = gen;
    s->version = NS_VERSION;
    s->units_per_m = 1000.0f;
    s->inst = ai; s->cap_inst = b->caps.cap_inst;
    s->piece = ap; s->cap_piece = b->caps.cap_piece;
    s->lights = al; s->cap_light = b->caps.cap_light;
    s->effects = ae; s->cap_effect = b->caps.cap_effect;
    return s;
}

NsScene *ns_bridge_current(NsBridge *b) { return b && b->wr >= 0 ? &b->slot[b->wr] : NULL; }
int ns_bridge_pending(const NsBridge *b) { return b && b->wr >= 0; }

void ns_bridge_publish(NsBridge *b, NsMeshStore *store) {
    if (!b || b->wr < 0) return;
    uint32_t w = (uint32_t)b->wr;
    NsScene *s = &b->slot[w];
    /* clamp defensivo de los contadores: el consumidor se fía de ellos */
    if (s->n_inst > s->cap_inst) s->n_inst = s->cap_inst;
    if (s->n_piece > s->cap_piece) s->n_piece = s->cap_piece;
    if (s->n_light > s->cap_light) s->n_light = s->cap_light;
    if (s->n_effect > s->cap_effect) s->n_effect = s->cap_effect;
    s->seq = ++b->seq;
    if (!s->t_ns) s->t_ns = ns_now_ns();
    b->store[w] = store;
    uint32_t old = atomic_load_explicit(&b->state, memory_order_relaxed), nw;
    int replaced;
    do {
        replaced = (old & 0xF) != NONE;
        nw = (old & ~0xFu) | w;   /* el buzón anterior (si había) queda libre: se sustituye */
    } while (!atomic_compare_exchange_weak_explicit(&b->state, &old, nw, memory_order_acq_rel, memory_order_relaxed));
    atomic_fetch_add_explicit(&b->published, 1, memory_order_relaxed);
    if (replaced) atomic_fetch_add_explicit(&b->dropped, 1, memory_order_relaxed);
    b->wr = -1;
}

/* ------------------------------------------------------------------------------------------------ consumidor */
int ns_scene_continuous(const NsScene *prev, const NsScene *cur) {
    if (!prev || !cur) return 0;
    if (prev->mesh_epoch != cur->mesh_epoch || prev->area_id != cur->area_id) return 0;
    if (!prev->valid || !cur->valid) return 0;               /* escena vacía (cambio de ejecutable / captura fallida): sin interpolación */
    if (cur->game_frame < prev->game_frame) return 0;        /* el contador de fotogramas de juego retrocede: otro ejecutable o reinicio */
    if (cur->t_ns <= prev->t_ns || cur->t_ns - prev->t_ns >= NS_DISCONT_NS) return 0;
    float dx = cur->cam.eye[0] - prev->cam.eye[0], dy = cur->cam.eye[1] - prev->cam.eye[1], dz = cur->cam.eye[2] - prev->cam.eye[2];
    if (dx * dx + dy * dy + dz * dz > 8000.0f * 8000.0f) return 0;
    if ((prev->valid & NS_SV_TOVR_OK) != (cur->valid & NS_SV_TOVR_OK)) return 0;
    if (cur->valid & NS_SV_TOVR_OK) {   /* el mapa mundo -> usuario también debe ser continuo (recentrado, cambio de escala): traslación en metros */
        float sc = 0.0f;
        for (int i = 0; i < 12; i++) if ((i & 3) != 3) sc = fmaxf(sc, fabsf(prev->to_vr[i]));
        for (int i = 0; i < 12; i++) {
            float d = fabsf(cur->to_vr[i] - prev->to_vr[i]);
            if (!(d <= ((i & 3) == 3 ? 2.0f : 0.25f * sc))) return 0;   /* giro de más de ~14 grados o escala distinta; NaN también descarta */
        }
    }
    return 1;
}

/* ------------------------------------------------------------------------------------------------ decisión nativo/GPU */
int ns_scene_native_ok(uint32_t valid) { return (valid & NS_NATIVE_NEED) == NS_NATIVE_NEED && !(valid & NS_NATIVE_BAD); }

int ns_native_decide(const NsView *v, const NsDecideIn *in, NsDecision *out) {
    NsDecision d = {0, 0, NS_WHY_OK};
    if (!in || !in->want_native) d.why = NS_WHY_MODE;
    else if (!in->renderer_ok) d.why = NS_WHY_RENDERER;
    else if (!v || !v->cur) d.why = NS_WHY_NOSCENE;
    else if (!in->scene_marked) d.why = NS_WHY_UNMARKED;
    else if (!in->meshes_ok) d.why = NS_WHY_MESHES;
    else if (!ns_scene_native_ok(v->cur->valid)) d.why = NS_WHY_INVALID;
    else if (in->now_ns > v->cur->t_ns && in->now_ns - v->cur->t_ns > in->max_age_ns) d.why = NS_WHY_STALE;   /* now < t_ns: la escena llegó tras muestrear el reloj, edad 0 */
    else {
        d.native = 1;
        d.skip = NS_SKIP_WORLD | ((v->cur->valid & NS_SV_FX) ? NS_SKIP_FX : 0u);   /* sin efectos nativos los batches kind 1 siguen por GPU */
    }
    if (out) *out = d;
    return d.native;
}
void ns_native_fail(NsDecision *d) { if (d) { d->native = 0; d->skip = 0; d->why = NS_WHY_FAILED; } }

int ns_bridge_acquire(NsBridge *b, uint64_t now_ns, NsView *out) {
    if (!b || !out) return 0;
    memset(out, 0, sizeof *out);
    out->alpha = 1.0f;
    atomic_fetch_add_explicit(&b->polls, 1, memory_order_relaxed);
    uint32_t s = atomic_load_explicit(&b->state, memory_order_acquire);
    for (;;) {
        uint32_t mb = s & 0xF;
        if (mb == NONE) break;
        uint32_t cur = (s >> 4) & 0xF;
        uint32_t nw = NONE | (mb << 4) | (cur << 8);   /* el buzón pasa a actual, el actual a anterior, el anterior queda libre */
        if (atomic_compare_exchange_weak_explicit(&b->state, &s, nw, memory_order_acq_rel, memory_order_acquire)) { s = nw; out->fresh = 1; break; }
    }
    uint32_t ci = (s >> 4) & 0xF, pi = (s >> 8) & 0xF;
    if (ci == NONE) return 0;
    const NsScene *cur = &b->slot[ci];
    const NsScene *prev = pi == NONE ? NULL : &b->slot[pi];
    out->cur = cur; out->prev = prev; out->store = b->store[ci];
    out->cur_gen = cur->slot_gen; out->prev_gen = prev ? prev->slot_gen : 0;
    if (out->fresh) atomic_fetch_add_explicit(&b->acquired, 1, memory_order_relaxed);
    if (out->fresh && prev && cur->t_ns > prev->t_ns) {
        uint64_t dt = cur->t_ns - prev->t_ns;
        if (dt > 1000000ull && dt < NS_DISCONT_NS) {
            uint64_t p = atomic_load_explicit(&b->period_ns, memory_order_relaxed);
            if (b->last_cur_seq == 0) p = dt;
            else p = (uint64_t)(0.8 * (double)p + 0.2 * (double)dt);
            atomic_store_explicit(&b->period_ns, p, memory_order_relaxed);
        }
    }
    if (out->fresh) b->last_cur_seq = cur->seq;
    if (prev && ns_scene_continuous(prev, cur)) {
        out->interp = 1;
        double period = (double)atomic_load_explicit(&b->period_ns, memory_order_relaxed);
        double lead = period * (double)b->lead_factor;
        if (lead < 20e6) lead = 20e6;
        if (lead > 100e6) lead = 100e6;
        double T = (double)now_ns - lead;
        double a = (T - (double)prev->t_ns) / (double)(cur->t_ns - prev->t_ns);
        if (!(a > 0.0)) a = 0.0;
        double amax = 1.0 + (double)b->max_extrap;
        if (a > amax) a = amax;
        out->alpha = (float)a;
    }
    return 1;
}

/* ------------------------------------------------------------------------------------------------ interpolación */
static void lerp12(const NsXf *a, const NsXf *b, float t, NsXf *o) {
    for (int i = 0; i < 12; i++) o->m[i] = a->m[i] + (b->m[i] - a->m[i]) * t;
}

/* Descompone M = R * diag(s) (columnas de R unitarias y ortogonales, det +1). 0 si no es una rotación con escala. */
static int decomp(const NsXf *x, float R[9], float s[3]) {
    for (int j = 0; j < 3; j++) {
        float c0 = x->m[j], c1 = x->m[4 + j], c2 = x->m[8 + j];
        s[j] = sqrtf(c0 * c0 + c1 * c1 + c2 * c2);
        if (!(s[j] > 1e-6f) || !isfinite(s[j])) return 0;
        R[j] = c0 / s[j]; R[3 + j] = c1 / s[j]; R[6 + j] = c2 / s[j];
    }
    for (int j = 0; j < 3; j++) {
        int k = (j + 1) % 3;
        float d = R[j] * R[k] + R[3 + j] * R[3 + k] + R[6 + j] * R[6 + k];
        if (fabsf(d) > 0.02f) return 0;
    }
    float det = R[0] * (R[4] * R[8] - R[5] * R[7]) - R[1] * (R[3] * R[8] - R[5] * R[6]) + R[2] * (R[3] * R[7] - R[4] * R[6]);
    return det > 0.95f && det < 1.05f;
}

static void mat_to_quat(const float R[9], float q[4]) {   /* q = x,y,z,w */
    float tr = R[0] + R[4] + R[8];
    if (tr > 0.0f) {
        float S = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * S; q[0] = (R[7] - R[5]) / S; q[1] = (R[2] - R[6]) / S; q[2] = (R[3] - R[1]) / S;
    } else if (R[0] > R[4] && R[0] > R[8]) {
        float S = sqrtf(1.0f + R[0] - R[4] - R[8]) * 2.0f;
        q[3] = (R[7] - R[5]) / S; q[0] = 0.25f * S; q[1] = (R[1] + R[3]) / S; q[2] = (R[2] + R[6]) / S;
    } else if (R[4] > R[8]) {
        float S = sqrtf(1.0f + R[4] - R[0] - R[8]) * 2.0f;
        q[3] = (R[2] - R[6]) / S; q[0] = (R[1] + R[3]) / S; q[1] = 0.25f * S; q[2] = (R[5] + R[7]) / S;
    } else {
        float S = sqrtf(1.0f + R[8] - R[0] - R[4]) * 2.0f;
        q[3] = (R[3] - R[1]) / S; q[0] = (R[2] + R[6]) / S; q[1] = (R[5] + R[7]) / S; q[2] = 0.25f * S;
    }
    float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 0.0f) for (int i = 0; i < 4; i++) q[i] /= n;
}

static void quat_to_mat(const float q[4], float R[9]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    R[0] = 1 - 2 * (y * y + z * z); R[1] = 2 * (x * y - z * w);     R[2] = 2 * (x * z + y * w);
    R[3] = 2 * (x * y + z * w);     R[4] = 1 - 2 * (x * x + z * z); R[5] = 2 * (y * z - x * w);
    R[6] = 2 * (x * z - y * w);     R[7] = 2 * (y * z + x * w);     R[8] = 1 - 2 * (x * x + y * y);
}

void ns_xf_lerp(const NsXf *a, const NsXf *b, float t, NsXf *out) {
    if (!(fabsf(t) > 0.0f)) { *out = *a; return; }
    if (memcmp(a, b, sizeof *a) == 0) { *out = *a; return; }
    float Ra[9], Rb[9], sa[3], sb[3];
    if (!decomp(a, Ra, sa) || !decomp(b, Rb, sb)) { lerp12(a, b, t, out); return; }
    float qa[4], qb[4], q[4];
    mat_to_quat(Ra, qa); mat_to_quat(Rb, qb);
    float d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
    if (d < 0.0f) { for (int i = 0; i < 4; i++) qb[i] = -qb[i]; d = -d; }
    if (d > 0.9995f) {
        for (int i = 0; i < 4; i++) q[i] = qa[i] + (qb[i] - qa[i]) * t;
    } else {
        float th = acosf(d), sn = sinf(th);
        float wa = sinf((1.0f - t) * th) / sn, wb = sinf(t * th) / sn;
        for (int i = 0; i < 4; i++) q[i] = qa[i] * wa + qb[i] * wb;
    }
    float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(n > 0.0f)) { lerp12(a, b, t, out); return; }
    for (int i = 0; i < 4; i++) q[i] /= n;
    float R[9];
    quat_to_mat(q, R);
    float s[3];
    for (int j = 0; j < 3; j++) s[j] = sa[j] + (sb[j] - sa[j]) * t;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) out->m[4 * i + j] = R[3 * i + j] * s[j];
        out->m[4 * i + 3] = a->m[4 * i + 3] + (b->m[4 * i + 3] - a->m[4 * i + 3]) * t;
    }
}

/* Bits ALTOS del producto de Fibonacci (los bajos de una dirección alineada casi no varían). */
static uint32_t hslot(uint32_t key, uint32_t shift) { return (key * 2654435761u) >> shift; }

void ns_interp_begin(NsBridge *b, const NsView *v, NsInterp *out) {
    memset(out, 0, sizeof *out);
    out->alpha = 1.0f;
    if (!v || !v->cur) return;
    out->cur = v->cur; out->prev = v->prev;
    if (!b || !v->interp || !v->prev) return;
    const NsScene *cur = v->cur, *prev = v->prev;
    uint32_t n = cur->n_inst < b->caps.cap_inst ? cur->n_inst : b->caps.cap_inst;
    uint32_t np = prev->n_inst < b->caps.cap_inst ? prev->n_inst : b->caps.cap_inst;
    if (!(b->map_valid && b->map_cur_seq == cur->seq && b->map_prev_seq == prev->seq)) {
        memset(b->tab_idx, 0xFF, b->tab_n * sizeof *b->tab_idx);   /* -1 = vacío */
        for (uint32_t i = 0; i < np; i++) {
            uint32_t h = hslot(prev->inst[i].key, b->tab_shift);
            while (b->tab_idx[h] >= 0) {
                if (b->tab_key[h] == prev->inst[i].key) break;   /* clave repetida: gana la primera */
                h = (h + 1) & (b->tab_n - 1u);
            }
            if (b->tab_idx[h] < 0) { b->tab_idx[h] = (int32_t)i; b->tab_key[h] = prev->inst[i].key; }
        }
        for (uint32_t i = 0; i < n; i++) {
            int32_t r = -1;
            uint32_t h = hslot(cur->inst[i].key, b->tab_shift);
            while (b->tab_idx[h] >= 0) {
                if (b->tab_key[h] == cur->inst[i].key) { r = b->tab_idx[h]; break; }
                h = (h + 1) & (b->tab_n - 1u);
            }
            b->map[i] = r;
        }
        b->map_cur_seq = cur->seq; b->map_prev_seq = prev->seq; b->map_valid = 1;
    }
    out->enabled = 1;
    out->alpha = v->alpha;
    out->map = b->map;
}

int ns_interp_piece(const NsInterp *ip, uint32_t inst, uint32_t k, NsXf *out) {
    const NsScene *cur = ip->cur;
    if (!cur || inst >= cur->n_inst) return 0;
    const NsInstance *ci = &cur->inst[inst];
    if (k >= ci->n_pieces || (uint64_t)ci->first_piece + k >= cur->n_piece) return 0;
    const NsPiece *cp = &cur->piece[ci->first_piece + k];
    *out = cp->world;
    if (!ip->enabled || !ip->map || (ci->flags & NS_IF_STATIC)) return 0;
    int32_t pj = ip->map[inst];
    const NsScene *prev = ip->prev;
    if (pj < 0 || (uint32_t)pj >= prev->n_inst) return 0;
    const NsInstance *pi = &prev->inst[pj];
    if (pi->n_pieces != ci->n_pieces || (uint64_t)pi->first_piece + k >= prev->n_piece) return 0;
    const NsPiece *pp = &prev->piece[pi->first_piece + k];
    if (pp->mesh_id != cp->mesh_id) return 0;
    float dx = pp->world.m[3] - cp->world.m[3], dy = pp->world.m[7] - cp->world.m[7], dz = pp->world.m[11] - cp->world.m[11];
    if (dx * dx + dy * dy + dz * dz > NS_TELEPORT_UNITS * NS_TELEPORT_UNITS) return 0;
    ns_xf_lerp(&pp->world, &cp->world, ip->alpha, out);
    return 1;
}

void ns_interp_to_vr(const NsInterp *ip, float out[12]) {
    const NsScene *cur = ip->cur;
    if (!cur) { memset(out, 0, 12 * sizeof(float)); return; }
    NsXf c;
    memcpy(c.m, cur->to_vr, sizeof c.m);
    if (ip->enabled && ip->prev && (ip->prev->valid & NS_SV_TOVR_OK) && (cur->valid & NS_SV_TOVR_OK)) {
        NsXf p, o;
        memcpy(p.m, ip->prev->to_vr, sizeof p.m);
        ns_xf_lerp(&p, &c, ip->alpha, &o);
        memcpy(out, o.m, sizeof o.m);
    } else {
        memcpy(out, c.m, sizeof c.m);
    }
}
