/* Lógica común de armas de cargador con recarga manual. Ver w_gunc.h. */
#include <math.h>
#include <string.h>
#include "w_gunc.h"

#define SND(e)  do { if (br && br->sound) br->sound(wid, (e)); } while (0)
#define HAP(h, k, t) do { if (br && br->haptic) br->haptic((h), (k), (t)); } while (0)

static int reserve_of(const WGameBridge *br, int wid) {
    return (br && br->ammo_reserve) ? br->ammo_reserve(wid) : 1000;
}
static void take_ammo(const WGameBridge *br, int wid, int n) {
    if (br && br->ammo_take && n != 0) br->ammo_take(wid, n);
}

WVec3 gun_waist_zone(const WInput *in, float *radius) {
    WVec3 f = wquat_rotate(in->head.q, wv(0, 0, -1));
    f.y = 0; f = wv_norm(f);
    if (wv_len(f) < 0.5f) f = wv(0, 0, -1);
    WVec3 r = wv(-f.z, 0, f.x);
    if (radius) *radius = 0.22f;
    return wv_add(in->head.p, wv_add(wv(0, -0.60f, 0), wv_add(wv_scale(f, 0.08f), wv_scale(r, -0.12f))));
}

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

static void spawn_loose_mag(GunState *g, const GunCfg *c, WPose wo) {
    WPose p; p.p = wpose_apply(wo, c->mag_rest); p.q = wo.q;
    loose_spawn(&g->mag_loose, p, wquat_rotate(wo.q, wv(0, -0.6f, 0.0f)),
                wquat_rotate(wo.q, wv(2.0f * wcore_rand(&g->seed), 0.5f, 1.5f * wcore_rand(&g->seed))), 3.0f);
}

/* ------------------------------------------------------------ bolt / carga */
/* Ciclo de la palanca: expulsa la bala encamarada (si la hay) y encamara la siguiente. La bala expulsada
 * es viva: vuelve al contador del juego (ammo_take(-1)), como en la pistola. */
static void cycle_bolt(GunState *g, const GunCfg *c, WPose wo, const WGameBridge *br, int wid) {
    WeaponState *s = &g->base;
    if (s->chambered) { eject_casing(g, c, wo); s->chambered = 0; take_ammo(br, wid, -1); }
    if (s->mag_inserted && s->mag_rounds > 0) { s->mag_rounds--; s->chambered = 1; }
    g->bolt_locked = 0;
    SND(WS_SLIDE_FWD);
}

/* ------------------------------------------------------------ inicio / recarga temporizada */
#define SEQ_T_MAG_IN   0.55f   /* instante en que entra el cargador nuevo */
#define SEQ_T_NOMAG    0.40f   /* sin cargador montado la secuencia arranca justo antes de insertar */
static void seq_start(GunState *g, const GunCfg *c, WPose wo, const WGameBridge *br, int wid) {
    WeaponState *s = &g->base;
    if (s->mag_inserted) {
        if (s->mag_rounds > 0) take_ammo(br, wid, -s->mag_rounds);   /* devuelve las balas al contador */
        s->mag_rounds = 0; s->mag_inserted = 0;
        spawn_loose_mag(g, c, wo);
        SND(WS_MAG_OUT);
        g->seq_t = 0.0f;
    } else {
        g->seq_t = SEQ_T_NOMAG;   /* debe quedar por debajo de SEQ_T_MAG_IN para que seq_step lo cruce */
    }
    g->seq = 1;
}

static void seq_step(GunState *g, const GunCfg *c, float dt, WPose wo, const WGameBridge *br, int wid, int cap) {
    WeaponState *s = &g->base;
    float t0 = g->seq_t, t1 = t0 + dt;
    g->seq_t = t1;
    if (t0 < SEQ_T_MAG_IN && t1 >= SEQ_T_MAG_IN) {
        int res = reserve_of(br, wid);
        if (res <= 0) { SND(WS_DRY); g->seq = 0; return; }
        int n = res < cap ? res : cap;
        take_ammo(br, wid, n);
        s->mag_inserted = 1; s->mag_rounds = n; g->mag_slide = 1.0f;
        SND(WS_MAG_IN); HAP(1, 0.6f, 0.05f);
    }
    if (g->seq_t >= 0.95f && g->seq_t < 1.20f && !s->chambered) {
        float u = (g->seq_t - 0.95f) / 0.25f;                 /* palanca atrás y adelante */
        g->charge_pos = u < 0.5f ? 2 * u : 2 * (1 - u);
        if (t0 < 1.075f && t1 >= 1.075f) SND(WS_SLIDE_BACK);
    }
    if (t0 < 1.20f && t1 >= 1.20f) {
        g->charge_pos = 0;
        if (!s->chambered) cycle_bolt(g, c, wo, br, wid);
    }
    if (g->seq_t >= 1.35f) g->seq = 0;
}

/* ------------------------------------------------------------ API */
void gun_init(GunState *g, const WeaponDef *d, const GunCfg *c) {
    wcore_state_reset(&g->base, d);
    g->seed = 0x1234567u;
    g->semi = c->start_semi;
    g->charge_hand = -1;
    g->equip_latch = 1;
}

void gun_make_ready(WeaponState *ws, int mag_rounds) {
    GunState *g = (GunState *)ws;
    ws->mag_inserted = 1; ws->mag_rounds = mag_rounds > 0 ? mag_rounds : 0; ws->chambered = 1;
    g->mag_slide = 1.0f; g->mag_loose.active = 0; g->hand_mag = 0;
    g->bolt_locked = 0; g->bolt_t = 0; g->bolt_pos = 0; g->seq = 0; g->seq_t = 0; g->empty_t = 0; g->charge_pos = 0; g->charge_hand = -1; g->charge_pulled = 0;
}

int gun_loose_mag(const GunState *g, WPose *out) {
    if (!g->mag_loose.active) return 0;
    *out = g->mag_loose.pose; return 1;
}
int gun_held_mag(const GunState *g, const GunCfg *c, const WInput *in, WPose *out) {
    if (!g->hand_mag) return 0;
    out->q = in->hand[1].q;
    out->p = wv_add(in->hand[1].p, wquat_rotate(in->hand[1].q, wv(0, c->mag_in_hand, 0)));
    return 1;
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
    const int cap = d->mag_capacity;
    float dt = wcore_dt(in->dt, 0.1f);
    if (dt <= 0) return;                       /* dt NaN, 0 o negativo: cuadro inválido */
    uint32_t e0 = in->buttons[0] & ~g->btn_prev[0];
    uint32_t e1 = in->buttons[1] & ~g->btn_prev[1];
    float grip1_edge = (in->grip[1] > 0.7f && g->grip_prev[1] <= 0.7f);

    /* primera actualización: cargador completo del contador del juego y bala en la recámara */
    if (!g->booted) {
        g->booted = 1;
        int res = reserve_of(br, wid);
        int n = res < cap ? res : cap;
        if (n > 0) {
            take_ammo(br, wid, n);
            s->mag_inserted = 1; s->mag_rounds = n - 1; s->chambered = 1;
        }
        g->charge_hand = -1;
    }

    wcore_recoil_step(s, in, dt);
    WPose wo = wcore_weapon_world(s, in);
    float floor_y = in->head.p.y - c->floor_drop;

    /* selector / seguro */
    if (e0 & WB_STICK) {
        if (c->has_safety) g->safe = !g->safe;
        else if (c->can_select) g->semi = !g->semi;
    }

    /* ---------------- recarga temporizada (simplificada / automática) ---------------- */
    if (g->seq == 0 && rm != WR_MANUAL) {
        int empty = !s->chambered && (!s->mag_inserted || s->mag_rounds == 0);
        g->empty_t = empty ? g->empty_t + dt : 0.0f;
        int btn = ((e0 | e1) & WB_MAG_RELEASE) != 0;
        int have = reserve_of(br, wid) > 0;
        int autoreload = rm == WR_AUTOMATIC && empty && g->empty_t > 0.3f && have;   /* sin reserva espera en silencio */
        if (btn && !have) SND(WS_DRY);
        if ((btn || autoreload) && have) { seq_start(g, c, wo, br, wid); g->empty_t = 0; }
    }
    if (g->seq) seq_step(g, c, dt, wo, br, wid, cap);

    /* ---------------- recarga manual ---------------- */
    if (rm == WR_MANUAL && !g->seq) {
        float dm = wcore_hand_dist(s, in, 1, WP_MAGWELL);
        /* 1) liberar el cargador con la mano libre en el alojamiento */
        if (s->mag_inserted && dm < c->magwell_r && ((e0 | e1) & WB_MAG_RELEASE) && !g->hand_mag) {
            if (in->grip[1] > 0.5f) {
                g->hand_mag = 1; g->hand_mag_old = 1; g->hand_mag_rounds = s->mag_rounds;
            } else {
                spawn_loose_mag(g, c, wo);              /* cae al suelo; sus balas se pierden */
            }
            s->mag_inserted = 0; s->mag_rounds = 0; g->mag_slide = 0;
            SND(WS_MAG_OUT); HAP(1, 0.45f, 0.05f);
        }
        /* 3) coger un cargador nuevo en la cintura */
        float zr; WVec3 zc = gun_waist_zone(in, &zr);
        float dz = wv_len(wv_sub(in->hand[1].p, zc));
        if (!s->mag_inserted && !g->hand_mag && grip1_edge && dz < zr) {
            int res = reserve_of(br, wid);
            if (res > 0) {
                int n = res < cap ? res : cap;
                take_ammo(br, wid, n);
                g->hand_mag = 1; g->hand_mag_old = 0; g->hand_mag_rounds = n;
                HAP(1, 0.5f, 0.04f);
            } else { SND(WS_DRY); }
        }
        /* 2) soltar el cargador de la mano: guardarlo en la cintura o tirarlo */
        if (g->hand_mag && in->grip[1] < 0.3f) {
            if (dz < zr) {
                if (g->hand_mag_rounds > 0) take_ammo(br, wid, -g->hand_mag_rounds);
            } else {
                WPose hp; gun_held_mag(g, c, in, &hp);
                loose_spawn(&g->mag_loose, hp, wv(0, 0, 0), wv(1.0f, 0.3f, 0.8f), 3.0f);
            }
            g->hand_mag = 0;
        }
        /* 4) insertar: la parte alta del cargador entra por la boca del alojamiento */
        if (g->hand_mag && !s->mag_inserted) {
            WPose hp; gun_held_mag(g, c, in, &hp);
            WVec3 up_h = wquat_rotate(hp.q, wv(0, 1, 0));
            WVec3 top = wv_add(hp.p, wv_scale(up_h, c->mag_len * 0.5f));
            WPose mw = wcore_point_world(s, in, WP_MAGWELL);
            WVec3 up_w = wquat_rotate(wo.q, wv(0, 1, 0));
            WVec3 fw_w = wquat_rotate(wo.q, wv(0, 0, -1));
            WVec3 fw_h = wquat_rotate(hp.q, wv(0, 0, -1));
            float dd = wv_len(wv_sub(top, mw.p));
            int aligned = wv_dot(up_h, up_w) > c->mag_align_cos && wv_dot(fw_h, fw_w) > -0.2f;
            if (dd < GUN_INSERT_R && aligned) {
                s->mag_inserted = 1; s->mag_rounds = g->hand_mag_rounds; g->hand_mag = 0;
                g->mag_slide = 0.6f;
                SND(WS_MAG_IN); HAP(1, 0.8f, 0.06f); HAP(0, 0.3f, 0.04f);
            } else if (dd < GUN_INSERT_R * 0.8f && g->bump_t <= 0) {
                HAP(1, 0.25f, 0.03f); g->bump_t = 0.25f;     /* tope: está mal orientado */
            }
        }
    }
    if (g->bump_t > 0) g->bump_t -= dt;
    if (g->mag_slide > 0) { g->mag_slide -= 5.0f * dt; if (g->mag_slide < 0) g->mag_slide = 0; }

    /* ---------------- palanca de carga (todos los modos, mano libre) ---------------- */
    if (!g->seq) {
        WPose inv = wpose_inv(wo);
        WVec3 loc = wpose_apply(inv, in->hand[1].p);
        WVec3 ch = d->points[WP_CHARGE].p;
        if (g->charge_hand < 0) {
            WVec3 dl = wv_sub(loc, ch);
            if (grip1_edge && wv_len(dl) < c->charge_grab_r) { g->charge_hand = 1; g->charge_z0 = loc.z; g->charge_pulled = 0; }
        } else {
            float lat = hypotf(loc.x - ch.x, loc.y - ch.y);
            if (in->grip[1] < 0.35f || lat > 0.12f) {
                if (g->charge_pulled) {
                    cycle_bolt(g, c, wo, br, wid);
                    HAP(1, 0.5f, 0.04f);
                }
                g->charge_hand = -1; g->charge_pulled = 0;
            } else {
                float tr = loc.z - g->charge_z0;
                if (tr < 0) tr = 0;
                if (tr > c->charge_travel) tr = c->charge_travel;
                g->charge_pos = tr / c->charge_travel;
                if (g->charge_pos > 0.9f && !g->charge_pulled) {
                    g->charge_pulled = 1; SND(WS_SLIDE_BACK); HAP(1, 0.5f, 0.04f);
                }
            }
        }
        if (g->charge_hand < 0 && g->charge_pos > 0) g->charge_pos = wcore_approach(g->charge_pos, 0, 12.0f * dt);
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
    int busy = g->seq || g->charge_hand >= 0 || g->charge_pos > 0.05f;
    for (int n = 0; n < 4 && pressed && g->cd <= 0 && !busy && !g->safe; n++) {
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
    if (g->charge_pos > bp) bp = g->charge_pos;
    g->bolt_pos = bp;

    for (int i = 0; i < GUN_MAX_CASINGS; i++) loose_step(&g->casing[i], dt, floor_y);
    loose_step(&g->mag_loose, dt, floor_y);

    s->channel[WCH_TRIGGER] = in->trigger[0];
    s->channel[WCH_BOLT] = g->bolt_pos;
    s->channel[WCH_MAG] = g->mag_slide;
    s->channel[WCH_CUSTOM0] = g->charge_pos;
    s->channel[WCH_SAFETY] = c->has_safety ? (float)g->safe : (float)g->semi;
    s->t += dt;
    s->phase = g->seq ? GP_AUTO : (!s->mag_inserted ? GP_NOMAG : (!s->chambered ? GP_NEEDCHARGE : GP_READY));

    g->btn_prev[0] = in->buttons[0]; g->btn_prev[1] = in->buttons[1];
    g->grip_prev[0] = in->grip[0]; g->grip_prev[1] = in->grip[1];
}
