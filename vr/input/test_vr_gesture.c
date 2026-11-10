/* Pruebas sintéticas de los gestos de cuerpo: el puño es el BOTÓN (R1/L1), el golpe/agarre/apretón son decisiones para el motor
 * (no botones sintéticos), la ventana de pecho y el respaldo del cuadrado. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "vr_gesture.h"
#include "vr_input.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void upd(vrg_t *g, vrg_in_t *in, vrg_out_t *o) {   /* en el test espacio de usuario == mundo */
    for (int s = 0; s < 2; s++) for (int i = 0; i < 3; i++) { in->hrel[s][i] = in->hand[s][i] - in->head[i]; in->hand_u[s][i] = in->hand[s][i] - in->body[i]; }
    memcpy(in->head_u, in->head, sizeof in->head_u);
    vrg_update(g, in, o);
}
static vrg_in_t base(void) {
    vrg_in_t in; memset(&in, 0, sizeof in);
    in.dt = 0.02f; in.active = 1; in.weapon = -1; in.life = 256;
    in.head[1] = 1.6f; in.body[1] = 1.0f;
    in.tracked[0] = in.tracked[1] = 1;
    in.hand[0][0] = -0.2f; in.hand[0][1] = 1.2f; in.hand[0][2] = 0.1f;
    in.hand[1][0] = 0.2f;  in.hand[1][1] = 1.2f; in.hand[1][2] = 0.1f;
    in.head_fwd[1] = 1.0f;     /* delante = +Z con yaw 0 */
    return in;
}
static void settle(vrg_t *g, vrg_in_t *in, int n) { vrg_out_t o; for (int i = 0; i < n; i++) upd(g, in, &o); }

/* golpe: la mano derecha avanza `dist` m en `n` pasos. Devuelve los eventos de golpe; `sq` cuenta VBlanks con cuadrado en `press` (debe ser 0 siempre) */
static int run_punch(vrg_t *g, vrg_in_t in, int fist, float dist, int n, int *sq, int *squeeze) {
    vrg_out_t o; int ev = 0;
    in.fist_btn[1] = fist;
    settle(g, &in, 12);
    for (int i = 1; i <= n + 12; i++) {
        float t = i > n ? 1.0f : (float)i / n;
        in.hand[1][2] = 0.1f + dist * t;
        upd(g, &in, &o);
        if (o.press & VRG_SQUARE) { if (sq) (*sq)++; }
        if (o.punch_side == 1) ev++;
        if (o.squeeze && squeeze) (*squeeze)++;
    }
    return ev;
}
static vrg_in_t with_enemy(void) { vrg_in_t in = base(); in.n_en = 1; in.en[0].p[2] = 1.5f; in.en[0].p[1] = 1.0f; return in; }

static void t_punch(void) {
    vrg_t g; vrg_init(&g); vrg_in_t in = with_enemy(); int sq = 0;
    CHECK(run_punch(&g, in, 1, 0.6f, 6, &sq, NULL) == 1);                 /* puño + golpe rápido = petición de golpe */
    CHECK(sq == 0);                                                        /* nunca un cuadrado sintético */
    vrg_init(&g); CHECK(run_punch(&g, in, 0, 0.6f, 6, NULL, NULL) == 0);   /* mano abierta: nada */
    vrg_init(&g); CHECK(run_punch(&g, in, 1, 0.6f, 90, NULL, NULL) == 0);  /* lento (0,33 m/s) */
    vrg_init(&g); vrg_in_t aw = in; aw.weapon = 0; CHECK(run_punch(&g, aw, 1, 0.6f, 6, NULL, NULL) == 0);   /* con arma: nada */
    vrg_init(&g); vrg_in_t ne = in; ne.n_en = 0; CHECK(run_punch(&g, ne, 1, 0.6f, 6, NULL, NULL) == 0);
    vrg_init(&g); vrg_in_t fa = in; fa.en[0].p[2] = 9; CHECK(run_punch(&g, fa, 1, 0.6f, 6, NULL, NULL) == 0);
    vrg_init(&g); vrg_in_t ia = in; ia.active = 0; CHECK(run_punch(&g, ia, 1, 0.6f, 6, NULL, NULL) == 0);
    /* hacia atrás: no */
    vrg_init(&g); vrg_out_t o; int cnt = 0; in.fist_btn[1] = 1; settle(&g, &in, 12);
    for (int i = 1; i <= 10; i++) { in.hand[1][2] = 0.1f - 0.1f * i; upd(&g, &in, &o); cnt += o.punch_side >= 0; }
    CHECK(cnt == 0);
    /* dos golpes separados 0,6 s: dos peticiones (enfriamiento 0,2 s) */
    vrg_init(&g); in = with_enemy(); in.fist_btn[1] = 1; settle(&g, &in, 12); cnt = 0;
    for (int k = 0; k < 2; k++) {
        for (int i = 1; i <= 6; i++) { in.hand[1][2] = 0.1f + 0.1f * i; upd(&g, &in, &o); cnt += o.punch_side == 1; }
        for (int i = 0; i < 30; i++) { in.hand[1][2] = 0.1f; upd(&g, &in, &o); cnt += o.punch_side == 1; }
    }
    CHECK(cnt == 2);
    /* el grip analógico NO decide: grip 1 sin botón no es puño */
    vrg_init(&g); vrg_in_t ga = with_enemy(); ga.grip[1] = 1.0f; CHECK(run_punch(&g, ga, 0, 0.6f, 6, NULL, NULL) == 0);
    /* el cuadrado físico pasa intacto: no se retira */
    vrg_init(&g); in = with_enemy(); in.fist_btn[1] = 1; settle(&g, &in, 12); upd(&g, &in, &o);
    CHECK(!(o.mask & VRG_SQUARE) && !(o.press & VRG_SQUARE));
    CHECK((o.mask & VRG_R1) && (o.mask & VRG_L1));                         /* R1/L1 nunca llegan al juego */
}

/* golpe por CONTACTO: el puño atraviesa el volumen del enemigo, en cualquier dirección razonable; sin pared de por medio */
static int run_path(vrg_t *g, vrg_in_t in, const float a[3], const float b[3], int n, int fist, int *contact) {
    vrg_out_t o; int ev = 0;
    in.fist_btn[1] = fist; in.hand[1][0] = a[0]; in.hand[1][1] = a[1]; in.hand[1][2] = a[2];
    settle(g, &in, 12);
    for (int i = 1; i <= n + 10; i++) {
        float t = i > n ? 1.0f : (float)i / n;
        for (int k = 0; k < 3; k++) in.hand[1][k] = a[k] + (b[k] - a[k]) * t;
        upd(g, &in, &o);
        if (o.punch_side == 1) { ev++; if (contact && o.gest.valid) *contact = o.gest.contact; }
    }
    return ev;
}
static void chest_center(const vrg_in_t *in, int s, float c[3]);
static int hold_trig(vrg_t *g, vrg_in_t *in, int s, int after, vrg_out_t *o);
static void t_contact(void) {
    vrg_t g; vrg_in_t in = base(); in.n_en = 1; in.en[0].p[0] = 0; in.en[0].p[1] = 1.0f; in.en[0].p[2] = 0.9f; in.fist_btn[1] = 1;
    float a[3] = {0.2f, 1.2f, 0.1f}, b[3] = {0.1f, 1.2f, 0.6f};            /* directo corto: el puño acaba a 0,3 m del eje del enemigo (dentro del cuerpo + tolerancia) */
    int ct = 0;
    vrg_init(&g); CHECK(run_path(&g, in, a, b, 8, 1, &ct) == 1);                          /* puño a 0,3 m del cuerpo que impacta */
    vrg_init(&g); CHECK(run_path(&g, in, a, b, 8, 0, NULL) == 0);                          /* sin el botón del puño: nada */
    vrg_init(&g); CHECK(run_path(&g, in, a, b, 70, 1, NULL) == 0);                         /* puño lento (0,1 m/s): no impacta */
    /* gancho: arco lateral a la altura del pecho que cruza el volumen (sin avance radial) */
    float h0[3] = {-0.4f, 1.3f, 0.55f}, h1[3] = {0.4f, 1.3f, 0.55f};
    vrg_init(&g); CHECK(run_path(&g, in, h0, h1, 14, 1, &ct) == 1 && ct == 1);
    vrg_init(&g); vrg_in_t fl = in; fl.n_en = 0; CHECK(run_path(&g, fl, h0, h1, 14, 1, NULL) == 0);   /* sin enemigo: nada */
    /* enemigo detrás de una pared (blocked): no impacta */
    vrg_init(&g); vrg_in_t bl = in; bl.en[0].blocked = 1; CHECK(run_path(&g, bl, a, b, 8, 1, NULL) == 0);
    vrg_init(&g); CHECK(run_path(&g, bl, h0, h1, 14, 1, NULL) == 0);
    /* enemigo caído o muy lejos del puño: no */
    vrg_init(&g); vrg_in_t dn = in; dn.en[0].down = 1; dn.en[0].p[2] = 9; CHECK(run_path(&g, dn, a, b, 8, 1, NULL) == 0);
    /* recogerse hacia la cabeza dentro del volumen no es un golpe */
    float r0[3] = {0.1f, 1.2f, 0.6f}, r1[3] = {0.2f, 1.2f, 0.1f};
    vrg_init(&g); vrg_in_t cl = in; cl.en[0].p[2] = 0.4f; CHECK(run_path(&g, cl, r0, r1, 8, 1, NULL) == 0);
    /* ventana abierta en la zona del pecho, puño cerrado y enemigo delante: el golpe cancela la ventana y pega */
    {
        vrg_out_t o; vrg_init(&g); vrg_in_t m = in; m.fist_btn[1] = 0; float c1[3]; chest_center(&m, 1, c1);
        memcpy(m.hand[1], c1, sizeof c1); settle(&g, &m, 12); hold_trig(&g, &m, 1, 15, &o);
        CHECK(o.menu_open[1] == 1);
        m.fist_btn[1] = 1;      /* puño cerrado con la ventana abierta: el golpe de verdad la cancela */
        int ev = 0, still = 0; float x[3];
        for (int i = 1; i <= 14; i++) { for (int k = 0; k < 3; k++) x[k] = c1[k] + ((k == 2 ? 0.55f : (k == 0 ? -0.1f : 0.0f)) * (i > 8 ? 1.0f : i / 8.0f)); memcpy(m.hand[1], x, sizeof x); upd(&g, &m, &o); ev += o.punch_side == 1; still += (o.press & VRG_R2) != 0 && ev > 0; }
        CHECK(ev >= 1); CHECK(still == 0);
    }
    /* umbrales reales del registro: 1,38 m/s hacia delante con alcance 0,25 m y un enemigo a 0,5 m es un golpe */
    {
        vrg_init(&g); vrg_in_t q = base(); q.n_en = 1; q.en[0].p[2] = 0.7f; q.en[0].p[1] = 1.0f; float s0[3] = {0.2f, 1.2f, 0.05f}, s1[3] = {0.2f, 1.2f, 0.28f};
        CHECK(run_path(&g, q, s0, s1, 9, 1, NULL) == 1);
    }
}

static vrg_in_t grab_setup(void);
static void t_throw(void);
/* barrido continuo y reglas de agarre vs puño */
static void t_rules(void) {
    vrg_out_t o; vrg_t g;
    /* puño a ~4 m/s con fotogramas de 0,09 s (0,36 m por fotograma, un fotograma perdido): ningún fotograma cae dentro de la cápsula fina, el segmento sí la cruza */
    vrg_init(&g); g.cfg.body_radius = 0.02f; g.cfg.fist_radius = 0.02f; g.cfg.contact_tol = 0.0f; g.cfg.melee_range = 0;
    vrg_in_t in = base(); in.dt = 0.09f; in.n_en = 1; in.en[0].p[0] = 0.0f; in.en[0].p[1] = 1.0f; in.en[0].p[2] = 0.5f; in.fist_btn[1] = 1;
    in.hand[1][0] = -1.3f; in.hand[1][1] = 1.2f; in.hand[1][2] = 0.5f; settle(&g, &in, 8);
    int ev = 0, ct = 0;
    for (int i = 1; i <= 8; i++) { in.hand[1][0] = -1.3f + 0.36f * i; upd(&g, &in, &o); if (o.punch_side == 1) { ev++; ct = o.gest.contact; } }
    CHECK(ev == 1);                                                    /* x = -0.94,-0.58,-0.22,0.14...: el enemigo en x=0 se salta entre dos fotogramas */
    (void)ct;
    /* acercamiento rápido con pulsación tardía (el botón se pulsa a mitad del movimiento): puño, nunca agarre */
    vrg_init(&g); in = grab_setup(); in.hand[1][0] = 0.1f; in.hand[1][1] = 1.4f; in.hand[1][2] = 0.4f; settle(&g, &in, 12);
    int gs = 0, pn = 0;
    for (int i = 1; i <= 14; i++) { in.hand[1][2] = 0.4f + 0.08f * i; if (i == 4) in.fist_btn[1] = 1; upd(&g, &in, &o); gs += o.grab_start; pn += o.punch_side == 1; }
    CHECK(gs == 0 && pn >= 1);
    /* mano quieta en el cuello + pulsación: agarre (llega al motor tras el margen) */
    vrg_init(&g); in = grab_setup(); in.hand[1][0] = 0.1f; in.hand[1][1] = 1.45f; in.hand[1][2] = 1.15f; settle(&g, &in, 12);
    in.fist_btn[1] = 1; upd(&g, &in, &o); CHECK(o.grab_start == 0 && o.grab_why && !strcmp(o.grab_why, "pendiente"));
    gs = 0; pn = 0; for (int i = 0; i < 22; i++) { upd(&g, &in, &o); gs += o.grab_start; pn += o.punch_side >= 0; }
    CHECK(gs == 1 && pn == 0);
    /* pulsación y golpe inmediato: el agarre pendiente se cancela y sale el puño */
    vrg_init(&g); in = grab_setup(); in.hand[1][0] = 0.1f; in.hand[1][1] = 1.45f; in.hand[1][2] = 1.0f; settle(&g, &in, 12);
    in.fist_btn[1] = 1; upd(&g, &in, &o); gs = 0; pn = 0; int canc = 0;
    for (int i = 1; i <= 8; i++) { in.hand[1][2] = 1.0f + 0.1f * i; upd(&g, &in, &o); gs += o.grab_start; pn += o.punch_side == 1; canc += o.grab_why && !strcmp(o.grab_why, "cancelado_puno"); }
    CHECK(gs == 0 && pn == 1 && canc >= 1);
    /* mano rápida al pulsar: motivo mano_rapida */
    vrg_init(&g); in = grab_setup(); in.hand[1][0] = 0.1f; in.hand[1][1] = 1.45f; in.hand[1][2] = 0.75f; settle(&g, &in, 12);
    for (int i = 1; i <= 4; i++) { in.hand[1][2] = 0.75f + 0.06f * i; upd(&g, &in, &o); }
    in.fist_btn[1] = 1; in.hand[1][2] += 0.06f; upd(&g, &in, &o);  CHECK(o.grab_start == 0 && o.punch_side == 1 && o.grab_why && (!strcmp(o.grab_why, "tras_golpe") || !strcmp(o.grab_why, "mano_rapida")));   /* el puño sale en el mismo fotograma */
}

/* lanzamiento: botón pulsado junto al enemigo + tirón rápido en <= 0,3 s */
static int run_throw(vrg_in_t in, const float p0[3], const float d[3], int n, float *yaw, int *grabs, int *punches) {
    vrg_t g; vrg_out_t o; vrg_init(&g); memcpy(in.hand[1], p0, 3 * sizeof(float)); settle(&g, &in, 12);
    in.fist_btn[1] = 1; upd(&g, &in, &o); int th = 0, gs = 0, pn = 0;
    for (int i = 1; i <= n + 3; i++) { float t = i > n ? 1.0f : (float)i / n; for (int k = 0; k < 3; k++) in.hand[1][k] = p0[k] + d[k] * t; upd(&g, &in, &o);
        if (o.throw_side == 1) { th++; if (yaw) *yaw = o.throw_yaw; if (!o.grab_start || o.event != VRG_EV_THROW) th += 100; } gs += o.grab_start; pn += o.punch_side >= 0; }
    if (grabs) *grabs = gs;
    if (punches) *punches = pn;
    return th;
}
static void t_throw(void) {
    vrg_in_t in = grab_setup(); float yaw = 0; int gs, pn;
    float neck[3] = {0.1f, 1.45f, 1.15f};
    float back[3] = {0, 0, -0.5f}, side[3] = {0.5f, 0, 0}, down[3] = {0, -0.5f, 0}, fwd[3] = {0, 0, 0.5f};
    CHECK(run_throw(in, neck, back, 6, &yaw, &gs, &pn) == 1 && fabsf(fabsf(yaw) - 3.14159f) < 0.3f && gs == 1 && pn == 0);        /* hacia atrás del jugador */
    CHECK(run_throw(in, neck, side, 6, &yaw, &gs, &pn) == 1 && fabsf(yaw - 1.5708f) < 0.3f);                                          /* hacia un lado */
    CHECK(run_throw(in, neck, down, 6, &yaw, &gs, &pn) == 1 && fabsf(fabsf(yaw) - 3.14159f) < 0.3f);                                   /* hacia abajo: lejos del enemigo */
    float shoulder[3] = {0.4f, 1.2f, 1.3f};
    CHECK(run_throw(in, shoulder, side, 6, &yaw, &gs, &pn) == 1 && fabsf(yaw - 1.5708f) < 0.3f);                                       /* junto al hombro/brazo */
    CHECK(run_throw(in, neck, fwd, 6, &yaw, &gs, &pn) == 0 && gs == 0);                                                                /* hacia el enemigo: puño, no lanzamiento */
    CHECK(run_throw(in, neck, side, 16, &yaw, &gs, &pn) == 1 && fabsf(yaw - 1.5708f) < 0.3f);                                        /* tirón del registro real (~1,5 m/s, acelerando desde parado): no cancela el agarre pendiente */
    CHECK(run_throw(in, neck, back, 80, &yaw, &gs, &pn) == 0 && gs == 1);                                                              /* tirón lento: agarre */
    vrg_in_t fl = in; fl.n_en = 0; CHECK(run_throw(fl, neck, back, 6, &yaw, &gs, &pn) == 0);                                           /* sin enemigo */
}

/* andar rápido (cuerpo, cabeza y manos juntos 4 m/s): no es un golpe */
static void t_walk(void) {
    vrg_t g; vrg_init(&g); vrg_in_t in = with_enemy(); in.fist_btn[1] = 1; vrg_out_t o; int cnt = 0;
    settle(&g, &in, 12);
    for (int i = 1; i <= 30; i++) {
        float dz = 0.08f * i; in.body[2] = dz; in.head[2] = dz; in.hand[1][2] = 0.1f + dz; in.hand[0][2] = 0.1f + dz;
        upd(&g, &in, &o); cnt += o.punch_side >= 0;
    }
    CHECK(cnt == 0);
}

/* el agarre llega al motor tras un margen de 0,15 s sin que la mano se lance: avanza hasta que arranque (o 10 VBlanks) */
static void pgrab(vrg_t *g, vrg_in_t *in, vrg_out_t *o) { for (int i = 0; i < 22; i++) { upd(g, in, o); if (o->grab_start) return; } }
/* enemigo a 0,4 m delante del cuerpo mirando a +Z (le damos la espalda): cuello (0,1.45,1.3) y espalda (0,1.2,1.05) */
static vrg_in_t grab_setup(void) {
    vrg_in_t in = base(); in.n_en = 1; in.en[0].p[0] = 0; in.en[0].p[1] = 1.0f; in.en[0].p[2] = 1.3f; in.en[0].yaw = 0;
    in.body[2] = 0.9f; in.head[2] = 0.9f;
    return in;
}
static void t_grab(void) {
    vrg_out_t o; vrg_t g; vrg_init(&g); vrg_in_t in = grab_setup();
    /* mano derecha abierta cerca del cuello; se PULSA R1 -> agarre directo */
    in.hand[1][0] = 0.1f; in.hand[1][1] = 1.45f; in.hand[1][2] = 1.15f;
    settle(&g, &in, 12);
    in.fist_btn[1] = 1; pgrab(&g, &in, &o);
    CHECK(o.grab_start == 1 && o.grab_side == 1 && o.grab_en == 0 && o.grab_hold == 1 && o.event == VRG_EV_GRAB);
    CHECK(o.grab_why && !strcmp(o.grab_why, "ok"));
    CHECK(!(o.press & VRG_SQUARE) && (o.mask & VRG_R1));
    /* mantener = sigue sujetando; nunca otro arranque */
    int starts = 0, holds = 0; in.holding = 1;
    for (int i = 0; i < 20; i++) { upd(&g, &in, &o); starts += o.grab_start; holds += o.grab_hold; }
    CHECK(starts == 0 && holds == 20);
    /* una pulsación nueva (soltar y pulsar) = apretar */
    in.fist_btn[1] = 0; upd(&g, &in, &o); CHECK(o.grab_hold == 0 && o.event == VRG_EV_RELEASE);
    int sq = 0; in.fist_btn[1] = 1; upd(&g, &in, &o); sq += o.squeeze; CHECK(sq == 1 && o.event == VRG_EV_SQUEEZE && o.grab_start == 0);
    /* mano lejos del cuello/espalda (0,5 m): no agarra, motivo */
    vrg_init(&g); in = grab_setup(); in.hand[1][0] = 1.0f; in.hand[1][1] = 1.45f; in.hand[1][2] = 1.15f; settle(&g, &in, 12);
    in.fist_btn[1] = 1; upd(&g, &in, &o); CHECK(o.grab_start == 0 && o.grab_why && !strcmp(o.grab_why, "mano_lejos"));
    /* justo en el límite: 0,28 m del cuello sí, 0,34 m no */
    vrg_init(&g); in = grab_setup(); in.hand[1][0] = 0.28f; in.hand[1][1] = 1.45f; in.hand[1][2] = 1.3f; settle(&g, &in, 12);
    in.fist_btn[1] = 1; pgrab(&g, &in, &o); CHECK(o.grab_start == 1);
    vrg_init(&g); in = grab_setup(); in.hand[1][0] = 0.34f; in.hand[1][1] = 1.45f; in.hand[1][2] = 1.3f; settle(&g, &in, 12);
    in.fist_btn[1] = 1; upd(&g, &in, &o); CHECK(o.grab_start == 0);
    /* la espalda también vale: 0,1 m de (0,1.2,1.05) */
    vrg_init(&g); in = grab_setup(); in.hand[0][0] = 0.0f; in.hand[0][1] = 1.2f; in.hand[0][2] = 1.0f; settle(&g, &in, 12);
    in.fist_btn[0] = 1; pgrab(&g, &in, &o); CHECK(o.grab_start == 1 && o.grab_side == 0);
    /* REGLA DURA: con el puño ya cerrado (botón mantenido de antes) NUNCA se agarra, ni aunque la mano llegue al cuello */
    vrg_init(&g); in = grab_setup(); in.fist_btn[1] = 1; in.hand[1][0] = 0.6f; in.hand[1][1] = 1.2f; in.hand[1][2] = 0.5f; settle(&g, &in, 30);
    int st = 0; const char *why = NULL;
    for (int i = 1; i <= 20; i++) { float t = i / 20.0f; in.hand[1][0] = 0.6f * (1 - t) + 0.05f * t; in.hand[1][1] = 1.2f * (1 - t) + 1.45f * t; in.hand[1][2] = 0.5f * (1 - t) + 1.3f * t; upd(&g, &in, &o); st += o.grab_start; if (o.grab_why) why = o.grab_why; }
    CHECK(st == 0 && why && !strcmp(why, "puno_cerrado"));
    /* botón ya mantenido al recuperar el control (reinicio): no es una pulsación */
    vrg_init(&g); in = grab_setup(); in.hand[1][0] = 0.1f; in.hand[1][1] = 1.45f; in.hand[1][2] = 1.15f; in.fist_btn[1] = 1;
    st = 0; for (int i = 0; i < 6; i++) { upd(&g, &in, &o); st += o.grab_start; } CHECK(st == 0);
    /* sin enemigo: pulsar es solo puño */
    vrg_init(&g); in = base(); settle(&g, &in, 12); in.fist_btn[1] = 1; upd(&g, &in, &o); CHECK(o.grab_start == 0 && o.menu_open[1] == 0);
    /* cuerpo a 2 m del enemigo: el motor no llegaría */
    vrg_init(&g); in = grab_setup(); in.body[2] = -0.5f; in.hand[1][0] = 0.1f; in.hand[1][1] = 1.45f; in.hand[1][2] = 1.15f; settle(&g, &in, 12);
    in.fist_btn[1] = 1; upd(&g, &in, &o); CHECK(o.grab_start == 0 && o.grab_why && !strcmp(o.grab_why, "cuerpo_lejos"));
    /* no hay agarre tras un golpe */
    vrg_init(&g); in = grab_setup(); in.hand[1][0] = 0.1f; in.hand[1][1] = 1.45f; in.hand[1][2] = 1.0f; in.fist_btn[1] = 1; settle(&g, &in, 12);
    int pe = 0; for (int i = 1; i <= 6; i++) { in.hand[1][2] = 1.0f + 0.1f * i; upd(&g, &in, &o); pe += o.punch_side == 1; }
    CHECK(pe == 1);
    in.fist_btn[1] = 0; upd(&g, &in, &o); in.fist_btn[1] = 1; in.hand[1][2] = 1.15f; upd(&g, &in, &o);   /* soltar y pulsar justo después del golpe */
    CHECK(o.grab_start == 0 && o.grab_why && !strcmp(o.grab_why, "tras_golpe"));
    settle(&g, &in, 30); in.fist_btn[1] = 0; settle(&g, &in, 3); in.fist_btn[1] = 1; pgrab(&g, &in, &o); CHECK(o.grab_start == 1);   /* pasado el tiempo, sí */
    /* un golpe con el puño estando agarrado = apretar/rematar */
    vrg_init(&g); in = grab_setup(); in.holding = 1; in.hand[0][2] = 0.4f; in.fist_btn[0] = 1; in.hand[0][0] = -0.2f; settle(&g, &in, 12);
    int sqz = 0, pun = 0; for (int i = 1; i <= 8; i++) { in.hand[0][2] = 0.4f + 0.1f * i; upd(&g, &in, &o); sqz += o.squeeze; pun += o.punch_side >= 0; }
    CHECK(sqz >= 1 && pun == 0);
}

/* punto a (lateral hacia la derecha, arriba, delante) de la cabeza en marco de guiñada: delante = (sin yaw, cos yaw), derecha del usuario = (-cos yaw, sin yaw) */
static void rel_pos(const vrg_in_t *in, float lat, float up, float fwd, float c[3]) {
    float fx = sinf(in->yaw), fz = cosf(in->yaw);
    c[0] = in->head[0] + fx * fwd - fz * lat; c[1] = in->head[1] + up; c[2] = in->head[2] + fz * fwd + fx * lat;
}
/* zona de menú de la mano `s` (0 izquierda, 1 derecha): 0,16 m a su lado, 0,26 m bajo la cabeza, 0,02 m detrás del plano de la cabeza */
static void chest_center(const vrg_in_t *in, int s, float c[3]) { rel_pos(in, s == 0 ? -0.16f : 0.16f, -0.26f, -0.02f, c); }
/* aprieta el gatillo de la mano `s` y lo mantiene `after` fotogramas más (0,02 s cada uno); devuelve 1 si la ventana se abrió en algún fotograma */
static int hold_trig(vrg_t *g, vrg_in_t *in, int s, int after, vrg_out_t *o) {
    int opened = 0; in->trig[s] = 1.0f;
    for (int i = 0; i <= after; i++) { upd(g, in, o); opened |= o->menu_open[s] && (o->press & (VRG_L2 | VRG_R2)); }
    return opened;
}
/* arrojadiza VR en la mano del arma (HELD/READY): su gatillo la prepara/lanza y NO abre la ventana de armas del pecho; la otra mano sigue abriendo la de objetos, y sin arrojadiza se abre como siempre */
static void t_chest_throw(void) {
    vrg_out_t o; vrg_t g; vrg_init(&g); vrg_in_t in = base();
    float c0[3], c1[3]; chest_center(&in, 0, c0); chest_center(&in, 1, c1);
    memcpy(in.hand[1], c1, sizeof c1); settle(&g, &in, 12);
    in.throw_hold = 1;
    CHECK(!hold_trig(&g, &in, 1, 30, &o) && o.menu_open[1] == 0 && !(o.press & VRG_R2));          /* 0,6 s con el gatillo en el pecho */
    in.trig[1] = 0; settle(&g, &in, 3);
    memcpy(in.hand[0], c0, sizeof c0); settle(&g, &in, 12);
    CHECK(hold_trig(&g, &in, 0, 15, &o) && (o.press & VRG_L2) && !(o.press & VRG_R2));            /* la mano libre abre objetos */
    in.trig[0] = 0; settle(&g, &in, 3);
    in.throw_hold = 0; settle(&g, &in, 12);
    CHECK(hold_trig(&g, &in, 1, 15, &o) && (o.press & VRG_R2) && o.pecho[1].opened);              /* sin arrojadiza: abre */
    in.trig[1] = 0; settle(&g, &in, 3);
    in.trig[1] = 1; int op = 0;                                                                   /* se prepara con el intento ya en curso: se cancela */
    for (int i = 0; i < 10; i++) upd(&g, &in, &o);
    in.throw_hold = 1;
    for (int i = 0; i < 30; i++) { upd(&g, &in, &o); op |= o.menu_open[1] || (o.press & VRG_R2); }
    CHECK(!op);
}
static void t_chest(void) {
    for (int mode = 0; mode < 4; mode++) {            /* 0 de pie, 1 sentado, 2 girado 90 grados, 3 girado y sentado */
        vrg_out_t o; vrg_t g; vrg_init(&g); vrg_in_t in = base();
        in.head[1] = (mode & 1) ? 1.05f : 1.65f; in.head[0] = 0.3f; in.head[2] = -0.4f;
        in.yaw = (mode & 2) ? 1.5707963f : 0.0f;
        in.head_fwd[0] = sinf(in.yaw); in.head_fwd[1] = cosf(in.yaw);
        float c0[3], c1[3]; chest_center(&in, 0, c0); chest_center(&in, 1, c1);
        /* manos fuera de la zona: pulsar = puño solamente */
        settle(&g, &in, 12);
        CHECK(hold_trig(&g, &in, 0, 0, &o) == 0 && o.pecho[0].valid && !o.pecho[0].opened && !strcmp(o.pecho[0].reason, "lejos") && o.pecho[0].dist > 0.15f);   /* [pecho] del rechazo */
        CHECK(!hold_trig(&g, &in, 0, 25, &o) && o.menu_open[0] == 0 && !(o.press & VRG_L2)); in.trig[0] = 0; settle(&g, &in, 3);
        /* mano izquierda en el pecho: 0,2 s no abre, 0,28 s tampoco; 0,30 s abre objetos (L2 del motor) y se mantiene */
        memcpy(in.hand[0], c0, sizeof c0); settle(&g, &in, 12);
        CHECK(!hold_trig(&g, &in, 0, 10, &o)); in.trig[0] = 0; upd(&g, &in, &o);
        CHECK(o.pecho[0].valid && !o.pecho[0].opened && !strcmp(o.pecho[0].reason, "soltado") && fabsf(o.pecho[0].hold - 0.2f) < 0.005f); settle(&g, &in, 3);
        CHECK(!hold_trig(&g, &in, 0, 14, &o)); in.trig[0] = 0; settle(&g, &in, 3);
        CHECK(hold_trig(&g, &in, 0, 15, &o));
        CHECK(o.menu_open[0] == 1 && (o.press & VRG_L2) && !(o.press & VRG_R2) && o.grab_start == 0);
        CHECK(o.pecho[0].valid && o.pecho[0].opened && !strcmp(o.pecho[0].reason, "ok") && o.pecho[0].hold > 0.29f && o.pecho[0].dist < 0.01f);
        int kept = 0; for (int i = 0; i < 25; i++) { in.hand[0][2] += 0.01f; upd(&g, &in, &o); kept += (o.press & VRG_L2) != 0; }   /* la mano puede moverse: sigue abierta */
        CHECK(kept == 25);
        in.trig[0] = 0; upd(&g, &in, &o); CHECK(o.menu_open[0] == 0 && !(o.press & VRG_L2));
        /* mano derecha en SU zona (el lado derecho) -> R2 (armas) */
        settle(&g, &in, 5); memcpy(in.hand[1], c1, sizeof c1); settle(&g, &in, 12);
        CHECK(hold_trig(&g, &in, 1, 15, &o)); CHECK(o.menu_open[1] == 1 && (o.press & VRG_R2) && !(o.press & VRG_L2));
        in.trig[1] = 0; settle(&g, &in, 3);
        /* salir de la zona antes de 0,30 s reinicia el tiempo: no abre ni al volver */
        settle(&g, &in, 12); memcpy(in.hand[0], c0, sizeof c0); settle(&g, &in, 12);
        { int op = 0; in.trig[0] = 1; for (int i = 0; i < 40; i++) { if (i == 8) in.hand[0][1] -= 0.5f; if (i == 9) in.hand[0][1] += 0.5f; upd(&g, &in, &o); op |= o.menu_open[0]; } CHECK(!op); }
        in.trig[0] = 0; settle(&g, &in, 3);
        /* mano colgando al costado (0,20 m lateral, 0,60 m bajo la cabeza): NO abre ni manteniendo el puño 1 s */
        for (int s = 0; s < 2; s++) {
            float lx = cosf(in.yaw), lz = -sinf(in.yaw), sd = s == 0 ? 1.0f : -1.0f;
            in.trig[s] = 0; settle(&g, &in, 12);
            in.hand[s][0] = in.head[0] + lx * 0.20f * sd; in.hand[s][1] = in.head[1] - 0.60f; in.hand[s][2] = in.head[2] + lz * 0.20f * sd;
            settle(&g, &in, 12);
            CHECK(!hold_trig(&g, &in, s, 50, &o)); in.trig[s] = 0; settle(&g, &in, 3);
        }
        /* cada mano solo en SU zona: la izquierda en la zona derecha (y al revés) no abre; ni la zona de una mano sirve a la otra aunque esté a 0,32 m */
        memcpy(in.hand[0], c1, sizeof c1); memcpy(in.hand[1], c0, sizeof c0); settle(&g, &in, 12);
        CHECK(hold_trig(&g, &in, 0, 0, &o) == 0 && !strcmp(o.pecho[0].reason, "lejos") && o.pecho[0].dist > 0.30f);
        CHECK(!hold_trig(&g, &in, 0, 25, &o)); in.trig[0] = 0; settle(&g, &in, 3);
        CHECK(!hold_trig(&g, &in, 1, 25, &o) && !(o.press & VRG_R2)); in.trig[1] = 0; settle(&g, &in, 3);
        /* posiciones reconstruidas del registro (lateral, arriba, delante): izquierda (-0,164, -0,26, +0,003), derecha (+0,149, -0,26, -0,041): abren cada una en su zona */
        rel_pos(&in, -0.164f, -0.26f, 0.003f, in.hand[0]); rel_pos(&in, 0.149f, -0.26f, -0.041f, in.hand[1]); settle(&g, &in, 12);
        CHECK(hold_trig(&g, &in, 0, 15, &o) && (o.press & VRG_L2) && o.pecho[0].opened && o.pecho[0].dist < 0.05f); in.trig[0] = 0; settle(&g, &in, 3);
        CHECK(hold_trig(&g, &in, 1, 15, &o) && (o.press & VRG_R2) && o.pecho[1].opened && o.pecho[1].dist < 0.05f); in.trig[1] = 0; settle(&g, &in, 3);
        /* el punto fijo anterior (0,30 m bajo la cabeza, 0,18 m delante, centrado) ya no abre */
        rel_pos(&in, 0.0f, -0.30f, 0.18f, in.hand[0]); settle(&g, &in, 12); CHECK(!hold_trig(&g, &in, 0, 25, &o)); in.trig[0] = 0; settle(&g, &in, 3);
        /* 0,5 m fuera de la zona: no */
        memcpy(in.hand[0], c0, sizeof c0); in.hand[0][1] -= 0.5f; settle(&g, &in, 12); CHECK(!hold_trig(&g, &in, 0, 25, &o));
        in.trig[0] = 0; settle(&g, &in, 3);
        /* regla dura: puño ya cerrado y la mano ENTRA en la zona: no se abre la ventana */
        memcpy(in.hand[0], base().hand[0], sizeof in.hand[0]); in.hand[0][0] += in.head[0]; in.hand[0][2] += in.head[2];
        in.trig[0] = 1; settle(&g, &in, 12);
        memcpy(in.hand[0], c0, sizeof c0); upd(&g, &in, &o); CHECK(o.menu_open[0] == 0 && !(o.press & VRG_L2));
        settle(&g, &in, 25); upd(&g, &in, &o); CHECK(o.menu_open[0] == 0 && !(o.press & VRG_L2));
        in.trig[0] = 0; settle(&g, &in, 3);
        /* aviso háptico corto al entrar en la zona con la mano libre */
        vrg_init(&g); in.hand[1][0] = 5; in.hand[1][1] = 1.0f; settle(&g, &in, 12);
        memcpy(in.hand[1], c1, sizeof c1); upd(&g, &in, &o); CHECK(o.haptic[1] > 0.1f && o.haptic[1] < 0.3f);
    }
    /* coordenadas literales del mundo mirando a +z (yaw 0): la derecha del usuario es -x. Mano izquierda en x = cabeza + 0,16 abre; en x = cabeza - 0,16 (lado derecho) no; la derecha al revés */
    {
        vrg_out_t o; vrg_t g; vrg_init(&g); vrg_in_t in = base(); in.head[0] = 0.3f; in.head[1] = 1.65f; in.head[2] = -0.4f; in.yaw = 0.0f; in.head_fwd[0] = 0.0f; in.head_fwd[1] = 1.0f;
        in.hand[0][0] = 0.46f; in.hand[0][1] = 1.39f; in.hand[0][2] = -0.42f; in.hand[1][0] = 0.46f; in.hand[1][1] = 1.39f; in.hand[1][2] = -0.42f; settle(&g, &in, 12);
        CHECK(hold_trig(&g, &in, 0, 15, &o) && (o.press & VRG_L2)); in.trig[0] = 0; settle(&g, &in, 3);
        CHECK(!hold_trig(&g, &in, 1, 25, &o) && !(o.press & VRG_R2)); in.trig[1] = 0; settle(&g, &in, 3);
        in.hand[0][0] = 0.14f; in.hand[1][0] = 0.14f; settle(&g, &in, 12);
        CHECK(!hold_trig(&g, &in, 0, 25, &o) && !(o.press & VRG_L2)); in.trig[0] = 0; settle(&g, &in, 3);
        CHECK(hold_trig(&g, &in, 1, 15, &o) && (o.press & VRG_R2));
    }
    /* cuello de un enemigo dentro de la zona del pecho: el puño (R1/L1) agarra y NO abre la ventana (la abre solo el gatillo) */
    vrg_out_t o; vrg_t g; vrg_init(&g); vrg_in_t in = base(); in.body[2] = 0.0f;
    float c1[3]; chest_center(&in, 1, c1);
    in.n_en = 1; in.en[0].p[0] = c1[0]; in.en[0].p[1] = c1[1] - 0.45f; in.en[0].p[2] = c1[2] + 0.1f; in.en[0].yaw = 0;
    memcpy(in.hand[1], c1, sizeof c1); settle(&g, &in, 12);
    in.fist_btn[1] = 1; pgrab(&g, &in, &o);
    CHECK(o.grab_start == 1 && o.menu_open[1] == 0 && !(o.press & VRG_R2));
    /* con el enemigo caído (sin agarre posible) el gatillo abre la ventana */
    vrg_init(&g); in.en[0].down = 1; in.fist_btn[1] = 0; settle(&g, &in, 12); CHECK(hold_trig(&g, &in, 1, 15, &o));
    CHECK(o.grab_start == 0 && o.menu_open[1] == 1);
    /* la ventana abierta bloquea el golpe de esa mano */
    int pc = 0; for (int i = 1; i <= 6; i++) { in.hand[1][2] += 0.1f; upd(&g, &in, &o); pc += o.punch_side >= 0; } CHECK(pc == 0);
}

/* superficie sólida a 0,5 m de los nudillos: el puño se acerca a `v` m/s hasta tocarla (0,03 m) y vuelve. `still` = 1: la mano no se mueve respecto a la cabeza (es el personaje el que anda hacia la pared) */
static int touch_still;
static int touches(vrg_t *g, vrg_in_t in, int n, float v, float gap_s, int side, const char **last_why, int *haptics) {
    vrg_out_t o; int k = 0; float d = 0.5f; const float z0 = in.hand[side][2];
    in.wall_valid[0] = in.wall_valid[1] = 1; in.wall_dist[0] = in.wall_dist[1] = 0.5f; settle(g, &in, 12);
#define TSTEP() do { in.wall_dist[side] = d; if (!touch_still) in.hand[side][2] = z0 + (0.5f - d); upd(g, &in, &o); k += o.knock_side == side; } while (0)
    for (int t = 0; t < n; t++) {
        while (d > 0.03f) { d -= v * 0.02f; if (d < 0.03f) d = 0.03f; TSTEP(); if (o.knock_why && last_why) *last_why = o.knock_why; if (o.haptic[side] > 0.3f && haptics) (*haptics)++; }
        for (int i = 0; i < (int)(gap_s / 0.02f); i++) { d += 0.02f * 1.5f; if (d > 0.5f) d = 0.5f; TSTEP(); }
        d = 0.5f; TSTEP();
    }
#undef TSTEP
    return k;
}
static void t_knock(void) {
    vrg_t g; vrg_init(&g); vrg_in_t in = base(); in.fist_btn[1] = 1; int hap = 0; const char *why = NULL;      /* SIN estar pegado a la pared (on_wall = 0): el toque no depende de ese estado del juego */
    CHECK(touches(&g, in, 3, 1.0f, 0.3f, 1, &why, &hap) == 3);            /* tres golpes de puño seguidos en la pared = tres toques */
    CHECK(hap == 3);                                                       /* un pulso háptico por toque */
    vrg_init(&g); CHECK(touches(&g, in, 1, 1.0f, 0.3f, 1, &why, NULL) == 1);   /* un gesto = un toque */
    vrg_init(&g); CHECK(touches(&g, in, 3, 0.2f, 0.3f, 1, &why, NULL) == 0);   /* toque lento */
    CHECK(why && !strcmp(why, "lento"));
    vrg_init(&g); vrg_in_t ow = in; ow.on_wall = 1; CHECK(touches(&g, ow, 2, 1.0f, 0.3f, 1, NULL, NULL) == 2);   /* pegado a la pared también */
    vrg_init(&g); vrg_in_t lh = in; lh.fist_btn[1] = 0; lh.fist_btn[0] = 1; CHECK(touches(&g, lh, 2, 1.0f, 0.3f, 0, NULL, NULL) == 2);   /* la otra mano */
    vrg_init(&g); CHECK(touches(&g, in, 2, 5.0f, 0.06f, 1, &why, NULL) == 1);                      /* demasiado seguidos: enfriamiento (0,25 s) */
    vrg_init(&g); vrg_in_t op = in; op.fist_btn[1] = 0; CHECK(touches(&g, op, 2, 1.0f, 0.3f, 1, &why, NULL) == 0 && !strcmp(why, "sin_puno"));   /* mano abierta (balancear el brazo, rozar la pared): no */
    touch_still = 1; vrg_init(&g); CHECK(touches(&g, in, 2, 1.0f, 0.3f, 1, &why, NULL) == 0 && !strcmp(why, "mano_quieta")); touch_still = 0;   /* andar hacia la pared con el puño quieto */
    vrg_init(&g); vrg_in_t en = in; en.n_en = 1; en.en[0].p[0] = 0.2f; en.en[0].p[1] = 1.2f; en.en[0].p[2] = 0.6f;     /* enemigo junto a la mano: es golpe, no pared */
    CHECK(touches(&g, en, 2, 1.0f, 0.3f, 1, &why, NULL) == 0 && !strcmp(why, "enemigo"));
    vrg_init(&g); vrg_in_t aw = in; aw.weapon = 0; CHECK(touches(&g, aw, 2, 1.0f, 0.3f, 1, NULL, NULL) == 0);
    vrg_init(&g); vrg_in_t nv = in; vrg_out_t o; nv.wall_valid[0] = nv.wall_valid[1] = 0; settle(&g, &nv, 12); nv.wall_dist[1] = 0.02f; upd(&g, &nv, &o); CHECK(o.knock_side < 0);   /* sin consulta de pared */
    /* solo superficies aproximadamente verticales: el suelo o la tapa de una caja (normal vertical) no cuentan */
    vrg_init(&g); vrg_in_t fl = in; fl.wall_ny[1] = 1.0f; CHECK(touches(&g, fl, 2, 1.0f, 0.3f, 1, &why, NULL) == 0 && !strcmp(why, "no_vertical"));
    vrg_init(&g); fl.wall_ny[1] = 0.7f; CHECK(touches(&g, fl, 2, 1.0f, 0.3f, 1, &why, NULL) == 0 && !strcmp(why, "no_vertical"));
    vrg_init(&g); fl.wall_ny[1] = 0.3f; CHECK(touches(&g, fl, 2, 1.0f, 0.3f, 1, NULL, NULL) == 2);                     /* pared algo inclinada */
    /* solo de pie o pegado a la pared: agachado, tumbado o gateando las manos cruzan superficies sin querer */
    for (int st = 1; st <= 2; st++) { vrg_init(&g); vrg_in_t ps = in; ps.stance = st; CHECK(touches(&g, ps, 2, 1.0f, 0.3f, 1, &why, NULL) == 0 && !strcmp(why, "postura")); }
    for (int st = 1; st <= 2; st++) { vrg_init(&g); vrg_in_t pw = in; pw.stance = st; pw.on_wall = 1; hap = 0; CHECK(touches(&g, pw, 2, 1.0f, 0.3f, 1, &why, &hap) == 0 && !strcmp(why, "postura") && hap == 0); }   /* pegado a la pared agachado o tumbado: tampoco (el motor lo rechaza; sin háptica sin golpe) */
    vrg_init(&g); vrg_in_t bx = in; bx.in_box = 1; CHECK(touches(&g, bx, 2, 1.0f, 0.3f, 1, &why, NULL) == 0 && !strcmp(why, "manos_ocupadas"));   /* en la caja */
    vrg_init(&g); vrg_in_t th = in; th.throw_hold = 1; CHECK(touches(&g, th, 2, 1.0f, 0.3f, 1, &why, NULL) == 0 && !strcmp(why, "manos_ocupadas"));   /* granada en la mano */
    /* balanceo del brazo (puño cerrado, 1,5 m/s, vaivén de 0,3 m) sin superficie al alcance: ningún toque */
    { vrg_init(&g); vrg_in_t sw = in; int k = 0; sw.wall_valid[0] = sw.wall_valid[1] = 0; settle(&g, &sw, 12);
      for (int i = 0; i < 200; i++) { sw.hand[1][2] = 0.1f + 0.3f * (float)fabs(((i % 20) - 10) / 10.0); upd(&g, &sw, &o); k += o.knock_side >= 0; }
      CHECK(k == 0); }
    /* balanceo junto a una pared sin llegar a tocarla (se queda a 0,15 m): ningún toque */
    { vrg_init(&g); vrg_in_t sw = in; int k = 0; sw.wall_valid[1] = 1; sw.wall_dist[1] = 0.45f; settle(&g, &sw, 12);
      for (int i = 0; i < 200; i++) { float u = (float)fabs(((i % 20) - 10) / 10.0); sw.hand[1][2] = 0.1f + 0.3f * u; sw.wall_dist[1] = 0.45f - 0.3f * u; upd(&g, &sw, &o); k += o.knock_side >= 0; }
      CHECK(k == 0); }
    /* acercarse rápido y parar a 0,2 m: lejos de la pared */
    vrg_init(&g); in.wall_valid[1] = 1; settle(&g, &in, 12); float d = 0.6f; why = NULL;
    for (int i = 0; i < 8; i++) { d -= 0.02f; in.wall_dist[1] = d; upd(&g, &in, &o); }
    for (int i = 0; i < 6; i++) { in.wall_dist[1] = d; upd(&g, &in, &o); if (o.knock_why) why = o.knock_why; }
    (void)why;
}
/* contacto del puño con las cápsulas por pieza (lo aporta el anfitrión con vr_hit): sustituye al cilindro y da el háptico por zona */
static void t_caps(void) {
    for (int zone = 0; zone <= 3; zone += (zone ? 2 : 1)) {       /* 0 sin contacto, 1 cabeza, 3 extremidad */
        vrg_t g; vrg_init(&g); vrg_in_t in = base(); vrg_out_t o; int contact = 0, punch = 0; float hap = 0;
        in.fist_btn[1] = 1; in.cap_valid = 1; in.n_en = 1; in.en[0].vital = 100;
        in.en[0].p[0] = 0.2f; in.en[0].p[1] = 1.0f; in.en[0].p[2] = zone ? 2.5f : 0.5f;     /* sin contacto: el cilindro antiguo SÍ lo tocaría (enemigo en el camino del puño) */
        settle(&g, &in, 12);
        for (int i = 1; i <= 16; i++) {
            if (i <= 6) in.hand[1][2] = 0.1f + 0.1f * (float)i;      /* 5 m/s hacia delante */
            in.cap_zone[1] = zone;
            upd(&g, &in, &o);
            if (o.gest.valid && o.gest.contact) contact = 1;
            if (o.punch_side == 1) punch++;
            if (o.haptic[1] > hap) hap = o.haptic[1];
        }
        if (!zone) { CHECK(!contact); }
        else { CHECK(contact && punch >= 1); CHECK(fabsf(hap - (zone == 1 ? 1.0f : 0.75f)) < 1e-4f); }
    }
}
static void t_peek(void) {
    vrg_t g; vrg_init(&g); vrg_in_t in = base(); vrg_out_t o; in.on_wall = 1;
    settle(&g, &in, 12); in.head_u[0] = 0; in.head[0] = 0.3f; int tri = 0;
    for (int i = 0; i < 10; i++) { upd(&g, &in, &o); tri += (o.press & VRG_TRIANGLE) != 0; }
    CHECK(tri >= 1);
}

static void t_robust(void) {
    vrg_t g; vrg_init(&g); vrg_in_t in = with_enemy(); vrg_out_t o; in.fist_btn[1] = 1;
    srand(7);
    for (int i = 0; i < 4000; i++) {
        for (int s = 0; s < 2; s++) for (int k = 0; k < 3; k++) in.hand[s][k] = ((rand() % 4000) - 2000) / 1000.0f;
        if (rand() % 50 == 0) in.hand[1][rand() % 3] = NAN;
        if (rand() % 50 == 0) in.head[rand() % 3] = INFINITY;
        else in.head[0] = in.head[2] = 0;
        in.fist_btn[0] = rand() & 1; in.fist_btn[1] = rand() & 1; in.holding = (rand() % 7) == 0; in.yaw = (rand() % 6283) / 1000.0f;
        in.en[0].p[0] = (rand() % 3000) / 1000.0f; in.en[0].yaw = (rand() % 6283) / 1000.0f;
        upd(&g, &in, &o);
        CHECK(o.grab_start == 0 || o.grab_side >= 0);
        CHECK(!(o.press & VRG_SQUARE));
        if (o.grab_start) CHECK(!(o.punch_side >= 0));
    }
}

/* los menús los abre el GATILLO de la mano en su zona (L2 izquierda, R2 derecha); el agarre (R1/L1) no; el gatillo en la zona no llega al juego */
static void t_trigmenu(void) {
    vrg_out_t o; vrg_t g; vrg_init(&g); vrg_in_t in = base(); float c[2][3];
    chest_center(&in, 0, c[0]); chest_center(&in, 1, c[1]);
    for (int s = 0; s < 2; s++) {
        uint16_t btn = s == 0 ? VRG_L2 : VRG_R2;
        vrg_init(&g); in = base(); memcpy(in.hand[s], c[s], sizeof c[s]); settle(&g, &in, 12);
        /* gatillo en la zona: abre a los 0,30 s con L2 (izquierda) / R2 (derecha) y el gatillo queda retirado del mando */
        CHECK(!hold_trig(&g, &in, s, 10, &o) && (o.mask & btn) && !(o.press & btn));              /* 0,22 s: aún no, pero ya enmascarado */
        in.trig[s] = 0; settle(&g, &in, 3);
        CHECK(hold_trig(&g, &in, s, 15, &o) && o.menu_open[s] == 1 && (o.press & btn) && (o.mask & btn) && o.pecho[s].opened);
        in.trig[s] = 0; upd(&g, &in, &o); CHECK(o.menu_open[s] == 0 && !(o.press & btn)); settle(&g, &in, 3);
        /* el agarre (R1/L1) en la zona NO abre */
        in.fist_btn[s] = 1; int op = 0, mk = 0;
        for (int i = 0; i < 40; i++) { upd(&g, &in, &o); op |= o.menu_open[s] || (o.press & (VRG_L2 | VRG_R2)) || o.pecho[s].valid; }
        CHECK(!op); in.fist_btn[s] = 0; settle(&g, &in, 3);
        /* gatillo bajo el umbral de pulsación (0,5 < 0,6): no abre; con histéresis ya pulsado sigue a 0,4 y suelta a 0,2 */
        in.trig[s] = 0.5f; op = 0; for (int i = 0; i < 40; i++) { upd(&g, &in, &o); op |= o.menu_open[s]; } CHECK(!op); in.trig[s] = 0; settle(&g, &in, 3);
        in.trig[s] = 1.0f; settle(&g, &in, 3); in.trig[s] = 0.4f; settle(&g, &in, 20); upd(&g, &in, &o); CHECK(o.menu_open[s] == 1 && (o.press & btn));
        in.trig[s] = 0.2f; upd(&g, &in, &o); CHECK(o.menu_open[s] == 0 && !(o.press & btn)); in.trig[s] = 0; settle(&g, &in, 3);
        /* gatillo FUERA de la zona: no abre y no se retira (sigue siendo disparo) */
        vrg_init(&g); in = base(); memcpy(in.hand[s], c[s], sizeof c[s]); in.hand[s][1] -= 0.5f; settle(&g, &in, 12);
        op = 0; mk = 0; in.trig[s] = 1.0f;
        for (int i = 0; i < 40; i++) { upd(&g, &in, &o); op |= o.menu_open[s] || (o.press & btn); mk |= (o.mask & (VRG_L2 | VRG_R2)) != 0; }
        CHECK(!op && !mk);
        /* la mano entra con el gatillo ya apretado: no abre, pero en la zona sigue retirado del mando; al salir vuelve a llegar */
        memcpy(in.hand[s], c[s], sizeof c[s]); upd(&g, &in, &o); CHECK((o.mask & btn) && !o.menu_open[s]);
        settle(&g, &in, 25); upd(&g, &in, &o); CHECK((o.mask & btn) && !o.menu_open[s] && !(o.press & btn));
        in.hand[s][1] -= 0.5f; upd(&g, &in, &o); CHECK(!(o.mask & btn));
    }
    /* el gatillo de una mano no se retira por estar la OTRA en su zona */
    vrg_init(&g); in = base(); memcpy(in.hand[0], c[0], sizeof c[0]); settle(&g, &in, 12); in.trig[1] = 1.0f; upd(&g, &in, &o); CHECK(!(o.mask & VRG_R2));
}

/* zona de menú en el marco FÍSICO: la cabeza/mano del mundo (escaladas) y el rumbo del personaje no intervienen; manda hrel + head_fwd */
static void upd_phys(vrg_t *g, vrg_in_t *in, vrg_out_t *o) {
    memcpy(in->head_u, in->head, sizeof in->head_u);
    for (int s = 0; s < 2; s++) memcpy(in->hand_u[s], in->hand[s], sizeof in->hand_u[s]);
    vrg_update(g, in, o);
}
/* mano `s` a (lat derecha, arriba, delante) de la cabeza en metros reales; el mundo va escalado por `sc` y con otro rumbo (in->yaw) a propósito */
static void put_hf(vrg_in_t *in, int s, float lat, float up, float fwd, float sc) {
    float fx = in->head_fwd[0], fz = in->head_fwd[1];
    in->hrel[s][0] = -lat * fz + fwd * fx; in->hrel[s][1] = up; in->hrel[s][2] = lat * fx + fwd * fz;
    for (int i = 0; i < 3; i++) in->hand[s][i] = in->head[i] + sc * in->hrel[s][i];
}
static vrg_in_t phys_base(void) {
    vrg_in_t in = base();
    in.head[0] = 0.255f; in.head[1] = 0.85f; in.head[2] = -0.34f;     /* mundo escalado 0,85 */
    in.yaw = 0.0f; in.head_fwd[0] = 1.0f; in.head_fwd[1] = 0.0f;       /* cabeza girada 90 grados respecto al personaje */
    return in;
}
/* aprieta el gatillo de `s` durante `n` fotogramas; devuelve cuántas veces se abrió la ventana (flancos de pecho.opened) */
static int press_phys(vrg_t *g, vrg_in_t *in, int s, int n, vrg_out_t *o) {
    int cnt = 0; in->trig[s] = 1.0f;
    for (int i = 0; i < n; i++) { upd_phys(g, in, o); cnt += o->pecho[s].valid && o->pecho[s].opened; }
    return cnt;
}
static void t_chest_phys(void) {
    vrg_out_t o;
    for (int s = 0; s < 2; s++) {
        float sd = s == 0 ? -1.0f : 1.0f;
        /* (a) mano en la zona física con el mundo escalado y el rumbo del personaje distinto del de la cabeza: abre */
        vrg_t g; vrg_init(&g); vrg_in_t in = phys_base();
        put_hf(&in, s, sd * 0.16f, -0.26f, -0.02f, 0.85f); for (int i = 0; i < 12; i++) upd_phys(&g, &in, &o);
        CHECK(press_phys(&g, &in, s, 16, &o) == 1 && o.menu_open[s] == 1);
        CHECK(o.pecho[s].rearm == 1 && fabsf(o.pecho[s].hf[0] - sd * 0.16f) < 0.005f && fabsf(o.pecho[s].hf[1] + 0.26f) < 0.005f && fabsf(o.pecho[s].hf[2] + 0.02f) < 0.005f);   /* hf para el registro [pecho] */
        /* (b) gatillo mantenido 3 s en la zona: exactamente una apertura */
        for (int i = 0; i < 135; i++) { int c = press_phys(&g, &in, s, 1, &o); CHECK(c == 0); }
        /* el seguimiento se pierde un fotograma con el gatillo aún apretado: al volver NO se reabre */
        in.tracked[s] = 0; upd_phys(&g, &in, &o); in.tracked[s] = 1;
        CHECK(press_phys(&g, &in, s, 40, &o) == 0 && o.menu_open[s] == 0);
        /* soltar el gatillo rearma: otra pulsación abre otra vez */
        in.trig[s] = 0; for (int i = 0; i < 3; i++) upd_phys(&g, &in, &o);
        CHECK(press_phys(&g, &in, s, 16, &o) == 1);
        /* (c) 0,35 m fuera de la zona: no abre */
        vrg_init(&g); in = phys_base();
        put_hf(&in, s, sd * (0.16f + 0.35f), -0.26f, -0.02f, 0.85f); for (int i = 0; i < 12; i++) upd_phys(&g, &in, &o);
        press_phys(&g, &in, s, 1, &o); CHECK(o.pecho[s].valid && o.pecho[s].reason && !strcmp(o.pecho[s].reason, "lejos"));
        CHECK(press_phys(&g, &in, s, 40, &o) == 0 && o.menu_open[s] == 0);
        /* histéresis: entra a 0,15, sigue dentro hasta 0,20; entrando de fuera a 0,18 no vale */
        vrg_init(&g); in = phys_base(); in.trig[s] = 0;
        put_hf(&in, s, sd * 0.16f, -0.26f, -0.02f, 0.85f); for (int i = 0; i < 12; i++) upd_phys(&g, &in, &o);
        put_hf(&in, s, sd * 0.16f, -0.26f + 0.18f, -0.02f, 0.85f); upd_phys(&g, &in, &o);
        CHECK(press_phys(&g, &in, s, 16, &o) == 1);
        vrg_init(&g); in = phys_base();
        put_hf(&in, s, sd * 0.16f, -0.26f + 0.18f, -0.02f, 0.85f); for (int i = 0; i < 12; i++) upd_phys(&g, &in, &o);
        CHECK(press_phys(&g, &in, s, 16, &o) == 0);
        /* (d) cabeza mirando casi al suelo (rumbo de la cabeza no válido, fwd_ok = 0): la zona usa el rumbo del personaje (in.yaw) como reserva y abre */
        vrg_init(&g); in = phys_base(); in.yaw = 0.7f;
        in.head_fwd[0] = sinf(in.yaw); in.head_fwd[1] = cosf(in.yaw);
        put_hf(&in, s, sd * 0.16f, -0.26f, -0.02f, 0.85f);                       /* hrel construido con el rumbo del personaje */
        in.head_fwd[0] = 0.05f; in.head_fwd[1] = 0.05f;                          /* casi vertical: |fwd| < 0,5 */
        for (int i = 0; i < 12; i++) upd_phys(&g, &in, &o);
        CHECK(press_phys(&g, &in, s, 16, &o) == 1 && o.menu_open[s] == 1);
    }
}


/* ---- ventana del pecho sostenida por el GATILLO (vrg_step_pad): L2/R2 continuo mientras el gatillo siga apretado, cambie la capa o no ---- */
static uint16_t hold_step(vrg_t *g, const vrg_in_t *in, vrg_out_t *o, int ctl, uint16_t pad_in) {
    vrg_pad_t p; memset(&p, 0, sizeof p);
    p.ctl = ctl; p.menu_open = !ctl; p.pad = p.pad_in = pad_in;
    for (int s = 0; s < 2; s++) { p.tracked[s] = in->tracked[s]; p.trig[s] = in->trig[s]; }
    vrg_in_t iv = *in;
    if (!ctl) { iv.active = 0; memset(iv.tracked, 0, sizeof iv.tracked); memset(iv.trig, 0, sizeof iv.trig); }   /* fuera de la capa de juego vrbodyg no rellena manos ni gatillo */
    memcpy(iv.head_u, iv.head, sizeof iv.head_u);
    for (int s = 0; s < 2; s++) memcpy(iv.hand_u[s], iv.hand[s], sizeof iv.hand_u[s]);
    return vrg_step_pad(g, &iv, o, &p);
}
static int open_hold(vrg_t *g, vrg_in_t *in, int s, vrg_out_t *o) {   /* mano en la zona, gatillo apretado en la capa de juego hasta abrir; 1 si abrió */
    float sd = s == 0 ? -1.0f : 1.0f;
    put_hf(in, s, sd * 0.16f, -0.26f, -0.02f, 0.85f);
    for (int i = 0; i < 12; i++) hold_step(g, in, o, 1, 0);
    in->trig[s] = 1.0f;
    for (int i = 0; i < 30; i++) { hold_step(g, in, o, 1, 0); if (o->pecho[s].valid && o->pecho[s].opened) return 1; }
    return 0;
}
static void t_menu_hold(void) {
    vrg_out_t o;
    for (int s = 0; s < 2; s++) {
        uint16_t btn = s == 0 ? VRG_L2 : VRG_R2, other = s == 0 ? VRG_R2 : VRG_L2;
        /* (a) patrón del registro real: 3 VBlanks de juego, luego control/menú de 2 en 2; el mando entrante (capa de menú con antirrebote) no lleva el botón los primeros 9 VBlanks. 3 s de gatillo */
        vrg_t g; vrg_init(&g); vrg_in_t in = phys_base();
        CHECK(open_hold(&g, &in, s, &o));
        int edges = 0, prev = 1, all = 1, oth = 0;   /* prev = 1: el VBlank de la apertura ya llevaba el botón */
        for (int i = 0; i < 150; i++) {
            uint16_t r = hold_step(&g, &in, &o, i < 2 ? 1 : ((i / 2) & 1) ^ 1, i < 9 ? 0 : btn);
            int on = (r & btn) != 0; if (on && !prev) edges++; if (!on) all = 0; prev = on; oth |= (r & other) != 0;
        }
        CHECK(all);                                   /* el bit está a 1 en TODOS los VBlanks */
        CHECK(edges == 0);                            /* sin flancos nuevos tras abrir (el flanco de subida fue el de la apertura) */
        CHECK(!oth);
        /* (b) lo mismo con un patrón pseudoaleatorio de capa y de mando entrante */
        vrg_init(&g); in = phys_base(); CHECK(open_hold(&g, &in, s, &o));
        unsigned r = 777u; all = 1;
        for (int i = 0; i < 150; i++) {
            r = r * 1664525u + 1013904223u;
            uint16_t rr = hold_step(&g, &in, &o, (r >> 20) & 1, (r >> 21) & 1 ? btn : 0);
            if (!(rr & btn)) all = 0;
        }
        CHECK(all);
        /* (c) histéresis: a 0,45 (entre trig_off y trig_on) sigue en las dos capas; a 0,2 se suelta y no vuelve sin nueva apertura */
        in.trig[s] = 0.45f;
        for (int i = 0; i < 20; i++) CHECK(hold_step(&g, &in, &o, (i / 2) & 1, 0) & btn);
        in.trig[s] = 0.2f;
        CHECK(!(hold_step(&g, &in, &o, 0, 0) & btn) && !(g.menu_hold & (1 << s)));
        in.trig[s] = 1.0f;
        for (int i = 0; i < 20; i++) CHECK(!(hold_step(&g, &in, &o, (i / 2) & 1, 0) & btn));   /* gatillo apretado de nuevo sin pasar por el flanco de apertura: nada */
        /* (d) soltar el gatillo estando en la capa de menú también suelta; perder el seguimiento también */
        vrg_init(&g); in = phys_base(); CHECK(open_hold(&g, &in, s, &o));
        hold_step(&g, &in, &o, 0, 0); in.trig[s] = 0.0f;
        CHECK(!(hold_step(&g, &in, &o, 0, 0) & btn));
        /* perder el seguimiento: se toleran 3 VBlanks seguidos con el botón (en ambas capas); el cuarto suelta. Recuperarlo antes reinicia la cuenta */
        for (int lay = 0; lay < 2; lay++) {
            vrg_init(&g); in = phys_base(); CHECK(open_hold(&g, &in, s, &o));
            hold_step(&g, &in, &o, lay, 0); in.tracked[s] = 0;
            for (int i = 0; i < 3; i++) CHECK(hold_step(&g, &in, &o, lay, 0) & btn);
            CHECK(!(hold_step(&g, &in, &o, lay, 0) & btn) && !(g.menu_hold & (1 << s)));
            vrg_init(&g); in = phys_base(); CHECK(open_hold(&g, &in, s, &o));
            in.tracked[s] = 0;
            for (int i = 0; i < 2; i++) CHECK(hold_step(&g, &in, &o, lay, 0) & btn);
            in.tracked[s] = 1;
            CHECK(hold_step(&g, &in, &o, lay, 0) & btn);
            in.tracked[s] = 0;
            for (int i = 0; i < 3; i++) CHECK(hold_step(&g, &in, &o, lay, 0) & btn);
            CHECK(!(hold_step(&g, &in, &o, lay, 0) & btn));
        }
        vrg_init(&g); in = phys_base(); CHECK(open_hold(&g, &in, s, &o));
        hold_step(&g, &in, &o, 0, 0); in.tracked[s] = 1; in.trig[s] = 0.0f;
        CHECK(!(hold_step(&g, &in, &o, 0, 0) & btn));
        /* (e) ventana abierta y el juego sale del menú (menu_open = 0 en la capa de menú: pausa, códec): se olvida */
        vrg_init(&g); in = phys_base(); CHECK(open_hold(&g, &in, s, &o));
        { vrg_pad_t p; memset(&p, 0, sizeof p); vrg_in_t iv = in; iv.active = 0; memset(iv.tracked, 0, sizeof iv.tracked); p.tracked[s] = 1; p.trig[s] = 1.0f;
          CHECK(!(vrg_step_pad(&g, &iv, &o, &p) & btn) && g.menu_hold == 0); }
    }
}
/* Bucle cerrado con vr_input real (antirrebote de menú) y un juego que abre el selector con el botón y lo cierra al soltarlo con 2 VBlanks de retardo: 3 s de gatillo = UNA apertura y el selector queda abierto */
static void t_menu_loop(void) {
    vr_input_t vi; vr_input_init(&vi);
    vr_input_frame_t f; memset(&f, 0, sizeof f);
    f.dt = 0.02f; f.game_posture = -1; f.hand[0].quat[3] = f.hand[1].quat[3] = 1; f.head_quat[3] = 1;
    vrg_t g; vrg_init(&g); vrg_in_t in = phys_base(); vrg_out_t o; vr_pad_t vo;
    put_hf(&in, 0, -0.16f, -0.26f, -0.02f, 0.85f);
    int menu = 0, hist[3] = {0, 0, 0}, opens = 0, pulses = 0, l2prev = 0, seen_open = 0, l2_after = 0, l2_total = 0;
    for (int t = 0; t < 200; t++) {
        in.trig[0] = f.hand[0].trigger = t < 12 ? 0.0f : 1.0f;
        f.menu = menu != 0; vr_input_update(&vi, &f, &vo);
        uint16_t pin = vo.buttons, pad = pin;
        if (!menu) pad = (uint16_t)(pad & ~VRG_R2);
        uint16_t r = hold_step(&g, &in, &o, !menu, pin);
        (void)pad;
        int l2 = (r & VRG_L2) != 0;
        if (l2 && !l2prev) pulses++;
        l2prev = l2;
        if (menu) seen_open = 1;
        if (seen_open) { l2_total++; l2_after += l2; }
        hist[2] = hist[1]; hist[1] = hist[0]; hist[0] = l2;
        int nm = hist[2];                       /* el juego ve el botón con 2 VBlanks de retardo y sigue su nivel */
        if (nm && !menu) opens++;
        menu = nm;
    }
    printf("t_menu_loop: aperturas del selector=%d pulsos L2=%d selector_abierto_al_final=%d\n", opens, pulses, menu);
    CHECK(opens == 1);
    CHECK(pulses == 1);
    CHECK(menu == 1 && l2_after == l2_total);
}

/* ---- NUEVO: golpes seguidos con la MISMA mano sin parar (boxeo): cada vaivén hacia delante con el puño cerrado es un golpe ---- */
static void t_repeat(void) {
    vrg_t g; vrg_init(&g); vrg_in_t in = with_enemy(); vrg_out_t o; int cnt = 0;
    in.fist_btn[1] = 1; settle(&g, &in, 12);
    /* vaivén de 0,5 s de periodo (2 Hz), amplitud 0,25 m, sin pausa: 6 avances en 3 s; el ENEMIGO a 1,5 m */
    for (int i = 0; i < 150; i++) {
        float ph = (float)i * 0.02f * 2.0f * 3.14159265f / 0.5f;
        in.hand[1][2] = 0.35f + 0.25f * (-cosf(ph)); in.hand[1][1] = 1.2f - 0.12f * sinf(ph);   /* elipse: el puño nunca se detiene (vuelve por abajo) */
        upd(&g, &in, &o); cnt += o.punch_side == 1;
    }
    printf("t_repeat: golpes=%d de 6 avances\n", cnt);
    CHECK(cnt >= 5);
}

int main(void) {
    t_repeat();
    t_punch(); t_contact(); t_rules(); t_throw(); t_walk(); t_grab(); t_chest(); t_chest_throw(); t_chest_phys(); t_menu_hold(); t_menu_loop(); t_trigmenu(); t_knock(); t_caps(); t_peek(); t_robust();
    printf("%d comprobaciones, %d fallos\n", checks, fails);
    return fails != 0;
}
