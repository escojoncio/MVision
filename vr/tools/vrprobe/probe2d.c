/* probe2d: comparación automática por fotograma entre el fotograma REAL (ref_raster, exacto a 1x) y la composición que hace la capa VR con la cámara del juego:
 * pase 3D (kind 0/1 de la escena ensamblada, hud_emu_run3d con la vista del juego) + filtro de pantalla (hud_emu_post2) + panel 2D (kind 2, hud_emu_run) encima.
 * Herramienta LOCAL (necesita el disco del usuario); no entra en la app ni en CI. Mismo arranque que vrprobe (título -> partida) y después un guion de mando
 * de partida (andar, girar, disparar, abrir ventanas).
 *
 *   MV_SIGS=firmas.json probe2d <exe> <disco> [--out DIR] [--frames N] [--sin-partida] [--tol T] [--umbral-px N] [--seq a,b] [--th nombre=valor]...
 *
 * Mide, sobre TODAS las listas capturadas (una por fotograma mostrado):
 *   discrepancias   píxeles de la composición que difieren del fotograma real (> tol en algún canal), atribuidos a la ÚLTIMA primitiva de la lista que cubre el píxel
 *                   (tipo = 3D opaco/semi, 2D polígono, parcial, rect, línea, evento de VRAM, sin primitiva), con y sin textura, con y sin semitransparencia;
 *   parpadeos       (a) cambios de clasificación 3D/2D/parcial de una MISMA primitiva (misma textura, UV, color y modo) entre listas consecutivas;
 *                   (b) fotogramas aislados con discrepancia alta entre vecinos limpios;
 *                   (c) primitivas dibujadas en fotogramas alternos (presente en t-2, ausente en t-1, presente en t);
 *   listas          saltos del índice de lista (listas perdidas).
 * Código de salida 0 = PASA. Ver bitacora.md ("Capa 2D"). */
#define _GNU_SOURCE
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "bridge.h"
#include "cpu.h"
#include "gpu.h"
#include "hud_emu.h"
#include "host.h"
#include "probe_raster.h"
#include "ref_raster.h"
#include "sys.h"
#include "vrhost.h"

/* ------------------------------------------------------------------------------------------------ parámetros */
static char g_out[512] = ".";
static int g_rad = 1;                /* radio del vecindario en la comparación */
static int g_tol = 28;               /* diferencia máxima por canal (0..255) entre medias de ventana que se tolera */
static int g_win = 4;                /* lado de la ventana de promediado */
static int g_px_thr = 24;            /* píxeles discrepantes para dar un fotograma por "malo" */
static uint32_t g_from = 0;          /* a partir de qué VBlank se analiza (antes solo se avanza la VRAM de referencia: más rápido) */
static uint32_t g_frames = 15000;    /* VBlanks de emulación a recorrer */
static int g_partida = 1;            /* 1: tras el control, guion de partida */
static int g_seq_a = -1, g_seq_b = -1;
static double TH_flip = 0, TH_alt = 0, TH_isol = 0, TH_bad = 1e9;   /* umbrales: cambios de clasificación, fotogramas alternos, aislados, fotogramas con discrepancia */
static const struct { const char *n; double *p; } THN[] = {{"cambios_clase", &TH_flip}, {"alternos", &TH_alt}, {"aislados", &TH_isol}, {"fotogramas_malos", &TH_bad}};

#define MAXF 40000
#define MAXW 1024
#define MAXH 512

/* ------------------------------------------------------------------------------------------------ tipos de primitiva */
enum { C_3D = 0, C_2D, C_PART, C_RECT, C_LINE, C_EVT, C_SPR, C_NCLS };   /* C_SPR: polígono 3D con vértices derivados (cartel) */
static const char *CLS_NAME[C_NCLS] = {"3d", "2d_poli", "descartado", "rect", "linea", "evento_vram", "cartel_3d"};
#define NT (C_NCLS * 4 + 2)          /* cls*4 + tex*2 + semi; + sin primitiva + radar */
#define T_NONE (C_NCLS * 4)
#define T_RADAR (C_NCLS * 4 + 1)
static const char *tname(int t, char *b) {
    if (t == T_NONE) { snprintf(b, 40, "sin_primitiva"); return b; }
    if (t == T_RADAR) { snprintf(b, 40, "radar"); return b; }
    snprintf(b, 40, "%s%s%s", CLS_NAME[t / 4], (t & 2) ? "+tex" : "", (t & 1) ? "+semi" : "");
    return b;
}

static MvVrGameState g_gs;
static int g_ops_a = -1, g_ops_b = -1, g_head = 1, g_trace_alt;
static const char *g_replay;
typedef struct { int x0, y0, x1, y1; int type; int cls; } OpBox;

/* diagnóstico de polígonos parciales (algún vértice sin posición de vista): patrón de vértices válidos, tamaño en pantalla y banderas de los válidos */
static struct { uint32_t n[2][16], big, small, flip_pat[2][16], nf; uint32_t vf[16], semi, tex, fbk, inhud; } PS;

typedef struct {
    uint32_t idx, hf;
    int w, h, has3d, filt, user_space;
    uint32_t mism[NT];
    uint32_t total;
    uint32_t flips[3][3];            /* [de][a] entre 3D / 2D / parcial */
    uint32_t alt[C_NCLS];            /* primitivas en fotograma alterno por clase */
    uint32_t nops, nrad;
    uint32_t lost;                   /* listas perdidas desde la anterior */
    int video;
    int kind, mode, reason;          /* clase de vista del host (VRV_K_*: 0 2D, 1 mundo, 2 cinemática del motor, 3 vídeo, 4 respaldo), modo (1 mundo, 0 cine) y motivo */
    uint32_t cpy_disp;               /* píxeles del área mostrada escritos por copias/subidas de VRAM (contenido que el panel/escena no reproduce) */
} FRec;
static FRec *FR;
static uint32_t nfr;

/* peores imágenes por tipo */
typedef struct { uint32_t score, idx; int w, h; uint8_t *img; } Worst;   /* img: 3w x h, [real | composición | diferencia] */
static Worst WST[NT];

/* ------------------------------------------------------------------------------------------------ tabla de claves (clasificación entre listas) */
#define KT (1u << 16)
typedef struct { uint64_t key; uint32_t last, lvis; uint8_t lastcls; } Key;
static Key KTAB[KT], KTAB2[KT];   /* KTAB2: clave sin colores (la iluminación cambia por fotograma y no es parpadeo) */

static uint64_t hmix(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); h *= 0xff51afd7ed558ccdull; return h ^ (h >> 33); }

static uint64_t op_key(const GpuOp *o) {
    uint64_t h = 1469598103934665603ull;
    h = hmix(h, o->type); h = hmix(h, o->nv); h = hmix(h, o->flags & ~(GPU_F_DITHER)); h = hmix(h, o->abr); h = hmix(h, o->tp);
    h = hmix(h, o->tx | ((uint32_t)o->ty << 16)); h = hmix(h, o->clx | ((uint32_t)o->cly << 16));
    int nv = o->type == GPU_OP_POLY ? o->nv : 1;
    for (int k = 0; k < nv; k++) h = hmix(h, o->v[k].u | (o->v[k].v << 8) | (o->v[k].r << 16) | ((uint64_t)o->v[k].g << 24) | ((uint64_t)o->v[k].b << 32));
    return h ? h : 1;
}

static uint64_t op_key_nocol(const GpuOp *o) {
    uint64_t h = 88172645463325252ull;
    h = hmix(h, o->type); h = hmix(h, o->nv); h = hmix(h, o->flags & ~(GPU_F_DITHER)); h = hmix(h, o->abr); h = hmix(h, o->tp);
    h = hmix(h, o->tx | ((uint32_t)o->ty << 16)); h = hmix(h, o->clx | ((uint32_t)o->cly << 16));
    int nv = o->type == GPU_OP_POLY ? o->nv : 1;
    for (int k = 0; k < nv; k++) h = hmix(h, o->v[k].u | (o->v[k].v << 8));
    return h ? h : 1;
}

/* ------------------------------------------------------------------------------------------------ utilidades */
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }

/* clase FINAL de la primitiva: la que decidió vr_scene (destino `fate`, VR_FATE_*), no la sospecha por vértices: un cuadrilátero con un vértice sin posición que se reconstruye es 3D. */
static int classify_op(const GpuOp *o, int fate, int *cls) {
    int tex = (o->flags & GPU_F_TEXTURED) != 0, semi = (o->flags & GPU_F_SEMI) != 0;
    switch (o->type) {
    case GPU_OP_POLY: {
        int nv = o->nv == 4 ? 4 : 3, der = 0;
        for (int k = 0; k < nv; k++) der += o->v[k].vvalid && (o->v[k].vflags & GPU_VF_DERIVED);
        *cls = fate == VR_FATE_3D || fate == VR_FATE_RECON ? (der ? C_SPR : C_3D) : fate == VR_FATE_2D ? C_2D : C_PART;   /* C_PART: descartado por vr_scene (parcial, ambiguo, cercano, ...) */
        break;
    }
    case GPU_OP_RECT: *cls = C_RECT; break;
    case GPU_OP_LINE: *cls = C_LINE; break;
    default: *cls = C_EVT; return C_EVT * 4;
    }
    return *cls * 4 + tex * 2 + semi;
}

/* caja en coordenadas de imagen (relativas al área mostrada) */
static int op_box(const GpuOp *o, const GpuDisplay *d, OpBox *b) {
    int x0 = 1 << 20, y0 = 1 << 20, x1 = -(1 << 20), y1 = -(1 << 20);
    switch (o->type) {
    case GPU_OP_POLY: case GPU_OP_LINE: {
        int nv = o->type == GPU_OP_LINE ? 2 : (o->nv == 4 ? 4 : 3);
        for (int k = 0; k < nv; k++) {
            if (o->v[k].x < x0) x0 = o->v[k].x;
            if (o->v[k].x > x1) x1 = o->v[k].x;
            if (o->v[k].y < y0) y0 = o->v[k].y;
            if (o->v[k].y > y1) y1 = o->v[k].y;
        }
        if (o->type == GPU_OP_LINE) { x0--; y0--; }
        x1++; y1++;
        break;
    }
    case GPU_OP_RECT: x0 = o->v[0].x; y0 = o->v[0].y; x1 = x0 + o->rw; y1 = y0 + o->rh; break;
    case GPU_OP_FILL: case GPU_OP_UPLOAD: case GPU_OP_VCOPY: x0 = o->rx; y0 = o->ry; x1 = o->rx + o->rw; y1 = o->ry + o->rh; break;
    default: return 0;
    }
    if (o->type != GPU_OP_FILL && o->type != GPU_OP_UPLOAD && o->type != GPU_OP_VCOPY) {   /* recorte vigente (inclusivo) */
        if (x0 < o->cx1) x0 = o->cx1;
        if (y0 < o->cy1) y0 = o->cy1;
        if (x1 > o->cx2 + 1) x1 = o->cx2 + 1;
        if (y1 > o->cy2 + 1) y1 = o->cy2 + 1;
    }
    b->x0 = x0 - d->x; b->y0 = y0 - d->y; b->x1 = x1 - d->x; b->y1 = y1 - d->y;
    return b->x1 > b->x0 && b->y1 > b->y0;
}

static void strip_rows(uint8_t *img, int w, int h, const uint8_t *a, const uint8_t *b, const uint8_t *dif) {
    for (int y = 0; y < h; y++) {
        memcpy(img + ((size_t)y * w * 3) * 3, a + (size_t)y * w * 3, (size_t)w * 3);
        memcpy(img + ((size_t)y * w * 3 + w) * 3, b + (size_t)y * w * 3, (size_t)w * 3);
        memcpy(img + ((size_t)y * w * 3 + 2 * w) * 3, dif + (size_t)y * w * 3, (size_t)w * 3);
    }
}

/* ------------------------------------------------------------------------------------------------ análisis de una lista (hilo de emulación) */
static RefR *rr;
static uint16_t *vA, *vB;
static uint32_t prev_idx, have_prev;
static uint8_t *u_ref, *u_cmp, *u_dif;
static uint8_t g_fate[1 << 18];            /* destino de cada primitiva de la lista del HUD (vr_scene_set_fate) */
static uint32_t g_dropwhy[VR_FATE_COUNT];  /* descartes por motivo (solo primitivas 3D que se perdieron) */


/* ---- volcado y repetición de listas (iterar sobre el análisis sin volver a emular, que son ~25 min): `--dump a,b[,c,d...]` guarda cada lista de esos rangos (lista capturada, qué primitivas son
 * del radar, estado del puente, opciones de escena y la VRAM del inicio de la primera de cada rango); `--replay DIR` las vuelve a analizar con el código actual de vr_scene/hud_emu. ---- */
#define DUMP_MAGIC 0x32445044u
typedef struct { uint32_t magic, index, host_frame, nops, ndata, has_vram; GpuDisplay disp; BridgeState bs; VrSceneOpts o; int ffa, kind, mode, reason; } DumpHdr;
static int g_dump_r[16][2], g_ndump;
static int g_last_dumped = -100;
static uint16_t *vS;
static int g_dump_kind[8], g_dump_max = 800, g_dump_cnt;   /* --dump-kind 1,2: toda lista de esas clases de vista (hasta --dump-max) */
static int want_dump(uint32_t idx) {
    for (int i = 0; i < g_ndump; i++) if ((int)idx >= g_dump_r[i][0] && (int)idx <= g_dump_r[i][1]) return 1;
    if (g_dump_kind[g_gs.view_kind & 7] && g_dump_cnt < g_dump_max) return 1;
    return 0;
}
static void dump_list(const GpuDrawList *full, const uint8_t *israd, const VrSceneOpts *o, const BridgeState *bs, const VrScene *sc, const uint16_t *start) {
    char pp[700]; snprintf(pp, sizeof pp, "%s/dump_%05u.bin", g_out, full->index);
    FILE *fo = fopen(pp, "wb");
    if (!fo) return;
    DumpHdr h; memset(&h, 0, sizeof h);
    h.magic = DUMP_MAGIC; h.index = full->index; h.host_frame = full->host_frame; h.nops = full->nops; h.ndata = full->ndata;
    h.has_vram = (int)full->index != g_last_dumped + 1 || full->vram_init != NULL; h.disp = full->disp; h.bs = *bs; h.o = *o; h.ffa = sc->filter.active;
    h.kind = g_gs.view_kind; h.mode = g_gs.view_mode; h.reason = g_gs.view_reason;
    fwrite(&h, sizeof h, 1, fo);
    fwrite(full->ops, sizeof(GpuOp), full->nops, fo);
    fwrite(israd, 1, full->nops, fo);
    fwrite(full->data, 2, full->ndata, fo);
    if (h.has_vram) fwrite(full->vram_init ? full->vram_init : start, 2, 1024 * 512, fo);
    fclose(fo);
    g_last_dumped = (int)full->index; g_dump_cnt++;
}

static void tap(const GpuDrawList *full, const GpuDrawList *hud, const VrScene *sc, const VrSceneOpts *o, const BridgeState *bs) {
    if (nfr >= MAXF) return;
    if (!vA) {
        if (!rr) { RefOpts ro = {1, 1, 0, 0.0f}; rr = ref_create(&ro); }
        vA = malloc(1024 * 512 * 2); vB = malloc(1024 * 512 * 2);
        u_ref = malloc(MAXW * MAXH * 3); u_cmp = malloc(MAXW * MAXH * 3); u_dif = malloc(MAXW * MAXH * 3);
    }
    if (full->host_frame < g_from) {
        if (!rr) { RefOpts ro = {1, 1, 0, 0.0f}; rr = ref_create(&ro); }
        ref_apply(rr, full, 0);
        return;
    }
    FRec *fr = &FR[nfr];
    memset(fr, 0, sizeof *fr);
    fr->idx = full->index; fr->hf = full->host_frame; fr->nops = full->nops;
    fr->lost = have_prev && full->index > prev_idx + 1 ? full->index - prev_idx - 1 : 0;
    prev_idx = full->index; have_prev = 1;

    /* VRAM al inicio de la lista, antes de que ref_apply la avance */
    const uint16_t *start = full->vram_init ? full->vram_init : ref_vram(rr);
    memcpy(vA, start, 1024 * 512 * 2); memcpy(vB, start, 1024 * 512 * 2);
    if ((g_ndump || g_dump_cnt < g_dump_max) && want_dump(full->index)) { if (!vS) vS = malloc(1024 * 512 * 2); memcpy(vS, start, 1024 * 512 * 2); }
    ref_apply(rr, full, 1);
    int W, H;
    const uint8_t *real = ref_present(rr, &full->disp, &W, &H);
    if (!real || W <= 0 || H <= 0 || W > MAXW || H > MAXH) return;
    fr->w = W; fr->h = H; fr->has3d = sc->has_3d; fr->filt = sc->filter.active; fr->user_space = sc->user_space;
    memcpy(u_ref, real, (size_t)W * H * 3);
    fr->kind = g_gs.view_kind; fr->mode = g_gs.view_mode; fr->reason = g_gs.view_reason;
    {   /* lo que el hardware no muestra (columnas fuera del rango del display: basura de VRAM vecina) no se compara */
        int vx0 = full->disp.pad_l, vx1 = full->disp.pad_l + (full->disp.aw > 0 ? full->disp.aw : W);
        if (vx1 > W) vx1 = W;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) if (x < vx0 || x >= vx1) u_ref[(y * W + x) * 3] = u_ref[(y * W + x) * 3 + 1] = u_ref[(y * W + x) * 3 + 2] = 0;
    }
    int is24 = full->disp.depth24;
    fr->video = is24;

    /* ---- composición ---- */
    HudImg i3 = hud_img_new(W, H), i2 = hud_img_new(W, H);
    float *zb = malloc((size_t)W * H * sizeof *zb);
    for (int i = 0; i < W * H; i++) i3.rgba[i * 4 + 3] = 1.0f;
    float proj_h = sc->filter.proj_h > 0 ? sc->filter.proj_h : 160.0f;
    float cx = sc->filter.proj_h > 0 ? sc->filter.cx_col : W * 0.5f, cy = sc->filter.proj_h > 0 ? sc->filter.cy_row : H * 0.5f;
    float view[16], tn[4] = {cx / proj_h, ((float)W - cx) / proj_h, cy / proj_h, ((float)H - cy) / proj_h};
    if (sc->has_3d && hud_emu_game_view_gl(sc, view)) hud_emu_run3d(sc, vA, &i3, zb, 1, view, tn);
    else hud_emu_run3d(sc, vA, NULL, zb, 0, view, tn);
    hud_emu_run(sc, vB, &i2, full->disp.x, full->disp.y);
    float *bs3 = malloc((size_t)W * H * 3 * sizeof *bs3), *bd3 = malloc((size_t)W * H * 3 * sizeof *bd3);
    for (int i = 0; i < W * H; i++) for (int k = 0; k < 3; k++) bs3[i * 3 + k] = i3.rgba[i * 4 + k];
    if (sc->filter.active && W == sc->filter.area[2] && H == sc->filter.area[3]) hud_emu_post2(bs3, bd3, W, H, cx, cy, proj_h, proj_h, &sc->filter);
    else memcpy(bd3, bs3, (size_t)W * H * 3 * sizeof *bd3);
    for (int i = 0; i < W * H; i++) {
        float a = i2.rgba[i * 4 + 3];
        for (int k = 0; k < 3; k++) {
            float v = i2.rgba[i * 4 + k] + (1.0f - a) * bd3[i * 3 + k];
            v = v < 0 ? 0 : v > 1 ? 1 : v;
            u_cmp[i * 3 + k] = (uint8_t)(v * 255.0f + 0.5f);
        }
    }
    hud_img_free(&i3); hud_img_free(&i2); free(zb); free(bs3); free(bd3);
    {
        int vx0 = full->disp.pad_l, vx1 = full->disp.pad_l + (full->disp.aw > 0 ? full->disp.aw : W);
        if (vx1 > W) vx1 = W;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) if (x < vx0 || x >= vx1) u_cmp[(y * W + x) * 3] = u_cmp[(y * W + x) * 3 + 1] = u_cmp[(y * W + x) * 3 + 2] = 0;
    }

    /* ---- cajas de primitivas ---- */
    uint32_t n = full->nops;
    OpBox *bx = malloc((size_t)(n ? n : 1) * sizeof *bx);
    uint8_t *israd = calloc(n ? n : 1, 1);
    {   /* radar: operaciones que están en `full` y no en `hud` (mismo orden; se emparejan por seq) */
        uint32_t j = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (j < hud->nops && hud->ops[j].seq == full->ops[i].seq) j++;
            else if (full->ops[i].type == GPU_OP_POLY || full->ops[i].type == GPU_OP_RECT || full->ops[i].type == GPU_OP_LINE) israd[i] = 1;
        }
    }
    if ((g_ndump || g_dump_cnt < g_dump_max) && want_dump(full->index)) dump_list(full, israd, o, bs, sc, vS);
    uint32_t nrad = 0, hj = 0;   /* hj: índice de la primitiva en la lista del HUD (sin radar), donde vr_scene anota el destino */
    for (uint32_t i = 0; i < n; i++) {
        const GpuOp *p = &full->ops[i];
        int fate = israd[i] ? VR_FATE_NONE : (hj < sizeof g_fate ? g_fate[hj] : VR_FATE_NONE);
        if (!israd[i]) hj++;
        int cls, t = classify_op(p, fate, &cls);
        if (p->type == GPU_OP_POLY && cls == C_PART && fate < VR_FATE_COUNT) g_dropwhy[fate]++;
        bx[i].type = israd[i] ? T_RADAR : t; bx[i].cls = cls;
        if (!op_box(p, &full->disp, &bx[i])) { bx[i].x1 = bx[i].x0; bx[i].y1 = bx[i].y0; }
        nrad += israd[i];
        if ((p->type == GPU_OP_UPLOAD || p->type == GPU_OP_VCOPY) && bx[i].x1 > bx[i].x0) {
            int ix0 = bx[i].x0 < 0 ? 0 : bx[i].x0, iy0 = bx[i].y0 < 0 ? 0 : bx[i].y0, ix1 = bx[i].x1 > W ? W : bx[i].x1, iy1 = bx[i].y1 > H ? H : bx[i].y1;
            if (ix1 > ix0 && iy1 > iy0) fr->cpy_disp += (uint32_t)((ix1 - ix0) * (iy1 - iy0));
        }
        /* claves y cambios de clasificación (solo polígonos con posible doble lectura) */
        if (p->type == GPU_OP_POLY && !israd[i]) {
            int cc = cls == C_3D || cls == C_SPR ? 0 : cls == C_2D ? 1 : 2;
            if (cc == 2) {
                int nvv = p->nv == 4 ? 4 : 3, m = 0; unsigned uf = 0;
                for (int q = 0; q < nvv; q++) if (p->v[q].vvalid) { m |= 1 << q; uf |= p->v[q].vflags; }
                PS.n[nvv == 4][m]++; PS.vf[uf & 15]++; PS.semi += (p->flags & GPU_F_SEMI) != 0; PS.tex += (p->flags & GPU_F_TEXTURED) != 0; PS.fbk += p->fbk != 0;
                int bw = bx[i].x1 - bx[i].x0, bh = bx[i].y1 - bx[i].y0;
                if (bw * bh > 2000) PS.big++; else PS.small++;
            }
            /* misma primitiva (sin colores) ya presente en la lista anterior: la variación de color no es un fotograma alterno */
            int in_prev = 0;
            {
                uint64_t k2 = op_key_nocol(p);
                Key *e2 = &KTAB2[k2 & (KT - 1)];
                for (int pr = 0; pr < 8 && e2->key && e2->key != k2; pr++) e2 = &KTAB2[(k2 + pr + 1) & (KT - 1)];
                if (e2->key != k2) { memset(e2, 0, sizeof *e2); e2->key = k2; }
                if (e2->last != full->index) { e2->lvis = e2->last; e2->last = full->index; }
                in_prev = e2->lvis + 1 == full->index;
            }
            uint64_t k = op_key(p);
            Key *e = &KTAB[k & (KT - 1)];
            for (int pr = 0; pr < 8 && e->key && e->key != k; pr++) e = &KTAB[(k + pr + 1) & (KT - 1)];
            if (e->key != k) { memset(e, 0, sizeof *e); e->key = k; }
            else {
                if (e->last == full->index) { /* dos usos de la misma clave en la lista: no se compara */ }
                else if (e->last + 1 == full->index && e->lastcls != cc) {
                    fr->flips[e->lastcls][cc]++;
                    if (cc == 2 || e->lastcls == 2) { int nvv = p->nv == 4 ? 4 : 3, m = 0; for (int q = 0; q < nvv; q++) if (p->v[q].vvalid) m |= 1 << q; if (cc == 2) PS.flip_pat[nvv == 4][m]++; PS.nf++; }
                }
                if (e->last != full->index && cc != 2 && e->lvis + 2 == full->index && e->last + 2 == full->index && !in_prev && (bx[i].x1 - bx[i].x0) * (bx[i].y1 - bx[i].y0) >= 100) {   /* solo las que se ven: < 100 px2 son polígonos lejanos que el propio juego alterna */
                    fr->alt[cls]++;
                    if (g_trace_alt > 0 && (fr->kind == 1 || fr->kind == 2)) {
                        g_trace_alt--;
                        fprintf(stderr, "[alt] lista %u op %u %s clase=%s fl=%03x abr=%d tex=(%d,%d) clut=(%d,%d) caja=(%d,%d)-(%d,%d) uv0=(%d,%d) vv=%x destino=%d\n", full->index, i, p->type == GPU_OP_POLY ? "POLY" : "OTRO", CLS_NAME[cls],
                                p->flags, p->abr, p->tx, p->ty, p->clx, p->cly, bx[i].x0, bx[i].y0, bx[i].x1, bx[i].y1, p->v[0].u, p->v[0].v, p->v[0].vvalid | (p->v[1].vvalid << 1) | (p->v[2].vvalid << 2) | (p->v[3].vvalid << 3), fate);
                    }
                }   /* visible en t-2, ausente en t-1, visible en t */
            }
            e->last = full->index; e->lastcls = (uint8_t)cc;
            if (cc != 2) e->lvis = full->index;
        }
    }
    fr->nrad = nrad;
    if (g_ops_a >= 0 && (int)full->index >= g_ops_a && (int)full->index <= g_ops_b) {
        char pp[700]; snprintf(pp, sizeof pp, "%s/ops_%05u.txt", g_out, full->index);
        FILE *fo = fopen(pp, "w");
        if (fo) {
            fprintf(fo, "lista %u vblank %u disp %d,%d %dx%d aw=%d pad=%d d24=%d | vista kind=%d modo=%d motivo=%d | 3D=%d filtro=%d\n", full->index, full->host_frame, full->disp.x, full->disp.y, full->disp.w, full->disp.h, full->disp.aw, full->disp.pad_l, full->disp.depth24,
                    fr->kind, fr->mode, fr->reason, sc->has_3d, sc->filter.active);
            uint32_t hj2 = 0;
            for (uint32_t i = 0; i < n; i++) {
                const GpuOp *q = &full->ops[i];
                int fate2 = israd[i] ? -1 : (hj2 < sizeof g_fate ? g_fate[hj2] : 0);
                if (!israd[i]) hj2++;
                static const char *tn_[] = {"POLY", "RECT", "LINE", "FILL", "VCOPY", "UPLOAD", "DOWNLOAD"};
                int nvv = q->type == GPU_OP_POLY ? q->nv : 1, vm = 0;
                for (int k = 0; k < nvv; k++) vm |= q->v[k].vvalid << k;
                fprintf(fo, "%4u %-6s fl=%03x abr=%d tp=%d tex=(%d,%d) clut=(%d,%d) caja=(%d,%d)-(%d,%d) recorte=(%d,%d)-(%d,%d) off=(%d,%d) vv=%x destino=%d%s fbk=%d\n", i, tn_[q->type < 7 ? q->type : 0], q->flags, q->abr, q->tp, q->tx, q->ty, q->clx, q->cly,
                        bx[i].x0, bx[i].y0, bx[i].x1, bx[i].y1, q->cx1, q->cy1, q->cx2, q->cy2, q->dox, q->doy, vm, fate2, israd[i] ? " (radar)" : "", q->fbk);
            }
            fclose(fo);
        }
    }

    /* ---- diferencias ----
     * Se comparan medias de ventanas de g_win x g_win píxeles (la escena 3D usa perspectiva correcta, sin tramado ni redondeo de vértices de la consola: los píxeles sueltos
     * nunca coinciden) y cada píxel de la composición se empareja con el mejor del vecindario (g_rad) del fotograma real, de modo que un borde desplazado un píxel no cuenta.
     * Lo que sí cuenta: geometría/texto/efectos que faltan, sobran o salen en otro sitio o color. */
    memset(u_dif, 0, (size_t)W * H * 3);
    if (!is24) {
        static float mr[MAXW * MAXH * 3], mc[MAXW * MAXH * 3];
        int wn = g_win;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                for (int k = 0; k < 3; k++) {
                    float sr = 0, sc2 = 0; int cnt = 0;
                    for (int dy = 0; dy < wn; dy++)
                        for (int dx = 0; dx < wn; dx++) {
                            int xx = x + dx - wn / 2, yy = y + dy - wn / 2;
                            if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
                            sr += u_ref[(yy * W + xx) * 3 + k]; sc2 += u_cmp[(yy * W + xx) * 3 + k]; cnt++;
                        }
                    mr[(y * W + x) * 3 + k] = sr / cnt; mc[(y * W + x) * 3 + k] = sc2 / cnt;
                }
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                float d = 1e9f;
                for (int dy = -g_rad; dy <= g_rad && d > g_tol; dy++)
                    for (int dx = -g_rad; dx <= g_rad && d > g_tol; dx++) {
                        int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
                        float m = 0;
                        for (int k = 0; k < 3; k++) { float a = fabsf(mr[(yy * W + xx) * 3 + k] - mc[(y * W + x) * 3 + k]); if (a > m) m = a; }
                        if (m < d) d = m;
                    }
                if (d <= g_tol) continue;
                int ty = T_NONE;
                for (int i = (int)n - 1; i >= 0; i--) {
                    if (x >= bx[i].x0 && x < bx[i].x1 && y >= bx[i].y0 && y < bx[i].y1) { ty = bx[i].type; break; }
                }
                fr->mism[ty]++; if (ty != T_RADAR) fr->total++;
                u_dif[(y * W + x) * 3] = 255; u_dif[(y * W + x) * 3 + 1] = (uint8_t)(d > 255 ? 255 : d) / 2;
            }
    }
    for (int t = 0; t < NT; t++) {   /* peores por tipo (solo vistas que usan esta composición: mundo y cinemática del motor) */
        if (fr->mism[t] < (uint32_t)g_px_thr || !(fr->kind == 1 || fr->kind == 2)) continue;
        Worst *w = &WST[t];
        if (fr->mism[t] > w->score) {
            w->score = fr->mism[t]; w->idx = full->index; w->w = W; w->h = H;
            free(w->img);
            w->img = malloc((size_t)W * 3 * H * 3);
            if (w->img) strip_rows(w->img, W, H, u_ref, u_cmp, u_dif);
        }
    }
    if (g_seq_a >= 0 && (int)full->index >= g_seq_a && (int)full->index <= g_seq_b) {
        char p[700]; snprintf(p, sizeof p, "%s/seq_%05u.png", g_out, full->index);
        uint8_t *img = malloc((size_t)W * 3 * H * 3);
        if (img) { strip_rows(img, W, H, u_ref, u_cmp, u_dif); pr_write_png(p, W * 3, H, img); free(img); }
    }
    free(bx); free(israd);
    nfr++;
}


#include <dirent.h>
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void run_replay(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) { fprintf(stderr, "no se puede abrir %s\n", dir); return; }
    char **names = NULL; size_t nn = 0, cap = 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (strncmp(e->d_name, "dump_", 5)) continue;
        if (nn == cap) { cap = cap ? cap * 2 : 256; names = realloc(names, cap * sizeof *names); }
        names[nn++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, nn, sizeof *names, cmp_str);
    for (size_t k = 0; k < nn; k++) {
        char pp[900]; snprintf(pp, sizeof pp, "%s/%s", dir, names[k]);
        FILE *f = fopen(pp, "rb");
        if (!f) continue;
        DumpHdr h;
        if (fread(&h, sizeof h, 1, f) != 1 || h.magic != DUMP_MAGIC) { fclose(f); continue; }
        GpuOp *ops = malloc((size_t)(h.nops ? h.nops : 1) * sizeof *ops);
        uint8_t *rad = malloc(h.nops ? h.nops : 1);
        uint16_t *data = malloc((size_t)(h.ndata ? h.ndata : 1) * 2), *vr = h.has_vram ? malloc(1024 * 512 * 2) : NULL;
        int ok = ops && rad && data && fread(ops, sizeof *ops, h.nops, f) == h.nops && fread(rad, 1, h.nops, f) == h.nops && fread(data, 2, h.ndata, f) == h.ndata &&
                 (!vr || fread(vr, 2, 1024 * 512, f) == 1024 * 512);
        fclose(f);
        if (ok) {
            GpuDrawList full; memset(&full, 0, sizeof full);
            full.index = h.index; full.host_frame = h.host_frame; full.disp = h.disp; full.ops = ops; full.nops = h.nops; full.cap_ops = h.nops; full.data = data; full.ndata = h.ndata; full.cap_data = h.ndata; full.vram_init = vr;
            GpuDrawList hud = full; hud.vram_init = NULL;
            GpuOp *hops = malloc((size_t)(h.nops ? h.nops : 1) * sizeof *hops);
            uint32_t nh = 0;
            for (uint32_t i = 0; i < h.nops; i++) if (!rad[i]) hops[nh++] = ops[i];
            hud.ops = hops; hud.nops = nh; hud.cap_ops = nh;
            VrScene sc;
            if (vr_scene_build(&hud, &h.bs, &h.o, &sc) == 0) {
                if (h.o.screen_filter && sc.filter.active && !h.ffa) { VrSceneOpts o2 = h.o; o2.screen_filter = 0; vr_scene_build(&hud, &h.bs, &o2, &sc); }
                g_gs.view_kind = h.kind; g_gs.view_mode = h.mode; g_gs.view_reason = h.reason;
                tap(&full, &hud, &sc, &h.o, &h.bs);
            }
            free(hops);
        }
        free(ops); free(rad); free(data); free(vr); free(names[k]);
    }
    free(names);
}

/* ------------------------------------------------------------------------------------------------ guion de mando */
typedef struct { uint32_t start, dur, every; uint16_t mask; } PadEv;
static const PadEv PADB[] = {{120, 6, 0, 1u << 3}, {400, 6, 0, 1u << 3}, {0, 6, 97, 1u << 13}, {0, 6, 211, 1u << 7}};
static uint16_t boot_pad(uint32_t f) {
    uint16_t b = 0;
    for (unsigned i = 0; i < sizeof PADB / sizeof PADB[0]; i++) {
        const PadEv *p = &PADB[i];
        if (p->every) { if (f >= p->every && f % p->every < p->dur) b |= p->mask; }
        else if (f >= p->start && f < p->start + p->dur) b |= p->mask;
    }
    return b;
}
/* partida: ciclo de 1500 VBlanks (andar, girar, disparar con el cuadrado, ventanas L1/R1, agacharse, ráfagas) */
static uint16_t play_pad(uint32_t k, uint8_t *lx, uint8_t *ly) {
    uint32_t t = k % 1500;
    uint16_t b = 0;
    *lx = *ly = 128;
    if (t < 160) b |= 1u << 4;
    else if (t < 210) b |= 1u << 5;
    else if (t < 360) b |= 1u << 4;
    else if (t < 480) { if (t % 12 < 5) b |= 1u << 15; }
    else if (t < 560) b |= (1u << 10);                         /* L1 mantenido */
    else if (t < 640) b |= (1u << 11);                         /* R1 mantenido */
    else if (t < 700) { if (t % 20 < 6) b |= 1u << 14; }
    else if (t < 900) { b |= 1u << 6; if (t % 60 < 30) b |= 1u << 7; }
    else if (t < 1000) { b |= 1u << 4; if (t % 15 < 5) b |= 1u << 15; }
    else if (t < 1100) b |= 1u << 12;
    else if (t < 1300) { *lx = (t / 40) % 2 ? 255 : 0; *ly = 40; }
    else if (t < 1400) { b |= 1u << 5; if (t % 10 < 4) b |= 1u << 15; }
    return b;
}

static _Atomic int g_done;
static int boot_ok = 1;
static void hook(void *cpu, uint32_t f) {
    (void)cpu;
    static int stable;
    static uint32_t ctl_f;
    mv_vrhost_game_state(&g_gs);
    int ctl = g_gs.game && g_gs.in_control && g_gs.cam_active;
    uint8_t lx = 128, ly = 128;
    uint16_t pb;
    if (stable > 90) {
        pb = g_partida ? play_pad(f - ctl_f, &lx, &ly) : 0;
    } else {
        pb = ctl && stable > 5 ? 0 : boot_pad(f);
        stable = ctl ? stable + 1 : 0;
        if (stable > 90) ctl_f = f;
    }
    mv_host_pad(pb, lx, ly, 128, 128);
    if (g_head) {   /* cabeza sintética: sin ella la cámara VR no se activa y la vista es siempre la pantalla de cine. Guiñada lenta de +-35 grados, de pie a 1,6 m */
        float yw = (float)(35.0 * sin((double)f * 0.004)) * 0.01745329252f, cyw = cosf(yw), syw = sinf(yw);
        float m[16] = {cyw, 0, -syw, 0, 0, 1, 0, 0, syw, 0, cyw, 0, 0, 1.6f, 0, 1};
        mv_vr_set_head(m, (double)f * 0.02);
    }
    if (f >= g_frames) atomic_store(&g_done, 1);
    if (f > 20000 && stable <= 90) { boot_ok = 0; atomic_store(&g_done, 1); }
}

/* ------------------------------------------------------------------------------------------------ informe */
static FILE *g_rep;
#define REP(...) do { printf(__VA_ARGS__); if (g_rep) fprintf(g_rep, __VA_ARGS__); } while (0)

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: MV_SIGS=firmas.json %s <exe> <disco> [--dump a,b] [--replay DIR] [--out DIR] [--frames N] [--sin-partida] [--tol T] [--umbral-px N] [--seq a,b] [--th nombre=valor]\n", argv[0]); return 2; }
    const char *exe = argv[1], *disc = argv[2];
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) snprintf(g_out, sizeof g_out, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) g_frames = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sin-partida")) g_partida = 0;
        else if (!strcmp(argv[i], "--sin-cabeza")) g_head = 0;
        else if (!strcmp(argv[i], "--trazar-alternos") && i + 1 < argc) g_trace_alt = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tol") && i + 1 < argc) g_tol = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) {
            char *d = strdup(argv[++i]), *sv = NULL; int v[2], k = 0;
            for (char *t = strtok_r(d, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) { v[k++ & 1] = atoi(t); if (!(k & 1) && g_ndump < 16) { g_dump_r[g_ndump][0] = v[0]; g_dump_r[g_ndump][1] = v[1]; g_ndump++; } }
            free(d);
        }
        else if (!strcmp(argv[i], "--dump-kind") && i + 1 < argc) { char *d = strdup(argv[++i]), *sv = NULL; for (char *t = strtok_r(d, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) g_dump_kind[atoi(t) & 7] = 1; free(d); }
        else if (!strcmp(argv[i], "--dump-max") && i + 1 < argc) g_dump_max = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--replay") && i + 1 < argc) g_replay = argv[++i];
        else if (!strcmp(argv[i], "--ops") && i + 1 < argc) { sscanf(argv[++i], "%d,%d", &g_ops_a, &g_ops_b); }
        else if (!strcmp(argv[i], "--ventana") && i + 1 < argc) g_win = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--radio") && i + 1 < argc) g_rad = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--desde") && i + 1 < argc) g_from = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--umbral-px") && i + 1 < argc) g_px_thr = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seq") && i + 1 < argc) { sscanf(argv[++i], "%d,%d", &g_seq_a, &g_seq_b); }
        else if (!strcmp(argv[i], "--th") && i + 1 < argc) {
            char *kv = argv[++i], *eq = strchr(kv, '=');
            int ok = 0;
            if (eq) { *eq = 0; for (unsigned k = 0; k < sizeof THN / sizeof THN[0]; k++) if (!strcmp(THN[k].n, kv)) { *THN[k].p = atof(eq + 1); ok = 1; } }
            if (!ok) { fprintf(stderr, "umbral desconocido: %s\n", kv); return 2; }
        }
    }
    mkdir(g_out, 0755);
    char p[700];
    snprintf(p, sizeof p, "%s/informe.txt", g_out); g_rep = fopen(p, "w");
    FR = calloc(MAXF, sizeof *FR);
    if (getenv("MV_SIGS")) mv_host_set_sigs(getenv("MV_SIGS"));
    char card[700]; snprintf(card, sizeof card, "%s/card", g_out); mkdir(card, 0755);
    vr_scene_set_fate(g_fate, sizeof g_fate);
    double t0 = now_s();
    if (g_replay) { run_replay(g_replay); }
    else {
    mv_vrhost_install();
    mv_vrhost_set_test_hook(hook);
    mv_vrhost_set_list_tap(tap);
    mv_host_set_turbo(1);
    int r = mv_host_start(exe, disc, card);
    if (r) { fprintf(stderr, "mv_host_start = %d: %s\n", r, mv_host_error()); return 2; }
    while (!atomic_load(&g_done)) {
        int got = 0;
        for (int k = 0; k < 8; k++) { VrScene sc; if (!mv_host_scene(&sc)) break; got++; }
        if (mv_host_state() == MV_HOST_ERROR) { fprintf(stderr, "error del motor: %s\n", mv_host_error()); break; }
        if (!got) usleep(300);
    }
    mv_host_pause();
    usleep(300000);

    }

    /* ---------------------------------------------------------------- análisis temporal
     * Por grupo de vista: 0 = otras (2D, menús, vídeo: en el visor van por la pantalla de cine y esta composición no se usa), 1 = mundo (primera persona), 2 = cinemática del motor (pantalla 3D). */
    enum { NG = 3 };
    static const char *GN[NG] = {"otras (2D/vídeo: pantalla de cine)", "mundo (primera persona)", "cinemática del motor (pantalla 3D)"};
    static uint32_t tot[NG][NT], fbad[NG][NT], flips[NG][3][3], alt[NG][C_NCLS], nbad[NG], nisol[NG], nfm[NG], flip_frames[NG], alt_frames[NG], cpyf[NG], lost = 0, nvid = 0, n3d = 0;
    FILE *csv; snprintf(p, sizeof p, "%s/frames.csv", g_out); csv = fopen(p, "w");
    if (csv) fprintf(csv, "lista,vblank,grupo,kind,modo,motivo,w,h,3d,filtro,discrepantes,perdidas,cambios_clase,alternos,radar,copias_a_pantalla\n");
    for (uint32_t i = 0; i < nfr; i++) {
        FRec *f = &FR[i];
        if (!f->w) continue;
        int g = f->kind == 1 ? 1 : f->kind == 2 ? 2 : 0;
        nfm[g]++; nvid += f->video; lost += f->lost; n3d += f->has3d;
        uint32_t fl = 0, al = 0;
        for (int a2 = 0; a2 < 3; a2++) for (int b2 = 0; b2 < 3; b2++) { flips[g][a2][b2] += f->flips[a2][b2]; if (a2 != b2) fl += f->flips[a2][b2]; }
        for (int c = 0; c < C_NCLS; c++) { alt[g][c] += f->alt[c]; al += f->alt[c]; }
        flip_frames[g] += fl > 0; alt_frames[g] += al > 0; cpyf[g] += f->cpy_disp > 0;
        for (int t = 0; t < NT; t++) { tot[g][t] += f->mism[t]; fbad[g][t] += f->mism[t] >= (uint32_t)g_px_thr; }
        nbad[g] += f->total >= (uint32_t)g_px_thr;
        if (csv) fprintf(csv, "%u,%u,%d,%d,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,%u,%u\n", f->idx, f->hf, g, f->kind, f->mode, f->reason, f->w, f->h, f->has3d, f->filt, f->total, f->lost, fl, al, f->nrad, f->cpy_disp);
        /* aislado: malo con los dos vecinos limpios (< la mitad) */
        if (i > 0 && i + 1 < nfr && f->total >= (uint32_t)g_px_thr && FR[i - 1].w && FR[i + 1].w && FR[i - 1].total * 2 < f->total && FR[i + 1].total * 2 < f->total) nisol[g]++;
    }
    if (csv) fclose(csv);

    REP("=== probador 2D/3D: %u listas (%u con 3D, %u de vídeo 24 bits sin comparar), %u perdidas, ventana %dx%d vecindario %d tolerancia %d, umbral %d px ===\n", nfm[0] + nfm[1] + nfm[2], n3d, nvid, lost, g_win, g_win, g_rad, g_tol, g_px_thr);
    REP("arranque título -> partida: %s\n", boot_ok ? "PASA" : "FALLA");
    static const char *why[VR_FATE_COUNT] = {"-", "3D", "2D", "parcial (algún vértice sin posición)", "ambiguo junto a saturado/detrás", "más cerca que el plano cercano", "detrás tras recortar", "arista enorme", "degenerado", "no finito", "derivado", "reconstruido"};
    const char *cn[3] = {"3D", "2D", "descartado"};
    uint32_t tflip[NG] = {0}, talt[NG] = {0};
    for (int g = 0; g < NG; g++) {
        REP("\n##### %s: %u listas #####\n", GN[g], nfm[g]);
        REP("DISCREPANCIAS (píxeles distintos del fotograma real, por tipo de la última primitiva que cubre el píxel)\n");
        REP("  %-24s %12s %10s\n", "tipo", "píxeles", "fotogramas");
        for (int t = 0; t < NT; t++) { char b[48]; if (tot[g][t]) REP("  %-24s %12u %10u\n", tname(t, b), tot[g][t], fbad[g][t]); }
        REP("  fotogramas con discrepancia >= umbral: %u de %u (%.1f%%); con copias/subidas de VRAM a la pantalla: %u\n", nbad[g], nfm[g], nfm[g] ? 100.0 * nbad[g] / nfm[g] : 0.0, cpyf[g]);
        for (int a2 = 0; a2 < 3; a2++) for (int b2 = 0; b2 < 3; b2++) if (a2 != b2 && flips[g][a2][b2]) { REP("  PARPADEO: cambio de clasificación %s -> %s de la misma primitiva: %u\n", cn[a2], cn[b2], flips[g][a2][b2]); tflip[g] += flips[g][a2][b2]; }
        REP("  cambios de clasificación (total): %u en %u fotogramas\n", tflip[g], flip_frames[g]);
        for (int c = 0; c < C_NCLS; c++) if (alt[g][c]) { REP("  PARPADEO: primitivas visibles en t-2 y t y ausentes en t-1 (clase %s): %u\n", CLS_NAME[c], alt[g][c]); talt[g] += alt[g][c]; }
        REP("  primitivas en fotogramas alternos (total): %u en %u fotogramas\n", talt[g], alt_frames[g]);
        REP("  fotogramas aislados con discrepancia alta entre vecinos limpios: %u\n", nisol[g]);
    }
    REP("\nPOLÍGONOS 3D DESCARTADOS POR vr_scene (motivo: cuántos; todo el recorrido)\n");
    for (int q = 3; q < VR_FATE_COUNT; q++) if (g_dropwhy[q] && q != VR_FATE_RECON) REP("  %-40s %u\n", why[q], g_dropwhy[q]);
    REP("\nPOLÍGONOS PARCIALES EN CRUDO (antes de reconstruir; patrón = vértices con posición de vista, bit0 = v0)\n");
    for (int q = 0; q < 2; q++) for (int m = 0; m < 16; m++) if (PS.n[q][m]) REP("  %s patrón %x: %u\n", q ? "quad" : "tri ", m, PS.n[q][m]);
    int fails = 0;
#define CHK(cond, ...) do { int ok_ = (cond); REP("%s: ", ok_ ? "PASA " : "FALLA"); REP(__VA_ARGS__); REP("\n"); if (!ok_) fails++; } while (0)
    REP("\nUMBRALES (mundo + cinemática del motor)\n");
    CHK(boot_ok, "arranque");
    CHK(tflip[1] + tflip[2] <= TH_flip, "cambios de clasificación 3D/2D/descartado = %u (máx %.0f)", tflip[1] + tflip[2], TH_flip);
    CHK(talt[1] + talt[2] <= TH_alt, "primitivas en fotogramas alternos = %u (máx %.0f)", talt[1] + talt[2], TH_alt);
    CHK(nisol[1] + nisol[2] <= TH_isol, "fotogramas aislados con discrepancia = %u (máx %.0f)", nisol[1] + nisol[2], TH_isol);
    CHK(nbad[1] + nbad[2] <= TH_bad, "fotogramas con discrepancia >= umbral = %u (máx %.0f)", nbad[1] + nbad[2], TH_bad);
    CHK(lost == 0, "listas perdidas = %u", lost);
    REP("duración %.0f s · veredicto: %s\n", now_s() - t0, fails ? "FALLA" : "PASA");
    for (int t = 0; t < NT; t++) {
        if (!WST[t].img) continue;
        char b[48]; tname(t, b);
        for (char *c = b; *c; c++) if (*c == '+') *c = '_';
        snprintf(p, sizeof p, "%s/peor_%s_lista%u.png", g_out, b, WST[t].idx);
        pr_write_png(p, WST[t].w * 3, WST[t].h, WST[t].img);
    }
    if (g_rep) fclose(g_rep);
    fflush(stdout);
    _exit(fails ? 1 : 0);
}
