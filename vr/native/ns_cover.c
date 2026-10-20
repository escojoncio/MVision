/* ns_cover: ver ns_cover.h. */
#include "ns_cover.h"
#include "ns_extract.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

const char *const ns_cover_cause_name[NC_N] = {"exacto", "otra_diagonal", "subdividido", "sprite", "pose_distinta", "otra_textura", "parcial", "ausente"};
const char *const ns_cover_rev_name[NR_N] = {"detras", "plano_cercano", "fuera_de_pantalla", "reverso", "lejos", "objeto_no_visible", "mayor_que_el_hardware", "visible_de_mas"};

void ns_cover_cfg_default(NsCoverCfg *c) {
    memset(c, 0, sizeof *c);
    c->tol_base = 20.0f; c->tol_z = 0.004f; c->surf_tol = 80.0f;
    c->screen_w = 320; c->screen_h = 256; c->max_detail = 4096;
}

#define GUARD 1024.0f   /* el motor no dibuja fuera de ±1024 px (rango de las coordenadas de pantalla) */
#define CELL 256.0f
#define TOL_CAP 200.0f

typedef struct { float p[3][3]; float c[3]; float r; uint16_t page, clut; float u[3], v[3]; } NTri;
typedef struct { int32_t *head, *next; uint32_t mask; } Grid;

typedef struct {
    NTri *nt; size_t n; const NsAdaptSrc *src;
    Grid g; int32_t *stamp; int32_t qid;
    uint8_t *used;
    const NsCoverCfg *cfg;
    NsCoverResult *out;
    NsCoverGroup grp[96]; int ngrp; NsCoverGroup rgrp[96]; int nrgrp; NsCoverGroup cgrp[96]; int ncgrp; NsCoverGroup sgrp[96]; int nsgrp;
    double col_sum, shift_sum;
} Ctx;

static double d3(const float *a, const float *b) { double x = (double)a[0] - b[0], y = (double)a[1] - b[1], z = (double)a[2] - b[2]; return sqrt(x * x + y * y + z * z); }
static void sub3(const float *a, const float *b, double *o) { for (int i = 0; i < 3; i++) o[i] = (double)a[i] - b[i]; }
static void crs(const double *a, const double *b, double *o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static double dot3(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* distancia de un punto a un triángulo 3D */
static double pt_tri(const float *P, const float tv[3][3]) {
    double e0[3], e1[3], n[3], w[3];
    sub3(tv[1], tv[0], e0); sub3(tv[2], tv[0], e1); crs(e0, e1, n);
    double nl = sqrt(dot3(n, n));
    sub3(P, tv[0], w);
    if (nl > 1e-9) {
        double dist = dot3(w, n) / nl;
        double pr[3] = {w[0] - dist * n[0] / nl, w[1] - dist * n[1] / nl, w[2] - dist * n[2] / nl};
        double d00 = dot3(e0, e0), d01 = dot3(e0, e1), d11 = dot3(e1, e1), d20 = dot3(pr, e0), d21 = dot3(pr, e1), den = d00 * d11 - d01 * d01;
        if (fabs(den) > 1e-12) {
            double b = (d11 * d20 - d01 * d21) / den, c = (d00 * d21 - d01 * d20) / den, a = 1.0 - b - c;
            if (a >= -0.01 && b >= -0.01 && c >= -0.01) return fabs(dist);
        }
    }
    double best = 1e30;
    for (int i = 0; i < 3; i++) {
        const float *A = tv[i], *B = tv[(i + 1) % 3];
        double ab[3], ap[3];
        sub3(B, A, ab); sub3(P, A, ap);
        double l2 = dot3(ab, ab), s = l2 > 0 ? dot3(ap, ab) / l2 : 0;
        s = s < 0 ? 0 : s > 1 ? 1 : s;
        double q[3] = {A[0] + s * ab[0] - P[0], A[1] + s * ab[1] - P[1], A[2] + s * ab[2] - P[2]};
        double dd = sqrt(dot3(q, q));
        if (dd < best) best = dd;
    }
    return best;
}

/* ---------------- cuadrícula espacial de esquinas nativas ---------------- */
static int32_t cell_of(float v) { float c = floorf(v / CELL); return (int32_t)(c < -1e6f ? -1e6f : c > 1e6f ? 1e6f : c); }
static uint32_t hcell(int32_t x, int32_t y, int32_t z) { return ((uint32_t)x * 73856093u) ^ ((uint32_t)y * 19349663u) ^ ((uint32_t)z * 83492791u); }
static int grid_build(Grid *g, const NTri *nt, size_t n) {
    size_t ne = n * 3, nb = 64;
    while (nb < ne * 2) nb <<= 1;
    g->head = (int32_t *)malloc(nb * sizeof(int32_t));
    g->next = (int32_t *)malloc((ne ? ne : 1) * sizeof(int32_t));
    if (!g->head || !g->next) return -1;
    g->mask = (uint32_t)(nb - 1);
    for (size_t i = 0; i < nb; i++) g->head[i] = -1;
    for (size_t e = 0; e < ne; e++) {
        const float *p = nt[e / 3].p[e % 3];
        uint32_t b = hcell(cell_of(p[0]), cell_of(p[1]), cell_of(p[2])) & g->mask;
        g->next[e] = g->head[b]; g->head[b] = (int32_t)e;
    }
    return 0;
}
/* triángulos con alguna esquina a <= r de q (únicos); devuelve cuántos (hasta cap) */
static int grid_query(Ctx *c, const float *q, float r, int32_t *out, int cap) {
    int n = 0;
    int32_t qid = ++c->qid;
    int32_t cx = cell_of(q[0]), cy = cell_of(q[1]), cz = cell_of(q[2]);
    for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) for (int dz = -1; dz <= 1; dz++) {
        uint32_t b = hcell(cx + dx, cy + dy, cz + dz) & c->g.mask;
        for (int32_t e = c->g.head[b]; e >= 0; e = c->g.next[e]) {
            int32_t t = e / 3;
            if (c->stamp[t] == qid) continue;
            if (d3(c->nt[t].p[e % 3], q) <= r) { c->stamp[t] = qid; if (n < cap) out[n++] = t; }
        }
    }
    return n;
}

static float tol_at(const NsCoverCfg *cfg, float z) {
    float t = cfg->tol_base + cfg->tol_z * (z > 0 ? z : 0);
    return t > TOL_CAP ? TOL_CAP : t;
}

static uint16_t op_page(const GpuOp *o) { return (uint16_t)(((o->tx / 64) & 15) | (((o->ty / 256) & 1) << 4) | ((o->tp & 3) << 7)); }
static uint16_t op_clut(const GpuOp *o) { return (uint16_t)(((o->clx / 16) & 63) | ((o->cly & 511) << 6)); }
#define PAGE_MASK 0x19Fu   /* x, y y profundidad de color (sin el modo de mezcla) */

static const int PERMS[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};

static void add_group(NsCoverGroup *tab, int *n, int cap, const NsCoverGroup *g) {
    for (int i = 0; i < *n; i++) {
        NsCoverGroup *e = &tab[i];
        if (e->cause == g->cause && e->semi == g->semi && e->key == g->key && e->piece == g->piece && e->tpage == g->tpage && e->clut == g->clut) {
            e->n++;
            if (g->zmin < e->zmin) e->zmin = g->zmin;
            return;
        }
    }
    if (*n < cap) { tab[*n] = *g; tab[*n].n = 1; (*n)++; }
}
static int grp_cmp(const void *a, const void *b) {
    const NsCoverGroup *x = (const NsCoverGroup *)a, *y = (const NsCoverGroup *)b;
    return (y->n > x->n) - (y->n < x->n);
}

/* ¿textura de la cara nativa j igual a la del triángulo de la lista? */
static int same_tex(const NTri *t, const GpuOp *o, int textured) {
    if (!textured) return 1;
    return (t->page & PAGE_MASK) == (op_page(o) & PAGE_MASK) && t->clut == op_clut(o);
}

/* mejor emparejamiento de las esquinas de q con un triángulo nativo (perm da el orden). Devuelve la peor distancia de esquina (1e30 si no hay) */
static double tri_dist(const NTri *t, float q[3][3], int *perm) {
    double best = 1e30;
    for (int pp = 0; pp < 6; pp++) {
        double m = 0;
        for (int k = 0; k < 3 && m < best; k++) { double d = d3(q[k], t->p[PERMS[pp][k]]); if (d > m) m = d; }
        if (m < best) { best = m; *perm = pp; }
    }
    return best;
}

int ns_cover_core(const NsRTri *tris, const NsAdaptSrc *src, size_t n, const NsCoverCam *cam, const GpuOp *ops, uint32_t nops, const NsCoverCfg *cfg_in, NsCoverResult *out) {
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    NsCoverCfg cfg;
    if (cfg_in) cfg = *cfg_in; else ns_cover_cfg_default(&cfg);
    if (!(cfg.tol_base > 0)) cfg.tol_base = 20.0f;
    if (!(cfg.surf_tol > 0)) cfg.surf_tol = 80.0f;
    if (cfg.screen_w <= 0) cfg.screen_w = 320;
    if (cfg.screen_h <= 0) cfg.screen_h = 256;
    if (!cfg.max_detail) cfg.max_detail = 4096;
    memset(out, 0, sizeof *out);
    out->list_ops = nops; out->nat_tris = (uint32_t)n;
    if (!cam || (n && (!tris || !src))) return -1;

    Ctx C;
    memset(&C, 0, sizeof C);
    C.n = n; C.src = src; C.cfg = &cfg; C.out = out;
    C.nt = (NTri *)malloc((n ? n : 1) * sizeof(NTri));
    C.stamp = (int32_t *)calloc(n ? n : 1, sizeof(int32_t));
    C.used = (uint8_t *)calloc(n ? n : 1, 1);
    if (!C.nt || !C.stamp || !C.used) { free(C.nt); free(C.stamp); free(C.used); return -1; }
    for (size_t i = 0; i < n; i++) {
        NTri *t = &C.nt[i];
        for (int c = 0; c < 3; c++) {
            const float *p = tris[i].v[c].p;
            for (int r = 0; r < 3; r++) t->p[c][r] = cam->A[3 * r] * p[0] + cam->A[3 * r + 1] * p[1] + cam->A[3 * r + 2] * p[2] + cam->t[r];
            t->u[c] = tris[i].v[c].u; t->v[c] = tris[i].v[c].v;
        }
        for (int a = 0; a < 3; a++) t->c[a] = (t->p[0][a] + t->p[1][a] + t->p[2][a]) / 3.0f;
        float r = 0;
        for (int c = 0; c < 3; c++) { float d = (float)d3(t->p[c], t->c); if (d > r) r = d; }
        t->r = r; t->page = tris[i].tpage; t->clut = tris[i].clut;
    }
    if (grid_build(&C.g, C.nt, n)) { free(C.nt); free(C.stamp); free(C.used); free(C.g.head); free(C.g.next); return -1; }

    float scr_h = 160.0f, scr_cx = cfg.screen_w * 0.5f, scr_cy = cfg.screen_h * 0.5f;
    int have_proj = 0;
    uint32_t detail = 0;
    enum { CANDMAX = 96 };
    int32_t cand[3][CANDMAX];

    for (uint32_t oi = 0; oi < nops; oi++) {
        const GpuOp *o = &ops[oi];
        if (o->type != GPU_OP_POLY || o->nv < 3) continue;
        int vv = 1;
        for (int k = 0; k < o->nv; k++) if (!o->v[k].vvalid) vv = 0;
        if (!vv) { out->list_no_view++; continue; }
        out->list_polys3d++;
        if (!have_proj && o->proj_h > 0) { scr_h = o->proj_h; scr_cx = (float)o->dox + o->proj_ox; scr_cy = (float)o->doy + o->proj_oy; have_proj = 1; }
        const int textured = (o->flags & GPU_F_TEXTURED) != 0, semi = (o->flags & GPU_F_SEMI) != 0;
        for (int h = 0; h < (o->nv == 4 ? 2 : 1); h++) {
            const int ci[3] = {h ? 1 : 0, h ? 2 : 1, h ? 3 : 2};
            float q[3][3];
            for (int k = 0; k < 3; k++) { const GpuVtx *v = &o->v[ci[k]]; q[k][0] = v->vx; q[k][1] = v->vy; q[k][2] = v->vz; }
            if (d3(q[0], q[1]) < 2 || d3(q[0], q[2]) < 2 || d3(q[1], q[2]) < 2) { out->list_degenerate++; continue; }
            out->list_tris++;
            const float zavg = (q[0][2] + q[1][2] + q[2][2]) / 3.0f;
            const float tol = tol_at(&cfg, zavg);
            /* ---- 1. pareja exacta ---- */
            int nc = grid_query(&C, q[0], tol, cand[0], CANDMAX);
            double best = 1e30; int bj = -1, bperm = 0;
            for (int i = 0; i < nc; i++) {
                int pp = 0; double d = tri_dist(&C.nt[cand[0][i]], q, &pp);
                if (d < best) { best = d; bj = cand[0][i]; bperm = pp; }
            }
            if (bj >= 0 && best <= tol) {
                out->cause[NC_MATCH][semi]++;
                C.used[bj] = 1;
                if (textured) {
                    const NTri *t = &C.nt[bj];
                    if ((t->page & PAGE_MASK) != (op_page(o) & PAGE_MASK)) out->tex_page++;
                    if (t->clut != op_clut(o)) out->tex_clut++;
                    /* color por vértice (128 = neutro): mismo orden de esquinas que la pareja */
                    double cs = 0;
                    for (int k = 0; k < 3; k++) {
                        const GpuVtx *v = &o->v[(o->flags & GPU_F_GOURAUD) ? ci[k] : 0];
                        const NsRVert *nv = &tris[bj].v[PERMS[bperm][k]];
                        cs += fabs((double)v->r - nv->r) + fabs((double)v->g - nv->g) + fabs((double)v->b - nv->b);
                    }
                    cs /= 9.0;
                    {   /* desplazamiento en píxeles */
                        double px = best * scr_h / (zavg > 8.0f ? zavg : 8.0f);
                        out->shift_n++; C.shift_sum += px;
                        if (px > 2.0) {
                            out->shift_bad++;
                            NsCoverGroup g; memset(&g, 0, sizeof g);
                            g.cause = NC_N + 2; g.semi = (uint8_t)semi; g.key = src[bj].key; g.piece = src[bj].piece_rel; g.mesh = src[bj].mesh_id; g.tpage = op_page(o); g.clut = op_clut(o); g.zmin = zavg;
                            add_group(C.sgrp, &C.nsgrp, 96, &g);
                        }
                    }
                    out->col_n++; C.col_sum += cs;
                    if (cs > 24.0) {
                        out->col_bad++;
                        NsCoverGroup g; memset(&g, 0, sizeof g);
                        g.cause = NC_N + 1; g.semi = (uint8_t)semi; g.key = src[bj].key; g.piece = src[bj].piece_rel; g.mesh = src[bj].mesh_id; g.tpage = op_page(o); g.clut = op_clut(o); g.zmin = zavg;
                        add_group(C.cgrp, &C.ncgrp, 96, &g);
                    }
                    for (int k = 0; k < 3; k++) {
                        const GpuVtx *v = &o->v[ci[k]]; int pk = PERMS[bperm][k];
                        if (fabsf((float)v->u - t->u[pk]) > 1.0f || fabsf((float)v->v - t->v[pk]) > 1.0f) { out->tex_uv++; break; }
                    }
                }
                continue;
            }
            /* ---- clasificación de los que no tienen pareja ---- */
            if (detail >= cfg.max_detail) {
                out->cause[NC_ABSENT][semi]++;
                NsCoverGroup g; memset(&g, 0, sizeof g);
                g.cause = NC_ABSENT; g.semi = (uint8_t)semi; g.tpage = op_page(o); g.clut = op_clut(o); g.zmin = zavg;
                add_group(C.grp, &C.ngrp, 96, &g);
                continue;
            }
            detail++;
            /* 2. misma cara nativa con otra diagonal: las tres esquinas son vértices de UNA cara */
            int split = 0, split_j = -1;
            {
                int cn[3];
                for (int k = 0; k < 3; k++) cn[k] = k == 0 ? nc : grid_query(&C, q[k], tol, cand[k], CANDMAX);
                for (int i = 0; i < cn[0] && !split; i++) {
                    const NsAdaptSrc *s0 = &src[cand[0][i]];
                    int ok = 1;
                    for (int k = 1; k < 3 && ok; k++) {
                        int found = 0;
                        for (int j = 0; j < cn[k]; j++) {
                            const NsAdaptSrc *sj = &src[cand[k][j]];
                            if (sj->inst == s0->inst && sj->piece == s0->piece && sj->face == s0->face) { found = 1; break; }
                        }
                        ok = found;
                    }
                    if (ok) { split = 1; split_j = cand[0][i]; }
                }
            }
            if (split) {
                out->cause[NC_SPLIT][semi]++;
                C.used[split_j] = 1;
                /* la otra mitad de la cara también cuenta como usada */
                if (split_j > 0 && src[split_j - 1].inst == src[split_j].inst && src[split_j - 1].piece == src[split_j].piece && src[split_j - 1].face == src[split_j].face) C.used[split_j - 1] = 1;
                if ((size_t)split_j + 1 < n && src[split_j + 1].inst == src[split_j].inst && src[split_j + 1].piece == src[split_j].piece && src[split_j + 1].face == src[split_j].face) C.used[split_j + 1] = 1;
                continue;
            }
            /* 3. sobre la superficie nativa (misma textura / cualquier textura) */
            const float stol = cfg.surf_tol + tol;
            int on_same = 0, on_any = 0; int own_same = -1, own_any = -1; double bsame[3] = {1e30, 1e30, 1e30};
            int ow[3] = {-1, -1, -1};
            for (int k = 0; k < 3; k++) {
                double bs = 1e30, ba = 1e30; int is = -1, ia = -1;
                for (size_t j = 0; j < n; j++) {
                    const NTri *t = &C.nt[j];
                    if (d3(q[k], t->c) > t->r + stol) continue;
                    double d = pt_tri(q[k], t->p);
                    if (d < ba) { ba = d; ia = (int)j; }
                    if (d < bs && same_tex(t, o, textured)) { bs = d; is = (int)j; }
                }
                if (bs <= stol) { on_same++; ow[k] = is; }
                if (ba <= stol) { on_any++; if (own_any < 0) own_any = ia; }
                bsame[k] = bs;
                if (ow[k] >= 0 && own_same < 0) own_same = ow[k];
            }
            (void)bsame;
            int cause;
            int owner = -1;
            if (on_same == 3) {
                cause = NC_SUBDIV; owner = own_same;
                for (int k = 0; k < 3; k++) if (ow[k] >= 0) C.used[ow[k]] = 1;
            } else if (fabsf(q[0][2] - q[1][2]) < 2.0f && fabsf(q[0][2] - q[2][2]) < 2.0f && on_any == 0) {   /* rectángulo a profundidad constante sin nada debajo */
                cause = NC_BILLBOARD;
            } else {
                /* misma textura y UV en otro sitio */
                int uj = -1; double ub = 1e30;
                if (textured) {
                    for (size_t j = 0; j < n; j++) {
                        const NTri *t = &C.nt[j];
                        if (!same_tex(t, o, 1)) continue;
                        for (int pp = 0; pp < 6; pp++) {
                            int ok = 1; double m = 0;
                            for (int k = 0; k < 3; k++) {
                                int pk = PERMS[pp][k]; const GpuVtx *v = &o->v[ci[k]];
                                if (fabsf((float)v->u - t->u[pk]) > 1.0f || fabsf((float)v->v - t->v[pk]) > 1.0f) { ok = 0; break; }
                                double d = d3(q[k], t->p[pk]); if (d > m) m = d;
                            }
                            if (ok && m < ub) { ub = m; uj = (int)j; }
                        }
                    }
                }
                if (uj >= 0) { cause = NC_POSE; owner = uj; C.used[uj] = 1; }
                else if (on_same > 0) { cause = NC_PARTIAL; owner = own_same; }
                else if (on_any > 0) { cause = NC_OTHERTEX; owner = own_any; }
                else {
                    cause = NC_ABSENT;
                    /* dueño más cercano (vértice nativo más próximo a la esquina 0) */
                    double nb = 1e30;
                    for (size_t j = 0; j < n; j++) for (int c = 0; c < 3; c++) { double d = d3(q[0], C.nt[j].p[c]); if (d < nb) { nb = d; owner = (int)j; } }
                }
            }
            out->cause[cause][semi]++;
            if (cfg.dbg) fprintf((FILE *)cfg.dbg, "LISTA op=%u h=%d causa=%s semi=%d z=%.0f page=%04x clut=%04x dueno=%08x pieza=%u cara=%u esquinas=(%.0f,%.0f,%.0f)(%.0f,%.0f,%.0f)(%.0f,%.0f,%.0f)\n", oi, h, ns_cover_cause_name[cause], semi, zavg, op_page(o), op_clut(o), owner >= 0 ? src[owner].key : 0u, owner >= 0 ? src[owner].piece_rel : 0u, owner >= 0 ? src[owner].face : 0u, q[0][0], q[0][1], q[0][2], q[1][0], q[1][1], q[1][2], q[2][0], q[2][1], q[2][2]);
            if (cause != NC_SUBDIV) {
                NsCoverGroup g; memset(&g, 0, sizeof g);
                g.cause = (uint8_t)cause; g.semi = (uint8_t)semi; g.tpage = op_page(o); g.clut = op_clut(o); g.zmin = zavg;
                if (owner >= 0) { g.key = src[owner].key; g.piece = src[owner].piece_rel; g.mesh = src[owner].mesh_id; }
                add_group(C.grp, &C.ngrp, 96, &g);
            }
        }
    }

    /* ---- inverso: triángulos nativos sin pareja ---- */
    for (size_t j = 0; j < n; j++) {
        if (C.used[j]) { out->nat_used++; continue; }
        const NTri *T = &C.nt[j];
        int behind = 0;
        double sx[3] = {0, 0, 0}, sy[3] = {0, 0, 0};
        for (int c = 0; c < 3; c++) {
            if (T->p[c][2] <= 1.0f) behind++;
            else { sx[c] = scr_cx + scr_h * T->p[c][0] / T->p[c][2]; sy[c] = scr_cy + scr_h * T->p[c][1] / T->p[c][2]; }
        }
        int cls;
        if (behind == 3) cls = NR_BEHIND;
        else if (behind) cls = NR_NEAR;
        else {
            double mnx = 1e30, mxx = -1e30, mny = 1e30, mxy = -1e30, zmin = 1e30;
            for (int c = 0; c < 3; c++) {
                if (sx[c] < mnx) mnx = sx[c];
                if (sx[c] > mxx) mxx = sx[c];
                if (sy[c] < mny) mny = sy[c];
                if (sy[c] > mxy) mxy = sy[c];
                if (T->p[c][2] < zmin) zmin = T->p[c][2];
            }
            double e0[3], e1[3], nn[3], p0[3] = {T->p[0][0], T->p[0][1], T->p[0][2]};
            sub3(T->p[1], T->p[0], e0); sub3(T->p[2], T->p[0], e1); crs(e0, e1, nn);
            double s = dot3(p0, nn);
            if (src[j].half == 1) s = -s;   /* (B,D,C) tiene el sentido contrario al del perímetro */
            if (mxx < 0 || mnx >= cfg.screen_w || mxy < 0 || mny >= cfg.screen_h || mnx < -GUARD || mxx > GUARD || mny < -GUARD || mxy > GUARD) cls = NR_OFF;   /* fuera de la pantalla del motor (el visor ve más) o con alguna esquina fuera del rango del hardware (±1024) */
            else if (!(src[j].mflags & NS_MF_TWOSIDED) && s <= 0) cls = NR_BACK;
            else if (cam->fog_far > 0 && zmin > cam->fog_far) cls = NR_FAR;
            else if (src[j].vis == 0) cls = NR_HIDDEN;
            else if (mxx - mnx > 1023.0 || mxy - mny > 511.0) cls = NR_BIG;
            else {
                cls = NR_EXTRA;
                if (cfg.dbg) fprintf((FILE *)cfg.dbg, "NATIVO_DE_MAS key=%08x pieza=%u cara=%u mitad=%u malla=%08x page=%04x clut=%04x z=%.0f scr=(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f) uv=(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)\n", src[j].key, src[j].piece_rel, src[j].face, src[j].half, src[j].mesh_id, T->page, T->clut, zmin, sx[0], sy[0], sx[1], sy[1], sx[2], sy[2], T->u[0], T->v[0], T->u[1], T->v[1], T->u[2], T->v[2]);
                NsCoverGroup g; memset(&g, 0, sizeof g);
                g.cause = NC_N; g.key = src[j].key; g.piece = src[j].piece_rel; g.mesh = src[j].mesh_id; g.tpage = (uint16_t)(T->page & PAGE_MASK); g.clut = T->clut; g.zmin = (float)zmin;
                add_group(C.rgrp, &C.nrgrp, 96, &g);
            }
        }
        out->rev[cls]++;
    }

    /* ---- agregados ---- */
    uint32_t cov = 0, cov_op = 0, tot_op = 0;
    for (int c = 0; c < NC_N; c++) for (int s = 0; s < 2; s++) {
        if (c == NC_MATCH || c == NC_SPLIT || c == NC_SUBDIV) { cov += out->cause[c][s]; if (!s) cov_op += out->cause[c][s]; }
        if (!s) tot_op += out->cause[c][s];
    }
    if (out->list_tris) {
        out->cover_pct = 100.0f * (float)cov / (float)out->list_tris;
        out->exact_pct = 100.0f * (float)(out->cause[NC_MATCH][0] + out->cause[NC_MATCH][1]) / (float)out->list_tris;
    } else out->cover_pct = out->exact_pct = 100.0f;
    out->cover_opaque_pct = tot_op ? 100.0f * (float)cov_op / (float)tot_op : 100.0f;
    qsort(C.grp, (size_t)C.ngrp, sizeof(NsCoverGroup), grp_cmp);
    out->n_worst = (uint32_t)(C.ngrp < NC_WORST ? C.ngrp : NC_WORST);
    memcpy(out->worst, C.grp, out->n_worst * sizeof(NsCoverGroup));
    out->shift_mean_px = out->shift_n ? (float)(C.shift_sum / out->shift_n) : 0.0f;
    qsort(C.sgrp, (size_t)C.nsgrp, sizeof(NsCoverGroup), grp_cmp);
    out->n_shift = (uint32_t)(C.nsgrp < 3 ? C.nsgrp : 3);
    memcpy(out->shift, C.sgrp, out->n_shift * sizeof(NsCoverGroup));
    out->col_mean = out->col_n ? (float)(C.col_sum / out->col_n) : 0.0f;
    qsort(C.cgrp, (size_t)C.ncgrp, sizeof(NsCoverGroup), grp_cmp);
    out->n_color = (uint32_t)(C.ncgrp < 3 ? C.ncgrp : 3);
    memcpy(out->color, C.cgrp, out->n_color * sizeof(NsCoverGroup));
    qsort(C.rgrp, (size_t)C.nrgrp, sizeof(NsCoverGroup), grp_cmp);
    out->n_extra = (uint32_t)(C.nrgrp < 3 ? C.nrgrp : 3);
    memcpy(out->extra, C.rgrp, out->n_extra * sizeof(NsCoverGroup));

    free(C.nt); free(C.stamp); free(C.used); free(C.g.head); free(C.g.next);
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    out->ms = (double)(ts1.tv_sec - ts0.tv_sec) * 1e3 + (double)(ts1.tv_nsec - ts0.tv_nsec) * 1e-6;
    return 0;
}

/* ---------------------------------------------------------------------------------------------------------------- completo */
struct NsCover { NsScene sc; NsMeshStore *st; NsFxScene *fx; int ok; };

NsCover *ns_cover_create(void) {
    NsCover *c = (NsCover *)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->st = ns_store_create();
    if (!c->st || ns_scene_alloc(&c->sc, NS_MIN_INST, 8192, NS_MIN_LIGHT, 0)) { ns_cover_destroy(c); return NULL; }
    ns_store_set_limit(c->st, 96u * 1024u * 1024u);
    c->ok = 1;
    return c;
}
void ns_cover_destroy(NsCover *c) {
    if (!c) return;
    if (c->ok) ns_scene_free(&c->sc);
    if (c->st) ns_store_destroy(c->st);
    if (c->fx) ns_fx_scene_destroy(c->fx);
    free(c);
}

int ns_cover_run(NsCover *c, const uint8_t *ram, const uint8_t *scratch, const GpuOp *ops, uint32_t nops, const NsCoverCfg *cfg, NsCoverResult *out) {
    if (!c || !c->ok || !ram || !out) return -1;
    memset(out, 0, sizeof *out);
    if (ns_extract_ex(&c->sc, c->st, ram, NS_RAM_SIZE, scratch, scratch ? 1024u : 0u, NULL, NULL) != 0) return -2;
    ns_store_ack(c->st, ns_store_epoch(c->st));   /* único consumidor: nunca retiene mallas de épocas viejas */
    NsAdaptOpts ao;
    memset(&ao, 0, sizeof ao);
    NsAdaptSrc *src = NULL;
    ao.src_out = &src; ao.no_tint = 1; ao.exposure = 1.0f;   /* colores reales de vértice (sin tinte) para compararlos con los de la lista */
    NsRTri *tr = NULL; NsAdaptStats as;
    size_t n = ns_adapt_scene(&c->sc, c->st, &ao, &tr, &as);
    NsCoverCam cam;
    memset(&cam, 0, sizeof cam);
    /* La lista se emitió con la cámara de un fotograma ANTES que la RAM del gancho: con la de la lista las mallas y los efectos se miden contra la misma vista. */
    const float *R = (cfg && cfg->list_eye && cfg->list_M) ? cfg->list_M : c->sc.cam.rot, *E = (cfg && cfg->list_eye && cfg->list_M) ? cfg->list_eye : c->sc.cam.eye;   /* vista -> mundo; mundo -> vista = R^T (p - eye) */
    for (int r = 0; r < 3; r++) for (int k = 0; k < 3; k++) cam.A[3 * r + k] = R[3 * k + r];
    for (int r = 0; r < 3; r++) cam.t[r] = -(cam.A[3 * r] * E[0] + cam.A[3 * r + 1] * E[1] + cam.A[3 * r + 2] * E[2]);
    cam.fog_far = c->sc.fog.active ? c->sc.fog.start + c->sc.fog.len : 0.0f;
    /* Efectos: lo que el motor genera aparte de las mallas (ondas de agua, carteles, destellos...) se extrae de la misma RAM; las operaciones de la lista que
     * casan con un elemento (exacto o tolerante, ver ns_fx_classify_near) se cuentan como cubiertas por él y no entran en la comparación con las mallas. */
    uint8_t *fxcls = NULL, *fxcls0 = NULL; int32_t *fxit = NULL; GpuOp *rest = NULL;
    NsFxListCam fxlc; const NsFxListCam *fxplc = NULL; int fx_done = 0;
    uint32_t nrest = 0, f_tris = 0, f_water = 0, f_sprite = 0, f_other = 0;
    if (nops) {
        if (!c->fx) c->fx = ns_fx_scene_create(0);
        fxcls = (uint8_t *)calloc(nops, 1); fxcls0 = (uint8_t *)calloc(nops, 1); fxit = (int32_t *)malloc(sizeof(int32_t) * nops); rest = (GpuOp *)malloc(sizeof(GpuOp) * nops);
    }
    GpuDrawList dl;
    memset(&dl, 0, sizeof dl);
    if (c->fx && fxcls && fxit && rest) {
        NsFxOpts fo = ns_fx_opts_default();
        if (ns_fx_extract(ram, &fo, c->fx) == 0) {
            dl.ops = (GpuOp *)ops; dl.nops = nops;
            for (uint32_t i = 0; i < nops; i++) fxit[i] = -1;
            if (ns_fx_classify(ram, c->fx, &dl, fxcls, NULL, fxit, NULL) == 0) {
                if (cfg && cfg->list_eye && cfg->list_M) { memcpy(fxlc.eye, cfg->list_eye, sizeof fxlc.eye); memcpy(fxlc.M, cfg->list_M, sizeof fxlc.M); fxplc = &fxlc; }
                if (fxcls0) memcpy(fxcls0, fxcls, nops);
                ns_fx_classify_near(c->fx, fxplc, &dl, fxcls, fxit);
                fx_done = 1;
            } else memset(fxcls, 0, nops);
        }
    }
    for (uint32_t i = 0; i < nops; i++) {
        const GpuOp *o = &ops[i];
        int isfx = fxcls && (fxcls[i] == NSFX_OP_FX || fxcls[i] == NSFX_OP_FX_PACK) && o->type == GPU_OP_POLY && o->nv >= 3;
        if (isfx) { for (int k = 0; k < o->nv; k++) if (!o->v[k].vvalid) isfx = 0; }
        if (!isfx) { if (rest) rest[nrest++] = *o; continue; }
        uint32_t t = o->nv == 4 ? 2u : 1u;
        const NsFxItem *it = (fxit && fxit[i] >= 0 && (uint32_t)fxit[i] < c->fx->n_items) ? &c->fx->items[fxit[i]] : NULL;
        f_tris += t;
        if (it && it->role == NSFX_ROLE_RIPPLE) f_water += t;
        else if (it && (it->shape == NSFX_SH_BILLBOARD || it->role == NSFX_ROLE_BUBBLE || it->role == NSFX_ROLE_LIGHT || it->role == NSFX_ROLE_SPRITE_ANIM)) f_sprite += t;
        else f_other += t;
    }
    int rc = ns_cover_core(tr, src, n, &cam, rest ? rest : ops, rest ? nrest : nops, cfg, out);
    if (rc == 0 && f_tris) {
        out->list_ops = nops;
        out->list_tris += f_tris;
        out->fx_tris = f_tris; out->fx_water = f_water; out->fx_sprite = f_sprite; out->fx_other = f_other;
        uint32_t cov = 0;
        for (int k = 0; k < NC_N; k++) if (k == NC_MATCH || k == NC_SPLIT || k == NC_SUBDIV) cov += out->cause[k][0] + out->cause[k][1];
        out->cover_pct = 100.0f * (float)(cov + f_tris) / (float)out->list_tris;
        out->exact_pct = 100.0f * (float)(out->cause[NC_MATCH][0] + out->cause[NC_MATCH][1]) / (float)out->list_tris;
    }
    if (rc == 0 && fx_done && fxcls && fxit) out->fxc_ok = ns_fxcmp_run(c->fx, fxplc, c->sc.to_vr, &dl, fxcls, fxit, fxcls0, &out->fxc) == 0;
    free(fxcls); free(fxcls0); free(fxit); free(rest);
    free(tr); free(src);
    return rc;
}

/* ---------------------------------------------------------------------------------------------------------------- texto */
size_t ns_cover_format(const NsCoverResult *r, uint32_t frame, char *buf, size_t cap) {
    if (!buf || !cap) return 0;
    size_t o = 0;
#define APP(...) do { if (o < cap) { int w_ = snprintf(buf + o, cap - o, __VA_ARGS__); if (w_ > 0) o += (size_t)w_ < cap - o ? (size_t)w_ : cap - o - 1; } } while (0)
    uint32_t no_cov[NC_N];
    for (int c = 0; c < NC_N; c++) no_cov[c] = r->cause[c][0] + r->cause[c][1];
    APP("[cobertura] fr=%u lista: tris3D=%u (poligonos=%u sin_vista=%u degenerados=%u) cubierto=%.1f%% (opacos %.1f%%, exacto %.1f%%) | cubierto_por: exacto=%u otra_diagonal=%u subdividido=%u efectos=%u (agua=%u sprites=%u otros=%u) | SIN_CUBRIR: pose=%u otra_textura=%u parcial=%u ausente=%u sprite=%u (semi: pose=%u otra_tex=%u parcial=%u ausente=%u sprite=%u) | textura_distinta(pagina/clut/uv)=%u/%u/%u | nativo: tris=%u sin_pareja=%u (detras=%u plano_cercano=%u fuera=%u reverso=%u lejos=%u objeto_no_visible=%u hardware_descarta_grande=%u de_mas=%u) | posicion(match): error_medio=%.2fpx desplazados_>2px=%u | color(match): comparados=%u distintos=%u error_medio=%.1f | %.1f ms\n",
        frame, r->list_tris, r->list_polys3d, r->list_no_view, r->list_degenerate, r->cover_pct, r->cover_opaque_pct, r->exact_pct,
        no_cov[NC_MATCH], no_cov[NC_SPLIT], no_cov[NC_SUBDIV], r->fx_tris, r->fx_water, r->fx_sprite, r->fx_other,
        no_cov[NC_POSE], no_cov[NC_OTHERTEX], no_cov[NC_PARTIAL], no_cov[NC_ABSENT], no_cov[NC_BILLBOARD],
        r->cause[NC_POSE][1], r->cause[NC_OTHERTEX][1], r->cause[NC_PARTIAL][1], r->cause[NC_ABSENT][1], r->cause[NC_BILLBOARD][1],
        r->tex_page, r->tex_clut, r->tex_uv, r->nat_tris, r->nat_tris - r->nat_used,
        r->rev[NR_BEHIND], r->rev[NR_NEAR], r->rev[NR_OFF], r->rev[NR_BACK], r->rev[NR_FAR], r->rev[NR_HIDDEN], r->rev[NR_BIG], r->rev[NR_EXTRA], r->shift_mean_px, r->shift_bad, r->col_n, r->col_bad, r->col_mean, r->ms);
    {   /* por CLASE: mallas opacas, mallas semitransparentes, efectos (ns_fx: agua / carteles y sprites / resto) y jugador (pose: brazos con IK, la lista manda) */
        uint32_t mo = 0, ms = 0, to = 0, ts = 0;
        for (int c = 0; c < NC_N; c++) { to += r->cause[c][0]; ts += r->cause[c][1]; }
        mo = r->cause[NC_MATCH][0] + r->cause[NC_SPLIT][0] + r->cause[NC_SUBDIV][0];
        ms = r->cause[NC_MATCH][1] + r->cause[NC_SPLIT][1] + r->cause[NC_SUBDIV][1];
        uint32_t pose = r->cause[NC_POSE][0] + r->cause[NC_POSE][1];
        uint32_t tot = to + ts + r->fx_tris, cov = mo + ms + r->fx_tris;
        uint32_t tot_np = tot - pose;
        APP("[cobertura] clases: mallas_opacas=%u/%u (%.1f%%) mallas_semi=%u/%u (%.1f%%) efectos=%u/%u (agua=%u sprites=%u otros=%u) jugador_pose=%u | total=%u/%u (%.1f%%) sin_pose=%u/%u (%.1f%%)\n",
            mo, to, to ? 100.0 * mo / to : 100.0, ms, ts, ts ? 100.0 * ms / ts : 100.0, r->fx_tris, r->fx_tris, r->fx_water, r->fx_sprite, r->fx_other, pose,
            cov, tot, tot ? 100.0 * cov / tot : 100.0, cov, tot_np, tot_np ? 100.0 * cov / tot_np : 100.0);
    }
    for (uint32_t i = 0; i < r->n_worst; i++) {
        const NsCoverGroup *g = &r->worst[i];
        APP("[cobertura]   peor %u: %s%s n=%u objeto=%08x pieza=%u malla=%08x tpage=%04x clut=%04x zmin=%.0f\n", i + 1, ns_cover_cause_name[g->cause], g->semi ? "(semi)" : "", g->n, g->key, g->piece, g->mesh, g->tpage, g->clut, g->zmin);
    }
    for (uint32_t i = 0; i < r->n_shift; i++) {
        const NsCoverGroup *g = &r->shift[i];
        APP("[cobertura]   desplazado %u: n=%u objeto=%08x pieza=%u malla=%08x tpage=%04x clut=%04x zmin=%.0f\n", i + 1, g->n, g->key, g->piece, g->mesh, g->tpage, g->clut, g->zmin);
    }
    for (uint32_t i = 0; i < r->n_color; i++) {
        const NsCoverGroup *g = &r->color[i];
        APP("[cobertura]   color_distinto %u: n=%u objeto=%08x pieza=%u malla=%08x tpage=%04x clut=%04x zmin=%.0f\n", i + 1, g->n, g->key, g->piece, g->mesh, g->tpage, g->clut, g->zmin);
    }
    for (uint32_t i = 0; i < r->n_extra; i++) {
        const NsCoverGroup *g = &r->extra[i];
        APP("[cobertura]   nativo_de_mas %u: n=%u objeto=%08x pieza=%u malla=%08x tpage=%04x clut=%04x zmin=%.0f\n", i + 1, g->n, g->key, g->piece, g->mesh, g->tpage, g->clut, g->zmin);
    }
    if (r->fxc_ok) o += ns_fxcmp_format(&r->fxc, frame, buf + o, cap - o);
#undef APP
    return o;
}
