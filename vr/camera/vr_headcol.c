/* vr_headcol: ver vr_headcol.h. */
#include "vr_headcol.h"
#include <math.h>
#include <string.h>

#define HC_SLOTS 16384
#define HC_PROBE 8
#define HC_MAX_CAND 16384

typedef struct { float v[3][3]; uint64_t key; uint32_t stamp; uint8_t used; } HcTri;

static struct {
    VrHcCfg cfg;
    HcTri *tris;                    /* HC_SLOTS (estático) */
    uint32_t now;                   /* escenas alimentadas */
    int live_hint;
    float fade;                     /* fundido suavizado */
    int n_nat; int nat_over; float nat_age;   /* lista nativa (mundo del motor): triángulos, desbordada, segundos de consultas desde la última alimentación */
} H;
static HcTri g_tris[HC_SLOTS];
static float g_nat[HC_NAT_MAX][3][3];

void vrhc_config_default(VrHcCfg *c) {
    memset(c, 0, sizeof *c);
    c->enabled = 1; c->radius_m = 0.20f; c->keep_m = 2.0f; c->body_m = 0.30f; c->safe_m = 0.12f; c->face_m = 0.18f; c->push_max_m = 0.60f; c->through_m = 0.03f;
    c->fade_start_m = 0.10f; c->fade_full_m = 0.30f; c->fade_release_s = 0.30f; c->ttl = 100; c->step_frac = 0.25f;
}
void vrhc_init(const VrHcCfg *c) {
    memset(&H, 0, sizeof H);
    memset(g_tris, 0, sizeof g_tris);
    H.tris = g_tris;
    if (c) H.cfg = *c; else vrhc_config_default(&H.cfg);
    VrHcCfg d; vrhc_config_default(&d);
    if (!(H.cfg.radius_m >= 0.03f && H.cfg.radius_m <= 0.5f)) H.cfg.radius_m = d.radius_m;
    if (!(H.cfg.keep_m >= 0.5f && H.cfg.keep_m <= 10.0f)) H.cfg.keep_m = d.keep_m;
    if (!(H.cfg.body_m >= 0.0f && H.cfg.body_m <= 1.0f)) H.cfg.body_m = d.body_m;
    if (!(H.cfg.safe_m >= 0.0f && H.cfg.safe_m < H.cfg.radius_m)) H.cfg.safe_m = d.safe_m < H.cfg.radius_m ? d.safe_m : 0.0f;
    if (!(H.cfg.face_m >= H.cfg.safe_m && H.cfg.face_m <= H.cfg.radius_m)) H.cfg.face_m = H.cfg.safe_m + 0.5f * (H.cfg.radius_m - H.cfg.safe_m);
    if (!(H.cfg.push_max_m >= 0.0f && H.cfg.push_max_m <= 2.0f)) H.cfg.push_max_m = d.push_max_m;
    if (!(H.cfg.through_m >= 0.0f && H.cfg.through_m < H.cfg.radius_m)) H.cfg.through_m = d.through_m < H.cfg.radius_m ? d.through_m : 0.0f;
    if (!(H.cfg.fade_start_m >= 0.0f && H.cfg.fade_start_m < 2.0f)) H.cfg.fade_start_m = d.fade_start_m;
    if (!(H.cfg.fade_full_m > H.cfg.fade_start_m && H.cfg.fade_full_m < 3.0f)) H.cfg.fade_full_m = H.cfg.fade_start_m + 0.27f;
    if (!(H.cfg.fade_release_s > 0.0f && H.cfg.fade_release_s < 10.0f)) H.cfg.fade_release_s = d.fade_release_s;
    if (H.cfg.ttl < 1) H.cfg.ttl = d.ttl;
    if (!(H.cfg.step_frac >= 0.05f && H.cfg.step_frac <= 1.0f)) H.cfg.step_frac = d.step_frac;
}
const VrHcCfg *vrhc_config(void) { return &H.cfg; }
void vrhc_reset(void) {
    if (!H.tris) { vrhc_init(NULL); return; }
    memset(g_tris, 0, sizeof g_tris);
    H.fade = 0.0f; H.now = 0; H.n_nat = 0; H.nat_over = 0; H.nat_age = 0.0f;
}

int vrhc_map_from_record(VrHcMap *m, const VrCamRecord *r) {
    memset(m, 0, sizeof *m);
    if (!r || !r->active) return 0;
    float th = (r->theta_play_deg + 180.0f) * (3.14159265f / 180.0f), cs = cosf(th), sn = sinf(th);
    const float R[9] = {cs, 0, -sn, 0, 1, 0, sn, 0, cs};
    memcpy(m->R, R, sizeof R);
    for (int i = 0; i < 3; i++) { m->eye[i] = (float)r->eye[i]; m->p_ref[i] = r->p_ref[i]; }
    m->upm = r->units_per_m > 1.0f ? r->units_per_m : 1000.0f;
    m->valid = 1;
    for (int i = 0; i < 3; i++) if (!isfinite(m->eye[i]) || !isfinite(m->p_ref[i])) m->valid = 0;
    return m->valid;
}

/* usuario -> mundo fijo del motor: W = eye + upm * R^T (P - p_ref) */
static void to_world(const VrHcMap *m, const float p[3], float w[3]) {
    float d[3] = {p[0] - m->p_ref[0], p[1] - m->p_ref[1], p[2] - m->p_ref[2]};
    for (int i = 0; i < 3; i++) w[i] = m->eye[i] + m->upm * (m->R[i] * d[0] + m->R[3 + i] * d[1] + m->R[6 + i] * d[2]);
}
/* mundo -> usuario: P = p_ref + R (W - eye) / upm */
static void to_user(const VrHcMap *m, const float w[3], float p[3]) {
    float d[3] = {w[0] - m->eye[0], w[1] - m->eye[1], w[2] - m->eye[2]};
    for (int i = 0; i < 3; i++) p[i] = m->p_ref[i] + (m->R[3 * i] * d[0] + m->R[3 * i + 1] * d[1] + m->R[3 * i + 2] * d[2]) / m->upm;
}

/* ---------------------------------------------------------------- geometría */
static inline float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline void sub3(const float a[3], const float b[3], float o[3]) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }

/* Punto más cercano de un triángulo (Ericson, Real-Time Collision Detection 5.1.5). */
static void closest_pt(const float a[3], const float b[3], const float c[3], const float p[3], float q[3]) {
    float ab[3], ac[3], ap[3];
    sub3(b, a, ab); sub3(c, a, ac); sub3(p, a, ap);
    float d1 = dot3(ab, ap), d2 = dot3(ac, ap);
    if (d1 <= 0 && d2 <= 0) { memcpy(q, a, 12); return; }
    float bp[3]; sub3(p, b, bp);
    float d3 = dot3(ab, bp), d4 = dot3(ac, bp);
    if (d3 >= 0 && d4 <= d3) { memcpy(q, b, 12); return; }
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) { float t = d1 / (d1 - d3); for (int i = 0; i < 3; i++) q[i] = a[i] + t * ab[i]; return; }
    float cp[3]; sub3(p, c, cp);
    float d5 = dot3(ab, cp), d6 = dot3(ac, cp);
    if (d6 >= 0 && d5 <= d6) { memcpy(q, c, 12); return; }
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) { float t = d2 / (d2 - d6); for (int i = 0; i < 3; i++) q[i] = a[i] + t * ac[i]; return; }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) { float t = (d4 - d3) / ((d4 - d3) + (d5 - d6)); for (int i = 0; i < 3; i++) q[i] = b[i] + t * (c[i] - b[i]); return; }
    float den = 1.0f / (va + vb + vc), v = vb * den, w = vc * den;
    for (int i = 0; i < 3; i++) q[i] = a[i] + ab[i] * v + ac[i] * w;
}
float vrhc_tri_dist(const float a[3], const float b[3], const float c[3], const float p[3], float closest[3]) {
    float q[3]; closest_pt(a, b, c, p, q);
    if (closest) memcpy(closest, q, 12);
    float d[3]; sub3(p, q, d);
    return sqrtf(dot3(d, d));
}

/* ---------------------------------------------------------------- almacén */
static uint64_t mix64(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33; return x; }
static uint64_t tri_key(const float w[3][3]) {
    float c[3] = {(w[0][0] + w[1][0] + w[2][0]) / 3, (w[0][1] + w[1][1] + w[2][1]) / 3, (w[0][2] + w[1][2] + w[2][2]) / 3};
    float e = 0;
    for (int i = 0; i < 3; i++) { float d[3]; sub3(w[i], w[(i + 1) % 3], d); e += sqrtf(dot3(d, d)); }
    const float q = 1.0f / 8.0f;
    int64_t a = (int64_t)floorf(c[0] * q), b = (int64_t)floorf(c[1] * q), d = (int64_t)floorf(c[2] * q), f = (int64_t)floorf(e * q);
    uint64_t k = mix64((uint64_t)a * 0x9E3779B97F4A7C15ULL ^ mix64((uint64_t)b + 0x1234567ULL) ^ mix64((uint64_t)d * 31u + 7u) ^ mix64((uint64_t)f * 131u + 99u));
    return k ? k : 1;
}
static int alive(const HcTri *t) { return t->used && (uint32_t)(H.now - t->stamp) < (uint32_t)H.cfg.ttl; }

int vrhc_count(void) {
    int n = 0;
    if (!H.tris) return 0;
    for (int i = 0; i < HC_SLOTS; i++) if (alive(&H.tris[i])) n++;
    return n;
}

static int store(const float w[3][3]) {
    uint64_t k = tri_key(w);
    uint32_t s0 = (uint32_t)(k >> 7) & (HC_SLOTS - 1);
    HcTri *victim = NULL;
    for (int p = 0; p < HC_PROBE; p++) {
        HcTri *t = &H.tris[(s0 + (uint32_t)p) & (HC_SLOTS - 1)];
        if (t->used && t->key == k) { memcpy(t->v, w, sizeof t->v); t->stamp = H.now; return 1; }
        if (!alive(t)) { if (!victim) victim = t; }
        else if (!victim && p == HC_PROBE - 1) victim = t;   /* ventana llena de vivos: el último */
    }
    if (!victim) return 0;
    memcpy(victim->v, w, sizeof victim->v); victim->key = k; victim->stamp = H.now; victim->used = 1;
    return 1;
}

static int near_joint(const float w[3][3], const float (*j)[3], int nj, float r) {
    if (nj <= 0 || r <= 0) return 0;
    float r2 = r * r;
    for (int v = 0; v < 3; v++) {
        int ok = 0;
        for (int i = 0; i < nj && !ok; i++) { float d[3]; sub3(w[v], j[i], d); if (dot3(d, d) <= r2) ok = 1; }
        if (!ok) return 0;
    }
    return 1;
}

static int feed_one(const VrHcMap *m, const float p[3][3], const float (*joints)[3], int nj) {
    float keep2 = H.cfg.keep_m * H.cfg.keep_m;
    int near = 0;
    for (int v = 0; v < 3; v++) {
        if (!isfinite(p[v][0]) || !isfinite(p[v][1]) || !isfinite(p[v][2])) return 0;
        float d[3]; sub3(p[v], m->p_ref, d);
        if (dot3(d, d) <= keep2) near = 1;
    }
    if (!near) {   /* un triángulo grande (suelo, pared) con ningún vértice cerca puede cruzar la zona: se conserva si el punto más cercano está cerca */
        float q[3], d[3];
        closest_pt(p[0], p[1], p[2], m->p_ref, q);
        sub3(q, m->p_ref, d);
        if (dot3(d, d) > keep2) return 0;
    }
    float w[3][3];
    for (int v = 0; v < 3; v++) to_world(m, p[v], w[v]);
    /* degenerado */
    float e1[3], e2[3], n[3];
    sub3(w[1], w[0], e1); sub3(w[2], w[0], e2);
    n[0] = e1[1] * e2[2] - e1[2] * e2[1]; n[1] = e1[2] * e2[0] - e1[0] * e2[2]; n[2] = e1[0] * e2[1] - e1[1] * e2[0];
    if (dot3(n, n) < 1.0f) return 0;                       /* área < 0,5 unidad^2 */
    if (near_joint(w, joints, nj, H.cfg.body_m * m->upm)) return 0;   /* cuerpo del propio personaje */
    return store(w);
}

int vrhc_feed_tris(const VrHcMap *m, const float *tris, int n, const float (*joints)[3], int nj) {
    if (!H.tris) vrhc_init(NULL);
    if (!H.cfg.enabled || !m || !m->valid || !tris || n <= 0) return 0;
    H.now++;
    int k = 0;
    for (int i = 0; i < n; i++) {
        float p[3][3];
        memcpy(p, tris + 9 * i, sizeof p);
        k += feed_one(m, p, joints, nj);
    }
    return k;
}

int vrhc_feed_scene(const VrHcMap *m, const VrScene *sc, const float (*joints)[3], int nj) {
    if (!H.tris) vrhc_init(NULL);
    if (!H.cfg.enabled || !m || !m->valid || !sc || !sc->user_space || !sc->v || !sc->b) return 0;
    H.now++;
    int k = 0;
    for (uint32_t bi = 0; bi < sc->nb; bi++) {
        const VrBatch *b = &sc->b[bi];
        if (b->kind != 0) continue;                         /* solo 3D opaco */
        for (uint32_t i = 0; i + 2 < b->count; i += 3) {
            const VrVtx *v = &sc->v[b->first + i];
            if ((v[0].flags | v[1].flags | v[2].flags) & VR_F_2D) continue;
            float p[3][3];
            for (int j = 0; j < 3; j++) memcpy(p[j], v[j].pos, 12);
            k += feed_one(m, p, joints, nj);
        }
    }
    return k;
}

int vrhc_feed_world(const float *tris, int n, int truncated) {
    if (!H.tris) vrhc_init(NULL);
    H.nat_over = truncated != 0;
    if (!H.cfg.enabled || !tris || n <= 0) { H.n_nat = 0; return 0; }
    if (n > HC_NAT_MAX) { n = HC_NAT_MAX; H.nat_over = 1; }
    int k = 0;
    for (int i = 0; i < n; i++) {
        const float *p = tris + 9 * i;
        int ok = 1;
        for (int j = 0; j < 9; j++) if (!isfinite(p[j])) ok = 0;
        if (!ok) continue;
        memcpy(g_nat[k++], p, 36);
    }
    H.n_nat = k; H.nat_age = 0.0f;
    return k;
}
int vrhc_native_count(void) { return H.n_nat; }

/* ---------------------------------------------------------------- consulta */
typedef struct { const float (*v)[3]; float thr; float lo[3], hi[3]; } Cand;
static inline float cand_dist(const Cand *c, const float p[3], float q[3]) { return vrhc_tri_dist(c->v[0], c->v[1], c->v[2], p, q); }
static inline int cand_far(const Cand *c, const float p[3]) {   /* rechazo rápido: p fuera de la caja del triángulo ensanchada por thr */
    for (int a = 0; a < 3; a++) if (p[a] < c->lo[a] - c->thr || p[a] > c->hi[a] + c->thr) return 1;
    return 0;
}

/* Primer punto del segmento a->b (a lo largo de t en 0..1) donde la esfera viola algún candidato; devuelve 1 si lo hay. t_ok = último t sin violación. */
static int sweep_first(Cand *cs, int nc, const float a[3], const float b[3], float step, float *t_ok, int *which) {
    float d[3]; sub3(b, a, d);
    float len = sqrtf(dot3(d, d));
    int ns = (int)ceilf(len / step); if (ns < 1) ns = 1; if (ns > 256) ns = 256;
    float prev = 0.0f;
    for (int s = 1; s <= ns; s++) {
        float t = (float)s / (float)ns, p[3] = {a[0] + d[0] * t, a[1] + d[1] * t, a[2] + d[2] * t};
        int hit = -1;
        for (int i = 0; i < nc; i++) {
            if (cand_far(&cs[i], p)) continue;
            if (cand_dist(&cs[i], p, NULL) < cs[i].thr) { hit = i; break; }
        }
        if (hit >= 0) {
            float lo = prev, hi = t;
            for (int it = 0; it < 10; it++) {
                float mid = 0.5f * (lo + hi), q[3] = {a[0] + d[0] * mid, a[1] + d[1] * mid, a[2] + d[2] * mid};
                int h2 = 0;
                for (int i = 0; i < nc; i++)
                    if (!cand_far(&cs[i], q) && cand_dist(&cs[i], q, NULL) < cs[i].thr) { h2 = 1; break; }
                if (h2) hi = mid; else lo = mid;
            }
            *t_ok = lo; *which = hit;
            return 1;
        }
        prev = t;
    }
    return 0;
}

/* Distancia (unidades del motor) de un punto del MUNDO FIJO a la superficie estática más cercana dentro de `max_units`; -1 si no hay ninguna.
 * Solo lectura del almacén (el que llama sostiene el candado del host). Sirve al golpe en la pared. */
float vrhc_nearest_world(const float p[3], float max_units) {
    if (!H.tris || !p || !isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]) || !(max_units > 0)) return -1.0f;
    float best = max_units;
    int found = 0;
    for (int i = 0; i < HC_SLOTS; i++) {
        const HcTri *t = &H.tris[i];
        if (!alive(t)) continue;
        int out_ = 0;
        for (int ax = 0; ax < 3 && !out_; ax++) {
            float mn = t->v[0][ax], mx = mn;
            for (int v = 1; v < 3; v++) { if (t->v[v][ax] < mn) mn = t->v[v][ax]; if (t->v[v][ax] > mx) mx = t->v[v][ax]; }
            if (mx < p[ax] - best || mn > p[ax] + best) out_ = 1;
        }
        if (out_) continue;
        float d = vrhc_tri_dist(t->v[0], t->v[1], t->v[2], p, NULL);
        if (d < best) { best = d; found = 1; }
    }
    return found ? best : -1.0f;
}

static float smooth01(float x) { x = x < 0 ? 0 : (x > 1 ? 1 : x); return x * x * (3 - 2 * x); }

int vrhc_query(const VrHcMap *m, const float head[3], float dt, VrHcOut *out) {
    VrHcOut o; memset(&o, 0, sizeof o);
    if (!H.tris) vrhc_init(NULL);
    if (!out) out = &o;
    memset(out, 0, sizeof *out);
    float rel = dt > 0 && dt < 1.0f ? dt / H.cfg.fade_release_s : 0.0f;
    if (!H.cfg.enabled || !m || !m->valid || !head || !isfinite(head[0]) || !isfinite(head[1]) || !isfinite(head[2])) {
        H.fade = H.fade > rel ? H.fade - rel : 0.0f;
        out->fade = H.fade;
        return H.fade > 0.0f;
    }
    float A[3], B[3];                                        /* mundo: ancla = ojo del personaje; B = cabeza física */
    memcpy(A, m->eye, 12);
    to_world(m, head, B);
    if (dt > 0.0f) H.nat_age += dt < 1.0f ? dt : 1.0f;
    int nat_live = H.n_nat > 0 && H.nat_age < 1.0f;
    out->native_live = nat_live;
    float r = H.cfg.radius_m * m->upm, hard = H.cfg.safe_m * m->upm, face = H.cfg.face_m * m->upm, pushmax = H.cfg.push_max_m * m->upm;
    float step = r * H.cfg.step_frac;
    /* candidatos: AABB del segmento + radio + margen de expulsión (si desborda, solo radio + 1 unidad; si aun así desborda, negro por seguridad) */
    static Cand cs[HC_MAX_CAND];
    int nc = 0, over = 0;
    for (int pass = 0; pass < 2; pass++) {
        float mg = pass == 0 ? r + pushmax : r + 1.0f, lo[3], hi[3];
        for (int i = 0; i < 3; i++) { lo[i] = (A[i] < B[i] ? A[i] : B[i]) - mg; hi[i] = (A[i] > B[i] ? A[i] : B[i]) + mg; }
        nc = 0; over = 0;
        for (int s = 0; s < 2 && !over; s++) {
            int cnt = s == 0 ? (nat_live ? H.n_nat : 0) : HC_SLOTS;
            for (int i = 0; i < cnt; i++) {
                const float (*v)[3];
                if (s == 0) v = g_nat[i];
                else { const HcTri *t = &H.tris[i]; if (!alive(t)) continue; v = (const float (*)[3])t->v; }
                float mn[3], mx[3];
                int out_ = 0;
                for (int ax = 0; ax < 3; ax++) {
                    mn[ax] = mx[ax] = v[0][ax];
                    for (int k = 1; k < 3; k++) { if (v[k][ax] < mn[ax]) mn[ax] = v[k][ax]; if (v[k][ax] > mx[ax]) mx[ax] = v[k][ax]; }
                    if (mx[ax] < lo[ax] || mn[ax] > hi[ax]) { out_ = 1; break; }
                }
                if (out_) continue;
                if (nc >= HC_MAX_CAND) { over = 1; break; }
                cs[nc].v = v; cs[nc].thr = 0; memcpy(cs[nc].lo, mn, 12); memcpy(cs[nc].hi, mx, 12); nc++;
            }
        }
        if (!over) break;
    }
    out->n_cand = nc;
    out->overflow = over || (nat_live && H.nat_over);
    float pos[3]; memcpy(pos, A, 12);
    float rem[3]; sub3(B, A, rem);
    /* distancia del ancla al sólido más cercano (holgura inicial) */
    float dA = 1e30f;
    for (int i = 0; i < nc; i++) { float d0 = cand_dist(&cs[i], A, NULL); if (d0 < dA) dA = d0; }
    int hit = 0;
    for (int iter = 0; iter < 3; iter++) {
        float rl = sqrtf(dot3(rem, rem));
        if (rl < 1e-4f * r) break;
        float tgt[3] = {pos[0] + rem[0], pos[1] + rem[1], pos[2] + rem[2]};
        /* radio efectivo desde la posición actual: r, o la holgura inicial mínima si ya estaba más cerca de algo (no se acerca más a nada; los vecinos
         * coplanares de una pared a la que ya se estaba pegado no atrapan). La expulsión de después corrige la holgura insuficiente. */
        float dmin = 1e30f;
        for (int i = 0; i < nc; i++) { float d0 = cand_dist(&cs[i], pos, NULL); if (d0 < dmin) dmin = d0; }
        float reff = dmin >= r ? r : (dmin - 0.02f * r > 0 ? dmin - 0.02f * r : 0.0f);
        for (int i = 0; i < nc; i++) cs[i].thr = reff;
        float t_ok = 1.0f; int which = -1;
        if (!sweep_first(cs, nc, pos, tgt, step, &t_ok, &which)) { memcpy(pos, tgt, 12); rem[0] = rem[1] = rem[2] = 0; break; }
        hit = 1;
        for (int i = 0; i < 3; i++) pos[i] += rem[i] * t_ok;
        /* normal de contacto: del punto más cercano hacia el centro; desliza el resto del movimiento por ese plano */
        float q[3], n[3];
        cand_dist(&cs[which], pos, q);
        sub3(pos, q, n);
        float nl = sqrtf(dot3(n, n));
        if (nl < 1e-6f) break;
        for (int i = 0; i < 3; i++) n[i] /= nl;
        float left[3] = {rem[0] * (1 - t_ok), rem[1] * (1 - t_ok), rem[2] * (1 - t_ok)};
        float into = dot3(left, n);
        if (into < 0) for (int i = 0; i < 3; i++) left[i] -= n[i] * into;
        memcpy(rem, left, 12);
    }
    out->hit = hit;
    /* EXPULSIÓN: la cara no puede quedar a menos de face del sólido más cercano (el plano cercano no corta paredes). Normal de contacto; hasta 6 pasadas (esquinas). */
    float pos0[3]; memcpy(pos0, pos, 12);
    float pushed = 0.0f;
    for (int it = 0; it < 6; it++) {
        float dn = 1e30f, qn[3] = {0, 0, 0}; int wn = -1;
        for (int i = 0; i < nc; i++) { float q[3]; float d0 = cand_dist(&cs[i], pos, q); if (d0 < dn) { dn = d0; memcpy(qn, q, 12); wn = i; } }
        if (wn < 0 || dn >= face) break;
        float n[3]; sub3(pos, qn, n);
        float nl = sqrtf(dot3(n, n));
        if (nl > 1e-3f) { for (int i = 0; i < 3; i++) n[i] /= nl; }
        else {   /* sobre la superficie: normal del triángulo del lado del ancla (o hacia arriba si no hay lado) */
            const float (*v)[3] = cs[wn].v;
            float e1[3], e2[3];
            sub3(v[1], v[0], e1); sub3(v[2], v[0], e2);
            n[0] = e1[1] * e2[2] - e1[2] * e2[1]; n[1] = e1[2] * e2[0] - e1[0] * e2[2]; n[2] = e1[0] * e2[1] - e1[1] * e2[0];
            nl = sqrtf(dot3(n, n));
            if (nl < 1e-9f) { n[0] = 0; n[1] = -1; n[2] = 0; } else for (int i = 0; i < 3; i++) n[i] /= nl;
            float toA[3]; sub3(A, qn, toA);
            if (dot3(toA, n) < 0) for (int i = 0; i < 3; i++) n[i] = -n[i];
        }
        float need = face - dn + 0.01f * face;
        float room = pushmax - pushed;
        if (room <= 0) break;
        if (need > room) need = room;
        for (int i = 0; i < 3; i++) pos[i] += n[i] * need;
        { float d[3]; sub3(pos, pos0, d); pushed = sqrtf(dot3(d, d)); }
    }
    /* holgura final */
    float clear = 1e30f;
    for (int i = 0; i < nc; i++) { float d0 = cand_dist(&cs[i], pos, NULL); if (d0 < clear) clear = d0; }
    out->pushed_m = pushed / m->upm;
    out->clear_m = clear > 1e29f ? 9.0f : clear / m->upm;
    /* resultado: pos (mundo) -> usuario */
    float pu[3];
    to_user(m, pos, pu);
    for (int i = 0; i < 3; i++) out->corr[i] = pu[i] - head[i];
    out->corr_len = sqrtf(dot3(out->corr, out->corr));
    /* ¿la cabeza FÍSICA atraviesa una superficie? Segmento ancla -> cabeza con un radio fino (through_m); si ya parte pegado, el radio se reduce a su holgura. */
    float through = 0.0f;
    {
        float thr_c = H.cfg.through_m * m->upm;
        if (dA * 0.98f < thr_c) thr_c = dA * 0.98f;
        float d[3]; sub3(B, A, d);
        float len = sqrtf(dot3(d, d));
        if (thr_c > 0.5f && len > 0.5f && nc > 0) {
            for (int i = 0; i < nc; i++) cs[i].thr = thr_c;
            float st = thr_c * 1.5f < step ? thr_c * 1.5f : step, t_ok = 1.0f; int which = -1;
            if (sweep_first(cs, nc, A, B, st, &t_ok, &which)) through = smooth01(len * (1.0f - t_ok) / m->upm / 0.04f);
        }
    }
    out->through = through;
    float pen = clear < hard ? hard - clear : 0.0f;
    out->pen_m = pen / m->upm;
    float f1 = smooth01((out->corr_len - H.cfg.fade_start_m) / (H.cfg.fade_full_m - H.cfg.fade_start_m));
    float f2 = hard > 0 ? smooth01(pen / (0.5f * hard)) : 0.0f;
    float f4 = smooth01((out->pushed_m - 0.25f) / 0.25f);
    float target = f1;
    if (f2 > target) target = f2;
    if (through > target) target = through;
    if (f4 > target) target = f4;
    if (out->overflow) target = 1.0f;                        /* faltan candidatos: ante la duda, negro; nunca ver a través */
    if (target >= H.fade) H.fade = target;
    else H.fade = (H.fade - rel > target) ? H.fade - rel : target;
    out->fade = H.fade;
    return out->hit || pushed > 0.0f || H.fade > 0.0f;
}
