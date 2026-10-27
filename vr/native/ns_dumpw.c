/* ns_dumpw: escritor de volcados (ver ns_dump.h). Entra en la app. */
#include "ns_dump.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static int put_file(const char *dir, const char *name, const void *p, size_t n) {
    char path[640];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(p, 1, n, f);
    int rc = fclose(f);
    return (w == n && rc == 0) ? 0 : -1;
}

/* ------------------------------------------------------------------------------------------------ PNG sin compresión */
static uint32_t crc_tab[256]; static int crc_init;
static uint32_t crc32_upd(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_init) {
        for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1; crc_tab[i] = v; }
        crc_init = 1;
    }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 255] ^ (c >> 8);
    return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static int chunk(FILE *f, const char *type, const uint8_t *data, size_t n) {
    uint8_t h[8]; be32(h, (uint32_t)n); memcpy(h + 4, type, 4);
    uint32_t c = crc32_upd(0, h + 4, 4);
    c = crc32_upd(c, data, n);
    uint8_t t[4]; be32(t, c);
    return (fwrite(h, 1, 8, f) == 8 && (n == 0 || fwrite(data, 1, n, f) == n) && fwrite(t, 1, 4, f) == 4) ? 0 : -1;
}

int ns_png_write_rgba(const char *path, const uint8_t *rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0 || w > 8192 || h > 8192) return -1;
    size_t row = (size_t)w * 4 + 1, raw = row * (size_t)h;
    uint8_t *z = (uint8_t *)malloc(2 + raw + (raw / 65535 + 2) * 5 + 4);
    if (!z) return -1;
    size_t zp = 0;
    z[zp++] = 0x78; z[zp++] = 0x01;
    uint32_t a = 1, b = 0;
    size_t off = 0;
    while (off < raw || raw == 0) {   /* bloques stored de <= 65535 B; el flujo se recorre como filas con filtro 0 */
        size_t n = raw - off; if (n > 65535) n = 65535;
        z[zp++] = (off + n >= raw) ? 1 : 0;
        z[zp++] = (uint8_t)(n & 255); z[zp++] = (uint8_t)(n >> 8);
        z[zp++] = (uint8_t)(~n & 255); z[zp++] = (uint8_t)((~n >> 8) & 255);
        for (size_t i = 0; i < n; i++, off++) {
            size_t y = off / row, x = off % row;
            uint8_t v = x == 0 ? 0 : rgba[y * (size_t)w * 4 + (x - 1)];
            z[zp++] = v; a = (a + v) % 65521u; b = (b + a) % 65521u;
        }
        if (raw == 0) break;
    }
    be32(z + zp, (b << 16) | a); zp += 4;
    FILE *f = fopen(path, "wb");
    if (!f) { free(z); return -1; }
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    uint8_t ihdr[13]; be32(ihdr, (uint32_t)w); be32(ihdr + 4, (uint32_t)h); ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    int rc = fwrite(sig, 1, 8, f) == 8 ? 0 : -1;
    if (!rc) rc = chunk(f, "IHDR", ihdr, 13);
    if (!rc) rc = chunk(f, "IDAT", z, zp);
    if (!rc) rc = chunk(f, "IEND", NULL, 0);
    if (fclose(f) != 0) rc = -1;
    free(z);
    return rc;
}

/* ------------------------------------------------------------------------------------------------ volcado */
static int write_list(const char *dir, const GpuDrawList *l) {
    char path[640];
    snprintf(path, sizeof path, "%s/list.bin", dir);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t h[7] = { 0x324C534Eu /* "NSL2" */, (uint32_t)sizeof(GpuOp), l->nops, l->ndata, l->index, l->host_frame, (uint32_t)l->reason };
    uint32_t szd = (uint32_t)sizeof(GpuDisplay);
    int ok = fwrite(h, sizeof h, 1, f) == 1 && fwrite(&szd, 4, 1, f) == 1 && fwrite(&l->disp, sizeof l->disp, 1, f) == 1;
    if (ok && l->nops) ok = fwrite(l->ops, sizeof(GpuOp), l->nops, f) == l->nops;
    if (ok && l->ndata) ok = fwrite(l->data, 2, l->ndata, f) == l->ndata;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

/* prov.bin: tabla de procedencia de la lista (ver ns_dump.h): char magic[4]="NSP1"; u32 sizeof(ProvEntry); u32 n; ProvEntry e[n] (índice = id; e[0] = sin procedencia). */
static int write_prov(const char *dir, const GpuDrawList *l) {
    char path[640];
    snprintf(path, sizeof path, "%s/prov.bin", dir);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t h[3] = { 0x3150534Eu /* "NSP1" */, (uint32_t)sizeof(ProvEntry), l->nprov };
    int ok = fwrite(h, sizeof h, 1, f) == 1 && fwrite(l->prov, sizeof(ProvEntry), l->nprov, f) == l->nprov;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

int ns_dump_write(const char *dir, const NsDumpSrc *s) {
    if (!dir || !s || !s->ram || strlen(dir) > 480) return -1;
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", dir);
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    int failed = 0;
    if (put_file(tmp, "ram.bin", s->ram, NS_DUMP_RAM_SIZE) != 0) return -1;   /* sin RAM no hay volcado: queda el .tmp incompleto, nunca el directorio final */
    if (s->scratch && put_file(tmp, "scratch.bin", s->scratch, 1024) != 0) failed++;
    if (s->vram && put_file(tmp, "vram.bin", s->vram, NS_DUMP_VRAM_BYTES) != 0) failed++;
    if (s->gte && put_file(tmp, "gte.bin", s->gte, 64 * 4) != 0) failed++;
    if (s->list && write_list(tmp, s->list) != 0) failed++;
    if (s->list && s->list->prov && s->list->nprov > 1 && s->list->nprov <= PROV_MAX && write_prov(tmp, s->list) != 0) failed++;
    if (s->rgba && s->fw > 0 && s->fh > 0) {
        char p[640]; snprintf(p, sizeof p, "%s/frame.png", tmp);
        if (ns_png_write_rgba(p, s->rgba, s->fw, s->fh) != 0) failed++;
    }
    size_t ml = s->meta ? strlen(s->meta) : 0;
    char *m = (char *)malloc(ml + 64);
    if (!m) failed++;
    else {
        int n = snprintf(m, ml + 64, "format=NSD1\nparts_failed=%d\n%s", failed, s->meta ? s->meta : "");
        if (put_file(tmp, "meta.txt", m, (size_t)n) != 0) failed++;
        free(m);
    }
    if (rename(tmp, dir) != 0) return -1;   /* publicación atómica: el directorio final aparece completo o no aparece */
    return failed;
}
