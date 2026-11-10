/* ns_wphide: ver ns_wphide.h. */
#include "ns_wphide.h"
#include <math.h>
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

uint32_t ns_wphide_key(const uint8_t *ram) {
    if (!ram) return 0;
    uint32_t pc = rd32(ram, NS_WPHIDE_PLAYER_CTRL);
    if (!ptr_ok(pc, NS_WPHIDE_FROM_CTRL + 0x9C0u)) return 0;
    uint32_t wa = rd32(ram, pc - NS_WPHIDE_FROM_CTRL + NS_WPHIDE_WEAPON_ACTOR);
    if (!ptr_ok(wa, NS_WPHIDE_WA_OBJS + 4u)) return 0;
    uint32_t objs = rd32(ram, wa + NS_WPHIDE_WA_OBJS);
    if (!ptr_ok(objs, OBJS_FIRST)) return 0;
    int nm = rd16(ram, objs + OBJS_NMODELS);
    if (nm <= 0 || nm > MAX_MODELS || !ptr_ok(objs, OBJS_FIRST + OBJ_SIZE * (uint32_t)nm)) return 0;
    return 0x80000000u | (objs & 0x1FFFFFFFu);   /* misma clave que el extractor (norm_addr) */
}

int ns_wphide_apply(NsScene *sc, const uint8_t *ram, int on) {
    if (!on || !sc || !ram || !sc->inst || !sc->piece) return 0;
    const uint32_t key = ns_wphide_key(ram);
    if (!key) return 0;
    int n = 0;
    for (uint32_t i = 0; i < sc->n_inst; i++) {
        const NsInstance *in = &sc->inst[i];
        if (in->key != key) continue;
        if ((uint64_t)in->first_piece + in->n_pieces > sc->n_piece) continue;
        for (uint32_t k = 0; k < in->n_pieces; k++) { sc->piece[in->first_piece + k].flags |= NS_PF_HIDDEN; n++; }
    }
    return n;
}

int ns_wphide_carry(NsScene *sc, const uint8_t *ram, const float Ra[9], const float ta[3], const float Rd[9], const float td[3], int mirror) {
    if (!sc || !ram || !sc->inst || !sc->piece || !Ra || !ta || !Rd || !td) return 0;
    for (int i = 0; i < 9; i++) if (!isfinite(Ra[i]) || !isfinite(Rd[i])) return 0;
    for (int i = 0; i < 3; i++) if (!isfinite(ta[i]) || !isfinite(td[i])) return 0;
    const uint32_t key = ns_wphide_key(ram);
    if (!key) return 0;
    int n = 0;
    for (uint32_t i = 0; i < sc->n_inst; i++) {
        const NsInstance *in = &sc->inst[i];
        if (in->key != key || (uint64_t)in->first_piece + in->n_pieces > sc->n_piece) continue;
        for (uint32_t k = 0; k < in->n_pieces; k++) {
            NsXf *w = &sc->piece[in->first_piece + k].world;
            float Kr[9], Kt[3], d[3] = {w->m[3] - ta[0], w->m[7] - ta[1], w->m[11] - ta[2]};
            for (int r = 0; r < 3; r++) {      /* K = inv(Ha) * W: Ra^T [R | t - ta] */
                for (int c = 0; c < 3; c++) Kr[3 * r + c] = Ra[r] * w->m[c] + Ra[3 + r] * w->m[4 + c] + Ra[6 + r] * w->m[8 + c];
                Kt[r] = Ra[r] * d[0] + Ra[3 + r] * d[1] + Ra[6 + r] * d[2];
            }
            if (mirror) {                      /* mano contraria: K girado en el espejo del eje lateral de la pieza mano (F K F, F = diag(-1, 1, 1); la pieza izquierda es la derecha con X invertida) */
                for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) if ((r == 0) != (c == 0)) Kr[3 * r + c] = -Kr[3 * r + c];
                Kt[0] = -Kt[0];
            }
            for (int r = 0; r < 3; r++) {      /* W' = Hd * K */
                for (int c = 0; c < 3; c++) w->m[4 * r + c] = Rd[3 * r] * Kr[c] + Rd[3 * r + 1] * Kr[3 + c] + Rd[3 * r + 2] * Kr[6 + c];
                w->m[4 * r + 3] = Rd[3 * r] * Kt[0] + Rd[3 * r + 1] * Kt[1] + Rd[3 * r + 2] * Kt[2] + td[r];
            }
            n++;
        }
    }
    return n;
}
