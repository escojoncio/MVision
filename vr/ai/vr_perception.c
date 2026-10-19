#include "vr_perception.h"
#include "vr_ai_scan.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

#define STALE_S 3.0f      /* una entrada sin evaluarse tanto tiempo ya no es del mismo soldado (muerto/recreado/dormido) */
#define SENTINEL 0x7F7F   /* valor imposible del campo de resultado: delata que la original no evaluó este fotograma */

/* ------------------------------------------------------------------------------------------------------ */
/* Ajuste                                                                                                  */
/* ------------------------------------------------------------------------------------------------------ */
void vrp_config_preset(VrpConfig *c, VrpPreset p) {
    memset(c, 0, sizeof *c);
    c->range_mult = 1.0f; c->rate_pow = 1.0f; c->periph_frac = 1.0f; c->periph_near = 0.5f; c->periph_move_rate = 1.0f;
    c->move_mult = c->crouch_mult = c->prone_mult = 1.0f; c->t_suspect = 1.0f; c->t_alert = 3.0f; c->decay = 1.0f;
    c->q_cooldown = 4.0f; c->dt_max = 0.5f; c->range_scale = 1.5f; c->head_dz = 80.0f; c->head_max = 1500.0f;
    switch (p) {
    case VRP_VR:   /* recomendado: se ve ~2,4 veces más lejos, de forma gradual */
        c->enabled = 1; c->range_mult = 2.4f; c->rate_pow = 1.0f; c->t_suspect = 0.7f; c->t_alert = 3.0f; c->decay = 0.7f;
        c->periph_frac = 0.6f; c->periph_near = 0.5f; c->periph_move_rate = 0.5f;
        c->move_mult = 1.4f; c->crouch_mult = 0.7f; c->prone_mult = 0.5f; c->q_cooldown = 4.0f; c->head_see = 1;
        break;
    case VRP_DEMANDING:   /* exigente: más lejos, más rápido, menos perdón */
        c->enabled = 1; c->range_mult = 3.2f; c->rate_pow = 0.8f; c->t_suspect = 0.4f; c->t_alert = 1.8f; c->decay = 0.4f;
        c->periph_frac = 0.75f; c->periph_near = 0.6f; c->periph_move_rate = 0.8f;
        c->move_mult = 1.6f; c->crouch_mult = 0.85f; c->prone_mult = 0.7f; c->q_cooldown = 3.0f; c->head_see = 1;
        break;
    case VRP_ORIGINAL: default: c->enabled = 0; c->range_scale = 1.0f; break;
    }
}

/* ------------------------------------------------------------------------------------------------------ */
/* Modelo                                                                                                  */
/* ------------------------------------------------------------------------------------------------------ */
float vrp_rate(const VrpConfig *c, float d, float L) {
    float lmax = L * c->range_mult;
    if (!(L > 0.0f) || !(lmax > L) || !(d > L) || d >= lmax) return 0.0f;
    float x = (lmax - d) / (lmax - L);
    if (x < 0.0f) x = 0.0f; else if (x > 1.0f) x = 1.0f;
    return powf(x, c->rate_pow > 0.0f ? c->rate_pow : 1.0f);
}

static float fmaxf_(float a, float b) { return a > b ? a : b; }

int vrp_step(const VrpConfig *c, VrpEntry *e, const VrpInput *in) {
    float dt = 0.0f;
    if (e->last_t >= 0.0f && in->now >= e->last_t) dt = fminf(in->now - e->last_t, c->dt_max);
    e->last_t = in->now;
    float d = in->dist, L = in->range;
    e->last_dist_q = (uint16_t)(d < 0 ? 0 : d > 65535.0f ? 65535 : d);
    int out;
    int vis = in->raw == 2;
    int periph = c->periph_frac < 1.0f && in->off_frac > c->periph_frac && d >= 500.0f && d > c->periph_near * L;
    e->seen_far = 0;
    if (!vis) {
        e->acc = fmaxf_(0.0f, e->acc - c->decay * dt);
        out = in->raw == 1 ? 1 : 0;
    } else if (periph && !in->moving) {
        e->acc = fmaxf_(0.0f, e->acc - c->decay * dt);   /* en la periferia, quieto: no lo registra */
        out = 1;
    } else if (d <= L) {
        e->acc = fmaxf_(e->acc, c->t_alert);             /* alcance original: alerta inmediata del juego */
        out = 2;
    } else {
        float r = vrp_rate(c, d, L);
        float sm = in->stance == 2 ? c->prone_mult : in->stance == 1 ? c->crouch_mult : 1.0f;
        r *= sm * (in->moving ? c->move_mult : 1.0f);
        if (periph) r *= c->periph_move_rate;
        e->acc = fminf(e->acc + r * dt, c->t_alert * 1.5f);
        e->seen_far = 1;
        out = e->acc >= c->t_alert ? 2 : 1;
    }
    if (vis) e->seen_t = in->now;
    if (e->acc < c->t_suspect * 0.5f) e->armed = 1;
    if (vis && out != 2 && e->seen_far && e->acc >= c->t_suspect && e->armed && in->now >= e->next_q) {
        e->q_pending = 1; e->armed = 0; e->next_q = in->now + c->q_cooldown;
    }
    if (out == 2) e->q_pending = 0;
    e->last_out = (uint8_t)out;
    return out;
}

/* ------------------------------------------------------------------------------------------------------ */
/* Localización                                                                                            */
/* ------------------------------------------------------------------------------------------------------ */
/* Variantes lógicas de la prueba de visión (forma de la función = hash de opcodes). Los offsets son los del
 * actor de cada familia. Se validan contra las instrucciones reales de la función antes de aceptarla. */
static const VrpLayout LAY_A = {"A", 2956, 2958, 2960, 2962, 3108, 3112};   /* soldados de las zonas principales */
static const VrpLayout LAY_B = {"B", 2952, 2954, 2956, 2958, 3112, 3116};   /* guardias de otras zonas */
static const VrpLayout LAY_C = {"C", 2832, 2834, 2836, 2838, 2940, 2944};   /* variante sin alcance global */
static const struct { const VrpLayout *lay; uint32_t nwords; uint64_t shape; } VARIANTS[] = {
    {&LAY_A, 135, 0x3d06215789928449ull}, {&LAY_A, 128, 0x4457c21f551fb129ull},
    {&LAY_B, 118, 0x2ad175654eebc83bull}, {&LAY_C, 107, 0xbaf5db53c3ccc898ull},
};

static int has_imm(const uint8_t *ram, uint32_t s, uint32_t e, uint32_t op, uint32_t imm) {
    for (uint32_t a = s; a < e; a += 4) { uint32_t w = vai_r32(ram, a); if ((w >> 26) == op && (w & 0xFFFF) == imm) return 1; }
    return 0;
}

/* addiu rX,rX,2000 precedido de lw rX,off(rX) y lui rX,hi -> dirección del alcance global. */
static uint32_t find_eye_global(const uint8_t *ram, uint32_t s, uint32_t e) {
    for (uint32_t a = s; a < e; a += 4) {
        uint32_t w = vai_r32(ram, a);
        if ((w >> 26) != 9 || (w & 0xFFFF) != 2000 || ((w >> 21) & 31) != ((w >> 16) & 31)) continue;
        uint32_t reg = (w >> 16) & 31;
        for (int k = 1; k <= 4 && a >= s + 4u * k; k++) {
            uint32_t l = vai_r32(ram, a - 4u * k);
            if ((l >> 26) != 0x23 || ((l >> 16) & 31) != reg || ((l >> 21) & 31) != reg) continue;
            for (int j = 1; j <= 4 && a - 4u * k >= s + 4u * j; j++) {
                uint32_t u = vai_r32(ram, a - 4u * k - 4u * j);
                if ((u >> 26) == 0xF && ((u >> 16) & 31) == reg)
                    return ((u & 0xFFFF) << 16) + (uint32_t)(int32_t)(int16_t)(l & 0xFFFF);
            }
        }
    }
    return 0;
}

static void find_gather(const uint8_t *ram, uint32_t lo, uint32_t hi, VrpSites *S) {
    uint32_t jv = vai_jal(S->vision);
    for (uint32_t a = lo; a + 28 < hi; a += 4) {
        if (vai_r32(ram, a) != jv) continue;
        for (int k = 1; k <= 6; k++) {
            uint32_t w = vai_r32(ram, a + 4u * k);
            if (!vai_is_jal(w)) continue;
            uint32_t g = vai_jal_target(w), gs = g, ge = g;
            if (g < lo || g + 8 >= hi || !vai_is_prologue(vai_r32(ram, g))) break;
            /* fin = primer `jr ra` + ranura de retardo (la función es corta y de una sola salida) */
            while (ge < hi && ge - gs < 160 && vai_r32(ram, ge) != 0x03E00008u) ge += 4;
            if (ge >= hi || ge - gs >= 160) break;
            ge += 8;
            /* sb $zero,off(base) = limpia el campo de eventos; luego lui+lw = modo global; addiu x,$zero,1 = valor "alerta" */
            uint32_t off = 0, mode = 0; int one = 0, seen_sb = 0;
            for (uint32_t p = gs; p < ge; p += 4) {
                uint32_t q = vai_r32(ram, p);
                if (!seen_sb && (q >> 26) == 0x28 && ((q >> 16) & 31) == 0) { off = q & 0xFFFF; seen_sb = 1; continue; }
                if (seen_sb && !mode && (q >> 26) == 0xF && p + 4 < ge) {
                    uint32_t l = vai_r32(ram, p + 4), reg = (q >> 16) & 31;
                    if ((l >> 26) == 0x23 && ((l >> 21) & 31) == reg) mode = ((q & 0xFFFF) << 16) + (uint32_t)(int32_t)(int16_t)(l & 0xFFFF);
                }
                if ((q >> 26) == 9 && ((q >> 21) & 31) == 0 && (q & 0xFFFF) == 1) one = 1;
            }
            if (seen_sb && mode && one && mode >= 0x800C0000u && mode < 0x80100000u) {
                S->gather = g; S->o_events = (uint16_t)off; S->mode_addr = mode;
            }
            return;
        }
    }
}

int vrp_locate(const uint8_t *ram, uint32_t lo, uint32_t hi, VrpSites *out) {
    memset(out, 0, sizeof *out);
    if (!ram || lo >= hi || (lo & 3) || (hi & 3) || lo < 0x80000000u || hi > 0x80000000u + VAI_RAM_SIZE) return 0;
    const uint32_t j_sub = vai_jal(VRP_FN_SUB3), j_dir = vai_jal(VRP_FN_DIR), j_len = vai_jal(VRP_FN_LEN);
    for (uint32_t a = lo; a + 40 < hi; a += 4) {
        if (vai_r32(ram, a) != j_sub) continue;
        int okd = 0, okl = 0;
        for (int k = 1; k <= 5; k++) okd |= vai_r32(ram, a + 4u * k) == j_dir;
        for (int k = 1; k <= 9; k++) okl |= vai_r32(ram, a + 4u * k) == j_len;
        if (!okd || !okl) continue;
        uint32_t s, e;
        if (!vai_func_bounds(ram, a, lo, hi, &s, &e)) continue;
        /* firma completa: ángulo respecto al eje + línea de visión contra geometría y blancos. Una cámara o un
         * proyector no la tienen (y no se tocan). */
        if (!vai_count_word(ram, s, e, vai_jal(VRP_FN_ADIFF)) || !vai_count_word(ram, s, e, vai_jal(VRP_FN_HAZARD)) ||
            !vai_count_word(ram, s, e, vai_jal(VRP_FN_TGTCHK)))
            continue;
        uint64_t sh = vai_shape_hash(ram, s, e);
        const VrpLayout *lay = NULL;
        for (unsigned v = 0; v < sizeof VARIANTS / sizeof *VARIANTS; v++)
            if (VARIANTS[v].nwords == (e - s) / 4 && VARIANTS[v].shape == sh) lay = VARIANTS[v].lay;
        if (!lay) continue;
        if (!has_imm(ram, s, e, 0x21, lay->o_len) || !has_imm(ram, s, e, 0x21, lay->o_facedir) ||
            !has_imm(ram, s, e, 0x21, lay->o_angle) || !has_imm(ram, s, e, 0x29, lay->o_flag) ||
            !has_imm(ram, s, e, 0x2B, lay->o_sndis) || !has_imm(ram, s, e, 0x2B, lay->o_sndir))
            continue;
        out->found = 1; out->vision = s; out->lay = lay; out->code_lo = lo; out->code_hi = hi;
        out->eye_global = find_eye_global(ram, s, e);
        find_gather(ram, lo, hi, out);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------------ */
/* Contexto y parches                                                                                      */
/* ------------------------------------------------------------------------------------------------------ */
void vrp_init(VrpCtx *x, uint8_t *ram, const VrpHost *host, VrpPreset p) {
    memset(x, 0, sizeof *x);
    x->ram = ram;
    if (host) x->host = *host;
    vrp_config_preset(&x->cfg, p);
    x->head_user = 1;
    vrp_reset(x);
}
void vrp_set_preset(VrpCtx *x, VrpPreset p) { float rs = x->cfg.range_scale; vrp_config_preset(&x->cfg, p); if (p != VRP_ORIGINAL && rs > 0.0f) x->cfg.range_scale = rs; }
void vrp_set_time(VrpCtx *x, float s) { x->now = s; }
void vrp_set_external_motion(VrpCtx *x, int m) { x->external_motion = m != 0; }
void vrp_reset(VrpCtx *x) {
    memset(x->tab, 0, sizeof x->tab);
    memset(&x->st, 0, sizeof x->st);
}

VrpEntry *vrp_entry(VrpCtx *x, uint32_t actor, int create) {
    if (!actor) return NULL;
    unsigned h = (actor >> 3) & (VRP_MAX_ENTRIES - 1), oldest = h;
    for (unsigned i = 0; i < VRP_MAX_ENTRIES; i++) {
        VrpEntry *e = &x->tab[(h + i) & (VRP_MAX_ENTRIES - 1)];
        if (e->actor == actor) return e;
    }
    if (!create) return NULL;
    float best = 1e30f;
    for (unsigned i = 0; i < VRP_MAX_ENTRIES; i++) {
        VrpEntry *e = &x->tab[(h + i) & (VRP_MAX_ENTRIES - 1)];
        if (!e->actor || x->now - e->last_t > 10.0f) { oldest = (h + i) & (VRP_MAX_ENTRIES - 1); best = -1; break; }
        if (e->last_t < best) { best = e->last_t; oldest = (h + i) & (VRP_MAX_ENTRIES - 1); }
    }
    VrpEntry *e = &x->tab[oldest];
    memset(e, 0, sizeof *e);
    e->actor = actor; e->last_t = -1.0f; e->armed = 1;
    return e;
}

static void call_orig(VrpCtx *x, uint32_t addr, uint32_t a0) {
    if (x->host.call_orig) x->host.call_orig(x->host.user, addr, a0);
}

/* Una pasada de la rutina ORIGINAL del motor con el alcance `lx` (y el punto del personaje sustituido por `pos` si no es NULL). Todo lo ampliado o
 * sustituido se restaura antes de volver (con el guardián del host impidiendo interrupciones en medio). Devuelve el resultado (0/1/2) o SENTINEL. */
typedef struct { int32_t dist, dir; } PassOut;
static int eval_pass(VrpCtx *x, uint32_t actor, int32_t len, int32_t lx, const int16_t *pos, PassOut *o) {
    const VrpSites *S = &x->sites;
    const VrpLayout *L = S->lay;
    uint8_t *ram = x->ram;
    const uint32_t vis_fn = S->vision, eye = S->eye_global;
    int32_t g = 0, gx = 0;
    if (eye) { g = (int32_t)vai_r32(ram, eye); gx = g < lx ? lx : g; }
    int16_t save[3] = {0, 0, 0};
    if (x->host.guard) x->host.guard(x->host.user, 1);
    if (pos) for (int i = 0; i < 3; i++) { save[i] = vai_r16(ram, VRP_A_PLAYER_POS + 2u * (uint32_t)i); vai_w16(ram, VRP_A_PLAYER_POS + 2u * (uint32_t)i, pos[i]); }
    vai_w16(ram, actor + L->o_len, (int16_t)lx);
    if (eye && gx != g) vai_w32(ram, eye, (uint32_t)gx);
    vai_w16(ram, actor + L->o_flag, SENTINEL);
    call_orig(x, vis_fn, actor);
    vai_w16(ram, actor + L->o_len, (int16_t)len);
    if (eye && gx != g) vai_w32(ram, eye, (uint32_t)g);
    if (pos) for (int i = 0; i < 3; i++) vai_w16(ram, VRP_A_PLAYER_POS + 2u * (uint32_t)i, save[i]);
    if (x->host.guard) x->host.guard(x->host.user, 0);
    o->dist = (int32_t)vai_r32(ram, actor + L->o_sndis);
    o->dir = (int32_t)vai_r32(ram, actor + L->o_sndir);
    return vai_r16(ram, actor + L->o_flag);
}

int vrp_head_point(const VrpConfig *c, const int16_t body[3], const float off[3], int16_t out[3]) {
    float n = sqrtf(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]);
    if (!(n > c->head_dz) || !isfinite(n)) return 0;
    float k = (n - c->head_dz) / n;                 /* continuo desde 0 en la zona muerta */
    float cap = c->head_max > 0 ? c->head_max : 1500.0f;
    if (n - c->head_dz > cap) k = cap / n;
    for (int i = 0; i < 3; i++) {
        float v = (float)body[i] + off[i] * k;
        out[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
    }
    return 1;
}

void vrp_head_offset(float theta_play_deg, float upm, const float p_ref[3], const float head[3], const float corr[3], float out[3]) {
    float th = (theta_play_deg + 180.0f) * (3.14159265f / 180.0f), cs = cosf(th), sn = sinf(th);
    float d[3];
    for (int i = 0; i < 3; i++) d[i] = head[i] + (corr ? corr[i] : 0.0f) - p_ref[i];
    if (!(upm > 1.0f)) upm = 1000.0f;
    out[0] = upm * (cs * d[0] + sn * d[2]);          /* R^T d, R = giro en Y (vr_headcol: W = eye + upm R^T (P - p_ref)) */
    out[1] = upm * d[1];
    out[2] = upm * (-sn * d[0] + cs * d[2]);
}

void vrp_set_head(VrpCtx *x, const float off[3]) {
    if (!off || !isfinite(off[0]) || !isfinite(off[1]) || !isfinite(off[2])) { x->head_valid = 0; return; }
    for (int i = 0; i < 3; i++) x->head_off[i] = off[i];
    x->head_valid = 1; x->head_t = x->now;
}
void vrp_set_head_enabled(VrpCtx *x, int on) { x->head_user = on != 0; }

void vrp_vision(VrpCtx *x, uint32_t actor) {
    const VrpSites *S = &x->sites;
    x->st.calls++;
    const VrpConfig *C = &x->cfg;
    float scale = C->range_scale > 0.0f ? C->range_scale : 1.0f;
    int ext = C->enabled && (C->range_mult > 1.0f || scale > 1.0f);
    int head_on = C->head_see && x->head_user && x->head_valid && x->now - x->head_t <= 0.25f && x->now >= x->head_t;
    if (!S->found || (!ext && !head_on)) { call_orig(x, S->vision, actor); return; }
    const VrpLayout *L = S->lay;
    uint8_t *ram = x->ram;
    int32_t len0 = vai_r16(ram, actor + L->o_len);
    if (len0 <= 0) { call_orig(x, S->vision, actor); return; }       /* ciego/mirando a otro lado: sin ampliar */
    /* Alcance de visión del soldado (el de su alerta inmediata): el original por el factor del menú. Zona de sospecha: range_mult sobre él. */
    int32_t len = (int32_t)((float)len0 * scale);
    if (len > 30000) len = 30000;
    if (len < len0) len = len0;
    int32_t lx = ext ? (int32_t)((float)len * (C->range_mult > 1.0f ? C->range_mult : 1.0f)) : len;
    if (lx > 30000) lx = 30000;
    if (lx < len) lx = len;
    int16_t prev_flag = vai_r16(ram, actor + L->o_flag);

    PassOut body, head;
    int raw = eval_pass(x, actor, len0, lx, NULL, &body);
    if (raw == SENTINEL) { vai_w16(ram, actor + L->o_flag, prev_flag); return; }   /* no le tocaba evaluar */
    if (raw < 0 || raw > 2) return;                                                /* valor ajeno: se respeta */
    int via = raw == 2 ? 1 : 0;
    PassOut *use = &body;
    if (head_on && raw < 2) {
        int16_t bp[3] = {vai_r16(ram, VRP_A_PLAYER_POS), vai_r16(ram, VRP_A_PLAYER_POS + 2), vai_r16(ram, VRP_A_PLAYER_POS + 4)}, hp[3];
        if (vrp_head_point(C, bp, x->head_off, hp)) {
            x->st.head_evals++;
            int rh = eval_pass(x, actor, len0, lx, hp, &head);
            if (rh >= 0 && rh <= 2 && (rh > raw || (rh == raw && rh > 0 && head.dist < body.dist))) {
                raw = rh; use = &head; if (rh == 2) via = 2;
            }
        }
    }
    /* Estado final = el de la pasada elegida (las escrituras de la original: distancia y rumbo al punto elegido; el resultado lo fija el modelo) */
    vai_w32(ram, actor + L->o_sndis, (uint32_t)use->dist);
    vai_w32(ram, actor + L->o_sndir, (uint32_t)use->dir);
    x->st.turns++;

    VrpEntry *e = vrp_entry(x, actor, 1);
    if (e->last_t >= 0.0f && (x->now < e->last_t || x->now - e->last_t > STALE_S)) {
        /* dirección reutilizada por otro actor, o reloj del host reiniciado: no se hereda nada */
        uint32_t a = e->actor;
        memset(e, 0, sizeof *e);
        e->actor = a; e->last_t = -1.0f; e->armed = 1;
    }
    VrpInput in;
    in.raw = raw;
    in.dist = (float)use->dist;
    in.range = (float)len;
    int32_t dir = use->dir, face = vai_r16(ram, actor + L->o_facedir);
    int32_t diff = (dir - face) & 4095;
    if (diff > 2048) diff = 4096 - diff;
    int32_t ang = vai_r16(ram, actor + L->o_angle);
    in.off_frac = ang > 0 ? (float)diff / (float)ang : 0.0f;
    uint32_t ps = vai_r32(ram, VRP_A_PLAYER_STATUS);
    in.moving = (ps & VRP_PS_MOVE) != 0 || x->external_motion;
    in.stance = (ps & VRP_PS_GROUND) ? 2 : (ps & VRP_PS_SQUAT) ? 1 : 0;
    in.now = x->now;
    uint8_t prev_out = e->last_out;
    int out = vrp_step(C, e, &in);
    e->via = (uint8_t)(raw == 2 ? via : 0);
    if (raw == 2) { if (via == 2) x->st.seen_head++; else x->st.seen_body++; }
    if (raw == 2 && in.dist > (float)len0) {
        x->st.extended_hits++;
        if (out == 2 && prev_out != 2) x->st.alerts_far++;
    }
    vai_w16(ram, actor + L->o_flag, (int16_t)out);
}

int vrp_log_tick(VrpCtx *x, char *buf, size_t n) {
    if (x->now < x->log_t) x->log_t = x->now - 1.0f;           /* reloj atrás */
    if (x->now - x->log_t < 1.0f) return 0;
    int nb = 0, nh = 0;
    for (unsigned i = 0; i < VRP_MAX_ENTRIES; i++) {
        const VrpEntry *e = &x->tab[i];
        if (!e->actor || !e->via || x->now < e->seen_t || x->now - e->seen_t > 1.0f) continue;
        if (e->via == 2) nh++; else nb++;
    }
    if (nb + nh == 0 && !x->log_active) return 0;
    x->log_t = x->now; x->log_active = nb + nh > 0;
    snprintf(buf, n, "[ia] ven=%d cuerpo=%d cabeza_vr=%d (cabeza fuera del cuerpo: %u evals)", nb + nh, nb, nh, x->st.head_evals);
    return 1;
}

void vrp_gather(VrpCtx *x, uint32_t actor) {
    const VrpSites *S = &x->sites;
    call_orig(x, S->gather, actor);
    if (!S->found || !S->gather || !x->cfg.enabled) return;
    VrpEntry *e = vrp_entry(x, actor, 0);
    if (!e || !e->q_pending) return;
    e->q_pending = 0;
    /* En modo alerta general los soldados ya buscan: la sospecha no aporta. El resto del tiempo, el bit de
     * "algo raro" hace que el soldado se vuelva hacia el jugador y mire (reacción propia del juego). */
    if ((int32_t)vai_r32(x->ram, S->mode_addr) == 1) return;
    vai_w8(x->ram, actor + S->o_events, (uint8_t)(vai_r8(x->ram, actor + S->o_events) | 2u));
    x->st.suspects++;
}
