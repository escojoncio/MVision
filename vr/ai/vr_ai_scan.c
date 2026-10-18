#include "vr_ai_scan.h"

static uint32_t mk(uint32_t a) { return a & (VAI_RAM_SIZE - 1); }

uint32_t vai_r32(const uint8_t *ram, uint32_t a) {
    const uint8_t *p = ram + (mk(a) & ~3u);
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
int16_t vai_r16(const uint8_t *ram, uint32_t a) {
    const uint8_t *p = ram + (mk(a) & ~1u);
    return (int16_t)(uint16_t)(p[0] | p[1] << 8);
}
uint8_t vai_r8(const uint8_t *ram, uint32_t a) { return ram[mk(a)]; }
void vai_w8(uint8_t *ram, uint32_t a, uint8_t v) { ram[mk(a)] = v; }
void vai_w16(uint8_t *ram, uint32_t a, int16_t v) {
    uint8_t *p = ram + (mk(a) & ~1u);
    p[0] = (uint8_t)(uint16_t)v; p[1] = (uint8_t)((uint16_t)v >> 8);
}
void vai_w32(uint8_t *ram, uint32_t a, uint32_t v) {
    uint8_t *p = ram + (mk(a) & ~3u);
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

uint32_t vai_jal(uint32_t t) { return (3u << 26) | ((t >> 2) & 0x3FFFFFFu); }
int vai_is_jal(uint32_t w) { return (w >> 26) == 3; }
uint32_t vai_jal_target(uint32_t w) { return 0x80000000u | ((w & 0x3FFFFFFu) << 2); }
int vai_is_prologue(uint32_t w) { return (w & 0xFFFF8000u) == 0x27BD8000u; }

int vai_func_bounds(const uint8_t *ram, uint32_t inside, uint32_t lo, uint32_t hi, uint32_t *start, uint32_t *end) {
    if (!ram || inside < lo || inside >= hi || hi > 0x80000000u + VAI_RAM_SIZE || (lo & 3) || (hi & 3)) return 0;
    uint32_t s = inside & ~3u;
    while (s >= lo && !vai_is_prologue(vai_r32(ram, s))) { if (s == lo) return 0; s -= 4; }
    if (s < lo) return 0;
    uint32_t e = (inside & ~3u) + 4;
    while (e < hi && !vai_is_prologue(vai_r32(ram, e))) e += 4;
    while (e > s + 4 && vai_r32(ram, e - 4) == 0) e -= 4;   /* cola de nops de alineación */
    *start = s; *end = e;
    return 1;
}

uint64_t vai_shape_hash(const uint8_t *ram, uint32_t start, uint32_t end) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t a = start; a < end; a += 4) {
        uint32_t w = vai_r32(ram, a), op = w >> 26;
        uint32_t k = op == 0 ? (w & 0x3F) | 0x100u : op;
        h = (h ^ k) * 1099511628211ull;
    }
    return h;
}

int vai_count_word(const uint8_t *ram, uint32_t start, uint32_t end, uint32_t w) {
    int n = 0;
    for (uint32_t a = start; a < end; a += 4) n += vai_r32(ram, a) == w;
    return n;
}
