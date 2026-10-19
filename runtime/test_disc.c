/* Pruebas de disc_img: imagen sintética MODE2/2352 + variantes de .cue + escaneo.
 * cd runtime && clang -Wall -Wextra -O1 -g -fsanitize=address,undefined -o ../build-disc/test_disc test_disc.c disc_img.c && ../build-disc/test_disc */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "disc_img.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void p32(uint8_t *p, uint32_t v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
    p[4] = v >> 24; p[5] = v >> 16; p[6] = v >> 8; p[7] = v;
}
static uint8_t bcd(int v) { return (v / 10) << 4 | v % 10; }

static int dirrec(uint8_t *p, const char *id, uint32_t lba, uint32_t size, int dir) {
    int il = id[0] < 2 ? 1 : (int)strlen(id), l = 33 + il; if (l & 1) l++;
    memset(p, 0, l); p[0] = l; p32(p + 2, lba); p32(p + 10, size); p[25] = dir ? 2 : 0; p[32] = il;
    memcpy(p + 33, id, il);
    return l;
}

#define NSEC 48
/* Imagen: 16 PVD, 17 term., 20 raíz, 21 SYSTEM.CNF, 22 dir datos, 23 EXE de datos, 24 fichero de datos. */
static void make_image(const char *path, const char *boot, const char *dexe, int nsec) {
    static uint8_t u[NSEC][2048];
    memset(u, 0, sizeof u);
    uint8_t *pvd = u[16];
    pvd[0] = 1; memcpy(pvd + 1, "CD001", 5); pvd[6] = 1;
    dirrec(pvd + 156, "\0", 20, 2048, 1);
    u[17][0] = 255; memcpy(u[17] + 1, "CD001", 5);
    char cnf[128];
    int cl = snprintf(cnf, sizeof cnf, "BOOT = cdrom:\\%s;1\r\nTCB = 4\r\nEVENT = 16\r\n", boot);
    memcpy(u[21], cnf, cl);
    int o = 0;
    o += dirrec(u[20] + o, "\0", 20, 2048, 1);
    o += dirrec(u[20] + o, "\1", 20, 2048, 1);
    o += dirrec(u[20] + o, "SYSTEM.CNF;1", 21, cl, 0);
    o += dirrec(u[20] + o, "DATA", 22, 2048, 1);
    o = 0;
    o += dirrec(u[22] + o, "\0", 22, 2048, 1);
    o += dirrec(u[22] + o, "\1", 20, 2048, 1);
    o += dirrec(u[22] + o, "ABC.DAT;1", 24, 2048, 0);
    char nm[32]; snprintf(nm, sizeof nm, "%s;1", dexe);
    o += dirrec(u[22] + o, nm, 23, 2048, 0);
    memcpy(u[23], EXE_MAGIC, 8);
    for (int i = 0; i < 2048; i++) u[24][i] = i * 7 + 1;

    FILE *f = fopen(path, "wb");
    for (int s = 0; s < nsec; s++) {
        uint8_t r[2352] = {0};
        r[0] = 0; memset(r + 1, 0xFF, 10);
        int a = s + 150;
        r[12] = bcd(a / 4500); r[13] = bcd(a / 75 % 60); r[14] = bcd(a % 75); r[15] = 2;
        r[16] = r[20] = 0; r[17] = r[21] = 0; r[18] = r[22] = 8; r[19] = r[23] = 0;
        memcpy(r + 24, s < NSEC ? u[s] : u[24], 2048);
        r[24 + 2048 + 5] = s;   /* huella en la zona EDC/ECC (a cero en el real) */
        fwrite(r, 1, sizeof r, f);
    }
    fclose(f);
}

static void wfile(const char *path, const char *txt, size_t n) { FILE *f = fopen(path, "wb"); fwrite(txt, 1, n, f); fclose(f); }
#define W(p, s) wfile(p, s, strlen(s))

static void check_image(const char *cue, const char *boot, const char *dexe, uint32_t nsec) {
    DiscImg *h = disc_img_open(cue);
    if (!h) { printf("FALLO: no abre %s\n", cue); fails++; return; }
    CHECK(disc_img_sectors(h) == nsec);
    DiscId id;
    CHECK(disc_img_identify(h, &id) == 0);
    CHECK(!strcmp(id.boot, boot));
    CHECK(!strcmp(id.datadir, "DATA"));
    CHECK(!strcmp(id.dataexe, dexe));
    uint8_t raw[2352], u[2048];
    CHECK(disc_img_read_raw(h, 24, raw) == 0);
    CHECK(raw[0] == 0 && raw[1] == 0xFF && raw[15] == 2 && raw[18] == 8 && raw[24 + 2048 + 5] == 24);
    CHECK(raw[12] == 0 && raw[13] == 2 && raw[14] == bcd(24)); /* MSF real 00:02:24 */
    CHECK(disc_img_read_user(h, 24, u) == 0 && u[3] == 22);
    CHECK(disc_img_read_raw(h, nsec, raw) == -1);
    uint32_t fl, fs;
    CHECK(disc_img_find(h, "data/abc.dat", &fl, &fs) == 0 && fl == 24 && fs == 2048);
    CHECK(disc_img_find(h, "SYSTEM.CNF", &fl, &fs) == 0 && fl == 21);
    CHECK(disc_img_find(h, "DATA/NOPE", &fl, &fs) == -1);
    disc_img_close(h);
}

int main(void) {
    char tmpl[] = "/tmp/test_disc_XXXXXX";
    if (!mkdtemp(tmpl)) return 1;
    char d[512], p[1024], q[1024];
    snprintf(d, sizeof d, "%s", tmpl);

    /* 1. bin suelto */
    snprintf(p, sizeof p, "%s/solo.bin", d);
    make_image(p, "BOOT_000.01", "MAIN.EXE", NSEC + 10);
    check_image(p, "BOOT_000.01", "MAIN.EXE", NSEC + 10);
    /* tamaño no múltiplo -> rechazo */
    snprintf(q, sizeof q, "%s/malo.bin", d);
    W(q, "hola");
    CHECK(disc_img_open(q) == NULL);
    remove(q);

    /* 2. .cue: variantes */
    snprintf(q, sizeof q, "%s/a.cue", d);
    W(q, "\xEF\xBB\xBF" "FILE \"solo.bin\" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n");
    check_image(q, "BOOT_000.01", "MAIN.EXE", NSEC + 10);
    snprintf(p, sizeof p, "%s/Mi Juego (Disc 1).BIN", d);
    make_image(p, "BOOT_000.01", "MAIN.EXE", NSEC);
    W(q, "REM x\nfile \"MI JUEGO (disc 1).bin\" binary\n\ttrack 01 mode2/2352\n\t\tindex 01 00:00:00\n");
    check_image(q, "BOOT_000.01", "MAIN.EXE", NSEC);                         /* mayúsculas distintas */
    W(q, "FILE Mi Juego (Disc 1).BIN BINARY\r\nTRACK 01 MODE2/2352\r\nINDEX 01 00:00:00\r\n");
    check_image(q, "BOOT_000.01", "MAIN.EXE", NSEC);                         /* sin comillas, con espacios */
    W(q, "FILE \"C:\\Juegos\\roms\\Mi Juego (Disc 1).BIN\" BINARY\r\nTRACK 01 MODE2/2352\r\nINDEX 01 00:00:00\r\n");
    check_image(q, "BOOT_000.01", "MAIN.EXE", NSEC);                         /* ruta Windows */
    W(q, "FILE \"no_existe.bin\" BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\n");
    CHECK(disc_img_open(q) == NULL);
    W(q, "FILE \"x.wav\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n");
    CHECK(disc_img_open(q) == NULL);

    /* 3. varias pistas / ficheros: datos + audio (con INDEX 00) en ficheros distintos */
    snprintf(p, sizeof p, "%s/t2.bin", d);
    { FILE *f = fopen(p, "wb"); for (int s = 0; s < 20; s++) { uint8_t a[2352]; memset(a, 0xA0 + s, sizeof a); fwrite(a, 1, sizeof a, f); } fclose(f); }
    W(q, "FILE \"Mi Juego (Disc 1).BIN\" BINARY\r\n TRACK 01 MODE2/2352\r\n  INDEX 01 00:00:00\r\n"
         "FILE \"t2.bin\" BINARY\r\n TRACK 02 AUDIO\r\n  INDEX 00 00:00:00\r\n  INDEX 01 00:02:00\r\n");
    DiscImg *h = disc_img_open(q);
    CHECK(h != NULL);
    if (h) {
        uint32_t st; int au;
        CHECK(disc_img_sectors(h) == NSEC + 20);
        CHECK(disc_img_tracks(h) == 2);
        CHECK(disc_img_track(h, 1, &st, &au) == 0 && st == NSEC + 150 && au == 1);
        uint8_t raw[2352];
        CHECK(disc_img_read_raw(h, NSEC + 3, raw) == 0 && raw[0] == 0xA3 && raw[2351] == 0xA3);
        CHECK(disc_img_read_raw(h, NSEC - 1, raw) == 0 && raw[15] == 2);
        CHECK(disc_img_nfiles(h) == 2);
        disc_img_close(h);
    }

    /* 4. caché: lecturas aleatorias idénticas a lectura directa */
    snprintf(p, sizeof p, "%s/big.bin", d);
    make_image(p, "BOOT_000.09", "BIG.EXE", NSEC + 200);
    h = disc_img_open(p);
    CHECK(h != NULL);
    if (h) {
        FILE *f = fopen(p, "rb");
        srand(1);
        for (int i = 0; i < 2000; i++) {
            uint32_t l = rand() % (NSEC + 200);
            uint8_t a[2352], b[2352];
            fseek(f, (long)l * 2352, SEEK_SET); if (fread(b, 1, 2352, f) != 2352) CHECK(0);
            CHECK(disc_img_read_raw(h, l, a) == 0 && !memcmp(a, b, 2352));
        }
        fclose(f);
        disc_img_close(h);
    }
    remove(p);

    /* 5. escaneo: dos discos (nombres de fichero engañosos), una carpeta duplicada de uno, un bin ajeno */
    char sd[600]; snprintf(sd, sizeof sd, "%s/scan", d); mkdir(sd, 0755);
    char b1[900], c1[900], b2[900], dd[900];
    snprintf(b1, sizeof b1, "%s/zzz primero.bin", sd);   /* disco con serial MAYOR, nombre que ordena primero */
    make_image(b1, "BOOT_000.12", "EXE_B.EXE", NSEC);
    snprintf(c1, sizeof c1, "%s/aaa.cue", sd);
    W(c1, "FILE \"zzz primero.bin\" BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\n");
    char sub[800]; snprintf(sub, sizeof sub, "%s/otro", sd); mkdir(sub, 0755);
    snprintf(b2, sizeof b2, "%s/suelto.bin", sub);
    make_image(b2, "BOOT_000.11", "EXE_A.EXE", NSEC);
    snprintf(dd, sizeof dd, "%s/extraido", sd); mkdir(dd, 0755);
    snprintf(p, sizeof p, "%s/SYSTEM.CNF", dd); W(p, "BOOT = cdrom:\\BOOT_000.10;1\r\n");
    snprintf(p, sizeof p, "%s/DATA", dd); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/DATA/Z.EXE", dd); W(p, EXE_MAGIC "\0\0\0\0");
    snprintf(p, sizeof p, "%s/DATA/A.DAT", dd); W(p, "nada");
    char dup[900]; snprintf(dup, sizeof dup, "%s/dup", sd); mkdir(dup, 0755);   /* misma identidad que el .bin suelto */
    snprintf(p, sizeof p, "%s/SYSTEM.CNF", dup); W(p, "BOOT = cdrom:\\BOOT_000.11;1\r\n");
    snprintf(p, sizeof p, "%s/DATA", dup); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/DATA/EXE_A.EXE", dup); W(p, EXE_MAGIC "\0\0\0\0");
    static DiscEntry ents[8];
    int n = disc_scan_dir(sd, ents, 8);
    for (int i = 0; i < n; i++) printf("  scan[%d] #%d dir=%d boot=%s dexe=%s %s\n", i, ents[i].id.number, ents[i].is_dir, ents[i].id.boot, ents[i].id.dataexe, ents[i].path);
    CHECK(n == 3);
    if (n == 3) {
        CHECK(!strcmp(ents[0].id.boot, "BOOT_000.10") && ents[0].is_dir && ents[0].id.number == 1);
        CHECK(!strcmp(ents[1].id.boot, "BOOT_000.11") && !ents[1].is_dir && ents[1].id.number == 2);   /* imagen gana a carpeta */
        CHECK(!strcmp(ents[2].id.boot, "BOOT_000.12") && ents[2].id.number == 3);
        CHECK(strstr(ents[2].path, "aaa.cue") != NULL);   /* .bin cubierto por su .cue: no duplica */
    }

    /* 6. imágenes reales del usuario si existen (../gamedata/img) y carpeta extraída ../gamedata/disc */
    if (access("../gamedata/img", R_OK) == 0) {
        static DiscEntry re[8];
        int m = disc_scan_dir("../gamedata/img", re, 8);
        printf("  gamedata/img: %d discos\n", m);
        for (int i = 0; i < m; i++) {
            printf("  #%d %s boot=%s datadir=%s dataexe=%s\n", re[i].id.number, re[i].path, re[i].id.boot, re[i].id.datadir, re[i].id.dataexe);
            DiscImg *r = disc_img_open(re[i].path);
            CHECK(r != NULL);
            if (r) { printf("     %u sectores, %d pistas\n", disc_img_sectors(r), disc_img_tracks(r)); disc_img_close(r); }
        }
    } else printf("  (sin ../gamedata/img: se omite)\n");
    if (access("../gamedata/disc", R_OK) == 0) {
        static DiscEntry re[8];
        int m = disc_scan_dir("../gamedata", re, 8);
        printf("  gamedata (modo carpeta): %d discos\n", m);
        for (int i = 0; i < m; i++)
            printf("  #%d dir=%d boot=%s datadir=%s dataexe=%s serial=%d\n", re[i].id.number, re[i].is_dir, re[i].id.boot, re[i].id.datadir, re[i].id.dataexe, re[i].id.serial);
        CHECK(m >= 1);
    }

    char cmd[600]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpl); if (system(cmd)) {}
    printf(fails ? "test_disc: %d FALLOS\n" : "test_disc: OK\n", fails);
    return fails != 0;
}
