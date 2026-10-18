/* Arrojadizas en mano: tres variantes (throw_frag, throw_b, throw_c) con espoletas distintas.
 * Interacción: sacar del chaleco (agarre en el pecho) -> tirar de la anilla con la otra mano ->
 * la palanca salta al soltar (o antes, con el gatillo, para "cocinar") -> lanzamiento físico.
 * El lanzamiento usa la velocidad lineal/angular de la mano estimada con el historial de poses.
 * Si el juego rechaza el evento (w5_emit devuelve 0) no se gasta nada: la arrojadiza sigue en la mano
 * y se reintenta cada RETRY_T s. */
#include "w_throw.h"
#include "wcore.h"
#include <string.h>
#include <math.h>


/* ---------------------------------------------------------------- parámetros ajustables */
#define R_DRAW_MANUAL     0.18f   /* radio del pecho para sacar del chaleco (m) */
#define R_DRAW_SIMPLE     0.35f
#define R_RING_MANUAL     0.045f  /* radio de agarre de la anilla */
#define R_RING_SIMPLE     0.12f
#define PULL_MANUAL       0.050f  /* recorrido para sacar el pasador */
#define PULL_SIMPLE       0.030f
#define GRIP_ON           0.70f   /* umbrales de agarre con histéresis */
#define GRIP_OFF          0.25f
#define TRIG_COOK         0.60f
#define V_MAX             22.0f   /* tope de velocidad de lanzamiento (m/s) */
#define W_MAX             45.0f   /* tope de velocidad angular entregada (rad/s) */
#define DT_MAX            0.1f    /* un cuadro largo no cuenta más que esto */
#define LEVER_H           0.008f  /* paso máximo de integración del muelle de la palanca (s) */
#define RETRY_T           0.15f   /* espera antes de reintentar un evento rechazado */
#define V_WIN             0.070f  /* ventana de estimación (s) */
#define V_LAG             0.025f  /* latencia del agarre: se ignoran los últimos ms antes de detectar la suelta */
#define RESPAWN_T         0.40f   /* tiempo hasta volver a poder sacar otra */

/* Datos ajustables por tipo (marco del arma: origen = centro del objeto, +Y = espoleta, -Z delante).
 * Los valores de posición son provisionales y se sustituyen al asociar las partes del modelo original. */
typedef struct {
    float fuse;               /* s de espoleta desde que salta la palanca */
    WVec3 pin_pos;            /* posición del pasador en el arma (reposo) */
    WVec3 pull_axis;          /* desplazamiento del pasador por unidad de tirón (m) */
    WVec3 ring_point;         /* punto de agarre de la anilla (WP_CHARGE) */
    WVec3 lever_hinge;        /* charnela de la palanca */
    float lever_angle;        /* rad de apertura de la palanca a canal = 1 */
} WThrowKind;

static const WThrowKind KINDS[3] = {
    { 3.0f, {0, 0.056f, 0.0125f}, {0, 0, 0.045f}, {0, 0.056f, 0.0485f}, {0.013f, 0.058f, 0}, 1.35f },  /* variante base */
    { 2.5f, {0, 0.098f, 0.0125f}, {0, 0, 0.045f}, {0, 0.098f, 0.0485f}, {0.012f, 0.098f, 0}, 1.35f },  /* variante b */
    { 1.8f, {0, 0.093f, 0.0125f}, {0, 0, 0.045f}, {0, 0.093f, 0.0485f}, {0.012f, 0.093f, 0}, 1.35f },  /* variante c */
};

/* Piezas esperadas del modelo original (por nombre -> canal). mesh = -1: se asocian por nombre a mesh_names. */
#define PART(nm, ch, ax, ay, az, rot) { nm, -1, {{0,0,0},{0,0,0,1}}, -1, ch, {ax, ay, az}, rot }
static const WPart THROW_PARTS[] = {
    PART("body", WCH_NONE, 0, 0, 0, 0),
    PART("fuze", WCH_NONE, 0, 0, 0, 0),
    PART("lever", WCH_CUSTOM1, 0, 0, 1.35f, 1),   /* rota alrededor de su charnela */
    PART("pin", WCH_CUSTOM0, 0, 0, 0.045f, 0),    /* pasador+anilla: se dibuja como WHeld (ver w_throw.h) */
};
static const char *const THROW_MESH_NAMES[] = { "body", "fuze", "lever", "pin", NULL };
#define THROW_MESH_PIN 3

/* ---------------------------------------------------------------- comunes */
static WLaunch g_last;
static WLaunchHook g_hook; static void *g_hook_user;
static uint32_t g_seq;
void w_throw_set_hook(WLaunchHook hook, void *user) { g_hook = hook; g_hook_user = user; }
const WLaunch *w_launch_last(void) { return &g_last; }

int w5_emit(const WGameBridge *g, WLaunch ev) {
    ev.seq = ++g_seq;
    g_last = ev;
    if (g_hook) return g_hook(g_hook_user, &g_last) ? 1 : 0;
    if (g && g->fire) g->fire(ev.weapon_id, ev.pose);
    return 1;
}

void w5_body_frame(const WInput *in, WVec3 *chest, WQuat *yaw_only) {
    WVec3 f = wquat_rotate(in->head.q, wv(0, 0, -1));
    float hx = f.x, hz = f.z;
    if (hx * hx + hz * hz < 0.04f) {          /* mirando casi vertical: usar el "arriba" de la cabeza */
        WVec3 u = wquat_rotate(in->head.q, wv(0, 1, 0));
        float s = f.y < 0 ? 1.0f : -1.0f;
        hx = u.x * s; hz = u.z * s;
    }
    float yaw = atan2f(-hx, -hz);
    WQuat q = wquat_axis_angle(wv(0, 1, 0), yaw);
    if (yaw_only) *yaw_only = q;
    if (chest) *chest = wv_add(in->head.p, wquat_rotate(q, wv(0.0f, -0.30f, -0.10f)));
}

int w5_draw_gesture(const WInput *in, WReloadMode rm, float grip_prev) {
    if (!(in->grip[0] > GRIP_ON && grip_prev <= GRIP_ON)) return 0;
    if (rm == WR_AUTOMATIC) return 1;
    WVec3 c; w5_body_frame(in, &c, NULL);
    return wv_len(wv_sub(in->hand[0].p, c)) < (rm == WR_SIMPLIFIED ? R_DRAW_SIMPLE : R_DRAW_MANUAL);
}

void w5_hide(WeaponState *s, int hidden) {
    s->recoil_offset.p = hidden ? wv(0, -100.0f, 0) : wv(0, 0, 0);
    s->recoil_offset.q = wquat_identity();
}

/* ---------------------------------------------------------------- historial y estimación de velocidad */
void wvh_reset(WVHist *h) { memset(h, 0, sizeof *h); }

void wvh_push(WVHist *h, float dt, WPose p) {
    dt = wcore_dt(dt, DT_MAX);
    if (dt <= 0) return;
    h->clock += dt;
    h->s[h->head].t = h->clock; h->s[h->head].p = p;
    h->head = (h->head + 1) % WV_HIST;
    if (h->n < WV_HIST) h->n++;
}

int wvh_estimate(const WVHist *h, float win, float lag, WVec3 *lin, WVec3 *ang) {
    if (lin) *lin = wv(0, 0, 0);
    if (ang) *ang = wv(0, 0, 0);
    if (h->n < 2) return 0;
    float t1 = h->clock - lag, t0 = t1 - win;
    const WVSample *sel[WV_HIST]; int n = 0;
    for (int i = h->n; i >= 1; i--) {         /* de más antigua a más reciente */
        const WVSample *s = &h->s[(h->head - i + WV_HIST) % WV_HIST];
        if (s->t >= t0 && s->t <= t1) sel[n++] = s;
    }
    if (n < 2) {                              /* muy pocas muestras: usa las dos más recientes */
        sel[0] = &h->s[(h->head - 2 + WV_HIST) % WV_HIST];
        sel[1] = &h->s[(h->head - 1 + WV_HIST) % WV_HIST];
        n = 2;
    }
    /* Lineal: recta por mínimos cuadrados ponderados (peso creciente hacia lo más reciente). */
    double sw = 0, st = 0;
    double w[WV_HIST];
    for (int i = 0; i < n; i++) { w[i] = 0.4 + 0.6 * (double)(i + 1) / n; sw += w[i]; st += w[i] * sel[i]->t; }
    double tm = st / sw, den = 0, nx = 0, ny = 0, nz = 0, mx = 0, my = 0, mz = 0;
    for (int i = 0; i < n; i++) { mx += w[i] * sel[i]->p.p.x; my += w[i] * sel[i]->p.p.y; mz += w[i] * sel[i]->p.p.z; }
    mx /= sw; my /= sw; mz /= sw;
    for (int i = 0; i < n; i++) {
        double dt = sel[i]->t - tm; den += w[i] * dt * dt;
        nx += w[i] * dt * (sel[i]->p.p.x - mx); ny += w[i] * dt * (sel[i]->p.p.y - my); nz += w[i] * dt * (sel[i]->p.p.z - mz);
    }
    WVec3 vl = wv(0, 0, 0);
    if (den > 1e-12) vl = wv((float)(nx / den), (float)(ny / den), (float)(nz / den));
    /* Corrección por aceleración: la mano acelera con fuerza justo antes de soltar y la recta da la velocidad
     * en el centro de la ventana (retrasada). Se ajusta una parábola p = c0 + c1 x + c2 x^2 (x = t - t1) y se
     * toma su derivada en t1 (con ventana de 70 ms el sesgo del modelo cuadrático queda por debajo del 10 %). */
    WVec3 v = vl;
    if (n >= 4 && den > 1e-12) {
        double S[5] = {0, 0, 0, 0, 0}, B[3][3] = {{0}};
        for (int i = 0; i < n; i++) {
            double x = sel[i]->t - t1, x2 = x * x, ww = w[i];
            S[0] += ww; S[1] += ww * x; S[2] += ww * x2; S[3] += ww * x2 * x; S[4] += ww * x2 * x2;
            const double pv[3] = {sel[i]->p.p.x, sel[i]->p.p.y, sel[i]->p.p.z};
            for (int a = 0; a < 3; a++) { B[a][0] += ww * pv[a]; B[a][1] += ww * x * pv[a]; B[a][2] += ww * x2 * pv[a]; }
        }
        double M[3][3] = {{S[0], S[1], S[2]}, {S[1], S[2], S[3]}, {S[2], S[3], S[4]}};
        double det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0])
                   + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
        if (fabs(det) > 1e-18) {
            float q[3];
            for (int a = 0; a < 3; a++) {      /* regla de Cramer solo para c1 (derivada en x = 0) */
                double Mc[3][3]; memcpy(Mc, M, sizeof Mc);
                for (int r = 0; r < 3; r++) Mc[r][1] = B[a][r];
                double dc = Mc[0][0] * (Mc[1][1] * Mc[2][2] - Mc[1][2] * Mc[2][1]) - Mc[0][1] * (Mc[1][0] * Mc[2][2] - Mc[1][2] * Mc[2][0])
                          + Mc[0][2] * (Mc[1][0] * Mc[2][1] - Mc[1][1] * Mc[2][0]);
                q[a] = (float)(dc / det);
            }
            WVec3 vq = wv(q[0], q[1], q[2]);
            v = wv_add(vl, wv_scale(wv_sub(vq, vl), WVH_ACCEL_BLEND));   /* mezcla parcial, no sustitución total */
        }
    }
    if (lin) *lin = v;
    /* Angular: giros entre muestras consecutivas (vector de rotación / dt, mundo); recta sobre esas velocidades
     * evaluada en t1 (mezcla 70 %) con la media ponderada como base. */
    if (ang) {
        WVec3 om[WV_HIST]; float tt[WV_HIST], ow[WV_HIST]; int m = 0;
        WVec3 acc = wv(0, 0, 0); float aw = 0;
        for (int i = 1; i < n; i++) {
            float dt = (float)(sel[i]->t - sel[i - 1]->t);
            if (dt < 1e-5f) continue;
            WQuat dq = wquat_mul(sel[i]->p.q, wquat_inv(sel[i - 1]->p.q));
            om[m] = wv_scale(wquat_to_rotvec(dq), 1.0f / dt);
            tt[m] = (float)(0.5 * (sel[i]->t + sel[i - 1]->t)); ow[m] = (float)w[i];
            acc = wv_add(acc, wv_scale(om[m], ow[m])); aw += ow[m]; m++;
        }
        if (aw > 0) {
            WVec3 mean = wv_scale(acc, 1.0f / aw), res = mean;
            if (m >= 3) {
                double a0 = 0, a1 = 0, tmm = 0;
                for (int i = 0; i < m; i++) { a0 += ow[i]; tmm += ow[i] * tt[i]; }
                tmm /= a0;
                for (int i = 0; i < m; i++) a1 += ow[i] * (tt[i] - tmm) * (tt[i] - tmm);
                if (a1 > 1e-12) {
                    WVec3 sl = wv(0, 0, 0);
                    for (int i = 0; i < m; i++) sl = wv_add(sl, wv_scale(om[i], (float)(ow[i] * (tt[i] - tmm))));
                    sl = wv_scale(sl, (float)(1.0 / a1));
                    WVec3 ext = wv_add(mean, wv_scale(sl, (float)(t1 - tmm)));
                    res = wv_add(mean, wv_scale(wv_sub(ext, mean), 0.7f));
                }
            }
            *ang = res;
        }
    }
    return n;
}

/* ---------------------------------------------------------------- arrojadizas */
static void throw_init_k(WeaponState *s, const WeaponDef *d, int kind) {
    WThrowState *t = (WThrowState *)s;
    wcore_state_reset(s, d);
    for (int i = 0; i < 4; i++) t->held_mesh[i] = -1;
    t->held_mesh[0] = THROW_MESH_PIN;
    t->kind = kind;
    s->phase = WT_PH_VEST;
    w5_hide(s, 1);
    wvh_reset(&t->hist);
}

static WPose pin_world(const WThrowState *t, const WInput *in, WVec3 *unused) {
    (void)unused;
    const WeaponState *s = &t->base;
    const WThrowKind *k = &KINDS[t->kind];
    float pull = t->ring_pull;
    WPose r; r.q = wquat_identity();
    r.p = wv_add(k->pin_pos, wv_scale(k->pull_axis, pull));
    return wpose_mul(wcore_weapon_world(s, in), r);
}

int w_throw_ring_pose(const WThrowState *t, const WInput *in, WPose *out) {
    if (t->base.phase == WT_PH_VEST || t->base.phase == WT_PH_GONE) return 0;
    if (t->ring_detached) {
        if (!t->ring_grabbed) return 0;      /* soltada: ya no se ve */
        *out = wpose_mul(in->hand[1], t->ring_grab_off);
        return 1;
    }
    *out = pin_world(t, in, NULL);
    return 1;
}

static void snd(const WGameBridge *g, int id, int ev) { if (g && g->sound) g->sound(id, ev); }
static void hap(const WGameBridge *g, int hand, float a, float sec) { if (g && g->haptic) g->haptic(hand, a, sec); }

static void sync_held(WThrowState *t, const WInput *in) {
    WeaponState *s = &t->base;
    WPose p;
    s->held[0].mesh = t->held_mesh[0];
    s->held[0].active = w_throw_ring_pose(t, in, &p);
    if (s->held[0].active) s->held[0].world = p;
}

static void lever_step(WThrowState *t, float dt) {
    float *c = &t->base.channel[WCH_CUSTOM1];
    if (!t->lever_off) { *c = 0; t->lever_v = 0; return; }
    /* muelle amortiguado hacia 1 con impulso inicial: la palanca "salta" y rebasa un poco.
     * Euler semi-implícito con subpasos <= LEVER_H (con k=900 es estable hasta ~60 ms) */
    int n = (int)ceilf(dt / LEVER_H); if (n < 1) n = 1;
    float h = dt / (float)n;
    for (int i = 0; i < n; i++) {
        float a = -900.0f * (*c - 1.0f) - 38.0f * t->lever_v;
        t->lever_v += a * h; *c += t->lever_v * h;
    }
    *c = wcore_clampf(*c, 0.0f, 1.5f);                 /* canal acotado: rebasa un poco, no más */
    t->lever_v = wcore_clampf(t->lever_v, -60.0f, 60.0f);
}

/* Entrega el lanzamiento (o la explosión en mano). Devuelve 1 si el juego lo aceptó: solo entonces se
 * gasta la munición y el objeto pasa a GONE; si no, queda en la mano y se reintentará. */
static int throw_it(WThrowState *t, const WInput *in, const WGameBridge *g, int inhand) {
    WeaponState *s = &t->base;
    const WeaponDef *d = s->def;
    WLaunch ev; memset(&ev, 0, sizeof ev);
    ev.weapon_id = d->game_weapon_id;
    ev.pose = wcore_weapon_world(s, in);
    ev.cooked = t->cooked;
    ev.fuse_left = t->lever_off ? t->fuse : KINDS[t->kind].fuse;
    if (ev.fuse_left < 0) ev.fuse_left = 0;
    if (inhand) {
        ev.kind = WL_INHAND; ev.fuse_left = 0;
    } else {
        ev.kind = WL_THROW;
        WVec3 vh, wh;
        wvh_estimate(&t->hist, V_WIN, V_LAG, &vh, &wh);
        float wl = wv_len(wh);
        if (!(wl <= W_MAX)) wh = wl > 0 && wl == wl ? wv_scale(wh, W_MAX / wl) : wv(0, 0, 0);   /* tope (y NaN -> 0) */
        WVec3 r = wv_sub(ev.pose.p, in->hand[0].p);          /* de la mano al centro del objeto */
        WVec3 v = wv_add(vh, wv_cross(wh, r));
        float sp = wv_len(v);
        if (!(sp <= V_MAX)) v = sp > 0 && sp == sp ? wv_scale(v, V_MAX / sp) : wv(0, 0, 0);
        ev.lin_vel = v; ev.ang_vel = wh;
    }
    if (!w5_emit(g, ev)) {                                   /* el juego no lo acepta: sigue en la mano */
        t->retry = RETRY_T;
        snd(g, d->game_weapon_id, WS_DRY); hap(g, 0, 0.2f, 0.04f);
        return 0;
    }
    if (!inhand) {
        hap(g, 0, 0.5f, 0.05f);
        snd(g, d->game_weapon_id, W5_S_THROW);
        snd(g, d->game_weapon_id, WS_FIRE);
    }
    /* la reserva nunca baja de 0: si ya no queda (se gastó fuera) no se descuenta */
    int res = g && g->ammo_reserve ? g->ammo_reserve(d->game_weapon_id) : 1;
    if (g && g->ammo_take && res > 0) g->ammo_take(d->game_weapon_id, 1);
    s->phase = WT_PH_GONE; s->t = 0; w5_hide(s, 1);
    t->ring_detached = 0; t->ring_grabbed = 0; t->launched = 1;
    return 1;
}

static void throw_update(WeaponState *s, const WInput *in, WReloadMode rm, const WGameBridge *g) {
    WThrowState *t = (WThrowState *)s;
    const WeaponDef *d = s->def;
    const WThrowKind *k = &KINDS[t->kind];
    float dt = wcore_dt(in->dt, DT_MAX);
    if (dt <= 0) return;                       /* dt NaN, 0 o negativo: cuadro inválido */
    int id = d->game_weapon_id;
    s->t += dt;
    if (t->retry > 0) t->retry -= dt;
    wvh_push(&t->hist, dt, in->hand[0]);
    int reserve = g && g->ammo_reserve ? g->ammo_reserve(id) : 1;
    if (reserve < 0) reserve = 0;
    s->mag_rounds = reserve;

    switch (s->phase) {
    case WT_PH_VEST:
        if (w5_draw_gesture(in, rm, t->grip_prev)) {
            if (reserve > 0) {
                s->phase = WT_PH_HELD; s->t = 0; w5_hide(s, 0);
                t->ring_pull = 0; t->ring_grabbed = 0; t->ring_detached = 0; t->lever_off = 0;
                t->fuse = 0; t->cooked = 0; t->launched = 0; s->channel[WCH_CUSTOM0] = 0;
                snd(g, id, W5_S_DRAW); hap(g, 0, 0.3f, 0.04f);
            } else { snd(g, id, W5_S_EMPTY); snd(g, id, WS_DRY); hap(g, 0, 0.15f, 0.03f); }
        }
        break;
    case WT_PH_GONE:
        if (s->t > RESPAWN_T) { s->phase = WT_PH_VEST; s->t = 0; t->lever_off = 0; s->channel[WCH_CUSTOM1] = 0; }
        break;
    case WT_PH_HELD: case WT_PH_PULL: {
        /* soltar con el pasador puesto: vuelve al chaleco sin gastarse */
        if (in->grip[0] < GRIP_OFF) {
            s->phase = WT_PH_VEST; w5_hide(s, 1); t->ring_grabbed = 0; t->ring_pull = 0; break;
        }
        float rr = rm == WR_SIMPLIFIED ? R_RING_SIMPLE : R_RING_MANUAL;
        float pl = rm == WR_SIMPLIFIED ? PULL_SIMPLE : PULL_MANUAL;
        if (rm == WR_AUTOMATIC) {
            /* anilla automática: con el gatillo se tira sola */
            if (in->trigger[0] > 0.7f || t->ring_pull > 0) {
                t->ring_pull += dt / 0.20f; s->phase = WT_PH_PULL;
                if (t->ring_pull >= 1.0f) { t->ring_pull = 1.0f; t->ring_detached = 1; t->ring_grabbed = 0; }
            }
        } else if (!t->ring_grabbed) {
            if (in->grip[1] > GRIP_ON && wcore_hand_dist(s, in, 1, WP_CHARGE) < rr) {
                t->ring_grabbed = 1; s->phase = WT_PH_PULL;
                /* posición de la mano 1 en el marco del arma al agarrar (auxiliar del tirón) */
                t->ring_grab_off.p = wpose_apply(wpose_inv(wcore_weapon_world(s, in)), in->hand[1].p);
                t->ring_grab_off.q = wquat_identity();
            }
            if (!t->ring_grabbed) {
                if (t->ring_pull > 0) { t->ring_pull -= dt / 0.15f; if (t->ring_pull < 0) t->ring_pull = 0; }
                if (t->ring_pull == 0 && s->phase == WT_PH_PULL) s->phase = WT_PH_HELD;
            }
        } else {
            if (in->grip[1] < 0.3f) { t->ring_grabbed = 0; }          /* soltó: el pasador vuelve */
            else {
                WVec3 hl = wpose_apply(wpose_inv(wcore_weapon_world(s, in)), in->hand[1].p);
                float dd = wv_len(wv_sub(hl, t->ring_grab_off.p));      /* recorrido de la mano desde el agarre */
                t->ring_pull = wcore_clampf(dd / pl, 0.0f, 1.0f);
                if (t->ring_pull >= 1.0f) t->ring_detached = 1;
            }
        }
        if (t->ring_detached) {
            if (rm != WR_AUTOMATIC) {
                WPose pw = pin_world(t, in, NULL);
                t->ring_grab_off = wpose_mul(wpose_inv(in->hand[1]), pw);   /* la anilla queda en la mano 1 */
                t->ring_grabbed = 1;
            } else t->ring_grabbed = 0;
            s->phase = WT_PH_ARMED; s->t = 0; t->lever_off = 0; t->lever_v = 0; t->cooked = 0;
            t->fuse = k->fuse; t->trig_ready = 0;
            snd(g, id, W5_S_PIN); snd(g, id, WS_MAG_OUT);
            hap(g, 0, 0.6f, 0.05f); hap(g, 1, 0.6f, 0.05f);
        }
        s->channel[WCH_CUSTOM0] = t->ring_pull;
        break;
    }
    case WT_PH_ARMED:
        /* la anilla se queda en la mano 1 mientras la agarre */
        if (t->ring_grabbed && in->grip[1] < 0.3f) t->ring_grabbed = 0;
        s->channel[WCH_CUSTOM0] = 1.0f;
        if (in->grip[0] < GRIP_OFF) {
            /* suelta: la palanca salta y la espoleta arranca; el lanzamiento sale ya */
            if (!t->lever_off) {
                t->lever_off = 1; t->lever_v = 14.0f; t->fuse = k->fuse;
                snd(g, id, W5_S_LEVER); snd(g, id, WS_SLIDE_FWD);
            }
            if (!throw_it(t, in, g, 0)) { s->phase = WT_PH_COOK; s->t = 0; t->cooked = 0; }   /* rechazado: sigue con la espoleta en marcha */
        } else if (in->trigger[0] > TRIG_COOK && t->trig_ready) {
            /* cocinado: la palanca salta con el objeto aún en la mano */
            t->lever_off = 1; t->lever_v = 14.0f; t->fuse = k->fuse; t->cooked = 0;
            s->phase = WT_PH_COOK; s->t = 0;
            snd(g, id, W5_S_LEVER); snd(g, id, WS_SLIDE_FWD); hap(g, 0, 0.4f, 0.05f);
        }
        if (in->trigger[0] < 0.3f) t->trig_ready = 1;
        break;
    case WT_PH_COOK:
        t->fuse -= dt; t->cooked += dt;
        if (t->retry > 0) { /* esperando para reintentar un evento rechazado */ }
        else if (in->grip[0] < GRIP_OFF) throw_it(t, in, g, 0);
        else if (t->fuse <= 0) throw_it(t, in, g, 1);
        else if (fmodf(t->cooked, 0.5f) < dt) hap(g, 0, 0.12f, 0.03f);   /* pulso táctil de aviso */
        break;
    }
    t->grip_prev = in->grip[0];
    lever_step(t, dt);
    sync_held(t, in);
}

/* ---------------------------------------------------------------- definiciones */
/* Los id de arma del juego son valores de integración: 100+ hasta que la capa de juego los mapee. */
#define ID_Q {0, 0, 0, 1}
#define THROW_POINTS(Y) { \
    [WP_GRIP]    = {{0, 0, 0}, ID_Q}, [WP_SUPPORT] = {{0, Y, 0.0485f}, ID_Q}, [WP_MUZZLE] = {{0, 0, -0.04f}, ID_Q}, \
    [WP_EYE]     = {{0, 0, 0}, ID_Q}, [WP_MAGWELL] = {{0, 0, 0}, ID_Q},  [WP_CHARGE] = {{0, Y, 0.0485f}, ID_Q}, \
    [WP_SCREEN]  = {{0, 0, 0}, ID_Q} }
/* Un init por variante: no dependen de que el llamador haya asignado s->def. */
#define THROW_DEF(NAME, ID, GID, K, KIND) \
    static void throw_init_##NAME(WeaponState *s); \
    const WeaponDef w_throw_##NAME = { .id = ID, .game_weapon_id = GID, .meshes = NULL, .nmeshes = 0, \
        .parts = THROW_PARTS, .nparts = 4, .points = THROW_POINTS(K), \
        .recoil = { .kick_back = 0, .kick_up = 0, .kick_yaw_rand = 0, .stiffness = 0, .damping = 0, .two_hand_factor = 1 }, \
        .mode = WF_THROWN, .rpm = 0, .mag_capacity = 1, \
        .state_size = sizeof(WThrowState), .init = throw_init_##NAME, .update = throw_update, \
        .mesh_names = THROW_MESH_NAMES }; \
    static void throw_init_##NAME(WeaponState *s) { throw_init_k(s, &w_throw_##NAME, KIND); }
THROW_DEF(frag, "throw_frag", 100, 0.056f, 0)
THROW_DEF(b,    "throw_b",    101, 0.098f, 1)
THROW_DEF(c,    "throw_c",    102, 0.093f, 2)
