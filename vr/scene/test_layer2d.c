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
#include "vr_layer2d_async.h"
#include <unistd.h>
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


/* ---- texto de 4 bpp con la fuente y la CLUT subidas por un camino FUERA DE LA LISTA (sin eventos: DMA, escritura directa del juego, dibujo a VRAM) ---- */
static GpuDrawList *dup_list(const GpuDrawList *l) {
    GpuDrawList *d = (GpuDrawList *)calloc(1, sizeof *d);
    *d = *l;
    d->ops = (GpuOp *)malloc((l->nops + 1) * sizeof(GpuOp)); if (l->nops) memcpy(d->ops, l->ops, l->nops * sizeof(GpuOp));
    d->data = (uint16_t *)malloc((l->ndata + 1) * 2); if (l->ndata) memcpy(d->data, l->data, (size_t)l->ndata * 2);
    d->cap_ops = d->cap_data = 0;
    d->vram_init = l->vram_init ? (uint16_t *)memcpy(malloc(sizeof gpu_vram), l->vram_init, sizeof gpu_vram) : NULL;
    d->vram_start = l->vram_start ? (uint16_t *)memcpy(malloc(sizeof gpu_vram), l->vram_start, sizeof gpu_vram) : NULL;
    d->vram_end = l->vram_end ? (uint16_t *)memcpy(malloc(sizeof gpu_vram), l->vram_end, sizeof gpu_vram) : NULL;
    d->vram_after = NULL;
    return d;
}
static void free_list(GpuDrawList *d) { if (!d) return; free(d->ops); free(d->data); free(d->vram_init); free(d->vram_start); free(d->vram_end); free(d); }

static void offlist_text(void) {
    /* fuente 4 bpp: 16 glifos de 8x8 en (640,0), índices 0..3 (0 = transparente); CLUT en (0,500) con 4 colores (uno con bit 15) */
    uint16_t font[64 * 8];   /* 64 halfwords de ancho (256 texels) x 8 filas */
    memset(font, 0, sizeof font);
    uint32_t r = 24681357u;
    for (int gl = 0; gl < 16; gl++)
        for (int v = 0; v < 8; v++)
            for (int u = 0; u < 8; u++) {
                r ^= r << 13; r ^= r >> 17; r ^= r << 5;
                int idx = (r % 5 < 2) ? 0 : (int)(1 + (r >> 8) % 3);
                int tu = gl * 8 + u;
                font[v * 64 + (tu >> 2)] = (uint16_t)(font[v * 64 + (tu >> 2)] | (idx << ((tu & 3) * 4)));
            }
    const uint16_t clut[16] = {0, 0x7FFF, 0x03E0, 0x8000 | 0x7C1F, 0x0210};
    uint32_t clutw = 0 | 500u << 6;

    gpu_capture_enable(0);
    gpu_reset();
    gpu_gp1(0x08000001); gpu_gp1(0x03000000); gpu_gp1(0x05000000 | 256u << 10);
    env(0, 0, 1023, 511, 0, 0, 0);
    gpu_gp0(0x02000000); gpu_gp0(xy(0, 0)); gpu_gp0(xy(320, 240));   /* fondo negro */
    /* (basura previa en la zona de la fuente y la CLUT: lo bueno llegará sin pasar por eventos de la lista) */
    for (int y = 0; y < 8; y++) for (int x = 0; x < 64; x++) gpu_vram[y][640 + x] = (uint16_t)(0x1111 * ((x + y) & 3));
    for (int i = 0; i < 16; i++) gpu_vram[500][i] = 0x7C00;
    gpu_capture_vram_start(1);
    gpu_capture_enable(1);
    /* lista 1: la CLUT se DIBUJA a VRAM (sprite crudo de 15 bits: render a textura; la lista no lleva ningún evento de VRAM que la contenga) y la fuente la deja el
     * juego en VRAM por un camino que no pasa por la lista (escritura directa, como un DMA no capturado) */
    {
        uint16_t src[16];
        for (int i = 0; i < 16; i++) src[i] = (uint16_t)(clut[i] & 0x7FFF);
        upload(512, 100, 16, 1, src);   /* origen de la CLUT (evento de subida normal; el destino se dibuja) */
        env(0, 0, 1023, 511, 8u | 2u << 7, 0, 0);   /* página 15 bits en (512,0) */
        gpu_gp0(0x65000000); gpu_gp0(xy(0, 500)); gpu_gp0(100u << 8 | 0u); gpu_gp0(xy(16, 1));
        for (int y = 0; y < 8; y++) for (int x = 0; x < 64; x++) gpu_vram[y][640 + x] = font[y * 64 + x];
        env(0, 0, 319, 239, 10, 0, 0);
        gpu_gp0(0x60204060); gpu_gp0(xy(600, 300)); gpu_gp0(xy(10, 10));   /* fuera de la zona mostrada */
    }
    gpu_gp1(0x05000000);
    const GpuDrawList *l1 = gpu_capture_frame();
    GpuDrawList *c1 = l1 ? dup_list(l1) : NULL;
    CHECK(c1 && c1->vram_init && c1->vram_start, "lista 1 con vram_init y vram_start");
    CHECK(gpu_vram[500][1] == 0x7FFF && gpu_vram[500][2] == 0x03E0, "la CLUT dibujada quedó en la VRAM del oráculo");
    /* lista 2: texto con sprites 8x8, un cuadrilátero 8x8 con UV 0..7 en 8 píxeles (como los sprites-polígono) y un rect de 16x16 */
    env(0, 0, 319, 239, 10, 0, 0);
    for (int i = 0; i < 16; i++) { gpu_gp0(0x64u << 24 | 0x808080); gpu_gp0(xy(16 + i * 10, 20)); gpu_gp0(clutw << 16 | (uint32_t)(i * 8)); gpu_gp0(xy(8, 8)); }   /* 0x65: sprite libre; aquí 0x64 + tamaño en la palabra */
    for (int i = 0; i < 8; i++) {
        gpu_gp0(0x2C808080);
        gpu_gp0(xy(16 + i * 12, 50)); gpu_gp0(clutw << 16 | (uint32_t)(i * 8));
        gpu_gp0(xy(16 + i * 12 + 8, 50)); gpu_gp0(10u << 16 | (uint32_t)(i * 8 + 7));
        gpu_gp0(xy(16 + i * 12, 58)); gpu_gp0((uint32_t)7 << 8 | (uint32_t)(i * 8));
        gpu_gp0(xy(16 + i * 12 + 8, 58)); gpu_gp0((uint32_t)7 << 8 | (uint32_t)(i * 8 + 7));
    }
    for (int i = 0; i < 4; i++) { gpu_gp0(0x7Cu << 24 | 0x808080); gpu_gp0(xy(20 + i * 18, 80)); gpu_gp0(clutw << 16 | (uint32_t)(i * 8)); }   /* 16x16 */
    for (int i = 0; i < 8; i++) gpu_capture_vblank();   /* cierre de reserva (sin cambio de búfer mostrado) */
    const GpuDrawList *l2 = gpu_capture_frame();
    GpuDrawList *c2 = l2 ? dup_list(l2) : NULL;
    CHECK(c2 && !c2->vram_init && c2->vram_start, "lista 2 solo con vram_start (init=%p start=%p idx1=%u idx2=%u)", c2 ? (void *)c2->vram_init : 0, c2 ? (void *)c2->vram_start : 0, c1 ? c1->index : 99, c2 ? c2->index : 99);
    gpu_capture_vram_start(0);
    if (!c1 || !c2) { free_list(c1); free_list(c2); return; }

    VrSceneOpts o = vr_scene_opts_default();
    o.drop_clear = 0;
    VrScene s1, s2;
    CHECK(vr_scene_build(c1, NULL, &o, &s1) == 0, "escena 1");
    CHECK(vr_scene_build(c2, NULL, &o, &s2) == 0, "escena 2");
    int n2 = 0;
    for (uint32_t i = 0; i < c2->nops; i++) if (s2.op_dest[i] == 2) n2++;
    CHECK(n2 >= 28, "texto de la lista 2 al panel 2D (%d)", n2);

    /* 1. escala 1, modo original: igual bit a bit que la GPU original (fondo negro) */
    VrLayer2D *T = vl2d_new();
    vl2d_set_options(T, 1, VL_SMOOTH_ORIGINAL);
    vl2d_run(T, c1, &s1, 0);
    CHECK(vl2d_run(T, c2, &s2, 0) == 1, "texto fuera de lista: vl2d_run");
    int tw = 0, th = 0;
    const VrLayerPx *tp = vl2d_px(T, &tw, &th);
    int bad = 0, drawn = 0;
    for (int y = 0; y < 240 && tp; y++) for (int x = 0; x < 320; x++) {
        const VrLayerPx *p = &tp[y * 320 + x];
        uint8_t bg[3] = {0, 0, 0}, o5[3];
        vl2d_compose5(p, bg, o5);
        if (p->t != 256) drawn++;
        for (int k = 0; k < 3; k++) if (o5[k] != (p->t == 256 ? 0 : pix5(x, y, k))) { if (bad < 5) printf("  dif (%d,%d) capa=%d,%d,%d gpu=%d,%d,%d t=%d\n", x, y, o5[0], o5[1], o5[2], pix5(x, y, 0), pix5(x, y, 1), pix5(x, y, 2), p->t); bad++; break; }
    }
    CHECK(drawn > 800, "el texto toca píxeles (%d)", drawn);
    CHECK(bad == 0, "texto 4 bpp con fuente y CLUT fuera de la lista: %d píxeles distintos de la GPU original", bad);

    /* 2. control: sin vram_start (solo eventos) la fuente y la CLUT salen con basura: la prueba detecta el fallo de antes */
    {
        uint16_t *keep = c2->vram_start; c2->vram_start = NULL;
        VrLayer2D *E = vl2d_new();
        vl2d_set_options(E, 1, VL_SMOOTH_ORIGINAL);
        vl2d_run(E, c1, &s1, 0);
        vl2d_run(E, c2, &s2, 0);
        const VrLayerPx *ep = vl2d_px(E, &tw, &th);
        int ebad = 0;
        for (int y = 0; y < 240 && ep; y++) for (int x = 0; x < 320; x++) {
            const VrLayerPx *p = &ep[y * 320 + x];
            uint8_t bg[3] = {0, 0, 0}, o5[3];
            vl2d_compose5(p, bg, o5);
            for (int k = 0; k < 3; k++) if (o5[k] != (p->t == 256 ? 0 : pix5(x, y, k))) { ebad++; break; }
        }
        CHECK(ebad > 200, "control sin vram_start: debe diferir de la GPU original (%d)", ebad);
        c2->vram_start = keep;
        vl2d_free(E);
    }

    /* 3. escala 4, modo original: cada píxel nativo se replica 4x4 con el color exacto de la GPU original (texto nítido, sin deformar) */
    {
        VrLayer2D *H = vl2d_new();
        vl2d_set_options(H, 4, VL_SMOOTH_ORIGINAL);
        vl2d_run(H, c1, &s1, 0);
        CHECK(vl2d_run(H, c2, &s2, 0) == 1, "vl2d_run x4 original");
        int hw = 0, hh = 0;
        const uint8_t *hp = vl2d_rgba(H, &hw, &hh);
        CHECK(hp && hw == 1280 && hh == 960, "dimensiones %dx%d", hw, hh);
        int hbad = 0, hop = 0;
        for (int y = 0; y < 240 && hp; y++) for (int x = 0; x < 320; x++) {
            int cov = 0;
            for (int sj = 0; sj < 4; sj++) for (int si = 0; si < 4; si++) {
                const uint8_t *q = &hp[((y * 4 + sj) * hw + x * 4 + si) * 4];
                const uint8_t *q0 = &hp[((y * 4) * hw + x * 4) * 4];
                if (memcmp(q, q0, 4)) hbad++;   /* las 16 muestras del píxel son idénticas */
                if (q[3] == 255) cov++;
            }
            if (cov == 16) {
                hop++;
                const uint8_t *q = &hp[((y * 4) * hw + x * 4) * 4];
                for (int k = 0; k < 3; k++) { int v = pix5(x, y, k); if (q[k] != ((v << 3) | (v >> 2))) hbad++; }
            }
        }
        CHECK(hbad == 0 && hop > 800, "x4 original: %d muestras distintas del original replicado (%d píxeles opacos)", hbad, hop);
        /* repetir (zona sucia) da lo mismo */
        vl2d_run(H, c2, &s2, 0);
        hp = vl2d_rgba(H, &hw, &hh);
        int rbad = 0;
        for (int y = 0; y < 240 && hp; y++) for (int x = 0; x < 320; x++) {
            const uint8_t *q = &hp[((y * 4) * hw + x * 4) * 4];
            if (q[3] == 255) for (int k = 0; k < 3; k++) { int v = pix5(x, y, k); if (q[k] != ((v << 3) | (v >> 2))) rbad++; }
        }
        CHECK(rbad == 0, "x4 original repetido: %d diferencias", rbad);
        /* informativo: el mismo texto en modo nítido (alta resolución + EPX) frente al original replicado: píxeles nativos cuyo color medio difiere */
        {
            VrLayer2D *N = vl2d_new();
            vl2d_set_options(N, 4, VL_SMOOTH_SHARP);
            vl2d_run(N, c1, &s1, 0); vl2d_run(N, c2, &s2, 0);
            int nw = 0, nh = 0, ndiff = 0;
            const uint8_t *np = vl2d_rgba(N, &nw, &nh);
            for (int y = 0; y < 240 && np; y++) for (int x = 0; x < 320; x++) {
                int d = 0;
                for (int sj = 0; sj < 4; sj++) for (int si = 0; si < 4; si++) if (memcmp(&np[((y * 4 + sj) * nw + x * 4 + si) * 4], &hp[((y * 4 + sj) * hw + x * 4 + si) * 4], 4)) d = 1;
                ndiff += d;
            }
            printf("  texto fuera de lista: nitido x4 difiere del original en %d de %d px nativos del texto\n", ndiff, hop);
            vl2d_free(N);
        }
        /* 4. hilo trabajador: las dos listas encoladas de golpe (la primera puede saltarse: la segunda trae su VRAM) dan la misma capa */
        VrL2Async *A = vl2a_new();
        vl2a_options(A, 4, VL_SMOOTH_ORIGINAL);
        vl2a_submit(A, c1, s1.op_dest, 0, 1);
        vl2a_submit(A, c2, s2.op_dest, 0, 0);
        vl2a_flush(A);
        const uint8_t *ap = NULL; int aw = 0, ah = 0, af = 0; uint32_t aseq = 0;
        int ar = vl2a_acquire(A, &ap, &aw, &ah, &af, &aseq);
        CHECK(ar >= 0 && ap && aw == hw && ah == hh && memcmp(ap, hp, (size_t)hw * hh * 4) == 0, "capa asincrona (texto fuera de lista) identica a la sincrona");
        vl2a_release(A);
        vl2a_free(A);
        vl2d_free(H);
    }
    vl2d_free(T);
    free_list(c1); free_list(c2);
}


/* ---- texto en los bordes del área mostrada (320/368/384/512 de ancho, 240/256 de alto, rangos ampliados o reducidos) y relleno semitransparente/máscara ---- */
static GpuDrawList *run_list(const GpuDrawList *l, VrScene *sc) {
    GpuDrawList *c = dup_list(l);
    VrSceneOpts o = vr_scene_opts_default();
    o.drop_clear = 0;
    if (vr_scene_build(c, NULL, &o, sc) != 0) { free_list(c); return NULL; }
    return c;
}

static void edge_case(const char *name, uint32_t mode, int x1, int x2, int y1, int y2) {
    uint16_t font[64 * 8];
    memset(font, 0, sizeof font);
    for (int gl = 0; gl < 4; gl++) for (int v = 0; v < 8; v++) for (int u = 0; u < 8; u++) {
        int tu = gl * 8 + u, idx = 1 + (u + v + gl) % 3;
        font[v * 64 + (tu >> 2)] = (uint16_t)(font[v * 64 + (tu >> 2)] | (idx << ((tu & 3) * 4)));
    }
    const uint16_t clut[4] = {0, 0x7FFF, 0x03E0, 0x7C1F};
    gpu_capture_enable(0);
    gpu_reset();
    gpu_gp1(0x03000000);
    gpu_gp1(0x08000000 | mode);
    gpu_gp1(0x06000000 | (uint32_t)x1 | (uint32_t)x2 << 12);
    gpu_gp1(0x07000000 | (uint32_t)y1 | (uint32_t)y2 << 10);
    gpu_gp1(0x05000000 | 256u << 10);
    env(0, 0, 1023, 511, 0, 0, 0);
    gpu_gp0(0x02000000); gpu_gp0(xy(0, 0)); gpu_gp0(xy(512, 256));
    upload(640, 0, 64, 8, font);
    upload(0, 500, 4, 1, clut);
    gpu_capture_vram_start(1);
    gpu_capture_enable(1);
    GpuDisplay d; gpu_get_display(&d);
    uint32_t clutw = 0 | 500u << 6;
    env(0, 0, 1023, 511, 10, 0, 0);
    int xs[4] = {0, d.aw - 8, 0, d.aw - 8}, ys[4] = {0, 0, d.h - 8, d.h - 8};
    for (int i = 0; i < 4; i++) { gpu_gp0(0x64u << 24 | 0x808080); gpu_gp0(xy(xs[i], ys[i])); gpu_gp0(clutw << 16 | (uint32_t)(i * 8)); gpu_gp0(xy(8, 8)); }
    gpu_gp1(0x05000000);   /* flip: cierra la lista y muestra el búfer dibujado */
    const GpuDrawList *l = gpu_capture_frame();
    gpu_capture_vram_start(0);
    CHECK(l != NULL, "%s: lista", name);
    if (!l) return;
    VrScene sc;
    GpuDrawList *c = run_list(l, &sc);
    CHECK(c != NULL, "%s: escena", name);
    if (!c) return;
    GpuDisplay cd = c->disp;
    size_t fwh = (size_t)cd.fw * cd.h;
    uint8_t *ref = (uint8_t *)malloc(fwh * 4);
    CHECK(gpu_present_rgba(&cd, NULL, ref, fwh) == fwh, "%s: marco de la GPU %dx%d", name, cd.fw, cd.h);
    for (int sm = 1; sm <= 4; sm += 3) {   /* escala 1 y escala 4 (original replicado) */
        VrLayer2D *L = vl2d_new();
        vl2d_set_options(L, sm, VL_SMOOTH_ORIGINAL);
        CHECK(vl2d_run(L, c, &sc, 1) == 1, "%s: vl2d_run", name);
        int w = 0, h = 0;
        const uint8_t *rg = vl2d_rgba(L, &w, &h);
        CHECK(rg && w == cd.fw * sm && h == cd.h * sm, "%s x%d: capa %dx%d, marco %dx%d", name, sm, w, h, cd.fw, cd.h);
        int bad = 0;
        for (int y = 0; y < h && rg && w == cd.fw * sm; y++) for (int x = 0; x < w; x++)
            if (memcmp(&rg[((size_t)y * w + x) * 4], &ref[((size_t)(y / sm) * cd.fw + x / sm) * 4], 3)) { if (!bad) printf("  1a dif (%d,%d) capa=%d,%d,%d gpu=%d,%d,%d\n", x, y, rg[((size_t)y*w+x)*4], rg[((size_t)y*w+x)*4+1], rg[((size_t)y*w+x)*4+2], ref[((size_t)(y/sm)*cd.fw+x/sm)*4], ref[((size_t)(y/sm)*cd.fw+x/sm)*4+1], ref[((size_t)(y/sm)*cd.fw+x/sm)*4+2]); bad++; }
        CHECK(bad == 0, "%s x%d: %d muestras distintas del marco de la GPU (texto en los bordes cortado)", name, sm, bad);
        /* el texto de las 4 esquinas de la zona visible está completo (64 píxeles de glifo, todos no negros) */
        int full4 = 0;
        for (int i = 0; i < 4 && rg && w == cd.fw * sm; i++)
            for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
                int px = (cd.pad_l + xs[i] + x) * sm, py = (ys[i] + y) * sm;
                const uint8_t *q = &rg[((size_t)py * w + px) * 4];
                if (q[0] | q[1] | q[2]) full4++;
            }
        CHECK(full4 == 4 * 64, "%s x%d: glifos de las esquinas completos (%d de 256)", name, sm, full4);
        vl2d_free(L);
    }
    free(ref);
    free_list(c);
}


/* modo de 24 bits (vídeo) con rango no estándar: el fondo de la pantalla completa debe ser el marco de la GPU (8 bits por canal, márgenes negros) */
static void codec24_case(void) {
    gpu_capture_enable(0);
    gpu_reset();
    gpu_gp1(0x03000000);
    gpu_gp1(0x08000000 | 0x40 | 0x10);   /* 384, 24 bits */
    gpu_gp1(0x06000000 | (uint32_t)(0x230 + 12 * 7) | (uint32_t)(0x230 + 12 * 7 + 330 * 7) << 12);
    gpu_gp1(0x07000000 | 0x13u | (0x13u + 240) << 10);
    gpu_gp1(0x05000000 | 256u << 10);
    env(0, 0, 1023, 511, 0, 0, 0);
    uint32_t r = 77123u;
    for (int y = 0; y < 240; y++) for (int x = 0; x < 600; x++) { r ^= r << 13; r ^= r >> 17; r ^= r << 5; gpu_vram[y][x] = (uint16_t)r; }
    gpu_capture_vram_start(1);
    gpu_capture_enable(1);
    env(0, 0, 1023, 511, 10, 0, 0);
    gpu_gp0(0x60204060); gpu_gp0(xy(900, 300)); gpu_gp0(xy(4, 4));   /* fuera de la zona mostrada */
    gpu_gp1(0x05000000);
    const GpuDrawList *l = gpu_capture_frame();
    gpu_capture_vram_start(0);
    VrScene sc;
    GpuDrawList *c = l ? run_list(l, &sc) : NULL;
    CHECK(c != NULL, "24 bits: lista");
    if (!c) return;
    GpuDisplay cd = c->disp;
    CHECK(cd.depth24 && cd.pad_l == 12 && cd.aw == 330, "24 bits: pantalla pad_l=%d aw=%d", cd.pad_l, cd.aw);
    size_t fwh = (size_t)cd.fw * cd.h;
    uint8_t *ref = (uint8_t *)malloc(fwh * 4);
    CHECK(gpu_present_rgba(&cd, NULL, ref, fwh) == fwh, "24 bits: marco");
    for (int sm = 2; sm <= 2; sm++) {   /* escala >= 2 (R=S): a escala 1 el marco de 24 bits se reduce a 5 bits por canal (limitación conocida) */
    VrLayer2D *L = vl2d_new();
    vl2d_set_options(L, sm, VL_SMOOTH_ORIGINAL);
    vl2d_run(L, c, &sc, 1);
    int w = 0, h = 0, bad = 0;
    const uint8_t *rg = vl2d_rgba(L, &w, &h);
    CHECK(rg && w == cd.fw * sm && h == cd.h * sm, "24 bits: capa %dx%d, marco %dx%d", w, h, cd.fw, cd.h);
    for (int y = 0; y < h && rg && w == cd.fw * sm; y++) for (int x = 0; x < w; x++)
        if (memcmp(&rg[((size_t)y * w + x) * 4], &ref[((size_t)(y / sm) * cd.fw + x / sm) * 4], 3)) bad++;
    CHECK(bad == 0, "24 bits con rango no estándar: %d muestras distintas del marco de la GPU", bad);
    vl2d_free(L);
    }
    free(ref); free_list(c);
}


/* ---- composición fuera de pantalla (render a textura) + borrados/copias a la zona mostrada + doble búfer: la causa de los textos duplicados del códec y la intro ----
 * El juego compone texto en una zona de VRAM FUERA de las dos pantallas (otra área de dibujo y otro offset), la reutiliza varias veces en la misma lista, la copia (VRAM->VRAM) o la usa de textura
 * sobre el búfer que se muestra, y antes borra con un relleno lo que quedó de fotogramas anteriores. El resultado debe ser IGUAL, bit a bit, al marco de la GPU original (gpu_present_rgba). */
static void draw_env(int x1, int y1, int x2, int y2, int ox, int oy, uint32_t e1, uint32_t e6) {
    gpu_gp0(0xE1000000 | e1);
    gpu_gp0(0xE2000000);
    gpu_gp0(0xE3000000 | (uint32_t)x1 | (uint32_t)y1 << 10);
    gpu_gp0(0xE4000000 | (uint32_t)x2 | (uint32_t)y2 << 10);
    gpu_gp0(0xE5000000 | (uint32_t)(ox & 0x7FF) | (uint32_t)(oy & 0x7FF) << 11);
    gpu_gp0(0xE6000000 | e6);
}
static void comp_text(int seq, uint32_t bg, uint32_t clutw) {   /* compone un "renglón" distinto en la zona (512,256) de 128x32 con otra área de dibujo y offset */
    draw_env(512, 256, 639, 287, 512, 256, 10, 0);
    gpu_gp0(0x02000000 | bg); gpu_gp0(xy(512, 256)); gpu_gp0(xy(128, 32));
    for (int i = 0; i < 12; i++) {
        gpu_gp0(0x64u << 24 | 0x808080u); gpu_gp0(xy(2 + i * 10, 3 + (i & 1) * 12)); gpu_gp0(clutw << 16 | (uint32_t)(((i * 7 + seq * 5) & 3) * 8)); gpu_gp0(xy(8, 8));
    }
}
static GpuDrawList *offscreen_list(void) {
    uint16_t font[64 * 8];
    memset(font, 0, sizeof font);
    for (int gl = 0; gl < 4; gl++) for (int v = 0; v < 8; v++) for (int u = 0; u < 8; u++) {
        int tu = gl * 8 + u, idx = 1 + (u * 3 + v + gl * 5) % 3;
        font[v * 64 + (tu >> 2)] = (uint16_t)(font[v * 64 + (tu >> 2)] | (idx << ((tu & 3) * 4)));
    }
    const uint16_t clut[4] = {0, 0x7FFF, 0x03E0, 0x7C1F};
    gpu_capture_enable(0);
    gpu_reset();
    gpu_gp1(0x03000000);
    gpu_gp1(0x08000001);
    gpu_gp1(0x06000000 | 0x270u | 0xC70u << 12);
    gpu_gp1(0x07000000 | 0x13u | (0x13u + 256) << 10);
    gpu_gp1(0x05000000 | 256u << 10);   /* se muestra el búfer B (y=256); el juego dibuja en A (y=0) y al final lo muestra */
    env(0, 0, 1023, 511, 0, 0, 0);
    for (int y = 0; y < 256; y++) for (int x = 0; x < 320; x++) gpu_vram[y][x] = (uint16_t)(((x >> 3) & 31) | (((y >> 3) & 15) << 5) | 0x0400);   /* A: restos del fotograma de hace 2 */
    uint32_t r = 1234567u;
    for (int y = 16; y < 64; y++) for (int x = 96; x < 224; x++) { r ^= r << 13; r ^= r >> 17; r ^= r << 5; gpu_vram[y][x] = (uint16_t)(0x4210 | (r & 0x3DEF)); }   /* texto fantasma de un fotograma anterior */
    upload(640, 0, 64, 8, font);
    upload(0, 500, 4, 1, clut);
    gpu_capture_vram_start(1);
    gpu_capture_enable(1);
    uint32_t clutw = 0 | 500u << 6;
    /* 1. composición fuera de pantalla -> se COPIA (VRAM->VRAM) a la zona mostrada tras borrarla con un relleno */
    comp_text(1, 0x00600808u, clutw);
    draw_env(0, 0, 319, 255, 0, 0, 10, 0);
    gpu_gp0(0x02101010u); gpu_gp0(xy(96, 16)); gpu_gp0(xy(128, 48));
    gpu_gp0(0x80000000u); gpu_gp0(xy(512, 256)); gpu_gp0(xy(96, 20)); gpu_gp0(xy(128, 32));
    /* 2. la misma zona se reutiliza (otro renglón) y se usa de TEXTURA (15 bits, página en (512,256)) sobre la pantalla */
    comp_text(2, 0x00086008u, clutw);
    draw_env(0, 0, 319, 255, 0, 0, 8 | 1 << 4 | 2 << 7, 0);
    gpu_gp0(0x64u << 24 | 0x808080u); gpu_gp0(xy(100, 120)); gpu_gp0(0); gpu_gp0(xy(96, 24));
    /* 3. otra vez, con un tercer renglón */
    comp_text(3, 0x08080860u, clutw);
    draw_env(0, 0, 319, 255, 0, 0, 8 | 1 << 4 | 2 << 7, 0);
    gpu_gp0(0x64u << 24 | 0x808080u); gpu_gp0(xy(100, 160)); gpu_gp0(0); gpu_gp0(xy(96, 24));
    /* 4. texto normal en la pantalla y un sprite semitransparente encima de lo copiado */
    draw_env(0, 0, 319, 255, 0, 0, 10 | 1 << 5, 0);
    for (int i = 0; i < 6; i++) { gpu_gp0(0x66u << 24 | 0x808080u); gpu_gp0(xy(100 + i * 12, 30)); gpu_gp0(clutw << 16 | (uint32_t)((i & 3) * 8)); gpu_gp0(xy(16, 16)); }
    draw_env(0, 0, 319, 255, 0, 0, 10, 0);
    for (int i = 0; i < 20; i++) { gpu_gp0(0x64u << 24 | 0x808080u); gpu_gp0(xy(20 + i * 9, 220)); gpu_gp0(clutw << 16 | (uint32_t)((i & 3) * 8)); gpu_gp0(xy(8, 8)); }
    /* 5. efecto de pantalla (la intro): tiras que leen de textura el PROPIO búfer que se dibuja (página 15 bits en (0,0)) y lo reescriben desplazado: la textura es lo dibujado antes en esta lista */
    draw_env(0, 0, 319, 255, 0, 0, 2 << 7, 0);
    for (int i = 0; i < 12; i++) { gpu_gp0(0x64u << 24 | 0x808080u); gpu_gp0(xy(48 + (i & 1) * 3, 70 + i * 2)); gpu_gp0((uint32_t)((70 + i * 2) << 8 | 40)); gpu_gp0(xy(64, 2)); }
    gpu_gp1(0x05000000);   /* cambio de búfer: cierra la lista y muestra A */
    const GpuDrawList *l = gpu_capture_frame();
    gpu_capture_vram_start(0);
    return l ? dup_list(l) : NULL;
}

static void offscreen_compose(void) {
    GpuDrawList *c0 = offscreen_list();
    CHECK(c0 != NULL && c0->vram_start && c0->vram_end, "composicion fuera de pantalla: lista con vram_start/vram_end");
    if (!c0) return;
    VrScene sc;
    VrSceneOpts o = vr_scene_opts_default();
    o.drop_clear = 0;
    CHECK(vr_scene_build(c0, NULL, &o, &sc) == 0, "composicion fuera de pantalla: escena");
    GpuDisplay cd = c0->disp;
    size_t fwh = (size_t)cd.fw * cd.h;
    uint8_t *ref = (uint8_t *)malloc(fwh * 4);
    CHECK(gpu_present_rgba(&cd, NULL, ref, fwh) == fwh, "composicion fuera de pantalla: marco");
    /* los renglones 2 y 3 de la composición son distintos en la imagen real (si no, la prueba no distinguiría el estado final del instantáneo) */
    int distinct = 0;
    for (int y = 0; y < 24; y++) for (int x = 0; x < 96; x++)
        if (memcmp(&ref[((size_t)(120 + y) * cd.fw + 100 + x) * 4], &ref[((size_t)(160 + y) * cd.fw + 100 + x) * 4], 3)) distinct++;
    CHECK(distinct > 200, "composicion fuera de pantalla: los dos renglones texturizados difieren en la imagen real (%d)", distinct);
    /* A. fotograma completo, sin la VRAM real de cierre: la capa por sí sola (raster) debe ser idéntica a la GPU original, a escala 1 y replicada x4 */
    GpuDrawList cn = *c0; cn.vram_end = NULL;
    for (int sm = 1; sm <= 4; sm += 3) {
        VrLayer2D *L = vl2d_new();
        vl2d_set_options(L, sm, VL_SMOOTH_ORIGINAL);
        CHECK(vl2d_run(L, &cn, &sc, 1) == 1, "composicion x%d: vl2d_run", sm);
        int w = 0, h = 0;
        const uint8_t *rg = vl2d_rgba(L, &w, &h);
        VrL2Diff df;
        int okc = vl2d_compare(rg, w, h, sm, ref, cd.fw, cd.h, &df);
        CHECK(okc, "composicion x%d: dimensiones capa %dx%d marco %dx%d", sm, w, h, cd.fw, cd.h);
        if (okc) printf("  composicion fuera de pantalla, capa completa x%d vs GPU: dif media %.4f, píxeles distintos %.3f%% (%ld de %ld), max %d\n", sm, df.mean, df.pct, df.bad, df.n, df.max);
        CHECK(okc && df.bad == 0, "composicion x%d: %ld píxeles (%.3f%%, dif media %.3f) distintos del marco de la GPU", sm, df.bad, df.pct, df.mean);
        VrL2Info in; vl2d_info(L, &in);
        CHECK(in.fallback == 0, "composicion x%d: sin vram_end no hay respaldo", sm);
        vl2d_free(L);
    }
    /* A2. fotograma completo sin escena (vr_scene_build falló): la capa sigue siendo exacta */
    {
        VrLayer2D *L = vl2d_new();
        vl2d_set_options(L, 1, VL_SMOOTH_ORIGINAL);
        CHECK(vl2d_run(L, &cn, NULL, 1) == 1, "composicion sin escena: vl2d_run");
        int w = 0, h = 0;
        const uint8_t *rg = vl2d_rgba(L, &w, &h);
        VrL2Diff df;
        int okc = vl2d_compare(rg, w, h, 1, ref, cd.fw, cd.h, &df);
        CHECK(okc && df.bad == 0, "composicion sin escena: %ld píxeles distintos del marco de la GPU", okc ? df.bad : -1L);
        vl2d_free(L);
    }
    /* B. con la VRAM real de cierre: fidelidad 100 % y sin respaldo */
    {
        VrLayer2D *L = vl2d_new();
        vl2d_set_options(L, 1, VL_SMOOTH_ORIGINAL);
        vl2d_run(L, c0, &sc, 1);
        VrL2Info in; vl2d_info(L, &in);
        CHECK(in.fid_n > 70000 && in.fid_bad == 0 && in.fallback == 0, "composicion con vram_end: fidelidad %d/%d, respaldo %d", in.fid_n - in.fid_bad, in.fid_n, in.fallback);
        vl2d_free(L);
    }
    /* C. capa NO fiel (se pierde la copia VRAM->VRAM del hardware): la medida lo detecta y se usa la imagen exacta del emulador */
    {
        GpuDrawList *t = dup_list(c0);
        int dropped = 0;
        for (uint32_t i = 0; i < t->nops; i++) if (t->ops[i].type == GPU_OP_VCOPY) { t->ops[i].type = GPU_OP_DOWNLOAD; dropped++; }
        CHECK(dropped == 1, "composicion: una copia VRAM->VRAM en la lista (%d)", dropped);
        VrScene s2;
        vr_scene_build(t, NULL, &o, &s2);
        GpuDrawList tn = *t; tn.vram_end = NULL;
        VrLayer2D *L = vl2d_new();
        vl2d_set_options(L, 1, VL_SMOOTH_ORIGINAL);
        vl2d_run(L, &tn, &s2, 1);
        int w = 0, h = 0;
        VrL2Diff df;
        const uint8_t *rgn = vl2d_rgba(L, &w, &h);
        vl2d_compare(rgn, w, h, 1, ref, cd.fw, cd.h, &df);
        CHECK(df.bad > 500, "composicion sin la copia: la capa se aparta de la GPU (%ld píxeles) y la medida lo ve", df.bad);
        vl2d_run(L, t, &s2, 1);   /* con vram_end: respaldo exacto */
        VrL2Info in; vl2d_info(L, &in);
        rgn = vl2d_rgba(L, &w, &h);
        vl2d_compare(rgn, w, h, 1, ref, cd.fw, cd.h, &df);
        CHECK(in.fallback == 1 && in.fid_bad > 500 && df.bad == 0, "composicion no fiel: respaldo=%d (fid_bad %d), tras el respaldo %ld píxeles distintos de la GPU", in.fallback, in.fid_bad, df.bad);
        printf("  capa no fiel: %d píxeles distintos, dif media %d/1000 niveles -> respaldo con la imagen del emulador: %ld distintos\n", in.fid_bad, in.fid_mean_milli, df.bad);
        vl2d_free(L);
        free_list(t);
    }
    /* D. panel sobre el mundo: solo se pintan las primitivas con destino en la zona mostrada. Todo píxel opaco de la capa es el de la GPU (ni restos de la composición fuera de pantalla
     *    ni el estado final de la lista en las texturas); las copias desde fuera de la pantalla aparecen. */
    {
        VrLayer2D *L = vl2d_new();
        vl2d_set_options(L, 1, VL_SMOOTH_ORIGINAL);
        CHECK(vl2d_run(L, c0, &sc, 0) == 1, "composicion panel: vl2d_run");
        int w = 0, h = 0;
        const uint8_t *rg = vl2d_rgba(L, &w, &h);
        long op = 0, bad = 0, copied = 0;
        for (int y = 0; y < h && rg; y++) for (int x = 0; x < w; x++) {
            const uint8_t *p = &rg[((size_t)y * w + x) * 4];
            if (p[3] != 255) continue;
            if (y >= 70 && y < 94) continue;   /* tiras que releen la pantalla (efecto de pantalla): el panel no tiene el mundo/imagen de debajo; es trabajo del filtro o de la pantalla completa */
            op++;
            if (memcmp(p, &ref[((size_t)y * cd.fw + x) * 4], 3)) bad++;
            if (y >= 20 && y < 52 && x >= 96 && x < 224) copied++;
        }
        CHECK(rg && op > 3000 && bad == 0, "composicion panel: %ld píxeles opacos, %ld distintos de la GPU", op, bad);
        CHECK(copied > 2000, "composicion panel: la copia VRAM->VRAM desde fuera de pantalla aparece (%ld píxeles)", copied);
        VrL2Info in; vl2d_info(L, &in);
        CHECK(in.mirror_prims >= 24, "composicion panel: %d primitivas solo a la VRAM espejo", in.mirror_prims);
        vl2d_free(L);
    }
    /* E. alta resolución "nítido" (R=S): los borrados/copias a la zona mostrada también llegan a la capa */
    {
        VrLayer2D *L = vl2d_new();
        vl2d_set_options(L, 2, VL_SMOOTH_SHARP);
        vl2d_run(L, &cn, &sc, 1);
        int w = 0, h = 0;
        const uint8_t *rg = vl2d_rgba(L, &w, &h);
        long ghost = 0;
        for (int y = 24; y < 56 && rg; y++) for (int x = 100; x < 220; x++) {   /* región copiada: ningún píxel puede conservar el texto fantasma de la VRAM de partida */
            const uint8_t *p = &rg[((size_t)(y * 2) * w + x * 2) * 4], *q = &ref[((size_t)y * cd.fw + x) * 4];
            int d = abs(p[0] - q[0]) + abs(p[1] - q[1]) + abs(p[2] - q[2]);
            if (d > 24) ghost++;
        }
        CHECK(rg && ghost < 40, "composicion x2 nitido: restos del fotograma anterior en la región copiada (%ld)", ghost);
        vl2d_free(L);
    }
    free(ref);
    free_list(c0);
}

static void semi_mask_cases(void) {
    /* A. relleno semitransparente sobre negro (4 modos de mezcla): visible y igual a la GPU original. CLUT: 1 = blanco semitransparente, 2 = verde opaco (contorno), 3 = rojo semi */
    uint16_t font[64 * 8];
    memset(font, 0, sizeof font);
    for (int gl = 0; gl < 4; gl++) for (int v = 0; v < 8; v++) for (int u = 0; u < 8; u++) {
        int tu = gl * 8 + u, edge = u == 0 || v == 0 || u == 7 || v == 7, idx = edge ? 2 : (gl & 1 ? 3 : 1);
        font[v * 64 + (tu >> 2)] = (uint16_t)(font[v * 64 + (tu >> 2)] | (idx << ((tu & 3) * 4)));
    }
    const uint16_t clut[4] = {0, 0x8000 | 0x5AD6, 0x03E0, 0x8000 | 0x001F};
    for (int m = 0; m < 4; m++) {
        gpu_capture_enable(0);
        gpu_reset();
        gpu_gp1(0x08000001); gpu_gp1(0x03000000); gpu_gp1(0x05000000 | 256u << 10);
        env(0, 0, 1023, 511, 0, 0, 0);
        gpu_gp0(0x02000000); gpu_gp0(xy(0, 0)); gpu_gp0(xy(320, 240));
        upload(640, 0, 64, 8, font); upload(0, 500, 4, 1, clut);
        gpu_capture_vram_start(1);
        gpu_capture_enable(1);
        uint32_t clutw = 0 | 500u << 6;
        env(0, 0, 319, 239, 10 | (uint32_t)m << 5, 0, 0);
        for (int i = 0; i < 4; i++) { gpu_gp0(0x66u << 24 | 0x808080); gpu_gp0(xy(20 + i * 10, 30)); gpu_gp0(clutw << 16 | (uint32_t)(i * 8)); }   /* 0x66: sprite 8x8... semitransparente modulado */
        gpu_gp1(0x05000000);
        const GpuDrawList *l = gpu_capture_frame();
        gpu_capture_vram_start(0);
        if (!l) { CHECK(0, "semi %d: lista", m); continue; }
        VrScene sc;
        GpuDrawList *c = run_list(l, &sc);
        if (!c) { CHECK(0, "semi %d: escena", m); continue; }
        int nz = 0, ref_nz = 0, bad = 0, w = 0, h = 0;
        for (int y = 30; y < 38; y++) for (int x = 20; x < 60; x++) if (gpu_vram[y][x] & 0x7FFF) ref_nz++;
        VrLayer2D *L = vl2d_new();
        vl2d_set_options(L, 4, VL_SMOOTH_ORIGINAL);
        vl2d_run(L, c, &sc, 0);
        const uint8_t *rg = vl2d_rgba(L, &w, &h);
        for (int y = 30; y < 38 && rg; y++) for (int x = 20; x < 60; x++) {
            const uint8_t *q = &rg[((size_t)(y * 4) * w + x * 4) * 4];
            int pv[3] = {q[0] * q[3] / 255, q[1] * q[3] / 255, q[2] * q[3] / 255};   /* sobre negro: color * alfa */
            if (pv[0] | pv[1] | pv[2]) nz++;
            for (int k = 0; k < 3; k++) { int v5 = (gpu_vram[y][x] >> (5 * k)) & 31, e = (v5 << 3) | (v5 >> 2); if (abs(pv[k] - e) > 3) { bad++; break; } }
        }
        CHECK(m == 2 || ref_nz > 20, "semi %d: el oráculo pinta (%d)", m, ref_nz);
        CHECK(nz == ref_nz, "semi %d: relleno visible sobre negro: %d píxeles frente a %d de la GPU", m, nz, ref_nz);
        CHECK(bad == 0, "semi %d: %d píxeles con color distinto sobre negro", m, bad);
        vl2d_free(L);
        free_list(c);
    }
    /* B. máscara: la VRAM mostrada trae el bit 15 puesto en un bloque; el texto con «comprobar máscara» (E6 bit 1) no debe pisarlo (pantalla completa) */
    {
        gpu_capture_enable(0);
        gpu_reset();
        gpu_gp1(0x08000001); gpu_gp1(0x03000000); gpu_gp1(0x05000000 | 256u << 10);
        env(0, 0, 1023, 511, 0, 0, 0);
        gpu_gp0(0x02000000); gpu_gp0(xy(0, 0)); gpu_gp0(xy(320, 240));
        upload(640, 0, 64, 8, font); upload(0, 500, 4, 1, clut);
        for (int y = 30; y < 38; y++) for (int x = 20; x < 40; x++) gpu_vram[y][x] = 0x8000 | 0x1234;   /* protegido por el bit 15 */
        gpu_capture_vram_start(1);
        gpu_capture_enable(1);
        env(0, 0, 319, 239, 10, 0, 2);
        for (int i = 0; i < 4; i++) { gpu_gp0(0x64u << 24 | 0x808080); gpu_gp0(xy(20 + i * 10, 30)); gpu_gp0(0u << 16 | 500u << 22 | (uint32_t)(i * 8)); gpu_gp0(xy(8, 8)); }
        gpu_gp1(0x05000000);
        const GpuDrawList *l = gpu_capture_frame();
        gpu_capture_vram_start(0);
        VrScene sc;
        GpuDrawList *c = l ? run_list(l, &sc) : NULL;
        CHECK(c != NULL, "máscara: lista");
        if (c) {
            VrLayer2D *L = vl2d_new();
            vl2d_set_options(L, 1, VL_SMOOTH_ORIGINAL);
            vl2d_run(L, c, &sc, 1);
            int w = 0, h = 0, bad = 0;
            const VrLayerPx *p = vl2d_px(L, &w, &h);
            for (int y = 0; y < 240 && p; y++) for (int x = 0; x < 320; x++) {
                uint8_t bg[3] = {0, 0, 0}, o5[3];
                vl2d_compose5(&p[y * w + x], bg, o5);
                for (int k = 0; k < 3; k++) if (o5[k] != (int)((gpu_vram[y][x] >> (5 * k)) & 31)) { bad++; break; }
            }
            CHECK(bad == 0, "máscara en pantalla completa: %d píxeles distintos de la GPU original", bad);
            vl2d_free(L);
            free_list(c);
        }
    }
}


/* ---------------- panel verificado (pantalla 3D del motor + panel 2D encima) ---------------- */
/* Escena: mundo (cuadrilátero con degradado + triángulo), texto opaco de 4 glifos, un fundido B/2+F/2 y, si occ > 0, un rectángulo del MUNDO dibujado DESPUÉS del texto (orden roto: el
 * panel no puede reproducirlo). op_dest: 0 mundo, 2 panel. Devuelve la lista (con vram_end del hardware). */
static GpuDrawList *panel_scene(int occ, uint8_t *opd, int *nopd) {
    uint16_t font[64 * 8];
    memset(font, 0, sizeof font);
    for (int gl = 0; gl < 4; gl++) for (int v = 0; v < 8; v++) for (int u = 0; u < 8; u++) {
        int tu = gl * 8 + u, idx = 1 + (u + v + gl) % 3;
        font[v * 64 + (tu >> 2)] = (uint16_t)(font[v * 64 + (tu >> 2)] | (idx << ((tu & 3) * 4)));
    }
    const uint16_t clut[4] = {0, 0x7FFF, 0x03E0, 0x7C1F};
    gpu_capture_enable(0);
    gpu_reset();
    gpu_gp1(0x08000001); gpu_gp1(0x03000000); gpu_gp1(0x05000000 | 256u << 10);
    env(0, 0, 1023, 511, 0, 0, 0);
    gpu_gp0(0x02000000); gpu_gp0(xy(0, 0)); gpu_gp0(xy(512, 256));
    upload(640, 0, 64, 8, font); upload(0, 500, 4, 1, clut);
    gpu_capture_vram_start(1);
    gpu_capture_enable(1);
    env(0, 0, 319, 239, 10, 0, 0);
    /* mundo: cuadrilátero con degradado y un triángulo */
    gpu_gp0(0x38000000 | 0x200000); gpu_gp0(xy(0, 0)); gpu_gp0(0x00A000); gpu_gp0(xy(319, 0)); gpu_gp0(0xA00000); gpu_gp0(xy(0, 239)); gpu_gp0(0x808080); gpu_gp0(xy(319, 239));
    gpu_gp0(0x30000000 | 0x4040FF); gpu_gp0(xy(160, 50)); gpu_gp0(0xFF4040); gpu_gp0(xy(260, 180)); gpu_gp0(0x40FF40); gpu_gp0(xy(80, 200));
    /* panel: texto (glifos de la CLUT: opacos de colores) y fundido B/2+F/2 */
    for (int i = 0; i < 4; i++) { gpu_gp0(0x64u << 24 | 0x808080); gpu_gp0(xy(20 + i * 10, 30)); gpu_gp0(0u << 16 | 500u << 22 | (uint32_t)(i * 8)); gpu_gp0(xy(8, 8)); }
    env(0, 0, 319, 239, 10, 0, 0);
    gpu_gp0(0x62000000 | 0xC0A060); gpu_gp0(xy(150, 100)); gpu_gp0(xy(100, 40));       /* 0x62: rectángulo monocromo semitransparente */
    if (occ > 0) { gpu_gp0(0x60000000 | 0x20FF20); gpu_gp0(xy(18, 28)); gpu_gp0(xy(occ, occ)); }   /* mundo DESPUÉS del texto, encima de él */
    gpu_gp1(0x05000000);
    const GpuDrawList *l = gpu_capture_frame();
    gpu_capture_vram_start(0);
    if (!l) return NULL;
    GpuDrawList *c = dup_list(l);
    *nopd = (int)c->nops;
    for (int i = 0; i < *nopd && i < 64; i++) opd[i] = (i == 0 || i == 1 || (occ > 0 && i == *nopd - 1)) ? 0 : 2;
    return c;
}

/* todo píxel opaco de la capa (escala 1) es igual a la imagen del hardware */
static int opaque_vs_hw(VrLayer2D *L, const GpuDisplay *d, int *n_opaque) {
    int w = 0, h = 0, bad = 0, n = 0;
    const VrLayerPx *p = vl2d_px(L, &w, &h);
    if (!p) return -1;
    for (int y = 0; y < h; y++) for (int x = 0; x < w && x < d->aw; x++) {
        const VrLayerPx *q = &p[(size_t)y * w + x];
        if (q->t != 0 || q->d[0] || q->d[1] || q->d[2]) continue;
        n++;
        uint16_t v = gpu_vram[d->y + y][d->x + x];
        if (q->c[0] != (v & 31) || q->c[1] != ((v >> 5) & 31) || q->c[2] != ((v >> 10) & 31)) bad++;
    }
    if (n_opaque) *n_opaque = n;
    return bad;
}

static void panel_verified(void) {
    uint8_t opd[64]; int nops = 0;
    VrScene sc; memset(&sc, 0, sizeof sc);
    GpuDrawList *ok = panel_scene(0, opd, &nops);
    CHECK(ok && nops == 7 && ok->vram_end, "panel verificado: lista de %d primitivas con vram_end", nops);
    if (!ok || nops != 7) { free_list(ok); return; }
    GpuDisplay d = ok->disp;
    sc.op_dest = opd; sc.n_op_dest = (uint32_t)nops;
    /* A. panel fiel: se verifica, no hay diferencias, no hay respaldo y la capa es IGUAL a la del panel sin verificar */
    VrLayer2D *Lp = vl2d_new(), *Lv = vl2d_new();
    vl2d_set_options(Lp, 1, VL_SMOOTH_ORIGINAL); vl2d_set_options(Lv, 1, VL_SMOOTH_ORIGINAL);
    CHECK(vl2d_run(Lp, ok, &sc, VL_PANEL) == 1 && vl2d_run(Lv, ok, &sc, VL_PANEL_VERIFIED) == 1, "A: capas");
    VrL2Info iv; vl2d_info(Lv, &iv);
    CHECK(iv.verify == 1 && iv.v_cov > 400 && iv.v_bad == 0 && iv.fallback == 0 && iv.v_patched == 0, "A: panel fiel: verificado %d, tocados %d, distintos %d, respaldo %d, sustituidos %d", iv.verify, iv.v_cov, iv.v_bad, iv.fallback, iv.v_patched);
    CHECK(iv.v_out == 0, "A: el mundo reproducido coincide con el hardware fuera del panel (%d px)", iv.v_out);
    { int w1, h1, w2, h2; const uint8_t *a = vl2d_rgba(Lp, &w1, &h1), *b = vl2d_rgba(Lv, &w2, &h2);
      CHECK(a && b && w1 == w2 && h1 == h2 && !memcmp(a, b, (size_t)w1 * h1 * 4), "A: verificar un panel fiel no cambia ni un byte"); }
    int nop = 0;
    CHECK(opaque_vs_hw(Lv, &d, &nop) == 0 && nop > 200, "A: píxeles opacos del panel = hardware (%d opacos)", nop);
    /* sin vram_end: no se puede verificar; sigue siendo el panel */
    { GpuDrawList nv = *ok; nv.vram_end = NULL; VrLayer2D *Ln = vl2d_new(); vl2d_set_options(Ln, 1, VL_SMOOTH_ORIGINAL);
      vl2d_run(Ln, &nv, &sc, VL_PANEL_VERIFIED); VrL2Info in; vl2d_info(Ln, &in);
      CHECK(in.verify_req == 1 && in.verify == 0 && in.fallback == 0, "sin vram_end: pedida y no aplicable");
      int w1, h1, w2, h2; const uint8_t *a = vl2d_rgba(Lp, &w1, &h1), *b = vl2d_rgba(Ln, &w2, &h2);
      CHECK(a && b && w1 == w2 && !memcmp(a, b, (size_t)w1 * h1 * 4), "sin vram_end: el panel es el de siempre");
      vl2d_free(Ln); }
    /* B. el panel pierde la fidelidad (color del texto distinto al del hardware: p. ej. otro rasterizado): sin verificar la capa difiere del hardware; verificada, la capa es el hardware */
    { GpuDrawList *bad = dup_list(ok);
      for (int i = 2; i < 6; i++) bad->ops[i].v[0].r = bad->ops[i].v[0].g = bad->ops[i].v[0].b = 0x40;
      VrLayer2D *Lb = vl2d_new(), *Lb2 = vl2d_new(); vl2d_set_options(Lb, 4, VL_SMOOTH_ORIGINAL); vl2d_set_options(Lb2, 4, VL_SMOOTH_ORIGINAL);
      vl2d_run(Lb, bad, &sc, VL_PANEL);
      int nb = 0; opaque_vs_hw(Lb, &d, &nb);   /* (a escala 4 original vl2d_px es la capa de rasterizado 1x) */
      CHECK(opaque_vs_hw(Lb, &d, NULL) > 20, "B: sin verificar, el texto roto difiere del hardware (%d px)", opaque_vs_hw(Lb, &d, NULL));
      CHECK(vl2d_run(Lb2, bad, &sc, VL_PANEL_VERIFIED) == 1, "B: capa");
      VrL2Info ib; vl2d_info(Lb2, &ib);
      CHECK(ib.verify == 1 && ib.v_bad >= 64 && ib.fallback == 1 && ib.v_patched > 0 && ib.v_patched < d.aw * d.h / 4, "B: respaldo de lo que toca el panel: distintos %d, sustituidos %d, respaldo %d", ib.v_bad, ib.v_patched, ib.fallback);
      /* A1: el respaldo NO opaca la pantalla: lo opaco es solo el texto (= hardware) y casi todo el resto de la capa es transparente (se ve el 3D del motor) */
      int nbb = 0;
      CHECK(opaque_vs_hw(Lb2, &d, &nbb) == 0 && nbb > 0 && nbb < 1200, "B: lo opaco de la capa verificada es el hardware y no cubre la pantalla (%d opacos de %d)", nbb, d.aw * d.h);
      { int w1 = 0, h1 = 0, ntr = 0; const VrLayerPx *pp = vl2d_px(Lb2, &w1, &h1);
        for (int y = 0; y < h1 && pp; y++) for (int x = 0; x < d.aw && x < w1; x++) { const VrLayerPx *q = &pp[(size_t)y * w1 + x]; if (q->t == 256 && !q->c[0] && !q->c[1] && !q->c[2] && !q->d[0] && !q->d[1] && !q->d[2]) ntr++; }
        CHECK(ntr > d.aw * d.h * 9 / 10, "B: el resto de la capa queda transparente (%d de %d)", ntr, d.aw * d.h);
        /* A2: el rectángulo semitransparente se recupera por resta: sobre el MUNDO reproducido da el hardware (sin llevar el mundo PS1 dentro) */
        GpuDrawList wl = *ok; wl.nops = 2; wl.vram_end = NULL;
        VrLayer2D *Lw = vl2d_new(); vl2d_set_options(Lw, 1, VL_SMOOTH_ORIGINAL);
        int okw = vl2d_run(Lw, &wl, &sc, VL_FULL);
        int w2 = 0, h2 = 0; const VrLayerPx *wp = vl2d_px(Lw, &w2, &h2);
        int nrect = 0, mism = 0, nsemi = 0;
        for (int y = 100; y < 140 && okw && wp && pp; y++) for (int x = 150; x < 250; x++) {
            const VrLayerPx *q = &pp[(size_t)y * w1 + x], *wq = &wp[(size_t)y * w2 + x];
            uint8_t bg[3] = {wq->c[0], wq->c[1], wq->c[2]}, o5[3];
            vl2d_compose5(q, bg, o5);
            uint16_t hv = gpu_vram[d.y + y][d.x + x];
            int e = abs(o5[0] - (hv & 31)); int e1 = abs(o5[1] - ((hv >> 5) & 31)), e2 = abs(o5[2] - ((hv >> 10) & 31));
            if (e1 > e) e = e1;
            if (e2 > e) e = e2;
            nrect++; if (e > 2) mism++;
            if (q->t == 256 && (q->c[0] | q->c[1] | q->c[2] | q->d[0] | q->d[1] | q->d[2])) nsemi++;
        }
        CHECK(okw && nrect == 4000 && mism == 0 && nsemi > 1000, "B/A2: la resta devuelve el hardware sobre el mundo reproducido (%d px, %d distintos, %d por resta)", nrect, mism, nsemi);
        CHECK(ib.v_sub > 1000, "B/A2: contados por resta: %d", ib.v_sub);
        vl2d_free(Lw);
        /* mismo mundo pero 3D del motor distinto: la contribución del panel se conserva (suma / resta), no se copia el mundo PS1 */
        uint8_t bgE[3] = {9, 9, 9}, o5[3]; const VrLayerPx *q = &pp[(size_t)120 * w1 + 200];
        vl2d_compose5(q, bgE, o5);
        uint16_t hv = gpu_vram[d.y + 120][d.x + 200];
        CHECK(q->t == 256 && !(o5[0] == (hv & 31) && o5[1] == ((hv >> 5) & 31) && o5[2] == ((hv >> 10) & 31)) , "B/A2: sobre otro 3D la capa NO reproduce el píxel del emulador con su mundo dentro");
      }
      /* salida RGBA a escala 4 en lo opaco: el hardware replicado */
      int w = 0, h = 0; const uint8_t *rg = vl2d_rgba(Lb2, &w, &h);
      size_t fwh = (size_t)d.fw * d.h; uint8_t *ref = (uint8_t *)malloc(fwh * 4);
      GpuDisplay dd = d;
      CHECK(gpu_present_rgba(&dd, NULL, ref, fwh) == fwh, "B: marco de la GPU");
      int diff = 0, nopq4 = 0;
      for (int y = 0; y < h && rg; y++) for (int x = 0; x < d.aw * 4 && x < w; x++) {
          if (rg[((size_t)y * w + x) * 4 + 3] != 255) continue;
          nopq4++;
          if (memcmp(&rg[((size_t)y * w + x) * 4], &ref[((size_t)(y / 4) * d.fw + d.pad_l + x / 4) * 4], 3)) diff++;
      }
      CHECK(diff == 0 && nopq4 > 0 && nopq4 < d.aw * d.h * 4 * 4 / 10, "B: RGBA a x4: lo opaco es el hardware (%d opacas, %d distintas)", nopq4, diff);
      free(ref);
      /* C. antirrebote: tras un fallo el respaldo dura 12 listas aunque el panel vuelva a ser fiel; después se recupera y no quedan restos del respaldo */
      int fb = 0;
      for (int i = 0; i < 12; i++) { vl2d_run(Lb2, ok, &sc, VL_PANEL_VERIFIED); vl2d_info(Lb2, &ib); fb += ib.fallback; }
      CHECK(fb == 12, "C: 12 listas de respaldo tras el fallo (%d)", fb);
      vl2d_run(Lb2, ok, &sc, VL_PANEL_VERIFIED); vl2d_info(Lb2, &ib);
      CHECK(ib.fallback == 0 && ib.v_bad == 0, "C: recuperado (respaldo %d, distintos %d)", ib.fallback, ib.v_bad);
      VrLayer2D *Lf = vl2d_new(); vl2d_set_options(Lf, 4, VL_SMOOTH_ORIGINAL); vl2d_run(Lf, ok, &sc, VL_PANEL);
      int w1, h1, w2, h2; const uint8_t *a = vl2d_rgba(Lf, &w1, &h1), *b = vl2d_rgba(Lb2, &w2, &h2);
      CHECK(a && b && w1 == w2 && !memcmp(a, b, (size_t)w1 * h1 * 4), "C: sin restos del respaldo: igual a un panel recién creado");
      /* un modo sin verificar reinicia la cuenta */
      vl2d_run(Lb2, bad, &sc, VL_PANEL_VERIFIED); vl2d_run(Lb2, ok, &sc, VL_PANEL); vl2d_run(Lb2, ok, &sc, VL_PANEL_VERIFIED); vl2d_info(Lb2, &ib);
      CHECK(ib.fallback == 0, "C: salir de la pantalla 3D reinicia el antirrebote");
      vl2d_free(Lf); vl2d_free(Lb); vl2d_free(Lb2); free_list(bad); }
    {   /* coste (informativo): el panel verificado frente al panel sin verificar, en la misma lista */
        double t0 = now_ms();
        for (int i = 0; i < 100; i++) vl2d_run(Lp, ok, &sc, VL_PANEL);
        double t1 = now_ms();
        for (int i = 0; i < 100; i++) vl2d_run(Lv, ok, &sc, VL_PANEL_VERIFIED);
        double t2 = now_ms();
        printf("  panel: %.2f ms/lista sin verificar, %.2f ms/lista verificado (mundo a pantalla completa: 1 cuadrilátero + 1 triángulo)\n", (t1 - t0) / 100, (t2 - t1) / 100);
    }
    vl2d_free(Lp); vl2d_free(Lv); free_list(ok);
    /* D. orden roto: una primitiva del MUNDO dibujada encima del texto. Grande (20x20): respaldo exacto; pequeña (4x4, solapa 4 px del texto): solo se sustituyen esos píxeles */
    for (int sz = 20; sz >= 4; sz -= 16) {
        GpuDrawList *oc = panel_scene(sz, opd, &nops);
        CHECK(oc && nops == 8, "D%d: lista de %d primitivas", sz, nops);
        if (!oc || nops != 8) { free_list(oc); continue; }
        sc.op_dest = opd; sc.n_op_dest = (uint32_t)nops;
        GpuDisplay d2 = oc->disp;
        VrLayer2D *L = vl2d_new(); vl2d_set_options(L, 1, VL_SMOOTH_ORIGINAL);
        vl2d_run(L, oc, &sc, VL_PANEL_VERIFIED);
        VrL2Info io; vl2d_info(L, &io);
        int nopq = 0;
        CHECK(opaque_vs_hw(L, &d2, &nopq) == 0, "D%d: todo píxel opaco = hardware tras verificar", sz);
        if (sz == 20) CHECK(io.v_bad >= 8 && io.fallback == 1, "D20: distintos %d, respaldo %d", io.v_bad, io.fallback);
        else CHECK(io.v_bad > 0 && io.v_bad < 8 && io.fallback == 0 && io.v_patched == io.v_bad, "D4: distintos %d, sustituidos %d, respaldo %d", io.v_bad, io.v_patched, io.fallback);
        vl2d_free(L); free_list(oc);
    }
    /* A1: umbral relativo (> 2 % de los píxeles que toca el panel) con mínimo absoluto de 64 píxeles */
    {
        GpuDrawList *o10 = panel_scene(10, opd, &nops);   /* tapa el glifo 0 entero: 64 distintos de ~4250 tocados (1,5 %): ni el 2 % */
        if (o10 && nops == 8) {
            sc.op_dest = opd; sc.n_op_dest = (uint32_t)nops;
            VrLayer2D *L = vl2d_new(); vl2d_set_options(L, 1, VL_SMOOTH_ORIGINAL);
            vl2d_run(L, o10, &sc, VL_PANEL_VERIFIED);
            VrL2Info i10; vl2d_info(L, &i10);
            CHECK(i10.v_bad == 64 && i10.fallback == 0 && i10.v_patched == 64, "A1: 64 distintos de %d tocados (%.1f %%) no llegan al 2 %%: solo los suyos (distintos %d, respaldo %d, sustituidos %d)", i10.v_cov, 100.0 * 64 / (i10.v_cov ? i10.v_cov : 1), i10.v_bad, i10.fallback, i10.v_patched);
            vl2d_free(L);
        } else CHECK(0, "A1: lista sz=10");
        free_list(o10);
        for (int sz = 9; sz <= 10; sz++) {   /* panel solo de texto (el rectángulo semitransparente pasa al mundo): 49 distintos no llegan a 64, 64 sí, aunque ambos superen el 2 % */
            GpuDrawList *o = panel_scene(sz, opd, &nops);
            if (!o || nops != 8) { CHECK(0, "A1: lista sz=%d", sz); free_list(o); continue; }
            opd[6] = 0;
            sc.op_dest = opd; sc.n_op_dest = (uint32_t)nops;
            VrLayer2D *L = vl2d_new(); vl2d_set_options(L, 1, VL_SMOOTH_ORIGINAL);
            vl2d_run(L, o, &sc, VL_PANEL_VERIFIED);
            VrL2Info im; vl2d_info(L, &im);
            CHECK(im.v_bad == (sz == 9 ? 49 : 64) && im.fallback == (sz == 10) && im.v_cov < 400, "A1: solo texto, tapa %d: distintos %d de %d tocados, respaldo %d (minimo absoluto 64)", sz, im.v_bad, im.v_cov, im.fallback);
            if (sz == 10) { int w1 = 0, h1 = 0, ntr = 0; const VrLayerPx *pp = vl2d_px(L, &w1, &h1);
                for (int y = 0; y < h1 && pp; y++) for (int x = 0; x < o->disp.aw && x < w1; x++) { const VrLayerPx *q = &pp[(size_t)y * w1 + x]; if (q->t == 256 && !(q->c[0] | q->c[1] | q->c[2] | q->d[0] | q->d[1] | q->d[2])) ntr++; }
                CHECK(ntr > o->disp.aw * o->disp.h * 95 / 100, "A1: respaldo solo sobre el texto dilatado 1 px: resto transparente (%d de %d)", ntr, o->disp.aw * o->disp.h);
                CHECK(opaque_vs_hw(L, &o->disp, NULL) == 0, "A1: lo opaco = hardware"); }
            vl2d_free(L); free_list(o);
        }
        /* A2: mundo reproducido NO fiable (el color del mundo de la lista no es el del hardware): el panel semitransparente se deja transparente, lo opaco usa el color del emulador */
        GpuDrawList *oc = panel_scene(0, opd, &nops);
        if (oc && nops == 7) {
            GpuDrawList *bw = dup_list(oc);
            bw->ops[0].v[0].r = bw->ops[0].v[1].r = bw->ops[0].v[2].r = bw->ops[0].v[3].r = 0x00;
            for (int k = 0; k < 4; k++) { bw->ops[0].v[k].g = 0xFF; bw->ops[0].v[k].b = 0x00; }
            sc.op_dest = opd; sc.n_op_dest = (uint32_t)nops;
            VrLayer2D *L = vl2d_new(); vl2d_set_options(L, 1, VL_SMOOTH_ORIGINAL);
            vl2d_run(L, bw, &sc, VL_PANEL_VERIFIED);
            VrL2Info iu; vl2d_info(L, &iu);
            CHECK(iu.verify == 1 && iu.v_out > 1000 && iu.fallback == 1 && iu.v_transp > 1000 && iu.v_sub < iu.v_transp, "A2: mundo no fiable: fuera distintos %d, transparentes %d, por resta %d", iu.v_out, iu.v_transp, iu.v_sub);
            int nsemi_bad = 0; { int w1 = 0, h1 = 0; const VrLayerPx *pp = vl2d_px(L, &w1, &h1);
                for (int y = 100; y < 140 && pp; y++) for (int x = 150; x < 250; x++) { const VrLayerPx *q = &pp[(size_t)y * w1 + x]; if (q->t == 0) nsemi_bad++; } }
            CHECK(nsemi_bad == 0, "A2: el semitransparente no se opaca con el mundo PS1 (%d opacos)", nsemi_bad);
            CHECK(opaque_vs_hw(L, &oc->disp, NULL) == 0, "A2: lo opaco = hardware");
            vl2d_free(L); free_list(bw);
        } else CHECK(0, "A2: lista");
        free_list(oc);
    }
    /* E. asíncrono: el modo verificado publica un PANEL (full = 0), la línea [capa2d] lo cuenta */
    {
        GpuDrawList *oc = panel_scene(0, opd, &nops);
        VrL2Async *A = vl2a_new();
        vl2a_options(A, 4, VL_SMOOTH_ORIGINAL);
        CHECK(vl2a_submit(A, oc, opd, VL_PANEL_VERIFIED, 1) == 1, "E: encolada");
        vl2a_flush(A);
        const uint8_t *px = NULL; int w = 0, h = 0, full = -1; uint32_t seq = 0;
        int r = vl2a_acquire(A, &px, &w, &h, &full, &seq);
        CHECK(r == 1 && px && full == 0, "E: capa publicada como panel (r=%d full=%d)", r, full);
        vl2a_release(A);
        char lb[1024]; vl2a_report(A, lb, sizeof lb);
        CHECK(strstr(lb, "panel prims=") && strstr(lb, "verificacion=si panel_px=") && strstr(lb, "distintos=0") && strstr(lb, "respaldo_emulador=no"), "E: línea de registro: %s", lb);
        vl2a_free(A); free_list(oc);
    }
}

int main(void) {
    gen_atlas();
    edge_case("320x240", 1, 0x270, 0xC70, 0x13, 0x13 + 240);
    edge_case("320x256", 1, 0x270, 0xC70, 0x13, 0x13 + 256);
    edge_case("368x240 (modo 384, rango 368)", 0x40, 0x230, 0x230 + 368 * 7, 0x13, 0x13 + 240);
    edge_case("384x256", 0x40, 0x230, 0xCB0, 0x13, 0x13 + 256);
    edge_case("512x240", 2, 0x27B, 0xC7B, 0x13, 0x13 + 240);
    edge_case("512x256", 2, 0x27B, 0xC7B, 0x13, 0x13 + 256);
    edge_case("320 con rango ampliado a 352", 1, 0x270 - 16 * 8, 0xC70 + 16 * 8, 0x13, 0x13 + 240);
    edge_case("codec: modo 384, rango 318", 0x40, 0x230, 0x230 + 318 * 7, 0x13, 0x13 + 240);
    edge_case("codec: modo 384, rango 300 desplazado 10", 0x40, 0x230 + 10 * 7, 0x230 + 310 * 7, 0x13, 0x13 + 240);
    edge_case("codec: 384 entrelazado 480 lineas", 0x40 | 0x24, 0x230, 0xCB0, 0x13, 0x13 + 240);
    edge_case("codec: 640 entrelazado 480 lineas", 0x03 | 0x24, 0x280, 0xC80, 0x13, 0x13 + 240);
    codec24_case();
    edge_case("320 con rango reducido a 288", 1, 0x270 + 16 * 8, 0x270 + 304 * 8, 0x13, 0x13 + 240);
    semi_mask_cases();
    offscreen_compose();
    offlist_text();
    panel_verified();

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
        GpuDisplay gd; gpu_get_display(&gd);
        CHECK(w == gd.fw && h == gd.h, "marco completo %dx%d (fw %d)", w, h, gd.fw);
        int badf = 0;
        for (int y = 0; y < 240 && pf; y++) for (int x = 0; x < w; x++) {
            uint8_t bg[3] = {0, 0, 0}, o5[3];
            vl2d_compose5(&pf[y * w + x], bg, o5);
            int vis = x >= gd.pad_l && x < gd.pad_l + gd.aw;
            for (int k = 0; k < 3; k++) if (o5[k] != (vis ? pix5(gd.x + x - gd.pad_l, y, k) : 0)) { badf++; break; }
        }
        CHECK(badf == 0, "fotograma completo (cine) a 1x: %d píxeles distintos de la GPU original", badf);
        vl2d_set_options(LF, 4, VL_SMOOTH_SHARP);
        double tf = now_ms();
        CHECK(vl2d_run(LF, g_list, &g_sc, 1) == 1, "vl2d_run full x4");
        printf("  vl2d_run full x4: %.1f ms\n", now_ms() - tf);
        const uint8_t *rf = vl2d_rgba(LF, &w, &h);
        int opq = 0;
        for (int i = 0; rf && i < w * h; i++) if (rf[i * 4 + 3] == 255) opq++;
        CHECK(rf && w == gd.fw * 4 && h == gd.h * 4 && opq == w * h, "cine x4 opaco (%d de %d)", opq, w * h);
        vl2d_free(LF);
    }

    /* Hilo trabajador (vr_layer2d_async): mismo resultado que la ejecución síncrona, el hilo que encola no espera y la capa lleva como mucho una lista de retraso. */
    {
        VrL2Async *A = vl2a_new();
        CHECK(A != NULL, "vl2a_new");
        vl2a_options(A, 4, VL_SMOOTH_SHARP);
        CHECK(vl2a_submit(A, g_list, g_sc.op_dest, 0, 1) == 1, "vl2a_submit");
        vl2a_flush(A);
        const uint8_t *ap = NULL; int aw = 0, ah = 0, afull = 0; uint32_t aseq = 0;
        CHECK(vl2a_acquire(A, &ap, &aw, &ah, &afull, &aseq) == 1, "vl2a_acquire nueva");
        VrLayer2D *LS = vl2d_new();
        vl2d_set_options(LS, 4, VL_SMOOTH_SHARP);
        vl2d_run(LS, g_list, &g_sc, 0);
        int sw = 0, sh = 0;
        const uint8_t *sp = vl2d_rgba(LS, &sw, &sh);
        CHECK(ap && sp && aw == sw && ah == sh && memcmp(ap, sp, (size_t)sw * sh * 4) == 0, "capa asincrona identica a la sincrona");
        CHECK(vl2a_acquire(A, &ap, &aw, &ah, &afull, &aseq) == 0, "vl2a_acquire repetida = misma");
        vl2a_release(A);
        vl2d_free(LS);
        /* ráfaga ritmada a 50 listas/s (sin vram_init: solo ops): coste de encolar y retraso */
        uint16_t *vi = g_list->vram_init; g_list->vram_init = NULL;
        double smax = 0; uint32_t lagmax = 0; int got = 0;
        for (int i = 0; i < 100; i++) {
            double t = now_ms();
            vl2a_submit(A, g_list, g_sc.op_dest, 0, 0);
            double d = now_ms() - t;
            if (d > smax) smax = d;
            usleep(20000);
            if (vl2a_acquire(A, &ap, &aw, &ah, &afull, &aseq) >= 0) { got++; vl2a_release(A); }
            int sc_; double wm, sm; uint32_t lag;
            vl2a_stats(A, &sc_, &wm, &sm, &lag);
            if (i >= 20 && lag > lagmax) lagmax = lag;
        }
        int scl; double wms, sms; uint32_t lg;
        vl2a_stats(A, &scl, &wms, &sms, &lg);
        printf("  asincrono: encolar max %.3f ms, retraso max %u listas (tras calentar), escala final %d, trabajador %.1f ms, capas recogidas %d/100\n", smax, lagmax, scl, wms, got);
        CHECK(smax < 3.0, "encolar tarda %.3f ms (>3 ms)", smax);
        CHECK(lagmax <= 1, "retraso de la capa %u listas (>1)", lagmax);
        g_list->vram_init = vi;
        /* ráfaga sin ritmo: la cola nunca bloquea al productor de forma apreciable y la VRAM espejo no se corrompe (solo eventos de las intermedias) */
        double sm2 = 0;
        for (int i = 0; i < 60; i++) { double t = now_ms(); vl2a_submit(A, g_list, g_sc.op_dest, 0, 0); double d = now_ms() - t; if (d > sm2) sm2 = d; }
        vl2a_flush(A);
        printf("  asincrono: rafaga sin ritmo, encolar max %.3f ms\n", sm2);
        vl2a_free(A);
    }

    vl2d_free(L);
    if (fails) { printf("test_layer2d: %d FALLOS\n", fails); return 1; }
    printf("test_layer2d: OK\n");
    return 0;
}
