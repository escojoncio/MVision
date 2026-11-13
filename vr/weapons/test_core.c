/* Pruebas headless del núcleo y de la pistola ("pist"). Salida 0 = todo correcto.
 * La pistola no tiene recarga VR: sus balas las iguala el anfitrión al cargador del juego con make_ready (aquí lo hace settle). */
#include "wcore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int fails = 0, evs[64], nev = 0, fires = 0, reserve = 40, dry = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static int b_res(int id) { (void)id; return reserve; }
static void b_take(int id, int n) { (void)id; reserve -= n; }
static WPose last_mz;
static void b_fire(int id, WPose m) { (void)id; last_mz = m; fires++; }
static void b_hap(int h, float s, float t) { (void)h; (void)s; (void)t; }
static void b_snd(int id, int e) { (void)id; if (nev < 64) evs[nev++] = e; if (e == WS_DRY) dry++; }
static WGameBridge BR = {.ammo_reserve = b_res, .ammo_take = b_take, .fire = b_fire, .haptic = b_hap, .sound = b_snd};

static const WeaponDef *find(const char *id) {
    for (int i = 0; i < w_registry_count; i++) if (w_registry[i] && !strcmp(w_registry[i]->id, id)) return w_registry[i];
    return NULL;
}
static int count_ev(int e) { int n = 0; for (int i = 0; i < nev; i++) n += evs[i] == e; return n; }

static void test_math(void) {
    WQuat q = wquat_axis_angle(wv(0, 1, 0), 1.5707963f);
    WVec3 v = wquat_rotate(q, wv(1, 0, 0));
    CHECK(fabsf(v.x) < 1e-5f && fabsf(v.z + 1) < 1e-5f, "rotar X 90 grados sobre Y: %f %f", v.x, v.z);
    WPose a = {{1, 2, 3}, q}, r = wpose_mul(a, wpose_inv(a));
    CHECK(wv_len(r.p) < 1e-5f && fabsf(fabsf(r.q.w) - 1) < 1e-5f, "pose*inv = identidad");
    WVec3 rv = wquat_to_rotvec(wquat_from_rotvec(wv(0.3f, -0.2f, 0.5f)));
    CHECK(fabsf(rv.x - 0.3f) < 1e-5f && fabsf(rv.z - 0.5f) < 1e-5f, "rotvec ida y vuelta");
}

static void setup(WeaponState **sp, const WeaponDef *d, WInput *in) {
    *sp = calloc(1, d->state_size); (*sp)->def = d; d->init(*sp);
    memset(in, 0, sizeof *in);
    in->head.q = in->hand[0].q = in->hand[1].q = wquat_identity();
    in->head.p = wv(0, 1.6f, 0); in->hand[0].p = wv(0, 1.3f, -0.4f);
    in->hand[1].p = wv(0.4f, 1.0f, -0.1f);
    in->dt = 1.0f / 120;
    nev = 0; fires = 0; reserve = 40; dry = 0;
}
/* Deja el arma lista como el anfitrión (cargador completo + bala en la recámara) y espera a ver el gatillo suelto. */
static void settle(const WeaponDef *d, WeaponState *s, WInput *in, WReloadMode rm) {
    d->make_ready(s, d->mag_capacity);
    in->trigger[0] = 0; for (int i = 0; i < 3; i++) d->update(s, in, rm, &BR);
}
static void run(const WeaponDef *d, WeaponState *s, WInput *in, WReloadMode rm, float secs) {
    for (int i = 0; i < (int)(secs * 120); i++) d->update(s, in, rm, &BR);
}

static void test_recoil(const WeaponDef *d) {
    WeaponState *s; WInput in; setup(&s, d, &in);
    wcore_kick(s, &in, 1);
    float peak = 0, ang = 0; int osc = 0; float prev = 0, last_z = 0;
    for (int i = 0; i < 240; i++) {
        wcore_recoil_step(s, &in, 1.0f / 120);
        float z = s->recoil_offset.p.z; if (z > peak) peak = z;
        float a = wv_len(wquat_to_rotvec(s->recoil_offset.q)); if (a > ang) ang = a;
        if (i > 5 && (z - prev) * (prev - last_z) < 0 && fabsf(z) > 1e-4f) osc++;
        last_z = prev; prev = z;
    }
    CHECK(peak > 0.005f && peak < 0.08f, "retroceso lineal %f m fuera de rango", peak);
    CHECK(ang > 0.03f && ang < 0.30f, "cabeceo %f rad fuera de rango", ang);
    CHECK(wv_len(s->recoil_offset.p) < 1e-3f && wv_len(s->recoil_vel) < 0.01f, "no vuelve al reposo");
    CHECK(osc <= 2, "oscila demasiado (%d)", osc);
    /* dos manos reduce */
    WeaponState *s2; WInput in2; setup(&s2, d, &in2); in2.support_held = 1;
    wcore_kick(s2, &in2, 1);
    float p2 = 0; for (int i = 0; i < 60; i++) { wcore_recoil_step(s2, &in2, 1.0f / 120); if (s2->recoil_offset.p.z > p2) p2 = s2->recoil_offset.p.z; }
    CHECK(p2 < peak * 0.8f, "dos manos no reduce el retroceso (%f vs %f)", p2, peak);
    free(s); free(s2);
}

static void shoot(const WeaponDef *d, WeaponState *s, WInput *in, WReloadMode rm) {
    in->trigger[0] = 1; run(d, s, in, rm, 0.12f); in->trigger[0] = 0; run(d, s, in, rm, 0.12f);
}

static void test_pist(const WeaponDef *d, WReloadMode rm) {
    WeaponState *s; WInput in; setup(&s, d, &in);
    CHECK(!s->chambered && !s->mag_inserted && reserve == 40, "recién creada: sin balas hasta que el anfitrión la iguale");
    run(d, s, &in, rm, 0.2f);
    CHECK(!s->chambered && !s->mag_inserted && reserve == 40 && s->channel[WCH_MAG] > 10.0f, "sin recarga propia: sigue sin cargador y no toca el contador (reserva %d)", reserve);
    settle(d, s, &in, rm);
    int cap = d->mag_capacity;
    CHECK(s->chambered && s->mag_inserted && s->mag_rounds == cap && s->channel[WCH_MAG] == 0.0f, "estado tras make_ready");
    CHECK(reserve == 40, "make_ready no toca el contador del juego (%d)", reserve);
    in.trigger[0] = 1; run(d, s, &in, rm, 0.5f);
    CHECK(fires == 1, "mantener gatillo dispara %d veces", fires);
    in.trigger[0] = 0; run(d, s, &in, rm, 0.2f);
    CHECK(s->mag_rounds == cap - 1 && s->chambered, "tras 1 tiro: cargador %d recámara %d", s->mag_rounds, s->chambered);
    CHECK(s->channel[WCH_SLIDE] < 0.001f, "corredera vuelve");
    {
        WeaponState *s3; WInput i3; setup(&s3, d, &i3); settle(d, s3, &i3, rm); i3.trigger[0] = 1;
        float mx = 0; int tend = 0;
        for (int i = 0; i < 60; i++) { d->update(s3, &i3, rm, &BR); float c = s3->channel[WCH_SLIDE]; if (c > mx) mx = c; if (c > 0.0005f) tend = i; }
        CHECK(mx > 0.03f, "recorrido corredera %f", mx);
        CHECK(tend * 1000.0f / 120 < 80.0f && tend * 1000.0f / 120 > 40.0f, "ciclo %.0f ms", tend * 1000.0f / 120);
        free(s3);
    }
    for (int i = 1; i < cap + 1; i++) shoot(d, s, &in, rm);
    CHECK(fires == cap + 1, "disparos %d != %d (cargador + recámara)", fires, cap + 1);
    CHECK(s->mag_rounds == 0 && !s->chambered, "vacía: cargador %d recámara %d", s->mag_rounds, s->chambered);
    CHECK(s->channel[WCH_SLIDE] > 0.04f, "slide lock: corredera abierta %f", s->channel[WCH_SLIDE]);
    CHECK(s->channel[WCH_ROUND] > 0.5f, "recámara visible vacía");
    int f0 = fires; shoot(d, s, &in, rm); CHECK(fires == f0, "dispara en seco");
    CHECK(reserve == 40, "disparar no toca la reserva (%d)", reserve);
    /* sin recarga VR: botones de cargador/corredera y la mano libre en el cargador, en la corredera o en la cintura, con reserva de sobra, no cambian nada */
    for (int m = 0; m <= 2; m++) {
        WPose W = wcore_weapon_world(s, &in);
        in.buttons[0] = WB_MAG_RELEASE | WB_B | WB_A; run(d, s, &in, (WReloadMode)m, 0.1f); in.buttons[0] = 0;
        in.hand[1].p = wpose_apply(W, d->points[WP_MAGWELL].p); in.grip[1] = 0; run(d, s, &in, (WReloadMode)m, 0.05f); in.grip[1] = 1; run(d, s, &in, (WReloadMode)m, 0.2f);
        in.hand[1].p = wpose_apply(W, d->points[WP_CHARGE].p); in.grip[1] = 0; run(d, s, &in, (WReloadMode)m, 0.05f); in.grip[1] = 1;
        for (int k = 0; k < 20; k++) { in.hand[1].p = wv_add(in.hand[1].p, wquat_rotate(W.q, wv(0, 0, 0.003f))); d->update(s, &in, (WReloadMode)m, &BR); }
        in.hand[1].p = wv_add(in.head.p, wv(0.12f, -0.70f, -0.10f)); in.grip[1] = 0; run(d, s, &in, (WReloadMode)m, 0.05f); in.grip[1] = 1; run(d, s, &in, (WReloadMode)m, 3.0f);
        in.grip[1] = 0; run(d, s, &in, (WReloadMode)m, 0.1f);
        CHECK(s->mag_inserted && s->mag_rounds == 0 && !s->chambered && s->channel[WCH_SLIDE] > 0.04f, "modo %d: sigue vacía con la corredera abierta", m);
        CHECK(count_ev(WS_MAG_OUT) == 0 && count_ev(WS_MAG_IN) == 0 && reserve == 40 && !s->held[0].active && !s->held[2].active, "modo %d: ni cargador suelto ni cuenta tocada", m);
        f0 = fires; shoot(d, s, &in, (WReloadMode)m); CHECK(fires == f0, "modo %d: sigue sin disparar", m);
    }
    d->make_ready(s, 4); run(d, s, &in, rm, 0.1f);
    CHECK(s->chambered && s->mag_rounds == 4 && s->channel[WCH_SLIDE] < 0.001f, "make_ready: lista (cargador %d recámara %d corredera %f)", s->mag_rounds, s->chambered, s->channel[WCH_SLIDE]);
    f0 = fires; shoot(d, s, &in, rm); CHECK(fires == f0 + 1, "no dispara tras igualarla al juego");
    s->mag_rounds = 0; s->chambered = 0; run(d, s, &in, rm, 0.1f);
    CHECK(s->channel[WCH_SLIDE] > 0.04f && s->channel[WCH_ROUND] > 0.5f, "vacía por el juego: corredera retenida");
    free(s);
}

static void test_equip_trigger(const WeaponDef *d) {
    /* equipar con el gatillo apretado: no dispara hasta soltarlo una vez */
    WeaponState *s; WInput in; setup(&s, d, &in);
    d->make_ready(s, d->mag_capacity);
    in.trigger[0] = 1; run(d, s, &in, WR_MANUAL, 1.0f);
    CHECK(fires == 0, "gatillo apretado al equipar disparó %d veces", fires);
    in.trigger[0] = 0; run(d, s, &in, WR_MANUAL, 0.1f);
    in.trigger[0] = 1; run(d, s, &in, WR_MANUAL, 0.3f);
    CHECK(fires == 1, "tras soltar y apretar debe disparar una vez (%d)", fires);
    free(s);
}

static void test_dt(const WeaponDef *d) {
    /* dt = 0, negativo, NaN, enorme y recién calloc: estado siempre finito */
    WeaponState *s; WInput in; setup(&s, d, &in);
    const float dts[] = {0.0f, -1.0f, NAN, INFINITY, 0.5f, 100.0f, 1.0f / 120};
    for (unsigned k = 0; k < sizeof dts / sizeof *dts; k++) {
        in.dt = dts[k]; in.trigger[0] = (k & 1) ? 1.0f : 0.0f;
        for (int i = 0; i < 5; i++) d->update(s, &in, WR_MANUAL, &BR);
        int ok = isfinite(s->recoil_offset.p.x) && isfinite(s->recoil_offset.q.w) && s->mag_rounds >= 0 && s->mag_rounds <= 100;
        for (int c = 0; c < WCH_COUNT; c++) ok = ok && isfinite(s->channel[c]);
        CHECK(ok, "estado no finito con dt=%g", dts[k]);
    }
    float q = s->recoil_offset.q.x * s->recoil_offset.q.x + s->recoil_offset.q.y * s->recoil_offset.q.y +
              s->recoil_offset.q.z * s->recoil_offset.q.z + s->recoil_offset.q.w * s->recoil_offset.q.w;
    CHECK(fabsf(q - 1) < 1e-3f, "cuaternión de retroceso no unitario (%f)", q);
    free(s);
    /* dt <= 0 no consume nada: ni carga inicial ni bridge */
    setup(&s, d, &in); in.dt = 0;
    for (int i = 0; i < 10; i++) d->update(s, &in, WR_MANUAL, &BR);
    CHECK(reserve == 40 && nev == 0, "dt=0 tocó el contador o sonó algo");
    free(s);
}

static void test_zero_state(const WeaponDef *d) {
    /* recién calloc + init: cuaternión de retroceso válido y sin piezas sueltas activas */
    WeaponState *s = calloc(1, d->state_size); d->init(s);
    CHECK(s->def == d && s->recoil_offset.q.w == 1.0f, "cuaternión de retroceso tras init");
    for (int i = 0; i < W_MAX_HELD; i++) CHECK(!s->held[i].active, "held[%d] activo tras init", i);
    free(s);
}

extern const float w_pist_bare_muzzle_z;

/* Supresor: lo decide el juego (WInput.game_flags & WGF_SUPP). Sin la bandera la pieza va apartada (canal CUSTOM0 = 1), nunca hay un objeto suelto y la boca es
 * la del cañón; con ella la pieza está puesta (CUSTOM0 = 0) y la boca es la punta del supresor. El mando de la mano libre no lo desmonta. */
static void test_pist_supp(const WeaponDef *d) {
    for (int flag = 0; flag <= 1; flag++) {
        WeaponState *s; WInput in; setup(&s, d, &in);
        in.game_flags = flag ? WGF_SUPP : 0;
        settle(d, s, &in, WR_MANUAL);
        CHECK(s->channel[WCH_CUSTOM0] == (flag ? 0.0f : 1.0f), "bandera %d: canal del supresor %f", flag, s->channel[WCH_CUSTOM0]);
        /* la mano libre sobre el supresor agarrando no lo suelta */
        WPose W = wcore_weapon_world(s, &in);
        in.hand[1].p = wpose_apply(W, d->parts[7].rest.p);
        in.grip[1] = 0; run(d, s, &in, WR_MANUAL, 0.1f);
        in.grip[1] = 1; run(d, s, &in, WR_MANUAL, 0.3f);
        in.hand[1].p = wv(0.4f, 1.0f, -0.1f); run(d, s, &in, WR_MANUAL, 0.3f);
        for (int i = 0; i < W_MAX_HELD; i++) CHECK(!(s->held[i].active && s->held[i].mesh == 3), "bandera %d: supresor suelto en held[%d]", flag, i);
        CHECK(s->channel[WCH_CUSTOM0] == (flag ? 0.0f : 1.0f), "bandera %d: canal tras agarrar %f", flag, s->channel[WCH_CUSTOM0]);
        /* la boca del disparo */
        in.grip[1] = 0; run(d, s, &in, WR_MANUAL, 0.2f);
        W = wcore_weapon_world(s, &in);
        in.trigger[0] = 1; d->update(s, &in, WR_MANUAL, &BR);
        CHECK(fires == 1, "bandera %d: disparos %d", flag, fires);
        WVec3 loc = wpose_apply(wpose_inv(W), last_mz.p);
        float zexp = flag ? d->points[WP_MUZZLE].p.z : w_pist_bare_muzzle_z;
        CHECK(fabsf(loc.z - zexp) < 1e-4f && fabsf(loc.y - d->points[WP_MUZZLE].p.y) < 1e-4f, "bandera %d: boca z=%f y=%f (esperada z=%f)", flag, loc.z, loc.y, zexp);
        CHECK(flag ? zexp < w_pist_bare_muzzle_z - 0.1f : 1, "la punta del supresor sobresale de la boca");
        free(s);
    }
}

/* Gatillo analógico: con más de 0,5 dispara; con 0,45 no; se rearma al bajar de 0,3. */
static void test_pist_threshold(const WeaponDef *d) {
    WeaponState *s; WInput in; setup(&s, d, &in);
    settle(d, s, &in, WR_MANUAL);
    int f0 = fires;
    in.trigger[0] = 0.45f; run(d, s, &in, WR_MANUAL, 0.1f);
    CHECK(fires == f0, "0,45 no dispara");
    in.trigger[0] = 0.6f; d->update(s, &in, WR_MANUAL, &BR);
    CHECK(fires == f0 + 1, "0,6 dispara (%d)", fires - f0);
    in.trigger[0] = 0.4f; run(d, s, &in, WR_MANUAL, 0.5f);
    in.trigger[0] = 0.6f; run(d, s, &in, WR_MANUAL, 0.1f);
    CHECK(fires == f0 + 1, "sin soltar por debajo de 0,3 no se rearma");
    in.trigger[0] = 0.2f; run(d, s, &in, WR_MANUAL, 0.1f);
    in.trigger[0] = 0.6f; run(d, s, &in, WR_MANUAL, 0.1f);
    CHECK(fires == f0 + 2, "rearmada al soltar (%d)", fires - f0);
    free(s);
}

int main(void) {
    test_math();
    const WeaponDef *d = find("pist");
    CHECK(d != NULL, "pist no registrada");
    if (d) {
        test_recoil(d);
        test_zero_state(d);
        test_pist(d, WR_MANUAL);
        test_pist_supp(d);
        test_pist_threshold(d);
        test_equip_trigger(d);
        test_dt(d);
    }
    printf(fails ? "test_core: %d fallos\n" : "test_core: OK\n", fails);
    return fails ? 1 : 0;
}
