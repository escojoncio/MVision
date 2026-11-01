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
#include "vr_glove.h"
#include "vr_grip.h"
#include "vr_hand_skin.h"
#include "vr_hand_pose.h"
#include "vr_hands.h"
#include "vr_pose.h"
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
 * game_id = índice de arma del puente (0 pistola, 1 fusil, 2 granada a, 3 misil guiado, 5 mina, 6 adhesivo, 8 granada c, 9 francotirador).
 * Sin entrada: granada b (7, sin modelo identificado): siguen con el arma del propio juego. */
#define WPN_STAGE "init"
typedef struct { int game_id; uint16_t model; } WpnModel;
static const WpnModel WPN_TAB[] = {
    {0, 0x36f6}, {1, 0x4725}, {9, 0x2d3b}, {3, 0x15a9}, {4, 0x57f8}, {2, 0x3b88}, {8, 0x761a}, {6, 0xf83d}, {5, 0x17a1},
};
#define NSLOT ((int)(sizeof WPN_TAB / sizeof WPN_TAB[0]))
/* Modelo VR del arma de id de juego dado (id16 del modelo en WPN_STAGE); 0 = sin modelo VR: se queda el arma del propio juego (granada 7, aturdidora/humo sin modelo identificado). */
uint16_t vrgame_weapon_model(int game_id) {
    for (int i = 0; i < NSLOT; i++) if (WPN_TAB[i].game_id == game_id) return WPN_TAB[i].model;
    return 0;
}
static int is_grenade(int game_id) { return game_id == 2 || game_id == 7 || game_id == 8; }
/* Granadas: los datos no dan el modelo que usa el juego por tipo (los de WPN_TAB son «dudosa» en wcut_tables.c; el de la 8 salía blanco mientras el juego lanzaba otro), así que se dibuja el modelo real del
 * propio juego (escena nativa, sin ocultarlo) y la lógica VR (gesto, trayectoria) sigue; el lanzado es siempre el actor del juego. Así la mano y el lanzado son el mismo modelo, uno solo. */
int vrgame_weapon_uses_game_model(int game_id) { return is_grenade(game_id); }

static uint64_t mono_ns(void) {
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
static int opt_right = 1, opt_reload = WR_AUTOMATIC;   /* recarga por defecto automática: sin cinturón ni cargadores a mano */
static int opt_aim = 1;            /* sin efecto: el arma cuelga de la mano por el zócalo de vr_grip (la boca apunta como la mano), ya no toma el apuntado del mando */
static float goff_p[3] = {0, 0, 0}, goff_q[4] = {0, 0, 0, 1};
static float hap_out[2];
static MvVrGameStatus pub_status;
static MvVrSpecialView pub_special;      /* vista especial del arma (mira, visor, pantalla del misil) */
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
static struct { int on; float p[3], q[4]; } pub_laser;      /* láser de la pistola: pose de la boca (espacio VR) del último VBlank; on = 0: no se dibuja (w_laser.h) */

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
    opt_reload = reload_mode == 1 ? WR_SIMPLIFIED : reload_mode == 2 ? WR_AUTOMATIC : WR_MANUAL;
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
static GripPose hand_frame_locked(int side, uint64_t now) {
    GripPose g = grip_locked(&hand_in[side], now);
    return hand_frame_from_grip(side, &g);
}

/* Manos para la IK de los brazos (hilo de emulación, etapa de dibujado). */
int vrpose_get_hands(VrHandPose out[2]) {
    uint64_t now = mono_ns();
    int n = 0;
    pthread_mutex_lock(&HM);
    for (int i = 0; i < 2; i++) {
        GripPose g = grip_locked(&hand_in[i], now);
        memset(&out[i], 0, sizeof out[i]);
        if (!g.valid) continue;
        out[i].valid = 1;
        memcpy(out[i].pos, g.p, sizeof g.p); memcpy(out[i].rot, g.q, sizeof g.q);
        if (hand_in[i].aim_valid) { out[i].aim_valid = 1; memcpy(out[i].aim_rot, hand_in[i].aim_rot, sizeof out[i].aim_rot); }   /* el apuntado endereza la mano del personaje (vr/body) */
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
    int n = vrbody_calibrate_hands();
    if (n > 0) { cal_serial++; vrgame_haptic(0, 1.0f); vrgame_haptic(1, 1.0f); }     /* una calibración fallida (n = 0) no cambia nada ni se persiste */
    fprintf(stderr, "[manos] calibradas %d manos (corrección agarre->mano fija)%s\n", n, n ? "" : ": ninguna (¿mandos sin seguimiento o dedos casi verticales?)");
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
void mv_vrgame_status(MvVrGameStatus *out) {
    if (!out) return;
    pthread_mutex_lock(&HM);
    *out = pub_status;
    pthread_mutex_unlock(&HM);
}
int32_t mv_vrgame_special_view(MvVrSpecialView *out) {
    if (!out) return 0;
    pthread_mutex_lock(&HM);
    *out = pub_special;
    pthread_mutex_unlock(&HM);
    return out->kind != MV_VIEW_NONE;
}
void mv_vrgame_set_missile_view(int32_t mode) {
    pthread_mutex_lock(&HM);
    opt_missile_view = mode == 1;
    pthread_mutex_unlock(&HM);
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

static void *load_thread(void *arg) {
    int si = (int)(intptr_t)arg;
    Slot *sl = &SL[si];
    const uint16_t model = WPN_TAB[si].model;
    int rc = -1;
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
#define PLACE_LIFT_M 0.02f      /* separación del suelo (la mina se apoya, no se hunde) */
#define THROW_TICK_S 0.04f      /* un tick del motor (2 VBlanks de 50 Hz) */

static struct {
    int body_ok, fire_ok;
    int inited;
    uint64_t last_ns;
    int cur;                    /* ranura del arma VR mostrada en el VBlank anterior (-1 ninguna) */
    int active;                 /* modo arma VR del VBlank anterior */
    int prev_mag_in;            /* mag_inserted del paso anterior (flanco "cargador insertado") */
    int prev_state;
    int fire_edge;              /* lo pone el callback fire() (o el gancho de lanzamiento) dentro de update */
    WPose fire_pose;
    int boot_done;              /* la primera toma de munición del arma (arranque) no cuenta como cargador de la cintura */
    int mag_weapon;             /* el arma activa lleva cargador en el juego (contabilidad virtual de cargadores) */
    int ammo_game;              /* munición total del arma activa en el juego (para los lanzamientos) */
    int game_mag;               /* cargador del juego (el cargador VR arranca con él) */
    VrFireMap map;              /* correspondencia usuario -> motor del VBlank (para el gancho de colocación) */
    WPose head;                 /* cabeza del VBlank (usuario) */
    float floor_u;              /* altura del suelo del juego en espacio de usuario (m) */
    int floor_ok;
    int det_t;                  /* VBlanks restantes pulsando el botón de detonación */
    WGameBridge br;
    VrMissile mis;              /* misil guiado del juego vigilado (vr_missile.h) */
    uint32_t mis_serial;        /* nº de misiles creados visto por última vez (vrfire_last_missile) */
    float steady;               /* efecto del tranquilizante suavizado (0..1) */
    VrReloadOut ro;             /* última salida de la recarga */
    uint32_t shots, arms, throws, places, dets, rejects;
    int last_disc;
    int logged;
} G;

static void publish_off(void) {
    vrhands_post_clear();
    pthread_mutex_lock(&HM);
    pub_sim_valid = 0;
    pub_laser.on = 0;
    memset(&pub_special, 0, sizeof pub_special);
    pthread_mutex_unlock(&HM);
}

void vrgame_reset(void) {
    vrbodyg_reset();
    G.inited = 0; G.active = 0; G.cur = -1; G.prev_mag_in = 0; G.prev_state = VRR_OFF; G.fire_edge = 0;
    G.boot_done = 0; G.last_ns = 0; G.det_t = 0;
    aimfix_clear();
    memset(&G.ro, 0, sizeof G.ro);
    G.ro.sync_rounds = G.ro.sync_chambered = -1;
    publish_off();      /* un arma publicada antes de reiniciar el motor no debe quedarse dibujada */
    pthread_mutex_lock(&HM);
    glv_pub = -1; pub_live_ok[0] = pub_live_ok[1] = 0;      /* los huecos se reutilizan (el renderer puede estar leyendo uno) */
    pthread_mutex_unlock(&HM);
}
void vrgame_set_enabled(int body_ok, int fire_ok) { G.body_ok = body_ok; G.fire_ok = fire_ok; }

static int cb_ammo_reserve(int id) {
    /* El cargador VR refleja al del juego: al arrancar el arma se llena con el cargador del juego (que no cuenta en la reserva), también con reserva 0 o menor que el cargador. */
    if (!G.boot_done && G.mag_weapon) return G.game_mag > 0 ? G.game_mag : 0;
    return vrreload_ammo_reserve(id);
}
static void cb_ammo_take(int id, int n) {
    /* Solo las armas de cargador llevan libro virtual (cargadores cogidos de la cintura). Las demás (misil, granadas, minas)
     * gastan la munición el propio juego al crear el proyectil: descontarla aquí la contaría dos veces. */
    if (!G.boot_done || !G.mag_weapon) return;
    vrreload_ammo_take(id, n);
}
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
#define SCOPE_TEX 384            /* lado (texels) de la textura de la mira */
#define SCOPE_LENS_D 0.040f      /* diámetro de la lente (el ocular de la mira), m */
#define AA_SIGHT_DIST 3.0f       /* distancia (m) a la que se coloca el HUD del visor (colimado: se ve a esa distancia con el tamaño angular del juego) */
static void put_pose(const WPose *w, float p[3], float q[4]) {
    p[0] = w->p.x; p[1] = w->p.y; p[2] = w->p.z;
    q[0] = w->q.x; q[1] = w->q.y; q[2] = w->q.z; q[3] = w->q.w;
}
static void special_view(int gid, WeaponState *st, const WeaponDef *def, const WPose *W, const WInput *in) {
    MvVrSpecialView sv;
    int fullscreen;
    memset(&sv, 0, sizeof sv);
    pthread_mutex_lock(&HM);
    fullscreen = opt_missile_view;
    pthread_mutex_unlock(&HM);
    if (gid == 9) {
        sv.kind = MV_VIEW_SCOPE;
        sv.blend = st->channel[WCH_SCREEN];
        sv.zoom = sniper_zoom(st);
        sv.cam_valid = st->aux.active || sv.blend > 0.01f;
        put_pose(&st->aux.camera, sv.cam_pos, sv.cam_rot);
        sv.cam_fov_y = st->aux.fov_y;
        sv.tex_w = sv.tex_h = SCOPE_TEX;
        WPose lens = wpose_mul(*W, def->parts[SNIPER_PT_LENS].rest);
        put_pose(&lens, sv.surf_pos, sv.surf_rot);
        sv.surf_w = sv.surf_h = SCOPE_LENS_D;
        sv.round = 1;
        sv.suppress_frame = vrcam_zoom_capped();  /* solo mientras el juego tiene SU mira activa (pidió más aumento que el tope): esa máscara es la abertura, que dibuja vr_scope en la lente; otras máscaras (gafas, careta) se conservan */
        sv.hide_hud = sv.blend > 0.25f;
        vrcam_set_zoom_cap(320);                 /* la cabeza no se acerca: el aumento está en la lente */
    } else {
        vrcam_set_zoom_cap(0);
        if (gid == 4) {
            VrAaSight sg;
            vraa_sight(&sg);
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
                sv.hide_hud = 1;
                sv.locked = sg.locked; sv.converged = sg.converged;
            }
        } else if (gid == 3 && st->phase != G_IDLE) {
            const GuidedState *gs = (const GuidedState *)st;
            WPose pnl;
            float pw = 0.1f, ph = 0.075f;
            float e = guided_view_panel(st, in, &pnl, &pw, &ph);
            sv.kind = MV_VIEW_MISSILE;
            sv.blend = e;
            if (fullscreen) {        /* pantalla grande fija ante la cara (comodidad: la cabeza no se mueve nunca) */
                WVec3 c = wv_add(in->head.p, wquat_rotate(in->head.q, wv(0, -0.05f, -1.1f)));
                pnl.p = c; pnl.q = in->head.q; pw = 1.4f; ph = 1.05f;
                sv.fullscreen = 1; sv.blend = 1.0f;
            }
            put_pose(&pnl, sv.surf_pos, sv.surf_rot);
            sv.surf_w = pw; sv.surf_h = ph;
            sv.cam_valid = st->phase == G_FLIGHT && G.mis.alive;
            put_pose(&st->aux.camera, sv.cam_pos, sv.cam_rot);
            sv.cam_fov_y = st->aux.fov_y;
            sv.tex_w = GD_VIEW_W; sv.tex_h = GD_VIEW_H;
            sv.noise = gs->noise;
            sv.ended = st->phase == G_STATIC;
        }
    }
    pthread_mutex_lock(&HM);
    pub_special = sv;
    pthread_mutex_unlock(&HM);
}

/* ---- colocación (mina, carga adhesiva) ----
 * El juego decide la superficie y si se puede; aquí solo se da la pose. El jugador puede estar SENTADO: no se exige acercar
 * la mano al suelo físico. Desde la pose del objeto en la mano se traza el rayo de su "delante" (-Z) y se proyecta sobre el
 * suelo del juego (plano a la altura de los pies del personaje); sin inclinación hacia abajo se coloca a la altura de la mano
 * a la distancia máxima alcanzable (pared: no hay geometría del escenario en esta capa, es aproximado). Dentro de PLACE_REACH_M
 * de la cabeza; fuera se rechaza (el arma no gasta nada). Devuelve 0 si se rechaza. */
static int place_project(const WPose *obj, WPose *out) {
    if (!isfinite(obj->p.x) || !isfinite(obj->p.y) || !isfinite(obj->p.z) || !isfinite(obj->q.x) || !isfinite(obj->q.y) || !isfinite(obj->q.z) ||
        !isfinite(obj->q.w) || !isfinite(G.head.p.x) || !isfinite(G.head.p.z)) return 0;   /* mano no rastreada / pose inválida: no se coloca */
    float qn = sqrtf(obj->q.x * obj->q.x + obj->q.y * obj->q.y + obj->q.z * obj->q.z + obj->q.w * obj->q.w);
    if (!(qn > 1e-3f)) return 0;
    const float qx = obj->q.x / qn, qy = obj->q.y / qn, qz = obj->q.z / qn, qw = obj->q.w / qn;   /* la fórmula del "delante" exige cuaternión unitario */
    float fx = -2 * (qx * qz + qw * qy);
    float fy = -2 * (qy * qz - qw * qx);
    float fz = -(1 - 2 * (qx * qx + qy * qy));
    float hx = obj->p.x - G.head.p.x, hz = obj->p.z - G.head.p.z;
    float p[3] = {obj->p.x, obj->p.y, obj->p.z};
    if (G.floor_ok && fy < -0.2f && obj->p.y > G.floor_u) {          /* apunta hacia abajo: suelo */
        float t = (G.floor_u + PLACE_LIFT_M - obj->p.y) / fy;
        p[0] = obj->p.x + fx * t; p[1] = G.floor_u + PLACE_LIFT_M; p[2] = obj->p.z + fz * t;
    } else {                                                          /* horizontal: a la distancia máxima */
        float hl = sqrtf(fx * fx + fz * fz);
        if (hl < 1e-3f) return 0;
        float t = PLACE_REACH_M * 0.8f - sqrtf(hx * hx + hz * hz);
        if (t < 0.1f) t = 0.1f;
        p[0] = obj->p.x + fx / hl * t; p[2] = obj->p.z + fz / hl * t;
    }
    float dx = p[0] - G.head.p.x, dz = p[2] - G.head.p.z;
    if (!(sqrtf(dx * dx + dz * dz) <= PLACE_REACH_M) || !isfinite(p[1])) return 0;
    out->p.x = p[0]; out->p.y = p[1]; out->p.z = p[2];
    /* orientación: solo la guiñada de la mano (el "delante" del objeto horizontal), así la mina mira hacia donde apunta el jugador */
    float hl = sqrtf(fx * fx + fz * fz);
    float yaw = hl > 1e-3f ? atan2f(-fx, -fz) : 0.0f;
    out->q.x = 0; out->q.y = sinf(yaw * 0.5f); out->q.z = 0; out->q.w = cosf(yaw * 0.5f);
    return 1;
}

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
        if (!vrfire_set_throw(p, v, THROW_TICK_S)) { G.rejects++; return 0; }
        G.fire_edge = 1; G.fire_pose = ev->pose; G.shots++; G.throws++;
        return 1; }
    case WL_PLACE_C:
    case WL_PLACE_MINE: {
        if (G.ammo_game <= 0 || !G.map.valid) { G.rejects++; return 0; }
        WPose pp;
        if (!place_project(&ev->pose, &pp)) { G.rejects++; return 0; }
        G.fire_edge = 1; G.fire_pose = pp; G.shots++; G.places++;
        return 1; }
    case WL_DETONATE:
        G.det_t = DET_PRESS_VBL; G.dets++;
        return 1;
    default:
        return 0;
    }
}

static void init_once(void) {
    if (G.inited) return;
    G.inited = 1;
    VrFireCfg fc;
    vrfire_cfg_default(&fc);
    vrfire_init(&fc);
    VrAaCfg ac;       /* apuntado del lanzador antiaéreo con el buscador del juego (vr_aa.h) */
    vraa_cfg_default(&ac);
    if (vrcam_config()->engine_h > 0) ac.h_scale = 320.0f / (float)vrcam_config()->engine_h;   /* el visor usa la H original del juego */
    vraa_init(&ac);
    VrReloadCfg rc;
    vrreload_cfg_default(&rc);
    vrreload_init(&rc);
    memset(&G.br, 0, sizeof G.br);
    G.br.ammo_reserve = cb_ammo_reserve;
    G.br.ammo_take = cb_ammo_take;
    G.br.fire = cb_fire;
    G.br.haptic = cb_haptic;
    G.br.projectile = cb_projectile;
    G.br.steer = cb_steer;
    vrmis_reset();
    G.ro.sync_rounds = G.ro.sync_chambered = -1;
    G.cur = -1;
    w_throw_set_hook(launch_hook, NULL);
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
/* arma VR mostrada o maniobra de recarga en curso: el juego desequipa el arma un instante al recargar, y entonces no hay manos libres */
int vrgame_weapon_engaged(void) { return G.active || G.cur >= 0 || G.ro.busy; }
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

/* Pruebas: instala un arma ya construida en la ranura de su id (sin cargar modelos del disco). */
void vrgame_test_install(int game_id, void *def, void *state, void *rig) {
    int si = slot_of(game_id);
    if (si < 0) return;
    SL[si].def = (WeaponDef *)def; SL[si].st = (WeaponState *)state; SL[si].rig = (VrHandsRig *)rig;
    atomic_store(&SL[si].state, LD_READY);
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
} ArmaDiag;
static void arma_diag(uint64_t now, const ArmaDiag *a) {
    static uint64_t next_ns;
    if (now < next_ns) return;
    next_ns = now + 1000000000ull;
    fprintf(stderr, "[arma] slot=%d carga=%d modo=%d fire_ok=%d cam=%d nucleo=%d mano_arma_valida=%d | recarga=%d disparo_permitido=%d gatillo=%.2f (crudo %.2f) armado=%d recamara=%d cargador=%d | fo.engaged=%d disparos=%u supresor=%d | juego: arma=%d objeto=%d municion=%d | dist empuñadura_arma-agarre_mando=%.3fm mano_libre-soporte=%.3fm\n",
            a->eq_slot, a->load, a->mode, G.fire_ok, a->cam, a->core, a->gpw, a->rstate, a->fire_allowed, a->trig, a->trig_raw, a->trig_armed, a->chambered, a->mag_in,
            a->engaged, G.shots, a->supp, a->gweapon, a->gitem, a->gammo, a->d_grip, a->d_supp);
}

uint16_t vrgame_pad(uint16_t pad, const BridgeState *bs, uint8_t *ram) {
    init_once();
    vraa_tick();
    if (drv_fn) pad = drv_fn(drv_vbl++, pad, bs);
    pad = vrbodyg_pad(pad, bs, opt_right);     /* gestos de cuerpo (golpe, agarre, pared): añaden/retiran botones del mando del usuario */
    uint64_t now = mono_ns();
    float dt = G.last_ns ? (float)((double)(now - G.last_ns) * 1e-9) : 0.02f;
    G.last_ns = now;
    if (!(dt > 0.001f)) dt = 0.001f;
    if (dt > 0.1f) dt = 0.1f;
    vrfire_grenade_track(ram, dt);       /* diagnóstico [granada]: sigue al actor de la granada lanzada 3 s */

    int cam = vrcam_active();
    int core = bs && (bs->valid & BRIDGE_OK_ALL_CORE) == BRIDGE_OK_ALL_CORE && bs->has_player;

    calib_tick(dt);
    glove_update(ram);

    /* manos (una sola toma coherente) */
    HandSlot hs[2];
    GripPose gp[2], wp[2];       /* gp: agarre (mano del personaje, gestos); wp: marco de la mano nueva de la mano que empuña el arma (vr_grip la cuelga de él) */
    int right, rmode;
    pthread_mutex_lock(&HM);
    right = opt_right; rmode = opt_reload;
    memset(wp, 0, sizeof wp);
    for (int i = 0; i < 2; i++) { hs[i] = hand_in[i]; gp[i] = grip_locked(&hand_in[i], now); }
    if (gp[right ? 1 : 0].valid) wp[right ? 1 : 0] = hand_frame_locked(right ? 1 : 0, now);   /* marco de la MANO del arma: función pura del agarre del mando (hand_frame_from_grip), sin IK */
    pthread_mutex_unlock(&HM);

    /* cuerpo: agarre de cada mano y mano dominante (la IK de los brazos lee las poses por vrpose_get_hands) */
    vrbody_set_dominant(right ? 1 : 0);
    for (int i = 0; i < 2; i++) vrbody_set_grip(i, gp[i].valid ? hs[i].grip : 0.0f);
    for (int i = 0; i < 2; i++) { atomic_store_explicit(&grip_diag_raw[i], hs[i].grip, memory_order_relaxed); atomic_store_explicit(&grip_diag_ok[i], gp[i].valid ? 1 : 0, memory_order_relaxed); }

    /* carga bajo demanda: el arma equipada que tiene modelo y aún no se ha pedido */
    load_poll();
    int eq_slot = core ? slot_of(bs->weapon) : -1;
    if (eq_slot >= 0 && G.fire_ok && cam) load_start(eq_slot);
    int eq_ready = eq_slot >= 0 && atomic_load_explicit(&SL[eq_slot].state, memory_order_acquire) == LD_READY;

    int w = right ? 1 : 0, o = 1 - w;   /* w: mando que empuña el arma; o: el otro */
    /* Arma mostrada: la equipada; durante la maniobra de recarga el juego la desequipa un instante y se conserva la anterior. */
    int slot = eq_ready ? eq_slot : (G.ro.busy && G.cur >= 0 ? G.cur : -1);
    int mode = slot >= 0 && G.fire_ok && cam && gp[w].valid && core;
    if (mode && slot != G.cur && G.cur >= 0 && G.active) {    /* cambio de arma: la anterior se reinicia al volver a mostrarla */
        Slot *old = &SL[G.cur];
        if (atomic_load(&old->state) == LD_READY) { old->def->init(old->st); old->st->def = old->def; }
        G.boot_done = 0; G.prev_mag_in = 0;
    }
    Slot *sl = slot >= 0 ? &SL[slot] : NULL;
    const int gid = slot >= 0 ? WPN_TAB[slot].game_id : -1;
    G.mag_weapon = core && bs->magazine_max > 0 && (gid == 0 || gid == 1 || gid == 9);
    G.ammo_game = core && bs->weapon >= 0 && bs->weapon < BRIDGE_N_WEAPONS ? bs->weapon_ammo[bs->weapon] : 0;
    G.game_mag = core ? bs->magazine : 0;

    /* la recarga corre siempre (si no hay arma VR de cargador vuelve a reposo y suelta su contabilidad) */
    VrReloadGame rg;
    vrreload_read_game(bs, &rg);
    VrReloadIn ri;
    memset(&ri, 0, sizeof ri);
    ri.weapon_id = mode && G.mag_weapon ? gid : -1;
    ri.mag_in_edge = 0;
    ri.ready = 0;
    if (mode && G.mag_weapon) {
        int mi = sl->st->mag_inserted;
        if (!G.active || slot != G.cur) G.prev_mag_in = mi;      /* arma recién mostrada: su cargador inicial no es un gesto de recarga */
        ri.mag_in_edge = mi && !G.prev_mag_in;
        ri.ready = mi && sl->st->chambered;
        G.prev_mag_in = mi;
    } else {
        G.prev_mag_in = 0;
    }
    VrReloadOut ro;
    vrreload_frame(&rg, &ri, &ro);
    /* Armas sin cargador en el juego (misil, granadas, minas): puede disparar mientras quede munición. */
    if (mode && !G.mag_weapon) { ro.fire_allowed = G.ammo_game > 0; ro.state = VRR_READY; ro.busy = 0; }

    if (!mode) {
        vrcam_set_zoom_cap(0);           /* sin mira VR la cabeza recibe el aumento del juego tal cual */
        memset(&G.mis, 0, sizeof G.mis);
        { uint32_t ser; vrfire_last_missile(&ser); G.mis_serial = ser; }
        vraa_set_scope(0, NULL, NULL);
        vraa_gesture(NULL, NULL, 0);     /* sin arma o sin mano rastreada: el gesto del visor se reinicia (sin estado viejo al volver) */
        if (G.active) {
            publish_off();
            /* al volver a mostrarla se reinicia el estado del arma (cargador del juego otra vez) */
            if (G.cur >= 0 && atomic_load(&SL[G.cur].state) == LD_READY) { SL[G.cur].def->init(SL[G.cur].st); SL[G.cur].st->def = SL[G.cur].def; }
            G.boot_done = 0;
        }
        G.active = 0;
        G.cur = -1;
        G.det_t = 0;             /* una pulsación de detonación a medias no sobrevive a desequipar el arma */
        vrbody_set_armed(0);
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
            ArmaDiag ad = {eq_slot, eq_slot >= 0 ? atomic_load(&SL[eq_slot].state) : -1, 0, cam, core, gp[w].valid, ro.state, ro.fire_allowed, fo.engaged, game_has_suppressor(bs),
                           hs[w].trigger, -1.0f, -1, -1, -1, bs->weapon, bs->item, G.ammo_game, -1.0f, -1.0f};
            arma_diag(now, &ad);
        }
        publish_hand_pose(hs, gp, right, NULL, 0);
        uint16_t out = pad;
        if (ro.busy) out = (uint16_t)(out & ~ro.pad_mask);
        out |= ro.pad_force;     /* una maniobra a medias termina aunque el arma ya no esté equipada */
        pthread_mutex_lock(&HM);
        pub_status.loader = eq_slot >= 0 ? atomic_load(&SL[eq_slot].state) : 0;
        pub_status.active = 0; pub_status.weapon = -1; pub_status.mag_rounds = pub_status.chambered = pub_status.mag_inserted = 0;
        pub_status.game_mag = pub_status.game_reserve = 0;
        pub_status.reload_state = ro.state;
        pub_status.shots = G.shots;
        pthread_mutex_unlock(&HM);
        return out;
    }

    /* ---- arma VR mostrada y mandando ---- */
    WeaponState *st = sl->st;
    const WeaponDef *def = sl->def;
    G.active = 1;
    G.cur = slot;
    vrbody_set_armed(1);
    vrfire_set_hide(sl->native ? 0 : 1);      /* las granadas (sl->native, vrgame_weapon_uses_game_model) se dibujan siempre con el modelo del juego y no se oculta; el resto lleva modelo VR y se oculta el del juego (la granada lanzada tampoco se oculta nunca: es otro actor) */

    /* tope/sincronía del cargador VR con el del juego (vr_reload.h): solo con cargador dentro y fuera de la espera de recarga */
    if (G.mag_weapon) {
        if (ro.state == VRR_EMPTY && G.prev_state != VRR_EMPTY) { st->chambered = 0; st->mag_rounds = 0; }   /* el juego vació: el VR también */
        if (ro.state == VRR_READY && st->mag_inserted) {
            if (ro.sync_rounds >= 0) { if (ro.sync_force || st->mag_rounds > ro.sync_rounds) st->mag_rounds = ro.sync_rounds; }
            if (ro.sync_chambered >= 0) { if (ro.sync_force || st->chambered > ro.sync_chambered) st->chambered = ro.sync_chambered; }
        }
    }
    G.prev_state = ro.state;

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
    const VrGripEntry *ge = vrgrip_find(gid);
    {
        WPose sock = ge ? ge->grip : wpose_identity();
        if (!w) sock = vrgrip_mirror(sock);                                   /* mando izquierdo: zócalo reflejado (la mano se dibuja a tamaño real: el zócalo va en m reales) */
        vrhands_set_socket(sl->rig, sock);
    }
    in.hand[0] = vrhands_weapon_hand(sl->rig, &hp);
    /* Boca = apuntado del mando: el eje del cañón (sin retroceso) se lleva al eje de apuntado (-Z de aim_rot) con el giro más corto, aplicado como giro extra agarre -> mano (vrbody_set_aim_fix) a la mano que
     * empuña (y que dibuja vr_body) y al arma que cuelga de ella: la empuñadura sigue en el agarre del mando. La relación agarre/apuntado del mando es fija, así que converge en un fotograma y se queda;
     * sin apuntado válido se mantiene la última corrección 0,25 s y se retira suavemente; solo en pistola y fusil (aimfix_weapon). */
    {
        int fixed_ok = 0;
        if (!aimfix_weapon(gid)) { aimfix_clear_side(w); fixed_ok = 2; }      /* arma sin boca real: nunca se gira */
        else if (hs[w].aim_valid && gp[w].valid) {
            WPose gr = vrhands_grip(sl->rig, st);
            WQuat bq = wquat_mul(wquat_mul(in.hand[0].q, wquat_inv(wquat_normalize(gr.q))), def->points[WP_MUZZLE].q);
            WVec3 bwv = wquat_rotate(wquat_normalize(bq), wv(0, 0, -1));
            float bw[3] = {bwv.x, bwv.y, bwv.z}, aw[3], z[3] = {0, 0, -1}, ax[3], axg[4], qc[4];
            q_rot(hs[w].aim_rot, z, aw);
            ax[0] = bw[1] * aw[2] - bw[2] * aw[1]; ax[1] = bw[2] * aw[0] - bw[0] * aw[2]; ax[2] = bw[0] * aw[1] - bw[1] * aw[0];
            float sn = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]), cs = bw[0] * aw[0] + bw[1] * aw[1] + bw[2] * aw[2];
            float ang = atan2f(sn, cs);
            if (finite_n(bw, 3) && finite_n(aw, 3) && ang < 1.5f) {
                fixed_ok = 1;
                if (ang > 0.0005f) {      /* eje de giro en el marco del agarre: Dg = conj(q_agarre) Dw q_agarre; nuevo = Dg * actual */
                    q_conj(gp[w].q, qc);
                    q_rot(qc, ax, axg);
                    float h = 0.5f * ang, k = sinf(h) / sn, dg[4] = {axg[0] * k, axg[1] * k, axg[2] * k, cosf(h)}, nf[4];
                    q_mul(dg, aimfix_q[w], nf); q_norm(nf);
                    memcpy(aimfix_q[w], nf, sizeof nf);
                    vrbody_set_aim_fix(w, aimfix_q[w]);
                }
            }
        }
        if (fixed_ok == 1) aimfix_miss[w] = 0;
        else if (fixed_ok == 0) aimfix_lost(w, dt);
    }
    if (gp[o].valid) in.hand[1] = wpose_of(&gp[o]);
    else { in.hand[1].p.x = 0; in.hand[1].p.y = -10; in.hand[1].p.z = 0; in.hand[1].q.w = 1; }   /* sin mano libre: lejos de todo punto de interacción */
    in.trigger[0] = hs[w].trigger;
    in.grip[0] = hs[w].grip;
    if (gp[o].valid) { in.trigger[1] = hs[o].trigger; in.grip[1] = hs[o].grip; }
    in.buttons[0] = (hs[w].low ? WB_A : 0u) | (hs[w].up ? WB_B : 0u) | (hs[w].stick ? WB_STICK : 0u);
    if (gp[o].valid) in.buttons[1] = (hs[o].low ? WB_A : 0u) | (hs[o].up ? WB_B : 0u) | (hs[o].stick ? WB_STICK : 0u);
    in.dt = dt;
    /* el gatillo solo actúa cuando el disparo puede salir (recarga del juego en curso, cargador del juego vacío...: sin disparos sin proyectil) */
    if (G.mag_weapon && !(ro.state == VRR_READY && ro.fire_allowed)) in.trigger[0] = 0;
    in.support_held = gp[o].valid && in.grip[1] > 0.5f && wcore_hand_dist(st, &in, 1, WP_SUPPORT) < 0.07f;
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
    }
    int booting = !G.boot_done;
    def->update(st, &in, (WReloadMode)rmode, &G.br);
    G.boot_done = 1;      /* la primera actualización es la del arranque del arma */
    if (booting && G.mag_weapon) G.prev_mag_in = st->mag_inserted;   /* el cargador que trae al arrancar no es un gesto de recarga: sin este ajuste el VBlank siguiente veía "cargador insertado" y, con el cargador del juego a medias, lanzaba la maniobra de recarga nada más equipar */

    /* publicación de las mallas */
    VrHandsFrame vf;
    int nb = sl->native ? 0 : vrhands_build(sl->rig, st, &hp, &vf);   /* fuera del cerrojo: el hilo de render no espera a la construcción de las mallas */
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
    VrFireGame fg;
    vrfire_read_game(ram, bs, &fg);
    VrFireIn fi;
    memset(&fi, 0, sizeof fi);
    fi.active = bs->in_control && !ro.busy && map.valid && (ro.fire_allowed || ro.state == VRR_READY || ro.state == VRR_OFF);
    {
        WPose W = vrhands_weapon_world(sl->rig, st, &hp);
        fi.muzzle = wpose_mul(W, def->points[WP_MUZZLE]);
        if (gid == 4) {
            /* Visor del lanzador: con el arma al ojo el juego tiene su mira y su buscador (vr_fire mantiene el botón) y el buscador
             * del juego corre con la vista de este visor (vr_aa). Sin gesto (a la cadera) se dispara como antes, sin fijar. */
            WPose sight = wpose_mul(W, def->points[WP_EYE]);
            int aim = vraa_gesture(&G.head, &sight, st->phase == AA_P_READY && st->mag_rounds > 0);
            vraa_set_h_scale(vrcam_config()->engine_h > 0 ? 320.0f / (float)vrcam_config()->engine_h : 1.0f);   /* cámara VR activa (mode la exige): el canal lleva la H del motor */
            vraa_set_scope(aim, &map, &sight);
            fi.ready = aim;
            fi.raise = 1;
        } else if (gid == 2 || gid == 7 || gid == 8) {
            /* Granada: mano cerrada sobre ella = el botón de ataque del juego mantenido (es el «preparar»/sacar el pasador del propio juego); abrirla lanza (fire con la
             * velocidad de la mano, ver launch_hook) y el juego la crea al soltar el botón. */
            fi.raise = 1;
            fi.ready = st->phase == WT_PH_READY;
        }
        special_view(gid, st, def, &W, &in);
        /* láser de la pistola (w_laser.h): desde la boca (la del disparo: sin supresor, la del cañón) a lo largo de su eje, el mismo con el que vr_fire orienta la bala; solo con el jugador al mando y el arma levantada */
        WPose lz = def->points[WP_MUZZLE];
        if (gid == 0 && !(in.game_flags & WGF_SUPP)) lz.p.z = w_pist_bare_muzzle_z;
        lz = wpose_mul(W, lz);
        int lz_on = gid == 0 && bs->in_control && wlaser_raised(&lz);
        pthread_mutex_lock(&HM);
        pub_laser.on = lz_on;
        if (lz_on) { pub_laser.p[0] = lz.p.x; pub_laser.p[1] = lz.p.y; pub_laser.p[2] = lz.p.z; pub_laser.q[0] = lz.q.x; pub_laser.q[1] = lz.q.y; pub_laser.q[2] = lz.q.z; pub_laser.q[3] = lz.q.w; }
        pthread_mutex_unlock(&HM);
    }
    fi.fire = G.fire_edge && ro.fire_allowed;
    fi.fire_pose = G.fire_pose;
    fi.fire_life = gid == 5 ? 180 : 0;      /* mina: el juego la crea ~70 VBlanks después (se agacha a colocarla); la petición no debe caducar antes */
    /* Fusil en automático: el juego dispara mientras el botón está mantenido; la pose de cada bala es la boca en vivo. */
    fi.trigger_held = gid == 1 && !rifle_is_semi(st) && in.trigger[0] > 0.5f && ro.fire_allowed && st->chambered > 0;
    fi.map = &map;
    VrFireOut fo;
    vrfire_frame(&fg, &fi, &fo);

    /* mando que ve el juego: gatillo y empuñaduras ya no son del juego (lo que hacen lo hace el arma VR) */
    /* Solo con el jugador al mando: en menús, pausa, comunicador y escenas el mando pasa tal cual (el gatillo VR no se traduce ahí).
     * El agarre de la mano libre (L1) solo se retira mientras sujeta el arma con las dos manos: lejos del arma abre las ventanas del juego. */
    uint16_t out = pad;
    if (bs->in_control) {
        out = (uint16_t)(out & ~(PAD_SQUARE | PAD_R1));
        if (in.support_held) out = (uint16_t)(out & ~PAD_L1);
    }
    if (ro.busy) out = (uint16_t)(out & ~ro.pad_mask);
    out |= ro.pad_force;
    if (gid == 3 && bs->in_control && G.mis.alive) {
        out = (uint16_t)(out & ~0xF0u);        /* durante el guiado la cruceta es solo del guiado (una sola fuente: sin mezclar bits del mando físico) */
        out |= vrmis_pad(vrcam_pad_origin());
    }   /* guiado: la cruceta del juego (vr_missile.h) */
    if (fo.engaged && fo.square) out |= PAD_SQUARE;
    publish_hand_pose(hs, gp, right, ge, in.support_held);
    if (bs->weapon >= 0) {
        ArmaDiag ad = {eq_slot, eq_slot >= 0 ? atomic_load(&SL[eq_slot].state) : -1, 1, cam, core, gp[w].valid, ro.state, ro.fire_allowed, fo.engaged, (in.game_flags & WGF_SUPP) != 0,
                       hs[w].trigger, in.trigger[0], gid == 0 ? w_pist_trig_armed(st) : -1, st->chambered, st->mag_inserted, bs->weapon, bs->item, G.ammo_game,
                       sqrtf((gp[w].p[0] - in.hand[0].p.x) * (gp[w].p[0] - in.hand[0].p.x) + (gp[w].p[1] - in.hand[0].p.y) * (gp[w].p[1] - in.hand[0].p.y) + (gp[w].p[2] - in.hand[0].p.z) * (gp[w].p[2] - in.hand[0].p.z)), gp[o].valid ? wcore_hand_dist(st, &in, 1, WP_SUPPORT) : -1.0f};
        arma_diag(now, &ad);
    }
    if (G.det_t > 0) { G.det_t--; if (bs->in_control) out |= PAD_DETONATE; }
    G.ro = ro;

    pthread_mutex_lock(&HM);
    pub_status.loader = LD_READY;
    pub_status.active = 1;
    pub_status.weapon = gid;
    pub_status.mag_rounds = st->mag_rounds; pub_status.chambered = st->chambered; pub_status.mag_inserted = st->mag_inserted;
    pub_status.game_mag = ro.show_mag; pub_status.game_reserve = ro.show_reserve;
    pub_status.reload_state = ro.state;
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
                GripPose g = grip_locked(&hand_in[i], tnow);
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
    *gn = grip_locked(&hand_in[side], now);
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
    static WLaserMesh lzbuf[3];       /* láser: el renderer lee el frame anterior mientras se arma el siguiente (mismo anillo de 3 que vr_hands) */
    static unsigned lzi;
    int rc, copied = 0, have_d = 0;
    float dq[4] = {0, 0, 0, 1}, dp[3] = {0, 0, 0};      /* movimiento rígido de la mano desde la simulación (D), para el láser */
    uint64_t now = mono_ns();
    pthread_mutex_lock(&HM);
    glv_read = -1;
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
        if (have_d) {
            float rp[3], nq[4];
            q_rot(dq, lp, rp);
            for (int i = 0; i < 3; i++) lp[i] = rp[i] + dp[i];
            q_mul(dq, lq, nq); q_norm(nq);
            memcpy(lq, nq, sizeof lq);
        }
        WPose lz = {{lp[0], lp[1], lp[2]}, {lq[0], lq[1], lq[2], lq[3]}};
        WLaserMesh *lm = &lzbuf[lzi % 3];
        if (wlaser_build(&lz, WLASER_MAX_M, lm)) {
            lzi++;
            int n0 = rc ? out->nmeshes : 0;
            if (!rc) {                       /* sin arma publicada ni guante: iluminación por defecto de vr/hands */
                VrHandsConfig hc;
                vrhands_default_config(&hc);
                memset(out, 0, sizeof *out);
                memcpy(out->light_dir, hc.light_dir, sizeof out->light_dir); out->ambient = hc.ambient; out->diffuse = hc.diffuse;
            } else if (out->meshes != buf) memcpy(buf, out->meshes, (size_t)n0 * sizeof buf[0]);
            wlaser_mesh(lm, &buf[n0]);
            out->meshes = buf; out->nmeshes = n0 + 1;
            rc = 1;
        }
    }
    pthread_mutex_unlock(&HM);
    return rc;
}
