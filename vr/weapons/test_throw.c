/* Pruebas de arrojadizas y colocables (headless): estimación de velocidad de lanzamiento con trayectoria
 * sintética, lanzamiento por gesto (cerrar la mano = listo, abrirla = lanzar con la velocidad de la mano; sin anilla ni pasador sueltos), soltar suave,
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
/* abrir la mano con el objeto y cerrarla: lo deja listo */
static void ready_it(Rig *r) {
    r->in.grip[0] = 0; step(r, 2);
    r->in.grip[0] = 1; step(r, 2);
}

/* la mano sigue la trayectoria con el puño cerrado y se abre al final (la suelta se detecta 2 fotogramas después) */
static void do_throw(Rig *r, float t_det, WVec3 *tv, WVec3 *tw) {
    float dt = r->in.dt; int n = (int)(t_det / dt);
    r->in.grip[0] = 1;
    for (int i = 0; i <= n; i++) { r->in.hand[0] = traj(i * dt); step(r, 1); }
    *tv = traj_v((n + 2) * dt - 0.025f); *tw = traj_w((n + 2) * dt - 0.025f);
    r->in.grip[0] = 0.6f; r->in.hand[0] = traj((n + 1) * dt); step(r, 1);   /* los dedos se abren */
    r->in.grip[0] = 0.1f; r->in.hand[0] = traj((n + 2) * dt); step(r, 1);
}

static void test_throw_flow(void) {
    printf("-- arrojadiza: flujo por gesto --\n");
    mock_reset(3);
    Rig r = rig_new(&w_throw_frag, WR_MANUAL);
    CHECK(r.s->phase == WT_PH_VEST, "empieza sin objeto");
    draw(&r);
    CHECK(r.s->phase == WT_PH_HELD, "aparece en la mano al equiparla, sin gesto del chaleco");
    CHECK(r.s->phase != WT_PH_READY && !any_held(&r), "con el puño cerrado al sacarla no queda lista; sin piezas sueltas");
    r.in.hand[0].p = wv(0.25f, 1.1f, -0.35f); step(&r, 3);
    r.in.grip[0] = 0.0f; step(&r, 3);
    CHECK(r.s->phase == WT_PH_HELD && M.nev == 0 && M.taken == 0, "soltar sin haberla preparado: sigue en la mano, sin gastar");
    r.in.grip[0] = 0.5f; step(&r, 2);
    CHECK(r.s->phase == WT_PH_HELD, "grip intermedio (histeresis): no se prepara");
    r.in.grip[0] = 1.0f; step(&r, 2);
    CHECK(r.s->phase == WT_PH_READY, "mano cerrada tras haberla abierto: lista (el anfitrion pulsa el boton del juego)");
    CHECK(M.sounds_pin == 1 && !any_held(&r), "sonido de pasador y ninguna pieza suelta flotando");
    r.in.grip[0] = 0.5f; step(&r, 5);
    CHECK(r.s->phase == WT_PH_READY && M.nev == 0, "grip intermedio: no lanza");
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
}

static void test_gentle(void) {
    printf("-- soltar suave y rechazos --\n");
    mock_reset(3);
    Rig r = rig_new(&w_throw_frag, WR_MANUAL);
    draw(&r); ready_it(&r);
    CHECK(r.s->phase == WT_PH_READY, "lista");
    r.in.hand[0].p = wv(0.25f, 1.1f, -0.35f); step(&r, 10);
    r.in.grip[0] = 0.0f; step(&r, 1);
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
    r.in.grip[0] = 0.0f; step(&r, 1);
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
        draw(&r); ready_it(&r); step(&r, 3); r.in.grip[0] = 0; step(&r, 1);
        CHECK(M.nev == 1 && fabsf(M.ev[0].fuse_left - fz[k]) < 0.01f && M.ev[0].weapon_id == vs[k]->game_weapon_id, "variante %d: espoleta y id", k);
        free(r.s);
    }
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
    test_gentle();
    test_carga();
    test_mina();
    test_defs();
    test_robust();
    test_ids();
    printf("\n%d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
