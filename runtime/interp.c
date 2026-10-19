/* Intérprete MIPS R3000 por bloques básicos predecodificados.
 *
 * Es el motor de CPU del modo "sin código generado" (MV_NOGEN: nada derivado del juego en el binario; el EXE
 * se carga en ejecución) y el respaldo de las direcciones sin función recompilada en el modo recompilado.
 *
 * Diseño:
 *  - Un bloque = secuencia de instrucciones simples hasta un salto + su delay slot (máx. MAXI), predecodificada
 *    a "ops" de 16 bytes con el puntero a la etiqueta del manejador (computed goto, sin switch por instrucción).
 *    Caché de bloques indexada por la palabra de RAM de su PC (tab[], un puntero por palabra de los 2 MB).
 *  - Ejecución PLANA: jal/jr no recursan en C; el retorno es un salto indirecto como en el hardware. Cada llamada
 *    desde el host (rt_call/interp_call) abre una activación con ra = SENTINEL; al saltar a SENTINEL la
 *    activación termina y restaura el ra de entrada. Las pilas C solo anidan por HLE/callbacks/excepciones
 *    (cada hilo del juego sigue teniendo su pila host, ver fiber.c). setjmp/longjmp son HLE planas (hlebind.c).
 *  - Delay slots: si el slot no escribe los registros que lee el salto, se emite ANTES del salto (una op menos);
 *    si no, se evalúa el salto, se ejecuta el slot y se confirma (mismo orden que el código recompilado).
 *  - Fusión lui+ori/addiu en una constante.
 *  - Código marcado: rt_codemap[] (1 byte por 256 B de RAM) marca los granos con bloques; toda escritura del
 *    runtime o de la CPU en un grano marcado invalida los bloques solapados (rt_code_hit). Los bloques muertos
 *    no se liberan al instante (una pila de otro hilo puede seguir dentro): se liberan pasados 600 VBlank.
 *  - HLE por dirección (hlebind.c) y vectores de BIOS: bloques especiales de una op.
 *  - RT_POLL en las aristas hacia atrás, como el código recompilado.
 * Modo mixto (sin MV_NOGEN): las llamadas/saltos a direcciones con función recompilada (rt_lookup) se hacen
 * nativamente; las direcciones interpretadas sin función se anotan en missing_funcs.txt. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "hlebind.h"
#include "rtpatch.h"
#include "sys.h"

#ifdef INTERP_TRACE
void rt_trace_enter(Cpu *c, uint32_t addr);
#endif
#define SENTINEL 0xFFFFFF00u
#define MAXI 60                   /* instrucciones de un bloque antes de cortarlo (+ salto + slot <= 64 palabras) */
#define GRAN 8                    /* grano de rt_codemap: 256 B */

uint8_t rt_codemap[PSX_RAM_SIZE >> GRAN];
int interp_log_enabled = 1;
unsigned long interp_insns;       /* instrucciones ejecutadas (por bloques completos; estadística/pruebas) */
unsigned long interp_blocks, interp_invalidations;   /* bloques decodificados / invalidaciones (estadística) */

typedef struct Op {
    const void *h;
    union { struct { uint32_t imm; uint8_t d, s, t, sa; }; void *ptr; };
} Op;

typedef struct Block {
    uint32_t pc, end, ninsn, nops, special;
    Op ops[];
} Block;

static Block **tab;               /* PSX_RAM_SIZE/4 punteros, creado en el primer uso */

enum {
    O_NOP, O_SLL, O_SRL, O_SRA, O_SLLV, O_SRLV, O_SRAV, O_ADDU, O_SUBU, O_AND, O_OR, O_XOR, O_NOR, O_SLT, O_SLTU,
    O_MFHI, O_MFLO, O_MTHI, O_MTLO, O_MULT, O_MULTU, O_DIV, O_DIVU,
    O_ADDIU, O_SLTI, O_SLTIU, O_ANDI, O_ORI, O_XORI, O_LI,
    O_LB, O_LH, O_LW, O_LBU, O_LHU, O_LWL, O_LWR, O_SB, O_SH, O_SW, O_SWL, O_SWR, O_LD0,
    O_MFC0, O_MTC0, O_RFE, O_MFC2, O_CFC2, O_MTC2, O_CTC2, O_LWC2, O_SWC2, O_GTE,
    O_SYS, O_BRK, O_END_MTC0, O_END_SYS, O_END_BRK, O_UNIMPL, O_FALL,
    /* evaluación del salto (el slot va después) */
    O_E_BEQ, O_E_BNE, O_E_BLEZ, O_E_BGTZ, O_E_BLTZ, O_E_BGEZ, O_E_BLTZAL, O_E_BGEZAL, O_E_JR,
    /* confirmación del salto tras el slot */
    O_LINK, O_C_BR, O_C_BRAL, O_C_J, O_C_JAL, O_C_JALN, O_C_JR, O_C_JALR,
    /* salto con el slot ya emitido antes */
    O_F_BEQ, O_F_BNE, O_F_BLEZ, O_F_BGTZ, O_F_BLTZ, O_F_BGEZ, O_F_J, O_F_JAL, O_F_JR, O_F_JALR,
    O_HLE, O_BIOS, O_EXIT,
    O_COUNT
};
static const void *lbl[O_COUNT];

#ifndef MV_NOGEN
#define SEEN_N 4096
static uint32_t seen[SEEN_N];
static unsigned nseen;

static void note_entry(uint32_t t, uint32_t c_ra) {
    for (unsigned i = 0; i < nseen; i++) if (seen[i] == t) return;
    if (nseen < SEEN_N) seen[nseen++] = t;
    fprintf(stderr, "[interp] interpretando 0x%08x %s (sin función recompilada; ra=0x%08x %s)\n", t, rt_name(t), c_ra, rt_name(c_ra));
    /* MV_RAMDUMP=fichero: vuelca la RAM al primer aviso (diagnóstico: localizar overlays cargadas). */
    { const char *rd = getenv("MV_RAMDUMP"); static int done;
      if (rd && !done && t >= 0x800c0000u) { done = 1; FILE *g = fopen(rd, "wb"); if (g) { fwrite(psx_ram, 1, PSX_RAM_SIZE, g); fclose(g); } } }
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
#endif

static int is_bios_vec(uint32_t t) {
    t &= 0x1FFFFFFFu;
    return t == 0xA0 || t == 0xB0 || t == 0xC0;
}

/* ------------------------------------------------------------------ caché de bloques */
typedef struct { Block *b; uint32_t born; } Zombie;
static Zombie *zomb;
static unsigned nzomb, zcap;

static void zombie_reclaim(void) {
    uint32_t now = sys_vblank_count();
    unsigned k = 0;
    for (unsigned i = 0; i < nzomb; i++) {
        if (now - zomb[i].born > 600u) free(zomb[i].b);
        else zomb[k++] = zomb[i];
    }
    nzomb = k;
}

static void kill_block(unsigned w) {
    Block *b = tab[w];
    tab[w] = NULL;
    if (nzomb == zcap) {
        zombie_reclaim();
        if (nzomb == zcap) { zcap = zcap ? zcap * 2 : 1024; zomb = realloc(zomb, zcap * sizeof *zomb); if (!zomb) abort(); }
    }
    zomb[nzomb].b = b; zomb[nzomb].born = sys_vblank_count(); nzomb++;
}

/* Escritura (CPU o runtime) sobre un grano marcado: invalida los bloques que lo solapan. */
#ifdef INTERP_TEST
void (*interp_test_hit)(uint32_t idx);   /* prueba diferencial: registra los granos escritos */
#endif
void rt_code_hit(uint32_t idx) {
#ifdef INTERP_TEST
    if (interp_test_hit) { interp_test_hit(idx); rt_codemap[idx >> GRAN] = 1; return; }
#endif
    if (!tab) return;
    uint32_t g0 = (idx >> GRAN) << GRAN, g1 = g0 + (1u << GRAN);
    unsigned w0 = (g0 >> 2) > 64 ? (g0 >> 2) - 64 : 0, w1 = (g1 >> 2);
    for (unsigned w = w0; w < w1; w++) {
        Block *b = tab[w];
        if (!b || b->special) continue;
        uint32_t s = (b->pc & 0x1FFFFF), e = s + (b->end - b->pc);
        if (e > g0 && s < g1) { kill_block(w); interp_invalidations++; }
    }
    rt_codemap[g0 >> GRAN] = 0;
}

void rt_code_invalidate(uint32_t addr, uint32_t len) {
    if (!tab || !len) return;
    uint32_t a = addr & 0x1FFFFFFFu;
    if (a >= 0x800000u) return;
    a &= 0x1FFFFF;
    uint32_t e = a + len;
    if (e > PSX_RAM_SIZE) e = PSX_RAM_SIZE;
    for (uint32_t g = a >> GRAN; g <= (e - 1) >> GRAN; g++) if (rt_codemap[g]) rt_code_hit(g << GRAN);
}

/* Registro/retirada de una sustitución (rtpatch.c): el bloque especial de `addr` y los bloques que lo cubren se decodifican de nuevo. */
void interp_forget(uint32_t addr) {
    if (!tab) return;
    unsigned w = (addr & 0x1FFFFC) >> 2;
    if (tab[w] && tab[w]->special) kill_block(w);
    rt_code_invalidate(addr, 4);
}

void interp_flush_all(void) {
    if (!tab) return;
    for (unsigned w = 0; w < PSX_RAM_SIZE / 4; w++) if (tab[w]) kill_block(w);
    memset(rt_codemap, 0, sizeof rt_codemap);
}

/* ------------------------------------------------------------------ decodificación */
#define F_RS ((w >> 21) & 31)
#define F_RT ((w >> 16) & 31)
#define F_RD ((w >> 11) & 31)
#define F_SA ((w >> 6) & 31)
#define F_IMM (w & 0xFFFFu)
#define F_SIMM ((uint32_t)(int32_t)(int16_t)(w & 0xFFFFu))
#define F_RS_OF(x) (((x) >> 21) & 31)
#define F_RT_OF(x) (((x) >> 16) & 31)

static inline Op mk(int id, unsigned d, unsigned s, unsigned t, unsigned sa, uint32_t imm) {
    Op o;
    o.h = lbl[id]; o.imm = imm; o.d = (uint8_t)d; o.s = (uint8_t)s; o.t = (uint8_t)t; o.sa = (uint8_t)sa;
    return o;
}

/* Decodifica una instrucción que no es salto. Devuelve 1 si emite una op, 0 si no hace nada (nop / destino r0),
 * -1 si no está soportada. *dst = registro que escribe (0 si ninguno). *end = 1 si debe cortar el bloque. */
static int decode_simple(uint32_t w, uint32_t pc, Op *o, unsigned *dst, int *end) {
    uint32_t op = w >> 26;
    unsigned rs = F_RS, rt = F_RT, rd = F_RD, sa = F_SA;
    *dst = 0; *end = 0;
    if (op == 0) {
        unsigned f = w & 63;
        int id = -1;
        switch (f) {
        case 0: id = O_SLL; break;
        case 2: id = O_SRL; break;
        case 3: id = O_SRA; break;
        case 4: id = O_SLLV; break;
        case 6: id = O_SRLV; break;
        case 7: id = O_SRAV; break;
        case 32: case 33: id = O_ADDU; break;
        case 34: case 35: id = O_SUBU; break;
        case 36: id = O_AND; break;
        case 37: id = O_OR; break;
        case 38: id = O_XOR; break;
        case 39: id = O_NOR; break;
        case 42: id = O_SLT; break;
        case 43: id = O_SLTU; break;
        case 16: id = O_MFHI; break;
        case 18: id = O_MFLO; break;
        case 17: *o = mk(O_MTHI, 0, rs, 0, 0, 0); return 1;
        case 19: *o = mk(O_MTLO, 0, rs, 0, 0, 0); return 1;
        case 24: *o = mk(O_MULT, 0, rs, rt, 0, 0); return 1;
        case 25: *o = mk(O_MULTU, 0, rs, rt, 0, 0); return 1;
        case 26: *o = mk(O_DIV, 0, rs, rt, 0, 0); return 1;
        case 27: *o = mk(O_DIVU, 0, rs, rt, 0, 0); return 1;
        case 12: *o = mk(O_SYS, 0, 0, 0, 0, pc); *end = 1; return 1;
        case 13: *o = mk(O_BRK, 0, 0, 0, 0, pc); *end = 1; return 1;
        default: return -1;
        }
        if (rd == 0) return 0;
        *dst = rd;
        *o = mk(id, rd, rs, rt, sa, 0);
        return 1;
    }
    switch (op) {
    case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15: {
        if (rt == 0) return 0;
        *dst = rt;
        static const unsigned char ids[] = {O_ADDIU, O_ADDIU, O_SLTI, O_SLTIU, O_ANDI, O_ORI, O_XORI, O_LI};
        uint32_t imm = (op == 15) ? (F_IMM << 16) : (op <= 11) ? F_SIMM : F_IMM;
        *o = mk(ids[op - 8], 0, rs, rt, 0, imm);
        return 1;
    }
    case 32: case 33: case 35: case 36: case 37: {      /* lb lh lw lbu lhu */
        static const struct { unsigned char id, sz; } m[] = {{O_LB, 1}, {O_LH, 2}, {0, 0}, {O_LW, 4}, {O_LBU, 1}, {O_LHU, 2}};
        unsigned k = op - 32;
        if (rt == 0) { *o = mk(O_LD0, 0, rs, 0, m[k].sz, F_SIMM); return 1; }
        *dst = rt;
        *o = mk(m[k].id, 0, rs, rt, 0, F_SIMM);
        return 1;
    }
    case 34: case 38:                                   /* lwl lwr */
        if (rt == 0) return 0;
        *dst = rt;
        *o = mk(op == 34 ? O_LWL : O_LWR, 0, rs, rt, 0, F_SIMM);
        return 1;
    case 40: *o = mk(O_SB, 0, rs, rt, 0, F_SIMM); return 1;
    case 41: *o = mk(O_SH, 0, rs, rt, 0, F_SIMM); return 1;
    case 43: *o = mk(O_SW, 0, rs, rt, 0, F_SIMM); return 1;
    case 42: *o = mk(O_SWL, 0, rs, rt, 0, F_SIMM); return 1;
    case 46: *o = mk(O_SWR, 0, rs, rt, 0, F_SIMM); return 1;
    case 50: *o = mk(O_LWC2, 0, rs, rt, 0, F_SIMM); return 1;
    case 58: *o = mk(O_SWC2, 0, rs, rt, 0, F_SIMM); return 1;
    case 16:
        if (rs == 0) { if (rt == 0) return 0; *dst = rt; *o = mk(O_MFC0, rd, 0, rt, 0, 0); return 1; }
        if (rs == 4) { *o = mk(O_MTC0, rd, 0, rt, 0, 0); *end = 1; return 1; }
        if (rs == 16 && (w & 63) == 16) { *o = mk(O_RFE, 0, 0, 0, 0, 0); return 1; }
        return -1;
    case 18:
        if (rs & 16) { *o = mk(O_GTE, 0, 0, 0, 0, w & 0x1FFFFFFu); return 1; }
        if (rs == 0) { if (rt == 0) { *o = mk(O_MFC2, rd, 0, 0, 1, 0); return 1; } *dst = rt; *o = mk(O_MFC2, rd, 0, rt, 0, 0); return 1; }
        if (rs == 2) { if (rt == 0) return 0; *dst = rt; *o = mk(O_CFC2, rd, 0, rt, 0, 0); return 1; }
        if (rs == 4) { *o = mk(O_MTC2, rd, 0, rt, 0, 0); return 1; }
        if (rs == 6) { *o = mk(O_CTC2, rd, 0, rt, 0, 0); return 1; }
        return -1;
    default: return -1;
    }
}

static inline uint32_t fetch(uint32_t a) {
    uint32_t w;
    memcpy(&w, &psx_ram[a & 0x1FFFFC], 4);
    return w;
}

/* Terminadores. kind: 0 no es salto. */
typedef struct { int kind; unsigned s, t; uint32_t tgt; int link, ra_backward; } Br;
enum { K_NONE, K_BEQ, K_BNE, K_BLEZ, K_BGTZ, K_BLTZ, K_BGEZ, K_J, K_JAL, K_JR, K_JALR };

static Br classify(uint32_t w, uint32_t a) {
    Br b = {K_NONE, 0, 0, 0, 0, 0};
    uint32_t op = w >> 26;
    if (op == 0 && ((w & 63) == 8 || (w & 63) == 9)) {
        b.kind = (w & 63) == 8 ? K_JR : K_JALR; b.s = F_RS; b.t = F_RD;
    } else if (op == 2 || op == 3) {
        b.kind = op == 2 ? K_J : K_JAL;
        b.tgt = ((a + 4) & 0xF0000000u) | ((w & 0x3FFFFFFu) << 2);
    } else if (op >= 4 && op <= 7) {
        b.kind = K_BEQ + (int)(op - 4); b.s = F_RS; b.t = F_RT; b.tgt = a + 4 + (F_SIMM << 2);
    } else if (op == 1 && (F_RT == 0 || F_RT == 1 || F_RT == 16 || F_RT == 17)) {
        b.kind = (F_RT & 1) ? K_BGEZ : K_BLTZ; b.link = F_RT >= 16; b.s = F_RS; b.tgt = a + 4 + (F_SIMM << 2);
    }
    return b;
}

static Block *finish(const Op *tmp, unsigned n, uint32_t pc, uint32_t end, int mark) {
    Block *b = malloc(sizeof(Block) + n * sizeof(Op));
    if (!b) abort();
    b->pc = pc; b->end = end; b->ninsn = (end - pc) >> 2; b->nops = n; b->special = !mark;
    memcpy(b->ops, tmp, n * sizeof(Op));
    interp_blocks++;
    if (mark) {
        uint32_t s = pc & 0x1FFFFF, e = s + (end - pc);
        if (e > PSX_RAM_SIZE) e = PSX_RAM_SIZE;      /* bloque al final de la RAM: no salirse de rt_codemap */
        for (uint32_t g = s >> GRAN; g <= (e - 1) >> GRAN; g++) rt_codemap[g] = 1;
    }
    return b;
}

static Block *decode(uint32_t pc, int plain) {
    pc = 0x80000000u | (pc & 0x1FFFFFFCu);
    Op tmp[MAXI + 16];
    unsigned n = 0;
    if (!plain) {
        FuncPtr f = rt_patch_is(pc) ? rt_patch_lookup(pc) : NULL;   /* sustitución en ejecución (rtpatch.c) */
        if (!f) f = hle_bind_lookup(pc);
        if (f) { tmp[0] = mk(O_HLE, 0, 0, 0, 0, 0); tmp[0].ptr = (void *)f; tmp[0].h = lbl[O_HLE]; return finish(tmp, 1, pc, pc + 4, 0); }
        if (is_bios_vec(pc)) { tmp[0] = mk(O_BIOS, 0, 0, 0, 0, pc & 0xFF); return finish(tmp, 1, pc, pc + 4, 0); }
    }
    uint32_t a = pc;
    int last_lui = -1;                    /* índice en tmp de un lui recién emitido (fusión) */
    for (unsigned count = 0;; count++) {
        uint32_t w = fetch(a);
        if (count >= MAXI || (count > 0 && (hle_bind_is(a) || rt_patch_is(a)))) { tmp[n++] = mk(O_FALL, 0, 0, 0, 0, 0); return finish(tmp, n, pc, a, 1); }
        Br br = classify(w, a);
        if (br.kind == K_NONE) {
            Op o;
            unsigned dst;
            int end;
            int r = decode_simple(w, a, &o, &dst, &end);
            if (r < 0) {
                tmp[n++] = mk(O_UNIMPL, 0, 0, 0, 0, w);
                tmp[n++] = mk(O_NOP, 0, 0, 0, 0, a);
                return finish(tmp, n, pc, a + 4, 1);
            }
            if (r > 0) {
                int fused = 0;
                if (last_lui >= 0 && (o.h == lbl[O_ORI] || o.h == lbl[O_ADDIU]) && o.s == tmp[last_lui].t) {
                    uint32_t v = o.h == lbl[O_ORI] ? (tmp[last_lui].imm | o.imm) : (tmp[last_lui].imm + o.imm);
                    if (o.t == tmp[last_lui].t) { tmp[last_lui].imm = v; fused = 1; }          /* lui r; ori r,r,x */
                    else { o = mk(O_LI, 0, 0, o.t, 0, v); }                                    /* dos registros */
                }
                last_lui = -1;
                if (!fused) {
                    if (o.h == lbl[O_LI]) last_lui = (int)n;
                    tmp[n++] = o;
                }
                if (end) {                    /* syscall / break / mtc0: la op puede entregar una excepción; se corta el bloque */
                    Op *e = &tmp[n - 1];
                    if (e->h == lbl[O_SYS]) e->h = lbl[O_END_SYS];
                    else if (e->h == lbl[O_BRK]) e->h = lbl[O_END_BRK];
                    else { e->h = lbl[O_END_MTC0]; e->imm = a + 4; }
                    return finish(tmp, n, pc, a + 4, 1);
                }
            } else last_lui = -1;
            a += 4;
            continue;
        }
        /* salto + delay slot */
        uint32_t wd = fetch(a + 4);
        Br bd = classify(wd, a + 4);
        Op ds;
        unsigned dd = 0;
        int dend, dr = bd.kind != K_NONE ? -1 : decode_simple(wd, a + 4, &ds, &dd, &dend);
        int bwd = br.tgt <= a;
        if (dr < 0) {
            tmp[n++] = mk(O_UNIMPL, 0, 0, 0, 0, wd);
            tmp[n++] = mk(O_NOP, 0, 0, 0, 0, a + 4);
            return finish(tmp, n, pc, a + 8, 1);
        }
        int reads_hazard = 0;
        switch (br.kind) {
        case K_BEQ: case K_BNE: reads_hazard = dd && (dd == br.s || dd == br.t); break;
        case K_J: case K_JAL: break;
        default: reads_hazard = dd && dd == br.s; break;
        }
        if (br.link) reads_hazard = 1;      /* bltzal/bgezal escriben ra antes del slot */
        /* jal/jalr escriben el enlace en el propio salto: el slot lo ve (y si lo escribe, gana el slot). Solo importa
         * cuando el slot toca ese registro; si no, se emite el slot primero y el enlace después. */
        unsigned lk = br.kind == K_JAL ? 31 : br.kind == K_JALR ? br.t : 0;
        if (lk && (dd == lk || F_RS_OF(wd) == lk || F_RT_OF(wd) == lk)) reads_hazard = 1;
        if (!reads_hazard) {
            if (dr > 0) tmp[n++] = ds;
            Op t;
            switch (br.kind) {
            case K_BEQ: t = mk(O_F_BEQ, 0, br.s, br.t, bwd, br.tgt); break;
            case K_BNE: t = mk(O_F_BNE, 0, br.s, br.t, bwd, br.tgt); break;
            case K_BLEZ: t = mk(O_F_BLEZ, 0, br.s, 0, bwd, br.tgt); break;
            case K_BGTZ: t = mk(O_F_BGTZ, 0, br.s, 0, bwd, br.tgt); break;
            case K_BLTZ: t = mk(O_F_BLTZ, 0, br.s, 0, bwd, br.tgt); break;
            case K_BGEZ: t = mk(O_F_BGEZ, 0, br.s, 0, bwd, br.tgt); break;
            case K_J: t = mk(O_F_J, 0, 0, 0, bwd, br.tgt); break;
            case K_JAL: t = mk(O_F_JAL, 0, 0, 0, 0, br.tgt); break;
            case K_JR: t = mk(O_F_JR, 0, br.s, 0, br.s != 31, 0); break;
            default: t = mk(O_F_JALR, br.t, br.s, 0, 0, 0); break;
            }
            tmp[n++] = t;
        } else {
            Op e = mk(O_NOP, 0, 0, 0, 0, 0), lko = e, c;
            switch (br.kind) {
            case K_BEQ: e = mk(O_E_BEQ, 0, br.s, br.t, 0, 0); c = mk(O_C_BR, 0, 0, 0, bwd, br.tgt); break;
            case K_BNE: e = mk(O_E_BNE, 0, br.s, br.t, 0, 0); c = mk(O_C_BR, 0, 0, 0, bwd, br.tgt); break;
            case K_BLEZ: e = mk(O_E_BLEZ, 0, br.s, 0, 0, 0); c = mk(O_C_BR, 0, 0, 0, bwd, br.tgt); break;
            case K_BGTZ: e = mk(O_E_BGTZ, 0, br.s, 0, 0, 0); c = mk(O_C_BR, 0, 0, 0, bwd, br.tgt); break;
            case K_BLTZ: e = mk(br.link ? O_E_BLTZAL : O_E_BLTZ, 0, br.s, 0, 0, 0); c = mk(br.link ? O_C_BRAL : O_C_BR, 0, 0, 0, bwd, br.tgt); break;
            case K_BGEZ: e = mk(br.link ? O_E_BGEZAL : O_E_BGEZ, 0, br.s, 0, 0, 0); c = mk(br.link ? O_C_BRAL : O_C_BR, 0, 0, 0, bwd, br.tgt); break;
            case K_J: c = mk(O_C_J, 0, 0, 0, bwd, br.tgt); break;
            case K_JAL: lko = mk(O_LINK, 31, 0, 0, 0, a + 8); c = mk(O_C_JALN, 0, 0, 0, 0, br.tgt); break;
            case K_JR: e = mk(O_E_JR, 0, br.s, 0, 0, 0); c = mk(O_C_JR, 0, 0, 0, br.s != 31, 0); break;
            default:   /* jalr: destino leído antes; enlace antes del slot */
                e = mk(O_E_JR, 0, br.s, 0, 0, 0);
                if (br.t) lko = mk(O_LINK, br.t, 0, 0, 0, a + 8);
                c = mk(O_C_JALR, 0, 0, 0, 0, 0);
                break;
            }
            if (e.h != lbl[O_NOP]) tmp[n++] = e;
            if (lko.h != lbl[O_NOP]) tmp[n++] = lko;
            if (dr > 0) tmp[n++] = ds;
            tmp[n++] = c;
        }
        return finish(tmp, n, pc, a + 8, 1);
    }
}

/* ------------------------------------------------------------------ memoria rápida */
static inline uint32_t rd32(Cpu *c, uint32_t a) {
    uint32_t p = a & 0x1FFFFFFFu, v;
    if (__builtin_expect(p < 0x800000u, 1)) { memcpy(&v, &psx_ram[p & 0x1FFFFF], 4); return v; }
    if (p - PSX_SCRATCH < 0x400u) { memcpy(&v, &psx_scratch[p - PSX_SCRATCH], 4); return v; }
    return mem_r32(c, a);
}
static inline uint32_t rd16(Cpu *c, uint32_t a) {
    uint32_t p = a & 0x1FFFFFFFu;
    uint16_t v;
    if (__builtin_expect(p < 0x800000u, 1)) { memcpy(&v, &psx_ram[p & 0x1FFFFF], 2); return v; }
    if (p - PSX_SCRATCH < 0x400u) { memcpy(&v, &psx_scratch[p - PSX_SCRATCH], 2); return v; }
    return mem_r16(c, a);
}
static inline uint32_t rd8(Cpu *c, uint32_t a) {
    uint32_t p = a & 0x1FFFFFFFu;
    if (__builtin_expect(p < 0x800000u, 1)) return psx_ram[p & 0x1FFFFF];
    if (p - PSX_SCRATCH < 0x400u) return psx_scratch[p - PSX_SCRATCH];
    return mem_r8(c, a);
}
static inline void ram_hit(uint32_t idx) { if (__builtin_expect(rt_codemap[idx >> GRAN], 0)) rt_code_hit(idx); }
static inline void wr32(Cpu *c, uint32_t a, uint32_t v) {
    uint32_t p = a & 0x1FFFFFFFu;
    if (__builtin_expect(p < 0x800000u, 1)) { uint32_t i = p & 0x1FFFFF; memcpy(&psx_ram[i], &v, 4); ram_hit(i); return; }
    if (p - PSX_SCRATCH < 0x400u) { memcpy(&psx_scratch[p - PSX_SCRATCH], &v, 4); return; }
    mem_w32(c, a, v);
}
static inline void wr16(Cpu *c, uint32_t a, uint16_t v) {
    uint32_t p = a & 0x1FFFFFFFu;
    if (__builtin_expect(p < 0x800000u, 1)) { uint32_t i = p & 0x1FFFFF; memcpy(&psx_ram[i], &v, 2); ram_hit(i); return; }
    if (p - PSX_SCRATCH < 0x400u) { memcpy(&psx_scratch[p - PSX_SCRATCH], &v, 2); return; }
    mem_w16(c, a, v);
}
static inline void wr8(Cpu *c, uint32_t a, uint8_t v) {
    uint32_t p = a & 0x1FFFFFFFu;
    if (__builtin_expect(p < 0x800000u, 1)) { uint32_t i = p & 0x1FFFFF; psx_ram[i] = v; ram_hit(i); return; }
    if (p - PSX_SCRATCH < 0x400u) { psx_scratch[p - PSX_SCRATCH] = v; return; }
    mem_w8(c, a, v);
}

/* ------------------------------------------------------------------ ejecución */
#ifdef INTERP_RING
static uint32_t ring[256];
static unsigned ringpos;
void interp_dump_ring(void) { for (unsigned i = ringpos > 64 ? ringpos - 64 : 0; i < ringpos; i++) fprintf(stderr, "  ring %08x %s\n", ring[i & 255], rt_name(ring[i & 255])); }
#endif
/* Anillo siempre activo de las últimas direcciones de bloque ejecutadas (diagnóstico del vigilante del host; lectura racy aceptada). */
#define WD_RING 64u
static volatile uint32_t wd_ring[WD_RING];
static volatile unsigned wd_pos;
int interp_recent_blocks(uint32_t *out, int max) {
    unsigned p = wd_pos;
    int n = 0;
    for (unsigned i = p > WD_RING ? p - WD_RING : 0; i < p && n < max; i++) out[n++] = wd_ring[i & (WD_RING - 1)];
    return n;
}
static Block *lookup_slow(Cpu *c, uint32_t pc, uint32_t ret) {
    (void)c;
    if (pc == SENTINEL || pc == ret) return NULL;
    if ((pc & 0x1FFFFFFFu) >= 0x800000u) rt_fatal(c, "ejecución fuera de RAM en 0x%08x", pc);
    Block *b = decode(pc, 0);
    tab[(pc & 0x1FFFFC) >> 2] = b;
    return b;
}

#ifdef INTERP_TEST
static int step_mode;
static uint32_t step_pc;
#endif
static void run(Cpu *c, uint32_t pc, Block *first) {
    if (!c) {
#define L(x) lbl[O_##x] = &&L_##x
        L(NOP); L(SLL); L(SRL); L(SRA); L(SLLV); L(SRLV); L(SRAV); L(ADDU); L(SUBU); L(AND); L(OR); L(XOR); L(NOR); L(SLT); L(SLTU);
        L(MFHI); L(MFLO); L(MTHI); L(MTLO); L(MULT); L(MULTU); L(DIV); L(DIVU);
        L(ADDIU); L(SLTI); L(SLTIU); L(ANDI); L(ORI); L(XORI); L(LI);
        L(LB); L(LH); L(LW); L(LBU); L(LHU); L(LWL); L(LWR); L(SB); L(SH); L(SW); L(SWL); L(SWR); L(LD0);
        L(MFC0); L(MTC0); L(RFE); L(MFC2); L(CFC2); L(MTC2); L(CTC2); L(LWC2); L(SWC2); L(GTE);
        L(SYS); L(BRK); L(END_MTC0); L(END_SYS); L(END_BRK); L(UNIMPL); L(FALL);
        L(E_BEQ); L(E_BNE); L(E_BLEZ); L(E_BGTZ); L(E_BLTZ); L(E_BGEZ); L(E_BLTZAL); L(E_BGEZAL); L(E_JR);
        L(LINK); L(C_BR); L(C_BRAL); L(C_J); L(C_JAL); L(C_JALN); L(C_JR); L(C_JALR);
        L(F_BEQ); L(F_BNE); L(F_BLEZ); L(F_BGTZ); L(F_BLTZ); L(F_BGEZ); L(F_J); L(F_JAL); L(F_JR); L(F_JALR);
        L(HLE); L(BIOS); L(EXIT);
#undef L
        return;
    }
    uint32_t *const r = c->r;
    const uint32_t ret = r[31];
#ifdef INTERP_TEST
    if (!step_mode)
#endif
    r[31] = SENTINEL;
    Block *b;
    Op *ip;
    int taken = 0;
    uint32_t jt = 0, tg = 0, cont = 0;
    if (first) { b = first; goto enter; }

dispatch:
#ifdef INTERP_TEST
    if (step_mode) { if (step_mode == 2) step_mode = 1; else { step_pc = pc; goto leave; } }
#endif
    {
        uint32_t p = pc & 0x1FFFFFFCu;
        if (__builtin_expect(p < 0x800000u, 1)) {
            b = tab[(p & 0x1FFFFC) >> 2];
            if (__builtin_expect(b != NULL, 1)) goto enter;
        }
        b = lookup_slow(c, pc, ret);
        if (!b) goto leave;
    }
enter:
    wd_ring[wd_pos & (WD_RING - 1)] = b->pc; wd_pos++;
#ifdef INTERP_RING
    ring[ringpos++ & 255] = b->pc;
#endif
    interp_insns += b->ninsn;
    ip = b->ops;
    goto *ip->h;

#define NEXT() do { ip++; goto *ip->h; } while (0)
#define EA() (r[ip->s] + ip->imm)
L_NOP: NEXT();
L_SLL: r[ip->d] = r[ip->t] << ip->sa; NEXT();
L_SRL: r[ip->d] = r[ip->t] >> ip->sa; NEXT();
L_SRA: r[ip->d] = (uint32_t)((int32_t)r[ip->t] >> ip->sa); NEXT();
L_SLLV: r[ip->d] = r[ip->t] << (r[ip->s] & 31); NEXT();
L_SRLV: r[ip->d] = r[ip->t] >> (r[ip->s] & 31); NEXT();
L_SRAV: r[ip->d] = (uint32_t)((int32_t)r[ip->t] >> (r[ip->s] & 31)); NEXT();
L_ADDU: r[ip->d] = r[ip->s] + r[ip->t]; NEXT();
L_SUBU: r[ip->d] = r[ip->s] - r[ip->t]; NEXT();
L_AND: r[ip->d] = r[ip->s] & r[ip->t]; NEXT();
L_OR: r[ip->d] = r[ip->s] | r[ip->t]; NEXT();
L_XOR: r[ip->d] = r[ip->s] ^ r[ip->t]; NEXT();
L_NOR: r[ip->d] = ~(r[ip->s] | r[ip->t]); NEXT();
L_SLT: r[ip->d] = (int32_t)r[ip->s] < (int32_t)r[ip->t]; NEXT();
L_SLTU: r[ip->d] = r[ip->s] < r[ip->t]; NEXT();
L_MFHI: r[ip->d] = c->hi; NEXT();
L_MFLO: r[ip->d] = c->lo; NEXT();
L_MTHI: c->hi = r[ip->s]; NEXT();
L_MTLO: c->lo = r[ip->s]; NEXT();
L_MULT: { int64_t m = (int64_t)(int32_t)r[ip->s] * (int32_t)r[ip->t]; c->lo = (uint32_t)m; c->hi = (uint32_t)(m >> 32); } NEXT();
L_MULTU: { uint64_t m = (uint64_t)r[ip->s] * r[ip->t]; c->lo = (uint32_t)m; c->hi = (uint32_t)(m >> 32); } NEXT();
L_DIV: {
        int32_t n = (int32_t)r[ip->s], d = (int32_t)r[ip->t];
        if (d == 0) { c->hi = (uint32_t)n; c->lo = n >= 0 ? 0xFFFFFFFFu : 1u; }
        else if (n == INT32_MIN && d == -1) { c->hi = 0; c->lo = 0x80000000u; }
        else { c->lo = (uint32_t)(n / d); c->hi = (uint32_t)(n % d); }
    } NEXT();
L_DIVU: {
        uint32_t n = r[ip->s], d = r[ip->t];
        if (d == 0) { c->hi = n; c->lo = 0xFFFFFFFFu; } else { c->lo = n / d; c->hi = n % d; }
    } NEXT();
L_ADDIU: r[ip->t] = r[ip->s] + ip->imm; NEXT();
L_SLTI: r[ip->t] = (int32_t)r[ip->s] < (int32_t)ip->imm; NEXT();
L_SLTIU: r[ip->t] = r[ip->s] < ip->imm; NEXT();
L_ANDI: r[ip->t] = r[ip->s] & ip->imm; NEXT();
L_ORI: r[ip->t] = r[ip->s] | ip->imm; NEXT();
L_XORI: r[ip->t] = r[ip->s] ^ ip->imm; NEXT();
L_LI: r[ip->t] = ip->imm; NEXT();
L_LB: r[ip->t] = (uint32_t)(int32_t)(int8_t)rd8(c, EA()); NEXT();
L_LH: r[ip->t] = (uint32_t)(int32_t)(int16_t)rd16(c, EA()); NEXT();
L_LW: r[ip->t] = rd32(c, EA()); NEXT();
L_LBU: r[ip->t] = rd8(c, EA()); NEXT();
L_LHU: r[ip->t] = rd16(c, EA()); NEXT();
L_LWL: r[ip->t] = mem_lwl(c, EA(), r[ip->t]); NEXT();
L_LWR: r[ip->t] = mem_lwr(c, EA(), r[ip->t]); NEXT();
L_SB: wr8(c, EA(), (uint8_t)r[ip->t]); NEXT();
L_SH: wr16(c, EA(), (uint16_t)r[ip->t]); NEXT();
L_SW: wr32(c, EA(), r[ip->t]); NEXT();
L_SWL: mem_swl(c, EA(), r[ip->t]); NEXT();
L_SWR: mem_swr(c, EA(), r[ip->t]); NEXT();
L_LD0: if (ip->sa == 1) (void)rd8(c, EA()); else if (ip->sa == 2) (void)rd16(c, EA()); else (void)rd32(c, EA()); NEXT();
L_MFC0: r[ip->t] = cop0_read(c, ip->d); NEXT();
L_MTC0: cop0_write(c, ip->d, r[ip->t]); NEXT();
L_RFE: cop0_rfe(c); NEXT();
L_MFC2: { uint32_t v = gte_mfc2(c, ip->d); if (!ip->sa) r[ip->t] = v; } NEXT();
L_CFC2: r[ip->t] = gte_cfc2(c, ip->d); NEXT();
L_MTC2: gte_mtc2(c, ip->d, r[ip->t]); NEXT();
L_CTC2: gte_ctc2(c, ip->d, r[ip->t]); NEXT();
L_LWC2: gte_lwc2(c, ip->t, rd32(c, EA())); NEXT();
L_SWC2: wr32(c, EA(), gte_swc2(c, ip->t)); NEXT();
L_GTE: gte_cmd(c, ip->imm); NEXT();
/* ip puede apuntar a un bloque invalidado y liberado cuando el hilo vuelve de una excepción/conmutación: se copia antes. */
L_END_MTC0: { uint32_t nx = ip->imm; cop0_write(c, ip->d, r[ip->t]); pc = nx; } goto dispatch;
L_END_SYS: { uint32_t at = ip->imm; rt_syscall(c, at); pc = at + 4; } goto dispatch;
L_SYS: rt_syscall(c, ip->imm); NEXT();
L_BRK: rt_break(c, ip->imm); NEXT();
L_END_BRK: rt_break(c, ip->imm); pc = ip->imm + 4; goto dispatch;
L_UNIMPL: rt_unimpl(c, ip[1].imm, ip->imm);
L_FALL: pc = b->end; goto dispatch;

    /* ---- evaluación del salto con el slot después ---- */
L_E_BEQ: taken = r[ip->s] == r[ip->t]; NEXT();
L_E_BNE: taken = r[ip->s] != r[ip->t]; NEXT();
L_E_BLEZ: taken = (int32_t)r[ip->s] <= 0; NEXT();
L_E_BGTZ: taken = (int32_t)r[ip->s] > 0; NEXT();
L_E_BLTZ: taken = (int32_t)r[ip->s] < 0; NEXT();
L_E_BGEZ: taken = (int32_t)r[ip->s] >= 0; NEXT();
L_E_BLTZAL: taken = (int32_t)r[ip->s] < 0; r[31] = b->end; NEXT();
L_E_BGEZAL: taken = (int32_t)r[ip->s] >= 0; r[31] = b->end; NEXT();
L_E_JR: jt = r[ip->s]; NEXT();
L_C_BR: goto do_br;
L_C_BRAL: if (taken) { tg = ip->imm; goto call_common; } pc = b->end; goto dispatch;
L_C_J: goto do_j;
L_C_JAL: goto do_jal;
L_C_JALN: tg = ip->imm; goto call_common;     /* jal con el enlace ya escrito antes del slot */
L_LINK: r[ip->d] = ip->imm; NEXT();
L_C_JR: goto do_jr;
L_C_JALR: goto do_jalr;

    /* ---- salto con el slot ya ejecutado ---- */
L_F_BEQ: taken = r[ip->s] == r[ip->t]; goto do_br;
L_F_BNE: taken = r[ip->s] != r[ip->t]; goto do_br;
L_F_BLEZ: taken = (int32_t)r[ip->s] <= 0; goto do_br;
L_F_BGTZ: taken = (int32_t)r[ip->s] > 0; goto do_br;
L_F_BLTZ: taken = (int32_t)r[ip->s] < 0; goto do_br;
L_F_BGEZ: taken = (int32_t)r[ip->s] >= 0; goto do_br;
L_F_J: goto do_j;
L_F_JAL: goto do_jal;
L_F_JR: jt = r[ip->s]; goto do_jr;
L_F_JALR: jt = r[ip->s]; goto do_jalr;

do_br:
    if (taken) { pc = ip->imm; if (ip->sa) RT_POLL(c); } else pc = b->end;
    goto dispatch;
do_j:
    tg = ip->imm;
    if (ip->sa) RT_POLL(c);
    goto tail_common;
do_jr:
    tg = jt;
    if (!ip->sa) { pc = tg; goto dispatch; }       /* jr ra: retorno, sin sondeo ni búsqueda nativa */
    RT_POLL(c);
    goto tail_common;
do_jal:
    r[31] = b->end; tg = ip->imm;
    goto call_common;
do_jalr:
    tg = jt;
    if (ip->d) r[ip->d] = b->end;
    goto call_common;

call_common:
#ifdef INTERP_TRACE
    rt_trace_enter(c, tg);
#endif
#ifndef MV_NOGEN
    {
        FuncPtr f = rt_lookup(tg);
        if (f) { cont = b->end; f(c); pc = cont; goto dispatch; }
        if (!is_bios_vec(tg) && !hle_bind_is(tg)) note_entry(tg, r[31]);
    }
#endif
    pc = tg;
    goto dispatch;
tail_common:
#ifndef MV_NOGEN
    {
        FuncPtr f = rt_lookup(tg);
        if (f) { f(c); pc = r[31]; goto dispatch; }
    }
#endif
    pc = tg;
    goto dispatch;

L_HLE: ((FuncPtr)ip->ptr)(c); pc = r[31]; goto dispatch;
L_BIOS: bios_hle(c, ip->imm); pc = r[31]; goto dispatch;
L_EXIT: goto leave;

leave:
#ifdef INTERP_TEST
    if (!step_mode)
#endif
    r[31] = ret;
    (void)cont;
    if (first) free(first);
}

static void interp_init(void) {
    static int inited;
    if (inited) return;
    inited = 1;
    run(NULL, 0, NULL);
    tab = calloc(PSX_RAM_SIZE / 4, sizeof *tab);
    if (!tab) abort();
}

void interp_call(Cpu *c, uint32_t entry) {
    interp_init();
#ifdef INTERP_TRACE
    rt_trace_enter(c, entry);   /* las activaciones abiertas desde el host (callbacks, hilos) también aparecen en la traza */
#endif
#ifndef MV_NOGEN
    if (!hle_bind_is(entry) && !is_bios_vec(entry)) note_entry(entry, c->r[31]);
#endif
    run(c, entry, NULL);
}

/* Ejecuta el código original de una función aunque tenga HLE enlazada (el primer bloque no pasa por la caché). */
void interp_call_raw(Cpu *c, uint32_t entry) {
    interp_init();
    Block *b = decode(entry, 1);
    run(c, entry, b);
}

#ifdef INTERP_TEST
/* Prueba diferencial (test_interp_diff.c): ejecuta SOLO el bloque de `pc` y devuelve el siguiente pc (el destino
 * del salto, sin seguir llamadas ni retornos). r31 no se toca fuera de lo que haga el propio bloque. */
uint32_t interp_step(Cpu *c, uint32_t pc) {
    interp_init();
    uint32_t keep = c->r[31];
    step_mode = 2;
    run(c, pc, NULL);   /* run guarda/restaura r31: se repone el valor del bloque */
    step_mode = 0;
    (void)keep;
    return step_pc;
}
#endif
