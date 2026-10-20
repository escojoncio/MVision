/* ns_fill: ver ns_fill.h. */
#include "ns_fill.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

const char *const ns_fill_cls_name[NF_N] = {"exacto", "otra_diagonal", "subdividido", "pegado_a_camara", "pose_jugador", "pose_otro", "ausente", "otra_textura", "parcial", "sprite", "textura_corregida",
                                            "detras", "ambiguo", "incoherente", "arista", "lejos", "nan", "texel_unico", "parcial_semi", "junto_a_cabeza", "tope"};

void ns_fill_cfg_default(NsFillCfg *c) {
    memset(c, 0, sizeof *c);
    c->tol_base = 20.0f; c->tol_z = 0.004f; c->surf_tol = 80.0f;
    c->pose_radius = 2500.0f; c->max_edge = 40000.0f; c->max_edge_flagged = 6000.0f; c->max_dist = 400000.0f;
    c->screen_w = 320; c->screen_h = 256;
    c->pose_fill = 0; c->fix_texture = 1; c->fill_partial_semi = 0;
    c->keep_frames = 15; c->max_out = 6000;
    c->cut_dist = 3000.0f; c->cut_cos = 0.7f; c->bias = 2;
    c->near_skip = 250.0f; c->view_match = 24.0f;
}

#define CELL 256.0f      /* rejilla de esquinas */
#define SCELL 512.0f     /* rejilla de superficies */
#define TOL_CAP 200.0f
#define PAGE_MASK 0x19Fu /* x, y y profundidad de color (sin el modo de mezcla) */
#define SURF_MAXC 6      /* celdas por eje a partir de las cuales un triángulo va a la lista «grande» */
#define NF_SRC_KEY 0xF1000000u

typedef struct { float p[3][3]; float c[3]; float r; uint16_t page, clut; float u[3], v[3]; uint8_t textured, dyn, player; uint32_t inst, piece, face; float lo[3], hi[3]; float n[3], dp; } FTri;

typedef struct { NsEffect e; float c[3]; float vc[3]; uint32_t sig; uint32_t last; uint32_t id; uint8_t claimed; } FRec;

struct NsFill {
    /* índice nativo */
    FTri *nt; size_t nt_cap, n;
    int32_t *cg_head, *cg_next; size_t cg_nb, cg_next_cap; uint32_t cg_mask;
    int32_t *sg_head, *sg_node_next, *sg_node_tri; size_t sg_nb, sg_node_cap, sg_node_cap2, sg_nodes; uint32_t sg_mask;
    int32_t *big; size_t big_n, big_cap;
    int32_t *tg_head, *tg_next; size_t tg_nb, tg_next_cap; uint32_t tg_mask;   /* dinámicos por (página, CLUT) */
    int32_t *stamp; size_t stamp_cap; int32_t qid;
    /* persistencia */
    FRec *alive; size_t alive_n, alive_cap;
    FRec *nw; size_t nw_n, nw_cap;
    int32_t *ph_head, *ph_next; size_t ph_nb, ph_cap;
    NsEffect *out; size_t out_cap, out_n;
    int have_last; float last_eye[3], last_fwd[3]; uint32_t last_frame;
    uint32_t next_id;                  /* identidad estable de lo rellenado (se hereda al reencontrarlo por mundo o por vista) */
    int32_t *vh_head, *vh_next; size_t vh_nb, vh_cap;   /* índice por posición de VISTA de lo visto hace poco (para reconocer lo pegado a la cabeza) */
    const NsRTri *idx_tris; size_t idx_n; uint32_t idx_player;   /* índice ya construido para esta escena (lo deja ns_fill_probe para ns_fill_run) */
    float surf_pad;   /* margen con el que se inflaron las cajas de la rejilla de superficies (tope de la tolerancia de «sobre la superficie») */
};

NsFill *ns_fill_create(void) { return (NsFill *)calloc(1, sizeof(NsFill)); }
void ns_fill_destroy(NsFill *f) {
    if (!f) return;
    free(f->nt); free(f->cg_head); free(f->cg_next); free(f->sg_head); free(f->sg_node_next); free(f->sg_node_tri); free(f->big); free(f->tg_head); free(f->tg_next); free(f->stamp);
    free(f->alive); free(f->nw); free(f->ph_head); free(f->ph_next); free(f->vh_head); free(f->vh_next); free(f->out);
    free(f);
}
void ns_fill_reset(NsFill *f) { if (f) { f->alive_n = 0; f->out_n = 0; f->have_last = 0; } }
const NsEffect *ns_fill_effects(const NsFill *f, uint32_t *n) { if (n) *n = f ? (uint32_t)f->out_n : 0; return f ? f->out : NULL; }

#define GROW(p, cap, need) do { if ((need) > (cap)) { size_t nc_ = (cap) ? (cap) : 64; while (nc_ < (size_t)(need)) nc_ *= 2; void *np_ = realloc((p), nc_ * sizeof(*(p))); if (!np_) return -1; (p) = np_; (cap) = nc_; } } while (0)

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1e3 + (double)t.tv_nsec * 1e-6; }

/* ---------------------------------------------------------------------------------------------------------------- geometría */
static double d3(const float *a, const float *b) { double x = (double)a[0] - b[0], y = (double)a[1] - b[1], z = (double)a[2] - b[2]; return sqrt(x * x + y * y + z * z); }
static void sub3(const float *a, const float *b, double *o) { for (int i = 0; i < 3; i++) o[i] = (double)a[i] - b[i]; }
static void crs(const double *a, const double *b, double *o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static double dot3(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static double pt_tri(const float *P, const float tv[3][3]) {   /* distancia de un punto a un triángulo 3D (como ns_cover) */
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

static int inv3(const double *A, double *I) {
    double c0 = A[4] * A[8] - A[5] * A[7], c1 = A[5] * A[6] - A[3] * A[8], c2 = A[3] * A[7] - A[4] * A[6];
    double det = A[0] * c0 + A[1] * c1 + A[2] * c2;
    if (!(fabs(det) > 1e-12) || !isfinite(det)) return -1;
    double id = 1.0 / det;
    I[0] = c0 * id; I[1] = (A[2] * A[7] - A[1] * A[8]) * id; I[2] = (A[1] * A[5] - A[2] * A[4]) * id;
    I[3] = c1 * id; I[4] = (A[0] * A[8] - A[2] * A[6]) * id; I[5] = (A[2] * A[3] - A[0] * A[5]) * id;
    I[6] = c2 * id; I[7] = (A[1] * A[6] - A[0] * A[7]) * id; I[8] = (A[0] * A[4] - A[1] * A[3]) * id;
    return 0;
}

int ns_fill_cam_maps(const NsFillCam *cam, double A[9], double Ai[9]) {
    for (int r = 0; r < 3; r++) for (int k = 0; k < 3; k++) A[3 * r + k] = (double)cam->R[3 * k + r];   /* A = R^T */
    if (cam->ys > 0) for (int k = 0; k < 3; k++) A[3 + k] *= (double)cam->ys;   /* fila Y de la vista */
    return inv3(A, Ai);
}

/* ---------------------------------------------------------------------------------------------------------------- índice espacial */
static int32_t cell_of(float v, float cs) { float c = floorf(v / cs); return (int32_t)(c < -1e6f ? -1e6f : c > 1e6f ? 1e6f : c); }
static uint32_t hcell(int32_t x, int32_t y, int32_t z) { return ((uint32_t)x * 73856093u) ^ ((uint32_t)y * 19349663u) ^ ((uint32_t)z * 83492791u); }
static uint32_t tex_key(uint16_t page, uint16_t clut) { return (((uint32_t)(page & PAGE_MASK)) << 16 | clut) * 2654435761u; }

static int index_build(NsFill *f, const NsFillNative *nat, uint32_t player_key, float surf_pad) {
    size_t n = nat ? nat->n : 0;
    f->surf_pad = surf_pad;
    GROW(f->nt, f->nt_cap, n + 1);
    f->n = n;
    size_t ndyn = 0;
    for (size_t i = 0; i < n; i++) {
        FTri *t = &f->nt[i];
        const NsRTri *s = &nat->tris[i];
        for (int c = 0; c < 3; c++) { memcpy(t->p[c], s->v[c].p, sizeof t->p[c]); t->u[c] = s->v[c].u; t->v[c] = s->v[c].v; }
        for (int a = 0; a < 3; a++) {
            t->c[a] = (t->p[0][a] + t->p[1][a] + t->p[2][a]) / 3.0f;
            float lo = fminf(t->p[0][a], fminf(t->p[1][a], t->p[2][a])), hi = fmaxf(t->p[0][a], fmaxf(t->p[1][a], t->p[2][a]));
            t->lo[a] = lo - surf_pad; t->hi[a] = hi + surf_pad;
        }
        float r = 0;
        for (int c = 0; c < 3; c++) { float d = (float)d3(t->p[c], t->c); if (d > r) r = d; }
        {   /* plano unitario para descartar candidatos de superficie sin el coste de pt_tri */
            double e0[3], e1[3], nn[3];
            sub3(t->p[1], t->p[0], e0); sub3(t->p[2], t->p[0], e1); crs(e0, e1, nn);
            double nl = sqrt(dot3(nn, nn));
            if (nl > 1e-9) { for (int a = 0; a < 3; a++) t->n[a] = (float)(nn[a] / nl); t->dp = (float)(t->n[0] * t->p[0][0] + t->n[1] * t->p[0][1] + t->n[2] * t->p[0][2]); }
            else { t->n[0] = t->n[1] = t->n[2] = 0; t->dp = 0; }
        }
        t->r = r; t->page = s->tpage; t->clut = s->clut; t->textured = (s->flags & NS_TF_TEXTURED) != 0;
        t->dyn = 0; t->player = 0; t->inst = t->piece = t->face = 0;
        if (nat->src) {
            const NsAdaptSrc *a = &nat->src[i];
            t->inst = a->inst; t->piece = a->piece; t->face = a->face;
            if (nat->sc && a->inst < nat->sc->n_inst) t->dyn = !(nat->sc->inst[a->inst].flags & NS_IF_STATIC);
            if (player_key && a->key == player_key) { t->player = 1; t->dyn = 1; }
        }
        if (t->dyn) ndyn++;
    }
    /* rejilla de esquinas */
    size_t ne = n * 3, nb = 64;
    while (nb < ne * 2) nb <<= 1;
    GROW(f->cg_head, f->cg_nb, nb); GROW(f->cg_next, f->cg_next_cap, ne ? ne : 1);
    f->cg_mask = (uint32_t)(nb - 1);
    for (size_t i = 0; i < nb; i++) f->cg_head[i] = -1;
    for (size_t e = 0; e < ne; e++) {
        const float *p = f->nt[e / 3].p[e % 3];
        uint32_t b = hcell(cell_of(p[0], CELL), cell_of(p[1], CELL), cell_of(p[2], CELL)) & f->cg_mask;
        f->cg_next[e] = f->cg_head[b]; f->cg_head[b] = (int32_t)e;
    }
    /* rejilla de superficies (cajas inflados por surf_pad); los muy grandes van a la lista «grande» */
    nb = 64;
    while (nb < n * 4) nb <<= 1;
    GROW(f->sg_head, f->sg_nb, nb);
    f->sg_mask = (uint32_t)(nb - 1);
    for (size_t i = 0; i < nb; i++) f->sg_head[i] = -1;
    f->sg_nodes = 0; f->big_n = 0;
    for (size_t i = 0; i < n; i++) {
        const FTri *t = &f->nt[i];
        int32_t c0[3], c1[3];
        int big = 0;
        for (int a = 0; a < 3; a++) { c0[a] = cell_of(t->lo[a], SCELL); c1[a] = cell_of(t->hi[a], SCELL); if (c1[a] - c0[a] >= SURF_MAXC) big = 1; }
        if (big) { GROW(f->big, f->big_cap, f->big_n + 1); f->big[f->big_n++] = (int32_t)i; continue; }
        for (int32_t x = c0[0]; x <= c1[0]; x++) for (int32_t y = c0[1]; y <= c1[1]; y++) for (int32_t z = c0[2]; z <= c1[2]; z++) {
            GROW(f->sg_node_next, f->sg_node_cap, f->sg_nodes + 1);
            GROW(f->sg_node_tri, f->sg_node_cap2, f->sg_nodes + 1);
            uint32_t b = hcell(x, y, z) & f->sg_mask;
            f->sg_node_tri[f->sg_nodes] = (int32_t)i; f->sg_node_next[f->sg_nodes] = f->sg_head[b]; f->sg_head[b] = (int32_t)f->sg_nodes++;
        }
    }
    /* cubos por textura de los dinámicos */
    nb = 64;
    while (nb < ndyn * 2) nb <<= 1;
    GROW(f->tg_head, f->tg_nb, nb); GROW(f->tg_next, f->tg_next_cap, n + 1);
    f->tg_mask = (uint32_t)(nb - 1);
    for (size_t i = 0; i < nb; i++) f->tg_head[i] = -1;
    for (size_t i = 0; i < n; i++) {
        const FTri *t = &f->nt[i];
        if (!t->dyn) continue;
        uint32_t b = tex_key(t->page, t->clut) & f->tg_mask;
        f->tg_next[i] = f->tg_head[b]; f->tg_head[b] = (int32_t)i;
    }
    GROW(f->stamp, f->stamp_cap, n + 1);
    memset(f->stamp, 0, (n + 1) * sizeof(int32_t));
    f->qid = 0;
    return 0;
}

static int corner_query(NsFill *f, const float *q, float r, int32_t *out, int cap) {
    int n = 0;
    int32_t qid = ++f->qid;
    int32_t c0[3], c1[3];
    for (int a = 0; a < 3; a++) { c0[a] = cell_of(q[a] - r, CELL); c1[a] = cell_of(q[a] + r, CELL); }
    const double r2 = (double)r * r;
    for (int32_t x = c0[0]; x <= c1[0]; x++) for (int32_t y = c0[1]; y <= c1[1]; y++) for (int32_t z = c0[2]; z <= c1[2]; z++) {
        uint32_t b = hcell(x, y, z) & f->cg_mask;
        for (int32_t e = f->cg_head[b]; e >= 0; e = f->cg_next[e]) {
            int32_t t = e / 3;
            if (f->stamp[t] == qid) continue;
            const float *p = f->nt[t].p[e % 3];
            double dx = (double)p[0] - q[0], dy = (double)p[1] - q[1], dz = (double)p[2] - q[2];
            if (dx * dx + dy * dy + dz * dz <= r2) { f->stamp[t] = qid; if (n < cap) out[n++] = t; }
        }
    }
    return n;
}

static float tol_at(const NsFillCfg *cfg, float z);
static float surf_pad_of(const NsFillCfg *cfg) {   /* tolerancia de superficie hasta 20 m de profundidad */
    float t = cfg->tol_base + cfg->tol_z * 20000.0f;
    (void)t; (void)cfg; return 150.0f;
}
static float tol_at(const NsFillCfg *cfg, float z) {
    float t = cfg->tol_base + cfg->tol_z * (z > 0 ? z : 0);
    return t > TOL_CAP ? TOL_CAP : t;
}

static uint16_t op_page(const GpuOp *o) { return (uint16_t)(((o->tx / 64) & 15) | (((o->ty / 256) & 1) << 4) | ((o->tp & 3) << 7)); }
static uint16_t op_clut(const GpuOp *o) { return (uint16_t)(((o->clx / 16) & 63) | ((o->cly & 511) << 6)); }
static const int PERMS[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};

static double tri_dist(const FTri *t, float q[3][3]) {
    double best = 1e30;
    for (int pp = 0; pp < 6; pp++) {
        double m = 0;
        for (int k = 0; k < 3 && m < best; k++) { double d = d3(q[k], t->p[PERMS[pp][k]]); if (d > m) m = d; }
        if (m < best) best = m;
    }
    return best;
}
static int same_tex(const FTri *t, uint16_t page, uint16_t clut, int textured) {
    if (!textured) return 1;
    return (t->page & PAGE_MASK) == (page & PAGE_MASK) && t->clut == clut;
}

/* ---------------------------------------------------------------------------------------------------------------- una primitiva de la lista */
typedef struct {
    float v[4][3];     /* espacio de vista */
    float w[4][3];     /* mundo del motor */
    float w2[4][3];    /* mundo con la cámara alternativa (si la hay) */
    int has_alt;
    unsigned uf;       /* unión de vflags */
    int nv, recon, skip, incoh;   /* skip: NF_SKIP_* o -1; incoh: algún vértice limpio no reproduce su pantalla */
} OpGeo;

static int recon_one(const GpuOp *op, int bad, float out[3]) {   /* plano de los 3 vértices fiables ∩ rayo del cuarto (px de pantalla / H) */
    const GpuVtx *a = &op->v[(bad + 1) & 3], *b = &op->v[(bad + 2) & 3], *c = &op->v[(bad + 3) & 3];
    float dxs = (float)op->v[bad].x - (float)op->dox - op->proj_ox, dys = (float)op->v[bad].y - (float)op->doy - op->proj_oy;
    if (!(fabsf(dxs) < 1000.0f && fabsf(dys) < 1000.0f)) return 0;
    float e1[3] = {b->vx - a->vx, b->vy - a->vy, b->vz - a->vz}, e2[3] = {c->vx - a->vx, c->vy - a->vy, c->vz - a->vz};
    float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    float d[3] = {dxs / op->proj_h, dys / op->proj_h, 1.0f};
    float den = n[0] * d[0] + n[1] * d[1] + n[2] * d[2];
    float nn = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), dn = sqrtf(d[0] * d[0] + d[1] * d[1] + 1.0f);
    if (!(nn > 0 && fabsf(den) > 0.05f * nn * dn)) return 0;
    float t = (n[0] * a->vx + n[1] * a->vy + n[2] * a->vz) / den;
    float zmin = fminf(a->vz, fminf(b->vz, c->vz)), zmax = fmaxf(a->vz, fmaxf(b->vz, c->vz));
    if (!(isfinite(t) && t > 0 && zmin > 0 && t > 0.2f * zmin && t < 5.0f * zmax)) return 0;
    out[0] = d[0] * t; out[1] = d[1] * t; out[2] = t;
    return 1;
}

/* Posiciones de vista de los vértices de una primitiva: las de la GTE (vvalid) que reproducen su propia pantalla (un vértice limpio debe dar sus x,y enteros con H y OFX/OFY; si no, la
 * asociación con la GTE falló y su posición es de otro vértice). Si falta uno (sin vista o incoherente): plano de los otros tres ∩ rayo de su pantalla; si solo quedan dos esquinas
 * diagonales (cartel a z constante), las otras dos salen de ellas. 0 = sin posición (2D). */
static int op_view(const GpuOp *op, OpGeo *g) {
    int nv = op->nv == 4 ? 4 : 3, nval = 0, m = 0, good = 0;
    for (int k = 0; k < nv; k++) if (op->v[k].vvalid) { nval++; m |= 1 << k; }
    g->nv = nv; g->recon = 0; g->uf = 0; g->skip = -1; g->incoh = 0;
    if (!nval) return 0;
    int bad_mask = 0;
    for (int k = 0; k < nv; k++) {
        const GpuVtx *v = &op->v[k];
        if (!v->vvalid) continue;
        g->uf |= v->vflags;
        if (v->vflags == 0 && op->proj_h > 0 && v->vz > 64.0f) {
            float sx = (float)op->dox + op->proj_ox + op->proj_h * v->vx / v->vz, sy = (float)op->doy + op->proj_oy + op->proj_h * v->vy / v->vz;
            if (!(fabsf(sx - (float)v->x) <= 4.0f && fabsf(sy - (float)v->y) <= 4.0f)) { bad_mask |= 1 << k; continue; }
        }
        good |= 1 << k;
        g->v[k][0] = v->vx; g->v[k][1] = v->vy; g->v[k][2] = v->vz;
    }
    if (good == (1 << nv) - 1) return 1;
    if (nv == 4 && op->proj_h > 0 && isfinite(op->proj_h)) {
        int cnt = __builtin_popcount((unsigned)good);
        if (cnt == 3) {
            int bad = (~good) & 15;
            bad = __builtin_ctz((unsigned)bad);
            int clean = 1;
            for (int k = 0; k < 4; k++) if (k != bad && (op->v[k].vflags != 0 || !isfinite(op->v[k].vx + op->v[k].vy + op->v[k].vz))) clean = 0;
            float p[3];
            if (clean && recon_one(op, bad, p)) { memcpy(g->v[bad], p, sizeof p); g->recon = 1; return 1; }
        }
        if ((good & 15) == 9 || (good & 15) == 6 || (cnt == 2 && ((m & 9) == 9 || (m & 6) == 6))) {   /* esquinas diagonales de un cartel a z constante */
            int mm = ((good & 9) == 9) ? 9 : ((good & 6) == 6) ? 6 : 0;
            if (mm) {
                int p0 = mm == 9 ? 0 : 1, p1 = mm == 9 ? 3 : 2;
                const GpuVtx *a = &op->v[p0], *b = &op->v[p1];
                float zz = a->vz;
                if (isfinite(a->vx + a->vy + a->vz + b->vx + b->vy + b->vz) && zz > 0 && fabsf(b->vz - zz) <= 0.02f * zz + 1.0f && a->vx != b->vx && a->vy != b->vy) {
                    int k1 = mm == 9 ? 1 : 0, k2 = mm == 9 ? 2 : 3;
                    for (int k = 0; k < 4; k++) g->v[k][2] = zz;
                    g->v[p0][0] = a->vx; g->v[p0][1] = a->vy; g->v[p1][0] = b->vx; g->v[p1][1] = b->vy;
                    if (mm == 9) { g->v[k1][0] = b->vx; g->v[k1][1] = a->vy; g->v[k2][0] = a->vx; g->v[k2][1] = b->vy; }
                    else         { g->v[k1][0] = a->vx; g->v[k1][1] = b->vy; g->v[k2][0] = b->vx; g->v[k2][1] = a->vy; }
                    g->uf |= GPU_VF_DERIVED | a->vflags | b->vflags; g->recon = 1;
                    return 1;
                }
            }
        }
    }
    if (nval == nv) {   /* todos con vista pero alguno incoherente y sin reconstrucción posible: se usan tal cual y se marca */
        for (int k = 0; k < nv; k++) { g->v[k][0] = op->v[k].vx; g->v[k][1] = op->v[k].vy; g->v[k][2] = op->v[k].vz; }
        g->incoh = 1;
        return 1;
    }
    return 0;
}

/* resultado de clasificar un triángulo */
typedef struct { int cls; int fill; int bias; float bs[3], ba[3]; float tol; } TriRes;

static void classify_tri_(NsFill *f, const NsFillCfg *cfg, const GpuOp *o, const int ci[3], const OpGeo *g, int textured, TriRes *res);
static void classify_tri(NsFill *f, const NsFillCfg *cfg, const GpuOp *o, const int ci[3], const OpGeo *g, int textured, TriRes *res, uint32_t oi, int h) {
    classify_tri_(f, cfg, o, ci, g, textured, res);
    if (!cfg->dbg || res->cls <= NF_LOCKED) return;
    /* depuración: ¿existe la misma textura+UV en algún sitio del nativo (cualquier instancia)? */
    const uint16_t page = op_page(o), clut = op_clut(o);
    int best = -1; double bd = 1e30;
    float pc[3] = {0, 0, 0};
    for (int k = 0; k < 3; k++) for (int a = 0; a < 3; a++) pc[a] += g->w[ci[k]][a] / 3.0f;
    for (size_t i = 0; i < f->n; i++) {
        const FTri *t = &f->nt[i];
        if ((t->page & PAGE_MASK) != (page & PAGE_MASK) || t->clut != clut) continue;
        for (int pp = 0; pp < 6; pp++) {
            int ok = 1;
            for (int k = 0; k < 3; k++) { int pk = PERMS[pp][k]; const GpuVtx *v = &o->v[ci[k]]; if (fabsf((float)v->u - t->u[pk]) > 1.0f || fabsf((float)v->v - t->v[pk]) > 1.0f) { ok = 0; break; } }
            if (ok) { double d = d3(t->c, pc); if (d < bd) { bd = d; best = (int)i; } }
        }
    }
    fprintf((FILE *)cfg->dbg, "TRI op=%u h=%d cls=%s fill=%d semi=%d z=%.0f page=%04x clut=%04x vf=%u uv_en_nativo=%s dist=%.0f dyn=%d jugador=%d inst=%u tol=%.0f bs=(%.0f,%.0f,%.0f) ba=(%.0f,%.0f,%.0f) dw=(%.0f,%.0f,%.0f) vc=(%.0f,%.0f,%.0f)\n", oi, h, ns_fill_cls_name[res->cls], res->fill, (o->flags & GPU_F_SEMI) != 0,
            (g->v[ci[0]][2] + g->v[ci[1]][2] + g->v[ci[2]][2]) / 3.0f, page, clut, g->uf, best >= 0 ? "si" : "no", best >= 0 ? bd : -1.0, best >= 0 ? f->nt[best].dyn : -1, best >= 0 ? f->nt[best].player : -1, best >= 0 ? f->nt[best].inst : 0u, res->tol, res->bs[0], res->bs[1], res->bs[2], res->ba[0], res->ba[1], res->ba[2], best >= 0 ? pc[0] - f->nt[best].c[0] : 0.0, best >= 0 ? pc[1] - f->nt[best].c[1] : 0.0, best >= 0 ? pc[2] - f->nt[best].c[2] : 0.0,
            (g->v[ci[0]][0] + g->v[ci[1]][0] + g->v[ci[2]][0]) / 3, (g->v[ci[0]][1] + g->v[ci[1]][1] + g->v[ci[2]][1]) / 3, (g->v[ci[0]][2] + g->v[ci[1]][2] + g->v[ci[2]][2]) / 3);
}
static void classify_tri_(NsFill *f, const NsFillCfg *cfg, const GpuOp *o, const int ci[3], const OpGeo *g, int textured, TriRes *res) {
    enum { CANDMAX = 96 };
    int32_t cand[3][CANDMAX];
    float q[3][3], vq[3][3];
    for (int k = 0; k < 3; k++) { memcpy(q[k], g->w[ci[k]], sizeof q[k]); memcpy(vq[k], g->v[ci[k]], sizeof vq[k]); }
    const float zavg = (vq[0][2] + vq[1][2] + vq[2][2]) / 3.0f;
    const float tol = tol_at(cfg, zavg);
    const uint16_t page = op_page(o), clut = op_clut(o);
    const int semi = (o->flags & GPU_F_SEMI) != 0;
    res->fill = 0; res->bias = 0; res->tol = tol;
    for (int k = 0; k < 3; k++) res->bs[k] = res->ba[k] = -1.0f;
    /* 1. pareja exacta */
    int nc = corner_query(f, q[0], tol, cand[0], CANDMAX);
    double best = 1e30; int bj = -1;
    for (int i = 0; i < nc; i++) { double d = tri_dist(&f->nt[cand[0][i]], q); if (d < best) { best = d; bj = cand[0][i]; } }
    if (bj >= 0 && best <= tol) {
        const FTri *t = &f->nt[bj];
        if (textured && cfg->fix_texture && !t->player && ((t->page & PAGE_MASK) != (page & PAGE_MASK) || t->clut != clut)) { res->cls = NF_FILL_TEXFIX; res->fill = 1; res->bias = cfg->bias; return; }
        res->cls = NF_MATCH; return;
    }
    /* 1b. pegado a la cámara del motor: con la cámara alternativa sí casa (el nativo tiene el objeto un fotograma desplazado) */
    if (g->has_alt) {
        float q2[3][3];
        for (int k = 0; k < 3; k++) memcpy(q2[k], g->w2[ci[k]], sizeof q2[k]);
        int32_t c2[CANDMAX];
        int n2 = corner_query(f, q2[0], tol, c2, CANDMAX);
        double b2 = 1e30;
        for (int i = 0; i < n2; i++) { double d = tri_dist(&f->nt[c2[i]], q2); if (d < b2) b2 = d; }
        if (b2 <= tol) { res->cls = NF_LOCKED; return; }
    }
    /* 2. misma cara con otra diagonal: alguna cara nativa tiene esquinas cerca de las tres */
    {
        int cn[3];
        cn[0] = nc;
        for (int k = 1; k < 3; k++) cn[k] = corner_query(f, q[k], tol, cand[k], CANDMAX);
        if (cn[0] && cn[1] && cn[2]) {
            uint64_t tab[2][128];   /* caras (instancia, pieza, cara) de las esquinas 1 y 2: tabla abierta de 128 */
            memset(tab, 0, sizeof tab);
            for (int k = 1; k < 3; k++)
                for (int j = 0; j < cn[k]; j++) {
                    const FTri *tj = &f->nt[cand[k][j]];
                    uint64_t key = (((uint64_t)tj->inst * 0x9E3779B1u + tj->piece) * 0x85EBCA6Bu + tj->face) | 1ull << 63;
                    uint32_t h = (uint32_t)(key ^ (key >> 29)) & 127u;
                    while (tab[k - 1][h] && tab[k - 1][h] != key) h = (h + 1) & 127u;
                    tab[k - 1][h] = key;
                }
            for (int i = 0; i < cn[0]; i++) {
                const FTri *t0 = &f->nt[cand[0][i]];
                uint64_t key = (((uint64_t)t0->inst * 0x9E3779B1u + t0->piece) * 0x85EBCA6Bu + t0->face) | 1ull << 63;
                int ok = 1;
                for (int k = 0; k < 2 && ok; k++) {
                    uint32_t h = (uint32_t)(key ^ (key >> 29)) & 127u;
                    int found = 0;
                    while (tab[k][h]) { if (tab[k][h] == key) { found = 1; break; } h = (h + 1) & 127u; }
                    ok = found;
                }
                if (ok) { res->cls = NF_SPLIT; return; }
            }
        }
    }
    /* 3. sobre la superficie nativa */
    const float stol = cfg->surf_tol + tol;
    const float extra = stol > f->surf_pad ? stol - f->surf_pad : 0.0f;   /* más allá del margen con el que se inflaron las cajas: se mira también alrededor del punto */
    int on_same = 0, on_any = 0;
    for (int k = 0; k < 3; k++) {
        double bs = 1e30, ba = 1e30;
        const float *P = q[k];
        int32_t qid = ++f->qid;
        int32_t c0[3], c1[3];
        for (int a = 0; a < 3; a++) { c0[a] = cell_of(P[a] - extra, SCELL); c1[a] = cell_of(P[a] + extra, SCELL); }
        for (int32_t cx = c0[0]; cx <= c1[0]; cx++) for (int32_t cy = c0[1]; cy <= c1[1]; cy++) for (int32_t cz = c0[2]; cz <= c1[2]; cz++) {
            uint32_t b = hcell(cx, cy, cz) & f->sg_mask;
            for (int32_t nd = f->sg_head[b]; nd >= 0; nd = f->sg_node_next[nd]) {
                int32_t ti = f->sg_node_tri[nd];
                if (f->stamp[ti] == qid) continue;
                f->stamp[ti] = qid;
                const FTri *t = &f->nt[ti];
                if (P[0] < t->lo[0] - extra || P[0] > t->hi[0] + extra || P[1] < t->lo[1] - extra || P[1] > t->hi[1] + extra || P[2] < t->lo[2] - extra || P[2] > t->hi[2] + extra) continue;
                if (fabsf(t->n[0] * P[0] + t->n[1] * P[1] + t->n[2] * P[2] - t->dp) > stol) continue;   /* el plano ya está más lejos que la tolerancia */
                double d = pt_tri(P, t->p);
                if (d < ba) ba = d;
                if (d < bs && same_tex(t, page, clut, textured)) bs = d;
            }
        }
        for (size_t bi = 0; bi < f->big_n; bi++) {
            const FTri *t = &f->nt[f->big[bi]];
            if (P[0] < t->lo[0] - extra || P[0] > t->hi[0] + extra || P[1] < t->lo[1] - extra || P[1] > t->hi[1] + extra || P[2] < t->lo[2] - extra || P[2] > t->hi[2] + extra) continue;
            if (fabsf(t->n[0] * P[0] + t->n[1] * P[1] + t->n[2] * P[2] - t->dp) > stol) continue;
            double d = pt_tri(P, t->p);
            if (d < ba) ba = d;
            if (d < bs && same_tex(t, page, clut, textured)) bs = d;
        }
        res->bs[k] = (float)bs; res->ba[k] = (float)ba;
        if (bs <= stol) on_same++;
        if (ba <= stol) on_any++;
    }
    if (on_same == 3) { res->cls = NF_SUBDIV; return; }
    /* 4. misma textura y UV en un objeto dinámico cercano: otra pose */
    if (textured && f->tg_head) {
        uint32_t b = tex_key(page, clut) & f->tg_mask;
        float pc[3] = {(q[0][0] + q[1][0] + q[2][0]) / 3.0f, (q[0][1] + q[1][1] + q[2][1]) / 3.0f, (q[0][2] + q[1][2] + q[2][2]) / 3.0f};
        int pose = 0, player = 0;
        for (int32_t i = f->tg_head[b]; i >= 0 && !pose; i = f->tg_next[i]) {
            const FTri *t = &f->nt[i];
            if ((t->page & PAGE_MASK) != (page & PAGE_MASK) || t->clut != clut) continue;
            if (d3(t->c, pc) > cfg->pose_radius + t->r) continue;
            for (int pp = 0; pp < 6 && !pose; pp++) {
                int ok = 1;
                for (int k = 0; k < 3; k++) {
                    int pk = PERMS[pp][k]; const GpuVtx *v = &o->v[ci[k]];
                    if (fabsf((float)v->u - t->u[pk]) > 1.0f || fabsf((float)v->v - t->v[pk]) > 1.0f) { ok = 0; break; }
                }
                if (ok) { pose = 1; player = t->player; }
            }
        }
        if (pose) {
            res->cls = player ? NF_POSE_PLAYER : NF_POSE_OTHER;
            res->fill = (!player && cfg->pose_fill) ? 1 : 0;
            return;
        }
    }
    /* 5. el resto se rellena */
    if (fabsf(vq[0][2] - vq[1][2]) < 2.0f && fabsf(vq[0][2] - vq[2][2]) < 2.0f && on_any == 0) { res->cls = NF_FILL_SPRITE; res->fill = 1; return; }
    if (on_same > 0) {
        if (semi && !cfg->fill_partial_semi) { res->cls = NF_SKIP_PARTIAL_SEMI; return; }
        res->cls = NF_FILL_PARTIAL; res->fill = 1; res->bias = cfg->bias; return;
    }
    if (on_any > 0) { res->cls = NF_FILL_OTHERTEX; res->fill = 1; res->bias = cfg->bias; return; }
    res->cls = NF_FILL_ABSENT; res->fill = 1;
}

/* ---------------------------------------------------------------------------------------------------------------- salida: NsEffect */
static uint32_t sig_of(uint16_t page, uint16_t clut, uint16_t flags, int nv, const uint8_t uv[4][2]) {
    uint32_t h = 2166136261u;
    uint32_t su = 0, sv = 0;
    for (int k = 0; k < nv; k++) { su += uv[k][0]; sv += uv[k][1]; }
    uint32_t w[5] = {page, clut, flags, su, sv};
    for (int i = 0; i < 5; i++) { h ^= w[i]; h *= 16777619u; h ^= h >> 15; }
    return h;
}

static int make_effect(const GpuOp *o, const OpGeo *g, const int *idx, int n, int bias, uint32_t seq_id, NsEffect *e) {
    memset(e, 0, sizeof *e);
    int textured = (o->flags & GPU_F_TEXTURED) != 0, semi = (o->flags & GPU_F_SEMI) != 0, gour = (o->flags & GPU_F_GOURAUD) != 0, raw = (o->flags & GPU_F_RAW) != 0;
    uint8_t rgb[3] = {128, 128, 128};
    if (o->fbk) {   /* realimentación resuelta: un solo texel; se trata como sin textura con el color ya modulado */
        if (o->fbt == 0) return 0;
        textured = 0; gour = 0;
        int tr = o->fbt & 31, tg = (o->fbt >> 5) & 31, tb = (o->fbt >> 10) & 31;
        if (raw) { rgb[0] = (uint8_t)(tr * 8); rgb[1] = (uint8_t)(tg * 8); rgb[2] = (uint8_t)(tb * 8); }
        else {
            int r = (tr * o->v[0].r) >> 4, gg = (tg * o->v[0].g) >> 4, b = (tb * o->v[0].b) >> 4;
            rgb[0] = (uint8_t)(r > 255 ? 255 : r); rgb[1] = (uint8_t)(gg > 255 ? 255 : gg); rgb[2] = (uint8_t)(b > 255 ? 255 : b);
        }
        semi = semi && (o->fbt & 0x8000);
    }
    e->kind = NS_EK_QUAD; e->nv = (uint8_t)n;
    e->flags = (uint16_t)((semi ? NS_EF_SEMI : 0) | (textured ? NS_EF_TEXTURED : 0) | (gour ? NS_EF_GOURAUD : 0) | NS_EF_NOFOG);
    e->depth_bias = (int16_t)bias; e->abr = (uint8_t)(o->abr & 3);
    e->tpage = textured ? op_page(o) : 0; e->clut = textured ? op_clut(o) : 0;
    e->src_key = NF_SRC_KEY | (seq_id & 0xFFFFFFu); e->idx = (uint16_t)idx[0]; e->sub = (uint8_t)n;
    memcpy(&e->box[0], &seq_id, sizeof seq_id);   /* operación de la lista (solo herramientas): src_key pasa a ser la identidad estable al persistir */
    for (int k = 0; k < n; k++) {
        const GpuVtx *v = &o->v[idx[k]];
        memcpy(e->p[k], g->w[idx[k]], sizeof e->p[k]);
        e->uv[k][0] = v->u; e->uv[k][1] = v->v;
        if (o->fbk) { memcpy(e->rgb[k], rgb, 3); }
        else if (textured && raw) { e->rgb[k][0] = e->rgb[k][1] = e->rgb[k][2] = 128; }
        else { const GpuVtx *c = gour ? v : &o->v[0]; e->rgb[k][0] = c->r; e->rgb[k][1] = c->g; e->rgb[k][2] = c->b; }
    }
    return 1;
}

/* ---------------------------------------------------------------------------------------------------------------- ejecución */
static int in_view(const FRec *r, const double A[9], const float eye[3], float H, float cx, float cy, int sw, int sh) {
    double d[3] = {(double)r->c[0] - eye[0], (double)r->c[1] - eye[1], (double)r->c[2] - eye[2]};
    double v[3];
    for (int i = 0; i < 3; i++) v[i] = A[3 * i] * d[0] + A[3 * i + 1] * d[1] + A[3 * i + 2] * d[2];
    if (v[2] < 32.0) return 0;
    double sx = cx + H * v[0] / v[2], sy = cy + H * v[1] / v[2];
    return sx >= 4 && sx < sw - 4 && sy >= 4 && sy < sh - 4;
}

float ns_fill_probe(NsFill *f, const NsFillNative *nat, const GpuOp *ops, uint32_t nops, const uint8_t *skip_op, const NsFillCam *cam, const NsFillCfg *cfg_in) {
    NsFillCfg cfg;
    if (cfg_in) cfg = *cfg_in; else ns_fill_cfg_default(&cfg);
    double A[9], Ai[9];
    if (!f || !cam || ns_fill_cam_maps(cam, A, Ai)) return -1.0f;
    if (index_build(f, nat, cfg.player_key, surf_pad_of(&cfg))) return -1.0f;
    f->idx_tris = nat->tris; f->idx_n = nat->n; f->idx_player = cfg.player_key;
    uint32_t total = 0;
    for (uint32_t i = 0; i < nops; i++) if (ops[i].type == GPU_OP_POLY && ops[i].v[0].vvalid && !(skip_op && skip_op[i])) total++;
    if (!total) return -1.0f;
    uint32_t step = total / 400u + 1u, seen = 0, hit = 0, n = 0;
    for (uint32_t i = 0; i < nops; i++) {
        const GpuOp *o = &ops[i];
        if (o->type != GPU_OP_POLY || (skip_op && skip_op[i])) continue;
        OpGeo g;
        if (!op_view(o, &g) || g.uf) continue;
        if ((seen++ % step) != 0) continue;
        float q[3][3];
        for (int k = 0; k < 3; k++) for (int r = 0; r < 3; r++) q[k][r] = (float)(cam->eye[r] + Ai[3 * r] * g.v[k][0] + Ai[3 * r + 1] * g.v[k][1] + Ai[3 * r + 2] * g.v[k][2]);
        if (d3(q[0], q[1]) < 2 || d3(q[0], q[2]) < 2 || d3(q[1], q[2]) < 2) continue;
        float tol = tol_at(&cfg, (g.v[0][2] + g.v[1][2] + g.v[2][2]) / 3.0f);
        int32_t cand[96];
        int nc = corner_query(f, q[0], tol, cand, 96);
        double best = 1e30;
        for (int c = 0; c < nc; c++) { double d = tri_dist(&f->nt[cand[c]], q); if (d < best) best = d; }
        n++; if (best <= tol) hit++;
    }
    return n ? (float)hit / (float)n : -1.0f;
}

static int rec_push(NsFill *f, const NsEffect *e, const float vc[3]) {
    GROW(f->nw, f->nw_cap, f->nw_n + 1);
    FRec *r = &f->nw[f->nw_n++];
    memset(r, 0, sizeof *r);
    r->e = *e;
    memcpy(r->vc, vc, sizeof r->vc);
    for (int a = 0; a < 3; a++) { float s = 0; for (int k = 0; k < e->nv; k++) s += e->p[k][a]; r->c[a] = s / (float)e->nv; }
    r->sig = sig_of(e->tpage, e->clut, e->flags, e->nv, e->uv);
    return 0;
}

int ns_fill_run(NsFill *f, const NsFillNative *nat, const GpuOp *ops, uint32_t nops, const uint8_t *skip_op, const NsFillCam *cam, const NsFillCam *alt, const NsFillCfg *cfg_in, uint32_t frame, NsFillStats *st) {
    double t0 = now_ms();
    NsFillCfg cfg;
    if (cfg_in) cfg = *cfg_in; else ns_fill_cfg_default(&cfg);
    if (!(cfg.tol_base > 0)) cfg.tol_base = 20.0f;
    if (!(cfg.surf_tol > 0)) cfg.surf_tol = 80.0f;
    if (cfg.screen_w <= 0) cfg.screen_w = 320;
    if (cfg.screen_h <= 0) cfg.screen_h = 256;
    if (!(cfg.max_edge > 0)) cfg.max_edge = 40000.0f;
    if (!(cfg.max_edge_flagged > 0)) cfg.max_edge_flagged = 6000.0f;
    if (!(cfg.max_dist > 0)) cfg.max_dist = 400000.0f;
    if (!(cfg.pose_radius > 0)) cfg.pose_radius = 2500.0f;
    if (!cfg.max_out) cfg.max_out = 6000;
    NsFillStats local;
    NsFillStats *S = st ? st : &local;
    memset(S, 0, sizeof *S);
    if (!f || !cam) return -1;
    double A[9], Ai[9], A2[9], Ai2[9];
    if (ns_fill_cam_maps(cam, A, Ai)) return -2;
    const int has_alt = alt && ns_fill_cam_maps(alt, A2, Ai2) == 0;
    S->ops = nops;
    if (!(f->idx_tris && nat && f->idx_tris == nat->tris && f->idx_n == nat->n && f->idx_player == cfg.player_key)) { if (index_build(f, nat, cfg.player_key, surf_pad_of(&cfg))) return -1; }
    f->idx_tris = NULL;
    S->nat_tris = (uint32_t)f->n;
    for (size_t i = 0; i < f->n; i++) S->nat_dyn += f->nt[i].dyn;
    double t1 = now_ms();
    S->ms_index = t1 - t0;

    /* corte de cámara / retroceso del contador: se vacía lo persistente */
    int cut = 0;
    {
        float fwd[3] = {cam->R[2], cam->R[5], cam->R[8]};
        float fl = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
        if (fl > 0) { fwd[0] /= fl; fwd[1] /= fl; fwd[2] /= fl; }
        if (f->have_last) {
            double dd = d3(f->last_eye, cam->eye), cs = fwd[0] * f->last_fwd[0] + fwd[1] * f->last_fwd[1] + fwd[2] * f->last_fwd[2];
            if (dd > cfg.cut_dist || cs < cfg.cut_cos || frame < f->last_frame) cut = 1;
        }
        memcpy(f->last_eye, cam->eye, sizeof f->last_eye); memcpy(f->last_fwd, fwd, sizeof fwd); f->last_frame = frame; f->have_last = 1;
    }

    f->nw_n = 0;
    float proj_h = 160.0f, pcx = cfg.screen_w * 0.5f, pcy = cfg.screen_h * 0.5f;
    int have_proj = 0;
    uint32_t n3d_ops = 0;
    for (uint32_t oi = 0; oi < nops; oi++) {
        const GpuOp *o = &ops[oi];
        if (o->type != GPU_OP_POLY || o->nv < 3) continue;
        if (skip_op && skip_op[oi]) { S->skipped_ops++; continue; }
        OpGeo g;
        if (!op_view(o, &g)) { S->no_view++; continue; }
        S->polys3d++; n3d_ops++;
        if (g.recon) S->recon++;
        if (!have_proj && o->proj_h > 0) { proj_h = o->proj_h; pcx = (float)o->dox + o->proj_ox; pcy = (float)o->doy + o->proj_oy; have_proj = 1; }
        const int nv = g.nv, ntri = nv == 4 ? 2 : 1;
        const int textured = (o->flags & GPU_F_TEXTURED) && !o->fbk;
        /* posición fiable? NaN/vacío antes de casar; el resto de las reservas (detrás de la cámara, ambiguo, aristas desproporcionadas) solo si habría que rellenar: lo que el nativo
         * ya cubre se cubre aunque la posición de la lista sea dudosa */
        int skip = -1, gate = -1;
        float emax = 0, dmax = 0;
        for (int k = 0; k < nv && skip < 0; k++) {
            if (!isfinite(g.v[k][0] + g.v[k][1] + g.v[k][2])) { skip = NF_SKIP_NAN; break; }
            float dd = sqrtf(g.v[k][0] * g.v[k][0] + g.v[k][1] * g.v[k][1] + g.v[k][2] * g.v[k][2]);
            if (dd > dmax) dmax = dd;
            const float *a = g.v[k], *b = g.v[(k + 1) % nv];
            float e = sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
            if (e > emax) emax = e;
        }
        if (skip < 0 && dmax > 4.0f * cfg.max_dist) skip = NF_SKIP_FAR;   /* tan lejos que ni la rejilla lo aguanta */
        if (skip >= 0) { S->cls[skip] += (uint32_t)ntri; S->tris += (uint32_t)ntri; continue; }
        if (g.uf & GPU_VF_BEHIND) gate = NF_SKIP_BEHIND;
        else if (((g.uf & GPU_VF_AMBIG) && (g.uf & GPU_VF_SAT)) || ((g.uf & GPU_VF_DERIVED) && (g.uf & GPU_VF_SAT))) gate = NF_SKIP_AMBIG;
        else if (g.incoh) gate = NF_SKIP_INCOH;
        else if (dmax > cfg.max_dist) gate = NF_SKIP_FAR;
        else if (emax > ((g.uf || g.recon) ? cfg.max_edge_flagged : cfg.max_edge)) gate = NF_SKIP_EDGE;
        else if (o->fbk && o->fbt == 0) gate = NF_SKIP_FB;
        g.has_alt = has_alt;
        for (int k = 0; k < nv; k++) {
            for (int r = 0; r < 3; r++) g.w[k][r] = (float)(cam->eye[r] + Ai[3 * r] * g.v[k][0] + Ai[3 * r + 1] * g.v[k][1] + Ai[3 * r + 2] * g.v[k][2]);
            if (has_alt) for (int r = 0; r < 3; r++) g.w2[k][r] = (float)(alt->eye[r] + Ai2[3 * r] * g.v[k][0] + Ai2[3 * r + 1] * g.v[k][1] + Ai2[3 * r + 2] * g.v[k][2]);
        }
        TriRes tr[2];
        int ok[2] = {0, 0};
        for (int h = 0; h < ntri; h++) {
            const int ci[3] = {h ? 1 : 0, h ? 2 : 1, h ? 3 : 2};
            if (d3(g.v[ci[0]], g.v[ci[1]]) < 2 || d3(g.v[ci[0]], g.v[ci[2]]) < 2 || d3(g.v[ci[1]], g.v[ci[2]]) < 2) { S->degenerate++; continue; }
            S->tris++;
            classify_tri(f, &cfg, o, ci, &g, textured, &tr[h], oi, h);
            ok[h] = 1;
            if (tr[h].fill && gate >= 0) { tr[h].cls = gate; tr[h].fill = 0; }
            else if (tr[h].fill && cfg.near_skip > 0.0f) {   /* centro a menos de near_skip del ojo: envoltura del jugador / pegado al plano de la cámara, rígido a la cabeza una vez en el mundo */
                float mx = (g.v[ci[0]][0] + g.v[ci[1]][0] + g.v[ci[2]][0]) / 3.0f, my = (g.v[ci[0]][1] + g.v[ci[1]][1] + g.v[ci[2]][1]) / 3.0f, mz = (g.v[ci[0]][2] + g.v[ci[1]][2] + g.v[ci[2]][2]) / 3.0f;
                if (mx * mx + my * my + mz * mz < cfg.near_skip * cfg.near_skip) { tr[h].cls = NF_SKIP_NEAR; tr[h].fill = 0; }
            }
            S->cls[tr[h].cls]++;
        }
        /* emisión: cuadrilátero entero si las dos mitades se rellenan igual, si no cada mitad */
        if (nv == 4 && ok[0] && ok[1] && tr[0].fill && tr[1].fill && tr[0].bias == tr[1].bias) {
            static const int I4[4] = {0, 1, 2, 3};
            NsEffect e;
            float vc[3] = {0, 0, 0};
            for (int k = 0; k < 4; k++) for (int r = 0; r < 3; r++) vc[r] += g.v[k][r] * 0.25f;
            if (make_effect(o, &g, I4, 4, tr[0].bias, o->seq, &e)) { rec_push(f, &e, vc); S->filled_tris += 2; }
            else S->cls[NF_SKIP_FB] += 2;
        } else {
            for (int h = 0; h < ntri; h++) {
                if (!ok[h] || !tr[h].fill) continue;
                const int ci[3] = {h ? 1 : 0, h ? 2 : 1, h ? 3 : 2};
                NsEffect e;
                float vc[3];
                for (int r = 0; r < 3; r++) vc[r] = (g.v[ci[0]][r] + g.v[ci[1]][r] + g.v[ci[2]][r]) / 3.0f;
                if (make_effect(o, &g, ci, 3, tr[h].bias, o->seq * 2u + (uint32_t)h, &e)) { rec_push(f, &e, vc); S->filled_tris++; }
                else S->cls[NF_SKIP_FB]++;
            }
        }
    }
    double t2 = now_ms();
    S->ms_match = t2 - t1;

    /* persistencia: lo visto antes y no visto ahora se conserva anclado al mundo mientras no esté en la vista actual */
    {
        if (cut || !cfg.keep_frames) { S->retired_cut = (uint32_t)f->alive_n; f->alive_n = 0; }
        size_t nprev = f->alive_n;
        size_t nb = 64;
        while (nb < nprev * 2) nb <<= 1;
        if (nprev) {
            GROW(f->ph_head, f->ph_nb, nb); GROW(f->ph_next, f->ph_cap, nprev);
            for (size_t i = 0; i < nb; i++) f->ph_head[i] = -1;
            for (size_t i = 0; i < nprev; i++) {
                f->alive[i].claimed = 0;
                uint32_t b = hcell(cell_of(f->alive[i].c[0], 64.0f), cell_of(f->alive[i].c[1], 64.0f), cell_of(f->alive[i].c[2], 64.0f)) & (uint32_t)(nb - 1);
                f->ph_next[i] = f->ph_head[b]; f->ph_head[b] = (int32_t)i;
            }
            const float vm = cfg.view_match;
            if (vm > 0.0f) {   /* índice por VISTA de lo visto hace poco (lo persistido de hace más no tiene una posición de vista vigente) */
                GROW(f->vh_head, f->vh_nb, nb); GROW(f->vh_next, f->vh_cap, nprev);
                for (size_t i = 0; i < nb; i++) f->vh_head[i] = -1;
                for (size_t i = 0; i < nprev; i++) {
                    f->vh_next[i] = -1;
                    if (frame - f->alive[i].last > 3u) continue;
                    uint32_t b = hcell(cell_of(f->alive[i].vc[0], 64.0f), cell_of(f->alive[i].vc[1], 64.0f), cell_of(f->alive[i].vc[2], 64.0f)) & (uint32_t)(nb - 1);
                    f->vh_next[i] = f->vh_head[b]; f->vh_head[b] = (int32_t)i;
                }
            }
            for (size_t k = 0; k < f->nw_n; k++) {
                FRec *r = &f->nw[k];
                int32_t cx = cell_of(r->c[0], 64.0f), cy = cell_of(r->c[1], 64.0f), cz = cell_of(r->c[2], 64.0f);
                for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) for (int dz = -1; dz <= 1; dz++) {
                    uint32_t b = hcell(cx + dx, cy + dy, cz + dz) & (uint32_t)(nb - 1);
                    for (int32_t i = f->ph_head[b]; i >= 0; i = f->ph_next[i]) {
                        FRec *p = &f->alive[i];
                        if (!p->claimed && p->sig == r->sig && d3(p->c, r->c) <= 48.0) { p->claimed = 1; if (!r->id) r->id = p->id; }
                    }
                }
                if (vm > 0.0f) {
                    int32_t vx = cell_of(r->vc[0], 64.0f), vy = cell_of(r->vc[1], 64.0f), vz = cell_of(r->vc[2], 64.0f);
                    for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) for (int dz = -1; dz <= 1; dz++) {
                        uint32_t b = hcell(vx + dx, vy + dy, vz + dz) & (uint32_t)(nb - 1);
                        for (int32_t i = f->vh_head[b]; i >= 0; i = f->vh_next[i]) {
                            FRec *p = &f->alive[i];
                            if (!p->claimed && p->sig == r->sig && d3(p->vc, r->vc) <= vm) { p->claimed = 1; if (!r->id) r->id = p->id; }   /* misma firma y misma posición de vista: el mismo elemento aunque el mundo haya cambiado */
                        }
                    }
                }
            }
        }
        for (size_t k = 0; k < f->nw_n; k++) {   /* identidad estable: heredada o nueva */
            FRec *r = &f->nw[k];
            if (!r->id) { f->next_id = (f->next_id + 1u) & 0xFFFFFFu; if (!f->next_id) f->next_id = 1u; r->id = f->next_id; }
            r->e.src_key = NF_SRC_KEY | r->id;
        }
        for (size_t k = 0; k < f->nw_n; k++) f->nw[k].last = frame;
        size_t nnew = f->nw_n;
        for (size_t i = 0; i < nprev; i++) {
            FRec *p = &f->alive[i];
            if (p->claimed) continue;
            if (frame - p->last > cfg.keep_frames) { S->retired_age++; continue; }
            if (n3d_ops && in_view(p, A, cam->eye, proj_h, pcx, pcy, cfg.screen_w, cfg.screen_h)) { S->retired_view++; continue; }
            if (f->nw_n >= cfg.max_out) break;
            GROW(f->nw, f->nw_cap, f->nw_n + 1);
            f->nw[f->nw_n++] = *p;
            S->persisted++;
        }
        if (f->nw_n > cfg.max_out) { S->cls[NF_SKIP_CAP] += (uint32_t)(f->nw_n - cfg.max_out); f->nw_n = cfg.max_out; }
        (void)nnew;
        /* intercambio de buffers */
        FRec *tmp = f->alive; size_t tc = f->alive_cap;
        f->alive = f->nw; f->alive_cap = f->nw_cap; f->alive_n = f->nw_n;
        f->nw = tmp; f->nw_cap = tc; f->nw_n = 0;
        GROW(f->out, f->out_cap, f->alive_n + 1);
        for (size_t i = 0; i < f->alive_n; i++) f->out[i] = f->alive[i].e;
        f->out_n = f->alive_n;
        S->n_out = (uint32_t)f->out_n;
    }
    double t3 = now_ms();
    S->ms_persist = t3 - t2;
    S->ms_total = t3 - t0;

    uint32_t cov = S->cls[NF_MATCH] + S->cls[NF_SPLIT] + S->cls[NF_SUBDIV] + S->cls[NF_LOCKED];
    uint32_t pose = S->cls[NF_POSE_PLAYER] + S->cls[NF_POSE_OTHER];
    uint32_t fill_cls = S->cls[NF_FILL_ABSENT] + S->cls[NF_FILL_OTHERTEX] + S->cls[NF_FILL_PARTIAL] + S->cls[NF_FILL_SPRITE] + S->cls[NF_FILL_TEXFIX];
    uint32_t denom = S->tris > pose ? S->tris - pose : 0;
    /* los POSE que se rellenan (pose_fill) cuentan como cubiertos */
    uint32_t posefilled = cfg.pose_fill ? S->cls[NF_POSE_OTHER] : 0;
    S->cover_pct = denom ? 100.0f * (float)(cov + fill_cls + posefilled) / (float)denom : 100.0f;
    S->nat_pct = denom ? 100.0f * (float)cov / (float)denom : 100.0f;
    return 0;
}

size_t ns_fill_format(const NsFillStats *s, uint32_t frame, char *buf, size_t cap) {
    if (!buf || !cap) return 0;
    uint32_t pose = s->cls[NF_POSE_PLAYER] + s->cls[NF_POSE_OTHER];
    int n = snprintf(buf, cap,
        "[relleno] fr=%u lista: tris3D=%u (poligonos=%u sin_vista=%u reconstruidos=%u degenerados=%u efectos_excluidos=%u) | cubierto_nativo=%u (exacto=%u otra_diagonal=%u subdividido=%u pegado_a_camara=%u) %.1f%% | "
        "rellenado=%u (ausente=%u otra_textura=%u parcial=%u sprite=%u textura_corregida=%u) | pose=%u (jugador=%u otro=%u) | sin_posicion_fiable: detras=%u ambiguo=%u incoherente=%u arista=%u lejos=%u nan=%u texel_unico=%u parcial_semi=%u | "
        "junto_a_cabeza=%u | cobertura(nativo+relleno, sin pose)=%.1f%% | salida=%u (persistentes=%u retirados: vista=%u edad=%u corte=%u tope=%u) | nativo: tris=%u dinamicos=%u | %.2f ms (indice %.2f, casar %.2f, persistencia %.2f)\n",
        frame, s->tris, s->polys3d, s->no_view, s->recon, s->degenerate, s->skipped_ops, s->cls[NF_MATCH] + s->cls[NF_SPLIT] + s->cls[NF_SUBDIV] + s->cls[NF_LOCKED], s->cls[NF_MATCH], s->cls[NF_SPLIT], s->cls[NF_SUBDIV], s->cls[NF_LOCKED], s->nat_pct,
        s->filled_tris, s->cls[NF_FILL_ABSENT], s->cls[NF_FILL_OTHERTEX], s->cls[NF_FILL_PARTIAL], s->cls[NF_FILL_SPRITE], s->cls[NF_FILL_TEXFIX], pose, s->cls[NF_POSE_PLAYER], s->cls[NF_POSE_OTHER],
        s->cls[NF_SKIP_BEHIND], s->cls[NF_SKIP_AMBIG], s->cls[NF_SKIP_INCOH], s->cls[NF_SKIP_EDGE], s->cls[NF_SKIP_FAR], s->cls[NF_SKIP_NAN], s->cls[NF_SKIP_FB], s->cls[NF_SKIP_PARTIAL_SEMI],
        s->cls[NF_SKIP_NEAR], s->cover_pct, s->n_out, s->persisted, s->retired_view, s->retired_age, s->retired_cut, s->cls[NF_SKIP_CAP], s->nat_tris, s->nat_dyn, s->ms_total, s->ms_index, s->ms_match, s->ms_persist);
    if (n < 0) return 0;
    return (size_t)n < cap ? (size_t)n : cap - 1;
}
