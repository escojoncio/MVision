/* ns_div: ver ns_div.h. Réplica de libdg (bound.c, trans.c, divide.c) de la decompilación pública FoxdieTeam/mgs_reversing sobre la RAM del motor. */
#include "ns_div.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define A_CHAN     0x800B2B7Cu   /* canal principal: eye_inv +0x10, eye +0x30, H +0x50 (s16), nº de objetos +0x56 (s16), cola +0x58 */
#define A_GROUP    0x800ACAF8u   /* DG_CurrentGroupID */
#define A_CLOCK    0x800ACAB0u   /* reloj de búfer (GV_Clock) */
#define A_MEMSYS   0x800AE480u   /* MemorySystems[0] (M_Sys = 4112 B: stat, start, end, n_units, units[512] de {addr, addr_ptr}) */
#define MEMSYS_SZ  0x1010u
#define ADDR_MASK  0x1FFFFFFFu
#define FREE_UNIT  0u
#define CLIP_X     160           /* DG_SetRVectorCode / DG_BoundCheck: ventana de la pantalla del motor */
#define CLIP_Y     112
#define DIV_THRESH 0x800u        /* divide_mem->field_14 (divide.c:577) */
#define PACK_SIZE  0x34          /* sizeof(POLY_GT4) */
#define MAX_QUEUE  256

typedef struct { const uint8_t *b; uint32_t size; } Ram;
static int okr(const Ram *r, uint32_t a, uint32_t n) {
    uint32_t seg = a >> 29;
    if (seg != 0 && seg != 4 && seg != 5) return 0;
    uint32_t o = a & ADDR_MASK;
    return o < r->size && n <= r->size - o;
}
static uint32_t r32(const Ram *r, uint32_t a) { uint32_t v = 0; if (okr(r, a, 4)) memcpy(&v, r->b + (a & ADDR_MASK), 4); return v; }
static uint16_t r16(const Ram *r, uint32_t a) { uint16_t v = 0; if (okr(r, a, 2)) memcpy(&v, r->b + (a & ADDR_MASK), 2); return v; }
static int16_t s16(const Ram *r, uint32_t a) { return (int16_t)r16(r, a); }
static int32_t s32(const Ram *r, uint32_t a) { return (int32_t)r32(r, a); }
static uint8_t r8(const Ram *r, uint32_t a) { return okr(r, a, 1) ? r->b[a & ADDR_MASK] : 0; }

/* ---------------------------------------------------------------------------------------------------------------- GTE (RTPS / NCLIP) */
static uint8_t unr_table[257];
static int unr_ready;
static void unr_init(void) {
    for (int i = 0; i < 257; i++) {
        int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
        unr_table[i] = (uint8_t)(v < 0 ? 0 : v);
    }
    unr_ready = 1;
}
/* División del hardware: H / SZ3 en 16.16, saturada a 0x1FFFF (si H >= 2 * SZ3 siempre satura: z <= H/2 se proyecta como si estuviera en H/2) */
static uint32_t gte_divide(uint32_t h, uint32_t sz3) {
    if (h >= sz3 * 2u) return 0x1FFFFu;
    if (!unr_ready) unr_init();
    int z = __builtin_clz(sz3 & 0xFFFFu) - 16;
    uint64_t n = (uint64_t)h << z;
    uint32_t d = sz3 << z;
    uint32_t u = unr_table[(d - 0x7FC0u) >> 7] + 0x101u;
    d = (0x2000080u - d * u) >> 8;
    d = (0x0000080u + d * u) >> 8;
    uint64_t q = (n * d + 0x8000u) >> 16;
    return q > 0x1FFFFu ? 0x1FFFFu : (uint32_t)q;
}
void ns_div_rtps(const NsDivMat *M, int H, int ofx, int ofy, const int16_t v[3], int16_t *sx, int16_t *sy, uint16_t *sz) {
    int64_t mac[3];
    for (int i = 0; i < 3; i++) {
        int64_t s = (int64_t)M->t[i] * 4096 + (int64_t)M->m[i][0] * v[0] + (int64_t)M->m[i][1] * v[1] + (int64_t)M->m[i][2] * v[2];
        mac[i] = s >> 12;
    }
    int64_t ir1 = mac[0] < -0x8000 ? -0x8000 : mac[0] > 0x7FFF ? 0x7FFF : mac[0];
    int64_t ir2 = mac[1] < -0x8000 ? -0x8000 : mac[1] > 0x7FFF ? 0x7FFF : mac[1];
    int64_t z = mac[2] < 0 ? 0 : mac[2] > 0xFFFF ? 0xFFFF : mac[2];
    uint32_t n = gte_divide((uint32_t)H & 0xFFFFu, (uint32_t)z);
    int64_t x = ((int64_t)n * ir1 + ((int64_t)ofx << 16)) >> 16, y = ((int64_t)n * ir2 + ((int64_t)ofy << 16)) >> 16;
    *sx = (int16_t)(x < -0x400 ? -0x400 : x > 0x3FF ? 0x3FF : x);
    *sy = (int16_t)(y < -0x400 ? -0x400 : y > 0x3FF ? 0x3FF : y);
    *sz = (uint16_t)z;
}
int32_t ns_div_nclip(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2) {
    return (int32_t)((int64_t)x0 * y1 + (int64_t)x1 * y2 + (int64_t)x2 * y0 - (int64_t)x0 * y2 - (int64_t)x1 * y0 - (int64_t)x2 * y1);
}

static NsDivMat read_mat(const Ram *r, uint32_t a) {
    NsDivMat M;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) M.m[i][j] = s16(r, a + 2u * (uint32_t)(3 * i + j));
    for (int i = 0; i < 3; i++) M.t[i] = s32(r, a + 0x14u + 4u * (uint32_t)i);
    return M;
}

/* ---------------------------------------------------------------------------------------------------------------- contexto */
typedef struct { int16_t v[3]; uint8_t uv[2]; uint8_t c[3]; uint8_t cd; int16_t sx, sy; uint32_t sz; } RV;   /* RVECTOREX (20 B) */

typedef struct {
    Ram r;
    NsDivCfg cfg;
    NsDivAttrFn attr; NsDivEmitFn emit; void *user;
    NsDivStats *S;
    uint32_t thresh;                   /* divide_mem->field_14 */
    int H;
    int group;
    int n_packs;                       /* paquetes de división libres (divide_mem->n_packs) */
    int divide_on;                     /* DG_InitDividePacks tuvo éxito */
    /* objeto en curso */
    uint32_t objs, node, head;         /* DG_OBJS, nodo en proceso (modelo base o extensión), nodo base */
    int model_idx, node_idx;
    NsDivMat scr;                      /* obj->screen del modelo base */
    int16_t raise;
    int delta;                         /* divide_mem->field_0C */
    int base_trans;                    /* el modelo BASE de la cadena es translúcido (divide.c:608-615: field_10 se fija una vez por obj base) */
    int base_face;                     /* caras de los modelos anteriores de la cadena: los paquetes de la cadena son contiguos desde packs[] del modelo base */
    NsDivPrim tmpl;                    /* plantilla de la cara que se divide */
    RV blk[3][9];
    int emitted_this_face;
} Ctx;

static void code_of(RV *v) {
    uint8_t c = 0;
    if (v->sx < -CLIP_X) c = 1; else if (v->sx > CLIP_X) c = 2;
    if (v->sy < -CLIP_Y) c |= 4; else if (v->sy > CLIP_Y) c |= 8;
    if (!v->sz) c |= 16;
    v->cd = c;
}
static void mid(const RV *a, const RV *b, RV *o) {
    for (int i = 0; i < 3; i++) o->v[i] = (int16_t)(((int)a->v[i] + (int)b->v[i]) / 2);
    for (int i = 0; i < 2; i++) o->uv[i] = (uint8_t)(((int)a->uv[i] + (int)b->uv[i]) / 2);
    for (int i = 0; i < 3; i++) o->c[i] = (uint8_t)(((int)a->c[i] + (int)b->c[i]) / 2);
}
static void proj(const Ctx *c, RV *v) { int16_t sx, sy; uint16_t sz; ns_div_rtps(&c->scr, c->H, 0, 0, v->v, &sx, &sy, &sz); v->sx = sx; v->sy = sy; v->sz = sz; }
/* DG_SetRVectorDelta: separa 1 px el punto medio del borde (solo modelos opacos; el signo lo da la cara trasera) */
static void delta_of(const Ctx *c, const RV *r1, const RV *r2, RV *r3) {
    if (c->cfg.flags & NSDIV_NO_NUDGE) return;
    int d = c->delta;
    int vy_diff = r2->sy - r1->sy, vx_diff = r1->sx - r2->sx;
    if (vy_diff >= 0) {
        if (vx_diff >= 0) {
            if (vx_diff / 2 < vy_diff) r3->sx = (int16_t)(r3->sx + d);
            if (vy_diff / 2 < vx_diff) r3->sy = (int16_t)(r3->sy + d);
        } else {
            vx_diff = -vx_diff;
            if (vx_diff / 2 < vy_diff) r3->sx = (int16_t)(r3->sx + d);
            if (vy_diff / 2 < vx_diff) r3->sy = (int16_t)(r3->sy - d);
        }
    } else {
        vy_diff = -vy_diff;
        if (vx_diff >= 0) {
            if (vx_diff / 2 < vy_diff) r3->sx = (int16_t)(r3->sx - d);
            if (vy_diff / 2 < vx_diff) r3->sy = (int16_t)(r3->sy + d);
        } else {
            vx_diff = -vx_diff;
            if (vx_diff / 2 < vy_diff) r3->sx = (int16_t)(r3->sx - d);
            if (vy_diff / 2 < vx_diff) r3->sy = (int16_t)(r3->sy - d);
        }
    }
}

static void drop_face(Ctx *c, const NsDivPrim *p, int reason) {
    if (c->cfg.drop) c->cfg.drop(c->user, p->obj, p->node, p->face, reason, p->pack);
}
static void emit_prim(Ctx *c, const NsDivPrim *p) {
    if (c->emit) c->emit(c->user, p);
}

/* divide_setup: rejilla 3x3 de la manzana b (esquinas 0, 2, 6, 8) */
static void setup(Ctx *c, int b) {
    RV *rv = c->blk[b];
    mid(&rv[0], &rv[2], &rv[1]);
    mid(&rv[6], &rv[8], &rv[7]);
    proj(c, &rv[1]); proj(c, &rv[7]);
    mid(&rv[0], &rv[6], &rv[3]);
    mid(&rv[1], &rv[7], &rv[4]);
    mid(&rv[2], &rv[8], &rv[5]);
    proj(c, &rv[3]); proj(c, &rv[4]); proj(c, &rv[5]);
    code_of(&rv[1]); code_of(&rv[7]);
    delta_of(c, &rv[0], &rv[2], &rv[1]);
    delta_of(c, &rv[8], &rv[6], &rv[7]);
    code_of(&rv[3]); code_of(&rv[4]); code_of(&rv[5]);
    delta_of(c, &rv[2], &rv[8], &rv[5]);
    delta_of(c, &rv[6], &rv[0], &rv[3]);
}

/* DG_CopyPackToRVector sobre el hijo cuya esquina superior izquierda es la `i` de la manzana b. 1 = hay que volver a partirlo. */
static int copy_pack(Ctx *c, int b, int i) {
    const RV *ch[4] = {&c->blk[b][i], &c->blk[b][i + 1], &c->blk[b][i + 3], &c->blk[b][i + 4]};
    if (ch[0]->cd & ch[1]->cd & ch[2]->cd & ch[3]->cd) { c->S->sub_outcode++; return 0; }   /* DG_GetRVectorCode */
    if (b + 1 <= 2 && c->n_packs >= 4) {   /* rvec (ya + 9) < 0x1F800254 */
        int32_t opz = ns_div_nclip(ch[0]->sx, ch[0]->sy, ch[1]->sx, ch[1]->sy, ch[2]->sx, ch[2]->sy);
        if (opz < 0) opz = -opz;
        if ((uint32_t)opz > c->thresh) {
            RV *t = c->blk[b + 1];
            t[0] = *ch[0]; t[2] = *ch[1]; t[6] = *ch[2]; t[8] = *ch[3];
            return 1;
        }
    }
    if (!(c->cfg.flags & NSDIV_NO_BUDGET)) {
        if (c->n_packs <= 0) { c->S->sub_nopack++; return 0; }   /* DG_GetDividePacks devuelve 0: el trozo se pierde */
        c->n_packs--;
    }
    NsDivPrim p = c->tmpl;
    p.kind = NSDIV_SUB; p.level = (uint8_t)(b + 1); p.weld = 0;
    for (int k = 0; k < 4; k++) {
        p.sx[k] = ch[k]->sx; p.sy[k] = ch[k]->sy; p.sz[k] = (uint16_t)ch[k]->sz;
        for (int j = 0; j < 3; j++) { p.p[k][j] = ch[k]->v[j]; p.rgb[k][j] = ch[k]->c[j]; }
        p.u[k] = ch[k]->uv[0]; p.v[k] = ch[k]->uv[1];
    }
    c->S->sub_emitted++; c->S->split_depth[b + 1]++;
    emit_prim(c, &p);
    return 0;
}
/* DG_SubDivideRVectors */
static void subdivide(Ctx *c, int b) {
    setup(c, b);
    static const int kid[4] = {0, 1, 3, 4};
    for (int i = 0; i < 4; i++) if (copy_pack(c, b, kid[i])) subdivide(c, b + 1);
}

/* ---------------------------------------------------------------------------------------------------------------- Bound */
static int bound_of(const Ctx *c, const NsDivMat *M, const int32_t bb[6]) {
    int xl = 100000, xh = -100000, yl = 100000, yh = -100000, anyz = 0;
    for (int i = 9; i > 1; i--) {   /* las 8 primeras de las 9 esquinas de MakeBoundVerts: i = 9..2 */
        int16_t v[3] = {(int16_t)((i & 1) ? bb[3] : bb[0]), (int16_t)((i & 2) ? bb[4] : bb[1]), (int16_t)((i & 4) ? bb[5] : bb[2])};
        int16_t sx, sy; uint16_t sz;
        ns_div_rtps(M, c->H, 0, 0, v, &sx, &sy, &sz);
        if (sx < xl) xl = sx;
        if (sx > xh) xh = sx;
        if (sy < yl) yl = sy;
        if (sy > yh) yh = sy;
        if (sz) anyz = 1;
    }
    if (xl > CLIP_X || xh < -CLIP_X || yl > CLIP_Y || yh < -CLIP_Y) return 0;
    int flag = (xh > CLIP_X || xl < -CLIP_X || yh > CLIP_Y || yl < -CLIP_Y) ? 1 : 2;
    return anyz ? flag : 0;
}

/* ---------------------------------------------------------------------------------------------------------------- Trans + Divide de un modelo */
typedef struct { int16_t sx, sy; uint16_t sz; uint8_t code; } SV;

/* Posición de pantalla de la esquina soldada: la del vértice del padre (el motor copia las coordenadas del paquete del padre). 0 = sin resolver. */
static int weld_xy(Ctx *c, uint32_t model, int vidx, int depth, int16_t *sx, int16_t *sy) {
    if (depth > 8) return 0;
    uint32_t pv = r32(&c->r, model + 0x38);
    uint16_t pad = r16(&c->r, pv + 8u * (uint32_t)vidx + 6);
    if (pad == 0xFFFFu || pad < 8) return 0;
    uint32_t off = (uint32_t)pad - 8u, face = off / 52u, rem = off % 52u;
    int pidx = s32(&c->r, model + 0x2C);
    if (rem >= 48u || rem % 12u || pidx < 0 || pidx > 1023) return 0;
    uint32_t pnode = c->objs + 0x48u + 0x5Cu * (uint32_t)pidx, pmodel = r32(&c->r, pnode + 0x40);
    int pnf = s32(&c->r, pmodel + 4);
    if (!okr(&c->r, pmodel, 0x58) || (int)face >= pnf) return 0;
    static const int ord[4] = {0, 1, 3, 2};
    uint8_t b = r8(&c->r, r32(&c->r, pmodel + 0x3C) + 4u * face + (uint32_t)ord[rem / 12u]);
    if (b & 0x80) return weld_xy(c, pmodel, b & 0x7F, depth + 1, sx, sy);
    NsDivMat M = read_mat(&c->r, pnode + 0x20u);
    uint32_t pverts = r32(&c->r, pmodel + 0x38) + 8u * (uint32_t)(b & 0x7F);
    int16_t v[3] = {s16(&c->r, pverts), s16(&c->r, pverts + 2), s16(&c->r, pverts + 4)};
    uint16_t sz;
    ns_div_rtps(&M, c->H, 0, 0, v, sx, sy, &sz);
    return 1;
}

static void do_model(Ctx *c, uint32_t node, int outcode_on, int gbound, uint32_t flag_trans_model) {
    (void)flag_trans_model;
    Ram *r = &c->r;
    uint32_t model = r32(r, node + 0x40);
    if (!okr(r, model, 0x58)) return;
    uint32_t mflag = r32(r, model);
    int nv = s32(r, model + 0x34), nf_model = s32(r, model + 4);
    int nfaces = s16(r, node + 0x52);
    if (nfaces <= 0 || nfaces > nf_model) nfaces = nf_model < 0 ? 0 : nf_model;
    if (nv < 0 || nv > 255) return;
    uint32_t pverts = r32(r, model + 0x38), pvind = r32(r, model + 0x3C);
    if (!okr(r, pverts, (uint32_t)nv * 8u) || !okr(r, pvind, (uint32_t)nfaces * 4u)) return;
    SV sv[256];
    for (int i = 0; i < nv; i++) {
        int16_t v[3] = {s16(r, pverts + 8u * (uint32_t)i), s16(r, pverts + 8u * (uint32_t)i + 2), s16(r, pverts + 8u * (uint32_t)i + 4)};
        ns_div_rtps(&c->scr, c->H, 0, 0, v, &sv[i].sx, &sv[i].sy, &sv[i].sz);
        uint8_t code = 0;
        if (sv[i].sx < -CLIP_X) code = 1; else if (sv[i].sx > CLIP_X) code = 2;
        if (sv[i].sy < -CLIP_Y) code |= 4; else if (sv[i].sy > CLIP_Y) code |= 8;
        sv[i].code = code;
    }
    const int bothface = (mflag & 0x400u) != 0, trans = (mflag & 2u) != 0;
    static const int ord[4] = {0, 1, 3, 2};   /* esquina de paquete (A, B, D, C) -> byte del vindices */
    for (int f = 0; f < nfaces; f++) {
        c->S->faces++;
        uint32_t raw = r32(r, pvind + 4u * (uint32_t)f);
        uint8_t byte[4] = {(uint8_t)raw, (uint8_t)(raw >> 8), (uint8_t)(raw >> 16), (uint8_t)(raw >> 24)};
        NsDivPrim p;
        memset(&p, 0, sizeof p);
        p.obj = c->objs; p.model = (uint16_t)c->model_idx; p.node = (uint16_t)c->node_idx; p.face = (uint16_t)f; p.nv = 4;
        p.raise = c->raise; p.trans = (uint8_t)trans;
        for (int b = 0; b < 2; b++) { uint32_t base = r32(r, c->head + 0x54u + 4u * (uint32_t)b); p.pack[b] = base ? base + PACK_SIZE * (uint32_t)(c->base_face + f) : 0; }
        uint32_t tag = 0;
        int16_t sx[4], sy[4]; uint16_t sz[4];
        if (raw & 0x80808080u) {   /* DG_WriteObjVerticesIndirect: sin descarte ni subdivisión; z = media de las esquinas propias */
            c->S->f_indirect++;
            int stride = 0; uint32_t num = 0;
            for (int k = 0; k < 4; k++) {
                uint8_t b = byte[ord[k]];
                int vi = b & 0x7F;
                if (vi >= nv) vi = 0;
                for (int j = 0; j < 3; j++) p.p[k][j] = s16(r, pverts + 8u * (uint32_t)vi + 2u * (uint32_t)j);
                if (!(b & 0x80)) { sx[k] = sv[vi].sx; sy[k] = sv[vi].sy; sz[k] = sv[vi].sz; num += sv[vi].sz; stride++; }
                else {
                    p.weld |= (uint8_t)(1u << k);
                    sx[k] = sy[k] = 0; sz[k] = 0;
                    weld_xy(c, model, vi, 0, &sx[k], &sy[k]);
                }
            }
            tag = stride == 2 ? num / 2 : stride == 1 ? num : stride == 3 ? num / 3 : 0;
            if (!tag) { c->S->f_indirect_z0++; drop_face(c, &p, NSDIV_DROP_INDIRECT_Z0); continue; }
        } else {
            const SV *n0 = &sv[byte[0] & 0x7F], *n1 = &sv[byte[1] & 0x7F], *n2 = &sv[byte[3] & 0x7F], *n3 = &sv[byte[2] & 0x7F];
            const SV *q[4] = {n0, n1, n2, n3};
            for (int k = 0; k < 4; k++) {
                int vi = byte[ord[k]] & 0x7F;
                if (vi >= nv) vi = 0;
                for (int j = 0; j < 3; j++) p.p[k][j] = s16(r, pverts + 8u * (uint32_t)vi + 2u * (uint32_t)j);
                sx[k] = q[k]->sx; sy[k] = q[k]->sy; sz[k] = q[k]->sz;
            }
            if (outcode_on && !(c->cfg.flags & NSDIV_NO_OUTCODE)) {
                uint8_t bv = 0;
                if (n0->code) bv = (uint8_t)(n0->code & n1->code & n2->code & n3->code);
                if (bv) { c->S->f_outcode++; drop_face(c, &p, NSDIV_DROP_OUTCODE); continue; }
            }
            int32_t area = ns_div_nclip(n0->sx, n0->sy, n1->sx, n1->sy, n2->sx, n2->sy);
            uint32_t t;
            if (area <= 0) {
                if (!bothface || area == 0) { if (area == 0) c->S->f_degenerate++; else c->S->f_back++; drop_face(c, &p, area == 0 ? NSDIV_DROP_AREA0 : NSDIV_DROP_BACK); continue; }
                t = ((((uint32_t)(-area)) << 8) & 0xFFFE0000u) | 0x10000u;
                p.back = 1;
            } else {
                t = ((uint32_t)area << 8) & 0xFFFE0000u;
            }
            uint32_t zc = (uint32_t)(((int)n0->sz + (int)n3->sz) / 2);   /* los códigos de salida van en el byte alto: no alcanzan los 16 bits bajos */
            tag = t | (zc & 0xFFFFu);
        }
        const uint32_t z16 = tag & 0xFFFFu, pack_addr = tag >> 8;
        if (!z16) { c->S->f_z0++; drop_face(c, &p, NSDIV_DROP_Z0); continue; }
        for (int k = 0; k < 4; k++) {
            p.sx[k] = sx[k]; p.sy[k] = sy[k]; p.sz[k] = sz[k];
            if (c->attr) c->attr(c->user, c->objs, c->node_idx, f, k, &p.u[k], &p.v[k], p.rgb[k]);
        }
        p.kind = NSDIV_FULL; p.level = 0;
        const int limit_z = c->H > 1000 ? 60000 : 8192;
        if (!gbound && c->divide_on && !(c->cfg.flags & NSDIV_NO_SPLIT) && pack_addr > c->thresh && (int)z16 < limit_z && c->n_packs >= 4) {
            /* DG_InitRVector: la cara se sustituye por sus trozos */
            c->S->f_split++;
            c->tmpl = p;
            c->delta = c->base_trans ? 0 : ((pack_addr & 0x100u) ? -1 : 1);   /* divide.c:449-456 */
            RV *rv = c->blk[0];
            memset(c->blk, 0, sizeof c->blk);
            static const int slot[4] = {0, 2, 6, 8};
            for (int k = 0; k < 4; k++) {
                RV *v = &rv[slot[k]];
                for (int j = 0; j < 3; j++) v->v[j] = p.p[k][j];
                v->uv[0] = p.u[k]; v->uv[1] = p.v[k];
                for (int j = 0; j < 3; j++) v->c[j] = p.rgb[k][j];
                v->sx = sx[k]; v->sy = sy[k];
                int16_t qx, qy; ns_div_rtps(&c->scr, c->H, 0, 0, v->v, &qx, &qy, &sz[k]);
                v->sz = sz[k];
                code_of(v);
            }
            subdivide(c, 0);
        } else {
            c->S->f_full++;
            emit_prim(c, &p);
        }
    }
}

int ns_div_run(const uint8_t *ram, size_t ram_size, const NsDivCfg *cfg, NsDivAttrFn attr, NsDivEmitFn emit, void *user, NsDivStats *stats) {
    NsDivStats dummy;
    NsDivStats *S = stats ? stats : &dummy;
    memset(S, 0, sizeof *S);
    if (!ram || ram_size < 0x200000u) return -1;
    Ctx *c = (Ctx *)calloc(1, sizeof *c);
    if (!c) return -1;
    c->r.b = ram; c->r.size = ram_size > 0x200000u ? 0x200000u : (uint32_t)ram_size;
    if (cfg) c->cfg = *cfg; else { memset(&c->cfg, 0, sizeof c->cfg); c->cfg.pool = -1; c->cfg.n_packs_override = -1; }
    c->attr = attr; c->emit = emit; c->user = user; c->S = S;
    c->thresh = c->cfg.div_thresh ? c->cfg.div_thresh : DIV_THRESH;
    if (!c->cfg.div_thresh && c->cfg.scratch) { uint32_t t; memcpy(&t, c->cfg.scratch + 0x14, 4); if (t) c->thresh = t; }
    S->thresh = c->thresh;
    const Ram *r = &c->r;
    int ret = -1;
    if (!okr(r, A_CHAN, 0x60)) goto done;
    c->H = c->cfg.h_override > 0 ? c->cfg.h_override : s16(r, A_CHAN + 0x50);
    S->h = c->H;
    c->group = (int)r32(r, A_GROUP);
    {   /* DG_InitDividePacks: paquetes libres de la memoria del búfer */
        int clk = (int)(r32(r, A_CLOCK) & 1u), pool = c->cfg.pool;
        if (pool == -1) pool = clk; else if (pool == -2) pool = clk ^ 1; else pool &= 1;
        S->pool = pool;
        uint32_t base = A_MEMSYS + MEMSYS_SZ * (uint32_t)pool;
        uint32_t nu = r32(r, base + 12);
        int total = 0, nfree = 0;
        if (nu > 512) nu = 512;
        for (uint32_t i = 0; i < nu; i++) {
            uint32_t ua = base + 16u + 8u * i;
            if (r32(r, ua + 4) == FREE_UNIT) { nfree++; uint32_t sz = r32(r, ua + 8) - r32(r, ua); if (sz < 0x400000u) total += (int)(sz / PACK_SIZE); }
        }
        c->n_packs = c->cfg.n_packs_override >= 0 ? c->cfg.n_packs_override : total;
        c->divide_on = 1;
        if (nfree == 0 && !(c->cfg.flags & NSDIV_NO_BUDGET)) { S->divide_aborted = 1; S->n_packs_start = 0; ret = 0; goto done; }   /* divide.c:571 */
        S->n_packs_start = c->n_packs;
        (void)nfree;
    }
    int n = s16(r, A_CHAN + 0x56);
    uint32_t q = r32(r, A_CHAN + 0x58);
    if (n < 0) n = 0;
    if (n > MAX_QUEUE) n = MAX_QUEUE;
    if (n > 0 && !okr(r, q, 4u * (uint32_t)n)) n = 0;
    ret = 0;
    int emitted = 0;
    for (int i = 0; i < n; i++) {
        uint32_t o = r32(r, q + 4u * (uint32_t)i);
        if (!o || !okr(r, o, 0x48)) continue;
        if (c->cfg.only_obj && (c->cfg.only_obj & ADDR_MASK) != (o & ADDR_MASK)) continue;
        S->objs++;
        uint32_t flag = r32(r, o + 0x28);
        int gid = r16(r, o + 0x2C), nm = s16(r, o + 0x2E);
        uint32_t def = r32(r, o + 0x24);
        if (flag & 0x80u) continue;                       /* DG_FLAG_INVISIBLE */
        if (gid && !(gid & c->group)) continue;
        if (nm <= 0 || nm > 1023 || !okr(r, o + 0x48u, 0x5Cu * (uint32_t)nm)) continue;
        const int gbound = (flag & 0x20u) != 0;
        c->objs = o;
        if (gbound) {
            S->objs_gbound++;
            NsDivMat M0 = read_mat(r, o + 0x48u + 0x20u);
            int32_t bb[6];
            for (int k = 0; k < 6; k++) bb[k] = s32(r, def + 8u + 4u * (uint32_t)k);
            if (!bound_of(c, &M0, bb)) continue;
        }
        int drew = 0;
        for (int j = 0; j < nm; j++) {
            uint32_t node = o + 0x48u + 0x5Cu * (uint32_t)j;
            uint32_t model = r32(r, node + 0x40);
            int bm = 2;
            c->scr = read_mat(r, node + 0x20u);
            if ((flag & 0x10u) && okr(r, model, 0x58)) {
                int32_t bb[6];
                for (int k = 0; k < 6; k++) bb[k] = s32(r, model + 8u + 4u * (uint32_t)k);
                bm = bound_of(c, &c->scr, bb);
            }
            int ram_bm = s16(r, node + 0x4C);
            if (bm != ram_bm) {
                S->bound_ram_mismatch++;
                if (bm == 0) S->bound_mine0_ram_on++; else if (ram_bm == 0) S->bound_mine_on_ram0++; else S->bound_1_vs_2++;
            }
            if (c->cfg.flags & NSDIV_IGNORE_RAM_BOUND) bm = ram_bm;
            if (bm == 0) { S->models_bound0++; continue; }
            if (bm == 1) S->models_bound1++; else S->models_bound2++;
            drew = 1;
            c->model_idx = j; c->raise = s16(r, node + 0x50);
            const int outcode_on = !gbound && bm == 1;
            c->base_trans = okr(r, model, 4) && (r32(r, model) & 2u) != 0;
            c->head = node; c->base_face = 0;
            uint32_t cur = node;
            for (int depth = 0; cur && depth < 64; depth++) {
                uint32_t nn = cur & ADDR_MASK, base = (o & ADDR_MASK) + 0x48u;
                int idx = nn >= base ? (int)((nn - base) / 0x5Cu) : 0;
                if (nn < base || (nn - base) % 0x5Cu || !okr(r, cur, 0x5C)) break;
                c->node = cur; c->node_idx = idx;
                do_model(c, cur, outcode_on, gbound, 0);
                c->base_face += s16(r, cur + 0x52);
                cur = r32(r, cur + 0x48);
            }
        }
        if (drew) S->objs_drawn++;
    }
    emitted = (int)(S->f_full + S->sub_emitted);
    S->n_packs_end = c->n_packs;
    ret = emitted;
done:
    free(c);
    return ret;
}
