/* Lógica común de armas de cargador (fusil y francotirador): disparo y cerrojo. Ver w_gunc.h. */
#include <math.h>
#include <string.h>
#include "w_gunc.h"

#define SND(e)  do { if (br && br->sound) br->sound(wid, (e)); } while (0)
#define HAP(h, k, t) do { if (br && br->haptic) br->haptic((h), (k), (t)); } while (0)

/* ------------------------------------------------------------ objetos sueltos */
static void loose_spawn(GunLoose *l, WPose p, WVec3 v, WVec3 w, float life) {
    l->active = 1; l->pose = p; l->v = v; l->w = w; l->life = life; l->rested = 0;
}
static void loose_step(GunLoose *l, float dt, float floor_y) {
    if (!l->active) return;
    l->life -= dt;
    if (l->life <= 0) { l->active = 0; return; }
    if (l->rested) return;
    l->v.y -= 9.81f * dt;
    l->pose.p = wv_add(l->pose.p, wv_scale(l->v, dt));
    l->pose.q = wquat_normalize(wquat_mul(wquat_from_rotvec(wv_scale(l->w, dt)), l->pose.q));
    if (l->pose.p.y < floor_y && l->v.y < 0) {
        l->pose.p.y = floor_y;
        l->v.y = -l->v.y * 0.35f;
        l->v.x *= 0.6f; l->v.z *= 0.6f; l->w = wv_scale(l->w, 0.6f);
        if (fabsf(l->v.y) < 0.4f) { l->v = wv(0, 0, 0); l->w = wv(0, 0, 0); l->rested = 1; }
    }
}

static void eject_casing(GunState *g, const GunCfg *c, WPose wo) {
    GunLoose *l = &g->casing[g->casing_next++ % GUN_MAX_CASINGS];
    WVec3 v = c->eject_vel;
    v.x *= 1.0f + 0.25f * wcore_rand(&g->seed);
    v.y *= 1.0f + 0.30f * wcore_rand(&g->seed);
    v.z += 0.5f * wcore_rand(&g->seed);
    WPose p; p.p = wpose_apply(wo, c->eject_pos); p.q = wo.q;
    WVec3 w = wv(12.0f * wcore_rand(&g->seed), 10.0f * wcore_rand(&g->seed), 25.0f * wcore_rand(&g->seed));
    loose_spawn(l, p, wquat_rotate(wo.q, v), wquat_rotate(wo.q, w), 1.6f);
}

/* ------------------------------------------------------------ API */
void gun_init(GunState *g, const WeaponDef *d, const GunCfg *c) {
    wcore_state_reset(&g->base, d);
    g->seed = 0x1234567u;
    g->semi = c->start_semi;
    g->equip_latch = 1;
}

void gun_make_ready(WeaponState *ws, int mag_rounds) {
    GunState *g = (GunState *)ws;
    ws->mag_inserted = 1; ws->mag_rounds = mag_rounds > 0 ? mag_rounds : 0; ws->chambered = 1;
    g->bolt_locked = 0; g->bolt_t = 0; g->bolt_pos = 0;
}

int gun_casings(const GunState *g, WPose *out, int max) {
    int n = 0;
    for (int i = 0; i < GUN_MAX_CASINGS && n < max; i++)
        if (g->casing[i].active) out[n++] = g->casing[i].pose;
    return n;
}

void gun_update(GunState *g, const GunCfg *c, const WInput *in, WReloadMode rm,
                const WGameBridge *br, const WQuat *aim) {
    WeaponState *s = &g->base;
    const WeaponDef *d = s->def;
    const int wid = d->game_weapon_id;
    (void)rm;                                  /* no hay recarga VR: el juego recarga solo y el anfitrión iguala el arma a su cargador (make_ready) */
    float dt = wcore_dt(in->dt, 0.1f);
    if (dt <= 0) return;                       /* dt NaN, 0 o negativo: cuadro inválido */
    uint32_t e0 = in->buttons[0] & ~g->btn_prev[0];

    wcore_recoil_step(s, in, dt);
    WPose wo = wcore_weapon_world(s, in);
    float floor_y = in->head.p.y - c->floor_drop;

    /* selector / seguro */
    if (e0 & WB_STICK) {
        if (c->has_safety) g->safe = !g->safe;
        else if (c->can_select) g->semi = !g->semi;
    }

    /* ---------------- disparo ---------------- */
    g->idle_t += dt;
    if (g->idle_t > 0.3f) g->burst = 0;
    g->cd -= dt;
    {   /* el sobrante de tiempo se arrastra para clavar la cadencia, pero con tope: si no, tras un bloqueo
         * largo (seguro, recarga, palanca) saldría una ráfaga acumulada */
        float lim = c->interval < dt ? c->interval : dt;
        if (g->cd < -lim) g->cd = -lim;
    }
    int pressed = in->trigger[0] > 0.5f;
    if (in->trigger[0] < 0.3f) { g->latch = 0; g->dry_latch = 0; g->equip_latch = 0; }
    if (!pressed && g->cd < 0) g->cd = 0;
    if (g->equip_latch) pressed = 0;
    for (int n = 0; n < 4 && pressed && g->cd <= 0 && !g->safe; n++) {
        if (g->semi && g->latch) break;
        if (!s->chambered || g->bolt_locked) {
            if (!g->dry_latch) { g->dry_latch = 1; SND(WS_DRY); HAP(0, 0.15f, 0.02f); }
            break;
        }
        /* bala fuera */
        WPose pm = wpose_mul(wo, g->sight_on ? g->sight : d->points[WP_MUZZLE]);
        if (aim) pm.q = wquat_normalize(wquat_mul(pm.q, *aim));
        if (br && br->fire) br->fire(wid, pm);
        SND(WS_FIRE);
        HAP(0, 0.8f, 0.05f);
        if (in->support_held) HAP(1, 0.35f, 0.05f);
        g->shots++;
        eject_casing(g, c, wo);
        /* retroceso: impulso base + subida acumulativa durante la ráfaga */
        g->seed = g->seed * 1664525u + 1013904223u + (uint32_t)g->shots;
        wcore_kick(s, in, g->seed);
        if (g->burst < GUN_BURST_MAX) g->burst += 1.0f;
        {
            const WRecoil *r = &d->recoil;
            float f = in->support_held ? r->two_hand_factor : 1.0f;
            WPose wih = wpose_inv(d->points[WP_GRIP]);
            WVec3 right = wquat_rotate(wih.q, wv(1, 0, 0));
            float k = c->burst_kick * (g->burst / GUN_BURST_MAX) * f;
            s->recoil_angvel = wv_add(s->recoil_angvel, wv_scale(right, k));
        }
        g->idle_t = 0;
        g->bolt_t = c->bolt_cycle;
        g->latch = 1;
        g->cd += c->interval;
        /* siguiente bala */
        if (s->mag_inserted && s->mag_rounds > 0) { s->mag_rounds--; s->chambered = 1; }
        else { s->chambered = 0; if (s->mag_inserted) g->bolt_locked = 1; }
    }

    /* ---------------- animaciones y salida ---------------- */
    float bp = 0;
    if (g->bolt_t > 0) {
        g->bolt_t -= dt;
        float u = 1.0f - (g->bolt_t > 0 ? g->bolt_t : 0) / c->bolt_cycle;
        bp = u < 0.5f ? 2 * u : 2 * (1 - u);
    }
    if (g->bolt_locked) bp = 1.0f;
    g->bolt_pos = bp;

    for (int i = 0; i < GUN_MAX_CASINGS; i++) loose_step(&g->casing[i], dt, floor_y);

    s->channel[WCH_TRIGGER] = in->trigger[0];
    s->channel[WCH_BOLT] = g->bolt_pos;
    s->channel[WCH_MAG] = 0.0f;
    s->channel[WCH_SAFETY] = c->has_safety ? (float)g->safe : (float)g->semi;
    s->t += dt;
    s->phase = !s->mag_inserted ? GP_NOMAG : (!s->chambered ? GP_EMPTY : GP_READY);

    g->btn_prev[0] = in->buttons[0];
}
