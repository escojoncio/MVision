/* Arranque headless: carga un PS-X EXE en RAM y ejecuta su punto de entrada recompilado.
 *   headless <exe> [frames] [--dump DIR] [--every N] [--disc RUTAS] [--card FICHERO] [--full]
 *  - frames: VBlanks a ejecutar (por defecto 300).
 *  - --disc "dir1;dir2" (o variable MV_DISC): discos extraídos; --card (o MV_CARD): imagen .mcd.
 *  - --peek DIR,LEN (repetible): vuelca palabras de RAM al terminar (diagnóstico).
 *  - --dump DIR --every N: vuelca a PPM el área de display cada N frames (--full: VRAM completa).
 * Al terminar imprime estadísticas de la GPU y del intérprete. */
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cdrom.h"
#include "cpu.h"
#include "gpu.h"
#include "hle.h"
#include "sys.h"

extern jmp_buf rt_fatal_jmp;
extern int rt_fatal_armed;
extern unsigned long interp_insns;
extern unsigned long irq_count[11], irq_exceptions;
static uint32_t max_frames = 300;
static const char *dump_dir;
static uint32_t dump_every = 50;
static int dump_full;
static GpuStats total;
static uint32_t peek_a[8], peek_n[8];
static int npeek;

static void report(void) {
    if (getenv("MV_RING")) rt_dump_trace();
    for (int i = 0; i < npeek; i++) {
        fprintf(stderr, "[peek] %08x:", peek_a[i]);
        for (uint32_t k = 0; k < peek_n[i]; k += 4) {
            if (k % 32 == 0 && k) fprintf(stderr, "\n[peek] %08x:", peek_a[i] + k);
            fprintf(stderr, " %08x", mem_r32(NULL, peek_a[i] + k));
        }
        fputc('\n', stderr);
    }
    GpuDisplay d;
    gpu_get_display(&d);
    fprintf(stderr, "[headless] GPU: poly=%u rect=%u line=%u fill=%u copy=%u upload=%u env=%u listas=%u | display %dx%d@(%d,%d) %s\n",
            gpu_stats.poly, gpu_stats.rect, gpu_stats.line, gpu_stats.fill, gpu_stats.copy, gpu_stats.upload,
            gpu_stats.env, gpu_stats.lists, d.w, d.h, d.x, d.y, d.enabled ? "activo" : "apagado");
    fprintf(stderr, "[headless] excepciones %lu (VBlank %lu, CD %lu, DMA %lu, TMR %lu/%lu/%lu, SPU %lu)\n", irq_exceptions,
            irq_count[0], irq_count[2], irq_count[3], irq_count[4], irq_count[5], irq_count[6], irq_count[9]);
    fprintf(stderr, "[headless] %u VBlank, %llu ciclos virtuales, %lu instrucciones interpretadas\n",
            sys_vblank_count(), (unsigned long long)sys_cycles(), interp_insns);
}

static void on_frame(Cpu *c, uint32_t f) {
    (void)c;
    alarm(10);
    if (f % 60 == 0) fprintf(stderr, "[headless] frame %u\n", f);
    if (dump_dir && f % dump_every == 0) {
        char p[1024];
        snprintf(p, sizeof p, "%s/frame_%05u.ppm", dump_dir, f);
        gpu_dump_ppm(p, dump_full);
    }
    if (f >= max_frames) {
        fprintf(stderr, "[headless] OK: %u frames\n", f);
        report();
        exit(0);   /* el gancho puede correr dentro de una fibra: no se hace longjmp entre pilas */
    }
}

static void on_alarm(int s) {
    (void)s;
    rt_fatal(NULL, "sin VBlank en 10 s (bucle sin puntos de sondeo)");
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

int main(int argc, char **argv) {
    const char *exe = NULL, *disc = getenv("MV_DISC"), *card = getenv("MV_CARD");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump_dir = argv[++i];
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) dump_every = (uint32_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--disc") && i + 1 < argc) disc = argv[++i];
        else if (!strcmp(argv[i], "--card") && i + 1 < argc) card = argv[++i];
        else if (!strcmp(argv[i], "--full")) dump_full = 1;
        else if (!strcmp(argv[i], "--peek") && i + 1 < argc && npeek < 8) {
            char *s = argv[++i], *e;
            peek_a[npeek] = (uint32_t)strtoul(s, &e, 16);
            peek_n[npeek++] = *e == ',' ? (uint32_t)strtoul(e + 1, 0, 0) : 16;
        }
        else if (!exe) exe = argv[i];
        else max_frames = (uint32_t)strtoul(argv[i], 0, 0);
    }
    if (!exe) { fprintf(stderr, "uso: %s <exe> [frames] [--dump DIR] [--every N] [--disc RUTAS] [--card MCD] [--full]\n", argv[0]); return 1; }
    if (!dump_every) dump_every = 1;
    if (dump_dir) mkdir(dump_dir, 0755);
    FILE *f = fopen(exe, "rb");
    if (!f) { perror(exe); return 1; }
    static uint8_t buf[PSX_RAM_SIZE + 0x800];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n < 0x800 || memcmp(buf, "PS-X EXE", 8)) { fprintf(stderr, "no es PS-X EXE\n"); return 1; }
    uint32_t pc = rd32(buf + 0x10), gp = rd32(buf + 0x14), t = rd32(buf + 0x18), sz = rd32(buf + 0x1C);
    if (sz > n - 0x800 || (t & 0x1FFFFF) + sz > PSX_RAM_SIZE) { fprintf(stderr, "cabecera inválida\n"); return 1; }
    memcpy(&psx_ram[t & 0x1FFFFF], buf + 0x800, sz);
    if (pc != rt_entry) fprintf(stderr, "[headless] aviso: pc 0x%08x != entrada recompilada 0x%08x\n", pc, rt_entry);

    if (disc) {
        int nd = cd_open(disc);
        fprintf(stderr, "[headless] disco(s) montado(s): %d\n", nd);
        if (!nd) return 1;
    }
    if (card && card_open(0, card)) fprintf(stderr, "[headless] aviso: no se pudo abrir la tarjeta '%s'\n", card);

    Cpu c = {0};
    c.r[28] = gp;
    c.r[29] = c.r[30] = 0x801FFFF0u; /* STACK de SYSTEM.CNF */
    c.r[31] = 0;
    gte_reset();
    gpu_reset();
    rt_host_frame = on_frame;
    signal(SIGALRM, on_alarm);
    rt_fatal_armed = 1;
    if (setjmp(rt_fatal_jmp)) { report(); return 2; }
    alarm(10);
    rt_call(&c, pc);
    fprintf(stderr, "[headless] la entrada retornó (v0=0x%08x) tras %u frames\n", c.r[2], hle_frame_count());
    report();
    (void)total;
    return 0;
}
