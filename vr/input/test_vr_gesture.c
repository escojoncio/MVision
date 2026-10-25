/* Pruebas sintéticas de los gestos de cuerpo: el puño es el BOTÓN (R1/L1), el golpe/agarre/apretón son decisiones para el motor
 * (no botones sintéticos), la ventana de pecho y el respaldo del cuadrado. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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
static void chest_center(const vrg_in_t *in, float c[3]);
static int hold_fist(vrg_t *g, vrg_in_t *in, int s, int after, vrg_out_t *o);
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
    /* puño cerrado en la zona del pecho (ventana abierta) y enemigo delante: el golpe cancela la ventana y pega */
    {
        vrg_out_t o; vrg_init(&g); vrg_in_t m = in; m.fist_btn[1] = 0; float c1[3]; chest_center(&m, c1);
        memcpy(m.hand[1], c1, sizeof c1); settle(&g, &m, 12); hold_fist(&g, &m, 1, 15, &o);
        CHECK(o.menu_open[1] == 1);
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

static void chest_center(const vrg_in_t *in, float c[3]) {   /* un solo punto para las dos manos: 0,30 m bajo la cabeza, 0,18 m delante del torso */
    float fx = sinf(in->yaw), fz = cosf(in->yaw);
    c[0] = in->head[0] + fx * 0.18f; c[1] = in->head[1] - 0.30f; c[2] = in->head[2] + fz * 0.18f;
}
/* pulsa el puño de la mano `s` y lo mantiene `after` fotogramas más (0,02 s cada uno); devuelve 1 si la ventana se abrió en algún fotograma */
static int hold_fist(vrg_t *g, vrg_in_t *in, int s, int after, vrg_out_t *o) {
    int opened = 0; in->fist_btn[s] = 1;
    for (int i = 0; i <= after; i++) { upd(g, in, o); opened |= o->menu_open[s] && (o->press & (VRG_L2 | VRG_R2)); }
    return opened;
}
static void t_chest(void) {
    for (int mode = 0; mode < 4; mode++) {            /* 0 de pie, 1 sentado, 2 girado 90 grados, 3 girado y sentado */
        vrg_out_t o; vrg_t g; vrg_init(&g); vrg_in_t in = base();
        in.head[1] = (mode & 1) ? 1.05f : 1.65f; in.head[0] = 0.3f; in.head[2] = -0.4f;
        in.yaw = (mode & 2) ? 1.5707963f : 0.0f;
        in.head_fwd[0] = sinf(in.yaw); in.head_fwd[1] = cosf(in.yaw);
        float c0[3]; chest_center(&in, c0);
        /* manos fuera de la zona: pulsar = puño solamente */
        settle(&g, &in, 12);
        CHECK(hold_fist(&g, &in, 0, 0, &o) == 0 && o.pecho[0].valid && !o.pecho[0].opened && !strcmp(o.pecho[0].reason, "lejos") && o.pecho[0].dist > 0.15f);   /* [pecho] del rechazo */
        CHECK(!hold_fist(&g, &in, 0, 25, &o) && o.menu_open[0] == 0 && !(o.press & VRG_L2)); in.fist_btn[0] = 0; settle(&g, &in, 3);
        /* mano izquierda en el pecho: 0,2 s no abre, 0,28 s tampoco; 0,30 s abre objetos (L2 del motor) y se mantiene */
        memcpy(in.hand[0], c0, sizeof c0); settle(&g, &in, 12);
        CHECK(!hold_fist(&g, &in, 0, 10, &o)); in.fist_btn[0] = 0; upd(&g, &in, &o);
        CHECK(o.pecho[0].valid && !o.pecho[0].opened && !strcmp(o.pecho[0].reason, "soltado") && fabsf(o.pecho[0].hold - 0.2f) < 0.005f); settle(&g, &in, 3);
        CHECK(!hold_fist(&g, &in, 0, 14, &o)); in.fist_btn[0] = 0; settle(&g, &in, 3);
        CHECK(hold_fist(&g, &in, 0, 15, &o));
        CHECK(o.menu_open[0] == 1 && (o.press & VRG_L2) && !(o.press & VRG_R2) && o.grab_start == 0);
        CHECK(o.pecho[0].valid && o.pecho[0].opened && !strcmp(o.pecho[0].reason, "ok") && o.pecho[0].hold > 0.29f && o.pecho[0].dist < 0.01f && o.pecho[0].depth > 0.17f);
        int kept = 0; for (int i = 0; i < 25; i++) { in.hand[0][2] += 0.01f; upd(&g, &in, &o); kept += (o.press & VRG_L2) != 0; }   /* la mano puede moverse: sigue abierta */
        CHECK(kept == 25);
        in.fist_btn[0] = 0; upd(&g, &in, &o); CHECK(o.menu_open[0] == 0 && !(o.press & VRG_L2));
        /* mano derecha en el mismo punto -> R2 (armas) */
        settle(&g, &in, 5); memcpy(in.hand[1], c0, sizeof c0); settle(&g, &in, 12);
        CHECK(hold_fist(&g, &in, 1, 15, &o)); CHECK(o.menu_open[1] == 1 && (o.press & VRG_R2) && !(o.press & VRG_L2));
        in.fist_btn[1] = 0; settle(&g, &in, 3);
        /* salir de la zona antes de 0,30 s reinicia el tiempo: no abre ni al volver */
        settle(&g, &in, 12); memcpy(in.hand[0], c0, sizeof c0); settle(&g, &in, 12);
        { int op = 0; in.fist_btn[0] = 1; for (int i = 0; i < 40; i++) { if (i == 8) in.hand[0][1] -= 0.5f; if (i == 9) in.hand[0][1] += 0.5f; upd(&g, &in, &o); op |= o.menu_open[0]; } CHECK(!op); }
        in.fist_btn[0] = 0; settle(&g, &in, 3);
        /* mano colgando al costado (0,20 m lateral, 0,60 m bajo la cabeza): NO abre ni manteniendo el puño 1 s */
        for (int s = 0; s < 2; s++) {
            float lx = cosf(in.yaw), lz = -sinf(in.yaw), sd = s == 0 ? 1.0f : -1.0f;
            in.fist_btn[s] = 0; settle(&g, &in, 12);
            in.hand[s][0] = in.head[0] + lx * 0.20f * sd; in.hand[s][1] = in.head[1] - 0.60f; in.hand[s][2] = in.head[2] + lz * 0.20f * sd;
            settle(&g, &in, 12);
            CHECK(!hold_fist(&g, &in, s, 50, &o)); in.fist_btn[s] = 0; settle(&g, &in, 3);
        }
        /* detrás del plano del torso (a 0,14 m del punto pero con 0,04 m de profundidad): no */
        memcpy(in.hand[0], c0, sizeof c0); in.hand[0][0] -= sinf(in.yaw) * 0.14f; in.hand[0][2] -= cosf(in.yaw) * 0.14f; settle(&g, &in, 12);
        CHECK(hold_fist(&g, &in, 0, 0, &o) == 0 && !strcmp(o.pecho[0].reason, "detras") && o.pecho[0].depth < 0.05f);
        CHECK(!hold_fist(&g, &in, 0, 25, &o)); in.fist_btn[0] = 0; settle(&g, &in, 3);
        /* 0,5 m fuera de la zona: no */
        memcpy(in.hand[0], c0, sizeof c0); in.hand[0][1] -= 0.5f; settle(&g, &in, 12); CHECK(!hold_fist(&g, &in, 0, 25, &o));
        in.fist_btn[0] = 0; settle(&g, &in, 3);
        /* regla dura: puño ya cerrado y la mano ENTRA en la zona: no se abre la ventana */
        memcpy(in.hand[0], base().hand[0], sizeof in.hand[0]); in.hand[0][0] += in.head[0]; in.hand[0][2] += in.head[2];
        in.fist_btn[0] = 1; settle(&g, &in, 12);
        memcpy(in.hand[0], c0, sizeof c0); upd(&g, &in, &o); CHECK(o.menu_open[0] == 0 && !(o.press & VRG_L2));
        settle(&g, &in, 25); upd(&g, &in, &o); CHECK(o.menu_open[0] == 0 && !(o.press & VRG_L2));
        in.fist_btn[0] = 0; settle(&g, &in, 3);
        /* aviso háptico corto al entrar en la zona con la mano libre */
        vrg_init(&g); in.hand[1][0] = 5; in.hand[1][1] = 1.0f; settle(&g, &in, 12);
        memcpy(in.hand[1], c0, sizeof c0); upd(&g, &in, &o); CHECK(o.haptic[1] > 0.1f && o.haptic[1] < 0.3f);
    }
    /* prioridad: cuello de un enemigo dentro de la zona del pecho -> agarre, no ventana */
    vrg_out_t o; vrg_t g; vrg_init(&g); vrg_in_t in = base(); in.body[2] = 0.0f;
    float c1[3]; chest_center(&in, c1);
    in.n_en = 1; in.en[0].p[0] = c1[0]; in.en[0].p[1] = c1[1] - 0.45f; in.en[0].p[2] = c1[2] + 0.1f; in.en[0].yaw = 0;
    memcpy(in.hand[1], c1, sizeof c1); settle(&g, &in, 12);
    in.fist_btn[1] = 1; pgrab(&g, &in, &o);
    CHECK(o.grab_start == 1 && o.menu_open[1] == 0 && !(o.press & VRG_R2));
    /* si el agarre no es posible (puño... enemigo caído), cae a la ventana */
    vrg_init(&g); in.en[0].down = 1; in.fist_btn[1] = 0; settle(&g, &in, 12); CHECK(hold_fist(&g, &in, 1, 15, &o));
    CHECK(o.grab_start == 0 && o.menu_open[1] == 1);
    /* la ventana abierta bloquea el golpe de esa mano */
    int pc = 0; for (int i = 1; i <= 6; i++) { in.hand[1][2] += 0.1f; upd(&g, &in, &o); pc += o.punch_side >= 0; } CHECK(pc == 0);
}

/* pared a 0,5 m de la mano: la mano se acerca a `v` m/s hasta tocarla (0,03 m) y vuelve */
static int touches(vrg_t *g, vrg_in_t in, int n, float v, float gap_s, int side, const char **last_why, int *haptics) {
    vrg_out_t o; int k = 0; float d = 0.5f;
    in.wall_valid[0] = in.wall_valid[1] = 1; in.wall_dist[0] = in.wall_dist[1] = 0.5f; settle(g, &in, 12);
    for (int t = 0; t < n; t++) {
        while (d > 0.03f) { d -= v * 0.02f; if (d < 0.03f) d = 0.03f; in.wall_dist[side] = d; upd(g, &in, &o); k += o.knock_side == side; if (o.knock_why && last_why) *last_why = o.knock_why; if (o.haptic[side] > 0.3f && haptics) (*haptics)++; }
        for (int i = 0; i < (int)(gap_s / 0.02f); i++) { d += 0.02f * 1.5f; if (d > 0.5f) d = 0.5f; in.wall_dist[side] = d; upd(g, &in, &o); k += o.knock_side == side; }
        d = 0.5f; in.wall_dist[side] = d; upd(g, &in, &o);
    }
    return k;
}
static void t_knock(void) {
    vrg_t g; vrg_init(&g); vrg_in_t in = base(); in.on_wall = 1; int hap = 0; const char *why = NULL;
    CHECK(touches(&g, in, 3, 1.0f, 0.3f, 1, &why, &hap) == 3);            /* tres toques seguidos = tres golpes */
    CHECK(hap == 3);                                                       /* un pulso háptico por toque */
    vrg_init(&g); CHECK(touches(&g, in, 3, 0.2f, 0.3f, 1, &why, NULL) == 0);   /* toque lento */
    CHECK(why && !strcmp(why, "lento"));
    vrg_init(&g); in.fist_btn[1] = 1; CHECK(touches(&g, in, 2, 1.0f, 0.3f, 1, NULL, NULL) == 2);   /* con el puño también */
    vrg_init(&g); in.fist_btn[1] = 0; CHECK(touches(&g, in, 2, 1.0f, 0.3f, 0, NULL, NULL) == 2);   /* la otra mano */
    vrg_init(&g); CHECK(touches(&g, in, 2, 5.0f, 0.06f, 1, &why, NULL) == 1);                      /* demasiado seguidos: enfriamiento (0,25 s) */
    vrg_init(&g); vrg_in_t nw = in; nw.on_wall = 0; CHECK(touches(&g, nw, 2, 1.0f, 0.3f, 1, &why, NULL) == 0 && !strcmp(why, "no_pegado"));
    vrg_init(&g); vrg_in_t en = in; en.n_en = 1; en.en[0].p[0] = 0.2f; en.en[0].p[1] = 1.2f; en.en[0].p[2] = 0.6f;     /* enemigo junto a la mano: es golpe, no pared */
    CHECK(touches(&g, en, 2, 1.0f, 0.3f, 1, &why, NULL) == 0 && !strcmp(why, "enemigo"));
    vrg_init(&g); vrg_in_t aw = in; aw.weapon = 0; CHECK(touches(&g, aw, 2, 1.0f, 0.3f, 1, NULL, NULL) == 0);
    vrg_init(&g); vrg_in_t nv = in; vrg_out_t o; nv.wall_valid[0] = nv.wall_valid[1] = 0; settle(&g, &nv, 12); nv.wall_dist[1] = 0.02f; upd(&g, &nv, &o); CHECK(o.knock_side < 0);   /* sin consulta de pared */
    /* acercarse rápido y parar a 0,2 m: lejos de la pared */
    vrg_init(&g); in.wall_valid[1] = 1; settle(&g, &in, 12); float d = 0.6f; why = NULL;
    for (int i = 0; i < 8; i++) { d -= 0.02f; in.wall_dist[1] = d; upd(&g, &in, &o); }
    for (int i = 0; i < 6; i++) { in.wall_dist[1] = d; upd(&g, &in, &o); if (o.knock_why) why = o.knock_why; }
    (void)why;
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

int main(void) {
    t_punch(); t_contact(); t_rules(); t_throw(); t_walk(); t_grab(); t_chest(); t_knock(); t_peek(); t_robust();
    printf("%d comprobaciones, %d fallos\n", checks, fails);
    return fails != 0;
}
