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

/* ------------------------------------------------------------------------------------------------ diagnóstico: partir triángulos largos */
enum { NS_SPLIT_DEPTH = 24 };
typedef struct { NsGpuCorner c; float p[3]; } SPt;
typedef struct { SPt p[3]; int depth; } STri;
typedef struct {
    NsGpuVert *v; uint32_t nv, capv;
    NsGpuCorner *c; uint32_t nt, capt;
    float *cen; uint8_t *abr; int want_cen;
} SOut;

static float sp_d2(const float *a, const float *b) {
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}

static float sp_longest(const STri *t, int *edge) {
    float e0 = sp_d2(t->p[0].p, t->p[1].p), e1 = sp_d2(t->p[1].p, t->p[2].p), e2 = sp_d2(t->p[2].p, t->p[0].p);
    float m = e0; int k = 0;
    if (e1 > m) { m = e1; k = 1; }
    if (e2 > m) { m = e2; k = 2; }
    *edge = k;
    return m;
}

static int sp_grow_t(SOut *o) {
    if (o->nt < o->capt) return 1;
    uint32_t nc = o->capt ? o->capt * 2 : 1024;
    NsGpuCorner *c = (NsGpuCorner *)realloc(o->c, (size_t)nc * 3 * sizeof(NsGpuCorner));
    if (!c) return 0;
    o->c = c;
    if (o->want_cen) {
        float *cen = (float *)realloc(o->cen, (size_t)nc * 3 * sizeof(float));
        if (!cen) return 0;
        o->cen = cen;
    }
    uint8_t *a = (uint8_t *)realloc(o->abr, nc);     /* abr siempre (barato); se descarta si la entrada no lo traía */
    if (!a) return 0;
    o->abr = a;
    o->capt = nc;
    return 1;
}

static int sp_push_v(SOut *o, const float p[3], uint32_t *idx) {
    if (o->nv >= o->capv) {
        uint32_t nc = o->capv * 2 + 16;
        NsGpuVert *v = (NsGpuVert *)realloc(o->v, (size_t)nc * sizeof(NsGpuVert));
        if (!v) return 0;
        o->v = v; o->capv = nc;
    }
    o->v[o->nv].p[0] = p[0]; o->v[o->nv].p[1] = p[1]; o->v[o->nv].p[2] = p[2]; o->v[o->nv].weld = NSG_NO_WELD;
    *idx = o->nv++;
    return 1;
}

static int sp_emit(SOut *o, const STri *t, uint8_t abr) {
    if (!sp_grow_t(o)) return 0;
    for (int k = 0; k < 3; k++) o->c[3u * o->nt + (uint32_t)k] = t->p[k].c;
    if (o->want_cen) for (int i = 0; i < 3; i++) o->cen[3u * o->nt + (uint32_t)i] = (t->p[0].p[i] + t->p[1].p[i] + t->p[2].p[i]) * (1.0f / 3.0f);
    o->abr[o->nt] = abr;
    o->nt++;
    return 1;
}

static uint8_t sp_lerp8(uint32_t a, uint32_t b) { return (uint8_t)((a + b + 1u) >> 1); }

static int sp_mid(SOut *o, const SPt *a, const SPt *b, SPt *m) {
    *m = *a;
    for (int i = 0; i < 3; i++) m->p[i] = 0.5f * (a->p[i] + b->p[i]);
    float n[3], l = 0.0f;
    for (int i = 0; i < 3; i++) { n[i] = 0.5f * (a->c.n[i] + b->c.n[i]); l += n[i] * n[i]; }
    if (l > 1e-12f) { l = 1.0f / sqrtf(l); for (int i = 0; i < 3; i++) m->c.n[i] = n[i] * l; }
    m->c.u = sp_lerp8(a->c.u, b->c.u); m->c.t = sp_lerp8(a->c.t, b->c.t);
    if ((a->c.flags & NSG_CF_PAINT) && (b->c.flags & NSG_CF_PAINT)) {
        uint32_t r = sp_lerp8(a->c.paint & 255u, b->c.paint & 255u), g = sp_lerp8((a->c.paint >> 8) & 255u, (b->c.paint >> 8) & 255u),
                 bl = sp_lerp8((a->c.paint >> 16) & 255u, (b->c.paint >> 16) & 255u);
        m->c.paint = r | (g << 8) | (bl << 16);
    }
    uint32_t idx;
    if (!sp_push_v(o, m->p, &idx)) return 0;
    m->c.v = idx;
    return 1;
}

/* Un triángulo -> hojas (DFS: el orden de salida sigue a la geometría). allowed = máximo de triángulos que puede producir. */
static int sp_one(SOut *o, const STri *src, float max_e2, uint32_t allowed, uint8_t abr, int *capped, uint32_t *leaves_out) {
    STri st[NS_SPLIT_DEPTH * 2 + 4];
    int sp = 1;
    st[0] = *src;
    uint32_t leaves = 0;
    while (sp) {
        STri cur = st[--sp];
        int e;
        float l2 = sp_longest(&cur, &e);
        if (l2 <= max_e2) { if (!sp_emit(o, &cur, abr)) return 0; leaves++; continue; }
        if (cur.depth >= NS_SPLIT_DEPTH || leaves + (uint32_t)sp + 2u > allowed || sp + 2 > (int)(sizeof st / sizeof st[0])) {
            *capped = 1;
            if (!sp_emit(o, &cur, abr)) return 0;
            leaves++;
            continue;
        }
        const SPt *a = &cur.p[e], *b = &cur.p[(e + 1) % 3], *c = &cur.p[(e + 2) % 3];
        SPt m;
        if (!sp_mid(o, a, b, &m)) return 0;
        STri t1, t2;
        t1.p[0] = *a; t1.p[1] = m; t1.p[2] = *c; t1.depth = cur.depth + 1;
        t2.p[0] = m; t2.p[1] = *b; t2.p[2] = *c; t2.depth = cur.depth + 1;
        st[sp++] = t2;
        st[sp++] = t1;
    }
    *leaves_out = leaves;
    return 1;
}

void ns_gpu_split_free(NsGpuSplit *s) {
    if (!s) return;
    free(s->verts); free(s->corners); free(s->centroid); free(s->abr);
    memset(s, 0, sizeof *s);
}

int ns_gpu_split(const NsGpuVert *verts, uint32_t nv, const NsGpuCorner *corners, uint32_t nt, const float *centroid, const uint8_t *abr,
                 float max_edge, uint32_t max_out_tris, NsGpuSplit *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!verts || !corners || !nv || !nt || !(max_edge > 0.0f) || !isfinite(max_edge) || !max_out_tris) return -1;
    const float max_e2 = max_edge * max_edge;
    /* ¿hay alguno largo y partible? */
    uint32_t nlong = 0;
    for (uint32_t t = 0; t < nt; t++) {
        STri tt;
        int weld = 0, ok = 1;
        for (int k = 0; k < 3; k++) {
            uint32_t vi = corners[3u * t + (uint32_t)k].v;
            if (vi >= nv) { ok = 0; break; }
            tt.p[k].p[0] = verts[vi].p[0]; tt.p[k].p[1] = verts[vi].p[1]; tt.p[k].p[2] = verts[vi].p[2];
            if (verts[vi].weld != NSG_NO_WELD) weld = 1;
            if (!isfinite(tt.p[k].p[0]) || !isfinite(tt.p[k].p[1]) || !isfinite(tt.p[k].p[2])) ok = 0;
        }
        int e;
        if (ok && sp_longest(&tt, &e) > max_e2) { if (weld) out->n_skip_weld++; else nlong++; }
    }
    if (!nlong) { uint32_t w = out->n_skip_weld; memset(out, 0, sizeof *out); out->n_skip_weld = w; return 1; }
    SOut o;
    memset(&o, 0, sizeof o);
    o.want_cen = centroid != NULL;
    o.v = (NsGpuVert *)malloc((size_t)(nv + 64) * sizeof(NsGpuVert));
    if (!o.v) return -1;
    memcpy(o.v, verts, (size_t)nv * sizeof(NsGpuVert));
    o.nv = nv; o.capv = nv + 64;
    uint32_t remaining = nt;
    int fail = 0;
    uint32_t skipw = out->n_skip_weld;
    for (uint32_t t = 0; t < nt && !fail; t++) {
        remaining--;
        STri tt;
        int weld = 0, ok = 1;
        for (int k = 0; k < 3; k++) {
            const NsGpuCorner *c = &corners[3u * t + (uint32_t)k];
            tt.p[k].c = *c;
            uint32_t vi = c->v;
            if (vi >= nv) { ok = 0; vi = 0; }
            tt.p[k].p[0] = verts[vi].p[0]; tt.p[k].p[1] = verts[vi].p[1]; tt.p[k].p[2] = verts[vi].p[2];
            if (verts[vi].weld != NSG_NO_WELD) weld = 1;
            if (!isfinite(tt.p[k].p[0]) || !isfinite(tt.p[k].p[1]) || !isfinite(tt.p[k].p[2])) ok = 0;
        }
        tt.depth = 0;
        uint8_t ab = abr ? abr[t] : 0;
        int e;
        int big = ok && !weld && sp_longest(&tt, &e) > max_e2;
        if (!big) {
            /* copia tal cual (incluye los soldados y los no finitos) con su centroide original */
            if (!sp_grow_t(&o)) { fail = 1; break; }
            for (int k = 0; k < 3; k++) o.c[3u * o.nt + (uint32_t)k] = corners[3u * t + (uint32_t)k];
            if (o.want_cen) for (int i = 0; i < 3; i++) o.cen[3u * o.nt + (uint32_t)i] = centroid[3u * t + (uint32_t)i];
            o.abr[o.nt] = ab;
            o.nt++;
            continue;
        }
        uint32_t used = o.nt + remaining;
        uint32_t allowed = max_out_tris > used ? max_out_tris - used : 1u;
        int capped = 0;
        uint32_t leaves = 0;
        if (!sp_one(&o, &tt, max_e2, allowed, ab, &capped, &leaves)) { fail = 1; break; }
        if (leaves > 1) out->n_split++;
        if (capped) out->n_skip_cap++;
    }
    if (fail) { free(o.v); free(o.c); free(o.cen); free(o.abr); uint32_t w = skipw; memset(out, 0, sizeof *out); out->n_skip_weld = w; return -1; }
    out->verts = o.v; out->nverts = o.nv; out->corners = o.c; out->ntris = o.nt; out->centroid = o.cen;
    if (abr) out->abr = o.abr; else free(o.abr);
    return 0;
}

void ns_gpu_rebase_corners(NsGpuCorner *c, uint32_t n, uint32_t base) {
    for (uint32_t i = 0; i < n; i++) c[i].v += base;
}

void ns_gpu_scan(const NsGpuVert *verts, const NsGpuCorner *corners, uint32_t ntri, const float m[16], float near, float big_edge, NsGpuScan *acc) {
    if (!verts || !corners || !m || !acc) return;
    const float big2 = big_edge * big_edge;
    for (uint32_t t = 0; t < ntri; t++) {
        float x[3][3], d[3];
        int ok = 1;
        for (int k = 0; k < 3; k++) {
            const NsGpuVert *v = &verts[corners[3u * t + (uint32_t)k].v];
            const float *p = v->p;
            if (!(isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]))) { ok = 0; break; }
            for (int i = 0; i < 3; i++) x[k][i] = m[i] * p[0] + m[4 + i] * p[1] + m[8 + i] * p[2] + m[12 + i];
            d[k] = -x[k][2];
        }
        if (!ok) continue;
        acc->tris++;
        float e0 = sp_d2(x[0], x[1]), e1 = sp_d2(x[1], x[2]), e2 = sp_d2(x[2], x[0]);
        float mx = e0 > e1 ? e0 : e1; if (e2 > mx) mx = e2;
        if (sqrtf(mx) > acc->max_edge) acc->max_edge = sqrtf(mx);
        int big = mx > big2;
        float dmin = d[0] < d[1] ? d[0] : d[1]; if (d[2] < dmin) dmin = d[2];
        float dmax = d[0] > d[1] ? d[0] : d[1]; if (d[2] > dmax) dmax = d[2];
        int cross = dmin < near && dmax >= near;
        if (big) acc->big++;
        if (cross) acc->cross++;
        if (big && cross) acc->big_cross++;
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

typedef struct { uint32_t key, piece, tri, abr; } SortItem;   /* 16 B */
/* float -> entero sin signo con el mismo orden (los negativos antes que los positivos) */
static uint32_t ordered_bits(float f) {
    uint32_t b; memcpy(&b, &f, 4);
    return (b & 0x80000000u) ? ~b : (b | 0x80000000u);
}

void ns_gpu_sort_free(NsGpuSortScratch *sc) {
    if (!sc) return;
    free(sc->a); free(sc->b);
    sc->a = sc->b = NULL; sc->cap = 0;
}

/* Ordenación por radix LSD (3 pasadas de 11 bits) sobre la clave de 32 bits = ~bits_ordenados(profundidad): estable, así que el empate de profundidad conserva el
 * orden de entrada (el criterio de la referencia). Sin malloc por fotograma: el scratch crece y se reutiliza. */
size_t ns_gpu_sort_semi(const NsGpuSemiSrc *src, size_t nsrc, NsGpuOrder *out, uint8_t *abr_out, size_t cap, NsGpuSortScratch *scratch) {
    size_t total = 0;
    for (size_t s = 0; s < nsrc; s++) total += src[s].ntri;
    if (!total || !cap) return 0;
    NsGpuSortScratch local = {0, 0, 0};
    NsGpuSortScratch *sc = scratch ? scratch : &local;
    if (sc->cap < total) {
        size_t nc = total + total / 4 + 1024;
        SortItem *na = (SortItem *)realloc(sc->a, nc * sizeof(SortItem));
        if (na) sc->a = na;
        SortItem *nb = (SortItem *)realloc(sc->b, nc * sizeof(SortItem));
        if (nb) sc->b = nb;
        if (!na || !nb) { if (!scratch) ns_gpu_sort_free(&local); return 0; }
        sc->cap = nc;
    }
    SortItem *a = (SortItem *)sc->a, *b = (SortItem *)sc->b;
    size_t n = 0;
    for (size_t s = 0; s < nsrc; s++) {
        const NsGpuSemiSrc *g = &src[s];
        for (uint32_t t = 0; t < g->ntri; t++) {
            const float *c = &g->centroid[3u * t];
            float depth = g->zrow[0] * c[0] + g->zrow[1] * c[1] + g->zrow[2] * c[2] + g->zrow[3];
            if (!(depth == depth)) depth = 0.0f;                     /* NaN */
            a[n].key = ~ordered_bits(depth);                          /* mayor profundidad primero */
            a[n].piece = g->piece; a[n].tri = g->tri_base + t; a[n].abr = g->abr[t];
            n++;
        }
    }
    enum { RB = 11, RN = 1 << RB };
    uint32_t hist[3][RN];
    memset(hist, 0, sizeof hist);
    for (size_t i = 0; i < n; i++) {
        uint32_t k = a[i].key;
        hist[0][k & (RN - 1)]++; hist[1][(k >> RB) & (RN - 1)]++; hist[2][k >> (2 * RB)]++;
    }
    for (int p = 0; p < 3; p++) {
        uint32_t sum = 0;
        for (int i = 0; i < RN; i++) { uint32_t c = hist[p][i]; hist[p][i] = sum; sum += c; }
        for (size_t i = 0; i < n; i++) {
            uint32_t k = a[i].key, d = p == 0 ? (k & (RN - 1)) : p == 1 ? ((k >> RB) & (RN - 1)) : (k >> (2 * RB));
            b[hist[p][d]++] = a[i];
        }
        SortItem *t = a; a = b; b = t;
    }
    /* a = ordenado de atrás adelante. Con tope se descartan los MÁS LEJANOS (el principio de la lista). */
    size_t first = n > cap ? n - cap : 0, m = n - first;
    for (size_t i = 0; i < m; i++) { out[i].piece = a[first + i].piece; out[i].tri = a[first + i].tri; abr_out[i] = (uint8_t)a[first + i].abr; }
    if (!scratch) ns_gpu_sort_free(&local);
    return m;
}
