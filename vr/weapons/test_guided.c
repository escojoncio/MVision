/* Pruebas del lanzador guiado: visor por cercanía, secuencia disparo->guiado->impacto y recargas.
 * Compilar: cc -I. test_guided.c w_guided.c wcore.c -lm -o test_guided
 * Con GUIDED_PREVIEW_DIR=<dir> escribe frames PPM del overlay. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wcore.h"
#include "w_guided.h"

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

typedef struct {
    int reserve, taken, fired, alive, steer_calls, sounds[16];
    float last_yaw, last_pitch, proj_x, proj_z;
    WPose muzzle;
} Sim;
static Sim sim;
static int b_res(int id) { (void)id; return sim.reserve; }
static void b_take(int id, int n) { (void)id; sim.reserve -= n; sim.taken += n; }
static void b_fire(int id, WPose m) { (void)id; sim.fired++; sim.alive = 1; sim.muzzle = m; sim.proj_z = m.p.z; }
static void b_hap(int h, float s, float t) { (void)h; (void)s; (void)t; }
static void b_snd(int id, int ev) { (void)id; if (ev >= 0 && ev < 16) sim.sounds[ev]++; }
static int b_proj(int id, WPose *o) {
    (void)id; if (!sim.alive) return 0;
    o->p = wv(sim.muzzle.p.x, 1.6f, sim.proj_z); o->q = wquat_identity(); return 1;
}
static void b_steer(int id, float y, float p) { (void)id; sim.steer_calls++; sim.last_yaw = y; sim.last_pitch = p; }
static WGameBridge G = {b_res, b_take, b_fire, b_hap, b_snd, NULL, b_proj, b_steer, NULL};

static WQuat qy(float deg) { return wquat_axis_angle(wv(0, 1, 0), deg * 3.14159265f / 180); }
static WQuat qmul_(WQuat a, WQuat b) { return wquat_mul(a, b); }
static WQuat qx(float deg) { return wquat_axis_angle(wv(1, 0, 0), deg * 3.14159265f / 180); }

static char stbuf[sizeof(GuidedState)] __attribute__((aligned(16)));
static GuidedState *S;

static WInput base_input(void) {
    WInput in; memset(&in, 0, sizeof in);
    in.head.q = wquat_identity(); in.head.p = wv(0, 1.7f, 0);
    in.hand[0].q = wquat_identity(); in.hand[0].p = wv(0.127f, 1.524f, -0.40f);   /* ojo sobre la mirilla */
    in.hand[1].q = wquat_identity(); in.hand[1].p = wv(0.1f, 1.2f, -0.2f);
    in.dt = 1.0f / 90;
    return in;
}
static void step(WInput *in, WReloadMode rm, int n) {
    for (int i = 0; i < n; i++) w_guided.update(&S->base, in, rm, &G);
}
static void reset(int reserve) {
    memset(stbuf, 0, sizeof stbuf); S = (GuidedState *)stbuf;
    w_guided.init(&S->base); memset(&sim, 0, sizeof sim); sim.reserve = reserve;
}
static void pull(WInput *in, WReloadMode rm) {
    in->trigger[0] = 0; step(in, rm, 2); in->trigger[0] = 1; step(in, rm, 2); in->trigger[0] = 0;
}

static void write_ppm(const char *name, const uint32_t *px, int w, int h) {
    const char *d = getenv("GUIDED_PREVIEW_DIR"); if (!d) return;
    char path[512]; snprintf(path, sizeof path, "%s/%s", d, name);
    FILE *f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { const uint8_t *q = (const uint8_t *)&px[i]; fwrite(q, 1, 3, f); }
    fclose(f);
}
/* fondo sintético: cielo, horizonte y suelo en perspectiva con edificios */
static void synth_bg(uint32_t *px, int w, int h, float t) {
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint8_t c[4] = {0, 0, 0, 255};
        int hz = h / 2 - 10;
        if (y < hz) { c[0] = 90 + y / 3; c[1] = 130 + y / 3; c[2] = 200; }
        else {
            float dz = (float)(y - hz + 1), u = (x - w / 2) / dz * 40.0f, v = 2000.0f / dz - t * 30;
            int chk = ((int)floorf(u) + (int)floorf(v)) & 1;
            c[0] = chk ? 120 : 90; c[1] = chk ? 110 : 85; c[2] = chk ? 70 : 60;
        }
        int bx = (x / 28) , bh = 20 + (bx * 37) % 50;
        if (y < hz && y > hz - bh && (x % 28) < 22 && (bx % 3)) { c[0] = 150; c[1] = 150; c[2] = 155; }
        memcpy(&px[y * w + x], c, 4);
    }
}

static void t_proximity(void) {
    reset(3);
    WInput in = base_input();
    step(&in, WR_MANUAL, 5);
    CHECK(S->shouldered); CHECK(S->eye_near);
    in.head.p.x += 0.40f; step(&in, WR_MANUAL, 5);
    CHECK(!S->eye_near);
    in.head.p.x -= 0.40f; step(&in, WR_MANUAL, 5);
    CHECK(S->eye_near);
    in.head.p.x += 0.12f; step(&in, WR_MANUAL, 3);          /* entre IN y OUT: histéresis, sigue dentro */
    CHECK(S->eye_near);
    in.head.p.x += 0.30f; step(&in, WR_MANUAL, 3);
    CHECK(!S->eye_near);
    CHECK(S->base.aux.active == 0);
}

static void t_flight(void) {
    reset(3);
    WInput in = base_input();
    step(&in, WR_MANUAL, 5);
    pull(&in, WR_MANUAL);
    CHECK(sim.fired == 1); CHECK(S->base.mag_rounds == 0); CHECK(S->base.phase == G_FLIGHT);
    CHECK(S->base.aux.active && S->base.aux.use_game_camera);
    CHECK(S->base.aux.overlay & WO_TELEMETRY); CHECK(S->base.aux.overlay & WO_NOISE);
    CHECK(S->base.aux.overlay & WO_SCANLINES); CHECK(S->base.aux.overlay & WO_RETICLE);
    CHECK(S->base.aux.target_part >= 0);
    {   /* la cámara de la pantalla va en la punta del misil: 2 cm por delante del morro, en su dirección y sin inclinación */
        const WVec3 mp = wv(sim.muzzle.p.x, 1.6f, sim.proj_z);
        const WPose c = S->base.aux.camera;
        const WVec3 fw = wquat_rotate(c.q, wv(0, 0, -1));
        CHECK(fabsf((mp.z - c.p.z) - (GD_CAM_NOSE + 0.02f)) < 1e-4f);    /* misil con delante = -Z: la cámara está 0,425 m por delante del origen */
        CHECK(fabsf(c.p.y - mp.y) < 1e-5f && fabsf(c.p.x - mp.x) < 1e-5f);
        CHECK(fw.z < -0.9999f && fabsf(fw.y) < 1e-4f);
        CHECK(fabsf(GD_CAM_AHEAD - 0.02f) < 1e-6f);
    }
    step(&in, WR_MANUAL, 60);                 /* 0,67 s: visor grande */
    CHECK(S->view_blend > 0.99f);
    WPose pp; float pw, ph;
    float e = guided_view_panel(&S->base, &in, &pp, &pw, &ph);
    CHECK(e > 0.99f); CHECK(fabsf(pw - 0.62f) < 0.01f);
    WVec3 eye = wv(0.032f, 1.7f, 0);          /* el panel grande queda ante el ojo, a ~0,30 m */
    CHECK(fabsf(wv_len(wv_sub(pp.p, eye)) - 0.30f) < 0.03f);
    /* al separar el ojo se reduce a pantallita y el misil sigue volando */
    in.head.p.x += 0.5f;
    for (int i = 0; i < 80; i++) { sim.proj_z -= 0.5f; step(&in, WR_MANUAL, 1); }
    CHECK(S->view_blend < 0.01f); CHECK(S->base.phase == G_FLIGHT);
    e = guided_view_panel(&S->base, &in, &pp, &pw, &ph);
    CHECK(e < 0.01f); CHECK(fabsf(pw - 0.10f) < 0.01f);
    CHECK(S->dist > 30.0f && S->alt > 0);
    in.head.p.x -= 0.5f;

    /* guiado: zona muerta, inclinación a la derecha / arriba, stick, recentrado */
    in.hand[0].q = qy(-2.0f); step(&in, WR_MANUAL, 30);
    CHECK(fabsf(sim.last_yaw) < 1e-3f);
    in.hand[0].q = qy(-15.0f); step(&in, WR_MANUAL, 60);
    CHECK(sim.last_yaw > 0.25f && sim.last_yaw < 0.6f); CHECK(fabsf(sim.last_pitch) < 0.01f);
    in.hand[0].q = qmul_(qy(-40.0f), qx(30.0f)); step(&in, WR_MANUAL, 60);
    CHECK(sim.last_yaw > 0.95f && sim.last_pitch > 0.9f);
    in.buttons[0] = WB_B; step(&in, WR_MANUAL, 1); in.buttons[0] = 0;   /* recentrar */
    step(&in, WR_MANUAL, 60);
    CHECK(fabsf(sim.last_yaw) < 0.02f);
    guided_set_stick(&S->base, -1.0f, 0.0f); step(&in, WR_MANUAL, 60);
    CHECK(sim.last_yaw < -0.9f);
    guided_set_stick(&S->base, 0, 0); step(&in, WR_MANUAL, 60);

    /* impacto: el juego deja de dar proyectil -> estática -> vista normal */
    sim.alive = 0; step(&in, WR_MANUAL, 2);
    CHECK(S->base.phase == G_STATIC); CHECK(S->ended_by == 1);
    CHECK(sim.last_yaw == 0 && sim.last_pitch == 0);
    CHECK(!S->base.aux.use_game_camera); CHECK(S->base.aux.active);
    step(&in, WR_MANUAL, (int)(GD_STATIC_TIME * 90) + 5);
    CHECK(S->base.phase == G_IDLE); CHECK(!S->base.aux.active);
    step(&in, WR_MANUAL, 40); CHECK(S->view_blend < 0.01f);

    /* sin combustible */
    reset(3); in = base_input(); step(&in, WR_MANUAL, 3); pull(&in, WR_MANUAL);
    for (int i = 0; i < 90 * 15; i++) { sim.proj_z -= 0.05f; step(&in, WR_MANUAL, 1); if (S->base.phase != G_FLIGHT) break; }
    CHECK(S->base.phase == G_STATIC && S->ended_by == 2);
    /* el juego no crea el misil */
    reset(3); in = base_input(); step(&in, WR_MANUAL, 3); sim.alive = 0;
    G.fire = NULL; pull(&in, WR_MANUAL); step(&in, WR_MANUAL, 90);
    CHECK(S->base.phase == G_FLIGHT);        /* aún dentro del plazo de creación */
    step(&in, WR_MANUAL, 60);
    CHECK(S->base.phase == G_STATIC && S->ended_by == 3);
    G.fire = b_fire;
}

static void t_dry(void) {
    reset(0);
    WInput in = base_input(); step(&in, WR_MANUAL, 3);
    pull(&in, WR_MANUAL); step(&in, WR_MANUAL, 3);
    CHECK(sim.fired == 1); sim.alive = 0; step(&in, WR_MANUAL, 90 + 5);
    pull(&in, WR_MANUAL);                         /* vacío */
    CHECK(sim.fired == 1); CHECK(sim.sounds[WS_DRY] >= 1);
}

static void t_reload_auto(WReloadMode rm, int stage_wait) {
    reset(2);
    WInput in = base_input(); step(&in, rm, 3);
    pull(&in, rm); sim.alive = 0; step(&in, rm, (int)(GD_STATIC_TIME * 90) + 10);
    CHECK(S->base.phase == G_IDLE && S->base.mag_rounds == 0);
    CHECK(!guided_round_visible(&S->base));
    if (rm == WR_SIMPLIFIED) {                  /* gesto: mano libre cerca de la boca trasera + grip */
        step(&in, rm, 20); CHECK(S->base.mag_rounds == 0);
        WPose mw = wcore_point_world(&S->base, &in, WP_MAGWELL);
        in.hand[1].p = mw.p; in.grip[1] = 0; step(&in, rm, 2); in.grip[1] = 1;
    }
    (void)stage_wait;
    step(&in, rm, 90 * 5);
    CHECK(S->base.mag_rounds == 1); CHECK(sim.taken == 1); CHECK(sim.reserve == 1);
    CHECK(S->base.channel[WCH_LID] < 0.01f); CHECK(S->auto_stage == GR_NONE);
    CHECK(guided_round_visible(&S->base));
    CHECK(sim.sounds[WS_MAG_IN] == 1);
    sim.alive = 0; pull(&in, rm); CHECK(sim.fired == 2);          /* vuelve a disparar */
}

static void t_reload_manual(void) {
    reset(2);
    WInput in = base_input(); step(&in, WR_MANUAL, 3);
    pull(&in, WR_MANUAL); sim.alive = 0; step(&in, WR_MANUAL, 90 + 10);
    CHECK(S->base.phase == G_IDLE && S->base.mag_rounds == 0);
    step(&in, WR_MANUAL, 200);
    CHECK(S->base.mag_rounds == 0 && S->base.channel[WCH_LID] < 0.01f);   /* no se recarga solo */
    WPose W = wcore_weapon_world(&S->base, &in);
    /* 1. abrir la tapa */
    in.hand[1].p = wpose_apply(W, wv(-0.045f, 0.10f, 0.545f)); in.grip[1] = 0; step(&in, WR_MANUAL, 2);
    in.grip[1] = 1; step(&in, WR_MANUAL, 2); in.grip[1] = 0;
    step(&in, WR_MANUAL, 60);
    CHECK(S->base.channel[WCH_LID] > 0.98f);
    /* 2. intentar insertar sin misil: nada */
    in.hand[1].p = wpose_apply(W, wv(0, 0.10f, 1.0f)); in.grip[1] = 1; step(&in, WR_MANUAL, 5);
    CHECK(S->base.mag_rounds == 0 && !S->hand_missile); in.grip[1] = 0; step(&in, WR_MANUAL, 2);
    /* 3. tomar misil de la espalda (mano por encima y detrás del hombro) */
    in.hand[1].p = wpose_apply(wcore_point_world(&S->base, &in, WP_GRIP), wv(0, 0, 0));
    in.hand[1].p = wv_add(in.head.p, wv(0.18f, 0.02f, 0.22f)); in.grip[1] = 0; step(&in, WR_MANUAL, 2);
    in.grip[1] = 1; step(&in, WR_MANUAL, 2);
    CHECK(S->hand_missile); CHECK(sim.reserve == 2);              /* aún no se descuenta */
    WPose hm; in.hand[1].q = W.q;
    CHECK(guided_hand_missile_pose(&S->base, &in, &hm));
    /* 4. soltar fuera de sitio = se pierde la mano (y se puede volver a coger) */
    in.grip[1] = 0; step(&in, WR_MANUAL, 2); CHECK(!S->hand_missile);
    in.grip[1] = 1; step(&in, WR_MANUAL, 2); CHECK(S->hand_missile);
    /* 5. desalineado: no entra */
    in.hand[1].q = qy(60.0f);
    in.hand[1].p = wpose_apply(W, wv(0, 0.10f, 0.97f)); step(&in, WR_MANUAL, 5);
    CHECK(S->ins == 0 && S->base.mag_rounds == 0);
    /* 6. alineado, empujar hacia delante por detrás */
    in.hand[1].q = W.q;
    for (float z = 1.4f; z > 0.9f; z -= 0.02f) {
        in.hand[1].p = wpose_apply(W, wv(0, 0.10f, z)); step(&in, WR_MANUAL, 2);
        if (z > 1.28f) CHECK(S->ins == 0);
    }
    CHECK(S->base.mag_rounds == 1); CHECK(sim.taken == 1); CHECK(sim.reserve == 1);
    CHECK(!S->hand_missile);
    /* 7. no se puede disparar con la tapa abierta */
    in.grip[1] = 0; step(&in, WR_MANUAL, 3);
    int f = sim.fired; pull(&in, WR_MANUAL); CHECK(sim.fired == f);
    /* 8. cerrar tapa */
    in.hand[1].p = wpose_apply(W, wv(-0.045f, 0.10f, 0.545f));
    in.grip[1] = 1; step(&in, WR_MANUAL, 2); in.grip[1] = 0; step(&in, WR_MANUAL, 60);
    CHECK(S->base.channel[WCH_LID] < 0.01f);
    in.hand[1].p = wv(0.1f, 1.2f, -0.2f);
    sim.alive = 0; pull(&in, WR_MANUAL); CHECK(sim.fired == f + 1);
    /* sin munición de reserva no se toma misil */
    reset(0); in = base_input(); step(&in, WR_MANUAL, 3); pull(&in, WR_MANUAL); sim.alive = 0; step(&in, WR_MANUAL, 100);
    W = wcore_weapon_world(&S->base, &in);
    in.hand[1].p = wpose_apply(W, wv(-0.045f, 0.10f, 0.545f)); in.grip[1] = 0; step(&in, WR_MANUAL, 2); in.grip[1] = 1; step(&in, WR_MANUAL, 40);
    in.grip[1] = 0; step(&in, WR_MANUAL, 2);
    in.hand[1].p = wv_add(in.head.p, wv(0.18f, 0.02f, 0.22f)); in.grip[1] = 1; step(&in, WR_MANUAL, 3);
    CHECK(!S->hand_missile);
}

static void t_overlay(void) {
    reset(3);
    enum { W = 320, H = 240 };
    static uint32_t px[W * H], ref[W * H];
    WInput in = base_input(); step(&in, WR_MANUAL, 5);
    /* reposo */
    synth_bg(px, W, H, 0); guided_draw_overlay(&S->base, px, W, H); write_ppm("idle.ppm", px, W, H);
    pull(&in, WR_MANUAL);
    for (int i = 0; i < 150; i++) { sim.proj_z -= 0.4f; step(&in, WR_MANUAL, 1); }
    in.hand[0].q = qmul_(qy(-9.0f), qx(5.0f)); step(&in, WR_MANUAL, 40);
    synth_bg(px, W, H, S->flight_t); memcpy(ref, px, sizeof px);
    guided_draw_overlay(&S->base, px, W, H); write_ppm("flight.ppm", px, W, H);
    int diff = 0; for (int i = 0; i < W * H; i++) diff += px[i] != ref[i];
    CHECK(diff > W * H / 2);
    const uint8_t *c = (const uint8_t *)&px[(H / 2 - 12) * W + W / 2];   /* trazo vertical de la retícula */
    CHECK(c[1] > 150 && c[1] > c[0] && c[1] > c[2]);
    for (int i = 0; i < W * H; i++) if (((px[i] >> 24) & 255) != 255) { CHECK(0); break; }
    /* resolución mayor no revienta */
    static uint32_t big[640 * 480]; synth_bg(big, 640, 480, 1); guided_draw_overlay(&S->base, big, 640, 480);
    write_ppm("flight_640.ppm", big, 640, 480);
    sim.alive = 0; step(&in, WR_MANUAL, 25);
    CHECK(S->base.phase == G_STATIC);
    guided_draw_overlay(&S->base, px, W, H); write_ppm("static.ppm", px, W, H);
    step(&in, WR_MANUAL, 100);
    guided_draw_overlay(&S->base, px, W, H); write_ppm("idle2.ppm", px, W, H);
}

static void t_equip(void) {
    /* gatillo apretado al equipar: no lanza; con dt inválido no avanza ni rompe nada */
    reset(3);
    WInput in = base_input(); in.trigger[0] = 1;
    step(&in, WR_MANUAL, 90);
    CHECK(sim.fired == 0); CHECK(S->base.phase == G_IDLE && S->base.mag_rounds == 1);
    in.trigger[0] = 0; step(&in, WR_MANUAL, 3); in.trigger[0] = 1; step(&in, WR_MANUAL, 3);
    CHECK(sim.fired == 1);
    reset(3); in = base_input();
    const float dts[] = {0.0f, -1.0f, NAN, INFINITY, 7.0f};
    for (unsigned k = 0; k < sizeof dts / sizeof *dts; k++) { in.dt = dts[k]; step(&in, WR_MANUAL, 5); }
    CHECK(isfinite(S->base.recoil_offset.p.x) && isfinite(S->view_blend) && isfinite(S->base.channel[WCH_LID]));
    /* sin estado global: dos instancias independientes; y el binding sobrevive a un init posterior */
    static char b2[sizeof(GuidedState)] __attribute__((aligned(16))); memset(b2, 0, sizeof b2);
    GuidedState *S2 = (GuidedState *)b2;
    w_guided.init(&S2->base);
    static WMesh ms[8]; ms[GP_ROUND].nv = 3; ms[GP_ROUND].name = "round";
    weapon_bind(&S2->base, ms, 8, NULL, 0);
    w_guided.init(&S2->base);
    int n; CHECK(weapon_meshes(&S2->base, &n) == ms && n == 8);
    CHECK(weapon_meshes(&S->base, &n) != ms);
    CHECK(wcore_part_find(&S->base, "round") == GP_ROUND);
    CHECK(weapon_part_mesh(&S2->base, GP_ROUND) == GP_ROUND && weapon_part_mesh(&S2->base, GP_TUBE) == GP_TUBE);
}

int main(void) {
    t_equip();
    t_proximity(); t_flight(); t_dry();
    t_reload_auto(WR_AUTOMATIC, 1); t_reload_auto(WR_SIMPLIFIED, 0); t_reload_manual(); t_overlay();
    printf("%d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
