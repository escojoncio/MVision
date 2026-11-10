/* vr_fire: ver vr_fire.h. Núcleo sin dependencias del runtime (se prueba con RAM sintética). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "vr_fire.h"
#include "vr_camera.h"          /* solo VrCamRecord (vrfire_map_from_cam) */
#include "bridge_addrs.h"

#define PI_F 3.14159265358979f
#define RAM_MASK 0x1FFFFFu
#define NQ 4

/* ---- direcciones/offsets del juego (edición europea; documentados en vr_fire_names.json) ---- */
#define PW_FROM_CTRL    0x20u        /* bloque del jugador = puntero de control - 0x20 */
#define PW_WEAPON_ACTOR 0x904u       /* actor del arma equipada (0 = ninguna) */
#define PW_TRIGGER      0x910u       /* banderas que lee el actor del arma (bit 0: levantada/lista, bit 1: disparar) */
#define PW_TYPE_TBL     0x91Cu       /* banderas del tipo de arma (bit 0: dispara mientras se mantiene) */
#define PW_WEAPON_IDX   0x918u       /* índice de arma del jugador (id+1 en la tabla de creadores; -1 ninguna) */
#define WA_OBJS         0x20u        /* actor de arma: lista de modelos del modelo */
#define OBJS_NMODELS    0x2Eu
#define OBJS_FIRST      0x48u
#define OBJ_SIZE        0x5Cu
#define OBJ_BOUND       0x4Cu
#define MAX_MODELS      64
#define WA_LASER_PRIM   0x58u        /* pistola: primitiva del haz */
#define WA_LIGHT_PRIM   0x10Cu       /* pistola: primitiva del punto */
#define PRIM_TYPE       0x24u        /* primitiva: tipo (bits) */
#define PRIM_INVISIBLE  0x100
/* actor de la granada (0x8006af98 lo crea; paso 0x8006a8f0): objeto en +0x20 (posición s16[3] en su inicio; +0x28 banderas, +0x2C grupo, +0x2E nº de piezas), mecha (ticks) en +0x100, velocidad por tick s16[3] en +0x108 */
#define GA_SIZE 0x124u
#define GA_OBJ 0x20u
#define GA_FUSE 0x100u
#define GA_VEL 0x108u
#define OBJ_FLAG 0x28u
#define OBJ_GID 0x2Cu
#define OBJ_HIDDEN_FLAG 0x80u       /* OF_HIDDEN del extractor nativo (ns_extract.c) */
#define CHAN_MAIN 0x800B2B7Cu       /* canal principal: nº de objetos en +0x56 (s16), cola en +0x58 */
#define A_GROUP 0x800ACAF8u         /* grupo activo (máscara) */


/* Puntero del invitado válido: KSEG0 dentro de los 2 MB de RAM (no los espejos por encima de 0x80200000) y n bytes caben. */
static int ptr_ok(uint32_t a, uint32_t n) { return a >= 0x80010000u && a < 0x80200000u && n <= 0x200000u && (a & RAM_MASK) + n <= RAM_MASK + 1u; }
static uint32_t rd_u32(const uint8_t *r, uint32_t a) { uint32_t v; memcpy(&v, r + (a & RAM_MASK), 4); return v; }
static int16_t rd_s16(const uint8_t *r, uint32_t a) { int16_t v; memcpy(&v, r + (a & RAM_MASK), 2); return v; }
static void wr_s16(uint8_t *r, uint32_t a, int16_t v) { memcpy(r + (a & RAM_MASK), &v, 2); }
static void wr_s32(uint8_t *r, uint32_t a, int32_t v) { memcpy(r + (a & RAM_MASK), &v, 4); }
static int32_t rd_s32(const uint8_t *r, uint32_t a) { int32_t v; memcpy(&v, r + (a & RAM_MASK), 4); return v; }

/* ================================================== geometría ================================================== */
static void rot_t(const VrFireMap *m, const float v[3], float o[3]) {
    float th = (m->theta_play_deg + 180.0f) * (PI_F / 180.0f), cs = cosf(th), sn = sinf(th);
    o[0] = cs * v[0] + sn * v[2]; o[1] = v[1]; o[2] = -sn * v[0] + cs * v[2];
}
void vrfire_dir_to_world(const VrFireMap *m, const float d[3], float out[3]) { rot_t(m, d, out); }
void vrfire_pos_to_world(const VrFireMap *m, const float p[3], float out[3]) {
    float d[3] = {p[0] - m->p_ref[0], p[1] - m->p_ref[1], p[2] - m->p_ref[2]}, r[3];
    rot_t(m, d, r);
    for (int i = 0; i < 3; i++) out[i] = (float)m->eye[i] + r[i] * m->units_per_m;
}
/* Inversa: mundo del motor -> espacio de usuario (rotación inversa, escala 1/upm). */
static void rot_inv(const VrFireMap *m, const float v[3], float o[3]) {
    float th = (m->theta_play_deg + 180.0f) * (PI_F / 180.0f), cs = cosf(th), sn = sinf(th);
    o[0] = cs * v[0] - sn * v[2]; o[1] = v[1]; o[2] = sn * v[0] + cs * v[2];
}
void vrfire_dir_from_world(const VrFireMap *m, const float d[3], float out[3]) { rot_inv(m, d, out); }
void vrfire_pos_from_world(const VrFireMap *m, const float w[3], float out[3]) {
    float d[3] = {(w[0] - (float)m->eye[0]) / m->units_per_m, (w[1] - (float)m->eye[1]) / m->units_per_m, (w[2] - (float)m->eye[2]) / m->units_per_m}, r[3];
    rot_inv(m, d, r);
    for (int i = 0; i < 3; i++) out[i] = m->p_ref[i] + r[i];
}
static int finite3(const float v[3]) { return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]); }
static int32_t round_clamp(float v) { if (!(v == v)) return 0; if (v > 1.0e9f) v = 1.0e9f; if (v < -1.0e9f) v = -1.0e9f; return (int32_t)lrintf(v); }
static int16_t q12(float v) { float r = v * 4096.0f; if (r > 32767.0f) r = 32767.0f; if (r < -32768.0f) r = -32768.0f; return (int16_t)lrintf(r); }

int vrfire_make_matrix(const VrFireMap *map, const WPose *mz, float shift, VrFireMat *out) {
    if (!map || !map->valid || !mz || !out || !(map->units_per_m > 0)) return 0;
    memset(out, 0, sizeof *out);
    WQuat q = mz->q;
    float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(n > 1e-6f) || !isfinite(n) || !isfinite(mz->p.x) || !isfinite(mz->p.y) || !isfinite(mz->p.z)) return 0;
    q.x /= n; q.y /= n; q.z /= n; q.w /= n;
    /* ejes de la pose en espacio de usuario: delante = -Z, arriba = +Y */
    float fu[3] = {-2 * (q.x * q.z + q.w * q.y), -2 * (q.y * q.z - q.w * q.x), -(1 - 2 * (q.x * q.x + q.y * q.y))};
    float uu[3] = {2 * (q.x * q.y - q.w * q.z), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z + q.w * q.x)};
    float f[3], u[3];
    rot_t(map, fu, f); rot_t(map, uu, u);
    /* columna 0 = izquierda = arriba x delante (ortonormal: arriba es perpendicular a delante) */
    float l[3] = {u[1] * f[2] - u[2] * f[1], u[2] * f[0] - u[0] * f[2], u[0] * f[1] - u[1] * f[0]};
    float ln = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
    if (!(ln > 1e-4f)) return 0;
    for (int i = 0; i < 3; i++) l[i] /= ln;
    float c2[3] = {f[1] * l[2] - f[2] * l[1], f[2] * l[0] - f[0] * l[2], f[0] * l[1] - f[1] * l[0]};
    /* columnas: c0 = l, c1 = -f, c2 = f x l ; m[i][j] = componente i de la columna j */
    for (int i = 0; i < 3; i++) { out->m[3 * i + 0] = q12(l[i]); out->m[3 * i + 1] = q12(-f[i]); out->m[3 * i + 2] = q12(c2[i]); }
    float w[3], pp[3] = {mz->p.x, mz->p.y, mz->p.z};
    vrfire_pos_to_world(map, pp, w);
    for (int i = 0; i < 3; i++) out->t[i] = round_clamp(w[i] + shift * f[i]);
    return 1;
}
void vrfire_store_mat(uint8_t *ram, uint32_t a, const VrFireMat *m) {
    for (int i = 0; i < 9; i++) wr_s16(ram, a + 2u * (uint32_t)i, m->m[i]);
    wr_s16(ram, a + 18, 0);
    for (int i = 0; i < 3; i++) wr_s32(ram, a + 20 + 4u * (uint32_t)i, m->t[i]);
}
void vrfire_load_mat(const uint8_t *ram, uint32_t a, VrFireMat *m) {
    for (int i = 0; i < 9; i++) m->m[i] = rd_s16(ram, a + 2u * (uint32_t)i);
    for (int i = 0; i < 3; i++) m->t[i] = rd_s32(ram, a + 20 + 4u * (uint32_t)i);
}
void vrfire_mat_forward(const VrFireMat *m, float out[3]) { for (int i = 0; i < 3; i++) out[i] = -(float)m->m[3 * i + 1] / 4096.0f; }

void vrfire_map_from_cam(VrFireMap *m, const struct VrCamRecord *r) {
    memset(m, 0, sizeof *m);
    if (!r || !r->active || !(r->units_per_m > 0)) return;
    m->valid = 1;
    for (int i = 0; i < 3; i++) { m->eye[i] = r->eye[i] - round_clamp(r->cam_off[i]); m->p_ref[i] = r->p_ref[i]; }   /* ojo de primera persona: en la tercera persona de la pared la cámara va desplazada (cam_off) pero manos y disparo siguen en la cabeza del personaje */
    m->theta_play_deg = r->theta_play_deg; m->units_per_m = r->units_per_m;
}

/* ================================================== estado del juego ================================================== */
void vrfire_read_game(const uint8_t *ram, const BridgeState *bs, VrFireGame *g) {
    memset(g, 0, sizeof *g);
    g->weapon = -1;
    if (!ram || !bs) return;
    g->weapon = bs->weapon; g->mag = bs->magazine; g->mag_max = bs->magazine_max;
    if (g->weapon >= 0 && g->weapon < BRIDGE_N_WEAPONS) g->total = bs->weapon_ammo[g->weapon];
    uint32_t pc = rd_u32(ram, BA_PLAYER_CTRL);
    if (!ptr_ok(pc, 0x20 + 0x9C0)) { g->ok = 0; return; }
    uint32_t w = pc - PW_FROM_CTRL;
    g->armed = (rd_u32(ram, w + PW_TRIGGER) & 1u) != 0;
    g->hold_fires = (int)(rd_u32(ram, w + PW_TYPE_TBL) & 1u);
    g->ok = 1;
}

/* ================================================== pulsación ================================================== */
typedef struct { WPose pose; VrFireMap map; int age; int life; } Req;
enum { P_IDLE, P_HOLD, P_REL };
static struct {
    VrFireCfg cfg;
    Req q[NQ]; int nq;
    int infl[NQ], ninfl;                 /* liberaciones que aún no han producido proyectil (edad) */
    int infl_life;                       /* vida de esas liberaciones (INFL_LIFE; más si hay una petición de vida larga en curso) */
    int phase, tphase;
    int last_raise;                      /* el último VBlank activo trataba el arma como «mantener levantada» (granada, visor): si se pierde el seguimiento con el botón mantenido, lo que suelte el juego es un desarmado */
    int disarm_t;                        /* VBlanks restantes en que un proyectil sin petición se suprime */
    int auto_tail;                       /* VBlanks tras soltar el gatillo en que aún salen balas del juego con la pose viva */
    WPose live; VrFireMap live_map; int live_ok, auto_live;
    VrFireMat last;
    VrFireStats st;
    int flash_age;                       /* VBlanks desde la última bala sustituida (el resplandor solo la acompaña si es reciente) */
    float tpos[3], tvel[3]; VrFireMap tmap; int tok, tok_age;
    float ttick_s;                       /* tick del motor (s) con el que se pasó la velocidad: tope de la extrapolación hasta la creación */
    float org[3]; int org_ok, org_age;   /* posición (motor) del objeto de la mano en la última escena nativa y VBlanks desde entonces (vrfire_set_throw_origin) */
    float body[3]; int body_ok, body_wall;   /* cuerpo del jugador y si hay pared entre él y la mano, para validar el próximo origen (vrfire_set_throw_body) */
    int born_hand, born_age;             /* diagnóstico: el último actor nació en la mano (1) o con la trayectoria adelantada (0) y tras cuántos VBlanks desde soltar */
    uint32_t fl_obj; int fl_wait, fl_hide;   /* granada lanzada: objeto del actor; VBlanks que aún se espera verlo en la escena; VBlanks que aún se oculta el modelo de la mano */
} S;

#define INFL_LIFE 14
#define AUTO_TAIL 6
#define DISARM_WINDOW 60
#define FLASH_LIFE 3             /* el resplandor se pide en el mismo tick que la bala; pasado esto es de otro (p. ej. un enemigo) */
/* Gravedad de la granada del juego: el paso del actor (0x8006a8f0) resta 16 a la velocidad vertical (unidades/tick) en cada tick con el objeto en el aire.
 * Con 1000 unidades/m y 0,04 s/tick son 10,0 m/s^2 (la real, 9,81: coherente). Se usa también para la parte del vuelo anterior a la creación del actor. */
#define VRFIRE_GAME_G 16.0f
#define ORG_REACH_M 1.5f         /* el origen en la mano no vale si dista más de esto del cuerpo (horizontal) */
#define ORG_LIFE 6               /* el origen en la mano vale si la última escena es de hace como mucho esto (las escenas llegan a 25 por s: una cada 2 VBlanks) */
#define FLIER_WAIT 75            /* VBlanks que se espera a ver el volador en la escena antes de dejar la mano como está */
#define HIDE_VBL 15              /* VBlanks (0,3 s) que se oculta el modelo de la mano desde que el volador está en la escena */
#define THROW_LIFE 90            /* datos de lanzamiento no consumidos caducan (la granada sale ~16 VBlanks tras soltar) */

void vrfire_cfg_default(VrFireCfg *c) {
    memset(c, 0, sizeof *c);
    c->arm_mode = VRFIRE_ARM_ON_TRIGGER; c->press_vbl = 2; c->release_vbl = 2; c->req_window = 60; c->arm_timeout = 40;
    c->bullet_shift = 400.0f; c->flash_shift_a = 130.0f; c->suppress_disarm = 1;
}
void vrfire_init(const VrFireCfg *c) {
    memset(&S, 0, sizeof S);
    S.infl_life = INFL_LIFE;
    if (c) S.cfg = *c; else vrfire_cfg_default(&S.cfg);
    if (S.cfg.press_vbl < 2) S.cfg.press_vbl = 2;
    if (S.cfg.release_vbl < 2) S.cfg.release_vbl = 2;
    if (S.cfg.req_window < 8) S.cfg.req_window = 8;
    if (S.cfg.arm_timeout < 4) S.cfg.arm_timeout = 4;
}
const VrFireCfg *vrfire_config(void) { return &S.cfg; }
void vrfire_stats(VrFireStats *s) { *s = S.st; }
static VrFireProbe g_probe;
void vrfire_set_probe(VrFireProbe f) { g_probe = f; }
/* Último actor de misil creado (kind 1) y cuántos van: lo usa vrgame para seguir el misil guiado (el puntero se recicla: ver vr_missile.h). */
static uint32_t g_last_missile, g_missile_serial;
uint32_t vrfire_last_missile(uint32_t *serial) { if (serial) *serial = g_missile_serial; return g_last_missile; }
static struct { int on; uint32_t actor; float t, next; int lines; } GT;     /* seguimiento diagnóstico de la granada lanzada (vrfire_grenade_track) */
#define GT_PERIOD_S 0.25f
#define GT_SPAN_S 3.0f
void vrfire_probe(int kind, int decision, uint32_t result, const VrFireMat *m) {
    if (kind == 1 && result) { g_last_missile = result; g_missile_serial++; }
    if (kind == 3 && ptr_ok(result, GA_SIZE)) { GT.on = 1; GT.actor = result; GT.t = 0; GT.next = GT_PERIOD_S; S.fl_obj = result + GA_OBJ; S.fl_wait = FLIER_WAIT; S.fl_hide = 0; }
    if (g_probe) g_probe(kind, decision, result, m);
}
const VrFireMat *vrfire_last_subst(void) { return &S.last; }

static void pop_req(void) { if (S.nq > 0) { memmove(&S.q[0], &S.q[1], sizeof(Req) * (size_t)(S.nq - 1)); S.nq--; } }
static void pop_infl(void) { if (S.ninfl > 0) { memmove(&S.infl[0], &S.infl[1], sizeof(int) * (size_t)(S.ninfl - 1)); S.ninfl--; } }

static void reset_machine(void) { S.nq = 0; S.ninfl = 0; S.infl_life = INFL_LIFE; S.phase = P_IDLE; S.tphase = 0; S.auto_live = 0; }

void vrfire_frame(const VrFireGame *g, const VrFireIn *in, VrFireOut *out) {
    out->square = 0; out->engaged = 0;
    if (S.disarm_t > 0) S.disarm_t--;
    if (S.auto_tail > 0) S.auto_tail--;
    if (S.flash_age < 1000000) S.flash_age++;
    if (S.tok && ++S.tok_age > THROW_LIFE) S.tok = 0;
    if (S.org_ok && ++S.org_age > ORG_LIFE) S.org_ok = 0;
    if (S.fl_wait > 0) S.fl_wait--;
    if (S.fl_hide > 0) S.fl_hide--;
    if (!in || !g || !g->ok || !in->active || !in->map || !in->map->valid) {
        if (S.live_ok && S.phase == P_HOLD && S.nq == 0 && S.last_raise) S.disarm_t = DISARM_WINDOW;   /* seguimiento perdido/arma fuera con el botón mantenido: el juego soltará sin petición */
        S.live_ok = 0; reset_machine();
        return;
    }
    S.live = in->muzzle; S.live_map = *in->map; S.live_ok = 1; S.last_raise = in->raise;
    for (int i = 0; i < S.nq;) {
        if (++S.q[i].age > (S.q[i].life > S.cfg.req_window ? S.q[i].life : S.cfg.req_window)) { memmove(&S.q[i], &S.q[i + 1], sizeof(Req) * (size_t)(S.nq - i - 1)); S.nq--; S.st.expired++; }
        else i++;
    }
    for (int i = 0; i < S.ninfl;) {
        if (++S.infl[i] > S.infl_life) { memmove(&S.infl[i], &S.infl[i + 1], sizeof(int) * (size_t)(S.ninfl - i - 1)); S.ninfl--; }
        else i++;
    }
    if (S.nq == 0 && S.ninfl == 0) S.infl_life = INFL_LIFE;
    int autoh = g->hold_fires && in->trigger_held;
    S.auto_live = autoh;
    if (autoh) S.auto_tail = AUTO_TAIL;
    if (in->fire && !autoh) {
        if (S.nq == NQ) { pop_req(); S.st.expired++; }
        int life = in->fire_life > 0 ? (in->fire_life > 600 ? 600 : in->fire_life) : 0;
        S.q[S.nq].pose = in->fire_pose; S.q[S.nq].map = *in->map; S.q[S.nq].age = 0; S.q[S.nq].life = life; S.nq++;
        if (life > S.infl_life) S.infl_life = life;
        S.st.requests++;
    }
    int unserved = S.nq - S.ninfl;       /* peticiones sin liberación en curso */
    int want;
    if (g->hold_fires) want = autoh || S.nq > 0;
    else want = unserved > 0 || ((S.cfg.arm_mode == VRFIRE_ARM_ON_RAISE || in->raise) && in->ready);
    S.tphase++;
    switch (S.phase) {
    case P_IDLE:
        if (want) { S.phase = P_HOLD; S.tphase = 0; S.st.presses++; out->square = 1; }
        break;
    case P_HOLD:
        out->square = 1;
        if (g->hold_fires) {
            if (!want && S.tphase >= S.cfg.press_vbl) { S.phase = P_IDLE; S.tphase = 0; S.st.releases++; out->square = 0; }
            else if (S.nq > 0 && !autoh && S.ninfl > 0 && S.tphase >= S.cfg.press_vbl) { /* semi sobre arma de mantener: ya salió una */
                S.phase = P_IDLE; S.tphase = 0; S.st.releases++; out->square = 0;
            }
        } else if (unserved > 0 && S.tphase >= S.cfg.press_vbl && (g->armed || S.tphase >= S.cfg.arm_timeout)) {
            if (!g->armed) S.st.arm_timeouts++;
            S.phase = P_REL; S.tphase = 0; S.st.releases++; out->square = 0;
            if (S.ninfl < NQ) S.infl[S.ninfl++] = 0;
        } else if (S.nq == 0 && !want && S.tphase >= S.cfg.press_vbl) {
            /* desarmar: soltar sin petición = el juego dispara un tiro sobrante, que se suprime */
            S.phase = P_REL; S.tphase = 0; S.st.releases++; out->square = 0;
            S.disarm_t = DISARM_WINDOW;
        }
        break;
    case P_REL:
        out->square = 0;
        if (S.tphase >= S.cfg.release_vbl) {
            if (want) { S.phase = P_HOLD; S.tphase = 0; S.st.presses++; out->square = 1; }
            else { S.phase = P_IDLE; S.tphase = 0; }
        }
        break;
    }
    out->engaged = 1;
}

/* ================================================== sustitutos ================================================== */
static const float KIND_SHIFT_MISSILE = 0.0f;

int vrfire_take_projectile(int kind, VrFireMat *m) {
    if (!m) return VRFIRE_PASS;
    float shift = kind == 0 ? S.cfg.bullet_shift : KIND_SHIFT_MISSILE;
    if (S.nq > 0) {
        Req r = S.q[0];
        if (!vrfire_make_matrix(&r.map, &r.pose, shift, m)) { pop_req(); pop_infl(); S.st.passed++; return VRFIRE_PASS; }
        pop_req(); pop_infl();
        S.last = *m; S.st.subst++; if (kind == 0) S.flash_age = 0;
        return VRFIRE_SUBST;
    }
    if ((S.auto_live || S.auto_tail > 0) && S.live_ok && vrfire_make_matrix(&S.live_map, &S.live, shift, m)) { S.last = *m; S.st.subst++; if (kind == 0) S.flash_age = 0; return VRFIRE_SUBST; }
    if (S.disarm_t > 0 && S.cfg.suppress_disarm) { S.st.skipped++; return VRFIRE_SKIP; }
    S.st.passed++;
    return VRFIRE_PASS;
}

/* Resplandor: tras una bala sustituida se coloca en la boca (pistola: el original suma 130 por delante de la matriz). */
int vrfire_take_flash(int style, VrFireMat *m) {
    if (!m) return 0;
    const VrFireMat *b = &S.last;
    if (S.st.subst == 0 || S.flash_age > FLASH_LIFE) return 0;
    float f[3];
    vrfire_mat_forward(b, f);
    *m = *b;
    /* t de la bala = boca + bullet_shift*delante  ->  boca = t - bullet_shift*delante */
    float k = style == 0 ? (S.cfg.bullet_shift + S.cfg.flash_shift_a) : S.cfg.bullet_shift;
    for (int i = 0; i < 3; i++) m->t[i] = round_clamp((float)b->t[i] - k * f[i]);
    return 1;
}

static int set_throw(const float pos_user[3], const float vel_user_mps[3], float tick_s) {
    if (!S.live_map.valid || !pos_user || !vel_user_mps || !(tick_s > 0)) return 0;
    float w[3], v[3];
    vrfire_pos_to_world(&S.live_map, pos_user, w);
    vrfire_dir_to_world(&S.live_map, vel_user_mps, v);
    for (int i = 0; i < 3; i++) { S.tpos[i] = w[i]; S.tvel[i] = v[i] * S.live_map.units_per_m * tick_s; }
    S.ttick_s = tick_s;
    S.tok = finite3(S.tpos) && finite3(S.tvel); S.tok_age = 0;
    return S.tok;
}
int vrfire_set_throw(const float pos_user[3], const float vel_user_mps[3], float tick_s) {
    if (!S.live_ok) return 0;
    return set_throw(pos_user, vel_user_mps, tick_s);
}
void vrfire_set_throw_body(const float body[3], int wall_between) {
    S.body_ok = body && finite3(body);
    if (!S.body_ok) return;
    for (int i = 0; i < 3; i++) S.body[i] = body[i];
    S.body_wall = wall_between != 0;
}
void vrfire_set_throw_origin(const float p[3]) {
    if (!p || !finite3(p)) return;
    if (S.body_ok) {      /* el origen no vale (pared entre el cuerpo y la mano, o demasiado lejos): se descarta también el anterior y vale la trayectoria adelantada */
        float reach = ORG_REACH_M * (S.live_map.valid ? S.live_map.units_per_m : 1000.0f), dx = p[0] - S.body[0], dz = p[2] - S.body[2];
        S.body_ok = 0;
        if (S.body_wall || dx * dx + dz * dz > reach * reach) { S.org_ok = 0; return; }
    }
    for (int i = 0; i < 3; i++) S.org[i] = p[i];
    S.org_ok = 1; S.org_age = 0;
}
uint32_t vrfire_flier_key(void) { return S.fl_wait > 0 && S.fl_obj ? 0x80000000u | (S.fl_obj & 0x1FFFFFFFu) : 0; }
void vrfire_flier_seen(void) { if (S.fl_wait > 0) { S.fl_wait = 0; S.fl_hide = HIDE_VBL; } }
int vrfire_held_hidden(void) { return S.fl_hide > 0; }
int vrfire_take_throw(int16_t pos[3], int16_t vel[3]) {
    if (S.nq > 0) { pop_req(); pop_infl(); }      /* el lanzamiento atiende a la petición pendiente (como cualquier proyectil) */
    else if (!S.tok && S.disarm_t > 0 && S.cfg.suppress_disarm && S.last_raise) {
        /* Granada preparada (botón mantenido) y soltada sin lanzar (arma fuera, seguimiento perdido): el juego la crearía con su velocidad por defecto a los pies.
         * Se cambia por una caída suave desde la última pose de la boca (velocidad 0, como la cocinada hasta el final en la mano). */
        const float z[3] = {0, 0, 0}, p[3] = {S.live.p.x, S.live.p.y, S.live.p.z};
        set_throw(p, z, 0.04f);
        S.disarm_t = 0;
    }
    if (!S.tok) { S.st.passed++; S.born_age = -1; S.born_hand = 0; return 0; }
    S.st.subst++;
    S.born_age = S.tok_age;
    if (S.org_ok) {      /* el objeto de la mano está en la escena de hace nada: el actor nace ahí con la velocidad del gesto (la gravedad del juego actúa desde ya) */
        for (int i = 0; i < 3; i++) {
            float p = S.org[i], v = S.tvel[i];
            pos[i] = (int16_t)(p > 32767 ? 32767 : p < -32768 ? -32768 : lrintf(p));
            vel[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrintf(v));
        }
        S.tok = 0; S.org_ok = 0; S.born_hand = 1;
        return 1;
    }
    S.born_hand = 0;
    /* El juego crea la granada unos 16 VBlanks después de soltar (animación de lanzar): se adelanta la posición y se descuenta la gravedad del juego
     * de ese retardo (n ticks de 2 VBlanks) para que continúe la trayectoria que llevaba la mano (VRFIRE_GAME_G sobre el eje Y del motor). */
    float n = (float)S.tok_age * 0.5f, nmax = 0.6f / S.ttick_s;
    if (n > nmax) n = nmax;
    for (int i = 0; i < 3; i++) {
        float p = S.tpos[i], v = S.tvel[i];
        p += v * n;
        if (i == 1) { p -= 0.5f * VRFIRE_GAME_G * n * n; v -= VRFIRE_GAME_G * n; }
        pos[i] = (int16_t)(p > 32767 ? 32767 : p < -32768 ? -32768 : lrintf(p));
        vel[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrintf(v));
    }
    S.tok = 0;
    return 1;
}

/* Diagnóstico de la granada ([granada], una línea por lanzamiento del juego): argumentos originales (posición y velocidad del motor) frente a los sustituidos,
 * y si la correspondencia usuario -> motor era válida. subst = 0: se llamó al original sin cambios (pos/vel sustituidos se ignoran). */
void vrfire_log_grenade(const int16_t o_pos[3], const int16_t o_vel[3], int subst, const int16_t pos[3], const int16_t vel[3]) {
    fprintf(stderr, "[granada] retardo_vbl=%d origen=%s sustituida=%d mapa_valido=%d agarre_vivo=%d peticiones=%d | original pos=(%d,%d,%d) vel=(%d,%d,%d) | ",
            S.born_age, S.born_hand ? "mano" : "adelantado", subst != 0, S.live_map.valid != 0, S.live_ok != 0, S.nq, o_pos[0], o_pos[1], o_pos[2], o_vel[0], o_vel[1], o_vel[2]);
    if (subst) fprintf(stderr, "sustituida pos=(%d,%d,%d) vel=(%d,%d,%d)\n", pos[0], pos[1], pos[2], vel[0], vel[1], vel[2]);
    else fprintf(stderr, "sin sustituir\n");
}

/* Una línea [granada] cada GT_PERIOD_S durante GT_SPAN_S tras crearse el actor de la granada: posición, velocidad y mecha del actor, y lo que decide si la escena nativa lo extrae
 * (bandera de oculto del objeto, grupo activo, presencia en la cola del canal principal). dt_s = tiempo real desde la llamada anterior. Devuelve 1 si escribió una línea. */
int vrfire_grenade_track(const uint8_t *ram, float dt_s) {
    if (!GT.on || !ram) return 0;
    if (!(dt_s > 0) || dt_s > 1.0f) dt_s = 0.02f;
    GT.t += dt_s;
    if (GT.t < GT.next) return 0;
    GT.next += GT_PERIOD_S;
    uint32_t a = GT.actor, obj = a + GA_OBJ;
    uint32_t flag = rd_u32(ram, obj + OBJ_FLAG), group = rd_u32(ram, A_GROUP);
    uint16_t gid = (uint16_t)rd_s16(ram, obj + OBJ_GID);
    int inq = 0, qn = rd_s16(ram, CHAN_MAIN + 0x56);
    uint32_t q = rd_u32(ram, CHAN_MAIN + 0x58);
    if (qn > 256) qn = 256;
    if (qn > 0 && ptr_ok(q, 4u * (uint32_t)qn)) for (int i = 0; i < qn && !inq; i++) inq = rd_u32(ram, q + 4u * (uint32_t)i) == obj;
    fprintf(stderr, "[granada] t=%.2f actor=%08x pos=(%d,%d,%d) vel=(%d,%d,%d) mecha=%d | objeto bandera=%03x oculto=%d grupo_ok=%d en_cola=%d gid=%04x grupo=%08x modelos=%d\n", GT.t, a,
            rd_s16(ram, obj), rd_s16(ram, obj + 2), rd_s16(ram, obj + 4), rd_s16(ram, a + GA_VEL), rd_s16(ram, a + GA_VEL + 2), rd_s16(ram, a + GA_VEL + 4),
            rd_s32(ram, a + GA_FUSE), flag & 0xFFFu, (flag & OBJ_HIDDEN_FLAG) != 0, gid == 0 || (gid & group) != 0, inq,
            gid, group, rd_s16(ram, obj + 0x2E));
    GT.lines++;
    if (GT.t >= GT_SPAN_S) GT.on = 0;
    return 1;
}

/* ================================================== ocultar el arma del juego ================================================== */
static int g_hide;
void vrfire_set_hide(int on) { g_hide = on != 0; }
int vrfire_hide_enabled(void) { return g_hide; }

int vrfire_hide_weapon(uint8_t *ram) {
    uint32_t pc = rd_u32(ram, BA_PLAYER_CTRL);
    if (!ptr_ok(pc, 0x20 + 0x9C0)) return 0;
    uint32_t wa = rd_u32(ram, pc - PW_FROM_CTRL + PW_WEAPON_ACTOR);
    if (!ptr_ok(wa, WA_LIGHT_PRIM + 4u)) return 0;
    uint32_t objs = rd_u32(ram, wa + WA_OBJS);
    if (!ptr_ok(objs, OBJS_FIRST)) return 0;
    int n = rd_s16(ram, objs + OBJS_NMODELS);
    if (n <= 0 || n > MAX_MODELS || !ptr_ok(objs, OBJS_FIRST + OBJ_SIZE * (uint32_t)n)) return 0;
    for (int i = 0; i < n; i++) wr_s16(ram, objs + OBJS_FIRST + OBJ_SIZE * (uint32_t)i + OBJ_BOUND, 0);
    /* Pistola: el haz y el punto del puntero láser son dos primitivas del actor que parten de la mano del modelo. */
    if (rd_s16(ram, BA_VARS + 2u * BRIDGE_V_WEAPON) == 0) {
        static const uint32_t prims[2] = {WA_LASER_PRIM, WA_LIGHT_PRIM};
        for (int i = 0; i < 2; i++) {
            uint32_t p = rd_u32(ram, wa + prims[i]);
            if (ptr_ok(p, PRIM_TYPE + 4u)) wr_s32(ram, p + PRIM_TYPE, rd_s32(ram, p + PRIM_TYPE) | PRIM_INVISIBLE);
        }
    }
    return n;
}
