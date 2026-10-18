/* Pruebas headless del núcleo y de la pistola ("pist"). Salida 0 = todo correcto. */
#include "wcore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int fails = 0, evs[64], nev = 0, fires = 0, reserve = 40, dry = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static int b_res(int id) { (void)id; return reserve; }
static void b_take(int id, int n) { (void)id; reserve -= n; }
static void b_fire(int id, WPose m) { (void)id; (void)m; fires++; }
static void b_hap(int h, float s, float t) { (void)h; (void)s; (void)t; }
static void b_snd(int id, int e) { (void)id; if (nev < 64) evs[nev++] = e; if (e == WS_DRY) dry++; }
static WGameBridge BR = {.ammo_reserve = b_res, .ammo_take = b_take, .fire = b_fire, .haptic = b_hap, .sound = b_snd};

static const WeaponDef *find(const char *id) {
    for (int i = 0; i < w_registry_count; i++) if (w_registry[i] && !strcmp(w_registry[i]->id, id)) return w_registry[i];
    return NULL;
}
/* Balas contabilizadas: reserva + cargador + recámara + disparadas. Debe valer siempre el total inicial
 * (sin cargador en la mano ni cartuchos por el suelo): detecta balas perdidas y duplicadas. */
static int accounted(const WeaponState *s) { return reserve + s->mag_rounds + s->chambered + fires; }
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
/* La pistola carga su cargador en la primera actualización y espera a ver el gatillo suelto. */
static void settle(const WeaponDef *d, WeaponState *s, WInput *in, WReloadMode rm) {
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
    CHECK(!s->chambered && !s->mag_inserted && reserve == 40, "recién creada: sin balas aún, contador intacto");
    settle(d, s, &in, rm);
    int cap = d->mag_capacity;
    CHECK(s->chambered && s->mag_inserted && s->mag_rounds == cap, "estado inicial");
    CHECK(reserve == 40 - (cap + 1), "la carga inicial se descuenta del contador: reserva %d", reserve);
    CHECK(accounted(s) == 40, "balas contabilizadas %d", accounted(s));
    /* semiauto: un disparo por pulsación */
    in.trigger[0] = 1; run(d, s, &in, rm, 0.5f);
    CHECK(fires == 1, "mantener gatillo dispara %d veces", fires);
    in.trigger[0] = 0; run(d, s, &in, rm, 0.2f);
    CHECK(s->mag_rounds == cap - 1 && s->chambered, "tras 1 tiro: cargador %d recámara %d", s->mag_rounds, s->chambered);
    CHECK(accounted(s) == 40, "tras 1 tiro: contabilizadas %d", accounted(s));
    CHECK(s->channel[WCH_SLIDE] < 0.001f, "corredera vuelve");
    /* ciclo ~60 ms: máx retroceso de corredera en el primer 40 ms */
    {
        WeaponState *s3; WInput i3; setup(&s3, d, &i3); settle(d, s3, &i3, rm); i3.trigger[0] = 1;
        float mx = 0; int tmax = 0, tend = 0;
        for (int i = 0; i < 60; i++) { d->update(s3, &i3, rm, &BR); float c = s3->channel[WCH_SLIDE]; if (c > mx) { mx = c; tmax = i; } if (c > 0.0005f) tend = i; }
        CHECK(mx > 0.03f, "recorrido corredera %f", mx);
        CHECK(tend * 1000.0f / 120 < 80.0f && tend * 1000.0f / 120 > 40.0f, "ciclo %.0f ms", tend * 1000.0f / 120);
        (void)tmax; free(s3);
    }
    /* disparar hasta vaciar */
    for (int i = 1; i < cap + 1; i++) shoot(d, s, &in, rm);
    CHECK(fires == cap + 1, "disparos %d != %d (cargador + recámara)", fires, cap + 1);
    CHECK(s->mag_rounds == 0 && !s->chambered, "vacía: cargador %d recámara %d", s->mag_rounds, s->chambered);
    CHECK(s->channel[WCH_SLIDE] > 0.04f, "slide lock: corredera abierta %f", s->channel[WCH_SLIDE]);
    CHECK(s->channel[WCH_ROUND] > 0.5f, "recámara visible vacía");
    int f0 = fires; shoot(d, s, &in, rm); CHECK(fires == f0, "dispara en seco");
    CHECK(reserve == 40 - (cap + 1), "disparar no toca la reserva (%d)", reserve);
    CHECK(accounted(s) == 40, "vacía: contabilizadas %d", accounted(s));

    if (rm == WR_MANUAL) {
        /* 1) soltar cargador */
        in.buttons[0] = WB_MAG_RELEASE; run(d, s, &in, rm, 0.05f); in.buttons[0] = 0; run(d, s, &in, rm, 0.1f);
        CHECK(!s->mag_inserted && count_ev(WS_MAG_OUT) == 1, "cargador no cae");
        CHECK(s->held[0].active, "cargador cayendo no visible");
        run(d, s, &in, rm, 1.5f);
        /* 2) mano libre a la cadera + grip */
        in.hand[1].p = wv_add(in.head.p, wv(0.12f, -0.70f, -0.10f)); in.grip[1] = 0; run(d, s, &in, rm, 0.1f);
        in.grip[1] = 1.0f; run(d, s, &in, rm, 0.1f);
        CHECK(s->held[0].active, "no toma cargador de la cadera");
        /* 3) llevarlo al pozo alineado: mano orientada, cargador desde abajo */
        WPose W = wcore_weapon_world(s, &in);
        WQuat r = wquat_axis_angle(wv(1, 0, 0), -0.7853982f);
        in.hand[1].q = wquat_mul(wquat_mul(W.q, d->points[WP_MAGWELL].q), wquat_inv(r));
        WVec3 well = wpose_apply(W, d->points[WP_MAGWELL].p);
        WVec3 up = wquat_rotate(W.q, wquat_rotate(d->points[WP_MAGWELL].q, wv(0, 1, 0)));
        WPose hr = {{0, 0, -0.02f}, r};          /* misma relación mano-cargador que el arma */
        (void)hr;
        for (int k = 0; k <= 60 && !s->mag_inserted; k++) {
            float sd = -0.08f + 0.004f * k;          /* profundidad de la punta del cargador sobre el pozo */
            WVec3 mc = wv_sub(wv_add(well, wv_scale(up, sd)), wv_scale(up, -0.0675f + 0.0675f * 2));
            WPose mrel = {{0, 0, -0.02f}, r};
            WPose hnd = {{0, 0, 0}, in.hand[1].q};
            WVec3 off = wquat_rotate(hnd.q, mrel.p);
            in.hand[1].p = wv_sub(mc, off);
            d->update(s, &in, rm, &BR);
        }
        CHECK(s->mag_inserted && s->mag_rounds == cap && count_ev(WS_MAG_IN) == 1, "no encaja (ins=%d n=%d)", s->mag_inserted, s->mag_rounds);
        CHECK(reserve == 40 - (cap + 1) - cap, "reserva tras recarga: %d", reserve);
        CHECK(accounted(s) == 40, "tras recarga: contabilizadas %d (balas perdidas o duplicadas)", accounted(s));
        CHECK(s->channel[WCH_SLIDE] > 0.04f && !s->chambered, "corredera sigue abierta tras recarga");
        in.grip[1] = 0; run(d, s, &in, rm, 0.1f);
        /* 4) soltar corredera con botón A */
        in.buttons[0] = WB_A; run(d, s, &in, rm, 0.05f); in.buttons[0] = 0; run(d, s, &in, rm, 0.4f);
        CHECK(s->chambered && s->mag_rounds == cap - 1, "tras soltar: recámara %d balas %d", s->chambered, s->mag_rounds);
        CHECK(accounted(s) == 40, "tras soltar corredera: contabilizadas %d", accounted(s));
        CHECK(s->channel[WCH_SLIDE] < 0.001f, "corredera cerrada");
        f0 = fires; shoot(d, s, &in, rm); CHECK(fires == f0 + 1, "no dispara tras recarga");
    }
    free(s);
}

static void test_manual_rack(const WeaponDef *d) {
    /* recarga automática completa y rack manual con la mano libre; las cuentas deben cuadrar siempre */
    WeaponState *s; WInput in; setup(&s, d, &in);
    settle(d, s, &in, WR_AUTOMATIC);
    for (int i = 0; i < 13; i++) shoot(d, s, &in, WR_AUTOMATIC);
    run(d, s, &in, WR_AUTOMATIC, 2.5f);
    CHECK(s->mag_inserted && s->mag_rounds == d->mag_capacity - 1 && s->chambered, "automatico: cargador %d ins %d cam %d", s->mag_rounds, s->mag_inserted, s->chambered);
    CHECK(accounted(s) == 40, "automatico: contabilizadas %d", accounted(s));
    run(d, s, &in, WR_AUTOMATIC, 0.5f);
    WPose W = wcore_weapon_world(s, &in);
    if (s->channel[WCH_SLIDE] >= 0.001f) {
        in.buttons[0] = WB_A; run(d, s, &in, WR_AUTOMATIC, 0.05f); in.buttons[0] = 0; run(d, s, &in, WR_AUTOMATIC, 0.4f);
    }
    CHECK(s->chambered, "recámara tras automática");
    /* rack manual: mano al punto, grip, tirar -Z->+Z, soltar */
    int before = s->mag_rounds, res0 = reserve;
    W = wcore_weapon_world(s, &in);
    WVec3 c = wpose_apply(W, d->points[WP_CHARGE].p);
    in.hand[1].p = c; in.grip[1] = 0; run(d, s, &in, WR_MANUAL, 0.05f);
    in.grip[1] = 1; run(d, s, &in, WR_MANUAL, 0.05f);
    for (int k = 0; k < 20; k++) { in.hand[1].p = wv_add(c, wquat_rotate(W.q, wv(0, 0, 0.003f * k))); d->update(s, &in, WR_MANUAL, &BR); }
    CHECK(s->channel[WCH_SLIDE] > 0.04f && !s->chambered, "rack: corredera atrás, cartucho expulsado (%f %d)", s->channel[WCH_SLIDE], s->chambered);
    CHECK(reserve == res0 + 1, "rack: el cartucho vivo expulsado vuelve a la reserva (%d -> %d)", res0, reserve);
    in.grip[1] = 0; run(d, s, &in, WR_MANUAL, 0.4f);
    CHECK(s->chambered && s->channel[WCH_SLIDE] < 0.001f, "rack: nuevo cartucho");
    CHECK(s->mag_rounds == before - 1, "rack: el cargador pierde exactamente una bala (%d -> %d)", before, s->mag_rounds);
    CHECK(accounted(s) == 40, "rack: contabilizadas %d (bala perdida o duplicada)", accounted(s));
    free(s);
}

static void test_equip_trigger(const WeaponDef *d) {
    /* equipar con el gatillo apretado: no dispara hasta soltarlo una vez */
    WeaponState *s; WInput in; setup(&s, d, &in);
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

static void test_auto_stage(const WeaponDef *d) {
    /* cambio de modo a mitad de la recarga automática: no se queda atascada ni pisa un cargador montado */
    WeaponState *s; WInput in; setup(&s, d, &in);
    settle(d, s, &in, WR_AUTOMATIC);
    for (int i = 0; i < 13; i++) shoot(d, s, &in, WR_AUTOMATIC);
    run(d, s, &in, WR_AUTOMATIC, 0.2f);                 /* ya expulsó el cargador vacío (espera al nuevo) */
    CHECK(!s->mag_inserted, "el automático no expulsó el cargador vacío");
    run(d, s, &in, WR_MANUAL, 2.0f);                    /* pasa a manual: se abandona la secuencia */
    CHECK(!s->mag_inserted, "manual no recarga solo");
    run(d, s, &in, WR_AUTOMATIC, 3.0f);                 /* vuelve a automático: debe terminar la recarga */
    CHECK(s->mag_inserted && s->mag_rounds > 0, "automático retoma la recarga sin cargador (%d)", s->mag_inserted);
    CHECK(accounted(s) == 40, "retomada: contabilizadas %d", accounted(s));
    free(s);
    /* cargador montado a mano mientras corre la animación del automático: no se pisa */
    setup(&s, d, &in);
    settle(d, s, &in, WR_AUTOMATIC);
    for (int i = 0; i < 13; i++) shoot(d, s, &in, WR_AUTOMATIC);
    for (int i = 0; i < 120 * 3 && !s->held[0].active; i++) d->update(s, &in, WR_AUTOMATIC, &BR);
    CHECK(s->held[0].active && !s->mag_inserted, "animación del cargador nuevo no empezó");
    s->mag_inserted = 1; s->mag_rounds = 5;            /* otro cargador montado durante la animación */
    int res = reserve;
    run(d, s, &in, WR_AUTOMATIC, 1.0f);
    CHECK(s->mag_rounds == 5 && reserve == res, "el cargador de la animación pisó el montado (%d balas, reserva %d->%d)", s->mag_rounds, res, reserve);
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
        test_manual_rack(d);
        test_equip_trigger(d);
        test_dt(d);
        test_auto_stage(d);
    }
    printf(fails ? "test_core: %d fallos\n" : "test_core: OK\n", fails);
    return fails ? 1 : 0;
}
