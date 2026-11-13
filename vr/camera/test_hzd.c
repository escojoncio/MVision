/* Pruebas de vr_hzd (RAM sintética; con VRHZD_DUMPS=<carpeta de volcados> también los volcados reales de gateo). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "vr_hzd.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static uint8_t ram[0x200000];
static void w32(uint32_t a, uint32_t v) { memcpy(ram + (a & 0x1FFFFF), &v, 4); }
static void w16(uint32_t a, int16_t v) { memcpy(ram + (a & 0x1FFFFF), &v, 2); }

/* conducto sintético: paredes z = -9000 y z = -10000 (x 1500..12500), base 7000, alto 2000 */
static void build(void) {
    memset(ram, 0, sizeof ram);
    uint32_t ctl = 0x8015d914, map = 0x800b8ab8, wr = 0x8015cde4, hdr = 0x80149268, grp = 0x80149284, walls = 0x8014929c, flags = 0x801492c0;
    w32(0x800ACB84, ctl); w32(ctl + 0x2C, map); w32(map + 8, wr); w32(wr, hdr); w32(0x800ACB38, 1);
    w16(hdr + 10, 1); w32(hdr + 16, grp);
    w16(grp + 2, 2); w32(grp + 8, walls); w32(grp + 20, flags);
    int16_t w[2][8] = {{1500, -9000, 7000, 2000, 12500, -9000, 7000, 2000}, {1500, -10000, 7000, 2000, 12500, -10000, 7000, 2000}};
    for (int i = 0; i < 2; i++) for (int k = 0; k < 8; k++) w16(walls + 16 * i + 2 * k, w[i][k]);
}

static void test_synth(void) {
    build();
    float ex = 5481, ez = -8996;                      /* ojo 4 uds al otro lado de la pared z=-9000 (d0007) */
    int n = vrhzd_clamp_eye(ram, 5338, -9461, &ex, &ez, 7745, 200.0f);
    CHECK(n > 0 && ez <= -9200.0f + 0.5f && ez > -9300.0f, "ojo fuera de la pared: ez=%.1f (esperado -9200)", ez);
    CHECK(fabsf(ex - 5481) < 0.5f, "solo se corrige en la normal (ex=%.1f)", ex);
    ex = 5400; ez = -9500;                            /* libre: no cambia */
    n = vrhzd_clamp_eye(ram, 5338, -9461, &ex, &ez, 7745, 200.0f);
    CHECK(n == 0 && ex == 5400 && ez == -9500, "ojo libre sin cambio (n=%d %.1f %.1f)", n, ex, ez);
    ex = 5400; ez = -9950;                            /* pegado a la pared z=-10000 por dentro: se aparta a 200 */
    vrhzd_clamp_eye(ram, 5338, -9461, &ex, &ez, 7745, 200.0f);
    CHECK(fabsf(ez + 9800.0f) < 0.5f, "se aparta de la pared lejana ez=%.1f", ez);
    ex = 5481; ez = -8996;                            /* altura fuera del tramo de la pared: no actua */
    n = vrhzd_clamp_eye(ram, 5338, -9461, &ex, &ez, 12000, 200.0f);
    CHECK(n == 0, "pared por debajo del ojo no cuenta");
    w32(0x800ACB84, 0);                               /* sin control: no toca nada */
    ex = 5481; ez = -8996;
    n = vrhzd_clamp_eye(ram, 5338, -9461, &ex, &ez, 7745, 200.0f);
    CHECK(n == 0 && ez == -8996, "sin datos HZD no cambia");
}

static void test_dumps(const char *root) {
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        char p[1024]; snprintf(p, sizeof p, "%s/%s/ram.bin", root, e->d_name);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        size_t got = fread(ram, 1, sizeof ram, f); fclose(f);
        snprintf(p, sizeof p, "%s/%s/meta.txt", root, e->d_name);
        f = fopen(p, "r");
        if (!f || got != sizeof ram) { if (f) fclose(f); continue; }
        char line[256]; int game = 0, post = -1; float px = 0, pz = 0, ex0 = 0, ez0 = 0, ey = 0;
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "game=", 5)) game = atoi(line + 5);
            else if (!strncmp(line, "posture=", 8)) post = atoi(line + 8);
            else if (!strncmp(line, "player_x=", 9)) px = (float)atof(line + 9);
            else if (!strncmp(line, "player_z=", 9)) pz = (float)atof(line + 9);
            else if (!strncmp(line, "eye_x=", 6)) ex0 = (float)atof(line + 6);
            else if (!strncmp(line, "eye_y=", 6)) ey = (float)atof(line + 6);
            else if (!strncmp(line, "eye_z=", 6)) ez0 = (float)atof(line + 6);
        }
        fclose(f);
        if (!game) continue;
        float ex = ex0, ez = ez0;
        int n = vrhzd_clamp_eye(ram, (int32_t)px, (int32_t)pz, &ex, &ez, ey, 198.0f);
        printf("  %s posture=%d eye=(%.0f,%.0f) -> (%.0f,%.0f) pushes=%d\n", e->d_name, post, ex0, ez0, ex, ez, n);
        if (post == 2 && strstr(e->d_name, "d0007")) CHECK(n > 0 && ez < ez0 - 150.0f, "d0007: el ojo debe volver dentro del conducto (ez %.0f -> %.0f)", ez0, ez);
        if (post == 2 && strstr(e->d_name, "d0006")) CHECK(n == 0, "d0006: ojo ya libre, sin cambio");
        if (post == 0) CHECK(n == 0, "%s: de pie sin cambio (n=%d)", e->d_name, n);
    }
    closedir(d);
}


/* ---- vrhzd_raycast: primer impacto de un rayo con las paredes de colisión del juego ---- */
static float rc(float ox, float oy, float oz, float dx, float dy, float dz, float mx, unsigned skip) {
    float o[3] = {ox, oy, oz}, d[3] = {dx, dy, dz};
    return vrhzd_raycast(ram, o, d, mx, skip);
}
static void test_ray_synth(void) {
    build();                                          /* paredes z=-9000 y z=-10000, x 1500..12500, y 7000..9000 */
    float t = rc(5000, 7500, -9500, 0, 0, 1, 15000, 0);
    CHECK(fabsf(t - 500.0f) < 0.5f, "hacia +z: pared a 500 (t=%.2f)", t);
    t = rc(5000, 7500, -9500, 0, 0, -1, 15000, 0);
    CHECK(fabsf(t - 500.0f) < 0.5f, "hacia -z: pared a 500 (t=%.2f)", t);
    t = rc(5000, 7500, -9500, 0, 0, 3, 15000, 0);
    CHECK(fabsf(t - 500.0f) < 0.5f, "dirección no unitaria: misma distancia (t=%.2f)", t);
    t = rc(5000, 7500, -9500, 0.70710678f, 0, 0.70710678f, 15000, 0);
    CHECK(fabsf(t - 707.1f) < 1.0f, "diagonal: 500*sqrt2 (t=%.2f)", t);
    t = rc(5000, 7500, -9500, 0.6f, 0.0f, 0.8f, 15000, 0);
    CHECK(fabsf(t - 625.0f) < 0.5f, "oblicuo 3-4-5: 500/0,8 (t=%.2f)", t);
    CHECK(rc(5000, 7500, -9500, 1, 0, 0, 15000, 0) < 0, "paralelo a las paredes (a lo largo del conducto): sin impacto");
    CHECK(rc(5000, 7500, -9500, 0, 1, 0, 15000, 0) < 0, "vertical: sin impacto con paredes verticales");
    CHECK(rc(5000, 9500, -9500, 0, 0, 1, 15000, 0) < 0, "por encima del borde superior de la pared: pasa");
    CHECK(rc(5000, 6500, -9500, 0, 0, 1, 15000, 0) < 0, "por debajo del borde inferior: pasa");
    CHECK(rc(5000, 6500, -9500, 0, 2, 1, 15000, 0) > 0, "subiendo: entra en la franja vertical y choca");
    CHECK(rc(13000, 7500, -9500, 0, 0, 1, 15000, 0) < 0, "más allá del extremo de la pared (x>12500): pasa");
    CHECK(rc(5000, 7500, -9500, 0, 0, 1, 400, 0) < 0, "tope de alcance menor que la pared: sin impacto");
    CHECK(rc(5000, 7500, -9500, 0, 0, 1, 501, 0) > 0, "tope justo por encima: impacto");
    /* monotonía: avanzar el origen hacia la pared acorta la distancia exactamente lo avanzado */
    float prev = 1e9f; int mono = 1;
    for (int k = 0; k < 10; k++) {
        float a = 40.0f * (float)k, tt = rc(5000, 7500, -9500 + a, 0, 0, 1, 15000, 0);
        if (!(tt > 0 && fabsf(tt - (500.0f - a)) < 0.5f && tt < prev)) mono = 0;
        prev = tt;
    }
    CHECK(mono, "acercar el origen a la pared: distancia positiva, estrictamente decreciente y exacta");
    /* máscara de banderas: la pared con bit en la máscara se ignora (como la función de línea de visión del juego) */
    uint32_t flags = 0x801492c0;
    ram[(flags + 0) & 0x1FFFFF] = 4;                  /* pared 0 (z=-9000) con el bit 4 */
    CHECK(rc(5000, 7500, -9500, 0, 0, 1, 15000, 0) > 0, "sin máscara cuenta toda pared");
    CHECK(rc(5000, 7500, -9500, 0, 0, 1, 15000, 4) < 0, "máscara 4: la pared marcada con 4 se ignora");
    CHECK(rc(5000, 7500, -9500, 0, 0, -1, 15000, 4) > 0, "máscara 4: la otra pared sigue valiendo");
    ram[(flags + 0) & 0x1FFFFF] = 0;
    uint32_t mask = 0; memcpy(&mask, ram + (0x800ACB38u & 0x1FFFFF), 4);
    w32(0x800ACB38, 0);
    CHECK(rc(5000, 7500, -9500, 0, 0, 1, 15000, 0) < 0, "grupo inactivo (máscara de grupos 0): sin paredes");
    w32(0x800ACB38, mask);
    /* entradas inválidas */
    float o[3] = {5000, 7500, -9500}, d[3] = {0, 0, 1}, nan3[3] = {NAN, 0, 1}, z3[3] = {0, 0, 0};
    CHECK(vrhzd_raycast(NULL, o, d, 15000, 0) < 0, "sin RAM");
    CHECK(vrhzd_raycast(ram, NULL, d, 15000, 0) < 0 && vrhzd_raycast(ram, o, NULL, 15000, 0) < 0, "sin origen o dirección");
    CHECK(vrhzd_raycast(ram, o, nan3, 15000, 0) < 0 && vrhzd_raycast(ram, nan3, d, 15000, 0) < 0, "no finitos");
    CHECK(vrhzd_raycast(ram, o, z3, 15000, 0) < 0, "dirección nula");
    CHECK(vrhzd_raycast(ram, o, d, 0, 0) < 0 && vrhzd_raycast(ram, o, d, NAN, 0) < 0, "alcance nulo o no finito");
    w32(0x800ACB84, 0);
    CHECK(rc(5000, 7500, -9500, 0, 0, 1, 15000, 0) < 0, "sin datos HZD: sin impacto (el llamante deja los 15 m)");
    /* espacio abierto: una pared lejana fuera del alcance */
    build();
    CHECK(rc(5000, 7500, -9500, 1, 0, 0, 15000, 0) < 0 && rc(5000, 7500, -9500, 0, 0, 1, 100, 0) < 0, "espacio abierto: sin impacto");
}

/* ---- segmentos dinámicos (puertas): la bala del juego (HZD_CHK_ALL) para en ellos y el láser también ---- */
static void test_ray_door(void) {
    build();                                          /* paredes fijas z=-9000 (a 500 del origen) y z=-10000 */
    uint32_t wr = 0x8015cde4, list = 0x80150000, door = 0x80150100, dflag = 0x80150200;
    int16_t dw[8] = {4000, -9200, 7000, 2000, 6000, -9200, 7000, 2000};      /* puerta z=-9200 (a 300), x 4000..6000 */
    for (int k = 0; k < 8; k++) w16(door + 2 * k, dw[k]);
    w32(list, door);
    w16(wr + 10, 1); w16(wr + 18, 4); w32(wr + 32, list); w32(wr + 36, dflag);
    float t = rc(5000, 7500, -9500, 0, 0, 1, 15000, 0);
    CHECK(fabsf(t - 300.0f) < 0.5f, "puerta dinámica a 300 delante de la pared fija a 500 (t=%.2f)", t);
    t = rc(5000, 7500, -9500, 0, 0, 1, 15000, 4);
    CHECK(fabsf(t - 300.0f) < 0.5f, "máscara 4 sin bit en la bandera de la puerta: cuenta (t=%.2f)", t);
    ram[dflag & 0x1FFFFF] = 4;
    t = rc(5000, 7500, -9500, 0, 0, 1, 15000, 4);
    CHECK(fabsf(t - 500.0f) < 0.5f, "máscara 4 con la puerta marcada con 4: se ignora y vale la fija (t=%.2f)", t);
    ram[dflag & 0x1FFFFF] = 0;
    CHECK(rc(7000, 7500, -9500, 0, 0, 1, 15000, 0) > 499.0f, "fuera del ancho de la puerta: la pared fija");
    CHECK(rc(5000, 7500, -9500, 0, 0, 1, 250, 0) < 0, "puerta más allá del tope de alcance: sin impacto");
    w16(wr + 10, 0);
    t = rc(5000, 7500, -9500, 0, 0, 1, 15000, 0);
    CHECK(fabsf(t - 500.0f) < 0.5f, "sin segmentos dinámicos: la pared fija (t=%.2f)", t);
    w16(wr + 10, 99);                                 /* contador corrupto (> máximo): se ignora */
    t = rc(5000, 7500, -9500, 0, 0, 1, 15000, 0);
    CHECK(fabsf(t - 500.0f) < 0.5f, "contador dinámico corrupto: se ignora (t=%.2f)", t);
}

/* ---- vrhzd_wall_dist: distancia de un punto a la pared de colisión más cercana (golpe en la pared estando pegado a ella) ---- */
static float wd(float x, float y, float z, float mx) { float p[3] = {x, y, z}; return vrhzd_wall_dist(ram, p, mx); }
static void test_wall_dist(void) {
    build();                                          /* paredes z=-9000 y z=-10000, x 1500..12500, y 7000..9000 */
    CHECK(fabsf(wd(5000, 7500, -9500, 600) - 500.0f) < 0.5f, "a mitad del conducto: 500 de cada pared (d=%.2f)", wd(5000, 7500, -9500, 600));
    CHECK(wd(5000, 7500, -9500, 400) < 0, "fuera del alcance: -1");
    CHECK(fabsf(wd(5000, 7500, -9100, 600) - 100.0f) < 0.5f, "a 100 de la pared z=-9000");
    CHECK(fabsf(wd(5000, 7500, -8900, 600) - 100.0f) < 0.5f, "al otro lado de la pared: misma distancia (sin signo)");
    CHECK(fabsf(wd(13000, 7500, -9100, 800) - sqrtf(500.0f * 500.0f + 100.0f * 100.0f)) < 0.5f, "más allá del extremo: distancia al extremo");
    CHECK(wd(5000, 9500, -9100, 600) < 0 && wd(5000, 6500, -9100, 600) < 0, "fuera de la franja vertical: no cuenta");
    CHECK(wd(5000, 7500, -9500, NAN) < 0 && wd(5000, 7500, -9500, 0) < 0, "alcance inválido");
    float nan3[3] = {NAN, 7500, -9500};
    CHECK(vrhzd_wall_dist(ram, nan3, 600) < 0 && vrhzd_wall_dist(ram, NULL, 600) < 0 && vrhzd_wall_dist(NULL, nan3, 600) < 0, "entradas inválidas");
    uint32_t mask = 0; memcpy(&mask, ram + (0x800ACB38u & 0x1FFFFF), 4);
    w32(0x800ACB38, 0);
    CHECK(wd(5000, 7500, -9100, 600) < 0, "grupo inactivo: sin paredes");
    w32(0x800ACB38, mask);
    w32(0x800ACB84, 0);
    CHECK(wd(5000, 7500, -9100, 600) < 0, "sin datos HZD: -1");
}

static void test_wall_near(void) {
    build();                                          /* paredes z=-9000 y z=-10000, x 1500..12500, y 7000..9000 */
    float n[2] = {7, 7}, p1[3] = {5000, 7500, -9100}, p2[3] = {5000, 7500, -9900}, p3[3] = {13000, 7500, -9100}, p4[3] = {5000, 7500, -9000}, p5[3] = {5000, 9500, -9100};
    float d = vrhzd_wall_near(ram, p1, 600, n);
    CHECK(fabsf(d - 100.0f) < 0.5f && fabsf(n[0]) < 1e-5f && fabsf(n[1] + 1.0f) < 1e-5f, "lado libre de z=-9000 hacia -z: d=%.2f n=(%.2f,%.2f)", d, n[0], n[1]);
    d = vrhzd_wall_near(ram, p2, 600, n);
    CHECK(fabsf(d - 100.0f) < 0.5f && fabsf(n[0]) < 1e-5f && fabsf(n[1] - 1.0f) < 1e-5f, "pared z=-10000: normal hacia +z");
    d = vrhzd_wall_near(ram, p3, 800, n);
    CHECK(fabsf(d - sqrtf(500.0f * 500.0f + 100.0f * 100.0f)) < 0.5f && fabsf(n[1] + 1.0f) < 1e-5f, "más allá del extremo: misma normal de la pared");
    n[0] = n[1] = 7;
    CHECK(vrhzd_wall_near(ram, p1, 50, n) < 0 && n[0] == 7 && n[1] == 7, "fuera del alcance: -1 y n sin tocar");
    CHECK(vrhzd_wall_near(ram, p4, 600, n) < 0 || fabsf(n[1] - 1.0f) < 1e-5f, "sobre una pared: no es la del lado libre");
    CHECK(vrhzd_wall_near(ram, p5, 600, n) < 0, "fuera de la franja vertical");
    CHECK(vrhzd_wall_near(ram, p1, 600, NULL) < 0 && vrhzd_wall_near(NULL, p1, 600, n) < 0, "sin n o sin RAM");
}

static int meta_f(const char *dir, const char *key, float *v) {
    char p[1024], line[256]; snprintf(p, sizeof p, "%s/meta.txt", dir);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    size_t kl = strlen(key); int ok = 0;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, key, kl) && line[kl] == '=') { *v = (float)atof(line + kl + 1); ok = 1; break; }
    fclose(f); return ok;
}
/* Volcados reales (RAM del juego): rayos horizontales en 72 direcciones desde el ojo. Los datos del volcado no son de confianza: solo se leen. */
static void test_ray_dumps(const char *root) {
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *e;
    int dumps = 0, closed = 0;
    while ((e = readdir(d))) {
        if (!strstr(e->d_name, "_frame") && strncmp(e->d_name, "auto", 4) && strncmp(e->d_name, "prov_", 5)) continue;
        char dir[1024], p[1100]; snprintf(dir, sizeof dir, "%s/%s", root, e->d_name);
        snprintf(p, sizeof p, "%s/ram.bin", dir);
        FILE *f = fopen(p, "rb"); if (!f) continue;
        size_t got = fread(ram, 1, sizeof ram, f); fclose(f);
        float game = 0, ex, ey, ez;
        if (got != sizeof ram || !meta_f(dir, "game", &game) || game < 1 || !meta_f(dir, "eye_x", &ex) || !meta_f(dir, "eye_y", &ey) || !meta_f(dir, "eye_z", &ez)) continue;
        dumps++;
        int hits = 0, bad = 0, nonmono = 0, subset = 0; float tmin = 1e9f;
        for (int k = 0; k < 72; k++) {
            float a = (float)k * 6.2831853f / 72.0f, dx = sinf(a), dz = cosf(a);
            float t = rc(ex, ey, ez, dx, 0, dz, 15000, 4), t0 = rc(ex, ey, ez, dx, 0, dz, 15000, 0);
            if (t >= 0) {
                hits++;
                if (!(t > 0 && t <= 15000)) bad++;
                if (t < tmin) tmin = t;
                float adv = 0.5f * t, t2 = rc(ex + dx * adv, ey, ez + dz * adv, dx, 0, dz, 15000, 4);   /* mover el origen hacia la pared */
                if (!(t2 > 0 && fabsf(t2 - (t - adv)) < 2.0f)) nonmono++;
            }
            if (t >= 0 && (t0 < 0 || t < t0 - 0.01f)) subset++;                                           /* ignorar paredes nunca acerca el impacto ni lo crea de la nada */
        }
        printf("  %s eye=(%.0f,%.0f,%.0f): %d/72 rayos chocan (mínimo %.0f uds)\n", e->d_name, ex, ey, ez, hits, hits ? tmin : 0.0f);
        CHECK(bad == 0, "%s: distancias en (0, 15000]", e->d_name);
        CHECK(nonmono == 0, "%s: acercar el origen a la pared acorta exactamente lo avanzado (%d fallos)", e->d_name, nonmono);
        CHECK(subset == 0, "%s: la máscara solo puede alejar el impacto (%d fallos)", e->d_name, subset);
        if (hits > 0 && hits < 72) closed++;
    }
    closedir(d);
    if (dumps) CHECK(closed > 0, "algún volcado tiene recinto cerrado a 15 m (rayos que chocan) y a la vez huecos (%d volcados)", dumps);
}

int main(void) {
    test_synth();
    const char *dd = getenv("VRHZD_DUMPS");
    test_ray_synth(); test_ray_door();
    test_wall_dist();
    test_wall_near();
    if (dd && *dd) { test_dumps(dd); test_ray_dumps(dd); }
    printf(fails ? "test_hzd: %d fallos de %d\n" : "test_hzd: %d comprobaciones OK\n", fails ? fails : checks, checks);
    return fails != 0;
}
