/* Ver ns_light.h y docs/iluminacion_nativa.md */
#include "ns_light.h"
#include <math.h>

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---------------------------------------------------------------- color de una normal */
void ns_light_color_acc(const NsLight *L, const int16_t n[3], int32_t mac[3]) {
    int32_t ir[3];
    for (int i = 0; i < 3; i++) {
        int64_t t = (int64_t)L->lmat[i][0] * n[0] + (int64_t)L->lmat[i][1] * n[1] + (int64_t)L->lmat[i][2] * n[2];
        ir[i] = clampi((int32_t)(t >> 12), 0, 0x7FFF);       /* desplazamiento 12, mínimo 0 */
    }
    for (int i = 0; i < 3; i++) {
        int64_t t = (int64_t)L->bg[i] * 4096 + (int64_t)L->cmat[i][0] * ir[0] + (int64_t)L->cmat[i][1] * ir[1] +
                    (int64_t)L->cmat[i][2] * ir[2];
        mac[i] = (int32_t)(t >> 12);
    }
}

void ns_light_color(const NsLight *L, const int16_t n[3], uint8_t rgb[3]) {
    int32_t m[3];
    ns_light_color_acc(L, n, m);
    for (int i = 0; i < 3; i++) rgb[i] = (uint8_t)clampi(m[i] >> 4, 0, 255);
}

void ns_light_tint_f(const NsLight *L, const float n[3], float tint[3]) {
    float ir[3];
    for (int i = 0; i < 3; i++) {
        float d = (L->lmat[i][0] * n[0] + L->lmat[i][1] * n[1] + L->lmat[i][2] * n[2]) * (1.0f / 4096.0f);
        ir[i] = d < 0.f ? 0.f : d > 7.999756f ? 7.999756f : d;
    }
    for (int i = 0; i < 3; i++) {
        float c = L->bg[i] * (1.0f / 4096.0f) +
                  (L->cmat[i][0] * ir[0] + L->cmat[i][1] * ir[1] + L->cmat[i][2] * ir[2]) * (1.0f / 4096.0f);
        float t = floorf(c * 256.0f) * (1.0f / 128.0f);       /* byte = floor(c*256); factor = byte/128 */
        tint[i] = t < 0.f ? 0.f : t > 255.0f / 128.0f ? 255.0f / 128.0f : t;
    }
}

/* ---------------------------------------------------------------- espacios */
void ns_light_to_local(const int16_t Lw[3][3], const int16_t R[3][3], int16_t out[3][3]) {
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) {
            int64_t t = (int64_t)Lw[i][0] * R[0][j] + (int64_t)Lw[i][1] * R[1][j] + (int64_t)Lw[i][2] * R[2][j];
            out[i][j] = (int16_t)clampi((int32_t)(t >> 12), -0x8000, 0x7FFF);
        }
}

void ns_light_world_pos(const int16_t R[3][3], const int32_t T[3], const int16_t v[3], int32_t out[3]) {
    for (int i = 0; i < 3; i++) {
        int64_t t = (int64_t)T[i] * 4096 + (int64_t)R[i][0] * v[0] + (int64_t)R[i][1] * v[1] + (int64_t)R[i][2] * v[2];
        out[i] = (int32_t)(t >> 12);
    }
}

/* ---------------------------------------------------------------- luces puntuales */
/* Raíz del motor: lzcnt par -> mantisa de 8 bits (64..255) -> tabla floor(512*sqrt(m)) -> desplazamiento. */
uint32_t ns_light_isqrt(int32_t a) {
    if (a <= 0) return 0;
    int lz = __builtin_clz((uint32_t)a);
    int r10 = lz & ~1;
    int r9 = (31 - r10) >> 1;
    int32_t m = r10 < 24 ? (a >> (24 - r10)) : (int32_t)((uint32_t)a << (r10 - 24));   /* 64..255 */
    uint32_t x = (uint32_t)(m) << 18, r = 0, bit = 1u << 30;                             /* isqrt(m<<18) */
    while (bit > x) bit >>= 2;
    while (bit) {
        if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return (r << r9) >> 12;
}

void ns_light_point_vector(const int32_t d[3], int r_range, int16_t out[3]) {
    if (r_range == 0) { out[0] = out[1] = out[2] = 0; return; }
    int32_t q[3];
    uint64_t s = 0;
    for (int i = 0; i < 3; i++) { q[i] = (int16_t)d[i]; s += (uint64_t)((int64_t)q[i] * q[i]); }   /* truncado a 16 bits una vez */
    if (s > 0x7FFFFFFFu) s = 0x7FFFFFFFu;                    /* saturación (el original desbordaría con signo) */
    int32_t sq = (int32_t)ns_light_isqrt((int32_t)s);
    if (sq == 0) sq = 1;
    int32_t sq2 = r_range * 2 - sq;
    if (sq2 < 0) sq2 = 0;
    for (int i = 0; i < 3; i++) {
        int32_t v = (q[i] * 3072) / sq;                      /* |q*3072| <= 1e8 */
        v = (v * sq2) / r_range;                             /* |v| <= ~3100, sq2 <= 131070 */
        out[i] = (int16_t)v;
    }
}

int ns_light_pick(const NsLightList *lists, int nlists, const int32_t pos[3], int16_t vec[2][3], uint8_t col[2][3]) {
    int found = 0;
    for (int k = 0; k < nlists && found < 2; k++)
        for (int i = 0; i < lists[k].n && found < 2; i++) {
            const NsPointLight *p = &lists[k].l[i];
            int32_t e = p->e_range, d[3];
            int ok = 1;
            for (int a = 0; a < 3 && ok; a++) {
                d[a] = pos[a] - p->pos[a];
                if (d[a] < -e || d[a] > e) ok = 0;
            }
            if (!ok) continue;
            ns_light_point_vector(d, p->r_range, vec[found]);
            for (int a = 0; a < 3; a++) col[found][a] = p->col[a];
            found++;
        }
    return found;
}

void ns_light_assemble(const NsLightEnv *env, int nl, const int16_t vec[2][3], const uint8_t col[2][3],
                       const int16_t *amb, NsLight *o) {
    const int16_t *a = amb ? amb : env->ambient;
    for (int j = 0; j < 3; j++) {
        o->lmat[0][j] = env->main_dir[j];
        o->lmat[1][j] = nl > 0 ? vec[0][j] : 0;
        o->lmat[2][j] = nl > 1 ? vec[1][j] : 0;
    }
    for (int i = 0; i < 3; i++) {
        o->cmat[i][0] = env->main_col[i];
        o->cmat[i][1] = nl > 0 ? (int16_t)(col[0][i] << 4) : 0;
        o->cmat[i][2] = nl > 1 ? (int16_t)(col[1][i] << 4) : 0;
        o->bg[i] = (int32_t)a[i] * 16;
    }
}

void ns_light_object(const NsLightEnv *env, const NsLightList *lists, int nlists, const int32_t pos[3],
                     const int16_t R[3][3], NsLight *out) {
    int16_t vec[2][3] = {{0}}; uint8_t col[2][3] = {{0}};
    int nl = ns_light_pick(lists, nlists, pos, vec, col);
    NsLight w;
    ns_light_assemble(env, nl, vec, col, 0, &w);
    ns_light_to_local(w.lmat, R, out->lmat);
    for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) out->cmat[i][j] = w.cmat[i][j]; out->bg[i] = w.bg[i]; }
}

void ns_light_preshade_vertex(const NsLightEnv *env, const NsLightList *lists, int nlists, const int32_t vw[3],
                              const int16_t R[3][3], const int16_t n[3], uint8_t rgb[3]) {
    NsLight l;
    ns_light_object(env, lists, nlists, vw, R, &l);
    ns_light_color(&l, n, rgb);
}

/* ---------------------------------------------------------------- niebla */
void ns_fog_init(NsFog *f, int start, int len) {
    f->len0 = len < 1 ? 1 : len;
    ns_fog_set_start(f, start);                  /* recorta len a 255-start */
}
void ns_fog_set_start(NsFog *f, int start) {
    if (start > 255) start = 255;
    if (start < 0) start = 0;
    f->start = start;
    f->len = (start + f->len0 > 255) ? 255 - start : f->len0;
}
int ns_fog_factor(const NsFog *f, int zi) {
    int k = zi - f->start;
    if (k < 0) return 0;
    if (k < f->len0) return (k * 4096) / f->len0;
    if (k == 255) return 0;     /* hueco de la tabla del motor (255 entradas escritas de 256) */
    return 4096;
}
int ns_fog_culled(const NsFog *f, int zi) { return zi >= f->start + f->len; }

/* Mezcla de profundidad con color lejano 0: ir = clamp16(-(c*65536)>>12); mac = (ir*f + c*65536)>>12; byte = mac>>4. */
void ns_fog_apply(int f, const uint8_t in[3], uint8_t out[3]) {
    f = (int)clampi(f, 0, 4096);
    for (int i = 0; i < 3; i++) {
        int64_t m = (int64_t)in[i] * 65536;
        int32_t ir = clampi((int32_t)((0 - m) >> 12), -0x8000, 0x7FFF);
        int32_t mac = (int32_t)((((int64_t)ir * f) + m) >> 12);
        out[i] = (uint8_t)clampi(mac >> 4, 0, 255);
    }
}

void ns_fog_light_object(const NsLight *in, const int16_t amb[3], int f, NsLight *out) {
    int s = 4096 - (int)clampi(f, 0, 4096);
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) out->cmat[i][j] = (int16_t)((in->cmat[i][j] * s) >> 12);
        out->bg[i] = ((amb[i] * s) / 4096) * 16;
        for (int j = 0; j < 3; j++) out->lmat[i][j] = in->lmat[i][j];
    }
}
