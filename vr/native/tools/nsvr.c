/* nsvr: réplica de escritorio de la PRESENTACIÓN del renderer nativo (NativeRenderer.swift + NativeShaders.metal) sobre un volcado NSD1. nshito1 dibuja la escena con el rasterizador de
 * referencia (ns_adapt + ns_cpuref), que NO pasa por el empaquetado de la GPU; aquí se sigue el camino de la app (ns_gpu_pack, ns_gpu_make_piece, reglas de ingest y de ns_vertex / ns_fs) para MEDIR
 * lo que la tubería presenta, regla por regla. Con cámara VR en el volcado (rec_active): el mundo en primera persona desde la cabeza real del usuario (rec_head), doble cara. Sin ella (o con
 * --pose game): la pantalla 3D con la cámara del motor y sus reglas (descarte de reversos, descarte cercano y tamaño máximo del hardware). Mide:
 *   - piezas que la app no dibuja (ocultas, fuera del grupo activo) y si retendría la escena por «mallas ausentes» (ns_gpu_scene_misses);
 *   - sesgo de orden (`raise`): píxeles cuya superficie visible cambia al aplicarlo, separados en «coplanares» (calcas: lo que el sesgo debe resolver) y «a través» (la superficie que gana estaba
 *     a más de --sep unidades por detrás: oclusión rota);
 *   - superficies coplanares que empatan en profundidad (parpadeo), reversos visibles de caras de una sola cara y triángulos que recorta el plano cercano;
 *   - con --list: cobertura de la ventana de la pantalla del motor frente a la lista de la GPU (lo que el motor dibujó de verdad): lo que el nativo tapa, pone de más o deja sin dibujar.
 * Salidas: <nombre>_vr.png (lo que presenta la app), <nombre>_vr_ref.png (la misma geometría sin sesgos) y <nombre>_vr_dif.png (rojo = a través, verde = coplanar resuelto por el sesgo,
 * azul = coplanares que empatan, gris = reverso visible de una cara de una sola cara). No ejecuta el juego ni forma parte de la app.
 * Límites: solo las mallas (los efectos de NsScene.effects no se dibujan), un ojo, cuerpo propio con la pose del juego (sin IK ni manos propias) y la VRAM del volcado.
 *
 * Uso: nsvr --dump DIR --out DIR --name N [--list DIR_FRAME] [--pose head|game] [--look guiñada,cabeceo (grados, sobre la cabeza)] [--size n (640)] [--tan t (1.2)] [--near m (0.1)]
 *           [--sep u (20)] [--gain g (3)] [--old-back] [--old-hw] (como la build 0d4635f: reversos que empatan / sin el tamaño máximo del hardware) [--old-rule (regla del motor por triángulo entero, sin el rescate del campo extra)] [--recortadas (sin la subdivisión del motor original)]
 *           diagnóstico: [--all-groups (como si el motor tuviera activos todos los grupos de mapa)] [--skip pieza,pieza (no dibujarlas)] [--probe x,y (pieza visible en ese píxel)] */
#include "../ns_cpuref.h"
#include "../ns_dump.h"
#include "../ns_extract.h"
#include "../ns_fx.h"
#include "../ns_gpu.h"
#include "../ns_player.h"
#include "../ns_sink.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float d, x, y; float u, v, r, g, b, du; } CV;   /* vértice de recorte: d = profundidad de vista (m), x/y = vista; du = profundidad SIN sesgo (m) */
typedef struct {
    int w, h; float tanh, znear;
    float eye[3], R[9];             /* ojo (mundo VR) y ejes derecha/arriba/atrás de la cabeza en filas */
    float *iz;                      /* 1/d del más cercano (0 = vacío) */
    float *du;                      /* profundidad sin sesgo del visible */
    int32_t *pid; int32_t *tid;     /* pieza y triángulo absoluto del visible (-1) */
    float *iz2; int32_t *pid2;      /* segundo más cercano de otra pieza, o de la misma si los dos triángulos no comparten arista (coplanares) */
    uint8_t *bk;                    /* 1 = el visible es el reverso de una cara de una sola cara */
    uint8_t *cov;                   /* 1 = algún fragmento escrito (opaco o semitransparente) */
    float *rgb;                     /* 0..1 */
} Buf;

typedef struct { const NsMesh *m; const NsBaked *bk; uint32_t vbase, tbase, ntri; int semi, textured; } GMesh;

static const uint16_t *g_vram;
static NsGpuVert *g_verts; static NsGpuCorner *g_corners; static NsGpuPiece *g_tbl;
static float g_tovr[12], g_mpu, g_raise_m;
static int g_old_back;   /* --old-back: como la build 0d4635f (los reversos empatan con los anversos) */
static int g_cull_all, g_no_hw; static float g_zreject, g_cam_fwd[4], g_cam_x[4], g_cam_y[4];   /* pantalla 3D (cámara del motor): descarte de reversos y descarte cercano del motor (cullBack de NativeRenderer) */
static float g_det_sign = 1.0f, g_hpx, g_hpy;
static int g_wincut;      /* el triángulo en curso es un rescatado (NSG_ENG_EXTRA): raster no lo dibuja dentro de la ventana del motor (ns_fs, NSG_CF_WINCUT); solo con la vista del motor */
static int g_no_split;    /* --recortadas: «Caras cercanas: Recortadas» (sin la subdivisión del motor original) */
static int g_old_rule;   /* --old-rule: regla del sombreador anterior a ns_gpu_engine_tri */
static int g_old_reject, g_nocull; static uint32_t g_skipflag; static int32_t *g_opmap; static int g_opcur;   /* diagnóstico --miss: operación de la lista que escribió cada píxel de L */

static void buf_alloc(Buf *b, int n) {
    b->w = b->h = n;
    size_t px = (size_t)n * n;
    b->iz = calloc(px, sizeof(float)); b->du = calloc(px, sizeof(float)); b->iz2 = calloc(px, sizeof(float));
    b->pid = malloc(px * sizeof(int32_t)); b->tid = malloc(px * sizeof(int32_t)); b->pid2 = malloc(px * sizeof(int32_t));
    b->rgb = calloc(px * 3, sizeof(float));
    b->bk = calloc(px, 1); b->cov = calloc(px, 1);
    for (size_t i = 0; i < px; i++) b->pid[i] = b->tid[i] = b->pid2[i] = -1;
}
static void buf_free(Buf *b) { free(b->iz); free(b->du); free(b->iz2); free(b->pid); free(b->tid); free(b->pid2); free(b->rgb); free(b->bk); free(b->cov); }

/* Color de una esquina como ns_vertex: precalculado, luz de la pieza o neutro; después niebla. */
static void corner_col(const NsScene *sc, const NsGpuCorner *c, const NsGpuPiece *P, const float wp[3], float col[3]) {
    float raw[3];
    if (c->flags & NSG_CF_PAINT) { raw[0] = (float)(c->paint & 255u); raw[1] = (float)((c->paint >> 8) & 255u); raw[2] = (float)((c->paint >> 16) & 255u); }
    else if (P->flags & NSG_PF_SHADE) {
        float I[3];
        for (int l = 0; l < 3; l++) { I[l] = P->l[3 * l] * c->n[0] + P->l[3 * l + 1] * c->n[1] + P->l[3 * l + 2] * c->n[2]; I[l] = I[l] < 0 ? 0 : I[l] > NSG_INTENSITY_MAX ? NSG_INTENSITY_MAX : I[l]; }
        for (int k = 0; k < 3; k++) raw[k] = P->k[4 * k + 3] + P->k[4 * k] * I[0] + P->k[4 * k + 1] * I[1] + P->k[4 * k + 2] * I[2];
    } else raw[0] = raw[1] = raw[2] = 128.0f;
    float s = P->fog;
    if (P->flags & NSG_PF_VFOG) s = 1.0f - ns_gpu_fog_f(&sc->fog, ns_gpu_game_z(&sc->cam, wp));
    for (int k = 0; k < 3; k++) { float v = raw[k] < 0 ? 0 : raw[k] > 255 ? 255 : raw[k]; col[k] = floorf(v * s); }
}

/* Recorte contra el plano cercano (d >= znear). Devuelve el nº de vértices del polígono (0, 3 o 4). */
static int clip_near(const CV in[3], float zn, CV out[4]) {
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const CV *a = &in[i], *b = &in[(i + 1) % 3];
        const int ia = a->d >= zn, ib = b->d >= zn;
        if (ia) out[n++] = *a;
        if (ia != ib) {
            const float t = (zn - a->d) / (b->d - a->d);
            CV *o = &out[n++];
            const float *pa = &a->d, *pb = &b->d; float *po = &o->d;
            for (int k = 0; k < (int)(sizeof(CV) / sizeof(float)); k++) po[k] = pa[k] + (pb[k] - pa[k]) * t;
        }
    }
    return n;
}

enum { PASS_OPAQUE = 0, PASS_SEMI = 1 };
/* ¿Pueden solaparse de verdad los triángulos a y b? No si son el mismo o comparten arista (dos vértices): ahí solo coinciden en el borde. */
static int tri_apart(int a, int b) {
    if (a < 0 || b < 0) return 0;
    if (a == b) return 0;
    int sh = 0;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) sh += g_corners[3 * a + i].v == g_corners[3 * b + j].v;
    return sh < 2;
}
static void raster(Buf *b, const CV t[3], const NsGpuCorner *c0, int pass, int gi, int tri, int back) {
    const float f = 0.5f * (float)b->w / b->tanh, cx = 0.5f * (float)b->w, cy = 0.5f * (float)b->h;
    float sx[3], sy[3], iw[3];
    for (int i = 0; i < 3; i++) { iw[i] = 1.0f / t[i].d; sx[i] = cx + f * t[i].x * iw[i]; sy[i] = cy - f * t[i].y * iw[i]; }
    const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
    if (!(fabsf(area) > 1e-9f)) return;
    float mnx = fminf(sx[0], fminf(sx[1], sx[2])), mxx = fmaxf(sx[0], fmaxf(sx[1], sx[2])), mny = fminf(sy[0], fminf(sy[1], sy[2])), mxy = fmaxf(sy[0], fmaxf(sy[1], sy[2]));
    int x0 = (int)floorf(mnx), x1 = (int)ceilf(mxx), y0 = (int)floorf(mny), y1 = (int)ceilf(mxy);
    x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0; x1 = x1 > b->w - 1 ? b->w - 1 : x1; y1 = y1 > b->h - 1 ? b->h - 1 : y1;
    const uint32_t cf = c0->flags;
    const int semi = (cf & NSG_CF_SEMI) != 0, tex = (cf & NSG_CF_TEX) != 0, abr = (int)NSG_CF_ABR(cf);
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        const float px = (float)x + 0.5f, py = (float)y + 0.5f;
        float w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
        float w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
        float w2 = 1.0f - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        if (g_wincut && fabsf((px - cx) / f) * g_hpx <= NSG_ENG_WIN_X && fabsf((py - cy) / f) * g_hpy <= NSG_ENG_WIN_Y) continue;   /* la vista es la del motor: el píxel es su proyección */
        const float iz = w0 * iw[0] + w1 * iw[1] + w2 * iw[2];
        const size_t o = (size_t)y * b->w + x;
        if (!(iz >= b->iz[o])) {     /* prueba de profundidad de la app: Z invertida, compare >= */
            if (pass == PASS_OPAQUE && iz > b->iz2[o] && tri_apart(tri, b->tid[o])) { b->iz2[o] = iz; b->pid2[o] = gi; }
            continue;
        }
        const float k0 = w0 * iw[0] / iz, k1 = w1 * iw[1] / iz, k2 = w2 * iw[2] / iz;   /* perspectiva correcta */
        float col[3] = {k0 * t[0].r + k1 * t[1].r + k2 * t[2].r, k0 * t[0].g + k1 * t[1].g + k2 * t[2].g, k0 * t[0].b + k1 * t[1].b + k2 * t[2].b}, rgb[3];
        if (tex) {
            const int u = (int)floorf(k0 * t[0].u + k1 * t[1].u + k2 * t[2].u + 1e-4f), v = (int)floorf(k0 * t[0].v + k1 * t[1].v + k2 * t[2].v + 1e-4f);
            const uint16_t tn = ns_sample_texel(g_vram, c0->tpage, (g_tbl[gi].flags & NSG_PF_IRCLUT) ? NSG_IR_CLUT : c0->clut, 0, u, v);
            if (!tn) continue;
            const int b15 = (tn & 0x8000u) != 0;
            if (pass == PASS_OPAQUE) { if (semi && b15) continue; } else if (!b15) continue;
            const float t5[3] = {(float)(tn & 31u), (float)((tn >> 5) & 31u), (float)((tn >> 10) & 31u)};
            for (int k = 0; k < 3; k++) { float q = t5[k] * col[k] / 128.0f; rgb[k] = (q > 31.0f ? 31.0f : q) / 31.0f; }
        } else {
            if (pass == PASS_OPAQUE && semi) continue;
            for (int k = 0; k < 3; k++) rgb[k] = col[k] / 255.0f;
        }
        float *dst = &b->rgb[3 * o];
        b->cov[o] = 1;
        if (pass == PASS_OPAQUE) {
            if (b->pid[o] >= 0 && b->iz[o] > b->iz2[o] && tri_apart(tri, b->tid[o])) { b->iz2[o] = b->iz[o]; b->pid2[o] = b->pid[o]; }
            b->iz[o] = iz; b->pid[o] = gi; b->tid[o] = tri; b->bk[o] = (uint8_t)back; if (g_opmap) g_opmap[o] = g_opcur;
            b->du[o] = k0 * t[0].du + k1 * t[1].du + k2 * t[2].du;
            memcpy(dst, rgb, sizeof rgb);
        } else {
            for (int k = 0; k < 3; k++) {
                float q = abr == 0 ? 0.5f * dst[k] + 0.5f * rgb[k] : abr == 1 ? dst[k] + rgb[k] : abr == 2 ? dst[k] - rgb[k] : dst[k] + 0.25f * rgb[k];
                dst[k] = q < 0 ? 0 : q > 1 ? 1 : q;
            }
        }
    }
}

typedef struct { uint32_t tris, clipped_out, crossing, back, near_rej, hw_big; } TriStats;

/* Un triángulo por el camino de ns_vertex: mundo del motor -> VR -> sesgo -> vista -> recorte -> raster. `use_raise`: 0 = sin sesgo (geometría pura). */
static void draw_tri(Buf *b, const NsScene *sc, int gi, uint32_t tri, int pass, int use_raise, TriStats *ts) {
    const NsGpuPiece *P = &g_tbl[gi];
    const NsGpuCorner *c = &g_corners[3u * tri];
    CV v[3];
    float pvs[3][3], zmin = 1e30f, ev[3][3], wps[3][3];
    for (int k = 0; k < 3; k++) {
        float wp[3], pv[3], col[3];
        ns_gpu_vert_world(g_verts, g_tbl, (uint32_t)gi, c[k].v, wp);
        memcpy(wps[k], wp, sizeof wp);
        ev[k][0] = g_cam_x[0] * wp[0] + g_cam_x[1] * wp[1] + g_cam_x[2] * wp[2] + g_cam_x[3];
        ev[k][1] = g_cam_y[0] * wp[0] + g_cam_y[1] * wp[1] + g_cam_y[2] * wp[2] + g_cam_y[3];
        ev[k][2] = g_cam_fwd[0] * wp[0] + g_cam_fwd[1] * wp[1] + g_cam_fwd[2] * wp[2] + g_cam_fwd[3];
        zmin = fminf(zmin, ev[k][2]);
        corner_col(sc, &c[k], P, wp, col);
        for (int i = 0; i < 3; i++) pv[i] = g_tovr[4 * i] * wp[0] + g_tovr[4 * i + 1] * wp[1] + g_tovr[4 * i + 2] * wp[2] + g_tovr[4 * i + 3];
        memcpy(pvs[k], pv, sizeof pv);
        float e[3] = {pv[0] - b->eye[0], pv[1] - b->eye[1], pv[2] - b->eye[2]};
        v[k].du = -(b->R[6] * e[0] + b->R[7] * e[1] + b->R[8] * e[2]);
        if (use_raise && P->raise != 0.0f) {   /* sesgo de orden (ns_vertex): acercar/alejar |raise| * raise_m a lo largo de la línea de visión, con tope 0,9 de la distancia */
            const float dl = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
            float sh = fminf(fabsf(P->raise) * g_raise_m, 0.9f * dl);
            if (P->raise < 0.0f) sh = -sh;
            for (int i = 0; i < 3; i++) e[i] -= e[i] * (sh / fmaxf(dl, 1e-5f));
        }
        v[k].x = b->R[0] * e[0] + b->R[1] * e[1] + b->R[2] * e[2];
        v[k].y = b->R[3] * e[0] + b->R[4] * e[1] + b->R[5] * e[2];
        v[k].d = -(b->R[6] * e[0] + b->R[7] * e[1] + b->R[8] * e[2]);
        v[k].u = (float)c[k].u; v[k].v = (float)c[k].t; v[k].r = col[0]; v[k].g = col[1]; v[k].b = col[2];
    }
    /* reverso de una cara de una sola cara visto desde ESTE ojo (ns_gpu_tri_back en el mundo VR; g_det_sign = sentido de to_vr): si no se descarta, se dibuja pero pierde los empates (ns_gpu_back_z:
     * la profundidad de recorte, cerca / d, se multiplica por el factor, es decir d se divide por él; la posición en pantalla no cambia) */
    int back = 0, wincut = 0;
    if (c[0].flags & NSG_CF_ONESIDED) {
        back = ns_gpu_tri_back(pvs[0], pvs[1], pvs[2], b->eye);
        if (g_det_sign < 0.0f) back = !back;
        const float bz = (use_raise && !g_old_back) ? ns_gpu_back_z(g_cull_all ? NSG_CULL_ALL : NSG_CULL_NONE, c[0].flags, back) : 1.0f;
        if (bz != 1.0f) for (int k = 0; k < 3; k++) { v[k].x /= bz; v[k].y /= bz; v[k].d /= bz; }
    }
    if (ts) { ts->tris++; ts->back += (uint32_t)back; }
    if (g_zreject > 0.0f && g_old_reject) {
        if (zmin <= g_zreject + 4.0f) { if (ts) ts->near_rej++; return; }   /* descarte cercano anterior (sustituido por el ajuste de la GTE): solo para comparar */
        if (g_cull_all && back && !g_nocull) return;
    } else if (g_zreject > 0.0f && g_old_rule) {   /* --old-rule: la regla anterior (ns_gpu_gte_tri por triángulo entero y reverso con el ojo de la vista), para comparar */
        float ec[3][3];
        const int r = ns_gpu_gte_tri((const float (*)[3])ev, g_zreject, g_hpx, g_hpy, ec);
        if (r == NSG_GTE_BEHIND) { if (ts) ts->near_rej++; return; }
        if (r == NSG_GTE_OVERSIZE && !g_no_hw) { if (ts) ts->hw_big++; return; }
        for (int k = 0; k < 3; k++) if (ec[k][2] != ev[k][2]) { v[k].d = ec[k][2] / 1000.0f; ev[k][2] = ec[k][2]; }
        if (g_cull_all && back && !g_nocull) return;                                  /* NSG_CULL_ALL */
    } else if (g_zreject > 0.0f) {   /* reglas del modo ENGINE (ns_gpu_engine_tri, como ns_vertex): la del motor por triángulo entero y el rescate en el campo extra */
        NsGpuEngCam ec;
        for (int k = 0; k < 3; k++) { ec.eye[k] = sc->cam.eye[k]; ec.x[k] = g_cam_x[k]; ec.y[k] = g_cam_y[k]; ec.fwd[k] = g_cam_fwd[k]; }
        ec.h = 2.0f * g_zreject; ec.hx = g_hpx; ec.hy = g_hpy;
        const float eyes[1][3] = {{sc->cam.eye[0], sc->cam.eye[1], sc->cam.eye[2]}};   /* el ojo de esta vista es el del motor */
        const int r = ns_gpu_engine_tri((const float (*)[3])wps, &ec, (c[0].flags & NSG_CF_ONESIDED) && g_cull_all && !g_nocull, !(P->flags & NSG_PF_NOSPLIT), eyes, 1, (g_no_hw ? NSG_ENG_OFF_SIZE : 0) | (g_no_split ? NSG_ENG_OFF_SPLIT : 0), NULL);   /* NSG_ENG_SPLIT: geometría real, la recorta el plano cercano */
        if (r == NSG_ENG_BEHIND) { if (ts) ts->near_rej++; return; }
        if (r == NSG_ENG_OVERSIZE) { if (ts) ts->hw_big++; return; }
        if (r == NSG_ENG_BACK) return;
        wincut = r == NSG_ENG_EXTRA;
        if (r == NSG_ENG_DRAW) for (int k = 0; k < 3; k++) if (ev[k][2] < g_zreject) { v[k].d = g_zreject / 1000.0f; ev[k][2] = g_zreject; }   /* ajuste al plano H/2 (x e y de la vista intactas) */
    } else if (g_cull_all && back && !g_nocull) return;                               /* NSG_CULL_ALL */
    CV poly[4];
    const int n = clip_near(v, b->znear, poly);
    g_wincut = wincut;
    if (ts) { if (n == 0) ts->clipped_out++; else if (v[0].d < b->znear || v[1].d < b->znear || v[2].d < b->znear) ts->crossing++; }
    for (int i = 1; i + 1 < n; i++) { const CV t3[3] = {poly[0], poly[i], poly[i + 1]}; raster(b, t3, c, pass, gi, (int)tri, back); }
    g_wincut = 0;
}

typedef struct { float depth; int gi; uint32_t tri; } SemiTri;
static int semi_cmp(const void *a, const void *b) { const float x = ((const SemiTri *)a)->depth, y = ((const SemiTri *)b)->depth; return x > y ? -1 : x < y ? 1 : 0; }

int main(int argc, char **argv) {
    const char *dumpd = NULL, *dir = ".", *name = "vr", *pose = "head", *listd = NULL;
    int size = 640, all_groups = 0, miss_top = 0, skip[32], nskip = 0, probe[8][2], nprobe = 0; float tanh = 1.2f, znear = 0.1f, sep = 20.0f, look_yaw = 0, look_pitch = 0, gain = 3.0f;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
#define NEXT() (i + 1 < argc ? argv[++i] : (fprintf(stderr, "falta valor para %s\n", a), exit(2), (char *)0))
        if (!strcmp(a, "--dump")) dumpd = NEXT();
        else if (!strcmp(a, "--out")) dir = NEXT();
        else if (!strcmp(a, "--list")) listd = NEXT();
        else if (!strcmp(a, "--name")) name = NEXT();
        else if (!strcmp(a, "--size")) size = atoi(NEXT());
        else if (!strcmp(a, "--tan")) tanh = (float)atof(NEXT());
        else if (!strcmp(a, "--near")) znear = (float)atof(NEXT());
        else if (!strcmp(a, "--sep")) sep = (float)atof(NEXT());
        else if (!strcmp(a, "--gain")) gain = (float)atof(NEXT());
        else if (!strcmp(a, "--pose")) pose = NEXT();
        else if (!strcmp(a, "--look")) { if (sscanf(NEXT(), "%f,%f", &look_yaw, &look_pitch) != 2) return 2; }
        else if (!strcmp(a, "--all-groups")) all_groups = 1;
        else if (!strcmp(a, "--skip")) { for (const char *q = NEXT(); *q && nskip < 32;) { char *e; skip[nskip++] = (int)strtol(q, &e, 0); if (e == q) return 2; q = *e == ',' ? e + 1 : e; } }
        else if (!strcmp(a, "--probe")) { if (nprobe < 8 && sscanf(NEXT(), "%d,%d", &probe[nprobe][0], &probe[nprobe][1]) == 2) nprobe++; }
        else if (!strcmp(a, "--old-reject")) g_old_reject = 1;   /* comparación: descarte cercano anterior (z <= H/2 + 4) en lugar del ajuste de la GTE */
        else if (!strcmp(a, "--nocull")) g_nocull = 1;
        else if (!strcmp(a, "--skipflag")) g_skipflag = (uint32_t)strtoul(NEXT(), NULL, 0);   /* diagnóstico: no dibuja las mallas con alguno de estos bits de model_flags */
        else if (!strcmp(a, "--miss")) miss_top = atoi(NEXT());
        else if (!strcmp(a, "--old-back")) g_old_back = 1;
        else if (!strcmp(a, "--old-hw")) g_no_hw = 1;
        else if (!strcmp(a, "--old-rule")) g_old_rule = 1;
        else if (!strcmp(a, "--recortadas")) g_no_split = 1;
        else { fprintf(stderr, "opción desconocida: %s (véase la cabecera de nsvr.c)\n", a); return 2; }
    }
    if (!dumpd || size < 16) { fprintf(stderr, "uso: nsvr --dump DIR --out DIR --name N (ver cabecera)\n"); return 2; }
    NsDump *nd = ns_dump_open(dumpd);
    if (!nd) { fprintf(stderr, "no se pudo abrir el volcado %s\n", dumpd); return 1; }
    g_vram = nd->vram;
    static uint16_t novram[1024 * 512];
    if (!g_vram) g_vram = novram;

    /* ---- extracción como la app (glue_extract + ns_hook.capture): sombras a efectos, pies hundidos, cabeza propia oculta ---- */
    NsScene sc; NsMeshStore *st = ns_store_create();
    if (ns_scene_alloc(&sc, NS_MIN_INST, 8192, NS_MIN_LIGHT, 0) || !st) { fprintf(stderr, "sin memoria\n"); return 1; }
    NsExtractOpts eo; memset(&eo, 0, sizeof eo);
    eo.player_key = ns_player_key(nd->ram);
    NsFxScene *fx = ns_fx_scene_create(0);
    NsFxOpts fo = ns_fx_opts_default();
    if (fx && ns_fx_extract(nd->ram, &fo, fx) == 0) { eo.shadow_objs = fx->shadow_objs; eo.n_shadow = fx->n_shadow_objs; }
    if (all_groups) { const uint32_t all = 0xFFFFu; memcpy(nd->ram + (0x800ACAF8u & 0x1FFFFFu), &all, 4); }   /* diagnóstico: máscara de grupos activos del motor a «todos» */
    NsExtractStats es;
    if (ns_extract_ex(&sc, st, nd->ram, NS_RAM_SIZE, nd->scratch, 1024, &eo, &es)) { fprintf(stderr, "sin escena\n"); return 1; }
    ns_sink_apply(&sc, st, NULL);
    const int rec_active = (int)ns_dump_meta_num(nd, "rec_active", 0);
    float rec_eye[3] = {0, 0, 0}, p_ref[3] = {0, 0, 0}, head[16];
    const int have_rec = ns_dump_meta_floats(nd, "rec_eye", rec_eye, 3) == 3 && ns_dump_meta_floats(nd, "rec_p_ref", p_ref, 3) == 3 && ns_dump_meta_floats(nd, "rec_head", head, 16) == 16;
    const int vr = rec_active && have_rec && strcmp(pose, "game");
    if (vr) {
        NsPlayerMap map; memset(&map, 0, sizeof map);
        if (eo.player_key && ns_player_bind(&map, &sc, nd->ram, eo.player_key) > 0) ns_player_hide_part(&map, &sc, NS_PART_HEAD);
        if (eo.player_key) ns_player_hide_head_attached(&sc, nd->ram, eo.player_key);
    } else if (eo.player_key) ns_player_hide_engine_hidden(&sc, nd->ram, eo.player_key);

    /* ---- mundo del motor -> VR (vrcam_native_to_vr: P = R (W - ojo) / upm + p_ref, R = R_y(theta + 180)) y pose de la cabeza ---- */
    Buf B, Bref;
    memset(&B, 0, sizeof B);
    const float upm = (float)ns_dump_meta_num(nd, "rec_units_per_m", 1000.0);
    if (vr) {
        const float th = ((float)ns_dump_meta_num(nd, "rec_theta", 0) + 180.0f) * 3.14159265f / 180.0f, cs = cosf(th), sn = sinf(th);
        const float R[9] = {cs, 0, -sn, 0, 1, 0, sn, 0, cs};
        for (int i = 0; i < 3; i++) {
            float a = 0;
            for (int j = 0; j < 3; j++) { g_tovr[4 * i + j] = R[3 * i + j] / upm; a += g_tovr[4 * i + j] * rec_eye[j]; }
            g_tovr[4 * i + 3] = p_ref[i] - a;
        }
        for (int i = 0; i < 3; i++) { B.eye[i] = head[12 + i]; for (int j = 0; j < 3; j++) B.R[3 * i + j] = head[4 * i + j]; }   /* columnas 0..2 = derecha, arriba, atrás */
        for (int i = 0; i < 3; i++) { sc.cam.eye[i] = rec_eye[i]; }
        float M[9];
        if (ns_dump_meta_floats(nd, "rec_M", M, 9) == 9) memcpy(sc.cam.rot, M, sizeof M);
        sc.cam.vr_active = 1; sc.cam.overscan_y = 1.0f;
    } else {
        /* cámara del motor: VR = vista del juego en metros (x derecha, y arriba, z atrás), ojo en el origen */
        for (int j = 0; j < 3; j++) { g_tovr[j] = sc.cam.rot[3 * j] / 1000.0f; g_tovr[4 + j] = -sc.cam.rot[3 * j + 1] / 1000.0f; g_tovr[8 + j] = -sc.cam.rot[3 * j + 2] / 1000.0f; }
        for (int i = 0; i < 3; i++) g_tovr[4 * i + 3] = -(g_tovr[4 * i] * sc.cam.eye[0] + g_tovr[4 * i + 1] * sc.cam.eye[1] + g_tovr[4 * i + 2] * sc.cam.eye[2]);
        B.R[0] = B.R[4] = B.R[8] = 1.0f;
    }
    if (look_yaw != 0 || look_pitch != 0) {   /* giro extra sobre la cabeza: guiñada sobre Y del mundo VR, cabeceo sobre el eje derecho */
        const float y = look_yaw * 3.14159265f / 180.0f, p = look_pitch * 3.14159265f / 180.0f, cy = cosf(y), sy = sinf(y), cp = cosf(p), sp = sinf(p);
        float R2[9];
        for (int r = 0; r < 3; r++) { const float *a = &B.R[3 * r]; R2[3 * r] = cy * a[0] + sy * a[2]; R2[3 * r + 1] = a[1]; R2[3 * r + 2] = -sy * a[0] + cy * a[2]; }
        for (int k = 0; k < 3; k++) { const float up = R2[3 + k], bk = R2[6 + k]; B.R[k] = R2[k]; B.R[3 + k] = cp * up + sp * bk; B.R[6 + k] = -sp * up + cp * bk; }
    }
    if (!vr) { g_cull_all = 1; g_zreject = 0.5f * sc.cam.h; }
    for (int j = 0; j < 3; j++) { g_cam_x[j] = sc.cam.rot[3 * j]; g_cam_y[j] = sc.cam.rot[3 * j + 1]; g_cam_fwd[j] = sc.cam.rot[3 * j + 2]; }
    g_cam_x[3] = -(g_cam_x[0] * sc.cam.eye[0] + g_cam_x[1] * sc.cam.eye[1] + g_cam_x[2] * sc.cam.eye[2]);
    g_cam_y[3] = -(g_cam_y[0] * sc.cam.eye[0] + g_cam_y[1] * sc.cam.eye[1] + g_cam_y[2] * sc.cam.eye[2]);
    g_cam_fwd[3] = -(g_cam_fwd[0] * sc.cam.eye[0] + g_cam_fwd[1] * sc.cam.eye[1] + g_cam_fwd[2] * sc.cam.eye[2]);
    g_hpx = sc.cam.h; g_hpy = sc.cam.h * (sc.cam.overscan_y > 0.5f ? sc.cam.overscan_y : 1.0f);
    g_det_sign = (g_tovr[0] * (g_tovr[5] * g_tovr[10] - g_tovr[6] * g_tovr[9]) - g_tovr[1] * (g_tovr[4] * g_tovr[10] - g_tovr[6] * g_tovr[8]) + g_tovr[2] * (g_tovr[4] * g_tovr[9] - g_tovr[5] * g_tovr[8])) < 0.0f ? -1.0f : 1.0f;
    g_mpu = sqrtf(g_tovr[0] * g_tovr[0] + g_tovr[4] * g_tovr[4] + g_tovr[8] * g_tovr[8]);
    g_raise_m = NS_GPU_RAISE_UNIT / (sc.units_per_m > 1 ? sc.units_per_m : 1000.0f);   /* fogPar.w de NativeRenderer.ingest */
    B.tanh = tanh; B.znear = znear;
    Bref = B;
    buf_alloc(&B, size); buf_alloc(&Bref, size);

    /* ---- mallas y tabla de piezas como NativeRenderer.ingest ---- */
    const uint32_t np = sc.n_piece;
    GMesh *gm = calloc(np ? np : 1, sizeof *gm);
    g_tbl = calloc(np ? np : 1, sizeof *g_tbl);
    uint32_t nv = 0, ntr = 0;
    for (uint32_t i = 0; i < np; i++) {
        const NsMesh *m = ns_store_get(st, sc.piece[i].mesh_id);
        if (!m) continue;
        gm[i].m = m; gm[i].ntri = ns_gpu_tri_count(m);
        gm[i].bk = (sc.piece[i].baked_ver && (m->flags & NS_MF_BAKED)) ? ns_store_get_baked(st, sc.piece[i].mesh_id, sc.piece[i].baked_ver) : NULL;
        nv += m->n_verts; ntr += gm[i].ntri;
    }
    g_verts = calloc(nv ? nv : 1, sizeof *g_verts); g_corners = calloc(ntr ? 3u * ntr : 1, sizeof *g_corners);
    nv = 0; ntr = 0;
    for (uint32_t i = 0; i < np; i++) {
        if (!gm[i].m || !gm[i].ntri || !gm[i].m->n_verts) { gm[i].ntri = 0; continue; }
        gm[i].vbase = nv; gm[i].tbase = ntr;
        ns_gpu_pack(gm[i].m, gm[i].bk, g_verts + nv, g_corners + 3u * ntr, nv, NULL, NULL);
        gm[i].semi = (gm[i].m->flags & NS_MF_BLEND) != 0;
        for (uint32_t f = 0; f < gm[i].m->n_faces; f++) if (gm[i].m->faces[f].texid) { gm[i].textured = 1; break; }
        nv += gm[i].m->n_verts; ntr += gm[i].ntri;
    }
    uint32_t n_hidden = 0, n_group = 0, n_nomesh = 0, n_draw = 0, n_bias_pos = 0, n_bias_neg = 0, n_lift = 0, n_static = 0;
    int *is_static = calloc(np ? np : 1, sizeof(int));
    for (uint32_t ii = 0; ii < sc.n_inst; ii++) {
        const NsInstance *in = &sc.inst[ii];
        const int in_group = (in->flags & NS_IF_STATIC) != 0 || sc.group_mask == 0 || in->zone_mask == 0 || (in->zone_mask & sc.group_mask) != 0;   /* como NativeRenderer: el escenario se dibuja con independencia del grupo activo */
        for (uint32_t k = 0; k < in->n_pieces; k++) {
            const uint32_t gi = in->first_piece + k;
            const NsPiece *p = &sc.piece[gi];
            is_static[gi] = (in->flags & NS_IF_STATIC) != 0;
            if (!gm[gi].m || !gm[gi].ntri) { if (!(p->flags & NS_PF_HIDDEN)) n_nomesh++; else n_hidden++; continue; }
            int pg = -1; uint32_t pnv = 0;
            if (p->parent >= 0 && p->parent < (int)in->n_pieces && gm[in->first_piece + (uint32_t)p->parent].ntri) { pg = (int)(in->first_piece + (uint32_t)p->parent); pnv = gm[pg].m->n_verts; }
            ns_gpu_make_piece(&sc, in, p, gm[gi].m->flags, gm[gi].m->depth_bias, gm[gi].vbase, pg, pnv, &g_tbl[gi]);
            g_tbl[gi].rest_r = ns_gpu_mesh_radius(gm[gi].m);
            if (p->flags & NS_PF_HIDDEN) { g_tbl[gi].flags &= ~NSG_PF_DRAW; n_hidden++; continue; }
            if (!in_group) { g_tbl[gi].flags &= ~NSG_PF_DRAW; n_group++; continue; }
            if (g_skipflag && (gm[gi].m->model_flags & g_skipflag)) { g_tbl[gi].flags &= ~NSG_PF_DRAW; continue; }
            { int sk = 0; for (int q = 0; q < nskip; q++) sk |= skip[q] == (int)gi; if (sk) { g_tbl[gi].flags &= ~NSG_PF_DRAW; continue; } }   /* diagnóstico: --skip */
            n_draw++;
            if (is_static[gi]) n_static++;
            if (gm[gi].m->depth_bias > 0) n_bias_pos++; else if (gm[gi].m->depth_bias < 0) n_bias_neg++;
            if (ns_sink_lift_of(p->flags) > 0) n_lift++;
        }
    }
    printf("== %s (%s) ==\n", name, vr ? "cabeza VR" : "camara del motor");
    printf("escena: instancias=%u piezas=%u grupo=0x%x | dibujadas=%u (escenario=%u) ocultas=%u fuera_de_grupo=%u sin_malla=%u | sesgo: +=%u -=%u adelanto_pies=%u\n",
           sc.n_inst, np, sc.group_mask, n_draw, n_static, n_hidden, n_group, n_nomesh, n_bias_pos, n_bias_neg, n_lift);
    {
        uint32_t empty = 0;
        for (uint32_t i = 0; i < np; i++) empty += !ns_gpu_piece_has_mesh(&sc.piece[i]);
        printf("mallas ausentes segun la app: %u (>0 retiene la escena anterior 25 escenas) | piezas sin malla (modelo nulo o sin caras): %u (la build 0d4635f las contaba como ausentes)\n", ns_gpu_scene_misses(&sc, st), empty);
    }
    printf("extractor: cola=%u (recortada=%u) descartes: nulo=%u ptr=%u dup=%u oculto=%u grupo=%u objeto=%u modelo=%u | ext=%u ext_rotas=%u | caras: leidas=%u v_malo=%u n_malo=%u degeneradas=%u | soldadura_mala=%u almacen_lleno=%u memo_llena=%u lecturas_fuera=%u\n",
           es.queue_n, es.q_clamped, es.drop_null, es.drop_badptr, es.drop_dup, es.drop_hidden_flag, es.drop_group, es.drop_badobj, es.drop_bad_model, es.n_ext_pieces, es.ext_bad, es.faces_seen, es.faces_bad_vindex, es.faces_bad_nindex, es.faces_degenerate, es.weld_bad, es.store_full, es.memo_full, es.oob_reads);
    {   /* red de seguridad de vértices (solo piezas dinámicas): triángulos que el sombreador colapsaría y piezas con causa de matriz/soldadura; sirve para buscar falsos positivos */
        uint32_t nex = 0, npn = 0, ntn = 0, nfar = 0, nedge = 0, ncause[4] = {0, 0, 0, 0};
        for (uint32_t gi = 0; gi < np; gi++) {
            if (!(g_tbl[gi].flags & NSG_PF_DRAW) || !(g_tbl[gi].flags & NSG_PF_NET) || !gm[gi].ntri) continue;
            nex++;
            NsGpuNetPiece pc; NsGpuNetTris acc; memset(&acc, 0, sizeof acc);
            ns_gpu_net_piece(g_verts, g_tbl, gi, gm[gi].m->n_verts, g_mpu, &pc);
            const uint32_t n = ns_gpu_net_tris(g_verts, g_corners + 3u * gm[gi].tbase, gm[gi].ntri, g_tbl, gi, g_mpu, &acc);
            for (int q = 0; q < 4; q++) ncause[q] += (pc.causes >> q) & 1u;
            if (n) { npn++; ntn += n; nfar += acc.bad_far; nedge += acc.bad_edge; }
        }
        printf("red de seguridad (dinamicas, %u piezas examinadas): %u piezas con triangulos colapsados, %u tris (lejos=%u arista=%u) | piezas con causa matriz=%u soldadura=%u lejos=%u arista=%u\n", nex, npn, ntn, nfar, nedge, ncause[0], ncause[1], ncause[2], ncause[3]);
    }
    if (vr) {
        float hw[3];   /* cabeza en el mundo del motor */
        for (int j = 0; j < 3; j++) hw[j] = rec_eye[j] + (g_tovr[j] * (B.eye[0] - p_ref[0]) + g_tovr[4 + j] * (B.eye[1] - p_ref[1]) + g_tovr[8 + j] * (B.eye[2] - p_ref[2])) / (g_mpu * g_mpu);
        printf("cabeza: VR=(%.3f %.3f %.3f) motor=(%.0f %.0f %.0f) ojo_del_motor=(%.0f %.0f %.0f) upm=%.1f\n", B.eye[0], B.eye[1], B.eye[2], hw[0], hw[1], hw[2], rec_eye[0], rec_eye[1], rec_eye[2], upm);
    }

    /* ---- dibujo: opacos (escenario, dinámicos) y semitransparentes de atrás adelante; dos veces: como la app y sin sesgo ---- */
    TriStats ts; memset(&ts, 0, sizeof ts);
    for (int variant = 0; variant < 2; variant++) {
        Buf *b = variant ? &Bref : &B;
        for (int stat = 1; stat >= 0; stat--)
            for (uint32_t gi = 0; gi < np; gi++) {
                if (!(g_tbl[gi].flags & NSG_PF_DRAW) || is_static[gi] != stat) continue;
                if (gm[gi].semi && !gm[gi].textured) continue;
                for (uint32_t t = 0; t < gm[gi].ntri; t++) draw_tri(b, &sc, (int)gi, gm[gi].tbase + t, PASS_OPAQUE, !variant, variant ? NULL : &ts);
            }
        uint32_t ns = 0;
        for (uint32_t gi = 0; gi < np; gi++) if ((g_tbl[gi].flags & NSG_PF_DRAW) && gm[gi].semi) ns += gm[gi].ntri;
        SemiTri *sl = malloc((ns ? ns : 1) * sizeof *sl);
        ns = 0;
        for (uint32_t gi = 0; gi < np; gi++) {
            if (!(g_tbl[gi].flags & NSG_PF_DRAW) || !gm[gi].semi) continue;
            for (uint32_t t = 0; t < gm[gi].ntri; t++) {
                float cen[3] = {0, 0, 0};
                for (int k = 0; k < 3; k++) { float wp[3]; ns_gpu_vert_world(g_verts, g_tbl, gi, g_corners[3u * (gm[gi].tbase + t) + (uint32_t)k].v, wp); for (int i = 0; i < 3; i++) cen[i] += wp[i] / 3.0f; }
                float pv[3];
                for (int i = 0; i < 3; i++) pv[i] = g_tovr[4 * i] * cen[0] + g_tovr[4 * i + 1] * cen[1] + g_tovr[4 * i + 2] * cen[2] + g_tovr[4 * i + 3] - b->eye[i];
                sl[ns].depth = -(b->R[6] * pv[0] + b->R[7] * pv[1] + b->R[8] * pv[2]); sl[ns].gi = (int)gi; sl[ns].tri = gm[gi].tbase + t; ns++;
            }
        }
        qsort(sl, ns, sizeof *sl, semi_cmp);
        for (uint32_t i = 0; i < ns; i++) draw_tri(b, &sc, sl[i].gi, sl[i].tri, PASS_SEMI, !variant, NULL);
        free(sl);
    }

    /* ---- pantalla 3D frente a la lista de la GPU (lo que el motor dibujó de verdad): cobertura de la ventana de la pantalla del motor ---- */
    if (listd && !vr) {
        NsDump *ld = ns_dump_open(listd);
        if (ld && ld->has_list) {
            Buf L = B;
            buf_alloc(&L, size);
            const float ys = sc.cam.overscan_y > 0.5f ? sc.cam.overscan_y : 1.0f, H = sc.cam.h > 8.0f ? sc.cam.h : 320.0f;
            static NsGpuCorner lc;   /* esquina ficticia: textura de la operación */
            uint32_t n3d = 0, nbig = 0;
            int32_t *opmap = miss_top ? malloc((size_t)size * size * sizeof(int32_t)) : NULL;
            if (opmap) { for (size_t q = 0; q < (size_t)size * size; q++) opmap[q] = -1; g_opmap = opmap; }
            const uint16_t *save = g_vram;
            if (ld->vram) g_vram = ld->vram;
            for (uint32_t i = 0; i < ld->list.nops; i++) {
                const GpuOp *op = &ld->list.ops[i];
                if (op->type != GPU_OP_POLY || op->nv < 3) continue;
                int ok = 1;
                for (int k = 0; k < op->nv; k++) ok &= op->v[k].vvalid && op->v[k].vz > 0.0f;
                if (!ok) continue;
                n3d++; g_opcur = (int)i;
                memset(&lc, 0, sizeof lc);
                if (op->flags & GPU_F_TEXTURED) { lc.flags = NSG_CF_TEX; lc.tpage = (uint16_t)((op->tx / 64) | ((op->ty / 256) << 4) | (op->tp << 7)); lc.clut = (uint16_t)((op->clx / 16) | (op->cly << 6)); }
                for (int h = 0; h + 2 < op->nv; h++) {
                    const GpuVtx *q[3] = {&op->v[h], &op->v[h + 1], &op->v[h + 2]};
                    int mnx = 32767, mxx = -32768, mny = 32767, mxy = -32768;
                    for (int k = 0; k < 3; k++) { mnx = q[k]->x < mnx ? q[k]->x : mnx; mxx = q[k]->x > mxx ? q[k]->x : mxx; mny = q[k]->y < mny ? q[k]->y : mny; mxy = q[k]->y > mxy ? q[k]->y : mxy; }
                    if (mxx - mnx > 1023 || mxy - mny > 511) { nbig++; continue; }   /* el hardware no dibuja un triángulo mayor de 1023 x 511 px */
                    CV t3[3];
                    for (int k = 0; k < 3; k++) {
                        t3[k].x = q[k]->vx / 1000.0f; t3[k].y = -q[k]->vy / ys / 1000.0f; t3[k].d = t3[k].du = q[k]->vz / 1000.0f;
                        t3[k].u = (float)q[k]->u; t3[k].v = (float)q[k]->v; t3[k].r = t3[k].g = t3[k].b = 128.0f;
                    }
                    CV poly[4];
                    const int n = clip_near(t3, B.znear, poly);
                    for (int j = 1; j + 1 < n; j++) { const CV tt[3] = {poly[0], poly[j], poly[j + 1]}; raster(&L, tt, &lc, PASS_OPAQUE, 0, 0, 0); }
                }
            }
            g_vram = save; g_opmap = NULL;
            size_t win = 0, both = 0, extra = 0, miss = 0, front = 0, behind = 0;
            uint32_t *opmiss = calloc(ld->list.nops ? ld->list.nops : 1, sizeof(uint32_t));
            const float f = 0.5f * (float)size / B.tanh;
            for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) {
                const float tx = ((float)x + 0.5f - 0.5f * (float)size) / f, ty = ((float)y + 0.5f - 0.5f * (float)size) / f;
                if (fabsf(tx) * H > 160.0f || fabsf(ty) * ys * H > 120.0f) continue;
                const size_t o = (size_t)y * size + x;
                win++;
                if (B.cov[o] && L.cov[o]) {
                    both++;
                    if (B.iz[o] > 1.3f * L.iz[o]) front++; else if (B.iz[o] < L.iz[o] / 1.3f) behind++;   /* otra superficie: el nativo tapa lo que el motor enseña / enseña lo que el motor tapa */
                } else if (B.cov[o]) extra++; else if (L.cov[o]) { miss++; if (opmap && opmap[o] >= 0) opmiss[opmap[o]]++; }
            }
            printf("pantalla 3D frente a la lista (ventana del motor, %zu px): poligonos3D=%u (tris que el hardware descarta por tamano=%u) | coinciden=%.1f%% (de ellos NATIVO_TAPA=%.1f%% nativo_mas_lejos=%.1f%%) NATIVO_DE_MAS=%.1f%% FALTA_EN_NATIVO=%.1f%% vacio=%.1f%%\n", win, n3d, nbig,
                   win ? 100.0 * (double)both / (double)win : 0.0, win ? 100.0 * (double)front / (double)win : 0.0, win ? 100.0 * (double)behind / (double)win : 0.0, win ? 100.0 * (double)extra / (double)win : 0.0, win ? 100.0 * (double)miss / (double)win : 0.0, win ? 100.0 * (double)(win - both - extra - miss) / (double)win : 0.0);
            if (opmap) {   /* diagnóstico --miss: mapa de cobertura (rojo = solo la lista, verde = solo el nativo, gris = ambos, azul oscuro = fuera de la ventana) */
                uint8_t *mp = calloc((size_t)size * size, 3);
                for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) {
                    const float tx = ((float)x + 0.5f - 0.5f * (float)size) / f, ty = ((float)y + 0.5f - 0.5f * (float)size) / f;
                    const size_t o = (size_t)y * size + x;
                    uint8_t *q = &mp[3 * o];
                    if (fabsf(tx) * H > 160.0f || fabsf(ty) * ys * H > 120.0f) { q[2] = 60; continue; }
                    if (B.cov[o] && L.cov[o]) q[0] = q[1] = q[2] = 120; else if (B.cov[o]) q[1] = 220; else if (L.cov[o]) q[0] = 255;
                }
                char mpath[1024]; snprintf(mpath, sizeof mpath, "%s/%s_vr_miss.png", dir, name); ns_write_png(mpath, size, size, mp); free(mp);
            }
            if (opmap) {   /* diagnóstico --miss: operaciones de la lista con más píxeles sin cubrir */
                for (int rank = 0; rank < miss_top; rank++) {
                    uint32_t bi = 0, bn = 0;
                    for (uint32_t i = 0; i < ld->list.nops; i++) if (opmiss[i] > bn) { bn = opmiss[i]; bi = i; }
                    if (!bn) break;
                    const GpuOp *op = &ld->list.ops[bi];
                    float zmn = 1e30f, zmx = -1e30f;
                    for (int k = 0; k < op->nv; k++) { zmn = fminf(zmn, op->v[k].vz); zmx = fmaxf(zmx, op->v[k].vz); }
                    printf("  miss op %u: %u px nv=%d tex=%d semi=%d vz=[%.0f..%.0f] tp=%u,%u", bi, bn, op->nv, (op->flags & GPU_F_TEXTURED) != 0, (op->flags & GPU_F_SEMI) != 0, zmn, zmx, op->tx, op->ty);
                    for (int k = 0; k < op->nv; k++) printf(" (%d,%d z%.0f)", op->v[k].x, op->v[k].y, op->v[k].vz);
                    printf("\n");
                    opmiss[bi] = 0;
                }
                free(opmap);
            }
            free(opmiss);
            buf_free(&L);
        }
        if (ld) ns_dump_close(ld);
    }
    /* ---- medidas ---- */
    const size_t npx = (size_t)size * size;
    size_t covered = 0, changed = 0, through = 0, coplanar = 0, fight = 0, holes_app = 0, backpx = 0;
    const float sep_m = sep * g_mpu;
    uint8_t *dif = calloc(npx, 3);
    typedef struct { int a, b; size_t n; float maxsep; } Pair;
    Pair pairs[64]; int npairs = 0;
    Pair fpairs[64]; int nfp = 0;
    int *inst_of = calloc(np ? np : 1, sizeof(int));
    for (uint32_t ii = 0; ii < sc.n_inst; ii++) for (uint32_t k = 0; k < sc.inst[ii].n_pieces; k++) inst_of[sc.inst[ii].first_piece + k] = (int)ii;
    for (size_t o = 0; o < npx; o++) {
        if (Bref.pid[o] >= 0) covered++;
        if (B.pid[o] >= 0 && B.bk[o]) { backpx++; dif[3 * o] = dif[3 * o + 1] = dif[3 * o + 2] = 90; }
        if (B.pid[o] < 0 && Bref.pid[o] >= 0) holes_app++;
        if (B.pid[o] != Bref.pid[o] && B.pid[o] >= 0 && Bref.pid[o] >= 0) {
            changed++;
            const float s = B.du[o] - Bref.du[o];    /* cuánto más lejos (sin sesgo) está lo que la app enseña */
            if (s > sep_m) {
                through++; dif[3 * o] = 255;
                int k;
                for (k = 0; k < npairs; k++) if (pairs[k].a == B.pid[o] && pairs[k].b == Bref.pid[o]) break;
                if (k == npairs && npairs < 64) { pairs[npairs].a = B.pid[o]; pairs[npairs].b = Bref.pid[o]; pairs[npairs].n = 0; pairs[npairs].maxsep = 0; npairs++; }
                if (k < npairs) { pairs[k].n++; if (s > pairs[k].maxsep) pairs[k].maxsep = s; }
            } else { coplanar++; dif[3 * o + 1] = 255; }
        }
        /* coplanares sin sesgo que las separe: dos piezas distintas a la misma profundidad (relativa < 2e-5) en la imagen de la app */
        if (B.pid[o] >= 0 && B.pid2[o] >= 0 && B.iz2[o] > 0 && fabsf(B.iz[o] - B.iz2[o]) < 2e-5f * B.iz[o]) {
            fight++; dif[3 * o + 2] = 255;
            const int a = B.pid[o] < B.pid2[o] ? B.pid[o] : B.pid2[o], c = B.pid[o] < B.pid2[o] ? B.pid2[o] : B.pid[o];
            int k;
            for (k = 0; k < nfp; k++) if (fpairs[k].a == a && fpairs[k].b == c) break;
            if (k == nfp && nfp < 64) { fpairs[nfp].a = a; fpairs[nfp].b = c; fpairs[nfp].n = 0; fpairs[nfp].maxsep = 0; nfp++; }
            if (k < nfp) fpairs[k].n++;
        }
    }
    if (g_cull_all) printf("pantalla 3D (camara del motor): descarte de reversos activo, ajuste GTE a z=%.0f (descartados sin vertice delante): %u tris, descarte del hardware por tamano (>1023x511 px): %u tris\n", g_zreject, ts.near_rej, ts.hw_big);
    printf("triangulos: %u | plano cercano %.2f m: recortados_enteros=%u lo_cruzan=%u | reversos de una cara: %u tris, visibles en %.2f%% px (con descarte de reversos serian huecos)\n", ts.tris, znear, ts.clipped_out,
           ts.crossing, ts.back, 100.0 * (double)backpx / (double)npx);
    printf("pixeles: cubiertos=%.1f%% | sesgo: cambian=%.2f%% (coplanares<=%.0fu=%.2f%%, A_TRAVES=%.2f%%) | coplanares_sin_sesgo=%.2f%% | huecos_solo_app=%.2f%%\n",
           100.0 * (double)covered / (double)npx, 100.0 * (double)changed / (double)npx, sep, 100.0 * (double)coplanar / (double)npx, 100.0 * (double)through / (double)npx,
           100.0 * (double)fight / (double)npx, 100.0 * (double)holes_app / (double)npx);
    for (int k = 0; k < npairs; k++) {
        if (pairs[k].n * 2000 < npx) continue;
        const int a = pairs[k].a, c = pairs[k].b;
        printf("  a_traves: pieza %d (sesgo %d%s%s) se ve por delante de pieza %d (sesgo %d%s): %.2f%% px, hasta %.0f u por detras\n", a, gm[a].m->depth_bias, is_static[a] ? ", escenario" : ", dinamica",
               ns_sink_lift_of(sc.piece[a].flags) > 0 ? ", pies" : "", c, gm[c].m->depth_bias, is_static[c] ? ", escenario" : ", dinamica", 100.0 * (double)pairs[k].n / (double)npx, (double)(pairs[k].maxsep / g_mpu));
    }

    for (int k = 0; k < nfp; k++) {
        if (fpairs[k].n * 500 < npx) continue;
        const int a = fpairs[k].a, c = fpairs[k].b;
        printf("  coplanares: pieza %d (obj %08x grupo 0x%x sesgo %d%s) y pieza %d (obj %08x grupo 0x%x sesgo %d%s): %.2f%% px\n", a, sc.inst[inst_of[a]].key, sc.inst[inst_of[a]].zone_mask,
               gm[a].m->depth_bias, is_static[a] ? " escenario" : "", c, sc.inst[inst_of[c]].key, sc.inst[inst_of[c]].zone_mask, gm[c].m->depth_bias, is_static[c] ? " escenario" : "",
               100.0 * (double)fpairs[k].n / (double)npx);
    }
    for (int q = 0; q < nprobe; q++) {   /* diagnóstico: qué pieza se ve en un píxel */
        const int x = probe[q][0], y = probe[q][1];
        if (x < 0 || y < 0 || x >= size || y >= size) continue;
        const size_t o = (size_t)y * size + x;
        const int a = B.pid[o];
        if (a < 0) { printf("  pixel %d,%d: vacio\n", x, y); continue; }
        const NsInstance *in = &sc.inst[inst_of[a]];
        printf("  pixel %d,%d: pieza %d (obj %08x pieza_rel %d grupo 0x%x vis %d flags_inst 0x%x) malla %08x flags_malla 0x%x marcas_motor 0x%x sesgo %d tris %u prof %.3f m reverso %d | detras: pieza %d\n", x, y, a, in->key,
               a - (int)in->first_piece, in->zone_mask, in->vis_state, in->flags, (uint32_t)gm[a].m->id, gm[a].m->flags, gm[a].m->model_flags, gm[a].m->depth_bias, gm[a].ntri, 1.0 / B.iz[o], B.bk[o], B.pid2[o]);
    }
    free(inst_of);
    /* ---- imágenes ---- */
    uint8_t *rgb = malloc(npx * 3);
    char path[1024];
    for (int variant = 0; variant < 2; variant++) {
        const Buf *b = variant ? &Bref : &B;
        for (size_t o = 0; o < npx * 3; o++) { float q = b->rgb[o] * gain; rgb[o] = (uint8_t)(255.0f * (q > 1 ? 1 : q) + 0.5f); }
        snprintf(path, sizeof path, "%s/%s_vr%s.png", dir, name, variant ? "_ref" : ""); ns_write_png(path, size, size, rgb);
    }
    snprintf(path, sizeof path, "%s/%s_vr_dif.png", dir, name); ns_write_png(path, size, size, dif);
    free(rgb); free(dif); free(gm); free(is_static); free(g_tbl); free(g_verts); free(g_corners);
    buf_free(&B); buf_free(&Bref);
    ns_scene_free(&sc); ns_store_destroy(st); ns_dump_close(nd);
    return 0;
}
