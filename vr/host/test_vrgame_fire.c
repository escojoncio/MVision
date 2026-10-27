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

    printf("%s vrgame_fire: %d comprobaciones, %d fallos\n", fails ? "FALLO" : "OK", checks, fails);
    return fails ? 1 : 0;
}
