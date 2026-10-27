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
    for (int i = 0; i < n; i++) { usleep(8000); set_hand(trig, 1.0f);   /* el arma avanza con el reloj real: ~8 ms por VBlank de prueba */ uint16_t o = vrgame_pad(0, &bs, ram); if (o & PAD_SQUARE) sq_seen++; }
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
    usleep(8000);
    uint16_t o = vrgame_pad(pad, &bs, ram);
    if (o & PAD_SQUARE) sq_seen++;
    return o;
}
static void grenade_gesture(void) {
    printf("-- granada: mano cerrada = listo, abrirla = lanzar --\n");
    world(2, 0, 3);
    fresh_weapon(2);
    for (int i = 0; i < 6; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    MvVrGameStatus st; mv_vrgame_status(&st);
    CHECK(st.active && st.weapon == 2, "granada VR mostrada en la mano");
    CHECK(sq_seen == 0 && st.throws == 0, "mano abierta: ni botón del juego ni lanzamiento");
    for (int i = 0; i < 10; i++) gvbl(0.2f, 1.0f, 0.0f, 0);
    CHECK(inst_state->phase == WT_PH_READY, "mano cerrada: lista (fase %d)", inst_state->phase);
    CHECK(sq_seen > 5, "el botón de ataque del juego se mantiene mientras está lista (%d VBlanks)", sq_seen);
    for (int i = 0; i < 10; i++) gvbl(0.2f + 0.045f * (float)i, 1.0f, 0.0f, 0);     /* el brazo avanza ~5 m/s */
    gvbl(0.2f + 0.045f * 10, 0.6f, 0.0f, 0); gvbl(0.2f + 0.045f * 11, 0.1f, 0.0f, 0);
    for (int i = 0; i < 4; i++) gvbl(0.2f + 0.045f * 11, 0.0f, 0.0f, 0);
    mv_vrgame_status(&st);
    CHECK(st.throws == 1 && st.rejects == 0, "abrir la mano: un lanzamiento aceptado (%u, rechazos %u)", st.throws, st.rejects);
    int16_t pos[3], vel[3];
    CHECK(vrfire_take_throw(pos, vel) == 1, "datos de lanzamiento listos para el juego");
    printf("   velocidad por tick %d %d %d (posición %d %d %d)\n", vel[0], vel[1], vel[2], pos[0], pos[1], pos[2]);
    CHECK(abs(vel[0]) + abs(vel[1]) + abs(vel[2]) > 40, "la velocidad de la mano llega al motor");
    for (int i = 0; i < 40; i++) gvbl(0.7f, 0.0f, 0.0f, 0);
    CHECK(inst_state->phase != WT_PH_READY, "tras lanzar, la mano ya no está lista (fase %d)", inst_state->phase);

    printf("-- granada lista y arma fuera: no se lanza con la velocidad del juego --\n");
    world(2, 0, 3);
    fresh_weapon(2);
    for (int i = 0; i < 6; i++) gvbl(0.2f, 0.0f, 0.0f, 0);
    for (int i = 0; i < 10; i++) gvbl(0.2f, 1.0f, 0.0f, 0);
    CHECK(inst_state->phase == WT_PH_READY && sq_seen > 0, "granada lista con el botón del juego mantenido (fase %d)", inst_state->phase);
    int16_t cpos[3], cvel[3];
    vrfire_take_throw(cpos, cvel);                                   /* vacía lo que dejó el lanzamiento anterior */
    world(-1, 0, 0);                                                 /* el arma se desequipa con la mano aún cerrada */
    for (int i = 0; i < 6; i++) gvbl(0.2f, 1.0f, 0.0f, 0);
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
            usleep(2000);
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
            usleep(2000);
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

int main(void) {
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

    printf("%s vrgame_fire: %d comprobaciones, %d fallos\n", fails ? "FALLO" : "OK", checks, fails);
    return fails ? 1 : 0;
}
