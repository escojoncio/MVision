#include "vr_perception_rt.h"
#include "vr_cameras.h"
#include "cpu.h"
#include <string.h>
#include <stdio.h>
#include "sys.h"

#include "rtpatch.h"
#include "vr_ai_scan.h"

static VrpCtx G;
static Cpu *cur;                  /* CPU de la llamada que se está sustituyendo */
static uint32_t patched[2];
static RtPatchGuard hguard[2];     /* hash del bloque de cada función al localizarla: si otra carga lo pisa el parche queda inerte */
static int inited;
static int use_gather = 1;
static float range_scale_user = 1.5f;

static void co(void *u, uint32_t addr, uint32_t a0) {
    (void)u;
    /* Se ejecuta la original sobre la MISMA estructura de CPU (la interrupción/cambio de hilo del runtime la
     * identifica por puntero) y después se restauran los registros del llamante: efecto = el de la función. */
    Cpu saved = *cur;
    cur->r[4] = a0;
    rt_call_orig(cur, addr);
    *cur = saved;
}

static int actor_ok(uint32_t a) { return a >= 0x80010000u && a < 0x80200000u - 0x1000u && !(a & 3); }

/* Mientras el alcance del juego está ampliado no se entregan interrupciones (podrían ejecutar un manejador o
 * cambiar de hilo y que otro código leyera el valor ampliado). Se retrasan lo que dura la prueba de visión y se
 * entregan en el siguiente sondeo tras restaurar. */
static uint32_t irq_saved;
static void guard(void *u, int enter) {
    (void)u;
    if (enter) { irq_saved = rt_irq_enabled; rt_irq_enabled = 0; }
    else { rt_irq_enabled = irq_saved; if (rt_irq_enabled) sys_kick(); }
}

static void hook_vision(Cpu *c) {
    uint32_t a = c->r[4];
    Cpu *prev = cur;
    cur = c;
    if (actor_ok(a)) vrp_vision(&G, a);
    else co(NULL, G.sites.vision, a);
    cur = prev;
}
static void hook_gather(Cpu *c) {
    uint32_t a = c->r[4];
    Cpu *prev = cur;
    cur = c;
    if (actor_ok(a)) vrp_gather(&G, a);
    else co(NULL, G.sites.gather, a);
    cur = prev;
}

/* Los parches solo existen mientras el ajuste está activo: con el original no hay envoltorio alguno (el código
 * del juego corre sin tocar, ni registros ni RAM). */
static void unpatch_all(void) {
    for (int i = 0; i < 2; i++) if (patched[i]) { rt_unpatch(patched[i]); patched[i] = 0; }
}
static void sync_patches(void) {
    if (!inited || !G.sites.found || !(G.cfg.enabled || (G.cfg.head_see && G.head_user))) { unpatch_all(); return; }
    if (!patched[0]) {
        /* Sin guarda de hash no se parchea (un parche sin validar podría quedar vivo sobre otra overlay). */
        if (!hguard[0].nwords || rt_patch_fn(G.sites.vision, hook_vision, &hguard[0])) { G.sites.found = 0; return; }
        patched[0] = G.sites.vision;
    }
    int g = use_gather && G.sites.gather;
    if (g && !patched[1]) { if (hguard[1].nwords && rt_patch_fn(G.sites.gather, hook_gather, &hguard[1]) == 0) patched[1] = G.sites.gather; }
    else if (!g && patched[1]) { rt_unpatch(patched[1]); patched[1] = 0; }
}

void vrp_rt_init(VrpPreset p) {
    VrpHost h = {NULL, co, guard};
    vrp_init(&G, emu_ram, &h, p);
    if (p != VRP_ORIGINAL) G.cfg.range_scale = range_scale_user;
    inited = 1;
}
void vrp_rt_set_preset(VrpPreset p) { if (inited) { vrp_set_preset(&G, p); if (p != VRP_ORIGINAL) G.cfg.range_scale = range_scale_user; sync_patches(); } }
void vrp_rt_set_head(const float off[3]) { if (inited) vrp_set_head(&G, off); }
void vrp_rt_set_head_enabled(int on) { if (inited) { vrp_set_head_enabled(&G, on); sync_patches(); } }
void vrp_rt_set_range_scale(float s) {
    if (!(s >= 1.0f)) s = 1.0f; if (s > 3.0f) s = 3.0f;
    range_scale_user = s;
    if (inited && G.cfg.enabled) G.cfg.range_scale = s;
}
void vrp_rt_log(void) {
    char b[160];
    if (inited && vrp_log_tick(&G, b, sizeof b)) fprintf(stderr, "%s\n", b);
}
void vrp_rt_set_external_motion(int m) { if (inited) vrp_set_external_motion(&G, m); }
void vrp_rt_set_gather(int on) { use_gather = on != 0; sync_patches(); }
void vrp_rt_frame(float s) { if (inited) { vrp_set_time(&G, s); vrp_rt_log(); } }
const VrpCtx *vrp_rt_ctx(void) { return &G; }

void vrp_rt_overlay_gone(void) {
    unpatch_all();
    if (inited) { G.sites.found = 0; vrp_reset(&G); }
}

int vrp_rt_overlay(uint32_t lo, uint32_t hi) {
    if (!inited) return 0;
    vrp_rt_overlay_gone();
    VrpSites s;
    if (!vrp_locate(emu_ram, lo, hi, &s)) return 0;
    /* Defensa: nunca parchear una función de cámara/sensor (tienen su propio módulo) */
    VrcSite cs[8];
    int nc = vrc_locate(emu_ram, lo, hi, cs, 8);
    if (vrc_is_sensor_func(cs, nc, s.vision) || (s.gather && vrc_is_sensor_func(cs, nc, s.gather))) return 0;
    G.sites = s;
    memset(hguard, 0, sizeof hguard);
    for (int i = 0; i < 2; i++) {
        uint32_t a = i ? s.gather : s.vision, fs, fe;
        if (a && vai_func_bounds(emu_ram, a, lo, hi, &fs, &fe) && fs == a && fe - fs <= 4u * 4096u) {
            hguard[i].nwords = (fe - fs) / 4;
            hguard[i].hash = rt_code_hash(fs, hguard[i].nwords);
        }
    }
    sync_patches();
    return G.sites.found;
}
