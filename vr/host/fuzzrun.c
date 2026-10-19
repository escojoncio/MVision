/* fuzzrun: ya en partida, alimenta a la capa de manos/armas/recarga/disparo/IK con entradas aleatorias y hostiles (NaN, infinitos, enormes,
 * gatillos y agarres de ambas manos cerca de los umbrales, cambio de arma y munición al azar, modos de recarga) durante N VBlanks, sobre el juego
 * real en MV_NOGEN. Pensado para compilarse con -fsanitize=address,undefined: cualquier aborto, desbordamiento o conversión float->entero
 * inválida se detecta. No lleva datos del juego.
 *   fuzzrun <exe> <disco> [--frames N] [--seed S] [--sane] [--ctl N]
 * --sane: sin NaN/Inf (solo ruido de umbral y movimientos extremos pero finitos). MV_SIGS = firmas. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "bridge.h"
#include "bridge_addrs.h"
#include "host.h"
#include "sys.h"
#include "vr_camera.h"
#include "vrgame.h"
#include "vrhost.h"

static uint32_t nframes = 12000, t0_vbl, rs = 1;
static int have_t0, inctl_run, sane, ctl_n = 30;

static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }
static float frand(float a, float b) { return a + (b - a) * (float)(rnd() & 0xFFFF) / 65535.0f; }
static float special(void) {
    static const float sp[] = {0, 1, -1, 0.5f, 0.55f, 0.45f, 0.52f, 0.60f, 1e30f, -1e30f, NAN, INFINITY, -INFINITY, 1e-30f, 1e9f};
    int n = sane ? 8 : 15;
    return sp[rnd() % (unsigned)n];
}
static void w16(uint32_t a, int v) { int16_t x = (int16_t)v; memcpy(emu_ram + (a & 0x1FFFFF), &x, 2); }

/* Misma secuencia que el recorrido estándar de padrun (títulos, menús, dificultad, aviso sin tarjeta, escena y radio: CRUZ a toques). */
static uint16_t std_pad(uint32_t v) {
    static const uint32_t st[] = {120, 200, 302, 450}, ok[] = {540, 640, 790, 1500};
    for (int i = 0; i < 4; i++) {
        if (v >= st[i] && v < st[i] + 6) return 1u << 3;
        if (v >= ok[i] && v < ok[i] + 6) return 0x6000;
    }
    if (v >= 740 && v < 746) return 1u << 7;
    if (v >= 1720 && v % 10 < 3) return 1u << 14;
    return 0;
}
static float num(int hostile) { return hostile ? special() : frand(-1.5f, 1.5f); }
static void rand_hand(MvVrHand *h, int hostile) {
    memset(h, 0, sizeof *h);
    h->valid = (rnd() % 10) != 0;
    h->pos[0] = frand(-0.7f, 0.7f); h->pos[1] = frand(0.2f, 2.0f); h->pos[2] = frand(-0.8f, 0.5f);
    for (int i = 0; i < 4; i++) h->rot[i] = frand(-1, 1);
    float n = sqrtf(h->rot[0] * h->rot[0] + h->rot[1] * h->rot[1] + h->rot[2] * h->rot[2] + h->rot[3] * h->rot[3]);
    if (n > 1e-4f) for (int i = 0; i < 4; i++) h->rot[i] /= n; else h->rot[3] = 1;
    static const float th[] = {0, 0.52f, 0.60f, 1.0f, 0.5f, 0.55f, 0.45f, 0.3f, 0.7f, 0.85f};
    h->trigger = th[rnd() % 10]; h->grip = th[rnd() % 10];
    h->low = rnd() & 1; h->up = rnd() & 1; h->stick = (rnd() % 8) == 0;
    h->aim_valid = rnd() & 1;
    for (int i = 0; i < 3; i++) h->aim_pos[i] = h->pos[i] + frand(-0.1f, 0.1f);
    for (int i = 0; i < 4; i++) h->aim_rot[i] = h->rot[i];
    if (hostile && (rnd() % 6) == 0) {         /* campos corruptos */
        switch (rnd() % 6) {
        case 0: for (int i = 0; i < 3; i++) h->pos[i] = special(); break;
        case 1: for (int i = 0; i < 4; i++) h->rot[i] = special(); break;
        case 2: h->trigger = special(); h->grip = special(); break;
        case 3: for (int i = 0; i < 4; i++) h->aim_rot[i] = special(); break;
        case 4: for (int i = 0; i < 3; i++) h->aim_pos[i] = special(); break;
        default: memset(h->rot, 0, sizeof h->rot); break;     /* cuaternión nulo */
        }
    }
    if (!hostile && (rnd() % 6) == 0) for (int i = 0; i < 3; i++) h->pos[i] = frand(-30, 30);
    (void)num;
}
static void equip(int wpn) {
    for (int i = 0; i < 10; i++) { w16(BA_VARS + 2 * (BRIDGE_V_WEAPON_AMMO + i), (int)(rnd() % 40)); w16(BA_VARS + 2 * (BRIDGE_V_WEAPON_MAX + i), 100); }
    w16(BA_VARS + 2 * BRIDGE_V_WEAPON, wpn);
}
static void set_head(void) {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1.65f, 0, 1};
    if (!sane && (rnd() % 50) == 0) m[13] = special();
    else if (rnd() % 4 == 0) m[13] = frand(0.4f, 2.0f);
    mv_vr_set_head(m, 0.0);
}
static uint32_t shots_seen;
static uint16_t driver(uint32_t vbl, uint16_t pad, const BridgeState *bs) {
    (void)pad;
    int ctl = bs && bs->in_control && bs->has_player && (bs->valid & BRIDGE_OK_ALL_CORE) == BRIDGE_OK_ALL_CORE && vrcam_active();
    if (!have_t0) {
        set_head();
        inctl_run = ctl ? inctl_run + 1 : 0;
        if (inctl_run >= ctl_n) { have_t0 = 1; t0_vbl = vbl; fprintf(stderr, "[fuzz] T0 = VBlank %u\n", vbl); }
        else return std_pad(vbl);
    }
    uint32_t t = vbl - t0_vbl;
    set_head();
    if (t % 90 == 0) { equip((int)(rnd() % 10)); mv_vrgame_set_options((int)(rnd() & 1), (int)(rnd() % 3)); }
    if (t % 400 == 0) {                       /* a veces objeto/postura/vida al azar en la RAM del juego */
        w16(BA_VARS + 2 * BRIDGE_V_ITEM, (int)(rnd() % 12));
        w16(BA_VARS + 2 * BRIDGE_V_STANCE, (int)(rnd() % 3));
    }
    MvVrHand h;
    for (int side = 0; side < 2; side++) {
        if (!sane && (rnd() % 200) == 0) { mv_vrgame_clear_hands(); continue; }
        rand_hand(&h, !sane);
        mv_vrgame_set_hand(side, &h);
    }
    MvVrForearm fa; mv_vrgame_forearm(rnd() & 1, &fa);
    float m[16]; mv_vrgame_hand_matrix(rnd() & 1, m); mv_vrgame_debug_frame((int)(rnd() % 3), (int)(rnd() & 1), m);
    float hl, hr; if (t % 10 == 0) mv_vrgame_take_haptics(&hl, &hr);
    MvVrSpecialView sv; mv_vrgame_special_view(&sv);
    if (t % 500 == 0) { MvVrGameStatus st; mv_vrgame_status(&st); if (st.shots != shots_seen) { shots_seen = st.shots; } fprintf(stderr, "[fuzz] t=%u arma=%d disparos=%u\n", t, st.weapon, st.shots); }
    return (rnd() % 4 == 0) ? (uint16_t)rnd() : 0;     /* mando físico aleatorio por encima */
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: %s <exe> <disco> [--frames N] [--seed S] [--sane] [--ctl N]\n", argv[0]); return 1; }
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) nframes = (uint32_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) rs = (uint32_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--ctl") && i + 1 < argc) ctl_n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sane")) sane = 1;
    }
    const char *sigs = getenv("MV_SIGS");
    if (sigs) mv_host_set_sigs(sigs);
    mkdir("build-host", 0755);
    mkdir("build-host/card", 0755);
    mv_vrhost_install();
    mv_vrgame_set_options(1, 0);
    vrgame_set_driver(driver);
    mv_host_set_turbo(1);
    int r = mv_host_start(argv[1], argv[2], "build-host/card");
    if (r) { fprintf(stderr, "mv_host_start = %d: %s\n", r, mv_host_error()); return 1; }
    for (;;) {
        if (mv_host_state() == MV_HOST_ERROR) { fprintf(stderr, "error del motor: %s\n", mv_host_error()); return 1; }
        if (mv_host_frame_count() >= nframes) break;
        usleep(2000);
    }
    mv_host_pause();
    usleep(100000);
    fprintf(stderr, "[fuzz] OK: %s, %u VBlanks, T0=%u\n", have_t0 ? "fin" : "NO se llegó a la partida", nframes, t0_vbl);
    return have_t0 ? 0 : 2;
}
