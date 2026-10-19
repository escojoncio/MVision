/* Ganchos de aatest: validación dinámica del apuntado del lanzador antiaéreo (vr_aa) con el juego real (intérprete).
 * Se enlaza con gptest.c (que lleva el guion base: pad, poke16, inv, ppm, trace, end...). Órdenes propias del guion:
 *   mapa DY THETA              correspondencia usuario->motor: ojo = jugador + (0,DY,0) unidades, giro THETA grados
 *   visor T YAW PITCH [ROLL]   pose del visor del lanzador desde T (YAW + = izquierda, PITCH + = arriba, grados; posición fija
 *                              delante del ojo de referencia)
 *   apuntar T0 T1              gesto del visor activo entre T0 y T1 (el visor manda en la vista del buscador y vr_fire mantiene el botón)
 *   blanco ID T0 T1 X Y Z      blanco sintético en el reparto de blancos (clase lanzable, bando enemigo) en la posición
 *                              (X,Y,Z) relativa al ojo, en el mundo del motor; se reescribe cada fotograma entre T0 y T1
 *   mueve ID VX VY VZ          velocidad del blanco sintético ID (unidades por fotograma)
 *   disparo T                  el gatillo VR se aprieta en T (petición de disparo con la boca del lanzador)
 *   sinhle T0 T1               sin sustituir la vista del buscador (control A/B: vista del juego)
 * Cada paso del buscador, cada cambio de blanco seguido y cada misil creado se imprimen. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gptest_hooks.h"
#include "hlebind.h"
#include "sys.h"
#include "vr_aa.h"
#include "vr_fire.h"
#include "bridge_addrs.h"

enum { C_MAPA, C_VISOR, C_APUNTAR, C_BLANCO, C_MUEVE, C_DISPARO, C_SINHLE };
typedef struct { int kind, t0, t1, id; double v[6]; } Cmd;
static Cmd cmd[128]; static int ncmd;
static uint8_t *g_ram;

void gptest_cmd(const char *l) {
    char k[16]; Cmd c; memset(&c, 0, sizeof c);
    if (sscanf(l, "%15s", k) != 1) return;
    if (!strcmp(k, "mapa") && sscanf(l, "%*s %lf %lf", &c.v[0], &c.v[1]) == 2) c.kind = C_MAPA;
    else if (!strcmp(k, "visor")) { c.kind = C_VISOR; if (sscanf(l, "%*s %d %lf %lf %lf", &c.t0, &c.v[0], &c.v[1], &c.v[2]) < 3) return; }
    else if (!strcmp(k, "apuntar") && sscanf(l, "%*s %d %d", &c.t0, &c.t1) == 2) c.kind = C_APUNTAR;
    else if (!strcmp(k, "blanco") && sscanf(l, "%*s %d %d %d %lf %lf %lf", &c.id, &c.t0, &c.t1, &c.v[0], &c.v[1], &c.v[2]) == 6) c.kind = C_BLANCO;
    else if (!strcmp(k, "mueve") && sscanf(l, "%*s %d %lf %lf %lf", &c.id, &c.v[0], &c.v[1], &c.v[2]) == 4) c.kind = C_MUEVE;
    else if (!strcmp(k, "disparo") && sscanf(l, "%*s %d", &c.t0) == 1) c.kind = C_DISPARO;
    else if (!strcmp(k, "sinhle") && sscanf(l, "%*s %d %d", &c.t0, &c.t1) == 2) c.kind = C_SINHLE;
    else return;
    if (ncmd < 128) cmd[ncmd++] = c;
}

static int16_t r16(uint32_t a) { int16_t v; memcpy(&v, g_ram + (a & 0x1FFFFF), 2); return v; }
static uint32_t r32(uint32_t a) { uint32_t v; memcpy(&v, g_ram + (a & 0x1FFFFF), 4); return v; }
static void w16(uint32_t a, int v) { int16_t x = (int16_t)v; memcpy(g_ram + (a & 0x1FFFFF), &x, 2); }

/* ---- misil creado ---- */
static uint32_t g_missile;
static int g_m_off = -1;
static void probe(int kind, int decision, uint32_t res, const VrFireMat *m) {
    if (kind != 1) return;
    fprintf(stderr, "[misil] creado actor=%08x decision=%s t=(%d,%d,%d)", res, decision == 1 ? "SUSTITUIDA" : decision == -1 ? "suprimida" : "pasa", m->t[0], m->t[1], m->t[2]);
    if (decision == 1) { float f[3]; vrfire_mat_forward(m, f); fprintf(stderr, " delante=(%.3f,%.3f,%.3f)", f[0], f[1], f[2]); }
    fprintf(stderr, "\n");
    if (res >= 0x80010000u && res < 0x80200000u) {
        g_missile = res;
        g_m_off = -1;
        for (uint32_t o = 0; o < 0x200; o += 2)
            if (!memcmp(g_ram + ((res + o) & 0x1FFFFF), m->m, 18)) { g_m_off = (int)o; break; }
        fprintf(stderr, "[misil] matriz de origen en el actor +0x%x\n", g_m_off);
    }
}

void gptest_pre_bind(void) {
    static const char *const n[] = {"vrfire_bullet", "vrfire_flash_a", "vrfire_flash_b", "vrfire_rmissile", "vrfire_amissile", "vrfire_mine",
                                    "vrfire_charge", "vrfire_grenade", "vrfire_hide", "vrfire_aalock", "vrfire_aaview"};
    for (size_t i = 0; i < sizeof n / sizeof *n; i++) hle_bind_declare_fixed(n[i]);
}
static const struct { const char *n; uint32_t a; } addrs[] = {
    {"vrfire_amissile", 0x8006eca0}, {"vrfire_aalock", 0x80071fc8}, {"vrfire_aaview", 0x800711ac}};

int gptest_init(uint8_t *ram) {
    g_ram = ram;
    for (size_t i = 0; i < sizeof addrs / sizeof *addrs; i++)
        if (hle_bind_fixed(addrs[i].n, addrs[i].a) != 0) { fprintf(stderr, "[aatest] no se pudo enlazar %s\n", addrs[i].n); return -1; }
    return 0;
}

static WQuat quat_ypr(double yaw, double pitch, double roll) {
    double y = yaw * M_PI / 180, p = pitch * M_PI / 180, r = roll * M_PI / 180;
    double cy = cos(y / 2), sy = sin(y / 2), cp = cos(p / 2), sp = sin(p / 2), cr = cos(r / 2), sr = sin(r / 2);
    /* q = Ry(yaw) * Rx(pitch) * Rz(roll) */
    double a[4] = {0, sy, 0, cy}, b[4] = {sp, 0, 0, cp}, c[4] = {0, 0, sr, cr}, t[4], o[4];
#define QM(o, a, b) do { o[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1]; o[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0]; \
                         o[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3]; o[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2]; } while (0)
    QM(t, a, b); QM(o, t, c);
    WQuat q = {(float)o[0], (float)o[1], (float)o[2], (float)o[3]};
    return q;
}
static void fwd_of(WQuat q, float f[3], float u[3]) {
    f[0] = -2 * (q.x * q.z + q.w * q.y); f[1] = -2 * (q.y * q.z - q.w * q.x); f[2] = -(1 - 2 * (q.x * q.x + q.y * q.y));
    u[0] = 2 * (q.x * q.y - q.w * q.z); u[1] = 1 - 2 * (q.x * q.x + q.z * q.z); u[2] = 2 * (q.y * q.z + q.w * q.x);
}

static WPose g_sight = {{0, 1.6f, -0.3f}, {0, 0, 0, 1}};
static double g_syaw, g_spitch;
static int g_inited, g_slot[8];
static double g_tpos[8][3], g_tvel[8][3];
static int g_tgt_on[8];
static int g_last_target = -1, g_last_steps, g_last_frames = -1;

/* escribe el blanco sintético en una ranura libre del reparto (y sube el tope si hace falta) */
static void put_target(int id, const BridgeState *bs, double dy) {
    int n = (int)r32(BA_N_TARGETS);
    if (!g_slot[id]) {
        for (int i = 63; i >= 0; i--) {
            int used = 0;
            for (int k = 0; k < 8; k++) if (g_slot[k] == i + 1) used = 1;
            if (!used && r16(BA_TARGET_ARRAY + 72u * (uint32_t)i) == 0) { g_slot[id] = i + 1; break; }
        }
        if (!g_slot[id]) return;
    }
    int s = g_slot[id] - 1;
    uint32_t a = BA_TARGET_ARRAY + 72u * (uint32_t)s;
    uint32_t pmap = r32(0x800ACB9Cu);
    w16(a + 0, 0x1F);                  /* disponible + captura + daño + empuje + búsqueda (sin "caído": el buscador excluye las clases 0x20 y 0x200) */
    w16(a + 2, 2);                     /* bando enemigo */
    w16(a + 4, (int)(pmap ? pmap : 1));
    w16(a + 6, 0);                     /* sin daño */
    double eye[3] = {bs->player_pos.x, bs->player_pos.y + dy, bs->player_pos.z};
    for (int i = 0; i < 3; i++) w16(a + 8 + 2u * (uint32_t)i, (int)lrint(eye[i] + g_tpos[id][i]));
    w16(a + 0x10, 300); w16(a + 0x12, 400); w16(a + 0x14, 300);
    w16(a + 0x26, 100);
    if (n < s + 1) { uint32_t v = (uint32_t)(s + 1); memcpy(g_ram + (BA_N_TARGETS & 0x1FFFFF), &v, 4); }
}

void gptest_frame(uint32_t f, int t, const BridgeState *bs, uint16_t *pad) {
    (void)f;
    if (t < 0) return;
    if (!g_inited) {
        g_inited = 1;
        VrFireCfg cfg; vrfire_cfg_default(&cfg);
        vrfire_init(&cfg);
        VrAaCfg ac; vraa_cfg_default(&ac);
        vraa_init(&ac);
        vrfire_set_probe(probe);
    }
    double dy = 1600, theta = 0;
    int aim = 0, shot = 0, nohle = 0;
    for (int i = 0; i < ncmd; i++) {
        Cmd *c = &cmd[i];
        switch (c->kind) {
        case C_MAPA: dy = c->v[0]; theta = c->v[1]; break;
        case C_VISOR:
            if (t == c->t0) {
                g_syaw = c->v[0]; g_spitch = c->v[1];
                g_sight.q = quat_ypr(c->v[0], c->v[1], c->v[2]);
                fprintf(stderr, "[visor] t=%d yaw=%.1f pitch=%.1f roll=%.1f\n", t, c->v[0], c->v[1], c->v[2]);
            }
            break;
        case C_APUNTAR: if (t >= c->t0 && t < c->t1) aim = 1; break;
        case C_DISPARO: if (t == c->t0) shot = 1; break;
        case C_SINHLE: if (t >= c->t0 && t < c->t1) nohle = 1; break;
        case C_BLANCO:
            if (t == c->t0) { for (int k = 0; k < 3; k++) { g_tpos[c->id][k] = c->v[k]; g_tvel[c->id][k] = 0; } }
            g_tgt_on[c->id] = (t >= c->t0 && t < c->t1);
            break;
        case C_MUEVE: for (int k = 0; k < 3; k++) g_tvel[c->id][k] = c->v[k]; break;
        }
    }
    for (int id = 0; id < 8; id++) {
        if (!g_tgt_on[id]) continue;
        for (int k = 0; k < 3; k++) g_tpos[id][k] += g_tvel[id][k];
        put_target(id, bs, dy);
    }
    VrFireMap map; memset(&map, 0, sizeof map);
    if (bs->has_player) {
        map.valid = 1; map.eye[0] = bs->player_pos.x; map.eye[1] = bs->player_pos.y + (int)dy; map.eye[2] = bs->player_pos.z;
        map.theta_play_deg = (float)theta; map.p_ref[1] = 1.6f; map.units_per_m = 1000.0f;
    }
    /* visor y boca (WP_EYE y WP_MUZZLE del lanzador: 0,34 m delante del visor y 0,055 m más bajo) */
    float fw[3], up[3];
    fwd_of(g_sight.q, fw, up);
    WPose mz = g_sight;
    mz.p.x += fw[0] * 0.34f - up[0] * 0.055f; mz.p.y += fw[1] * 0.34f - up[1] * 0.055f; mz.p.z += fw[2] * 0.34f - up[2] * 0.055f;
    vraa_set_scope(aim && !nohle, &map, &g_sight);
    vraa_tick();

    VrFireGame g; vrfire_read_game(g_ram, bs, &g);
    VrFireIn in; memset(&in, 0, sizeof in);
    in.active = bs->in_control && bs->has_player; in.muzzle = mz; in.fire_pose = mz;
    in.ready = aim; in.raise = 1; in.fire = shot; in.map = &map;
    VrFireOut out; vrfire_frame(&g, &in, &out);
    if (out.engaged) { *pad = (uint16_t)(*pad & ~(1u << 15)); if (out.square) *pad |= 1u << 15; }
    if (shot) fprintf(stderr, "[disparo] t=%d armado=%d blanco_juego=%08x\n", t, g.armed, r32(0x800BF080u));

    VrAaLock lk; vraa_lock_info(&lk);
    if (lk.steps != g_last_steps || (int)lk.target != g_last_target || lk.frames != g_last_frames) {
        if ((int)lk.target != g_last_target || (lk.frames % 6 == 0) || lk.steps - g_last_steps > 1)
            fprintf(stderr, "[buscador] t=%d vivo=%d pasos=%d(vr=%d) blanco=%08x frames=%d perdido=%d cand=%d cerrado=%d blanco_global=%08x\n", t, lk.alive, lk.steps, lk.swapped, lk.target,
                    lk.frames, lk.lost, lk.candidates, lk.converged, r32(0x800BF080u));
        g_last_steps = lk.steps; g_last_target = (int)lk.target; g_last_frames = lk.frames;
    }
    if (g_missile && g_m_off >= 0) {
        uint32_t o = g_missile + (uint32_t)g_m_off + 20;
        int32_t px = (int32_t)r32(o), py = (int32_t)r32(o + 4), pz = (int32_t)r32(o + 8);
        static int lastlog = -1000;
        if (t - lastlog >= 4) {
            lastlog = t;
            double best = 1e18; int bi = -1;
            for (int id = 0; id < 8; id++) if (g_tgt_on[id]) {
                double dx = bs->player_pos.x + g_tpos[id][0] - px, dyy = bs->player_pos.y + dy + g_tpos[id][1] - py, dz = bs->player_pos.z + g_tpos[id][2] - pz;
                double d = sqrt(dx * dx + dyy * dyy + dz * dz); if (d < best) { best = d; bi = id; }
            }
            fprintf(stderr, "[misil] t=%d pos=(%d,%d,%d) dist_blanco=%.0f (id %d) edad=%d\n", t, px, py, pz, bi >= 0 ? best : -1.0, bi, (int)r32(g_missile + 0x120));
        }
    }
}

void gptest_finish(void) {
    VrFireStats s; vrfire_stats(&s);
    VrAaLock lk; vraa_lock_info(&lk);
    fprintf(stderr, "[vrfire] peticiones=%u caducadas=%u sustituidas=%u pasadas=%u suprimidas=%u pulsaciones=%u soltados=%u timeouts=%u\n",
            s.requests, s.expired, s.subst, s.passed, s.skipped, s.presses, s.releases, s.arm_timeouts);
    fprintf(stderr, "[vraa] pasos del buscador=%d con la vista del visor=%d\n", lk.steps, lk.swapped);
}
