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

/* Energía RMS de un tramo estéreo intercalado. */
static double rms(const int16_t *o, int frames) {
    double a = 0;
    for (int i = 0; i < frames * 2; i++) a += (double)o[i] * o[i];
    return frames ? sqrt(a / (frames * 2)) : 0;
}
/* Voz de bucle con onda cuadrada en 0x1000 (bandera 7 = inicio+repetición+fin) por registros; hi = ADSR alto. */
static void setup_loop_voice(int v, uint16_t lo, uint16_t hi, uint8_t flags, uint32_t addr) {
    uint8_t blk[16] = { 0x00, flags };
    for (int i = 0; i < 14; i++) blk[2 + i] = i < 7 ? 0x33 : 0xCC;
    wr(0x1F801DA6, (uint16_t)(addr / 8));
    for (int i = 0; i < 8; i++) wr(0x1F801DA8, (uint16_t)(blk[i * 2] | blk[i * 2 + 1] << 8));
    uint32_t b = 0x1F801C00 + (uint32_t)v * 16;
    wr(b, 0x3FFF); wr(b + 2, 0x3FFF); wr(b + 4, 0x1000); wr(b + 6, (uint16_t)(addr / 8)); wr(b + 8, lo); wr(b + 10, hi);
}

/* Los campos de decaimiento (4 bits) y relajación (5 bits) son el desplazamiento del hardware tal cual, no x4. */
static void test_release_rates(void) {
    SpuAdsr e = { 0 };
    e.lo = 0x00FF; e.hi = 0x000E;      /* ataque instantáneo, decaimiento 15 -> sostenido en 0x8000 (máx), relajación lineal 14 */
    spu_adsr_keyon(&e);
    for (int i = 0; i < 8; i++) spu_adsr_tick(&e);
    CHECK(e.phase == 2 && e.level >= 0x7000, "sostenido previo fase %d nivel %d", e.phase, e.level);
    spu_adsr_keyoff(&e);
    int t = 0;
    while (e.phase != 4 && t < 60000) { spu_adsr_tick(&e); t++; }
    /* shift 14 lineal: 1<<3 = 8 ticks por paso de -8 -> 4096 pasos = 32768 ticks */
    CHECK(e.phase == 4 && t > 30000 && t < 35000, "relajación lineal shift 14: %d ticks (fase %d)", t, e.phase);
    /* exponencial shift 8: termina siempre (la aritmética de piso da al menos -1 por paso) */
    e.lo = 0x00FF; e.hi = 0x0020 | 8;
    spu_adsr_keyon(&e);
    for (int i = 0; i < 8; i++) spu_adsr_tick(&e);
    spu_adsr_keyoff(&e);
    t = 0;
    while (e.phase != 4 && t < 400000) { spu_adsr_tick(&e); t++; }
    CHECK(e.phase == 4 && t > 1000 && t < 100000, "relajación exponencial shift 8: %d ticks", t);
    /* decaimiento 4 (exponencial, paso -8 << 7): tau ~32 ticks, de 0x7FFF a 0x800 en ~90 ticks (antes del arreglo: >100 mil) */
    e.lo = 0x0040; e.hi = 0x0000;      /* ataque 0, decaimiento 4, sostenido nivel 0 (0x800) */
    spu_adsr_keyon(&e);
    t = 0;
    while (e.phase < 2 && t < 100000) { spu_adsr_tick(&e); t++; }
    CHECK(e.phase == 2 && t > 60 && t < 150, "decaimiento shift 4: %d ticks", t);
}

/* KOFF por registros: una voz en bucle (como un búfer de transmisión que el juego deja de rellenar) debe ir a silencio. */
static void test_koff_silence(void) {
    spu_reset();
    wr(0x1F801DAA, 0xC000); wr(0x1F801D80, 0x3FFF); wr(0x1F801D82, 0x3FFF);
    setup_loop_voice(5, 0x000F, 0x000C, 7, 0x1000);   /* sostenido, relajación lineal shift 12 (~8 mil ticks) */
    wr(0x1F801D88, 1u << 5);
    static int16_t out[2048 * 2];
    spu_render(out, 2048);
    CHECK(rms(out, 2048) > 1000, "voz en bucle sin sonido antes de KOFF");
    wr(0x1F801D8C, 1u << 5);
    int ok = 0;
    for (int k = 0; k < 40 && !ok; k++) { spu_render(out, 2048); ok = rms(out, 2048) == 0; }
    CHECK(ok, "la voz sigue sonando mucho después de KOFF (rms %.0f)", rms(out, 2048));
    CHECK(spu_io_read(0x1F801C0C, 2) == 0, "ADSR actual no llegó a 0 tras la relajación");
    /* KOFF de las voces 16-23 (segundo registro) */
    setup_loop_voice(20, 0x000F, 0x000C, 7, 0x1100);
    wr(0x1F801D8A, 1u << 4);
    spu_render(out, 2048);
    CHECK(rms(out, 2048) > 1000, "voz 20 sin sonido");
    wr(0x1F801D8E, 1u << 4);
    ok = 0;
    for (int k = 0; k < 40 && !ok; k++) { spu_render(out, 2048); ok = rms(out, 2048) == 0; }
    CHECK(ok, "voz 20 sigue sonando tras KOFF alto");
    /* SPUCNT con bit 15 a 0 corta todo */
    wr(0x1F801D88, 1u << 5);
    spu_render(out, 512);
    CHECK(rms(out, 512) > 1000, "voz renovada sin sonido");
    wr(0x1F801DAA, 0x4000);
    spu_render(out, 512);
    CHECK(rms(out, 512) == 0, "SPU apagada sigue sonando");
    wr(0x1F801DAA, 0xC000);
    spu_render(out, 512);
    CHECK(rms(out, 512) == 0, "las voces no deben revivir al reactivar la SPU");
}

/* Bandera de fin sin repetición (bit 0 sin bit 1): ENDX y silencio, sin repetir el bloque. */
static void test_end_no_repeat(void) {
    spu_reset();
    wr(0x1F801DAA, 0xC000); wr(0x1F801D80, 0x3FFF); wr(0x1F801D82, 0x3FFF);
    setup_loop_voice(2, 0x000F, 0x0000, 1, 0x1200);   /* fin sin repetición, relajación irrelevante */
    wr(0x1F801D88, 1u << 2);
    static int16_t out[512 * 2];
    spu_render(out, 10);
    CHECK(rms(out, 10) > 1000 && !(spu_io_read(0x1F801D9C, 2) & 4), "voz de un solo disparo: sonido antes del fin / ENDX prematuro");
    spu_render(out, 512);                              /* 28 muestras a 44100 Hz: el bloque ya terminó */
    CHECK(spu_io_read(0x1F801D9C, 2) & 4, "ENDX no se marcó al terminar el bloque de fin");
    spu_render(out, 512);
    CHECK(rms(out, 512) == 0, "el bloque de fin sin repetición se repite (rms %.0f)", rms(out, 512));
    wr(0x1F801D88, 1u << 2);                           /* nuevo key-on limpia ENDX */
    CHECK(!(spu_io_read(0x1F801D9C, 2) & 4), "key-on no limpia ENDX");
}

/* Reverb: sin entrada nueva la cola decae a casi cero; sin voces con reverb el búfer no se realimenta. */
static void test_reverb_tail(void) {
    spu_reset();
    wr(0x1F801DAA, 0xC080);                            /* SPU activa + escritura del reverb */
    wr(0x1F801D80, 0x3FFF); wr(0x1F801D82, 0x3FFF);
    wr(0x1F801D84, 0x3000); wr(0x1F801D86, 0x3000);    /* volumen de salida del reverb */
    wr(0x1F801DA2, 0xE000 / 8);                        /* búfer de 8 KB..., tamaño hasta el final de la RAM */
    /* Configuración propia, estable (todas las ganancias < 1): retardos de apf, iir, 4 combs, pared, apf, 20 direcciones, entradas. */
    static const uint16_t rv[32] = { 0x0020, 0x0030, 0x4000, 0x3000, 0x2000, 0x1000, 0x0000, 0x2000, 0x2000, 0x2000,
        0x0400, 0x0500, 0x0300, 0x0380, 0x0600, 0x0680, 0x0410, 0x0510, 0x0420, 0x0520, 0x0700, 0x0780, 0x0800, 0x0880,
        0x0430, 0x0530, 0x0440, 0x0540, 0x0450, 0x0550, 0x7FFF, 0x7FFF };
    for (int i = 0; i < 32; i++) wr(0x1F801DC0 + (uint32_t)i * 2, rv[i]);
    setup_loop_voice(3, 0x000F, 0x000C, 7, 0x1000);
    wr(0x1F801D98, 1u << 3);                           /* voz 3 con reverb */
    wr(0x1F801D88, 1u << 3);
    static int16_t out[44100 * 2];
    spu_render(out, 8192);
    wr(0x1F801D8C, 1u << 3);
    double w[40];
    for (int k = 0; k < 40; k++) { spu_render(out, 4096); w[k] = rms(out, 4096); }
    double early = 0, late = w[39];
    for (int k = 0; k < 40; k++) if (w[k] > early) early = w[k];
    CHECK(early > 1000 && late < 3.0, "cola de reverb no decae: temprano %.1f, tardío %.1f", early, late);
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
    test_release_rates();
    test_koff_silence();
    test_end_no_repeat();
    test_reverb_tail();
    test_xa();
    test_mdec();
    printf(fails ? "test_av: %d fallos\n" : "test_av: OK\n", fails);
    return fails != 0;
}
