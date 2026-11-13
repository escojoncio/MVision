/* Pruebas sintéticas de vr_reload (espejo del cargador del juego): sin datos del juego. Un "juego falso" recarga como el original (sna_init.c): tras el último disparo, o al pulsar el ataque
 * con el cargador a 0 y reserva, GM_Magazine pasa a min(capacidad, total); recoger munición solo sube el total. Compilar con ASan/UBSan (make test-san). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_reload.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define SQ (1u << 15)

typedef struct {
    int weapon, mag, mag_max, total, ok, menu;
    int rl_wait;                         /* VBlanks hasta que acabe de recargar (0: no recarga) */
    int vr;                              /* balas del arma VR (cargador + recámara) */
    int vb, shot_vb;
} Game;
static Game newgame(int mag, int total) { Game g; memset(&g, 0, sizeof g); g.weapon = 0; g.mag = mag; g.mag_max = 12; g.total = total; g.ok = 1; g.vr = mag; return g; }
static void game_shoot(Game *g) { if (g->mag > 0) { g->mag--; g->total--; if (g->mag == 0 && g->total > 0) g->rl_wait = 6; } }
static void game_read(const Game *g, VrReloadGame *o) { memset(o, 0, sizeof *o); o->ok = g->ok; o->weapon = g->weapon; o->mag = g->mag; o->mag_max = g->mag_max; o->total = g->total; o->menu = g->menu; }

/* Un VBlank: lee el juego, ejecuta vr_reload, aplica sus efectos (arma VR igualada, load_mag, ataque pulsado) y avanza el juego. */
typedef struct { int trig, shot; } Ev;
static VrReloadOut step(Game *g, Ev ev, int weapon_id) {
    VrReloadGame rg; game_read(g, &rg);
    VrReloadIn in; memset(&in, 0, sizeof in);
    in.weapon_id = weapon_id; in.trigger_edge = ev.trig; in.vr_held = g->vr; in.vr_shot = ev.shot;
    VrReloadOut o; vrreload_frame(&rg, &in, &o);
    if (ev.shot && g->vr > 0) g->vr--;
    if (o.set_held >= 0) g->vr = o.set_held;
    if (o.load_mag > 0) g->mag = o.load_mag;                                 /* el anfitrión escribe GM_Magazine */
    if ((o.pad_force & SQ) && g->mag == 0 && g->total > 0 && !g->rl_wait) g->rl_wait = 17;      /* ataque con el cargador vacío: el juego recarga */
    if (g->rl_wait > 0 && --g->rl_wait == 0 && g->mag == 0 && g->total > 0) g->mag = g->total < g->mag_max ? g->total : g->mag_max;
    g->vb++;
    return o;
}
static VrReloadOut run(Game *g, int n, int weapon_id) { VrReloadOut o; memset(&o, 0, sizeof o); Ev e = {0, 0}; for (int i = 0; i < n; i++) o = step(g, e, weapon_id); return o; }

static void t_mirror_steady(void) {
    vrreload_init();
    Game g = newgame(10, 40); g.vr = 3;                      /* arma recién mostrada con otra cuenta */
    Ev e = {0, 0};
    VrReloadOut o = step(&g, e, 0);
    CHECK(o.set_held == 10 && g.vr == 10, "recién mostrada: se iguala al cargador del juego (%d)", g.vr);
    CHECK(o.show_mag == 10 && o.show_reserve == 30, "munición mostrada: cargador %d reserva %d", o.show_mag, o.show_reserve);
    int sets = 0; for (int i = 0; i < 100; i++) { o = step(&g, e, 0); if (o.set_held >= 0) sets++; }
    CHECK(sets == 0 && o.fire_allowed && !o.busy, "estable: no se toca el arma VR (%d correcciones)", sets);
    /* el arma VR tiene más que el juego: se corrige al instante */
    g.vr = 12; o = step(&g, e, 0);
    CHECK(o.set_held == 10 && g.vr == 10, "más balas que el juego: corregido ya (%d)", g.vr);
    /* disparo VR que el juego no gasta: espera REFLECT_SETTLE y vuelve al cargador del juego */
    Ev sh = {0, 1}; o = step(&g, sh, 0);
    CHECK(g.vr == 9 && o.set_held < 0, "disparo VR: 9 balas y sin corregir (%d)", g.vr);
    for (int i = 0; i < 50; i++) step(&g, e, 0);
    CHECK(g.vr == 9, "a los 50 VBlanks aún no se iguala (%d)", g.vr);
    for (int i = 0; i < 30; i++) step(&g, e, 0);
    CHECK(g.vr == 10, "pasada la espera vuelve al cargador del juego (%d)", g.vr);
    /* el juego gasta la bala del disparo VR: nada que corregir */
    sh.shot = 1; step(&g, sh, 0); game_shoot(&g);
    o = run(&g, 100, 0);
    CHECK(g.mag == 9 && g.vr == 9 && o.fire_allowed, "juego y VR gastan una: %d / %d", g.mag, g.vr);
}
static void t_game_reload(void) {
    vrreload_init();
    Game g = newgame(1, 25); Ev e = {0, 0};
    run(&g, 5, 0);
    Ev sh = {0, 1}; step(&g, sh, 0); game_shoot(&g);         /* última bala: el juego recarga solo en pocos VBlanks */
    CHECK(g.mag == 0 && g.vr == 0, "vacía (juego %d, VR %d)", g.mag, g.vr);
    VrReloadOut o; int got = 0, busy_after = 0;
    for (int i = 0; i < 40; i++) { o = step(&g, e, 0); if (g.mag > 0 && !got) { got = 1; CHECK(o.set_held < 0 || o.set_held == 0, "aún no refleja en el VBlank de la recarga (set %d)", o.set_held); } }
    CHECK(g.mag == 12 && g.vr == 12, "tras la recarga del juego el arma VR lleva %d (juego %d)", g.vr, g.mag);
    /* tras el evento de recarga hay un bloqueo (animación de recarga del juego) */
    o = step(&g, e, 0); (void)busy_after;
    for (int i = 0; i < 100; i++) o = step(&g, e, 0);
    CHECK(o.fire_allowed && !o.busy, "pasado el bloqueo se puede disparar");
    CHECK(vrreload_ammo_reserve(0) == 12 && vrreload_ammo_reserve(3) == 0, "reserva = total - cargador (%d), otra arma 0", vrreload_ammo_reserve(0));
}
static void t_lock_after_reload(void) {
    vrreload_init();
    Game g = newgame(0, 12); run(&g, 3, 0);
    g.mag = 12; VrReloadOut o = step(&g, (Ev){0, 0}, 0);     /* el juego recarga por su cuenta */
    CHECK(o.busy && !o.fire_allowed && o.set_held == 12, "recarga del juego: ocupado, sin disparo y arma VR igualada (busy %d permitido %d set %d)", o.busy, o.fire_allowed, o.set_held);
    int n = 0; while (n < 200 && !(o = step(&g, (Ev){0, 0}, 0)).fire_allowed) n++;
    CHECK(n > 20 && n < 100, "el bloqueo acaba (%d VBlanks)", n);
}
static void t_dry_press(void) {
    vrreload_init();
    Game g = newgame(0, 12); run(&g, 3, 0);
    CHECK(g.vr == 0, "cargador vacío: arma VR sin balas");
    VrReloadOut o = run(&g, 20, 0);
    CHECK(!o.pad_force && o.show_reserve == 12, "sin gatillo no se pulsa nada");
    o = step(&g, (Ev){1, 0}, 0);
    CHECK((o.pad_force & SQ) && o.busy && !o.fire_allowed, "gatillo con el cargador vacío: pulsa el ataque (force %04x)", o.pad_force);
    int forced = 1, n = 0;
    while (n < 100 && g.mag == 0) { o = step(&g, (Ev){0, 0}, 0); if (g.mag == 0 && !(o.pad_force & SQ)) forced = 0; n++; }
    CHECK(g.mag == 12 && forced, "mantiene el ataque hasta que el juego recarga (%d VBlanks, mag %d)", n, g.mag);
    o = step(&g, (Ev){0, 0}, 0);
    CHECK(!(o.pad_force & SQ) && g.vr == 12, "suelta al recargar y el arma VR lo refleja (VR %d)", g.vr);
    /* sin flanco, con el cargador cargado, sin reserva o con menú: no pulsa */
    vrreload_init(); g = newgame(5, 20); run(&g, 3, 0);
    CHECK(!step(&g, (Ev){1, 0}, 0).pad_force, "con balas no pulsa");
    vrreload_init(); g = newgame(0, 0); run(&g, 3, 0);
    CHECK(!step(&g, (Ev){1, 0}, 0).pad_force, "sin reserva no pulsa");
    vrreload_init(); g = newgame(0, 12); g.menu = 1; run(&g, 3, 0);
    CHECK(!step(&g, (Ev){1, 0}, 0).pad_force, "con menú no pulsa");
    /* tope: el juego no recarga nunca -> deja de pulsar */
    vrreload_init(); g = newgame(0, 12); run(&g, 3, 0);
    step(&g, (Ev){1, 0}, 0); g.rl_wait = 0;
    int cnt = 0; for (int i = 0; i < 300; i++) { VrReloadOut q = step(&g, (Ev){0, 0}, 0); g.rl_wait = 0; if (q.pad_force) cnt++; }
    CHECK(cnt <= 95, "sin recarga del juego el botón se suelta (%d VBlanks)", cnt);
}
static void t_pickup(void) {
    vrreload_init();
    Game g = newgame(0, 0); VrReloadOut o = run(&g, 5, 0);
    g.total = 30;                                            /* recoge: solo sube el total */
    o = step(&g, (Ev){0, 0}, 0);
    CHECK(o.load_mag == 12 && vrreload_pickup_pending(), "recogida con el arma seca: carga el cargador del juego (%d)", o.load_mag);
    o = step(&g, (Ev){0, 0}, 0);
    CHECK(g.mag == 12 && g.vr == 12 && o.set_held == 12 && !o.busy && !vrreload_pickup_pending(), "y el arma VR lo refleja sin bloqueo (VR %d, busy %d)", g.vr, o.busy);
    VrReloadStats st; vrreload_stats(&st);
    CHECK(st.loads == 1, "una carga (%u)", st.loads);
    /* menos munición que el cargador */
    vrreload_init(); g = newgame(0, 0); run(&g, 5, 0); g.total = 5; o = step(&g, (Ev){0, 0}, 0);
    CHECK(o.load_mag == 5, "recoge 5: carga 5 (%d)", o.load_mag);
    /* francotirador: siempre 5 */
    vrreload_init(); g = newgame(0, 0); g.weapon = 9; g.mag_max = 5; run(&g, 5, 9); g.total = 20; o = step(&g, (Ev){0, 0}, 9);
    CHECK(o.load_mag == 5, "PSG1: carga 5 (%d)", o.load_mag);
    /* recoger con balas en el cargador no escribe nada */
    vrreload_init(); g = newgame(4, 4); run(&g, 5, 0); g.total = 20; o = step(&g, (Ev){0, 0}, 0);
    CHECK(o.load_mag == 0 && !vrreload_pickup_pending(), "con balas no toca el cargador");
    /* la recogida sobrevive a una pausa y se carga al volver */
    vrreload_init(); g = newgame(0, 0); run(&g, 5, 0); g.ok = 0; g.total = 30;
    for (int i = 0; i < 20; i++) step(&g, (Ev){0, 0}, 0);
    CHECK(g.mag == 0 && vrreload_pickup_pending(), "en pausa queda pendiente");
    g.ok = 1; run(&g, 4, 0);
    CHECK(g.mag == 12 && g.vr == 12, "al volver carga y refleja (mag %d, VR %d)", g.mag, g.vr);
}
static void t_switch_and_invalid(void) {
    vrreload_init();
    Game g = newgame(8, 30); run(&g, 5, 0);
    /* cambio a otra arma de cargador: se iguala en el VBlank */
    g.weapon = 1; g.mag = 30; g.mag_max = 30; g.total = 120; g.vr = 0;
    VrReloadOut o = step(&g, (Ev){0, 0}, 1);
    CHECK(o.set_held == 30 && g.vr == 30, "arma nueva: igualada (%d)", g.vr);
    /* sin arma VR de cargador (-1): nada que hacer y se suelta lo pendiente */
    g = newgame(0, 12); run(&g, 3, 0); step(&g, (Ev){1, 0}, 0);
    o = step(&g, (Ev){0, 0}, -1);
    CHECK(!o.pad_force && !o.busy && o.set_held < 0, "sin arma VR: no pulsa ni iguala");
    /* otra arma distinta de la mostrada */
    o = step(&g, (Ev){0, 0}, 5);
    CHECK(!o.pad_force && o.set_held < 0, "arma VR distinta de la equipada: no hace nada");
    /* puente inválido */
    VrReloadGame rg; vrreload_read_game(NULL, &rg);
    CHECK(!rg.ok && rg.weapon == -1, "puente nulo");
    BridgeState bs; memset(&bs, 0, sizeof bs);
    vrreload_read_game(&bs, &rg);
    CHECK(!rg.ok && rg.weapon == -1 && rg.total == 0, "puente a cero: sin arma");
    vrreload_init(); VrReloadOut z = {0};
    vrreload_frame(NULL, NULL, &z);
    CHECK(z.set_held == -1 && !z.pad_force, "frame sin datos: nada");
    /* VBlank en que el jugador no controla: no iguala ni pulsa */
    g = newgame(0, 12); g.ok = 0; vrreload_init();
    o = step(&g, (Ev){1, 0}, 0);
    CHECK(!o.pad_force && o.set_held < 0, "sin control: nada");
}
static unsigned rnd_s = 12345u;
static unsigned rnd(void) { rnd_s = rnd_s * 1664525u + 1013904223u; return rnd_s >> 8; }
#define RND() rnd()
static void t_fuzz(void) {
    for (int run_i = 0; run_i < 200; run_i++) {
        vrreload_init();
        Game g = newgame((int)(RND() % 13), (int)(RND() % 40)); if (g.total < g.mag) g.total = g.mag; g.vr = g.mag;
        for (int i = 0; i < 400; i++) {
            unsigned r = RND() % 40;
            if (r == 0) g.total += (int)(RND() % 15);
            else if (r == 1 && g.mag > 0) game_shoot(&g);
            else if (r == 2) g.ok = !g.ok;
            else if (r == 3) g.menu = !g.menu;
            else if (r == 4) { g.weapon = (int)(RND() % 3); g.vr = (int)(RND() % 14); }
            else if (r == 5) g.mag = g.mag > g.total ? g.total : g.mag;
            Ev e = {(int)(RND() % 12 == 0), (int)(RND() % 15 == 0)};
            VrReloadOut o = step(&g, e, (int)(RND() % 4) - 1 == g.weapon ? g.weapon : -1);
            CHECK(o.set_held >= -1 && o.set_held <= 13, "set_held fuera de rango (%d)", o.set_held);
            CHECK((o.pad_force & ~SQ) == 0, "solo el botón de ataque (%04x)", o.pad_force);
            CHECK(!o.fire_allowed || (g.mag > 0 && !o.busy), "disparo permitido solo con cargador y sin ocupado");
            CHECK(o.show_mag >= 0 && o.show_reserve >= 0, "munición mostrada no negativa");
            if (g.vr < 0) g.vr = 0;
        }
    }
}

int main(void) {
    t_mirror_steady(); t_game_reload(); t_lock_after_reload(); t_dry_press(); t_pickup(); t_switch_and_invalid(); t_fuzz();
    printf("test_reload: %d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
