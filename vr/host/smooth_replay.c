/* smooth_replay: repite la medida de smoothrun sobre una traza (--trace) SIN emular el juego, para ajustar vr_smooth con datos reales.
 *   smooth_replay <traza> [--from VBL] [--to VBL] [--lead s] [--blend s] [--velnew w] [--pll g] [--nosmooth] [--dump]
 * La traza trae por escena de mundo (S) el origen, la correspondencia al espacio del usuario y la marca de tiempo de juego, y por VBlank (P) la posicion del cuerpo del puente.
 * Los ticks de render son de 10 ms exactos, a +5 ms del ensamblado. Imprime el paso de un punto fijo del mundo por tick, sin y con correccion. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_smooth.h"

typedef struct { uint32_t vbl; double t; VrSmSample s; double eye[3]; } Sc;
typedef struct { uint32_t vbl; int x, y, z, yaw, mov; } Pp;
static Sc *sc; static size_t nsc, capsc;
static Pp *pp; static size_t npp, cappp;

static void proj(const VrSmSample *c, const double W[3], double P[3]) {
    double d[3] = {W[0] - c->B[0], W[1] - c->B[1], W[2] - c->B[2]};
    for (int i = 0; i < 3; i++) P[i] = c->c[i] + (double)c->K * (c->R[3 * i] * d[0] + c->R[3 * i + 1] * d[1] + c->R[3 * i + 2] * d[2]);
}
static void apply(const VrSmCorr *m, const double P[3], double out[3]) {
    for (int i = 0; i < 3; i++) out[i] = (double)m->m[i] * P[0] + (double)m->m[4 + i] * P[1] + (double)m->m[8 + i] * P[2] + (double)m->m[12 + i];
}
typedef struct { double n, s1, s2, mx; } Acc;
static void acc(Acc *a, double v) { a->n++; a->s1 += v; a->s2 += v * v; if (v > a->mx) a->mx = v; }
static double mean(const Acc *a) { return a->n ? a->s1 / a->n : 0; }
static double sdev(const Acc *a) { double m = mean(a); return a->n > 1 ? sqrt(fmax(0, a->s2 / a->n - m * m)) : 0; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: %s <traza> [--from V] [--to V] [--lead s] [--blend s] [--velnew w] [--pll g] [--nosmooth] [--dump]\n", argv[0]); return 1; }
    uint32_t from = 0, to = 0xFFFFFFFFu; int dump = 0;
    VrSmoothCfg cfg; vrsm_cfg_default(&cfg);
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--from") && i + 1 < argc) from = (uint32_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--to") && i + 1 < argc) to = (uint32_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--lead") && i + 1 < argc) cfg.lead_s = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--blend") && i + 1 < argc) cfg.blend_s = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--velnew") && i + 1 < argc) cfg.vel_new = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--pll") && i + 1 < argc) cfg.pll_gain = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--nosmooth")) cfg.enabled = 0;
        else if (!strcmp(argv[i], "--dump")) dump = 1;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 1; }
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == 'S') {
            Sc c; memset(&c, 0, sizeof c);
            double R[9], K, cc[3], O[3], turn, eye[3]; unsigned vbl, ep; double t;
            int e0, e1, e2;
            if (sscanf(line + 2, "%u %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %u %d %d %d", &vbl, &t, &O[0], &O[1], &O[2], &R[0], &R[1], &R[2], &R[3], &R[4], &R[5], &R[6], &R[7], &R[8],
                       &K, &cc[0], &cc[1], &cc[2], &turn, &ep, &e0, &e1, &e2) != 23) continue;
            eye[0] = e0; eye[1] = e1; eye[2] = e2;
            c.vbl = vbl; c.t = t; memcpy(c.eye, eye, sizeof eye);
            c.s.valid = 1; c.s.t_wall = t; c.s.game_s = vbl / 50.0; c.s.K = (float)K; c.s.turn_deg = turn; c.s.epoch = ep;
            for (int i = 0; i < 3; i++) { c.s.B[i] = (float)O[i]; c.s.c[i] = (float)cc[i]; }
            for (int i = 0; i < 9; i++) c.s.R[i] = (float)R[i];
            if (nsc == capsc) { capsc = capsc ? capsc * 2 : 1024; sc = realloc(sc, capsc * sizeof *sc); }
            sc[nsc++] = c;
        } else if (line[0] == 'P') {
            Pp p; if (sscanf(line + 2, "%u %d %d %d %d %d", &p.vbl, &p.x, &p.y, &p.z, &p.yaw, &p.mov) != 6) continue;
            if (npp == cappp) { cappp = cappp ? cappp * 2 : 4096; pp = realloc(pp, cappp * sizeof *pp); }
            pp[npp++] = p;
        }
    }
    fclose(f);
    if (!nsc) { fprintf(stderr, "sin escenas en la traza\n"); return 1; }
    if (dump) {   /* velocidad del origen (ojo) y del cuerpo por VBlank de escena */
        for (size_t i = 1; i < nsc; i++) {
            if (sc[i].vbl < from || sc[i].vbl > to) continue;
            double dt = (sc[i].vbl - sc[i - 1].vbl) * 0.02;
            if (dt <= 0) continue;
            double vx = (sc[i].s.B[0] - sc[i - 1].s.B[0]) / dt, vz = (sc[i].s.B[2] - sc[i - 1].s.B[2]) / dt;
            const Pp *a = NULL, *b = NULL;
            for (size_t k = 0; k < npp; k++) { if (pp[k].vbl == sc[i - 1].vbl) a = &pp[k]; if (pp[k].vbl == sc[i].vbl) b = &pp[k]; }
            printf("vbl %u dt %.2f eye(%.0f,%.0f) v(%.0f,%.0f)u/s |v|=%.0f turn %.2f", sc[i].vbl, dt, sc[i].s.B[0], sc[i].s.B[2], vx, vz, sqrt(vx * vx + vz * vz), sc[i].s.turn_deg);
            if (a && b) printf(" | cuerpo(%d,%d) v(%.0f,%.0f) yaw %d", b->x, b->z, (b->x - a->x) / dt, (b->z - a->z) / dt, b->yaw);
            printf("\n");
        }
        return 0;
    }
    VrSmooth sm; vrsm_init(&sm, &cfg);
    double W[3] = {sc[0].s.B[0], sc[0].s.B[1], sc[0].s.B[2] + 2500.0};
    Acc off, on, dl, sh;
    memset(&off, 0, sizeof off); memset(&on, 0, sizeof on); memset(&dl, 0, sizeof dl); memset(&sh, 0, sizeof sh);
    size_t next = 0, first = nsc, last = 0;
    for (size_t i = 0; i < nsc; i++) if (sc[i].vbl >= from && sc[i].vbl <= to) { if (first == nsc) first = i; last = i; }
    if (first == nsc) { fprintf(stderr, "sin escenas en la ventana\n"); return 1; }
    next = first;
    int have_prev = 0; double po[3] = {0}, pn[3] = {0};
    const VrSmSample *curs = NULL;
    double t_end = sc[last].t + 0.04;   /* hasta que llegaria la siguiente escena */
    for (double t = sc[first].t + 0.005; t < t_end; t += 0.01) {
        while (next < nsc && sc[next].t + 0.005 <= t + 1e-9) {
            if (sc[next].vbl >= from && sc[next].vbl <= to) { vrsm_push(&sm, &sc[next].s, t); curs = &sc[next].s; if (getenv("SR_DBG")) printf("push t %.3f vbl %u ts-t %.1f ms vel(%.0f,%.0f) per %.1f ms rho %.3f err(%.1f,%.1f) u\n", t, sc[next].vbl, (sm.ts - t) * 1000, sm.vel[0], sm.vel[2], sm.period * 1000, sm.rho, sm.err[0], sm.err[2]); }
            next++;
        }
        if (!curs) continue;
        VrSmCorr corr; vrsm_eval(&sm, t, curs->turn_deg, &corr);   /* sin giro pendiente: la traza de esta herramienta mide solo el avance */
        double P[3], Pc[3];
        proj(curs, W, P); apply(&corr, P, Pc);
        if (have_prev) {
            double so = sqrt((P[0] - po[0]) * (P[0] - po[0]) + (P[1] - po[1]) * (P[1] - po[1]) + (P[2] - po[2]) * (P[2] - po[2])) * 1000.0;
            double sn = sqrt((Pc[0] - pn[0]) * (Pc[0] - pn[0]) + (Pc[1] - pn[1]) * (Pc[1] - pn[1]) + (Pc[2] - pn[2]) * (Pc[2] - pn[2])) * 1000.0;
            if (getenv("SR_STEPS")) printf("t %.3f paso sin %.2f con %.2f shift %.1f\n", t, so, sn, corr.active ? sqrt((double)corr.shift_m[0]*corr.shift_m[0]+(double)corr.shift_m[2]*corr.shift_m[2])*1000.0 : 0.0);
            if (dump == 0) { acc(&off, so); acc(&on, sn); if (corr.active) acc(&sh, sqrt((double)corr.shift_m[0] * corr.shift_m[0] + (double)corr.shift_m[2] * corr.shift_m[2]) * 1000.0); }
        }
        memcpy(po, P, sizeof P); memcpy(pn, Pc, sizeof Pc); have_prev = 1;
    }
    VrSmoothStats st; vrsm_stats(&sm, &st);
    printf("ticks %.0f | paso medio sin %.2f con %.2f mm | desv sin %.2f con %.2f | CV sin %.3f con %.3f | max/medio sin %.2f con %.2f | traslacion media %.1f max %.1f mm | resets %u err-pred medio %.1f max %.1f mm\n",
           off.n, mean(&off), mean(&on), sdev(&off), sdev(&on), mean(&off) > 0 ? sdev(&off) / mean(&off) : 0, mean(&on) > 0 ? sdev(&on) / mean(&on) : 0,
           mean(&off) > 0 ? off.mx / mean(&off) : 0, mean(&on) > 0 ? on.mx / mean(&on) : 0, mean(&sh), sh.mx, st.resets, st.pushes ? st.sum_err_m * 1000.0 / st.pushes : 0.0, st.max_err_m * 1000.0);
    return 0;
}
