/* Pruebas de arrojadizas y colocables (headless): estimación de velocidad de lanzamiento con trayectoria
 * sintética, lanzamiento con el gatillo (apretar = preparada, soltar = lanzar con la velocidad de la mano; el agarre no interviene; sin anilla ni pasador sueltos), soltar suave,
 * colocación al soltar el gatillo (sin tapa ni botón virtuales) y sanidad de mallas.
 * Compilar: gcc -std=gnu11 -I. test_throw.c w_throw.c w_placed.c wcore.c -lm */
#include "w_laser.h"
#include "w_throw.h"
#include "wcore.h"
#include "wcut.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------- puente simulado ---------------- */
typedef struct { int ammo; int taken, fires, sounds_dry, sounds_empty, sounds_pin, sounds_lever, sounds_throw; WLaunch ev[16]; int nev; int accept; int haps, probe; } Mock;
static Mock M;
static int b_reserve(int id) { (void)id; return M.ammo; }
static void b_take(int id, int n) { (void)id; M.taken += n; M.ammo -= n; }
static void b_fire(int id, WPose p) { (void)id; (void)p; M.fires++; }
static void b_haptic(int h, float a, float s) { (void)h; (void)a; (void)s; M.haps++; }
static int probe_hook(void *u, const WLaunch *e) { (void)u; (void)e; return M.probe; }
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
    if (M.nev) { printf("  gatillo lento: %.2f m/s (pico %.1f)\n", (double)wv_len(M.ev[0].lin_vel), (double)vpk); CHECK(M.ev[0].v_gest >= 0.85f * vpk, "gatillo lento: el gesto medido %.2f < 85 %% del pico", (double)M.ev[0].v_gest); CHECK(fabsf(wv_len(M.ev[0].lin_vel) - 4.93f) < 0.01f && M.ev[0].thr_mode == WLM_MAX, "gatillo lento: sale al maximo del juego (%.2f m/s, modo %d)", (double)wv_len(M.ev[0].lin_vel), M.ev[0].thr_mode); }
    if (M.nev) {      /* diagnóstico del log [lanzar]: la ventana más rápida recorrida y la velocidad entre las dos últimas poses */
        CHECK(M.ev[0].v_peak >= M.ev[0].v_used - 1e-3f && M.ev[0].v_peak > 0.85f * vpk, "diagnóstico: v_peak %.2f (pico %.1f)", (double)M.ev[0].v_peak, (double)vpk);
        CHECK(M.ev[0].v_inst >= 0.0f && M.ev[0].v_inst < 0.5f, "diagnóstico: v_inst %.2f (la mano ya paró)", (double)M.ev[0].v_inst);
    }
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

/* Soltar muy lento (gatillo a medias 0,6 s, más que el historial de ventanas): las ventanas candidatas que caen fuera del historial se descartan, no devuelven la velocidad de las dos últimas muestras (aquí un barrido rápido DESPUES de empezar a soltar) */
static void test_throw_window_outside_history(void) {
    printf("-- arrojadiza: ventana candidata fuera del historial --\n");
    mock_reset(3);
    Rig r = rig_new(&w_throw_frag, WR_MANUAL);
    const float dt = 0.02f;
    r.in.dt = dt;
    float z = 0, t = 0;
    r.in.trigger[0] = 0; step(&r, 3);
    r.in.trigger[0] = 1; step(&r, 3);
    for (int i = 0; i < 80 && M.nev == 0; i++) {
        t += dt;
        z -= (t < 0.64f ? 0.4f : 8.0f) * dt;                       /* hacia delante (-Z): lento, y rápido solo en las 4 últimas muestras */
        r.in.hand[0].p = wv(0.25f, 1.0f, -0.3f + z);
        r.in.trigger[0] = t < 0.10f ? 1.0f : (t < 0.10f + 0.60f ? 0.6f : 0.1f);      /* empieza a soltar en 0,1 s y acaba 0,6 s después */
        step(&r, 1);
    }
    CHECK(M.nev == 1 && M.ev[0].kind == WL_THROW, "soltar a medias 0,6 s: lanza");
    if (M.nev) { printf("  velocidad %.2f m/s\n", (double)wv_len(M.ev[0].lin_vel)); CHECK(wv_len(M.ev[0].lin_vel) < 1.5f, "el barrido posterior al soltar no cuenta: velocidad %.2f m/s", (double)wv_len(M.ev[0].lin_vel)); }
    free(r.s);
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
        float err = fabsf(e->v_gest - wv_len(tv)) / wv_len(tv);      /* la estimación del gesto (la salida es el módulo fijo del juego, en la dirección del gesto) */
        printf("  v_gesto %.2f  salida %.2f %.2f %.2f  real %.2f %.2f %.2f  (%.1f %%)\n", e->v_gest, e->lin_vel.x, e->lin_vel.y, e->lin_vel.z, tv.x, tv.y, tv.z, err * 100);
        CHECK(err < 0.15f, "error velocidad lineal %.3f", err);
        CHECK(e->thr_mode == WLM_MAX && fabsf(wv_len(e->lin_vel) - 4.93f) < 0.01f, "lanzamiento al maximo del juego (%.2f m/s, modo %d)", wv_len(e->lin_vel), e->thr_mode);
        CHECK(e->lin_vel.z * tv.z > 0 && e->elev_deg >= 19.9f && e->elev_deg <= 60.1f, "sale hacia donde va la mano (z %.2f frente a %.2f), elevación %.1f", e->lin_vel.z, tv.z, e->elev_deg);
        CHECK(wv_len(wv_sub(e->ang_vel, tw)) / wv_len(tw) < 0.3f, "velocidad angular");
        CHECK(fabsf(e->fuse_left - 3.0f) < 0.01f, "espoleta %.2f", e->fuse_left);
        CHECK(e->cooked == 0, "sin cocinar");
        CHECK(wv_len(e->lin_vel) <= 4.93f + 0.01f, "tope de velocidad (el máximo de su tipo)");
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
        CHECK(wv_len(M.ev[0].lin_vel) <= 4.94f, "lin_vel acotada: %f", wv_len(M.ev[0].lin_vel));
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
static int tr_orient;                      /* 1: throw_run usa tr_head_q / tr_hand_q en vez de la identidad (mirada y mano) */
static WQuat tr_head_q, tr_hand_q;
static int throw_run(float v0, float acel, WVec3 dir, uint32_t seed, int jitter, int stale, WVec3 *got, float *true_speed, const WeaponDef *def) {
    mock_reset(3);
    Rig r = rig_new(def, WR_MANUAL);
    if (tr_orient) { r.in.head.q = tr_head_q; r.in.hand[0].q = tr_hand_q; }
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

/* Todo lanzamiento (gesto >= 1 m/s) sale SIEMPRE con el módulo máximo de su tipo (granada 4,93, aturdidora 5,154, chaff 1,131 m/s: weapon/grenade.c dword_8009F3EC), sin excepciones; por debajo es soltar suave con la velocidad medida. */
#define V_FIX 4.93f
#define V_STUN 5.154f
#define V_CHAFF 1.131f
static float heading_deg(WVec3 a, WVec3 b) {      /* ángulo horizontal entre dos vectores */
    float la = sqrtf(a.x * a.x + a.z * a.z), lb = sqrtf(b.x * b.x + b.z * b.z);
    if (la < 1e-6f || lb < 1e-6f) return 180.0f;
    return acosf(wcore_clampf((a.x * b.x + a.z * b.z) / (la * lb), -1, 1)) * 57.29578f;
}
static void test_proportional(void) {
    printf("-- todo lanzamiento sale al maximo del juego, en la direccion del gesto --\n");
    const WVec3 dir = wv(0, 0.70710678f, -0.70710678f);          /* hacia delante y arriba, 45 grados */
    const float speeds[] = { 0.4f, 1.0f, 2.0f, 4.0f, 6.0f, 9.0f, 13.0f, 18.0f };
    const int ns = (int)(sizeof speeds / sizeof *speeds);
    for (int mode = 0; mode < 3; mode++) {           /* 0: 50 Hz exactos; 1: con fluctuacion y rafagas; 2: ademas pose de 90 Hz con antiguedad */
        float rng_ref = -1;
        for (int i = 0; i < ns; i++) {
            WVec3 got = wv(0, 0, 0); float ts;
            int ok = throw_run(speeds[i], 0, dir, 777u + (uint32_t)i, mode >= 1, mode >= 2, &got, &ts, &w_throw_frag);
            CHECK(ok, "modo %d v=%.1f: lanza al soltar el gatillo", mode, speeds[i]);
            if (!ok) continue;
            const WLaunch *e = &M.ev[M.nev - 1];
            float sp = wv_len(got), rng = ballistic_range(got);
            if (i == 0) { CHECK(e->thr_mode == WLM_SOFT && sp < 1.0f && rng < 0.35f, "modo %d: mano casi quieta (0,4 m/s): soltar suave y cae cerca (%.2f m/s, %.2f m, modo %d)", mode, sp, rng, e->thr_mode); continue; }
            if (i == 1) { CHECK((e->thr_mode == WLM_SOFT && sp < 1.0f) || (e->thr_mode == WLM_MAX && fabsf(sp - V_FIX) < 0.01f), "modo %d: 1 m/s: suave o maximo (%.2f m/s, modo %d)", mode, sp, e->thr_mode); continue; }
            CHECK(e->thr_mode == WLM_MAX && fabsf(sp - V_FIX) < 0.01f, "modo %d: mano a %.2f m/s: sale al maximo del juego (%.3f m/s, modo %d)", mode, speeds[i], sp, e->thr_mode);
            CHECK(sp <= V_FIX + 0.01f, "modo %d: tope fisico (%.2f)", mode, sp);
            CHECK(heading_deg(got, dir) < 12.0f, "modo %d: va hacia donde la mano (rumbo a %.1f grados)", mode, heading_deg(got, dir));
            CHECK(e->elev_deg > 19.9f && e->elev_deg < 60.1f && fabsf(atan2f(got.y, sqrtf(got.x * got.x + got.z * got.z)) * 57.29578f - e->elev_deg) < 0.1f, "modo %d: elevacion %.1f", mode, e->elev_deg);
            CHECK(rng > 2.5f, "modo %d: llega lejos (%.2f m)", mode, rng);
            if (rng_ref < 0) rng_ref = rng;
            CHECK(fabsf(rng - rng_ref) < 0.15f, "modo %d: el alcance no depende del gesto (%.2f frente a %.2f m)", mode, rng, rng_ref);
        }
    }
    /* las otras dos arrojadizas salen al maximo de SU tipo: aturdidora 5,154 m/s, chaff 1,131 m/s (no 4,93 para todas) */
    const WeaponDef *vs[2] = { &w_throw_b, &w_throw_c };
    const float vmax_k[2] = { V_STUN, V_CHAFF };
    for (int k = 0; k < 2; k++) {
        WVec3 lo, hi; float ts;
        CHECK(throw_run(2.0f, 0, dir, 5, 1, 1, &lo, &ts, vs[k]) && throw_run(8.0f, 0, dir, 5, 1, 1, &hi, &ts, vs[k]), "variante %d lanza", k);
        CHECK(fabsf(wv_len(hi) - vmax_k[k]) < 0.005f && fabsf(wv_len(lo) - vmax_k[k]) < 0.005f, "variante %d: 2 y 8 m/s salen a su maximo %.3f (%.3f / %.3f)", k, vmax_k[k], wv_len(lo), wv_len(hi));
        WVec3 dn, bk;      /* gesto hacia abajo (60 grados) y hacia atras, a 9 m/s: tambien al maximo de su tipo y hacia delante */
        CHECK(throw_run(9.0f, 0, wv_norm(wv(0, -1.732f, -1)), 6, 1, 1, &dn, &ts, vs[k]) && throw_run(9.0f, 0, wv(0, 0, 1), 7, 1, 1, &bk, &ts, vs[k]), "variante %d: gestos abajo y atras lanzan", k);
        CHECK(fabsf(wv_len(dn) - vmax_k[k]) < 0.005f && fabsf(wv_len(bk) - vmax_k[k]) < 0.005f && dn.y > 0 && dn.z < 0 && bk.y > 0 && bk.z < 0, "variante %d: abajo/atras a 9 m/s salen a %.3f hacia delante y arriba (%.3f / %.3f)", k, vmax_k[k], wv_len(dn), wv_len(bk));
    }
    /* direcciones: plano = elevacion minima; casi vertical = frente de la mano; lado; abajo y atras tambien salen al maximo (hacia delante) */
    struct { const char *n; WVec3 d; float v; int mode; float el_lo, el_hi; } cs[] = {
        { "horizontal hacia delante", wv(0, 0, -1), 3.0f, WLM_MAX, 19.9f, 20.1f },
        { "hacia la derecha", wv(1, 0, 0), 3.0f, WLM_MAX, 19.9f, 20.1f },
        { "hacia arriba y delante (80 grados)", wv_norm(wv(0, 5.67f, -1)), 3.0f, WLM_MAX, 59.9f, 60.1f },
        { "casi vertical (frente de la mano)", wv(0, 1, 0), 3.0f, WLM_MAX, 59.9f, 60.1f },
        { "hacia delante y abajo suave (20 grados)", wv_norm(wv(0, -0.364f, -1)), 3.0f, WLM_MAX, 19.9f, 20.1f },
        { "hacia abajo (60 grados)", wv_norm(wv(0, -1.732f, -1)), 3.0f, WLM_MAX, 19.9f, 20.1f },
        { "hacia atras", wv(0, 0, 1), 3.0f, WLM_MAX, 19.9f, 20.1f },
        { "hacia atras y arriba", wv_norm(wv(0, 0.5f, 1)), 3.0f, WLM_MAX, 26.0f, 27.1f },
        { "hacia abajo (60 grados) a 9 m/s", wv_norm(wv(0, -1.732f, -1)), 9.0f, WLM_MAX, 19.9f, 20.1f },
        { "hacia atras a 9 m/s", wv(0, 0, 1), 9.0f, WLM_MAX, 19.9f, 20.1f },
        { "hacia abajo vertical a 9 m/s", wv(0, -1, 0), 9.0f, WLM_MAX, 19.9f, 20.1f },
        { "hacia atras y abajo a 12 m/s", wv_norm(wv(0, -1, 1)), 12.0f, WLM_MAX, 19.9f, 20.1f },
    };
    for (size_t i = 0; i < sizeof cs / sizeof *cs; i++) {
        WVec3 got = wv(0, 0, 0); float ts;
        int ok = throw_run(cs[i].v, 0, cs[i].d, 900u + (uint32_t)i, 0, 0, &got, &ts, &w_throw_frag);
        CHECK(ok, "%s: lanza", cs[i].n);
        if (!ok) continue;
        const WLaunch *e = &M.ev[M.nev - 1];
        float sp = wv_len(got);
        CHECK(e->thr_mode == cs[i].mode, "%s: modo %d (esperado %d)", cs[i].n, e->thr_mode, cs[i].mode);
        CHECK(fabsf(sp - V_FIX) < 0.01f && e->elev_deg >= cs[i].el_lo && e->elev_deg <= cs[i].el_hi, "%s: %.3f m/s, elevacion %.1f", cs[i].n, sp, e->elev_deg);
        if (i == 1) CHECK(got.x > 4.0f && fabsf(got.z) < 0.2f, "%s: sale hacia +X (%.2f %.2f %.2f)", cs[i].n, got.x, got.y, got.z);
        if (i == 3) CHECK(got.z < -2.0f, "%s: sale hacia el frente de la mano, no hacia arriba (%.2f %.2f %.2f)", cs[i].n, got.x, got.y, got.z);
        if (i == 5 || i == 8) CHECK(got.y > 1.0f && got.z < -1.0f, "%s: sale hacia delante y arriba, no hacia abajo (%.2f %.2f %.2f)", cs[i].n, got.x, got.y, got.z);
        if (i == 6 || i == 7 || i == 9 || i == 10 || i == 11) CHECK(got.z < -1.0f && got.y > 1.0f, "%s: el frente de la mano, nunca hacia atras (%.2f %.2f %.2f)", cs[i].n, got.x, got.y, got.z);
    }
    {   /* rumbo segun mirada y mano: gesto a 90 grados de la mirada = valido; gesto y mano hacia atras = la mirada */
        WVec3 got; float ts;
        tr_orient = 1;
        tr_head_q = wquat_axis_angle(wv(0, 1, 0), 1.5707963f);       /* mira hacia -X */
        tr_hand_q = wquat_axis_angle(wv(0, 1, 0), 3.14159265f);      /* la mano apunta hacia +Z */
        int ok = throw_run(9.0f, 0, wv(0, 0, 1), 950u, 0, 0, &got, &ts, &w_throw_frag);      /* gesto hacia +Z, mirada hacia -X */
        tr_orient = 0;
        CHECK(ok && fabsf(wv_len(got) - V_FIX) < 0.01f && got.z > 4.0f, "gesto a 90 grados de la mirada (es hacia delante, <= 120): rumbo del gesto, maximo (%.3f %.3f %.3f)", got.x, got.y, got.z);
        tr_orient = 1;
        tr_head_q = wquat_axis_angle(wv(0, 1, 0), 0.0f);            /* mira hacia -Z */
        ok = throw_run(9.0f, 0, wv(0, 0, 1), 951u, 0, 0, &got, &ts, &w_throw_frag);          /* gesto hacia +Z (180 grados de la mirada), mano hacia +Z: sale hacia la mirada (-Z) */
        tr_orient = 0;
        CHECK(ok && fabsf(wv_len(got) - V_FIX) < 0.01f && got.z < -1.0f, "gesto y mano hacia atras: sale hacia la mirada, al maximo (%.3f %.3f %.3f)", got.x, got.y, got.z);
    }
    /* mano acelerando (un lanzamiento real): sale siempre al maximo, hacia donde va la mano */
    float worst_h = 0, worst_sp = 0;
    for (int trial = 0; trial < 300; trial++) {
        Clk c = { 1000u + (uint32_t)trial, 1, 1 };
        float v0 = 0.5f + 3.0f * clk_rnd(&c), acel = 8.0f + 20.0f * clk_rnd(&c);       /* hasta ~12 m/s a los 0,4 s */
        WVec3 d = wv_norm(wv(clk_rnd(&c) - 0.5f, 0.2f + 0.8f * clk_rnd(&c), -0.2f - clk_rnd(&c)));
        WVec3 got = wv(0, 0, 0); float ts;
        int ok = throw_run(v0, acel, d, 31u * (uint32_t)trial + 3u, 1, 1, &got, &ts, &w_throw_frag);
        CHECK(ok, "prueba %d: lanza", trial);
        if (!ok) continue;
        float h = heading_deg(got, d), e = fabsf(wv_len(got) - V_FIX);
        if (h > worst_h) worst_h = h;
        if (e > worst_sp) worst_sp = e;
    }
    printf("  mano acelerando, 300 pruebas con fluctuacion: rumbo de salida a %.1f grados como mucho de la mano; modulo a %.3f m/s del maximo como mucho\n", worst_h, worst_sp);
    CHECK(worst_sp < 0.01f, "siempre al maximo (peor desviacion %.3f m/s)", worst_sp);
    CHECK(worst_h < 15.0f, "siempre hacia donde va la mano (peor rumbo %.1f grados)", worst_h);
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
        WQuat a = M.ev[0].pose.q, b = wquat_mul(r.in.hand[0].q, wquat_axis_angle(wv(0, 1, 0), -1.5707963f));   /* modelo en la mano (WP_GRIP sin giro) girado R_y(-90): -Z = cara picuda (+X del modelo) */
        float dot = fabsf(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
        CHECK(dot > 0.9999f, "orientacion = la del modelo con -Z en la cara picuda");
        WVec3 f = wquat_rotate(a, wv(0, 0, -1)), fm = wquat_rotate(r.in.hand[0].q, wv(1, 0, 0));
        CHECK(fabsf(f.x - fm.x) < 1e-3f && fabsf(f.z - fm.z) < 1e-3f, "frente del evento = +X del modelo girado (cara picuda)");
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

/* Pulso haptico al detectar donde colocar (carga y mina): una sola vez por entrada (del sondeo no valido a valido), con histeresis: parpadeos cortos del sondeo no repiten el pulso. */
static void probe_run(const WeaponDef *d, const char *nm) {
    mock_reset(4); w_throw_set_probe(probe_hook, NULL);
    Rig r = rig_new(d, WR_MANUAL);
    draw(&r); step(&r, 50);                         /* mina: patas desplegadas */
    int h0 = M.haps;
    M.probe = 0; step(&r, 90);
    CHECK(M.haps == h0, "%s: sin superficie no vibra (%d)", nm, M.haps - h0);
    M.probe = 1; step(&r, 90);
    CHECK(M.haps == h0 + 1, "%s: al entrar en una superficie valida vibra una vez (%d)", nm, M.haps - h0);
    int h1 = M.haps;
    for (int k = 0; k < 40; k++) { M.probe = (k % 6) < 2 ? 0 : 1; step(&r, 1); }      /* parpadeo corto del sondeo (0,02 s): no repite */
    M.probe = 1; step(&r, 90);
    CHECK(M.haps == h1, "%s: el parpadeo del sondeo no repite el pulso (%d)", nm, M.haps - h1);
    M.probe = 0; step(&r, 60); M.probe = 1; step(&r, 90);
    CHECK(M.haps == h1 + 1, "%s: al salir y volver a entrar vibra otra vez (%d)", nm, M.haps - h1);
    M.probe = 0; step(&r, 90);
    h1 = M.haps;
    r.in.trigger[0] = 1; step(&r, 5); M.probe = 1; r.in.trigger[0] = 0; step(&r, 90);    /* colocada: la siguiente aparece sin pulso de superficie hasta que haya sondeo valido */
    CHECK(M.haps >= h1, "%s: colocar no rompe el contador", nm);
    free(r.s);
    w_throw_set_probe(NULL, NULL);
}
static void test_probe_haptic(void) {
    printf("-- pulso haptico al detectar superficie --\n");
    probe_run(&w_placed_a, "carga");
    probe_run(&w_placed_b, "mina");
    mock_reset(4);
    Rig r = rig_new(&w_placed_a, WR_MANUAL);
    draw(&r); step(&r, 20);
    int h0 = M.haps; step(&r, 90);
    CHECK(M.haps == h0, "sin gancho de sondeo no vibra");
    free(r.s);
}

/* Claymore en la mano: el punto de empuñadura (el que va al centro de agarre de la mano) cae dentro del cuerpo del modelo (marco del arma, m: la caja de w_laser.h WGHOST_MINE_*: x -0,080..0,031, y -0,224..0,100, z -0,370..-0,045), no en su origen
 * (detras de la cara trasera: el bloque flotaba 21 cm por delante de la mano). */
static void test_mina_agarre(void) {
    printf("-- mina: punto de empunadura dentro del cuerpo --\n");
    WVec3 g = w_placed_b.points[WP_GRIP].p;
    CHECK(g.x > WGHOST_MINE_X0 && g.x < WGHOST_MINE_X1 && g.y > -WGHOST_MINE_LEGS_U / 1000.0f && g.y < WGHOST_MINE_UP_M && g.z > WGHOST_MINE_Z0 && g.z < WGHOST_MINE_Z1, "empunadura de la mina (%.3f %.3f %.3f) dentro del cuerpo", g.x, g.y, g.z);
    /* carga adhesiva (0xf83d): con la empunadura en el origen del marco (fuera del bloque: caja z -156..-56 mm) la mano cogía el aire y el modelo quedaba 6-16 cm por delante; el punto debe caer dentro de la caja del modelo,
     * y el punto de colocación (cara adhesiva) seguir en el mismo sitio respecto a la mano que antes de mover la empunadura (WL_PLACE_C no cambia) */
    const WCutModel *cm = wcut_find(0xf83d);
    WVec3 c = w_placed_a.points[WP_GRIP].p;
    CHECK(cm && c.x > cm->bounds.lo.x && c.x < cm->bounds.hi.x && c.y > cm->bounds.lo.y && c.y < cm->bounds.hi.y && c.z > cm->bounds.lo.z && c.z < cm->bounds.hi.z, "empunadura de la carga (%.3f %.3f %.3f) dentro del bloque", c.x, c.y, c.z);
    mock_reset(2);
    Rig r = rig_new(&w_placed_a, WR_MANUAL);
    draw(&r);
    r.in.hand[0].p = wv(0.1f, 1.2f, -0.5f); r.in.hand[0].q = wquat_axis_angle(wv(0, 1, 0), 0.7f); r.in.trigger[0] = 0; step(&r, 3);
    r.in.trigger[0] = 1; step(&r, 3);
    r.in.trigger[0] = 0; step(&r, 2);
    CHECK(M.nev == 1 && M.ev[0].kind == WL_PLACE_C, "carga colocada");
    if (M.nev) {
        WVec3 e = wv_add(r.in.hand[0].p, wquat_rotate(r.in.hand[0].q, wv(0, 0, -0.0225f)));
        CHECK(wv_len(wv_sub(M.ev[0].pose.p, e)) < 1e-4f, "la cara adhesiva sigue 22,5 mm por delante de la mano (%.4f %.4f %.4f)", M.ev[0].pose.p.x - e.x, M.ev[0].pose.p.y - e.y, M.ev[0].pose.p.z - e.z);
    }
    free(r.s);
}

int main(void) {
    test_mina_agarre();
    test_probe_haptic();
    test_estimator();
    test_throw_flow();
    test_throw_slow_release();
    test_throw_slow_release_direction();
    test_throw_window_outside_history();
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
