/* l2d_dump: reproduce sin conexión la capa 2D de un volcado (list.bin + vram.bin) y la compara con frame.png, el fotograma REAL mostrado.
 * Uso (desde la raíz del repo):  make -C vr/scene l2d-dump ARGS="<carpeta del volcado> [--max-pct P]"
 * Mide, con vl2d a escala 1 (modo original, como la app):
 *   - pantalla completa (vl2d_run full=1): diferencia media por canal (0..255) y % de píxeles distintos frente a frame.png;
 *   - panel (full=0, lo que se pinta sobre el mundo): píxeles OPACOS de la capa frente a frame.png (los semitransparentes dependen del mundo de debajo).
 * Sale con 1 si la pantalla completa difiere más de P % (def. 0) o si algún píxel opaco del panel difiere.
 * Limitación: el volcado solo trae la VRAM AL FINAL de la lista; se usa como vram_start y como vram_end (la zona mostrada de partida es, pues, la final: mide la idempotencia del
 * rasterizado y las texturas, no los borrados ni las copias del propio fotograma: eso lo cubre test_layer2d con el oráculo gpu.c). frame.png: PNG RGBA sin compresión (el que escribe ns_dump). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "gpu.h"
#include "hle.h"
#include "vr_scene.h"
#include "vr_layer2d.h"
#include "ns_dump.h"

uint8_t emu_ram[EMU_RAM_SIZE];
uint32_t hle_frame_count(void) { return 0; }
void gte_hp_enable(int on) { (void)on; }
void gte_hp_frame(void) {}
int gte_hp_lookup(int sx, int sy, GpuHpInfo *o) { (void)sx; (void)sy; (void)o; return 0; }
int gte_hp_resolve(int n, const int *sx, const int *sy, GpuHpInfo *o, uint8_t *ok) { (void)n; (void)sx; (void)sy; (void)o; (void)ok; return 0; }
void hle_invoke(Cpu *c, uint32_t fn, uint32_t arg) { (void)c; (void)fn; (void)arg; }
int rt_orig(Cpu *c, const char *name) { (void)c; (void)name; return 0; }
uint8_t mem_r8(Cpu *c, uint32_t a) { (void)c; return emu_ram[a & 0x1FFFFF]; }
uint16_t mem_r16(Cpu *c, uint32_t a) { (void)c; uint16_t v; memcpy(&v, &emu_ram[a & 0x1FFFFE], 2); return v; }
uint32_t mem_r32(Cpu *c, uint32_t a) { (void)c; uint32_t v; memcpy(&v, &emu_ram[a & 0x1FFFFC], 4); return v; }
void mem_w32(Cpu *c, uint32_t a, uint32_t v) { (void)c; memcpy(&emu_ram[a & 0x1FFFFC], &v, 4); }

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* PNG RGBA8 con bloques "stored" (sin compresión) -> RGBA. NULL si no es de ese tipo. */
static uint8_t *png_read_stored(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    if (n < 33 || memcmp(b, "\x89PNG\r\n\x1a\n", 8)) { free(b); return NULL; }
    uint8_t *z = (uint8_t *)malloc((size_t)n); size_t zn = 0;
    int W = 0, H = 0;
    for (long o = 8; o + 12 <= n;) {
        uint32_t len = be32(b + o); const uint8_t *t = b + o + 4, *d = b + o + 8;
        if (o + 12 + len > (uint32_t)n) break;
        if (!memcmp(t, "IHDR", 4)) { W = (int)be32(d); H = (int)be32(d + 4); if (d[8] != 8 || d[9] != 6 || d[12] != 0) { free(b); free(z); return NULL; } }
        else if (!memcmp(t, "IDAT", 4)) { memcpy(z + zn, d, len); zn += len; }
        o += 12 + len;
    }
    free(b);
    size_t rowb = (size_t)W * 4 + 1, need = rowb * (size_t)H;
    uint8_t *raw = (uint8_t *)malloc(need + 16); size_t rn = 0;
    size_t i = 2;   /* cabecera zlib */
    while (i + 5 <= zn && rn < need) {
        int fin = z[i] & 1, bt = (z[i] >> 1) & 3;
        if (bt != 0) { free(z); free(raw); return NULL; }
        uint32_t L = z[i + 1] | (uint32_t)z[i + 2] << 8;
        i += 5;
        if (i + L > zn || rn + L > need + 16) { free(z); free(raw); return NULL; }
        memcpy(raw + rn, z + i, L); rn += L; i += L;
        if (fin) break;
    }
    free(z);
    if (rn < need) { free(raw); return NULL; }
    uint8_t *out = (uint8_t *)malloc((size_t)W * H * 4);
    for (int y = 0; y < H; y++) {   /* deshacer filtros */
        const uint8_t *s = raw + (size_t)y * rowb + 1; uint8_t *d = out + (size_t)y * W * 4; int ft = raw[(size_t)y * rowb];
        for (int x = 0; x < W * 4; x++) {
            int a = x >= 4 ? d[x - 4] : 0, up = y ? d[x - W * 4] : 0, c = (x >= 4 && y) ? d[x - W * 4 - 4] : 0, v = s[x];
            switch (ft) {
            case 1: v += a; break;
            case 2: v += up; break;
            case 3: v += (a + up) >> 1; break;
            case 4: { int p = a + up - c, pa = abs(p - a), pb = abs(p - up), pc = abs(p - c); v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? up : c); break; }
            default: break;
            }
            d[x] = (uint8_t)v;
        }
    }
    free(raw);
    *w = W; *h = H;
    return out;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: l2d_dump <carpeta del volcado> [--max-pct P]\n"); return 2; }
    double maxpct = 0;
    for (int i = 2; i + 1 < argc; i++) if (!strcmp(argv[i], "--max-pct")) maxpct = atof(argv[i + 1]);
    NsDump *d = ns_dump_open(argv[1]);
    if (!d || !d->has_list || !d->list.nops || !d->vram) { fprintf(stderr, "volcado sin list.bin/vram.bin\n"); return 2; }
    char path[600]; snprintf(path, sizeof path, "%s/frame.png", argv[1]);
    int rw = 0, rh = 0;
    uint8_t *ref = png_read_stored(path, &rw, &rh);
    if (!ref) { fprintf(stderr, "frame.png ilegible (se espera PNG RGBA sin compresión)\n"); return 2; }
    GpuDrawList l = d->list; l.vram_start = d->vram; l.vram_end = NULL;   /* sin vram_end: se mide el rasterizado, no el respaldo */
    VrSceneOpts o = vr_scene_opts_default(); o.screen_filter = 1;
    BridgeState bs; memset(&bs, 0, sizeof bs);
    VrScene sc;
    if (vr_scene_build(&l, &bs, &o, &sc) != 0) { fprintf(stderr, "vr_scene_build falló\n"); return 2; }
    printf("lista: %u ops, zona mostrada (%d,%d) %dx%d marco %d aw=%d pad_l=%d; frame.png %dx%d\n", l.nops, l.disp.x, l.disp.y, l.disp.w, l.disp.h, l.disp.fw, l.disp.aw, l.disp.pad_l, rw, rh);
    int rc = 0;
    VrLayer2D *L = vl2d_new();
    vl2d_set_options(L, 1, VL_SMOOTH_ORIGINAL);
    VrL2Info in;
    int w = 0, h = 0;
    if (vl2d_run(L, &l, &sc, 1)) {
        const uint8_t *rg = vl2d_rgba(L, &w, &h);
        VrL2Diff df;
        vl2d_info(L, &in);
        if (vl2d_compare(rg, w, h, 1, ref, rw, rh, &df)) {
            printf("completa  (escala 1): dif media %.4f / 255, píxeles distintos %.3f%% (%ld de %ld), max %d | primitivas %d, solo VRAM %d\n", df.mean, df.pct, df.bad, df.n, df.max, in.prims, in.mirror_prims);
            if (df.pct > maxpct) rc = 1;
        } else printf("completa: dimensiones de la capa %dx%d no encajan con frame.png %dx%d\n", w, h, rw, rh), rc = 1;
    }
    if (vl2d_run(L, &l, &sc, 0)) {
        const uint8_t *rg = vl2d_rgba(L, &w, &h);
        long op = 0, semi = 0, bad = 0, sum = 0;
        for (int y = 0; y < h && y < rh; y++) for (int x = 0; x < w && x < rw; x++) {
            const uint8_t *p = &rg[((size_t)y * w + x) * 4], *q = &ref[((size_t)y * rw + x) * 4];
            if (p[3] == 255) { int e = abs(p[0] - q[0]) + abs(p[1] - q[1]) + abs(p[2] - q[2]); op++; if (e) bad++; sum += e; }
            else if (p[3]) semi++;
        }
        vl2d_info(L, &in);
        printf("panel     (escala 1): %ld píxeles opacos (dif media %.4f / 255, distintos %ld = %.3f%%), %ld semitransparentes | primitivas al panel %d, solo VRAM %d\n", op, op ? sum / (3.0 * op) : 0.0, bad, op ? 100.0 * bad / op : 0.0, semi, in.prims, in.mirror_prims);
        if (bad) rc = 1;
    }
    printf(rc ? "RESULTADO: DIFERENCIAS\n" : "RESULTADO: OK\n");
    vl2d_free(L); free(ref); ns_dump_close(d);
    return rc;
}
