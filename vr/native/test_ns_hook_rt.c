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

static uint32_t rd_mark8(void) { uint32_t v; memcpy(&v, emu_ram + ((MARK + 8) & 0x1FFFFFu), 4); return v; }
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
static int pre_calls;
static void pre_cb(uint8_t *ram) { (void)ram; pre_calls++; }

int main(void) {
    Cpu c;
    interp_log_enabled = 0;
    NsBridge *b = ns_bridge_create(NULL);
    ns_hook_init(b); ns_hook_reset();
    ns_hook_set_extractor(extract, NULL, NULL);
    ns_hook_set_stage_cb(stage_cb);
    ns_hook_set_precapture_cb(pre_cb);

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

    /* Respaldo en el bucle de 7 etapas (fin de la ordenación de canales): captura solo si la envoltura de la etapa «pantalla» no capturó en ese fotograma */
    {
        NsHookStats s0, s1;
        ns_hook_stats(&s0);
        int e0 = ex_calls, cb0 = cb_calls, pr0 = pre_calls;
        ns_hook_backup_begin(); run(&c, NS_MAIN_CHANNEL); ns_hook_backup_capture();   /* la envoltura capturó: el respaldo no repite */
        CHECK(ex_calls == e0 + 1 && cb_calls == cb0 + 1 && pre_calls == pr0 + 1, "envoltura + respaldo: una sola captura (ex=%d)", ex_calls - e0);
        ns_hook_backup_begin(); ns_hook_backup_capture();                 /* sin etapa «pantalla» en medio (entrada 0 de la tabla en una copia de overlay): el respaldo captura */
        CHECK(ex_calls == e0 + 2 && pre_calls == pr0 + 2, "sin envoltura: el respaldo captura (ex=%d)", ex_calls - e0);
        CHECK(cb_calls == cb0 + 1, "el respaldo NO llama al callback de etapa (volcado del punto de enganche: GTE de otra etapa)");
        ns_hook_backup_begin(); run(&c, 0x800B2990u); ns_hook_backup_capture();   /* otro canal no marca: el respaldo captura */
        CHECK(ex_calls == e0 + 3, "otro canal no cuenta como captura de la envoltura");
        ns_hook_backup_begin(); run(&c, NS_MAIN_CHANNEL); ns_hook_backup_capture(); ns_hook_backup_capture();   /* la marca se consume: el segundo respaldo del mismo fotograma sí captura */
        CHECK(ex_calls == e0 + 5, "la marca se consume (ex=%d)", ex_calls - e0);
        run(&c, NS_MAIN_CHANNEL);                                         /* fotograma con envoltura y sin respaldo (marca huérfana)... */
        ns_hook_backup_begin(); ns_hook_backup_capture();                 /* ...no roba el respaldo del siguiente: begin la borra */
        CHECK(ex_calls == e0 + 7, "una marca huérfana no roba el respaldo del fotograma siguiente (ex=%d)", ex_calls - e0);
        ns_hook_stats(&s1);
        CHECK(s1.cap_wrap - s0.cap_wrap == 3 && s1.cap_backup - s0.cap_backup == 4 && s1.captures - s0.captures == 7, "capturas por canal: envoltura %u respaldo %u", s1.cap_wrap - s0.cap_wrap, s1.cap_backup - s0.cap_backup);
        CHECK(ns_hook_stage_entry() == NS_STAGE_ADDR, "entrada 0 de la tabla");
        mem_w32(NULL, NS_STAGE_TABLE, 0x8008240Cu);
        CHECK(ns_hook_stage_entry() == 0x8008240Cu, "entrada 0 apunta a una copia en overlay");
        mem_w32(NULL, NS_STAGE_TABLE, NS_STAGE_ADDR);
    }

    /* se carga otra overlay encima: el parche queda inerte y corre el código real sin capturar */
    put_stage(0x27BDFFF0u);
    rt_code_invalidate(NS_STAGE_ADDR, 28);
    int before = ex_calls;
    run(&c, NS_MAIN_CHANNEL);
    CHECK(ex_calls == before && rt_patch_stale(NS_STAGE_ADDR), "guarda por hash");
    /* B4: la overlay nueva tiene otra forma valida -> poll retira el parche caducado y reinstala con el hash nuevo */
    CHECK(ns_hook_poll() && ns_hook_installed() && !rt_patch_stale(NS_STAGE_ADDR) && ns_hook_stale_count() == 1, "reinstalado tras caducar");
    before = ex_calls;
    run(&c, NS_MAIN_CHANNEL);
    CHECK(ex_calls == before + 1, "captura con la overlay nueva");
    ns_hook_backup_capture();                                 /* fin del fotograma: consume la marca de la envoltura */
    CHECK(ex_calls == before + 1, "el respaldo no repite la captura de la overlay nueva");
    /* overlay sin la forma esperada: queda sin instalar y se reintenta */
    put_stage(0);
    rt_code_invalidate(NS_STAGE_ADDR, 28);
    CHECK(!ns_hook_poll() && !ns_hook_installed() && ns_hook_last_rc() == NS_HOOK_BAD_CODE, "forma incorrecta: sin parche");
    put_stage(0x27BDFFF8u);
    rt_code_invalidate(NS_STAGE_ADDR, 28);
    CHECK(ns_hook_poll() && ns_hook_installed(), "reintento correcto");
    ns_hook_uninstall();
    CHECK(!ns_hook_installed() && !rt_patched(NS_STAGE_ADDR), "desinstalado");
    before = ex_calls;
    run(&c, NS_MAIN_CHANNEL);
    CHECK(ex_calls == before && c.r[V0] == 1234, "tras desinstalar no se captura");
    ns_hook_backup_capture();
    CHECK(ex_calls == before + 1, "sin enganche instalado el respaldo sigue capturando");
    /* envoltura DINÁMICA: la entrada 0 de la tabla apunta a una copia en overlay (0x8008240c): se parchea con su propio salto al original y captura dentro */
    {
        const uint32_t OV = 0x8008240Cu;
        NsHookStats s0, s1;
        ns_hook_install();
        pc_asm = OV; emit(0x27BDFFF8u); emit(LUI(T0, 0x8010)); emit(ADDIU(T1, 0, 0x77)); emit(SW(T1, T0, 8)); emit(ADDIU(V0, 0, 4321)); emit(JR_RA); emit(ADDIU(SP, SP, 8));
        rt_code_invalidate(OV, 28);
        mem_w32(NULL, NS_STAGE_TABLE, OV);
        CHECK(!ns_hook_dyn_installed(), "dinámica: sin poll aún no hay parche");
        ns_hook_poll();
        CHECK(ns_hook_dyn_installed() && ns_hook_dyn_addr() == OV && rt_patched(OV), "dinámica instalada en la entrada 0");
        ns_hook_stats(&s0);
        int e0 = ex_calls, cb0 = cb_calls;
        memset(&c, 0, sizeof c); c.r[SP] = 0x801FFF00u; c.r[RA] = 0xDEAD0000u; c.r[A0] = NS_MAIN_CHANNEL;
        mem_w32(NULL, MARK + 8, 0);
        interp_call(&c, OV);
        CHECK(ex_calls == e0 + 1, "dinámica: captura");
        CHECK(rd_mark8() == 0x77 && c.r[V0] == 4321 && c.r[SP] == 0x801FFF00u && c.r[RA] == 0xDEAD0000u, "dinámica: el original corre y se comporta igual (marca=%x v0=%u)", rd_mark8(), c.r[V0]);
        CHECK(cb_calls == cb0 + 1, "dinámica: callback de etapa");
        ns_hook_stats(&s1);
        CHECK(s1.cap_dyn - s0.cap_dyn == 1 && s1.cap_wrap == s0.cap_wrap, "dinámica: contada aparte (dyn +%u wrap +%u)", s1.cap_dyn - s0.cap_dyn, s1.cap_wrap - s0.cap_wrap);
        ns_hook_backup_begin(); memset(&c, 0, sizeof c); c.r[SP] = 0x801FFF00u; c.r[RA] = 0xDEAD0000u; c.r[A0] = NS_MAIN_CHANNEL; interp_call(&c, OV); ns_hook_backup_capture();
        CHECK(ex_calls == e0 + 2, "dinámica: el respaldo no repite (ex=%d)", ex_calls - e0);
        memset(&c, 0, sizeof c); c.r[SP] = 0x801FFF00u; c.r[RA] = 0xDEAD0000u; c.r[A0] = 0x800B2990u; interp_call(&c, OV);
        CHECK(ex_calls == e0 + 2 && c.r[V0] == 4321, "dinámica: otros canales no se capturan");
        /* la overlay se descarga/cambia: el parche caduca por hash; con otra forma válida se reinstala con el hash nuevo */
        pc_asm = OV; emit(0x27BDFFF0u);
        rt_code_invalidate(OV, 28);
        memset(&c, 0, sizeof c); c.r[SP] = 0x801FFF00u; c.r[RA] = 0xDEAD0000u; c.r[A0] = NS_MAIN_CHANNEL; interp_call(&c, OV);
        CHECK(ex_calls == e0 + 2 && rt_patch_stale(OV), "dinámica: guarda por hash");
        unsigned st0 = ns_hook_dyn_stale_count();
        ns_hook_poll();
        CHECK(ns_hook_dyn_installed() && !rt_patch_stale(OV) && ns_hook_dyn_stale_count() == st0 + 1, "dinámica: reinstalada tras caducar");
        /* la entrada vuelve a la estándar: se retira; apuntando a código sin prólogo: no se instala */
        mem_w32(NULL, NS_STAGE_TABLE, NS_STAGE_ADDR);
        ns_hook_poll();
        CHECK(!ns_hook_dyn_installed() && !rt_patched(OV) && ns_hook_dyn_addr() == 0, "dinámica: retirada al volver la entrada estándar");
        pc_asm = OV; emit(0);
        rt_code_invalidate(OV, 28);
        mem_w32(NULL, NS_STAGE_TABLE, OV);
        ns_hook_poll();
        CHECK(!ns_hook_dyn_installed() && !rt_patched(OV), "dinámica: sin prólogo de pila no se instala");
        mem_w32(NULL, NS_STAGE_TABLE, NS_STAGE_ADDR);
        ns_hook_uninstall();
    }
    ns_bridge_destroy(b);
    printf(fails ? "ns_hook_rt: %d fallos\n" : "ns_hook_rt: todas las pruebas OK\n", fails);
    return fails != 0;
}
