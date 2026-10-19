#include "vr_gesture.h"
#include <math.h>
#include <string.h>

static int fin(float v) { return isfinite(v); }
static int fin3(const float *v) { return fin(v[0]) && fin(v[1]) && fin(v[2]); }

void vrg_cfg_defaults(vrg_cfg_t *c) {
    memset(c, 0, sizeof *c);
    c->enable = 1; c->fist_required = 1; c->fist_haptic = 0.12f; c->item_hold_s = 0.35f; c->punch_dir_cos = 0.5f; c->trig_on = 0.6f; c->trig_off = 0.3f; c->punch = c->grab = c->knock = c->peek = 1;
    c->right_handed = 1;
    c->punch_speed = 1.3f; c->knock_speed = 0.4f; c->knock_touch = 0.08f; c->knock_cooldown_s = 0.25f; c->knock_enemy_m = 1.0f; c->reach_min = 0.30f; c->melee_range = 3.0f; c->front_cos = 0.5f;
    c->grab_range = 1.3f; c->neck_radius = 0.30f; c->neck_height = 0.45f; c->back_dist = 0.25f; c->grab_after_punch_s = 0.3f;
    c->chest_down = 0.37f; c->chest_side = 0.15f; c->chest_fwd = 0.12f; c->chest_radius = 0.22f;
    c->grip_on = 0.65f; c->grip_off = 0.35f; c->peek_on = 0.12f; c->peek_off = 0.06f; c->cooldown_s = 0.2f;
}
void vrg_reset(vrg_t *g) {
    vrg_cfg_t c = g->cfg;
    memset(g, 0, sizeof *g);
    g->cfg = c; g->grab_side = -1; g->warm = 2; g->since_punch = 10.0f;
}
void vrg_init(vrg_t *g) { memset(g, 0, sizeof *g); vrg_cfg_defaults(&g->cfg); g->grab_side = -1; g->warm = 2; g->since_punch = 10.0f; }

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
    memset(out, 0, sizeof *out); out->punch_side = -1; out->knock_side = -1; out->knock_try_side = -1; out->grab_side_try = -1; out->grab_side = -1; out->grab_en = -1;
    const vrg_cfg_t *c = &g->cfg;
    float dt = (fin(in->dt) && in->dt > 0.001f) ? (in->dt > 0.1f ? 0.1f : in->dt) : 0.02f;
    int ok = c->enable && in->active && fin3(in->body) && fin3(in->head) && fin3(in->head_u) && fin(in->yaw);
    if (!ok) {   /* sin control: se suelta todo y se olvida la historia (las manos pueden saltar al recuperar) */
        float wh = g->weapon_hold;
        vrg_reset(g);
        g->weapon_hold = in->weapon_busy ? 0.8f : (wh > dt ? wh - dt : 0.0f);
        if (c->enable && (c->punch || c->grab) && (in->tracked[0] || in->tracked[1])) out->mask |= (uint16_t)(VRG_R1 | VRG_L1);   /* R1/L1 nunca llegan al juego */
        return;
    }
    g->clock_s += dt;
    if (g->warm > 0) g->warm--;
    if (g->settle > 0) g->settle -= dt;
    if (g->hurt_cool > 0) g->hurt_cool -= dt;
    for (int s = 0; s < 2; s++) { if (g->fist_hap_cool[s] > 0) g->fist_hap_cool[s] -= dt; }
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
        /* La velocidad sale de la posición de la mano RELATIVA A LA CABEZA y expresada en el marco de la cabeza (x = a la derecha, y = arriba, z = delante): ni andar, ni mover la cabeza,
         * ni girar el cuerpo (la mano orbita la cabeza sin avanzar) cuentan como movimiento de la mano. */
        float rx = in->hrel[s][0], rz = in->hrel[s][2];
        float hf[3] = {rx * (-fwd[1]) + rz * fwd[0], in->hrel[s][1], rx * fwd[0] + rz * fwd[1]};
        hand_vel(g, s, hf, t, vel[s]);
        spd[s] = sqrtf(vel[s][0] * vel[s][0] + vel[s][1] * vel[s][1] + vel[s][2] * vel[s][2]);
        reach[s] = sqrtf(hf[0] * hf[0] + hf[2] * hf[2]);
        fv[s] = vel[s][2];
        if (reach[s] > 1e-3f) rad[s] = (vel[s][0] * hf[0] + vel[s][2] * hf[2]) / reach[s];
    }
    g->cool -= dt; if (g->cool < -1.0f) g->cool = -1.0f;
    g->gest_tokens += 10.0f * dt; if (g->gest_tokens > 2.0f) g->gest_tokens = 2.0f;

    /* enemigo más cercano (horizontal) */
    int ne = -1; float nd = 1e9f;
    for (int i = 0; i < in->n_en && i < VRG_MAX_EN; i++) {
        if (in->en[i].down || !fin3(in->en[i].p)) continue;
        float d = hdist(in->en[i].p, in->body);
        if (d < nd) { nd = d; ne = i; }
    }
    /* enemigo más cercano DELANTE (ángulo con la mirada <= acos(front_cos); la mirada = el rumbo del jugador, que sigue a la cabeza) para el golpe */
    int nf = -1; float ndf = 1e9f;
    for (int i = 0; i < in->n_en && i < VRG_MAX_EN; i++) {
        if (in->en[i].down || !fin3(in->en[i].p)) continue;
        float dx = in->en[i].p[0] - in->body[0], dz = in->en[i].p[2] - in->body[2], d = sqrtf(dx * dx + dz * dz);
        if (d < 0.05f) { if (d < ndf) { ndf = d; nf = i; } continue; }       /* encima de nosotros: cuenta */
        if ((dx * sinf(in->yaw) + dz * cosf(in->yaw)) / d >= c->front_cos && d < ndf) { ndf = d; nf = i; }
    }
    int free_hands = no_wpn && !in->in_box && !in->in_water;

    /* ---------------- pared: golpear y asomarse ---------------- */
    int circle = 0, triangle = 0;

    /* ---------------- llamar con los nudillos: la mano (abierta o en puño) TOCA la pared moviéndose hacia ella ---------------- */
    out->knock_side = -1;
    {
        for (int s = 0; s < 2; s++) {
            if (g->knock_cool[s] > 0) g->knock_cool[s] -= dt;
            if (!tr[s] || !in->wall_valid[s] || !fin(in->wall_dist[s])) { g->kn_n[s] = 0; g->kn_toward[s] = 0; continue; }
            double t = (fin(in->hand_time[s]) && in->hand_time[s] > 0) ? in->hand_time[s] : g->clock_s;
            int n = g->kn_n[s];
            if (n > 0 && t - g->kn_t[s][0] < 0.004) continue;                 /* misma muestra */
            if (n > 0 && (t < g->kn_t[s][0] || t - g->kn_t[s][0] > 0.25)) n = 0;
            for (int k = (n < 2 ? n : 2); k > 0; k--) { g->kn_d[s][k] = g->kn_d[s][k - 1]; g->kn_t[s][k] = g->kn_t[s][k - 1]; }
            float dprev = n > 0 ? g->kn_d[s][0] : in->wall_dist[s];
            g->kn_d[s][0] = in->wall_dist[s]; g->kn_t[s][0] = t; g->kn_n[s] = n < 3 ? n + 1 : 3; n = g->kn_n[s];
            /* velocidad de acercamiento: la mayor de los últimos intervalos (la distancia a la superficie no depende del marco ni de andar) */
            float ap = 0;
            for (int k = 0; k + 1 < n; k++) {
                double dts = g->kn_t[s][k] - g->kn_t[s][k + 1];
                if (dts < 0.008) continue;
                float r = (float)((g->kn_d[s][k + 1] - g->kn_d[s][k]) / dts);
                if (r > ap) ap = r;
            }
            float d = in->wall_dist[s];
            int touch = d <= c->knock_touch && dprev > c->knock_touch;
            if (ap >= c->knock_speed && d < 0.35f && d > c->knock_touch) g->kn_toward[s] = 1;
            const char *why = NULL;
            if (touch) {
                int enemy_near = 0;
                for (int i = 0; i < in->n_en && i < VRG_MAX_EN; i++) {
                    if (in->en[i].down || !fin3(in->en[i].p)) continue;
                    float dx = in->hand[s][0] - in->en[i].p[0], dy = in->hand[s][1] - in->en[i].p[1], dz = in->hand[s][2] - in->en[i].p[2];
                    if (sqrtf(dx * dx + dy * dy + dz * dz) < c->knock_enemy_m) enemy_near = 1;
                }
                if (!c->knock) why = "desactivado";
                else if (!in->on_wall) why = "no_pegado";
                else if (!free_hands) why = "manos_ocupadas";
                else if (enemy_near) why = "enemigo";
                else if (g->settle > 0) why = "calma";
                else if (ap < c->knock_speed) why = "lento";
                else if (g->knock_cool[s] > 0 || g->cool_knock > 0) why = "enfriamiento";
                else why = "ok";
                if (!strcmp(why, "ok") && out->knock_side < 0) {
                    out->knock_side = s; g->knock_cool[s] = c->knock_cooldown_s; g->cool_knock = c->knock_cooldown_s;
                    out->haptic[s] = 0.6f; if (!out->event) out->event = VRG_EV_KNOCK;
                }
                out->knock_why = why; out->knock_speed = ap; out->knock_dist = d; out->knock_try_side = s;
                g->kn_toward[s] = 0;
            } else if (g->kn_toward[s] && ap < 0.1f) {
                /* se acercó rápido pero se detuvo sin llegar a la pared */
                g->kn_toward[s] = 0;
                if (in->on_wall && c->knock) { out->knock_why = "lejos_de_la_pared"; out->knock_speed = ap; out->knock_dist = d; out->knock_try_side = s; }
            } else if (d > 0.5f) g->kn_toward[s] = 0;
        }
        if (g->cool_knock > 0) g->cool_knock -= dt;
    }
    if (in->on_wall) {
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
    } else { g->peek_on = 0; g->peek_ref_ok = 0; }
    if (g->peek_on) triangle = 1;

    /* ---------------- impactos: háptico fuerte al llegar el golpe (contacto de la mano con el enemigo o descenso de su vida) ---------------- */
    int vit_drop = 0;
    for (int i = 0; i < in->n_en && i < VRG_MAX_EN; i++) if (i < g->vit_n && in->en[i].vital < g->vit_prev[i]) vit_drop = 1;
    g->vit_n = in->n_en < VRG_MAX_EN ? in->n_en : VRG_MAX_EN;
    for (int i = 0; i < g->vit_n; i++) g->vit_prev[i] = in->en[i].vital;
    if (g->impact_t > 0) {
        g->impact_t -= dt;
        int s = g->impact_side, contact = 0;
        if (tr[s])
            for (int i = 0; i < in->n_en && i < VRG_MAX_EN && !contact; i++) {
                if (in->en[i].down || !fin3(in->en[i].p)) continue;
                float dx = in->hand[s][0] - in->en[i].p[0], dy = in->hand[s][1] - (in->en[i].p[1] + 0.3f), dz = in->hand[s][2] - in->en[i].p[2];
                if (sqrtf(dx * dx + dy * dy + dz * dz) <= 0.55f) contact = 1;
            }
        if (contact || vit_drop) {
            out->haptic[s] = 0.95f; g->impact_t = 0; if (!out->event) out->event = VRG_EV_IMPACT;
        }
    }

    /* ---------------- puño: botones R1 (mano dominante) / L1 (la otra) = estado de puño; el grip analógico solo es la pose ---------------- */
    int fst[2] = {0, 0}, edge[2] = {0, 0}, near_neck_prev = 0;
    (void)near_neck_prev;
    for (int s = 0; s < 2; s++) {
        if (!tr[s]) continue;
        int was = (g->fist >> s) & 1;
        fst[s] = in->fist_btn[s] ? 1 : 0;
        edge[s] = fst[s] && !was && g->warm == 0;       /* un botón ya mantenido al recuperar el control NO es una pulsación */
        if (edge[s] && free_hands && c->fist_haptic > 0 && g->fist_hap_cool[s] <= 0) {
            g->fist_hap_cool[s] = 0.6f;
            if (out->haptic[s] < c->fist_haptic) out->haptic[s] = c->fist_haptic;
        }
    }
    g->fist = (fst[0] ? 1 : 0) | (fst[1] ? 2 : 0);
    if (g->since_punch < 10.0f) g->since_punch += dt;
    /* R1/L1 NUNCA llegan al juego con las manos rastreadas: su función de equipar rápido desaparece (son el puño y el agarre) */
    int fist_guard = (c->punch || c->grab) && (tr[0] || tr[1]);
    int grabbing = g->grab_side >= 0 || in->holding;

    /* ---------------- golpe: gesto físico CON EL PUÑO cerrado (R1/L1 mantenido); estando agarrado = apretar/rematar ---------------- */
    if (c->punch) {
        int busy = !(free_hands && !in->on_wall) ? 1 : 0;
        for (int s = 0; s < 2; s++) {
            if (!tr[s]) continue;
            int near_swing = spd[s] >= (g->swing[s] ? 0.4f : 0.6f) * c->punch_speed;
            g->swing[s] = near_swing;
            float fcos = spd[s] > 1e-3f ? fv[s] / spd[s] : 0.0f, rcos = spd[s] > 1e-3f ? rad[s] / spd[s] : 0.0f;
            const char *why = "ok";
            int dir_ok = fcos >= c->punch_dir_cos && rcos >= c->punch_dir_cos;
            if (busy && !grabbing) why = "manos_ocupadas";
            else if (in->stance >= 2) why = "ocupado";
            else if (g->settle > 0) why = "calma";
            else if (g->cool > 0) why = "enfriamiento";
            else if (c->fist_required && (!fst[s] || ((g->menu_hold >> s) & 1))) why = "sin_puno";
            else if (spd[s] < c->punch_speed) why = "lento";
            else if (!dir_ok) why = "dir";
            else if (reach[s] < c->reach_min) why = "alcance";
            else if (!grabbing && c->melee_range > 0 && ne < 0) why = "sin_enemigo";
            else if (!grabbing && c->melee_range > 0 && nd > c->melee_range) why = "lejos";
            else if (!grabbing && c->melee_range > 0 && nf < 0) why = "detras";
            int fire = !strcmp(why, "ok");
            vrg_gest_t *pk = &g->gpk[s];
            if (near_swing && !(busy && !grabbing)) {
                if (!pk->valid || spd[s] >= pk->speed || fire) {
                    pk->valid = 1; pk->side = s; pk->speed = spd[s]; pk->fcos = fcos; pk->rcos = rcos; pk->reach = reach[s]; pk->dist = ne >= 0 ? nd : -1.0f;
                    pk->grip = fin(in->grip[s]) ? in->grip[s] : 0.0f; pk->fist = fst[s]; pk->reason = why;
                }
                if (fire && !out->gest.valid) { out->gest = *pk; pk->valid = 0; }
            } else if (pk->valid && !out->gest.valid) {
                if (g->gest_tokens >= 1.0f) { g->gest_tokens -= 1.0f; out->gest = *pk; }
                pk->valid = 0;
            }
            if (fire) {
                g->cool = c->cooldown_s;
                if (grabbing) { out->squeeze = 1; out->haptic[s] = 0.5f; out->event = VRG_EV_SQUEEZE; }
                else {
                    out->punch_side = s; out->haptic[s] = 0.25f; out->event = VRG_EV_PUNCH;
                    g->impact_t = 0.6f; g->impact_side = s; g->since_punch = 0;
                }
                break;
            }
        }
    }

    /* ---------------- agarre: PULSAR R1/L1 (flanco) con la mano abierta junto al cuello/espalda de un enemigo; con el puño ya cerrado NUNCA ---------------- */
    int dom = c->right_handed ? 1 : 0;
    (void)dom;
    float hd[2] = {-1, -1};     /* distancia de cada mano al cuello/espalda del enemigo más cercano */
    if (ne >= 0) {
        const float *ep = in->en[ne].p;
        float nk[3] = {ep[0], ep[1] + c->neck_height, ep[2]};
        float bk[3] = {ep[0] - sinf(in->en[ne].yaw) * c->back_dist, ep[1] + 0.2f, ep[2] - cosf(in->en[ne].yaw) * c->back_dist};
        for (int s = 0; s < 2; s++) {
            if (!tr[s]) continue;
            const float *h = in->hand[s];
            float d1 = sqrtf((h[0] - nk[0]) * (h[0] - nk[0]) + (h[1] - nk[1]) * (h[1] - nk[1]) + (h[2] - nk[2]) * (h[2] - nk[2]));
            float d2 = sqrtf((h[0] - bk[0]) * (h[0] - bk[0]) + (h[1] - bk[1]) * (h[1] - bk[1]) + (h[2] - bk[2]) * (h[2] - bk[2]));
            hd[s] = d1 < d2 ? d1 : d2;
        }
    }
    /* zona del pecho de cada mano (ventana de objetos = izquierda, armas = derecha): se calcula con la cabeza y el rumbo del torso del
     * personaje (el cuerpo del jugador), no con coordenadas fijas del espacio de seguimiento: vale sentado, de pie o girado */
    int chest[2] = {0, 0};
    {
        float fx = sinf(in->yaw), fz = cosf(in->yaw), lx = fz, lz = -fx;     /* delante y izquierda del torso */
        for (int s = 0; s < 2; s++) {
            if (!tr[s]) { g->chest_prev[s] = 0; continue; }
            float sd = s == 0 ? 1.0f : -1.0f;
            float ctr[3] = {in->head[0] + fx * c->chest_fwd + lx * c->chest_side * sd, in->head[1] - c->chest_down, in->head[2] + fz * c->chest_fwd + lz * c->chest_side * sd};
            float dx = in->hand[s][0] - ctr[0], dy = in->hand[s][1] - ctr[1], dz = in->hand[s][2] - ctr[2];
            chest[s] = sqrtf(dx * dx + dy * dy + dz * dz) <= c->chest_radius;
            if (chest[s] && !g->chest_prev[s] && !fst[s] && g->warm == 0 && g->chest_cool[s] <= 0) { g->chest_cool[s] = 0.5f; if (out->haptic[s] < 0.15f) out->haptic[s] = 0.15f; }   /* aviso al entrar en la zona */
            g->chest_prev[s] = chest[s];
            if (g->chest_cool[s] > 0) g->chest_cool[s] -= dt;
        }
    }
    int grab_start = 0;
    if (c->grab) {
        if (g->grab_side >= 0) {
            /* sujetando: mientras algún botón siga pulsado se mantiene; soltar todos = soltar (el motor lo deja caer a los 9 fotogramas) */
            if (fst[g->grab_side] || fst[1 - g->grab_side]) out->grab_hold = 1;
            else { g->grab_side = -1; out->event = out->event ? out->event : VRG_EV_RELEASE; out->haptic[0] = out->haptic[1] = 0.2f; }
        }
        for (int s = 0; s < 2; s++) {
            if (!tr[s]) continue;
            int near_now = hd[s] >= 0 && hd[s] <= c->neck_radius;
            int started = 0;
            if (edge[s] && !grabbing && !grab_start) {
                /* PRIORIDAD al pulsar: 1) cuello/espalda de un enemigo = agarre; 2) zona del pecho = ventana; 3) puño (no hace falta nada más) */
                if (ne >= 0 && nd <= 3.0f) {
                    const char *why = "ok";
                    if (!free_hands || in->on_wall || in->stance >= 2) why = "manos_ocupadas";
                    else if (g->since_punch < c->grab_after_punch_s) why = "tras_golpe";
                    else if (g->settle > 0) why = "calma";
                    else if (nd > c->grab_range) why = "cuerpo_lejos";
                    else if (!near_now) why = "mano_lejos";
                    out->grab_why = why; out->grab_dist = hd[s]; out->grab_side_try = s;
                    if (!strcmp(why, "ok")) {
                        grab_start = started = 1; g->grab_side = s; g->grab_en = ne;
                        out->grab_start = 1; out->grab_hold = 1; out->event = VRG_EV_GRAB; out->haptic[s] = 0.9f;
                        g->impact_t = 0;
                    }
                }
                if (!started && chest[s]) { g->menu_hold |= 1 << s; out->menu_open[s] = 1; out->haptic[s] = 0.2f; if (!out->event) out->event = VRG_EV_MENU; }
            } else if (edge[s] && grabbing && !grab_start) {
                /* ya agarrando: cada pulsación nueva aprieta (y remata tras el décimo apretón) */
                out->squeeze = 1; out->grab_hold = 1; out->event = VRG_EV_SQUEEZE; out->haptic[s] = 0.5f;
                if (g->grab_side < 0 && in->holding) g->grab_side = s;
            } else if (fst[s] && !edge[s] && near_now && !g->near_prev[s] && ne >= 0 && !grabbing) {
                out->grab_why = "puno_cerrado"; out->grab_dist = hd[s]; out->grab_side_try = s;   /* regla dura: botón ya mantenido = nunca agarra */
            }
            g->near_prev[s] = near_now;
        }
        if (in->holding && !out->grab_hold && (fst[0] || fst[1])) out->grab_hold = 1;
        if (g->grab_side >= 0) { out->grab_side = g->grab_side; out->grab_en = g->grab_en; }
    } else g->grab_side = -1;
    if (g->grab_side >= 0 && ne >= 0) g->grab_en = ne;
    for (int s = 0; s < 2; s++) {      /* ventana abierta: se mantiene mientras se mantenga el botón (R1 = armas, L1 = objetos del motor) */
        if (!((g->menu_hold >> s) & 1)) continue;
        if (tr[s] && fst[s]) { out->menu_open[s] = 1; out->press |= (uint16_t)(s == (c->right_handed ? 1 : 0) ? VRG_R1 : VRG_L1);   /* mano dominante = R1 (armas), la otra = L1 (objetos) */ }
        else g->menu_hold &= ~(1 << s);
    }
    if (fist_guard) out->mask |= (uint16_t)(VRG_R1 | VRG_L1);

    /* ventana de objetos (R1) / armas (L1): con las manos libres los grips no las abren; mantener el clic del stick de cada mano sí */
    {
        int st[2] = {in->stick_dom, in->stick_off};
        /* los dos clics a la vez (mantenidos 1 s = gesto de volcado de otra rama) no abren ninguna ventana */
        if (st[0] && st[1]) st[0] = st[1] = 0;
        for (int k = 0; k < 2; k++) {
            if (st[k] && free_hands) g->item_t[k] += dt; else g->item_t[k] = 0;
            if (g->item_t[k] >= c->item_hold_s) out->press |= (uint16_t)(k == 0 ? VRG_R1 : VRG_L1);
        }
    }
    if (circle) out->press |= VRG_CIRCLE;
    if (triangle) out->press |= VRG_TRIANGLE;
}
