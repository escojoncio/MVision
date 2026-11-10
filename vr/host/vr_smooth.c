/* vr_smooth: ver vr_smooth.h. Hilo de render; sin estado global. */
#include "vr_smooth.h"
#include <math.h>
#include <string.h>

#define DEG2RAD 0.017453292519943295

static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int fin3(const float *v) { return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]); }

void vrsm_cfg_default(VrSmoothCfg *c) {
    c->enabled = 1;
    c->lead_s = 0.012f;
    c->blend_s = 0.045f;
    c->turn_lead = 1.0f;
    c->vel_new = 0.6f;
    c->pll_gain = 0.15f;
    c->cut_units = 2500.0f;
    c->max_gap_s = 0.5f;
    c->max_turn_deg = 400.0f;
}

void vrsm_reset(VrSmooth *s) {
    s->have = 0;
    memset(s->vel, 0, sizeof s->vel);
    memset(s->err, 0, sizeof s->err);
    s->t_err = 0.0;
    s->have_prev_game = 0;
    s->rho_anchor = 0;
}

void vrsm_init(VrSmooth *s, const VrSmoothCfg *c) {
    memset(s, 0, sizeof *s);
    if (c) s->cfg = *c; else vrsm_cfg_default(&s->cfg);
    s->rho = 1.0;
    s->period = 0.04;
    s->st.rho = 1.0;
    s->st.period_s = 0.04;
}

/* Horizonte efectivo de extrapolación: lineal hasta ~1,25 periodos de escena y después tiende suavemente a un tope (si el juego deja de publicar no se sigue
 * corriendo indefinidamente ni se frena en seco). */
static double soft_horizon(const VrSmooth *s, double h) {
    if (h <= 0.0) return 0.0;
    double a = 1.25 * s->period + 0.01;
    double hmax = clampd(2.5 * s->period + 0.02, 0.06, 0.2);
    if (a > hmax - 0.01) a = hmax - 0.01;
    if (h <= a) return h;
    double w = hmax - a;
    return a + w * (1.0 - exp(-(h - a) / w));
}

/* Desplazamiento del cuerpo (unidades del juego) respecto a la posición de la escena en el instante `te`, sin el error pendiente. */
static void shift_pred(const VrSmooth *s, double te, float out[3]) {
    double hg = soft_horizon(s, te - s->ts);
    double k = s->rho * hg;
    for (int i = 0; i < 3; i++) out[i] = (float)(s->vel[i] * k);
}

static double mag_m(const VrSmooth *s, const float v[3]) {
    return sqrt((double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2]) * s->cur.K;
}

void vrsm_push(VrSmooth *s, const VrSmSample *sm, double t_now) {
    s->st.pushes++;
    if (!s->cfg.enabled || !sm || !sm->valid || !fin3(sm->B) || !isfinite(sm->K) || sm->K <= 0.0f || !isfinite(sm->turn_deg)) {
        if (s->have) s->st.resets++;
        vrsm_reset(s);
        return;
    }
    double te = t_now + s->cfg.lead_s;
    int hard = !s->have;
    if (!hard) {
        double dx = (double)sm->B[0] - s->cur.B[0], dz = (double)sm->B[2] - s->cur.B[2], dy = (double)sm->B[1] - s->cur.B[1];
        if (sm->epoch != s->cur.epoch || sqrt(dx * dx + dz * dz) > s->cfg.cut_units || fabs(dy) > s->cfg.cut_units * 0.6 ||
            sm->t_wall - s->cur.t_wall > s->cfg.max_gap_s || sm->t_wall < s->cur.t_wall - 1e-3)
            hard = 1;
    }
    if (hard) {
        if (s->have) s->st.resets++;
        s->cur = *sm;
        s->ts = sm->t_wall < t_now ? sm->t_wall : t_now;
        memset(s->vel, 0, sizeof s->vel);
        memset(s->err, 0, sizeof s->err);
        s->t_err = te;
        s->have = 1;
        if (!(s->rho > 0.25 && s->rho < 2.0)) s->rho = 1.0;
        return;
    }
    /* posición que se estaba mostrando (mundo, unidades) en el instante de presentación de este fotograma */
    float sh[3], eold[3];
    shift_pred(s, te, sh);
    double fe = exp(-(te - s->t_err) / (double)(s->cfg.blend_s > 1e-3f ? s->cfg.blend_s : 1e-3f));
    for (int i = 0; i < 3; i++) eold[i] = s->cur.B[i] + sh[i] + (float)(s->err[i] * fe);

    double dg = sm->game_s - s->cur.game_s;
    if (dg > 0.004) {
        double dtw = sm->t_wall - s->cur.t_wall;
        double wn = clampd(s->cfg.vel_new, 0.0, 1.0);
        for (int i = 0; i < 3; i++) {
            double vn = ((double)sm->B[i] - s->cur.B[i]) / dg;
            s->vel[i] = (float)(wn * vn + (1.0 - wn) * s->vel[i]);
        }
        (void)dtw;
        if (!s->rho_anchor) { s->rho_g0 = sm->game_s; s->rho_w0 = sm->t_wall; s->rho_anchor = 1; }
        else if (sm->t_wall - s->rho_w0 >= 1.0) {   /* ventana de 1 s: la fluctuación de cada reloj de ensamblado pesa <1 % */
            s->rho += 0.5 * (clampd((sm->game_s - s->rho_g0) / (sm->t_wall - s->rho_w0), 0.25, 2.0) - s->rho);
            s->rho_g0 = sm->game_s; s->rho_w0 = sm->t_wall;
        }
        s->period += 0.2 * (clampd(dg / s->rho, 0.005, 0.5) - s->period);
        double tpred = s->ts + dg / s->rho;
        s->ts = tpred + clampd(s->cfg.pll_gain, 0.0, 1.0) * (sm->t_wall - tpred);
    } else {
        s->ts = sm->t_wall < t_now ? sm->t_wall : t_now;
    }
    if (s->ts > t_now) s->ts = t_now;
    s->cur = *sm;

    float snew[3];
    shift_pred(s, te, snew);
    double e2 = 0.0;
    for (int i = 0; i < 3; i++) {
        float en = sm->B[i] + snew[i];
        s->err[i] = eold[i] - en;
        e2 += (double)s->err[i] * s->err[i];
    }
    if (sqrt(e2) > s->cfg.cut_units * 0.5) memset(s->err, 0, sizeof s->err);   /* un salto grande no es un error de predicción: se muestra tal cual */
    s->t_err = te;
    double em = mag_m(s, s->err);
    s->st.sum_err_m += em;
    if (em > s->st.max_err_m) s->st.max_err_m = em;
    s->st.rho = s->rho; s->st.period_s = s->period;
}

void vrsm_eval(VrSmooth *s, double t_now, double turn_req_deg, VrSmCorr *out) {
    memset(out, 0, sizeof *out);
    out->m[0] = out->m[5] = out->m[10] = out->m[15] = 1.0f;
    s->st.evals++;
    if (!s->cfg.enabled || !s->have) return;
    double delta = (double)s->cfg.turn_lead * (turn_req_deg - s->cur.turn_deg);
    if (!isfinite(delta) || fabs(delta) > s->cfg.max_turn_deg) return;
    double te = t_now + s->cfg.lead_s;
    float sh[3];
    shift_pred(s, te, sh);
    double fe = exp(-(te - s->t_err) / (double)(s->cfg.blend_s > 1e-3f ? s->cfg.blend_s : 1e-3f));
    for (int i = 0; i < 3; i++) sh[i] += (float)(s->err[i] * fe);

    double cs = cos(delta * DEG2RAD), sn = sin(delta * DEG2RAD);
    const float *R = s->cur.R;
    double ks[3] = {sh[0] * (double)s->cur.K, sh[1] * (double)s->cur.K, sh[2] * (double)s->cur.K};
    double rv[3] = {R[0] * ks[0] + R[1] * ks[1] + R[2] * ks[2], R[3] * ks[0] + R[4] * ks[1] + R[5] * ks[2], R[6] * ks[0] + R[7] * ks[1] + R[8] * ks[2]};
    /* T = -Q (R_i K shift);  P' = Q P + (c - Q c + T) */
    double qr[3] = {cs * rv[0] - sn * rv[2], rv[1], sn * rv[0] + cs * rv[2]};
    const float *c = s->cur.c;
    double qc[3] = {cs * c[0] - sn * c[2], c[1], sn * c[0] + cs * c[2]};
    double t[3] = {c[0] - qc[0] - qr[0], c[1] - qc[1] - qr[1], c[2] - qc[2] - qr[2]};
    out->m[0] = (float)cs;  out->m[1] = 0;  out->m[2] = (float)sn;  out->m[3] = 0;
    out->m[4] = 0;          out->m[5] = 1;  out->m[6] = 0;          out->m[7] = 0;
    out->m[8] = (float)-sn; out->m[9] = 0;  out->m[10] = (float)cs; out->m[11] = 0;
    out->m[12] = (float)t[0]; out->m[13] = (float)t[1]; out->m[14] = (float)t[2]; out->m[15] = 1;
    for (int i = 0; i < 16; i++) if (!isfinite(out->m[i])) { memset(out, 0, sizeof *out); out->m[0] = out->m[5] = out->m[10] = out->m[15] = 1.0f; return; }
    out->active = 1;
    out->delta_deg = (float)delta;
    for (int i = 0; i < 3; i++) out->shift_m[i] = (float)ks[i];
    double sm = sqrt(ks[0] * ks[0] + ks[1] * ks[1] + ks[2] * ks[2]);
    s->st.active_evals++;
    s->st.sum_shift_m += sm;
    if (sm > s->st.max_shift_m) s->st.max_shift_m = sm;
    if (fabs(delta) > s->st.max_delta_deg) s->st.max_delta_deg = fabs(delta);
}

void vrsm_stats(const VrSmooth *s, VrSmoothStats *out) { *out = s->st; }

void vrsm_make_sample(VrSmSample *out, const float origin[3], const float R[9], float K, const float p_ref[3], float eye_h, double game_s, double turn_deg, uint32_t epoch, double t_wall) {
    memset(out, 0, sizeof *out);
    out->t_wall = t_wall;
    if (!(K > 0.0f) || !isfinite(K)) return;
    out->valid = 1;
    out->game_s = game_s;
    /* B = el ojo (el origen está bajo él, a eye_h): el mismo mapa que con c = p_ref - R (0, eye_h, 0) y B = origen, pero así la altura del ojo (posturas, cuestas) también se
     * reparte por fotograma de render en vez de saltar con cada escena. El giro es sobre la vertical: c solo cuenta en x, z. */
    for (int i = 0; i < 3; i++) out->B[i] = origin[i];
    out->B[1] += eye_h / K;
    memcpy(out->R, R, sizeof out->R);
    out->K = K;
    for (int i = 0; i < 3; i++) out->c[i] = p_ref[i];
    out->turn_deg = turn_deg;
    out->epoch = epoch;
}
