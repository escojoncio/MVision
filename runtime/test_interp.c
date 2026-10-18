/* Pruebas del intérprete MIPS de respaldo (interp.c) y de setjmp/longjmp (rt.c + hle.c).
 * Sin datos del juego: los programas se ensamblan a mano en RAM. */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "cpu.h"

void interp_call(Cpu *c, uint32_t entry);
extern int interp_log_enabled;
void hle_setjmp(Cpu *c);
void hle_longjmp(Cpu *c);
jmp_buf *rt_sj_slot(Cpu *c, uint32_t buf);
void rt_sj_resumed(Cpu *c);

/* Función "recompilada" de prueba en 0x80030000: v0 = a0 * 3. */
static void nat_triple(Cpu *c) { c->r[2] = c->r[4] * 3; }
/* Simula el código generado de una función que llama a setjmp(a0) y luego a 0x80031000. */
static int sj_hits;
static void nat_sj(Cpu *c) {
    uint32_t buf = c->r[4];
    if (setjmp(*rt_sj_slot(c, buf))) rt_sj_resumed(c); else hle_setjmp(c);
    sj_hits++;
    if (c->r[2] == 0) { c->r[4] = buf; c->r[5] = 7; hle_longjmp(c); }
}
const FuncEntry rt_func_table[] = {{0x80030000u, nat_triple}, {0x80030100u, nat_sj}};
const unsigned rt_func_count = 2;
const FuncName rt_func_names[] = {{0xFFFFFFFFu, ""}};
const unsigned rt_func_name_count = 0;
const uint32_t rt_entry = 0;

static int fails;
#define EQ(a, b) do { unsigned long long _a = (a), _b = (b); \
    if (_a != _b) { printf("FALLO %s:%d %s = 0x%llx, esperado 0x%llx\n", __FILE__, __LINE__, #a, _a, _b); fails++; } } while (0)

/* ---- mini ensamblador ---- */
static uint32_t pc_asm;
static void emit(uint32_t w) { mem_w32(NULL, pc_asm, w); pc_asm += 4; }
#define R_(op, s, t, d, sh, f) (((op) << 26) | ((s) << 21) | ((t) << 16) | ((d) << 11) | ((sh) << 6) | (f))
#define I_(op, s, t, i) (((op) << 26) | ((s) << 21) | ((t) << 16) | ((i) & 0xFFFF))
#define J_(op, a) (((op) << 26) | (((a) >> 2) & 0x3FFFFFF))
#define NOP 0
#define JR_RA R_(0, 31, 0, 0, 0, 8)
#define ADDIU(t, s, i) I_(9, s, t, i)
#define ADDU(d, s, t) R_(0, s, t, d, 0, 33)
#define LUI(t, i) I_(15, 0, t, i)
#define ORI(t, s, i) I_(13, s, t, i)
#define BNE(s, t, off) I_(5, s, t, off)
#define BEQ(s, t, off) I_(4, s, t, off)
#define SW(t, s, i) I_(43, s, t, i)
#define LW(t, s, i) I_(35, s, t, i)
#define JAL(a) J_(3, a)
#define J(a) J_(2, a)
#define JALR(d, s) R_(0, s, 0, d, 0, 9)
#define JR(s) R_(0, s, 0, 0, 0, 8)
enum { V0 = 2, V1 = 3, A0 = 4, A1 = 5, T0 = 8, T1 = 9, T2 = 10, S0 = 16, SP = 29, RA = 31 };

static void reset(Cpu *c) {
    memset(c, 0, sizeof *c);
    c->r[SP] = 0x801FFF00u;
    c->r[RA] = 0xDEAD0000u;   /* ra de entrada: el intérprete vuelve al saltar aquí */
}

/* Bucle con delay slots: suma 1..a0 en v0. */
static void t_loop(Cpu *c) {
    reset(c);
    pc_asm = 0x80020000;
    emit(ADDU(V0, 0, 0));          /* v0 = 0 */
    emit(ADDU(V0, V0, A0));        /* L: v0 += a0 */
    emit(ADDIU(A0, A0, -1));
    emit(BNE(A0, 0, -3));          /* -> L */
    emit(ADDIU(T0, T0, 1));        /* delay slot: cuenta iteraciones */
    emit(JR_RA);
    emit(ORI(V1, 0, 0x55));        /* delay slot del retorno */
    c->r[A0] = 10;
    interp_call(c, 0x80020000);
    EQ(c->r[V0], 55);
    EQ(c->r[T0], 10);
    EQ(c->r[V1], 0x55);
}

/* jal a función nativa + jal a función interpretada (recursión) + jalr + guardado de ra en pila. */
static void t_calls(Cpu *c) {
    reset(c);
    /* g(a0) = a0 + 1, interpretada */
    pc_asm = 0x80021000;
    emit(JR_RA);
    emit(ADDIU(V0, A0, 1));
    /* f(a0) = triple(g(a0)) + 100 vía jalr */
    pc_asm = 0x80020100;
    emit(ADDIU(SP, SP, -8));
    emit(SW(RA, SP, 4));
    emit(JAL(0x80021000));
    emit(NOP);
    emit(LUI(T1, 0x8003));         /* t1 = 0x80030000 (nativa) */
    emit(JALR(RA, T1));
    emit(ADDU(A0, V0, 0));         /* delay: a0 = g(a0) */
    emit(LW(RA, SP, 4));
    emit(ADDIU(SP, SP, 8));
    emit(JR_RA);
    emit(ADDIU(V0, V0, 100));
    c->r[A0] = 4;
    interp_call(c, 0x80020100);
    EQ(c->r[V0], (4 + 1) * 3 + 100);
    EQ(c->r[RA], 0xDEAD0000u);
    EQ(c->r[SP], 0x801FFF00u);
}

/* j (salto en cola) a una función nativa: ejecuta y retorna al llamante del intérprete. */
static void t_tail(Cpu *c) {
    reset(c);
    pc_asm = 0x80020200;
    emit(J(0x80030000));
    emit(ADDIU(A0, 0, 7));
    interp_call(c, 0x80020200);
    EQ(c->r[V0], 21);
}

/* Tabla de saltos interna (jr a registro distinto de ra) + bltzal/bgezal + beq tomado. */
static void t_jrtable(Cpu *c) {
    reset(c);
    pc_asm = 0x80020300;
    emit(LUI(T0, 0x8002));
    emit(ORI(T0, T0, 0x0318));     /* destino interno */
    emit(JR(T0));
    emit(ADDIU(V0, 0, 1));
    emit(ADDIU(V0, 0, 2));         /* 0x80020310: no se ejecuta */
    emit(ADDIU(V0, 0, 3));
    emit(BEQ(0, 0, 2));            /* 0x80020318: tomado -> 0x80020324 */
    emit(ADDIU(V1, V0, 10));       /* delay: v1 = 11 */
    emit(ADDIU(V1, 0, 0));         /* saltado */
    /* bgezal $zero: llamada siempre tomada a g (0x80021000), ra = pc+8 */
    emit(I_(1, 0, 17, (0x80021000 - 0x80020328) >> 2));
    emit(ADDIU(A0, 0, 41));        /* delay */
    emit(LW(RA, SP, -4));          /* restaura ra guardado abajo */
    emit(JR_RA);
    emit(NOP);
    mem_w32(c, 0x801FFF00u - 4, 0xDEAD0000u);
    interp_call(c, 0x80020300);
    EQ(c->r[V1], 11);
    EQ(c->r[V0], 42);
}

/* Aritmética variada: lwl/lwr no alineado, mult/div, slt, sra, lb con signo, sh. */
static void t_alu(Cpu *c) {
    reset(c);
    mem_w32(c, 0x80040000, 0x44332211);
    mem_w32(c, 0x80040004, 0x88776655);
    pc_asm = 0x80020400;
    emit(LUI(T0, 0x8004));
    emit(I_(38, T0, T1, 1));       /* lwr t1, 1(t0) */
    emit(I_(34, T0, T1, 4));       /* lwl t1, 4(t0) */
    emit(ADDIU(T2, 0, -7));
    emit(ADDIU(A1, 0, 2));
    emit(R_(0, T2, A1, 0, 0, 26)); /* div t2, a1 */
    emit(R_(0, 0, 0, V0, 0, 18));  /* mflo v0 */
    emit(R_(0, 0, 0, V1, 0, 16));  /* mfhi v1 */
    emit(R_(0, T2, A1, 0, 0, 24)); /* mult -> lo = -14 */
    emit(R_(0, 0, 0, A0, 0, 18));
    emit(R_(0, T2, 0, S0, 0, 42)); /* slt s0, t2, zero = 1 */
    emit(R_(0, 0, T2, 11, 1, 3));  /* sra t3, t2, 1 = -4 */
    emit(I_(32, T0, 12, 7));       /* lb t4, 7(t0) = 0x88 con signo */
    emit(JR_RA);
    emit(I_(41, T0, T1, 8));       /* delay: sh t1, 8(t0) */
    interp_call(c, 0x80020400);
    EQ(c->r[T1], 0x55443322);
    EQ(c->r[V0], (uint32_t)-3);
    EQ(c->r[V1], (uint32_t)-1);
    EQ(c->r[A0], (uint32_t)-14);
    EQ(c->r[S0], 1);
    EQ(c->r[11], (uint32_t)-4);
    EQ(c->r[12], 0xFFFFFF88u);
    EQ(mem_r16(c, 0x80040008), 0x3322);
    EQ(c->r[0], 0);
}

/* setjmp/longjmp: el longjmp vuelve al marco host de setjmp con v0 = 7 y registros restaurados. */
static void t_setjmp(Cpu *c) {
    reset(c);
    c->r[A0] = 0x80050000;
    c->r[S0] = 0x1234;
    c->r[RA] = 0xCAFE0000u;
    nat_sj(c);
    EQ(sj_hits, 2);
    EQ(c->r[V0], 7);
    EQ(c->r[S0], 0x1234);
    EQ(c->r[RA], 0xCAFE0000u);
    EQ(mem_r32(c, 0x80050000), 0xCAFE0000u);
}


/* El enlace de jal/jalr se escribe en el propio salto: el delay slot lo ve; si el slot escribe ra, gana el slot. */
static void t_link_slot(Cpu *c) {
    reset(c);
    pc_asm = 0x80021000; emit(JR_RA); emit(NOP);                 /* g */
    pc_asm = 0x80002000; emit(ADDIU(V1, 0, 99)); emit(JR(S0)); emit(NOP);
    pc_asm = 0x80020000;
    emit(ADDU(S0, RA, 0));
    emit(JAL(0x80021000));
    emit(ADDU(T0, RA, 0));                                       /* slot lee ra = 0x8002000c */
    emit(LUI(T2, 0x8002)); emit(ORI(T2, T2, 0x1000));
    emit(JALR(RA, T2));
    emit(ADDU(A1, RA, 0));                                       /* slot lee rd = 0x8002001c */
    emit(JAL(0x80021000));
    emit(ORI(RA, 0, 0x2000));                                    /* slot escribe ra: g vuelve a 0x2000 */
    emit(JR(S0)); emit(NOP);
    interp_call(c, 0x80020000);
    EQ(c->r[T0], 0x8002000cu);
    EQ(c->r[A1], 0x8002001cu);
    EQ(c->r[V1], 99);
}

/* Invalidación de código decodificado: escrituras del invitado y del runtime, delay slot, bloque que cruza granos. */
static void t_codewrite(Cpu *c) {
    reset(c);
    pc_asm = 0x80030000; emit(ADDIU(V0, 0, 1)); emit(JR_RA); emit(NOP);
    interp_call(c, 0x80030000); EQ(c->r[V0], 1);
    pc_asm = 0x80030100; emit(LUI(T0, 0x8003)); emit(LUI(T1, 0x2402)); emit(ORI(T1, T1, 2)); emit(SW(T1, T0, 0)); emit(JR_RA); emit(NOP);
    interp_call(c, 0x80030100);                                  /* sw del invitado sobre su propio código */
    interp_call(c, 0x80030000); EQ(c->r[V0], 2);
    mem_w8(c, 0x80030000, 3); interp_call(c, 0x80030000); EQ(c->r[V0], 3);
    mem_w16(c, 0xA0030000, 4); interp_call(c, 0x80030000); EQ(c->r[V0], 4);
    uint32_t w = ADDIU(V0, 0, 5); memcpy(&psx_ram[0x30000], &w, 4);
    rt_code_invalidate(0x80030000, 4); interp_call(c, 0x80030000); EQ(c->r[V0], 5);
    pc_asm = 0x80030200; emit(JR_RA); emit(ADDIU(V1, 0, 1));    /* delay slot */
    interp_call(c, 0x80030200); EQ(c->r[V1], 1);
    mem_w32(c, 0x80030204, ADDIU(V1, 0, 2)); interp_call(c, 0x80030200); EQ(c->r[V1], 2);
    pc_asm = 0x80040000 - 32;                                    /* bloque que cruza el límite de grano 0x80040000 */
    for (int i = 0; i < 8; i++) emit(NOP);
    emit(ADDIU(V1, 0, 1)); emit(JR_RA); emit(NOP);
    interp_call(c, 0x80040000 - 32); EQ(c->r[V1], 1);
    mem_w32(c, 0x80040000, ADDIU(V1, 0, 3)); interp_call(c, 0x80040000 - 32); EQ(c->r[V1], 3);
}

/* DMA a RAM (OTC): invalida el bloque decodificado en la zona escrita. */
extern unsigned long interp_invalidations;
static void t_dma_inval(Cpu *c) {
    reset(c);
    pc_asm = 0x80090000; emit(ADDIU(V0, 0, 1)); emit(JR_RA); emit(NOP);
    interp_call(c, 0x80090000); EQ(c->r[V0], 1);
    unsigned long before = interp_invalidations;
    mem_w32(c, 0x1F8010E0, 0x00090008u); mem_w32(c, 0x1F8010E4, 4); mem_w32(c, 0x1F8010E8, 0x11000002u);
    EQ(interp_invalidations - before, 1);
}

int main(void) {
    Cpu c;
    interp_log_enabled = 0;
    t_loop(&c);
    t_calls(&c);
    t_tail(&c);
    t_jrtable(&c);
    t_alu(&c);
    t_setjmp(&c);
    t_link_slot(&c);
    t_codewrite(&c);
    t_dma_inval(&c);
    printf(fails ? "%d fallos\n" : "intérprete: todas las pruebas OK\n", fails);
    return fails != 0;
}
