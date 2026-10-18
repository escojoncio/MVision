/* Contrato común sobre el registro de armas, enlazando contra libweapons.a (no contra los .o): comprueba que
 * las diez armas sobreviven al enlazado estático y que todas cumplen el contrato de weapon.h
 * (ids únicos, puntos y piezas válidos, init/bind, dt inválido, gatillo apretado al equipar, estado recién
 * creado y entradas aleatorias; con -fsanitize se vigilan además accesos fuera de rango). */
#include "wcore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define N_EXPECTED 10

/* ---------------- puente de pruebas ---------------- */
static struct { int calls, fires, reserve; } G;
static int b_res(int id) { (void)id; G.calls++; return G.reserve; }
static void b_take(int id, int n) { (void)id; G.calls++; G.reserve -= n; }
static void b_fire(int id, WPose m) { (void)id; (void)m; G.calls++; G.fires++; }
static void b_hap(int h, float s, float t) { (void)h; (void)s; (void)t; G.calls++; }
static void b_snd(int id, int e) { (void)id; G.calls++; if (e < 0 || e >= WS_COUNT) { fails++; printf("FALLO sonido fuera del enum WS_*: %d\n", e); } }
static int b_tgt(WVec3 *o, int max) { G.calls++; if (max > 0) { o[0] = wv(0, 1.6f, -100); return 1; } return 0; }
static int b_proj(int id, WPose *o) { (void)id; G.calls++; (void)o; return 0; }
static void b_steer(int id, float y, float p) { (void)id; (void)y; (void)p; G.calls++; }
static const WGameBridge BR = { b_res, b_take, b_fire, b_hap, b_snd, b_tgt, b_proj, b_steer, NULL };

static uint32_t rs = 0x1234567u;
static float rnd(void) { rs = rs * 1664525u + 1013904223u; return ((rs >> 8) & 0xFFFF) / 65535.0f; }

static WInput mk_input(void) {
    WInput in; memset(&in, 0, sizeof in);
    in.head.q = in.hand[0].q = in.hand[1].q = wquat_identity();
    in.head.p = wv(0, 1.7f, 0); in.hand[0].p = wv(0.2f, 1.3f, -0.4f); in.hand[1].p = wv(-0.2f, 1.2f, -0.5f);
    in.dt = 1.0f / 90;
    return in;
}
static int finite_state(const WeaponState *s) {
    int ok = isfinite(s->recoil_offset.p.x) && isfinite(s->recoil_offset.p.y) && isfinite(s->recoil_offset.p.z) &&
             isfinite(s->recoil_offset.q.x) && isfinite(s->recoil_offset.q.w) && s->mag_rounds >= 0;
    for (int c = 0; c < WCH_COUNT; c++) ok = ok && isfinite(s->channel[c]);
    for (int i = 0; i < W_MAX_HELD; i++) ok = ok && isfinite(s->held[i].world.p.x) && isfinite(s->held[i].world.q.w);
    return ok;
}
static float qn(WQuat q) { return q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w; }

static void test_registry_shape(void) {
    CHECK(w_registry_count == N_EXPECTED, "registro con %d entradas (esperadas %d)", w_registry_count, N_EXPECTED);
    int present = 0;
    for (int i = 0; i < w_registry_count; i++) present += w_registry[i] != NULL;
    CHECK(present == N_EXPECTED, "armas presentes tras enlazar el .a: %d/%d", present, N_EXPECTED);
    static const char *const forbidden[] = { "c4", "chaff", "stun", "mine", "ps1", "psx" };
    for (int i = 0; i < w_registry_count; i++) {
        const WeaponDef *d = w_registry[i];
        if (!d) continue;
        CHECK(d->id && d->id[0], "arma %d sin id", i);
        for (unsigned k = 0; k < sizeof forbidden / sizeof *forbidden; k++) CHECK(!strstr(d->id, forbidden[k]), "id no neutro: %s", d->id);
        for (int j = i + 1; j < w_registry_count; j++) {
            if (!w_registry[j]) continue;
            CHECK(strcmp(d->id, w_registry[j]->id), "id repetido: %s", d->id);
            CHECK(d->game_weapon_id != w_registry[j]->game_weapon_id, "game_weapon_id repetido entre %s y %s (%d)", d->id, w_registry[j]->id, d->game_weapon_id);
        }
        CHECK(d->init && d->update && d->state_size >= sizeof(WeaponState), "%s: init/update/state_size", d->id);
        CHECK(d->meshes == NULL && d->nmeshes == 0, "%s: no debe llevar mallas compiladas", d->id);
        CHECK(d->mesh_names && weapon_mesh_count(d) > 0, "%s: mesh_names ausente o vacío", d->id);
        CHECK(d->nparts > 0 && d->parts, "%s: sin piezas", d->id);
        for (int k = 0; k < WP_COUNT; k++) CHECK(fabsf(qn(d->points[k].q) - 1) < 1e-4f, "%s: punto %d con cuaternión no unitario", d->id, k);
        for (int k = 0; k < d->nparts; k++) {
            CHECK(fabsf(qn(d->parts[k].rest.q) - 1) < 1e-4f, "%s: pieza %s con cuaternión no unitario", d->id, d->parts[k].name);
            CHECK(d->parts[k].parent < k, "%s: pieza %s con padre posterior", d->id, d->parts[k].name);
            CHECK(d->parts[k].mesh < weapon_mesh_count(d), "%s: pieza %s con índice de mesh fuera de mesh_names", d->id, d->parts[k].name);
        }
        CHECK(d->recoil.stiffness >= 0 && d->recoil.damping >= 0, "%s: retroceso", d->id);
    }
}

static void test_lifecycle(void) {
    for (int i = 0; i < w_registry_count; i++) {
        const WeaponDef *d = w_registry[i];
        WeaponState *s = calloc(1, d->state_size);
        d->init(s);                                           /* sin asignar s->def antes */
        CHECK(s->def == d, "%s: init no fija def", d->id);
        CHECK(s->recoil_offset.q.w == 1.0f, "%s: recoil_offset.q sin identidad tras init", d->id);
        for (int k = 0; k < W_MAX_HELD; k++) CHECK(s->held[k].mesh >= -1 && !s->held[k].active, "%s: held[%d] tras init", d->id, k);
        /* binding: init -> bind -> init conserva el binding y limpia el resto de la extensión */
        WMesh ms[16]; memset(ms, 0, sizeof ms);
        int nn = weapon_mesh_count(d); if (nn > 16) nn = 16;
        for (int k = 0; k < nn; k++) ms[k].name = d->mesh_names[k];
        WPart ps[2] = { { "root", -1, { { 0, 0, 0 }, { 0, 0, 0, 1 } }, 0, WCH_NONE, { 0, 0, 0 }, 0 },
                        { "otra", 0, { { 0, 0, 0 }, { 0, 0, 0, 1 } }, 1, WCH_NONE, { 0, 0, 0 }, 0 } };
        weapon_bind(s, ms, nn, ps, 2);
        s->mag_rounds = 7; s->phase = 3; s->t = 5.0f;
        d->init(s);
        int n1, n2;
        CHECK(weapon_meshes(s, &n1) == ms && n1 == nn, "%s: init perdió las meshes enlazadas", d->id);
        CHECK(weapon_parts(s, &n2) == ps && n2 == 2, "%s: init perdió las piezas enlazadas", d->id);
        CHECK(s->mag_rounds != 7 && s->phase == 0 && s->t == 0, "%s: init no limpió el estado", d->id);
        CHECK(wcore_part_find(s, "otra") == 1 && wcore_part_find(s, "root") == 0, "%s: wcore_part_find ignora las piezas enlazadas", d->id);
        CHECK(wcore_part_find(s, d->parts[d->nparts - 1].name) == -1, "%s: wcore_part_find sigue viendo las piezas del def", d->id);
        /* con binding de piezas propias nada debe desbordar */
        WPose out[2]; int np = wcore_part_poses(s, wpose_identity(), out, 2);
        CHECK(np == 2, "%s: wcore_part_poses con binding", d->id);
        WInput in = mk_input();
        memset(&G, 0, sizeof G); G.reserve = 30;
        for (int f = 0; f < 200; f++) d->update(s, &in, WR_MANUAL, &BR);
        CHECK(finite_state(s), "%s: estado no finito con piezas enlazadas", d->id);
        free(s);
    }
}

static void test_dt_and_zero(void) {
    for (int i = 0; i < w_registry_count; i++) {
        const WeaponDef *d = w_registry[i];
        WeaponState *s = calloc(1, d->state_size); d->init(s);
        WInput in = mk_input();
        const float bad[] = { 0.0f, -1.0f, NAN, -INFINITY, 0.0f };
        int mr0 = s->mag_rounds, ph0 = s->phase;
        memset(&G, 0, sizeof G); G.reserve = 30;
        for (unsigned k = 0; k < sizeof bad / sizeof *bad; k++) {
            in.dt = bad[k];
            for (int f = 0; f < 20; f++) d->update(s, &in, WR_MANUAL, &BR);
        }
        CHECK(G.calls == 0, "%s: dt inválido no debe tocar el puente (%d llamadas)", d->id, G.calls);
        CHECK(finite_state(s) && s->mag_rounds == mr0 && s->phase == ph0, "%s: dt inválido cambió el estado", d->id);
        /* dt enorme / +inf: se recorta */
        const float big[] = { 0.2f, 10.0f, 1e9f, INFINITY };
        for (unsigned k = 0; k < sizeof big / sizeof *big; k++) {
            in.dt = big[k];
            for (int f = 0; f < 10; f++) d->update(s, &in, (WReloadMode)(f % 3), &BR);
            CHECK(finite_state(s), "%s: estado no finito con dt=%g", d->id, big[k]);
        }
        free(s);
        /* recién calloc: entrada toda a cero (cuaterniones nulos incluidos) no rompe nada */
        s = calloc(1, d->state_size); d->init(s);
        WInput z; memset(&z, 0, sizeof z); z.dt = 1.0f / 90;
        memset(&G, 0, sizeof G); G.reserve = 30;
        for (int f = 0; f < 30; f++) d->update(s, &z, WR_MANUAL, &BR);
        CHECK(finite_state(s), "%s: entrada a cero rompe el estado", d->id);
        free(s);
    }
}

static void test_equip_held_trigger(void) {
    /* equipar con el gatillo apretado no dispara, ni siquiera tras un segundo */
    for (int i = 0; i < w_registry_count; i++) {
        const WeaponDef *d = w_registry[i];
        for (int rm = 0; rm < 3; rm++) {
            WeaponState *s = calloc(1, d->state_size); d->init(s);
            WInput in = mk_input(); in.trigger[0] = 1.0f; in.trigger[1] = 1.0f;
            memset(&G, 0, sizeof G); G.reserve = 30;
            for (int f = 0; f < 90; f++) d->update(s, &in, (WReloadMode)rm, &BR);
            CHECK(G.fires == 0, "%s (modo %d): disparó %d veces con el gatillo apretado al equipar", d->id, rm, G.fires);
            free(s);
        }
    }
}

static void test_fuzz(void) {
    /* entradas aleatorias con todos los modos: sin NaN, sin munición negativa, sin desbordes */
    for (int i = 0; i < w_registry_count; i++) {
        const WeaponDef *d = w_registry[i];
        WeaponState *s = calloc(1, d->state_size); d->init(s);
        WInput in = mk_input();
        memset(&G, 0, sizeof G); G.reserve = 60;
        for (int f = 0; f < 4000; f++) {
            in.head.p = wv(0.3f * rnd(), 1.5f + 0.3f * rnd(), 0.3f * rnd());
            for (int h = 0; h < 2; h++) {
                in.hand[h].p = wv_add(in.head.p, wv(1.0f * (rnd() - 0.5f), -0.6f * rnd(), -0.7f * rnd()));
                in.hand[h].q = wquat_normalize((WQuat){ rnd() - 0.5f, rnd() - 0.5f, rnd() - 0.5f, rnd() + 0.2f });
                in.trigger[h] = rnd() < 0.3f ? 1.0f : rnd() * 0.2f;
                in.grip[h] = rnd() < 0.5f ? 1.0f : 0.0f;
                in.buttons[h] = (uint32_t)(rnd() * 16.0f) & 15u;
            }
            in.head.q = wquat_normalize((WQuat){ 0.1f * (rnd() - 0.5f), rnd() - 0.5f, 0.1f * (rnd() - 0.5f), 1.0f });
            in.support_held = rnd() < 0.3f;
            in.dt = rnd() < 0.05f ? 0.2f : 1.0f / 90;
            d->update(s, &in, (WReloadMode)((f / 700) % 3), &BR);
            if (!finite_state(s)) { CHECK(0, "%s: estado no finito en el cuadro %d", d->id, f); break; }
            if (G.reserve < 0) { CHECK(0, "%s: reserva negativa (%d) en el cuadro %d", d->id, G.reserve, f); break; }
        }
        free(s);
    }
}

int main(void) {
    test_registry_shape();
    test_lifecycle();
    test_dt_and_zero();
    test_equip_held_trigger();
    test_fuzz();
    printf("%d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
