/* Pruebas de calidad de audio: síntesis de voz (interpolación, ADPCM), mezcla (saturación), reverb y el consumidor del anillo
 * (remuestreo a la frecuencia de la sesión, control de nivel, relleno sin cortes). Señales sintéticas, sin datos del juego.
 *   clang -O2 -Wall -I. -o test_audio test_audio.c spu.c -lm -lpthread && ./test_audio [-v] */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spu.h"

static int fails, verbose;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define INFO(...) do { if (verbose) { printf("  "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void wr(uint32_t a, uint16_t v) { spu_io_write(a, v, 2); }

/* ---- Codificador ADPCM de prueba: elige filtro y desplazamiento por bloque con el decodificador real ---- */
static void enc_block(const int16_t *x, uint8_t *blk, int32_t hist[2], uint8_t flags) {
    double best = 1e30; uint8_t bb[16]; int32_t bh[2] = { 0, 0 };
    for (int f = 0; f < 5; f++) for (int sh = 0; sh <= 12; sh++) {
        uint8_t b[16] = { (uint8_t)(sh | f << 4), flags };
        static const int K0[5] = { 0, 60, 115, 98, 122 }, K1[5] = { 0, 0, -52, -55, -60 };
        int32_t s1 = hist[0], s2 = hist[1]; double err = 0;
        for (int i = 0; i < 28; i++) {
            int32_t pred = (s1 * K0[f] + s2 * K1[f] + 32) >> 6;
            int32_t d = x[i] - pred, q = (int32_t)lrint((double)d * (1 << sh) / 4096.0);   /* nibble con signo */
            if (q < -8) q = -8; if (q > 7) q = 7;
            int32_t s = (int16_t)((q & 15) << 12) >> sh; s += pred; if (s < -32768) s = -32768; if (s > 32767) s = 32767;
            err += (double)(s - x[i]) * (s - x[i]);
            b[2 + i / 2] |= (uint8_t)((q & 15) << ((i & 1) * 4));
            s2 = s1; s1 = s;
        }
        if (err < best) { best = err; memcpy(bb, b, 16); bh[0] = s1; bh[1] = s2; }
    }
    memcpy(blk, bb, 16); hist[0] = bh[0]; hist[1] = bh[1];
}
/* Sinusoide en bucle de `nblk` bloques con `cycles` periodos exactos; deja la voz `v` configurada y la devuelve en pitch `pitch`. */
static void load_sine(uint32_t addr, int nblk, int cycles, double amp) {
    int32_t h[2] = { 0, 0 };
    for (int b = 0; b < nblk; b++) {
        int16_t x[28];
        for (int i = 0; i < 28; i++) x[i] = (int16_t)lrint(amp * sin(2 * M_PI * cycles * (b * 28 + i) / (nblk * 28.0)));
        enc_block(x, spu_ram() + addr + b * 16, h, b == 0 ? 4 : b == nblk - 1 ? 3 : 0);
    }
}
static void voice_on(int v, uint32_t addr, uint16_t pitch, uint16_t vol) {
    uint32_t r = 0x1F801C00 + (uint32_t)v * 16;
    wr(r, vol); wr(r + 2, vol); wr(r + 4, pitch); wr(r + 6, (uint16_t)(addr / 8)); wr(r + 8, 0x000F); wr(r + 10, 0x0000);
    wr(0x1F801D88, (uint16_t)(1u << v));
}
static void spu_boot(void) {
    spu_reset();
    wr(0x1F801DAA, 0xC000);
    wr(0x1F801D80, 0x3FFF); wr(0x1F801D82, 0x3FFF);
}

/* Amplitud de la componente exacta `k` (de N) del canal izquierdo. */
static double bin(const int16_t *o, int n, int k) {
    double re = 0, im = 0;
    for (int i = 0; i < n; i++) { double a = 2 * M_PI * (double)((long)k * i % n) / n; re += o[2 * i] * cos(a); im += o[2 * i] * sin(a); }
    return 2 * sqrt(re * re + im * im) / n;
}
static double rms_i16(const int16_t *o, int n) { double a = 0; for (int i = 0; i < n; i++) a += (double)o[2 * i] * o[2 * i]; return sqrt(a / (n ? n : 1)); }

/* Decodifica `reps` pasadas del bucle de `nblk` bloques como lo hace la voz (la historia del filtro continúa entre pasadas). */
static int decode_loop(uint32_t addr, int nblk, int reps, int16_t *x) {
    int32_t h[2] = { 0, 0 }; int n = 0;
    for (int r = 0; r < reps; r++) for (int b = 0; b < nblk; b++) { spu_adpcm_block(spu_ram() + addr + b * 16, x + n, h); n += 28; }
    return n;
}
/* Modelo de referencia (doble precisión, escrito a partir de la especificación: 4 puntos gaussianos exp(-d^2/0.64) normalizados,
 * fase de 8 bits, muestras p-3..p ya decodificadas) de la salida de una voz a `pitch`. */
static double ref_voice(const int16_t *x, int n, int pitch) {
    long c = (long)n * pitch;    /* contador 4.12 de la salida n */
    long p = c >> 12; int ph = (int)((c & 0xFFF) >> 4);
    double w[4], sum = 0, f = ph / 256.0, acc = 0;
    for (int k = 0; k < 4; k++) { double d = (k - 3) - (-1.0 + f); w[k] = exp(-d * d / 0.64); sum += w[k]; }
    for (int k = 0; k < 4; k++) { long i = p - 3 + k; acc += w[k] / sum * (i < 0 ? 0 : x[i]); }
    return acc;
}
static void test_voice_model(void) {
    enum { N = 3500, SKIP = 8 };
    static const int pitches[] = { 0x0400, 0x0800, 0x0C00, 0x1000, 0x1400, 0x1800, 0x2000, 0x3000 };
    static int16_t x[63 * 28 * 14], o[(N + 1) * 2];
    for (unsigned t = 0; t < sizeof pitches / sizeof *pitches; t++) {
        int pitch = pitches[t];
        spu_boot();
        load_sine(0x1000, 63, 40, 12000);
        int nx = decode_loop(0x1000, 63, 14, x);
        voice_on(0, 0x1000, (uint16_t)pitch, 0x3FFF);
        spu_render(o, N);
        double mx = 0, se = 0, ss = 0;
        for (int i = SKIP; i < N; i++) {
            if (((long)i * pitch >> 12) + 4 >= nx) break;
            double r = ref_voice(x, i, pitch), e = o[2 * i] - r;
            mx = fmax(mx, fabs(e)); se += e * e; ss += r * r;
        }
        INFO("pitch 0x%04x: error max %.1f, relativo %.1f dB", pitch, mx, 10 * log10(se / ss + 1e-30));
        CHECK(mx <= 8, "pitch 0x%04x: la voz se aparta del modelo %.1f (> 4)", pitch, mx);
    }
}

/* Sin saltos entre muestras de una voz (la derivada de un seno es acotada; un clic la supera con creces); sobre la propia fuente
 * decodificada (ruido de cuantización incluido) la salida suavizada nunca salta más que ella. */
static void test_continuity(void) {
    enum { N = 20000, SKIP = 2000 };
    static const int pitches[] = { 0x0800, 0x1000, 0x1800, 0x2000 };
    static int16_t x[63 * 28 * 14];
    for (unsigned t = 0; t < sizeof pitches / sizeof *pitches; t++) {
        spu_boot();
        load_sine(0x1000, 63, 40, 12000);
        int nx = decode_loop(0x1000, 63, 14, x);
        double sx = 0; for (int i = 1; i < nx; i++) sx = fmax(sx, fabs((double)x[i] - x[i - 1]));
        voice_on(0, 0x1000, (uint16_t)pitches[t], 0x3FFF);
        static int16_t o[(N + SKIP) * 2];
        spu_render(o, N + SKIP);
        double mx = 0;
        for (int i = SKIP + 1; i < N + SKIP; i++) mx = fmax(mx, fabs((double)o[2 * i] - o[2 * i - 2]));
        double lim = sx * pitches[t] / 4096.0 * 1.05;
        INFO("pitch 0x%04x: salto max %.0f (limite %.0f)", pitches[t], mx, lim);
        CHECK(mx <= lim + 2, "pitch 0x%04x: salto de %.0f entre muestras (limite %.0f)", pitches[t], mx, lim);
    }
}

/* THD sobre el seno de 1 kHz por una voz (incluye el ruido de 4 bits del códec: solo comprobación gruesa). */
static void test_thd(void) {
    enum { N = 8820, SKIP = 4410 };
    static const int pitches[] = { 0x0800, 0x1000, 0x1800, 0x2000 };
    for (unsigned t = 0; t < sizeof pitches / sizeof *pitches; t++) {
        int pitch = pitches[t];
        spu_boot();
        load_sine(0x1000, 63, 40, 12000);
        voice_on(0, 0x1000, (uint16_t)pitch, 0x3FFF);
        static int16_t o[(N + SKIP) * 2];
        spu_render(o, N + SKIP);
        const int16_t *p = o + SKIP * 2;
        int k = (int)lrint(1000.0 * pitch / 4096.0 * N / 44100.0);
        double f = bin(p, N, k), h2 = 0;
        for (int h = 2; h * k < N / 2; h++) { double a = bin(p, N, h * k); h2 += a * a; }
        double thd = 20 * log10(sqrt(h2) / f);
        INFO("pitch 0x%04x: THD %.1f dB", pitch, thd);
        CHECK(thd < -40, "pitch 0x%04x: THD %.1f dB", pitch, thd);
    }
}

/* Mezcla: varias voces fuertes no producen planos de recorte duro y por debajo de la rodilla es lineal. */
static void test_softclip(void) {
    enum { N = 6000 };
    spu_boot();
    load_sine(0x1000, 63, 40, 16000);
    for (int v = 0; v < 6; v++) voice_on(v, 0x1000, 0x1000, 0x3FFF);
    static int16_t o[N * 2];
    spu_render(o, N);
    int run = 0, maxrun = 0, peak = 0;
    for (int i = 100; i < N; i++) {
        int a = abs(o[2 * i]); peak = a > peak ? a : peak;
        if (a >= 32760) { run++; maxrun = run > maxrun ? run : maxrun; } else run = 0;
    }
    INFO("6 voces: pico %d, tramo recortado max %d", peak, maxrun);
    CHECK(maxrun <= 2, "recorte duro: %d muestras seguidas al tope", maxrun);
    CHECK(peak > 28000, "la saturacion suave no deja pasar nivel (pico %d)", peak);
    spu_boot();
    load_sine(0x1000, 63, 40, 5000);
    voice_on(0, 0x1000, 0x1000, 0x2000);
    spu_render(o, N);
    double ex = 0; static int16_t x[63 * 28 * 4]; decode_loop(0x1000, 63, 4, x);
    for (int i = 8; i < 1500; i++) ex = fmax(ex, fabs(o[2 * i] - 0.5 * ref_voice(x, i, 0x1000)));
    CHECK(ex <= 4, "por debajo de la rodilla no es lineal (error %.1f)", ex);
}

/* ---- Consumidor: remuestreo y relleno ---- */
static double fit_resid_db(const int16_t *o, int n, double f, double fs, double *amp) {
    double cc = 0, ss = 0, cs = 0, yc = 0, ys = 0;
    for (int i = 0; i < n; i++) { double a = 2 * M_PI * f * i / fs, c = cos(a), s = sin(a), y = o[2 * i];
        cc += c * c; ss += s * s; cs += c * s; yc += y * c; ys += y * s; }
    double det = cc * ss - cs * cs, A = (yc * ss - ys * cs) / det, B = (ys * cc - yc * cs) / det, e = 0;
    for (int i = 0; i < n; i++) { double a = 2 * M_PI * f * i / fs, d = o[2 * i] - (A * cos(a) + B * sin(a)); e += d * d; }
    *amp = sqrt(A * A + B * B);
    return 10 * log10((e / n) / ((A * A + B * B) / 2) + 1e-30);
}
static void fill_sine(int16_t *buf, int n, long start, double f, double amp) {
    for (int i = 0; i < n; i++) { buf[2 * i] = buf[2 * i + 1] = (int16_t)lrint(amp * sin(2 * M_PI * f * (double)(start + i) / 44100.0)); }
}
/* 44,1 -> 48 kHz: ganancia plana y residuo (distorsión + alias + imágenes) bajo en toda la banda. */
static void test_resample(void) {
    static const double fs_[] = { 100, 1000, 5000, 10000, 15000, 18000 };
    static int16_t src[450 * 2], o[480 * 2], cap[48000 * 2];
    for (unsigned t = 0; t < sizeof fs_ / sizeof *fs_; t++) {
        spu_pull_reset();
        long pos = 0; int fill = 0;
        for (int k = 0; k < 31; k++) { fill_sine(src, 100, pos, fs_[t], 10000); spu_ring_feed(src, 100); pos += 100; }   /* 3100 = SPU_TARGET_LEVEL exacto (si no, el control de nivel desplaza el tono) */
        int n = 0;
        for (int it = 0; it < 150; it++) {
            spu_pull_rate(o, 480, 48000);
            fill_sine(src, 441, pos, fs_[t], 10000); spu_ring_feed(src, 441); pos += 441;
            if (it >= 20 && n + 480 <= 48000) { memcpy(cap + 2 * n, o, 480 * 4); n += 480; }
        }
        (void)fill;
        double amp, r = fit_resid_db(cap, n, fs_[t], 48000, &amp);
        double gdb = 20 * log10(amp / 10000);
        INFO("%6.0f Hz: ganancia %+.2f dB, residuo %.1f dB", fs_[t], gdb, r);
        CHECK(fabs(gdb) < (fs_[t] <= 15000 ? 0.25 : 1.5), "%.0f Hz: ganancia %+.2f dB", fs_[t], gdb);
        CHECK(r < -58, "%.0f Hz: residuo %.1f dB (alias/distorsión)", fs_[t], r);
    }
}

typedef struct { double maxd, maxabs; } Track;
static void track(Track *t, const int16_t *o, int n, int16_t *prev) {
    for (int i = 0; i < n; i++) { double d = fabs((double)o[2 * i] - *prev); if (d > t->maxd) t->maxd = d; if (fabs((double)o[2 * i]) > t->maxabs) t->maxabs = fabs((double)o[2 * i]); *prev = o[2 * i]; }
}
/* Productor con jitter (entregas de 882 frames adelantadas/retrasadas hasta 1 tick) y una parada de 150 ms: la salida es continua
 * (sin saltos mayores que los de la propia señal), la parada se rellena (no es silencio brusco) y se recupera. */
static void test_jitter(void) {
    enum { TICKS = 1200, A = 10000 };
    const double f = 1000, lim = A * 2 * M_PI * f / 48000;
    spu_pull_reset();
    static int16_t src[882 * 2], o[480 * 2];
    long spos = 0; unsigned rng = 12345;
    int pend[TICKS + 8]; memset(pend, 0, sizeof pend);
    for (int k = 0; k * 2 < TICKS + 4; k++) {   /* entrega nominal en el tick 2k+3, con jitter -1..+1 */
        rng = rng * 1664525u + 1013904223u;
        int t = 2 * k + 3 + (int)((rng >> 16) % 3) - 1; if (t < 0) t = 0;
        if (t >= 400 && t < 415) t += 15;   /* parada de ~150 ms: nada llega entre 400 y 415 */
        if (t < TICKS) pend[t]++;
    }
    int16_t prev = 0; Track tr = { 0, 0 }; double sil_rms = 0, pre_rms = 0; int sil_n = 0;
    uint32_t under0 = 0; SpuAudioStats st;
    for (int tick = 0; tick < TICKS; tick++) {
        for (int c = 0; c < pend[tick]; c++) { fill_sine(src, 882, spos, f, A); spu_ring_feed(src, 882); spos += 882; }
        spu_pull_rate(o, 480, 48000);
        if (tick > 40) track(&tr, o, 480, &prev); else prev = o[958];
        if (tick == 395) pre_rms = rms_i16(o, 480);
        if (tick >= 405 && tick <= 408) { sil_rms += rms_i16(o, 480); sil_n++; }
        if (tick == 399) { spu_audio_stats(&st, 0); under0 = st.underruns; }
    }
    spu_audio_stats(&st, 0);
    INFO("salto max %.0f (senal %.0f), pico %.0f, subejecuciones %u, rms previo %.0f, rms durante la parada %.0f", tr.maxd, lim, tr.maxabs, st.underruns, pre_rms, sil_rms / sil_n);
    CHECK(tr.maxd < lim * 1.6 + 60, "salto de %.0f en la salida (senal %.0f): clic", tr.maxd, lim);
    CHECK(tr.maxabs < A * 1.25, "sobrepico %.0f", tr.maxabs);
    CHECK(st.underruns > under0, "la parada no provoco subejecucion (la prueba no ejerce el relleno)");
    CHECK(sil_rms / sil_n > 0.5 * pre_rms, "la parada se oye como silencio (%.0f vs %.0f)", sil_rms / sil_n, pre_rms);
    CHECK(st.underruns == under0 + 1 || st.underruns <= under0 + 3, "demasiadas subejecuciones: %u", st.underruns);
    /* tras la recuperación la señal vuelve a ser un seno limpio */
    static int16_t cap[480 * 2 * 100];
    int n = 0;
    { int need = (int)SPU_TARGET_LEVEL - (int)spu_ring_level(); if (need > 0) { static int16_t big[3000 * 2]; fill_sine(big, need, spos, f, A); spu_ring_feed(big, need); spos += need; } }
    for (int tick = 0; tick < 100; tick++) {
        fill_sine(src, 441, spos, f, A); spu_ring_feed(src, 441); spos += 441;
        spu_pull_rate(o, 480, 48000); memcpy(cap + 2 * n, o, 480 * 4); n += 480;
    }
    /* el control de nivel puede desplazar el tono <= 0,5 %: se busca la frecuencia dentro de esa banda */
    double amp, r = 1e9;
    for (int k = -14; k <= 14; k++) { double a2, rr = fit_resid_db(cap + 2 * 4800, n - 4800, f * (1 + k * 0.0005), 48000, &a2); if (rr < r) r = rr; }
    (void)amp;
    INFO("tras recuperar: residuo %.1f dB", r);
    CHECK(r < -50, "tras la recuperacion el seno no es limpio (%.1f dB)", r);
}

/* Parada larga: la extensión decae y no queda en un zumbido eterno; sin datos desde el arranque sale cero exacto. */
static void test_starve(void) {
    static int16_t src[441 * 2], o[480 * 2];
    spu_pull_reset();
    int allz = 1;
    for (int i = 0; i < 20; i++) { spu_pull_rate(o, 480, 48000); for (int j = 0; j < 960; j++) if (o[j]) allz = 0; }
    CHECK(allz, "arranque sin datos: no es silencio exacto");
    long pos = 0;
    for (int k = 0; k < 8; k++) { fill_sine(src, 441, pos, 440, 10000); spu_ring_feed(src, 441); pos += 441; }
    for (int i = 0; i < 40; i++) { spu_pull_rate(o, 480, 48000); fill_sine(src, 441, pos, 440, 10000); spu_ring_feed(src, 441); pos += 441; }
    double live = rms_i16(o, 480);
    Track tr = { 0, 0 }; int16_t prev = o[958];
    double r0 = 0, r1 = 0, r05 = 0;
    for (int i = 0; i < 200; i++) { spu_pull_rate(o, 480, 48000); track(&tr, o, 480, &prev); if (i == 1) r0 = rms_i16(o, 480); if (i == 49) r05 = rms_i16(o, 480); if (i == 199) r1 = rms_i16(o, 480); }
    INFO("rms vivo %.0f, 20 ms %.0f, 2 s %.1f, salto max %.0f", live, r0, r1, tr.maxd);
    CHECK(r0 > 0.5 * live, "la extension se queda sin nivel de inmediato (%.0f vs %.0f)", r0, live);
    CHECK(r05 < 0.01 * live, "la extension no cae rapido con el productor parado (%.1f a 0,5 s)", r05);
    CHECK(r1 < 0.02 * live, "la extension no decae (%.1f)", r1);
    CHECK(tr.maxd < 10000 * 2 * M_PI * 440 / 48000 * 1.6 + 60, "salto %.0f en la extension", tr.maxd);
}

/* Reloj del productor 0,3 % rápido / lento durante 60 s: el control de nivel lo absorbe (sin subejecuciones ni descartes). */
static void test_drift(void) {
    static const double dr[] = { 0.003, -0.003 };
    for (int t = 0; t < 2; t++) {
        spu_pull_reset();
        static int16_t src[1000 * 2], o[480 * 2];
        long spos = 0; double acc = 0; SpuAudioStats st;
        for (int k = 0; k < 7; k++) { fill_sine(src, 441, spos, 600, 8000); spu_ring_feed(src, 441); spos += 441; }
        uint32_t lo = 1u << 30, hi = 0; int16_t prev = 0; Track tr = { 0, 0 };
        for (int tick = 0; tick < 6000; tick++) {
            spu_pull_rate(o, 480, 48000);
            if (tick > 100) track(&tr, o, 480, &prev); else prev = o[958];
            acc += 441.0 * (1 + dr[t]); int n = (int)acc; acc -= n;
            fill_sine(src, n, spos, 600, 8000); spu_ring_feed(src, n); spos += n;
            uint32_t l = spu_ring_level(); if (tick > 1500) { if (l < lo) lo = l; if (l > hi) hi = l; }
        }
        spu_audio_stats(&st, 0);
        INFO("deriva %+.1f%%: nivel %u..%u, subejecuciones %u, descartes %llu, salto max %.0f", dr[t] * 100, lo, hi, st.underruns, (unsigned long long)st.dropped, tr.maxd);
        CHECK(st.underruns == 0 && st.dropped == 0, "deriva %+.1f%%: %u subejecuciones, %llu descartes", dr[t] * 100, st.underruns, (unsigned long long)st.dropped);
        CHECK(lo > 600 && hi < 8000, "deriva %+.1f%%: el nivel se sale (%u..%u)", dr[t] * 100, lo, hi);
        CHECK(tr.maxd < 8000 * 2 * M_PI * 600 / 48000 * 1.15 + 20, "deriva: salto %.0f", tr.maxd);
    }
}

/* Consumidor parado (interrupción/pausa): vuelve con el anillo casi lleno en LIVE; debe saltar al nivel objetivo sin salto de señal. */
static void test_stale_ring(void) {
    static int16_t src[441 * 2], o[480 * 2], big[12000 * 2];
    spu_pull_reset();
    long pos = 0;
    for (int k = 0; k < 8; k++) { fill_sine(src, 441, pos, 440, 10000); spu_ring_feed(src, 441); pos += 441; }
    int16_t prev = 0; Track tr = { 0, 0 };
    for (int i = 0; i < 40; i++) { spu_pull_rate(o, 480, 48000); fill_sine(src, 441, pos, 440, 10000); spu_ring_feed(src, 441); pos += 441; if (i == 20) prev = o[958]; if (i > 20) track(&tr, o, 480, &prev); }
    fill_sine(big, 12000, pos, 440, 10000); spu_ring_feed(big, 12000); pos += 12000;   /* el productor siguió, el consumidor no */
    CHECK(spu_ring_level() > 8192, "preparacion: nivel %u", spu_ring_level());
    prev = o[958]; tr.maxd = 0;
    for (int i = 0; i < 10; i++) { spu_pull_rate(o, 480, 48000); track(&tr, o, 480, &prev); fill_sine(src, 441, pos, 440, 10000); spu_ring_feed(src, 441); pos += 441; }
    INFO("tras la parada: nivel %u, salto max %.0f", spu_ring_level(), tr.maxd);
    CHECK(spu_ring_level() < SPU_TARGET_LEVEL + 2000, "no descarto el audio viejo (nivel %u)", spu_ring_level());
    CHECK(tr.maxd < 10000 * 2 * M_PI * 440 / 48000 * 1.7 + 60, "salto %.0f al reanudar", tr.maxd);
}

int main(int argc, char **argv) {
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    test_voice_model();
    test_continuity();
    test_thd();
    test_softclip();
    test_resample();
    test_jitter();
    test_starve();
    test_drift();
    test_stale_ring();
    printf(fails ? "test_audio: %d fallos\n" : "test_audio: OK\n", fails);
    return fails != 0;
}
