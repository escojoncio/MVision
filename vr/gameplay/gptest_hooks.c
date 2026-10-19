/* Ganchos de gptest para validar vr_fire / vr_reload con el juego real (intérprete). Guion (además de los de gptest):
 *   vrfire MODO T0 T1          activa el disparo VR entre T0 y T1; MODO = trigger | raise
 *   mapauto DY THETA           correspondencia usuario->motor: ojo = jugador + (0,DY,0) unidades, giro THETA grados,
 *                              p_ref = (0, 1.6, 0) m, 1000 unidades/m (se recalcula cada fotograma)
 *   muzzle T X Y Z YAW PITCH   desde T la boca del arma VR (espacio de usuario, m; YAW + = izquierda, PITCH + = arriba, grados)
 *   shot T                     petición de disparo del arma VR en T
 *   ready T0 T1                arma levantada (modo raise)
 *   autohold T0 T1             gatillo mantenido (armas que disparan mientras se mantiene)
 *   hide T0 T1                 ocultar el arma del juego
 * Cada proyectil creado por el juego se imprime con su matriz y, para balas, con la posición y el paso leídos del actor. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "gptest_hooks.h"
#include "hlebind.h"
#include "sys.h"
#include "vr_fire.h"
#include "vr_reload.h"
#include "bridge_addrs.h"

enum { C_VRFIRE, C_MAPAUTO, C_MUZZLE, C_SHOT, C_READY, C_AUTO, C_HIDE, C_THROW, C_RELOAD, C_MAGIN, C_NOTREADY };
typedef struct { int kind, t0, t1, mode; double v[6]; } Cmd;
static Cmd cmd[128]; static int ncmd;
static uint8_t *g_ram;
static double g_yaw, g_pitch, g_theta, g_shot_yaw, g_shot_pitch, g_shot_theta;

void gptest_cmd(const char *l) {
    char k[16], m[16]; Cmd c; memset(&c, 0, sizeof c);
    if (sscanf(l, "%15s", k) != 1) return;
    if (!strcmp(k, "vrfire") && sscanf(l, "%*s %15s %d %d", m, &c.t0, &c.t1) == 3) { c.kind = C_VRFIRE; c.mode = !strcmp(m, "raise"); }
    else if (!strcmp(k, "mapauto") && sscanf(l, "%*s %lf %lf", &c.v[0], &c.v[1]) == 2) c.kind = C_MAPAUTO;
    else if (!strcmp(k, "muzzle") && sscanf(l, "%*s %d %lf %lf %lf %lf %lf", &c.t0, &c.v[0], &c.v[1], &c.v[2], &c.v[3], &c.v[4]) == 6) c.kind = C_MUZZLE;
    else if (!strcmp(k, "shot") && sscanf(l, "%*s %d", &c.t0) == 1) c.kind = C_SHOT;
    else if (!strcmp(k, "ready") && sscanf(l, "%*s %d %d", &c.t0, &c.t1) == 2) c.kind = C_READY;
    else if (!strcmp(k, "autohold") && sscanf(l, "%*s %d %d", &c.t0, &c.t1) == 2) c.kind = C_AUTO;
    else if (!strcmp(k, "throw") && sscanf(l, "%*s %d %lf %lf %lf %lf %lf %lf", &c.t0, &c.v[0], &c.v[1], &c.v[2], &c.v[3], &c.v[4], &c.v[5]) == 7) c.kind = C_THROW;
    else if (!strcmp(k, "reload") && sscanf(l, "%*s %15s %d %d", m, &c.t0, &c.t1) == 3) { c.kind = C_RELOAD; c.mode = !strcmp(m, "r2"); }
    else if (!strcmp(k, "magin") && sscanf(l, "%*s %d", &c.t0) == 1) c.kind = C_MAGIN;
    else if (!strcmp(k, "notready") && sscanf(l, "%*s %d %d", &c.t0, &c.t1) == 2) c.kind = C_NOTREADY;
    else if (!strcmp(k, "hide") && sscanf(l, "%*s %d %d", &c.t0, &c.t1) == 2) c.kind = C_HIDE;
    else return;
    if (ncmd < 128) cmd[ncmd++] = c;
}

static int16_t r16(uint32_t a) { return *(int16_t *)(g_ram + (a & 0x1FFFFF)); }
static uint32_t r32(uint32_t a) { return *(uint32_t *)(g_ram + (a & 0x1FFFFF)); }

/* sonda: lo que el juego ha creado con la matriz sustituida */
static void probe(int kind, int decision, uint32_t res, const VrFireMat *m) {
    static const char *dn[] = {"pasa", "SUSTITUIDA", "?"};
    fprintf(stderr, "[fire] kind=%d %s actor=%08x", kind, dn[decision == 1 ? 1 : decision == 0 ? 0 : 2], res);
    if (decision == -1) { fprintf(stderr, " (suprimido)\n"); return; }
    if (decision == 1) {
        float f[3]; vrfire_mat_forward(m, f);
        fprintf(stderr, " t=(%d,%d,%d) delante=(%.3f,%.3f,%.3f)", m->t[0], m->t[1], m->t[2], f[0], f[1], f[2]);
        {   /* dirección esperada calculada aparte: (cos p sin(theta+yaw), sin p, cos p cos(theta+yaw)) */
            double p = g_shot_pitch * M_PI / 180, h = (g_shot_theta + g_shot_yaw) * M_PI / 180;
            double e[3] = {cos(p) * sin(h), sin(p), cos(p) * cos(h)};
            double dot = e[0] * f[0] + e[1] * f[1] + e[2] * f[2];
            double cx = e[1] * f[2] - e[2] * f[1], cy = e[2] * f[0] - e[0] * f[2], cz = e[0] * f[1] - e[1] * f[0];
            fprintf(stderr, " esperada=(%.3f,%.3f,%.3f) err=%.3f grados", e[0], e[1], e[2], atan2(sqrt(cx * cx + cy * cy + cz * cz), dot) * 180 / M_PI);
        }
    }
    if (kind == 3) { fprintf(stderr, " lanzamiento %s pos=(%d,%d,%d) vel/tick=(%d,%d,%d)\n", decision ? "SUSTITUIDO" : "original", m->t[0], m->t[1], m->t[2], m->m[0], m->m[1], m->m[2]); return; }
    if (kind == 0 && res >= 0x80010000u && res < 0x80200000u) {
        /* busca la copia de la matriz en el actor y los VEC16 pos/paso */
        int off_w = -1;
        for (uint32_t o = 0; o < 0x80 && decision == 1; o += 4) {
            if (!memcmp(g_ram + ((res + o) & 0x1FFFFF), m->m, 18)) { off_w = (int)o; break; }
        }
        fprintf(stderr, " world@+%x", off_w);
        if (decision == 1) {
            float f[3]; vrfire_mat_forward(m, f);
            int found_p = -1, found_s = -1;
            for (uint32_t o = 0x24; o < 0x300; o += 2) {
                int x = r16(res + o), y = r16(res + o + 2), z = r16(res + o + 4);
                /* inicio del trazo: t - 400*delante */
                if (found_p < 0 && abs(x - (int)lrintf(m->t[0] - 400 * f[0])) <= 2 && abs(y - (int)lrintf(m->t[1] - 400 * f[1])) <= 2 && abs(z - (int)lrintf(m->t[2] - 400 * f[2])) <= 2 && o > 0x44) { found_p = (int)o; fprintf(stderr, " pos@+%x=(%d,%d,%d)", o, x, y, z); }
                /* paso por tick: velocidad*delante, con velocidad 750/5000 */
                for (int sp = 0; sp < 2 && found_s < 0 && o > 0x44; sp++) {
                    int v = sp ? 5000 : 750;
                    if (abs(x - (int)lrintf(v * f[0])) <= 3 && abs(y - (int)lrintf(v * f[1])) <= 3 && abs(z - (int)lrintf(v * f[2])) <= 3 && (abs(x) + abs(y) + abs(z)) > 100) { found_s = (int)o; fprintf(stderr, " paso@+%x=(%d,%d,%d)", o, x, y, z); }
                }
            }
        }
    }
    fprintf(stderr, "\n");
}

void gptest_pre_bind(void) {
    static const char *const n[] = {"vrfire_bullet", "vrfire_flash_a", "vrfire_flash_b", "vrfire_rmissile", "vrfire_amissile", "vrfire_mine",
                                    "vrfire_charge", "vrfire_grenade", "vrfire_hide"};
    for (size_t i = 0; i < sizeof n / sizeof *n; i++) hle_bind_declare_fixed(n[i]);
}
static const struct { const char *n; uint32_t a; } addrs[] = {
    {"vrfire_bullet", 0x800779a8}, {"vrfire_flash_a", 0x8005e6dc}, {"vrfire_flash_b", 0x8005e358}, {"vrfire_rmissile", 0x8006e128},
    {"vrfire_amissile", 0x8006eca0}, {"vrfire_mine", 0x8006c4b4}, {"vrfire_charge", 0x8006b69c}, {"vrfire_grenade", 0x8006af98}, {"vrfire_hide", 0x80018c8c}};

int gptest_init(uint8_t *ram) {
    g_ram = ram;
    for (size_t i = 0; i < sizeof addrs / sizeof *addrs; i++)
        if (hle_bind_fixed(addrs[i].n, addrs[i].a) != 0) { fprintf(stderr, "[gptest] no se pudo enlazar %s\n", addrs[i].n); return -1; }
    return 0;
}

static WQuat quat_ypr(double yaw, double pitch) {
    double y = yaw * M_PI / 180, p = pitch * M_PI / 180;
    /* q = Ry(yaw) * Rx(pitch) */
    double cy = cos(y / 2), sy = sin(y / 2), cp = cos(p / 2), sp = sin(p / 2);
    WQuat q = {(float)(cy * sp), (float)(sy * cp), (float)(-sy * sp), (float)(cy * cp)};
    return q;
}

static WPose g_mz = {{0, 1.3f, -0.4f}, {0, 0, 0, 1}};
static int g_inited;
void gptest_frame(uint32_t f, int t, const BridgeState *bs, uint16_t *pad) {
    (void)f;
    if (t < 0) return;
    if (!g_inited) {
        g_inited = 1;
        VrFireCfg cfg; vrfire_cfg_default(&cfg);
        VrReloadCfg rc; vrreload_cfg_default(&rc);
        for (int i = 0; i < ncmd; i++) { if (cmd[i].kind == C_VRFIRE) cfg.arm_mode = cmd[i].mode; if (cmd[i].kind == C_RELOAD) rc.method = cmd[i].mode ? VRR_VIA_R2_TAPS : VRR_VIA_R1_DOUBLE; }
        vrfire_init(&cfg); vrreload_init(&rc);
        vrfire_set_probe(probe);
    }
    int on = 0, ready = 0, autoh = 0, hide = 0, fire = 0, rl_on = 0, magin = 0, notready = 0;
    double dy = 1600, theta = 0; int have_map = 0;
    for (int i = 0; i < ncmd; i++) {
        Cmd *c = &cmd[i];
        switch (c->kind) {
        case C_VRFIRE: if (t >= c->t0 && t < c->t1) on = 1; break;
        case C_MAPAUTO: dy = c->v[0]; theta = c->v[1]; have_map = 1; break;
        case C_MUZZLE: if (t == c->t0) { g_mz.p.x = (float)c->v[0]; g_mz.p.y = (float)c->v[1]; g_mz.p.z = (float)c->v[2]; g_mz.q = quat_ypr(c->v[3], c->v[4]); g_yaw = c->v[3]; g_pitch = c->v[4];
                           fprintf(stderr, "[muzzle] t=%d pos=(%.2f,%.2f,%.2f) yaw=%.1f pitch=%.1f\n", t, c->v[0], c->v[1], c->v[2], c->v[3], c->v[4]); } break;
        case C_SHOT: if (t == c->t0) fire = 1; break;
        case C_READY: if (t >= c->t0 && t < c->t1) ready = 1; break;
        case C_AUTO: if (t >= c->t0 && t < c->t1) autoh = 1; break;
        case C_THROW: if (t == c->t0) { float p[3] = {(float)c->v[0], (float)c->v[1], (float)c->v[2]}, v[3] = {(float)c->v[3], (float)c->v[4], (float)c->v[5]}; fire = 1;
                          fprintf(stderr, "[throw] t=%d set=%d\n", t, vrfire_set_throw(p, v, 0.04f)); } break;
        case C_RELOAD: if (t >= c->t0 && t < c->t1) rl_on = 1; break;
        case C_MAGIN: if (t == c->t0) magin = 1; break;
        case C_NOTREADY: if (t >= c->t0 && t < c->t1) notready = 1; break;
        case C_HIDE: if (t >= c->t0 && t < c->t1) hide = 1; break;
        }
    }
    (void)have_map; g_theta = theta;
    if (fire) { g_shot_yaw = g_yaw; g_shot_pitch = g_pitch; g_shot_theta = theta; }
    VrFireMap map; memset(&map, 0, sizeof map);
    if (bs->has_player) {
        map.valid = 1; map.eye[0] = bs->player_pos.x; map.eye[1] = bs->player_pos.y + (int)dy; map.eye[2] = bs->player_pos.z;
        map.theta_play_deg = (float)theta; map.p_ref[1] = 1.6f; map.units_per_m = 1000.0f;
    }
    vrfire_set_hide(hide);
    VrReloadOut ro; memset(&ro, 0, sizeof ro);
    if (rl_on) {
        VrReloadGame rg; vrreload_read_game(bs, &rg);
        VrReloadIn ri = {0, magin, !notready};
        vrreload_frame(&rg, &ri, &ro);
        static int last_state = -1, last_force;
        if (ro.state != last_state || ro.pad_force != last_force) {
            fprintf(stderr, "[reload] t=%d estado=%d busy=%d force=%04x mag=%d/%d mun=%d permitido=%d juego_arma=%d\n", t, ro.state, ro.busy, ro.pad_force, bs->magazine, bs->magazine_max,
                    bs->weapon >= 0 ? bs->weapon_ammo[bs->weapon] : -9, ro.fire_allowed, bs->weapon);
            last_state = ro.state; last_force = ro.pad_force;
        }
        if (!ro.fire_allowed) fire = 0;
        if (ro.busy) *pad = (uint16_t)(*pad & ~ro.pad_mask);
        *pad |= ro.pad_force;
    }
    if (!on) { VrFireIn in; memset(&in, 0, sizeof in); VrFireGame g; VrFireOut o; vrfire_read_game(g_ram, bs, &g); vrfire_frame(&g, &in, &o); return; }
    VrFireGame g; vrfire_read_game(g_ram, bs, &g);
    VrFireIn in; memset(&in, 0, sizeof in);
    in.active = bs->in_control && bs->has_player && !ro.busy && (!rl_on || ro.fire_allowed || ro.state == VRR_READY || ro.state == VRR_OFF); in.muzzle = g_mz; in.fire_pose = g_mz; in.ready = ready; in.fire = fire; in.trigger_held = autoh; in.map = &map;
    VrFireOut out; vrfire_frame(&g, &in, &out);
    if (out.engaged) { *pad = (uint16_t)(*pad & ~(1u << 15)); if (out.square) *pad |= 1u << 15; }
    (void)r32;
}

void gptest_finish(void) {
    VrFireStats s; vrfire_stats(&s);
    fprintf(stderr, "[vrfire] peticiones=%u caducadas=%u sustituidas=%u pasadas=%u suprimidas=%u pulsaciones=%u soltados=%u timeouts=%u\n",
            s.requests, s.expired, s.subst, s.passed, s.skipped, s.presses, s.releases, s.arm_timeouts);
}
