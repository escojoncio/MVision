/* vr_hit: ver vr_hit.h. Solo lee/escribe la RAM del juego (edición europea); sin dependencias del runtime. No es reentrante (hilo del emulador). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "vr_hit.h"

#define RAM_MASK 0x1FFFFFu
/* ---- direcciones/offsets del juego (medidos en el desensamblado y en volcados de RAM) ---- */
#define T_TABLE      0x800B7660u     /* 64 blancos de 72 B: +0 clase (0x10 = lo sellan las balas, 4 = dañable), +2 bando, +4 mapa, +6 banderas, +8 centro s16[3], +16 semiejes s16[3], +0x26 vida */
#define T_N          64
#define T_SIZE       72u
#define A_ACTIVE_MAP 0x800ACB9Cu     /* máscara de mapas activos: el juego solo sella/daña blancos de un mapa activo (0x8002dc20) */
#define CHAN_MAIN    0x800B2B7Cu     /* canal principal: +0x56 nº de objetos (s16), +0x58 cola de punteros */
#define OBJ_T        20u             /* objeto: traslación de la raíz (s32[3]) */
#define OBJ_NMODELS  0x2Eu
#define OBJ_PIECES   0x48u           /* piezas de 0x5C B: +0 matriz local->mundo, +0x40 modelo */
#define PIECE_SIZE   0x5Cu
#define MODEL_BOX    0x08u           /* modelo: caja local s32[6], +0x20 traslación de la articulación s32[3], +0x2C padre */
#define MODEL_JT     0x20u
#define MODEL_PARENT 0x2Cu
#define A_PLAYER_CTRL 0x800ACB84u    /* control del jugador: +0x2C mapa -> +8 envoltura -> +0 cabecera de colisión (+10 nº de grupos, +16 grupos de 24 B: +4 nº de suelos, +12 suelos de 48 B) */
#define A_HZD_MASK   0x800ACB38u     /* grupos de colisión activos */
/* actor de la bala (0x800779a8 lo crea; 0x800771ac es su tick) */
#define B_SIZE   0x170u
#define SILENCER_FLAG 0x800B5FA0u    /* BA_VARS (0x800B5F28) + 2 * 60: GM_SilencerFlag */
#define B_TARGET 0x44u               /* su blanco de daño (72 B; el daño en +38) */
#define B_POS    0x110u              /* posición s16[3]: origen del segmento del próximo tick */
#define B_END    0x118u              /* final del trazado s16[3] (pared del juego o alcance) */
#define B_VEL    0x120u              /* avance por tick s16[3] */
#define B_LIMIT  0x138u              /* distancia a la que acaba */
#define B_WALLHIT 0x140u             /* 1 = su trazado dio en la colisión del juego (pared, suelo, puerta u otra colisión dinámica) a la distancia B_LIMIT */
#define B_TYPE   0x144u              /* tipo de la bala (bullet.c work->type): 2 hace GM_SetNoise(100,2,hitpos) al impactar, 1 no */
#define B_SIDE   0x148u
#define B_DAMAGE 0x154u
#define B_RANGE  0x158u
#define PLAYER_SIDE 1u

static int ptr_ok(uint32_t a, uint32_t n) { return a >= 0x80010000u && a < 0x80200000u && n <= 0x200000u && (a & RAM_MASK) + n <= RAM_MASK + 1u; }
static uint32_t ru32(const uint8_t *r, uint32_t a) { uint32_t v; memcpy(&v, r + (a & RAM_MASK), 4); return v; }
static int32_t rs32(const uint8_t *r, uint32_t a) { int32_t v; memcpy(&v, r + (a & RAM_MASK), 4); return v; }
static int16_t rs16(const uint8_t *r, uint32_t a) { int16_t v; memcpy(&v, r + (a & RAM_MASK), 2); return v; }
static uint16_t ru16(const uint8_t *r, uint32_t a) { return (uint16_t)rs16(r, a); }
static void ws16(uint8_t *r, uint32_t a, int16_t v) { memcpy(r + (a & RAM_MASK), &v, 2); }
static void ws32(uint8_t *r, uint32_t a, int32_t v) { memcpy(r + (a & RAM_MASK), &v, 4); }
static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static int unit3(const float d[3], float o[3]) {
    float l = sqrtf(dot3(d, d));
    if (!(l > 1e-6f) || !isfinite(l)) return 0;
    for (int i = 0; i < 3; i++) o[i] = d[i] / l;
    return 1;
}

const char *vrhit_zone_name(int z) {
    static const char *nm[] = {"ninguna", "cabeza", "cuerpo", "extremidad", "caja", "pared", "fallo"};
    return z >= 0 && z <= VRHIT_Z_MISS ? nm[z] : nm[0];
}

/* ================================================== cápsulas ================================================== */
static void no_caps(VrHitTarget *t) { t->object = 0; t->ncap = 0; t->head = -1; }
static void build_caps(const uint8_t *ram, VrHitTarget *t) {
    uint32_t o = t->object;
    int n = rs16(ram, o + OBJ_NMODELS), par[VRHIT_MAX_CAP], leaf[VRHIT_MAX_CAP];
    float jy[VRHIT_MAX_CAP];
    if (n > VRHIT_MAX_CAP) n = VRHIT_MAX_CAP;
    if (!ptr_ok(o, OBJ_PIECES + PIECE_SIZE * (uint32_t)n)) { no_caps(t); return; }
    for (int k = 0; k < n; k++) {
        uint32_t p = o + OBJ_PIECES + PIECE_SIZE * (uint32_t)k, m = ru32(ram, p + 0x40);
        if (!ptr_ok(m, 0x58)) { no_caps(t); return; }
        float lo[3], hi[3], hl[3], c[3];
        for (int i = 0; i < 3; i++) {
            lo[i] = (float)rs32(ram, m + MODEL_BOX + 4u * (uint32_t)i); hi[i] = (float)rs32(ram, m + MODEL_BOX + 12u + 4u * (uint32_t)i);
            hl[i] = (hi[i] - lo[i]) * 0.5f; c[i] = (hi[i] + lo[i]) * 0.5f;
            if (!(hl[i] >= 0.0f && hl[i] < 2000.0f)) { no_caps(t); return; }
        }
        /* cápsula de la caja local: eje = lado más largo, radio = el mayor de los otros dos semiejes */
        int ax = hl[0] >= hl[1] ? (hl[0] >= hl[2] ? 0 : 2) : (hl[1] >= hl[2] ? 1 : 2);
        float r = ax == 0 ? fmaxf(hl[1], hl[2]) : ax == 1 ? fmaxf(hl[0], hl[2]) : fmaxf(hl[0], hl[1]);
        float seg = hl[ax] > r ? hl[ax] - r : 0.0f, la[3] = {c[0], c[1], c[2]}, lb[3] = {c[0], c[1], c[2]};
        la[ax] -= seg; lb[ax] += seg;
        VrHitCap *q = &t->cap[k];
        for (int i = 0; i < 3; i++) {
            float m0 = rs16(ram, p + 6u * (uint32_t)i), m1 = rs16(ram, p + 6u * (uint32_t)i + 2u), m2 = rs16(ram, p + 6u * (uint32_t)i + 4u), tt = (float)rs32(ram, p + 20u + 4u * (uint32_t)i);
            q->a[i] = tt + (m0 * la[0] + m1 * la[1] + m2 * la[2]) / 4096.0f;
            q->b[i] = tt + (m0 * lb[0] + m1 * lb[1] + m2 * lb[2]) / 4096.0f;
            if (!(fabsf(q->a[i] - t->c[i]) < 4000.0f && fabsf(q->b[i] - t->c[i]) < 4000.0f)) { no_caps(t); return; }      /* matrices de pieza sin calcular o de otro sitio */
        }
        q->r = r < 20.0f ? 20.0f : r; q->zone = VRHIT_Z_LIMB;
        par[k] = rs32(ram, m + MODEL_PARENT); if (par[k] < -1 || par[k] >= n || par[k] == k) par[k] = -1;
        jy[k] = (float)rs32(ram, m + MODEL_JT + 4u);
        leaf[k] = 1;
    }
    for (int k = 0; k < n; k++) if (par[k] >= 0) leaf[par[k]] = 0;
    /* cabeza: la pieza hoja más alta en la pose de reposo (altura = suma de las traslaciones de articulación hasta la raíz) */
    int head = -1; float best = 0;
    for (int k = 0; k < n; k++) {
        if (!leaf[k]) continue;
        float y = 0; int j = k;
        for (int g = 0; j >= 0 && g < n; g++) { y += jy[j]; j = par[j]; }
        if (head < 0 || y > best) { head = k; best = y; }
    }
    t->head = head; t->ncap = n;
    if (head < 0) return;
    t->cap[head].zone = VRHIT_Z_HEAD;
    int j = par[head];
    for (int g = 0; j >= 0 && g < n; g++) { t->cap[j].zone = VRHIT_Z_BODY; j = par[j]; }
}

/* Empareja cada blanco con el objeto articulado (>= 2 piezas) cuya raíz cae en su caja; un objeto es del blanco (de cualquier bando) cuyo centro le queda más cerca. */
static void pair_objects(const uint8_t *ram, VrHitTarget *t, int n) {
    int qn = rs16(ram, CHAN_MAIN + 0x56);
    uint32_t q = ru32(ram, CHAN_MAIN + 0x58);
    float best[VRHIT_MAX_T];
    if (qn > 256) qn = 256;
    if (qn <= 0 || !ptr_ok(q, 4u * (uint32_t)qn)) return;
    for (int i = 0; i < n; i++) best[i] = 1e30f;
    for (int k = 0; k < qn; k++) {
        uint32_t o = ru32(ram, q + 4u * (uint32_t)k);
        if (!ptr_ok(o, OBJ_PIECES) || rs16(ram, o + OBJ_NMODELS) < 2) continue;
        float r[3], od = 1e30f; int own = -1;
        for (int i = 0; i < 3; i++) r[i] = (float)rs32(ram, o + OBJ_T + 4u * (uint32_t)i);
        for (int i = 0; i < T_N; i++) {
            uint32_t b = T_TABLE + T_SIZE * (uint32_t)i;
            if (!ru16(ram, b)) continue;
            float d2 = 0; int in = 1;
            for (int a = 0; a < 3; a++) { float d = r[a] - (float)rs16(ram, b + 8u + 2u * (uint32_t)a); if (fabsf(d) > (float)rs16(ram, b + 16u + 2u * (uint32_t)a)) in = 0; d2 += d * d; }
            if (in && d2 < od) { od = d2; own = i; }
        }
        for (int i = 0; i < n; i++) if (t[i].index == own && od < best[i]) { best[i] = od; t[i].object = o; }
    }
    for (int i = 0; i < n; i++) if (t[i].object) build_caps(ram, &t[i]);
}

int vrhit_scan(const uint8_t *ram, unsigned skip_side, unsigned need_cls, int caps, VrHitTarget *out, int max) {
    if (!ram || !out || max <= 0) return 0;
    uint32_t amap = ru32(ram, A_ACTIVE_MAP);
    int n = 0;
    if (max > VRHIT_MAX_T) max = VRHIT_MAX_T;
    for (int i = 0; i < T_N && n < max; i++) {
        uint32_t b = T_TABLE + T_SIZE * (uint32_t)i;
        uint16_t cls = ru16(ram, b), side = ru16(ram, b + 2);
        if (!(cls & need_cls) || side == skip_side || !(ru16(ram, b + 4) & amap)) continue;
        VrHitTarget *t = &out[n++];
        t->addr = b; t->index = i; t->cls = cls; t->side = side; t->vital = rs16(ram, b + 0x26);
        for (int a = 0; a < 3; a++) { t->c[a] = rs16(ram, b + 8u + 2u * (uint32_t)a); t->h[a] = rs16(ram, b + 16u + 2u * (uint32_t)a); }
        no_caps(t);
    }
    if (caps && n) pair_objects(ram, out, n);
    return n;
}

/* ================================================== rayos ================================================== */
static float sphere_ray(const float oc[3], const float d[3], float r) {
    float b = dot3(d, oc), c = dot3(oc, oc) - r * r, h = b * b - c;
    if (c <= 0.0f) return 0.0f;                    /* el origen ya está dentro */
    if (h < 0.0f) return -1.0f;
    float t = -b - sqrtf(h);
    return t >= 0.0f ? t : -1.0f;
}
/* Rayo o + t*d (d unitario) contra la cápsula a-b de radio r: t >= 0 del primer contacto (0 si el origen está dentro) o -1. */
static float cap_ray(const float o[3], const float d[3], const float a[3], const float b[3], float r) {
    float ba[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, oa[3] = {o[0] - a[0], o[1] - a[1], o[2] - a[2]}, ob[3] = {o[0] - b[0], o[1] - b[1], o[2] - b[2]};
    float baba = dot3(ba, ba), bard = dot3(ba, d), baoa = dot3(ba, oa), rdoa = dot3(d, oa), oaoa = dot3(oa, oa);
    float best = -1.0f, t;
    if (baba > 1e-3f) {
        float A = baba - bard * bard, B = baba * rdoa - baoa * bard, C = baba * oaoa - baoa * baoa - r * r * baba;
        float y0 = baoa;                           /* proyección del origen sobre el eje */
        if (C <= 0.0f && y0 >= 0.0f && y0 <= baba) return 0.0f;      /* origen dentro del cilindro */
        if (A > 1e-6f * baba) {
            float h = B * B - A * C;
            if (h >= 0.0f) {
                t = (-B - sqrtf(h)) / A;
                float y = baoa + t * bard;
                if (t >= 0.0f && y >= 0.0f && y <= baba) best = t;
            }
        }
    }
    t = sphere_ray(oa, d, r); if (t >= 0.0f && (best < 0.0f || t < best)) best = t;
    t = sphere_ray(ob, d, r); if (t >= 0.0f && (best < 0.0f || t < best)) best = t;
    return best;
}
/* Rayo contra la caja del blanco: distancia de entrada (0 si el origen está dentro) o -1. */
static float box_ray(const VrHitTarget *t, const float o[3], const float d[3]) {
    float t0 = 0.0f, t1 = 1e30f;
    for (int k = 0; k < 3; k++) {
        float lo = t->c[k] - t->h[k], hi = t->c[k] + t->h[k];
        if (fabsf(d[k]) < 1e-9f) { if (o[k] < lo || o[k] > hi) return -1.0f; continue; }
        float a = (lo - o[k]) / d[k], b = (hi - o[k]) / d[k];
        if (a > b) { float x = a; a = b; b = x; }
        if (a > t0) t0 = a;
        if (b < t1) t1 = b;
        if (t0 > t1) return -1.0f;
    }
    return t0;
}
/* Volumen de impacto de un blanco: sus cápsulas o, sin ellas, su caja. */
static float target_ray(const VrHitTarget *t, const float o[3], const float d[3], int *zone) {
    if (!t->ncap) { *zone = VRHIT_Z_BOX; return box_ray(t, o, d); }
    float best = -1.0f;
    for (int k = 0; k < t->ncap; k++) {
        float tc = cap_ray(o, d, t->cap[k].a, t->cap[k].b, t->cap[k].r);
        if (tc >= 0.0f && (best < 0.0f || tc < best)) { best = tc; *zone = t->cap[k].zone; }
    }
    return best;
}

int vrhit_ray(const VrHitTarget *t, int n, const float o[3], const float d[3], float max, float *dist, int *zone) {
    float dn[3], best = max; int bi = -1;
    if (!t || !o || !d || !unit3(d, dn)) return -1;
    for (int i = 0; i < n; i++) {
        int z = VRHIT_Z_NONE; float tc = target_ray(&t[i], o, dn, &z);
        if (tc >= 0.0f && tc <= best) { best = tc; bi = i; if (zone) *zone = z; }
    }
    if (bi >= 0 && dist) *dist = best;
    return bi;
}

int vrhit_sweep(const VrHitTarget *t, int n, const float a[3], const float b[3], float r, int *zone, float pt[3]) {
    if (!t || !a || !b) return -1;
    float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, dn[3] = {0, 0, 1}, len = sqrtf(dot3(d, d)), best = 0; int bi = -1;
    if (!isfinite(len)) return -1;
    if (len > 1e-3f) for (int i = 0; i < 3; i++) dn[i] = d[i] / len; else len = 0.0f;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < t[i].ncap; k++) {
            float tc = cap_ray(a, dn, t[i].cap[k].a, t[i].cap[k].b, t[i].cap[k].r + r);
            if (tc >= 0.0f && tc <= len && (bi < 0 || tc < best)) { best = tc; bi = i; if (zone) *zone = t[i].cap[k].zone; }
        }
    if (bi >= 0 && pt) for (int i = 0; i < 3; i++) pt[i] = a[i] + dn[i] * best;
    return bi;
}

void vrhit_clamp_box(const VrHitTarget *t, const float p[3], float out[3]) {
    for (int k = 0; k < 3; k++) {
        float h = t->h[k] > 2.0f ? t->h[k] - 2.0f : 0.0f, lo = t->c[k] - h, hi = t->c[k] + h;
        out[k] = p[k] < lo ? lo : p[k] > hi ? hi : p[k];
    }
}

/* Suelos de colisión: los mismos cuadriláteros que prueba el trazado de la bala del juego (0x80028d14 con la bandera 1; 0x800287c8 por suelo). */
float vrhit_floor_ray(const uint8_t *ram, const float o[3], const float d[3], float max) {
    float dn[3];
    if (!ram || !o || !d || !(max > 0.0f) || !isfinite(max) || !unit3(d, dn) || !isfinite(o[0]) || !isfinite(o[1]) || !isfinite(o[2])) return -1.0f;
    uint32_t ctl = ru32(ram, A_PLAYER_CTRL);
    if (!ptr_ok(ctl, 0x30)) return -1.0f;
    uint32_t map = ru32(ram, ctl + 0x2C);
    if (!ptr_ok(map, 12)) return -1.0f;
    uint32_t wr = ru32(ram, map + 8);
    if (!ptr_ok(wr, 4)) return -1.0f;
    uint32_t hdr = ru32(ram, wr);
    if (!ptr_ok(hdr, 20)) return -1.0f;
    int ng = rs16(ram, hdr + 10);
    uint32_t gp = ru32(ram, hdr + 16), mask = ru32(ram, A_HZD_MASK);
    if (ng <= 0 || ng > 32 || !ptr_ok(gp, 24u * (uint32_t)ng)) return -1.0f;
    float best = max; int found = 0;
    for (int g = 0; g < ng; g++) {
        if (!((mask >> g) & 1u)) continue;
        uint32_t b = gp + 24u * (uint32_t)g, fp = ru32(ram, b + 12);
        int nf = rs16(ram, b + 4);
        if (nf <= 0 || nf > 4096 || !ptr_ok(fp, 48u * (uint32_t)nf)) continue;
        for (int i = 0; i < nf; i++) {
            float p[4][3];
            for (int k = 0; k < 4; k++) {
                uint32_t a = fp + 48u * (uint32_t)i + 16u + 8u * (uint32_t)k;
                p[k][0] = rs16(ram, a); p[k][2] = rs16(ram, a + 2); p[k][1] = rs16(ram, a + 4);
            }
            for (int tri = 0; tri < 2; tri++) {                    /* (p1,p2,p3) y (p1,p3,p4) */
                const float *v0 = p[0], *v1 = p[1 + tri], *v2 = p[2 + tri];
                float e1[3] = {v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]}, e2[3] = {v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]};
                float pv[3] = {dn[1] * e2[2] - dn[2] * e2[1], dn[2] * e2[0] - dn[0] * e2[2], dn[0] * e2[1] - dn[1] * e2[0]}, det = dot3(e1, pv);
                if (fabsf(det) < 1e-3f) continue;
                float tv[3] = {o[0] - v0[0], o[1] - v0[1], o[2] - v0[2]}, u = dot3(tv, pv) / det;
                if (u < 0.0f || u > 1.0f) continue;
                float qv[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0]}, v = dot3(dn, qv) / det;
                if (v < 0.0f || u + v > 1.0f) continue;
                float t = dot3(e2, qv) / det;
                if (t > 1.0f && t <= best) { best = t; found = 1; }
            }
        }
    }
    return found ? best : -1.0f;
}

/* ================================================== balas ================================================== */
static VrHitWorld W;
static int w_set;
void vrhit_set_world(const VrHitWorld *w) { if (w) { W = *w; w_set = 1; } else { memset(&W, 0, sizeof W); w_set = 0; } }

typedef struct { int on, age; float o[3], d[3], rel[3], dist; uint32_t addr; int zone; } Aim;
#define N_AIM 4
static Aim g_aim[N_AIM];
static VrHitTarget g_t[VRHIT_MAX_T];

typedef struct {
    int idx, zone;           /* blanco alcanzado (índice en g_t; -1 ninguno) y zona */
    float dist, pt[3];       /* distancia y punto del impacto en el blanco */
    float wall;              /* distancia a la geometría (el alcance si no hay) */
    int crossed;             /* el rayo cruza la caja de algún blanco dentro del alcance */
    int cam;                 /* el blanco alcanzado es una cámara de vigilancia */
    uint32_t cam_ext;        /* blanco de una cámara que el juego no prueba (fuera de g_t) alcanzada antes que todo lo demás; 0 = no */
    int src;                 /* qué cortó la línea de tiro (wall < alcance): 1 trazado del juego, 2 pared HZD, 3 suelo HZD */
    float solid;             /* distancia al primer sólido de render de la escena (solo registro; no bloquea), -1 ninguno */
} Res;

/* Un único criterio de línea de tiro: el blanco más cercano que toca el rayo antes que la geometría sólida y dentro del alcance. */
static void resolve(uint8_t *ram, const VrHitTarget *t, int n, const float o[3], const float d[3], float R, float game_wall, int want_solid, Res *r) {
    memset(r, 0, sizeof *r);
    r->idx = -1; r->wall = R; r->solid = -1.0f;
    float best = R;
    for (int i = 0; i < n; i++) {
        float tb = box_ray(&t[i], o, d);
        if (tb >= 0.0f && tb <= R) r->crossed = 1;
        int z = VRHIT_Z_NONE; float tc = target_ray(&t[i], o, d, &z);
        if (tc >= 0.0f && tc < best) { best = tc; r->idx = i; r->zone = z; }
    }
    /* la geometría solo importa hasta el blanco más cercano (o hasta el alcance si no hay ninguno) */
    int src = 0;
    if (game_wall >= 0.0f && game_wall < best) { r->wall = game_wall; src = 1; }
    float w = W.wall ? W.wall(ram, o, d, best) : -1.0f;
    if (w >= 0.0f && w < r->wall) { r->wall = w; src = 2; }
    w = vrhit_floor_ray(ram, o, d, best);
    if (w >= 0.0f && w < r->wall) { r->wall = w; src = 3; }
    if (r->wall < best) { best = r->wall; r->idx = -1; r->zone = VRHIT_Z_NONE; r->src = src; }
    float tcam = 0; uint32_t tg = 0;
    if (W.camera && W.camera(ram, o, d, R, &tcam, &tg)) {
        int in = -1;
        for (int i = 0; i < n; i++) if (t[i].addr == tg) in = i;
        if (in >= 0) { if (in == r->idx) r->cam = 1; }
        else if (tcam >= 0.0f && tcam < best) { best = tcam; r->idx = -1; r->cam_ext = tg; r->zone = VRHIT_Z_BOX; r->cam = 1; }
    }
    r->dist = best;
    if (want_solid && W.solid) r->solid = W.solid(ram, o, d, best);
    for (int i = 0; i < 3; i++) r->pt[i] = o[i] + d[i] * best;
}

float vrhit_probe(const uint8_t *ram, const float o[3], const float d[3], float max) {
    float dn[3], best = -1.0f;
    if (!ram || !o || !d || !(max > 0.0f) || !unit3(d, dn)) return -1.0f;
    int n = vrhit_scan(ram, PLAYER_SIDE, 0x10, 1, g_t, VRHIT_MAX_T);
    for (int i = 0; i < n; i++) {
        int z = VRHIT_Z_NONE; float tc = target_ray(&g_t[i], o, dn, &z);
        if (tc >= 0.0f && tc <= max && (best < 0.0f || tc < best)) best = tc;
    }
    return best;
}

/* ---- láser = bala ---- */
typedef struct { int on, age, hit; float o[3], d[3], pt[3]; } Note;
static Note g_note[N_AIM];
static float g_range = VRHIT_RANGE_MIN, g_upm;      /* g_upm: unidades por metro reales (calibradas) que da el anfitrión cada VBlank; 0 = aún no */
static float log_upm(void) { return g_upm > 0.0f ? g_upm : W.units_per_m > 0.0f ? W.units_per_m : 1000.0f; }
static int g_ext;
void vrhit_set_range(float units, float upm) {
    g_range = isfinite(units) && units > 1000.0f ? units : VRHIT_RANGE_MIN;
    g_upm = isfinite(upm) && upm > 0.0f ? upm : 0.0f;
}
void vrhit_next_bullet_extended(void) { g_ext = 1; }

float vrhit_shot_dist(const uint8_t *ram, const float o[3], const float d[3], float max) {
    float dn[3]; Res r;
    if (!ram || !o || !d || !(max > 0.0f) || !unit3(d, dn)) return -1.0f;
    int n = vrhit_scan(ram, PLAYER_SIDE, 0x10, 1, g_t, VRHIT_MAX_T);
    resolve((uint8_t *)ram, g_t, n, o, dn, max, -1.0f, 0, &r);
    return r.dist < max ? r.dist : -1.0f;
}

void vrhit_note_laser(const uint8_t *ram, const float o[3], const float d[3]) {
    float dn[3];
    if (!ram || !o || !d || !unit3(d, dn)) return;
    float t = vrhit_shot_dist(ram, o, dn, g_range);
    Note *a = &g_note[0];
    for (int i = 0; i < N_AIM; i++) { if (!g_note[i].on) { a = &g_note[i]; break; } if (g_note[i].age > a->age) a = &g_note[i]; }
    a->on = 1; a->age = 0; a->hit = t >= 0.0f;
    for (int i = 0; i < 3; i++) { a->o[i] = o[i]; a->d[i] = dn[i]; a->pt[i] = o[i] + dn[i] * (t >= 0.0f ? t : g_range); }
}

/* Anotación del láser para la bala que sale por (o, d): la más reciente que coincide (misma tolerancia que la puntería); se consume. NULL si no hay. */
static const Note *take_note(const float o[3], const float d[3]) {
    Note *best = NULL;
    for (int k = 0; k < N_AIM; k++) {
        Note *a = &g_note[k];
        if (!a->on || fabsf(a->o[0] - o[0]) > 16.0f || fabsf(a->o[1] - o[1]) > 16.0f || fabsf(a->o[2] - o[2]) > 16.0f || dot3(a->d, d) < 0.9995f) continue;
        if (!best || a->age < best->age) best = a;
    }
    if (best) best->on = 0;
    return best;
}

void vrhit_frame(void) {
    for (int i = 0; i < N_AIM; i++) {
        if (g_aim[i].on && ++g_aim[i].age > VRHIT_AIM_LIFE) g_aim[i].on = 0;
        if (g_note[i].on && ++g_note[i].age > VRHIT_AIM_LIFE) g_note[i].on = 0;
    }
}

void vrhit_aim(const uint8_t *ram, const float o[3], const float d[3]) {
    float dn[3]; Res r;
    if (!w_set || !ram || !o || !d || !unit3(d, dn)) return;
    int n = vrhit_scan(ram, PLAYER_SIDE, 0x10, 1, g_t, VRHIT_MAX_T);
    resolve((uint8_t *)ram, g_t, n, o, dn, VRHIT_RANGE_MIN, -1.0f, 1, &r);
    if (r.idx < 0) return;
    Aim *a = &g_aim[0];
    for (int i = 0; i < N_AIM; i++) { if (!g_aim[i].on) { a = &g_aim[i]; break; } if (g_aim[i].age > a->age) a = &g_aim[i]; }
    a->on = 1; a->age = 0; a->addr = g_t[r.idx].addr; a->zone = r.zone; a->dist = r.dist;
    for (int i = 0; i < 3; i++) { a->o[i] = o[i]; a->d[i] = dn[i]; a->rel[i] = r.pt[i] - g_t[r.idx].c[i]; }
}

/* Puntería anotada para este mismo rayo (misma boca y dirección que en el gatillo): se consume; vale si su blanco sigue en la tabla. */
static int take_aim(uint8_t *ram, const float o[3], const float d[3], const VrHitTarget *t, int n, float R, float game_wall, Res *r) {
    for (int k = 0; k < N_AIM; k++) {
        Aim *a = &g_aim[k];
        if (!a->on || fabsf(a->o[0] - o[0]) > 16.0f || fabsf(a->o[1] - o[1]) > 16.0f || fabsf(a->o[2] - o[2]) > 16.0f || dot3(a->d, d) < 0.9995f) continue;
        a->on = 0;
        if (game_wall >= 0.0f && game_wall < a->dist - 100.0f) return 0;      /* el trazado del juego corta ese rayo antes del blanco (una puerta cerrada, p. ej.) */
        for (int i = 0; i < n; i++) {
            if (t[i].addr != a->addr) continue;
            memset(r, 0, sizeof *r);
            r->idx = i; r->zone = a->zone; r->wall = R; r->crossed = 1; r->solid = -1.0f;
            float dv[3];
            for (int j = 0; j < 3; j++) { r->pt[j] = t[i].c[j] + a->rel[j]; dv[j] = r->pt[j] - o[j]; }
            r->dist = sqrtf(dot3(dv, dv));
            /* el blanco se ha movido: la línea boca -> blanco de AHORA se revalida con la misma geometría (paredes y sólidos del anfitrión, suelos de colisión) */
            float dn[3];
            if (unit3(dv, dn)) {
                float w = W.wall ? W.wall(ram, o, dn, r->dist) : -1.0f, f = vrhit_floor_ray(ram, o, dn, r->dist);
                int src = 2;
                if (f >= 0.0f && (w < 0.0f || f < w)) { w = f; src = 3; }
                if (W.solid) r->solid = W.solid(ram, o, dn, r->dist);
                if (w >= 0.0f && w < r->dist - 50.0f) { r->idx = -1; r->zone = VRHIT_Z_NONE; r->wall = r->dist = w; r->src = src; }      /* tras una esquina: pared */
            }
            return 1;
        }
        return 0;
    }
    return 0;
}

/* La bala acaba en su punto final sin recorrer nada: el segmento de su próximo tick es nulo y no sella ninguna caja (si el juego trazó una pared, salta ahí su chispa). */
static void void_bullet(uint8_t *ram, uint32_t actor) {
    memcpy(ram + ((actor + B_POS) & RAM_MASK), ram + ((actor + B_END) & RAM_MASK), 6);
    ws32(ram, actor + B_LIMIT, 0);
}

/* Bala de alcance ampliado sin impacto dentro del alcance del láser: acaba ahí, sin chispa ni sonido de una pared lejana (lo que le queda de recorrido al juego no existe para el tiro VR). */
static void trim_bullet(uint8_t *ram, uint32_t actor, const float o[3], const float d[3], float R) {
    ws32(ram, actor + B_LIMIT, (int32_t)R); ws32(ram, actor + B_WALLHIT, 0);
    for (int k = 0; k < 3; k++) { long q = lrintf(o[k] + d[k] * R); ws16(ram, actor + B_END + 2u * (uint32_t)k, (int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : q)); }
}

/* Bala de un enemigo: ni se recoloca ni se anula ni se marca impacto. Si su rayo cruza la caja de un blanco de otro bando (el jugador) más allá de donde el juego la acaba,
 * su trazado no dio en nada y la geometría del anfitrión (que aquí solo resta) está despejada hasta esa caja, se amplía su recorrido hasta VRHIT_RANGE_MIN: quien sella es
 * el tick del juego. Devuelve 1 si cruza alguna caja (hay algo que anotar). */
static int enemy_bullet(uint8_t *ram, uint32_t actor, unsigned side, const float o[3], const float d[3], VrHitShot *s) {
    int n = vrhit_scan(ram, side, 0x10, 0, g_t, VRHIT_MAX_T), bi = -1;
    float tb = 0;
    for (int i = 0; i < n; i++) { float t = box_ray(&g_t[i], o, d); if (t >= 0.0f && t <= VRHIT_RANGE_MIN && (bi < 0 || t < tb)) { bi = i; tb = t; } }
    if (bi < 0) return 0;                          /* lo normal en una ráfaga: sin más coste que este recorrido de la tabla */
    float limit = (float)rs32(ram, actor + B_LIMIT);
    s->dist = tb;
    if (tb <= limit) { s->target = g_t[bi].index; s->vital = g_t[bi].vital; s->zone = VRHIT_Z_BOX; return 1; }      /* a su alcance: decide el juego */
    if (rs32(ram, actor + B_WALLHIT) == 1) { s->zone = VRHIT_Z_WALL; s->dist = limit; return 1; }                 /* su trazado dio en la colisión del juego */
    float w = W.wall ? W.wall(ram, o, d, tb) : -1.0f, f = vrhit_floor_ray(ram, o, d, tb);
    if (f >= 0.0f && (w < 0.0f || f < w)) w = f;
    if (w >= 0.0f) { s->zone = VRHIT_Z_WALL; s->dist = w; return 1; }
    float L = VRHIT_RANGE_MIN;                     /* recorrido nuevo, sin salir de las coordenadas s16 del motor */
    for (int k = 0; k < 3; k++) if (fabsf(d[k]) > 1e-6f) { float e = ((d[k] > 0.0f ? 32767.0f : -32768.0f) - o[k]) / d[k]; if (e < L) L = e; }
    if (!(L > limit)) return 1;
    ws32(ram, actor + B_RANGE, (int32_t)L); ws32(ram, actor + B_LIMIT, (int32_t)L);
    for (int k = 0; k < 3; k++) { long q = lrintf(o[k] + d[k] * L); ws16(ram, actor + B_END + 2u * (uint32_t)k, (int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : q)); }
    s->target = g_t[bi].index; s->vital = g_t[bi].vital; s->zone = VRHIT_Z_BOX; s->action = VRHIT_ACT_RANGE;
    return 1;
}

int vrhit_bullet(uint8_t *ram, uint32_t actor, VrHitShot *out) {
    const int ext_req = g_ext;
    g_ext = 0;       /* la marca es de esta bala, se resuelva o no */
    /* Con el silenciador puesto el impacto de la bala promocionada a tipo 2 no debe hacer ruido (el original la crea de tipo 1 y los guardias no oyen la SOCOM silenciada): se devuelve a tipo 1 (el alcance ya se trazó en GetResources).
     * GM_SilencerFlag = linkvarbuf[60] (s16 en BA_VARS + 120; socom.c GetResources: < 0 sin supresor, >= 0 con él). Sin silenciador el ruido se mantiene. */
    if (ext_req && ram && ptr_ok(actor, B_SIZE) && rs16(ram, SILENCER_FLAG) >= 0) ws32(ram, actor + B_TYPE, 1);
    if (!w_set || !ram || !ptr_ok(actor, B_SIZE)) return 0;
    float o[3], v[3], d[3];
    for (int i = 0; i < 3; i++) { o[i] = rs16(ram, actor + B_POS + 2u * (uint32_t)i); v[i] = rs16(ram, actor + B_VEL + 2u * (uint32_t)i); }
    if (!(dot3(v, v) > 1.0f) || !unit3(v, d)) return 0;
    unsigned side = ru32(ram, actor + B_SIDE) & 0xFFFFu;
    int player = side == PLAYER_SIDE;
    float R = (float)rs32(ram, actor + B_RANGE);
    const int ext = ext_req && player;      /* alcance ampliado por la capa VR: manda el del láser, no el del actor */
    if (player) R = ext ? g_range : (R > g_range ? R : g_range);
    else if (!(R >= VRHIT_RANGE_MIN)) R = VRHIT_RANGE_MIN;
    VrHitShot s = {(int)side, -1, VRHIT_Z_MISS, VRHIT_ACT_NONE, rs32(ram, actor + B_DAMAGE), -1, 0.0f};
    if (!player) {
        if (enemy_bullet(ram, actor, side, o, d, &s))
            fprintf(stderr, "[impacto] blanco=%d zona=%s dist=%.2f m vida=%d | bando=enemigo dano=%d accion=%s\n", s.target, s.target >= 0 ? "jugador" : vrhit_zone_name(s.zone),
                    s.dist / log_upm(), s.vital, s.damage, s.action == VRHIT_ACT_RANGE ? "alcance" : "intacta");
        if (out) *out = s;
        return 1;
    }
    int n = vrhit_scan(ram, side, 0x10, 1, g_t, VRHIT_MAX_T);
    /* geometría sólida = la del anfitrión + los suelos de colisión + lo que ya encontró el trazado del propio juego (colisión del mapa del tirador, puertas y demás colisión dinámica) */
    float gw = rs32(ram, actor + B_WALLHIT) == 1 ? (float)rs32(ram, actor + B_LIMIT) : -1.0f;
    Res r;
    if (!take_aim(ram, o, d, g_t, n, R, gw, &r)) resolve(ram, g_t, n, o, d, R, gw, 1, &r);
    s.dist = r.dist;
    int16_t P[3];
    if (r.idx >= 0) {
        const VrHitTarget *t = &g_t[r.idx];
        float p[3]; int same = 1;
        vrhit_clamp_box(t, r.pt, p);
        for (int i = 0; i < 3; i++) { P[i] = (int16_t)lrintf(p[i]); if (P[i] != rs16(ram, actor + B_END + 2u * (uint32_t)i)) same = 0; }
        if (same) P[1] = (int16_t)(P[1] + ((float)P[1] < t->c[1] ? 1 : -1));       /* origen = final sería un segmento nulo */
        for (int i = 0; i < 3; i++) ws16(ram, actor + B_POS + 2u * (uint32_t)i, P[i]);
        /* letal solo en el soldado normal: dañable, bando enemigo, su caja (300,1000,300) y vida de 2-3 balas; jefes y demás humanoides reciben el daño del juego */
        int soldier = (t->cls & 4) && t->side == 2 && t->h[0] == 300.0f && t->h[1] == 1000.0f && t->h[2] == 300.0f && t->vital <= 3 * s.damage;
        if (r.zone == VRHIT_Z_HEAD && soldier && t->vital > s.damage) {
            s.damage = t->vital;
            ws32(ram, actor + B_DAMAGE, s.damage); ws16(ram, actor + B_TARGET + 38u, (int16_t)s.damage);
        }
        if (r.cam && !(t->cls & 4) && W.camera_hit) W.camera_hit(ram, t->addr, P);      /* cámara (la reconoce el anfitrión por su blanco) que el juego no va a dañar */
        s.target = t->index; s.vital = t->vital; s.zone = r.zone; s.action = VRHIT_ACT_SEAL;
    } else if (r.cam_ext) {
        for (int i = 0; i < 3; i++) P[i] = (int16_t)lrintf(fmaxf(-32768.0f, fminf(32767.0f, r.pt[i])));
        if (W.camera_hit) W.camera_hit(ram, r.cam_ext, P);
        void_bullet(ram, actor);
        s.zone = VRHIT_Z_BOX; s.action = VRHIT_ACT_VOID;
    } else {
        s.zone = r.wall < R ? VRHIT_Z_WALL : VRHIT_Z_MISS; s.dist = r.wall;
        if (ext && !(r.wall < R) && (float)rs32(ram, actor + B_LIMIT) > R + 1.0f) trim_bullet(ram, actor, o, d, R);
        if (r.crossed) { void_bullet(ram, actor); s.action = VRHIT_ACT_VOID; }
    }
    {
        float upm = log_upm();
        const char *zn = r.cam ? "camara" : vrhit_zone_name(s.zone);
        char ex[320]; int ne = 0;
        ex[0] = 0;
        if (s.zone == VRHIT_Z_WALL) ne += snprintf(ex + ne, sizeof ex - (size_t)ne, " motivo=%s", r.src == 1 ? "juego" : r.src == 2 ? "pared_hzd" : r.src == 3 ? "suelo_hzd" : "otro");
        if (s.zone == VRHIT_Z_MISS) {
            int bi = -1; float bp = 0, bt = 0;      /* blanco más cercano al rayo (distancia perpendicular y recorrido hasta su punto más próximo) */
            for (int i = 0; i < n; i++) {
                float v[3] = {g_t[i].c[0] - o[0], g_t[i].c[1] - o[1], g_t[i].c[2] - o[2]}, tp = dot3(v, d), pp = dot3(v, v) - tp * tp;
                if (tp > 0.0f && (bi < 0 || pp < bp)) { bi = i; bp = pp; bt = tp; }
            }
            ne += snprintf(ex + ne, sizeof ex - (size_t)ne, " motivo=ninguno o=(%.0f,%.0f,%.0f) d=(%.3f,%.3f,%.3f)", o[0], o[1], o[2], d[0], d[1], d[2]);
            if (bi >= 0) ne += snprintf(ex + ne, sizeof ex - (size_t)ne, " mas_cercano=blanco %d a %.2f m de recorrido, %.2f m del rayo", g_t[bi].index, bt / upm, sqrtf(bp > 0.0f ? bp : 0.0f) / upm);
        }
        if (r.solid >= 0.0f && r.solid < s.dist - 1.0f) snprintf(ex + ne, sizeof ex - (size_t)ne, " escena=%.2f m(no bloquea)", r.solid / upm);
        fprintf(stderr, "[impacto] blanco=%d zona=%s dist=%.2f m vida=%d | bando=%s dano=%d accion=%s%s\n", s.target, zn, s.dist / upm, s.vital, "jugador", s.damage,
                s.action == VRHIT_ACT_SEAL ? "sellada" : s.action == VRHIT_ACT_VOID ? "anulada" : "intacta", ex);
    }
    {   /* láser frente a bala: dónde acababa el rayo del láser al disparar y dónde acaba la bala (mismo criterio, ver vrhit_shot_dist); desvío = distancia entre ambos puntos */
        const Note *nt = take_note(o, d);
        float upm = log_upm();
        if (nt) {
            float dv[3] = {nt->pt[0] - r.pt[0], nt->pt[1] - r.pt[1], nt->pt[2] - r.pt[2]}, dev = sqrtf(dot3(dv, dv));
            fprintf(stderr, "[disparo] laser=(%.0f,%.0f,%.0f) bala_impacto=(%.0f,%.0f,%.0f) desvio=%.0f uds (%.1f cm) | laser %s, bala %s (%s) alcance=%.1f m\n", nt->pt[0], nt->pt[1], nt->pt[2], r.pt[0], r.pt[1], r.pt[2],
                    dev, dev / upm * 100.0f, nt->hit ? "impacta" : "sin impacto", r.dist < R ? "impacta" : "sin impacto", vrhit_zone_name(s.zone), R / upm);
        } else {
            fprintf(stderr, "[disparo] laser=sin_dato bala_impacto=(%.0f,%.0f,%.0f) desvio=? | bala %s (%s) alcance=%.1f m\n", r.pt[0], r.pt[1], r.pt[2], r.dist < R ? "impacta" : "sin impacto", vrhit_zone_name(s.zone), R / upm);
        }
    }
    if (out) *out = s;
    return 1;
}
