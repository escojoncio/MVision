/* Enganche de ns_capture a la etapa «pantalla» sobre el intérprete real del runtime con una RAM falsa (programa MIPS a mano; sin datos del juego). */
#include <stdio.h>
#include <string.h>
#include "cpu.h"
#include "hlebind.h"
#include "rtpatch.h"
#include "ns_hook.h"
#include "ns_hook_rt.h"

void interp_call(Cpu *c, uint32_t entry);
extern int interp_log_enabled;
const FuncEntry rt_func_table[] = {{0, 0}};
const unsigned rt_func_count = 0;
const FuncName rt_func_names[] = {{0xFFFFFFFFu, ""}};
const unsigned rt_func_name_count = 0;
const uint32_t rt_entry = 0;

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static uint32_t pc_asm;
static void emit(uint32_t w) { mem_w32(NULL, pc_asm, w); pc_asm += 4; }
#define R_(op, s, t, d, sh, f) (((uint32_t)(op) << 26) | ((uint32_t)(s) << 21) | ((uint32_t)(t) << 16) | ((uint32_t)(d) << 11) | ((sh) << 6) | (f))
#define I_(op, s, t, i) (((uint32_t)(op) << 26) | ((uint32_t)(s) << 21) | ((uint32_t)(t) << 16) | ((i) & 0xFFFF))
#define JR_RA R_(0, 31, 0, 0, 0, 8)
#define ADDIU(t, s, i) I_(9, s, t, i)
#define LUI(t, i) I_(15, 0, t, i)
#define SW(t, s, i) I_(43, s, t, i)
enum { V0 = 2, A0 = 4, T0 = 8, T1 = 9, SP = 29, RA = 31 };
#define MARK 0x80100000u

static void put_stage(uint32_t first_word) {
    pc_asm = NS_STAGE_ADDR;
    emit(first_word);
    emit(LUI(T0, 0x8010));
    emit(ADDIU(T1, 0, 0x55));
    emit(SW(T1, T0, 0));            /* la etapa original deja una marca en RAM */
    emit(ADDIU(V0, 0, 1234));
    emit(JR_RA);
    emit(ADDIU(SP, SP, 8));
}
static void run(Cpu *c, uint32_t a0) {
    memset(c, 0, sizeof *c); c->r[SP] = 0x801FFF00u; c->r[RA] = 0xDEAD0000u; c->r[A0] = a0;
    interp_call(c, NS_STAGE_ADDR);
}

static int ex_calls; static uint32_t ex_mark;
static int extract(const uint8_t *ram, const uint8_t *scratch, NsScene *s, void *u) {
    (void)scratch; (void)u;
    ex_calls++;
    memcpy(&ex_mark, ram + (MARK & 0x1FFFFF), 4);
    s->valid = NS_SV_GAME;
    return 0;
}
static int cb_calls;
static void stage_cb(Cpu *c) { (void)c; cb_calls++; }

int main(void) {
    Cpu c;
    interp_log_enabled = 0;
    NsBridge *b = ns_bridge_create(NULL);
    ns_hook_init(b); ns_hook_reset();
    ns_hook_set_extractor(extract, NULL, NULL);
    ns_hook_set_stage_cb(stage_cb);

    /* forma incorrecta: sin prólogo de pila */
    put_stage(0);
    CHECK(ns_hook_install() == NS_HOOK_BAD_CODE && !ns_hook_installed(), "prólogo incorrecto");
    put_stage(0x27BDFFF8u);                                   /* addiu sp,sp,-8 */
    mem_w32(NULL, NS_STAGE_TABLE, 0x80012345u);
    CHECK(ns_hook_install() == NS_HOOK_BAD_TABLE, "tabla con otra rutina");
    mem_w32(NULL, NS_STAGE_TABLE, NS_STAGE_ADDR);
    run(&c, NS_MAIN_CHANNEL);                                   /* sin parche: el original corre solo */
    CHECK(ex_calls == 0 && c.r[V0] == 1234, "sin enganche no se captura");
    mem_w32(NULL, MARK, 0);

    CHECK(ns_hook_install() == NS_HOOK_OK && ns_hook_installed() && rt_patched(NS_STAGE_ADDR), "instalado");
    CHECK(ns_hook_install() == NS_HOOK_OK, "idempotente");
    run(&c, NS_MAIN_CHANNEL);
    CHECK(ex_calls == 1 && ex_mark == 0x55, "el extractor corre DESPUÉS de la etapa original (marca=%x)", ex_mark);
    CHECK(c.r[V0] == 1234 && c.r[SP] == 0x801FFF00u && c.r[RA] == 0xDEAD0000u, "el original se comporta igual (v0=%u sp=%x ra=%x)", c.r[V0], c.r[SP], c.r[RA]);
    CHECK(cb_calls == 1, "callback de etapa");
    run(&c, 0x800B2990u);                                       /* otro canal */
    CHECK(ex_calls == 1 && cb_calls == 1 && c.r[V0] == 1234, "otros canales no se capturan");
    for (int i = 0; i < 4; i++) { run(&c, NS_MAIN_CHANNEL); ns_hook_tick(emu_ram); }
    NsHookStats st; ns_hook_stats(&st);
    CHECK(st.captures == 5 && st.published == 4 && st.replaced == 1, "fotogramas contados: %u / publicadas %u / reemplazadas %u", st.captures, st.published, st.replaced);
    NsView v;
    CHECK(ns_bridge_acquire(b, ns_now_ns(), &v) && v.cur->game_frame == 5 && (v.cur->valid & NS_SV_GAME), "escena publicada con contador de fotogramas");

    /* se carga otra overlay encima: el parche queda inerte y corre el código real sin capturar */
    put_stage(0x27BDFFF0u);
    rt_code_invalidate(NS_STAGE_ADDR, 28);
    int before = ex_calls;
    run(&c, NS_MAIN_CHANNEL);
    CHECK(ex_calls == before && rt_patch_stale(NS_STAGE_ADDR), "guarda por hash");
    put_stage(0x27BDFFF8u);
    rt_code_invalidate(NS_STAGE_ADDR, 28);
    ns_hook_uninstall();
    CHECK(!ns_hook_installed() && !rt_patched(NS_STAGE_ADDR), "desinstalado");
    before = ex_calls;
    run(&c, NS_MAIN_CHANNEL);
    CHECK(ex_calls == before && c.r[V0] == 1234, "tras desinstalar no se captura");
    ns_bridge_destroy(b);
    printf(fails ? "ns_hook_rt: %d fallos\n" : "ns_hook_rt: todas las pruebas OK\n", fails);
    return fails != 0;
}
