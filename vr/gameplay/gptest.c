/* gptest: validación dinámica de la capa de armas (disparo, recarga) con el runtime sin código generado (intérprete).
 *   gptest <exe> <frames> --sigs FIRMAS [--script FICHERO]        (disco: MV_DISC)
 * Guion (líneas; t = fotogramas desde que el jugador es controlable 30 fotogramas seguidos; "#" comenta):
 *   pad T0 T1 BOTONES            botones (nombres de headless.c unidos por '+') pulsados en T0 <= t < T1
 *   poke16 T DIR VALOR           escribe un s16/u16 en la RAM (SOLO pruebas: inventario de la copia de prueba)
 *   poke32 T DIR VALOR
 *   inv T                        inventario de prueba: todas las armas con munición (vars de partida) y la pistola equipada
 *   trace T0 T1                  una línea por VBlank con el estado del juego (arma, cargador, munición, estado, mando)
 *   end T                        fin
 * Nada de esto se enlaza a la app: es una herramienta de desarrollo (los datos del juego son locales). */
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "bridge.h"
#include "cdrom.h"
#include "cpu.h"
#include "gpu.h"
#include "hle.h"
#include "hlebind.h"
#include "sys.h"
#include "gptest_hooks.h"

extern jmp_buf rt_fatal_jmp;
extern int rt_fatal_armed;

typedef struct { int kind; int t0, t1; unsigned mask; uint32_t addr, val; char path[160]; } Ev;   /* path: ruta o etiqueta */
enum { E_PAD, E_POKE16, E_POKE32, E_INV, E_TRACE, E_END, E_DUMP, E_PROBE, E_PPM };
static Ev ev[512]; static int nev;
static int t_ready = -1, ok_count;
static BridgeState bs;
static uint32_t max_frames = 9000;

static const struct { const char *n; int bit; } pad_names[] = {
    {"select", 0}, {"l3", 1}, {"r3", 2}, {"start", 3}, {"up", 4}, {"right", 5}, {"down", 6}, {"left", 7},
    {"l2", 8}, {"r2", 9}, {"l1", 10}, {"r1", 11}, {"triangle", 12}, {"circle", 13}, {"cross", 14}, {"square", 15}};
static uint16_t pad_mask(const char *s) {
    uint16_t m = 0; char b[128]; snprintf(b, sizeof b, "%s", s);
    for (char *sv = NULL, *t = strtok_r(b, "+", &sv); t; t = strtok_r(NULL, "+", &sv)) {
        int ok = 0;
        for (size_t i = 0; i < sizeof pad_names / sizeof *pad_names; i++) if (!strcasecmp(t, pad_names[i].n)) { m |= (uint16_t)(1u << pad_names[i].bit); ok = 1; }
        if (!ok) { fprintf(stderr, "botón desconocido '%s'\n", t); exit(1); }
    }
    return m;
}
/* guion de arranque (hasta tomar el control): el mismo del headless */
static uint16_t boot_pad(uint32_t f) {
    uint16_t b = 0;
    if ((f >= 120 && f < 126) || (f >= 400 && f < 406)) b |= 1u << 3;
    if (f >= 97 && f % 97 < 6) b |= 1u << 13;
    if (f >= 211 && f % 211 < 6) b |= 1u << 7;
    if (f >= 151 && f % 151 < 6 && f < 2400) b |= 1u << 3;
    return b;
}
static void load_script(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) { perror(path); exit(1); }
    char l[256];
    while (fgets(l, sizeof l, f) && nev < 512) {
        char k[16], n[128]; Ev e; memset(&e, 0, sizeof e);
        if (sscanf(l, "%15s", k) != 1 || k[0] == '#') continue;
        if (!strcmp(k, "pad") && sscanf(l, "%*s %d %d %127s", &e.t0, &e.t1, n) == 3) { e.kind = E_PAD; e.mask = pad_mask(n); }
        else if (!strcmp(k, "poke16")) { e.kind = E_POKE16; sscanf(l, "%*s %d %x %i", &e.t0, &e.addr, (int *)&e.val); }
        else if (!strcmp(k, "poke32")) { e.kind = E_POKE32; sscanf(l, "%*s %d %x %i", &e.t0, &e.addr, (int *)&e.val); }
        else if (!strcmp(k, "inv")) { e.kind = E_INV; sscanf(l, "%*s %d", &e.t0); }
        else if (!strcmp(k, "trace")) { e.kind = E_TRACE; sscanf(l, "%*s %d %d", &e.t0, &e.t1); }
        else if (!strcmp(k, "ramdump")) { e.kind = E_DUMP; sscanf(l, "%*s %d %159s", &e.t0, e.path); }
        else if (!strcmp(k, "ppm")) { e.kind = E_PPM; sscanf(l, "%*s %d %159s", &e.t0, e.path); }
        else if (!strcmp(k, "probe")) { e.kind = E_PROBE; sscanf(l, "%*s %d %159s", &e.t0, e.path); }
        else if (!strcmp(k, "end")) { e.kind = E_END; sscanf(l, "%*s %d", &e.t0); }
        else { gptest_cmd(l); continue; }
        ev[nev++] = e;
    }
    fclose(f);
}

static int16_t r16(uint32_t a) { return *(int16_t *)(psx_ram + (a & 0x1FFFFF)); }
static int32_t r32(uint32_t a) { return *(int32_t *)(psx_ram + (a & 0x1FFFFF)); }
static void w16(uint32_t a, int v) { *(int16_t *)(psx_ram + (a & 0x1FFFFF)) = (int16_t)v; }
static void w32(uint32_t a, uint32_t v) { *(uint32_t *)(psx_ram + (a & 0x1FFFFF)) = v; }

/* variables de partida (puente): [14] arma equipada, [17+i] munición, [27+i] máximo */
static void set_inventory(void) {
    static const int ammo[10] = {60, 120, 6, 6, 4, 4, 4, 3, 3, 20}, mx[10] = {200, 400, 10, 10, 10, 10, 10, 10, 10, 100};
    for (int i = 0; i < 10; i++) { w16(BA_VARS + 2 * (BRIDGE_V_WEAPON_AMMO + i), ammo[i]); w16(BA_VARS + 2 * (BRIDGE_V_WEAPON_MAX + i), mx[i]); }
    w16(BA_VARS + 2 * BRIDGE_V_WEAPON, 0);
}

static void trace_line(uint32_t f, int t, uint16_t pad) {
    static char prev[512];
    char cur[512];
    uint32_t pc = (uint32_t)r32(BA_PLAYER_CTRL), work = pc - 0x20;
    int n = snprintf(cur, sizeof cur, "pos=%d,%d pad=%04x arma=%d mag=%d/%d mun=%d est=%08x modo=%s", bs.player_pos.x / 16, bs.player_pos.z / 16, pad, bs.weapon, r16(BA_MAGAZINE), r16(BA_MAGAZINE_MAX),
                     bs.weapon >= 0 && bs.weapon < 10 ? bs.weapon_ammo[bs.weapon] : -9, bs.player_status, bridge_mode_name(&bs));
    if (pc >= 0x80010000u && pc < 0x80200000u)
        snprintf(cur + n, sizeof cur - (size_t)n, " wa=%08x idx=%d trg=%d wfn=%08x menu=%02x pause=%d", (uint32_t)r32(work + 0x904), r32(work + 0x918), r32(work + 0x910), (uint32_t)r32(work + 0x908),
                 bs.menu_state, bs.pause_level);
    if (strcmp(cur, prev)) { fprintf(stderr, "[tr] f=%u t=%d %s\n", f, t, cur); strcpy(prev, cur); }
}

static uint16_t pad_now;
static void on_frame(Cpu *c, uint32_t f) {
    (void)c;
    alarm(60);
    bridge_read(psx_ram, &bs);
    int ok = (bs.valid & BRIDGE_OK_ALL_CORE) == BRIDGE_OK_ALL_CORE && bs.has_player && bs.in_control;
    if (ok) ok_count++; else ok_count = 0;
    if (t_ready < 0 && ok_count >= 30) t_ready = (int)f;
    int t = t_ready < 0 ? -1 : (int)f - t_ready;
    uint16_t b = 0;
    int want_trace = 0;
    if (t < 0) b = boot_pad(f);
    for (int i = 0; i < nev && t >= 0; i++) {
        Ev *e = &ev[i];
        switch (e->kind) {
        case E_PAD: if (t >= e->t0 && t < e->t1) b |= e->mask; break;
        case E_POKE16: if (t == e->t0) w16(e->addr, (int)e->val); break;
        case E_POKE32: if (t == e->t0) w32(e->addr, e->val); break;
        case E_INV: if (t == e->t0) set_inventory(); break;
        case E_TRACE: if (t >= e->t0 && t < e->t1) want_trace = 1; break;
        case E_DUMP: if (t == e->t0) { FILE *o = fopen(e->path, "wb"); if (o) { fwrite(psx_ram, 1, PSX_RAM_SIZE, o); fclose(o); } } break;
        case E_PPM: if (t == e->t0) gpu_dump_ppm(e->path, 0); break;
        case E_PROBE:
            if (t == e->t0)
                fprintf(stderr, "[probe] %s t=%d arma=%d mag=%d/%d mun=%d menu=%02x pausa=%d est=%08x\n", e->path, t, bs.weapon, r16(BA_MAGAZINE), r16(BA_MAGAZINE_MAX),
                        bs.weapon >= 0 && bs.weapon < 10 ? bs.weapon_ammo[bs.weapon] : -9, bs.menu_state, bs.pause_level, bs.player_status);
            break;
        case E_END: if (t >= e->t0) { gptest_finish(); fprintf(stderr, "[gptest] fin t=%d f=%u\n", t, f); exit(0); } break;
        }
    }
    gptest_frame(f, t, &bs, &b);
    if (want_trace) trace_line(f, t, b);
    if (b != pad_now) { pad_set_state(0, b, 0x80, 0x80, 0x80, 0x80); pad_now = b; }
    if (f % 500 == 0) fprintf(stderr, "[gptest] frame %u modo=%s t=%d\n", f, bridge_mode_name(&bs), t);
    if (f >= max_frames) { fprintf(stderr, "[gptest] límite de fotogramas (%u) sin llegar a end\n", f); gptest_finish(); exit(0); }
}
static void on_alarm(int s) { (void)s; rt_fatal(NULL, "sin VBlank en 60 s"); }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

int main(int argc, char **argv) {
    const char *exe = NULL, *disc = getenv("MV_DISC"), *sigs = getenv("MV_SIGS"), *script = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sigs") && i + 1 < argc) sigs = argv[++i];
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) script = argv[++i];
        else if (!exe) exe = argv[i];
        else max_frames = (uint32_t)strtoul(argv[i], 0, 0);
    }
    if (!exe || !sigs) { fprintf(stderr, "uso: %s <exe> <frames> --sigs FIRMAS [--script G]\n", argv[0]); return 1; }
    if (script) load_script(script);
    FILE *f = fopen(exe, "rb"); if (!f) { perror(exe); return 1; }
    static uint8_t buf[PSX_RAM_SIZE + 0x800];
    size_t n = fread(buf, 1, sizeof buf, f); fclose(f);
    if (n < 0x800 || memcmp(buf, "PS-X EXE", 8)) { fprintf(stderr, "ejecutable no válido\n"); return 1; }
    uint32_t pc = rd32(buf + 0x10), gp = rd32(buf + 0x14), t = rd32(buf + 0x18), sz = rd32(buf + 0x1C);
    memcpy(&psx_ram[t & 0x1FFFFF], buf + 0x800, sz);
    rt_entry = pc;
    gptest_pre_bind();
    if (hle_bind_file(sigs, t, sz, pc) < 0) return 1;
    if (gptest_init(psx_ram) < 0) return 1;
    if (disc && !cd_open(disc)) return 1;
    Cpu c = {0};
    c.r[28] = gp; c.r[29] = c.r[30] = 0x801FFFF0u; c.r[31] = 0;
    gte_reset(); gpu_reset();
    rt_host_frame = on_frame;
    signal(SIGALRM, on_alarm);
    rt_fatal_armed = 1;
    if (setjmp(rt_fatal_jmp)) return 2;
    alarm(60);
    rt_call(&c, pc);
    return 0;
}
