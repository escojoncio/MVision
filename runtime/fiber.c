/* Hilos del kernel original (abrir hilo/cambiar de hilo/cerrar hilo) sobre fibras del host.
 * Cada hilo MIPS tiene su banco de registros y su propia pila C, porque el código recompilado
 * anida llamadas C: cambiar de hilo = guardar registros MIPS + cambiar de pila host. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "fiber.h"
#include "sys.h"

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

#define MAX_TH 24
#define HOST_STACK (1u << 20)
#define TCB_SIZE 0xC0u           /* tamaño de TCB del kernel: status, 32 registros, EPC, HI, LO, SR, CAUSE */
#define TCB_REGS 8u
#define TCB_EPC 0x88u
#define TCB_HI 0x8Cu
#define TCB_LO 0x90u
#define TCB_SR 0x94u
#define TCB_CAUSE 0x98u
#define TCB_FREE 0x1000u
#define TCB_USED 0x4000u

typedef struct {
    int used, started;
    Cpu regs;          /* banco MIPS guardado mientras no corre */
    uint32_t pc;
    uint32_t sr;       /* bits de interrupción de SR (0x401) que tendrá al reanudarse */
    void *host_sp;     /* pila host guardada */
    void *stack;       /* memoria de pila host (NULL para el hilo 0) */
} Thread;

static Thread th[MAX_TH] = {{.used = 1, .started = 1, .sr = 0x401}};
static int cur;
static Cpu *g_cpu;
static uint32_t pcb, tcb_base, tcb_n;

static uint32_t tcb_addr(int i) { return tcb_base + (uint32_t)i * TCB_SIZE; }

/* Refleja un banco de registros en el TCB de RAM (los juegos leen sp y compañía de ahí). */
static void tcb_store(Cpu *c, int i, const Cpu *r) {
    uint32_t a = tcb_addr(i);
    for (int k = 0; k < 32; k++) mem_w32(c, a + TCB_REGS + 4u * (uint32_t)k, r->r[k]);
    mem_w32(c, a + TCB_HI, r->hi);
    mem_w32(c, a + TCB_LO, r->lo);
}

/* Reserva PCB y tabla de TCB en memoria de kernel y publica los punteros en 0x108/0x110. La tabla se
 * reserva entera (MAX_TH) la primera vez: sys_conf solo cambia cuántos TCB se anuncian, sin mover ni
 * perder los hilos ya abiertos. */
void fiber_kinit(Cpu *c, uint32_t n) {
    if (n < 4) n = 4;
    if (n > MAX_TH) n = MAX_TH;
    if (!tcb_base) {
        tcb_base = bios_kalloc(MAX_TH * TCB_SIZE);
        pcb = bios_kalloc(8);
        if (!tcb_base || !pcb) rt_fatal(c, "sin memoria de kernel para la tabla de TCB");
        for (uint32_t i = 0; i < MAX_TH; i++) mem_w32(c, tcb_addr((int)i), i == 0 ? TCB_USED : TCB_FREE);
        mem_w32(c, pcb, tcb_addr(cur));
    }
    if (n < tcb_n) n = tcb_n;          /* la tabla no se encoge */
    tcb_n = n;
    mem_w32(c, 0x80000108u, pcb);
    mem_w32(c, 0x8000010Cu, 8);
    mem_w32(c, 0x80000110u, tcb_base);
    mem_w32(c, 0x80000114u, n * TCB_SIZE);
}

static void ensure_init(Cpu *c) { if (!tcb_base) fiber_kinit(c, 4); }

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
    ensure_init(c);
    for (uint32_t i = 1; i < tcb_n; i++) if (!th[i].used) {
        Thread *t = &th[i];
        void *keep = t->stack;
        memset(t, 0, sizeof *t);
        t->stack = keep;
        t->used = 1;
        t->pc = pc;
        t->sr = 0x401;   /* SR inicial: IEc e IM2 activos (el 0x400 que escribe el juego en el TCB no se usa, ver bitácora) */
        t->regs.r[29] = t->regs.r[30] = sp;
        t->regs.r[28] = gp;
        if (!t->stack) t->stack = malloc(HOST_STACK);
        if (!t->stack) rt_fatal(c, "sin memoria para pila de hilo");
        t->host_sp = initial_sp(t->stack);
        uint32_t a = tcb_addr((int)i);
        mem_w32(c, a, TCB_USED);
        tcb_store(c, (int)i, &t->regs);
        mem_w32(c, a + TCB_EPC, pc);
        mem_w32(c, a + TCB_SR, 0x404);
        return 0xFF000000u | i;
    }
    return 0xFFFFFFFFu;
}

uint32_t fiber_close(Cpu *c, uint32_t h) {
    unsigned i = h & 0xFF;
    ensure_init(c);
    if (i == 0 || i >= MAX_TH || !th[i].used) return 0;
    if ((int)i == cur) rt_fatal(c, "cerrar hilo del hilo en ejecución");
    th[i].used = 0;
    mem_w32(c, tcb_addr((int)i), TCB_FREE);
    return 1;     /* la pila host se conserva para reutilizarla en el próximo abrir hilo */
}

/* Cambia al hilo i guardando el actual. set_ret: v0 = 1 al reanudar (retorno de cambiar de hilo). */
static void switch_to(Cpu *c, int i, int set_ret) {
    int from = cur;
    if (set_ret) c->r[2] = 1;
    th[from].regs = *c;
    th[from].sr = rt_sr_get();
    tcb_store(c, from, c);
    *c = th[i].regs;
    rt_sr_set(th[i].sr);
    g_cpu = c;
    cur = i;
    th[i].started = 1;
    mem_w32(c, pcb, tcb_addr(i));
    fiber_swap(&th[from].host_sp, th[i].host_sp);
    /* reanudado: quien nos devolvió el control ya restauró nuestros registros en *c y nuestro IEc */
    if (rt_irq_enabled) sys_kick();
}

uint32_t fiber_change(Cpu *c, uint32_t h) {
    unsigned i = h & 0xFF;
    ensure_init(c);
    if (i >= MAX_TH || !th[i].used) rt_fatal(c, "cambiar de hilo a hilo inválido 0x%08x", h);
    if ((int)i == cur) { mem_w32(c, pcb, tcb_addr(cur)); return 1; }
    /* Dentro de la excepción (manejador del planificador) solo se escribe el PCB: la conmutación real se
     * hace al retornar (fiber_return_from_exception), con la pila de excepción y el estado de IRQ
     * intactos para el hilo interrumpido. */
    if (irq_in_exception()) { mem_w32(c, pcb, tcb_addr((int)i)); return 1; }
    switch_to(c, (int)i, 1);
    return 1;
}

static int pcb_index(Cpu *c) {
    uint32_t t = mem_r32(c, pcb);
    if (t < tcb_base || t >= tcb_base + tcb_n * TCB_SIZE || (t - tcb_base) % TCB_SIZE) return -1;
    return (int)((t - tcb_base) / TCB_SIZE);
}

/* Entrada de excepción: el estado del hilo interrumpido se refleja en su TCB (registros, EPC, SR, CAUSE)
 * como hace el kernel, para que el planificador del juego lo vea coherente. En el código recompilado no
 * hay PC en los sondeos: EPC es el último valor conocido (COP0 o, si no hay, el que ya tenía el TCB). */
void fiber_exception_enter(Cpu *c) {
    ensure_init(c);
    tcb_store(c, cur, c);
    uint32_t a = tcb_addr(cur), epc = cop0_read(c, 14);
    if (epc) mem_w32(c, a + TCB_EPC, epc);
    mem_w32(c, a + TCB_SR, cop0_read(c, 12));
    mem_w32(c, a + TCB_CAUSE, (cop0_read(c, 13) & ~0x7Cu) | 0x400u);   /* IP2 pendiente, ExcCode 0 (Int) */
}

void fiber_return_from_exception(Cpu *c, uint32_t saved_sr) {
    ensure_init(c);
    int i = pcb_index(c);
    if (i < 0) {
        fprintf(stderr, "[fiber] PCB con TCB actual inválido 0x%08x: se restaura el hilo %d\n", mem_r32(c, pcb), cur);
        i = cur;
    }
    if (i == cur) { mem_w32(c, pcb, tcb_addr(cur)); rt_sr_set(saved_sr); return; }
    if (i >= MAX_TH || !th[i].used) rt_fatal(c, "la excepción retorna a un TCB sin hilo (%d)", i);
    rt_sr_set(saved_sr);
    switch_to(c, i, 0);
}

uint32_t fiber_current(void) { return 0xFF000000u | (uint32_t)cur; }
