#include "vr_input.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, e) CHECK(fabs((double)(a) - (double)(b)) <= (e))

static void qyaw(float q[4], float yaw) { /* giro a la derecha (rumbo +) = rotación -yaw alrededor de Y */
    q[0] = 0; q[1] = sinf(-yaw / 2); q[2] = 0; q[3] = cosf(-yaw / 2);
}

static vr_input_frame_t frame(void) {
    vr_input_frame_t f; memset(&f, 0, sizeof f);
    f.dt = 1.0f / 90; f.head_valid = true; f.head_pos[1] = 1.65f; f.head_quat[3] = 1;
    for (int i = 0; i < 2; i++) f.hand[i].quat[3] = 1;
    f.game_posture = -1;
    return f;
}

static void run(vr_input_t *s, vr_input_frame_t *f, int n, vr_pad_t *o) { for (int i = 0; i < n; i++) vr_input_update(s, f, o); }


/* Postura física opcional: nunca fuerza la postura del botón del juego. */
static void t_posture_modes(void) {
    vr_pad_t o; int taps;
    /* OFF (defecto): de pie físico y el juego tumbado (por el botón): no se pulsa nada ni se levanta */
    vr_input_t s; vr_input_init(&s); vr_input_frame_t f = frame();
    run(&s, &f, 3, &o);
    f.game_posture = 2; taps = 0;
    for (int i = 0; i < 300; i++) { vr_input_update(&s, &f, &o); if (o.buttons & VRI_PAD_CROSS) taps++; }
    CHECK(taps == 0); CHECK(vr_input_posture(&s) == VRI_PRONE);
    f.head_pos[1] = 0.7f; taps = 0;                      /* agacharse físicamente tampoco hace nada */
    f.game_posture = 0;
    for (int i = 0; i < 300; i++) { vr_input_update(&s, &f, &o); if (o.buttons & VRI_PAD_CROSS) taps++; }
    CHECK(taps == 0); CHECK(vr_input_posture(&s) == VRI_STAND);
    /* el botón de cruz del usuario llega intacto */
    f.pad_buttons = 0; f.hand[1].buttons = VRI_H_LOW; vr_input_update(&s, &f, &o); CHECK(o.buttons & VRI_PAD_CROSS);
    /* DOWN: bajar físicamente baja el juego una vez; levantarse no lo sube; con el juego ya tumbado no baja */
    vr_input_t d; vr_input_init(&d); d.cfg.posture_mode = VRI_POSTURE_DOWN; f = frame(); f.game_posture = 0;
    run(&d, &f, 3, &o);
    f.head_pos[1] = 1.65f - 0.45f; taps = 0; int game = 0; bool prev = false;
    for (int i = 0; i < 300; i++) {
        vr_input_update(&d, &f, &o);
        bool c = o.buttons & VRI_PAD_CROSS; if (c && !prev) { taps++; game = game == 0 ? 1 : game; } prev = c; f.game_posture = game;
    }
    CHECK(taps == 1 && game == 1);
    f.head_pos[1] = 1.65f; taps = 0;                     /* se levanta: el juego sigue agachado, sin toques */
    for (int i = 0; i < 300; i++) { vr_input_update(&d, &f, &o); if (o.buttons & VRI_PAD_CROSS) taps++; }
    CHECK(taps == 0);
    f.game_posture = 2; f.head_pos[1] = 1.65f - 0.45f; taps = 0;   /* juego tumbado por botón, el usuario se agacha: no se toca */
    for (int i = 0; i < 300; i++) { vr_input_update(&d, &f, &o); if (o.buttons & VRI_PAD_CROSS) taps++; }
    CHECK(taps == 0);
    /* DOWN: me agacho, el juego recibe mi toque pero el usuario además pulsa la cruz a mano hasta tumbarse: ningún reintento más */
    vr_input_init(&d); d.cfg.posture_mode = VRI_POSTURE_DOWN; f = frame(); f.game_posture = 0; run(&d, &f, 3, &o);
    f.head_pos[1] = 1.65f - 0.45f; taps = 0;
    for (int i = 0; i < 3; i++) { vr_input_update(&d, &f, &o); if (o.buttons & VRI_PAD_CROSS) taps++; }
    CHECK(taps >= 1);                                    /* el primer toque sí sale */
    f.game_posture = 2; taps = 0; bool pv = (o.buttons & VRI_PAD_CROSS) != 0;
    for (int i = 0; i < 600; i++) { vr_input_update(&d, &f, &o); bool c = (o.buttons & VRI_PAD_CROSS) != 0; if (c && !pv) taps++; pv = c; }
    CHECK(taps == 0);
}

static void t_axis(void) {
    CHECK(vr_input_axis_u8(0) == 0x80);
    CHECK(vr_input_axis_u8(1) == 255);
    CHECK(vr_input_axis_u8(-1) == 0);
    CHECK(vr_input_axis_u8(5) == 255);
    CHECK(vr_input_axis_u8(NAN) == 0x80);
    CHECK(vr_input_axis_u8(-INFINITY) == 0x80);
}

static void t_heading(void) {
    float q[4]; qyaw(q, 0); NEAR(vr_input_quat_heading(q), 0, 1e-5f);
    qyaw(q, 1.0f); NEAR(vr_input_quat_heading(q), 1.0f, 1e-4f);
    qyaw(q, -2.0f); NEAR(vr_input_quat_heading(q), -2.0f, 1e-4f);
    float z[4] = {0, 0, 0, 0}; NEAR(vr_input_quat_heading(z), 0, 1e-6f);
    float n[4] = {NAN, 0, 0, 1}; (void)vr_input_quat_heading(n);
    /* mirando al techo girado 90 grados: el rumbo sigue el giro */
    float up[4] = {sinf(M_PI / 4 * 0.98f), 0, 0, cosf(M_PI / 4 * 0.98f)}; /* pitch +88 grados */
    NEAR(vr_input_quat_heading(up), 0, 0.1f);
}

static void t_sense_buttons(void) {
    vr_input_t s; vr_input_init(&s); vr_input_frame_t f = frame(); vr_pad_t o;
    vr_input_update(&s, &f, &o);
    CHECK(o.buttons == 0 && o.lx == 0x80 && o.ly == 0x80 && o.rx == 0x80 && o.ry == 0x80);
    /* gatillo derecho = cuadrado (ataque del juego), con histéresis */
    f.hand[1].trigger = 1; vr_input_update(&s, &f, &o); CHECK(o.buttons == VRI_PAD_SQUARE);
    f.hand[1].trigger = 0.45f; vr_input_update(&s, &f, &o); CHECK(o.buttons == VRI_PAD_SQUARE);
    f.hand[1].trigger = 0.2f; vr_input_update(&s, &f, &o); CHECK(o.buttons == 0);
    /* caras: derecha A = cruz, B = círculo; izquierda A = cuadrado, B = triángulo */
    f.hand[1].buttons = VRI_H_LOW; vr_input_update(&s, &f, &o); CHECK(o.buttons == VRI_PAD_CROSS);
    f.hand[1].buttons = VRI_H_UP; vr_input_update(&s, &f, &o); CHECK(o.buttons == VRI_PAD_CIRCLE);
    f.hand[1].buttons = 0; f.hand[0].buttons = VRI_H_LOW; vr_input_update(&s, &f, &o); CHECK(o.buttons == VRI_PAD_SQUARE);
    f.hand[0].buttons = VRI_H_UP; vr_input_update(&s, &f, &o); CHECK(o.buttons == VRI_PAD_TRIANGLE);
    /* clics y menús */
    f.hand[0].buttons = VRI_H_STICK; f.hand[1].buttons = VRI_H_STICK; vr_input_update(&s, &f, &o);
    CHECK(o.buttons == (VRI_PAD_L3 | VRI_PAD_R3));
    f.hand[0].buttons = VRI_H_MENU; f.hand[1].buttons = VRI_H_MENU; vr_input_update(&s, &f, &o);
    CHECK(o.buttons == (VRI_PAD_SELECT | VRI_PAD_START));
    f.hand[0].buttons = f.hand[1].buttons = 0; vr_input_update(&s, &f, &o);
    /* gatillo izquierdo = L2 */
    f.hand[0].trigger = 1; vr_input_update(&s, &f, &o); CHECK(o.buttons == VRI_PAD_L2);
    f.hand[0].trigger = 0; vr_input_update(&s, &f, &o); CHECK(o.buttons == 0);
    /* agarre derecho = R1 inmediato */
    f.hand[1].grip = 1; vr_input_update(&s, &f, &o); CHECK(o.buttons == VRI_PAD_R1);
    f.hand[1].grip = 0; vr_input_update(&s, &f, &o);
    /* agarre izquierdo = L1 tras el retardo (0,12 s); un toque corto emite un pulso al soltar */
    f.hand[0].grip = 1; vr_input_update(&s, &f, &o); CHECK(o.buttons == 0);
    run(&s, &f, 12, &o); CHECK(o.buttons == VRI_PAD_L1);
    f.hand[0].grip = 0; vr_input_update(&s, &f, &o);
    vr_input_t t; vr_input_init(&t);
    f.hand[0].grip = 1; run(&t, &f, 4, &o); f.hand[0].grip = 0;
    int l1 = 0; for (int i = 0; i < 40; i++) { vr_input_update(&t, &f, &o); if (o.buttons & VRI_PAD_L1) l1++; }
    CHECK(l1 >= 6 && l1 <= 11);                          /* ~0,10 s a 90 Hz, sin otros botones */
    CHECK((o.buttons & ~VRI_PAD_L1) == 0);
    /* acorde: agarre + gatillo izquierdos juntos = R2, sin L1 ni L2 ni toque posterior */
    vr_input_t u; vr_input_init(&u); f = frame();
    f.hand[0].grip = 1; f.hand[0].trigger = 1; l1 = 0; int r2 = 0, l2 = 0;
    for (int i = 0; i < 30; i++) { vr_input_update(&u, &f, &o); if (o.buttons & VRI_PAD_L1) l1++; if (o.buttons & VRI_PAD_R2) r2++; if (o.buttons & VRI_PAD_L2) l2++; }
    CHECK(l1 == 0 && l2 == 0 && r2 == 30);
    f.hand[0].trigger = 0; vr_input_update(&u, &f, &o); CHECK(o.buttons == 0);   /* soltar el gatillo con el agarre: sin L1 ni L2 */
    f.hand[0].grip = 0; l1 = 0; for (int i = 0; i < 30; i++) { vr_input_update(&u, &f, &o); if (o.buttons & VRI_PAD_L1) l1++; }
    CHECK(l1 == 0);
    /* con el agarre ya emitido (L1 sostenido), el gatillo da L2 (L1+L2), no R2 */
    vr_input_t w; vr_input_init(&w); f = frame(); f.hand[0].grip = 1; run(&w, &f, 20, &o);
    f.hand[0].trigger = 1; vr_input_update(&w, &f, &o); CHECK(o.buttons == (VRI_PAD_L1 | VRI_PAD_L2));
    /* sin acorde configurado: inmediato y sin R2 */
    vr_input_t x; vr_input_init(&x); vri_cfg_t cx = x.cfg; cx.map.chord_off = 0; vr_input_set_cfg(&x, &cx); f = frame();
    f.hand[0].grip = 1; f.hand[0].trigger = 1; vr_input_update(&x, &f, &o); CHECK(o.buttons == (VRI_PAD_L1 | VRI_PAD_L2));
    /* zurdo: se espejan los roles (gatillo izquierdo = cuadrado, derecho = L2, agarre derecho = L1 con retardo) */
    vri_cfg_t c = s.cfg; c.dominant_hand = 0; vr_input_set_cfg(&s, &c);
    vr_input_t z; vr_input_init(&z); vr_input_set_cfg(&z, &c); f = frame();
    f.hand[0].trigger = 1; vr_input_update(&z, &f, &o); CHECK(o.buttons == VRI_PAD_SQUARE);
    f.hand[0].trigger = 0; f.hand[1].trigger = 1; vr_input_update(&z, &f, &o); CHECK(o.buttons == VRI_PAD_L2);
    f.hand[1].trigger = 0; f.hand[0].buttons = VRI_H_LOW; f.hand[1].buttons = VRI_H_LOW; vr_input_update(&z, &f, &o);
    CHECK(o.buttons == (VRI_PAD_CROSS | VRI_PAD_SQUARE));
}

/* Menús (jugador fuera de control): gatillo y A confirman (círculo + cruz: confirma en los menús y avanza texto/salta escenas),
 * B cancela (cruz); los sticks hacen de cruceta; los menús Start/Select y los agarres siguen. */
static void t_menu_map(void) {
    const uint16_t CONF = VRI_PAD_CIRCLE | VRI_PAD_CROSS;
    vr_input_t s; vr_input_init(&s); vr_input_frame_t f = frame(); vr_pad_t o; f.menu = true;
    f.hand[1].trigger = 1; vr_input_update(&s, &f, &o); CHECK(o.buttons == CONF);
    f.hand[1].trigger = 0; f.hand[1].buttons = VRI_H_LOW; vr_input_update(&s, &f, &o); CHECK(o.buttons == CONF);
    f.hand[1].buttons = VRI_H_UP; vr_input_update(&s, &f, &o); CHECK(o.buttons == VRI_PAD_CROSS);
    f.hand[1].buttons = VRI_H_MENU; vr_input_update(&s, &f, &o); CHECK(o.buttons == VRI_PAD_START);
    f.hand[1].buttons = 0; f.hand[0].buttons = VRI_H_MENU; vr_input_update(&s, &f, &o); CHECK(o.buttons == VRI_PAD_SELECT);
    f.hand[0].buttons = VRI_H_LOW; vr_input_update(&s, &f, &o); CHECK(o.buttons == CONF);
    f.hand[0].buttons = VRI_H_UP; vr_input_update(&s, &f, &o); CHECK(o.buttons == VRI_PAD_CROSS);
    f.hand[0].buttons = 0; f.hand[0].trigger = 1; vr_input_update(&s, &f, &o); CHECK(o.buttons == CONF);   /* L2 no existe en menús */
    f.hand[0].trigger = 0;
    /* el stick derecho (el de girar) también navega: sin giro ni locomoción */
    f.hand[1].stick_y = -1; vr_input_update(&s, &f, &o); CHECK(o.buttons == VRI_PAD_DOWN && o.lx == 0x80 && o.ly == 0x80);
    NEAR(vr_input_body_yaw(&s), 0, 1e-9f);
    f.hand[1].stick_y = 0; vr_input_update(&s, &f, &o); CHECK(o.buttons == 0);
    f.hand[1].stick_x = 1; f.hand[0].stick_y = 0.6f; vr_input_update(&s, &f, &o); CHECK(o.buttons == VRI_PAD_RIGHT);  /* gana el mayor desvío */
    f.hand[1].stick_x = 0; f.hand[0].stick_y = 0;
    /* agarre derecho pasa (R1); el agarre izquierdo en menús no emite toques tardíos */
    f.hand[1].grip = 1; vr_input_update(&s, &f, &o); CHECK(o.buttons == VRI_PAD_R1); f.hand[1].grip = 0;
    f.hand[0].grip = 1; run(&s, &f, 3, &o); f.hand[0].grip = 0;
    int n = 0; for (int i = 0; i < 40; i++) { vr_input_update(&s, &f, &o); n += o.buttons != 0; }
    CHECK(n == 0);
    /* al volver al juego el gatillo derecho es el ataque, no confirmar */
    f.menu = false; f.hand[1].trigger = 1; run(&s, &f, 15, &o); CHECK(o.buttons == VRI_PAD_SQUARE);   /* tras el antirrebote (0,12 s) */
    /* tabla sin estado (ventana): mismo mapa de menús */
    vri_hand_t h[2]; memset(h, 0, sizeof h);
    h[1].trigger = 1; CHECK(vr_input_sense_plain(&s.cfg, h) == CONF);
    h[1].trigger = 0; h[1].buttons = VRI_H_UP; CHECK(vr_input_sense_plain(&s.cfg, h) == VRI_PAD_CROSS);
    h[1].buttons = VRI_H_MENU; h[0].buttons = VRI_H_MENU; h[0].grip = 1; h[1].grip = 1;
    CHECK(vr_input_sense_plain(&s.cfg, h) == (VRI_PAD_START | VRI_PAD_SELECT | VRI_PAD_L1 | VRI_PAD_R1));
    memset(h, 0, sizeof h); h[0].stick_x = -1; CHECK(vr_input_sense_plain(&s.cfg, h) == VRI_PAD_LEFT);
    CHECK(vr_input_sense_plain(NULL, h) == 0 && vr_input_sense_plain(&s.cfg, NULL) == 0);
    h[0].trigger = NAN; h[0].stick_x = NAN; h[1].grip = INFINITY; (void)vr_input_sense_plain(&s.cfg, h);
}

static void t_pad_mode(void) {
    vr_input_t s; vr_input_init(&s); vri_cfg_t c = s.cfg; c.mode = VRI_MODE_PAD; c.pass_right_stick = true; vr_input_set_cfg(&s, &c);
    vr_input_frame_t f = frame(); vr_pad_t o;
    f.pad_buttons = VRI_PAD_CROSS | VRI_PAD_UP | VRI_PAD_START; f.pad_rx = 1; f.pad_ry = -1;
    vr_input_update(&s, &f, &o);
    CHECK(o.buttons == f.pad_buttons);
    CHECK(o.rx == 255 && o.ry == 255);            /* ry abajo -> 255 */
    NEAR(vr_input_body_yaw(&s), 0, 1e-6f);
    c.pass_right_stick = false; vr_input_set_cfg(&s, &c);
    vr_input_update(&s, &f, &o);
    CHECK(o.rx == 0x80 && o.ry == 0x80);
    CHECK(vr_input_snap_count(&s) == 1);
}

static void t_loco(void) {
    vr_input_t s; vr_input_init(&s); vr_input_frame_t f = frame(); vr_pad_t o;
    f.hand[0].stick_y = 1;                         /* adelante */
    vr_input_update(&s, &f, &o);
    CHECK(o.lx == 0x80 && o.ly == 0);              /* arriba del pad */
    /* cabeza girada 90 grados a la derecha: adelante = derecha en el espacio del juego */
    qyaw(f.head_quat, M_PI / 2); vr_input_update(&s, &f, &o);
    CHECK(o.lx == 255 && abs((int)o.ly - 128) <= 1);
    /* y si el juego dice que "arriba" ya es ese rumbo, vuelve a ser arriba */
    vr_input_set_game_up(&s, M_PI / 2); vr_input_update(&s, &f, &o);
    CHECK(abs((int)o.lx - 128) <= 1 && o.ly == 0);
    vr_input_set_game_up(&s, 0);
    /* stick a la derecha con cabeza al frente -> derecha */
    qyaw(f.head_quat, 0); f.hand[0].stick_x = 1; f.hand[0].stick_y = 0; vr_input_update(&s, &f, &o);
    CHECK(o.lx == 255);
    /* zona muerta */
    f.hand[0].stick_x = 0.1f; vr_input_update(&s, &f, &o); CHECK(o.lx == 0x80 && o.ly == 0x80);
    /* mano como referencia */
    vri_cfg_t c = s.cfg; c.loco_ref = VRI_LOCO_HAND; vr_input_set_cfg(&s, &c);
    f.hand[0].stick_x = 0; f.hand[0].stick_y = 1; f.hand[0].tracked = true; qyaw(f.hand[0].quat, M_PI / 2);
    vr_input_update(&s, &f, &o); CHECK(o.lx == 255);
    f.hand[0].tracked = false; vr_input_update(&s, &f, &o); CHECK(o.ly == 0);   /* sin mano -> cabeza */
    /* walk_scale */
    c.walk_scale = 0.5f; vr_input_set_cfg(&s, &c); vr_input_update(&s, &f, &o);
    CHECK(o.ly > 60 && o.ly < 70);
    /* la rotación del cuerpo suma al rumbo */
    c.walk_scale = 1; vr_input_set_cfg(&s, &c);
    vr_input_reset_body_yaw(&s, M_PI / 2); vr_input_update(&s, &f, &o); CHECK(o.lx == 255);
}

static void t_snap(void) {
    vr_input_t s; vr_input_init(&s); vr_input_frame_t f = frame(); vr_pad_t o;
    f.hand[1].stick_x = 1;
    run(&s, &f, 30, &o);                           /* mantenido: un solo paso */
    NEAR(vr_input_body_yaw(&s), 30 * M_PI / 180, 1e-4f);
    CHECK(vr_input_snap_count(&s) == 1);
    f.hand[1].stick_x = 0; run(&s, &f, 3, &o);
    f.hand[1].stick_x = -1; run(&s, &f, 3, &o);
    NEAR(vr_input_body_yaw(&s), 0, 1e-4f);
    vri_cfg_t c = s.cfg; c.snap_deg = 45; vr_input_set_cfg(&s, &c);
    f.hand[1].stick_x = 0; run(&s, &f, 3, &o); f.hand[1].stick_x = 1; run(&s, &f, 3, &o);
    NEAR(vr_input_body_yaw(&s), 45 * M_PI / 180, 1e-4f);
    /* envoltura */
    for (int i = 0; i < 20; i++) { f.hand[1].stick_x = 0; run(&s, &f, 2, &o); f.hand[1].stick_x = 1; run(&s, &f, 2, &o); }
    CHECK(vr_input_body_yaw(&s) > -M_PI - 1e-4f && vr_input_body_yaw(&s) <= M_PI + 1e-4f);
    /* el stick derecho no se envía al juego en Sense */
    CHECK(o.rx == 0x80 && o.ry == 0x80);
    /* giro desactivado */
    c.turn_mode = VRI_TURN_OFF; vr_input_set_cfg(&s, &c); float y0 = vr_input_body_yaw(&s);
    f.hand[1].stick_x = 0; run(&s, &f, 3, &o); f.hand[1].stick_x = 1; run(&s, &f, 5, &o);
    NEAR(vr_input_body_yaw(&s), y0, 1e-6f);
}

static void t_smooth(void) {
    vr_input_t s; vr_input_init(&s); vri_cfg_t c = s.cfg; c.turn_mode = VRI_TURN_SMOOTH; c.smooth_dps = 90; vr_input_set_cfg(&s, &c);
    vr_input_frame_t f = frame(); vr_pad_t o;
    f.dt = 0.01f; f.hand[1].stick_x = 1;
    run(&s, &f, 100, &o);                          /* 1 s a tope */
    NEAR(vr_input_body_yaw(&s), M_PI / 2, 1e-3f);
    CHECK(vr_input_snap_count(&s) == 0);
    CHECK(vr_input_vignette(&s) > 0.2f);
    f.hand[1].stick_x = 0.5f; /* tras zona muerta 0.15: (0.5-0.15)/0.85 */
    float y0 = vr_input_body_yaw(&s); run(&s, &f, 100, &o);
    NEAR(vr_input_body_yaw(&s) - y0, (0.35f / 0.85f) * (M_PI / 2), 5e-3f);
}

static void t_vignette(void) {
    vr_input_t s; vr_input_init(&s); vr_input_frame_t f = frame(); vr_pad_t o;
    run(&s, &f, 50, &o); NEAR(vr_input_vignette(&s), 0, 1e-6f);
    f.hand[1].stick_x = 1; vr_input_update(&s, &f, &o);   /* paso: pulso */
    float a = vr_input_vignette(&s); CHECK(a > 0.0f);
    f.hand[1].stick_x = 0; run(&s, &f, 180, &o);          /* 2 s */
    CHECK(vr_input_vignette(&s) < 0.01f);
    f.hand[0].stick_y = 1; run(&s, &f, 90, &o);           /* caminar: leve */
    float w = vr_input_vignette(&s); CHECK(w > 0.05f && w < 0.6f * 0.5f);
    vri_cfg_t c = s.cfg; c.vignette = 0; vr_input_set_cfg(&s, &c);
    run(&s, &f, 200, &o); CHECK(vr_input_vignette(&s) < 0.01f);
    c.vignette = 5; vr_input_set_cfg(&s, &c); CHECK(s.cfg.vignette == 1.0f);
}

/* Juego simulado: cada toque de la cruz cambia la postura 0->1->2 y 2->0 (si es el ciclo supuesto) */
static void t_crouch(void) {
    vr_input_t s; vr_input_init(&s); s.cfg.posture_mode = VRI_POSTURE_FULL; s.cfg.allow_prone = true; vr_input_frame_t f = frame(); vr_pad_t o;
    run(&s, &f, 3, &o); NEAR(vr_input_height(&s), 1.65f, 1e-5f);
    CHECK(vr_input_posture(&s) == VRI_STAND);
    int taps = 0; bool prev = false; int game = 0; f.game_posture = 0;
    /* bajar 0.30: aún de pie */
    f.head_pos[1] = 1.35f; run(&s, &f, 30, &o); CHECK(vr_input_posture(&s) == VRI_STAND && !(o.buttons & VRI_PAD_CROSS));
    /* bajar 0.40: agacharse */
    f.head_pos[1] = 1.25f;
    for (int i = 0; i < 180; i++) {
        vr_input_update(&s, &f, &o);
        bool p = o.buttons & VRI_PAD_CROSS; if (p && !prev) { taps++; game = game == 0 ? 1 : (game == 1 ? 2 : 0); } prev = p;
        f.game_posture = game;
    }
    CHECK(vr_input_posture(&s) == VRI_CROUCH); CHECK(taps == 1); CHECK(game == 1);
    /* histéresis: subir a 0.31 de caída (por encima de 0.35-0.06) -> sigue agachado */
    f.head_pos[1] = 1.65f - 0.31f; run(&s, &f, 30, &o); CHECK(vr_input_posture(&s) == VRI_CROUCH);
    /* tumbarse */
    f.head_pos[1] = 0.7f; taps = 0;
    for (int i = 0; i < 180; i++) {
        vr_input_update(&s, &f, &o);
        bool p = o.buttons & VRI_PAD_CROSS; if (p && !prev) { taps++; game = game == 0 ? 1 : (game == 1 ? 2 : 0); } prev = p;
        f.game_posture = game;
    }
    CHECK(vr_input_posture(&s) == VRI_PRONE); CHECK(taps == 1 && game == 2);
    /* levantarse: la simulación vuelve de tumbado a de pie con un toque */
    f.head_pos[1] = 1.65f; taps = 0;
    for (int i = 0; i < 180; i++) {
        vr_input_update(&s, &f, &o);
        bool p = o.buttons & VRI_PAD_CROSS; if (p && !prev) { taps++; game = game == 2 ? 0 : game + 1; } prev = p;
        f.game_posture = game;
    }
    CHECK(vr_input_posture(&s) == VRI_STAND); CHECK(game == 0 && taps == 1);
    /* sin retroalimentación (-1): un toque por nivel, sin bucle infinito */
    f.game_posture = -1; f.head_pos[1] = 1.2f; taps = 0; prev = false;
    for (int i = 0; i < 300; i++) { vr_input_update(&s, &f, &o); bool p = o.buttons & VRI_PAD_CROSS; if (p && !prev) taps++; prev = p; }
    CHECK(taps == 1);
    /* si el juego nunca responde, se rinde tras reintentos */
    f.game_posture = 0; f.head_pos[1] = 1.65f; run(&s, &f, 60, &o); f.head_pos[1] = 1.2f; taps = 0; prev = false;
    for (int i = 0; i < 1500; i++) { vr_input_update(&s, &f, &o); bool p = o.buttons & VRI_PAD_CROSS; if (p && !prev) taps++; prev = p; }
    CHECK(taps >= 1 && taps <= 6);
    /* menú: sin toques */
    f.head_pos[1] = 1.65f; run(&s, &f, 60, &o); f.menu = true; f.head_pos[1] = 1.2f; taps = 0; run(&s, &f, 15, &o);   /* antirrebote del menú */
    for (int i = 0; i < 300; i++) { vr_input_update(&s, &f, &o); if (o.buttons & VRI_PAD_CROSS) taps++; }
    CHECK(taps == 0);
    /* prone desactivado */
    f.menu = false; vri_cfg_t c = s.cfg; c.allow_prone = false; vr_input_set_cfg(&s, &c);
    f.head_pos[1] = 0.5f; run(&s, &f, 10, &o); CHECK(vr_input_posture(&s) != VRI_PRONE);
}

static void t_calib(void) {
    vr_input_t s; vr_input_init(&s); vr_input_frame_t f = frame(); vr_pad_t o;
    f.head_valid = false; run(&s, &f, 5, &o); CHECK(vr_input_height(&s) == 0);
    f.head_valid = true; f.head_pos[1] = 1.80f; run(&s, &f, 2, &o); NEAR(vr_input_height(&s), 1.80f, 1e-5f);
    f.head_pos[1] = 1.50f; vr_input_recalibrate(&s); run(&s, &f, 2, &o); NEAR(vr_input_height(&s), 1.50f, 1e-5f);
    CHECK(vr_input_posture(&s) == VRI_STAND);
    vr_input_set_height(&s, 1.7f); NEAR(vr_input_height(&s), 1.7f, 1e-6f);
    vr_input_set_height(&s, NAN); vr_input_set_height(&s, 9); NEAR(vr_input_height(&s), 1.7f, 1e-6f);
    /* la altura de pie solo sube */
    f.head_pos[1] = 1.75f; run(&s, &f, 270, &o); CHECK(vr_input_height(&s) > 1.74f);
}

static void t_menu(void) {
    vr_input_t s; vr_input_init(&s); vr_input_frame_t f = frame(); vr_pad_t o;
    f.menu = true; f.hand[0].stick_y = 1; vr_input_update(&s, &f, &o);
    CHECK(o.buttons == VRI_PAD_UP && o.lx == 0x80 && o.ly == 0x80);
    f.hand[0].stick_y = 0; f.hand[0].stick_x = -1; vr_input_update(&s, &f, &o); CHECK(o.buttons == VRI_PAD_LEFT);
    f.hand[1].stick_x = 1; vr_input_update(&s, &f, &o); NEAR(vr_input_body_yaw(&s), 0, 1e-9f);
    f.hand[1].stick_x = 0; f.hand[0].stick_x = 0; vr_input_update(&s, &f, &o); CHECK(o.buttons == 0);
}

static void t_robust(void) {
    vr_input_t s; vr_input_init(&s); vr_input_frame_t f = frame(); vr_pad_t o;
    f.dt = NAN; f.head_pos[1] = NAN; f.head_quat[0] = INFINITY;
    f.hand[0].stick_x = NAN; f.hand[0].stick_y = 1e9f; f.hand[1].stick_x = -INFINITY; f.hand[1].trigger = NAN;
    f.pad_lx = NAN; run(&s, &f, 10, &o);
    CHECK(isfinite(vr_input_body_yaw(&s)) && isfinite(vr_input_vignette(&s)));
    CHECK(vr_input_vignette(&s) >= 0 && vr_input_vignette(&s) <= 1);
    vri_cfg_t c; memset(&c, 0, sizeof c); c.snap_deg = NAN; c.walk_scale = -3; c.turn_mode = (vri_turn_t)77; vr_input_set_cfg(&s, &c);
    CHECK(s.cfg.snap_deg >= 5 && s.cfg.walk_scale >= 0.1f && s.cfg.turn_mode == VRI_TURN_SNAP);
    vr_input_update(&s, &f, NULL);
}

static void t_review(void) {
    vr_input_t s; vr_input_init(&s); vr_input_frame_t f = frame(); vr_pad_t o;
    /* reset de rumbo con valores enormes: no se cuelga y queda en (-pi, pi] */
    vr_input_reset_body_yaw(&s, 1e30f); CHECK(fabsf(vr_input_body_yaw(&s)) <= 3.1416f);
    vr_input_reset_body_yaw(&s, 7.0f); NEAR(vr_input_body_yaw(&s), 7.0f - 2 * 3.14159265f, 1e-4f);
    /* altura NaN con head_valid: ignorada, sin "tumbado" falso ni pulsaciones */
    run(&s, &f, 5, &o); f.head_pos[1] = NAN; int taps = 0;
    for (int i = 0; i < 200; i++) { vr_input_update(&s, &f, &o); if (o.buttons & VRI_PAD_CROSS) taps++; }
    CHECK(taps == 0 && vr_input_posture(&s) == VRI_STAND);
    /* pico de tracking de 5 fotogramas (+1 m): la altura calibrada apenas sube */
    f.head_pos[1] = 1.65f; run(&s, &f, 5, &o);
    f.head_pos[1] = 2.40f; run(&s, &f, 5, &o);
    f.head_pos[1] = 1.65f; run(&s, &f, 5, &o);
    CHECK(vr_input_height(&s) < 1.65f + 0.02f);
    /* arranque sentado: al ponerse de pie la altura converge y agacharse vuelve a funcionar */
    vr_input_t s2; vr_input_init(&s2); f.head_pos[1] = 1.10f; run(&s2, &f, 3, &o);
    f.head_pos[1] = 1.70f; run(&s2, &f, 90 * 8, &o); CHECK(vr_input_height(&s2) > 1.69f);
    /* giro por pasos: de un extremo al otro entre dos fotogramas (sin pasar por reposo) = dos giros */
    vr_input_t s3; vr_input_init(&s3); vr_input_frame_t g = frame();
    g.hand[1].stick_x = 1; vr_input_update(&s3, &g, &o);
    g.hand[1].stick_x = -1; vr_input_update(&s3, &g, &o);
    CHECK(vr_input_snap_count(&s3) == 2);
    for (int i = 0; i < 60; i++) vr_input_update(&s3, &g, &o);
    CHECK(vr_input_snap_count(&s3) == 2);
}

/* Avance sostenido de la radio, histéresis analógica y antirrebote de menú. */
static void t_hold_advance(void) {
    vr_input_t s; vr_input_init(&s); vr_input_frame_t f = frame(); vr_pad_t o;
    f.menu = true; f.comms = true; f.dt = 0.01f;
    /* toque normal: A derecho = CÍRCULO+CRUZ al instante (se confirma), sin avance sostenido */
    f.hand[1].buttons = VRI_H_LOW; vr_input_update(&s, &f, &o);
    CHECK(o.buttons == (VRI_PAD_CIRCLE | VRI_PAD_CROSS));
    run(&s, &f, 40, &o); CHECK(o.buttons == (VRI_PAD_CIRCLE | VRI_PAD_CROSS));   /* 0,41 s: aún no */
    run(&s, &f, 15, &o);                                       /* 0,56 s: ya en avance */
    /* pasado 0,5 s: CRUZ a toques (nunca CÍRCULO) */
    int on = 0, off = 0, circle = 0, edges = 0; bool prev = false;
    for (int i = 0; i < 300; i++) {
        vr_input_update(&s, &f, &o);
        if (o.buttons & VRI_PAD_CIRCLE) circle++;
        bool c = o.buttons & VRI_PAD_CROSS; if (c) on++; else off++; if (c && !prev) edges++; prev = c;
    }
    CHECK(circle == 0 && on > 0 && off > 0);
    CHECK(edges >= 15 && edges <= 20);                       /* 3 s / 0,16 s */
    NEAR((double)on / (on + off), 0.06 / 0.16, 0.08);
    /* B de la otra mano también cuenta; soltar detiene y reinicia el temporizador */
    f.hand[1].buttons = 0; vr_input_update(&s, &f, &o); CHECK(o.buttons == 0);
    f.hand[0].buttons = VRI_H_UP; vr_input_update(&s, &f, &o); CHECK(o.buttons == VRI_PAD_CROSS);
    run(&s, &f, 20, &o); CHECK(o.buttons == VRI_PAD_CROSS);   /* otra vez desde cero */
    /* cruceta/stick (elegir respuesta) detiene el avance mientras dure; el gatillo no cuenta */
    f.hand[0].buttons = VRI_H_UP; run(&s, &f, 80, &o);
    f.hand[1].stick_x = 1; edges = 0; circle = 0;
    for (int i = 0; i < 100; i++) { vr_input_update(&s, &f, &o); if (o.buttons & VRI_PAD_CIRCLE) circle++; CHECK(o.buttons & VRI_PAD_RIGHT); }
    CHECK(circle == 0);
    f.hand[1].stick_x = 0; f.hand[0].buttons = 0; f.hand[1].trigger = 1.0f; run(&s, &f, 200, &o);
    CHECK(o.buttons == (VRI_PAD_CIRCLE | VRI_PAD_CROSS));
    /* sin comunicador, o desactivado, no hay avance sostenido */
    vr_input_init(&s); f.hand[1].trigger = 0; f.hand[1].buttons = VRI_H_LOW; f.comms = false;
    run(&s, &f, 200, &o); CHECK(o.buttons == (VRI_PAD_CIRCLE | VRI_PAD_CROSS));
    f.comms = true; s.cfg.hold_advance_s = 0; run(&s, &f, 200, &o); CHECK(o.buttons == (VRI_PAD_CIRCLE | VRI_PAD_CROSS));
    /* mantener antes de abrirse el comunicador: el tiempo cuenta desde que se abre */
    vr_input_init(&s); f.comms = false; run(&s, &f, 100, &o); f.comms = true; run(&s, &f, 10, &o);
    CHECK(o.buttons == (VRI_PAD_CIRCLE | VRI_PAD_CROSS));
    /* modo PAD: sin efecto */
    vr_input_init(&s); s.cfg.mode = VRI_MODE_PAD; f.pad_buttons = VRI_PAD_CIRCLE | VRI_PAD_CROSS; run(&s, &f, 200, &o);
    CHECK(o.buttons == (VRI_PAD_CIRCLE | VRI_PAD_CROSS));
}

static void t_hysteresis(void) {
    vr_input_t s; vr_input_init(&s); vr_input_frame_t f = frame(); vr_pad_t o;
    /* gatillo derecho oscilando alrededor de 0,5-0,6 (log real): una sola pulsación, sin parpadeo */
    int edges = 0; bool prev = false;
    for (int i = 0; i < 300; i++) {
        f.hand[1].trigger = (i & 1) ? 0.60f : 0.52f;
        if (i == 0) f.hand[1].trigger = 0.60f;
        vr_input_update(&s, &f, &o); bool c = o.buttons & VRI_PAD_SQUARE; if (c != prev) edges++; prev = c;
    }
    CHECK(edges == 1 && prev);
    f.hand[1].trigger = 0.46f; vr_input_update(&s, &f, &o); CHECK(o.buttons & VRI_PAD_SQUARE);   /* por encima de 0,45 sigue */
    f.hand[1].trigger = 0.44f; vr_input_update(&s, &f, &o); CHECK(!(o.buttons & VRI_PAD_SQUARE)); /* por debajo se suelta */
    f.hand[1].trigger = 0.54f; vr_input_update(&s, &f, &o); CHECK(!(o.buttons & VRI_PAD_SQUARE)); /* hay que superar 0,55 para volver */
    f.hand[1].trigger = 0.56f; vr_input_update(&s, &f, &o); CHECK(o.buttons & VRI_PAD_SQUARE);
    /* config absurda saneada: off < on */
    vri_cfg_t c = s.cfg; c.analog_on = 0.3f; c.analog_off = 0.9f; vr_input_set_cfg(&s, &c); CHECK(s.cfg.analog_off < s.cfg.analog_on);
}

static void t_menu_debounce(void) {
    /* control <-> menú alternando en cada fotograma con ambos gatillos (bucle del log real): el mapa no cambia de golpe */
    vr_input_t s; vr_input_init(&s); vr_input_frame_t f = frame(); vr_pad_t o;
    f.hand[0].trigger = 1; f.hand[1].trigger = 1; f.menu = false; vr_input_update(&s, &f, &o);
    CHECK(o.buttons == (VRI_PAD_L2 | VRI_PAD_SQUARE));
    int switches = 0; uint16_t last = o.buttons;
    for (int i = 0; i < 400; i++) {
        f.menu = (i & 1);
        vr_input_update(&s, &f, &o); if (o.buttons != last) switches++; last = o.buttons;
    }
    CHECK(switches == 0);
    /* un menú real (estable) sí se acepta, con retardo < 0,2 s, y vuelve al soltarse */
    f.menu = true; int n = 0; while (!(o.buttons & VRI_PAD_CROSS) && n < 100) { vr_input_update(&s, &f, &o); n++; }
    CHECK(n >= 1 && n <= 18);
    f.menu = false; n = 0; while ((o.buttons & VRI_PAD_CROSS) && n < 100) { vr_input_update(&s, &f, &o); n++; }
    CHECK(n >= 1 && n <= 18);
    /* sin antirrebote: inmediato */
    s.cfg.menu_debounce_s = 0; f.menu = true; vr_input_update(&s, &f, &o); CHECK(o.buttons & VRI_PAD_CROSS);
}

/* Robustez: entradas aleatorias con NaN/Inf/enormes en todos los campos no abortan ni producen valores fuera de rango. */
static void t_fuzz(void) {
    vr_input_t s; vr_input_init(&s); vr_pad_t o; unsigned r = 12345;
    #define RND() (r = r * 1664525u + 1013904223u, (r >> 8) & 0xFFFF)
    for (int it = 0; it < 20000; it++) {
        vr_input_frame_t f = frame();
        float sp[] = {0, 1, -1, 0.5f, 0.55f, 0.45f, 1e30f, -1e30f, NAN, INFINITY, -INFINITY, 1e-30f};
        #define SP() (sp[RND() % 12])
        f.dt = (RND() & 7) ? (RND() % 200) / 1000.0f : SP();
        f.menu = RND() & 1; f.comms = RND() & 1; f.head_valid = RND() & 1; f.game_posture = (int)(RND() % 5) - 2;
        for (int i = 0; i < 3; i++) { f.head_pos[i] = SP(); }
        for (int i = 0; i < 4; i++) f.head_quat[i] = (RND() & 3) ? 0.0f : SP();
        for (int h = 0; h < 2; h++) {
            f.hand[h].tracked = RND() & 1; f.hand[h].trigger = SP(); f.hand[h].grip = SP();
            f.hand[h].stick_x = SP(); f.hand[h].stick_y = SP(); f.hand[h].buttons = RND() & 15;
            for (int i = 0; i < 4; i++) f.hand[h].quat[i] = (RND() & 3) ? 0.0f : SP();
        }
        s.cfg.mode = (RND() & 7) ? VRI_MODE_SENSE : VRI_MODE_PAD; f.pad_buttons = (uint16_t)RND();
        f.pad_lx = SP(); f.pad_ly = SP(); f.pad_rx = SP(); f.pad_ry = SP();
        vr_input_update(&s, &f, &o);
        if (!isfinite(vr_input_body_yaw(&s)) || vr_input_vignette(&s) < 0 || vr_input_vignette(&s) > 1 || !isfinite(s.adv_phase)) { CHECK(0); return; }
    }
    CHECK(1);
}

int main(void) {
    t_axis(); t_heading(); t_sense_buttons(); t_menu_map(); t_pad_mode(); t_loco(); t_snap(); t_smooth();
    t_vignette(); t_crouch(); t_posture_modes(); t_calib(); t_menu(); t_robust(); t_review(); t_hold_advance(); t_hysteresis(); t_menu_debounce(); t_fuzz();
    printf("%d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
