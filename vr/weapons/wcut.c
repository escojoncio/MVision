/* wcut: ver wcut.h. Algoritmo:
 *  1. Los triángulos del modelo (sin indexar) se validan y se ingieren corregidos (orientación por triángulo y por
 *     componente conexa); el cuerpo empieza con todos.
 *  2. Cada regla extrae de su origen los triángulos que cumplen la selección (caja y/o componente y/o UV). Con
 *     caja en modo CLIP los triángulos que cruzan se parten contra los 6 planos (Sutherland-Hodgman; las
 *     intersecciones se calculan en orden canónico, así dos triángulos vecinos obtienen los mismos puntos EXACTOS).
 *  3. Interfaz = aristas del contorno de la pieza cuyo gemelo (invertido) está en el resto. Se encadenan en bucles;
 *     los bucles coplanares se clasifican por inclusión (contornos exteriores y agujeros) y cada exterior se
 *     triangula con sus agujeros (puentes + orejas) con UV planares tomadas de la zona de atlas indicada: tapa de la
 *     pieza y relleno del hueco (los mismos triángulos con el orden invertido). Ambas mallas quedan cerradas
 *     allí donde el original lo era.
 *  Robustez: la entrada se valida (finita, índices en rango) y toda la ejecución tiene tope de triángulos por lista,
 *  de empujes totales y de trabajo; si se agotan, error -3 en vez de colgarse o agotar la memoria. Las tolerancias
 *  geométricas escalan con la magnitud de las coordenadas (g_sc). */
#include "wcut.h"
#include <stdlib.h>
#include <string.h>

/* ---- contexto de la ejecución en curso (por hilo) ---- */
static _Thread_local double g_sc = 1.0;      /* escala de coordenadas (>= 1): las tolerancias absolutas se multiplican por ella */
static _Thread_local size_t g_cap = 0;       /* tope de triángulos por lista (0 = sin tope: utilidades fuera de wcut_run) */
static _Thread_local double g_push = 0;      /* empujes de triángulo que quedan */
static _Thread_local double g_work = 0;      /* trabajo (pasos elementales) que queda */
#define EPS_D (2e-7 * g_sc)                  /* tolerancia de pertenencia a un plano (m) */
#define SC2   (g_sc * g_sc)
#define MAXCOORD 1.0e4                       /* |coordenada| máxima admitida en la entrada (m) */

static int spend(double w) { if (g_cap && (g_work -= w) < 0) return -3; return 0; }

typedef struct { float p[3], n[3], uv[2]; uint8_t c[4]; } Vx;
typedef struct { Vx v[3]; int comp; uint8_t cap; } Tri;      /* cap: WCUT_TRI_* */
typedef struct { Tri *t; size_t n, cap; } TL;
typedef struct { Vx v[14]; int n; } Poly;

static int tl_push(TL *l, const Tri *t) {
    if (g_cap && (l->n >= g_cap || --g_push < 0)) return -3;
    if (l->n == l->cap) {
        size_t c = l->cap ? l->cap * 2 : 64;
        Tri *x = (Tri *)realloc(l->t, c * sizeof(Tri));
        if (!x) return -3;
        l->t = x; l->cap = c;
    }
    l->t[l->n++] = *t;
    return 0;
}
static void tl_free(TL *l) { free(l->t); memset(l, 0, sizeof *l); }

/* ------------------------------------------------------------------ vectores */
static void sub3(const float *a, const float *b, double *o) { for (int i = 0; i < 3; i++) o[i] = (double)a[i] - b[i]; }
static void cross3(const double *a, const double *b, double *o) {
    o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}
static double tri_area2(const Tri *t) {   /* 2 * área */
    double a[3], b[3], c[3];
    sub3(t->v[1].p, t->v[0].p, a); sub3(t->v[2].p, t->v[0].p, b); cross3(a, b, c);
    return sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
}
#define AREA_MIN (1e-13 * SC2)   /* por debajo, triángulo degenerado */
/* triángulo con dos vértices coincidentes (se descarta; los colineales se conservan: cosen T-uniones) */
static int tri_coinc(const Tri *t) {
    for (int i = 0; i < 3; i++) if (!memcmp(t->v[i].p, t->v[(i + 1) % 3].p, 12)) return 1;
    return 0;
}
static void norm_f(float *n) {
    float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l > 1e-12f) { n[0] /= l; n[1] /= l; n[2] /= l; } else { n[0] = 0; n[1] = 1; n[2] = 0; }
}

/* ------------------------------------------------------------------ claves exactas de posición */
typedef struct { uint32_t k[3]; } PK;
static PK pkey(const float *p) {
    PK r; float q[3] = {p[0] + 0.0f, p[1] + 0.0f, p[2] + 0.0f};   /* -0 -> +0 */
    memcpy(r.k, q, 12); return r;
}
typedef struct { PK a, b; } EK;   /* arista dirigida a -> b */
static int ek_cmp(const void *x, const void *y) { return memcmp(x, y, sizeof(EK)); }

static EK *edges_of(const TL *l, size_t *n) {
    EK *e = (EK *)malloc((l->n * 3 + 1) * sizeof(EK));
    if (!e) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < l->n; i++)
        for (int a = 0; a < 3; a++) {
            e[k].a = pkey(l->t[i].v[a].p); e[k].b = pkey(l->t[i].v[(a + 1) % 3].p);
            if (memcmp(&e[k].a, &e[k].b, sizeof(PK)) != 0) k++;   /* aristas de longitud 0 no cuentan */
        }
    qsort(e, k, sizeof(EK), ek_cmp);
    *n = k; return e;
}
static int has_rev(const EK *s, size_t n, const EK *e) {
    EK r; r.a = e->b; r.b = e->a;
    return n && bsearch(&r, s, n, sizeof(EK), ek_cmp) != NULL;
}

/* ------------------------------------------------------------------ caja */
typedef struct { double c[3], h[3], R[3][3]; } BoxX;   /* local = R^T (p - c) */
static void box_make(const WCutBox *b, BoxX *o) {
    double cx = cos(b->rot.x), sx = sin(b->rot.x), cy = cos(b->rot.y), sy = sin(b->rot.y), cz = cos(b->rot.z), sz = sin(b->rot.z);
    double Rx[3][3] = {{1, 0, 0}, {0, cx, -sx}, {0, sx, cx}};
    double Ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
    double Rz[3][3] = {{cz, -sz, 0}, {sz, cz, 0}, {0, 0, 1}};
    double T[3][3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { T[i][j] = 0; for (int k = 0; k < 3; k++) T[i][j] += Rz[i][k] * Ry[k][j]; }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { o->R[i][j] = 0; for (int k = 0; k < 3; k++) o->R[i][j] += T[i][k] * Rx[k][j]; }
    o->c[0] = b->c.x; o->c[1] = b->c.y; o->c[2] = b->c.z;
    o->h[0] = b->h.x; o->h[1] = b->h.y; o->h[2] = b->h.z;
}
/* distancia con signo al plano k (0..5: +x,-x,+y,-y,+z,-z locales); <= 0 dentro */
static double box_d(const BoxX *b, const float *p, int k) {
    int a = k >> 1; double q = 0;
    for (int i = 0; i < 3; i++) q += b->R[i][a] * ((double)p[i] - b->c[i]);
    return (k & 1) ? -q - b->h[a] : q - b->h[a];
}
static int box_inside(const BoxX *b, const float *p) {
    for (int k = 0; k < 6; k++) if (box_d(b, p, k) > EPS_D) return 0;
    return 1;
}
#define ON_TOL (2.5 * EPS_D)        /* un punto a menos de esto de un plano de la caja "está en el plano" */
#define PERI_TOL (10.0 * EPS_D)     /* ... y a menos de esto del perímetro de una cara "está en el perímetro" */

/* ------------------------------------------------------------------ recorte de polígonos */
static Vx vx_lerp(const Vx *a, const Vx *b, double t) {
    Vx r; float f = (float)t;
    for (int i = 0; i < 3; i++) { r.p[i] = a->p[i] + f * (b->p[i] - a->p[i]); r.n[i] = a->n[i] + f * (b->n[i] - a->n[i]); }
    for (int i = 0; i < 2; i++) r.uv[i] = a->uv[i] + f * (b->uv[i] - a->uv[i]);
    for (int i = 0; i < 4; i++) r.c[i] = (uint8_t)(a->c[i] + (int)((b->c[i] - (int)a->c[i]) * t + (b->c[i] >= a->c[i] ? 0.5 : -0.5)));
    norm_f(r.n);
    return r;
}
static int p_less(const float *a, const float *b) {
    for (int i = 0; i < 3; i++) { if (a[i] < b[i]) return 1; if (a[i] > b[i]) return 0; }
    return 0;
}
/* intersección canónica de la arista a-b (da, db = distancias con signo) */
static Vx cut_edge(const Vx *a, double da, const Vx *b, double db) {
    if (p_less(b->p, a->p)) { const Vx *t = a; a = b; b = t; double d = da; da = db; db = d; }
    double t = da / (da - db);
    return vx_lerp(a, b, t);
}
/* Un polígono contenido en el plano k (todos sus vértices a +-EPS): pertenece al interior solo si mira hacia fuera
 * de la caja por esa cara (el sólido queda detrás, dentro); si mira hacia dentro, el sólido está fuera. */
static void clip_plane(const BoxX *bx, int k, const Poly *in, Poly *ins, Poly *out) {
    double d[16]; double mx = -1e30, mn = 1e30;
    for (int i = 0; i < in->n; i++) { d[i] = box_d(bx, in->v[i].p, k); if (d[i] > mx) mx = d[i]; if (d[i] < mn) mn = d[i]; }
    ins->n = out->n = 0;
    if (mx <= EPS_D && mn >= -EPS_D) {   /* coplanar */
        double nw[3] = {0, 0, 0}, on[3]; int a = k >> 1;
        for (int i = 0; i < in->n; i++) {
            const float *p = in->v[i].p, *q = in->v[(i + 1) % in->n].p;
            nw[0] += ((double)p[1] - q[1]) * ((double)p[2] + q[2]);
            nw[1] += ((double)p[2] - q[2]) * ((double)p[0] + q[0]);
            nw[2] += ((double)p[0] - q[0]) * ((double)p[1] + q[1]);
        }
        for (int r = 0; r < 3; r++) on[r] = (k & 1) ? -bx->R[r][a] : bx->R[r][a];
        if (nw[0] * on[0] + nw[1] * on[1] + nw[2] * on[2] >= 0) *ins = *in; else *out = *in;
        return;
    }
    if (mx <= EPS_D) { *ins = *in; return; }
    if (mn >= -EPS_D) { *out = *in; return; }
    for (int i = 0; i < in->n; i++) {
        int j = (i + 1) % in->n;
        if (d[i] <= EPS_D) ins->v[ins->n++] = in->v[i];
        if (d[i] >= -EPS_D) out->v[out->n++] = in->v[i];
        if ((d[i] < -EPS_D && d[j] > EPS_D) || (d[i] > EPS_D && d[j] < -EPS_D)) {
            Vx x = cut_edge(&in->v[i], d[i], &in->v[j], d[j]);
            ins->v[ins->n++] = x; out->v[out->n++] = x;
        }
    }
}

/* ------------------------------------------------------------------ polígonos de tapa */
typedef struct { float p[3]; } P3;
typedef struct { double x, y; } V2;

/* Triangulación por orejas de un anillo de índices sobre X (antihorario). Los índices pueden repetirse (puentes de
 * agujeros): una oreja ignora los puntos con el mismo índice que sus vértices. tri[] recibe m-2 triángulos como mucho. */
static int ear_clip(const V2 *X, int *ix, int m, int (*tri)[3], int *nt) {
    const double E = 1e-13 * SC2;
#define CR(A, B, C) ((X[B].x - X[A].x) * (X[C].y - X[A].y) - (X[B].y - X[A].y) * (X[C].x - X[A].x))
    int rc = 0;
    while (m >= 3 && !rc) {
        int pick = -1, best = -1; double bestcr = -1e300;
        if (m == 3) pick = 1;
        else for (int i = 0; i < m && pick < 0; i++) {
            if ((rc = spend(4.0 * m))) return rc;
            int A = ix[(i + m - 1) % m], B = ix[i], C = ix[(i + 1) % m];
            double cr = CR(A, B, C);
            if (cr > bestcr) { bestcr = cr; best = i; }
            if (cr <= E) continue;
            int ok = 1;
            for (int j = 0; j < m && ok; j++) {
                int Q = ix[j]; if (Q == A || Q == B || Q == C) continue;
                if (CR(A, B, Q) > -E && CR(B, C, Q) > -E && CR(C, A, Q) > -E) ok = 0;
            }
            if (ok) pick = i;
        }
        if (pick < 0) pick = best >= 0 ? best : 1;
        int A = ix[(pick + m - 1) % m], B = ix[pick], C = ix[(pick + 1) % m];
        if (CR(A, B, C) > E) { tri[*nt][0] = A; tri[*nt][1] = B; tri[*nt][2] = C; (*nt)++; }
        for (int j = pick; j + 1 < m; j++) ix[j] = ix[j + 1];
        m--;
    }
#undef CR
    return rc;
}

static int pip(const V2 *P, int n, V2 q) {   /* punto en polígono (cruces de rayo) */
    int in = 0;
    for (int i = 0, j = n - 1; i < n; j = i++)
        if ((P[i].y > q.y) != (P[j].y > q.y) && q.x < (P[j].x - P[i].x) * (q.y - P[i].y) / (P[j].y - P[i].y) + P[i].x) in = !in;
    return in;
}
static double area2_v2(const V2 *P, int n) {   /* 2 * área con signo (positivo = antihorario) */
    double a = 0;
    for (int i = 0; i < n; i++) { int j = (i + 1) % n; a += P[i].x * P[j].y - P[j].x * P[i].y; }
    return a;
}
static void newell(const P3 *p, int n, double *nw) {
    nw[0] = nw[1] = nw[2] = 0;
    for (int i = 0; i < n; i++) {
        const float *a = p[i].p, *b = p[(i + 1) % n].p;
        nw[0] += ((double)a[1] - b[1]) * ((double)a[2] + b[2]);
        nw[1] += ((double)a[2] - b[2]) * ((double)a[0] + b[0]);
        nw[2] += ((double)a[0] - b[0]) * ((double)a[1] + b[1]);
    }
}

/* Une un agujero (horario) al anillo exterior (antihorario) con un puente al vértice visible más cercano por la
 * derecha de su vértice de x máxima; inserta [M, agujero..., M, P] tras P (índices repetidos). */
static int bridge_hole(const V2 *X, int *ring, int *m, int hs, int hn) {
    int mi = hs;
    for (int i = 1; i < hn; i++) if (X[hs + i].x > X[mi].x) mi = hs + i;
    V2 M = X[mi]; int n = *m;
    double bestx = 1e300; int bi = -1;
    for (int i = 0; i < n; i++) {
        V2 A = X[ring[i]], B = X[ring[(i + 1) % n]];
        if (A.y == B.y) continue;
        if (!((A.y <= M.y && B.y >= M.y) || (A.y >= M.y && B.y <= M.y))) continue;
        double ix = A.x + (M.y - A.y) / (B.y - A.y) * (B.x - A.x);
        if (ix >= M.x - 1e-12 * g_sc && ix < bestx) { bestx = ix; bi = i; }
    }
    if (bi < 0) return -7;
    int pa = bi, pb = (bi + 1) % n, pp;
    V2 A = X[ring[pa]], B = X[ring[pb]];
    if (fabs(bestx - A.x) < 1e-12 * g_sc && A.y == M.y) pp = pa;
    else if (fabs(bestx - B.x) < 1e-12 * g_sc && B.y == M.y) pp = pb;
    else pp = A.x > B.x ? pa : pb;
    V2 I = {bestx, M.y}, P = X[ring[pp]];
    double tri_or = (I.x - M.x) * (P.y - M.y) - (I.y - M.y) * (P.x - M.x);
    if (fabs(tri_or) > 1e-14 * SC2) {   /* vértice reflejo dentro del triángulo (M, I, P): el de menor ángulo con el rayo */
        double bestang = 1e300;
        for (int r = 0; r < n; r++) {
            if (r == pp) continue;
            V2 U = X[ring[(r + n - 1) % n]], Q = X[ring[r]], W = X[ring[(r + 1) % n]];
            if ((Q.x - U.x) * (W.y - Q.y) - (Q.y - U.y) * (W.x - Q.x) >= 0) continue;    /* no reflejo */
            double s1 = ((I.x - M.x) * (Q.y - M.y) - (I.y - M.y) * (Q.x - M.x)) * tri_or;
            double s2 = ((P.x - I.x) * (Q.y - I.y) - (P.y - I.y) * (Q.x - I.x)) * tri_or;
            double s3 = ((M.x - P.x) * (Q.y - P.y) - (M.y - P.y) * (Q.x - P.x)) * tri_or;
            if (s1 < 0 || s2 < 0 || s3 < 0) continue;
            double dx = Q.x - M.x, dy = fabs(Q.y - M.y), ang = atan2(dy, dx);
            if (dx >= 0 && ang < bestang) { bestang = ang; pp = r; }
        }
    }
    /* anillo nuevo: ring[0..pp], M, h1..h(n-1), M, P, ring[pp+1..] */
    int add = hn + 2, pidx = ring[pp];
    memmove(ring + pp + 1 + add, ring + pp + 1, sizeof(int) * (size_t)(n - pp - 1));
    int w = pp + 1;
    for (int j = 0; j < hn; j++) ring[w++] = hs + (mi - hs + j) % hn;
    ring[w++] = mi; ring[w++] = pidx;
    *m = n + add;
    return 0;
}

/* Bucle de tapa: puntos en el sentido de la tapa de la PIEZA (los triángulos salen con la normal de Newell). */
typedef struct { P3 *p; int n; int comp; unsigned want; } Loop;   /* want: 1 = tapa de la pieza, 2 = relleno del hueco */
typedef struct { Loop *l; int n, cap; } LoopArr;
static int la_add(LoopArr *a, const P3 *pts, int n, int comp, unsigned want, int reverse) {
    if (n < 3) return 0;
    if (a->n == a->cap) {
        int c = a->cap ? a->cap * 2 : 8; Loop *x = (Loop *)realloc(a->l, sizeof(Loop) * (size_t)c);
        if (!x) return -3;
        a->l = x; a->cap = c;
    }
    P3 *cp = (P3 *)malloc(sizeof(P3) * (size_t)n);
    if (!cp) return -3;
    for (int i = 0; i < n; i++) cp[i] = pts[reverse ? n - 1 - i : i];
    a->l[a->n].p = cp; a->l[a->n].n = n; a->l[a->n].comp = comp; a->l[a->n].want = want; a->n++;
    return 0;
}
static void la_free(LoopArr *a) { for (int i = 0; i < a->n; i++) free(a->l[i].p); free(a->l); memset(a, 0, sizeof *a); }

/* Triangula un exterior con sus agujeros y emite la tapa (want&1) y el relleno (want&2, mismos triángulos invertidos). */
static int emit_polygon(const Loop *o, const Loop *const *hl, int nh, WCutUV capP, WCutUV capH, TL *Pc, TL *Hc) {
    double nw[3]; newell(o->p, o->n, nw);
    double nl = sqrt(nw[0] * nw[0] + nw[1] * nw[1] + nw[2] * nw[2]);
    if (nl < 1e-14 * SC2) return 0;
    float nf[3] = {(float)(nw[0] / nl), (float)(nw[1] / nl), (float)(nw[2] / nl)};
    int a = 0; for (int i = 1; i < 3; i++) if (fabsf(nf[i]) > fabsf(nf[a])) a = i;
    int s = (a + 1) % 3, t = (a + 2) % 3; double sg = nf[a] >= 0 ? 1 : -1;
    int tot = o->n; for (int h = 0; h < nh; h++) tot += hl[h]->n;
    P3 *pts = (P3 *)malloc(sizeof(P3) * (size_t)tot); V2 *X = (V2 *)malloc(sizeof(V2) * (size_t)tot);
    int *ring = (int *)malloc(sizeof(int) * (size_t)(tot + 2 * nh + 4)); int (*tri)[3] = (int (*)[3])malloc(sizeof(int[3]) * (size_t)(tot + 2 * nh + 4));
    int *hst = (int *)malloc(sizeof(int) * (size_t)(nh + 1)), *hord = (int *)malloc(sizeof(int) * (size_t)(nh + 1));
    int rc = 0;
    if (!pts || !X || !ring || !tri || !hst || !hord) { rc = -3; goto done; }
    int w = 0;
    for (int i = 0; i < o->n; i++) { pts[w] = o->p[i]; X[w].x = sg * o->p[i].p[s]; X[w].y = o->p[i].p[t]; ring[i] = w; w++; }
    int m = o->n;
    for (int h = 0; h < nh; h++) {   /* agujeros: orientación horaria en X */
        const Loop *H = hl[h]; hst[h] = w;
        for (int i = 0; i < H->n; i++) { pts[w] = H->p[i]; X[w].x = sg * H->p[i].p[s]; X[w].y = H->p[i].p[t]; w++; }
        if (area2_v2(X + hst[h], H->n) > 0)
            for (int i = 0, j = H->n - 1; i < j; i++, j--) {
                P3 tp = pts[hst[h] + i]; pts[hst[h] + i] = pts[hst[h] + j]; pts[hst[h] + j] = tp;
                V2 tx = X[hst[h] + i]; X[hst[h] + i] = X[hst[h] + j]; X[hst[h] + j] = tx;
            }
        hord[h] = h;
    }
    for (int i = 1; i < nh; i++) {   /* por x máxima descendente */
        int x = hord[i], j = i - 1; double mx = -1e300;
        for (int q = 0; q < hl[x]->n; q++) if (X[hst[x] + q].x > mx) mx = X[hst[x] + q].x;
        while (j >= 0) {
            double mj = -1e300; for (int q = 0; q < hl[hord[j]]->n; q++) if (X[hst[hord[j]] + q].x > mj) mj = X[hst[hord[j]] + q].x;
            if (mj >= mx) break;
            hord[j + 1] = hord[j]; j--;
        }
        hord[j + 1] = x;
    }
    for (int h = 0; h < nh && !rc; h++) { if ((rc = spend(4.0 * m))) break; rc = bridge_hole(X, ring, &m, hst[hord[h]], hl[hord[h]]->n); }
    if (rc) goto done;
    int nt = 0;
    rc = ear_clip(X, ring, m, tri, &nt);
    if (rc) goto done;
    double smin = 1e30, smax = -1e30, tmin = 1e30, tmax = -1e30;
    for (int i = 0; i < o->n; i++) {
        double ps = o->p[i].p[s], pt = o->p[i].p[t];
        if (ps < smin) smin = ps;
        if (ps > smax) smax = ps;
        if (pt < tmin) tmin = pt;
        if (pt > tmax) tmax = pt;
    }
    double ds = smax - smin > 1e-9 * g_sc ? smax - smin : 1, dt = tmax - tmin > 1e-9 * g_sc ? tmax - tmin : 1;
    for (int pass = 0; pass < 2 && !rc; pass++) {
        if (!(o->want & (pass ? 2u : 1u)) || !(pass ? Hc : Pc)) continue;
        WCutUV uv = pass ? capH : capP;
        for (int q = 0; q < nt && !rc; q++) {
            Tri tr; tr.comp = o->comp; tr.cap = pass ? WCUT_TRI_FILL : WCUT_TRI_CAP;
            int idx3[3] = {tri[q][0], tri[q][pass ? 2 : 1], tri[q][pass ? 1 : 2]};
            idx3[0] = tri[q][0];
            for (int k = 0; k < 3; k++) {
                Vx *v = &tr.v[k]; const float *pp = pts[idx3[k]].p;
                memcpy(v->p, pp, 12);
                for (int r = 0; r < 3; r++) v->n[r] = pass ? -nf[r] : nf[r];
                double fu = (pp[s] - smin) / ds, fv = (pp[t] - tmin) / dt;
                fu = fu < 0 ? 0 : (fu > 1 ? 1 : fu); fv = fv < 0 ? 0 : (fv > 1 ? 1 : fv);
                v->uv[0] = uv.u0 + (float)fu * (uv.u1 - uv.u0); v->uv[1] = uv.v0 + (float)fv * (uv.v1 - uv.v0);
                v->c[0] = v->c[1] = v->c[2] = v->c[3] = 255;
            }
            rc = tl_push(pass ? Hc : Pc, &tr);
        }
    }
done:
    free(pts); free(X); free(ring); free(tri); free(hst); free(hord);
    return rc;
}

/* Clasifica los bucles por plano común e inclusión (exterior / agujero) y emite las tapas.
 * Un bucle de orientación contraria sin exterior que lo contenga se rellena por su cuenta (con su propia orientación). */
static int caps_from_loops(Loop *lp, int nl, WCutUV capP, WCutUV capH, TL *Pc, TL *Hc) {
    if (nl <= 0) return 0;
    int rc = 0;
    double (*N)[3] = (double (*)[3])calloc((size_t)nl, sizeof *N), *Ar = (double *)calloc((size_t)nl, sizeof(double));
    int *grp = (int *)malloc(sizeof(int) * (size_t)nl), *ord = (int *)malloc(sizeof(int) * (size_t)nl);
    if (!N || !Ar || !grp || !ord) { free(N); free(Ar); free(grp); free(ord); return -3; }
    for (int i = 0; i < nl; i++) {
        double nw[3]; newell(lp[i].p, lp[i].n, nw);
        double l = sqrt(nw[0] * nw[0] + nw[1] * nw[1] + nw[2] * nw[2]);
        Ar[i] = l / 2; grp[i] = l < 1e-14 * SC2 ? -2 : -1;
        for (int r = 0; r < 3; r++) N[i][r] = l > 0 ? nw[r] / l : 0;
        ord[i] = i;
    }
    for (int i = 1; i < nl; i++) { int x = ord[i], j = i - 1; while (j >= 0 && Ar[ord[j]] < Ar[x]) { ord[j + 1] = ord[j]; j--; } ord[j + 1] = x; }
    for (int oi = 0; oi < nl && !rc; oi++) {   /* grupos coplanares */
        int r0 = ord[oi]; if (grp[r0] != -1) continue;
        int G[256], ng = 0;   /* miembros del grupo (tope: el resto va a grupos propios) */
        grp[r0] = r0; G[ng++] = r0;
        for (int oj = oi + 1; oj < nl && ng < 256; oj++) {
            int j = ord[oj]; if (grp[j] != -1) continue;
            double dn = fabs(N[j][0] * N[r0][0] + N[j][1] * N[r0][1] + N[j][2] * N[r0][2]);
            double dd = 0; for (int r = 0; r < 3; r++) dd += ((double)lp[j].p[0].p[r] - lp[r0].p[0].p[r]) * N[r0][r];
            if (dn >= 0.99999 && fabs(dd) <= 2e-6 * g_sc) { grp[j] = r0; G[ng++] = j; }
        }
        if ((rc = spend(64.0 * ng * ng))) break;
        /* proyección del grupo */
        int a = 0; for (int i = 1; i < 3; i++) if (fabs(N[r0][i]) > fabs(N[r0][a])) a = i;
        int s = (a + 1) % 3, t = (a + 2) % 3; double sg = N[r0][a] >= 0 ? 1 : -1;
        V2 **XS = (V2 **)calloc((size_t)ng, sizeof(V2 *)); double *sa = (double *)calloc((size_t)ng, sizeof(double));
        int *cont = (int *)malloc(sizeof(int) * (size_t)ng);
        if (!XS || !sa || !cont) { free(XS); free(sa); free(cont); rc = -3; break; }
        for (int g = 0; g < ng && !rc; g++) {
            const Loop *L = &lp[G[g]];
            XS[g] = (V2 *)malloc(sizeof(V2) * (size_t)L->n);
            if (!XS[g]) { rc = -3; break; }
            for (int i = 0; i < L->n; i++) { XS[g][i].x = sg * L->p[i].p[s]; XS[g][i].y = L->p[i].p[t]; }
            sa[g] = area2_v2(XS[g], L->n);
        }
        /* contenedor de cada agujero: el exterior más pequeño que lo contiene */
        for (int g = 0; g < ng && !rc; g++) {
            cont[g] = -1;
            if (sa[g] >= 0) continue;
            const Loop *L = &lp[G[g]]; double bestA = 1e300;
            for (int o = 0; o < ng; o++) {
                if (o == g || sa[o] <= 0 || fabs(sa[o]) >= bestA || fabs(sa[o]) <= fabs(sa[g])) continue;
                if ((rc = spend((double)L->n * lp[G[o]].n))) break;
                int in = 0; for (int i = 0; i < L->n; i++) in += pip(XS[o], lp[G[o]].n, XS[g][i]);
                if (2 * in > L->n) { bestA = fabs(sa[o]); cont[g] = o; }
            }
        }
        for (int o = 0; o < ng && !rc; o++) {
            int isout = sa[o] > 0, fallback = sa[o] < 0 && cont[o] < 0;
            if (!isout && !fallback) continue;
            const Loop *hl[64]; int nh = 0;
            if (isout) for (int g = 0; g < ng && nh < 64; g++) if (cont[g] == o) hl[nh++] = &lp[G[g]];
            if (fallback) {   /* sin exterior: se rellena con su propia orientación */
                rc = emit_polygon(&lp[G[o]], hl, 0, capP, capH, Pc, Hc);
            } else rc = emit_polygon(&lp[G[o]], hl, nh, capP, capH, Pc, Hc);
        }
        for (int g = 0; g < ng; g++) free(XS[g]);
        free(XS); free(sa); free(cont);
    }
    for (int i = 0; i < nl && !rc; i++) if (grp[i] == -1) rc = 0;
    free(N); free(Ar); free(grp); free(ord);
    return rc;
}

/* Bucles a partir de aristas dirigidas (u -> v); devuelve puntos, longitudes y componente de cada bucle */
typedef struct { P3 *pt; int *len; int *comp; int nloops, npt; } Loops;
static void loops_free(Loops *L) { free(L->pt); free(L->len); free(L->comp); memset(L, 0, sizeof *L); }
static int build_loops(const EK *e, const float (*pa)[3], const int *ecomp, int ne, Loops *L) {
    /* e[i].a/.b = claves; pa[i] = posición del inicio (a) de la arista i */
    memset(L, 0, sizeof *L);
    if (ne <= 0) return 0;
    int *order = (int *)malloc(sizeof(int) * (size_t)ne), *used = (int *)calloc((size_t)ne, sizeof(int));
    L->pt = (P3 *)malloc(sizeof(P3) * (size_t)ne); L->len = (int *)malloc(sizeof(int) * (size_t)ne); L->comp = (int *)malloc(sizeof(int) * (size_t)ne);
    if (!order || !used || !L->pt || !L->len || !L->comp) { free(order); free(used); loops_free(L); return -3; }
    for (int i = 0; i < ne; i++) order[i] = i;
    /* orden por clave de inicio (insertion/qsort con contexto: se hace a mano) */
    for (int i = 1; i < ne; i++) {
        int x = order[i], j = i - 1;
        if (spend(8)) { free(order); free(used); loops_free(L); return -3; }
        while (j >= 0 && memcmp(&e[order[j]].a, &e[x].a, sizeof(PK)) > 0) { order[j + 1] = order[j]; j--; }
        order[j + 1] = x;
    }
    for (int s = 0; s < ne; s++) {
        int i0 = order[s]; if (used[i0]) continue;
        int start = L->npt, cur = i0, guard = 0;
        for (;;) {
            used[cur] = 1;
            memcpy(L->pt[L->npt].p, pa[cur], 12); L->npt++;
            /* siguiente arista: la no usada que empieza donde acaba la actual */
            int nxt = -1, lo = 0, hi = ne - 1;
            while (lo < hi) { int mid = (lo + hi) / 2; if (memcmp(&e[order[mid]].a, &e[cur].b, sizeof(PK)) < 0) lo = mid + 1; else hi = mid; }
            for (int q = lo; q < ne && memcmp(&e[order[q]].a, &e[cur].b, sizeof(PK)) == 0; q++)
                if (!used[order[q]]) { nxt = order[q]; break; }
            if (nxt < 0 || ++guard > ne) break;
            cur = nxt;
        }
        L->comp[L->nloops] = ecomp ? ecomp[i0] : -1;
        L->len[L->nloops++] = L->npt - start;
    }
    free(order); free(used);
    return 0;
}

/* ------------------------------------------------------------------ componentes conexas */
typedef struct { PK k; int tri; } CK;
static int ck_cmp(const void *x, const void *y) { return memcmp(&((const CK *)x)->k, &((const CK *)y)->k, sizeof(PK)); }
static int uf_find(int *par, int a) { while (par[a] != a) { par[a] = par[par[a]]; a = par[a]; } return a; }
static int comp_label(TL *l) {
    size_t n = l->n; if (!n) return 0;
    CK *ks = (CK *)malloc(n * 3 * sizeof(CK)); int *par = (int *)malloc(n * sizeof(int)), *rank = (int *)malloc(n * sizeof(int));
    if (!ks || !par || !rank) { free(ks); free(par); free(rank); return -3; }
    for (size_t i = 0; i < n; i++) { par[i] = (int)i; for (int a = 0; a < 3; a++) { ks[i * 3 + a].k = pkey(l->t[i].v[a].p); ks[i * 3 + a].tri = (int)i; } }
    qsort(ks, n * 3, sizeof(CK), ck_cmp);
    for (size_t i = 1; i < n * 3; i++)
        if (memcmp(&ks[i].k, &ks[i - 1].k, sizeof(PK)) == 0) {
            int a = uf_find(par, ks[i].tri), b = uf_find(par, ks[i - 1].tri);
            if (a != b) par[a > b ? a : b] = a < b ? a : b;
        }
    int nc = 0;
    for (size_t i = 0; i < n; i++) rank[i] = -1;
    for (size_t i = 0; i < n; i++) { int r = uf_find(par, (int)i); if (rank[r] < 0) rank[r] = nc++; l->t[i].comp = rank[r]; }
    free(ks); free(par); free(rank);
    return nc;
}

/* ------------------------------------------------------------------ piezas en construcción */
typedef struct { char name[24]; TL tl; int copy; const WCutRule *rule; double gen_vol; } BPiece;

static int sel_uv_ok(const WCutSel *s, const Tri *t) {
    if (!s->has_uv) return 1;
    float u = (t->v[0].uv[0] + t->v[1].uv[0] + t->v[2].uv[0]) / 3, v = (t->v[0].uv[1] + t->v[1].uv[1] + t->v[2].uv[1]) / 3;
    return u >= s->uv.u0 && u <= s->uv.u1 && v >= s->uv.v0 && v <= s->uv.v1;
}

/* ---- recorte de la piel por la caja + tapas por cara ---- */
typedef struct { Vx v[28]; int n; } BPoly;

/* inserta en las aristas de 'f' los vértices de 'vs' que caen sobre ellas (uniones en T dentro de un mismo triángulo) */
static void frag_conform(BPoly *f, const Vx *vs, int nvs) {
    for (int guard = 0; guard < 40; guard++) {
        int done = 1;
        for (int e = 0; e < f->n && done; e++) {
            const Vx *a = &f->v[e], *b = &f->v[(e + 1) % f->n];
            double ab[3]; sub3(b->p, a->p, ab);
            double L2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
            if (L2 < 1e-16 * SC2) continue;
            for (int q = 0; q < nvs; q++) {
                double aq[3]; sub3(vs[q].p, a->p, aq);
                double s = (aq[0] * ab[0] + aq[1] * ab[1] + aq[2] * ab[2]) / L2;
                if (s <= 1e-6 || s >= 1 - 1e-6) continue;
                double d0 = aq[0] - s * ab[0], d1 = aq[1] - s * ab[1], d2 = aq[2] - s * ab[2];
                if (d0 * d0 + d1 * d1 + d2 * d2 > 1e-14 * SC2 || f->n >= 27) continue;
                Vx m = vx_lerp(a, b, s); memcpy(m.p, vs[q].p, 12);
                for (int k = f->n; k > e + 1; k--) f->v[k] = f->v[k - 1];
                f->v[e + 1] = m; f->n++; done = 0; break;
            }
        }
        if (done) break;
    }
}
static int emit_bpoly(const BPoly *p, int comp, uint8_t cap, TL *l) {
    for (int i = 1; i + 1 < p->n; i++) {
        Tri t; t.v[0] = p->v[0]; t.v[1] = p->v[i]; t.v[2] = p->v[i + 1]; t.comp = comp; t.cap = cap;
        if (tri_coinc(&t)) continue;
        if (tl_push(l, &t)) return -3;
    }
    return 0;
}

/* intersección rayo-triángulo (Moller-Trumbore) */
static int ray_tri(const double *o, const double *d, const Tri *t) {
    double e1[3], e2[3], pv[3], tv[3], qv[3];
    sub3(t->v[1].p, t->v[0].p, e1); sub3(t->v[2].p, t->v[0].p, e2);
    cross3(d, e2, pv);
    double det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
    if (fabs(det) < 1e-18 * SC2) return 0;
    double inv = 1.0 / det;
    for (int i = 0; i < 3; i++) tv[i] = o[i] - t->v[0].p[i];
    double u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv;
    if (u < 0 || u > 1) return 0;
    cross3(tv, e1, qv);
    double v = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) * inv;
    if (v < 0 || u + v > 1) return 0;
    double tt = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv;
    return tt > 1e-9 * g_sc;
}
static int point_inside(const TL *m, const double *o) {
    double d[3] = {0.5773, 0.3711, 0.7311}; int n = 0;
    for (size_t i = 0; i < m->n; i++) n += ray_tri(o, d, &m->t[i]);
    return n & 1;
}

static void box_corner(const BoxX *b, const int s[3], float *out) {
    for (int i = 0; i < 3; i++) {
        double v = b->c[i];
        for (int a = 0; a < 3; a++) v += s[a] * b->h[a] * b->R[i][a];
        out[i] = (float)v;
    }
}

/* Arista de contorno de la pieza (P) con gemelo en el resto: u -> v, componente del triángulo y máscara de caras de
 * la caja con las que ese triángulo es coplanar (esa arista NO puede cerrarse con la tapa de una de esas caras). */
typedef struct { float u[3], v[3]; int comp; unsigned cop; } IEdge;

/* Bucles de tapa de la cara k de la caja a partir de las aristas de contorno de P que están en ese plano: cadenas
 * invertidas (v -> u); las cerradas son bucles, las abiertas se unen por el perímetro de la cara con sus esquinas.
 * Si solo hay agujeros (o ninguna arista) y la cara está dentro del sólido, se añade el rectángulo de la cara. */
static int face_loops(const BoxX *bx, int k, const IEdge *E, int ne, const TL *cand, LoopArr *out) {
    int a = k >> 1, ua = (k & 1) ? (a + 2) % 3 : (a + 1) % 3, va = (k & 1) ? (a + 1) % 3 : (a + 2) % 3;
    double h1 = bx->h[ua], h2 = bx->h[va];
    int sgn[3]; sgn[a] = (k & 1) ? -1 : 1;
    int *mine = (int *)malloc(sizeof(int) * ((size_t)ne + 1)); int nm = 0, nall = 0;
    if (!mine) return -3;
    for (int i = 0; i < ne; i++)
        if (fabs(box_d(bx, E[i].u, k)) < ON_TOL && fabs(box_d(bx, E[i].v, k)) < ON_TOL) {
            nall++;   /* (las de un triángulo coplanar con la cara pertenecen a la tapa de otra cara) */
            if (!((E[i].cop >> k) & 1)) mine[nm++] = i;
        }
    int rc = 0;
    typedef struct { P3 *pt; int n; double th0, th1; int closed, used, comp; } Chain;
    Chain *ch = (Chain *)calloc((size_t)nm + 1, sizeof(Chain)); int nch = 0;
    char *usedE = (char *)calloc((size_t)nm + 1, 1);
    if (!ch || !usedE) { free(mine); free(ch); free(usedE); return -3; }
    /* parámetro antihorario sobre el perímetro */
    #define THETA(P_, TH_) do { double q_[3]; for (int i_ = 0; i_ < 3; i_++) q_[i_] = (double)(P_)[i_] - bx->c[i_]; \
        double u_ = 0, v_ = 0; for (int i_ = 0; i_ < 3; i_++) { u_ += q_[i_] * bx->R[i_][ua]; v_ += q_[i_] * bx->R[i_][va]; } \
        double d0_ = fabs(v_ + h2), d1_ = fabs(u_ - h1), d2_ = fabs(v_ - h2), d3_ = fabs(u_ + h1), m_ = d0_; int w_ = 0; \
        if (d1_ < m_) { m_ = d1_; w_ = 1; } if (d2_ < m_) { m_ = d2_; w_ = 2; } if (d3_ < m_) { m_ = d3_; w_ = 3; } \
        if (m_ > PERI_TOL) TH_ = -1; \
        else if (w_ == 0) TH_ = u_ + h1; else if (w_ == 1) TH_ = 2 * h1 + (v_ + h2); \
        else if (w_ == 2) TH_ = 2 * h1 + 2 * h2 + (h1 - u_); else TH_ = 4 * h1 + 2 * h2 + (h2 - v_); \
        if (m_ <= PERI_TOL && TH_ < 0) TH_ = 0; } while (0)
    /* primero cadenas abiertas (pasada 0: empiezan donde no acaba otra), luego bucles */
    for (int pass = 0; pass < 2 && !rc; pass++)
        for (int s = 0; s < nm && !rc; s++) {
            if (usedE[s]) continue;
            if ((rc = spend(4.0 * nm))) break;
            int i0 = mine[s];
            PK st = pkey(E[i0].v);
            int is_start = 1;
            for (int q = 0; q < nm; q++) { if (q == s) continue; PK en = pkey(E[mine[q]].u); if (memcmp(&en, &st, sizeof(PK)) == 0) { is_start = 0; break; } }
            if (pass == 0 && !is_start) continue;
            Chain *C = &ch[nch]; C->pt = (P3 *)malloc(sizeof(P3) * ((size_t)nm + 2));
            if (!C->pt) { rc = -3; break; }
            C->comp = E[i0].comp;
            int cur = s; int guard = 0;
            for (;;) {
                usedE[cur] = 1; memcpy(C->pt[C->n++].p, E[mine[cur]].v, 12);
                PK en = pkey(E[mine[cur]].u); int nxt = -1;
                for (int q = 0; q < nm; q++) if (!usedE[q]) { PK s2 = pkey(E[mine[q]].v); if (memcmp(&s2, &en, sizeof(PK)) == 0) { nxt = q; break; } }
                if (nxt < 0 || ++guard > nm) { memcpy(C->pt[C->n++].p, E[mine[cur]].u, 12); break; }
                cur = nxt;
            }
            nch++;
            PK f0 = pkey(C->pt[0].p), f1 = pkey(C->pt[C->n - 1].p);
            C->closed = memcmp(&f0, &f1, sizeof(PK)) == 0;
            if (C->closed) C->n--;
            THETA(C->pt[0].p, C->th0); THETA(C->pt[C->n - 1].p, C->th1);
            if (!C->closed && (C->th0 < 0 || C->th1 < 0)) C->closed = 2;   /* extremo suelto (superficie original abierta): se cierra con recta */
        }
    #undef THETA
    double per = 4 * h1 + 4 * h2;
    int corner_s[4][3];
    { int su[4] = {-1, 1, 1, -1}, sv[4] = {-1, -1, 1, 1}; for (int q = 0; q < 4; q++) { corner_s[q][a] = sgn[a]; corner_s[q][ua] = su[q]; corner_s[q][va] = sv[q]; } }
    double th_c[4] = {0, 2 * h1, 2 * h1 + 2 * h2, 4 * h1 + 2 * h2};
    P3 *poly = (P3 *)malloc(sizeof(P3) * ((size_t)nm * 2 + 4 * (size_t)nch + 16));
    if (!poly) rc = -3;
    int npos = 0, nneg = 0;
    #define NOTE_ORIENT(PTS_, N_) do { double sa_ = 0; for (int i_ = 0; i_ < (N_); i_++) { int j_ = (i_ + 1) % (N_); \
        double u0_ = 0, v0_ = 0, u1_ = 0, v1_ = 0; for (int r_ = 0; r_ < 3; r_++) { \
            u0_ += ((double)(PTS_)[i_].p[r_] - bx->c[r_]) * bx->R[r_][ua]; v0_ += ((double)(PTS_)[i_].p[r_] - bx->c[r_]) * bx->R[r_][va]; \
            u1_ += ((double)(PTS_)[j_].p[r_] - bx->c[r_]) * bx->R[r_][ua]; v1_ += ((double)(PTS_)[j_].p[r_] - bx->c[r_]) * bx->R[r_][va]; } \
        sa_ += u0_ * v1_ - u1_ * v0_; } if (sa_ > 0) npos++; else nneg++; } while (0)
    for (int q = 0; q < nch && !rc; q++) {
        Chain *C = &ch[q];
        if (C->closed == 1 || C->closed == 2) {   /* bucle cerrado o cadena suelta: polígono directo */
            rc = la_add(out, C->pt, C->n, C->comp, 3, 0);
            NOTE_ORIENT(C->pt, C->n);
            C->used = 1;
        }
    }
    for (int q = 0; q < nch && !rc; q++) {
        Chain *C = &ch[q]; if (C->used) continue;
        int np = 0, cur = q, guard = 0;
        for (;;) {
            Chain *Cc = &ch[cur]; Cc->used = 1;
            for (int i = 0; i < Cc->n; i++) poly[np++] = Cc->pt[i];
            /* siguiente cadena abierta: la de menor th0 >= th1 (cíclico) */
            double th = Cc->th1, best = 1e30; int nx = -1;
            for (int r = 0; r < nch; r++) {
                if (ch[r].closed) continue;
                double dth = ch[r].th0 - th; if (dth < -1e-9 * g_sc) dth += per;
                if (dth < best) { best = dth; nx = r; }
            }
            if (nx < 0) break;
            double th2 = ch[nx].th0;
            double span = th2 - th; if (span < -1e-9 * g_sc) span += per;
            { /* esquinas estrictamente entre th y th2 (antihorario), en orden creciente de ángulo desde th */
                double dcs[4]; int cid[4]; int nc4 = 0;
                for (int c4 = 0; c4 < 4; c4++) {
                    double dc = th_c[c4] - th; if (dc < 0) dc += per;
                    if (dc > 1e-9 * g_sc && dc < span - 1e-9 * g_sc && (th2 != th || nx != cur)) {
                        int j = nc4++; while (j > 0 && dcs[j - 1] > dc) { dcs[j] = dcs[j - 1]; cid[j] = cid[j - 1]; j--; }
                        dcs[j] = dc; cid[j] = c4;
                    }
                }
                for (int c4 = 0; c4 < nc4; c4++) { box_corner(bx, corner_s[cid[c4]], poly[np].p); np++; }
            }
            if (nx == q || ch[nx].used) break;
            cur = nx;
            if (++guard > nch + 2) break;
        }
        rc = la_add(out, poly, np, C->comp, 3, 0);
        if (np >= 3) NOTE_ORIENT(poly, np);
    }
    #undef NOTE_ORIENT
    /* cara entera: sin contorno, o solo agujeros, y dentro del sólido (se busca un punto cercano a un borde fuera de los agujeros) */
    if (!rc && (nall == 0 || (nm > 0 && nneg > 0 && npos == 0)) && cand->n) {
        double fc[3], pt[3] = {0, 0, 0}; int found = 0;
        for (int i = 0; i < 3; i++) fc[i] = bx->c[i] + sgn[a] * bx->h[a] * bx->R[i][a];
        for (int q = 0; q < 4 && !found; q++) {
            float cr[3]; box_corner(bx, corner_s[q], cr);
            for (int i = 0; i < 3; i++) pt[i] = cr[i] + 0.05 * (fc[i] - cr[i]);
            found = 1;
            for (int g = 0; g < out->n && found; g++) {   /* fuera de todo agujero existente */
                const Loop *L = &out->l[g]; V2 *Xs = (V2 *)malloc(sizeof(V2) * (size_t)L->n);
                if (!Xs) { rc = -3; break; }
                for (int i = 0; i < L->n; i++) { double u = 0, v = 0; for (int r = 0; r < 3; r++) { u += ((double)L->p[i].p[r] - bx->c[r]) * bx->R[r][ua]; v += ((double)L->p[i].p[r] - bx->c[r]) * bx->R[r][va]; } Xs[i].x = u; Xs[i].y = v; }
                double pu = 0, pv = 0; for (int r = 0; r < 3; r++) { pu += (pt[r] - bx->c[r]) * bx->R[r][ua]; pv += (pt[r] - bx->c[r]) * bx->R[r][va]; }
                V2 qq = {pu, pv}; if (pip(Xs, L->n, qq)) found = 0;
                free(Xs);
            }
        }
        if (!rc && found && point_inside(cand, pt)) {
            P3 w[4]; for (int q = 0; q < 4; q++) box_corner(bx, corner_s[q], w[q].p);
            rc = la_add(out, w, 4, cand->t[0].comp, 3, 0);
        }
    }
    for (int q = 0; q < nch; q++) free(ch[q].pt);
    free(ch); free(usedE); free(mine); free(poly);
    return rc;
}

/* ---- reparación de uniones en T: inserta en las aristas de cada triángulo los puntos de corte que caen sobre ellas ---- */
typedef struct { float (*p)[3]; size_t n, cap; } PtList;
static int pl_add(PtList *l, const float *p) {
    if (spend((double)l->n + 1)) return -3;
    for (size_t i = 0; i < l->n; i++) if (!memcmp(l->p[i], p, 12)) return 0;
    if (g_cap && l->n >= g_cap) return -3;
    if (l->n == l->cap) {
        size_t c = l->cap ? l->cap * 2 : 64; float (*x)[3] = (float (*)[3])realloc(l->p, c * sizeof(float[3]));
        if (!x) return -3;
        l->p = x; l->cap = c;
    }
    memcpy(l->p[l->n++], p, 12);
    return 0;
}
static int conform_tl(TL *l, const PtList *pts) {
    if (!pts->n) return 0;
    for (size_t i = 0; i < l->n; i++) {
        int guard = 0, again = 1;
        while (again && guard++ < 64) {
            again = 0;
            if (spend(8.0 + 3.0 * (double)pts->n)) return -3;
            Tri t = l->t[i];
            for (int e = 0; e < 3 && !again; e++) {
                const Vx *a = &t.v[e], *b = &t.v[(e + 1) % 3];
                double ab[3]; sub3(b->p, a->p, ab);
                double L2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
                if (L2 < 1e-16 * SC2) continue;
                for (size_t q = 0; q < pts->n; q++) {
                    double aq[3]; sub3(pts->p[q], a->p, aq);
                    double sp = (aq[0] * ab[0] + aq[1] * ab[1] + aq[2] * ab[2]) / L2;
                    if (sp <= 1e-6 || sp >= 1 - 1e-6) continue;
                    double d0 = aq[0] - sp * ab[0], d1 = aq[1] - sp * ab[1], d2 = aq[2] - sp * ab[2];
                    if (d0 * d0 + d1 * d1 + d2 * d2 > 1e-14 * SC2) continue;
                    Vx m = vx_lerp(a, b, sp); memcpy(m.p, pts->p[q], 12);
                    Tri t1 = t, t2 = t;
                    t1.v[(e + 1) % 3] = m;           /* (a, m, c) */
                    t2.v[e] = m;                     /* (m, b, c) */
                    l->t[i] = t1;
                    if (tl_push(l, &t2)) return -3;
                    again = 1; break;
                }
            }
        }
    }
    return 0;
}
/* Elimina triángulos de área nula (3 vértices alineados o con dos coincidentes) y cose las uniones en T que dejan:
 * el vértice intermedio del triángulo alineado se inserta en el vecino que comparte la arista larga. */
static int clean_collinear(TL *l) {
    for (int pass = 0; pass < 6; pass++) {
        PtList mids = {0}; size_t k = 0; int removed = 0, rc = 0;
        for (size_t i = 0; i < l->n && !rc; i++) {
            const Tri *t = &l->t[i];
            if (tri_area2(t) >= AREA_MIN && !tri_coinc(t)) { l->t[k++] = *t; continue; }
            removed = 1;
            if (tri_coinc(t)) continue;
            /* vértice intermedio = el que no es extremo de la arista más larga */
            double best = -1; int mid = 0;
            for (int e = 0; e < 3; e++) {
                double d[3]; sub3(t->v[(e + 1) % 3].p, t->v[e].p, d);
                double L2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                if (L2 > best) { best = L2; mid = (e + 2) % 3; }
            }
            rc = pl_add(&mids, t->v[mid].p);
        }
        if (rc) { free(mids.p); return rc; }
        /* los triángulos no revisados (si hubo salida anticipada no llegamos aquí) ya están compactados hasta k */
        l->n = k;
        rc = conform_tl(l, &mids);
        free(mids.p);
        if (rc) return rc;
        if (!removed) break;
    }
    return 0;
}

static int op_clip(const TL *cand, const BoxX *bx, unsigned flags, WCutUV capP, WCutUV capH, TL *P, TL *Rskin, TL *H, PtList *cuts) {
    int rc = 0;
    for (size_t i = 0; i < cand->n && !rc; i++) {
        const Tri *t = &cand->t[i];
        int away = 0;   /* triángulo enteramente fuera de algún plano de la caja: pasa al resto sin partirse */
        for (int k = 0; k < 6 && !away; k++) {
            int all = 1; for (int q = 0; q < 3 && all; q++) if (box_d(bx, t->v[q].p, k) <= EPS_D) all = 0;
            away = all;
        }
        if (away) { if (tl_push(Rskin, t)) rc = -3; continue; }
        Poly cur; cur.n = 3; for (int q = 0; q < 3; q++) cur.v[q] = t->v[q];
        BPoly frag[7]; int nf = 0;
        for (int k = 0; k < 6 && cur.n >= 3; k++) {
            Poly in, out; clip_plane(bx, k, &cur, &in, &out);
            if (out.n >= 3) { for (int q = 0; q < out.n; q++) frag[nf].v[q] = out.v[q]; frag[nf].n = out.n; nf++; }
            cur = in;
        }
        Vx vs[64]; int nvs = 0;
        for (int q = 0; q < cur.n && nvs < 64; q++) vs[nvs++] = cur.v[q];
        for (int f = 0; f < nf; f++) for (int q = 0; q < frag[f].n && nvs < 64; q++) vs[nvs++] = frag[f].v[q];
        for (int q = 0; q < nvs && !rc; q++) {   /* puntos de corte: vértices que no son esquinas del triángulo */
            int corner = 0; for (int c = 0; c < 3; c++) if (!memcmp(vs[q].p, t->v[c].p, 12)) corner = 1;
            if (!corner) rc = pl_add(cuts, vs[q].p);
        }
        for (int f = 0; f < nf && !rc; f++) { frag_conform(&frag[f], vs, nvs); rc = emit_bpoly(&frag[f], t->comp, t->cap, Rskin); }
        if (!rc && cur.n >= 3) {
            BPoly pin; pin.n = cur.n; for (int q = 0; q < cur.n; q++) pin.v[q] = cur.v[q];
            frag_conform(&pin, vs, nvs); rc = emit_bpoly(&pin, t->comp, t->cap, P);
        }
    }
    if (rc) return rc;
    rc = conform_tl(P, cuts);
    if (!rc) rc = conform_tl(Rskin, cuts);
    if (rc) return rc;
    /* contorno de P con gemelo en la piel recortada */
    size_t np = 0, nr = 0; EK *ep = edges_of(P, &np), *er = edges_of(Rskin, &nr);
    IEdge *E = (IEdge *)malloc((P->n * 3 + 1) * sizeof(IEdge));
    if (!ep || !er || !E) { free(ep); free(er); free(E); return -3; }
    int ne = 0;
    for (size_t i = 0; i < P->n; i++) {
        unsigned cop = 0;
        for (int k = 0; k < 6; k++) {
            int all = 1; for (int q = 0; q < 3 && all; q++) if (fabs(box_d(bx, P->t[i].v[q].p, k)) >= ON_TOL) all = 0;
            if (all) cop |= 1u << k;
        }
        for (int a = 0; a < 3; a++) {
            const float *u = P->t[i].v[a].p, *v = P->t[i].v[(a + 1) % 3].p;
            EK e; e.a = pkey(u); e.b = pkey(v);
            if (memcmp(&e.a, &e.b, sizeof(PK)) == 0 || has_rev(ep, np, &e)) continue;
            if (!has_rev(er, nr, &e)) continue;
            memcpy(E[ne].u, u, 12); memcpy(E[ne].v, v, 12); E[ne].comp = P->t[i].comp; E[ne].cop = cop; ne++;
        }
    }
    TL Pc = {0}, Hc = {0};
    for (int k = 0; k < 6 && !rc; k++) {
        LoopArr la = {0};
        rc = face_loops(bx, k, E, ne, cand, &la);
        if (!rc) rc = caps_from_loops(la.l, la.n, capP, capH, &Pc, &Hc);
        la_free(&la);
    }
    if (!(flags & WCUT_NO_CAP)) for (size_t i = 0; i < Pc.n && !rc; i++) rc = tl_push(P, &Pc.t[i]);
    for (size_t i = 0; i < Hc.n && !rc; i++) rc = tl_push(H, &Hc.t[i]);
    tl_free(&Pc); tl_free(&Hc); free(ep); free(er); free(E);
    return rc;
}

/* Interfaz entre P y R (selección sin recorte): bucles de aristas de P con gemelo en R. */
static int cap_interface(TL *P, TL *R, unsigned flags, WCutUV cap_piece, WCutUV cap_hole, TL *into_hole) {
    size_t np = 0, nr = 0;
    EK *ep = edges_of(P, &np), *er = edges_of(R, &nr);
    if (!ep || !er) { free(ep); free(er); return -3; }
    {   /* uniones en T entre P y R: los extremos de las aristas sin pareja se insertan en las aristas vecinas */
        PtList pts = {0}; int rc0 = 0;
        for (size_t i = 0; i < np && !rc0; i++)
            if (memcmp(&ep[i].a, &ep[i].b, sizeof(PK)) && !has_rev(ep, np, &ep[i]) && !has_rev(er, nr, &ep[i])) {
                if ((rc0 = spend((double)P->n))) break;
                for (size_t k = 0; k < P->n && !rc0; k++) for (int a = 0; a < 3; a++) {
                    PK ka = pkey(P->t[k].v[a].p);
                    if (!memcmp(&ka, &ep[i].a, sizeof ka)) { rc0 = pl_add(&pts, P->t[k].v[a].p); break; }
                }
            }
        for (size_t i = 0; i < nr && !rc0; i++)
            if (memcmp(&er[i].a, &er[i].b, sizeof(PK)) && !has_rev(er, nr, &er[i]) && !has_rev(ep, np, &er[i])) {
                if ((rc0 = spend((double)R->n))) break;
                for (size_t k = 0; k < R->n && !rc0; k++) for (int a = 0; a < 3; a++) {
                    PK ka = pkey(R->t[k].v[a].p);
                    if (!memcmp(&ka, &er[i].a, sizeof ka)) { rc0 = pl_add(&pts, R->t[k].v[a].p); break; }
                }
            }
        if (!rc0 && pts.n) { rc0 = conform_tl(P, &pts); if (!rc0) rc0 = conform_tl(R, &pts); }
        free(pts.p);
        if (rc0) { free(ep); free(er); return rc0; }
        free(ep); free(er); np = nr = 0;
        ep = edges_of(P, &np); er = edges_of(R, &nr);
    }
    if (!ep || !er) { free(ep); free(er); return -3; }
    EK *sel = (EK *)malloc((P->n * 3 + 1) * sizeof(EK)); float (*pa)[3] = (float (*)[3])malloc((P->n * 3 + 1) * sizeof(float[3]));
    int *scomp = (int *)malloc((P->n * 3 + 1) * sizeof(int));
    if (!sel || !pa || !scomp) { free(ep); free(er); free(sel); free(pa); free(scomp); return -3; }
    int ns = 0;
    for (size_t i = 0; i < P->n; i++)
        for (int a = 0; a < 3; a++) {
            const float *pu = P->t[i].v[a].p, *pv = P->t[i].v[(a + 1) % 3].p;
            EK e; e.a = pkey(pu); e.b = pkey(pv);
            if (memcmp(&e.a, &e.b, sizeof(PK)) == 0) continue;
            if (has_rev(ep, np, &e)) continue;                          /* interior de P */
            int twin_r = has_rev(er, nr, &e);
            if (twin_r || (flags & WCUT_CAP_OPEN)) { sel[ns] = e; memcpy(pa[ns], pu, 12); scomp[ns] = P->t[i].comp; ns++; }
        }
    Loops L; int rc = build_loops(sel, (const float (*)[3])pa, scomp, ns, &L);
    LoopArr la = {0};
    if (!rc) {
        int off = 0;
        for (int l = 0; l < L.nloops && !rc; l++) {
            int n = L.len[l]; const P3 *pts = L.pt + off; off += n;
            unsigned want = (flags & WCUT_NO_CAP) ? 0u : 1u;
            if (into_hole && !(flags & WCUT_NO_HOLE)) {   /* relleno del hueco: solo si el bucle tiene gemelo en R */
                int twin = 1;
                for (int i = 0; i < n && twin; i++) {
                    EK e; e.a = pkey(pts[i].p); e.b = pkey(pts[(i + 1) % n].p);
                    if (!has_rev(er, nr, &e)) twin = 0;
                }
                if (twin) want |= 2u;
            }
            if (want) rc = la_add(&la, pts, n, L.comp[l], want, 1);   /* la tapa de la pieza recorre el bucle al revés */
        }
    }
    if (!rc) rc = caps_from_loops(la.l, la.n, cap_piece, cap_hole, P, into_hole);
    la_free(&la);
    loops_free(&L); free(sel); free(pa); free(scomp); free(ep); free(er);
    return rc;
}

static int sel_cand(const WCutSel *s, const Tri *t) {
    return (!s->comps || (t->comp >= 0 && t->comp < 32 && (s->comps >> t->comp & 1))) && sel_uv_ok(s, t);
}

/* Separa 'src' en P (seleccionado, ya con tapas) y R (resto, ya con el relleno del hueco). */
static int select_split(const TL *src, const WCutRule *ru, TL *P, TL *R) {
    const WCutSel *s = &ru->sel;
    BoxX bx; if (s->has_box) box_make(&s->box, &bx);
    TL C = {0}; int rc = 0;
    for (size_t i = 0; i < src->n && !rc; i++) rc = tl_push(sel_cand(s, &src->t[i]) ? &C : R, &src->t[i]);
    if (!rc && s->has_box && s->mode == WCUT_CLIP) {
        TL H = {0}, Rs = {0}; PtList cuts = {0};
        rc = op_clip(&C, &bx, ru->flags, ru->cap_piece, ru->cap_hole, P, &Rs, &H, &cuts);
        if (!rc) {
            for (size_t i = 0; i < Rs.n && !rc; i++) rc = tl_push(R, &Rs.t[i]);
            if (!rc) rc = conform_tl(R, &cuts);   /* también los triángulos no candidatos del resto */
            if (!(ru->flags & (WCUT_NO_HOLE | WCUT_COPY))) for (size_t i = 0; i < H.n && !rc; i++) rc = tl_push(R, &H.t[i]);
        }
        tl_free(&H); tl_free(&Rs); tl_free(&C); free(cuts.p);
        return rc;
    }
    for (size_t i = 0; i < C.n && !rc; i++) {
        const Tri *t = &C.t[i];
        if (!s->has_box) rc = tl_push(P, t);
        else if (s->mode == WCUT_WHOLE) {
            int all = box_inside(&bx, t->v[0].p) && box_inside(&bx, t->v[1].p) && box_inside(&bx, t->v[2].p);
            rc = tl_push(all ? P : R, t);
        } else {
            float c[3]; for (int k = 0; k < 3; k++) c[k] = (t->v[0].p[k] + t->v[1].p[k] + t->v[2].p[k]) / 3;
            rc = tl_push(box_inside(&bx, c) ? P : R, t);
        }
    }
    tl_free(&C);
    if (rc) return rc;
    TL hole = {0};
    rc = cap_interface(P, R, ru->flags, ru->cap_piece, ru->cap_hole, (ru->flags & WCUT_COPY) ? NULL : &hole);
    for (size_t i = 0; i < hole.n && !rc; i++) rc = tl_push(R, &hole.t[i]);
    tl_free(&hole);
    return rc;
}

/* ------------------------------------------------------------------ utilidades públicas de comprobación */
uint32_t wcut_open_edges(const WCutPiece *p) {
    TL l = {0};
    for (uint32_t i = 0; i + 2 < p->ni; i += 3) {
        Tri t; memset(&t, 0, sizeof t);
        for (int k = 0; k < 3; k++) memcpy(t.v[k].p, p->v[p->idx[i + k]].pos, 12);
        if (tl_push(&l, &t)) { tl_free(&l); return 0xFFFFFFFFu; }
    }
    size_t n = 0; EK *e = edges_of(&l, &n); uint32_t open_ = 0;
    if (e) { for (size_t i = 0; i < n; i++) if (memcmp(&e[i].a, &e[i].b, sizeof(PK)) && !has_rev(e, n, &e[i])) open_++; }
    free(e); tl_free(&l);
    return open_;
}
double wcut_volume(const WCutPiece *p) {
    double v = 0;
    for (uint32_t i = 0; i + 2 < p->ni; i += 3) {
        const float *a = p->v[p->idx[i]].pos, *b = p->v[p->idx[i + 1]].pos, *c = p->v[p->idx[i + 2]].pos;
        v += ((double)a[0] * ((double)b[1] * c[2] - (double)b[2] * c[1]) - (double)a[1] * ((double)b[0] * c[2] - (double)b[2] * c[0])
              + (double)a[2] * ((double)b[0] * c[1] - (double)b[1] * c[0])) / 6.0;
    }
    return v;
}
uint32_t wcut_degenerate(const WCutPiece *p) {
    uint32_t n = 0;
    for (uint32_t i = 0; i + 2 < p->ni; i += 3) {
        Tri t; for (int k = 0; k < 3; k++) memcpy(t.v[k].p, p->v[p->idx[i + k]].pos, 12);
        if (tri_area2(&t) < 1e-13) n++;
    }
    return n;
}

/* ------------------------------------------------------------------ orquestación */
static BPiece *find_piece(BPiece *pc, int n, const char *name) {
    for (int i = 0; i < n; i++) if (!strcmp(pc[i].name, name)) return &pc[i];
    return NULL;
}
static void copy_name(char *d, const char *s) { strncpy(d, s ? s : "", 23); d[23] = 0; }

void wcut_free(WCutSet *s) {
    if (!s) return;
    if (s->piece) for (int i = 0; i < s->npiece; i++) { free(s->piece[i].v); free(s->piece[i].idx); free(s->piece[i].kind); }
    if (s->mv) for (int i = 0; i < s->npiece; i++) free(s->mv[i]);
    if (s->mi) for (int i = 0; i < s->npiece; i++) free(s->mi[i]);
    free(s->piece); free(s->meshes); free(s->mv); free(s->mi); free(s->parts); free(s->atlas);
    memset(s, 0, sizeof *s);
}
int wcut_piece_index(const WCutSet *s, const char *name) {
    for (int i = 0; i < s->npiece; i++) if (!strcmp(s->piece[i].name, name)) return i;
    return -1;
}
const WCutPiece *wcut_piece(const WCutSet *s, const char *name) {
    int i = wcut_piece_index(s, name); return i < 0 ? NULL : &s->piece[i];
}

/* Valida el modelo cargado y la tabla; calcula la escala. 0 = bien, -1 = inválido. */
static int validate(const WLoaded *m, const WCutModel *model, double *scale) {
    if (!m->v || !m->idx || m->ni != m->nv || m->ni % 3 || m->nv < 3) return -1;
    if (!model->body || !model->body[0] || model->nrules < 0 || (model->nrules && !model->rules)) return -1;
    if (model->nrules > 32) return -1;
    int nn = 0; if (model->names) while (model->names[nn] && nn < 33) nn++;
    if (nn > 32) return -1;
    for (int i = 0; i < nn; i++) if (!model->names[i]) return -1;
    for (int r = 0; r < model->nrules; r++) if (!model->rules[r].name || !model->rules[r].name[0]) return -1;
    double mx = 1.0;
    for (uint32_t i = 0; i < m->nv; i++) {
        const WVertex *w = &m->v[i];
        for (int k = 0; k < 3; k++) {
            if (!isfinite(w->pos[k]) || !isfinite(w->nrm[k]) || fabsf(w->pos[k]) > MAXCOORD) return -1;
            if (fabs((double)w->pos[k]) > mx) mx = fabs((double)w->pos[k]);
        }
        if (!isfinite(w->uv[0]) || !isfinite(w->uv[1])) return -1;
    }
    for (uint32_t i = 0; i < m->ni; i++) if (m->idx[i] >= m->nv) return -1;
    *scale = mx;
    return 0;
}

/* Orientación coherente en tres pasos:
 *  1. por conectividad de aristas: dos triángulos que comparten una arista en el MISMO sentido tienen la orientación
 *     contraria (uno está invertido en el original): se propaga por cada cáscara conexa (BFS) y se gira el que sobra;
 *  2. por cáscara: volumen con signo; si es negativo (cáscara entera hacia dentro) se gira entera (las planas, de
 *     volumen ~0, se dejan);
 *  3. las normales siguen a la geometría: un triángulo cuyas normales guardadas contradicen con claridad su orden se
 *     queda con las normales negadas. */
typedef struct { PK a, b; int tri; int dir; } UE;   /* arista no dirigida (a < b) */
static int ue_cmp(const void *x, const void *y) { return memcmp(x, y, 2 * sizeof(PK)); }
static void flip_tri(Tri *t) { Vx tmp = t->v[1]; t->v[1] = t->v[2]; t->v[2] = tmp; }
static int orient(TL *l) {
    size_t n = l->n;
    if (!n) return 0;
    UE *ue = (UE *)malloc(n * 3 * sizeof(UE)); int (*nb)[6] = (int (*)[6])malloc(n * sizeof(int[6]));
    int *shell = (int *)malloc(n * sizeof(int)), *queue = (int *)malloc(n * sizeof(int)); char *st = (char *)calloc(n, 1);
    if (!ue || !nb || !shell || !queue || !st) { free(ue); free(nb); free(shell); free(queue); free(st); return -3; }
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        for (int q = 0; q < 6; q++) nb[i][q] = -1;
        for (int e = 0; e < 3; e++) {
            PK a = pkey(l->t[i].v[e].p), b = pkey(l->t[i].v[(e + 1) % 3].p);
            int c = memcmp(&a, &b, sizeof(PK)); if (!c) continue;
            ue[k].tri = (int)i;
            if (c < 0) { ue[k].a = a; ue[k].b = b; ue[k].dir = 1; } else { ue[k].a = b; ue[k].b = a; ue[k].dir = 0; }
            k++;
        }
    }
    qsort(ue, k, sizeof(UE), ue_cmp);
    for (size_t i = 0; i + 1 < k; i++) {   /* aristas con exactamente dos triángulos */
        if (ue_cmp(&ue[i], &ue[i + 1])) continue;
        if (i + 2 < k && !ue_cmp(&ue[i], &ue[i + 2])) { while (i + 1 < k && !ue_cmp(&ue[i], &ue[i + 1])) i++; continue; }
        int x = ue[i].tri, y = ue[i + 1].tri, rel = ue[i].dir == ue[i + 1].dir;   /* rel = deben diferir */
        for (int q = 0; q < 3; q++) if (nb[x][2 * q] < 0) { nb[x][2 * q] = y; nb[x][2 * q + 1] = rel; break; }
        for (int q = 0; q < 3; q++) if (nb[y][2 * q] < 0) { nb[y][2 * q] = x; nb[y][2 * q + 1] = rel; break; }
        i++;
    }
    int ns = 0, rc = 0;
    for (size_t i = 0; i < n; i++) shell[i] = -1;
    for (size_t i = 0; i < n; i++) {
        if (shell[i] >= 0) continue;
        size_t qh = 0, qt = 0; queue[qt++] = (int)i; shell[i] = ns; st[i] = 0;
        while (qh < qt) {
            int x = queue[qh++];
            for (int q = 0; q < 3; q++) {
                int y = nb[x][2 * q]; if (y < 0 || shell[y] >= 0) continue;
                shell[y] = ns; st[y] = st[x] ^ (char)nb[x][2 * q + 1]; queue[qt++] = y;
            }
        }
        ns++;
    }
    for (size_t i = 0; i < n; i++) if (st[i]) flip_tri(&l->t[i]);
    double (*bb)[6] = (double (*)[6])malloc(sizeof(double[6]) * (size_t)ns); double *vol = (double *)calloc((size_t)ns, sizeof(double));
    char *fl = (char *)calloc((size_t)ns, 1);
    if (!bb || !vol || !fl) rc = -3;
    else {
        for (int c = 0; c < ns; c++) for (int q = 0; q < 3; q++) { bb[c][q] = 1e300; bb[c][3 + q] = -1e300; }
        for (size_t i = 0; i < n; i++) for (int q = 0; q < 3; q++) for (int a = 0; a < 3; a++) {
            double v = l->t[i].v[q].p[a]; double *b = bb[shell[i]];
            if (v < b[a]) b[a] = v;
            if (v > b[3 + a]) b[3 + a] = v;
        }
        for (size_t i = 0; i < n; i++) {
            const double *b = bb[shell[i]];
            double o[3] = {(b[0] + b[3]) / 2, (b[1] + b[4]) / 2, (b[2] + b[5]) / 2}, p[3], q[3], r[3];
            for (int a = 0; a < 3; a++) { p[a] = l->t[i].v[0].p[a] - o[a]; q[a] = l->t[i].v[1].p[a] - o[a]; r[a] = l->t[i].v[2].p[a] - o[a]; }
            vol[shell[i]] += (p[0] * (q[1] * r[2] - q[2] * r[1]) - p[1] * (q[0] * r[2] - q[2] * r[0]) + p[2] * (q[0] * r[1] - q[1] * r[0])) / 6.0;
        }
        for (int c = 0; c < ns; c++) {
            double dx = bb[c][3] - bb[c][0], dy = bb[c][4] - bb[c][1], dz = bb[c][5] - bb[c][2], d = sqrt(dx * dx + dy * dy + dz * dz);
            fl[c] = vol[c] < -1e-4 * d * d * d;      /* las cáscaras planas (volumen ~0) se dejan como están */
        }
        for (size_t i = 0; i < n; i++) if (fl[shell[i]]) flip_tri(&l->t[i]);
        for (size_t i = 0; i < n; i++) {   /* normales siguen a la geometría */
            Tri *t = &l->t[i];
            double e1[3], e2[3], c[3], av[3] = {0, 0, 0};
            sub3(t->v[1].p, t->v[0].p, e1); sub3(t->v[2].p, t->v[0].p, e2); cross3(e1, e2, c);
            for (int a = 0; a < 3; a++) for (int q = 0; q < 3; q++) av[q] += t->v[a].n[q];
            double lc = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]), la = sqrt(av[0] * av[0] + av[1] * av[1] + av[2] * av[2]);
            if (lc < AREA_MIN || la < 1e-6) continue;
            if ((c[0] * av[0] + c[1] * av[1] + c[2] * av[2]) / (lc * la) < -0.1)
                for (int a = 0; a < 3; a++) for (int q = 0; q < 3; q++) t->v[a].n[q] = -t->v[a].n[q];
        }
    }
    free(bb); free(vol); free(fl); free(ue); free(nb); free(shell); free(queue); free(st);
    if (rc) return rc;
    return comp_label(l);
}

/* ------------------------------------------------------------------ geometría generada (extrusión y cañón) */
static double tl_volume(const TL *l) {
    double v = 0;
    for (size_t i = 0; i < l->n; i++) {
        const float *a = l->t[i].v[0].p, *b = l->t[i].v[1].p, *c = l->t[i].v[2].p;
        v += ((double)a[0] * ((double)b[1] * c[2] - (double)b[2] * c[1]) - (double)a[1] * ((double)b[0] * c[2] - (double)b[2] * c[0])
              + (double)a[2] * ((double)b[0] * c[1] - (double)b[1] * c[0])) / 6.0;
    }
    return v;
}
/* triángulo con la normal geométrica orientada hacia 'want'; UV = esquinas del rectángulo de material */
static int gen_tri(TL *l, const float *a, const float *b, const float *c, const double *want, WCutUV uv, uint8_t kind, int comp, int corner0) {
    Tri t; t.comp = comp; t.cap = kind;
    memcpy(t.v[0].p, a, 12); memcpy(t.v[1].p, b, 12); memcpy(t.v[2].p, c, 12);
    double e1[3], e2[3], n[3];
    sub3(b, a, e1); sub3(c, a, e2); cross3(e1, e2, n);
    double ln = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (ln < 1e-18 * SC2) return 0;
    if (want && n[0] * want[0] + n[1] * want[1] + n[2] * want[2] < 0) { Vx tmp = t.v[1]; t.v[1] = t.v[2]; t.v[2] = tmp; for (int i = 0; i < 3; i++) n[i] = -n[i]; }
    static const int cu[4] = {0, 1, 1, 0}, cv[4] = {0, 0, 1, 1};
    for (int k = 0; k < 3; k++) {
        for (int i = 0; i < 3; i++) t.v[k].n[i] = (float)(n[i] / ln);
        int cc = (corner0 + k) & 3;
        t.v[k].uv[0] = cu[cc] ? uv.u1 : uv.u0; t.v[k].uv[1] = cv[cc] ? uv.v1 : uv.v0;
        t.v[k].c[0] = t.v[k].c[1] = t.v[k].c[2] = t.v[k].c[3] = 255;
    }
    return tl_push(l, &t);
}
static int gen_quad(TL *l, const float *a, const float *b, const float *c, const float *d, const double *want, WCutUV uv, uint8_t kind) {
    double e1[3], e2[3], n[3];
    sub3(b, a, e1); sub3(c, a, e2); cross3(e1, e2, n);
    int fl = want && n[0] * want[0] + n[1] * want[1] + n[2] * want[2] < 0;
    int rc = fl ? gen_tri(l, a, c, b, NULL, uv, kind, -1, 0) : gen_tri(l, a, b, c, NULL, uv, kind, -1, 0);
    if (!rc) rc = fl ? gen_tri(l, a, d, c, NULL, uv, kind, -1, 0) : gen_tri(l, a, c, d, NULL, uv, kind, -1, 0);
    return rc;
}

static int extrude_piece(TL *P, const WCutRule *ru) {
    const WCutExtrude *x = &ru->extrude;
    double d[3] = {x->dir.x, x->dir.y, x->dir.z}, dl = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (dl < 1e-9) return -1;
    for (int i = 0; i < 3; i++) d[i] /= dl;
    int rc = 0;
    TL T = {0}, Keep = {0}, New = {0};
    for (size_t i = 0; i < P->n && !rc; i++) {   /* tapas que miran hacia 'dir' */
        const Tri *t = &P->t[i];
        double e1[3], e2[3], n[3]; sub3(t->v[1].p, t->v[0].p, e1); sub3(t->v[2].p, t->v[0].p, e2); cross3(e1, e2, n);
        double ln = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (t->cap == WCUT_TRI_CAP && ln > 0 && (n[0] * d[0] + n[1] * d[1] + n[2] * d[2]) > 0.95 * ln) rc = tl_push(&T, t);
        else rc = tl_push(&Keep, t);
    }
    if (!rc && !T.n) rc = -7;
    size_t nT = T.n; EK *e = NULL; size_t ne = 0; EK *sel = NULL; float (*pa)[3] = NULL; int *sc = NULL; int nsel = 0; Loops L = {0};
    if (!rc) {
        e = edges_of(&T, &ne);
        sel = (EK *)malloc((nT * 3 + 1) * sizeof(EK)); pa = (float (*)[3])malloc((nT * 3 + 1) * sizeof(float[3])); sc = (int *)malloc((nT * 3 + 1) * sizeof(int));
        if (!e || !sel || !pa || !sc) rc = -3;
    }
    for (size_t i = 0; i < nT && !rc; i++)
        for (int a = 0; a < 3; a++) {
            EK k; k.a = pkey(T.t[i].v[a].p); k.b = pkey(T.t[i].v[(a + 1) % 3].p);
            if (!memcmp(&k.a, &k.b, sizeof(PK)) || has_rev(e, ne, &k)) continue;
            sel[nsel] = k; memcpy(pa[nsel], T.t[i].v[a].p, 12); sc[nsel] = T.t[i].comp; nsel++;
        }
    if (!rc) rc = build_loops(sel, (const float (*)[3])pa, sc, nsel, &L);
    if (!rc && !L.nloops) rc = -7;
    int off = 0;
    for (int l = 0; l < L.nloops && !rc; l++) {
        int n = L.len[l]; const P3 *pts = L.pt + off; off += n;
        if (n < 3) continue;
        double c[3] = {0, 0, 0}; for (int i = 0; i < n; i++) for (int a = 0; a < 3; a++) c[a] += pts[i].p[a] / n;
        P3 *q = (P3 *)malloc(sizeof(P3) * (size_t)n * 2);
        if (!q) { rc = -3; break; }
        P3 *up = q + n;
        for (int i = 0; i < n; i++) for (int a = 0; a < 3; a++) {
            double v = c[a] + x->inset * (pts[i].p[a] - c[a]);
            q[i].p[a] = (float)v; up[i].p[a] = (float)(v + d[a] * x->len);
        }
        Loop outer = { (P3 *)pts, n, L.comp[l], 1 }, hole = { q, n, L.comp[l], 1 }, top = { up, n, L.comp[l], 1 };
        const Loop *hl[1] = { &hole };
        rc = emit_polygon(&outer, hl, 1, ru->cap_piece, ru->cap_piece, &New, NULL);
        if (!rc) rc = emit_polygon(&top, NULL, 0, ru->cap_piece, ru->cap_piece, &New, NULL);
        for (int i = 0; i < n && !rc; i++) {
            int j = (i + 1) % n; double ed[3], w[3], u[3] = {d[0] * x->len, d[1] * x->len, d[2] * x->len};
            for (int a = 0; a < 3; a++) ed[a] = (double)q[j].p[a] - q[i].p[a];
            cross3(ed, u, w);
            rc = gen_quad(&New, q[i].p, q[j].p, up[j].p, up[i].p, w, ru->cap_piece, WCUT_TRI_CAP);
        }
        free(q);
    }
    if (!rc) {
        for (size_t i = 0; i < New.n && !rc; i++) rc = tl_push(&Keep, &New.t[i]);
        if (!rc) { tl_free(P); *P = Keep; Keep.t = NULL; Keep.n = Keep.cap = 0; }
    }
    loops_free(&L); free(e); free(sel); free(pa); free(sc); tl_free(&T); tl_free(&Keep); tl_free(&New);
    return rc;
}

/* Cañón bajo 'ref' (ver WCutTube). Perfora la cara frontal de 'ref' y devuelve el tubo en 'out'. */
static int gen_tube(const WCutRule *ru, BPiece *ref, TL *out) {
    const WCutTube *g = &ru->tube;
    TL *R = &ref->tl; int rc = 0;
    double zf = 1e30;
    for (size_t i = 0; i < R->n; i++) for (int k = 0; k < 3; k++) if (R->t[i].v[k].p[2] < zf) zf = R->t[i].v[k].p[2];
    TL Keep = {0}, F = {0}, New = {0};
    for (size_t i = 0; i < R->n && !rc; i++) {
        const Tri *t = &R->t[i];
        int fr = 1; for (int k = 0; k < 3; k++) if (fabs((double)t->v[k].p[2] - zf) > ON_TOL) fr = 0;
        double e1[3], e2[3], n[3]; sub3(t->v[1].p, t->v[0].p, e1); sub3(t->v[2].p, t->v[0].p, e2); cross3(e1, e2, n);
        double ln = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        rc = (fr && ln > 0 && n[2] < -0.95 * ln) ? tl_push(&F, t) : tl_push(&Keep, t);
    }
    if (!rc && !F.n) rc = -7;
    double xlo = 1e30, xhi = -1e30, ylo = 1e30, yhi = -1e30;
    for (size_t i = 0; i < F.n && !rc; i++) for (int k = 0; k < 3; k++) {
        double x = F.t[i].v[k].p[0], y = F.t[i].v[k].p[1];
        if (x < xlo) xlo = x;
        if (x > xhi) xhi = x;
        if (y < ylo) ylo = y;
        if (y > yhi) yhi = y;
    }
    double w = xhi - xlo, h = yhi - ylo, cx = (xlo + xhi) / 2, cy = g->axis_y;
    double Ro = g->r_frac * 0.5 * (w < h ? w : h), Rb = Ro - g->gap, rb = Rb * g->bore_frac;
    int ns = g->segs < 8 ? 8 : (g->segs > 48 ? 48 : g->segs);
    double zr = zf + g->length, zbe = zf + g->bore_depth;
    if (!rc && (Ro <= 0 || Rb <= 0 || rb <= 0 || rb >= Rb || cy - Ro < ylo || cy + Ro > yhi || cx - Ro < xlo || cx + Ro > xhi || zr <= zbe)) rc = -7;
    /* contorno de la cara frontal */
    EK *e = NULL; size_t ne = 0; EK *sel = NULL; float (*pa)[3] = NULL; int *sc = NULL; int nsel = 0; Loops L = {0};
    if (!rc) {
        e = edges_of(&F, &ne);
        sel = (EK *)malloc((F.n * 3 + 1) * sizeof(EK)); pa = (float (*)[3])malloc((F.n * 3 + 1) * sizeof(float[3])); sc = (int *)malloc((F.n * 3 + 1) * sizeof(int));
        if (!e || !sel || !pa || !sc) rc = -3;
    }
    for (size_t i = 0; i < F.n && !rc; i++)
        for (int a = 0; a < 3; a++) {
            EK k; k.a = pkey(F.t[i].v[a].p); k.b = pkey(F.t[i].v[(a + 1) % 3].p);
            if (!memcmp(&k.a, &k.b, sizeof(PK)) || has_rev(e, ne, &k)) continue;
            sel[nsel] = k; memcpy(pa[nsel], F.t[i].v[a].p, 12); sc[nsel] = F.t[i].comp; nsel++;
        }
    if (!rc) rc = build_loops(sel, (const float (*)[3])pa, sc, nsel, &L);
    if (!rc && L.nloops != 1) rc = -7;
    /* círculos (mismos puntos exactos en todas las superficies que los comparten) */
    float (*cr)[2] = NULL; P3 *cO = NULL;
    #define CPT(OUT_, R_, Z_, I_) do { (OUT_)[0] = (float)(cx + (R_) * cs[(I_) % ns]); (OUT_)[1] = (float)(cy + (R_) * sn[(I_) % ns]); (OUT_)[2] = (float)(Z_); } while (0)
    double cs[48], sn[48];
    for (int i = 0; i < ns; i++) { cs[i] = cos(6.283185307179586 * i / ns); sn[i] = sin(6.283185307179586 * i / ns); }
    (void)cr;
    if (!rc) {   /* cara frontal con orificio: UV afines de la mayor de las originales */
        cO = (P3 *)malloc(sizeof(P3) * (size_t)ns);
        if (!cO) rc = -3;
        else {
            for (int i = 0; i < ns; i++) CPT(cO[i].p, Ro, zf, i);
            Loop outer = { L.pt, L.len[0], L.comp[0], 1 }, hole = { cO, ns, L.comp[0], 1 };
            const Loop *hl[1] = { &hole };
            rc = emit_polygon(&outer, hl, 1, ru->cap_piece, ru->cap_piece, &New, NULL);
        }
        if (!rc) {
            size_t bi = 0; double ba = -1;
            for (size_t i = 0; i < F.n; i++) { double a = tri_area2(&F.t[i]); if (a > ba) { ba = a; bi = i; } }
            const Tri *o = &F.t[bi];
            double m00 = (double)o->v[1].p[0] - o->v[0].p[0], m01 = (double)o->v[2].p[0] - o->v[0].p[0];
            double m10 = (double)o->v[1].p[1] - o->v[0].p[1], m11 = (double)o->v[2].p[1] - o->v[0].p[1];
            double det = m00 * m11 - m01 * m10;
            if (fabs(det) < 1e-18 * SC2) rc = -7;
            for (size_t i = 0; i < New.n && !rc; i++) for (int k = 0; k < 3; k++) {
                Vx *v = &New.t[i].v[k];
                double dx = (double)v->p[0] - o->v[0].p[0], dy = (double)v->p[1] - o->v[0].p[1];
                double s = (m11 * dx - m01 * dy) / det, t = (-m10 * dx + m00 * dy) / det;
                for (int a = 0; a < 2; a++) v->uv[a] = (float)(o->v[0].uv[a] + s * (o->v[1].uv[a] - o->v[0].uv[a]) + t * (o->v[2].uv[a] - o->v[0].uv[a]));
                memcpy(v->n, o->v[0].n, 12); memcpy(v->c, o->v[0].c, 4);
                New.t[i].cap = WCUT_TRI_SKIN; New.t[i].comp = o->comp;
            }
        }
    }
    double v0 = tl_volume(R);
    if (!rc) {   /* canal en la pieza de referencia (paredes hacia dentro, fondo oscuro) */
        for (size_t i = 0; i < New.n && !rc; i++) rc = tl_push(&Keep, &New.t[i]);
        for (int i = 0; i < ns && !rc; i++) {
            float a[3], b[3], c[3], d[3]; double wn[3] = {-(cs[i] + cs[(i + 1) % ns]), -(sn[i] + sn[(i + 1) % ns]), 0};
            CPT(a, Ro, zf, i); CPT(b, Ro, zf, i + 1); CPT(c, Ro, zr, i + 1); CPT(d, Ro, zr, i);
            rc = gen_quad(&Keep, a, b, c, d, wn, ru->cap_hole, WCUT_TRI_FILL);
            float cc[3] = {(float)cx, (float)cy, (float)zr}; double zn[3] = {0, 0, -1};
            if (!rc) rc = gen_tri(&Keep, cc, c, d, zn, ru->cap_hole, WCUT_TRI_FILL, -1, 0);
        }
        if (!rc) { tl_free(R); *R = Keep; Keep.t = NULL; Keep.n = Keep.cap = 0; ref->gen_vol += tl_volume(R) - v0; }
    }
    if (!rc) {   /* el cañón */
        double zrb = zr - 0.0005;
        for (int i = 0; i < ns && !rc; i++) {
            float a[3], b[3], c[3], d[3]; double ro_[3] = {cs[i] + cs[(i + 1) % ns], sn[i] + sn[(i + 1) % ns], 0};
            double ri_[3] = {-ro_[0], -ro_[1], 0}, zm[3] = {0, 0, -1}, zp[3] = {0, 0, 1};
            CPT(a, Rb, zf, i); CPT(b, Rb, zf, i + 1); CPT(c, Rb, zrb, i + 1); CPT(d, Rb, zrb, i);
            rc = gen_quad(out, a, b, c, d, ro_, ru->cap_piece, WCUT_TRI_CAP);                    /* pared exterior */
            float a2[3], b2[3]; CPT(a2, rb, zf, i); CPT(b2, rb, zf, i + 1);
            if (!rc) rc = gen_quad(out, a, b, b2, a2, zm, ru->cap_piece, WCUT_TRI_CAP);          /* corona de la boca */
            float c2[3], d2[3]; CPT(c2, rb, zbe, i + 1); CPT(d2, rb, zbe, i);
            if (!rc) rc = gen_quad(out, a2, b2, c2, d2, ri_, ru->cap_hole, WCUT_TRI_FILL);       /* pared del ánima */
            float cb[3] = {(float)cx, (float)cy, (float)zbe};
            if (!rc) rc = gen_tri(out, cb, c2, d2, zm, ru->cap_hole, WCUT_TRI_FILL, -1, 0);      /* fondo del ánima */
            float cr_[3] = {(float)cx, (float)cy, (float)zrb};
            if (!rc) rc = gen_tri(out, cr_, c, d, zp, ru->cap_piece, WCUT_TRI_CAP, -1, 0);       /* culata */
        }
    }
    #undef CPT
    loops_free(&L); free(e); free(sel); free(pa); free(sc); free(cO); tl_free(&Keep); tl_free(&F); tl_free(&New);
    return rc;
}

static int run_impl(const WLoaded *m, const WCutModel *model, WCutSet *out, double scale);

int wcut_run(const WLoaded *m, const WCutModel *model, WCutSet *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!m || !model) return -1;
    double sc = 1.0;
    int rc = validate(m, model, &sc);
    if (rc) return rc;
    uint32_t nt = m->nv / 3;
    g_sc = sc; g_cap = 4 * (size_t)nt + 4096; g_push = 64.0 * ((double)nt + 1024.0); g_work = 4e8 + 2e5 * (double)nt;
    rc = run_impl(m, model, out, sc);
    g_sc = 1.0; g_cap = 0; g_push = g_work = 0;
    if (rc) wcut_free(out);
    return rc;
}

static int run_impl(const WLoaded *m, const WCutModel *model, WCutSet *out, double scale) {
    (void)scale;
    int rc = 0;
    BPiece pc[40]; int npc = 0; memset(pc, 0, sizeof pc);
    copy_name(pc[0].name, model->body); npc = 1;
    uint32_t nt = m->nv / 3;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t i = 0; i < nt && !rc; i++) {
        Tri t; t.comp = 0; t.cap = 0;
        for (int k = 0; k < 3; k++) {
            const WVertex *w = &m->v[m->idx[i * 3 + k]]; Vx *v = &t.v[k];
            memcpy(v->p, w->pos, 12); memcpy(v->n, w->nrm, 12); memcpy(v->uv, w->uv, 8); memcpy(v->c, w->rgba, 4);
            if (model->fix & WCUT_FIX_UPRIGHT) { v->p[0] = -v->p[0]; v->p[1] = -v->p[1]; v->n[0] = -v->n[0]; v->n[1] = -v->n[1]; }
            if (model->fix & WCUT_FIX_WINDING) for (int q = 0; q < 3; q++) v->n[q] = -v->n[q];
            for (int a = 0; a < 3; a++) { if (v->p[a] < lo[a]) lo[a] = v->p[a]; if (v->p[a] > hi[a]) hi[a] = v->p[a]; }
        }
        if (model->fix & WCUT_FIX_WINDING) { Vx tmp = t.v[1]; t.v[1] = t.v[2]; t.v[2] = tmp; }
        rc = tl_push(&pc[0].tl, &t);
    }
    out->src_tris = nt;
    if (!rc && model->src_tris) {   /* ¿es el modelo que describe la tabla? */
        if (nt != model->src_tris) rc = -4;
        else {
            const float *b[2] = {&model->bounds.lo.x, &model->bounds.hi.x};
            for (int a = 0; a < 3; a++) if (fabsf(lo[a] - b[0][a]) > WCUT_BB_TOL || fabsf(hi[a] - b[1][a]) > WCUT_BB_TOL) rc = -4;
        }
    }
    int ncomp = 0;
    if (!rc) { ncomp = orient(&pc[0].tl); if (ncomp < 0) rc = ncomp; }
    if (!rc && ncomp > 32) {   /* la máscara de componentes tiene 32 bits */
        for (int r = 0; r < model->nrules; r++) if (model->rules[r].sel.comps) rc = -6;
    }
    for (int r = 0; r < model->nrules && !rc; r++) {
        const WCutRule *ru = &model->rules[r];
        BPiece *src = ru->src && ru->src[0] ? find_piece(pc, npc, ru->src) : &pc[0];
        if (!src) { rc = -2; break; }
        TL P = {0}, R = {0};
        if (ru->tube.on) {   /* cañón generado: sin selección; modifica la pieza de referencia */
            BPiece *ref = find_piece(pc, npc, ru->tube.ref ? ru->tube.ref : "");
            if (!ref) { rc = -2; break; }
            rc = gen_tube(ru, ref, &P);
            if (!rc) { R = src->tl; src->tl.t = NULL; src->tl.n = src->tl.cap = 0; }   /* el origen no cambia */
            if (!rc) { double gv = tl_volume(&P); BPiece *dst0 = find_piece(pc, npc, ru->name); if (!dst0 && npc < 40) { dst0 = &pc[npc++]; copy_name(dst0->name, ru->name); dst0->rule = ru; } if (dst0) dst0->gen_vol += gv; }
        } else {
            rc = select_split(&src->tl, ru, &P, &R);
            if (!rc && ru->extrude.len > 0) {
                double v0 = tl_volume(&P);
                rc = extrude_piece(&P, ru);
                if (!rc) { BPiece *dst0 = find_piece(pc, npc, ru->name); if (!dst0 && npc < 40) { dst0 = &pc[npc++]; copy_name(dst0->name, ru->name); dst0->rule = ru; dst0->copy = (ru->flags & WCUT_COPY) ? 1 : 0; } if (dst0) dst0->gen_vol += tl_volume(&P) - v0; }
            }
        }
        if (!rc) {
            if (ru->tube.on) { src->tl = R; R.t = NULL; R.n = R.cap = 0; }
            else if (!(ru->flags & WCUT_COPY)) {
                TL old = src->tl; src->tl = R; R.t = NULL; R.n = R.cap = 0; tl_free(&old);
            }
            BPiece *dst = find_piece(pc, npc, ru->name);
            if (!dst) {
                if (npc >= 40) rc = -3;
                else { dst = &pc[npc++]; copy_name(dst->name, ru->name); dst->rule = ru; dst->copy = (ru->flags & WCUT_COPY) ? 1 : 0; }
            }
            if (dst) for (size_t i = 0; i < P.n && !rc; i++) rc = tl_push(&dst->tl, &P.t[i]);
        }
        tl_free(&P); tl_free(&R);
    }
    for (int i = 0; i < npc && !rc; i++) rc = clean_collinear(&pc[i].tl);
    for (int i = 0; i < npc && !rc; i++) if (pc[i].tl.n * 3 > 65535) rc = -5;   /* índices de 16 bits: se comprueba antes de reservar */
    if (rc) { for (int i = 0; i < npc; i++) tl_free(&pc[i].tl); return rc; }

    /* orden de salida: names, luego extras */
    int ord[48], nord = 0, extra_names = 0;
    const char *nm[48];
    if (model->names) for (; model->names[extra_names]; extra_names++) { nm[nord] = model->names[extra_names]; ord[nord] = -1; nord++; if (nord >= 40) break; }
    for (int i = 0; i < nord; i++) for (int j = 0; j < npc; j++) if (!strcmp(pc[j].name, nm[i])) ord[i] = j;
    int taken[40] = {0};
    for (int i = 0; i < nord; i++) if (ord[i] >= 0) taken[ord[i]] = 1;
    for (int j = 0; j < npc && nord < 48; j++) if (!taken[j]) { nm[nord] = pc[j].name; ord[nord] = j; nord++; }

    out->npiece = nord; out->nmeshes = nord;
    out->piece = (WCutPiece *)calloc((size_t)nord, sizeof(WCutPiece));
    out->meshes = (WMesh *)calloc((size_t)nord, sizeof(WMesh));
    out->mv = (WVertex **)calloc((size_t)nord, sizeof(WVertex *));
    out->mi = (uint16_t **)calloc((size_t)nord, sizeof(uint16_t *));
    out->parts = (WPart *)calloc((size_t)nord, sizeof(WPart));
    if (!out->piece || !out->meshes || !out->mv || !out->mi || !out->parts) { rc = -3; }
    for (int i = 0; i < nord && !rc; i++) {
        WCutPiece *pp = &out->piece[i];
        copy_name(pp->name, nm[i]);
        BPiece *b = ord[i] >= 0 ? &pc[ord[i]] : NULL;
        pp->copy = b ? b->copy : 0;
        pp->gen_vol = b ? b->gen_vol : 0;
        size_t n = b ? b->tl.n : 0;
        pp->nv = pp->ni = (uint32_t)(n * 3);
        pp->v = (WVertex *)calloc(n * 3 + 1, sizeof(WVertex)); pp->idx = (uint16_t *)calloc(n * 3 + 1, sizeof(uint16_t));
        pp->kind = (uint8_t *)calloc(n + 1, 1);
        if (!pp->v || !pp->idx || !pp->kind) { rc = -3; break; }
        float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
        for (size_t t = 0; t < n; t++) {
            Tri *tt = &b->tl.t[t];   /* las normales de salida concuerdan siempre con el orden de los vértices */
            double e1[3], e2[3], c[3], av[3] = {0, 0, 0};
            sub3(tt->v[1].p, tt->v[0].p, e1); sub3(tt->v[2].p, tt->v[0].p, e2); cross3(e1, e2, c);
            double lc = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
            for (int a = 0; a < 3; a++) for (int q = 0; q < 3; q++) av[q] += tt->v[a].n[q];
            if (lc > 0) {
                if (tt->cap) { for (int a = 0; a < 3; a++) for (int q = 0; q < 3; q++) tt->v[a].n[q] = (float)(c[q] / lc); }
                else if (c[0] * av[0] + c[1] * av[1] + c[2] * av[2] < 0)
                    for (int a = 0; a < 3; a++) for (int q = 0; q < 3; q++) tt->v[a].n[q] = -tt->v[a].n[q];
            }
        }
        for (size_t t = 0; t < n; t++) {
            pp->kind[t] = b->tl.t[t].cap;
            if (b->tl.t[t].cap) pp->ncap++;
            for (int k = 0; k < 3; k++) {
                const Vx *v = &b->tl.t[t].v[k]; WVertex *w = &pp->v[t * 3 + k];
                memcpy(w->pos, v->p, 12); memcpy(w->nrm, v->n, 12); memcpy(w->uv, v->uv, 8); memcpy(w->rgba, v->c, 4);
                pp->idx[t * 3 + k] = (uint16_t)(t * 3 + k);
                for (int a = 0; a < 3; a++) { if (v->p[a] < mn[a]) mn[a] = v->p[a]; if (v->p[a] > mx[a]) mx[a] = v->p[a]; }
            }
        }
        if (!n) { mn[0] = mn[1] = mn[2] = mx[0] = mx[1] = mx[2] = 0; }
        pp->bmin = (WVec3){mn[0], mn[1], mn[2]}; pp->bmax = (WVec3){mx[0], mx[1], mx[2]};
        const WCutRule *ru = b ? b->rule : NULL;
        WVec3 pv;
        if (b && ord[i] == 0) pv = model->body_pivot;
        else if (ru) pv = ru->pivot; else pv = (WVec3){NAN, NAN, NAN};
        float c3[3] = {(mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2};
        float pa[3] = {pv.x, pv.y, pv.z};
        for (int a = 0; a < 3; a++) if (isnan(pa[a])) pa[a] = c3[a];
        pp->pivot = (WVec3){pa[0], pa[1], pa[2]};
    }
    if (!rc) {
        for (int i = 0; i < nord && !rc; i++) {
            WCutPiece *pp = &out->piece[i];
            out->mv[i] = (WVertex *)calloc(pp->nv + 1, sizeof(WVertex)); out->mi[i] = (uint16_t *)calloc(pp->ni + 1, sizeof(uint16_t));
            if (!out->mv[i] || !out->mi[i]) { rc = -3; break; }
            for (uint32_t k = 0; k < pp->nv; k++) {
                out->mv[i][k] = pp->v[k];
                out->mv[i][k].pos[0] -= pp->pivot.x; out->mv[i][k].pos[1] -= pp->pivot.y; out->mv[i][k].pos[2] -= pp->pivot.z;
                out->mi[i][k] = pp->idx[k];
            }
            WMesh *me = &out->meshes[i];
            me->name = pp->name; me->v = out->mv[i]; me->nv = pp->nv; me->idx = out->mi[i]; me->ni = pp->ni;
            me->texture = m->atlas ? 1 : 0;
        }
    }
    if (!rc) {   /* WPart: una por pieza con regla, más el cuerpo */
        for (int i = 0; i < nord; i++) {
            const WCutPiece *pp = &out->piece[i];
            int j = -1; for (int q = 0; q < npc; q++) if (!strcmp(pc[q].name, pp->name)) j = q;
            if (j < 0) continue;
            WPart *pt = &out->parts[out->nparts++];
            const WCutRule *ru = pc[j].rule;
            pt->name = pp->name; pt->mesh = i; pt->rest.q = (WQuat){0, 0, 0, 1};
            pt->parent = -1; pt->channel = WCH_NONE; pt->axis = (WVec3){0, 0, 0}; pt->rotates = 0;
            WVec3 pp_pivot = pp->pivot;
            if (ru) {
                pt->channel = ru->channel; pt->axis = ru->axis; pt->rotates = ru->rotates;
                if (ru->parent) {
                    for (int q = 0; q < nord; q++) if (!strcmp(out->piece[q].name, ru->parent)) {
                        pp_pivot = (WVec3){pp->pivot.x - out->piece[q].pivot.x, pp->pivot.y - out->piece[q].pivot.y, pp->pivot.z - out->piece[q].pivot.z};
                        pt->parent = -2 - q;   /* se resuelve abajo al índice de parts */
                    }
                }
            }
            pt->rest.p = pp_pivot;
        }
        for (int i = 0; i < out->nparts; i++)       /* parent: de índice de pieza a índice de parts */
            if (out->parts[i].parent <= -2) {
                int q = -2 - out->parts[i].parent, found = -1;
                for (int k = 0; k < out->nparts; k++) if (out->parts[k].mesh == q) found = k;
                out->parts[i].parent = found;
            }
    }
    if (!rc && m->atlas) {
        size_t sz = (size_t)m->atlas_w * m->atlas_h * 4;
        out->atlas = (uint8_t *)malloc(sz ? sz : 1);
        if (!out->atlas) rc = -3; else { memcpy(out->atlas, m->atlas, sz); out->atlas_w = m->atlas_w; out->atlas_h = m->atlas_h; }
    }
    for (int i = 0; i < npc; i++) tl_free(&pc[i].tl);
    return rc;
}
