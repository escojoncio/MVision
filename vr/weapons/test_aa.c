/* Pruebas del lanzamisiles antiaéreo: adquisición, bloqueo, disparo, pérdida de blanco y recarga.
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
    int reserve, fired, takes, steers, dry, tone, locked, mag_in, mag_out, haptics;
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
static int b_targets(WVec3 *o, int max) { int n = G.ntg < max ? G.ntg : max; for (int i = 0; i < n; i++) o[i] = G.tg[i]; return n; }
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
/* Blanco a `range` m en el eje del tubo con desvío angular (rad) horizontal. */
static void put_target(float range, float off) {
    WPose e = wcore_point_world(S, &in, WP_EYE);
    WVec3 d = wv(sinf(off), 0, -cosf(off));
    G.ntg = 1; G.tg[0] = wv_add(e.p, wv_scale(wquat_rotate(e.q, d), range));
}
static WPose magwell(void) { return wcore_point_world(S, &in, WP_MAGWELL); }

int main(void) {
    /* ---------- 1. arranque y enfriado ---------- */
    reset(3);
    step(WR_MANUAL, 0.05f);
    CHECK(S->phase == AA_P_READY && S->mag_rounds == 1 && G.reserve == 2 && G.takes == 1, "carga inicial fase=%d res=%d", S->phase, G.reserve);
    CHECK(aa_lock_state(S) == AA_LK_COOL, "debe enfriar");
    put_target(200, 0.01f);
    step(WR_MANUAL, 0.5f);
    CHECK(aa_lock_state(S) == AA_LK_COOL && G.tone == 0, "no adquiere enfriando");
    step(WR_MANUAL, 0.6f);
    CHECK(aa_lock_state(S) != AA_LK_COOL, "enfriado terminado");
    CHECK(S->aux.active && !S->aux.use_game_camera && S->aux.target_part == 8 && S->aux.overlay == (WO_RETICLE | WO_LOCK_BOX | WO_TELEMETRY),
          "aux mal configurada");

    /* ---------- 2. adquisición -> bloqueo ---------- */
    reset(3); step(WR_MANUAL, 1.2f);
    put_target(200, 0.02f);                      /* 1,1 grados: dentro del cono (4) */
    step(WR_MANUAL, 0.8f);
    CHECK(aa_lock_state(S) == AA_LK_ACQ, "adquiriendo (estado %d)", aa_lock_state(S));
    CHECK(G.tone >= 3, "tonos crecientes (%d)", G.tone);
    float p1 = aa_lock_progress(S);
    CHECK(p1 > 0.4f && p1 < 0.6f, "progreso a 0,8 s = %.2f", p1);
    int tones_half = G.tone;
    step(WR_MANUAL, 0.6f);
    CHECK(aa_lock_state(S) == AA_LK_ACQ, "aún sin bloqueo a 1,4 s");
    CHECK(G.tone - tones_half >= 3, "tono más rápido en la 2a mitad (%d vs %d)", G.tone - tones_half, tones_half);
    step(WR_MANUAL, 0.3f);
    CHECK(aa_lock_state(S) == AA_LK_LOCKED && G.locked == 1, "bloqueado a 1,7 s, WS_LOCKED=%d", G.locked);
    step(WR_MANUAL, 0.5f);
    CHECK(G.locked == 1 && aa_lock_state(S) == AA_LK_LOCKED, "bloqueo estable, un solo evento");

    /* ---------- 3. disparo con bloqueo y guiado ---------- */
    in.trigger[0] = 1.0f;
    G.proj_active = 1; G.proj.p = wv(0, 1.4f, -2); G.proj.q = wquat_identity();
    step(WR_MANUAL, 0.05f);
    CHECK(G.fired == 1 && S->mag_rounds == 0 && S->phase == AA_P_SPENT, "disparo");
    CHECK(fabsf(G.last_muzzle.p.z - wcore_point_world(S, &in, WP_MUZZLE).p.z) < 0.2f, "boca de fuego");
    CHECK(aa_lock_state(S) == AA_LK_OFF, "buscador apagado tras disparo");
    CHECK(G.steers > 0, "guía el misil hacia el blanco bloqueado");
    CHECK(fabsf(G.last_yaw) <= 1.0f, "steer acotado");
    in.trigger[0] = 0; step(WR_MANUAL, 0.1f);
    in.trigger[0] = 1; int dry0 = G.dry; step(WR_MANUAL, 0.05f);
    CHECK(G.dry == dry0 + 1 && G.fired == 1, "gatillo en seco con tubo gastado");
    in.trigger[0] = 0; G.proj_active = 0; step(WR_MANUAL, 0.5f);

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

    /* ---------- 5. disparo sin bloqueo (tras enfriar) ---------- */
    G.ntg = 0; G.steers = 0; step(WR_MANUAL, 1.2f);
    CHECK(aa_lock_state(S) == AA_LK_SEARCH, "buscando");
    in.trigger[0] = 1; step(WR_MANUAL, 0.05f);
    CHECK(G.fired == 2 && G.steers == 0, "sin bloqueo sale recto, sin guiado");
    in.trigger[0] = 0;

    /* ---------- 6. pérdida de blanco y reinicio ---------- */
    reset(3); step(WR_MANUAL, 1.2f);
    put_target(300, 0.0f); step(WR_MANUAL, 1.0f);
    CHECK(aa_lock_state(S) == AA_LK_ACQ, "acq");
    put_target(300, 0.25f);                    /* fuera del cono ampliado */
    step(WR_MANUAL, 0.1f);                      /* menos que la gracia */
    put_target(300, 0.0f); step(WR_MANUAL, 0.1f);
    CHECK(aa_lock_progress(S) > 0.6f, "microcorte tolerado (%.2f)", aa_lock_progress(S));
    put_target(300, 0.25f); step(WR_MANUAL, 0.4f);
    CHECK(aa_lock_state(S) == AA_LK_SEARCH && aa_lock_progress(S) == 0, "pérdida reinicia");
    put_target(300, 0.0f); step(WR_MANUAL, 1.0f);
    CHECK(aa_lock_state(S) == AA_LK_ACQ, "reinicia desde cero (no bloquea a 1,0 s)");
    step(WR_MANUAL, 0.8f);
    CHECK(aa_lock_state(S) == AA_LK_LOCKED, "bloquea tras 1,6 s");
    /* blanco fuera de alcance mínimo/máximo */
    reset(3); step(WR_MANUAL, 1.2f); put_target(600, 0); step(WR_MANUAL, 1);
    CHECK(aa_lock_state(S) == AA_LK_SEARCH, "fuera de alcance máximo");
    put_target(5, 0); step(WR_MANUAL, 1);
    CHECK(aa_lock_state(S) == AA_LK_SEARCH, "dentro de alcance mínimo");
    /* dos blancos: elige el más cercano al eje */
    put_target(200, 0.05f);
    { WPose e = wcore_point_world(S, &in, WP_EYE); G.tg[1] = wv_add(e.p, wv(sinf(0.01f) * 300, 0, -300)); G.ntg = 2; }
    step(WR_MANUAL, 0.3f);
    CHECK(aa_lock_state(S) == AA_LK_ACQ && fabsf(st.tgt.z - G.tg[1].z) < 1.0f, "elige el más centrado");

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

    /* ---------- 9. overlay ---------- */
    reset(3); step(WR_MANUAL, 1.2f); put_target(250, 0.015f); step(WR_MANUAL, 0.9f);
    static uint32_t img[AA_VIEW_W * AA_VIEW_H];
    for (int i = 0; i < AA_VIEW_W * AA_VIEW_H; i++) img[i] = 0xFF303840u;
    aa_draw_overlay(S, img, AA_VIEW_W, AA_VIEW_H);
    int changed = 0; for (int i = 0; i < AA_VIEW_W * AA_VIEW_H; i++) changed += img[i] != 0xFF303840u;
    CHECK(changed > 300, "overlay dibuja (%d px)", changed);
    CHECK(st.tgt_on_screen && fabsf(st.ndc_x) < 1 && fabsf(st.ndc_y) < 0.2f, "blanco proyectado en pantalla ndc=(%.2f,%.2f)", st.ndc_x, st.ndc_y);

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
