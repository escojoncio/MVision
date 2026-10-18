/* Pruebas unitarias del runtime (sin datos del juego). */
#include <stdio.h>
#include <stdint.h>
#include "cpu.h"
#include "gte_vectors.h"

const FuncEntry rt_func_table[] = {{0, 0}};
const unsigned rt_func_count = 0;
const FuncName rt_func_names[] = {{0xFFFFFFFFu, ""}};
const unsigned rt_func_name_count = 0;
const uint32_t rt_entry = 0;

static int fails;
#define EQ(a, b) do { unsigned long long _a = (a), _b = (b); \
    if (_a != _b) { printf("FALLO %s:%d %s = 0x%llx, esperado 0x%llx\n", __FILE__, __LINE__, #a, _a, _b); fails++; } } while (0)

static void t_mem(Cpu *c) {
    mem_w32(c, 0x80001000, 0x44332211);
    mem_w32(c, 0x80001004, 0x88776655);
    EQ(mem_r32(c, 0x00001000), 0x44332211);         /* espejo KUSEG */
    EQ(mem_r32(c, 0xA0601000), 0x44332211);         /* espejo KSEG1 + 6 MB */
    EQ(mem_r16(c, 0x80001002), 0x4433);
    /* lwr/lwl para leer 0x80001001 no alineado = 0x55443322 */
    uint32_t v = mem_lwr(c, 0x80001001, 0xAAAAAAAA);
    v = mem_lwl(c, 0x80001004, v);
    EQ(v, 0x55443322);
    /* swr/swl para escribir 0xDDCCBBAA en 0x80001001 */
    mem_swr(c, 0x80001001, 0xDDCCBBAA);
    mem_swl(c, 0x80001004, 0xDDCCBBAA);
    EQ(mem_r32(c, 0x80001000), 0xCCBBAA11);
    EQ(mem_r32(c, 0x80001004), 0x887766DD);
    mem_w8(c, 0x1F800010, 0x7F);
    EQ(mem_r8(c, 0x1F800010), 0x7F);
}

static void t_div(Cpu *c) {
    rt_div(c, (uint32_t)-7, 2); EQ(c->lo, (uint32_t)-3); EQ(c->hi, (uint32_t)-1);
    rt_div(c, 5, 0); EQ(c->lo, 0xFFFFFFFF); EQ(c->hi, 5);
    rt_div(c, (uint32_t)-5, 0); EQ(c->lo, 1);
    rt_div(c, 0x80000000u, (uint32_t)-1); EQ(c->lo, 0x80000000u); EQ(c->hi, 0);
    rt_divu(c, 7, 0); EQ(c->lo, 0xFFFFFFFF); EQ(c->hi, 7);
}

static void t_gte(Cpu *c) {
    gte_reset();
    gte_ctc2(c, 0, 0x1000); gte_ctc2(c, 1, 0); gte_ctc2(c, 2, 0x1000); gte_ctc2(c, 3, 0); gte_ctc2(c, 4, 0x1000);
    gte_ctc2(c, 5, 0); gte_ctc2(c, 6, 0); gte_ctc2(c, 7, 1000);
    gte_ctc2(c, 24, 0); gte_ctc2(c, 25, 0); gte_ctc2(c, 26, 1000);
    gte_mtc2(c, 0, (50u << 16) | 100); gte_mtc2(c, 1, 0);
    gte_cmd(c, 0x00180001); /* RTPS sf=1 */
    EQ(gte_mfc2(c, 19), 1000);                     /* SZ3 */
    EQ(gte_mfc2(c, 14), (50u << 16) | 100);         /* SXY2 */
    EQ(gte_cfc2(c, 31), 0);                         /* FLAG */
    EQ(gte_cfc2(c, 26), 1000);                      /* H se lee con signo */
    /* Proyección con división: H=500, Z=1000 -> 0.5 */
    gte_ctc2(c, 26, 500);
    gte_cmd(c, 0x00180001);
    EQ(gte_mfc2(c, 14), (25u << 16) | 50);
    /* Desbordamiento de división: Z pequeño */
    gte_ctc2(c, 7, 100); gte_ctc2(c, 26, 1000);
    gte_cmd(c, 0x00180001);
    EQ((gte_cfc2(c, 31) >> 17) & 1, 1);
    EQ(gte_cfc2(c, 31) >> 31, 1);
    /* NCLIP: triángulo CCW (0,0) (10,0) (0,10) -> 100 */
    gte_mtc2(c, 12, 0); gte_mtc2(c, 13, 10); gte_mtc2(c, 14, 10u << 16);
    gte_cmd(c, 0x00000006);
    EQ(gte_mfc2(c, 24), 100);
    /* AVSZ3 */
    gte_mtc2(c, 17, 300); gte_mtc2(c, 18, 600); gte_mtc2(c, 19, 900);
    gte_ctc2(c, 29, 0x555);                         /* ~1/3 en 1.12 */
    gte_cmd(c, 0x0000002D);
    EQ(gte_mfc2(c, 7), (0x555 * 1800) >> 12);
    /* LZCS/LZCR */
    gte_mtc2(c, 30, 0x00F00000); EQ(gte_mfc2(c, 31), 8);
    gte_mtc2(c, 30, 0xFFFF0000); EQ(gte_mfc2(c, 31), 16);
    /* IRGB -> IR */
    gte_mtc2(c, 28, 0x7FFF); EQ(gte_mfc2(c, 9), 0xF80); EQ(gte_mfc2(c, 29), 0x7FFF);
    /* SQR */
    gte_mtc2(c, 9, 0x10); gte_mtc2(c, 10, (uint32_t)-0x20); gte_mtc2(c, 11, 0x7FFF);
    gte_cmd(c, 0x00000428); /* sf=0, lm=1 */
    EQ(gte_mfc2(c, 9), 0x100); EQ(gte_mfc2(c, 10), 0x400); EQ(gte_mfc2(c, 11), 0x7FFF);
    EQ((gte_cfc2(c, 31) >> 22) & 1, 1);            /* IR3 saturado */
}

/* ---- vectores diferenciales de la GTE ----
 * Cada caso se reconstruye de forma determinista desde (cmd, semilla): se rellenan los registros de
 * datos y control con valores pseudoaleatorios (xorshift64), se ejecuta el comando y se compara un hash
 * FNV-1a de los 64 registros de salida (datos vía mfc2 y control vía cfc2, incluido FLAG), más FLAG y
 * MAC0 en claro para facilitar el diagnóstico. Los valores esperados (gte_vectors.h) son una instantánea
 * del comportamiento verificado de gte.c: detectan regresiones. */
static uint64_t gv_s;
static uint32_t gv_rnd(void) { gv_s ^= gv_s << 13; gv_s ^= gv_s >> 7; gv_s ^= gv_s << 17; return (uint32_t)(gv_s >> 16); }
static uint32_t gv_gen(int mode) {
    uint32_t r = gv_rnd();
    static const uint32_t ext[16] = {0, 1, 0xFFFFFFFFu, 0x7FFF, 0x8000, 0xFFFF8000u, 0x7FFFFFFFu, 0x80000000u,
                                     0xFFFF, 0x10000, 0x1000, 0xFFFFF000u, 0x7FFF0000u, 0x80000000u, 0x8000, 0x0FFF};
    switch (mode) {
    case 0: return r;
    case 1: return (uint32_t)(int32_t)(int16_t)r;
    case 2: return ext[r & 15];
    case 3: return (uint32_t)((int32_t)(r % 0x2001) - 0x1000);
    default: return (uint32_t)((int32_t)(r % 65536) - 32768) * ((gv_rnd() & 1) ? 0x10001u : 1u);
    }
}
static uint32_t gv_run(Cpu *c, uint32_t cmd, uint32_t seed, uint32_t *flag, uint32_t *mac0) {
    gv_s = 88172645463325252ull ^ ((uint64_t)seed * 0x9E3779B97F4A7C15ull);
    gte_reset();
    static const int modes[8] = {3, 3, 3, 1, 4, 0, 2, 3};
    int mode = modes[seed & 7];
    for (int r = 0; r < 31; r++) gte_ctc2(c, r, gv_gen((gv_rnd() & 3) == 0 ? (int)(gv_rnd() % 5) : mode));
    for (int r = 0; r < 31; r++) {
        if (r == 15 || r == 28 || r == 29) continue;
        gte_mtc2(c, r, gv_gen((gv_rnd() & 3) == 0 ? (int)(gv_rnd() % 5) : mode));
    }
    if (gv_rnd() & 1) gte_mtc2(c, 28, gv_rnd());
    if (gv_rnd() & 1) {
        uint32_t hv = gv_rnd() % 0x2000, zr = 16 + gv_rnd() % 4, zv = gv_rnd() % 0x3000;   /* orden fijo */
        gte_ctc2(c, 26, hv); gte_mtc2(c, (int)zr, zv);
    }
    gte_cmd(c, cmd);
    uint32_t h = 2166136261u;
    for (int r = 0; r < 64; r++) {
        uint32_t v = r < 32 ? gte_mfc2(c, r) : gte_cfc2(c, r - 32);
        for (int b = 0; b < 4; b++) { h ^= (v >> (8 * b)) & 0xFF; h *= 16777619u; }
    }
    *flag = gte_cfc2(c, 31);
    *mac0 = gte_mfc2(c, 24);
    return h;
}

static void t_gte_vec(Cpu *c) {
    unsigned n = sizeof gte_vectors / sizeof gte_vectors[0];
    int malos = 0;
    for (unsigned i = 0; i < n; i++) {
        uint32_t flag, mac0;
        uint32_t h = gv_run(c, gte_vectors[i].cmd, gte_vectors[i].seed, &flag, &mac0);
        if (flag != gte_vectors[i].flag || mac0 != gte_vectors[i].mac0 || h != gte_vectors[i].hash) {
            if (malos++ < 8)
                printf("FALLO gte vector %u cmd=%08x: flag=%08x/%08x mac0=%08x/%08x hash=%08x/%08x\n", i,
                       gte_vectors[i].cmd, flag, gte_vectors[i].flag, mac0, gte_vectors[i].mac0, h,
                       gte_vectors[i].hash);
        }
    }
    EQ(malos, 0);
}

/* Los registros de 16 bits ignoran la mitad alta al escribirlos (p.ej. lwc2 de una palabra con dos
 * halfwords): los cálculos no deben verla. */
static void t_gte_regs16(Cpu *c) {
    gte_reset();
    gte_lwc2(c, 9, 0x00010003);                    /* IR1 = 3 aunque la mitad alta valga 1 */
    gte_lwc2(c, 10, 0xFFFF8000u);                  /* IR2 = -32768 */
    gte_lwc2(c, 11, 0x7FFF0004);                   /* IR3 = 4 */
    EQ(gte_mfc2(c, 9), 3); EQ(gte_mfc2(c, 10), 0xFFFF8000u); EQ(gte_mfc2(c, 11), 4);
    gte_cmd(c, 0x00000028);                        /* SQR sf=0 */
    EQ(gte_mfc2(c, 25), 9); EQ(gte_mfc2(c, 26), 0x40000000u); EQ(gte_mfc2(c, 27), 16);
    gte_lwc2(c, 8, 0x12341000);                    /* IR0 = 0x1000 */
    gte_lwc2(c, 19, 0xABCD0100);                   /* SZ3 = 0x100 sin signo */
    EQ(gte_mfc2(c, 8), 0x1000); EQ(gte_mfc2(c, 19), 0x100);
    gte_lwc2(c, 7, 0xFFFF8000u); EQ(gte_mfc2(c, 7), 0x8000);   /* OTZ sin signo */
    gte_lwc2(c, 1, 0xFFFF8000u); EQ(gte_mfc2(c, 1), 0xFFFF8000u); /* VZ0 con signo */
    gte_lwc2(c, 16, 0x7FFF0010);                   /* SZ0 */
    gte_lwc2(c, 17, 0x00010020); gte_lwc2(c, 18, 0x00010030);
    gte_ctc2(c, 29, 0x1000);
    gte_cmd(c, 0x0000002D);                        /* AVSZ3 con SZ1..3 = 0x20,0x30,0x100 */
    EQ(gte_mfc2(c, 7), 0x20 + 0x30 + 0x100);
}

int main(void) {
    Cpu c = {0};
    t_mem(&c);
    t_div(&c);
    t_gte(&c);
    t_gte_regs16(&c);
    t_gte_vec(&c);
    printf(fails ? "%d fallos\n" : "runtime: todas las pruebas OK\n", fails);
    return fails != 0;
}
