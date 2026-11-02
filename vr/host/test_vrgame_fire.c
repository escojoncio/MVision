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
#include "vr_body.h"
#include "vr_fire.h"
#include "vr_hands.h"
#include "vr_grip.h"
#include "vrgame.h"
#include "w_laser.h"
#include "w_throw.h"
#include "wcore.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO l.%d: %s ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- sustitutos de lo que vrgame enlaza del anfitrión ---- */
static VrCamRecord rec;
static VrCamConfig ccfg;
int vrcam_active(void) { return 1; }
const VrCamConfig *vrcam_config(void) { return &ccfg; }
const VrCamRecord *vrcam_last_record(void) { return &rec; }
int vrcam_pad_origin(void) { return 0; }
void vrcam_set_zoom_cap(int cap) { (void)cap; }
float vrcam_view_yaw_deg(void) { return 0; }
int vrcam_zoom_capped(void) { return 0; }
int cd_current_disc(void) { return 0; }
int cd_current_source(char *p, size_t n, int *d) { (void)p; (void)n; (void)d; return -1; }
uint8_t *disc_load_stage_archive(const char *p, int d, size_t *n) { (void)p; (void)d; (void)n; return NULL; }
int mv_vr_get_head(float m[16], double *t) { static const float h[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1.6f, 0, 1}; memcpy(m, h, sizeof h); if (t) *t = 0; return 1; }
int mv_vrhost_hands_active(void) { return 0; }
float mv_vrhost_wall_distance(const float p[3], float mx) { (void)p; (void)mx; return -1; }
static float ray_stub_mm = -1;      /* geometría sólida de vr_headcol sustituida: impacto a esta distancia (mm del motor), -1 ninguno */
float mv_vrhost_ray_distance(const float o[3], const float d[3], float mx) { (void)o; (void)d; return ray_stub_mm >= 0 && ray_stub_mm <= mx ? ray_stub_mm : -1; }
int vr_scene_build(void) { return 0; }
void vr_scene_opts_default(void) {}
int vrmelee_rt_installed(void) { return 0; }
void vrmelee_rt_frame(const void *f) { (void)f; }

#define PAD_SQUARE (1u << 15)
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
    printf("   estado: cargador VR %d recamara %d insertado %d recarga %d\n", st0.mag_rounds, st0.chambered, st0.mag_inserted, st0.reload_state);
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

    printf("-- granada: flojo llega cerca, fuerte lejos (velocidad proporcional a la del mando, de punta a punta) --\n");
    int16_t v_soft[3], v_mid[3], v_hard[3];
    for (int i = 0; i < 60; i++) gvbl(0.2f, 0.0f, 0.0f, 0);                       /* sale la siguiente */
    int h_soft = throw_with_trigger(0.0045f, v_soft);
    for (int i = 0; i < 60; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    int h_mid = throw_with_trigger(0.020f, v_mid);
    for (int i = 0; i < 60; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    int h_hard = throw_with_trigger(0.080f, v_hard);
    printf("   velocidad horizontal por tick: suave %d, media %d, fuerte %d\n", h_soft, h_mid, h_hard);
    CHECK(h_soft >= 0 && h_mid >= 0 && h_hard >= 0, "tres lanzamientos con datos");
    CHECK(h_soft < h_mid && h_mid < h_hard, "más rápido el mando, más velocidad en el motor");
    CHECK(h_hard > 4 * h_soft + 40 && h_hard > 150, "fuerte (~10 m/s) sale mucho más rápido que flojo (~0,6 m/s): %d frente a %d", h_hard, h_soft);

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
static float t_ang(const float a[3], const float b[3]) { float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; return acosf(fminf(1.0f, fmaxf(-1.0f, d))) * 57.29578f; }

static WMesh syn_mesh[16];
static WeaponState *syn_state;
static const WeaponDef *syn_def;
static void fresh_pistol_meshes(void) {      /* pistola del registro con una malla mínima por pieza: sus frames no salen vacíos */
    static WeaponState *old;
    if (old) { free(old); old = NULL; }
    syn_def = NULL;
    for (int i = 0; i < w_registry_count; i++) if (w_registry[i] && w_registry[i]->game_weapon_id == 0) syn_def = w_registry[i];
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
    vrgame_test_install(0, (void *)syn_def, st, rig);
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
static int drawn_weapon(float dir[3], float org[3]) {
    VrHandsFrame f;
    int rc = mv_host_hands(&f);
    if (!rc || f.nmeshes < 1) { mv_host_hands_done(); return 0; }
    const float *m = f.meshes[0].model;      /* piezas por orden de def->parts: la 0 es la raíz */
    dir[0] = -m[8]; dir[1] = -m[9]; dir[2] = -m[10];
    const WPose *rest = &syn_def->parts[0].rest;
    for (int i = 0; i < 3; i++) org[i] = m[12 + i] - (m[i] * rest->p.x + m[4 + i] * rest->p.y + m[8 + i] * rest->p.z);
    mv_host_hands_done();
    return 1;
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
    CHECK(maxpos < 0.003f, "el punto de empuñadura del arma cae en el agarre del mando: %.4f m", maxpos);

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

/* ---- granadas: modelo por id de juego y dibujo VR / nativo ---- */
static void grenade_models(void) {
    printf("-- granadas (ids 2, 7, 8): un modelo VR por id, la sin modelo con el arma del juego --\n");
    CHECK(vrgame_weapon_model(2) == 0x3b88, "granada de fragmentación (id 2): modelo %04x", vrgame_weapon_model(2));
    CHECK(vrgame_weapon_model(8) == 0x761a, "granada cilíndrica (id 8): modelo %04x", vrgame_weapon_model(8));
    CHECK(vrgame_weapon_model(7) == 0, "id 7 sin modelo identificado: se queda la del juego (nunca la de otra granada)");
    CHECK(vrgame_weapon_model(2) != vrgame_weapon_model(8) && vrgame_weapon_model(2) != vrgame_weapon_model(0), "los modelos de granada difieren entre sí y de la pistola");
    CHECK(vrgame_weapon_model(0) == 0x36f6 && vrgame_weapon_model(5) == 0x17a1 && vrgame_weapon_model(99) == 0 && vrgame_weapon_model(-1) == 0, "pistola, mina y ids fuera de tabla");
    /* política de modelo: el modelo de granada que usa el juego por tipo no está identificado en los datos (los de WPN_TAB son «dudosa», y el de la 8 salía blanco
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
    vrgame_test_set_native(2, 0);
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
            CHECK((m->flags & VRH_MF_DYNAMIC) && m->tex_id == 0 && m->nverts == WLASER_NV && m->nidx == WLASER_NI, "malla láser dinámica, sin textura, topología fija");
        }
    }
    mv_host_hands_done();
    return found;
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
static void laser_stops_at_walls(void) {
    printf("-- láser: se acorta en el primer impacto con el escenario --\n");
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
    CHECK(drawn_laser(org, ax, &len) == 1 && fabsf(len - WLASER_MAX_M) < 1e-3f, "sin datos de colisión: 15 m (%.3f)", len);
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
    CHECK(drawn_laser(org, ax, &len) == 1 && fabsf(len - WLASER_MAX_M) < 1e-3f, "pared a 20 m (fuera de alcance): 15 m (%.3f)", len);
    float back[3] = {-dw[0], -dw[1], -dw[2]};
    hzd_wall(ow, back, 3.0f * map.units_per_m); lvbl(p, q, 0.0f, 2);
    CHECK(drawn_laser(org, ax, &len) == 1 && fabsf(len - WLASER_MAX_M) < 1e-3f, "pared a la espalda: no corta (%.3f)", len);
    /* geometría sólida (suelo, techo, puertas) más cerca que la pared HZD: gana la menor */
    hzd_wall(ow, dw, 6.0f * map.units_per_m);
    ray_stub_mm = 1.5f * map.units_per_m; lvbl(p, q, 0.0f, 2);
    CHECK(drawn_laser(org, ax, &len) == 1 && fabsf(len - (1.5f - WLASER_SURF_GAP_M)) < 0.01f, "sólido a 1,5 m y pared a 6 m: %.3f m", len);
    ray_stub_mm = -1; lvbl(p, q, 0.0f, 2);
    CHECK(drawn_laser(org, ax, &len) == 1 && fabsf(len - (6.0f - WLASER_SURF_GAP_M)) < 0.01f, "sin sólido: vuelve a la pared HZD a 6 m: %.3f m", len);
    ray_stub_mm = 3.0f * map.units_per_m; w32r(0x800ACB84, 0); lvbl(p, q, 0.0f, 2);
    CHECK(drawn_laser(org, ax, &len) == 1 && fabsf(len - (3.0f - WLASER_SURF_GAP_M)) < 0.01f, "solo sólido a 3 m (sin HZD): %.3f m", len);
    ray_stub_mm = 0.02f * map.units_per_m; lvbl(p, q, 0.0f, 2);
    CHECK(drawn_laser(org, ax, &len) == 0, "superficie a 2 cm de la boca: nada que dibujar");
    ray_stub_mm = -1; lvbl(p, q, 0.0f, 2);
    CHECK(drawn_laser(org, ax, &len) == 1 && fabsf(len - WLASER_MAX_M) < 1e-3f, "espacio abierto otra vez: 15 m (%.3f)", len);
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
    float worst_w = 0, worst_l = 0, worst_b = 0, worst_g = 0, worst_nofix = 0, worst_back = 0;
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
    CHECK(shots == 12 && worst_b < 0.15f, "la bala sale por el apuntado del mando: %.3f grados, %d disparos (< 0,15)", worst_b, shots);
    CHECK(worst_g < 0.003f, "la empuñadura sigue en el agarre del mando: %.4f m", worst_g);
    CHECK(worst_back < 0.5f, "sin apuntado válido vuelve el desvío fijo: %.3f grados", worst_back);
    rec = saved;
}

/* ---- la corrección de puntería: solo armas con boca real, y sin saltos si el apuntado falta un instante ---- */
static float q_angle_deg(const float a[4], const float b[4]) { float d = fabsf(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]); return 2.0f * acosf(fminf(1.0f, d)) * 57.29578f; }
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
    CHECK(conv > 10.0f, "pistola con apuntado: la mano se gira para alinear la boca (%.1f grados)", conv);
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
    weapon_follows_controller();
    grenade_models();
    laser_aims_with_bullet();
    aim_pose();
    aim_guard();
    laser_stops_at_walls();       /* al final: consume números aleatorios y las pruebas anteriores dependen de la secuencia */

    printf("%s vrgame_fire: %d comprobaciones, %d fallos\n", fails ? "FALLO" : "OK", checks, fails);
    return fails ? 1 : 0;
}
