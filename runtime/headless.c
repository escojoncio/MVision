/* Arranque headless: carga un PS-X EXE en RAM y ejecuta su punto de entrada recompilado.
 *   headless <exe> [frames] [--dump DIR] [--every N] [--disc RUTAS] [--card FICHERO] [--full] [--sigs FIRMAS]
 *  - frames: VBlanks a ejecutar (por defecto 300).
 *  - --disc "dir1;dir2" (o variable MV_DISC): discos extraídos; --card (o MV_CARD): imagen .mcd.
 *  - --peek DIR,LEN (repetible): vuelca palabras de RAM al terminar (diagnóstico).
 *  - --dump DIR --every N: vuelca a PPM el área de display cada N frames (--full: VRAM completa).
 *  - --pad GUION: pulsa botones del mando 1 en frames concretos. GUION = lista separada por comas de
 *    FRAME:BOTONES[:DURACION]; BOTONES = nombres unidos por '+' (start select cross circle square triangle
 *    up down left right l1 l2 r1 r2); DURACION en frames (por defecto 6). Ej.: --pad 1800:start,2400:cross+up:10
 *  - --pad-every N:BOTON: pulsa BOTON 6 frames cada N frames (útil para saltar escenas). Repetible junto a --pad.
 *    Los botones se pasan a pad_set_state() en positivo (bit a 1 = pulsado; pad.c invierte); sticks a 0x80.
 *  - --sigs FICHERO (o MV_SIGS): firmas del SDK; obligatorio en el runtime sin código generado (headless_interp).
 * Al terminar imprime estadísticas de la GPU y del intérprete. */
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "cdrom.h"
#include "cpu.h"
#include "gpu.h"
#include "hle.h"
#include "hlebind.h"
#include "sys.h"

extern jmp_buf rt_fatal_jmp;
extern int rt_fatal_armed;
extern unsigned long interp_insns, interp_blocks, interp_invalidations;
extern unsigned long irq_count[11], irq_exceptions;
static uint32_t max_frames = 300;
static const char *dump_dir;
static uint32_t dump_every = 50;
static int dump_full;
static GpuStats total;
static struct timespec t0;
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
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double wall = (double)(t1.tv_sec - t0.tv_sec) + 1e-9 * (double)(t1.tv_nsec - t0.tv_nsec);
    if (wall > 0)
        fprintf(stderr, "[headless] rendimiento: %.2f s | %.1f VBlank/s (%.2fx tiempo real PAL) | %.1f M instr/s | bloques %lu, invalidaciones %lu\n",
                wall, sys_vblank_count() / wall, sys_vblank_count() / wall / 50.0, interp_insns / wall / 1e6, interp_blocks, interp_invalidations);
}

typedef struct { uint32_t start, dur, every; uint16_t mask; } PadEv;
static PadEv pad_ev[256];
static int npad_ev;

static const struct { const char *n; int bit; } pad_names[] = {
    {"select", 0}, {"l3", 1}, {"r3", 2}, {"start", 3}, {"up", 4}, {"right", 5}, {"down", 6}, {"left", 7},
    {"l2", 8}, {"r2", 9}, {"l1", 10}, {"r1", 11}, {"triangle", 12}, {"circle", 13}, {"cross", 14}, {"square", 15}};

/* "start+cross" -> máscara; devuelve 0 si algún nombre es desconocido */
static uint16_t pad_mask(const char *s, size_t len) {
    uint16_t m = 0;
    while (len) {
        size_t k = 0;
        while (k < len && s[k] != '+') k++;
        int ok = 0;
        for (size_t i = 0; i < sizeof pad_names / sizeof *pad_names; i++)
            if (strlen(pad_names[i].n) == k && !strncasecmp(s, pad_names[i].n, k)) { m |= (uint16_t)(1u << pad_names[i].bit); ok = 1; }
        if (!ok) return 0;
        s += k; len -= k;
        if (len) { s++; len--; }
    }
    return m;
}

static int pad_add_script(const char *g) {
    char *dup = strdup(g), *sv = NULL;
    for (char *tok = strtok_r(dup, ",", &sv); tok; tok = strtok_r(NULL, ",", &sv)) {
        char *e;
        uint32_t fr = (uint32_t)strtoul(tok, &e, 0);
        if (*e != ':' || npad_ev >= 256) { free(dup); return -1; }
        char *b = e + 1, *c2 = strchr(b, ':');
        uint32_t dur = 6;
        uint16_t m = pad_mask(b, c2 ? (size_t)(c2 - b) : strlen(b));
        if (c2) dur = (uint32_t)strtoul(c2 + 1, 0, 0);
        if (!m || !dur) { free(dup); return -1; }
        pad_ev[npad_ev++] = (PadEv){fr, dur, 0, m};
    }
    free(dup);
    return 0;
}

static int pad_add_every(const char *a) {
    char *e;
    uint32_t n = (uint32_t)strtoul(a, &e, 0);
    if (*e != ':' || !n || npad_ev >= 256) return -1;
    uint16_t m = pad_mask(e + 1, strlen(e + 1));
    if (!m) return -1;
    pad_ev[npad_ev++] = (PadEv){0, 6, n, m};
    return 0;
}

static void script_pad_frame(uint32_t f) {
    static uint16_t last = 0xFFFF;
    uint16_t b = 0;
    for (int i = 0; i < npad_ev; i++) {
        const PadEv *p = &pad_ev[i];
        if (p->every) { if (f >= p->every && f % p->every < p->dur) b |= p->mask; }
        else if (f >= p->start && f < p->start + p->dur) b |= p->mask;
    }
    if (b != last) { pad_set_state(0, b, 0x80, 0x80, 0x80, 0x80); last = b; }
}

static void on_frame(Cpu *c, uint32_t f) {
    (void)c;
    alarm(10);
    if (npad_ev) script_pad_frame(f);
    if (f % 60 == 0) fprintf(stderr, "[headless] frame %u\n", f);
    { /* MV_RAMDUMP_AT=N:FICHERO vuelca los 2 MB de RAM en el VBlank N (diagnóstico: comparar compilado/intérprete) */
        const char *e = getenv("MV_RAMDUMP_AT");
        if (e && (uint32_t)strtoul(e, 0, 0) == f && strchr(e, ':')) {
            FILE *o = fopen(strchr(e, ':') + 1, "wb");
            if (o) { fwrite(psx_ram, 1, PSX_RAM_SIZE, o); fclose(o); }
        }
    }
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
    const char *exe = NULL, *disc = getenv("MV_DISC"), *card = getenv("MV_CARD"), *sigs = getenv("MV_SIGS");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump_dir = argv[++i];
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) dump_every = (uint32_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--disc") && i + 1 < argc) disc = argv[++i];
        else if (!strcmp(argv[i], "--card") && i + 1 < argc) card = argv[++i];
        else if (!strcmp(argv[i], "--full")) dump_full = 1;
        else if (!strcmp(argv[i], "--sigs") && i + 1 < argc) sigs = argv[++i];
        else if (!strcmp(argv[i], "--pad") && i + 1 < argc) {
            if (pad_add_script(argv[++i])) { fprintf(stderr, "guion --pad inválido: '%s'\n", argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "--pad-every") && i + 1 < argc) {
            if (pad_add_every(argv[++i])) { fprintf(stderr, "--pad-every inválido: '%s' (N:BOTON)\n", argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "--peek") && i + 1 < argc && npeek < 8) {
            char *s = argv[++i], *e;
            peek_a[npeek] = (uint32_t)strtoul(s, &e, 16);
            peek_n[npeek++] = *e == ',' ? (uint32_t)strtoul(e + 1, 0, 0) : 16;
        }
        else if (!exe) exe = argv[i];
        else max_frames = (uint32_t)strtoul(argv[i], 0, 0);
    }
    if (!exe) { fprintf(stderr, "uso: %s <exe> [frames] [--dump DIR] [--every N] [--disc RUTAS] [--card MCD] [--full] [--pad GUION] [--pad-every N:BOTON]\n", argv[0]); return 1; }
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
#ifdef MV_NOGEN
    rt_entry = pc;
    if (!sigs) { fprintf(stderr, "falta el fichero de firmas del SDK (--sigs FICHERO o MV_SIGS)\n"); return 1; }
    if (hle_bind_file(sigs, t, sz, pc) < 0) return 1;
#else
    (void)sigs;
    if (pc != rt_entry) fprintf(stderr, "[headless] aviso: pc 0x%08x != entrada recompilada 0x%08x\n", pc, rt_entry);
#endif

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
    clock_gettime(CLOCK_MONOTONIC, &t0);
    rt_call(&c, pc);
    fprintf(stderr, "[headless] la entrada retornó (v0=0x%08x) tras %u frames\n", c.r[2], hle_frame_count());
    report();
    (void)total;
    return 0;
}
