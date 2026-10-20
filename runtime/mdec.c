/* MDEC por software, escrito desde la documentación pública del hardware.
 * Comandos: 1 = decodificar macrobloques (RLE ya generado por la CPU), 2 = tablas de
 * cuantización, 3 = tabla de escala de la IDCT. Se decodifica en cuanto llega el último
 * parámetro del comando; la salida queda en una FIFO que se lee por registro o DMA1. */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "mdec.h"

/* Palabras de píxeles que el decodificador ha entregado desde el arranque (DMA1 o lectura por registro): un reproductor de vídeo activo las hace crecer. */
static _Atomic uint32_t mdec_words_out;
uint32_t mdec_out_words_total(void) { return atomic_load_explicit(&mdec_words_out, memory_order_relaxed); }

static inline int32_t rnd(float f) { return (int32_t)(f + (f >= 0 ? 0.5f : -0.5f)); }

static const uint8_t ZIGZAG[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };

static struct {
    uint8_t qy[64], qc[64];   /* tablas de cuantización (orden zigzag) */
    int16_t scale[64];        /* tabla de escala IDCT */
    float idct[64];           /* escala / 2^15 convertida */
    uint32_t cmd;             /* comando en curso */
    uint32_t *in; uint32_t in_n, in_cap, in_need;
    uint32_t *out; uint32_t out_n, out_cap, out_rd;
    uint32_t ctrl;
    int busy;
} M;

static void push_out(uint32_t w) {
    if (M.out_n == M.out_cap) {
        M.out_cap = M.out_cap ? M.out_cap * 2 : 4096;
        M.out = realloc(M.out, M.out_cap * 4);
    }
    M.out[M.out_n++] = w;
}

static void set_scale(const int16_t *t) {
    memcpy(M.scale, t, sizeof M.scale);
    for (int i = 0; i < 64; i++) M.idct[i] = (float)t[i] / 65536.0f; /* la tabla es 2x la ortonormal */
}

void mdec_set_default_tables(void) {
    static const uint8_t q[64] = { /* matriz intra MPEG-1 en orden zigzag */
        2, 16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27,
        27, 27, 26, 26, 26, 26, 27, 27, 27, 29, 29, 29, 34, 34, 34, 29,
        29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38, 37, 35, 35, 34,
        35, 38, 38, 40, 40, 40, 48, 48, 46, 46, 56, 56, 58, 69, 69, 83 };
    memcpy(M.qy, q, 64); memcpy(M.qc, q, 64);
    int16_t t[64];
    for (int u = 0; u < 8; u++)
        for (int x = 0; x < 8; x++) {
            /* formato del hardware: fila 0 = 0x5A82 (1/sqrt2), resto cos((2x+1)u*pi/16) en 1.15 */
            static const int16_t COS16[9] = { 32767, 32138, 30274, 27246, 23170, 18205, 12540, 6393, 0 };
            int k = ((2 * x + 1) * u) % 32, sg = 1;
            if (k > 16) k = 32 - k;
            if (k > 8) { k = 16 - k; sg = -1; }
            t[u * 8 + x] = (int16_t)(u == 0 ? 23170 : sg * COS16[k]);
        }
    set_scale(t);
}

void mdec_reset(void) {
    M.cmd = 0; M.in_n = M.in_need = 0; M.out_n = M.out_rd = 0; M.busy = 0; M.ctrl = 0;
}

/* IDCT separable 8x8: tabla[u][x] = C(u) cos(...) escalada 2^15 (fila u, columna x). */
static void idct(const int32_t *coef, int32_t *pix) {
    float tmp[64];
    for (int y = 0; y < 8; y++)       /* columnas: frecuencia vertical v -> fila y */
        for (int u = 0; u < 8; u++) {
            float s = 0;
            for (int v = 0; v < 8; v++) s += (float)coef[v * 8 + u] * M.idct[v * 8 + y];
            tmp[y * 8 + u] = s;
        }
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            float s = 0;
            for (int u = 0; u < 8; u++) s += tmp[y * 8 + u] * M.idct[u * 8 + x];
            pix[y * 8 + x] = (int32_t)rnd(s);
        }
}

/* Lee un bloque RLE (halfwords). Devuelve 0 si se agotaron los datos. */
static int rl_block(const uint16_t *h, uint32_t n, uint32_t *p, const uint8_t *qt, int32_t *pix) {
    int32_t coef[64] = { 0 };
    while (*p < n && h[*p] == 0xFE00) (*p)++; /* relleno entre bloques */
    if (*p >= n) return 0;
    uint16_t w = h[(*p)++];
    int q = w >> 10;
    int32_t dc = (int32_t)(int16_t)(w << 6) >> 6;
    int k = 0;
    int32_t val = dc * qt[0];
    for (;;) {
        if (q == 0) val = dc * 2;
        if (val < -1024) val = -1024;
        if (val > 1023) val = 1023;
        coef[ZIGZAG[k]] = val;
        if (*p >= n) break;
        w = h[(*p)++];
        if (w == 0xFE00) break;
        k += (w >> 10) + 1;
        if (k > 63) break;
        dc = (int32_t)(int16_t)(w << 6) >> 6;
        val = (dc * qt[k] * q + 4) / 8;
    }
    idct(coef, pix);
    return 1;
}

static inline int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

static void decode_macroblocks(void) {
    uint32_t depth = (M.cmd >> 27) & 3, sgn = (M.cmd >> 26) & 1, b15 = (M.cmd >> 25) & 1;
    const uint16_t *h = (const uint16_t *)M.in;
    uint32_t n = M.in_n * 2, p = 0;
    int32_t blk[6][64];
    if (depth <= 1) { /* monocromo: un bloque Y por vez */
        while (rl_block(h, n, &p, M.qy, blk[0])) {
            if (depth == 1) {
                for (int i = 0; i < 64; i += 4) {
                    uint32_t w = 0;
                    for (int j = 0; j < 4; j++) {
                        int32_t y = clampi(blk[0][i + j], -128, 127);
                        w |= (uint32_t)(uint8_t)(sgn ? y : y + 128) << (j * 8);
                    }
                    push_out(w);
                }
            } else {
                for (int i = 0; i < 64; i += 8) {
                    uint32_t w = 0;
                    for (int j = 0; j < 8; j++) {
                        int32_t y = clampi(blk[0][i + j], -128, 127);
                        w |= (uint32_t)(((uint8_t)(sgn ? y : y + 128)) >> 4) << (j * 4);
                    }
                    push_out(w);
                }
            }
        }
        return;
    }
    for (;;) { /* color: Cr, Cb, Y1..Y4 */
        if (!rl_block(h, n, &p, M.qc, blk[0])) return;
        if (!rl_block(h, n, &p, M.qc, blk[1])) return;
        for (int i = 2; i < 6; i++) if (!rl_block(h, n, &p, M.qy, blk[i])) return;
        uint8_t rgb[16 * 16 * 3];
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                int bi = 2 + (y >= 8) * 2 + (x >= 8);
                int32_t Y = blk[bi][(y & 7) * 8 + (x & 7)];
                int ci = (y >> 1) * 8 + (x >> 1);
                float cr = (float)blk[0][ci], cb = (float)blk[1][ci];
                int32_t r = clampi(Y + (int32_t)rnd(1.402f * cr), -128, 127);
                int32_t g = clampi(Y + (int32_t)rnd(-0.3437f * cb - 0.7143f * cr), -128, 127);
                int32_t b = clampi(Y + (int32_t)rnd(1.772f * cb), -128, 127);
                uint8_t *o = &rgb[(y * 16 + x) * 3];
                o[0] = (uint8_t)(sgn ? r : r + 128);
                o[1] = (uint8_t)(sgn ? g : g + 128);
                o[2] = (uint8_t)(sgn ? b : b + 128);
            }
        if (depth == 2) {
            for (int i = 0; i < 768; i += 4)
                push_out((uint32_t)rgb[i] | (uint32_t)rgb[i + 1] << 8 | (uint32_t)rgb[i + 2] << 16 | (uint32_t)rgb[i + 3] << 24);
        } else {
            for (int i = 0; i < 256; i += 2) {
                uint32_t w = 0;
                for (int j = 0; j < 2; j++) {
                    const uint8_t *c = &rgb[(i + j) * 3];
                    uint32_t px = (uint32_t)(c[0] >> 3) | (uint32_t)(c[1] >> 3) << 5 | (uint32_t)(c[2] >> 3) << 10 | b15 << 15;
                    w |= px << (j * 16);
                }
                push_out(w);
            }
        }
    }
}

static void finish_cmd(void) {
    uint32_t op = M.cmd >> 29;
    if (op == 1) {
        M.out_n = M.out_rd = 0;
        decode_macroblocks();
    } else if (op == 2) {
        const uint8_t *b = (const uint8_t *)M.in;
        memcpy(M.qy, b, 64);
        if (M.cmd & 1) memcpy(M.qc, b + 64, 64);
    } else if (op == 3) {
        set_scale((const int16_t *)M.in);
    }
    M.in_n = M.in_need = 0;
    M.cmd &= ~0xE0000000u; /* conserva bits de formato para el estado */
}

static void cmd_word(uint32_t w) {
    if (M.in_need == 0) {
        uint32_t op = w >> 29;
        M.cmd = w;
        M.in_n = 0;
        M.in_need = op == 1 ? (w & 0xFFFF) : op == 2 ? ((w & 1) ? 32 : 16) : op == 3 ? 32 : 0;
        if (M.in_need > M.in_cap) { M.in_cap = M.in_need; M.in = realloc(M.in, M.in_cap * 4); }
        if (!M.in_need && op == 1) finish_cmd();
        return;
    }
    M.in[M.in_n++] = w;
    if (M.in_n == M.in_need) finish_cmd();
}

void mdec_dma_in(const uint32_t *src, uint32_t words) {
    for (uint32_t i = 0; i < words; i++) cmd_word(src[i]);
}

uint32_t mdec_dma_out(uint32_t *dst, uint32_t words) {
    uint32_t i = 0;
    for (; i < words && M.out_rd < M.out_n; i++) dst[i] = M.out[M.out_rd++];
    for (uint32_t j = i; j < words; j++) dst[j] = 0;
    if (i) atomic_fetch_add_explicit(&mdec_words_out, i, memory_order_relaxed);
    return i;
}

static uint32_t status(void) {
    uint32_t s = 0;
    uint32_t left = M.out_n - M.out_rd;
    if (!left) s |= 1u << 31;
    if (M.in_need) s |= 1u << 29;                         /* ocupado mientras espera parámetros */
    if ((M.ctrl >> 30) & 1) s |= 1u << 28;                /* petición DMA de entrada */
    if (((M.ctrl >> 29) & 1) && left) s |= 1u << 27;      /* petición DMA de salida */
    s |= ((M.cmd >> 25) & 0xF) << 23;
    s |= (M.in_need ? (M.in_need - M.in_n - 1) : 0xFFFF) & 0xFFFF;
    return s;
}

uint32_t mdec_io_read(uint32_t addr, int size) {
    uint32_t v;
    if ((addr & 0xF) < 4) {
        if (M.out_rd < M.out_n) { v = M.out[M.out_rd++]; atomic_fetch_add_explicit(&mdec_words_out, 1u, memory_order_relaxed); } else v = 0;
    } else v = status();
    if (size == 2) v = (addr & 2) ? v >> 16 : v & 0xFFFF;
    return v;
}

void mdec_io_write(uint32_t addr, uint32_t v, int size) {
    (void)size;
    if ((addr & 0xF) < 4) { cmd_word(v); return; }
    if (v & 0x80000000u) { uint32_t c = M.ctrl; mdec_reset(); (void)c; }
    M.ctrl = v & 0x60000000u;
}
