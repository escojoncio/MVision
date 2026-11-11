/* ns_sink: ver ns_sink.h. */
#include "ns_sink.h"
#include "ns_extract.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float a[3], b[3], c[3]; float x0, x1, z0, z1, y0, y1; float bias; } Tri;   /* cara de suelo en el mundo + caja + sesgo (positivo, unidades) */
static struct { Tri *t; uint32_t n, cap; uint64_t sig; int have; } F;   /* suelos de la escena vigente (el escenario no cambia entre fotogramas: se rehace solo si cambia su firma) */

static void xf_pt(const NsXf *w, float x, float y, float z, float o[3]) {
    o[0] = w->m[0] * x + w->m[1] * y + w->m[2] * z + w->m[3];
    o[1] = w->m[4] * x + w->m[5] * y + w->m[6] * z + w->m[7];
    o[2] = w->m[8] * x + w->m[9] * y + w->m[10] * z + w->m[11];
}
static void add_tri(const float a[3], const float b[3], const float c[3], float bias) {
    float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    float nx = e1[1] * e2[2] - e1[2] * e2[1], ny = e1[2] * e2[0] - e1[0] * e2[2], nz = e1[0] * e2[1] - e1[1] * e2[0];
    float nl = sqrtf(nx * nx + ny * ny + nz * nz);
    if (!(nl > 1e-3f) || !(fabsf(ny) >= 0.7f * nl)) return;        /* solo caras casi horizontales (suelos); degeneradas fuera */
    if (F.n == F.cap) { uint32_t nc = F.cap ? F.cap * 2 : 256; Tri *nt = (Tri *)realloc(F.t, nc * sizeof *nt); if (!nt) return; F.t = nt; F.cap = nc; }
    Tri *t = &F.t[F.n++];
    memcpy(t->a, a, sizeof t->a); memcpy(t->b, b, sizeof t->b); memcpy(t->c, c, sizeof t->c);
    t->x0 = fminf(a[0], fminf(b[0], c[0])); t->x1 = fmaxf(a[0], fmaxf(b[0], c[0]));
    t->z0 = fminf(a[2], fminf(b[2], c[2])); t->z1 = fmaxf(a[2], fmaxf(b[2], c[2]));
    t->y0 = fminf(a[1], fminf(b[1], c[1])); t->y1 = fmaxf(a[1], fmaxf(b[1], c[1]));
    t->bias = bias;
}
static uint64_t mix(uint64_t h, uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); return h; }

/* Suelos con sesgo negativo del escenario, en el mundo. La firma (mallas y traslaciones de esas piezas) evita rehacerlos cada fotograma. */
static void floors(const NsScene *sc, const NsMeshStore *st) {
    uint64_t sig = 0x5151u;
    for (uint32_t i = 0; i < sc->n_inst; i++) {
        const NsInstance *in = &sc->inst[i];
        if (!(in->flags & NS_IF_STATIC) || (uint64_t)in->first_piece + in->n_pieces > sc->n_piece) continue;
        for (uint32_t k = 0; k < in->n_pieces; k++) {
            const NsPiece *p = &sc->piece[in->first_piece + k];
            if ((p->flags & NS_PF_HIDDEN) || !p->mesh_id) continue;
            const NsMesh *m = ns_store_get(st, p->mesh_id);
            if (!m || m->depth_bias >= 0) continue;
            sig = mix(sig, p->mesh_id);
            for (int j = 0; j < 12; j++) { uint32_t u; memcpy(&u, &p->world.m[j], 4); sig = mix(sig, u); }
        }
    }
    if (F.have && sig == F.sig) return;
    F.n = 0; F.sig = sig; F.have = 1;
    for (uint32_t i = 0; i < sc->n_inst; i++) {
        const NsInstance *in = &sc->inst[i];
        if (!(in->flags & NS_IF_STATIC) || (uint64_t)in->first_piece + in->n_pieces > sc->n_piece) continue;
        for (uint32_t k = 0; k < in->n_pieces; k++) {
            const NsPiece *p = &sc->piece[in->first_piece + k];
            if ((p->flags & NS_PF_HIDDEN) || !p->mesh_id) continue;
            const NsMesh *m = ns_store_get(st, p->mesh_id);
            if (!m || m->depth_bias >= 0 || !m->verts || !m->faces) continue;
            for (uint32_t f = 0; f < m->n_faces; f++) {
                const NsFace *fc = &m->faces[f];
                float q[4][3]; int nv = (fc->flags & NS_FF_TRI) ? 3 : 4, bad = 0;
                for (int c = 0; c < nv; c++) { if (fc->v[c] >= m->n_verts) { bad = 1; break; } const NsVert *v = &m->verts[fc->v[c]]; xf_pt(&p->world, v->x, v->y, v->z, q[c]); }
                if (bad) continue;
                add_tri(q[0], q[1], nv == 4 ? q[3] : q[2], (float)-m->depth_bias);      /* cuadrilátero A,B,C,D = (A,B,D) y (B,D,C), la diagonal del hardware */
                if (nv == 4) add_tri(q[1], q[3], q[2], (float)-m->depth_bias);
            }
        }
    }
}

/* Altura de la cara t sobre el punto (x, z), o 0 (falso) si cae fuera de su planta. */
static int floor_y(const Tri *t, float x, float z, float *y) {
    float d = (t->b[2] - t->c[2]) * (t->a[0] - t->c[0]) + (t->c[0] - t->b[0]) * (t->a[2] - t->c[2]);
    if (fabsf(d) < 1e-6f) return 0;
    float w0 = ((t->b[2] - t->c[2]) * (x - t->c[0]) + (t->c[0] - t->b[0]) * (z - t->c[2])) / d;
    float w1 = ((t->c[2] - t->a[2]) * (x - t->c[0]) + (t->a[0] - t->c[0]) * (z - t->c[2])) / d, w2 = 1.0f - w0 - w1;
    if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) return 0;
    *y = w0 * t->a[1] + w1 * t->b[1] + w2 * t->c[1];
    return 1;
}

int ns_sink_apply(NsScene *sc, const NsMeshStore *st, NsSinkStats *stats) {
    if (stats) memset(stats, 0, sizeof *stats);
    if (!sc || !st) return 0;
    for (uint32_t i = 0; i < sc->n_piece; i++) sc->piece[i].flags &= (uint16_t)~NS_PF_LIFT_MASK;
    floors(sc, st);
    if (stats) stats->floor_tris = F.n;
    if (!F.n) return 0;
    int lifted = 0;
    const float *eye = sc->cam.eye;
    for (uint32_t i = 0; i < sc->n_inst; i++) {
        const NsInstance *in = &sc->inst[i];
        if ((in->flags & (NS_IF_STATIC | NS_IF_SHADOW | NS_IF_RIGID)) || in->n_pieces < NS_SINK_MIN_PIECES || (uint64_t)in->first_piece + in->n_pieces > sc->n_piece) continue;   /* solo cuerpos articulados */
        for (uint32_t k = 0; k < in->n_pieces; k++) {
            NsPiece *p = &sc->piece[in->first_piece + k];
            if ((p->flags & NS_PF_HIDDEN) || !p->mesh_id) continue;
            const NsMesh *m = ns_store_get(st, p->mesh_id);
            if (!m || !m->verts || m->n_verts == 0 || m->n_verts > 256) continue;
            float w[256][3], lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (uint32_t v = 0; v < m->n_verts; v++) {
                xf_pt(&p->world, m->verts[v].x, m->verts[v].y, m->verts[v].z, w[v]);
                for (int a = 0; a < 3; a++) { if (w[v][a] < lo[a]) lo[a] = w[v][a]; if (w[v][a] > hi[a]) hi[a] = w[v][a]; }
            }
            if (!isfinite(lo[0] + lo[1] + lo[2] + hi[0] + hi[1] + hi[2])) continue;
            if (stats) stats->pieces_tested++;
            float sink = 0.0f, fy = 0.0f, fbias = 0.0f;
            for (uint32_t t = 0; t < F.n; t++) {
                const Tri *T = &F.t[t];
                if (T->x1 < lo[0] || T->x0 > hi[0] || T->z1 < lo[2] || T->z0 > hi[2] || T->y1 <= lo[1] || T->y0 - T->bias > hi[1]) continue;
                for (uint32_t v = 0; v < m->n_verts; v++) {
                    float y;
                    if (w[v][0] < T->x0 || w[v][0] > T->x1 || w[v][2] < T->z0 || w[v][2] > T->z1 || !floor_y(T, w[v][0], w[v][2], &y)) continue;
                    float s = y - w[v][1];
                    if (s > sink && s <= T->bias && s <= NS_SINK_MAX) { sink = s; fy = y; fbias = T->bias; }
                }
            }
            if (!(sink > 0.0f)) continue;
            float c[3] = {0.5f * (lo[0] + hi[0]), 0.5f * (lo[1] + hi[1]), 0.5f * (lo[2] + hi[2])};
            float dy = eye[1] - fy, dist = sqrtf((eye[0] - c[0]) * (eye[0] - c[0]) + (eye[1] - c[1]) * (eye[1] - c[1]) + (eye[2] - c[2]) * (eye[2] - c[2]));
            if (!(dy > 0.0f) || !(dist > NS_SINK_EYE_KEEP)) continue;                    /* visto desde debajo del suelo, o pegado al ojo: nada */
            float sn = dy / dist; if (sn < NS_SINK_MIN_SIN) sn = NS_SINK_MIN_SIN;
            float need = sink / sn + 3.0f * NS_LIFT_UNIT, cap = fminf(fbias, dist - NS_SINK_EYE_KEEP);
            if (need > cap) need = cap;
            int steps = (int)ceilf(need / NS_LIFT_UNIT);
            if (steps <= 0) continue;
            if (steps > 255) steps = 255;
            p->flags = (uint16_t)((p->flags & ~NS_PF_LIFT_MASK) | ((unsigned)steps << NS_PF_LIFT_SHIFT));
            lifted++;
            if (stats) { stats->pieces_lifted++; if (sink > stats->max_sink) stats->max_sink = sink; if ((float)steps * NS_LIFT_UNIT > stats->max_lift) stats->max_lift = (float)steps * NS_LIFT_UNIT; }
        }
    }
    return lifted;
}
