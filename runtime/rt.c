/* Núcleo del runtime: despacho de direcciones, división, COP0, syscall, errores fatales, traza. */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"

#define MAX_TABLES 8
static struct { const FuncEntry *t; unsigned n; } tables[MAX_TABLES];
static unsigned ntables;

void rt_register_table(const FuncEntry *t, unsigned n) {
    if (ntables == MAX_TABLES) { fprintf(stderr, "[rt] demasiadas tablas\n"); abort(); }
    tables[ntables].t = t; tables[ntables].n = n; ntables++;
}
void rt_clear_tables(void) { ntables = 0; }

static FuncPtr find(const FuncEntry *t, unsigned n, uint32_t a) {
    unsigned lo = 0, hi = n;
    while (lo < hi) {
        unsigned m = (lo + hi) / 2;
        if (t[m].addr < a) lo = m + 1; else hi = m;
    }
    return lo < n && t[lo].addr == a ? t[lo].fn : NULL;
}

FuncPtr rt_lookup(uint32_t a) {
    a = (a & 0x1FFFFFFFu) | 0x80000000u; /* KUSEG/KSEG1 -> KSEG0 */
    FuncPtr f = find(rt_func_table, rt_func_count, a);
    for (unsigned i = ntables; !f && i-- > 0;) f = find(tables[i].t, tables[i].n, a);
    return f;
}

/* Tabla vacía por defecto (pruebas); el recompilador la define en dispatch.c. */
__attribute__((weak)) const HleOrig rt_hle_orig[] = {{0, 0}};
__attribute__((weak)) const unsigned rt_hle_orig_count = 0;

int rt_orig(Cpu *c, const char *name) {
    for (unsigned i = 0; i < rt_hle_orig_count; i++)
        if (!strcmp(rt_hle_orig[i].name, name)) { rt_hle_orig[i].fn(c); return 1; }
    return 0;
}

const char *rt_name(uint32_t a) {
    unsigned lo = 0, hi = rt_func_name_count;
    while (lo < hi) {
        unsigned m = (lo + hi) / 2;
        if (rt_func_names[m].addr < a) lo = m + 1; else hi = m;
    }
    return lo < rt_func_name_count && rt_func_names[lo].addr == a ? rt_func_names[lo].name : "?";
}

#include "fiber.h"
#include "sys.h"
/* --- traza circular (RT_TRACE) --- */
#define TRACE_N 64
static uint32_t trace_ring[TRACE_N];
static unsigned trace_pos;
/* MV_TRACE=N imprime las N primeras entradas a funciones (diagnóstico). */
void rt_dump_trace(void);
void rt_trace_enter(Cpu *c, uint32_t a) {
    static long lim = -1, shown, from;
    if (lim < 0) {
        const char *e = getenv("MV_TRACE"), *f = getenv("MV_TRACE_FROM");
        lim = e ? atol(e) : 0;
        from = f ? atol(f) : 0;   /* MV_TRACE_FROM=N: empezar en el VBlank N */
    }
    if (shown < lim && (long)sys_vblank_count() >= from) { shown++; fprintf(stderr, "[tr] %08x %s t%u %08x %08x %08x\n", a, rt_name(a), fiber_current() & 0xFF, c->r[4], c->r[5], c->r[6]); }
    trace_ring[trace_pos++ % TRACE_N] = a;
}

void rt_dump_trace(void) {
    if (!trace_pos) return;
    fprintf(stderr, "[rt] últimas funciones:\n");
    unsigned n = trace_pos < TRACE_N ? trace_pos : TRACE_N;
    for (unsigned i = trace_pos - n; i < trace_pos; i++) {
        uint32_t a = trace_ring[i % TRACE_N];
        fprintf(stderr, "    0x%08x %s\n", a, rt_name(a));
    }
}

jmp_buf rt_fatal_jmp;
int rt_fatal_armed;

_Noreturn void rt_fatal(Cpu *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[rt] FATAL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    if (c) {
        fprintf(stderr, "    ra=0x%08x (%s) sp=0x%08x v0=0x%08x a0=0x%08x a1=0x%08x\n",
                c->r[31], rt_name(c->r[31]), c->r[29], c->r[2], c->r[4], c->r[5]);
    }
    rt_dump_trace();
    if (rt_fatal_armed) longjmp(rt_fatal_jmp, 1);
    exit(2);
}

static int is_bios_vec(uint32_t t) {
    t &= 0x1FFFFFFFu;
    return t == 0xA0 || t == 0xB0 || t == 0xC0;
}

/* Intérprete (interp.c): ejecuta desde t hasta volver a ra de entrada. */
void interp_call(Cpu *c, uint32_t t);

void rt_call(Cpu *c, uint32_t t) {
    if (is_bios_vec(t)) { bios_hle(c, t & 0xFF); return; }
    FuncPtr f = rt_lookup(t);
    if (f) f(c);
    else interp_call(c, t);
}

/* Salto en cola: misma semántica que una llamada, la función destino retorna por ra al llamante C. */
void rt_jump(Cpu *c, uint32_t t) { rt_call(c, t); }

void rt_fallthrough(Cpu *c, uint32_t t) { rt_call(c, t); }

/* ---- setjmp/longjmp del juego ----
 * En cada llamada a setjmp el código generado hace un setjmp del host en el marco C del llamante,
 * indexado por (hilo, dirección del jmp_buf MIPS). longjmp restaura los registros MIPS desde el
 * buffer en RAM (como el original) y hace longjmp del host a ese marco. Cada hilo tiene su pila
 * host (fiber.c), así que solo se permite saltar dentro del mismo hilo. */
#include "fiber.h"
#define SJ_MAX 32
static struct { uint32_t fiber, buf; int valid; jmp_buf env; } sj[SJ_MAX];
static int sj_armed; /* lo pone rt_sj_slot justo antes de la llamada directa a setjmp */

static int sj_find(uint32_t fb, uint32_t buf) {
    for (int i = 0; i < SJ_MAX; i++) if (sj[i].valid && sj[i].fiber == fb && sj[i].buf == buf) return i;
    return -1;
}

jmp_buf *rt_sj_slot(Cpu *c, uint32_t buf) {
    uint32_t fb = fiber_current();
    int i = sj_find(fb, buf);
    if (i < 0) for (i = 0; i < SJ_MAX && sj[i].valid; i++) {}
    if (i == SJ_MAX) rt_fatal(c, "demasiados setjmp activos");
    sj[i].fiber = fb; sj[i].buf = buf; sj[i].valid = 1;
    sj_armed = 1;
    return &sj[i].env;
}

/* Tras volver por longjmp: los registros ya se restauraron en rt_sj_longjmp. */
void rt_sj_resumed(Cpu *c) { (void)c; }

/* setjmp llamado sin punto de reanudación del host (vía puntero/jalr): se invalida el anterior. */
int rt_sj_take_armed(void) { int a = sj_armed; sj_armed = 0; return a; }
void rt_sj_forget(uint32_t buf) {
    int i = sj_find(fiber_current(), buf);
    if (i >= 0) sj[i].valid = 0;
}

_Noreturn void rt_sj_longjmp(Cpu *c, uint32_t buf, uint32_t val) {
    int i = sj_find(fiber_current(), buf);
    if (i < 0) rt_fatal(c, "longjmp(0x%08x) sin setjmp registrado en este hilo", buf);
    static const int regs[] = {31, 28, 29, 30, 16, 17, 18, 19, 20, 21, 22, 23};
    static const int offs[] = {0, 44, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40};
    for (int k = 0; k < 12; k++) c->r[regs[k]] = mem_r32(c, buf + offs[k]);
    c->r[2] = val;
    longjmp(sj[i].env, 1);
}

void rt_unimpl(Cpu *c, uint32_t pc, uint32_t insn) {
    rt_fatal(c, "instrucción no soportada 0x%08x en 0x%08x", insn, pc);
}

void rt_break(Cpu *c, uint32_t pc) {
    rt_fatal(c, "break en 0x%08x", pc);
}

void rt_div(Cpu *c, uint32_t a, uint32_t b) {
    int32_t n = (int32_t)a, d = (int32_t)b;
    if (d == 0) { c->hi = a; c->lo = n >= 0 ? 0xFFFFFFFFu : 1u; return; }
    if (n == INT32_MIN && d == -1) { c->hi = 0; c->lo = 0x80000000u; return; }
    c->lo = (uint32_t)(n / d);
    c->hi = (uint32_t)(n % d);
}

void rt_divu(Cpu *c, uint32_t a, uint32_t b) {
    if (b == 0) { c->hi = a; c->lo = 0xFFFFFFFFu; return; }
    c->lo = a / b;
    c->hi = a % b;
}

/* --- COP0 mínimo: SR (12), CAUSE (13), EPC (14). La entrega de interrupciones es la excepción HLE (irq.c):
 * se toma si SR.IEc (bit 0) y SR.IM2 (bit 10) y hay IRQ pendiente. Esos dos bits son propios de cada hilo
 * (el kernel restaura SR desde el TCB al cambiar de contexto; fiber.c los guarda y repone). El juego los
 * manipula con mtc0 (SwEnter/ExitCriticalSection: &~0x401 / |0x401) y con syscall (IEc solamente). --- */
static uint32_t cop0[32] = {[12] = 0x401};
uint32_t rt_irq_enabled = 1;

static void sr_changed(void) { rt_irq_enabled = (cop0[12] & 1u) && (cop0[12] & 0x400u); }
uint32_t rt_sr_get(void) { return cop0[12] & 0x401u; }
void rt_sr_set(uint32_t v) { cop0[12] = (cop0[12] & ~0x401u) | (v & 0x401u); sr_changed(); }

uint32_t cop0_read(Cpu *c, int r) { (void)c; return cop0[r & 31]; }
void cop0_write(Cpu *c, int r, uint32_t v) {
    cop0[r & 31] = v;
    if (r == 12) {
        sr_changed();
        if (rt_irq_enabled) rt_irq_check(c);   /* habilitar con IRQ pendiente: se entrega ya */
    }
}
void cop0_rfe(Cpu *c) {
    (void)c;
    uint32_t sr = cop0[12];
    cop0[12] = (sr & ~0xFu) | ((sr >> 2) & 0xFu);
    sr_changed();
}

/* syscall: a0 = 1 EnterCriticalSection, 2 ExitCriticalSection. */
void rt_syscall(Cpu *c, uint32_t pc) {
    switch (c->r[4]) {
    case 1: c->r[2] = cop0[12] & 1u; cop0[12] &= ~1u; sr_changed(); break;
    case 2: cop0[12] |= 1u; sr_changed(); rt_irq_check(c); break;
    case 0: break;
    default: rt_fatal(c, "syscall a0=%u en 0x%08x", c->r[4], pc);
    }
}
