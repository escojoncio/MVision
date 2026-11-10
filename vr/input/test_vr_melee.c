/* Pruebas de vr_melee con RAM falsa (y, si existe, una captura real de RAM para comprobar los desplazamientos). */
#include "vr_melee.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CK(c) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint8_t RAM[VRM_RAM_SIZE];
#define W 0x8016CAA8u
#define PAD 0x800B1750u
#define ATK 0x8009FEB8u
#define AP 0x8009FF4Cu
static void p32(uint32_t a, uint32_t v) { memcpy(RAM + (a & 0x1FFFFF), &v, 4); }
static void p16(uint32_t a, int v) { int16_t x = (int16_t)v; memcpy(RAM + (a & 0x1FFFFF), &x, 2); }
static uint32_t g32(uint32_t a) { uint32_t v; memcpy(&v, RAM + (a & 0x1FFFFF), 4); return v; }
static int g16(uint32_t a) { int16_t v; memcpy(&v, RAM + (a & 0x1FFFFF), 2); return v; }

static uint32_t calls[32][3]; static int ncalls;
static void eng_call(void *u, uint32_t fn, uint32_t a0, uint32_t a1) {
    (void)u;
    if (ncalls < 32) { calls[ncalls][0] = fn; calls[ncalls][1] = a0; calls[ncalls][2] = a1; ncalls++; }
    if (fn == VRM_FN_START) { p32(a0 + VRM_W_ANIMFN, a1); p32(a0 + VRM_W_ANIMT, 0); }
}
static VrmEng ENG = { RAM, NULL, eng_call };
static char LOG[8192];
static void lg(const char *s) { strncat(LOG, s, sizeof LOG - strlen(LOG) - 2); strcat(LOG, "\n"); }
static int logged(const char *s) { return strstr(LOG, s) != NULL; }
static int started(uint32_t st) { for (int i = 0; i < ncalls; i++) if (calls[i][0] == VRM_FN_START && calls[i][2] == st) return 1; return 0; }

static void enemy(int i, int x, int y, int z) {
    uint32_t b = VRM_TARGETS + 72u * (uint32_t)i;
    p16(b, 1 | 2 | 4); p16(b + 2, 2);
    p16(b + 8, x); p16(b + 10, y); p16(b + 12, z);
    p16(b + 16, 200); p16(b + 18, 500); p16(b + 20, 200);
    p16(b + 0x26, 100); p16(b + 0x42, 3);
}
static void world(void) {
    memset(RAM, 0, sizeof RAM); ncalls = 0; LOG[0] = 0;
    p32(VRM_PLAYER_CTRL, W + 0x20);
    p32(W + VRM_W_ACTPACK, AP); p32(AP + 16, ATK); RAM[(ATK + 2) & 0x1FFFFF] = 0x10;
    p32(W + VRM_W_PAD, PAD);
    p32(W + VRM_W_ANIMFN, 0x80052D1Cu);
}
static VrmFrame fr(void) { VrmFrame f; memset(&f, 0, sizeof f); f.punch_side = f.knock_side = -1; f.grab_side = -1; f.throw_side = -1; return f; }
static Vrm V;
static void fresh(void) { world(); vrm_init(&V); V.log = lg; }

int main(void) {
    /* sin peticiones: el original (los botones del juego siguen valiendo) */
    fresh(); { VrmFrame f = fr(); vrm_frame(&V, &ENG, &f); CK(vrm_action_input(&V, &ENG, W) == 0); CK(ncalls == 0); }

    /* golpe: arranca el estado de golpe del motor directamente, sin cuadrado */
    fresh(); { VrmFrame f = fr(); f.punch_side = 1; f.fist_ok[1] = f.fist_closed[1] = 1; vrm_frame(&V, &ENG, &f);
        CK(vrm_action_input(&V, &ENG, W) == 1); CK(started(VRM_FN_PUNCH)); CK(!started(VRM_FN_GRAB)); CK(g32(W + VRM_W_ANIMFN) == VRM_FN_PUNCH);
        CK(g32(W + VRM_W_FLAGS) & 4u); CK(logged("golpe disparado")); CK(V.n_punch == 1); }

    /* un golpe sin golpe equipado se rechaza con motivo */
    fresh(); RAM[(ATK + 2) & 0x1FFFFF] = 0x88; { VrmFrame f = fr(); f.punch_side = 0; vrm_frame(&V, &ENG, &f);
        CK(vrm_action_input(&V, &ENG, W) == 0); CK(logged("no tiene golpe")); }

    /* impacto: puño cerca del enemigo => impacto y centro sustituido; puño lejos => fallo con distancia */
    for (int far = 0; far < 2; far++) {
        fresh(); enemy(0, 1000, 0, 0);
        VrmFrame f = fr(); f.punch_side = 0; f.fist_ok[0] = f.fist_closed[0] = 1;
        f.fist[0][0] = far ? 3000.0f : 900.0f; f.fist[0][1] = 0; f.fist[0][2] = 0;
        vrm_frame(&V, &ENG, &f); vrm_action_input(&V, &ENG, W);
        p16(W + VRM_W_TARGET + 16, 150); p16(W + VRM_W_TARGET + 18, 150); p16(W + VRM_W_TARGET + 20, 150);
        vrm_pre_hit(&V, &ENG, W);
        CK(V.ctx_hit);
        uint32_t vec = 0x80100000u; p16(vec, 5); p16(vec + 2, 5); p16(vec + 4, 5);
        CK(vrm_move_target(&V, &ENG, W + VRM_W_TARGET, vec) == 1);
        CK(g16(vec) == (far ? 3000 : 900));
        CK(vrm_move_target(&V, &ENG, W + 8, vec) == 0);        /* otro blanco: no se toca */
        if (!far) p16(VRM_TARGETS + 6, 4);                      /* el motor marca el daño al solaparse */
        vrm_post_hit(&V, &ENG, W, 0);
        if (far) { CK(V.n_miss == 1 && V.n_hit == 0); CK(logged("FALLO")); } else { CK(V.n_hit == 1); CK(logged("REGISTRADO")); }
        CK(!V.ctx_hit);
    }

    /* el resultado del motor (vida quitada) es 0 en un puñetazo: lo que cuenta es que el contador de aturdimiento baje o la bandera de daño se marque */
    fresh(); enemy(0, 1000, 0, 0);
    { uint32_t b = VRM_TARGETS; p16(b + 0x2A, 7);
      VrmFrame f = fr(); f.punch_side = 0; f.fist_ok[0] = f.fist_closed[0] = 1; f.fist[0][0] = 500.0f; f.fist[0][1] = 100.0f; f.fist[0][2] = 0;
      vrm_frame(&V, &ENG, &f); vrm_action_input(&V, &ENG, W);
      p16(W + VRM_W_TARGET + 16, 400); p16(W + VRM_W_TARGET + 18, 200); p16(W + VRM_W_TARGET + 20, 400);
      vrm_pre_hit(&V, &ENG, W);
      p16(b + 0x2A, 6); p16(b + 6, 4);                                    /* lo que hace el motor al solaparse */
      vrm_post_hit(&V, &ENG, W, 0);
      CK(V.n_hit == 1 && V.n_miss == 0); CK(logged("REGISTRADO")); }
    fresh(); enemy(0, 3000, 0, 0);                                         /* el motor no toca nada: fallo, sin reintentos */
    { VrmFrame f = fr(); f.punch_side = 0; f.fist_ok[0] = f.fist_closed[0] = 1; f.fist[0][0] = 0; vrm_frame(&V, &ENG, &f); vrm_action_input(&V, &ENG, W);
      vrm_pre_hit(&V, &ENG, W); vrm_post_hit(&V, &ENG, W, 0); CK(V.n_hit == 0 && V.n_miss == 1); CK(logged("FALLO")); }
    fresh(); enemy(0, 400, 0, 0); p16(VRM_TARGETS, 1 | 2);               /* enemigo en reacción (sin el bit de daño): nadie golpeable */
    { VrmFrame f = fr(); f.punch_side = 0; f.fist_ok[0] = f.fist_closed[0] = 1; f.fist[0][0] = 400; vrm_frame(&V, &ENG, &f); vrm_action_input(&V, &ENG, W);
      vrm_pre_hit(&V, &ENG, W); vrm_post_hit(&V, &ENG, W, 0); CK(V.n_nobody == 1 && logged("ningún enemigo golpeable")); }
    /* NUEVO: el enemigo pegado al puño está en reacción pero hay OTRO golpeable lejos (n > 0): el golpe se encola igualmente */
    fresh(); enemy(0, 400, 0, 0); p16(VRM_TARGETS, 1 | 2); enemy(1, 5000, 0, 0);
    { VrmFrame f = fr(); f.punch_side = 0; f.fist_ok[0] = f.fist_closed[0] = 1; f.fist[0][0] = 400; vrm_frame(&V, &ENG, &f); vrm_action_input(&V, &ENG, W);
      vrm_pre_hit(&V, &ENG, W); vrm_post_hit(&V, &ENG, W, 0); CK(V.n_miss == 1 && V.retry_ttl > 0); }
    /* el enemigo en reacción NO es el más cercano al puño (otro golpeable está más cerca): sin reintento */
    fresh(); enemy(0, 1000, 0, 0); p16(VRM_TARGETS, 1 | 2); enemy(1, 700, 0, 0);
    { VrmFrame f = fr(); f.punch_side = 0; f.fist_ok[0] = f.fist_closed[0] = 1; f.fist[0][0] = 400; vrm_frame(&V, &ENG, &f); vrm_action_input(&V, &ENG, W);
      vrm_pre_hit(&V, &ENG, W); vrm_post_hit(&V, &ENG, W, 0); CK(V.n_miss == 1 && V.retry_ttl == 0); }
    /* historia larga del puño: la muestra más cercana al enemigo (hace ~0,3 s) sigue disponible */
    fresh(); enemy(0, 1000, 0, 0);
    { VrmFrame f = fr(); f.punch_side = 0; f.fist_ok[0] = f.fist_closed[0] = 1; f.fist[0][0] = 900; vrm_frame(&V, &ENG, &f); vrm_action_input(&V, &ENG, W);
      VrmFrame h = fr(); h.fist_ok[0] = h.fist_closed[0] = 1; for (int i = 0; i < 18; i++) { h.fist[0][0] = 900.0f - 40.0f * (float)i; vrm_frame(&V, &ENG, &h); }
      vrm_pre_hit(&V, &ENG, W); CK(V.ctx_hit && V.ctx_origin[0] >= 800.0f); vrm_post_hit(&V, &ENG, W, 0); }
    /* enemigo en reacción: el golpe se encola y se repite en cuanto puede recibirlo; caduca pasado el margen */
    fresh(); enemy(0, 400, 0, 0); p16(VRM_TARGETS, 1 | 2);
    { VrmFrame f = fr(); f.punch_side = 0; f.fist_ok[0] = f.fist_closed[0] = 1; f.fist[0][0] = 400; vrm_frame(&V, &ENG, &f); vrm_action_input(&V, &ENG, W);
      vrm_pre_hit(&V, &ENG, W); vrm_post_hit(&V, &ENG, W, 0); CK(V.retry_ttl > 0);
      V.in_punch = 0; VrmFrame h = fr(); h.fist_ok[0] = h.fist_closed[0] = 1; h.fist[0][0] = 400;
      vrm_frame(&V, &ENG, &h); CK(!V.p_punch);                              /* sigue en reacción */
      p16(VRM_TARGETS, 1 | 2 | 4); vrm_frame(&V, &ENG, &h); CK(V.p_punch && V.n_requeue == 1 && logged("ya puede recibirlo")); }
    fresh(); enemy(0, 400, 0, 0); p16(VRM_TARGETS, 1 | 2);
    { VrmFrame f = fr(); f.punch_side = 0; f.fist_ok[0] = f.fist_closed[0] = 1; f.fist[0][0] = 400; vrm_frame(&V, &ENG, &f); vrm_action_input(&V, &ENG, W);
      vrm_pre_hit(&V, &ENG, W); vrm_post_hit(&V, &ENG, W, 0); V.in_punch = 0; VrmFrame h = fr(); h.fist_ok[0] = h.fist_closed[0] = 1; h.fist[0][0] = 400;
      for (int i = 0; i < VRM_RETRY_FRAMES + 2; i++) vrm_frame(&V, &ENG, &h); CK(!V.p_punch && logged("caducó el margen")); }
    /* cola del combo llena: se registra */
    fresh(); { VrmFrame f = fr(); f.punch_side = 0; vrm_frame(&V, &ENG, &f); vrm_action_input(&V, &ENG, W);
        for (int i = 0; i < 3; i++) { VrmFrame q = fr(); q.punch_side = 1; vrm_frame(&V, &ENG, &q); vrm_punch_tick(&V, &ENG, W, 2); }
        CK(V.combo_q == 2 && V.n_qfull == 1 && logged("cola del combo está llena")); }
    /* patada (etapa 3): no se sustituye el origen */
    fresh(); enemy(0, 400, 0, 0);
    { VrmFrame f = fr(); f.punch_side = 0; f.fist_ok[0] = f.fist_closed[0] = 1; f.fist[0][0] = 400; vrm_frame(&V, &ENG, &f); vrm_action_input(&V, &ENG, W);
      vrm_pre_hit(&V, &ENG, W); vrm_post_hit(&V, &ENG, W, 0); vrm_pre_hit(&V, &ENG, W); vrm_post_hit(&V, &ENG, W, 0);
      vrm_pre_hit(&V, &ENG, W); CK(V.stage == 3 && !V.ctx_hit); vrm_post_hit(&V, &ENG, W, 0); CK(logged("patada")); }

    /* combo golpe-golpe-patada: las peticiones se encolan y el bit 0x20 se pone en los instantes 6 y 13 (aunque lleguen fuera de las ventanas del botón) */
    fresh(); { VrmFrame f = fr(); f.punch_side = 0; vrm_frame(&V, &ENG, &f); vrm_action_input(&V, &ENG, W);
        VrmFrame q = fr(); q.punch_side = 1; vrm_frame(&V, &ENG, &q);      /* segunda petición temprana */
        CK(vrm_punch_tick(&V, &ENG, W, 2) == 0); CK(!(g16(W + VRM_W_LOCAL) & 0x20) && V.combo_q == 1);
        VrmFrame t = fr(); t.punch_side = 0; vrm_frame(&V, &ENG, &t);      /* tercera, también antes del 6: se conserva para el 13 */
        CK(vrm_punch_tick(&V, &ENG, W, 5) == 0); CK(V.combo_q == 2);
        CK(vrm_punch_tick(&V, &ENG, W, 6) == 0); CK(g16(W + VRM_W_LOCAL) & 0x20); CK(V.combo_q == 1);
        p16(W + VRM_W_LOCAL, g16(W + VRM_W_LOCAL) & ~0x20);                 /* el motor consume el bit */
        CK(vrm_punch_tick(&V, &ENG, W, 9) == 0); CK(!(g16(W + VRM_W_LOCAL) & 0x20));
        CK(vrm_punch_tick(&V, &ENG, W, 13) == 0); CK(g16(W + VRM_W_LOCAL) & 0x20); CK(V.combo_q == 0); }
    /* espera en el instante de decisión mientras el brazo se mueve: la animación no avanza y no se ejecuta el original; sin brazo en movimiento no espera */
    fresh(); { VrmFrame f = fr(); f.punch_side = 0; vrm_frame(&V, &ENG, &f); vrm_action_input(&V, &ENG, W);
        VrmFrame s = fr(); s.swinging = 1; vrm_frame(&V, &ENG, &s);
        int held = 0; for (int i = 0; i < 20; i++) { p32(W + VRM_W_ANIMT, 7); if (vrm_punch_tick(&V, &ENG, W, 6)) { held++; CK(g32(W + VRM_W_ANIMT) == 6); } else break; }
        CK(held == VRM_HOLD_MAX);
        /* la petición llega durante la espera: se concede el segundo golpe */
        fresh(); VrmFrame f2 = fr(); f2.punch_side = 0; vrm_frame(&V, &ENG, &f2); vrm_action_input(&V, &ENG, W);
        vrm_frame(&V, &ENG, &s); p32(W + VRM_W_ANIMT, 7); CK(vrm_punch_tick(&V, &ENG, W, 6) == 1);
        VrmFrame r = fr(); r.punch_side = 1; r.swinging = 1; vrm_frame(&V, &ENG, &r); CK(vrm_punch_tick(&V, &ENG, W, 6) == 0); CK(g16(W + VRM_W_LOCAL) & 0x20);
        fresh(); VrmFrame f3 = fr(); f3.punch_side = 0; vrm_frame(&V, &ENG, &f3); vrm_action_input(&V, &ENG, W); CK(vrm_punch_tick(&V, &ENG, W, 6) == 0); CK(V.n_hold == 0); }

    /* combo: una petición nueva durante la animación se encola y marca el bit 0x20 en el instante 6 */
    fresh(); { VrmFrame f = fr(); f.punch_side = 0; vrm_frame(&V, &ENG, &f); vrm_action_input(&V, &ENG, W);
        VrmFrame g = fr(); g.punch_side = 1; vrm_frame(&V, &ENG, &g); vrm_punch_tick(&V, &ENG, W, 3); vrm_punch_tick(&V, &ENG, W, 6);
        CK(g16(W + VRM_W_LOCAL) & 0x20); CK(V.n_combo == 1); }

    /* petición caducada: motivo registrado */
    fresh(); { VrmFrame f = fr(); f.punch_side = 0; vrm_frame(&V, &ENG, &f); VrmFrame n = fr();
        for (int i = 0; i < 10; i++) { vrm_frame(&V, &ENG, &n); }
        CK(logged("no aceptó")); CK(vrm_action_input(&V, &ENG, W) == 0); }

    /* agarre: arranca el estado de presa, la caja de captura sale de la mano VR */
    fresh(); enemy(0, 500, 0, 0); { VrmFrame f = fr(); f.grab_start = 1; f.grab_side = 0; f.fist_ok[0] = 1; f.fist[0][0] = 480; f.fist[0][1] = 300; f.fist[0][2] = 0;
        vrm_frame(&V, &ENG, &f);
        CK(vrm_action_input(&V, &ENG, W) == 1); CK(started(VRM_FN_GRAB)); CK(!started(VRM_FN_PUNCH)); CK(V.grab_owned);
        vrm_pre_grab(&V, &ENG, W, 0); CK(V.ctx_grab);
        uint32_t vec = 0x80100000u; CK(vrm_move_target(&V, &ENG, W + VRM_W_TARGET, vec) == 1); CK(g16(vec) == 480 && g16(vec + 2) == 300);
        p32(W + VRM_W_PTARGET, VRM_TARGETS); p16(W + VRM_W_LOCAL, 0);
        vrm_post_grab(&V, &ENG, W, 0); CK(logged("agarre iniciado")); CK(V.grab_owned); }
    /* agarre sin presa en la caja: rechazado con motivo */
    fresh(); { VrmFrame f = fr(); f.grab_start = 1; f.grab_side = 1; f.fist_ok[1] = 1; vrm_frame(&V, &ENG, &f); vrm_action_input(&V, &ENG, W);
        vrm_pre_grab(&V, &ENG, W, 0); vrm_post_grab(&V, &ENG, W, 0); CK(logged("agarre rechazado por el motor")); CK(!V.grab_owned); CK(V.n_grab_rejected == 1); }

    /* mantener / apretar / soltar */
    fresh(); enemy(0, 500, 0, 0);
    { VrmFrame f = fr(); f.grab_start = 1; f.grab_side = 0; f.fist_ok[0] = 1; vrm_frame(&V, &ENG, &f); vrm_action_input(&V, &ENG, W);
      p32(W + VRM_W_PTARGET, VRM_TARGETS); VrmFrame h = fr(); h.hold = 1; vrm_frame(&V, &ENG, &h);
      CK(vrm_hold_tick(&V, &ENG, W) == 1); CK(g16(W + VRM_W_LOCAL) == 0);
      h.squeeze = 1; vrm_frame(&V, &ENG, &h); ncalls = 0; CK(vrm_hold_tick(&V, &ENG, W) == 1); CK(started(VRM_ST_SQUEEZE)); CK(V.n_squeeze == 1);
      /* 9 fotogramas sin botón: suelta */
      VrmFrame o = fr(); for (int i = 0; i < 9; i++) { vrm_frame(&V, &ENG, &o); vrm_hold_tick(&V, &ENG, W); }
      CK(!V.grab_owned); CK(logged("agarre terminado")); }
    /* lanzamiento: el agarre se arma con el tirón, arrastra con el rumbo y suelta */
    fresh(); enemy(0, 500, 0, 0);
    { VrmFrame f = fr(); f.grab_start = 1; f.grab_side = 0; f.fist_ok[0] = 1; f.throw_side = 0; f.throw_yaw = 3.14159265f; vrm_frame(&V, &ENG, &f);
      vrm_action_input(&V, &ENG, W); vrm_pre_grab(&V, &ENG, W, 0);
      p32(W + VRM_W_PTARGET, VRM_TARGETS); p16(W + VRM_W_LOCAL, 0); vrm_post_grab(&V, &ENG, W, 0);
      CK(logged("lanzamiento:")); CK(V.throw_left == VRM_THROW_FRAMES); CK(V.throw_dir == 2048);
      VrmFrame o = fr(); int n = 0; ncalls = 0;
      while (V.grab_owned && n < 40) { vrm_frame(&V, &ENG, &o); vrm_hold_tick(&V, &ENG, W); if (n == 0) { CK(g32(VRM_G_MOVEDIR) == 2048); CK(started(VRM_ST_DRAG)); } n++; }
      CK(n == VRM_THROW_FRAMES); CK(!V.grab_owned); CK(logged("lanzamiento terminado")); }
    /* tirón sin presa: lanzamiento cancelado */
    fresh(); { VrmFrame f = fr(); f.grab_start = 1; f.grab_side = 1; f.fist_ok[1] = 1; f.throw_side = 1; f.throw_yaw = 0.5f; vrm_frame(&V, &ENG, &f); vrm_action_input(&V, &ENG, W);
      vrm_pre_grab(&V, &ENG, W, 0); vrm_post_grab(&V, &ENG, W, 0); CK(logged("lanzamiento cancelado")); CK(V.throw_left == 0); }
    /* 10 apretones = remate */
    fresh(); enemy(0, 500, 0, 0);
    { VrmFrame f = fr(); f.grab_start = 1; f.grab_side = 0; f.fist_ok[0] = 1; vrm_frame(&V, &ENG, &f); vrm_action_input(&V, &ENG, W);
      p32(W + VRM_W_PTARGET, VRM_TARGETS); int killed = 0;
      for (int i = 0; i < 10; i++) { VrmFrame s = fr(); s.squeeze = 1; s.hold = 1; vrm_frame(&V, &ENG, &s); ncalls = 0; killed = vrm_squeeze_tick(&V, &ENG, W, 3); }
      CK(killed == 1); CK(started(VRM_ST_KILL)); CK(logged("remate")); }
    /* presa lanzada por el botón del juego: los ganchos de mantener no intervienen */
    fresh(); CK(vrm_hold_tick(&V, &ENG, W) == 0); CK(vrm_squeeze_tick(&V, &ENG, W, 3) == 0);

    /* arrastre: mientras el puño siga pulsado el motor ve la presa mantenida; la entrada se restaura */
    fresh(); { V.grab_owned = 1; V.hold = 1; uint16_t sv; p16(PAD, 0x0001); vrm_pre_drag(&V, &ENG, W, &sv);
        CK(g16(PAD) & VRM_PAD_ACT); vrm_post_drag(&V, &ENG, W, sv); CK((g16(PAD) & 0xFFFF) == 0x0001); }

    /* toque en la pared: entra en el estado de nudillos solo con tiempo >= 8 */
    fresh(); { VrmFrame f = fr(); f.knock_side = 0; vrm_frame(&V, &ENG, &f);
        CK(vrm_knock_check(&V, &ENG, W, 3) == 0);
        CK(vrm_knock_check(&V, &ENG, W, 8) == 1); CK(g32(W + VRM_W_FLAGS) & 0x100u); CK(g32(W + VRM_W_KNOCK_FN) == VRM_ST_KNOCK); CK(V.n_knock == 1); CK(logged("toque en la pared disparado"));
        CK(vrm_knock_check(&V, &ENG, W, 9) == 0); }               /* un toque, una llamada */
    fresh(); { VrmFrame f = fr(); f.knock_side = 1; vrm_frame(&V, &ENG, &f); VrmFrame n = fr(); for (int i = 0; i < 10; i++) { vrm_frame(&V, &ENG, &n); }
        CK(logged("toque en la pared rechazado")); }

    /* desactivado: nada */
    fresh(); V.enable = 0; { VrmFrame f = fr(); f.punch_side = 0; vrm_frame(&V, &ENG, &f); CK(vrm_action_input(&V, &ENG, W) == 0); }

    /* captura real: los desplazamientos del actor del jugador (si hay volcados) */
    const char *dd = getenv("VRM_DUMPS");
    if (dd) {
        char p[512]; static uint8_t D[VRM_RAM_SIZE];
        snprintf(p, sizeof p, "%s/body.bin", dd);
        FILE *fp = fopen(p, "rb");
        if (fp && fread(D, 1, VRM_RAM_SIZE, fp) == VRM_RAM_SIZE) {
            uint32_t ctrl; memcpy(&ctrl, D + (VRM_PLAYER_CTRL & 0x1FFFFF), 4);
            uint32_t w = ctrl - 0x20, v;
            CK(w == W);
            memcpy(&v, D + ((w + VRM_W_PAD) & 0x1FFFFF), 4); CK(v == PAD);
            memcpy(&v, D + ((w + VRM_W_ANIMFN) & 0x1FFFFF), 4); CK(v == VRM_FN_PUNCH);      /* captura en pleno golpe */
            memcpy(&v, D + ((w + VRM_W_FLAGS) & 0x1FFFFF), 4); CK(v & 4u);
            uint32_t ap; memcpy(&ap, D + ((w + VRM_W_ACTPACK) & 0x1FFFFF), 4); CK(ap == AP);
            memcpy(&v, D + ((ap + 16) & 0x1FFFFF), 4); CK(v == ATK);
            CK(D[(ATK + 2) & 0x1FFFFF] != 0x88);
            printf("captura real: desplazamientos coherentes (actor %08x, entrada %08x)\n", w, PAD);
        } else printf("captura real: no disponible, omitida\n");
        if (fp) fclose(fp);
    }
    /* toque sin estar pegado a la pared: la decisión de acción lanza el sonido y el ruido del toque del motor, una sola vez, y los botones siguen su curso */
    fresh(); RAM[VRM_KNOCK_SND & 0x1FFFFF] = 55; { VrmFrame f = fr(); f.knock_side = 1; vrm_frame(&V, &ENG, &f);
        CK(vrm_action_input(&V, &ENG, W) == 0); CK(ncalls == 1 && calls[0][0] == VRM_FN_KNOCK_SE && calls[0][1] == 55 && calls[0][2] == 1); CK(V.n_knock == 1); CK(logged("sin estado de nudillos"));
        CK(vrm_action_input(&V, &ENG, W) == 0); CK(ncalls == 1);                    /* un toque, una llamada */
        VrmFrame n = fr(); for (int i = 0; i < 10; i++) vrm_frame(&V, &ENG, &n);
        CK(!logged("toque en la pared rechazado")); }
    /* golpe a un enemigo: sin petición de toque no hay sonido de toque */
    fresh(); enemy(0, 400, 0, 0); { VrmFrame f = fr(); f.punch_side = 0; f.fist_ok[0] = f.fist_closed[0] = 1; f.fist[0][0] = 400; vrm_frame(&V, &ENG, &f); vrm_action_input(&V, &ENG, W);
        for (int i = 0; i < ncalls; i++) CK(calls[i][0] != VRM_FN_KNOCK_SE);
        CK(V.n_knock == 0 && V.n_punch == 1); }
    /* cápsulas por pieza: con contacto real el golpe se centra en el punto de contacto (dentro de la caja del blanco) aunque el puño ya haya pasado de largo; sin contacto,
     * aunque el puño esté dentro de la caja, no se registra; un contacto viejo (más de VRM_HIST VBlanks) no vale */
    for (int cas = 0; cas < 3; cas++) {
        fresh(); enemy(0, 1000, 0, 0);
        VrmFrame f = fr(); f.fist_ok[0] = f.fist_closed[0] = 1; f.fist[0][0] = 950; f.contact_valid = 1;
        if (cas) { f.contact[0] = 1; f.contact_pt[0][0] = 802; f.contact_pt[0][1] = 300; f.contact_pt[0][2] = -50; }
        vrm_frame(&V, &ENG, &f);
        VrmFrame n = fr(); n.contact_valid = 1; n.fist_ok[0] = n.fist_closed[0] = 1; n.fist[0][0] = 950;
        for (int i = 0; i < (cas == 2 ? VRM_HIST + 2 : 3); i++) vrm_frame(&V, &ENG, &n);
        n.punch_side = 0; vrm_frame(&V, &ENG, &n); n.punch_side = -1; vrm_action_input(&V, &ENG, W);
        n.fist[0][0] = 2500; for (int i = 0; i < 4; i++) vrm_frame(&V, &ENG, &n);
        vrm_pre_hit(&V, &ENG, W); CK(V.ctx_hit);
        uint32_t vec = 0x80100000u; CK(vrm_move_target(&V, &ENG, W + VRM_W_TARGET, vec) == 1);
        if (cas == 1) { CK(g16(vec) == 802 && g16(vec + 2) == 300 && g16(vec + 4) == -50); CK(!logged("no tocó a ningún enemigo")); }
        else { CK(g16(vec + 2) == 30000); CK(logged("no tocó a ningún enemigo")); }
        vrm_post_hit(&V, &ENG, W, 0);
    }

    /* toque: nunca agachado, tumbado/gateando ni en la caja (banderas de estado del jugador 0x20, 0x40, 0x1000) */
    for (int k = 0; k < 3; k++) {
        fresh(); RAM[VRM_KNOCK_SND & 0x1FFFFF] = 55; p32(VRM_G_PSTATUS, k == 0 ? 0x20u : k == 1 ? 0x40u : 0x1000u);
        VrmFrame f = fr(); f.knock_side = 0; vrm_frame(&V, &ENG, &f);
        CK(vrm_action_input(&V, &ENG, W) == 0); CK(ncalls == 0 && V.n_knock == 0); CK(logged("toque en la pared rechazado: postura"));
    }
    /* combo de 3 golpes con cápsulas: el primero con contacto real; el segundo llega cuando el contacto ya caducó (más de VRM_HIST VBlanks) pero el puño sigue junto al enemigo:
     * se registra en el mismo punto; el tercero es la patada del motor. Con el puño lejos en el segundo, ese no se registra */
    for (int far = 0; far < 2; far++) {
        fresh(); enemy(0, 1000, 0, 0);
        VrmFrame f = fr(); f.fist_ok[0] = f.fist_closed[0] = 1; f.fist[0][0] = 950; f.contact_valid = 1; f.contact[0] = 1; f.contact_pt[0][0] = 802; f.contact_pt[0][1] = 300; f.contact_pt[0][2] = -50;
        f.punch_side = 0; vrm_frame(&V, &ENG, &f); vrm_action_input(&V, &ENG, W);
        uint32_t vec = 0x80100000u;
        vrm_pre_hit(&V, &ENG, W); CK(V.stage == 1 && vrm_move_target(&V, &ENG, W + VRM_W_TARGET, vec) == 1 && g16(vec) == 802); p16(VRM_TARGETS + 6, 4); vrm_post_hit(&V, &ENG, W, 0); p16(VRM_TARGETS + 6, 0);
        VrmFrame n = fr(); n.contact_valid = 1; n.fist_ok[0] = n.fist_closed[0] = 1; n.fist[0][0] = far ? 2500.0f : 700.0f;      /* 700: a 100 mm de la caja (800..1200), sin tocar ninguna pieza */
        for (int i = 0; i < VRM_HIST + 6; i++) vrm_frame(&V, &ENG, &n);
        vrm_pre_hit(&V, &ENG, W); CK(V.stage == 2 && V.ctx_hit && vrm_move_target(&V, &ENG, W + VRM_W_TARGET, vec) == 1);
        if (far) { CK(g16(vec + 2) == 30000); } else { CK(g16(vec) == 802 && g16(vec + 2) == 300 && g16(vec + 4) == -50); }
        vrm_post_hit(&V, &ENG, W, 0);
        vrm_pre_hit(&V, &ENG, W); CK(V.stage == 3 && !V.ctx_hit); vrm_post_hit(&V, &ENG, W, 0);      /* patada: la caja del propio motor */
    }
    /* un golpe nuevo no hereda el contacto del combo anterior */
    { fresh(); enemy(0, 1000, 0, 0);
      VrmFrame f = fr(); f.fist_ok[0] = f.fist_closed[0] = 1; f.fist[0][0] = 950; f.contact_valid = 1; f.contact[0] = 1; f.contact_pt[0][0] = 802; f.punch_side = 0;
      vrm_frame(&V, &ENG, &f); vrm_action_input(&V, &ENG, W); vrm_pre_hit(&V, &ENG, W); vrm_post_hit(&V, &ENG, W, 0);
      VrmFrame n = fr(); n.contact_valid = 1; n.fist_ok[0] = n.fist_closed[0] = 1; n.fist[0][0] = 700;
      for (int i = 0; i < VRM_HIST + 6; i++) vrm_frame(&V, &ENG, &n);
      CK(vrm_action_input(&V, &ENG, W) == 0);                                                    /* el estado de golpe terminó (sin petición) */
      n.punch_side = 0; vrm_frame(&V, &ENG, &n); n.punch_side = -1; vrm_action_input(&V, &ENG, W);
      V.combo_ct = 1;                                                                            /* resto de un combo anterior: la etapa 1 sin contacto lo borra */
      uint32_t vec = 0x80100000u; vrm_pre_hit(&V, &ENG, W); CK(V.stage == 1 && vrm_move_target(&V, &ENG, W + VRM_W_TARGET, vec) == 1 && g16(vec + 2) == 30000 && V.combo_ct == 0); vrm_post_hit(&V, &ENG, W, 0); }

    printf("test_vr_melee: %d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
