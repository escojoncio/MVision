/* Pruebas de vr_smooth (sin datos del juego): secuencias sintéticas de escenas a 25 Hz con movimiento constante, fluctuación del reloj de la emulación, arranque y parada,
 * giro del stick y cortes, evaluadas a 100 Hz. Mide el ERROR DE POSICIÓN respecto al movimiento ideal continuo y la FLUCTUACIÓN de paso (jitter: desviación típica de
 * la diferencia entre el avance de un fotograma y el avance ideal). Compara con la corrección apagada (el mundo se queda en la última escena).
 *   make -C vr/host test_smooth */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_smooth.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FALLO %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

static double rnd(void) { return (double)rand() / (double)RAND_MAX; }

/* P(W) = c + R K (W - O): la fórmula de vr_scene (user_map) */
static void rot_form(float th_deg, float R[9]) {
    float th = th_deg * 0.017453292519943295f, cs = cosf(th), sn = sinf(th);
    float r[9] = {cs, 0, -sn, 0, 1, 0, sn, 0, cs};
    memcpy(R, r, sizeof r);
}
static void project(const float R[9], float K, const float c[3], const float O[3], const double W[3], double P[3]) {
    double d[3] = {W[0] - O[0], W[1] - O[1], W[2] - O[2]};
    for (int i = 0; i < 3; i++) P[i] = c[i] + K * (R[3 * i] * d[0] + R[3 * i + 1] * d[1] + R[3 * i + 2] * d[2]);
}
static void apply(const VrSmCorr *m, const double P[3], double out[3]) {
    for (int i = 0; i < 3; i++) out[i] = m->m[i] * P[0] + m->m[4 + i] * P[1] + m->m[8 + i] * P[2] + m->m[12 + i];
}

typedef struct {
    double v_x;            /* unidades/s en x (parada en t_stop) */
    double t_stop, t_start;
    double scene_hz, jitter_ms, run_s;
    double turn_dps, turn_t0, turn_t1;   /* giro del stick pedido (grados/s, + izquierda) */
    double turn_lag_s;     /* retardo de la tubería: giro "tomado" por la emulación = pedido(t - lag) */
    int smooth;
    double teleport_t;     /* >0: salto de 4000 unidades del cuerpo en x */
    double settle_t0, settle_t1;   /* intervalo que se excluye de las métricas (el propio corte) */
    double geom_ahead_s;   /* la geometría dibujada es de un registro del motor POSTERIOR al de la muestra que se empuja (escena nativa frente a la lista de la GPU) */
    int own_sample;        /* 1: la muestra empujada es la de la geometría dibujada (vrsm_make_sample de su propio registro) */
} Sim;

typedef struct { double rms_err_mm, max_err_mm, jitter_mm, max_step_dev_mm, rms_ang_deg, max_ang_deg; int active; } Res;

static double req_turn(const Sim *m, double t) {
    if (m->turn_dps == 0) return 0;
    double a = t < m->turn_t0 ? m->turn_t0 : (t > m->turn_t1 ? m->turn_t1 : t);
    return m->turn_dps * (a - m->turn_t0);
}
static double body_x(const Sim *m, double t) {
    /* x(t): 0 hasta t_start, v hasta t_stop, quieto */
    double a = t < m->t_start ? m->t_start : (t > m->t_stop ? m->t_stop : t);
    double x = m->v_x * (a - m->t_start);
    if (m->teleport_t > 0 && t >= m->teleport_t) x += 4000.0;
    return x;
}

static Res run(const Sim *m, VrSmooth *sm) {
    const double upm = 1000.0, K = 1.0 / upm;
    const float c[3] = {0.0f, 0.0f - 1.65f, 0.0f};   /* p_ref - (0, h, 0) */
    const double W[3] = {-2500.0, 0.0, 3000.0};      /* punto fijo del mundo */
    double dt_scene = 1.0 / m->scene_hz, dt_r = 0.01;
    int k = 0;
    double jt_k = 0.0;
    srand(7);
    jt_k = (rnd() * 2.0 - 1.0) * m->jitter_ms * 1e-3;
    VrSmSample cur;
    int have_scene = 0;
    double P_i[3] = {0}, prev_disp[3] = {0}, prev_ideal[3] = {0};
    double se = 0, mx = 0, sj = 0, md = 0, sa = 0, ma = 0;
    int n = 0, nj = 0;
    int first = 1;
    for (double t = 0.0; t < m->run_s; t += dt_r) {
        /* la emulación publica escenas a su ritmo con fluctuación de reloj; el renderer las toma en el primer fotograma posterior a su ensamblado */
        for (;;) {
            double tg = k * dt_scene;                               /* tiempo de juego de la escena k */
            double tb = tg + jt_k + (m->own_sample ? m->geom_ahead_s : 0.0);   /* reloj de pared en que se ensambló (con fluctuación); la escena nativa, de un registro posterior, se captura después */
            if (t < tb) break;
            memset(&cur, 0, sizeof cur);
            cur.valid = 1;
            cur.t_wall = tb; cur.game_s = tg;
            double tgeo = tg + m->geom_ahead_s;                        /* instante del registro de la geometría que se dibuja */
            double tsm = m->own_sample ? tgeo : tg;                    /* instante que declara la muestra */
            double bx = body_x(m, tgeo);
            double taken = req_turn(m, tgeo - m->turn_lag_s);          /* giro que la emulación ya había tomado al construir la cámara */
            float Og[3] = {(float)bx, 0, 0}, Rg[9];
            rot_form((float)(180.0 + 90.0 + taken), Rg);
            const float pref[3] = {0.0f, 0.0f, 0.0f};
            double bxs = body_x(m, tsm), takens = req_turn(m, tsm - m->turn_lag_s);
            float Os[3] = {(float)bxs, 0, 0}, Rs[9];
            rot_form((float)(180.0 + 90.0 + takens), Rs);
            vrsm_make_sample(&cur, Os, Rs, (float)K, pref, 1.65f, tsm, takens, (m->teleport_t > 0 && tg >= m->teleport_t) ? 2 : 1, tb);
            project(Rg, (float)K, c, Og, W, P_i);
            have_scene = 1;
            vrsm_push(sm, &cur, t);
            k++;
            jt_k = (rnd() * 2.0 - 1.0) * m->jitter_ms * 1e-3;
        }
        if (!have_scene) continue;
        VrSmCorr corr;
        if (m->smooth) vrsm_eval(sm, t, req_turn(m, t), &corr);
        else { memset(&corr, 0, sizeof corr); corr.m[0] = corr.m[5] = corr.m[10] = corr.m[15] = 1; }
        double disp[3];
        apply(&corr, P_i, disp);
        /* ideal: el mismo punto con el cuerpo y el giro al instante t + lead (modelo continuo) */
        double te = t + sm->cfg.lead_s;
        double bxi = body_x(m, te);
        float Oi[3] = {(float)bxi, 0, 0}, Ri[9];
        rot_form((float)(180.0 + 90.0 + req_turn(m, t)), Ri);
        double ideal[3];
        project(Ri, (float)K, c, Oi, W, ideal);
        if (t > 0.5 && !(m->settle_t1 > 0 && t >= m->settle_t0 && t < m->settle_t1)) {
            double e[3] = {disp[0] - ideal[0], disp[1] - ideal[1], disp[2] - ideal[2]};
            double em = sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]) * 1000.0;
            se += em * em; if (em > mx) mx = em;
            /* ángulo: dirección del punto respecto a la cabeza (p_ref) en el plano horizontal */
            double a1 = atan2(disp[0] - c[0], disp[2] - c[2]), a2 = atan2(ideal[0] - c[0], ideal[2] - c[2]);
            double da = (a1 - a2) * 57.29577951308232; while (da > 180) da -= 360; while (da < -180) da += 360;
            sa += da * da; if (fabs(da) > ma) ma = fabs(da);
            n++;
            if (!first) {
                double sd[3] = {disp[0] - prev_disp[0], disp[1] - prev_disp[1], disp[2] - prev_disp[2]};
                double si[3] = {ideal[0] - prev_ideal[0], ideal[1] - prev_ideal[1], ideal[2] - prev_ideal[2]};
                double dv = sqrt((sd[0] - si[0]) * (sd[0] - si[0]) + (sd[1] - si[1]) * (sd[1] - si[1]) + (sd[2] - si[2]) * (sd[2] - si[2])) * 1000.0;
                sj += dv * dv; nj++; if (dv > md) md = dv;
            }
            first = 0;
        }
        memcpy(prev_disp, disp, sizeof disp); memcpy(prev_ideal, ideal, sizeof ideal);
    }
    Res r;
    r.rms_err_mm = sqrt(se / (n ? n : 1)); r.max_err_mm = mx; r.jitter_mm = sqrt(sj / (nj ? nj : 1)); r.max_step_dev_mm = md;
    r.rms_ang_deg = sqrt(sa / (n ? n : 1)); r.max_ang_deg = ma; r.active = (int)sm->st.active_evals;
    return r;
}

static void show(const char *name, const Res *off, const Res *on) {
    printf("%-34s sin: err rms %.2f max %.2f mm, jitter %.3f mm, paso max %.2f mm | con: err rms %.2f max %.2f mm, jitter %.3f mm, paso max %.2f mm | ang rms %.3f max %.3f (sin %.3f)\n",
           name, off->rms_err_mm, off->max_err_mm, off->jitter_mm, off->max_step_dev_mm, on->rms_err_mm, on->max_err_mm, on->jitter_mm, on->max_step_dev_mm,
           on->rms_ang_deg, on->max_ang_deg, off->max_ang_deg);
}

static Res runboth(const char *name, Sim m, Res *offr) {
    VrSmooth a, b;
    vrsm_init(&a, NULL); vrsm_init(&b, NULL);
    m.smooth = 0; Res off = run(&m, &a);
    m.smooth = 1; Res on = run(&m, &b);
    show(name, &off, &on);
    if (offr) *offr = off;
    return on;
}

int main(void) {
    Sim base = {.v_x = 600.0, .t_start = 0.0, .t_stop = 1e9, .scene_hz = 25.0, .jitter_ms = 0.0, .run_s = 6.0, .turn_lag_s = 0.1};

    /* 1) movimiento constante a 25 escenas/s, reloj perfecto: el error debe ser ~0 y el paso uniforme */
    Res off, on = runboth("constante 25 Hz sin fluctuacion", base, &off);
    CHECK(on.rms_err_mm < 0.5 && on.jitter_mm < 0.3, "constante: err rms %.3f jitter %.3f", on.rms_err_mm, on.jitter_mm);
    CHECK(off.jitter_mm > 5.0, "la referencia sin corregir debería tener judder (jitter %.3f)", off.jitter_mm);

    /* 2) con fluctuación de +-6 ms del reloj de la emulación (el ritmo acompasado al audio) */
    Sim j = base; j.jitter_ms = 6.0;
    on = runboth("constante 25 Hz jitter +-6 ms", j, &off);
    CHECK(on.rms_err_mm < 3.0 && on.jitter_mm < 1.5, "jitter reloj: err rms %.3f jitter %.3f", on.rms_err_mm, on.jitter_mm);
    CHECK(on.jitter_mm < off.jitter_mm / 4.0, "mejora < 4x: %.3f vs %.3f", on.jitter_mm, off.jitter_mm);

    /* 3) 50 escenas/s y movimiento más rápido */
    Sim f = base; f.scene_hz = 50.0; f.v_x = 1500.0; f.jitter_ms = 4.0;
    on = runboth("50 Hz 1.5 m/s jitter +-4 ms", f, &off);
    CHECK(on.rms_err_mm < 3.0 && on.jitter_mm < 1.5, "50 Hz: err rms %.3f jitter %.3f", on.rms_err_mm, on.jitter_mm);

    /* 4) 12,5 escenas/s (juego muy lento) */
    Sim sl = base; sl.scene_hz = 12.5; sl.jitter_ms = 6.0;
    on = runboth("12.5 Hz jitter +-6 ms", sl, &off);
    CHECK(on.jitter_mm < off.jitter_mm / 3.0, "12,5 Hz: %.3f vs %.3f", on.jitter_mm, off.jitter_mm);

    /* 5) arranque y parada: el error de predicción se disipa; sin saltos grandes por fotograma */
    Sim sp = base; sp.t_start = 1.0; sp.t_stop = 3.0; sp.jitter_ms = 3.0;
    on = runboth("arranque y parada", sp, &off);
    CHECK(on.max_err_mm < 40.0, "arranque/parada: error máximo %.2f mm", on.max_err_mm);
    CHECK(on.max_step_dev_mm < off.max_step_dev_mm, "paso máximo: %.2f vs %.2f", on.max_step_dev_mm, off.max_step_dev_mm);

    /* 6) giro del stick a 90 grados/s con 100 ms de retardo de la tubería: el giro mostrado coincide con el pedido (error angular ~0), sin la corrección va 9 grados por detrás */
    Sim tn = base; tn.v_x = 0; tn.turn_dps = 90; tn.turn_t0 = 1.0; tn.turn_t1 = 3.0; tn.jitter_ms = 3.0;
    on = runboth("giro 90 gr/s lag 100 ms", tn, &off);
    CHECK(on.rms_ang_deg < 0.2 && on.max_ang_deg < 0.5, "giro: ang rms %.3f max %.3f", on.rms_ang_deg, on.max_ang_deg);
    CHECK(off.max_ang_deg > 5.0, "referencia sin corregir debería ir retrasada (max %.3f)", off.max_ang_deg);

    /* 7) andar y girar a la vez */
    Sim wt = tn; wt.v_x = 700; wt.jitter_ms = 5.0;
    on = runboth("andar y girar", wt, &off);
    CHECK(on.rms_ang_deg < 0.2 && on.jitter_mm < 2.0, "andar+girar: ang %.3f jitter %.3f", on.rms_ang_deg, on.jitter_mm);

    /* 8) corte (teletransporte de 4 m): no debe extrapolar ni arrastrar un error enorme */
    Sim tp = base; tp.teleport_t = 2.0; tp.settle_t0 = 1.98; tp.settle_t1 = 2.25; tp.jitter_ms = 3.0; tp.v_x = 400;
    on = runboth("teletransporte", tp, &off);
    CHECK(on.max_err_mm < 20.0, "teletransporte: error máximo %.2f mm", on.max_err_mm);

    /* 8b) la escena que se dibuja (nativa) es de un registro del motor posterior (40 ms) al de la muestra que recibe el suavizador (la de la lista de la GPU): la corrección
     * la mueve de más. Con la muestra de la propia escena (vrsm_make_sample), andando y girando, el desfase con la cabeza (posición ideal en el instante de presentación) es 0. */
    {
        Sim w = wt; w.jitter_ms = 0.0; w.geom_ahead_s = 0.040; w.own_sample = 0;
        Res offx, bad = runboth("nativa +1 registro, muestra de la lista", w, &offx);
        w.own_sample = 1;
        Res good = runboth("nativa +1 registro, muestra propia", w, &offx);
        CHECK(bad.max_ang_deg > 2.0 || bad.rms_err_mm > 20.0, "la muestra de otro registro debería desfasar la escena (err rms %.2f mm, ang max %.3f)", bad.rms_err_mm, bad.max_ang_deg);
        CHECK(good.rms_err_mm < 3.0 && good.max_ang_deg < 0.5, "muestra propia: desfase con la cabeza err rms %.3f mm, ang max %.3f gr (esperado ~0)", good.rms_err_mm, good.max_ang_deg);
        printf("  desfase de la escena nativa con la cabeza: muestra de la lista %.1f mm / %.2f gr -> muestra propia %.2f mm / %.3f gr\n", bad.rms_err_mm, bad.max_ang_deg, good.rms_err_mm, good.max_ang_deg);
    }

    /* 9) desactivado: identidad exacta */
    {
        VrSmoothCfg cfg; vrsm_cfg_default(&cfg); cfg.enabled = 0;
        VrSmooth s; vrsm_init(&s, &cfg);
        VrSmSample sm; memset(&sm, 0, sizeof sm);
        sm.valid = 1; sm.K = 0.001f; rot_form(270, sm.R);
        vrsm_push(&s, &sm, 0.0);
        VrSmCorr c; vrsm_eval(&s, 0.01, 50.0, &c);
        CHECK(!c.active && c.m[0] == 1 && c.m[12] == 0, "desactivado debe ser identidad");
    }
    /* 10) escena no válida: identidad */
    {
        VrSmooth s; vrsm_init(&s, NULL);
        VrSmSample sm; memset(&sm, 0, sizeof sm);
        sm.valid = 1; sm.K = 0.001f; rot_form(270, sm.R); sm.t_wall = 0; sm.game_s = 0;
        vrsm_push(&s, &sm, 0.0);
        sm.valid = 0; vrsm_push(&s, &sm, 0.04);
        VrSmCorr c; vrsm_eval(&s, 0.05, 0.0, &c);
        CHECK(!c.active, "escena no válida: identidad");
    }
    /* 11) álgebra exacta: dos escenas con movimiento lineal y giro; la corrección extrapolada a la tercera coincide con ella (sin fluctuación) */
    {
        VrSmooth s; vrsm_init(&s, NULL);
        s.cfg.lead_s = 0.0f; s.cfg.blend_s = 1e-3f;
        VrSmSample a; memset(&a, 0, sizeof a);
        a.valid = 1; a.K = 0.001f; rot_form(250, a.R); a.c[1] = -1.6f; a.c[0] = 0.3f; a.c[2] = -0.2f; a.epoch = 1;
        VrSmSample b = a, cc = a;
        a.t_wall = 0.0; a.game_s = 0.0; a.B[0] = 100; a.B[2] = -50; a.turn_deg = 0;
        b.t_wall = 0.04; b.game_s = 0.04; b.B[0] = 130; b.B[2] = -10; b.turn_deg = 0;
        vrsm_push(&s, &a, 0.0);
        s.cfg.vel_new = 1.0f;
        vrsm_push(&s, &b, 0.04);
        /* a las 0,08 s: velocidad constante (30,40)/0,04 -> B = (160, 30) y 20 grados de giro pedidos */
        cc = b; cc.t_wall = 0.08; cc.game_s = 0.08; cc.B[0] = 160; cc.B[2] = 30; cc.turn_deg = 20; rot_form(250 + 20, cc.R);
        double W[3] = {500, 100, 900}, Pi[3], P3[3], Pc[3];
        float Ob[3] = {b.B[0], b.B[1], b.B[2]}, O3[3] = {cc.B[0], cc.B[1], cc.B[2]};
        project(b.R, b.K, b.c, Ob, W, Pi);
        project(cc.R, cc.K, cc.c, O3, W, P3);
        VrSmCorr corr;
        vrsm_eval(&s, 0.08, 20.0, &corr);
        apply(&corr, Pi, Pc);
        double d = sqrt((Pc[0] - P3[0]) * (Pc[0] - P3[0]) + (Pc[1] - P3[1]) * (Pc[1] - P3[1]) + (Pc[2] - P3[2]) * (Pc[2] - P3[2]));
        CHECK(d < 1e-4, "álgebra: distancia %.6g m", d);
        printf("%-34s distancia a la escena real %.3g m\n", "algebra exacta (2 escenas -> 3a)", d);
    }
    printf(fails ? "FALLOS: %d\n" : "OK: vr_smooth\n", fails);
    return fails ? 1 : 0;
}
