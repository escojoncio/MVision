/* Truco «Dar todas las armas» (vrgame_cheat_weapons_apply y la petición mv_vrgame_cheat_all_weapons consumida en vrgame_pad) sobre una RAM falsa: antes sin armas, después todas con su
 * máximo, nada fuera de las dos tablas de 10 s16; la petición se aplica una vez y sin partida se descarta. make -C vr/host test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bridge.h"
#include "bridge_addrs.h"
#include "host.h"
#include "vr_camera.h"
#include "vrgame.h"

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
float mv_vrhost_ray_distance(const float o[3], const float d[3], float mx) { (void)o; (void)d; (void)mx; return -1; }
int vr_scene_build(void) { return 0; }
void vr_scene_opts_default(void) {}
int vrmelee_rt_installed(void) { return 0; }
void vrmelee_rt_frame(const void *f) { (void)f; }

static uint8_t ram[BRIDGE_RAM_SIZE], ref[BRIDGE_RAM_SIZE];
static BridgeState bs;

#define OFF_AMMO ((BA_VARS + 2u * BRIDGE_V_WEAPON_AMMO) & 0x1FFFFFu)
#define OFF_MAX  ((BA_VARS + 2u * BRIDGE_V_WEAPON_MAX) & 0x1FFFFFu)
static int16_t g16(uint32_t off) { int16_t v; memcpy(&v, ram + off, 2); return v; }
static void p16(uint32_t off, int v) { int16_t s = (int16_t)v; memcpy(ram + off, &s, 2); }

/* estado inicial real de un volcado (b's01a / b's03d): sin armas salvo las dadas, máximos 25,0,8,0,0,0,4,3,3,0 */
static void fresh(void) {
    static const int mx[10] = {25, 0, 8, 0, 0, 0, 4, 3, 3, 0};
    for (size_t i = 0; i < sizeof ram; i++) ram[i] = (uint8_t)(i * 2654435761u >> 24);     /* relleno no trivial: detecta cualquier escritura fuera de la tabla */
    for (int i = 0; i < 10; i++) { p16(OFF_AMMO + 2 * i, -1); p16(OFF_MAX + 2 * i, mx[i]); }
    memcpy(ref, ram, sizeof ram);
}
/* solo cambian las dos tablas: el resto de la RAM idéntico */
static int outside_same(void) {
    for (size_t i = 0; i < sizeof ram; i++) {
        if (i >= OFF_AMMO && i < OFF_AMMO + 20) continue;
        if (i >= OFF_MAX && i < OFF_MAX + 20) continue;
        if (ram[i] != ref[i]) return 0;
    }
    return 1;
}

int main(void) {
    /* direcciones: GM_Weapons = linkvarbuf[17..26] y GM_WeaponsMax = linkvarbuf[27..36] (source/include/linkvar.h) con linkvarbuf = BA_VARS */
    CHECK(BA_VARS + 2u * BRIDGE_V_WEAPON_AMMO == 0x800B5F4Au && BA_VARS + 2u * BRIDGE_V_WEAPON_MAX == 0x800B5F5Eu, "direcciones %08x %08x", BA_VARS + 2u * BRIDGE_V_WEAPON_AMMO, BA_VARS + 2u * BRIDGE_V_WEAPON_MAX);

    /* aplicar directo */
    fresh();
    for (int i = 0; i < 10; i++) CHECK(g16(OFF_AMMO + 2 * i) == -1, "antes: arma %d no poseída", i);
    CHECK(vrgame_cheat_weapons_apply(NULL) == 0, "ram NULL");
    CHECK(vrgame_cheat_weapons_apply(ram) == 10, "diez armas");
    static const int want_max[10] = {25, 450, 8, 8, 8, 8, 4, 3, 3, 15};
    for (int i = 0; i < 10; i++) {
        CHECK(g16(OFF_AMMO + 2 * i) == want_max[i] - 1, "arma %d munición %d (máximo - 1: las cajas de munición no dan FULL)", i, g16(OFF_AMMO + 2 * i));
        CHECK(g16(OFF_MAX + 2 * i) == want_max[i], "arma %d máximo %d", i, g16(OFF_MAX + 2 * i));
    }
    CHECK(outside_same(), "nada fuera de las dos tablas");
    /* idempotente */
    static uint8_t snap[BRIDGE_RAM_SIZE]; memcpy(snap, ram, sizeof ram);
    vrgame_cheat_weapons_apply(ram);
    CHECK(memcmp(snap, ram, sizeof ram) == 0, "idempotente");

    /* un máximo del juego mayor que la capacidad provisional se respeta (no se baja); poseída con munición baja sube al máximo; bit 0x8000 (bloqueada) se conserva */
    fresh();
    p16(OFF_MAX + 0, 200); p16(OFF_AMMO + 0, 12);          /* pistola 12/200 */
    p16(OFF_MAX + 2 * 2, 30); p16(OFF_AMMO + 2 * 2, 0x8000 | 5);      /* granada bloqueada con 5 */
    vrgame_cheat_weapons_apply(ram);
    CHECK(g16(OFF_AMMO + 0) == 199 && g16(OFF_MAX + 0) == 200, "máximo del juego respetado (%d/%d)", g16(OFF_AMMO + 0), g16(OFF_MAX + 0));
    CHECK((uint16_t)g16(OFF_AMMO + 4) == (0x8000u | 29u) && g16(OFF_MAX + 4) == 30, "bloqueada conserva el bit (%04x)", (uint16_t)g16(OFF_AMMO + 4));
    CHECK(outside_same(), "nada fuera (2)");

    /* máximo negativo (basura / aún sin cargar): se sustituye por la capacidad provisional, nunca una munición negativa ni -1 */
    fresh();
    p16(OFF_MAX + 0, -5); p16(OFF_MAX + 2 * 2, -32768);
    vrgame_cheat_weapons_apply(ram);
    CHECK(g16(OFF_MAX + 0) == 25 && g16(OFF_AMMO + 0) == 24, "máximo negativo: max %d munición %d", g16(OFF_MAX + 0), g16(OFF_AMMO + 0));
    CHECK(g16(OFF_MAX + 4) == 8 && g16(OFF_AMMO + 4) == 7, "máximo -32768: max %d munición %d", g16(OFF_MAX + 4), g16(OFF_AMMO + 4));
    /* máximo 1: queda poseída con 0 (no -1) */
    fresh();
    p16(OFF_MAX + 2 * 6, 1);
    vrgame_cheat_weapons_apply(ram);
    CHECK(g16(OFF_AMMO + 2 * 6) == 0 && g16(OFF_MAX + 2 * 6) == 1, "máximo 1: munición %d", g16(OFF_AMMO + 2 * 6));

    /* petición: sin partida (puente no válido) se descarta; con partida se aplica una sola vez */
    fresh();
    memset(&bs, 0, sizeof bs);
    mv_vrgame_cheat_all_weapons();
    vrgame_pad(0, &bs, ram);
    CHECK(g16(OFF_AMMO) == -1, "sin partida: sin efecto");
    bs.valid = BRIDGE_OK_ALL_CORE; bs.has_player = 1; bs.in_control = 1; bs.weapon = -1;
    vrgame_pad(0, &bs, ram);
    CHECK(g16(OFF_AMMO) == -1, "la petición sin partida se descartó, no queda pendiente");
    mv_vrgame_cheat_all_weapons();
    vrgame_pad(0, &bs, ram);
    for (int i = 0; i < 10; i++) CHECK(g16(OFF_AMMO + 2 * i) == want_max[i] - 1, "con partida: arma %d = %d", i, g16(OFF_AMMO + 2 * i));
    CHECK(outside_same(), "nada fuera de las tablas tras vrgame_pad");
    /* sin el control del jugador (cinemática, etc.) también se descarta */
    fresh();
    bs.in_control = 0;
    mv_vrgame_cheat_all_weapons();
    vrgame_pad(0, &bs, ram);
    CHECK(g16(OFF_AMMO) == -1, "sin control: sin efecto");
    bs.in_control = 1;
    vrgame_pad(0, &bs, ram);
    CHECK(g16(OFF_AMMO) == -1, "la petición sin control se descartó, no queda pendiente");
    mv_vrgame_cheat_all_weapons();
    vrgame_pad(0, &bs, ram);
    CHECK(g16(OFF_AMMO) == 24, "con control: aplicada");
    p16(OFF_AMMO + 2 * 3, -1);                              /* el juego la gasta/quita: una segunda llamada a vrgame_pad no la repone (acción única) */
    vrgame_pad(0, &bs, ram);
    CHECK(g16(OFF_AMMO + 2 * 3) == -1, "acción única");

    printf("test_cheat_weapons: %d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
