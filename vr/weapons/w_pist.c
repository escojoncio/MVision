/* Pistola semiautomática con supresor ("pist"). Lógica de mano, corredera, cargador y recarga manual.
 *
 * Convenciones (ver también wcore.c):
 *  - Mano 0 = mano del arma. Mano 1 = mano libre (cargador nuevo, tirar de la corredera).
 *  - Canales usados: WCH_SLIDE (m, hacia atrás +Z), WCH_TRIGGER (0..1), WCH_HAMMER (0..1 amartillado),
 *    WCH_MAG (m a lo largo del eje del cargador hacia abajo; 50 = oculto), WCH_ROUND (1 = recámara vacía,
 *    la pieza se aparta 100 m), WCH_CUSTOM0 (1 = sin supresor, pieza apartada).
 *  - Supresor: no se monta ni se desmonta en el VR. Lo decide el juego: WInput.game_flags & WGF_SUPP (visible y con
 *    la punta como boca) o no (pieza apartada, boca del cañón). Nunca es un objeto suelto.
 *  - Objetos sueltos (WeaponState.held): 0 = cargador (en mano / guiado / cayendo), 1 = vaina expulsada,
 *    2 = cartucho expulsado al montar la corredera.
 *  - Contador del juego: en la primera actualización se descuentan min(capacidad + 1, reserva) (cargador
 *    completo + bala en la recámara), igual que en w_gunc; al encajar un cargador nuevo, n = min(capacidad,
 *    reserva); al soltar un cargador o expulsar un cartucho vivo vuelven con ammo_take(-n) (se asume
 *    recogida). Disparar NO descuenta (esas balas se consumen). Nunca se pierde ni se duplica una bala:
 *    reserva + cargador + recámara + disparos = total inicial.
 *  - Equipar con el gatillo apretado no dispara: hay que soltarlo una vez. */
#include "wcore.h"
/* Sin meshes compilados: los modelos se enlazan en ejecución con weapon_bind() por nombre de hueco
 * (pist_mesh_names). Las cotas de abajo son el "layout" del arma (m, espacio arma). */
#define PIST_BY 0.063f   /* eje del cañón = centro del asiento del supresor (wcut_tables.c AXIS_Y) */
#define PIST_MAG_HALF 0.0675f
#define PIST_MAG_CY -0.022291f
#define PIST_MAG_CZ 0.007675f
#define PIST_MAG_QX -0.173648f   /* inclinación del mango medida en el modelo: 20 grados (ver wcut_tables.c MAG_UY) */
#define PIST_MAG_QW 0.984808f
#define PIST_MAG_UY 0.939693f
#define PIST_MAG_UZ -0.342020f
#define PIST_WELL_Y -0.075f
#define PIST_WELL_Z 0.021501f
#define PIST_SEAT_DEPTH 0.121992f
#define PIST_SUPP_Z -0.21964f   /* asiento del supresor = boca del cañón (asoma 2,64 mm de la corredera: wcut_tables.c PROT_MM) */
/* boca del cañón (a ras de la cara frontal de la corredera del modelo) y punta del supresor: salen del modelo cortado (wcut) */
#define PIST_MUZZLE_Z -0.21964f
#define PIST_SUPP_TIP_Z -0.37364f
#define PIST_TRIG_Y 0.024f
#define PIST_TRIG_Z -0.080f
#define PIST_HAM_Y 0.050f
#define PIST_HAM_Z 0.042f
#define PIST_CHAMBER_Z -0.057f
#define PIST_SLIDE_TRAVEL 0.042f
#define PIST_EJECT_X 0.014f
#define PIST_EJECT_Z -0.010f
#define PIST_CHARGE_Z 0.020f
#define PIST_EYE_Z 0.055f
#define PIST_SUP_Y 0.030f   /* punto de agarre de la mano de apoyo: bajo el cañón (eje y 0,063), a medio camino entre el guardamonte (z -0,08) y la boca (z -0,22); estimado del diseño, no medido en el modelo */
#define PIST_SUP_Z -0.150f
const float w_pist_slide_travel = PIST_SLIDE_TRAVEL;   /* recorrido de la corredera: lo usa wcut_tables.c (SLIDE_TRAVEL_MM) y lo comprueba test_wcut */
const float w_pist_supp_seat_z = PIST_SUPP_Z;
const float w_pist_bare_muzzle_z = PIST_MUZZLE_Z;   /* sin supresor (lo comprueba test_wcut contra la boca del cañón) */
enum { PIST_MESH_FRAME, PIST_MESH_SLIDE, PIST_MESH_BARREL, PIST_MESH_SUPP, PIST_MESH_TRIGGER,
       PIST_MESH_HAMMER, PIST_MESH_MAG, PIST_MESH_ROUND, PIST_MESH_CASE };
static const char *const pist_mesh_names[] = {"frame", "slide", "barrel", "supp", "trigger", "hammer", "mag", "round", "case", NULL};
#include <string.h>
#include <math.h>

extern const WeaponDef w_pist;
#define PIST_ID 0
enum { P_FRAME, P_BARREL, P_SLIDE, P_TRIGGER, P_HAMMER, P_MAG, P_ROUND, P_SUPP, P_COUNT };
enum { H_MAG, H_CASE, H_ROUND };
enum { FM_NONE, FM_HAND, FM_GUIDE, FM_ANIM, FM_FALL, FM_REST };
enum { PH_NORMAL, PH_CYCLING, PH_LOCKED, PH_CHARGING, PH_NOMAG };

#define TRAVEL PIST_SLIDE_TRAVEL
#define CYC_BACK 0.024f      /* s hasta el final del retroceso */
#define CYC_TOTAL 0.060f     /* ciclo completo ~60 ms */
#define RET_SPEED 1.6f       /* m/s al soltar la corredera a mano */
#define SEAT PIST_SEAT_DEPTH

static const WPart k_parts[P_COUNT] = {
    {"frame", -1, {{0, 0, 0}, {0, 0, 0, 1}}, PIST_MESH_FRAME, WCH_NONE, {0, 0, 0}, 0},
    {"barrel", 0, {{0, 0, 0}, {0, 0, 0, 1}}, PIST_MESH_BARREL, WCH_NONE, {0, 0, 0}, 0},
    {"slide", 0, {{0, 0, 0}, {0, 0, 0, 1}}, PIST_MESH_SLIDE, WCH_SLIDE, {0, 0, 1}, 0},
    {"trigger", 0, {{0, PIST_TRIG_Y, PIST_TRIG_Z}, {0, 0, 0, 1}}, PIST_MESH_TRIGGER, WCH_TRIGGER, {-0.35f, 0, 0}, 1},
    {"hammer", 0, {{0, PIST_HAM_Y, PIST_HAM_Z}, {0, 0, 0, 1}}, PIST_MESH_HAMMER, WCH_HAMMER, {0.75f, 0, 0}, 1},
    {"mag", 0, {{0, PIST_MAG_CY, PIST_MAG_CZ}, {PIST_MAG_QX, 0, 0, PIST_MAG_QW}}, PIST_MESH_MAG, WCH_MAG,
        {0, -PIST_MAG_UY, -PIST_MAG_UZ}, 0},
    {"round", 0, {{0, PIST_BY, PIST_CHAMBER_Z}, {0, 0, 0, 1}}, PIST_MESH_ROUND, WCH_ROUND, {0, 100, 0}, 0},
    {"supp", P_BARREL, {{0, PIST_BY, PIST_SUPP_Z}, {0, 0, 0, 1}}, PIST_MESH_SUPP, WCH_CUSTOM0, {0, 100, 0}, 0},
};

typedef struct {
    int mode;                 /* FM_* */
    WPose rel;                /* FM_HAND: pose relativa a la mano 1. FM_ANIM: pose de origen en espacio arma */
    WVec3 v, w;               /* FM_FALL: velocidad lineal y angular (mundo) */
    float t, dur;             /* FM_ANIM: progreso; vida restante en FM_REST (<0 = permanente) */
} Free;

typedef struct {
    WeaponState b;
    float slide;              /* posición real de la corredera (m) */
    int locked, cyc, peaked, fed;
    float cyc_t;
    int hammer_cocked; float hammer;
    int trig_armed;           /* el gatillo ya pasó por reposo: puede disparar */
    int booted;               /* primera actualización hecha (carga inicial descontada del contador) */
    uint32_t pbtn[2]; float pgrip[2];
    int charging, racked; float charge_ref_z, charge_slide0;
    Free fr[W_MAX_HELD];
    float mag_slide;
    int auto_stage; float auto_t;
    uint32_t seed; int shots;
    int rel_pending;          /* soltar corredera tras encajar (modos asistidos) */
    float rel_t;
} PistState;

/* ---------------- puente ---------------- */
static void snd(const WGameBridge *g, int ev) { if (g && g->sound) g->sound(PIST_ID, ev); }
static void hap(const WGameBridge *g, int hand, float str, float sec) { if (g && g->haptic) g->haptic(hand, str, sec); }
static int reserve(const WGameBridge *g) { return (g && g->ammo_reserve) ? g->ammo_reserve(PIST_ID) : 0; }
static void take(const WGameBridge *g, int n) { if (g && g->ammo_take && n) g->ammo_take(PIST_ID, n); }

static WPose mag_seat_rel(void) {
    WPose p = {{0, PIST_MAG_CY, PIST_MAG_CZ}, {PIST_MAG_QX, 0, 0, PIST_MAG_QW}}; return p;
}
static WPose mag_hand_rel(void) {
    WPose p = {{0, 0, -0.02f}, wquat_axis_angle(wv(1, 0, 0), -0.7853982f)}; return p;
}
static WVec3 mag_axis_w(void) { return wv(0, PIST_MAG_UY, PIST_MAG_UZ); }  /* "arriba" del cargador, espacio arma */

static float smooth(float u) { u = wcore_clampf(u, 0, 1); return u * u * (3 - 2 * u); }

/* ---------------- objetos sueltos ---------------- */
static void drop_free(PistState *s, int slot, WPose world, WVec3 v, WVec3 w, float life) {
    Free *f = &s->fr[slot];
    f->mode = FM_FALL; f->v = v; f->w = w; f->dur = life;
    s->b.held[slot].world = world; s->b.held[slot].active = 1;
}
static void kill_free(PistState *s, int slot) { s->fr[slot].mode = FM_NONE; s->b.held[slot].active = 0; }

static void step_free(PistState *s, int slot, float dt, float floor_y, const WInput *in, WPose W) {
    Free *f = &s->fr[slot]; WHeld *h = &s->b.held[slot];
    if (f->mode == FM_NONE) { h->active = 0; return; }
    h->active = 1;
    if (f->mode == FM_HAND) {
        h->world = wpose_mul(in->hand[1], f->rel);
    } else if (f->mode == FM_FALL) {
        f->v.y -= 9.81f * dt;
        h->world.p = wv_add(h->world.p, wv_scale(f->v, dt));
        float wl = wv_len(f->w);
        if (wl > 1e-5f) h->world.q = wquat_normalize(wquat_mul(wquat_axis_angle(f->w, wl * dt), h->world.q));
        if (h->world.p.y <= floor_y + 0.012f) {
            h->world.p.y = floor_y + 0.012f; f->mode = FM_REST; f->v = wv(0, 0, 0); f->w = wv(0, 0, 0);
        }
    } else if (f->mode == FM_REST) {
        if (f->dur >= 0) { f->dur -= dt; if (f->dur <= 0) kill_free(s, slot); }
    }
    (void)W;
}

/* ---------------- cargador ---------------- */
static void set_mag_hidden(PistState *s, int hidden) { s->mag_slide = hidden ? 50.0f : 0.0f; }

static void eject_mag(PistState *s, const WInput *in, const WGameBridge *g, WPose W) {
    if (!s->b.mag_inserted) return;
    s->mag_slide = 0; s->b.channel[WCH_MAG] = 0;
    WVec3 down = wquat_rotate(W.q, wv(0, -PIST_MAG_UY, -PIST_MAG_UZ));
    WVec3 spin = wquat_rotate(W.q, wv(1, 0, 0));
    /* el cargador montado cuelga del marco (pieza raíz sin canal): pose = arma · reposo de la pieza "mag" */
    drop_free(s, H_MAG, wpose_mul(W, k_parts[P_MAG].rest), wv_scale(down, 0.5f), wv_scale(spin, 1.5f), 3.0f);
    s->b.held[H_MAG].mesh = PIST_MESH_MAG;
    take(g, -s->b.mag_rounds);             /* las balas del cargador soltado vuelven a la reserva */
    s->b.mag_inserted = 0; s->b.mag_rounds = 0;
    set_mag_hidden(s, 1);
    snd(g, WS_MAG_OUT); hap(g, 0, 0.35f, 0.04f);
    (void)in;
}

static int in_belt(const WInput *in) {
    WVec3 d = wv_sub(in->hand[1].p, in->head.p);
    float h = sqrtf(d.x * d.x + d.z * d.z);
    return h < 0.45f && d.y < -0.40f && d.y > -1.10f;
}

/* Encaja un cargador nuevo. Si ya hay uno montado no se pisa (se descartaría su cuenta de balas): el
 * cargador libre simplemente desaparece sin tocar el contador (sus balas aún no se habían descontado). */
static void seat_mag(PistState *s, const WGameBridge *g) {
    if (s->b.mag_inserted) { kill_free(s, H_MAG); return; }
    int n = reserve(g); if (n > s->b.def->mag_capacity) n = s->b.def->mag_capacity;
    if (n < 0) n = 0;
    take(g, n);
    s->b.mag_inserted = 1; s->b.mag_rounds = n;
    set_mag_hidden(s, 0);
    kill_free(s, H_MAG);
    snd(g, WS_MAG_IN); hap(g, 0, 0.5f, 0.05f); hap(g, 1, 0.6f, 0.06f);
}

/* ---------------- corredera ---------------- */
static void chamber_round(PistState *s) {
    if (!s->b.chambered && s->b.mag_inserted && s->b.mag_rounds > 0) { s->b.mag_rounds--; s->b.chambered = 1; }
}

static void eject_live_round(PistState *s, const WGameBridge *g, WPose W) {
    if (!s->b.chambered) return;
    s->b.chambered = 0;
    take(g, -1);                           /* cartucho expulsado: vuelve a la reserva */
    WPose at = {{0.012f, 0.072f, PIST_EJECT_Z}, {0, 0, 0, 1}};
    WPose wp = wpose_mul(W, at);
    WVec3 v = wquat_rotate(W.q, wv(1.6f, 1.3f, 0.4f));
    wp.q = W.q;
    drop_free(s, H_ROUND, wp, v, wquat_rotate(W.q, wv(0, 18, 0)), 2.5f);
    s->b.held[H_ROUND].mesh = PIST_MESH_ROUND;
}

static void eject_case(PistState *s, const WInput *in, WPose W) {
    WPose at = {{PIST_EJECT_X, 0.070f, PIST_EJECT_Z}, W.q};
    at.q = wquat_identity();
    WPose wp = wpose_mul(W, at);
    float a = wcore_rand(&s->seed), b = wcore_rand(&s->seed), c = wcore_rand(&s->seed);
    WVec3 v = wquat_rotate(W.q, wv(2.0f + 0.4f * a, 1.2f + 0.3f * b, 0.7f + 0.3f * c));
    WVec3 w = wquat_rotate(W.q, wv(6 * c, 22 + 8 * a, 9 * b));
    drop_free(s, H_CASE, wp, v, w, 2.0f);
    s->b.held[H_CASE].mesh = PIST_MESH_CASE;
    (void)in;
}

static void start_fire_cycle(PistState *s) { s->cyc = 1; s->cyc_t = 0; s->peaked = 0; s->fed = 0; }

static void release_slide(PistState *s) {   /* soltar: vuelve hacia delante y alimenta */
    s->locked = 0; s->racked = 1; s->charging = 0;
}

/* ---------------- actualización ---------------- */
int w_pist_trig_armed(const WeaponState *ws) { return ((const PistState *)ws)->trig_armed; }   /* diagnóstico (vrgame): solo para el estado de la pistola */

static void pist_init(WeaponState *ws) {
    wcore_state_reset(ws, &w_pist);          /* estado entero a cero, binding conservado */
    PistState *s = (PistState *)ws;
    /* sin cargador hasta la primera actualización: ahí se descuentan sus balas del contador del juego */
    s->hammer_cocked = 1; s->hammer = 1; s->trig_armed = 0; s->seed = 0xC0FFEEu;
    set_mag_hidden(s, 1);
    ws->held[H_MAG].mesh = PIST_MESH_MAG; ws->held[H_CASE].mesh = PIST_MESH_CASE;
    ws->held[H_ROUND].mesh = PIST_MESH_ROUND;
}

static void pist_update(WeaponState *ws, const WInput *in, WReloadMode rm, const WGameBridge *g) {
    PistState *s = (PistState *)ws;
    float dt = wcore_dt(in->dt, 0.1f);
    if (dt <= 0) return;                       /* dt NaN, 0 o negativo: cuadro inválido */
    if (!s->booted) {                          /* cargador completo + bala en la recámara, del contador del juego */
        s->booted = 1;
        int n = reserve(g), cap1 = ws->def->mag_capacity + 1;
        if (n > cap1) n = cap1;
        if (n > 0) {
            take(g, n);
            ws->mag_inserted = 1; ws->chambered = 1; ws->mag_rounds = n - 1;
            set_mag_hidden(s, 0);
        }
    }
    wcore_recoil_step(ws, in, dt);
    WPose W = wcore_weapon_world(ws, in), Wi = wpose_inv(W);
    float floor_y = in->head.p.y - 1.65f;
    uint32_t b0 = in->buttons[0];
    float tr = in->trigger[0], g1 = in->grip[1];
    int g1_press = (s->pgrip[1] < 0.5f && g1 >= 0.7f);
    WVec3 h1w = in->hand[1].p, h1l = wpose_apply(Wi, h1w);
    int free_busy = 0;
    int supp = (in->game_flags & WGF_SUPP) != 0;   /* supresor puesto según el juego (la capa de integración lo rellena) */

    /* ---- gatillo / disparo ---- */
    if (tr < 0.3f) s->trig_armed = 1;       /* se rearma al soltar por debajo de 0,3 */
    if (tr > 0.5f && s->trig_armed) {
        s->trig_armed = 0;
        if (!s->cyc && !s->locked && !s->charging && s->slide < 0.002f) {
            if (s->b.chambered && s->hammer_cocked) {
                WPose mz = wpose_mul(W, ws->def->points[WP_MUZZLE]);
                if (!supp) { WPose loc = ws->def->points[WP_MUZZLE]; loc.p.z = PIST_MUZZLE_Z; mz = wpose_mul(W, loc); }
                s->b.chambered = 0; s->hammer_cocked = 0; s->shots++;
                if (g && g->fire) g->fire(PIST_ID, mz);
                snd(g, WS_FIRE);
                wcore_kick(ws, in, s->seed ^ (uint32_t)(s->shots * 2654435761u));
                hap(g, 0, supp ? 0.7f : 1.0f, 0.06f);
                if (in->support_held) hap(g, 1, 0.45f, 0.05f);
                start_fire_cycle(s);
            } else {
                s->hammer_cocked = 0; snd(g, WS_DRY); hap(g, 0, 0.15f, 0.02f);
            }
        }
    }

    /* ---- ciclo de la corredera tras disparo ---- */
    if (s->cyc) {
        s->cyc_t += dt;
        if (!s->peaked && s->cyc_t >= CYC_BACK) {
            s->peaked = 1; s->hammer_cocked = 1;
            eject_case(s, in, W); snd(g, WS_SLIDE_BACK);
            if (!(s->b.mag_inserted && s->b.mag_rounds > 0) && s->b.mag_inserted) {   /* cargador vacío: retén */
                s->locked = 1; s->cyc = 0; s->slide = TRAVEL;
            }
        }
        if (s->cyc) {
            if (s->cyc_t < CYC_BACK) { float u = s->cyc_t / CYC_BACK; s->slide = TRAVEL * (1 - (1 - u) * (1 - u)); }
            else {
                float u = (s->cyc_t - CYC_BACK) / (CYC_TOTAL - CYC_BACK); s->slide = TRAVEL * (1 - smooth(u));
                if (!s->fed && u > 0.45f) { s->fed = 1; chamber_round(s); }
            }
            if (s->cyc_t >= CYC_TOTAL) { s->cyc = 0; s->slide = 0; snd(g, WS_SLIDE_FWD); hap(g, 0, 0.2f, 0.02f); }
        }
    }

    /* ---- botones del arma ---- */
    if (wcore_edge_down(s->pbtn[0], b0, WB_B | WB_MAG_RELEASE) && s->b.mag_inserted) eject_mag(s, in, g, W);
    if (wcore_edge_down(s->pbtn[0], b0, WB_A) && s->locked) { release_slide(s); }

    /* ---- tirar de la corredera con la mano libre ---- */
    {
        WPose cp = ws->def->points[WP_CHARGE];
        float dc = wv_len(wv_sub(h1l, cp.p));
        if (!s->charging && g1_press && dc < 0.07f && !in->support_held && s->fr[H_MAG].mode != FM_HAND && !s->cyc) {
            s->charging = 1; s->racked = 0;
            s->charge_ref_z = h1l.z; s->charge_slide0 = s->slide;
            s->locked = 0;
        }
        if (s->charging) {
            free_busy = 1;
            if (g1 < 0.3f || dc > 0.20f) {
                s->charging = 0;
                if (s->racked && s->b.mag_inserted && s->b.mag_rounds == 0 && !s->b.chambered && s->slide >= 0.9f * TRAVEL) {
                    s->locked = 1; s->slide = TRAVEL;     /* cargador vacío: se queda abierta */
                }
            } else {
                float ns = wcore_clampf(s->charge_slide0 + (h1l.z - s->charge_ref_z), 0, TRAVEL);
                if (ns >= 0.9f * TRAVEL && !s->racked) {
                    s->racked = 1; eject_live_round(s, g, W); s->hammer_cocked = 1; snd(g, WS_SLIDE_BACK); hap(g, 1, 0.3f, 0.03f);
                }
                s->slide = ns;
            }
        } else if (!s->cyc && !s->locked) {
            /* retorno por resorte */
            if (s->slide > 0) {
                float prev = s->slide;
                s->slide = wcore_approach(s->slide, 0, RET_SPEED * dt);
                if (s->racked && prev > 0.5f * TRAVEL && s->slide <= 0.5f * TRAVEL) chamber_round(s);
                if (s->slide <= 0) {
                    if (s->racked) { chamber_round(s); s->racked = 0; snd(g, WS_SLIDE_FWD); hap(g, 0, 0.25f, 0.03f); hap(g, 1, 0.25f, 0.03f); }
                }
            }
        }
        if (s->locked && !s->cyc && !s->charging) s->slide = TRAVEL;
    }

    /* ---- liberar corredera tras encajar (modos asistidos / pulsando A) ---- */
    if (s->rel_pending) {
        s->rel_t -= dt;
        if (s->rel_t <= 0) { s->rel_pending = 0; if (s->locked) release_slide(s); else if (!s->b.chambered) { s->racked = 1; s->slide = s->slide > 0 ? s->slide : 0.0001f; } }
    }

    /* ---- cargador nuevo ---- */
    Free *fm = &s->fr[H_MAG];
    WeaponState *hd = ws;
    if (fm->mode == FM_NONE || fm->mode == FM_REST || fm->mode == FM_FALL) {
        /* tomar uno del cinturón con la mano libre */
        if (g1_press && in_belt(in) && !in->support_held && !s->charging && !free_busy) {
            if (reserve(g) > 0) {
                fm->mode = FM_HAND; fm->rel = mag_hand_rel(); hd->held[H_MAG].mesh = PIST_MESH_MAG;
                hd->held[H_MAG].active = 1; hd->held[H_MAG].world = wpose_mul(in->hand[1], fm->rel);
                hap(g, 1, 0.35f, 0.04f);
            } else hap(g, 1, 0.15f, 0.08f);
        }
    }
    if (fm->mode == FM_HAND || fm->mode == FM_GUIDE) {
        free_busy = 1;
        if (g1 < 0.3f) {
            /* soltado: cae */
            WPose w0 = hd->held[H_MAG].world;
            drop_free(s, H_MAG, w0, wv(0, 0, 0), wv(0, 0, 0), 2.0f);
        } else {
            WPose hw = wpose_mul(in->hand[1], fm->rel);
            WVec3 uw = wv_norm(wquat_rotate(Wi.q, wquat_rotate(hw.q, wv(0, 1, 0))));
            WVec3 wa = mag_axis_w();
            WVec3 cw = wpose_apply(Wi, hw.p);
            WVec3 T = wv_add(cw, wv_scale(uw, PIST_MAG_HALF));
            WVec3 E = wv(0, PIST_WELL_Y, PIST_WELL_Z);
            WVec3 r = wv_sub(T, E);
            float sd = wv_dot(r, wa);
            WVec3 lat = wv_sub(r, wv_scale(wa, sd));
            float ang = acosf(wcore_clampf(wv_dot(uw, wa), -1, 1));
            if (rm == WR_MANUAL || rm == WR_AUTOMATIC) {
                int ok = !s->b.mag_inserted && ang < 0.61f && wv_len(lat) < 0.028f && sd > -0.004f && sd < SEAT + 0.05f;
                if (ok) {
                    float k = wcore_clampf(sd / 0.03f, 0, 1);
                    float sc = sd > SEAT ? SEAT : sd;
                    WVec3 top = wv_add(wv_add(E, wv_scale(wa, sc)), wv_scale(lat, 1 - k));
                    WQuat qs = wquat_slerp(wquat_mul(Wi.q, hw.q), mag_seat_rel().q, k);
                    WVec3 u_eff = wquat_rotate(qs, wv(0, 1, 0));
                    WPose rel = {wv_sub(top, wv_scale(u_eff, PIST_MAG_HALF)), qs};
                    hd->held[H_MAG].world = wpose_mul(W, rel);
                    fm->mode = FM_GUIDE;
                    if (sd >= SEAT - 0.004f) { seat_mag(s, g); s->rel_pending = 0; }
                } else {
                    fm->mode = FM_HAND; hd->held[H_MAG].world = hw;
                }
            } else {                     /* simplificado: gesto hacia el pozo */
                hd->held[H_MAG].world = hw;
                WPose wp = wpose_mul(W, (WPose){{0, PIST_WELL_Y, PIST_WELL_Z}, {0, 0, 0, 1}});
                if (wv_len(wv_sub(hw.p, wp.p)) < 0.10f) {
                    if (s->b.mag_inserted) eject_mag(s, in, g, W);
                    fm->mode = FM_ANIM; fm->t = 0; fm->dur = 0.18f; fm->rel = wpose_mul(Wi, hw);
                    hd->held[H_MAG].mesh = PIST_MESH_MAG;
                }
            }
        }
    }
    if (fm->mode == FM_ANIM) {
        free_busy = 1;
        fm->t += dt;
        float k = smooth(fm->t / fm->dur);
        WPose rel = wpose_lerp(fm->rel, mag_seat_rel(), k);
        hd->held[H_MAG].world = wpose_mul(W, rel); hd->held[H_MAG].active = 1;
        if (fm->t >= fm->dur) {
            int ya = s->b.mag_inserted;            /* otro cargador montado a mano durante la animación: no se pisa */
            seat_mag(s, g);
            if (!ya) { s->rel_pending = 1; s->rel_t = 0.12f; }
        }
    }

    /* ---- recarga automática tras vaciar ---- */
    if (rm != WR_AUTOMATIC) s->auto_stage = 0;      /* cambio de modo a mitad: se abandona la secuencia */
    if (rm == WR_AUTOMATIC) {
        int fm_libre = fm->mode == FM_NONE || fm->mode == FM_REST || fm->mode == FM_FALL;
        int empty = s->b.mag_inserted && s->b.mag_rounds == 0 && !s->b.chambered && !s->cyc && !s->charging;
        int sin_cargador = !s->b.mag_inserted && !s->charging;      /* p.ej. se cambió de modo a mitad de la secuencia */
        if (s->auto_stage == 0 && reserve(g) > 0 && fm_libre) {
            if (empty) { s->auto_stage = 1; s->auto_t = 0; }
            else if (sin_cargador) { s->auto_stage = 2; s->auto_t = 0; }
        }
        if (s->auto_stage) {
            s->auto_t += dt;
            if (s->auto_stage == 1 && s->auto_t > 0.35f) {
                if (!s->b.mag_inserted || s->b.mag_rounds > 0 || s->b.chambered) s->auto_stage = 0;   /* ya no hace falta */
                else { eject_mag(s, in, g, W); s->auto_stage = 2; s->auto_t = 0; }
            } else if (s->auto_stage == 2 && s->auto_t > 0.45f) {
                if (s->b.mag_inserted || !fm_libre) s->auto_stage = 0;          /* montaron uno a mano / lo llevan en la mano */
                else if (reserve(g) > 0) {
                    WPose start = {{0.10f, -0.55f, 0.15f}, mag_hand_rel().q};
                    start.p = wpose_apply(Wi, wv_add(in->head.p, wquat_rotate(in->head.q, wv(0.12f, -0.62f, -0.10f))));
                    fm->mode = FM_ANIM; fm->t = 0; fm->dur = 0.40f; fm->rel = start;
                    hd->held[H_MAG].mesh = PIST_MESH_MAG; hd->held[H_MAG].active = 1;
                    s->auto_stage = 3; s->auto_t = 0;
                } else s->auto_stage = 0;
            } else if (s->auto_stage == 3 && fm->mode != FM_ANIM) { s->auto_stage = 0; }
        }
    }

    /* ---- física de los objetos sueltos ---- */
    for (int i = 0; i < W_MAX_HELD; i++) step_free(s, i, dt, floor_y, in, W);

    /* ---- martillo ---- */
    {
        float tgt = s->hammer_cocked ? 1.0f : 0.0f;
        float rate = s->hammer_cocked ? 40.0f : 70.0f;
        s->hammer = wcore_approach(s->hammer, tgt, rate * dt);
    }

    /* ---- canales ---- */
    ws->channel[WCH_SLIDE] = s->slide;
    ws->channel[WCH_TRIGGER] = wcore_clampf(tr, 0, 1);
    ws->channel[WCH_HAMMER] = s->hammer;
    ws->channel[WCH_MAG] = s->mag_slide;
    ws->channel[WCH_ROUND] = ws->chambered ? 0.0f : 1.0f;
    ws->channel[WCH_CUSTOM0] = supp ? 0.0f : 1.0f;
    ws->phase = s->cyc ? PH_CYCLING : s->charging ? PH_CHARGING : s->locked ? PH_LOCKED : !ws->mag_inserted ? PH_NOMAG : PH_NORMAL;
    ws->t = s->cyc ? s->cyc_t : 0;

    s->pbtn[0] = in->buttons[0]; s->pbtn[1] = in->buttons[1];
    s->pgrip[0] = in->grip[0]; s->pgrip[1] = in->grip[1];
}

const WeaponDef w_pist = {
    .id = "pist", .game_weapon_id = PIST_ID,
    .meshes = NULL, .nmeshes = 0, .mesh_names = pist_mesh_names,
    .parts = k_parts, .nparts = P_COUNT,
    .points = {
        [WP_GRIP]    = {{0, 0, 0}, {0, 0, 0, 1}},
        [WP_SUPPORT] = {{0, PIST_SUP_Y, PIST_SUP_Z}, {0, 0, 0, 1}},   /* mano libre: parte delantera/inferior de la pistola (antes junto a la empuñadura, donde la otra mano no cabe) */
        [WP_MUZZLE]  = {{0, PIST_BY, PIST_SUPP_TIP_Z}, {0, 0, 0, 1}},
        [WP_EYE]     = {{0, 0.0875f, PIST_EYE_Z}, {0, 0, 0, 1}},
        [WP_MAGWELL] = {{0, PIST_WELL_Y, PIST_WELL_Z}, {PIST_MAG_QX, 0, 0, PIST_MAG_QW}},
        [WP_CHARGE]  = {{0, PIST_BY, PIST_CHARGE_Z}, {0, 0, 0, 1}},
        [WP_SCREEN]  = {{0, 0, 0}, {0, 0, 0, 1}},
    },
    /* k=200 (w=14 rad/s), c=24 -> zeta~0,85: pico ~2,5 cm / ~7 grados, vuelve en ~0,3 s sin oscilar */
    .recoil = {.kick_back = 0.9f, .kick_up = 4.6f, .kick_yaw_rand = 0.8f, .stiffness = 200.0f, .damping = 24.0f,
               .two_hand_factor = 0.55f},
    .mode = WF_SEMI, .rpm = 360, .mag_capacity = 12,
    .state_size = sizeof(PistState), .init = pist_init, .update = pist_update,
};
