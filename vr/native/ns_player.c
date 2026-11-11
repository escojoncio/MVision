/* ns_player: ver ns_player.h. */
#include "ns_player.h"
#include <math.h>
#include <string.h>

#define OBJS_PARENT_MAT 0x20   /* puntero a la matriz de otro objeto (objetos pegados; docs/escena_nativa.md §3.2) */
#define OBJS_FLAG 0x28         /* banderas del conjunto */
#define OF_HIDDEN 0x080u       /* el motor no dibuja el objeto (la misma que mira ns_extract.c) */
#define OBJS_NMODELS 0x2E
#define OBJS_FIRST 0x48
#define OBJ_SIZE 0x5C
#define RAM_SIZE 0x200000u

static int ptr_ok(uint32_t a, uint32_t len) {
    return (a & 0xFFE00000u) == 0x80000000u && (a & 3) == 0 && (uint64_t)(a & 0x1FFFFFu) + len <= RAM_SIZE;
}
static uint32_t rd32(const uint8_t *ram, uint32_t a) { uint32_t v; memcpy(&v, ram + (a & 0x1FFFFFu), 4); return v; }
static int16_t rd16(const uint8_t *ram, uint32_t a) { int16_t v; memcpy(&v, ram + (a & 0x1FFFFFu), 2); return v; }

uint32_t ns_player_key(const uint8_t *ram) {
    if (!ram) return 0;
    uint32_t ob = rd32(ram, NS_PLAYER_BODY_PTR);
    if (!ptr_ok(ob, 8)) return 0;
    uint32_t objs = rd32(ram, ob);
    if (!ptr_ok(objs, OBJS_FIRST + OBJ_SIZE * (NS_PART_HEAD + 1))) return 0;
    return objs;
}

int ns_player_find(const NsScene *sc, uint32_t key) {
    if (!sc || !key) return -1;
    for (uint32_t i = 0; i < sc->n_inst; i++) if (sc->inst[i].key == key) return (int)i;
    return -1;
}

static int read_part(const uint8_t *ram, uint32_t objs, int part, NsXf *x) {
    uint32_t a = objs + OBJS_FIRST + OBJ_SIZE * (uint32_t)part;
    if (!ptr_ok(a, OBJ_SIZE)) return 0;
    int16_t r[9]; int32_t t[3];
    for (int i = 0; i < 9; i++) r[i] = rd16(ram, a + 2u * (uint32_t)i);
    for (int i = 0; i < 3; i++) t[i] = (int32_t)rd32(ram, a + 0x14u + 4u * (uint32_t)i);
    ns_xf_from_engine(x, r, t);
    return 1;
}

/* Igualdad EXACTA: ambas matrices salen de los mismos s16 (rotación, múltiplos de 1/4096) y s32 (traslación) con ns_xf_from_engine. */
static int xf_same(const NsXf *a, const NsXf *b) {
    for (int i = 0; i < 12; i++) if (!(a->m[i] == b->m[i])) return 0;
    return 1;
}

int ns_player_bind(NsPlayerMap *m, const NsScene *sc, const uint8_t *ram, uint32_t key) {
    memset(m, 0, sizeof *m);
    m->inst = -1;
    if (!sc || !ram || !key || !ptr_ok(key, OBJS_FIRST)) return 0;
    int ii = ns_player_find(sc, key);
    if (ii < 0) return 0;
    const NsInstance *in = &sc->inst[ii];
    if (in->n_pieces == 0 || (uint64_t)in->first_piece + in->n_pieces > sc->n_piece) return 0;
    int nm = rd16(ram, key + OBJS_NMODELS);
    if (nm <= NS_PART_HEAD || nm > 64) return 0;
    if (nm > NS_PLAYER_PARTS) nm = NS_PLAYER_PARTS;
    enum { MAXP = 1024 };
    uint32_t np = in->n_pieces < MAXP ? in->n_pieces : (uint32_t)MAXP;
    uint8_t owner[MAXP];        /* parte que reclama la pieza: 0xFF ninguna, 0xFE disputada por varias partes (no se enlaza a ninguna) */
    memset(owner, 0xFF, np);
    NsXf xs[NS_PLAYER_PARTS];
    uint32_t have = 0;
    for (int k = 0; k < nm; k++) {
        if (!read_part(ram, key, k, &xs[k])) continue;
        have |= 1u << k;
    }
    for (uint32_t p = 0; p < np; p++) {
        for (int k = 0; k < nm; k++) {
            if (!(have & (1u << k)) || !xf_same(&sc->piece[in->first_piece + p].world, &xs[k])) continue;
            owner[p] = owner[p] == 0xFF ? (uint8_t)k : 0xFE;   /* dos partes con la misma matriz: la pieza es ambigua y no se reutiliza */
        }
    }
    int bound = 0;
    for (uint32_t p = 0; p < np; p++) {
        int k = owner[p];
        if (k >= NS_PLAYER_PARTS || m->part[k].n >= NS_PLAYER_MAXPC) continue;
        m->part[k].idx[m->part[k].n++] = in->first_piece + p;
    }
    for (int k = 0; k < nm; k++) if (m->part[k].n) bound++;
    m->key = key; m->inst = ii; m->bound = bound > 0;
    return bound;
}

int ns_player_hide_part(const NsPlayerMap *m, NsScene *sc, int part) {
    if (!m || !m->bound || !sc || part < 0 || part >= NS_PLAYER_PARTS) return 0;
    int n = 0;
    for (int i = 0; i < m->part[part].n; i++) {
        uint32_t p = m->part[part].idx[i];
        if (p < sc->n_piece) { sc->piece[p].flags |= NS_PF_HIDDEN; n++; }
    }
    return n;
}

int ns_player_hide_head_attached(NsScene *sc, const uint8_t *ram, uint32_t key) {
    if (!sc || !ram || !key || !ptr_ok(key, OBJS_FIRST + OBJ_SIZE * (NS_PART_HEAD + 1))) return 0;
    const uint32_t head = (key + OBJS_FIRST + OBJ_SIZE * (uint32_t)NS_PART_HEAD) & 0x1FFFFFu;
    int n = 0;
    for (uint32_t i = 0; i < sc->n_inst; i++) {
        const NsInstance *in = &sc->inst[i];
        if (in->key == key || !ptr_ok(in->key, OBJS_FIRST)) continue;
        uint32_t pm = rd32(ram, in->key + OBJS_PARENT_MAT);
        if (!pm || (pm & 0xFFE00000u) != 0x80000000u || (pm & 0x1FFFFFu) != head) continue;
        if ((uint64_t)in->first_piece + in->n_pieces > sc->n_piece) continue;
        for (uint32_t k = 0; k < in->n_pieces; k++) sc->piece[in->first_piece + k].flags |= NS_PF_HIDDEN;
        n++;
    }
    return n;
}

int ns_player_hide_engine_hidden(NsScene *sc, const uint8_t *ram, uint32_t key) {
    if (!sc || !ram || !key || !ptr_ok(key, OBJS_FIRST) || !(rd32(ram, key + OBJS_FLAG) & OF_HIDDEN)) return 0;
    int ii = ns_player_find(sc, key);
    if (ii < 0) return 0;
    const NsInstance *in = &sc->inst[ii];
    if ((uint64_t)in->first_piece + in->n_pieces > sc->n_piece) return 0;
    for (uint32_t k = 0; k < in->n_pieces; k++) sc->piece[in->first_piece + k].flags |= NS_PF_HIDDEN;
    return (int)in->n_pieces;
}

int ns_player_refresh(const NsPlayerMap *m, NsScene *sc, const uint8_t *ram, uint32_t part_mask) {
    if (!m || !m->bound || !sc || !ram) return 0;
    int upd = 0;
    for (int k = 0; k < NS_PLAYER_PARTS; k++) {
        if (!(part_mask & (1u << k)) || !m->part[k].n) continue;
        NsXf x;
        if (!read_part(ram, m->key, k, &x)) continue;
        for (int i = 0; i < m->part[k].n; i++) {
            uint32_t p = m->part[k].idx[i];
            if (p < sc->n_piece) sc->piece[p].world = x;
        }
        upd++;
    }
    return upd;
}
