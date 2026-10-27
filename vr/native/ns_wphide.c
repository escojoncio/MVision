/* ns_wphide: ver ns_wphide.h. */
#include "ns_wphide.h"
#include <string.h>

#define OBJS_NMODELS 0x2E
#define OBJS_FIRST 0x48
#define OBJ_SIZE 0x5C
#define MAX_MODELS 64
#define RAM_SIZE 0x200000u

static int ptr_ok(uint32_t a, uint32_t len) {
    return (a & 0xFFE00000u) == 0x80000000u && a >= 0x80010000u && (a & 3) == 0 && (uint64_t)(a & 0x1FFFFFu) + len <= RAM_SIZE;
}
static uint32_t rd32(const uint8_t *ram, uint32_t a) { uint32_t v; memcpy(&v, ram + (a & 0x1FFFFFu), 4); return v; }
static int16_t rd16(const uint8_t *ram, uint32_t a) { int16_t v; memcpy(&v, ram + (a & 0x1FFFFFu), 2); return v; }

int ns_wphide_apply(NsScene *sc, const uint8_t *ram, int on) {
    if (!on || !sc || !ram || !sc->inst || !sc->piece) return 0;
    uint32_t pc = rd32(ram, NS_WPHIDE_PLAYER_CTRL);
    if (!ptr_ok(pc, NS_WPHIDE_FROM_CTRL + 0x9C0u)) return 0;
    uint32_t wa = rd32(ram, pc - NS_WPHIDE_FROM_CTRL + NS_WPHIDE_WEAPON_ACTOR);
    if (!ptr_ok(wa, NS_WPHIDE_WA_OBJS + 4u)) return 0;
    uint32_t objs = rd32(ram, wa + NS_WPHIDE_WA_OBJS);
    if (!ptr_ok(objs, OBJS_FIRST)) return 0;
    int nm = rd16(ram, objs + OBJS_NMODELS);
    if (nm <= 0 || nm > MAX_MODELS || !ptr_ok(objs, OBJS_FIRST + OBJ_SIZE * (uint32_t)nm)) return 0;
    const uint32_t key = 0x80000000u | (objs & 0x1FFFFFFFu);   /* misma clave que el extractor (norm_addr) */
    int n = 0;
    for (uint32_t i = 0; i < sc->n_inst; i++) {
        const NsInstance *in = &sc->inst[i];
        if (in->key != key) continue;
        if ((uint64_t)in->first_piece + in->n_pieces > sc->n_piece) continue;
        for (uint32_t k = 0; k < in->n_pieces; k++) { sc->piece[in->first_piece + k].flags |= NS_PF_HIDDEN; n++; }
    }
    return n;
}
