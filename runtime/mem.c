/* Memoria PS1 para código recompilado. Host little-endian (arm64/x86-64). */
#include <stdio.h>
#include <string.h>
#include "cpu.h"

uint8_t psx_ram[PSX_RAM_SIZE];
uint8_t psx_scratch[0x400];
static uint32_t io_regs[0x2000 / 4];

static inline uint32_t phys(uint32_t a) { return a & 0x1FFFFFFFu; }

void *mem_ptr(uint32_t a) {
    uint32_t p = phys(a);
    if (p < 0x800000u) return &psx_ram[p & (PSX_RAM_SIZE - 1)];
    if (p - PSX_SCRATCH < 0x400u) return &psx_scratch[p - PSX_SCRATCH];
    return NULL;
}

__attribute__((weak)) uint32_t io_read(uint32_t a, int size) {
    uint32_t off = (phys(a) - PSX_IO) & ~3u;
    uint32_t v = io_regs[off / 4] >> ((phys(a) & 3) * 8);
    static uint8_t seen[0x2000 / 4];
    if (!seen[off / 4]++) fprintf(stderr, "[io] lectura %d B en 0x%08x (sin HLE)\n", size, a);
    return size == 4 ? v : size == 2 ? (v & 0xFFFF) : (v & 0xFF);
}

__attribute__((weak)) void io_write(uint32_t a, uint32_t v, int size) {
    uint32_t off = (phys(a) - PSX_IO) & ~3u;
    if (size == 4) io_regs[off / 4] = v;
    else {
        int sh = (phys(a) & 3) * 8;
        uint32_t m = (size == 2 ? 0xFFFFu : 0xFFu) << sh;
        io_regs[off / 4] = (io_regs[off / 4] & ~m) | ((v << sh) & m);
    }
}

static void bad(Cpu *c, uint32_t a, const char *op) {
    static unsigned n;
    if (n++ < 32) fprintf(stderr, "[mem] %s fuera de mapa 0x%08x (ra=0x%08x)\n", op, a, c->r[31]);
}

static inline int is_io(uint32_t p) { return p - PSX_IO < 0x2000u; }

#define READ(T, SZ)                                                     \
    void *h = mem_ptr(a);                                               \
    if (h) { T v; memcpy(&v, h, sizeof v); return v; }                  \
    if (is_io(phys(a))) return (T)io_read(a, SZ);                       \
    if (phys(a) >= 0x1FC00000u && phys(a) < 0x1FC80000u) return 0;     \
    bad(c, a, "lectura"); return 0;

uint8_t  mem_r8(Cpu *c, uint32_t a)  { READ(uint8_t, 1) }
uint16_t mem_r16(Cpu *c, uint32_t a) { READ(uint16_t, 2) }
uint32_t mem_r32(Cpu *c, uint32_t a) { READ(uint32_t, 4) }

#define WRITE(SZ)                                                       \
    void *h = mem_ptr(a);                                               \
    if (h) { memcpy(h, &v, sizeof v); return; }                         \
    if (is_io(phys(a))) { io_write(a, v, SZ); return; }                 \
    if (a == 0xFFFE0130u) return; /* control de caché */                \
    bad(c, a, "escritura");

void mem_w8(Cpu *c, uint32_t a, uint8_t v)   { WRITE(1) }
void mem_w16(Cpu *c, uint32_t a, uint16_t v) { WRITE(2) }
void mem_w32(Cpu *c, uint32_t a, uint32_t v) { WRITE(4) }

/* Accesos no alineados (little-endian), semántica R3000. */
uint32_t mem_lwl(Cpu *c, uint32_t a, uint32_t cur) {
    unsigned s = a & 3;
    uint32_t w = mem_r32(c, a & ~3u);
    return (cur & (0x00FFFFFFu >> (s * 8))) | (w << (24 - s * 8));
}

uint32_t mem_lwr(Cpu *c, uint32_t a, uint32_t cur) {
    unsigned s = a & 3;
    uint32_t w = mem_r32(c, a & ~3u);
    uint32_t keep = s ? (0xFFFFFFFFu << ((4 - s) * 8)) : 0;
    return (cur & keep) | (w >> (s * 8));
}

void mem_swl(Cpu *c, uint32_t a, uint32_t v) {
    unsigned s = a & 3;
    uint32_t m = mem_r32(c, a & ~3u);
    uint32_t keep = s == 3 ? 0 : (0xFFFFFF00u << (s * 8));
    mem_w32(c, a & ~3u, (m & keep) | (v >> (24 - s * 8)));
}

void mem_swr(Cpu *c, uint32_t a, uint32_t v) {
    unsigned s = a & 3;
    uint32_t m = mem_r32(c, a & ~3u);
    uint32_t keep = s ? (0xFFFFFFFFu >> ((4 - s) * 8)) : 0;
    mem_w32(c, a & ~3u, (m & keep) | (v << (s * 8)));
}
