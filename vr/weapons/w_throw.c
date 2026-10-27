/* Arrojadizas en mano: tres variantes (throw_frag, throw_b, throw_c) con espoletas distintas.
 * Interacción: el objeto equipado aparece en la mano (sin gesto). Cerrar la mano (agarre) = "listo" (el anfitrión mantiene el botón de ataque del
 * juego, que es el que saca el pasador); abrirla = lanzamiento físico con la velocidad lineal/angular de la mano estimada con el historial de poses
 * (wvh_estimate). Por debajo de V_THROW_MIN (m/s) es un soltar suave (la velocidad medida, sin sonido de lanzamiento). Sin anilla, pasador ni palanca
 * propios: el pasador y la palanca son piezas del modelo, pegadas a él.
 * Si el juego rechaza el evento (w5_emit devuelve 0) no se gasta nada: la arrojadiza sigue lista y se reintenta cada RETRY_T s. */
#include "w_throw.h"
#include "wcore.h"
#include <string.h>
#include <math.h>


/* ---------------------------------------------------------------- parámetros ajustables */
#define GRIP_ON           0.70f   /* umbrales de agarre con histéresis */
#define GRIP_OFF          0.25f
#define V_THROW_MIN       1.0f    /* velocidad de la mano (m/s) por debajo de la cual el soltar es un dejar caer suave */
#define V_MAX             22.0f   /* tope de velocidad de lanzamiento (m/s) */
#define W_MAX             45.0f   /* tope de velocidad angular entregada (rad/s) */
#define DT_MAX            0.1f    /* un cuadro largo no cuenta más que esto */
#define RETRY_T           0.15f   /* espera antes de reintentar un evento rechazado */
#define V_WIN             0.070f  /* ventana de estimación (s) */
#define V_LAG             0.025f  /* latencia del agarre: se ignoran los últimos ms antes de detectar la suelta */
#define RESPAWN_T         0.40f   /* tiempo hasta volver a poder sacar otra */

/* Datos ajustables por tipo: s de espoleta (provisionales; el juego pone la suya al crear la granada). */
typedef struct { float fuse; } WThrowKind;
static const WThrowKind KINDS[3] = { { 3.0f }, { 2.5f }, { 1.8f } };   /* variante base, b, c */

/* Piezas esperadas del modelo original (por nombre -> canal). mesh = -1: se asocian por nombre a mesh_names. */
#define PART(nm, ch, ax, ay, az, rot) { nm, -1, {{0,0,0},{0,0,0,1}}, -1, ch, {ax, ay, az}, rot }
static const WPart THROW_PARTS[] = {
    PART("body", WCH_NONE, 0, 0, 0, 0),
    PART("fuze", WCH_NONE, 0, 0, 0, 0),
    PART("lever", WCH_CUSTOM1, 0, 0, 1.35f, 1),   /* palanca y pasador: pegados al cuerpo (sus canales quedan a 0) */
    PART("pin", WCH_CUSTOM0, 0, 0, 0.045f, 0),
};
static const char *const THROW_MESH_NAMES[] = { "body", "fuze", "lever", "pin", NULL };

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
    t->kind = kind;
    s->phase = WT_PH_VEST;
    w5_hide(s, 1);
    wvh_reset(&t->hist);
}

static void snd(const WGameBridge *g, int id, int ev) { if (g && g->sound) g->sound(id, ev); }
static void hap(const WGameBridge *g, int hand, float a, float sec) { if (g && g->haptic) g->haptic(hand, a, sec); }

/* Entrega el lanzamiento. Devuelve 1 si el juego lo aceptó: solo entonces se gasta la munición y el objeto pasa a GONE; si no, sigue listo en la
 * mano y se reintentará. */
static int throw_it(WThrowState *t, const WInput *in, const WGameBridge *g) {
    WeaponState *s = &t->base;
    const WeaponDef *d = s->def;
    WLaunch ev; memset(&ev, 0, sizeof ev);
    ev.weapon_id = d->game_weapon_id;
    ev.pose = wcore_weapon_world(s, in);
    ev.fuse_left = KINDS[t->kind].fuse;
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
    if (!w5_emit(g, ev)) {                                   /* el juego no lo acepta: sigue en la mano */
        t->retry = RETRY_T;
        snd(g, d->game_weapon_id, WS_DRY); hap(g, 0, 0.2f, 0.04f);
        return 0;
    }
    if (sp >= V_THROW_MIN) {
        hap(g, 0, 0.5f, 0.05f);
        snd(g, d->game_weapon_id, W5_S_THROW);
        snd(g, d->game_weapon_id, WS_FIRE);
    } else hap(g, 0, 0.15f, 0.03f);                          /* dejar caer: sin sonido de lanzamiento */
    /* la reserva nunca baja de 0: si ya no queda (se gastó fuera) no se descuenta */
    int res = g && g->ammo_reserve ? g->ammo_reserve(d->game_weapon_id) : 1;
    if (g && g->ammo_take && res > 0) g->ammo_take(d->game_weapon_id, 1);
    s->phase = WT_PH_GONE; s->t = 0; w5_hide(s, 1);
    return 1;
}

static void throw_update(WeaponState *s, const WInput *in, WReloadMode rm, const WGameBridge *g) {
    (void)rm;                                  /* sin anilla el modo de recarga no interviene */
    WThrowState *t = (WThrowState *)s;
    const WeaponDef *d = s->def;
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
    case WT_PH_VEST:      /* sin objeto en la mano: lo equipado en el menú del juego aparece en la mano si queda reserva (sin gesto del chaleco) */
        if (reserve > 0) {
            s->phase = WT_PH_HELD; s->t = 0; w5_hide(s, 0);
            t->primed = 0;
            snd(g, id, W5_S_DRAW); hap(g, 0, 0.3f, 0.04f);
        }
        break;
    case WT_PH_GONE:
        if (s->t > RESPAWN_T) { s->phase = WT_PH_VEST; s->t = 0; }
        break;
    case WT_PH_HELD:      /* en la mano abierta; cerrarla (tras haberla abierto con el objeto) lo deja listo: el anfitrión pulsa el botón del juego */
        if (in->grip[0] < GRIP_OFF) t->primed = 1;
        else if (in->grip[0] > GRIP_ON && t->primed) {
            s->phase = WT_PH_READY; s->t = 0;
            snd(g, id, W5_S_PIN); hap(g, 0, 0.4f, 0.04f);
        }
        break;
    case WT_PH_READY:     /* abrir la mano lanza; un rechazo del juego lo deja listo y se reintenta */
        if (in->grip[0] < GRIP_OFF && t->retry <= 0) throw_it(t, in, g);
        break;
    }
}

/* ---------------------------------------------------------------- definiciones */
/* Ids de la tabla del puente: 2 la granada principal; 7 y 8 las otras dos variantes (b/c; asignación por fusible, a confirmar). */
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
THROW_DEF(frag, "throw_frag", 2, 0.056f, 0)
THROW_DEF(b,    "throw_b",    7, 0.098f, 1)
THROW_DEF(c,    "throw_c",    8, 0.093f, 2)
