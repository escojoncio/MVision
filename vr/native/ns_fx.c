/* ns_fx: extracción de efectos del motor desde la RAM y clasificación (candidata) de la lista de la GPU. Formato y semántica: docs/efectos_nativos.md. */
#include "ns_fx.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------------------------------ acceso a la RAM */
#define RAMMASK 0x1FFFFFu
static int in_ram(uint32_t a, uint32_t n) { uint32_t o = a & RAMMASK; return (uint64_t)o + n <= NSFX_RAM_SIZE; }
static int ptr_ok(uint32_t p) { uint32_t seg = p & 0xFFE00000u; return seg == 0x80000000u || seg == 0xA0000000u; }
static uint32_t rd8(const uint8_t *m, uint32_t a) { return in_ram(a, 1) ? m[a & RAMMASK] : 0; }
static uint32_t rdu16(const uint8_t *m, uint32_t a) { return in_ram(a, 2) ? (uint32_t)(m[a & RAMMASK] | (m[(a & RAMMASK) + 1] << 8)) : 0; }
static int rds16(const uint8_t *m, uint32_t a) { return (int16_t)rdu16(m, a); }
static uint32_t rdu32(const uint8_t *m, uint32_t a) { return in_ram(a, 4) ? (uint32_t)m[a & RAMMASK] | ((uint32_t)m[(a & RAMMASK) + 1] << 8) | ((uint32_t)m[(a & RAMMASK) + 2] << 16) | ((uint32_t)m[(a & RAMMASK) + 3] << 24) : 0; }
static int rds32(const uint8_t *m, uint32_t a) { return (int32_t)rdu32(m, a); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ------------------------------------------------------------------------------------------------ ajustes */
NsFxTuning ns_fx_tuning_default(void) {
    NsFxTuning t; memset(&t, 0, sizeof t);
    t.y_scale = 66.0f / 64.0f; t.line_px = 1.0f; t.line_tex_px = 1.5f; t.shadow_half = 4000.0f;
    t.px_z_min = 32.0f; t.px_z_ref = 1000.0f; t.ring_outer_px = 230.0f; t.ring_segments = 16;
    t.wave_rows = 4; t.wave_period = 32; t.bar_rows = 6; t.idx_max = 65536u; t.chain_max = 32;
    return t;
}
NsFxOpts ns_fx_opts_default(void) { NsFxOpts o; memset(&o, 0, sizeof o); o.buffer = -1; o.tune = ns_fx_tuning_default(); o.owner_roles = 1; o.include_viewlocked = 1; o.include_engine_culled = 1; o.check_reproj = 1; return o; }

/* ------------------------------------------------------------------------------------------------ tabla de actores (solo hashes) */
uint32_t ns_fx_name_hash(const char *s) { uint32_t h = 0x811C9DC5u; for (; *s; s++) h = (h ^ (uint8_t)*s) * 0x01000193u; return h; }
static const NsFxActorEntry PROD_TAB[] = {
    {0xA3E8C479u, NSFX_AK_WATER}, {0x79FD80A8u, NSFX_AK_GAS}, {0x1E6D9C0Cu, NSFX_AK_FADE},
    {0x5CA63D1Du, NSFX_ROLE_LIGHT},
    {0x70ECA404u, NSFX_ROLE_LIGHT},
    {0x812E86A5u, NSFX_ROLE_LIGHT},
    {0xECEEB9ECu, NSFX_ROLE_LIGHT},
    {0x9075794Cu, NSFX_ROLE_LIGHT},
    {0xD568C8EBu, NSFX_ROLE_LIGHT},
    {0xE6B2056Eu, NSFX_ROLE_LIGHT},
    {0x8D683859u, NSFX_ROLE_LIGHT},
    {0x3C82CA9Cu, NSFX_ROLE_LIGHT},
    {0xA01F9914u, NSFX_ROLE_LIGHT},
    {0xDB5B6C9Bu, NSFX_ROLE_LIGHT},
    {0xA20414BAu, NSFX_ROLE_LIGHT},
    {0x550E0920u, NSFX_ROLE_LIGHT},
    {0x338E88D4u, NSFX_ROLE_LIGHT},
    {0x6A1B4ECBu, NSFX_ROLE_BUBBLE},
    {0xC9459602u, NSFX_ROLE_BUBBLE},
    {0x157973DFu, NSFX_ROLE_BUBBLE},
    {0xCF22CC0Cu, NSFX_ROLE_BUBBLE},
    {0x4B46A701u, NSFX_ROLE_SMOKE},
    {0x6E859DB5u, NSFX_ROLE_SMOKE},
    {0x84630B4Eu, NSFX_ROLE_SMOKE},
    {0x1DB7A8BDu, NSFX_ROLE_SPARK},
    {0x0F797123u, NSFX_ROLE_SPARK},
    {0xBAB10FE3u, NSFX_ROLE_SPARK},
    {0x3CDDA7E5u, NSFX_ROLE_SPARK},
    {0x22BD2238u, NSFX_ROLE_DROPLET},
    {0x3C59452Cu, NSFX_ROLE_DROPLET},
    {0xE3483FB1u, NSFX_ROLE_DROPLET},
    {0x9968D658u, NSFX_ROLE_DROPLET},
    {0xF078000Du, NSFX_ROLE_DROPLET},
    {0x55F3C58Eu, NSFX_ROLE_DROPLET},
    {0xDFF67DC0u, NSFX_ROLE_DROPLET},
    {0xD3E30AE1u, NSFX_ROLE_SPLASH},
    {0x531A1C6Fu, NSFX_ROLE_SPLASH},
    {0xB895E1F0u, NSFX_ROLE_SPLASH},
    {0x9852A746u, NSFX_ROLE_SPLASH},
    {0x1F098995u, NSFX_ROLE_SPLASH},
    {0xE224DA66u, NSFX_ROLE_SPLASH},
    {0x201C2AF9u, NSFX_ROLE_SPLASH},
    {0x7E079608u, NSFX_ROLE_RIPPLE},
    {0x1300C05Fu, NSFX_ROLE_RIPPLE},
    {0x0C7BDF22u, NSFX_ROLE_TRACER},
    {0x17E45EAAu, NSFX_ROLE_TRACER},
    {0x9BD7E327u, NSFX_ROLE_TRACER},
    {0xF996150Au, NSFX_ROLE_TRACER},
    {0xE75CBC57u, NSFX_ROLE_TRACER},
    {0x0F130FEAu, NSFX_ROLE_TRACER},
    {0x65B8B609u, NSFX_ROLE_TRACER},
    {0x67DA84ECu, NSFX_ROLE_TRACER},
    {0xD484C9B9u, NSFX_ROLE_TRACER},
    {0xFF93528Eu, NSFX_ROLE_BEAM},
    {0x7A5C68A7u, NSFX_ROLE_BEAM},
    {0xCF2798FEu, NSFX_ROLE_BEAM},
    {0x3358C645u, NSFX_ROLE_DECAL},
    {0xD5D6FB5Bu, NSFX_ROLE_DECAL},
    {0x6C46E4BAu, NSFX_ROLE_DECAL},
    {0xCC5FB1C9u, NSFX_ROLE_SHADOW},
    {0x6EDB750Fu, NSFX_ROLE_SHADOW},
    {0x59AF06C0u, NSFX_ROLE_PLASMA},
    {0xFF00FD93u, NSFX_ROLE_PLASMA},
    {0xEB4E9D07u, NSFX_ROLE_PLASMA},
    {0x7DECB08Bu, NSFX_ROLE_SURFACE},
    {0x608DC056u, NSFX_ROLE_SURFACE},
    {0x43B7EF3Au, NSFX_ROLE_SURFACE},
    {0xEF167DF4u, NSFX_ROLE_SURFACE},
    {0x78193186u, NSFX_ROLE_WATER_SURFACE},
    {0x0A77E7FFu, NSFX_ROLE_WATER_SURFACE},
    {0x8DDB7395u, NSFX_ROLE_AMBIENT},
    {0xF24DFA68u, NSFX_ROLE_AMBIENT},
    {0x6BCE39BDu, NSFX_ROLE_AMBIENT},
    {0x5B9DBFB6u, NSFX_ROLE_PARTICLE},
    {0xAC19CFA5u, NSFX_ROLE_CONE},
    {0x83C2ACAAu, NSFX_ROLE_CONE},
    {0x5B67A1D8u, NSFX_ROLE_TRAIL},
    {0xCBDAE533u, NSFX_ROLE_TRAIL},
    {0x78356D8Du, NSFX_ROLE_STRIP},
    {0x371FF4B3u, NSFX_ROLE_PARTICLE},
    {0x7BBF9889u, NSFX_ROLE_PARTICLE},
    {0x5771629Au, NSFX_ROLE_SHADOW_OBJ},
};
static const NsFxActorEntry *g_tab = PROD_TAB; static uint32_t g_tab_n = sizeof PROD_TAB / sizeof PROD_TAB[0];
void ns_fx_set_actor_table(const NsFxActorEntry *t, uint32_t n) { if (t) { g_tab = t; g_tab_n = n; } else { g_tab = PROD_TAB; g_tab_n = sizeof PROD_TAB / sizeof PROD_TAB[0]; } }
static int code_of_hash(uint32_t h) { for (uint32_t i = 0; i < g_tab_n; i++) if (g_tab[i].hash == h) return g_tab[i].code; return NSFX_ROLE_NONE; }

typedef struct { uint32_t node; uint32_t hash; } ActorRec;
#define MAX_ACTORS 1024
/* Recorre las 9 listas de actores. Cada nodo es el propio actor (cabecera: +8 función, +20 puntero al nombre de fichero). */
static int walk_actors(const uint8_t *ram, ActorRec *out, int cap) {
    int n = 0;
    for (int lv = 0; lv < 9; lv++) {
        uint32_t base = NSFX_A_ACTOR_LISTS + 68u * (uint32_t)lv, endn = base + 32u;
        uint32_t cur = rdu32(ram, base + 4);
        for (int st = 0; cur != endn && ptr_ok(cur) && st < 1024 && n < cap; st++) {
            if (rdu32(ram, cur + 8)) {
                uint32_t nm = rdu32(ram, cur + 20); uint32_t h = 0;
                if (ptr_ok(nm)) {
                    uint32_t hh = 0x811C9DC5u; int l = 0;
                    for (; l < 31; l++) { uint32_t c = rd8(ram, nm + (uint32_t)l); if (!c) break; hh = (hh ^ c) * 0x01000193u; }
                    h = hh;
                }
                out[n].node = cur; out[n].hash = h; n++;
            }
            cur = rdu32(ram, cur + 4);
        }
    }
    return n;
}

/* ------------------------------------------------------------------------------------------------ cámara */
static void inv3(const float a[3][3], float o[3][3], float *det_out) {
    float c00 = a[1][1] * a[2][2] - a[1][2] * a[2][1], c01 = a[1][2] * a[2][0] - a[1][0] * a[2][2], c02 = a[1][0] * a[2][1] - a[1][1] * a[2][0];
    float det = a[0][0] * c00 + a[0][1] * c01 + a[0][2] * c02;
    if (det_out) *det_out = det;
    if (fabsf(det) < 1e-6f) { memset(o, 0, 9 * sizeof(float)); return; }
    float id = 1.0f / det;
    o[0][0] = c00 * id; o[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * id; o[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * id;
    o[1][0] = c01 * id; o[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * id; o[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * id;
    o[2][0] = c02 * id; o[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * id; o[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * id;
}
static int read_cam(const uint8_t *ram, float ys, NsFxCam *c) {
    memset(c, 0, sizeof *c);
    uint32_t ch = NSFX_A_CHANL;
    c->proj_h = (float)rds16(ram, ch + 0x50); c->y_scale = ys;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) c->inv[i][j] = (float)rds16(ram, ch + 0x10 + 2u * (uint32_t)(3 * i + j)) / 4096.0f;
        c->inv_t[i] = (float)rds32(ram, ch + 0x10 + 20u + 4u * (uint32_t)i);
    }
    float det; inv3(c->inv, c->axes, &det);
    c->det_neg = det < 0;
    int ok = c->proj_h >= 16 && c->proj_h <= 4096 && fabsf(fabsf(det) - 1.0f) < 0.25f;
    for (int i = 0; i < 3 && ok; i++) {
        float l = c->inv[i][0] * c->inv[i][0] + c->inv[i][1] * c->inv[i][1] + c->inv[i][2] * c->inv[i][2];
        if (fabsf(l - 1.0f) > 0.25f) ok = 0;
    }
    for (int r = 0; r < 3; r++) c->eye[r] = -(c->axes[r][0] * c->inv_t[0] + c->axes[r][1] * c->inv_t[1] + c->axes[r][2] * c->inv_t[2]);
    c->valid = ok;
    return ok ? 0 : -1;
}
static void w2v(const NsFxCam *c, const float w[3], float v[3]) {
    for (int r = 0; r < 3; r++) v[r] = c->inv[r][0] * w[0] + c->inv[r][1] * w[1] + c->inv[r][2] * w[2] + c->inv_t[r];
    v[1] *= c->y_scale;
}
static void v2w(const NsFxCam *c, const float v_in[3], float w[3]) {
    float v[3] = {v_in[0] - c->inv_t[0], v_in[1] / c->y_scale - c->inv_t[1], v_in[2] - c->inv_t[2]};
    for (int r = 0; r < 3; r++) w[r] = c->axes[r][0] * v[0] + c->axes[r][1] * v[1] + c->axes[r][2] * v[2];
}
static void vg2w(const NsFxCam *c, const float v_in[3], float w[3]) {
    float v[3] = {v_in[0] - c->inv_t[0], v_in[1] - c->inv_t[1], v_in[2] - c->inv_t[2]};
    for (int r = 0; r < 3; r++) w[r] = c->axes[r][0] * v[0] + c->axes[r][1] * v[1] + c->axes[r][2] * v[2];
}

/* ------------------------------------------------------------------------------------------------ paquetes */
enum { KIND_S2F = 0, KIND_S4G = 5, KIND_SPR = 6, KIND_SPR8 = 7, KIND_SPR16 = 8, KIND_BOX = 9, KIND_BOX1 = 10, KIND_BOX8 = 11, KIND_BOX16 = 12,
       KIND_P3F = 13, KIND_P4F = 14, KIND_P3G = 15, KIND_P4G = 16, KIND_P3T = 17, KIND_P4T = 18, KIND_P3TG = 19, KIND_P4TG = 20,
       KIND_S2T = 21, KIND_S2TG = 22, KIND_FREE = 23 };
enum { T_INVISIBLE = 0x100, T_ON_CAMERA = 0x200, T_RECT = 0x400, T_SORTONLY = 0x800, T_ONESIDE = 0x1000, T_FREEPACKS = 0x2000 };
enum { C_LINE = 0, C_RECT = 1, C_POLY = 2 };

typedef struct {
    int cls, nv;
    int x[4], y[4];
    uint8_t u[4], v[4], rgb[4][3], code;
    int textured, gouraud, semi, raw;
    uint16_t clut, tpage;
    int w, h;
    int sz;
} Dec;

static int kind_textured(int k) { return (k >= KIND_SPR && k <= KIND_SPR16) || (k >= KIND_P3T && k <= KIND_S2TG); }
static int kind_gouraud(int k) { return (k >= 3 && k <= 5) || k == KIND_P3G || k == KIND_P4G || k == KIND_P3TG || k == KIND_P4TG || k == KIND_S2TG; }

/* Decodifica el paquete en `b`. nvdec: nº de vértices a leer (0 = el de la primitiva). Devuelve 0, o -1 si el formato no se conoce. */
static int dec_pack(const uint8_t *m, uint32_t b, int kind, int verts, int voff, int vst, int nvdec, Dec *d) {
    memset(d, 0, sizeof *d);
    if (kind == KIND_FREE || kind < 0 || kind > 23) return -1;
    d->sz = (int)rdu16(m, b);
    d->code = (uint8_t)rd8(m, b + 7);
    d->textured = kind_textured(kind);
    d->gouraud = kind_gouraud(kind);
    d->semi = (d->code & 2) != 0;
    d->raw = d->textured && (d->code & 1);
    int gt_off = d->textured ? 8 : 4;
    d->cls = kind <= KIND_S4G ? C_LINE : kind <= KIND_BOX16 ? C_RECT : C_POLY;
    int nv = nvdec ? nvdec : verts;
    if (nv < 1) nv = 1;
    if (nv > 4) nv = 4;
    if (d->cls == C_RECT) {
        d->x[0] = rds16(m, b + 8); d->y[0] = rds16(m, b + 10);
        for (int c = 0; c < 3; c++) d->rgb[0][c] = (uint8_t)rd8(m, b + 4u + (uint32_t)c);
        if (kind <= KIND_SPR16) { d->u[0] = (uint8_t)rd8(m, b + 12); d->v[0] = (uint8_t)rd8(m, b + 13); d->clut = (uint16_t)rdu16(m, b + 14); }
        switch (kind) {
        case KIND_SPR: d->w = (int)rdu16(m, b + 16); d->h = (int)rdu16(m, b + 18); break;
        case KIND_SPR8: case KIND_BOX8: d->w = d->h = 8; break;
        case KIND_SPR16: case KIND_BOX16: d->w = d->h = 16; break;
        case KIND_BOX: d->w = (int)rdu16(m, b + 12); d->h = (int)rdu16(m, b + 14); break;
        default: d->w = d->h = 1; break;
        }
        d->nv = 1;
        return 0;
    }
    d->nv = nv;
    for (int v = 0; v < nv; v++) {
        uint32_t o = (uint32_t)(voff + vst * v);
        d->x[v] = rds16(m, b + o); d->y[v] = rds16(m, b + o + 2);
        if (d->textured) { d->u[v] = (uint8_t)rd8(m, b + o + 4); d->v[v] = (uint8_t)rd8(m, b + o + 5); }
        uint32_t co = !d->gouraud || v == 0 ? 4u : (uint32_t)(voff + vst * (v - 1) + gt_off);
        for (int c = 0; c < 3; c++) d->rgb[v][c] = (uint8_t)rd8(m, b + co + (uint32_t)c);
    }
    if (d->textured) { d->clut = (uint16_t)rdu16(m, b + (uint32_t)voff + 6); d->tpage = (uint16_t)rdu16(m, b + (uint32_t)(voff + vst) + 6); }
    return 0;
}

/* ------------------------------------------------------------------------------------------------ escena */
NsFxScene *ns_fx_scene_create(uint32_t cap) {
    NsFxScene *s = (NsFxScene *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->cap_items = cap ? cap : NSFX_MAX_ITEMS_DEFAULT;
    uint32_t hs = 1; while (hs < s->cap_items * 2u + 2u) hs <<= 1;
    s->items = (NsFxItem *)calloc(s->cap_items, sizeof(NsFxItem));
    s->pm_key = (uint64_t *)calloc(hs, sizeof(uint64_t));
    s->pm_val = (int32_t *)calloc(hs, sizeof(int32_t));
    s->pm_mask = hs - 1;
    if (!s->items || !s->pm_key || !s->pm_val) { free(s->items); free(s->pm_key); free(s->pm_val); free(s); return NULL; }
    return s;
}
void ns_fx_scene_destroy(NsFxScene *s) { if (s) { free(s->items); free(s->pm_key); free(s->pm_val); free(s); } }

static uint32_t pm_hash(uint64_t k) { k ^= k >> 33; k *= 0xFF51AFD7ED558CCDull; k ^= k >> 33; return (uint32_t)k; }
static int32_t pm_find(const NsFxScene *s, uint32_t prim, uint32_t idx) {
    uint64_t key = ((uint64_t)prim << 16) | (idx & 0xFFFFu); key |= 1ull << 63;
    for (uint32_t h = pm_hash(key) & s->pm_mask, n = 0; n <= s->pm_mask; h = (h + 1) & s->pm_mask, n++) {
        if (s->pm_key[h] == key) return s->pm_val[h];
        if (!s->pm_key[h]) return -1;
    }
    return -1;
}
static void pm_build(NsFxScene *s) {
    memset(s->pm_key, 0, sizeof(uint64_t) * (s->pm_mask + 1u));
    for (uint32_t i = 0; i < s->n_items; i++) {
        uint64_t key = (((uint64_t)s->items[i].prim << 16) | s->items[i].idx) | (1ull << 63);
        uint32_t h = pm_hash(key) & s->pm_mask;
        while (s->pm_key[h] && s->pm_key[h] != key) h = (h + 1) & s->pm_mask;
        if (!s->pm_key[h]) { s->pm_key[h] = key; s->pm_val[h] = (int32_t)i; }
    }
}

typedef struct {
    const uint8_t *ram; const NsFxOpts *opt; NsFxScene *sc; int buf; uint32_t map;
    uint32_t qprim[1024]; uint8_t qrole[1024];
    int nq;
} Ctx;

static NsFxItem *new_item(Ctx *c) {
    NsFxScene *s = c->sc;
    if (s->n_items >= s->cap_items) { s->stats.n_overflow++; return NULL; }
    NsFxItem *it = &s->items[s->n_items++];
    memset(it, 0, sizeof *it);
    return it;
}
static void count_item(Ctx *c, const NsFxItem *it) {
    c->sc->stats.by_shape[it->shape <= 4 ? it->shape : 0]++;
    c->sc->stats.by_role[it->role < NSFX_ROLE_COUNT ? it->role : 0]++;
    if (it->flags & NSFX_F_ENGINE_CULLED) c->sc->stats.n_culled++;
}
static void chk_add(Ctx *c, float ex, float ey, int ax, int ay) {
    NsFxScene *s = c->sc;
    float e = fmaxf(fabsf(ex - (float)ax), fabsf(ey - (float)ay));
    s->stats.chk_n++; s->stats.chk_sum_px += e; if (e > s->stats.chk_max_px) s->stats.chk_max_px = e;
    if (e > 3.0f) s->stats.chk_bad++;
}
static int heuristic_role(const NsFxItem *it) {
    switch (it->shape) {
    case NSFX_SH_RIBBON: return NSFX_ROLE_BEAM;
    case NSFX_SH_BILLBOARD: return (it->flags & NSFX_F_SEMI) ? NSFX_ROLE_LIGHT : NSFX_ROLE_SPRITE_ANIM;
    default: return (it->flags & NSFX_F_SEMI) ? NSFX_ROLE_PARTICLE : NSFX_ROLE_SURFACE;
    }
}

/* Rellena lo común del elemento y los datos del paquete para casarlo con la lista de la GPU. */
static void fill_common(NsFxItem *it, uint32_t prim, int j, int kind, const Dec *d, int16_t raise, int sz) {
    it->prim = prim; it->idx = (uint16_t)j; it->kind = (uint8_t)kind; it->raise = raise; it->sz = sz;
    if (d->semi) it->flags |= NSFX_F_SEMI;
    if (d->textured) it->flags |= NSFX_F_TEXTURED;
    if (d->gouraud) it->flags |= NSFX_F_GOURAUD;
    if (d->raw) it->flags |= NSFX_F_RAW;
    if (sz == 0) it->flags |= NSFX_F_LOW_CONF;
    it->gflags = (uint16_t)((d->gouraud ? GPU_F_GOURAUD : 0) | (d->textured ? GPU_F_TEXTURED : 0) | (d->raw ? GPU_F_RAW : 0) | (d->semi ? GPU_F_SEMI : 0));
    it->ptp = 255;
    if (d->textured) {
        it->clut = d->clut;
        if (kind >= KIND_P3T) { it->tpage = d->tpage; it->abr = (uint8_t)((d->tpage >> 5) & 3); it->tp = (uint8_t)((d->tpage >> 7) & 3); it->ptp = it->tp; }
        else it->flags |= NSFX_F_TPAGE_UNKNOWN;
    }
    if (d->semi && (!d->textured || kind < KIND_P3T)) { it->abr = 1; it->flags |= NSFX_F_ABR_UNKNOWN; }
    for (int v = 0; v < 4; v++) {
        int sv = (d->cls == C_RECT || v >= d->nv) ? 0 : v;
        for (int c = 0; c < 3; c++) it->rgb[v][c] = d->rgb[sv][c];
    }
    it->pcls = (uint8_t)d->cls; it->pnv = (uint8_t)(d->cls == C_RECT ? 1 : d->nv);
    for (int v = 0; v < 4 && v < d->nv; v++) { it->sxy[v][0] = (int16_t)d->x[v]; it->sxy[v][1] = (int16_t)d->y[v]; }
    it->pu0 = d->u[0]; it->pv0 = d->v[0]; it->sw = (uint16_t)d->w; it->sh = (uint16_t)d->h;
}

static void mat_world(const uint8_t *m, uint32_t a, float R[3][3], float t[3]) {
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) R[i][j] = (float)rds16(m, a + 2u * (uint32_t)(3 * i + j)) / 4096.0f;
        t[i] = (float)rds32(m, a + 20u + 4u * (uint32_t)i);
    }
}
static void xf(const float R[3][3], const float t[3], const float p[3], float o[3]) { for (int r = 0; r < 3; r++) o[r] = R[r][0] * p[0] + R[r][1] * p[1] + R[r][2] * p[2] + t[r]; }
static void cross3(const float a[3], const float b[3], float o[3]) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static void set_face(NsFxItem *it, const NsFxCam *cam) {
    float a[3], b[3], n[3];
    for (int i = 0; i < 3; i++) { a[i] = it->pos[1][i] - it->pos[0][i]; b[i] = it->pos[2][i] - it->pos[0][i]; }
    cross3(a, b, n);
    float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), s = cam->det_neg ? -1.0f : 1.0f;
    if (l > 1e-6f) for (int i = 0; i < 3; i++) it->fn[i] = s * n[i] / l;
    it->flags |= NSFX_F_ONESIDE;
}
/* Profundidad con la que se convierte un tamaño en píxeles a mundo: la real si es fiable, si no la de referencia (el cartel no debe quedar invisible). */
static float eff_z(const NsFxTuning *t, float z) { return z >= t->px_z_min ? z : t->px_z_ref; }
static void set_uv_rect(NsFxItem *it, const Dec *d) {
    it->uv[0][0] = d->u[0]; it->uv[0][1] = d->v[0];
    int u1 = d->u[0] + (d->w > 0 ? d->w - 1 : 0), v1 = d->v[0] + (d->h > 0 ? d->h - 1 : 0);
    it->uv[1][0] = (uint8_t)(u1 > 255 ? 255 : u1); it->uv[1][1] = d->v[0];
    it->uv[2][0] = d->u[0]; it->uv[2][1] = (uint8_t)(v1 > 255 ? 255 : v1);
    it->uv[3][0] = it->uv[1][0]; it->uv[3][1] = it->uv[2][1];
}

/* ---- una primitiva ---- */
static void do_prim(Ctx *c, uint32_t p, int role_owner) {
    const uint8_t *m = c->ram; NsFxScene *S = c->sc; const NsFxCam *cam = &S->cam; const NsFxTuning *T = &c->opt->tune;
    S->stats.n_prims++;
    int type = rds32(m, p + 0x24), kind = type & 0x1F;
    uint32_t group = rdu16(m, p + 0x28);
    int n = rds16(m, p + 0x2A); int16_t raise = (int16_t)rds16(m, p + 0x2E);
    int psz = rds16(m, p + 0x30), verts = rds16(m, p + 0x32), voff = rds16(m, p + 0x34), vst = rds16(m, p + 0x36);
    uint32_t pos = rdu32(m, p + 0x38), rect = rdu32(m, p + 0x3C), packs = rdu32(m, p + 0x40 + 4u * (uint32_t)c->buf);
    if (type & T_SORTONLY) { S->stats.n_sortonly++; return; }
    if (type & T_INVISIBLE) { S->stats.n_invisible++; return; }
    if (group && !(group & c->map)) { S->stats.n_wrong_map++; return; }
    if (n <= 0 || n > 8192 || psz < 8 || psz > 64 || verts < 1 || verts > 4 || !ptr_ok(packs) || !in_ram(packs, (uint32_t)n * (uint32_t)psz) || kind > 23) { S->stats.n_bad++; return; }
    if (kind == KIND_FREE) { S->stats.n_free++; return; }
    if (!c->opt->include_viewlocked && (type & T_ON_CAMERA)) return;
    int rectmode = (type & T_RECT) != 0, freepacks = (type & T_FREEPACKS) != 0, oncam = (type & T_ON_CAMERA) != 0, oneside = (type & T_ONESIDE) != 0;
    int is_line = kind <= KIND_S4G, is_tex_line = kind == KIND_S2T || kind == KIND_S2TG;
    int vcount = is_tex_line ? 2 : verts;
    if (!freepacks) {
        uint32_t need = rectmode ? (uint32_t)n : (uint32_t)n * (uint32_t)vcount;
        if (!ptr_ok(pos) || !in_ram(pos, 8u * need)) { S->stats.n_bad++; return; }
    }
    uint32_t root = rdu32(m, p + 0x20);
    float R[3][3], Tt[3];
    mat_world(m, ptr_ok(root) ? root : p, R, Tt);
    float ys = cam->y_scale, H = cam->proj_h;
    int role_prim = role_owner;
    for (int j = 0; j < n; j++) {
        uint32_t b = packs + (uint32_t)j * (uint32_t)psz;
        Dec d;
        if (dec_pack(m, b, kind, verts, voff, vst, is_tex_line ? 4 : 0, &d) < 0) continue;
        int sz = d.sz, culled = sz == 0;
        if (culled && !c->opt->include_engine_culled) continue;
        float wv[4][3], vv[4][3]; int nvp = 0;
        if (!freepacks) {
            int cnt = rectmode ? 1 : vcount;
            for (int v = 0; v < cnt; v++) {
                uint32_t q = pos + 8u * (uint32_t)(rectmode ? j : j * vcount + v);
                float lp[3] = {(float)rds16(m, q), (float)rds16(m, q + 2), (float)rds16(m, q + 4)};
                if (oncam) { memcpy(vv[v], lp, sizeof lp); v2w(cam, vv[v], wv[v]); }
                else { xf(R, Tt, lp, wv[v]); w2v(cam, wv[v], vv[v]); }
            }
            nvp = cnt;
        }
        if (freepacks) {
            /* El motor ya proyectó el paquete con su propia función: se reconstruye a la profundidad del paquete (plano de la cámara del motor). */
            if (culled || d.cls == C_LINE) { if (culled) S->stats.n_culled++; continue; }
            float z = eff_z(T, (float)sz);
            float vx[4][3]; int nn = d.cls == C_RECT ? 4 : d.nv;
            int bx0 = 99999, by0 = 99999, bx1 = -99999, by1 = -99999, sx[4], sy[4];
            if (d.cls == C_RECT) { sx[0] = d.x[0]; sy[0] = d.y[0]; sx[1] = d.x[0] + d.w; sy[1] = d.y[0]; sx[2] = d.x[0]; sy[2] = d.y[0] + d.h; sx[3] = d.x[0] + d.w; sy[3] = d.y[0] + d.h; }
            else for (int v = 0; v < nn; v++) { sx[v] = d.x[v]; sy[v] = d.y[v]; }
            for (int v = 0; v < nn; v++) { if (sx[v] < bx0) bx0 = sx[v]; if (sx[v] > bx1) bx1 = sx[v]; if (sy[v] < by0) by0 = sy[v]; if (sy[v] > by1) by1 = sy[v]; vx[v][0] = (float)sx[v] * z / H; vx[v][1] = (float)sy[v] * z / H; vx[v][2] = z; }
            if (nn < 3 && d.cls != C_RECT) continue;
            int axis_rect = d.cls == C_RECT || (nn == 4 && d.x[0] == d.x[2] && d.x[1] == d.x[3] && d.y[0] == d.y[1] && d.y[2] == d.y[3]);
            NsFxItem *it = new_item(c); if (!it) return;
            fill_common(it, p, j, kind, &d, raise, sz);
            it->role = (uint8_t)role_prim;
            if (axis_rect) {
                it->shape = NSFX_SH_BILLBOARD; it->nv = 4; it->flags |= NSFX_F_PXSIZE;
                float cxs = 0.5f * (float)(bx0 + bx1), cys = 0.5f * (float)(by0 + by1);
                float cv[3] = {cxs * z / H, cys * z / H, z};
                vg2w(cam, cv, it->pos[0]); memcpy(it->vpos[0], cv, sizeof cv);
                float hw = 0.5f * (float)(bx1 - bx0) * z / H, hh = 0.5f * (float)(by1 - by0) * z / (H * ys);
                it->off[0] = -hw; it->off[1] = -hh; it->off[2] = hw; it->off[3] = hh;
                it->box_px[0] = -0.5f * (float)(bx1 - bx0); it->box_px[1] = -0.5f * (float)(by1 - by0); it->box_px[2] = -it->box_px[0]; it->box_px[3] = -it->box_px[1];
                if (d.cls == C_RECT) set_uv_rect(it, &d);
                else {
                    for (int v = 0; v < 4; v++) { it->uv[v][0] = d.u[v]; it->uv[v][1] = d.v[v]; }
                    int idx4[4] = {0, 0, 0, 0}, ok4 = 1;
                    for (int v = 0; v < 4; v++) { int col = d.x[v] > cxs ? 1 : 0, row = d.y[v] > cys ? 1 : 0; idx4[row * 2 + col] = v + 1; }
                    for (int q = 0; q < 4; q++) if (!idx4[q]) ok4 = 0;
                    if (ok4) {
                        uint8_t uvn[4][2], rg[4][3];
                        for (int q = 0; q < 4; q++) { uvn[q][0] = d.u[idx4[q] - 1]; uvn[q][1] = d.v[idx4[q] - 1]; memcpy(rg[q], d.rgb[idx4[q] - 1], 3); }
                        memcpy(it->uv, uvn, sizeof uvn); memcpy(it->rgb, rg, sizeof rg);
                    }
                }
            } else {
                it->shape = nn == 3 ? NSFX_SH_TRI : NSFX_SH_QUAD; it->nv = (uint8_t)nn;
                for (int v = 0; v < nn; v++) { vg2w(cam, vx[v], it->pos[v]); memcpy(it->vpos[v], vx[v], sizeof vx[v]); it->uv[v][0] = d.u[v]; it->uv[v][1] = d.v[v]; }
                if (oneside) set_face(it, cam);
            }
            if (!it->role) it->role = (uint8_t)heuristic_role(it);
            count_item(c, it); continue;
        }
        if (rectmode) {
            float z = vv[0][2], ze = eff_z(T, z);
            NsFxItem *it = new_item(c); if (!it) return;
            fill_common(it, p, j, kind, &d, raise, sz);
            it->shape = NSFX_SH_BILLBOARD; it->nv = 4;
            if (oncam) it->flags |= NSFX_F_VIEWLOCKED;
            if (culled) it->flags |= NSFX_F_ENGINE_CULLED;
            memcpy(it->pos[0], wv[0], sizeof wv[0]); memcpy(it->vpos[0], vv[0], sizeof vv[0]);
            int rx = 0, ry = 0, rw = 0, rh = 0;
            if (ptr_ok(rect)) { rx = rds16(m, rect); ry = rds16(m, rect + 2); rw = rds16(m, rect + 4); rh = rds16(m, rect + 6); }
            float l, t, r, bt;
            if (verts == 1 || d.cls == C_RECT) {
                float k = ze / H;
                l = (float)(-rx) * k; t = (float)(-ry) * k / ys; r = l + (float)d.w * k; bt = t + (float)d.h * k / ys;
                it->flags |= NSFX_F_PXSIZE;
                it->box_px[0] = (float)-rx; it->box_px[1] = (float)-ry; it->box_px[2] = (float)(d.w - rx); it->box_px[3] = (float)(d.h - ry);
            } else {
                l = (float)(-rx); t = (float)(-ry) / ys; r = (float)(rw - rx); bt = (float)(rh - ry) / ys;
                float k = H / ze;
                it->box_px[0] = l * k; it->box_px[1] = t * ys * k; it->box_px[2] = r * k; it->box_px[3] = bt * ys * k;
            }
            it->off[0] = l; it->off[1] = t; it->off[2] = r; it->off[3] = bt;
            if (d.cls == C_RECT) set_uv_rect(it, &d);
            else for (int v = 0; v < 4 && v < d.nv; v++) { it->uv[v][0] = d.u[v]; it->uv[v][1] = d.v[v]; }
            it->role = (uint8_t)(role_prim ? role_prim : heuristic_role(it));
            if (c->opt->check_reproj && !culled && z > 1) {
                float sx = H * vv[0][0] / z, sy = H * vv[0][1] / z;
                if (fabsf(sx) < 600 && fabsf(sy) < 600) {
                    if (d.cls == C_RECT) chk_add(c, sx - (float)rx, sy - (float)ry, d.x[0], d.y[0]);
                    else if (d.nv == 4) {
                        float k2 = H / z;
                        chk_add(c, sx + (float)(-rx) * k2, sy + (float)(-ry) * k2, d.x[0], d.y[0]);
                        chk_add(c, sx + (float)(rw - rx) * k2, sy + (float)(rh - ry) * k2, d.x[3], d.y[3]);
                    }
                }
            }
            count_item(c, it); continue;
        }
        if (nvp < 1) continue;
        if (is_line || is_tex_line) {
            int nseg = is_tex_line ? 1 : vcount - 1;
            float pxw = is_tex_line ? T->line_tex_px : T->line_px;
            for (int sgi = 0; sgi < nseg; sgi++) {
                NsFxItem *it = new_item(c); if (!it) return;
                fill_common(it, p, j, kind, &d, raise, sz);
                it->sub = (uint16_t)sgi; it->shape = NSFX_SH_RIBBON; it->nv = 2;
                if (!is_tex_line) {   /* la GPU ve una línea de dos vértices por segmento */
                    it->pcls = C_LINE; it->pnv = 2; memset(it->sxy, 0, sizeof it->sxy);
                    for (int e = 0; e < 2; e++) { it->sxy[e][0] = (int16_t)d.x[sgi + e]; it->sxy[e][1] = (int16_t)d.y[sgi + e]; }
                }
                if (oncam) it->flags |= NSFX_F_VIEWLOCKED;
                if (culled) it->flags |= NSFX_F_ENGINE_CULLED;
                for (int e = 0; e < 2; e++) {
                    memcpy(it->pos[e], wv[sgi + e], sizeof wv[0]); memcpy(it->vpos[e], vv[sgi + e], sizeof vv[0]);
                    float z = eff_z(T, vv[sgi + e][2]); it->width[e] = pxw * z / H;
                    memcpy(it->rgb[e], d.rgb[(sgi + e) < d.nv ? sgi + e : 0], 3);
                }
                if (d.textured) { it->uv[0][0] = d.u[0]; it->uv[0][1] = d.v[0]; it->uv[1][0] = d.u[1]; it->uv[1][1] = d.v[1]; }
                it->role = (uint8_t)(role_prim ? role_prim : heuristic_role(it));
                if (c->opt->check_reproj && !culled && sgi == 0 && !is_tex_line)
                    for (int e = 0; e < 2; e++) { float z = vv[e][2]; if (z > 1) { float sx = H * vv[e][0] / z, sy = H * vv[e][1] / z; if (fabsf(sx) < 600 && fabsf(sy) < 600) chk_add(c, sx, sy, d.x[e], d.y[e]); } }
                count_item(c, it);
            }
            continue;
        }
        if (verts == 1) {   /* sprite/rectángulo con proyección de vértice: esquina sup-izq en el punto proyectado */
            NsFxItem *it = new_item(c); if (!it) return;
            fill_common(it, p, j, kind, &d, raise, sz);
            it->shape = NSFX_SH_BILLBOARD; it->nv = 4; it->flags |= NSFX_F_PXSIZE;
            if (oncam) it->flags |= NSFX_F_VIEWLOCKED;
            if (culled) it->flags |= NSFX_F_ENGINE_CULLED;
            float z = vv[0][2], k = eff_z(T, z) / H;
            memcpy(it->pos[0], wv[0], sizeof wv[0]); memcpy(it->vpos[0], vv[0], sizeof vv[0]);
            it->off[0] = 0; it->off[1] = 0; it->off[2] = (float)d.w * k; it->off[3] = (float)d.h * k / ys;
            it->box_px[0] = 0; it->box_px[1] = 0; it->box_px[2] = (float)d.w; it->box_px[3] = (float)d.h;
            set_uv_rect(it, &d);
            it->role = (uint8_t)(role_prim ? role_prim : heuristic_role(it));
            if (c->opt->check_reproj && !culled && z > 1) { float sx = H * vv[0][0] / z, sy = H * vv[0][1] / z; if (fabsf(sx) < 600 && fabsf(sy) < 600) chk_add(c, sx, sy, d.x[0], d.y[0]); }
            count_item(c, it); continue;
        }
        if (vcount < 3 || nvp < 3) continue;   /* polígono sin vértices suficientes */
        {
            NsFxItem *it = new_item(c); if (!it) return;
            fill_common(it, p, j, kind, &d, raise, sz);
            int nn = vcount > 4 ? 4 : vcount;
            it->shape = nn == 3 ? NSFX_SH_TRI : NSFX_SH_QUAD; it->nv = (uint8_t)nn;
            if (oncam) it->flags |= NSFX_F_VIEWLOCKED;
            if (culled) it->flags |= NSFX_F_ENGINE_CULLED;
            for (int v = 0; v < nn && v < nvp; v++) {
                memcpy(it->pos[v], wv[v], sizeof wv[0]); memcpy(it->vpos[v], vv[v], sizeof vv[0]);
                it->uv[v][0] = d.u[v]; it->uv[v][1] = d.v[v];
            }
            if (oneside) set_face(it, cam);
            it->role = (uint8_t)(role_prim ? role_prim : heuristic_role(it));
            if (c->opt->check_reproj && !culled)
                for (int v = 0; v < nn && v < nvp; v++) { float z = vv[v][2]; if (z > 1) { float sx = H * vv[v][0] / z, sy = H * vv[v][1] / z; if (fabsf(sx) < 600 && fabsf(sy) < 600) chk_add(c, sx, sy, d.x[v], d.y[v]); } }
            count_item(c, it);
        }
    }
}

/* ------------------------------------------------------------------------------------------------ sombras de objeto (malla de una pieza) */
static void do_shadow(Ctx *c, uint32_t node) {
    const uint8_t *m = c->ram; NsFxScene *S = c->sc; const NsFxCam *cam = &S->cam; const NsFxTuning *T = &c->opt->tune;
    uint32_t objs = rdu32(m, node + 0x28);
    if (!ptr_ok(objs) || !in_ram(objs, 0x48 + 0x5C)) return;
    if (S->n_shadow_objs >= NSFX_MAX_SHADOW_OBJS) { S->stats.n_overflow++; return; }   /* sin sitio en la lista de reclamados: no se extrae (el objeto se dibuja como malla) */
    S->shadow_objs[S->n_shadow_objs++] = objs;
    uint32_t flag = rdu32(m, objs + 0x28), group = rdu16(m, objs + 0x2C);
    if ((flag & 0x80) || !rds32(m, node + 0x90)) return;
    if (group && !(group & c->map)) return;
    float R[3][3], Tt[3]; mat_world(m, objs, R, Tt);
    float hx = T->shadow_half * (float)rds16(m, objs + 0x48) / 4096.0f, hz = T->shadow_half * (float)rds16(m, objs + 0x48 + 16) / 4096.0f;
    float loc[4][3] = {{-hx, 0, -hz}, {hx, 0, -hz}, {-hx, 0, hz}, {hx, 0, hz}};
    float wc[4][3], vc[4][3];
    for (int i = 0; i < 4; i++) { xf(R, Tt, loc[i], wc[i]); w2v(cam, wc[i], vc[i]); }
    uint32_t piece = objs + 0x48, pk = rdu32(m, piece + 0x54 + 4u * (uint32_t)c->buf);
    int have = ptr_ok(pk) && rds16(m, piece + 0x52) > 0 && in_ram(pk, 52);
    Dec d; memset(&d, 0, sizeof d);
    int order[4] = {0, 1, 2, 3}, sz = 0;
    if (have) {
        dec_pack(m, pk, KIND_P4TG, 4, 8, 12, 4, &d);
        sz = d.sz;
        if (sz != 0 && (d.code & 0x20)) {
            int used = 0, okm = 1, ord[4] = {0, 0, 0, 0};
            for (int v = 0; v < 4 && okm; v++) {
                int best = -1; float bd = 1e30f;
                for (int q = 0; q < 4; q++) {
                    float z = vc[q][2]; if (z < 1) continue;
                    float dx = cam->proj_h * vc[q][0] / z - (float)d.x[v], dy = cam->proj_h * vc[q][1] / z - (float)d.y[v];
                    float dd = dx * dx + dy * dy; if (dd < bd) { bd = dd; best = q; }
                }
                if (best < 0 || (used & (1 << best))) okm = 0; else { used |= 1 << best; ord[v] = best; }
            }
            if (okm) memcpy(order, ord, sizeof order);
        }
    }
    NsFxItem *it = new_item(c); if (!it) return;
    it->prim = objs; it->idx = 0; it->shape = NSFX_SH_QUAD; it->role = NSFX_ROLE_SHADOW_OBJ; it->kind = KIND_P4TG; it->nv = 4;
    it->flags = NSFX_F_FROM_OBJ | NSFX_F_TEXTURED | NSFX_F_SEMI | NSFX_F_GOURAUD;
    it->gflags = GPU_F_TEXTURED | GPU_F_SEMI | GPU_F_GOURAUD;
    it->abr = 2; it->raise = (int16_t)rds16(m, piece + 0x50); it->sz = sz; it->ptp = 255;
    it->pcls = C_POLY; it->pnv = 4;
    if (have && d.nv == 4) {
        it->clut = d.clut; it->tpage = d.tpage;
        if (d.tpage) { it->abr = (uint8_t)((d.tpage >> 5) & 3); it->tp = (uint8_t)((d.tpage >> 7) & 3); it->ptp = it->tp; } else it->flags |= NSFX_F_TPAGE_UNKNOWN;
        for (int v = 0; v < 4; v++) { it->sxy[v][0] = (int16_t)d.x[v]; it->sxy[v][1] = (int16_t)d.y[v]; }
        it->pu0 = d.u[0]; it->pv0 = d.v[0];
        if (!(d.code & 2)) it->flags &= (uint16_t)~NSFX_F_SEMI, it->gflags &= (uint16_t)~GPU_F_SEMI;
    } else it->flags |= NSFX_F_TPAGE_UNKNOWN;
    if (!have || sz == 0) it->flags |= NSFX_F_ENGINE_CULLED | NSFX_F_LOW_CONF;
    for (int v = 0; v < 4; v++) {
        int q = order[v];
        memcpy(it->pos[v], wc[q], sizeof wc[q]); memcpy(it->vpos[v], vc[q], sizeof vc[q]);
        it->uv[v][0] = d.u[v]; it->uv[v][1] = d.v[v];
        for (int k = 0; k < 3; k++) it->rgb[v][k] = have ? d.rgb[v][k] : 128;
        if (c->opt->check_reproj && have && sz != 0 && vc[q][2] > 1) {
            float sx = cam->proj_h * vc[q][0] / vc[q][2], sy = cam->proj_h * vc[q][1] / vc[q][2];
            if (fabsf(sx) < 600 && fabsf(sy) < 600) chk_add(c, sx, sy, d.x[v], d.y[v]);
        }
    }
    count_item(c, it);
}

/* ------------------------------------------------------------------------------------------------ efectos de pantalla */
static void read_screen(Ctx *c) {
    const uint8_t *m = c->ram; NsFxScreen *sc = &c->sc->screen; const NsFxTuning *T = &c->opt->tune; memset(sc, 0, sizeof *sc);
    sc->status = rdu32(m, NSFX_A_STATUS);
    sc->aux = (sc->status & 1) != 0; sc->flash = (sc->status & 2) != 0; sc->mode_a = (sc->status & 4) != 0; sc->mode_b = (sc->status & 8) != 0;
    ActorRec *ar = (ActorRec *)calloc(MAX_ACTORS, sizeof *ar); if (!ar) return;
    int na = walk_actors(m, ar, MAX_ACTORS);
    for (int i = 0; i < na; i++) {
        uint32_t n = ar[i].node; int code = code_of_hash(ar[i].hash);
        if (code == NSFX_AK_WATER) {
            NsFxWater *w = &sc->water; int act = rds32(m, n + 0x40) != 0;
            if (w->present && (w->active || !act)) continue;
            w->present = 1; w->active = act; w->actor = n;
            for (int k = 0; k < 3; k++) { w->box_min[k] = (float)rds16(m, n + 0x28 + 2u * (uint32_t)k); w->box_max[k] = (float)rds16(m, n + 0x30 + 2u * (uint32_t)k); w->tint[k] = (float)rd8(m, n + 0x3C + (uint32_t)k) / 128.0f; }
            w->phase = rdu32(m, n + 0x48); w->wave_rows = T->wave_rows; w->wave_period = T->wave_period; w->bar_rows = T->bar_rows;
        } else if (code == NSFX_AK_GAS) {
            NsFxGas *g = &sc->gas; int alloc = rds32(m, n + 0x40) != 0;
            if (g->present && g->active && !alloc) continue;
            g->present = 1; g->actor = n;
            g->active = alloc && !sc->mode_b;
            int inten = rds32(m, n + 0x4C), f44 = rds32(m, n + 0x44), f50 = rds32(m, n + 0x50), f54 = rds32(m, n + 0x54), f64 = rds32(m, n + 0x64);
            g->abr = rds32(m, n + 0x58) & 3; g->inner_px = clampf((float)rds32(m, n + 0x5C), 0.0f, 4096.0f); g->outer_px = T->ring_outer_px;
            g->aspect = clampf((float)f64 / 4096.0f, 0.0f, 2.0f); g->pulse_amp = clampf((float)f50 / 4096.0f, -4.0f, 4.0f);
            g->intensity = clampf((float)inten / 4096.0f, 0, 1);
            float ang = 6.2831853f * (float)(((int64_t)f44 * f54) & 4095) / 4096.0f;
            float scale = (float)inten + sinf(ang) * ((float)inten * (float)f50 / 4096.0f);
            for (int k = 0; k < 3; k++) {
                g->col_inner[k] = clampf((float)rds16(m, n + 0x2C + 2u * (uint32_t)k) * scale / 4096.0f, 0, 255) / 255.0f;
                g->col_outer[k] = clampf((float)rds16(m, n + 0x34 + 2u * (uint32_t)k) * scale / 4096.0f, 0, 255) / 255.0f;
            }
        } else if (code == NSFX_AK_FADE) {
            NsFxFade *f = &sc->fade; int mode = rds32(m, n + 0x34), v = rds32(m, n + 0x30);
            float amt = clampf((float)v / 255.0f, 0, 1);
            if (f->actor && f->amount >= amt) continue;
            f->actor = n; f->white = (mode & 2) != 0; f->fade_in = (mode & 1) != 0; f->amount = amt; f->active = amt > 0.0f;
        }
    }
    free(ar);
}

/* ------------------------------------------------------------------------------------------------ extracción */
static void run_buffer(Ctx *c, const ActorRec *ar, int na) {
    NsFxScene *S = c->sc;
    S->n_items = 0; S->n_shadow_objs = 0; memset(&S->stats, 0, sizeof S->stats); S->buffer = c->buf;
    for (int i = 0; i < c->nq; i++) do_prim(c, c->qprim[i], c->qrole[i]);
    for (int i = 0; i < na; i++) if (code_of_hash(ar[i].hash) == NSFX_ROLE_SHADOW_OBJ) do_shadow(c, ar[i].node);
}
static int score(const NsFxScene *S) { return (int)S->stats.chk_n - 2 * (int)S->stats.chk_bad; }
typedef struct { uint32_t addr; int k; } PA;
static int pa_cmp(const void *a, const void *b) { uint32_t x = ((const PA *)a)->addr, y = ((const PA *)b)->addr; return x < y ? -1 : x > y; }

int ns_fx_extract(const uint8_t *ram, const NsFxOpts *opt_in, NsFxScene *out) {
    if (!ram || !out) return -1;
    NsFxOpts opt = opt_in ? *opt_in : ns_fx_opts_default();
    if (opt.tune.y_scale <= 0) opt.tune.y_scale = ns_fx_tuning_default().y_scale;
    out->n_items = 0; out->n_shadow_objs = 0; memset(&out->stats, 0, sizeof out->stats);
    memset(out->pm_key, 0, sizeof(uint64_t) * (out->pm_mask + 1u));
    Ctx *c = (Ctx *)calloc(1, sizeof *c);
    if (!c) return -1;
    c->ram = ram; c->opt = &opt; c->sc = out;
    c->map = rdu32(ram, NSFX_A_MAP); out->cur_map = c->map;
    read_screen(c);
    if (read_cam(ram, opt.tune.y_scale, &out->cam) < 0) { free(c); return -2; }
    uint32_t ch = NSFX_A_CHANL;
    int qs = rds16(ram, ch + 0x52), pi = rds16(ram, ch + 0x54); uint32_t q = rdu32(ram, ch + 0x58);
    if (!ptr_ok(q) || pi < 0 || qs < pi || qs > 1024) { out->cam.valid = 0; free(c); return -2; }
    for (int i = pi; i < qs && c->nq < 1024; i++) { uint32_t p = rdu32(ram, q + 4u * (uint32_t)i); if (ptr_ok(p) && in_ram(p, 0x54)) { c->qprim[c->nq] = p; c->qrole[c->nq] = 0; c->nq++; } }
    ActorRec *ar = (ActorRec *)calloc(MAX_ACTORS, sizeof *ar); int na = 0;
    if (ar) na = walk_actors(ram, ar, MAX_ACTORS);
    if (opt.owner_roles && c->nq) {
        PA *pa = (PA *)malloc(sizeof(PA) * (size_t)c->nq);
        if (pa) {
            for (int k = 0; k < c->nq; k++) { pa[k].addr = c->qprim[k]; pa[k].k = k; }
            qsort(pa, (size_t)c->nq, sizeof(PA), pa_cmp);
            for (int i = 0; i < na; i++) {
                int role = code_of_hash(ar[i].hash);
                if (role <= 0 || role >= NSFX_ROLE_COUNT || role == NSFX_ROLE_SHADOW_OBJ) continue;
                for (uint32_t o = 0x20; o < 0x200; o += 4) {   /* ventana acotada de la estructura de trabajo del actor */
                    uint32_t w = rdu32(ram, ar[i].node + o);
                    if (!ptr_ok(w)) continue;
                    int lo = 0, hi = c->nq - 1;
                    while (lo <= hi) {
                        int mid = (lo + hi) / 2;
                        if (pa[mid].addr == w) { if (!c->qrole[pa[mid].k]) c->qrole[pa[mid].k] = (uint8_t)role; break; }
                        if (pa[mid].addr < w) lo = mid + 1; else hi = mid - 1;
                    }
                }
            }
            free(pa);
        }
    }
    int clock = (int)(rdu32(ram, NSFX_A_CLOCK) & 1u);
    c->buf = opt.buffer >= 0 ? (opt.buffer & 1) : clock;
    run_buffer(c, ar, na);
    if (opt.buffer < 0 && (out->stats.chk_n == 0 || out->stats.chk_bad * 10 > out->stats.chk_n)) {
        int s0 = score(out);
        NsFxItem *keep = (NsFxItem *)malloc(sizeof(NsFxItem) * (out->n_items ? out->n_items : 1));
        if (keep) {
            uint32_t n0 = out->n_items; memcpy(keep, out->items, sizeof(NsFxItem) * n0);
            uint32_t sh0[NSFX_MAX_SHADOW_OBJS]; uint32_t nsh0 = out->n_shadow_objs; memcpy(sh0, out->shadow_objs, sizeof sh0);
            __typeof__(out->stats) st0 = out->stats;
            c->buf = clock ^ 1; run_buffer(c, ar, na);
            if (score(out) <= s0) { memcpy(out->items, keep, sizeof(NsFxItem) * n0); out->n_items = n0; memcpy(out->shadow_objs, sh0, sizeof sh0); out->n_shadow_objs = nsh0; out->stats = st0; out->buffer = clock; }
            free(keep);
        }
    }
    pm_build(out);
    free(ar); free(c);
    return 0;
}

/* ------------------------------------------------------------------------------------------------ correspondencia mundo -> VR */
NsFxXform ns_fx_xform_default(void) {
    NsFxXform x; memset(&x, 0, sizeof x);
    x.scale = 0.001f; x.mirror_z = -1; x.user_k = 1.0f;
    x.user_R[0] = x.user_R[4] = x.user_R[8] = 1.0f;
    return x;
}
static int mirror_of(const NsFxXform *x, const NsFxCam *cam) { return x->mirror_z < 0 ? (cam ? cam->det_neg : 0) : x->mirror_z; }
void ns_fx_to_vr(const NsFxXform *x, const NsFxCam *cam, const float w[3], float out[3]) {
    float p[3] = {(w[0] - x->origin[0]) * x->scale, (w[1] - x->origin[1]) * x->scale, (w[2] - x->origin[2]) * x->scale};
    if (mirror_of(x, cam)) p[2] = -p[2];
    if (!x->user_map_on) { out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; return; }
    float q[3] = {p[0] * x->user_k, p[1] * x->user_k, p[2] * x->user_k};
    for (int r = 0; r < 3; r++) out[r] = x->user_R[3 * r] * q[0] + x->user_R[3 * r + 1] * q[1] + x->user_R[3 * r + 2] * q[2] - x->user_R[3 * r + 1] * x->user_eye_h_m + x->user_p_ref[r];
}
void ns_fx_dir_to_vr(const NsFxXform *x, const NsFxCam *cam, const float d[3], float out[3]) {
    float p[3] = {d[0], d[1], d[2]};
    if (mirror_of(x, cam)) p[2] = -p[2];
    if (!x->user_map_on) { out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; return; }
    for (int r = 0; r < 3; r++) out[r] = x->user_k * (x->user_R[3 * r] * p[0] + x->user_R[3 * r + 1] * p[1] + x->user_R[3 * r + 2] * p[2]);
}
float ns_fx_len_scale(const NsFxXform *x) { return x->scale * (x->user_map_on ? x->user_k : 1.0f); }

void ns_fx_billboard_corners(const NsFxItem *it, const NsFxXform *x, const NsFxCam *cam, const float right[3], const float up[3], float out[4][3]) {
    float c[3]; ns_fx_to_vr(x, cam, it->pos[0], c);
    float L = ns_fx_len_scale(x);
    const float ox[4] = {it->off[0], it->off[2], it->off[0], it->off[2]}, oy[4] = {it->off[1], it->off[1], it->off[3], it->off[3]};
    for (int k = 0; k < 4; k++) for (int r = 0; r < 3; r++) out[k][r] = c[r] + right[r] * ox[k] * L - up[r] * oy[k] * L;
}
int ns_fx_ribbon_corners(const NsFxItem *it, const NsFxXform *x, const NsFxCam *cam, const float head[3], float out[4][3]) {
    float a[3], b[3]; ns_fx_to_vr(x, cam, it->pos[0], a); ns_fx_to_vr(x, cam, it->pos[1], b);
    float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {head[0] - 0.5f * (a[0] + b[0]), head[1] - 0.5f * (a[1] + b[1]), head[2] - 0.5f * (a[2] + b[2])}, s[3];
    cross3(d, v, s);
    float l = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (l < 1e-9f) return 0;
    float L = ns_fx_len_scale(x), h0 = 0.5f * it->width[0] * L / l, h1 = 0.5f * it->width[1] * L / l;
    for (int r = 0; r < 3; r++) { out[0][r] = a[r] - s[r] * h0; out[1][r] = a[r] + s[r] * h0; out[2][r] = b[r] - s[r] * h1; out[3][r] = b[r] + s[r] * h1; }
    return 1;
}
int ns_fx_item_faces(const NsFxItem *it, const float eye[3]) {
    if (!(it->flags & NSFX_F_ONESIDE)) return 1;
    return it->fn[0] * (it->pos[0][0] - eye[0]) + it->fn[1] * (it->pos[0][1] - eye[1]) + it->fn[2] * (it->pos[0][2] - eye[2]) > 0;
}
static const char *const ROLE_NAMES[NSFX_ROLE_COUNT] = {"none", "light", "bubble", "smoke", "spark", "droplet", "splash", "ripple", "tracer", "beam", "decal", "shadow", "shadow_obj", "plasma", "surface",
    "water_surface", "ambient", "cone", "trail", "strip", "particle", "sprite_anim", "view_overlay"};
const char *ns_fx_role_name(int r) { return r >= 0 && r < NSFX_ROLE_COUNT ? ROLE_NAMES[r] : "?"; }
const char *ns_fx_shape_name(int s) { static const char *const n[5] = {"none", "billboard", "quad", "tri", "ribbon"}; return s >= 0 && s < 5 ? n[s] : "?"; }

/* ------------------------------------------------------------------------------------------------ clasificación de la lista de la GPU */
enum { PRIO_2D = 100, PRIO_FX = 3, PRIO_MESH = 2, PRIO_PACK = 1 };
typedef struct { uint32_t next; int32_t item; uint8_t cls, kc, nv, textured, semi, tp, u0, v0, rgb[3]; int16_t x[4], y[4], w, h; } Ent;
typedef struct { Ent *e; uint32_t n, cap, max; uint32_t *head; uint32_t mask; int overflow, chain_max; } Idx;

static uint32_t mix(uint32_t h, uint32_t v) { h ^= v + 0x9E3779B9u + (h << 6) + (h >> 2); return h; }
static uint32_t hkey(int kc, int nv, int x0, int y0, int x1, int y1) {
    uint32_t h = (uint32_t)kc * 31u + (uint32_t)nv;
    h = mix(h, (uint32_t)(x0 & 0xFFFF)); h = mix(h, (uint32_t)(y0 & 0xFFFF)); h = mix(h, (uint32_t)(x1 & 0xFFFF)); h = mix(h, (uint32_t)(y1 & 0xFFFF));
    return h;
}
static uint32_t ent_key(const Ent *e) { return e->kc == C_RECT ? hkey(e->kc, 0, e->x[0], e->y[0], e->w, e->h) : hkey(e->kc, e->nv, e->x[0], e->y[0], e->nv > 1 ? e->x[1] : 0, e->nv > 1 ? e->y[1] : 0); }
static int idx_init(Idx *ix, uint32_t hint, const NsFxTuning *t) {
    memset(ix, 0, sizeof *ix);
    ix->max = t->idx_max ? t->idx_max : 65536u; ix->chain_max = t->chain_max > 0 ? t->chain_max : 32;
    ix->cap = hint < 64 ? 64 : hint; if (ix->cap > ix->max) ix->cap = ix->max;
    uint32_t hs = 1; while (hs < ix->max * 2u) hs <<= 1;
    ix->e = (Ent *)malloc(sizeof(Ent) * ix->cap); ix->head = (uint32_t *)malloc(sizeof(uint32_t) * hs); ix->mask = hs - 1;
    if (!ix->e || !ix->head) return -1;
    memset(ix->head, 0xFF, sizeof(uint32_t) * hs);
    return 0;
}
static void idx_free(Idx *ix) { free(ix->e); free(ix->head); }
static void idx_put(Idx *ix, const Ent *src) {
    if (ix->overflow) return;
    if (ix->n >= ix->max) { ix->overflow = 1; return; }
    if (ix->n >= ix->cap) { uint32_t nc = ix->cap * 2; if (nc > ix->max) nc = ix->max; Ent *ne = (Ent *)realloc(ix->e, sizeof(Ent) * nc); if (!ne) { ix->overflow = 1; return; } ix->e = ne; ix->cap = nc; }
    Ent *e = &ix->e[ix->n]; *e = *src;
    uint32_t k = ent_key(e) & ix->mask, len = 0;
    for (uint32_t i = ix->head[k]; i != 0xFFFFFFFFu; i = ix->e[i].next) if (++len > (uint32_t)ix->chain_max) { ix->overflow = 1; return; }
    e->next = ix->head[k]; ix->head[k] = ix->n; ix->n++;
}
/* Entrada desde un paquete decodificado; las líneas multi-segmento se parten en segmentos de 2 vértices. */
static void ent_put_dec(Idx *ix, const Dec *d, int cls, int item, int tp) {
    Ent e; memset(&e, 0, sizeof e);
    e.cls = (uint8_t)cls; e.item = item; e.textured = (uint8_t)d->textured; e.semi = (uint8_t)d->semi; e.tp = (uint8_t)tp; e.u0 = d->u[0]; e.v0 = d->v[0];
    memcpy(e.rgb, d->rgb[0], 3);
    if (d->cls == C_LINE) {
        for (int s = 0; s + 1 < d->nv; s++) { e.kc = C_LINE; e.nv = 2; e.x[0] = (int16_t)d->x[s]; e.y[0] = (int16_t)d->y[s]; e.x[1] = (int16_t)d->x[s + 1]; e.y[1] = (int16_t)d->y[s + 1]; memcpy(e.rgb, d->rgb[s], 3); idx_put(ix, &e); }
    } else if (d->cls == C_RECT) { e.kc = C_RECT; e.nv = 1; e.x[0] = (int16_t)d->x[0]; e.y[0] = (int16_t)d->y[0]; e.w = (int16_t)d->w; e.h = (int16_t)d->h; idx_put(ix, &e); }
    else if (d->nv >= 3) { e.kc = C_POLY; e.nv = (uint8_t)d->nv; for (int v = 0; v < d->nv; v++) { e.x[v] = (int16_t)d->x[v]; e.y[v] = (int16_t)d->y[v]; } idx_put(ix, &e); }
}
static void ent_put_item(Idx *ix, const NsFxItem *it, int i) {
    if (it->flags & (NSFX_F_ENGINE_CULLED | NSFX_F_LOW_CONF)) return;   /* sin paquete emitido: no hay operación que casar */
    Ent e; memset(&e, 0, sizeof e);
    e.cls = NSFX_OP_FX; e.item = i; e.kc = it->pcls; e.nv = it->pnv; e.textured = (it->flags & NSFX_F_TEXTURED) != 0; e.semi = (it->flags & NSFX_F_SEMI) != 0; e.tp = it->ptp;
    e.u0 = it->pu0; e.v0 = it->pv0; memcpy(e.rgb, it->rgb[0], 3); e.w = (int16_t)it->sw; e.h = (int16_t)it->sh;
    if (e.kc > C_POLY || e.nv < 1 || e.nv > 4) return;
    for (int v = 0; v < e.nv; v++) { e.x[v] = it->sxy[v][0]; e.y[v] = it->sxy[v][1]; }
    idx_put(ix, &e);
}
static int ent_match(const Ent *e, const GpuOp *o, int kc, int nv) {
    if (e->kc != kc || e->nv != nv) return 0;
    int dx = o->dox, dy = o->doy;
    if (kc == C_RECT) { if (e->w != o->rw || e->h != o->rh || e->x[0] != o->v[0].x - dx || e->y[0] != o->v[0].y - dy) return 0; }
    else for (int v = 0; v < nv; v++) if (e->x[v] != o->v[v].x - dx || e->y[v] != o->v[v].y - dy) return 0;
    if (e->semi != ((o->flags & GPU_F_SEMI) != 0)) return 0;
    if (e->textured != ((o->flags & GPU_F_TEXTURED) != 0)) return 0;
    if (e->textured && (e->u0 != o->v[0].u || e->v0 != o->v[0].v)) return 0;
    if (e->textured && e->tp != 255 && e->tp != o->tp) return 0;
    if (e->rgb[0] != o->v[0].r || e->rgb[1] != o->v[0].g || e->rgb[2] != o->v[0].b) return 0;
    return 1;
}
/* Devuelve la clase elegida (NSFX_OP_*) o -1 si no casa. Gana 2D si casa cualquier entrada 2D; si no FX > MESH > FX_PACK. Cadena demasiado larga: 2D. */
static int idx_find(const Idx *ix, const GpuOp *o, int32_t *item) {
    int kc, nv = o->nv;
    if (o->type == GPU_OP_POLY) { kc = C_POLY; if (nv < 3 || nv > 4) return -1; }
    else if (o->type == GPU_OP_LINE) { kc = C_LINE; nv = 2; }
    else if (o->type == GPU_OP_RECT) { kc = C_RECT; nv = 1; }
    else return -1;
    int x[4], y[4];
    for (int i = 0; i < nv; i++) { x[i] = o->v[i].x - o->dox; y[i] = o->v[i].y - o->doy; }
    uint32_t k = (kc == C_RECT ? hkey(kc, 0, x[0], y[0], o->rw, o->rh) : hkey(kc, nv, x[0], y[0], nv > 1 ? x[1] : 0, nv > 1 ? y[1] : 0)) & ix->mask;
    int best = -1, bprio = -1; int32_t bitem = -1; int steps = 0;
    for (uint32_t i = ix->head[k]; i != 0xFFFFFFFFu; i = ix->e[i].next) {
        if (++steps > ix->chain_max) return NSFX_OP_2D;
        const Ent *e = &ix->e[i];
        if (!ent_match(e, o, kc, nv)) continue;
        int pr = e->cls == NSFX_OP_2D ? PRIO_2D : e->cls == NSFX_OP_FX ? PRIO_FX : e->cls == NSFX_OP_MESH ? PRIO_MESH : PRIO_PACK;
        if (pr > bprio) { bprio = pr; best = e->cls; bitem = e->item; }
    }
    if (item) *item = bitem;
    return best;
}

static void op_bbox(const GpuOp *o, int *x0, int *y0, int *x1, int *y1) {
    if (o->type == GPU_OP_RECT) { *x0 = o->v[0].x; *y0 = o->v[0].y; *x1 = *x0 + o->rw; *y1 = *y0 + o->rh; return; }
    int n = o->type == GPU_OP_LINE ? 2 : (o->nv > 4 ? 4 : o->nv); *x0 = *y0 = 99999; *x1 = *y1 = -99999;
    for (int i = 0; i < n; i++) { if (o->v[i].x < *x0) *x0 = o->v[i].x; if (o->v[i].x > *x1) *x1 = o->v[i].x; if (o->v[i].y < *y0) *y0 = o->v[i].y; if (o->v[i].y > *y1) *y1 = o->v[i].y; }
}
/* La textura de 15 bits sale del área de dibujo vigente (relectura del búfer de imagen). */
static int reads_draw_area(const GpuOp *o) {
    if (!(o->flags & GPU_F_TEXTURED) || o->tp < 2) return 0;
    int u0 = 255, v0 = 255, u1 = 0, v1 = 0;
    if (o->type == GPU_OP_RECT) { u0 = o->v[0].u; v0 = o->v[0].v; u1 = u0 + o->rw; v1 = v0 + o->rh; }
    else if (o->type == GPU_OP_POLY && o->nv >= 3 && o->nv <= 4) for (int i = 0; i < o->nv; i++) { if (o->v[i].u < u0) u0 = o->v[i].u; if (o->v[i].u > u1) u1 = o->v[i].u; if (o->v[i].v < v0) v0 = o->v[i].v; if (o->v[i].v > v1) v1 = o->v[i].v; }
    else return 0;
    int sx0 = o->tx + u0, sy0 = o->ty + v0, sx1 = o->tx + u1, sy1 = o->ty + v1;
    return sx0 >= o->cx1 && sx1 <= o->cx2 && sy0 >= o->cy1 && sy1 <= o->cy2;
}
static int op_clean3d(const GpuOp *o) {
    if (o->type != GPU_OP_POLY || o->nv < 3 || o->nv > 4 || !(o->proj_h > 0)) return 0;
    for (int v = 0; v < o->nv; v++) if (!o->v[v].vvalid || o->v[v].vflags != 0) return 0;
    return 1;
}

/* Paquetes de objetos de malla emitidos del búfer vigente. Los objetos de sombra extraídos por ns_fx no se indexan aquí (ya van por sus elementos). */
static void idx_add_objs(Idx *ix, const uint8_t *m, const NsFxScene *sc) {
    uint32_t ch = NSFX_A_CHANL;
    int nobj = rds16(m, ch + 0x56); uint32_t q = rdu32(m, ch + 0x58);
    if (nobj < 0 || nobj > 2000 || !ptr_ok(q)) return;
    for (int i = 0; i < nobj && !ix->overflow; i++) {
        uint32_t o = rdu32(m, q + 4u * (uint32_t)i); if (!ptr_ok(o)) continue;
        int np = rds16(m, o + 0x2E); if (np < 0 || np > 64) continue;
        int skip = 0; for (uint32_t s = 0; s < sc->n_shadow_objs; s++) if (sc->shadow_objs[s] == o) skip = 1;
        if (skip) continue;
        for (int k = 0; k < np && !ix->overflow; k++) {
            uint32_t seg = o + 0x48u + 0x5Cu * (uint32_t)k;
            for (int g = 0; g < 8 && ptr_ok(seg) && !ix->overflow; g++) {
                int n = rds16(m, seg + 0x52);
                uint32_t pb = rdu32(m, seg + 0x54 + 4u * (uint32_t)sc->buffer);
                if (n > 0 && n <= 4000 && ptr_ok(pb) && in_ram(pb, 52u * (uint32_t)n)) {
                    for (int j = 0; j < n && !ix->overflow; j++) {
                        uint32_t a = pb + 52u * (uint32_t)j;
                        if (!rdu16(m, a)) continue;
                        int code = (int)rd8(m, a + 7);
                        if (!(code & 0x20)) continue;
                        Dec d;
                        dec_pack(m, a, KIND_P4TG, 4, 8, 12, (code & 8) ? 4 : 3, &d);
                        d.textured = (code & 4) != 0; d.semi = (code & 2) != 0;
                        int tp = 255; if (d.textured) { uint32_t tpg = rdu16(m, a + 20 + 6); (void)tpg; tp = (int)((rdu16(m, a + 26) >> 7) & 3); }
                        ent_put_dec(ix, &d, NSFX_OP_MESH, -1, tp);
                    }
                }
                seg = rdu32(m, seg + 0x48);
            }
        }
    }
}
/* Paquetes de primitivas no extraídas del búfer vigente: de ordenación 2D y de formato libre -> 2D; invisibles/otro mapa -> FX_PACK. */
static void idx_add_prims(Idx *ix, const uint8_t *m, const NsFxScene *sc) {
    uint32_t ch = NSFX_A_CHANL;
    int qs = rds16(m, ch + 0x52), pi = rds16(m, ch + 0x54); uint32_t q = rdu32(m, ch + 0x58);
    if (!ptr_ok(q) || pi < 0 || qs < pi || qs > 1024) return;
    for (int qi = pi; qi < qs && !ix->overflow; qi++) {
        uint32_t p = rdu32(m, q + 4u * (uint32_t)qi); if (!ptr_ok(p) || !in_ram(p, 0x54)) continue;
        int type = rds32(m, p + 0x24), kind = type & 0x1F, n = rds16(m, p + 0x2A);
        uint32_t group = rdu16(m, p + 0x28);
        int psz = rds16(m, p + 0x30), verts = rds16(m, p + 0x32), voff = rds16(m, p + 0x34), vst = rds16(m, p + 0x36);
        if (n <= 0 || n > 8192 || psz < 8 || psz > 64 || verts < 1 || verts > 4 || kind > 23) continue;
        uint32_t pk = rdu32(m, p + 0x40 + 4u * (uint32_t)sc->buffer);
        if (!ptr_ok(pk) || !in_ram(pk, (uint32_t)n * (uint32_t)psz)) continue;
        int sortonly = (type & T_SORTONLY) != 0, hidden = (type & T_INVISIBLE) || (group && !(group & sc->cur_map));
        for (int j = 0; j < n && !ix->overflow; j++) {
            uint32_t b = pk + (uint32_t)j * (uint32_t)psz;
            if (!rdu16(m, b)) continue;
            if (!sortonly && pm_find(sc, p, (uint32_t)j) >= 0) continue;   /* ya indexado desde el elemento */
            int cls = sortonly ? NSFX_OP_2D : kind == KIND_FREE ? NSFX_OP_2D : hidden ? NSFX_OP_FX_PACK : NSFX_OP_2D;   /* visible y sin elemento: la GPU lo dibuja */
            Dec d;
            if (kind == KIND_FREE) {   /* formato libre: se asume una lista de vértices xy con los parámetros de la primitiva */
                memset(&d, 0, sizeof d);
                d.cls = verts == 2 ? C_LINE : C_POLY; d.nv = verts; d.sz = (int)rdu16(m, b); d.code = (uint8_t)rd8(m, b + 7); d.semi = (d.code & 2) != 0;
                if (verts < 2) continue;
                for (int v = 0; v < verts; v++) { d.x[v] = rds16(m, b + (uint32_t)(voff + vst * v)); d.y[v] = rds16(m, b + (uint32_t)(voff + vst * v) + 2); }
                for (int c = 0; c < 3; c++) d.rgb[0][c] = (uint8_t)rd8(m, b + 4u + (uint32_t)c);
            } else if (dec_pack(m, b, kind, verts, voff, vst, (kind == KIND_S2T || kind == KIND_S2TG) ? 4 : 0, &d) < 0) continue;
            ent_put_dec(ix, &d, cls, -1, (d.textured && kind >= KIND_P3T) ? (int)((d.tpage >> 7) & 3) : 255);
        }
    }
}

int ns_fx_classify(const uint8_t *ram, const NsFxScene *sc, const GpuDrawList *dl, uint8_t *cls, uint8_t *sub, int32_t *item, NsFxClsStats *st) {
    if (!sc || !dl) return -1;
    NsFxClsStats z; memset(&z, 0, sizeof z); NsFxClsStats *S = st ? st : &z; memset(S, 0, sizeof *S);
    NsFxTuning T = ns_fx_tuning_default();
    uint32_t nops = dl->nops, cap = nops ? nops : 1;
    Idx ix; if (idx_init(&ix, sc->n_items * 2u + 256u, &T) < 0) { idx_free(&ix); return -1; }
    uint8_t *hit = (uint8_t *)calloc(sc->n_items ? sc->n_items : 1, 1);
    uint8_t *lc = (uint8_t *)calloc(cap, 2); uint8_t *ls = lc ? lc + cap : NULL;
    int32_t *li = (int32_t *)calloc(cap, 4);
    if (!hit || !lc || !li) { free(hit); free(lc); free(li); idx_free(&ix); return -1; }
    for (uint32_t i = 0; i < sc->n_items; i++) ent_put_item(&ix, &sc->items[i], (int)i);
    if (ram) { idx_add_prims(&ix, ram, sc); idx_add_objs(&ix, ram, sc); }
    S->idx_overflow = (uint32_t)ix.overflow;
    for (uint32_t i = 0; i < sc->n_items; i++) { const NsFxItem *it = &sc->items[i]; if (!(it->flags & (NSFX_F_ENGINE_CULLED | NSFX_F_LOW_CONF))) S->n_fx_items_vis++; }
    const NsFxScreen *scr = &sc->screen;
    int water_on = scr->water.active, cover_on = scr->fade.active || scr->flash || scr->mode_a || scr->mode_b, scan_on = scr->mode_a || scr->mode_b, gas_on = scr->gas.active;
    int ring_n = 0; double rcx = 0, rcy = 0; int quad[4] = {0, 0, 0, 0};
    const GpuDisplay *dsp = &dl->disp;
    double dw = dsp->w > 0 ? dsp->w : 320, dh = dsp->h > 0 ? dsp->h : 240;
    for (uint32_t i = 0; i < nops; i++) {
        const GpuOp *o = &dl->ops[i];
        uint8_t c = NSFX_OP_2D, sc_sub = NSFX_SC_NONE; int32_t it = -1;
        S->n_ops++;
        if (o->type == GPU_OP_FILL || o->type == GPU_OP_VCOPY || o->type == GPU_OP_UPLOAD || o->type == GPU_OP_DOWNLOAD) { c = NSFX_OP_EVENT; goto done; }
        if (ix.overflow) goto done;   /* índice incompleto: todo 2D */
        {
            int32_t mi = -1; int r = idx_find(&ix, o, &mi);
            if (r == NSFX_OP_FX || r == NSFX_OP_MESH) { c = (uint8_t)r; it = mi; goto done; }
            if (r == NSFX_OP_FX_PACK) { if (op_clean3d(o)) c = NSFX_OP_FX_PACK; goto done; }
            if (r == NSFX_OP_2D) goto done;
        }
        {
            int x0, y0, x1, y1; op_bbox(o, &x0, &y0, &x1, &y1);
            double bw = x1 - x0, bh = y1 - y0;
            int tex = (o->flags & GPU_F_TEXTURED) != 0, semi = (o->flags & GPU_F_SEMI) != 0;
            if (water_on && reads_draw_area(o)) { c = NSFX_OP_SCREEN; sc_sub = NSFX_SC_COPY; goto done; }
            if (o->type != GPU_OP_LINE && !tex) {
                if (cover_on && semi && bw >= 0.85 * dw && bh >= 0.85 * dh) { c = NSFX_OP_SCREEN; sc_sub = NSFX_SC_COVER; goto done; }
                if (scan_on && bw >= 0.9 * dw && bh >= 0 && bh <= 3) { c = NSFX_OP_SCREEN; sc_sub = NSFX_SC_SCAN; goto done; }
                if (water_on && !semi && !(o->flags & GPU_F_GOURAUD) && bw >= 0.9 * dw && bh >= 2 && bh <= 0.2 * dh && (y0 <= o->cy1 + 1 || y1 >= o->cy2 - 1)) { c = NSFX_OP_SCREEN; sc_sub = NSFX_SC_BARS; goto done; }
            }
            if (op_clean3d(o)) { c = NSFX_OP_MESH_GUESS; goto done; }
        }
    done:
        lc[i] = c; ls[i] = sc_sub; li[i] = it;
        if (c == NSFX_OP_FX && it >= 0 && (uint32_t)it < sc->n_items) hit[it] = 1;
        /* candidatos a anillo (se confirman después con el recuento y la simetría) */
        if (gas_on && c == NSFX_OP_2D && o->type == GPU_OP_POLY && o->nv >= 3 && o->nv <= 4 && (o->flags & GPU_F_SEMI) && (o->flags & GPU_F_GOURAUD) && !(o->flags & GPU_F_TEXTURED)) {
            int x0, y0, x1, y1; op_bbox(o, &x0, &y0, &x1, &y1);
            if ((x1 - x0) < 0.6 * dw && (y1 - y0) < 0.6 * dh) {
                ring_n++; double cx = 0.5 * (x0 + x1) - o->dox, cy = 0.5 * (y0 + y1) - o->doy; rcx += cx; rcy += cy;
                quad[(cx >= 0) + 2 * (cy >= 0)]++;
            }
        }
    }
    if (gas_on && ring_n == T.ring_segments) {
        double mx = rcx / ring_n, my = rcy / ring_n; int ok = fabs(mx) < 0.1 * dw && fabs(my) < 0.1 * dh;
        for (int k = 0; k < 4; k++) if (quad[k] < ring_n / 8) ok = 0;
        if (ok) for (uint32_t i = 0; i < nops; i++) {
            const GpuOp *o = &dl->ops[i];
            if (lc[i] != NSFX_OP_2D || o->type != GPU_OP_POLY || o->nv < 3 || o->nv > 4 || !(o->flags & GPU_F_SEMI) || !(o->flags & GPU_F_GOURAUD) || (o->flags & GPU_F_TEXTURED)) continue;
            int x0, y0, x1, y1; op_bbox(o, &x0, &y0, &x1, &y1);
            if ((x1 - x0) < 0.6 * dw && (y1 - y0) < 0.6 * dh) { lc[i] = NSFX_OP_SCREEN; ls[i] = NSFX_SC_RING; }
        }
    }
    for (uint32_t i = 0; i < nops; i++) {
        switch (lc[i]) {
        case NSFX_OP_EVENT: S->n_event++; break; case NSFX_OP_2D: S->n_2d++; break; case NSFX_OP_MESH: S->n_mesh++; break;
        case NSFX_OP_MESH_GUESS: S->n_mesh_guess++; break; case NSFX_OP_FX: S->n_fx++; break; case NSFX_OP_FX_PACK: S->n_fx_pack++; break;
        case NSFX_OP_SCREEN: S->n_screen++; S->sub[ls[i] & 7]++; break; default: S->n_maint++; break;
        }
    }
    for (uint32_t i = 0; i < sc->n_items; i++) {
        if (hit[i]) S->n_fx_items_hit++;
        const NsFxItem *it = &sc->items[i];
        if (!hit[i] && !(it->flags & (NSFX_F_ENGINE_CULLED | NSFX_F_LOW_CONF))) S->n_fx_items_missing++;
    }
    if (cls) memcpy(cls, lc, nops);
    if (sub) memcpy(sub, ls, nops);
    if (item) memcpy(item, li, sizeof(int32_t) * nops);
    free(hit); free(lc); free(li); idx_free(&ix);
    return 0;
}

uint64_t ns_fx_item_key(const NsFxItem *it) {
    return ((uint64_t)it->prim << 32) | ((uint64_t)it->idx << 8) | (uint64_t)(it->sub & 0xFF);
}

typedef struct { uint32_t sig; uint32_t i; } NearSig;
static int near_cmp(const void *a, const void *b) {
    const NearSig *x = (const NearSig *)a, *y = (const NearSig *)b;
    if (x->sig != y->sig) return x->sig < y->sig ? -1 : 1;
    return x->i < y->i ? -1 : x->i > y->i;
}
static uint32_t near_sig(uint16_t tpage, uint16_t clut, int textured, int semi, int nv) {
    uint32_t h = (uint32_t)(tpage & 0x19Fu) | ((uint32_t)clut << 9);
    return mix(mix(h, (uint32_t)(textured | (semi << 1) | (nv << 2))), 0x51u);
}
uint32_t ns_fx_classify_near(const NsFxScene *sc, const NsFxListCam *lc, const GpuDrawList *dl, uint8_t *cls, int32_t *item) {
    if (!sc || !dl || !cls || !item || !sc->n_items || !dl->nops) return 0;
    uint32_t cand = 0;
    for (uint32_t i = 0; i < sc->n_items; i++) { const NsFxItem *it = &sc->items[i]; if (it->shape == NSFX_SH_QUAD || it->shape == NSFX_SH_TRI) cand++; }
    if (!cand) return 0;
    NearSig *tab = (NearSig *)malloc(sizeof(NearSig) * cand);
    float (*vl)[4][3] = (float (*)[4][3])malloc(sizeof(float) * 12 * sc->n_items);   /* vértices de cada elemento en la vista de la lista */
    if (!tab || !vl) { free(tab); free(vl); return 0; }
    uint32_t nt = 0;
    const float ys = sc->cam.y_scale > 0 ? sc->cam.y_scale : 1.0f;
    for (uint32_t i = 0; i < sc->n_items; i++) {
        const NsFxItem *it = &sc->items[i];
        if (it->shape != NSFX_SH_QUAD && it->shape != NSFX_SH_TRI) continue;
        int nv = it->shape == NSFX_SH_TRI ? 3 : 4;
        for (int v = 0; v < nv; v++) {
            if (lc) {
                float d[3] = {it->pos[v][0] - lc->eye[0], it->pos[v][1] - lc->eye[1], it->pos[v][2] - lc->eye[2]};
                for (int r = 0; r < 3; r++) vl[i][v][r] = lc->M[r] * d[0] + lc->M[3 + r] * d[1] + lc->M[6 + r] * d[2];   /* M^T d */
                vl[i][v][1] *= ys;
            } else for (int r = 0; r < 3; r++) vl[i][v][r] = it->vpos[v][r];
        }
        tab[nt].sig = near_sig(it->tpage, it->clut, (it->flags & NSFX_F_TEXTURED) != 0, (it->flags & NSFX_F_SEMI) != 0, nv); tab[nt].i = i; nt++;
    }
    qsort(tab, nt, sizeof *tab, near_cmp);
    uint32_t changed = 0;
    for (uint32_t oi = 0; oi < dl->nops; oi++) {
        const GpuOp *o = &dl->ops[oi];
        if ((cls[oi] != NSFX_OP_2D && cls[oi] != NSFX_OP_MESH_GUESS) || o->type != GPU_OP_POLY || o->nv < 3 || o->nv > 4) continue;
        int ok = 1; float zs = 0;
        for (int v = 0; v < o->nv; v++) { if (!o->v[v].vvalid) { ok = 0; break; } zs += o->v[v].vz; }
        if (!ok) continue;
        zs /= (float)o->nv;
        const int tex = (o->flags & GPU_F_TEXTURED) != 0, semi = (o->flags & GPU_F_SEMI) != 0;
        uint16_t page = (uint16_t)(((o->tx / 64) & 15) | (((o->ty / 256) & 1) << 4) | ((o->tp & 3) << 7));
        uint16_t clut = tex ? (uint16_t)(((o->clx / 16) & 63) | ((o->cly & 511) << 6)) : 0;
        uint32_t sig = near_sig(tex ? page : 0, clut, tex, semi, o->nv);
        uint32_t lo = 0, hi = nt;
        while (lo < hi) { uint32_t mid = (lo + hi) / 2; if (tab[mid].sig < sig) lo = mid + 1; else hi = mid; }
        const float tol = 40.0f + 0.03f * (zs > 0 ? zs : 0);
        float bestd = 1e30f; int32_t best = -1;
        for (uint32_t k = lo; k < nt && tab[k].sig == sig; k++) {
            const NsFxItem *it = &sc->items[tab[k].i];
            if (tex && (it->uv[0][0] != o->v[0].u || it->uv[0][1] != o->v[0].v)) continue;
            if (semi && it->abr != (o->abr & 3) && !(it->flags & NSFX_F_ABR_UNKNOWN)) continue;
            float worst = 0; int nchk = 0;
            for (int v = 0; v < o->nv && worst <= tol; v++) {
                if (o->v[v].vflags & GPU_VF_AMBIG) continue;   /* asociación dudosa con la GTE: no sirve para comparar */
                nchk++;
                float dx = vl[tab[k].i][v][0] - o->v[v].vx, dy = vl[tab[k].i][v][1] - o->v[v].vy, dz = vl[tab[k].i][v][2] - o->v[v].vz;
                float d = sqrtf(dx * dx + dy * dy + dz * dz);
                if (d > worst) worst = d;
            }
            if (nchk >= 2 && worst <= tol && worst < bestd) { bestd = worst; best = (int32_t)tab[k].i; }
        }
        if (best >= 0) { cls[oi] = NSFX_OP_FX; item[oi] = best; changed++; }
    }
    /* Segunda pasada, por FAMILIA: polígonos texturizados semitransparentes con vista válida cuya firma completa (página, CLUT, u/v0, modo de mezcla, nº de vértices)
     * coincide con elementos que ninguna otra operación ha reclamado (ondas, anillos: se mueven más que la tolerancia entre el paquete de la RAM y el de la lista, o
     * tienen vértices recortados/saturados que no sirven para medir). Se asigna el elemento libre cuyo centro queda más cerca en la vista de la lista; el nº de
     * operaciones asignadas nunca supera el de elementos libres. El HUD no tiene posición de vista, así que no puede entrar aquí. */
    uint8_t *used = (uint8_t *)calloc(sc->n_items, 1);
    if (used) {
        for (uint32_t oi = 0; oi < dl->nops; oi++) if (cls[oi] == NSFX_OP_FX && item[oi] >= 0 && (uint32_t)item[oi] < sc->n_items) used[item[oi]] = 1;
        for (uint32_t oi = 0; oi < dl->nops; oi++) {
            const GpuOp *o = &dl->ops[oi];
            if (cls[oi] != NSFX_OP_2D || o->type != GPU_OP_POLY || o->nv < 3 || o->nv > 4) continue;
            if (!(o->flags & GPU_F_TEXTURED) || !(o->flags & GPU_F_SEMI)) continue;
            int ok = 1; float cx[3] = {0, 0, 0};
            for (int v = 0; v < o->nv; v++) { if (!o->v[v].vvalid) { ok = 0; break; } cx[0] += o->v[v].vx; cx[1] += o->v[v].vy; cx[2] += o->v[v].vz; }
            if (!ok) continue;
            for (int r = 0; r < 3; r++) cx[r] /= (float)o->nv;
            uint16_t page = (uint16_t)(((o->tx / 64) & 15) | (((o->ty / 256) & 1) << 4) | ((o->tp & 3) << 7));
            uint16_t clut = (uint16_t)(((o->clx / 16) & 63) | ((o->cly & 511) << 6));
            uint32_t sig = near_sig(page, clut, 1, 1, o->nv);
            uint32_t lo = 0, hi = nt;
            while (lo < hi) { uint32_t mid = (lo + hi) / 2; if (tab[mid].sig < sig) lo = mid + 1; else hi = mid; }
            float bestd = 1e30f; int32_t best = -1;
            for (uint32_t k = lo; k < nt && tab[k].sig == sig; k++) {
                uint32_t ii = tab[k].i; const NsFxItem *it = &sc->items[ii];
                if (used[ii] || it->uv[0][0] != o->v[0].u || it->uv[0][1] != o->v[0].v) continue;
                if (it->abr != (o->abr & 3) && !(it->flags & NSFX_F_ABR_UNKNOWN)) continue;
                float c[3] = {0, 0, 0};
                int nv = it->shape == NSFX_SH_TRI ? 3 : 4;
                for (int v = 0; v < nv; v++) for (int r = 0; r < 3; r++) c[r] += vl[ii][v][r] / (float)nv;
                float d = (c[0] - cx[0]) * (c[0] - cx[0]) + (c[1] - cx[1]) * (c[1] - cx[1]) + (c[2] - cx[2]) * (c[2] - cx[2]);
                if (d < bestd) { bestd = d; best = (int32_t)ii; }
            }
            if (best >= 0) { used[best] = 1; cls[oi] = NSFX_OP_FX; item[oi] = best; changed++; }
        }
        free(used);
    }
    free(tab); free(vl);
    return changed;
}

uint32_t ns_fx_gpu_mask(const uint8_t *cls, const uint8_t *confirmed, uint32_t n, uint8_t *gpu) {
    uint32_t k = 0;
    if (!cls || !gpu) return 0;
    for (uint32_t i = 0; i < n; i++) {
        int cand = cls[i] == NSFX_OP_MESH || cls[i] == NSFX_OP_MESH_GUESS || cls[i] == NSFX_OP_FX || cls[i] == NSFX_OP_FX_PACK || cls[i] == NSFX_OP_SCREEN;
        gpu[i] = (uint8_t)(!(cand && confirmed && confirmed[i]));
        k += gpu[i];
    }
    return k;
}

int ns_fx_resolve_items(NsFxScene *sc, const GpuDrawList *dl, const int32_t *item) {
    int n = 0;
    if (!sc || !dl || !item) return 0;
    for (uint32_t i = 0; i < dl->nops; i++) {
        int32_t k = item[i]; if (k < 0 || (uint32_t)k >= sc->n_items) continue;
        NsFxItem *it = &sc->items[k]; const GpuOp *o = &dl->ops[i];
        if (!(it->flags & (NSFX_F_TPAGE_UNKNOWN | NSFX_F_ABR_UNKNOWN))) continue;
        if (it->flags & NSFX_F_TPAGE_UNKNOWN) {
            it->tp = o->tp;
            it->tpage = (uint16_t)((o->tx / 64) | ((o->ty / 256) << 4) | ((o->abr & 3) << 5) | ((o->tp & 3) << 7));
            if (!(o->clx == 0 && o->cly == 0)) it->clut = (uint16_t)((o->clx / 16) | (o->cly << 6));
            it->flags &= (uint16_t)~NSFX_F_TPAGE_UNKNOWN;
        }
        if (it->flags & NSFX_F_ABR_UNKNOWN) { it->abr = o->abr & 3; it->flags &= (uint16_t)~NSFX_F_ABR_UNKNOWN; }
        n++;
    }
    return n;
}
