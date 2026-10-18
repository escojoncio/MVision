/* HLE de funciones de librería del SDK (sustituyen al código recompilado vía --hle).
 * Nombres = símbolos del mapa de firmas. Las de GPU son stubs hasta el rasterizador Metal. */
#include <setjmp.h>
#include <stdio.h>
#include "cpu.h"
#include "hle.h"

#define V0 c->r[2]
#define A0 c->r[4]
#define A1 c->r[5]

static uint32_t vsync_cb;
static uint32_t frame;
HostFrameHook rt_host_frame;

/* Ejecuta una función MIPS como si fuera una interrupción: preserva todo el estado. */
void hle_invoke(Cpu *c, uint32_t fn, uint32_t arg) {
    if (!fn) return;
    Cpu saved = *c;
    c->r[4] = arg;
    c->r[29] = (c->r[29] - 0x400) & ~7u; /* margen bajo la pila actual */
    rt_call(c, fn);
    *c = saved;
}

uint32_t hle_frame_count(void) { return frame; }

/* VSync(mode): 0 = esperar un frame; <0 = contador de frames; 1 = contador horizontal; n>1 = esperar n. */
void hle_VSync(Cpu *c) {
    int32_t mode = (int32_t)A0;
    if (mode < 0) { V0 = frame; return; }
    if (mode == 1) { V0 = 0; return; }
    int n = mode == 0 ? 1 : mode;
    for (int i = 0; i < n; i++) {
        frame++;
        hle_invoke(c, vsync_cb, 0);
        if (rt_host_frame) rt_host_frame(c, frame);
    }
    V0 = 0;
}

void hle_VSyncCallback(Cpu *c) { uint32_t old = vsync_cb; vsync_cb = A0; V0 = old; }
void hle_ResetCallback(Cpu *c) { vsync_cb = 0; V0 = 0; }
void hle_InterruptCallback(Cpu *c) { V0 = 0; }
void hle_DMACallback(Cpu *c) { V0 = 0; }

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
