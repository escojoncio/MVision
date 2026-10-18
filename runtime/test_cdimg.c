/* Imagen BIN (MODE2/2352 y MODE1/2352) frente a carpeta en cdrom.c: se sintetiza una carpeta de disco, se vuelca a una imagen
 * sector a sector con cd_read_sector y se comprueba que la imagen montada da exactamente lo mismo (datos, formato 2340,
 * búsqueda de ficheros). Sin datos del juego. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "cdrom.h"
#include "cpu.h"

const FuncEntry rt_func_table[] = {{0, 0}};
const unsigned rt_func_count = 0;
const FuncName rt_func_names[] = {{0xFFFFFFFFu, ""}};
const unsigned rt_func_name_count = 0;
const uint32_t rt_entry = 0;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void wfile(const char *dir, const char *name, size_t n, unsigned seed) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    for (size_t i = 0; i < n; i++) fputc((int)((i * 31 + seed + (i >> 8)) & 255), f);
    fclose(f);
}

int main(void) {
    char root[] = "/tmp/mvcdimgXXXXXX";
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
    char d[1024], img[1024], img1[1024];
    snprintf(d, sizeof d, "%s/disc", root); mkdir(d, 0755);
    char sub[1100]; snprintf(sub, sizeof sub, "%s/DATA", d); mkdir(sub, 0755);
    wfile(d, "SYSTEM.CNF", 70, 1);
    wfile(d, "BOOT.EXE", 5000, 2);
    wfile(sub, "B.DAT", 3000, 3);
    wfile(sub, "C.STR", 4 * 2336, 4);
    snprintf(img, sizeof img, "%s/disc.bin", root);
    snprintf(img1, sizeof img1, "%s/disc1.bin", root);

    CHECK(cd_open(d) == 1);
    uint8_t pvd[2048];
    CHECK(cd_read_sector(16, pvd, 0) == 0 && !memcmp(pvd + 1, "CD001", 5));
    uint32_t total = 0;
    memcpy(&total, pvd + 80, 4);
    total -= 150;
    CHECK(total > 24);
    static const uint8_t sync[12] = {0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0};
    FILE *f = fopen(img, "wb"), *f1 = fopen(img1, "wb");
    for (uint32_t l = 0; l < total; l++) {
        uint8_t raw[2340], u[2048];
        cd_read_sector(l, raw, 1);
        fwrite(sync, 1, 12, f); fwrite(raw, 1, 2340, f);
        /* misma imagen en MODE1: cabecera de 4 bytes (modo 1), 2048 de datos, EDC/ECC a cero */
        cd_read_sector(l, u, 0);
        uint8_t m1[2352] = {0};
        memcpy(m1, sync, 12); memcpy(m1 + 12, raw, 3); m1[15] = 1; memcpy(m1 + 16, u, 2048);
        fwrite(m1, 1, 2352, f1);
    }
    fclose(f); fclose(f1);

    /* Cada variante en un proceso hijo (el módulo no desmonta discos y admite 2): carpeta (disco 0) + imagen (disco 1). */
    for (int v = 0; v < 2; v++) {
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            int bad_ = 0;
            CHECK(cd_open(v ? img1 : img) == 2);
            if (!freopen("/dev/null", "w", stderr)) {}   /* cd_select_disc registra cada cambio de disco */
            uint32_t bad = 0;
            for (uint32_t l = 0; l < total; l++) {
                uint8_t a[2340], b[2340], ua[2048], ub[2048];
                cd_select_disc(0); cd_read_sector(l, a, 1); cd_read_sector(l, ua, 0);
                cd_select_disc(1); cd_read_sector(l, b, 1); cd_read_sector(l, ub, 0);
                int diff = memcmp(ua, ub, 2048) != 0;
                if (!v) diff |= memcmp(a, b, 2340) != 0;                       /* MODE2: idéntico */
                else diff |= memcmp(a + 12, b + 12, 2048) != 0 || memcmp(a, b, 3) != 0;   /* MODE1: datos y tiempo; subcabecera vacía */
                if (v && (b[4] | b[5] | b[6] | b[7] | b[8] | b[9] | b[10] | b[11])) diff = 1;
                if (diff) { if (!bad) printf("variante %d: primer sector distinto: %u\n", v, l); bad++; }
            }
            CHECK(bad == 0);
            uint32_t la, sa, lb, sb;
            cd_select_disc(0); CHECK(cd_find("DATA/B.DAT", &la, &sa) == 0);
            cd_select_disc(1); CHECK(cd_find("DATA/B.DAT", &lb, &sb) == 0);
            CHECK(la == lb && sa == sb && sa == 3000);
            cd_select_disc(0); CHECK(cd_find("BOOT.EXE", &la, &sa) == 0);
            cd_select_disc(1); CHECK(cd_find("BOOT.EXE", &lb, &sb) == 0);
            CHECK(la == lb && sa == 5000);
            CHECK(cd_find("NOEXISTE.BIN", &lb, &sb) != 0);
            uint8_t z[2340];                       /* fuera de rango: error y ceros */
            memset(z, 0xAA, sizeof z);
            CHECK(cd_read_sector(total + 100, z, 1) != 0 && z[100] == 0);
            memset(z, 0xAA, 2048);
            CHECK(cd_read_sector(total + 100, z, 0) != 0 && z[100] == 0);
            (void)bad_;
            fflush(stdout);
            _exit(fails != 0);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    printf(fails ? "%d fallos\n" : "imagen de disco en cdrom.c: todas las pruebas OK\n", fails);
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    if (system(cmd)) {}
    return fails != 0;
}
