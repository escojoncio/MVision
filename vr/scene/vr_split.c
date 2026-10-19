#include "vr_split.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { SPLIT_MAX_DEPTH = 24 };

static float d2(const VrVtx *a, const VrVtx *b) {
    float x = a->pos[0] - b->pos[0], y = a->pos[1] - b->pos[1], z = a->pos[2] - b->pos[2];
    return x * x + y * y + z * z;
}

static int finite3(const VrVtx *a) { return isfinite(a->pos[0]) && isfinite(a->pos[1]) && isfinite(a->pos[2]); }

/* arista más larga (al cuadrado) de un triángulo; i = índice de la arista (0: v0-v1, 1: v1-v2, 2: v2-v0) */
static float longest(const VrVtx t[3], int *edge) {
    float e0 = d2(&t[0], &t[1]), e1 = d2(&t[1], &t[2]), e2 = d2(&t[2], &t[0]);
    float m = e0; int k = 0;
    if (e1 > m) { m = e1; k = 1; }
    if (e2 > m) { m = e2; k = 2; }
    *edge = k;
    return m;
}

static float lerpf(float a, float b) { return 0.5f * (a + b); }

static VrVtx mid(const VrVtx *a, const VrVtx *b) {
    VrVtx m = *a;
    for (int i = 0; i < 3; i++) m.pos[i] = lerpf(a->pos[i], b->pos[i]);
    for (int i = 0; i < 2; i++) m.uv[i] = lerpf(a->uv[i], b->uv[i]);
    uint32_t c = 0;
    for (int s = 0; s < 32; s += 8) {
        uint32_t ca = (a->rgba >> s) & 255u, cb = (b->rgba >> s) & 255u;
        c |= ((ca + cb + 1u) >> 1) << s;
    }
    m.rgba = c;
    return m;
}

typedef struct { VrVtx p[3]; int depth; } Tri;

typedef struct { VrVtx *v; uint32_t n, cap; } VBuf;

static int vpush(VBuf *o, const VrVtx *t) {
    if (o->n + 3 > o->cap) {
        uint32_t nc = o->cap ? o->cap * 2 : 3072;
        VrVtx *nv = (VrVtx *)realloc(o->v, (size_t)nc * sizeof(VrVtx));
        if (!nv) return 0;
        o->v = nv; o->cap = nc;
    }
    memcpy(o->v + o->n, t, 3 * sizeof(VrVtx));
    o->n += 3;
    return 1;
}

/* Parte un triángulo (DFS, el orden de salida sigue el de la geometría). allowed = nº máximo de triángulos que puede producir. Devuelve nº de hojas o -1 (memoria). */
static int split_one(VBuf *o, const VrVtx src[3], float max_e2, uint32_t allowed, int *capped) {
    Tri st[SPLIT_MAX_DEPTH * 2 + 4];
    int sp = 0;
    memcpy(st[0].p, src, 3 * sizeof(VrVtx)); st[0].depth = 0; sp = 1;
    uint32_t leaves = 0;
    while (sp) {
        Tri cur = st[--sp];
        int e;
        float l2 = longest(cur.p, &e);
        if (l2 <= max_e2) { if (!vpush(o, cur.p)) return -1; leaves++; continue; }
        if (cur.depth >= SPLIT_MAX_DEPTH || leaves + (uint32_t)sp + 2u > allowed || sp + 2 > (int)(sizeof st / sizeof st[0])) {
            *capped = 1;
            if (!vpush(o, cur.p)) return -1;
            leaves++;
            continue;
        }
        const VrVtx *a = &cur.p[e], *b = &cur.p[(e + 1) % 3], *c = &cur.p[(e + 2) % 3];
        VrVtx m = mid(a, b);
        Tri t1, t2;
        t1.p[0] = *a; t1.p[1] = m; t1.p[2] = *c; t1.depth = cur.depth + 1;
        t2.p[0] = m; t2.p[1] = *b; t2.p[2] = *c; t2.depth = cur.depth + 1;
        st[sp++] = t2;   /* primero se procesa t1 */
        st[sp++] = t1;
    }
    return (int)leaves;
}

void vr_split_free(VrSplit *s) {
    if (!s) return;
    free(s->v); free(s->first); free(s->count);
    memset(s, 0, sizeof *s);
}

int vr_split_run(const VrVtx *v, uint32_t nv, const VrBatch *b, uint32_t nb, float max_edge, uint32_t max_out_tris, VrSplit *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!v || !b || !nb || !(max_edge > 0.0f) || !isfinite(max_edge) || !max_out_tris) return -1;
    const float max_e2 = max_edge * max_edge;
    uint32_t *first = (uint32_t *)malloc((size_t)nb * 4), *count = (uint32_t *)malloc((size_t)nb * 4);
    if (!first || !count) { free(first); free(count); return -1; }
    /* 1) qué batches tienen triángulos largos, y cuántos triángulos 3D hay en total */
    uint8_t *need = (uint8_t *)calloc(nb, 1);
    if (!need) { free(first); free(count); return -1; }
    uint32_t tot_src = 0, any = 0;
    for (uint32_t i = 0; i < nb; i++) {
        first[i] = 0xFFFFFFFFu; count[i] = 0;
        if (b[i].kind > 1 || b[i].first >= nv) continue;
        uint32_t n = b[i].count; if (b[i].first + n > nv) n = nv - b[i].first; n -= n % 3u;
        for (uint32_t k = 0; k + 3 <= n; k += 3) {
            const VrVtx *t = v + b[i].first + k;
            tot_src++;
            if (!(finite3(&t[0]) && finite3(&t[1]) && finite3(&t[2]))) continue;
            int e;
            VrVtx tt[3] = {t[0], t[1], t[2]};
            if (longest(tt, &e) > max_e2) need[i] = 1;
        }
        any |= need[i];
    }
    out->src_tris = tot_src;
    if (!any) { free(first); free(count); free(need); return 1; }
    /* 2) reescritura de los batches marcados (todos sus triángulos, en orden) */
    VBuf o = {0, 0, 0};
    uint32_t out_tris = 0, remaining = 0;
    for (uint32_t i = 0; i < nb; i++) if (need[i]) { uint32_t n = b[i].count; if (b[i].first + n > nv) n = nv - b[i].first; remaining += n / 3u; }
    int rc = 0;
    for (uint32_t i = 0; i < nb && rc == 0; i++) {
        if (!need[i]) continue;
        uint32_t n = b[i].count; if (b[i].first + n > nv) n = nv - b[i].first; n -= n % 3u;
        uint32_t start = o.n;
        for (uint32_t k = 0; k + 3 <= n; k += 3) {
            const VrVtx *t = v + b[i].first + k;
            remaining--;
            int capped = 0, big = 0;
            if (finite3(&t[0]) && finite3(&t[1]) && finite3(&t[2])) { int e; VrVtx tt[3] = {t[0], t[1], t[2]}; big = longest(tt, &e) > max_e2; }
            if (!big) {
                if (!vpush(&o, t)) { rc = -1; break; }
                out_tris++;
                continue;
            }
            uint32_t used = out_tris + remaining;
            uint32_t allowed = max_out_tris > used ? max_out_tris - used : 1u;
            int lv = split_one(&o, t, max_e2, allowed, &capped);
            if (lv < 0) { rc = -1; break; }
            out_tris += (uint32_t)lv;
            if (lv > 1) out->split_tris++;
            if (capped) out->cap_skipped++;
        }
        first[i] = nv + start;     /* índice absoluto en el búfer combinado (originales + añadidos) */
        count[i] = o.n - start;
    }
    free(need);
    if (rc) { free(o.v); free(first); free(count); return -1; }
    out->v = o.v; out->n = o.n; out->first = first; out->count = count; out->nb = nb; out->out_tris = out_tris;
    return 0;
}

void vr_scan_tris(const VrVtx *v, uint32_t n, const float view[16], float near, float big_edge, VrScan *acc) {
    if (!v || !view || !acc) return;
    const float big2 = big_edge * big_edge;
    for (uint32_t k = 0; k + 3 <= n; k += 3) {
        float x[3][3], depth[3];
        int ok = 1;
        for (int i = 0; i < 3; i++) {
            const float *p = v[k + (uint32_t)i].pos;
            if (!(isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]))) { ok = 0; break; }
            x[i][0] = p[0]; x[i][1] = p[1]; x[i][2] = p[2];
            depth[i] = -(view[2] * p[0] + view[6] * p[1] + view[10] * p[2] + view[14]);
        }
        if (!ok) continue;
        acc->tris++;
        float e0 = 0, e1 = 0, e2 = 0;
        for (int c = 0; c < 3; c++) {
            float a = x[0][c] - x[1][c], b = x[1][c] - x[2][c], d = x[2][c] - x[0][c];
            e0 += a * a; e1 += b * b; e2 += d * d;
        }
        float mx = e0 > e1 ? e0 : e1; if (e2 > mx) mx = e2;
        if (sqrtf(mx) > acc->max_edge) acc->max_edge = sqrtf(mx);
        int big = mx > big2;
        float dmin = depth[0], dmax = depth[0];
        for (int i = 1; i < 3; i++) { if (depth[i] < dmin) dmin = depth[i]; if (depth[i] > dmax) dmax = depth[i]; }
        int cross = dmin < near && dmax >= near;
        if (big) acc->big++;
        if (cross) acc->cross++;
        if (big && cross) acc->big_cross++;
    }
}
