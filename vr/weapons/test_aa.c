/* Pruebas del lanzamisiles antiaéreo: reflejo del buscador del juego (estado, progreso, háptico), disparo y recarga.
 * El fijado de blanco lo hace el juego (vr/gameplay/vr_aa.h): aquí solo se comprueba que el arma NO inventa un buscador.
 * Compilar: cc -I. -Wall -O1 test_aa.c w_aa.c wcore.c -lm -o test_aa (sin mallas compiladas) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "w_aa.h"
#include "wcore.h"

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO l.%d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- puente simulado ---- */
static struct {
    int reserve, fired, takes, steers, dry, tone, locked, mag_in, mag_out, haptics, targets_calls;
    int ntg; WVec3 tg[4];
    int proj_active; WPose proj;
    float last_yaw, last_pit;
    WPose last_muzzle;
} G;
static int b_reserve(int id) { (void)id; return G.reserve; }
static void b_take(int id, int n) { (void)id; G.reserve -= n; G.takes += n; }
static void b_fire(int id, WPose m) { (void)id; G.fired++; G.last_muzzle = m; }
static void b_hap(int h, float s, float t) { (void)h; (void)s; (void)t; G.haptics++; }
static void b_snd(int id, int ev) {
    (void)id;
    if (ev == WS_DRY) G.dry++; else if (ev == WS_LOCK_TONE) G.tone++; else if (ev == WS_LOCKED) G.locked++;
    else if (ev == WS_MAG_IN) G.mag_in++; else if (ev == WS_MAG_OUT) G.mag_out++;
}
static int b_targets(WVec3 *o, int max) { G.targets_calls++; int n = G.ntg < max ? G.ntg : max; for (int i = 0; i < n; i++) o[i] = G.tg[i]; return n; }
static int b_proj(int id, WPose *o) { (void)id; if (G.proj_active) *o = G.proj; return G.proj_active; }
static void b_steer(int id, float y, float p) { (void)id; G.steers++; G.last_yaw = y; G.last_pit = p; }
static WGameBridge BR = { b_reserve, b_take, b_fire, b_hap, b_snd, b_targets, b_proj, b_steer, NULL };

static AAState st;
static WInput in;
static WeaponState *S = &st.base;

static void reset(int reserve) {
    memset(&G, 0, sizeof G); G.reserve = reserve;
    memset(&st, 0, sizeof st); memset(&in, 0, sizeof in);
    w_aa.init(S);
    in.head.p = wv(0, 1.6f, 0); in.head.q = wquat_identity();
    in.hand[0].p = wv(0.2f, 1.3f, -0.3f); in.hand[0].q = wquat_identity();
    in.hand[1].p = wv(0.1f, 1.0f, -0.6f); in.hand[1].q = wquat_identity();
    in.dt = 1.0f / 90;
}
static void step(WReloadMode rm, float secs) {
    int n = (int)(secs * 90 + 0.5f);
    for (int i = 0; i < n; i++) w_aa.update(S, &in, rm, &BR);
}
/* Estado del buscador del juego que el anfitrión pasaría: visor levantado, con/sin blanco, fotogramas con el mismo blanco. */
static void game_lock(int sight, int target, int frames) { AAGameLock g = {sight, target, frames}; aa_feed_game_lock(S, &g); }
static WPose magwell(void) { return wcore_point_world(S, &in, WP_MAGWELL); }

int main(void) {
    /* ---------- 1. arranque ---------- */
    reset(3);
    step(WR_MANUAL, 0.05f);
    CHECK(S->phase == AA_P_READY && S->mag_rounds == 1 && G.reserve == 2 && G.takes == 1, "carga inicial fase=%d res=%d", S->phase, G.reserve);
    CHECK(aa_lock_state(S) == AA_LK_STBY, "tubo listo, visor sin levantar (estado %d)", aa_lock_state(S));
    CHECK(S->aux.active == 0, "sin vista auxiliar: la mira es la del juego");
    CHECK(G.targets_calls == 0 && G.tone == 0 && G.locked == 0, "el arma no consulta blancos ni emite tonos de fijación (los hace el juego)");
    { WPose e = aa_sight_pose(S, &in); WPose e2 = wcore_point_world(S, &in, WP_EYE);
      CHECK(fabsf(e.p.x - e2.p.x) < 1e-6f && fabsf(e.p.y - e2.p.y) < 1e-6f && fabsf(e.p.z - e2.p.z) < 1e-6f && fabsf(e.q.w - e2.q.w) < 1e-6f, "aa_sight_pose = WP_EYE en mundo"); }

    /* ---------- 2. reflejo del buscador del juego ---------- */
    reset(3); step(WR_MANUAL, 0.1f);
    G.targets_calls = 0;
    game_lock(1, 0, 0); step(WR_MANUAL, 0.05f);
    CHECK(aa_lock_state(S) == AA_LK_SEARCH && aa_lock_progress(S) == 0, "buscador del juego activo sin blanco: buscando");
    int hap0 = G.haptics;
    game_lock(1, 1, 1); step(WR_MANUAL, 0.05f);
    CHECK(aa_lock_state(S) == AA_LK_ACQ, "el juego sigue un blanco: adquiriendo");
    CHECK(G.haptics > hap0, "háptico al empezar a seguir un blanco");
    game_lock(1, 1, 6); step(WR_MANUAL, 0.05f);
    CHECK(fabsf(aa_lock_progress(S) - 0.5f) < 1e-4f, "progreso = fotogramas / %d (%.3f)", AA_BOX_FRAMES, aa_lock_progress(S));
    game_lock(1, 1, 11); step(WR_MANUAL, 0.05f);
    CHECK(aa_lock_state(S) == AA_LK_ACQ && aa_lock_progress(S) < 1.0f, "11 fotogramas: marco aún abriéndose");
    hap0 = G.haptics;
    game_lock(1, 1, 12); step(WR_MANUAL, 0.05f);
    CHECK(aa_lock_state(S) == AA_LK_LOCKED && aa_lock_progress(S) == 1.0f, "12 fotogramas: marco cerrado");
    CHECK(G.haptics >= hap0 + 2, "háptico al cerrarse el marco (%d)", G.haptics - hap0);
    hap0 = G.haptics;
    game_lock(1, 1, 40); step(WR_MANUAL, 0.5f);
    CHECK(aa_lock_state(S) == AA_LK_LOCKED && G.haptics == hap0, "bloqueo estable, sin háptico repetido");
    CHECK(G.tone == 0 && G.locked == 0 && G.targets_calls == 0, "el arma sigue sin emitir tonos ni consultar blancos");
    game_lock(1, 0, 0); step(WR_MANUAL, 0.05f);
    CHECK(aa_lock_state(S) == AA_LK_SEARCH && aa_lock_progress(S) == 0, "el juego soltó el blanco");
    game_lock(0, 0, 0); step(WR_MANUAL, 0.05f);
    CHECK(aa_lock_state(S) == AA_LK_STBY, "visor bajado: en espera");
    aa_feed_game_lock(S, NULL); step(WR_MANUAL, 0.05f);
    CHECK(aa_lock_state(S) == AA_LK_STBY, "sin datos: en espera");

    /* ---------- 3. disparo (con bloqueo del juego) ---------- */
    reset(3); step(WR_MANUAL, 0.1f);
    game_lock(1, 1, 30); step(WR_MANUAL, 0.1f);
    CHECK(aa_lock_state(S) == AA_LK_LOCKED, "bloqueado");
    in.trigger[0] = 1.0f;
    step(WR_MANUAL, 0.05f);
    CHECK(G.fired == 1 && S->mag_rounds == 0 && S->phase == AA_P_SPENT, "disparo");
    CHECK(fabsf(G.last_muzzle.p.z - wcore_point_world(S, &in, WP_MUZZLE).p.z) < 0.2f, "boca de fuego");
    CHECK(aa_lock_state(S) == AA_LK_OFF, "tras el disparo el tubo está gastado");
    CHECK(G.steers == 0, "el arma no guía el misil (lo persigue el juego)");
    game_lock(1, 1, 40); step(WR_MANUAL, 0.1f);
    CHECK(aa_lock_state(S) == AA_LK_OFF, "con el tubo gastado el estado sigue apagado aunque el buscador del juego siga vivo");
    in.trigger[0] = 0; step(WR_MANUAL, 0.1f);
    in.trigger[0] = 1; int dry0 = G.dry; step(WR_MANUAL, 0.05f);
    CHECK(G.dry == dry0 + 1 && G.fired == 1, "gatillo en seco con tubo gastado");
    in.trigger[0] = 0; step(WR_MANUAL, 0.5f);

    /* ---------- 4. recarga manual completa ---------- */
    in.buttons[0] = WB_MAG_RELEASE; step(WR_MANUAL, 0.05f); in.buttons[0] = 0;
    CHECK(S->phase == AA_P_EJECT && G.mag_out == 1, "expulsión");
    step(WR_MANUAL, 0.2f);
    CHECK(S->channel[WCH_MAG] > 0.9f, "tubo desliza atrás");
    step(WR_MANUAL, 1.5f);
    CHECK(S->phase == AA_P_AWAIT && !S->mag_inserted && S->channel[WCH_CUSTOM0] >= 2.4f, "tubo gastado caído");
    /* mano de apoyo lejos de la espalda: no coge */
    in.grip[1] = 1.0f; step(WR_MANUAL, 0.1f);
    CHECK(S->phase == AA_P_AWAIT, "sin coger fuera de la zona");
    float rad; WVec3 z = aa_back_zone(&in, &rad);
    in.hand[1].p = z; step(WR_MANUAL, 0.1f);
    CHECK(S->phase == AA_P_CARRY, "coge tubo de la espalda");
    WPose sp; CHECK(aa_spare_pose(S, &in, &sp), "pose del tubo de repuesto");
    /* soltar -> vuelve a la espalda */
    in.grip[1] = 0; step(WR_MANUAL, 0.1f);
    CHECK(S->phase == AA_P_AWAIT, "soltar devuelve el tubo");
    in.grip[1] = 1; step(WR_MANUAL, 0.1f);
    CHECK(S->phase == AA_P_CARRY, "recoge de nuevo");
    /* desalineado: tubo cruzado no encaja */
    WPose mw = magwell();
    in.hand[1].p = wv_add(mw.p, wv(0, 0, 0.30f));
    in.hand[1].q = wquat_axis_angle(wv(0, 1, 0), 1.2f);
    step(WR_MANUAL, 0.2f);
    CHECK(S->phase == AA_P_CARRY, "desalineado no encaja");
    in.hand[1].q = wquat_identity();
    step(WR_MANUAL, 0.05f);
    CHECK(S->phase == AA_P_INSERT, "alineado: empieza encaje (fase %d)", S->phase);
    /* empujar */
    WVec3 h0 = in.hand[1].p;
    for (int i = 1; i <= 20; i++) {
        in.hand[1].p = wv(h0.x, h0.y, h0.z - 0.3f * i / 20);
        step(WR_MANUAL, 1.0f / 90);
        if (i == 10) CHECK(S->channel[WCH_MAG] > 0.3f && S->channel[WCH_MAG] < 0.7f, "encaje a mitad: %.2f", S->channel[WCH_MAG]);
    }
    CHECK(S->phase == AA_P_READY && S->mag_rounds == 1 && G.reserve == 1 && G.mag_in == 2, "tubo acoplado res=%d fase=%d", G.reserve, S->phase);
    CHECK(S->channel[WCH_MAG] == 0 && S->channel[WCH_CUSTOM0] == 0, "tubo en su sitio");
    in.grip[1] = 0;

    /* ---------- 5. disparo sin bloqueo ---------- */
    G.steers = 0; game_lock(1, 0, 0); step(WR_MANUAL, 0.1f);
    CHECK(aa_lock_state(S) == AA_LK_SEARCH, "buscando");
    in.trigger[0] = 1; step(WR_MANUAL, 0.05f);
    CHECK(G.fired == 2 && G.steers == 0, "sin bloqueo sale recto, sin guiado");
    in.trigger[0] = 0;
    /* disparo a la cadera (visor sin levantar): sale igual, sin bloqueo */
    reset(3); step(WR_MANUAL, 0.1f); in.trigger[0] = 0; step(WR_MANUAL, 0.05f); in.trigger[0] = 1; step(WR_MANUAL, 0.05f);
    CHECK(G.fired == 1 && aa_lock_state(S) == AA_LK_OFF, "a la cadera: el disparo sale");

    /* ---------- 7. sin reserva ---------- */
    reset(0); step(WR_MANUAL, 0.1f);
    CHECK(S->phase == AA_P_AWAIT && S->mag_rounds == 0 && aa_lock_state(S) == AA_LK_OFF, "sin munición: sin tubo");
    in.trigger[0] = 1; step(WR_MANUAL, 0.05f);
    CHECK(G.dry == 1 && G.fired == 0, "seco sin tubo");
    in.trigger[0] = 0;
    { float r; WVec3 z2 = aa_back_zone(&in, &r); in.hand[1].p = z2; in.grip[1] = 1; step(WR_MANUAL, 0.1f);
      CHECK(S->phase == AA_P_AWAIT && G.dry == 2, "no se puede coger tubo sin reserva"); in.grip[1] = 0; }

    /* ---------- 8. modos simplificado y automático ---------- */
    reset(3); step(WR_SIMPLIFIED, 1.2f); in.trigger[0] = 1; step(WR_SIMPLIFIED, 0.05f); in.trigger[0] = 0;
    step(WR_SIMPLIFIED, 2.0f);
    CHECK(S->phase == AA_P_SPENT, "simplificado espera el botón");
    in.buttons[0] = WB_MAG_RELEASE; step(WR_SIMPLIFIED, 0.05f); in.buttons[0] = 0;
    step(WR_SIMPLIFIED, 3.0f);
    CHECK(S->phase == AA_P_READY && S->mag_rounds == 1 && G.reserve == 1, "simplificado recarga solo (fase %d)", S->phase);
    reset(3); step(WR_AUTOMATIC, 1.2f); in.trigger[0] = 1; step(WR_AUTOMATIC, 0.05f); in.trigger[0] = 0;
    step(WR_AUTOMATIC, 3.5f);
    CHECK(S->phase == AA_P_READY && S->mag_rounds == 1 && G.reserve == 1, "automático recarga solo (fase %d)", S->phase);

    /* sin mallas compiladas: los huecos se asocian por nombre de pieza (weapon_part_mesh) */
    CHECK(w_aa.meshes == NULL && w_aa.nmeshes == 0 && w_aa.mesh_names, "w_aa no debe llevar mallas compiladas");
    { int n = weapon_mesh_count(&w_aa); CHECK(n == 8, "mesh_names: %d", n);
      reset(3);
      int sc = weapon_part_mesh(S, 8);
      CHECK(sc >= 0 && !strcmp(w_aa.mesh_names[sc], "screen"), "la pieza de pantalla resuelve su hueco por nombre");
      CHECK(weapon_part_mesh(S, 1) == -1, "tube_slide no tiene mesh");
      CHECK(wcore_part_find(S, "tube") == 2 && wcore_part_find(S, "nada") == -1, "wcore_part_find"); }

    /* ---------- 10. equipar con el gatillo apretado no dispara ---------- */
    reset(3); in.trigger[0] = 1.0f; step(WR_MANUAL, 1.5f);
    CHECK(G.fired == 0 && S->phase == AA_P_READY && S->mag_rounds == 1, "gatillo apretado al equipar disparó (%d)", G.fired);
    in.trigger[0] = 0; step(WR_MANUAL, 0.1f); in.trigger[0] = 1; step(WR_MANUAL, 0.1f);
    CHECK(G.fired == 1, "tras soltar y apretar sí dispara");

    /* ---------- 11. dt inválido y estado recién creado ---------- */
    reset(3);
    { const float dts[] = {0.0f, -1.0f, NAN, INFINITY, 5.0f};
      for (unsigned k = 0; k < sizeof dts / sizeof *dts; k++) { in.dt = dts[k]; step(WR_MANUAL, 0.1f); }
      in.dt = 1.0f / 90; }
    CHECK(isfinite(S->recoil_offset.p.x) && fabsf(S->recoil_offset.q.w) > 0.9f && isfinite(S->channel[WCH_MAG]), "estado no finito con dt inválido");
    reset(3); in.dt = 0; step(WR_MANUAL, 0.2f);
    CHECK(G.takes == 0 && S->phase == AA_P_BOOT, "dt=0 no debe avanzar el estado"); in.dt = 1.0f / 90;
    { AAState z; memset(&z, 0, sizeof z); w_aa.init(&z.base);          /* recién creado: cuaternión válido desde el primer cuadro */
      CHECK(z.base.recoil_offset.q.w == 1.0f && z.base.def == &w_aa, "init deja el cuaternión de retroceso en identidad");
      WPose W0 = wcore_weapon_world(&z.base, &in); CHECK(isfinite(W0.p.x) && fabsf(W0.q.w) > 0.9f, "pose del arma válida en el primer cuadro"); }
    printf("%s: %d comprobaciones, %d fallos\n", fails ? "FALLO" : "OK", checks, fails);
    return fails ? 1 : 0;
}
