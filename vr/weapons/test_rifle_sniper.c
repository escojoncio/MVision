/* Pruebas de fusil y francotirador: vaciado, ráfaga, recarga manual/simplificada/automática y mira.
 * Compilar: cc -I. test_rifle_sniper.c w_rifle.c w_sniper.c w_gunc.c wcore.c -lm
 * Con argumento "png-dir" vuelca PPM de la retícula (en /home/claude/work/w2, fuera del repo). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "w_rifle.h"
#include "w_sniper.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FALLO %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

typedef struct { int reserve, fired, dry, magin, magout, back, fwd; int last_fire_n; WPose muz; } Br;
static Br B;
static int b_res(int id) { (void)id; return B.reserve; }
static void b_take(int id, int n) { (void)id; B.reserve -= n; }
static void b_fire(int id, WPose p) { (void)id; B.fired++; B.muz = p; }
static void b_hap(int h, float k, float t) { (void)h; (void)k; (void)t; }
static void b_snd(int id, int e) { (void)id;
    if (e == WS_DRY) B.dry++; else if (e == WS_MAG_IN) B.magin++; else if (e == WS_MAG_OUT) B.magout++;
    else if (e == WS_SLIDE_BACK) B.back++; else if (e == WS_SLIDE_FWD) B.fwd++; }
static WGameBridge BR = { .ammo_reserve = b_res, .ammo_take = b_take, .fire = b_fire, .haptic = b_hap, .sound = b_snd };

static WInput mk(void) {
    WInput in; memset(&in, 0, sizeof in);
    in.head.q.w = 1; in.head.p = (WVec3){0, 1.7f, 0};
    for (int i = 0; i < 2; i++) in.hand[i].q.w = 1;
    in.hand[0].p = (WVec3){0.2f, 1.3f, -0.4f};
    in.hand[1].p = (WVec3){-0.2f, 1.2f, -0.6f};
    in.dt = 1.0f / 90;
    return in;
}
static WeaponState *mkw(const WeaponDef *d) {
    WeaponState *s = calloc(1, d->state_size); s->def = d; d->init(s); return s;
}
static void step(WeaponState *s, WInput *in, WReloadMode rm, int n) { for (int i = 0; i < n; i++) s->def->update(s, in, rm, &BR); }
static void put_hand_on(WeaponState *s, WInput *in, int h, WPoint p, WVec3 off) {
    WPose pw = wcore_point_world(s, in, p); in->hand[h].p = wv_add(pw.p, off);
}

static void test_burst(void) {
    memset(&B, 0, sizeof B); B.reserve = 200;
    WeaponState *s = mkw(&w_rifle); WInput in = mk();
    step(s, &in, WR_MANUAL, 2);
    CHECK(s->mag_rounds == 24 && s->chambered && B.reserve == 175);
    in.trigger[0] = 1;
    float t = 0; int n0 = 0;
    for (int i = 0; i < 90; i++) { s->def->update(s, &in, WR_MANUAL, &BR); t += in.dt; if (B.fired == 12 && !n0) n0 = i; }
    /* 1 s a 950 rpm ~ 15-16 disparos */
    printf("rifle: %d disparos en 1 s (rpm medida %.0f)\n", B.fired, B.fired * 60.0f);
    CHECK(B.fired >= 14 && B.fired <= 17);
    float pitch1 = wv_len(wquat_to_rotvec(s->recoil_offset.q));
    CHECK(s->recoil_offset.p.z > 0.003f || s->recoil_offset.p.z < -0.003f);
    /* vaciado: sigue hasta quedarse sin balas */
    step(s, &in, WR_MANUAL, 90 * 3);
    CHECK(B.fired == 25 && !s->chambered && s->mag_rounds == 0);
    CHECK(s->channel[WCH_BOLT] == 1.0f);        /* cerrojo retenido atrás */
    int dry0 = B.dry;
    in.trigger[0] = 0; step(s, &in, WR_MANUAL, 5); in.trigger[0] = 1; step(s, &in, WR_MANUAL, 5);
    CHECK(B.dry > dry0 && B.fired == 25);
    /* una mano vs dos manos: subida tras una ráfaga de 12 disparos */
    float ang[2];
    for (int th = 0; th < 2; th++) {
        free(s); s = mkw(&w_rifle); in = mk(); B.reserve = 200; in.support_held = th; step(s, &in, WR_MANUAL, 2); in.trigger[0] = 1;
        float mx = 0; int sh = B.fired;
        for (int i = 0; i < 70; i++) { s->def->update(s, &in, WR_MANUAL, &BR); float a = wv_len(wquat_to_rotvec(s->recoil_offset.q)); if (a > mx) mx = a; }
        ang[th] = mx; (void)sh;
    }
    printf("rifle: subida maxima 1 mano %.2f grados, 2 manos %.2f grados\n", ang[0] * 57.3f, ang[1] * 57.3f);
    CHECK(ang[1] < ang[0] * 0.7f && ang[0] > 0.03f);
    (void)pitch1; free(s);
}

static void reload_manual(const WeaponDef *d, int cap) {
    memset(&B, 0, sizeof B); B.reserve = 100;
    WeaponState *s = mkw(d); WInput in = mk();
    step(s, &in, WR_MANUAL, 2);
    in.trigger[0] = 1;
    int guard = 0;
    while (B.fired < cap && guard++ < 4000) { step(s, &in, WR_MANUAL, 1); if (d == &w_sniper) { in.trigger[0] = (guard & 8) ? 1 : 0; } }
    in.trigger[0] = 0;
    step(s, &in, WR_MANUAL, 3);
    CHECK(B.fired == cap && !s->chambered && s->mag_rounds == 0);
    int res0 = B.reserve;
    /* 1) mano libre a la boca del alojamiento, soltar cargador agarrándolo */
    put_hand_on(s, &in, 1, WP_MAGWELL, wv(0, 0, 0));
    in.grip[1] = 0.9f; in.buttons[1] = WB_MAG_RELEASE; step(s, &in, WR_MANUAL, 1); in.buttons[1] = 0;
    CHECK(!s->mag_inserted && B.magout == 1);
    /* 2) tirarlo lejos de la cintura */
    in.hand[1].p = wv_add(in.hand[1].p, wv(-0.3f, 0, 0)); in.grip[1] = 0; step(s, &in, WR_MANUAL, 3);
    WPose lm; int loose = (d == &w_rifle) ? rifle_loose_mag(s, &lm) : sniper_loose_mag(s, &lm);
    CHECK(loose);
    step(s, &in, WR_MANUAL, 90 * 4);
    loose = (d == &w_rifle) ? rifle_loose_mag(s, &lm) : sniper_loose_mag(s, &lm);
    CHECK(!loose);
    /* 3) coger cargador nuevo en la cintura */
    float zr; WVec3 zc = gun_waist_zone(&in, &zr);
    in.hand[1].p = zc; in.grip[1] = 0; step(s, &in, WR_MANUAL, 1); in.grip[1] = 0.9f; step(s, &in, WR_MANUAL, 1);
    WPose hm; int held = (d == &w_rifle) ? rifle_held_mag(s, &in, &hm) : sniper_held_mag(s, &in, &hm);
    CHECK(held && B.reserve == res0 - cap);
    /* 4a) insertar mal orientado (al revés) -> no entra */
    WPose mw = wcore_point_world(s, &in, WP_MAGWELL);
    WQuat flip = wquat_axis_angle(wv(1, 0, 0), 3.14159f);
    in.hand[1].q = flip;
    in.hand[1].p = wv_add(mw.p, wv(0, -0.15f + 0.0f, 0)); /* parte alta (invertida) cerca */
    in.hand[1].p = wv_add(mw.p, wquat_rotate(flip, wv(0, -(0.5f * (d == &w_rifle ? 0.2f : 0.19f) + 0.03f), 0)));
    step(s, &in, WR_MANUAL, 3);
    CHECK(!s->mag_inserted);
    /* 4b) bien alineado */
    in.hand[1].q = (WQuat){0, 0, 0, 1};
    in.hand[1].p = wv_add(mw.p, wv(0, -(0.5f * (d == &w_rifle ? 0.2f : 0.19f) + 0.03f) + 0.02f, 0));
    step(s, &in, WR_MANUAL, 3);
    CHECK(s->mag_inserted && s->mag_rounds == cap && B.magin == 1);
    CHECK(s->phase == GP_NEEDCHARGE);
    /* sin tirar de la palanca no dispara */
    int f0 = B.fired; in.trigger[0] = 1; step(s, &in, WR_MANUAL, 5); in.trigger[0] = 0;
    CHECK(B.fired == f0);
    /* 5) palanca: agarrar, tirar atrás, soltar */
    in.grip[1] = 0; step(s, &in, WR_MANUAL, 1);
    in.hand[1].q = (WQuat){0, 0, 0, 1};
    put_hand_on(s, &in, 1, WP_CHARGE, wv(0, 0, 0));
    in.grip[1] = 0.9f; step(s, &in, WR_MANUAL, 1);
    for (int i = 0; i < 12; i++) { in.hand[1].p.z += 0.007f; step(s, &in, WR_MANUAL, 1); }
    CHECK(B.back == 1);
    in.grip[1] = 0; step(s, &in, WR_MANUAL, 2);
    CHECK(s->chambered && s->mag_rounds == cap - 1 && s->phase == GP_READY && B.fwd >= 1);
    step(s, &in, WR_MANUAL, 12);
    in.trigger[0] = 1; step(s, &in, WR_MANUAL, 3);
    CHECK(B.fired == f0 + 1);
    printf("%s: recarga manual completa OK (reserva %d)\n", d->id, B.reserve);
    free(s);
}

static void test_auto_modes(void) {
    for (int m = 1; m <= 2; m++) {
        memset(&B, 0, sizeof B); B.reserve = 100;
        WeaponState *s = mkw(&w_rifle); WInput in = mk();
        step(s, &in, WR_MANUAL, 2);
        in.trigger[0] = 1; step(s, &in, m, 90 * 3); in.trigger[0] = 0;
        if (m == 1) {
            CHECK(s->mag_rounds == 0 && !s->chambered);
            in.buttons[1] = WB_MAG_RELEASE; step(s, &in, m, 1); in.buttons[1] = 0;
        }
        step(s, &in, m, 90 * 3);
        CHECK(s->mag_inserted && s->chambered && s->mag_rounds == 24);
        printf("rifle: recarga %s OK (reserva %d)\n", m == 1 ? "simplificada" : "automatica", B.reserve);
        free(s);
    }
}

/* Balas contabilizadas: reserva + cargador + recámara + disparadas = total inicial (sin cargadores sueltos). */
static int accounted(const WeaponState *s) { return B.reserve + s->mag_rounds + s->chambered + B.fired; }

/* Recarga temporizada partiendo SIN cargador montado, y reserva que pasa de 0 a >0. */
static void test_seq_nomag(const WeaponDef *d, int cap) {
    memset(&B, 0, sizeof B); B.reserve = 100;
    WeaponState *s = mkw(d); WInput in = mk();
    step(s, &in, WR_MANUAL, 2);
    int total = accounted(s);
    CHECK(total == 100);
    /* manual: soltar el cargador sin agarrarlo (cae y sus balas se pierden, documentado) */
    put_hand_on(s, &in, 1, WP_MAGWELL, wv(0, 0, 0));
    in.grip[1] = 0.1f; in.buttons[1] = WB_MAG_RELEASE; step(s, &in, WR_MANUAL, 1); in.buttons[1] = 0;
    CHECK(!s->mag_inserted);
    int perdidas = s->chambered;                       /* la recámara se conserva; el cargador soltado se perdió */
    (void)perdidas;
    in.hand[1].p = (WVec3){-0.4f, 1.2f, -0.6f}; step(s, &in, WR_MANUAL, 90 * 4);
    int res0 = B.reserve;
    /* simplificado: sin cargador, pulsar el botón lanza la secuencia entera y debe insertar uno */
    in.buttons[1] = WB_MAG_RELEASE; step(s, &in, WR_SIMPLIFIED, 1); in.buttons[1] = 0;
    step(s, &in, WR_SIMPLIFIED, 90 * 3);
    CHECK(s->mag_inserted && s->mag_rounds > 0);
    CHECK(B.reserve == res0 - cap);
    CHECK(B.magin >= 1);
    free(s);
    /* automático: sin cargador y sin reserva espera en silencio; al aparecer reserva recarga sola */
    memset(&B, 0, sizeof B); B.reserve = 100;
    s = mkw(d); in = mk(); step(s, &in, WR_MANUAL, 2);
    put_hand_on(s, &in, 1, WP_MAGWELL, wv(0, 0, 0));
    in.grip[1] = 0.1f; in.buttons[1] = WB_MAG_RELEASE; step(s, &in, WR_MANUAL, 1); in.buttons[1] = 0;
    in.hand[1].p = (WVec3){-0.4f, 1.2f, -0.6f};
    in.trigger[0] = 1; step(s, &in, WR_MANUAL, 90 * 2);              /* gasta la bala de la recámara */
    in.trigger[0] = 0; step(s, &in, WR_MANUAL, 10);
    CHECK(!s->mag_inserted && !s->chambered);
    B.reserve = 0; int dry0 = B.dry;
    step(s, &in, WR_AUTOMATIC, 90 * 6);
    CHECK(!s->mag_inserted && B.dry == dry0);                       /* silencio: ni seq ni DRY en bucle */
    B.reserve = 50;
    step(s, &in, WR_AUTOMATIC, 90 * 4);
    CHECK(s->mag_inserted && s->chambered && B.reserve == 50 - cap);
    CHECK(s->mag_rounds == cap - 1);
    free(s);
}

/* La palanca con bala viva la devuelve al contador: ni se pierde ni se duplica ninguna. */
static void test_bolt_ammo(const WeaponDef *d) {
    memset(&B, 0, sizeof B); B.reserve = 100;
    WeaponState *s = mkw(d); WInput in = mk();
    step(s, &in, WR_MANUAL, 2);
    CHECK(accounted(s) == 100);
    int r0 = B.reserve, m0 = s->mag_rounds;
    in.hand[1].q = (WQuat){0, 0, 0, 1};
    put_hand_on(s, &in, 1, WP_CHARGE, wv(0, 0, 0));
    in.grip[1] = 0; step(s, &in, WR_MANUAL, 1); in.grip[1] = 0.9f; step(s, &in, WR_MANUAL, 1);
    for (int i = 0; i < 12; i++) { in.hand[1].p.z += 0.007f; step(s, &in, WR_MANUAL, 1); }
    in.grip[1] = 0; step(s, &in, WR_MANUAL, 3);
    CHECK(s->chambered && s->mag_rounds == m0 - 1);
    CHECK(B.reserve == r0 + 1);
    CHECK(accounted(s) == 100);
    free(s);
}

/* Tras un bloqueo largo con el gatillo apretado no sale una ráfaga acumulada. */
static void test_cd_clamp(const WeaponDef *d) {
    memset(&B, 0, sizeof B); B.reserve = 100;
    WeaponState *s = mkw(d); WInput in = mk();
    step(s, &in, WR_MANUAL, 2);
    in.hand[1].q = (WQuat){0, 0, 0, 1};
    put_hand_on(s, &in, 1, WP_CHARGE, wv(0, 0, 0));
    in.grip[1] = 0; step(s, &in, WR_MANUAL, 1); in.grip[1] = 0.9f; step(s, &in, WR_MANUAL, 1);   /* agarra la palanca: bloquea el disparo */
    in.trigger[0] = 1; step(s, &in, WR_MANUAL, 90 * 4);
    CHECK(B.fired == 0);
    in.grip[1] = 0; step(s, &in, WR_MANUAL, 1);          /* suelta: libera el disparo */
    int f1 = B.fired;
    CHECK(f1 <= 1);                                     /* como mucho un disparo en el primer cuadro */
    step(s, &in, WR_MANUAL, 17);                        /* ~0,19 s */
    CHECK(B.fired <= 4);
    free(s);
    /* con el seguro del francotirador: tampoco hay disparo acumulado */
    memset(&B, 0, sizeof B); B.reserve = 100;
    s = mkw(&w_sniper); in = mk(); step(s, &in, WR_MANUAL, 2);
    in.buttons[0] = WB_STICK; step(s, &in, WR_MANUAL, 1); in.buttons[0] = 0;
    in.trigger[0] = 1; step(s, &in, WR_MANUAL, 90 * 3);
    CHECK(B.fired == 0);
    in.trigger[0] = 0; step(s, &in, WR_MANUAL, 2);
    free(s);
}

static void test_equip_trigger(const WeaponDef *d) {
    memset(&B, 0, sizeof B); B.reserve = 100;
    WeaponState *s = mkw(d); WInput in = mk();
    in.trigger[0] = 1; step(s, &in, WR_MANUAL, 90);
    CHECK(B.fired == 0);                                /* apretado al equipar: no dispara */
    in.trigger[0] = 0; step(s, &in, WR_MANUAL, 5);
    in.trigger[0] = 1; step(s, &in, WR_MANUAL, 10);
    CHECK(B.fired >= 1);
    free(s);
}

static void test_dt_guns(void) {
    const WeaponDef *ds[2] = { &w_rifle, &w_sniper };
    for (int k = 0; k < 2; k++) {
        memset(&B, 0, sizeof B); B.reserve = 100;
        WeaponState *s = mkw(ds[k]); WInput in = mk();
        const float dts[] = {0.0f, -2.0f, NAN, INFINITY, 0.2f, 50.0f};
        for (unsigned i = 0; i < sizeof dts / sizeof *dts; i++) {
            in.dt = dts[i]; in.trigger[0] = (i & 1) ? 1.0f : 0.0f;
            step(s, &in, WR_AUTOMATIC, 4);
            CHECK(isfinite(s->recoil_offset.p.z) && isfinite(s->channel[WCH_BOLT]) && s->mag_rounds >= 0);
        }
        in.dt = 0; int r0 = B.reserve, f0 = B.fired; step(s, &in, WR_MANUAL, 20);
        CHECK(B.reserve == r0 && B.fired == f0);
        in.dt = 0.2f; in.trigger[0] = 0; step(s, &in, WR_MANUAL, 2); in.trigger[0] = 1; B.reserve = 100; f0 = B.fired;
        step(s, &in, WR_MANUAL, 3);                        /* dt grande: recortado a 0,1 s, sin ráfaga */
        CHECK(B.fired - f0 <= 6);
        free(s);
    }
}

static void test_overlay_iris(void) {
    WeaponState *s = mkw(&w_sniper);                    /* sin mira activa: iris cerrado (blend = 0) */
    enum { N = 320 };
    static uint32_t buf[N * N];
    for (int i = 0; i < N * N; i++) buf[i] = 0xFFC8C8C8u;
    sniper_draw_overlay(s, buf, N, N);
    int cx = N / 2 + 11, cy = N / 2 + 11;               /* r ~ 0,1 del radio, fuera de la retícula */
    uint32_t px = buf[cy * N + cx];
    CHECK((px & 255) >= 190);                           /* sin viñeta en el centro del iris cerrado */
    CHECK(((buf[5 * N + 5]) & 255) == 0);               /* fuera de la lente: negro */
    for (int i = 0; i < N * N; i++) if ((buf[i] >> 24) != 0xFF) { CHECK(0); break; }
    uint32_t one = 0xFF808080u; sniper_draw_overlay(s, &one, 1, 1); sniper_draw_overlay(s, NULL, 4, 4);
    sniper_draw_overlay(s, buf, 0, 0);
    free(s);
}

static void write_ppm(const char *dir, const char *name, const uint32_t *px, int w, int h) {
    char p[512]; snprintf(p, sizeof p, "%s/%s.ppm", dir, name);
    FILE *f = fopen(p, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { fputc(px[i] & 255, f); fputc((px[i] >> 8) & 255, f); fputc((px[i] >> 16) & 255, f); }
    fclose(f);
}

static void dump_scope(const char *dir, const char *name, const WeaponState *s) {
    if (!dir) return;
    enum { N = 320 };
    static uint32_t buf[N * N];
    float yw, pt; sniper_sway(s, &yw, &pt);
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
        float fx = x + yw * 6000, fy = y - pt * 6000;   /* la escena se desplaza con el balanceo */
        int sky = fy < N / 2 + 20 * sinf(fx * 0.03f);
        uint32_t r = sky ? 120 + 80 * y / N : 70 + 40 * (0.5f + 0.5f * sinf(fx * 0.08f));
        uint32_t g = sky ? 170 + 50 * y / N : 100 + 40 * (0.5f + 0.5f * sinf(fx * 0.08f));
        uint32_t b = sky ? 230 : 60;
        buf[y * N + x] = r | (g << 8) | (b << 16) | 0xFF000000u;
    }
    sniper_draw_overlay(s, buf, N, N);
    write_ppm(dir, name, buf, N, N);
}

static void test_scope(const char *dir) {
    memset(&B, 0, sizeof B); B.reserve = 100;
    WeaponState *s = mkw(&w_sniper); WInput in = mk();
    step(s, &in, WR_MANUAL, 2);
    CHECK(!s->aux.active);
    /* ojo derecho junto al ocular, mirando por él */
    WPose ep = wcore_point_world(s, &in, WP_EYE);
    in.head.p = wv_sub(ep.p, wv(0.032f, 0, 0)); in.head.p.z += 0.05f;
    step(s, &in, WR_MANUAL, 20);
    CHECK(s->aux.active && s->aux.target_part == SNIPER_PT_LENS && (s->aux.overlay & WO_RETICLE));
    dump_scope(dir, "scope_6x_rest", s);
    float fov6 = s->aux.fov_y;
    printf("sniper: mira ACTIVA a %.0f mm; %.3fx -> fov %.2f grados\n", 50.0, sniper_zoom(s), fov6 * 57.3f);
    CHECK(sniper_zoom(s) == SNIPER_ZOOM_GAME);                                   /* el aumento del original es el de por defecto */
    CHECK(fabsf(fov6 - 2 * atanf(128.0f / 3000.0f)) < 1e-5f);                    /* 3000/320 sobre el lienzo de 256 px */
    /* cámara en el objetivo, delante del ojo */
    CHECK(s->aux.camera.p.z < ep.p.z);
    /* histéresis: 12 cm sigue activa, 20 cm se apaga */
    in.head.p.z = ep.p.z + 0.12f; step(s, &in, WR_MANUAL, 3); CHECK(s->aux.active);
    in.head.p.z = ep.p.z + 0.20f; step(s, &in, WR_MANUAL, 3); CHECK(!s->aux.active);
    /* zoom */
    in.head.p.z = ep.p.z + 0.05f; step(s, &in, WR_MANUAL, 10);
    in.buttons[0] = WB_A; step(s, &in, WR_MANUAL, 1); in.buttons[0] = 0;
    CHECK(fabsf(sniper_zoom(s) - SNIPER_ZOOM_GAME * 0.5f) < 1e-4f);
    step(s, &in, WR_MANUAL, 40); dump_scope(dir, "scope_10x_sway", s);
    /* balanceo: amplitud en 6 s con 1 mano, 2 manos y tranquilizante */
    float amp[4];
    for (int c = 0; c < 4; c++) {
        in.support_held = (c == 1 || c == 3); sniper_set_steady(s, c >= 2 ? 1.0f : 0.0f);
        float mx = 0, y, p;
        for (int i = 0; i < 90 * 6; i++) { step(s, &in, WR_MANUAL, 1); sniper_sway(s, &y, &p); float a = hypotf(y, p); if (a > mx) mx = a; }
        amp[c] = mx;
    }
    printf("sniper: balanceo pico 1 mano %.2f mrad | 2 manos %.2f | tranq. %.2f | tranq.+2 manos %.2f\n",
           amp[0] * 1e3f, amp[1] * 1e3f, amp[2] * 1e3f, amp[3] * 1e3f);
    CHECK(amp[1] < amp[0] * 0.7f && amp[2] < amp[0] * 0.4f && amp[3] < amp[2]);
    in.head.p.z = ep.p.z + 0.13f; step(s, &in, WR_MANUAL, 4); dump_scope(dir, "scope_iris_closing", s);
    in.head.p.z = ep.p.z + 0.05f; step(s, &in, WR_MANUAL, 30);
    /* semiautomático: mantener el gatillo = un disparo; seguro bloquea */
    sniper_set_steady(s, 0); in.support_held = 0;
    int f0 = B.fired; in.trigger[0] = 1; step(s, &in, WR_MANUAL, 90); CHECK(B.fired == f0 + 1);
    in.trigger[0] = 0; step(s, &in, WR_MANUAL, 25);
    in.buttons[0] = WB_STICK; step(s, &in, WR_MANUAL, 1); in.buttons[0] = 0;
    in.trigger[0] = 1; step(s, &in, WR_MANUAL, 30); CHECK(B.fired == f0 + 1);
    in.trigger[0] = 0; in.buttons[0] = WB_STICK; step(s, &in, WR_MANUAL, 1); in.buttons[0] = 0; step(s, &in, WR_MANUAL, 25);
    free(s);
}

/* make_ready (puesta a punto del anfitrión en recarga automática): arma vacía con el cerrojo retenido y sin reserva (la recarga automática no puede arrancar) -> lista con las balas
 * indicadas, cerrojo cerrado, y dispara; no toca la contabilidad de munición. */
static void test_make_ready(const WeaponDef *d, int cap) {
    memset(&B, 0, sizeof B); B.reserve = cap;                               /* justo un cargador: tras montarlo no queda reserva */
    WeaponState *s = mkw(d); WInput in = mk();
    step(s, &in, WR_AUTOMATIC, 2);
    CHECK(s->chambered && s->mag_rounds == cap - 1 && B.reserve == 0);
    for (int k = 0; k < cap + 4 && (s->chambered || s->mag_rounds); k++) { in.trigger[0] = 1; step(s, &in, WR_AUTOMATIC, 90); in.trigger[0] = 0; step(s, &in, WR_AUTOMATIC, 30); }   /* vaciar (semiautomática o ráfaga) */
    in.trigger[0] = 0; step(s, &in, WR_AUTOMATIC, 180);
    CHECK(!s->chambered && s->mag_rounds == 0 && B.fired == cap);       /* muerta: sin reserva la recarga automática espera para siempre */
    CHECK(d->make_ready != NULL);
    d->make_ready(s, 7);
    CHECK(s->mag_inserted && s->chambered && s->mag_rounds == 7 && B.reserve == 0);
    step(s, &in, WR_AUTOMATIC, 30);
    CHECK(s->mag_inserted && s->chambered && s->mag_rounds == 7);           /* estable: ninguna secuencia a medias la deshace */
    CHECK(s->channel[WCH_BOLT] < 0.5f);                                     /* cerrojo cerrado */
    int f0 = B.fired;
    in.trigger[0] = 1; step(s, &in, WR_AUTOMATIC, 3); in.trigger[0] = 0; step(s, &in, WR_AUTOMATIC, 30);
    CHECK(B.fired > f0);                                                    /* vuelve a disparar */
    d->make_ready(s, -3);
    CHECK(s->mag_rounds == 0 && s->chambered);                              /* cuenta negativa: cargador a cero, bala en la recámara */
    free(s);
}

int main(int argc, char **argv) {
    test_burst();
    test_make_ready(&w_rifle, RIFLE_MAG); test_make_ready(&w_sniper, SNIPER_MAG);
    reload_manual(&w_rifle, RIFLE_MAG);
    reload_manual(&w_sniper, SNIPER_MAG);
    test_auto_modes();
    test_scope(argc > 1 ? argv[1] : 0);
    test_seq_nomag(&w_rifle, RIFLE_MAG); test_seq_nomag(&w_sniper, SNIPER_MAG);
    test_bolt_ammo(&w_rifle); test_bolt_ammo(&w_sniper);
    test_cd_clamp(&w_rifle);
    test_equip_trigger(&w_rifle); test_equip_trigger(&w_sniper);
    test_dt_guns();
    test_overlay_iris();
    printf(fails ? "RESULTADO: %d FALLOS\n" : "RESULTADO: todo OK\n", fails);
    return fails != 0;
}
