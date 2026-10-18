/* Pruebas de audio/vídeo: ADPCM, ADSR, mezcla SPU por registros, XA y MDEC sintético.
 *   clang -O1 -Wall -I. -o test_av test_av.c spu.c mdec.c -lm -lpthread && ./test_av */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mdec.h"
#include "spu.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_adpcm(void) {
    /* filtro 0, shift 12: nibble n -> (n con signo) sin escalar */
    uint8_t b[16] = { 0x0C, 0 };
    for (int i = 0; i < 14; i++) b[2 + i] = (uint8_t)((i & 7) | (((i + 1) & 7) << 4));
    int16_t o[28]; int32_t h[2] = { 0, 0 };
    spu_adpcm_block(b, o, h);
    CHECK(o[0] == 0 && o[1] == 1 && o[2] == 1 && o[3] == 2, "shift12 %d %d %d %d", o[0], o[1], o[2], o[3]);
    /* nibble 8 = -8 con shift 0 -> -32768 */
    uint8_t n[16] = { 0x00, 0, 0x08 };
    h[0] = h[1] = 0; spu_adpcm_block(n, o, h);
    CHECK(o[0] == -32768 && o[1] == 0, "signo %d %d", o[0], o[1]);
    /* filtro 1 (60/64) con historia: muestra = s1*60/64 */
    uint8_t f[16] = { 0x1C, 0 };
    h[0] = 6400; h[1] = 0; spu_adpcm_block(f, o, h);
    CHECK(o[0] == 6000, "filtro1 %d", o[0]);
    CHECK(h[0] == o[27] && h[1] == o[26], "historia");
    /* filtro 2: s = 115/64 s1 - 52/64 s2 */
    uint8_t g[16] = { 0x2C, 0 };
    h[0] = 640; h[1] = 640; spu_adpcm_block(g, o, h);
    CHECK(o[0] == (640 * 115 - 640 * 52 + 32) / 64, "filtro2 %d", o[0]);
}

static void test_adsr(void) {
    SpuAdsr e = { 0 };
    /* ataque lineal rápido (shift 0, paso 7-0 -> +7<<11 por tick), decaimiento shift 0, sostenido 0xF */
    e.lo = 0x000F; e.hi = 0x0000;
    spu_adsr_keyon(&e);
    int t = 0;
    while (e.phase == 0 && t < 100) { spu_adsr_tick(&e); t++; }
    CHECK(e.phase == 1 && t == 3, "ataque t=%d fase=%d", t, e.phase);
    spu_adsr_tick(&e);
    CHECK(e.phase == 2, "sostenido 0x%x no alcanzado: fase %d nivel %d", e.lo & 0xF, e.phase, e.level);
    /* relajación lineal shift 0: -8<<11 por tick hasta 0 */
    e.hi = 0x0000;
    spu_adsr_keyoff(&e);
    t = 0;
    while (e.phase != 4 && t < 100) { spu_adsr_tick(&e); t++; }
    CHECK(e.phase == 4 && t >= 1 && t <= 2, "relajación t=%d", t);
    /* ataque lento: shift 20 -> 1 paso cada 512 ticks */
    e.lo = (20 << 10); e.hi = 0;
    spu_adsr_keyon(&e);
    for (int i = 0; i < 511; i++) spu_adsr_tick(&e);
    CHECK(e.level == 0, "ataque lento prematuro %d", e.level);
    spu_adsr_tick(&e);
    CHECK(e.level == 7, "ataque lento paso %d", e.level);
}

static void wr(uint32_t a, uint16_t v) { spu_io_write(a, v, 2); }

static void test_voice(void) {
    spu_reset();
    /* onda cuadrada en bucle: bloque con bandera inicio+repetición+fin (7), shift 0 */
    uint8_t blk[16] = { 0x00, 0x07 };
    for (int i = 0; i < 14; i++) blk[2 + i] = i < 7 ? 0x33 : 0xCC;
    wr(0x1F801DA6, 0x1000 / 8);
    for (int i = 0; i < 8; i++) wr(0x1F801DA8, (uint16_t)(blk[i * 2] | blk[i * 2 + 1] << 8));
    CHECK(spu_ram()[0x1000] == 0x00 && spu_ram()[0x1001] == 0x07, "FIFO manual");
    /* DMA de prueba */
    uint32_t w[4] = { 0x11223344, 0x55667788, 1, 2 };
    wr(0x1F801DA6, 0x2000 / 8);
    spu_dma_write(w, 4);
    CHECK(memcmp(spu_ram() + 0x2000, w, 16) == 0, "DMA escritura");
    wr(0x1F801DAA, 0xC000);
    wr(0x1F801D80, 0x3FFF); wr(0x1F801D82, 0x3FFF);
    wr(0x1F801C00, 0x3FFF); wr(0x1F801C02, 0x3FFF);
    wr(0x1F801C04, 0x1000);            /* 44100 Hz */
    wr(0x1F801C06, 0x1000 / 8);
    wr(0x1F801C08, 0x000F); wr(0x1F801C0A, 0x0000);
    wr(0x1F801DA4, 0x1000 / 8);        /* IRQ en el bloque */
    wr(0x1F801DAA, 0xC040);
    wr(0x1F801D88, 1);
    CHECK(spu_irq_take() == 1, "IRQ por dirección en key-on");
    CHECK(spu_io_read(0x1F801DAE, 2) & 0x40, "SPUSTAT IRQ");
    int16_t out[200 * 2];
    spu_render(out, 200);
    int peak = 0;
    for (int i = 0; i < 400; i++) if (abs(out[i]) > peak) peak = abs(out[i]);
    CHECK(peak > 4000, "voz sin salida (pico %d)", peak);
    CHECK(spu_io_read(0x1F801D9C, 2) & 1, "ENDX tras bucle");
    wr(0x1F801D8C, 1);
    spu_render(out, 50);
    CHECK(out[98] == 0 && out[99] == 0, "key-off no silencia %d", out[98]);
}

static void test_xa(void) {
    spu_reset();
    spu_io_write(0x1F801DAA, 0xC001, 2);
    spu_io_write(0x1F801D80, 0x3FFF, 2); spu_io_write(0x1F801D82, 0x3FFF, 2);
    spu_io_write(0x1F801DB0, 0x7FFF, 2); spu_io_write(0x1F801DB2, 0x7FFF, 2);
    static uint8_t sec[8 + 2304 + 24];
    memset(sec, 0, sizeof sec);
    sec[2] = 0x64; sec[3] = 0x01; /* estéreo, 37800 Hz, 4 bits */
    for (int g = 0; g < 18; g++) {
        uint8_t *grp = sec + 8 + g * 128;
        for (int u = 0; u < 8; u++) grp[4 + u] = 0x08; /* shift 8, filtro 0 */
        for (int j = 0; j < 28; j++) for (int u = 0; u < 4; u++) grp[16 + j * 4 + u] = 0x77; /* +7 */
    }
    spu_feed_xa(sec, sizeof sec);
    int16_t out[2000 * 2];
    spu_render(out, 2000);
    /* 7<<12>>8 = 112; mezcla x0x7FFF x0x7FFE */
    CHECK(out[1000] > 100 && out[1000] < 120 && out[1001] > 100, "XA %d %d", out[1000], out[1001]);
}

static void test_mdec(void) {
    mdec_reset();
    mdec_set_default_tables();
    /* subimos la tabla de escala generada por defecto también por comando 3 (idempotente) */
    /* macrobloque 15 bits: Cr=0, Cb=0, Y = DC uniforme */
    uint16_t h[64]; int n = 0;
    int dc_y = 64; /* q=0 en el DC -> valor = dc*2 = 128 -> Y = 128/8 = 16 */
    h[n++] = 0x0000; h[n++] = 0xFE00;          /* Cr: DC 0 */
    h[n++] = 0x0000; h[n++] = 0xFE00;          /* Cb */
    for (int i = 0; i < 4; i++) { h[n++] = (uint16_t)(dc_y & 0x3FF); h[n++] = 0xFE00; }
    uint32_t words = (uint32_t)(n + 1) / 2;
    uint32_t cmd = (1u << 29) | (3u << 27) | words; /* 15 bits, sin signo */
    mdec_io_write(0x1F801824, 0x60000000, 4);
    mdec_dma_in(&cmd, 1);
    mdec_dma_in((uint32_t *)h, words);
    uint32_t pix[128];
    uint32_t got = mdec_dma_out(pix, 128);
    CHECK(got == 128, "MDEC salida %u palabras", got);
    uint32_t p0 = pix[0] & 0x7FFF;
    int r = p0 & 31, g = (p0 >> 5) & 31, b = (p0 >> 10) & 31;
    /* 128 + 16 = 144 -> >>3 = 18 */
    CHECK(r == 18 && g == 18 && b == 18, "color %d %d %d", r, g, b);
    int uniform = 1;
    for (int i = 0; i < 128; i++) if (pix[i] != pix[0]) uniform = 0;
    CHECK(uniform, "bloque DC no uniforme");
    CHECK(mdec_io_read(0x1F801824, 4) & 0x80000000u, "FIFO de salida no vacía");
    /* 24 bits con croma: Cr DC positivo -> rojo > azul */
    h[0] = 0x0040;
    cmd = (1u << 29) | (2u << 27) | words;
    mdec_dma_in(&cmd, 1);
    mdec_dma_in((uint32_t *)h, words);
    uint32_t rgb[192];
    CHECK(mdec_dma_out(rgb, 192) == 192, "24 bits tamaño");
    uint8_t *c = (uint8_t *)rgb;
    CHECK(c[0] > c[2] + 10, "croma rojo %d azul %d", c[0], c[2]);
}

int main(void) {
    test_adpcm();
    test_adsr();
    test_voice();
    test_xa();
    test_mdec();
    printf(fails ? "test_av: %d fallos\n" : "test_av: OK\n", fails);
    return fails != 0;
}
