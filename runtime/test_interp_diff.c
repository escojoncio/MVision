/* Prueba diferencial del intérprete por bloques (interp.c) contra un intérprete de referencia de una instrucción
 * (la semántica sencilla del intérprete original). Para cada posible inicio de bloque de un binario de código
 * (EXE u overlay) ejecuta ambos con estados aleatorios (registros, hi/lo, RAM) y compara registros, hi/lo, RAM de
 * trabajo y destino del salto. Se compila con -DINTERP_TEST (interp_step).
 *   test_interp_diff <fichero> <base_hex> [offset_ini_hex fin_hex] [rondas]
 * El fichero se carga tal cual en RAM en <base>. Las cabeceras PS-X EXE se saltan solas (0x800). Sin datos
 * del juego en el repo: se invoca a mano con los binarios locales. Bloques con cop0/cop2/syscall/break se omiten. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"

const FuncEntry rt_func_table[] = {{0, 0}};
const unsigned rt_func_count = 0;
const FuncName rt_func_names[] = {{0xFFFFFFFFu, ""}};
const unsigned rt_func_name_count = 0;
const uint32_t rt_entry = 0;

uint32_t interp_step(Cpu *c, uint32_t pc);

/* Sin io.c: los registros de hardware son memoria plana (mem.c) y no hay efectos secundarios. */
int32_t rt_budget;
void sys_clock_tick(Cpu *c) { (void)c; }
uint32_t sys_dma_flags_take(void) { return 0; }
void sys_dma_irq_enable(int ch, int on) { (void)ch; (void)on; }
uint32_t sys_hcount(void) { return 0; }
void sys_idle(Cpu *c) { (void)c; }
void sys_irq_ack(int i) { (void)i; }
void sys_irq_enable(int i, int on) { (void)i; (void)on; }
uint32_t sys_irq_mask(void) { return 0; }
void sys_irq_raise(int i) { (void)i; }
uint32_t sys_irq_stat(void) { return 0; }
void sys_kick(void) {}
uint32_t sys_vblank_count(void) { return 0; }
extern int interp_log_enabled;

#define RS ((w >> 21) & 31)
#define RT ((w >> 16) & 31)
#define RD ((w >> 11) & 31)
#define SA ((w >> 6) & 31)
#define IMM (w & 0xFFFFu)
#define SIMM ((uint32_t)(int32_t)(int16_t)(w & 0xFFFFu))

static inline void wr(Cpu *c, int r, uint32_t v) { if (r) c->r[r] = v; }

/* Referencia: copia de exec_simple del intérprete original (sin cop0/cop2/syscall/break). */
static int ref_simple(Cpu *c, uint32_t w) {
    uint32_t op = w >> 26, s = c->r[RS], t = c->r[RT];
    uint32_t ea = s + SIMM;
    if (op == 0) {
        switch (w & 63) {
        case 0: wr(c, RD, t << SA); return 1;
        case 2: wr(c, RD, t >> SA); return 1;
        case 3: wr(c, RD, (uint32_t)((int32_t)t >> SA)); return 1;
        case 4: wr(c, RD, t << (s & 31)); return 1;
        case 6: wr(c, RD, t >> (s & 31)); return 1;
        case 7: wr(c, RD, (uint32_t)((int32_t)t >> (s & 31))); return 1;
        case 16: wr(c, RD, c->hi); return 1;
        case 17: c->hi = s; return 1;
        case 18: wr(c, RD, c->lo); return 1;
        case 19: c->lo = s; return 1;
        case 24: { int64_t m = (int64_t)(int32_t)s * (int32_t)t; c->lo = (uint32_t)m; c->hi = (uint32_t)(m >> 32); return 1; }
        case 25: { uint64_t m = (uint64_t)s * t; c->lo = (uint32_t)m; c->hi = (uint32_t)(m >> 32); return 1; }
        case 26: rt_div(c, s, t); return 1;
        case 27: rt_divu(c, s, t); return 1;
        case 32: case 33: wr(c, RD, s + t); return 1;
        case 34: case 35: wr(c, RD, s - t); return 1;
        case 36: wr(c, RD, s & t); return 1;
        case 37: wr(c, RD, s | t); return 1;
        case 38: wr(c, RD, s ^ t); return 1;
        case 39: wr(c, RD, ~(s | t)); return 1;
        case 42: wr(c, RD, (int32_t)s < (int32_t)t); return 1;
        case 43: wr(c, RD, s < t); return 1;
        default: return 0;
        }
    }
    switch (op) {
    case 8: case 9: wr(c, RT, s + SIMM); return 1;
    case 10: wr(c, RT, (int32_t)s < (int32_t)SIMM); return 1;
    case 11: wr(c, RT, s < SIMM); return 1;
    case 12: wr(c, RT, s & IMM); return 1;
    case 13: wr(c, RT, s | IMM); return 1;
    case 14: wr(c, RT, s ^ IMM); return 1;
    case 15: wr(c, RT, IMM << 16); return 1;
    case 32: { uint32_t v = (uint32_t)(int8_t)mem_r8(c, ea); wr(c, RT, v); return 1; }
    case 33: { uint32_t v = (uint32_t)(int16_t)mem_r16(c, ea); wr(c, RT, v); return 1; }
    case 34: wr(c, RT, mem_lwl(c, ea, t)); return 1;
    case 35: { uint32_t v = mem_r32(c, ea); wr(c, RT, v); return 1; }
    case 36: { uint32_t v = mem_r8(c, ea); wr(c, RT, v); return 1; }
    case 37: { uint32_t v = mem_r16(c, ea); wr(c, RT, v); return 1; }
    case 38: wr(c, RT, mem_lwr(c, ea, t)); return 1;
    case 40: mem_w8(c, ea, (uint8_t)t); return 1;
    case 41: mem_w16(c, ea, (uint16_t)t); return 1;
    case 42: mem_swl(c, ea, t); return 1;
    case 43: mem_w32(c, ea, t); return 1;
    case 46: mem_swr(c, ea, t); return 1;
    default: return 0;
    }
}

/* Un bloque de referencia: hasta salto+slot (o 60 instrucciones). Devuelve 0 si no se puede comparar. */
static int ref_block(Cpu *c, uint32_t pc, uint32_t *next) {
    for (unsigned count = 0;; count++) {
        if (count >= 60) { *next = pc; return 1; }
        uint32_t w = mem_r32(c, pc), op = w >> 26;
        int kind = 0, cond = 0, link = 0;
        uint32_t tgt = 0;
        if (op == 0 && ((w & 63) == 8 || (w & 63) == 9)) { kind = (w & 63) == 8 ? 4 : 5; tgt = c->r[RS]; }
        else if (op == 2 || op == 3) { kind = op == 2 ? 2 : 3; tgt = ((pc + 4) & 0xF0000000u) | ((w & 0x3FFFFFFu) << 2); }
        else if (op >= 4 && op <= 7) {
            uint32_t s = c->r[RS], t = c->r[RT];
            cond = op == 4 ? s == t : op == 5 ? s != t : op == 6 ? (int32_t)s <= 0 : (int32_t)s > 0;
            kind = 1; tgt = pc + 4 + (SIMM << 2);
        } else if (op == 1 && (RT == 0 || RT == 1 || RT == 16 || RT == 17)) {
            int32_t s = (int32_t)c->r[RS];
            cond = (RT & 1) ? s >= 0 : s < 0;
            link = RT >= 16;
            kind = 1; tgt = pc + 4 + (SIMM << 2);
        }
        if (kind == 0) {
            if (op == 16 || op == 18 || op == 50 || op == 58 || (op == 0 && ((w & 63) == 12 || (w & 63) == 13))) return 0;
            if (!ref_simple(c, w)) return 0;
            pc += 4;
            continue;
        }
        uint32_t wd = mem_r32(c, pc + 4), od = wd >> 26;
        if (od == 16 || od == 18 || od == 50 || od == 58 || (od == 0 && ((wd & 63) == 12 || (wd & 63) == 13))) return 0;
        if (link || kind == 3) c->r[31] = pc + 8;                /* bltzal/bgezal y jal: enlace antes del slot */
        if (kind == 5) wr(c, (w >> 11) & 31, pc + 8);            /* jalr: destino ya leído, enlace antes del slot */
        if (!ref_simple(c, wd)) return 0;
        switch (kind) {
        case 1: *next = cond ? tgt : pc + 8; return 1;
        default: *next = tgt; return 1;
        }
    }
}

#define WIN 0x80100000u
#define WSZ 0x20000u
static uint64_t rng = 88172645463325252ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }

static void rand_state(Cpu *c, uint8_t *win) {
    memset(c, 0, sizeof *c);
    for (int i = 1; i < 32; i++) {
        switch (rnd() % 6) {
        case 0: c->r[i] = WIN + 0x8000 + (rnd() & 0x7FF0); break;
        case 1: c->r[i] = WIN + 0x8000 + (rnd() & 0x7FFF); break;
        case 2: c->r[i] = rnd() % 64; break;
        case 3: c->r[i] = (uint32_t)-(int)(rnd() % 8); break;
        case 4: case 5: {   /* valores sueltos, lejos de la RAM para no corromper el código con las escrituras */
            uint32_t v = rnd();
            uint32_t top = v >> 24;
            if (top == 0 || top == 0x80 || top == 0xA0 || top == 0x1F || top == 0x20 || top == 0x40) v ^= 0x50000000u;
            c->r[i] = v;
            break;
        }
        default: c->r[i] = WIN + 0x8000 + (rnd() & 0x7FF8); break;
        }
    }
    c->hi = rnd(); c->lo = rnd();
    for (unsigned i = 0; i < WSZ; i += 4) { uint32_t v = rnd(); memcpy(win + i, &v, 4); }
}


extern void (*interp_test_hit)(uint32_t idx);
#define MAXD 4096
static uint32_t dirty[MAXD];
static unsigned ndirty;
static void on_hit(uint32_t idx) {
    for (uint32_t g = idx >> 8; g <= (idx >> 8) + 1; g++) {   /* un acceso no alineado puede cruzar al grano siguiente */
        unsigned i;
        for (i = 0; i < ndirty; i++) if (dirty[i] == g) break;
        if (i == ndirty && ndirty < MAXD) dirty[ndirty++] = g;
    }
}
static uint8_t snap[PSX_RAM_SIZE];
static uint8_t win0[WSZ];
static const uint8_t *base_of(uint32_t g) {
    uint32_t a = g << 8, w = WIN & 0x1FFFFF;
    return a >= w && a < w + WSZ ? &win0[a - w] : &snap[a];
}
static void restore_dirty(void) {
    for (unsigned i = 0; i < ndirty; i++) memcpy(&psx_ram[dirty[i] << 8], base_of(dirty[i]), 256);
    ndirty = 0;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: %s <fichero> <base_hex> [ini fin] [rondas]\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    static uint8_t buf[PSX_RAM_SIZE + 0x800];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    uint32_t base = (uint32_t)strtoul(argv[2], 0, 16);
    size_t skip = (n > 8 && !memcmp(buf, "PS-X EXE", 8)) ? 0x800 : 0;
    n -= skip;
    memcpy(&psx_ram[base & 0x1FFFFF], buf + skip, n);
    uint32_t lo = 0, hi = (uint32_t)n;
    if (argc >= 5) { lo = (uint32_t)strtoul(argv[3], 0, 16); hi = (uint32_t)strtoul(argv[4], 0, 16); }
    int rounds = argc >= 6 ? atoi(argv[5]) : 3;
    interp_log_enabled = 0;
    memset(rt_codemap, 1, sizeof rt_codemap);      /* toda escritura de RAM pasa por rt_code_hit -> on_hit */
    interp_test_hit = on_hit;
    unsigned tested = 0, skipped = 0, bad = 0;
    static uint8_t copyA[MAXD][256];
    static uint32_t dirtyA[MAXD];
    memcpy(snap, psx_ram, sizeof snap);
    for (uint32_t off = lo & ~3u; off < hi; off += 4) {
        uint32_t pc = base + off;
        for (int r = 0; r < rounds; r++) {
            Cpu a, b;
            rand_state(&a, win0);
            b = a;
            memcpy(&psx_ram[WIN & 0x1FFFFF], win0, WSZ);
            ndirty = 0;
            static uint8_t scr0[0x400], scrA[0x400];
            memcpy(scr0, psx_scratch, sizeof scr0);
            rt_budget = 1 << 30;
            uint32_t nx_ref;
            int ok = ref_block(&a, pc, &nx_ref);
            unsigned nA = ndirty;
            for (unsigned i = 0; i < nA; i++) { dirtyA[i] = dirty[i]; memcpy(copyA[i], &psx_ram[dirty[i] << 8], 256); }
            memcpy(scrA, psx_scratch, sizeof scrA);
            memcpy(psx_scratch, scr0, sizeof scr0);
            restore_dirty();
            if (!ok) { skipped++; break; }
            uint32_t nx_new = interp_step(&b, pc);
            tested++;
            int diff = nx_ref != nx_new || memcmp(&a, &b, sizeof a);
            int ramdiff = memcmp(psx_scratch, scrA, sizeof scrA) != 0;
            memcpy(psx_scratch, scr0, sizeof scr0);
            for (unsigned i = 0; i < ndirty && !ramdiff; i++) {      /* granos escritos por la nueva */
                const uint8_t *exp = base_of(dirty[i]);
                for (unsigned k = 0; k < nA; k++) if (dirtyA[k] == dirty[i]) exp = copyA[k];
                if (memcmp(&psx_ram[dirty[i] << 8], exp, 256)) ramdiff = 1;
            }
            for (unsigned k = 0; k < nA && !ramdiff; k++) {          /* granos escritos solo por la referencia */
                int seen = 0;
                for (unsigned i = 0; i < ndirty; i++) if (dirty[i] == dirtyA[k]) seen = 1;
                if (!seen && memcmp(copyA[k], base_of(dirtyA[k]), 256)) ramdiff = 1;
            }
            restore_dirty();
            if (diff || ramdiff) {
                bad++;
                if (bad <= 10) {
                    printf("DIFERENCIA en 0x%08x: next ref=0x%08x new=0x%08x%s\n", pc, nx_ref, nx_new, ramdiff ? " (RAM distinta)" : "");
                    for (int i = 0; i < 32; i++) if (a.r[i] != b.r[i]) printf("   r%d ref=0x%08x new=0x%08x\n", i, a.r[i], b.r[i]);
                    if (a.hi != b.hi || a.lo != b.lo) printf("   hi/lo ref=%08x/%08x new=%08x/%08x\n", a.hi, a.lo, b.hi, b.lo);
                    for (uint32_t k = 0; k < 14; k++) printf("   %08x: %08x\n", pc + 4 * k, mem_r32(NULL, pc + 4 * k));
                }
                break;
            }
        }
    }
    printf("bloques comparados %u, omitidos %u, diferencias %u\n", tested, skipped, bad);
    return bad != 0;
}
