/* test_special_run: integración de las vistas especiales del arma (mira óptica, visor antiaéreo, pantalla del misil guiado) con el host completo
 * (vrgame + vr_fire + vr_aa + vr_missile + HLE) sobre el juego real en MV_NOGEN, sin render. Mismo gancho de VBlank determinista que test_aa_run.
 *   test_special_run <exe> <disco> --wpn sniper|aa|guided [--frames N] [--aim T0:T1] [--fire T] [--steer T0:T1:GRADOS] [--tgt DIST] [--tranq T]
 *                    [--fullscreen] [--hips] [--log N]
 * Equipa el arma, mueve la mano del arma (mando derecho sintético): a la cadera hasta --aim, "ojo en la mira" entre T0 y T1, gatillo en --fire,
 * y durante --steer gira la mano GRADOS a la derecha (el guiado del misil se hace inclinando el lanzador). Imprime cada --log VBlanks (50) el
 * descriptor que recibiría la app (mv_vrgame_special_view) y, para el misil, el actor del juego. MV_SIGS = firmas. Sin datos del juego en el repo. */
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
#include "vr_missile.h"
#include "vrgame.h"
#include "vrhost.h"

static uint32_t nframes = 12000, t0_vbl;
static int have_t0, inctl_run;
static int wpn = 9, aim_t0 = 40, aim_t1 = 100000, fire_t = -1, tq_t = -1, log_n = 50, fullscreen, hips;
static int steer_t0 = -1, steer_t1 = -1;
static float steer_deg, tgt_dist = 7.0f;
static int tgt_slot = -1, no_target;

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
static void equip(void) {
    static const int ammo[10] = {60, 120, 6, 6, 6, 4, 4, 3, 3, 20}, mx[10] = {200, 400, 10, 10, 10, 10, 10, 10, 10, 100};
    for (int i = 0; i < 10; i++) { w16(BA_VARS + 2 * (BRIDGE_V_WEAPON_AMMO + i), ammo[i]); w16(BA_VARS + 2 * (BRIDGE_V_WEAPON_MAX + i), mx[i]); }
    w16(BA_VARS + 2 * BRIDGE_V_WEAPON, wpn);
}
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

/* mano del arma (derecho): posición de "ojo en la mira" por arma (con la cabeza en (0, 1.65, 0)) */
static void eye_hand(float pos[3]) {
    if (wpn == 9) { pos[0] = 0.032f; pos[1] = 1.50f; pos[2] = -0.20f; }           /* ocular del francotirador (WP_EYE a (0, 0,1475, 0,135) del agarre) */
    else if (wpn == 3) { pos[0] = 0.095f + 0.032f; pos[1] = 1.47f; pos[2] = -0.30f; }   /* mirilla del lanzador guiado (WP_EYE a (-0,095, 0,176, 0,40)) */
    else { pos[0] = 0.0f; pos[1] = 1.455f; pos[2] = 0.55f; }                      /* visor del lanzador antiaéreo */
}

static uint32_t vbl_seen;
static void log_view(int t, const BridgeState *bs) {
    MvVrSpecialView v;
    int has = mv_vrgame_special_view(&v);
    MvVrGameStatus st; mv_vrgame_status(&st);
    VrMissile m; vrmis_get(&m);
    fprintf(stderr, "[vista] t=%d arma=%d vr=%d kind=%d blend=%.2f cam=%d fov=%.2f zoom=%.2f hud_off=%d marco_off=%d | surf=(%.2f,%.2f,%.2f) %.3fx%.3f redonda=%d | "
            "AA lock=%d cerrado=%d | misil vivo=%d fin=%d edad=%d pos=(%d,%d,%d) rumbo=%d | ruido=%.2f estatica=%d pantalla=%d zcap=%d disparos=%u\n",
            t, bs ? bs->weapon : -1, st.active, has ? v.kind : 0, has ? v.blend : 0, has ? v.cam_valid : 0, has ? v.cam_fov_y : 0, has ? v.zoom : 0, has ? v.hide_hud : 0, has ? v.suppress_frame : 0,
            v.surf_pos[0], v.surf_pos[1], v.surf_pos[2], v.surf_w, v.surf_h, v.round, has ? v.locked : 0, has ? v.converged : 0, m.alive, m.ended, m.age,
            m.pos[0], m.pos[1], m.pos[2], m.heading, has ? v.noise : 0, has ? v.ended : 0, has ? v.fullscreen : 0, 0, st.shots);
    if (has && v.kind == MV_VIEW_MISSILE && v.cam_valid && m.alive) {
        /* coherencia: la cámara de la vista (espacio de usuario) debe corresponder al actor del juego (mundo del motor) */
        VrFireMap map; vrfire_map_from_cam(&map, vrcam_last_record());
        if (map.valid) {
            float w[3]; vrfire_pos_to_world(&map, v.cam_pos, w);
            fprintf(stderr, "[vista]   cam usuario=(%.2f,%.2f,%.2f) -> mundo=(%.0f,%.0f,%.0f) vs actor=(%d,%d,%d) dif=%.1f u; rot=(%.2f,%.2f,%.2f,%.2f)\n",
                    v.cam_pos[0], v.cam_pos[1], v.cam_pos[2], w[0], w[1], w[2], m.pos[0], m.pos[1], m.pos[2],
                    sqrtf((w[0] - (float)m.pos[0]) * (w[0] - (float)m.pos[0]) + (w[1] - (float)m.pos[1]) * (w[1] - (float)m.pos[1]) + (w[2] - (float)m.pos[2]) * (w[2] - (float)m.pos[2])),
                    v.cam_rot[0], v.cam_rot[1], v.cam_rot[2], v.cam_rot[3]);
        }
    }
    if (has && v.kind == MV_VIEW_SCOPE && v.cam_valid)
        fprintf(stderr, "[vista]   mira: cam=(%.3f,%.3f,%.3f) rot=(%.3f,%.3f,%.3f,%.3f) fov=%.2f grados lente=(%.3f,%.3f,%.3f)\n", v.cam_pos[0], v.cam_pos[1], v.cam_pos[2],
                v.cam_rot[0], v.cam_rot[1], v.cam_rot[2], v.cam_rot[3], v.cam_fov_y * 57.29578f, v.surf_pos[0], v.surf_pos[1], v.surf_pos[2]);
    (void)vbl_seen;
}

static uint16_t driver(uint32_t vbl, uint16_t pad, const BridgeState *bs) {
    (void)pad;
    int ctl = bs && bs->in_control && bs->has_player && (bs->valid & BRIDGE_OK_ALL_CORE) == BRIDGE_OK_ALL_CORE && vrcam_active();
    set_head();
    if (!have_t0) {
        inctl_run = ctl ? inctl_run + 1 : 0;
        if (inctl_run >= 30) { have_t0 = 1; t0_vbl = vbl; fprintf(stderr, "[esp] T0 = VBlank %u\n", vbl); }
        else return std_pad(vbl);
    }
    int t = (int)(vbl - t0_vbl);
    if (t == 3) equip();
    if (wpn == 4 && t >= 10 && !no_target) put_target(bs);
    if (tq_t >= 0 && t >= tq_t && t < tq_t + 3000) w16(0x800B5FBAu, 1200);            /* tranquilizante activo (temporizador del juego) */
    MvVrHand h; memset(&h, 0, sizeof h);
    h.valid = 1;
    int aiming = !hips && t >= aim_t0 && t < aim_t1;
    float ep[3]; eye_hand(ep);
    if (aiming) { h.pos[0] = ep[0]; h.pos[1] = ep[1]; h.pos[2] = ep[2]; }
    else { h.pos[0] = 0.25f; h.pos[1] = 1.0f; h.pos[2] = -0.1f; }
    float yaw = (t >= steer_t0 && t < steer_t1) ? -steer_deg : 0.0f;     /* + deg = a la derecha: qy es + a la izquierda */
    qy(yaw, h.rot);
    h.trigger = (fire_t >= 0 && t >= fire_t && t < fire_t + 6) ? 1.0f : 0.0f;
    mv_vrgame_set_hand(1, &h);
    MvVrHand l; memset(&l, 0, sizeof l);
    mv_vrgame_set_hand(0, &l);
    mv_vrgame_set_missile_view(fullscreen);
    if (t >= 0 && t % log_n == 0) log_view(t, bs);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: %s <exe> <disco> --wpn sniper|aa|guided [--frames N] [--aim T0:T1] [--fire T] [--steer T0:T1:GRADOS] [--tgt DIST] [--tranq T] [--fullscreen] [--hips] [--log N]\n", argv[0]); return 1; }
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--wpn") && i + 1 < argc) { i++; wpn = !strcmp(argv[i], "aa") ? 4 : !strcmp(argv[i], "guided") ? 3 : 9; }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) nframes = (uint32_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--aim") && i + 1 < argc) sscanf(argv[++i], "%d:%d", &aim_t0, &aim_t1);
        else if (!strcmp(argv[i], "--fire") && i + 1 < argc) fire_t = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--steer") && i + 1 < argc) sscanf(argv[++i], "%d:%d:%f", &steer_t0, &steer_t1, &steer_deg);
        else if (!strcmp(argv[i], "--tgt") && i + 1 < argc) tgt_dist = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--tranq") && i + 1 < argc) tq_t = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log_n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--hips")) hips = 1;
        else if (!strcmp(argv[i], "--noblock")) no_target = 1;
    }
    if (log_n < 1) log_n = 50;
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
    MvVrGameStatus st; mv_vrgame_status(&st);
    VrFireStats fs; vrfire_stats(&fs);
    fprintf(stderr, "[esp] fin: disparos VR=%u peticiones=%u sustituidos=%u\n", st.shots, fs.requests, fs.subst);
    return 0;
}
