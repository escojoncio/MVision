/* Intérprete MIPS R3000 de respaldo para direcciones sin función recompilada.
 * Mismo conjunto de instrucciones que emite tools/recomp/recomp.py, con delay slots y los mismos
 * helpers de memoria/GTE/COP0. Semántica de llamada: interp_call(c, t) ejecuta desde t hasta que el
 * código salta (jr) a la dirección de retorno con la que se entró (c->r[31] al entrar).
 *  - jal/jalr/bgezal/bltzal: si el destino tiene función recompilada se llama nativamente; si no,
 *    se interpreta recursivamente (la pila C refleja la pila de llamadas MIPS, igual que el código
 *    generado, así que fibras y setjmp del host siguen funcionando).
 *  - j / jr a un inicio de función recompilada: llamada en cola nativa y retorno.
 * Cada dirección de entrada interpretada se anota (sin duplicados) en missing_funcs.txt (cwd) para
 * pasarla al recompilador con --funcs. */
#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"

#define RS ((w >> 21) & 31)
#define RT ((w >> 16) & 31)
#define RD ((w >> 11) & 31)
#define SA ((w >> 6) & 31)
#define IMM (w & 0xFFFFu)
#define SIMM ((uint32_t)(int32_t)(int16_t)(w & 0xFFFFu))

/* ---- registro de direcciones interpretadas ---- */
#define SEEN_N 4096
static uint32_t seen[SEEN_N];
static unsigned nseen;
int interp_log_enabled = 1;
unsigned long interp_insns; /* contador global (estadística/pruebas) */

static void note_entry(uint32_t t) {
    for (unsigned i = 0; i < nseen; i++) if (seen[i] == t) return;
    if (nseen < SEEN_N) seen[nseen++] = t;
    fprintf(stderr, "[interp] interpretando 0x%08x (sin función recompilada)\n", t);
    if (!interp_log_enabled) return;
    FILE *f = fopen("missing_funcs.txt", "a+");
    if (!f) return;
    /* sin duplicados entre ejecuciones */
    rewind(f);
    char line[64];
    while (fgets(line, sizeof line, f))
        if (strtoul(line, 0, 16) == t) { fclose(f); return; }
    fprintf(f, "%08x\n", t);
    fclose(f);
}

static int is_bios_vec(uint32_t t) {
    t &= 0x1FFFFFFFu;
    return t == 0xA0 || t == 0xB0 || t == 0xC0;
}

static inline void wr(Cpu *c, int r, uint32_t v) { if (r) c->r[r] = v; }

/* Ejecuta una instrucción que no es salto. Devuelve 0 si no está soportada. */
static int exec_simple(Cpu *c, uint32_t pc, uint32_t w) {
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
        case 12: rt_syscall(c, pc); return 1;
        case 13: rt_break(c, pc); return 1;
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
    case 50: gte_lwc2(c, RT, mem_r32(c, ea)); return 1;
    case 58: mem_w32(c, ea, gte_swc2(c, RT)); return 1;
    case 16: /* COP0 */
        if (RS == 0) { wr(c, RT, cop0_read(c, RD)); return 1; }
        if (RS == 4) { cop0_write(c, RD, t); return 1; }
        if (RS == 16 && (w & 63) == 16) { cop0_rfe(c); return 1; }
        return 0;
    case 18: /* COP2 (GTE) */
        if (RS & 16) { gte_cmd(c, w & 0x1FFFFFFu); return 1; }
        if (RS == 0) { wr(c, RT, gte_mfc2(c, RD)); return 1; }
        if (RS == 2) { wr(c, RT, gte_cfc2(c, RD)); return 1; }
        if (RS == 4) { gte_mtc2(c, RD, t); return 1; }
        if (RS == 6) { gte_ctc2(c, RD, t); return 1; }
        return 0;
    default: return 0;
    }
}

static void step_slot(Cpu *c, uint32_t pc) {
    uint32_t w = mem_r32(c, pc);
    interp_insns++;
    if (!exec_simple(c, pc, w)) rt_unimpl(c, pc, w);
}

void interp_call(Cpu *c, uint32_t entry);

/* Llamada desde el intérprete: nativa si existe, si no interpretación recursiva. */
static void do_call(Cpu *c, uint32_t t) {
    if (is_bios_vec(t)) { bios_hle(c, t & 0xFF); return; }
    FuncPtr f = rt_lookup(t);
    if (f) f(c);
    else interp_call(c, t);
}

void interp_call(Cpu *c, uint32_t entry) {
    note_entry(entry);
    const uint32_t ret = c->r[31];
    uint32_t pc = entry;
    for (;;) {
        uint32_t w = mem_r32(c, pc);
        uint32_t op = w >> 26;
        interp_insns++;
        int kind = 0;           /* 0 simple, 1 rama condicional, 2 j, 3 jal, 4 jr, 5 jalr */
        int cond = 0, link = 0;
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
            if (!exec_simple(c, pc, w)) rt_unimpl(c, pc, w);
            pc += 4;
            continue;
        }
        /* Rama: condición y destino ya evaluados; ahora el delay slot; luego el salto. */
        if (link) c->r[31] = pc + 8;   /* bltzal/bgezal escriben ra siempre */
        step_slot(c, pc + 4);
        uint32_t next = pc + 8;
        switch (kind) {
        case 1:
            if (cond && link) { do_call(c, tgt); }
            else if (cond) next = tgt;
            break;
        case 2:
            if (rt_lookup(tgt) || is_bios_vec(tgt)) { do_call(c, tgt); return; }
            next = tgt;
            break;
        case 3:
            c->r[31] = pc + 8;
            do_call(c, tgt);
            break;
        case 4:
            if (tgt == ret) return;
            if (rt_lookup(tgt) || is_bios_vec(tgt)) { do_call(c, tgt); return; }
            next = tgt;
            break;
        case 5:
            wr(c, RD, pc + 8);
            do_call(c, tgt);
            break;
        }
        pc = next;
    }
}
