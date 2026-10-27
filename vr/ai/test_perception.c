/* Pruebas de vr_perception / vr_cameras con datos sintéticos (y, si existen, con las overlays extraídas). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include "vr_perception.h"
#include "vr_cameras.h"
#include "vr_ai_scan.h"

static int nfail, nok;
#define CHECK(c, ...) do { if (c) nok++; else { nfail++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static VrpEntry fresh(void) { VrpEntry e; memset(&e, 0, sizeof e); e.last_t = -1; e.armed = 1; return e; }

/* Simula `secs` segundos de exposición continua a distancia d (pasos de 0,2 s); devuelve el tiempo hasta out==2
 * (-1 si no llega) y el tiempo hasta la primera sospecha en *tq. */
static float time_to(const VrpConfig *c, float d, float L, float off, int mv, int st, float secs, float *tq) {
    VrpEntry e = fresh();
    float t = 0, ta = -1; if (tq) *tq = -1;
    for (; t <= secs; t += 0.2f) {
        int o = vrp_step(c, &e, &(VrpInput){2, d, L, off, mv, st, t});
        if (e.q_pending && tq && *tq < 0) *tq = t;
        if (o == 2 && ta < 0) { ta = t; break; }
    }
    return ta;
}

static void t_config(void) {
    VrpConfig o, v, x;
    vrp_config_preset(&o, VRP_ORIGINAL); vrp_config_preset(&v, VRP_VR); vrp_config_preset(&x, VRP_DEMANDING);
    CHECK(!o.enabled && v.enabled && x.enabled, "presets enabled");
    CHECK(v.range_mult > 1.0f && x.range_mult > v.range_mult, "exigente alcanza más lejos");
    CHECK(x.t_alert < v.t_alert && x.t_suspect < v.t_suspect && x.decay < v.decay, "exigente más rápido");
    CHECK(v.range_scale == 1.0f && x.range_scale == 1.0f, "alcance de visión por defecto = el del juego (x1): %.2f %.2f", v.range_scale, x.range_scale);
}

static void t_rate(void) {
    VrpConfig c; vrp_config_preset(&c, VRP_VR);
    float L = 4000, lmax = L * c.range_mult;
    CHECK(vrp_rate(&c, L, L) == 0.0f, "en L el tramo ampliado no rige (lo cubre el original)");
    CHECK(fabsf(vrp_rate(&c, L + 1, L) - 1.0f) < 1e-3f, "justo tras L ritmo ~1");
    CHECK(vrp_rate(&c, lmax, L) == 0.0f && vrp_rate(&c, lmax + 100, L) == 0.0f, "0 en Lmax y fuera");
    float prev = 2;
    for (float d = L + 1; d < lmax; d += 100) { float r = vrp_rate(&c, d, L); CHECK(r <= prev + 1e-6f && r >= 0, "monótona decreciente"); prev = r; }
    CHECK(vrp_rate(&c, 0, 0) == 0.0f && vrp_rate(&c, 100, -5) == 0.0f, "L inválido");
    CHECK(vrp_rate(&c, NAN, L) == 0.0f, "NaN");
}

static void t_curve(void) {
    VrpConfig c; vrp_config_preset(&c, VRP_VR);
    const float L = 4000;
    /* cerca (<= L): alerta inmediata en la primera evaluación */
    VrpEntry e = fresh();
    CHECK(vrp_step(&c, &e, &(VrpInput){2, 3000, L, 0.1f, 0, 0, 1.0f}) == 2, "cerca: inmediata");
    /* media: primero 1 (sospecha) y luego 2 tras un tiempo; lejos: más tiempo; muy lejos: casi nunca */
    float tq1, tq2, tq3;
    float a1 = time_to(&c, 1.2f * L, L, 0.1f, 0, 0, 120, &tq1);
    float a2 = time_to(&c, 1.8f * L, L, 0.1f, 0, 0, 120, &tq2);
    float a3 = time_to(&c, 2.3f * L, L, 0.1f, 0, 0, 120, &tq3);
    CHECK(a1 > 0 && a2 > a1 && (a3 < 0 || a3 > a2), "tiempo a alerta crece con la distancia: %.1f %.1f %.1f", a1, a2, a3);
    CHECK(tq1 >= 0 && tq1 < a1, "la sospecha ('?') llega antes que la alerta (%.1f < %.1f)", tq1, a1);
    CHECK(tq2 > tq1, "sospecha más tardía más lejos");
    /* fuera de Lmax: nunca */
    CHECK(time_to(&c, 2.45f * L, L, 0.1f, 0, 0, 600, NULL) < 0, "justo fuera de Lmax (2,4 L) nunca alerta");
    CHECK(time_to(&c, 2.6f * L, L, 0.1f, 0, 0, 600, NULL) < 0, "fuera de Lmax nunca alerta");
    /* en el original (range_mult=1/enabled=0 se gestiona fuera): con el ajuste exigente es más rápido */
    VrpConfig x; vrp_config_preset(&x, VRP_DEMANDING);
    CHECK(time_to(&x, 1.5f * L, L, 0.1f, 0, 0, 120, NULL) < time_to(&c, 1.5f * L, L, 0.1f, 0, 0, 120, NULL), "exigente alerta antes");
}

static void t_decay_events(void) {
    VrpConfig c; vrp_config_preset(&c, VRP_VR);
    const float L = 4000, d = 1.3f * L;
    VrpEntry e = fresh();
    float t = 0; int q = 0;
    for (; t < 3.0f; t += 0.2f) { vrp_step(&c, &e, &(VrpInput){2, d, L, 0.1f, 0, 0, t}); if (e.q_pending) { q++; e.q_pending = 0; } }
    CHECK(q == 1, "una sola sospecha mientras sigue a la vista (cooldown): %d", q);
    float acc = e.acc;
    CHECK(acc > c.t_suspect, "acumulador sube");
    /* se oculta 2 s: decae */
    for (int i = 0; i < 10; i++, t += 0.2f) vrp_step(&c, &e, &(VrpInput){0, d, L, 0.1f, 0, 0, t});
    CHECK(e.acc < acc && e.acc >= 0, "decae al ocultarse");
    for (int i = 0; i < 40; i++, t += 0.2f) vrp_step(&c, &e, &(VrpInput){0, d, L, 0.1f, 0, 0, t});
    CHECK(e.acc == 0.0f && e.armed, "a cero y rearmado");
    /* la sospecha puede repetirse tras ocultarse y pasar el cooldown */
    q = 0;
    for (int i = 0; i < 40; i++, t += 0.2f) { vrp_step(&c, &e, &(VrpInput){2, d, L, 0.1f, 0, 0, t}); if (e.q_pending) { q++; e.q_pending = 0; } }
    CHECK(q >= 1, "nueva sospecha tras volver a la vista");
    /* paso de tiempo gigante (pausa) se acota */
    VrpEntry f = fresh();
    vrp_step(&c, &f, &(VrpInput){2, d, L, 0.1f, 0, 0, 0});
    vrp_step(&c, &f, &(VrpInput){2, d, L, 0.1f, 0, 0, 1e6f});
    CHECK(f.acc <= c.dt_max + 1e-4f, "dt acotado (%f)", f.acc);
    vrp_step(&c, &f, &(VrpInput){2, d, L, 0.1f, 0, 0, 5});   /* tiempo hacia atrás: dt = 0 */
    CHECK(f.acc <= c.dt_max + 1e-4f, "tiempo no monótono sin efecto");
    /* alerta: pasa a 2 y limpia la sospecha pendiente */
    VrpEntry g = fresh();
    int o = 0;
    for (float tt = 0; tt < 60 && o != 2; tt += 0.2f) o = vrp_step(&c, &g, &(VrpInput){2, d, L, 0.1f, 0, 0, tt});
    CHECK(o == 2 && !g.q_pending, "alerta lejana limpia q_pending");
    /* una vez reconocido, al alejarse sigue reconocido mientras esté dentro de L (histéresis) */
    CHECK(vrp_step(&c, &g, &(VrpInput){2, 0.9f * L, L, 0.1f, 0, 0, 100}) == 2, "dentro de L alerta original");
}

static void t_periphery(void) {
    VrpConfig c; vrp_config_preset(&c, VRP_VR);
    const float L = 4000;
    /* periferia, quieto, lejos: nunca registra */
    VrpEntry e = fresh();
    int o = 0;
    for (float t = 0; t < 120; t += 0.2f) o = vrp_step(&c, &e, &(VrpInput){2, 1.3f * L, L, 0.9f, 0, 0, t});
    CHECK(o == 1 && e.acc == 0, "periferia quieto: nada");
    /* periferia, en movimiento: sí, pero más lento que en el centro */
    float tc = time_to(&c, 1.3f * L, L, 0.1f, 1, 0, 200, NULL), tp = time_to(&c, 1.3f * L, L, 0.9f, 1, 0, 200, NULL);
    CHECK(tc > 0 && tp > tc, "periferia moviéndose más lento (%.1f vs %.1f)", tp, tc);
    /* periferia cerca (<= periph_near*L): se mantiene la lógica original */
    VrpEntry n = fresh();
    CHECK(vrp_step(&c, &n, &(VrpInput){2, 0.4f * L, L, 0.95f, 0, 0, 1}) == 2, "periferia a corta distancia = original");
    /* a menos de 500 la original ignora el ángulo: también aquí */
    VrpEntry m = fresh();
    CHECK(vrp_step(&c, &m, &(VrpInput){2, 300, 400, 5.0f, 0, 0, 1}) == 2, "pegado: sin regla de periferia");
    /* periferia dentro de L pero más allá de periph_near: quieto no alerta */
    VrpEntry p = fresh();
    CHECK(vrp_step(&c, &p, &(VrpInput){2, 0.8f * L, L, 0.9f, 0, 0, 1}) == 1, "periferia quieto en 0,8L: sin alerta inmediata");
    CHECK(vrp_step(&c, &p, &(VrpInput){2, 0.8f * L, L, 0.9f, 1, 0, 1.2f}) == 2, "periferia moviéndose en 0,8L: alerta original");
}

static void t_stance(void) {
    VrpConfig c; vrp_config_preset(&c, VRP_VR);
    const float L = 4000, d = 1.3f * L;
    float st = time_to(&c, d, L, 0.1f, 0, 0, 400, NULL), cr = time_to(&c, d, L, 0.1f, 0, 1, 400, NULL), pr = time_to(&c, d, L, 0.1f, 0, 2, 400, NULL);
    float mv = time_to(&c, d, L, 0.1f, 1, 0, 400, NULL);
    CHECK(st > 0 && cr > st && pr > cr, "agachado/tumbado tardan más (%.1f %.1f %.1f)", st, cr, pr);
    CHECK(mv > 0 && mv < st, "en movimiento antes");
}

/* ---------------- RAM sintética + "original" falsa ---------------- */
static uint8_t RAM[VAI_RAM_SIZE];
#define ACTOR 0x80100000u
#define GLOBAL_EYE 0x800E3104u
#define MODE_ADDR 0x800E3128u
#define VISION 0x800CB1ACu
#define GATHER 0x800CB054u
static int turn, fake_called, saw_len, saw_global, fake_gather_called;
static int32_t fk_dis, fk_dir, fk_vis_ok;   /* escenario */
static int fk_force = -1, guard_on, guard_enters, guard_exits, fake_in_guard;   /* valor forzado en el campo; guarda del host */
static void guard_fn(void *u, int enter) { (void)u; guard_on = enter; if (enter) guard_enters++; else guard_exits++; }

static void fake(void *u, uint32_t addr, uint32_t a0) {
    (void)u;
    const VrpLayout L = {"A", 2956, 2958, 2960, 2962, 3108, 3112};
    if (addr == VISION) {
        fake_called++; fake_in_guard = guard_on;
        saw_len = vai_r16(RAM, a0 + L.o_len);
        saw_global = (int32_t)vai_r32(RAM, GLOBAL_EYE);
        vai_w32(RAM, a0 + L.o_sndis, (uint32_t)fk_dis);
        vai_w32(RAM, a0 + L.o_sndir, (uint32_t)fk_dir);
        if (!turn) return;                                  /* la original no evalúa este fotograma */
        int16_t f = 0;
        if (fk_vis_ok && saw_global + 2000 >= fk_dis) f = saw_len < fk_dis ? 1 : 2;
        if (fk_force >= 0) f = (int16_t)fk_force;
        vai_w16(RAM, a0 + L.o_flag, f);
    } else if (addr == GATHER) {
        fake_gather_called++;
        vai_w8(RAM, a0 + 2977, 0);
    }
}

static void setup(VrpCtx *x, VrpPreset p) {
    memset(RAM, 0, sizeof RAM);
    VrpHost h = {NULL, fake, guard_fn};
    vrp_init(x, RAM, &h, p);
    x->cfg.range_scale = 1.0f;   /* estas pruebas miden el modelo con el alcance original; el factor del menú se prueba en test_head.c */
    fk_force = -1; guard_on = guard_enters = guard_exits = 0;
    static const VrpLayout LA = {"A", 2956, 2958, 2960, 2962, 3108, 3112};
    x->sites.found = 1; x->sites.vision = VISION; x->sites.lay = &LA; x->sites.eye_global = GLOBAL_EYE;
    x->sites.gather = GATHER; x->sites.o_events = 2977; x->sites.mode_addr = MODE_ADDR;
    vai_w16(RAM, ACTOR + 2960, 4000);   /* alcance */
    vai_w16(RAM, ACTOR + 2958, 512);    /* semiángulo */
    vai_w16(RAM, ACTOR + 2956, 0);      /* rumbo */
    vai_w32(RAM, GLOBAL_EYE, 4000);
    vai_w32(RAM, MODE_ADDR, 0);
    vai_w32(RAM, VRP_A_PLAYER_STATUS, 0);
    turn = 1; fk_vis_ok = 1; fk_dir = 0;
}

static void t_wrap(void) {
    VrpCtx x; 
    /* original: pasa directo, el alcance no se toca */
    setup(&x, VRP_ORIGINAL);
    fk_dis = 6000;
    vrp_vision(&x, ACTOR);
    CHECK(fake_called == 1 && saw_len == 4000 && vai_r16(RAM, ACTOR + 2962) == 1, "ORIGINAL: transparente (1 = fuera de alcance)");
    /* VR: la original ve el alcance ampliado y se restaura */
    setup(&x, VRP_VR);
    fk_dis = 6000; vrp_set_time(&x, 1.0f);
    vrp_vision(&x, ACTOR);
    CHECK(saw_len == 9600 && saw_global == 9600, "alcance ampliado durante la llamada (%d %d)", saw_len, saw_global);
    CHECK(vai_r16(RAM, ACTOR + 2960) == 4000 && (int32_t)vai_r32(RAM, GLOBAL_EYE) == 4000, "alcance restaurado");
    CHECK(vai_r16(RAM, ACTOR + 2962) == 1, "lejos, primera vez: visto pero no reconocido (1)");
    /* con el tiempo reconoce */
    for (int i = 1; i < 400; i++) { vrp_set_time(&x, 1.0f + 0.2f * i); vrp_vision(&x, ACTOR); }
    CHECK(vai_r16(RAM, ACTOR + 2962) == 2, "tras exposición prolongada: alerta");
    CHECK(x.st.alerts_far == 1, "una alerta lejana contada");
    /* cerca: inmediata */
    fk_dis = 3000; vrp_set_time(&x, 500);
    VrpEntry *e = vrp_entry(&x, ACTOR, 0); e->acc = 0; e->last_out = 0;
    vrp_vision(&x, ACTOR);
    CHECK(vai_r16(RAM, ACTOR + 2962) == 2, "cerca: inmediata");
    /* fuera de Lmax (original da 1/0) */
    setup(&x, VRP_VR); fk_dis = 20000; vrp_set_time(&x, 1);
    vrp_vision(&x, ACTOR);
    CHECK(vai_r16(RAM, ACTOR + 2962) == 0, "fuera de Lmax: 0 (la original lo descarta)");
    /* no visible (geometría): 0 */
    fk_dis = 5000; fk_vis_ok = 0;
    vrp_vision(&x, ACTOR);
    CHECK(vai_r16(RAM, ACTOR + 2962) == 0, "oculto: 0");
    /* turno ajeno: el campo se conserva tal como estaba */
    fk_vis_ok = 1; turn = 0; vai_w16(RAM, ACTOR + 2962, 2);
    vrp_vision(&x, ACTOR);
    CHECK(vai_r16(RAM, ACTOR + 2962) == 2, "no es su turno: sin cambios");
    CHECK((int32_t)vai_r32(RAM, ACTOR + 3108) == 5000, "sn_dis lo actualiza la original");
    /* ciego (alcance 0): original sin ampliar */
    setup(&x, VRP_VR); vai_w16(RAM, ACTOR + 2960, 0); fk_dis = 100;
    vrp_vision(&x, ACTOR);
    CHECK(saw_len == 0, "alcance 0: sin ampliar");
    /* sin localizar: transparente */
    setup(&x, VRP_VR); x.sites.found = 0; fk_dis = 100; fake_called = 0;
    vrp_vision(&x, ACTOR);
    CHECK(fake_called == 1 && saw_len == 4000, "sin sitio: transparente");
    /* valor ajeno en el campo de resultado se respeta (y no crea entrada) */
    setup(&x, VRP_VR); fk_dis = 100; fk_force = 7; vrp_set_time(&x, 1);
    vrp_vision(&x, ACTOR);
    CHECK(vai_r16(RAM, ACTOR + 2962) == 7 && !vrp_entry(&x, ACTOR, 0), "valor ajeno respetado");
    CHECK(vai_r16(RAM, ACTOR + 2960) == 4000 && (int32_t)vai_r32(RAM, GLOBAL_EYE) == 4000, "valor ajeno: alcance restaurado igualmente");
    fk_force = -1;
    /* la guarda del host rodea exactamente la llamada a la original ampliada */
    setup(&x, VRP_VR); fk_dis = 6000; vrp_set_time(&x, 1); fake_in_guard = 0;
    vrp_vision(&x, ACTOR);
    CHECK(guard_enters == 1 && guard_exits == 1 && guard_on == 0 && fake_in_guard == 1, "guarda: dentro durante la original, cerrada al volver");
    setup(&x, VRP_ORIGINAL); fk_dis = 6000; vrp_vision(&x, ACTOR);
    CHECK(guard_enters == 0, "ORIGINAL: sin guarda");
    setup(&x, VRP_VR);
    /* periferia: sn_dir lejos del rumbo, quieto, lejos de la mitad -> 1 */
    fk_dis = 3000; fk_dir = 480; vrp_set_time(&x, 1);
    vrp_vision(&x, ACTOR);
    CHECK(vai_r16(RAM, ACTOR + 2962) == 1, "periferia quieto a 0,75L: no reconoce");
    vai_w32(RAM, VRP_A_PLAYER_STATUS, VRP_PS_MOVE);
    vrp_set_time(&x, 1.2f);
    vrp_vision(&x, ACTOR);
    CHECK(vai_r16(RAM, ACTOR + 2962) == 2, "periferia en movimiento: original");
    vai_w32(RAM, VRP_A_PLAYER_STATUS, 0); vrp_set_external_motion(&x, 1);
    vrp_set_time(&x, 1.4f);
    vrp_vision(&x, ACTOR);
    CHECK(vai_r16(RAM, ACTOR + 2962) == 2, "movimiento externo (VR) cuenta");
}

/* Con el ajuste original (y con soldado ciego/sin sitio) la RAM resultante es idéntica byte a byte a llamar a la
 * original directamente, en varios escenarios. */
static void t_identity(void) {
    static uint8_t ref[VAI_RAM_SIZE];
    VrpCtx x;
    for (int sc = 0; sc < 6; sc++) {
        for (int pass = 0; pass < 2; pass++) {
            setup(&x, VRP_ORIGINAL);
            if (sc == 4) vai_w16(RAM, ACTOR + 2960, 0);
            if (sc == 5) x.sites.found = 0;
            fk_dis = sc == 0 ? 3000 : sc == 1 ? 6000 : sc == 2 ? 20000 : 5000; fk_dir = sc == 3 ? 480 : 0;
            fk_vis_ok = sc != 2; turn = sc != 1;
            vrp_set_time(&x, 1.0f);
            if (pass == 0) fake(NULL, VISION, ACTOR); else vrp_vision(&x, ACTOR);
            if (pass == 0) memcpy(ref, RAM, sizeof RAM);
        }
        CHECK(!memcmp(ref, RAM, sizeof RAM), "ORIGINAL escenario %d: RAM idéntica a la original", sc);
    }
}

/* Entrada reutilizada por otro actor o reloj reiniciado: no hereda acumulador, armado ni enfriamiento. */
static void t_stale(void) {
    VrpCtx x; setup(&x, VRP_VR);
    fk_dis = 6000;
    for (int i = 0; i < 12; i++) { vrp_set_time(&x, 1.0f + 0.2f * i); vrp_vision(&x, ACTOR); }
    VrpEntry *e = vrp_entry(&x, ACTOR, 0);
    CHECK(e && e->acc > 1.0f && e->next_q > 0, "acumulador alto antes");
    vrp_set_time(&x, 20.0f);   /* el soldado desapareció; otro actor ocupa la misma dirección */
    vrp_vision(&x, ACTOR);
    e = vrp_entry(&x, ACTOR, 0);
    CHECK(e && e->acc == 0.0f && e->next_q == 0.0f && e->armed && !e->q_pending, "entrada obsoleta: reiniciada (acc=%f)", e ? e->acc : -1);
    for (int i = 0; i < 6; i++) { vrp_set_time(&x, 20.0f + 0.2f * (i + 1)); vrp_vision(&x, ACTOR); }
    float a1 = vrp_entry(&x, ACTOR, 0)->acc;
    vrp_set_time(&x, 0.5f);    /* reloj del host hacia atrás (reinicio) */
    vrp_vision(&x, ACTOR);
    e = vrp_entry(&x, ACTOR, 0);
    CHECK(e->acc < a1 && e->last_t == 0.5f, "reloj hacia atrás: reiniciada");
}

static void t_gather(void) {
    VrpCtx x;
    setup(&x, VRP_VR);
    fk_dis = 6000;   /* zona media (1,5 L) */
    uint8_t seen_q = 0;
    for (int i = 0; i < 60 && !seen_q; i++) {
        vrp_set_time(&x, 0.2f * i);
        vrp_vision(&x, ACTOR);
        fake_gather_called = 0;
        vrp_gather(&x, ACTOR);
        seen_q = (vai_r8(RAM, ACTOR + 2977) & 2) != 0;
    }
    CHECK(seen_q && fake_gather_called == 1, "la sospecha se entrega como bit 1 de eventos");
    CHECK(x.st.suspects == 1, "contada");
    vrp_gather(&x, ACTOR);
    CHECK((vai_r8(RAM, ACTOR + 2977) & 2) == 0, "no se repite sin nueva sospecha (la original limpia el campo)");
    /* modo alerta general: no se entrega */
    setup(&x, VRP_VR); fk_dis = 6000; vai_w32(RAM, MODE_ADDR, 1);
    int got = 0;
    for (int i = 0; i < 60; i++) { vrp_set_time(&x, 0.2f * i); vrp_vision(&x, ACTOR); vrp_gather(&x, ACTOR); got |= (vai_r8(RAM, ACTOR + 2977) & 2) != 0; }
    CHECK(!got, "en alerta general no hay '?'");
    /* otros bits del campo se respetan */
    setup(&x, VRP_VR); fk_dis = 6000;
    VrpEntry *e = vrp_entry(&x, ACTOR, 1); e->q_pending = 1;
    vai_w8(RAM, ACTOR + 2977, 0);   /* la falsa lo limpia siempre; comprobamos que solo se añade el bit 2 */
    vrp_gather(&x, ACTOR);
    CHECK(vai_r8(RAM, ACTOR + 2977) == 2, "solo se añade el bit 2");
    /* actor sin entrada: nada */
    vrp_gather(&x, ACTOR + 0x1000);
    CHECK(vai_r8(RAM, ACTOR + 0x1000 + 2977) == 0, "sin entrada");
}

static void t_table(void) {
    VrpCtx x; setup(&x, VRP_VR);
    for (uint32_t i = 1; i <= 300; i++) { x.now = (float)i * 0.01f; VrpEntry *e = vrp_entry(&x, ACTOR + i * 0x1000, 1); CHECK(e != NULL, "entrada %u", i); e->last_t = x.now; }
    int n = 0; for (int i = 0; i < VRP_MAX_ENTRIES; i++) n += x.tab[i].actor != 0;
    CHECK(n == VRP_MAX_ENTRIES, "tabla llena sin desbordar");
    CHECK(vrp_entry(&x, 0, 1) == NULL, "actor 0");
    x.now = 100; VrpEntry *e = vrp_entry(&x, ACTOR + 0xFFFF0, 1);
    CHECK(e && e->acc == 0 && e->last_t < 0, "reutiliza una obsoleta");
    vrp_reset(&x); n = 0; for (int i = 0; i < VRP_MAX_ENTRIES; i++) n += x.tab[i].actor != 0;
    CHECK(n == 0, "reset");
}

/* Fuzz: RAM aleatoria, el localizador no se sale ni cuelga */
static void t_fuzz(void) {
    srand(1234);
    VrpSites s; VrcSite cs[8];
    for (int it = 0; it < 200; it++) {
        for (size_t i = 0; i < sizeof RAM; i++) RAM[i] = (uint8_t)rand();
        if (it & 1) { /* insertar las llamadas de la firma en sitios aleatorios */
            for (int k = 0; k < 50; k++) {
                uint32_t a = ((uint32_t)rand() % (VAI_RAM_SIZE - 64)) & ~3u;
                vai_w32(RAM, 0x80000000u + a, vai_jal(VRP_FN_SUB3)); vai_w32(RAM, 0x80000000u + a + 8, vai_jal(VRP_FN_DIR));
                vai_w32(RAM, 0x80000000u + a + 12, vai_jal(VRP_FN_LEN)); vai_w32(RAM, 0x80000000u + a + 16, vai_jal(VRP_FN_ADIFF));
            }
        }
        vrp_locate(RAM, 0x800C4380u, 0x800F0000u, &s);
        vrc_locate(RAM, 0x800C4380u, 0x800F0000u, cs, 8);
    }
    CHECK(!vrp_locate(RAM, 0x800F0000u, 0x800C4380u, &s) && !vrp_locate(NULL, 0x800C4380u, 0x800F0000u, &s) &&
          !vrp_locate(RAM, 0x800C4381u, 0x800F0000u, &s) && !vrp_locate(RAM, 0x80000000u, 0x80300000u, &s), "ventanas inválidas");
    CHECK(vrc_locate(RAM, 0x800C4380u, 0x800F0000u, cs, 0) == 0, "max 0");
}

/* ---------------- Overlays reales (si existen) ---------------- */
static int load_ov(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    memset(RAM, 0, sizeof RAM);
    size_t n = fread(RAM + 0xC4380, 1, 0x100000, f);
    fclose(f);
    return (int)(n & ~3u);
}

static void t_overlays(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) { printf("(sin overlays en %s: parte real omitida)\n", dir); return; }
    struct dirent *de;
    int nov = 0, nvis = 0, ncam = 0, nbad = 0, ngather = 0, nglob = 0;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l < 5 || strcmp(de->d_name + l - 4, ".bin")) continue;
        char p[512]; snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        int n = load_ov(p);
        if (n <= 0) continue;
        nov++;
        VrpSites s; VrcSite cs[8];
        uint32_t lo = 0x800C4380u, hi = lo + (uint32_t)n;
        int f = vrp_locate(RAM, lo, hi, &s);
        int nc = vrc_locate(RAM, lo, hi, cs, 8);
        ncam += nc;
        if (f) {
            nvis++;
            ngather += s.gather != 0;
            nglob += s.eye_global != 0;
            CHECK(s.vision >= lo && s.vision < hi, "%s: dirección en ventana", de->d_name);
            CHECK(!vrc_is_sensor_func(cs, nc, s.vision), "%s: la prueba de soldados no es una cámara", de->d_name);
            if (s.eye_global) CHECK(s.eye_global >= 0x800C0000u && s.eye_global < 0x80100000u && !(s.eye_global & 3), "%s: global %08x", de->d_name, s.eye_global);
            if (s.gather) CHECK(s.mode_addr >= 0x800C0000u && s.o_events > 2900 && s.o_events < 3100, "%s: gather", de->d_name);
            if (getenv("VRP_VERBOSE")) printf("%-10s vision=%08x var=%s eye=%08x gather=%08x ev=%u mode=%08x | cam=%d\n", de->d_name, s.vision, s.lay->id, s.eye_global, s.gather, s.o_events, s.mode_addr, nc);
        } else if (getenv("VRP_VERBOSE") && nc) printf("%-10s (sin soldados) cam=%d\n", de->d_name, nc);
        if (getenv("VRP_VERBOSE")) for (int i = 0; i < nc; i++) printf("    cam fam=%d detect=%08x\n", cs[i].family, cs[i].detect);
        (void)nbad;
    }
    closedir(d);
    printf("overlays reales: %d leídas, %d con soldados (%d con alcance global, %d con recogida de eventos), %d sensores de cámara\n", nov, nvis, nglob, ngather, ncam);
    if (nov >= 90) { CHECK(nvis >= 31, "overlays con soldados: %d (<31 = regresión del localizador)", nvis); CHECK(ncam >= 32, "sensores de cámara: %d (<32)", ncam); }
    else if (nov) { CHECK(nvis >= 1, "pocas overlays con soldados: %d", nvis); }
}

int main(int argc, char **argv) {
    t_config(); t_rate(); t_curve(); t_decay_events(); t_periphery(); t_stance();
    t_wrap(); t_identity(); t_stale(); t_gather(); t_table(); t_fuzz();
    t_overlays(argc > 1 ? argv[1] : "../../build/ov");
    printf("%d comprobaciones OK, %d fallos\n", nok, nfail);
    return nfail != 0;
}
