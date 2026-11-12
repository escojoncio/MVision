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
static VrReloadIn in0(void) { VrReloadIn i = {0, 0, 1, 0}; return i; }
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
/* ---- 3b. RECOGER MUNICIÓN con el arma seca (log del visor 20261112_000750, [arma] líneas 391-451 de las con esa etiqueta): disparo 13 -> municion=0 cargador_juego=0 reserva=0 (EMPTY);
 * recoge 12: municion=12 cargador_juego=0 reserva=12; recoge otras 12: municion=24 cargador_juego=0 reserva=24; el juego NO carga su cargador (decompilación: item.c add_ammo solo suma a
 * GM_Weapons; sna_init.c sub_80057BF0 recarga solo con PLAYER_ATTACK y el disparo VR bloqueado nunca lo activa) y el arma VR se quedaba sin cargador (cargador=0 recamara=0 balas_vr=0) con
 * recarga=2 (EMPTY) durante todo el resto de la sesión. Debe quedar LISTA sin gesto del usuario (aquí el arma VR sigue sin cargador: ready = 0, no_mag = 1 todo el rato). */
static void t_pickup_dry(void) {
    cfg_method(VRR_VIA_R1_DOUBLE, 2);
    Game g = newgame(1, 1); VrReloadOut o; PulseLog pl = newlog();
    VrReloadIn dry = in0(); dry.ready = 0; dry.no_mag = 1;
    run(&g, 5, in0(), &o, &pl);
    g.mag = 0; g.total = 0;                                     /* último disparo: municion=0 cargador_juego=0 */
    run(&g, 60, dry, &o, &pl);
    CHECK(o.state == VRR_EMPTY && !o.fire_allowed && o.show_mag == 0 && o.show_reserve == 0 && o.pad_force == 0 && !o.busy, "sin balas: EMPTY, nada que hacer (estado %d)", o.state);
    g.total = 12;                                               /* recoge 12: municion=12 cargador_juego=0 (el juego no toca el cargador) */
    int arms = 0, presses = 0, ready_vbl = -1;
    for (int i = 0; i < 400; i++) {
        if (i == 10) g.total = 24;                              /* segunda recogida antes de que el juego cargue: municion=24 */
        o = step(&g, dry, &pl);
        if (o.pad_force) presses++;
        if (o.arm_ready) { arms++; if (ready_vbl < 0) ready_vbl = i; CHECK(o.state == VRR_READY, "arm_ready solo en READY (%d)", o.state); }
        if (o.state == VRR_READY && ready_vbl >= 0) break;
    }
    CHECK(g.weapon == 0 && g.mag == 12, "el juego cargó su cargador (%d): R1 doble sobre mag 0 con 24 balas -> min(12, 24)", g.mag);
    CHECK(presses == 4, "R1 doble (pulsado %d VBlanks)", presses);
    CHECK(arms == 1 && ready_vbl >= 0 && ready_vbl < 120, "el arma VR queda lista una vez, en %d VBlanks", ready_vbl);
    CHECK(o.state == VRR_READY && o.fire_allowed && o.show_mag == 12 && o.show_reserve == 12, "READY con 12/12 (%d/%d)", o.show_mag, o.show_reserve);
    CHECK(o.sync_rounds == 11 && o.sync_chambered == 1 && o.sync_force, "cuenta del arma VR = cargador del juego - 1 en el cargador + 1 en la recámara (%d)", o.sync_rounds);
    VrReloadIn rd = in0(); rd.ready = 1;                        /* el anfitrión hizo make_ready: sin más rearmes ni maniobras */
    run(&g, 200, rd, &o, &pl);
    CHECK(o.state == VRR_READY && !o.arm_ready && o.pad_force == 0, "estable tras el rearme");
    VrReloadStats st; vrreload_stats(&st);
    CHECK(st.pickups == 1 && st.arms == 1 && st.empty_runs == 1, "contadores recogidas=%u rearmes=%u maniobras=%u", st.pickups, st.arms, st.empty_runs);
    CHECK(vrreload_pickup_pending() == 0, "sin recogida pendiente");
}
/* ---- 3c. RECOGIDA con el arma seca cuando la maniobra R1 NO recarga (log del visor 20261112_111703, [arma] vb~38096-38202: la primera maniobra no cargó nada, 106 VBlanks con el gatillo muerto):
 * el cargador del juego se carga directamente (VrReloadOut.load_mag = min(GM_MagazineMax, GM_Weapons), lo mismo que hace el juego en sna_init.c sna_80057A90 con time == 0), sin toques del mando. */
static void t_pickup_direct(void) {
    cfg_method(VRR_VIA_R1_DOUBLE, 2);
    Game g = newgame(1, 1); g.never = 1; VrReloadOut o; PulseLog pl = newlog();
    VrReloadIn dry = in0(); dry.ready = 0; dry.no_mag = 1;
    run(&g, 5, in0(), &o, &pl);
    g.mag = 0; g.total = 0; g.mag_max = 12;                     /* el último disparo deja GM_MagazineMax = 12 (weapon/socom.c:359) */
    run(&g, 60, dry, &o, &pl);
    CHECK(o.state == VRR_EMPTY && o.load_mag == 0, "sin balas: nada que cargar (%d)", o.load_mag);
    g.total = 30;
    int presses = 0, ready_vbl = -1;
    for (int i = 0; i < 10 && ready_vbl < 0; i++) {
        o = step(&g, dry, &pl);
        if (o.pad_force) presses++;
        if (o.load_mag > 0) { CHECK(o.load_mag == 12, "carga = min(capacidad 12, total 30) (%d)", o.load_mag); g.mag = o.load_mag; }   /* el anfitrión lo escribe en GM_Magazine */
        if (o.arm_ready) ready_vbl = i;
    }
    CHECK(ready_vbl >= 0 && ready_vbl <= 2 && presses == 0 && o.state == VRR_READY && o.fire_allowed, "lista en %d VBlanks sin maniobra (toques %d, estado %d)", ready_vbl, presses, o.state);
    CHECK(o.sync_rounds == 11 && o.sync_chambered == 1 && o.load_mag == 0, "cuenta del arma VR 11+1 (%d)", o.sync_rounds);
    /* recogida con el jugador sin control o el arma VR sin mostrar (el flanco se pierde): al volver, munición sin cargar con el arma seca = lo mismo */
    g.mag = 0; g.total = 0; run(&g, 60, dry, &o, &pl);
    g.ok = 0; run(&g, 3, dry, &o, &pl); g.total = 5; run(&g, 3, dry, &o, &pl); g.ok = 1;
    ready_vbl = -1;
    for (int i = 0; i < 10 && ready_vbl < 0; i++) {
        o = step(&g, dry, &pl);
        if (o.load_mag > 0) { CHECK(o.load_mag == 5, "carga = total 5 (%d)", o.load_mag); g.mag = o.load_mag; }
        if (o.arm_ready) ready_vbl = i;
    }
    CHECK(ready_vbl >= 0 && ready_vbl <= 3 && o.state == VRR_READY && o.sync_rounds == 4, "recogida sin control: lista en %d VBlanks (estado %d)", ready_vbl, o.state);
    /* recogida con el arma VR sin mostrar (seguimiento perdido): igual */
    g.mag = 0; g.total = 0; run(&g, 60, dry, &o, &pl);
    VrReloadIn hid = dry; hid.weapon_id = -1; run(&g, 2, hid, &o, &pl); g.total = 7; run(&g, 2, hid, &o, &pl);
    ready_vbl = -1;
    for (int i = 0; i < 10 && ready_vbl < 0; i++) { o = step(&g, dry, &pl); if (o.load_mag > 0) g.mag = o.load_mag; if (o.arm_ready) ready_vbl = i; }
    CHECK(ready_vbl >= 0 && ready_vbl <= 3 && o.state == VRR_READY && g.mag == 7, "recogida con el arma VR oculta: lista en %d VBlanks (cargador %d)", ready_vbl, g.mag);
    /* francotirador: el juego recarga siempre 5 (sna_init.c:6098-6106) aunque GM_MagazineMax sea 6 (rifle.c:205) */
    cfg_method(VRR_VIA_R1_DOUBLE, 2);
    Game r = newgame(1, 1); r.never = 1; r.weapon = r.equipped_id = 9; r.mag_max = 6;
    VrReloadIn rin = in0(); rin.weapon_id = 9; VrReloadIn rdry = rin; rdry.ready = 0; rdry.no_mag = 1;
    run(&r, 5, rin, &o, &pl); r.mag = 0; r.total = 0; run(&r, 60, rdry, &o, &pl);
    r.total = 20; o = step(&r, rdry, &pl);
    CHECK(o.load_mag == 5, "francotirador: carga 5 (%d)", o.load_mag);
    r.mag = 5; r.ok = 0; run(&r, 5, rdry, &o, &pl); r.ok = 1; o = step(&r, rdry, &pl);       /* pausa justo tras cargar: el rearme pendiente es del arma 9 */
    CHECK(o.arm_ready && o.state == VRR_READY, "francotirador: pausa tras cargar, rearma al volver (estado %d)", o.state);
}
/* SIN recogida: cargador agotado con reserva (el gesto de recarga es obligatorio, t_empty_needs_gesture). Una pausa/menú/códec o el arma VR oculta un VBlank NO la dejan lista ni cargan nada. */
static void t_pause_keeps_gesture(void) {
    cfg_method(VRR_VIA_R1_DOUBLE, 2);
    Game g = newgame(1, 30); g.never = 1; VrReloadOut o; PulseLog pl = newlog();
    VrReloadIn dry = in0(); dry.ready = 0; dry.no_mag = 1;
    run(&g, 5, in0(), &o, &pl);
    g.mag = 0;
    run(&g, 10, dry, &o, &pl);
    g.ok = 0; run(&g, 3, dry, &o, &pl); g.ok = 1;
    int arms = 0, loads = 0;
    for (int i = 0; i < 6; i++) { o = step(&g, dry, &pl); if (o.load_mag > 0) { loads++; g.mag = o.load_mag; } arms += o.arm_ready; }
    CHECK(o.state == VRR_EMPTY && !o.fire_allowed && arms == 0 && loads == 0, "pausa con el arma seca y reserva, sin recogida: sigue EMPTY (estado %d, rearmes %d, cargas %d)", o.state, arms, loads);
    VrReloadIn hid = dry; hid.weapon_id = -1; run(&g, 1, hid, &o, &pl);
    for (int i = 0; i < 6; i++) { o = step(&g, dry, &pl); if (o.load_mag > 0) { loads++; g.mag = o.load_mag; } arms += o.arm_ready; }
    CHECK(o.state == VRR_EMPTY && !o.fire_allowed && arms == 0 && loads == 0, "arma VR oculta 1 VBlank, sin recogida: sigue EMPTY (estado %d, rearmes %d, cargas %d)", o.state, arms, loads);
    /* una recogida perdida de OTRA arma no vale para esta */
    hid.weapon_id = -1; g.weapon = 1; g.total = 0; run(&g, 2, hid, &o, &pl); g.total = 50; run(&g, 2, hid, &o, &pl); g.weapon = 0; g.total = 30;
    for (int i = 0; i < 6; i++) { o = step(&g, dry, &pl); if (o.load_mag > 0) { loads++; g.mag = o.load_mag; } arms += o.arm_ready; }
    CHECK(o.state == VRR_EMPTY && arms == 0 && loads == 0, "recogida de otra arma: sigue EMPTY (estado %d, rearmes %d, cargas %d)", o.state, arms, loads);
}
/* ---- 3d. ventanas de 1 VBlank de la recogida con el arma seca (cada una dejaba la pistola muerta hasta re-seleccionarla). El anfitrión escribe load_mag en el cargador del juego. ---- */
typedef struct { int arms, loads, presses; VrReloadOut o; } Acc;
static void runl(Game *g, int n, VrReloadIn in, Acc *a) {
    for (int i = 0; i < n; i++) { a->o = step(g, in, NULL); if (a->o.load_mag > 0) { a->loads++; g->mag = a->o.load_mag; } a->arms += a->o.arm_ready; if (a->o.pad_force) a->presses++; }
}
static Game drygame(int mag, int total) { Game g = newgame(mag, total); g.never = 1; g.mag_max = 12; return g; }
static void t_pickup_windows(void) {
    VrReloadIn rdy = in0(), dry = in0(), hid = in0(); dry.ready = 0; dry.no_mag = 1; hid.weapon_id = -1; hid.ready = 0;
    Acc a;
    /* S1: la recogida llega en el mismo VBlank que el último disparo (READY -> EMPTY) */
    { cfg_method(VRR_VIA_R1_DOUBLE, 2); Game g = drygame(1, 1); memset(&a, 0, sizeof a); runl(&g, 5, rdy, &a);
      g.mag = 0; g.total = 12; runl(&g, 4, dry, &a);
      CHECK(a.o.state == VRR_READY && a.arms == 1 && a.loads == 1 && a.presses == 0 && g.mag == 12, "S1 recogida en el VBlank del último disparo: lista (estado %d, rearmes %d, cargas %d)", a.o.state, a.arms, a.loads); }
    /* S3: cargador escrito y el VBlank siguiente sin control (pausa): al volver el arma VR se rearma (no basta READY: sigue sin cargador) */
    { cfg_method(VRR_VIA_R1_DOUBLE, 2); Game g = drygame(0, 0); memset(&a, 0, sizeof a); runl(&g, 5, dry, &a);
      g.total = 12; runl(&g, 1, dry, &a); CHECK(a.loads == 1 && a.arms == 0, "S3: cargado, aún sin rearmar");
      g.ok = 0; runl(&g, 30, dry, &a); g.ok = 1; runl(&g, 4, dry, &a);
      CHECK(a.o.state == VRR_READY && a.arms == 1, "S3 pausa justo tras cargar: rearma al volver (estado %d, rearmes %d)", a.o.state, a.arms); }
    /* S3b: lo mismo con el arma VR oculta (con control) */
    { cfg_method(VRR_VIA_R1_DOUBLE, 2); Game g = drygame(0, 0); memset(&a, 0, sizeof a); runl(&g, 5, dry, &a);
      g.total = 12; runl(&g, 1, dry, &a); runl(&g, 30, hid, &a); runl(&g, 4, dry, &a);
      CHECK(a.o.state == VRR_READY && a.arms == 1 && a.loads == 1, "S3b arma oculta justo tras cargar: rearma al volver (estado %d, rearmes %d)", a.o.state, a.arms);
      /* ...pero no si entretanto se cambió de arma y se volvió (el arma se crea de nuevo: cuenta del juego, sin rearme pendiente) */
      cfg_method(VRR_VIA_R1_DOUBLE, 2); g = drygame(0, 0); memset(&a, 0, sizeof a); runl(&g, 5, dry, &a);
      g.total = 12; runl(&g, 1, dry, &a); runl(&g, 2, hid, &a); g.weapon = 1; runl(&g, 3, hid, &a); g.weapon = 0; g.mag = 0; g.total = 30; runl(&g, 6, dry, &a);
      CHECK(a.o.state == VRR_EMPTY && a.arms == 0 && a.loads == 1, "S3b con cambio de arma por medio: sin rearme pendiente (estado %d, rearmes %d, cargas %d)", a.o.state, a.arms, a.loads); }
    /* P6: recogida sin control durante la maniobra de EMPTY, que falla: al volver carga y rearma; una pausa posterior no vuelve a rearmar */
    { cfg_method(VRR_VIA_R1_DOUBLE, 2); Game g = drygame(0, 5); memset(&a, 0, sizeof a);
      for (int i = 0; i < 400 && !a.o.busy; i++) runl(&g, 1, dry, &a);
      CHECK(a.o.busy, "P6: maniobra de EMPTY en curso");
      g.ok = 0; g.total = 17; runl(&g, 10, dry, &a); g.ok = 1; runl(&g, 1500, dry, &a);
      CHECK(a.o.state == VRR_READY && a.arms == 1 && a.loads == 1 && g.mag == 12 && !vrreload_pickup_pending(), "P6 recogida sin control en la maniobra: lista (estado %d, rearmes %d, cargas %d)", a.o.state, a.arms, a.loads);
      g.mag = 0; runl(&g, 10, dry, &a); g.ok = 0; runl(&g, 60, dry, &a); g.ok = 1; runl(&g, 20, dry, &a);
      CHECK(a.o.state == VRR_EMPTY && a.arms == 1 && a.loads == 1, "P6 vaciado con reserva y pausa posterior: sigue EMPTY (estado %d, rearmes %d, cargas %d)", a.o.state, a.arms, a.loads); }
    /* (a) recogida sin control, el juego carga solo (el arma VR seca se rearma una vez al volver), se vacía con reserva, pausa: sigue EMPTY sin rearme (el cerrojo no sobrevive a READY) */
    { cfg_method(VRR_VIA_R1_DOUBLE, 2); Game g = drygame(0, 0); memset(&a, 0, sizeof a); runl(&g, 5, dry, &a);
      g.ok = 0; runl(&g, 3, dry, &a); g.total = 30; runl(&g, 3, dry, &a); g.mag = 12; runl(&g, 3, dry, &a); g.ok = 1; runl(&g, 5, dry, &a);
      CHECK(a.o.state == VRR_READY && a.loads == 0 && a.arms == 1, "(a) el juego cargó solo: READY y arma VR rearmada una vez (estado %d, rearmes %d)", a.o.state, a.arms);
      int arms0 = a.arms; g.mag = 0; runl(&g, 10, dry, &a); g.ok = 0; runl(&g, 5, dry, &a); g.ok = 1; runl(&g, 10, dry, &a);
      CHECK(a.o.state == VRR_EMPTY && a.arms == arms0 && a.loads == 0, "(a) vaciado con reserva y pausa: sigue EMPTY (estado %d, rearmes %d, cargas %d)", a.o.state, a.arms - arms0, a.loads); }
    /* (a3) el cerrojo no sobrevive a READY: recogida sin control con el cargador a 0 en mitad de una recarga táctica; después se vacía con reserva: EMPTY sin carga ni rearme */
    { cfg_method(VRR_VIA_R1_DOUBLE, 2); Game g = drygame(5, 60); memset(&a, 0, sizeof a); runl(&g, 5, rdy, &a);
      VrReloadIn ins = rdy; ins.mag_in_edge = 1; runl(&g, 1, ins, &a); runl(&g, 8, rdy, &a);
      CHECK(a.o.state == VRR_VERIFY, "(a3) maniobra táctica en verificación (estado %d)", a.o.state);
      g.ok = 0; g.mag = 0; runl(&g, 1, rdy, &a); g.total = 70; runl(&g, 2, rdy, &a); g.ok = 1; g.mag = 12; runl(&g, 5, rdy, &a);
      CHECK(a.o.state == VRR_READY && a.arms == 0, "(a3) recarga táctica terminada: READY (estado %d, rearmes %d)", a.o.state, a.arms);
      g.mag = 0; runl(&g, 10, dry, &a);
      CHECK(a.o.state == VRR_EMPTY && a.arms == 0 && a.loads == 0, "(a3) vaciado con reserva: EMPTY, espera el gesto (estado %d, rearmes %d, cargas %d)", a.o.state, a.arms, a.loads); }
    /* (d) rearme pendiente y, en la pausa, se cambia a otra arma de cargador: la otra no se rearma */
    { cfg_method(VRR_VIA_R1_DOUBLE, 2); Game g = drygame(0, 0); memset(&a, 0, sizeof a); runl(&g, 5, dry, &a);
      g.total = 12; runl(&g, 1, dry, &a); g.ok = 0; runl(&g, 5, dry, &a); g.weapon = 1; g.mag = 25; g.mag_max = 25; g.total = 100; runl(&g, 5, dry, &a); g.ok = 1;
      VrReloadIn fam = rdy; fam.weapon_id = 1; runl(&g, 5, fam, &a);
      CHECK(a.o.state == VRR_READY && a.arms == 0, "(d) otra arma tras la pausa: sin rearme (estado %d, rearmes %d)", a.o.state, a.arms); }
    /* (e) rearme pendiente y carga de zona en la pausa (el juego sin cargador: capacidad 0): no se rearma un arma sin balas */
    { cfg_method(VRR_VIA_R1_DOUBLE, 2); Game g = drygame(0, 0); memset(&a, 0, sizeof a); runl(&g, 5, dry, &a);
      g.total = 12; runl(&g, 1, dry, &a); g.ok = 0; runl(&g, 5, dry, &a); g.mag = 0; g.mag_max = 0; g.ok = 1; runl(&g, 5, dry, &a);
      CHECK(a.arms == 0 && !a.o.fire_allowed, "(e) sin cargador en el juego al volver: sin rearme (estado %d, rearmes %d)", a.o.state, a.arms); }
    /* (f) arma seca con reserva esperando el gesto (el juego ya recargó su cargador); recogida durante una pausa: el gesto sigue siendo obligatorio (sin rearme) */
    { cfg_method(VRR_VIA_R1_DOUBLE, 2); Game g = drygame(1, 30); memset(&a, 0, sizeof a); runl(&g, 5, rdy, &a);
      g.mag = 0; runl(&g, 3, dry, &a); g.mag = 12; runl(&g, 3, dry, &a);
      CHECK(a.o.state == VRR_EMPTY, "(f) espera el gesto (estado %d)", a.o.state);
      g.ok = 0; runl(&g, 3, dry, &a); g.total = 42; runl(&g, 3, dry, &a); g.ok = 1; runl(&g, 10, dry, &a);
      CHECK(a.arms == 0 && a.loads == 0, "(f) recogida en pausa con el cargador del juego cargado: sin rearme (rearmes %d)", a.arms); }
    /* (g) S3 con el juego dando «sin arma» durante la pausa (escena): al volver con la misma arma, rearma; dos pausas seguidas no rearman dos veces */
    { cfg_method(VRR_VIA_R1_DOUBLE, 2); Game g = drygame(0, 0); memset(&a, 0, sizeof a); runl(&g, 5, dry, &a);
      g.total = 12; runl(&g, 1, dry, &a); g.ok = 0; g.weapon = -1; runl(&g, 30, dry, &a); g.weapon = 0; g.ok = 1; runl(&g, 1, dry, &a);
      CHECK(a.o.state == VRR_READY && a.arms == 1, "(g) pausa sin arma tras cargar: rearma al volver (estado %d, rearmes %d)", a.o.state, a.arms);
      g.ok = 0; runl(&g, 5, rdy, &a); g.ok = 1; runl(&g, 5, rdy, &a);
      CHECK(a.arms == 1, "(g) otra pausa inmediata: sin segundo rearme (%d)", a.arms); }
    /* (h) rearmada en EMPTY y pausa en el VBlank siguiente: sin segundo rearme */
    { cfg_method(VRR_VIA_R1_DOUBLE, 2); Game g = drygame(0, 0); memset(&a, 0, sizeof a); runl(&g, 5, dry, &a);
      g.total = 12; runl(&g, 2, dry, &a); CHECK(a.arms == 1, "(h) rearmada");
      g.ok = 0; runl(&g, 5, rdy, &a); g.ok = 1; runl(&g, 5, rdy, &a);
      CHECK(a.arms == 1 && a.o.state == VRR_READY, "(h) pausa tras rearmar: sin segundo rearme (%d)", a.arms); }
    /* (i) recogida sin control durante los toques de la maniobra de EMPTY con el cargador del juego ya cargado: como (f), el gesto sigue siendo obligatorio */
    { cfg_method(VRR_VIA_R1_DOUBLE, 2); Game g = drygame(0, 5); memset(&a, 0, sizeof a);
      for (int i = 0; i < 400 && !a.o.busy; i++) runl(&g, 1, dry, &a);
      g.mag = 12; g.ok = 0; g.total = 17; runl(&g, 3, dry, &a); CHECK(a.o.state == VRR_PRESS, "(i) en los toques (estado %d)", a.o.state);
      g.ok = 1; runl(&g, 100, dry, &a);
      CHECK(a.o.state == VRR_EMPTY && a.arms == 0 && a.loads == 0, "(i) recogida sin control en los toques, cargador cargado: espera el gesto (estado %d, rearmes %d)", a.o.state, a.arms); }
    /* (j) puente no válido un VBlank (BridgeState a cero: arma 0, total 0) con el arma seca, reserva y el cargador del juego a 0, sin recogida: el total «0 -> N» al volver no es una recogida */
    { cfg_method(VRR_VIA_R1_DOUBLE, 2); memset(&a, 0, sizeof a);
      BridgeState b; memset(&b, 0, sizeof b); b.valid = BRIDGE_OK_ALL_CORE; b.has_player = 1; b.in_control = 1; b.weapon = 0; b.magazine = 0; b.magazine_max = 12; b.weapon_ammo[0] = 30;
      BridgeState z; memset(&z, 0, sizeof z);
      VrReloadGame rg; vrreload_read_game(&z, &rg); CHECK(rg.weapon == -1 && rg.total == 0 && !rg.ok, "(j) puente no válido: sin arma (%d)", rg.weapon);
      for (int i = 0; i < 20; i++) {
          vrreload_read_game(i == 10 ? &z : &b, &rg); vrreload_frame(&rg, &dry, &a.o);
          if (a.o.load_mag > 0) { a.loads++; b.magazine = (int16_t)a.o.load_mag; } a.arms += a.o.arm_ready;
      }
      CHECK(a.o.state == VRR_EMPTY && a.arms == 0 && a.loads == 0, "(j) puente no válido 1 VBlank sin recogida: sigue EMPTY (estado %d, rearmes %d, cargas %d)", a.o.state, a.arms, a.loads); }
    /* (b) recogida en el mismo VBlank en que el arma VR se oculta, con control (EMPTY -> OFF) */
    { cfg_method(VRR_VIA_R1_DOUBLE, 2); Game g = drygame(0, 0); memset(&a, 0, sizeof a); runl(&g, 5, dry, &a);
      g.total = 12; runl(&g, 3, hid, &a); runl(&g, 4, dry, &a);
      CHECK(a.o.state == VRR_READY && a.arms == 1 && a.loads == 1, "(b) recogida en el VBlank en que se oculta el arma: lista al volver (estado %d, rearmes %d, cargas %d)", a.o.state, a.arms, a.loads); }
    /* (c) recogida en el VBlank de OFF -> EMPTY */
    { cfg_method(VRR_VIA_R1_DOUBLE, 2); Game g = drygame(0, 0); memset(&a, 0, sizeof a); runl(&g, 5, dry, &a);
      runl(&g, 3, hid, &a); g.total = 12; runl(&g, 4, dry, &a);
      CHECK(a.o.state == VRR_READY && a.arms == 1 && a.loads == 1, "(c) recogida en el VBlank en que vuelve a mostrarse: lista (estado %d, rearmes %d, cargas %d)", a.o.state, a.arms, a.loads); }
}
/* la recogida con el juego que SÍ carga su cargador al recoger (lo que suponía t_ledger_pickup): rearma al instante sin pulsar nada */
static void t_pickup_game_loads(void) {
    cfg_method(VRR_VIA_R1_DOUBLE, 2);
    Game g = newgame(0, 0); VrReloadOut o; PulseLog pl = newlog();
    VrReloadIn dry = in0(); dry.ready = 0; dry.no_mag = 1;
    run(&g, 20, dry, &o, &pl);
    CHECK(o.state == VRR_EMPTY, "EMPTY");
    g.total = 12; g.mag = 12;
    o = step(&g, dry, &pl);
    CHECK(o.state == VRR_READY && o.arm_ready && o.fire_allowed && o.pad_force == 0, "rearma sin maniobra (estado %d)", o.state);
    o = step(&g, dry, &pl);
    CHECK(!o.arm_ready, "una sola vez");
}
/* sin recogida: el arma VR seca con reserva (modo manual: el usuario debe recargar a mano) NO se rearma sola, pero el juego sí queda cargado (maniobra sin esperar al gesto) */
static void t_dry_with_reserve(void) {
    cfg_method(VRR_VIA_R1_DOUBLE, 2);
    Game g = newgame(1, 30); VrReloadOut o; PulseLog pl = newlog();
    VrReloadIn dry = in0(); dry.ready = 0; dry.no_mag = 1;
    run(&g, 5, in0(), &o, &pl);
    g.mag = 0;                                                  /* último disparo con reserva y el juego sin recargar solo (arma bajada) */
    int arms = 0;
    for (int i = 0; i < 300; i++) { o = step(&g, dry, &pl); arms += o.arm_ready; }
    CHECK(g.mag > 0, "el juego quedó cargado sin esperar al gesto (%d)", g.mag);
    CHECK(o.state == VRR_EMPTY && !o.fire_allowed && arms == 0, "el arma VR espera su gesto (estado %d, rearmes %d)", o.state, arms);
    VrReloadIn rd = in0(); rd.ready = 1;
    o = step(&g, rd, &pl);
    CHECK(o.state == VRR_READY && o.fire_allowed && !o.arm_ready && o.sync_rounds == g.mag - 1, "gesto -> READY con el cargador del juego");
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
/* Reserva tras agotar las balas (log del visor: recoger 12 balas con el arma vacía dejaba la reserva en 0 y el cargador VR vacío para siempre). Si el arma VR no tiene cargador montado ni bala en la
 * recámara, nada del cargador del juego está «dentro» del arma: toda la munición del juego es reserva (aunque el juego ya haya cargado su cargador al recogerla) y lo «cogido» no cuenta.
 * Al agotarse la munición del juego el libro virtual vuelve a cero. */
static void t_ledger_pickup(void) {
    cfg_method(VRR_VIA_R1_DOUBLE, 2);
    WGameBridge b; memset(&b, 0, sizeof b); vrreload_bridge_fill(&b);
    VrReloadOut o; PulseLog pl = newlog();
    VrReloadIn wm0 = in0(), nm = in0(); nm.ready = 0; nm.no_mag = 1;
    Game g = newgame(1, 1);
    run(&g, 5, wm0, &o, &pl);
    run(&g, 2, nm, &o, &pl);                                    /* el último disparo VR aún sin descontar en el juego (1-2 fotogramas): no sube la reserva */
    CHECK(b.ammo_reserve(0) == 0, "último disparo sin descontar: la reserva sigue en 0 (%d)", b.ammo_reserve(0));
    g.mag = 0; g.total = 0;
    run(&g, 10, nm, &o, &pl);
    CHECK(o.state == VRR_EMPTY && b.ammo_reserve(0) == 0, "sin balas: EMPTY y reserva 0 (%d)", b.ammo_reserve(0));
    g.total = 12; g.mag = 12;                                   /* recoge 12 balas y el juego carga su cargador */
    run(&g, 20, nm, &o, &pl);
    CHECK(b.ammo_reserve(0) == 12, "recogidas con el arma VR sin balas: las 12 son reserva (%d)", b.ammo_reserve(0));
    g.mag = 0;                                                  /* el cargador del juego aún vacío */
    run(&g, 5, nm, &o, &pl);
    CHECK(b.ammo_reserve(0) == 12, "con el cargador del juego a 0 también (%d)", b.ammo_reserve(0));
    VrReloadIn wm = in0(); wm.ready = 0;                        /* con cargador montado: la cuenta de siempre (total - cargador del juego - cogido) */
    g.mag = 5; g.total = 20;
    run(&g, 5, wm, &o, &pl);
    CHECK(b.ammo_reserve(0) == 15, "con cargador montado: total - cargador (%d)", b.ammo_reserve(0));
    /* lo cogido de la cintura y el arma VR se queda sin cargador (se expulsa sin soltarlo en el libro, se pierde al reiniciar el arma): no queda «cogido» huérfano */
    b.ammo_take(0, 12);
    CHECK(b.ammo_reserve(0) == 3, "cogido (%d)", b.ammo_reserve(0));
    run(&g, 20, nm, &o, &pl);
    CHECK(b.ammo_reserve(0) == 20, "sin balas en el arma VR el libro se pone a 0 y la reserva es todo (%d)", b.ammo_reserve(0));
    run(&g, 3, wm, &o, &pl);
    CHECK(b.ammo_reserve(0) == 15, "y al montar uno vuelve la cuenta normal sin restos (%d)", b.ammo_reserve(0));
    /* agotar: lo cogido no sobrevive a quedarse el juego sin balas */
    b.ammo_take(0, 12);
    g.mag = 0; g.total = 0; run(&g, 3, wm, &o, &pl);
    g.total = 12; g.mag = 0; run(&g, 3, wm, &o, &pl);
    CHECK(b.ammo_reserve(0) == 12, "tras agotar y recoger 12 la reserva es 12, no 0 (%d)", b.ammo_reserve(0));
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
            VrReloadIn in = {(rand() % 12) - 2, rand() % 5 == 0, rand() & 1, 0};
            VrReloadOut o; vrreload_frame(rand() % 500 ? &g : NULL, rand() % 500 ? &in : NULL, &o);
            CHECK((o.pad_force & ~(R1 | R2)) == 0, "pad_force fuera de R1/R2");
            CHECK(o.state >= VRR_OFF && o.state <= VRR_VERIFY, "estado válido");
            CHECK(o.show_mag >= 0 && o.show_reserve >= 0, "pantalla no negativa");
            CHECK(o.sync_rounds >= -1 && o.sync_chambered >= -1, "sync válido");
            CHECK(!o.busy || (o.pad_mask == (R1 | R2)), "ocupado -> máscara");
            CHECK(!(o.busy && o.fire_allowed), "ocupado no dispara");
            CHECK(!o.arm_ready || (o.state == VRR_READY && o.fire_allowed == (g.mag > 0)), "rearme solo en READY");
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
    t_tactical(); t_adopt(); t_empty_latch(); t_empty_no_reserve(); t_empty_needs_gesture(); t_empty_forced(); t_pickup_dry(); t_pickup_direct(); t_pause_keeps_gesture(); t_pickup_windows(); t_pickup_game_loads(); t_dry_with_reserve(); t_nothing_to_do(); t_lost_tap(); t_never();
    t_r2(); t_ledger(); t_ledger_pickup(); t_ledger_switch(); t_modes_weapon(); t_fuzz();
    printf("test_reload: %d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
