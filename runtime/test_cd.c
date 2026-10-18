/* Prueba del disco virtual + HLE de CD usando las rutinas de lectura del propio juego
 * (petición de lectura 0x80022ea8 + callback de dato listo 0x80022718), sin planificador.
 *   test_cd <exe> <dir_disco>[;<dir_disco2>] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "hle.h"
#include "cdrom.h"

void hle_CdReady(Cpu *c);
void hle_CdControl(Cpu *c);

#define FS_REQ   0x80022ea8u  /* (dst, sector_abs, bytes, cb) */
#define FS_LEFT  0x80022f08u  /* bytes pendientes */
#define FS_READY 0x80022718u  /* callback de dato listo del demonio */
#define BUF      0x80180000u
#define PARAM    0x80170000u

static int fails;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FALLO: %s (línea %d)\n", #x, __LINE__); fails++; } } while (0)

static void tobcd(uint32_t abs, uint8_t *o) {
    uint32_t m = abs / 4500, s = abs / 75 % 60, f = abs % 75;
    o[0] = (m / 10) << 4 | m % 10; o[1] = (s / 10) << 4 | s % 10; o[2] = (f / 10) << 4 | f % 10;
}

/* Lee con el código del juego: lo que haría su demonio (Setmode, Setloc+ReadN, callback). */
static int game_read(Cpu *c, uint32_t lba, uint32_t bytes) {
    c->r[4] = BUF; c->r[5] = lba + 150; c->r[6] = bytes; c->r[7] = 0;
    rt_call(c, FS_REQ);
    mem_w32(c, 0x800b5fe8u, 1); /* estado del demonio: leyendo */
    c->r[4] = FS_READY; hle_CdReady(c);
    uint8_t m[3]; tobcd(lba + 150, m);
    for (int i = 0; i < 3; i++) mem_w8(c, PARAM + i, m[i]);
    mem_w8(c, PARAM + 3, 0);
    mem_w8(c, PARAM + 8, 0xA0);
    c->r[4] = 0x0E; c->r[5] = PARAM + 8; c->r[6] = 0; hle_CdControl(c);
    c->r[4] = 0x06; c->r[5] = PARAM; c->r[6] = 0; hle_CdControl(c);
    for (int i = 0; i < 100000 && cd_busy(); i++) cd_pump(c); /* IRQ de CD: entrega los sectores */
    rt_call(c, FS_LEFT);
    return (int)c->r[2];
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: %s <exe> <dir_disco>[;dir2]\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    static uint8_t b[PSX_RAM_SIZE + 0x800];
    size_t n = fread(b, 1, sizeof b, f); fclose(f);
    uint32_t t, sz; memcpy(&t, b + 0x18, 4); memcpy(&sz, b + 0x1C, 4);
    if (n < 0x800 + sz) return 1;
    memcpy(&psx_ram[t & 0x1FFFFF], b + 0x800, sz);
    int nd = cd_open(argv[2]);
    CHECK(nd >= 1);

    Cpu cpu = {0}, *c = &cpu;
    c->r[29] = 0x801FFF00u; c->r[28] = 0;

    /* 1. descriptor primario */
    CHECK(game_read(c, 16, 2048) == 0);
    CHECK(!memcmp(mem_ptr(BUF + 1), "CD001", 5));
    /* 2. directorio de datos: el juego busca sus ficheros y el ejecutable que identifica el disco */
    CHECK(game_read(c, 23, 2048) == 0);
    int found_exe = 0;
    for (int o = 0; o < 2048;) {
        uint8_t *r = mem_ptr(BUF + o);
        if (!r[0]) break;
        if (r[32] > 4 && !memcmp(r + 33 + r[32] - 6, ".EXE;1", 6)) found_exe = 1;
        o += r[0];
    }
    CHECK(found_exe);
    /* 3. fichero grande: tabla de stages, comparación byte a byte con el original */
    /* ruta relativa del fichero grande a comparar: variable de entorno CD_TEST_FILE (p.ej. "DIR/FICHERO") */
    const char *rel = getenv("CD_TEST_FILE");
    if (!rel) { printf("test_cd: sin CD_TEST_FILE, se omite la parte 3-4\n"); return fails != 0; }
    uint32_t lba, size;
    CHECK(cd_find(rel, &lba, &size) == 0);
    CHECK(game_read(c, lba, 64 * 2048) == 0);
    char path[2048]; snprintf(path, sizeof path, "%.*s", (int)strcspn(argv[2], ";"), argv[2]);
    char p2[2100]; snprintf(p2, sizeof p2, "%s/%s", path, rel);
    FILE *g = fopen(p2, "rb");
    if (g) {
        static uint8_t ref[64 * 2048];
        CHECK(fread(ref, 1, sizeof ref, g) == sizeof ref);
        CHECK(!memcmp(mem_ptr(BUF), ref, sizeof ref));
        fclose(g);
    }
    /* 4. lectura no alineada a fichero (stage dentro del DIR: base + offset) */
    uint32_t sec; memcpy(&sec, (uint8_t *)mem_ptr(BUF) + 4 + 12 * 2 + 8, 4);
    CHECK(game_read(c, lba + sec, 2048) == 0);
    CHECK(*(uint8_t *)mem_ptr(BUF) == 1); /* versión de cabecera de stage */
    /* 5. sector XA del vídeo */
    uint32_t xl, xs; uint8_t raw[2340];
    /* ruta de un fichero XA: variable de entorno CD_TEST_XA (opcional) */
    const char *xa = getenv("CD_TEST_XA");
    if (xa) {
        CHECK(cd_find(xa, &xl, &xs) == 0);
        CHECK(cd_read_sector(xl, raw, 1) == 0);
    }
    /* 6. cambio de disco */
    if (nd > 1) {
        CHECK(cd_select_disc(1) == 0);
        CHECK(cd_current_disc() == 1);
        CHECK(game_read(c, 16, 2048) == 0);
        CHECK(cd_select_disc(0) == 0);
    }
    fprintf(stderr, fails ? "test_cd: %d fallos\n" : "test_cd: OK (%d discos)\n", fails ? fails : nd);
    return fails != 0;
}
