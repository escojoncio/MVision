#include "vr_input.h"
#include <math.h>
#include <string.h>

#define PI_F 3.14159265358979f

static float fin0(float v) { return isfinite(v) ? v : 0.0f; }
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

uint8_t vr_input_axis_u8(float v) {
    v = clampf(fin0(v), -1.0f, 1.0f);
    int r = v >= 0 ? 128 + (int)lroundf(v * 127.0f) : 128 + (int)lroundf(v * 128.0f);
    return (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
}

float vr_input_quat_heading(const float q[4]) {
    float x = fin0(q[0]), y = fin0(q[1]), z = fin0(q[2]), w = fin0(q[3]);
    float n = x * x + y * y + z * z + w * w;
    if (n < 1e-6f) return 0.0f;
    float inv = 1.0f / sqrtf(n);
    x *= inv; y *= inv; z *= inv; w *= inv;
    /* adelante = q * (0,0,-1) */
    float fx = -2.0f * (x * z + w * y);
    float fy = -2.0f * (y * z - w * x);
    float fz = -(1.0f - 2.0f * (x * x + y * y));
    float hx = fx, hz = fz;
    if (hx * hx + hz * hz < 0.04f) {
        /* mirando casi vertical: usar el "arriba" local (apunta atrás al mirar arriba, adelante al mirar abajo) */
        float ux = 2.0f * (x * y - w * z), uz = 2.0f * (y * z + w * x);
        float s = fy > 0 ? -1.0f : 1.0f;
        hx = s * ux; hz = s * uz;
        if (hx * hx + hz * hz < 1e-8f) return 0.0f;
    }
    return atan2f(hx, -hz);
}

static float wrap_pi(float a) {
    if (!isfinite(a)) return 0.0f;
    if (a > PI_F || a <= -PI_F) {                 /* fmodf: sin bucle aunque a sea enorme (1e30) */
        a = fmodf(a, 2.0f * PI_F);
        if (a > PI_F) a -= 2.0f * PI_F;
        else if (a <= -PI_F) a += 2.0f * PI_F;
    }
    return a;
}

void vr_input_cfg_defaults(vri_cfg_t *c) {
    memset(c, 0, sizeof *c);
    c->mode = VRI_MODE_SENSE;
    c->dominant_hand = 1;
    c->loco_ref = VRI_LOCO_HEAD;
    c->turn_mode = VRI_TURN_SNAP;
    c->snap_deg = 30.0f;
    c->smooth_dps = 120.0f;
    c->vignette = 0.6f;
    c->stick_deadzone = 0.15f;
    c->walk_scale = 1.0f;
    c->pass_right_stick = false;
    c->allow_prone = false;   /* nunca se detecta "tumbado" por la cabeza por defecto: se juega sentado o de pie; tumbarse es solo del botón del juego */
    c->posture_mode = VRI_POSTURE_OFF;
    c->crouch_drop_m = 0.35f;
    c->prone_drop_m = 0.80f;
    c->hysteresis_m = 0.06f;
    c->crouch_btn = VRI_PAD_CROSS;
    c->grip_delay_s = 0.12f;
    c->grip_tap_s = 0.10f;
    c->hold_advance_s = 0.5f; c->adv_on_s = 0.06f; c->adv_off_s = 0.10f;
    c->analog_on = 0.55f; c->analog_off = 0.45f;
    c->menu_debounce_s = 0.12f;
    /* Mapa por defecto (Sense, estándar de los juegos de VR):
     *   mano dominante: A (cara baja) = cruz, B (cara alta) = círculo, gatillo = cuadrado (disparo/golpe: el botón de ataque del
     *     juego; con arma VR en mano lo gobierna el módulo de armas), agarre = R1 (arma), clic = R3, Menú = Start.
     *   mano no dominante: A = cuadrado, B = triángulo, gatillo = L2, agarre = L1 (objeto), clic = L3, Menú = Select;
     *     agarre + gatillo a la vez = R2.
     *   En menús: gatillo y A = confirmar (círculo + cruz: círculo confirma en los menús y cruz avanza texto y salta escenas;
     *     con ambos pulsados el confirmar gana), B = cancelar (cruz). */
    vri_map_t *m = &c->map;
    m->trig_dom = VRI_PAD_SQUARE; m->trig_off = VRI_PAD_L2;
    m->grip_dom = VRI_PAD_R1;     m->grip_off = VRI_PAD_L1;
    m->low_dom = VRI_PAD_CROSS;   m->up_dom = VRI_PAD_CIRCLE;
    m->low_off = VRI_PAD_SQUARE;  m->up_off = VRI_PAD_TRIANGLE;
    m->stick_dom = VRI_PAD_R3;    m->stick_off = VRI_PAD_L3;
    m->menu_dom = VRI_PAD_START;  m->menu_off = VRI_PAD_SELECT;
    m->chord_off = VRI_PAD_R2;
    m->menu_trig = VRI_PAD_CIRCLE | VRI_PAD_CROSS;
    m->menu_low = VRI_PAD_CIRCLE | VRI_PAD_CROSS;
    m->menu_up = VRI_PAD_CROSS;
}

static void sanitize(vri_cfg_t *c) {
    c->mode = c->mode == VRI_MODE_PAD ? VRI_MODE_PAD : VRI_MODE_SENSE;
    c->dominant_hand = c->dominant_hand ? 1 : 0;
    if ((unsigned)c->posture_mode > VRI_POSTURE_FULL) c->posture_mode = VRI_POSTURE_OFF;
    c->loco_ref = c->loco_ref == VRI_LOCO_HAND ? VRI_LOCO_HAND : VRI_LOCO_HEAD;
    if ((unsigned)c->turn_mode > VRI_TURN_OFF) c->turn_mode = VRI_TURN_SNAP;
    c->snap_deg = clampf(fin0(c->snap_deg), 5.0f, 180.0f);
    if (c->snap_deg < 5.0f) c->snap_deg = 30.0f;
    c->smooth_dps = clampf(fin0(c->smooth_dps), 10.0f, 360.0f);
    c->vignette = clampf(fin0(c->vignette), 0.0f, 1.0f);
    c->stick_deadzone = clampf(fin0(c->stick_deadzone), 0.0f, 0.9f);
    c->walk_scale = clampf(fin0(c->walk_scale), 0.1f, 1.0f);
    c->crouch_drop_m = clampf(fin0(c->crouch_drop_m), 0.15f, 0.7f);
    c->prone_drop_m = clampf(fin0(c->prone_drop_m), c->crouch_drop_m + 0.2f, 1.4f);
    c->hysteresis_m = clampf(fin0(c->hysteresis_m), 0.01f, 0.15f);
    c->grip_delay_s = clampf(fin0(c->grip_delay_s), 0.0f, 0.5f);
    c->grip_tap_s = clampf(fin0(c->grip_tap_s), 0.04f, 0.3f);
    c->hold_advance_s = clampf(fin0(c->hold_advance_s), 0.0f, 5.0f);
    c->adv_on_s = clampf(fin0(c->adv_on_s), 0.03f, 0.5f);
    c->adv_off_s = clampf(fin0(c->adv_off_s), 0.03f, 1.0f);
    c->analog_on = clampf(fin0(c->analog_on), 0.2f, 0.95f);
    c->analog_off = clampf(fin0(c->analog_off), 0.05f, c->analog_on - 0.02f);
    c->menu_debounce_s = clampf(fin0(c->menu_debounce_s), 0.0f, 0.5f);
}

void vr_input_set_cfg(vr_input_t *s, const vri_cfg_t *cfg) {
    s->cfg = *cfg;
    sanitize(&s->cfg);
}

void vr_input_init(vr_input_t *s) {
    memset(s, 0, sizeof *s);
    vr_input_cfg_defaults(&s->cfg);
    s->turn_armed = 1;
    s->need_calib = true;
}

float vr_input_body_yaw(const vr_input_t *s) { return s->body_yaw; }
void vr_input_reset_body_yaw(vr_input_t *s, float yaw) { s->body_yaw = wrap_pi(fin0(yaw)); }
uint32_t vr_input_snap_count(const vr_input_t *s) { return s->snap_count; }
float vr_input_vignette(const vr_input_t *s) { return s->vig; }
void vr_input_recalibrate(vr_input_t *s) { s->need_calib = true; }
void vr_input_set_height(vr_input_t *s, float h) {
    if (isfinite(h) && h > 0.5f && h < 2.5f) { s->calib_h = h; s->need_calib = false; }
}
float vr_input_height(const vr_input_t *s) { return s->calib_h; }
int vr_input_posture(const vr_input_t *s) { return s->desired; }
void vr_input_set_game_up(vr_input_t *s, float h) { s->game_up = fin0(h); }

/* Zona muerta radial con reescalado; escribe el vector resultante. */
static float radial(float *x, float *y, float dz) {
    float vx = clampf(fin0(*x), -1, 1), vy = clampf(fin0(*y), -1, 1);
    float m = sqrtf(vx * vx + vy * vy);
    if (m <= dz || m < 1e-6f) { *x = *y = 0; return 0; }
    float k = clampf((m - dz) / (1.0f - dz), 0.0f, 1.0f);
    *x = vx / m * k; *y = vy / m * k;
    return k;
}

static bool hys(bool *st, float v, float on, float off) {
    v = fin0(v);
    if (*st) { if (v < off) *st = false; }
    else if (v > on) *st = true;
    return *st;
}

/* Botones digitales y gatillos/agarres de las dos manos -> máscara original. Sin estado salvo el de los agarres (acorde y retardo). */
static uint16_t map_sense(vr_input_t *s, const vr_input_frame_t *f, bool menu, int dom, float dt) {
    const vri_cfg_t *c = &s->cfg;
    const vri_map_t *m = &c->map;
    uint16_t b = 0;
    for (int role = 0; role < 2; role++) {            /* role 0 = dominante, 1 = no dominante */
        int hi = role == 0 ? dom : 1 - dom;           /* índice físico */
        const vri_hand_t *h = &f->hand[hi];
        bool trig = hys(&s->hold_trig[role], h->trigger, c->analog_on, c->analog_off);
        bool grip = hys(&s->hold_grip[role], h->grip, c->analog_on, c->analog_off);
        bool use_chord = role == 1 && !menu && m->chord_off != 0 && c->grip_delay_s > 0.0f;
        /* acorde: agarre + gatillo con el agarre aún sin emitir; dura mientras se mantenga el agarre */
        bool was_chord = s->grip_chord[role];
        if (!grip) s->grip_chord[role] = false;
        else if (use_chord && trig && !s->grip_out[role]) s->grip_chord[role] = true;
        if (!use_chord) s->grip_chord[role] = false;
        bool chord = s->grip_chord[role];
        /* agarre: L1/R1 con retardo solo en la mano del acorde (un toque corto emite un pulso al soltar) */
        bool g_out = false;
        if (grip) {
            if (!s->grip_prev[role]) { s->grip_t[role] = 0.0f; s->grip_out[role] = false; }
            s->grip_t[role] += dt;
            float delay = use_chord ? c->grip_delay_s : 0.0f;
            if (!chord && s->grip_t[role] >= delay) { g_out = true; s->grip_out[role] = true; }
        } else if (s->grip_prev[role] && !s->grip_out[role] && !was_chord && !menu) {
            s->grip_tap[role] = c->grip_tap_s;         /* agarre más corto que el retardo: toque de L1 */
        }
        s->grip_prev[role] = grip;
        if (s->grip_tap[role] > 0.0f) { g_out = true; s->grip_tap[role] -= dt; }
        if (menu) s->grip_tap[role] = 0.0f;
        if (g_out) b |= role == 0 ? m->grip_dom : m->grip_off;
        if (chord) { if (trig) b |= m->chord_off; }
        else if (trig) b |= menu ? m->menu_trig : (role == 0 ? m->trig_dom : m->trig_off);
        if (h->buttons & VRI_H_LOW) b |= menu ? m->menu_low : (role == 0 ? m->low_dom : m->low_off);
        if (h->buttons & VRI_H_UP) b |= menu ? m->menu_up : (role == 0 ? m->up_dom : m->up_off);
        if (h->buttons & VRI_H_STICK) b |= role == 0 ? m->stick_dom : m->stick_off;
        if (h->buttons & VRI_H_MENU) b |= role == 0 ? m->menu_dom : m->menu_off;
    }
    return b;
}

uint16_t vr_input_sense_plain(const vri_cfg_t *cfg, const vri_hand_t hand[2]) {
    if (!cfg || !hand) return 0;
    const vri_map_t *m = &cfg->map;
    int dom = cfg->dominant_hand ? 1 : 0;
    uint16_t b = 0;
    float sx = 0, sy = 0;
    for (int role = 0; role < 2; role++) {
        const vri_hand_t *h = &hand[role == 0 ? dom : 1 - dom];
        if (fin0(h->trigger) > 0.55f) b |= m->menu_trig;
        if (fin0(h->grip) > 0.55f) b |= role == 0 ? m->grip_dom : m->grip_off;
        if (h->buttons & VRI_H_LOW) b |= m->menu_low;
        if (h->buttons & VRI_H_UP) b |= m->menu_up;
        if (h->buttons & VRI_H_STICK) b |= role == 0 ? m->stick_dom : m->stick_off;
        if (h->buttons & VRI_H_MENU) b |= role == 0 ? m->menu_dom : m->menu_off;
        float x = fin0(h->stick_x), y = fin0(h->stick_y);
        if (x * x + y * y > sx * sx + sy * sy) { sx = x; sy = y; }
    }
    if (sy > 0.55f) b |= VRI_PAD_UP;
    if (sy < -0.55f) b |= VRI_PAD_DOWN;
    if (sx > 0.55f) b |= VRI_PAD_RIGHT;
    if (sx < -0.55f) b |= VRI_PAD_LEFT;
    return b;
}

/* Agacharse físico: devuelve la máscara de botón a pulsar este fotograma. */
enum { PH_IDLE = 0, PH_ON = 1, PH_SETTLE = 2 };

static uint16_t crouch_step(vr_input_t *s, const vr_input_frame_t *f, float dt) {
    const vri_cfg_t *c = &s->cfg;
    if (!f->head_valid) return 0;
    /* NaN/inf o altura imposible = pose basura: se ignora el fotograma (antes y=0 -> "tumbado" falso). */
    if (!isfinite(f->head_pos[1]) || f->head_pos[1] > 2.5f) return 0;
    float y = f->head_pos[1];
    if (s->need_calib || s->calib_h <= 0.0f) {
        if (y > 0.5f) { s->calib_h = y; s->need_calib = false; s->desired = s->prev_desired = VRI_STAND; s->crouch_pending = false; }
        return 0;
    }
    if ((s->desired == VRI_STAND || c->posture_mode == VRI_POSTURE_OFF) && y > s->calib_h)
    {   /* la altura "de pie" solo sube, con velocidad limitada (0.15 m/s): un pico de tracking no la infla */
        float step = fminf((y - s->calib_h) * 2.0f, 0.15f) * dt;
        s->calib_h += step;
    }
    if (c->posture_mode == VRI_POSTURE_OFF) {          /* la postura es solo del botón del juego: se sigue, no se fuerza */
        if (f->game_posture >= 0 && f->game_posture <= 2) s->desired = s->prev_desired = f->game_posture;
        s->crouch_pending = false; s->crouch_phase = PH_IDLE;
        return 0;
    }
    float drop = s->calib_h - y;
    int d = s->desired;
    float hy = c->hysteresis_m;
    if (d == VRI_STAND) { if (drop > c->crouch_drop_m) d = VRI_CROUCH; }
    if (d == VRI_CROUCH) {
        if (c->allow_prone && drop > c->prone_drop_m) d = VRI_PRONE;
        else if (drop < c->crouch_drop_m - hy) d = VRI_STAND;
    } else if (d == VRI_PRONE) {
        if (!c->allow_prone) d = VRI_CROUCH;
        else if (drop < c->prone_drop_m - hy) d = drop < c->crouch_drop_m - hy ? VRI_STAND : VRI_CROUCH;
    }
    if (d != s->desired) {
        s->prev_desired = s->desired;
        s->desired = d;
        /* DOWN: solo se actúa al bajar y solo si el juego está más alto que lo pedido; subir o igualar no pulsa nada */
        bool act = c->posture_mode == VRI_POSTURE_FULL || (d > s->prev_desired && (f->game_posture < 0 || f->game_posture < d));
        if (!act) { s->crouch_pending = false; goto no_change; }
        s->crouch_pending = true;
        s->retries = 0;
        int n = d > s->prev_desired ? d - s->prev_desired : s->prev_desired - d;
        s->taps_left = n < 1 ? 1 : n;
        if (s->crouch_phase == PH_ON) { s->crouch_phase = PH_SETTLE; s->crouch_timer = 0.15f; }
    }
no_change:
    if (f->menu) { s->crouch_pending = false; s->crouch_phase = PH_IDLE; return 0; }

    uint16_t out = 0;
    s->crouch_timer -= dt;
    if (s->crouch_phase == PH_ON) {
        out = c->crouch_btn;
        if (s->crouch_timer <= 0) { s->crouch_phase = PH_SETTLE; s->crouch_timer = f->game_posture >= 0 ? 0.6f : 0.25f; }
    } else if (s->crouch_phase == PH_SETTLE) {
        if (s->crouch_timer <= 0) s->crouch_phase = PH_IDLE;
    }
    if (s->crouch_phase == PH_IDLE && s->crouch_pending) {
        bool ok;
        if (f->game_posture >= 0) ok = c->posture_mode == VRI_POSTURE_DOWN ? f->game_posture >= s->desired : f->game_posture == s->desired;   /* DOWN: el juego ya igual o más bajo (botón manual) = nada que corregir, sin reintentos */
        else ok = s->taps_left <= 0;
        if (ok || s->retries >= 3) s->crouch_pending = false;
        else {
            s->crouch_phase = PH_ON; s->crouch_timer = 0.09f; s->retries++; s->taps_left--;
            out = c->crouch_btn;
        }
    }
    return out;
}

/* Avance sostenido de la radio (ver vri_cfg_t.hold_advance_s). Solo en modo Sense con el comunicador abierto. Se parte de los botones ya
 * calculados: tras hold_advance_s con una cara A/B apretada se quitan CÍRCULO/CRUZ y se emite CRUZ a toques (nunca CÍRCULO). Cualquier
 * cruceta/stick desvía la intención (elegir respuesta) y detiene el avance hasta soltar. */
static uint16_t hold_advance(vr_input_t *s, const vr_input_frame_t *f, uint16_t b, float dt) {
    const vri_cfg_t *c = &s->cfg;
    bool face = false;
    for (int i = 0; i < 2; i++) if (f->hand[i].buttons & (VRI_H_LOW | VRI_H_UP)) face = true;
    bool dpad = (b & (VRI_PAD_UP | VRI_PAD_DOWN | VRI_PAD_LEFT | VRI_PAD_RIGHT)) != 0;
    if (c->mode != VRI_MODE_SENSE || c->hold_advance_s <= 0.0f || !f->comms || !face || dpad) {
        s->adv_hold = 0.0f; s->adv_active = false; s->adv_phase = 0.0f;
        return b;
    }
    s->adv_hold += dt;
    if (s->adv_hold < c->hold_advance_s) return b;
    if (!s->adv_active) { s->adv_active = true; s->adv_phase = 0.0f; }
    float period = c->adv_on_s + c->adv_off_s;
    s->adv_phase = fmodf(s->adv_phase + dt, period);
    b = (uint16_t)(b & ~(VRI_PAD_CIRCLE | VRI_PAD_CROSS));
    if (s->adv_phase < c->adv_on_s) b |= VRI_PAD_CROSS;
    return b;
}

void vr_input_update(vr_input_t *s, const vr_input_frame_t *f_in, vr_pad_t *out) {
    vri_cfg_t *c = &s->cfg;
    float dt = clampf(fin0(f_in->dt), 0.001f, 0.1f);
    /* Antirrebote del estado de menú: el juego puede alternar control/menú cada fotograma (p. ej. una acción que retira el control un
     * instante y lo devuelve); sin esto el mapa saltaba entre juego y menú en cada fotograma. El primer valor se acepta tal cual. */
    if (!s->menu_init || c->menu_debounce_s <= 0.0f) { s->menu_eff = f_in->menu; s->menu_init = true; s->menu_t = 0.0f; }
    else if (f_in->menu == s->menu_eff) s->menu_t = 0.0f;
    else if ((s->menu_t += dt) >= c->menu_debounce_s) { s->menu_eff = f_in->menu; s->menu_t = 0.0f; }
    vr_input_frame_t fr = *f_in;
    fr.menu = s->menu_eff;
    const vr_input_frame_t *f = &fr;
    int dom = c->dominant_hand ? 1 : 0;
    uint16_t buttons;
    float mx, my, tx, ty;                   /* stick de mover / de girar */
    const vri_hand_t *move_hand = &f->hand[1 - dom];

    if (c->mode == VRI_MODE_PAD) {
        buttons = f->pad_buttons;
        mx = f->pad_lx; my = f->pad_ly; tx = f->pad_rx; ty = f->pad_ry;
    } else {
        buttons = map_sense(s, f, f->menu, dom, dt);
        mx = move_hand->stick_x; my = move_hand->stick_y;
        tx = f->hand[dom].stick_x; ty = f->hand[dom].stick_y;
    }
    float mmag = radial(&mx, &my, c->stick_deadzone);
    float raw_rx = tx, raw_ry = ty;     /* para pasar tal cual (sin zona muerta radial) */
    radial(&tx, &ty, c->stick_deadzone);

    uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    float omega = 0.0f, lin = 0.0f;

    if (f->menu) {
        /* Menús del juego: el stick de mover hace de cruceta (el digital del mando se conserva en modo PAD). */
        const float on = c->analog_on, off = c->analog_off;
        if (c->mode == VRI_MODE_SENSE && tx * tx + ty * ty > mx * mx + my * my) { mx = tx; my = ty; }   /* en menús vale cualquiera de los dos sticks (el de mayor desvío) */
        bool *dp = s->menu_dpad;                 /* arriba, derecha, abajo, izquierda */
        hys(&dp[0], my, on, off); hys(&dp[2], -my, on, off);
        hys(&dp[1], mx, on, off); hys(&dp[3], -mx, on, off);
        if (dp[0]) buttons |= VRI_PAD_UP;
        if (dp[1]) buttons |= VRI_PAD_RIGHT;
        if (dp[2]) buttons |= VRI_PAD_DOWN;
        if (dp[3]) buttons |= VRI_PAD_LEFT;
        s->turn_armed = 1;
        buttons = hold_advance(s, f, buttons, dt);
    } else {
        s->adv_hold = 0.0f; s->adv_active = false; s->adv_phase = 0.0f;
        memset(s->menu_dpad, 0, sizeof s->menu_dpad);
        /* giro */
        bool turn_on = c->turn_mode != VRI_TURN_OFF && !(c->mode == VRI_MODE_PAD && c->pass_right_stick);
        if (turn_on) {
            if (c->turn_mode == VRI_TURN_SNAP) {
                float ax = fabsf(tx);
                int dir = tx > 0 ? 1 : -1;
                /* también gira si el stick pasa de un lado al otro entre dos fotogramas sin pasar por reposo */
                if ((s->turn_armed || dir != s->turn_dir) && ax > 0.7f) {
                    float step = c->snap_deg * (PI_F / 180.0f);
                    s->body_yaw = wrap_pi(s->body_yaw + (tx > 0 ? step : -step));
                    s->snap_count++;
                    s->snap_pulse = 1.0f;
                    s->turn_armed = 0;
                    s->turn_dir = dir;
                } else if (ax < 0.35f) s->turn_armed = 1;
            } else {
                omega = tx * c->smooth_dps * (PI_F / 180.0f);
                s->body_yaw = wrap_pi(s->body_yaw + omega * dt);
            }
        } else if (c->mode == VRI_MODE_PAD && c->pass_right_stick) {
            rx = vr_input_axis_u8(raw_rx);
            ry = vr_input_axis_u8(-raw_ry);
        }
        /* locomoción */
        if (mmag > 0.0f) {
            float ref = 0.0f;
            if (c->loco_ref == VRI_LOCO_HAND && c->mode == VRI_MODE_SENSE && move_hand->tracked)
                ref = vr_input_quat_heading(move_hand->quat);
            else if (f->head_valid)
                ref = vr_input_quat_heading(f->head_quat);
            float heading = ref + s->body_yaw + atan2f(mx, my);
            float rel = heading - s->game_up;
            float mag = mmag * c->walk_scale;
            lin = mag;
            lx = vr_input_axis_u8(mag * sinf(rel));
            ly = vr_input_axis_u8(-mag * cosf(rel));     /* original: abajo positivo */
        }
        buttons |= crouch_step(s, f, dt);
    }

    /* viñeta: máx(giro suave, pulso de giro por pasos, locomoción), suavizada (ataque rápido, suelta lenta) */
    float target = 0.0f;
    if (c->vignette > 0.0f && !f->menu) {
        float t_ang = clampf(fabsf(omega) / (150.0f * PI_F / 180.0f), 0.0f, 1.0f);
        float t_lin = clampf(lin * 0.35f, 0.0f, 1.0f);
        target = c->vignette * fmaxf(fmaxf(t_ang, 0.6f * s->snap_pulse), t_lin);
    }
    s->snap_pulse = fmaxf(0.0f, s->snap_pulse - dt / 0.25f);
    float tau = target > s->vig ? 0.06f : 0.5f;
    s->vig += (target - s->vig) * (1.0f - expf(-dt / tau));
    s->vig = clampf(s->vig, 0.0f, 1.0f);
    s->omega = omega; s->lin = lin;

    s->last.buttons = buttons;
    s->last.lx = lx; s->last.ly = ly; s->last.rx = rx; s->last.ry = ry;
    if (out) *out = s->last;
}
