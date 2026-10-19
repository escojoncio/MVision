#include "ns_gpu.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

uint32_t ns_gpu_tri_count(const NsMesh *m) {
    uint32_t n = 0;
    if (!m || !m->faces) return 0;
    for (uint32_t f = 0; f < m->n_faces; f++) n += (m->faces[f].flags & NS_FF_TRI) ? 1u : 2u;
    return n;
}

void ns_gpu_pack(const NsMesh *m, const NsBaked *baked, NsGpuVert *verts, NsGpuCorner *corners, uint32_t vert_base, float *centroid, uint8_t *abr) {
    for (uint32_t i = 0; i < m->n_verts; i++) {
        verts[i].p[0] = (float)m->verts[i].x; verts[i].p[1] = (float)m->verts[i].y; verts[i].p[2] = (float)m->verts[i].z;
        verts[i].weld = m->verts[i].weld;
    }
    const int semi = (m->flags & NS_MF_BLEND) != 0;
    const int paint_mesh = (m->flags & NS_MF_BAKED) != 0 && baked != NULL && baked->rgba != NULL;
    uint32_t tri = 0;
    for (uint32_t f = 0; f < m->n_faces; f++) {
        const NsFace *fc = &m->faces[f];
        NsGpuCorner c4[4];
        const int hastex = fc->texid != 0;
        const uint32_t ab = (uint32_t)((fc->tpage >> 5) & 3u);
        for (int k = 0; k < 4; k++) {
            NsGpuCorner *c = &c4[k];
            memset(c, 0, sizeof *c);
            uint32_t vi = fc->v[k], ni = fc->n[k];   /* 0..255: el extractor ya enmascaró a 7 bits donde el motor lo hace */
            if (vi >= m->n_verts) vi = 0;
            c->v = vert_base + vi;
            if (m->n_norms && ni < m->n_norms && m->norms) {
                c->n[0] = (float)m->norms[ni].x * (1.0f / 4096.0f); c->n[1] = (float)m->norms[ni].y * (1.0f / 4096.0f); c->n[2] = (float)m->norms[ni].z * (1.0f / 4096.0f);
            }
            c->u = fc->u[k]; c->t = fc->t[k];
            c->tpage = fc->tpage; c->clut = fc->clut;
            uint32_t fl = 0;
            if (hastex) fl |= NSG_CF_TEX;
            if (semi) fl |= NSG_CF_SEMI | (ab << 2);
            if (paint_mesh && (fc->flags & NS_FF_BAKED_OK) && 4u * f + (uint32_t)k < baked->n_colors) { fl |= NSG_CF_PAINT; c->paint = baked->rgba[4u * f + (uint32_t)k] & 0xFFFFFFu; }
            c->flags = (uint16_t)fl;
        }
        const int tris = (fc->flags & NS_FF_TRI) ? 1 : 2;
        static const int idx[2][3] = {{0, 1, 2}, {0, 2, 3}};
        for (int t = 0; t < tris; t++, tri++) {
            for (int k = 0; k < 3; k++) corners[3u * tri + (uint32_t)k] = c4[idx[t][k]];
            if (centroid) {
                float s[3] = {0, 0, 0};
                for (int k = 0; k < 3; k++) {
                    const NsGpuVert *v = &verts[c4[idx[t][k]].v - vert_base];
                    s[0] += v->p[0]; s[1] += v->p[1]; s[2] += v->p[2];
                }
                centroid[3u * tri] = s[0] * (1.0f / 3.0f); centroid[3u * tri + 1] = s[1] * (1.0f / 3.0f); centroid[3u * tri + 2] = s[2] * (1.0f / 3.0f);
            }
            if (abr) abr[tri] = (uint8_t)ab;
        }
    }
}

float ns_gpu_game_z(const NsCamera *cam, const float p[3]) {
    /* rot: vista -> mundo, fila-mayor; la tercera columna (rot[2], rot[5], rot[8]) es el eje Z de la vista (adelante) en el mundo */
    return cam->rot[2] * (p[0] - cam->eye[0]) + cam->rot[5] * (p[1] - cam->eye[1]) + cam->rot[8] * (p[2] - cam->eye[2]);
}

float ns_gpu_fog_f(const NsFog *fog, float z) {
    if (!fog || !fog->active || !(fog->len > 0.0f)) return 0.0f;
    return ns_clampf((z - fog->start) / fog->len, 0.0f, 1.0f);
}

void ns_gpu_make_piece(const NsScene *sc, const NsInstance *inst, const NsPiece *p, uint32_t mesh_flags, int depth_bias, uint32_t vert_base,
                       int parent_global, uint32_t parent_nverts, NsGpuPiece *o) {
    memset(o, 0, sizeof *o);
    o->fog = 1.0f;
    o->parent = -1;
    memcpy(o->w, p->world.m, sizeof o->w);
    o->vert_base = vert_base;
    o->flags = NSG_PF_DRAW;
    o->raise = (float)depth_bias;
    if (parent_global >= 0) { o->parent = parent_global; o->parent_nverts = parent_nverts; }

    /* luz: la de la instancia (o la global); ambiente propio solo con NS_IF_OWN_AMBIENT */
    const NsLight *L = (inst->light >= 0 && (uint32_t)inst->light < sc->n_light) ? &sc->lights[inst->light] : &sc->global_light;
    const int16_t *amb = (inst->flags & NS_IF_OWN_AMBIENT) ? L->amb : sc->global_light.amb;
    if ((inst->flags & NS_IF_LIT) && !(mesh_flags & NS_MF_UNLIT)) {
        o->flags |= NSG_PF_SHADE;
        /* normal de mundo = R n  =>  dot(dir, R n) = dot(R^T dir, n): se rota la luz al espacio local una vez por pieza */
        for (int l = 0; l < 3; l++)
            for (int j = 0; j < 3; j++) {
                float s = 0.0f;
                for (int i = 0; i < 3; i++) s += (float)L->dir[l][i] * p->world.m[4 * i + j];
                o->l[3 * l + j] = s * (1.0f / 4096.0f);
            }
        for (int c = 0; c < 3; c++) {
            for (int l = 0; l < 3; l++) o->k[4 * c + l] = (float)L->col[c][l] * (1.0f / 16.0f);
            o->k[4 * c + 3] = (float)amb[c];
        }
    }
    if (sc->fog.active && sc->fog.len > 0.0f) {
        if (inst->flags & NS_IF_STATIC) o->flags |= NSG_PF_VFOG;
        else {
            /* objeto: a la profundidad de la pieza raíz (la 0 de la instancia) */
            const NsPiece *root = &sc->piece[inst->first_piece];
            float rp[3] = {root->world.m[3], root->world.m[7], root->world.m[11]};
            o->fog = 1.0f - ns_gpu_fog_f(&sc->fog, ns_gpu_game_z(&sc->cam, rp));
        }
    }
}

typedef struct { uint64_t key; uint32_t piece, tri; uint8_t abr; } SortItem;
static int sort_cmp(const void *a, const void *b) {
    const SortItem *x = (const SortItem *)a, *y = (const SortItem *)b;
    return x->key < y->key ? -1 : x->key > y->key;
}
/* float -> entero sin signo con el mismo orden (los negativos antes que los positivos) */
static uint32_t ordered_bits(float f) {
    uint32_t b; memcpy(&b, &f, 4);
    return (b & 0x80000000u) ? ~b : (b | 0x80000000u);
}

size_t ns_gpu_sort_semi(const NsGpuSemiSrc *src, size_t nsrc, NsGpuOrder *out, uint8_t *abr_out, size_t cap) {
    size_t total = 0;
    for (size_t s = 0; s < nsrc; s++) total += src[s].ntri;
    if (total > cap) total = cap;
    if (!total) return 0;
    SortItem *it = (SortItem *)malloc(total * sizeof *it);
    if (!it) return 0;
    size_t n = 0;
    uint32_t seq = 0;
    for (size_t s = 0; s < nsrc && n < total; s++) {
        const NsGpuSemiSrc *g = &src[s];
        for (uint32_t t = 0; t < g->ntri && n < total; t++, seq++) {
            const float *c = &g->centroid[3u * t];
            float depth = g->zrow[0] * c[0] + g->zrow[1] * c[1] + g->zrow[2] * c[2] + g->zrow[3];
            if (!(depth == depth)) depth = 0.0f;                     /* NaN */
            it[n].key = ((uint64_t)(~ordered_bits(depth)) << 32) | seq;   /* mayor profundidad primero; empate: orden de entrada */
            it[n].piece = g->piece; it[n].tri = g->tri_base + t; it[n].abr = g->abr[t];
            n++;
        }
    }
    qsort(it, n, sizeof *it, sort_cmp);
    for (size_t i = 0; i < n; i++) { out[i].piece = it[i].piece; out[i].tri = it[i].tri; abr_out[i] = it[i].abr; }
    free(it);
    return n;
}
