/* Pruebas unitarias del runtime (sin datos del juego). */
#include <stdio.h>
#include "cpu.h"

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

int main(void) {
    Cpu c = {0};
    t_mem(&c);
    t_div(&c);
    t_gte(&c);
    printf(fails ? "%d fallos\n" : "runtime: todas las pruebas OK\n", fails);
    return fails != 0;
}
