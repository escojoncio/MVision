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

int main(void) {
    test_synth();
    const char *dd = getenv("VRHZD_DUMPS");
    if (dd && *dd) test_dumps(dd);
    printf(fails ? "test_hzd: %d fallos de %d\n" : "test_hzd: %d comprobaciones OK\n", fails ? fails : checks, checks);
    return fails != 0;
}
