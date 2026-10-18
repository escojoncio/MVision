#pragma once
#include <stdint.h>

typedef struct Cpu {
    uint32_t r[32];
    uint32_t hi, lo;
} Cpu;

typedef void (*FuncPtr)(Cpu *c);
typedef struct { uint32_t addr; FuncPtr fn; } FuncEntry;
typedef struct { uint32_t addr; const char *name; } FuncName;

/* Generado por tools/recomp/recomp.py (dispatch.c). Tabla ordenada por dirección. */
extern const FuncEntry rt_func_table[];
extern const unsigned rt_func_count;
extern const FuncName rt_func_names[];
extern const unsigned rt_func_name_count;
extern const uint32_t rt_entry;

/* Traza opcional de entrada a funciones (compilar con -DRT_TRACE). */
#ifdef RT_TRACE
void rt_trace_enter(Cpu *c, uint32_t addr);
#define RT_ENTER(a) rt_trace_enter(c, (a))
#else
#define RT_ENTER(a) ((void)0)
#endif

/* ---- Memoria PS1: RAM 2 MB (espejos KUSEG/KSEG0/KSEG1 x4), scratchpad 1 KB, E/S. ---- */
#define PSX_RAM_SIZE   0x200000u
#define PSX_SCRATCH    0x1F800000u
#define PSX_IO         0x1F801000u
extern uint8_t psx_ram[PSX_RAM_SIZE];
extern uint8_t psx_scratch[0x400];

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
/* Puntero host a RAM/scratchpad (NULL si no es memoria directa). */
void *mem_ptr(uint32_t a);

/* Ganchos de E/S (registros hardware 0x1F801000-0x1F802FFF). Por defecto: registro plano + log. */
uint32_t io_read(uint32_t a, int size);
void io_write(uint32_t a, uint32_t v, int size);

/* ---- Control de flujo fuera de lo conocido estáticamente. ---- */
FuncPtr rt_lookup(uint32_t addr);
void rt_call(Cpu *c, uint32_t target);
void rt_jump(Cpu *c, uint32_t target);
void rt_fallthrough(Cpu *c, uint32_t target);
void rt_unimpl(Cpu *c, uint32_t pc, uint32_t insn);
void rt_syscall(Cpu *c, uint32_t pc);
void rt_break(Cpu *c, uint32_t pc);
void rt_div(Cpu *c, uint32_t a, uint32_t b);
void rt_divu(Cpu *c, uint32_t a, uint32_t b);
/* Registro de tablas extra (overlays cargados en RAM). */
void rt_register_table(const FuncEntry *t, unsigned n);
void rt_clear_tables(void);
const char *rt_name(uint32_t addr);
_Noreturn void rt_fatal(Cpu *c, const char *fmt, ...);

/* BIOS HLE (vectores A0/B0/C0, función en t1). */
void bios_hle(Cpu *c, uint32_t vec);

/* ---- COP0 / GTE ---- */
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
void gte_reset(void);
