/* nsdesc: descartes de triángulo ENTERO del render nativo con la cámara del motor (pantalla 3D), medidos fuera de la app con las réplicas en C del sombreador y el rasterizador de referencia.
 * Uso: nsdesc DIR_VOLCADO [--escenario]      (por defecto toda la escena, objetos dinámicos incluidos; --escenario: solo el escenario)
 * Barre la cámara por una trayectoria sintética alrededor de la cámara real del volcado (giros de hasta 30 grados, traslaciones de hasta 40 cm) y, en cada pose, dibuja una imagen 1,5 veces la
 * ventana del motor desde el ojo del motor y desde dos ojos a +-33 unidades (estéreo de 66 mm):
 *   ideal    todos los triángulos con recorte real por el plano cercano; las caras de una sola cara, solo por su anverso desde ESE ojo;
 *   antes    la regla del motor por triángulo entero con su ojo (ns_gpu_gte_tri: ajuste al plano H/2, sin vértice delante, más de 1023 x 511 px; reverso con los vértices ajustados);
 *   despues  ns_gpu_engine_tri: lo mismo, más el rescate de lo que el motor descarta sin ser cercano (con el ojo de la vista), dibujado solo donde su proyección con el ojo del motor cae fuera
 *            de la ventana (se compone por píxel con la superficie rescatada más cercana: una rescatada tapada por otra que se recorta no se cuenta).
 * DENTRO de la ventana del motor (+-160 x +-120 px del centro) la verdad es el fotograma del juego: ahí «despues» debe ser idéntico a «antes» desde el ojo del motor (dentro_distinto = 0) y se
 * compara con la lista de la GPU del volcado en la pose real (ns_wincmp: tapa / de más / falta). FUERA de la ventana y en los otros ojos no hay verdad del juego: se cuentan los píxeles que el
 * ideal cubre y cada regla deja al descubierto (campo extra) y, en la ventana de los otros ojos, los que «despues» tapa por delante de «antes» (barrido: regla «Recortadas», sin subdivisión).
 * Línea [nsdesc-original] (pose real, ojo del motor, ventana de 160 x 112 celdas): el modo «Caras cercanas: Original» frente a la REFERENCIA del motor original, la réplica exacta ns_div con el
 * umbral 0x800 (caras enteras y trozos que entregaría a la tarjeta, sin los de más de 1023 x 511 px ni los que tienen alguna esquina con sz = 0), resuelta por profundidad real (búfer z):
 * de_mas / de_menos / otra_superficie (profundidad a más de 1,3 veces) en % de la ventana; lo mismo para «Recortadas»; cuánto difiere cada modo de la lista del juego parcheado; y
 * orden_OT = celdas donde el orden de la tabla de ordenación del motor (por la profundidad media de cada cara o trozo menos su sesgo, de lejos a cerca) deja delante otra superficie que el
 * búfer z: ahí el original podría verse distinto.
 * Línea [nsdesc-paralaje] (pose real, escena entera): por modo, lo más cercano que la regla dibuja en la ventana (celdas de ns_winmask_native), la cota que usa la pantalla 3D
 * (ns_solid_view_near) y el paralaje cruzado que resulta con la ventana estéreo de la app (ns_cinema_depth + ns_cinema_stereo sin filtro temporal; IPD 64 mm, pantalla de 3 m a 2,5 m) frente
 * al límite NS_CINEMA_PAR_NEG. Sale con error si alguno lo supera. NO ejecuta el juego. */
#include "../ns_adapt.h"
#include "../ns_cover.h"
#include "../ns_cpuref.h"
#include "../ns_div.h"
#include "../ns_dump.h"
#include "../ns_extract.h"
#include "../ns_gpu.h"
#include "../ns_solid.h"
#include "../ns_cinema.h"
typedef char nsdesc_near[(int)NS_SOLID_SCREEN_NEAR == (int)NS_WIN_NEAR ? 1 : -1];
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { W = 480, HH = 360 };
/* referencia del motor original: primitivas de ns_div en pantalla */
typedef struct { float s[4][3]; float key; } RefPrim;
static RefPrim *g_ref; static size_t g_nref, g_capref; static unsigned g_ref_big, g_ref_z0;
static void ref_emit(void *u, const NsDivPrim *p) {
    (void)u;
    int mnx = 32767, mxx = -32768, mny = 32767, mxy = -32768, z0 = 0;
    for (int k = 0; k < 4; k++) { if (p->sx[k] < mnx) mnx = p->sx[k]; if (p->sx[k] > mxx) mxx = p->sx[k]; if (p->sy[k] < mny) mny = p->sy[k]; if (p->sy[k] > mxy) mxy = p->sy[k]; z0 |= p->sz[k] == 0; }
    if (mxx - mnx > 1023 || mxy - mny > 511) { g_ref_big++; return; }
    if (z0) { g_ref_z0++; return; }
    if (g_nref == g_capref) { g_capref = g_capref ? g_capref * 2 : 4096; g_ref = (RefPrim *)realloc(g_ref, g_capref * sizeof *g_ref); }
    RefPrim *r = &g_ref[g_nref++];
    for (int k = 0; k < 4; k++) { r->s[k][0] = p->sx[k]; r->s[k][1] = p->sy[k]; r->s[k][2] = 1.0f / (float)p->sz[k]; }
    r->key = 0.5f * ((float)p->sz[0] + (float)p->sz[3]) - (float)p->raise;
}
static int ref_cmp(const void *a, const void *b) { const float x = ((const RefPrim *)a)->key, y = ((const RefPrim *)b)->key; return x > y ? -1 : x < y; }
static void mask_dif(const float *a, const float *b, double pct[3]) {   /* a frente a b: de más, de menos, otra superficie (% de la ventana) */
    unsigned m = 0, l = 0, o = 0;
    for (int i = 0; i < NS_WIN_W * NS_WIN_H; i++) {
        if (a[i] > 0 && !(b[i] > 0)) m++; else if (!(a[i] > 0) && b[i] > 0) l++; else if (a[i] > 0 && (a[i] > 1.3f * b[i] || b[i] > 1.3f * a[i])) o++;
    }
    pct[0] = 100.0 * m / (NS_WIN_W * NS_WIN_H); pct[1] = 100.0 * l / (NS_WIN_W * NS_WIN_H); pct[2] = 100.0 * o / (NS_WIN_W * NS_WIN_H);
}
typedef struct { float eye[3], x[3], y[3], f[3]; } Pose;

static void rot(float *a, float *b, float deg) {   /* gira el par de ejes (a, b) `deg` grados en su plano */
    const float r = deg * 3.14159265f / 180.0f, c = cosf(r), s = sinf(r);
    for (int k = 0; k < 3; k++) { const float u = a[k], v = b[k]; a[k] = c * u + s * v; b[k] = -s * u + c * v; }
}
static void render(const NsRTri *t, size_t n, const Pose *p, const float er[3], float h, float ys, NsRBuf *out) {
    NsRScene sc = {t, n, NULL};
    NsRCam c; memset(&c, 0, sizeof c);
    for (int k = 0; k < 3; k++) { c.A[k] = p->x[k]; c.A[3 + k] = p->y[k]; c.A[6 + k] = p->f[k]; }
    for (int r = 0; r < 3; r++) c.t[r] = -(c.A[3 * r] * er[0] + c.A[3 * r + 1] * er[1] + c.A[3 * r + 2] * er[2]);
    c.fx = h; c.fy = h * ys; c.cx = 0.5f * W; c.cy = 0.5f * HH; c.w = W; c.h = HH; c.znear = 50.0f;
    ns_buf_clear(out, 0, 0, 0);
    ns_render_view(&sc, &c, NS_O_1THREAD, out, NULL);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: nsdesc DIR_VOLCADO [--escenario]\n"); return 2; }
    int rc_par = 0;
    const int all = !(argc > 2 && !strcmp(argv[2], "--escenario"));
    NsDump *d = ns_dump_open(argv[1]);
    if (!d) { fprintf(stderr, "no se pudo abrir %s\n", argv[1]); return 1; }
    NsScene sc; memset(&sc, 0, sizeof sc);
    NsMeshStore *st = ns_store_create();
    if (!st || ns_scene_alloc(&sc, 512, 8192, 256, 0) || ns_extract_ex(&sc, st, d->ram, NS_RAM_SIZE, d->scratch, 1024u, NULL, NULL)) { fprintf(stderr, "sin escena\n"); return 1; }
    NsAdaptOpts ao; memset(&ao, 0, sizeof ao);
    NsAdaptSrc *src = NULL; NsRTri *tr = NULL; NsAdaptStats as;
    ao.src_out = &src; ao.no_tint = 1; ao.no_shade = 1; ao.exposure = 1.0f;
    const size_t n0 = ns_adapt_scene(&sc, st, &ao, &tr, &as);
    const float h = sc.cam.h, ys = sc.cam.overscan_y > 0 ? sc.cam.overscan_y : 1.0f;
    if (!n0 || !(h > 8)) { fprintf(stderr, "sin triangulos o sin camara\n"); return 1; }
    /* triángulos en el orden del sombreador (la segunda mitad de un cuadrilátero como (B,C,D)), opacos y sin textura */
    size_t n = 0;
    NsRTri *t = (NsRTri *)malloc(n0 * sizeof *t), *ti = (NsRTri *)malloc(n0 * sizeof *t), *tb = (NsRTri *)malloc(n0 * sizeof *t), *ta = (NsRTri *)malloc(n0 * sizeof *t), *tx = (NsRTri *)malloc(n0 * sizeof *t);
    uint8_t *one = (uint8_t *)malloc(n0), *stat = (uint8_t *)malloc(n0);
    for (size_t i = 0; i < n0; i++) {
        if (!all && !(src[i].iflags & NS_IF_STATIC)) continue;
        t[n] = tr[i]; t[n].flags = 0;
        if (src[i].half) { const NsRVert v = t[n].v[1]; t[n].v[1] = t[n].v[2]; t[n].v[2] = v; }
        stat[n] = !(src[i].iflags & NS_IF_GBOUND);   /* el motor original parte sus caras */
        one[n++] = !(src[i].mflags & NS_MF_TWOSIDED);
    }
    Pose base;
    for (int k = 0; k < 3; k++) { base.eye[k] = sc.cam.eye[k]; base.x[k] = sc.cam.rot[3 * k]; base.y[k] = sc.cam.rot[3 * k + 1]; base.f[k] = sc.cam.rot[3 * k + 2]; }
    static const float SW[][5] = {   /* guiñada, cabeceo (grados), derecha, abajo, delante (unidades del motor) */
        {0, 0, 0, 0, 0}, {3, 0, 0, 0, 0}, {-3, 0, 0, 0, 0}, {8, 0, 0, 0, 0}, {-8, 0, 0, 0, 0}, {15, 0, 0, 0, 0}, {-15, 0, 0, 0, 0}, {30, 0, 0, 0, 0}, {-30, 0, 0, 0, 0},
        {0, 5, 0, 0, 0}, {0, -5, 0, 0, 0}, {0, 12, 0, 0, 0}, {0, -12, 0, 0, 0}, {0, 0, 150, 0, 0}, {0, 0, -150, 0, 0}, {0, 0, 400, 0, 0}, {0, 0, -400, 0, 0},
        {0, 0, 0, 0, 150}, {0, 0, 0, 0, -150}, {0, 0, 0, 0, 400}, {0, 0, 0, 0, -400}, {0, 0, 0, 150, 0}, {0, 0, 0, -150, 0}, {8, 5, 150, 0, 150}, {-8, -5, -150, 0, -150}};
    const int np = (int)(sizeof SW / sizeof SW[0]);
    NsRBuf bi, bb, ba, bx;
    if (ns_buf_alloc(&bi, W, HH) || ns_buf_alloc(&bb, W, HH) || ns_buf_alloc(&ba, W, HH) || ns_buf_alloc(&bx, W, HH)) return 1;
    unsigned long long x_ideal = 0, x_unc_b = 0, x_unc_a = 0, x_more_a = 0;      /* campo extra (fuera de la ventana, los tres ojos): ideal / al descubierto antes / después / después pinta donde el ideal no */
    unsigned long long w_dif = 0, w_px = 0;                                        /* ventana, ojo del motor: píxeles / con cobertura o superficie distinta entre antes y después */
    unsigned long long l_px = 0, l_unc_b = 0, l_unc_a = 0, l_front = 0;            /* ventana de los otros ojos: ideal / al descubierto antes / después / después por delante de antes */
    unsigned long long cb[3] = {0, 0, 0}, ca[4] = {0, 0, 0, 0}, sent = 0;          /* triángulos por causa (detrás, tamaño, reverso; después además rescatados), sumados en las poses, ojo del motor */
    for (int pi = 0; pi < np; pi++) {
        Pose p = base;
        rot(p.x, p.f, SW[pi][0]);     /* guiñada: gira derecha y delante */
        rot(p.f, p.y, -SW[pi][1]);    /* cabeceo: gira delante y abajo */
        for (int k = 0; k < 3; k++) p.eye[k] += SW[pi][2] * p.x[k] + SW[pi][3] * p.y[k] + SW[pi][4] * p.f[k];
        NsGpuEngCam ec;
        for (int k = 0; k < 3; k++) { ec.eye[k] = p.eye[k]; ec.x[k] = p.x[k]; ec.y[k] = p.y[k]; ec.fwd[k] = p.f[k]; }
        ec.h = h; ec.hx = h; ec.hy = h * ys;
        for (int e = 0; e < 3; e++) {
            const float off = e == 0 ? 0.0f : e == 1 ? 33.0f : -33.0f;
            float er[1][3];
            for (int k = 0; k < 3; k++) er[0][k] = p.eye[k] + off * p.x[k];
            size_t ni = 0, nb = 0, na = 0, nx = 0;
            for (size_t i = 0; i < n; i++) {
                float w[3][3], v[3][3], g[3][3], gv[3][3];
                for (int k = 0; k < 3; k++) {
                    memcpy(w[k], t[i].v[k].p, 12);
                    const float r[3] = {w[k][0] - p.eye[0], w[k][1] - p.eye[1], w[k][2] - p.eye[2]};
                    v[k][0] = p.x[0] * r[0] + p.x[1] * r[1] + p.x[2] * r[2]; v[k][1] = p.y[0] * r[0] + p.y[1] * r[1] + p.y[2] * r[2]; v[k][2] = p.f[0] * r[0] + p.f[1] * r[1] + p.f[2] * r[2];
                }
                if (!(one[i] && ns_gpu_tri_back(w[0], w[1], w[2], er[0]))) ti[ni++] = t[i];
                /* antes: la regla del motor */
                const int rb = ns_gpu_gte_tri((const float (*)[3])v, 0.5f * h, ec.hx, ec.hy, gv);
                int drop = rb != NSG_GTE_DRAW, why = rb == NSG_GTE_BEHIND ? 0 : 1;
                if (!drop) {
                    for (int k = 0; k < 3; k++) for (int a = 0; a < 3; a++) g[k][a] = w[k][a] + p.f[a] * (gv[k][2] - v[k][2]);
                    if (one[i] && ns_gpu_tri_back(g[0], g[1], g[2], p.eye)) { drop = 1; why = 2; }
                }
                if (drop) { if (e == 0) cb[why]++; }
                else { tb[nb] = t[i]; for (int k = 0; k < 3; k++) memcpy(tb[nb].v[k].p, g[k], 12); nb++; }
                /* después: la regla del motor y el rescate con el ojo de la vista */
                const int ra = ns_gpu_engine_tri((const float (*)[3])w, &ec, one[i], 0, (const float (*)[3])er, 1, NSG_ENG_OFF_SPLIT, g);
                if (ra == NSG_ENG_DRAW) { ta[na] = t[i]; for (int k = 0; k < 3; k++) memcpy(ta[na].v[k].p, g[k], 12); na++; }
                else if (ra == NSG_ENG_EXTRA) tx[nx++] = t[i];
                if (e == 0 && ra != NSG_ENG_DRAW) ca[ra - 1]++;
            }
            if (e == 0) sent += n;
            render(ti, ni, &p, er[0], h, ys, &bi); render(tb, nb, &p, er[0], h, ys, &bb); render(ta, na, &p, er[0], h, ys, &ba); render(tx, nx, &p, er[0], h, ys, &bx);
            for (int y = 0; y < HH; y++) for (int x = 0; x < W; x++) {
                const int o = y * W + x;
                if (bx.iz[o] > ba.iz[o]) {   /* rescatado visible: solo si su proyección con el ojo del motor cae fuera de la ventana (el ojo de la vista está `off` a la derecha: x_motor = x + H off / z) */
                    const float sxe = (float)x + 0.5f - 0.5f * W + h * off * bx.iz[o], sye = (float)y + 0.5f - 0.5f * HH;
                    if (!(fabsf(sxe) <= NSG_ENG_WIN_X && fabsf(sye) <= NSG_ENG_WIN_Y)) ba.iz[o] = bx.iz[o];
                }
                const int I = bi.iz[o] > 0, B = bb.iz[o] > 0, A = ba.iz[o] > 0;
                const int in_win = fabsf((float)x + 0.5f - 0.5f * W) <= NSG_ENG_WIN_X && fabsf((float)y + 0.5f - 0.5f * HH) <= 120.0f;
                if (!in_win) { x_ideal += (unsigned)I; x_unc_b += (unsigned)(I && !B); x_unc_a += (unsigned)(I && !A); x_more_a += (unsigned)(A && !I); }
                else if (e == 0) { w_px++; w_dif += (unsigned)(A != B || (A && fabsf(ba.iz[o] - bb.iz[o]) > 0.01f * bb.iz[o])); }
                else { l_px += (unsigned)I; l_unc_b += (unsigned)(I && !B); l_unc_a += (unsigned)(I && !A); l_front += (unsigned)(A && B && ba.iz[o] > 1.3f * bb.iz[o]); }
            }
        }
    }
    NsWinCmp wc; memset(&wc, 0, sizeof wc);   /* pose real, ojo del motor, ventana del motor: nativo (regla nueva) frente a la lista del volcado */
    if (d->has_list) {
        NsGpuEngCam ec;
        for (int k = 0; k < 3; k++) { ec.eye[k] = base.eye[k]; ec.x[k] = base.x[k]; ec.y[k] = base.y[k]; ec.fwd[k] = base.f[k]; }
        ec.h = h; ec.hx = h; ec.hy = h * ys;
        NsAdaptSrc *s2 = (NsAdaptSrc *)calloc(n, sizeof *s2);   /* t ya va en el orden del sombreador */
        for (size_t i = 0; s2 && i < n; i++) { s2[i].mflags = one[i] ? 0u : NS_MF_TWOSIDED; s2[i].iflags = stat[i] ? 0u : NS_IF_GBOUND; }
        if (s2) ns_wincmp(t, s2, n, &ec, NSG_ENG_OFF_SPLIT, d->list.ops, d->list.nops, &wc);
        /* «Original» y «Recortadas» frente a la referencia del motor original (ns_div con el umbral 0x800) y frente a la lista del juego parcheado */
        NsDivCfg dc; memset(&dc, 0, sizeof dc);
        dc.pool = -1; dc.n_packs_override = -1; dc.div_thresh = 0x800u;
        NsDivStats ds;
        const int nd = ns_div_run(d->ram, NS_DUMP_RAM_SIZE, &dc, NULL, ref_emit, NULL, &ds);
        const size_t cells = (size_t)NS_WIN_W * NS_WIN_H;
        float *m = (float *)calloc(6 * cells, sizeof(float));
        if (s2 && m && nd >= 0) {
            float *ref = m, *ot = m + cells, *org = m + 2 * cells, *spl = m + 3 * cells, *cut = m + 4 * cells, *lst = m + 5 * cells;
            for (size_t i = 0; i < g_nref; i++) for (int h2 = 0; h2 < 2; h2++) { float s3[3][3]; memcpy(s3, g_ref[i].s[h2], sizeof s3); ns_winmask_tri(ref, (const float (*)[3])s3); }
            qsort(g_ref, g_nref, sizeof *g_ref, ref_cmp);   /* orden de la tabla de ordenación: de lejos a cerca; el último que pinta gana */
            float *one_p = (float *)calloc(cells, sizeof(float));
            for (size_t i = 0; one_p && i < g_nref; i++) {
                memset(one_p, 0, cells * sizeof(float));
                for (int h2 = 0; h2 < 2; h2++) { float s3[3][3]; memcpy(s3, g_ref[i].s[h2], sizeof s3); ns_winmask_tri(one_p, (const float (*)[3])s3); }
                for (size_t c2 = 0; c2 < cells; c2++) if (one_p[c2] > 0) ot[c2] = one_p[c2];
            }
            free(one_p);
            ns_winmask_native(t, s2, n, &ec, 0, org, spl);
            for (size_t c2 = 0; c2 < cells; c2++) if (spl[c2] > org[c2]) org[c2] = spl[c2];
            ns_winmask_native(t, s2, n, &ec, NSG_ENG_OFF_SPLIT, cut, NULL);
            ns_winmask_list(d->list.ops, d->list.nops, h, lst);
            double a1[3], a2[3], a3[3], a4[3], a5[3]; unsigned nsp = 0;
            mask_dif(org, ref, a1); mask_dif(cut, ref, a2); mask_dif(org, lst, a3); mask_dif(cut, lst, a4); mask_dif(ot, ref, a5);
            for (size_t c2 = 0; c2 < cells; c2++) nsp += spl[c2] > 0 && spl[c2] >= org[c2];
            printf("[nsdesc-original] referencia ns_div: prims=%zu caras_partidas=%u trozos=%u (sin dibujar: >1023x511=%u, con sz=0=%u) | ORIGINAL vs referencia: de_mas=%.1f%% de_menos=%.1f%% otra_superficie=%.1f%% | "
                   "RECORTADAS vs referencia: de_mas=%.1f%% de_menos=%.1f%% otra_superficie=%.1f%% | ORIGINAL vs lista parcheada: de_mas=%.1f%% de_menos=%.1f%% otra_superficie=%.1f%% (caras partidas visibles=%.1f%%) | "
                   "RECORTADAS vs lista parcheada: de_mas=%.1f%% de_menos=%.1f%% otra_superficie=%.1f%% | orden_OT distinto del bufer z en la referencia=%.1f%%\n",
                   g_nref, ds.f_split, ds.sub_emitted, g_ref_big, g_ref_z0, a1[0], a1[1], a1[2], a2[0], a2[1], a2[2], a3[0], a3[1], a3[2], 100.0 * nsp / (double)cells, a4[0], a4[1], a4[2], a5[2]);
        }
        if (s2 && m && nd >= 0 && all) {
            const float *org = m + 2 * cells, *cut = m + 4 * cells;
            printf("[nsdesc-paralaje] H=%.0f", h);
            for (int mode = 0; mode < 2; mode++) {
                float iz = 0.0f, zn = 0.0f, zs = 0.0f;
                for (size_t c2 = 0; c2 < cells; c2++) { const float v = mode ? cut[c2] : org[c2]; if (v > iz) iz = v; }
                ns_gpu_engine_set_off(mode ? NSG_ENG_OFF_SPLIT : 0);
                const float bound = ns_solid_view_near(&sc, st);
                ns_gpu_engine_set_off(0);
                const int dyn = ns_cinema_depth(&sc, &zn, &zs);
                if (bound > 0.0f && (!dyn || 0.001f * bound < zn)) zn = 0.001f * bound;      /* como ns_cinema_depth_filter en un corte */
                if (!dyn) zs = NS_CINEMA_CONV_DEFAULT;
                NsCinemaStereo cs; memset(&cs, 0, sizeof cs);
                const float M = (3.0f / (2.0f * 2.5f)) / (160.0f / h);
                const int ok = ns_cinema_stereo(0.064f, zn, zs, 160.0f / h, 3.0f / (2.0f * 2.5f), 2.5f, &cs);
                const float par = ok && iz > 0.0f ? M * cs.b * (1000.0f * iz - 1.0f / cs.conv) * 57.29578f : 0.0f;   /* grados de paralaje cruzado de lo más cercano dibujado */
                printf(" | %s: cercano_dibujado=%.0f u cota=%.0f u separacion=%.1f mm paralaje=%.2f grados (limite %.2f)%s", mode ? "RECORTADAS" : "ORIGINAL", iz > 0.0f ? 1.0f / iz : 0.0f, bound, 1000.0f * cs.b, par,
                       NS_CINEMA_PAR_NEG * 57.29578f, par > NS_CINEMA_PAR_NEG * 57.29578f * 1.001f ? " SUPERADO" : "");
                if (par > NS_CINEMA_PAR_NEG * 57.29578f * 1.001f) rc_par = 1;
            }
            printf("\n");
        }
        free(m);
        free(s2);
    }
    const double xi = x_ideal ? 100.0 / (double)x_ideal : 0.0, li = l_px ? 100.0 / (double)l_px : 0.0, wi = wc.cells ? 100.0 / wc.cells : 0.0;
    printf("[nsdesc] %s H=%.0f tris=%zu poses=%d | campo_extra: px_ideal=%llu al_descubierto antes=%llu (%.2f%%) despues=%llu (%.2f%%) despues_de_mas=%llu | ventana_otros_ojos: px_ideal=%llu al_descubierto antes=%llu (%.2f%%) despues=%llu (%.2f%%) despues_tapa=%llu (%.3f%%) | "
           "ventana_ojo_motor: dentro_distinto=%llu de %llu | lista (pose real): tapa=%.1f%% de_mas=%.1f%% falta=%.1f%% | "
           "tris por pose (de %llu): antes detras=%.0f tamano=%.0f reverso=%.0f; despues detras=%.0f tamano=%.0f reverso=%.0f rescatados=%.0f\n",
           all ? "todo" : "escenario", h, n, np, x_ideal, x_unc_b, xi * (double)x_unc_b, x_unc_a, xi * (double)x_unc_a, x_more_a, l_px, l_unc_b, li * (double)l_unc_b, l_unc_a, li * (double)l_unc_a, l_front, li * (double)l_front,
           w_dif, w_px, wi * wc.cover, wi * wc.extra, wi * wc.missing,
           sent / (unsigned long long)np, (double)cb[0] / np, (double)cb[1] / np, (double)cb[2] / np, (double)ca[0] / np, (double)ca[1] / np, (double)ca[2] / np, (double)ca[3] / np);
    return rc_par;
}
