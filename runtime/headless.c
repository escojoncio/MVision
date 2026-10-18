/* Arranque headless: carga un PS-X EXE en RAM y ejecuta su punto de entrada recompilado.
 *   headless <exe> [frames]   (por defecto 300 frames) */
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cpu.h"
#include "hle.h"

extern jmp_buf rt_fatal_jmp;
extern int rt_fatal_armed;
static uint32_t max_frames = 300;
static jmp_buf done_jmp;

static void on_frame(Cpu *c, uint32_t f) {
    (void)c;
    if (f % 60 == 0) fprintf(stderr, "[headless] frame %u\n", f);
    if (f >= max_frames) longjmp(done_jmp, 1);
}

static void on_alarm(int s) {
    (void)s;
    rt_fatal(NULL, "sin VSync en 10 s (bucle de espera de hardware sin HLE)");
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: %s <exe> [frames]\n", argv[0]); return 1; }
    if (argc > 2) max_frames = (uint32_t)strtoul(argv[2], 0, 0);
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    static uint8_t buf[PSX_RAM_SIZE + 0x800];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n < 0x800 || memcmp(buf, "PS-X EXE", 8)) { fprintf(stderr, "no es PS-X EXE\n"); return 1; }
    uint32_t pc = rd32(buf + 0x10), gp = rd32(buf + 0x14), t = rd32(buf + 0x18), sz = rd32(buf + 0x1C);
    if (sz > n - 0x800 || (t & 0x1FFFFF) + sz > PSX_RAM_SIZE) { fprintf(stderr, "cabecera inválida\n"); return 1; }
    memcpy(&psx_ram[t & 0x1FFFFF], buf + 0x800, sz);
    if (pc != rt_entry) fprintf(stderr, "[headless] aviso: pc 0x%08x != entrada recompilada 0x%08x\n", pc, rt_entry);

    Cpu c = {0};
    c.r[28] = gp;
    c.r[29] = c.r[30] = 0x801FFFF0u; /* STACK de SYSTEM.CNF */
    c.r[31] = 0;
    gte_reset();
    rt_host_frame = on_frame;
    signal(SIGALRM, on_alarm);
    if (setjmp(done_jmp)) { fprintf(stderr, "[headless] OK: %u frames\n", hle_frame_count()); return 0; }
    rt_fatal_armed = 1;
    if (setjmp(rt_fatal_jmp)) return 2;
    uint32_t last = 0;
    (void)last;
    alarm(10);
    rt_call(&c, pc);
    fprintf(stderr, "[headless] la entrada retornó (v0=0x%08x) tras %u frames\n", c.r[2], hle_frame_count());
    return 0;
}
