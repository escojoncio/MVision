/* ns_adapt: ver ns_adapt.h. */
#include "ns_adapt.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ns_types.h y ns_light.h definen ambos NsLight/NsFog (el contrato en float y el modelo entero de la UG): se renombran los de ns_light.h aquí. */
#define NsLight NsLightUG
#define NsFog NsFogUG
#include "ns_light.h"
#undef NsLight
#undef NsFog

typedef struct { NsRTri *t; size_t n, cap; } Vec;
static int vec_push(Vec *v, const NsRTri *t) {
    if (v->n == v->cap) {
        size_t nc = v->cap ? v->cap * 2 : 4096;
        NsRTri *p = (NsRTri *)realloc(v->t, nc * sizeof *p);
        if (!p) return -1;
        v->t = p; v->cap = nc;
    }
    v->t[v->n++] = *t;
    return 0;
}

/* tinte estable por (tpage, clut): matiz del hash, saturación media */
static void tint_of(uint16_t tpage, uint16_t clut, float o[3]) {
    uint32_t h = 2166136261u;
    uint8_t b[4] = {(uint8_t)tpage, (uint8_t)(tpage >> 8), (uint8_t)clut, (uint8_t)(clut >> 8)};
    h = ns_hash32(h, b, 4);
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    float hue = (float)(h % 360u) / 60.0f, s = 0.55f;
    int i = (int)hue; float f = hue - (float)i, p = 1.0f - s, q = 1.0f - s * f, t = 1.0f - s * (1.0f - f);
    switch (i % 6) {
    case 0: o[0] = 1; o[1] = t; o[2] = p; break;
    case 1: o[0] = q; o[1] = 1; o[2] = p; break;
    case 2: o[0] = p; o[1] = 1; o[2] = t; break;
    case 3: o[0] = p; o[1] = q; o[2] = 1; break;
    case 4: o[0] = t; o[1] = p; o[2] = 1; break;
    default: o[0] = 1; o[1] = p; o[2] = q; break;
    }
}
static int16_t to_s16(float x) { long v = lroundf(x); return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }
static uint8_t to_u8(float x) { return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : (int)(x + 0.5f)); }

size_t ns_adapt_scene(const NsScene *sc, const NsMeshStore *st, const NsAdaptOpts *opt, NsRTri **out, NsAdaptStats *stats) {
    NsAdaptStats dummy;
    NsAdaptStats *S = stats ? stats : &dummy;
    memset(S, 0, sizeof *S);
    *out = NULL;
    Vec V = {0, 0, 0};
    const float expo = (opt && opt->exposure > 0) ? opt->exposure : 1.0f;
    const uint16_t *vram = opt ? opt->vram : NULL;
    float wp[256][3];
    uint8_t ncol[256][3];

    for (uint32_t ii = 0; ii < sc->n_inst; ii++) {
        const NsInstance *in = &sc->inst[ii];
        if (opt && opt->skip_key && in->key == opt->skip_key) { S->drop_skip_inst += in->n_pieces; continue; }
        for (uint32_t k = 0; k < in->n_pieces; k++) {
            uint32_t pi = in->first_piece + k;
            if (pi >= sc->n_piece) break;
            const NsPiece *p = &sc->piece[pi];
            S->pieces_in++;
            if (p->flags & NS_PF_HIDDEN) { S->drop_hidden_piece++; continue; }
            const NsMesh *m = ns_store_get(st, p->mesh_id);
            if (!m || !m->n_faces) { S->drop_no_mesh++; continue; }
            S->pieces_drawn++;
            /* posiciones de mundo (con soldaduras) */
            const NsPiece *pp = NULL; const NsMesh *pm = NULL;
            if ((m->flags & NS_MF_INDIRECT) && p->parent >= 0 && (uint32_t)p->parent < in->n_pieces) {
                pp = &sc->piece[in->first_piece + (uint32_t)p->parent];
                pm = ns_store_get(st, pp->mesh_id);
                if (pm && (pp->flags & NS_PF_HIDDEN)) pm = NULL;
            }
            for (uint32_t i = 0; i < m->n_verts && i < 256; i++) {
                const NsVert *v = &m->verts[i];
                float l[3];
                const NsXf *xf = &p->world;
                l[0] = v->x; l[1] = v->y; l[2] = v->z;
                if (pm && v->weld != 0xFF && v->weld < pm->n_verts) {
                    l[0] = pm->verts[v->weld].x; l[1] = pm->verts[v->weld].y; l[2] = pm->verts[v->weld].z;
                    xf = &pp->world; S->welded_verts++;
                }
                ns_xf_apply(xf, l, wp[i]);
            }
            /* luz dinámica por normal, en el espacio de la pieza */
            int dyn = 0;
            if ((in->flags & NS_IF_SHADE) && in->light >= 0 && (uint32_t)in->light < sc->n_light && !(m->flags & NS_MF_PAINT) && !(m->flags & NS_MF_UNLIT) &&
                m->n_norms && !(opt && opt->no_shade)) {
                const NsLight *L = &sc->lights[in->light];
                NsLightUG lw, ll;
                int16_t R[3][3];
                for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) {
                    lw.lmat[a][b] = L->dir[a][b]; lw.cmat[a][b] = L->col[a][b]; R[a][b] = to_s16(p->world.m[4 * a + b] * 4096.0f);
                }
                for (int a = 0; a < 3; a++) lw.bg[a] = (int32_t)L->amb[a] * 16;
                memcpy(ll.cmat, lw.cmat, sizeof ll.cmat); memcpy(ll.bg, lw.bg, sizeof ll.bg);
                ns_light_to_local(lw.lmat, R, ll.lmat);
                for (uint32_t j = 0; j < m->n_norms && j < 256; j++) {
                    int16_t n[3] = {m->norms[j].x, m->norms[j].y, m->norms[j].z};
                    ns_light_color(&ll, n, ncol[j]);
                }
                dyn = 1;
            }
            for (uint32_t fi = 0; fi < m->n_faces; fi++) {
                const NsFace *f = &m->faces[fi];
                S->faces_in++;
                uint8_t col[4][3];
                if (m->paint && (f->flags & NS_FF_PAINT_OK)) {
                    for (int c = 0; c < 4; c++) { uint32_t q = m->paint[fi * 4 + (uint32_t)c]; col[c][0] = (uint8_t)q; col[c][1] = (uint8_t)(q >> 8); col[c][2] = (uint8_t)(q >> 16); }
                    S->faces_paint++;
                } else if (dyn) {
                    for (int c = 0; c < 4; c++) memcpy(col[c], ncol[f->n[c] < m->n_norms ? f->n[c] : 0], 3);
                    S->faces_dyn++;
                } else {
                    memset(col, 128, sizeof col); S->faces_flat++;
                }
                int textured = vram && f->texid;
                float tint[3] = {1, 1, 1};
                if (!vram) {
                    if (!(opt && opt->no_tint) && f->texid) tint_of(f->tpage, f->clut, tint);
                }
                NsRVert rv[4];
                for (int c = 0; c < 4; c++) {
                    memset(&rv[c], 0, sizeof rv[c]);
                    memcpy(rv[c].p, wp[f->v[c] < m->n_verts ? f->v[c] : 0], sizeof rv[c].p);
                    rv[c].u = f->u[c]; rv[c].v = f->t[c];
                    if (textured) { rv[c].r = col[c][0]; rv[c].g = col[c][1]; rv[c].b = col[c][2]; }
                    else { rv[c].r = to_u8((float)col[c][0] * expo * tint[0]); rv[c].g = to_u8((float)col[c][1] * expo * tint[1]); rv[c].b = to_u8((float)col[c][2] * expo * tint[2]); }
                }
                if (opt && opt->cull_eye && !(m->flags & NS_MF_BOTHFACE)) {
                    const float *A0 = wp[f->v[0] < m->n_verts ? f->v[0] : 0], *B0 = wp[f->v[1] < m->n_verts ? f->v[1] : 0], *D0 = wp[f->v[3] < m->n_verts ? f->v[3] : 0];
                    float e1[3], e2[3], cr[3], r0[3];
                    for (int a2 = 0; a2 < 3; a2++) { e1[a2] = B0[a2] - A0[a2]; e2[a2] = D0[a2] - A0[a2]; r0[a2] = A0[a2] - opt->cull_eye[a2]; }
                    cr[0] = e1[1] * e2[2] - e1[2] * e2[1]; cr[1] = e1[2] * e2[0] - e1[0] * e2[2]; cr[2] = e1[0] * e2[1] - e1[1] * e2[0];
                    if (r0[0] * cr[0] + r0[1] * cr[1] + r0[2] * cr[2] <= 0.0f) { S->drop_backface++; continue; }
                }
                const int tri = (f->flags & NS_FF_TRI) != 0;
                if (tri) S->tris++; else S->quads++;
                if (textured) S->faces_textured++;
                static const uint8_t order_q[2][3] = {{0, 1, 3}, {1, 3, 2}};   /* hardware: (A,B,D) y (B,D,C) */
                static const uint8_t order_t[1][3] = {{0, 1, 2}};
                int nt = tri ? 1 : 2;
                for (int tt = 0; tt < nt; tt++) {
                    const uint8_t *o = tri ? order_t[tt] : order_q[tt];
                    NsRTri T;
                    memset(&T, 0, sizeof T);
                    for (int c = 0; c < 3; c++) T.v[c] = rv[o[c]];
                    uint8_t i0 = f->v[o[0]], i1 = f->v[o[1]], i2 = f->v[o[2]];
                    float e1[3], e2[3], cr[3];
                    for (int a = 0; a < 3; a++) { e1[a] = T.v[1].p[a] - T.v[0].p[a]; e2[a] = T.v[2].p[a] - T.v[0].p[a]; }
                    cr[0] = e1[1] * e2[2] - e1[2] * e2[1]; cr[1] = e1[2] * e2[0] - e1[0] * e2[2]; cr[2] = e1[0] * e2[1] - e1[1] * e2[0];
                    float area2 = cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2];
                    int finite = isfinite(area2);
                    if (i0 == i1 || i0 == i2 || i1 == i2 || !finite || area2 <= 0.0f) { S->drop_degenerate++; continue; }
                    T.tpage = f->tpage; T.clut = f->clut;
                    T.abr = (uint8_t)((f->tpage >> 5) & 3);
                    if (textured) T.flags |= NS_TF_TEXTURED;
                    if (m->flags & NS_MF_TRANS) T.flags |= NS_TF_SEMI;
                    if (vec_push(&V, &T)) { free(V.t); return 0; }
                    S->faces_out_tris++;
                }
            }
        }
    }
    *out = V.t;
    return V.n;
}
