/* Pruebas de rtpatch.c (sustitución de función y parche de palabras en ejecución). Sin datos del juego: programas a mano en RAM. */
#include <stdio.h>
#include <string.h>
#include "cpu.h"
#include "rtpatch.h"

void interp_call(Cpu *c, uint32_t entry);
extern int interp_log_enabled;

const FuncEntry rt_func_table[] = {{0, 0}};
const unsigned rt_func_count = 0;
const FuncName rt_func_names[] = {{0xFFFFFFFFu, ""}};
const unsigned rt_func_name_count = 0;
const uint32_t rt_entry = 0;

static int fails;
#define EQ(a, b) do { unsigned long long _a = (a), _b = (b); \
    if (_a != _b) { printf("FALLO %s:%d %s = 0x%llx, esperado 0x%llx\n", __FILE__, __LINE__, #a, _a, _b); fails++; } } while (0)

static uint32_t pc_asm;
static void emit(uint32_t w) { mem_w32(NULL, pc_asm, w); pc_asm += 4; }
#define R_(op, s, t, d, sh, f) (((uint32_t)(op) << 26) | ((uint32_t)(s) << 21) | ((uint32_t)(t) << 16) | ((uint32_t)(d) << 11) | ((sh) << 6) | (f))
#define I_(op, s, t, i) (((uint32_t)(op) << 26) | ((uint32_t)(s) << 21) | ((uint32_t)(t) << 16) | ((i) & 0xFFFF))
#define J_(op, a) (((uint32_t)(op) << 26) | (((a) >> 2) & 0x3FFFFFF))
#define NOP 0
#define JR_RA R_(0, 31, 0, 0, 0, 8)
#define ADDIU(t, s, i) I_(9, s, t, i)
#define ADDU(d, s, t) R_(0, s, t, d, 0, 33)
#define SW(t, s, i) I_(43, s, t, i)
#define LW(t, s, i) I_(35, s, t, i)
#define JAL(a) J_(3, a)
enum { V0 = 2, A0 = 4, A1 = 5, T0 = 8, SP = 29, RA = 31 };

static void reset(Cpu *c) { memset(c, 0, sizeof *c); c->r[SP] = 0x801FFF00u; c->r[RA] = 0xDEAD0000u; }

/* g(a0) = a0 + 1 en 0x80021000 (3 palabras) */
static void put_g(void) {
    pc_asm = 0x80021000;
    emit(ADDIU(A1, 0, 5));
    emit(JR_RA);
    emit(ADDIU(V0, A0, 1));
}
/* f(a0) = g(a0) + 100 en 0x80020100 */
static void put_f(void) {
    pc_asm = 0x80020100;
    emit(ADDIU(SP, SP, -8));
    emit(SW(RA, SP, 4));
    emit(JAL(0x80021000));
    emit(NOP);
    emit(LW(RA, SP, 4));
    emit(ADDIU(SP, SP, 8));
    emit(JR_RA);
    emit(ADDIU(V0, V0, 100));
}
static int run_f(Cpu *c, uint32_t a0) { reset(c); c->r[A0] = a0; interp_call(c, 0x80020100); return (int)c->r[V0]; }

/* sustitutas */
static int calls_sub;
static void sub_times2(Cpu *c) { calls_sub++; c->r[V0] = c->r[A0] * 2; }
static void sub_wrap(Cpu *c) {   /* envoltorio: original + 1000 */
    calls_sub++;
    rt_call_orig(c, 0x80021000);
    c->r[V0] += 1000;
}

static void t_replace(Cpu *c) {
    put_g(); put_f();
    EQ(run_f(c, 4), 4 + 1 + 100);                /* decodifica y deja en caché los bloques */
    EQ(rt_patch(0x80021000, sub_times2), RT_PATCH_OK);
    EQ(rt_patched(0x80021000), 1);
    EQ(rt_patch_count(), 1);
    EQ(run_f(c, 4), 8 + 100);                    /* el bloque cacheado de f ya llama a la sustituta */
    EQ(calls_sub, 1);
    EQ(c->r[RA], 0xDEAD0000u);
    EQ(c->r[SP], 0x801FFF00u);
    rt_unpatch(0x80021000);
    EQ(rt_patched(0x80021000), 0);
    EQ(run_f(c, 4), 5 + 100);                    /* reversible: vuelve el original */
    EQ(calls_sub, 1);
    EQ(rt_patch_count(), 0);
}

static void t_orig(Cpu *c) {
    put_g(); put_f();
    EQ(rt_patch(0x80021000, sub_wrap), RT_PATCH_OK);
    calls_sub = 0;
    EQ(run_f(c, 9), 9 + 1 + 1000 + 100);
    EQ(calls_sub, 1);
    EQ(c->r[A1], 5);                             /* el original sí corrió (su addiu a1) */
    rt_unpatch(0x80021000);
}

static void t_guard(Cpu *c) {
    put_g(); put_f();
    RtPatchGuard g = {3, rt_code_hash(0x80021000, 3)};
    EQ(g.hash != 0, 1);
    RtPatchGuard bad = {3, g.hash ^ 1};
    EQ(rt_patch_fn(0x80021000, sub_times2, &bad), RT_PATCH_HASH);   /* otra edición: no se parchea */
    EQ(rt_patched(0x80021000), 0);
    EQ(rt_patch_fn(0x80021000, sub_times2, &g), RT_PATCH_OK);
    calls_sub = 0;
    EQ(run_f(c, 4), 108);
    /* se carga otro código encima (otra overlay): el parche queda inerte y corre el código real */
    pc_asm = 0x80021000;
    emit(ADDIU(A1, 0, 6));
    emit(JR_RA);
    emit(ADDIU(V0, A0, 50));
    rt_code_invalidate(0x80021000, 12);
    EQ(run_f(c, 4), 54 + 100);
    EQ(rt_patch_stale(0x80021000), 1);
    EQ(rt_patched(0x80021000), 0);
    EQ(calls_sub, 1);
    /* la misma overlay vuelve: el parche vuelve a valer */
    put_g();
    rt_code_invalidate(0x80021000, 12);
    EQ(run_f(c, 4), 108);
    EQ(rt_patch_stale(0x80021000), 0);
    EQ(rt_patched(0x80021000), 1);
    rt_unpatch(0x80021000);
}

static void t_busy_limits(Cpu *c) {
    (void)c;
    EQ(rt_patch(0x80021002, sub_times2), RT_PATCH_BAD_ADDR);   /* sin alinear */
    EQ(rt_patch(0x80300000, sub_times2), RT_PATCH_BAD_ADDR);   /* fuera de la RAM */
    EQ(rt_patch(0x80021000, NULL), RT_PATCH_BAD_ADDR);
    RtPatchGuard g0 = {0, 1};
    EQ(rt_patch_fn(0x80021000, sub_times2, &g0), RT_PATCH_BAD_ADDR);
    for (int i = 0; i < RT_PATCH_MAX; i++) EQ(rt_patch(0x80022000 + 4u * i, sub_times2), RT_PATCH_OK);
    EQ(rt_patch(0x80023000, sub_times2), RT_PATCH_FULL);
    EQ(rt_patch(0x80022000, sub_wrap), RT_PATCH_OK);           /* re-registrar la misma dirección no ocupa otra ranura */
    rt_unpatch_all();
    EQ(rt_patch_count(), 0);
}

static void t_code(Cpu *c) {
    put_g(); put_f();
    uint32_t exp[3], neu[3];
    for (int i = 0; i < 3; i++) memcpy(&exp[i], psx_ram + 0x21000 + 4 * i, 4);
    memcpy(neu, exp, sizeof neu);
    neu[2] = ADDIU(V0, A0, 7);                  /* g(a0) = a0 + 7 */
    EQ(run_f(c, 4), 105);                        /* bloques en caché */
    uint32_t wrong[3] = {exp[0], exp[1], exp[2] ^ 1};
    int id = -1;
    EQ(rt_patch_code(0x80021000, 3, wrong, neu, &id), RT_PATCH_HASH);   /* todo o nada */
    EQ(run_f(c, 4), 105);
    EQ(rt_patch_code(0x80021000, 3, exp, neu, &id), RT_PATCH_OK);
    EQ(id >= 0, 1);
    EQ(rt_patch_code_count(), 1);
    EQ(run_f(c, 4), 111);                        /* la caché del intérprete se invalidó */
    EQ(rt_unpatch_code(id), 0);
    EQ(run_f(c, 4), 105);
    EQ(rt_patch_code_count(), 0);
    /* restaurar tras pisarlo otra carga: no se toca lo nuevo */
    EQ(rt_patch_code(0x80021000, 3, exp, neu, &id), RT_PATCH_OK);
    pc_asm = 0x80021000; emit(ADDIU(A1, 0, 9)); emit(JR_RA); emit(ADDIU(V0, A0, 20));
    rt_code_invalidate(0x80021000, 12);
    EQ(rt_unpatch_code(id), 1);
    EQ(run_f(c, 4), 124);
    EQ(rt_unpatch_code(id), -1);
}

int main(void) {
    Cpu c;
    interp_log_enabled = 0;
    t_replace(&c);
    t_orig(&c);
    t_guard(&c);
    t_busy_limits(&c);
    t_code(&c);
    printf(fails ? "%d fallos\n" : "rtpatch: todas las pruebas OK\n", fails);
    return fails != 0;
}
