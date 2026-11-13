/* Pruebas de arrojadizas y colocables (headless): estimación de velocidad de lanzamiento con trayectoria
 * sintética, lanzamiento con el gatillo (apretar = preparada, soltar = lanzar con la velocidad de la mano; el agarre no interviene; sin anilla ni pasador sueltos), soltar suave,
 * colocación al soltar el gatillo (sin tapa ni botón virtuales) y sanidad de mallas.
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
typedef struct { int ammo; int taken, fires, sounds_dry, sounds_empty, sounds_pin, sounds_lever, sounds_throw; WLaunch ev[16]; int nev; int accept; } Mock;
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
    if (e == W5_S_THROW) M.sounds_throw++;
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
/* El objeto equipado aparece en la mano sin gesto (sin chaleco): basta actualizar con la mano principal cerrada. */
static void draw(Rig *r) { r->in.grip[0] = 1; step(r, 4); }

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
static int any_held(const Rig *r) { for (int i = 0; i < W_MAX_HELD; i++) if (r->s->held[i].active) return 1; return 0; }
/* gatillo suelto y apretado con el objeto en la mano: lo deja preparado (el agarre se queda cerrado todo el rato: no interviene) */
static void ready_it(Rig *r) {
    r->in.trigger[0] = 0; step(r, 2);
    r->in.trigger[0] = 1; step(r, 2);
}

/* la mano sigue la trayectoria con el gatillo apretado y lo suelta al final: empieza a soltarse en el fotograma n+1 (0,6) y se detecta en el n+2 (0,1);
 * la velocidad que vale es la de la mano cuando el gatillo empezó a soltarse (n+1), no la de cuando se detecta la suelta */
#define REL_LAG 0.010f      /* margen mínimo antes del instante de soltar con el que se mide (w_throw.c, V_LAG_MIN) */
static void do_throw(Rig *r, float t_det, WVec3 *tv, WVec3 *tw) {
    float dt = r->in.dt; int n = (int)(t_det / dt);
    r->in.trigger[0] = 1;
    for (int i = 0; i <= n; i++) { r->in.hand[0] = traj(i * dt); step(r, 1); }
    *tv = traj_v((n + 1) * dt - REL_LAG); *tw = traj_w((n + 1) * dt - REL_LAG);
    r->in.trigger[0] = 0.6f; r->in.hand[0] = traj((n + 1) * dt); step(r, 1);   /* el dedo empieza a soltar */
    r->in.trigger[0] = 0.1f; r->in.hand[0] = traj((n + 2) * dt); step(r, 1);
}

/* Gatillo lento: la mano frena justo tras empezar a soltar y el soltar (0,7 -> 0,4) tarda 0,2 s; la velocidad que vale es la del pico (log 13-11: lanzadas medidas a 0,2-2 m/s, caian a los pies) */
static void test_throw_slow_release(void) {
    mock_reset(3);
    Rig r = rig_new(&w_throw_frag, WR_MANUAL);
    const float vpk = 8.0f, dt = 0.02f;
    r.in.dt = dt;
    float x = 0, t = 0;
    r.in.trigger[0] = 0; step(&r, 3);
    r.in.trigger[0] = 1; step(&r, 3);
    for (int i = 0; i < 40 && M.nev == 0; i++) {
        t += dt;
        float v = t < 0.30f ? vpk * t / 0.30f : fmaxf(0.0f, vpk * (1.0f - (t - 0.30f) / 0.10f));
        x += v * dt;
        r.in.hand[0].p = wv(0.25f, 1.0f, -0.3f - x);
        r.in.trigger[0] = t < 0.30f ? 1.0f : (t < 0.30f + 0.2f ? 0.6f : 0.1f);      /* empieza a soltar en el pico y no cruza 0,4 hasta 0,2 s despues */
        step(&r, 1);
    }
    CHECK(M.nev == 1 && M.ev[0].kind == WL_THROW, "gatillo lento: lanza");
    if (M.nev) { printf("  gatillo lento: %.2f m/s (pico %.1f)\n", (double)wv_len(M.ev[0].lin_vel), (double)vpk); CHECK(wv_len(M.ev[0].lin_vel) >= 0.85f * vpk, "gatillo lento: velocidad %.2f < 85 %% del pico", (double)wv_len(M.ev[0].lin_vel)); }
    free(r.s);
}

/* Gatillo lento tras un amago hacia atrás: la mano va hacia atrás rápido (preparar el brazo), invierte justo antes de empezar a soltar el gatillo y avanza despacio. La ventana lenta (hasta V_LAG_SLOW antes) ve el
 * amago (más rápido que la ventana corta pero en sentido contrario); no debe ganar por |v| sin mirar la dirección: la granada saldría hacia atrás. Solo vale si apunta a menos de ~60 grados de la ventana corta o del frente de la mano. */
static float slow_dir_run(float back_speed, float fwd_speed, float t_rev) {
    mock_reset(3);
    Rig r = rig_new(&w_throw_frag, WR_MANUAL);
    const float dt = 0.02f;
    r.in.dt = dt;
    float z = 0, t = 0, vz = 0;
    r.in.trigger[0] = 0; step(&r, 3);
    r.in.trigger[0] = 1; step(&r, 3);
    for (int i = 0; i < 80 && M.nev == 0; i++) {
        t += dt;
        const float v = t < t_rev ? back_speed : -fwd_speed;           /* +Z = hacia atrás (el frente de la mano es -Z) */
        z += v * dt;
        r.in.hand[0].p = wv(0.25f, 1.0f, -0.3f + z);
        r.in.trigger[0] = t < 0.45f ? 1.0f : (t < 0.45f + 0.2f ? 0.6f : 0.1f);
        step(&r, 1);
    }
    CHECK(M.nev == 1 && M.ev[0].kind == WL_THROW, "gatillo lento tras un amago: lanza");
    if (M.nev) vz = M.ev[0].lin_vel.z;
    free(r.s);
    return vz;
}
static void test_throw_slow_release_direction(void) {
    printf("-- arrojadiza: gatillo lento tras un amago hacia atrás --\n");
    const float vz = slow_dir_run(6.0f, 1.5f, 0.46f);
    printf("  amago atrás 6 m/s, avance 1,5 m/s: vz = %.2f\n", (double)vz);
    CHECK(vz < 0.0f, "gatillo lento: la granada sale hacia delante, no hacia el amago (vz %.2f)", (double)vz);
    const float vz2 = slow_dir_run(1.0f, 6.0f, 0.30f);                    /* sin amago contrario: la ventana lenta más rápida sigue ganando (el lanzamiento fuerte antes de frenar) */
    CHECK(vz2 < -3.0f, "gatillo lento sin amago: sigue ganando la ventana rápida del pico (vz %.2f)", (double)vz2);
}

static void test_throw_flow(void) {
    printf("-- arrojadiza: flujo con el gatillo --\n");
    mock_reset(3);
    Rig r = rig_new(&w_throw_frag, WR_MANUAL);
    CHECK(r.s->phase == WT_PH_VEST, "empieza sin objeto");
    r.in.trigger[0] = 1; draw(&r);
    CHECK(r.s->phase == WT_PH_HELD, "aparece en la mano al equiparla, sin gesto del chaleco");
    r.in.hand[0].p = wv(0.25f, 1.1f, -0.35f); step(&r, 30);
    CHECK(r.s->phase == WT_PH_HELD && !any_held(&r), "con el gatillo apretado al sacarla no queda preparada; sin piezas sueltas");
    r.in.trigger[0] = 0.0f; step(&r, 3);
    CHECK(r.s->phase == WT_PH_HELD && M.nev == 0 && M.taken == 0, "soltar sin haberla preparado: sigue en la mano, sin lanzar ni gastar");
    r.in.trigger[0] = 0.5f; step(&r, 2);
    CHECK(r.s->phase == WT_PH_HELD, "gatillo a medias (histeresis): no se prepara");
    r.in.trigger[0] = 1.0f; step(&r, 2);
    CHECK(r.s->phase == WT_PH_READY, "gatillo apretado tras haberlo soltado: preparada (el anfitrion pulsa el boton del juego)");
    CHECK(M.sounds_pin == 1 && !any_held(&r), "sonido de pasador y ninguna pieza suelta flotando");
    CHECK(M.nev == 0 && M.taken == 0, "preparar no lanza ni gasta");
    r.in.trigger[0] = 0.5f; step(&r, 5);
    CHECK(r.s->phase == WT_PH_READY && M.nev == 0, "gatillo a medias: sigue preparada, no lanza");
    /* mantener el gatillo mientras la mano se mueve rapido: nada sale */
    r.in.trigger[0] = 1.0f;
    for (int i = 0; i < 90; i++) { r.in.hand[0].p = wv(0.25f + 0.05f * (float)i, 1.1f, -0.35f); step(&r, 1); }
    CHECK(r.s->phase == WT_PH_READY && M.nev == 0 && M.taken == 0, "gatillo mantenido (1 s, mano a 4.5 m/s): no se lanza nada");
    /* lanzamiento por velocidad */
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
        CHECK(wv_len(e->lin_vel) > 1.0f, "lanzamiento rapido (%.2f m/s)", wv_len(e->lin_vel));
    }
    CHECK(M.taken == 1 && M.ammo == 2, "municion descontada una vez");
    CHECK(M.sounds_throw == 1, "sonido de lanzamiento");
    CHECK(r.s->phase == WT_PH_GONE && r.s->recoil_offset.p.y < -50.0f && !any_held(&r), "la malla VR se oculta al lanzar (se ve la granada del juego)");
    step(&r, 60);
    CHECK(r.s->phase == WT_PH_HELD, "la siguiente aparece en la mano");
    free(r.s);

    /* el agarre no prepara ni lanza: con el gatillo en reposo, cerrar y abrir la mano no hace nada */
    mock_reset(3);
    r = rig_new(&w_throw_frag, WR_MANUAL);
    draw(&r);
    r.in.grip[0] = 0; step(&r, 3);
    r.in.grip[0] = 1; step(&r, 30);
    CHECK(r.s->phase == WT_PH_HELD, "agarre cerrado: no queda preparada (fase %d)", r.s->phase);
    r.in.grip[0] = 0; step(&r, 5);
    CHECK(r.s->phase == WT_PH_HELD && M.nev == 0 && M.taken == 0, "abrir la mano sin gatillo: no lanza");
    free(r.s);
}

static void test_gentle(void) {
    printf("-- soltar suave y rechazos --\n");
    mock_reset(3);
    Rig r = rig_new(&w_throw_frag, WR_MANUAL);
    draw(&r); ready_it(&r);
    CHECK(r.s->phase == WT_PH_READY, "lista");
    r.in.hand[0].p = wv(0.25f, 1.1f, -0.35f); step(&r, 10);
    r.in.trigger[0] = 0.0f; step(&r, 1);
    CHECK(M.nev == 1 && M.ev[0].kind == WL_THROW && wv_len(M.ev[0].lin_vel) < 1.0f, "mano quieta: soltar suave (v=%.3f)", M.nev ? wv_len(M.ev[0].lin_vel) : -1.0f);
    CHECK(M.sounds_throw == 0 && M.taken == 1 && r.s->phase == WT_PH_GONE, "sin sonido de lanzamiento, gasta una y desaparece de la mano");
    free(r.s);

    /* sin reserva no aparece; con la reserva gastada fuera al lanzar no se descuenta de mas */
    mock_reset(0);
    r = rig_new(&w_throw_frag, WR_MANUAL);
    draw(&r);
    CHECK(r.s->phase == WT_PH_VEST, "sin granadas: no aparece");
    free(r.s);
    mock_reset(1);
    r = rig_new(&w_throw_frag, WR_MANUAL);
    draw(&r); ready_it(&r);
    M.ammo = 0;
    WVec3 tv, tw;
    do_throw(&r, 0.16f, &tv, &tw);
    CHECK(M.nev == 1 && M.taken == 0 && M.ammo == 0, "no descuenta con reserva 0 (%d)", M.taken);
    free(r.s);

    /* el juego rechaza el lanzamiento: no se gasta municion ni pasa a GONE; sigue lista, reintenta con espera y luego sale */
    mock_reset(3); M.accept = 0;
    r = rig_new(&w_throw_frag, WR_MANUAL);
    draw(&r); ready_it(&r);
    do_throw(&r, 0.16f, &tv, &tw);
    CHECK(M.nev >= 1 && M.taken == 0 && r.s->phase == WT_PH_READY, "rechazo: municion %d fase %d", M.taken, r.s->phase);
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
    draw(&r); ready_it(&r);
    for (int i = 0; i < 12; i++) {
        r.in.hand[0].p = wv(0.25f + 0.3f * i, 1.2f, -0.35f);
        r.in.hand[0].q = wquat_axis_angle(wv(1, 0.3f, 0), 1.2f * i);       /* >100 rad/s */
        step(&r, 1);
    }
    r.in.trigger[0] = 0.0f; step(&r, 1);
    CHECK(M.nev == 1 && M.ev[0].kind == WL_THROW, "lanzo");
    if (M.nev) {
        CHECK(wv_len(M.ev[0].ang_vel) <= 45.01f, "ang_vel acotada: %f", wv_len(M.ev[0].ang_vel));
        CHECK(wv_len(M.ev[0].lin_vel) <= 22.01f, "lin_vel acotada: %f", wv_len(M.ev[0].lin_vel));
        CHECK(isfinite(M.ev[0].lin_vel.x) && isfinite(M.ev[0].ang_vel.x), "velocidades finitas");
    }
    free(r.s);

    /* variantes: la espoleta de cada una sale en el evento */
    const WeaponDef *vs[3] = { &w_throw_frag, &w_throw_b, &w_throw_c };
    const float fz[3] = { 3.0f, 2.5f, 1.8f };
    for (int k = 0; k < 3; k++) {
        mock_reset(2);
        r = rig_new(vs[k], WR_MANUAL);
        draw(&r); ready_it(&r); step(&r, 3); r.in.trigger[0] = 0; step(&r, 1);
        CHECK(M.nev == 1 && fabsf(M.ev[0].fuse_left - fz[k]) < 0.01f && M.ev[0].weapon_id == vs[k]->game_weapon_id, "variante %d: espoleta y id", k);
        free(r.s);
    }
}

/* ---------------- velocidad de salida proporcional a la del mando ---------------- */
/* Reloj de VBlank (~50 Hz) con fluctuacion y con rafagas (varios VBlanks casi juntos); la mano llega con la pose del ultimo muestreo de 90 Hz (hasta 11 ms de antiguedad),
 * con su marca de tiempo propia (WInput.hand_dt: tiempo entre poses, no entre VBlanks).
 * Sin reloj real: dt inyectado. El gatillo se aprieta en reposo, la mano se mueve con velocidad v(t) y el gatillo se suelta a 1 -> 0,6 -> 0,1 (tres VBlanks). */
typedef struct { uint32_t rs; int jitter; int stale; } Clk;
static float clk_rnd(Clk *c) { c->rs = c->rs * 1664525u + 1013904223u; return ((c->rs >> 8) & 0xFFFF) / 65535.0f; }
static float clk_dt(Clk *c) {
    if (!c->jitter) return 0.02f;
    float u = clk_rnd(c);
    if (u < 0.2f) return 0.001f + 0.002f * clk_rnd(c);          /* rafaga: el VBlank siguiente casi inmediato */
    return 0.014f + 0.026f * clk_rnd(c);                        /* 14..40 ms */
}
/* v(t): velocidad de la mano en m/s a lo largo de dir; acel = 0: constante v0; si no, v0 + acel * t. Devuelve 1 si hubo lanzamiento y escribe su velocidad y la real (en t del soltar) */
static int throw_run(float v0, float acel, WVec3 dir, uint32_t seed, int jitter, int stale, WVec3 *got, float *true_speed, const WeaponDef *def) {
    mock_reset(3);
    Rig r = rig_new(def, WR_MANUAL);
    Clk c = { seed, jitter, stale };
    const WVec3 p0 = wv(0.25f, 1.0f, -0.3f);
    r.in.hand[0].p = p0;
    r.in.trigger[0] = 0;                                                                          /* aparece y queda en reposo */
    for (int i = 0; i < 10; i++) { r.in.dt = clk_dt(&c); r.s->def->update(r.s, &r.in, r.rm, &G); }
    r.in.trigger[0] = 1;                                                                          /* preparada, mano quieta */
    for (int i = 0; i < 4; i++) { r.in.dt = clk_dt(&c); r.s->def->update(r.s, &r.in, r.rm, &G); }
    float t = 0, tm = 0, tm_prev = 0;
    while (t < 0.4f) {                                                                            /* la mano se mueve 0,4 s con el gatillo apretado */
        float dt = clk_dt(&c); t += dt;
        tm = stale ? floorf(t / 0.0111f) * 0.0111f : t;                                           /* ultimo muestreo de 90 Hz */
        r.in.dt = dt; r.in.hand[0].p = wv_add(p0, wv_scale(dir, v0 * tm + 0.5f * acel * tm * tm));
        r.in.hand_dt = stale ? (tm > tm_prev ? tm - tm_prev : 1e-4f) : 0.0f;                      /* tiempo entre las poses (no entre VBlanks), como da el anfitrion */
        tm_prev = tm;
        r.s->def->update(r.s, &r.in, r.rm, &G);
    }
    int nev0 = M.nev;
    static const float rel[3] = { 0.6f, 0.1f, 0.0f };                                             /* el gatillo se suelta */
    for (int k = 0; k < 3 && M.nev == nev0; k++) {
        float dt = clk_dt(&c); t += dt;
        tm = stale ? floorf(t / 0.0111f) * 0.0111f : t;
        if (k == 0) *true_speed = v0 + acel * (tm - REL_LAG);                                     /* velocidad de la mano al empezar a soltar */
        r.in.trigger[0] = rel[k]; r.in.dt = dt;
        r.in.hand[0].p = wv_add(p0, wv_scale(dir, v0 * tm + 0.5f * acel * tm * tm));
        r.in.hand_dt = stale ? (tm > tm_prev ? tm - tm_prev : 1e-4f) : 0.0f;
        tm_prev = tm;
        r.s->def->update(r.s, &r.in, r.rm, &G);
    }
    int ok = M.nev == nev0 + 1 && M.ev[M.nev - 1].kind == WL_THROW;
    if (ok) *got = M.ev[M.nev - 1].lin_vel;
    free(r.s);
    return ok;
}
static float ballistic_range(WVec3 v) {          /* alcance horizontal (m) desde la altura de la mano hasta el suelo, g = 9,81, mano a 1 m */
    float vh = sqrtf(v.x * v.x + v.z * v.z), a = 0.5f * 9.81f, b = -v.y, c = -1.0f;      /* y(t) = 1 + vy t - a t^2 = 0  ->  a t^2 - vy t - 1 = 0 */
    float disc = b * b - 4 * a * c, t = (-b + sqrtf(disc)) / (2 * a);
    return vh * t;
}

static void test_proportional(void) {
    printf("-- velocidad de salida proporcional a la del mando --\n");
    const WVec3 dir = wv(0, 0.70710678f, -0.70710678f);          /* hacia delante y arriba, 45 grados */
    const float speeds[] = { 0.4f, 1.0f, 2.0f, 4.0f, 6.0f, 9.0f, 13.0f, 18.0f };
    const int ns = (int)(sizeof speeds / sizeof *speeds);
    for (int mode = 0; mode < 3; mode++) {           /* 0: 50 Hz exactos; 1: con fluctuacion y rafagas; 2: ademas pose de 90 Hz con antiguedad */
        float prev_speed = 0, prev_range = 0; int mono = 1;
        for (int i = 0; i < ns; i++) {
            WVec3 got = wv(0, 0, 0); float ts;
            int ok = throw_run(speeds[i], 0, dir, 777u + (uint32_t)i, mode >= 1, mode >= 2, &got, &ts, &w_throw_frag);
            CHECK(ok, "modo %d v=%.1f: lanza al soltar el gatillo", mode, speeds[i]);
            if (!ok) continue;
            float sp = wv_len(got), ratio = sp / speeds[i], rng = ballistic_range(got);
            CHECK(ratio > 0.88f && ratio < 1.12f, "modo %d: velocidad de salida %.2f m/s para una mano a %.2f (x%.2f)", mode, sp, speeds[i], ratio);
            if (!(sp > prev_speed) || !(rng > prev_range)) mono = 0;
            prev_speed = sp; prev_range = rng;
            if (i == 0) CHECK(rng < 0.35f, "modo %d: mano casi quieta (0,4 m/s): cae cerca (%.2f m)", mode, rng);
            if (i == 1) CHECK(rng < 0.9f, "modo %d: suave (1 m/s): cerca (%.2f m)", mode, rng);
            if (i == ns - 3) CHECK(rng > 8.0f, "modo %d: fuerte (9 m/s): lejos (%.2f m)", mode, rng);
            if (i == ns - 1) CHECK(sp <= 22.01f, "modo %d: tope fisico (%.2f)", mode, sp);
        }
        CHECK(mono, "modo %d: a mas velocidad del mando, mas velocidad y mas alcance (estrictamente)", mode);
    }
    /* mismas velocidades con las otras dos arrojadizas (aturdidora, chaff) */
    const WeaponDef *vs[2] = { &w_throw_b, &w_throw_c };
    for (int k = 0; k < 2; k++) {
        WVec3 lo, hi; float ts;
        CHECK(throw_run(2.0f, 0, dir, 5, 1, 1, &lo, &ts, vs[k]) && throw_run(8.0f, 0, dir, 5, 1, 1, &hi, &ts, vs[k]), "variante %d lanza", k);
        CHECK(wv_len(hi) > 3.0f * wv_len(lo), "variante %d: 8 m/s sale mucho mas rapido que 2 (%.2f / %.2f)", k, wv_len(hi), wv_len(lo));
    }
    /* mano acelerando (un lanzamiento real): la salida sigue a la velocidad al soltar y no se queda en 0 */
    float worst = 9, worst_lo = 9, worst_hi = 0;
    for (int trial = 0; trial < 300; trial++) {
        Clk c = { 1000u + (uint32_t)trial, 1, 1 };
        float v0 = 0.5f + 3.0f * clk_rnd(&c), acel = 8.0f + 20.0f * clk_rnd(&c);       /* hasta ~12 m/s a los 0,4 s */
        WVec3 d = wv_norm(wv(clk_rnd(&c) - 0.5f, 0.2f + 0.8f * clk_rnd(&c), -0.2f - clk_rnd(&c)));
        WVec3 got = wv(0, 0, 0); float ts;
        int ok = throw_run(v0, acel, d, 31u * (uint32_t)trial + 3u, 1, 1, &got, &ts, &w_throw_frag);
        CHECK(ok, "prueba %d: lanza", trial);
        if (!ok) continue;
        float ratio = wv_len(got) / ts;               /* ts: velocidad de la mano en el fotograma en que el gatillo empezo a soltarse */
        if (ratio < worst_lo) worst_lo = ratio;
        if (ratio > worst_hi) worst_hi = ratio;
        if (wv_len(got) < worst) worst = wv_len(got);
    }
    printf("  mano acelerando, 300 pruebas con fluctuacion: salida/mano entre x%.2f y x%.2f (salida minima %.2f m/s)\n", worst_lo, worst_hi, worst);
    CHECK(worst_lo > 0.75f && worst_hi < 1.2f, "salida entre x0,75 y x1,2 de la velocidad de la mano (x%.2f..x%.2f)", worst_lo, worst_hi);
    CHECK(worst > 1.5f, "con la mano en movimiento nunca sale con velocidad ~0 (min %.2f m/s)", worst);
}

/* ---------------- colocables ---------------- */
static void test_carga(void) {
    printf("-- carga adhesiva: se coloca al soltar el gatillo --\n");
    mock_reset(4);
    Rig r = rig_new(&w_placed_a, WR_MANUAL);
    CHECK(!any_held(&r), "sin piezas sueltas");
    draw(&r);
    CHECK(r.s->phase == WPL_PH_HELD, "carga en mano");
    r.in.hand[0].p = wv(0.1f, 1.2f, -0.5f); r.in.hand[0].q = wquat_axis_angle(wv(0, 1, 0), 0.7f);
    r.in.trigger[0] = 1.0f; step(&r, 5);
    CHECK(M.nev == 0 && !any_held(&r), "apretar el gatillo no coloca (se coloca al soltarlo)");
    r.in.trigger[0] = 0; M.accept = 0; step(&r, 2);
    CHECK(M.nev == 1 && M.taken == 0 && r.s->phase == WPL_PH_HELD, "juego rechaza (sin superficie al alcance): no se gasta y sigue en la mano");
    r.in.trigger[0] = 0; step(&r, 3);
    CHECK(M.nev == 1, "sin nuevo gatillo no reintenta");
    M.accept = 1; r.in.trigger[0] = 1; step(&r, 3); r.in.trigger[0] = 0; step(&r, 2);
    CHECK(M.nev == 2 && M.ev[1].kind == WL_PLACE_C, "colocada al soltar");
    WLaunch e = M.ev[1];
    WVec3 want = wpose_apply(r.in.hand[0], wv(0, 0, -0.0225f));
    CHECK(wv_len(wv_sub(e.pose.p, want)) < 1e-4f, "pose de la cara adhesiva");
    CHECK(M.taken == 1 && r.s->phase == WPL_PH_PLACED_OUT && !any_held(&r), "municion; sin mando remoto en la otra mano");
    step(&r, 60);
    CHECK(r.s->phase == WPL_PH_HELD, "la siguiente aparece en la mano");
    /* la otra mano: tapa/boton virtuales ya no existen; nada detona ni emite */
    int nev0 = M.nev;
    r.in.buttons[1] = WB_A; r.in.trigger[1] = 1; step(&r, 20); r.in.buttons[1] = 0; r.in.trigger[1] = 0; step(&r, 20);
    int det = 0; for (int i = 0; i < M.nev; i++) det += M.ev[i].kind == WL_DETONATE;
    CHECK(M.nev == nev0 && det == 0 && !any_held(&r), "A/B/gatillo de la otra mano no hacen nada (el detonador es el boton del juego)");
    free(r.s);

    /* sacar con el gatillo apretado no prepara */
    mock_reset(4);
    r = rig_new(&w_placed_a, WR_MANUAL);
    r.in.trigger[0] = 1; draw(&r); step(&r, 5);
    r.in.trigger[0] = 0; step(&r, 3);
    CHECK(M.nev == 0, "gatillo apretado al sacar: al soltar no coloca");
    r.in.trigger[0] = 1; step(&r, 3); r.in.trigger[0] = 0; step(&r, 3);
    CHECK(M.nev == 1 && M.taken == 1, "tras pasar por reposo si coloca");
    free(r.s);
}

static void test_mina(void) {
    printf("-- mina direccional --\n");
    mock_reset(2);
    Rig r = rig_new(&w_placed_b, WR_MANUAL);
    draw(&r);
    CHECK(r.s->channel[WCH_CUSTOM0] < 0.5f, "patas plegadas al sacarla");
    r.in.hand[0].q = wquat_axis_angle(wv(0, 1, 0), -1.1f); r.in.trigger[0] = 1; step(&r, 3);
    r.in.trigger[0] = 0; step(&r, 1);
    CHECK(M.nev == 0, "no se prepara con las patas a medio desplegar");
    step(&r, 40);
    CHECK(r.s->channel[WCH_CUSTOM0] > 0.95f, "patas desplegadas");
    r.in.trigger[0] = 1; step(&r, 3);
    CHECK(M.nev == 0, "apretar no coloca");
    r.in.trigger[0] = 0; step(&r, 2);
    CHECK(M.nev == 1 && M.ev[0].kind == WL_PLACE_MINE, "colocada al soltar");
    if (M.nev) {
        WQuat a = M.ev[0].pose.q, b = r.in.hand[0].q;
        float dot = fabsf(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
        CHECK(dot > 0.9999f, "orientacion igual a la de la mano");
        WVec3 f = wquat_rotate(a, wv(0, 0, -1));
        CHECK(fabsf(f.x - (-sinf(-1.1f))) < 1e-3f, "frente de la mina = -Z local girado");
    }
    CHECK(M.taken == 1 && !any_held(&r), "municion");
    free(r.s);
}

/* ---------------- robustez: dt ---------------- */
static void test_robust(void) {
    printf("-- robustez --\n");
    /* dt invalido: no avanza nada y no rompe */
    mock_reset(3);
    Rig r = rig_new(&w_throw_frag, WR_MANUAL);
    const float dts[] = {0.0f, -1.0f, NAN, INFINITY, 9.0f};
    for (unsigned k = 0; k < sizeof dts / sizeof *dts; k++) { r.in.dt = dts[k]; step(&r, 5); }
    CHECK(isfinite(r.s->channel[WCH_CUSTOM1]) && isfinite(r.s->recoil_offset.p.y), "estado finito");
    free(r.s);
    mock_reset(3);
    Rig rp = rig_new(&w_placed_a, WR_MANUAL);
    rp.in.dt = 0.2f; draw(&rp); rp.in.hand[0].p = wv(0.1f, 1.2f, -0.5f); rp.in.trigger[0] = 0; step(&rp, 5);
    rp.in.dt = NAN; step(&rp, 3); rp.in.dt = 0; step(&rp, 3);
    CHECK(isfinite(rp.s->channel[WCH_CUSTOM1]) && isfinite(rp.s->recoil_offset.p.y), "placed: estado finito");
    free(rp.s);
    /* reserva nunca negativa: el puente devuelve -5 */
    mock_reset(-5);
    r = rig_new(&w_throw_frag, WR_MANUAL);
    draw(&r);
    CHECK(r.s->phase == WT_PH_VEST && r.s->mag_rounds == 0 && M.taken == 0, "reserva negativa tratada como 0");
    free(r.s);
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
    test_throw_slow_release();
    test_throw_slow_release_direction();
    test_gentle();
    test_proportional();
    test_carga();
    test_mina();
    test_defs();
    test_robust();
    test_ids();
    printf("\n%d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
