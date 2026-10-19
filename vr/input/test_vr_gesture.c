/* Pruebas sintéticas de los gestos de cuerpo (trayectorias de manos). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "vr_gesture.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void upd(vrg_t *g, vrg_in_t *in, vrg_out_t *o) {   /* en el test espacio de usuario == mundo */
    for (int s = 0; s < 2; s++) for (int i = 0; i < 3; i++) { in->hrel[s][i] = in->hand[s][i] - in->head[i]; in->hand_u[s][i] = in->hand[s][i] - in->body[i]; }
    memcpy(in->head_u, in->head, sizeof in->head_u);
    vrg_update(g, in, o);
}

static vrg_in_t base(void) {
    vrg_in_t in; memset(&in, 0, sizeof in);
    in.dt = 0.02f; in.active = 1; in.view_yaw = 0; in.weapon = -1; in.life = 256;
    in.head[1] = 1.6f; in.body[1] = 1.0f;
    in.tracked[0] = in.tracked[1] = 1;
    in.hand[0][0] = -0.2f; in.hand[0][1] = 1.2f; in.hand[0][2] = 0.1f;
    in.hand[1][0] = 0.2f;  in.hand[1][1] = 1.2f; in.hand[1][2] = 0.1f;
    in.head_fwd[1] = 1.0f;     /* en el test el delante es +Z */
    return in;
}
/* golpe: la mano derecha avanza `dist` m en `n` pasos desde el pecho; delante = +Z con yaw 0. Devuelve nº de VBlanks con cuadrado. */
static int run_punch(vrg_t *g, vrg_in_t in, float dist, int n, int *ev) {
    int sq = 0; vrg_out_t o;
    in.grip[1] = 1.0f;                                   /* el golpe cuenta con la mano en puño */
    for (int i = 0; i < 10; i++) upd(g, &in, &o);
    for (int i = 1; i <= n + 12; i++) {
        float t = i > n ? 1.0f : (float)i / n;
        in.hand[1][2] = 0.1f + dist * t;
        upd(g, &in, &o);
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
    for (int i = 0; i < 10; i++) upd(&g, &in, &o);
    for (int i = 1; i <= 10; i++) { in.hand[1][2] = 0.1f - 0.1f * i; upd(&g, &in, &o); cnt += (o.press & VRG_SQUARE) != 0; }
    CHECK(cnt == 0);
    /* dos golpes seguidos respetan el enfriamiento: 2 golpes separados 0,6 s dan 2 eventos */
    vrg_init(&g); in = base(); in.n_en = 1; in.en[0].p[2] = 1.5f; in.grip[1] = 1.0f; ev = 0; cnt = 0;
    for (int i = 0; i < 10; i++) upd(&g, &in, &o);
    for (int k = 0; k < 2; k++) {
        for (int i = 1; i <= 6; i++) { in.hand[1][2] = 0.1f + 0.1f * i; upd(&g, &in, &o); ev += o.event == VRG_EV_PUNCH; cnt += (o.press & VRG_SQUARE) != 0; }
        for (int i = 0; i < 30; i++) { in.hand[1][2] = 0.1f; upd(&g, &in, &o); ev += o.event == VRG_EV_PUNCH; cnt += (o.press & VRG_SQUARE) != 0; }
    }
    CHECK(ev == 2);
}

/* andar rápido (cuerpo, cabeza y manos se mueven juntos 4 m/s en el mundo): no es un golpe */
static void t_walk(void) {
    vrg_t g; vrg_init(&g); vrg_in_t in = base(); vrg_out_t o; in.n_en = 1; in.en[0].p[2] = 1.5f; int sq = 0;
    for (int i = 0; i < 100; i++) {
        float d = 0.08f * i;
        in.body[2] = d; in.head[2] = d; in.hand[0][2] = 0.1f + d; in.hand[1][2] = 0.1f + d; in.en[0].p[2] = 1.5f + d;
        /* hrel constante: el helper upd lo recalcula a partir de mundo, que mantiene la diferencia */
        upd(&g, &in, &o); sq += (o.press & VRG_SQUARE) != 0;
    }
    CHECK(sq == 0);
}

static void t_grab(void) {
    vrg_t g; vrg_init(&g); vrg_in_t in = base(); vrg_out_t o;
    in.n_en = 1; in.en[0].p[0] = 0; in.en[0].p[1] = 1.0f; in.en[0].p[2] = 0.9f;     /* enemigo a 0,9 m delante */
    in.hand[1][0] = 0.1f; in.hand[1][1] = 1.45f; in.hand[1][2] = 0.85f;            /* mano derecha en su cuello */
    for (int i = 0; i < 10; i++) upd(&g, &in, &o);
    CHECK(!(o.press & VRG_SQUARE));
    in.grip[1] = in.trig[1] = 1.0f;
    int sq = 0, first = -1, ev_grab = 0, masked = 0;
    for (int i = 0; i < 20; i++) { upd(&g, &in, &o); if (o.press & VRG_SQUARE) { sq++; if (first < 0) first = i; } ev_grab += o.event == VRG_EV_GRAB; masked += (o.mask & (VRG_R1 | VRG_L1)) != 0; }
    CHECK(sq >= 18 && first <= 1 && ev_grab == 1 && masked >= 18);   /* cuadrado mantenido mientras el grip está cerrado */
    /* apretar de nuevo (abrir y cerrar) con el juego sujetando = toques de cuadrado */
    in.holding = 1; int taps = 0, prev = 1;
    for (int k = 0; k < 3; k++) {
        in.grip[1] = in.trig[1] = 0.2f; for (int i = 0; i < 6; i++) { upd(&g, &in, &o); prev = (o.press & VRG_SQUARE) != 0; }
        in.grip[1] = in.trig[1] = 1.0f; for (int i = 0; i < 6; i++) { upd(&g, &in, &o); int p = (o.press & VRG_SQUARE) != 0; if (p && !prev) taps++; prev = p; }
    }
    CHECK(taps >= 2);
    /* soltar: el cuadrado se suelta y el grip libre vuelve a manos del usuario */
    in.grip[1] = in.trig[1] = 0.0f; for (int i = 0; i < 8; i++) upd(&g, &in, &o);
    CHECK(!(o.press & VRG_SQUARE) && !(o.mask & (VRG_R1 | VRG_L1)));
    /* mano lejos del cuello: cerrar el grip no agarra (R1/L1 siguen siendo del usuario) */
    vrg_init(&g); in.holding = 0; in.hand[1][0] = 0.6f; in.hand[1][1] = 1.0f; in.hand[1][2] = 0.1f; in.grip[1] = in.trig[1] = 0;
    for (int i = 0; i < 10; i++) upd(&g, &in, &o);
    in.grip[1] = in.trig[1] = 1.0f; sq = 0; masked = 0;
    for (int i = 0; i < 20; i++) { upd(&g, &in, &o); sq += (o.press & VRG_SQUARE) != 0; masked += (o.mask & (VRG_R1 | VRG_L1)) != 0; }
    CHECK(sq == 4);    /* gatillo lejos del cuello = golpe de botón (toque de 4 VBlanks, nunca agarre mantenido) */
    vrg_init(&g); g.cfg.button_jab = 0; in.grip[1] = in.trig[1] = 0; for (int i = 0; i < 10; i++) upd(&g, &in, &o);
    in.grip[1] = in.trig[1] = 1.0f; sq = 0; for (int i = 0; i < 20; i++) { upd(&g, &in, &o); sq += (o.press & VRG_SQUARE) != 0; }
    CHECK(sq == 0);                   /* sin golpe de botón: el cuadrado físico se descarta */
    /* grip ya cerrado al llegar: no agarra (hay que cerrar la mano cerca) */
    vrg_init(&g); in.grip[1] = in.trig[1] = 1.0f; in.hand[1][0] = 0.6f; for (int i = 0; i < 10; i++) upd(&g, &in, &o);
    in.hand[1][0] = 0.1f; in.hand[1][1] = 1.45f; in.hand[1][2] = 0.85f; sq = 0;
    for (int i = 0; i < 10; i++) { upd(&g, &in, &o); sq += (o.press & VRG_SQUARE) != 0; }
    CHECK(sq == 0);
    /* con arma equipada: nada */
    vrg_init(&g); in.weapon = 1; in.grip[1] = in.trig[1] = 0; for (int i = 0; i < 10; i++) upd(&g, &in, &o);
    in.grip[1] = in.trig[1] = 1; sq = 0; for (int i = 0; i < 10; i++) { upd(&g, &in, &o); sq += (o.press & VRG_SQUARE) != 0; } CHECK(sq == 0);
    /* enemigo caído: no se agarra */
    vrg_init(&g); in.weapon = -1; in.en[0].down = 1; in.grip[1] = in.trig[1] = 0; for (int i = 0; i < 10; i++) upd(&g, &in, &o);
    in.grip[1] = in.trig[1] = 1; sq = 0; for (int i = 0; i < 10; i++) { upd(&g, &in, &o); sq += (o.press & VRG_SQUARE) != 0; } CHECK(sq == 4);   /* caído: golpe de botón, no agarre */
}

static void t_wall(void) {
    vrg_t g; vrg_init(&g); vrg_in_t in = base(); vrg_out_t o;
    in.on_wall = 1;
    for (int i = 0; i < 10; i++) upd(&g, &in, &o);
    int circ = 0, sq = 0, ev = 0;
    for (int i = 1; i <= 6; i++) { in.hand[1][2] = 0.1f - 0.1f * i; upd(&g, &in, &o); circ += (o.press & VRG_CIRCLE) != 0; sq += (o.press & VRG_SQUARE) != 0; ev += o.event == VRG_EV_KNOCK; }
    for (int i = 0; i < 10; i++) { in.hand[1][2] = -0.5f; upd(&g, &in, &o); circ += (o.press & VRG_CIRCLE) != 0; }
    CHECK(circ == 4 && sq == 0 && ev == 1);             /* golpe hacia atrás/pared: círculo, y nunca cuadrado en la pared */
    /* lento: nada */
    vrg_init(&g); in = base(); in.on_wall = 1; for (int i = 0; i < 10; i++) upd(&g, &in, &o);
    circ = 0; for (int i = 1; i <= 60; i++) { in.hand[1][2] = 0.1f - 0.01f * i; upd(&g, &in, &o); circ += (o.press & VRG_CIRCLE) != 0; }
    CHECK(circ == 0);
    /* sin pared: el mismo golpe no es círculo */
    vrg_init(&g); in = base(); for (int i = 0; i < 10; i++) upd(&g, &in, &o);
    circ = 0; for (int i = 1; i <= 6; i++) { in.hand[1][2] = 0.1f - 0.1f * i; upd(&g, &in, &o); circ += (o.press & VRG_CIRCLE) != 0; }
    CHECK(circ == 0);
    /* asomarse: la cabeza se inclina de lado estando pegado */
    vrg_init(&g); in = base(); in.on_wall = 1; for (int i = 0; i < 10; i++) upd(&g, &in, &o);
    CHECK(!(o.press & VRG_TRIANGLE));
    for (int i = 1; i <= 8; i++) { in.head[0] = 0.025f * i; upd(&g, &in, &o); }      /* 0,2 m en 0,16 s */
    for (int i = 0; i < 3; i++) upd(&g, &in, &o);
    CHECK(o.press & VRG_TRIANGLE);
    in.head[0] = 0.08f; upd(&g, &in, &o); CHECK(o.press & VRG_TRIANGLE);      /* histéresis */
    in.head[0] = 0.02f; upd(&g, &in, &o); CHECK(!(o.press & VRG_TRIANGLE));
    /* sentado, sin inclinar: ninguna acción forzada; el stick lo hace el juego */
    vrg_init(&g); in = base(); in.on_wall = 1; int tri = 0;
    for (int i = 0; i < 100; i++) { upd(&g, &in, &o); tri += (o.press & VRG_TRIANGLE) != 0; } CHECK(tri == 0);
}

static void t_haptics_robust(void) {
    vrg_t g; vrg_init(&g); vrg_in_t in = base(); vrg_out_t o;
    for (int i = 0; i < 3; i++) upd(&g, &in, &o);
    in.life = 200; upd(&g, &in, &o);
    CHECK(o.event == VRG_EV_HURT && o.haptic[0] > 0.3f && o.haptic[1] > 0.3f);
    upd(&g, &in, &o); CHECK(o.haptic[0] == 0);
    in.life = 256; upd(&g, &in, &o); CHECK(o.event != VRG_EV_HURT);        /* curarse no vibra */
    in.on_wall = 1; upd(&g, &in, &o); CHECK(o.event == VRG_EV_WALL && o.haptic[0] > 0);
    /* NaN / basura */
    vrg_in_t bad = base(); bad.hand[1][0] = NAN; bad.head[1] = INFINITY; bad.dt = NAN;
    for (int i = 0; i < 5; i++) upd(&g, &bad, &o);
    CHECK(o.press == 0);
    bad = base(); bad.grip[1] = NAN; bad.hand[0][1] = NAN; bad.n_en = 20; for (int i = 0; i < 5; i++) upd(&g, &bad, &o);
    CHECK(o.press == 0);
    /* desactivado */
    vrg_cfg_defaults(&g.cfg); g.cfg.enable = 0; in = base(); in.n_en = 1; in.en[0].p[2] = 1.5f;
    int sq = run_punch(&g, in, 0.6f, 6, NULL); CHECK(sq == 0);
    /* mano sin seguimiento */
    vrg_init(&g); in = base(); in.n_en = 1; in.en[0].p[2] = 1.5f; in.tracked[1] = 0; sq = run_punch(&g, in, 0.6f, 6, NULL); CHECK(sq == 0);
}

/* ---- revisión adversarial: saltos de pose, armas, hápticos, agarre ---- */
static int count_sq(vrg_t *g, vrg_in_t *in, int n) { vrg_out_t o; int c = 0; for (int i = 0; i < n; i++) { upd(g, in, &o); c += (o.press & VRG_SQUARE) != 0; } return c; }

static void t_pose_jumps(void) {
    vrg_t g; vrg_out_t o; vrg_in_t in;
    /* la mano salta 1 m hacia delante en una muestra (seguimiento): no es un golpe */
    vrg_init(&g); in = base(); in.n_en = 1; in.en[0].p[2] = 1.5f; count_sq(&g, &in, 10);
    in.hand[1][2] += 1.0f; int sq = count_sq(&g, &in, 1); in.hand[1][2] += 0.0f; sq += count_sq(&g, &in, 30);
    CHECK(sq == 0);
    /* recentrado: cabeza y manos saltan 0,4 m juntas y luego quietas: nada */
    vrg_init(&g); in = base(); in.n_en = 1; in.en[0].p[2] = 1.5f; count_sq(&g, &in, 10);
    in.head[2] += 0.4f; in.hand[1][2] += 0.4f; in.hand[0][2] += 0.4f; sq = count_sq(&g, &in, 30);
    CHECK(sq == 0);
    /* recentrado corto (0,2 m, bajo el umbral de salto de la mano pero con la cabeza saltando igual): nada */
    vrg_init(&g); in = base(); in.n_en = 1; in.en[0].p[2] = 1.5f; count_sq(&g, &in, 10);
    in.head[2] += 0.2f; in.hand[1][2] += 0.2f; in.hand[0][2] += 0.2f; sq = count_sq(&g, &in, 30);
    CHECK(sq == 0);
    /* recentrado que gira: el rumbo de la cabeza cambia 90 grados de golpe, las manos (en espacio de usuario) saltan */
    vrg_init(&g); in = base(); in.n_en = 1; in.en[0].p[2] = 1.5f; count_sq(&g, &in, 10);
    in.head_fwd[0] = 1; in.head_fwd[1] = 0; in.hand[1][2] = 0.5f; in.hand[1][0] = 0.1f; sq = count_sq(&g, &in, 1);
    in.hand[1][2] = 0.1f; in.hand[1][0] = 0.5f; sq += count_sq(&g, &in, 5); CHECK(sq == 0);
    /* pérdida de seguimiento: reaparece lejos y se queda quieta; ni siquiera una mano que reaparece moviéndose cuenta hasta tener historia */
    vrg_init(&g); in = base(); in.n_en = 1; in.en[0].p[2] = 1.5f; count_sq(&g, &in, 10);
    in.tracked[1] = 0; count_sq(&g, &in, 5);
    in.tracked[1] = 1; in.hand[1][2] = 0.7f; sq = count_sq(&g, &in, 1); in.hand[1][2] = 0.1f; sq += count_sq(&g, &in, 10); CHECK(sq == 0);
    /* velocidad imposible sostenida (pose basura que sube 0,5 m por muestra): descartada */
    vrg_init(&g); in = base(); in.n_en = 1; in.en[0].p[2] = 1.5f; count_sq(&g, &in, 10); sq = 0;
    for (int i = 1; i <= 8; i++) { in.hand[1][2] = 0.1f + 0.5f * i; sq += count_sq(&g, &in, 1); } CHECK(sq == 0);
    (void)o;
}

/* marcas de tiempo de la pose: el golpe se mide en tiempo real, no por llamada */
static void t_timestamps(void) {
    vrg_t g; vrg_out_t o; vrg_in_t in;
    for (int pass = 0; pass < 3; pass++) {
        /* pass 0: 5 m/s con marcas cada 20 ms: golpe. pass 1: las mismas posiciones con marcas cada 100 ms (1 m/s): nada.
         * pass 2: el render repite la muestra (misma marca y pose) en llamadas alternas: golpe igualmente */
        vrg_init(&g); in = base(); in.n_en = 1; in.en[0].p[2] = 1.5f; in.grip[1] = 1.0f; double t = 100.0; int sq = 0, ev = 0;
        for (int i = 0; i < 10; i++) { in.hand_time[0] = in.hand_time[1] = t; upd(&g, &in, &o); t += 0.02; }
        for (int i = 1; i <= 24; i++) {
            int fresh = pass != 2 || (i & 1);
            if (fresh) {
                int k = pass == 2 ? (i + 1) / 2 : i; if (k > 6) k = 6;
                in.hand[1][2] = 0.1f + 0.1f * k;
                t += pass == 1 ? 0.1 : 0.02;
            }
            in.hand_time[0] = in.hand_time[1] = t; upd(&g, &in, &o); sq += (o.press & VRG_SQUARE) != 0; ev += o.event == VRG_EV_PUNCH;
        }
        if (pass == 1) CHECK(sq == 0);
        else CHECK(ev == 1 && sq == 4);
    }
}

static void t_weapon_lock(void) {
    vrg_t g; vrg_out_t o; vrg_in_t in = base(); in.n_en = 1; in.en[0].p[2] = 1.5f;
    /* maniobra de recarga: el juego desequipa el arma (weapon -1) pero la capa VR sigue mostrando el arma */
    vrg_init(&g); in.weapon_busy = 1; count_sq(&g, &in, 10);
    int sq = run_punch(&g, in, 0.6f, 6, NULL); CHECK(sq == 0);
    /* arma desequipada hace un momento (cambio de arma): 0,8 s de calma, luego sí hay golpes */
    vrg_init(&g); in = base(); in.n_en = 1; in.en[0].p[2] = 1.5f; in.weapon = 2; count_sq(&g, &in, 10); in.weapon = -1;
    sq = run_punch(&g, in, 0.6f, 6, NULL); CHECK(sq == 0);
    for (int i = 0; i < 60; i++) upd(&g, &in, &o);
    sq = run_punch(&g, in, 0.6f, 6, NULL); CHECK(sq == 4);
    /* pared con arma VR: un gesto rápido del arma no golpea la pared */
    vrg_init(&g); in = base(); in.on_wall = 1; in.weapon = 1; count_sq(&g, &in, 10);
    int circ = 0; for (int i = 1; i <= 6; i++) { in.hand[1][2] = 0.1f - 0.1f * i; upd(&g, &in, &o); circ += (o.press & VRG_CIRCLE) != 0; } CHECK(circ == 0);
    /* retroceso de la mano hacia el cuerpo en la pared: no es golpe (solo hacia fuera) */
    vrg_init(&g); in = base(); in.on_wall = 1; in.hand[1][2] = 0.8f; count_sq(&g, &in, 10);
    circ = 0; for (int i = 1; i <= 6; i++) { in.hand[1][2] = 0.8f - 0.1f * i; upd(&g, &in, &o); circ += (o.press & VRG_CIRCLE) != 0; } CHECK(circ == 0);
}

static void t_grip_latch(void) {
    vrg_t g; vrg_out_t o; vrg_in_t in = base();
    in.n_en = 1; in.en[0].p[1] = 1.0f; in.en[0].p[2] = 0.9f; in.hand[1][0] = 0.1f; in.hand[1][1] = 1.45f; in.hand[1][2] = 0.85f;
    vrg_init(&g); count_sq(&g, &in, 10); in.grip[1] = in.trig[1] = 1.0f; count_sq(&g, &in, 10);
    /* aparece un arma con el grip cerrado: se suelta el cuadrado pero R1/L1 siguen retirados hasta abrir el grip */
    in.weapon = 1; int masked = 0, sq = 0;
    for (int i = 0; i < 20; i++) { upd(&g, &in, &o); masked += o.mask != 0; sq += (o.press & VRG_SQUARE) != 0; }
    CHECK(sq == 0 && masked == 20);
    in.grip[1] = in.trig[1] = 0; for (int i = 0; i < 3; i++) upd(&g, &in, &o); CHECK(o.mask == 0);
    /* seguimiento perdido con el grip cerrado: R1/L1 retirados como máximo 0,5 s */
    vrg_init(&g); in.weapon = -1; count_sq(&g, &in, 10); in.grip[1] = in.trig[1] = 1.0f; count_sq(&g, &in, 10);
    in.tracked[1] = 0; int m = 0; for (int i = 0; i < 40; i++) { upd(&g, &in, &o); m += o.mask != 0; }
    CHECK(m > 5 && m <= 27 && o.mask == 0 && !(o.press & VRG_SQUARE));
    /* perder el control (menú, escena) con el grip cerrado: tampoco queda nada bloqueado más de 0,5 s */
    vrg_init(&g); in.tracked[1] = 1; in.grip[1] = in.trig[1] = 0; count_sq(&g, &in, 10); in.grip[1] = in.trig[1] = 1.0f; count_sq(&g, &in, 10);
    in.active = 0; m = 0; for (int i = 0; i < 40; i++) { upd(&g, &in, &o); m += o.mask != 0; }
    CHECK(m <= 27 && o.mask == 0 && o.press == 0);
    /* grip ya cerrado al volver el control (tras un reinicio): no agarra */
    vrg_init(&g); in.active = 1; in.grip[1] = in.trig[1] = 1.0f; sq = count_sq(&g, &in, 12); CHECK(sq == 0);
    /* grip cerrado mientras había arma y se queda sin ella cerca de un enemigo: tampoco es un flanco */
    vrg_init(&g); in.weapon = 1; count_sq(&g, &in, 20); in.weapon = -1; sq = count_sq(&g, &in, 80); CHECK(sq == 0);
    /* sujetando ya al enemigo, apretar de nuevo con la mano algo apartada del cuello sigue siendo un toque */
    vrg_init(&g); in.grip[1] = in.trig[1] = 0; in.holding = 1; in.hand[1][0] = 0.1f; in.hand[1][1] = 1.45f; in.hand[1][2] = 0.85f; count_sq(&g, &in, 10);
    in.hand[1][0] = 0.5f; in.hand[1][1] = 1.1f; in.grip[1] = in.trig[1] = 1.0f; sq = count_sq(&g, &in, 10); CHECK(sq > 0);
}

static void t_peek_axis_haptics(void) {
    vrg_t g; vrg_out_t o; vrg_in_t in = base();
    /* girar la cabeza (cabeza algo desplazada del origen) estando pegado no es inclinarse */
    in.on_wall = 1; in.head[0] = 0.1f; in.head[2] = 0.1f; vrg_init(&g); count_sq(&g, &in, 10);
    int tri = 0;
    for (int i = 1; i <= 30; i++) { float a = 3.14159f * 0.5f * i / 30; in.head_fwd[0] = sinf(a); in.head_fwd[1] = cosf(a); upd(&g, &in, &o); tri += (o.press & VRG_TRIANGLE) != 0; }
    CHECK(tri == 0);
    /* vida bajando cada VBlank (fuego, gas): como mucho un pulso cada 0,2 s */
    vrg_init(&g); in = base(); count_sq(&g, &in, 5); int hur = 0;
    for (int i = 0; i < 50; i++) { in.life = 256 - 3 * (i + 1); upd(&g, &in, &o); hur += o.event == VRG_EV_HURT; }
    CHECK(hur >= 3 && hur <= 6);
    /* pared que parpadea: no inunda */
    vrg_init(&g); in = base(); int w = 0;
    for (int i = 0; i < 50; i++) { in.on_wall = i & 1; upd(&g, &in, &o); w += o.event == VRG_EV_WALL; }
    CHECK(w <= 4);
}

/* GOLPEAR != AGARRAR: el puño (agarre cerrado) con la mano en el cuello del enemigo NO agarra; solo el gatillo agarra */
static void t_fist_vs_grab(void) {
    vrg_t g; vrg_init(&g); vrg_in_t in = base(); vrg_out_t o;
    in.n_en = 1; in.en[0].p[1] = 1.0f; in.en[0].p[2] = 0.9f;
    in.hand[1][0] = 0.1f; in.hand[1][1] = 1.45f; in.hand[1][2] = 0.85f;
    for (int i = 0; i < 10; i++) upd(&g, &in, &o);
    in.grip[1] = 1.0f;                                /* puño cerrado junto al cuello */
    int sq = 0, grab = 0, jab = 0;
    for (int i = 0; i < 30; i++) { upd(&g, &in, &o); sq += (o.press & VRG_SQUARE) != 0; grab += o.event == VRG_EV_GRAB; jab += o.event == VRG_EV_JAB; }
    CHECK(sq == 0 && grab == 0 && jab == 0);
    /* golpe con el puño cerrado hacia el enemigo: toque de 4 VBlanks y nunca agarre */
    vrg_init(&g); in = base(); in.n_en = 1; in.en[0].p[2] = 1.5f; in.en[0].p[1] = 1.0f; in.grip[1] = 1.0f; in.grip[0] = 1.0f;
    for (int i = 0; i < 10; i++) upd(&g, &in, &o);
    sq = 0; grab = 0; int pun = 0, rmask = 0, imp = 0, maxh = 0; float hmax = 0;
    for (int i = 1; i <= 60; i++) {
        in.hand[1][2] = 0.1f + 0.6f * (i > 6 ? 1.0f : i / 6.0f);
        in.hand[1][1] = 1.2f; in.en[0].p[2] = 1.1f;
        upd(&g, &in, &o);
        sq += (o.press & VRG_SQUARE) != 0; grab += o.event == VRG_EV_GRAB; pun += o.event == VRG_EV_PUNCH; imp += o.event == VRG_EV_IMPACT; rmask += (o.mask & (VRG_R1 | VRG_L1)) != 0;
        if (o.haptic[1] > hmax) hmax = o.haptic[1];
        (void)maxh;
    }
    CHECK(sq == 4 && grab == 0 && pun == 1 && imp == 1 && hmax >= 0.9f && rmask > 10);
    /* el impacto también llega si baja la vida del enemigo sin que la mano llegue a tocarlo */
    vrg_init(&g); in = base(); in.n_en = 1; in.en[0].p[2] = 2.4f; in.en[0].vital = 100; in.grip[1] = 1.0f;
    for (int i = 0; i < 10; i++) upd(&g, &in, &o);
    imp = 0; for (int i = 1; i <= 20; i++) { in.hand[1][2] = 0.1f + 0.6f * (i > 6 ? 1.0f : i / 6.0f); if (i == 12) in.en[0].vital = 80; upd(&g, &in, &o); imp += o.event == VRG_EV_IMPACT; }
    CHECK(imp == 1);
    /* gatillo con la mano en el cuello = agarre mantenido; las manos del personaje reciben lado y enemigo */
    vrg_init(&g); in = base(); in.n_en = 1; in.en[0].p[1] = 1.0f; in.en[0].p[2] = 0.9f;
    in.hand[1][0] = 0.1f; in.hand[1][1] = 1.45f; in.hand[1][2] = 0.85f;
    for (int i = 0; i < 10; i++) upd(&g, &in, &o);
    in.trig[1] = 1.0f; sq = 0; int gs = -2, ge = -2, ms = 0;
    for (int i = 0; i < 20; i++) { upd(&g, &in, &o); sq += (o.press & VRG_SQUARE) != 0; ms += (o.mask & VRG_SQUARE) != 0; gs = o.grab_side; ge = o.grab_en; }
    CHECK(sq >= 18 && gs == 1 && ge == 0 && ms == 20);
    /* gatillo cerrado por el botón A de la otra mano (cuadrado físico): cuenta como gatillo dominante */
    vrg_init(&g); in.trig[1] = 0; in.sq_user = 0; for (int i = 0; i < 10; i++) upd(&g, &in, &o);
    in.sq_user = 1; sq = 0; for (int i = 0; i < 20; i++) { upd(&g, &in, &o); sq += (o.press & VRG_SQUARE) != 0; } CHECK(sq >= 18 && o.grab_side == 1);
    /* sin seguimiento de manos el cuadrado físico pasa intacto (modo botón) */
    vrg_init(&g); in = base(); in.tracked[0] = in.tracked[1] = 0; in.sq_user = 1; for (int i = 0; i < 10; i++) upd(&g, &in, &o);
    CHECK(!(o.mask & VRG_SQUARE) && o.jab_side == -1);
}

/* golpe de botón (accesibilidad): sin enemigo cerca de la mano, el cuadrado físico da UN toque corto y pide el golpe sintético de la mano; mantenerlo no repite */
static void t_button_jab(void) {
    vrg_t g; vrg_init(&g); vrg_in_t in = base(); vrg_out_t o;
    in.n_en = 1; in.en[0].p[2] = 1.5f;
    for (int i = 0; i < 10; i++) upd(&g, &in, &o);
    in.sq_user = 1; in.trig[1] = 1.0f;
    int sq = 0, jabs = 0, js = -2, masked = 0;
    for (int i = 0; i < 40; i++) { upd(&g, &in, &o); sq += (o.press & VRG_SQUARE) != 0; masked += (o.mask & VRG_SQUARE) != 0; if (o.jab_side >= 0) { jabs++; js = o.jab_side; } }
    CHECK(sq == 4 && jabs == 1 && js == 1 && masked == 40);
    in.sq_user = 0; in.trig[1] = 0; for (int i = 0; i < 20; i++) upd(&g, &in, &o);
    in.sq_user = 1; in.trig[1] = 1.0f; jabs = 0; for (int i = 0; i < 40; i++) { upd(&g, &in, &o); jabs += o.jab_side >= 0; }
    CHECK(jabs == 1);                         /* otra pulsación = otro golpe */
    /* con arma: el cuadrado pasa tal cual (lo gobierna el arma) */
    vrg_init(&g); in.weapon = 1; in.sq_user = 0; in.trig[1] = 0; for (int i = 0; i < 10; i++) upd(&g, &in, &o);
    in.sq_user = 1; in.trig[1] = 1.0f; jabs = 0; masked = 0; for (int i = 0; i < 20; i++) { upd(&g, &in, &o); jabs += o.jab_side >= 0; masked += (o.mask & VRG_SQUARE) != 0; }
    CHECK(jabs == 0 && masked == 0);
}

/* puño: el golpe solo cuenta con la mano en puño (grip cerrado con histéresis); háptico suave al cerrar; el grip no abre el menú junto a un enemigo */
static int punch_with(vrg_cfg_t *cfg, float grip, float grip_then, int *haptic_closes) {
    vrg_t g; vrg_init(&g); if (cfg) g.cfg = *cfg;
    vrg_in_t in = base(); vrg_out_t o; in.n_en = 1; in.en[0].p[2] = 1.5f; in.en[0].p[1] = 1.0f;
    for (int i = 0; i < 10; i++) upd(&g, &in, &o);        /* mano abierta primero: cerrar es un flanco */
    in.grip[1] = grip; for (int i = 0; i < 10; i++) { upd(&g, &in, &o); if (haptic_closes && o.haptic[1] > 0.05f && o.haptic[1] < 0.3f) (*haptic_closes)++; }
    in.grip[1] = grip_then; int sq = 0;
    for (int i = 1; i <= 18; i++) { in.hand[1][2] = 0.1f + 0.6f * (i > 6 ? 1.0f : i / 6.0f); upd(&g, &in, &o); sq += (o.press & VRG_SQUARE) != 0; }
    return sq;
}
static void t_fist(void) {
    int hc = 0;
    CHECK(punch_with(NULL, 0.0f, 0.0f, NULL) == 0);                /* mano abierta: el movimiento rápido NO golpea */
    CHECK(punch_with(NULL, 0.5f, 0.5f, NULL) == 0);                /* a medio cerrar (bajo el umbral de cierre 0,65): tampoco */
    CHECK(punch_with(NULL, 1.0f, 1.0f, &hc) == 4);                 /* puño: golpe */
    CHECK(hc == 1);                                                /* háptico suave una sola vez al cerrar (0,12) */
    CHECK(punch_with(NULL, 1.0f, 0.5f, NULL) == 4);                /* histéresis: cerrado a 1,0 y relajado a 0,5 (> 0,35) sigue siendo puño */
    CHECK(punch_with(NULL, 1.0f, 0.3f, NULL) == 0);                /* abrir la mano (< 0,35) en pleno movimiento: ya no golpea */
    vrg_cfg_t legacy; vrg_cfg_defaults(&legacy); legacy.fist_required = 0;
    CHECK(punch_with(&legacy, 0.0f, 0.0f, NULL) == 4);             /* fist_required = 0: comportamiento anterior */
    vrg_cfg_t nohap; vrg_cfg_defaults(&nohap); nohap.fist_haptic = 0; hc = 0;
    punch_with(&nohap, 1.0f, 1.0f, &hc); CHECK(hc == 0);
    /* sin manos libres (arma) no hay háptico de puño ni golpe */
    { vrg_t g; vrg_init(&g); vrg_in_t in = base(); vrg_out_t o; in.weapon = 1; in.n_en = 1; in.en[0].p[2] = 1.5f; int h = 0;
      for (int i = 0; i < 6; i++) upd(&g, &in, &o);
      in.grip[1] = 1.0f; for (int i = 0; i < 10; i++) { upd(&g, &in, &o); h += o.haptic[1] > 0.05f; } CHECK(h == 0); }
    /* el grip cerrado junto a un enemigo no llega al juego como R1/L1; lejos o sin grip, sí (equipar/objetos) */
    { vrg_t g; vrg_init(&g); vrg_in_t in = base(); vrg_out_t o; in.n_en = 1; in.en[0].p[2] = 1.5f;
      for (int i = 0; i < 10; i++) upd(&g, &in, &o);
      CHECK(!(o.mask & (VRG_R1 | VRG_L1)));
      in.grip[1] = 1.0f; for (int i = 0; i < 5; i++) upd(&g, &in, &o); CHECK((o.mask & VRG_R1) && (o.mask & VRG_L1));
      in.grip[1] = 0.0f; for (int i = 0; i < 5; i++) upd(&g, &in, &o); CHECK(!(o.mask & (VRG_R1 | VRG_L1)));
      in.en[0].p[2] = 9.0f; in.grip[1] = 1.0f; for (int i = 0; i < 5; i++) upd(&g, &in, &o); CHECK(!(o.mask & (VRG_R1 | VRG_L1))); }
    /* grip ya cerrado al volver el control (reinicio): sin háptico de cierre */
    { vrg_t g; vrg_init(&g); vrg_in_t in = base(); vrg_out_t o; in.grip[1] = 1.0f; int h = 0; for (int i = 0; i < 6; i++) { upd(&g, &in, &o); h += o.haptic[1] > 0.05f; } CHECK(h == 0); }
}

int main(void) {
    t_pose_jumps(); t_timestamps(); t_weapon_lock(); t_grip_latch(); t_peek_axis_haptics();
    t_punch(); t_fist(); t_walk(); t_grab(); t_fist_vs_grab(); t_button_jab(); t_wall(); t_haptics_robust();
    printf("%d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
