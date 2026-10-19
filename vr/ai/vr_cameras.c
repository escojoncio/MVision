#include "vr_cameras.h"
#include "vr_ai_scan.h"
#include "vr_perception.h"   /* direcciones de funciones del EXE comunes */

#define VRC_FN_DIFFVEC 0x80016FA0u   /* distancia 3D entre dos puntos */

static const struct { VrcFamily fam; uint32_t nwords; uint64_t shape; uint16_t range, half; uint8_t w32; uint16_t det; } FAMS[] = {
    {VRC_FAM_CAMERA, 122, 0x342c01b9ef6f79cfull, 638, 640, 0, 476},
    {VRC_FAM_GUN,    105, 0x85657969aae40952ull, 868, 872, 1, 0},
    {VRC_FAM_SIMPLE,  62, 0xee97c3ee0ba786dfull, 842, 844, 0, 0},
};

static int has_imm(const uint8_t *ram, uint32_t s, uint32_t e, uint32_t op, uint32_t imm) {
    for (uint32_t a = s; a < e; a += 4) { uint32_t w = vai_r32(ram, a); if ((w >> 26) == op && (w & 0xFFFF) == imm) return 1; }
    return 0;
}

int vrc_locate(const uint8_t *ram, uint32_t lo, uint32_t hi, VrcSite *out, int max) {
    int n = 0;
    if (!ram || !out || max <= 0 || lo >= hi || (lo & 3) || (hi & 3) || lo < 0x80000000u || hi > 0x80000000u + VAI_RAM_SIZE) return 0;
    const uint32_t jd = vai_jal(VRP_FN_ADIFF);
    for (uint32_t a = lo; a + 4 < hi && n < max; a += 4) {
        if (vai_r32(ram, a) != jd) continue;
        uint32_t s, e;
        if (!vai_func_bounds(ram, a, lo, hi, &s, &e)) continue;
        if (n && out[n - 1].detect == s) continue;
        int dup = 0;
        for (int i = 0; i < n; i++) dup |= out[i].detect == s;
        if (dup) continue;
        /* firma: dos diferencias de ángulo + distancia 3D + línea de visión contra geometría; SIN línea de
         * visión contra blancos (eso es de los soldados) */
        if (vai_count_word(ram, s, e, jd) != 2 || !vai_count_word(ram, s, e, vai_jal(VRC_FN_DIFFVEC)) ||
            !vai_count_word(ram, s, e, vai_jal(VRP_FN_HAZARD)) || vai_count_word(ram, s, e, vai_jal(VRP_FN_TGTCHK)))
            continue;
        uint64_t sh = vai_shape_hash(ram, s, e);
        for (unsigned f = 0; f < sizeof FAMS / sizeof *FAMS; f++) {
            if (FAMS[f].nwords != (e - s) / 4 || FAMS[f].shape != sh) continue;
            if (!has_imm(ram, s, e, FAMS[f].w32 ? 0x23 : 0x25, FAMS[f].range) || !has_imm(ram, s, e, FAMS[f].w32 ? 0x23 : 0x25, FAMS[f].half)) break;
            out[n].family = FAMS[f].fam; out[n].detect = s; out[n].o_range = FAMS[f].range; out[n].o_half = FAMS[f].half;
            out[n].range_is_32 = FAMS[f].w32; out[n].o_detected = FAMS[f].det;
            n++;
            break;
        }
    }
    return n;
}

void vrc_read_params(const uint8_t *ram, const VrcSite *s, uint32_t actor, int32_t *range, int32_t *half) {
    if (!range || !half) return;
    *range = *half = 0;
    if (!ram || !s) return;
    if (s->range_is_32) {
        *range = (int32_t)vai_r32(ram, actor + s->o_range);
        *half = (int32_t)vai_r32(ram, actor + s->o_half);
    } else {
        *range = (uint16_t)vai_r16(ram, actor + s->o_range);
        *half = (uint16_t)vai_r16(ram, actor + s->o_half);
    }
}

int vrc_is_sensor_func(const VrcSite *s, int n, uint32_t addr) {
    if (!s) return 0;
    for (int i = 0; i < n; i++) if (s[i].detect == addr) return 1;
    return 0;
}

/* =====================================================================================================
 * PARTE 2: tabla de cámaras, daño, sonido, visibilidad (ver vr_cameras.h). Direcciones de la edición europea.
 * ===================================================================================================== */
#include <math.h>
#include <string.h>

#define VC_A_LISTS     0x800ADDA8u   /* 9 listas x 68 B: nodo inicio, nodo fin... */
#define VC_A_TARGETS   0x800B7660u   /* 64 blancos de 72 B */
#define VC_N_TARGETS   64u
#define VC_A_PLAYER_MAP 0x800ACB9Cu  /* máscara de mapas del jugador */
#define VC_A_PLAYER_POS 0x800ACBA0u  /* 3 x s16 */
#define VC_A_GAME_STATUS 0x800AC570u
#define VC_A_CAMERA    0x800B8968u   /* cámara del juego: +0x22 s16 primera persona */
#define VC_A_VIEW      0x800B2B7Cu   /* canal 3D principal: +0x30 matriz vista->mundo (t en +0x14 de ella) */
#define VC_OV_LO       0x800C4380u   /* ventana de código de overlays de zona */
#define VC_OV_HI       0x80100000u
#define VC_STATUS_MUTE 0x84000000u   /* fin de partida | demo: el juego no reproduce sonidos */

typedef struct VcLay {
    VrcFamily fam; uint32_t nwords; uint64_t shape;
    uint16_t o_target, o_dead, o_active, o_sound, o_mode, o_proc, o_range, o_half, o_turnmax, o_center;
    uint8_t dead32, active32, sound32, mode32, w32;
} VcLay;

/* Offsets dentro del bloque de trabajo (verificados sobre las funciones de las overlays europeas):
 *  CAMERA: blanco +0x1CC, contador de muerte +0x1F0, activa s16 +0x284, sonido s16 +0x28A, modo +0x1E0,
 *          script +0x28C, alcance u16 +0x27E, semiángulo u16 +0x280, giro máx u16 +0x282, rumbo central +0x1C0.
 *  SIMPLE: blanco +0x2A0, contador de muerte +0x2B8, activa s32 +0x2BC, modo(pensar) +0x2A8, alcance +0x34A, semi +0x34C.
 *  GUN:    blanco +0x1E0, contador de muerte +0x35C, sonido s32 +0x404, alcance s32 +0x364, semi s32 +0x368, script +0x3D4. */
static const VcLay LAY[] = {
    {VRC_FAM_CAMERA, 144, 0xcdf9127c94b1bbefull, 0x1CC, 0x1F0, 0x284, 0x28A, 0x1E0, 0x28C, 0x27E, 0x280, 0x282, 0x1C0, 1, 0, 0, 1, 0},
    {VRC_FAM_SIMPLE, 240, 0xfb8c827bc70301daull, 0x2A0, 0x2B8, 0x2BC, 0,     0x2A8, 0,     0x34A, 0x34C, 0x34E, 0x278, 1, 1, 0, 1, 0},
    {VRC_FAM_GUN,    551, 0x14f167f40b4beadfull, 0x1E0, 0x35C, 0,     0x404, 0,     0x3D4, 0x364, 0x368, 0,     0,     1, 0, 1, 1, 1},
};
#define N_LAY ((int)(sizeof LAY / sizeof LAY[0]))

static int pro_at(const uint8_t *ram, uint32_t a) { return vai_is_prologue(vai_r32(ram, a)); }

/* Forma (hash de opcodes) de la función de actualización: empieza en `act` (puede haber 1-2 instrucciones antes
 * del prólogo) y acaba en el siguiente prólogo, sin la cola de ceros. 0 si no tiene aspecto de función. */
static int act_shape(const uint8_t *ram, uint32_t act, uint32_t *nwords, uint64_t *hash) {
    if (act < VC_OV_LO || act >= VC_OV_HI || (act & 3)) return 0;
    uint32_t p = act, lim = act + 32;
    while (p < lim && !pro_at(ram, p)) p += 4;
    if (p >= lim) return 0;
    uint32_t e = p + 4, top = act + 4 * 2048;
    if (top > 0x80000000u + VAI_RAM_SIZE) top = 0x80000000u + VAI_RAM_SIZE;
    while (e < top && !pro_at(ram, e)) e += 4;
    if (e >= top) return 0;
    while (e > p + 4 && vai_r32(ram, e - 4) == 0) e -= 4;
    *nwords = (e - act) / 4;
    *hash = vai_shape_hash(ram, act, e);
    return 1;
}

static VcLay g_extra[4];
static int g_nextra;
int vrc_add_signature(VrcFamily fam, uint32_t nwords, uint64_t shape) {
    if (g_nextra >= 4 || nwords == 0) return 0;
    for (int i = 0; i < N_LAY; i++) if (LAY[i].fam == fam) { g_extra[g_nextra] = LAY[i]; break; }
    if (g_extra[g_nextra].nwords == 0) return 0;
    g_extra[g_nextra].nwords = nwords; g_extra[g_nextra].shape = shape;
    g_nextra++;
    return 1;
}
static const VcLay *lay_of(const uint8_t *ram, uint32_t act) {
    uint32_t n; uint64_t h;
    if (!act_shape(ram, act, &n, &h)) return NULL;
    for (int i = 0; i < N_LAY; i++) if (LAY[i].nwords == n && LAY[i].shape == h) return &LAY[i];
    for (int i = 0; i < g_nextra; i++) if (g_extra[i].nwords == n && g_extra[i].shape == h) return &g_extra[i];
    return NULL;
}

static int ptr_ok(uint32_t a, uint32_t len) {
    return a >= 0x80010000u && (a & 3) == 0 && a + len <= 0x80000000u + VAI_RAM_SIZE && a + len > a;
}
static int tgt_ok(uint32_t t) { return t >= VC_A_TARGETS && t < VC_A_TARGETS + 72u * VC_N_TARGETS && (t - VC_A_TARGETS) % 72u == 0; }

static int32_t rd_dyn(const uint8_t *ram, uint32_t a, int w32, int sgn) {
    if (w32) return (int32_t)vai_r32(ram, a);
    int16_t v = vai_r16(ram, a);
    return sgn ? v : (int32_t)(uint16_t)v;
}

/* ---- Configuración y estado interno ---- */
static VrcConfig g_cfg;
static int g_cfg_init;
static VrcSoundCfg g_snd;
static int g_snd_init;
static VrcTable g_tab;
static int g_tab_valid;
static struct { int32_t pos[3]; int yaw; int valid; } g_listener;

void vrc_config_default(VrcConfig *c) {
    memset(c, 0, sizeof *c);
    c->bullets_destroy = 1; c->bullet_hits = 2; c->ignore_class = 1; c->pickup_game_bullets = 1;
    c->cosmetic_sweep = 0; c->sweep_max_dist = 0;
}
void vrc_sound_default(VrcSoundCfg *c) {
    memset(c, 0, sizeof *c);
    c->mode = VRC_SND_NATURAL; c->ref_dist = 1500.f; c->rolloff = 1.f; c->max_dist = 30000.f; c->fade_frac = 0.25f;
    c->gain = 1.f; c->min_vol = 16; c->n_se = 3; c->se[0] = 94; c->se[1] = 95; c->se[2] = 109;
}
static void ensure_init(void) {
    if (!g_cfg_init) { vrc_config_default(&g_cfg); g_cfg_init = 1; }
    if (!g_snd_init) { vrc_sound_default(&g_snd); g_snd_init = 1; }
}
void vrc_config_get(VrcConfig *c) { ensure_init(); *c = g_cfg; }
void vrc_config_set(const VrcConfig *c) {
    ensure_init(); g_cfg = *c;
    if (g_cfg.bullet_hits < 1) g_cfg.bullet_hits = 1;
    if (g_cfg.sweep_max_dist < 0) g_cfg.sweep_max_dist = 0;
}
void vrc_sound_get(VrcSoundCfg *c) { ensure_init(); *c = g_snd; }
void vrc_sound_set(const VrcSoundCfg *c) {
    ensure_init(); g_snd = *c;
    if (!(g_snd.ref_dist >= 1.f)) g_snd.ref_dist = 1.f;
    if (!(g_snd.rolloff >= 0.f)) g_snd.rolloff = 0.f;
    if (!(g_snd.max_dist > g_snd.ref_dist)) g_snd.max_dist = g_snd.ref_dist + 1.f;
    if (!(g_snd.fade_frac >= 0.f)) g_snd.fade_frac = 0.f;
    if (g_snd.fade_frac > 1.f) g_snd.fade_frac = 1.f;
    if (!(g_snd.gain >= 0.f)) g_snd.gain = 0.f;
    if (g_snd.min_vol < 0) g_snd.min_vol = 0;
    if (g_snd.min_vol > 63) g_snd.min_vol = 63;
    if (g_snd.n_se < 0) g_snd.n_se = 0;
    if (g_snd.n_se > 8) g_snd.n_se = 8;
}
void vrc_set_listener(const int32_t pos[3], int yaw, int valid) {
    g_listener.valid = valid && pos;
    if (g_listener.valid) { for (int i = 0; i < 3; i++) g_listener.pos[i] = pos[i]; g_listener.yaw = yaw & 4095; }
}

/* ---- Memoria por actor (daño acumulado, barrido cosmético, transiciones) ---- */
typedef struct { uint32_t actor; int32_t hits; int8_t prev_status; int8_t dir; int16_t wait; uint32_t alert_frame; uint8_t swept; int16_t turn0; } VcMem;
static VcMem g_mem[VRC_MAX_CAMERAS * 2];
static VrcSoundEvent g_ev[VRC_MAX_SND_EVENTS];
static int g_ev_n;
static uint32_t g_ev_seq, g_frame;

static VcMem *mem_get(uint32_t actor, int create) {
    VcMem *free_slot = NULL;
    for (unsigned i = 0; i < sizeof g_mem / sizeof *g_mem; i++) {
        if (g_mem[i].actor == actor) return &g_mem[i];
        if (!g_mem[i].actor && !free_slot) free_slot = &g_mem[i];
    }
    if (!create || !free_slot) return NULL;
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->actor = actor; free_slot->prev_status = -1; free_slot->dir = 1;
    return free_slot;
}

void vrc_reset(void) {
    memset(g_mem, 0, sizeof g_mem);
    memset(g_ev, 0, sizeof g_ev);
    g_ev_n = 0; g_tab_valid = 0;
    memset(&g_tab, 0, sizeof g_tab);
}
void vrc_reset_all(void) {
    vrc_reset();
    g_nextra = 0; memset(g_extra, 0, sizeof g_extra);
    g_ev_seq = 0; g_frame = 0;
    memset(&g_listener, 0, sizeof g_listener);
    g_cfg_init = g_snd_init = 0;
}

/* ---- Exploración de las listas de actores ---- */
static void read_cam(const uint8_t *ram, uint32_t actor, const VcLay *L, uint32_t frame, VrcCamera *c) {
    memset(c, 0, sizeof *c);
    c->actor = actor; c->family = L->fam; c->act = vai_r32(ram, actor + 8); c->control = actor + 0x20;
    for (int i = 0; i < 3; i++) { c->pos[i] = vai_r16(ram, actor + 0x20 + 2u * i); c->rot[i] = vai_r16(ram, actor + 0x28 + 2u * i); }
    uint32_t mp = vai_r32(ram, actor + 0x20 + 0x2c);
    c->map_bit = ptr_ok(mp, 4) ? vai_r32(ram, mp) : 0;
    uint32_t pm = vai_r32(ram, VC_A_PLAYER_MAP);
    c->in_player_map = (c->map_bit & pm) != 0;
    c->range = rd_dyn(ram, actor + L->o_range, L->w32, 0);
    c->half = rd_dyn(ram, actor + L->o_half, L->w32, 0);
    uint32_t tg = vai_r32(ram, actor + L->o_target);
    c->target = tgt_ok(tg) ? tg : 0;
    if (c->target) for (int i = 0; i < 3; i++) { c->box_center[i] = vai_r16(ram, tg + 8 + 2u * i); c->box_half[i] = vai_r16(ram, tg + 0x10 + 2u * i); }
    int32_t dead = rd_dyn(ram, actor + L->o_dead, L->dead32, 1);
    c->alive = dead == 0;
    c->active = L->o_active ? (rd_dyn(ram, actor + L->o_active, L->active32, 1) != 0) : 1;
    c->sound_on = L->o_sound ? (rd_dyn(ram, actor + L->o_sound, L->sound32, 1) != 0) : 1;
    c->proc = L->o_proc ? (int32_t)vai_r32(ram, actor + L->o_proc) : -1;
    int32_t mode = L->o_mode ? rd_dyn(ram, actor + L->o_mode, L->mode32, 1) : 0;
    if (!c->alive) c->status = VRC_ST_DYING;
    else if (!c->active) c->status = VRC_ST_INACTIVE;
    else c->status = (mode >= 0 && mode <= 3) ? (VrcStatus)mode : VRC_ST_PATROL;
    VcMem *m = mem_get(actor, 1);
    if (m) {
        c->hits = m->hits;
        if (m->prev_status != (int8_t)c->status && c->status == VRC_ST_ALERT) m->alert_frame = frame ? frame : 1;
        m->prev_status = (int8_t)c->status;
        c->alert_frame = m->alert_frame;
    }
}

int vrc_classify_actor(const uint8_t *ram, uint32_t actor) {
    if (!ram || !ptr_ok(actor, 0x20)) return -1;
    const VcLay *L = lay_of(ram, vai_r32(ram, actor + 8));
    return L ? (int)L->fam : -1;
}

int vrc_scan(const uint8_t *ram, VrcTable *t) {
    if (!t) return 0;
    uint32_t frame = t->frame;
    memset(t, 0, sizeof *t);
    t->frame = frame;
    if (!ram) return 0;
    uint32_t memo_act[48]; const VcLay *memo_lay[48]; int nmemo = 0;
    for (int lv = 0; lv < 9; lv++) {
        uint32_t base = VC_A_LISTS + 68u * (uint32_t)lv, endn = base + 32u;
        if (vai_r32(ram, base) != 0 || vai_r32(ram, endn + 4) != 0 || vai_r32(ram, endn + 8) != 0) continue;
        uint32_t cur = vai_r32(ram, base + 4), prev = base;
        for (int steps = 0; cur != endn; steps++) {
            if (steps > 1024 || !ptr_ok(cur, 32) || vai_r32(ram, cur) != prev) break;
            uint32_t act = vai_r32(ram, cur + 8);
            if (act >= VC_OV_LO && act < VC_OV_HI) {
                const VcLay *L = NULL; int k;
                for (k = 0; k < nmemo; k++) if (memo_act[k] == act) { L = memo_lay[k]; break; }
                if (k == nmemo) {
                    L = lay_of(ram, act);
                    if (nmemo < 48) { memo_act[nmemo] = act; memo_lay[nmemo] = L; nmemo++; }
                }
                if (L && t->n < VRC_MAX_CAMERAS && ptr_ok(cur + 0x20, 0x80)) read_cam(ram, cur, L, t->frame, &t->cam[t->n++]);
            }
            prev = cur; cur = vai_r32(ram, cur + 4);
        }
    }
    return t->n;
}

const VrcCamera *vrc_find(const VrcTable *t, uint32_t actor) {
    if (!t) return NULL;
    for (int i = 0; i < t->n && i < VRC_MAX_CAMERAS; i++) if (t->cam[i].actor == actor) return &t->cam[i];
    return NULL;
}

/* ---- Geometría ---- */
#define PI_F 3.14159265358979f
static float ang4096(float x, float z) { return atan2f(x, z) * (4096.f / (2.f * PI_F)); }
static int diff_abs(float a, float b) {
    int d = ((int)lrintf(a) - (int)lrintf(b)) & 4095;
    return d > 2048 ? 4096 - d : d;
}
/* Rumbo y cabeceo al estilo del juego: rumbo = atan2(dx,dz); cabeceo = atan2(distancia horizontal, dy) - 1024. */
static void dir_to(const int16_t from[3], const int16_t to[3], float *yaw, float *pitch) {
    float dx = (float)to[0] - from[0], dy = (float)to[1] - from[1], dz = (float)to[2] - from[2];
    *yaw = ang4096(dx, dz);
    *pitch = ang4096(sqrtf(dx * dx + dz * dz), dy) - 1024.f;
}
int vrc_in_cone(const VrcCamera *c, const int16_t pt[3]) {
    if (!c || !pt) return 0;
    float yaw, pitch;
    dir_to(c->pos, pt, &yaw, &pitch);
    int dy = diff_abs(yaw, (float)c->rot[1]), dx = diff_abs(pitch, (float)c->rot[0]);
    if (dx > c->half || dy > c->half) return 0;
    float ex = (float)pt[0] - c->pos[0], ey = (float)pt[1] - c->pos[1], ez = (float)pt[2] - c->pos[2];
    return sqrtf(ex * ex + ey * ey + ez * ez) <= (float)c->range;
}
void vrc_forward(const VrcCamera *c, float out[3]) {
    float yaw = (float)c->rot[1] * (2.f * PI_F / 4096.f), pit = (float)c->rot[0] * (2.f * PI_F / 4096.f);
    out[0] = sinf(yaw) * cosf(pit); out[1] = -sinf(pit); out[2] = cosf(yaw) * cosf(pit);
}

/* ---- Daño ---- */
static int apply_power_hit(uint8_t *ram, uint32_t tg, int damage) {
    /* Efecto sobre la víctima de la función de daño del juego: bit de daño, modo de arma 2 (explosivo), daño. */
    vai_w16(ram, tg + 6, (int16_t)(vai_r16(ram, tg + 6) | 4));
    vai_w16(ram, tg + 0x3e, 2);
    int64_t d = (int64_t)vai_r16(ram, tg + 0x28) + damage;
    vai_w16(ram, tg + 0x28, (int16_t)(d > 32767 ? 32767 : d));
    return 1;
}

int vr_cameras_hit_ex(uint8_t *ram, uint32_t actor, const int16_t punto[3], int damage, VrcDamageKind kind) {
    (void)punto;
    ensure_init();
    if (!ram || damage <= 0 || !ptr_ok(actor, 0x20)) return VRC_HIT_BAD;
    if (kind != VRC_DMG_BULLET && kind != VRC_DMG_EXPLOSIVE) return VRC_HIT_BAD;
    const VcLay *L = lay_of(ram, vai_r32(ram, actor + 8));
    if (!L) return VRC_HIT_BAD;
    uint32_t tg = vai_r32(ram, actor + L->o_target);
    if (!tgt_ok(tg)) return VRC_HIT_BAD;
    if (rd_dyn(ram, actor + L->o_dead, L->dead32, 1) != 0) return VRC_HIT_IGNORED;   /* ya muriendo */
    uint16_t cls = (uint16_t)vai_r16(ram, tg);
    if (cls == 0) return VRC_HIT_IGNORED;                                             /* blanco liberado (clase 0) */
    if (!(cls & 4) && !g_cfg.ignore_class) return VRC_HIT_IGNORED;                    /* no dañable en el original */
    if (kind == VRC_DMG_EXPLOSIVE) {
        apply_power_hit(ram, tg, damage);
        VcMem *m = mem_get(actor, 1); if (m) m->hits = g_cfg.bullet_hits;
        return VRC_HIT_DESTROYED;
    }
    if (!g_cfg.bullets_destroy) return VRC_HIT_IGNORED;
    VcMem *m = mem_get(actor, 1);
    if (!m) return VRC_HIT_BAD;
    if (m->hits < 0x3fffffff) m->hits++;                       /* un impacto = un golpe, sea cual sea el daño */
    if (m->hits >= g_cfg.bullet_hits) { apply_power_hit(ram, tg, damage); return VRC_HIT_DESTROYED; }
    return VRC_HIT_DAMAGED;
}
int vr_cameras_hit(uint8_t *ram, uint32_t actor, const int16_t punto[3], int damage) {
    return vr_cameras_hit_ex(ram, actor, punto, damage, VRC_DMG_BULLET);
}

int vrc_raycast(const VrcTable *t, const float o[3], const float d[3], float max_len, float *t_out, int16_t hit[3]) {
    if (!t || !o || !d || !(max_len > 0.f)) return -1;
    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 1e-6f)) return -1;
    if (!(fabsf(o[0]) < 1e9f && fabsf(o[1]) < 1e9f && fabsf(o[2]) < 1e9f)) return -1;
    float dn[3] = {d[0] / len, d[1] / len, d[2] / len}, best = max_len; int bi = -1;
    for (int i = 0; i < t->n && i < VRC_MAX_CAMERAS; i++) {
        const VrcCamera *c = &t->cam[i];
        if (!c->alive || !c->target) continue;
        float t0 = 0.f, t1 = best; int ok = 1;
        for (int k = 0; k < 3 && ok; k++) {
            float lo = (float)c->box_center[k] - c->box_half[k], hi = (float)c->box_center[k] + c->box_half[k];
            if (fabsf(dn[k]) < 1e-9f) { if (o[k] < lo || o[k] > hi) ok = 0; continue; }
            float a = (lo - o[k]) / dn[k], b = (hi - o[k]) / dn[k];
            if (a > b) { float x = a; a = b; b = x; }
            if (a > t0) t0 = a;
            if (b < t1) t1 = b;
            if (t0 > t1) ok = 0;
        }
        if (ok && t0 <= best) { best = t0; bi = i; }
    }
    if (bi >= 0) {
        if (t_out) *t_out = best;
        if (hit) for (int k = 0; k < 3; k++) { long v = lrintf(o[k] + dn[k] * best); hit[k] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }
    }
    return bi;
}

/* ---- Oyente ---- */
typedef struct { float pos[3]; int yaw; } Ear;
static Ear get_ear(const uint8_t *ram) {
    Ear e = {{0, 0, 0}, 0};
    if (g_listener.valid) { for (int i = 0; i < 3; i++) e.pos[i] = (float)g_listener.pos[i]; e.yaw = g_listener.yaw; return e; }
    int first = vai_r16(ram, VC_A_CAMERA + 0x22) != 0;
    if (first) {
        uint32_t m = VC_A_VIEW + 0x30;
        for (int i = 0; i < 3; i++) e.pos[i] = (float)(int32_t)vai_r32(ram, m + 0x14 + 4u * i);
        e.yaw = (int)lrintf(ang4096((float)vai_r16(ram, m + 4), (float)vai_r16(ram, m + 16)));
    } else {
        for (int i = 0; i < 3; i++) e.pos[i] = (float)vai_r16(ram, VC_A_PLAYER_POS + 2u * i);
        e.yaw = 0;
    }
    return e;
}

/* ---- Sonido ---- */
static int orig_vol(float dist) {
    if (!(dist < 1e6f)) dist = 1e6f;
    int d = (int)dist;
    if (d < 0) d = 0;
    return d < 9500 ? 63 - d / 150 : 0;
}

int vrc_sound_curve(const VrcSoundCfg *c, float dist, float *gain_out) {
    float g;
    if (!(dist >= 0.f)) dist = 0.f;
    if (c->mode == VRC_SND_ORIGINAL) {
        int v = orig_vol(dist);
        g = (float)v / 63.f;
        if (gain_out) *gain_out = g;
        return v;
    }
    if (dist >= c->max_dist) g = 0.f;
    else {
        g = dist <= c->ref_dist ? 1.f : powf(c->ref_dist / dist, c->rolloff);
        float f0 = c->max_dist * (1.f - c->fade_frac);
        if (c->fade_frac > 0.f && dist > f0) g *= (c->max_dist - dist) / (c->max_dist - f0);
        g *= c->gain;
    }
    /* Nunca más bajo que el original: la curva natural cae por debajo de la del juego a media distancia. */
    { float go = (float)orig_vol(dist) / 63.f; if (go > g) g = go; }
    if (g > 1.f) g = 1.f;
    if (!(g >= 0.f)) g = 0.f;
    if (gain_out) *gain_out = g;
    int v = (int)lrintf(63.f * g);
    if (g > 0.f && v < c->min_vol) v = c->min_vol;
    if (v > 63) v = 63;
    return v;
}

static int rcos_i(int a) { return (int)lrintf(4096.f * cosf((float)(a & 4095) * (2.f * PI_F / 4096.f))); }
/* Paneo del juego (primera persona): rcos(ángulo)*escala/4096, con los negativos mapeados a 255-|p|. */
static int pan_orig(float dist, int angle) {
    if (!(dist < 1e6f)) dist = 1e6f;
    int d = (int)dist; if (d < 0) d = 0;
    if (d > 9500) d = 9500;
    d *= 4; if (d > 9500) d = 9500;
    int scale = (d * 31) / 9500;
    int pan = rcos_i(angle) * scale / 4096;
    if (pan < 0) pan += 255;
    return pan & 0xff;
}

static int is_cam_se(uint32_t se) {
    for (int i = 0; i < g_snd.n_se; i++) if (g_snd.se[i] == se) return 1;
    return 0;
}

static void push_event(const VrcSoundEvent *e) {
    if (g_ev_n == VRC_MAX_SND_EVENTS) { memmove(g_ev, g_ev + 1, (VRC_MAX_SND_EVENTS - 1) * sizeof *g_ev); g_ev_n--; }
    g_ev[g_ev_n] = *e; g_ev[g_ev_n].seq = ++g_ev_seq; g_ev_n++;
}

int vrc_sound_events(VrcSoundEvent *out, int max) {
    int n = g_ev_n < max ? g_ev_n : max;
    if (n < 0) n = 0;
    if (out) memcpy(out, g_ev, (size_t)n * sizeof *g_ev);
    memmove(g_ev, g_ev + n, (size_t)(g_ev_n - n) * sizeof *g_ev);
    g_ev_n -= n;
    return n;
}

int vrc_se_intercept(const uint8_t *ram, uint32_t pos_ptr, uint32_t se, uint32_t *cmd) {
    ensure_init();
    se &= 0xff;
    if (!ram || !pos_ptr || !is_cam_se(se) || !ptr_ok(pos_ptr & ~3u, 8)) return VRC_SE_PASS;
    VrcSoundEvent ev; memset(&ev, 0, sizeof ev);
    ev.frame = g_frame; ev.se = (uint16_t)se; ev.cam_index = -1;
    for (int i = 0; i < 3; i++) ev.pos[i] = vai_r16(ram, pos_ptr + 2u * i);
    /* asociar con la cámara cuyo origen coincide con la posición del sonido */
    if (g_tab_valid) for (int i = 0; i < g_tab.n; i++) {
        const VrcCamera *c = &g_tab.cam[i];
        if (c->pos[0] == ev.pos[0] && c->pos[1] == ev.pos[1] && c->pos[2] == ev.pos[2]) { ev.cam_index = i; ev.actor = c->actor; break; }
    }
    Ear e = get_ear(ram);
    float dx = (float)ev.pos[0] - e.pos[0], dy = (float)ev.pos[1] - e.pos[1], dz = (float)ev.pos[2] - e.pos[2];
    ev.dist = sqrtf(dx * dx + dy * dy + dz * dz);
    int pan = pan_orig(ev.dist, (int)lrintf(ang4096(dx, dz)) + 1024 - e.yaw);
    ev.vol_orig = (uint8_t)orig_vol(ev.dist);
    ev.pan = (uint8_t)pan;
    int muted = (vai_r32(ram, VC_A_GAME_STATUS) & VC_STATUS_MUTE) != 0;
    if (g_snd.mode == VRC_SND_ORIGINAL) {
        VrcSoundCfg oc = g_snd; oc.mode = VRC_SND_ORIGINAL;
        ev.vol = (uint8_t)vrc_sound_curve(&oc, ev.dist, &ev.gain);
        ev.played = !muted && ev.vol > 0;
        push_event(&ev);
        return VRC_SE_PASS;
    }
    ev.vol = (uint8_t)vrc_sound_curve(&g_snd, ev.dist, &ev.gain);
    if (muted || ev.vol == 0) { ev.vol = muted ? 0 : ev.vol; ev.played = 0; push_event(&ev); return VRC_SE_DROP; }
    ev.played = 1;
    push_event(&ev);
    if (cmd) *cmd = ((uint32_t)pan << 16) | ((uint32_t)ev.vol << 8) | se;
    return VRC_SE_PLAY;
}

/* ---- Fotograma ---- */
const VrcTable *vrc_table(void) { return g_tab_valid ? &g_tab : NULL; }

static void sweep_cosmetic(uint8_t *ram, const VrcCamera *c, VcMem *m, const Ear *ear) {
    const VcLay *L = &LAY[0];                                 /* solo la familia con mensajes de entrada/salida */
    uint32_t turn_y = c->actor + 0x6c + 2;                   /* giro objetivo (rumbo) del actor */
    if (!m) return;
    /* fuera de condiciones (activa, muriendo, otra familia, oyente lejos): devuelve el giro que tenía el juego
     * para que la detección original no vea un rumbo alterado */
    int far_ = 0;
    if (g_cfg.sweep_max_dist > 0) {
        float dx = (float)c->pos[0] - ear->pos[0], dy = (float)c->pos[1] - ear->pos[1], dz = (float)c->pos[2] - ear->pos[2];
        far_ = sqrtf(dx * dx + dy * dy + dz * dz) > (float)g_cfg.sweep_max_dist;
    }
    if (c->family != VRC_FAM_CAMERA || !c->alive || c->active || far_) {
        if (m->swept && c->family == VRC_FAM_CAMERA && c->alive) vai_w16(ram, turn_y, m->turn0);
        m->swept = 0;
        return;
    }
    int maxturn = (uint16_t)vai_r16(ram, c->actor + L->o_turnmax);
    if (maxturn < 8 || maxturn > 1024) return;
    int center = vai_r16(ram, c->actor + L->o_center + 2) & 4095;
    if (!m->swept) { m->swept = 1; m->wait = 60; m->dir = 1; m->turn0 = vai_r16(ram, turn_y); }
    if (m->wait > 0) { m->wait--; return; }
    int ty = ((vai_r16(ram, turn_y) & 4095) + 8 * m->dir) & 4095;
    int d = (ty - center) & 4095; if (d > 2048) d = 4096 - d;
    vai_w16(ram, turn_y, (int16_t)ty);
    if (d >= maxturn) { m->dir = (int8_t)-m->dir; m->wait = 60; }
}

int vrc_frame(uint8_t *ram, VrcTable *t) {
    ensure_init();
    if (!ram) return 0;
    VrcTable *tab = t ? t : &g_tab;
    g_frame++;
    tab->frame = g_frame;
    vrc_scan(ram, tab);
    Ear ear = get_ear(ram);
    for (int i = 0; i < tab->n; i++) {
        VrcCamera *c = &tab->cam[i];
        VcMem *m = mem_get(c->actor, 1);
        if (g_cfg.pickup_game_bullets && c->alive && c->target) {
            uint16_t dm = (uint16_t)vai_r16(ram, c->target + 6);
            if (vai_r16(ram, c->target) != 0 && (dm & 4) && vai_r16(ram, c->target + 0x3e) == 1) {      /* impacto de bala del juego (modo 1) */
                if (g_cfg.bullets_destroy && m) {
                    if (m->hits < 0x3fffffff) m->hits++;                /* un impacto = un golpe */
                    if (m->hits >= g_cfg.bullet_hits) vai_w16(ram, c->target + 0x3e, 2);   /* la rutina de la cámara hace el resto */
                    else { vai_w16(ram, c->target + 6, (int16_t)(dm & ~4)); vai_w16(ram, c->target + 0x28, 0); }
                }
                c->hits = m ? m->hits : 0;
            }
        }
        if (g_cfg.cosmetic_sweep) sweep_cosmetic(ram, c, m, &ear);
        else if (m && m->swept) { if (c->alive && c->family == VRC_FAM_CAMERA) vai_w16(ram, c->actor + 0x6c + 2, m->turn0); m->swept = 0; }
    }
    /* olvida el estado de actores que ya no existen */
    for (unsigned k = 0; k < sizeof g_mem / sizeof *g_mem; k++)
        if (g_mem[k].actor && !vrc_find(tab, g_mem[k].actor)) memset(&g_mem[k], 0, sizeof g_mem[k]);
    if (tab != &g_tab) memcpy(&g_tab, tab, sizeof g_tab);
    g_tab_valid = 1;
    return tab->n;
}
