#pragma once
#include <stdint.h>

typedef struct Cpu {
    uint32_t r[32];
    uint32_t hi, lo;
} Cpu;

typedef void (*FuncPtr)(Cpu *c);
typedef struct { uint32_t addr; FuncPtr fn; } FuncEntry;
extern const FuncEntry rt_func_table[];
extern const unsigned rt_func_count;
extern const uint32_t rt_entry;

/* Memoria PS1: RAM 2 MB (espejada), scratchpad 1 KB, E/S. */
uint8_t  mem_r8(Cpu *c, uint32_t a);
uint16_t mem_r16(Cpu *c, uint32_t a);
uint32_t mem_r32(Cpu *c, uint32_t a);
void mem_w8(Cpu *c, uint32_t a, uint8_t v);
void mem_w16(Cpu *c, uint32_t a, uint16_t v);
void mem_w32(Cpu *c, uint32_t a, uint32_t v);
uint32_t mem_lwl(Cpu *c, uint32_t a, uint32_t cur);
uint32_t mem_lwr(Cpu *c, uint32_t a, uint32_t cur);
void mem_swl(Cpu *c, uint32_t a, uint32_t v);
void mem_swr(Cpu *c, uint32_t a, uint32_t v);

/* Control de flujo fuera de lo conocido estáticamente. */
void rt_call(Cpu *c, uint32_t target);
void rt_jump(Cpu *c, uint32_t target);
void rt_fallthrough(Cpu *c, uint32_t target);
void rt_unimpl(Cpu *c, uint32_t pc, uint32_t insn);
void rt_syscall(Cpu *c, uint32_t pc);
void rt_break(Cpu *c, uint32_t pc);
void rt_div(Cpu *c, uint32_t a, uint32_t b);
void rt_divu(Cpu *c, uint32_t a, uint32_t b);

/* COP0 / GTE */
uint32_t cop0_read(Cpu *c, int r);
void cop0_write(Cpu *c, int r, uint32_t v);
void cop0_rfe(Cpu *c);
void gte_cmd(Cpu *c, uint32_t cmd);
uint32_t gte_mfc2(Cpu *c, int r);
uint32_t gte_cfc2(Cpu *c, int r);
void gte_mtc2(Cpu *c, int r, uint32_t v);
void gte_ctc2(Cpu *c, int r, uint32_t v);
void gte_lwc2(Cpu *c, int r, uint32_t v);
uint32_t gte_swc2(Cpu *c, int r);
