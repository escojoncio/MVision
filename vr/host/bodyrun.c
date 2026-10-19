/* bodyrun: banco de pruebas de las mecánicas de cuerpo VR sobre el juego real (host completo en MV_NOGEN, sin escenas ni render).
 * Llega a la partida con el guion de mando estándar, y a partir de ahí (T0 = primer VBlank con 30 fotogramas seguidos de control del
 * jugador) ejecuta un guion de mando/cabeza/manos sintéticos, determinista por VBlank (vrgame_set_driver), y registra el puente.
 *   bodyrun <exe> <disco> [--frames N] [--pad "T:botones:dur,..."] [--log CADA] [--scn nombre] [--setup "dir:valor,.."]
 * Botones: nombres PS1 separados por '+' (square, circle, cross, triangle, l1, r1, l2, r2, up, down, left, right, start, select).
 * T y dur en VBlanks relativos a T0. MV_SIGS = firmas. No lleva datos del juego. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "bridge.h"
#include "bridge_addrs.h"
#include "host.h"
#include "sys.h"
#include "vr_camera.h"
#include "vr_fire.h"
#include "vr_gesture.h"
#include "vrgame.h"
#include "vrhost.h"

static const struct { const char *n; int bit; } PN[] = {
    {"select", 0}, {"l3", 1}, {"r3", 2}, {"start", 3}, {"up", 4}, {"right", 5}, {"down", 6}, {"left", 7},
    {"l2", 8}, {"r2", 9}, {"l1", 10}, {"r1", 11}, {"triangle", 12}, {"circle", 13}, {"cross", 14}, {"square", 15}};
static uint16_t pmask(const char *s, size_t len) {
    uint16_t m = 0;
    while (len) {
        size_t k = 0;
        while (k < len && s[k] != '+') k++;
        for (unsigned i = 0; i < sizeof PN / sizeof *PN; i++)
            if (strlen(PN[i].n) == k && !strncasecmp(s, PN[i].n, k)) m |= (uint16_t)(1u << PN[i].bit);
        s += k; len -= k;
        if (len) { s++; len--; }
    }
    return m;
}

typedef struct { uint32_t t, dur; uint16_t m; } Ev;
static Ev ev[256];
static int nev;
static uint32_t nframes = 9000, log_every = 0, t0_vbl;
static int have_t0, inctl_run;

static float head_y = 1.65f;
/* --tp T:dist:lado: teletransporta al jugador (SOLO PRUEBAS, escribe la RAM) detrás (lado=0) o delante (1) del enemigo 0, a dist unidades, con su mismo rumbo */
static int tp_t = -1, tp_side;
static float tp_dist;
static void teleport(const BridgeState *bs) {
    if (!bs || bs->n_enemies < 1 || !bs->ctl_addr) return;
    const BridgeEnemy *e = &bs->enemies[0];
    float a = (float)e->rot.y * 6.2831853f / 4096.0f, sx = sinf(a), cz = cosf(a);
    float k = tp_side ? tp_dist : -tp_dist;
    int16_t x = (int16_t)(e->pos.x + sx * k), z = (int16_t)(e->pos.z + cz * k), y = e->pos.y;
    int16_t yaw = tp_side ? (int16_t)(e->rot.y + 2048) : e->rot.y;
    uint8_t *c = psx_ram + (bs->ctl_addr & 0x1FFFFF);
    memcpy(c + 0, &x, 2); memcpy(c + 2, &y, 2); memcpy(c + 4, &z, 2); memcpy(c + 8 + 2, &yaw, 2);
    uint8_t *p = psx_ram + (BA_PLAYER_POS & 0x1FFFFF);
    memcpy(p + 0, &x, 2); memcpy(p + 2, &y, 2); memcpy(p + 4, &z, 2);
}
static float hyaw_deg;   /* guiñada de la cabeza sintética (+ = izquierda) */
static void set_head(float x, float y, float z) {
    float a = hyaw_deg * 3.14159265f / 180.0f, c = cosf(a), sn = sinf(a);
    float m[16] = {c, 0, -sn, 0, 0, 1, 0, 0, sn, 0, c, 0, x, y, z, 1};
    mv_vr_set_head(m, 0.0);
}

static void print_bs(uint32_t t, const BridgeState *b) {
    fprintf(stderr, "[t=%u] st=%08x stance=%d act=%d wpn=%d pos=(%d,%d,%d) yaw=%d life=%d/%d alert=%d en=%d |", t, b->player_status, b->stance, b->player_action, b->weapon,
            b->ctl_pos.x, b->ctl_pos.y, b->ctl_pos.z, b->ctl_rot.y, b->life, b->life_max, b->alert_mode, b->n_enemies);
    for (int i = 0; i < b->n_enemies && i < 4; i++) {
        const BridgeEnemy *e = &b->enemies[i];
        int dx = e->pos.x - b->ctl_pos.x, dz = e->pos.z - b->ctl_pos.z;
        fprintf(stderr, " e%d cone=%d/%d d=%d rot=%d v=%d f=%d dn=%d a=%d", i, e->has_cone, e->cone_dir, (int)sqrtf((float)(dx * dx + dz * dz)), e->rot.y, e->vital, e->faint, e->down, e->alert_state);
    }
    if (getenv("BR_TGT")) {   /* blancos cercanos al jugador */
        for (int i = 0; i < b->n_targets; i++) {
            const BridgeTarget *g = &b->targets[i];
            int dx = g->center.x - b->ctl_pos.x, dz = g->center.z - b->ctl_pos.z;
            if (dx * dx + dz * dz < 900 * 900) fprintf(stderr, " | tgt%d cls=%u side=%u dmg=%u vit=%d dm=%d fa=%d c=(%d,%d,%d) h=(%d,%d,%d)", i, g->cls, g->side, g->damaged, g->vital, g->damage, g->faint, g->center.x, g->center.y, g->center.z, g->half_size.x, g->half_size.y, g->half_size.z);
        }
    }
    fputc('\n', stderr);
}

static uint16_t std_pad(uint32_t v) {
    uint16_t b = 0;
    if ((v >= 120 && v < 126) || (v >= 400 && v < 406)) b |= 1u << 3;
    if (v >= 97 && v % 97 < 6) b |= 1u << 13;
    if (v >= 211 && v % 211 < 6) b |= 1u << 7;
    return b;
}

/* ---- manos sintéticas (espacio del usuario) ---- */
static const char *scn;
static float head_x;     /* desplazamiento lateral de la cabeza (asomarse) */
static void hand_set(int side, float x, float y, float z, float grip) {
    MvVrHand h; memset(&h, 0, sizeof h);
    h.valid = 1; h.rot[3] = 1; h.pos[0] = x; h.pos[1] = y; h.pos[2] = z; h.grip = grip;
    mv_vrgame_set_hand(side, &h);
}
static float lerpf(float a, float b, float t) { return a + (b - a) * (t < 0 ? 0 : t > 1 ? 1 : t); }
/* mundo (unidades del motor) -> espacio del usuario, invirtiendo la correspondencia del disparo */
static int world_to_user(const float W[3], float u[3]) {
    VrFireMap m; vrfire_map_from_cam(&m, vrcam_last_record());
    if (!m.valid) return 0;
    float o[3] = {m.p_ref[0], m.p_ref[1], m.p_ref[2]}, w0[3], wx[3], wy[3], wz[3], q[3];
    vrfire_pos_to_world(&m, o, w0);
    q[0] = o[0] + 1; q[1] = o[1]; q[2] = o[2]; vrfire_pos_to_world(&m, q, wx);
    q[0] = o[0]; q[1] = o[1] + 1; q[2] = o[2]; vrfire_pos_to_world(&m, q, wy);
    q[0] = o[0]; q[1] = o[1]; q[2] = o[2] + 1; vrfire_pos_to_world(&m, q, wz);
    float A[3][3], d[3];
    for (int i = 0; i < 3; i++) { A[i][0] = wx[i] - w0[i]; A[i][1] = wy[i] - w0[i]; A[i][2] = wz[i] - w0[i]; d[i] = W[i] - w0[i]; }
    float det = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) + A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
    if (fabsf(det) < 1e-3f) return 0;
    float inv[3][3];
    inv[0][0] = (A[1][1] * A[2][2] - A[1][2] * A[2][1]) / det; inv[0][1] = (A[0][2] * A[2][1] - A[0][1] * A[2][2]) / det; inv[0][2] = (A[0][1] * A[1][2] - A[0][2] * A[1][1]) / det;
    inv[1][0] = (A[1][2] * A[2][0] - A[1][0] * A[2][2]) / det; inv[1][1] = (A[0][0] * A[2][2] - A[0][2] * A[2][0]) / det; inv[1][2] = (A[0][2] * A[1][0] - A[0][0] * A[1][2]) / det;
    inv[2][0] = (A[1][0] * A[2][1] - A[1][1] * A[2][0]) / det; inv[2][1] = (A[0][1] * A[2][0] - A[0][0] * A[2][1]) / det; inv[2][2] = (A[0][0] * A[1][1] - A[0][1] * A[1][0]) / det;
    for (int i = 0; i < 3; i++) u[i] = o[i] + inv[i][0] * d[0] + inv[i][1] * d[1] + inv[i][2] * d[2];
    return 1;
}
static void hands_for(uint32_t t, const BridgeState *bs) {
    float gR = 0;
    float rx = 0.2f, ry = 1.25f, rz = -0.15f;          /* reposo, mano derecha */
    if (scn && (!strcmp(scn, "punch") || !strcmp(scn, "punch_noenemy") || !strcmp(scn, "punch_slow"))) {
        uint32_t k = t < 230 ? 999 : (t - 230) % 40;
        int n = !strcmp(scn, "punch_slow") ? 60 : 6;
        if (k < (uint32_t)n) rz = lerpf(-0.15f, -0.75f, (float)(k + 1) / n);
        else if (k < (uint32_t)n + 10) rz = lerpf(-0.75f, -0.15f, (float)(k - n + 1) / 10);
    } else if (scn && !strcmp(scn, "grab") && bs && bs->n_enemies > 0) {
        const BridgeEnemy *e = &bs->enemies[0];
        float W[3] = {(float)e->pos.x, (float)e->pos.y + 450.0f, (float)e->pos.z}, u[3];
        if (t >= 215 && world_to_user(W, u)) { rx = u[0]; ry = u[1]; rz = u[2]; }
        if (t >= 225 && t < 360) gR = 1.0f;
        if ((t >= 300 && t < 308) || (t >= 322 && t < 330)) gR = 0.1f;   /* aflojar y apretar de nuevo: toques */
    } else if (scn && !strcmp(scn, "knock")) {
        uint32_t k = t < 70 ? 999 : (t - 70) % 40;
        if (k < 6) rz = lerpf(-0.15f, 0.4f, (float)(k + 1) / 6);
        else if (k < 16) rz = lerpf(0.4f, -0.15f, (float)(k - 5) / 10);
    }
    hand_set(1, rx, ry, rz, gR);
    hand_set(0, -0.2f, 1.25f, -0.15f, 0);
}

static uint32_t last_st = 0xFFFFFFFFu; static int last_act = -1;
static uint16_t driver(uint32_t vbl, uint16_t pad, const BridgeState *bs) {
    (void)pad;
    int ctl = bs && bs->in_control && bs->has_player && (bs->valid & BRIDGE_OK_ALL_CORE) == BRIDGE_OK_ALL_CORE && vrcam_active();
    if (!have_t0) {
        if (vbl % 500 == 0) fprintf(stderr, "[bodyrun] vbl=%u valid=%x has_player=%d in_control=%d cam=%d game=%d\n", vbl, bs ? bs->valid : 0, bs ? bs->has_player : 0, bs ? bs->in_control : 0, vrcam_active(), bs ? (int)bs->game_status : 0);
        inctl_run = ctl ? inctl_run + 1 : 0;
        if (inctl_run >= 30) { have_t0 = 1; t0_vbl = vbl; fprintf(stderr, "[bodyrun] T0 = VBlank %u\n", vbl); }
        else { set_head(0, head_y, 0); return std_pad(vbl); }
    }
    uint32_t t = vbl - t0_vbl;
    if (scn && !strcmp(scn, "peek")) head_x = t >= 80 && t < 140 ? 0.2f : 0.0f;
    set_head(head_x, head_y, 0);
    hands_for(t, bs);
    if (tp_t >= 0 && t >= (uint32_t)tp_t && t < (uint32_t)tp_t + 4) teleport(bs);
    uint16_t b = 0;
    for (int i = 0; i < nev; i++) if (t >= ev[i].t && t < ev[i].t + ev[i].dur) b |= ev[i].m;
    if (bs && (bs->player_status != last_st || bs->player_action != last_act || (log_every && t % log_every == 0))) { print_bs(t, bs); last_st = bs->player_status; last_act = bs->player_action; }
    return b;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: %s <exe> <disco> [--frames N] [--pad guion] [--log N]\n", argv[0]); return 1; }
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) nframes = (uint32_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log_every = (uint32_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--nogest")) mv_vrgame_set_gestures(0, 0);
        else if (!strcmp(argv[i], "--scn") && i + 1 < argc) scn = argv[++i];
        else if (!strcmp(argv[i], "--hyaw") && i + 1 < argc) hyaw_deg = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--tp") && i + 1 < argc) { int tt, sd = 0; float d = 0; sscanf(argv[++i], "%d:%f:%d", &tt, &d, &sd); tp_t = tt; tp_dist = d; tp_side = sd; }
        else if (!strcmp(argv[i], "--pad") && i + 1 < argc) {
            char *dup = strdup(argv[++i]), *sv = NULL;
            for (char *tok = strtok_r(dup, ",", &sv); tok && nev < 256; tok = strtok_r(NULL, ",", &sv)) {
                char *e;
                uint32_t tt = (uint32_t)strtoul(tok, &e, 0);
                if (*e != ':') continue;
                char *bt = e + 1, *c2 = strchr(bt, ':');
                uint32_t d = c2 ? (uint32_t)strtoul(c2 + 1, 0, 0) : 6;
                ev[nev++] = (Ev){tt, d, pmask(bt, c2 ? (size_t)(c2 - bt) : strlen(bt))};
            }
            free(dup);
        }
    }
    const char *sigs = getenv("MV_SIGS");
    if (sigs) mv_host_set_sigs(sigs);
    mkdir("build-host", 0755);
    mkdir("build-host/card", 0755);
    mv_vrhost_install();
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
    fprintf(stderr, "[bodyrun] eventos de gestos: golpe %u agarre %u apretón %u soltar %u pared(golpe) %u asomar %u contacto-pared %u daño %u\n", vrbodyg_event_count(VRG_EV_PUNCH), vrbodyg_event_count(VRG_EV_GRAB),
            vrbodyg_event_count(VRG_EV_SQUEEZE), vrbodyg_event_count(VRG_EV_RELEASE), vrbodyg_event_count(VRG_EV_KNOCK), vrbodyg_event_count(VRG_EV_PEEK), vrbodyg_event_count(VRG_EV_WALL), vrbodyg_event_count(VRG_EV_HURT));
    return 0;
}
