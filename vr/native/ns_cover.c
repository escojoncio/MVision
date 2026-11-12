/* ns_cover: ver ns_cover.h. */
#include "ns_cover.h"
#include "ns_extract.h"
#include "ns_fx_export.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
            const NTri *pose_nat = NULL;   /* solo diagnóstico: nativo con la misma textura/UV de una POSE */
            if (on_same == 3) {
                cause = NC_SUBDIV; owner = own_same;
                for (int k = 0; k < 3; k++) if (ow[k] >= 0) C.used[ow[k]] = 1;
            } else if (fabsf(q[0][2] - q[1][2]) < 2.0f && fabsf(q[0][2] - q[2][2]) < 2.0f && on_any == 0) {   /* rectángulo a profundidad constante sin nada debajo */
                cause = NC_BILLBOARD;
            } else {
                /* misma textura y UV en otro sitio: solo vale como «otra pose» si el nativo es de un objeto DINÁMICO (animación, IK). El escenario no cambia de pose: con una textura repetida casaba con
                 * cualquier pared a cualquier distancia y ocultaba tanto geometría ausente como una cámara de la lista distinta de la medida (17027 triángulos «pose» de dueños estáticos en una sesión). */
                int uj = -1; double ub = 1e30;
                if (textured) {
                    for (size_t j = 0; j < n; j++) {
                        const NTri *t = &C.nt[j];
                        if ((src[j].iflags & NS_IF_STATIC) || !same_tex(t, o, 1)) continue;
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
                if (uj >= 0) { cause = NC_POSE; owner = uj; C.used[uj] = 1; pose_nat = &C.nt[uj]; }
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
            if (cfg.dbg && pose_nat) fprintf((FILE *)cfg.dbg, "  POSE_NATIVO centro=(%.0f,%.0f,%.0f) vertices=(%.0f,%.0f,%.0f)(%.0f,%.0f,%.0f)(%.0f,%.0f,%.0f)\n", pose_nat->c[0], pose_nat->c[1], pose_nat->c[2], pose_nat->p[0][0], pose_nat->p[0][1], pose_nat->p[0][2], pose_nat->p[1][0], pose_nat->p[1][1], pose_nat->p[1][2], pose_nat->p[2][0], pose_nat->p[2][1], pose_nat->p[2][2]);
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

/* ---------------------------------------------------------------------------------------------------------------- verificador geométrico */
const char *const ns_verif_cls_name[NV_N] = {"estatico", "dinamico", "prim", "sortonly", "fuera_grupo", "otros"};

#define VCELL 512.0f
#define VBIG 512          /* un triángulo que ocupa más celdas va a la lista de grandes (fuerza bruta) */
typedef struct { float p[3][3]; float lo[3], hi[3]; } VTri;
typedef struct { int32_t *head; uint32_t mask; int32_t *enext, *etri; size_t ne, ecap; int32_t *big; size_t nbig; int32_t *stamp; int32_t qid; const VTri *t; size_t n; } VGrid;
static int32_t vcell(float v) { float c = floorf(v / VCELL); return (int32_t)(c < -1e6f ? -1e6f : c > 1e6f ? 1e6f : c); }
static void vgrid_free(VGrid *g) { free(g->head); free(g->enext); free(g->etri); free(g->big); free(g->stamp); memset(g, 0, sizeof *g); }
static int vgrid_build(VGrid *g, const VTri *t, size_t n) {
    memset(g, 0, sizeof *g);
    g->t = t; g->n = n;
    size_t nb = 256; while (nb < n * 4) nb <<= 1;
    g->head = (int32_t *)malloc(nb * sizeof(int32_t)); g->big = (int32_t *)malloc((n ? n : 1) * sizeof(int32_t)); g->stamp = (int32_t *)calloc(n ? n : 1, sizeof(int32_t));
    if (!g->head || !g->big || !g->stamp) { vgrid_free(g); return -1; }
    g->mask = (uint32_t)(nb - 1);
    for (size_t i = 0; i < nb; i++) g->head[i] = -1;
    for (size_t i = 0; i < n; i++) {
        int32_t a[3], b[3]; int64_t cnt = 1;
        for (int k = 0; k < 3; k++) { a[k] = vcell(t[i].lo[k]); b[k] = vcell(t[i].hi[k]); cnt *= (int64_t)(b[k] - a[k] + 1); }
        if (cnt > VBIG) { g->big[g->nbig++] = (int32_t)i; continue; }
        for (int32_t x = a[0]; x <= b[0]; x++) for (int32_t y = a[1]; y <= b[1]; y++) for (int32_t z = a[2]; z <= b[2]; z++) {
            if (g->ne == g->ecap) {
                size_t nc = g->ecap ? g->ecap * 2 : 4096;
                int32_t *en = (int32_t *)realloc(g->enext, nc * sizeof(int32_t)), *et = en ? (int32_t *)realloc(g->etri, nc * sizeof(int32_t)) : NULL;
                if (en) g->enext = en;
                if (et) g->etri = et;
                if (!en || !et) { vgrid_free(g); return -1; }
                g->ecap = nc;
            }
            uint32_t h = hcell(x, y, z) & g->mask;
            g->etri[g->ne] = (int32_t)i; g->enext[g->ne] = g->head[h]; g->head[h] = (int32_t)g->ne++;
        }
    }
    return 0;
}
/* distancia del punto a la superficie nativa más cercana; corta en cuanto encuentra una a <= tol (devuelve > tol si no hay ninguna) */
static double vgrid_dist(VGrid *g, const float *P, float tol) {
    double best = 1e30;
    int32_t qid = ++g->qid, a[3], b[3];
    for (int k = 0; k < 3; k++) { a[k] = vcell(P[k] - tol); b[k] = vcell(P[k] + tol); }
    for (int32_t x = a[0]; x <= b[0]; x++) for (int32_t y = a[1]; y <= b[1]; y++) for (int32_t z = a[2]; z <= b[2]; z++)
        for (int32_t e = g->head[hcell(x, y, z) & g->mask]; e >= 0; e = g->enext[e]) {
            int32_t ti = g->etri[e];
            if (g->stamp[ti] == qid) continue;
            g->stamp[ti] = qid;
            const VTri *t = &g->t[ti];
            if (P[0] < t->lo[0] - tol || P[0] > t->hi[0] + tol || P[1] < t->lo[1] - tol || P[1] > t->hi[1] + tol || P[2] < t->lo[2] - tol || P[2] > t->hi[2] + tol) continue;
            double d = pt_tri(P, t->p);
            if (d < best) { best = d; if (best <= tol) return best; }
        }
    for (size_t i = 0; i < g->nbig; i++) {
        const VTri *t = &g->t[g->big[i]];
        if (P[0] < t->lo[0] - tol || P[0] > t->hi[0] + tol || P[1] < t->lo[1] - tol || P[1] > t->hi[1] + tol || P[2] < t->lo[2] - tol || P[2] > t->hi[2] + tol) continue;
        double d = pt_tri(P, t->p);
        if (d < best) { best = d; if (best <= tol) return best; }
    }
    return best;
}
static void vtri_set(VTri *t, const float *a, const float *b, const float *c) {
    memcpy(t->p[0], a, 12); memcpy(t->p[1], b, 12); memcpy(t->p[2], c, 12);
    for (int k = 0; k < 3; k++) {
        t->lo[k] = fminf(a[k], fminf(b[k], c[k])); t->hi[k] = fmaxf(a[k], fmaxf(b[k], c[k]));
    }
}

static const NsVerifOwner *vown_find(const NsVerifOwner *own, size_t n, uint32_t pkt) {
    uint32_t a = pkt & 0x1FFFFFFFu;
    if (!a || !n) return NULL;
    size_t lo = 0, hi = n;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (own[m].lo <= a) lo = m + 1; else hi = m; }
    return (lo && a < own[lo - 1].hi) ? &own[lo - 1] : NULL;
}

/* grupos de triángulos nativos por pieza (ns_adapt los emite contiguos) */
typedef struct { uint32_t key, piece, first, n, weld; } VPiece;
static const VPiece *vpiece_find(const VPiece *pg, size_t n, uint32_t key, uint32_t piece) {
    for (size_t i = 0; i < n; i++) if (pg[i].piece == piece && (pg[i].key & 0x1FFFFFFFu) == (key & 0x1FFFFFFFu)) return &pg[i];
    return NULL;
}

/* Cámara con su inversa exacta: la matriz de ojo del motor no es ortonormal (lleva una escala de ~1,0075 en los volcados medidos), así que la traspuesta no vale como inversa. */
typedef struct { float A[9], t[3], Ai[9]; } VCam;
static int vcam_make(VCam *o, const float *A, const float *t) {
    const double a = A[0], b = A[1], c = A[2], d = A[3], e = A[4], f = A[5], g = A[6], h = A[7], i = A[8];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!(fabs(det) > 1e-9)) return -1;
    const double inv[9] = {(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det, (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det, (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det};
    for (int k = 0; k < 9; k++) { o->A[k] = A[k]; o->Ai[k] = (float)inv[k]; }
    for (int k = 0; k < 3; k++) o->t[k] = t[k];
    return 0;
}
static void vcam_world(const VCam *c, const float *v, float *w) {   /* vista (sin factor vertical) -> mundo */
    const float d[3] = {v[0] - c->t[0], v[1] - c->t[1], v[2] - c->t[2]};
    for (int k = 0; k < 3; k++) w[k] = c->Ai[3 * k] * d[0] + c->Ai[3 * k + 1] * d[1] + c->Ai[3 * k + 2] * d[2];
}
typedef struct { float v[3]; const VPiece *pg; } VSamp;
/* cuántas muestras (vértices de la lista con dueño estático) caen sobre un vértice de su propia pieza nativa con esta cámara */
/* tolerancia de una muestra: crece con la profundidad (precisión de 12 bits de las matrices del motor); `k` la relaja para las hipótesis rígidas (la cámara del motor lleva una escala que una rígida no modela) */
static float vsamp_tol(const VSamp *s, float k) { return k * (20.0f + 0.004f * (s->v[2] > 0 ? s->v[2] : 0)); }
static uint32_t vcam_score(const VCam *c, const VSamp *s, size_t ns, size_t stride, const NsRTri *tris, float k) {
    uint32_t in = 0;
    for (size_t i = 0; i < ns; i += stride) {
        float w[3]; vcam_world(c, s[i].v, w);
        const float tol = vsamp_tol(&s[i], k);
        const VPiece *g = s[i].pg; int hit = 0;
        for (uint32_t j = 0; j < g->n && !hit; j++) for (int k = 0; k < 3 && !hit; k++) hit = d3(w, tris[g->first + j].v[k].p) <= tol;
        in += (uint32_t)hit;
    }
    return in;
}
static void vnorm(double *a) { double l = sqrt(dot3(a, a)); if (l > 1e-12) { a[0] /= l; a[1] /= l; a[2] /= l; } }
/* cámara rígida que lleva el triángulo de mundo (a,b,c) al de vista (A,B,C): marcos ortonormales de cada uno */
static int vcam_from_tri(const float w[3][3], const float v[3][3], VCam *c) {
    double fw[3][3], fv[3][3], e[3], f[3];
    sub3(w[1], w[0], e); sub3(w[2], w[0], f); crs(e, f, fw[2]);
    if (dot3(fw[2], fw[2]) < 1e-6) return -1;
    vnorm(e); vnorm(fw[2]); memcpy(fw[0], e, sizeof e); crs(fw[2], fw[0], fw[1]);
    sub3(v[1], v[0], e); sub3(v[2], v[0], f); crs(e, f, fv[2]);
    if (dot3(fv[2], fv[2]) < 1e-6) return -1;
    vnorm(e); vnorm(fv[2]); memcpy(fv[0], e, sizeof e); crs(fv[2], fv[0], fv[1]);
    for (int r = 0; r < 3; r++) for (int k = 0; k < 3; k++) c->A[3 * r + k] = (float)(fv[0][r] * fw[0][k] + fv[1][r] * fw[1][k] + fv[2][r] * fw[2][k]);
    for (int r = 0; r < 3; r++) c->t[r] = v[0][r] - (c->A[3 * r] * w[0][0] + c->A[3 * r + 1] * w[0][1] + c->A[3 * r + 2] * w[0][2]);
    for (int r = 0; r < 3; r++) for (int k = 0; k < 3; k++) c->Ai[3 * r + k] = c->A[3 * k + r];
    return 0;
}
/* mejor AFÍN por mínimos cuadrados de los pares mundo -> vista (v = A w + t): la matriz de ojo del motor no es ortonormal. -1 si los puntos no determinan las 12 incógnitas (coplanares). */
static int vcam_fit_affine(const float (*w)[3], const float (*v)[3], size_t n, VCam *c) {
    if (n < 8) return -1;
    double S[4][4] = {{0}}, B[3][4] = {{0}}, cw[3] = {0, 0, 0};
    for (size_t i = 0; i < n; i++) for (int k = 0; k < 3; k++) cw[k] += w[i][k] / (double)n;
    for (size_t i = 0; i < n; i++) {
        const double h[4] = {w[i][0] - cw[0], w[i][1] - cw[1], w[i][2] - cw[2], 1.0};
        for (int a = 0; a < 4; a++) { for (int b = 0; b < 4; b++) S[a][b] += h[a] * h[b]; for (int r = 0; r < 3; r++) B[r][a] += v[i][r] * h[a]; }
    }
    double M[4][7];
    for (int a = 0; a < 4; a++) { for (int b = 0; b < 4; b++) M[a][b] = S[a][b]; for (int r = 0; r < 3; r++) M[a][4 + r] = B[r][a]; }
    const double scale = S[0][0] + S[1][1] + S[2][2];
    for (int col = 0; col < 4; col++) {
        int piv = col;
        for (int r = col + 1; r < 4; r++) if (fabs(M[r][col]) > fabs(M[piv][col])) piv = r;
        if (fabs(M[piv][col]) < 1e-4 * (col < 3 ? scale / 3.0 : 1.0) + 1e-9) return -1;
        if (piv != col) for (int k = 0; k < 7; k++) { double t = M[col][k]; M[col][k] = M[piv][k]; M[piv][k] = t; }
        for (int r = 0; r < 4; r++) if (r != col) { const double f = M[r][col] / M[col][col]; for (int k = col; k < 7; k++) M[r][k] -= f * M[col][k]; }
    }
    float A[9], t[3];
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 3; k++) A[3 * r + k] = (float)(M[k][4 + r] / M[k][k]);
        t[r] = (float)(M[3][4 + r] / M[3][3] - (A[3 * r] * cw[0] + A[3 * r + 1] * cw[1] + A[3 * r + 2] * cw[2]));
    }
    return vcam_make(c, A, t);
}
/* mejor rígida por mínimos cuadrados (Horn, cuaternión por iteración de potencias) de los pares mundo -> vista */
static int vcam_fit(const float (*w)[3], const float (*v)[3], size_t n, VCam *c) {
    if (n < 4) return -1;
    double cw[3] = {0, 0, 0}, cv[3] = {0, 0, 0}, M[3][3] = {{0}};
    for (size_t i = 0; i < n; i++) for (int k = 0; k < 3; k++) { cw[k] += w[i][k]; cv[k] += v[i][k]; }
    for (int k = 0; k < 3; k++) { cw[k] /= (double)n; cv[k] /= (double)n; }
    for (size_t i = 0; i < n; i++) for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) M[a][b] += (w[i][a] - cw[a]) * (v[i][b] - cv[b]);
    double N[4][4] = {
        {M[0][0] + M[1][1] + M[2][2], M[1][2] - M[2][1], M[2][0] - M[0][2], M[0][1] - M[1][0]},
        {M[1][2] - M[2][1], M[0][0] - M[1][1] - M[2][2], M[0][1] + M[1][0], M[2][0] + M[0][2]},
        {M[2][0] - M[0][2], M[0][1] + M[1][0], -M[0][0] + M[1][1] - M[2][2], M[1][2] + M[2][1]},
        {M[0][1] - M[1][0], M[2][0] + M[0][2], M[1][2] + M[2][1], -M[0][0] - M[1][1] + M[2][2]}};
    double sh = 0;
    for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) sh += fabs(N[a][b]);
    if (!(sh > 0)) return -1;
    double q[4] = {1, 0.01, 0.02, 0.03};
    for (int it = 0; it < 200; it++) {
        double r[4], l = 0;
        for (int a = 0; a < 4; a++) { r[a] = sh * q[a]; for (int b = 0; b < 4; b++) r[a] += N[a][b] * q[b]; l += r[a] * r[a]; }
        l = sqrt(l); if (!(l > 0)) return -1;
        for (int a = 0; a < 4; a++) q[a] = r[a] / l;
    }
    const double q0 = q[0], qx = q[1], qy = q[2], qz = q[3];
    const double R[9] = {q0 * q0 + qx * qx - qy * qy - qz * qz, 2 * (qx * qy - q0 * qz), 2 * (qx * qz + q0 * qy),
                         2 * (qy * qx + q0 * qz), q0 * q0 - qx * qx + qy * qy - qz * qz, 2 * (qy * qz - q0 * qx),
                         2 * (qz * qx - q0 * qy), 2 * (qz * qy + q0 * qx), q0 * q0 - qx * qx - qy * qy + qz * qz};
    for (int k = 0; k < 9; k++) c->A[k] = (float)R[k];
    for (int r = 0; r < 3; r++) c->t[r] = (float)(cv[r] - (R[3 * r] * cw[0] + R[3 * r + 1] * cw[1] + R[3 * r + 2] * cw[2]));
    for (int r = 0; r < 3; r++) for (int k = 0; k < 3; k++) c->Ai[3 * r + k] = c->A[3 * k + r];
    return 0;
}
/* refinado por mínimos cuadrados (afín; rígido si los pares son coplanares) con los pares muestra -> vértice más cercano de su pieza, estrechando la tolerancia de k0 a k1 */
static void vcam_refine(VCam *hb, const VSamp *samp, size_t nsamp, const NsRTri *tris, float k0, float k1) {
    float (*pw)[3] = (float (*)[3])malloc(nsamp * sizeof *pw), (*pv)[3] = (float (*)[3])malloc(nsamp * sizeof *pv);
    if (pw && pv) for (int it = 0; it < 6; it++) {
        const float k = it < 2 ? k0 : it < 4 ? 0.5f * (k0 + k1) : k1;
        size_t np = 0;
        for (size_t i = 0; i < nsamp; i++) {
            float w[3]; vcam_world(hb, samp[i].v, w);
            const VPiece *g = samp[i].pg; double bd = vsamp_tol(&samp[i], k); const float *bp = NULL;
            for (uint32_t j = 0; j < g->n; j++) for (int kk = 0; kk < 3; kk++) { double d = d3(w, tris[g->first + j].v[kk].p); if (d <= bd) { bd = d; bp = tris[g->first + j].v[kk].p; } }
            if (bp) { memcpy(pw[np], bp, 12); memcpy(pv[np], samp[i].v, 12); np++; }
        }
        VCam h;
        if ((vcam_fit_affine(pw, pv, np, &h) == 0 || vcam_fit(pw, pv, np, &h) == 0) && vcam_score(&h, samp, nsamp, 1, tris, k) >= vcam_score(hb, samp, nsamp, 1, tris, k)) *hb = h;
    }
    free(pw); free(pv);
}
static void sort3(double *e) { for (int i = 0; i < 2; i++) for (int j = 0; j < 2 - i; j++) if (e[j] > e[j + 1]) { double t = e[j]; e[j] = e[j + 1]; e[j + 1] = t; } }
static void tri_edges(const float q[3][3], double *e) { e[0] = d3(q[0], q[1]); e[1] = d3(q[1], q[2]); e[2] = d3(q[2], q[0]); }
static int edge_eq(double a, double b) { double t = 0.02 * (a > b ? a : b); if (t < 3.0) t = 3.0; return fabs(a - b) <= t; }
/* ¿tiene la pieza un triángulo nativo congruente con q (mismas tres aristas)? */
static int vpiece_congruent(const VPiece *g, const NsRTri *tris, const float q[3][3]) {
    double e[3]; tri_edges(q, e); sort3(e);
    for (uint32_t j = 0; j < g->n; j++) {
        const NsRTri *t = &tris[g->first + j];
        float p[3][3]; for (int k = 0; k < 3; k++) memcpy(p[k], t->v[k].p, 12);
        double f[3]; tri_edges(p, f); sort3(f);
        if (edge_eq(e[0], f[0]) && edge_eq(e[1], f[1]) && edge_eq(e[2], f[2])) return 1;
    }
    return 0;
}

/* ¿hay superficie nativa en el centro y las tres esquinas del triángulo de vista `vw` llevado al mundo con la cámara `c`? */
static int vtri_on_surface(VGrid *G, const VCam *c, const float vw[3][3], float tol, const float tk[3]) {
    float q[3][3];
    for (int k = 0; k < 3; k++) vcam_world(c, vw[k], q[k]);
    const float ctr[3] = {(q[0][0] + q[1][0] + q[2][0]) / 3.0f, (q[0][1] + q[1][1] + q[2][1]) / 3.0f, (q[0][2] + q[1][2] + q[2][2]) / 3.0f};
    int ok = vgrid_dist(G, ctr, tol) <= tol;
    for (int k = 0; k < 3 && ok; k++) ok = vgrid_dist(G, q[k], tk[k]) <= tk[k];
    return ok;
}
enum { VSAMP_MAX = 900, VHYP_OPS = 48, VHYP_PER_OP = 6 };
int ns_verif_core(const NsRTri *tris, const NsAdaptSrc *src, size_t n, const NsVerifFxTri *fx, size_t nfx, const NsVerifOwner *own, size_t nown,
                  const NsVerifCam *cams, int ncam, float ys, const GpuOp *ops, uint32_t nops, uint32_t max_tris, NsVerif *out) {
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    memset(out, 0, sizeof *out);
    if (!cams || ncam < 1 || (n && (!tris || !src)) || (nfx && !fx) || (nops && !ops)) return -1;
    if (!(ys > 0.0f)) ys = 1.0f;
    if (!max_tris) max_tris = 6000;
    NsCoverCfg cfg; ns_cover_cfg_default(&cfg);
    int rc = -1;
    VTri *vt = (VTri *)malloc((n + nfx + 1) * sizeof(VTri));
    VPiece *pg = (VPiece *)malloc((n + 1) * sizeof(VPiece));
    VSamp *samp = (VSamp *)malloc(VSAMP_MAX * sizeof(VSamp));
    VGrid G; memset(&G, 0, sizeof G);
    size_t nvt = 0, npg = 0, nsamp = 0;
    if (!vt || !pg || !samp) goto done;
    for (size_t i = 0; i < n; i++) {
        vtri_set(&vt[nvt++], tris[i].v[0].p, tris[i].v[1].p, tris[i].v[2].p);
        if (!npg || pg[npg - 1].key != src[i].key || pg[npg - 1].piece != src[i].piece_rel) { pg[npg].key = src[i].key; pg[npg].piece = src[i].piece_rel; pg[npg].first = (uint32_t)i; pg[npg].n = 0; pg[npg].weld = 0; npg++; }
        pg[npg - 1].n++;
        if (src[i].mflags & NS_MF_WELDED) pg[npg - 1].weld = 1;
    }
    for (size_t i = 0; i < nfx; i++) if (!fx[i].nogeo) vtri_set(&vt[nvt++], fx[i].p[0], fx[i].p[1], fx[i].p[2]);
    if (vgrid_build(&G, vt, nvt)) goto done;

    /* ---- pasada 1: recuento y muestras de vértices con dueño estático (para la cámara) ---- */
    uint32_t total = 0;
    for (uint32_t oi = 0; oi < nops; oi++) {
        const GpuOp *o = &ops[oi];
        if (o->type != GPU_OP_POLY || o->nv < 3) continue;
        int vv = 1; for (int k = 0; k < o->nv; k++) if (!o->v[k].vvalid) vv = 0;
        if (vv) total += o->nv == 4 ? 2u : 1u;
    }
    {
        uint32_t seen = 0, want = total / 2u / (VSAMP_MAX / 4u) + 1u;   /* una de cada `want` operaciones con dueño estático aporta sus vértices */
        for (uint32_t oi = 0; oi < nops && nsamp + 4 <= VSAMP_MAX; oi++) {
            const GpuOp *o = &ops[oi];
            if (o->type != GPU_OP_POLY || o->nv < 3) continue;
            int vv = 1; for (int k = 0; k < o->nv; k++) if (!o->v[k].vvalid || (o->v[k].vflags & (GPU_VF_AMBIG | GPU_VF_DERIVED))) vv = 0;
            if (!vv) continue;
            const NsVerifOwner *w = vown_find(own, nown, o->pkt_addr);
            if (!w || w->cls != NV_STATIC) continue;
            const VPiece *g = vpiece_find(pg, npg, w->key, w->piece);
            if (!g || (seen++ % want)) continue;
            for (int k = 0; k < o->nv; k++) { samp[nsamp].v[0] = o->v[k].vx; samp[nsamp].v[1] = o->v[k].vy / ys; samp[nsamp].v[2] = o->v[k].vz; samp[nsamp].pg = g; nsamp++; }
        }
    }
    /* ---- cámara: la mejor candidata; si ninguna explica (casi) todas las muestras, ajuste por consenso ---- */
    VCam cam;
    out->cam = -2; out->cam_pts = (uint32_t)nsamp;
    for (int c = 0; c < ncam && out->cam == -2; c++) if (vcam_make(&cam, cams[c].A, cams[c].t) == 0) out->cam = c;
    if (out->cam == -2) goto done;   /* ninguna cámara candidata invertible */
    if (nsamp >= 12) {
        const float stol = 1.0f, htol = 4.0f;   /* factores de la tolerancia por profundidad: medida / hipótesis rígidas */
        uint32_t best = 0;
        for (int c = 0; c < ncam; c++) { VCam vc; if (vcam_make(&vc, cams[c].A, cams[c].t)) continue; uint32_t s = vcam_score(&vc, samp, nsamp, 1, tris, stol); if (s > best) { best = s; cam = vc; out->cam = c; } }
        if ((float)best < 0.995f * (float)nsamp) {
            /* (a) la candidata era casi buena (la cámara se movió poco entre el fotograma de la lista y el de la RAM): refinado desde ella */
            VCam r = cam;
            vcam_refine(&r, samp, nsamp, tris, htol, stol);
            uint32_t s = vcam_score(&r, samp, nsamp, 1, tris, stol);
            if (s > best) { best = s; cam = r; out->cam = -1; }
        }
        if ((float)best < 0.995f * (float)nsamp) {
            /* (b) otra cámara (corte): hipótesis rígida por cada triángulo congruente de la pieza dueña, consenso y refinado */
            VCam hb = cam; uint32_t hs = 0; int found = 0;
            const size_t stride = nsamp / 120u + 1u, nsub = (nsamp + stride - 1u) / stride;
            const size_t ostep = nsamp / 4u / VHYP_OPS + 1u;
            for (size_t si = 0; si + 2 < nsamp && !found; si += 4u * ostep) {   /* las muestras van de operación en operación (3 o 4 vértices seguidos de la misma pieza) */
                const VPiece *g = samp[si].pg;
                if (samp[si + 1].pg != g || samp[si + 2].pg != g) continue;
                float q[3][3]; for (int k = 0; k < 3; k++) memcpy(q[k], samp[si + (size_t)k].v, 12);
                double e[3]; tri_edges(q, e);
                if (e[0] < 8 || e[1] < 8 || e[2] < 8) continue;
                int tried = 0;
                for (uint32_t j = 0; j < g->n && tried < VHYP_PER_OP && !found; j++) for (int pp = 0; pp < 6 && tried < VHYP_PER_OP && !found; pp++) {
                    float wp[3][3]; for (int k = 0; k < 3; k++) memcpy(wp[k], tris[g->first + j].v[PERMS[pp][k]].p, 12);
                    double f[3]; tri_edges(wp, f);
                    if (!edge_eq(e[0], f[0]) || !edge_eq(e[1], f[1]) || !edge_eq(e[2], f[2])) continue;
                    VCam h;
                    if (vcam_from_tri(wp, q, &h)) continue;
                    tried++;
                    uint32_t s = vcam_score(&h, samp, nsamp, stride, tris, htol);
                    if (s > hs) { hs = s; hb = h; if ((float)s >= 0.9f * (float)nsub) found = 1; }
                }
            }
            if (hs * 2u >= nsub && hs >= 6) {
                vcam_refine(&hb, samp, nsamp, tris, htol, stol);
                uint32_t s = vcam_score(&hb, samp, nsamp, 1, tris, stol);
                if (s > best) { best = s; cam = hb; out->cam = -1; }
            }
        }
        out->cam_in = (float)best / (float)nsamp;
    }
    /* ---- honradez: la cámara usada frente a la de la app (última candidata) ---- */
    VCam trust[8]; int ntrust = 0;   /* cámaras con las que un triángulo vale como equivalente */
    {
        int near_any = 0;   /* la usada coincide (dentro de tolerancia) con alguna candidata: la de la app o la registrada con la lista */
        for (int c = 0; c < ncam; c++) {
            VCam k;
            if (vcam_make(&k, cams[c].A, cams[c].t)) continue;
            float eu[3], ek[3], giro = 0.0f; const float z3[3] = {0, 0, 0};
            vcam_world(&cam, z3, eu); vcam_world(&k, z3, ek);
            const float dist = (float)d3(eu, ek);
            for (int r = 0; r < 3; r++) {   /* giro: el mayor ángulo entre ejes homólogos (filas de mundo -> vista) */
                const double u[3] = {cam.A[3 * r], cam.A[3 * r + 1], cam.A[3 * r + 2]}, a[3] = {k.A[3 * r], k.A[3 * r + 1], k.A[3 * r + 2]};
                const double lu = sqrt(dot3(u, u)), la = sqrt(dot3(a, a));
                double cs = (lu > 0 && la > 0) ? dot3(u, a) / (lu * la) : 1.0; if (cs > 1) cs = 1; if (cs < -1) cs = -1;
                const float g = (float)(acos(cs) * 180.0 / M_PI);
                if (g > giro) giro = g;
            }
            if (dist <= NV_CAM_DIST_TOL && giro <= NV_CAM_ROT_TOL) near_any = 1;
            if (c == ncam - 1) {   /* la de la app */
                out->cam_dist = dist; out->cam_giro = giro;
                if (nsamp) out->app_in = (float)vcam_score(&k, samp, nsamp, 1, tris, 1.0f) / (float)nsamp;
            }
        }
        out->cam_distinct = out->cam == -1 && !near_any;
        if (!out->cam_distinct) trust[ntrust++] = cam;
        else for (int c = 0; c < ncam && ntrust < 8; c++) if (vcam_make(&trust[ntrust], cams[c].A, cams[c].t) == 0) ntrust++;
    }
    memcpy(out->cam_A, cam.A, sizeof out->cam_A); memcpy(out->cam_t, cam.t, sizeof out->cam_t);
    const int cam_ok = out->cam_in >= 0.5f;   /* la cámara explica la lista (hay muestras de dueño estático y la mayoría caen en su pieza) */

    /* ---- pasada 2: cada triángulo GTE contra la superficie nativa ---- */
    struct { uint32_t key, n; uint8_t cls; } worst[64]; int nworst = 0;
    out->step = total > max_tris ? (total + max_tris - 1u) / max_tris : 1u;
    uint32_t cnt = 0;
    for (uint32_t oi = 0; oi < nops; oi++) {
        const GpuOp *o = &ops[oi];
        if (o->type != GPU_OP_POLY || o->nv < 3) continue;
        const NsVerifOwner *w = vown_find(own, nown, o->pkt_addr);
        int vv = 1, amb = 0;
        for (int k = 0; k < o->nv; k++) { if (!o->v[k].vvalid) vv = 0; else if (o->v[k].vflags & (GPU_VF_AMBIG | GPU_VF_DERIVED)) amb = 1; }
        const uint32_t nt = o->nv == 4 ? 2u : 1u;
        if (!vv) {   /* sin posición de vista: solo cuenta la primitiva de solo ordenación de la cola (cielo) que el nativo no genera */
            if (!w) { out->no_view_2d++; continue; }
            if (w->cls == NV_STATIC) out->no_view_static += nt;
            if (w->cls != NV_SORTONLY) continue;
            int gen = 0;
            for (size_t i = 0; i < nfx && !gen; i++) gen = (fx[i].key & 0x1FFFFFFFu) == (w->key & 0x1FFFFFFFu);
            if (!gen) {
                out->cls[NV_SORTONLY] += nt; out->missing += nt;
                int wi; for (wi = 0; wi < nworst; wi++) if (worst[wi].key == w->key) break;
                if (wi == nworst && nworst < 64) { worst[nworst].key = w->key; worst[nworst].n = 0; worst[nworst].cls = NV_SORTONLY; nworst++; }
                if (wi < nworst) worst[wi].n += nt;
            }
            continue;
        }
        for (uint32_t h = 0; h < nt; h++) {
            if (cnt++ % out->step) continue;
            if (amb) { out->ambig++; continue; }
            const int ci[3] = {h ? 1 : 0, h ? 2 : 1, h ? 3 : 2};
            if (!w && !cam_ok) { out->unverified++; continue; }   /* sin dueño y con una cámara que no explica la lista: ni equivalente ni ausente */
            float q[3][3], vw[3][3], z = 0, tk[3];   /* tolerancia por esquina según SU profundidad (un triángulo que cruza la cámara tiene esquinas a profundidades muy distintas) */
            for (int k = 0; k < 3; k++) { vw[k][0] = o->v[ci[k]].vx; vw[k][1] = o->v[ci[k]].vy / ys; vw[k][2] = o->v[ci[k]].vz; vcam_world(&trust[0], vw[k], q[k]); tk[k] = tol_at(&cfg, fabsf(vw[k][2])); z += fabsf(vw[k][2]) / 3.0f; }
            if (d3(q[0], q[1]) < 2 || d3(q[0], q[2]) < 2 || d3(q[1], q[2]) < 2) continue;
            out->tris_gte++;
            if (!w) out->no_owner++;
            const float tol = tol_at(&cfg, z);
            int ok = 0;
            for (int t = 0; t < ntrust && !ok; t++) ok = vtri_on_surface(&G, &trust[t], vw, tol, tk);
            if (ok) continue;
            if (out->cam_distinct && vtri_on_surface(&G, &cam, vw, tol, tk)) { out->only_fit++; out->unverified++; continue; }   /* solo casa con la ajustada distinta: ni equivalente ni ausente */
            int cls = w ? w->cls : NV_OTHER;
            if (cls == NV_SORTONLY) cls = NV_PRIM;   /* una primitiva de solo ordenación no pasa por la GTE: si llega aquí es una primitiva más */
            if (cls == NV_DYNAMIC) {
                const VPiece *g = vpiece_find(pg, npg, w->key, w->piece);
                int near = 0;   /* pieza soldada (se deforma: sin triángulo congruente): su superficie nativa debe estar cerca de las tres esquinas con alguna cámara de confianza */
                for (int t = 0; g && g->weld && t < ntrust && !near; t++) {
                    near = 1;
                    for (int k = 0; k < 3 && near; k++) {
                        float pw[3]; double bd = 1e30; vcam_world(&trust[t], vw[k], pw);
                        for (uint32_t j = 0; j < g->n && bd > NV_MOVE_TOL; j++) { float pp[3][3]; for (int e = 0; e < 3; e++) memcpy(pp[e], tris[g->first + j].v[e].p, 12); const double d = pt_tri(pw, pp); if (d < bd) bd = d; }
                        near = bd <= NV_MOVE_TOL;
                    }
                }
                if (g && (near || vpiece_congruent(g, tris, q))) { out->moved++; continue; }
                int gen = 0;   /* objeto que el nativo dibuja como efecto (sombra de personaje: el extractor de mallas la omite) */
                for (size_t i = 0; i < nfx && !gen; i++) gen = (fx[i].key & 0x1FFFFFFFu) == (w->key & 0x1FFFFFFFu);
                if (gen) { out->moved++; continue; }
            } else if (cls == NV_PRIM && w->psize) {
                const uint32_t idx = ((o->pkt_addr & 0x1FFFFFFFu) - w->lo) / w->psize;
                int gen = 0;
                for (size_t i = 0; i < nfx && !gen; i++) gen = fx[i].idx == idx && (fx[i].key & 0x1FFFFFFFu) == (w->key & 0x1FFFFFFFu);
                if (gen) { out->moved++; continue; }
            }
            out->cls[cls]++; out->missing++;
            const uint32_t key = w ? w->key : 0u;
            int wi; for (wi = 0; wi < nworst; wi++) if (worst[wi].key == key && worst[wi].cls == cls) break;
            if (wi == nworst && nworst < 64) { worst[nworst].key = key; worst[nworst].n = 0; worst[nworst].cls = (uint8_t)cls; nworst++; }
            if (wi < nworst) worst[wi].n++;
        }
    }
    for (int i = 0; i < nworst; i++) if (worst[i].n > out->worst_n) { out->worst_n = worst[i].n; out->worst_key = worst[i].key; out->worst_cls = worst[i].cls; }
    out->valid = 1;
    rc = 0;
done:
    vgrid_free(&G); free(vt); free(pg); free(samp);
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    out->ms = (double)(ts1.tv_sec - ts0.tv_sec) * 1e3 + (double)(ts1.tv_nsec - ts0.tv_nsec) * 1e-6;
    return rc;
}

/* ---- dueños desde la RAM (colas reales del canal principal) ---- */
#define V_CHAN 0x800B2B7Cu
#define V_GROUP 0x800ACAF8u
static int v_ok(uint32_t a, uint32_t nb) { uint32_t seg = a >> 29, o = a & 0x1FFFFFFFu; return (seg == 0 || seg == 4 || seg == 5) && o >= 0x10000u && o < NS_RAM_SIZE && nb <= NS_RAM_SIZE - o; }
static uint32_t v_r32(const uint8_t *m, uint32_t a) { uint32_t v = 0; if (v_ok(a, 4)) memcpy(&v, m + (a & 0x1FFFFFFFu), 4); return v; }
static int v_s16(const uint8_t *m, uint32_t a) { int16_t v = 0; if (v_ok(a, 2)) memcpy(&v, m + (a & 0x1FFFFFFFu), 2); return v; }
static int vown_cmp(const void *a, const void *b) { uint32_t x = ((const NsVerifOwner *)a)->lo, y = ((const NsVerifOwner *)b)->lo; return x < y ? -1 : x > y; }
static void vown_add(NsVerifOwner *out, size_t cap, size_t *n, uint32_t pk, uint32_t bytes, uint32_t key, uint32_t piece, uint32_t psize, int cls) {
    if (*n >= cap || !bytes || !v_ok(pk, bytes)) return;
    NsVerifOwner *w = &out[(*n)++];
    w->lo = pk & 0x1FFFFFFFu; w->hi = w->lo + bytes; w->key = key; w->piece = (uint16_t)piece; w->psize = (uint16_t)psize; w->cls = (uint8_t)cls; w->screen = 0;
}
size_t ns_verif_owners(const uint8_t *m, NsVerifOwner *out, size_t cap) {
    size_t n = 0;
    if (!m || !out) return 0;
    const uint32_t gmask = v_r32(m, V_GROUP), q = v_r32(m, V_CHAN + 0x58);
    int qs = v_s16(m, V_CHAN + 0x52), pi = v_s16(m, V_CHAN + 0x54), no = v_s16(m, V_CHAN + 0x56);
    if (no > 256) no = 256;
    for (int i = 0; i < no; i++) {
        const uint32_t o = v_r32(m, q + 4u * (uint32_t)i);
        if (!v_ok(o, 0x48)) continue;
        const uint32_t flag = v_r32(m, o + 0x28), gid = (uint32_t)v_s16(m, o + 0x2C) & 0xFFFFu, def = v_r32(m, o + 0x24);
        const int nm = v_s16(m, o + 0x2E);
        if (nm <= 0 || nm > 1024 || !v_ok(o + 0x48u, 0x5Cu * (uint32_t)nm)) continue;
        const int cls = (gid && !(gid & gmask)) ? NV_OFFGROUP : ((flag & 2u) && !(flag & 8u)) ? NV_STATIC : NV_DYNAMIC;
        uint32_t nx = 1024; { int v = (int)v_r32(m, def + 4); if (v_ok(def, 8) && v > 0 && v < 1024) nx = (uint32_t)v; }
        uint32_t ext = (uint32_t)nm;   /* las extensiones siguen a las piezas en la instancia nativa, en el orden del extractor */
        for (int k = 0; k < nm; k++) {
            const uint32_t p = o + 0x48u + 0x5Cu * (uint32_t)k;
            /* los paquetes de una pieza y de sus extensiones van seguidos en el búfer de la pieza cabecera */
            uint32_t off = 0, e = p, piece = (uint32_t)k;
            for (int depth = 0; depth <= 32 && e; depth++) {
                if (depth) {
                    const uint32_t idx = (e - (o + 0x48u)) / 0x5Cu;
                    if (e < o + 0x48u || (e - (o + 0x48u)) % 0x5Cu || idx < (uint32_t)nm || idx >= nx || !v_ok(e, 0x5C)) break;
                    piece = ext++;
                }
                const int np = v_s16(m, e + 0x52);
                if (np > 0 && np <= 8192) {
                    for (uint32_t b = 0; b < 2; b++) { const uint32_t pk = v_r32(m, p + 0x54u + 4u * b); if (pk) vown_add(out, cap, &n, pk + off, (uint32_t)np * 52u, o, piece, 52u, cls); }
                    off += (uint32_t)np * 52u;
                }
                e = v_r32(m, e + 0x48);
            }
        }
    }
    if (qs > 1024) qs = 1024;
    for (int i = pi < 0 ? 0 : pi; i < qs; i++) {
        const uint32_t p = v_r32(m, q + 4u * (uint32_t)i);
        if (!v_ok(p, 0x54)) continue;
        const uint32_t type = v_r32(m, p + 0x24);
        const int np = v_s16(m, p + 0x2A), psz = v_s16(m, p + 0x30);
        if (np <= 0 || np > 8192 || psz < 8 || psz > 64) continue;
        for (uint32_t b = 0; b < 2; b++) {
            const size_t n0 = n;
            vown_add(out, cap, &n, v_r32(m, p + 0x40u + 4u * b), (uint32_t)np * (uint32_t)psz, p, 0, (uint32_t)psz, (type & 0x800u) ? NV_SORTONLY : NV_PRIM);
            if (n > n0 && (type & 0x200u)) out[n0].screen = 1;
        }
    }
    qsort(out, n, sizeof *out, vown_cmp);
    return n;
}

size_t ns_verif_format(const NsVerif *v, uint32_t frame, char *buf, size_t cap) {
    if (!buf || !cap || !v->valid) return 0;
    char cam[40];
    if (v->cam_distinct) snprintf(cam, sizeof cam, "DISTINTA"); else if (v->cam < 0) snprintf(cam, sizeof cam, "ajustada"); else snprintf(cam, sizeof cam, "candidata%d", v->cam);
    int w = snprintf(buf, cap, "[verif] fr=%u tris_gte=%u sin_equivalente=%u (estatico=%u dinamico=%u prim=%u sortonly=%u fuera_grupo=%u otros=%u) peor=%s/%08x(n=%u) mundo_en_2d=%u | en_otra_pose=%u sin_dueno_en_colas=%u ambiguos=%u sin_vista_2d=%u muestreo=1/%u camara=%s(explica=%.0f%% de %u) cam_dist=%.0f cam_giro=%.2f app_explica=%.0f%% solo_ajustada=%u sin_vista_estatico=%u sin_verificar=%u ok=%s %.1f ms\n",
                     frame, v->tris_gte, v->missing, v->cls[NV_STATIC], v->cls[NV_DYNAMIC], v->cls[NV_PRIM], v->cls[NV_SORTONLY], v->cls[NV_OFFGROUP], v->cls[NV_OTHER],
                     v->worst_n ? ns_verif_cls_name[v->worst_cls] : "-", v->worst_key, v->worst_n, v->world_in_2d, v->moved, v->no_owner, v->ambig, v->no_view_2d, v->step, cam, 100.0 * v->cam_in, v->cam_pts,
                     (double)v->cam_dist, (double)v->cam_giro, 100.0 * v->app_in, v->only_fit, v->no_view_static, v->unverified, (!v->missing && !v->unverified && !v->no_view_static) ? "si" : "no", v->ms);
    return w < 0 ? 0 : (size_t)w < cap ? (size_t)w : cap - 1;
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

/* ---------------------------------------------------------------------------------------------------------------- descartes del modo ENGINE */
/* triángulo i en el orden del sombreador: ns_adapt entrega la segunda mitad del cuadrilátero como (B,D,C) y ns_gpu_pack la dibuja como (B,C,D) */
static void disc_tri(const NsRTri *t, const NsAdaptSrc *s, float p[3][3]) {
    const int h = s->half != 0;
    memcpy(p[0], t->v[0].p, 12); memcpy(p[1], t->v[h ? 2 : 1].p, 12); memcpy(p[2], t->v[h ? 1 : 2].p, 12);
}
void ns_discards_core(const NsRTri *tris, const NsAdaptSrc *src, size_t n, const NsGpuEngCam *cam, const float (*eyes)[3], int neyes, uint32_t off, NsDiscards *out) {
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < n; i++) {
        float p[3][3];
        disc_tri(&tris[i], &src[i], p);
        const int r = ns_gpu_engine_tri((const float (*)[3])p, cam, !(src[i].mflags & NS_MF_TWOSIDED), !(src[i].iflags & NS_IF_GBOUND), eyes, neyes, off, NULL);
        out->sent++;
        if (r == NSG_ENG_BEHIND) out->behind++;
        else if (r == NSG_ENG_OVERSIZE) out->size++;
        else if (r == NSG_ENG_BACK) out->back++;
        else { out->drawn++; out->extra += r == NSG_ENG_EXTRA; out->split += r == NSG_ENG_SPLIT; if (src[i].iflags & NS_IF_STATIC) out->drawn_static++; else out->drawn_dyn++; }
        if (r == NSG_ENG_EXTRA && neyes > 1) {   /* cada pase decide el rescate con su ojo (ns_vertex): ¿lo rescatan todos? */
            int all = 1;
            for (int e = 0; e < neyes && all; e++) all = ns_gpu_engine_tri((const float (*)[3])p, cam, !(src[i].mflags & NS_MF_TWOSIDED), !(src[i].iflags & NS_IF_GBOUND), eyes + e, 1, off, NULL) == NSG_ENG_EXTRA;
            out->extra_one += !all;
        }
    }
}
static void disc_cam(const NsScene *sc, NsGpuEngCam *c) {
    memset(c, 0, sizeof *c);
    for (int k = 0; k < 3; k++) { c->eye[k] = sc->cam.eye[k]; c->x[k] = sc->cam.rot[3 * k]; c->y[k] = sc->cam.rot[3 * k + 1]; c->fwd[k] = sc->cam.rot[3 * k + 2]; }
    c->h = sc->cam.h; c->hx = sc->cam.h; c->hy = sc->cam.h * (sc->cam.overscan_y > 0.0f ? sc->cam.overscan_y : 1.0f);
}
void ns_discards_count(const NsScene *sc, const NsMeshStore *st, uint32_t off, NsDiscards *out) {
    memset(out, 0, sizeof *out);
    if (!sc || !st) return;
    NsAdaptOpts ao;
    memset(&ao, 0, sizeof ao);
    NsAdaptSrc *src = NULL; NsRTri *tr = NULL; NsAdaptStats as;
    ao.src_out = &src; ao.no_tint = 1; ao.no_shade = 1; ao.exposure = 1.0f;
    const size_t n = ns_adapt_scene(sc, st, &ao, &tr, &as);
    NsGpuEngCam c;
    disc_cam(sc, &c);
    const float hs = 0.5f * ns_gpu_engine_sep();   /* ojos de la pantalla 3D: el del motor -+ media separación a lo largo de su eje X */
    float eyes[2][3];
    for (int k = 0; k < 3; k++) { eyes[0][k] = c.eye[k] - hs * c.x[k]; eyes[1][k] = c.eye[k] + hs * c.x[k]; }
    if (n && tr && src) ns_discards_core(tr, src, n, &c, (const float (*)[3])eyes, 2, off, out);
    for (uint32_t i = 0; i < sc->n_piece; i++) {
        const NsMesh *m = sc->piece[i].mesh_id ? ns_store_get(st, sc->piece[i].mesh_id) : NULL;
        for (uint32_t f = 0; m && m->faces && f < m->n_faces; f++) out->published += (m->faces[f].flags & NS_FF_TRI) ? 1u : 2u;
    }
    free(tr); free(src);
}
/* ---- nativo frente a la lista en la ventana del motor ---- */
void ns_winmask_tri(float *iz, const float s[3][3]) {   /* s = (x px, y px, 1/z) por vértice; se queda con el más cercano (1/z mayor) en el centro de cada celda */
    float mnx = fminf(s[0][0], fminf(s[1][0], s[2][0])), mxx = fmaxf(s[0][0], fmaxf(s[1][0], s[2][0])), mny = fminf(s[0][1], fminf(s[1][1], s[2][1])), mxy = fmaxf(s[0][1], fmaxf(s[1][1], s[2][1]));
    if (!(mxx >= -160.0f && mnx < 160.0f && mxy >= -112.0f && mny < 112.0f)) return;
    const double area = (double)(s[1][0] - s[0][0]) * (s[2][1] - s[0][1]) - (double)(s[2][0] - s[0][0]) * (s[1][1] - s[0][1]);
    if (!(fabs(area) > 1e-6)) return;
    int x0 = (int)floorf((mnx + 160.0f) * 0.5f), x1 = (int)floorf((mxx + 160.0f) * 0.5f), y0 = (int)floorf((mny + 112.0f) * 0.5f), y1 = (int)floorf((mxy + 112.0f) * 0.5f);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > NS_WIN_W - 1) x1 = NS_WIN_W - 1;
    if (y1 > NS_WIN_H - 1) y1 = NS_WIN_H - 1;
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        const double px = 2.0 * x + 1.0 - 160.0, py = 2.0 * y + 1.0 - 112.0;
        const double w0 = ((s[1][0] - px) * (s[2][1] - py) - (s[2][0] - px) * (s[1][1] - py)) / area, w1 = ((s[2][0] - px) * (s[0][1] - py) - (s[0][0] - px) * (s[2][1] - py)) / area, w2 = 1.0 - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        const float z = (float)(w0 * s[0][2] + w1 * s[1][2] + w2 * s[2][2]);
        if (z > iz[y * NS_WIN_W + x]) iz[y * NS_WIN_W + x] = z;
    }
}
/* triángulo de mundo -> vista del motor, recortado por z >= zn, proyectado y rasterizado */
static void wc_world_tri(float *iz, const NsGpuEngCam *cam, const float g[3][3], float zn) {
    float v[4][3], o[5][3]; int no = 0;
    for (int k = 0; k < 3; k++) {
        const float r[3] = {g[k][0] - cam->eye[0], g[k][1] - cam->eye[1], g[k][2] - cam->eye[2]};
        v[k][0] = cam->x[0] * r[0] + cam->x[1] * r[1] + cam->x[2] * r[2]; v[k][1] = cam->y[0] * r[0] + cam->y[1] * r[1] + cam->y[2] * r[2]; v[k][2] = cam->fwd[0] * r[0] + cam->fwd[1] * r[1] + cam->fwd[2] * r[2];
    }
    for (int k = 0; k < 3; k++) {   /* Sutherland-Hodgman contra el plano cercano */
        const float *a = v[k], *b = v[(k + 1) % 3];
        if (a[2] >= zn) memcpy(o[no++], a, 12);
        if ((a[2] >= zn) != (b[2] >= zn)) { const float t = (zn - a[2]) / (b[2] - a[2]); for (int c = 0; c < 3; c++) o[no][c] = a[c] + t * (b[c] - a[c]); no++; }
    }
    for (int k = 0; k < no; k++) { const float z = o[k][2]; o[k][0] = cam->hx * o[k][0] / z; o[k][1] = cam->hy * o[k][1] / z; o[k][2] = 1.0f / z; }
    for (int k = 1; k + 1 < no; k++) { float s3[3][3]; memcpy(s3[0], o[0], 12); memcpy(s3[1], o[k], 12); memcpy(s3[2], o[k + 1], 12); ns_winmask_tri(iz, (const float (*)[3])s3); }
}
void ns_winmask_native(const NsRTri *tris, const NsAdaptSrc *src, size_t n, const NsGpuEngCam *cam, uint32_t off, float *iz, float *iz_split) {
    for (size_t i = 0; i < n; i++) {   /* lo rescatado (NSG_ENG_EXTRA) solo se dibuja fuera de la ventana */
        float p[3][3], g[3][3];
        disc_tri(&tris[i], &src[i], p);
        const int r = ns_gpu_engine_tri((const float (*)[3])p, cam, !(src[i].mflags & NS_MF_TWOSIDED), !(src[i].iflags & NS_IF_GBOUND), NULL, 0, off, g);
        if (r == NSG_ENG_DRAW) wc_world_tri(iz, cam, (const float (*)[3])g, 1.0f);
        else if (r == NSG_ENG_SPLIT && iz_split) wc_world_tri(iz_split, cam, (const float (*)[3])g, NS_WIN_NEAR);
    }
}
void ns_winmask_list(const GpuOp *ops, uint32_t nops, float h, float *iz) {
    for (uint32_t i = 0; i < nops; i++) {   /* polígonos 3D con todos los vértices delante, como los proyecta la GTE (z < H/2 en el plano H/2), menos los que la tarjeta no dibuja */
        const GpuOp *o = &ops[i];
        if (o->type != GPU_OP_POLY || o->nv < 3) continue;
        int ok = 1;
        for (int k = 0; k < o->nv; k++) ok &= o->v[k].vvalid && o->v[k].vz > 0.0f;
        if (!ok) continue;
        for (int t = 0; t + 2 < o->nv; t++) {
            float s3[3][3]; int mnx = 32767, mxx = -32768, mny = 32767, mxy = -32768;
            for (int k = 0; k < 3; k++) {
                const GpuVtx *q = &o->v[t + k];
                mnx = q->x < mnx ? q->x : mnx; mxx = q->x > mxx ? q->x : mxx; mny = q->y < mny ? q->y : mny; mxy = q->y > mxy ? q->y : mxy;
                const float z = q->vz < 0.5f * h ? 0.5f * h : q->vz;
                s3[k][0] = h * q->vx / z; s3[k][1] = h * q->vy / z; s3[k][2] = 1.0f / z;   /* vy ya lleva el factor vertical de la pantalla del motor */
            }
            if (mxx - mnx > 1023 || mxy - mny > 511) continue;
            ns_winmask_tri(iz, (const float (*)[3])s3);
        }
    }
}
void ns_winmask_cmp(const float *a, const float *b, NsWinCmp *out) {
    out->cells = NS_WIN_W * NS_WIN_H; out->cover = out->extra = out->missing = 0;
    for (int o = 0; o < NS_WIN_W * NS_WIN_H; o++) {
        if (a[o] > 0 && b[o] > 0) out->cover += a[o] > 1.3f * b[o];
        else if (a[o] > 0) out->extra++;
        else if (b[o] > 0) out->missing++;
    }
}
void ns_wincmp(const NsRTri *tris, const NsAdaptSrc *src, size_t n, const NsGpuEngCam *cam, uint32_t off, const GpuOp *ops, uint32_t nops, NsWinCmp *out) {
    memset(out, 0, sizeof *out);
    float *izn = (float *)calloc(3u * NS_WIN_W * NS_WIN_H, sizeof(float));
    if (!izn || !cam || !(cam->h > 0.0f)) { free(izn); return; }
    float *izl = izn + NS_WIN_W * NS_WIN_H, *izs = izl + NS_WIN_W * NS_WIN_H;
    ns_winmask_native(tris, src, n, cam, off, izn, izs);
    ns_winmask_list(ops, nops, cam->h, izl);
    ns_winmask_cmp(izn, izl, out);
    for (int o = 0; o < NS_WIN_W * NS_WIN_H; o++) out->split += izs[o] > izn[o];
    free(izn);
}
size_t ns_discards_format(const NsDiscards *d, float jump_pct, const NsWinCmp *win, uint32_t off, char *buf, size_t cap) {
    if (!buf || !cap) return 0;
    char w[128];
    if (win && win->cells) snprintf(w, sizeof w, "tapa=%.1f%% de_mas=%.1f%% falta=%.1f%% partidas=%.1f%%", 100.0 * win->cover / win->cells, 100.0 * win->extra / win->cells, 100.0 * win->missing / win->cells, 100.0 * win->split / win->cells);
    else snprintf(w, sizeof w, "sin_medida");
    const int n = snprintf(buf, cap, "[descartes] caras=%s publicados=%u enviados=%u detras=%u tamano=%u reverso=%u extra=%u (un_ojo=%u) partidas=%u dibujados=%u (escenario=%u dinamicos=%u) salto=%.0f%% | lista: %s\n",
                           (off & NSG_ENG_OFF_SPLIT) ? "recortadas" : "original", d->published, d->sent, d->behind, d->size, d->back, d->extra, d->extra_one, d->split, d->drawn, d->drawn_static, d->drawn_dyn, (double)jump_pct, w);
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}

typedef char ns_discards_words[sizeof(NsDiscards) == NS_DISCARDS_WORDS * sizeof(uint32_t) && sizeof(NsWinCmp) == NS_WINCMP_WORDS * sizeof(uint32_t) ? 1 : -1];
void ns_discards_scene(const NsScene *sc, const NsMeshStore *st, uint32_t out[NS_DISCARDS_WORDS]) { NsDiscards d; ns_discards_count(sc, st, ns_gpu_engine_off(), &d); memcpy(out, &d, sizeof d); }
size_t ns_discards_line(const uint32_t d[NS_DISCARDS_WORDS], float jump_pct, const uint32_t win[NS_WINCMP_WORDS], char *buf, size_t cap) {
    NsDiscards x; NsWinCmp w;
    memcpy(&x, d, sizeof x); memset(&w, 0, sizeof w);
    if (win) memcpy(&w, win, sizeof w);
    return ns_discards_format(&x, jump_pct, &w, ns_gpu_engine_off(), buf, cap);
}

/* ---------------------------------------------------------------------------------------------------------------- huecos del mundo */
void ns_cover_holes(const NsRTri *tris, size_t ntris, size_t nsky, const float eye[3], NsCoverHoles *out) {
    memset(out, 0, sizeof *out);
    if (!tris || !ntris || !eye) return;
    const size_t ngeo = nsky < ntris ? ntris - nsky : 0;
    memcpy(out->eye, eye, sizeof out->eye);
    for (int j = 0; j < NC_HOLE_EL; j++) {
        const double el = ((double)j + 0.5) / NC_HOLE_EL * M_PI - 0.5 * M_PI;   /* de -90 (abajo) a +90 (arriba); j = 0 abajo */
        const double ce = cos(el), se = sin(el);
        for (int i = 0; i < NC_HOLE_AZ; i++) {
            const double az = ((double)i + 0.5) / NC_HOLE_AZ * 2.0 * M_PI;
            const double d[3] = {ce * sin(az), se, ce * cos(az)};   /* Y arriba */
            int hit = 0, sky = 0;
            for (size_t t = 0; t < ntris && !hit && !sky; t++) {   /* Möller-Trumbore sin descarte de caras; primero la geometría y, si el rayo no da en ninguna, el cielo */
                const float *a = tris[t].v[0].p, *b = tris[t].v[1].p, *c = tris[t].v[2].p;
                const double e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                const double p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
                const double det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
                if (fabs(det) < 1e-9) continue;
                const double inv = 1.0 / det, s[3] = {eye[0] - a[0], eye[1] - a[1], eye[2] - a[2]};
                const double u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
                if (u < 0.0 || u > 1.0) continue;
                const double q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
                const double v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
                if (v < 0.0 || u + v > 1.0) continue;
                if ((e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv > 1.0) { if (t < ngeo) hit = 1; else sky = 1; }   /* a más de 1 unidad del ojo */
            }
            out->rays++;
            if (!hit) {
                out->miss++; out->sky += (uint32_t)sky;
                if (j >= NC_HOLE_EL / 2) out->miss_up++; else out->miss_dn++;
                out->band[(NC_HOLE_EL - 1 - j) * 12 / NC_HOLE_EL]++;
            }
        }
    }
}

static int cover_run_once(NsCover *c, const uint8_t *ram, const uint8_t *scratch, const GpuOp *ops, uint32_t nops, const NsCoverCfg *cfg, NsCoverResult *out, double *holes_ms) {
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
    /* La lista lleva las posiciones de vista que dio la GTE con las matrices de pantalla del motor: con la cámara del propio juego (cinemáticas, introducción) su fila Y va multiplicada por 66/64 (sobreescaneo) y
     * con la de la capa VR no. Sin ese factor todo nativo con |y| grande queda ~3 % fuera de su pareja (medido en d0004: 72,9 % sin factor, 96,3 % con él). */
    float ys = cfg ? cfg->ys : 0.0f;
    if (!(ys > 0.0f)) ys = ns_extract_screen_ys(ram, NS_RAM_SIZE);
    if (!(ys > 0.0f)) ys = 1.0f;
    for (int k = 0; k < 3; k++) cam.A[3 + k] *= ys;
    cam.t[1] *= ys;
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
    if (rc == 0) out->ys = ys;
    NsVerifFxTri *ft = NULL; size_t nft = 0;   /* triángulos de los efectos nativos en el mundo (cielo incluido) */
    if (rc == 0 && holes_ms) {   /* verificador geométrico (solo en la primera medida, como los huecos) */
        NsVerifOwner *own = (NsVerifOwner *)malloc(8192 * sizeof *own);
        const size_t nown = own ? ns_verif_owners(ram, own, 8192) : 0;
        NsEffect *ef = NULL;
        if (fx_done && c->fx->n_items) {
            ef = (NsEffect *)malloc(c->fx->n_items * sizeof *ef); ft = (NsVerifFxTri *)malloc(2u * c->fx->n_items * sizeof *ft);
            const uint32_t ne = (ef && ft) ? ns_fx_export(c->fx, 1, ef, c->fx->n_items, NULL) : 0;
            const float *Rw = c->sc.cam.rot;   /* carteles: orientados con la cámara de la RAM (columnas = derecha, abajo) */
            for (uint32_t i = 0; i < ne; i++) {
                const NsEffect *e = &ef[i];
                float q[4][3]; int nq = e->nv;
                if (e->kind == NS_EK_BILLBOARD) {
                    for (int v = 0; v < 4; v++) { const float bx = e->box[(v & 1) ? 2 : 0], by = e->box[(v & 2) ? 3 : 1]; for (int r = 0; r < 3; r++) q[v][r] = e->p[0][r] + Rw[3 * r] * bx + Rw[3 * r + 1] * by; }
                    nq = 4;
                } else { for (int v = 0; v < 4; v++) memcpy(q[v], e->p[v < e->nv ? v : e->nv - 1], sizeof q[v]); if (nq < 3) nq = 3; }
                for (int h = 0; h < (nq == 4 ? 2 : 1); h++) {
                    NsVerifFxTri *t = &ft[nft++];
                    memcpy(t->p[0], q[h], 12); memcpy(t->p[1], q[h + 1], 12); memcpy(t->p[2], q[h + 2], 12);
                    t->key = e->src_key; t->idx = e->idx; t->nogeo = e->role == NSFX_ROLE_SKY;
                }
            }
        }
        NsVerifCam vc[3]; int nvc = 0;
        if (cfg && cfg->list_M && cfg->list_eye) {   /* candidata 0: la cámara con la que se emitió la lista (vista->mundo y ojo): se invierte de verdad */
            VCam iv; const float z3[3] = {0, 0, 0};
            if (vcam_make(&iv, cfg->list_M, z3) == 0) {
                memcpy(vc[nvc].A, iv.Ai, sizeof iv.Ai);
                for (int r = 0; r < 3; r++) vc[nvc].t[r] = -(iv.Ai[3 * r] * cfg->list_eye[0] + iv.Ai[3 * r + 1] * cfg->list_eye[1] + iv.Ai[3 * r + 2] * cfg->list_eye[2]);
                nvc++;
            }
        }
        if (cfg && cfg->prev_A && cfg->prev_t) {   /* candidata: la cámara de la RAM del fotograma anterior (con la que se emitió la lista cuando no hay registro de cámara) */
            memcpy(vc[nvc].A, cfg->prev_A, sizeof vc[nvc].A); memcpy(vc[nvc].t, cfg->prev_t, sizeof vc[nvc].t);
            nvc++;
        }
        {   /* última candidata: la matriz mundo->vista del canal tal como está en la RAM (la de la app) */
            for (int k = 0; k < 9; k++) vc[nvc].A[k] = (float)v_s16(ram, V_CHAN + 0x10u + 2u * (uint32_t)k) * (1.0f / 4096.0f);
            for (int k = 0; k < 3; k++) vc[nvc].t[k] = (float)(int32_t)v_r32(ram, V_CHAN + 0x24u + 4u * (uint32_t)k);
            nvc++;
        }
        if (own && ns_verif_core(tr, src, n, ft, nft, own, nown, vc, nvc, ys, ops, nops, 0, &out->verif) == 0) {
            out->ms += out->verif.ms;
            for (uint32_t i = 0; i < nops; i++) {   /* primitivas de mundo de la cola que nadie reclama: siguen en la vía 2D */
                const GpuOp *o = &ops[i];
                if (o->type != GPU_OP_POLY && o->type != GPU_OP_LINE && o->type != GPU_OP_RECT) continue;
                const NsVerifOwner *w = vown_find(own, nown, o->pkt_addr);
                if (!w || w->cls != NV_PRIM || w->screen) continue;
                if (!fx_done || !ns_fx_covers_packet(ram, c->fx, o->pkt_addr)) out->verif.world_in_2d++;   /* el mismo criterio con el que el anfitrión decide la vía 2D (prov_covered) */
            }
        }
        free(own); free(ef);
    }
    if (rc == 0 && n && nops && holes_ms && out->verif.valid && out->verif.cam_in >= 0.5f) {   /* nativo frente a la lista en la ventana del motor, con la cámara que explica la lista (la del verificador) */
        NsGpuEngCam ec; VCam uc; const float z3[3] = {0, 0, 0};
        disc_cam(&c->sc, &ec);
        if (vcam_make(&uc, out->verif.cam_A, out->verif.cam_t) == 0) {
            vcam_world(&uc, z3, ec.eye);
            for (int k = 0; k < 3; k++) { ec.x[k] = uc.A[k]; ec.y[k] = uc.A[3 + k]; ec.fwd[k] = uc.A[6 + k]; }
            ec.hy = ec.h * ys;
            ns_wincmp(tr, src, n, &ec, ns_gpu_engine_off(), ops, nops, &out->win);
        }
    }
    if (rc == 0 && (n || nft) && holes_ms) {   /* ojo de la RAM (el del juego en ese instante); solo en la primera medida: la escena y el ojo son los mismos en la segunda. Cuenta lo que el visor dibuja: mallas y efectos de mundo (el cielo) */
        struct timespec h0, h1;
        clock_gettime(CLOCK_MONOTONIC, &h0);
        NsRTri *all = (NsRTri *)malloc((n + nft + 1) * sizeof *all);
        if (all) {
            if (n) memcpy(all, tr, n * sizeof *all);
            size_t na = n, nsky = 0;   /* mallas, efectos de mundo y, al final, el cielo (no es geometría) */
            for (int pass = 0; pass < 2; pass++) for (size_t i = 0; i < nft; i++) if ((ft[i].nogeo != 0) == pass) { memset(&all[na], 0, sizeof all[0]); for (int k = 0; k < 3; k++) memcpy(all[na].v[k].p, ft[i].p[k], 12); na++; nsky += (size_t)pass; }
            ns_cover_holes(all, na, nsky, c->sc.cam.eye, &out->holes);
            free(all);
        }
        clock_gettime(CLOCK_MONOTONIC, &h1);
        *holes_ms = (double)(h1.tv_sec - h0.tv_sec) * 1e3 + (double)(h1.tv_nsec - h0.tv_nsec) * 1e-6;
        out->ms += *holes_ms;
    }
    free(ft);
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
    APP("[cobertura] fr=%u lista: tris3D=%u (poligonos=%u sin_vista=%u degenerados=%u) cubierto=%.1f%% (opacos %.1f%%, exacto %.1f%%) | cubierto_por: exacto=%u otra_diagonal=%u subdividido=%u efectos=%u (agua=%u sprites=%u otros=%u) | SIN_CUBRIR: pose=%u otra_textura=%u parcial=%u ausente=%u sprite=%u (semi: pose=%u otra_tex=%u parcial=%u ausente=%u sprite=%u) | textura_distinta(pagina/clut/uv)=%u/%u/%u | nativo: tris=%u sin_pareja=%u (detras=%u plano_cercano=%u fuera=%u reverso=%u lejos=%u objeto_no_visible=%u hardware_descarta_grande=%u de_mas=%u) | posicion(match): error_medio=%.2fpx desplazados_>2px=%u | color(match): comparados=%u distintos=%u error_medio=%.1f | vista_y=%.4f | %.1f ms\n",
        frame, r->list_tris, r->list_polys3d, r->list_no_view, r->list_degenerate, r->cover_pct, r->cover_opaque_pct, r->exact_pct,
        no_cov[NC_MATCH], no_cov[NC_SPLIT], no_cov[NC_SUBDIV], r->fx_tris, r->fx_water, r->fx_sprite, r->fx_other,
        no_cov[NC_POSE], no_cov[NC_OTHERTEX], no_cov[NC_PARTIAL], no_cov[NC_ABSENT], no_cov[NC_BILLBOARD],
        r->cause[NC_POSE][1], r->cause[NC_OTHERTEX][1], r->cause[NC_PARTIAL][1], r->cause[NC_ABSENT][1], r->cause[NC_BILLBOARD][1],
        r->tex_page, r->tex_clut, r->tex_uv, r->nat_tris, r->nat_tris - r->nat_used,
        r->rev[NR_BEHIND], r->rev[NR_NEAR], r->rev[NR_OFF], r->rev[NR_BACK], r->rev[NR_FAR], r->rev[NR_HIDDEN], r->rev[NR_BIG], r->rev[NR_EXTRA], r->shift_mean_px, r->shift_bad, r->col_n, r->col_bad, r->col_mean, r->ys, r->ms);
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
    if (r->holes.rays) {
        const NsCoverHoles *H = &r->holes;
        APP("[huecos] fr=%u ojo=(%.0f,%.0f,%.0f) rayos=%u sin_geometria=%.1f%% (arriba %.1f%%, abajo %.1f%%) solo_cielo=%.1f%% franjas15(zenit->nadir)=", frame, H->eye[0], H->eye[1], H->eye[2], H->rays, 100.0 * H->miss / H->rays,
            100.0 * H->miss_up / (H->rays / 2), 100.0 * H->miss_dn / (H->rays / 2), 100.0 * H->sky / H->rays);
        for (int b = 0; b < 12; b++) APP("%s%.0f%%", b ? " " : "", 100.0 * H->band[b] / (H->rays / 12));
        APP("\n");
    }
    if (r->verif.valid) o += ns_verif_format(&r->verif, frame, buf + o, cap - o);
    if (r->fxc_ok) o += ns_fxcmp_format(&r->fxc, frame, buf + o, cap - o);
#undef APP
    return o;
}

/* El registro de cámara de la lista puede estar OBSOLETO (intro, muerte, cinemáticas del motor que no pasan por la cámara VR: conserva la cámara de la pantalla de título): la medida
 * saldría 0 % con una escena nativa perfecta (volcado de la muerte: 0,0 % con él y 87,4 % con la cámara de la RAM) y la app abandonaría la pantalla 3D. Si con la cámara de la lista la
 * cobertura es baja se vuelve a medir con la de la RAM (la de la propia escena) y se queda la mejor. */
int ns_cover_run(NsCover *c, const uint8_t *ram, const uint8_t *scratch, const GpuOp *ops, uint32_t nops, const NsCoverCfg *cfg, NsCoverResult *out) {
    double hms = 0.0;
    int rc = cover_run_once(c, ram, scratch, ops, nops, cfg, out, &hms);
    if (rc != 0 || !cfg || !cfg->list_eye || !cfg->list_M || out->cover_pct >= 80.0f) return rc;
    NsCoverCfg c2 = *cfg;
    c2.list_eye = NULL; c2.list_M = NULL;
    NsCoverResult r2;
    if (cover_run_once(c, ram, scratch, ops, nops, &c2, &r2, NULL) == 0 && r2.cover_pct > out->cover_pct) { r2.holes = out->holes; r2.verif = out->verif; r2.ms += hms + out->verif.ms; *out = r2; return 0; }
    return 0;   /* ganó la de la lista: `out` conserva esa medida */
}
