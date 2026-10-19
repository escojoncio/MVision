/* Parcheo en ejecución del código en RAM: ver rtpatch.h. */
#include "rtpatch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hlebind.h"

uint8_t rt_patch_bits[EMU_RAM_SIZE / 32];

typedef struct {
    uint32_t addr;          /* 0 = libre */
    FuncPtr fn;
    RtPatchGuard g;
    uint32_t retired;       /* dirección de la última vida de la ranura (bloques ya decodificados que aún la llamen) */
    uint8_t has_guard, stale;
} Slot;
static Slot S[RT_PATCH_MAX];

typedef struct { uint32_t addr; unsigned n; uint32_t *orig, *neu; } CodePatch;
#define CODE_MAX 64
static CodePatch CP[CODE_MAX];

static uint32_t norm(uint32_t a) { return (a & 0x1FFFFFFFu) | 0x80000000u; }
static int addr_ok(uint32_t a, unsigned nwords) {
    uint32_t o = a & 0x1FFFFFFFu;
    return !(a & 3) && o >= 0x10000u && (uint64_t)o + 4ull * nwords <= EMU_RAM_SIZE;
}
static uint32_t rd(uint32_t a) {
    uint32_t v;
    memcpy(&v, emu_ram + (a & 0x1FFFFFFFu), 4);
    return v;
}

uint64_t rt_code_hash(uint32_t addr, unsigned nwords) {
    if (!nwords || !addr_ok(addr, nwords)) return 0;
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned i = 0; i < nwords; i++) {
        uint32_t w = rd(addr + 4 * i);
        for (int k = 0; k < 4; k++) { h ^= (uint8_t)(w >> (8 * k)); h *= 0x100000001b3ull; }
    }
    return h;
}

static void bit_set(uint32_t a, int on) {
    uint32_t w = (a & (EMU_RAM_SIZE - 1)) >> 2;
    if (on) rt_patch_bits[w >> 3] |= (uint8_t)(1u << (w & 7));
    else rt_patch_bits[w >> 3] &= (uint8_t)~(1u << (w & 7));
}

static Slot *find(uint32_t a) {
    a = norm(a);
    for (int i = 0; i < RT_PATCH_MAX; i++) if (S[i].addr == a) return &S[i];
    return NULL;
}

int rt_patched(uint32_t addr) { Slot *s = find(addr); return s && !s->stale; }
int rt_patch_stale(uint32_t addr) { Slot *s = find(addr); return s && s->stale; }
unsigned rt_patch_count(void) { unsigned n = 0; for (int i = 0; i < RT_PATCH_MAX; i++) n += S[i].addr != 0; return n; }

/* Cada ranura tiene su propia función de entrada (FuncPtr no lleva contexto): comprueba la guarda y despacha. */
static void run_slot(int i, Cpu *c) {
    Slot *s = &S[i];
    if (!s->addr) { interp_call_raw(c, s->retired); return; }   /* parche retirado con un bloque decodificado aún en vuelo */
    if (s->has_guard && s->g.hash != rt_code_hash(s->addr, s->g.nwords)) {
        s->stale = 1;
        interp_call_raw(c, s->addr);
        return;
    }
    s->stale = 0;
    s->fn(c);
}
#define THUNK(n) static void thunk_##n(Cpu *c) { run_slot(n, c); }
THUNK(0) THUNK(1) THUNK(2) THUNK(3) THUNK(4) THUNK(5) THUNK(6) THUNK(7) THUNK(8) THUNK(9) THUNK(10) THUNK(11)
THUNK(12) THUNK(13) THUNK(14) THUNK(15) THUNK(16) THUNK(17) THUNK(18) THUNK(19) THUNK(20) THUNK(21) THUNK(22) THUNK(23)
static const FuncPtr thunks[RT_PATCH_MAX] = {
    thunk_0, thunk_1, thunk_2, thunk_3, thunk_4, thunk_5, thunk_6, thunk_7, thunk_8, thunk_9, thunk_10, thunk_11,
    thunk_12, thunk_13, thunk_14, thunk_15, thunk_16, thunk_17, thunk_18, thunk_19, thunk_20, thunk_21, thunk_22, thunk_23};

/* Lo que ejecuta el intérprete en la entrada: la función de la ranura (comprueba la guarda y despacha). */
FuncPtr rt_patch_lookup(uint32_t addr) {
    addr = norm(addr);
    for (int i = 0; i < RT_PATCH_MAX; i++) if (S[i].addr == addr) return thunks[i];
    return NULL;
}

int rt_patch_fn(uint32_t addr, FuncPtr fn, const RtPatchGuard *g) {
    if (!fn || !addr_ok(addr, 1)) return RT_PATCH_BAD_ADDR;
    addr = norm(addr);
    if (hle_bind_is(addr)) return RT_PATCH_BUSY;
    if (g) {
        if (g->nwords == 0 || g->nwords > 4096 || !addr_ok(addr, g->nwords)) return RT_PATCH_BAD_ADDR;
        if (rt_code_hash(addr, g->nwords) != g->hash) return RT_PATCH_HASH;
    }
    Slot *s = find(addr);
    if (!s) {
        for (int i = 0; i < RT_PATCH_MAX; i++) if (!S[i].addr) { s = &S[i]; break; }
        if (!s) return RT_PATCH_FULL;
    }
    int fresh = s->addr == 0;
    s->retired = 0;
    s->addr = addr; s->fn = fn; s->stale = 0;
    s->has_guard = g != NULL;
    if (g) s->g = *g; else memset(&s->g, 0, sizeof s->g);
    if (fresh) { bit_set(addr, 1); interp_forget(addr); }
    return RT_PATCH_OK;
}
int rt_patch(uint32_t addr, FuncPtr fn) { return rt_patch_fn(addr, fn, NULL); }

void rt_unpatch(uint32_t addr) {
    Slot *s = find(addr);
    if (!s) return;
    uint32_t a = s->addr;
    bit_set(a, 0);
    memset(s, 0, sizeof *s);
    s->retired = a;
    interp_forget(a);
}
void rt_unpatch_all(void) {
    for (int i = 0; i < RT_PATCH_MAX; i++) if (S[i].addr) rt_unpatch(S[i].addr);
}

void rt_call_orig(Cpu *c, uint32_t addr) { interp_call_raw(c, addr); }

/* ------------------------------------------------------------------ parches de palabras */
int rt_patch_code(uint32_t addr, unsigned n, const uint32_t *expect, const uint32_t *neu, int *id) {
    if (!n || n > 1024 || !expect || !neu || !addr_ok(addr, n)) return RT_PATCH_BAD_ADDR;
    for (unsigned i = 0; i < n; i++) if (rd(addr + 4 * i) != expect[i]) return RT_PATCH_HASH;
    int k = -1;
    for (int i = 0; i < CODE_MAX; i++) if (!CP[i].addr) { k = i; break; }
    if (k < 0) return RT_PATCH_FULL;
    uint32_t *buf = malloc(sizeof(uint32_t) * 2 * n);
    if (!buf) return RT_PATCH_FULL;
    memcpy(buf, expect, 4 * n); memcpy(buf + n, neu, 4 * n);
    CP[k].addr = norm(addr); CP[k].n = n; CP[k].orig = buf; CP[k].neu = buf + n;
    memcpy(emu_ram + (addr & 0x1FFFFFFFu), neu, 4 * n);
    rt_code_invalidate(addr, 4 * n);
    if (id) *id = k;
    return RT_PATCH_OK;
}
int rt_unpatch_code(int id) {
    if (id < 0 || id >= CODE_MAX || !CP[id].addr) return -1;
    CodePatch *p = &CP[id];
    int same = 1;
    for (unsigned i = 0; i < p->n; i++) if (rd(p->addr + 4 * i) != p->neu[i]) { same = 0; break; }
    if (same) {
        memcpy(emu_ram + (p->addr & 0x1FFFFFFFu), p->orig, 4 * p->n);
        rt_code_invalidate(p->addr, 4 * p->n);
    }
    free(p->orig);
    memset(p, 0, sizeof *p);
    return same ? 0 : 1;
}
void rt_unpatch_code_all(void) { for (int i = 0; i < CODE_MAX; i++) if (CP[i].addr) rt_unpatch_code(i); }
unsigned rt_patch_code_count(void) { unsigned n = 0; for (int i = 0; i < CODE_MAX; i++) n += CP[i].addr != 0; return n; }
