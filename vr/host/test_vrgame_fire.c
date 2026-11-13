/* Disparo VR de punta a punta sobre vrgame_pad con RAM y puente sintéticos (sin datos del juego ni runtime): pistola del registro colgada de la mano por el zócalo, cámara y cabeza
 * sustitutas. Comprueba: gatillo > 0,5 -> 1 disparo y cuadrado pulsado; rearme al soltar; sin arma equipada no pasa nada; cargador del juego sin reserva dispara igual; la boca sale delante de la mano.
 * make -C vr/host test */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "bridge.h"
#include "bridge_addrs.h"
#include "host.h"
#include "vr_camera.h"
#include "vr_aa.h"
#include "vr_body.h"
#include "vr_fire.h"
#include "vr_hands.h"
#include "vr_hit.h"
#include "vr_missile.h"
#include "vr_hzd.h"
#include "vr_pose.h"
#include "vr_reload.h"
#include "vr_grip.h"
/* Contrato (cambiado con las poses por arma de vr_hand_grips.inc / vr_grip_fit.inc, generadas por vr/hands/gripfit_gen.c): la mano de apoyo de la pistola usa la pose propia del arma
 * (vrgrip_find(0)->support_pose = HP_W_SOCOM_L, en copa bajo la mano que empuña) y su zócalo FIT_SOCOM_SUP deja su centro de agarre a una distancia FIJA de WP_SUPPORT (sup_dist: sale de la
 * tabla, 16,4 mm con la mano a tamaño real y la SOCOM a escala real; 25,8 mm antes), no encima. */
#define SUPPOSE (vrgrip_find(0)->support_pose)
#define SUP_D sup_dist()
#include "vrgame.h"
#include "w_guided.h"
#include "w_laser.h"
#include "wload.h"
#include "w_sniper.h"
#include "w_throw.h"
#include "wcore.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO l.%d: %s ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- sustitutos de lo que vrgame enlaza del anfitrión ---- */
static VrCamRecord rec;
static VrCamConfig ccfg;
static int g_cam_active = 1;
int vrcam_active(void) { return g_cam_active; }
const VrCamConfig *vrcam_config(void) { return &ccfg; }
const VrCamRecord *vrcam_last_record(void) { return &rec; }
int vrcam_pad_origin(void) { return 0; }
void vrcam_set_zoom_cap(int cap) { (void)cap; }
float vrcam_view_yaw_deg(void) { return 0; }
int vrcam_zoom_capped(void) { return 0; }
int cd_current_disc(void) { return 0; }
int cd_current_source(char *p, size_t n, int *d) { (void)p; (void)n; (void)d; return -1; }
uint8_t *disc_load_stage_archive(const char *p, int d, size_t *n) { (void)p; (void)d; (void)n; return NULL; }
static float g_head[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1.6f, 0, 1};      /* pose de la cabeza (columnas X, Y, Z = -delante, traslación); la prueba del visor del lanzador la mueve */
int mv_vr_get_head(float m[16], double *t) { memcpy(m, g_head, sizeof g_head); if (t) *t = 0; return 1; }
int mv_vrhost_hands_active(void) { return 0; }
float mv_vrhost_wall_distance(const float p[3], float mx) { (void)p; (void)mx; return -1; }
static float ray_stub_mm = -1;      /* geometría sólida de vr_headcol sustituida: impacto a esta distancia (mm del motor), -1 ninguno */
#define RAY_LOG_MAX 512
static float ray_log_o[RAY_LOG_MAX][3], ray_log_d[RAY_LOG_MAX][3];      /* origen y dirección (mundo del motor) de cada rayo pedido desde ray_log_n = 0 */
static int ray_log_n;
static float ray_floor_u = -1e9f;      /* suelo plano en espacio de usuario (m): sin impacto fijo (ray_stub_mm), los rayos hacia abajo impactan en él; -1e9 = no hay geometría */
float mv_vrhost_ray_distance(const float o[3], const float d[3], float mx) {
    if (ray_log_n < RAY_LOG_MAX) { memcpy(ray_log_o[ray_log_n], o, sizeof ray_log_o[0]); memcpy(ray_log_d[ray_log_n], d, sizeof ray_log_d[0]); ray_log_n++; }
    if (ray_stub_mm < 0 && ray_floor_u > -1e8f) {
        VrFireMap map; float uo[3], ud[3];
        vrfire_map_from_cam(&map, &rec); vrfire_pos_from_world(&map, o, uo); vrfire_dir_from_world(&map, d, ud);
        if (ud[1] < -0.5f && uo[1] >= ray_floor_u) { const float t = (uo[1] - ray_floor_u) / -ud[1] * map.units_per_m; return t <= mx ? t : -1; }
        return -1;
    }
    return ray_stub_mm >= 0 && ray_stub_mm <= mx ? ray_stub_mm : -1;
}
int vr_scene_build(void) { return 0; }
void vr_scene_opts_default(void) {}
int vrmelee_rt_installed(void) { return 0; }
void vrmelee_rt_frame(const void *f) { (void)f; }

#define PAD_SQUARE (1u << 15)
#define PAD_R1 (1u << 11)
static uint8_t ram[2 * 1024 * 1024];
static BridgeState bs;
static WeaponState *pist_st;
static const WeaponDef *pist_def;
static int sq_seen;

static void set_hand(float trig, float grip) {
    MvVrHand h;
    memset(&h, 0, sizeof h);
    h.valid = 1; h.pos[0] = 0.2f; h.pos[1] = 1.2f; h.pos[2] = -0.3f; h.rot[3] = 1.0f; h.trigger = trig; h.grip = grip;
    mv_vrgame_set_hand(1, &h);
}
static void vbl(float trig, int n) {
    for (int i = 0; i < n; i++) { vrgame_test_advance(8000000ull); set_hand(trig, 1.0f);   /* reloj inyectado: 8 ms por VBlank de prueba, sin depender de la carga de la máquina */ uint16_t o = vrgame_pad(0, &bs, ram); if (o & PAD_SQUARE) sq_seen++; }
}
static void world(int weapon, int mag, int total) {
    memset(&bs, 0, sizeof bs);
    bs.valid = BRIDGE_OK_ALL_CORE; bs.has_player = 1; bs.in_control = 1;
    bs.weapon = weapon; bs.magazine = mag; bs.magazine_max = 12;
    if (weapon >= 0) bs.weapon_ammo[weapon] = total;
}
static void fresh_pistol(void) {
    static WeaponState *old;
    if (old) { free(old); old = NULL; }
    for (int i = 0; i < w_registry_count; i++) if (w_registry[i] && w_registry[i]->game_weapon_id == 0) pist_def = w_registry[i];
    WeaponState *st = calloc(1, pist_def->state_size);
    st->def = pist_def; pist_def->init(st); st->def = pist_def;
    pist_st = old = st;
    VrHandsConfig hc; vrhands_default_config(&hc); hc.grip_mode = VRH_GRIP_SOCKET;
    VrHandsRig *rig = vrhands_create(&hc);
    vrhands_bind(rig, st, NULL, 0, 0);
    vrgame_test_install(0, (void *)pist_def, st, rig);
    vrgame_reset();
    vrgame_set_enabled(0, 1);
    sq_seen = 0;
}

static void scenario(const char *name, int mag, int total) {
    printf("-- %s --\n", name);
    world(0, mag, total);
    fresh_pistol();
    vbl(0.0f, 5);
    MvVrGameStatus st0, st;
    mv_vrgame_status(&st0);
    CHECK(st0.active && st0.weapon == 0, "%s: arma VR mostrada", name);
    printf("   estado: cargador VR %d recamara %d insertado %d\n", st0.mag_rounds, st0.chambered, st0.mag_inserted);
    uint32_t s0 = st0.shots;
    vbl(0.3f, 3);
    mv_vrgame_status(&st);
    CHECK(st.shots == s0 && sq_seen == 0, "%s: 0,3 de gatillo no dispara", name);
    vbl(0.6f, 3);                                                   /* > 0,5: dispara */
    mv_vrgame_status(&st);
    CHECK(st.shots == s0 + 1, "%s: gatillo 0,6 = 1 disparo (%u)", name, st.shots - s0);
    CHECK(sq_seen > 0, "%s: el cuadrado del juego se pulsa (%d VBlanks)", name, sq_seen);
    VrFireStats fs; vrfire_stats(&fs);
    CHECK(fs.requests == 1 && fs.presses >= 1, "%s: petición de disparo (%u) y pulsación (%u)", name, fs.requests, fs.presses);
    VrFireMat m;
    int dec = vrfire_take_projectile(0, &m);                        /* lo que sustituiría el juego al crear la bala */
    CHECK(dec == VRFIRE_SUBST, "%s: la bala se sustituye por la boca del arma VR", name);
    if (dec == VRFIRE_SUBST) {
        VrFireMap map; vrfire_map_from_cam(&map, &rec);
        float f[3], w[3], u[3];
        vrfire_mat_forward(&m, f);
        for (int i = 0; i < 3; i++) w[i] = (float)m.t[i] - 400.0f * f[i];     /* boca = t - 400 * delante */
        vrfire_pos_from_world(&map, w, u);
        float dx = u[0] - 0.2f, dy = u[1] - 1.2f, dz = u[2] + 0.3f;
        CHECK(sqrtf(dx * dx + dy * dy + dz * dz) < 0.6f && dz < 0.0f, "%s: boca a %.2f %.2f %.2f de la mano (delante)", name, dx, dy, dz);
    }
    vbl(0.6f, 20);
    mv_vrgame_status(&st);
    CHECK(st.shots == s0 + 1, "%s: sin soltar el gatillo no repite (%u)", name, st.shots - s0);
    vbl(0.1f, 20); vbl(0.7f, 3);
    mv_vrgame_status(&st);
    CHECK(st.shots == s0 + 2, "%s: soltar y apretar otra vez = 2.º disparo (%u)", name, st.shots - s0);
}


/* ---- granada por gesto (id 2) y adhesivo (id 6) de punta a punta sobre vrgame_pad ---- */
#define PAD_CIRCLE (1u << 13)
static WeaponState *inst_state;
static void fresh_weapon(int gid) {
    static WeaponState *old;
    const WeaponDef *def = NULL;
    if (old) { free(old); old = NULL; }
    for (int i = 0; i < w_registry_count; i++) if (w_registry[i] && w_registry[i]->game_weapon_id == gid) def = w_registry[i];
    WeaponState *st = calloc(1, def->state_size);
    st->def = def; def->init(st); st->def = def;
    inst_state = old = st;
    VrHandsConfig hc; vrhands_default_config(&hc); hc.grip_mode = VRH_GRIP_SOCKET;
    VrHandsRig *rig = vrhands_create(&hc);
    vrhands_bind(rig, st, NULL, 0, 0);
    vrgame_test_install(gid, (void *)def, st, rig);
    vrgame_reset();
    vrgame_set_enabled(0, 1);
    sq_seen = 0;
}
/* un VBlank con la mano derecha en (x,y,z), grip y gatillo dados; devuelve el mando que ve el juego */
static uint16_t gvbl(float x, float grip, float trig, uint16_t pad) {
    MvVrHand h;
    memset(&h, 0, sizeof h);
    h.valid = 1; h.pos[0] = x; h.pos[1] = 1.2f; h.pos[2] = -0.3f; h.rot[3] = 1.0f; h.trigger = trig; h.grip = grip;
    mv_vrgame_set_hand(1, &h);
    vrgame_test_advance(8000000ull);
    uint16_t o = vrgame_pad(pad, &bs, ram);
    if (o & PAD_SQUARE) sq_seen++;
    return o;
}
/* lanzamiento completo con el gatillo: la mano avanza `step` m por VBlank con el gatillo apretado y lo suelta (1 -> 0,6 -> 0,1); devuelve la velocidad horizontal por tick que llega al motor (0 si no hubo datos) */
static int throw_with_trigger(float step, int16_t vel[3]) {
    float x = 0.2f;
    for (int i = 0; i < 10; i++) gvbl(x, 0.0f, 1.0f, 0);
    for (int i = 0; i < 10; i++) { x += step; gvbl(x, 0.0f, 1.0f, 0); }
    x += step; gvbl(x, 0.0f, 0.6f, 0);
    x += step; gvbl(x, 0.0f, 0.1f, 0);
    for (int i = 0; i < 4; i++) gvbl(x, 0.0f, 0.0f, 0);
    int16_t pos[3];
    return vrfire_take_throw(pos, vel) ? (int)lroundf(sqrtf((float)vel[0] * (float)vel[0] + (float)vel[2] * (float)vel[2])) : -1;
}
static void grenade_gesture(void) {
    printf("-- granada: gatillo apretado = preparada, soltarlo = lanzar --\n");
    world(2, 0, 3);
    fresh_weapon(2);
    for (int i = 0; i < 6; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    MvVrGameStatus st; mv_vrgame_status(&st);
    CHECK(st.active && st.weapon == 2, "granada VR mostrada en la mano");
    CHECK(sq_seen == 0 && st.throws == 0, "mano abierta: ni botón del juego ni lanzamiento");
    for (int i = 0; i < 10; i++) gvbl(0.2f, 1.0f, 0.0f, 0);
    for (int i = 0; i < 4; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    mv_vrgame_status(&st);
    CHECK(inst_state->phase == WT_PH_HELD && sq_seen == 0 && st.throws == 0, "el agarre (cerrar y abrir la mano) no prepara ni lanza (fase %d, cuadrado %d)", inst_state->phase, sq_seen);
    for (int i = 0; i < 10; i++) gvbl(0.2f, 0.0f, 1.0f, 0);
    CHECK(inst_state->phase == WT_PH_READY, "gatillo apretado: preparada (fase %d)", inst_state->phase);
    CHECK(sq_seen > 5, "el botón de ataque del juego se mantiene mientras está preparada (%d VBlanks)", sq_seen);
    for (int i = 0; i < 10; i++) gvbl(0.2f + 0.045f * (float)i, 0.0f, 1.0f, 0);     /* el brazo avanza ~5 m/s con el gatillo apretado */
    mv_vrgame_status(&st);
    CHECK(st.throws == 0 && inst_state->phase == WT_PH_READY, "mientras se mantiene el gatillo no se lanza nada (%u lanzamientos)", st.throws);
    gvbl(0.2f + 0.045f * 10, 0.0f, 0.6f, 0); gvbl(0.2f + 0.045f * 11, 0.0f, 0.1f, 0);
    for (int i = 0; i < 4; i++) gvbl(0.2f + 0.045f * 11, 0.0f, 0.0f, 0);
    mv_vrgame_status(&st);
    CHECK(st.throws == 1 && st.rejects == 0, "soltar el gatillo: un lanzamiento aceptado (%u, rechazos %u)", st.throws, st.rejects);
    int16_t pos[3], vel[3];
    CHECK(vrfire_take_throw(pos, vel) == 1, "datos de lanzamiento listos para el juego");
    printf("   velocidad por tick %d %d %d (posición %d %d %d)\n", vel[0], vel[1], vel[2], pos[0], pos[1], pos[2]);
    CHECK(abs(vel[0]) + abs(vel[1]) + abs(vel[2]) > 40, "la velocidad de la mano llega al motor");
    for (int i = 0; i < 40; i++) gvbl(0.7f, 0.0f, 0.0f, 0);
    CHECK(inst_state->phase != WT_PH_READY, "tras lanzar, la mano ya no está preparada (fase %d)", inst_state->phase);

    printf("-- granada: un soltar flojo (< 1 m/s) cae cerca; cualquier gesto de lanzar sale al maximo del juego (4,93 m/s, 20 grados de elevacion minima), de punta a punta --\n");
    int16_t v_soft[3], v_mid[3], v_hard[3];
    for (int i = 0; i < 60; i++) gvbl(0.2f, 0.0f, 0.0f, 0);                       /* sale la siguiente */
    int h_soft = throw_with_trigger(0.0045f, v_soft);
    for (int i = 0; i < 60; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    int h_mid = throw_with_trigger(0.030f, v_mid);
    for (int i = 0; i < 60; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    int h_hard = throw_with_trigger(0.080f, v_hard);
    printf("   velocidad horizontal por tick: suave %d, media %d, fuerte %d\n", h_soft, h_mid, h_hard);
    CHECK(h_soft >= 0 && h_mid >= 0 && h_hard >= 0, "tres lanzamientos con datos");
    /* 4,93 m/s con 20 grados de elevación (el gesto es horizontal): 4,93 * cos(20) * 1000 u/m * 0,04 s = 185 u/tick en horizontal y 67 en vertical */
    CHECK(abs(h_mid - 185) <= 4 && abs(h_hard - 185) <= 4, "gesto medio (1,5 m/s) y fuerte (4 m/s): los dos salen al maximo del juego (%d y %d u/tick, esperado 185)", h_mid, h_hard);
    CHECK(h_soft < 60, "soltar flojo (0,2 m/s): cae a los pies, no es un lanzamiento (%d u/tick)", h_soft);
    CHECK(abs(v_mid[1] - v_hard[1]) <= 2 && v_mid[1] > 0, "misma elevación mínima en los dos (vertical %d y %d u/tick: 67 de lanzamiento menos la gravedad del juego del retardo de la animación)", v_mid[1], v_hard[1]);

    printf("-- granada lista y arma fuera: no se lanza con la velocidad del juego --\n");
    world(2, 0, 3);
    fresh_weapon(2);
    for (int i = 0; i < 6; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    for (int i = 0; i < 10; i++) gvbl(0.2f, 0.0f, 1.0f, 0);
    CHECK(inst_state->phase == WT_PH_READY && sq_seen > 0, "granada preparada con el botón del juego mantenido (fase %d)", inst_state->phase);
    int16_t cpos[3], cvel[3];
    vrfire_take_throw(cpos, cvel);                                   /* vacía lo que dejó el lanzamiento anterior */
    world(-1, 0, 0);                                                 /* el arma se desequipa con el gatillo aún apretado */
    for (int i = 0; i < 6; i++) gvbl(0.2f, 0.0f, 1.0f, 0);
    CHECK(vrfire_take_throw(cpos, cvel) == 1 && cvel[0] == 0 && cvel[1] == 0 && cvel[2] == 0,
          "la suelta que haría el juego se sustituye por una caída suave (vel %d %d %d)", cvel[0], cvel[1], cvel[2]);

    printf("-- adhesivo: el círculo del juego (detonar) no se retira --\n");
    world(6, 0, 4);
    fresh_weapon(6);
    for (int i = 0; i < 6; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    uint16_t o = gvbl(0.2f, 0.0f, 0.0f, PAD_CIRCLE);
    CHECK((o & PAD_CIRCLE) != 0, "sin mando remoto virtual: el botón del juego pasa tal cual");
    gvbl(0.2f, 0.0f, 0.0f, 0);
    /* colocar = soltar el gatillo (tras apretarlo a fondo) con la superficie al alcance */
    for (int i = 0; i < 6; i++) gvbl(0.2f, 0.0f, 1.0f, 0);
    CHECK(inst_state->phase == WPL_PH_HELD, "apretar el gatillo no coloca (fase %d)", inst_state->phase);
    mv_vrgame_status(&st); uint32_t pl0 = st.places + st.rejects;
    for (int i = 0; i < 4; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    mv_vrgame_status(&st);
    CHECK(st.places + st.rejects > pl0, "soltar el gatillo emite la colocación (colocadas %u, rechazos %u)", st.places, st.rejects);
}

/* ---- C4: se coloca en el punto del rayo de la mano (pared, suelo), con su cara adhesiva apoyada, y se rechaza fuera de alcance ---- */
static float c4_yaw_deg;      /* guiñada de la mano (alrededor de Y, antes de la inclinación); 0 salvo en la prueba de orientación de la mina */
static uint16_t c4vbl(float pitch_deg, float trig) {      /* mano derecha en (0,2; 1,2; -0,3) inclinada `pitch_deg` hacia abajo (rotación en X) */
    MvVrHand h;
    memset(&h, 0, sizeof h);
    float a = -pitch_deg * 0.017453292f * 0.5f;
    h.valid = 1; h.pos[0] = 0.2f; h.pos[1] = 1.2f; h.pos[2] = -0.3f; h.rot[0] = sinf(a); h.rot[3] = cosf(a); h.trigger = trig; h.grip = 0.0f;
    if (c4_yaw_deg != 0.0f) {      /* q = Ry(yaw) * Rx(a) */
        const float ya = c4_yaw_deg * 0.017453292f * 0.5f, sy = sinf(ya), cy = cosf(ya), sx = sinf(a), cx = cosf(a);
        h.rot[0] = cy * sx; h.rot[1] = sy * cx; h.rot[2] = -sy * sx; h.rot[3] = cy * cx;
    }
    mv_vrgame_set_hand(1, &h);
    vrgame_test_advance(8000000ull);
    uint16_t o = vrgame_pad(0, &bs, ram);
    if (o & PAD_SQUARE) sq_seen++;
    return o;
}
/* aprieta a fondo, suelta (= colocar) y devuelve 1 si el hook aceptó; `hit` = distancia del impacto simulado en mm (-1 ninguno) */
static float c4_f[3];      /* delante de la carga en el último c4_try aceptado (espacio de usuario) */
static int c4_direct_last;     /* 1: el último c4_try aceptado salió por la colocación directa (sin pulsar el ataque del juego); 0: por el juego (blanco C4 adherido) */
static int c4_try(float pitch_deg, float hit_mm, VrFireMat *m, float *exp_p, float *dist_m) {
    ray_stub_mm = hit_mm;
    { VrFireMat dm; for (int n = 0; n < 8 && vrfire_take_projectile(2, &dm) == VRFIRE_SUBST; n++) {} while (vrfire_c4_take(&dm)) {} }
    fresh_weapon(6);
    for (int i = 0; i < 6; i++) c4vbl(pitch_deg, 0.0f);
    for (int i = 0; i < 6; i++) c4vbl(pitch_deg, 1.0f);
    MvVrGameStatus st0, st; mv_vrgame_status(&st0);
    for (int i = 0; i < 4; i++) c4vbl(pitch_deg, 0.0f);
    mv_vrgame_status(&st);
    if (st.places == st0.places) return 0;
    const WLaunch *ev = w_launch_last();
    const float qx = ev->pose.q.x, qy = ev->pose.q.y, qz = ev->pose.q.z, qw = ev->pose.q.w;
    const float f[3] = {-2 * (qx * qz + qw * qy), -2 * (qy * qz - qw * qx), -(1 - 2 * (qx * qx + qy * qy))};
    const float d = hit_mm / 1000.0f;
    memcpy(c4_f, f, sizeof f);
    exp_p[0] = ev->pose.p.x + f[0] * d; exp_p[1] = ev->pose.p.y + f[1] * d; exp_p[2] = ev->pose.p.z + f[2] * d;
    *dist_m = d;
    if (vrfire_c4_take(m)) { c4_direct_last = 1; return 1; }
    c4_direct_last = 0;
    return vrfire_take_projectile(2, m) == VRFIRE_SUBST;
}
static void hzd_wall(const float o[3], const float d[3], float dist);
static void w16r(uint32_t a, int16_t v);
/* C4 con superficie a 0,9 m detrás del soldado: la carga la acepta el hook, sale por la colocación directa (nunca el ataque del juego, que la pegaría al soldado) y queda 7,9 cm sobre el punto del rayo */
static int c4_surface_not_soldier(const VrFireMap *map) {
    VrFireMat m; float e[3], d, u[3];
    MvVrGameStatus s0, s1; mv_vrgame_status(&s0);
    const int ok = c4_try(-55.0f, 900.0f, &m, e, &d);      /* c4_try pone sq_seen a 0 (fresh_weapon) */
    mv_vrgame_status(&s1);
    if (!ok || s1.places != s0.places + 1 || c4_direct_last != 1 || sq_seen != 0) return 0;
    vrfire_pos_from_world(map, (float[3]){(float)m.t[0], (float)m.t[1], (float)m.t[2]}, u);
    const float dd = sqrtf((u[0] - e[0]) * (u[0] - e[0]) + (u[1] - e[1]) * (u[1] - e[1]) + (u[2] - e[2]) * (u[2] - e[2]));
    return fabsf(dd - 0.079f) < 0.02f;
}
static void c4_ray_placement(void) {
    printf("-- C4: colocación por rayo --\n");
    world(6, 0, 4);
    VrFireMap map; vrfire_map_from_cam(&map, &rec);
    VrFireMat m; float e[3], d, u[3], cz[3], nz[3];
    /* pared a 0,5 m delante (mano inclinada 55 grados hacia arriba: el delante de la carga, con el agarre MAIN, queda ~horizontal): la carga queda en el punto del impacto, 7,9 cm hacia fuera (sin pared HZD en la RAM de
     * pruebas la normal es la opuesta al rayo), con el eje Z del modelo (c4_bomb) saliendo de la pared */
    CHECK(c4_try(-55.0f, 500.0f, &m, e, &d) == 1, "pared a 0,5 m: aceptada");
    CHECK(c4_direct_last == 1 && sq_seen == 0, "pared: colocación directa, el ataque del juego (animación de Snake) no se pulsa nunca (cuadrados %d)", sq_seen);
    float hl = sqrtf(c4_f[0] * c4_f[0] + c4_f[2] * c4_f[2]), wn[3] = {-c4_f[0] / hl, 0.0f, -c4_f[2] / hl};
    CHECK(fabsf(c4_f[1]) < 0.4f, "delante de la carga ~horizontal (%.2f)", c4_f[1]);
    vrfire_pos_from_world(&map, (float[3]){(float)m.t[0], (float)m.t[1], (float)m.t[2]}, u);
    CHECK(fabsf(u[0] - (e[0] + 0.079f * wn[0])) < 0.01f && fabsf(u[1] - e[1]) < 0.01f && fabsf(u[2] - (e[2] + 0.079f * wn[2])) < 0.01f, "pared: punto (%.3f %.3f %.3f) frente al del rayo (%.3f %.3f %.3f)+7,9 cm hacia fuera", u[0], u[1], u[2], e[0], e[1], e[2]);
    for (int i = 0; i < 3; i++) cz[i] = (float)m.m[3 * i + 2] / 4096.0f;
    vrfire_dir_from_world(&map, cz, nz);
    CHECK(nz[0] * wn[0] + nz[2] * wn[2] > 0.95f && fabsf(nz[1]) < 0.1f, "pared: eje Z del modelo (%.2f %.2f %.2f) sale de la pared", nz[0], nz[1], nz[2]);
    /* mano sin inclinar: el delante de la carga apunta ~55 grados hacia abajo: suelo/cara superior, Z del modelo hacia arriba */
    CHECK(c4_try(0.0f, 600.0f, &m, e, &d) == 1, "suelo a 0,6 m: aceptado");
    CHECK(c4_direct_last == 1 && sq_seen == 0, "suelo: colocación directa, sin pulsar el ataque del juego (cuadrados %d)", sq_seen);
    vrfire_pos_from_world(&map, (float[3]){(float)m.t[0], (float)m.t[1], (float)m.t[2]}, u);
    CHECK(fabsf(u[0] - e[0]) < 0.01f && fabsf(u[1] - (e[1] + 0.079f)) < 0.01f && fabsf(u[2] - e[2]) < 0.01f, "suelo: punto (%.3f %.3f %.3f) frente al del rayo (%.3f %.3f %.3f)+7,9 cm arriba", u[0], u[1], u[2], e[0], e[1], e[2]);
    for (int i = 0; i < 3; i++) cz[i] = (float)m.m[3 * i + 2] / 4096.0f;
    vrfire_dir_from_world(&map, cz, nz);
    CHECK(nz[1] > 0.95f, "suelo: eje Z del modelo (%.2f %.2f %.2f) hacia arriba", nz[0], nz[1], nz[2]);
    /* el plano del suelo a la altura de los pies no gana a una colisión más lejana (pasado un borde / escalera abajo): la carga va a la colisión, no flotando en el plano */
    bs.ctl_pos.y = -2500;                                      /* suelo del juego a 0,9 m bajo el ojo del motor: el plano de los pies queda a ~0,6 m por delante en el rayo */
    CHECK(c4_try(0.0f, 600.0f, &m, e, &d) == 1, "plano de los pies confirmado por la colisión a 0,6 m: aceptado");
    CHECK(c4_try(0.0f, 900.0f, &m, e, &d) == 1, "colisión a 0,9 m más lejos que el plano de los pies: aceptada");
    vrfire_pos_from_world(&map, (float[3]){(float)m.t[0], (float)m.t[1], (float)m.t[2]}, u);
    CHECK(fabsf(u[0] - e[0]) < 0.01f && fabsf(u[1] - (e[1] + 0.079f)) < 0.01f && fabsf(u[2] - e[2]) < 0.01f, "borde: punto (%.3f %.3f %.3f) en la colisión del rayo (%.3f %.3f %.3f), no en el plano de los pies", u[0], u[1], u[2], e[0], e[1], e[2]);
    bs.ctl_pos.y = 0;
    /* fuera de alcance: sin impacto en 1 m, o el impacto más allá del alcance de brazo */
    MvVrGameStatus s0, s1; mv_vrgame_status(&s0);
    CHECK(c4_try(-55.0f, -1.0f, &m, e, &d) == 0, "sin superficie al alcance: rechazada");
    CHECK(c4_try(-55.0f, 1500.0f, &m, e, &d) == 0, "pared a 1,5 m (fuera del brazo): rechazada");
    mv_vrgame_status(&s1);
    CHECK(s1.places == s0.places && s1.rejects >= s0.rejects + 2, "dos rechazos y ninguna colocación (colocadas %u, rechazos %u)", s1.places - s0.places, s1.rejects - s0.rejects);
    /* soldado (blanco C4 del juego) EN el rayo: el juego (sna_8004FDE8 / NewBakudan attached=1) pega la carga a su cuerpo y le sigue; la pose VR no vale. Si el rayo no lo toca, o el juego no lo
     * tomaría (fuera de su caja de sonda, ya marcado), NO se intenta pegar: con la pose de la mano la carga quedaba flotando en el aire. El rayo se reconstruye del último evento aceptado. */
    CHECK(c4_try(-55.0f, 500.0f, &m, e, &d) == 1, "pared a 0,5 m (rayo de referencia para el soldado): aceptada");
    const WLaunch *rev = w_launch_last();
    float rf[3], rq = sqrtf(rev->pose.q.x * rev->pose.q.x + rev->pose.q.y * rev->pose.q.y + rev->pose.q.z * rev->pose.q.z + rev->pose.q.w * rev->pose.q.w), ro[3] = {rev->pose.p.x, rev->pose.p.y, rev->pose.p.z}, rw[3], rd[3];
    { const float qx = rev->pose.q.x / rq, qy = rev->pose.q.y / rq, qz = rev->pose.q.z / rq, qw = rev->pose.q.w / rq;
      rf[0] = -2 * (qx * qz + qw * qy); rf[1] = -2 * (qy * qz - qw * qx); rf[2] = -(1 - 2 * (qx * qx + qy * qy)); }
    vrfire_pos_to_world(&map, ro, rw); vrfire_dir_to_world(&map, rf, rd);
    const float upm = map.units_per_m, sold_c[3] = {rw[0] + rd[0] * 0.4f * upm, rw[1] + rd[1] * 0.4f * upm, rw[2] + rd[2] * 0.4f * upm};
    const int32_t ctl0[3] = {bs.ctl_pos.x, bs.ctl_pos.y, bs.ctl_pos.z};
    bs.valid |= BRIDGE_OK_TARGETS; bs.n_targets = 1;
    bs.targets[0].cls = 64; bs.targets[0].side = 2; bs.targets[0].damaged = 0;
    bs.player_map = 1; bs.targets[0].map = 1;                   /* el puente da player_map (BRIDGE_OK_PLAYER_ST): el juego exige player_map & map */
    bs.targets[0].half_size.x = 200; bs.targets[0].half_size.y = 2500; bs.targets[0].half_size.z = 200;   /* y alto: el rayo de la prueba va ~2 m bajo el origen de y de ctl_pos (que no se mueve para no tocar el suelo del juego) */
    #define SET_SOLDIER(cx, cz) do { bs.targets[0].center.x = (int)lroundf(cx); bs.targets[0].center.y = (int)lroundf(sold_c[1]); bs.targets[0].center.z = (int)lroundf(cz); \
        bs.ctl_pos.x = (int)lroundf((cx)); bs.ctl_pos.y = ctl0[1]; bs.ctl_pos.z = (int)lroundf((cz) - 300.0f); } while (0)
    SET_SOLDIER(sold_c[0], sold_c[2]);                           /* el jugador a 300 unidades detrás del soldado, mirando a él (ctl_rot.y = 0): dentro de la caja de sonda del juego */
    CHECK(fabsf((float)bs.targets[0].center.y - (float)bs.ctl_pos.y) <= (float)bs.targets[0].half_size.y + 600.0f, "montaje: el soldado cae en la altura de la caja de sonda (y %d)", (int)bs.targets[0].center.y);
    mv_vrgame_status(&s0);
    c4_try(-55.0f, -1.0f, &m, e, &d);
    mv_vrgame_status(&s1);
    CHECK(s1.places == s0.places + 1, "soldado en el rayo, sin superficie: se deja pasar al juego (colocadas %u)", s1.places - s0.places);
    CHECK(c4_direct_last == 0 && sq_seen > 0, "adherida a un enemigo: la pone el propio juego (necesita su animación y su blanco), no la colocación directa (cuadrados %d)", sq_seen);
    mv_vrgame_status(&s0);
    c4_try(-55.0f, 900.0f, &m, e, &d);
    mv_vrgame_status(&s1);
    CHECK(s1.places == s0.places + 1 && c4_direct_last == 0, "soldado en el rayo con una pared 0,5 m más atrás: va al juego (se pega al soldado), no a la pared de detrás (colocadas %u, directa %d)", s1.places - s0.places, c4_direct_last);
    /* el soldado está en la caja de sonda pero el rayo pasa de largo (0,5 m al lado): no se intenta pegar */
    { const float hl = sqrtf(rd[0] * rd[0] + rd[2] * rd[2]), sx = sold_c[0] + rd[2] / hl * 500.0f, sz = sold_c[2] - rd[0] / hl * 500.0f;
      SET_SOLDIER(sx, sz); }
    mv_vrgame_status(&s0);
    c4_try(-55.0f, -1.0f, &m, e, &d);
    mv_vrgame_status(&s1);
    CHECK(s1.places == s0.places, "rayo sin superficie que no toca al soldado: rechazada, no se deja la carga en el aire");
    SET_SOLDIER(sold_c[0], sold_c[2]);
    bs.targets[0].damaged = 64;                                  /* ya marcado por otra carga: el juego no lo toma */
    mv_vrgame_status(&s0);
    c4_try(-55.0f, -1.0f, &m, e, &d);
    mv_vrgame_status(&s1);
    CHECK(s1.places == s0.places, "blanco C4 ya marcado: rechazada");
    bs.targets[0].damaged = 0;
    SET_SOLDIER(sold_c[0], sold_c[2]); bs.ctl_pos.z -= 3000;     /* el jugador 3000 unidades más atrás: el juego no tomaría al soldado (fuera de su caja de sonda) */
    mv_vrgame_status(&s0);
    c4_try(-55.0f, -1.0f, &m, e, &d);
    mv_vrgame_status(&s1);
    CHECK(s1.places == s0.places, "soldado en el rayo pero fuera de la caja de sonda del juego: rechazada");
    /* el juego no toma al soldado pero hay superficie detrás: NO se rechaza, la carga va a la superficie como un C4 normal (nunca flotando ni pegada al soldado) */
    CHECK(c4_surface_not_soldier(&map), "soldado en el rayo fuera de la caja de sonda con pared detrás: la carga va a la pared (superficie), no al soldado");
    static uint8_t ram_c4[sizeof ram];
    memcpy(ram_c4, ram, sizeof ram);
    /* pared HZD entre Snake (ctl_pos) y el soldado (sna_line_check) que el rayo VR no cruza: el juego descarta al soldado */
    SET_SOLDIER(sold_c[0], sold_c[2]);
    { const float o3[3] = {(float)bs.ctl_pos.x, (float)bs.ctl_pos.y, (float)bs.ctl_pos.z}, c3[3] = {(float)bs.targets[0].center.x, (float)bs.targets[0].center.y, (float)bs.targets[0].center.z};
      const float d3[3] = {c3[0] - o3[0], c3[1] - o3[1], c3[2] - o3[2]};
      hzd_wall(o3, d3, 150.0f);                                  /* monta la cadena HZD; después se estrecha la pared a +-30 uds, atravesada en el medio del segmento */
      const float y0 = (o3[1] < c3[1] ? o3[1] : c3[1]) - 200.0f, h = fabsf(d3[1]) + 400.0f;
      const int16_t wl[8] = {(int16_t)lroundf(o3[0] - 30.0f), (int16_t)lroundf(o3[2] + 150.0f), (int16_t)lroundf(y0), (int16_t)lroundf(h),
                             (int16_t)lroundf(o3[0] + 30.0f), (int16_t)lroundf(o3[2] + 150.0f), (int16_t)lroundf(y0), (int16_t)lroundf(h)};
      for (int k = 0; k < 8; k++) w16r(0x801F0400u + 2u * (uint32_t)k, wl[k]);
      CHECK(vrhzd_raycast(ram, o3, d3, sqrtf(d3[0] * d3[0] + d3[1] * d3[1] + d3[2] * d3[2]), 0u) >= 0.0f, "montaje: hay pared entre Snake y el soldado");
      CHECK(vrhzd_raycast(ram, rw, rd, 1000.0f, 0u) < 0.0f, "montaje: el rayo VR no cruza esa pared"); }
    CHECK(c4_surface_not_soldier(&map), "pared entre Snake y el soldado: no se pega al soldado, va a la superficie de detrás");
    /* SEGMENT_ATR = HZD_SEG_NO_COLLIDE (0x01): sna_line_check ignora las paredes marcadas NO_COLLIDE; otras banderas (p. ej. 4 = línea de visión) siguen bloqueando */
    ram[0x801F0500u & 0x1FFFFFu] = 4;
    CHECK(c4_surface_not_soldier(&map), "pared con bandera 4 (no NO_COLLIDE): sigue bloqueando al soldado");
    ram[0x801F0500u & 0x1FFFFFu] = 1;
    { const float o3[3] = {(float)bs.ctl_pos.x, (float)bs.ctl_pos.y, (float)bs.ctl_pos.z}, d3[3] = {(float)bs.targets[0].center.x - o3[0], (float)bs.targets[0].center.y - o3[1], (float)bs.targets[0].center.z - o3[2]};
      CHECK(vrhzd_raycast(ram, o3, d3, sqrtf(d3[0] * d3[0] + d3[1] * d3[1] + d3[2] * d3[2]), 1u) < 0.0f, "montaje: con la bandera NO_COLLIDE la pared no cuenta para el juego"); }
    mv_vrgame_status(&s0); { c4_try(-55.0f, 900.0f, &m, e, &d); mv_vrgame_status(&s1);
      CHECK(s1.places == s0.places + 1 && c4_direct_last == 0 && sq_seen > 0, "pared NO_COLLIDE entre Snake y el soldado: no lo rechaza, va al juego (directa %d)", c4_direct_last); }
    ram[0x801F0500u & 0x1FFFFFu] = 0;
    ray_stub_mm = -1;
    mv_vrgame_status(&s0); c4_try(-55.0f, -1.0f, &m, e, &d); mv_vrgame_status(&s1);
    CHECK(s1.places == s0.places, "pared entre Snake y el soldado y sin superficie detrás: rechazada (solo se rechaza sin superficie)");
    memcpy(ram, ram_c4, sizeof ram);
    /* sin pared: el juego sí lo toma (control positivo del montaje) */
    mv_vrgame_status(&s0); { c4_try(-55.0f, 900.0f, &m, e, &d); mv_vrgame_status(&s1);
      CHECK(s1.places == s0.places + 1 && c4_direct_last == 0 && sq_seen > 0, "sin pared: va al juego (se pega al soldado)"); }
    /* el juego toma el PRIMER blanco de su lista; el rayo VR toca a otro: no se pega */
    { const float hl = sqrtf(rd[0] * rd[0] + rd[2] * rd[2]), sx = sold_c[0] + rd[2] / hl * 500.0f, sz = sold_c[2] - rd[0] / hl * 500.0f;
      bs.n_targets = 2; bs.targets[1] = bs.targets[0];
      bs.targets[0].center.x = (int)lroundf(sx); bs.targets[0].center.z = (int)lroundf(sz);
      CHECK(fabsf((float)bs.targets[0].center.x - (float)(bs.ctl_pos.x)) <= 600.0f && fabsf((float)bs.targets[0].center.z - ((float)bs.ctl_pos.z + 300.0f)) <= 600.0f, "montaje: el blanco lateral está en la caja de sonda");
      CHECK(c4_surface_not_soldier(&map), "el juego toma el primer blanco y el rayo toca al segundo: no se pega al soldado tocado, va a la superficie");
      bs.targets[0] = bs.targets[1]; bs.n_targets = 1; }
    /* clase 0x20: el juego descarta el blanco y no pega */
    bs.targets[0].cls = 64 | 0x20;
    CHECK(c4_surface_not_soldier(&map), "blanco con clase 0x20: no se pega, va a la superficie");
    bs.targets[0].cls = 64;
    /* MapConflict: el mapa del blanco no comparte bits con el del jugador */
    bs.targets[0].map = 2;
    CHECK(c4_surface_not_soldier(&map), "blanco en otro mapa: no se pega, va a la superficie");
    bs.targets[0].map = 3;
    mv_vrgame_status(&s0); { c4_try(-55.0f, 900.0f, &m, e, &d); mv_vrgame_status(&s1);
      CHECK(s1.places == s0.places + 1 && c4_direct_last == 0 && sq_seen > 0, "blanco en el mapa del jugador: va al juego"); }
    /* con BRIDGE_OK_PLAYER_ST válido, player_map = 0 es un valor real del juego: player_map & map == 0, MapConflict (el caso sin ST no es alcanzable por vrgame_pad: pide ALL_CORE) */
    bs.player_map = 0; bs.targets[0].map = 2;
    CHECK(c4_surface_not_soldier(&map), "player_map 0 con el puente válido: MapConflict, va a la superficie");
    /* bando: side & 1 != 0 no es un blanco del C4 (aunque el resto cumpla) */
    bs.player_map = 1; bs.targets[0].map = 1; bs.targets[0].side = 1;
    mv_vrgame_status(&s0); c4_try(-55.0f, -1.0f, &m, e, &d); mv_vrgame_status(&s1);
    CHECK(s1.places == s0.places, "blanco con side&1: no es de otro bando, el juego no lo toma, sin superficie: rechazada");
    CHECK(c4_surface_not_soldier(&map), "blanco con side&1 en el rayo: no se pega, va a la superficie");
    bs.targets[0].side = 2;
    /* soldado a 0,9 m en el rayo: una pared MÁS CERCA que el soldado gana (la carga va a ella), una pared pegada a su entrada no */
    SET_SOLDIER(rw[0] + rd[0] * 0.9f * upm, rw[2] + rd[2] * 0.9f * upm);
    mv_vrgame_status(&s0); { c4_try(-55.0f, 300.0f, &m, e, &d); mv_vrgame_status(&s1);
      CHECK(s1.places == s0.places + 1 && c4_direct_last == 1 && sq_seen == 0, "pared a 0,3 m ANTES del soldado (0,9 m): gana la pared, colocación directa (directa %d, cuadrados %d)", c4_direct_last, sq_seen); }
    mv_vrgame_status(&s0); { c4_try(-55.0f, 600.0f, &m, e, &d); mv_vrgame_status(&s1);
      CHECK(s1.places == s0.places + 1 && c4_direct_last == 0 && sq_seen > 0, "pared a 0,6 m junto a la entrada del soldado: va al juego (directa %d)", c4_direct_last); }
    SET_SOLDIER(sold_c[0], sold_c[2]);
    ray_stub_mm = -1;
    #undef SET_SOLDIER
    bs.ctl_pos.x = ctl0[0]; bs.ctl_pos.y = ctl0[1]; bs.ctl_pos.z = ctl0[2];
    bs.n_targets = 0;
    ray_stub_mm = -1;
}

/* ---- C4: detona el agarre del mando DERECHO (no el círculo/CQC) ---- */
static void c4_set_placed(int n) { int32_t v = n; memcpy(ram + (0x800A0604u & 0x1FFFFFu), &v, 4); }
static void c4_remote_grip(void) {
    printf("-- C4: detonación con el agarre derecho --\n");
    world(6, 0, 4);                                              /* C4 equipado (id de juego 6) */
    vrgame_reset(); vrgame_set_enabled(0, 1);
    int circ = 0;
    c4_set_placed(1);
    for (int i = 0; i < 6; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    for (int i = 0; i < 8; i++) circ += (gvbl(0.2f, 0.0f, 0.0f, PAD_CIRCLE) & PAD_CIRCLE) != 0;
    CHECK(circ == 0, "con cargas puestas el círculo físico (CQC) ya no detona (%d VBlanks con círculo)", circ);
    circ = 0;
    for (int i = 0; i < 12; i++) circ += (gvbl(0.2f, 1.0f, 0.0f, 0) & PAD_CIRCLE) != 0;
    CHECK(circ >= 2 && circ <= 6, "apretar el agarre derecho pulsa el círculo del juego unos VBlanks (%d)", circ);
    for (int i = 0; i < 6; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    circ = 0;
    for (int i = 0; i < 12; i++) circ += (gvbl(0.2f, 1.0f, 0.0f, 0) & PAD_CIRCLE) != 0;
    CHECK(circ >= 2, "tras soltar y volver a apretar vuelve a detonar (%d)", circ);
    /* mantener el agarre no repite; si estaba apretado desde antes, tampoco detona (hay que soltarlo antes) */
    circ = 0;
    for (int i = 0; i < 30; i++) circ += (gvbl(0.2f, 1.0f, 0.0f, 0) & PAD_CIRCLE) != 0;
    CHECK(circ == 0, "agarre mantenido: sin repetir (%d)", circ);
    vrgame_reset(); vrgame_set_enabled(0, 1);
    circ = 0;
    for (int i = 0; i < 12; i++) circ += (gvbl(0.2f, 1.0f, 0.0f, 0) & PAD_CIRCLE) != 0;
    CHECK(circ == 0, "agarre apretado desde el principio: no detona sin haberlo soltado (%d)", circ);
    /* golpe/agarre del cuerpo en ese VBlank: el agarre derecho no detona (aunque sea el flanco) */
    vrgame_reset(); vrgame_set_enabled(0, 1);
    for (int i = 0; i < 6; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    vrbodyg_test_force_busy(1);
    circ = 0;
    for (int i = 0; i < 12; i++) circ += (gvbl(0.2f, 1.0f, 0.0f, 0) & PAD_CIRCLE) != 0;
    vrbodyg_test_force_busy(0);
    for (int i = 0; i < 12; i++) circ += (gvbl(0.2f, 1.0f, 0.0f, 0) & PAD_CIRCLE) != 0;
    CHECK(circ == 0, "en golpe/agarre: el agarre derecho no detona, ni al acabar con el agarre mantenido (%d)", circ);
    for (int i = 0; i < 6; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    circ = 0;
    for (int i = 0; i < 12; i++) circ += (gvbl(0.2f, 1.0f, 0.0f, 0) & PAD_CIRCLE) != 0;
    CHECK(circ >= 2, "tras soltar y sin golpe vuelve a detonar (%d)", circ);
    /* arma distinta del C4: ni el agarre detona ni se retira el círculo del usuario (CQC) aunque haya cargas */
    world(0, 12, 48);
    for (int i = 0; i < 6; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    circ = 0;
    for (int i = 0; i < 12; i++) circ += (gvbl(0.2f, 1.0f, 0.0f, 0) & PAD_CIRCLE) != 0;
    CHECK(circ == 0, "otra arma: el agarre derecho no detona (%d)", circ);
    for (int i = 0; i < 6; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    CHECK((gvbl(0.2f, 0.0f, 0.0f, PAD_CIRCLE) & PAD_CIRCLE) != 0, "otra arma: el círculo del CQC pasa al juego");
    world(6, 0, 4);
    /* sin cargas colocadas: el círculo pasa (CQC) y el agarre no pulsa nada */
    c4_set_placed(0);
    for (int i = 0; i < 6; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    CHECK((gvbl(0.2f, 0.0f, 0.0f, PAD_CIRCLE) & PAD_CIRCLE) != 0, "sin cargas: el círculo del CQC pasa al juego");
    circ = 0;
    for (int i = 0; i < 12; i++) circ += (gvbl(0.2f, i < 6 ? 0.0f : 1.0f, 0.0f, 0) & PAD_CIRCLE) != 0;
    CHECK(circ == 0, "sin cargas: el agarre no pulsa el círculo (%d)", circ);
    /* sin el control del jugador no detona */
    c4_set_placed(1);
    for (int i = 0; i < 6; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    bs.in_control = 0;
    circ = 0;
    for (int i = 0; i < 12; i++) circ += (gvbl(0.2f, 1.0f, 0.0f, 0) & PAD_CIRCLE) != 0;
    CHECK(circ == 0, "sin control: no detona (%d)", circ);
    bs.in_control = 1;
    c4_set_placed(0);
}


/* ---- C4 directo de punta a punta: colocar (sin ataque del juego) -> NewBakudan falso -> munición -> detonar con el agarre ---- */
static int ch_made;
static uint32_t ch_create(void *u, const VrFireMat *m) {
    (void)u; (void)m;
    ch_made++;
    int32_t n; memcpy(&n, ram + (0x800A0604u & 0x1FFFFFu), 4); n++; memcpy(ram + (0x800A0604u & 0x1FFFFFu), &n, 4);
    return 0x800D0000u;
}
static void ch_sound(void *u, uint32_t pos, int id) { (void)u; (void)pos; (void)id; }
static void c4_direct_chain(void) {
    printf("-- C4 directo: colocar, gastar una unidad y detonar --\n");
    world(6, 0, 4);
    const uint32_t ctrl = 0x800C0020u, ammo_a = BA_VARS + 2u * (BRIDGE_V_WEAPON_AMMO + 6), w_a = BA_VARS + 2u * BRIDGE_V_WEAPON;
    const int16_t w6 = 6, am = 4;
    uint8_t keep[4 + 2 + 2];      /* lo que había en esas posiciones (otras pruebas dependen del bloque de jugador de la RAM) */
    memcpy(keep, ram + (BA_PLAYER_CTRL & 0x1FFFFFu), 4); memcpy(keep + 4, ram + (w_a & 0x1FFFFFu), 2); memcpy(keep + 6, ram + (ammo_a & 0x1FFFFFu), 2);
    memcpy(ram + (BA_PLAYER_CTRL & 0x1FFFFFu), &ctrl, 4);
    memcpy(ram + (w_a & 0x1FFFFFu), &w6, 2); memcpy(ram + (ammo_a & 0x1FFFFFu), &am, 2);
    c4_set_placed(0);
    ray_stub_mm = 500.0f;
    { VrFireMat dm; while (vrfire_c4_take(&dm)) {} }
    fresh_weapon(6);
    for (int i = 0; i < 6; i++) c4vbl(-55.0f, 0.0f);
    for (int i = 0; i < 6; i++) c4vbl(-55.0f, 1.0f);
    for (int i = 0; i < 4; i++) c4vbl(-55.0f, 0.0f);
    CHECK(vrfire_c4_pending() && sq_seen == 0, "soltar el gatillo contra la pared deja una petición directa y no pulsa el ataque (cuadrados %d)", sq_seen);
    const VrFireC4Ops ops = {ch_create, ch_sound, NULL};
    CHECK(vrfire_c4_pump(ram, &ops) == 1 && ch_made == 1, "el juego (gancho de la cámara) crea la carga");
    int16_t am2; memcpy(&am2, ram + (ammo_a & 0x1FFFFFu), 2);
    CHECK(am2 == 3, "se gasta una unidad (%d)", am2);
    int circ = 0;
    for (int i = 0; i < 6; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    for (int i = 0; i < 12; i++) circ += (gvbl(0.2f, 1.0f, 0.0f, 0) & PAD_CIRCLE) != 0;
    CHECK(circ >= 2 && circ <= 6, "con la carga creada así el agarre derecho detona (círculo %d VBlanks)", circ);
    c4_set_placed(0);
    memcpy(ram + (BA_PLAYER_CTRL & 0x1FFFFFu), keep, 4); memcpy(ram + (w_a & 0x1FFFFFu), keep + 4, 2); memcpy(ram + (ammo_a & 0x1FFFFFu), keep + 6, 2);
    ray_stub_mm = -1;
}

/* ---- zona de menú junto al cuerpo: el menú se abre con el GRIP (botón R1 del mando entrante) de la mano en su zona; el gatillo en la zona dispara como fuera; con el menú abierto el gatillo del arma está bloqueado ---- */
static uint16_t zone_pad;           /* mando entrante de las pruebas de zona: PAD_R1 = grip derecho apretado (vr_input lo entrega como R1 de la mano dominante) */
static float zone_grip = 1.0f;      /* agarre de la mano en las pruebas de zona (la granada se prepara con la mano abierta: 0) */
static float zone_off[3];            /* desvío entre la pose puesta y la que lee vrgame (agarre del mando) */
static uint16_t zvbl(float d, float trig) {      /* mano derecha a `d` m lateral de su zona de menú (0,16 a la derecha, 0,305 bajo la cabeza, 0,005 delante; el "delante" de la cabeza de la prueba es -Z) */
    MvVrHand h;
    memset(&h, 0, sizeof h);
    h.valid = 1; h.pos[0] = 0.16f + d - zone_off[0]; h.pos[1] = 1.6f - 0.305f - zone_off[1]; h.pos[2] = -0.005f - zone_off[2]; h.rot[3] = 1.0f; h.trigger = trig; h.grip = zone_grip;
    mv_vrgame_set_hand(1, &h);
    vrgame_test_advance(8000000ull);
    uint16_t o = vrgame_pad(zone_pad, &bs, ram);
    if (o & PAD_SQUARE) sq_seen++;
    return o;
}
static void zones(float d, float trig, int n) { for (int i = 0; i < n; i++) zvbl(d, trig); }
static void menu_zone_blocks_fire(void) {
    printf("-- zona de menú: el gatillo dispara y no abre; el grip abre y bloquea el gatillo --\n");
    world(0, 12, 48);
    fresh_pistol();
    { MvVrHand h; memset(&h, 0, sizeof h); h.valid = 1; h.pos[1] = 1.0f; h.rot[3] = 1.0f; mv_vrgame_set_hand(1, &h); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram);
      float p[3]; if (vrgame_hand_raw(1, p, NULL, NULL, NULL)) { zone_off[0] = p[0] - 0.0f; zone_off[1] = p[1] - 1.0f; zone_off[2] = p[2] - 0.0f; } }
    zone_pad = 0;
    zones(0.6f, 0.0f, 10);
    MvVrGameStatus st; mv_vrgame_status(&st);
    CHECK(st.active && st.weapon == 0, "pistola mostrada");
    uint32_t s0 = st.shots; VrFireStats fs; vrfire_stats(&fs); uint32_t r0 = fs.requests; sq_seen = 0;
    zones(0.0f, 0.0f, 8);                                            /* entra en la zona con el gatillo suelto */
    zones(0.0f, 0.9f, 12);                                           /* y lo aprieta dentro */
    mv_vrgame_status(&st); vrfire_stats(&fs);
    CHECK(st.shots == s0 + 1 && fs.requests == r0 + 1, "gatillo apretado en la zona = 1 disparo (%u), 1 petición (%u)", st.shots - s0, fs.requests - r0);
    zones(0.0f, 0.9f, 60);                                           /* mantenido 1 s: no abre ninguna ventana */
    CHECK(mv_vrgame_menu_hand() == -1, "el gatillo mantenido en la zona no abre el menú (mano %d)", (int)mv_vrgame_menu_hand());
    zones(0.0f, 0.0f, 6);
    /* gatillo ya apretado fuera (disparo pedido) y la mano entra en la zona: la petición en cola NO se anula */
    zones(0.6f, 0.0f, 20);
    mv_vrgame_status(&st); s0 = st.shots; vrfire_stats(&fs); r0 = fs.requests; uint32_t c0 = fs.cancelled;
    zones(0.6f, 0.9f, 1);
    zones(0.0f, 0.9f, 1);                                            /* entra en la zona con la petición en cola */
    mv_vrgame_status(&st); vrfire_stats(&fs);
    CHECK(st.shots == s0 + 1 && fs.requests == r0 + 1 && fs.cancelled == c0, "entrar en la zona con el disparo en cola no lo anula (disparos %u, peticiones %u, anuladas %u)", st.shots - s0, fs.requests - r0, fs.cancelled - c0);
    zones(0.0f, 0.0f, 20); { VrFireMat m; (void)m; while (vrfire_take_projectile(0, &m) == VRFIRE_SUBST) {} }
    zones(0.6f, 0.0f, 20); { VrFireMat m; (void)m; while (vrfire_take_projectile(0, &m) == VRFIRE_SUBST) {} }
    /* el grip en la zona abre la ventana (0,3 s) y bloquea el gatillo del arma; la mano puede salir; al soltar el grip con el gatillo apretado sigue bloqueado hasta soltarlo */
    mv_vrgame_status(&st); s0 = st.shots;
    zones(0.0f, 0.0f, 8);
    zone_pad = PAD_R1; zones(0.0f, 0.0f, 30);
    CHECK(mv_vrgame_menu_hand() == 1, "grip derecho en la zona: ventana de armas abierta (mano %d)", (int)mv_vrgame_menu_hand());
    sq_seen = 0; zones(0.0f, 0.9f, 12);                              /* gatillo con la ventana abierta */
    mv_vrgame_status(&st);
    CHECK(st.shots == s0 && sq_seen == 0, "ventana abierta: el gatillo no dispara (%u, cuadrado %d)", st.shots - s0, sq_seen);
    zones(0.6f, 0.9f, 12);                                           /* la mano sale con el grip y el gatillo apretados */
    CHECK(mv_vrgame_menu_hand() == 1, "al salir de la zona la ventana sigue mientras siga el grip (mano %d)", (int)mv_vrgame_menu_hand());
    zone_pad = 0; zones(0.6f, 0.9f, 8);                              /* suelta el grip con el gatillo apretado: sigue bloqueado hasta soltarlo */
    mv_vrgame_status(&st);
    CHECK(mv_vrgame_menu_hand() == -1 && st.shots == s0 && sq_seen == 0, "grip suelto: ventana cerrada y gatillo aún bloqueado (mano %d, disparos %u)", (int)mv_vrgame_menu_hand(), st.shots - s0);
    zones(0.6f, 0.0f, 6);
    zones(0.6f, 0.9f, 4);                                            /* pulsación nueva */
    mv_vrgame_status(&st);
    CHECK(st.shots == s0 + 1, "soltar y apretar de nuevo = 1 disparo (%u)", st.shots - s0);
    /* grip FUERA de la zona: no abre */
    zones(0.6f, 0.0f, 60);
    zone_pad = PAD_R1; zones(0.6f, 0.0f, 40);
    CHECK(mv_vrgame_menu_hand() == -1, "grip fuera de la zona no abre el menú (mano %d)", (int)mv_vrgame_menu_hand());
    zone_pad = 0; zones(0.6f, 0.0f, 20);
    printf("   zona de menú: desvío de la pose %.3f %.3f %.3f\n", zone_off[0], zone_off[1], zone_off[2]);
}

/* granada equipada: el gatillo en la zona del pecho la prepara y la lanza como fuera; con la ventana abierta con el grip no se prepara ni se lanza */
static void menu_zone_blocks_grenade(void) {
    printf("-- zona de menú: la granada se lanza con el gatillo; el menú abierto con el grip la bloquea --\n");
    world(2, 0, 3);
    fresh_weapon(2);
    zone_grip = 0.0f; zone_pad = 0;
    { MvVrHand h; memset(&h, 0, sizeof h); h.valid = 1; h.pos[1] = 1.0f; h.rot[3] = 1.0f; mv_vrgame_set_hand(1, &h); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram);
      float p[3]; if (vrgame_hand_raw(1, p, NULL, NULL, NULL)) { zone_off[0] = p[0] - 0.0f; zone_off[1] = p[1] - 1.0f; zone_off[2] = p[2] - 0.0f; } }
    zones(0.6f, 0.0f, 12);
    MvVrGameStatus st; mv_vrgame_status(&st);
    const uint32_t t0 = st.throws;                                   /* el contador de lanzamientos es acumulado */
    CHECK(st.active && st.weapon == 2 && inst_state->phase == WT_PH_HELD, "granada mostrada en la mano (fase %d)", inst_state->phase);
    zones(0.0f, 0.0f, 8);                                            /* mano en la zona, gatillo suelto */
    zones(0.0f, 0.9f, 12);                                           /* gatillo en la zona: se prepara */
    CHECK(inst_state->phase == WT_PH_READY, "gatillo en la zona: la granada se prepara (fase %d)", inst_state->phase);
    CHECK(mv_vrgame_menu_hand() == -1, "y no se abre ningún menú (mano %d)", (int)mv_vrgame_menu_hand());
    zones(0.0f, 0.0f, 8);
    mv_vrgame_status(&st);
    CHECK(st.throws == t0 + 1, "soltar en la zona la lanza (%u)", st.throws - t0);
    zones(0.6f, 0.0f, 120);
    mv_vrgame_status(&st);
    if (inst_state->phase == WT_PH_HELD) {
        sq_seen = 0;
        zones(0.0f, 0.0f, 8);
        zone_pad = PAD_R1; zones(0.0f, 0.0f, 45);                    /* grip en la zona: abre la ventana de armas */
        CHECK(mv_vrgame_menu_hand() == 1, "grip en la zona: ventana abierta (mano %d)", (int)mv_vrgame_menu_hand());
        zones(0.0f, 0.9f, 12);                                       /* gatillo con la ventana abierta */
        CHECK(inst_state->phase == WT_PH_HELD && sq_seen == 0, "ventana abierta: la granada no se prepara (fase %d, cuadrado %d)", inst_state->phase, sq_seen);
        zones(0.6f, 0.9f, 10);                                       /* sale con el grip y el gatillo apretados */
        zone_pad = 0; zones(0.6f, 0.9f, 6);                          /* suelta el grip: sigue bloqueado hasta soltar el gatillo */
        zones(0.6f, 0.0f, 8);
        mv_vrgame_status(&st);
        CHECK(st.throws == t0 + 1 && inst_state->phase == WT_PH_HELD && sq_seen == 0, "salir, soltar el grip y el gatillo: sin lanzamiento nuevo (%u, fase %d, cuadrado %d)", st.throws - t0, inst_state->phase, sq_seen);
    } else printf("   (sin segunda granada en la mano tras lanzar: fase %d; se omite la parte con la ventana abierta)\n", inst_state->phase);
    zone_pad = 0;
    zone_grip = 1.0f;
}

/* C4 y zona del pecho: el grip derecho abre el menú de armas y NO detona dentro de la zona; fuera de ella detona (el círculo del CQC sigue retirado con el C4 equipado) */
static void c4_remote_chest(void) {
    printf("-- C4: el grip en la zona del pecho abre el menú y no detona; fuera detona --\n");
    world(6, 0, 4);
    vrgame_reset(); vrgame_set_enabled(0, 1);
    zone_grip = 1.0f; zone_pad = 0;
    { MvVrHand h; memset(&h, 0, sizeof h); h.valid = 1; h.pos[1] = 1.0f; h.rot[3] = 1.0f; mv_vrgame_set_hand(1, &h); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram);
      float p[3]; if (vrgame_hand_raw(1, p, NULL, NULL, NULL)) { zone_off[0] = p[0]; zone_off[1] = p[1] - 1.0f; zone_off[2] = p[2]; } }
    c4_set_placed(1);
    int circ = 0;
    zone_grip = 0.0f; zones(0.0f, 0.0f, 10);                      /* mano en la zona, grip suelto */
    zone_grip = 1.0f; zone_pad = PAD_R1;                          /* grip a fondo (analógico y R1 del mando entrante) */
    for (int i = 0; i < 40; i++) circ += (zvbl(0.0f, 0.0f) & PAD_CIRCLE) != 0;
    CHECK(circ == 0, "grip en la zona con C4: no detona (%d VBlanks con círculo)", circ);
    CHECK(mv_vrgame_menu_hand() == 1, "grip en la zona con C4: abre la ventana de armas (mano %d)", (int)mv_vrgame_menu_hand());
    zone_grip = 0.0f; zone_pad = 0; zones(0.0f, 0.0f, 10);        /* suelta el grip */
    CHECK(mv_vrgame_menu_hand() == -1, "soltar el grip cierra la ventana (mano %d)", (int)mv_vrgame_menu_hand());
    zones(0.6f, 0.0f, 10);                                        /* fuera de la zona, grip suelto */
    zone_grip = 1.0f; zone_pad = PAD_R1; circ = 0;
    for (int i = 0; i < 12; i++) circ += (zvbl(0.6f, 0.0f) & PAD_CIRCLE) != 0;
    CHECK(circ >= 2, "grip fuera de la zona con C4: detona (%d)", circ);
    CHECK(mv_vrgame_menu_hand() == -1, "y no abre ningún menú (mano %d)", (int)mv_vrgame_menu_hand());
    /* entra en la zona con el grip ya apretado tras detonar: ni detona otra vez ni abre */
    circ = 0;
    for (int i = 0; i < 30; i++) circ += (zvbl(0.0f, 0.0f) & PAD_CIRCLE) != 0;
    CHECK(circ == 0 && mv_vrgame_menu_hand() == -1, "grip mantenido que entra en la zona: sin detonar ni abrir (%d, mano %d)", circ, (int)mv_vrgame_menu_hand());
    zone_grip = 1.0f; zone_pad = 0; c4_set_placed(0);
    zones(0.6f, 0.0f, 20);
}

/* ---- arma colgada del mando: mundo = agarre del mando * desvío fijo * zócalo^-1 (a la cadencia del renderer) ---- */
static float t_rnd(void) { return (float)rand() / (float)RAND_MAX; }
static void t_qmul(const float a[4], const float b[4], float o[4]) {
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
static void t_qnorm(float q[4]) { float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]); for (int i = 0; i < 4; i++) q[i] /= l; }
static void t_qmat(const float q[4], float R[9]) {      /* fila-mayor */
    float x = q[0], y = q[1], z = q[2], w = q[3];
    R[0] = 1 - 2 * (y * y + z * z); R[1] = 2 * (x * y - z * w);     R[2] = 2 * (x * z + y * w);
    R[3] = 2 * (x * y + z * w);     R[4] = 1 - 2 * (x * x + z * z); R[5] = 2 * (y * z - x * w);
    R[6] = 2 * (x * z - y * w);     R[7] = 2 * (y * z + x * w);     R[8] = 1 - 2 * (x * x + y * y);
}
static void t_mmul(const float A[9], const float B[9], float C[9]) { for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) C[3 * i + j] = A[3 * i] * B[j] + A[3 * i + 1] * B[3 + j] + A[3 * i + 2] * B[6 + j]; }
static void t_mtr(const float A[9], float T[9]) { for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) T[3 * i + j] = A[3 * j + i]; }
static void t_rand_quat(float q[4]) {      /* uniforme; el signo también al azar (q y -q son el mismo giro) */
    float u1 = t_rnd(), u2 = t_rnd() * 6.2831853f, u3 = t_rnd() * 6.2831853f;
    q[0] = sqrtf(1 - u1) * sinf(u2); q[1] = sqrtf(1 - u1) * cosf(u2); q[2] = sqrtf(u1) * sinf(u3); q[3] = sqrtf(u1) * cosf(u3);
    if (rand() & 1) for (int i = 0; i < 4; i++) q[i] = -q[i];
}
/* dirección esperada de la boca (-Z del arma en el marco del arma): R(agarre) * R(cabeceo) * R(alabeo) * M_mano * R(zócalo)^T * (0,0,-1). Todo sale de la configuración del cuerpo y de la tabla de vr_grip. */
static void expected_weapon_rot(int side, const float gq[4], float R[9]) {
    const VrBodyConfig *bc = vrbody_config();
    float a = bc->hand_pitch_deg * 0.017453292f * 0.5f, qp[4] = {sinf(a), 0, 0, cosf(a)}, qr[4] = {bc->hand_rot_offset[side][0], bc->hand_rot_offset[side][1], bc->hand_rot_offset[side][2], bc->hand_rot_offset[side][3]};
    t_qnorm(qr);
    static const float MR[9] = {0, 0, -1, -1, 0, 0, 0, 1, 0}, ML[9] = {0, 0, 1, 1, 0, 0, 0, 1, 0};
    WPose sock = vrgrip_find(0)->grip;
    if (side == 0) sock = vrgrip_mirror(sock);
    float qs[4] = {sock.q.x, sock.q.y, sock.q.z, sock.q.w}, Rg[9], Rp[9], Rr[9], Rs[9], Rst[9], T1[9], T2[9], T3[9];
    t_qnorm(qs);
    t_qmat(gq, Rg); t_qmat(qp, Rp); t_qmat(qr, Rr); t_qmat(qs, Rs); t_mtr(Rs, Rst);
    t_mmul(Rg, Rp, T1); t_mmul(T1, Rr, T2); t_mmul(T2, side == 1 ? MR : ML, T3); t_mmul(T3, Rst, R);
}
/* Distancia fija entre el agarre del mando (= centro de agarre de la mano, vrhs_grip_center) y el origen del arma, que fija el zócalo de la pistola (vr_grip_fit.inc): |inv(zócalo) * 0 - centro|.
 * Sale de la tabla (cambia cuando se regenera el ajuste o el tamaño de la mano), no de una constante. */
static float sock_grip_dist(void) {
    const WPose si = wpose_inv(vrgrip_find(0)->grip);
    float c[3]; vrhs_grip_center(1, c);
    const float d[3] = {si.p.x - c[0] * VRHS_UNIT_M, si.p.y - c[1] * VRHS_UNIT_M, si.p.z - c[2] * VRHS_UNIT_M};
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}
/* Distancia fija entre el centro de agarre de la mano de apoyo (la posición que da la IK) y WP_SUPPORT: |zócalo de apoyo * centro de agarre|. El módulo no depende del lado (la izquierda es el espejo exacto). */
static float sup_dist(void) {
    float c[3]; vrhs_grip_center(1, c);
    const WVec3 w = wpose_apply(vrgrip_find(0)->support, wv(c[0] * VRHS_UNIT_M, c[1] * VRHS_UNIT_M, c[2] * VRHS_UNIT_M));
    return sqrtf(w.x * w.x + w.y * w.y + w.z * w.z);
}
static float t_ang(const float a[3], const float b[3]) { float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; return acosf(fminf(1.0f, fmaxf(-1.0f, d))) * 57.29578f; }

static WMesh syn_mesh[16];
static WeaponState *syn_state;
static const WeaponDef *syn_def;
static void fresh_meshes(int gid) {      /* arma del registro con una malla mínima por pieza: sus frames no salen vacíos */
    static WeaponState *old;
    if (old) { free(old); old = NULL; }
    syn_def = NULL;
    for (int i = 0; i < w_registry_count; i++) if (w_registry[i] && w_registry[i]->game_weapon_id == gid) syn_def = w_registry[i];
    static WVertex vtx[3] = {{{0, 0, 0}, {0, 1, 0}, {0, 0}, {255, 255, 255, 255}}, {{0.01f, 0, 0}, {0, 1, 0}, {1, 0}, {255, 255, 255, 255}}, {{0, 0.01f, 0}, {0, 1, 0}, {0, 1}, {255, 255, 255, 255}}};
    static uint16_t idx[3] = {0, 1, 2};
    int n = weapon_mesh_count(syn_def);
    for (int i = 0; i < n; i++) syn_mesh[i] = (WMesh){syn_def->mesh_names[i], vtx, 3, idx, 3, 0};
    WeaponState *st = calloc(1, syn_def->state_size);
    st->def = syn_def; syn_def->init(st); st->def = syn_def;
    weapon_bind(st, syn_mesh, n, syn_def->parts, syn_def->nparts);
    syn_state = old = st;
    VrHandsConfig hc; vrhands_default_config(&hc); hc.grip_mode = VRH_GRIP_SOCKET;
    VrHandsRig *rig = vrhands_create(&hc);
    vrhands_bind(rig, st, NULL, 0, 0);
    vrgame_test_install(gid, (void *)syn_def, st, rig);
    vrgame_reset();
    vrgame_set_enabled(0, 1);
}
static void fresh_pistol_meshes(void) { fresh_meshes(0); }
/* Como fresh_meshes, pero con la pieza de la pantalla LED sintética (un rectángulo de cara +Z) para que aa_led_face / guided_led_face la hallen: Stinger = pieza "screen" (en el modelo real cuelga del cuerpo con parent -1), Nikita = cara del "tube" */
static WPart led_parts[16];
static void fresh_led_meshes(int gid) {
    static WeaponState *old;
    if (old) { free(old); old = NULL; }
    syn_def = NULL;
    for (int i = 0; i < w_registry_count; i++) if (w_registry[i] && w_registry[i]->game_weapon_id == gid) syn_def = w_registry[i];
    static WVertex tri[3] = {{{0, 0, 0}, {0, 1, 0}, {0, 0}, {255, 255, 255, 255}}, {{0.01f, 0, 0}, {0, 1, 0}, {1, 0}, {255, 255, 255, 255}}, {{0, 0.01f, 0}, {0, 1, 0}, {0, 1}, {255, 255, 255, 255}}};
    static uint16_t tidx[3] = {0, 1, 2};
    static WVertex quad_aa[4] = {{{-0.03f, -0.0225f, 0}, {0, 0, 1}, {0, 0}, {255, 255, 255, 255}}, {{0.03f, -0.0225f, 0}, {0, 0, 1}, {1, 0}, {255, 255, 255, 255}}, {{0.03f, 0.0225f, 0}, {0, 0, 1}, {1, 1}, {255, 255, 255, 255}}, {{-0.03f, 0.0225f, 0}, {0, 0, 1}, {0, 1}, {255, 255, 255, 255}}};
    static WVertex quad_gd[4] = {{{-0.09f, -0.03f, -0.08f}, {0, 0, 1}, {0, 0}, {255, 255, 255, 255}}, {{-0.02f, -0.03f, -0.08f}, {0, 0, 1}, {1, 0}, {255, 255, 255, 255}}, {{-0.02f, 0.03f, -0.08f}, {0, 0, 1}, {1, 1}, {255, 255, 255, 255}}, {{-0.09f, 0.03f, -0.08f}, {0, 0, 1}, {0, 1}, {255, 255, 255, 255}}};
    static uint16_t qidx[6] = {0, 1, 2, 0, 2, 3};
    int n = weapon_mesh_count(syn_def), np = syn_def->nparts;
    for (int i = 0; i < n; i++) syn_mesh[i] = (WMesh){syn_def->mesh_names[i], tri, 3, tidx, 3, 0};
    for (int i = 0; i < np && i < 16; i++) {
        led_parts[i] = syn_def->parts[i];
        if (gid == 4 && led_parts[i].name && !strcmp(led_parts[i].name, "screen")) led_parts[i].parent = -1;
    }
    for (int i = 0; i < n; i++) {
        if (gid == 4 && !strcmp(syn_def->mesh_names[i], "screen")) syn_mesh[i] = (WMesh){syn_def->mesh_names[i], quad_aa, 4, qidx, 6, 0};
        if (gid == 3 && !strcmp(syn_def->mesh_names[i], "tube")) syn_mesh[i] = (WMesh){syn_def->mesh_names[i], quad_gd, 4, qidx, 6, 0};
    }
    WeaponState *st = calloc(1, syn_def->state_size);
    st->def = syn_def; syn_def->init(st); st->def = syn_def;
    weapon_bind(st, syn_mesh, n, led_parts, np);
    syn_state = old = st;
    VrHandsConfig hc; vrhands_default_config(&hc); hc.grip_mode = VRH_GRIP_SOCKET;
    VrHandsRig *rig = vrhands_create(&hc);
    vrhands_bind(rig, st, NULL, 0, 0);
    vrgame_test_install(gid, (void *)syn_def, st, rig);
    vrgame_reset();
    vrgame_set_enabled(0, 1);
}
static void hand_at(int side, const float p[3], const float q[4]) {
    MvVrHand h;
    memset(&h, 0, sizeof h);
    h.valid = 1; memcpy(h.pos, p, sizeof h.pos); memcpy(h.rot, q, sizeof h.rot); h.grip = 1.0f;
    mv_vrgame_set_hand(side, &h);
}
/* Pose de la pieza raíz del arma dibujada por el renderer (mv_host_hands): dirección de la boca (-Z) y origen del arma. 1 = hay arma dibujada. */
static float drawn_up[3];      /* +Y del arma de la última llamada (arriba del arma: su alabeo alrededor de la boca) */
static int drawn_weapon(float dir[3], float org[3]) {
    VrHandsFrame f;
    int rc = mv_host_hands(&f);
    if (!rc || f.nmeshes < 1) { mv_host_hands_done(); return 0; }
    const float *m = f.meshes[0].model;      /* piezas por orden de def->parts: la 0 es la raíz */
    dir[0] = -m[8]; dir[1] = -m[9]; dir[2] = -m[10];
    drawn_up[0] = m[4]; drawn_up[1] = m[5]; drawn_up[2] = m[6];
    const WPose *rest = &syn_def->parts[0].rest;
    for (int i = 0; i < 3; i++) org[i] = m[12 + i] - (m[i] * rest->p.x + m[4 + i] * rest->p.y + m[8 + i] * rest->p.z);
    mv_host_hands_done();
    return 1;
}
/* ---- banco de lanzamiento real: curva de mano de una persona, gatillo que se abre en distintos momentos, por la cadena completa de vrgame ----
 * Poses del mando a ~100 Hz con jitter (mv_vrgame_set_hand, reloj inyectado), VBlanks a 50 Hz con jitter (vrgame_pad), gatillo analógico que cae en 60 ms. La mano acelera hasta `vp` en 0,15 s
 * (smoothstep), frena en 0,12 s y el gatillo empieza a abrirse `off` ms después del pico. Se mide lo que llega al motor (vrfire_take_throw, velocidad horizontal por tick -> m/s). */
static float bench_speed(float vp, float t, float windup) {
    const float ta = 0.15f, td = 0.12f, tw = 0.30f;
    if (t < 0) { float x = (t + tw) / tw; return x > 0 && windup > 0 ? -windup * sinf(3.14159265f * x) : 0.0f; }
    if (t < ta) { float x = t / ta; return vp * x * x * (3 - 2 * x); }
    if (t < ta + td) { float x = (t - ta) / td; return vp * (1 - x * x * (3 - 2 * x)); }
    return 0.0f;
}
/* Devuelve la velocidad horizontal que llega al motor (m/s; -1: sin lanzamiento); v_pk = pico de la mano en los 250 ms previos a abrirse el gatillo; v_rel = velocidad al empezar a abrirse. */
static int bench_burst;      /* 1: de cada 5 VBlanks, el siguiente llega pegado (el emulador se pone al día: misma pose, 0 ms) */
static float bench_throw(float vp, int off_ms, float windup, float fall_s, float *v_pk, float *v_rel, int hold_after) {
    enum { T0 = 1000, N = 3000 };
    static float px[N + 1], pz[N + 1], py[N + 1];
    const float dx = 0.2f, dy = 0.25f, dz = -0.95f, dl = sqrtf(dx * dx + dy * dy + dz * dz), hz = sqrtf(dx * dx + dz * dz) / dl;
    float x = 0.2f, y = 1.2f, z = -0.3f;
    for (int ms = 0; ms <= N; ms++) {
        float v = bench_speed(vp, (float)(ms - T0) * 0.001f, windup);
        x += dx / dl * v * 0.001f; y += dy / dl * v * 0.001f; z += dz / dl * v * 0.001f;
        px[ms] = x; py[ms] = y; pz[ms] = z;
    }
    const int rel = T0 + 150 + off_ms;                                 /* ms en que el gatillo empieza a abrirse */
    *v_pk = 0; *v_rel = fabsf(bench_speed(vp, (float)(rel - T0) * 0.001f, windup)) * hz;
    for (int ms = rel - 250; ms <= rel; ms++) { float v = fabsf(bench_speed(vp, (float)(ms - T0) * 0.001f, windup)) * hz; if (v > *v_pk) *v_pk = v; }
    world(2, 0, 3);
    fresh_weapon(2);
    int16_t cp[3], cv[3];
    vrfire_take_throw(cp, cv);                                         /* vacía lo anterior */
    int k = 0;
    for (int ms = 0; ms < rel + hold_after; ms++) {
        vrgame_test_advance(1000000ull);
        if (ms % 10 == 3) {                                            /* pose del visor (~100 Hz) con el gatillo analógico del momento */
            float tr = ms < 400 ? 0.0f : ms < 430 ? (float)(ms - 400) / 30.0f : ms < rel ? 1.0f : 1.0f - (float)(ms - rel) / (fall_s * 1000.0f);
            if (tr < 0) tr = 0;
            MvVrHand h; memset(&h, 0, sizeof h);
            h.valid = 1; h.pos[0] = px[ms]; h.pos[1] = py[ms]; h.pos[2] = pz[ms]; h.rot[3] = 1.0f; h.trigger = tr; h.grip = 0.0f;
            mv_vrgame_set_hand(1, &h);
        }
        if (ms >= 20 * k + ((k * 37) % 9) - 4) { k++; vrgame_pad(0, &bs, ram); if (bench_burst && k % 5 == 0) vrgame_pad(0, &bs, ram); }      /* VBlank a 50 Hz con jitter de +-4 ms */
    }
    int16_t pos[3], vel[3];
    if (!vrfire_take_throw(pos, vel)) return -1.0f;
    return sqrtf((float)vel[0] * (float)vel[0] + (float)vel[2] * (float)vel[2]) / (1000.0f * 0.04f);
}
static void throw_bench(void) {
    printf("-- banco de lanzamiento real (cadena completa): velocidad que llega al motor frente a la de la mano --\n");
    static const float vps[3] = {4.0f, 6.0f, 8.0f};
    static const int offs[7] = {-60, -30, 0, 30, 60, 100, 150};
    static const float falls[2] = {0.05f, 0.12f};
    float worst = 9.0f, best_late = 0.0f;
    for (int w = 0; w < 4; w++)
    for (int f = 0; f < 2; f++)
    for (int a = 0; a < 3; a++)
        for (int o = 0; o < 7; o++) {
            bench_burst = w >> 1;
            float pk, rl, v = bench_throw(vps[a], offs[o], (w & 1) ? 2.0f : 0.0f, falls[f], &pk, &rl, 400);
            /* techo: la mano sigue acelerando hasta que el gatillo cruza 0,4 (~60 % de la caida tras empezar a abrirse) */
            float up = pk;
            for (int ms = 0; ms <= (int)(falls[f] * 600.0f); ms++) { float h = fabsf(bench_speed(vps[a], (float)(150 + offs[o] + ms) * 0.001f, (w & 1) ? 2.0f : 0.0f)) * 0.958f; if (h > up) up = h; }
            if (offs[o] >= -60 && offs[o] <= 60) {
                if (pk > 0 && v / pk < worst) worst = v / pk;
                /* el gesto (mano a 3,8-7,7 m/s hacia delante y 14,5 grados arriba) siempre es un lanzamiento: sale con el módulo fijo del juego (4,93 m/s) a 20 grados de elevación -> 4,63 m/s en horizontal */
                (void)up; (void)rl;
                CHECK(fabsf(v - 4.63f) < 0.12f, "pico %.2f, gatillo %+d ms, caida %.0f ms, retroceso/rafaga %d: llegan %.2f m/s en horizontal (esperado 4,63, el máximo del juego; pico de la mano %.2f)", vps[a] * 0.958f, offs[o], falls[f] * 1000.0f, w, v, pk);
            } else if (offs[o] == 100 && pk > 0 && v / pk > best_late) best_late = v / pk;
        }
    bench_burst = 0;
    printf("   peor caso con el gatillo de -60 a +60 ms del pico: %.0f %% de la velocidad pico de la mano (la salida es el módulo fijo del juego)\n", 100.0f * worst);
}
static void weapon_follows_controller(void) {
    printf("-- arma colgada del mando: la boca sigue al mando girado por un desvío fijo --\n");
    srand(12345);
    float maxerr = 0, maxerr_live = 0, maxpos = 0;
    int shown = 0;
    for (int side = 1; side >= 0; side--) {
        world(0, 12, 60);
        fresh_pistol_meshes();
        mv_vrgame_set_options(side, 2);
        for (int k = 0; k < 150; k++) {
            float p[3] = {t_rnd() * 2 - 1, 0.8f + t_rnd(), -t_rnd()}, q[4], R[9], dir[3], org[3];
            t_rand_quat(q);
            hand_at(side, p, q);
            vrgame_test_advance(2000000ull);
            vrgame_pad(0, &bs, ram);
            if (!drawn_weapon(dir, org)) continue;
            shown++;
            expected_weapon_rot(side, q, R);
            float want[3] = {-R[2], -R[5], -R[8]};
            float e = t_ang(dir, want);
            if (e > maxerr) maxerr = e;
            float dp = sqrtf((org[0] - p[0]) * (org[0] - p[0]) + (org[1] - p[1]) * (org[1] - p[1]) + (org[2] - p[2]) * (org[2] - p[2]));
            if (dp > maxpos) maxpos = dp;
            /* a la cadencia del renderer: el mando se movió desde la última simulación y el arma lo sigue sin esperar al VBlank */
            float p2[3] = {p[0] + 0.03f * (t_rnd() - 0.5f), p[1] + 0.03f * (t_rnd() - 0.5f), p[2]}, q2[4], dq[4] = {0.02f * (t_rnd() - 0.5f), 0.02f * (t_rnd() - 0.5f), 0.02f * (t_rnd() - 0.5f), 1};
            t_qnorm(dq); t_qmul(dq, q, q2);
            hand_at(side, p2, q2);
            if (drawn_weapon(dir, org)) {
                expected_weapon_rot(side, q2, R);
                float w2[3] = {-R[2], -R[5], -R[8]};
                float e2 = t_ang(dir, w2);
                if (e2 > maxerr_live) maxerr_live = e2;
            }
        }
    }
    printf("   boca vs mando*desvío fijo: error máx %.4f grados (simulado) / %.4f (a cadencia del renderer), empuñadura %.5f m, %d poses aleatorias\n", maxerr, maxerr_live, maxpos, shown);
    CHECK(shown > 250, "arma dibujada en casi todas las poses (%d de 300)", shown);
    CHECK(maxerr < 0.5f, "boca = adelante del mando girado por el desvío fijo: error máximo %.3f grados (< 0,5)", maxerr);
    CHECK(maxerr_live < 0.5f, "con el mando movido tras la simulación el arma lo sigue: error máximo %.3f grados (< 0,5)", maxerr_live);
    /* Contrato (cambiado): el agarre del mando es la PALMA de la mano y el zócalo de la pistola (vr_grip_fit.inc, FIT_SOCOM_GRIP: ajustado contra la malla real con gripfit_gen.c, dedo en el gatillo)
     * deja el origen del arma a una distancia FIJA del agarre del mando (sock_grip_dist: 41,5 mm con la mano a tamaño real y la SOCOM a escala real; 32,3 mm antes), no encima. Que sea
     * constante garantiza que la boca/láser/bala siguen saliendo del cañón (comprobaciones de arriba). */
    const float sgd = sock_grip_dist();
    CHECK(maxpos > sgd - 0.003f && maxpos < sgd + 0.003f, "el punto de empuñadura del arma queda a la distancia fija del zócalo (%.1f mm) del agarre del mando: %.4f m", sgd * 1000.0f, maxpos);

    /* continuidad: el mando gira unos grados por fotograma (con saltos de signo del cuaternión): la boca gira lo mismo, sin saltos */
    for (int side = 1; side >= 0; side--) {
        world(0, 12, 60);
        fresh_pistol_meshes();
        mv_vrgame_set_options(side, 2);
        float q[4], p[3] = {0.1f, 1.2f, -0.3f}, prev[3] = {0, 0, 0}, pq[4] = {0, 0, 0, 1};
        t_rand_quat(q);
        int have = 0;
        float worst = 0;
        for (int k = 0; k < 400; k++) {
            float ax[3] = {t_rnd() - 0.5f, t_rnd() - 0.5f, t_rnd() - 0.5f}, l = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]) + 1e-6f, step = 3.0f * 0.017453292f * 0.5f;
            float dq[4] = {ax[0] / l * sinf(step), ax[1] / l * sinf(step), ax[2] / l * sinf(step), cosf(step)}, nq[4];
            t_qmul(dq, q, nq); memcpy(q, nq, sizeof q); t_qnorm(q);
            float qs[4]; memcpy(qs, q, sizeof qs);
            if (k % 7 == 3) for (int i = 0; i < 4; i++) qs[i] = -qs[i];        /* mismo giro, otro signo */
            hand_at(side, p, qs);
            vrgame_test_advance(2000000ull);
            vrgame_pad(0, &bs, ram);
            float dir[3], org[3];
            if (!drawn_weapon(dir, org)) continue;
            if (have) { float s = t_ang(dir, prev); if (s > worst) worst = s; }
            memcpy(prev, dir, sizeof prev); memcpy(pq, qs, sizeof pq); have = 1;
        }
        printf("   lado %d: giro máximo de la boca por fotograma %.3f grados con el mando a 3 grados\n", side, worst);
        CHECK(have && worst < 3.0f + 0.5f, "lado %d: la boca gira como mucho %.2f grados por fotograma con el mando a 3 grados (continuidad)", side, worst);
    }
}

/* ---- equipar un arma con giro de mano (Stinger) con la muestra de la mano inválida o válida: sin marco ni arma con muestra inválida y marco unitario (nunca cuaternión nulo) con la válida ----
 * No se puede probar de forma determinista la carrera real (mv_vrgame_set_hand entre dos tomas de HM: depende de la intercalación de hilos dentro de vrgame_pad). Sí se comprueba que wp[w] del PRIMER VBlank
 * tras equipar usa el giro nuevo: pub_sim_hand (la mano con la que se simuló el frame publicado, vrgame_test_sim_hand) debe coincidir con la del VBlank estable. Si vrbody_set_hand_turn se hiciera detrás de
 * la toma de manos, el primer VBlank usaría el giro anterior (el !mode del VBlank sin mano lo deja en NULL) y la mano diferiría del giro de la Stinger. */
static void equip_uses_new_turn(void) {
    printf("-- equipar con giro de mano: sin muestra válida no hay marco ni arma; con ella el marco es unitario --\n");
    float dir[3], org[3], d1[3], o1[3], p[3] = {0.2f, 1.2f, -0.3f}, q[4] = {0, 0, 0, 1}, hp[3], hq[4], sp1[3], sq1[4], sp2[3], sq2[4];
    CHECK(vrgrip_find(4) && (vrgrip_find(4)->turn.x != 0 || vrgrip_find(4)->turn.y != 0 || vrgrip_find(4)->turn.z != 0), "la Stinger de la tabla lleva giro de mano (si no, la prueba no discrimina)");
    world(4, 1, 4);
    fresh_meshes(4);
    mv_vrgame_set_options(1, 2);
    mv_vrgame_clear_hands();
    vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram);
    CHECK(drawn_weapon(dir, org) == 0 && vrgame_hand_frame_now(1, hp, hq) == 0, "sin mano válida: ni arma dibujada ni marco de la mano");
    hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram);
    CHECK(drawn_weapon(d1, o1) == 1 && vrgame_hand_frame_now(1, hp, hq) == 1, "primer VBlank con mano válida: arma dibujada");
    CHECK(vrgame_test_sim_hand(sp1, sq1) == 1, "primer VBlank con mano válida: hay mano simulada publicada");
    CHECK(fabsf(sqrtf(d1[0] * d1[0] + d1[1] * d1[1] + d1[2] * d1[2]) - 1.0f) < 1e-3f && fabsf(sqrtf(hq[0] * hq[0] + hq[1] * hq[1] + hq[2] * hq[2] + hq[3] * hq[3]) - 1.0f) < 1e-3f, "sin cuaternión nulo: la boca y el marco de la mano son unitarios");
    for (int i = 0; i < 4; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(drawn_weapon(dir, org) == 1, "arma dibujada tras 4 VBlanks");
    float de = 0, oe = 0;
    for (int i = 0; i < 3; i++) { de = fmaxf(de, fabsf(dir[i] - d1[i])); oe = fmaxf(oe, fabsf(org[i] - o1[i])); }
    CHECK(de < 1e-3f && oe < 1e-3f, "mano quieta: la pose dibujada del primer VBlank coincide con la estable (boca %.5f, origen %.5f m de diferencia)", de, oe);
    float pe = 0, qd;
    CHECK(vrgame_test_sim_hand(sp2, sq2) == 1, "VBlank estable: hay mano simulada publicada");
    for (int i = 0; i < 3; i++) pe = fmaxf(pe, fabsf(sp1[i] - sp2[i]));
    qd = fabsf(sq1[0] * sq2[0] + sq1[1] * sq2[1] + sq1[2] * sq2[2] + sq1[3] * sq2[3]);
    CHECK(pe < 1e-4f && qd > 1.0f - 1e-5f, "la mano simulada del primer VBlank (marco con el giro de la Stinger) coincide con la del VBlank estable (pos %.6f m, |q1.q2| %.7f)", pe, qd);
    mv_vrgame_clear_hands();
    vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram);
    CHECK(vrgame_hand_frame_now(1, hp, hq) == 0 && drawn_weapon(dir, org) == 0, "la mano se pierde: sin marco y sin arma dibujada (nunca marco nulo)");
}

/* ---- Stinger (id 4): (1) con el arma de dos manos empuñada y el jugador al mando se pide la IK de brazos aunque el motor oculte a Snake (vrbody_set_engine_hidden_ik); (2) el gesto de apuntar se mide contra la
 * pantalla LED (cara trasera, junto a la cara del jugador), no contra el visor WP_EYE 0,64 m por delante del tubo: cabeza a 0,15 m tras la LED y mirando por el eje del tubo apunta; en el visor antiguo (WP_EYE
 * menos 0,10 m, 0,54 m por delante del origen del arma) o mirando de lado no. ---- */
static void head_look(const float p[3], const float f[3]) {      /* cabeza en p mirando a f (unitario, no vertical): X = f x arriba, Y = X x f, Z = -f */
    float x[3] = {f[1] * 0 - f[2] * 1, f[2] * 0 - f[0] * 0, f[0] * 1 - f[1] * 0}, n = sqrtf(x[0] * x[0] + x[2] * x[2]);
    x[0] /= n; x[2] /= n;
    float y[3] = {x[1] * f[2] - x[2] * f[1], x[2] * f[0] - x[0] * f[2], x[0] * f[1] - x[1] * f[0]};
    memset(g_head, 0, sizeof g_head);
    for (int i = 0; i < 3; i++) { g_head[i] = x[i]; g_head[4 + i] = y[i]; g_head[8 + i] = -f[i]; g_head[12 + i] = p[i]; }
    g_head[15] = 1.0f;
}
static float d3(const float a[3], const float b[3]);
static void stinger_hidden_body_and_led_gesture(void) {
    printf("-- Stinger: IK pedida con el motor ocultando a Snake; el gesto de apuntar se mide contra la LED --\n");
    float dir[3], org[3], p[3] = {0.2f, 1.2f, -0.3f}, q[4] = {0, 0, 0, 1};
    world(4, 1, 4);
    fresh_meshes(4);
    mv_vrgame_set_options(1, 2);
    mv_vrgame_clear_hands();
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(drawn_weapon(dir, org) == 1, "Stinger dibujado");
    CHECK(vrbody_engine_hidden_ik() == 1, "Stinger empuñado y jugador al mando: se pide la IK aunque el motor oculte a Snake");
    bs.in_control = 0;
    for (int i = 0; i < 2; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(vrbody_engine_hidden_ik() == 0, "sin el control del jugador ni cámara (escena): sin petición");
    bs.cam_ctl = 1;      /* pausa o ventana de armas / objetos abierta con el Stinger en la mano: la IK se mantiene (antes in_control = 0 la retiraba y los brazos volvían a la pose del motor) */
    for (int i = 0; i < 2; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(vrbody_engine_hidden_ik() == 1, "pausa / selector con el arma en la mano (cam_ctl): se mantiene la petición");
    bs.cinematic = 1;
    for (int i = 0; i < 2; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(vrbody_engine_hidden_ik() == 0, "cinemática: sin petición");
    bs.cinematic = 0; bs.cam_ctl = 0;
    bs.in_control = 1;
    for (int i = 0; i < 2; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(vrbody_engine_hidden_ik() == 1, "con el control otra vez: petición");
    CHECK(vrgame_weld_main() == 1, "Stinger soldado con la derecha: la mano del gatillo es la derecha"); CHECK(vrbody_get_weld(0) == 1 && vrbody_get_weld(1) == 1, "Stinger: los DOS brazos atados al arma (apoyo %d, principal %d)", vrbody_get_weld(0), vrbody_get_weld(1));
    world(-1, 0, 0);
    for (int i = 0; i < 2; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(vrbody_engine_hidden_ik() == 1 && vrbody_get_weld(0) == 1 && vrbody_get_weld(1) == 1, "desequipado breve (recarga): se retienen la petición y las soldaduras");
    for (int i = 0; i < 10; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(vrbody_engine_hidden_ik() == 0 && vrbody_get_weld(0) == 0 && vrbody_get_weld(1) == 0, "sin arma tras la retención: sin petición ni soldadura");
    world(4, 1, 4);
    CHECK(vrgame_ik_hidden_wanted(&bs) == 1, "Stinger ya cargado recién equipado: la IK se pide ANTES del primer VBlank con el arma (el apply del frame de equipar corre antes de vrgame_pad)");
    bs.in_control = 0; bs.cam_ctl = 0; CHECK(vrgame_ik_hidden_wanted(&bs) == 0, "escena: la sonda no la pide");
    bs.cinematic = 1; bs.cam_ctl = 1; CHECK(vrgame_ik_hidden_wanted(&bs) == 0, "cinemática: la sonda no la pide");
    bs.cinematic = 0; bs.in_control = 1; bs.cam_ctl = 0;
    { BridgeState b2 = bs; b2.stance = 2; CHECK(vrgame_ik_hidden_wanted(&b2) == 0, "tumbado con el Stinger (arma oculta): la sonda no la pide"); }
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(drawn_weapon(dir, org) == 1, "Stinger dibujado otra vez");
    {   /* recarga: el juego desequipa un instante y el arma vuelve. La soldadura no se suelta (ni arranca la rampa WELD_FADE_NS): la mano de apoyo no se mueve de donde estaba */
        VrHandPose h0[2], h1[2], h2[2];
        CHECK(vrpose_get_hands(h0) == 2 && h0[0].derived == 1, "mano de apoyo soldada antes de la recarga");
        world(-1, 0, 0);
        for (int i = 0; i < 3; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
        vrpose_get_hands(h1);
        CHECK(vrpose_get_hands(h1) == 2 && h1[0].derived == 1 && h1[0].pos[0] == h0[0].pos[0] && h1[0].pos[1] == h0[0].pos[1] && h1[0].pos[2] == h0[0].pos[2], "recarga (arma fuera 3 VBlanks): la mano de apoyo sigue soldada, sin moverse");
        world(4, 1, 4);
        for (int i = 0; i < 3; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
        CHECK(vrpose_get_hands(h2) == 2 && h2[0].derived == 1 && d3(h2[0].pos, h0[0].pos) < 1e-5f && vrbody_get_weld(0) == 1 && vrbody_get_weld(1) == 1 && drawn_weapon(dir, org) == 1, "el arma vuelve: soldadura intacta (%.6f m)", d3(h2[0].pos, h0[0].pos));
        world(-1, 0, 0);
        const float away[3] = {-0.5f, 1.0f, -0.3f};
        for (int i = 0; i < 40; i++) { hand_at(1, p, q); hand_at(0, away, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }      /* no vuelve: se suelta y la mano de apoyo vuelve a su mando tras la rampa */
        CHECK(vrpose_get_hands(h1) == 2 && h1[0].derived == 0 && d3(h1[0].pos, away) < 1e-4f, "sin recuperar el arma: se suelta y la mano de apoyo vuelve a su mando");
        world(4, 1, 4);
        for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    }

    float hp[3] = {org[0], org[1], org[2]};
    int aimed = 0;       /* cabeza tras el arma, sobre el eje del tubo y mirando por el: apunta a alguna distancia razonable (<= 0,5 m del origen del arma dibujado; el visor antiguo no se alcanzaba nunca por detras) */
    for (int k = 0; k <= 10 && !aimed; k++) {
        const float t = 0.05f * (float)k, hq[3] = {org[0] - dir[0] * t, org[1] - dir[1] * t, org[2] - dir[2] * t};
        head_look(hq, dir);
        for (int i = 0; i < 6; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
        if (vraa_aiming()) { aimed = 1; memcpy(hp, hq, sizeof hp); }
    }
    CHECK(aimed, "cabeza tras el arma (hasta 0,5 m) y mirando por el eje del tubo: apunta (buscador y visor)");
    float far_p[3] = {org[0] + dir[0] * 0.54f, org[1] + dir[1] * 0.54f + 0.165f, org[2] + dir[2] * 0.54f};      /* el punto del visor antiguo (WP_EYE menos 0,10 m) */
    head_look(far_p, dir);
    for (int i = 0; i < 6; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(vraa_aiming() == 0, "cabeza donde estaba el visor antiguo (0,54 m por delante del arma): ya no apunta");
    const float side[3] = {dir[2], 0, -dir[0]};
    head_look(hp, side);
    for (int i = 0; i < 6; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(vraa_aiming() == 0, "cabeza junto a la LED pero mirando de lado: no apunta");
    const float h0[3] = {0, 1.6f, 0}, f0[3] = {0, 0, -1};
    head_look(h0, f0);
}

/* ---- cambio de mano dominante con el arma soldada y la otra mano sin rastrear: el marco de la mano del arma sale del agarre soldado de la mano nueva-dominante (inválido), aunque su mando sí se rastree.
 * Nunca debe haber arma "activa" con marco nulo: el arma queda sin dibujar y sin lógica mientras dura la rampa de salida (WELD_FADE_NS) y vuelve, con pose unitaria, al terminar. ---- */
static void dominant_switch_welded(void) {
    printf("-- cambio de mano dominante con el arma soldada y la otra mano sin rastrear: nunca marco nulo --\n");
    float dir[3], org[3], p[3] = {0.2f, 1.2f, -0.3f}, q[4] = {0, 0, 0, 1};
    MvVrGameStatus st;
    CHECK(vrgrip_find(4) && vrgrip_find(4)->support_pose >= 0, "la Stinger lleva mano de apoyo (si no, no se suelda y la prueba no discrimina)");
    world(4, 1, 4);
    fresh_meshes(4);
    mv_vrgame_set_options(1, 2);
    mv_vrgame_clear_hands();
    for (int i = 0; i < 6; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    mv_vrgame_status(&st);
    CHECK(st.active && drawn_weapon(dir, org) == 1, "soldada con la derecha: arma activa y dibujada");
    mv_vrgame_set_options(0, 2);                                  /* dominante = izquierda: su mando se rastrea, el derecho (el que sostenía) no */
    MvVrHand off; memset(&off, 0, sizeof off); mv_vrgame_set_hand(1, &off);
    hand_at(0, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram);
    mv_vrgame_status(&st);
    int drawn = drawn_weapon(dir, org);
    CHECK(st.active == 0 && drawn == 0, "primer VBlank tras el cambio: sin marco de la mano no hay arma activa ni dibujada (activa %d, dibujada %d)", st.active, drawn);
    int bad = 0, back = 0;
    for (int i = 0; i < 40; i++) {                                /* 320 ms > WELD_FADE_NS */
        hand_at(0, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram);
        mv_vrgame_status(&st);
        drawn = drawn_weapon(dir, org);
        if (drawn && fabsf(sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]) - 1.0f) > 1e-3f) bad++;
        if (st.active != drawn) bad++;
        if (drawn) back = 1;
    }
    CHECK(bad == 0, "durante la rampa y después: arma activa <=> dibujada y siempre con boca unitaria (%d incoherencias)", bad);
    CHECK(back && st.active && drawn, "acabada la rampa el arma vuelve con la mano izquierda");
}

/* ---- conductos (INTRUDE): el arma no se ve ni en la mano ni en el juego; el disparo no se toca. Tumbado con un arma que no es el PSG1 también se oculta ---- */
static void duct_hides_weapon(void) {
    printf("-- conducto (tumbado): sin arma VR, sin la del juego a la vista y sin copia del juego en la mano --\n");
    float dir[3], org[3], p[3] = {0.2f, 1.2f, -0.3f}, q[4] = {0, 0, 0, 1};
    world(0, 12, 36);
    fresh_pistol_meshes();
    mv_vrgame_set_options(1, 2);
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(drawn_weapon(dir, org) == 1 && vrfire_hide_enabled() == 1, "de pie: arma VR dibujada y la del juego oculta");
    bs.stance = 1; bs.prone = 1;       /* tumbado (prone) con stance distinta de 2 y sin INTRUDE (gateo): con un arma que no es el PSG1 se oculta igual */
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(drawn_weapon(dir, org) == 0 && vrfire_hide_enabled() == 1 && vrgame_native_hand() == -1, "tumbado con la SOCOM y sin INTRUDE: el arma queda oculta (modelo VR no dibujado, sin copia en la mano)");
    bs.player_status |= BRIDGE_PS_INTRUDE;
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    MvVrGameStatus st; mv_vrgame_status(&st);
    CHECK(drawn_weapon(dir, org) == 0, "conducto: el modelo VR del arma no se dibuja");
    CHECK(vrfire_hide_enabled() == 1 && vrgame_native_hand() == -1, "conducto: el arma del juego sigue oculta (%d) y sin copia en la mano (%d)", vrfire_hide_enabled(), vrgame_native_hand());
    CHECK(st.active && st.weapon == 0, "conducto: la lógica del arma (disparo) sigue activa");
    bs.stance = 0; bs.prone = 0; bs.player_status &= ~(uint32_t)BRIDGE_PS_INTRUDE;
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(drawn_weapon(dir, org) == 1, "de pie otra vez: el arma VR vuelve a dibujarse");
    /* SOCOM con stance 2 (prone 0) y sin INTRUDE: el arma queda oculta (el término stance==2 por separado) */
    bs.stance = 2;
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(drawn_weapon(dir, org) == 0 && vrfire_hide_enabled() == 1 && vrgame_native_hand() == -1, "SOCOM con stance 2 (prone 0) y sin INTRUDE: el arma queda oculta");
    bs.stance = 0;
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    /* PSG1 (gid 9) tumbado (prone, stance distinta de 2) y sin INTRUDE: el arma VR sí se ve */
    world(9, 5, 20);
    fresh_meshes(9);
    bs.stance = 1; bs.prone = 1;
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(drawn_weapon(dir, org) == 1, "tumbado con el PSG1 y sin INTRUDE: el arma VR se dibuja");
    MvVrSpecialView sv9;
    CHECK(mv_vrgame_special_view(&sv9) == 1 && sv9.kind == MV_VIEW_SCOPE, "tumbado con el PSG1 y sin INTRUDE: la lente SCOPE se publica");
    /* PSG1 con INTRUDE y de pie (stance 0, prone 0): el conducto lo oculta igual (no depende de tumbarse); sin rifle VR no hay lente */
    bs.stance = 0; bs.prone = 0; bs.player_status |= BRIDGE_PS_INTRUDE;
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(drawn_weapon(dir, org) == 0, "PSG1 con INTRUDE (stance 0, prone 0): el arma VR no se dibuja");
    CHECK(mv_vrgame_special_view(&sv9) == 0 || sv9.kind != MV_VIEW_SCOPE, "PSG1 con INTRUDE (stance 0, prone 0): no se publica la lente");
    bs.player_status &= ~(uint32_t)BRIDGE_PS_INTRUDE;
    world(0, 12, 36);
    fresh_pistol_meshes();
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(drawn_weapon(dir, org) == 1, "SOCOM de pie y sin INTRUDE: el arma VR se dibuja (antes de activar INTRUDE)");
    /* SOCOM con INTRUDE y de pie (stance 0, prone 0): el arma queda oculta */
    bs.player_status |= BRIDGE_PS_INTRUDE;
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(drawn_weapon(dir, org) == 0 && vrfire_hide_enabled() == 1 && vrgame_native_hand() == -1, "SOCOM con INTRUDE (stance 0, prone 0): el arma queda oculta");
    bs.player_status &= ~(uint32_t)BRIDGE_PS_INTRUDE;
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    /* granada (modelo del juego en la mano): tumbado se oculta y no se lleva a la mano */
    world(2, 0, 3);
    fresh_weapon(2);
    vrgame_test_set_native(2, 1);
    for (int i = 0; i < 6; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    CHECK(vrgame_native_hand() == 1 && vrfire_hide_enabled() == 0, "granada nativa de pie: copia en la mano derecha y sin ocultar (%d)", vrgame_native_hand());
    bs.stance = 2; bs.player_status |= BRIDGE_PS_INTRUDE;
    for (int i = 0; i < 6; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    CHECK(vrgame_native_hand() == -1 && vrfire_hide_enabled() == 1, "granada nativa en conducto: sin copia en la mano (%d) y la del juego oculta (%d)", vrgame_native_hand(), vrfire_hide_enabled());
    bs.stance = 0; bs.player_status &= ~(uint32_t)BRIDGE_PS_INTRUDE;
    vrgame_test_set_native(2, 0);
    world(-1, 0, 0);
    for (int i = 0; i < 3; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
}

/* ---- PSG1 (id 9): el original lo tumba (stance 2) al sacarlo; con el rifle VR y su lente no se oculta nada (el conducto es solo para el resto de armas) ---- */
/* Instala además el arma `gid` (sin tocar la de fresh_meshes): para probar el cambio de un arma a otra */
static void install_extra(int gid) {
    static WMesh mesh2[16];
    static WVertex vtx[3] = {{{0, 0, 0}, {0, 1, 0}, {0, 0}, {255, 255, 255, 255}}, {{0.01f, 0, 0}, {0, 1, 0}, {1, 0}, {255, 255, 255, 255}}, {{0, 0.01f, 0}, {0, 1, 0}, {0, 1}, {255, 255, 255, 255}}};
    static uint16_t idx[3] = {0, 1, 2};
    const WeaponDef *d = NULL;
    for (int i = 0; i < w_registry_count; i++) if (w_registry[i] && w_registry[i]->game_weapon_id == gid) d = w_registry[i];
    int n = weapon_mesh_count(d);
    for (int i = 0; i < n; i++) mesh2[i] = (WMesh){d->mesh_names[i], vtx, 3, idx, 3, 0};
    WeaponState *st = calloc(1, d->state_size);
    st->def = d; d->init(st); st->def = d;
    weapon_bind(st, mesh2, n, d->parts, d->nparts);
    VrHandsConfig hc; vrhands_default_config(&hc); hc.grip_mode = VRH_GRIP_SOCKET;
    VrHandsRig *rig = vrhands_create(&hc);
    vrhands_bind(rig, st, NULL, 0, 0);
    vrgame_test_install(gid, (void *)d, st, rig);
}
/* La vista especial pegada al arma (lente del PSG1, pantalla LED del Nikita y del Stinger) se publica con la pose de la mano del VBlank (50 Hz), pero el modelo del arma se dibuja con la mano de AHORA (mv_host_hands, 90 Hz): sin la misma
 * compensación la pantalla se queda atrás del modelo al mover el arma y parte de ella cae detrás de las caras vecinas del tubo («se borra un poco al moverlo»). */
static void special_view_follows_hand(void) {
    printf("-- vista especial: sigue a la mano de ahora igual que el modelo del arma --\n");
    float p[3] = {0.2f, 1.2f, -0.3f}, q[4] = {0, 0, 0, 1}, p2[3] = {0.26f, 1.22f, -0.34f}, p3[3] = {0.14f, 1.17f, -0.25f}, dir[3], org0[3], org1[3];
    MvVrSpecialView a, b;
    world(0, 12, 36);
    fresh_pistol_meshes();
    install_extra(9);
    bs.weapon = 9; bs.weapon_ammo[9] = 20;
    for (int i = 0; i < 14; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(mv_vrgame_special_view(&a) == 1 && a.kind == MV_VIEW_SCOPE && drawn_weapon(dir, org0) == 1, "PSG1 listo: lente publicada y arma dibujada");
    hand_at(1, p2, q); vrgame_test_advance(2000000ull);                  /* el mando se mueve entre dos VBlanks */
    CHECK(mv_vrgame_special_view(&b) == 1 && drawn_weapon(dir, org1) == 1, "tras mover el mando: lente y arma siguen");
    float dl[3], dw[3], err = 0, mv = 0;
    for (int i = 0; i < 3; i++) { dl[i] = b.surf_pos[i] - a.surf_pos[i]; dw[i] = org1[i] - org0[i]; err += (dl[i] - dw[i]) * (dl[i] - dw[i]); mv += dw[i] * dw[i]; }
    CHECK(mv > 1e-4f && sqrtf(err) < 0.002f, "la lente se desplaza como el arma (error %.1f mm de %.1f mm)", sqrtf(err) * 1000.0f, sqrtf(mv) * 1000.0f);
    /* giro del mando entre dos VBlanks: surf_rot = dq * surf_rot del VBlank, con dq = giro del mando de ahora * inverso del de la simulación (el producto no conmuta: q_mul(sq, dq) daría otro giro) */
    const float a1 = 0.45f, q3[4] = {0.3f * sinf(a1), sinf(a1) * 0.9539392f, 0.0f, cosf(a1)};
    float q3n[4] = {q3[0], q3[1], q3[2], q3[3]};
    t_qnorm(q3n);
    hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram);      /* nuevo VBlank con el mando sin girar: la simulación vuelve a q identidad */
    MvVrSpecialView c0, c1;
    CHECK(mv_vrgame_special_view(&c0) == 1 && c0.kind == MV_VIEW_SCOPE, "PSG1 listo tras otro VBlank: lente publicada");
    hand_at(1, p, q3n); vrgame_test_advance(2000000ull);
    CHECK(mv_vrgame_special_view(&c1) == 1 && c1.kind == MV_VIEW_SCOPE, "mando girado entre VBlanks: lente publicada");
    float want[4];
    t_qmul(q3n, c0.surf_rot, want);      /* q_mul(dq, sq): dq = q3n (el mando de la simulación está sin girar) */
    const float dot = fabsf(want[0] * c1.surf_rot[0] + want[1] * c1.surf_rot[1] + want[2] * c1.surf_rot[2] + want[3] * c1.surf_rot[3]);
    float wrong[4];
    t_qmul(c0.surf_rot, q3n, wrong);
    const float dotw = fabsf(wrong[0] * c1.surf_rot[0] + wrong[1] * c1.surf_rot[1] + wrong[2] * c1.surf_rot[2] + wrong[3] * c1.surf_rot[3]);
    CHECK(dot > 0.9999f, "lente girada con el mando: surf_rot = dq * surf_rot del VBlank (producto escalar %.5f; orden contrario %.5f)", dot, dotw);
}
/* Cambio de arma (selector abierto o directo): la vista especial (lente SCOPE del PSG1) del arma que entra no se publica mientras dura el cambio, solo cuando está lista */
static void special_view_waits_on_weapon_change(void) {
    printf("-- vista especial: durante el cambio de arma no se publica la del arma que entra --\n");
    float p[3] = {0.2f, 1.2f, -0.3f}, q[4] = {0, 0, 0, 1};
    MvVrSpecialView sv;
    world(0, 12, 36);
    fresh_pistol_meshes();
    install_extra(9);
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(drawn_weapon((float[3]){0}, (float[3]){0}) == 1 && (mv_vrgame_special_view(&sv) == 0 || sv.kind != MV_VIEW_SCOPE), "la pistola equipada no publica lente");
    /* el selector de armas se abre y el cursor pasa por el PSG1 (equipado en el juego): ni un solo VBlank con su lente */
    bs.menu_open = 1; bs.menu_state = 2; bs.in_control = 0; bs.cam_ctl = 1;
    bs.weapon = 9; bs.weapon_ammo[9] = 20;
    int leaked = 0;
    for (int i = 0; i < 12; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); if (mv_vrgame_special_view(&sv) == 1 && sv.kind == MV_VIEW_SCOPE) leaked++; }
    CHECK(leaked == 0, "selector abierto con el PSG1 de paso: lente publicada en %d VBlanks", leaked);
    { MvVrGameStatus gst; mv_vrgame_status(&gst);      /* la app solo pone el disco de reserva de sight.c (mira circular grande ante la cabeza) si NO hay arma VR mostrada: aqui la hay, y sin lente */
      CHECK(gst.active == 1 && gst.weapon == 9, "PSG1 de paso por el selector: arma VR mostrada (active %d weapon %d), la app no debe poner el disco de reserva", gst.active, gst.weapon); }
    /* el selector se cierra con el PSG1 elegido: el primer VBlank (arma recién entrada) no la publica; unos VBlanks después sí */
    bs.menu_open = 0; bs.menu_state = 0; bs.in_control = 1; bs.cam_ctl = 0;
    hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram);
    CHECK(mv_vrgame_special_view(&sv) == 0 || sv.kind != MV_VIEW_SCOPE, "selector recién cerrado: el PSG1 aún no está listo, sin lente");
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(mv_vrgame_special_view(&sv) == 1 && sv.kind == MV_VIEW_SCOPE && sv.cam_valid == 1, "PSG1 ya listo: lente publicada");
    /* cambio directo (sin selector) del PSG1 a la pistola y vuelta: la lente tampoco aparece en el primer VBlank del PSG1 */
    bs.weapon = 0;
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    bs.weapon = 9;
    hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram);
    CHECK(mv_vrgame_special_view(&sv) == 0 || sv.kind != MV_VIEW_SCOPE, "cambio directo a PSG1: el primer VBlank no publica la lente");
    /* el selector abierto con el arma ya lista (sin cambiar): su vista se mantiene */
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    bs.menu_open = 1; bs.menu_state = 2; bs.in_control = 0; bs.cam_ctl = 1;
    for (int i = 0; i < 4; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(mv_vrgame_special_view(&sv) == 1 && sv.kind == MV_VIEW_SCOPE, "selector abierto sin cambiar de arma: la lente del PSG1 ya listo se mantiene");
    bs.menu_open = 0; bs.menu_state = 0; bs.in_control = 1; bs.cam_ctl = 0;
    /* recarga: el juego retira el arma un instante y vuelve la misma (la soldadura se retiene): la lente no espera otra vez */
    bs.weapon = -1;
    for (int i = 0; i < 2; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    bs.weapon = 9;
    hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram);
    CHECK(mv_vrgame_special_view(&sv) == 1 && sv.kind == MV_VIEW_SCOPE, "desequipado breve (recarga) y vuelta del mismo PSG1: la lente no espera");
    /* desequipado de verdad (pasa la retención): al volver sí espera */
    bs.weapon = -1;
    for (int i = 0; i < 12; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    bs.weapon = 9;
    hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram);
    CHECK(mv_vrgame_special_view(&sv) == 0 || sv.kind != MV_VIEW_SCOPE, "desequipado largo y vuelta del PSG1: la lente espera");
    /* desequipado y selector abierto con el cursor por el PSG1: no conserva el derecho del PSG1 de antes de desequipar */
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(mv_vrgame_special_view(&sv) == 1 && sv.kind == MV_VIEW_SCOPE, "PSG1 listo otra vez: lente publicada");
    bs.weapon = -1;
    for (int i = 0; i < 12; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    bs.menu_open = 1; bs.menu_state = 2; bs.in_control = 0; bs.cam_ctl = 1; bs.weapon = 9; bs.weapon_ammo[9] = 20;
    leaked = 0;
    for (int i = 0; i < 12; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); if (mv_vrgame_special_view(&sv) == 1 && sv.kind == MV_VIEW_SCOPE) leaked++; }
    CHECK(leaked == 0, "desequipar y pasar el cursor del selector por el PSG1: lente publicada en %d VBlanks", leaked);
    bs.menu_open = 0; bs.menu_state = 0; bs.in_control = 1; bs.cam_ctl = 0;
}

/* Stinger equipado: el panel plano del HUD del jugador (radar, vida...) SIGUE visible como con cualquier arma: hide_hud = 0 con el visor en uso o sin usar (su HUD propio es la clase WEAPON_HUD, otra capa) */
static void stinger_hud_only_on_led(void) {
    printf("-- Stinger: el panel plano del HUD sigue en el mundo --\n");
    float p[3] = {0.2f, 1.2f, -0.3f}, q[4] = {0, 0, 0, 1};
    MvVrSpecialView sv;
    world(4, 1, 4);
    fresh_meshes(4);
    mv_vrgame_set_options(1, 2);
    for (int i = 0; i < 12; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    if (mv_vrgame_special_view(&sv) == 1 && (sv.kind == MV_VIEW_AA_SCREEN || sv.kind == MV_VIEW_AA_SIGHT)) CHECK(sv.hide_hud == 0, "Stinger sin usar el visor: el panel plano sigue (hide_hud %d, kind %d)", sv.hide_hud, sv.kind);
    else printf("   (sin pieza LED en la malla mínima de la prueba: no se publica vista, comprobación de hide_hud omitida)\n");
}
/* Pantallas LED con pieza sintética: hide_hud de cada vista (Stinger y Nikita con pantalla LED: siempre 0, el radar y el resto del panel plano siguen; Nikita con pantalla grande ante la cara en vuelo: 1, el panel taparía la cámara del misil) y que con el arma oculta (INTRUDE) no se publica la vista LED
 * (la pantalla grande del vuelo, que no depende del arma, se mantiene) */
static void led_views_hide_hud(void) {
    printf("-- pantallas LED sintéticas: hide_hud y arma oculta --\n");
    float p[3] = {0.2f, 1.2f, -0.3f}, q[4] = {0, 0, 0, 1};
    MvVrSpecialView sv;
    mv_vrgame_set_missile_view(0);
    world(4, 1, 4);
    fresh_led_meshes(4);
    mv_vrgame_set_options(1, 2);
    for (int i = 0; i < 12; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(mv_vrgame_special_view(&sv) == 1 && sv.kind == MV_VIEW_AA_SCREEN, "Stinger con pieza LED: vista AA_SCREEN publicada (kind %d)", sv.kind);
    CHECK(sv.hide_hud == 0, "Stinger AA_SCREEN: el panel plano del HUD sigue (hide_hud %d)", sv.hide_hud);
    bs.player_status |= BRIDGE_PS_INTRUDE;
    for (int i = 0; i < 4; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(mv_vrgame_special_view(&sv) == 0 && sv.kind == MV_VIEW_NONE, "Stinger con INTRUDE: sin vista LED ni visor (kind %d)", sv.kind);
    bs.player_status &= ~(uint32_t)BRIDGE_PS_INTRUDE;

    world(3, 1, 4);
    fresh_led_meshes(3);
    mv_vrgame_set_options(1, 2);
    GuidedState *gs = (GuidedState *)syn_state;
    for (int i = 0; i < 12; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(mv_vrgame_special_view(&sv) == 1 && sv.kind == MV_VIEW_MISSILE && !sv.fullscreen && sv.standby == 1, "Nikita en espera: pantalla LED (kind %d fullscreen %d standby %d)", sv.kind, sv.fullscreen, sv.standby);
    CHECK(sv.hide_hud == 0, "Nikita en espera: el HUD normal sigue (hide_hud %d)", sv.hide_hud);
    for (int mode = 0; mode < 2; mode++) {
        mv_vrgame_set_missile_view(mode);
        syn_state->phase = G_FLIGHT; gs->flight_t = 0; gs->fuel = GD_FUEL_TIME; gs->seen = 0;
        hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram);
        const int got = mv_vrgame_special_view(&sv);
        CHECK(got == 1 && sv.kind == MV_VIEW_MISSILE && sv.fullscreen == mode && sv.standby == 0, "Nikita en vuelo (pantalla grande %d): vista MISSILE (kind %d fullscreen %d)", mode, sv.kind, sv.fullscreen);
        CHECK(sv.hide_hud == (mode ? 1 : 0), "Nikita en vuelo (pantalla grande %d): hide_hud %d (solo la pantalla grande oculta el panel)", mode, sv.hide_hud);
        bs.player_status |= BRIDGE_PS_INTRUDE;
        syn_state->phase = G_FLIGHT; gs->flight_t = 0; gs->fuel = GD_FUEL_TIME; gs->seen = 0;
        hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram);
        const int got2 = mv_vrgame_special_view(&sv);
        if (mode) CHECK(got2 == 1 && sv.kind == MV_VIEW_MISSILE && sv.fullscreen == 1 && sv.hide_hud == 1, "Nikita en vuelo con INTRUDE y pantalla grande: la cámara del misil se mantiene (kind %d)", sv.kind);
        else CHECK(got2 == 0 && sv.kind == MV_VIEW_NONE, "Nikita en vuelo con INTRUDE y pantalla en el lanzador: sin vista LED (kind %d)", sv.kind);
        bs.player_status &= ~(uint32_t)BRIDGE_PS_INTRUDE;
    }
    mv_vrgame_set_missile_view(0);
}
/* La pantalla pegada al arma debe moverse con la MISMA mano con la que mv_host_hands colocó el modelo en el fotograma: entre mv_vrgame_special_view (inicio del fotograma de la app) y el dibujo del arma pasan varios ms y el mando
 * se ha movido, así que sin mv_vrgame_special_sync la pantalla queda atrás del modelo (hundida tras sus caras, «se borra al moverla»). */
static void special_view_synced_to_drawn_model(void) {
    printf("-- vista especial: reajustada a la mano con la que se dibujó el modelo --\n");
    float p[3] = {0.2f, 1.2f, -0.3f}, q[4] = {0, 0, 0, 1}, p2[3] = {0.26f, 1.22f, -0.34f}, p3[3] = {0.14f, 1.17f, -0.25f}, dir[3], org0[3], org1[3];
    MvVrSpecialView a, b;
    world(0, 12, 36);
    fresh_pistol_meshes();
    install_extra(9);
    bs.weapon = 9; bs.weapon_ammo[9] = 20;
    for (int i = 0; i < 14; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(mv_vrgame_special_view(&a) == 1 && a.kind == MV_VIEW_SCOPE && drawn_weapon(dir, org0) == 1, "PSG1 listo: lente publicada y arma dibujada");
    MvVrSpecialView early;
    hand_at(1, p2, q); vrgame_test_advance(2000000ull);
    CHECK(mv_vrgame_special_view(&early) == 1, "vista leida al inicio del fotograma");      /* la app la lee aqui... */
    hand_at(1, p3, q); vrgame_test_advance(6000000ull);                                        /* ...y el mando sigue moviendose hasta el dibujo del arma (6 ms despues), a una pose DISTINTA de la simulada (D no identidad) */
    CHECK(drawn_weapon(dir, org1) == 1, "arma dibujada con la mano de ese instante");
    b = early;
    CHECK(mv_vrgame_special_sync(&b) == 1, "sync: la vista coincide con la publicada");
    float dl[3], dw[3], err = 0, err_early = 0, mv = 0;
    for (int i = 0; i < 3; i++) {
        dl[i] = b.surf_pos[i] - a.surf_pos[i]; dw[i] = org1[i] - org0[i];
        err += (dl[i] - dw[i]) * (dl[i] - dw[i]); mv += dw[i] * dw[i];
        const float de = early.surf_pos[i] - a.surf_pos[i];
        err_early += (de - dw[i]) * (de - dw[i]);
    }
    CHECK(sqrtf(err_early) > 0.02f, "sin sync la pantalla quedaria %.1f mm fuera del modelo (mas que los 6 mm de adelanto de la LED)", sqrtf(err_early) * 1000.0f);
    CHECK(sqrtf(mv) > 0.05f, "el modelo se dibujo con una D no identidad (mov. del arma %.1f mm)", sqrtf(mv) * 1000.0f);
    CHECK(sqrtf(err) < 0.0005f, "con sync la pantalla sigue al modelo dibujado (error %.2f mm, mov. del arma %.1f mm)", sqrtf(err) * 1000.0f, sqrtf(mv) * 1000.0f);
    {   /* hubo un VBlank (nueva publicacion) entre mv_host_hands y el sync: la D guardada es de la mano de la publicacion anterior, no se mezcla con la base vigente (la vista no se toca) */
        MvVrSpecialView late = early;
        vrgame_test_advance(2000000ull); vrgame_pad(0, &bs, ram);
        CHECK(mv_vrgame_special_sync(&late) == 0 && memcmp(late.surf_pos, early.surf_pos, sizeof late.surf_pos) == 0 && memcmp(late.surf_rot, early.surf_rot, sizeof late.surf_rot) == 0,
              "sync: con otra publicacion vigente que la de la D guardada no se aplica y la vista queda intacta");
    }
    MvVrSpecialView other = early;
    other.surf_w *= 1.5f;      /* una vista de la app (prismaticos...) no se toca */
    float keep[3]; memcpy(keep, other.surf_pos, sizeof keep);
    CHECK(mv_vrgame_special_sync(&other) == 0 && memcmp(keep, other.surf_pos, sizeof keep) == 0, "sync: otra vista (otro tamano) intacta");
}
/* HUD propio del Nikita: el juego solo emite combustible / cifras / ENEMY con el boton triangulo mantenido (rmissile.c, vista en 1a persona), que aqui nunca se pulsa; la app los dibuja (nik_hud, nik_fuel, nik_pos) con la formula del juego */
static void nikita_native_hud(void) {
    printf("-- Nikita en vuelo: HUD propio (combustible y posicion del misil) --\n");
    float p[3] = {0.2f, 1.2f, -0.3f}, q[4] = {0, 0, 0, 1};
    MvVrSpecialView sv;
    mv_vrgame_set_missile_view(0);
    world(3, 1, 4);
    fresh_led_meshes(3);
    mv_vrgame_set_options(1, 2);
    GuidedState *gs = (GuidedState *)syn_state;
    const uint32_t actor = 0x80100000u, ao = actor & 0x1FFFFFu;
    uint8_t saved[0x328];
    memcpy(saved, ram + ao, sizeof saved);
    memset(ram + ao, 0, sizeof saved);
    hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram);      /* primer VBlank: el juego fija el nº de misil visto; el actor se anota despues */
    vrmis_reset();
    vrmis_begin(actor);
    for (int i = 0; i < 12; i++) {
        const int16_t age = (int16_t)(250 + i * 2), pos[3] = {1234, -567, 8901};
        memcpy(ram + ao + 0x118, &age, 2);
        memcpy(ram + ao + 0x20, pos, sizeof pos);
        memcpy(ram + ao + 0x108, pos, sizeof pos);
        syn_state->phase = G_FLIGHT; gs->flight_t = 0; gs->fuel = GD_FUEL_TIME; gs->seen = 0;
        hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram);
    }
    CHECK(mv_vrgame_special_view(&sv) == 1 && sv.kind == MV_VIEW_MISSILE && sv.standby == 0, "Nikita en vuelo con misil vivo (kind %d standby %d)", sv.kind, sv.standby);
    CHECK(sv.nik_hud == 1, "HUD del Nikita pedido (nik_hud %d)", sv.nik_hud);
    CHECK(sv.nik_fuel == (float)(((1000 - 272) * 60) / 1000), "barra de combustible = (1000 - edad) * 60 / 1000 = 43 px (%.1f)", sv.nik_fuel);
    CHECK(sv.nik_pos[0] == 1234.0f && sv.nik_pos[1] == -567.0f && sv.nik_pos[2] == 8901.0f, "cifras = posicion del misil (%.0f %.0f %.0f)", sv.nik_pos[0], sv.nik_pos[1], sv.nik_pos[2]);
    vrmis_reset();
    syn_state->phase = G_IDLE;
    for (int i = 0; i < 6; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(mv_vrgame_special_view(&sv) == 1 && sv.nik_hud == 0, "sin misil vivo no hay HUD propio (nik_hud %d)", sv.nik_hud);
    memcpy(ram + ao, saved, sizeof saved);
}
/* Vibración del juego por mando: el pequeño a la derecha y el grande a la izquierda; con el PSG1 mostrado el grande (latido del corazón al apuntar) también a la derecha */
static void rumble_routing(void) {
    printf("-- vibración: latido del PSG1 en las dos manos --\n");
    float p[3] = {0.2f, 1.2f, -0.3f}, q[4] = {0, 0, 0, 1}, l, r;
    world(0, 12, 36);
    fresh_pistol_meshes();
    for (int i = 0; i < 12; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    mv_vrgame_route_rumble(0, 255, &l, &r);
    CHECK(l == 1.0f && r == 0.0f, "pistola: el motor grande solo vibra a la izquierda (%.2f %.2f)", l, r);
    mv_vrgame_route_rumble(128, 0, &l, &r);
    CHECK(l == 0.0f && fabsf(r - 128.0f / 255.0f) < 1e-6f, "pistola: el pequeño solo a la derecha (%.2f %.2f)", l, r);
    world(9, 5, 20);
    fresh_meshes(9);
    for (int i = 0; i < 12; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    mv_vrgame_route_rumble(0, 196, &l, &r);
    CHECK(fabsf(l - 196.0f / 255.0f) < 1e-6f && r == l, "PSG1: el latido (motor grande) vibra en las dos manos (%.3f %.3f)", l, r);
    mv_vrgame_route_rumble(255, 100, &l, &r);
    CHECK(r == 1.0f && fabsf(l - 100.0f / 255.0f) < 1e-6f, "PSG1: el pequeño más fuerte se conserva a la derecha (%.3f %.3f)", l, r);
    mv_vrgame_route_rumble(0, 0, &l, &r);
    CHECK(l == 0.0f && r == 0.0f, "PSG1 sin vibración: nada");
    mv_vrgame_route_rumble(0, 90, NULL, &r); mv_vrgame_route_rumble(0, 90, &l, NULL);
}
/* ---- mina: se apoya en el suelo por las patas (modelo 0x17a1: el punto más bajo del modelo real, medido con wload y dibujado con la matriz del juego) y no se coloca sin suelo; el fantasma queda fijo en el suelo (sin la compensación de latencia de la mano) ---- */
static int mine_try(float pitch_deg, VrFireMat *m) {
    { VrFireMat dm; for (int n = 0; n < 8 && vrfire_take_projectile(2, &dm) == VRFIRE_SUBST; n++) {} }
    fresh_weapon(5);
    for (int i = 0; i < 70; i++) c4vbl(pitch_deg, 0.0f);          /* las patas tardan 0,4 s en desplegarse (w_placed.c LEGS_TIME) */
    for (int i = 0; i < 6; i++) c4vbl(pitch_deg, 1.0f);
    MvVrGameStatus st0, st; mv_vrgame_status(&st0);
    for (int i = 0; i < 4; i++) c4vbl(pitch_deg, 0.0f);
    mv_vrgame_status(&st);
    if (st.places == st0.places) return 0;
    return vrfire_take_projectile(2, m) == VRFIRE_SUBST;
}
static int ghost_verts(float lo[3], float hi[3]) {
    VrHandsFrame f; int found = 0;
    if (mv_host_hands(&f)) for (int i = 0; i < f.nmeshes; i++) {
        const VrHandsMesh *m = &f.meshes[i];
        if (m->mesh_id != WGHOST_MESH_ID) continue;
        found = 1;
        for (int k = 0; k < 3; k++) { lo[k] = 1e9f; hi[k] = -1e9f; }
        for (uint32_t v = 0; v < (uint32_t)m->nverts; v++) for (int k = 0; k < 3; k++) { if (m->verts[v].pos[k] < lo[k]) lo[k] = m->verts[v].pos[k]; if (m->verts[v].pos[k] > hi[k]) hi[k] = m->verts[v].pos[k]; }
    }
    mv_host_hands_done();
    return found;
}
/* modelo REAL 0x17a1 (marco del motor sin rotar, uds = mm) del archivo de stages del usuario; 0 si no hay datos del juego (la comprobación con la medida real se omite) */
static int mine_model(WLoaded *out) {
    const char *cands[3] = {getenv("STAGE_FILE"), "/tmp/gamedata/STAGE.DIR", "gamedata/stages.bin"};
    FILE *f = NULL;
    for (int i = 0; i < 3 && !f; i++) if (cands[i]) f = fopen(cands[i], "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 ? (uint8_t *)malloc((size_t)n) : NULL;
    const int rd = d && fread(d, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    WLoadOpts o; wload_default_opts(&o);
    o.frame = W_FRAME_ENGINE; o.scale = 1.0f; o.resident = "init";
    const int ok = rd && wload_model(d, (size_t)n, "init", 0x17a1, &o, out) == 0;
    free(d);
    return ok;
}
static void mine_rests_on_floor(void) {
    printf("-- mina: apoyada en el suelo por las patas --\n");
    WLoaded mm;
    const int have = mine_model(&mm);
    if (!have) printf("   sin datos del juego (STAGE.DIR): la altura de las patas no se mide con el modelo real (omitido)\n");
    world(5, 0, 4);
    bs.ctl_pos.y = -2500;                                       /* suelo del juego a 0,9 m bajo el ojo del motor */
    VrFireMap map; vrfire_map_from_cam(&map, &rec);
    const float y0 = g_head[13] - (float)(map.eye[1] - bs.ctl_pos.y) / map.units_per_m;
    ray_floor_u = y0;                                           /* la geometría de render tiene el suelo a la altura de los pies */
    int accepted = 0;
    for (int pitch = -60; pitch <= 90; pitch += 15) {
        VrFireMat m;
        if (!mine_try((float)pitch, &m)) continue;
        accepted++;
        if (have) {      /* el punto más bajo del modelo real dibujado con la matriz que recibe el juego (columnas de VrFireMat: izquierda, -delante, arriba) */
            float low = 1e9f;
            for (uint32_t k = 0; k < mm.nv; k++) {
                const float *v = mm.v[k].pos; float w[3], uv[3];
                for (int i = 0; i < 3; i++) w[i] = (float)m.t[i] + ((float)m.m[3 * i] * v[0] + (float)m.m[3 * i + 1] * v[1] + (float)m.m[3 * i + 2] * v[2]) / 4096.0f;
                vrfire_pos_from_world(&map, w, uv);
                if (uv[1] < low) low = uv[1];
            }
            CHECK(fabsf(low - y0) <= 0.01f, "inclinación %d: el punto más bajo del modelo real de la mina (%.3f) queda a %.3f del suelo (%.3f)", pitch, low, low - y0, y0);
        }
    }
    CHECK(accepted >= 4, "se colocó con %d de 11 inclinaciones (mano horizontal y hacia el suelo)", accepted);
    if (have) wload_free(&mm);
    /* sin suelo conocido (pies fuera de rango): no se puede colocar */
    ray_floor_u = -1e9f;
    bs.ctl_pos.y = 0;
    MvVrGameStatus s0, s1; mv_vrgame_status(&s0);
    VrFireMat m;
    int any = 0;
    for (int pitch = -60; pitch <= 90; pitch += 30) any += mine_try((float)pitch, &m);
    mv_vrgame_status(&s1);
    CHECK(any == 0 && s1.places == s0.places && s1.rejects > s0.rejects, "sin suelo: ninguna colocación (colocadas %u, rechazos %u)", s1.places - s0.places, s1.rejects - s0.rejects);
    bs.ctl_pos.y = 0;
}
/* mina: el suelo es el REAL bajo el punto de colocación (rayo hacia abajo contra la geometría sólida), no el plano de los pies: en un escalón o una escalera no queda en el aire ni enterrada */
static void mine_real_floor(void) {
    printf("-- mina: suelo real bajo el punto (escalón y escalera) --\n");
    world(5, 0, 4);
    bs.ctl_pos.y = -2500;
    VrFireMap map; vrfire_map_from_cam(&map, &rec);
    const float upm = map.units_per_m, feet = g_head[13] - (float)(map.eye[1] - bs.ctl_pos.y) / upm;
    const float dz[3] = {0.30f, -0.40f, 0.0f};      /* suelo real respecto a los pies: escalón arriba, escalera abajo, igual */
    int accepted = 0;
    for (int k = 0; k < 3; k++) for (int pitch = -60; pitch <= 0; pitch += 30) {
        VrFireMat m;
        const float real = feet + dz[k];
        ray_stub_mm = 0.0f;      /* primer intento solo para conocer la pose de la mano (impacto a 0 mm: suelo = altura de partida del rayo) */
        if (!mine_try((float)pitch, &m)) { ray_stub_mm = -1; continue; }
        const float top = w_launch_last()->pose.p.y, y0 = top > feet + 0.5f ? top : feet + 0.5f;
        ray_stub_mm = (y0 - real) * upm;
        ray_log_n = 0;
        if (!mine_try((float)pitch, &m)) { ray_stub_mm = -1; CHECK(0, "con suelo real %+.2f m y inclinación %d la mina se rechazó", dz[k], pitch); continue; }
        float w[3] = {(float)m.t[0], (float)m.t[1], (float)m.t[2]}, uv[3];
        vrfire_pos_from_world(&map, w, uv);
        const float want = real + WGHOST_MINE_LEGS_U / upm;
        CHECK(fabsf(uv[1] - want) <= 0.01f, "suelo real %+.2f m sobre los pies, inclinación %d: la mina queda a %.3f (esperado %.3f, plano de los pies %.3f)", dz[k], pitch, uv[1], want, feet + WGHOST_MINE_LEGS_U / upm);
        int down = 0;      /* rayo de place_floor_y: desde la mano (o pies + PLACE_FLOOR_STEP_M si la mano está más baja) hacia abajo (0,-1,0) */
        for (int r = 0; r < ray_log_n; r++) {
            float uo[3], ud[3];
            vrfire_pos_from_world(&map, ray_log_o[r], uo); vrfire_dir_from_world(&map, ray_log_d[r], ud);
            if (fabsf(ud[0]) < 1e-3f && ud[1] < -0.999f && fabsf(ud[2]) < 1e-3f && fabsf(uo[1] - y0) < 0.02f) down++;
        }
        CHECK(down >= 1, "suelo real %+.2f m, inclinación %d: rayo hacia abajo desde y0 %.3f (rayos pedidos %d, bajo ese origen %d)", dz[k], pitch, y0, ray_log_n, down);
        accepted++;
    }
    ray_stub_mm = -1;
    CHECK(accepted >= 6, "se colocó con %d de 9 combinaciones", accepted);
    bs.ctl_pos.y = 0;
}
/* orientación de la mina colocada: la cara picuda (+X del modelo 0x17a1) mira hacia donde apunta el jugador (guiñada del delante de la mano), sea cual sea esa guiñada */
static void mine_orientation(void) {
    printf("-- mina: orientación (cara picuda hacia delante) --\n");
    world(5, 0, 4);
    bs.ctl_pos.y = -2500;
    VrFireMap map; vrfire_map_from_cam(&map, &rec);
    int n = 0;
    ray_floor_u = g_head[13] - (float)(map.eye[1] - bs.ctl_pos.y) / map.units_per_m;
    const float yaws[5] = {0.0f, 60.0f, -120.0f, 170.0f, 250.0f};
    for (int k = 0; k < 5; k++) for (int pitch = -30; pitch <= 0; pitch += 30) {
        c4_yaw_deg = yaws[k];
        VrFireMat m;
        if (!mine_try((float)pitch, &m)) continue;
        const WLaunch *ev = w_launch_last();
        const float qx = ev->pose.q.x, qy = ev->pose.q.y, qz = ev->pose.q.z, qw = ev->pose.q.w;
        float fx = -2 * (qx * qz + qw * qy), fz = -(1 - 2 * (qx * qx + qy * qy));
        const float fl = sqrtf(fx * fx + fz * fz);
        /* columna 0 de la matriz del juego = izquierda del objeto colocado (arriba x delante): su +X (cara picuda) es el opuesto */
        float cx[3] = {-(float)m.m[0] / 4096.0f, -(float)m.m[3] / 4096.0f, -(float)m.m[6] / 4096.0f}, ux[3];
        vrfire_dir_from_world(&map, cx, ux);
        const float ul = sqrtf(ux[0] * ux[0] + ux[2] * ux[2]);
        CHECK(fl > 0.1f && ul > 0.5f && (ux[0] * fx + ux[2] * fz) / (ul * fl) > 0.98f, "guiñada %.0f inclinación %d: +X del objeto (%.2f %.2f) mira al delante de la mano (%.2f %.2f)", yaws[k], pitch, ux[0], ux[2], fx / fl, fz / fl);
        n++;
    }
    c4_yaw_deg = 0.0f;
    ray_floor_u = -1e9f;
    CHECK(n >= 6, "se colocó con %d de 10 combinaciones de guiñada e inclinación", n);
    bs.ctl_pos.y = 0;
}
static void mine_ghost_fixed(void) {
    printf("-- mina: fantasma fijo en el suelo --\n");
    world(5, 0, 4);
    bs.ctl_pos.y = -2500;
    VrFireMap map; vrfire_map_from_cam(&map, &rec);
    const float y0 = g_head[13] - (float)(map.eye[1] - bs.ctl_pos.y) / map.units_per_m;
    fresh_meshes(5);
    ray_floor_u = y0;
    float p[3] = {0.2f, 1.2f, -0.3f}, q[4] = {0, 0, 0, 1};
    { float a = -45.0f * 0.017453292f * 0.5f; q[0] = sinf(a); q[3] = cosf(a); }
    for (int i = 0; i < 80; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    float lo[3], hi[3], lo2[3], hi2[3];
    CHECK(ghost_verts(lo, hi) == 1, "mina en la mano con suelo: hay fantasma");
    if (ghost_verts(lo, hi)) {
        CHECK(lo[1] >= y0 - 1e-3f && hi[1] <= y0 + WGHOST_LIFT_M + 1e-3f, "fantasma anclado al suelo (y %.3f..%.3f, suelo %.3f)", lo[1], hi[1], y0);
        float p2[3] = {p[0] + 0.1f, p[1] + 0.05f, p[2] - 0.08f};      /* la mano se mueve tras la simulación: el fantasma no la sigue */
        hand_at(1, p2, q);
        CHECK(ghost_verts(lo2, hi2) == 1 && fabsf(lo2[0] - lo[0]) < 1e-5f && fabsf(lo2[1] - lo[1]) < 1e-5f && fabsf(lo2[2] - lo[2]) < 1e-5f && fabsf(hi2[0] - hi[0]) < 1e-5f && fabsf(hi2[2] - hi[2]) < 1e-5f,
              "fantasma sin compensación de latencia (x %.4f -> %.4f, z %.4f -> %.4f)", lo[0], lo2[0], lo[2], lo2[2]);
    }
    /* sin impacto del rayo (geometría no disponible): ni fantasma verde ni colocación (antes caía al plano de los pies y la mina quedaba en el aire) */
    ray_floor_u = -1e9f;
    fresh_meshes(5);
    for (int i = 0; i < 80; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(ghost_verts(lo, hi) == 0, "sin impacto del rayo no hay fantasma");
    bs.ctl_pos.y = 0;
}
/* mina sin impacto del rayo (suelo de los pies conocido, geometría de render ausente): se rechaza en todas las inclinaciones, el arma no gasta nada */
static void mine_no_ray_hit_rejects(void) {
    printf("-- mina: sin impacto del rayo no se coloca --\n");
    world(5, 0, 4);
    bs.ctl_pos.y = -2500;
    ray_floor_u = -1e9f; ray_stub_mm = -1;
    MvVrGameStatus s0, s1; mv_vrgame_status(&s0);
    VrFireMat m; int any = 0;
    for (int pitch = -60; pitch <= 90; pitch += 30) any += mine_try((float)pitch, &m);
    mv_vrgame_status(&s1);
    CHECK(any == 0 && s1.places == s0.places && s1.rejects > s0.rejects, "sin impacto: ninguna colocación (colocadas %u, rechazos %u)", s1.places - s0.places, s1.rejects - s0.rejects);
    bs.ctl_pos.y = 0;
}
static void psg1_prone_keeps_weapon(void) {
    printf("-- PSG1 tumbado: el rifle VR se construye y la lente se publica --\n");
    float dir[3], org[3], p[3] = {0.2f, 1.2f, -0.3f}, q[4] = {0, 0, 0, 1};
    MvVrSpecialView sv;
    world(9, 5, 20);
    fresh_meshes(9);
    mv_vrgame_set_options(1, 2);
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(drawn_weapon(dir, org) == 1 && vrfire_hide_enabled() == 1, "PSG1 de pie: rifle VR dibujado y el del juego oculto");
    bs.stance = 2;
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(drawn_weapon(dir, org) == 1, "PSG1 con stance 2: el rifle VR sigue dibujado (si no, la lente flotaría sin rifle)");
    CHECK(vrfire_hide_enabled() == 1, "PSG1 con stance 2: el rifle del juego sigue oculto");
    CHECK(mv_vrgame_special_view(&sv) == 1 && sv.kind == MV_VIEW_SCOPE && sv.occlude == 1 && sv.surf_w > 0.0f && sv.tex_w == 1024,
          "PSG1 con stance 2: lente publicada (kind %d occlude %d surf_w %.3f tex %d)", sv.kind, sv.occlude, sv.surf_w, sv.tex_w);
    CHECK(sv.cam_valid == 1 && sv.blend == 1.0f && sv.hide_hud == 0, "lente viva: cam_valid %d blend %.2f hide_hud %d", sv.cam_valid, sv.blend, sv.hide_hud);
    bs.stance = 0; bs.prone = 1;
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(drawn_weapon(dir, org) == 1, "PSG1 prone: el rifle VR sigue dibujado");
    /* los prismáticos del juego sí lo ocultan (el aumento va en el mundo) y entonces tampoco hay lente */
    vrgame_set_view_hide(1);
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(drawn_weapon(dir, org) == 0, "PSG1 con prismáticos: el rifle VR no se dibuja");
    CHECK(mv_vrgame_special_view(&sv) == 0 || sv.kind != MV_VIEW_SCOPE, "PSG1 con prismáticos: no se publica la lente (flotaría sin rifle)");
    vrgame_set_view_hide(0);
    bs.prone = 0;
    /* la pistola en un conducto sigue ocultándose y su vista especial no es una lente */
    world(0, 12, 36);
    fresh_pistol_meshes();
    bs.stance = 2; bs.player_status |= BRIDGE_PS_INTRUDE;
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(drawn_weapon(dir, org) == 0, "pistola en conducto: sigue oculta");
    CHECK(mv_vrgame_special_view(&sv) == 0 || sv.kind != MV_VIEW_SCOPE, "la pistola no publica lente SCOPE");
    bs.stance = 0; bs.player_status &= ~(uint32_t)BRIDGE_PS_INTRUDE;
    world(-1, 0, 0);
    for (int i = 0; i < 3; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
}

/* ---- prismáticos del juego (scope.c, aumento en el mundo nativo): sin arma VR ni copia del juego en la mano; el disparo no se toca ---- */
static void binocular_hides_weapon(void) {
    printf("-- prismaticos: sin arma VR, sin la del juego a la vista y sin copia del juego en la mano --\n");
    float dir[3], org[3], p[3] = {0.2f, 1.2f, -0.3f}, q[4] = {0, 0, 0, 1};
    world(0, 12, 36);
    fresh_pistol_meshes();
    mv_vrgame_set_options(1, 2);
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(drawn_weapon(dir, org) == 1 && vrfire_hide_enabled() == 1, "sin prismaticos: arma VR dibujada");
    vrgame_set_view_hide(1);
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    MvVrGameStatus st; mv_vrgame_status(&st);
    CHECK(drawn_weapon(dir, org) == 0, "prismaticos: el modelo VR del arma no se dibuja");
    CHECK(vrfire_hide_enabled() == 1 && vrgame_native_hand() == -1, "prismaticos: el arma del juego sigue oculta (%d) y sin copia en la mano (%d)", vrfire_hide_enabled(), vrgame_native_hand());
    CHECK(st.active && st.weapon == 0, "prismaticos: la logica del arma sigue activa");
    vrgame_set_view_hide(0);
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(drawn_weapon(dir, org) == 1, "sin prismaticos otra vez: el arma VR vuelve a dibujarse");
    world(2, 0, 3);
    fresh_weapon(2);
    vrgame_test_set_native(2, 1);
    for (int i = 0; i < 6; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    CHECK(vrgame_native_hand() == 1 && vrfire_hide_enabled() == 0, "granada nativa sin prismaticos: copia en la mano (%d)", vrgame_native_hand());
    vrgame_set_view_hide(1);
    for (int i = 0; i < 6; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    CHECK(vrgame_native_hand() == -1 && vrfire_hide_enabled() == 1, "granada nativa con prismaticos: sin copia en la mano (%d) y la del juego oculta (%d)", vrgame_native_hand(), vrfire_hide_enabled());
    vrgame_set_view_hide(0);
    vrgame_test_set_native(2, 0);
    world(-1, 0, 0);
    for (int i = 0; i < 3; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
}

/* ---- granadas: modelo por id de juego y dibujo VR / nativo ---- */
static void grenade_models(void) {
    printf("-- granadas (ids 2, 7, 8): modelo de grenade.c por id, dibujada siempre con el del juego --\n");
    CHECK(vrgame_weapon_model(2) == 0x3b88, "granada de fragmentación (id 2): modelo %04x", vrgame_weapon_model(2));
    CHECK(vrgame_weapon_model(8) == 0x7a64, "señuelo (id 8): can_gren, modelo %04x (grenade.c: GRD_CHAFF -> can_gren)", vrgame_weapon_model(8));
    CHECK(vrgame_weapon_model(7) == 0x7a64, "aturdidora (id 7): can_gren, modelo %04x (grenade.c: GRD_STUN -> can_gren)", vrgame_weapon_model(7));
    CHECK(vrgame_weapon_model(7) == vrgame_weapon_model(8) && vrgame_weapon_model(2) != vrgame_weapon_model(8) && vrgame_weapon_model(2) != vrgame_weapon_model(0), "aturdidora y señuelo comparten bote; la de fragmentación difiere de ellos y de la pistola");
    CHECK(vrgame_weapon_model(0) == 0x36f6 && vrgame_weapon_model(5) == 0x17a1 && vrgame_weapon_model(99) == 0 && vrgame_weapon_model(-1) == 0, "pistola, mina y ids fuera de tabla");
    /* política de modelo: no hay tabla de corte ni textura VR fiable para los modelos de granada (el antiguo de la 8, 0x761a, salía blanco
     * mientras el juego lanzaba otro): toda granada (2, 7, 8) se dibuja con su modelo real del juego; las demás armas, con el VR */
    for (int g = 2; g <= 8; g++) {
        int gren = g == 2 || g == 7 || g == 8;
        CHECK(vrgame_weapon_uses_game_model(g) == gren, "id %d: modelo %s", g, gren ? "real del juego (nunca el VR dudoso/blanco)" : "VR");
    }
    CHECK(!vrgame_weapon_uses_game_model(0) && !vrgame_weapon_uses_game_model(9) && !vrgame_weapon_uses_game_model(-1), "pistola, francotirador y fuera de tabla: sin cambio");
    /* dibujo: con el modelo VR completo el arma del juego se oculta y las mallas VR se publican; con textura incompleta se dibuja la del juego (nativa) y NO se oculta */
    world(2, 0, 3);
    fresh_weapon(2);
    for (int i = 0; i < 6; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    CHECK(vrfire_hide_enabled() == 1, "granada VR con textura completa: el arma del juego se oculta");
    vrgame_test_set_native(2, 1);
    for (int i = 0; i < 6; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    MvVrGameStatus st; mv_vrgame_status(&st);
    CHECK(st.active && st.weapon == 2, "con textura incompleta la lógica VR de la granada sigue activa");
    CHECK(vrfire_hide_enabled() == 0, "con textura incompleta el modelo del juego (escena nativa) NO se oculta");
    VrHandsFrame f;
    CHECK(mv_host_hands(&f) == 0, "con textura incompleta no se publica malla VR (sería blanca)");
    mv_host_hands_done();
    /* la granada del juego cuelga de la mano que empuña el arma: el anfitrión la lleva a esa mano (ns_wphide_carry); el modelo VR y sin arma no */
    mv_vrgame_set_options(1, 2);        /* diestro (pruebas anteriores dejaron el mando izquierdo) */
    for (int i = 0; i < 3; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    CHECK(vrgame_native_hand() == 1, "granada nativa en la mano derecha: lado %d", vrgame_native_hand());
    vrgame_test_set_native(2, 0);
    for (int i = 0; i < 3; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    CHECK(vrgame_native_hand() == -1, "arma con modelo VR: ninguna mano con objeto del juego (%d)", vrgame_native_hand());
    vrgame_test_set_native(2, 1);
    for (int i = 0; i < 3; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    CHECK(vrgame_native_hand() == 1, "granada nativa otra vez en la mano derecha");
    world(-1, 0, 0);
    for (int i = 0; i < 3; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    CHECK(vrgame_native_hand() == -1, "sin arma equipada: sin objeto en la mano (%d)", vrgame_native_hand());
    vrgame_test_set_native(2, 0);
    /* granada 7 (aturdidora): sin modelo VR pero con ranura de lógica: gesto, mano y trayectoria como 2 y 8, dibujada con el modelo del juego */
    CHECK(vrgame_test_load_logic(7) == 1 && vrgame_test_load_logic(2) == 1 && vrgame_test_load_logic(8) == 1 && vrgame_test_load_logic(0) == 0 && vrgame_test_load_logic(99) == 0, "ranura solo de lógica: las granadas 2, 7 y 8 (dibujadas por el juego, sin carga de disco) sí; la pistola (modelo VR) y un id fuera de tabla no");
    world(7, 0, 3);
    vrgame_reset();
    vrgame_set_enabled(0, 1);
    mv_vrgame_set_options(1, 2);
    sq_seen = 0;
    for (int i = 0; i < 6; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    MvVrGameStatus s7; mv_vrgame_status(&s7);
    CHECK(s7.active && s7.weapon == 7, "granada 7 con la lógica VR activa");
    CHECK(vrgame_native_hand() == 1 && vrfire_hide_enabled() == 0, "granada 7: modelo del juego en la mano derecha y sin ocultarlo (mano %d)", vrgame_native_hand());
    const uint32_t rj7 = s7.rejects;      /* acumulado: el adhesivo sin superficie de pruebas anteriores también cuenta */
    for (int i = 0; i < 10; i++) gvbl(0.2f, 0.0f, 1.0f, 0);
    for (int i = 0; i < 10; i++) gvbl(0.2f + 0.045f * (float)i, 0.0f, 1.0f, 0);
    gvbl(0.2f + 0.045f * 10, 0.0f, 0.6f, 0); gvbl(0.2f + 0.045f * 11, 0.0f, 0.1f, 0);
    for (int i = 0; i < 4; i++) gvbl(0.2f + 0.045f * 11, 0.0f, 0.0f, 0);
    mv_vrgame_status(&s7);
    CHECK(s7.throws >= 1 && s7.rejects == rj7, "granada 7: soltar el gatillo lanza con el gesto (%u lanzamientos, %u rechazos)", s7.throws, s7.rejects);
    world(-1, 0, 0);
    for (int i = 0; i < 3; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
}

/* ---- marco de la mano del mando: el de la mano propia en la escena y el del arma son el mismo, y se mueven igual ---- */
static void hand_frames(void) {
    printf("-- marco de la mano: ahora y simulado --\n");
    float p0[3], q0[4], p1[3], q1[4], ps[3], qs[4];
    mv_vrgame_clear_hands(); mv_vrgame_set_options(1, 2);
    world(0, 12, 48);
    fresh_pistol();
    gvbl(0.2f, 0.0f, 0.0f, 0);
    CHECK(vrgame_hand_frame_now(1, p0, q0) && vrgame_hand_frame_now(0, ps, qs) == 1 && !vrgame_hand_frame_now(2, ps, qs) && !vrgame_hand_frame_now(1, NULL, qs), "ahora: la mano que empuña y la de apoyo soldada (sin seguimiento propio)");
    CHECK(!vrgame_hand_frame_sim(1, ps, qs) && !vrgame_hand_frame_sim(0, ps, qs) && !vrgame_hand_frame_sim(-1, ps, qs), "simulado: sin IK del cuerpo no hay marco");
    gvbl(0.5f, 0.0f, 0.0f, 0);          /* el mando se mueve 0,3 m en X sin girar: el marco de la mano, igual */
    CHECK(vrgame_hand_frame_now(1, p1, q1), "ahora (2)");
    { float pl[3], ql[4]; CHECK(vrgame_hand_frame_now(0, pl, ql) && fabsf(pl[0] - ps[0] - 0.3f) < 1e-4f && fabsf(pl[1] - ps[1]) < 1e-4f && fabsf(pl[2] - ps[2]) < 1e-4f, "la mano de apoyo soldada se mueve con la que empuña (%.4f)", pl[0] - ps[0]); }
    float dq = 0; for (int i = 0; i < 4; i++) dq = fmaxf(dq, fabsf(q1[i] - q0[i]));
    CHECK(fabsf(p1[0] - p0[0] - 0.3f) < 1e-4f && fabsf(p1[1] - p0[1]) < 1e-4f && fabsf(p1[2] - p0[2]) < 1e-4f && dq < 1e-5f, "el marco sigue al mando rígidamente: (%.4f, %.4f, %.4f)", p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]);
    CHECK(fabsf(sqrtf(q1[0] * q1[0] + q1[1] * q1[1] + q1[2] * q1[2] + q1[3] * q1[3]) - 1.0f) < 1e-4f, "cuaternión unitario");
    vrgame_test_advance(400000000ull);  /* más de 0,3 s sin mando: sin seguimiento */
    CHECK(!vrgame_hand_frame_now(1, p1, q1), "mano sin actualizar más de 0,3 s: sin marco");
}

/* ---- láser de la pistola y puntería: el rayo dibujado, la boca del arma y la bala sustituida comparten eje y origen ---- */
static void hand_trig(const float p[3], const float q[4], float trig) {
    MvVrHand h;
    memset(&h, 0, sizeof h);
    h.valid = 1; memcpy(h.pos, p, sizeof h.pos); memcpy(h.rot, q, sizeof h.rot); h.grip = 1.0f; h.trigger = trig;
    mv_vrgame_set_hand(1, &h);
}
static void lvbl(const float p[3], const float q[4], float trig, int n) { for (int i = 0; i < n; i++) { vrgame_test_advance(8000000ull); hand_trig(p, q, trig); vrgame_pad(0, &bs, ram); } }
/* Mallas láser del frame del renderer (mv_host_hands): cuántas hay y, de la primera, origen (centro del anillo en la boca) y eje (hacia el anillo lejano) en el espacio VR; len = su longitud. */
static int drawn_laser(float org[3], float ax[3], float *len) {
    VrHandsFrame f;
    int found = 0;
    if (mv_host_hands(&f)) for (int i = 0; i < f.nmeshes; i++) {
        const VrHandsMesh *m = &f.meshes[i];
        if (m->mesh_id != WLASER_MESH_ID) continue;
        if (!found++) {
            float c0[3] = {0, 0, 0}, c1[3] = {0, 0, 0};
            for (int v = 0; v < 4; v++) for (int k = 0; k < 3; k++) { c0[k] += m->verts[v].pos[k] / 4; c1[k] += m->verts[4 + v].pos[k] / 4; }
            float l = sqrtf((c1[0] - c0[0]) * (c1[0] - c0[0]) + (c1[1] - c0[1]) * (c1[1] - c0[1]) + (c1[2] - c0[2]) * (c1[2] - c0[2]));
            for (int k = 0; k < 3; k++) { org[k] = c0[k]; ax[k] = (c1[k] - c0[k]) / l; }
            *len = l;
            {   /* el punto (octaedro, vértices 8..13) cae sobre la línea del rayo: línea y punto salen de la misma pose, también durante el retroceso y el disparo */
                float dc[3] = {0, 0, 0};
                for (int v = 0; v < 6; v++) for (int k = 0; k < 3; k++) dc[k] += m->verts[8 + v].pos[k] / 6;
                float rel[3] = {dc[0] - org[0], dc[1] - org[1], dc[2] - org[2]}, along = rel[0] * ax[0] + rel[1] * ax[1] + rel[2] * ax[2];
                float perp[3] = {rel[0] - ax[0] * along, rel[1] - ax[1] * along, rel[2] - ax[2] * along};
                CHECK(sqrtf(perp[0] * perp[0] + perp[1] * perp[1] + perp[2] * perp[2]) < 1e-4f && along > 0.0f && along <= l + 1e-4f, "punto sobre la línea del láser (a %.2e m del eje, a %.3f de %.3f m)", sqrtf(perp[0] * perp[0] + perp[1] * perp[1] + perp[2] * perp[2]), along, l);
            }
            CHECK((m->flags & VRH_MF_DYNAMIC) && m->tex_id == 0 && m->nverts == WLASER_NV && m->nidx == WLASER_NI, "malla láser dinámica, sin textura, topología fija");
        }
    }
    mv_host_hands_done();
    return found;
}
/* Retroceso: el kick del arma (recoil_offset) solo mueve el modelo. Tras un disparo el rayo del láser (y su punto, sobre la línea: ver drawn_laser) sigue sobre el eje de la boca sin retroceso, y la
 * siguiente bala, disparada con el retroceso aún vivo, sale por ese mismo rayo (misma dirección y origen) */
static void laser_ignores_recoil(void) {
    printf("-- láser: el retroceso no mueve el rayo ni la bala siguiente --\n");
    srand(991);
    VrCamRecord saved = rec;
    rec.theta_play_deg = 37.0f; rec.eye[0] = 1234; rec.eye[2] = -4321; rec.p_ref[0] = 0.1f; rec.p_ref[2] = -0.2f;
    VrFireMap map; vrfire_map_from_cam(&map, &rec);
    float worst_las = 0, worst_bul = 0, worst_org = 0;
    int pairs = 0, samples = 0;
    for (int k = 0; k < 12; k++) {
        float q[4], R[9], want[3], p[3] = {0.1f + 0.3f * t_rnd(), 1.0f + 0.4f * t_rnd(), -0.2f - 0.4f * t_rnd()};
        do { t_rand_quat(q); expected_weapon_rot(1, q, R); want[0] = -R[2]; want[1] = -R[5]; want[2] = -R[8]; } while (want[1] < -0.3f);
        world(0, 12, 60); fresh_pistol_meshes(); mv_vrgame_set_options(1, 2);
        lvbl(p, q, 0.0f, 6);
        { VrFireMat dm; for (int n = 0; n < 8 && vrfire_take_projectile(0, &dm) == VRFIRE_SUBST; n++) {} }
        MvVrGameStatus st0, st;
        mv_vrgame_status(&st0);
        for (int i = 0; i < 12; i++) { lvbl(p, q, 0.7f, 1); mv_vrgame_status(&st); if (st.shots > st0.shots) break; }
        if (st.shots == st0.shots) { CHECK(0, "pose %d: primer disparo", k); continue; }
        { VrFireMat dm; for (int n = 0; n < 8 && vrfire_take_projectile(0, &dm) == VRFIRE_SUBST; n++) {} }
        float org[3], ax[3], len = 0;
        for (int i = 0; i < 8; i++) {      /* retroceso en curso, el mando quieto */
            lvbl(p, q, 0.0f, 1);
            if (drawn_laser(org, ax, &len) == 1) { float e = t_ang(ax, want); if (e > worst_las) worst_las = e; samples++; }
        }
        mv_vrgame_status(&st0);
        for (int i = 0; i < 40; i++) {      /* segundo disparo lo antes posible */
            lvbl(p, q, i % 6 < 3 ? 0.0f : 0.7f, 1); mv_vrgame_status(&st);
            if (st.shots > st0.shots) break;
        }
        if (st.shots == st0.shots) { CHECK(0, "pose %d: segundo disparo", k); continue; }
        float lorg[3], lax[3], llen;
        CHECK(drawn_laser(lorg, lax, &llen) == 1, "pose %d: láser en el VBlank del segundo disparo", k);
        VrFireMat m;
        CHECK(vrfire_take_projectile(0, &m) == VRFIRE_SUBST, "pose %d: segunda bala sustituida", k);
        float fw[3], dw[3], bw[3], bu[3];
        vrfire_mat_forward(&m, fw);
        vrfire_dir_from_world(&map, fw, dw);
        { float n = sqrtf(dw[0] * dw[0] + dw[1] * dw[1] + dw[2] * dw[2]); for (int i = 0; i < 3; i++) dw[i] /= n; }
        float eb = t_ang(dw, lax), eb2 = t_ang(dw, want);
        if (eb > worst_bul) worst_bul = eb;
        if (eb2 > worst_bul) worst_bul = eb2;
        for (int i = 0; i < 3; i++) bw[i] = (float)m.t[i] - 400.0f * fw[i];
        vrfire_pos_from_world(&map, bw, bu);
        float d = sqrtf((bu[0] - lorg[0]) * (bu[0] - lorg[0]) + (bu[1] - lorg[1]) * (bu[1] - lorg[1]) + (bu[2] - lorg[2]) * (bu[2] - lorg[2]));
        if (d > worst_org) worst_org = d;
        pairs++;
    }
    printf("   %d disparos dobles, %d muestras de láser en retroceso: rayo vs eje sin retroceso %.4f grados, segunda bala vs rayo/eje %.4f grados, origen %.4f m\n", pairs, samples, worst_las, worst_bul, worst_org);
    CHECK(pairs == 12 && samples >= 12 * 6, "12 pares de disparos y láser dibujado durante el retroceso (%d, %d)", pairs, samples);
    CHECK(worst_las < 0.5f, "durante el retroceso el rayo no se mueve: %.4f grados (< 0,5)", worst_las);
    CHECK(worst_bul < 0.1f, "la segunda bala (retroceso vivo) sale por el rayo y el eje sin retroceso: %.4f grados (< 0,1)", worst_bul);
    CHECK(worst_org < 0.003f, "la segunda bala parte del origen del rayo: %.4f m (< 3 mm)", worst_org);
    rec = saved;
}

static void laser_aims_with_bullet(void) {
    printf("-- láser de la pistola: rayo, boca y bala comparten eje --\n");
    srand(777);
    VrCamRecord saved = rec;
    rec.theta_play_deg = 37.0f; rec.eye[0] = 1234; rec.eye[2] = -4321; rec.p_ref[0] = 0.1f; rec.p_ref[2] = -0.2f;      /* correspondencia usuario -> motor no trivial */
    VrFireMap map; vrfire_map_from_cam(&map, &rec);
    float worst_bul = 0, worst_las = 0, worst_org = 0, worst_live = 0;
    int shots = 0;
    for (int variant = 0; variant < 2; variant++) for (int k = 0; k < 12; k++) {
        float q[4], R[9], want[3], p[3] = {0.1f + 0.3f * t_rnd(), 1.0f + 0.4f * t_rnd(), -0.2f - 0.4f * t_rnd()};
        do { t_rand_quat(q); expected_weapon_rot(1, q, R); want[0] = -R[2]; want[1] = -R[5]; want[2] = -R[8]; } while (want[1] < -0.3f);     /* arma levantada */
        world(0, 12, 60);
        if (variant) fresh_pistol_meshes(); else fresh_pistol();
        mv_vrgame_set_options(1, 2);
        lvbl(p, q, 0.0f, 6);
        float org[3], ax[3], len = 0;
        CHECK(drawn_laser(org, ax, &len) == 1, "v%d pose %d: un láser con la pistola levantada", variant, k);
        CHECK(fabsf(len - WLASER_MAX_M) < 1e-3f, "longitud %.3f m", len);
        float e0 = t_ang(ax, want);
        if (e0 > worst_las) worst_las = e0;
        if (variant) {      /* a la cadencia del renderer el rayo sigue al arma: el mando se movió tras la simulación */
            float p2[3] = {p[0] + 0.02f, p[1] - 0.01f, p[2]}, q2[4], dq[4] = {0.03f * (t_rnd() - 0.5f), 0.03f * (t_rnd() - 0.5f), 0.03f * (t_rnd() - 0.5f), 1}, R2[9];
            t_qnorm(dq); t_qmul(dq, q, q2);
            hand_trig(p2, q2, 0.0f);
            if (drawn_laser(org, ax, &len) == 1) { expected_weapon_rot(1, q2, R2); float w2[3] = {-R2[2], -R2[5], -R2[8]}; float e = t_ang(ax, w2); if (e > worst_live) worst_live = e; }
            hand_trig(p, q, 0.0f);
        }
        MvVrGameStatus st0, st;
        { VrFireMat dm; for (int n = 0; n < 8 && vrfire_take_projectile(0, &dm) == VRFIRE_SUBST; n++) {} }     /* peticiones de pruebas anteriores */
        mv_vrgame_status(&st0);
        for (int i = 0; i < 12; i++) { lvbl(p, q, 0.7f, 1); mv_vrgame_status(&st); if (st.shots > st0.shots) break; }
        if (st.shots == st0.shots) { CHECK(0, "v%d pose %d: la pistola no disparó", variant, k); continue; }
        shots++;
        float lorg[3], lax[3], llen;
        CHECK(drawn_laser(lorg, lax, &llen) == 1, "v%d pose %d: láser en el VBlank del disparo", variant, k);
        VrFireMat m;
        CHECK(vrfire_take_projectile(0, &m) == VRFIRE_SUBST, "v%d pose %d: bala sustituida", variant, k);
        float fw[3], dw[3], bw[3], bu[3];
        vrfire_mat_forward(&m, fw);
        vrfire_dir_from_world(&map, fw, dw);                                  /* eje de la bala en el espacio del usuario */
        { float n = sqrtf(dw[0] * dw[0] + dw[1] * dw[1] + dw[2] * dw[2]); for (int i = 0; i < 3; i++) dw[i] /= n; }     /* la matriz del motor está en 1.12: delante sale con norma 0,9998; t_ang pide unitarios */
        float eb = t_ang(dw, lax), eb2 = t_ang(dw, want);
        if (eb > worst_bul) worst_bul = eb;
        if (eb2 > worst_bul) worst_bul = eb2;
        for (int i = 0; i < 3; i++) bw[i] = (float)m.t[i] - 400.0f * fw[i];   /* boca = t - 400 * delante */
        vrfire_pos_from_world(&map, bw, bu);
        float d = sqrtf((bu[0] - lorg[0]) * (bu[0] - lorg[0]) + (bu[1] - lorg[1]) * (bu[1] - lorg[1]) + (bu[2] - lorg[2]) * (bu[2] - lorg[2]));
        if (d > worst_org) worst_org = d;
    }
    printf("   %d disparos: bala vs rayo/boca %.4f grados, rayo vs boca %.4f, rayo vivo vs boca %.4f, origen bala vs rayo %.4f m\n", shots, worst_bul, worst_las, worst_live, worst_org);
    CHECK(shots == 24, "24 disparos medidos (%d)", shots);
    CHECK(worst_bul < 0.1f, "la bala sale por el eje del rayo y de la boca: %.4f grados (< 0,1)", worst_bul);
    CHECK(worst_las < 0.5f, "el rayo sigue el eje de la boca: %.4f grados (< 0,5)", worst_las);
    CHECK(worst_live < 0.5f, "el rayo sigue al mando a la cadencia del renderer: %.4f grados (< 0,5)", worst_live);
    CHECK(worst_org < 0.003f, "el rayo parte de donde sale la bala: %.4f m (< 3 mm)", worst_org);

    /* cuándo NO se ve */
    float org[3], ax[3], len, q[4], R[9], p[3] = {0.2f, 1.2f, -0.3f};
    do { t_rand_quat(q); expected_weapon_rot(1, q, R); } while (-R[5] < -0.3f);
    world(0, 12, 60); fresh_pistol_meshes(); mv_vrgame_set_options(1, 2);
    lvbl(p, q, 0.0f, 6);
    CHECK(drawn_laser(org, ax, &len) == 1, "levantada: láser");
    bs.in_control = 0; lvbl(p, q, 0.0f, 3);
    CHECK(drawn_laser(org, ax, &len) == 0, "pausa/menú/cinemática (sin control del jugador): sin láser");
    bs.in_control = 1; lvbl(p, q, 0.0f, 3);
    CHECK(drawn_laser(org, ax, &len) == 1, "con el control otra vez: láser");
    float qd[4];
    do { t_rand_quat(qd); expected_weapon_rot(1, qd, R); } while (-R[5] > -0.8f);                      /* boca casi al suelo */
    lvbl(p, qd, 0.0f, 3);
    CHECK(drawn_laser(org, ax, &len) == 0, "boca hacia el suelo (arma no levantada): sin láser");
    lvbl(p, q, 0.0f, 3);
    CHECK(drawn_laser(org, ax, &len) == 1, "otra vez levantada: láser");
    world(-1, 0, 0); lvbl(p, q, 0.0f, 3);
    CHECK(drawn_laser(org, ax, &len) == 0, "sin arma equipada: sin láser");
    world(2, 0, 3); fresh_weapon(2);
    for (int i = 0; i < 6; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    CHECK(drawn_laser(org, ax, &len) == 0, "granada en la mano: sin láser (solo la pistola lo lleva)");
    world(0, 12, 60); fresh_pistol_meshes(); mv_vrgame_set_options(1, 2);
    lvbl(p, q, 0.0f, 6);
    CHECK(drawn_laser(org, ax, &len) == 1, "pistola otra vez: láser");
    vrgame_reset();
    CHECK(drawn_laser(org, ax, &len) == 0, "tras reiniciar el motor no queda láser publicado");
    rec = saved;
}


/* ---- el láser se acorta en el primer impacto con el escenario: paredes HZD del juego (RAM) y geometría sólida de vr_headcol (sustituida) ---- */
static void w32r(uint32_t a, uint32_t v) { memcpy(ram + (a & 0x1FFFFF), &v, 4); }
static void w16r(uint32_t a, int16_t v) { memcpy(ram + (a & 0x1FFFFF), &v, 2); }
/* una pared HZD vertical de 6 m de ancho y 4 m de alto, perpendicular a la proyección horizontal de la dirección d (mundo del motor), que corta al rayo o + t*d en t = dist (unidades) */
static void hzd_wall(const float o[3], const float d[3], float dist) {
    uint32_t ctl = 0x801F0000, map = 0x801F0100, wr = 0x801F0200, hdr = 0x801F0300, grp = 0x801F0340, walls = 0x801F0400, flags = 0x801F0500;
    memset(ram + (0x801F0000 & 0x1FFFFF), 0, 0x1000);
    w32r(0x800ACB84, ctl); w32r(ctl + 0x2C, map); w32r(map + 8, wr); w32r(wr, hdr); w32r(0x800ACB38, 1);
    w16r(hdr + 10, 1); w32r(hdr + 16, grp);
    w16r(grp + 2, 1); w32r(grp + 8, walls); w32r(grp + 20, flags);
    float hl = sqrtf(d[0] * d[0] + d[2] * d[2]), hx = d[0] / hl, hz = d[2] / hl;
    float cx = o[0] + d[0] * dist, cy = o[1] + d[1] * dist, cz = o[2] + d[2] * dist;
    int16_t w[8] = {(int16_t)lroundf(cx - hz * 3000.0f), (int16_t)lroundf(cz + hx * 3000.0f), (int16_t)lroundf(cy - 2000.0f), 4000,
                    (int16_t)lroundf(cx + hz * 3000.0f), (int16_t)lroundf(cz - hx * 3000.0f), (int16_t)lroundf(cy - 2000.0f), 4000};
    for (int k = 0; k < 8; k++) w16r(walls + 2 * k, w[k]);
}
static float t_hit_wall(const uint8_t *r, const float o[3], const float d[3], float max) { return vrhzd_raycast(r, o, d, max, VRHZD_SKIP_SIGHT); }      /* la misma que registra vrhost.c (hit_wall) */
static void laser_stops_at_walls(void) {
    printf("-- láser: se acorta en el primer impacto con el escenario --\n");
    { VrHitWorld hw = {1000.0f, t_hit_wall, NULL, NULL, NULL}; vrhit_set_world(&hw); }      /* el láser usa la prueba de la bala (vrhit_shot_dist) */
    srand(4242);
    VrCamRecord saved = rec;
    rec.theta_play_deg = 37.0f; rec.eye[0] = 0; rec.eye[1] = 1500; rec.eye[2] = 0; rec.p_ref[0] = 0.1f; rec.p_ref[2] = -0.2f;
    VrFireMap map; vrfire_map_from_cam(&map, &rec);
    float q[4], R[9], p[3] = {0.2f, 1.2f, -0.3f};
    do { t_rand_quat(q); expected_weapon_rot(1, q, R); } while (-R[5] < -0.2f || -R[5] > 0.3f);       /* casi horizontal, la pared de 4 m de alto la corta seguro */
    world(0, 12, 60); fresh_pistol_meshes(); mv_vrgame_set_options(1, 2);
    memset(ram + (0x801F0000 & 0x1FFFFF), 0, 0x1000); w32r(0x800ACB84, 0); ray_stub_mm = -1;
    lvbl(p, q, 0.0f, 6);
    float org[3], ax[3], len = 0, ow[3], dw[3];
    CHECK(drawn_laser(org, ax, &len) == 1 && fabsf(len - WLASER_MAX_M) < 1e-3f, "sin datos de colisión: alcance completo (%.3f)", len);
    vrfire_pos_to_world(&map, org, ow); vrfire_dir_to_world(&map, ax, dw);
    const float dists_m[] = {0.5f, 1.0f, 2.5f, 4.0f, 9.0f, 14.0f};
    float prev = 0; int mono = 1;
    for (unsigned i = 0; i < sizeof dists_m / sizeof *dists_m; i++) {
        hzd_wall(ow, dw, dists_m[i] * map.units_per_m);
        lvbl(p, q, 0.0f, 2);
        float l = 0;
        CHECK(drawn_laser(org, ax, &l) == 1, "pared a %.1f m: láser dibujado", dists_m[i]);
        CHECK(fabsf(l - (dists_m[i] - WLASER_SURF_GAP_M)) < 0.01f, "pared HZD a %.1f m: el rayo mide %.3f m (esperado %.3f)", dists_m[i], l, dists_m[i] - WLASER_SURF_GAP_M);
        if (l <= prev) mono = 0;
        prev = l;
    }
    CHECK(mono, "paredes cada vez más lejos: longitud estrictamente creciente");
    hzd_wall(ow, dw, 20.0f * map.units_per_m); lvbl(p, q, 0.0f, 2);
    CHECK(drawn_laser(org, ax, &len) == 1 && fabsf(len - (20.0f - WLASER_SURF_GAP_M)) < 0.01f, "pared a 20 m (dentro del alcance de 25 m): %.3f m", len);
    CHECK(fabsf(WLASER_MAX_M - vrhit_weapon_range_m(0)) < 1e-6f, "el láser mide el alcance de la bala de la SOCOM (%.0f m)", WLASER_MAX_M);
    hzd_wall(ow, dw, 26.0f * map.units_per_m); lvbl(p, q, 0.0f, 2);
    CHECK(drawn_laser(org, ax, &len) == 1 && fabsf(len - WLASER_MAX_M) < 1e-3f, "pared a 26 m (fuera de alcance): %.0f m (%.3f)", WLASER_MAX_M, len);
    float back[3] = {-dw[0], -dw[1], -dw[2]};
    hzd_wall(ow, back, 3.0f * map.units_per_m); lvbl(p, q, 0.0f, 2);
    CHECK(drawn_laser(org, ax, &len) == 1 && fabsf(len - WLASER_MAX_M) < 1e-3f, "pared a la espalda: no corta (%.3f)", len);
    /* geometría de render nativa (vr_headcol) más cerca que la pared HZD: NO corta el láser, porque no bloquea la bala (vr_hit.h, VrHitWorld.solid): láser = bala */
    hzd_wall(ow, dw, 6.0f * map.units_per_m);
    ray_stub_mm = 1.5f * map.units_per_m; lvbl(p, q, 0.0f, 2);
    CHECK(drawn_laser(org, ax, &len) == 1 && fabsf(len - (6.0f - WLASER_SURF_GAP_M)) < 0.01f, "sólido de render a 1,5 m y pared a 6 m: la pared manda (%.3f m)", len);
    ray_stub_mm = -1; lvbl(p, q, 0.0f, 2);
    CHECK(drawn_laser(org, ax, &len) == 1 && fabsf(len - (6.0f - WLASER_SURF_GAP_M)) < 0.01f, "sin sólido: pared HZD a 6 m: %.3f m", len);
    ray_stub_mm = 3.0f * map.units_per_m; w32r(0x800ACB84, 0); lvbl(p, q, 0.0f, 2);
    CHECK(drawn_laser(org, ax, &len) == 1 && fabsf(len - WLASER_MAX_M) < 1e-3f, "solo sólido de render a 3 m (sin HZD): no corta, alcance completo (%.3f)", len);
    ray_stub_mm = -1; hzd_wall(ow, dw, 0.02f * map.units_per_m); lvbl(p, q, 0.0f, 2);
    CHECK(drawn_laser(org, ax, &len) == 0, "pared HZD a 2 cm de la boca: nada que dibujar");
    ray_stub_mm = -1; w32r(0x800ACB84, 0); lvbl(p, q, 0.0f, 2);
    CHECK(drawn_laser(org, ax, &len) == 1 && fabsf(len - WLASER_MAX_M) < 1e-3f, "espacio abierto otra vez: alcance completo (%.3f)", len);
    /* un enemigo en el rayo lo corta como corta la bala (vr_hit): sin esto el punto caía en la pared de detrás del enemigo */
    {
        const uint32_t tb = 0x800B7660u + 72u * 5u;
        w32r(0x800ACB9C, 1);
        for (int pass = 0; pass < 3; pass++) {
            float dist_m = pass == 2 ? 6.0f : 4.0f;
            if (pass == 1) hzd_wall(ow, dw, 9.0f * map.units_per_m);
            if (pass == 2) hzd_wall(ow, dw, 2.0f * map.units_per_m);
            w16r(tb, 0xDF); w16r(tb + 2, 2); w16r(tb + 4, 1);
            for (int k = 0; k < 3; k++) { w16r(tb + 8 + 2 * k, (int)lroundf(ow[k] + dw[k] * dist_m * map.units_per_m)); w16r(tb + 16 + 2 * k, 600); }
            lvbl(p, q, 0.0f, 2);
            float l = 0;
            CHECK(drawn_laser(org, ax, &l) == 1, "enemigo a %.0f m (pared %s): láser dibujado", dist_m, pass == 0 ? "ninguna" : pass == 1 ? "a 9 m" : "a 2 m");
            if (pass < 2) CHECK(l < dist_m - 0.05f && l > dist_m - 1.1f, "enemigo a %.0f m: el rayo acaba en su volumen, no detrás (%.3f m)", dist_m, l);
            else CHECK(fabsf(l - (2.0f - WLASER_SURF_GAP_M)) < 0.01f, "pared a 2 m delante del enemigo a 6 m: manda la pared (%.3f m)", l);
        }
        memset(ram + (tb & 0x1FFFFF), 0, 72);
        hzd_wall(ow, dw, 30.0f * map.units_per_m); lvbl(p, q, 0.0f, 2);
        CHECK(drawn_laser(org, ax, &len) == 1 && fabsf(len - WLASER_MAX_M) < 1e-3f, "sin el enemigo (pared a 30 m): alcance completo otra vez (%.3f)", len);
    }
    vrhit_set_world(NULL);
    vrgame_reset();
    rec = saved;
}

/* ---- PSG1: el autoenfoque del zoom (rifle.c GetZoomLimit) mide el eje de la mira contra las paredes HZD y la geometría sólida nativa ---- */
static void psg1_autofocus_wiring(void) {
    printf("-- PSG1: zoom por distancia al muro en el eje de la mira --\n");
    static uint8_t ram_saved[sizeof ram];
    memcpy(ram_saved, ram, sizeof ram);
    VrCamRecord saved = rec;
    rec.theta_play_deg = 37.0f; rec.eye[0] = 0; rec.eye[1] = 1500; rec.eye[2] = 0; rec.p_ref[0] = 0.1f; rec.p_ref[2] = -0.2f;
    VrFireMap map; vrfire_map_from_cam(&map, &rec);
    float p[3] = {0.2f, 1.2f, -0.3f}, q[4] = {0, 0, 0, 1};
    MvVrSpecialView sv;
    world(9, 5, 20); fresh_meshes(9); mv_vrgame_set_options(1, 2);
    memset(ram + (0x801F0000 & 0x1FFFFF), 0, 0x1000); w32r(0x800ACB84, 0); ray_stub_mm = -1;
    for (int i = 0; i < 8; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(mv_vrgame_special_view(&sv) == 1 && sv.kind == MV_VIEW_SCOPE && sv.cam_valid == 1, "PSG1 con lente");
    /* eje de la mira en coordenadas del motor: cámara de la mira y su -Z */
    const float x = sv.cam_rot[0], y = sv.cam_rot[1], z = sv.cam_rot[2], w = sv.cam_rot[3];
    const float fu[3] = {-2.0f * (x * z + w * y), -2.0f * (y * z - w * x), -(1.0f - 2.0f * (x * x + y * y))}, cp[3] = {sv.cam_pos[0], sv.cam_pos[1], sv.cam_pos[2]};
    float ow[3], dw[3];
    vrfire_pos_to_world(&map, cp, ow); vrfire_dir_to_world(&map, fu, dw);
    const int ticks_vbl = 130;                               /* 130 VBlanks de 8 ms = 1,04 s = 26 ticks de 25 Hz: más que los 9 de espera + la subida */
    /* pared HZD a 500 unidades de la cámara (~800 de la lente): menos de 1000 = sin zoom */
    hzd_wall(ow, dw, 500.0f);
    for (int i = 0; i < ticks_vbl; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(fabsf(sniper_zoom(syn_state) - 1.0f) < 1e-4f, "pared HZD a ~800 unidades: sin zoom (%.3f)", sniper_zoom(syn_state));
    /* sin pared: zoom completo (el rango se vuelve a -1 al desaparecer el impacto) */
    memset(ram + (0x801F0000 & 0x1FFFFF), 0, 0x1000); w32r(0x800ACB84, 0);
    for (int i = 0; i < ticks_vbl; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(fabsf(sniper_zoom(syn_state) - SNIPER_ZOOM_GAME) < 1e-3f, "espacio abierto: 9,375x (%.3f)", sniper_zoom(syn_state));
    /* geometría sólida nativa más cerca que la pared HZD: manda el mínimo */
    ray_stub_mm = 500.0f;
    for (int i = 0; i < ticks_vbl; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(fabsf(sniper_zoom(syn_state) - 1.0f) < 1e-4f, "geometría sólida a 500 unidades: sin zoom (%.3f)", sniper_zoom(syn_state));
    hzd_wall(ow, dw, 2000.0f);                               /* pared HZD lejos (~2300) y geometría sólida más cerca (500) */
    for (int i = 0; i < ticks_vbl; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(fabsf(sniper_zoom(syn_state) - 1.0f) < 1e-4f, "pared lejana y sólido cerca: gana el más cercano (%.3f)", sniper_zoom(syn_state));
    ray_stub_mm = -1;                                        /* solo la pared a ~2300: zoom intermedio (limite/320) */
    for (int i = 0; i < ticks_vbl; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    CHECK(sniper_zoom(syn_state) > 5.0f && sniper_zoom(syn_state) < SNIPER_ZOOM_GAME - 0.5f, "pared HZD a ~2300 unidades: zoom intermedio (%.3f)", sniper_zoom(syn_state));
    memcpy(ram, ram_saved, sizeof ram);      /* hzd_wall y vrfire_skip_raise tocan la RAM del juego */
    world(-1, 0, 0);
    for (int i = 0; i < 3; i++) { hand_at(1, p, q); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram); }
    vrgame_reset();
    rec = saved;
}

/* ---- boca = apuntado del mando (aim): el eje del arma, el láser y la bala siguen el eje de apuntado; la empuñadura sigue en el agarre ---- */
static void hand_aim(const float p[3], const float q[4], const float aq[4], float trig) {
    MvVrHand h;
    memset(&h, 0, sizeof h);
    h.valid = 1; memcpy(h.pos, p, sizeof h.pos); memcpy(h.rot, q, sizeof h.rot); h.grip = 1.0f; h.trigger = trig;
    if (aq) { h.aim_valid = 1; memcpy(h.aim_pos, p, sizeof h.aim_pos); memcpy(h.aim_rot, aq, sizeof h.aim_rot); }
    mv_vrgame_set_hand(1, &h);
}
static void avbl(const float p[3], const float q[4], const float aq[4], float trig, int n) { for (int i = 0; i < n; i++) { vrgame_test_advance(8000000ull); hand_aim(p, q, aq, trig); vrgame_pad(0, &bs, ram); } }
static void rot_z(const float q[4], float o[3]) { float R[9]; t_qmat(q, R); o[0] = -R[2]; o[1] = -R[5]; o[2] = -R[8]; }
static void aim_pose(void) {
    printf("-- boca = apuntado del mando --\n");
    srand(2024);
    VrCamRecord saved = rec;
    rec.theta_play_deg = 37.0f; rec.eye[0] = 1234; rec.eye[2] = -4321; rec.p_ref[0] = 0.1f; rec.p_ref[2] = -0.2f;
    VrFireMap map; vrfire_map_from_cam(&map, &rec);
    float worst_w = 0, worst_l = 0, worst_b = 0, worst_g = 0, worst_nofix = 0, worst_back = 0, worst_up = 0;
    int shots = 0;
    for (int k = 0; k < 12; k++) {
        float q[4], R[9], want[3], p[3] = {0.1f + 0.3f * t_rnd(), 1.0f + 0.4f * t_rnd(), -0.2f - 0.4f * t_rnd()};
        do { t_rand_quat(q); expected_weapon_rot(1, q, R); want[0] = -R[2]; want[1] = -R[5]; want[2] = -R[8]; } while (want[1] < -0.2f);
        /* apuntado del mando: el del agarre girado -35 grados sobre su eje X (medido en vr_body.h: el apuntado queda 35 grados por debajo del -Z del agarre) más un giro lateral aleatorio */
        float a = -(35.0f + 20.0f * (t_rnd() - 0.5f)) * 0.017453292f * 0.5f, rel[4] = {sinf(a), 0, 0, cosf(a)}, aq[4], aim[3];
        t_qmul(q, rel, aq); t_qnorm(aq);
        rot_z(aq, aim);
        world(0, 12, 60);
        fresh_pistol_meshes();
        mv_vrgame_set_options(1, 2);
        avbl(p, q, NULL, 0.0f, 4);                                   /* sin apuntado: el desvío de siempre */
        float dir[3], org[3];
        if (drawn_weapon(dir, org)) { float e = t_ang(dir, want); if (e > worst_back) worst_back = e; worst_nofix = fmaxf(worst_nofix, t_ang(dir, aim)); }
        avbl(p, q, aq, 0.0f, 8);                                     /* con apuntado: se corrige */
        if (!drawn_weapon(dir, org)) { CHECK(0, "pose %d: arma dibujada", k); continue; }
        float ew = t_ang(dir, aim);
        if (ew > worst_w) worst_w = ew;
        {   /* el alabeo también sigue al mando: el arriba del arma es el arriba del apuntado (+Y de aq) */
            float R[9], aup[3]; t_qmat(aq, R); aup[0] = R[1]; aup[1] = R[4]; aup[2] = R[7];
            float eu = t_ang(drawn_up, aup); if (eu > worst_up) worst_up = eu;
        }
        float dg = sqrtf((org[0] - p[0]) * (org[0] - p[0]) + (org[1] - p[1]) * (org[1] - p[1]) + (org[2] - p[2]) * (org[2] - p[2]));
        if (dg > worst_g) worst_g = dg;
        float lorg[3], lax[3], ll;
        CHECK(drawn_laser(lorg, lax, &ll) == 1, "pose %d: láser", k);
        float el = t_ang(lax, aim);
        if (el > worst_l) worst_l = el;
        MvVrGameStatus st0, st;
        { VrFireMat dm; for (int n = 0; n < 8 && vrfire_take_projectile(0, &dm) == VRFIRE_SUBST; n++) {} }
        mv_vrgame_status(&st0);
        for (int i = 0; i < 12; i++) { avbl(p, q, aq, 0.7f, 1); mv_vrgame_status(&st); if (st.shots > st0.shots) break; }
        VrFireMat m;
        if (st.shots > st0.shots && vrfire_take_projectile(0, &m) == VRFIRE_SUBST) {
            float fw[3], dw[3];
            vrfire_mat_forward(&m, fw); vrfire_dir_from_world(&map, fw, dw);
            float n = sqrtf(dw[0] * dw[0] + dw[1] * dw[1] + dw[2] * dw[2]); for (int i = 0; i < 3; i++) dw[i] /= n;
            float eb = t_ang(dw, aim); if (eb > worst_b) worst_b = eb;
            shots++;
        }
        avbl(p, q, NULL, 0.0f, 80);                                  /* se pierde el apuntado (y el retroceso del disparo se asienta, ~0,3 s): vuelve el desvío de siempre */
        if (drawn_weapon(dir, org)) { float e = t_ang(dir, want); if (e > worst_back) worst_back = e; }
    }
    printf("   sin corregir la boca queda a %.1f grados del apuntado; corregida: arma %.3f, láser %.3f, bala %.3f grados, empuñadura %.5f m, %d disparos; sin apuntado vuelve a %.3f grados del desvío fijo\n", worst_nofix, worst_w, worst_l, worst_b, worst_g, shots, worst_back);
    CHECK(worst_nofix > 10.0f, "sin apuntado la boca difiere del apuntado (prueba con sentido): %.1f grados", worst_nofix);
    CHECK(worst_w < 0.5f, "el arma dibujada apunta como el mando: %.3f grados (< 0,5)", worst_w);
    CHECK(worst_l < 0.5f, "el láser sigue el apuntado del mando: %.3f grados (< 0,5)", worst_l);
    CHECK(worst_up < 0.5f, "el arriba del arma es el del apuntado del mando (sin alabeo heredado de la mano): %.3f grados (< 0,5)", worst_up);
    CHECK(shots == 12 && worst_b < 0.15f, "la bala sale por el apuntado del mando: %.3f grados, %d disparos (< 0,15)", worst_b, shots);
    CHECK(worst_g > sock_grip_dist() - 0.003f && worst_g < sock_grip_dist() + 0.003f, "la empuñadura sigue al agarre del mando a la distancia fija del zócalo (%.1f mm; ver arriba): %.4f m", sock_grip_dist() * 1000.0f, worst_g);
    CHECK(worst_back < 0.5f, "sin apuntado válido vuelve el desvío fijo: %.3f grados", worst_back);
    rec = saved;
}

/* ---- mando recto = arma recta: con el mando como se coge una pistola (apuntado horizontal al frente y arriba arriba; el agarre del mando queda 35 grados por encima del apuntado) la boca sale recta al frente
 * y el arma no se alabea. Ni el giro de mano (hand_roll_deg) ni la calibración de manos lo cambian: son la corrección agarre -> mano (cómo se dibuja la mano), no la del arma. Causa que medía el fallo:
 * el zócalo de la SOCOM (FIT_SOCOM_GRIP) se diseñó con la mano en apretón de manos (alabeo 90); con el alabeo de mando por defecto (45) el arma quedaba alabeada 45 grados y la boca 6 arriba y 6 a un lado,
 * y el giro corto de la boca al apuntado (aimfix) no quita el alabeo. ---- */
static void hand_aim_side(int side, const float p[3], const float q[4], const float aq[4]) {
    MvVrHand h;
    memset(&h, 0, sizeof h);
    h.valid = 1; memcpy(h.pos, p, sizeof h.pos); memcpy(h.rot, q, sizeof h.rot); h.grip = 1.0f; h.trigger = 0.0f;
    h.aim_valid = 1; memcpy(h.aim_pos, p, sizeof h.aim_pos); memcpy(h.aim_rot, aq, sizeof h.aim_rot);
    mv_vrgame_set_hand(side, &h);
}
static void aim_upright(void) {
    printf("-- mando recto: la boca al frente y el arma sin alabeo, con cualquier giro de mano y calibración --\n");
    VrCamRecord saved = rec;
    rec.theta_play_deg = 37.0f; rec.eye[0] = 1234; rec.eye[2] = -4321; rec.p_ref[0] = 0.1f; rec.p_ref[2] = -0.2f;
    const float p[3] = {0.2f, 1.2f, -0.3f}, aq[4] = {0, 0, 0, 1}, a35 = 35.0f * 0.017453292f * 0.5f, q[4] = {sinf(a35), 0, 0, cosf(a35)};      /* agarre = apuntado girado +35 sobre X */
    const float fwd[3] = {0, 0, -1}, up[3] = {0, 1, 0};
    const float rolls[] = {45.0f, 0.0f, 90.0f, -30.0f};
    float worst_b = 0, worst_u = 0;
    for (int side = 1; side >= 0; side--) {
        for (int k = 0; k < 6; k++) {
            VrBodyConfig bc; vrbody_config_default(&bc); vrbody_init(&bc); vrbody_clear_calibration();      /* alabeo por defecto (45), sin calibración */
            if (k < 4) vrbody_set_hand_roll(rolls[k]);
            if (k == 4) { const float qc[4] = {0.2f, 0.3f, 0.5f, 0.78f}; vrbody_set_calibration(side, qc); }
            if (k == 5) { vrbody_set_hand_roll(60.0f); const float qc[4] = {0.0f, 0.38268f, 0.0f, 0.92388f}; vrbody_set_calibration(side, qc); }
            world(0, 12, 60);
            fresh_pistol_meshes();
            mv_vrgame_set_options(side, 2);
            for (int i = 0; i < 10; i++) { vrgame_test_advance(8000000ull); hand_aim_side(side, p, q, aq); vrgame_pad(0, &bs, ram); }
            float dir[3], org[3];
            if (!drawn_weapon(dir, org)) { CHECK(0, "lado %d, caso %d: arma dibujada", side, k); continue; }
            float eb = t_ang(dir, fwd), eu = t_ang(drawn_up, up);
            if (eb > worst_b) worst_b = eb;
            if (eu > worst_u) worst_u = eu;
            CHECK(eb < 0.5f && eu < 0.5f, "lado %d, caso %d (giro de mano %.0f, calibrada %d): boca %.2f grados del frente, arriba del arma %.2f grados del arriba (< 0,5)", side, k, k < 4 ? rolls[k] : (k == 5 ? 60.0f : 45.0f), k >= 4, eb, eu);
        }
    }
    printf("   mando recto: boca peor %.3f grados del frente, alabeo peor %.3f grados (12 combinaciones de lado, giro de mano y calibración)\n", worst_b, worst_u);
    { VrBodyConfig bc; vrbody_config_default(&bc); vrbody_init(&bc); vrbody_clear_calibration(); }
    rec = saved;
}

/* ---- pistola a dos manos: la mano libre agarra la parte delantera/inferior (WP_SUPPORT) con el agarre analógico; la boca pasa a la línea mano principal -> mano de apoyo ---- */
static void hand_full(int side, const float p[3], const float q[4], const float aq[4], float trig, float grip) {
    MvVrHand h;
    memset(&h, 0, sizeof h);
    h.valid = 1; memcpy(h.pos, p, sizeof h.pos); memcpy(h.rot, q, sizeof h.rot); h.grip = grip; h.trigger = trig;
    if (aq) { h.aim_valid = 1; memcpy(h.aim_pos, p, sizeof h.aim_pos); memcpy(h.aim_rot, aq, sizeof h.aim_rot); }
    mv_vrgame_set_hand(side, &h);
}
static void tvbl(int main_side, const float p[3], const float q[4], const float aq[4], const float lp[3], float lgrip, int n) {
    for (int i = 0; i < n; i++) { vrgame_test_advance(8000000ull); hand_full(main_side, p, q, aq, 0.0f, 1.0f); hand_full(1 - main_side, lp, q, NULL, 0.0f, lgrip); vrgame_pad(0, &bs, ram); }
}
/* matriz del arma dibujada (raíz, columna-mayor) -> rotación y posición mundo del punto `pt` del arma */
static int drawn_point(WPoint pt, float R[9], float out[3]) {
    VrHandsFrame f;
    int rc = mv_host_hands(&f);
    if (!rc || f.nmeshes < 1) { mv_host_hands_done(); return 0; }
    const float *m = f.meshes[0].model;
    for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) R[3 * r + c] = m[4 * c + r];
    const WVec3 v = syn_def->points[pt].p;
    for (int i = 0; i < 3; i++) out[i] = m[12 + i] + m[i] * v.x + m[4 + i] * v.y + m[8 + i] * v.z;
    mv_host_hands_done();
    return 1;
}
static int hp_item(int side) { int it; float tr, th; vrgame_hand_pose_in(side, &it, &tr, &th); return it; }
/* La recarga VR es la del juego (vr_reload.h): el arma VR refleja siempre GM_Magazine. Las pruebas de abajo usan un juego falso que recarga como el original: solo tras el último disparo o al pulsar el ataque con el cargador a 0. */
static int vr_held(void) { return (pist_st->mag_inserted ? pist_st->mag_rounds : 0) + pist_st->chambered; }
static int gm_rl;                  /* VBlanks hasta que el juego falso acabe de recargar (0: no recarga) */

/* Munición del juego tras un disparo: el cargador y el total bajan una bala por disparo del juego (cuadrado pulsado con el arma lista). */
static void game_shots_follow(uint32_t *seen) {
    MvVrGameStatus st; mv_vrgame_status(&st);
    while (*seen < st.shots) { (*seen)++; if (bs.magazine > 0) bs.magazine--; if (bs.weapon_ammo[0] > 0) bs.weapon_ammo[0]--; if (bs.magazine == 0 && bs.weapon_ammo[0] > 0 && !gm_rl) gm_rl = 6; }
}
static void vbl_game(float trig, int n, uint32_t *seen) {
    for (int i = 0; i < n; i++) {
        int sq0 = sq_seen;
        { int16_t z = (int16_t)bs.magazine; memcpy(ram + (BA_MAGAZINE & 0x1FFFFFu), &z, 2); }      /* el puente lee GM_Magazine de la RAM del juego (el anfitrión la escribe al cargar por recogida) */
        vbl(trig, 1);
        { int16_t z; memcpy(&z, ram + (BA_MAGAZINE & 0x1FFFFFu), 2); bs.magazine = z; }
        game_shots_follow(seen);
        if (sq_seen > 0 && bs.magazine == 0 && bs.weapon_ammo[0] > 0 && !gm_rl) gm_rl = 17;      /* ataque pulsado con el cargador vacío: el juego pasa a su recarga (sna_init.c sub_80057BF0) */
        if (gm_rl > 0 && --gm_rl == 0 && bs.magazine == 0 && bs.weapon_ammo[0] > 0) bs.magazine = bs.weapon_ammo[0] < 12 ? bs.weapon_ammo[0] : 12;
    }
}
static uint32_t shots_now(void) { MvVrGameStatus st; mv_vrgame_status(&st); return st.shots; }
static void drain_pistol(uint32_t *seen) {      /* dispara hasta que el juego se queda sin balas */
    for (int k = 0; k < 60 && bs.weapon_ammo[0] > 0; k++) { vbl_game(0.9f, 12, seen); vbl_game(0.0f, 14, seen); }
}
static void fire_ok(uint32_t *seen, const char *what) {      /* un gatillo: sale exactamente un disparo */
    uint32_t s0 = shots_now();
    vbl_game(0.0f, 4, seen); vbl_game(0.9f, 12, seen); vbl_game(0.0f, 14, seen);
    CHECK(shots_now() == s0 + 1, "%s: un disparo (%u)", what, shots_now() - s0);
}
static void mirror_game_magazine(void) {
    printf("-- recarga: el arma VR es siempre el cargador del juego --\n");
    /* a) vaciar el cargador: el juego recarga solo y el arma VR queda con el cargador nuevo del juego */
    gm_rl = 0; world(0, 3, 15); fresh_pistol(); vbl(0.0f, 20);
    uint32_t seen = shots_now();
    CHECK(vr_held() == 3, "arranca con el cargador del juego (%d)", vr_held());
    for (int k = 0; k < 3; k++) { vbl_game(0.9f, 12, &seen); vbl_game(0.0f, 14, &seen); }
    vbl_game(0.0f, 100, &seen);
    CHECK(bs.magazine == 12 && bs.weapon_ammo[0] == 12 && vr_held() == 12 && pist_st->chambered, "tras la recarga del juego el arma VR lleva %d (juego %d/%d)", vr_held(), bs.magazine, bs.weapon_ammo[0]);
    fire_ok(&seen, "tras la recarga automática");
    CHECK(vr_held() == bs.magazine, "arma VR = cargador del juego (%d / %d)", vr_held(), bs.magazine);
    /* b) nunca muerta: el arma VR sin balas con balas en el cargador del juego se iguala sola */
    gm_rl = 0; world(0, 5, 5); fresh_pistol(); vbl(0.0f, 20);
    pist_st->mag_rounds = 0; pist_st->chambered = 0;
    vbl(0.0f, 100); seen = shots_now();
    CHECK(vr_held() == 5 && pist_st->chambered, "arma VR vacía con 5 en el juego: se iguala (%d)", vr_held());
    fire_ok(&seen, "tras igualar");
    /* c) el arma VR no puede llevar más que el juego */
    gm_rl = 0; world(0, 12, 12); fresh_pistol(); vbl(0.0f, 20);
    bs.magazine = 3; bs.weapon_ammo[0] = 3; vbl(0.0f, 5);
    CHECK(vr_held() == 3, "el juego baja a 3: el arma VR no pasa de 3 (%d)", vr_held());
    /* d) un disparo VR que el juego no gastó (golpe): el arma VR vuelve al cargador del juego tras la espera, no antes */
    gm_rl = 0; world(0, 12, 12); fresh_pistol(); vbl(0.0f, 20);
    uint32_t sd = shots_now();
    vbl(0.9f, 12); vbl(0.0f, 10);
    CHECK(shots_now() == sd + 1 && vr_held() == 11, "un disparo VR sin gasto del juego: 11 balas (%d)", vr_held());
    vbl(0.0f, 30);
    CHECK(vr_held() == 11, "a los 30 VBlanks aún no se iguala (%d)", vr_held());
    vbl(0.0f, 80);
    CHECK(vr_held() == 12, "pasada la espera vuelve al cargador del juego (%d)", vr_held());
    /* e) disparar con el cargador vacío y reserva: el host pulsa el ataque, el juego recarga y el disparo del gatillo no sale mientras recarga */
    gm_rl = 0; world(0, 0, 12); fresh_pistol(); vbl(0.0f, 20);
    seen = shots_now();
    CHECK(vr_held() == 0 && !pist_st->chambered, "cargador del juego vacío: arma VR sin balas (%d)", vr_held());
    sq_seen = 0;
    vbl_game(0.9f, 25, &seen); vbl_game(0.0f, 80, &seen);
    CHECK(sq_seen > 0 && bs.magazine == 12 && vr_held() == 12, "pulsar el gatillo con el cargador vacío pulsa el ataque y el juego recarga (cuadrado %d, juego %d, VR %d)", sq_seen, bs.magazine, vr_held());
    CHECK(shots_now() == seen, "y ese gatillo no dispara (disparos %u)", shots_now() - seen);
    fire_ok(&seen, "pasada la recarga");
    /* f) sin reserva no se pulsa nada y no hay disparo */
    gm_rl = 0; world(0, 0, 0); fresh_pistol(); vbl(0.0f, 20);
    seen = shots_now(); sq_seen = 0;
    vbl_game(0.9f, 30, &seen); vbl_game(0.0f, 10, &seen);
    CHECK(sq_seen == 0 && shots_now() == seen && vr_held() == 0, "sin reserva: no pulsa el ataque ni dispara (cuadrado %d)", sq_seen);
}
/* Recoger munición con la pistola seca: el anfitrión carga el cargador del juego en su RAM (GM_Magazine = min(GM_MagazineMax, GM_Weapons), como sna_init.c sna_80057A90) y el arma VR queda lista en pocos VBlanks. */
static void ammo_pickup_direct(void) {
    printf("-- munición: recoger con el arma seca carga el cargador del juego directamente --\n");
    gm_rl = 0; world(0, 12, 12); fresh_pistol(); vbl(0.0f, 20);
    bs.magazine = 0; bs.weapon_ammo[0] = 0; vbl(0.0f, 100);
    CHECK(vr_held() == 0, "seca (%d)", vr_held());
    int16_t z = 0; memcpy(ram + (BA_MAGAZINE & 0x1FFFFFu), &z, 2);
    bs.weapon_ammo[0] = 30;                                       /* recoge: solo sube el total */
    for (int i = 0; i < 4; i++) { vbl(0.0f, 1); memcpy(&z, ram + (BA_MAGAZINE & 0x1FFFFFu), 2); bs.magazine = z; }   /* el puente lee el cargador de la RAM del juego */
    CHECK(bs.magazine == 12, "cargador del juego cargado en la RAM (%d)", bs.magazine);
    CHECK(pist_st->mag_inserted && pist_st->chambered && vr_held() == 12, "arma VR lista en 4 VBlanks (balas %d)", vr_held());
    uint32_t s0 = shots_now();
    vbl(0.9f, 3);
    CHECK(shots_now() == s0 + 1, "dispara al primer gatillo tras recoger (%u)", shots_now() - s0);
    vbl(0.0f, 30);
    /* pausa en el VBlank siguiente a cargar el cargador del juego (antes de reflejarlo): al volver queda lista y dispara */
    gm_rl = 0; world(0, 12, 12); fresh_pistol(); vbl(0.0f, 20);
    uint32_t seen = shots_now(); drain_pistol(&seen); bs.magazine = 0; bs.weapon_ammo[0] = 0; vbl(0.0f, 100);
    CHECK(bs.weapon_ammo[0] == 0 && bs.magazine == 0 && vr_held() == 0, "vaciada disparando (total %d cargador %d VR %d)", bs.weapon_ammo[0], bs.magazine, vr_held());
    z = 0; memcpy(ram + (BA_MAGAZINE & 0x1FFFFFu), &z, 2);
    bs.weapon_ammo[0] = 30; vbl(0.0f, 1); memcpy(&z, ram + (BA_MAGAZINE & 0x1FFFFFu), 2); bs.magazine = z;
    CHECK(bs.magazine == 12 && vr_held() == 0, "cargado el juego, arma VR aún seca (%d, %d)", bs.magazine, vr_held());
    bs.in_control = 0; vbl(0.0f, 30); bs.in_control = 1; vbl(0.0f, 4);
    CHECK(pist_st->mag_inserted && pist_st->chambered && vr_held() == 12, "pausa justo tras cargar: arma VR lista al volver (insertado %d recámara %d balas %d)", pist_st->mag_inserted, pist_st->chambered, vr_held());
    s0 = shots_now();
    vbl(0.9f, 3);
    CHECK(shots_now() == s0 + 1, "y dispara (%u)", shots_now() - s0);
    vbl(0.0f, 30);
}
/* El anfitrión salta el levantado del arma del juego (vrfire_skip_raise). Juego falso tick a tick según la decompilación (sna_init.c: sna_8005009C arranca el ataque con el contador a 0; sna_gun_800540D0 lo cuenta
 * hasta 8, con el soltar en cola, y pasa a sub_80057BF0, que dispara al soltar), sobre la RAM que ve vrgame_pad: del gatillo a la bala pasan pocos VBlanks (sin el salto, 21) y sale una sola bala. */
static void raise_skipped(void) {
    printf("-- disparo: el levantado del arma del juego se salta --\n");
    const uint32_t pw = (0x80100000u - 0x20u) & 0x1FFFFFu;
    world(0, 12, 12); fresh_pistol(); vbl(0.0f, 20);
    uint32_t v = 0x9042u; memcpy(ram + pw + 0x91C, &v, 4); v = 0x80054520u; memcpy(ram + pw + 0x908, &v, 4); v = 0; memcpy(ram + pw + 0x90C, &v, 4); memcpy(ram + pw + 0x910, &v, 4);
    int prev = 0, attack = 0, shoot = 0, a38 = 0, idle = 0, bullets = 0, first = -1;
    for (int i = 0; i < 80; i++) {
        int s0 = sq_seen; vbl(i >= 7 && i < 11 ? 0.9f : 0.0f, 1); int sq = sq_seen > s0;      /* gatillo apretado en el VBlank 7 (a mitad de tick) */
        if (i % 2) continue;
        int32_t cnt; memcpy(&cnt, ram + pw + 0x90C, 4);
        uint32_t trg = 0; int rel = prev && !sq; prev = sq;
        if (!attack && sq) { cnt = 0; attack = 1; }
        if (attack) {
            int time = cnt++;
            if (!shoot) { if (time == 0) a38 = 0; if (rel) a38 = 1; if (time == 8) { shoot = 1; cnt = 0; idle = 0; } }
            else { trg = 1u | ((time == 0 && a38) || rel ? 2u : 0u); if (!sq) { if (++idle > 4) { shoot = 0; attack = 0; cnt = 0; } } else idle = 0; }
        }
        v = shoot ? 0x80056000u : 0x80054520u;
        memcpy(ram + pw + 0x90C, &cnt, 4); memcpy(ram + pw + 0x910, &trg, 4); memcpy(ram + pw + 0x908, &v, 4);
        if (trg & 2u) { VrFireMat m; CHECK(vrfire_take_projectile(0, &m) == VRFIRE_SUBST || bullets > 0, "la bala del juego lleva la boca del arma VR"); bullets++; if (first < 0) first = i - 7; }
    }
    CHECK(bullets == 1 && first >= 0 && first <= 7, "gatillo -> bala del juego en %d VBlanks (sin el salto, 21), %d bala", first, bullets);
    v = 0; memcpy(ram + pw + 0x91C, &v, 4); memcpy(ram + pw + 0x908, &v, 4); memcpy(ram + pw + 0x90C, &v, 4); memcpy(ram + pw + 0x910, &v, 4);
    vbl(0.0f, 30);
}
static void ammo_pickup_after_empty(void) {
    printf("-- munición: gastar todo, recoger 12 y recargar --\n");
    for (int variant = 0; variant < 2; variant++) {        /* el juego carga solo su cargador al recoger (1) o lo deja a 0 (0) */
        gm_rl = 0; world(0, 12, 12); fresh_pistol(); vbl(0.0f, 20);
        uint32_t seen = shots_now();
        drain_pistol(&seen); vbl_game(0.0f, 300, &seen);
        CHECK(bs.weapon_ammo[0] == 0 && vr_held() == 0, "variante %d: sin balas, arma VR vacía (%d)", variant, vr_held());
        bs.weapon_ammo[0] = 12; bs.magazine = variant ? 12 : 0;      /* recoge 12 balas */
        vbl_game(0.0f, 400, &seen);
        printf("   variante %d: juego mag=%d total=%d | VR insertado=%d balas=%d recamara=%d reserva=%d\n", variant, bs.magazine, bs.weapon_ammo[0], pist_st->mag_inserted, pist_st->mag_rounds, pist_st->chambered, vrreload_ammo_reserve(0));
        CHECK(pist_st->mag_inserted && pist_st->chambered && vr_held() == bs.magazine && vr_held() > 0, "variante %d: tras recoger 12 el arma VR = cargador del juego (VR %d, juego %d)", variant, vr_held(), bs.magazine);
        fire_ok(&seen, "tras recoger");
    }
    /* dos recogidas seguidas con el cargador del juego a 0 (24 de reserva, el caso del log): el cargador del juego carga 12 */
    gm_rl = 0; world(0, 12, 12); fresh_pistol(); vbl(0.0f, 20);
    uint32_t seen = shots_now();
    bs.magazine = 0; bs.weapon_ammo[0] = 0; vbl(0.0f, 100);
    bs.weapon_ammo[0] = 12; vbl_game(0.0f, 30, &seen); bs.weapon_ammo[0] = 24; vbl_game(0.0f, 400, &seen);
    CHECK(bs.magazine == 12 && bs.weapon_ammo[0] == 24 && pist_st->mag_inserted && pist_st->chambered && vr_held() == 12, "24 recogidas con el cargador a 0: juego %d/%d, VR balas %d", bs.magazine, bs.weapon_ammo[0], vr_held());
    CHECK(vrreload_ammo_reserve(0) == 12, "reserva mostrada 12 (%d)", vrreload_ammo_reserve(0));
    /* cambio de zona: el arma VR cargada, el juego pasa a una zona sin balas (con el tamaño de cargador a 0 durante la carga): no puede quedar «cargada» */
    gm_rl = 0; world(0, 10, 10); fresh_pistol(); vbl(0.0f, 20);
    CHECK(pist_st->mag_inserted && pist_st->chambered, "zona: arranca cargada");
    seen = shots_now();
    world(0, 0, 0); bs.magazine_max = 0;                          /* carga: el motor sin arma ni cargador */
    vbl_game(0.0f, 60, &seen);
    world(0, 0, 0);                                               /* zona nueva sin balas */
    vbl_game(0.0f, 60, &seen);
    uint32_t s0 = shots_now();
    vbl_game(0.9f, 12, &seen); vbl_game(0.0f, 10, &seen);
    CHECK(vr_held() == 0 && !pist_st->chambered && shots_now() == s0, "zona sin balas: el arma VR no está «cargada» y no dispara (balas %d, disparos %u)", vr_held(), shots_now() - s0);
}

static float d3(const float a[3], const float b[3]) { return sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }
/* Armas de fuego: ambas manos soldadas al arma mientras está equipada. La mano libre no sigue a su mando (que solo es fantasma de gestos): su agarre efectivo es el de la mano que empuña por una relación fija
 * (zócalo de apoyo de vr_grip en WP_SUPPORT), sin captura ni suelta por distancia ni apuntado a dos manos. */
static void weld_firearm(void) {
    printf("-- armas de fuego: ambas manos soldadas al arma, el mando libre es un fantasma de gestos --\n");
    for (int ms = 1; ms >= 0; ms--) {
        const int os = 1 - ms;
        float p[3] = {0.2f, 1.2f, -0.3f}, q[4] = {0, 0, 0, 1}, far_[3] = {-0.5f, 1.0f, -0.3f}, a = -20.0f * 0.017453292f * 0.5f, aq[4] = {sinf(a), 0, 0, cosf(a)}, aim[3];
        rot_z(aq, aim);
        world(0, 12, 60);
        fresh_pistol_meshes();
        mv_vrgame_set_options(ms, 2);
        tvbl(ms, p, q, aq, far_, 0.0f, 15);                                    /* mando libre lejos y sin apretar el agarre */
        float R[9], S[3], org[3], dir[3];
        CHECK(drawn_point(WP_SUPPORT, R, S) == 1 && drawn_weapon(dir, org) == 1, "lado %d: arma dibujada", ms);
        CHECK(t_ang(dir, aim) < 0.5f, "lado %d: la boca es el apuntado del mando que empuña (%.3f grados), sin apuntado a dos manos", ms, t_ang(dir, aim));
        CHECK(hp_item(os) == SUPPOSE && hp_item(ms) != SUPPOSE, "lado %d: la mano libre agarra el arma aunque su mando esté lejos y suelto (pose %d)", ms, hp_item(os));
        VrHandPose hands[2];
        CHECK(vrpose_get_hands(hands) == 2 && hands[os].valid && hands[ms].valid, "lado %d: poses de manos para la IK", ms);
        const float ds = d3(hands[os].pos, S), dm = d3(hands[ms].pos, p);
        CHECK(fabsf(ds - SUP_D) < 0.004f && dm < 1e-4f, "lado %d: la mano de apoyo de la IK está a la distancia fija de WP_SUPPORT (%.4f m) y la que empuña en su mando (%.5f)", ms, ds, dm);
        {   /* la mano soldada lleva una pose derivada de la que empuña, pero el cuerpo recibe el agarre REAL de su mando (raw_rot) para calibrar: calibrar con el arma en la mano no puede usar la pose soldada */
            const float fq[4] = {0.258819f, 0, 0, 0.965926f};          /* mando libre girado 30 grados sobre X, distinto de la pose soldada */
            for (int i = 0; i < 5; i++) { vrgame_test_advance(8000000ull); hand_full(ms, p, q, aq, 0.0f, 1.0f); hand_full(os, far_, fq, NULL, 0.0f, 0.0f); vrgame_pad(0, &bs, ram); }
            VrHandPose hr[2];
            CHECK(vrpose_get_hands(hr) == 2 && hr[os].derived == 1 && hr[ms].derived == 0, "lado %d: la mano de apoyo se marca derivada y la que empuña no", ms);
            const float dr = fabsf(hr[os].raw_rot[0] * fq[0] + hr[os].raw_rot[1] * fq[1] + hr[os].raw_rot[2] * fq[2] + hr[os].raw_rot[3] * fq[3]);
            const float de = fabsf(hr[os].rot[0] * fq[0] + hr[os].rot[1] * fq[1] + hr[os].rot[2] * fq[2] + hr[os].rot[3] * fq[3]);
            CHECK(hr[os].raw_valid && dr > 0.99999f && de < 0.999f, "lado %d: raw_rot de la mano soldada es el agarre de su mando (|dot| %.6f) y rot no (|dot| %.4f)", ms, dr, de);
            tvbl(ms, p, q, aq, far_, 0.0f, 5);                         /* mando libre otra vez como antes */
        }
        const float rel0 = d3(hands[os].pos, hands[ms].pos);
        CHECK(rel0 > 0.03f && rel0 < 0.5f, "lado %d: distancia entre manos creíble (%.3f m)", ms, rel0);
        /* el mando libre no mueve la mano: lejos, pegado al arma, detrás, con el agarre apretado */
        const float spots[4][3] = {{S[0], S[1] + 0.02f, S[2]}, {p[0], p[1], p[2] + 0.3f}, {0.9f, 0.2f, 0.4f}, {-0.2f, 1.6f, -0.9f}};
        for (int k = 0; k < 4; k++) {
            tvbl(ms, p, q, aq, spots[k], k & 1 ? 1.0f : 0.2f, 8);
            VrHandPose h2[2];
            CHECK(vrpose_get_hands(h2) == 2 && d3(h2[os].pos, hands[os].pos) < 1e-4f && hp_item(os) == SUPPOSE, "lado %d: mando libre en el punto %d: la mano no se mueve (%.5f m)", ms, k, d3(h2[os].pos, hands[os].pos));
            float rp[3], rg = -1.0f;
            CHECK(vrgame_hand_raw(os, rp, &rg, NULL, NULL) && d3(rp, spots[k]) < 1e-4f, "lado %d: el mando libre sigue disponible para gestos (%.5f)", ms, d3(rp, spots[k]));
        }
        /* la que empuña lleva a la otra rígidamente: se mueve y se gira (guiñada 40 grados) */
        {
            const float yq[4] = {0, sinf(20.0f * 0.017453292f), 0, cosf(20.0f * 0.017453292f)}, p2[3] = {-0.3f, 1.4f, -0.6f};
            tvbl(ms, p2, yq, NULL, far_, 0.0f, 8);
            VrHandPose h3[2];
            CHECK(vrpose_get_hands(h3) == 2, "lado %d: poses tras mover", ms);
            CHECK(fabsf(d3(h3[os].pos, h3[ms].pos) - rel0) < 1e-4f, "lado %d: la distancia entre manos se conserva al mover y girar (%.5f -> %.5f)", ms, rel0, d3(h3[os].pos, h3[ms].pos));
            float R2[9], S2[3];
            CHECK(drawn_point(WP_SUPPORT, R2, S2) == 1 && fabsf(d3(h3[os].pos, S2) - SUP_D) < 0.004f, "lado %d: y sigue a la distancia fija de WP_SUPPORT (%.4f m)", ms, d3(h3[os].pos, S2));
        }
        /* la bala sale por la boca */
        tvbl(ms, p, q, aq, far_, 0.0f, 15);
        CHECK(drawn_point(WP_SUPPORT, R, S) == 1 && drawn_weapon(dir, org) == 1, "lado %d: arma dibujada (2)", ms);
        { VrFireMat dm_; for (int n = 0; n < 8 && vrfire_take_projectile(0, &dm_) == VRFIRE_SUBST; n++) {} }
        MvVrGameStatus st0, st;
        mv_vrgame_status(&st0);
        for (int i = 0; i < 12; i++) { vrgame_test_advance(8000000ull); hand_full(ms, p, q, aq, 0.7f, 1.0f); hand_full(os, far_, q, NULL, 0.0f, 0.0f); vrgame_pad(0, &bs, ram); mv_vrgame_status(&st); if (st.shots > st0.shots) break; }
        VrFireMat fm;
        if (st.shots > st0.shots && vrfire_take_projectile(0, &fm) == VRFIRE_SUBST) {
            VrFireMap map; vrfire_map_from_cam(&map, &rec);
            float fw[3], dw[3];
            vrfire_mat_forward(&fm, fw); vrfire_dir_from_world(&map, fw, dw);
            CHECK(t_ang(dw, dir) < 3.0f, "lado %d: la bala sale por la boca (%.2f grados de la boca dibujada; el retroceso la mueve)", ms, t_ang(dw, dir));
        } else CHECK(0, "lado %d: disparo con una mano soldada", ms);
        /* sin arma de fuego equipada la mano libre vuelve a su mando */
        world(-1, 0, 0);
        tvbl(ms, p, q, aq, far_, 0.0f, 40);      /* retención de la soldadura (WELD_HOLD_VBL) + rampa de salida (WELD_FADE_NS) */
        CHECK(vrpose_get_hands(hands) == 2 && d3(hands[os].pos, far_) < 1e-4f, "lado %d: sin arma la mano libre vuelve a su mando (%.5f)", ms, d3(hands[os].pos, far_));
    }
    mv_vrgame_set_options(1, 2);
}

/* Tumbado con un arma que no es el PSG1 el arma está oculta: la mano de apoyo no queda soldada a ella, el mando la mueve (tras la rampa de salida). Al levantarse vuelve a soldarse. */
static void prone_releases_support_hand(void) {
    printf("-- tumbado con la SOCOM: la mano libre sigue a su mando --\n");
    const int ms = 1, os = 0;
    float p[3] = {0.2f, 1.2f, -0.3f}, q[4] = {0, 0, 0, 1}, far_[3] = {-0.5f, 1.0f, -0.3f};
    VrHandPose hands[2];
    world(0, 12, 60);
    fresh_pistol_meshes();
    mv_vrgame_set_options(ms, 2);
    tvbl(ms, p, q, NULL, far_, 0.0f, 15);
    CHECK(vrpose_get_hands(hands) == 2 && d3(hands[os].pos, far_) > 0.1f, "de pie: la mano libre va soldada al arma, no en su mando (%.3f m)", d3(hands[os].pos, far_));
    bs.stance = 1; bs.prone = 1;       /* tumbado, stance distinta de 2 y sin INTRUDE */
    tvbl(ms, p, q, NULL, far_, 0.0f, 40);
    CHECK(vrpose_get_hands(hands) == 2 && d3(hands[os].pos, far_) < 1e-4f, "tumbado con la SOCOM y sin INTRUDE: la mano libre sigue a su mando (%.5f m)", d3(hands[os].pos, far_));
    bs.stance = 0; bs.prone = 0;
    tvbl(ms, p, q, NULL, far_, 0.0f, 15);
    CHECK(vrpose_get_hands(hands) == 2 && d3(hands[os].pos, far_) > 0.1f, "de pie otra vez: la mano libre vuelve a soldarse al arma (%.3f m)", d3(hands[os].pos, far_));
}

/* ---- la corrección de puntería: solo armas con boca real, y sin saltos si el apuntado falta un instante ---- */
/* Ángulo entre dos giros. Con acos(|a.b|) un cuaternión repetido bit a bit da 0,04-0,06 grados: su norma al cuadrado es 1-1e-7 y el producto interno cae justo bajo 1 (sqrt(2e-7) rad), según se contraiga o no con FMA. atan2 de la parte vectorial de conj(a)*b contra su parte escalar no depende de la norma ni de ese redondeo (identicos -> 0 exacto). */
static float q_angle_deg(const float a[4], const float b[4]) {
    double w = (double)a[3] * b[3] + (double)a[0] * b[0] + (double)a[1] * b[1] + (double)a[2] * b[2];
    double x = (double)a[3] * b[0] - (double)a[0] * b[3] - (double)a[1] * b[2] + (double)a[2] * b[1];
    double y = (double)a[3] * b[1] + (double)a[0] * b[2] - (double)a[1] * b[3] - (double)a[2] * b[0];
    double z = (double)a[3] * b[2] - (double)a[0] * b[1] + (double)a[1] * b[0] - (double)a[2] * b[3];
    return (float)(2.0 * atan2(sqrt(x * x + y * y + z * z), fabs(w)) * 57.29577951308232);
}
static void aim_guard(void) {
    printf("-- corrección de puntería: armas sin boca y apuntado intermitente --\n");
    srand(99);
    float q[4], p[3] = {0.2f, 1.2f, -0.3f}, aq[4], rel[4] = {sinf(-0.30543f), 0, 0, cosf(-0.30543f)};      /* apuntado = agarre girado -35 grados sobre X */
    t_rand_quat(q); t_qmul(q, rel, aq); t_qnorm(aq);
    float base[4], off[4];
    /* granada (2) y adhesivo (6): con apuntado válido la mano no gira (el desvío total sigue siendo el de siempre) */
    int gids[2] = {2, 6};
    for (int i = 0; i < 2; i++) {
        world(gids[i], i == 0 ? 0 : 1, 3);
        fresh_weapon(gids[i]);
        avbl(p, q, NULL, 0.0f, 3);
        vrbody_hand_offset(1, base);
        avbl(p, q, aq, 0.0f, 12);
        vrbody_hand_offset(1, off);
        MvVrGameStatus st; mv_vrgame_status(&st);
        CHECK(st.active && st.weapon == gids[i], "arma %d en la mano", gids[i]);
        CHECK(q_angle_deg(base, off) < 0.01f, "arma %d (sin boca real) con apuntado válido: la mano no se gira (%.3f grados)", gids[i], q_angle_deg(base, off));
    }
    /* pistola: converge, mantiene 0,25 s sin apuntado y luego lo retira sin saltos */
    world(0, 12, 60); fresh_pistol_meshes(); mv_vrgame_set_options(1, 2);
    avbl(p, q, NULL, 0.0f, 4);
    vrbody_hand_offset(1, base);
    avbl(p, q, aq, 0.0f, 10);
    vrbody_hand_offset(1, off);
    float conv = q_angle_deg(base, off);
    /* Giro esperado = ángulo del giro que lleva el marco del arma sin corregir (R(agarre) R(cabeceo) R(alabeo) M R(zócalo FIT_SOCOM_GRIP)^T, expected_weapon_rot) al marco del apuntado (agarre girado -35 grados): boca Y
     * alabeo (contrato cambiado: antes solo la boca, 9,7 grados, y el arma conservaba el alabeo de 45 de la mano). Depende de la tabla y del giro de mano, no de una constante. Se exige que el
     * giro sea el de la tabla (+-0,5 grados) y que no sea despreciable (> 1 grado: la corrección actúa). */
    float R0[9], R0t[9], Ra0[9], Rrel[9];
    expected_weapon_rot(1, q, R0); t_qmat(aq, Ra0); t_mtr(R0, R0t); t_mmul(R0t, Ra0, Rrel);
    const float conv_exp = acosf(fminf(1.0f, fmaxf(-1.0f, (Rrel[0] + Rrel[4] + Rrel[8] - 1.0f) * 0.5f))) * 57.29578f;
    CHECK(conv > 1.0f && fabsf(conv - conv_exp) < 0.5f, "pistola con apuntado: la mano se gira para alinear la boca y el alabeo (%.1f grados; la tabla del zócalo y el giro de mano dan %.1f)", conv, conv_exp);
    float prev[4]; memcpy(prev, off, sizeof prev);
    avbl(p, q, NULL, 0.0f, 3);                                    /* un parpadeo del apuntado (~25 ms) */
    vrbody_hand_offset(1, off);
    CHECK(q_angle_deg(prev, off) < 0.01f, "apuntado perdido 3 fotogramas: la corrección se mantiene (%.3f grados de salto)", q_angle_deg(prev, off));
    avbl(p, q, aq, 0.0f, 3);                                      /* vuelve */
    vrbody_hand_offset(1, off);
    CHECK(q_angle_deg(prev, off) < 0.1f, "y al volver sigue igual (%.3f grados)", q_angle_deg(prev, off));
    float worst_step = 0;
    memcpy(prev, off, sizeof prev);
    for (int i = 0; i < 150; i++) {                              /* apuntado perdido de verdad (~1,2 s): retirada suave */
        avbl(p, q, NULL, 0.0f, 1);
        vrbody_hand_offset(1, off);
        float st = q_angle_deg(prev, off); if (st > worst_step) worst_step = st;
        memcpy(prev, off, sizeof prev);
    }
    CHECK(q_angle_deg(base, off) < 0.5f, "pasado el tiempo sin apuntado la corrección se retira del todo (%.3f grados)", q_angle_deg(base, off));
    CHECK(worst_step < conv * 0.6f, "la retirada no es un salto: el mayor paso por fotograma es %.1f grados de %.1f", worst_step, conv);
    printf("   corrección %.1f grados; mayor paso al retirarla %.1f grados por VBlank\n", conv, worst_step);
}

static void rv_extra(void) {
    printf("-- RV: gatillo mantenido durante la recarga --\n");
    gm_rl = 0; world(0, 0, 12); fresh_pistol(); vbl(0.0f, 20);
    uint32_t seen = shots_now(), s00 = seen; sq_seen = 0;
    for (int i = 0; i < 150; i++) { vbl_game(0.9f, 1, &seen); if (shots_now() != s00) { printf("   RV disparo en VBlank %d tras pulsar (mag %d)\n", i, bs.magazine); break; } }
    printf("   RV mantenido 150: disparos=%u mag=%d vr=%d\n", shots_now() - s00, bs.magazine, vr_held());
    CHECK(shots_now() == s00, "RV: mantener el gatillo durante la recarga no dispara al acabar (%u)", shots_now() - s00);
    vbl_game(0.0f, 40, &seen);
    printf("-- RV: zona de menu con el arma seca --\n");
    gm_rl = 0; world(0, 12, 24); fresh_pistol();
    { MvVrHand h; memset(&h, 0, sizeof h); h.valid = 1; h.pos[1] = 1.0f; h.rot[3] = 1.0f; mv_vrgame_set_hand(1, &h); vrgame_test_advance(8000000ull); vrgame_pad(0, &bs, ram);
      float p[3]; if (vrgame_hand_raw(1, p, NULL, NULL, NULL)) { zone_off[0] = p[0]; zone_off[1] = p[1] - 1.0f; zone_off[2] = p[2]; } }
    bs.magazine = 0; bs.weapon_ammo[0] = 12; zones(0.6f, 0.0f, 100);
    zones(0.6f, 0.9f, 12); const int sq_out = sq_seen; zones(0.6f, 0.0f, 60); sq_seen = 0;
    zones(0.0f, 0.0f, 8); zones(0.0f, 0.9f, 12);
    printf("   RV zona seca: cuadrado dentro=%d fuera=%d\n", sq_seen, sq_out);
    CHECK(sq_seen > 0 && sq_out > 0, "RV: gatillo en la zona con el arma seca pulsa el ataque como fuera (dentro %d, fuera %d)", sq_seen, sq_out);
    zones(0.0f, 0.0f, 8); zone_pad = PAD_R1; zones(0.0f, 0.0f, 45); sq_seen = 0; zones(0.0f, 0.9f, 12);
    CHECK(sq_seen == 0, "RV: con la ventana abierta con el grip el gatillo con el arma seca no pulsa el ataque (cuadrado %d)", sq_seen);
    zone_pad = 0; zones(0.6f, 0.0f, 60);
}

int main(void) {
    vrgame_test_clock(1000000000ull);                               /* tiempo simulado: los dt de vrgame_pad y la antigüedad de las manos no dependen del reloj real */
    VrBodyConfig bc;
    vrbody_config_default(&bc);
    vrbody_init(&bc);
    memset(&rec, 0, sizeof rec);
    rec.serial = 1; rec.active = 1; rec.units_per_m = 1000.0f; rec.screen = 160;
    rec.M[0] = rec.M[4] = rec.M[8] = 1.0f; rec.p_ref[1] = 1.6f; rec.eye[1] = -1600;
    uint32_t pc = 0x80100000u;                                      /* puntero de control del jugador: el bloque del juego queda dentro de la RAM */
    memcpy(ram + (BA_PLAYER_CTRL & 0x1FFFFFu), &pc, 4);

    scenario("cargador 12 con reserva 36", 12, 48);
    scenario("cargador 12 sin reserva", 12, 12);                    /* antes el cargador VR arrancaba vacío (reserva 0) y el gatillo solo sonaba en seco */
    scenario("cargador 5 con reserva 2", 5, 7);
    menu_zone_blocks_fire();

    printf("-- sin arma equipada --\n");
    world(-1, 0, 0);
    fresh_pistol();
    world(-1, 0, 0);
    MvVrGameStatus st; mv_vrgame_status(&st);
    uint32_t base = st.shots;
    VrFireStats fs;
    vbl(0.0f, 5); vbl(0.9f, 10);
    mv_vrgame_status(&st); vrfire_stats(&fs);
    CHECK(!st.active && st.shots == base && sq_seen == 0 && fs.requests == 0, "sin arma: ni disparo ni cuadrado (activo %d, disparos %u, cuadrado %d)", st.active, st.shots - base, sq_seen);

    printf("-- arma equipada sin munición en el juego --\n");
    world(0, 0, 0);
    fresh_pistol();
    mv_vrgame_status(&st); base = st.shots;
    vbl(0.0f, 5); vbl(0.9f, 10);
    mv_vrgame_status(&st);
    CHECK(st.shots == base && sq_seen == 0, "sin munición: no sale disparo (%u) ni cuadrado (%d)", st.shots - base, sq_seen);

    grenade_gesture();
    throw_bench();
    weapon_follows_controller();
    grenade_models();
    menu_zone_blocks_grenade();
    c4_ray_placement();
    c4_remote_grip();
    c4_direct_chain();
    c4_remote_chest();
    stinger_hud_only_on_led();
    led_views_hide_hud();
    special_view_synced_to_drawn_model();
    nikita_native_hud();
    rumble_routing();
    mine_rests_on_floor();
    mine_real_floor();
    mine_orientation();
    mine_ghost_fixed();
    mine_no_ray_hit_rejects();
    duct_hides_weapon();
    equip_uses_new_turn();
    stinger_hidden_body_and_led_gesture();
    dominant_switch_welded();
    psg1_prone_keeps_weapon();
    psg1_autofocus_wiring();
    binocular_hides_weapon();
    hand_frames();
    laser_aims_with_bullet();
    laser_ignores_recoil();
    aim_pose();
    aim_upright();
    aim_guard();
    weld_firearm();
    prone_releases_support_hand();
    mirror_game_magazine();
    ammo_pickup_after_empty();
    ammo_pickup_direct();
    raise_skipped();
    special_view_waits_on_weapon_change();
    special_view_follows_hand();
    laser_stops_at_walls();       /* al final: consume números aleatorios y las pruebas anteriores dependen de la secuencia */

    printf("%s vrgame_fire: %d comprobaciones, %d fallos\n", fails ? "FALLO" : "OK", checks, fails);
    rv_extra();
    return fails ? 1 : 0;
}
