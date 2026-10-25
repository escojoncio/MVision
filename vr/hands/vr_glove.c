/* vr_glove: ver vr_glove.h. */
#include "vr_glove.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RAM_MASK 0x1FFFFFu
#define RAM_SIZE 0x200000u
#define A_TEXTAB 0x800B30E0u
#define MF_SHARED_VERTS 0x10000u
#define MF_TWO_SIDED 0x400u          /* la capa del cuerpo lo activa durante el dibujado: no cuenta para el hash */
#define MDL_SIZE 0x58u

static uint32_t g_glove_id;

/* ------------------------------------------------------------------------------------------------------ lectura de la RAM */
static int rng_ok(uint32_t a, uint32_t len) { return (a >> 29) == 4u && (uint64_t)(a & RAM_MASK) + len <= RAM_SIZE; }
static uint8_t rd8(const uint8_t *r, uint32_t a) { return r[a & RAM_MASK]; }
static uint16_t rd16(const uint8_t *r, uint32_t a) { return (uint16_t)(rd8(r, a) | (rd8(r, a + 1) << 8)); }
static int16_t rs16(const uint8_t *r, uint32_t a) { return (int16_t)rd16(r, a); }
static uint32_t rd32(const uint8_t *r, uint32_t a) { return (uint32_t)rd16(r, a) | ((uint32_t)rd16(r, a + 2) << 16); }
static int32_t rs32(const uint8_t *r, uint32_t a) { return (int32_t)rd32(r, a); }

/* ------------------------------------------------------------------------------------------------------ hash (FNV-1a 64) */
static uint64_t hb(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
static uint64_t hu(uint64_t h, uint32_t v) { return hb(h, &v, 4); }

/* ------------------------------------------------------------------------------------------------------ regiones de textura */
typedef struct { uint16_t texid, tpage, clut; uint8_t ox, oy, w, h; int rw, rh, ax, ay; } Region;
typedef struct { Region r[VRGLOVE_MAXREG]; int n; } Regs;

/* Como tex_find de ns_extract.c: tabla de 512 entradas de 12 B, sondeo lineal desde id % 512. */
static uint32_t tex_find(const uint8_t *ram, uint16_t id) {
    if (!id) return 0;
    for (uint32_t k = 0; k < 512; k++) {
        uint32_t e = A_TEXTAB + 12u * (((uint32_t)id + k) % 512u);
        uint16_t eid = rd16(ram, e);
        if (eid == id) return e;
        if (eid == 0) return 0;
    }
    return 0;
}

/* Índice de la región de un texid (1..); 0 = sin textura (blanco). */
static int reg_get(Regs *rg, const uint8_t *ram, uint16_t texid) {
    for (int i = 1; i < rg->n; i++) if (rg->r[i].texid == texid) return i;
    uint32_t e = tex_find(ram, texid);
    if (!e || rg->n >= VRGLOVE_MAXREG) return 0;
    Region *R = &rg->r[rg->n];
    memset(R, 0, sizeof *R);
    R->texid = texid; R->tpage = rd16(ram, e + 4); R->clut = rd16(ram, e + 6);
    R->ox = rd8(ram, e + 8); R->oy = rd8(ram, e + 9); R->w = rd8(ram, e + 10); R->h = rd8(ram, e + 11);
    R->rw = (int)R->w + 1; if (R->ox + R->rw > 256) R->rw = 256 - R->ox;
    R->rh = (int)R->h + 1; if (R->oy + R->rh > 256) R->rh = 256 - R->oy;
    return rg->n++;
}

/* Color de un texel (u, v) de la página de textura: 4 u 8 bits con CLUT o 15 bits directo. 0x0000 = transparente. */
static void texel_rgba(const uint16_t *vram, uint16_t tpage, uint16_t clut, int u, int v, uint8_t out[4]) {
    int mode = (tpage >> 7) & 3, tx = (tpage & 15) * 64, ty = ((tpage >> 4) & 1) * 256;
    int cx = (clut & 63) * 16, cy = (clut >> 6) & 511;
    int row = ((ty + v) & 511) * 1024;
    uint16_t c;
    if (mode == 0) {
        uint16_t w = vram[row + ((tx + (u >> 2)) & 1023)];
        c = vram[cy * 1024 + ((cx + ((w >> ((u & 3) * 4)) & 15)) & 1023)];
    } else if (mode == 1) {
        uint16_t w = vram[row + ((tx + (u >> 1)) & 1023)];
        c = vram[cy * 1024 + ((cx + ((w >> ((u & 1) * 8)) & 255)) & 1023)];
    } else {
        c = vram[row + ((tx + u) & 1023)];
    }
    if (c == 0) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    out[0] = (uint8_t)((r << 3) | (r >> 2)); out[1] = (uint8_t)((g << 3) | (g >> 2)); out[2] = (uint8_t)((b << 3) | (b >> 2)); out[3] = 255;
}

/* Hash de lo que se leerá de la VRAM para una región: fila de CLUT y halfwords con los texels. */
static uint64_t hash_region_vram(uint64_t h, const uint16_t *vram, const Region *R) {
    int mode = (R->tpage >> 7) & 3, tx = (R->tpage & 15) * 64, ty = ((R->tpage >> 4) & 1) * 256;
    int cx = (R->clut & 63) * 16, cy = (R->clut >> 6) & 511;
    int n = mode == 0 ? 16 : (mode == 1 ? 256 : 0);
    for (int i = 0; i < n; i++) { uint16_t c = vram[cy * 1024 + ((cx + i) & 1023)]; h = hb(h, &c, 2); }
    int x0 = mode == 0 ? R->ox >> 2 : (mode == 1 ? R->ox >> 1 : R->ox);
    int x1 = mode == 0 ? (R->ox + R->rw - 1) >> 2 : (mode == 1 ? (R->ox + R->rw - 1) >> 1 : R->ox + R->rw - 1);
    for (int j = 0; j < R->rh; j++)
        for (int x = x0; x <= x1; x++) { uint16_t c = vram[(((ty + R->oy + j) & 511) * 1024) + ((tx + x) & 1023)]; h = hb(h, &c, 2); }
    return h;
}

/* ------------------------------------------------------------------------------------------------------ pieza */
/* parent_m = model* del padre dentro del conjunto (0 = el padre no está: sin soldar). mirror = construye el ESPEJO en X de la pieza (x de vértices y normales cambiada de signo, esquinas de cada cara
 * en orden inverso A,D,C,B para conservar el winding, con sus mismas UV/texturas; sin soldaduras: las pone quien lo pide). 0 = bien (pieza vacía si no tiene caras); -2 ilegible; -3 demasiado grande. */
static int parse_piece(const uint8_t *ram, uint32_t m, uint32_t parent_m, VrGlovePiece *P, Regs *rg, uint64_t *hash, int mirror) {
    memset(P, 0, sizeof *P);
    memset(P->weld, 0xFF, sizeof P->weld);
    if (!rng_ok(m, MDL_SIZE)) return -2;
    uint32_t mflag = rd32(ram, m);
    int32_t nf = rs32(ram, m + 4), nv = rs32(ram, m + 0x34), nn = rs32(ram, m + 0x40);
    uint32_t pv = rd32(ram, m + 0x38), pvi = rd32(ram, m + 0x3C), pn = rd32(ram, m + 0x44), pni = rd32(ram, m + 0x48), puv = rd32(ram, m + 0x4C), ptx = rd32(ram, m + 0x50);
    *hash = hu(*hash, mflag & ~MF_TWO_SIDED);
    if (nf == 0) return 0;
    if (nf < 0 || nv < 1 || nn < 0) return -2;
    if (nf > VRGLOVE_MAXF || nv > VRGLOVE_MAXV || nn > VRGLOVE_MAXV) return -3;
    if (!rng_ok(pv, (uint32_t)nv * 8u) || !rng_ok(pvi, (uint32_t)nf * 4u) || !rng_ok(puv, (uint32_t)nf * 8u) || !rng_ok(ptx, (uint32_t)nf * 2u)) return -2;
    if (nn > 0 && (!rng_ok(pn, (uint32_t)nn * 8u) || !rng_ok(pni, (uint32_t)nf * 4u))) return -2;
    const int shared = (mflag & MF_SHARED_VERTS) != 0;
    const uint8_t imask = shared ? 0x7F : 0xFF;
    P->nv = nv; P->nn = nn; P->nf = nf;
    for (int i = 0; i < nv; i++) for (int k = 0; k < 4; k++) P->v[4 * i + k] = rs16(ram, pv + 8u * (uint32_t)i + 2u * (uint32_t)k);
    for (int i = 0; i < nn; i++) for (int k = 0; k < 4; k++) P->n[4 * i + k] = rs16(ram, pn + 8u * (uint32_t)i + 2u * (uint32_t)k);
    for (int i = 0; i < nf * 4; i++) P->vidx[i] = rd8(ram, pvi + (uint32_t)i);
    if (nn > 0) for (int i = 0; i < nf * 4; i++) P->nidx[i] = rd8(ram, pni + (uint32_t)i);
    /* esquina de origen de cada esquina del espejo (cuadrilátero A,D,C,B; triángulo A,C,B,B) */
    static const int QPERM[4] = {0, 3, 2, 1}, TPERM[4] = {0, 2, 1, 1};
    if (mirror) {
        for (int i = 0; i < nv; i++) P->v[4 * i] = (int16_t)(P->v[4 * i] == -32768 ? 32767 : -P->v[4 * i]);
        for (int i = 0; i < nn; i++) P->n[4 * i] = (int16_t)(P->n[4 * i] == -32768 ? 32767 : -P->n[4 * i]);
        for (int f = 0; f < nf; f++) {
            const int *pm = (P->vidx[4 * f + 2] & imask) == (P->vidx[4 * f + 3] & imask) ? TPERM : QPERM;
            uint8_t a[4], b[4];
            for (int k = 0; k < 4; k++) { a[k] = P->vidx[4 * f + pm[k]]; b[k] = P->nidx[4 * f + pm[k]]; }
            for (int k = 0; k < 4; k++) { P->vidx[4 * f + k] = a[k]; if (nn > 0) P->nidx[4 * f + k] = b[k]; }
        }
    }

    uint8_t marked[VRGLOVE_MAXV];
    memset(marked, 0, sizeof marked);
    for (int f = 0; f < nf; f++) {
        uint8_t vi[4], ni[4];
        int bad = 0;
        for (int k = 0; k < 4; k++) {
            uint8_t b = P->vidx[4 * f + k];
            vi[k] = (uint8_t)(b & imask);
            if (vi[k] >= nv) bad = 1;
            ni[k] = nn > 0 ? (uint8_t)(P->nidx[4 * f + k] & imask) : 0;
            if (ni[k] >= nn) ni[k] = 0;
        }
        if (bad) continue;
        for (int k = 0; k < 4; k++) if (shared && (P->vidx[4 * f + k] & 0x80)) marked[vi[k]] = 1;
        const int tri = vi[2] == vi[3];
        const int nc = tri ? 3 : 4;
        if (P->nout + (uint32_t)nc > VRGLOVE_MAXOUT) return -3;
        uint16_t texid = rd16(ram, ptx + 2u * (uint32_t)f);
        int reg = reg_get(rg, ram, texid);
        uint32_t b0 = P->nout;
        for (int k = 0; k < nc; k++) {
            const uint32_t sk = mirror ? (uint32_t)(tri ? TPERM[k] : QPERM[k]) : (uint32_t)k;       /* esquina del juego de la que salen las UV */
            uint32_t u = rd8(ram, puv + 8u * (uint32_t)f + 2u * sk), v = rd8(ram, puv + 8u * (uint32_t)f + 2u * sk + 1);
            uint32_t j = P->nout++;
            P->out_v[j] = vi[k]; P->out_n[j] = ni[k]; P->out_reg[j] = (uint8_t)reg;
            if (reg) {
                const Region *R = &rg->r[reg];
                uint32_t uu = ((u * ((uint32_t)R->w + 1u)) >> 8) + R->ox, vv = ((v * ((uint32_t)R->h + 1u)) >> 8) + R->oy;
                P->out_u[j] = (uint8_t)(uu > 255u ? 255u : uu); P->out_t[j] = (uint8_t)(vv > 255u ? 255u : vv);
            }
        }
        if (tri) { P->idx[P->nidx_out++] = (uint16_t)b0; P->idx[P->nidx_out++] = (uint16_t)(b0 + 1); P->idx[P->nidx_out++] = (uint16_t)(b0 + 2); }
        else {
            const uint16_t t[6] = {0, 1, 3, 1, 2, 3};      /* (A,B,D), (B,C,D) */
            for (int k = 0; k < 6; k++) P->idx[P->nidx_out++] = (uint16_t)(b0 + t[k]);
        }
    }

    /* soldaduras contra el padre (ns_extract.c: el campo apunta al paquete del padre -> cara y esquina -> índice de vértice del padre) */
    if (shared && parent_m && !mirror && rng_ok(parent_m, MDL_SIZE)) {
        int32_t pnf = rs32(ram, parent_m + 4), pnv = rs32(ram, parent_m + 0x34);
        uint32_t ppvi = rd32(ram, parent_m + 0x3C), pflag = rd32(ram, parent_m);
        if (pnf > 0 && pnf <= 4096 && pnv > 0 && rng_ok(ppvi, (uint32_t)pnf * 4u)) {
            static const uint8_t WELD_ORDER[4] = {0, 1, 3, 2};
            for (int i = 0; i < nv; i++) {
                if (!marked[i]) continue;
                uint16_t pad = (uint16_t)P->v[4 * i + 3];
                if (pad == 0xFFFFu || pad < 8) continue;
                uint32_t off = (uint32_t)pad - 8u, face = off / 52u, rem = off % 52u;
                if (rem >= 48u || rem % 12u != 0 || face >= (uint32_t)pnf) continue;
                uint8_t b = rd8(ram, ppvi + 4u * face + WELD_ORDER[rem / 12u]);
                uint8_t pvx = (uint8_t)(b & ((pflag & MF_SHARED_VERTS) ? 0x7F : 0xFF));
                if (pvx < pnv) P->weld[i] = pvx;
            }
        }
    }

    *hash = hu(*hash, (uint32_t)nv); *hash = hu(*hash, (uint32_t)nn); *hash = hu(*hash, (uint32_t)nf);
    *hash = hb(*hash, P->v, (size_t)nv * 8); *hash = hb(*hash, P->n, (size_t)nn * 8);
    *hash = hb(*hash, P->vidx, (size_t)nf * 4); *hash = hb(*hash, P->nidx, (size_t)nf * 4);
    *hash = hb(*hash, P->weld, (size_t)nv);
    *hash = hu(*hash, P->nout);
    *hash = hb(*hash, P->out_v, P->nout); *hash = hb(*hash, P->out_n, P->nout);
    *hash = hb(*hash, P->out_u, P->nout); *hash = hb(*hash, P->out_t, P->nout); *hash = hb(*hash, P->out_reg, P->nout);
    return 0;
}

/* La mano derecha del juego NO es el espejo de la izquierda (pulgar abierto ~39 grados frente a ~22, dedos 20 mm más cortos y curvados hacia la palma): se sustituye por el espejo de la izquierda,
 * conservando las soldaduras con SU antebrazo (cada vértice marcado del espejo toma el campo de soldadura del vértice marcado de la derecha original más cercano). Solo si las dos son del mismo
 * modelo (misma topología, ninguna posición a más de MIRROR_MAX_DEV unidades del espejo) y todas las soldaduras originales se reencuentran; si no, queda la mano original. */
#define MIRROR_MAX_DEV 30
static int mirror_hand_fits(const VrGlovePiece *R, VrGlovePiece *M) {
    if (M->nv != R->nv || M->nn != R->nn || M->nf != R->nf || !M->nout) return 0;
    uint8_t marked[VRGLOVE_MAXV];
    memset(marked, 0, sizeof marked);
    for (int f = 0; f < M->nf; f++) for (int k = 0; k < 4; k++) if (M->vidx[4 * f + k] & 0x80) marked[M->vidx[4 * f + k] & 0x7F] = 1;
    for (int j = 0; j < R->nv; j++) {          /* ningún vértice de la derecha lejos del espejo: lo contrario no es la misma mano */
        int near = 0;
        for (int i = 0; i < M->nv && !near; i++) {
            int d = 0;
            for (int c = 0; c < 3; c++) { int e = abs(M->v[4 * i + c] - R->v[4 * j + c]); if (e > d) d = e; }
            near = d <= MIRROR_MAX_DEV;
        }
        if (!near) return 0;
    }
    int want = 0, got = 0;
    uint8_t used[VRGLOVE_MAXV];
    memset(used, 0, sizeof used);
    for (int j = 0; j < R->nv; j++) if (R->weld[j] != 0xFF) want++;
    memset(M->weld, 0xFF, sizeof M->weld);
    for (int i = 0; i < M->nv; i++) {
        if (!marked[i]) continue;
        int best = -1, bd = MIRROR_MAX_DEV + 1;
        for (int j = 0; j < R->nv; j++) {
            if (R->weld[j] == 0xFF || used[j]) continue;
            int d = 0;
            for (int c = 0; c < 3; c++) { int e = abs(M->v[4 * i + c] - R->v[4 * j + c]); if (e > d) d = e; }
            if (d < bd) { bd = d; best = j; }
        }
        if (best >= 0) { M->weld[i] = R->weld[best]; used[best] = 1; got++; }
    }
    return got == want;
}

/* ------------------------------------------------------------------------------------------------------ construcción */
VrGlove *vrglove_create(void) { return (VrGlove *)calloc(1, sizeof(VrGlove)); }
void vrglove_destroy(VrGlove *g) {
    if (!g) return;
    free(g->atlas);
    free(g);
}

int vrglove_build(VrGlove *g, const uint8_t *ram, const uint16_t *vram, const uint32_t models[2][3], uint64_t skip_hash) {
    if (!g || !ram || !vram || !models) return -1;
    g->valid = 0;
    Regs rg;
    memset(&rg, 0, sizeof rg);
    rg.n = 1;                       /* región 0: blanco (1 x 1) */
    rg.r[0].rw = rg.r[0].rh = 1;
    uint64_t h = 14695981039346656037ull;
    for (int s = 0; s < 2; s++)
        for (int k = 0; k < 3; k++) {
            int rc = parse_piece(ram, models[s][k], k > 0 ? models[s][k - 1] : 0, &g->p[s][k], &rg, &h, 0);
            if (rc) return rc;
            if (s == 1 && k == 2 && g->p[0][2].nf > 0) {       /* mano derecha del juego: pulgar abierto y dedos curvados; se usa el espejo de la izquierda si encaja (ver mirror_hand_fits) */
                VrGlovePiece *M = (VrGlovePiece *)malloc(sizeof *M);
                if (!M) return -4;
                if (parse_piece(ram, models[0][2], 0, M, &rg, &h, 1) == 0 && mirror_hand_fits(&g->p[1][2], M)) g->p[1][2] = *M;
                free(M);
            }
        }
    for (int i = 1; i < rg.n; i++) {
        const Region *R = &rg.r[i];
        h = hu(h, R->texid); h = hu(h, R->tpage); h = hu(h, R->clut);
        h = hu(h, (uint32_t)R->ox | ((uint32_t)R->oy << 8) | ((uint32_t)R->w << 16) | ((uint32_t)R->h << 24));
        h = hash_region_vram(h, vram, R);
    }
    g->hash = h;
    if (h == skip_hash) return 0;

    /* atlas: bloques apilados (borde de 1 texel replicado), ancho = región más ancha + 2 */
    int aw = 3, ah = 0;
    for (int i = 0; i < rg.n; i++) if (rg.r[i].rw + 2 > aw) aw = rg.r[i].rw + 2;
    for (int i = 0; i < rg.n; i++) { rg.r[i].ax = 1; rg.r[i].ay = ah + 1; ah += rg.r[i].rh + 2; }
    if (ah > VRGLOVE_MAXATLAS_H) return -4;
    uint8_t *px = (uint8_t *)malloc((size_t)aw * (size_t)ah * 4);
    if (!px) return -4;
    for (int i = 0; i < rg.n; i++) {
        const Region *R = &rg.r[i];
        for (int y = 0; y < R->rh + 2; y++)
            for (int x = 0; x < R->rw + 2; x++) {
                int sx = x - 1 < 0 ? 0 : (x - 1 >= R->rw ? R->rw - 1 : x - 1), sy = y - 1 < 0 ? 0 : (y - 1 >= R->rh ? R->rh - 1 : y - 1);
                uint8_t *d = px + ((size_t)(R->ay - 1 + y) * (size_t)aw + (size_t)(R->ax - 1 + x)) * 4;
                if (i == 0) { d[0] = d[1] = d[2] = d[3] = 255; }
                else texel_rgba(vram, R->tpage, R->clut, R->ox + sx, R->oy + sy, d);
            }
        for (int x = R->rw + 2; x < aw; x++)      /* relleno a la derecha del bloque: transparente */
            for (int y = 0; y < R->rh + 2; y++) memset(px + ((size_t)(R->ay - 1 + y) * (size_t)aw + (size_t)(R->ax - 1 + x)) * 4, 0, 4);
    }
    for (int s = 0; s < 2; s++)
        for (int k = 0; k < 3; k++) {
            VrGlovePiece *P = &g->p[s][k];
            for (uint32_t j = 0; j < P->nout; j++) {
                const Region *R = &rg.r[P->out_reg[j]];
                if (P->out_reg[j] == 0) { P->out_uv[j][0] = ((float)R->ax + 0.5f) / (float)aw; P->out_uv[j][1] = ((float)R->ay + 0.5f) / (float)ah; }
                else {
                    P->out_uv[j][0] = ((float)R->ax + (float)(P->out_u[j] - R->ox) + 0.5f) / (float)aw;
                    P->out_uv[j][1] = ((float)R->ay + (float)(P->out_t[j] - R->oy) + 0.5f) / (float)ah;
                }
            }
        }
    free(g->atlas);
    g->atlas = px; g->aw = (uint32_t)aw; g->ah = (uint32_t)ah;
    g->id = ++g_glove_id;
    g->valid = 1;
    return 1;
}

/* ------------------------------------------------------------------------------------------------------ pose */
int vrglove_pose(const VrGlove *g, int side, const VrGlovePose *p, VrGloveOut *o) {
    if (!g || !g->valid || !p || !o || (side != 0 && side != 1) || !(p->upm > 1.0f) || !isfinite(p->upm) || !isfinite(p->grip)) return -1;
    for (int k = 0; k < 3; k++) {
        for (int i = 0; i < 3; i++) if (!isfinite(p->org[k][i])) return -1;
        for (int i = 0; i < 9; i++) if (!isfinite(p->R[k][i])) return -1;
    }
    const float inv = 1.0f / p->upm;
    float pos[3][VRGLOVE_MAXV][3];
    for (int k = 0; k < 3; k++) {
        const VrGlovePiece *P = &g->p[side][k];
        const float *R = p->R[k], *O = p->org[k];
        o->nv[k] = 0;
        if (!P->nv) continue;
        const int16_t *sv = P->v, *sn = P->n;
        if (k == 2 && p->fist && p->grip >= p->fist_min) {
            float gq = floorf(fminf(fmaxf(p->grip, 0.0f), 1.0f) * 64.0f + 0.5f) / 64.0f;      /* la deformación se repite solo si el agarre cambia 1/64 */
            if (!(o->fist_valid && o->fist_id == g->id && o->fist_grip == gq)) {
                VrFistMesh m = {P->nv, P->nn, P->nf, P->v, P->n, P->vidx, P->nidx};
                o->fist_valid = vrfist_deform(&m, side ? 1.0f : -1.0f, gq, p->fist, o->fv, o->fn) == 0;
                o->fist_id = g->id; o->fist_grip = gq;
            }
            if (o->fist_valid) { sv = o->fv; sn = o->fn; }
        }
        for (int i = 0; i < P->nv; i++) {
            const VrGlovePiece *Q = k > 0 ? &g->p[side][k - 1] : NULL;
            if (Q && P->weld[i] != 0xFF && P->weld[i] < Q->nv) { memcpy(pos[k][i], pos[k - 1][P->weld[i]], sizeof pos[k][i]); continue; }
            float lx = sv[4 * i] * inv, ly = sv[4 * i + 1] * inv, lz = sv[4 * i + 2] * inv;
            pos[k][i][0] = O[0] + R[0] * lx + R[1] * ly + R[2] * lz;
            pos[k][i][1] = O[1] + R[3] * lx + R[4] * ly + R[5] * lz;
            pos[k][i][2] = O[2] + R[6] * lx + R[7] * ly + R[8] * lz;
        }
        for (uint32_t j = 0; j < P->nout; j++) {
            VrHandsVertex *V = &o->v[k][j];
            memcpy(V->pos, pos[k][P->out_v[j]], sizeof V->pos);
            float nx = 0, ny = 1, nz = 0;
            if (P->nn > 0 && P->out_n[j] < P->nn) {
                const int16_t *q = sn + 4 * P->out_n[j];
                float ax = q[0] / 4096.0f, ay = q[1] / 4096.0f, az = q[2] / 4096.0f;
                nx = R[0] * ax + R[1] * ay + R[2] * az; ny = R[3] * ax + R[4] * ay + R[5] * az; nz = R[6] * ax + R[7] * ay + R[8] * az;
                float l = sqrtf(nx * nx + ny * ny + nz * nz);
                if (l > 1e-6f) { nx /= l; ny /= l; nz /= l; } else { nx = 0; ny = 1; nz = 0; }
            }
            V->nrm[0] = nx; V->nrm[1] = ny; V->nrm[2] = nz;
            V->uv[0] = P->out_uv[j][0]; V->uv[1] = P->out_uv[j][1];
            V->rgba[0] = V->rgba[1] = V->rgba[2] = V->rgba[3] = 255;       /* brillo ya establecido de las manos (255); el modelo no trae color por cara */
        }
        o->nv[k] = P->nout;
    }
    return 0;
}
