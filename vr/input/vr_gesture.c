#include "vr_gesture.h"
#include <math.h>
#include <string.h>

static int fin(float v) { return isfinite(v); }
static int fin3(const float *v) { return fin(v[0]) && fin(v[1]) && fin(v[2]); }

void vrg_cfg_defaults(vrg_cfg_t *c) {
    memset(c, 0, sizeof *c);
    c->enable = 1; c->punch = c->grab = c->knock = c->peek = 1;
    c->right_handed = 1;
    c->punch_speed = 1.8f; c->knock_speed = 1.2f; c->reach_min = 0.22f; c->melee_range = 2.5f;
    c->grab_range = 1.3f; c->neck_radius = 0.45f; c->neck_height = 0.45f;
    c->grip_on = 0.65f; c->grip_off = 0.35f; c->peek_on = 0.12f; c->peek_off = 0.06f; c->cooldown_s = 0.3f;
}
void vrg_reset(vrg_t *g) {
    vrg_cfg_t c = g->cfg;
    memset(g, 0, sizeof *g);
    g->cfg = c; g->grab_side = -1; g->warm = 2;
}
void vrg_init(vrg_t *g) { memset(g, 0, sizeof *g); vrg_cfg_defaults(&g->cfg); g->grab_side = -1; g->warm = 2; }

static float hdist(const float *a, const float *b) { float dx = a[0] - b[0], dz = a[2] - b[2]; return sqrtf(dx * dx + dz * dz); }

#define JUMP_MPS 12.0f      /* entre dos muestras seguidas: más rápido que cualquier mano = salto de pose (recentrado, seguimiento perdido) */
#define VEL_MAX_MPS 10.0f   /* velocidad media por encima de la cual la medida se descarta */
#define SETTLE_S 0.12f

/* Velocidad de la mano (m/s) con las últimas 4 muestras y SUS marcas de tiempo (no el dt del VBlank: el VBlank y el fotograma de render
 * no van sincronizados). Devuelve 0 mientras no haya historia fiable; los saltos de pose reinician la historia y abren un periodo de calma. */
static int hand_vel(vrg_t *g, int s, const float p[3], double t, float v[3]) {
    v[0] = v[1] = v[2] = 0;
    int n = g->npos[s], push = 1, jump = 0;
    if (n > 0) {
        double dtl = t - g->tim[s][0];
        if (dtl < 0) { n = 0; jump = 1; }                      /* el reloj retrocede: historia inservible */
        else if (dtl < 0.004) push = 0;                        /* misma muestra (el render no publicó otra) */
        else {
            float dx = p[0] - g->pos[s][0][0], dy = p[1] - g->pos[s][0][1], dz = p[2] - g->pos[s][0][2];
            if (dtl > 0.25 || sqrtf(dx * dx + dy * dy + dz * dz) > JUMP_MPS * dtl) { n = 0; jump = 1; }
        }
    }
    if (push) {
        for (int k = (n < 3 ? n : 3); k > 0; k--) { memcpy(g->pos[s][k], g->pos[s][k - 1], sizeof g->pos[s][0]); g->tim[s][k] = g->tim[s][k - 1]; }
        memcpy(g->pos[s][0], p, 3 * sizeof(float)); g->tim[s][0] = t;
        g->npos[s] = n < 4 ? n + 1 : 4;
    } else g->npos[s] = n;
    if (jump && g->settle < SETTLE_S) g->settle = SETTLE_S;
    if (g->npos[s] < 4) return 0;
    double span = g->tim[s][0] - g->tim[s][3];
    if (span < 0.03 || span > 0.3) return 0;
    float k = (float)(1.0 / span);
    for (int i = 0; i < 3; i++) v[i] = (g->pos[s][0][i] - g->pos[s][3][i]) * k;
    if (!(sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) <= VEL_MAX_MPS)) {   /* medida imposible: pose basura */
        v[0] = v[1] = v[2] = 0; g->npos[s] = 1; if (g->settle < SETTLE_S) g->settle = SETTLE_S;
        return 0;
    }
    return 1;
}

void vrg_update(vrg_t *g, const vrg_in_t *in, vrg_out_t *out) {
    memset(out, 0, sizeof *out);
    const vrg_cfg_t *c = &g->cfg;
    float dt = (fin(in->dt) && in->dt > 0.001f) ? (in->dt > 0.1f ? 0.1f : in->dt) : 0.02f;
    int ok = c->enable && in->active && fin3(in->body) && fin3(in->head) && fin3(in->head_u) && fin(in->yaw);
    if (!ok) {   /* sin control: se suelta todo y se olvida la historia (las manos pueden saltar al recuperar) */
        float wh = g->weapon_hold, lt = g->latch_t; int latch = g->latch;
        vrg_reset(g);
        g->weapon_hold = in->weapon_busy ? 0.8f : (wh > dt ? wh - dt : 0.0f);
        if (latch && lt + dt < 0.5f) { g->latch = latch; g->latch_t = lt + dt; out->mask |= (uint16_t)(VRG_R1 | VRG_L1); }   /* el grip aún cerrado no abre menús */
        return;
    }
    g->clock_s += dt;
    if (g->warm > 0) g->warm--;
    if (g->settle > 0) g->settle -= dt;
    if (g->hurt_cool > 0) g->hurt_cool -= dt;
    if (g->wall_cool > 0) g->wall_cool -= dt;
    /* arma: el juego desequipa el arma un instante al recargar y al cambiar; durante ese tiempo y un poco después no hay manos libres */
    if (in->weapon >= 0 || in->weapon_busy) g->weapon_hold = 0.8f; else if (g->weapon_hold > 0) g->weapon_hold -= dt;
    int no_wpn = in->weapon < 0 && !in->weapon_busy && g->weapon_hold <= 0;
    /* daño recibido */
    if (g->have_life && in->life < g->prev_life && g->hurt_cool <= 0) {
        float d = (float)(g->prev_life - in->life) / 256.0f;
        float s = 0.35f + 2.0f * d; if (s > 1) s = 1;
        out->haptic[0] = out->haptic[1] = s; out->event = VRG_EV_HURT; g->hurt_cool = 0.2f;
    }
    g->prev_life = in->life; g->have_life = 1;
    if (in->on_wall && !g->prev_wall && g->wall_cool <= 0) {
        out->haptic[0] = out->haptic[1] = out->haptic[0] > 0.25f ? out->haptic[0] : 0.25f; if (!out->event) out->event = VRG_EV_WALL;
        g->wall_cool = 0.3f;
    }
    g->prev_wall = in->on_wall;

    float fwd[2] = {in->head_fwd[0], in->head_fwd[1]};
    float fl = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1]);
    int fwd_ok = fin(fl) && fl > 0.5f;           /* casi vertical: el rumbo es ruido */
    if (!(fl > 0.1f) || !fin(fl)) { fwd[0] = 0; fwd[1] = -1; } else { fwd[0] /= fl; fwd[1] /= fl; }
    /* salto de pose de la cabeza (recentrado: cabeza y manos saltan juntas): la velocidad de las manos no vale */
    {
        int pf = g->prev_fwd[0] * g->prev_fwd[0] + g->prev_fwd[1] * g->prev_fwd[1] > 0.5f;
        if (g->have_head) {
            float dx = in->head_u[0] - g->prev_head[0], dy = in->head_u[1] - g->prev_head[1], dz = in->head_u[2] - g->prev_head[2];
            int turned = fwd_ok && pf && (fwd[0] * g->prev_fwd[0] + fwd[1] * g->prev_fwd[1]) < 0.866f;
            if (sqrtf(dx * dx + dy * dy + dz * dz) > 0.15f || turned) {
                g->npos[0] = g->npos[1] = 0; if (g->settle < SETTLE_S) g->settle = SETTLE_S;
            }
        }
        memcpy(g->prev_head, in->head_u, sizeof g->prev_head); g->have_head = 1;
        if (fwd_ok) { g->prev_fwd[0] = fwd[0]; g->prev_fwd[1] = fwd[1]; } else g->prev_fwd[0] = g->prev_fwd[1] = 0;
    }
    float vel[2][3] = {{0}}, spd[2] = {0, 0}, rad[2] = {0, 0}, reach[2] = {0, 0}, fv[2] = {0, 0};
    int tr[2];
    for (int s = 0; s < 2; s++) {
        tr[s] = in->tracked[s] && fin3(in->hand[s]) && fin3(in->hrel[s]) && fin3(in->hand_u[s]);
        if (!tr[s]) { g->npos[s] = 0; continue; }
        double t = (fin(in->hand_time[s]) && in->hand_time[s] > 0) ? in->hand_time[s] : g->clock_s;
        hand_vel(g, s, in->hand_u[s], t, vel[s]);
        spd[s] = sqrtf(vel[s][0] * vel[s][0] + vel[s][1] * vel[s][1] + vel[s][2] * vel[s][2]);
        float rx = in->hrel[s][0], rz = in->hrel[s][2];
        reach[s] = sqrtf(rx * rx + rz * rz);
        fv[s] = vel[s][0] * fwd[0] + vel[s][2] * fwd[1];
        if (reach[s] > 1e-3f) rad[s] = (vel[s][0] * rx + vel[s][2] * rz) / reach[s];
    }
    g->cool -= dt; if (g->cool < -1.0f) g->cool = -1.0f;

    /* enemigo más cercano (horizontal) */
    int ne = -1; float nd = 1e9f;
    for (int i = 0; i < in->n_en && i < VRG_MAX_EN; i++) {
        if (in->en[i].down || !fin3(in->en[i].p)) continue;
        float d = hdist(in->en[i].p, in->body);
        if (d < nd) { nd = d; ne = i; }
    }
    int free_hands = no_wpn && !in->in_box && !in->in_water;

    /* ---------------- pared: golpear y asomarse ---------------- */
    int sq_pulse = 0, circle = 0, triangle = 0;
    if (in->on_wall) {
        /* golpear: solo con las manos libres (con arma VR un gesto rápido del arma no debe hacer ruido) y solo hacia fuera (el retroceso no cuenta) */
        if (c->knock && no_wpn && g->settle <= 0 && g->cool <= 0 && g->knock_left <= 0) {
            for (int s = 0; s < 2; s++)
                if (tr[s] && spd[s] >= c->knock_speed && reach[s] >= 0.15f && rad[s] >= 0.5f * spd[s]) {
                    g->knock_left = 4; g->cool = c->cooldown_s; out->haptic[s] = 0.8f; out->event = VRG_EV_KNOCK; break;
                }
        }
        if (c->peek) {
            /* inclinación lateral de la cabeza respecto a la entrada en la pared; el eje (perpendicular al rumbo de la cabeza al pegarse) queda
             * fijo: girar la cabeza estando pegado no es inclinarse */
            if (!g->peek_ref_ok) {
                g->peek_ax[0] = -fwd[1]; g->peek_ax[1] = fwd[0];
                g->peek_ref = in->head_u[0] * g->peek_ax[0] + in->head_u[2] * g->peek_ax[1]; g->peek_ref_ok = 1;
            }
            float cur = in->head_u[0] * g->peek_ax[0] + in->head_u[2] * g->peek_ax[1];
            float lean = fabsf(cur - g->peek_ref);
            if (g->peek_on) { if (lean < c->peek_off) g->peek_on = 0; }
            else if (lean > c->peek_on && g->settle <= 0) { g->peek_on = 1; out->haptic[0] = out->haptic[1] = 0.15f; if (!out->event) out->event = VRG_EV_PEEK; }
        }
    } else { g->peek_on = 0; g->peek_ref_ok = 0; g->knock_left = 0; }
    if (g->knock_left > 0) { circle = 1; g->knock_left--; }
    if (g->peek_on) triangle = 1;

    /* ---------------- golpe ---------------- */
    if (c->punch && free_hands && !in->on_wall && in->stance < 2 && g->grab_side < 0 && !in->holding && g->settle <= 0) {
        if (g->punch_left <= 0 && g->cool <= 0) {
            for (int s = 0; s < 2; s++) {
                if (!tr[s]) continue;
                if (spd[s] >= c->punch_speed && rad[s] >= 0.7f * spd[s] && fv[s] >= 0.6f * spd[s] && reach[s] >= c->reach_min && (c->melee_range <= 0 || (ne >= 0 && nd <= c->melee_range))) {
                    g->punch_left = 4; g->cool = c->cooldown_s; out->haptic[s] = 0.6f; out->event = VRG_EV_PUNCH; break;
                }
            }
        }
    }
    if (g->punch_left > 0) { sq_pulse = 1; g->punch_left--; }

    /* ---------------- agarre: grip cerrado == cuadrado ---------------- */
    int cl[2] = {0, 0};
    for (int s = 0; s < 2; s++) {
        if (!tr[s]) continue;
        float gv = fin(in->grip[s]) ? in->grip[s] : 0;
        int was = (g->grab_side == s) || (g->prev_grip_closed & (1 << s));
        cl[s] = was ? gv > c->grip_off : gv > c->grip_on;
    }
    int want_sq = 0;
    if (c->grab && free_hands && !in->on_wall) {
        if (g->grab_side >= 0) {                 /* ya sujetando: sigue la mano que agarró (o la otra si hay ambas) */
            int s = g->grab_side;
            if (cl[s] || cl[1 - s]) { want_sq = 1; if (!cl[s]) g->grab_side = 1 - s; }
            else { g->grab_side = -1; }
        } else if (ne >= 0 && nd <= c->grab_range && g->warm == 0) {
            for (int s = 0; s < 2 && g->grab_side < 0; s++) {
                if (!cl[s] || (g->prev_grip_closed & (1 << s))) continue;   /* hay que cerrar la mano ya cerca (flanco) */
                float nk[3] = {in->en[ne].p[0], in->en[ne].p[1] + c->neck_height, in->en[ne].p[2]};
                float dx = in->hand[s][0] - nk[0], dy = in->hand[s][1] - nk[1], dz = in->hand[s][2] - nk[2];
                /* sujetando ya al enemigo, apretar de nuevo no exige tener la mano en su cuello (el brazo del personaje la lleva) */
                if (in->holding || sqrtf(dx * dx + dy * dy + dz * dz) <= c->neck_radius + 0.2f) { g->grab_side = s; want_sq = 1; }
            }
        }
    } else if (g->grab_side >= 0) g->grab_side = -1;
    g->prev_grip_closed = (cl[0] ? 1 : 0) | (cl[1] ? 2 : 0);   /* siempre el estado real: un grip ya cerrado al quedar libres las manos no es un flanco */
    /* mínimos de 3 VBlanks pulsado y soltado (el mando se muestrea cada 2) */
    g->sq_age++;
    int cur = g->sq_hold;
    if (want_sq != cur && g->sq_age >= 3) {
        g->sq_hold = want_sq; g->sq_age = 0;
        if (want_sq) {
            out->haptic[g->grab_side >= 0 ? g->grab_side : 0] = 0.9f; out->event = in->holding ? VRG_EV_SQUEEZE : VRG_EV_GRAB;
            if (in->holding) out->haptic[g->grab_side >= 0 ? g->grab_side : 0] = 0.5f;
        } else { out->event = VRG_EV_RELEASE; out->haptic[0] = out->haptic[1] = 0.2f; }
    }
    if (g->sq_hold) sq_pulse = 1;
    /* R1/L1 (el grip) fuera del mando del juego mientras agarra y, tras soltar el enemigo (o perder el control del agarre: arma, seguimiento),
     * hasta que se abra el grip: si no, el grip aún cerrado abriría la ventana de objetos. Sin seguimiento no se puede saber: 0,5 s como máximo. */
    if (g->grab_side >= 0 || g->sq_hold) {
        g->latch = cl[0] ? 1 : 0; g->latch |= cl[1] ? 2 : 0; if (g->grab_side >= 0) g->latch |= 1 << g->grab_side; g->latch_t = 0;
        out->mask |= (uint16_t)(VRG_R1 | VRG_L1);
    } else if (g->latch) {
        g->latch_t += dt;
        for (int s = 0; s < 2; s++) {
            if (!(g->latch & (1 << s))) continue;
            int keep = tr[s] ? (fin(in->grip[s]) && in->grip[s] > c->grip_off) : g->latch_t < 0.5f;
            if (!keep) g->latch &= ~(1 << s);
        }
        if (g->latch) out->mask |= (uint16_t)(VRG_R1 | VRG_L1);
    }

    if (sq_pulse) out->press |= VRG_SQUARE;
    if (circle) out->press |= VRG_CIRCLE;
    if (triangle) out->press |= VRG_TRIANGLE;
}
