#include "vr_melee.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- acceso a la RAM del invitado (little endian, KSEG0) ---- */
static int okr(uint32_t a, uint32_t n) { uint32_t o = a & 0x1FFFFFu; return (a & 0xE0000000u) == 0x80000000u && o + n <= VRM_RAM_SIZE; }
static uint32_t r32(const VrmEng *e, uint32_t a) { uint32_t v = 0; if (okr(a, 4)) memcpy(&v, e->ram + (a & 0x1FFFFFu), 4); return v; }
static int16_t r16(const VrmEng *e, uint32_t a) { int16_t v = 0; if (okr(a, 2)) memcpy(&v, e->ram + (a & 0x1FFFFFu), 2); return v; }
static uint8_t r8(const VrmEng *e, uint32_t a) { return okr(a, 1) ? e->ram[a & 0x1FFFFFu] : 0; }
static void w32(const VrmEng *e, uint32_t a, uint32_t v) { if (okr(a, 4)) memcpy(e->ram + (a & 0x1FFFFFu), &v, 4); }
static void w16(const VrmEng *e, uint32_t a, int16_t v) { if (okr(a, 2)) memcpy(e->ram + (a & 0x1FFFFFu), &v, 2); }

static void say(Vrm *v, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void say(Vrm *v, const char *fmt, ...) {
    char b[256]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (v->log) v->log(b); else fprintf(stderr, "%s\n", b);
}

void vrm_init(Vrm *v) { memset(v, 0, sizeof *v); v->enable = 1; v->ttl_frames = 8; v->p_punch_side = v->p_grab_side = v->p_knock_side = -1; }
void vrm_reset(Vrm *v) { void (*lg)(const char *) = v->log; int en = v->enable, ttl = v->ttl_frames; vrm_init(v); v->log = lg; v->enable = en; v->ttl_frames = ttl; }

/* ---- enemigos (blancos del motor) ---- */
#define MAXEN 32
typedef struct { float c[3], h[3]; int vital; } En;
static int scan(const VrmEng *e, int grab, En *out) {
    int n = 0;
    for (int i = 0; i < 64 && n < MAXEN; i++) {
        uint32_t b = VRM_TARGETS + 72u * (uint32_t)i;
        uint16_t cl = (uint16_t)r16(e, b), side = (uint16_t)r16(e, b + 2);
        if (side != 2 || !(cl & 1) || (cl & 0x20)) continue;
        if (grab ? !(cl & 2) : !(cl & 4)) continue;
        int vital = r16(e, b + 0x26);
        if (vital <= 0) continue;
        for (int k = 0; k < 3; k++) { out[n].c[k] = r16(e, b + 8 + 2u * (uint32_t)k); out[n].h[k] = r16(e, b + 16 + 2u * (uint32_t)k); }
        out[n].vital = vital; n++;
    }
    return n;
}
/* distancia (mm) de un punto a la superficie de la caja (0 dentro), con una caja de medio tamaño extra `grow` */
static float box_dist(const float p[3], const En *en, const float grow[3]) {
    float s = 0;
    for (int k = 0; k < 3; k++) { float d = fabsf(p[k] - en->c[k]) - (en->h[k] + grow[k]); if (d > 0) s += d * d; }
    return sqrtf(s);
}
static float nearest(const float p[3], const En *en, int n, const float grow[3]) {
    float best = 1e9f;
    for (int i = 0; i < n; i++) { float d = box_dist(p, &en[i], grow); if (d < best) best = d; }
    return best;
}

/* ---- por VBlank ---- */
static void push_hist(Vrm *v, int s, const float p[3], int closed) {
    int i = v->hist_i[s];
    memcpy(v->hist[s][i], p, 3 * sizeof(float)); v->hist_cl[s][i] = (unsigned char)closed;
    v->hist_i[s] = (i + 1) % VRM_HIST; if (v->hist_n[s] < VRM_HIST) v->hist_n[s]++;
}
void vrm_frame(Vrm *v, const VrmEng *e, const VrmFrame *f) {
    if (!v->enable) { v->p_punch = v->p_grab = 0; v->hold = 0; v->squeeze_pend = 0; return; }
    if (v->p_punch && --v->p_punch_ttl <= 0) {
        v->p_punch = 0; v->n_expired++;
        say(v, "[gest] golpe rechazado: el motor no aceptó la acción (estado=%08x)", r32(e, r32(e, VRM_PLAYER_CTRL) - 0x20 + VRM_W_ANIMFN));
    }
    if (v->p_grab && --v->p_grab_ttl <= 0) {
        v->p_grab = 0; v->n_expired++; v->n_grab_rejected++;
        say(v, "[gest] agarre rechazado: el motor no aceptó la acción (estado=%08x)", r32(e, r32(e, VRM_PLAYER_CTRL) - 0x20 + VRM_W_ANIMFN));
    }
    if (v->p_knock && --v->p_knock_ttl <= 0) {
        v->p_knock = 0; v->n_expired++;
        say(v, "[gest] toque en la pared rechazado: el motor no lo aceptó (estado=%08x)", r32(e, r32(e, VRM_PLAYER_CTRL) - 0x20 + VRM_W_ANIMFN));
    }
    if (f->knock_side >= 0 && f->knock_side < 2) { v->p_knock = 1; v->p_knock_side = f->knock_side; v->p_knock_ttl = v->ttl_frames; }
    for (int s = 0; s < 2; s++) {
        v->fist_ok[s] = f->fist_ok[s]; v->fist_closed[s] = f->fist_closed[s];
        if (f->fist_ok[s]) { memcpy(v->fist[s], f->fist[s], sizeof v->fist[s]); push_hist(v, s, f->fist[s], f->fist_closed[s]); }
        else v->hist_n[s] = 0;
    }
    v->hold = f->hold;
    if (!v->grab_owned) v->squeeze_pend = 0;
    if (f->squeeze) { v->squeeze_pend = 1; }
    if (f->punch_side >= 0 && f->punch_side < 2) { v->p_punch = 1; v->p_punch_side = f->punch_side; v->p_punch_ttl = v->ttl_frames; }
    if (f->grab_start && f->grab_side >= 0 && f->grab_side < 2) { v->p_grab = 1; v->p_grab_side = f->grab_side; v->p_grab_ttl = v->ttl_frames; }
}

/* origen del puñetazo: de las últimas muestras con el puño cerrado, la más cercana a algún enemigo (la animación del motor llega unos
 * fotogramas después del gesto: el puño ya pasó por el punto de contacto). Devuelve 0 sin muestras. */
static int pick_origin(Vrm *v, const VrmEng *e, float out[3], float *dist) {
    En en[MAXEN]; int n = scan(e, 0, en);
    const float zero[3] = {0, 0, 0};
    float best = 1e9f; int found = 0;
    for (int s = 0; s < 2; s++)
        for (int k = 0; k < v->hist_n[s]; k++) {
            if (!v->hist_cl[s][k]) continue;
            float d = n ? nearest(v->hist[s][k], en, n, zero) : 0;
            /* a igualdad de distancia gana la más reciente (se recorre de antigua a nueva... no importa: se toma <=) */
            if (!found || d <= best) { best = d; memcpy(out, v->hist[s][k], 3 * sizeof(float)); found = 1; }
        }
    if (dist) *dist = best;
    return found;
}

int vrm_knock_check(Vrm *v, const VrmEng *e, uint32_t work, uint32_t time) {
    if (!v->enable || !v->p_knock || time < 8 || !okr(work, 2700)) return 0;
    int moving = (r32(e, VRM_G_PSTATUS) & 0x10u) != 0;
    if (r32(e, work + VRM_W_WPN) == 6u && !moving) { v->p_knock = 0; say(v, "[gest] toque en la pared rechazado: el equipo actual no llama"); return 0; }
    w32(e, work + VRM_W_FLAGS, r32(e, work + VRM_W_FLAGS) | 0x100u);
    w32(e, work + VRM_W_KNOCK_FN, VRM_ST_KNOCK);
    w32(e, work + VRM_W_KNOCK_T, 0);
    v->p_knock = 0; v->n_knock++;
    say(v, "[gest] toque en la pared disparado mano=%d (estado de nudillos del motor)", v->p_knock_side);
    return 1;
}

int vrm_action_input(Vrm *v, const VrmEng *e, uint32_t work) {
    if (!v->enable || !okr(work, 2700)) return 0;
    if (v->p_grab) {
        w32(e, work + VRM_W_F910, 0);
        e->call(e->user, VRM_FN_START, work, VRM_FN_GRAB);
        w32(e, work + VRM_W_FLAGS, r32(e, work + VRM_W_FLAGS) | 4u);
        w32(e, VRM_G_PSTATUS, r32(e, VRM_G_PSTATUS) & ~0x10u);
        v->grab_owned = 1; v->p_grab = 0; v->n_grab++;
        say(v, "[gest] agarre disparado mano=%d (estado de presa del motor)", v->p_grab_side);
        return 1;
    }
    if (v->p_punch) {
        uint32_t ap = r32(e, work + VRM_W_ACTPACK), atk = okr(ap + 16, 4) ? r32(e, ap + 16) : 0;
        if (!atk || r8(e, atk + 2) == 0x88) {   /* sin golpe con lo que lleva equipado */
            v->p_punch = 0; say(v, "[gest] golpe rechazado: el equipo actual no tiene golpe");
            return 0;
        }
        e->call(e->user, VRM_FN_START, work, VRM_FN_PUNCH);
        w32(e, work + VRM_W_FLAGS, r32(e, work + VRM_W_FLAGS) | 4u);
        v->p_punch = 0; v->in_punch = 1; v->stage = 0; v->n_punch++;
        say(v, "[gest] golpe disparado mano=%d (estado de golpe del motor)", v->p_punch_side);
        return 1;
    }
    v->in_punch = 0;
    return 0;
}

void vrm_pre_grab(Vrm *v, const VrmEng *e, uint32_t work, uint32_t time) {
    (void)e;
    if (!v->enable || time != 0 || !v->grab_owned) return;
    int s = v->p_grab_side;
    if (s < 0 || s > 1 || !v->fist_ok[s]) return;
    v->ctx_grab = 1; v->ctx_ok = 1; v->ctx_target = work + VRM_W_TARGET; memcpy(v->ctx_origin, v->fist[s], sizeof v->ctx_origin);
}
void vrm_post_grab(Vrm *v, const VrmEng *e, uint32_t work, uint32_t time) {
    if (!v->enable || time != 0) return;
    if (!v->ctx_grab) { v->grab_owned = 0; return; }   /* presa iniciada por el botón del juego */
    v->ctx_grab = 0; v->ctx_ok = 0;
    if (r32(e, work + VRM_W_PTARGET) && r16(e, work + VRM_W_LOCAL) == 0) {
        say(v, "[gest] agarre iniciado (presa tomada con la caja centrada en la mano: %.0f,%.0f,%.0f mm)", v->ctx_origin[0], v->ctx_origin[1], v->ctx_origin[2]);
    } else {
        v->grab_owned = 0; v->n_grab_rejected++;
        say(v, "[gest] agarre rechazado por el motor: ninguna presa en la caja centrada en la mano (%.0f,%.0f,%.0f mm)", v->ctx_origin[0], v->ctx_origin[1], v->ctx_origin[2]);
    }
}

void vrm_pre_hit(Vrm *v, const VrmEng *e, uint32_t work) {
    if (!v->enable || !v->in_punch || r32(e, work + VRM_W_ANIMFN) != VRM_FN_PUNCH) return;
    float o[3], d;
    v->stage++;
    if (!pick_origin(v, e, o, &d)) { say(v, "[gest] golpe etapa %d: sin puño VR con seguimiento; el motor usa su origen", v->stage); return; }
    v->ctx_hit = 1; v->ctx_ok = 1; v->ctx_target = work + VRM_W_TARGET; memcpy(v->ctx_origin, o, sizeof o);
}
void vrm_post_hit(Vrm *v, const VrmEng *e, uint32_t work, uint32_t result) {
    if (!v->ctx_hit) return;
    v->ctx_hit = 0; v->ctx_ok = 0;
    float half[3]; for (int k = 0; k < 3; k++) half[k] = r16(e, work + VRM_W_TARGET + 16 + 2u * (uint32_t)k);
    En en[MAXEN]; int n = scan(e, 0, en);
    float d = n ? nearest(v->ctx_origin, en, n, half) : -1.0f;
    if (n && d <= 0) { v->n_hit++; say(v, "[gest] golpe etapa %d: IMPACTO con el puño VR (%.0f,%.0f,%.0f mm), motor=%u", v->stage, v->ctx_origin[0], v->ctx_origin[1], v->ctx_origin[2], result); }
    else { v->n_miss++; say(v, "[gest] golpe etapa %d: FALLO, el puño VR queda a %.2f m del enemigo más cercano", v->stage, d < 0 ? -1.0f : d / 1000.0f); }
}

int vrm_move_target(Vrm *v, const VrmEng *e, uint32_t target, uint32_t vec) {
    if (!v->ctx_ok || !(v->ctx_hit || v->ctx_grab) || target != v->ctx_target || !okr(vec, 6)) return 0;
    for (int k = 0; k < 3; k++) {
        float x = v->ctx_origin[k]; if (!(x == x)) return 0;
        if (x > 32767.0f) { x = 32767.0f; }
        if (x < -32768.0f) { x = -32768.0f; }
        w16(e, vec + 2u * (uint32_t)k, (int16_t)lrintf(x));
    }
    return 1;
}

void vrm_punch_tick(Vrm *v, const VrmEng *e, uint32_t work, uint32_t time) {
    (void)time;
    if (!v->enable || !v->p_punch) return;
    /* nueva petición durante la animación: continuación del combo (el motor mira este bit en los tiempos 6 y 13) */
    w16(e, work + VRM_W_LOCAL, (int16_t)(r16(e, work + VRM_W_LOCAL) | 0x20));
    v->p_punch = 0; v->n_combo++;
    say(v, "[gest] golpe encadenado (combo) mano=%d en el tiempo %u de la animación", v->p_punch_side, time);
}

static void end_hold(Vrm *v, const VrmEng *e, uint32_t work) {
    e->call(e->user, VRM_FN_HOLD_END, work, 2);
    e->call(e->user, VRM_FN_START, work, VRM_ST_HOLD);
    w32(e, work + VRM_W_FLAGS, r32(e, work + VRM_W_FLAGS) & ~4u);
    v->grab_owned = 0;
    say(v, "[gest] agarre terminado");
}
int vrm_hold_tick(Vrm *v, const VrmEng *e, uint32_t work) {
    if (!v->enable || !v->grab_owned) return 0;       /* presa por el botón del juego: el original */
    uint32_t fr = r32(e, work + VRM_W_FRAMES) + 1;
    w32(e, work + VRM_W_FRAMES, fr);
    uint32_t tg = r32(e, work + VRM_W_PTARGET), pad = r32(e, work + VRM_W_PAD);
    uint16_t status = okr(pad, 4) ? (uint16_t)r16(e, pad) : 0, press = okr(pad, 4) ? (uint16_t)r16(e, pad + 2) : 0;
    int held = v->hold || (status & VRM_PAD_ACT), sq = v->squeeze_pend || (press & VRM_PAD_ACT);
    if (!okr(tg, 72) || r16(e, tg + 38) < 1 || fr >= 90 || r16(e, tg + 66) == 0) { end_hold(v, e, work); v->squeeze_pend = 0; return 1; }
    if (sq) {
        v->squeeze_pend = 0; v->n_squeeze++;
        e->call(e->user, VRM_FN_START, work, VRM_ST_SQUEEZE);
        say(v, "[gest] apretar (estado de apretón del motor)");
    } else if (!held) {
        int16_t l = (int16_t)(r16(e, work + VRM_W_LOCAL) + 1); w16(e, work + VRM_W_LOCAL, l);
        if (l >= 9) end_hold(v, e, work);
    } else {
        w16(e, work + VRM_W_LOCAL, 0);
        if ((int32_t)r32(e, VRM_G_MOVEDIR) >= 0) e->call(e->user, VRM_FN_START, work, VRM_ST_DRAG);
    }
    return 1;
}
int vrm_squeeze_tick(Vrm *v, const VrmEng *e, uint32_t work, uint32_t time) {
    if (!v->enable || !v->grab_owned || time == 0 || !v->squeeze_pend) return 0;
    uint32_t tg = r32(e, work + VRM_W_PTARGET);
    if (!okr(tg, 72)) return 0;
    v->squeeze_pend = 0; v->n_squeeze++;
    w16(e, tg + 42, (int16_t)(r16(e, tg + 42) - 1));
    int16_t c = (int16_t)(r16(e, work + VRM_W_SQZCNT) + 1); w16(e, work + VRM_W_SQZCNT, c);
    say(v, "[gest] apretar %d/10", c);
    if (c >= 10) { e->call(e->user, VRM_FN_START, work, VRM_ST_KILL); say(v, "[gest] remate"); return 1; }
    return 0;
}

void vrm_pre_drag(Vrm *v, const VrmEng *e, uint32_t work, uint16_t *saved) {
    *saved = 0xFFFF;
    if (!v->enable || !v->grab_owned || !v->hold) return;
    uint32_t pad = r32(e, work + VRM_W_PAD);
    if (!okr(pad, 4)) return;
    *saved = (uint16_t)r16(e, pad); w16(e, pad, (int16_t)(*saved | VRM_PAD_ACT));
}
void vrm_post_drag(Vrm *v, const VrmEng *e, uint32_t work, uint16_t saved) {
    (void)v;
    if (saved == 0xFFFF) return;
    uint32_t pad = r32(e, work + VRM_W_PAD);
    if (okr(pad, 4)) w16(e, pad, (int16_t)saved);
}
