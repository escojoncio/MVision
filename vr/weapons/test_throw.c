/* Pruebas de arrojadizas y colocables (headless): estimación de velocidad de lanzamiento con trayectoria
 * sintética, temporizador/cocinado, anilla, modos de ayuda, colocación, detonador y sanidad de mallas.
 * Compilar: gcc -std=gnu11 -I. test_throw.c w_throw.c w_placed.c wcore.c -lm */
#include "w_throw.h"
#include "wcore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------- puente simulado ---------------- */
typedef struct { int ammo; int taken, fires, sounds_dry, sounds_empty, sounds_pin, sounds_lever; WLaunch ev[16]; int nev; int accept; } Mock;
static Mock M;
static int b_reserve(int id) { (void)id; return M.ammo; }
static void b_take(int id, int n) { (void)id; M.taken += n; M.ammo -= n; }
static void b_fire(int id, WPose p) { (void)id; (void)p; M.fires++; }
static void b_haptic(int h, float a, float s) { (void)h; (void)a; (void)s; }
static void b_sound(int id, int e) {
    (void)id;
    if (e == WS_DRY) M.sounds_dry++;
    if (e == W5_S_EMPTY) M.sounds_empty++;
    if (e == W5_S_PIN) M.sounds_pin++;
    if (e == W5_S_LEVER) M.sounds_lever++;
}
static int hook(void *u, const WLaunch *e) { (void)u; if (M.nev < 16) M.ev[M.nev++] = *e; return M.accept; }
static WGameBridge G = { b_reserve, b_take, b_fire, b_haptic, b_sound, NULL, NULL, NULL, NULL };
static void mock_reset(int ammo) { memset(&M, 0, sizeof M); M.ammo = ammo; M.accept = 1; w_throw_set_hook(hook, NULL); }

/* ---------------- arma bajo prueba ---------------- */
typedef struct { WeaponState *s; WInput in; WReloadMode rm; float t; } Rig;
static Rig rig_new(const WeaponDef *d, WReloadMode rm) {
    Rig r; memset(&r, 0, sizeof r);
    r.s = calloc(1, d->state_size); r.s->def = d; d->init(r.s); r.rm = rm;
    r.in.head.p = wv(0, 1.7f, 0); r.in.head.q = wquat_identity();
    r.in.hand[0].q = r.in.hand[1].q = wquat_identity();
    r.in.hand[0].p = wv(0.25f, 1.0f, -0.3f); r.in.hand[1].p = wv(-0.25f, 1.0f, -0.3f);
    r.in.dt = 1.0f / 90.0f;
    return r;
}
static void step(Rig *r, int n) { for (int i = 0; i < n; i++) { r->s->def->update(r->s, &r->in, r->rm, &G); r->t += r->in.dt; } }
static void to_chest(Rig *r, float dist) { WVec3 c; w5_body_frame(&r->in, &c, NULL); r->in.hand[0].p = wv_add(c, wv(dist, 0, 0)); }
static void draw(Rig *r) { to_chest(r, 0.05f); r->in.grip[0] = 0; step(r, 2); r->in.grip[0] = 1; step(r, 2); }

/* ---------------- trayectoria sintética de lanzamiento ---------------- */
static uint32_t rs = 12345;
static float rnd(void) { rs = rs * 1664525u + 1013904223u; return ((rs >> 8) & 0xFFFF) / 65535.0f - 0.5f; }
static float gauss(void) { return (rnd() + rnd() + rnd() + rnd()) * 0.866f; }   /* ~N(0,1) */
static const float ALPHA = 60.0f, RAD = 0.6f;
static float phi(float t) { return -0.9f + 0.5f * ALPHA * t * t; }
static WPose traj(float t) {
    WPose p; float f = phi(t);
    p.p = wv(0.25f, 1.4f + RAD * sinf(f), -0.1f - RAD * cosf(f));
    p.q = wquat_axis_angle(wv(1, 0, 0), f);
    return p;
}
static WVec3 traj_v(float t) { float h = 1e-4f; return wv_scale(wv_sub(traj(t + h).p, traj(t - h).p), 1.0f / (2 * h)); }
static WVec3 traj_w(float t) { return wv(ALPHA * t, 0, 0); }

static void test_estimator(void) {
    printf("-- estimador de velocidad --\n");
    const float rates[3] = { 72, 90, 120 };
    for (int ri = 0; ri < 3; ri++) {
        float worst_v = 0, worst_a = 0, worst_w = 0;
        for (int trial = 0; trial < 40; trial++) {
            WVHist h; wvh_reset(&h);
            float dt = 1.0f / rates[ri];
            float t_det = 0.16f + 0.001f * trial * 1.7f;
            int n = (int)(t_det / dt);
            float t = 0;
            for (int i = 0; i <= n; i++) {
                t = i * dt;
                WPose p = traj(t);
                p.p.x += 0.0005f * gauss(); p.p.y += 0.0005f * gauss(); p.p.z += 0.0005f * gauss();   /* ruido 0,5 mm */
                p.q = wquat_normalize(wquat_mul(wquat_axis_angle(wv(0, 1, 0), 0.003f * gauss()), p.q));   /* 0,17 grados */
                wvh_push(&h, dt, p);
            }
            WVec3 v, w; wvh_estimate(&h, 0.070f, 0.025f, &v, &w);
            WVec3 tv = traj_v(t - 0.025f), tw = traj_w(t - 0.025f);
            float err = wv_len(wv_sub(v, tv)) / wv_len(tv);
            float ang = acosf(wcore_clampf(wv_dot(wv_norm(v), wv_norm(tv)), -1, 1)) * 57.29578f;
            float werr = wv_len(wv_sub(w, tw)) / wv_len(tw);
            if (err > worst_v) worst_v = err;
            if (ang > worst_a) worst_a = ang;
            if (werr > worst_w) worst_w = werr;
        }
        printf("  %3.0f Hz: error velocidad lineal max %.1f %%, angulo max %.1f grados, angular max %.1f %%\n",
               rates[ri], worst_v * 100, worst_a, worst_w * 100);
        CHECK(worst_v < 0.14f, "error lineal %.3f a %.0f Hz", worst_v, rates[ri]);
        CHECK(worst_a < 7.5f, "error angular de direccion %.2f", worst_a);
        CHECK(worst_w < 0.25f, "error de velocidad angular %.3f", worst_w);
    }
    /* mano quieta: velocidad ~0 */
    WVHist h; wvh_reset(&h); WPose q0 = { {0, 1, 0}, {0, 0, 0, 1} };
    for (int i = 0; i < 30; i++) wvh_push(&h, 1.0f / 90, q0);
    WVec3 v, w; wvh_estimate(&h, 0.07f, 0.025f, &v, &w);
    CHECK(wv_len(v) < 1e-4f && wv_len(w) < 1e-3f, "mano quieta debe dar velocidad nula");
}

/* ---------------- interaccion de arrojadiza ---------------- */
static void pull_ring(Rig *r) {
    WPose rp = wcore_point_world(r->s, &r->in, WP_CHARGE);
    r->in.hand[1].p = rp.p; r->in.grip[1] = 0; step(r, 1);
    r->in.grip[1] = 1; step(r, 2);
    for (int i = 0; i < 40 && r->s->phase != WT_PH_ARMED; i++) { r->in.hand[1].p.z += 0.003f; step(r, 1); }
}

static void do_throw(Rig *r, float t_det, WVec3 *tv, WVec3 *tw) {
    float dt = r->in.dt; int n = (int)(t_det / dt);
    for (int i = 0; i <= n; i++) { r->in.hand[0] = traj(i * dt); step(r, 1); }
    *tv = traj_v((n + 2) * dt - 0.025f); *tw = traj_w((n + 2) * dt - 0.025f);   // la suelta se detecta 2 frames despues
    r->in.grip[0] = 0.6f; r->in.hand[0] = traj((n + 1) * dt); step(r, 1);   /* los dedos se abren */
    r->in.grip[0] = 0.1f; r->in.hand[0] = traj((n + 2) * dt); step(r, 1);
}

static void test_throw_flow(void) {
    printf("-- arrojadiza: flujo completo (manual) --\n");
    mock_reset(3);
    Rig r = rig_new(&w_throw_frag, WR_MANUAL);
    WThrowState *t = (WThrowState *)r.s;
    CHECK(r.s->phase == WT_PH_VEST, "empieza en el chaleco");
    to_chest(&r, 0.30f); r.in.grip[0] = 0; step(&r, 2); r.in.grip[0] = 1; step(&r, 2);
    CHECK(r.s->phase == WT_PH_VEST, "agarre lejos del pecho (30 cm) no saca en manual");
    r.in.grip[0] = 0; step(&r, 2);
    draw(&r);
    CHECK(r.s->phase == WT_PH_HELD, "sacada del chaleco");
    r.in.hand[0].p = wv(0.25f, 1.1f, -0.35f); step(&r, 1);
    /* soltar con pasador puesto: vuelve al chaleco sin consumir */
    r.in.grip[0] = 0.0f; step(&r, 1);
    CHECK(r.s->phase == WT_PH_VEST && M.taken == 0 && M.nev == 0, "soltar con pasador: vuelve sin gastar");
    r.in.grip[0] = 0; step(&r, 2); draw(&r);
    r.in.hand[0].p = wv(0.25f, 1.1f, -0.35f); step(&r, 1);
    /* tirar de la anilla */
    WPose rp = wcore_point_world(r.s, &r.in, WP_CHARGE);
    r.in.hand[1].p = wv_add(rp.p, wv(0, 0, 0.0f)); r.in.grip[1] = 1; step(&r, 1);
    CHECK(t->ring_grabbed, "anilla agarrada");
    for (int i = 0; i < 6; i++) { r.in.hand[1].p.z += 0.004f; step(&r, 1); }
    CHECK(r.s->phase == WT_PH_PULL && t->ring_pull > 0.3f && t->ring_pull < 1.0f, "tiron parcial: %.2f", t->ring_pull);
    CHECK(r.s->channel[WCH_CUSTOM0] > 0.3f, "canal de anilla sigue el tiron");
    r.in.grip[1] = 0; step(&r, 10);
    CHECK(t->ring_pull < 0.05f && r.s->phase == WT_PH_HELD, "soltar la anilla antes de tiempo: el pasador vuelve");
    pull_ring(&r);
    CHECK(r.s->phase == WT_PH_ARMED, "pasador fuera");
    CHECK(r.s->held[0].active, "anilla visible en la mano 1");
    float d = wv_len(wv_sub(r.s->held[0].world.p, r.in.hand[1].p));
    CHECK(d < 0.08f, "anilla pegada a la mano 1 (%.3f)", d);
    CHECK(M.sounds_pin == 1, "sonido de pasador");
    CHECK(r.s->channel[WCH_CUSTOM1] < 0.01f, "palanca aun cerrada");
    r.in.hand[1].p.x -= 0.3f; step(&r, 3);
    float d2 = wv_len(wv_sub(r.s->held[0].world.p, r.in.hand[1].p));
    CHECK(fabsf(d2 - d) < 1e-3f, "la anilla acompana a la mano 1");
    r.in.grip[1] = 0; step(&r, 2);
    CHECK(!r.s->held[0].active, "anilla soltada desaparece");
    /* lanzamiento */
    WVec3 tv, tw;
    do_throw(&r, 0.16f, &tv, &tw);
    CHECK(M.nev == 1 && M.ev[0].kind == WL_THROW, "evento de lanzamiento (%d)", M.nev);
    if (M.nev) {
        WLaunch *e = &M.ev[0];
        float err = wv_len(wv_sub(e->lin_vel, tv)) / wv_len(tv);
        printf("  v estimada %.2f %.2f %.2f  real %.2f %.2f %.2f  (%.1f %%)\n", e->lin_vel.x, e->lin_vel.y, e->lin_vel.z, tv.x, tv.y, tv.z, err * 100);
        CHECK(err < 0.15f, "error velocidad lineal %.3f", err);
        CHECK(wv_len(wv_sub(e->ang_vel, tw)) / wv_len(tw) < 0.3f, "velocidad angular");
        CHECK(fabsf(e->fuse_left - 3.0f) < 0.01f, "espoleta %.2f", e->fuse_left);
        CHECK(e->cooked == 0, "sin cocinar");
        CHECK(wv_len(e->lin_vel) <= 22.01f, "tope de velocidad");
    }
    CHECK(M.taken == 1 && M.ammo == 2, "municion descontada una vez");
    CHECK(M.sounds_lever == 1, "sonido de palanca");
    step(&r, 10); CHECK(r.s->channel[WCH_CUSTOM1] > 0.8f, "la palanca salto (%.2f)", r.s->channel[WCH_CUSTOM1]);
    step(&r, 60);
    CHECK(r.s->phase == WT_PH_VEST, "listo para sacar otra");
    free(r.s);
}

static void test_cook(void) {
    printf("-- cocinado y espoleta --\n");
    mock_reset(3);
    Rig r = rig_new(&w_throw_b, WR_MANUAL);
    draw(&r); r.in.hand[0].p = wv(0.25f, 1.1f, -0.35f); step(&r, 1); pull_ring(&r);
    r.in.grip[1] = 0; step(&r, 1);
    CHECK(r.s->phase == WT_PH_ARMED, "armada");
    r.in.trigger[0] = 0; step(&r, 3); r.in.trigger[0] = 1.0f; step(&r, 1);
    CHECK(r.s->phase == WT_PH_COOK, "cocinando");
    step(&r, (int)(1.2f * 90));
    WVec3 tv, tw; r.in.trigger[0] = 0;
    do_throw(&r, 0.16f, &tv, &tw);
    CHECK(M.nev == 1 && M.ev[0].kind == WL_THROW, "lanzamiento tras cocinar");
    if (M.nev) {
        printf("  cocinada %.2f s, espoleta restante %.2f s (total 2.50)\n", M.ev[0].cooked, M.ev[0].fuse_left);
        CHECK(fabsf(M.ev[0].fuse_left + M.ev[0].cooked - 2.5f) < 0.2f, "fuse+cooked = total");
        CHECK(M.ev[0].cooked > 1.2f && M.ev[0].fuse_left < 1.3f, "tiempos de cocinado");
    }
    free(r.s);
    /* explota en la mano */
    mock_reset(2);
    r = rig_new(&w_throw_c, WR_MANUAL);
    draw(&r); r.in.hand[0].p = wv(0.25f, 1.1f, -0.35f); step(&r, 1); pull_ring(&r);
    r.in.grip[1] = 0; step(&r, 1);
    r.in.trigger[0] = 0; step(&r, 2); r.in.trigger[0] = 1; step(&r, 1);
    step(&r, (int)(2.0f * 90));
    CHECK(M.nev == 1 && M.ev[0].kind == WL_INHAND, "explota en la mano (%d eventos)", M.nev);
    CHECK(M.taken == 1, "municion gastada al explotar");
    free(r.s);
}

static void test_assist_modes(void) {
    printf("-- modos simplificado / automatico --\n");
    mock_reset(3);
    Rig r = rig_new(&w_throw_frag, WR_SIMPLIFIED);
    to_chest(&r, 0.30f); r.in.grip[0] = 0; step(&r, 2); r.in.grip[0] = 1; step(&r, 2);
    CHECK(r.s->phase == WT_PH_HELD, "simplificado: radio de pecho ampliado");
    r.in.hand[0].p = wv(0.25f, 1.1f, -0.35f); step(&r, 1);
    WPose rp = wcore_point_world(r.s, &r.in, WP_CHARGE);
    r.in.hand[1].p = wv_add(rp.p, wv(0.09f, 0, 0)); r.in.grip[1] = 1; step(&r, 1);
    for (int i = 0; i < 10 && r.s->phase != WT_PH_ARMED; i++) { r.in.hand[1].p.z += 0.004f; step(&r, 1); }
    CHECK(r.s->phase == WT_PH_ARMED, "simplificado: agarre de anilla a 9 cm y tiron corto");
    free(r.s);

    mock_reset(3);
    r = rig_new(&w_throw_frag, WR_AUTOMATIC);
    r.in.hand[0].p = wv(0.5f, 0.2f, 0.3f); r.in.grip[0] = 0; step(&r, 2); r.in.grip[0] = 1; step(&r, 2);
    CHECK(r.s->phase == WT_PH_HELD, "automatico: se saca en cualquier sitio");
    r.in.trigger[0] = 1; step(&r, 30);
    CHECK(r.s->phase == WT_PH_ARMED && M.sounds_pin == 1, "automatico: la anilla se tira sola");
    CHECK(!r.s->held[0].active, "automatico: sin anilla en mano");
    free(r.s);

    mock_reset(0);
    r = rig_new(&w_throw_frag, WR_MANUAL);
    draw(&r);
    CHECK(r.s->phase == WT_PH_VEST && M.sounds_empty == 1, "sin granadas: no se saca");
    free(r.s);
}

/* ---------------- colocables ---------------- */
static void test_carga(void) {
    printf("-- carga adhesiva y detonador --\n");
    mock_reset(4);
    Rig r = rig_new(&w_placed_a, WR_MANUAL);
    WPlacedState *p = (WPlacedState *)r.s;
    CHECK(!r.s->held[0].active, "sin cargas: sin detonador");
    draw(&r);
    CHECK(r.s->phase == WPL_PH_HELD, "carga en mano");
    r.in.hand[0].p = wv(0.1f, 1.2f, -0.5f); r.in.hand[0].q = wquat_axis_angle(wv(0, 1, 0), 0.7f);
    r.in.trigger[0] = 1.0f; M.accept = 0; step(&r, 2);
    CHECK(M.taken == 0 && r.s->phase == WPL_PH_HELD && p->n_placed == 0, "juego rechaza (sin superficie): no se gasta");
    M.accept = 1; step(&r, 1);
    CHECK(M.nev >= 1 && M.ev[M.nev - 1].kind == WL_PLACE_C, "colocada");
    WLaunch e = M.ev[M.nev - 1];
    WVec3 want = wpose_apply(r.in.hand[0], wv(0, 0, -0.0225f));
    CHECK(wv_len(wv_sub(e.pose.p, want)) < 1e-4f, "pose de la cara adhesiva");
    CHECK(M.taken == 1 && p->n_placed == 1, "municion y contador");
    CHECK(r.s->held[0].active && wv_len(wv_sub(r.s->held[0].world.p, r.in.hand[1].p)) < 1e-4f, "detonador en mano 1");
    r.in.trigger[0] = 0; step(&r, 60);
    CHECK(r.s->phase == WPL_PH_VEST, "listo para otra");
    draw(&r); r.in.trigger[0] = 1; step(&r, 2);
    CHECK(p->n_placed == 2, "segunda carga");
    r.in.trigger[0] = 0; step(&r, 40);
    int nev0 = M.nev;
    r.in.trigger[1] = 1; step(&r, 10);
    CHECK(M.nev == nev0 && p->n_placed == 2, "tapa cerrada: el boton no detona");
    r.in.trigger[1] = 0; step(&r, 10);
    r.in.buttons[1] = WB_A; step(&r, 1); r.in.buttons[1] = 0; step(&r, 30);
    CHECK(r.s->channel[WCH_LID] > 0.95f, "tapa levantada (%.2f)", r.s->channel[WCH_LID]);
    r.in.trigger[1] = 1; step(&r, 10);
    int det = 0; for (int i = 0; i < M.nev; i++) det += M.ev[i].kind == WL_DETONATE;
    CHECK(det == 2 && p->n_placed == 0, "detonan las dos cargas (%d)", det);
    CHECK(!r.s->held[0].active, "sin cargas: detonador desaparece");
    free(r.s);
}

static void test_mina(void) {
    printf("-- mina direccional --\n");
    mock_reset(2);
    Rig r = rig_new(&w_placed_b, WR_MANUAL);
    draw(&r);
    CHECK(r.s->channel[WCH_CUSTOM0] < 0.5f, "patas plegadas al sacarla");
    r.in.hand[0].q = wquat_axis_angle(wv(0, 1, 0), -1.1f); r.in.trigger[0] = 1; step(&r, 3);
    CHECK(M.nev == 0, "no se coloca con las patas a medio desplegar");
    step(&r, 40);
    CHECK(r.s->channel[WCH_CUSTOM0] > 0.95f, "patas desplegadas");
    CHECK(M.nev == 1 && M.ev[0].kind == WL_PLACE_MINE, "colocada");
    if (M.nev) {
        WQuat a = M.ev[0].pose.q, b = r.in.hand[0].q;
        float dot = fabsf(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
        CHECK(dot > 0.9999f, "orientacion igual a la de la mano");
        WVec3 f = wquat_rotate(a, wv(0, 0, -1));
        CHECK(fabsf(f.x - (-sinf(-1.1f))) < 1e-3f, "frente de la mina = -Z local girado");
    }
    CHECK(M.taken == 1, "municion");
    free(r.s);
}

/* ---------------- robustez: dt, rechazos del juego, reservas ---------------- */
static void arm_it(Rig *r) {      /* saca, tira de la anilla y deja la arrojadiza armada con la mano 1 libre */
    draw(r); r->in.hand[0].p = wv(0.25f, 1.1f, -0.35f); step(r, 1); pull_ring(r);
    r->in.grip[1] = 0; step(r, 1);
}

static void test_robust(void) {
    printf("-- robustez --\n");
    /* palanca con dt grande: el resorte no diverge y el canal queda acotado */
    mock_reset(3);
    Rig r = rig_new(&w_throw_frag, WR_MANUAL);
    arm_it(&r);
    CHECK(r.s->phase == WT_PH_ARMED, "armada");
    r.in.trigger[0] = 0; step(&r, 3); r.in.trigger[0] = 1; step(&r, 1);
    CHECK(r.s->phase == WT_PH_COOK, "cocinando");
    r.in.dt = 0.2f;
    for (int i = 0; i < 8; i++) {
        step(&r, 1);
        float c = r.s->channel[WCH_CUSTOM1];
        CHECK(isfinite(c) && c >= 0 && c <= 1.5f, "canal de palanca fuera de rango con dt=0,2: %f", c);
    }
    free(r.s);
    /* dt inválido: no avanza nada y no rompe */
    mock_reset(3);
    r = rig_new(&w_throw_frag, WR_MANUAL);
    const float dts[] = {0.0f, -1.0f, NAN, INFINITY, 9.0f};
    for (unsigned k = 0; k < sizeof dts / sizeof *dts; k++) { r.in.dt = dts[k]; step(&r, 5); }
    CHECK(isfinite(r.s->channel[WCH_CUSTOM1]) && isfinite(r.s->recoil_offset.p.y), "estado finito");
    free(r.s);
    mock_reset(3);
    Rig rp = rig_new(&w_placed_a, WR_MANUAL);
    rp.in.dt = 0.2f; draw(&rp); rp.in.hand[0].p = wv(0.1f, 1.2f, -0.5f); rp.in.trigger[0] = 0; step(&rp, 5);
    rp.in.dt = NAN; step(&rp, 3); rp.in.dt = 0; step(&rp, 3);
    CHECK(isfinite(rp.s->channel[WCH_CUSTOM1]) && isfinite(rp.s->channel[WCH_LID]), "placed: estado finito");
    free(rp.s);

    /* el juego rechaza el lanzamiento: no se gasta munición ni pasa a GONE; luego sí acepta */
    mock_reset(3); M.accept = 0;
    r = rig_new(&w_throw_frag, WR_MANUAL);
    arm_it(&r);
    WVec3 tv, tw;
    do_throw(&r, 0.16f, &tv, &tw);
    CHECK(M.nev >= 1 && M.taken == 0 && r.s->phase == WT_PH_COOK, "rechazo: munición %d fase %d", M.taken, r.s->phase);
    int n_rej = M.nev;
    step(&r, 90);
    CHECK(M.nev > n_rej && M.nev < n_rej + 12 && M.taken == 0, "reintenta con espera, sin saturar (%d eventos)", M.nev - n_rej);
    M.accept = 1; step(&r, 30);
    CHECK(r.s->phase == WT_PH_GONE || r.s->phase == WT_PH_VEST, "al aceptar sale");
    CHECK(M.taken == 1 && M.ammo == 2, "se gasta una vez (%d)", M.taken);
    free(r.s);

    /* velocidad angular y lineal acotadas aunque la mano gire como loca */
    mock_reset(3);
    r = rig_new(&w_throw_frag, WR_MANUAL);
    arm_it(&r);
    for (int i = 0; i < 12; i++) {
        r.in.hand[0].p = wv(0.25f + 0.3f * i, 1.2f, -0.35f);
        r.in.hand[0].q = wquat_axis_angle(wv(1, 0.3f, 0), 1.2f * i);       /* >100 rad/s */
        step(&r, 1);
    }
    r.in.grip[0] = 0.0f; step(&r, 1);
    CHECK(M.nev == 1 && M.ev[0].kind == WL_THROW, "lanzó");
    if (M.nev) {
        CHECK(wv_len(M.ev[0].ang_vel) <= 45.01f, "ang_vel acotada: %f", wv_len(M.ev[0].ang_vel));
        CHECK(wv_len(M.ev[0].lin_vel) <= 22.01f, "lin_vel acotada: %f", wv_len(M.ev[0].lin_vel));
        CHECK(isfinite(M.ev[0].lin_vel.x) && isfinite(M.ev[0].ang_vel.x), "velocidades finitas");
    }
    free(r.s);

    /* reserva nunca negativa: el puente devuelve -5 y no se descuenta de más */
    mock_reset(-5);
    r = rig_new(&w_throw_frag, WR_MANUAL);
    draw(&r);
    CHECK(r.s->phase == WT_PH_VEST && r.s->mag_rounds == 0 && M.taken == 0, "reserva negativa tratada como 0");
    free(r.s);
    /* se gasta la reserva fuera con la granada armada: al lanzar no se descuenta (no baja de 0) */
    mock_reset(1);
    r = rig_new(&w_throw_frag, WR_MANUAL);
    arm_it(&r);
    M.ammo = 0;
    do_throw(&r, 0.16f, &tv, &tw);
    CHECK(M.taken == 0 && M.ammo == 0, "no descuenta con reserva 0 (%d)", M.taken);
    free(r.s);

    /* carga adhesiva: sacar con el gatillo apretado no coloca; el rechazo del juego conserva las cargas */
    mock_reset(4);
    Rig rc = rig_new(&w_placed_a, WR_MANUAL);
    WPlacedState *p = (WPlacedState *)rc.s;
    rc.in.trigger[0] = 1; draw(&rc); step(&rc, 5);
    CHECK(M.nev == 0 && p->n_placed == 0, "gatillo apretado al sacar no coloca");
    rc.in.trigger[0] = 0; step(&rc, 3); rc.in.trigger[0] = 1; step(&rc, 3);
    CHECK(p->n_placed == 1, "tras soltar el gatillo coloca");
    rc.in.trigger[0] = 0; step(&rc, 60); draw(&rc); rc.in.trigger[0] = 0; step(&rc, 3); rc.in.trigger[0] = 1; step(&rc, 3);
    CHECK(p->n_placed == 2, "segunda carga");
    rc.in.trigger[0] = 0; step(&rc, 40);
    rc.in.buttons[1] = WB_A; step(&rc, 1); rc.in.buttons[1] = 0; step(&rc, 30);
    M.accept = 0; rc.in.trigger[1] = 1; step(&rc, 10);
    CHECK(p->n_placed == 2, "el juego rechaza la detonación: las cargas se conservan (%d)", p->n_placed);
    M.accept = 1; rc.in.trigger[1] = 0; step(&rc, 40);
    rc.in.buttons[1] = WB_A; step(&rc, 1); rc.in.buttons[1] = 0; step(&rc, 30);
    rc.in.trigger[1] = 1; step(&rc, 10);
    CHECK(p->n_placed == 0, "al aceptar detonan (%d)", p->n_placed);
    free(rc.s);
}

static void test_ids(void) {
    printf("-- ids neutros --\n");
    const WeaponDef *ds[5] = { &w_throw_frag, &w_throw_b, &w_throw_c, &w_placed_a, &w_placed_b };
    const char *want[5] = { "throw_frag", "throw_b", "throw_c", "placed_a", "placed_b" };
    for (int i = 0; i < 5; i++) {
        CHECK(!strcmp(ds[i]->id, want[i]), "id %s", ds[i]->id);
        for (int j = i + 1; j < 5; j++) CHECK(ds[i]->game_weapon_id != ds[j]->game_weapon_id, "id de juego repetido");
        /* init sin asignar s->def: cada variante conoce su definición */
        WeaponState *s = calloc(1, ds[i]->state_size); ds[i]->init(s);
        CHECK(s->def == ds[i] && s->recoil_offset.q.w == 1.0f, "init de %s", want[i]);
        free(s);
    }
    mock_reset(3);
    Rig a = rig_new(&w_throw_b, WR_MANUAL), b = rig_new(&w_throw_c, WR_MANUAL);
    CHECK(((WThrowState *)a.s)->kind == 1 && ((WThrowState *)b.s)->kind == 2, "kind por variante");
    free(a.s); free(b.s);
}

/* ---------------- sanidad de mallas y tablas generadas ---------------- */
static void test_defs(void) {
    printf("-- definiciones y mallas --\n");
    const WeaponDef *ds[5] = { &w_throw_frag, &w_throw_b, &w_throw_c, &w_placed_a, &w_placed_b };
    for (int i = 0; i < 5; i++) {
        const WeaponDef *d = ds[i];
        CHECK(d->nparts > 0, "%s tablas", d->id);
        for (int k = 0; k < WP_COUNT; k++) {
            const WQuat *q = &d->points[k].q;
            CHECK(fabsf(q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w - 1) < 1e-4f, "%s punto %d con cuaternión nulo", d->id, k);
        }
        WeaponState *ws = calloc(1, d->state_size); d->init(ws);   /* toda pieza resuelve su hueco de mesh por nombre */
        for (int k = 0; k < d->nparts; k++) CHECK(weapon_part_mesh(ws, k) >= 0, "%s: la pieza %s no tiene hueco en mesh_names", d->id, d->parts[k].name);
        free(ws);
        long tris = 0;
        for (int k = 0; k < d->nparts; k++) {
            const WPart *pt = &d->parts[k];
            CHECK(pt->mesh == -1 && pt->name, "%s parte sin mesh asociado", d->id);
            CHECK(pt->parent < k, "%s parte %s padre", d->id, pt->name);
            float q = pt->rest.q.x * pt->rest.q.x + pt->rest.q.y * pt->rest.q.y + pt->rest.q.z * pt->rest.q.z + pt->rest.q.w * pt->rest.q.w;
            CHECK(fabsf(q - 1) < 1e-4f, "%s parte %s cuaternion", d->id, pt->name);
        }
        printf("  %-12s %d piezas esperadas%s\n", d->id, d->nparts, tris ? "" : "");
        CHECK(d->state_size >= sizeof(WeaponState), "state_size");
    }
}

int main(void) {
    test_estimator();
    test_throw_flow();
    test_cook();
    test_assist_modes();
    test_carga();
    test_mina();
    test_defs();
    test_robust();
    test_ids();
    printf("\n%d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
