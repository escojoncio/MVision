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

/* Conjunto de piezas del jugador (BA_PLAYER_BODY -> objeto -> primer campo) o 0. */
static uint32_t player_objs(const uint8_t *ram) {
    uint32_t ob = rd32(ram, NS_WPHIDE_PLAYER_BODY);
    if (!ptr_ok(ob, 8)) return 0;
    uint32_t po = rd32(ram, ob);
    return ptr_ok(po, OBJS_FIRST + OBJ_SIZE * (NS_WPHIDE_HAND_R + 1)) ? po : 0;
}
/* Parte del jugador (0..15) cuya matriz de mundo es `pm`, o -1. */
static int player_part(uint32_t po, uint32_t pm) {
    if (!po || (pm & 0xFFE00000u) != 0x80000000u) return -1;
    uint32_t d = (pm & 0x1FFFFFu) - ((po + OBJS_FIRST) & 0x1FFFFFu);
    return (pm & 0x1FFFFFu) >= ((po + OBJS_FIRST) & 0x1FFFFFu) && d % OBJ_SIZE == 0 && d / OBJ_SIZE < 16 ? (int)(d / OBJ_SIZE) : -1;
}
static int objs_ok(const uint8_t *ram, uint32_t objs) {
    if (!ptr_ok(objs, OBJS_FIRST)) return 0;
    int nm = rd16(ram, objs + OBJS_NMODELS);
    return nm > 0 && nm <= MAX_MODELS && ptr_ok(objs, OBJS_FIRST + OBJ_SIZE * (uint32_t)nm);
}
/* El actor del arma guarda el conjunto de piezas de su modelo en +0x20 (socom, famas, rifle, rcm, aam: el objeto va primero) o en +0x28 (grenade, bomb, mine: antes van root_ctrl y
 * root_obj, y +0x20 es el CONTROL del jugador). Regla general: vale el candidato cuya matriz padre (+0x20) es una parte del cuerpo del jugador (el modelo cuelga de su mano). Sin jugador
 * legible se conserva +0x20. */
static uint32_t actor_objs(const uint8_t *ram, uint32_t wa) {
    const uint32_t po = player_objs(ram);
    static const uint32_t slot[2] = {NS_WPHIDE_WA_OBJS, NS_WPHIDE_WA_OBJS_B};
    for (int i = 0; i < 2; i++) {
        uint32_t c = rd32(ram, wa + slot[i]);
        if (po && objs_ok(ram, c) && player_part(po, rd32(ram, c + NS_WPHIDE_PARENT_MAT)) >= 0) return c;
    }
    uint32_t c = rd32(ram, wa + slot[0]);
    return objs_ok(ram, c) ? c : 0;
}

uint32_t ns_wphide_key(const uint8_t *ram) {
    if (!ram) return 0;
    uint32_t pc = rd32(ram, NS_WPHIDE_PLAYER_CTRL);
    if (!ptr_ok(pc, NS_WPHIDE_FROM_CTRL + 0x9C0u)) return 0;
    uint32_t wa = rd32(ram, pc - NS_WPHIDE_FROM_CTRL + NS_WPHIDE_WEAPON_ACTOR);
    if (!ptr_ok(wa, NS_WPHIDE_WA_OBJS_B + 4u)) return 0;
    uint32_t objs = actor_objs(ram, wa);
    return objs ? 0x80000000u | (objs & 0x1FFFFFFFu) : 0;   /* misma clave que el extractor (norm_addr) */
}

/* Nombre de actor de arma (archivo fuente del actor: carpeta weapon). Los objetos de guion colgados de una mano (p. ej. belong.c de s16d) no lo son. */
static int weapon_actor_name(const uint8_t *ram, uint32_t p) {
    static const char *const names[] = {"aam.c", "bomb.c", "famas.c", "grenade.c", "mine.c", "rcm.c", "rfsight.c", "rifle.c", "socom.c", "stnsight.c"};
    if ((p & 0xFFE00000u) != 0x80000000u || (uint64_t)(p & 0x1FFFFFu) + 16u > RAM_SIZE) return 0;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) if (strncmp((const char *)ram + (p & 0x1FFFFFu), names[i], 16) == 0) return 1;
    return 0;
}
/* Claves (NsInstance.key) de los conjuntos de piezas de los actores de arma vivos: recorre las listas de actores (nodo: +0x14 puntero al nombre; el conjunto de piezas va en +0x20/+0x24/+0x28 del actor). */
static int weapon_actor_keys(const uint8_t *ram, uint32_t keys[16]) {
    int n = 0;
    for (int lv = 0; lv < NS_WPHIDE_N_ACTOR_LISTS; lv++) {
        const uint32_t base = NS_WPHIDE_ACTOR_LISTS + 68u * (uint32_t)lv, endn = base + 32u;
        uint32_t cur = rd32(ram, base + 4);
        for (int steps = 0; cur != endn && ptr_ok(cur, 0x30) && steps < 1024; steps++, cur = rd32(ram, cur + 4)) {
            if (!rd32(ram, cur + 8) || !weapon_actor_name(ram, rd32(ram, cur + 20))) continue;
            for (uint32_t o = 0x20; o <= 0x28 && n < 16; o += 4) {
                const uint32_t w = rd32(ram, cur + o);
                if (ptr_ok(w, OBJS_FIRST)) keys[n++] = 0x80000000u | (w & 0x1FFFFFFFu);
            }
        }
    }
    return n;
}

int ns_wphide_apply(NsScene *sc, const uint8_t *ram, int on) {
    if (!on || !sc || !ram || !sc->inst || !sc->piece) return 0;
    const uint32_t key = ns_wphide_key(ram);
    const uint32_t po = player_objs(ram), pkey = po ? 0x80000000u | (po & 0x1FFFFFFFu) : 0;
    int n = 0, nk = -1;
    uint32_t wkeys[16];
    for (uint32_t i = 0; i < sc->n_inst; i++) {
        const NsInstance *in = &sc->inst[i];
        int hide = key && in->key == key;
        if (!hide && po && in->key != pkey && ptr_ok(in->key, OBJS_FIRST)) {   /* regla general: todo objeto que el cuerpo del juego lleva en la mano (derecha/izquierda) y que creó un actor de arma, sea cual sea el arma */
            int part = player_part(po, rd32(ram, in->key + NS_WPHIDE_PARENT_MAT));
            if (part == NS_WPHIDE_HAND_R || part == NS_WPHIDE_HAND_L) {
                if (nk < 0) nk = weapon_actor_keys(ram, wkeys);
                for (int k = 0; k < nk && !hide; k++) hide = wkeys[k] == in->key;
            }
        }
        if (!hide || (uint64_t)in->first_piece + in->n_pieces > sc->n_piece) continue;
        for (uint32_t k = 0; k < in->n_pieces; k++) { sc->piece[in->first_piece + k].flags |= NS_PF_HIDDEN; n++; }
    }
    return n;
}

/* K = inv(Ha) * W de una pieza (Ra^T [R | t - ta]); con mirror, K girado en el espejo del eje lateral de la pieza mano (F K F, F = diag(-1, 1, 1): la pieza izquierda es la derecha con X invertida). */
static void rel_to_hand(const NsXf *w, const float Ra[9], const float ta[3], int mirror, float Kr[9], float Kt[3]) {
    const float d[3] = {w->m[3] - ta[0], w->m[7] - ta[1], w->m[11] - ta[2]};
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) Kr[3 * r + c] = Ra[r] * w->m[c] + Ra[3 + r] * w->m[4 + c] + Ra[6 + r] * w->m[8 + c];
        Kt[r] = Ra[r] * d[0] + Ra[3 + r] * d[1] + Ra[6 + r] * d[2];
    }
    if (mirror) {
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) if ((r == 0) != (c == 0)) Kr[3 * r + c] = -Kr[3 * r + c];
        Kt[0] = -Kt[0];
    }
}
/* Inversa 3x3 fila-mayor; 0 si es singular. */
static int inv3(const float a[9], float o[9]) {
    const float c0 = a[4] * a[8] - a[5] * a[7], c1 = a[5] * a[6] - a[3] * a[8], c2 = a[3] * a[7] - a[4] * a[6], det = a[0] * c0 + a[1] * c1 + a[2] * c2;
    if (!(fabsf(det) > 1e-12f)) return 0;
    const float id = 1.0f / det;
    o[0] = c0 * id; o[1] = (a[2] * a[7] - a[1] * a[8]) * id; o[2] = (a[1] * a[5] - a[2] * a[4]) * id;
    o[3] = c1 * id; o[4] = (a[0] * a[8] - a[2] * a[6]) * id; o[5] = (a[2] * a[3] - a[0] * a[5]) * id;
    o[6] = c2 * id; o[7] = (a[1] * a[6] - a[0] * a[7]) * id; o[8] = (a[0] * a[4] - a[1] * a[3]) * id;
    return 1;
}

int ns_wphide_carry_off(NsScene *sc, const uint8_t *ram, const float Ra[9], const float ta[3], const float Rd[9], const float td[3], int mirror, const float Ro[9], const float to[3]) {
    if (!sc || !ram || !sc->inst || !sc->piece || !Ra || !ta || !Rd || !td) return 0;
    if ((Ro == NULL) != (to == NULL)) return 0;
    for (int i = 0; i < 9; i++) if (!isfinite(Ra[i]) || !isfinite(Rd[i]) || (Ro && !isfinite(Ro[i]))) return 0;
    for (int i = 0; i < 3; i++) if (!isfinite(ta[i]) || !isfinite(td[i]) || (to && !isfinite(to[i]))) return 0;
    const uint32_t key = ns_wphide_key(ram);
    if (!key) return 0;
    if (Ro) for (uint32_t i = 0; i < sc->n_inst; i++) {      /* pieza de referencia singular (no hay D * inv(K0)): 0 sin tocar nada, no la relación del juego */
        const NsInstance *in = &sc->inst[i];
        float K0r[9], K0t[3], K0i[9];
        if (in->key != key || in->n_pieces == 0 || (uint64_t)in->first_piece + in->n_pieces > sc->n_piece) continue;
        rel_to_hand(&sc->piece[in->first_piece].world, Ra, ta, mirror, K0r, K0t);
        if (!inv3(K0r, K0i)) return 0;
    }
    int n = 0;
    for (uint32_t i = 0; i < sc->n_inst; i++) {
        const NsInstance *in = &sc->inst[i];
        if (in->key != key || (uint64_t)in->first_piece + in->n_pieces > sc->n_piece) continue;
        float Tr[9], Tt[3];      /* con desplazamiento: T = D * inv(K0), D = pose pedida de la pieza de referencia (la primera, la raíz del modelo) en la mano, K0 = la que le da el juego */
        int off = 0;
        if (Ro && in->n_pieces > 0) {
            float K0r[9], K0t[3], K0i[9], Dr[9], Dt[3];
            rel_to_hand(&sc->piece[in->first_piece].world, Ra, ta, mirror, K0r, K0t);
            memcpy(Dr, Ro, sizeof Dr); memcpy(Dt, to, sizeof Dt);
            if (mirror) {
                for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) if ((r == 0) != (c == 0)) Dr[3 * r + c] = -Dr[3 * r + c];
                Dt[0] = -Dt[0];
            }
            if (inv3(K0r, K0i)) {
                for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) Tr[3 * r + c] = Dr[3 * r] * K0i[c] + Dr[3 * r + 1] * K0i[3 + c] + Dr[3 * r + 2] * K0i[6 + c];
                for (int r = 0; r < 3; r++) Tt[r] = Dt[r] - (Tr[3 * r] * K0t[0] + Tr[3 * r + 1] * K0t[1] + Tr[3 * r + 2] * K0t[2]);
                off = 1;
            }
        }
        for (uint32_t k = 0; k < in->n_pieces; k++) {
            NsXf *w = &sc->piece[in->first_piece + k].world;
            float Kr[9], Kt[3];
            rel_to_hand(w, Ra, ta, mirror, Kr, Kt);
            if (off) {             /* K -> T * K */
                float Mr[9], Mt[3];
                for (int r = 0; r < 3; r++) {
                    for (int c = 0; c < 3; c++) Mr[3 * r + c] = Tr[3 * r] * Kr[c] + Tr[3 * r + 1] * Kr[3 + c] + Tr[3 * r + 2] * Kr[6 + c];
                    Mt[r] = Tr[3 * r] * Kt[0] + Tr[3 * r + 1] * Kt[1] + Tr[3 * r + 2] * Kt[2] + Tt[r];
                }
                memcpy(Kr, Mr, sizeof Kr); memcpy(Kt, Mt, sizeof Kt);
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
int ns_wphide_carry(NsScene *sc, const uint8_t *ram, const float Ra[9], const float ta[3], const float Rd[9], const float td[3], int mirror) {
    return ns_wphide_carry_off(sc, ram, Ra, ta, Rd, td, mirror, NULL, NULL);
}

int ns_wphide_has(const NsScene *sc, uint32_t key) {
    if (!sc || !key || !sc->inst || !sc->piece) return 0;
    for (uint32_t i = 0; i < sc->n_inst; i++) {
        const NsInstance *in = &sc->inst[i];
        if (in->key == key && in->n_pieces > 0 && (uint64_t)in->first_piece + in->n_pieces <= sc->n_piece) return 1;
    }
    return 0;
}

int ns_wphide_pos(const NsScene *sc, const uint8_t *ram, float p[3]) {
    if (!sc || !ram || !p) return 0;
    const uint32_t key = ns_wphide_key(ram);
    if (!ns_wphide_has(sc, key)) return 0;
    for (uint32_t i = 0; i < sc->n_inst; i++) {
        const NsInstance *in = &sc->inst[i];
        if (in->key != key || in->n_pieces == 0 || (uint64_t)in->first_piece + in->n_pieces > sc->n_piece) continue;
        const NsXf *w = &sc->piece[in->first_piece].world;
        if (!isfinite(w->m[3]) || !isfinite(w->m[7]) || !isfinite(w->m[11])) return 0;
        p[0] = w->m[3]; p[1] = w->m[7]; p[2] = w->m[11];
        return 1;
    }
    return 0;
}
