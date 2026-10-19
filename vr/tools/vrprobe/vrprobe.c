/* vrprobe: PROBADOR de la capa VR sobre el host completo (mismo camino que la app: vrhost + vr_scene + vrcam + cuerpo + jugabilidad, en MV_NOGEN,
 * intérprete) con datos LOCALES del usuario. Herramienta local: no entra en la app (stage.sh no copia vr/tools) ni en CI; nada de datos del juego
 * en el repo (los discos y los nombres de zona se leen de las rutas que se le pasan).
 *
 *   MV_SIGS=firmas.json vrprobe <exe> <disco> [--out DIR] [--zonas a,b,c | --zonas-fichero F] [--solo grupo[,grupo]] [--th nombre=valor]...
 *
 * Qué hace (ver bitacora.md, sección "Probador VR"):
 *   título -> partida -> zona inicial (+ las zonas pedidas con la receta de cambio de zona) y en cada punto, con un guion fijo y determinista (todo
 *   dentro del gancho de VBlank del hilo de emulación, con el consumidor de escenas sincronizado a paso):
 *     barrido    cabeza 360° en guiñada (pasos de 15°) y cabeceo 0, ±30, ±60 con la cabeza quieta en cada vista;
 *     giro_c     giro continuo de la cabeza (125°/s);
 *     giro_s     giro con el stick suave (120°/s, 360°) y giro_p por pasos (12 x 30°) con la cabeza quieta;
 *     manos      mandos sintéticos con el usuario de pie (cabeza 1,6 m) y sentado (1,04 m): mirar abajo/delante/lados;
 *     rend       VBlank/s y ms de ensamblado de escena.
 *   Por vista proyecta la escena VR (lo que dibujaría el renderer, por ojo, FOV 100°, IPD 63 mm, el mundo VR tal cual como espacio del usuario)
 *   con un rasterizador de cobertura por CPU y compara con una referencia omnidireccional: la unión de la geometría de todas las vistas del
 *   barrido de ese instante (en coordenadas fijas del juego), vista desde la misma pose. Mide:
 *     huecos      % de píxeles del campo de visión donde la referencia tiene geometría y la escena no (fondo donde debería haber mundo);
 *     registro    error de posición (cm) y de mirada (grados) entre la cámara del motor en la escena y la cabeza del usuario, y retardo de cámara;
 *     descartes   polígonos 3D emitidos por el juego frente a los que quedan en la escena, y los descartes de vr_scene por motivo;
 *     manos       error (cm) entre la muñeca del mando (espacio del usuario) y la muñeca del personaje tal como queda dibujada en la escena,
 *                 y si hay geometría de brazo/mano dibujada ahí;
 *     rendimiento VBlank/s y ms de vr_scene_build.
 *   Salida: informe de texto con PASA/FALLA por umbral (stdout y OUT/informe.txt), hojas PNG en OUT. Código de salida 0 = todo PASA.
 */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "bridge.h"
#include "bridge_addrs.h"
#include "cpu.h"
#include "host.h"
#include "probe_raster.h"
#include "sys.h"
#include "vr_body.h"
#include "vr_camera.h"
#include "vrgame.h"
#include "vrhost.h"

/* ------------------------------------------------------------------------------------------------ constantes */
#define RAMM 0x1FFFFFu
#define RING 8192
#define SETTLE 6                /* fotogramas de VBlank que se esperan tras mover la cabeza antes de medir una vista estática */
#define VIEW_FRAMES 12
#define EYE_W 128               /* resolución por ojo del rasterizador de cobertura */
#define TILE 64
#define IPD 0.063f
#define FOV_DEG 100.0f
#define H_STAND 1.60f
#define H_SIT 1.04f
#define NYAW 24
#define NPITCH 5
#define LAG_PRESENT 1           /* fotogramas de VBlank entre la pose con la que se construye la escena y la pose con la que se presenta */
#define MAXZ 8

enum { CK_NONE = 0, CK_SWEEP, CK_HSMOOTH, CK_TSMOOTH, CK_TSTEPS, CK_HANDS };
enum { SG_BOOT, SG_ZONE, SG_SWEEP, SG_HSMOOTH, SG_TSMOOTH, SG_TSTEPS, SG_HANDS, SG_PERF, SG_FLUSH, SG_END };
static const char *CK_NAME[] = {"-", "barrido", "giro_c", "giro_s", "giro_p", "manos"};
static const float PITCHES[NPITCH] = {0, -30, 30, -60, 60};

/* ------------------------------------------------------------------------------------------------ umbrales */
typedef struct {
    double hole_max, hole_mean, hole_p95;   /* % del campo de visión */
    double lag_deg;                 /* desajuste angular dinámico de la escena con la cabeza girando (p95, grados) */
    double reg_pos_cm, reg_gaze_deg, conv_frames;
    double disc_share;              /* fracción máxima de polígonos 3D emitidos que se descartan */
    double hand_err_cm, hand_ok_frac;
    double vblank_min, build_avg_ms, build_max_ms;
} Thr;
static Thr TH = {25.0, 2.0, 6.0, 15.0, 6.0, 3.0, 6.0, 0.15, 8.0, 0.8, 35.0, 2.0, 100.0};
static const struct { const char *n; double *p; } THN[] = {
    {"hueco_max", &TH.hole_max}, {"hueco_medio", &TH.hole_mean}, {"hueco_p95", &TH.hole_p95}, {"desajuste_grados", &TH.lag_deg}, {"reg_pos_cm", &TH.reg_pos_cm}, {"reg_mirada_grados", &TH.reg_gaze_deg},
    {"retardo_camara", &TH.conv_frames}, {"descartes", &TH.disc_share}, {"mano_err_cm", &TH.hand_err_cm}, {"mano_ok", &TH.hand_ok_frac},
    {"vblank_min", &TH.vblank_min}, {"escena_ms_medio", &TH.build_avg_ms}, {"escena_ms_max", &TH.build_max_ms},
};

/* ------------------------------------------------------------------------------------------------ intención por VBlank (hilo de emulación -> consumidor) */
typedef struct {
    uint32_t f;                  /* VBlank (sys_vblank_count) al escribir; 0 = vacío. Se escribe el último */
    int16_t zone, kind, view, age;
    uint8_t store;               /* 0 nada, 1 solo metadatos, 2 metadatos y triángulos */
    uint8_t posture;             /* 0 de pie, 1 sentado */
    uint8_t hands;
    float head[16];
    float yaw, pitch;
    float turn_total;
    float hp[2][3], hq[2][4];    /* mandos [0]=izq, [1]=der (espacio del usuario) */
    int drv[2];
    float wrist[2][3], target[2][3], ik_err[2], fade[2];   /* vrbody_debug: muñeca del personaje en el mundo del motor (mm) */
} Intent;
static Intent ring[RING];
static _Atomic uint32_t g_hook_f;
static _Atomic uint32_t g_cons;            /* último VBlank cuya escena consumió el hilo principal */
static _Atomic int g_flush_req = -1, g_flush_done = -1, g_done, g_lockstep;

/* ------------------------------------------------------------------------------------------------ capturas */
typedef struct {
    Intent in;
    uint32_t hf;
    float eye_vr[3], org[3], scale, fwd[3];
    int world_valid, has3d;
    VrStats st;
    uint32_t ntri, nsemi;
    float *tri;                  /* ntri * 9, mundo VR de ESA escena */
} Cap;

typedef struct {
    float *t;                    /* 9 floats por triángulo, coordenadas FIJAS del juego (unidades del motor) */
    size_t n, cap;
    uint64_t *hs;                /* tabla hash de claves */
    size_t hmask;
} Ref;

typedef struct { float hole_l, hole_r, view_l, view_r, ref_l, ref_r; int drawn; float gaze, pos; int valid; } Ev;

typedef struct {
    char name[16];
    int reached;
    Cap *c;
    size_t n, cap;
    Ev *ev;
    uint8_t **thumb;             /* una miniatura por captura evaluada (o NULL) */
    int tsz;
    /* rendimiento */
    double vblank_s, build_avg_ms, build_max_ms; int perf_ok;
    uint32_t seen_scenes;
} Zone;
static Zone Z[MAXZ];
static int nzones;
static int g_zs = 1;             /* -1 si el mundo del juego es zurdo (espejo en Z de vr_scene) */
static int g_zs_known;

static int fails;
static int G_CONT = 1, G_STICK = 1, G_MANOS = 1, G_REND = 1, G_CONTROL = 1;
static FILE *g_rep;
static char g_out[512] = ".";
#define REP(...) do { printf(__VA_ARGS__); if (g_rep) fprintf(g_rep, __VA_ARGS__); } while (0)

/* ------------------------------------------------------------------------------------------------ utilidades */
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }

static void head_matrix(float yaw_deg, float pitch_deg, float x, float y, float z, float m[16]) {
    float yw = yaw_deg * 0.01745329252f, pt = pitch_deg * 0.01745329252f;
    float cy = cosf(yw), sy = sinf(yw), cp = cosf(pt), sp = sinf(pt);
    float c[16] = {cy, 0, -sy, 0,   sy * sp, cp, cy * sp, 0,   sy * cp, -sp, cy * cp, 0,   x, y, z, 1};
    memcpy(m, c, sizeof c);
}
static void quat_rot(const float q[4], const float v[3], float o[3]) {   /* q = x,y,z,w */
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float n = sqrtf(x * x + y * y + z * z + w * w);
    if (n < 1e-6f) { memcpy(o, v, 3 * sizeof(float)); return; }
    x /= n; y /= n; z /= n; w /= n;
    float tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    o[0] = v[0] + w * tx + (y * tz - z * ty);
    o[1] = v[1] + w * ty + (z * tx - x * tz);
    o[2] = v[2] + w * tz + (x * ty - y * tx);
}
static float angle_between(const float a[3], const float b[3]) {
    float la = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), lb = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    if (la < 1e-9f || lb < 1e-9f) return 180.0f;
    float d = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb);
    if (d > 1) d = 1;
    if (d < -1) d = -1;
    return acosf(d) * 57.29578f;
}
static int cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }
typedef struct { double mean, p50, p95, max; int n; } Stat;
static Stat stat_of(float *v, int n) {
    Stat s = {0, 0, 0, 0, n};
    if (n <= 0) return s;
    qsort(v, (size_t)n, sizeof *v, cmpf);
    double sum = 0;
    for (int i = 0; i < n; i++) sum += v[i];
    s.mean = sum / n; s.p50 = v[n / 2]; s.p95 = v[(int)((n - 1) * 0.95)]; s.max = v[n - 1];
    return s;
}

/* ------------------------------------------------------------------------------------------------ guion (hilo de emulación) */
typedef struct { int type, zone, arg; } Seg;
static Seg PROG[12 * MAXZ + 8];
static int nprog;
static int cur, segk;
static int in_call;
static const char *g_zname[MAXZ];

/* pose ordenada */
static float cmd_yaw, cmd_pitch;
static int cmd_posture;          /* 0 de pie, 1 sentado */
static int cmd_hands;
static float turn_total;
static int pose_age;
static int boot_ok = 1;
static uint32_t boot_frame;
static int zone_ok_flag[MAXZ];

/* arranque: mismo guion de mando que vrhost_run (título -> partida) */
typedef struct { uint32_t start, dur, every; uint16_t mask; } PadEv;
static const PadEv PADB[] = {{120, 6, 0, 1u << 3}, {400, 6, 0, 1u << 3}, {0, 6, 97, 1u << 13}, {0, 6, 211, 1u << 7}};
static uint16_t boot_pad(uint32_t f) {
    uint16_t b = 0;
    for (unsigned i = 0; i < sizeof PADB / sizeof PADB[0]; i++) {
        const PadEv *p = &PADB[i];
        if (p->every) { if (f >= p->every && f % p->every < p->dur) b |= p->mask; }
        else if (f >= p->start && f < p->start + p->dur) b |= p->mask;
    }
    return b;
}

static BridgeState g_bs;
static MvVrGameState g_gs;

/* cambio de zona (receta de la bitácora: registrar la zona y pedir la carga) */
static void stage_register(Cpu *c, const char *name) {
    const uint32_t buf = 0x800B5F18u;
    memset(emu_ram + (buf & RAMM), 0, 8);
    memcpy(emu_ram + (buf & RAMM), name, strlen(name) < 8 ? strlen(name) : 8);
    Cpu save = *c;
    uint32_t sp = 0x801FFF00u;
    c->r[29] = sp; c->r[4] = buf;
    in_call = 1; rt_call(c, 0x80016DE8u); in_call = 0;
    uint32_t h = c->r[2] & 0xFFFFu;
    *c = save;
    c->r[29] = sp; c->r[4] = h; c->r[5] = buf;
    in_call = 1; rt_call(c, 0x8002B124u); in_call = 0;
    *c = save;
}

static void hands_pose(int posture, MvVrHand out[2]) {
    float y = posture ? H_SIT - 0.42f : H_STAND - 0.48f;
    memset(out, 0, 2 * sizeof *out);
    for (int s = 0; s < 2; s++) {
        out[s].valid = 1;
        out[s].pos[0] = s ? 0.22f : -0.22f; out[s].pos[1] = y; out[s].pos[2] = -0.28f;
        out[s].rot[3] = 1.0f;
    }
}

static void lockstep_wait(uint32_t f) {
    double t0 = now_s();
    while (atomic_load(&g_lockstep) && (int32_t)(f - atomic_load(&g_cons)) > 8 && now_s() - t0 < 2.0) usleep(100);
}

static void put_head(float yaw, float pitch, float y, float m[16], uint32_t f) {
    head_matrix(yaw, pitch, 0, y, 0, m);
    mv_vr_set_head(m, (double)f * 0.02);
}

static void seg_next(void) { cur++; segk = 0; pose_age = 0; }

static void step(Cpu *c, uint32_t f) {
    Seg *sg = &PROG[cur];
    float hy = cmd_posture ? H_SIT : H_STAND;
    int store = 0, kind = CK_NONE, view = 0;
    int advance = 0;
    atomic_store(&g_lockstep, 0);

    mv_vrhost_game_state(&g_gs);
    int have_b = mv_host_bridge(&g_bs);
    int ctl = g_gs.game && g_gs.in_control && g_gs.cam_active;

    switch (sg->type) {
    case SG_BOOT: {
        static int stable;
        mv_host_pad(ctl && stable > 5 ? 0 : boot_pad(f), 128, 128, 128, 128);
        stable = ctl ? stable + 1 : 0;
        if (stable > 90) { mv_host_pad(0, 128, 128, 128, 128); boot_frame = f; advance = 1; }
        else if (f > 20000) { fprintf(stderr, "[vrprobe] la partida no arranca en 20000 VBlanks\n"); boot_ok = 0; cur = nprog - 1; segk = 0; return; }
        break;
    }
    case SG_ZONE: {
        static int stable, issued;
        const char *name = g_zname[sg->zone];
        if (sg->zone > 0) {
            if (segk == 0) { issued = 0; stable = 0; }
            if (segk == 0) { stage_register(c, name); fprintf(stderr, "[vrprobe] f=%u: zona pedida (registro)\n", f); }
            if (segk == 2) { *(uint32_t *)(emu_ram + (0x800AC574u & RAMM)) = 0x81000000u; issued = 1; }
            int here = have_b && !strncmp(g_bs.stage_code, name, 8);
            /* escenas de apertura de la zona: pulsaciones de circulo y cruz para pasarlas (como en el arranque) */
            if (issued && !ctl) { uint16_t pb = 0; int m = segk % 40; if (m < 6) pb |= 1u << 13; if (m >= 20 && m < 26) pb |= 1u << 14; mv_host_pad(pb, 128, 128, 128, 128); }
            else mv_host_pad(0, 128, 128, 128, 128);
            if (getenv("VRPROBE_DEBUG") && segk % 300 == 0)
                fprintf(stderr, "[vrprobe] zona %s k=%d f=%u: código='%s' game=%d ctl=%d cam=%d cine=%d comms=%d pausa=%d menu=%d stop=%d has_player=%d valid=%x vista=%d motivo=%d\n", name, segk, f, have_b ? g_bs.stage_code : "?",
                        g_gs.game, g_gs.in_control, g_gs.cam_active, g_bs.cinematic, g_bs.comms, g_bs.paused, g_bs.menu_open, g_bs.stopped, g_bs.has_player, g_bs.valid, g_gs.view_mode, g_gs.view_reason);
            stable = (issued && here && ctl) ? stable + 1 : 0;
            if (stable > 120) { advance = 1; }
            else if (segk > 8000) { fprintf(stderr, "[vrprobe] zona %s: no se llegó al control en 8000 VBlanks (código actual '%s')\n", name, have_b ? g_bs.stage_code : "?"); zone_ok_flag[sg->zone] = 0; advance = 1; }
        } else {
            stable = ctl ? stable + 1 : 0;
            if (segk >= 90 && stable > 60) advance = 1;
            else if (segk > 3000) { advance = 1; }
        }
        if (advance) {
            snprintf(Z[sg->zone].name, sizeof Z[sg->zone].name, "%s", have_b ? g_bs.stage_code : "?");
            Z[sg->zone].reached = zone_ok_flag[sg->zone] || sg->zone == 0;
            cmd_posture = 0; cmd_yaw = cmd_pitch = 0; cmd_hands = 1;
            mv_vr_recenter();
        }
        break;
    }
    case SG_SWEEP: {
        int nv = NYAW * NPITCH;
        int v = segk / VIEW_FRAMES, a = segk % VIEW_FRAMES;
        if (v >= nv) { advance = 1; break; }
        cmd_yaw = (float)(v % NYAW) * (360.0f / NYAW);
        cmd_pitch = PITCHES[v / NYAW];
        pose_age = a;
        store = (a >= SETTLE - 1 && a <= SETTLE + 1) ? 2 : 1;
        kind = CK_SWEEP; view = v;
        atomic_store(&g_lockstep, 1);
        break;
    }
    case SG_HSMOOTH: {
        const int n = 144;
        if (segk >= n) { advance = 1; break; }
        cmd_yaw = (float)segk * 2.5f; cmd_pitch = 0;
        pose_age = SETTLE; store = 2; kind = CK_HSMOOTH; view = segk;
        atomic_store(&g_lockstep, 1);
        break;
    }
    case SG_TSMOOTH: {
        const int n = 150;   /* 150 x 2,4° = 360° (120°/s a 50 Hz) */
        if (segk >= n) { advance = 1; break; }
        cmd_yaw = 0; cmd_pitch = 0;
        if (segk > 0) { mv_vr_turn(2.4f); turn_total += 2.4f; }
        pose_age = SETTLE; store = 2; kind = CK_TSMOOTH; view = segk;
        atomic_store(&g_lockstep, 1);
        break;
    }
    case SG_TSTEPS: {
        const int per = 10, steps = 12;
        if (segk >= per * steps + 4) { advance = 1; break; }
        cmd_yaw = 0; cmd_pitch = 0;
        if (segk > 0 && segk % per == 0 && segk / per <= steps) { mv_vr_turn(30.0f); turn_total += 30.0f; pose_age = 0; }
        else pose_age = pose_age < 30 ? pose_age + 1 : 30;
        store = 2; kind = CK_TSTEPS; view = segk;
        atomic_store(&g_lockstep, 1);
        break;
    }
    case SG_HANDS: {
        /* sg->zone codifica la zona y la postura: se usa hands_posture para la postura; 5 vistas de 40 VBlanks tras 30 de fundido */
        static const float VY[5] = {0, 0, 0, 90, -90}, VP[5] = {0, -60, -35, 0, 0};
        int hp = sg->arg;
        int nview = 5, pre = 30, per = 40;
        if (segk == 0) { cmd_posture = hp; cmd_yaw = cmd_pitch = 0; cmd_hands = 1; mv_vr_recenter(); hy = hp ? H_SIT : H_STAND; }
        if (segk < pre) { pose_age = 0; kind = CK_NONE; break; }
        int v = (segk - pre) / per, a = (segk - pre) % per;
        if (v >= nview) { advance = 1; break; }
        cmd_yaw = VY[v]; cmd_pitch = VP[v];
        pose_age = a;
        store = a >= 28 ? 2 : (a >= SETTLE ? 1 : 0);
        kind = CK_HANDS; view = v;
        atomic_store(&g_lockstep, 1);
        break;
    }
    case SG_PERF: {
        static double t0; static uint32_t f0; static MvVrHostStats s0;
        const int n = 400;
        cmd_yaw = cmd_pitch = 0; cmd_posture = 0;
        if (segk == 0) { t0 = now_s(); f0 = f; mv_vrhost_stats(&s0); }
        if (segk >= n) {
            MvVrHostStats s1; mv_vrhost_stats(&s1);
            double dt = now_s() - t0;
            Zone *zz = &Z[sg->zone];
            zz->vblank_s = (double)(f - f0) / dt;
            uint32_t nb = s1.built - s0.built;
            zz->build_avg_ms = nb ? (s1.build_ms_avg * s1.built - s0.build_ms_avg * s0.built) / nb : 0;
            zz->build_max_ms = s1.build_ms_max;
            zz->perf_ok = 1;
            advance = 1;
        }
        break;
    }
    case SG_FLUSH: {
        if (segk == 0) { atomic_store(&g_flush_req, sg->zone); }
        double t0 = now_s();
        while (atomic_load(&g_flush_done) != sg->zone && now_s() - t0 < 600.0) {
            float m[16]; put_head(cmd_yaw, cmd_pitch, hy, m, f);
            usleep(20000);
        }
        advance = 1;
        break;
    }
    case SG_END:
        atomic_store(&g_done, 1);
        break;
    default: break;
    }

    if (store || sg->type == SG_SWEEP || sg->type == SG_HANDS) lockstep_wait(f);

    /* cabeza y manos de este VBlank */
    hy = cmd_posture ? H_SIT : H_STAND;
    float m[16];
    put_head(cmd_yaw, cmd_pitch, hy, m, f);
    MvVrHand hh[2];
    if (cmd_hands) {
        hands_pose(cmd_posture, hh);
        mv_vrgame_set_hand(0, &hh[0]);
        mv_vrgame_set_hand(1, &hh[1]);
    }

    Intent *in = &ring[f % RING];
    memset(in, 0, sizeof *in);
    in->zone = (int16_t)sg->zone; in->kind = (int16_t)kind; in->view = (int16_t)view; in->age = (int16_t)pose_age;
    in->store = (uint8_t)store; in->posture = (uint8_t)cmd_posture; in->hands = (uint8_t)cmd_hands;
    memcpy(in->head, m, sizeof m);
    in->yaw = cmd_yaw; in->pitch = cmd_pitch; in->turn_total = turn_total;
    if (cmd_hands) for (int s = 0; s < 2; s++) {
        memcpy(in->hp[s], hh[s].pos, sizeof in->hp[s]);
        memcpy(in->hq[s], hh[s].rot, sizeof in->hq[s]);
        const VrBodyDebug *d = vrbody_debug(s);
        if (d) {
            in->drv[s] = d->driven; in->fade[s] = d->w; in->ik_err[s] = d->sol.err_mm;
            memcpy(in->wrist[s], d->sol.wrist, sizeof in->wrist[s]);
            memcpy(in->target[s], d->target_wrist, sizeof in->target[s]);
        }
    }
    atomic_store_explicit(&g_hook_f, f, memory_order_release);
    in->f = f;

    if (advance && sg->type != SG_END) seg_next();
    else if (!advance) segk++;
    if (sg->type == SG_BOOT && advance) { segk = 0; }
}

static void hook(void *cpu, uint32_t f) {
    if (in_call) return;
    step((Cpu *)cpu, f);
}

/* ------------------------------------------------------------------------------------------------ captura de escenas (hilo principal) */
static void zs_detect(void) {
    if (g_zs_known) return;
    BridgeState *b = malloc(sizeof *b);
    if (!b) return;
    if (mv_host_bridge(b) && (b->valid & BRIDGE_OK_EYE)) {
        float R[3][3];
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) R[r][c] = (float)b->eye_axes[r][c];
        float det = R[0][0] * (R[1][1] * R[2][2] - R[1][2] * R[2][1]) - R[0][1] * (R[1][0] * R[2][2] - R[1][2] * R[2][0]) + R[0][2] * (R[1][0] * R[2][1] - R[1][1] * R[2][0]);
        g_zs = det < 0 ? -1 : 1;
        g_zs_known = 1;
    }
    free(b);
}

static void on_scene(const VrScene *s) {
    if (!s->src) return;
    uint32_t hf = s->src->host_frame;
    const Intent *in = &ring[hf % RING];
    if (in->f != hf) { atomic_store(&g_cons, hf); return; }
    Intent snap = *in;
    atomic_store(&g_cons, hf);
    if (!snap.store || snap.zone < 0 || snap.zone >= MAXZ) return;
    Zone *z = &Z[snap.zone];
    z->seen_scenes++;
    if (z->n == z->cap) {
        size_t nc = z->cap ? z->cap * 2 : 2048;
        Cap *p = realloc(z->c, nc * sizeof *p);
        if (!p) return;
        z->c = p; z->cap = nc;
    }
    Cap *c = &z->c[z->n++];
    memset(c, 0, sizeof *c);
    c->in = snap; c->hf = hf;
    memcpy(c->eye_vr, s->eye_vr, sizeof c->eye_vr);
    memcpy(c->org, s->origin_game, sizeof c->org);
    c->scale = s->scale;
    c->st = s->stats; c->world_valid = s->world_valid; c->has3d = s->has_3d;
    c->fwd[0] = s->game_view[8]; c->fwd[1] = s->game_view[9]; c->fwd[2] = s->game_view[10];
    if (s->world_valid) zs_detect();
    if (snap.store >= 2 && s->has_3d && s->world_valid && s->v && s->b) {
        uint32_t tot = 0, semi = 0;
        for (uint32_t i = 0; i < s->nb; i++) { if (s->b[i].kind == 0) tot += s->b[i].count / 3; else if (s->b[i].kind == 1) semi += s->b[i].count / 3; }
        c->ntri = tot; c->nsemi = semi;
        c->tri = malloc((size_t)(tot ? tot : 1) * 9 * sizeof(float));
        if (c->tri) {
            uint32_t k = 0;
            for (uint32_t i = 0; i < s->nb; i++) {
                const VrBatch *b = &s->b[i];
                if (b->kind != 0) continue;
                for (uint32_t j = 0; j + 2 < b->count; j += 3) {
                    if ((uint64_t)b->first + j + 2 >= s->nv) break;
                    for (int q = 0; q < 3; q++) memcpy(c->tri + (size_t)k * 9 + 3 * q, s->v[b->first + j + q].pos, 3 * sizeof(float));
                    k++;
                }
            }
            c->ntri = k;
        } else c->ntri = 0;
    }
}

/* ------------------------------------------------------------------------------------------------ referencia omnidireccional */
static uint64_t tri_key(const float *P) {
    int32_t q[3][3];
    for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) q[i][k] = (int32_t)floorf(P[3 * i + k] * (1.0f / 25.0f));
    for (int a = 0; a < 2; a++) for (int b = 0; b < 2 - a; b++) {
        int sw = 0;
        for (int k = 0; k < 3; k++) { if (q[b][k] != q[b + 1][k]) { sw = q[b][k] > q[b + 1][k]; break; } }
        if (sw) for (int k = 0; k < 3; k++) { int32_t t = q[b][k]; q[b][k] = q[b + 1][k]; q[b + 1][k] = t; }
    }
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) { h ^= (uint32_t)q[i][k]; h *= 1099511628211ull; h ^= h >> 29; }
    return h | 1;
}
static void ref_init(Ref *R) { memset(R, 0, sizeof *R); R->hmask = (1u << 21) - 1; R->hs = calloc(R->hmask + 1, sizeof(uint64_t)); }
static void ref_free(Ref *R) { free(R->t); free(R->hs); memset(R, 0, sizeof *R); }
static void ref_add(Ref *R, const float *P) {
    if (R->n > 1500000) return;
    uint64_t k = tri_key(P);
    size_t i = (size_t)(k >> 7) & R->hmask;
    while (R->hs[i]) { if (R->hs[i] == k) return; i = (i + 1) & R->hmask; }
    R->hs[i] = k;
    if (R->n == R->cap) {
        size_t nc = R->cap ? R->cap * 2 : 65536;
        float *p = realloc(R->t, nc * 9 * sizeof(float));
        if (!p) return;
        R->t = p; R->cap = nc;
    }
    memcpy(R->t + R->n * 9, P, 9 * sizeof(float));
    R->n++;
}
static void to_fixed(const Cap *c, const float *v, float *P) {
    float is = 1.0f / c->scale;
    P[0] = v[0] * is + c->org[0];
    P[1] = v[1] * is + c->org[1];
    P[2] = (float)g_zs * v[2] * is + c->org[2];
}
static void from_fixed(const Cap *c, const float *P, float *v) {
    v[0] = (P[0] - c->org[0]) * c->scale;
    v[1] = (P[1] - c->org[1]) * c->scale;
    v[2] = (float)g_zs * (P[2] - c->org[2]) * c->scale;
}

/* ------------------------------------------------------------------------------------------------ evaluación de una captura */
static const float *present_head(const Cap *c) {
    const Intent *n = &ring[(c->hf + LAG_PRESENT) % RING];
    return (n->f == c->hf + LAG_PRESENT) ? n->head : c->in.head;
}
static PrCam make_cam(void) { PrCam c = {EYE_W, EYE_W, tanf(FOV_DEG * 0.5f * 0.01745329252f), tanf(FOV_DEG * 0.5f * 0.01745329252f), 0.1f}; return c; }

static void shade(const PrBuf *V, const PrBuf *R, uint8_t *rgb, int tile) {
    int step = EYE_W / tile;
    for (int y = 0; y < tile; y++) for (int x = 0; x < tile; x++) {
        int hv = 0, hr = 0; float zv = 1e30f;
        for (int dy = 0; dy < step; dy++) for (int dx = 0; dx < step; dx++) {
            int i = (y * step + dy) * EYE_W + x * step + dx;
            if (V->z[i] < 1e29f) { hv = 1; if (V->z[i] < zv) zv = V->z[i]; }
            if (R->z[i] < 1e29f) hr = 1;
        }
        uint8_t *p = rgb + ((size_t)y * tile + x) * 3;
        if (hv) { float g = 70.0f + 160.0f * expf(-zv / 6.0f); p[0] = p[1] = p[2] = (uint8_t)g; }
        else if (hr) { p[0] = 225; p[1] = 30; p[2] = 30; }
        else { p[0] = 14; p[1] = 22; p[2] = 52; }
    }
}

/* Transformación ideal de la escena a espacio del usuario (control positivo): giro en Y alrededor del ojo del motor + traslación del ojo a la cabeza. */
typedef struct { int on; float cs, sn, pv[3], t[3]; } Xf;
static void xf_make(const Cap *c, Xf *x) {
    const float *h = c->in.head;
    float hf[3] = {-h[8], -h[9], -h[10]};
    float phi = atan2f(hf[0], hf[2]) - atan2f(c->fwd[0], c->fwd[2]);
    x->on = 1; x->cs = cosf(phi); x->sn = sinf(phi);
    memcpy(x->pv, c->eye_vr, sizeof x->pv);
    x->t[0] = h[12]; x->t[1] = h[13]; x->t[2] = h[14];
}
static void xf_apply(const Xf *x, float *v) {
    if (!x || !x->on) return;
    float dx = v[0] - x->pv[0], dy = v[1] - x->pv[1], dz = v[2] - x->pv[2];
    v[0] = dx * x->cs + dz * x->sn + x->t[0];
    v[1] = dy + x->t[1];
    v[2] = -dx * x->sn + dz * x->cs + x->t[2];
}

static void eval_cap_x(const Cap *c, const Ref *R, Ev *ev, uint8_t **thumb, int tile, const Xf *xf) {
    memset(ev, 0, sizeof *ev);
    const float *head = present_head(c);
    PrCam cam = make_cam();
    PrBuf V, B;
    pr_buf_init(&V, EYE_W, EYE_W); pr_buf_init(&B, EYE_W, EYE_W);
    for (int side = 0; side < 2; side++) {
        PrEye e;
        pr_eye_from_head(head, side ? 1 : -1, IPD, &e);
        pr_buf_clear(&V); pr_buf_clear(&B);
        int drawn = 0;
        for (uint32_t i = 0; i < c->ntri; i++) {
            float t9[9];
            memcpy(t9, c->tri + (size_t)i * 9, sizeof t9);
            for (int q = 0; q < 3; q++) xf_apply(xf, t9 + 3 * q);
            if (pr_tri(&V, &cam, &e, t9)) drawn++;
        }
        if (R) for (size_t i = 0; i < R->n; i++) {
            float t9[9];
            for (int q = 0; q < 3; q++) { from_fixed(c, R->t + i * 9 + 3 * q, t9 + 3 * q); xf_apply(xf, t9 + 3 * q); }
            pr_tri(&B, &cam, &e, t9);
        }
        int nv = 0, nr = 0, nh = 0;
        for (int i = 0; i < EYE_W * EYE_W; i++) {
            int a = V.z[i] < 1e29f, b = B.z[i] < 1e29f || a;   /* la referencia incluye siempre lo que la escena dibuja */
            nv += a; nr += b; nh += (b && !a);
            if (b && !a) B.z[i] = 1e20f; else if (!b) B.z[i] = 1e30f; else B.z[i] = 1e30f;
        }
        float tot = (float)(EYE_W * EYE_W);
        if (side) { ev->hole_r = 100.0f * nh / tot; ev->view_r = 100.0f * nv / tot; ev->ref_r = 100.0f * nr / tot; }
        else { ev->hole_l = 100.0f * nh / tot; ev->view_l = 100.0f * nv / tot; ev->ref_l = 100.0f * nr / tot; ev->drawn = drawn; if (thumb) {
            *thumb = malloc((size_t)tile * tile * 3);
            if (*thumb) shade(&V, &B, *thumb, tile);
        } }
    }
    pr_buf_free(&V); pr_buf_free(&B);
    float hf[3] = {-head[8], -head[9], -head[10]};
    ev->gaze = angle_between(c->fwd, hf);
    ev->pos = sqrtf((c->eye_vr[0] - c->in.head[12]) * (c->eye_vr[0] - c->in.head[12]) + (c->eye_vr[1] - c->in.head[13]) * (c->eye_vr[1] - c->in.head[13]) +
                    (c->eye_vr[2] - c->in.head[14]) * (c->eye_vr[2] - c->in.head[14])) * 100.0f;
    ev->valid = 1;
}

static void eval_cap(const Cap *c, const Ref *R, Ev *ev, uint8_t **thumb, int tile) { eval_cap_x(c, R, ev, thumb, tile, NULL); }

/* manos: muñeca del mando (espacio del usuario) frente a muñeca del personaje en la escena. Se mide tal cual (lo que ve el usuario) y con la
 * escena corregida idealmente (giro y altura de la cámara resueltos: aísla el error de la IK / del mapeo de la mano). */
typedef struct { int ok[2], vis[2], drv[2], near[2], near_c[2]; float err[2], err_c[2]; } HandEv;
static void eval_hands(const Cap *c, HandEv *h, uint8_t *thumb) {
    memset(h, 0, sizeof *h);
    if (!c->in.hands) return;
    PrCam cam = make_cam();
    PrEye e;
    pr_eye_from_head(c->in.head, -1, IPD, &e);
    Xf xf;
    xf_make(c, &xf);
    for (int s = 0; s < 2; s++) {
        float back[3] = {0, 0, 0.055f}, rb[3], E[3], D[3], Dc[3];
        quat_rot(c->in.hq[s], back, rb);
        for (int k = 0; k < 3; k++) E[k] = c->in.hp[s][k] + rb[k];
        D[0] = c->scale * (c->in.wrist[s][0] - c->org[0]);
        D[1] = c->scale * (c->in.wrist[s][1] - c->org[1]);
        D[2] = (float)g_zs * c->scale * (c->in.wrist[s][2] - c->org[2]);
        memcpy(Dc, D, sizeof Dc);
        xf_apply(&xf, Dc);
        h->drv[s] = c->in.drv[s] && c->in.fade[s] > 0.9f;
        h->err[s] = sqrtf((D[0] - E[0]) * (D[0] - E[0]) + (D[1] - E[1]) * (D[1] - E[1]) + (D[2] - E[2]) * (D[2] - E[2])) * 100.0f;
        h->err_c[s] = sqrtf((Dc[0] - E[0]) * (Dc[0] - E[0]) + (Dc[1] - E[1]) * (Dc[1] - E[1]) + (Dc[2] - E[2]) * (Dc[2] - E[2])) * 100.0f;
        float px, py, dp;
        h->vis[s] = pr_project(&cam, &e, E, &px, &py, &dp) && px >= 4 && py >= 4 && px < EYE_W - 4 && py < EYE_W - 4;
        int near = 0, near_c = 0;
        for (uint32_t i = 0; i < c->ntri; i++) {
            const float *t = c->tri + (size_t)i * 9;
            float cen[3] = {(t[0] + t[3] + t[6]) / 3, (t[1] + t[4] + t[7]) / 3, (t[2] + t[5] + t[8]) / 3}, cc[3];
            float d2 = (cen[0] - E[0]) * (cen[0] - E[0]) + (cen[1] - E[1]) * (cen[1] - E[1]) + (cen[2] - E[2]) * (cen[2] - E[2]);
            if (d2 < 0.15f * 0.15f) near++;
            memcpy(cc, cen, sizeof cc);
            xf_apply(&xf, cc);
            d2 = (cc[0] - E[0]) * (cc[0] - E[0]) + (cc[1] - E[1]) * (cc[1] - E[1]) + (cc[2] - E[2]) * (cc[2] - E[2]);
            if (d2 < 0.15f * 0.15f) near_c++;
        }
        h->near[s] = near; h->near_c[s] = near_c;
        h->ok[s] = h->vis[s] && h->drv[s] && h->err[s] <= TH.hand_err_cm && near >= 3;
        if (thumb) {
            float qx, qy, qd;
            if (pr_project(&cam, &e, E, &qx, &qy, &qd)) { pr_line(thumb, EYE_W, EYE_W, (int)qx - 5, (int)qy, (int)qx + 5, (int)qy, 40, 255, 80); pr_line(thumb, EYE_W, EYE_W, (int)qx, (int)qy - 5, (int)qx, (int)qy + 5, 40, 255, 80); }
            if (pr_project(&cam, &e, D, &qx, &qy, &qd)) { pr_line(thumb, EYE_W, EYE_W, (int)qx - 4, (int)qy - 4, (int)qx + 4, (int)qy + 4, 255, 60, 255); pr_line(thumb, EYE_W, EYE_W, (int)qx - 4, (int)qy + 4, (int)qx + 4, (int)qy - 4, 255, 60, 255); }
        }
    }
}

/* ------------------------------------------------------------------------------------------------ resultados / informe */
typedef struct { int pass; } Res;
static void verdict(const char *what, double val, double thr, int le, const char *unit, const char *extra) {
    int ok = le ? val <= thr : val >= thr;
    if (!ok) fails++;
    REP("  [%s] %-34s %9.2f %s (umbral %s %.2f)%s%s\n", ok ? "PASA " : "FALLA", what, val, unit, le ? "<=" : ">=", thr, extra && *extra ? "  " : "", extra ? extra : "");
}
static void heat(double p, uint8_t rgb[3]) {
    double t = p <= 0 ? 0 : p >= 10 ? 1 : p / 10.0;
    if (t < 0.3) { double u = t / 0.3; rgb[0] = (uint8_t)(40 + u * 200); rgb[1] = (uint8_t)(170 + u * 50); rgb[2] = (uint8_t)(70 - u * 40); }
    else { double u = (t - 0.3) / 0.7; rgb[0] = (uint8_t)(240 - u * 20); rgb[1] = (uint8_t)(220 - u * 190); rgb[2] = (uint8_t)(30); }
}
static float hmax(const Ev *e) { return e->hole_l > e->hole_r ? e->hole_l : e->hole_r; }

static int cap_idx(const Zone *z, int kind, int view, int age_min, int need_tri) {
    for (size_t i = 0; i < z->n; i++) {
        const Cap *c = &z->c[i];
        if (c->in.kind == kind && c->in.view == view && c->in.age >= age_min && (!need_tri || c->tri) && c->world_valid) return (int)i;
    }
    return -1;
}

static void sheet_sweep(Zone *z, int zi, const int *idx) {
    int W = 44 + NYAW * TILE, H = 22 + NPITCH * TILE;
    uint8_t *img = calloc((size_t)W * H * 3, 1);
    if (!img) return;
    pr_rect(img, W, H, 0, 0, W, H, 24, 24, 28);
    for (int v = 0; v < NYAW * NPITCH; v++) {
        int r = v / NYAW, c = v % NYAW, x = 44 + c * TILE, y = 20 + r * TILE;
        if (idx[v] >= 0 && z->thumb[idx[v]]) {
            for (int j = 0; j < TILE; j++) memcpy(img + ((size_t)(y + j) * W + x) * 3, z->thumb[idx[v]] + (size_t)j * TILE * 3, (size_t)TILE * 3);
            char s[16]; snprintf(s, sizeof s, "%.1f", hmax(&z->ev[idx[v]]));
            pr_rect(img, W, H, x, y, (int)strlen(s) * 4 + 1, 7, 0, 0, 0);
            pr_text(img, W, H, x + 1, y + 1, 1, s, 255, 255, 255);
        } else pr_rect(img, W, H, x, y, TILE, TILE, 80, 0, 80);
        pr_rect(img, W, H, x, y, TILE, 1, 0, 0, 0); pr_rect(img, W, H, x, y, 1, TILE, 0, 0, 0);
    }
    for (int r = 0; r < NPITCH; r++) { char s[16]; snprintf(s, sizeof s, "%d", (int)PITCHES[r]); pr_text(img, W, H, 2, 20 + r * TILE + TILE / 2 - 2, 1, s, 230, 230, 230); }
    for (int c = 0; c < NYAW; c += 2) { char s[16]; snprintf(s, sizeof s, "%d", c * 15); pr_text(img, W, H, 44 + c * TILE + 2, 6, 1, s, 230, 230, 230); }
    char p[640]; snprintf(p, sizeof p, "%s/zona%d_barrido.png", g_out, zi);
    pr_write_png(p, W, H, img);
    free(img);
}

static void sheet_turn(Zone *z, int zi, const int *idxs[3], const int ns[3]) {
    int cols = 24, W = 44 + cols * TILE, H = 3 * (TILE * 2 + 16) + 4;
    uint8_t *img = calloc((size_t)W * H * 3, 1);
    if (!img) return;
    pr_rect(img, W, H, 0, 0, W, H, 24, 24, 28);
    static const char *NM[3] = {"GIRO C", "STICK S", "STICK P"};
    for (int t = 0; t < 3; t++) {
        int y0 = 4 + t * (TILE * 2 + 16);
        pr_text(img, W, H, 2, y0 + 20, 1, NM[t], 255, 220, 120);
        if (ns[t] <= 0) continue;
        float mx = 10;
        for (int i = 0; i < ns[t]; i++) { float h = hmax(&z->ev[idxs[t][i]]); if (h > mx) mx = h; }
        if (mx > 100) mx = 100;
        for (int k = 0; k < cols; k++) {
            int i = (int)((long)k * (ns[t] - 1) / (cols - 1));
            const int ci = idxs[t][i];
            int x = 44 + k * TILE;
            if (z->thumb[ci]) for (int j = 0; j < TILE; j++) memcpy(img + ((size_t)(y0 + j) * W + x) * 3, z->thumb[ci] + (size_t)j * TILE * 3, (size_t)TILE * 3);
            char s[16]; snprintf(s, sizeof s, "%.1f", hmax(&z->ev[ci]));
            pr_rect(img, W, H, x, y0, (int)strlen(s) * 4 + 1, 7, 0, 0, 0);
            pr_text(img, W, H, x + 1, y0 + 1, 1, s, 255, 255, 255);
        }
        int py = y0 + TILE + 6, ph = TILE;
        pr_rect(img, W, H, 44, py, cols * TILE, ph, 10, 10, 14);
        int px0 = -1, py0 = 0;
        for (int i = 0; i < ns[t]; i++) {
            float h = hmax(&z->ev[idxs[t][i]]);
            int x = 44 + (int)((long)i * (cols * TILE - 1) / (ns[t] > 1 ? ns[t] - 1 : 1));
            int y = py + ph - 1 - (int)(h / mx * (ph - 1));
            if (px0 >= 0) pr_line(img, W, H, px0, py0, x, y, 240, 90, 60);
            px0 = x; py0 = y;
        }
        char s[24]; snprintf(s, sizeof s, "%.0f", mx);
        pr_text(img, W, H, 44, py, 1, s, 200, 200, 200);
    }
    char p[640]; snprintf(p, sizeof p, "%s/zona%d_giro.png", g_out, zi);
    pr_write_png(p, W, H, img);
    free(img);
}

static void sheet_hands(Zone *z, int zi, const int idx[2][5], const HandEv he[2][5]) {
    int W = 5 * EYE_W + 10, H = 2 * (EYE_W + 12) + 4;
    uint8_t *img = calloc((size_t)W * H * 3, 1);
    if (!img) return;
    pr_rect(img, W, H, 0, 0, W, H, 24, 24, 28);
    for (int p = 0; p < 2; p++) for (int v = 0; v < 5; v++) {
        int x = 5 + v * EYE_W, y = 2 + p * (EYE_W + 12);
        if (idx[p][v] >= 0 && z->thumb[idx[p][v]]) for (int j = 0; j < EYE_W; j++) memcpy(img + ((size_t)(y + 10 + j) * W + x) * 3, z->thumb[idx[p][v]] + (size_t)j * EYE_W * 3, (size_t)EYE_W * 3);
        char s[48]; snprintf(s, sizeof s, "%s %s E%.0f/%.0f N%d/%d", p ? "SENT" : "PIE", (const char *[]){"FRENTE", "ABAJO", "ABAJO2", "IZQ", "DER"}[v], he[p][v].err[0], he[p][v].err[1], he[p][v].near[0], he[p][v].near[1]);
        pr_text(img, W, H, x, y, 1, s, 255, 255, 255);
    }
    char pth[640]; snprintf(pth, sizeof pth, "%s/zona%d_manos.png", g_out, zi);
    pr_write_png(pth, W, H, img);
    free(img);
}

typedef struct { int have; double sw_mean, sw_p95, sw_max, hs_mean, ts_mean, tp_mean, ctrl_mean, gaze, pos_stand, pos_sit, hands_ok, vblank; int nvis; } ZRes;
static ZRes ZR[MAXZ];
static FILE *g_csv;
static float g_heat[MAXZ][NYAW * NPITCH];
static int g_heat_ok[MAXZ];

static void eval_zone(int zi) {
    Zone *z = &Z[zi];
    REP("\n=== zona %d (código '%s') ===\n", zi, z->name);
    if (zi > 0 && !z->reached) { REP("  [AVISO] zona no alcanzada: no se mide (la receta de cambio de zona no llegó al control)\n"); return; }
    if (!z->n) { REP("  [FALLA] sin capturas\n"); fails++; return; }
    double t0 = now_s();
    z->ev = calloc(z->n, sizeof *z->ev);
    z->thumb = calloc(z->n, sizeof *z->thumb);
    /* referencia: unión de los triángulos de las vistas asentadas del barrido, en coordenadas fijas */
    Ref R;
    ref_init(&R);
    for (size_t i = 0; i < z->n; i++) {
        const Cap *c = &z->c[i];
        if (c->in.kind != CK_SWEEP || !c->tri || !c->world_valid) continue;
        for (uint32_t k = 0; k < c->ntri; k++) {
            float P[9];
            for (int q = 0; q < 3; q++) to_fixed(c, c->tri + (size_t)k * 9 + 3 * q, P + 3 * q);
            ref_add(&R, P);
        }
    }
    REP("  referencia omnidireccional: %zu triángulos únicos (%zu escenas guardadas)\n", R.n, z->n);

    /* --- barrido --- */
    int idx[NYAW * NPITCH];
    int nsw = 0;
    float hs[NYAW * NPITCH];
    int worst = -1;
    float wmax = -1;
    for (int v = 0; v < NYAW * NPITCH; v++) {
        idx[v] = cap_idx(z, CK_SWEEP, v, SETTLE, 1);
        if (idx[v] < 0) continue;
        eval_cap(&z->c[idx[v]], &R, &z->ev[idx[v]], &z->thumb[idx[v]], TILE);
        hs[nsw] = hmax(&z->ev[idx[v]]);
        g_heat[zi][v] = hs[nsw];
        if (hs[nsw] > wmax) { wmax = hs[nsw]; worst = v; }
        nsw++;
    }
    g_heat_ok[zi] = nsw > 0;
    if (nsw < NYAW * NPITCH / 2) { REP("  [FALLA] barrido: solo %d de %d vistas con escena 3D válida\n", nsw, NYAW * NPITCH); fails++; }
    if (nsw) {
        float tmp[NYAW * NPITCH]; memcpy(tmp, hs, sizeof hs);
        Stat s = stat_of(tmp, nsw);
        char ex[96]; snprintf(ex, sizeof ex, "peor vista: guiñada %d cabeceo %d, p95 %.2f%%", (worst % NYAW) * 15, (int)PITCHES[worst / NYAW], s.p95);
        REP("  -- huecos (%% del campo de visión sin escena donde la referencia tiene geometría), %d vistas --\n", nsw);
        verdict("barrido: hueco máximo", s.max, TH.hole_max, 1, "%", ex);
        verdict("barrido: hueco p95", s.p95, TH.hole_p95, 1, "%", "");
        verdict("barrido: hueco medio", s.mean, TH.hole_mean, 1, "%", "");
        ZR[zi].have = 1; ZR[zi].sw_mean = s.mean; ZR[zi].sw_p95 = s.p95; ZR[zi].sw_max = s.max;
        for (int r = 0; r < NPITCH; r++) {
            double sum = 0; int n = 0;
            for (int c = 0; c < NYAW; c++) if (idx[r * NYAW + c] >= 0) { sum += hmax(&z->ev[idx[r * NYAW + c]]); n++; }
            if (n) REP("      cabeceo %+3d: medio %.2f %%\n", (int)PITCHES[r], sum / n);
        }
        /* fondo absoluto */
        double fv = 0, fr = 0; int nn = 0;
        for (int v = 0; v < NYAW * NPITCH; v++) if (idx[v] >= 0) { const Ev *e = &z->ev[idx[v]]; fv += 100 - 0.5 * (e->view_l + e->view_r); fr += 100 - 0.5 * (e->ref_l + e->ref_r); nn++; }
        if (nn) REP("      fondo/cielo medio: %.1f %% de la vista en la escena, %.1f %% en la referencia\n", fv / nn, fr / nn);
    }
    sheet_sweep(z, zi, idx);

    /* --- giros --- */
    int *ti[3] = {NULL, NULL, NULL}; int tn[3] = {0, 0, 0};
    static const int TK[3] = {CK_HSMOOTH, CK_TSMOOTH, CK_TSTEPS};
    static const char *TN[3] = {"giro de cabeza continuo (125°/s)", "giro con stick suave (120°/s, 360°)", "giro con stick por pasos (12 x 30°)"};
    static const char *TN2[3] = {"giro_c", "giro_s", "giro_p"};
    for (int t = 0; t < 3; t++) {
        ti[t] = malloc(z->n * sizeof(int));
        if ((t == 0 && !G_CONT) || (t > 0 && !G_STICK)) continue;
        float *hv = malloc(z->n * sizeof(float)), *hset = malloc(z->n * sizeof(float));
        int nh = 0, nset = 0;
        for (size_t i = 0; i < z->n; i++) {
            Cap *c = &z->c[i];
            if (c->in.kind != TK[t] || !c->tri || !c->world_valid) continue;
            eval_cap(c, &R, &z->ev[i], &z->thumb[i], TILE);
            ti[t][tn[t]++] = (int)i;
            hv[nh++] = hmax(&z->ev[i]);
            if (c->in.age >= SETTLE) hset[nset++] = hmax(&z->ev[i]);
        }
        if (!nh) { REP("  [FALLA] %s: sin escenas 3D válidas\n", TN[t]); fails++; }
        else {
            Stat s = stat_of(hv, nh);
            char ex[96]; snprintf(ex, sizeof ex, "p95 %.2f%%, %d escenas", s.p95, nh);
            REP("  -- %s --\n", TN[t]);
            verdict(t == 2 ? "hueco máximo (todos los fotogramas)" : "hueco máximo", s.max, TH.hole_max, 1, "%", ex);
            verdict("hueco medio", s.mean, TH.hole_mean, 1, "%", "");
            if (t == 0) ZR[zi].hs_mean = s.mean; else if (t == 1) ZR[zi].ts_mean = s.mean; else ZR[zi].tp_mean = s.mean;
            if (t == 0 && nh > 4) {   /* desajuste angular dinámico: la mirada de la escena respecto a la cabeza varía con la velocidad (retardo de cámara) */
                float *g = malloc((size_t)nh * sizeof(float)), *dv = malloc((size_t)nh * sizeof(float));
                int ng = 0;
                for (size_t i = 0; i < z->n; i++) if (z->c[i].in.kind == TK[t] && z->c[i].tri && z->c[i].world_valid) g[ng++] = z->ev[i].gaze;
                float tmp2[2048]; int nn = ng < 2048 ? ng : 2048; memcpy(tmp2, g, (size_t)nn * sizeof(float));
                float med = stat_of(tmp2, nn).p50;
                for (int i = 0; i < ng; i++) dv[i] = fabsf(g[i] - med);
                Stat sd = stat_of(dv, ng);
                verdict("desajuste angular de la escena (p95)", sd.p95, TH.lag_deg, 1, "grados", "");
                free(g); free(dv);
            }
            if (t == 2 && nset) { Stat s2 = stat_of(hset, nset); verdict("hueco máximo (tras asentar el paso)", s2.max, TH.hole_max, 1, "%", ""); }
        }
        (void)TN2;
        free(hv); free(hset);
    }
    if (G_CONT || G_STICK) sheet_turn(z, zi, (const int *[3]){ti[0], ti[1], ti[2]}, tn);

    /* --- control positivo: la misma escena con la corrección ideal (giro en Y alrededor del ojo del motor y ojo llevado a la cabeza) --- */
    if (G_CONTROL) {
        float cv[NYAW * NPITCH + 64]; int n = 0;
        for (int v = 0; v < NYAW * NPITCH; v++) {
            if (idx[v] < 0) continue;
            Xf x; xf_make(&z->c[idx[v]], &x);
            Ev e; eval_cap_x(&z->c[idx[v]], &R, &e, NULL, TILE, &x);
            cv[n++] = hmax(&e);
        }
        double mean = 0; for (int i = 0; i < n; i++) mean += cv[i]; if (n) mean /= n;
        Stat s = stat_of(cv, n);
        if (n) {
            char ex[96]; snprintf(ex, sizeof ex, "p95 %.2f%%, máx %.2f%%; si falla, la medida no es fiable", s.p95, s.max);
            REP("  -- control positivo (barrido con la escena corregida idealmente: valida la medida) --\n");
            verdict("control: hueco medio", mean, TH.hole_mean, 1, "%", ex);
            ZR[zi].ctrl_mean = mean;
        }
        if (G_STICK && tn[2] > 0) {
            float st[256]; int m = 0;
            for (int i = 0; i < tn[2] && m < 256; i++) {
                const Cap *c = &z->c[ti[2][i]];
                if (c->in.age < SETTLE) continue;
                Xf x; xf_make(c, &x);
                Ev e; eval_cap_x(c, &R, &e, NULL, TILE, &x);
                st[m++] = hmax(&e);
            }
            Stat s2 = stat_of(st, m);
            if (m) verdict("control: giro por pasos, hueco medio", s2.mean, TH.hole_mean, 1, "%", "");
        }
    }

    /* --- registro cámara <-> cabeza --- */
    {
        float *pe = malloc(z->n * sizeof(float)), *ge = malloc(z->n * sizeof(float));
        for (int posture = 0; posture < 2; posture++) {
            int n = 0;
            for (size_t i = 0; i < z->n; i++) {
                const Cap *c = &z->c[i];
                if (!c->world_valid || !c->has3d || c->in.age < SETTLE) continue;
                if (posture == 0 ? (c->in.kind == CK_SWEEP) : 0) {} else if (!(c->in.kind == CK_HANDS && c->in.posture == posture)) { if (!(posture == 0 && c->in.kind == CK_HANDS && c->in.posture == 0)) continue; }
                Ev e; memset(&e, 0, sizeof e);
                const float *hf = c->in.head;
                float fw[3] = {-hf[8], -hf[9], -hf[10]};
                ge[n] = angle_between(c->fwd, fw);
                pe[n] = sqrtf((c->eye_vr[0] - hf[12]) * (c->eye_vr[0] - hf[12]) + (c->eye_vr[1] - hf[13]) * (c->eye_vr[1] - hf[13]) + (c->eye_vr[2] - hf[14]) * (c->eye_vr[2] - hf[14])) * 100.0f;
                n++;
            }
            if (!n && posture && !G_MANOS) continue;
            if (!n) { REP("  [FALLA] registro %s: sin escenas\n", posture ? "sentado" : "de pie"); fails++; continue; }
            Stat sp = stat_of(pe, n), sg = stat_of(ge, n);
            char ex[96]; snprintf(ex, sizeof ex, "máx %.1f cm, %d escenas", sp.max, n);
            REP("  -- registro cámara del motor <-> cabeza (%s, cabeza a %.2f m) --\n", posture ? "sentado" : "de pie", posture ? H_SIT : H_STAND);
            verdict("posición: error mediano", sp.p50, TH.reg_pos_cm, 1, "cm", ex);
            snprintf(ex, sizeof ex, "máx %.1f°", sg.max);
            verdict("mirada: error mediano", sg.p50, TH.reg_gaze_deg, 1, "grados", ex);
            if (posture) ZR[zi].pos_sit = sp.p50; else { ZR[zi].pos_stand = sp.p50; ZR[zi].gaze = sg.p50; }
        }
        free(pe); free(ge);
    }
    /* giro con stick: el error de mirada tras asentar cada paso no debe crecer con el giro acumulado */
    if (G_STICK) {
        float mx = 0; int n = 0;
        for (int i = 0; i < tn[2]; i++) { const Cap *c = &z->c[ti[2][i]]; if (c->in.age < SETTLE) continue; const float *hf = c->in.head; float fw[3] = {-hf[8], -hf[9], -hf[10]}; float g = angle_between(c->fwd, fw); if (g > mx) mx = g; n++; }
        if (n) verdict("giro stick: mirada máx. tras asentar", mx, TH.reg_gaze_deg, 1, "grados", "");
    }
    /* retardo de cámara tras mover la cabeza (fotogramas hasta que la mirada de la escena converge) */
    {
        float cv[NYAW * NPITCH]; int n = 0;
        for (int v = 0; v < NYAW * NPITCH; v += 3) {
            int fin = -1;
            for (size_t i = 0; i < z->n; i++) { const Cap *c = &z->c[i]; if (c->in.kind == CK_SWEEP && c->in.view == v && c->in.age == SETTLE + 1 && c->world_valid) { fin = (int)i; break; } }
            if (fin < 0) continue;
            int conv = -1;
            for (int a = 0; a <= SETTLE + 1 && conv < 0; a++)
                for (size_t i = 0; i < z->n; i++) { const Cap *c = &z->c[i]; if (c->in.kind == CK_SWEEP && c->in.view == v && c->in.age == a && c->world_valid && angle_between(c->fwd, z->c[fin].fwd) < 1.0f) { conv = a; break; } }
            cv[n++] = conv < 0 ? (float)(SETTLE + 2) : (float)conv;
        }
        if (n) { Stat s = stat_of(cv, n); char ex[64]; snprintf(ex, sizeof ex, "medio %.1f", s.mean); verdict("retardo de la cámara (p95)", s.p95, TH.conv_frames, 1, "VBlanks", ex); }
    }
    /* --- descartes --- */
    {
        VrStats sum; memset(&sum, 0, sizeof sum);
        double emit = 0, kept = 0, drawn = 0, n = 0, tris = 0, semi = 0;
        for (int v = 0; v < NYAW * NPITCH; v++) {
            if (idx[v] < 0) continue;
            const Cap *c = &z->c[idx[v]];
            const VrStats *s = &c->st;
            sum.drop_partial += s->drop_partial; sum.drop_near += s->drop_near; sum.drop_degenerate += s->drop_degenerate; sum.drop_huge += s->drop_huge; sum.drop_nan += s->drop_nan;
            sum.drop_behind += s->drop_behind; sum.drop_ambig += s->drop_ambig; sum.drop_derived += s->drop_derived; sum.n_poly3d += s->n_poly3d; sum.n_tris3d += s->n_tris3d; sum.n_clipped += s->n_clipped;
            kept += s->n_poly3d;
            drawn += z->ev[idx[v]].drawn; tris += c->ntri; semi += c->nsemi; n++;
        }
        double dropped = (double)sum.drop_partial + sum.drop_near + sum.drop_degenerate + sum.drop_huge + sum.drop_nan + sum.drop_behind + sum.drop_ambig + sum.drop_derived;
        emit = kept + dropped;
        if (n && emit > 0) {
            REP("  -- polígonos 3D: emitidos por el juego frente a los que quedan en la escena (media por vista del barrido) --\n");
            REP("      emitidos %.0f · en escena %.0f polígonos (%.0f triángulos opacos + %.0f semitransparentes) · dibujados en la vista (ojo izq.) %.0f triángulos\n", emit / n, kept / n, tris / n, semi / n, drawn / n);
            REP("      descartes de vr_scene por motivo (media por vista): parciales %.1f · cercanos %.1f · detrás %.1f · ambiguos %.1f · derivados %.1f · degenerados %.1f · enormes %.1f · NaN %.1f · recortados %.1f\n",
                (double)sum.drop_partial / n, (double)sum.drop_near / n, (double)sum.drop_behind / n, (double)sum.drop_ambig / n, (double)sum.drop_derived / n, (double)sum.drop_degenerate / n,
                (double)sum.drop_huge / n, (double)sum.drop_nan / n, (double)sum.n_clipped / n);
            verdict("descartes / emitidos", dropped / emit, TH.disc_share, 1, "", "");
        }
    }
    /* --- manos --- */
    if (G_MANOS) {
        int hidx[2][5]; HandEv he[2][5]; int nvis = 0, nok = 0;
        float emax = 0, emax_c = 0; int lowc = 0;
        REP("  -- manos y brazos (mandos sintéticos; error = muñeca del mando vs muñeca del personaje dibujada en la escena) --\n");
        for (int p = 0; p < 2; p++) for (int v = 0; v < 5; v++) {
            hidx[p][v] = -1;
            memset(&he[p][v], 0, sizeof he[p][v]);
            for (size_t i = 0; i < z->n; i++) { const Cap *c = &z->c[i]; if (c->in.kind == CK_HANDS && c->in.posture == p && c->in.view == v && c->in.age >= 28 && c->tri && c->world_valid) { hidx[p][v] = (int)i; break; } }
            if (hidx[p][v] < 0) continue;
            Cap *c = &z->c[hidx[p][v]];
            Ev *e = &z->ev[hidx[p][v]];
            eval_cap(c, &R, e, &z->thumb[hidx[p][v]], EYE_W);
            eval_hands(c, &he[p][v], z->thumb[hidx[p][v]]);
            static const char *VN[5] = {"delante", "abajo", "abajo-delante", "izquierda", "derecha"};
            HandEv *h = &he[p][v];
            REP("      %-7s %-13s izq.: %s err %5.1f cm (corregida %4.1f), %3d tri junto al mando (%3d), IK %s | der.: %s err %5.1f cm (corregida %4.1f), %3d tri (%3d), IK %s\n", p ? "sentado" : "de pie", VN[v],
                h->vis[0] ? "visible" : "fuera  ", h->err[0], h->err_c[0], h->near[0], h->near_c[0], h->drv[0] ? "sí" : "NO",
                h->vis[1] ? "visible" : "fuera  ", h->err[1], h->err_c[1], h->near[1], h->near_c[1], h->drv[1] ? "sí" : "NO");
            for (int s = 0; s < 2; s++) if (h->vis[s]) { nvis++; nok += h->ok[s]; if (h->err[s] > emax) emax = h->err[s]; if (h->err_c[s] > emax_c) emax_c = h->err_c[s]; if (h->near_c[s] < 3) lowc++; }
        }
        if (nvis == 0) { REP("  [FALLA] ninguna mano visible en las vistas de prueba\n"); fails++; }
        else {
            char ex[160]; snprintf(ex, sizeof ex, "%d de %d visibles correctas; error máx %.1f cm (con la cámara corregida: %.1f cm; %d sin malla cerca)", nok, nvis, emax, emax_c, lowc);
            verdict("manos correctas (fracción de las visibles)", (double)nok / nvis, TH.hand_ok_frac, 0, "", ex);
            ZR[zi].hands_ok = (double)nok / nvis; ZR[zi].nvis = nvis;
        }
        sheet_hands(z, zi, hidx, he);
    }
    /* --- rendimiento --- */
    if (z->perf_ok) {
        REP("  -- rendimiento (hilo de emulación en avance libre con un consumidor de escenas) --\n");
        verdict("VBlank/s del juego", z->vblank_s, TH.vblank_min, 0, "VBlank/s", "");
        ZR[zi].vblank = z->vblank_s;
        verdict("vr_scene: ms medio por escena", z->build_avg_ms, TH.build_avg_ms, 1, "ms", "");
        verdict("vr_scene: ms máximo (acumulado)", z->build_max_ms, TH.build_max_ms, 1, "ms", "");
    }
    /* CSV por captura evaluada */
    if (g_csv) for (size_t i = 0; i < z->n; i++) {
        const Cap *c = &z->c[i];
        if (!z->ev[i].valid) continue;
        fprintf(g_csv, "%d,%s,%d,%d,%.1f,%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f,%.1f,%u\n", zi, CK_NAME[c->in.kind], c->in.view, c->in.age, c->in.yaw, c->in.pitch, z->ev[i].hole_l, z->ev[i].hole_r, z->ev[i].view_l, z->ev[i].ref_l,
                z->ev[i].gaze, z->ev[i].pos, c->in.turn_total, 0.0, c->ntri);
    }
    for (int t = 0; t < 3; t++) free(ti[t]);
    ref_free(&R);
    for (size_t i = 0; i < z->n; i++) { free(z->c[i].tri); z->c[i].tri = NULL; free(z->thumb[i]); }
    free(z->thumb); z->thumb = NULL;
    REP("  (evaluación: %.1f s)\n", now_s() - t0);
}

/* ------------------------------------------------------------------------------------------------ principal */
static void summary_sheet(void) {
    int W = 44 + NYAW * 14, H = 14;
    int rows = 0;
    for (int z = 0; z < nzones; z++) if (g_heat_ok[z]) rows++;
    if (!rows) return;
    H = rows * (NPITCH * 14 + 12) + 4;
    uint8_t *img = calloc((size_t)W * H * 3, 1);
    if (!img) return;
    pr_rect(img, W, H, 0, 0, W, H, 24, 24, 28);
    int ry = 0;
    for (int z = 0; z < nzones; z++) {
        if (!g_heat_ok[z]) continue;
        char s[48]; snprintf(s, sizeof s, "ZONA %d %s", z, Z[z].name);
        pr_text(img, W, H, 2, ry + 2, 1, s, 255, 220, 120);
        for (int v = 0; v < NYAW * NPITCH; v++) {
            uint8_t col[3];
            heat(g_heat[z][v], col);
            pr_rect(img, W, H, 44 + (v % NYAW) * 14, ry + 10 + (v / NYAW) * 14, 13, 13, col[0], col[1], col[2]);
        }
        for (int r = 0; r < NPITCH; r++) { char t[16]; snprintf(t, sizeof t, "%d", (int)PITCHES[r]); pr_text(img, W, H, 2, ry + 10 + r * 14 + 4, 1, t, 220, 220, 220); }
        ry += NPITCH * 14 + 12;
    }
    char p[640]; snprintf(p, sizeof p, "%s/resumen_huecos.png", g_out);
    pr_write_png(p, W, H, img);
    free(img);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: MV_SIGS=firmas.json %s <exe> <disco> [--out DIR] [--zonas a,b,c | --zonas-fichero F] [--solo barrido,giro_c,stick,manos,rend] [--th nombre=valor]...\n", argv[0]); return 2; }
    const char *exe = argv[1], *disc = argv[2];
    const char *zfile = NULL;
    char zlist[512] = "";
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) snprintf(g_out, sizeof g_out, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--zonas") && i + 1 < argc) snprintf(zlist, sizeof zlist, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--zonas-fichero") && i + 1 < argc) zfile = argv[++i];
        else if (!strcmp(argv[i], "--solo") && i + 1 < argc) {
            char *d = strdup(argv[++i]), *sv = NULL;
            G_CONT = G_STICK = G_MANOS = G_REND = 0;
            for (char *t = strtok_r(d, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) {
                if (!strcmp(t, "giro_c")) G_CONT = 1; else if (!strcmp(t, "stick")) G_STICK = 1; else if (!strcmp(t, "manos")) G_MANOS = 1; else if (!strcmp(t, "rend")) G_REND = 1;
            }
            free(d);
        } else if (!strcmp(argv[i], "--sin-control")) { G_CONTROL = 0;
        } else if (!strcmp(argv[i], "--th") && i + 1 < argc) {
            char *kv = argv[++i], *eq = strchr(kv, '=');
            int ok = 0;
            if (eq) { *eq = 0; for (unsigned k = 0; k < sizeof THN / sizeof THN[0]; k++) if (!strcmp(THN[k].n, kv)) { *THN[k].p = atof(eq + 1); ok = 1; } }
            if (!ok) { fprintf(stderr, "umbral desconocido: %s\n", kv); return 2; }
        }
    }
    mkdir(g_out, 0755);
    char p[640];
    snprintf(p, sizeof p, "%s/informe.txt", g_out); g_rep = fopen(p, "w");
    snprintf(p, sizeof p, "%s/vistas.csv", g_out); g_csv = fopen(p, "w");
    if (g_csv) fprintf(g_csv, "zona,prueba,vista,edad,guinada,cabeceo,hueco_izq,hueco_der,cobertura_izq,referencia_izq,error_mirada_grados,error_pos_cm,giro_acumulado,0,triangulos\n");

    /* zonas: la inicial (la que da la partida) y las pedidas */
    g_zname[0] = "(inicial)";
    nzones = 1;
    if (!zfile && !*zlist) { char b[600]; snprintf(b, sizeof b, "%.*s", (int)(strrchr(exe, '/') ? (size_t)(strrchr(exe, '/') - exe) : 0), exe); snprintf(p, sizeof p, "%s/probador_zonas.txt", strrchr(exe, '/') ? b : "."); if (!access(p, R_OK)) zfile = strdup(p); }
    if (zfile) {
        FILE *f = fopen(zfile, "r");
        char l[128];
        while (f && fgets(l, sizeof l, f) && nzones < MAXZ) {
            char *s = l + strspn(l, " \t"); s[strcspn(s, " \t\r\n#")] = 0;
            if (*s) g_zname[nzones++] = strdup(s);
        }
        if (f) fclose(f);
    }
    if (*zlist) {
        char *sv = NULL;
        for (char *t = strtok_r(zlist, ",", &sv); t && nzones < MAXZ; t = strtok_r(NULL, ",", &sv)) g_zname[nzones++] = strdup(t);
    }
    for (int i = 0; i < MAXZ; i++) zone_ok_flag[i] = 1;

    /* programa */
    nprog = 0;
    PROG[nprog++] = (Seg){SG_BOOT, 0, 0};
    for (int z = 0; z < nzones; z++) {
        PROG[nprog++] = (Seg){SG_ZONE, z, 0};
        PROG[nprog++] = (Seg){SG_SWEEP, z, 0};
        if (G_CONT) PROG[nprog++] = (Seg){SG_HSMOOTH, z, 0};
        if (G_STICK) { PROG[nprog++] = (Seg){SG_TSMOOTH, z, 0}; PROG[nprog++] = (Seg){SG_TSTEPS, z, 0}; }
        if (G_MANOS) { PROG[nprog++] = (Seg){SG_HANDS, z, 0}; PROG[nprog++] = (Seg){SG_HANDS, z, 1}; }
        if (G_REND) PROG[nprog++] = (Seg){SG_PERF, z, 0};
        PROG[nprog++] = (Seg){SG_FLUSH, z, 0};
    }
    PROG[nprog++] = (Seg){SG_END, 0, 0};

    if (getenv("MV_SIGS")) mv_host_set_sigs(getenv("MV_SIGS"));
    char card[640]; snprintf(card, sizeof card, "%s/card", g_out); mkdir(card, 0755);
    mv_vrhost_install();
    mv_vrhost_set_test_hook(hook);
    mv_host_set_turbo(1);
    float m0[16]; head_matrix(0, 0, 0, H_STAND, 0, m0); mv_vr_set_head(m0, 0.0);
    int r = mv_host_start(exe, disc, card);
    if (r) { fprintf(stderr, "mv_host_start = %d: %s\n", r, mv_host_error()); return 2; }

    double t_start = now_s();
    for (;;) {
        int got = 0;
        for (int k = 0; k < 8; k++) { VrScene sc; if (!mv_host_scene(&sc)) break; got++; on_scene(&sc); }
        if (mv_host_state() == MV_HOST_ERROR) { fprintf(stderr, "error del motor: %s\n", mv_host_error()); fails++; break; }
        int fr = atomic_load(&g_flush_req);
        if (fr >= 0 && !got) {
            usleep(60000);
            for (int k = 0; k < 64; k++) { VrScene sc; if (!mv_host_scene(&sc)) break; on_scene(&sc); }
            eval_zone(fr);
            fflush(stdout);
            if (g_rep) fflush(g_rep);
            atomic_store(&g_flush_req, -1);
            atomic_store(&g_flush_done, fr);
        }
        if (atomic_load(&g_done)) break;
        if (!got) usleep(150);
    }
    mv_host_pause();
    summary_sheet();
    REP("\n=== RESUMEN ===\n");
    REP("huecos medios (%% del campo de visión) por zona y prueba; registro = error mediano de mirada (grados) y de altura/posición (cm, de pie / sentado)\n");
    REP("  zona  código   barrido(medio/p95/máx)    giro_c   stick_s  stick_p  control   mirada  pos pie/sent   manos ok  VBlank/s\n");
    for (int zi = 0; zi < nzones; zi++) {
        if (!ZR[zi].have) { REP("  %-5d %-6s   (sin medir)\n", zi, Z[zi].name); continue; }
        REP("  %-5d %-6s   %5.1f / %5.1f / %5.1f     %6.1f   %6.1f   %6.1f   %6.2f   %6.1f   %5.1f/%5.1f     %4.0f%%     %5.1f\n", zi, Z[zi].name, ZR[zi].sw_mean, ZR[zi].sw_p95, ZR[zi].sw_max, ZR[zi].hs_mean, ZR[zi].ts_mean,
            ZR[zi].tp_mean, ZR[zi].ctrl_mean, ZR[zi].gaze, ZR[zi].pos_stand, ZR[zi].pos_sit, 100 * ZR[zi].hands_ok, ZR[zi].vblank);
    }
    REP("arranque título -> partida: %s (control del jugador y cámara VR estables en el VBlank %u)\n", boot_ok ? "PASA" : "FALLA", boot_frame);
    if (!boot_ok) fails++;
    REP("duración total: %.0f s · umbrales sin cumplir: %d\n", now_s() - t_start, fails);
    REP("veredicto: %s\n", fails ? "FALLA" : "PASA");
    if (g_rep) fclose(g_rep);
    if (g_csv) fclose(g_csv);
    return fails ? 1 : 0;
}
