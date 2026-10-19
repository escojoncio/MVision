/* test_aa_run: prueba de integración del apuntado del lanzador antiaéreo con el host completo (vrgame + vr_fire + vr_aa + HLE) sobre el
 * juego real en MV_NOGEN, sin escenas ni render. Parte de bodyrun (mismo gancho de VBlank determinista) y de gptest (equipo por RAM).
 *   test_aa_run <exe> <disco> [--frames N] [--tgt DIST] [--aim T0:T1] [--fire T] [--off GRADOS] [--noblock]
 * Mueve la mano del arma (mando derecho sintético) a la posición de "visor al ojo" entre T0 y T1 (VBlanks desde que el jugador es
 * controlable), pone un blanco sintético de bando enemigo a DIST metros por delante de la cabeza (solo RAM de la copia de prueba) y aprieta el
 * gatillo en T. --off GRADOS gira el tubo ese ángulo a la izquierda respecto a la dirección del blanco. MV_SIGS = firmas. Sin datos del juego. */
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
#include "vr_aa.h"
#include "vr_camera.h"
#include "vr_fire.h"
#include "vrgame.h"
#include "vrhost.h"

static uint32_t nframes = 9000, t0_vbl;
static int have_t0, inctl_run;
static int aim_t0 = 40, aim_t1 = 400, fire_t = 200, no_target;
static float tgt_dist = 7.0f, off_deg;
static int tgt_slot = -1;

static int16_t r16(uint32_t a) { int16_t v; memcpy(&v, psx_ram + (a & 0x1FFFFF), 2); return v; }
static uint32_t r32(uint32_t a) { uint32_t v; memcpy(&v, psx_ram + (a & 0x1FFFFF), 4); return v; }
static void w16(uint32_t a, int v) { int16_t x = (int16_t)v; memcpy(psx_ram + (a & 0x1FFFFF), &x, 2); }

static uint16_t std_pad(uint32_t v) {
    uint16_t b = 0;
    if ((v >= 120 && v < 126) || (v >= 400 && v < 406)) b |= 1u << 3;
    if (v >= 97 && v % 97 < 6) b |= 1u << 13;
    if (v >= 211 && v % 211 < 6) b |= 1u << 7;
    return b;
}
static void set_head(void) {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1.65f, 0, 1};
    mv_vr_set_head(m, 0.0);
}
/* equipo de prueba: todas las armas con munición, lanzador antiaéreo equipado (variables de partida del puente) */
static void equip(void) {
    static const int ammo[10] = {60, 120, 6, 6, 6, 4, 4, 3, 3, 20}, mx[10] = {200, 400, 10, 10, 10, 10, 10, 10, 10, 100};
    for (int i = 0; i < 10; i++) { w16(BA_VARS + 2 * (BRIDGE_V_WEAPON_AMMO + i), ammo[i]); w16(BA_VARS + 2 * (BRIDGE_V_WEAPON_MAX + i), mx[i]); }
    w16(BA_VARS + 2 * BRIDGE_V_WEAPON, 4);
}
/* cuaternión Ry(grados) (+ = izquierda) */
static void qy(float deg, float q[4]) { float h = deg * 3.14159265f / 360.0f; q[0] = 0; q[1] = sinf(h); q[2] = 0; q[3] = cosf(h); }

static void put_target(const BridgeState *bs) {
    VrFireMap map;
    vrfire_map_from_cam(&map, vrcam_last_record());
    if (!map.valid || !bs) return;
    float p[3] = {0, 1.55f, -tgt_dist}, w[3];
    vrfire_pos_to_world(&map, p, w);
    int n = (int)r32(BA_N_TARGETS);
    if (tgt_slot < 0) {
        for (int i = 63; i >= 0; i--) if (r16(BA_TARGET_ARRAY + 72u * (uint32_t)i) == 0) { tgt_slot = i; break; }
        if (tgt_slot < 0) return;
    }
    uint32_t a = BA_TARGET_ARRAY + 72u * (uint32_t)tgt_slot;
    uint32_t pmap = r32(0x800ACB9Cu);
    w16(a + 0, 0x1F); w16(a + 2, 2); w16(a + 4, (int)(pmap ? pmap : 1)); w16(a + 6, 0);
    for (int i = 0; i < 3; i++) w16(a + 8 + 2u * (uint32_t)i, (int)lrintf(w[i]));
    w16(a + 0x10, 300); w16(a + 0x12, 400); w16(a + 0x14, 300); w16(a + 0x26, 100);
    if (n < tgt_slot + 1) { uint32_t v = (uint32_t)(tgt_slot + 1); memcpy(psx_ram + (BA_N_TARGETS & 0x1FFFFF), &v, 4); }
}

static uint32_t g_missile; static int g_moff = -1;
static void probe(int kind, int decision, uint32_t res, const VrFireMat *m) {
    if (kind != 1) return;
    fprintf(stderr, "[misil] creado actor=%08x decision=%s t=(%d,%d,%d)\n", res, decision == 1 ? "SUSTITUIDA" : decision == -1 ? "suprimida" : "pasa", m->t[0], m->t[1], m->t[2]);
    g_missile = res; g_moff = -1;
    for (uint32_t o = 0; o < 0x200; o += 2) if (!memcmp(psx_ram + ((res + o) & 0x1FFFFF), m->m, 18)) { g_moff = (int)o; break; }
}

static uint16_t driver(uint32_t vbl, uint16_t pad, const BridgeState *bs) {
    (void)pad;
    int ctl = bs && bs->in_control && bs->has_player && (bs->valid & BRIDGE_OK_ALL_CORE) == BRIDGE_OK_ALL_CORE && vrcam_active();
    set_head();
    if (!have_t0) {
        inctl_run = ctl ? inctl_run + 1 : 0;
        if (inctl_run >= 30) { have_t0 = 1; t0_vbl = vbl; fprintf(stderr, "[aa] T0 = VBlank %u\n", vbl); vrfire_set_probe(probe); }
        else return std_pad(vbl);
    }
    int t = (int)(vbl - t0_vbl);
    if (t == 3) equip();
    if (t >= 10 && !no_target) put_target(bs);
    /* mano del arma (derecha): "visor al ojo" (punto de visor del lanzador a ~0,1 m por delante del ojo) o a la cadera */
    MvVrHand h; memset(&h, 0, sizeof h);
    h.valid = 1;
    int aiming = t >= aim_t0 && t < aim_t1;
    if (aiming) { h.pos[0] = 0.0f; h.pos[1] = 1.455f; h.pos[2] = 0.55f; }
    else { h.pos[0] = 0.25f; h.pos[1] = 1.0f; h.pos[2] = -0.1f; }
    qy(aiming ? off_deg : 0.0f, h.rot);
    h.trigger = (t >= fire_t && t < fire_t + 6) ? 1.0f : 0.0f;
    mv_vrgame_set_hand(1, &h);
    MvVrHand l; memset(&l, 0, sizeof l);          /* mano izquierda sin seguimiento */
    mv_vrgame_set_hand(0, &l);

    VrAaLock lk; vraa_lock_info(&lk);
    static int last_t = -1000, last_tg = -1, last_al = -1;
    if (lk.alive != last_al || (int)lk.target != last_tg || t - last_t >= 20) {
        MvVrGameStatus st; mv_vrgame_status(&st);
        fprintf(stderr, "[aa] t=%d arma=%d vr_activa=%d cargador=%d visor=%d buscador(vivo=%d blanco=%08x cuadros=%d cerrado=%d cand=%d pasos=%d vr=%d) blanco_global=%08x disparos=%u\n",
                t, bs ? bs->weapon : -1, st.active, st.loader, vraa_aiming(), lk.alive, lk.target, lk.frames, lk.converged, lk.candidates, lk.steps, lk.swapped, r32(0x800BF080u), st.shots);
        last_t = t; last_al = lk.alive; last_tg = (int)lk.target;
    }
    if (g_missile && g_moff >= 0 && bs) {
        uint32_t o = g_missile + (uint32_t)g_moff + 20;
        int32_t px = (int32_t)r32(o), py = (int32_t)r32(o + 4), pz = (int32_t)r32(o + 8);
        VrFireMap map; vrfire_map_from_cam(&map, vrcam_last_record());
        float p[3] = {0, 1.55f, -tgt_dist}, w[3] = {0, 0, 0};
        if (map.valid) vrfire_pos_to_world(&map, p, w);
        static int lastlog = -1000;
        if (t - lastlog >= 4) {
            lastlog = t;
            double d = sqrt((double)(px - w[0]) * (px - w[0]) + (double)(py - w[1]) * (py - w[1]) + (double)(pz - w[2]) * (pz - w[2]));
            fprintf(stderr, "[misil] t=%d pos=(%d,%d,%d) dist_blanco=%.0f edad=%d\n", t, px, py, pz, d, (int)r32(g_missile + 0x120));
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: %s <exe> <disco> [--frames N] [--tgt DIST] [--aim T0:T1] [--fire T] [--off GRADOS] [--noblock]\n", argv[0]); return 1; }
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) nframes = (uint32_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--tgt") && i + 1 < argc) tgt_dist = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--aim") && i + 1 < argc) sscanf(argv[++i], "%d:%d", &aim_t0, &aim_t1);
        else if (!strcmp(argv[i], "--fire") && i + 1 < argc) fire_t = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--off") && i + 1 < argc) off_deg = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--noblock")) no_target = 1;
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
    VrAaLock lk; vraa_lock_info(&lk);
    VrFireStats fs; vrfire_stats(&fs);
    MvVrGameStatus st; mv_vrgame_status(&st);
    fprintf(stderr, "[aa] fin: pasos del buscador=%d con la vista del visor=%d disparos VR=%u peticiones=%u sustituidos=%u\n", lk.steps, lk.swapped, st.shots, fs.requests, fs.subst);
    return 0;
}
