#include "ns_gpu.h"
#include <math.h>
#include <stdio.h>
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
            if (!(m->flags & NS_MF_TWOSIDED)) fl |= NSG_CF_ONESIDED;
            if (semi) fl |= NSG_CF_SEMI | (ab << 2);
            if (paint_mesh && (fc->flags & NS_FF_BAKED_OK) && 4u * f + (uint32_t)k < baked->n_colors) { fl |= NSG_CF_PAINT; c->paint = baked->rgba[4u * f + (uint32_t)k] & 0xFFFFFFu; }
            c->flags = (uint16_t)fl;
        }
        const int tris = (fc->flags & NS_FF_TRI) ? 1 : 2;
        /* diagonal B-D como el hardware (v0,v1,v2,v3 = A,B,D,C -> (A,B,D) y (B,D,C)); (B,C,D) conserva el sentido del perímetro */
        static const int idxq[2][3] = {{0, 1, 3}, {1, 2, 3}};
        static const int idxt[1][3] = {{0, 1, 2}};   /* triángulo (C == D): (A,B,C), como ns_adapt */
        const int (*idx)[3] = (fc->flags & NS_FF_TRI) ? idxt : idxq;
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
    if (!(inst->flags & NS_IF_STATIC)) o->flags |= NSG_PF_NET;   /* objetos dinámicos: red de seguridad de vértices (el escenario es fijo y trae triángulos legítimamente enormes: agua, cielo) */

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

/* ------------------------------------------------------------------------------------------------ red de seguridad de vértices (réplica en C de ns_vertex / ns_net_* de NativeShaders.metal) */
static void net_mul34(const float *m, const float p[3], float o[3]) {
    o[0] = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
    o[1] = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
    o[2] = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
}

float ns_gpu_mesh_radius(const NsMesh *m) {
    if (!m || !m->verts) return 0.0f;
    float r2 = 0.0f;
    for (uint32_t i = 0; i < m->n_verts; i++) {
        float x = (float)m->verts[i].x, y = (float)m->verts[i].y, z = (float)m->verts[i].z, d = x * x + y * y + z * z;
        if (d > r2) r2 = d;
    }
    return sqrtf(r2);
}

void ns_gpu_vert_world(const NsGpuVert *pool, const NsGpuPiece *tbl, uint32_t gi, uint32_t vabs, float out[3]) {
    const NsGpuPiece *P = &tbl[gi];
    const NsGpuVert *V = &pool[vabs];
    net_mul34(P->w, V->p, out);
    if (P->flags & NSG_PF_NOWELD) return;
    uint32_t w = V->weld;
    int pi = P->parent;
    uint32_t pn = P->parent_nverts;
    for (int d = 0; d < NSG_WELD_DEPTH; d++) {
        if (w == NSG_NO_WELD || pi < 0 || w >= pn) break;
        const NsGpuPiece *Q = &tbl[pi];
        const NsGpuVert *PV = &pool[Q->vert_base + w];
        net_mul34(Q->w, PV->p, out);
        if (Q->flags & NSG_PF_NOWELD) break;
        w = PV->weld; pi = Q->parent; pn = Q->parent_nverts;
    }
}

uint32_t ns_gpu_net_piece(const NsGpuVert *pool, const NsGpuPiece *tbl, uint32_t gi, uint32_t nverts, float mpu, NsGpuNetPiece *out) {
    NsGpuNetPiece local;
    NsGpuNetPiece *o = out ? out : &local;
    memset(o, 0, sizeof *o);
    const NsGpuPiece *P = &tbl[gi];
    uint32_t causes = 0;
    float mx = 0.0f;
    for (int c = 0; c < 3; c++) {
        float n2 = P->w[c] * P->w[c] + P->w[4 + c] * P->w[4 + c] + P->w[8 + c] * P->w[8 + c];
        if (!(n2 <= 1e30f)) n2 = 1e30f;   /* NaN/inf -> enorme */
        float n = sqrtf(n2);
        if (n > mx) mx = n;
    }
    for (int i = 0; i < 12; i++) if (!isfinite(P->w[i])) mx = 1e30f;
    if (fabsf(P->w[3]) > 1.6e7f || fabsf(P->w[7]) > 1.6e7f || fabsf(P->w[11]) > 1.6e7f) mx = 1e30f;
    o->max_scale = mx;
    if (mx > NSG_NET_MAT_SCALE) causes |= NSG_NET_MAT;
    /* soldaduras: el vértice soldado debe caer cerca de donde lo pone la propia malla (en pose coherente, <= ~0,4 m en los datos reales) */
    if (!(P->flags & NSG_PF_NOWELD) && P->parent >= 0) {
        const float gap_u = NSG_NET_WELD_GAP_M / (mpu > 0.0f ? mpu : 1.0f);
        for (uint32_t i = 0; i < nverts; i++) {
            const NsGpuVert *V = &pool[P->vert_base + i];
            if (V->weld == NSG_NO_WELD) continue;
            o->n_welded++;
            float own[3], wp[3];
            net_mul34(P->w, V->p, own);
            ns_gpu_vert_world(pool, tbl, gi, P->vert_base + i, wp);
            float dx = wp[0] - own[0], dy = wp[1] - own[1], dz = wp[2] - own[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (!(d <= gap_u)) { o->n_gap++; causes |= NSG_NET_WELD; }
            if (isfinite(d) && d * mpu > o->max_gap_m) o->max_gap_m = d * mpu;
            else if (!isfinite(d)) o->max_gap_m = 1e30f;
        }
    }
    o->causes = causes;
    return causes;
}

/* Decisión por triángulo (la misma que el shader). Devuelve NSG_NET_FAR / NSG_NET_EDGE (o 0) y el peor valor en metros (distancia al origen de la pieza / exceso de arista). */
static uint32_t net_tri(const NsGpuVert *pool, const NsGpuPiece *tbl, uint32_t gi, const uint32_t v[3], float mpu, float *far_m, float *edge_m) {
    const NsGpuPiece *P = &tbl[gi];
    float wp[3][3];
    for (int k = 0; k < 3; k++) ns_gpu_vert_world(pool, tbl, gi, v[k], wp[k]);
    const float org[3] = {P->w[3], P->w[7], P->w[11]};
    const float slack_u = NSG_NET_SLACK_M / (mpu > 0.0f ? mpu : 1.0f);
    uint32_t bad = 0;
    for (int k = 0; k < 3; k++) {
        float dx = wp[k][0] - org[0], dy = wp[k][1] - org[1], dz = wp[k][2] - org[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (P->rest_r > 0.0f && !(d <= NSG_NET_FAR_RATIO * P->rest_r + slack_u)) bad |= NSG_NET_FAR;
        if (!isfinite(d)) d = 1e30f;
        if (d * mpu > *far_m) *far_m = d * mpu;
        int k2 = (k + 1) % 3;
        const float *a = pool[v[k]].p, *b = pool[v[k2]].p;
        float lx = a[0] - b[0], ly = a[1] - b[1], lz = a[2] - b[2], l = sqrtf(lx * lx + ly * ly + lz * lz);
        float ex = wp[k][0] - wp[k2][0], ey = wp[k][1] - wp[k2][1], ez = wp[k][2] - wp[k2][2], e = sqrtf(ex * ex + ey * ey + ez * ez);
        if (!(e <= NSG_NET_EDGE_RATIO * l + slack_u)) bad |= NSG_NET_EDGE;
        if (!isfinite(e)) e = 1e30f;
        float ex_m = (e - NSG_NET_EDGE_RATIO * l) * mpu;
        if (ex_m > *edge_m) *edge_m = ex_m;
    }
    return bad;
}

uint32_t ns_gpu_net_tris(const NsGpuVert *pool, const NsGpuCorner *corners, uint32_t ntri, const NsGpuPiece *tbl, uint32_t gi, float mpu, NsGpuNetTris *acc) {
    if (!(tbl[gi].flags & NSG_PF_NET)) return 0;
    uint32_t n = 0;
    for (uint32_t t = 0; t < ntri; t++) {
        const uint32_t v[3] = {corners[3u * t].v, corners[3u * t + 1].v, corners[3u * t + 2].v};
        float far_m = 0.0f, edge_m = 0.0f;
        uint32_t b = net_tri(pool, tbl, gi, v, mpu, &far_m, &edge_m);
        if (acc) {
            acc->tris++;
            if (far_m > acc->worst_far_m) acc->worst_far_m = far_m;
            if (edge_m > acc->worst_edge_m) acc->worst_edge_m = edge_m;
            if (b & NSG_NET_FAR) acc->bad_far++;
            if (b & NSG_NET_EDGE) acc->bad_edge++;
        }
        if (b) n++;
    }
    return n;
}

typedef struct { uint32_t key, piece, tri, abr; float depth; } SortItem;   /* 20 B */
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
    return ns_gpu_sort_semi_ex(src, nsrc, out, abr_out, NULL, cap, scratch);
}
size_t ns_gpu_sort_semi_ex(const NsGpuSemiSrc *src, size_t nsrc, NsGpuOrder *out, uint8_t *abr_out, float *depth_out, size_t cap, NsGpuSortScratch *scratch) {
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
            a[n].piece = g->piece; a[n].tri = g->tri_base + t; a[n].abr = g->abr[t]; a[n].depth = depth;
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
    for (size_t i = 0; i < m; i++) { out[i].piece = a[first + i].piece; out[i].tri = a[first + i].tri; abr_out[i] = (uint8_t)a[first + i].abr; if (depth_out) depth_out[i] = a[first + i].depth; }
    if (!scratch) ns_gpu_sort_free(&local);
    return m;
}

/* ------------------------------------------------------------------------------------------------------------ efectos nativos */
typedef struct { float depth; uint32_t first, count; uint8_t abr; uint32_t order; } FxSemi;
static int fx_semi_cmp(const void *a, const void *b) {
    const FxSemi *x = (const FxSemi *)a, *y = (const FxSemi *)b;
    if (x->depth != y->depth) return x->depth > y->depth ? -1 : 1;   /* más lejos primero */
    return x->order < y->order ? -1 : x->order > y->order;
}
static int fx_finite3(const float *p) { return isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]); }
static void fx_vr(const float *m, const float *w, float *o) {
    for (int r = 0; r < 3; r++) o[r] = m[4 * r] * w[0] + m[4 * r + 1] * w[1] + m[4 * r + 2] * w[2] + m[4 * r + 3];
}
/* inversa de la parte afín 3x4; 0 si es singular */
static int fx_inv34(const float *m, float *o) {
    double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!(fabs(det) > 1e-30)) return 0;
    double iv = 1.0 / det;
    double r[9] = {(e * i - f * h) * iv, (c * h - b * i) * iv, (b * f - c * e) * iv,
                   (f * g - d * i) * iv, (a * i - c * g) * iv, (c * d - a * f) * iv,
                   (d * h - e * g) * iv, (b * g - a * h) * iv, (a * e - b * d) * iv};
    for (int k = 0; k < 9; k++) o[4 * (k / 3) + (k % 3)] = (float)r[k];
    for (int k = 0; k < 3; k++) o[4 * k + 3] = (float)-(r[3 * k] * m[3] + r[3 * k + 1] * m[7] + r[3 * k + 2] * m[11]);
    return 1;
}
static float fx_len_scale(const float *m) {   /* metros por unidad del motor: longitud de la imagen de un eje */
    double s = 0;
    for (int c = 0; c < 3; c++) s += sqrt((double)m[c] * m[c] + (double)m[4 + c] * m[4 + c] + (double)m[8 + c] * m[8 + c]);
    return (float)(s / 3.0);
}


/* ---- Comodidad VR de los efectos cerca de la cabeza ---- */
/* Papeles (mismos valores que NSFX_ROLE_*; test_ns_gpu_fx lo comprueba). */
enum { FXR_SURFACE = 14, FXR_WATER_SURFACE = 15, FXR_VIEW_OVERLAY = 22, FXR_COUNT = 23 };
static const char *const k_fx_role_names[FXR_COUNT] = {"none", "light", "bubble", "smoke", "spark", "droplet", "splash", "ripple", "tracer", "beam", "decal", "shadow", "shadow_obj", "plasma",
                                                       "surface", "water_surface", "ambient", "cone", "trail", "strip", "particle", "sprite_anim", "view_overlay"};
const char *ns_gpu_fx_role_name(int role) { return role >= 0 && role < FXR_COUNT ? k_fx_role_names[role] : "?"; }
float ns_gpu_fx_comfort_cap_deg(float d, float x, float dmin) {
    if (!(x > 0.0f)) return 1e9f;
    if (!(dmin > 0.0f)) dmin = 0.12f;
    if (!(d > dmin)) return 0.0f;
    if (d < 0.3f) { float t = (d - dmin) / (0.3f - dmin); t = t * t * (3.0f - 2.0f * t); return x * t; }
    if (d < 1.0f) return x + (90.0f - x) * (d - 0.3f) / 0.7f;
    return 1e9f;
}
/* Diámetro angular (grados) del polígono visto desde la cabeza: mayor ángulo entre dos esquinas. Una esquina en la cabeza cuenta como 180. */
static float fx_ang_deg(float q[4][3], int nq, const float *head) {
    float u[4][3];
    for (int v = 0; v < nq; v++) {
        float dx = q[v][0] - head[0], dy = q[v][1] - head[1], dz = q[v][2] - head[2], l = sqrtf(dx * dx + dy * dy + dz * dz);
        if (!(l > 1e-4f)) return 180.0f;
        u[v][0] = dx / l; u[v][1] = dy / l; u[v][2] = dz / l;
    }
    float best = 0.0f;
    for (int a = 0; a < nq; a++) for (int b = a + 1; b < nq; b++) {
        float c = u[a][0] * u[b][0] + u[a][1] * u[b][1] + u[a][2] * u[b][2];
        if (c > 1.0f) { c = 1.0f; } else if (c < -1.0f) { c = -1.0f; }
        float g = acosf(c) * (180.0f / 3.14159265f);
        if (g > best) best = g;
    }
    return best;
}
static float fx_size_m(float q[4][3], int nq) {
    float best = 0.0f;
    for (int a = 0; a < nq; a++) for (int b = a + 1; b < nq; b++) {
        float dx = q[a][0] - q[b][0], dy = q[a][1] - q[b][1], dz = q[a][2] - q[b][2], l = sqrtf(dx * dx + dy * dy + dz * dz);
        if (l > best) best = l;
    }
    return best;
}
static void fx_rec_insert(NsGpuFxNear *rec, uint32_t *n, const NsGpuFxNear *r) {
    uint32_t pos = *n;
    for (uint32_t i = 0; i < *n; i++) if (rec[i].src_key == r->src_key && rec[i].idx == r->idx) {   /* mismo origen: se queda el mayor */
        if (r->deg_raw <= rec[i].deg_raw) return;
        for (uint32_t k = i; k + 1 < *n; k++) rec[k] = rec[k + 1];
        (*n)--; break;
    }
    pos = *n;
    if (pos >= NSG_FXN_MAX) { if (r->deg_raw <= rec[NSG_FXN_MAX - 1].deg_raw) return; pos = NSG_FXN_MAX - 1; } else (*n)++;
    rec[pos] = *r;
    while (pos > 0 && rec[pos].deg_raw > rec[pos - 1].deg_raw) { NsGpuFxNear t = rec[pos]; rec[pos] = rec[pos - 1]; rec[pos - 1] = t; pos--; }
}
void ns_gpu_fx_near_reset(NsGpuFxNearAcc *a) { if (a) memset(a, 0, sizeof *a); }
void ns_gpu_fx_near_accum(NsGpuFxNearAcc *a, const NsGpuFxOut *o, const float head[3], const float right[3], float t) {
    if (!a || !o) return;
    int moving = 0;
    if (head && right && a->has_prev && t > a->prev_t + 1e-4f) {
        float dt = t - a->prev_t, dx = head[0] - a->prev_head[0], dy = head[1] - a->prev_head[1], dz = head[2] - a->prev_head[2];
        float v = sqrtf(dx * dx + dy * dy + dz * dz) / dt;
        float c = right[0] * a->prev_right[0] + right[1] * a->prev_right[1] + right[2] * a->prev_right[2];
        if (c > 1.0f) { c = 1.0f; } else if (c < -1.0f) { c = -1.0f; }
        float w = acosf(c) * (180.0f / 3.14159265f) / dt;
        if (isfinite(v) && v > a->max_speed_mps) a->max_speed_mps = v;
        if (isfinite(w) && w > a->max_turn_dps) a->max_turn_dps = w;
        moving = (isfinite(v) && v > 0.3f) || (isfinite(w) && w > 30.0f);
    }
    if (head && right) { for (int i = 0; i < 3; i++) { a->prev_head[i] = head[i]; a->prev_right[i] = right[i]; } a->prev_t = t; a->has_prev = 1; }
    a->frames++;
    if (moving) a->frames_moving++;
    if (o->n_near) { a->frames_near++; if (moving) a->frames_moving_near++; }
    if (o->n_near > a->max_near) a->max_near = o->n_near;
    if (o->n_near_oncam > a->max_near_oncam) a->max_near_oncam = o->n_near_oncam;
    if (o->n_near_screen > a->max_near_screen) a->max_near_screen = o->n_near_screen;
    if (o->n_shrunk > a->max_shrunk) a->max_shrunk = o->n_shrunk;
    if (o->n_zref > a->max_zref) a->max_zref = o->n_zref;
    a->sum_drop_inside += o->n_drop_inside; a->sum_drop_oncam += o->n_drop_oncam;
    for (uint32_t i = 0; i < o->n_rec && i < NSG_FXN_MAX; i++) {
        if (o->rec[i].deg_raw > a->max_deg_raw) a->max_deg_raw = o->rec[i].deg_raw;
        fx_rec_insert(a->rec, &a->n_rec, &o->rec[i]);
    }
}
size_t ns_gpu_fx_near_describe(const NsGpuFxNearAcc *a, char *buf, size_t cap) {
    if (!buf || !cap) return 0;
    buf[0] = 0;
    if (!a) return 0;
    static const char *const act[4] = {"intacto", "reducido", "descartado_dentro", "descartado_pegado"};
    int n = snprintf(buf, cap, "fx-cerca(<0.5m): fotogramas=%u con_cercanos=%u en_movimiento=%u (con_cercanos=%u) max_vel=%.2fm/s max_giro=%.0fgr/s | max_cercanos=%u (pegados=%u pantalla=%u) reducidos=%u descartados(dentro=%u pegados=%u) cartel_prof_ref=%u max_ang=%.0fgr",
                     a->frames, a->frames_near, a->frames_moving, a->frames_moving_near, a->max_speed_mps, a->max_turn_dps, a->max_near, a->max_near_oncam, a->max_near_screen, a->max_shrunk,
                     a->sum_drop_inside, a->sum_drop_oncam, a->max_zref, a->max_deg_raw);
    for (uint32_t i = 0; i < a->n_rec && n > 0 && (size_t)n < cap; i++) {
        const NsGpuFxNear *r = &a->rec[i];
        n += snprintf(buf + n, cap - (size_t)n, " | [rol=%s %s%s d=%.2fm tam=%.2fm ang=%.0f->%.0fgr %s prim=%08x/%u]", ns_gpu_fx_role_name(r->role), r->kind == NS_EK_BILLBOARD ? "cartel" : "poligono",
                      (r->flags & NS_EF_ON_CAMERA) ? " ON_CAMERA" : "", r->dist_m, r->size_m, r->deg_raw, r->deg_after, act[r->action & 3], r->src_key, r->idx);
    }
    return n > 0 ? ((size_t)n < cap ? (size_t)n : cap - 1) : 0;
}

int ns_gpu_fx_build(const NsEffect *fx, uint32_t n_fx, const NsGpuFxParams *prm, NsGpuFxVert *verts, uint32_t cap_verts, NsGpuFxRun *runs, uint32_t cap_runs,
                    uint64_t *keys, uint32_t cap_keys, NsGpuFxOut *out) {
    return ns_gpu_fx_build_ex(fx, n_fx, prm, verts, cap_verts, runs, cap_runs, keys, cap_keys, NULL, 0, out);
}
int ns_gpu_fx_build_ex(const NsEffect *fx, uint32_t n_fx, const NsGpuFxParams *prm, NsGpuFxVert *verts, uint32_t cap_verts, NsGpuFxRun *runs, uint32_t cap_runs,
                       uint64_t *keys, uint32_t cap_keys, NsGpuFxSemiItem *items, uint32_t cap_items, NsGpuFxOut *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!prm || !prm->to_vr || !prm->cam || prm->n_eyes < 1 || prm->n_eyes > 2 || (n_fx && (!fx || !verts || !keys)) || (n_fx && !runs)) return -1;
    if (!n_fx) return 0;
    const float *M = prm->to_vr;
    float inv[12];
    const int have_inv = fx_inv34(M, inv);
    const float L = fx_len_scale(M);
    float eye_w[2][3] = {{0, 0, 0}, {0, 0, 0}};
    for (int e = 0; e < prm->n_eyes; e++) if (have_inv) fx_vr(inv, prm->eyes[e], eye_w[e]);
    const float *R = prm->cam->rot;
    const float det = R[0] * (R[4] * R[8] - R[5] * R[7]) - R[1] * (R[3] * R[8] - R[5] * R[6]) + R[2] * (R[3] * R[7] - R[4] * R[6]);
    const float face_sign = det < 0 ? -1.0f : 1.0f;
    const float zmin = prm->px_z_min > 0 ? prm->px_z_min : 32.0f, zref = prm->px_z_ref > 0 ? prm->px_z_ref : 1000.0f;
    const float ys = NS_OVERSCAN_Y;
    const uint32_t all_eyes = prm->n_eyes == 2 ? 3u : 1u;

    FxSemi *semi = (FxSemi *)malloc(sizeof(FxSemi) * n_fx);
    if (!semi) return -1;
    uint32_t nv = 0, nsemi = 0;
    /* los semitransparentes se escriben primero en la zona alta del búfer (de la cola hacia delante) y luego se reordenan: se genera todo en un solo recorrido
     * a una zona de trabajo = el propio búfer; los opacos se compactan al principio y los semitransparentes ordenados detrás (copia por trozos) */
    NsGpuFxVert *tmp = (NsGpuFxVert *)malloc(sizeof(NsGpuFxVert) * (size_t)cap_verts);
    if (!tmp) { free(semi); return -1; }
    uint32_t tn = 0;   /* vértices de los semitransparentes en tmp */
    for (uint32_t i = 0; i < n_fx; i++) {
        const NsEffect *e = &fx[i];
        out->n_items++;
        int ok = 1;
        float q[4][3];       /* esquinas en el mundo VR */
        float uvs[4][2];
        float cols[4][3];
        float fz[4] = {0, 0, 0, 0};   /* profundidad de la cámara del juego de cada esquina (niebla) */
        uint32_t mask = all_eyes;
        int nq = 0, tri_only = 0, used_zref = 0;
        for (int v = 0; v < 4; v++) { uvs[v][0] = e->uv[v][0]; uvs[v][1] = e->uv[v][1]; for (int c = 0; c < 3; c++) cols[v][c] = e->rgb[v][c]; }
        if (e->kind == NS_EK_BILLBOARD) {
            const int wsz = (e->flags & NS_EF_WORLD_SIZE) != 0;
            if (!fx_finite3(e->p[0]) || (!wsz && !(e->ref_h > 0))) ok = 0;
            else {
                float c[3];
                fx_vr(M, e->p[0], c);
                /* profundidad del motor del centro respecto a la cámara del juego; con poca profundidad (el tamaño en px explota) se usa la de referencia */
                const float *E = prm->cam->eye;
                float z = 0;
                for (int r = 0; r < 3; r++) z += (e->p[0][r] - E[r]) * R[3 * r + 2];
                float x0, y0, x1, y1;
                if (wsz) {   /* tamaño en mundo (unidades del motor del plano de la vista): no depende de ninguna profundidad */
                    x0 = e->box[0]; y0 = e->box[1]; x1 = e->box[2]; y1 = e->box[3];
                } else {     /* heredado: caja en píxeles a la profundidad de la cámara del juego */
                    if (z < zmin) { z = zref; used_zref = 1; }
                    float k = z / e->ref_h;
                    x0 = e->box[0] * k; y0 = e->box[1] * k / ys; x1 = e->box[2] * k; y1 = e->box[3] * k / ys;
                }
                const float ox[4] = {x0, x1, x0, x1}, oy[4] = {y0, y0, y1, y1};
                for (int v = 0; v < 4; v++) for (int r = 0; r < 3; r++) q[v][r] = c[r] + prm->head_right[r] * ox[v] * L - prm->head_up[r] * oy[v] * L;
                { float zc = ns_gpu_game_z(prm->cam, e->p[0]); fz[0] = fz[1] = fz[2] = fz[3] = zc; }
                nq = 4; out->n_billboard++; if (used_zref) out->n_zref++;
            }
        } else if ((e->flags & NS_EF_LINE) || e->nv == 2) {
            if (!fx_finite3(e->p[0]) || !fx_finite3(e->p[1])) ok = 0;
            else {
                float a[3], b[3];
                fx_vr(M, e->p[0], a); fx_vr(M, e->p[1], b);
                float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, vv[3] = {prm->head[0] - 0.5f * (a[0] + b[0]), prm->head[1] - 0.5f * (a[1] + b[1]), prm->head[2] - 0.5f * (a[2] + b[2])};
                float s[3] = {d[1] * vv[2] - d[2] * vv[1], d[2] * vv[0] - d[0] * vv[2], d[0] * vv[1] - d[1] * vv[0]};
                float l = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
                const float *E = prm->cam->eye;
                float z = 0;
                for (int r = 0; r < 3; r++) z += (e->p[0][r] - E[r]) * R[3 * r + 2];
                if (z < zmin) z = zref;
                float wpx = e->width_px > 0 ? e->width_px : 1.0f;
                float half = ((e->flags & NS_EF_WORLD_SIZE) ? 0.5f * wpx : 0.5f * wpx * z / (prm->cam->h > 0 ? prm->cam->h : 320.0f)) * L;   /* anchura en el mundo VR */
                float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (!(dl > 1e-9f) || !(l > 1e-4f * dl)) ok = 0;   /* línea nula o vista de punta (la cabeza sobre ella, a menos de 0,1 mm) */
                else {
                    for (int r = 0; r < 3; r++) {
                        float sr = s[r] / l * half;
                        q[0][r] = a[r] - sr; q[1][r] = a[r] + sr; q[2][r] = b[r] - sr; q[3][r] = b[r] + sr;
                    }
                    /* uv/color: extremo 0 para las esquinas 0,1 y extremo 1 para 2,3 */
                    for (int c = 0; c < 3; c++) { cols[2][c] = cols[1][c]; cols[3][c] = cols[1][c]; cols[1][c] = cols[0][c]; }
                    uvs[2][0] = uvs[1][0]; uvs[2][1] = uvs[1][1]; uvs[3][0] = uvs[1][0]; uvs[3][1] = uvs[1][1]; uvs[1][0] = uvs[0][0]; uvs[1][1] = uvs[0][1];
                    fz[0] = fz[1] = ns_gpu_game_z(prm->cam, e->p[0]); fz[2] = fz[3] = ns_gpu_game_z(prm->cam, e->p[1]);
                    nq = 4; out->n_ribbon++;
                }
            }
        } else {
            nq = e->nv == 3 ? 3 : 4;
            tri_only = nq == 3;
            for (int v = 0; v < nq; v++) { if (!fx_finite3(e->p[v])) ok = 0; else { fx_vr(M, e->p[v], q[v]); fz[v] = ns_gpu_game_z(prm->cam, e->p[v]); } }
            if (ok) out->n_poly++;
            if (ok && (e->flags & NS_EF_ONESIDE)) {
                float a[3], b[3], n[3];
                for (int r = 0; r < 3; r++) { a[r] = e->p[1][r] - e->p[0][r]; b[r] = e->p[2][r] - e->p[0][r]; }
                n[0] = a[1] * b[2] - a[2] * b[1]; n[1] = a[2] * b[0] - a[0] * b[2]; n[2] = a[0] * b[1] - a[1] * b[0];
                mask = 0;
                for (int ey = 0; ey < prm->n_eyes; ey++) {
                    float dd = face_sign * (n[0] * (e->p[0][0] - eye_w[ey][0]) + n[1] * (e->p[0][1] - eye_w[ey][1]) + n[2] * (e->p[0][2] - eye_w[ey][2]));
                    if (have_inv ? dd > 0 : 1) mask |= 1u << ey;
                }
            }
        }
        if (ok) for (int v = 0; v < nq; v++) if (!fx_finite3(q[v])) ok = 0;
        if (!ok) { out->n_skipped++; continue; }
        {   /* orden de dibujo del motor: sesgo `raise` por elemento y adelanto de los carteles */
            float pull = (prm->raise_m > 0.0f) ? (float)e->depth_bias * prm->raise_m : 0.0f;
            float cc[3] = {0, 0, 0};
            for (int v = 0; v < nq; v++) for (int r = 0; r < 3; r++) cc[r] += q[v][r] / (float)nq;
            float dv[3] = {cc[0] - prm->head[0], cc[1] - prm->head[1], cc[2] - prm->head[2]};
            float dist = sqrtf(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
            if (e->kind == NS_EK_BILLBOARD && prm->board_pull > 0.0f && nq == 4) {
                float w[3] = {q[1][0] - q[0][0], q[1][1] - q[0][1], q[1][2] - q[0][2]}, h[3] = {q[2][0] - q[0][0], q[2][1] - q[0][1], q[2][2] - q[0][2]};
                float hw = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]), hh = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
                pull += 0.5f * (hw > hh ? hw : hh) * prm->board_pull;
            }
            if (dist > 1e-4f && pull != 0.0f) {
                float lim = 0.3f * dist; if (pull > lim) pull = lim; if (pull < -lim) pull = -lim;
                float f = (dist - pull) / dist;   /* escalar desde la cabeza: la proyección no cambia, la profundidad sí */
                for (int v = 0; v < nq; v++) for (int r = 0; r < 3; r++) q[v][r] = prm->head[r] + (q[v][r] - prm->head[r]) * f;
            }
        }
        int dropped = 0;
        {   /* comodidad VR y registro de los cercanos (después del adelanto: es lo que ven los ojos) */
            const float nearm = prm->near_log_m > 0.0f ? prm->near_log_m : 0.5f;
            float cc[3] = {0, 0, 0};
            for (int v = 0; v < nq; v++) for (int r = 0; r < 3; r++) cc[r] += q[v][r] / (float)nq;
            const float dc = sqrtf((cc[0] - prm->head[0]) * (cc[0] - prm->head[0]) + (cc[1] - prm->head[1]) * (cc[1] - prm->head[1]) + (cc[2] - prm->head[2]) * (cc[2] - prm->head[2]));
            const int oncam = (e->flags & NS_EF_ON_CAMERA) != 0, screen = oncam || e->role == FXR_VIEW_OVERLAY;
            const int env = e->role == FXR_SURFACE || e->role == FXR_WATER_SURFACE;   /* superficies del entorno (agua, paneles): no son sprites, no se reducen */
            const int watch = dc < nearm;
            float deg0 = 0.0f, deg1 = 0.0f; int action = NSG_FXN_KEPT;
            if (watch || prm->comfort_deg > 0.0f || prm->drop_inside_m > 0.0f || (screen && prm->oncam_drop_m > 0.0f)) deg0 = deg1 = fx_ang_deg(q, nq, prm->head);
            if (screen && prm->oncam_drop_m > 0.0f && dc < prm->oncam_drop_m) { dropped = 1; action = NSG_FXN_DROP_ONCAM; out->n_drop_oncam++; deg1 = 0.0f; }
            else if (prm->drop_inside_m > 0.0f && !env && dc < prm->drop_inside_m) { dropped = 1; action = NSG_FXN_DROP_INSIDE; out->n_drop_inside++; deg1 = 0.0f; }
            else if (prm->comfort_deg > 0.0f && !env) {
                const float cap = ns_gpu_fx_comfort_cap_deg(dc, prm->comfort_deg, prm->comfort_dmin_m);
                if (cap <= 0.0f) { dropped = 1; action = NSG_FXN_DROP_INSIDE; out->n_drop_inside++; deg1 = 0.0f; }
                else if (deg0 > cap) {
                    for (int it = 0; it < 4 && deg1 > cap * 1.001f; it++) {   /* la reducción no es lineal con el ángulo: se afina */
                        const float sc = cap / deg1 < 0.98f ? cap / deg1 : 0.98f;
                        for (int v = 0; v < nq; v++) for (int r = 0; r < 3; r++) q[v][r] = cc[r] + (q[v][r] - cc[r]) * sc;
                        deg1 = fx_ang_deg(q, nq, prm->head);
                    }
                    action = NSG_FXN_SHRUNK; out->n_shrunk++;
                }
            }
            if (watch || action == NSG_FXN_DROP_ONCAM) {
                if (watch) { out->n_near++; if (oncam) out->n_near_oncam++; if (screen) out->n_near_screen++; }
                NsGpuFxNear r; memset(&r, 0, sizeof r);
                r.src_key = e->src_key; r.idx = e->idx; r.flags = e->flags; r.role = e->role; r.kind = e->kind; r.action = (uint8_t)action; r.screen = (uint8_t)screen;
                r.dist_m = dc; r.size_m = fx_size_m(q, nq); r.deg_raw = deg0; r.deg_after = deg1;
                if (action == NSG_FXN_SHRUNK) { /* tamaño original */ r.size_m = fx_size_m(q, nq) * (deg1 > 1e-3f ? deg0 / deg1 : 1.0f); }
                fx_rec_insert(out->rec, &out->n_rec, &r);
            }
        }
        if (prm->fog && prm->fog->active && prm->fog->len > 0.0f)
            for (int v = 0; v < nq; v++) {
                const float s = 1.0f - ns_gpu_fog_f(prm->fog, fz[v]);
                cols[v][0] *= s; cols[v][1] *= s; cols[v][2] *= s;
            }
        if (out->n_keys < cap_keys) keys[out->n_keys++] = ns_gpu_fx_key(e);
        if (mask == 0 || dropped) continue;   /* confirmado (el motor tampoco lo dibujaría desde aquí) pero sin triángulos */
        const int semi_f = (e->flags & NS_EF_SEMI) != 0, tex = (e->flags & NS_EF_TEXTURED) != 0;
        const uint32_t cf = (uint32_t)((tex ? NSG_CF_TEX : 0u) | (semi_f ? NSG_CF_SEMI : 0u) | ((uint32_t)(e->abr & 3u) << 2));
        const uint32_t clutf = (uint32_t)e->clut | (cf << 16);
        static const int T4[6] = {0, 1, 2, 1, 2, 3}, T3[3] = {0, 1, 2};
        const int *T = tri_only ? T3 : T4;
        const int nt = tri_only ? 3 : 6;
        /* copias: pase 0 si es opaco o semitransparente texturizado; pase 1 si es semitransparente */
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 0 && semi_f && !tex) continue;
            if (pass == 1 && !semi_f) continue;
            if (pass == 0) {
                if (nv + (uint32_t)nt > cap_verts) { out->n_skipped++; break; }
            } else if (tn + (uint32_t)nt > cap_verts) { out->n_skipped++; break; }
            NsGpuFxVert *o = pass == 0 ? &verts[nv] : &tmp[tn];
            for (int k = 0; k < nt; k++) {
                const int v = T[k];
                o[k].p[0] = q[v][0]; o[k].p[1] = q[v][1]; o[k].p[2] = q[v][2];
                o[k].u = uvs[v][0]; o[k].v = uvs[v][1];
                o[k].col[0] = cols[v][0]; o[k].col[1] = cols[v][1]; o[k].col[2] = cols[v][2];
                o[k].tpage = e->tpage; o[k].clutf = clutf; o[k].eyes = mask;
            }
            if (pass == 0) nv += (uint32_t)nt;
            else {
                float cx[3] = {0, 0, 0};
                for (int v = 0; v < nq; v++) for (int r = 0; r < 3; r++) cx[r] += q[v][r] / (float)nq;
                float dx = cx[0] - prm->head[0], dy = cx[1] - prm->head[1], dz = cx[2] - prm->head[2];
                semi[nsemi].depth = prm->has_depth_row ? prm->depth_row[0] * cx[0] + prm->depth_row[1] * cx[1] + prm->depth_row[2] * cx[2] + prm->depth_row[3]
                                                       : dx * dx + dy * dy + dz * dz; semi[nsemi].first = tn; semi[nsemi].count = (uint32_t)nt; semi[nsemi].abr = (uint8_t)(e->abr & 3u); semi[nsemi].order = nsemi;
                nsemi++; tn += (uint32_t)nt;
            }
        }
    }
    out->n_opaque = nv;
    qsort(semi, nsemi, sizeof *semi, fx_semi_cmp);
    uint32_t w = nv;
    for (uint32_t s = 0; s < nsemi; s++) {
        if (w + semi[s].count > cap_verts) { out->n_skipped++; continue; }
        memcpy(&verts[w], &tmp[semi[s].first], sizeof(NsGpuFxVert) * semi[s].count);
        if (out->n_runs && runs[out->n_runs - 1].abr == semi[s].abr && runs[out->n_runs - 1].start + runs[out->n_runs - 1].count == w) runs[out->n_runs - 1].count += semi[s].count;
        else if (out->n_runs < cap_runs) { runs[out->n_runs].start = w; runs[out->n_runs].count = semi[s].count; runs[out->n_runs].abr = semi[s].abr; out->n_runs++; }
        else { /* sin sitio para otro run: se mezcla en el último (el orden relativo se conserva) */ runs[out->n_runs - 1].count += semi[s].count; }
        if (items && out->n_sitems < cap_items) {
            NsGpuFxSemiItem *it = &items[out->n_sitems++];
            it->depth = semi[s].depth; it->start = w; it->count = semi[s].count; it->abr = semi[s].abr;
        }
        w += semi[s].count;
    }
    out->n_semi = w - nv;
    free(semi); free(tmp);
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------ pasada semitransparente única */
size_t ns_gpu_semi_merge(const float *mesh_depth, const uint8_t *mesh_abr, size_t n_mesh, const NsGpuFxSemiItem *fx, size_t n_fx, NsGpuSemiRun *runs, size_t cap_runs,
                         int *truncated) {
    if (truncated) *truncated = 0;
    if (!runs || !cap_runs) { if (truncated && (n_mesh || n_fx)) *truncated = 1; return 0; }
    size_t i = 0, j = 0, n = 0;
    while (i < n_mesh || j < n_fx) {
        int take_mesh;
        if (i >= n_mesh) take_mesh = 0;
        else if (j >= n_fx) take_mesh = 1;
        else take_mesh = mesh_depth[i] >= fx[j].depth;   /* el más lejano primero; empate: malla antes que efecto */
        uint32_t kind = take_mesh ? 0u : 1u, start, count;
        uint8_t abr;
        if (take_mesh) { start = (uint32_t)i; count = 1; abr = (uint8_t)(mesh_abr[i] & 3u); i++; }
        else { start = fx[j].start; count = fx[j].count; abr = (uint8_t)(fx[j].abr & 3u); j++; }
        if (n && runs[n - 1].kind == kind && runs[n - 1].abr == abr && runs[n - 1].start + runs[n - 1].count == start) { runs[n - 1].count += count; continue; }
        if (n >= cap_runs) { if (truncated) *truncated = 1; break; }
        runs[n].kind = kind; runs[n].start = start; runs[n].count = count; runs[n].abr = abr; n++;
    }
    return n;
}

int ns_gpu_tri_back(const float p0[3], const float p1[3], const float p2[3], const float eye[3]) {
    float e1[3], e2[3], r[3];
    for (int a = 0; a < 3; a++) { e1[a] = p1[a] - p0[a]; e2[a] = p2[a] - p0[a]; r[a] = p0[a] - eye[a]; }
    const float cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2], cz = e1[0] * e2[1] - e1[1] * e2[0];
    return r[0] * cx + r[1] * cy + r[2] * cz <= 0.0f;
}
