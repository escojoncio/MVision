/* HLE de funciones de librería del SDK (sustituyen al código recompilado vía --hle).
 * Nombres = símbolos del mapa de firmas. Las de GPU son stubs hasta el rasterizador Metal. */
#include <stdio.h>
#include "cpu.h"
#include "hle.h"

#define V0 c->r[2]
#define A0 c->r[4]
#define A1 c->r[5]

static uint32_t vsync_cb, drawsync_cb;
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
void hle_ResetCallback(Cpu *c) { vsync_cb = drawsync_cb = 0; V0 = 0; }
void hle_InterruptCallback(Cpu *c) { V0 = 0; }
void hle_DMACallback(Cpu *c) { V0 = 0; }

/* --- GPU (stubs) --- */
void hle_ResetGraph(Cpu *c) { V0 = 0; }
void hle_SetGraphDebug(Cpu *c) { V0 = 0; }
void hle_DrawSync(Cpu *c) { V0 = 0; }
void hle_DrawSyncCallback(Cpu *c) { uint32_t old = drawsync_cb; drawsync_cb = A0; V0 = old; }
void hle_SetDispMask(Cpu *c) { V0 = 0; }
void hle_PutDrawEnv(Cpu *c) { V0 = A0; }
void hle_PutDispEnv(Cpu *c) { V0 = A0; }
void hle_DrawOTag(Cpu *c) { hle_invoke(c, drawsync_cb, 0); V0 = 0; }
void hle_LoadImage(Cpu *c) { V0 = 0; }
void hle_StoreImage(Cpu *c) { V0 = 0; }
void hle_MoveImage(Cpu *c) { V0 = 0; }
void hle_ClearImage(Cpu *c) { V0 = 0; }

/* longjmp necesita soporte del recompilador (pila C); por ahora se detecta. */
void hle_longjmp(Cpu *c) { rt_fatal(c, "longjmp(0x%08x, %d) aún no soportado", A0, A1); }
