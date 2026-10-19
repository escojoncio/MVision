/* Pruebas de vr_layer2d contra el oráculo (runtime/gpu.c): una lista 2D sintética (texto 4 bpp con CLUT con y sin bit 15, ventana de textura, degradados con
 * tramado, líneas, fundidos aditivo y sustractivo, mezclas B/2+F/2 y B+F/4, máscara) se dibuja con los comandos de la GPU original (oráculo, con captura) y se
 * reproduce con vl2d a escala 1: sobre fondo negro la capa debe ser IGUAL bit a bit a la VRAM del oráculo; sobre otros fondos, igual en lo opaco y con
 * tolerancia mínima en lo semitransparente. A escala 4: geometría exacta (un rect cubre exactamente su huella), suavizado de píxel art sin colores nuevos, etc.
 * Compilar: make -C vr/scene test-layer2d  (enlaza gpu.c con stubs mínimos, como runtime/test_gpu.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cpu.h"
#include "gpu.h"
#include "hle.h"
#include "vr_scene.h"
#include "vr_layer2d.h"

/* stubs del runtime para enlazar gpu.c aislado */
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

static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6; }
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FALLO: " __VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng = 987654321u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static uint32_t xy(int x, int y) { return (uint32_t)(x & 0x7FF) | (uint32_t)(y & 0x7FF) << 16; }

static void env(int x1, int y1, int x2, int y2, uint32_t e1, uint32_t e2, uint32_t e6) {
    gpu_gp0(0xE1000000 | e1);
    gpu_gp0(0xE2000000 | e2);
    gpu_gp0(0xE3000000 | (uint32_t)x1 | (uint32_t)y1 << 10);
    gpu_gp0(0xE4000000 | (uint32_t)x2 | (uint32_t)y2 << 10);
    gpu_gp0(0xE5000000);
    gpu_gp0(0xE6000000 | e6);
}
static void upload(int x, int y, int w, int h, const uint16_t *d) {
    gpu_gp0(0xA0000000); gpu_gp0(xy(x, y)); gpu_gp0(xy(w, h));
    int n = w * h;
    for (int i = 0; i < n; i += 2) gpu_gp0((uint32_t)d[i] | (i + 1 < n ? (uint32_t)d[i + 1] << 16 : 0));
}

/* Genera la escena: devuelve la lista capturada (copia propia). bg: color de fondo de la zona mostrada (RGB555) puesto ANTES de la captura. */
static GpuDrawList *g_list;
static uint16_t g_atlas[64 * 64];   /* 4 bpp: 64 halfwords x 64 filas = 256x64 texels */
static uint16_t g_clut[16];

static void gen_atlas(void) {
    for (int i = 0; i < 64 * 64; i++) {   /* texto: trazos con huecos */
        uint16_t w = 0;
        for (int k = 0; k < 4; k++) { int idx = (rnd() % 3 == 0) ? (int)(1 + rnd() % 15) : 0; w |= (uint16_t)(idx << (4 * k)); }
        g_atlas[i] = w;
    }
    for (int i = 0; i < 16; i++) {
        uint16_t c = (uint16_t)((i * 2) | ((31 - i * 2) & 31) << 5 | ((i * 7) & 31) << 10);
        if (i == 0) c = 0;                       /* transparente */
        else if (i % 3 == 0) c |= 0x8000;        /* semitransparente */
        g_clut[i] = c;
    }
}

static void build_list(uint16_t bg) {
    gpu_reset();
    gpu_capture_enable(0);
    gpu_gp1(0x08000001);   /* 320x240 */
    gpu_gp1(0x03000000);
    gpu_gp1(0x05000000 | 0 | 256u << 10);   /* mostrando y=256 (el dibujo va a y=0) */
    env(0, 0, 1023, 511, 0, 0, 0);
    /* fondo de la zona de dibujo (antes de la captura: queda en vram_init, no es un evento) */
    uint32_t bgc = (uint32_t)((bg & 31) << 3) | (uint32_t)(((bg >> 5) & 31) << 3) << 8 | (uint32_t)(((bg >> 10) & 31) << 3) << 16;
    gpu_gp0(0x02000000 | bgc); gpu_gp0(xy(0, 0)); gpu_gp0(xy(320, 240));
    upload(640, 0, 64, 64, g_atlas);       /* página x=640 (640/64=10), y=0, 4 bpp */
    upload(0, 500, 16, 1, g_clut);         /* CLUT en (0,500) */
    gpu_capture_enable(1);
    uint32_t clut = 0 | 500u << 6;
    env(0, 0, 319, 239, 10 | 0 << 5, 0, 0);
    /* 1. texto opaco: rects 8x8 y 16x16 con modulación y crudo */
    for (int i = 0; i < 12; i++) {
        int sz = i & 1 ? 2 : 3;
        gpu_gp0((uint32_t)(0x64 | sz << 3) << 24 | 0x808080); gpu_gp0(xy(8 + i * 20, 8)); gpu_gp0(clut << 16 | (uint32_t)((rnd() % 200) | (rnd() % 40) << 8));
    }
    /* 2. texto con 4 modos de mezcla (los texels con bit 15 mezclan, los demás son opacos) */
    for (int m = 0; m < 4; m++) {
        env(0, 0, 319, 239, 10 | (uint32_t)m << 5, 0, 0);
        for (int i = 0; i < 5; i++) {
            gpu_gp0((uint32_t)0x76 << 24 | (0x40u + (rnd() & 0x7F)) << 16 | (0x40u + (rnd() & 0x7F)) << 8 | (0x40u + (rnd() & 0x7F)));
            gpu_gp0(xy(10 + m * 70 + i * 4, 40 + i * 3)); gpu_gp0(clut << 16 | (uint32_t)((rnd() % 200) | (rnd() % 40) << 8));
        }
    }
    /* 3. ventana de textura (E2) + polígono FT4 modulado con tramado (E1 bit 9) */
    env(0, 0, 319, 239, 10 | 1u << 9, 3u | 3u << 5 | 1u << 10 | 2u << 15, 0);
    gpu_gp0(0x2C808080);
    gpu_gp0(xy(10, 90)); gpu_gp0(clut << 16 | 0);
    gpu_gp0(xy(60, 94)); gpu_gp0((10u) << 16 | 63);
    gpu_gp0(xy(14, 140)); gpu_gp0(63 << 8);
    gpu_gp0(xy(66, 134)); gpu_gp0(63 << 8 | 63);
    env(0, 0, 319, 239, 10 | 1u << 9, 0, 0);
    /* 4. degradados con tramado, semitransparentes y no */
    gpu_gp0(0x38FF2010); gpu_gp0(xy(100, 90)); gpu_gp0(0x0020FF30); gpu_gp0(xy(180, 96)); gpu_gp0(0x003040FF); gpu_gp0(xy(104, 150)); gpu_gp0(0x00FFFF40); gpu_gp0(xy(176, 156));
    env(0, 0, 319, 239, 10 | 1u << 9 | 0u << 5, 0, 0);
    gpu_gp0(0x3A806040); gpu_gp0(xy(120, 100)); gpu_gp0(0x00A0C020); gpu_gp0(xy(200, 100)); gpu_gp0(0x00403080); gpu_gp0(xy(150, 170)); gpu_gp0(0x00FF8020); gpu_gp0(xy(210, 160));
    /* 5. líneas planas y degradadas */
    gpu_gp0(0x40FFFFFF); gpu_gp0(xy(200, 20)); gpu_gp0(xy(300, 60));
    gpu_gp0(0x50FF0000); gpu_gp0(xy(210, 60)); gpu_gp0(0x0000FF00); gpu_gp0(xy(300, 30));
    /* 6. subida a VRAM a mitad de lista (cambia el atlas y la CLUT) y más texto */
    {
        uint16_t a2[64 * 64];
        for (int i = 0; i < 64 * 64; i++) a2[i] = (uint16_t)(~g_atlas[i]);
        upload(640, 0, 64, 64, a2);
        uint16_t c2[16];
        for (int i = 0; i < 16; i++) c2[i] = i == 0 ? 0 : (uint16_t)(g_clut[(i + 5) & 15] ^ 0x3C1F);
        upload(0, 500, 16, 1, c2);
    }
    env(0, 0, 319, 239, 10, 0, 0);
    for (int i = 0; i < 8; i++) { gpu_gp0(0x74808080); gpu_gp0(xy(220 + i * 12, 100 + (i & 3) * 10)); gpu_gp0(clut << 16 | (uint32_t)((rnd() % 200) | (rnd() % 40) << 8)); gpu_gp0(xy(10 + (i & 1) * 6, 12)); }
    /* 7. máscara: pone bit 15 en lo escrito y luego no se escribe encima */
    env(0, 0, 319, 239, 10, 0, 1);
    gpu_gp0(0x60FF8000); gpu_gp0(xy(20, 190)); gpu_gp0(xy(40, 30));
    env(0, 0, 319, 239, 10, 0, 2);
    gpu_gp0(0x6000FF80); gpu_gp0(xy(30, 195)); gpu_gp0(xy(40, 30));
    env(0, 0, 319, 239, 10, 0, 0);
    /* 8. fundidos a pantalla parcial: aditivo y sustractivo (gris), luego B/2 y B+F/4 */
    env(0, 0, 319, 239, 10 | 1u << 5, 0, 0);
    gpu_gp0(0x62303030); gpu_gp0(xy(100, 180)); gpu_gp0(xy(80, 40));
    env(0, 0, 319, 239, 10 | 2u << 5, 0, 0);
    gpu_gp0(0x62404040); gpu_gp0(xy(120, 190)); gpu_gp0(xy(80, 40));
    env(0, 0, 319, 239, 10 | 0u << 5, 0, 0);
    gpu_gp0(0x62A0A0A0); gpu_gp0(xy(200, 190)); gpu_gp0(xy(60, 40));
    env(0, 0, 319, 239, 10 | 3u << 5, 0, 0);
    gpu_gp0(0x62C0C0C0); gpu_gp0(xy(230, 200)); gpu_gp0(xy(60, 30));
    gpu_gp1(0x05000000);   /* flip: cierra la lista con la zona mostrada = (0,0) */
    const GpuDrawList *l = gpu_capture_frame();
    if (g_list) { free(g_list->ops); free(g_list->data); free(g_list->vram_init); free(g_list); }
    g_list = (GpuDrawList *)calloc(1, sizeof *g_list);
    if (!l) return;
    *g_list = *l;
    g_list->ops = (GpuOp *)malloc(l->nops * sizeof(GpuOp)); memcpy(g_list->ops, l->ops, l->nops * sizeof(GpuOp));
    g_list->data = (uint16_t *)malloc((l->ndata + 1) * 2); if (l->ndata) memcpy(g_list->data, l->data, (size_t)l->ndata * 2);
    g_list->vram_init = NULL;
    if (l->vram_init) { g_list->vram_init = (uint16_t *)malloc(sizeof gpu_vram); memcpy(g_list->vram_init, l->vram_init, sizeof gpu_vram); }
}

static int pix5(int x, int y, int k) { uint16_t v = gpu_vram[y][x]; return (v >> (5 * k)) & 31; }

static VrScene g_sc;
static VrSceneOpts g_o;
static void classify(void) {
    g_o = vr_scene_opts_default();
    g_o.drop_clear = 0;
    VrScene sc;
    int rc = vr_scene_build(g_list, NULL, &g_o, &sc);
    CHECK(rc == 0, "vr_scene_build rc=%d", rc);
    g_sc = sc;
}

int main(void) {
    gen_atlas();

    /* ---- fondo negro, escala 1: IGUAL bit a bit a la GPU original ---- */
    build_list(0x0000);
    CHECK(g_list && g_list->nops > 50 && g_list->vram_init, "lista capturada");
    classify();
    int n2d = 0;
    for (uint32_t i = 0; i < g_list->nops; i++) if (g_sc.op_dest[i] == 2) n2d++;
    CHECK(n2d > 40, "op_dest: primitivas al panel = %d", n2d);
    VrLayer2D *L = vl2d_new();
    vl2d_set_options(L, 1, VL_SMOOTH_ORIGINAL);
    CHECK(vl2d_run(L, g_list, &g_sc, 0) == 1, "vl2d_run");
    int w = 0, h = 0;
    const VrLayerPx *px = vl2d_px(L, &w, &h);
    CHECK(px && w == 320 && h == 240, "dimensiones %dx%d", w, h);
    int bad = 0, touched = 0;
    for (int y = 0; y < 240 && px; y++)
        for (int x = 0; x < 320; x++) {
            uint8_t bg[3] = {0, 0, 0}, o5[3];
            vl2d_compose5(&px[y * 320 + x], bg, o5);
            if (px[y * 320 + x].t != 256) touched++;
            for (int k = 0; k < 3; k++) if (o5[k] != pix5(x, y, k)) { if (bad < 8) printf("  dif (%d,%d) c%d capa=%d gpu=%d t=%d\n", x, y, k, o5[k], pix5(x, y, k), px[y * 320 + x].t); bad++; break; }
        }
    CHECK(bad == 0, "sobre negro: %d píxeles distintos de la GPU original (de %d tocados)", bad, touched);
    CHECK(touched > 4000, "la escena toca píxeles (%d)", touched);
    /* RGBA: lo opaco es la salida de vídeo exacta (5 -> 8 bits) */
    const uint8_t *rgba = vl2d_rgba(L, &w, &h);
    int badrgba = 0, opaque = 0;
    for (int y = 0; y < 240 && rgba; y++)
        for (int x = 0; x < 320; x++) {
            const VrLayerPx *p = &px[y * 320 + x];
            if (p->t == 0 && !p->d[0] && !p->d[1] && !p->d[2]) {
                opaque++;
                const uint8_t *q = &rgba[(y * 320 + x) * 4];
                int ok = q[3] == 255;
                for (int k = 0; k < 3; k++) { int v = pix5(x, y, k); if (q[k] != ((v << 3) | (v >> 2))) ok = 0; }
                if (!ok) badrgba++;
            }
        }
    CHECK(badrgba == 0 && opaque > 3000, "RGBA opaco exacto: %d malos de %d", badrgba, opaque);
    /* la VRAM espejo = VRAM del oráculo en lo que no son primitivas: el atlas cambiado a mitad de lista */
    const uint16_t *mv = vl2d_vram(L);
    int badv = 0;
    for (int y = 0; y < 64; y++) for (int x = 640; x < 704; x++) if (mv[y * 1024 + x] != gpu_vram[y][x]) badv++;
    CHECK(badv == 0, "VRAM espejo del atlas: %d diferencias", badv);

    /* ---- otros fondos: el oráculo se repite con el fondo puesto; la capa se compone sobre ese fondo ---- */
    uint16_t bgs[3] = {0x7FFF, 0x4210, 0x1C07};
    for (int bi = 0; bi < 3; bi++) {
        rng = 987654321u;
        gen_atlas();
        build_list(bgs[bi]);
        classify();
        vl2d_set_options(L, 1, VL_SMOOTH_ORIGINAL);
        vl2d_reset(L);
        vl2d_run(L, g_list, &g_sc, 0);
        px = vl2d_px(L, &w, &h);
        uint8_t bg[3] = {(uint8_t)(bgs[bi] & 31), (uint8_t)((bgs[bi] >> 5) & 31), (uint8_t)((bgs[bi] >> 10) & 31)};
        int exact = 0, near1 = 0, far = 0, tot = 0, maxd = 0;
        for (int y = 0; y < 240 && px; y++)
            for (int x = 0; x < 320; x++) {
                uint8_t o5[3];
                vl2d_compose5(&px[y * 320 + x], bg, o5);
                int d = 0;
                for (int k = 0; k < 3; k++) { int e = abs(o5[k] - pix5(x, y, k)); if (e > d) d = e; }
                tot++;
                if (d == 0) exact++; else if (d <= 2) near1++; else { far++; if (far < 6 || (far % 700) == 0) printf("  lejos (%d,%d) capa=%d,%d,%d gpu=%d,%d,%d t=%d c=%d d=%d\n", x, y, o5[0], o5[1], o5[2], pix5(x, y, 0), pix5(x, y, 1), pix5(x, y, 2), px[y * 320 + x].t, px[y * 320 + x].c[0], px[y * 320 + x].d[0]); }
                if (d > maxd) maxd = d;
            }
        printf("  fondo %04x: exactos %d, <=2 niveles %d, mas %d (max %d) de %d\n", bgs[bi], exact, near1, far, maxd, tot);
        CHECK(far * 30 < tot && maxd <= 8, "fondo %04x: demasiados píxeles lejos del original (%d de %d)", bgs[bi], far, tot);
    }

    /* ---- escala 4 ---- */
    rng = 987654321u; gen_atlas();
    build_list(0x0000);
    classify();
    /* rect plano opaco: cubre exactamente su huella (S x S muestras por píxel) */
    vl2d_set_options(L, 4, VL_SMOOTH_SHARP);
    vl2d_reset(L);
    double t0 = now_ms();
    CHECK(vl2d_run(L, g_list, &g_sc, 0) == 1, "vl2d_run x4");
    printf("  vl2d_run x4 (lista de prueba, nitido): %.1f ms\n", now_ms() - t0);
    t0 = now_ms();
    vl2d_run(L, g_list, &g_sc, 0);
    printf("  vl2d_run x4 repetido (zona sucia): %.1f ms\n", now_ms() - t0);
    px = vl2d_px(L, &w, &h);
    CHECK(px && w == 1280 && h == 960, "dimensiones x4 %dx%d", w, h);
    /* el rect de máscara (20,190)-(60,220) naranja 0xFF8000 luego el verde (30,195): compara con el original por bloques */
    int blkbad = 0, blkn = 0;
    if (px) {
        /* sin texto ni mezclas: zona de la máscara y las líneas; verifica color medio por bloque con el original en píxeles opacos planos */
        for (int y = 190; y < 195; y++) for (int x = 20; x < 60; x++) {
            blkn++;
            for (int sj = 0; sj < 4; sj++) for (int si = 0; si < 4; si++) {
                const VrLayerPx *p = &px[(y * 4 + sj) * 1280 + x * 4 + si];
                uint8_t bg[3] = {0, 0, 0}, o[3];
                vl2d_compose5(p, bg, o);
                int e = abs(o[0] - ((pix5(x, y, 0) << 3) | (pix5(x, y, 0) >> 2)));
                if (e > 0) blkbad++;
            }
        }
    }
    CHECK(blkbad == 0, "rect plano a x4: %d muestras distintas en %d píxeles", blkbad, blkn);
    /* el borde de un polígono en diagonal está más definido que a 1x: hay más de 2 niveles de alfa distintos... aquí basta que el área media coincida */
    long sum1 = 0, sum4 = 0;
    {
        VrLayer2D *L1 = vl2d_new();
        vl2d_set_options(L1, 1, VL_SMOOTH_ORIGINAL);
        vl2d_run(L1, g_list, &g_sc, 0);
        int w1, h1;
        const VrLayerPx *p1 = vl2d_px(L1, &w1, &h1);
        for (int i = 0; i < w1 * h1; i++) if (p1[i].t != 256) sum1++;
        for (int i = 0; i < w * h; i++) if (px[i].t != 256) sum4++;
        double ratio = (double)sum4 / 16.0 / (double)(sum1 ? sum1 : 1);
        CHECK(ratio > 0.9 && ratio < 1.1, "área cubierta x4/x1 = %.3f", ratio);
        vl2d_free(L1);
    }
    /* suavizado: nítido no inventa colores (solo texels reales); suave sí mezcla */
    {
        /* textura de escalera en diagonal: 8x8 texels, 4 bpp, dos índices */
        uint16_t a[64 * 64];
        memset(a, 0, sizeof a);
        for (int v = 0; v < 8; v++) for (int u = 0; u < 8; u++) {
            int idx = u >= v ? 1 : 2;
            int hw = v * 64 + (u >> 2), sh = (u & 3) * 4;
            a[hw] = (uint16_t)(a[hw] | (idx << sh));
        }
        uint16_t cl[16] = {0, 0x001F, 0x7C00};
        gpu_capture_enable(0);
        gpu_reset();
        gpu_gp1(0x08000001); gpu_gp1(0x03000000); gpu_gp1(0x05000000 | 256u << 10);
        env(0, 0, 1023, 511, 0, 0, 0);
        upload(640, 0, 64, 64, a); upload(0, 500, 16, 1, cl);
        gpu_capture_enable(1);
        env(0, 0, 319, 239, 10, 0, 0);
        gpu_gp0(0x65808080); gpu_gp0(xy(100, 100)); gpu_gp0((uint32_t)(0 | 500 << 6) << 16); gpu_gp0(xy(8, 8));   /* rect 8x8 crudo ampliado? no: 1:1 */
        gpu_gp0(0x2C808080);   /* quad texturizado 8x8 texels ampliado a 64x64 px */
        gpu_gp0(xy(150, 100)); gpu_gp0((uint32_t)(0 | 500 << 6) << 16 | 0);
        gpu_gp0(xy(214, 100)); gpu_gp0((uint32_t)(10) << 16 | 7);
        gpu_gp0(xy(150, 164)); gpu_gp0(7 << 8);
        gpu_gp0(xy(214, 164)); gpu_gp0(7 << 8 | 7);
        gpu_gp1(0x05000000);
        const GpuDrawList *l = gpu_capture_frame();
        CHECK(l != NULL, "lista de la escalera");
        if (l) {
            GpuDrawList cp = *l;
            cp.ops = (GpuOp *)malloc(l->nops * sizeof(GpuOp)); memcpy(cp.ops, l->ops, l->nops * sizeof(GpuOp));
            cp.data = (uint16_t *)malloc((l->ndata + 1) * 2); if (l->ndata) memcpy(cp.data, l->data, (size_t)l->ndata * 2);
            cp.vram_init = (uint16_t *)malloc(sizeof gpu_vram); memcpy(cp.vram_init, l->vram_init, sizeof gpu_vram);
            VrScene sc;
            VrSceneOpts o = vr_scene_opts_default();
            vr_scene_build(&cp, NULL, &o, &sc);
            int modes[3] = {VL_SMOOTH_ORIGINAL, VL_SMOOTH_SHARP, VL_SMOOTH_SOFT};
            int ncol[3];
            for (int m = 0; m < 3; m++) {
                vl2d_set_options(L, 4, modes[m]);
                vl2d_reset(L);
                vl2d_run(L, &cp, &sc, 0);
                const uint8_t *r = vl2d_rgba(L, &w, &h);
                int seen[8] = {0}, nc = 0;
                uint32_t cols[64]; int ncols = 0;
                (void)seen; (void)nc;
                for (int y = 100 * 4; y < 164 * 4 && r; y++)
                    for (int x = 150 * 4; x < 214 * 4; x++) {
                        const uint8_t *q = &r[(y * w + x) * 4];
                        uint32_t c = (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
                        int f = 0;
                        for (int i = 0; i < ncols; i++) if (cols[i] == c) { f = 1; break; }
                        if (!f && ncols < 64) cols[ncols++] = c;
                    }
                ncol[m] = ncols;
            }
            printf("  colores distintos en la escalera: original %d, nitido %d, suave %d\n", ncol[0], ncol[1], ncol[2]);
            CHECK(ncol[0] <= 3, "original: solo texels reales (%d colores)", ncol[0]);
            CHECK(ncol[1] <= 3, "nítido: solo texels reales (%d colores)", ncol[1]);
            CHECK(ncol[2] > 6, "suave: mezcla (%d colores)", ncol[2]);
            /* nítido corta las esquinas de la escalera: difiere del original en algunas muestras */
            vl2d_set_options(L, 4, VL_SMOOTH_ORIGINAL); vl2d_reset(L); vl2d_run(L, &cp, &sc, 0);
            const uint8_t *r0 = vl2d_rgba(L, &w, &h);
            uint8_t *copy0 = (uint8_t *)malloc((size_t)w * h * 4); memcpy(copy0, r0, (size_t)w * h * 4);
            vl2d_set_options(L, 4, VL_SMOOTH_SHARP); vl2d_reset(L); vl2d_run(L, &cp, &sc, 0);
            const uint8_t *r1 = vl2d_rgba(L, &w, &h);
            int diff = 0;
            for (int y = 100 * 4; y < 164 * 4; y++) for (int x = 150 * 4; x < 214 * 4; x++) if (memcmp(&copy0[(y * w + x) * 4], &r1[(y * w + x) * 4], 4)) diff++;
            CHECK(diff > 50, "nítido corrige las diagonales (%d muestras distintas del original)", diff);
            free(copy0);
            free(cp.ops); free(cp.data); free(cp.vram_init);
        }
    }

    /* ---- pantalla de cine: fotograma entero sobre la VRAM espejo ---- */
    {
        rng = 987654321u; gen_atlas();
        build_list(0x2108);   /* fondo (8,8,8): el fotograma completo parte de la VRAM mostrada */
        classify();
        VrLayer2D *LF = vl2d_new();
        vl2d_set_options(LF, 1, VL_SMOOTH_ORIGINAL);
        CHECK(vl2d_run(LF, g_list, &g_sc, 1) == 1, "vl2d_run full");
        const VrLayerPx *pf = vl2d_px(LF, &w, &h);
        int badf = 0;
        for (int y = 0; y < 240 && pf; y++) for (int x = 0; x < 320; x++) {
            uint8_t bg[3] = {0, 0, 0}, o5[3];
            vl2d_compose5(&pf[y * 320 + x], bg, o5);
            for (int k = 0; k < 3; k++) if (o5[k] != pix5(x, y, k)) { badf++; break; }
        }
        CHECK(badf == 0, "fotograma completo (cine) a 1x: %d píxeles distintos de la GPU original", badf);
        vl2d_set_options(LF, 4, VL_SMOOTH_SHARP);
        double tf = now_ms();
        CHECK(vl2d_run(LF, g_list, &g_sc, 1) == 1, "vl2d_run full x4");
        printf("  vl2d_run full x4: %.1f ms\n", now_ms() - tf);
        const uint8_t *rf = vl2d_rgba(LF, &w, &h);
        int opq = 0;
        for (int i = 0; rf && i < w * h; i++) if (rf[i * 4 + 3] == 255) opq++;
        CHECK(rf && w == 1280 && h == 960 && opq == w * h, "cine x4 opaco (%d de %d)", opq, w * h);
        vl2d_free(LF);
    }

    vl2d_free(L);
    if (fails) { printf("test_layer2d: %d FALLOS\n", fails); return 1; }
    printf("test_layer2d: OK\n");
    return 0;
}
