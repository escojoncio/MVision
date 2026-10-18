/* HLE de funciones de librería del SDK (sustituyen al código recompilado vía --hle).
 * Nombres = símbolos del mapa de firmas. Las de GPU son stubs hasta el rasterizador Metal. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"
#include "hle.h"
#include "sys.h"

#define V0 c->r[2]
#define A0 c->r[4]
#define A1 c->r[5]

HostFrameHook rt_host_frame;
#include "fiber.h"
/* Contador de VBlank en la última salida de VSync, por hilo: VSync(n) de un hilo no debe leer la base de otro. */
static uint32_t vs_base[32];

static uint32_t cb_lo, cb_hi;
static uint32_t cb_entry[16];     /* sp inicial de cada callback en curso (pila de profundidad) */
static unsigned cb_depth;
uint32_t hle_cb_sp(uint32_t cur_sp) {
    if (!cb_hi) {
        cb_lo = bios_kalloc(0x1000);
        if (cb_lo) cb_hi = cb_lo + 0x1000 - 16;
    }
    uint32_t s = cur_sp & 0x1FFFFFFFu, lo = cb_lo & 0x1FFFFFFFu, hi = cb_hi & 0x1FFFFFFFu, sp;
    if (!cb_hi) sp = (cur_sp - 0x400) & ~7u;                               /* sin memoria de kernel: comportamiento antiguo */
    else if (s > lo && s <= hi) sp = (cur_sp - 0x80) & ~7u;                /* callback dentro de un callback */
    else if (cb_depth) sp = (cb_entry[(cb_depth - 1) & 15] - 0x400) & ~7u;  /* excepción que interrumpe un callback: bajo el suyo */
    else sp = cb_hi;
    if (cb_hi && (sp & 0x1FFFFFFFu) <= lo + 0x100) sp = (cur_sp - 0x400) & ~7u;   /* región agotada: comportamiento antiguo */
    cb_entry[cb_depth++ & 15] = sp;
    return sp;
}
/* Fin del callback abierto con hle_cb_sp (hle_invoke y el CD lo llaman tras rt_call). */
void hle_cb_done(void) { if (cb_depth) cb_depth--; }

/* Ejecuta una función MIPS como si fuera una interrupción: preserva todo el estado. */
void hle_invoke(Cpu *c, uint32_t fn, uint32_t arg) {
    if (!fn) return;
    Cpu saved = *c;
    c->r[4] = arg;
    c->r[29] = hle_cb_sp(c->r[29]);
    rt_call(c, fn);
    hle_cb_done();
    *c = saved;
}

uint32_t hle_frame_count(void) { return sys_vblank_count(); }

/* VSync(mode): <0 = contador de VBlank; 1 = contador horizontal; 0 = esperar al siguiente VBlank;
 * n>1 = esperar hasta n VBlank desde la salida de la llamada anterior. El VBlank lo genera el reloj
 * virtual (io.c); aquí solo se deja avanzar el tiempo y entregar excepciones mientras se espera, de
 * modo que otras tareas pueden ejecutarse (el planificador del juego conmuta desde su VSyncCallback). */
void hle_VSync(Cpu *c) {
    int32_t mode = (int32_t)A0;
    if (mode < 0) { V0 = sys_vblank_count(); return; }
    if (mode == 1) { V0 = sys_hcount(); return; }
    uint32_t target = mode == 0 ? sys_vblank_count() + 1 : vs_base[fiber_current() & 31] + (uint32_t)mode;
    while ((int32_t)(sys_vblank_count() - target) < 0) {
        sys_idle(c);
        rt_irq_check(c);
    }
    vs_base[fiber_current() & 31] = sys_vblank_count();
    V0 = 0;
}

void hle_VSyncCallback(Cpu *c) { V0 = irq_set_vsync_callback(A0); }
/* ResetCallback es idempotente como en libapi: libspu la llama desde SpuInit, que el juego ejecuta
 * después de instalar su VSyncCallback, y no debe borrarlo. Solo la primera llamada reinicia. */
void hle_ResetCallback(Cpu *c) {
    static int done;
    if (!done) { done = 1; irq_reset_callbacks(); }
    V0 = 0;
}
void hle_InterruptCallback(Cpu *c) { V0 = irq_set_callback((int)A0, A1); }
void hle_DMACallback(Cpu *c) { V0 = irq_set_dma_callback((int)A0, A1); }

/* setjmp/longjmp: buffer MIPS = ra, sp, fp, s0-s7, gp (layout del SDK). El punto de reanudación del
 * host lo crea el código generado en la llamada directa a setjmp (rt.c, rt_sj_*). */
jmp_buf *rt_sj_slot(Cpu *c, uint32_t buf);
int rt_sj_take_armed(void);
void rt_sj_forget(uint32_t buf);
_Noreturn void rt_sj_longjmp(Cpu *c, uint32_t buf, uint32_t val);

void hle_setjmp(Cpu *c) {
    static const int regs[] = {31, 28, 29, 30, 16, 17, 18, 19, 20, 21, 22, 23};
    static const int offs[] = {0, 44, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40};
    for (int k = 0; k < 12; k++) mem_w32(c, A0 + offs[k], c->r[regs[k]]);
    if (!rt_sj_take_armed()) rt_sj_forget(A0); /* llamada indirecta: sin marco host al que volver */
    V0 = 0;
}

void hle_longjmp(Cpu *c) { rt_sj_longjmp(c, A0, A1); }
