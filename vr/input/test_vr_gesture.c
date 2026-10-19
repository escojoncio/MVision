/* Pruebas sintéticas de los gestos de cuerpo (trayectorias de manos). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "vr_gesture.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static vrg_in_t base(void) {
    vrg_in_t in; memset(&in, 0, sizeof in);
    in.dt = 0.02f; in.active = 1; in.view_yaw = 0; in.weapon = -1; in.life = 256;
    in.head[1] = 1.6f; in.body[1] = 1.0f;
    in.tracked[0] = in.tracked[1] = 1;
    in.hand[0][0] = -0.2f; in.hand[0][1] = 1.2f; in.hand[0][2] = 0.1f;
    in.hand[1][0] = 0.2f;  in.hand[1][1] = 1.2f; in.hand[1][2] = 0.1f;
    return in;
}
/* golpe: la mano derecha avanza `dist` m en `n` pasos desde el pecho; delante = +Z con yaw 0. Devuelve nº de VBlanks con cuadrado. */
static int run_punch(vrg_t *g, vrg_in_t in, float dist, int n, int *ev) {
    int sq = 0; vrg_out_t o;
    for (int i = 0; i < 10; i++) vrg_update(g, &in, &o);
    for (int i = 1; i <= n + 12; i++) {
        float t = i > n ? 1.0f : (float)i / n;
        in.hand[1][2] = 0.1f + dist * t;
        vrg_update(g, &in, &o);
        if (o.press & VRG_SQUARE) sq++;
        if (o.event == VRG_EV_PUNCH && ev) (*ev)++;
    }
    return sq;
}

static void t_punch(void) {
    vrg_t g; vrg_init(&g); vrg_in_t in = base();
    in.n_en = 1; in.en[0].p[2] = 1.5f; in.en[0].p[1] = 1.0f;
    int ev = 0;
    int sq = run_punch(&g, in, 0.6f, 6, &ev);          /* 0,6 m en 0,12 s = 5 m/s */
    CHECK(sq == 4 && ev == 1);
    vrg_init(&g); ev = 0; sq = run_punch(&g, in, 0.6f, 60, &ev);   /* lento: 0,5 m/s */
    CHECK(sq == 0 && ev == 0);
    vrg_init(&g); vrg_in_t nowpn = in; nowpn.weapon = 0; sq = run_punch(&g, nowpn, 0.6f, 6, NULL);   /* con arma: nada */
    CHECK(sq == 0);
    vrg_init(&g); vrg_in_t noen = in; noen.n_en = 0; sq = run_punch(&g, noen, 0.6f, 6, NULL);       /* sin enemigo cerca */
    CHECK(sq == 0);
    vrg_init(&g); vrg_in_t far = in; far.en[0].p[2] = 9; sq = run_punch(&g, far, 0.6f, 6, NULL); CHECK(sq == 0);
    vrg_init(&g); vrg_in_t inact = in; inact.active = 0; sq = run_punch(&g, inact, 0.6f, 6, NULL); CHECK(sq == 0);
    /* hacia atrás o hacia un lado (velocidad no radial): no */
    vrg_init(&g); vrg_out_t o; int cnt = 0; in.hand[1][2] = 0.1f;
    for (int i = 0; i < 10; i++) vrg_update(&g, &in, &o);
    for (int i = 1; i <= 10; i++) { in.hand[1][2] = 0.1f - 0.1f * i; vrg_update(&g, &in, &o); cnt += (o.press & VRG_SQUARE) != 0; }
    CHECK(cnt == 0);
    /* dos golpes seguidos respetan el enfriamiento: 2 golpes separados 0,6 s dan 2 eventos */
    vrg_init(&g); in = base(); in.n_en = 1; in.en[0].p[2] = 1.5f; ev = 0; cnt = 0;
    for (int i = 0; i < 10; i++) vrg_update(&g, &in, &o);
    for (int k = 0; k < 2; k++) {
        for (int i = 1; i <= 6; i++) { in.hand[1][2] = 0.1f + 0.1f * i; vrg_update(&g, &in, &o); ev += o.event == VRG_EV_PUNCH; cnt += (o.press & VRG_SQUARE) != 0; }
        for (int i = 0; i < 30; i++) { in.hand[1][2] = 0.1f; vrg_update(&g, &in, &o); ev += o.event == VRG_EV_PUNCH; cnt += (o.press & VRG_SQUARE) != 0; }
    }
    CHECK(ev == 2);
}

static void t_grab(void) {
    vrg_t g; vrg_init(&g); vrg_in_t in = base(); vrg_out_t o;
    in.n_en = 1; in.en[0].p[0] = 0; in.en[0].p[1] = 1.0f; in.en[0].p[2] = 0.9f;     /* enemigo a 0,9 m delante */
    in.hand[1][0] = 0.1f; in.hand[1][1] = 1.45f; in.hand[1][2] = 0.85f;            /* mano derecha en su cuello */
    for (int i = 0; i < 10; i++) vrg_update(&g, &in, &o);
    CHECK(!(o.press & VRG_SQUARE));
    in.grip[1] = 1.0f;
    int sq = 0, first = -1, ev_grab = 0, masked = 0;
    for (int i = 0; i < 20; i++) { vrg_update(&g, &in, &o); if (o.press & VRG_SQUARE) { sq++; if (first < 0) first = i; } ev_grab += o.event == VRG_EV_GRAB; masked += (o.mask & (VRG_R1 | VRG_L1)) != 0; }
    CHECK(sq >= 18 && first <= 1 && ev_grab == 1 && masked >= 18);   /* cuadrado mantenido mientras el grip está cerrado */
    /* apretar de nuevo (abrir y cerrar) con el juego sujetando = toques de cuadrado */
    in.holding = 1; int taps = 0, prev = 1;
    for (int k = 0; k < 3; k++) {
        in.grip[1] = 0.2f; for (int i = 0; i < 6; i++) { vrg_update(&g, &in, &o); prev = (o.press & VRG_SQUARE) != 0; }
        in.grip[1] = 1.0f; for (int i = 0; i < 6; i++) { vrg_update(&g, &in, &o); int p = (o.press & VRG_SQUARE) != 0; if (p && !prev) taps++; prev = p; }
    }
    CHECK(taps >= 2);
    /* soltar: el cuadrado se suelta y el grip libre vuelve a manos del usuario */
    in.grip[1] = 0.0f; for (int i = 0; i < 8; i++) vrg_update(&g, &in, &o);
    CHECK(!(o.press & VRG_SQUARE) && !(o.mask & (VRG_R1 | VRG_L1)));
    /* mano lejos del cuello: cerrar el grip no agarra (R1/L1 siguen siendo del usuario) */
    vrg_init(&g); in.holding = 0; in.hand[1][0] = 0.6f; in.hand[1][1] = 1.0f; in.hand[1][2] = 0.1f; in.grip[1] = 0;
    for (int i = 0; i < 10; i++) vrg_update(&g, &in, &o);
    in.grip[1] = 1.0f; sq = 0; masked = 0;
    for (int i = 0; i < 20; i++) { vrg_update(&g, &in, &o); sq += (o.press & VRG_SQUARE) != 0; masked += o.mask != 0; }
    CHECK(sq == 0 && masked == 0);
    /* grip ya cerrado al llegar: no agarra (hay que cerrar la mano cerca) */
    vrg_init(&g); in.grip[1] = 1.0f; in.hand[1][0] = 0.6f; for (int i = 0; i < 10; i++) vrg_update(&g, &in, &o);
    in.hand[1][0] = 0.1f; in.hand[1][1] = 1.45f; in.hand[1][2] = 0.85f; sq = 0;
    for (int i = 0; i < 10; i++) { vrg_update(&g, &in, &o); sq += (o.press & VRG_SQUARE) != 0; }
    CHECK(sq == 0);
    /* con arma equipada: nada */
    vrg_init(&g); in.weapon = 1; in.grip[1] = 0; for (int i = 0; i < 10; i++) vrg_update(&g, &in, &o);
    in.grip[1] = 1; sq = 0; for (int i = 0; i < 10; i++) { vrg_update(&g, &in, &o); sq += (o.press & VRG_SQUARE) != 0; } CHECK(sq == 0);
    /* enemigo caído: no se agarra */
    vrg_init(&g); in.weapon = -1; in.en[0].down = 1; in.grip[1] = 0; for (int i = 0; i < 10; i++) vrg_update(&g, &in, &o);
    in.grip[1] = 1; sq = 0; for (int i = 0; i < 10; i++) { vrg_update(&g, &in, &o); sq += (o.press & VRG_SQUARE) != 0; } CHECK(sq == 0);
}

static void t_wall(void) {
    vrg_t g; vrg_init(&g); vrg_in_t in = base(); vrg_out_t o;
    in.on_wall = 1;
    for (int i = 0; i < 10; i++) vrg_update(&g, &in, &o);
    int circ = 0, sq = 0, ev = 0;
    for (int i = 1; i <= 6; i++) { in.hand[1][2] = 0.1f - 0.1f * i; vrg_update(&g, &in, &o); circ += (o.press & VRG_CIRCLE) != 0; sq += (o.press & VRG_SQUARE) != 0; ev += o.event == VRG_EV_KNOCK; }
    for (int i = 0; i < 10; i++) { in.hand[1][2] = -0.5f; vrg_update(&g, &in, &o); circ += (o.press & VRG_CIRCLE) != 0; }
    CHECK(circ == 4 && sq == 0 && ev == 1);             /* golpe hacia atrás/pared: círculo, y nunca cuadrado en la pared */
    /* lento: nada */
    vrg_init(&g); in = base(); in.on_wall = 1; for (int i = 0; i < 10; i++) vrg_update(&g, &in, &o);
    circ = 0; for (int i = 1; i <= 60; i++) { in.hand[1][2] = 0.1f - 0.01f * i; vrg_update(&g, &in, &o); circ += (o.press & VRG_CIRCLE) != 0; }
    CHECK(circ == 0);
    /* sin pared: el mismo golpe no es círculo */
    vrg_init(&g); in = base(); for (int i = 0; i < 10; i++) vrg_update(&g, &in, &o);
    circ = 0; for (int i = 1; i <= 6; i++) { in.hand[1][2] = 0.1f - 0.1f * i; vrg_update(&g, &in, &o); circ += (o.press & VRG_CIRCLE) != 0; }
    CHECK(circ == 0);
    /* asomarse: la cabeza se inclina de lado estando pegado */
    vrg_init(&g); in = base(); in.on_wall = 1; for (int i = 0; i < 10; i++) vrg_update(&g, &in, &o);
    CHECK(!(o.press & VRG_TRIANGLE));
    in.head[0] = 0.2f; for (int i = 0; i < 5; i++) vrg_update(&g, &in, &o);
    CHECK(o.press & VRG_TRIANGLE);
    in.head[0] = 0.08f; vrg_update(&g, &in, &o); CHECK(o.press & VRG_TRIANGLE);      /* histéresis */
    in.head[0] = 0.02f; vrg_update(&g, &in, &o); CHECK(!(o.press & VRG_TRIANGLE));
    /* sentado, sin inclinar: ninguna acción forzada; el stick lo hace el juego */
    vrg_init(&g); in = base(); in.on_wall = 1; int tri = 0;
    for (int i = 0; i < 100; i++) { vrg_update(&g, &in, &o); tri += (o.press & VRG_TRIANGLE) != 0; } CHECK(tri == 0);
}

static void t_haptics_robust(void) {
    vrg_t g; vrg_init(&g); vrg_in_t in = base(); vrg_out_t o;
    for (int i = 0; i < 3; i++) vrg_update(&g, &in, &o);
    in.life = 200; vrg_update(&g, &in, &o);
    CHECK(o.event == VRG_EV_HURT && o.haptic[0] > 0.3f && o.haptic[1] > 0.3f);
    vrg_update(&g, &in, &o); CHECK(o.haptic[0] == 0);
    in.life = 256; vrg_update(&g, &in, &o); CHECK(o.event != VRG_EV_HURT);        /* curarse no vibra */
    in.on_wall = 1; vrg_update(&g, &in, &o); CHECK(o.event == VRG_EV_WALL && o.haptic[0] > 0);
    /* NaN / basura */
    vrg_in_t bad = base(); bad.hand[1][0] = NAN; bad.head[1] = INFINITY; bad.dt = NAN;
    for (int i = 0; i < 5; i++) vrg_update(&g, &bad, &o);
    CHECK(o.press == 0);
    bad = base(); bad.grip[1] = NAN; bad.hand[0][1] = NAN; bad.n_en = 20; for (int i = 0; i < 5; i++) vrg_update(&g, &bad, &o);
    CHECK(o.press == 0);
    /* desactivado */
    vrg_cfg_defaults(&g.cfg); g.cfg.enable = 0; in = base(); in.n_en = 1; in.en[0].p[2] = 1.5f;
    int sq = run_punch(&g, in, 0.6f, 6, NULL); CHECK(sq == 0);
    /* mano sin seguimiento */
    vrg_init(&g); in = base(); in.n_en = 1; in.en[0].p[2] = 1.5f; in.tracked[1] = 0; sq = run_punch(&g, in, 0.6f, 6, NULL); CHECK(sq == 0);
}

int main(void) {
    t_punch(); t_grab(); t_wall(); t_haptics_robust();
    printf("%d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
