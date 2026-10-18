/* Pruebas sintéticas de vr_reload (máquina de recarga): sin datos del juego. Un "juego falso" modela lo medido en el
 * juego real: el equipar crea el arma con el cargador lleno (capacidad, +1 si había bala), dos toques de R1 (>= 2 VBlanks
 * pulsado y >= 2 soltado, mando muestreado cada 2 VBlanks) desequipan y equipan, un toque de R2 refresca el arma, y con el
 * cargador vacío y reserva el juego recarga solo. Compilar con ASan/UBSan (make test-san). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_reload.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define R1 (1u << 11)
#define R2 (1u << 9)

/* ---- juego falso ---- */
typedef struct {
    int weapon, equipped_id;             /* arma equipada (-1 ninguna) y la que se re-equipa */
    int mag, mag_max, total;
    int cap;                             /* capacidad nominal (12) */
    int ok;
    int drop_taps;                       /* nº de toques R1 a ignorar (simula toques perdidos) */
    int never;                           /* 1: ignora todo (la recarga nunca ocurre) */
    int auto_reload_at;                  /* VBlank en que recarga sola (-1 = no) */
    int vb;
    unsigned pad_prev, pad;              /* mando muestreado cada 2 VBlanks */
    int r1_toggle_pending;
    int menu_t;                          /* VBlanks de menú abierto (R2) */
} Game;

static void game_equip(Game *g) {
    g->weapon = g->equipped_id;
    int tot = g->total, m = tot < g->cap ? tot : g->cap;
    g->mag_max = g->cap + (g->mag > 0 ? 1 : 0);
    g->mag = m + (g->mag > 0 ? 1 : 0); if (g->mag > tot) g->mag = tot;
    if (g->mag > g->mag_max) g->mag = g->mag_max;
}
static void game_tick(Game *g, unsigned pad_bits) {
    g->vb++;
    if (g->menu_t > 0 && --g->menu_t == 0) g->ok = 1;
    if (g->vb % 2 == 0) {                /* el mando se muestrea cada 2 VBlanks */
        unsigned press = pad_bits & ~g->pad_prev;
        g->pad_prev = pad_bits;
        if (!g->never && (press & R1)) {
            if (g->drop_taps > 0) g->drop_taps--;
            else if (g->weapon >= 0) { g->weapon = -1; }
            else game_equip(g);
        }
        if (!g->never && (press & R2) && g->weapon >= 0) { game_equip(g); g->menu_t = 3; g->ok = 0; }
    }
    if (g->weapon >= 0 && g->mag == 0 && g->total > 0 && g->auto_reload_at >= 0 && g->vb >= g->auto_reload_at) { game_equip(g); g->auto_reload_at = -1; }
}
static void game_read(const Game *g, VrReloadGame *o) {
    memset(o, 0, sizeof *o);
    o->ok = g->ok; o->weapon = g->weapon; o->mag = g->mag; o->mag_max = g->mag_max; o->total = g->weapon >= 0 ? g->total : 0; o->menu = g->menu_t > 0;
}
static Game newgame(int mag, int total) {
    Game g; memset(&g, 0, sizeof g);
    g.weapon = 0; g.equipped_id = 0; g.cap = 12; g.total = total; g.mag = mag; g.mag_max = 13; g.ok = 1; g.auto_reload_at = -1;
    return g;
}

/* Avanza un VBlank: lee el juego, ejecuta la máquina, aplica el mando al juego. Devuelve la salida y registra el ancho de pulsos. */
typedef struct { int w_press_min, w_gap_min, run, gap; unsigned last; } PulseLog;
static VrReloadOut step(Game *g, VrReloadIn in, PulseLog *pl) {
    VrReloadGame rg; game_read(g, &rg);
    VrReloadOut o; vrreload_frame(&rg, &in, &o);
    unsigned force = o.pad_force;
    if (pl) {
        if (force) { if (!pl->last && pl->gap > 0 && pl->gap < pl->w_gap_min) pl->w_gap_min = pl->gap; pl->run++; pl->gap = 0; }
        else { if (pl->last && pl->run > 0 && pl->run < pl->w_press_min) pl->w_press_min = pl->run; if (pl->last) pl->run = 0; pl->gap++; }
        pl->last = force ? 1 : 0;
    }
    game_tick(g, force);
    return o;
}
static PulseLog newlog(void) { PulseLog p; memset(&p, 0, sizeof p); p.w_press_min = p.w_gap_min = 1000; return p; }
static VrReloadIn in0(void) { VrReloadIn i = {0, 0, 1}; return i; }
static void run(Game *g, int n, VrReloadIn in, VrReloadOut *last, PulseLog *pl) { for (int i = 0; i < n; i++) { VrReloadOut o = step(g, in, pl); if (last) *last = o; } }

static void cfg_method(VrReloadMethod m, int taps) { VrReloadCfg c; vrreload_cfg_default(&c); c.method = m; c.taps = taps; vrreload_init(&c); }

/* ---- 1. recarga táctica con R1 doble ---- */
static void t_tactical(void) {
    cfg_method(VRR_VIA_R1_DOUBLE, 2);
    Game g = newgame(5, 60); VrReloadOut o; PulseLog pl = newlog();
    run(&g, 4, in0(), &o, &pl);
    CHECK(o.state == VRR_READY && o.fire_allowed, "partida en READY");
    CHECK(o.show_mag == 5 && o.show_reserve == 55, "munición mostrada = la del juego (%d/%d)", o.show_mag, o.show_reserve);
    CHECK(o.sync_rounds == 4 && o.sync_chambered == 1 && !o.sync_force, "tope de rondas VR = mag-1 (%d)", o.sync_rounds);
    VrReloadIn ie = in0(); ie.mag_in_edge = 1;
    VrReloadOut o1 = step(&g, ie, &pl);
    CHECK(o1.state == VRR_PRESS && o1.busy && o1.pad_mask == (R1 | R2) && !o1.fire_allowed, "empieza la maniobra al insertar el cargador");
    int seen_force = 0, forced_vbl = 0;
    for (int i = 0; i < 60 && vrreload_state() != VRR_READY; i++) { o = step(&g, in0(), &pl); if (o.pad_force) { seen_force = 1; forced_vbl++; CHECK(o.pad_force == R1, "solo R1"); } CHECK(o.busy || o.state == VRR_READY, "ocupado durante la maniobra"); }
    CHECK(seen_force && forced_vbl == 4, "dos toques de 2 VBlanks (%d)", forced_vbl);
    CHECK(pl.w_press_min >= 2 && pl.w_gap_min >= 2, "pulsos >= 2 VBlanks (%d) y huecos >= 2 (%d)", pl.w_press_min, pl.w_gap_min);
    CHECK(g.weapon == 0 && g.mag == 13, "el juego recargó: mag=%d", g.mag);
    o = step(&g, in0(), &pl);
    CHECK(o.state == VRR_READY && o.fire_allowed, "vuelve a READY");
    VrReloadStats st; vrreload_stats(&st);
    CHECK(st.sequences == 1 && st.ok == 1 && st.fails == 0 && st.retries == 0, "contadores %u %u %u %u", st.sequences, st.ok, st.fails, st.retries);
    CHECK(st.last_latency_vbl > 0 && st.last_latency_vbl < 20, "latencia %d VBlanks", st.last_latency_vbl);
    /* tras completar: se fijan las rondas del cargador VR a las del juego (una vez) */
}
static void t_adopt(void) {
    cfg_method(VRR_VIA_R1_DOUBLE, 2);
    Game g = newgame(5, 60); VrReloadOut o; PulseLog pl = newlog();
    run(&g, 4, in0(), &o, &pl);
    VrReloadIn ie = in0(); ie.mag_in_edge = 1; step(&g, ie, &pl);
    int forced = 0, k = 0;
    for (int i = 0; i < 60; i++) { o = step(&g, in0(), &pl); if (o.sync_force) { forced++; k = o.sync_rounds; CHECK(o.sync_chambered == 1, "chambered fijado"); } }
    CHECK(forced == 1 && k == 12, "fija exactamente una vez, rondas=%d (juego 13 = 12 + 1 en la recámara)", k);
}
/* ---- 2. el juego vacía y recarga solo: bloqueo hasta el gesto ---- */
static void t_empty_latch(void) {
    cfg_method(VRR_VIA_R1_DOUBLE, 2);
    Game g = newgame(1, 30); VrReloadOut o; PulseLog pl = newlog();
    run(&g, 3, in0(), &o, &pl);
    g.mag = 0; g.auto_reload_at = g.vb + 10;                 /* se disparó la última: cargador 0 */
    VrReloadIn nr = in0(); nr.ready = 0;                      /* el arma VR vacía: no lista */
    run(&g, 2, nr, &o, &pl);
    CHECK(o.state == VRR_EMPTY && !o.fire_allowed && o.show_mag == 0 && o.sync_rounds == 0 && o.sync_chambered == 0, "EMPTY bloquea");
    run(&g, 40, nr, &o, &pl);
    CHECK(g.mag == 13 || g.mag == 12, "el juego recargó solo (%d)", g.mag);
    CHECK(o.state == VRR_EMPTY && !o.fire_allowed && o.show_mag == 0, "sigue BLOQUEADO hasta el gesto aunque el juego ya recargó");
    CHECK(o.pad_force == 0 && !o.busy, "sin maniobra: el juego recarga solo");
    VrReloadIn ie = nr; ie.mag_in_edge = 1;                   /* cargador nuevo dentro, aún sin corredera */
    o = step(&g, ie, &pl);
    CHECK(o.state == VRR_EMPTY && o.pad_force == 0, "cargador dentro pero sin corredera: sigue bloqueado y sin maniobra");
    o = step(&g, in0(), &pl);                                 /* corredera: lista */
    CHECK(o.state == VRR_READY && o.fire_allowed && o.sync_force && o.sync_rounds == g.mag - 1 && o.show_mag == g.mag, "gesto completo -> READY con el cargador del juego (%d)", o.sync_rounds);
    CHECK(o.pad_force == 0, "no se pulsó nada");
    VrReloadStats st; vrreload_stats(&st);
    CHECK(st.empties == 1 && st.sequences == 0, "una vaciada, ninguna maniobra");
}
static void t_empty_no_reserve(void) {
    cfg_method(VRR_VIA_R1_DOUBLE, 2);
    Game g = newgame(0, 0); VrReloadOut o; PulseLog pl = newlog();
    VrReloadIn nr = in0(); nr.ready = 0;
    run(&g, 10, nr, &o, &pl);
    CHECK(o.state == VRR_EMPTY && !o.fire_allowed, "sin balas: EMPTY");
    run(&g, 400, in0(), &o, &pl);
    CHECK(o.state == VRR_EMPTY && !o.fire_allowed && o.pad_force == 0 && !o.busy, "sin reserva no hay maniobra ni desbloqueo");
    CHECK(o.show_mag == 0 && o.show_reserve == 0, "pantalla 0/0");
}
/* el arma VR sigue "lista" (desincronizada) cuando el juego se vacía: no se desbloquea sin ver el gesto (no lista -> lista) */
static void t_empty_needs_gesture(void) {
    cfg_method(VRR_VIA_R1_DOUBLE, 2);
    Game g = newgame(1, 30); VrReloadOut o; PulseLog pl = newlog();
    run(&g, 3, in0(), &o, &pl);
    g.mag = 0; g.auto_reload_at = g.vb + 10;
    run(&g, 80, in0(), &o, &pl);                              /* ready = 1 todo el rato */
    CHECK(g.mag > 0 && o.state == VRR_EMPTY && !o.fire_allowed, "juego recargado pero sin gesto: bloqueado (%d)", o.state);
    VrReloadIn nr = in0(); nr.ready = 0; run(&g, 3, nr, &o, &pl);
    o = step(&g, in0(), &pl);
    CHECK(o.state == VRR_READY && o.fire_allowed, "tras el gesto se desbloquea");
}
/* el gesto termina y el juego sigue vacío con reserva (no recarga solo): se provoca con la maniobra */
static void t_empty_forced(void) {
    cfg_method(VRR_VIA_R1_DOUBLE, 2);
    Game g = newgame(0, 40); g.auto_reload_at = -1; VrReloadOut o; PulseLog pl = newlog();
    VrReloadIn nr = in0(); nr.ready = 0;
    run(&g, 5, nr, &o, &pl);
    CHECK(o.state == VRR_EMPTY, "EMPTY");
    int pressed = 0;
    for (int i = 0; i < 400; i++) { o = step(&g, in0(), &pl); if (o.pad_force) pressed++; if (o.state == VRR_READY) break; }
    CHECK(pressed == 4 && g.mag > 0 && o.state == VRR_READY, "tras esperar el autoreload se provoca con R1 doble (pulsaciones %d, mag %d)", pressed, g.mag);
}
/* ---- 3. insertar con el cargador del juego lleno / sin reserva: nada que hacer ---- */
static void t_nothing_to_do(void) {
    cfg_method(VRR_VIA_R1_DOUBLE, 2);
    Game g = newgame(13, 60); VrReloadOut o; PulseLog pl = newlog();
    run(&g, 3, in0(), &o, &pl);
    VrReloadIn ie = in0(); ie.mag_in_edge = 1;
    o = step(&g, ie, &pl);
    CHECK(o.state == VRR_READY && !o.busy && o.pad_force == 0, "cargador lleno: no se pulsa nada");
    Game h = newgame(4, 4); run(&h, 3, in0(), &o, &pl);
    o = step(&h, ie, &pl);
    CHECK(o.state == VRR_READY && !o.busy, "sin reserva (total = cargador): no se pulsa nada");
}
/* ---- 4. toques perdidos: se vuelve a equipar; y si nada funciona se abandona ---- */
static void t_lost_tap(void) {
    cfg_method(VRR_VIA_R1_DOUBLE, 2);
    Game g = newgame(5, 60); g.drop_taps = 1;                 /* el primer toque se pierde: el segundo desequipa y nadie equipa */
    VrReloadOut o; PulseLog pl = newlog();
    run(&g, 3, in0(), &o, &pl);
    VrReloadIn ie = in0(); ie.mag_in_edge = 1; step(&g, ie, &pl);
    int presses = 0;
    for (int i = 0; i < 300 && vrreload_state() != VRR_READY; i++) { o = step(&g, in0(), &pl); if (o.pad_force) presses++; }
    VrReloadStats st; vrreload_stats(&st);
    CHECK(g.weapon == 0 && g.mag == 13, "se recuperó: arma=%d mag=%d", g.weapon, g.mag);
    CHECK(st.retries == 1 && st.ok == 1 && st.fails == 0, "un reintento (%u %u %u)", st.retries, st.ok, st.fails);
    CHECK(presses >= 6, "pulsaciones %d", presses);
}
static void t_never(void) {
    cfg_method(VRR_VIA_R1_DOUBLE, 2);
    Game g = newgame(5, 60); g.never = 1;
    VrReloadOut o; PulseLog pl = newlog();
    run(&g, 3, in0(), &o, &pl);
    VrReloadIn ie = in0(); ie.mag_in_edge = 1; step(&g, ie, &pl);
    int n = 0, presses = 0;
    for (n = 0; n < 2000 && vrreload_state() != VRR_READY; n++) { o = step(&g, in0(), &pl); if (o.pad_force) presses++; }
    VrReloadStats st; vrreload_stats(&st);
    CHECK(vrreload_state() == VRR_READY && st.fails == 1 && st.ok == 0, "abandona (fails=%u) sin atascarse (%d VBlanks)", st.fails, n);
    CHECK(presses <= 40, "no martillea el mando: %d pulsaciones", presses);
    o = step(&g, in0(), &pl);
    CHECK(o.state == VRR_READY && !o.busy, "tras abandonar el mando queda libre");
}
/* ---- 5. R2 (un toque basta; el menú del juego hace ok=0 unos VBlanks: la maniobra espera) ---- */
static void t_r2(void) {
    cfg_method(VRR_VIA_R2_TAPS, 2);
    Game g = newgame(5, 60); VrReloadOut o; PulseLog pl = newlog();
    run(&g, 3, in0(), &o, &pl);
    VrReloadIn ie = in0(); ie.mag_in_edge = 1; step(&g, ie, &pl);
    int presses = 0, others = 0;
    for (int i = 0; i < 200 && vrreload_state() != VRR_READY; i++) { o = step(&g, in0(), &pl); if (o.pad_force == R2) presses++; else if (o.pad_force) others++; }
    CHECK(vrreload_state() == VRR_READY && g.mag == 13, "recargó con R2 (mag %d)", g.mag);
    CHECK(others == 0 && presses == 4, "solo R2, 2 toques de 2 VBlanks (%d)", presses);
    CHECK(pl.w_press_min >= 2 && pl.w_gap_min >= 2, "anchos mínimos %d/%d", pl.w_press_min, pl.w_gap_min);
    cfg_method(VRR_VIA_R2_TAPS, 1);
    Game h = newgame(5, 60); presses = 0; step(&h, in0(), &pl); step(&h, ie, &pl);
    for (int i = 0; i < 200 && vrreload_state() != VRR_READY; i++) { o = step(&h, in0(), &pl); if (o.pad_force) presses++; }
    CHECK(h.mag == 13 && presses == 2, "con un toque (%d VBlanks pulsado)", presses);
}
/* ---- 6. contabilidad virtual y modos ---- */
static void t_ledger(void) {
    cfg_method(VRR_VIA_R1_DOUBLE, 2);
    Game g = newgame(5, 60); VrReloadOut o; PulseLog pl = newlog();
    run(&g, 3, in0(), &o, &pl);
    WGameBridge b; memset(&b, 0, sizeof b); vrreload_bridge_fill(&b);
    CHECK(b.ammo_reserve(0) == 55 && b.ammo_reserve(1) == 0, "reserva = total - cargador (%d)", b.ammo_reserve(0));
    b.ammo_take(0, 12);
    CHECK(b.ammo_reserve(0) == 43, "cargador cogido de la cintura: la reserva baja (%d)", b.ammo_reserve(0));
    b.ammo_take(0, -12);
    CHECK(b.ammo_reserve(0) == 55, "devuelto");
    b.ammo_take(0, -50); CHECK(b.ammo_reserve(0) == 55, "el libro no es negativo");
    b.ammo_take(0, 12);
    VrReloadIn ie = in0(); ie.mag_in_edge = 1; step(&g, ie, &pl);
    for (int i = 0; i < 60 && vrreload_state() != VRR_READY; i++) step(&g, in0(), &pl);
    o = step(&g, in0(), &pl);
    CHECK(o.show_reserve == 60 - 13 && b.ammo_reserve(0) == 47, "tras recargar el libro se pone a 0 y la reserva sale del juego (%d)", o.show_reserve);
    b.ammo_take(2, 5); CHECK(b.ammo_reserve(0) == 47, "otra arma no cuenta");
}
static void t_ledger_switch(void) {
    cfg_method(VRR_VIA_R1_DOUBLE, 2);
    Game g = newgame(5, 60); VrReloadOut o; PulseLog pl = newlog();
    run(&g, 3, in0(), &o, &pl);
    WGameBridge b; memset(&b, 0, sizeof b); vrreload_bridge_fill(&b);
    b.ammo_take(0, 12); CHECK(b.ammo_reserve(0) == 43, "cargador cogido");
    g.weapon = 1; run(&g, 2, in0(), &o, &pl);                 /* el jugador cambia de arma */
    g.weapon = 0; run(&g, 2, in0(), &o, &pl);
    CHECK(b.ammo_reserve(0) == 55, "cambiar de arma anula lo cogido de la anterior (%d)", b.ammo_reserve(0));
}
static void t_modes_weapon(void) {
    cfg_method(VRR_VIA_R1_DOUBLE, 2);
    Game g = newgame(5, 60); VrReloadOut o; PulseLog pl = newlog();
    VrReloadIn in = in0(); in.weapon_id = 1;                  /* el arma VR mostrada no es la equipada en el juego */
    run(&g, 10, in, &o, &pl);
    CHECK(o.state == VRR_OFF && !o.fire_allowed && o.pad_force == 0, "arma VR distinta de la del juego: apagado");
    in.mag_in_edge = 1; o = step(&g, in, &pl);
    CHECK(o.state == VRR_OFF && !o.busy, "sin arma coincidente no se pulsa nada");
    in.weapon_id = -1; o = step(&g, in, &pl); CHECK(o.state == VRR_OFF, "sin arma VR");
    g.ok = 0; in.weapon_id = 0; in.mag_in_edge = 0; o = step(&g, in, &pl); CHECK(o.state == VRR_OFF && !o.fire_allowed, "jugador no controlable");
}
/* ---- 7. robustez: entradas aleatorias y degeneradas ---- */
static void t_fuzz(void) {
    srand(1234);
    for (int m = 0; m < 2; m++) {
        cfg_method(m ? VRR_VIA_R2_TAPS : VRR_VIA_R1_DOUBLE, 1 + m);
        for (int it = 0; it < 200000; it++) {
            VrReloadGame g; memset(&g, 0, sizeof g);
            g.ok = rand() % 8 != 0; g.weapon = (rand() % 12) - 2; g.mag = (rand() % 5 == 0) ? (rand() % 100000) - 50000 : rand() % 14;
            g.mag_max = (rand() % 5 == 0) ? (rand() % 100000) - 50000 : rand() % 14; g.total = (rand() % 5 == 0) ? (rand() % 100000) - 50000 : rand() % 200; g.menu = rand() & 1;
            VrReloadIn in = {(rand() % 12) - 2, rand() % 5 == 0, rand() & 1};
            VrReloadOut o; vrreload_frame(rand() % 500 ? &g : NULL, rand() % 500 ? &in : NULL, &o);
            CHECK((o.pad_force & ~(R1 | R2)) == 0, "pad_force fuera de R1/R2");
            CHECK(o.state >= VRR_OFF && o.state <= VRR_VERIFY, "estado válido");
            CHECK(o.show_mag >= 0 && o.show_reserve >= 0, "pantalla no negativa");
            CHECK(o.sync_rounds >= -1 && o.sync_chambered >= -1, "sync válido");
            CHECK(!o.busy || (o.pad_mask == (R1 | R2)), "ocupado -> máscara");
            CHECK(!(o.busy && o.fire_allowed), "ocupado no dispara");
            CHECK(o.busy || o.pad_force == 0, "sin maniobra no hay toques sueltos");
            if (it % 7 == 0) { int r = vrreload_ammo_reserve(g.weapon); CHECK(r >= 0, "reserva >= 0"); vrreload_ammo_take(g.weapon, (rand() % 2000) - 1000); }
        }
    }
    VrReloadOut o; vrreload_frame(NULL, NULL, &o); CHECK(o.pad_force == 0 && !o.busy, "NULL seguro");
    VrReloadCfg c; memset(&c, 0, sizeof c); c.press_vbl = -5; c.gap_vbl = 0; c.taps = 99; c.verify_vbl = 0; c.max_retries = -3; vrreload_init(&c);
    CHECK(vrreload_config()->press_vbl >= 2 && vrreload_config()->gap_vbl >= 2 && vrreload_config()->taps <= 4 && vrreload_config()->max_retries >= 0, "configuración saneada");
    vrreload_init(NULL);
    CHECK(vrreload_config()->press_vbl == 2, "defecto");
}

int main(void) {
    t_tactical(); t_adopt(); t_empty_latch(); t_empty_no_reserve(); t_empty_needs_gesture(); t_empty_forced(); t_nothing_to_do(); t_lost_tap(); t_never();
    t_r2(); t_ledger(); t_ledger_switch(); t_modes_weapon(); t_fuzz();
    printf("test_reload: %d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
