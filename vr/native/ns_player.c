/* ns_player: ver ns_player.h. */
#include "ns_player.h"
#include <math.h>
#include <string.h>

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

static int xf_close(const NsXf *a, const NsXf *b) {
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) if (fabsf(a->m[4 * i + j] - b->m[4 * i + j]) > 3e-4f) return 0;
        if (fabsf(a->m[4 * i + 3] - b->m[4 * i + 3]) > 1.5f) return 0;
    }
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
    uint8_t used[1024];
    uint32_t np = in->n_pieces < sizeof used ? in->n_pieces : (uint32_t)sizeof used;
    memset(used, 0, np);
    int bound = 0;
    for (int k = 0; k < nm; k++) {
        NsXf x;
        if (!read_part(ram, key, k, &x)) continue;
        for (uint32_t p = 0; p < np && m->part[k].n < NS_PLAYER_MAXPC; p++) {
            if (used[p]) continue;
            if (xf_close(&sc->piece[in->first_piece + p].world, &x)) {
                m->part[k].idx[m->part[k].n++] = in->first_piece + p;
                used[p] = 1;
            }
        }
        if (m->part[k].n) bound++;
    }
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
