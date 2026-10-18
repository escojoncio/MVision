/* Hilos del kernel PS1 (OpenTh/ChangeTh/CloseTh) sobre fibras del host.
 * Cada hilo MIPS tiene su banco de registros y su propia pila C, porque el código recompilado
 * anida llamadas C: cambiar de hilo = guardar registros MIPS + cambiar de pila host. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "fiber.h"

#if defined(__APPLE__)
#define SYM(x) "_" #x
#else
#define SYM(x) #x
#endif

/* void fiber_swap(void **save_sp, void *new_sp) */
void fiber_swap(void **save_sp, void *new_sp);

#if defined(__x86_64__)
__asm__(".text\n.globl " SYM(fiber_swap) "\n" SYM(fiber_swap) ":\n"
        "  pushq %rbp\n  pushq %rbx\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
        "  movq %rsp, (%rdi)\n  movq %rsi, %rsp\n"
        "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rbx\n  popq %rbp\n  ret\n");
#define FRAME_WORDS 6
#elif defined(__aarch64__)
__asm__(".text\n.globl " SYM(fiber_swap) "\n.p2align 2\n" SYM(fiber_swap) ":\n"
        "  sub sp, sp, #176\n"
        "  stp x19, x20, [sp, #0]\n  stp x21, x22, [sp, #16]\n  stp x23, x24, [sp, #32]\n"
        "  stp x25, x26, [sp, #48]\n  stp x27, x28, [sp, #64]\n  stp x29, x30, [sp, #80]\n"
        "  stp d8, d9, [sp, #96]\n  stp d10, d11, [sp, #112]\n  stp d12, d13, [sp, #128]\n  stp d14, d15, [sp, #144]\n"
        "  mov x2, sp\n  str x2, [x0]\n  mov sp, x1\n"
        "  ldp x19, x20, [sp, #0]\n  ldp x21, x22, [sp, #16]\n  ldp x23, x24, [sp, #32]\n"
        "  ldp x25, x26, [sp, #48]\n  ldp x27, x28, [sp, #64]\n  ldp x29, x30, [sp, #80]\n"
        "  ldp d8, d9, [sp, #96]\n  ldp d10, d11, [sp, #112]\n  ldp d12, d13, [sp, #128]\n  ldp d14, d15, [sp, #144]\n"
        "  add sp, sp, #176\n  ret\n");
#else
#error "arquitectura sin fiber_swap"
#endif

#define MAX_TH 8
#define HOST_STACK (1u << 20)

typedef struct {
    int used, started;
    Cpu regs;          /* banco MIPS guardado mientras no corre */
    uint32_t pc;
    void *host_sp;     /* pila host guardada */
    void *stack;       /* memoria de pila host (NULL para el hilo 0) */
} Thread;

static Thread th[MAX_TH] = {{.used = 1, .started = 1}};
static int cur;
static Cpu *g_cpu;

static void fiber_entry(void) {
    Thread *t = &th[cur];
    rt_call(g_cpu, t->pc);
    rt_fatal(g_cpu, "el hilo 0x%08x retornó", 0xFF000000u | (unsigned)cur);
}

static void *initial_sp(void *stack) {
    uintptr_t top = ((uintptr_t)stack + HOST_STACK) & ~(uintptr_t)15;
#if defined(__x86_64__)
    uintptr_t *p = (uintptr_t *)(top - 16);
    p[0] = (uintptr_t)fiber_entry;          /* dirección de retorno del ret */
    p[1] = 0;
    p -= FRAME_WORDS;
    memset(p, 0, FRAME_WORDS * sizeof *p);
    return p;
#else
    uint64_t *p = (uint64_t *)(top - 176);
    memset(p, 0, 176);
    p[11] = (uint64_t)(uintptr_t)fiber_entry; /* x30 */
    return p;
#endif
}

uint32_t fiber_open(Cpu *c, uint32_t pc, uint32_t sp, uint32_t gp) {
    (void)c;
    for (int i = 1; i < MAX_TH; i++) if (!th[i].used) {
        Thread *t = &th[i];
        memset(t, 0, sizeof *t);
        t->used = 1;
        t->pc = pc;
        t->regs.r[29] = t->regs.r[30] = sp;
        t->regs.r[28] = gp;
        if (!t->stack) t->stack = malloc(HOST_STACK);
        if (!t->stack) rt_fatal(c, "sin memoria para pila de hilo");
        t->host_sp = initial_sp(t->stack);
        return 0xFF000000u | (uint32_t)i;
    }
    return 0xFFFFFFFFu;
}

uint32_t fiber_close(Cpu *c, uint32_t h) {
    unsigned i = h & 0xFF;
    if (i == 0 || i >= MAX_TH || !th[i].used) return 0;
    if ((int)i == cur) rt_fatal(c, "CloseTh del hilo en ejecución");
    free(th[i].stack);
    memset(&th[i], 0, sizeof th[i]);
    return 1;
}

uint32_t fiber_change(Cpu *c, uint32_t h) {
    unsigned i = h & 0xFF;
    if (i >= MAX_TH || !th[i].used) rt_fatal(c, "ChangeTh a hilo inválido 0x%08x", h);
    if ((int)i == cur) return 1;
    int from = cur;
    c->r[2] = 1;                 /* valor de retorno de ChangeTh al reanudar este hilo */
    th[from].regs = *c;
    *c = th[i].regs;
    g_cpu = c;
    cur = (int)i;
    th[i].started = 1;
    fiber_swap(&th[from].host_sp, th[i].host_sp);
    /* reanudado: otro hilo ya restauró nuestros registros en *c */
    return 1;
}

uint32_t fiber_current(void) { return 0xFF000000u | (uint32_t)cur; }
