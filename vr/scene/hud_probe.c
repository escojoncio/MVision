/* hud_probe: herramienta de diagnóstico del panel HUD (no forma parte de la app). Corre el juego con un guion de mando, y en los
 * fotogramas pedidos ensambla la escena con vr_scene_build, dibuja el HUD con el emulador por CPU de los shaders Metal (hud_emu.c) y lo compara
 * con ref_raster (exacto a 1x). Imprime las primitivas (--ops [--y Y0,Y1]) y escribe hud_<f>.png = [referencia | emulación | diferencia].
 *   hud_probe <exe> [frames_max] --out DIR --at F1,F2,... [--ops] [--y Y0,Y1] [--pad G] [--pad-every N:B]  (disco: MV_DISC) */
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "bridge.h"
#include "cdrom.h"
#include "cpu.h"
#include "gpu.h"
#include "hle.h"
#include "ref_raster.h"
#include "sys.h"
#include "vr_scene.h"
#include "hud_emu.h"
#ifdef MV_NOGEN
#include "hlebind.h"
#endif

extern jmp_buf rt_fatal_jmp;
extern int rt_fatal_armed;

#define MAXT 128
static uint32_t max_frames = 8000, at[MAXT];
static uint32_t toggle_n;
static int regscan, barscan, bigthr = 2500, scan_all, dump_ops, oy0 = -99999, oy1 = 99999;
static int nat;
static int no_fb;
static const char *outdir = ".";
static VrSceneOpts opts;
static RefR *rv;
static BridgeState bs;

/* ---- guion de mando (igual que bridge_dump) ---- */
typedef struct { uint32_t start, dur, every; uint16_t mask; } PadEv;
static PadEv pad_ev[256];
static int npad_ev;
static const struct { const char *n; int bit; } pad_names[] = {
    {"select", 0}, {"l3", 1}, {"r3", 2}, {"start", 3}, {"up", 4}, {"right", 5}, {"down", 6}, {"left", 7},
    {"l2", 8}, {"r2", 9}, {"l1", 10}, {"r1", 11}, {"triangle", 12}, {"circle", 13}, {"cross", 14}, {"square", 15}};
static uint16_t pad_mask(const char *s, size_t len) {
    uint16_t m = 0;
    while (len) {
        size_t k = 0;
        while (k < len && s[k] != '+') k++;
        int ok = 0;
        for (size_t i = 0; i < sizeof pad_names / sizeof *pad_names; i++)
            if (strlen(pad_names[i].n) == k && !strncasecmp(s, pad_names[i].n, k)) { m |= (uint16_t)(1u << pad_names[i].bit); ok = 1; }
        if (!ok) return 0;
        s += k; len -= k;
        if (len) { s++; len--; }
    }
    return m;
}
static int pad_add_script(const char *g) {
    char *dup = strdup(g), *sv = NULL;
    for (char *tok = strtok_r(dup, ",", &sv); tok; tok = strtok_r(NULL, ",", &sv)) {
        char *e;
        uint32_t fr = (uint32_t)strtoul(tok, &e, 0);
        if (*e != ':' || npad_ev >= 256) { free(dup); return -1; }
        char *b = e + 1, *c2 = strchr(b, ':');
        uint32_t dur = 6;
        if (c2) dur = (uint32_t)strtoul(c2 + 1, 0, 0);
        uint16_t m = pad_mask(b, c2 ? (size_t)(c2 - b) : strlen(b));
        if (!m) { free(dup); return -1; }
        pad_ev[npad_ev++] = (PadEv){fr, dur, 0, m};
    }
    free(dup);
    return 0;
}
static int pad_add_every(const char *a) {
    char *e;
    uint32_t n = (uint32_t)strtoul(a, &e, 0);
    if (*e != ':' || !n || npad_ev >= 256) return -1;
    uint16_t m = pad_mask(e + 1, strlen(e + 1));
    if (!m) return -1;
    pad_ev[npad_ev++] = (PadEv){0, 6, n, m};
    return 0;
}
static void script_pad_frame(uint32_t f) {
    static uint16_t last = 0xFFFF;
    uint16_t b = 0;
    for (int i = 0; i < npad_ev; i++) {
        const PadEv *p = &pad_ev[i];
        if (p->every) { if (f >= p->every && f % p->every < p->dur) b |= p->mask; }
        else if (f >= p->start && f < p->start + p->dur) b |= p->mask;
    }
    if (b != last) { pad_set_state(0, b, 0x80, 0x80, 0x80, 0x80); last = b; }
}

/* ---- PNG mínimo ---- */
static uint32_t crc_tab[256];
static uint32_t crc32u(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_tab[1])
        for (uint32_t i = 0; i < 256; i++) { uint32_t x = i; for (int k = 0; k < 8; k++) x = x & 1 ? 0xEDB88320u ^ (x >> 1) : x >> 1; crc_tab[i] = x; }
    c = ~c;
    while (n--) c = crc_tab[(c ^ *p++) & 255] ^ (c >> 8);
    return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE *f, const char *t, const uint8_t *d, uint32_t n) {
    uint8_t h[8]; be32(h, n); memcpy(h + 4, t, 4);
    fwrite(h, 1, 8, f); if (n) fwrite(d, 1, n, f);
    uint32_t c = crc32u(0, (const uint8_t *)t, 4); c = crc32u(c, d, n);
    uint8_t e[4]; be32(e, c); fwrite(e, 1, 4, f);
}
static void write_png(const char *path, const uint8_t *rgb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ih[13]; be32(ih, w); be32(ih + 4, h); ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0;
    chunk(f, "IHDR", ih, 13);
    size_t rawn = (size_t)h * (1 + (size_t)w * 3);
    uint8_t *raw = malloc(rawn);
    for (int y = 0; y < h; y++) { raw[(size_t)y * (1 + w * 3)] = 0; memcpy(raw + (size_t)y * (1 + w * 3) + 1, rgb + (size_t)y * w * 3, (size_t)w * 3); }
    size_t nb = (rawn + 65534) / 65535, zn = 2 + rawn + nb * 5 + 4;
    uint8_t *z = malloc(zn), *q = z;
    *q++ = 0x78; *q++ = 1;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < rawn; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < rawn; off += 65535) {
        size_t n = rawn - off < 65535 ? rawn - off : 65535;
        *q++ = off + n >= rawn; *q++ = n; *q++ = n >> 8; *q++ = ~n; *q++ = ~n >> 8;
        memcpy(q, raw + off, n); q += n;
    }
    be32(q, b << 16 | a); q += 4;
    chunk(f, "IDAT", z, (uint32_t)(q - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f); free(raw); free(z);
}


/* ---- sonda del camino HUD: compara el panel (emulado en CPU, como lo dibujaría Metal) con el rasterizador de referencia ---- */
/* VRAM del renderer: SOLO eventos (fill/copia/subida), como la textura de RGRenderer; lo dibujado por primitivas no llega a ella */
static uint16_t evram[512 * 1024];
static int late_from = -1, late_started;   /* --late F: el renderer "arranca" en la primera lista con frame >= F (VRAM inicial = la completa del oráculo) */
static void track_events(const GpuDrawList *l) {
    if (late_from >= 0 && !late_started) return;
    if (l->vram_init) memcpy(evram, l->vram_init, sizeof evram);
    for (uint32_t i = 0; i < l->nops; i++) hud_emu_event(l, &l->ops[i], evram);
}
/* un objetivo lo consume la primera lista con frame >= objetivo (no hay lista en todos los frames) */
static int target_frame(uint32_t f) { if (scan_all) return 1; for (int i = 0; i < nat; i++) if (at[i] && f >= at[i]) { at[i] = 0; return 1; } return 0; }
static const char *opname(int t) { static const char *n[] = {"POLY", "RECT", "LINE", "FILL", "VCOPY", "UPLOAD", "DOWNLOAD"}; return t < 7 ? n[t] : "?"; }

static void handle_list(uint32_t f, const GpuDrawList *l) {
    uint16_t *vb = evram;
    if (l->vram_init) memcpy(vb, l->vram_init, sizeof evram);   /* instantánea completa de la lista (primera tras encender la captura) */
    VrScene s;
    if (no_fb) for (uint32_t i = 0; i < l->nops; i++) ((GpuOp *)&l->ops[i])->fbk = 0;   /* --nofb: ignora la realimentación resuelta (comportamiento anterior) */
    int rc = vr_scene_build(l, &bs, &opts, &s);
    if (scan_all && s.stats.n_tris3d) { ref_apply(rv, l, 0); track_events(l); return; }   /* escena 3D: el HUD no la contiene */
    if (!scan_all) fprintf(stderr, "[probe] frame %u lista %u rc=%d ops=%u poly=%u(3D %u, 2D %u) rect=%u tris3D=%u tris2D=%u batches=%u disp(%d,%d,%d,%d)\n", f, l->index, rc, s.stats.n_ops,
            s.stats.n_poly, s.stats.n_poly3d, s.stats.n_poly2d, s.stats.n_rect, s.stats.n_tris3d, s.stats.n_tris2d, s.stats.n_batches, l->disp.x, l->disp.y, l->disp.w, l->disp.h);
    if (dump_ops) {
        for (uint32_t i = 0; i < l->nops; i++) {
            const GpuOp *o = &l->ops[i];
            if (o->type == GPU_OP_POLY || o->type == GPU_OP_RECT || o->type == GPU_OP_LINE) {
                int y = o->v[0].y;
                if (y < oy0 || y > oy1) continue;
                fprintf(stderr, "  op %u %s nv=%d fl=0x%x abr=%d tp=%d tex(%d,%d) clut(%d,%d) win(%d,%d,%d,%d) clip(%d,%d,%d,%d) off(%d,%d)", i, opname(o->type), o->nv, o->flags, o->abr,
                        o->tp, o->tx, o->ty, o->clx, o->cly, o->twmx, o->twmy, o->twox, o->twoy, o->cx1, o->cy1, o->cx2, o->cy2, o->dox, o->doy);
                if (o->type == GPU_OP_RECT) fprintf(stderr, " rect %dx%d", o->rw, o->rh);
                for (int k = 0; k < (o->type == GPU_OP_RECT ? 1 : o->nv); k++)
                    fprintf(stderr, " [(%d,%d) c%02x%02x%02x uv(%d,%d) vv%d f%d]", o->v[k].x, o->v[k].y, o->v[k].r, o->v[k].g, o->v[k].b, o->v[k].u, o->v[k].v, o->v[k].vvalid, o->v[k].vflags);
                fprintf(stderr, "\n");
            } else fprintf(stderr, "  op %u %s (%d,%d) %dx%d\n", i, opname(o->type), o->rx, o->ry, o->rw, o->rh);
        }
    }
    if (dump_ops) {   /* VRAM del oráculo al INICIO de la lista: filas 0..7, x 0..39 (palabras) */
        const uint16_t *rv0 = ref_vram(rv);
        fprintf(stderr, "  VRAM(0..15,0) [CLUT 0,0]:");
        for (int x = 0; x < 16; x++) fprintf(stderr, " %04x", rv0[x]);
        fprintf(stderr, "\n");
        for (int y = 0; y < 8; y++) { fprintf(stderr, "  fila %d:", y); for (int x = 0; x < 20; x++) fprintf(stderr, " %04x", rv0[y * 1024 + x]); fprintf(stderr, "\n"); }
    }
    if (dump_ops && getenv("PROBE_TEXEL")) {   /* PROBE_TEXEL="tx,ty,clx,cly,u0,v0,w,h": dibuja en ASCII los texels 4bpp (indices) y la CLUT, VRAM del oraculo al inicio */
        int tx, ty, clx, cly, u0, v0, ww, hh;
        if (sscanf(getenv("PROBE_TEXEL"), "%d,%d,%d,%d,%d,%d,%d,%d", &tx, &ty, &clx, &cly, &u0, &v0, &ww, &hh) == 8) {
            const uint16_t *rv0 = ref_vram(rv);
            fprintf(stderr, "  CLUT(%d,%d):", clx, cly);
            for (int i = 0; i < 16; i++) fprintf(stderr, " %04x", rv0[cly * 1024 + clx + i]);
            fprintf(stderr, "\n");
            for (int y = 0; y < hh; y++) {
                fprintf(stderr, "  ");
                for (int x = 0; x < ww; x++) {
                    int u = u0 + x; uint16_t w = rv0[((ty + v0 + y) & 511) * 1024 + ((tx + (u >> 2)) & 1023)];
                    int idx = (w >> ((u & 3) * 4)) & 15;
                    fputc(idx ? "0123456789abcdef"[idx] : '.', stderr);
                }
                fputc('\n', stderr);
            }
        }
    }
    /* referencia: aplicar la lista y presentar */
    ref_apply(rv, l, 1);
    int w, h;
    const uint8_t *ref = ref_present(rv, &l->disp, &w, &h);
    HudImg im = hud_img_new(l->disp.w, l->disp.h);
    hud_emu_run(&s, vb, &im, l->disp.x, l->disp.y);
    uint8_t *emu = malloc((size_t)im.w * im.h * 3);
    hud_emu_to_rgb(&im, emu);
    uint64_t bad = 0, big = 0;
    uint8_t *side = malloc((size_t)w * h * 3 * 3);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *a = ref + ((size_t)y * w + x) * 3, *b = emu + ((size_t)y * im.w + x) * 3;
            int m = 0;
            for (int k = 0; k < 3; k++) { int d = abs((int)a[k] - (int)b[k]); if (d > m) m = d; }
            bad += m > 8; big += m > 80;
            uint8_t *o = side + (size_t)y * w * 9;
            memcpy(o + x * 3, a, 3); memcpy(o + w * 3 + x * 3, b, 3);
            o[w * 6 + x * 3] = m > 8 ? 255 : 0; o[w * 6 + x * 3 + 1] = o[w * 6 + x * 3 + 2] = 0;
        }
    if (regscan) {   /* error medio absoluto en la región del texto del menú (y 186..236, x 90..230) */
        long sum = 0; int n = 0;
        for (int y = 186; y < 236 && y < h; y++) for (int x = 90; x < 230 && x < w; x++, n++)
            for (int k = 0; k < 3; k++) sum += abs((int)ref[((size_t)y * w + x) * 3 + k] - (int)emu[((size_t)y * im.w + x) * 3 + k]);
        fprintf(stderr, "[reg] frame %u lista %u err %.2f\n", f, l->index, (double)sum / (n * 3));
    }
    if (getenv("PROBE_REGION")) {   /* PROBE_REGION="x0,y0,x1,y1": color medio de ref y emulación */
        int x0, y0, x1, y1;
        if (sscanf(getenv("PROBE_REGION"), "%d,%d,%d,%d", &x0, &y0, &x1, &y1) == 4) {
            long ra[3] = {0, 0, 0}, ea[3] = {0, 0, 0}; int n = 0;
            for (int y = y0; y < y1 && y < h; y++) for (int x = x0; x < x1 && x < w; x++, n++)
                for (int k = 0; k < 3; k++) { ra[k] += ref[((size_t)y * w + x) * 3 + k]; ea[k] += emu[((size_t)y * im.w + x) * 3 + k]; }
            if (n) fprintf(stderr, "[region] frame %u ref (%ld,%ld,%ld) emu (%ld,%ld,%ld)\n", f, ra[0] / n, ra[1] / n, ra[2] / n, ea[0] / n, ea[1] / n, ea[2] / n);
        }
    }
    if (barscan) {   /* franja de la barra: color medio de la referencia y de la emulación */
        long ra[3] = {0, 0, 0}, ea[3] = {0, 0, 0}; int n = 0;
        for (int y = 209; y < 215 && y < h; y++) for (int x = 90; x < 230 && x < w; x++, n++)
            for (int k = 0; k < 3; k++) { ra[k] += ref[((size_t)y * w + x) * 3 + k]; ea[k] += emu[((size_t)y * im.w + x) * 3 + k]; }
        if (n) fprintf(stderr, "[bar] frame %u ref (%ld,%ld,%ld) emu (%ld,%ld,%ld)\n", f, ra[0] / n, ra[1] / n, ra[2] / n, ea[0] / n, ea[1] / n, ea[2] / n);
    }
    if (!scan_all || big > (uint64_t)bigthr) fprintf(stderr, "[probe] frame %u: px distintos (ref vs emulacion Metal HUD): %llu de %d (|d|>80: %llu)\n", f, (unsigned long long)bad, w * h, (unsigned long long)big);
    char p[1024];
    snprintf(p, sizeof p, "%s/hud_%05u.png", outdir, f);
    if (!scan_all || big > (uint64_t)bigthr) write_png(p, side, w * 3, h);
    free(side); free(emu); hud_img_free(&im);
}

static void on_alarm(int s) { (void)s; rt_fatal(NULL, "sin VBlank en 60 s"); }
static int fb_scan;
/* --fb-scan: primitivas texturizadas cuya página de textura o CLUT cae en la zona de los búferes de pantalla (x<640, y<256): leen lo que se está dibujando */
static void fbscan_list(uint32_t f, const GpuDrawList *l) {
    for (uint32_t i = 0; i < l->nops; i++) {
        const GpuOp *o = &l->ops[i];
        if ((o->type != GPU_OP_POLY && o->type != GPU_OP_RECT) || !(o->flags & GPU_F_TEXTURED)) continue;
        int clut_in = o->tp < 2 && o->cly < 256 && o->clx < 640;
        int tex_in = o->ty < 256 && o->tx < 640;
        if (!clut_in && !tex_in) continue;
        fprintf(stderr, "[fb] frame %u lista %u op %u %s tp=%d tex(%d,%d)%s clut(%d,%d)%s uv(%d,%d)", f, l->index, i, o->type == GPU_OP_POLY ? "POLY" : "RECT", o->tp, o->tx, o->ty, tex_in ? "*" : "", o->clx, o->cly, clut_in ? "*" : "", o->v[0].u, o->v[0].v);
        if (o->type == GPU_OP_POLY) fprintf(stderr, " .. (%d,%d)", o->v[o->nv - 1].u, o->v[o->nv - 1].v);
        fprintf(stderr, " col %02x%02x%02x pos(%d,%d)\n", o->v[0].r, o->v[0].g, o->v[0].b, o->v[0].x, o->v[0].y);
    }
}
static void on_frame(Cpu *c, uint32_t f) {
    (void)c;
    alarm(60);
    script_pad_frame(f);
    if (toggle_n && f > 0) {   /* --toggle N: la captura se apaga y enciende como en el host (sin consumidor > 1,5 s) */
        uint32_t ph = f % (toggle_n * 2);
        if (ph == 0) { gpu_capture_hiprec(1); gpu_capture_enable(1); }
        else if (ph == toggle_n) { gpu_capture_enable(0); gpu_capture_hiprec(0); }
    }
    bridge_read(emu_ram, &bs);
    gpu_capture_vblank();
    const GpuDrawList *l;
    while ((l = gpu_capture_frame())) {
        if (fb_scan) fbscan_list(f, l);
        if (late_from >= 0 && !late_started && f >= (uint32_t)late_from) { late_started = 1; memcpy(evram, ref_vram(rv), sizeof evram); }
        if (target_frame(f)) handle_list(f, l);
        else { ref_apply(rv, l, 0); track_events(l); }
    }
    if (f >= max_frames) {
        fprintf(stderr, "[probe] realimentacion (gpu_fb_stats): candidatas %u resueltas %u transparentes %u no uniformes %u\n", gpu_fb_stats.cand, gpu_fb_stats.baked, gpu_fb_stats.cleared, gpu_fb_stats.nonuniform);
        exit(0);
    }
}
static uint32_t rd32f(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

int main(int argc, char **argv) {
    const char *exe = NULL, *disc = getenv("MV_DISC");
    opts = vr_scene_opts_default();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--disc") && i + 1 < argc) disc = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "--at") && i + 1 < argc) {
            char *dup = strdup(argv[++i]), *sv = NULL;
            for (char *t = strtok_r(dup, ",", &sv); t && nat < MAXT; t = strtok_r(NULL, ",", &sv)) at[nat++] = (uint32_t)strtoul(t, 0, 0);
            free(dup);
        } else if (!strcmp(argv[i], "--pad") && i + 1 < argc) { if (pad_add_script(argv[++i])) return 1; }
        else if (!strcmp(argv[i], "--pad-every") && i + 1 < argc) { if (pad_add_every(argv[++i])) return 1; }
        else if (!strcmp(argv[i], "--ops")) dump_ops = 1;
        else if (!strcmp(argv[i], "--toggle") && i + 1 < argc) toggle_n = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--regscan")) { regscan = 1; scan_all = 1; if (!nat) at[nat++] = 1; }
        else if (!strcmp(argv[i], "--barscan")) { barscan = 1; scan_all = 1; if (!nat) at[nat++] = 1; }
        else if (!strcmp(argv[i], "--nofb")) no_fb = 1;
        else if (!strcmp(argv[i], "--fb-scan")) { fb_scan = 1; if (!nat) at[nat++] = 1; }
        else if (!strcmp(argv[i], "--late") && i + 1 < argc) late_from = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--thr") && i + 1 < argc) bigthr = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scan")) { scan_all = 1; if (!nat) at[nat++] = 1; }
        else if (!strcmp(argv[i], "--y") && i + 1 < argc) { char *e; oy0 = (int)strtol(argv[++i], &e, 0); oy1 = e[0] == ',' ? (int)strtol(e + 1, 0, 0) : oy0; }
        else if (!exe) exe = argv[i];
        else max_frames = (uint32_t)strtoul(argv[i], 0, 0);
    }
    if (!exe || !nat) { fprintf(stderr, "uso: %s <exe> [frames_max] --out DIR --at F1,F2 [--ops] [--y Y0,Y1] [--pad G] [--pad-every N:B]\n", argv[0]); return 1; }
    for (int i = 0; i < nat; i++) if (at[i] > max_frames) max_frames = at[i];
    mkdir(outdir, 0755);
    FILE *f = fopen(exe, "rb");
    if (!f) { perror(exe); return 1; }
    static uint8_t buf[EMU_RAM_SIZE + 0x800];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n < 0x800 || memcmp(buf, EXE_MAGIC, 8)) { fprintf(stderr, "ejecutable no válido\n"); return 1; }
    uint32_t pc = rd32f(buf + 0x10), gp = rd32f(buf + 0x14), t = rd32f(buf + 0x18), sz = rd32f(buf + 0x1C);
    memcpy(&emu_ram[t & 0x1FFFFF], buf + 0x800, sz);
#ifdef MV_NOGEN   /* runtime sin código generado (como la app): intérprete + HLE por firmas (MV_SIGS) */
    rt_entry = pc;
    if (!getenv("MV_SIGS")) { fprintf(stderr, "falta MV_SIGS\n"); return 1; }
    if (hle_bind_file(getenv("MV_SIGS"), t, sz, pc) < 0) return 1;
#endif
    if (disc && !cd_open(disc)) { fprintf(stderr, "sin discos\n"); return 1; }
    RefOpts ro = {1, 1, 0, 0};
    rv = ref_create(&ro);
    Cpu c = {0};
    c.r[28] = gp;
    c.r[29] = c.r[30] = 0x801FFFF0u;
    gte_reset();
    gpu_reset();
    gpu_capture_hiprec(1);
    gpu_capture_enable(1);
    rt_host_frame = on_frame;
    signal(SIGALRM, on_alarm);
    rt_fatal_armed = 1;
    if (setjmp(rt_fatal_jmp)) return 2;
    alarm(60);
    rt_call(&c, pc);
    return 0;
}
