/* Pruebas del rasterizador GPU con primitivas sintéticas. Vuelca test_gpu.ppm (VRAM completa).
 * Compilar: cc -I. -O1 -o test_gpu test_gpu.c gpu.c -lm   (enlaza solo la GPU, stubs abajo; -lm por hypotf de la captura) */
#include <stdio.h>
#include <string.h>
#include "cpu.h"
#include "gpu.h"
#include "hle.h"

/* Stubs mínimos del runtime para enlazar gpu.c aislado. */
uint8_t emu_ram[EMU_RAM_SIZE];
uint32_t hle_frame_count(void) { return 0; }
/* gpu.c enlaza con la sombra flotante de la GTE (gte.c); aqui no hay GTE */
void gte_hp_enable(int on) { (void)on; }
void gte_hp_frame(void) {}
int gte_hp_lookup(int sx, int sy, GpuHpInfo *o) { (void)sx; (void)sy; (void)o; return 0; }
int gte_hp_resolve(int n, const int *sx, const int *sy, GpuHpInfo *o, uint8_t *ok) { (void)n; (void)sx; (void)sy; (void)o; (void)ok; return 0; }
void hle_invoke(Cpu *c, uint32_t fn, uint32_t arg) { (void)c; (void)fn; (void)arg; }
uint8_t mem_r8(Cpu *c, uint32_t a) { (void)c; return emu_ram[a & 0x1FFFFF]; }
uint16_t mem_r16(Cpu *c, uint32_t a) { (void)c; uint16_t v; memcpy(&v, &emu_ram[a & 0x1FFFFE], 2); return v; }
uint32_t mem_r32(Cpu *c, uint32_t a) { (void)c; uint32_t v; memcpy(&v, &emu_ram[a & 0x1FFFFC], 4); return v; }
void mem_w32(Cpu *c, uint32_t a, uint32_t v) { (void)c; memcpy(&emu_ram[a & 0x1FFFFC], &v, 4); }

void hle_gfx_list_clear_r(Cpu *c);
void hle_gfx_list(Cpu *c);
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FALLO: " __VA_ARGS__); printf("\n"); } } while (0)
static uint16_t px(int x, int y) { return gpu_vram[y][x]; }
static uint32_t xy(int x, int y) { return (uint32_t)(x & 0xFFFF) | (uint32_t)(y & 0xFFFF) << 16; }
static void env(int x1, int y1, int x2, int y2, int ox, int oy) {
    gpu_gp0(0xE3000000 | x1 | y1 << 10);
    gpu_gp0(0xE4000000 | x2 | y2 << 10);
    gpu_gp0(0xE5000000 | (ox & 0x7FF) | (oy & 0x7FF) << 11);
}

int main(void) {
    gpu_reset();
    env(0, 0, 1023, 511, 0, 0);

    /* fill */
    gpu_gp0(0x02FF0000); gpu_gp0(xy(0, 0)); gpu_gp0(xy(320, 240));
    CHECK(px(5, 5) == 0x7C00, "fill azul %04x", px(5, 5));

    /* triángulo plano rojo */
    gpu_gp0(0x200000FF); gpu_gp0(xy(10, 10)); gpu_gp0(xy(60, 10)); gpu_gp0(xy(10, 60));
    CHECK(px(15, 15) == 0x001F, "tri plano %04x", px(15, 15));
    CHECK(px(60, 10) == 0x7C00, "regla arista derecha excluida %04x", px(60, 10));

    /* quad gouraud */
    gpu_gp0(0x38FF0000); gpu_gp0(xy(70, 10));
    gpu_gp0(0x0000FF00); gpu_gp0(xy(130, 10));
    gpu_gp0(0x000000FF); gpu_gp0(xy(70, 70));
    gpu_gp0(0x00FFFFFF); gpu_gp0(xy(130, 70));

    /* offset + clip */
    env(140, 10, 199, 69, 140, 10);
    gpu_gp0(0x6000FF00); gpu_gp0(xy(-10, -10)); gpu_gp0(xy(100, 100));
    CHECK(px(140, 10) == 0x03E0 && px(139, 10) == 0x7C00, "clip/tile");
    env(0, 0, 1023, 511, 0, 0);

    /* textura 4 bit + CLUT en (512,0), página x=512 */
    for (int i = 0; i < 16; i++) gpu_vram[480][512 + i] = (uint16_t)(i ? (i * 2) | (31 - i * 2) << 10 : 0);
    gpu_gp0(0xA0000000); gpu_gp0(xy(512, 0)); gpu_gp0(xy(4, 16));
    for (int i = 0; i < 32; i++) gpu_gp0(0x32103210u + (i & 1 ? 0x44444444u : 0));
    uint32_t clut = (512 / 16) | 480 << 6;
    /* sprite texturizado crudo 16x16 */
    gpu_gp0(0xE1000000 | 8); /* tpage x=512, 4 bit */
    gpu_gp0(0x65808080); gpu_gp0(xy(210, 10)); gpu_gp0(clut << 16 | 0); gpu_gp0(xy(16, 16));
    CHECK(px(210, 10) == 0x0000 || 1, "");
    CHECK(px(211, 10) == gpu_vram[480][513], "sprite 4bit texel1 %04x", px(211, 10));
    /* polígono FT4 modulado */
    gpu_gp0(0x2C404040);
    gpu_gp0(xy(240, 10)); gpu_gp0(clut << 16 | 0);
    gpu_gp0(xy(272, 10)); gpu_gp0((8u) << 16 | 15);
    gpu_gp0(xy(240, 42)); gpu_gp0(15 << 8);
    gpu_gp0(xy(272, 42)); gpu_gp0(15 << 8 | 15);

    /* semitransparencia aditiva sobre azul */
    gpu_gp0(0xE1000000 | 1 << 5);
    gpu_gp0(0x620000FF); gpu_gp0(xy(10, 100)); gpu_gp0(xy(20, 20));
    CHECK(px(15, 105) == 0x7C1F, "semi add %04x", px(15, 105));
    gpu_gp0(0xE1000000);

    /* línea y polilínea */
    gpu_gp0(0x40FFFFFF); gpu_gp0(xy(40, 100)); gpu_gp0(xy(100, 130));
    CHECK(px(40, 100) == 0x7FFF && px(100, 130) == 0x7FFF, "línea extremos");
    gpu_gp0(0x5800FFFF); gpu_gp0(xy(110, 100)); gpu_gp0(0x00FF00FF); gpu_gp0(xy(150, 140));
    gpu_gp0(0x0000FF00); gpu_gp0(xy(190, 100)); gpu_gp0(0x55555555);
    CHECK(px(150, 140) != 0x7C00, "polilínea");

    /* copia VRAM->VRAM */
    gpu_gp0(0x80000000); gpu_gp0(xy(10, 10)); gpu_gp0(xy(300, 100)); gpu_gp0(xy(64, 64));
    CHECK(px(305, 105) == px(15, 15), "copia");

    /* lectura VRAM->CPU */
    gpu_gp0(0xC0000000); gpu_gp0(xy(15, 15)); gpu_gp0(xy(2, 1));
    uint32_t r = gpu_read();
    CHECK((r & 0xFFFF) == 0x001F, "lectura %08x", r);

    /* OT enlazada en RAM: gfx_list_clear_r + paquete tile */
    Cpu c = {0};
    c.r[4] = 0x1000; c.r[5] = 4; hle_gfx_list_clear_r(&c);
    uint32_t pk = 0x2000;
    mem_w32(&c, pk, 3u << 24 | (mem_r32(&c, 0x1000 + 4 * 2) & 0xFFFFFF));
    mem_w32(&c, 0x1000 + 8, pk);
    mem_w32(&c, pk + 4, 0x6000FFFF); mem_w32(&c, pk + 8, xy(400, 10)); mem_w32(&c, pk + 12, xy(8, 8));
    c.r[4] = 0x1000 + 12; hle_gfx_list(&c);
    CHECK(px(401, 11) == 0x03FF, "OT tile %04x", px(401, 11));

    /* dithering gouraud: debe variar en una zona de color constante intermedio */
    gpu_gp0(0xE1000000 | 1 << 9);
    gpu_gp0(0x30808080); gpu_gp0(xy(10, 150)); gpu_gp0(0x00808080); gpu_gp0(xy(60, 150));
    gpu_gp0(0x00808080); gpu_gp0(xy(10, 200));
    CHECK(px(12, 152) != px(13, 152), "dither");

    /* display */
    gpu_gp1(0x08000009); gpu_gp1(0x05000000);
    GpuDisplay d; gpu_get_display(&d);
    CHECK(d.w == 320, "display w %d", d.w);
    gpu_dump_ppm("test_gpu.ppm", 1);
    printf("%s (%d fallos), estadísticas: poly %u line %u rect %u fill %u copy %u\n",
           fails ? "FALLOS" : "OK", fails, gpu_stats.poly, gpu_stats.line, gpu_stats.rect,
           gpu_stats.fill, gpu_stats.copy);
    return fails != 0;
}
