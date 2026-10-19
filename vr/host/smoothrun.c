/* smoothrun: mide la fluidez de la locomoción sobre el juego real (host completo en MV_NOGEN, sin Metal). Hace de hilo de render con un reloj VIRTUAL (100 Hz exactos
 * sobre el tiempo de juego: 1 VBlank = 20 ms), para que la medida no dependa de la velocidad de la emulación: en cada VBlank toma las escenas que el host ha ensamblado
 * (mv_host_scene), pide la corrección por fotograma de vr_smooth (mv_vrhost_smooth) dos veces (ticks de 10 ms) y sigue un punto FIJO del mundo del juego W: su posición en el
 * espacio del usuario con la última escena, sin corrección (lo que dibujaba el visor antes) y con ella.
 *   smoothrun <exe> <disco> [--frames N] [--walk T:dur] [--turn T:grados:dur] [--hyaw-at T:grados] [--log N] [--nosmooth]
 * T y dur en VBlanks relativos a T0 (30 VBlanks seguidos con control del jugador). --walk pulsa la cruceta arriba (andar hacia delante). --turn gira el stick (+ = izquierda)
 * repartido en `dur` ticks de render. Salida: escenas nuevas por segundo de juego, histograma de huecos entre escenas (VBlanks), y por ventana (andar / girar): paso por fotograma
 * de render de W (media, desviación, coeficiente de variación y máximo/media), sin y con corrección, y el giro pendiente máximo compensado. MV_SIGS = firmas. Sin datos del juego. */
#include <math.h>
#include <pthread.h>
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
#include "vr_heading.h"
#include "vrgame.h"
#include "vrhost.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FALLO %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

static uint32_t nframes = 3000, t0_vbl, log_every;
static int have_t0, inctl_run, smooth_on = 1;
typedef struct { uint32_t t, dur; } WalkEv;
static WalkEv walks[16]; static int nwalks;
static int in_walk_at(uint32_t rel) { for (int i = 0; i < nwalks; i++) if (rel >= walks[i].t + 10 && rel < walks[i].t + walks[i].dur) return 1; return 0; }
typedef struct { uint32_t t, dur; float deg; } TurnEv;
static TurnEv turns[16]; static int nturns;
typedef struct { uint32_t t; float deg; } HyEv;
static HyEv hyev[16]; static int nhy;
static float hyaw_deg;
static float head_y = 1.65f;
static FILE *trace;   /* --trace f: una linea por escena de mundo (S) y por VBlank (P) para repetir la medida sin emular (smooth_replay.c) */

static double vclock;
static double clock_fn(void) { return vclock; }

static void set_head(void) {
    float a = hyaw_deg * 3.14159265f / 180.0f, c = cosf(a), sn = sinf(a);
    float m[16] = {c, 0, -sn, 0, 0, 1, 0, 0, sn, 0, c, 0, 0, head_y, 0, 1};
    mv_vr_set_head(m, 0.0);
}

/* ---- escena vigente (la última entregada) ---- */
typedef struct {
    int valid;
    double game_s, t_wall;
    float O[3], R[9], K, c[3];
    double turn;
} Cur;
static Cur cur;
static double cur_speed;   /* velocidad del origen entre las dos ultimas escenas (unidades del juego por segundo de juego) */
static uint32_t n_scene, n_valid;
static uint32_t gap_hist[16];
static uint32_t last_vbl_seen = 0xFFFFFFFFu;
static double W[3];
static int have_W;

/* ---- ventanas de medida ---- */
typedef struct { double n, s1, s2, mx; } Acc;
static void acc(Acc *a, double v) { a->n++; a->s1 += v; a->s2 += v * v; if (v > a->mx) a->mx = v; }
static double mean(const Acc *a) { return a->n ? a->s1 / a->n : 0; }
static double sdev(const Acc *a) { double m = mean(a); return a->n > 1 ? sqrt(fmax(0, a->s2 / a->n - m * m)) : 0; }
typedef struct { Acc step_off, step_on, ang_off, ang_on, delta, shift; } Win;
static Win win_walk, win_turn, win_both;
static double prev_off[3], prev_on[3], prev_ang_off, prev_ang_on;
static int have_prev;

static void proj(const Cur *c, const double Wp[3], double P[3]) {
    double d[3] = {Wp[0] - c->O[0], Wp[1] - c->O[1], Wp[2] - c->O[2]};
    for (int i = 0; i < 3; i++) P[i] = c->c[i] + (double)c->K * (c->R[3 * i] * d[0] + c->R[3 * i + 1] * d[1] + c->R[3 * i + 2] * d[2]);
}
static void apply(const VrSmCorr *m, const double P[3], double out[3]) {
    for (int i = 0; i < 3; i++) out[i] = (double)m->m[i] * P[0] + (double)m->m[4 + i] * P[1] + (double)m->m[8 + i] * P[2] + (double)m->m[12 + i];
}

static void take_scenes(uint32_t rel) {
    static VrScene sc;
    while (mv_host_scene(&sc)) {
        n_scene++;
        int ok = sc.user_space && sc.has_3d && sc.world_valid && !sc.cine_active;
        const VrCamRecord *r = vrcam_last_record();
        if (!ok || !r || !r->active) continue;
        n_valid++;
        Cur n;
        memset(&n, 0, sizeof n);
        n.valid = 1;
        n.K = sc.user_k * sc.scale;
        for (int i = 0; i < 3; i++) { n.O[i] = sc.origin_game[i]; n.c[i] = sc.user_p_ref[i] - sc.user_R[3 * i + 1] * sc.user_eye_h; }
        memcpy(n.R, sc.user_R, sizeof n.R);
        /* la marca de tiempo de juego y el giro de la cámara que la construyó: el registro más reciente es el de esta escena o uno posterior; se toma por secuencia */
        const VrCamRecord *rs = NULL;
        if (sc.src) for (uint32_t i = 0; i < sc.src->nops; i++) if (sc.src->ops[i].type == GPU_OP_POLY && sc.src->ops[i].v[0].vvalid) { rs = vrcam_seq_record(sc.src->ops[i].seq); break; }
        if (!rs) rs = r;
        n.game_s = rs->vbl / 50.0; n.turn = rs->turn_total_deg;
        if (trace) fprintf(trace, "S %u %.6f %.3f %.3f %.3f %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.9g %.6f %.6f %.6f %.9f %u %d %d %d\n", rs->vbl, vclock, n.O[0], n.O[1], n.O[2],
                                (double)n.R[0], (double)n.R[1], (double)n.R[2], (double)n.R[3], (double)n.R[4], (double)n.R[5], (double)n.R[6], (double)n.R[7], (double)n.R[8], (double)n.K,
                                (double)n.c[0], (double)n.c[1], (double)n.c[2], n.turn, rs->epoch, rs->eye[0], rs->eye[1], rs->eye[2]);
        if (rs->vbl != last_vbl_seen) {
            if (last_vbl_seen != 0xFFFFFFFFu) { uint32_t g = rs->vbl - last_vbl_seen; gap_hist[g < 15 ? g : 15]++; }
            last_vbl_seen = rs->vbl;
        }
        if (!have_W) { W[0] = n.O[0]; W[1] = n.O[1]; W[2] = n.O[2] + 2500.0; have_W = 1; }
        if (cur.valid && n.game_s > cur.game_s + 1e-6) cur_speed = sqrt(((double)n.O[0] - cur.O[0]) * ((double)n.O[0] - cur.O[0]) + ((double)n.O[2] - cur.O[2]) * ((double)n.O[2] - cur.O[2])) / (n.game_s - cur.game_s);
        cur = n;
    }
}

static void tick(double t, uint32_t rel, int inj_turn, int in_walk, int in_turn) {
    vclock = t;
    if (inj_turn) for (int i = 0; i < nturns; i++) if (rel >= turns[i].t && rel < turns[i].t + turns[i].dur) mv_vr_turn(turns[i].deg / (float)turns[i].dur / 2.0f);
    take_scenes(rel);
    VrSmCorr corr;
    mv_vrhost_smooth(&corr);
    if (!cur.valid || !have_W) return;
    double P[3], Pc[3];
    proj(&cur, W, P);
    apply(&corr, P, Pc);
    double ang_off = atan2(P[0] - cur.c[0], P[2] - cur.c[2]) * 57.29577951308232, ang_on = atan2(Pc[0] - cur.c[0], Pc[2] - cur.c[2]) * 57.29577951308232;
    if (have_prev) {
        double so[3] = {P[0] - prev_off[0], P[1] - prev_off[1], P[2] - prev_off[2]}, sn[3] = {Pc[0] - prev_on[0], Pc[1] - prev_on[1], Pc[2] - prev_on[2]};
        double dao = ang_off - prev_ang_off, dan = ang_on - prev_ang_on;
        while (dao > 180) dao -= 360; while (dao < -180) dao += 360;
        while (dan > 180) dan -= 360; while (dan < -180) dan += 360;
        Win *w = in_turn ? (cur_speed > 1000.0 ? &win_both : &win_turn) : (in_walk && cur_speed > 1000.0 ? &win_walk : NULL);
        if (w) {
            acc(&w->step_off, sqrt(so[0] * so[0] + so[1] * so[1] + so[2] * so[2]) * 1000.0);
            acc(&w->step_on, sqrt(sn[0] * sn[0] + sn[1] * sn[1] + sn[2] * sn[2]) * 1000.0);
            acc(&w->ang_off, fabs(dao)); acc(&w->ang_on, fabs(dan));
            if (corr.active) { acc(&w->delta, fabs(corr.delta_deg)); acc(&w->shift, sqrt((double)corr.shift_m[0] * corr.shift_m[0] + (double)corr.shift_m[1] * corr.shift_m[1] + (double)corr.shift_m[2] * corr.shift_m[2]) * 1000.0); }
        }
    }
    memcpy(prev_off, P, sizeof P); memcpy(prev_on, Pc, sizeof Pc); prev_ang_off = ang_off; prev_ang_on = ang_on; have_prev = 1;
}

static uint16_t std_pad(uint32_t v) {
    uint16_t b = 0;
    if ((v >= 120 && v < 126) || (v >= 400 && v < 406)) b |= 1u << 3;
    if (v >= 97 && v % 97 < 6) b |= 1u << 13;
    if (v >= 211 && v % 211 < 6) b |= 1u << 7;
    return b;
}

static uint16_t driver(uint32_t vbl, uint16_t pad, const BridgeState *bs) {
    (void)pad;
    int ctl = bs && bs->in_control && bs->has_player && (bs->valid & BRIDGE_OK_ALL_CORE) == BRIDGE_OK_ALL_CORE && vrcam_active();
    double base = (double)vbl * 0.02;
    if (!have_t0) {
        vclock = base; set_head();
        inctl_run = ctl ? inctl_run + 1 : 0;
        if (inctl_run >= 30) { have_t0 = 1; t0_vbl = vbl; fprintf(stderr, "[smoothrun] T0 = VBlank %u\n", vbl); }
        else { tick(base - 0.015, 0, 0, 0, 0); tick(base - 0.005, 0, 0, 0, 0); return std_pad(vbl); }
    }
    uint32_t rel = vbl - t0_vbl;
    for (int i = 0; i < nhy; i++) if (rel >= hyev[i].t) hyaw_deg = hyev[i].deg;
    set_head();
    int in_walk = in_walk_at(rel);
    int in_turn = 0;
    for (int i = 0; i < nturns; i++) if (turns[i].dur > 1 && rel >= turns[i].t + 10 && rel < turns[i].t + turns[i].dur) in_turn = 1;
    /* dos ticks de render por VBlank, a +5 y +15 ms del VBlank anterior (las escenas del VBlank anterior ya están ensambladas) */
    tick(base - 0.015, rel, 1, in_walk, in_turn);
    tick(base - 0.005, rel, 1, in_walk, in_turn);
    vclock = base;   /* el ensamblado de las escenas de este VBlank se marca con su instante */
    if (log_every && rel % log_every == 0 && bs) fprintf(stderr, "[t=%u] pos=(%d,%d,%d) yaw=%d mov=%d\n", rel, bs->ctl_pos.x, bs->ctl_pos.y, bs->ctl_pos.z, bs->ctl_rot.y, bs->moving);
    if (trace && bs) fprintf(trace, "P %u %d %d %d %d %d\n", vbl, bs->ctl_pos.x, bs->ctl_pos.y, bs->ctl_pos.z, bs->ctl_rot.y, bs->moving);
    uint16_t b = 0;
    for (int i = 0; i < nwalks; i++) if (rel >= walks[i].t && rel < walks[i].t + walks[i].dur) b |= 1u << 4;
    return b;
}

static void report_win(const char *name, const Win *w) {
    if (w->step_off.n < 10) { printf("%-8s sin muestras suficientes (%.0f)\n", name, w->step_off.n); return; }
    double mo = mean(&w->step_off), mn = mean(&w->step_on);
    printf("%-8s ticks %.0f | paso medio sin %.2f mm  con %.2f mm | desviacion sin %.2f con %.2f | CV sin %.3f con %.3f | max/medio sin %.2f con %.2f | ang paso medio sin %.3f con %.3f gr, max sin %.3f con %.3f | giro pendiente compensado: medio %.2f max %.2f gr | traslacion extrapolada: media %.1f max %.1f mm\n",
           name, w->step_off.n, mo, mn, sdev(&w->step_off), sdev(&w->step_on), mo > 0 ? sdev(&w->step_off) / mo : 0, mn > 0 ? sdev(&w->step_on) / mn : 0,
           mo > 0 ? w->step_off.mx / mo : 0, mn > 0 ? w->step_on.mx / mn : 0, mean(&w->ang_off), mean(&w->ang_on), w->ang_off.mx, w->ang_on.mx,
           mean(&w->delta), w->delta.mx, mean(&w->shift), w->shift.mx);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: %s <exe> <disco> [--frames N] [--walk T:dur] [--turn T:grados:dur] [--hyaw-at T:grados] [--log N] [--nosmooth]\n", argv[0]); return 1; }
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) nframes = (uint32_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log_every = (uint32_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--nosmooth")) smooth_on = 0;
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace = fopen(argv[++i], "w");
        else if (!strcmp(argv[i], "--walk") && i + 1 < argc && nwalks < 16) { unsigned t = 0, d = 0; sscanf(argv[++i], "%u:%u", &t, &d); walks[nwalks++] = (WalkEv){t, d}; }
        else if (!strcmp(argv[i], "--turn") && i + 1 < argc && nturns < 16) { unsigned tt = 0, dd = 1; float dg = 0; sscanf(argv[++i], "%u:%f:%u", &tt, &dg, &dd); turns[nturns++] = (TurnEv){tt, dd ? dd : 1, dg}; }
        else if (!strcmp(argv[i], "--hyaw-at") && i + 1 < argc && nhy < 16) { unsigned tt = 0; float dg = 0; sscanf(argv[++i], "%u:%f", &tt, &dg); hyev[nhy++] = (HyEv){tt, dg}; }
    }
    const char *sigs = getenv("MV_SIGS");
    if (sigs) mv_host_set_sigs(sigs);
    mkdir("build-host", 0755);
    mkdir("build-host/card", 0755);
    mv_vrhost_install();
    mv_vrhost_set_clock(clock_fn);
    mv_vrhost_set_smooth(smooth_on);
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
    VrSmoothStats st;
    mv_vrhost_smooth_stats(&st);
    uint32_t tot = 0; for (int i = 0; i < 16; i++) tot += gap_hist[i];
    printf("escenas entregadas %u, de mundo con camara VR %u; pushes %u resets %u; evals %u activos %u; periodo medido %.1f ms, rho %.3f\n", n_scene, n_valid, st.pushes, st.resets, st.evals, st.active_evals, st.period_s * 1000.0, st.rho);
    printf("huecos entre escenas (VBlanks de 20 ms):");
    for (int i = 1; i < 16; i++) if (gap_hist[i]) printf(" %d:%u", i, gap_hist[i]);
    double gm = 0; for (int i = 1; i < 16; i++) gm += (double)i * gap_hist[i];
    printf(" -> %.2f escenas nuevas por segundo de juego (media de %u huecos)\n", tot && gm > 0 ? 50.0 * tot / gm : 0.0, tot);
    report_win("andar", &win_walk);
    report_win("girar", &win_turn);
    report_win("ambos", &win_both);
    if (smooth_on && win_walk.step_off.n >= 30) {
        double mo = mean(&win_walk.step_off), mn = mean(&win_walk.step_on);
        CHECK(sdev(&win_walk.step_on) / (mn > 0 ? mn : 1) < 0.6 * sdev(&win_walk.step_off) / (mo > 0 ? mo : 1), "andar: el CV del paso no mejora");
    }
    if (smooth_on && win_turn.step_off.n >= 30) CHECK(win_turn.delta.mx > 0.1, "girar: la correccion no se activo");
    if (trace) fclose(trace);
    printf(fails ? "FALLOS: %d\n" : "OK: smoothrun\n", fails);
    return fails ? 1 : 0;
}
