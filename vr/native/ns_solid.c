/* ns_solid: ver ns_solid.h. */
#include "ns_solid.h"
#include <math.h>
#include <string.h>

/* Posición de mundo del vértice i de la pieza p con las soldaduras al padre (igual que ns_adapt.c: las encadenadas usan la posición ya soldada del padre). */
static void vworld(const NsScene *sc, const NsMeshStore *st, const NsInstance *in, const NsPiece *p, const NsMesh *m, uint32_t i, int depth, float out[3]) {
    const NsVert *v = &m->verts[i];
    if ((m->flags & NS_MF_WELDED) && v->weld != 0xFF && depth < 8 && p->parent >= 0 && (uint32_t)p->parent < in->n_pieces) {
        uint32_t pi = in->first_piece + (uint32_t)p->parent;
        if (pi < sc->n_piece) {
            const NsPiece *pp = &sc->piece[pi];
            const NsMesh *pm = (pp->flags & NS_PF_HIDDEN) ? NULL : ns_store_get(st, pp->mesh_id);
            if (pm && v->weld < pm->n_verts) { vworld(sc, st, in, pp, pm, v->weld, depth + 1, out); return; }
        }
    }
    float l[3] = {(float)v->x, (float)v->y, (float)v->z};
    ns_xf_apply(&p->world, l, out);
}

static int near_joint(const float a[3], const float b[3], const float c[3], const float (*j)[3], int nj, float r) {
    if (nj <= 0 || r <= 0) return 0;
    const float *vs[3] = {a, b, c};
    float r2 = r * r;
    for (int v = 0; v < 3; v++) {
        int ok = 0;
        for (int i = 0; i < nj && !ok; i++) {
            float d0 = vs[v][0] - j[i][0], d1 = vs[v][1] - j[i][1], d2 = vs[v][2] - j[i][2];
            if (d0 * d0 + d1 * d1 + d2 * d2 <= r2) ok = 1;
        }
        if (!ok) return 0;
    }
    return 1;
}

size_t ns_solid_collect(const NsScene *sc, const NsMeshStore *st, const NsSolidOpts *opt, float *tris, NsSolidSrc *src, size_t cap, NsSolidStats *stats) {
    NsSolidStats dummy;
    NsSolidStats *S = stats ? stats : &dummy;
    memset(S, 0, sizeof *S);
    if (!sc || !st || !tris || !sc->inst || !sc->piece) return 0;
    const float rad = opt ? opt->radius : 0.0f;
    const float *ctr = opt ? opt->center : NULL;
    size_t n = 0;
    float wp[256][3];
    for (uint32_t ii = 0; ii < sc->n_inst; ii++) {
        const NsInstance *in = &sc->inst[ii];
        if (opt && opt->skip_key && in->key == opt->skip_key) { S->inst_skipped++; continue; }
        if (in->flags & NS_IF_SHADOW) continue;
        const int is_static = (in->flags & NS_IF_STATIC) != 0;
        S->inst_used++;
        for (uint32_t k = 0; k < in->n_pieces; k++) {
            uint32_t pi = in->first_piece + k;
            if (pi >= sc->n_piece) break;
            const NsPiece *p = &sc->piece[pi];
            S->pieces++;
            if (p->flags & NS_PF_HIDDEN) { S->pieces_hidden++; continue; }
            const NsMesh *m = ns_store_get(st, p->mesh_id);
            if (!m) { if (p->mesh_id) S->pieces_no_mesh++; continue; }
            if (!m->n_faces) continue;
            if ((m->flags & NS_MF_BLEND) && !(opt && opt->include_blend)) { S->pieces_blend++; continue; }
            if (rad > 0 && ctr) {   /* esfera envolvente de la caja local transformada */
                float c[3] = {0.5f * (m->bbox[0] + m->bbox[3]), 0.5f * (m->bbox[1] + m->bbox[4]), 0.5f * (m->bbox[2] + m->bbox[5])};
                float h[3] = {0.5f * (m->bbox[3] - m->bbox[0]), 0.5f * (m->bbox[4] - m->bbox[1]), 0.5f * (m->bbox[5] - m->bbox[2])};
                float cw[3]; ns_xf_apply(&p->world, c, cw);
                float sc2 = 0.0f;
                for (int col = 0; col < 3; col++) {
                    float s2 = p->world.m[col] * p->world.m[col] + p->world.m[4 + col] * p->world.m[4 + col] + p->world.m[8 + col] * p->world.m[8 + col];
                    if (s2 > sc2) sc2 = s2;
                }
                float rr = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]) * sqrtf(sc2) + 4.0f;
                float d0 = cw[0] - ctr[0], d1 = cw[1] - ctr[1], d2 = cw[2] - ctr[2];
                if (d0 * d0 + d1 * d1 + d2 * d2 > (rad + rr) * (rad + rr)) { S->pieces_culled++; continue; }
            }
            for (uint32_t i = 0; i < m->n_verts && i < 256; i++) vworld(sc, st, in, p, m, i, 0, wp[i]);
            for (uint32_t fi = 0; fi < m->n_faces; fi++) {
                const NsFace *f = &m->faces[fi];
                const int tri = (f->flags & NS_FF_TRI) != 0;
                const uint32_t vi[4] = {f->v[0] < m->n_verts ? f->v[0] : 0u, f->v[1] < m->n_verts ? f->v[1] : 0u, f->v[2] < m->n_verts ? f->v[2] : 0u, f->v[3] < m->n_verts ? f->v[3] : 0u};
                static const uint8_t oq[2][3] = {{0, 1, 3}, {1, 3, 2}};
                static const uint8_t ot[1][3] = {{0, 1, 2}};
                const int nt = tri ? 1 : 2;
                for (int t = 0; t < nt; t++) {
                    const uint8_t *o = tri ? ot[t] : oq[t];
                    if (vi[o[0]] == vi[o[1]] || vi[o[0]] == vi[o[2]] || vi[o[1]] == vi[o[2]]) continue;
                    const float *a = wp[vi[o[0]]], *b = wp[vi[o[1]]], *c = wp[vi[o[2]]];
                    float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                    float cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                    float a2 = cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2];
                    if (!(a2 > 0.0f) || !isfinite(a2)) continue;
                    if (rad > 0 && ctr) {   /* caja del triángulo ensanchada por el radio contra el centro */
                        int out = 0;
                        for (int ax = 0; ax < 3 && !out; ax++) {
                            float mn = a[ax], mx = a[ax];
                            if (b[ax] < mn) mn = b[ax];
                            if (b[ax] > mx) mx = b[ax];
                            if (c[ax] < mn) mn = c[ax];
                            if (c[ax] > mx) mx = c[ax];
                            if (ctr[ax] < mn - rad || ctr[ax] > mx + rad) out = 1;
                        }
                        if (out) continue;
                    }
                    if (!is_static && opt && opt->n_joints > 0 && near_joint(a, b, c, opt->joints, opt->n_joints, opt->body_units)) { S->tris_body++; continue; }
                    if (n >= cap) { S->overflow++; continue; }
                    memcpy(tris + 9 * n, a, 12); memcpy(tris + 9 * n + 3, b, 12); memcpy(tris + 9 * n + 6, c, 12);
                    if (src) { src[n].key = in->key; src[n].piece_rel = (uint16_t)k; src[n].is_static = (uint8_t)is_static; src[n].pad = 0; src[n].mesh32 = (uint32_t)p->mesh_id; }
                    if (is_static) S->tris_static++; else S->tris_dyn++;
                    n++;
                }
            }
        }
    }
    return n;
}
