/* Pruebas de la procedencia de las primitivas (prov.h/prov.c, rt.c, mem.c, interp.c, fiber.c, gpu.c). Sin datos del juego: programas MIPS ensamblados a mano.
 * Normal (hay tabla nativa de prueba) y MV_NOGEN (todo en el intérprete plano; los casos con función nativa llamada por jalr se omiten).
 * `test_prov bench` mide el sobrecoste (apagado / encendido) de escrituras, llamadas indirectas nativas y del intérprete. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cpu.h"
#include "fiber.h"
#include "gpu.h"
#include "prov.h"

#define NAT_A     0x80030000u
#define NAT_B     0x80030010u
#define NAT_SJ    0x80030020u
#define NAT_LJ    0x80030030u
#define NAT_FMAIN 0x80030040u
#define NAT_FIB   0x80030050u
#define NAT_LEAF  0x80030060u
#define NAT_ACT   0x80030080u
#define NAT_ACT2  0x80030090u

#define WA 0x80100000u
#define WB 0x80100010u
#define WC 0x80100020u
#define WS 0x80100030u
#define WF 0x80100040u
#define WF2 0x80100050u
#define WM 0x80100060u

static int fails;
#define EQ(a, b) do { unsigned long long _a = (a), _b = (b); \
    if (_a != _b) { printf("FALLO %s:%d %s = 0x%llx, esperado 0x%llx\n", __FILE__, __LINE__, #a, _a, _b); fails++; } } while (0)
#define OK(c) do { if (!(c)) { printf("FALLO %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static Cpu C;
static uint16_t tag(uint32_t a) { return rt_prov_mem[(a & 0x1FFFFCu) >> 2]; }

/* ---- mini ensamblador (como test_interp.c) ---- */
static uint32_t pc_asm;
static void emit(uint32_t w) { mem_w32(&C, pc_asm, w); pc_asm += 4; }
#define R_(op, s, t, d, sh, f) (((op) << 26) | ((s) << 21) | ((t) << 16) | ((d) << 11) | ((sh) << 6) | (f))
#define I_(op, s, t, i) (((op) << 26) | ((s) << 21) | ((t) << 16) | ((i) & 0xFFFF))
#define NOP 0
#define JR_RA R_(0, 31, 0, 0, 0, 8)
#define ADDIU(t, s, i) I_(9, s, t, i)
#define LUI(t, i) I_(15, 0, t, i)
#define ORI(t, s, i) I_(13, s, t, i)
#define BNE(s, t, off) I_(5, s, t, off)
#define SW(t, s, i) I_(43, s, t, i)
#define LW(t, s, i) I_(35, s, t, i)
#define JALR(d, s) R_(0, s, 0, d, 0, 9)
enum { V0 = 2, A0 = 4, A1 = 5, A2 = 6, T9 = 25, S0 = 16, SP = 29, RA = 31 };

/* ---- tablas: sin datos del juego (en MV_NOGEN las pone nogen.c) ---- */
#ifndef MV_NOGEN
const FuncEntry rt_func_table[] = {{0, 0}};
const unsigned rt_func_count = 0;
const FuncName rt_func_names[] = {{0xFFFFFFFFu, ""}};
const unsigned rt_func_name_count = 0;
const uint32_t rt_entry = 0;
#endif

extern int interp_log_enabled;   /* sin anotar las entradas interpretadas en missing_funcs.txt */
void hle_jmp_save(Cpu *c);
void hle_jmp_restore(Cpu *c);
jmp_buf *rt_sj_slot(Cpu *c, uint32_t buf);
void rt_sj_resumed(Cpu *c);

/* ---- funciones nativas de prueba ---- */
static uint16_t a_scope, b_scope;
static void nat_b(Cpu *c) { b_scope = rt_prov_cur; mem_w32(c, WB, 0xB); }
static void nat_a(Cpu *c) {
    a_scope = rt_prov_cur;
    mem_w32(c, WA, 0xA);
    rt_call(c, NAT_B);
    mem_w32(c, WC, 0xC);       /* tras volver de B: otra vez el ámbito de A */
}
static int sj_hits; static uint16_t sj_scope[4], lj_scope;
static void nat_lj(Cpu *c) { lj_scope = rt_prov_cur; hle_jmp_restore(c); }
static void nat_sj(Cpu *c) {
    uint32_t buf = c->r[4];
    if (setjmp(*rt_sj_slot(c, buf))) rt_sj_resumed(c); else hle_jmp_save(c);
    sj_scope[sj_hits++] = rt_prov_cur;
    mem_w32(c, WS, (uint32_t)sj_hits);
    if (c->r[2] == 0) { c->r[4] = buf; c->r[5] = 7; rt_call(c, NAT_LJ); }
}
static uint16_t fm_scope, fm_after1, fm_after2, f_scope, f_scope2;
static void nat_fib(Cpu *c) {
    f_scope = rt_prov_cur;
    mem_w32(c, WF, 1);
    fiber_change(c, 0xFF000000u);
    f_scope2 = rt_prov_cur;
    mem_w32(c, WF2, 2);
    for (;;) fiber_change(c, 0xFF000000u);
}
static void nat_fmain(Cpu *c) {
    fm_scope = rt_prov_cur;
    uint32_t h = fiber_open(c, NAT_FIB, 0x801FE000u, 0x80100000u);
    fiber_change(c, h);
    fm_after1 = rt_prov_cur;
    mem_w32(c, WM, 1);
    fiber_change(c, h);
    fm_after2 = rt_prov_cur;
}
static void nat_leaf(Cpu *c) { mem_w32(c, c->r[4] + 16, 0x1EAF); }
/* "Actor" de dibujo: escribe el paquete de a1 (rectángulo monocromo) y lo encadena a a2 (como AddPrim). */
static void nat_act(Cpu *c) {
    uint32_t p = c->r[5];
    mem_w32(c, p + 4, 0x6000FF00u); mem_w32(c, p + 8, 0x00100010u); mem_w32(c, p + 12, 0x00080008u);
    mem_w32(c, p, (3u << 24) | (c->r[6] & 0xFFFFFFu));
}
static const FuncEntry nat_table[] = {{NAT_A, nat_a}, {NAT_B, nat_b}, {NAT_SJ, nat_sj}, {NAT_LJ, nat_lj}, {NAT_FMAIN, nat_fmain}, {NAT_FIB, nat_fib},
                                      {NAT_LEAF, nat_leaf}, {NAT_ACT, nat_act}, {NAT_ACT2, nat_act}};

static void reset(void) {
    memset(&C, 0, sizeof C);
    C.r[SP] = 0x801FFF00u;
    C.r[RA] = 0xDEAD0000u;
}
static void put_str(uint32_t a, const char *s) { for (size_t i = 0; i <= strlen(s); i++) mem_w8(&C, a + (uint32_t)i, (uint8_t)s[i]); }
static void put_actor(uint32_t node, uint32_t fn, uint32_t name_addr) { mem_w32(&C, node + 8, fn); mem_w32(&C, node + 20, name_addr); }
static void stub_leaf(uint32_t a) { pc_asm = a; emit(SW(A0, A0, 16)); emit(JR_RA); emit(NOP); }   /* sw a0, 16(a0); jr ra */

/* ámbitos anidados, restauración y escrituras sin ámbito */
static void t_scope(void) {
    rt_prov_reset(); rt_prov_set(0); reset();
    rt_call(&C, NAT_A);
    EQ(tag(WA), 0); EQ(tag(WB), 0); EQ(tag(WC), 0);   /* apagado: nada se anota */
    EQ(rt_prov_n, 0);
    rt_prov_set(1);
    mem_w32(&C, WS, 5); EQ(tag(WS), 0);                 /* escritura sin ámbito = 0 */
    mem_w8(&C, WS + 1, 5); mem_w16(&C, WS + 2, 5); EQ(tag(WS), 0);
    rt_call(&C, NAT_A);
    OK(a_scope != 0 && b_scope != 0 && a_scope != b_scope);
    EQ(tag(WA), a_scope); EQ(tag(WC), a_scope); EQ(tag(WB), b_scope);
    EQ(rt_prov_cur, 0);                                 /* restaurado al salir */
    mem_w32(&C, WA, 1); EQ(tag(WA), 0);                 /* escribir fuera de todo ámbito borra la procedencia anterior */
    EQ(rt_prov_tab[a_scope].fn, NAT_A); EQ(rt_prov_tab[b_scope].fn, NAT_B); EQ(rt_prov_tab[a_scope].actor_node, 0);
    unsigned n = rt_prov_n;
    rt_call(&C, NAT_A);
    EQ(rt_prov_n, n);                                   /* internado: mismo id */
    mem_w16(&C, WS + 2, 5); EQ(tag(WS), 0);
    rt_prov_set(0);
    OK(rt_prov_cur == 0);
}

/* setjmp/longjmp del juego (rt_sj_*): el ámbito vuelve al de la función que hizo setjmp */
static void t_longjmp(void) {
    rt_prov_reset(); rt_prov_set(1); reset(); sj_hits = 0;
    C.r[4] = 0x80100200u;
    rt_call(&C, NAT_SJ);
    EQ(sj_hits, 2);
    OK(sj_scope[0] != 0);
    EQ(sj_scope[1], sj_scope[0]);                       /* tras el longjmp: el de NAT_SJ, no el de NAT_LJ (que se saltó) */
    OK(lj_scope != 0 && lj_scope != sj_scope[0]);
    EQ(tag(WS), sj_scope[0]);
    EQ(rt_prov_cur, 0);
    rt_prov_set(0);
}

/* hilos: cada fibra conserva su ámbito al conmutar */
static void t_fiber(void) {
    rt_prov_reset(); rt_prov_set(1); reset();
    rt_call(&C, NAT_FMAIN);
    OK(fm_scope != 0 && f_scope != 0 && f_scope != fm_scope);
    EQ(rt_prov_tab[f_scope].fn, NAT_FIB); EQ(rt_prov_tab[fm_scope].fn, NAT_FMAIN);
    EQ(tag(WF), f_scope);
    EQ(fm_after1, fm_scope); EQ(tag(WM), fm_scope);     /* de vuelta en el hilo principal: su ámbito, no el de la fibra */
    EQ(f_scope2, f_scope); EQ(tag(WF2), f_scope);       /* y la fibra reanuda en el suyo */
    EQ(fm_after2, fm_scope);
    EQ(rt_prov_cur, 0);
    rt_prov_set(0);
}

/* intérprete plano: jalr a código MIPS (y a una nativa); el ámbito se cierra en el jr que vuelve */
static void t_flat(void) {
    rt_prov_reset(); rt_prov_set(1); reset();
    stub_leaf(0x80021000u);
    pc_asm = 0x80020000u;
    emit(ADDIU(SP, SP, -8)); emit(SW(RA, SP, 0));
    emit(LUI(T9, 0x8002)); emit(ORI(T9, T9, 0x1000));
    emit(JALR(RA, T9)); emit(NOP);
    emit(SW(A1, A2, 0));                                 /* tras volver: fuera del ámbito */
    emit(LW(RA, SP, 0)); emit(ADDIU(SP, SP, 8));
    emit(JR_RA); emit(NOP);
    put_actor(0x80110000u, 0x80021000u, 0x80111000u); put_str(0x80111000u, "scope.c");
    C.r[A0] = 0x80110000u; C.r[A1] = 0x77; C.r[A2] = 0x80100300u;
    interp_call(&C, 0x80020000u);
    uint16_t id = tag(0x80110000u + 16);
    OK(id != 0);
    EQ(rt_prov_tab[id].fn, 0x80021000u); EQ(rt_prov_tab[id].actor_node, 0x80110000u); OK(!strcmp(rt_prov_tab[id].name, "scope.c"));
    EQ(tag(0x80100300u), 0);                             /* escrito tras el retorno */
    EQ(rt_prov_cur, 0);
    /* sin nodo de actor (a0 no apunta a un actor de esa función) */
    C.r[A0] = 0x80110100u; interp_call(&C, 0x80020000u);
    id = tag(0x80110100u + 16);
    OK(id != 0 && rt_prov_tab[id].actor_node == 0 && rt_prov_tab[id].name[0] == 0);
    EQ(rt_prov_cur, 0);
#ifndef MV_NOGEN
    /* jalr a una función nativa (tabla): ámbito con C local */
    pc_asm = 0x80020100u;
    emit(ADDIU(SP, SP, -8)); emit(SW(RA, SP, 0));
    emit(LUI(T9, 0x8003)); emit(ORI(T9, T9, 0x0060));
    emit(JALR(RA, T9)); emit(NOP);
    emit(SW(A1, A2, 0));
    emit(LW(RA, SP, 0)); emit(ADDIU(SP, SP, 8));
    emit(JR_RA); emit(NOP);
    C.r[A0] = 0x80110200u; C.r[A1] = 0x77; C.r[A2] = 0x80100310u;
    interp_call(&C, 0x80020100u);
    id = tag(0x80110200u + 16);
    OK(id != 0); EQ(rt_prov_tab[id].fn, NAT_LEAF);
    EQ(tag(0x80100310u), 0); EQ(rt_prov_cur, 0);
#endif
    rt_prov_set(0);
}

/* pila sombra del intérprete: pop exacto, unwind (longjmp), desbordamiento */
static void t_stack(void) {
    rt_prov_reset(); rt_prov_set(1);
    rt_prov_push(0x80021000u, 0, 0x80020010u, 0x1000u); uint16_t ia = rt_prov_cur;
    rt_prov_push(0x80021100u, 0, 0x80020020u, 0x0F00u); uint16_t ib = rt_prov_cur;
    OK(ia != 0 && ib != 0 && ia != ib);
    rt_prov_pop(0x0F00u, 0x80020099u); EQ(rt_prov_cur, ib);   /* otra dirección: no cierra */
    rt_prov_pop(0x0E00u, 0x80020020u); EQ(rt_prov_cur, ib);   /* otro sp: no cierra */
    rt_prov_pop(0x0F00u, 0x80020020u); EQ(rt_prov_cur, ia);
    rt_prov_push(0x80021100u, 0, 0x80020020u, 0x0F00u); EQ(rt_prov_cur, ib);
    rt_prov_unwind(0x0F00u); EQ(rt_prov_cur, ia);               /* longjmp a un marco de sp 0xF00: la llamada de ib queda abandonada */
    rt_prov_unwind(0x1000u); EQ(rt_prov_cur, 0);
    /* cierra una llamada con otras (abandonadas) encima */
    rt_prov_push(0x80021000u, 0, 0x80020010u, 0x1000u);
    rt_prov_push(0x80021100u, 0, 0x80020020u, 0x0F00u);
    rt_prov_pop(0x1000u, 0x80020010u); EQ(rt_prov_cur, 0);
    /* desborda */
    for (unsigned i = 0; i < PROV_STK + 3; i++) rt_prov_push(0x80022000u + 4 * i, 0, 0x80020000u + 4 * i, 0x2000u - 8 * i);
    EQ(rt_prov_ovf, 3);
    rt_prov_set(0);
}

/* 8 actores sintéticos: un id por (función, nodo), nombre del nodo, estables al repetir */
static void t_actors(void) {
    rt_prov_reset(); rt_prov_set(1); reset();
    uint16_t ids[8];
    for (int i = 0; i < 8; i++) {
        uint32_t fn = 0x80021200u + 0x10u * (uint32_t)i, node = 0x80110400u + 0x40u * (uint32_t)i, nm = 0x80111100u + 0x10u * (uint32_t)i;
        stub_leaf(fn);
        char s[16]; snprintf(s, sizeof s, "act%d.c", i);
        put_str(nm, s); put_actor(node, fn, nm);
    }
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < 8; i++) {
            uint32_t fn = 0x80021200u + 0x10u * (uint32_t)i, node = 0x80110400u + 0x40u * (uint32_t)i;
            C.r[A0] = node; rt_call(&C, fn);
            ids[i] = tag(node + 16);
        }
    EQ(rt_prov_n, 8);
    for (int i = 0; i < 8; i++) {
        uint32_t fn = 0x80021200u + 0x10u * (uint32_t)i, node = 0x80110400u + 0x40u * (uint32_t)i;
        char s[16]; snprintf(s, sizeof s, "act%d.c", i);
        OK(ids[i] != 0);
        for (int j = 0; j < i; j++) OK(ids[i] != ids[j]);
        EQ(rt_prov_tab[ids[i]].fn, fn); EQ(rt_prov_tab[ids[i]].actor_node, node); OK(!strcmp(rt_prov_tab[ids[i]].name, s));
        EQ(rt_prov_tab[ids[i]].id, ids[i]);
    }
    /* nombre inválido (no imprimible): sin nombre, pero el actor se reconoce */
    uint32_t fn = 0x80021300u, node = 0x80110800u; stub_leaf(fn); mem_w8(&C, 0x80111200u, 0x01); mem_w8(&C, 0x80111201u, 0); put_actor(node, fn, 0x80111200u);
    C.r[A0] = node; rt_call(&C, fn);
    uint16_t id = tag(node + 16); OK(id != 0 && rt_prov_tab[id].actor_node == node && rt_prov_tab[id].name[0] == 0);
    rt_prov_set(0);
}

/* tabla llena: id 0 y contado */
static void t_full(void) {
    rt_prov_reset(); rt_prov_set(1);
    for (unsigned i = 0; i < 1100; i++) { uint16_t old = rt_prov_enter(0x80200000u + 4 * i, 0); rt_prov_leave(old); }
    EQ(rt_prov_n, PROV_MAX - 1); EQ(rt_prov_full, 1100 - (PROV_MAX - 1));
    uint16_t old = rt_prov_enter(0x80200000u + 4 * 1099, 0);
    EQ(rt_prov_cur, 0);                                       /* sin id: 0 */
    mem_w32(&C, WS, 1); EQ(tag(WS), 0);
    rt_prov_leave(old);
    old = rt_prov_enter(0x80200000u, 0); OK(rt_prov_cur == 1); rt_prov_leave(old);   /* los internados siguen valiendo */
    rt_prov_set(0);
}

/* gpu_draw_list: el paquete encadenado dentro de rt_call(act) lleva su procedencia y su dirección */
static void t_gpu(void) {
    rt_prov_reset(); rt_prov_set(1); reset(); gpu_reset();
    gpu_capture_enable(1);
    gpu_capture_set_prov(rt_prov_mem, rt_prov_tab, &rt_prov_n);
    gpu_gp0(0xE3000000u); gpu_gp0(0xE4000000u | 1023u | (511u << 10)); gpu_gp0(0xE5000000u);
    const uint32_t OT = 0x80130000u, P1 = 0x80130100u, P2 = 0x80130200u, P3 = 0x80130300u, N1 = 0x80120000u, N2 = 0x80120040u;
    put_actor(N1, NAT_ACT, 0x80121000u); put_str(0x80121000u, "act.c");
    put_actor(N2, NAT_ACT2, 0x80121010u); put_str(0x80121010u, "act2.c");
    mem_w32(&C, OT, P1 & 0xFFFFFFu);                           /* cabeza de la OT (sin ámbito) */
    C.r[A0] = N1; C.r[A1] = P1; C.r[A2] = P2; rt_call(&C, NAT_ACT);
    uint16_t id1 = tag(P1);
    mem_w32(&C, P2 + 4, 0x6000FF00u); mem_w32(&C, P2 + 8, 0x00200010u); mem_w32(&C, P2 + 12, 0x00080008u);
    mem_w32(&C, P2, (3u << 24) | (P3 & 0xFFFFFFu));          /* paquete escrito sin ámbito */
    C.r[A0] = N2; C.r[A1] = P3; C.r[A2] = 0x00FFFFFFu; rt_call(&C, NAT_ACT2);
    uint16_t id2 = tag(P3);
    OK(id1 != 0 && id2 != 0 && id1 != id2); EQ(tag(P2), 0);
    gpu_draw_list(OT);
    gpu_gp1(0x05000000u | (1u << 10));                         /* cambio de dirección de display: cierra la lista */
    const GpuDrawList *L = gpu_capture_frame();
    OK(L != NULL);
    if (L) {
        EQ(L->nops, 3);
        if (L->nops == 3) {
            EQ(L->ops[0].type, GPU_OP_RECT); EQ(L->ops[0].prov, id1); EQ(L->ops[0].prov2, id1); EQ(L->ops[0].pkt_addr, P1);
            EQ(L->ops[1].prov, 0); EQ(L->ops[1].prov2, 0); EQ(L->ops[1].pkt_addr, P2);
            EQ(L->ops[2].prov, id2); EQ(L->ops[2].prov2, id2); EQ(L->ops[2].pkt_addr, P3);
        }
        OK(L->prov != NULL);
        EQ(L->nprov, rt_prov_n + 1);
        if (L->prov && L->nprov > id2) {
            EQ(L->prov[id1].fn, NAT_ACT); EQ(L->prov[id1].actor_node, N1); OK(!strcmp(L->prov[id1].name, "act.c"));
            EQ(L->prov[id2].fn, NAT_ACT2); OK(!strcmp(L->prov[id2].name, "act2.c"));
        }
    }
    /* sin enganche de procedencia: prov = 0 pero pkt_addr se conserva */
    gpu_capture_set_prov(NULL, NULL, NULL);
    gpu_draw_list(OT);
    gpu_gp1(0x05000000u | (2u << 10));
    L = gpu_capture_frame();
    OK(L != NULL);
    if (L && L->nops == 3) { EQ(L->ops[0].prov, 0); EQ(L->ops[0].pkt_addr, P1); EQ(L->nprov, 0); OK(L->prov == NULL); }
    /* los comandos GP0 directos (no de una lista) no traen paquete */
    gpu_gp0(0x6000FF00u); gpu_gp0(0x00100010u); gpu_gp0(0x00080008u);
    gpu_gp1(0x05000000u | (3u << 10));
    L = gpu_capture_frame();
    OK(L != NULL);
    if (L && L->nops == 1) { EQ(L->ops[0].pkt_addr, 0); EQ(L->ops[0].prov, 0); }
    gpu_capture_enable(0);
    rt_prov_set(0);
}

/* ---- sobrecoste ---- */
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec; }
static void bench(void) {
    enum { NW = 40000000, NC = 5000000, NI = 3000000 };
    reset();
    /* a) mem_w32 (código recompilado / HLE) */
    double w[2];
    for (int on = 0; on < 2; on++) {
        rt_prov_set(on);
        double t0 = now_s();
        for (uint32_t i = 0; i < NW; i++) mem_w32(&C, 0x80100000u + ((i * 4u) & 0xFFFFCu), i);
        w[on] = (now_s() - t0) * 1e9 / NW;
    }
    /* b) rt_call a una función nativa */
    double cl[2];
    for (int on = 0; on < 2; on++) {
        rt_prov_set(on);
        double t0 = now_s();
        for (int i = 0; i < NC; i++) { C.r[4] = 0x80110000u; rt_call(&C, NAT_LEAF); }
        cl[on] = (now_s() - t0) * 1e9 / NC;
    }
    /* c) intérprete: bucle con un jalr a una hoja que escribe (más una escritura propia) */
    stub_leaf(0x80021000u);
    pc_asm = 0x80020400u;
    emit(ADDIU(SP, SP, -8)); emit(SW(RA, SP, 0));
    emit(LUI(T9, 0x8002)); emit(ORI(T9, T9, 0x1000));
    emit(JALR(RA, T9)); emit(ADDIU(S0, S0, -1));               /* L (0x80020410): jalr; slot: s0-- */
    emit(SW(A1, A2, 0));
    emit(BNE(S0, 0, -4)); emit(NOP);
    emit(LW(RA, SP, 0)); emit(ADDIU(SP, SP, 8)); emit(JR_RA); emit(NOP);
    double ip[2];
    for (int on = 0; on < 2; on++) {
        rt_prov_set(on);
        reset(); C.r[S0] = NI; C.r[A0] = 0x80110000u; C.r[A2] = 0x80100300u;
        double t0 = now_s();
        interp_call(&C, 0x80020400u);
        ip[on] = (now_s() - t0) * 1e9 / NI;
    }
    rt_prov_set(0);
    printf("bench (ns por operacion, apagado -> encendido):\n");
    printf("  mem_w32:                 %.2f -> %.2f (+%.2f)\n", w[0], w[1], w[1] - w[0]);
    printf("  rt_call nativa (hoja):   %.1f -> %.1f (+%.1f)\n", cl[0], cl[1], cl[1] - cl[0]);
    printf("  interprete jalr+2 sw:    %.1f -> %.1f (+%.1f) por iteracion (jalr + hoja + 2 sw + bne, ~8 instr)\n", ip[0], ip[1], ip[1] - ip[0]);
}

int main(int argc, char **argv) {
    interp_log_enabled = 0;
    rt_register_table(nat_table, sizeof nat_table / sizeof nat_table[0]);
    if (argc > 1 && !strcmp(argv[1], "bench")) { bench(); return 0; }
    OK(rt_prov_on == 0);
    t_scope();
    t_longjmp();
    t_fiber();
    t_flat();
    t_stack();
    t_actors();
    t_full();
    t_gpu();
    if (fails) { printf("prov: %d FALLOS\n", fails); return 1; }
    printf("prov: todas las pruebas OK\n");
    return 0;
}
