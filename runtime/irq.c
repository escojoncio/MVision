/* Excepción HLE: entrega de interrupciones asíncronas en puntos seguros.
 *
 * Modelo (equivale al ExceptionHandler del kernel de BIOS):
 *  1. El código recompilado/interpretado llama a RT_POLL en sus aristas hacia atrás (cpu.h). Cuando el
 *     presupuesto se agota, rt_poll avanza el reloj virtual (io.c) y, si SR.IEc && SR.IM2 y
 *     (I_STAT & I_MASK) != 0, llama a exception().
 *  2. exception() guarda el banco de registros, cambia a la pila de excepción del kernel, recorre las
 *     colas encolar manejador de IRQ (prioridades 0..3) y después atiende cada fuente pendiente en orden de bit:
 *     reconoce el bit de I_STAT, entrega los eventos de kernel (contadores de raíz 0-3) y llama a los
 *     manejadores que registró la librería (cb_irq / cb_dma / frame_cb).
 *  3. Retorno (retorno de excepción): restaura los registros y consulta el PCB del kernel (*0x108).
 *     Si algún manejador cambió el TCB actual (como hace el planificador del juego, que escribe el TCB
 *     de la tarea elegida en el PCB), se conmuta de hilo: el hilo interrumpido queda suspendido dentro
 *     de este mismo marco C y reanudará en el punto exacto del sondeo cuando alguien vuelva a él
 *     (cambiar de hilo u otra excepción).
 *  Sin anidamiento: dentro de la excepción IEc está a 0. cambiar de hilo dentro de la excepción es diferido
 *  (fiber.c solo escribe el PCB), de modo que toda excepción se ejecuta y termina sobre una única pila
 *  de fibra y el estado de este módulo (in_exc, exc_stack) y el de cdrom.c no necesita ser por hilo. */
#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"
#include "fiber.h"
#include "hle.h"
#include "sys.h"
#include "cdrom.h"

static uint32_t intr_cb[11];     /* cb_irq(irq) */
static uint32_t dma_cb[7];       /* cb_dma(canal) */
static uint32_t vsync_cb;        /* frame_cb */
static uint32_t exit_hook;       /* B0:19 gancho de salida de excepción */
static uint32_t qhead[4];        /* colas encolar manejador de IRQ por prioridad (nodos en RAM del juego) */
static int in_exc;
static uint32_t exc_stack;
unsigned long irq_count[11], irq_exceptions;   /* estadísticas (headless) */

int irq_in_exception(void) { return in_exc; }

/* Aviso único por condición de truncamiento (evita inundar el log). */
static void warn_once(int *flag, const char *msg) {
    if (*flag) return;
    *flag = 1;
    fprintf(stderr, "[irq] %s\n", msg);
}

/* ---- registro de manejadores (la librería base) ---- */
uint32_t irq_set_callback(int irq, uint32_t fn) {
    if (irq < 0 || irq > 10) return 0;
    uint32_t old = intr_cb[irq];
    intr_cb[irq] = fn;
    sys_irq_enable(irq, fn != 0);
    return old;
}

uint32_t irq_set_dma_callback(int ch, uint32_t fn) {
    if (ch < 0 || ch > 6) return 0;
    uint32_t old = dma_cb[ch];
    dma_cb[ch] = fn;
    sys_dma_irq_enable(ch, fn != 0);
    if (fn) sys_irq_enable(IRQ_DMA, 1);
    return old;
}

uint32_t irq_set_vsync_callback(uint32_t fn) {
    uint32_t old = vsync_cb;
    vsync_cb = fn;
    if (fn) sys_irq_enable(IRQ_VBLANK, 1);
    return old;
}

void irq_reset_callbacks(void) {
    for (int i = 0; i < 11; i++) intr_cb[i] = 0;
    for (int i = 0; i < 7; i++) dma_cb[i] = 0;
    vsync_cb = 0;
    sys_irq_enable(IRQ_VBLANK, 1);
    sys_irq_enable(IRQ_DMA, 1);
}

void irq_set_exit_hook(uint32_t fn) {
    if (fn && fn != exit_hook) fprintf(stderr, "[irq] gancho de salida de excepción 0x%08x (no se invoca: retorno HLE)\n", fn);
    exit_hook = fn;
}

/* ---- colas del kernel: nodo = {next, handler(+4), verificador(+8)} (layout supuesto, ver bitácora) ---- */
uint32_t irq_enq(Cpu *c, uint32_t prio, uint32_t node) {
    if (prio > 3 || !node) return 0;
    mem_w32(c, node, 0);
    if (!qhead[prio]) { qhead[prio] = node; return 0; }
    uint32_t n = qhead[prio];
    int g = 0;
    for (; g < 256 && mem_r32(c, n); g++) n = mem_r32(c, n);
    static int warned;
    if (g == 256) warn_once(&warned, "cola de interrupciones demasiado larga: recorrido de enqueue truncado");
    mem_w32(c, n, node);
    return 0;
}

uint32_t irq_deq(Cpu *c, uint32_t prio, uint32_t node) {
    if (prio > 3) return 0;
    if (qhead[prio] == node) { qhead[prio] = mem_r32(c, node); return 0; }
    for (uint32_t n = qhead[prio]; n; n = mem_r32(c, n))
        if (mem_r32(c, n) == node) { mem_w32(c, n, mem_r32(c, node)); break; }
    return 0;
}

/* ---- entrega ---- */
static void call(Cpu *c, uint32_t fn, uint32_t a0) {
    if (!fn) return;
    c->r[4] = a0;
    c->r[5] = 0;
    c->r[29] = exc_stack;
    rt_call(c, fn);
}

static void run_queues(Cpu *c) {
    static int warned;
    for (int p = 0; p < 4; p++)
        for (uint32_t n = qhead[p], g = 0; n; n = mem_r32(c, n), g++) {
            if (g >= 64) { warn_once(&warned, "cola de interrupciones truncada a 64 nodos"); break; }
            uint32_t handler = mem_r32(c, n + 4), verify = mem_r32(c, n + 8);
            if (verify) {
                c->r[29] = exc_stack; c->r[4] = n;
                rt_call(c, verify);
                if (!c->r[2]) continue;
            }
            call(c, handler, n);
        }
}

static void service(Cpu *c, int irq) {
    switch (irq) {
    case IRQ_VBLANK:
        bios_deliver_event(c, EV_RCNT(3), EVS_INTR);
        call(c, intr_cb[0], 0);
        call(c, vsync_cb, 0);
        break;
    case IRQ_CD:
        cd_pump(c);
        call(c, intr_cb[2], 0);
        break;
    case IRQ_DMA: {
        uint32_t f = sys_dma_flags_take();
        for (int ch = 0; ch < 7; ch++) if (f & (1u << ch)) call(c, dma_cb[ch], 0);
        call(c, intr_cb[3], 0);
        break;
    }
    case IRQ_TMR0: case IRQ_TMR1: case IRQ_TMR2:
        bios_deliver_event(c, EV_RCNT(irq - IRQ_TMR0), EVS_INTR);
        call(c, intr_cb[irq], 0);
        break;
    default:
        call(c, intr_cb[irq], 0);
    }
}

static void exception(Cpu *c) {
    if (!exc_stack) {
        uint32_t b = bios_kalloc(0x1000);
        if (!b) rt_fatal(c, "sin memoria de kernel para la pila de excepción");
        exc_stack = b + 0x1000 - 16;
    }
    irq_exceptions++;
    fiber_exception_enter(c);          /* estado del hilo interrumpido -> su TCB */
    in_exc = 1;
    uint32_t saved_sr = rt_sr_get();
    rt_sr_set(saved_sr & ~1u);
    Cpu saved = *c;
    run_queues(c);
    static int warned;
    for (int guard = 0;; guard++) {
        uint32_t pend = sys_irq_stat() & sys_irq_mask();
        if (!pend) break;
        if (guard >= 64) { warn_once(&warned, "demasiadas IRQ encadenadas en una excepción: se difiere el resto"); break; }
        int irq = __builtin_ctz(pend);
        sys_irq_ack(irq);
        irq_count[irq]++;
        service(c, irq);
    }
    *c = saved;
    in_exc = 0;
    /* retorno de excepción: el PCB decide qué hilo continúa. */
    fiber_return_from_exception(c, saved_sr);
}

/* ¿Alguien puede reaccionar a esta fuente? (cola del kernel, manejador de librería o evento de BIOS) */
static int has_handler(int irq) {
    for (int p = 0; p < 4; p++) if (qhead[p]) return 1;
    switch (irq) {
    case IRQ_VBLANK: return intr_cb[0] || vsync_cb || bios_event_listening(EV_RCNT(3), EVS_INTR);
    case IRQ_CD: return 1;     /* cd_pump entrega comandos y sectores pendientes */
    case IRQ_DMA:
        for (int ch = 0; ch < 7; ch++) if (dma_cb[ch]) return 1;
        return intr_cb[3] != 0;
    case IRQ_TMR0: case IRQ_TMR1: case IRQ_TMR2:
        return intr_cb[irq] || bios_event_listening(EV_RCNT(irq - IRQ_TMR0), EVS_INTR);
    default: return intr_cb[irq] != 0;
    }
}

void rt_irq_check(Cpu *c) {
    if (in_exc || !rt_irq_enabled) return;
    if (!(cop0_read(c, 12) & 0x400)) return;
    uint32_t pend = sys_irq_stat() & sys_irq_mask();
    if (!pend) return;
    /* Fuentes sin manejador ni cola: se reconocen sin montar la excepción completa. */
    for (uint32_t m = pend; m; m &= m - 1) {
        int irq = __builtin_ctz(m);
        if (has_handler(irq)) continue;
        sys_irq_ack(irq);
        irq_count[irq]++;
        if (irq == IRQ_DMA) sys_dma_flags_take();
        pend &= ~(1u << irq);
    }
    if (pend) exception(c);
}

/* Llamado cuando se agota rt_budget. */
void rt_poll(Cpu *c) {
    sys_clock_tick(c);
    rt_irq_check(c);
}
