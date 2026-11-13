/* vrgame: ver vrgame.h. */
#include "vrgame.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "bridge.h"
#include "cdrom.h"
#include "disc_img.h"
#include "host.h"
#include "vr_body.h"
#include "vr_camera.h"
#include "vr_aa.h"
#include "vr_fire.h"
#include "vr_hit.h"
#include "vr_glove.h"
#include "vr_grip.h"
#include "vr_hand_skin.h"
#include "vr_hand_pose.h"
#include "vr_hands.h"
#include "vr_hzd.h"
#include "vr_pose.h"
#include "vrhost.h"
#include "vr_radar.h"
#include "vr_missile.h"
#include "vr_reload.h"
#include "w_aa.h"
#include "w_guided.h"
#include "w_laser.h"
#include "w_rifle.h"
#include "w_sniper.h"
#include "w_throw.h"
#include "wcut.h"
#include "wload.h"

#define PAD_L1 (1u << 10)
#define PAD_R1 (1u << 11)
#define PAD_SQUARE (1u << 15)
#define PAD_CIRCLE (1u << 13)

/* Modelos de las armas VR en el archivo de stages del usuario (stage residente y id16 del modelo; tablas de corte en vr/weapons/wcut_tables.c).
 * game_id = índice de arma del puente (0 pistola, 1 fusil, 2 granada, 3 misil guiado, 4 antiaéreo, 5 mina, 6 adhesivo, 7 aturdidora, 8 señuelo, 9 francotirador).
 * Granadas (decompilación, foxdieteam grenade.c): la de fragmentación (2) es grenade = 0x3b88; la aturdidora (7) y el señuelo (8) son can_gren = 0x7a64. La ranura de las granadas solo lleva la lógica (gesto, mano,
 * trayectoria) y el modelo que se ve es el del propio juego (vrgame_weapon_uses_game_model). */
#define WPN_STAGE "init"
typedef struct { int game_id; uint16_t model; } WpnModel;
static const WpnModel WPN_TAB[] = {
    {0, 0x36f6}, {1, 0x4725}, {9, 0x2d3b}, {3, 0x15a9}, {4, 0x57f8}, {2, 0x3b88}, {8, 0x7a64}, {6, 0xf83d}, {5, 0x17a1}, {7, 0x7a64},
};
#define NSLOT ((int)(sizeof WPN_TAB / sizeof WPN_TAB[0]))
/* Modelo del arma de id de juego dado (id16 del modelo en WPN_STAGE); 0 = sin modelo VR: se queda el arma del propio juego. Las granadas (2, 7, 8) devuelven el modelo de grenade.c pero se dibuja el del juego (ranura solo de lógica). */
uint16_t vrgame_weapon_model(int game_id) {
    for (int i = 0; i < NSLOT; i++) if (WPN_TAB[i].game_id == game_id) return WPN_TAB[i].model;
    return 0;
}
static _Atomic int native_hand = -1;     /* lado de la mano que sujeta el arma que se dibuja con el modelo del juego (vrgame_weapon_uses_game_model) mientras está equipada; -1 = ninguna */
static int is_grenade(int game_id) { return game_id == 2 || game_id == 7 || game_id == 8; }
/* Granadas: no hay tabla de corte ni textura VR fiable para sus modelos (wcut_tables.c: 0x761a «dudosa», 0x7a64 sin entrada; el antiguo de la 8 salía blanco), así que se dibuja el modelo real del
 * propio juego (escena nativa, sin ocultarlo) y la lógica VR (gesto, trayectoria) sigue; el lanzado es siempre el actor del juego. Así la mano y el lanzado son el mismo modelo, uno solo. */
int vrgame_weapon_uses_game_model(int game_id) { return is_grenade(game_id); }

/* Reloj inyectable para las pruebas (vrgame_test_clock): por defecto, el monótono real. Con la inyección, el tiempo solo avanza con vrgame_test_advance (sin depender de la carga de la máquina). */
static _Atomic int clk_fake;
static _Atomic uint64_t clk_fake_ns;
static uint64_t mono_ns(void) {
    if (atomic_load_explicit(&clk_fake, memory_order_acquire)) return atomic_load_explicit(&clk_fake_ns, memory_order_relaxed);
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* ============================================================================================================== matemática */
static void q_mul(const float a[4], const float b[4], float o[4]) {
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
static void q_norm(float q[4]) {
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(l > 1e-6f) || !isfinite(l)) { q[0] = q[1] = q[2] = 0; q[3] = 1; return; }
    for (int i = 0; i < 4; i++) q[i] /= l;
}
static void q_rot(const float q[4], const float v[3], float o[3]) {   /* v + 2w(u x v) + 2 u x (u x v) */
    float t[3] = {2 * (q[1] * v[2] - q[2] * v[1]), 2 * (q[2] * v[0] - q[0] * v[2]), 2 * (q[0] * v[1] - q[1] * v[0])};
    o[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    o[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    o[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}
static void q_conj(const float q[4], float o[4]) { o[0] = -q[0]; o[1] = -q[1]; o[2] = -q[2]; o[3] = q[3]; }
/* matriz 4x4 columna-mayor (columnas = ejes) -> cuaternión */
static void q_from_m16(const float m[16], float q[4]) {
#define R(r, c) m[4 * (c) + (r)]
    float tr = R(0, 0) + R(1, 1) + R(2, 2);
    if (tr > 0) {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s; q[0] = (R(2, 1) - R(1, 2)) / s; q[1] = (R(0, 2) - R(2, 0)) / s; q[2] = (R(1, 0) - R(0, 1)) / s;
    } else if (R(0, 0) > R(1, 1) && R(0, 0) > R(2, 2)) {
        float s = sqrtf(1.0f + R(0, 0) - R(1, 1) - R(2, 2)) * 2.0f;
        q[3] = (R(2, 1) - R(1, 2)) / s; q[0] = 0.25f * s; q[1] = (R(0, 1) + R(1, 0)) / s; q[2] = (R(0, 2) + R(2, 0)) / s;
    } else if (R(1, 1) > R(2, 2)) {
        float s = sqrtf(1.0f + R(1, 1) - R(0, 0) - R(2, 2)) * 2.0f;
        q[3] = (R(0, 2) - R(2, 0)) / s; q[0] = (R(0, 1) + R(1, 0)) / s; q[1] = 0.25f * s; q[2] = (R(1, 2) + R(2, 1)) / s;
    } else {
        float s = sqrtf(1.0f + R(2, 2) - R(0, 0) - R(1, 1)) * 2.0f;
        q[3] = (R(1, 0) - R(0, 1)) / s; q[0] = (R(0, 2) + R(2, 0)) / s; q[1] = (R(1, 2) + R(2, 1)) / s; q[2] = 0.25f * s;
    }
#undef R
    q_norm(q);
}
static void m16_from_pq(const float p[3], const float q[4], float m[16]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y + z * w);     m[2] = 2 * (x * z - y * w);     m[3] = 0;
    m[4] = 2 * (x * y - z * w);     m[5] = 1 - 2 * (x * x + z * z); m[6] = 2 * (y * z + x * w);     m[7] = 0;
    m[8] = 2 * (x * z + y * w);     m[9] = 2 * (y * z - x * w);     m[10] = 1 - 2 * (x * x + y * y); m[11] = 0;
    m[12] = p[0]; m[13] = p[1]; m[14] = p[2]; m[15] = 1;
}
static int finite_n(const float *v, int n) { for (int i = 0; i < n; i++) if (!isfinite(v[i])) return 0; return 1; }

/* ============================================================================================================== buzón de manos */
typedef struct { int valid; float pos[3], rot[4]; float trigger, grip; int low, up, stick; int aim_valid; float aim_pos[3], aim_rot[4]; uint64_t t_ns; } HandSlot;
typedef struct { int valid; float p[3], q[4]; } GripPose;

static pthread_mutex_t HM = PTHREAD_MUTEX_INITIALIZER;   /* manos, opciones, hápticos, estado, publicación del arma */
static HandSlot hand_in[2];
static int opt_right = 1;
static int opt_aim = 1;            /* sin efecto: el arma cuelga de la mano por el zócalo de vr_grip (la boca apunta como la mano), ya no toma el apuntado del mando */
static float goff_p[3] = {0, 0, 0}, goff_q[4] = {0, 0, 0, 1};
static float hap_out[2];
static MvVrGameStatus pub_status;
static MvVrSpecialView pub_special;      /* vista especial del arma (mira, visor, pantalla del misil) */
static uint32_t pub_special_ser;      /* número de publicación de pub_special (sube con cada publicación; HM) */
static int opt_missile_view;               /* 0 pantalla en el lanzador, 1 pantalla grande ante la cara */
static MvVrForearm pub_fore[2];
static int pub_fore_ok[2];
/* Guantes (brazo + antebrazo + mano originales con la IK repetida con el agarre vivo, ver vr_glove.h y vrbody_arm_resolve). La simulación construye mallas+atlas en un hueco libre
 * de tres (publicado / en lectura por el renderer / en construcción) y publica el estado de la IK de cada brazo; el hilo de render compone cada frame (mv_host_hands). */
static VrGlove *glv[3];
static int glv_pub = -1, glv_read = -1;
static int glv_on;                         /* modo gpu con la IK enlazada (vrgame_set_gloves) */
static const uint16_t *glv_vram;
static VrBodyArmLive pub_live[2];
static int pub_live_ok[2];
static uint64_t pub_live_ns[2];            /* instante del último apply de la IK que produjo pub_live */
static uint32_t pub_live_applies;
#define GLOVE_REBUILD_VB 15u               /* VBlanks mínimos entre comprobaciones/reconstrucciones del guante ya publicado */
#define GLOVE_MAX_AGE_NS 250000000ull      /* IK más vieja que esto: sin guante (el motor dejó de aplicar el cuerpo: cinemática, menú) */
static struct { int valid; uint32_t ser; float dq[4], dp[3]; } hd_last;      /* movimiento rígido D con el que el ÚLTIMO mv_host_hands colocó el modelo del arma (valid = 0 si no lo aplicó); lo reutiliza mv_vrgame_special_sync (HM) ; ser = pub_special_ser vigente cuando se aplicó (sync solo la usa si sigue siendo la misma publicación) */
static GripPose pub_sim_hand;      /* mano del arma con la que se simuló el último frame publicado (compensación de latencia) */
static int pub_sim_valid;
static float aimfix_q[2][4] = {{0, 0, 0, 1}, {0, 0, 0, 1}};      /* giro extra agarre -> mano que lleva vr_body (vrbody_set_aim_fix) para que la boca apunte como el mando; solo hilo de emulación */
static float aimfix_miss[2];                   /* segundos sin apuntado válido con una corrección en curso */
#define AIMFIX_HOLD_S 0.25f                    /* tras perder el apuntado se mantiene la última corrección este tiempo ... */
#define AIMFIX_DECAY_S 0.08f                   /* ... y luego se retira con esta constante de tiempo (nlerp a la identidad) */
static void aimfix_clear_side(int i) { aimfix_q[i][0] = aimfix_q[i][1] = aimfix_q[i][2] = 0; aimfix_q[i][3] = 1; aimfix_miss[i] = 0; vrbody_set_aim_fix(i, NULL); }
static void aimfix_clear(void) { aimfix_clear_side(0); aimfix_clear_side(1); }
/* Armas cuyo -Z de WP_MUZZLE es una boca real (la corrección de puntería solo vale para ellas): pistola y fusil. Granadas y armas puestas llevan una pose de boca identidad sin eje de disparo; misil guiado, antiaéreo y
 * francotirador quedan fuera porque su mira/visor (WP_EYE) depende de la orientación del arma y no está comprobado que sobreviva a girar la mano hasta ~40 grados. */
static int aimfix_weapon(int gid) { return gid == 0 || gid == 1; }
/* Sin apuntado válido este fotograma: mantiene la corrección AIMFIX_HOLD_S y después la retira suavemente (sin salto de decenas de grados). */
static void aimfix_lost(int i, float dt) {
    if (aimfix_q[i][0] == 0 && aimfix_q[i][1] == 0 && aimfix_q[i][2] == 0) { aimfix_miss[i] = 0; return; }
    aimfix_miss[i] += dt;
    if (aimfix_miss[i] <= AIMFIX_HOLD_S) return;
    float t = 1.0f - expf(-dt / AIMFIX_DECAY_S), sg = aimfix_q[i][3] < 0 ? -1.0f : 1.0f, q[4];
    for (int k = 0; k < 4; k++) q[k] = (1.0f - t) * sg * aimfix_q[i][k] + (k == 3 ? t : 0.0f);
    q_norm(q);
    float v2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2];
    if (v2 < 3e-6f) { aimfix_clear_side(i); return; }       /* < 0,2 grados: fuera */
    memcpy(aimfix_q[i], q, sizeof q);
    vrbody_set_aim_fix(i, q);
}
/* Mano de apoyo SOLDADA al arma de fuego (vrgame_pad -> grip_eff_locked, bajo HM): lado de la mano soldada y pose de SU agarre en el marco del agarre de la mano que empuña. El agarre efectivo de la mano soldada es
 * siempre agarre_dominante * (p, q): el mando de ese lado no lo mueve (solo es fantasma de gestos, vrgame_hand_raw). */
static struct { int on, side; float p[3], q[4]; uint64_t off_ns; } pub_weld;      /* off_ns: instante en que se soltó (0 = no hay rampa); durante WELD_FADE_NS la mano vuelve al mando mezclando */
#define WELD_HOLD_VBL 6                 /* VBlanks que se retiene la soldadura tras desequipar (~0,12 s): la recarga del juego quita el arma un instante y no debe soltar las manos del arma */
#define WELD_FADE_NS 200000000ull       /* al desequipar el arma, la mano de apoyo vuelve al mando en 0,2 s */
static struct { int on, ghost; float p[3], q[4], hit_m; } pub_laser;      /* láser de la pistola: pose de la boca (espacio VR) del último VBlank y distancia (m) al primer impacto con el escenario (-1 = sin impacto: 15 m); on = 0: no se dibuja (w_laser.h) */

#define HAND_MAX_AGE_NS 300000000ull    /* mano sin actualizar más de 0,3 s = sin seguimiento */

void mv_vrgame_set_hand(int32_t side, const MvVrHand *h) {
    if (!h || side < 0 || side > 1) return;
    HandSlot s;
    memset(&s, 0, sizeof s);
    s.valid = h->valid && finite_n(h->pos, 3) && finite_n(h->rot, 4);
    if (s.valid) {
        memcpy(s.pos, h->pos, sizeof s.pos); memcpy(s.rot, h->rot, sizeof s.rot); q_norm(s.rot);
        s.trigger = isfinite(h->trigger) ? h->trigger : 0; s.grip = isfinite(h->grip) ? h->grip : 0;
        s.low = h->low != 0; s.up = h->up != 0; s.stick = h->stick != 0;
        s.aim_valid = h->aim_valid && finite_n(h->aim_pos, 3) && finite_n(h->aim_rot, 4);
        if (s.aim_valid) { memcpy(s.aim_pos, h->aim_pos, sizeof s.aim_pos); memcpy(s.aim_rot, h->aim_rot, sizeof s.aim_rot); q_norm(s.aim_rot); }
    }
    s.t_ns = mono_ns();
    pthread_mutex_lock(&HM);
    hand_in[side] = s;
    pthread_mutex_unlock(&HM);
}
void mv_vrgame_clear_hands(void) {
    pthread_mutex_lock(&HM);
    memset(hand_in, 0, sizeof hand_in);
    pthread_mutex_unlock(&HM);
}
void mv_vrgame_set_options(int32_t right_handed, int32_t reload_mode) {
    pthread_mutex_lock(&HM);
    opt_right = right_handed != 0;
    (void)reload_mode;      /* sin efecto: la recarga es siempre la del juego (vr_reload.h); se conserva el parámetro por la interfaz */
    pthread_mutex_unlock(&HM);
}
void mv_vrgame_set_weapon_aim(int32_t on) {
    pthread_mutex_lock(&HM);
    opt_aim = on != 0;
    pthread_mutex_unlock(&HM);
}
void mv_vrgame_set_grip_offset(const float p[3], const float q[4]) {
    if (!p || !q || !finite_n(p, 3) || !finite_n(q, 4)) return;
    pthread_mutex_lock(&HM);
    memcpy(goff_p, p, sizeof goff_p); memcpy(goff_q, q, sizeof goff_q); q_norm(goff_q);
    pthread_mutex_unlock(&HM);
}

/* Pose del mando -> marco del agarre (vr/body). Con HM tomado. */
static GripPose grip_locked(const HandSlot *s, uint64_t now) {
    GripPose g;
    memset(&g, 0, sizeof g);
    g.q[3] = 1;
    if (!s->valid || now - s->t_ns > HAND_MAX_AGE_NS) return g;
    float off[3];
    q_rot(s->rot, goff_p, off);
    for (int i = 0; i < 3; i++) g.p[i] = s->pos[i] + off[i];
    q_mul(s->rot, goff_q, g.q);
    q_norm(g.q);
    g.valid = 1;
    return g;
}

/* IK de la mano repetida con el agarre vivo (definida más abajo; HM tomado). Solo la usan el guante y el antebrazo del radar: el arma NO cuelga de ella. */
static int live_arm_locked(int side, uint64_t now, VrBodyArmLive *a, GripPose *gn, VrBodyArmPose *ap, float *dev_mm, float *age_ms);

static void m3_to_q(const float R[9], float q[4]) {      /* R fila-mayor con los ejes en las columnas */
    float m[16] = {0};
    for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) m[4 * c + r] = R[3 * r + c];
    m[15] = 1;
    q_from_m16(m, q);
}
static GripPose hand_from_arm(const float R[9], const float wrist[3]) {
    GripPose g;
    memset(&g, 0, sizeof g);
    m3_to_q(R, g.q); memcpy(g.p, wrist, sizeof g.p); g.valid = finite_n(g.p, 3) && finite_n(g.q, 4);
    return g;
}
/* Marco de la MANO nueva (el de la pieza mano del personaje y de vr_hand_skin: dedos -Y, palma +X en la derecha, pulgar +Z; origen en la muñeca; m, espacio recentrado) con el que el arma
 * cuelga: función PURA del agarre del mando (una sola fuente, sin la IK ni ningún estado del cuerpo): R_mano = R(agarre * desvío fijo agarre->mano) * mapa de la pieza mano, y muñeca = agarre -
 * R_mano * centro de agarre (lo mismo que hace vr_body al dibujar la mano). El desvío es vrbody_hand_offset (calibración o cabeceo fijo + alabeo; vr_body lo usa también con la mano armada).
 * El arma = este marco * inv(zócalo de vr_grip): su punto de empuñadura cae en el agarre del mando y su boca sigue al mando girada por un desvío constante. g sin validar = marco inválido. */
static GripPose hand_frame_from_grip(int side, const GripPose *g) {
    GripPose o;
    memset(&o, 0, sizeof o);
    if (!g->valid) return o;
    float qr[4], q[4], Rg[16], Rf[9];
    vrbody_hand_offset(side, qr);
    q_mul(g->q, qr, q); q_norm(q);
    m16_from_pq(g->p, q, Rg);
    /* hand_map (vr_body.c): columnas = imágenes de los ejes locales de la pieza mano en el marco de mano (dedos -Z, dorso +Y) */
    static const float MR[9] = {0, 0, -1, -1, 0, 0, 0, 1, 0}, ML[9] = {0, 0, 1, 1, 0, 0, 0, 1, 0};
    const float *M = side == 1 ? MR : ML;
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) Rf[3 * r + c] = Rg[r] * M[c] + Rg[4 + r] * M[3 + c] + Rg[8 + r] * M[6 + c];
    float c[3], org[3];                                          /* muñeca = agarre - R_mano * centro de agarre (la mano se dibuja a tamaño real: mm -> m) */
    vrhs_grip_center(side == 1 ? 1 : -1, c);
    for (int i = 0; i < 3; i++) org[i] = g->p[i] - (Rf[3 * i] * c[0] + Rf[3 * i + 1] * c[1] + Rf[3 * i + 2] * c[2]) * VRHS_UNIT_M;
    return hand_from_arm(Rf, org);
}
/* Inversa de hand_frame_from_grip: el agarre del mando cuyo marco de mano es f (mismo desvío agarre -> mano y mismo centro de agarre). */
static GripPose grip_from_hand_frame(int side, const GripPose *f) {
    GripPose o;
    memset(&o, 0, sizeof o);
    o.q[3] = 1;
    if (!f->valid) return o;
    float qr[4], qc[4], F[16], Rg[9], zero[3] = {0, 0, 0};
    vrbody_hand_offset(side, qr);
    m16_from_pq(zero, f->q, F);
    static const float MR[9] = {0, 0, -1, -1, 0, 0, 0, 1, 0}, ML[9] = {0, 0, 1, 1, 0, 0, 0, 1, 0};
    const float *M = side == 1 ? MR : ML;
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) Rg[3 * r + c] = F[r] * M[3 * c] + F[4 + r] * M[3 * c + 1] + F[8 + r] * M[3 * c + 2];      /* Rg = Rf * M^T */
    float qt[4];
    m3_to_q(Rg, qt);
    q_conj(qr, qc);
    q_mul(qt, qc, o.q); q_norm(o.q);
    float c[3];
    vrhs_grip_center(side == 1 ? 1 : -1, c);
    float Rf[9];
    for (int r = 0; r < 3; r++) for (int k = 0; k < 3; k++) Rf[3 * r + k] = F[4 * k + r];
    for (int i = 0; i < 3; i++) o.p[i] = f->p[i] + (Rf[3 * i] * c[0] + Rf[3 * i + 1] * c[1] + Rf[3 * i + 2] * c[2]) * VRHS_UNIT_M;
    o.valid = finite_n(o.p, 3) && finite_n(o.q, 4);
    return o;
}
/* Agarre EFECTIVO de una mano (HM tomado): el del mando, salvo la mano soldada al arma, cuyo agarre es el de la mano que empuña * la relación fija (pub_weld): con un arma de fuego equipada el mando de esa mano ya no la mueve. */
static GripPose grip_eff_locked(int side, uint64_t now) {
    float k = pub_weld.on ? 1.0f : 0.0f;
    if (!pub_weld.on && pub_weld.off_ns && now > pub_weld.off_ns) { const float f = (float)(now - pub_weld.off_ns) / (float)WELD_FADE_NS; k = f < 1.0f ? 1.0f - f : 0.0f; }
    if (k > 0.0f && side == pub_weld.side) {
        GripPose d = grip_locked(&hand_in[1 - side], now), g;
        memset(&g, 0, sizeof g);
        g.q[3] = 1;
        if (!d.valid) return g;
        float t[3];
        q_rot(d.q, pub_weld.p, t);
        for (int i = 0; i < 3; i++) g.p[i] = d.p[i] + t[i];
        q_mul(d.q, pub_weld.q, g.q); q_norm(g.q);
        g.valid = finite_n(g.p, 3) && finite_n(g.q, 4);
        if (k < 1.0f) {      /* rampa de salida: del agarre soldado al del mando */
            const GripPose c = grip_locked(&hand_in[side], now);
            if (c.valid) {
                const float sg = (g.q[0] * c.q[0] + g.q[1] * c.q[1] + g.q[2] * c.q[2] + g.q[3] * c.q[3]) < 0 ? -1.0f : 1.0f;
                for (int i = 0; i < 3; i++) g.p[i] = c.p[i] + (g.p[i] - c.p[i]) * k;
                for (int i = 0; i < 4; i++) g.q[i] = c.q[i] * sg + (g.q[i] - c.q[i] * sg) * k;
                q_norm(g.q);
                g.valid = finite_n(g.p, 3) && finite_n(g.q, 4);
            }
        }
        return g;
    }
    return grip_locked(&hand_in[side], now);
}
static GripPose hand_frame_locked(int side, uint64_t now) {
    GripPose g = grip_eff_locked(side, now);
    return hand_frame_from_grip(side, &g);
}

/* Manos para la IK de los brazos (hilo de emulación, etapa de dibujado). */
int vrpose_get_hands(VrHandPose out[2]) {
    uint64_t now = mono_ns();
    int n = 0;
    pthread_mutex_lock(&HM);
    for (int i = 0; i < 2; i++) {
        GripPose g = grip_eff_locked(i, now);
        memset(&out[i], 0, sizeof out[i]);
        if (!g.valid) continue;
        out[i].valid = 1;
        memcpy(out[i].pos, g.p, sizeof g.p); memcpy(out[i].rot, g.q, sizeof g.q);
        if (i == pub_weld.side && (pub_weld.on || (pub_weld.off_ns && now > pub_weld.off_ns && now - pub_weld.off_ns < WELD_FADE_NS))) {      /* mano soldada (o en la rampa de salida): su pose sale de la mano que empuña; la calibración del cuerpo parte del agarre REAL de su mando (vr_pose.h: derived / raw_rot) */
            const GripPose r = grip_locked(&hand_in[i], now);
            out[i].derived = 1;
            if (r.valid) { out[i].raw_valid = 1; memcpy(out[i].raw_rot, r.q, sizeof r.q); }
        }
        if (hand_in[i].aim_valid && !(pub_weld.on && i == pub_weld.side)) { out[i].aim_valid = 1; memcpy(out[i].aim_rot, hand_in[i].aim_rot, sizeof out[i].aim_rot); }   /* el apuntado endereza la mano del personaje (vr/body); la mano soldada no lleva el del mando fantasma */
        n++;
    }
    pthread_mutex_unlock(&HM);
    return n;
}


/* ---- orientación de las manos del personaje y calibración ---- */
static volatile float cal_countdown = 0;       /* s restantes; 0 = nada pendiente */
static volatile uint32_t cal_serial;
void mv_vrgame_set_hand_pose(float pitch_deg, int32_t auto_pitch, float roll_deg) {
    vrbody_set_hand_pitch(pitch_deg); vrbody_set_hand_pitch_auto(auto_pitch != 0); vrbody_set_hand_roll(roll_deg);
}
void mv_vrgame_calibrate_hands(float delay_s) { cal_countdown = (isfinite(delay_s) && delay_s > 0.02f) ? (delay_s > 30.0f ? 30.0f : delay_s) : 0.02f; }
float mv_vrgame_calibration_countdown(void) { return cal_countdown; }
void mv_vrgame_clear_hand_calibration(void) { vrbody_clear_calibration(); cal_serial++; }
int32_t mv_vrgame_hand_calibration(int32_t side, float q[4]) { return vrbody_get_calibration(side, q); }
int32_t mv_vrgame_arm_length(int32_t side, float *len_m) { return vrbody_get_arm_length(side, len_m); }
void mv_vrgame_set_arm_length(int32_t side, float len_m) { vrbody_set_arm_length(side, len_m); }
void mv_vrgame_set_hand_calibration(int32_t side, const float q[4]) { vrbody_set_calibration(side, q); }
uint32_t mv_vrgame_calibration_serial(void) { return cal_serial; }
/* Una vez por VBlank (hilo de emulación): cuenta atrás de la calibración con háptico. La calibración tiene prioridad sobre el cabeceo/alabeo configurados de esa mano. */
/* Diagnóstico de las manos: agarre CRUDO que llegó del visor y si la pose del agarre era válida (si no lo era, al cuerpo se le pasa 0). 0 = izquierda, 1 = derecha. */
static _Atomic float grip_diag_raw[2];
static _Atomic int grip_diag_ok[2];
void mv_vrgame_grip_diag(float raw[2], int32_t valid[2]) {
    for (int i = 0; i < 2; i++) {
        if (raw) raw[i] = atomic_load_explicit(&grip_diag_raw[i], memory_order_relaxed);
        if (valid) valid[i] = atomic_load_explicit(&grip_diag_ok[i], memory_order_relaxed);
    }
}

static void calib_tick(float dt) {
    float t = cal_countdown;
    if (t <= 0) return;
    float t2 = t - dt;
    if (t2 > 0) {
        if ((int)ceilf(t2) != (int)ceilf(t)) { vrgame_haptic(0, 0.3f); vrgame_haptic(1, 0.3f); }     /* un pulso por segundo */
        cal_countdown = t2;
        return;
    }
    cal_countdown = 0;
    int n = vrbody_calibrate_hands(), na = vrbody_calibrate_arms();
    if (n > 0 || na > 0) { cal_serial++; vrgame_haptic(0, 1.0f); vrgame_haptic(1, 1.0f); }     /* una calibración fallida (n = na = 0) no cambia nada ni se persiste */
    fprintf(stderr, "[manos] calibradas %d manos (corrección agarre->mano fija)%s; largo de brazo medido en %d lados\n", n, n ? "" : ": ninguna (¿mandos sin seguimiento o dedos casi verticales?)", na);
}

int32_t mv_vrgame_hand_matrix(int32_t side, float m[16]) {
    if (!m || side < 0 || side > 1) return 0;
    pthread_mutex_lock(&HM);
    GripPose g = grip_locked(&hand_in[side], mono_ns());
    pthread_mutex_unlock(&HM);
    if (!g.valid) return 0;
    m16_from_pq(g.p, g.q, m);
    return 1;
}
int32_t mv_vrgame_debug_frame(int32_t kind, int32_t side, float m[16]) {
    if (!m || side < 0 || side > 1 || kind < 0 || kind > 2) return 0;
    if (kind == 1) return vrbody_debug_hand_frame(side, m);
    uint64_t now = mono_ns();
    pthread_mutex_lock(&HM);
    GripPose g;
    if (kind == 0) g = grip_locked(&hand_in[side], now);
    else { g = hand_frame_locked(opt_right ? 1 : 0, now); if (!pub_status.active) g.valid = 0; }
    pthread_mutex_unlock(&HM);
    if (!g.valid) return 0;
    m16_from_pq(g.p, g.q, m);
    return 1;
}
int32_t mv_vrgame_forearm(int32_t side, MvVrForearm *out) {
    if (!out || side < 0 || side > 1) return 0;
    pthread_mutex_lock(&HM);
    int ok = pub_fore_ok[side];
    if (ok) *out = pub_fore[side];
    pthread_mutex_unlock(&HM);
    return ok;
}
void mv_vrgame_take_haptics(float *left, float *right) {
    pthread_mutex_lock(&HM);
    if (left) *left = hap_out[0];
    if (right) *right = hap_out[1];
    hap_out[0] = hap_out[1] = 0;
    pthread_mutex_unlock(&HM);
}
/* Vibración del juego (motor pequeño / grande, 0..255) por mando: el pequeño va a la derecha y el grande a la izquierda. El latido del corazón al apuntar con el PSG1 (sna_anim_psg1_helper_80057FD4:
 * NewPadVibration(dword_8009EF1C, 2) = GM_PadVibration2 = motor GRANDE) solo movía el mando izquierdo: con el PSG1 mostrado el motor grande se refleja también en el mando derecho. */
void mv_vrgame_route_rumble(uint8_t small, uint8_t large, float *left, float *right) {
    pthread_mutex_lock(&HM);
    const int psg1 = pub_status.active && pub_status.weapon == 9;
    pthread_mutex_unlock(&HM);
    float l = (float)large / 255.0f, r = (float)small / 255.0f;
    if (psg1 && l > r) r = l;
    if (left) *left = l;
    if (right) *right = r;
}
void mv_vrgame_status(MvVrGameStatus *out) {
    if (!out) return;
    pthread_mutex_lock(&HM);
    *out = pub_status;
    pthread_mutex_unlock(&HM);
}
/* Movimiento rígido D de la mano del arma desde el VBlank que simuló el último frame (pub_sim_hand) hasta el mando de AHORA (90 Hz): el mismo D con el que mv_host_hands mueve el modelo del arma. HM tomado. 0 si no hay D válido. */
static int sim_delta_locked(float dq[4], float dp[3]) {
    GripPose hn = hand_frame_locked(opt_right ? 1 : 0, mono_ns()), hs = pub_sim_hand;
    if (!pub_sim_valid || !hn.valid || !hs.valid) return 0;
    float qc[4], rp[3];
    q_conj(hs.q, qc);
    q_mul(hn.q, qc, dq);
    q_norm(dq);
    q_rot(dq, hs.p, rp);
    for (int i = 0; i < 3; i++) dp[i] = hn.p[i] - rp[i];
    float dmove = sqrtf((hn.p[0] - hs.p[0]) * (hn.p[0] - hs.p[0]) + (hn.p[1] - hs.p[1]) * (hn.p[1] - hs.p[1]) + (hn.p[2] - hs.p[2]) * (hn.p[2] - hs.p[2]));
    return dmove < 0.35f && 2.0f * acosf(fminf(1.0f, fabsf(dq[3]))) < 1.2f;
}
/* ¿La vista va pegada al modelo del arma (la superficie sigue al mando igual que el modelo)? Lente del PSG1, pantalla LED del Stinger y del Nikita en el lanzador; no la pantalla grande del misil */
static int view_follows_weapon(const MvVrSpecialView *v) {
    return v->kind == MV_VIEW_SCOPE || v->kind == MV_VIEW_AA_SCREEN || (v->kind == MV_VIEW_MISSILE && !v->fullscreen);
}
/* surf = D * surf (D = giro dq y desplazamiento dp de la mano) */
static void apply_hand_delta(MvVrSpecialView *v, const float dq[4], const float dp[3]) {
    float rp[3], sq[4] = {v->surf_rot[0], v->surf_rot[1], v->surf_rot[2], v->surf_rot[3]}, nq[4];
    q_rot(dq, v->surf_pos, rp);
    for (int i = 0; i < 3; i++) v->surf_pos[i] = rp[i] + dp[i];
    q_mul(dq, sq, nq); q_norm(nq);
    for (int i = 0; i < 4; i++) v->surf_rot[i] = nq[i];
}
int32_t mv_vrgame_special_view(MvVrSpecialView *out) {
    if (!out) return 0;
    pthread_mutex_lock(&HM);
    *out = pub_special;
    /* Vistas pegadas al modelo del arma (lente del PSG1, pantalla LED del Stinger y del Nikita): la superficie sigue al mando de ahora, igual que el modelo (mv_host_hands); con la pose del VBlank se quedaba atrás
     * al mover el arma y parte de la pantalla caía detrás de las caras vecinas del tubo. La cámara de la segunda vista (cam_pos / cam_rot) es la de la simulación y no se toca. */
    float dq[4], dp[3];
    if (view_follows_weapon(out) && sim_delta_locked(dq, dp)) apply_hand_delta(out, dq, dp);
    pthread_mutex_unlock(&HM);
    return out->kind != MV_VIEW_NONE;
}
int32_t mv_vrgame_special_sync(MvVrSpecialView *io) {
    int32_t ok = 0;
    if (!io) return 0;
    pthread_mutex_lock(&HM);
    /* Misma vista que la publicada (no una de la app, como los prismáticos: mismo tipo pero otro tamaño / redondez) */
    if (io->kind == pub_special.kind && io->round == pub_special.round && io->fullscreen == pub_special.fullscreen && fabsf(io->surf_w - pub_special.surf_w) < 1e-6f && fabsf(io->surf_h - pub_special.surf_h) < 1e-6f && view_follows_weapon(&pub_special)) {
        /* D calculada contra la mano de OTRA publicación (hubo un VBlank entre mv_host_hands y esta llamada): no vale para la superficie base de la vigente; no se toca y la app conserva la muestra de mv_vrgame_special_view */
        if (hd_last.valid && hd_last.ser != pub_special_ser) { pthread_mutex_unlock(&HM); return 0; }
        MvVrSpecialView b = pub_special;      /* superficie base de la simulación + la D EXACTA del modelo dibujado (o ninguna si el modelo se dibujó sin D) */
        if (hd_last.valid) apply_hand_delta(&b, hd_last.dq, hd_last.dp);
        memcpy(io->surf_pos, b.surf_pos, sizeof io->surf_pos);
        memcpy(io->surf_rot, b.surf_rot, sizeof io->surf_rot);
        ok = 1;
    }
    pthread_mutex_unlock(&HM);
    return ok;
}
void mv_vrgame_set_missile_view(int32_t mode) {
    pthread_mutex_lock(&HM);
    opt_missile_view = mode == 1;
    pthread_mutex_unlock(&HM);
}

/* Truco de depuración «Dar todas las armas». Capacidad PROVISIONAL (sin verificar contra el juego: GM_WeaponsMax lo fijan los guiones GCL del disco, no la decompilación) de las armas cuyo máximo aún es 0 en la RAM; las que ya tienen máximo conservan el suyo.
 * Verificados en volcados (/tmp/a30..a32, 75 RAM): 0 pistola 25, 2 granada 8, 6 adhesivo 4, 7 aturdidora 3, 8 humo 3. Sin verificar: 1 fusil, 3 misil guiado, 4 antiaéreo, 5 mina, 9 francotirador. */
static const int16_t CHEAT_WEAPON_CAP[BRIDGE_N_WEAPONS] = {25, 450, 8, 8, 8, 8, 4, 3, 3, 15};
static _Atomic int cheat_weapons_req;      /* petición de la app (hilo de render) -> la aplica el hilo de emulación una vez en vrgame_pad */
void mv_vrgame_cheat_all_weapons(void) { atomic_store_explicit(&cheat_weapons_req, 1, memory_order_release); }
/* Lo mismo que el juego al recoger un arma (game/item.c add_weapon): GM_Weapons[i] (BA_VARS + 2 * (17 + i), s16, -1 = no la tiene) = GM_WeaponsMax[i] (BA_VARS + 2 * (27 + i)). Conserva el bit 0x8000 (arma bloqueada) de las que ya se tenían.
 * Solo toca esas dos tablas de 10 s16. Devuelve cuántas armas escribió (0 con ram NULL). */
int vrgame_cheat_weapons_apply(uint8_t *ram) {
    if (!ram) return 0;
    uint8_t *am = ram + ((BA_VARS + 2u * BRIDGE_V_WEAPON_AMMO) & 0x1FFFFFu);
    uint8_t *mx = ram + ((BA_VARS + 2u * BRIDGE_V_WEAPON_MAX) & 0x1FFFFFu);
    for (int i = 0; i < BRIDGE_N_WEAPONS; i++) {
        int16_t a, m;
        memcpy(&a, am + 2 * i, 2);
        memcpy(&m, mx + 2 * i, 2);
        if (m <= 0) { m = CHEAT_WEAPON_CAP[i]; memcpy(mx + 2 * i, &m, 2); }
        int16_t n = (int16_t)(m - 1);      /* max - 1: con el cargador al máximo las cajas de munición dan «FULL» y no ejecutan su guion */
        if (a != -1 && (a & 0x8000)) n = (int16_t)((uint16_t)n | 0x8000u);
        memcpy(am + 2 * i, &n, 2);
    }
    return BRIDGE_N_WEAPONS;
}

/* ============================================================================================================== carga del arma */
enum { LD_IDLE = 0, LD_LOADING = 1, LD_READY = 2, LD_FAILED = 3 };

/* Una ranura por arma con modelo. Se carga bajo demanda la primera vez que el jugador la equipa (un hilo cada vez). */
typedef struct {
    _Atomic int state;
    int disc;                    /* disco con el que falló (otro disco: se reintenta) */
    WCutSet cs;
    WeaponDef *def;
    WeaponState *st;
    VrHandsRig *rig;
    int native;                  /* granada (vrgame_weapon_uses_game_model): la lógica VR sigue, pero se dibuja y NO se oculta el modelo del juego (escena nativa, con su propio CLUT); nunca el modelo VR (dudoso/blanco) */
} Slot;
static Slot SL[NSLOT];
static struct {
    char path[1024];
    int is_dir;
    pthread_t th;
    int th_started;
    int loading;                 /* ranura en carga (-1 ninguna) */
} LD = {.loading = -1};

/* Ranura solo de lógica (modelo 0, granada 7): sin mallas ni disco; el arma registrada con el id y un aparejo de manos sin mallas. 1 = lista. */
static int load_logic_only(Slot *sl, int game_id) {
    const WeaponDef *base = NULL;
    for (int i = 0; i < w_registry_count; i++) if (w_registry[i] && w_registry[i]->game_weapon_id == game_id) base = w_registry[i];
    if (!base) return 0;
    WeaponDef *def = (WeaponDef *)malloc(sizeof *def);
    WeaponState *st = (WeaponState *)calloc(1, base->state_size);
    if (!def || !st) { free(def); free(st); return 0; }
    *def = *base;
    st->def = def; def->init(st); st->def = def;
    VrHandsConfig hc;
    vrhands_default_config(&hc);
    hc.grip_mode = VRH_GRIP_SOCKET;
    VrHandsRig *rig = vrhands_create(&hc);
    if (!rig) { free(def); free(st); return 0; }      /* sin mallas no hay nada que enlazar: la mano y el mundo del arma no lo necesitan */
    sl->def = def; sl->st = st; sl->rig = rig;
    return 1;
}

static void *load_thread(void *arg) {
    int si = (int)(intptr_t)arg;
    Slot *sl = &SL[si];
    const uint16_t model = WPN_TAB[si].model;
    int rc = -1;
    if (!model || vrgame_weapon_uses_game_model(WPN_TAB[si].game_id)) {   /* las granadas se dibujan con el modelo del propio juego: sin carga de disco (el arma VR estaría lista segundos después de equipar, o nunca si falla), la mano la lleva desde el primer VBlank */
        if (!load_logic_only(sl, WPN_TAB[si].game_id)) { atomic_store_explicit(&sl->state, LD_FAILED, memory_order_release); return NULL; }
        sl->native = vrgame_weapon_uses_game_model(WPN_TAB[si].game_id);
        atomic_store_explicit(&sl->state, LD_READY, memory_order_release);
        fprintf(stderr, "[vrgame] arma VR %d lista (solo lógica; se dibuja el modelo del propio juego)\n", WPN_TAB[si].game_id);
        return NULL;
    }
    size_t len = 0;
    WLoaded m;
    memset(&m, 0, sizeof m);
    WCutSet cs;
    memset(&cs, 0, sizeof cs);
    uint8_t *arch = disc_load_stage_archive(LD.path, LD.is_dir, &len);
    if (!arch) { fprintf(stderr, "[vrgame] arma VR %d: el disco no tiene el archivo de modelos legible\n", WPN_TAB[si].game_id); goto fail; }
    WLoadOpts o;
    wload_default_opts(&o);
    o.resident = WPN_STAGE;
    o.upscale = 2;
    rc = wload_model(arch, len, WPN_STAGE, model, &o, &m);
    free(arch);
    if (rc) { fprintf(stderr, "[vrgame] arma VR %d: wload_model=%d\n", WPN_TAB[si].game_id, rc); goto fail; }
    /* Cobertura de texturas: una malla sin atlas, o con caras sin su textura, o con una de 8 bits sin paleta (rampa de grises), se ve blanca o plana. */
    fprintf(stderr, "[vrgame] arma VR %d texturas: atlas %ux%u, %u de %u cargadas, %u sin encontrar (primera %04x), %u de 8 bits sin paleta\n", WPN_TAB[si].game_id, m.atlas_w, m.atlas_h, m.ntex, m.ntex_ref,
            m.ntex_missing, m.first_missing, m.ntex_nopal);
    const int tex_bad = !m.atlas || m.ntex_missing || m.ntex_nopal;
    if (vrgame_weapon_uses_game_model(WPN_TAB[si].game_id)) fprintf(stderr, "[vrgame] arma VR %d: se dibuja el modelo del propio juego (escena nativa)%s\n", WPN_TAB[si].game_id, tex_bad ? " (texturas VR incompletas)" : "");
    const WCutModel *cm = wcut_find(model);
    if (!cm || !cm->weapon) { wload_free(&m); fprintf(stderr, "[vrgame] arma VR %d: sin tabla de corte\n", WPN_TAB[si].game_id); goto fail; }
    rc = wcut_run(&m, cm, &cs);
    wload_free(&m);
    if (rc) { fprintf(stderr, "[vrgame] arma VR %d: wcut_run=%d\n", WPN_TAB[si].game_id, rc); goto fail; }
    const WeaponDef *base = NULL;
    for (int i = 0; i < w_registry_count; i++) if (w_registry[i] && !strcmp(w_registry[i]->id, cm->weapon)) base = w_registry[i];
    if (!base || base->game_weapon_id != WPN_TAB[si].game_id) { fprintf(stderr, "[vrgame] arma VR %d: arma no registrada\n", WPN_TAB[si].game_id); goto fail2; }
    WeaponDef *def = (WeaponDef *)malloc(sizeof *def);
    WeaponState *st = (WeaponState *)calloc(1, base->state_size);
    if (!def || !st) { free(def); free(st); goto fail2; }
    *def = *base;
    st->def = def;
    def->init(st);                                        /* init pone s->def = &<arma base>: se corrige abajo */
    st->def = def;
    weapon_bind(st, cs.meshes, cs.nmeshes, cs.parts, cs.nparts);
    /* El agarre derivado del modelo pasa a ser el del arma: dibujo (vr_hands) y lógica (puntos de interacción, boca) coinciden.
     * Sin pieza de referencia (granadas, minas...) se conserva el punto de la definición. */
    WPose g;
    if (vrhands_derive_grip(st, &g) == 0) def->points[WP_GRIP] = g;
    VrHandsConfig hc;
    vrhands_default_config(&hc);
    hc.grip_mode = VRH_GRIP_SOCKET;     /* el arma cuelga de la mano por el zócalo de vr_grip (se fija cada VBlank); el agarre del modelo es el de def */
    VrHandsRig *rig = vrhands_create(&hc);
    if (!rig || vrhands_bind(rig, st, cs.atlas, cs.atlas_w, cs.atlas_h)) {
        if (rig) vrhands_destroy(rig);
        free(def); free(st);
        goto fail2;
    }
    sl->cs = cs; sl->def = def; sl->st = st; sl->rig = rig;
    sl->native = vrgame_weapon_uses_game_model(WPN_TAB[si].game_id);
    atomic_store_explicit(&sl->state, LD_READY, memory_order_release);
    fprintf(stderr, "[vrgame] arma VR %d lista (%d piezas)\n", WPN_TAB[si].game_id, cs.nmeshes);
    return NULL;
fail2:
    wcut_free(&cs);
fail:
    atomic_store_explicit(&sl->state, LD_FAILED, memory_order_release);
    return NULL;
}

static int slot_of(int game_id) {
    for (int i = 0; i < NSLOT; i++) if (WPN_TAB[i].game_id == game_id) return i;
    return -1;
}

/* Hilo de emulación. Recoge el hilo anterior si ya terminó y pide el siguiente modelo. */
static void load_poll(void) {
    if (LD.loading >= 0) {
        int s = atomic_load_explicit(&SL[LD.loading].state, memory_order_acquire);
        if (s == LD_READY || s == LD_FAILED) {
            if (LD.th_started) { pthread_join(LD.th, NULL); LD.th_started = 0; }
            if (s == LD_FAILED) SL[LD.loading].disc = cd_current_disc();
            LD.loading = -1;
        }
    }
}
static void load_start(int si) {
    if (si < 0 || LD.loading >= 0) return;
    int st = atomic_load(&SL[si].state);
    if (st == LD_FAILED && SL[si].disc != cd_current_disc()) { atomic_store(&SL[si].state, LD_IDLE); st = LD_IDLE; }   /* otro disco: se reintenta */
    if (st != LD_IDLE) return;
    if (cd_current_source(LD.path, sizeof LD.path, &LD.is_dir) != 0) { atomic_store(&SL[si].state, LD_FAILED); SL[si].disc = cd_current_disc(); return; }
    atomic_store(&SL[si].state, LD_LOADING);
    LD.loading = si;
    if (pthread_create(&LD.th, NULL, load_thread, (void *)(intptr_t)si) != 0) { atomic_store(&SL[si].state, LD_FAILED); SL[si].disc = cd_current_disc(); LD.loading = -1; return; }
    LD.th_started = 1;
}

/* ============================================================================================================== simulación del arma */
/* Botón con el que el juego detona las cargas adhesivas colocadas: círculo (bit interno 0x20 del mando del juego). Comprobado en el EXE: el actor de la
 * carga lee el flanco de ese bit (0x20) del mando, no el de ataque (0x80, cuadrado), que solo coloca. */
#define PAD_DETONATE PAD_CIRCLE
#define DET_PRESS_VBL 4

#define PLACE_REACH_M 1.6f      /* radio (m, desde la cabeza) dentro del cual se coloca una mina o carga */
#define THROW_TICK_S 0.04f      /* un tick del motor (2 VBlanks de 50 Hz) */

static struct {
    int body_ok, fire_ok;
    int inited;
    uint64_t last_ns;
    int throw_hold;             /* arrojadiza VR (2, 7, 8) mostrada en HELD o READY al final del VBlank anterior (vrgame_throw_hold) */
    uint64_t hand_ns;           /* instante (t_ns) de la pose de la mano del arma vista en el VBlank anterior (marca de tiempo de la pose para la velocidad de lanzamiento) */
    int cur;                    /* ranura del arma VR mostrada en el VBlank anterior (-1 ninguna) */
    int active;                 /* modo arma VR del VBlank anterior */
    int trig_prev;              /* gatillo VR (crudo) apretado en el VBlank anterior (flanco para pulsar el ataque con el cargador del juego vacío) */
    uint32_t shots_prev;        /* G.shots del VBlank anterior (disparo VR en el VBlank = VrReloadIn.vr_shot) */
    int fire_edge;              /* lo pone el callback fire() (o el gancho de lanzamiento) dentro de update */
    WPose fire_pose;
    int probe_ok;               /* sondeo de colocación (probe_hook) de este VBlank: el juego podría colocar el objeto en la mano ahora; probe_pose = dónde y con qué orientación quedaría */
    WPose probe_pose;
    int mag_weapon;             /* el arma activa lleva cargador en el juego (contabilidad virtual de cargadores) */
    int ammo_game;              /* munición total del arma activa en el juego (para los lanzamientos) */
    int game_mag;               /* cargador del juego (el cargador VR arranca con él) */
    VrFireMap map;              /* correspondencia usuario -> motor del VBlank (para el gancho de colocación) */
    WPose head;                 /* cabeza del VBlank (usuario) */
    float floor_u;              /* altura del suelo del juego en espacio de usuario (m) */
    int floor_ok;
    int det_t;                  /* VBlanks restantes pulsando el botón de detonación */
    int det_c4;                 /* VBlanks restantes del círculo que detona el C4 (agarre derecho, c4_remote) */
    int det_arm;                /* el agarre derecho estuvo suelto desde la última detonación / desde el arranque: puede detonar */
    int fblk;                   /* bloqueo de disparo por menú abierto del último VBlank con arma VR (0 sin ella) */
    int weld_hold;              /* VBlanks restantes en que un desequipado breve (recarga: el juego retira el arma un instante) NO suelta el brazo atado ni la petición de IK; 0 = suelta de inmediato */
    int sv_wait;                /* VBlanks que faltan para que la vista especial del arma mostrada pueda publicarse (arma recién entrada o cambio en curso, SV_SETTLE_TICKS) */
    int sv_prev;                /* ranura del arma retirada un instante (recarga, weld_hold): si vuelve la misma no espera otra vez a que su vista especial esté lista; -1 = ninguna */
    int sv_slot;                /* ranura del último arma cuya vista especial se publicó con el selector cerrado: con el selector abierto solo esa conserva su vista */
    int rl_latch;               /* recarga del juego en curso (ro.busy): el gatillo sigue sin llegar al arma hasta bajar de 0,3 (si no, el arma ve un flanco al acabar la recarga y dispara sola) */
    WGameBridge br;
    VrMissile mis;              /* misil guiado del juego vigilado (vr_missile.h) */
    uint32_t mis_serial;        /* nº de misiles creados visto por última vez (vrfire_last_missile) */
    float steady;               /* efecto del tranquilizante suavizado (0..1) */
    VrReloadOut ro;             /* última salida de la recarga */
    uint32_t shots, arms, throws, places, dets, rejects;
    int last_disc;
    int logged;
} G;

static void publish_off(int keep_weld) {      /* keep_weld: retención de la soldadura tras un desequipado breve (WELD_HOLD_VBL): la relación de la mano de apoyo se conserva */
    vrhands_post_clear();
    pthread_mutex_lock(&HM);
    pub_sim_valid = 0;
    pub_laser.on = 0;
    if (!keep_weld) { pub_weld.on = 0; pub_weld.off_ns = 0; }
    memset(&pub_special, 0, sizeof pub_special);
    pub_special_ser++;
    hd_last.valid = 0;
    pthread_mutex_unlock(&HM);
}

void vrgame_reset(void) {
    vrbodyg_reset();
    G.sv_wait = 0; G.sv_slot = -1; G.sv_prev = -1;
    G.inited = 0; G.active = 0; G.cur = -1; G.trig_prev = 0; G.shots_prev = G.shots; G.fire_edge = 0;
    G.last_ns = 0; G.hand_ns = 0; G.det_t = 0; G.det_c4 = 0; G.det_arm = 0; G.fblk = 0; G.rl_latch = 0; G.throw_hold = 0;
    vrbody_set_weld(0, 0); vrbody_set_weld(1, 0); vrbody_set_pole(0, NULL); vrbody_set_pole(1, NULL);
    vrbody_set_engine_hidden_ik(0);
    vrbody_set_hand_turn(0, NULL); vrbody_set_hand_turn(1, NULL);
    aimfix_clear();
    memset(&G.ro, 0, sizeof G.ro);
    publish_off(0);      /* un arma publicada antes de reiniciar el motor no debe quedarse dibujada */
    pthread_mutex_lock(&HM);
    glv_pub = -1; pub_live_ok[0] = pub_live_ok[1] = 0;      /* los huecos se reutilizan (el renderer puede estar leyendo uno) */
    pthread_mutex_unlock(&HM);
}
void vrgame_set_enabled(int body_ok, int fire_ok) { G.body_ok = body_ok; G.fire_ok = fire_ok; }

/* Balas que tiene el arma VR (cargador montado + recámara). */
static int weapon_held(const WeaponState *st) { return (st->mag_inserted ? st->mag_rounds : 0) + (st->chambered ? 1 : 0); }
/* Deja el arma VR con `n` balas (cargador montado + recámara), como el cargador del juego: sin balas, cargador montado vacío y corredera retenida. */
static void weapon_set_held(WeaponState *st, const WeaponDef *def, int n) {
    if (def->make_ready) def->make_ready(st, n > 0 ? n - 1 : 0);
    else { st->mag_inserted = 1; st->mag_rounds = n > 0 ? n - 1 : 0; st->chambered = 1; }
    if (n <= 0) { st->mag_rounds = 0; st->chambered = 0; }
}
static int cb_ammo_reserve(int id) { return vrreload_ammo_reserve(id); }
static void cb_fire(int id, WPose mz) { (void)id; G.fire_edge = 1; G.fire_pose = mz; G.shots++; }
static void cb_haptic(int hand, float strength, float seconds) {
    (void)seconds;
    int right;
    pthread_mutex_lock(&HM);
    right = opt_right;
    int side = hand == 0 ? (right ? 1 : 0) : (right ? 0 : 1);   /* hand 0 = mano del arma */
    if (isfinite(strength) && strength > hap_out[side]) hap_out[side] = strength > 1 ? 1 : strength;
    pthread_mutex_unlock(&HM);
}

/* Misil guiado: pose del misil del juego (actor vigilado por vr_missile) en el espacio de usuario; -Z = dirección de vuelo. */
static void pose_look(const float d[3], float q[4]) {
    float f[3] = {d[0], d[1], d[2]};
    float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (!(fl > 1e-6f)) { q[0] = q[1] = q[2] = 0; q[3] = 1; return; }
    for (int i = 0; i < 3; i++) f[i] /= fl;
    float r[3] = {f[1] * 0 - f[2] * 1, f[2] * 0 - f[0] * 0, f[0] * 1 - f[1] * 0};      /* delante x arriba(0,1,0) */
    float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (rl < 1e-4f) { r[0] = 1; r[1] = 0; r[2] = 0; rl = 1; }                         /* mirando recto arriba/abajo */
    for (int i = 0; i < 3; i++) r[i] /= rl;
    float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};   /* derecha x delante: sale arriba (+Y) */
    float m[16] = {r[0], r[1], r[2], 0, u[0], u[1], u[2], 0, -f[0], -f[1], -f[2], 0, 0, 0, 0, 1};
    q_from_m16(m, q);
}
static int cb_projectile(int id, WPose *out) {
    if (id != 3 || !out || !G.mis.alive || !G.map.valid) return 0;
    float w[3] = {(float)G.mis.pos[0], (float)G.mis.pos[1], (float)G.mis.pos[2]}, u[3], d[3], q[4];
    float dw[3] = {G.mis.dir[0], G.mis.dir[1], G.mis.dir[2]};
    vrfire_pos_from_world(&G.map, w, u);
    vrfire_dir_from_world(&G.map, dw, d);
    if (!finite_n(u, 3) || !finite_n(d, 3)) return 0;
    pose_look(d, q);
    out->p.x = u[0]; out->p.y = u[1]; out->p.z = u[2];
    out->q.x = q[0]; out->q.y = q[1]; out->q.z = q[2]; out->q.w = q[3];
    return 1;
}
static void cb_steer(int id, float yaw, float pitch) { if (id == 3) vrmis_steer(yaw, pitch); }

/* ---- vistas especiales (mira óptica, visor antiaéreo, pantalla del misil): descriptor para la app (ver vrgame.h) ---- */
#define SCOPE_TEX 1024           /* lado (texels) de la textura de la mira (la app admite 16..2048) */
#define AA_SIGHT_DEF_HALF_FOV 26.565f /* semicampo horizontal (grados) del visor del Stinger mientras el juego aún no ha dado el suyo: atan(160 / 320); el Stinger no cambia GM_Camera.zoom (camera.c:980, stnsight.c, aam.c) y la proyección normal tiene H = 320 en una pantalla de 320 de ancho */
#define SV_SETTLE_TICKS 3        /* VBlanks que espera la vista especial de un arma recién mostrada (cambio de arma) antes de publicarse */
#define LED_FRAME_FRAC 0.045f    /* grosor del marco oscuro de las pantallas LED (sombreador), como fracción de su altura */
#define AA_SIGHT_DIST 3.0f       /* distancia (m) a la que se coloca el HUD del visor (colimado: se ve a esa distancia con el tamaño angular del juego) */
static void put_pose(const WPose *w, float p[3], float q[4]) {
    p[0] = w->p.x; p[1] = w->p.y; p[2] = w->p.z;
    q[0] = w->q.x; q[1] = w->q.y; q[2] = w->q.z; q[3] = w->q.w;
}
/* HUD propio del Nikita (bullet/rmissile.c DrawMissileHud): barra de combustible de ((1000 - edad) * 60) / 1000 píxeles (tope 60) y la posición del misil. Solo con el misil vivo */
static void nikita_hud(MvVrSpecialView *sv, int live) {
    if (!live || !G.mis.alive) return;
    int w = ((1000 - G.mis.age) * 60) / 1000;
    sv->nik_hud = 1;
    sv->nik_fuel = (float)(w < 0 ? 0 : w > 60 ? 60 : w);
    for (int i = 0; i < 3; i++) sv->nik_pos[i] = (float)G.mis.pos[i];
}
static void special_view(int gid, WeaponState *st, const WeaponDef *def, const WPose *W, const WInput *in, int duct, int ready) {      /* ready = 0: arma recién entrada o cambio en curso, no se publica su vista */
    MvVrSpecialView sv;
    int fullscreen;
    (void)def;
    memset(&sv, 0, sizeof sv);
    pthread_mutex_lock(&HM);
    fullscreen = opt_missile_view;
    pthread_mutex_unlock(&HM);
    if (ready && gid == 9 && !duct) {       /* con el rifle oculto (prismáticos del juego) tampoco hay lente: flotaría sin rifle */
        sv.kind = MV_VIEW_SCOPE;
        sv.blend = st->channel[WCH_SCREEN];
        sv.zoom = sniper_zoom(st);
        {   /* la vista de la mira se renderiza siempre con el PSG1 en la mano (imagen viva en la lente); solo se omite si la lente no puede verse */
            WPose lw;
            sv.cam_valid = st->aux.active && sniper_lens_world(st, &lw) && sniper_lens_visible(&lw, &in->head);
        }
        put_pose(&st->aux.camera, sv.cam_pos, sv.cam_rot);
        sv.cam_fov_y = st->aux.fov_y;
        SniperLens sl;      /* la imagen llena la abertura interior del extremo trasero del tubo de la mira del modelo (sl.face: centro, orientación y caja ancho x alto de su contorno), en el plano de su borde y adelantada hacia el ojo para no solaparse con él; sin tubo hallado, la placa "lens" */
        sniper_lens(st, &sl);
        WPose lens = wpose_mul(*W, sl.face);
        lens.p = wv_add(lens.p, wquat_rotate(lens.q, wv(0, 0, SNIPER_LENS_LIFT)));
        put_pose(&lens, sv.surf_pos, sv.surf_rot);
        sv.surf_w = sl.w; sv.surf_h = sl.h;
        sv.round = 0;                            /* rectángulo = caja de la abertura; el fragmento descarta lo que cae fuera de su polígono (sv.poly), el resto del interior del tubo no se ve */
        sv.npoly = 0;
        if (sl.npoly >= 3 && sl.npoly <= SNIPER_POLY_MAX && sl.w > 1e-4f && sl.h > 1e-4f) {   /* vértices en coordenadas de la textura de la superficie: u = x / w + 0,5; v = 0,5 - y / h (v hacia abajo) */
            sv.npoly = sl.npoly;
            for (int i = 0; i < sl.npoly; i++) { sv.poly[2 * i] = sl.poly[i][0] / sl.w + 0.5f; sv.poly[2 * i + 1] = 0.5f - sl.poly[i][1] / sl.h; }
        }
        sv.tex_w = sv.tex_h = SCOPE_TEX;         /* textura con la proporción de la abertura: la imagen no se deforma */
        if (sl.w > sl.h) sv.tex_h = (int32_t)(SCOPE_TEX * sl.h / sl.w + 0.5f); else sv.tex_w = (int32_t)(SCOPE_TEX * sl.w / sl.h + 0.5f);
        if (sv.tex_w < 64) sv.tex_w = 64;
        if (sv.tex_h < 64) sv.tex_h = 64;
        sv.occlude = 1;                          /* la lente es un objeto del mundo: la tapan el escenario y el arma que tenga delante */
        sv.suppress_frame = vrcam_zoom_capped();  /* solo mientras el juego tiene SU mira activa (pidió más aumento que el tope): esa máscara es la abertura, que dibuja vr_scope en la lente; otras máscaras (gafas, careta) se conservan */
        vrcam_set_zoom_cap(320);                 /* la cabeza no se acerca: el aumento está en la lente */
    } else {
        vrcam_set_zoom_cap(0);
        if (ready && gid == 4 && !duct) {       /* con el lanzador oculto (conducto, tumbado, prismáticos) no hay pantalla LED ni visor plano: flotarían sin arma */
            VrAaSight sg;
            vraa_sight(&sg);
            WScreenFace af;
            if (aa_led_face(st, &af) && af.w > 1e-4f && af.h > 1e-4f) {
                /* Visor en su pieza del modelo ("screen", cara trasera de la mira), siempre vivo mientras se sostiene el lanzador (como la lente del PSG1): imagen de una segunda vista desde el eje del buscador (WP_EYE)
                 * con el campo horizontal del juego (la caja de la pieza cubre el ancho del HUD del juego, la altura su parte central a la misma escala angular), la mira y el marco de fijación del HUD encima y todo
                 * recortado al contorno exacto de la pieza. Sin campo del juego aún (visor sin usar) se supone AA_SIGHT_DEF_HALF_FOV. */
                float half = sg.half_fov_deg > 0.5f && sg.half_fov_deg < 80.0f ? sg.half_fov_deg : AA_SIGHT_DEF_HALF_FOV;
                float t = tanf(half * (3.14159265f / 180.0f));
                WPose eye = aa_sight_pose(st, in), face = wpose_mul(*W, af.face);
                sv.kind = MV_VIEW_AA_SCREEN;
                sv.blend = 1.0f;
                sv.cam_valid = 1;
                put_pose(&eye, sv.cam_pos, sv.cam_rot);
                sv.frame = LED_FRAME_FRAC;
                sv.cam_fov_y = 2.0f * atanf(t * (af.h * (1.0f - 2.0f * LED_FRAME_FRAC)) / (af.w - 2.0f * LED_FRAME_FRAC * af.h));   /* campo horizontal del juego sobre la pantalla útil (sin el marco) */
                put_pose(&face, sv.surf_pos, sv.surf_rot);
                sv.surf_w = af.w; sv.surf_h = af.h;
                sv.tex_w = SCOPE_TEX; sv.tex_h = (int32_t)(SCOPE_TEX * af.h / af.w + 0.5f);
                if (sv.tex_h < 64) sv.tex_h = 64;
                float k = (af.h / af.w) / 0.75f;     /* el HUD (4:3) cubre el ancho de la pieza; su subrectángulo central cubre la altura */
                if (k <= 1.0f) { sv.uv[0] = 0; sv.uv[1] = 0.5f - 0.5f * k; sv.uv[2] = 1; sv.uv[3] = k; }
                else { sv.uv[0] = 0.5f - 0.5f / k; sv.uv[1] = 0; sv.uv[2] = 1.0f / k; sv.uv[3] = 1; }
                sv.npoly = af.npoly;
                for (int i = 0; i < af.npoly; i++) { sv.poly[2 * i] = af.poly[i][0] / af.w + 0.5f; sv.poly[2 * i + 1] = 0.5f - af.poly[i][1] / af.h; }
                sv.occlude = 1;
                sv.hide_hud = 0;                 /* el panel plano del HUD del jugador (radar, vida, alerta...) SIGUE como con cualquier arma; solo el HUD propio del Stinger (mira, escala, LOCK_ON) se recorta del panel (XRPresent.weaponHudMask) y va en la pantalla LED */
                sv.locked = sg.locked; sv.converged = sg.converged;
            } else
            if (sg.active && sg.half_fov_deg > 0.5f && sg.half_fov_deg < 80.0f) {
                float t = tanf(sg.half_fov_deg * (3.14159265f / 180.0f));
                WVec3 fwd = wquat_rotate(sg.pose.q, wv(0, 0, -1));
                WPose pn = sg.pose;
                pn.p = wv_add(sg.pose.p, wv_scale(fwd, AA_SIGHT_DIST));
                sv.kind = MV_VIEW_AA_SIGHT;
                sv.blend = 1.0f;
                put_pose(&pn, sv.surf_pos, sv.surf_rot);
                sv.surf_w = 2.0f * AA_SIGHT_DIST * t;
                sv.surf_h = sv.surf_w * 0.75f;           /* la imagen de la consola (320 x 256) se muestra a 4:3 */
                sv.uv[0] = 0; sv.uv[1] = 0; sv.uv[2] = 1; sv.uv[3] = 1;
                sv.hide_hud = 0;                 /* respaldo: igual que la pantalla LED, el panel plano sigue (con el HUD propio del Stinger recortado) */
                sv.locked = sg.locked; sv.converged = sg.converged;
            }
        } else if (ready && gid == 3) {
            const GuidedState *gs = (const GuidedState *)st;
            WScreenFace lf;
            const int route = guided_led_route(st, G.mis.alive, fullscreen);
            if (route == GLED_FULLSCREEN) {     /* opción de comodidad: pantalla grande fija ante la cara durante el vuelo (la cabeza no se mueve nunca) */
                WPose pnl;
                float pw = 0.1f, ph = 0.075f;
                guided_view_panel(st, in, &pnl, &pw, &ph);
                WVec3 c = wv_add(in->head.p, wquat_rotate(in->head.q, wv(0, -0.05f, -1.1f)));
                pnl.p = c; pnl.q = in->head.q; pw = 1.4f; ph = 1.05f;
                sv.kind = MV_VIEW_MISSILE; sv.fullscreen = 1; sv.blend = 1.0f;
                put_pose(&pnl, sv.surf_pos, sv.surf_rot);
                sv.surf_w = pw; sv.surf_h = ph;
                sv.cam_valid = st->phase == G_FLIGHT && G.mis.alive;
                put_pose(&st->aux.camera, sv.cam_pos, sv.cam_rot);
                sv.cam_fov_y = st->aux.fov_y;
                sv.tex_w = GD_VIEW_W; sv.tex_h = GD_VIEW_H;
                sv.noise = gs->noise;
                sv.ended = route == GLED_STATIC || st->phase == G_STATIC;
                sv.hide_hud = 1;                 /* pantalla grande del misil ante la cara: el panel plano (se dibuja encima de la escena) taparía la cámara del misil */
                nikita_hud(&sv, sv.cam_valid && !sv.ended);
            } else if (!duct && guided_led_face(st, &lf) && lf.w > 1e-4f && lf.h > 1e-4f) {      /* con el lanzador oculto no hay pantalla LED; la vista completa del vuelo no depende del arma y se mantiene */
                /* Pantalla LED en la cara trasera del visor del modelo (como la lente del PSG1, recortada a su contorno), siempre a la vista mientras se sostiene el lanzador: en reposo muestra la pantalla
                 * en espera; en vuelo, la cámara del misil del juego (el jugador sigue viendo el mundo normal y el control del misil no cambia); al terminar, la estática. */
                WPose face = wpose_mul(*W, lf.face);
                sv.kind = MV_VIEW_MISSILE;
                sv.blend = 1.0f;
                put_pose(&face, sv.surf_pos, sv.surf_rot);
                sv.surf_w = lf.w; sv.surf_h = lf.h;
                sv.npoly = lf.npoly;
                for (int i = 0; i < lf.npoly; i++) { sv.poly[2 * i] = lf.poly[i][0] / lf.w + 0.5f; sv.poly[2 * i + 1] = 0.5f - lf.poly[i][1] / lf.h; }
                sv.frame = LED_FRAME_FRAC;
                {   /* la imagen del misil es 4:3 y la pantalla útil (sin marco) casi: se recorta el ancho/alto sobrante para no deformarla */
                    float iw = lf.w - 2.0f * LED_FRAME_FRAC * lf.h, ih = lf.h * (1.0f - 2.0f * LED_FRAME_FRAC), k = (iw / ih) / ((float)GD_VIEW_W / (float)GD_VIEW_H);
                    if (k <= 1.0f) { sv.uv[0] = 0.5f - 0.5f * k; sv.uv[1] = 0; sv.uv[2] = k; sv.uv[3] = 1; }
                    else { sv.uv[0] = 0; sv.uv[1] = 0.5f - 0.5f / k; sv.uv[2] = 1; sv.uv[3] = 1.0f / k; }
                }
                sv.occlude = 1;
                sv.cam_valid = route == GLED_LIVE && G.mis.alive;
                put_pose(&st->aux.camera, sv.cam_pos, sv.cam_rot);
                sv.cam_fov_y = st->aux.fov_y;
                sv.tex_w = GD_VIEW_W; sv.tex_h = GD_VIEW_H;
                sv.noise = gs->noise;
                sv.ended = route == GLED_STATIC;
                sv.standby = route == GLED_STANDBY;
                sv.hide_hud = 0;                 /* pantalla LED (espera, vuelo, estática): el panel plano del HUD del jugador sigue; solo el HUD propio del Nikita (ENEMY) se recorta del panel (XRPresent.weaponHudMask) */
                nikita_hud(&sv, sv.cam_valid && !sv.ended);
            }
        }
    }
    pthread_mutex_lock(&HM);
    pub_special = sv;
    pub_special_ser++;
    pthread_mutex_unlock(&HM);
}

/* Suelo REAL bajo (x, z) (espacio de usuario). El juego decide si se puede colocar (snake.c sub_8004E5E8 / sub_8004E588): HZD_LevelHazardCheck(HZD_CHK_FLOOR) en el hueso del pecho (objs[4]) + (150, 0, 50); acepta si el suelo
 * queda a menos de 350 uds bajo ese hueso o el techo a menos de 125 uds sobre él, si no rechaza (no es un sondeo del claymore: claymore_loader_helper_800735A0 sondea hacia delante, alcance de la explosión). Aquí solo se
 * da la altura: un rayo hacia abajo contra la geometría sólida nativa (mv_vrhost_ray_distance: es geometría de render, así que puede incluir objetos además de los suelos de colisión) desde `top` (mano) o, si la mano está
 * más baja, PLACE_FLOOR_STEP_M sobre los pies (escalón), hasta PLACE_FLOOR_DROP_M bajo ellos (escalera). Origen (x, y0, z) y dirección (0, -1, 0) en espacio de usuario, pasados al motor con vrfire_pos/dir_to_world; mv_vrhost_ray_distance
 * devuelve mm del motor y se divide por units_per_m. Sin impacto (geometría no disponible o suelo fuera de alcance) devuelve NAN: la mina NO se coloca (caer al plano de los pies la dejaba en el aire, no hay forma de saber si ahí hay suelo).
 * Falta para usar el suelo de COLISIÓN del juego (HZD_CHK_FLOOR, snake.c sub_8004E5E8): vr/camera/vr_hzd*.h solo lee las PAREDES del mapa (grupos HZD +16, paredes de 16 B); el suelo son HZD_FLR (libhzd/level.c,
 * HZD_LevelHazardCheck: cuadriláteros con altura por nivel) y no hay lector en RAM; habría que añadirlo (estructura de HZD_FLR del EXE EU, sin medir). */
#define PLACE_FLOOR_STEP_M 0.5f
#define PLACE_FLOOR_DROP_M 1.5f
static struct { float floor, hand, org[3]; int hit; } mine_dbg;      /* última colocación de mina probada (registro [mina]): suelo del rayo, mano, impacto, origen de la mina */
static float place_floor_y(float x, float z, float top) {
    const float y0 = top > G.floor_u + PLACE_FLOOR_STEP_M ? top : G.floor_u + PLACE_FLOOR_STEP_M;
    const float o[3] = {x, y0, z}, d[3] = {0.0f, -1.0f, 0.0f}, upm = G.map.units_per_m;
    float ow[3], dw[3];
    vrfire_pos_to_world(&G.map, o, ow); vrfire_dir_to_world(&G.map, d, dw);
    const float t = mv_vrhost_ray_distance(ow, dw, (y0 - (G.floor_u - PLACE_FLOOR_DROP_M)) * upm);
    return t >= 0.0f && isfinite(t) ? y0 - t / upm : NAN;
}

/* ---- colocación (mina, carga adhesiva) ----
 * El juego decide la superficie y si se puede; aquí solo se da la pose. El jugador puede estar SENTADO: no se exige acercar
 * la mano al suelo físico. Desde la pose del objeto en la mano se traza el rayo de su "delante" (-Z) y se proyecta sobre el
 * suelo del juego (el real bajo el punto, place_floor_y; sin impacto del rayo se rechaza); sin inclinación hacia abajo se coloca a la distancia máxima
 * alcanzable (pared: no hay geometría del escenario en esta capa, es aproximado). En los dos casos la mina se apoya en el suelo por las
 * patas: el origen del modelo 0x17a1 queda WGHOST_MINE_LEGS_U uds sobre él; sin suelo conocido no se coloca. Dentro de PLACE_REACH_M
 * de la cabeza; fuera se rechaza (el arma no gasta nada). Devuelve 0 si se rechaza. */
static int place_project(const WPose *obj, WPose *out) {
    mine_dbg.floor = NAN; mine_dbg.hand = obj->p.y; mine_dbg.hit = 0; mine_dbg.org[0] = mine_dbg.org[1] = mine_dbg.org[2] = NAN;
    if (!isfinite(obj->p.x) || !isfinite(obj->p.y) || !isfinite(obj->p.z) || !isfinite(obj->q.x) || !isfinite(obj->q.y) || !isfinite(obj->q.z) ||
        !isfinite(obj->q.w) || !isfinite(G.head.p.x) || !isfinite(G.head.p.z)) return 0;   /* mano no rastreada / pose inválida: no se coloca */
    if (!G.floor_ok || !G.map.valid || !(G.map.units_per_m > 0)) return 0;      /* la altura de la mina sale del suelo: sin él no hay dónde apoyarla */
    float qn = sqrtf(obj->q.x * obj->q.x + obj->q.y * obj->q.y + obj->q.z * obj->q.z + obj->q.w * obj->q.w);
    if (!(qn > 1e-3f)) return 0;
    const float qx = obj->q.x / qn, qy = obj->q.y / qn, qz = obj->q.z / qn, qw = obj->q.w / qn;   /* la fórmula del "delante" exige cuaternión unitario */
    float fx = -2 * (qx * qz + qw * qy);
    float fy = -2 * (qy * qz - qw * qx);
    float fz = -(1 - 2 * (qx * qx + qy * qy));
    float hx = obj->p.x - G.head.p.x, hz = obj->p.z - G.head.p.z;
    float p[3] = {obj->p.x, obj->p.y, obj->p.z};
    float gy = G.floor_u;                                             /* suelo bajo el punto de colocación: el real del mapa (place_floor_y), no el plano de los pies */
    if (fy < -0.2f && obj->p.y > G.floor_u) {                         /* apunta hacia abajo: suelo (dos pasadas: el suelo real puede estar a otra altura que el de los pies) */
        for (int it = 0; it < 2 && obj->p.y > gy; it++) {
            float t = (gy - obj->p.y) / fy;
            p[0] = obj->p.x + fx * t; p[2] = obj->p.z + fz * t;
            gy = place_floor_y(p[0], p[2], obj->p.y);
            if (!isfinite(gy)) return 0;                              /* sin impacto: no se coloca */
        }
    } else {                                                          /* horizontal: a la distancia máxima, apoyada en el suelo bajo ese punto */
        float hl = sqrtf(fx * fx + fz * fz);
        if (hl < 1e-3f) return 0;
        float t = PLACE_REACH_M * 0.8f - sqrtf(hx * hx + hz * hz);
        if (t < 0.1f) t = 0.1f;
        p[0] = obj->p.x + fx / hl * t; p[2] = obj->p.z + fz / hl * t;
        gy = place_floor_y(p[0], p[2], obj->p.y);
        if (!isfinite(gy)) return 0;                                  /* sin impacto: no se coloca */
    }
    p[1] = gy + WGHOST_MINE_LEGS_U / G.map.units_per_m;               /* origen del modelo con las patas en el suelo */
    mine_dbg.floor = gy; mine_dbg.hit = 1; mine_dbg.org[0] = p[0]; mine_dbg.org[1] = p[1]; mine_dbg.org[2] = p[2];
    float dx = p[0] - G.head.p.x, dz = p[2] - G.head.p.z;
    if (!(sqrtf(dx * dx + dz * dz) <= PLACE_REACH_M) || !isfinite(p[1])) return 0;
    out->p.x = p[0]; out->p.y = p[1]; out->p.z = p[2];
    /* orientación: solo la guiñada del "delante" del evento (-Z = cara picuda de la mina, w_placed.c MINE_FACE_Q) y el modelo girado R_y(+90) sobre ella: su cara picuda (+X del modelo 0x17a1) mira hacia donde apunta el jugador */
    float hl = sqrtf(fx * fx + fz * fz);
    float yaw = (hl > 1e-3f ? atan2f(-fx, -fz) : 0.0f) + 1.5707963f;
    out->q.x = 0; out->q.y = sinf(yaw * 0.5f); out->q.z = 0; out->q.w = cosf(yaw * 0.5f);
    return 1;
}

/* ---- carga adhesiva (C4): colocación por rayo ----
 * El rayo parte de la cara adhesiva de la carga en la mano y sigue su "delante" (-Z) hasta PLACE_RAY_M (alcance de brazo). Impacta con el primero de: el suelo del juego, las paredes de
 * colisión HZD (vrhzd_raycast) y la geometría sólida nativa (mv_vrhost_ray_distance: suelos, objetos). La carga queda en el punto del impacto, con la cara adhesiva apoyada en la superficie
 * y el eje Z del modelo (c4_bomb, medido en STAGE.DIR: cara adhesiva en z = -79, antena hasta z = 170) saliendo de ella; suelo/techo: el eje largo sigue la guiñada de la mano; pared: vertical.
 * Sin impacto en el alcance, o con el punto a más de PLACE_REACH_M de la cabeza: se rechaza (la carga sigue en la mano). */
#define PLACE_RAY_M 1.0f
#define C4_LIFT_M 0.079f
static const uint8_t *place_ram;      /* RAM del juego del VBlank en curso (vrgame_pad), para los rayos HZD */
static const BridgeState *place_bs;   /* puente del VBlank en curso (vrgame_pad), para el blanco C4 del juego */
/* Blanco al que el juego pegaría la carga: réplica de sna_8004FDE8 / GM_C4Target / TargetConflict (sna_init.c, game/target.c). field_8E8 del jugador solo se rellena al empezar la animación de ataque, no se puede leer antes;
 * se replica su prueba: caja de sonda stru_8009EFE4[0] (centro (0,0,300) delante del jugador, semitamaños (400,600,400)) contra los blancos en el ORDEN DEL JUEGO; el juego se queda con el PRIMERO con clase C4 (64),
 * sin el bit 64 en damaged, de otro bando (side & 1 == 0), dentro de la caja y con MapConflict (player_map & map; solo si el puente da BRIDGE_OK_PLAYER_ST). Después, si ese blanco tiene clase 0x20 no se pega, y si sna_line_check
 * (SEGMENT_ATR = HZD_SEG_NO_COLLIDE, sna_init.c: aquí vrhzd_raycast con skip 1, las paredes marcadas NO_COLLIDE no cuentan) halla pared entre Snake (ctl_pos) y el centro del blanco, el juego lo descarta.
 * El juego pega la carga al cuerpo del soldado (NewBakudan attached=1, desplazamiento fijo en su espacio local; sna_800571B8) con SU blanco, no con el punto del rayo VR: solo se deja pasar al juego si el rayo
 * TOCA a ese mismo blanco. Devuelve 1 si lo hay; *t_touch = unidades hasta el primer soldado enemigo tocado por el rayo (con o sin la prueba del juego), -1 ninguno. */
#define C4_AIM_TOL 150.0f       /* holgura (unidades = mm) del rayo contra la caja del soldado */
static int c4_aim_target(const float ow[3], const float dw[3], float reach, float *t_touch) {
    *t_touch = -1.0f;
    const BridgeState *bs = place_bs;
    if (!bs || !(bs->valid & BRIDGE_OK_TARGETS)) return 0;
    const float yaw = (float)bs->ctl_rot.y * (6.2831853f / 4096.0f), sn = sinf(yaw), cs = cosf(yaw);
    const float cx = (float)bs->ctl_pos.x + 300.0f * sn, cy = (float)bs->ctl_pos.y, cz = (float)bs->ctl_pos.z + 300.0f * cs;
    int game = -1, touched = -1;
    for (int i = 0; i < bs->n_targets && i < BRIDGE_MAX_TARGETS; i++) {
        const BridgeTarget *t = &bs->targets[i];
        if (!(t->cls & 64) || (t->side & 1)) continue;
        const float c[3] = {(float)t->center.x, (float)t->center.y, (float)t->center.z}, hs[3] = {(float)t->half_size.x, (float)t->half_size.y, (float)t->half_size.z};
        if (game < 0 && !(t->damaged & 64) && fabsf(c[0] - cx) <= hs[0] + 400.0f && fabsf(c[2] - cz) <= hs[2] + 400.0f && fabsf(c[1] - cy) <= hs[1] + 600.0f &&
            (!(bs->valid & BRIDGE_OK_PLAYER_ST) || (bs->player_map & t->map))) game = i;
        float tn = 0.0f, tf = reach;
        int hit = 1;
        for (int k = 0; k < 3 && hit; k++) {      /* rayo contra caja alineada a los ejes (losas) */
            const float lo = c[k] - hs[k] - C4_AIM_TOL - ow[k], hi = c[k] + hs[k] + C4_AIM_TOL - ow[k];
            if (fabsf(dw[k]) < 1e-6f) { if (lo > 0.0f || hi < 0.0f) hit = 0; continue; }
            float t0 = lo / dw[k], t1 = hi / dw[k];
            if (t0 > t1) { const float x = t0; t0 = t1; t1 = x; }
            if (t0 > tn) tn = t0;
            if (t1 < tf) tf = t1;
            if (tn > tf) hit = 0;
        }
        if (hit && (*t_touch < 0.0f || tn < *t_touch)) { *t_touch = tn; touched = i; }
    }
    if (game < 0 || game != touched) return 0;                                  /* el juego tomaría a otro (o a ninguno): la carga no se pega a este */
    const BridgeTarget *g = &bs->targets[game];
    if (g->cls & 0x20) return 0;                                                  /* sna_8004FDE8: clase 0x20 -> descarta el blanco y no pega */
    if (place_ram) {                                                              /* sna_line_check(ctl_pos -> centro del blanco): con pared en medio el juego lo descarta */
        const float o[3] = {(float)bs->ctl_pos.x, (float)bs->ctl_pos.y, (float)bs->ctl_pos.z}, d[3] = {(float)g->center.x - o[0], (float)g->center.y - o[1], (float)g->center.z - o[2]};
        const float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dl > 1.0f && vrhzd_raycast(place_ram, o, d, dl, 1u) >= 0.0f) return 0;
    }
    return 1;
}
static int place_c4(const WPose *obj, WPose *out, int *via_game) {      /* via_game = 1: la pose no vale, la carga se pega a un blanco del juego (adherida a un enemigo) y la pone el propio juego */
    *via_game = 0;
    if (!isfinite(obj->p.x) || !isfinite(obj->p.y) || !isfinite(obj->p.z) || !isfinite(obj->q.x) || !isfinite(obj->q.y) || !isfinite(obj->q.z) || !isfinite(obj->q.w) ||
        !isfinite(G.head.p.x) || !isfinite(G.head.p.z) || !G.map.valid || !(G.map.units_per_m > 0)) return 0;
    float qn = sqrtf(obj->q.x * obj->q.x + obj->q.y * obj->q.y + obj->q.z * obj->q.z + obj->q.w * obj->q.w);
    if (!(qn > 1e-3f)) return 0;
    const float qx = obj->q.x / qn, qy = obj->q.y / qn, qz = obj->q.z / qn, qw = obj->q.w / qn;
    const float f[3] = {-2 * (qx * qz + qw * qy), -2 * (qy * qz - qw * qx), -(1 - 2 * (qx * qx + qy * qy))};
    const float o[3] = {obj->p.x, obj->p.y, obj->p.z}, upm = G.map.units_per_m;
    float best = -1.0f, tplane = -1.0f;
    int on_floor = 0;
    if (G.floor_ok && f[1] < -0.05f && o[1] > G.floor_u) {                       /* suelo del juego (plano a la altura de los pies): solo respaldo o confirmado por un impacto a +-5 cm */
        float t = (G.floor_u - o[1]) / f[1];
        if (t > 0.0f && t <= PLACE_RAY_M) tplane = t;
    }
    float ow[3], dw[3];
    vrfire_pos_to_world(&G.map, o, ow); vrfire_dir_to_world(&G.map, f, dw);
    if (place_ram) {
        const float reach = PLACE_RAY_M * upm;
        const float th = vrhzd_raycast(place_ram, ow, dw, reach, 0u);              /* paredes de colisión del juego */
        const float ts = mv_vrhost_ray_distance(ow, dw, th >= 0.0f ? th : reach);  /* geometría sólida nativa: suelos y objetos */
        const float tw = ts >= 0.0f ? ts : th;
        if (tw >= 0.0f) { best = tw / upm; on_floor = tplane >= 0.0f && fabsf(best - tplane) <= 0.05f; }
    }
    if (best < 0.0f && tplane >= 0.0f) { best = tplane; on_floor = 1; }          /* sin impacto: el plano de los pies (nunca gana a una colisión más lejana: pasado un borde la carga quedaría flotando) */
    if (!isfinite(best)) return 0;
    float t_sold;
    const int sold_ok = c4_aim_target(ow, dw, PLACE_RAY_M * upm, &t_sold);
    if (sold_ok && t_sold >= 0.0f && (!(best >= 0.0f) || t_sold / upm <= best + 0.05f)) {      /* el rayo toca al soldado que el juego tomaría, antes que a una superficie: apuntas al soldado, no a lo que hay tras él */
        *out = *obj; *via_game = 1; return 1;                                         /* el juego la pega al soldado (NewBakudan attached=1; hle_vrfire_charge ignora la pose) y la lleva con él */
    }
    if (!(best >= 0.0f)) return 0;                                                    /* sin superficie y sin blanco del juego: con la pose de la mano la carga quedaría flotando */
    float p[3] = {o[0] + f[0] * best, o[1] + f[1] * best, o[2] + f[2] * best}, n[3] = {0, 1, 0};
    if (on_floor || (G.floor_ok && p[1] <= G.floor_u + 0.04f && f[1] < 0.0f) || f[1] < -0.7f) { n[0] = 0; n[1] = 1; n[2] = 0; }   /* suelo o cara superior */
    else if (f[1] > 0.7f) { n[1] = -1; }                                                                                             /* techo */
    else {                                                                                                                           /* pared: normal horizontal hacia el lado libre */
        float hl = sqrtf(f[0] * f[0] + f[2] * f[2]);
        n[0] = hl > 1e-3f ? -f[0] / hl : 0; n[1] = 0; n[2] = hl > 1e-3f ? -f[2] / hl : 1;
        if (place_ram) {      /* normal de la pared HZD: tangente por dos rayos más, ±20 grados de guiñada en torno al rayo (solo vrhzd_raycast, que sí está en la app) */
            float hw = sqrtf(dw[0] * dw[0] + dw[2] * dw[2]), pt[2][2];
            int okk = hw > 1e-3f;
            for (int k = 0; okk && k < 2; k++) {
                const float a = (k ? 0.35f : -0.35f), c = cosf(a), sn = sinf(a);
                const float rd[3] = {(dw[0] * c - dw[2] * sn) / hw, 0.0f, (dw[0] * sn + dw[2] * c) / hw};
                const float tt = vrhzd_raycast(place_ram, ow, rd, PLACE_RAY_M * upm * 1.5f, 0u);
                if (tt < 0.0f) okk = 0; else { pt[k][0] = rd[0] * tt; pt[k][1] = rd[2] * tt; }
            }
            if (okk) {
                float tx = pt[1][0] - pt[0][0], tz = pt[1][1] - pt[0][1], tl = sqrtf(tx * tx + tz * tz);
                if (tl > 1.0f) {
                    float nw[3] = {-tz / tl, 0.0f, tx / tl}, nu[3];
                    if (nw[0] * dw[0] + nw[2] * dw[2] > 0.0f) { nw[0] = -nw[0]; nw[2] = -nw[2]; }       /* hacia el lado de la mano */
                    vrfire_dir_from_world(&G.map, nw, nu);
                    float l = sqrtf(nu[0] * nu[0] + nu[2] * nu[2]);
                    if (l > 1e-3f && isfinite(l)) { n[0] = nu[0] / l; n[2] = nu[2] / l; }
                }
            }
        }
    }
    float dx = p[0] - G.head.p.x, dz = p[2] - G.head.p.z;
    if (!(sqrtf(dx * dx + dz * dz) <= PLACE_REACH_M)) return 0;
    out->p.x = p[0] + n[0] * C4_LIFT_M; out->p.y = p[1] + n[1] * C4_LIFT_M; out->p.z = p[2] + n[2] * C4_LIFT_M;
    /* "delante" de la pose = hacia la superficie (-n); arriba de la pose = n (el eje Z del modelo sale de la superficie) */
    float fp[3] = {-n[0], -n[1], -n[2]};
    if (fabsf(n[1]) > 0.9f) {                                                       /* suelo/techo: eje largo por la guiñada de la mano */
        float hl = sqrtf(f[0] * f[0] + f[2] * f[2]);
        if (hl > 1e-3f) { fp[0] = f[0] / hl; fp[1] = 0; fp[2] = f[2] / hl; } else { fp[0] = 0; fp[1] = 0; fp[2] = -1; }
    } else { fp[0] = 0; fp[1] = -1; fp[2] = 0; }                                    /* pared: eje largo vertical */
    float zx = -fp[0], zy = -fp[1], zz = -fp[2];                                    /* Z local (atrás) = -delante; X = Y x Z */
    float xx = n[1] * zz - n[2] * zy, xy = n[2] * zx - n[0] * zz, xz = n[0] * zy - n[1] * zx;
    float m[16] = {xx, xy, xz, 0, n[0], n[1], n[2], 0, zx, zy, zz, 0, 0, 0, 0, 1}, q[4];
    q_from_m16(m, q);
    out->q.x = q[0]; out->q.y = q[1]; out->q.z = q[2]; out->q.w = q[3];
    return 1;
}

/* Cargas vivas del juego (bakudan_count, 0x800A0604; NewBakudan no crea la 17ª). Como c4_placed, más abajo, pero antes de su definición. */
static int c4_live_count(const uint8_t *ram) { int32_t n; memcpy(&n, ram + (VRFIRE_BA_C4_COUNT & 0x1FFFFFu), 4); return n; }

static int launch_hook(void *user, const WLaunch *ev) {
    (void)user;
    if (!ev) return 0;
    switch (ev->kind) {
    case WL_THROW:
    case WL_INHAND: {
        if (G.ammo_game <= 0 || !G.map.valid) { G.rejects++; return 0; }
        float v[3] = {ev->lin_vel.x, ev->lin_vel.y, ev->lin_vel.z};
        if (ev->kind == WL_INHAND) v[0] = v[1] = v[2] = 0;     /* cocinada hasta el final en la mano: cae a los pies (mecha del juego) */
        float p[3] = {ev->pose.p.x, ev->pose.p.y, ev->pose.p.z};
        const int ok = vrfire_set_throw(p, v, THROW_TICK_S);
        float pe[3];
        vrfire_pos_to_world(&G.map, p, pe);
        static const char *const LM[2] = {"suave", "maximo"};
        fprintf(stderr, "[lanzar] arma=%d modo=%s v_gesto=%.2f m/s elevacion=%.0f grados (salida SIEMPRE al maximo de su tipo del juego: granada 4,93 / aturdidora 5,15 / chaff 1,13 m/s; suave < 1 m/s = velocidad medida) v_mando=%.2f v_usada=%.2f m/s ventana=%.0f ms (acaba %.0f ms antes del ultimo muestreo) soltar=%.0f ms tras empezar a abrir el gatillo v_pico=%.2f v_entre_poses=%.2f m/s v_actor=%.2f m/s (%.2f %.2f %.2f) angular=%.1f rad/s pos_motor=(%.0f %.0f %.0f) aceptado=%d\n", ev->weapon_id,
                ev->kind == WL_INHAND ? "en_mano" : LM[ev->thr_mode >= 0 && ev->thr_mode <= 1 ? ev->thr_mode : 0], ev->v_gest, ev->elev_deg, ev->v_hand, ev->v_used, 1000.0f * ev->win_len, 1000.0f * ev->win_lag, 1000.0f * ev->rel_lag, ev->v_peak, ev->v_inst, vrfire_throw_mps(), v[0], v[1], v[2], wv_len(ev->ang_vel), pe[0], pe[1], pe[2], ok);
        if (!ok) { G.rejects++; return 0; }
        G.fire_edge = 1; G.fire_pose = ev->pose; G.shots++; G.throws++;
        return 1; }
    case WL_PLACE_C:
    case WL_PLACE_MINE: {
        if (G.ammo_game <= 0 || !G.map.valid) { G.rejects++; return 0; }
        WPose pp;
        int via_game = 1;
        const int placed = ev->kind == WL_PLACE_C ? place_c4(&ev->pose, &pp, &via_game) : place_project(&ev->pose, &pp);
        if (ev->kind == WL_PLACE_MINE)
            fprintf(stderr, "[mina] colocar=%s suelo_rayo=%.3f pies=%.3f mano=%.3f impacto=%s origen_mina=(%.3f %.3f %.3f) m (espacio de usuario)\n", placed ? "si" : "rechazada", mine_dbg.floor, G.floor_u, mine_dbg.hand,
                    mine_dbg.hit ? "si" : "no", mine_dbg.org[0], mine_dbg.org[1], mine_dbg.org[2]);
        if (!placed) { G.rejects++; return 0; }
        if (ev->kind == WL_PLACE_C && !via_game) {      /* C4 en la pared o el suelo: el actor se crea directamente donde apunta el mando (vr_fire.h), SIN el ataque del juego: Snake no hace ninguna animación */
            if ((place_ram && c4_live_count(place_ram) >= 16) || !vrfire_c4_request(&G.map, &pp)) { G.rejects++; return 0; }
            G.shots++; G.places++;
            return 1;
        }
        G.fire_edge = 1; G.fire_pose = pp; G.shots++; G.places++;
        return 1; }
    case WL_DETONATE:
        G.det_t = DET_PRESS_VBL; G.dets++;
        return 1;
    default:
        return 0;
    }
}

/* Sondeo de colocación sin efectos (w_throw.h w5_probe): la misma prueba que al soltar el gatillo, sin consumir nada. Devuelve 1 si hay dónde colocar y deja la pose resultante en G.probe_pose. */
static int probe_hook(void *user, const WLaunch *ev) {
    (void)user;
    if (!ev || G.ammo_game <= 0 || !G.map.valid) return 0;
    WPose pp;
    int via_game;
    const int ok = ev->kind == WL_PLACE_C ? place_c4(&ev->pose, &pp, &via_game) : ev->kind == WL_PLACE_MINE ? place_project(&ev->pose, &pp) : 0;
    if (ok) { G.probe_ok = 1; G.probe_pose = pp; }
    return ok;
}

static int ik_probe(const uint8_t *ram);      /* regla de la IK de brazos con el motor ocultando a Snake: más abajo, junto a is_duct */
static void init_once(void) {
    if (G.inited) return;
    G.inited = 1;
    vrbody_set_hidden_ik_fn(ik_probe);
    VrFireCfg fc;
    vrfire_cfg_default(&fc);
    vrfire_init(&fc);
    VrAaCfg ac;       /* apuntado del lanzador antiaéreo con el buscador del juego (vr_aa.h) */
    vraa_cfg_default(&ac);
    if (vrcam_config()->engine_h > 0) ac.h_scale = 320.0f / (float)vrcam_config()->engine_h;   /* el visor usa la H original del juego */
    vraa_init(&ac);
    vrreload_init();
    memset(&G.br, 0, sizeof G.br);
    G.br.ammo_reserve = cb_ammo_reserve;
    G.br.fire = cb_fire;
    G.br.haptic = cb_haptic;
    G.br.projectile = cb_projectile;
    G.br.steer = cb_steer;
    vrmis_reset();
    G.cur = -1;
    w_throw_set_hook(launch_hook, NULL);
    w_throw_set_probe(probe_hook, NULL);
}

static void head_pose(WPose *h) {
    float m[16];
    memset(h, 0, sizeof *h);
    h->q.w = 1;
    if (!mv_vr_get_head(m, NULL) || !finite_n(m, 16)) return;
    float q[4];
    q_from_m16(m, q);
    h->p.x = m[12]; h->p.y = m[13]; h->p.z = m[14];
    h->q.x = q[0]; h->q.y = q[1]; h->q.z = q[2]; h->q.w = q[3];
}
static WPose wpose_of(const GripPose *g) {
    WPose w;
    w.p.x = g->p[0]; w.p.y = g->p[1]; w.p.z = g->p[2];
    w.q.x = g->q[0]; w.q.y = g->q[1]; w.q.z = g->q[2]; w.q.w = g->q[3];
    return w;
}

static VrGameDriver drv_fn;
int vrgame_hand_raw(int side, float p[3], float *grip, float *trig, double *t_s) {
    if (side < 0 || side > 1) return 0;
    pthread_mutex_lock(&HM);
    GripPose g = grip_locked(&hand_in[side], mono_ns());
    float gr = hand_in[side].grip, tg = hand_in[side].trigger;
    uint64_t tn = hand_in[side].t_ns;
    pthread_mutex_unlock(&HM);
    if (!g.valid) return 0;
    memcpy(p, g.p, sizeof g.p);
    if (grip) *grip = gr;
    if (trig) *trig = tg;
    if (t_s) *t_s = drv_fn ? 0.0 : (double)tn * 1e-9;     /* con guion de pruebas (VBlanks más rápidos que el reloj) el tiempo es el del VBlank: lo pone el consumidor */
    return 1;
}
/* arma VR mostrada */
int vrgame_weapon_engaged(void) { return G.active || G.cur >= 0; }
/* arrojadiza VR en la mano del arma, sin preparar o preparada (vr_gesture throw_hold: sin golpes ni agarres; en la zona del pecho su gatillo se bloquea y abre la ventana como el de cualquier arma) */
int vrgame_throw_hold(void) { return G.throw_hold; }
int vrgame_glove_pub(void) { return glv_pub; }
void vrgame_haptic(int side, float s) {
    if (side < 0 || side > 1 || !isfinite(s) || s <= 0) return;
    pthread_mutex_lock(&HM);
    if (s > hap_out[side]) hap_out[side] = s > 1 ? 1 : s;
    pthread_mutex_unlock(&HM);
}

/* ---- guantes: construcción (hilo de emulación) ---- */
void vrgame_set_gloves(int on, const uint16_t *vram) {
    pthread_mutex_lock(&HM);
    glv_on = on != 0; glv_vram = vram;
    pthread_mutex_unlock(&HM);
}
/* Una vez por VBlank, con la RAM del juego en reposo (sin matrices IK ni mallas deformadas escritas por vr/body): lee los 6 modelos de los brazos, y si lo que lee (geometría, CLUT, texels) cambió
 * construye mallas y atlas en un hueco libre y lo publica. own_hands solo se activa con un guante publicado: nunca se ocultan los brazos del juego sin tener con qué sustituirlos. */
static void glove_update(const uint8_t *ram) {
    static int last_rc = 1;
    static unsigned since_build = GLOVE_REBUILD_VB;
    pthread_mutex_lock(&HM);
    int on = glv_on && G.body_ok;
    const uint16_t *vram = glv_vram;
    int pub = glv_pub, rd = glv_read;
    uint64_t ph = pub >= 0 ? glv[pub]->hash : 0;
    pthread_mutex_unlock(&HM);
    if (!on || !vram) { vrbody_set_own_hands(0); return; }
    vrbody_set_own_hands(pub >= 0);
    uint32_t md[2][3];
    if (since_build < GLOVE_REBUILD_VB) since_build++;
    if (!ram || vrbody_ram_dirty() || !vrbody_player_models(ram, md)) return;
    if (pub >= 0 && since_build < GLOVE_REBUILD_VB) return;   /* con guante publicado, a lo sumo una comprobación/reconstrucción cada GLOVE_REBUILD_VB VBlanks */
    since_build = 0;
    int slot = 0;
    while (slot == pub || slot == rd) slot++;                 /* hay uno libre: solo 2 excluidos de 3 */
    if (!glv[slot]) glv[slot] = vrglove_create();
    if (!glv[slot]) return;
    int rc = vrglove_build(glv[slot], ram, vram, md, ph);
    if (rc == 1) {
        pthread_mutex_lock(&HM);
        glv_pub = slot;
        pthread_mutex_unlock(&HM);
        vrbody_set_own_hands(1);
    } else if (rc < 0 && pub >= 0) {      /* el guante publicado ya no es reconstruible: vuelven los brazos del juego */
        pthread_mutex_lock(&HM);
        glv_pub = -1;
        pthread_mutex_unlock(&HM);
        vrbody_set_own_hands(0);
    }
    if (rc < 0 && rc != last_rc) fprintf(stderr, "[guante] no se pudo construir (rc=%d): se siguen dibujando los brazos del juego\n", rc);
    last_rc = rc;
}

/* Pose de cada mano nueva (vr_hand_pose): la del objeto que sostiene (tabla de vr_grip por arma; la mano de apoyo solo si ya agarra el arma) o sin objeto (HP_RELAX: el agarre cierra el puño), con el
 * gatillo y el pulgar sobre los botones del mando. Se publica una vez por VBlank y lo lee la escena nativa (ns_hands_finalize). */
static struct { int valid, item; float trigger, thumb; } pub_hp[2];
static void publish_hand_pose(const HandSlot hs[2], const GripPose gp[2], int right, const VrGripEntry *ge, int support) {
    pthread_mutex_lock(&HM);
    for (int i = 0; i < 2; i++) {
        pub_hp[i].valid = gp[i].valid;
        pub_hp[i].item = HP_RELAX;
        if (ge) { if (i == (right ? 1 : 0)) pub_hp[i].item = ge->pose; else if (support && ge->support_pose >= 0) pub_hp[i].item = ge->support_pose; }
        pub_hp[i].trigger = gp[i].valid ? fminf(fmaxf(hs[i].trigger, 0.0f), 1.0f) : 0.0f;
        pub_hp[i].thumb = gp[i].valid && (hs[i].low || hs[i].up || hs[i].stick) ? 1.0f : 0.0f;
    }
    pthread_mutex_unlock(&HM);
}
int vrgame_hand_pose_in(int side, int *item_pose, float *trigger, float *thumb) {
    *item_pose = HP_RELAX; *trigger = 0; *thumb = 0;
    if (side < 0 || side > 1) return 0;
    pthread_mutex_lock(&HM);
    int ok = pub_hp[side].valid;
    if (ok) { *item_pose = pub_hp[side].item; *trigger = pub_hp[side].trigger; *thumb = pub_hp[side].thumb; }
    pthread_mutex_unlock(&HM);
    return ok;
}

/* Marco de la mano del mando (el de hand_frame_from_grip, espacio del usuario, m) con el que se simuló la última IK de ese brazo: el agarre FILTRADO que usó vr_body. Hilo de emulación. */
int vrgame_hand_frame_sim(int side, float p[3], float q[4]) {
    VrBodyArmLive a;
    if (!p || !q || (side != 0 && side != 1) || !vrbody_arm_live(side, &a)) return 0;
    GripPose g;
    memset(&g, 0, sizeof g);
    memcpy(g.p, a.grip_p, sizeof g.p); memcpy(g.q, a.grip_q, sizeof g.q); g.valid = finite_n(g.p, 3) && finite_n(g.q, 4);
    GripPose h = hand_frame_from_grip(side, &g);
    if (!h.valid) return 0;
    memcpy(p, h.p, sizeof h.p); memcpy(q, h.q, sizeof h.q);
    return 1;
}
/* El mismo marco con el agarre del mando de AHORA (el que cuelga el arma). Cualquier hilo. */
int vrgame_hand_frame_now(int side, float p[3], float q[4]) {
    if (!p || !q || (side != 0 && side != 1)) return 0;
    pthread_mutex_lock(&HM);
    GripPose h = hand_frame_locked(side, mono_ns());
    pthread_mutex_unlock(&HM);
    if (!h.valid) return 0;
    memcpy(p, h.p, sizeof h.p); memcpy(q, h.q, sizeof h.q);
    return 1;
}
int vrgame_weld_main(void) {
    pthread_mutex_lock(&HM);
    const int m = pub_weld.on ? 1 - pub_weld.side : -1;
    pthread_mutex_unlock(&HM);
    return m;
}
int vrgame_native_hand(void) { return atomic_load_explicit(&native_hand, memory_order_relaxed); }
static _Atomic int view_hide;   /* vrhost: prismáticos del juego activos (mundo ampliado): el arma no se ve, como en el conducto */
void vrgame_set_view_hide(int on) { atomic_store_explicit(&view_hide, on != 0, memory_order_relaxed); }
/* Conducto (estado INTRUDE) o tumbado (stance 2 / prone) sin PSG1 (gid 9: el original tumba a Snake al sacarlo y el rifle VR se sigue viendo), o prismáticos del juego (el arma VR no lleva el aumento del mundo): el arma no se ve ni se lleva a la mano */
static int is_duct(const BridgeState *bs, int gid) {
    return (bs->player_status & BRIDGE_PS_INTRUDE) != 0 || (gid != 9 && (bs->stance == 2 || bs->prone)) || atomic_load_explicit(&view_hide, memory_order_relaxed);
}
/* Se pide la IK de brazos aunque el motor oculte a Snake (vrbody_set_engine_hidden_ik): arma de dos manos mostrada (modelo cargado) con la cámara VR y el jugador al mando o con un menú / la pausa encima (cam_ctl), sin cinemática ni escena.
 * Caja, derribo y muerte los descarta vr_body. Es la regla del VBlank (pad_core) y de la sonda del apply (ik_probe). */
static int ik_wanted(const BridgeState *bs) {
    if (!bs || (bs->valid & BRIDGE_OK_ALL_CORE) != BRIDGE_OK_ALL_CORE || !bs->has_player || !(bs->in_control || bs->cam_ctl) || bs->cinematic || !G.fire_ok || !vrcam_active()) return 0;
    const int slot = slot_of(bs->weapon);
    if (slot < 0 || atomic_load_explicit(&SL[slot].state, memory_order_acquire) != LD_READY) return 0;
    const VrGripEntry *ge = vrgrip_find(WPN_TAB[slot].game_id);
    return ge && ge->support_pose >= 0 && !is_duct(bs, WPN_TAB[slot].game_id);
}
int vrgame_ik_hidden_wanted(const BridgeState *bs) { return ik_wanted(bs); }
/* El apply corre en el dibujado, después del VBlank que fijó la petición: al equipar un arma de dos manos ya cargada el motor oculta a Snake en ESE frame y la petición del VBlank siguiente llegaría un fotograma tarde */
static int ik_probe(const uint8_t *ram) {
    BridgeState b;
    return ram && bridge_read(ram, &b) ? ik_wanted(&b) : 0;
}

/* Pruebas: instala un arma ya construida en la ranura de su id (sin cargar modelos del disco). */
void vrgame_test_install(int game_id, void *def, void *state, void *rig) {
    int si = slot_of(game_id);
    if (si < 0) return;
    SL[si].def = (WeaponDef *)def; SL[si].st = (WeaponState *)state; SL[si].rig = (VrHandsRig *)rig;
    atomic_store(&SL[si].state, LD_READY);
}

/* Pruebas: la carga de la ranura solo de lógica (granada 7) tal como la hace el hilo de carga, sin disco. 1 = lista. */
int vrgame_test_load_logic(int game_id) {
    int si = slot_of(game_id);
    if (si < 0 || (WPN_TAB[si].model && !vrgame_weapon_uses_game_model(game_id)) || !load_logic_only(&SL[si], game_id)) return 0;
    SL[si].native = vrgame_weapon_uses_game_model(game_id);
    atomic_store(&SL[si].state, LD_READY);
    return 1;
}

void vrgame_test_clock(uint64_t ns) { atomic_store(&clk_fake_ns, ns); atomic_store_explicit(&clk_fake, ns != 0, memory_order_release); }
void vrgame_test_advance(uint64_t ns) { atomic_fetch_add(&clk_fake_ns, ns); }

/* Pruebas: mano del arma con la que se simuló el último frame publicado (pub_sim_hand); 0 si no hay frame publicado. */
int vrgame_test_sim_hand(float p[3], float q[4]) {
    pthread_mutex_lock(&HM);
    const int ok = pub_sim_valid;
    if (ok) { memcpy(p, pub_sim_hand.p, sizeof pub_sim_hand.p); memcpy(q, pub_sim_hand.q, sizeof pub_sim_hand.q); }
    pthread_mutex_unlock(&HM);
    return ok;
}
void vrgame_test_set_native(int game_id, int on) { int si = slot_of(game_id); if (si >= 0) SL[si].native = on != 0; }

static uint32_t drv_vbl;
void vrgame_set_driver(VrGameDriver f) { drv_fn = f; drv_vbl = 0; }

/* Supresor de la pistola: lo decide el juego, no el VR (el arma lo muestra con WGF_SUPP, ver weapon.h). Regla PROVISIONAL hasta conocer el campo de la RAM:
 * visible solo si el juego da el objeto «supresor» como poseído (item_count >= 0) Y equipado (item). TODO(supresor): índice 0..23 del objeto en BRIDGE_V_ITEM_COUNT,
 * aún sin identificar; mientras valga -1 el supresor NO se ve nunca. */
#define VRGAME_SUPPRESSOR_ITEM (-1)
static int game_has_suppressor(const BridgeState *bs) {
    if (!bs || VRGAME_SUPPRESSOR_ITEM < 0 || VRGAME_SUPPRESSOR_ITEM >= BRIDGE_N_ITEMS) return 0;
    return bs->item_count[VRGAME_SUPPRESSOR_ITEM] >= 0 && bs->item == VRGAME_SUPPRESSOR_ITEM;
}

/* Diagnóstico [arma] (una línea por segundo): por qué el arma VR se ve o no, y su estado. Los campos que aún no se conocen valen -1. */
extern int w_pist_trig_armed(const WeaponState *ws);   /* vr/weapons/w_pist.c */
extern const float w_pist_bare_muzzle_z;               /* vr/weapons/w_pist.c: boca del cañón sin supresor (la de w_pist al disparar) */
typedef struct {
    int eq_slot, load, mode, cam, core, gpw;
    int rstate, fire_allowed, engaged, supp;
    float trig_raw, trig;
    int trig_armed, chambered, mag_in;
    int gweapon, gitem, gammo;
    float d_grip, d_supp;
    int gmag, reserve, vr_rounds;              /* cargador del juego, reserva (total - cargador) y balas del arma VR (cargador + recámara; -1 sin arma VR) */
    int gmag_max, pk_pending;                  /* capacidad del cargador del juego (GM_MagazineMax) y recogida pendiente de rearmar el arma VR (vrreload_pickup_pending) */
} ArmaDiag;
static void arma_diag(uint64_t now, const ArmaDiag *a) {
    static uint64_t next_ns;
    if (now < next_ns) return;
    next_ns = now + 1000000000ull;
    VrReloadStats rls;
    vrreload_stats(&rls);
    fprintf(stderr, "[arma] slot=%d carga=%d modo=%d fire_ok=%d cam=%d nucleo=%d mano_arma_valida=%d | recarga_ocupada=%d disparo_permitido=%d gatillo=%.2f (crudo %.2f) armado=%d recamara=%d cargador=%d | fo.engaged=%d disparos=%u supresor=%d | juego: arma=%d objeto=%d municion=%d cargador_juego=%d reserva=%d balas_vr=%d | dist empuñadura_arma-agarre_mando=%.3fm mano_libre-soporte=%.3fm | recarga_vr: cargador_max_juego=%d recogida_pendiente=%d pulsaciones_vacio=%u cargas_recogida=%u correcciones=%u\n",
            a->eq_slot, a->load, a->mode, G.fire_ok, a->cam, a->core, a->gpw, a->rstate, a->fire_allowed, a->trig, a->trig_raw, a->trig_armed, a->chambered, a->mag_in,
            a->engaged, G.shots, a->supp, a->gweapon, a->gitem, a->gammo, a->gmag, a->reserve, a->vr_rounds, a->d_grip, a->d_supp, a->gmag_max, a->pk_pending, rls.presses, rls.loads, rls.syncs);
}

/* Detonación del C4 con el AGARRE del mando DERECHO (no con el círculo/B, que es el CQC del juego). El juego detona todas las cargas cuando ve el círculo (bakudan.c) y ese mismo botón es el CQC, así
 * que mientras haya cargas colocadas (BA_C4_COUNT: contador de actores de bakudan.c; dirección de la edición europea confirmada por desensamblado: NewBakudan 0x8006B69C lo compara con 16; su valor con
 * cargas puestas no está medido en volcado) el círculo del usuario se retira y el juego solo lo ve como pulsación nuestra al apretar a fondo el agarre derecho (flanco; antes debe haber estado suelto).
 * Siempre el derecho, sea cual sea la mano dominante. Solo con el C4 equipado (C4_WEAPON; con otra arma el círculo pasa tal cual y el juego detona como en el original) y nunca si vr_gesture está en golpe/agarre
 * en ese VBlank (vrbodyg_melee_busy), ni con la mano derecha en su zona de menú del pecho (ahí el grip abre el menú: vrbodyg_hand_in_chest), ni con el bloqueo de disparo de un menú abierto; la pulsación del agarre sin detonar también gasta el flanco. No actúa sin el control del jugador. */
#define C4_WEAPON 6
#define BA_C4_COUNT 0x800A0604u
#define C4_GRIP_ON 0.85f
#define C4_GRIP_OFF 0.30f
static int c4_placed(const uint8_t *ram) {
    int32_t n = 0;
    if (!ram) return 0;
    memcpy(&n, ram + (BA_C4_COUNT & 0x1FFFFFu), 4);
    return n > 0 && n <= 16;
}
static uint16_t c4_remote(uint16_t out, const BridgeState *bs, const uint8_t *ram) {
    const int core = bs && (bs->valid & BRIDGE_OK_ALL_CORE) == BRIDGE_OK_ALL_CORE && bs->has_player && bs->in_control;
    const int live = core && c4_placed(ram);
    const int c4_eq = live && bs->weapon == C4_WEAPON;
    float gr = 0.0f;
    pthread_mutex_lock(&HM);
    if (hand_in[1].valid) gr = hand_in[1].grip;
    pthread_mutex_unlock(&HM);
    if (!(gr == gr)) gr = 0.0f;
    if (gr < C4_GRIP_OFF) G.det_arm = 1;
    if (live && G.det_arm && gr > C4_GRIP_ON) {
        G.det_arm = 0;
        if (c4_eq && !G.fblk && !vrbodyg_melee_busy() && !vrbodyg_hand_in_chest(1)) { G.det_c4 = DET_PRESS_VBL; G.dets++; }
    }
    if (c4_eq || G.det_c4 > 0) out = (uint16_t)(out & ~PAD_DETONATE);       /* con el C4 equipado el círculo físico (CQC) ya no detona */
    if (G.det_c4 > 0) { G.det_c4--; if (core) out |= PAD_DETONATE; }
    return out;
}

/* ---- Nikita: nacimiento seguro y registro [nikita] (causa de la explosión al nacer: ver vr_fire.h, vrfire_missile_settle y vrfire_level_matrix) ---- */
typedef struct { const uint8_t *ram; const VrFireMap *map; } NikWall;
static float nik_wall_m(void *u, WVec3 p) {      /* distancia (m) de un punto de usuario a la pared de colisión más cercana (HZD del juego), -1 = ninguna a < 1,5 m */
    const NikWall *w = (const NikWall *)u;
    const float pu[3] = {p.x, p.y, p.z};
    float ew[3];
    vrfire_pos_to_world(w->map, pu, ew);
    const float d = vrhzd_wall_dist(w->ram, ew, 1500.0f);
    return d < 0.0f ? -1.0f : d / w->map->units_per_m;
}
/* La boca del tubo (la pose que pide el arma) corrida por el eje del tubo si queda cerca de la cabeza o de una pared (guided_spawn_pose); la dirección no cambia. */
static WPose nikita_spawn(const WPose *mz, const WInput *in, const VrFireMap *map, const uint8_t *ram, const BridgeState *bs) {
    NikWall nw = {ram, map};
    GuidedSpawnInfo gi;
    const WPose used = guided_spawn_pose(*mz, in->head.p, ram ? nik_wall_m : NULL, &nw, &gi);
    const float eu[3] = {used.p.x, used.p.y, used.p.z};
    float ew[3];
    vrfire_pos_to_world(map, eu, ew);
    const float dy = ew[1] - (float)bs->ctl_pos.y;
    fprintf(stderr, "[nikita] disparo mano=(%.3f %.3f %.3f) boca=(%.3f %.3f %.3f) usada=(%.3f %.3f %.3f) m (espacio de usuario) corrimiento=%.3f m (%s) | cabeza: boca %.2f, usada %.2f m (minimo %.2f) | pared mas cercana: boca %.2f, usada %.2f m (minimo %.2f; -1 = ninguna a < 1,5 m) | altura de la boca sobre los pies de Snake=%.0f u: el planeo del juego habria dado un primer paso vertical de %.0f u (explota con >= %d; vr_fire.c coloca el actor ya en la boca) | pos_motor=(%.0f %.0f %.0f)\n",
            in->hand[0].p.x, in->hand[0].p.y, in->hand[0].p.z, mz->p.x, mz->p.y, mz->p.z, used.p.x, used.p.y, used.p.z, gi.shift_m, gi.moved ? (gi.shift_m > 0 ? "adelantada" : "retrasada") : "sin mover",
            gi.head0_m, gi.head_m, GD_SPAWN_HEAD_M, gi.wall0_m, gi.wall_m, GD_SPAWN_WALL_M, dy, dy * 0.5f, VRFIRE_MIS_MAX_STEP_Y, ew[0], ew[1], ew[2]);
    return used;
}
static struct { uint32_t serial; int vb, reported; } NK;
/* Seguimiento del misil guiado: una línea cada 10 VBlanks (0,2 s) en vuelo y una al terminar con el motivo que se puede deducir de lo que da el juego (rmissile_act_helper_8006C114). pad_bits = cruceta enviada al juego. */
static void nikita_track(const WeaponState *st, const uint8_t *ram, const BridgeState *bs, uint16_t pad_bits) {
    const GuidedState *gs = (const GuidedState *)st;
    if (G.mis.serial != NK.serial) { NK.serial = G.mis.serial; NK.vb = 0; NK.reported = 0; }
    if (!G.mis.actor) return;
    NK.vb++;
    const float pw[3] = {(float)G.mis.pos[0], (float)G.mis.pos[1], (float)G.mis.pos[2]};
    const float wall = ram ? vrhzd_wall_dist(ram, pw, 1500.0f) : -1.0f;
    const float dx = pw[0] - (float)bs->ctl_pos.x, dyy = pw[1] - (float)bs->ctl_pos.y, dz = pw[2] - (float)bs->ctl_pos.z, snake = sqrtf(dx * dx + dyy * dyy + dz * dz);
    if (G.mis.alive && NK.vb % 10 == 0)
        fprintf(stderr, "[nikita] vuelo t=%.2f s edad=%d ticks (combustible del juego %d/1000 ticks; VR %.1f s) pos=(%d %d %d) dir=(%.2f %.2f %.2f) paso_y=%d guinada=%d fase=%d guiado enviado yaw=%.2f pitch=%.2f (bruto %.2f %.2f) cruceta=0x%02x | pared=%.0f u Snake=%.0f u\n",
                (float)NK.vb * 0.02f, G.mis.age, 1000 - G.mis.age, gs->fuel, G.mis.pos[0], G.mis.pos[1], G.mis.pos[2], G.mis.dir[0], G.mis.dir[1], G.mis.dir[2], G.mis.dy, G.mis.heading, st->phase,
                gs->steer[0], gs->steer[1], gs->steer_raw[0], gs->steer_raw[1], (unsigned)pad_bits, wall, snake);
    if (!G.mis.alive && G.mis.ended && !NK.reported) {
        char why[200] = "";
        if (G.mis.ended == 2) strcat(why, " edad_parada(actor destruido o reciclado)");
        if (G.mis.dy >= VRFIRE_MIS_MAX_STEP_Y) strcat(why, " paso_vertical>=200");
        if (G.mis.age >= 999) strcat(why, " combustible(1000 ticks)");
        if (abs(G.mis.pos[0]) > 30000 || abs(G.mis.pos[1]) > 30000 || abs(G.mis.pos[2]) > 30000) strcat(why, " limite_30000");
        if (wall >= 0.0f && wall < 250.0f) strcat(why, " pared<250u(NearHazard)");
        if (!why[0]) strcpy(why, " toque_o_poder_o_mensaje(no distinguible desde fuera)");
        NK.reported = 1;
        fprintf(stderr, "[nikita] fin t=%.2f s ended=%d motivo:%s | edad=%d ticks estado=%d terminado=%d pos=(%d %d %d) paso_y=%d pared=%.0f u Snake=%.0f u (blast de Snake si < ~2000 u) fase=%d\n", (float)NK.vb * 0.02f, G.mis.ended, why, G.mis.age, (int)G.mis.state,
                G.mis.done, G.mis.pos[0], G.mis.pos[1], G.mis.pos[2], G.mis.dy, wall, snake, st->phase);
    }
}

static uint16_t vrgame_pad_core(uint16_t pad, const BridgeState *bs, uint8_t *ram);
uint16_t vrgame_pad(uint16_t pad, const BridgeState *bs, uint8_t *ram) { return c4_remote(vrgame_pad_core(pad, bs, ram), bs, ram); }
static uint16_t vrgame_pad_core(uint16_t pad, const BridgeState *bs, uint8_t *ram) {
    init_once();
    place_ram = ram; place_bs = bs;
    vraa_tick();
    if (drv_fn) pad = drv_fn(drv_vbl++, pad, bs);
    pad = vrbodyg_pad(pad, bs, opt_right, ram);     /* gestos de cuerpo (golpe, agarre, pared): añaden/retiran botones del mando del usuario */
    uint64_t now = mono_ns();
    float dt = G.last_ns ? (float)((double)(now - G.last_ns) * 1e-9) : 0.02f;
    G.last_ns = now;
    if (!(dt > 0.001f)) dt = 0.001f;
    if (dt > 0.1f) dt = 0.1f;
    vrfire_grenade_track(ram, dt);       /* diagnóstico [granada]: sigue al actor de la granada lanzada 3 s */

    int cam = vrcam_active();
    int core = bs && (bs->valid & BRIDGE_OK_ALL_CORE) == BRIDGE_OK_ALL_CORE && bs->has_player;
    if (atomic_exchange_explicit(&cheat_weapons_req, 0, memory_order_acq_rel) && core && bs->in_control && ram) fprintf(stderr, "[truco] dar todas las armas: %d armas puestas a su máximo\n", vrgame_cheat_weapons_apply(ram));   /* una vez; sin partida o sin el control del jugador se descarta */

    calib_tick(dt);
    glove_update(ram);

    /* carga bajo demanda: el arma equipada que tiene modelo y aún no se ha pedido (slot y ge solo dependen de bs: se calculan antes de tomar las manos) */
    int right;
    pthread_mutex_lock(&HM);
    right = opt_right;
    pthread_mutex_unlock(&HM);
    load_poll();
    int eq_slot = core ? slot_of(bs->weapon) : -1;
    if (eq_slot >= 0 && G.fire_ok && cam) load_start(eq_slot);
    int eq_ready = eq_slot >= 0 && atomic_load_explicit(&SL[eq_slot].state, memory_order_acquire) == LD_READY;

    int w = right ? 1 : 0, o = 1 - w;   /* w: mando que empuña el arma; o: el otro */
    /* Arma mostrada: la equipada. */
    int slot = eq_ready ? eq_slot : -1;
    const int armed = slot >= 0 && G.fire_ok && cam && core;      /* mode sin la condición de la muestra de la mano (gp[w].valid) */
    Slot *sl = slot >= 0 ? &SL[slot] : NULL;
    const int gid = slot >= 0 ? WPN_TAB[slot].game_id : -1;
    const VrGripEntry *ge = vrgrip_find(gid);
    if (armed) {     /* el giro de la mano se fija ANTES de tomar las manos y de calcular el marco de la mano (hand_frame_from_grip lo lleva en vrbody_hand_offset): si no, al equipar o cambiar de arma un VBlank usa el giro anterior */
        const WQuat tq = ge ? ge->turn : (WQuat){0, 0, 0, 1};
        const float t4[4] = {tq.x, w ? tq.y : -tq.y, w ? tq.z : -tq.z, tq.w};
        vrbody_set_hand_turn(w, (tq.x != 0 || tq.y != 0 || tq.z != 0) ? t4 : NULL);
        vrbody_set_hand_turn(!w, NULL);
    }

    /* manos (una sola toma coherente: agarre y marco de la mano del arma salen de la misma muestra, sin hueco entre ambos) */
    HandSlot hs[2];
    GripPose gp[2], wp[2];       /* gp: agarre (mano del personaje, gestos); wp: marco de la mano nueva de la mano que empuña el arma (vr_grip la cuelga de él) */
    pthread_mutex_lock(&HM);
    memset(wp, 0, sizeof wp);
    for (int i = 0; i < 2; i++) { hs[i] = hand_in[i]; gp[i] = grip_locked(&hand_in[i], now); }
    if (armed && gp[w].valid) wp[w] = hand_frame_locked(w, now);      /* marco de la MANO del arma: función pura del agarre del mando (hand_frame_from_grip), sin IK */
    pthread_mutex_unlock(&HM);
    const int mode = armed && gp[w].valid && wp[w].valid;      /* wp[w] puede ser inválido con gp[w] válido (mano dominante cambiada con el arma soldada o en WELD_FADE_NS: el marco sale del agarre soldado de la otra mano); sin marco no hay arma (cuaternión nulo en hp.rot) */

    /* cuerpo: agarre de cada mano y mano dominante (la IK de los brazos lee las poses por vrpose_get_hands) */
    vrbody_set_dominant(right ? 1 : 0);
    for (int i = 0; i < 2; i++) vrbody_set_grip(i, gp[i].valid ? hs[i].grip : 0.0f);
    for (int i = 0; i < 2; i++) { atomic_store_explicit(&grip_diag_raw[i], hs[i].grip, memory_order_relaxed); atomic_store_explicit(&grip_diag_ok[i], gp[i].valid ? 1 : 0, memory_order_relaxed); }

    if (mode && slot != G.cur && G.cur >= 0 && G.active) {    /* cambio de arma: la anterior se reinicia al volver a mostrarla */
        Slot *old = &SL[G.cur];
        if (atomic_load(&old->state) == LD_READY) { old->def->init(old->st); old->st->def = old->def; }
    }
    G.mag_weapon = core && bs->magazine_max > 0 && (gid == 0 || gid == 1 || gid == 9);
    G.ammo_game = core && bs->weapon >= 0 && bs->weapon < BRIDGE_N_WEAPONS ? bs->weapon_ammo[bs->weapon] : 0;
    G.game_mag = core ? bs->magazine : 0;

    /* la recarga corre siempre (vr_reload.h): refleja el cargador del juego en el arma VR y pulsa el ataque con el cargador vacío */
    VrReloadGame rg;
    vrreload_read_game(bs, &rg);
    VrReloadIn ri;
    memset(&ri, 0, sizeof ri);
    ri.weapon_id = mode && G.mag_weapon ? gid : -1;
    float gtr = hs[w].trigger;      /* gatillo tras el bloqueo del menú abierto (vrbodyg_fire_gate: una vez por VBlank, aquí) */
    const int fblk = mode ? vrbodyg_fire_gate(w, 1, &gtr) != 0 : 0;
    {
        const int tr = hs[w].trigger > 0.5f;
        ri.trigger_edge = tr && !G.trig_prev && !fblk;      /* con un menú abierto no hay flanco: con el cargador vacío no pulsa el cuadrado de recarga */
        G.trig_prev = tr;
    }
    ri.vr_held = ri.weapon_id >= 0 ? weapon_held(sl->st) : 0;
    ri.vr_shot = G.shots != G.shots_prev;
    G.shots_prev = G.shots;
    VrReloadOut ro;
    vrreload_frame(&rg, &ri, &ro);
    if (fblk && ro.pad_force) vrreload_cancel_press(&ro);      /* con un menú abierto el gatillo no pulsa el ataque (ni mantiene una pulsación en curso) */
    if (ro.load_mag > 0 && ram) { int16_t lm = (int16_t)ro.load_mag; memcpy(ram + (BA_MAGAZINE & 0x1FFFFFu), &lm, 2); }   /* recogida con el arma seca: el cargador del juego se carga aquí (VrReloadOut.load_mag); el VBlank siguiente el puente lo lee y el arma VR queda lista */
    {   /* diagnóstico: recogida de munición con el arma seca (el total del juego subió con el cargador del juego a 0) */
        static int pk_prev;
        int pk = vrreload_pickup_pending();
        if (pk && !pk_prev) fprintf(stderr, "[recarga] municion recogida con el arma seca: arma=%d total=%d cargador_juego=%d cargador_max=%d balas_vr=%d -> se carga el cargador del juego y el arma VR lo refleja\n", bs ? bs->weapon : -1, G.ammo_game, G.game_mag, bs ? bs->magazine_max : -1, mode && sl ? weapon_held(sl->st) : -1);
        pk_prev = pk;
    }
    /* Armas sin cargador en el juego (misil, granadas, minas): puede disparar mientras quede munición. */
    if (mode && !G.mag_weapon) { ro.fire_allowed = G.ammo_game > 0; ro.busy = 0; }

    if (!mode) {
        vrcam_set_zoom_cap(0);           /* sin mira VR la cabeza recibe el aumento del juego tal cual */
        memset(&G.mis, 0, sizeof G.mis);
        { uint32_t ser; vrfire_last_missile(&ser); G.mis_serial = ser; }
        vraa_set_scope(0, NULL, NULL);
        vraa_gesture(NULL, NULL, 0);     /* sin arma o sin mano rastreada: el gesto del visor se reinicia (sin estado viejo al volver) */
        const int hold = G.weld_hold > 0 && core && cam;
        if (G.active) {
            G.sv_slot = -1;                   /* desequipada: la vista especial de antes no da derecho a conservarla con el selector abierto */
            G.sv_prev = hold ? G.cur : -1;    /* recarga: si vuelve esta misma arma no espera otra vez */
            publish_off(hold);
            /* al volver a mostrarla se reinicia el estado del arma (cargador del juego otra vez) */
            if (G.cur >= 0 && atomic_load(&SL[G.cur].state) == LD_READY) { SL[G.cur].def->init(SL[G.cur].st); SL[G.cur].st->def = SL[G.cur].def; }
        }
        if (!hold) G.sv_prev = -1;
        G.active = 0;
        G.cur = -1;
        G.det_t = 0;             /* una pulsación de detonación a medias no sobrevive a desequipar el arma */
        G.fblk = 0; G.rl_latch = 0;
        G.hand_ns = 0; G.throw_hold = 0;
        atomic_store_explicit(&native_hand, -1, memory_order_relaxed);
        vrbody_set_armed(0);
        vrbody_set_hand_turn(0, NULL); vrbody_set_hand_turn(1, NULL);
        if (hold) G.weld_hold--;      /* desequipado breve (recarga): las manos siguen en el arma unos VBlanks por si vuelve; si no vuelve se sueltan y empieza la rampa WELD_FADE_NS */
        else {
            G.weld_hold = 0;
            vrbody_set_engine_hidden_ik(0);
            vrbody_set_weld(0, 0); vrbody_set_weld(1, 0); vrbody_set_pole(0, NULL); vrbody_set_pole(1, NULL);
            pthread_mutex_lock(&HM); if (pub_weld.on) pub_weld.off_ns = mono_ns(); pub_weld.on = 0; pthread_mutex_unlock(&HM);
        }
        aimfix_clear();
        vrfire_set_hide(0);
        VrFireGame fg;
        vrfire_read_game(ram, bs, &fg);
        VrFireIn fi;
        memset(&fi, 0, sizeof fi);
        VrFireOut fo;
        vrfire_frame(&fg, &fi, &fo);
        G.ro = ro;
        if (core && bs->weapon >= 0) {
            ArmaDiag ad = {eq_slot, eq_slot >= 0 ? atomic_load(&SL[eq_slot].state) : -1, 0, cam, core, gp[w].valid, ro.busy, ro.fire_allowed, fo.engaged, game_has_suppressor(bs),
                           hs[w].trigger, -1.0f, -1, -1, -1, bs->weapon, bs->item, G.ammo_game, -1.0f, -1.0f, bs->magazine, vrreload_ammo_reserve(bs->weapon), -1, bs->magazine_max, vrreload_pickup_pending()};
            arma_diag(now, &ad);
        }
        publish_hand_pose(hs, gp, right, NULL, 0);
        uint16_t out = pad;
        out |= ro.pad_force;
        pthread_mutex_lock(&HM);
        pub_status.loader = eq_slot >= 0 ? atomic_load(&SL[eq_slot].state) : 0;
        pub_status.active = 0; pub_status.weapon = -1; pub_status.mag_rounds = pub_status.chambered = pub_status.mag_inserted = 0;
        pub_status.game_mag = pub_status.game_reserve = 0;
        pub_status.shots = G.shots;
        pthread_mutex_unlock(&HM);
        return out;
    }

    /* ---- arma VR mostrada y mandando ---- */
    WeaponState *st = sl->st;
    const WeaponDef *def = sl->def;
    G.active = 1;
    /* Vista especial (lente del PSG1, pantalla del Stinger / Nikita): solo la del arma realmente lista. Un arma que entra (cambio directo, o tras desequipar) espera SV_SETTLE_TICKS VBlanks, y con el selector
     * abierto solo conserva la suya el arma que ya estaba lista al abrirlo: el cursor pasando por el PSG1 (el juego lo equipa al pasar) no enseña su lente. */
    if ((slot != G.cur && slot != G.sv_prev) || (bs->menu_open && slot != G.sv_slot)) G.sv_wait = SV_SETTLE_TICKS;
    else if (G.sv_wait > 0) G.sv_wait--;
    const int sv_ready = G.sv_wait == 0 && (!bs->menu_open || slot == G.sv_slot);
    if (sv_ready && !bs->menu_open) G.sv_slot = slot;
    G.sv_prev = -1;
    G.cur = slot;
    /* Conducto (estado INTRUDE) o tumbado (stance 2 / prone): el arma no se ve ni el modelo VR, ni el del juego, ni la copia del juego en la mano (se oculta siempre y no se lleva a la mano). Solo visibilidad: el disparo no se toca (si el juego lo bloquea ahí, es del propio juego).
     * El arma VR solo se ve tumbado con el PSG1 (gid 9). */
    const int duct = is_duct(bs, gid);
    atomic_store_explicit(&native_hand, sl->native && !duct ? w : -1, memory_order_relaxed);
    vrbody_set_armed(1);
    vrfire_set_hide(sl->native && !duct ? 0 : 1);      /* las granadas (sl->native, vrgame_weapon_uses_game_model) se dibujan siempre con el modelo del juego y no se oculta; el resto lleva modelo VR y se oculta el del juego (la granada lanzada tampoco se oculta nunca: es otro actor) */

    /* El arma VR refleja el cargador del juego (vr_reload.h, ESPEJO): recarga del juego, recogida con el arma seca, arma recién mostrada. */
    if (G.mag_weapon && ro.set_held >= 0 && (weapon_held(st) != ro.set_held || !st->mag_inserted)) {
        const int before = weapon_held(st);
        weapon_set_held(st, def, ro.set_held);
        fprintf(stderr, "[recarga] arma %d: arma VR igualada al cargador del juego (%d -> %d balas; cargador del juego %d, total %d)\n", gid, before, ro.set_held, G.game_mag, G.ammo_game);
    }

    /* correspondencia usuario -> motor y suelo del juego (para colocar minas y cargas) */
    VrFireMap map;
    vrfire_map_from_cam(&map, vrcam_last_record());
    G.map = map;
    head_pose(&G.head);
    G.floor_ok = 0;
    if (map.valid && core) {
        /* los pies del personaje: ctl_pos.y es la base del bloque de control; ojo del motor <-> cabeza del usuario */
        float feet_below_eye_m = (float)(map.eye[1] - bs->ctl_pos.y) / map.units_per_m;
        if (feet_below_eye_m > 0.3f && feet_below_eye_m < 2.5f) { G.floor_u = G.head.p.y - feet_below_eye_m; G.floor_ok = 1; }
    }

    WInput in;
    memset(&in, 0, sizeof in);
    in.head = G.head;
    VrHandsPose hp;                    /* mano del arma (marco de la mano nueva); con VRH_GRIP_SOCKET el arma y su lógica cuelgan de ella por el zócalo */
    memset(&hp, 0, sizeof hp);
    hp.valid = 1;
    memcpy(hp.pos, wp[w].p, sizeof hp.pos); memcpy(hp.rot, wp[w].q, sizeof hp.rot);
    {
        WPose sock = ge ? ge->grip : wpose_identity();
        if (!w) sock = vrgrip_mirror(sock);                                   /* mando izquierdo: zócalo reflejado (la mano se dibuja a tamaño real: el zócalo va en m reales) */
        vrhands_set_socket(sl->rig, sock);        /* giro local de la mano que empuña (vr_grip turn, FIT_*_TURN): la malla gira respecto al mando y el zócalo ya lo compensa, así que el arma no gira; reflejado en el mando izquierdo */
    }
    in.hand[0] = vrhands_weapon_hand(sl->rig, &hp);
    if (gp[o].valid) in.hand[1] = wpose_of(&gp[o]);
    else { in.hand[1].p.x = 0; in.hand[1].p.y = -10; in.hand[1].p.z = 0; in.hand[1].q.w = 1; }   /* sin mano libre: lejos de todo punto de interacción */
    /* Mano de apoyo. Armas de fuego (la tabla de vr_grip lleva pose de apoyo: pistola, fusil, francotirador, lanzador, guiado): ambas manos van SOLDADAS al arma mientras está equipada (support_held = 1 siempre);
     * la mano libre se dibuja donde la deja el agarre efectivo (grip_eff_locked) y el mando de ese lado solo es fantasma de gestos (in.hand[1], vrbodyg). Resto: agarre analógico cerca de WP_SUPPORT como siempre. */
    const int welded = ge && ge->support_pose >= 0 && gp[w].valid && wp[w].valid && !duct;      /* conducto / tumbado: el arma está oculta, la mano de apoyo vuelve al mando (rampa WELD_FADE_NS) */
    int sup_held;
    {
        const float sg = gp[o].valid ? hs[o].grip : 0.0f, sdist = gp[o].valid ? wcore_hand_dist(st, &in, 1, WP_SUPPORT) : 1e9f;
        sup_held = welded ? 1 : (sg > 0.5f && sdist < 0.07f);
    }
    /* Arma = marco del apuntado del mando: el marco del cañón (sin retroceso: -Z = boca, +Y = arriba del arma) se lleva al marco de apuntado de aim_rot (-Z hacia donde apunta, +Y arriba), boca Y alabeo, aplicado como giro
     * extra agarre -> mano (vrbody_set_aim_fix) a la mano que empuña (y que dibuja vr_body) y al arma que cuelga de ella: la empuñadura sigue en el agarre del mando. Con el mando recto el arma sale recta al frente y sin
     * alabeo, MANDE lo que mande el giro de mano (hand_roll_deg), la calibración y el zócalo de vr_grip: esos solo fijan cómo se dibuja la mano. (Antes solo se corregía la boca con el giro más corto: el zócalo FIT_*_GRIP se
     * diseñó con la mano en apretón de manos, alabeo 90, y con el 45 de los mandos el arma salía alabeada 45 grados -SOCOM- que ese giro no quita.) La relación agarre/apuntado del mando es fija, así que converge en un
     * fotograma y se queda; sin apuntado válido se mantiene la última corrección 0,25 s y se retira suavemente; solo en pistola y fusil (aimfix_weapon). */
    {
        int fixed_ok = 0;
        if (!aimfix_weapon(gid)) { aimfix_clear_side(w); fixed_ok = 2; }      /* arma sin boca real: nunca se gira */
        else if (hs[w].aim_valid && gp[w].valid) {
            WPose gr = vrhands_grip(sl->rig, st);
            const WQuat wq = wquat_mul(in.hand[0].q, wquat_inv(wquat_normalize(gr.q)));      /* giro del arma */
            WQuat bq = wquat_mul(wq, def->points[WP_MUZZLE].q);
            WVec3 bwv = wquat_rotate(wquat_normalize(bq), wv(0, 0, -1));
            float bw[3] = {bwv.x, bwv.y, bwv.z}, aw[3], z[3] = {0, 0, -1}, ax[3], qc[4];
            q_rot(hs[w].aim_rot, z, aw);
            ax[0] = bw[1] * aw[2] - bw[2] * aw[1]; ax[1] = bw[2] * aw[0] - bw[0] * aw[2]; ax[2] = bw[0] * aw[1] - bw[1] * aw[0];
            float sn = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]), cs = bw[0] * aw[0] + bw[1] * aw[1] + bw[2] * aw[2];
            float ang = atan2f(sn, cs);
            if (finite_n(bw, 3) && finite_n(aw, 3) && ang < 1.5f) {
                fixed_ok = 1;
                {      /* giro del mundo Dw = apuntado * conj(marco del cañón) (boca y alabeo); en el marco del agarre: Dg = conj(q_agarre) Dw q_agarre; nuevo = Dg * actual */
                    const WQuat bn = wquat_normalize(bq);
                    const float bqa[4] = {bn.x, bn.y, bn.z, bn.w};
                    float bqc[4], dw[4], t1[4], dg[4], nf[4];
                    q_conj(bqa, bqc); q_mul(hs[w].aim_rot, bqc, dw); q_norm(dw);
                    if (dw[3] < 0.0f) for (int k = 0; k < 4; k++) dw[k] = -dw[k];
                    if (dw[3] < 0.9999999f) {      /* > ~0,05 grados */
                        q_conj(gp[w].q, qc); q_mul(qc, dw, t1); q_mul(t1, gp[w].q, dg); q_norm(dg);
                        q_mul(dg, aimfix_q[w], nf); q_norm(nf);
                        memcpy(aimfix_q[w], nf, sizeof nf);
                        vrbody_set_aim_fix(w, aimfix_q[w]);
                    }
                }
            }
        }
        if (fixed_ok == 1) aimfix_miss[w] = 0;
        else if (fixed_ok == 0) aimfix_lost(w, dt);
    }
    in.trigger[0] = hs[w].trigger;
    in.grip[0] = hs[w].grip;
    if (gp[o].valid) { in.trigger[1] = hs[o].trigger; in.grip[1] = hs[o].grip; }
    /* menú abierto (vr_gesture VRG_FB_*): con la ventana de la mano abierta, con cualquier menú abierto o con el gatillo aún apretado tras cerrarse, el gatillo no es del arma (ni dispara, ni prepara, ni lanza, ni detona) y el disparo en cola se anula */
    if (fblk) in.trigger[0] = is_grenade(gid) ? gtr : 0.0f;      /* arrojadiza: nivel congelado (no se lanza ni se prepara); armas de fuego: suelto (el auto-disparo de w_gunc no sigue con el gatillo congelado) */
    G.fblk = fblk;
    if (gp[o].valid) vrbodyg_fire_gate(o, 0, &in.trigger[1]);
    in.buttons[0] = (hs[w].low ? WB_A : 0u) | (hs[w].up ? WB_B : 0u) | (hs[w].stick ? WB_STICK : 0u);
    if (gp[o].valid) in.buttons[1] = (hs[o].low ? WB_A : 0u) | (hs[o].up ? WB_B : 0u) | (hs[o].stick ? WB_STICK : 0u);
    in.dt = dt;
    {   /* tiempo entre poses: la mano llega a ritmo del visor (~90 Hz, t_ns = instante de llegada) y el VBlank (50 Hz, con fluctuación) toma la última; la velocidad de lanzamiento se mide en tiempo de pose, no de VBlank */
        uint64_t tn = hs[w].t_ns;
        in.hand_dt = G.hand_ns == 0 || tn < G.hand_ns ? 0.0f : tn == G.hand_ns ? 1e-4f : (float)((double)(tn - G.hand_ns) * 1e-9);   /* misma pose que el VBlank anterior: muestra repetida (0,1 ms) */
        G.hand_ns = tn;
    }
    /* el gatillo solo actúa cuando el disparo puede salir (recarga del juego en curso, cargador del juego vacío...: sin disparos sin proyectil) */
    if (G.mag_weapon) {      /* cerrojo: mientras el juego recarga el arma VR no ve el gatillo, y tras acabar sigue sin verlo hasta soltarlo (< 0,3): mantenerlo no dispara solo al acabar */
        if (ro.busy) G.rl_latch = 1;
        else if (G.rl_latch && hs[w].trigger < 0.3f) G.rl_latch = 0;
        if (G.rl_latch) in.trigger[0] = 0;
    }
    in.support_held = sup_held;
    in.game_flags = game_has_suppressor(bs) ? WGF_SUPP : 0u;

    G.fire_edge = 0;
    if (gid == 4) {      /* lanzador antiaéreo: el arma refleja el buscador del propio juego (para háptico y estado visible) */
        VrAaLock al;
        vraa_lock_info(&al);
        AAGameLock gl = {al.alive, al.target != 0, al.frames};
        aa_feed_game_lock(st, &gl);
    } else {
        vraa_set_scope(0, NULL, NULL);
    }
    /* misil guiado del juego: se vigila el actor que crea el juego (vr_missile.h); las demás armas lo ignoran */
    {
        uint32_t ser;
        uint32_t act = vrfire_last_missile(&ser);
        if (gid == 3 && ser != G.mis_serial) vrmis_begin(act);
        G.mis_serial = ser;
        if (gid == 3) vrmis_poll(ram, &G.mis); else memset(&G.mis, 0, sizeof G.mis);
    }
    if (gid == 9) {      /* francotirador: el tranquilizante del juego (temporizador distinto de cero) estabiliza la mira; suavizado para no dar saltos */
        int16_t tq; memcpy(&tq, ram + (0x800B5FBAu & 0x1FFFFFu), 2);
        G.steady += ((tq > 0 ? 1.0f : 0.0f) - G.steady) * 0.02f;
        sniper_set_steady(st, G.steady);
        {   /* autoenfoque del original (rifle.c GetZoomLimit): primer muro (HZD, misma máscara que su línea de visión) o geometría sólida en el eje de la mira, hasta 3000 unidades */
            float hit = -1.0f;
            WPose lp;
            if (map.valid && sniper_lens_world(st, &lp)) {
                const WVec3 fw = wquat_rotate(lp.q, wv(0, 0, -1));
                const float pu[3] = {lp.p.x, lp.p.y, lp.p.z}, du[3] = {fw.x, fw.y, fw.z};
                float ow[3], dw[3];
                vrfire_pos_to_world(&map, pu, ow); vrfire_dir_to_world(&map, du, dw);
                hit = vrhzd_raycast(ram, ow, dw, SNIPER_ZOOM_MAX_H, VRHZD_SKIP_SIGHT);
                const float t2 = mv_vrhost_ray_distance(ow, dw, hit >= 0.0f ? hit : SNIPER_ZOOM_MAX_H);      /* el original (HZD_CHK_ALL) también ve suelos y objetos: geometría sólida nativa, como el láser */
                if (t2 >= 0.0f) hit = t2;
            }
            sniper_set_range(st, hit, map.units_per_m);
        }
    }
    G.probe_ok = 0;
    def->update(st, &in, WR_AUTOMATIC, &G.br);
    G.throw_hold = is_grenade(gid) && (st->phase == WT_PH_HELD || st->phase == WT_PH_READY);

    /* publicación de las mallas */
    VrHandsFrame vf;
    int nb = sl->native || duct ? 0 : vrhands_build(sl->rig, st, &hp, &vf);   /* fuera del cerrojo: el hilo de render no espera a la construcción de las mallas */
    pthread_mutex_lock(&HM);
    if (nb > 0) {
        vrhands_post(&vf);
        pub_sim_hand = wp[w];
        pub_sim_valid = 1;
    } else {
        vrhands_post_clear();
        pub_sim_valid = 0;
    }
    pthread_mutex_unlock(&HM);

    /* disparo */
    vrfire_skip_raise(ram);      /* sin el levantado de 8 ticks del arma del juego (vr_fire.h) */
    VrFireGame fg;
    vrfire_read_game(ram, bs, &fg);
    VrFireIn fi;
    memset(&fi, 0, sizeof fi);
    fi.active = bs->in_control && !ro.busy && map.valid;
    WPose aim_fix = wpose_identity();      /* Wa * W^-1: quita el retroceso visual (recoil_offset) de una pose del arma en el mundo (ver más abajo) */
    {
        WPose W = vrhands_weapon_world(sl->rig, st, &hp);
        /* El rayo de puntería (láser, boca en vivo de la ráfaga y pose del disparo) se calcula con la pose del arma SIN recoil_offset: el retroceso solo mueve el modelo del arma. La bala sale
         * ~60-100 ms después del gatillo, cuando el retroceso ya ha movido el arma; con él, el láser se balancea y la bala (que sale de la pose anterior al retroceso) no cae donde estaba el punto. */
        WPose Wa = W;
        {
            const WPose rs = st->recoil_offset;
            st->recoil_offset = wpose_identity();
            Wa = vrhands_weapon_world(sl->rig, st, &hp);
            st->recoil_offset = rs;
        }
        aim_fix = wpose_mul(Wa, wpose_inv(W));
        fi.muzzle = wpose_mul(Wa, def->points[WP_MUZZLE]);
        {   /* mano de apoyo soldada: su marco de mano es el del zócalo de apoyo de vr_grip en el punto WP_SUPPORT del arma; se guarda su agarre en el marco del agarre de la mano que empuña (así sigue a esa mano en vivo) */
            float rp[3] = {0, 0, 0}, rq[4] = {0, 0, 0, 1};
            int on = welded;
            if (on) {
                const WPose hf = wpose_mul(W, vrgrip_weapon_from_hand(def->points[WP_SUPPORT], ge->support, o == 1 ? 1 : -1));
                GripPose fr;
                memset(&fr, 0, sizeof fr);
                fr.p[0] = hf.p.x; fr.p[1] = hf.p.y; fr.p[2] = hf.p.z; fr.q[0] = hf.q.x; fr.q[1] = hf.q.y; fr.q[2] = hf.q.z; fr.q[3] = hf.q.w; q_norm(fr.q);
                fr.valid = finite_n(fr.p, 3) && finite_n(fr.q, 4);
                const GripPose go = grip_from_hand_frame(o, &fr);
                float qc[4], dv[3] = {go.p[0] - gp[w].p[0], go.p[1] - gp[w].p[1], go.p[2] - gp[w].p[2]};
                q_conj(gp[w].q, qc);
                q_rot(qc, dv, rp); q_mul(qc, go.q, rq); q_norm(rq);
                on = go.valid && finite_n(rp, 3) && finite_n(rq, 4);
            }
            pthread_mutex_lock(&HM);
            if (on) { pub_weld.on = 1; pub_weld.off_ns = 0; pub_weld.side = o; memcpy(pub_weld.p, rp, sizeof rp); memcpy(pub_weld.q, rq, sizeof rq); }
            else { if (pub_weld.on) pub_weld.off_ns = mono_ns(); pub_weld.on = 0; }      /* sin soldar: se conserva la última relación para la rampa de salida */
            pthread_mutex_unlock(&HM);
            vrbody_set_weld(o, on); vrbody_set_weld(w, !duct);      /* los DOS brazos atados al arma (la que empuña y la de apoyo soldada; conducto / tumbado: el arma está oculta): la muñeca llega siempre al arma, sin límites de hombro ni tope del codo */
            G.weld_hold = (on || !duct) ? WELD_HOLD_VBL : 0;
            vrbody_set_engine_hidden_ik(on && (bs->in_control || bs->cam_ctl) && !bs->cinematic);      /* arma de dos manos con el control del jugador o con la pausa / un menú encima (cam_ctl), sin cinemática ni escena: la IK de brazos y manos se aplica aunque el motor oculte a Snake de pie (el Stinger lo hace en su primera persona; sin IK el arma flotaba sin manos) */
            if (ge) {   /* codos como los pone Snake con esa arma (vr/hands/vr_grip.c: medidos en sus animaciones); sin soldar, los del cuerpo */
                const float em[3] = {ge->elbow_main.x, ge->elbow_main.y, ge->elbow_main.z}, es[3] = {ge->elbow_support.x, ge->elbow_support.y, ge->elbow_support.z};
                vrbody_set_pole(w, on ? em : NULL); vrbody_set_pole(o, on ? es : NULL);
            }
        }
        if (gid == 4) {
            /* Visor del lanzador: con el arma al ojo el juego tiene su mira y su buscador (vr_fire mantiene el botón) y el buscador
             * del juego corre con la vista de este visor (vr_aa). Sin gesto (a la cadera) se dispara como antes, sin fijar. */
            WPose sight = wpose_mul(W, def->points[WP_EYE]);        /* origen y eje de la vista del buscador: el tubo, no la cabeza */
            WPose led = wpose_mul(W, aa_gesture_point(st));         /* gesto: la cabeza junto a la cara de la pantalla LED (WP_EYE queda ~0,6 m por delante), con el eje del tubo */
            int aim = vraa_gesture(&G.head, &led, st->phase == AA_P_READY && st->mag_rounds > 0);
            vraa_set_h_scale(vrcam_config()->engine_h > 0 ? 320.0f / (float)vrcam_config()->engine_h : 1.0f);   /* cámara VR activa (mode la exige): el canal lleva la H del motor */
            vraa_set_scope(aim, &map, &sight);
            fi.ready = aim;
            fi.raise = 1;
        } else if (gid == 2 || gid == 7 || gid == 8) {
            /* Granada: gatillo apretado = el botón de ataque del juego mantenido (es el «preparar»/sacar el pasador del propio juego); soltarlo lanza (fire con la
             * velocidad de la mano, ver launch_hook) y el juego la crea al soltar el botón. */
            fi.raise = 1;
            fi.ready = st->phase == WT_PH_READY;
        }
        special_view(gid, st, def, &W, &in, duct, sv_ready);
        /* láser de la pistola (w_laser.h): desde la boca (la del disparo: sin supresor, la del cañón) a lo largo de su eje, el mismo con el que vr_fire orienta la bala; solo con el jugador al mando y el arma levantada */
        WPose lz = def->points[WP_MUZZLE];
        if (gid == 0 && !(in.game_flags & WGF_SUPP)) lz.p.z = w_pist_bare_muzzle_z;
        lz = wpose_mul(Wa, lz);
        int lz_on = gid == 0 && !duct && bs->in_control && wlaser_raised(&lz);
        float lz_hit_m = -1.0f;
        const int gh_on = gid == 5 && !duct && bs->in_control && G.probe_ok && st->phase == WPL_PH_HELD;   /* mina en la mano con sitio donde colocarla: fantasma (contorno + flecha de su dirección de disparo) en el punto y con la orientación de colocación */
        if (gh_on) { lz = G.probe_pose; lz.p.y -= WGHOST_MINE_LEGS_U / map.units_per_m; }      /* el fantasma se tiende sobre el suelo (w_laser.h): placed.p.y es el origen del modelo, WGHOST_MINE_LEGS_U uds más arriba */
        if (lz_on && map.valid) {
            /* El rayo acaba donde acaba la bala: misma prueba y mismo alcance (vrhit_shot_dist: primer blanco con los volúmenes de la bala, paredes HZD con la máscara de su línea de visión y suelos de
             * colisión). La geometría de render nativa (vr_headcol) no corta: no bloquea balas (cristales con huecos, rejillas), y el láser debe parar en lo que para la bala. */
            WVec3 fw = wquat_rotate(wquat_normalize(lz.q), wv(0, 0, -1));
            float pu[3] = {lz.p.x, lz.p.y, lz.p.z}, du[3] = {fw.x, fw.y, fw.z}, ow[3], dw[3];
            vrfire_pos_to_world(&map, pu, ow); vrfire_dir_to_world(&map, du, dw);
            float t = vrhit_shot_dist(ram, ow, dw, vrhit_weapon_range_m(gid) * map.units_per_m);
            if (t >= 0.0f) lz_hit_m = t / map.units_per_m;
        }
        pthread_mutex_lock(&HM);
        pub_laser.on = lz_on || gh_on; pub_laser.ghost = gh_on;
        pub_laser.hit_m = lz_hit_m;
        if (lz_on || gh_on) { pub_laser.p[0] = lz.p.x; pub_laser.p[1] = lz.p.y; pub_laser.p[2] = lz.p.z; pub_laser.q[0] = lz.q.x; pub_laser.q[1] = lz.q.y; pub_laser.q[2] = lz.q.z; pub_laser.q[3] = lz.q.w; }
        pthread_mutex_unlock(&HM);
    }
    fi.fire = G.fire_edge && ro.fire_allowed && !fblk;
    fi.fire_pose = G.fire_pose;
    /* pose del disparo sin retroceso (el residuo del disparo anterior incluido): conserva las correcciones del arma (eje óptico, apuntado) y solo quita recoil_offset */
    if (gid == 0 || gid == 1 || gid == 9) fi.fire_pose = wpose_mul(aim_fix, G.fire_pose);
    if (gid == 3 && fi.fire && map.valid) fi.fire_pose = nikita_spawn(&fi.fire_pose, &in, &map, ram, bs);      /* misil guiado: nace en la boca, a distancia segura de la cabeza y de las paredes */
    vrhit_frame();
    if (map.valid) vrhit_set_range(vrhit_weapon_range_m(gid) * map.units_per_m, map.units_per_m);      /* alcance del tiro = el del arma en la mano (SOCOM 25 m, FAMAS 30 m, resto 15 m) y el del láser (vr_hit.h) */
    if (fi.fire && map.valid) {      /* instante del gatillo: se anota a qué enemigo y zona apunta la boca ahora (el juego crea la bala ~0,4 s después y el enemigo se habrá movido) */
        VrFireMat am;
        if (vrfire_make_matrix(&map, &fi.fire_pose, 0.0f, &am)) { float ao[3] = {(float)am.t[0], (float)am.t[1], (float)am.t[2]}, ad[3]; vrfire_mat_forward(&am, ad); vrhit_aim(ram, ao, ad); if (aimfix_weapon(gid)) vrhit_note_laser(ram, ao, ad); }
    }
    fi.fire_life = gid == 5 ? 180 : 0;      /* mina: el juego la crea ~70 VBlanks después (se agacha a colocarla); la petición no debe caducar antes */
    /* Fusil en automático: el juego dispara mientras el botón está mantenido; la pose de cada bala es la boca en vivo. */
    fi.trigger_held = gid == 1 && !rifle_is_semi(st) && in.trigger[0] > 0.5f && ro.fire_allowed && st->chambered > 0 && !fblk;
    if (fi.trigger_held && fi.active && map.valid) {      /* ráfaga: cada bala sale por la boca en vivo; se anota su rayo para el registro [disparo] laser=... bala_impacto=... */
        VrFireMat am;
        if (vrfire_make_matrix(&map, &fi.muzzle, 0.0f, &am)) { float ao[3] = {(float)am.t[0], (float)am.t[1], (float)am.t[2]}, ad[3]; vrfire_mat_forward(&am, ad); vrhit_note_laser(ram, ao, ad); }
    }
    fi.map = &map;
    VrFireOut fo;
    vrfire_frame(&fg, &fi, &fo);

    /* mando que ve el juego: gatillo y empuñaduras ya no son del juego (lo que hacen lo hace el arma VR) */
    /* Solo con el jugador al mando: en menús, pausa, comunicador y escenas el mando pasa tal cual (el gatillo VR no se traduce ahí).
     * El agarre de la mano libre (L1) pasa tal cual: el mando izquierdo es un fantasma de gestos que abre las ventanas del juego. */
    uint16_t out = pad;
    if (bs->in_control) out = (uint16_t)(out & ~(PAD_SQUARE | PAD_R1));
    out |= ro.pad_force;
    uint16_t nk_pad = 0;
    if (gid == 3 && bs->in_control && G.mis.alive) {
        out = (uint16_t)(out & ~0xF0u);        /* durante el guiado la cruceta es solo del guiado (una sola fuente: sin mezclar bits del mando físico) */
        nk_pad = vrmis_pad(vrcam_pad_origin());
        out |= nk_pad;
    }   /* guiado: la cruceta del juego (vr_missile.h) */
    if (gid == 3) nikita_track(st, ram, bs, nk_pad);
    if (fo.engaged && fo.square) out |= PAD_SQUARE;
    publish_hand_pose(hs, gp, right, ge, in.support_held);
    if (bs->weapon >= 0) {
        ArmaDiag ad = {eq_slot, eq_slot >= 0 ? atomic_load(&SL[eq_slot].state) : -1, 1, cam, core, gp[w].valid, ro.busy, ro.fire_allowed, fo.engaged, (in.game_flags & WGF_SUPP) != 0,
                       hs[w].trigger, in.trigger[0], gid == 0 ? w_pist_trig_armed(st) : -1, st->chambered, st->mag_inserted, bs->weapon, bs->item, G.ammo_game,
                       sqrtf((gp[w].p[0] - in.hand[0].p.x) * (gp[w].p[0] - in.hand[0].p.x) + (gp[w].p[1] - in.hand[0].p.y) * (gp[w].p[1] - in.hand[0].p.y) + (gp[w].p[2] - in.hand[0].p.z) * (gp[w].p[2] - in.hand[0].p.z)), gp[o].valid ? wcore_hand_dist(st, &in, 1, WP_SUPPORT) : -1.0f,
                       bs->magazine, vrreload_ammo_reserve(bs->weapon), weapon_held(st), bs->magazine_max, vrreload_pickup_pending()};
        arma_diag(now, &ad);
    }
    if (G.det_t > 0) { G.det_t--; if (bs->in_control && !fblk) out |= PAD_DETONATE; }
    G.ro = ro;

    pthread_mutex_lock(&HM);
    pub_status.loader = LD_READY;
    pub_status.active = 1;
    pub_status.weapon = gid;
    pub_status.mag_rounds = st->mag_rounds; pub_status.chambered = st->chambered; pub_status.mag_inserted = st->mag_inserted;
    pub_status.game_mag = ro.show_mag; pub_status.game_reserve = ro.show_reserve;
    pub_status.shots = G.shots;
    pub_status.throws = G.throws; pub_status.places = G.places; pub_status.dets = G.dets; pub_status.rejects = G.rejects;
    pthread_mutex_unlock(&HM);
    return out;
}

void vrgame_frame_end(void) {
    MvVrForearm f[2];
    int ok[2];
    for (int i = 0; i < 2; i++) {
        VrBodyForearm b;
        ok[i] = G.body_ok && vrbody_forearm_pose(i, &b);
        if (ok[i]) { memcpy(f[i].elbow, b.elbow, sizeof b.elbow); memcpy(f[i].wrist, b.wrist, sizeof b.wrist); memcpy(f[i].dorsal, b.dorsal, sizeof b.dorsal); }
    }
    VrBodyStats bst;
    vrbody_stats(&bst);
    VrBodyArmLive al[2];
    int aok[2];
    for (int i = 0; i < 2; i++) aok[i] = G.body_ok && vrbody_arm_live(i, &al[i]);
    uint64_t tnow = mono_ns();
    pthread_mutex_lock(&HM);
    if (bst.applies != pub_live_applies) { pub_live_applies = bst.applies; pub_live_ns[0] = pub_live_ns[1] = tnow; }   /* instante del último apply (también sin brazos movidos) */
    for (int i = 0; i < 2; i++) { pub_live_ok[i] = aok[i]; if (aok[i]) pub_live[i] = al[i]; }
    {   /* dorso de reserva = el MISMO que la ruta viva (mv_vrgame_forearm_live): +Y del marco de MANO (agarre actual girado por hand_rot_offset), no el +Y del agarre (45 grados desviado); sin agarre válido no se publica (el consumidor usa otra fuente) */
        const VrBodyConfig *bc = vrbody_config();
        for (int i = 0; i < 2; i++) {
            if (ok[i]) {
                GripPose g = grip_eff_locked(i, tnow);
                float m[16], bone[3], d[3];
                for (int k = 0; k < 3; k++) bone[k] = f[i].wrist[k] - f[i].elbow[k];
                if (g.valid) m16_from_pq(g.p, g.q, m);
                if (g.valid && vrradar_dorsal_from_grip(m, bc ? bc->hand_rot_offset[i] : NULL, bone, d)) memcpy(f[i].dorsal, d, sizeof d);
                else ok[i] = 0;
            }
            pub_fore_ok[i] = ok[i]; if (ok[i]) pub_fore[i] = f[i];
        }
    }
    pub_status.arms_driven = bst.arms_driven;
    pub_status.loader = G.cur >= 0 ? atomic_load(&SL[G.cur].state) : pub_status.loader;
    pthread_mutex_unlock(&HM);
}

/* ============================================================================================================== lado de render */
/* Compensación de latencia: el arma se simula en el hilo del motor a la cadencia del VBlank y su frame trae la mano de ese instante.
 * Aquí, al pedirlo el renderer, se mueve todo el frame (las piezas del arma, no los objetos sueltos) con el movimiento rígido de la
 * mano desde entonces hasta ahora: D = H_ahora * inv(H_sim). Se ignora si el salto es incoherente (recentrado, pérdida de seguimiento). */
/* IK del guante repetida con el agarre vivo (HM tomado). 1 = válido. dev_mm: desvío del agarre vivo respecto al simulado; age_ms: edad de la IK. */
static int live_arm_locked(int side, uint64_t now, VrBodyArmLive *a, GripPose *gn, VrBodyArmPose *ap, float *dev_mm, float *age_ms) {
    if (!pub_live_ok[side] || now - pub_live_ns[side] > GLOVE_MAX_AGE_NS) return 0;
    *gn = grip_eff_locked(side, now);
    if (!gn->valid) return 0;
    *a = pub_live[side];
    if (vrbody_arm_resolve(a, gn->p, gn->q, ap) != 0) return 0;
    if (dev_mm) *dev_mm = 1000.0f * sqrtf((gn->p[0] - a->grip_p[0]) * (gn->p[0] - a->grip_p[0]) + (gn->p[1] - a->grip_p[1]) * (gn->p[1] - a->grip_p[1]) + (gn->p[2] - a->grip_p[2]) * (gn->p[2] - a->grip_p[2]));
    if (age_ms) *age_ms = (float)((double)(now - pub_live_ns[side]) * 1e-6);
    return 1;
}

int32_t mv_vrgame_forearm_live(int32_t side, MvVrForearm *out, float *dev_mm, float *age_ms) {
    if (!out || side < 0 || side > 1) return 0;
    VrBodyArmLive a; GripPose gn; VrBodyArmPose ap;
    uint64_t now = mono_ns();
    pthread_mutex_lock(&HM);
    int ok = live_arm_locked(side, now, &a, &gn, &ap, dev_mm, age_ms);
    pthread_mutex_unlock(&HM);
    if (!ok) return 0;
    float m[16], bone[3], d[3];
    m16_from_pq(gn.p, gn.q, m);
    for (int i = 0; i < 3; i++) bone[i] = ap.wrist[i] - ap.elbow[i];
    if (!(sqrtf(bone[0] * bone[0] + bone[1] * bone[1] + bone[2] * bone[2]) > 1e-4f)) return 0;
    /* Dorso = +Y del marco de MANO (agarre girado por hand_rot_offset: roll 45 grados), no +Y del agarre (45 grados desviado hacia el pulgar: el panel del radar quedaba de lado), sin la componente del hueso */
    const VrBodyConfig *bc = vrbody_config();
    if (!vrradar_dorsal_from_grip(m, bc ? bc->hand_rot_offset[side] : NULL, bone, d)) return 0;
    for (int i = 0; i < 3; i++) { out->elbow[i] = ap.elbow[i]; out->wrist[i] = ap.wrist[i]; out->dorsal[i] = d[i]; }
    return 1;
}

/* Mallas del guante del frame (HM tomado): una por pieza y lado, vértices ya en el espacio VR (VRH_MF_DYNAMIC, modelo = identidad). Devuelve cuántas añadió a `mesh` (como mucho `room`) y,
 * si añadió, rellena `tex` con el atlas y marca el hueco como en lectura hasta mv_host_hands_done. */
static VrGloveOut glv_out[2][2];                  /* [paridad del frame][lado]: el frame anterior sigue en uso por el renderer */
static unsigned glv_par;
static float glv_grip[2]; static uint64_t glv_grip_ns[2];
static int glove_emit(uint64_t now, VrHandsMesh *mesh, int room, VrHandsTex *tex) {
    if (!glv_on || glv_pub < 0 || room < 6) return 0;
    const VrGlove *g = glv[glv_pub];
    if (!g || !g->valid) return 0;
    const VrBodyConfig *bc = vrbody_config();
    int n = 0;
    glv_par ^= 1u;
    for (int side = 0; side < 2; side++) {
        VrBodyArmLive a; GripPose gn; VrBodyArmPose ap;
        if (!live_arm_locked(side, now, &a, &gn, &ap, NULL, NULL)) { glv_grip_ns[side] = 0; continue; }
        /* agarre: el del mando a la cadencia del renderer (suavizado como el de vr/body, 0,06 s), con el mínimo del arma/golpe y el fundido de la IK */
        float tgt = fminf(fmaxf(fmaxf(hand_in[side].grip, a.grip_floor), 0.0f), 1.0f) * a.fade;
        float dt = glv_grip_ns[side] ? (float)((double)(now - glv_grip_ns[side]) * 1e-9) : 1.0f;
        glv_grip_ns[side] = now;
        if (dt > 0.2f) glv_grip[side] = tgt;
        else glv_grip[side] += (1.0f - expf(-fmaxf(dt, 0.0f) / 0.06f)) * (tgt - glv_grip[side]);
        VrGlovePose pz;
        memset(&pz, 0, sizeof pz);
        memcpy(pz.org[0], ap.shoulder, sizeof pz.org[0]); memcpy(pz.org[1], ap.elbow, sizeof pz.org[1]); memcpy(pz.org[2], ap.wrist, sizeof pz.org[2]);
        memcpy(pz.R[0], ap.R_upper, sizeof pz.R[0]); memcpy(pz.R[1], ap.R_fore, sizeof pz.R[1]); memcpy(pz.R[2], ap.R_hand, sizeof pz.R[2]);
        pz.upm = a.upm; pz.grip = glv_grip[side];
        if (bc && bc->fist_enabled) { pz.fist = &bc->fist; pz.fist_min = bc->fist_min_grip; }
        VrGloveOut *o = &glv_out[glv_par][side];
        if (vrglove_pose(g, side, &pz, o) != 0) continue;
        for (int k = 0; k < 3; k++) {
            const VrGlovePiece *P = &g->p[side][k];
            if (!o->nv[k] || !P->nidx_out) continue;
            VrHandsMesh *m = &mesh[n++];
            memset(m, 0, sizeof *m);
            m->mesh_id = 0x80000000u | (g->id << 4) | (uint32_t)(side * 3 + k);
            m->tex_id = 0x80000000u | g->id;
            m->model[0] = m->model[5] = m->model[10] = m->model[15] = 1.0f;
            m->verts = o->v[k]; m->nverts = o->nv[k];
            m->idx = P->idx; m->nidx = P->nidx_out;
            m->flags = VRH_MF_DYNAMIC;
        }
    }
    if (n > 0) {
        tex->id = 0x80000000u | g->id; tex->w = g->aw; tex->h = g->ah; tex->rgba = g->atlas;
        glv_read = glv_pub;
    }
    return n;
}

/* Vértices (xyz, m, espacio recentrado) de la malla del antebrazo del guante con la pose VIVA (la misma IK que mv_vrgame_forearm_live): el radar de muñeca se coloca según su radio real.
 * Copia como mucho max_verts; devuelve cuántos (0 = sin guante vivo). La pose se arma como en glove_emit, con la mano abierta (el puño solo deforma la mano). */
int32_t mv_vrgame_forearm_mesh(int32_t side, float *xyz, int32_t max_verts) {
    if (!xyz || max_verts <= 0 || side < 0 || side > 1) return 0;
    VrBodyArmLive a; GripPose gn; VrBodyArmPose ap;
    VrGloveOut o;
    memset(&o, 0, sizeof o);
    int32_t n = 0;
    pthread_mutex_lock(&HM);
    const VrGlove *g = glv_on && glv_pub >= 0 ? glv[glv_pub] : NULL;
    if (g && g->valid && live_arm_locked(side, mono_ns(), &a, &gn, &ap, NULL, NULL)) {
        VrGlovePose pz;
        memset(&pz, 0, sizeof pz);
        memcpy(pz.org[0], ap.shoulder, sizeof pz.org[0]); memcpy(pz.org[1], ap.elbow, sizeof pz.org[1]); memcpy(pz.org[2], ap.wrist, sizeof pz.org[2]);
        memcpy(pz.R[0], ap.R_upper, sizeof pz.R[0]); memcpy(pz.R[1], ap.R_fore, sizeof pz.R[1]); memcpy(pz.R[2], ap.R_hand, sizeof pz.R[2]);
        pz.upm = a.upm;
        if (vrglove_pose(g, side, &pz, &o) == 0) {
            n = (int32_t)o.nv[1] < max_verts ? (int32_t)o.nv[1] : max_verts;
            for (int32_t i = 0; i < n; i++) memcpy(xyz + 3 * i, o.v[1][i].pos, 3 * sizeof(float));
        }
    }
    pthread_mutex_unlock(&HM);
    return n;
}

/* El renderer termina con el frame: se libera la lectura del arma (vr_hands) y la del guante. */
void mv_host_hands_done(void) {
    vrhands_release();
    pthread_mutex_lock(&HM);
    glv_read = -1;
    pthread_mutex_unlock(&HM);
}

/* [agarre] (1 Hz): desvío (mm) entre el punto de empuñadura del arma dibujada (marco de la mano del arma, que sale del agarre del mando de ahora, * centro de agarre) y el centro de la palma de la mano
 * con piel (la IK del guante con ese mismo agarre: muñeca + R_mano * centro de agarre), y cuánto movió la compensación el arma respecto a la mano con la que se simuló (sim -> ahora). El arma sigue al mando
 * sin pasar por la IK: el desvío es lo que la IK no alcanza (brazo estirado, giro del antebrazo) más la edad de la IK del guante; -1 si no hay guante vivo. */
static void agarre_diag(uint64_t now, int side, const GripPose *wh, const GripPose *sim) {
    static uint64_t next_ns;
    if (now < next_ns) return;
    next_ns = now + 1000000000ull;
    float c[3], gw[3], rc[3], ds = 0, delta = -1.0f;
    vrhs_grip_center(side == 1 ? 1 : -1, c);
    for (int i = 0; i < 3; i++) c[i] *= VRHS_UNIT_M;
    q_rot(wh->q, c, rc);
    for (int i = 0; i < 3; i++) { gw[i] = wh->p[i] + rc[i]; ds += (sim->p[i] - wh->p[i]) * (sim->p[i] - wh->p[i]); }
    VrBodyArmLive a; GripPose gn; VrBodyArmPose ap;
    if (live_arm_locked(side, now, &a, &gn, &ap, NULL, NULL)) {
        float d = 0;
        for (int i = 0; i < 3; i++) {
            float pc = ap.wrist[i] + (ap.R_hand[3 * i] * c[0] + ap.R_hand[3 * i + 1] * c[1] + ap.R_hand[3 * i + 2] * c[2]);
            d += (gw[i] - pc) * (gw[i] - pc);
        }
        delta = 1000.0f * sqrtf(d);
    }
    fprintf(stderr, "[agarre] lado=%d delta_mm=%.1f (empuñadura del arma - centro de la palma con piel; -1 = sin guante vivo) comp_sim_ahora_mm=%.1f\n", side, delta, 1000.0f * sqrtf(ds));
}

int mv_host_hands(VrHandsFrame *out) {
    static VrHandsMesh buf[VRH_MAX_MESHES];
    static VrHandsTex tbuf[VRH_MAX_BOUND + 1];
    static WGhostMesh gzbuf[3];       /* fantasma de la mina: mismo anillo y mismo hueco que el láser (nunca a la vez) */
    static WLaserMesh lzbuf[3];       /* láser: el renderer lee el frame anterior mientras se arma el siguiente (mismo anillo de 3 que vr_hands) */
    static unsigned lzi;
    int rc, copied = 0, have_d = 0;
    float dq[4] = {0, 0, 0, 1}, dp[3] = {0, 0, 0};      /* movimiento rígido de la mano desde la simulación (D), para el láser */
    uint64_t now = mono_ns();
    pthread_mutex_lock(&HM);
    glv_read = -1;
    hd_last.valid = 0;
    rc = vrhands_fetch(out);
    if (rc && pub_sim_valid && out->meshes && out->nmeshes > 0 && out->nmeshes <= VRH_MAX_MESHES) {
        int w = opt_right ? 1 : 0;
        GripPose hn = hand_frame_locked(w, now), hs = pub_sim_hand;      /* el arma cuelga del mando de AHORA (90 Hz): mano = agarre vivo * desvío fijo */
        if (hn.valid && hs.valid) {
            agarre_diag(now, w, &hn, &hs);
            float qc[4], qd[4], pd[3], rp[3];
            q_conj(hs.q, qc);
            q_mul(hn.q, qc, qd);            /* Rd = Rn * Rs^-1 */
            q_norm(qd);
            q_rot(qd, hs.p, rp);
            for (int i = 0; i < 3; i++) pd[i] = hn.p[i] - rp[i];
            float dmove = sqrtf((hn.p[0] - hs.p[0]) * (hn.p[0] - hs.p[0]) + (hn.p[1] - hs.p[1]) * (hn.p[1] - hs.p[1]) + (hn.p[2] - hs.p[2]) * (hn.p[2] - hs.p[2]));
            float ang = 2.0f * acosf(fminf(1.0f, fabsf(qd[3])));
            if (dmove < 0.35f && ang < 1.2f) {
                float D[16];
                m16_from_pq(pd, qd, D);
                memcpy(dq, qd, sizeof dq); memcpy(dp, pd, sizeof dp); have_d = 1;
                hd_last.valid = 1; hd_last.ser = pub_special_ser; memcpy(hd_last.dq, qd, sizeof hd_last.dq); memcpy(hd_last.dp, pd, sizeof hd_last.dp);
                for (int k = 0; k < out->nmeshes; k++) {
                    buf[k] = out->meshes[k];
                    if (buf[k].flags & VRH_MF_HELD) continue;
                    const float *M = out->meshes[k].model;
                    for (int c = 0; c < 4; c++)
                        for (int r = 0; r < 4; r++)
                            buf[k].model[4 * c + r] = D[r] * M[4 * c + 0] + D[4 + r] * M[4 * c + 1] + D[8 + r] * M[4 * c + 2] + D[12 + r] * M[4 * c + 3];
                }
                out->meshes = buf;
                copied = 1;
            }
        }
    }
    /* guantes: se añaden a la lista del arma (o forman la lista solos) */
    int nw = rc && out->meshes && out->nmeshes > 0 && out->nmeshes <= VRH_MAX_MESHES ? out->nmeshes : 0;
    VrHandsTex gtex;
    memset(&gtex, 0, sizeof gtex);
    VrHandsMesh gm[6];
    int ng = nw + 6 <= VRH_MAX_MESHES ? glove_emit(now, gm, 6, &gtex) : 0;
    if (ng > 0) {
        if (!rc) {                       /* sin arma: iluminación por defecto de vr/hands */
            VrHandsConfig hc;
            vrhands_default_config(&hc);
            memset(out, 0, sizeof *out);
            memcpy(out->light_dir, hc.light_dir, sizeof out->light_dir); out->ambient = hc.ambient; out->diffuse = hc.diffuse;
        } else if (!copied) memcpy(buf, out->meshes, (size_t)nw * sizeof buf[0]);
        memcpy(buf + nw, gm, (size_t)ng * sizeof buf[0]);
        out->meshes = buf; out->nmeshes = nw + ng;
        int nt = rc && out->tex && out->ntex > 0 ? (out->ntex > VRH_MAX_BOUND ? VRH_MAX_BOUND : out->ntex) : 0;
        if (nt) memcpy(tbuf, out->tex, (size_t)nt * sizeof tbuf[0]);
        tbuf[nt] = gtex;
        out->tex = tbuf; out->ntex = nt + 1;
        rc = 1;
    }
    /* láser de la pistola: una malla dinámica más al final de la lista (el renderer admite 8 dinámicas por frame: 6 del guante + 1). Con la misma compensación de latencia que el arma. */
    if (pub_laser.on && (!rc || (out->meshes && out->nmeshes > 0 && out->nmeshes < VRH_MAX_MESHES))) {
        float lp[3], lq[4];
        memcpy(lp, pub_laser.p, sizeof lp); memcpy(lq, pub_laser.q, sizeof lq);
        if (have_d && !pub_laser.ghost) {      /* el fantasma está fijo en el suelo: no sigue a la mano */
            float rp[3], nq[4];
            q_rot(dq, lp, rp);
            for (int i = 0; i < 3; i++) lp[i] = rp[i] + dp[i];
            q_mul(dq, lq, nq); q_norm(nq);
            memcpy(lq, nq, sizeof lq);
        }
        WPose lz = {{lp[0], lp[1], lp[2]}, {lq[0], lq[1], lq[2], lq[3]}};
        WLaserMesh *lm = &lzbuf[lzi % 3];
        WGhostMesh *gm = &gzbuf[lzi % 3];
        if (pub_laser.ghost ? wghost_build(&lz, 0.45f, gm) : wlaser_build(&lz, wlaser_length(pub_laser.hit_m), lm)) {
            lzi++;
            int n0 = rc ? out->nmeshes : 0;
            if (!rc) {                       /* sin arma publicada ni guante: iluminación por defecto de vr/hands */
                VrHandsConfig hc;
                vrhands_default_config(&hc);
                memset(out, 0, sizeof *out);
                memcpy(out->light_dir, hc.light_dir, sizeof out->light_dir); out->ambient = hc.ambient; out->diffuse = hc.diffuse;
            } else if (out->meshes != buf) memcpy(buf, out->meshes, (size_t)n0 * sizeof buf[0]);
            if (pub_laser.ghost) wghost_mesh(gm, &buf[n0]); else wlaser_mesh(lm, &buf[n0]);
            out->meshes = buf; out->nmeshes = n0 + 1;
            rc = 1;
        }
    }
    pthread_mutex_unlock(&HM);
    return rc;
}
