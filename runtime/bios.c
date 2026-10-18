/* HLE de la BIOS PS1 (vectores A0/B0/C0, número de función en t1).
 * Solo lo que usan las librerías del juego; lo no implementado se registra una vez y devuelve 0. */
#include <stdio.h>
#include <string.h>
#include "cpu.h"
#include "fiber.h"
#include "hle.h"
#include "sys.h"

#define V0 c->r[2]
#define A0 c->r[4]
#define A1 c->r[5]
#define A2 c->r[6]
#define A3 c->r[7]

/* Tablas de kernel ficticias en RAM baja (zona libre: el juego empieza en 0x80010000). */
#define K_B0_TABLE 0x00000874u
#define K_C0_TABLE 0x00000674u

#define MAX_EVENTS 32
/* Estado de evento como en el kernel: 0x1000 desactivado, 0x2000 activo, 0x4000 entregado. */
#define EV_DISABLED 0x1000u
#define EV_ENABLED 0x2000u
#define EV_READY 0x4000u
#define EV_MODE_CALL 0x1000u
#define EV_MODE_FLAG 0x2000u
static struct { uint32_t cls, spec, mode, func, status; } events[MAX_EVENTS];

/* Memoria de kernel (alloc_kernel_memory): zona baja libre entre tablas ficticias y 0x10000. */
#define K_HEAP_START 0x00001000u
#define K_HEAP_END 0x0000F000u
static uint32_t k_heap = K_HEAP_START;

static int ev_index(uint32_t h) {
    uint32_t i = h & 0xFFFFu;
    return (h & 0xFF000000u) == 0xF1000000u && i < MAX_EVENTS && events[i].cls ? (int)i : -1;
}

void bios_deliver_event(Cpu *c, uint32_t cls, uint32_t spec) {
    for (int i = 0; i < MAX_EVENTS; i++) {
        if (events[i].cls != cls || events[i].spec != spec || events[i].status != EV_ENABLED) continue;
        if (events[i].mode == EV_MODE_CALL) hle_invoke(c, events[i].func, 0);
        else events[i].status = EV_READY;
    }
}

static uint32_t strlen_psx(Cpu *c, uint32_t a) {
    uint32_t n = 0;
    while (mem_r8(c, a + n)) n++;
    return n;
}

static void psx_printf(Cpu *c) {
    /* printf mínimo: copia el formato sin expandir (solo diagnóstico). */
    uint32_t a = A0;
    char buf[256]; unsigned i = 0;
    while (i < sizeof buf - 1) { char ch = (char)mem_r8(c, a + i); if (!ch) break; buf[i++] = ch; }
    buf[i] = 0;
    fprintf(stderr, "[psx] %s", buf);
}

static void unknown(Cpu *c, uint32_t vec, uint32_t fn) {
    static uint8_t seen[3][256];
    int k = vec == 0xA0 ? 0 : vec == 0xB0 ? 1 : 2;
    if (!seen[k][fn & 0xFF]++)
        fprintf(stderr, "[bios] %02X:%02X sin HLE (ra=0x%08x %s)\n", vec, fn, c->r[31], rt_name(c->r[31]));
    V0 = 0;
}

static void bios_a0(Cpu *c, uint32_t fn) {
    switch (fn) {
    case 0x13: V0 = 0; return;                     /* setjmp: lo cubre el HLE de la librería */
    case 0x17: {                                   /* strcmp */
        for (uint32_t i = 0;; i++) {
            uint8_t x = mem_r8(c, A0 + i), y = mem_r8(c, A1 + i);
            if (x != y || !x) { V0 = (uint32_t)(int32_t)(x - y); return; }
        }
    }
    case 0x19: {                                   /* strcpy */
        uint32_t i = 0; uint8_t ch;
        do { ch = mem_r8(c, A1 + i); mem_w8(c, A0 + i, ch); i++; } while (ch);
        V0 = A0; return;
    }
    case 0x1B: V0 = strlen_psx(c, A0); return;     /* strlen */
    case 0x25: V0 = (A0 >= 'a' && A0 <= 'z') ? A0 - 32 : A0; return; /* toupper */
    case 0x2A: {                                   /* memcpy */
        void *d = mem_ptr(A0), *s = mem_ptr(A1);
        if (d && s && A2) memmove(d, s, A2);
        else for (uint32_t i = 0; i < A2; i++) mem_w8(c, A0 + i, mem_r8(c, A1 + i));
        V0 = A0; return;
    }
    case 0x2B: {                                   /* memset */
        void *d = mem_ptr(A0);
        if (d && A2) memset(d, (int)(A1 & 0xFF), A2);
        else for (uint32_t i = 0; i < A2; i++) mem_w8(c, A0 + i, (uint8_t)A1);
        V0 = A0; return;
    }
    case 0x3F: psx_printf(c); V0 = 0; return;      /* printf */
    case 0x44: V0 = 0; return;                     /* FlushCache */
    case 0x49: V0 = 0; return;                     /* GPU_cw */
    case 0x72: V0 = 0; return;                     /* CdRemove */
    case 0x9F: V0 = 0; return;                     /* SetMem */
    case 0x39: V0 = 0; return;                     /* InitHeap (el juego usa su propio gestor) */
    default:
        if (card_bios(c, 0xA0, fn)) return;
        unknown(c, 0xA0, fn);
    }
}

static void bios_b0(Cpu *c, uint32_t fn) {
    switch (fn) {
    case 0x00:                                     /* alloc_kernel_memory */
        A0 = (A0 + 3) & ~3u;
        if (k_heap + A0 > K_HEAP_END) { V0 = 0; return; }
        V0 = k_heap; memset(&psx_ram[k_heap], 0, A0); k_heap += A0; return;
    case 0x01: V0 = 0; return;                     /* free_kernel_memory */
    case 0x07: bios_deliver_event(c, A0, A1); V0 = 1; return;          /* DeliverEvent */
    case 0x08: {                                   /* OpenEvent(class, spec, mode, func) */
        for (unsigned i = 0; i < MAX_EVENTS; i++) if (!events[i].cls) {
            events[i].cls = A0; events[i].spec = A1; events[i].mode = A2; events[i].func = A3;
            events[i].status = EV_DISABLED;
            V0 = 0xF1000000u | i; return;
        }
        V0 = 0xFFFFFFFFu; return;
    }
    case 0x09: { int i = ev_index(A0); if (i >= 0) events[i].cls = 0; V0 = 1; return; }  /* CloseEvent */
    case 0x0A: {                                   /* WaitEvent: aquí no se puede bloquear */
        int i = ev_index(A0);
        if (i < 0 || events[i].status == EV_DISABLED) { V0 = 0; return; }
        if (events[i].status != EV_READY) {
            static unsigned warn;
            if (warn++ < 4) fprintf(stderr, "[bios] WaitEvent(0x%08x) sin evento pendiente: se continúa\n", A0);
        }
        events[i].status = EV_ENABLED; V0 = 1; return;
    }
    case 0x0B: {                                   /* TestEvent */
        int i = ev_index(A0);
        if (i >= 0 && events[i].status == EV_READY) { events[i].status = EV_ENABLED; V0 = 1; }
        else V0 = 0;
        return;
    }
    case 0x0C: { int i = ev_index(A0); if (i >= 0) events[i].status = EV_ENABLED; V0 = 1; return; }  /* EnableEvent */
    case 0x0D: { int i = ev_index(A0); if (i >= 0) events[i].status = EV_DISABLED; V0 = 1; return; } /* DisableEvent */
    case 0x0E: V0 = fiber_open(c, A0, A1, A2); return;   /* OpenTh(pc, sp, gp) */
    case 0x0F: V0 = fiber_close(c, A0); return;          /* CloseTh */
    case 0x10: fiber_change(c, A0); V0 = 1; return;      /* ChangeTh */
    case 0x12: pad_bios_init(c, A0, A1, A2, A3); V0 = 1; return;   /* InitPAD */
    case 0x13: pad_bios_start(c); V0 = 1; return;                   /* StartPAD */
    case 0x14: pad_bios_stop(); V0 = 1; return;                     /* StopPAD */
    case 0x15: V0 = 1; return;                                      /* OutdatedPadInitAndStart */
    case 0x17: rt_fatal(c, "ReturnFromException fuera de IRQ");
    case 0x18: case 0x19: V0 = 0; return;         /* Set*ExitFromException */
    case 0x20: {                                   /* UnDeliverEvent */
        for (int i = 0; i < MAX_EVENTS; i++)
            if (events[i].cls == A0 && events[i].spec == A1 && events[i].status == EV_READY) events[i].status = EV_ENABLED;
        V0 = 1; return;
    }
    case 0x3D: fputc((int)(A0 & 0xFF), stderr); V0 = A0; return; /* putchar */
    case 0x3F: {                                   /* puts */
        uint32_t n = strlen_psx(c, A0);
        for (uint32_t i = 0; i < n; i++) fputc(mem_r8(c, A0 + i), stderr);
        fputc('\n', stderr); V0 = 0; return;
    }
    case 0x56: V0 = K_C0_TABLE; return;            /* GetC0Table */
    case 0x57: V0 = K_B0_TABLE; return;            /* GetB0Table */
    case 0x5B: V0 = 0; return;                     /* ChangeClearPad */
    default:
        if (card_bios(c, 0xB0, fn)) return;
        unknown(c, 0xB0, fn);
    }
}

static void bios_c0(Cpu *c, uint32_t fn) {
    switch (fn) {
    case 0x00: case 0x01: case 0x02: case 0x03: V0 = 0; return; /* colas de IRQ: HLE propio */
    case 0x08: V0 = 0; return;                                  /* SysInitMemory */
    case 0x0A: V0 = 0; return;                                  /* ChangeClearRCnt */
    default: unknown(c, 0xC0, fn);
    }
}

void bios_hle(Cpu *c, uint32_t vec) {
    uint32_t fn = c->r[9] & 0xFF;
    if (vec == 0xA0) bios_a0(c, fn);
    else if (vec == 0xB0) bios_b0(c, fn);
    else bios_c0(c, fn);
}

/* SetConf(eventos, tcbs, pila): reconfigura tablas del kernel parcheando su código; con el kernel
 * en HLE (eventos/hilos con tablas propias) no hay nada que hacer. */
void hle_SetConf(Cpu *c) { V0 = 1; }
