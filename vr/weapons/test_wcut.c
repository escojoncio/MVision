/* Prueba de wcut (cortador de modelos). Dos partes:
 *   1. sintética (siempre): entrada corrupta, tabla distinta del modelo (-4), máscara de componentes con más de 32,
 *      pieza de más de 65535 vértices, y un prisma con agujero pasante cortado por la mitad (la tapa es un anillo: sin
 *      doble recubrimiento);
 *   2. sobre los modelos reales del archivo de stages del usuario (se omite si no hay datos).
 * Salida: solo OK / FALLO (con el nombre de la comprobación, sin recuentos ni volúmenes de los modelos reales).
 * Para depurar: WCUT_VERBOSE=1 imprime también los valores medidos.
 *   make -C vr/weapons test
 * Búsqueda del archivo: argumento 1, variable STAGE_FILE, gamedata/stages.bin o ../../gamedata/stages.bin. El stage
 * residente se toma de STAGE_NAME (por defecto "init").
 * Comprobaciones por modelo: orden y contenido de piezas, ejes/canales/nombres idénticos a los del arma del registro,
 * sin triángulos degenerados, normales coherentes con el orden, volumen con signo >= 0, aristas abiertas EXACTAS (solo
 * las del original abierto, documentadas abajo), tapas sin solape, UV de tapas dentro de su material, rest.p + vértice
 * = posición original, suma de volúmenes = original (menos la geometría generada), y las comprobaciones propias de la
 * pistola (cañón y cargador). */
#include "wcut.h"
#include "weapon.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, verbose;
#define CHECK(c, ...) do { if (!(c)) { printf("  FALLO: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
#define VERB(...) do { if (verbose) { printf("    " __VA_ARGS__); printf("\n"); } } while (0)

extern const float w_pist_bare_muzzle_z;

/* ------------------------------------------------------------------ utilidades */
static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = (uint8_t *)malloc(l > 0 ? (size_t)l : 1);
    if (!d || fread(d, 1, (size_t)l, f) != (size_t)l) { free(d); fclose(f); return NULL; }
    fclose(f); *n = (size_t)l; return d;
}
static void tri_n(const WCutPiece *p, uint32_t t, double *n) {   /* normal geométrica sin normalizar */
    const float *a = p->v[p->idx[t * 3]].pos, *b = p->v[p->idx[t * 3 + 1]].pos, *c = p->v[p->idx[t * 3 + 2]].pos;
    double e1[3], e2[3];
    for (int i = 0; i < 3; i++) { e1[i] = (double)b[i] - a[i]; e2[i] = (double)c[i] - a[i]; }
    n[0] = e1[1] * e2[2] - e1[2] * e2[1]; n[1] = e1[2] * e2[0] - e1[0] * e2[2]; n[2] = e1[0] * e2[1] - e1[1] * e2[0];
}
static int ray_hits(const double *o, const double *d, const float *a, const float *b, const float *c) {
    double e1[3], e2[3], pv[3], tv[3], qv[3];
    for (int i = 0; i < 3; i++) { e1[i] = (double)b[i] - a[i]; e2[i] = (double)c[i] - a[i]; }
    pv[0] = d[1] * e2[2] - d[2] * e2[1]; pv[1] = d[2] * e2[0] - d[0] * e2[2]; pv[2] = d[0] * e2[1] - d[1] * e2[0];
    double det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
    if (fabs(det) < 1e-18) return 0;
    double inv = 1.0 / det;
    for (int i = 0; i < 3; i++) tv[i] = o[i] - a[i];
    double u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv;
    if (u < 0 || u > 1) return 0;
    qv[0] = tv[1] * e1[2] - tv[2] * e1[1]; qv[1] = tv[2] * e1[0] - tv[0] * e1[2]; qv[2] = tv[0] * e1[1] - tv[1] * e1[0];
    double v = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) * inv;
    if (v < 0 || u + v > 1) return 0;
    return (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv > 1e-9;
}

/* ¿hay dos triángulos coplanares de la pieza que se solapan (centroide de uno estrictamente dentro del otro)? */
static int piece_overlaps(const WCutPiece *p) {
    uint32_t nt = p->ni / 3;
    for (uint32_t i = 0; i < nt; i++) {
        double ni[3]; tri_n(p, i, ni);
        double li = sqrt(ni[0] * ni[0] + ni[1] * ni[1] + ni[2] * ni[2]);
        if (li < 1e-13) continue;
        const float *a = p->v[p->idx[i * 3]].pos;
        for (uint32_t j = i + 1; j < nt; j++) {
            double nj[3]; tri_n(p, j, nj);
            double lj = sqrt(nj[0] * nj[0] + nj[1] * nj[1] + nj[2] * nj[2]);
            if (lj < 1e-13) continue;
            if (p->kind[i] == WCUT_TRI_SKIN && p->kind[j] == WCUT_TRI_SKIN) continue;   /* los solapes de la piel son del original */
            if (ni[0] * nj[0] + ni[1] * nj[1] + ni[2] * nj[2] < 0.9999 * li * lj) continue;   /* mismo sentido: las opuestas son láminas finas */
            const float *b0 = p->v[p->idx[j * 3]].pos;
            double dd = 0; for (int k = 0; k < 3; k++) dd += ((double)b0[k] - a[k]) * ni[k] / li;
            if (fabs(dd) > 1e-6) continue;
            for (int pass = 0; pass < 2; pass++) {
                uint32_t x = pass ? j : i, y = pass ? i : j;   /* ¿el centroide de x está dentro de y? */
                double c[3] = {0, 0, 0};
                for (int k = 0; k < 3; k++) for (int q = 0; q < 3; q++) c[q] += p->v[p->idx[x * 3 + k]].pos[q] / 3.0;
                const float *A = p->v[p->idx[y * 3]].pos, *B = p->v[p->idx[y * 3 + 1]].pos, *C = p->v[p->idx[y * 3 + 2]].pos;
                double ny[3]; tri_n(p, y, ny); double ly2 = ny[0] * ny[0] + ny[1] * ny[1] + ny[2] * ny[2];
                double s[3];
                const float *V[3] = {A, B, C};
                for (int k = 0; k < 3; k++) {
                    const float *P0 = V[k], *P1 = V[(k + 1) % 3];
                    double e[3], w[3], cr[3];
                    for (int q = 0; q < 3; q++) { e[q] = (double)P1[q] - P0[q]; w[q] = c[q] - P0[q]; }
                    cr[0] = e[1] * w[2] - e[2] * w[1]; cr[1] = e[2] * w[0] - e[0] * w[2]; cr[2] = e[0] * w[1] - e[1] * w[0];
                    s[k] = (cr[0] * ny[0] + cr[1] * ny[1] + cr[2] * ny[2]) / ly2;   /* ~ coordenada baricéntrica */
                }
                if (s[0] > 1e-4 && s[1] > 1e-4 && s[2] > 1e-4) { VERB("   solape %u(k%d) %u(k%d) dot %.3f", i, p->kind[i], j, p->kind[j], (ni[0]*nj[0]+ni[1]*nj[1]+ni[2]*nj[2])/(li*lj)); return 1; }
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ parte sintética */
typedef struct { WVertex *v; uint16_t *idx; uint32_t n, cap; } Soup;
static void soup_tri(Soup *s, const double *a, const double *b, const double *c, const double *want) {
    if (s->n + 3 > s->cap) { s->cap = s->cap ? s->cap * 2 : 96; s->v = (WVertex *)realloc(s->v, s->cap * sizeof(WVertex)); s->idx = (uint16_t *)realloc(s->idx, s->cap * 2); }
    double e1[3], e2[3], n[3];
    for (int i = 0; i < 3; i++) { e1[i] = b[i] - a[i]; e2[i] = c[i] - a[i]; }
    n[0] = e1[1] * e2[2] - e1[2] * e2[1]; n[1] = e1[2] * e2[0] - e1[0] * e2[2]; n[2] = e1[0] * e2[1] - e1[1] * e2[0];
    int flip = n[0] * want[0] + n[1] * want[1] + n[2] * want[2] < 0;
    const double *P[3] = {a, flip ? c : b, flip ? b : c};
    double l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]); if (l == 0) l = 1;
    for (int k = 0; k < 3; k++) {
        WVertex *w = &s->v[s->n]; memset(w, 0, sizeof *w);
        for (int i = 0; i < 3; i++) { w->pos[i] = (float)P[k][i]; w->nrm[i] = (float)((flip ? -n[i] : n[i]) / l); }
        w->uv[0] = (float)(0.1 * k); w->uv[1] = 0.2f; w->rgba[0] = w->rgba[1] = w->rgba[2] = w->rgba[3] = 255;
        s->idx[s->n] = (uint16_t)s->n; s->n++;
    }
}
static void soup_quad(Soup *s, const double *a, const double *b, const double *c, const double *d, const double *want) {
    soup_tri(s, a, b, c, want); soup_tri(s, a, c, d, want);
}
static WLoaded soup_loaded(Soup *s) {
    WLoaded m; memset(&m, 0, sizeof m);
    m.v = s->v; m.idx = s->idx; m.nv = m.ni = s->n; m.nparts = 0;
    return m;
}
/* prisma 20x20 (mm) de z=-20 a 20 con un agujero pasante cuadrado de 8x8: variedad cerrada de género 1 */
static void make_holed_prism(Soup *s) {
    const double O = 0.010, I = 0.004, Z = 0.020;
    for (int e = 0; e < 2; e++) {   /* extremos: coronas */
        double z = e ? Z : -Z, wn[3] = {0, 0, e ? 1 : -1};
        double o[4][3] = {{-O, -O, z}, {O, -O, z}, {O, O, z}, {-O, O, z}}, in[4][3] = {{-I, -I, z}, {I, -I, z}, {I, I, z}, {-I, I, z}};
        for (int i = 0; i < 4; i++) soup_quad(s, o[i], o[(i + 1) % 4], in[(i + 1) % 4], in[i], wn);
    }
    for (int i = 0; i < 4; i++) {   /* paredes exteriores e interiores */
        double a[2][2] = {{-O, -O}, {O, -O}}, dummy = 0; (void)dummy; (void)a;
        double cx[4] = {-O, O, O, -O}, cy[4] = {-O, -O, O, O}, ix[4] = {-I, I, I, -I}, iy[4] = {-I, -I, I, I};
        int j = (i + 1) % 4;
        double p0[3] = {cx[i], cy[i], -Z}, p1[3] = {cx[j], cy[j], -Z}, p2[3] = {cx[j], cy[j], Z}, p3[3] = {cx[i], cy[i], Z};
        double wo[3] = {(cy[j] - cy[i]), -(cx[j] - cx[i]), 0};   /* hacia fuera */
        soup_quad(s, p0, p1, p2, p3, wo);
        double q0[3] = {ix[i], iy[i], -Z}, q1[3] = {ix[j], iy[j], -Z}, q2[3] = {ix[j], iy[j], Z}, q3[3] = {ix[i], iy[i], Z};
        double wi[3] = {-(iy[j] - iy[i]), (ix[j] - ix[i]), 0};    /* hacia el eje */
        soup_quad(s, q0, q1, q2, q3, wi);
    }
}
static const char *const SYN_NAMES[] = {"body", "half", NULL};
static const WCutRule SYN_RULES[] = {
    {.name = "half", .sel = {1, WCUT_BOX(0, 0, -15, 30, 30, 15, 0, 0, 0), WCUT_CLIP, 0, 0, {0, 0, 0, 0}},
     .cap_piece = {0.1f, 0.1f, 0.2f, 0.2f}, .cap_hole = {0.5f, 0.5f, 0.6f, 0.6f}, .pivot = {NAN, NAN, NAN}},
};
static const WCutModel SYN_MODEL = {.id = 1, .tag = "syn", .body = "body", .names = SYN_NAMES, .rules = SYN_RULES, .nrules = 1};

static void synthetic(void) {
    WCutSet s; WLoaded m;
    /* argumentos y entradas corruptas: siempre -1, 'out' a cero y wcut_free seguro */
    memset(&s, 0x5a, sizeof s);
    CHECK(wcut_run(NULL, &SYN_MODEL, &s) == -1 && s.piece == NULL, "m NULL");
    CHECK(wcut_run(&(WLoaded){0}, NULL, &s) == -1, "modelo NULL");
    CHECK(wcut_run(&(WLoaded){0}, &SYN_MODEL, NULL) == -1, "out NULL");
    wcut_free(&s); wcut_free(NULL);
    Soup so = {0}; make_holed_prism(&so);
    m = soup_loaded(&so);
    int r = wcut_run(&m, &SYN_MODEL, &s);
    CHECK(r == 0, "prisma con agujero: wcut_run");
    if (r == 0) {
        const WCutPiece *h = wcut_piece(&s, "half"), *b = wcut_piece(&s, "body");
        CHECK(h && b && h->ni && b->ni, "prisma con agujero: piezas");
        if (h && b) {
            CHECK(wcut_open_edges(h) == 0 && wcut_open_edges(b) == 0, "prisma con agujero: piezas cerradas");
            CHECK(!piece_overlaps(h) && !piece_overlaps(b), "prisma con agujero: tapas sin solape");
            /* área de la tapa = corona (20x20 - 8x8 mm); volumen de cada mitad = (400 - 64) * 20 mm3 */
            double area = 0;
            for (uint32_t t = 0; t < h->ni / 3; t++) if (h->kind[t] == WCUT_TRI_CAP) { double n[3]; tri_n(h, t, n); area += 0.5 * sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]); }
            CHECK(fabs(area - 336e-6) < 1e-8, "prisma con agujero: la tapa es un anillo (no cubre el agujero)");
            CHECK(fabs(wcut_volume(h) - 336e-6 * 0.020) < 1e-9 && fabs(wcut_volume(b) - 336e-6 * 0.020) < 1e-9, "prisma con agujero: volúmenes");
        }
        wcut_free(&s);
    }
    /* tabla que no es la del modelo */
    WCutModel wrong = SYN_MODEL; wrong.src_tris = 7;
    CHECK(wcut_run(&m, &wrong, &s) == -4, "nº de triángulos distinto -> -4");
    wrong.src_tris = so.n / 3; wrong.bounds = (WCutBounds)WCUT_BB(0, 0, 0, 1, 1, 1);
    CHECK(wcut_run(&m, &wrong, &s) == -4, "caja envolvente distinta -> -4");
    wrong.bounds = (WCutBounds)WCUT_BB(-10, -10, -20, 10, 10, 20);
    r = wcut_run(&m, &wrong, &s); CHECK(r == 0, "tabla que sí coincide"); if (r == 0) wcut_free(&s);
    /* mallas corruptas */
    WLoaded c = m;
    c.idx = (uint16_t *)malloc(so.n * 2); memcpy(c.idx, so.idx, so.n * 2);
    c.idx[5] = (uint16_t)(so.n + 3); CHECK(wcut_run(&c, &SYN_MODEL, &s) == -1 && !s.piece, "índice fuera de rango");
    c.idx[5] = 5; c.ni = so.n - 1; CHECK(wcut_run(&c, &SYN_MODEL, &s) == -1, "ni != nv");
    c.ni = c.nv = so.n - 1; CHECK(wcut_run(&c, &SYN_MODEL, &s) == -1, "nv no múltiplo de 3");
    c.ni = c.nv = 0; CHECK(wcut_run(&c, &SYN_MODEL, &s) == -1, "nv < 3");
    c.ni = c.nv = so.n; uint16_t *keep = c.idx; c.idx = NULL; CHECK(wcut_run(&c, &SYN_MODEL, &s) == -1, "idx NULL");
    free(keep);
    c = m; c.v = (WVertex *)malloc(so.n * sizeof(WVertex)); memcpy(c.v, so.v, so.n * sizeof(WVertex));
    c.v[3].pos[1] = NAN; CHECK(wcut_run(&c, &SYN_MODEL, &s) == -1, "posición NaN");
    c.v[3].pos[1] = 0; c.v[4].nrm[0] = INFINITY; CHECK(wcut_run(&c, &SYN_MODEL, &s) == -1, "normal infinita");
    c.v[4].nrm[0] = 0; c.v[5].pos[2] = 1e9f; CHECK(wcut_run(&c, &SYN_MODEL, &s) == -1, "coordenada enorme");
    free(c.v);
    /* geometría deformada: debe terminar sin colgarse (0 o error) */
    srand(7);
    for (int it = 0; it < 30; it++) {
        c = m; c.v = (WVertex *)malloc(so.n * sizeof(WVertex)); memcpy(c.v, so.v, so.n * sizeof(WVertex));
        for (int k = 0; k < 6; k++) c.v[rand() % so.n].pos[rand() % 3] += ((rand() % 2000) - 1000) * 1e-5f;
        r = wcut_run(&c, &SYN_MODEL, &s); if (r == 0) wcut_free(&s); else CHECK(r < 0 && !s.piece, "deformada: error limpio");
        free(c.v);
    }
    /* más de 32 componentes con máscara -> -6 */
    {
        Soup many = {0};
        for (int i = 0; i < 40; i++) {
            double o = i * 0.1, a[3] = {o, 0, 0}, b[3] = {o + 0.05, 0, 0}, d[3] = {o, 0.05, 0}, w[3] = {0, 0, 1};
            soup_tri(&many, a, b, d, w);
        }
        WLoaded mm = soup_loaded(&many);
        static const WCutRule rr[] = {{.name = "half", .sel = {0, {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}, WCUT_CLIP, 2, 0, {0, 0, 0, 0}}, .pivot = {NAN, NAN, NAN}}};
        WCutModel mod = SYN_MODEL; mod.rules = rr;
        CHECK(wcut_run(&mm, &mod, &s) == -6, "máscara con más de 32 componentes -> -6");
        mod.rules = NULL; mod.nrules = 0;
        r = wcut_run(&mm, &mod, &s); CHECK(r == 0, "40 componentes sin máscara"); if (r == 0) wcut_free(&s);
        free(many.v); free(many.idx);
    }
    /* una pieza de más de 65535 vértices -> -5 */
    {
        Soup big = {0};
        for (int i = 0; i < 21846; i++) {
            double o = (i % 200) * 0.01, p = (i / 200) * 0.01, a[3] = {o, p, 0}, b[3] = {o + 0.004, p, 0}, d[3] = {o, p + 0.004, 0}, w[3] = {0, 0, 1};
            soup_tri(&big, a, b, d, w);
            if (big.n > 65535) break;
        }
        big.cap = 0;   /* idx es uint16: la malla de entrada ya no cabe en 16 bits, se prueba con el límite de la pieza */
        free(big.v); free(big.idx);
        WLoaded bm; memset(&bm, 0, sizeof bm);
        uint32_t n = 65535 - (65535 % 3);   /* 65535 vértices de entrada: caben, pero con las tapas la pieza se pasa */
        bm.v = (WVertex *)calloc(n, sizeof(WVertex)); bm.idx = (uint16_t *)calloc(n, 2); bm.nv = bm.ni = n;
        for (uint32_t i = 0; i < n; i++) { bm.idx[i] = (uint16_t)i; uint32_t t = i / 3; bm.v[i].pos[0] = (float)(t % 100) * 0.01f + (i % 3 == 1 ? 0.004f : 0); bm.v[i].pos[1] = (float)(t / 100) * 0.01f + (i % 3 == 2 ? 0.004f : 0); bm.v[i].nrm[2] = 1; }
        r = wcut_run(&bm, &SYN_MODEL, &s);
        CHECK(r == -3 || r == -5 || r == 0, "malla grande: código válido");
        if (r == 0) wcut_free(&s);
        free(bm.v); free(bm.idx);
    }
    free(so.v); free(so.idx);
}

/* ------------------------------------------------------------------ modelos reales */
/* Aristas abiertas EXACTAS por (modelo, pieza); cualquier otra pieza debe quedar en 0. Todas vienen de superficies del
 * original que no son cerradas (se miden sobre el modelo sin reglas). Los cuerpos del fusil, del francotirador y del lanzador
 * guiado ya no figuran: la tabla los sella (WCutSeal: boca hueca del cañon, hueco del vientre del tubo) y quedan a 0.
 * Pendientes de sellar:
 *   9a90 round 10: cuerpo del misil antiaéreo (abierto en el original);
 *   57f8 body 4 / tube 3: el bucle de interfaz del tubo con el cuerpo es alabeado (no plano) en el original; la tapa se
 *       proyecta y quedan sus aristas sin pareja. */
typedef struct { uint16_t id; const char *piece; uint32_t open; } Open;
static const Open OPEN[] = {
    {0x57f8, "body", 4}, {0x57f8, "tube", 3}, {0x9a90, "round", 10},
};
static uint32_t open_expected(uint16_t id, const char *piece) {
    for (size_t i = 0; i < sizeof OPEN / sizeof OPEN[0]; i++) if (OPEN[i].id == id && !strcmp(OPEN[i].piece, piece)) return OPEN[i].open;
    return 0;
}
/* Volumen de la unión frente al original (5 %), salvo excepción documentada por modelo. */
typedef struct { uint16_t id; double tol; const char *why; } VolEx;
static const VolEx VOLEX[] = {
    {0x36f6, 0.10, "el original sin cortar tiene 6 aristas abiertas (el supresor no esta cerrado): su volumen por divergencia depende del origen; la pieza cortada si queda cerrada"},
};
static double vol_tol(uint16_t id) {
    for (size_t i = 0; i < sizeof VOLEX / sizeof VOLEX[0]; i++) if (VOLEX[i].id == id && VOLEX[i].tol > 0) return VOLEX[i].tol;
    return 0.05;
}

static const WPart *def_part(const WeaponDef *d, const char *name) {
    for (int i = 0; i < d->nparts; i++) if (!strcmp(d->parts[i].name, name)) return &d->parts[i];
    return NULL;
}
static const WeaponDef *def_find(const char *id) {
    for (int i = 0; i < w_registry_count; i++) if (!strcmp(w_registry[i]->id, id)) return w_registry[i];
    return NULL;
}

static void check_weapon(const char *tag, const WCutModel *cm) {
    if (!cm->weapon) return;
    const WeaponDef *d = def_find(cm->weapon);
    CHECK(d != NULL, "%s: arma '%s' no está en el registro", tag, cm->weapon);
    if (!d) return;
    for (int i = 0; cm->names && cm->names[i]; i++)
        CHECK(d->mesh_names && d->mesh_names[i] && !strcmp(cm->names[i], d->mesh_names[i]), "%s: nombre de pieza #%d distinto del arma", tag, i);
    for (int r = 0; r < cm->nrules; r++) {
        const WCutRule *ru = &cm->rules[r];
        if (ru->channel == WCH_NONE) continue;
        const char *pn = ru->name;
        if (cm->id == 0x57f8 && !strcmp(pn, "tube")) pn = "tube_slide";   /* en el arma, el deslizamiento es de la pieza padre */
        const WPart *wp = def_part(d, pn);
        if (!wp) continue;   /* el arma no articula esta pieza (objeto suelto) */
        CHECK(wp->channel == ru->channel, "%s: canal de '%s' distinto del arma", tag, ru->name);
        CHECK(fabsf(wp->axis.x - ru->axis.x) < 1e-3f && fabsf(wp->axis.y - ru->axis.y) < 1e-3f && fabsf(wp->axis.z - ru->axis.z) < 1e-3f,
              "%s: eje de '%s' distinto del arma", tag, ru->name);
        CHECK(wp->rotates == ru->rotates, "%s: rotates de '%s' distinto del arma", tag, ru->name);
    }
}

/* ¿el punto está dentro del sólido original (paridad de cruces de un rayo oblicuo)? marco corregido: x,y negadas */
static int inside_orig(const WLoaded *m, const float *pt) {   /* mayoría de tres rayos oblicuos (el original tiene piezas internas solapadas) */
    static const double D[3][3] = {{0.5773, 0.3711, 0.7311}, {-0.3141, 0.8123, -0.4922}, {0.2718, -0.6180, -0.7371}};
    double o[3] = {-pt[0], -pt[1], pt[2]}; int votes = 0;
    for (int r = 0; r < 3; r++) {
        int n = 0;
        for (uint32_t i = 0; i + 2 < m->ni; i += 3) n += ray_hits(o, D[r], m->v[m->idx[i]].pos, m->v[m->idx[i + 1]].pos, m->v[m->idx[i + 2]].pos);
        votes += n & 1;
    }
    return votes >= 2;
}

static void check_pistol(const char *tag, const WCutModel *cm, const WLoaded *m, const WCutSet *s) {
    const WCutPiece *bar = wcut_piece(s, "barrel"), *mag = wcut_piece(s, "mag"), *slide = wcut_piece(s, "slide");
    CHECK(bar && bar->ni >= 3, "%s: el cañón existe y no está vacío", tag);
    CHECK(mag && mag->ni >= 3 && slide, "%s: cargador y corredera", tag);
    const WeaponDef *d = def_find("pist");
    if (!bar || !mag || !slide || !d) return;
    CHECK(wcut_open_edges(bar) == 0, "%s: el cañón es cerrado", tag);
    /* boca: los vértices del cañón en la z mínima; su centro y radio exterior */
    float zm = bar->bmin.z; double cx = 0, cy = 0; int n = 0; float rmax = 0;
    for (uint32_t k = 0; k < bar->nv; k++) if (fabsf(bar->v[k].pos[2] - zm) < 1e-6f) { cx += bar->v[k].pos[0]; cy += bar->v[k].pos[1]; n++; }
    CHECK(n >= 8, "%s: boca del cañón", tag);
    if (n) {
        cx /= n; cy /= n;
        for (uint32_t k = 0; k < bar->nv; k++) if (fabsf(bar->v[k].pos[2] - zm) < 1e-6f) {
            float r = hypotf((float)(bar->v[k].pos[0] - cx), (float)(bar->v[k].pos[1] - cy)); if (r > rmax) rmax = r;
        }
        const WVec3 mz = d->points[WP_MUZZLE].p;
        float zexp = cm->id == 0x36f6 ? wcut_piece(s, "supp")->bmin.z : w_pist_bare_muzzle_z;   /* con supresor, WP_MUZZLE es su punta */
        CHECK(hypotf((float)(mz.x - cx), (float)(mz.y - cy)) < rmax, "%s: WP_MUZZLE cae dentro de la boca del cañón", tag);
        CHECK(fabsf(mz.x - (float)cx) < 1e-3f && fabsf(mz.y - (float)cy) < 1e-3f, "%s: WP_MUZZLE centrado en la boca", tag);
        CHECK(fabsf(zexp - (cm->id == 0x36f6 ? mz.z : zm)) < 1e-3f, "%s: z de la boca / punta del supresor", tag);
        if (cm->id == 0x36f6) {   /* el supresor se acopla al extremo del cañón */
            const WCutPiece *su = wcut_piece(s, "supp");
            CHECK(su && fabsf(su->pivot.z - zm) < 1e-3f, "%s: el supresor arranca en la boca del cañón", tag);
        }
        VERB("%s: boca z=%.4f radio=%.4f", tag, zm, rmax);
    }
    /* el cañón es visible al retroceder la corredera: su caja sale de la de la corredera hacia atrás y está sobre el marco */
    CHECK(bar->bmax.z < slide->bmax.z && bar->bmin.z >= slide->bmin.z - 1e-4f, "%s: el cañón queda bajo la corredera", tag);
    /* cargador completo: volumen cerrado; solo la base (franja del fondo) queda fuera del contorno del mango */
    CHECK(wcut_open_edges(mag) == 0, "%s: el cargador es cerrado", tag);
    float ytop = mag->bmin.y + 0.0075f; int nstub = 0, outside = 0;
    for (uint32_t k = 0; k < mag->nv; k++) {
        if (mag->v[k].pos[1] <= ytop) continue;
        nstub++;
        if (!inside_orig(m, mag->v[k].pos)) { outside++; VERB("   fuera %.2f %.2f %.2f mm kind %d", mag->v[k].pos[0]*1e3, mag->v[k].pos[1]*1e3, mag->v[k].pos[2]*1e3, mag->kind[k/3]); }
    }
    CHECK(nstub > 0, "%s: el cargador tiene cuerpo interior", tag);
    CHECK(outside == 0, "%s: parte del cargador sobresale del contorno del mango", tag);
    CHECK(mag->bmin.y <= -0.0359f, "%s: base a ras del fondo del mango", tag);
    /* altura = la del pozo: de la base a la cara inferior de la corredera menos la holgura de la bala (0,8 mm) */
    float gap = slide->bmin.y - mag->bmax.y;
    CHECK(gap > 0.0006f && gap < 0.0013f, "%s: holgura del cargador bajo la corredera (%.2f mm)", tag, gap * 1e3);
    CHECK(mag->bmax.y - mag->bmin.y > 0.075f, "%s: altura del cargador", tag);
    /* seccion = contorno del mango menos la pared: los vertices del cuerpo quedan dentro del ancho del mango (22 mm) */
    float xm = 0; for (uint32_t k = 0; k < mag->nv; k++) if (mag->v[k].pos[1] > ytop && fabsf(mag->v[k].pos[0]) > xm) xm = fabsf(mag->v[k].pos[0]);
    CHECK(xm < 0.0090f && xm > 0.0075f, "%s: ancho del cuerpo del cargador (%.2f mm de semiancho)", tag, xm * 1e3);
    /* calibre: la bala tiene el diametro del anima del canon generado y la vaina 1,1 veces; la bala usa su propio material */
    {
        const WCutRule *mr = NULL; for (int q = 0; q < cm->nrules; q++) if (cm->rules[q].mag.on) mr = &cm->rules[q];
        float rin = 1e9f;
        for (uint32_t k = 0; k < bar->nv; k++) if (fabsf(bar->v[k].pos[2] - zm) < 1e-6f) {
            float r = hypotf((float)(bar->v[k].pos[0] - cx), (float)(bar->v[k].pos[1] - cy)); if (r < rin) rin = r;
        }
        float bxm = 0, cxm = 0;
        for (uint32_t t = 0; mr && t < mag->ni / 3; t++) {
            const WCutUV *u = NULL;
            const float *uv = mag->v[mag->idx[t * 3]].uv;
            #define INUV(R) (uv[0] >= (R).u0 - 1e-5f && uv[0] <= (R).u1 + 1e-5f && uv[1] >= (R).v0 - 1e-5f && uv[1] <= (R).v1 + 1e-5f)
            if (INUV(mr->mag.mat_bullet)) u = &mr->mag.mat_bullet; else if (INUV(mr->mag.mat_case)) u = &mr->mag.mat_case;
            #undef INUV
            if (!u) continue;
            float lo = 1e9f, hi = -1e9f;
            for (int k = 0; k < 3; k++) { float x = mag->v[mag->idx[t * 3 + k]].pos[0]; if (x < lo) lo = x; if (x > hi) hi = x; }
            if (u == &mr->mag.mat_bullet) { if (hi > bxm) bxm = hi; } else if (hi > cxm) cxm = hi;
        }
        VERB("%s: radio del anima %.3f mm, bala max x %.3f, vaina max x %.3f", tag, rin * 1e3, bxm * 1e3, cxm * 1e3);
        CHECK(rin > 0.002f && rin < 0.004f, "%s: radio del anima del canon", tag);
        /* el cartucho esta centrado en x en la ranura (centro = cx del cargador, que es 0 en este modelo): radio = max x */
        CHECK(fabsf(bxm - rin) < 2e-5f, "%s: la bala tiene el calibre del anima", tag);
        CHECK(fabsf(cxm - 1.1f * rin) < 2e-5f, "%s: la vaina mide 1,1 veces el calibre", tag);
    }
    /* color del cuerpo del cargador: casi negro como el mango. Sale de los texels mas oscuros del propio modelo: su luminancia
     * (media del bloque) no pasa del tope de 28/255 ni del percentil 10 de los texels del mango (triangulos del original cuyo
     * centroide queda bajo y = -25 mm en el marco bruto de wload), y el bloque es uniforme (sin dibujo) y de color casi neutro */
    {
        const WCutRule *mr = NULL; for (int q = 0; q < cm->nrules; q++) if (cm->rules[q].mag.on) mr = &cm->rules[q];
        CHECK(mr && m->atlas && m->atlas_w && m->atlas_h, "%s: atlas para el color del cuerpo", tag);
        if (mr && m->atlas) {
            const uint32_t W = m->atlas_w, H = m->atlas_h;
            int hist[256] = {0}, nh = 0;
            for (uint32_t i = 0; i + 2 < m->ni; i += 3) {
                float cy = 0, u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
                for (int k = 0; k < 3; k++) {
                    const WVertex *vv = &m->v[m->idx[i + k]]; cy += vv->pos[1] / 3;
                    u0 = fminf(u0, vv->uv[0]); u1 = fmaxf(u1, vv->uv[0]); v0 = fminf(v0, vv->uv[1]); v1 = fmaxf(v1, vv->uv[1]);
                }
                if (cy >= -0.025f) continue;
                for (uint32_t y = (uint32_t)(v0 * H); y <= (uint32_t)(v1 * H) && y < H; y++)
                    for (uint32_t x = (uint32_t)(u0 * W); x <= (uint32_t)(u1 * W) && x < W; x++) {
                        const uint8_t *px = m->atlas + 4 * ((size_t)y * W + x);
                        hist[(px[0] * 30 + px[1] * 59 + px[2] * 11) / 100]++; nh++;
                    }
            }
            int p10 = 0, acc = 0; while (p10 < 255 && acc + hist[p10] < nh / 10) acc += hist[p10++];
            const WCutUV *u = &mr->mag.mat_body;
            uint32_t x0 = (uint32_t)(u->u0 * W + 0.5f), x1 = (uint32_t)(u->u1 * W + 0.5f), y0 = (uint32_t)(u->v0 * H + 0.5f), y1 = (uint32_t)(u->v1 * H + 0.5f);
            if (x1 > W) x1 = W;
            if (y1 > H) y1 = H;  /* tabla corrupta: no leer fuera del atlas */
            int lmin = 255, lmax = 0, cmax = 0;
            for (uint32_t y = y0; y < y1; y++) for (uint32_t x = x0; x < x1; x++) {
                const uint8_t *px = m->atlas + 4 * ((size_t)y * W + x);
                int l = (px[0] * 30 + px[1] * 59 + px[2] * 11) / 100; lmin = l < lmin ? l : lmin; lmax = l > lmax ? l : lmax;
                int mx = px[0] > px[1] ? px[0] : px[1]; mx = px[2] > mx ? px[2] : mx;
                int mn = px[0] < px[1] ? px[0] : px[1]; mn = px[2] < mn ? px[2] : mn; cmax = mx - mn > cmax ? mx - mn : cmax;
            }
            VERB("%s: cuerpo del cargador: luminancia %d..%d, percentil 10 del mango %d (%d texels), croma max %d", tag, lmin, lmax, p10, nh, cmax);
            CHECK(nh > 50 && x1 > x0 && y1 > y0, "%s: texels del mango para el color del cuerpo", tag);
            CHECK(lmax <= 28, "%s: cuerpo del cargador mas oscuro que 28/255 (%d)", tag, lmax);
            CHECK(lmax <= p10, "%s: cuerpo del cargador no mas claro que el percentil 10 del mango (%d > %d)", tag, lmax, p10);
            CHECK(lmax == lmin && cmax <= 8, "%s: bloque del cuerpo del cargador uniforme y casi neutro", tag);
        }
    }
    /* extraccion por el eje del mango: el cargador barrido hacia abajo no atraviesa el mango (los puntos que aun estan sobre el
     * suelo del pozo, y = -29 mm, siguen dentro del contorno original) */
    const WPart *mp = def_part(d, "mag");
    CHECK(mp && fabsf(mp->axis.y + 0.939693f) < 1e-4f && fabsf(mp->axis.z - 0.342020f) < 1e-4f, "%s: eje del cargador", tag);
    if (mp) {
        const float L = (mag->bmax.y - mag->bmin.y) / -mp->axis.y;   /* recorrido hasta sacarlo del todo */
        for (int stp = 1; stp <= 3; stp++) {
            float sdist = L * 0.25f * (float)stp, bad = 0;
            for (uint32_t k = 0; k < mag->nv; k += 3) {   /* un vertice por triangulo */
                float p[3] = {mag->v[k].pos[0] + mp->axis.x * sdist, mag->v[k].pos[1] + mp->axis.y * sdist, mag->v[k].pos[2] + mp->axis.z * sdist};
                if (p[1] > -0.0289f && !inside_orig(m, p)) bad++;
            }
            CHECK(bad == 0, "%s: el cargador, extraido %.0f mm, atraviesa el mango (%d vertices)", tag, sdist * 1e3, (int)bad);
        }
    }
}

static void check_model(const uint8_t *d, size_t n, const char *stage, const WCutModel *cm) {
    char tag[32]; snprintf(tag, sizeof tag, "%04x %s", cm->id, cm->tag);
    WLoadOpts o; wload_default_opts(&o); o.resident = stage;
    WLoaded m;
    int r = wload_model(d, n, stage, cm->id, &o, &m);
    CHECK(r == 0, "%s: wload_model", tag);
    if (r) return;
    WCutSet s; r = wcut_run(&m, cm, &s);
    CHECK(r == 0, "%s: wcut_run", tag);
    if (r) { wload_free(&m); return; }
    check_weapon(tag, cm);
    int nnames = 0; while (cm->names && cm->names[nnames]) nnames++;
    CHECK(s.nmeshes >= nnames, "%s: menos meshes que nombres", tag);
    CHECK(s.src_tris == m.ni / 3, "%s: triángulos de origen", tag);
    CHECK(s.atlas && s.atlas_w == m.atlas_w && s.atlas_h == m.atlas_h, "%s: atlas", tag);
    for (int i = 0; i < nnames; i++) CHECK(!strcmp(s.piece[i].name, cm->names[i]), "%s: orden de piezas #%d", tag, i);
    for (int i = 0; i < nnames; i++) {   /* piezas con regla no vacías; huecos sin regla vacíos */
        int has_rule = !strcmp(cm->names[i], cm->body);
        for (int q = 0; q < cm->nrules; q++) if (!strcmp(cm->rules[q].name, cm->names[i])) has_rule = 1;
        if (has_rule) CHECK(s.piece[i].ni >= 3, "%s: pieza '%s' vacía", tag, s.piece[i].name);
        else CHECK(s.piece[i].ni == 0, "%s: hueco '%s' sin regla pero con geometría", tag, s.piece[i].name);
    }
    for (int q = 0; q < cm->nrules; q++) CHECK(wcut_piece(&s, cm->rules[q].name) != NULL, "%s: regla '%s' sin pieza", tag, cm->rules[q].name);
    for (int i = 0; i < s.nmeshes; i++) {   /* meshes coherentes */
        const WMesh *me = &s.meshes[i];
        CHECK(me->nv == me->ni && me->nv % 3 == 0, "%s: mesh %d conteos", tag, i);
        for (uint32_t k = 0; k < me->ni; k++) if (me->idx[k] >= me->nv) { CHECK(0, "%s: mesh %d índice fuera de rango", tag, i); break; }
        for (uint32_t k = 0; k < me->nv; k++) {
            const WVertex *v = &me->v[k];
            if (!isfinite(v->pos[0] + v->pos[1] + v->pos[2]) || v->uv[0] < -1e-4f || v->uv[0] > 1.0001f || v->uv[1] < -1e-4f || v->uv[1] > 1.0001f) { CHECK(0, "%s: mesh %d vértice inválido", tag, i); break; }
        }
    }
    for (int i = 0; i < s.nparts; i++) {
        CHECK(s.parts[i].mesh >= 0 && s.parts[i].mesh < s.nmeshes, "%s: part %d mesh", tag, i);
        CHECK(s.parts[i].parent < s.nparts, "%s: part %d padre", tag, i);
    }
    /* rest.p (acumulado por los padres) + vértice de la mesh = posición original de la pieza */
    for (int i = 0; i < s.nparts; i++) {
        float off[3] = {0, 0, 0}; int p = i, guard = 0;
        while (p >= 0 && guard++ < 8) { off[0] += s.parts[p].rest.p.x; off[1] += s.parts[p].rest.p.y; off[2] += s.parts[p].rest.p.z; p = s.parts[p].parent; }
        const WCutPiece *pp = &s.piece[s.parts[i].mesh]; const WMesh *me = &s.meshes[s.parts[i].mesh];
        int bad = 0;
        for (uint32_t k = 0; k < me->nv && !bad; k++)
            for (int a = 0; a < 3; a++) if (fabsf(me->v[k].pos[a] + off[a] - pp->v[k].pos[a]) > 1e-6f) bad = 1;
        CHECK(!bad, "%s: rest.p + vértice no reproduce el original en '%s'", tag, pp->name);
    }
    /* material de las tapas: UV dentro de alguno de los rectángulos de material de las reglas */
    for (int q = 0; q < cm->nrules; q++) {
        const WCutUV *uvs[5] = {&cm->rules[q].cap_piece, &cm->rules[q].cap_hole, &cm->rules[q].mag.mat_body, &cm->rules[q].mag.mat_case, &cm->rules[q].mag.mat_bullet};
        for (int k = 0; k < (cm->rules[q].mag.on ? 5 : 2); k++) CHECK(uvs[k]->u0 >= 0 && uvs[k]->v0 >= 0 && uvs[k]->u1 <= 1 && uvs[k]->v1 <= 1, "%s: material fuera del atlas", tag);
    }
    for (int i = 0; i < s.npiece; i++) {
        const WCutPiece *p = &s.piece[i];
        uint32_t nt = p->ni / 3;
        CHECK(wcut_degenerate(p) == 0, "%s: '%s' tiene triángulos degenerados", tag, p->name);
        uint32_t open = wcut_open_edges(p), exp_ = open_expected(cm->id, p->name);
        CHECK(open == exp_, "%s: '%s' aristas abiertas distintas de las esperadas", tag, p->name);
        VERB("%s '%s': abiertas %u (esperadas %u), tris %u, vol %.3f cm3", tag, p->name, open, exp_, nt, wcut_volume(p) * 1e6);
        CHECK(p->ncap <= nt, "%s: '%s' ncap", tag, p->name);
        CHECK(wcut_volume(p) >= -1e-9, "%s: '%s' tiene volumen con signo negativo", tag, p->name);
        int badn = 0, badk = 0, baduv = 0;
        for (uint32_t t = 0; t < nt; t++) {
            double nn[3]; tri_n(p, t, nn);
            double dn = 0; for (int c3 = 0; c3 < 3; c3++) for (int q = 0; q < 3; q++) dn += nn[q] * p->v[p->idx[t * 3 + c3]].nrm[q];
            if (dn < 0) { badn++; VERB("   normal mala tri %u kind %d", t, p->kind[t]); }
            if (p->kind[t] > WCUT_TRI_FILL) badk++;
            if (p->kind[t] != WCUT_TRI_SKIN) {
                int inany = 0;
                for (int q = 0; q < cm->nrules && !inany; q++) for (int k = 0; k < 5 && !inany; k++) {
                    const WCutRule *rq = &cm->rules[q];
                    if (k > 1 && !rq->mag.on) break;
                    const WCutUV *u = k == 0 ? &rq->cap_piece : k == 1 ? &rq->cap_hole : k == 2 ? &rq->mag.mat_body : k == 3 ? &rq->mag.mat_case : &rq->mag.mat_bullet; int all = 1;
                    for (int c = 0; c < 3; c++) { const float *uv = p->v[p->idx[t * 3 + c]].uv; if (uv[0] < u->u0 - 1e-5f || uv[0] > u->u1 + 1e-5f || uv[1] < u->v0 - 1e-5f || uv[1] > u->v1 + 1e-5f) all = 0; }
                    inany = all;
                }
                if (!inany) baduv++;
            }
        }
        CHECK(badn == 0, "%s: '%s' tiene normales contrarias al orden de los vértices", tag, p->name);
        CHECK(badk == 0, "%s: '%s' tipo de triángulo inválido", tag, p->name);
        CHECK(baduv == 0, "%s: '%s' tapa con UV fuera de su material", tag, p->name);
        CHECK(!piece_overlaps(p), "%s: '%s' tiene tapas solapadas", tag, p->name);
    }
    /* volumen de la unión = original (menos la geometría generada) */
    WCutModel e0 = *cm; e0.rules = NULL; e0.nrules = 0;
    WCutSet s0;
    if (wcut_run(&m, &e0, &s0) == 0) {
        double v0 = wcut_volume(&s0.piece[0]), v1 = 0;
        for (int i = 0; i < s.npiece; i++) if (!s.piece[i].copy) v1 += wcut_volume(&s.piece[i]) - s.piece[i].gen_vol;
        /* si el original es abierto, el volumen por divergencia depende del origen: se compara contra el cuerpo sin reglas
         * y la tolerancia por modelo es la de VOLEX */
        double tol = vol_tol(cm->id);
        CHECK(fabs(v1 - v0) <= tol * fabs(v0), "%s: volumen de la unión lejos del original", tag);
        VERB("%s: volumen original %.1f cm3, unión %.1f cm3 (%.2f %%)", tag, v0 * 1e6, v1 * 1e6, v0 != 0 ? 100.0 * (v1 - v0) / v0 : 0.0);
        wcut_free(&s0);
    }
    if (!strncmp(cm->tag, "pist", 4)) check_pistol(tag, cm, &m, &s);
    /* id distinto: la tabla de este modelo aplicada a otro -> -4 */
    for (int j = 0; j < wcut_model_count(); j++) {
        const WCutModel *other = wcut_model_at(j);
        if (other->id == cm->id || other->src_tris == cm->src_tris) continue;
        WCutSet sx; int rr = wcut_run(&m, other, &sx);
        CHECK(rr == -4, "%s: tabla de otro modelo aceptada", tag);
        if (rr == 0) wcut_free(&sx);
        break;
    }
    wcut_free(&s); wload_free(&m);
}

/* corrupción aleatoria de un modelo real: no debe colgarse ni fallar (ASan/UBSan en test-san) */
static void fuzz_real(const uint8_t *d, size_t n, const char *stage, const WCutModel *cm) {
    WLoadOpts o; wload_default_opts(&o); o.resident = stage;
    WLoaded m;
    if (wload_model(d, n, stage, cm->id, &o, &m)) return;
    srand(cm->id);
    for (int it = 0; it < 12; it++) {
        WLoaded f = m;
        f.v = (WVertex *)malloc(sizeof(WVertex) * (m.nv + 1)); f.idx = (uint16_t *)malloc(2 * (size_t)(m.ni + 1));
        memcpy(f.v, m.v, sizeof(WVertex) * m.nv); memcpy(f.idx, m.idx, 2 * (size_t)m.ni);
        switch (it % 4) {
        case 0: for (int k = 0; k < 5; k++) f.idx[rand() % m.ni] = (uint16_t)rand(); break;
        case 1: for (int k = 0; k < 5; k++) f.v[rand() % m.nv].pos[rand() % 3] = (rand() % 2) ? NAN : INFINITY; break;
        case 2: for (uint32_t k = 0; k < f.nv; k++) for (int q = 0; q < 3; q++) f.v[k].pos[q] *= (rand() % 3) ? 1.0f : 1e6f; break;
        default: for (int k = 0; k < 20; k++) f.v[rand() % f.nv].pos[rand() % 3] += ((rand() % 2000) - 1000) * 1e-3f; break;
        }
        WCutSet fs; int r = wcut_run(&f, cm, &fs);
        if (r == 0) wcut_free(&fs); else CHECK(fs.piece == NULL && fs.meshes == NULL, "%04x: error sin limpiar", cm->id);
        free(f.v); free(f.idx);
    }
    wload_free(&m);
}

/* parametros absurdos del generador de cargador: rechazo limpio (nunca geometria abierta ni autointersecada) y con la tabla
 * intacta el mismo modelo sigue funcionando */
static void mag_bad_params(const uint8_t *d, size_t n, const char *stage, const WCutModel *cm0) {
    WLoadOpts o; wload_default_opts(&o); o.resident = stage;
    WLoaded m; if (wload_model(d, n, stage, cm0->id, &o, &m)) return;
    WCutRule rules[40]; WCutModel cm = *cm0; int mi = -1;
    if (cm0->nrules > 40) { wload_free(&m); return; }
    memcpy(rules, cm0->rules, sizeof(WCutRule) * (size_t)cm0->nrules); cm.rules = rules;
    for (int i = 0; i < cm.nrules; i++) if (rules[i].mag.on) mi = i;
    if (mi < 0) { wload_free(&m); return; }
    const WCutMag base = rules[mi].mag;
    for (int c = 0; c < 4; c++) {
        WCutMag g = base;
        if (c == 0) g.case_frac = 0.5f;          /* bala mas ancha que la vaina */
        if (c == 1) g.wall_frac = 0.6f;          /* pared mayor que el semiancho: contorno invertido */
        if (c == 2) g.dir.y = 0.1f;              /* eje casi horizontal */
        if (c == 3) g.wall_frac = NAN;
        rules[mi].mag = g;
        WCutSet s; int r = wcut_run(&m, &cm, &s);
        CHECK(r != 0, "%04x: parametros de cargador absurdos (caso %d) aceptados", cm0->id, c);
        if (r == 0) wcut_free(&s);
    }
    rules[mi].mag = base;
    WCutSet s; int r = wcut_run(&m, &cm, &s);
    CHECK(r == 0, "%04x: la tabla original deja de funcionar", cm0->id);
    if (r == 0) wcut_free(&s);
    wload_free(&m);
}

int main(int argc, char **argv) {
    verbose = getenv("WCUT_VERBOSE") != NULL;
    synthetic();
    const char *cands[4] = { argc > 1 ? argv[1] : NULL, getenv("STAGE_FILE"), "gamedata/stages.bin", "../../gamedata/stages.bin" };
    uint8_t *d = NULL; size_t n = 0;
    for (int i = 0; i < 4 && !d; i++) if (cands[i]) d = slurp(cands[i], &n);
    if (!d) printf("test_wcut: sin datos de modelos reales (gamedata/stages.bin ausente): parte real omitida\n");
    else {
        const char *stage = getenv("STAGE_NAME"); if (!stage) stage = "init";
        CHECK(wcut_model_count() == 12, "la tabla no tiene 12 modelos");
        for (int i = 0; i < wcut_model_count(); i++) check_model(d, n, stage, wcut_model_at(i));
        for (int i = 0; i < wcut_model_count(); i++) fuzz_real(d, n, stage, wcut_model_at(i));
        mag_bad_params(d, n, stage, wcut_find(0x21b6));
        CHECK(wcut_find(0x0000) == NULL, "id inexistente");
        free(d);
    }
    printf(fails ? "test_wcut: FALLO (%d)\n" : "test_wcut: OK\n", fails);
    return fails ? 1 : 0;
}
