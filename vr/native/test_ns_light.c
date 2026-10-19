/* Pruebas de ns_light: comparación bit a bit con la unidad de geometría de referencia del runtime y casos sintéticos. */
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

static Cpu *CPU;   /* la unidad de referencia no usa el Cpu */
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
    double emax = 0;
    for (int i = 0; i < 300000; i++) {
        int32_t a = (int32_t)(rnd() & 0x7FFFFFFF) + 1000;
        if (a < 0) continue;
        double e = fabs((double)ns_light_isqrt(a) - sqrt((double)a)) / sqrt((double)a);
        if (e > emax) emax = e;
    }
    printf("  isqrt: error relativo maximo %.4f %%\n", emax * 100);
    CHECK(emax < 0.009, "isqrt aprox (umbral 0,9 %% del doc) %f", emax);
}

static void rand_light(NsLight *L, int realistic) {
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
        L->lmat[i][j] = realistic ? (int16_t)rr(-4096, 4096) : (int16_t)rr(-32768, 32767);
        L->cmat[i][j] = realistic ? (int16_t)rr(0, 4080) : (int16_t)rr(-32768, 32767);
    }
    for (int i = 0; i < 3; i++) L->bg[i] = realistic ? rr(0, 4080) : rr(-100000, 100000);
}

static void t_color_vs_ref(void) {
    for (int it = 0; it < 200000; it++) {
        NsLight L; rand_light(&L, it & 1);
        int16_t n[3] = {(int16_t)rr(-4096, 4096), (int16_t)rr(-4096, 4096), (int16_t)rr(-4096, 4096)};
        if (it % 7 == 0) { n[0] = (int16_t)rr(-32768, 32767); n[1] = (int16_t)rr(-32768, 32767); n[2] = (int16_t)rr(-32768, 32767); }
        uint8_t code = (uint8_t)rr(0, 255);
        gte_reset();
        ld_mat(8, L.lmat); ld_mat(16, L.cmat);
        for (int i = 0; i < 3; i++) gte_ctc2(CPU, 13 + i, (uint32_t)L.bg[i]);
        ld_v(0, n);
        gte_mtc2(CPU, 6, (uint32_t)code << 24 | 0x123456);
        gte_cmd(CPU, 0x00c8041e);                        /* color por normal */
        uint32_t g = gte_mfc2(CPU, 22);
        uint8_t rgb[3]; ns_light_color(&L, n, rgb);
        CHECK((g & 0xFF) == rgb[0] && ((g >> 8) & 0xFF) == rgb[1] && ((g >> 16) & 0xFF) == rgb[2],
              "color it=%d ref=%06x ns=%02x%02x%02x", it, g & 0xFFFFFF, rgb[0], rgb[1], rgb[2]);
        int32_t mac[3]; ns_light_color_acc(&L, n, mac);
        for (int i = 0; i < 3; i++) CHECK((int32_t)gte_mfc2(CPU, 25 + i) == mac[i], "acc%d it=%d", i + 1, it);
    }
}

static void t_float_vs_int(void) {
    /* modo 0: valores reales del motor; 1: cmat negativos; 2: lmat completos (satura ir hasta 7.999756) y cmat completos */
    for (int mode = 0; mode < 3; mode++) {
        double sum = 0, mx = 0; int cnt = 0, sat = 0;
        for (int it = 0; it < 200000; it++) {
            NsLight L; rand_light(&L, 1);
            for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
                L.lmat[i][j] = (int16_t)(mode == 2 ? rr(-32768, 32767) : rr(-6000, 6000));
                if (mode == 1) L.cmat[i][j] = (int16_t)rr(-4080, 4080);
                if (mode == 2) L.cmat[i][j] = (int16_t)rr(-32768, 32767);
            }
            if (mode == 2) for (int i = 0; i < 3; i++) L.bg[i] = rr(-100000, 100000);
            float nf[3] = {(float)rr(-1000, 1000), (float)rr(-1000, 1000), (float)rr(-1000, 1000)};
            float l = sqrtf(nf[0] * nf[0] + nf[1] * nf[1] + nf[2] * nf[2]); if (l < 1) continue;
            int16_t n[3]; for (int i = 0; i < 3; i++) { nf[i] /= l; n[i] = (int16_t)lrintf(nf[i] * 4096.f); nf[i] = n[i] / 4096.f; }
            for (int i = 0; i < 3; i++) {
                int32_t d = (L.lmat[i][0] * n[0] + L.lmat[i][1] * n[1] + L.lmat[i][2] * n[2]) >> 12;
                if (d > 0x7FFF) sat++;
            }
            uint8_t rgb[3]; float t[3];
            ns_light_color(&L, n, rgb); ns_light_tint_f(&L, nf, t);
            for (int i = 0; i < 3; i++) {
                double e = t[i] * 128.0 - rgb[i]; cnt++;
                if (e < 0) e = -e;
                sum += e;
                if (e > mx) mx = e;
            }
        }
        printf("  float vs entero (modo %d, %d saturaciones de ir): error medio %.4f, maximo %.4f bytes\n", mode, sat, sum / cnt, mx);
        /* modo 0: redondeos de la cadena entera (<= 1 byte). Con cmat negativos o enormes el piso de ir se amplifica
           por |cmat| (hasta 8x): cota teorica 3*8*(1/4096)*256/1 ~ 1,5 byte + 1 */
        CHECK(mode == 0 ? mx <= 1.0001 : mx <= 3.0, "tint float lejos del entero (modo %d): %f", mode, mx);
        if (mode == 2) CHECK(sat > 1000, "el modo 2 debe saturar ir");
    }
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
            gte_cmd(CPU, 0x0049e012);                    /* producto matriz x vector de IR */
            for (int i = 0; i < 3; i++) CHECK((int16_t)gte_mfc2(CPU, 9 + i) == o[i][j], "to_local it=%d", it);
        }
        int32_t T[3] = {rr(-30000, 30000), rr(-30000, 30000), rr(-30000, 30000)};
        int16_t v[3] = {(int16_t)rr(-32768, 32767), (int16_t)rr(-32768, 32767), (int16_t)rr(-32768, 32767)};
        int32_t w[3]; ns_light_world_pos(R, T, v, w);
        gte_reset(); ld_mat(0, R); for (int i = 0; i < 3; i++) gte_ctc2(CPU, 5 + i, (uint32_t)T[i]);
        ld_v(0, v); gte_cmd(CPU, 0x00480012);            /* transformación */
        for (int i = 0; i < 3; i++) CHECK((int32_t)gte_mfc2(CPU, 25 + i) == w[i], "world_pos it=%d", it);
    }
}

static void t_fog(void) {
    for (int it = 0; it < 100000; it++) {
        uint8_t c[3] = {(uint8_t)rr(0, 255), (uint8_t)rr(0, 255), (uint8_t)rr(0, 255)}, o[3];
        int f = rr(0, 4096);
        gte_reset(); gte_mtc2(CPU, 8, (uint32_t)f);
        gte_mtc2(CPU, 6, c[0] | (c[1] << 8) | (c[2] << 16) | (0x3Cu << 24));
        gte_cmd(CPU, 0x00780010);                        /* mezcla de profundidad */
        uint32_t g = gte_mfc2(CPU, 22);
        ns_fog_apply(f, c, o);
        CHECK((g & 0xFF) == o[0] && ((g >> 8) & 0xFF) == o[1] && ((g >> 16) & 0xFF) == o[2], "mezcla f=%d", f);
    }
    {   /* start+len > 255 ya en init: len se recorta a 255-start */
        NsFog G; ns_fog_init(&G, 230, 50);
        CHECK(G.start == 230 && G.len == 25 && G.len0 == 50 && ns_fog_culled(&G, 255) && !ns_fog_culled(&G, 254), "init recorta len %d", G.len);
        ns_fog_init(&G, 300, 50); CHECK(G.start == 255 && G.len == 0, "start>255");
        ns_fog_init(&G, -5, 0); CHECK(G.start == 0 && G.len0 == 1, "start<0, len<1");
        uint8_t in[3] = {200, 100, 50}, o1[3], o2[3];
        ns_fog_apply(-100, in, o1); ns_fog_apply(0, in, o2);
        CHECK(!memcmp(o1, o2, 3) && o2[0] == 200, "f<0 recortado a 0");
        ns_fog_apply(9999, in, o1); CHECK(o1[0] == 0 && o1[1] == 0 && o1[2] == 0, "f>4096 recortado a 4096");
        NsLight A, Bq; memset(&A, 0, sizeof A); A.cmat[0][0] = 4000; int16_t am[3] = {100, 0, 0};
        ns_fog_light_object(&A, am, -5, &Bq); CHECK(Bq.cmat[0][0] == 4000, "fog_light f<0");
        ns_fog_light_object(&A, am, 99999, &Bq); CHECK(Bq.cmat[0][0] == 0 && Bq.bg[0] == 0, "fog_light f>4096");
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
    NsLight L; memset(&L, 0, sizeof L); L.cmat[0][0] = 4000; L.bg[0] = 100 * 16;
    int16_t amb[3] = {100, 0, 0}; NsLight O; ns_fog_light_object(&L, amb, 2048, &O);
    CHECK(O.cmat[0][0] == 2000 && O.bg[0] == 50 * 16, "niebla en objeto");
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
    /* casos limite: componentes enormes (la suma de cuadrados desborda int32), negativas y truncado a 16 bits */
    {
        int32_t big[3] = {30000, 30000, 30000}; ns_light_point_vector(big, 40000, v);
        CHECK(v[0] == v[1] && v[1] == v[2] && v[0] > 0, "d=30000x3 sin desbordar: %d %d %d", v[0], v[1], v[2]);
        int32_t mn[3] = {-32768, -32768, -32768}; ns_light_point_vector(mn, 65535, v);
        CHECK(v[0] == v[1] && v[1] == v[2] && v[0] < 0, "d=-32768x3: %d", v[0]);
        int32_t tr[3] = {65536 + 1000, 0, 0}, t0[3] = {1000, 0, 0}; int16_t v2[3];
        ns_light_point_vector(tr, 1000, v); ns_light_point_vector(t0, 1000, v2);
        CHECK(v[0] == v2[0], "truncado a 16 bits coherente en distancia y numerador");
        int32_t z[3] = {0, 0, 0}; ns_light_point_vector(z, 65535, v);
        CHECK(v[0] == 0 && v[1] == 0 && v[2] == 0, "d=0 con r grande");
        int32_t one[3] = {1, 0, 0}; ns_light_point_vector(one, 1, v);
        CHECK(abs(v[0]) <= 4000, "r=1");
        CHECK(ns_light_isqrt(0x7FFFFFFF) > 46000 && ns_light_isqrt(0x7FFFFFFF) < 47000, "isqrt maximo %u", ns_light_isqrt(0x7FFFFFFF));
    }

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
    ns_light_color(&L, up, c);   /* amb 64*16=1024 -> 64 + (1024*4096>>12)>>4=64 -> 128 */
    CHECK(c[0] == 128 && c[1] == 128 && c[2] == 128, "cara iluminada = neutro (%d)", c[0]);
    ns_light_color(&L, dn, c);
    CHECK(c[0] == 64, "cara en sombra = ambiente (%d)", c[0]);
    /* rotación de 90 grados en Z: el objeto "arriba" pasa a apuntar a +X en mundo; la normal local (0,4096,0) debe
       seguir iluminada si R lleva local-y a mundo-y... aqui R lleva local x -> mundo y: normal local (4096,0,0) ilumina */
    int16_t R[3][3] = {{0, -4096, 0}, {4096, 0, 0}, {0, 0, 4096}};   /* columnas = ejes locales en mundo */
    ns_light_object(&env, 0, 0, p0, R, &L);
    int16_t lx[3] = {4096, 0, 0}; ns_light_color(&L, lx, c);
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
    t_isqrt(); t_lights(); t_fog(); t_to_local_and_pos(); t_color_vs_ref(); t_float_vs_int();
    printf("%s: %d comprobaciones, %d fallos\n", fails ? "FALLO" : "OK", checks, fails);
    return fails != 0;
}
