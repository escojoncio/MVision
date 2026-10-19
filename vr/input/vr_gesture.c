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
    g->cfg = c; g->grab_side = -1;
}
void vrg_init(vrg_t *g) { memset(g, 0, sizeof *g); vrg_cfg_defaults(&g->cfg); g->grab_side = -1; }

static float hdist(const float *a, const float *b) { float dx = a[0] - b[0], dz = a[2] - b[2]; return sqrtf(dx * dx + dz * dz); }

/* velocidad de la mano (m/s) a partir de las últimas muestras; devuelve 0 si aún no hay historia */
static int hand_vel(vrg_t *g, int s, const float p[3], float dt, float v[3]) {
    int n = g->npos[s];
    for (int k = (n < 3 ? n : 3); k > 0; k--) memcpy(g->pos[s][k], g->pos[s][k - 1], sizeof g->pos[s][0]);
    memcpy(g->pos[s][0], p, 3 * sizeof(float));
    if (n < 4) g->npos[s] = n + 1;
    if (n < 3) { v[0] = v[1] = v[2] = 0; return 0; }
    float k = 1.0f / (3.0f * dt);
    for (int i = 0; i < 3; i++) v[i] = (g->pos[s][0][i] - g->pos[s][3][i]) * k;
    return 1;
}

void vrg_update(vrg_t *g, const vrg_in_t *in, vrg_out_t *out) {
    memset(out, 0, sizeof *out);
    const vrg_cfg_t *c = &g->cfg;
    float dt = (fin(in->dt) && in->dt > 0.001f) ? (in->dt > 0.1f ? 0.1f : in->dt) : 0.02f;
    int ok = c->enable && in->active && fin3(in->body) && fin3(in->head) && fin(in->yaw);
    if (!ok) {   /* sin control: se suelta todo y se olvida la historia (las manos pueden saltar al recuperar) */
        int sq = g->sq_hold;
        vrg_reset(g);
        (void)sq;
        return;
    }
    /* daño recibido */
    if (g->have_life && in->life < g->prev_life) {
        float d = (float)(g->prev_life - in->life) / 256.0f;
        float s = 0.35f + 2.0f * d; if (s > 1) s = 1;
        out->haptic[0] = out->haptic[1] = s; out->event = VRG_EV_HURT;
    }
    g->prev_life = in->life; g->have_life = 1;
    if (in->on_wall && !g->prev_wall) { out->haptic[0] = out->haptic[1] = out->haptic[0] > 0.25f ? out->haptic[0] : 0.25f; if (!out->event) out->event = VRG_EV_WALL; }
    g->prev_wall = in->on_wall;

    float vy = fin(in->view_yaw) ? in->view_yaw : in->yaw;
    float fwd[2] = {sinf(vy), cosf(vy)};
    float vel[2][3] = {{0}}, spd[2] = {0, 0}, rad[2] = {0, 0}, reach[2] = {0, 0}, fv[2] = {0, 0};
    int tr[2];
    for (int s = 0; s < 2; s++) {
        tr[s] = in->tracked[s] && fin3(in->hand[s]);
        if (!tr[s]) { g->npos[s] = 0; continue; }
        hand_vel(g, s, in->hand[s], dt, vel[s]);
        spd[s] = sqrtf(vel[s][0] * vel[s][0] + vel[s][1] * vel[s][1] + vel[s][2] * vel[s][2]);
        float rx = in->hand[s][0] - in->head[0], rz = in->hand[s][2] - in->head[2];
        reach[s] = sqrtf(rx * rx + rz * rz);
        fv[s] = vel[s][0] * fwd[0] + vel[s][2] * fwd[1];
        if (reach[s] > 1e-3f) rad[s] = (vel[s][0] * rx + vel[s][2] * rz) / reach[s];
    }
    g->cool -= dt;
    float up_dummy = 0; (void)up_dummy;

    /* enemigo más cercano (horizontal) */
    int ne = -1; float nd = 1e9f;
    for (int i = 0; i < in->n_en && i < VRG_MAX_EN; i++) {
        if (in->en[i].down || !fin3(in->en[i].p)) continue;
        float d = hdist(in->en[i].p, in->body);
        if (d < nd) { nd = d; ne = i; }
    }
    int free_hands = in->weapon < 0 && !in->in_box && !in->in_water;

    /* ---------------- pared: golpear y asomarse ---------------- */
    int sq_pulse = 0, circle = 0, triangle = 0;
    if (in->on_wall) {
        if (c->knock && g->cool <= 0 && g->knock_left <= 0) {
            for (int s = 0; s < 2; s++)
                if (tr[s] && spd[s] >= c->knock_speed && reach[s] >= 0.15f && fabsf(rad[s]) >= 0.5f * spd[s]) {
                    g->knock_left = 4; g->cool = c->cooldown_s; out->haptic[s] = 0.8f; out->event = VRG_EV_KNOCK; break;
                }
        }
        if (c->peek) {
            /* inclinación lateral de la cabeza respecto a la entrada en la pared, eje = perpendicular al rumbo */
            float rx = cosf(vy), rz = -sinf(vy);
            float cur = (in->head[0] - in->body[0]) * rx + (in->head[2] - in->body[2]) * rz;
            if (!g->peek_ref_ok) { g->peek_ref[0] = cur; g->peek_ref_ok = 1; }
            float lean = fabsf(cur - g->peek_ref[0]);
            if (g->peek_on) { if (lean < c->peek_off) g->peek_on = 0; }
            else if (lean > c->peek_on) { g->peek_on = 1; out->haptic[0] = out->haptic[1] = 0.15f; if (!out->event) out->event = VRG_EV_PEEK; }
        }
    } else { g->peek_on = 0; g->peek_ref_ok = 0; g->knock_left = 0; }
    if (g->knock_left > 0) { circle = 1; g->knock_left--; }
    if (g->peek_on) triangle = 1;

    /* ---------------- golpe ---------------- */
    if (c->punch && free_hands && !in->on_wall && in->stance < 2 && g->grab_side < 0 && !in->holding) {
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
    int want_sq = 0;
    if (c->grab && free_hands && !in->on_wall) {
        int closed_any = 0;
        int cl[2] = {0, 0};
        for (int s = 0; s < 2; s++) {
            if (!tr[s]) continue;
            float gv = fin(in->grip[s]) ? in->grip[s] : 0;
            int was = (g->grab_side == s) || (g->prev_grip_closed & (1 << s));
            cl[s] = was ? gv > c->grip_off : gv > c->grip_on;
        }
        if (g->grab_side >= 0) {                 /* ya sujetando: sigue la mano que agarró (o la otra si hay ambas) */
            int s = g->grab_side;
            if (cl[s] || cl[1 - s]) { want_sq = 1; closed_any = 1; if (!cl[s]) g->grab_side = 1 - s; }
            else { g->grab_side = -1; }
        } else if (ne >= 0 && nd <= c->grab_range) {
            for (int s = 0; s < 2 && g->grab_side < 0; s++) {
                if (!cl[s] || (g->prev_grip_closed & (1 << s))) continue;   /* hay que cerrar la mano ya cerca (flanco) */
                float nk[3] = {in->en[ne].p[0], in->en[ne].p[1] + c->neck_height, in->en[ne].p[2]};
                float dx = in->hand[s][0] - nk[0], dy = in->hand[s][1] - nk[1], dz = in->hand[s][2] - nk[2];
                if (sqrtf(dx * dx + dy * dy + dz * dz) <= c->neck_radius + 0.2f) { g->grab_side = s; want_sq = 1; closed_any = 1; }
            }
        }
        (void)closed_any;
        g->prev_grip_closed = (cl[0] ? 1 : 0) | (cl[1] ? 2 : 0);
    } else {
        if (g->grab_side >= 0) g->grab_side = -1;
        g->prev_grip_closed = 0;
    }
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
    if (g->grab_side >= 0 || g->sq_hold) out->mask |= (uint16_t)(VRG_R1 | VRG_L1);

    if (sq_pulse) out->press |= VRG_SQUARE;
    if (circle) out->press |= VRG_CIRCLE;
    if (triangle) out->press |= VRG_TRIANGLE;
}
