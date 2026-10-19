/* Pruebas de ns_light: comparación bit a bit con la GTE de referencia (runtime/gte.c) y casos sintéticos. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ns_light.h"
#include "cpu.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; if (fails < 30) { printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }
static int rr(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }

static Cpu *CPU;   /* la GTE no usa el Cpu */
static void ld_mat(int base, const int16_t m[3][3]) {
    const int16_t *f = &m[0][0];
    for (int k = 0; k < 4; k++) gte_ctc2(CPU, base + k, (uint16_t)f[2 * k] | ((uint32_t)(uint16_t)f[2 * k + 1] << 16));
    gte_ctc2(CPU, base + 4, (uint16_t)f[8]);
}
static void ld_v(int reg, const int16_t v[3]) {
    gte_mtc2(CPU, reg, (uint16_t)v[0] | ((uint32_t)(uint16_t)v[1] << 16));
    gte_mtc2(CPU, reg + 1, (uint16_t)v[2]);
}

/* ---- isqrt: implementación independiente literal (tabla floor(512 sqrt) como el motor) ---- */
static uint32_t isqrt_ref(int32_t a) {
    if (a <= 0) return 0;
    int lz = __builtin_clz((uint32_t)a), r10 = lz & ~1, r9 = (31 - r10) >> 1;
    int32_t m = r10 < 24 ? a >> (24 - r10) : (int32_t)((uint32_t)a << (r10 - 24));
    int tbl = (int)floor(512.0 * sqrt((double)m) + 1e-9);
    return ((uint32_t)tbl << r9) >> 12;
}

static void t_isqrt(void) {
    for (int32_t a = 1; a < 200000; a++) CHECK(ns_light_isqrt(a) == isqrt_ref(a), "isqrt %d", a);
    for (int i = 0; i < 200000; i++) { int32_t a = (int32_t)(rnd() & 0x7FFFFFFF); CHECK(ns_light_isqrt(a) == isqrt_ref(a), "isqrt %d", a); }
    /* valores conocidos de la tabla del motor */
    CHECK(ns_light_isqrt(1) == 1 && ns_light_isqrt(4) == 2 && ns_light_isqrt(0) == 0 && ns_light_isqrt(-5) == 0, "isqrt basicos");
    /* aproxima floor(sqrt) con error relativo < 0,5 % */
    for (int i = 0; i < 100000; i++) {
        int32_t a = (int32_t)(rnd() & 0x3FFFFFFF) + 1000;
        double e = fabs((double)ns_light_isqrt(a) - sqrt((double)a)) / sqrt((double)a);
        CHECK(e < 0.01, "isqrt aprox %d %f", a, e);
    }
}

static void rand_light(NsLight *L, int realistic) {
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
        L->llm[i][j] = realistic ? (int16_t)rr(-4096, 4096) : (int16_t)rr(-32768, 32767);
        L->lcm[i][j] = realistic ? (int16_t)rr(0, 4080) : (int16_t)rr(-32768, 32767);
    }
    for (int i = 0; i < 3; i++) L->bk[i] = realistic ? rr(0, 4080) : rr(-100000, 100000);
}

static void t_ncs_vs_gte(void) {
    for (int it = 0; it < 200000; it++) {
        NsLight L; rand_light(&L, it & 1);
        int16_t n[3] = {(int16_t)rr(-4096, 4096), (int16_t)rr(-4096, 4096), (int16_t)rr(-4096, 4096)};
        if (it % 7 == 0) { n[0] = (int16_t)rr(-32768, 32767); n[1] = (int16_t)rr(-32768, 32767); n[2] = (int16_t)rr(-32768, 32767); }
        uint8_t code = (uint8_t)rr(0, 255);
        gte_reset();
        ld_mat(8, L.llm); ld_mat(16, L.lcm);
        for (int i = 0; i < 3; i++) gte_ctc2(CPU, 13 + i, (uint32_t)L.bk[i]);
        ld_v(0, n);
        gte_mtc2(CPU, 6, (uint32_t)code << 24 | 0x123456);
        gte_cmd(CPU, 0x00c8041e);                        /* NCS sf=1 lm=1 */
        uint32_t g = gte_mfc2(CPU, 22);
        uint8_t rgb[3]; ns_light_ncs(&L, n, rgb);
        CHECK((g & 0xFF) == rgb[0] && ((g >> 8) & 0xFF) == rgb[1] && ((g >> 16) & 0xFF) == rgb[2],
              "NCS it=%d gte=%06x ns=%02x%02x%02x", it, g & 0xFFFFFF, rgb[0], rgb[1], rgb[2]);
        int32_t mac[3]; ns_light_ncs_mac(&L, n, mac);
        for (int i = 0; i < 3; i++) CHECK((int32_t)gte_mfc2(CPU, 25 + i) == mac[i], "MAC%d it=%d", i + 1, it);
    }
}

static void t_float_vs_int(void) {
    double sum = 0, mx = 0; int cnt = 0;
    for (int it = 0; it < 200000; it++) {
        NsLight L; rand_light(&L, 1);
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) L.llm[i][j] = (int16_t)rr(-6000, 6000);
        float nf[3] = {(float)rr(-1000, 1000), (float)rr(-1000, 1000), (float)rr(-1000, 1000)};
        float l = sqrtf(nf[0] * nf[0] + nf[1] * nf[1] + nf[2] * nf[2]); if (l < 1) continue;
        int16_t n[3]; for (int i = 0; i < 3; i++) { nf[i] /= l; n[i] = (int16_t)lrintf(nf[i] * 4096.f); nf[i] = n[i] / 4096.f; }
        uint8_t rgb[3]; float t[3];
        ns_light_ncs(&L, n, rgb); ns_light_tint_f(&L, nf, t);
        for (int i = 0; i < 3; i++) {
            double e = t[i] * 128.0 - rgb[i]; cnt++;
            if (e < 0) e = -e;
            sum += e;
            if (e > mx) mx = e;
        }
    }
    printf("  float vs entero: error medio %.4f, maximo %.4f (en unidades de byte, 1/255)\n", sum / cnt, mx);
    CHECK(mx <= 1.0001, "tint float lejos del entero: %f", mx);
}

static void t_to_local_and_pos(void) {
    for (int it = 0; it < 100000; it++) {
        int16_t Lw[3][3], R[3][3], o[3][3];
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { Lw[i][j] = (int16_t)rr(-6000, 6000); R[i][j] = (int16_t)rr(-4096, 4096); }
        if (it % 11 == 0) for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { Lw[i][j] = (int16_t)rr(-32768, 32767); R[i][j] = (int16_t)rr(-32768, 32767); }
        ns_light_to_local(Lw, R, o);
        gte_reset(); ld_mat(0, Lw);
        for (int j = 0; j < 3; j++) {
            int16_t col[3] = {R[0][j], R[1][j], R[2][j]};
            gte_mtc2(CPU, 9, (uint16_t)col[0]); gte_mtc2(CPU, 10, (uint16_t)col[1]); gte_mtc2(CPU, 11, (uint16_t)col[2]);
            gte_cmd(CPU, 0x0049e012);                    /* RTIR sf=1 lm=0 */
            for (int i = 0; i < 3; i++) CHECK((int16_t)gte_mfc2(CPU, 9 + i) == o[i][j], "to_local it=%d", it);
        }
        int32_t T[3] = {rr(-30000, 30000), rr(-30000, 30000), rr(-30000, 30000)};
        int16_t v[3] = {(int16_t)rr(-32768, 32767), (int16_t)rr(-32768, 32767), (int16_t)rr(-32768, 32767)};
        int32_t w[3]; ns_light_world_pos(R, T, v, w);
        gte_reset(); ld_mat(0, R); for (int i = 0; i < 3; i++) gte_ctc2(CPU, 5 + i, (uint32_t)T[i]);
        ld_v(0, v); gte_cmd(CPU, 0x00480012);            /* RT sf=1 */
        for (int i = 0; i < 3; i++) CHECK((int32_t)gte_mfc2(CPU, 25 + i) == w[i], "world_pos it=%d", it);
    }
}

static void t_fog(void) {
    for (int it = 0; it < 100000; it++) {
        uint8_t c[3] = {(uint8_t)rr(0, 255), (uint8_t)rr(0, 255), (uint8_t)rr(0, 255)}, o[3];
        int f = rr(0, 4096);
        gte_reset(); gte_mtc2(CPU, 8, (uint32_t)f);
        gte_mtc2(CPU, 6, c[0] | (c[1] << 8) | (c[2] << 16) | (0x3Cu << 24));
        gte_cmd(CPU, 0x00780010);                        /* DPCS */
        uint32_t g = gte_mfc2(CPU, 22);
        ns_fog_apply(f, c, o);
        CHECK((g & 0xFF) == o[0] && ((g >> 8) & 0xFF) == o[1] && ((g >> 16) & 0xFF) == o[2], "DPCS f=%d", f);
    }
    NsFog F; ns_fog_init(&F, 50, 50);
    CHECK(ns_fog_factor(&F, 10) == 0 && ns_fog_factor(&F, 50) == 0 && ns_fog_factor(&F, 75) == 2048 &&
          ns_fog_factor(&F, 100) == 4096 && ns_fog_factor(&F, 200) == 4096, "rampa niebla");
    CHECK(!ns_fog_culled(&F, 99) && ns_fog_culled(&F, 100), "descarte niebla");
    ns_fog_set_start(&F, 230);   /* start+len0 > 255 -> len recortada, rampa con len0 */
    CHECK(F.len == 25 && ns_fog_culled(&F, 255) && !ns_fog_culled(&F, 254), "niebla recortada %d", F.len);
    CHECK(ns_fog_factor(&F, 240) == (10 * 4096) / 50, "rampa conserva len0");
    uint8_t a[3] = {200, 100, 0}, b[3]; ns_fog_apply(2048, a, b);
    CHECK(b[0] == 100 && b[1] == 50 && b[2] == 0, "mezcla a la mitad %d %d", b[0], b[1]);
    /* objeto: escala de la matriz de color y del ambiente */
    NsLight L; memset(&L, 0, sizeof L); L.lcm[0][0] = 4000; L.bk[0] = 100 * 16;
    int16_t amb[3] = {100, 0, 0}; NsLight O; ns_fog_light_object(&L, amb, 2048, &O);
    CHECK(O.lcm[0][0] == 2000 && O.bk[0] == 50 * 16, "niebla en objeto");
}

static void t_lights(void) {
    int32_t d[3] = {1000, 0, 0}; int16_t v[3];
    ns_light_point_vector(d, 1000, v);
    CHECK(abs(v[0] - 3072) < 40 && v[1] == 0 && v[2] == 0, "caida en r: %d", v[0]);
    d[0] = 0; ns_light_point_vector(d, 1000, v);
    CHECK(v[0] == 0 && v[1] == 0, "distancia 0");
    d[0] = 2000; ns_light_point_vector(d, 1000, v);
    CHECK(abs(v[0]) < 40, "fuera de 2r ~0: %d", v[0]);
    d[0] = -500; d[1] = 500; d[2] = 0; ns_light_point_vector(d, 1000, v);
    CHECK(v[0] < 0 && v[1] > 0 && v[0] == -v[1], "signo y simetria");
    ns_light_point_vector(d, 0, v); CHECK(v[0] == 0, "r=0");

    NsPointLight A[3] = {{{0, 0, 0}, 1000, 500, {255, 0, 0}}, {{10000, 0, 0}, 1000, 500, {0, 255, 0}}, {{0, 0, 100}, 1000, 500, {0, 0, 255}}};
    NsPointLight B[1] = {{{0, 0, 200}, 1000, 500, {10, 20, 30}}};
    NsLightList lists[2] = {{A, 3}, {B, 1}};
    int32_t pos[3] = {100, 0, 0}; int16_t vec[2][3]; uint8_t col[2][3];
    int n = ns_light_pick(lists, 2, pos, vec, col);
    CHECK(n == 2 && col[0][0] == 255 && col[1][2] == 255, "orden de seleccion");
    pos[0] = 9800; n = ns_light_pick(lists, 2, pos, vec, col);
    CHECK(n == 1 && col[0][1] == 255, "caja de influencia");
    pos[0] = 99999; n = ns_light_pick(lists, 2, pos, vec, col);
    CHECK(n == 0, "fuera de todo");
    pos[0] = 0; pos[1] = 501; n = ns_light_pick(lists, 2, pos, vec, col);
    CHECK(n == 0, "caja estricta en y");

    /* montaje: luz principal desde arriba, sin puntuales, ambiente 64, color principal 64*16 */
    NsLightEnv env = {{0, 4096, 0}, {1024, 1024, 1024}, {64, 64, 64}};
    NsLight L; int16_t I[3][3] = {{4096, 0, 0}, {0, 4096, 0}, {0, 0, 4096}};
    int32_t p0[3] = {0, 0, 0};
    ns_light_object(&env, 0, 0, p0, I, &L);
    int16_t up[3] = {0, 4096, 0}, dn[3] = {0, -4096, 0}; uint8_t c[3];
    ns_light_ncs(&L, up, c);   /* amb 64*16=1024 -> 64 + (1024*4096>>12)>>4=64 -> 128 */
    CHECK(c[0] == 128 && c[1] == 128 && c[2] == 128, "cara iluminada = neutro (%d)", c[0]);
    ns_light_ncs(&L, dn, c);
    CHECK(c[0] == 64, "cara en sombra = ambiente (%d)", c[0]);
    /* rotación de 90 grados en Z: el objeto "arriba" pasa a apuntar a +X en mundo; la normal local (0,4096,0) debe
       seguir iluminada si R lleva local-y a mundo-y... aqui R lleva local x -> mundo y: normal local (4096,0,0) ilumina */
    int16_t R[3][3] = {{0, -4096, 0}, {4096, 0, 0}, {0, 0, 4096}};   /* columnas = ejes locales en mundo */
    ns_light_object(&env, 0, 0, p0, R, &L);
    int16_t lx[3] = {4096, 0, 0}; ns_light_ncs(&L, lx, c);
    CHECK(c[0] == 128, "luz principal en espacio local rotado (%d)", c[0]);
    /* vértice de mapa con una luz puntual cercana, normal hacia (luz->vertice) */
    NsPointLight P[1] = {{{0, 0, 0}, 1000, 2000, {255, 128, 0}}};
    NsLightList pl[1] = {{P, 1}};
    NsLightEnv e2 = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    int32_t vw[3] = {500, 0, 0}; int16_t nn[3] = {4096, 0, 0};
    ns_light_preshade_vertex(&e2, pl, 1, vw, I, nn, c);
    CHECK(c[0] > 200 && c[1] > 90 && c[1] < c[0] && c[2] == 0, "preshade puntual %d %d %d", c[0], c[1], c[2]);
}

int main(void) {
    static Cpu cpu; CPU = &cpu;
    t_isqrt(); t_lights(); t_fog(); t_to_local_and_pos(); t_ncs_vs_gte(); t_float_vs_int();
    printf("%s: %d comprobaciones, %d fallos\n", fails ? "FALLO" : "OK", checks, fails);
    return fails != 0;
}
