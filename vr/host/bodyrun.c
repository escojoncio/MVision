/* bodyrun: banco de pruebas de las mecánicas de cuerpo VR sobre el juego real (host completo en MV_NOGEN, sin escenas ni render).
 * Llega a la partida con el guion de mando estándar, y a partir de ahí (T0 = primer VBlank con 30 fotogramas seguidos de control del
 * jugador) ejecuta un guion de mando/cabeza/manos sintéticos, determinista por VBlank (vrgame_set_driver), y registra el puente.
 *   bodyrun <exe> <disco> [--frames N] [--pad "T:botones:dur,..."] [--log CADA] [--scn nombre] [--setup "dir:valor,.."]
 * Botones: nombres original separados por '+' (square, circle, cross, triangle, l1, r1, l2, r2, up, down, left, right, start, select).
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
#include <pthread.h>
#include "eyeshot.h"
#include "vr_calib.h"
#include "vr_input.h"
#include "vr_camera.h"
#include "vr_heading.h"
#include "vr_fire.h"
#include "vr_gesture.h"
#include "vrgame.h"
#include "vrhost.h"
#include "vr_body.h"

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
/* --turn T:grados[:dur]: giro del stick (+ = izquierda) repartido en `dur` VBlanks (1 = por pasos); --hyaw-at T:grados: guiñada de la cabeza (+ = izquierda) desde T;
 * --walk T:dur: stick de mover hacia delante (pad analógico) ; --headlog N: línea [head] cada N VBlanks */
typedef struct { uint32_t t, dur; float deg; } TurnEv;
static TurnEv turns[32]; static int nturns;
typedef struct { uint32_t t; float deg; } HyEv;
static HyEv hyev[32]; static int nhy;
static uint32_t headlog_every;
/* --poke T:unidades[:modo]: escribe el rumbo del jugador (4096 = vuelta) en el bloque de control; modo 0 = solo +0x0A, 1 = +0x0A y +0x2A (SOLO PRUEBAS) ; --nohead: sin vr_heading */
typedef struct { uint32_t t; int units, both; } PokeEv;
static PokeEv pokes[16]; static int npokes;
static int no_heading;
static int use_hc; static float hc_corr[3];   /* ver --hc */
/* --stick T:dur:x:y : stick izquierdo (x derecha, y adelante, -1..1) por la cadena REAL de la app (vr_input_update con la cabeza sintetica; la salida analogica va a mv_host_pad);
 * --stickr T:dur:x : stick derecho (giro de cuerpo); --hyaw-ramp T0:T1:g0:g1 : guinada de cabeza lineal; --loco log: linea [loco] por construccion de camara con
 * la distancia ojo-camara vs ojos del modelo; --stick-hand deg : (loco por mando) guinada fija del mando izquierdo */
typedef struct { uint32_t t, dur; float x, y; } StEv;
static StEv sticks[32]; static int nsticks;
static StEv sticksr[32]; static int nsticksr;
typedef struct { uint32_t t0, t1; float g0, g1; } RampEv;
static RampEv ramps[16]; static int nramps;
static int loco_log, loco_hand; static float hand_yaw_deg;
static vr_input_t vin; static int vin_init;
static float last_serial_eye[3]; static uint32_t last_serial;
static float prev_model[3]; static int have_prev_model;
static double sum_cam, max_cam, sum_prev, n_cam;
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
    uint8_t *c = emu_ram + (bs->ctl_addr & 0x1FFFFF);
    memcpy(c + 0, &x, 2); memcpy(c + 2, &y, 2); memcpy(c + 4, &z, 2); memcpy(c + 8 + 2, &yaw, 2);
    uint8_t *p = emu_ram + (BA_PLAYER_POS & 0x1FFFFF);
    memcpy(p + 0, &x, 2); memcpy(p + 2, &y, 2); memcpy(p + 4, &z, 2);
}
static float hyaw_deg;   /* guiñada de la cabeza sintética (+ = izquierda) */
static float hpitch_deg; /* cabeceo (+ = mirando ABAJO) */
static float cur_head[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1.6f, 0, 1};
static pthread_mutex_t head_mx = PTHREAD_MUTEX_INITIALIZER;
static void set_head(float x, float y, float z) {
    float a = hyaw_deg * 3.14159265f / 180.0f, c = cosf(a), sn = sinf(a), e = -hpitch_deg * 3.14159265f / 180.0f, ce = cosf(e), se = sinf(e);
    float m[16] = {c, 0, -sn, 0, sn * se, ce, c * se, 0, sn * ce, -se, c * ce, 0, x, y, z, 1};
    pthread_mutex_lock(&head_mx); memcpy(cur_head, m, sizeof m); pthread_mutex_unlock(&head_mx);
    mv_vr_set_head(m, 0.0);
}
/* --shot T:nombre : en T (VBlanks tras T0) se pide una imagen de lo que ve el usuario (escena VR rasterizada por software desde la cabeza sintética) ->
 * <BR_SHOTDIR>/<nombre>.ppm. --hpitch-at T:grados : cabeceo desde T (+ = mirando abajo). */
typedef struct { uint32_t t; char name[64]; int done; } ShotEv;
static ShotEv shots[16]; static int nshots;
static volatile int shot_pending = -1;
typedef struct { uint32_t t; float deg; } PiEv;
static PiEv piev[32]; static int npi;
static volatile int poll_run = 1;
static void *poller(void *arg) {
    (void)arg;
    static VrScene sc;
    const char *dir = getenv("BR_SHOTDIR");
    while (poll_run) {
        if (!mv_host_scene(&sc)) { usleep(1500); continue; }
        eyeshot_begin(&sc);
        int k = shot_pending;
        if (k >= 0 && sc.has_3d && sc.world_valid) {
            float h[16]; pthread_mutex_lock(&head_mx); memcpy(h, cur_head, sizeof h); pthread_mutex_unlock(&head_mx);
            if (use_hc && strncmp(shots[k].name, "raw", 3)) { h[12] += hc_corr[0]; h[13] += hc_corr[1]; h[14] += hc_corr[2]; }
            char pth[512]; snprintf(pth, sizeof pth, "%s/%s.ppm", dir ? dir : ".", shots[k].name);
            int rc = eyeshot_render(&sc, h, 640, 480, 100.0f, pth);
            fprintf(stderr, "[shot] %s rc=%d tris3d=%u cabeza=(%.2f,%.2f,%.2f)\n", shots[k].name, rc, sc.stats.n_tris3d, h[12], h[13], h[14]);
            shots[k].done = 1; shot_pending = -1;
        }
        eyeshot_end(&sc);
    }
    return NULL;
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
    if (v < 2000) {
        if (v >= 97 && v % 97 < 6) b |= 1u << 13;
        if (v >= 211 && v % 211 < 6) b |= 1u << 7;
    } else if (v % 83 < 6) b |= (1u << 13) | (1u << 14);   /* confirmar + cruz: avanza el comunicador y salta la escena inicial (mapa de menús del visor) */
    return b;
}

/* ---- manos sintéticas (espacio del usuario) ---- */
static const char *scn;
static float head_x, head_z;     /* desplazamiento de la cabeza en el plano (asomarse; espacio del usuario, + x derecha, - z delante) */
/* --head-ramp T0:T1:x:y:z : la posicion fisica de la cabeza (espacio del usuario recentrado; y = altura absoluta) pasa linealmente a (x,y,z) entre T0 y T1.
 * --hc : activa el limite de la cabeza contra el escenario (mv_vrhost_head_limit) por VBlank y lo registra ([hc]); las imagenes de --shot se rasterizan desde la
 * cabeza corregida. */
typedef struct { uint32_t t0, t1; float x, y, z; float sx, sy, sz; int started; } HrEv;
static HrEv hramps[16]; static int nhramps;
static float hc_fade;
static double hc_max_corr, hc_max_fade; static uint32_t hc_n_hit, hc_n_fade;
static float prev_ctl[2]; static int have_prev_ctl;
static void hand_set(int side, float x, float y, float z, float grip, float trig) {
    MvVrHand h; memset(&h, 0, sizeof h);
    h.valid = 1; h.rot[3] = 1;
    { /* BR_HROLL / BR_HPITCH (grados): giro de la mano sobre el eje de los dedos (z) y sobre x, para mirar el puño de lado/por la palma (pruebas) */
      const char *er = getenv("BR_HROLL"), *ep = getenv("BR_HPITCH");
      float r = er ? (float)atof(er) * 3.14159265f / 360.0f : 0, pp = ep ? (float)atof(ep) * 3.14159265f / 360.0f : 0;
      float qz[4] = {0, 0, sinf(side ? -r : r), cosf(r)}, qx[4] = {sinf(pp), 0, 0, cosf(pp)};
      h.rot[0] = qx[3] * qz[0] + qx[0] * qz[3]; h.rot[1] = -qx[0] * qz[2]; h.rot[2] = qx[3] * qz[2] + qx[2] * qz[3]; h.rot[3] = qx[3] * qz[3] - qx[0] * qz[0]; }
    h.pos[0] = x; h.pos[1] = y; h.pos[2] = z; h.grip = grip; h.trigger = trig;
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
    float gR = 0, tR = 0, gL = 0;        /* agarre (puño) y gatillo de la mano derecha; puño de la izquierda */
    float lx = -0.2f, ly = head_y - 0.40f, lz = -0.15f;
    float rx = 0.2f, ry = head_y - 0.40f, rz = -0.15f;  /* reposo, mano derecha (a 0,40 m bajo la cabeza) */
    if (scn && (!strcmp(scn, "punch") || !strcmp(scn, "punch_noenemy") || !strcmp(scn, "punch_slow") || !strcmp(scn, "punch_open"))) {
        uint32_t k = t < 230 ? 999 : (t - 230) % 40;
        gR = gL = strcmp(scn, "punch_open") ? 1.0f : 0.0f;   /* punch_open: la MISMA trayectoria con la mano abierta (no debe golpear) */
        /* puños cerrados (el agarre NO es el gatillo): golpear nunca debe agarrar */
        /* golpe realista: 0,6 m en 0,28 s con perfil suave (media 2,1 m/s, pico ~4 m/s) y vuelta en 0,3 s */
        int n = !strcmp(scn, "punch_slow") ? 60 : 14;
        if (k < (uint32_t)n) { float u = (float)(k + 1) / n; rz = lerpf(-0.15f, -0.75f, u * u * (3 - 2 * u)); }
        else if (k < (uint32_t)n + 15) { float u = (float)(k - n + 1) / 15; rz = lerpf(-0.75f, -0.15f, u * u * (3 - 2 * u)); }
    } else if (scn && !strcmp(scn, "fist")) {         /* manos delante de la vista; el agarre (grip) se cierra progresivamente de 230 a 290 y se abre de 420 a 480 */
        rx = 0.14f; lx = -0.14f; ry = ly = head_y - 0.38f; rz = lz = -0.30f;
        float u = t < 230 ? 0.0f : (t < 290 ? (float)(t - 230) / 60.0f : (t < 420 ? 1.0f : (t < 480 ? 1.0f - (float)(t - 420) / 60.0f : 0.0f)));
        gR = gL = u;
    } else if (scn && !strcmp(scn, "grab") && bs && bs->n_enemies > 0) {
        const BridgeEnemy *e = &bs->enemies[0];
        float W[3] = {(float)e->pos.x, (float)e->pos.y + 450.0f, (float)e->pos.z}, u[3];
        if (t >= 215 && world_to_user(W, u)) { rx = u[0] + 0.05f; ry = u[1]; rz = u[2]; lx = u[0] - 0.05f; ly = u[1]; lz = u[2]; }
        if (t >= 225 && t < 360) tR = 1.0f;                                 /* AGARRAR = gatillo con la mano en el cuello */
        if ((t >= 300 && t < 308) || (t >= 322 && t < 330)) tR = 0.1f;      /* aflojar y apretar de nuevo: toques */
    } else if (scn && !strcmp(scn, "knock")) {
        uint32_t k = t < 70 ? 999 : (t - 70) % 40;
        if (k < 6) rz = lerpf(-0.15f, 0.4f, (float)(k + 1) / 6);
        else if (k < 16) rz = lerpf(0.4f, -0.15f, (float)(k - 5) / 10);
    }
    hand_set(1, rx, ry, rz, gR, tR);
    hand_set(0, lx, ly, lz, gL, 0);
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
    for (int i = 0; i < nturns; i++)
        if (t >= turns[i].t && t < turns[i].t + turns[i].dur) mv_vr_turn(turns[i].deg / (float)turns[i].dur);
    for (int i = 0; i < nhy; i++) if (t >= hyev[i].t) hyaw_deg = hyev[i].deg;
    for (int i = 0; i < nramps; i++) if (t >= ramps[i].t0 && t <= ramps[i].t1) hyaw_deg = ramps[i].g0 + (ramps[i].g1 - ramps[i].g0) * (float)(t - ramps[i].t0) / (float)(ramps[i].t1 > ramps[i].t0 ? ramps[i].t1 - ramps[i].t0 : 1);
    if (nsticks || nsticksr) {
        if (!vin_init) { vr_input_init(&vin); vri_cfg_t c = vin.cfg; c.mode = VRI_MODE_SENSE; c.turn_mode = VRI_TURN_SMOOTH; if (loco_hand) c.loco_ref = VRI_LOCO_HAND; vr_input_set_cfg(&vin, &c); vin_init = 1; }
        float sx = 0, sy = 0, rx2 = 0;
        for (int i = 0; i < nsticks; i++) if (t >= sticks[i].t && t < sticks[i].t + sticks[i].dur) { sx = sticks[i].x; sy = sticks[i].y; }
        for (int i = 0; i < nsticksr; i++) if (t >= sticksr[i].t && t < sticksr[i].t + sticksr[i].dur) rx2 = sticksr[i].x;
        vr_input_frame_t f; memset(&f, 0, sizeof f);
        float a = hyaw_deg * 3.14159265f / 180.0f;
        f.dt = 0.02f; f.head_valid = true; f.game_posture = bs ? bs->stance : -1;
        f.head_quat[1] = sinf(a * 0.5f); f.head_quat[3] = cosf(a * 0.5f); f.head_pos[1] = head_y;
        f.hand[0].tracked = true; f.hand[0].stick_x = sx; f.hand[0].stick_y = sy;
        { float h = hand_yaw_deg * 3.14159265f / 180.0f; f.hand[0].quat[1] = sinf(h * 0.5f); f.hand[0].quat[3] = cosf(h * 0.5f); f.hand[0].pos[1] = head_y - 0.4f; }
        f.hand[1].tracked = true; f.hand[1].stick_x = rx2; f.hand[1].quat[3] = 1; f.hand[1].pos[1] = head_y - 0.4f;
        f.menu = !(bs && bs->in_control);
        vr_input_set_game_up(&vin, vr_input_quat_heading(f.head_quat) + vr_input_body_yaw(&vin));
        float by0 = vr_input_body_yaw(&vin);
        vr_pad_t out; vr_input_update(&vin, &f, &out);
        float dy = vr_input_body_yaw(&vin) - by0;
        while (dy > 3.14159265f) dy -= 6.2831853f; while (dy <= -3.14159265f) dy += 6.2831853f;
        if (dy != 0) mv_vr_turn(-dy * 180.0f / 3.14159265f);
        mv_host_pad(out.buttons, out.lx, out.ly, out.rx, out.ry);
    }
    if (loco_log && bs && vrcam_active()) {
        const VrCamRecord *rr = vrcam_last_record();
        float me[3];
        if (rr && rr->active && rr->serial != last_serial && vrcam_model_eye(emu_ram, rr->yaw_deg, me)) {
            float dxz = hypotf((float)rr->eye[0] - me[0], (float)rr->eye[2] - me[2]);
            float dp = have_prev_model ? hypotf((float)rr->eye[0] - prev_model[0], (float)rr->eye[2] - prev_model[2]) : -1;
            float dy3 = (float)rr->eye[1] - me[1];
            float mvx = have_prev_ctl ? (float)bs->ctl_pos.x - prev_ctl[0] : 0, mvz = have_prev_ctl ? (float)bs->ctl_pos.z - prev_ctl[1] : 0;
            float spd = hypotf(mvx, mvz), mdir = spd > 0.5f ? atan2f(mvx, mvz) * 57.29578f : 0; if (mdir < 0) mdir += 360.0f;
            prev_ctl[0] = (float)bs->ctl_pos.x; prev_ctl[1] = (float)bs->ctl_pos.z; have_prev_ctl = 1;
            fprintf(stderr, "[loco] t=%u ser=%u stance=%d act=%d mov=%d wall=%d ojo-modelo(xz)=%.1f (vs modelo del fotograma anterior %.1f) dy=%.1f ctl=(%d,%d) vel=%.1f rumbo-marcha=%.1f cuerpo=%.1f vista=%.1f cabeza=%.1f\n", t, rr->serial, bs->stance, bs->player_action, bs->moving, bs->on_wall,
                    dxz, dp, dy3, bs->ctl_pos.x, bs->ctl_pos.z, spd, mdir, (float)((uint16_t)bs->ctl_rot.y & 4095u) * (360.0f / 4096.0f), vrcam_view_yaw_deg(), hyaw_deg);
            sum_cam += dxz; if (dxz > max_cam) max_cam = dxz; if (dp >= 0) sum_prev += dp; n_cam++;
            last_serial = rr->serial;
        }
        if (vrcam_model_eye(emu_ram, vrcam_view_yaw_deg(), me)) { memcpy(prev_model, me, sizeof me); have_prev_model = 1; }
    }
    for (int i = 0; i < npi; i++) if (t >= piev[i].t) hpitch_deg = piev[i].deg;
    for (int i = 0; i < npokes; i++)
        if (t == pokes[i].t && bs && bs->ctl_addr) {
            int16_t v = (int16_t)pokes[i].units;
            memcpy(emu_ram + ((bs->ctl_addr + 0x0A) & 0x1FFFFF), &v, 2);
            if (pokes[i].both) memcpy(emu_ram + ((bs->ctl_addr + 0x2A) & 0x1FFFFF), &v, 2);
            if (pokes[i].both >= 2) memcpy(emu_ram + ((bs->ctl_addr + 0x4E) & 0x1FFFFF), &v, 2);
        }
    for (int i = 0; i < nshots; i++) if (!shots[i].done && t >= shots[i].t && shot_pending < 0) shot_pending = i;
    if (headlog_every && t % headlog_every == 0 && bs) {
        const VrCalib *cal = vrcam_calib();
        const VrCamRecord *rr = vrcam_last_record();
        float bw = (float)((uint16_t)bs->ctl_rot.y & 4095u) * (360.0f / 4096.0f), vw = vrcam_view_yaw_deg();
        float off = vw - bw; while (off > 180) off -= 360; while (off < -180) off += 360;
        fprintf(stderr, "[head] t=%u cuerpo=%.1f vista=%.1f desvio=%.1f cabeza=%.1f theta=%.1f sentado=%d escala=%.3f upm=%.1f pref_y=%.2f pos=(%d,%d,%d) mov=%d\n", t, bw, vw, off, hyaw_deg,
                rr ? rr->theta_play_deg : 0.0f, cal->seated, cal->scale, rr ? rr->units_per_m : 0.0f, rr ? rr->p_ref[1] : 0.0f, bs->ctl_pos.x, bs->ctl_pos.y, bs->ctl_pos.z, bs->moving);
    }
    if (getenv("BR_DUMPCTL") && headlog_every && t % headlog_every == 0 && bs && bs->ctl_addr) {
        fprintf(stderr, "[ctl] t=%u:", t);
        for (int k = 0; k < 0x80; k += 2) { int16_t w; memcpy(&w, emu_ram + ((bs->ctl_addr + (uint32_t)k) & 0x1FFFFF), 2); fprintf(stderr, " %d", w); }
        fputc('\n', stderr);
    }
    if (scn && !strcmp(scn, "peek")) head_x = t >= 80 && t < 140 ? 0.2f : 0.0f;
    for (int i = 0; i < nhramps; i++) {
        HrEv *e = &hramps[i];
        if (t < e->t0) continue;
        if (!e->started) { e->started = 1; e->sx = head_x; e->sy = head_y; e->sz = head_z; }
        float k = t >= e->t1 || e->t1 <= e->t0 ? 1.0f : (float)(t - e->t0) / (float)(e->t1 - e->t0);
        head_x = e->sx + (e->x - e->sx) * k; head_y = e->sy + (e->y - e->sy) * k; head_z = e->sz + (e->z - e->sz) * k;
    }
    set_head(head_x, head_y, head_z);
    if (use_hc) {
        float h[16], c[3] = {0, 0, 0}, fd = 0;
        pthread_mutex_lock(&head_mx); memcpy(h, cur_head, sizeof h); pthread_mutex_unlock(&head_mx);
        int r = mv_vrhost_head_limit(h, 0.02f, c, &fd);
        memcpy(hc_corr, c, sizeof c); hc_fade = fd;
        float cl = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        if (cl > hc_max_corr) hc_max_corr = cl;
        if (fd > hc_max_fade) hc_max_fade = fd;
        if (cl > 1e-4f) hc_n_hit++;
        if (fd > 0.01f) hc_n_fade++;
        if (r && (t % 4 == 0)) fprintf(stderr, "[hc] t=%u cabeza=(%.3f,%.3f,%.3f) corr=(%.3f,%.3f,%.3f) |corr|=%.3f fade=%.2f\n", t, h[12], h[13], h[14], c[0], c[1], c[2], cl, fd);
    }
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
        else if (!strcmp(argv[i], "--head-y") && i + 1 < argc) head_y = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--headlog") && i + 1 < argc) headlog_every = (uint32_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--turn") && i + 1 < argc && nturns < 32) { unsigned tt = 0, dd = 1; float dg = 0; sscanf(argv[++i], "%u:%f:%u", &tt, &dg, &dd); turns[nturns++] = (TurnEv){tt, dd ? dd : 1, dg}; }
        else if (!strcmp(argv[i], "--hyaw-at") && i + 1 < argc && nhy < 32) { unsigned tt = 0; float dg = 0; sscanf(argv[++i], "%u:%f", &tt, &dg); hyev[nhy++] = (HyEv){tt, dg}; }
        else if (!strcmp(argv[i], "--hpitch-at") && i + 1 < argc && npi < 32) { unsigned tt = 0; float dg = 0; sscanf(argv[++i], "%u:%f", &tt, &dg); piev[npi++] = (PiEv){tt, dg}; }
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc && nshots < 16) { unsigned tt = 0; char nm[64] = "shot"; sscanf(argv[++i], "%u:%63s", &tt, nm); shots[nshots].t = tt; snprintf(shots[nshots].name, sizeof shots[nshots].name, "%s", nm); nshots++; }
        else if (!strcmp(argv[i], "--poke") && i + 1 < argc && npokes < 16) { unsigned tt = 0; int u = 0, b = 0; sscanf(argv[++i], "%u:%d:%d", &tt, &u, &b); pokes[npokes++] = (PokeEv){tt, u, b}; }
        else if (!strcmp(argv[i], "--nohead")) no_heading = 1;
        else if (!strcmp(argv[i], "--stick") && i + 1 < argc && nsticks < 32) { unsigned tt = 0, dd = 1; float x = 0, y = 0; sscanf(argv[++i], "%u:%u:%f:%f", &tt, &dd, &x, &y); sticks[nsticks++] = (StEv){tt, dd, x, y}; }
        else if (!strcmp(argv[i], "--stickr") && i + 1 < argc && nsticksr < 32) { unsigned tt = 0, dd = 1; float x = 0; sscanf(argv[++i], "%u:%u:%f", &tt, &dd, &x); sticksr[nsticksr++] = (StEv){tt, dd, x, 0}; }
        else if (!strcmp(argv[i], "--hyaw-ramp") && i + 1 < argc && nramps < 16) { unsigned a = 0, b = 0; float g0 = 0, g1 = 0; sscanf(argv[++i], "%u:%u:%f:%f", &a, &b, &g0, &g1); ramps[nramps++] = (RampEv){a, b, g0, g1}; }
        else if (!strcmp(argv[i], "--loco-log")) loco_log = 1;
        else if (!strcmp(argv[i], "--hc")) use_hc = 1;
        else if (!strcmp(argv[i], "--head-ramp") && i + 1 < argc && nhramps < 16) { unsigned a = 0, b = 0; float x = 0, y = head_y, z = 0; sscanf(argv[++i], "%u:%u:%f:%f:%f", &a, &b, &x, &y, &z); hramps[nhramps++] = (HrEv){a, b, x, y, z, 0, 0, 0, 0}; }
        else if (!strcmp(argv[i], "--loco-hand") && i + 1 < argc) { loco_hand = 1; hand_yaw_deg = (float)atof(argv[++i]); }
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
    if (no_heading) { VrHeadingConfig hc; vrhead_config_default(&hc); hc.enabled = 0; vrhead_set_default(&hc); }
    vrgame_set_driver(driver);
    mv_host_set_turbo(1);
    pthread_t pt; int have_pt = 0;
    if (nshots || use_hc) have_pt = pthread_create(&pt, NULL, poller, NULL) == 0;
    int r = mv_host_start(argv[1], argv[2], "build-host/card");
    if (r) { fprintf(stderr, "mv_host_start = %d: %s\n", r, mv_host_error()); return 1; }
    for (;;) {
        if (mv_host_state() == MV_HOST_ERROR) { fprintf(stderr, "error del motor: %s\n", mv_host_error()); return 1; }
        if (mv_host_frame_count() >= nframes) break;
        usleep(2000);
    }
    mv_host_pause();
    usleep(100000);
    poll_run = 0; if (have_pt) pthread_join(pt, NULL);
    fprintf(stderr, "[bodyrun] eventos de gestos: golpe %u agarre %u apretón %u soltar %u pared(golpe) %u asomar %u contacto-pared %u daño %u impacto %u golpe-botón %u\n", vrbodyg_event_count(VRG_EV_PUNCH), vrbodyg_event_count(VRG_EV_GRAB),
            vrbodyg_event_count(VRG_EV_SQUEEZE), vrbodyg_event_count(VRG_EV_RELEASE), vrbodyg_event_count(VRG_EV_KNOCK), vrbodyg_event_count(VRG_EV_PEEK), vrbodyg_event_count(VRG_EV_WALL), vrbodyg_event_count(VRG_EV_HURT), vrbodyg_event_count(VRG_EV_IMPACT), vrbodyg_event_count(VRG_EV_JAB));
    { VrBodyStats bst; vrbody_stats(&bst); fprintf(stderr, "[bodyrun] cuerpo: apply %u brazos %u puños %u malla-no-reconocida %u\n", bst.applies, bst.arms_driven, bst.fist_applies, bst.fist_skipped); }
    if (use_hc) fprintf(stderr, "[hc-resumen] VBlanks con recorte=%u con fundido=%u corr max=%.3f m fade max=%.2f\n", hc_n_hit, hc_n_fade, hc_max_corr, hc_max_fade);
    if (n_cam > 0) fprintf(stderr, "[loco-resumen] construcciones=%.0f distancia ojo-modelo(xz) media=%.1f max=%.1f unidades (vs modelo del fotograma anterior: media=%.1f)\n", n_cam, sum_cam / n_cam, max_cam, sum_prev / n_cam);
    return 0;
}
