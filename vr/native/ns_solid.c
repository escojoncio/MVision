/* ns_solid: ver ns_solid.h. */
#include "ns_solid.h"
#include "ns_hand.h"
#include "ns_gpu.h"
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
        if ((opt && opt->skip_key && in->key == opt->skip_key) || ns_key_is_hand(in->key)) { S->inst_skipped++; continue; }
        if (in->flags & NS_IF_SHADOW) continue;
        const int is_static = (in->flags & NS_IF_STATIC) != 0;
        if (is_static && in->zone_mask && sc->group_mask && !(in->zone_mask & sc->group_mask)) continue;   /* escenario de un grupo de mapa no activo (criterio de ns_gpu_make_piece): se dibuja, pero no es sólido */
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

/* z mínimo del triángulo t (vista del motor en px por unidad de tangente: x * H, y * H * factor vertical, z) recortado por la ventana del motor y por z >= zn; 0 si no queda nada dentro. */
float ns_solid_win_min_z(const float t[3][3], float zn) {   /* sin static: la prueba lo llama */
    float a[16][3], b[16][3]; int n = 3;   /* 3 + 5 planos = 8 en exacto; en coma flotante puede pasar (medido: 9) */
    memcpy(a, t, 9 * sizeof(float));
    for (int pl = 0; pl < 5 && n; pl++) {
        int m = 0;
        for (int i = 0; i < n && m <= 14; i++) {   /* cada vértice escribe 2 como mucho: nunca fuera de b */
            const float *p = a[i], *q = a[(i + 1) % n];
            const float dp = pl == 0 ? NSG_ENG_WIN_X * p[2] - p[0] : pl == 1 ? NSG_ENG_WIN_X * p[2] + p[0] : pl == 2 ? NSG_ENG_WIN_Y * p[2] - p[1] : pl == 3 ? NSG_ENG_WIN_Y * p[2] + p[1] : p[2] - zn;
            const float dq = pl == 0 ? NSG_ENG_WIN_X * q[2] - q[0] : pl == 1 ? NSG_ENG_WIN_X * q[2] + q[0] : pl == 2 ? NSG_ENG_WIN_Y * q[2] - q[1] : pl == 3 ? NSG_ENG_WIN_Y * q[2] + q[1] : q[2] - zn;
            if (dp >= 0.0f) memcpy(b[m++], p, 3 * sizeof(float));
            if ((dp >= 0.0f) != (dq >= 0.0f)) { const float u = dp / (dp - dq); for (int k = 0; k < 3; k++) b[m][k] = p[k] + u * (q[k] - p[k]); m++; }
        }
        n = m; memcpy(a, b, sizeof a);
    }
    float z = 0.0f;
    for (int i = 0; i < n; i++) if (i == 0 || a[i][2] < z) z = a[i][2];
    return n >= 3 && z > 0.0f ? z : 0.0f;
}

float ns_solid_view_near(const NsScene *sc, const NsMeshStore *st) {
    if (!sc || !st || !sc->inst || !sc->piece || !(sc->cam.h > 8.0f)) return 0.0f;
    NsGpuEngCam c;
    for (int k = 0; k < 3; k++) { c.eye[k] = sc->cam.eye[k]; c.x[k] = sc->cam.rot[3 * k]; c.y[k] = sc->cam.rot[3 * k + 1]; c.fwd[k] = sc->cam.rot[3 * k + 2]; }
    c.h = c.hx = sc->cam.h; c.hy = sc->cam.h * (sc->cam.overscan_y > 0.5f && sc->cam.overscan_y < 2.0f ? sc->cam.overscan_y : 1.0f);
    const ns_u32 off = ns_gpu_engine_off();
    float best = 0.0f, wp[256][3], wz[256];
    for (uint32_t ii = 0; ii < sc->n_inst; ii++) {
        const NsInstance *in = &sc->inst[ii];
        if (in->flags & NS_IF_SHADOW) continue;   /* el escenario de un grupo de mapa no activo cuenta: el renderer también lo dibuja */
        for (uint32_t k = 0; k < in->n_pieces && in->first_piece + k < sc->n_piece; k++) {
            const NsPiece *p = &sc->piece[in->first_piece + k];
            if (p->flags & NS_PF_HIDDEN) continue;
            const NsMesh *m = ns_store_get(st, p->mesh_id);
            if (!m || !m->verts || !m->faces) continue;
            const uint32_t nv = m->n_verts < 256 ? m->n_verts : 256;
            for (uint32_t i = 0; i < nv; i++) {
                vworld(sc, st, in, p, m, i, 0, wp[i]);
                wz[i] = c.fwd[0] * (wp[i][0] - c.eye[0]) + c.fwd[1] * (wp[i][1] - c.eye[1]) + c.fwd[2] * (wp[i][2] - c.eye[2]);
            }
            for (uint32_t fi = 0; fi < m->n_faces; fi++) {
                const NsFace *f = &m->faces[fi];
                static const int idx[2][3] = {{0, 1, 3}, {1, 2, 3}};   /* el orden de ns_gpu_pack: (A,B,C) o (A,B,D) y (B,C,D) */
                const int nt = (f->flags & NS_FF_TRI) ? 1 : 2;
                for (int t = 0; t < nt; t++) {
                    float w[3][3], g[3][3], v[3][3], zlo = 1e30f; int bad = 0;
                    for (int e = 0; e < 3; e++) { const uint32_t vi = f->v[nt == 1 ? e : idx[t][e]]; if (vi >= nv) { bad = 1; break; } memcpy(w[e], wp[vi], sizeof w[e]); if (wz[vi] < zlo) zlo = wz[vi]; }
                    if (bad || (best > 0.0f && fmaxf(zlo, fminf(NS_SOLID_SCREEN_NEAR, 0.5f * c.h)) >= best)) continue;   /* ni partida (plano cercano) ni ajustada al plano H/2 puede quedar más cerca que lo ya encontrado */
                    const int r = ns_gpu_engine_tri((const float (*)[3])w, &c, !(m->flags & NS_MF_TWOSIDED), !(in->flags & NS_IF_GBOUND), NULL, 0, off, g);
                    if (r != NSG_ENG_DRAW && r != NSG_ENG_SPLIT) continue;   /* descartado, o rescatado solo fuera de la ventana */
                    for (int e = 0; e < 3; e++) {
                        const float d[3] = {g[e][0] - c.eye[0], g[e][1] - c.eye[1], g[e][2] - c.eye[2]};
                        v[e][0] = c.hx * (c.x[0] * d[0] + c.x[1] * d[1] + c.x[2] * d[2]); v[e][1] = c.hy * (c.y[0] * d[0] + c.y[1] * d[1] + c.y[2] * d[2]);
                        v[e][2] = c.fwd[0] * d[0] + c.fwd[1] * d[1] + c.fwd[2] * d[2];
                    }
                    const float zn = r == NSG_ENG_SPLIT ? NS_SOLID_SCREEN_NEAR : 0.0f;
                    if (best > 0.0f && fmaxf(fminf(fminf(v[0][2], v[1][2]), v[2][2]), zn) >= best) continue;
                    const float z = ns_solid_win_min_z((const float (*)[3])v, zn);
                    if (z > 0.0f && (!(best > 0.0f) || z < best)) best = z;
                }
            }
        }
    }
    return best;
}
