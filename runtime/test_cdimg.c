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
#include "disc_img.h"
#include "cpu.h"

const FuncEntry rt_func_table[] = {{0, 0}};
const unsigned rt_func_count = 0;
const FuncName rt_func_names[] = {{0xFFFFFFFFu, ""}};
const unsigned rt_func_name_count = 0;
const uint32_t rt_entry = 0;

static uint8_t *stage_ref;
static size_t stage_ref_n;
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void wfile(const char *dir, const char *name, size_t n, unsigned seed) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    for (size_t i = 0; i < n; i++) fputc((int)((i * 31 + seed + (i >> 8)) & 255), f);
    fclose(f);
}

/* Ritmo de entrega de sectores: x2 = 3 por frame PAL (150/s), x1 = 1,5; sin chorro tras una pausa larga. */
static void test_rate(void) {
    uint64_t cr = 0, tot = 0;
    const uint64_t frame = 33868800u / 50;
    for (int i = 0; i < 50; i++) { uint32_t n = cd_rate_take(&cr, frame, 1); CHECK(n == 3); tot += n; }
    CHECK(tot == 150);
    cr = 0; tot = 0;
    for (int i = 0; i < 100; i++) { uint32_t n = cd_rate_take(&cr, frame, 0); CHECK(n == 1 || n == 2); tot += n; }
    CHECK(tot == 150);                                   /* 75/s durante 2 s */
    cr = 0;
    CHECK(cd_rate_take(&cr, 33868800ull * 10, 1) == 16);  /* tope de ráfaga */
    CHECK(cd_rate_take(&cr, 0, 1) == 0);                  /* sin tiempo no hay sectores */
    cr = 0;
    uint64_t sub = 33868800u / 150 - 1;                   /* justo menos de un sector */
    CHECK(cd_rate_take(&cr, sub, 1) == 0);
    CHECK(cd_rate_take(&cr, 2, 1) == 1);
}

int main(void) {
    test_rate();
    char root[] = "/tmp/mvcdimgXXXXXX";
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
    char d[1024], img[1024], img1[1024];
    snprintf(d, sizeof d, "%s/disc", root); mkdir(d, 0755);
    char sub[1100]; snprintf(sub, sizeof sub, "%s/DATA", d); mkdir(sub, 0755);
    wfile(d, "SYSTEM.CNF", 70, 1);
    {   /* SYSTEM.CNF legible para la identificación por contenido */
        char p[1100]; snprintf(p, sizeof p, "%s/SYSTEM.CNF", d);
        FILE *sf = fopen(p, "wb");
        fputs("BOOT = cdrom:\\BOOT_000.42;1\r\nTCB = 4\r\nEVENT = 16\r\nSTACK = 801FFFF0\r\n", sf);
        fclose(sf);
    }
    wfile(d, "BOOT.EXE", 5000, 2);
    wfile(sub, "B.DAT", 3000, 3);
    wfile(sub, "C.STR", 4 * 2336, 4);
    {   /* archivo de stages sintético (tabla de entradas nombre[8] + sector, >= 1 MiB) entre ficheros que no lo son */
        char p[1100]; snprintf(p, sizeof p, "%s/STG.DAT", sub);
        size_t n = (1u << 20) + 5000;
        uint8_t *b = malloc(n);
        for (size_t i = 0; i < n; i++) b[i] = (uint8_t)((i * 7 + (i >> 9)) & 255);
        uint32_t tsz = 12 * 4;
        memcpy(b, &tsz, 4);
        static const char *nm[4] = {"init", "st01", "st02", "st03"};
        for (uint32_t i = 0; i < 4; i++) {
            uint32_t sec = 2 + i * 3;
            memset(b + 4 + 12 * i, 0, 12);
            memcpy(b + 4 + 12 * i, nm[i], strlen(nm[i]));
            memcpy(b + 4 + 12 * i + 8, &sec, 4);
        }
        FILE *sf = fopen(p, "wb"); fwrite(b, 1, n, sf); fclose(sf);
        stage_ref = b; stage_ref_n = n;
    }
    snprintf(img, sizeof img, "%s/disc.bin", root);
    snprintf(img1, sizeof img1, "%s/disc1.bin", root);

    CHECK(cd_open(d) == 1);
    {   /* origen del disco montado y carga del archivo de stages desde la carpeta */
        char src[1100]; int isd = -1;
        CHECK(cd_current_source(src, sizeof src, &isd) == 0 && isd == 1 && !strcmp(src, d));
        CHECK(cd_current_source(src, 4, &isd) != 0);          /* búfer corto: error, no trunca en silencio */
        size_t ln = 0;
        uint8_t *a = disc_load_stage_archive(d, 1, &ln);
        CHECK(a && ln == stage_ref_n && !memcmp(a, stage_ref, ln));
        free(a);
    }
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

    {   /* archivo de stages desde la imagen (MODE2 y MODE1): mismo contenido; sin tabla de stages (disco sin él) = NULL */
        for (int v = 0; v < 2; v++) {
            size_t ln = 0;
            uint8_t *a = disc_load_stage_archive(v ? img1 : img, 0, &ln);
            CHECK(a && ln == stage_ref_n && !memcmp(a, stage_ref, ln));
            free(a);
        }
        size_t ln = 7;
        CHECK(disc_load_stage_archive("/nonexistent/x.bin", 0, &ln) == NULL && ln == 0);
        CHECK(disc_load_stage_archive("/nonexistent", 1, &ln) == NULL);
    }

    /* Rutas y .cue como los de un volcado real: espacios, corchetes y paréntesis en carpeta y nombre, CRLF, BOM, comillas
     * o no, '\' en el nombre; escaneo de carpeta; errores de E/S (fichero truncado tras abrir) sin cuelgue ni caché envenenada. */
    {
        char dir[1200], bin[1400], cue[1400], cmd[3000];
        snprintf(dir, sizeof dir, "%s/Carpeta Juego/[BOOT-00042] (Region)", root);
        snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
        if (system(cmd)) {}
        snprintf(bin, sizeof bin, "%s/BOOT-00042 (Region) (Disc 1).bin", dir);
        snprintf(cue, sizeof cue, "%s/BOOT-00042 (Region) (Disc 1).cue", dir);
        snprintf(cmd, sizeof cmd, "cp '%s' '%s'", img, bin);
        CHECK(system(cmd) == 0);
        static const char *cues[] = {
            "FILE \"BOOT-00042 (Region) (Disc 1).bin\" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n",
            "\xEF\xBB\xBF" "FILE BOOT-00042 (Region) (Disc 1).bin BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n",
            "file \"x\\boot-00042 (region) (disc 1).BIN\" binary\n  track 01 mode2/2352\n    index 01 00:00:00\n",
        };
        uint8_t ref[2340], got[2340];
        for (unsigned v = 0; v < sizeof cues / sizeof *cues; v++) {
            FILE *cf = fopen(cue, "wb");
            fwrite(cues[v], 1, strlen(cues[v]), cf);
            fclose(cf);
            DiscImg *h = disc_img_open(cue);
            CHECK(h != NULL);
            if (!h) continue;
            CHECK(disc_img_sectors(h) == total && disc_img_tracks(h) == 1 && disc_img_nfiles(h) == 1);
            DiscId id;
            CHECK(disc_img_identify(h, &id) == 0 && id.serial == 42 && !strcmp(id.datadir, "DATA"));
            uint8_t a[2048], b[2048];
            for (uint32_t l = 0; l < total; l += 7) {
                CHECK(disc_img_read_user(h, l, a) == 0);
                cd_select_disc(0); cd_read_sector(l, b, 0);
                if (memcmp(a, b, 2048)) { CHECK(0 && "sector distinto"); break; }
            }
            CHECK(disc_img_io_errors(h) == 0);
            disc_img_close(h);
        }
        DiscEntry ents[4];
        snprintf(cmd, sizeof cmd, "%s/Carpeta Juego", root);
        CHECK(disc_scan_dir(cmd, ents, 4) == 1 && !ents[0].is_dir && strstr(ents[0].path, "[BOOT-00042] (Region)/") != NULL);
        /* montaje por cd_open con la ruta tal cual (disco 2 tras la carpeta) */
        CHECK(cd_open(cue) == 2);
        cd_select_disc(1);
        CHECK(cd_read_sector(16, ref, 1) == 0);
        cd_select_disc(0);
        CHECK(cd_read_sector(16, got, 1) == 0 && !memcmp(ref, got, 2340));

        /* E/S: se trunca el .bin con el disco abierto; los sectores perdidos fallan (sin colgarse), los demás siguen bien,
         * y al restaurar el tamaño el siguiente intento vuelve a leer (el bloque fallido no queda en caché). */
        FILE *cf = fopen(cue, "wb");
        fputs(cues[0], cf);
        fclose(cf);
        DiscImg *h = disc_img_open(cue);
        CHECK(h != NULL);
        if (h) {
            uint8_t raw[DISC_RAW], u[2048];
            CHECK(disc_img_read_raw(h, 3, raw) == 0);
            uint32_t cut = total / 2;
            CHECK(truncate(bin, (off_t)cut * DISC_RAW) == 0);
            CHECK(disc_img_read_raw(h, total - 5, raw) != 0 && raw[100] == 0);   /* bloque no leído en caché: error */
            CHECK(disc_img_io_errors(h) > 0);
            CHECK(disc_img_read_raw(h, 3, raw) == 0);                             /* lo anterior sigue accesible */
            CHECK(disc_img_read_user(h, total - 5, u) != 0);
            CHECK(truncate(bin, (off_t)total * DISC_RAW) == 0);
            CHECK(disc_img_read_raw(h, total - 5, raw) == 0);                     /* recuperado */
            disc_img_close(h);
        }
    }
    printf(fails ? "%d fallos\n" : "imagen de disco en cdrom.c: todas las pruebas OK\n", fails);
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    if (system(cmd)) {}
    return fails != 0;
}
