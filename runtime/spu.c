/* SPU de original emulada a nivel de registros, escrita a partir de la documentación pública del
 * hardware. 24 voces ADPCM (envolvente ADSR, pitch, modulación por pitch, ruido), reverb a
 * 22050 Hz, IRQ por dirección, transferencias manuales y DMA canal 4, captura en RAM de
 * sonido y entrada CD-XA.
 * Hilos: el estado de la SPU solo lo toca el hilo de emulación (bajo un mutex que protege además las pruebas que
 * llaman a spu_render); el hilo de audio del sistema solo usa spu_pull, que no toma ningún candado (anillo SPSC). */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include "spu.h"

#define NV 24
#define XA_Q (44100 * 2)   /* 2 s de cola estéreo (en frames) */

typedef struct {
    uint16_t pitch, start;
    int16_t cur_vol[2];      /* volumen real tras barrido */
    int32_t sweep_cnt[2];
    uint32_t addr;           /* bloque actual (bytes) */
    uint32_t rep_addr;       /* dirección de repetición (bytes) */
    uint32_t counter;        /* posición 4.12 dentro del bloque */
    int16_t buf[28];         /* bloque decodificado */
    int16_t prev[4];         /* cola del bloque anterior (interpolación) */
    int32_t hist[2];
    uint8_t blk_flags;
    int ignore_loop;         /* el juego fijó la repetición a mano */
    SpuAdsr env;
    int32_t out;             /* última salida (modulación de pitch) */
} Voice;

static pthread_mutex_t MX = PTHREAD_MUTEX_INITIALIZER;
static uint32_t xa_pos;   /* fracción 16.16 arrastrada entre sectores XA */
static int16_t xa_last[2];

static struct {
    uint8_t ram[SPU_RAM_SIZE];
    uint16_t regs[0x200];    /* copia plana de 0x1F801C00..0x1F801FFF */
    Voice v[NV];
    uint32_t endx;
    uint32_t xfer_addr;      /* puntero interno de transferencia (bytes) */
    int irq_pending;
    int32_t noise_timer;
    uint16_t noise_lfsr;
    uint32_t rv_cur;         /* posición actual del búfer de reverb (bytes) */
    int rv_odd;
    int32_t rv_in[4], rv_out[2], rv_prev_out[2];
    int16_t main_cur[2];
    int32_t main_cnt[2];
    uint32_t cap_pos;
    /* CD-XA */
    int16_t xa_q[XA_Q][2];
    uint32_t xa_rd, xa_wr;
    int32_t xa_hist[2][2];
    /* WAV */
    FILE *wav;
    uint32_t wav_frames;
} S;

void (*spu_irq_hook)(void);
/* Estado de arranque: las 24 voces apagadas (el hardware no reproduce nada hasta el primer key-on). Sin esto, si nadie llama a
 * spu_reset, todas las voces empiezan en fase de ataque sobre la dirección 0. */
__attribute__((constructor)) static void spu_boot_voices(void) { for (int i = 0; i < NV; i++) S.v[i].env.phase = 4; }
uint8_t *spu_ram(void) { return S.ram; }

#define RG(off) S.regs[((off) - 0x1C00) / 2]
#define CNT RG(0x1DAA)

/* Saturación suave de la mezcla: lineal hasta SOFT_KNEE (28000: las mezclas normales no se tocan) y después curva racional con pendiente continua que tiende a +-32767
 * (sin recorte duro al sumar 24 voces + reverb). */
#define SOFT_KNEE 28000
static inline int32_t soft16(int32_t x) {
    int32_t a = x < 0 ? -x : x;
    if (a > 1 << 20) a = 1 << 20;
    if (a > SOFT_KNEE) { int32_t r = 32767 - SOFT_KNEE, u = a - SOFT_KNEE; a = SOFT_KNEE + (int32_t)((int64_t)u * r / (r + u)); }
    return x < 0 ? -a : a;
}
static inline int32_t clamp16(int32_t x) { return x < -0x8000 ? -0x8000 : x > 0x7FFF ? 0x7FFF : x; }

/* ---------------- IRQ por dirección ---------------- */
static void irq_check(uint32_t a, uint32_t len) {
    if (!(CNT & 0x40) || (RG(0x1DAE) & 0x40)) return;
    uint32_t ia = (uint32_t)RG(0x1DA4) * 8;
    if (ia - a < len) {
        RG(0x1DAE) |= 0x40;
        S.irq_pending = 1;
        if (spu_irq_hook) spu_irq_hook();
    }
}

int spu_irq_take(void) {
    pthread_mutex_lock(&MX);
    int r = S.irq_pending;
    S.irq_pending = 0;
    pthread_mutex_unlock(&MX);
    return r;
}

/* ---------------- ADPCM ---------------- */
static const int8_t K_POS[5] = { 0, 60, 115, 98, 122 };
static const int8_t K_NEG[5] = { 0, 0, -52, -55, -60 };

void spu_adpcm_block(const uint8_t *blk, int16_t out[28], int32_t hist[2]) {
    int shift = blk[0] & 0xF, filt = (blk[0] >> 4) & 7;
    if (shift > 12) shift = 9;   /* valores reservados */
    if (filt > 4) filt = 4;
    int32_t s1 = hist[0], s2 = hist[1];
    for (int i = 0; i < 28; i++) {
        int nib = (blk[2 + i / 2] >> ((i & 1) * 4)) & 0xF;
        int32_t s = (int16_t)(nib << 12) >> shift;
        s = clamp16(s + ((s1 * K_POS[filt] + s2 * K_NEG[filt] + 32) >> 6));
        out[i] = (int16_t)s;
        s2 = s1; s1 = s;
    }
    hist[0] = s1; hist[1] = s2;
}

/* ---------------- Envolventes ---------------- */
/* Paso común de ADSR y barridos: shift/step según el formato del hardware. */
static int32_t env_step(int32_t level, int32_t *cnt, int shift, int step, int exp, int dec) {
    int32_t cycles = 1 << (shift > 11 ? shift - 11 : 0);
    int32_t st = step * (1 << (shift < 11 ? 11 - shift : 0));
    if (exp && !dec && level > 0x6000) cycles *= 4;
    if (exp && dec) st = (st * level) >> 15;
    if (++*cnt < cycles) return level;
    *cnt = 0;
    level += st;
    return level < 0 ? 0 : level > 0x7FFF ? 0x7FFF : level;
}

void spu_adsr_keyon(SpuAdsr *e) { e->phase = 0; e->level = 0; e->counter = 0; }
void spu_adsr_keyoff(SpuAdsr *e) { if (e->phase != 4) { e->phase = 3; e->counter = 0; } }

void spu_adsr_tick(SpuAdsr *e) {
    uint16_t lo = e->lo, hi = e->hi;
    switch (e->phase) {
    case 0: /* ataque */
        e->level = env_step(e->level, &e->counter, (lo >> 10) & 0x1F, 7 - ((lo >> 8) & 3), lo >> 15, 0);
        if (e->level >= 0x7FFF) { e->phase = 1; e->counter = 0; }
        break;
    case 1: { /* decaimiento exponencial hasta el nivel de sostenido (shift = campo de 4 bits tal cual, paso -8) */
        int32_t sus = ((lo & 0xF) + 1) * 0x800;
        if (sus > 0x7FFF) sus = 0x7FFF;
        e->level = env_step(e->level, &e->counter, (lo >> 4) & 0xF, -8, 1, 1);
        if (e->level <= sus) { e->phase = 2; e->counter = 0; }
        break;
    }
    case 2: { /* sostenido hasta key-off */
        int dec = (hi >> 14) & 1, st = (hi >> 6) & 3;
        e->level = env_step(e->level, &e->counter, (hi >> 8) & 0x1F, dec ? -8 + st : 7 - st, hi >> 15, dec);
        break;
    }
    case 3: /* relajación: shift = campo de 5 bits tal cual (NO multiplicado por 4), paso -8; bit 5 = exponencial */
        e->level = env_step(e->level, &e->counter, hi & 0x1F, -8, (hi >> 5) & 1, 1);
        if (e->level < 8) { e->level = 0; e->phase = 4; }
        break;
    default:
        e->level = 0;
    }
}

/* Volumen: bit15=0 fijo (bits 14-0 = volumen/2 con signo); bit15=1 barrido. */
static int32_t sweep_vol(uint16_t reg, int16_t *cur, int32_t *cnt) {
    if (!(reg & 0x8000)) { *cur = (int16_t)(reg << 1); return *cur; }
    int exp = (reg >> 14) & 1, dec = (reg >> 13) & 1, neg = (reg >> 12) & 1;
    int shift = (reg >> 2) & 0x1F, st = reg & 3;
    int32_t lvl = *cur < 0 ? -*cur : *cur;
    lvl = env_step(lvl, cnt, shift, dec ? -8 + st : 7 - st, exp, dec);
    *cur = (int16_t)(neg ? -lvl : lvl);
    return *cur;
}

/* ---------------- Voces ---------------- */
static void voice_decode(Voice *v) {
    uint32_t a = v->addr & (SPU_RAM_SIZE - 1);
    irq_check(a, 16);
    const uint8_t *b = &S.ram[a];
    for (int i = 0; i < 4; i++) v->prev[i] = v->buf[24 + i];
    spu_adpcm_block(b, v->buf, v->hist);
    v->blk_flags = b[1];
    if ((b[1] & 4) && !v->ignore_loop) v->rep_addr = a;
}

static void key_on(int i) {
    Voice *v = &S.v[i];
    uint16_t *r = &S.regs[i * 8];
    v->start = r[3];
    v->addr = (uint32_t)v->start * 8;
    v->ignore_loop = 0;
    v->counter = 0;
    v->hist[0] = v->hist[1] = 0;
    memset(v->buf, 0, sizeof v->buf);
    v->env.lo = r[4]; v->env.hi = r[5];
    spu_adsr_keyon(&v->env);
    S.endx &= ~(1u << i);
    voice_decode(v);
}

static inline int16_t vs(Voice *v, int idx) { return idx < 0 ? v->prev[4 + idx] : v->buf[idx]; }

/* Núcleo de interpolación gaussiano: 256 fases x 4 puntos, cada fila suma exactamente 0x8000 (ganancia 1 en continua).
 * Pesos exp(-d^2/(2*0.32)) con d = distancia de cada punto a la posición; en fase 0 da ~(0,147;0,70;0,147;0) como el chip real,
 * que suaviza (quita el aliasing chirriante de los tonos agudos). Se calcula una vez al arrancar. */
static int32_t gauss_tab[256][4];
__attribute__((constructor)) static void gauss_init(void) {
    for (int ph = 0; ph < 256; ph++) {
        double f = ph / 256.0, w[4], sum = 0;
        /* puntos en p-3, p-2, p-1, p; la posición interpolada está entre p-2 y p-1 desplazada f hacia p-1: x = -1 + f respecto a p */
        for (int k = 0; k < 4; k++) { double d = (k - 3) - (-1.0 + f); w[k] = exp(-d * d / 0.64); sum += w[k]; }
        int32_t tot = 0;
        for (int k = 0; k < 4; k++) { gauss_tab[ph][k] = (int32_t)lrint(w[k] / sum * 32768.0); tot += gauss_tab[ph][k]; }
        gauss_tab[ph][1] += 0x8000 - tot;
    }
}

static int32_t voice_sample(int i, int16_t noise, uint32_t noise_mask, uint32_t pmon) {
    Voice *v = &S.v[i];
    uint16_t *r = &S.regs[i * 8];
    v->env.lo = r[4]; v->env.hi = r[5];
    int32_t s;
    if (noise_mask & (1u << i)) {
        s = noise;
    } else { /* interpolación gaussiana de 4 puntos sobre muestras YA decodificadas (p-3..p): nunca mira más allá del bloque */
        int p = v->counter >> 12;
        int ph = (int)((v->counter & 0xFFF) >> 4);   /* 0..255 */
        const int32_t *g = gauss_tab[ph];
        int64_t acc = (int64_t)g[0] * vs(v, p - 3) + (int64_t)g[1] * vs(v, p - 2) + (int64_t)g[2] * vs(v, p - 1) + (int64_t)g[3] * vs(v, p);
        s = clamp16((int32_t)((acc + 0x4000) >> 15));
    }
    spu_adsr_tick(&v->env);
    int32_t out = (s * v->env.level) >> 15;

    uint32_t step = r[2];
    if (i > 0 && (pmon & (1u << i))) {
        int32_t fac = S.v[i - 1].out + 0x8000;
        step = ((uint32_t)((int32_t)(int16_t)step * fac) >> 15) & 0xFFFF;
    }
    if (step > 0x4000) step = 0x4000;
    v->counter += step;
    while (v->counter >= 28u << 12) {
        v->counter -= 28u << 12;
        if (v->blk_flags & 1) { /* fin de bloque con bandera de final */
            S.endx |= 1u << i;
            v->addr = v->rep_addr;
            if (!(v->blk_flags & 2)) { v->env.phase = 4; v->env.level = 0; }
        } else {
            v->addr = (v->addr + 16) & (SPU_RAM_SIZE - 1);
        }
        voice_decode(v);
    }
    v->out = out;
    return out;
}

/* ---------------- Ruido ---------------- */
static int16_t noise_tick(void) {
    int shift = (CNT >> 10) & 0xF, st = ((CNT >> 8) & 3) + 4;
    S.noise_timer -= st;
    if (S.noise_timer < 0) {
        uint16_t l = S.noise_lfsr;
        int fb = ((l >> 15) ^ (l >> 12) ^ (l >> 11) ^ (l >> 10) ^ 1) & 1;
        S.noise_lfsr = (uint16_t)((l << 1) | fb);
        S.noise_timer += 0x20000 >> shift;
        if (S.noise_timer < 0) S.noise_timer += 0x20000 >> shift;
    }
    return (int16_t)S.noise_lfsr;
}

/* ---------------- Reverb ---------------- */
enum { dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL, vAPF1, vAPF2,
       mLSAME, mRSAME, mLCOMB1, mRCOMB1, mLCOMB2, mRCOMB2, dLSAME, dRSAME, mLDIFF, mRDIFF,
       mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4, dLDIFF, dRDIFF, mLAPF1, mRAPF1, mLAPF2, mRAPF2, vLIN, vRIN };
#define RV(n) S.regs[(0x1DC0 - 0x1C00) / 2 + (n)]
#define VV(n) ((int32_t)(int16_t)RV(n))
#define M(a, b) (((a) * (b)) >> 15)

static uint32_t rv_wrap(int32_t off_bytes) {
    int32_t base = (int32_t)RG(0x1DA2) * 8;
    int32_t size = (int32_t)SPU_RAM_SIZE - base;
    int32_t a = ((int32_t)S.rv_cur - base + off_bytes) % size;
    if (a < 0) a += size;
    return (uint32_t)(base + a) & ~1u;
}
static int32_t rvr(int32_t off) { int16_t v; memcpy(&v, &S.ram[rv_wrap(off)], 2); return v; }
static void rvw(int32_t off, int32_t val) {
    if (!(CNT & 0x80)) return;
    uint32_t a = rv_wrap(off);
    int16_t v = (int16_t)clamp16(val);
    irq_check(a, 2);
    memcpy(&S.ram[a], &v, 2);
}
#define A(n) ((int32_t)RV(n) * 8)

static void reverb_step(int32_t inl, int32_t inr) {
    if (RG(0x1DA2) == 0 || (uint32_t)RG(0x1DA2) * 8 >= SPU_RAM_SIZE) return;
    int32_t lin = M(VV(vLIN), inl), rin = M(VV(vRIN), inr);
    int32_t iir = VV(vIIR), wall = VV(vWALL);
    rvw(A(mLSAME), M(lin + M(rvr(A(dLSAME)), wall) - rvr(A(mLSAME) - 2), iir) + rvr(A(mLSAME) - 2));
    rvw(A(mRSAME), M(rin + M(rvr(A(dRSAME)), wall) - rvr(A(mRSAME) - 2), iir) + rvr(A(mRSAME) - 2));
    rvw(A(mLDIFF), M(lin + M(rvr(A(dRDIFF)), wall) - rvr(A(mLDIFF) - 2), iir) + rvr(A(mLDIFF) - 2));
    rvw(A(mRDIFF), M(rin + M(rvr(A(dLDIFF)), wall) - rvr(A(mRDIFF) - 2), iir) + rvr(A(mRDIFF) - 2));
    int32_t lo = M(VV(vCOMB1), rvr(A(mLCOMB1))) + M(VV(vCOMB2), rvr(A(mLCOMB2))) +
                 M(VV(vCOMB3), rvr(A(mLCOMB3))) + M(VV(vCOMB4), rvr(A(mLCOMB4)));
    int32_t ro = M(VV(vCOMB1), rvr(A(mRCOMB1))) + M(VV(vCOMB2), rvr(A(mRCOMB2))) +
                 M(VV(vCOMB3), rvr(A(mRCOMB3))) + M(VV(vCOMB4), rvr(A(mRCOMB4)));
    int32_t t;
    t = rvr(A(mLAPF1) - A(dAPF1)); lo -= M(VV(vAPF1), t); rvw(A(mLAPF1), lo); lo = M(lo, VV(vAPF1)) + t;
    t = rvr(A(mRAPF1) - A(dAPF1)); ro -= M(VV(vAPF1), t); rvw(A(mRAPF1), ro); ro = M(ro, VV(vAPF1)) + t;
    t = rvr(A(mLAPF2) - A(dAPF2)); lo -= M(VV(vAPF2), t); rvw(A(mLAPF2), lo); lo = M(lo, VV(vAPF2)) + t;
    t = rvr(A(mRAPF2) - A(dAPF2)); ro -= M(VV(vAPF2), t); rvw(A(mRAPF2), ro); ro = M(ro, VV(vAPF2)) + t;
    S.rv_out[0] = clamp16(M(clamp16(lo), (int16_t)RG(0x1D84)));
    S.rv_out[1] = clamp16(M(clamp16(ro), (int16_t)RG(0x1D86)));
    uint32_t base = (uint32_t)RG(0x1DA2) * 8;
    S.rv_cur += 2;
    if (S.rv_cur >= SPU_RAM_SIZE || S.rv_cur < base) S.rv_cur = base;
}

/* ---------------- Mezcla ---------------- */
static void capture(uint32_t base, int16_t s) {
    uint32_t a = base + S.cap_pos * 2;
    memcpy(&S.ram[a], &s, 2);
    irq_check(a, 2);
}

static void render_one(int16_t *o) {
    int16_t noise = noise_tick();
    uint32_t noise_mask = RG(0x1D94) | ((uint32_t)RG(0x1D96) << 16);
    uint32_t pmon = RG(0x1D90) | ((uint32_t)RG(0x1D92) << 16);
    uint32_t rvon = RG(0x1D98) | ((uint32_t)RG(0x1D9A) << 16);
    int32_t dl = 0, dr = 0, rl = 0, rr = 0;
    for (int i = 0; i < NV; i++) {
        Voice *v = &S.v[i];
        if (v->env.phase == 4) {
            v->out = 0;
            uint16_t *r = &S.regs[i * 8];
            sweep_vol(r[0], &v->cur_vol[0], &v->sweep_cnt[0]);
            sweep_vol(r[1], &v->cur_vol[1], &v->sweep_cnt[1]);
            continue;
        }
        int32_t s = voice_sample(i, noise, noise_mask, pmon);
        uint16_t *r = &S.regs[i * 8];
        int32_t l = M(s, sweep_vol(r[0], &v->cur_vol[0], &v->sweep_cnt[0]));
        int32_t rt = M(s, sweep_vol(r[1], &v->cur_vol[1], &v->sweep_cnt[1]));
        dl += l; dr += rt;
        if (rvon & (1u << i)) { rl += l; rr += rt; }
        if (i == 1) capture(0x800, (int16_t)clamp16(s));
        if (i == 3) capture(0xC00, (int16_t)clamp16(s));
    }
    /* CD (XA) */
    int16_t cdl = 0, cdr = 0;
    if (S.xa_rd != S.xa_wr) {
        cdl = S.xa_q[S.xa_rd % XA_Q][0];
        cdr = S.xa_q[S.xa_rd % XA_Q][1];
        S.xa_rd++;
    }
    capture(0x000, cdl);
    capture(0x400, cdr);
    S.cap_pos = (S.cap_pos + 1) & 0x1FF;
    if (CNT & 1) {
        int32_t l = M((int32_t)cdl, (int16_t)RG(0x1DB0)), r = M((int32_t)cdr, (int16_t)RG(0x1DB2));
        dl += l; dr += r;
        if (CNT & 4) { rl += l; rr += r; }
    }
    /* reverb a 22050 Hz: se acumula la entrada de dos muestras */
    rl = soft16(rl); rr = soft16(rr);
    if (S.rv_odd) {   /* entrada: paso bajo [1 2 1]/4 antes de decimar (sin aliasing); salida: interpolación lineal (sin imágenes de sostén) */
        S.rv_prev_out[0] = S.rv_out[0]; S.rv_prev_out[1] = S.rv_out[1];
        reverb_step((S.rv_in[0] + 2 * S.rv_in[2] + rl) >> 2, (S.rv_in[1] + 2 * S.rv_in[3] + rr) >> 2);
        S.rv_in[0] = rl; S.rv_in[1] = rr;
    } else { S.rv_in[2] = rl; S.rv_in[3] = rr; }
    int32_t ol = S.rv_odd ? (S.rv_prev_out[0] + S.rv_out[0]) >> 1 : S.rv_out[0], orr = S.rv_odd ? (S.rv_prev_out[1] + S.rv_out[1]) >> 1 : S.rv_out[1];
    S.rv_odd ^= 1;
    dl += ol; dr += orr;
    int32_t ml = sweep_vol(RG(0x1D80), &S.main_cur[0], &S.main_cnt[0]);
    int32_t mr = sweep_vol(RG(0x1D82), &S.main_cur[1], &S.main_cnt[1]);
    if (!(CNT & 0x4000) || !(CNT & 0x8000)) { o[0] = o[1] = 0; return; }
    o[0] = (int16_t)clamp16(M(soft16(dl), ml));
    o[1] = (int16_t)clamp16(M(soft16(dr), mr));
}

static void wav_put(const int16_t *s, int frames) {
    if (!S.wav) return;
    fwrite(s, 4, (size_t)frames, S.wav);
    S.wav_frames += (uint32_t)frames;
}

int spu_render(int16_t *out, int frames) {
    pthread_mutex_lock(&MX);
    for (int i = 0; i < frames; i++) render_one(&out[i * 2]);
    wav_put(out, frames);
    pthread_mutex_unlock(&MX);
    return frames;
}

/* ---- Reloj virtual: la SPU avanza con la CPU y deja la salida en un anillo SPSC para el host ----
 * Productor: el hilo de emulación (spu_clock, bajo MX porque toca el estado de la SPU). Consumidor: el hilo de audio del
 * sistema (spu_pull), que NO toma ningún candado ni espera a nadie: solo atómicos. Contadores monótonos de frames
 * (módulo 2^32) y tamaño potencia de 2. Si el anillo se llena se descartan las muestras NUEVAS (el productor no puede tocar
 * la lectura); en la práctica no ocurre porque el host acompasa la emulación al nivel del anillo (host.c, pace()). */
#define OUT_RING 16384u
#define OUT_MASK (OUT_RING - 1u)
#define PREFILL_FRAMES 1764u             /* 2 VBlanks PAL: nivel mínimo para reanudar tras una infraejecución */
static int16_t out_ring[OUT_RING * 2];
static _Alignas(64) _Atomic uint32_t out_wr;   /* línea de caché propia por índice: productor y consumidor no comparten línea */
static _Alignas(64) _Atomic uint32_t out_rd;
/* Estadísticas (escritas por un solo lado cada una, leídas con relaxed) */
static _Atomic uint64_t st_prod, st_drop;          /* productor */
static _Atomic uint64_t st_cons, st_sil;           /* consumidor */
static _Atomic uint32_t st_under;
static _Atomic uint32_t st_min = 0xFFFFFFFFu, st_max;
static _Atomic int out_discard;                    /* spu_out_discard: lo renderizado no entra en el anillo */
void spu_out_discard(int on) { atomic_store_explicit(&out_discard, on != 0, memory_order_relaxed); }

/* Depuración (solo por variable de entorno): MV_SPU_WAV=fichero graba la mezcla; MV_SPU_TRACE=1 imprime por VBlank las
 * voces activas, KON/KOFF recientes, ENDX, volúmenes y estado del reverb. */
static void wav_header(FILE *f, uint32_t frames);
static int dbg_init, dbg_trace;
static uint32_t dbg_frames, dbg_kon, dbg_koff;
static pid_t dbg_pid;
static void dbg_atexit(void) { if (getpid() == dbg_pid) spu_dump_close(); }   /* los hijos fork de camtest no tocan el WAV del padre */
static void dbg_setup(void) {
    dbg_init = 1;
    const char *w = getenv("MV_SPU_WAV");
    if (w && *w && !S.wav) { S.wav = fopen(w, "wb"); S.wav_frames = 0; if (S.wav) wav_header(S.wav, 0); }
    dbg_trace = getenv("MV_SPU_TRACE") != NULL;
    dbg_pid = getpid();
    if (S.wav) atexit(dbg_atexit);
}
static void dbg_vblank(void) {
    static uint32_t n;
    int act = 0;
    char buf[1400] = ""; int o = 0;
    for (int i = 0; i < NV; i++) {
        Voice *v = &S.v[i];
        if (v->env.phase == 4) continue;
        act++;
        uint16_t *r = &S.regs[i * 8];
        o += snprintf(buf + o, sizeof buf - (size_t)o, " v%d[a=%05x ph%d e=%04x adsr=%04x%04x vol=%04x/%04x p=%04x fl=%x%s]", i, v->addr, v->env.phase,
                      (unsigned)v->env.level, r[5], r[4], (unsigned)(uint16_t)v->cur_vol[0], (unsigned)(uint16_t)v->cur_vol[1], r[2], v->blk_flags, (RG(0x1D98) >> i) & 1 ? " rv" : "");
        if (o > 1200) break;
    }
    fprintf(stderr, "[spu] vb=%u act=%d kon=%06x koff=%06x endx=%06x cnt=%04x main=%04x/%04x cur=%04x/%04x rvon=%x rvbase=%04x rvvol=%04x/%04x xa=%u%s\n",
            n++, act, dbg_kon, dbg_koff, S.endx, CNT, RG(0x1D80), RG(0x1D82), (uint16_t)S.main_cur[0], (uint16_t)S.main_cur[1],
            RG(0x1D98) | (RG(0x1D9A) << 16), RG(0x1DA2), RG(0x1D84), RG(0x1D86), S.xa_wr - S.xa_rd, buf);
    dbg_kon = dbg_koff = 0;
}

void spu_clock(int frames) {
    int16_t tmp[2];
    pthread_mutex_lock(&MX);
    if (!dbg_init) dbg_setup();
    uint32_t wr = atomic_load_explicit(&out_wr, memory_order_relaxed);
    uint32_t rd = atomic_load_explicit(&out_rd, memory_order_acquire);
    uint64_t dropped = 0;
    const int discard = atomic_load_explicit(&out_discard, memory_order_relaxed);
    for (int i = 0; i < frames; i++) {
        render_one(tmp);
        wav_put(tmp, 1);
        if (dbg_trace && ++dbg_frames >= 882) { dbg_frames = 0; dbg_vblank(); }
        if (discard) continue;
        if (wr - rd >= OUT_RING) {   /* lleno: se relee rd por si el consumidor avanzó */
            rd = atomic_load_explicit(&out_rd, memory_order_acquire);
            if (wr - rd >= OUT_RING) { dropped++; continue; }
        }
        out_ring[(wr & OUT_MASK) * 2] = tmp[0];
        out_ring[(wr & OUT_MASK) * 2 + 1] = tmp[1];
        wr++;
    }
    atomic_store_explicit(&out_wr, wr, memory_order_release);
    pthread_mutex_unlock(&MX);
    atomic_fetch_add_explicit(&st_prod, (uint64_t)frames, memory_order_relaxed);
    if (dropped) atomic_fetch_add_explicit(&st_drop, dropped, memory_order_relaxed);
    uint32_t lvl = wr - rd;
    if (lvl > atomic_load_explicit(&st_max, memory_order_relaxed)) atomic_store_explicit(&st_max, lvl, memory_order_relaxed);
}

uint32_t spu_ring_level(void) {
    /* rd ANTES que wr: rd nunca supera a wr, así que con el orden inverso un consumidor rápido daría una resta negativa (~4G)
     * a los hilos que no son el productor (vigilante). */
    uint32_t rd = atomic_load_explicit(&out_rd, memory_order_acquire);
    return atomic_load_explicit(&out_wr, memory_order_acquire) - rd;
}
uint32_t spu_ring_reads(void) { return atomic_load_explicit(&out_rd, memory_order_relaxed); }

/* ---- Consumidor (hilo de audio de tiempo real): sin candados, sin esperas, sin memoria dinámica ----
 * Entrega `frames` muestras a la frecuencia de la sesión `rate` (44100/48000/...): remuestreo polifásico de calidad (sinc con
 * ventana Kaiser, 48 puntos, 256 fases con interpolación lineal entre fases; paso de banda hasta ~19,5 kHz, rechazo >60 dB a partir
 * de ~23,6 kHz) con relación de reloj CONTINUA ajustada por el nivel del anillo (filtro lento, +-0,5 % como máximo): ningún salto
 * de posición, ninguna muestra repetida ni descartada.
 * Si el anillo se queda sin datos no hay silencio brusco ni fundido a cero: se continúa con una extensión periódica de lo último
 * oído (el último periodo de ~11 ms repetido, con corrección de continuidad en cada vuelta y caída lenta de ganancia) y cuando
 * vuelven los datos (con margen RESUME_FRAMES) se funde cruzado ~256 muestras de la extensión al audio real. La señal es continua
 * en todo momento. */
#define RS_TAPS 48
#define RS_HALF 23                    /* se usan las fuentes base-23 .. base+24 */
#define RS_PHASES 256
#define RS_FC 0.4887                  /* corte relativo a la frecuencia de muestreo de la fuente */
#define RS_BETA 7.0
#define RESUME_FRAMES 1100u           /* margen para volver de la extensión */
#define TARGET_LEVEL ((double)SPU_TARGET_LEVEL)   /* nivel objetivo del anillo (frames); definido en spu.h junto al umbral de pace() */
#define RS_MAX_ADJ 0.005
#define CC_P 512                      /* periodo de la extensión (muestras de salida) */
#define XF_FRAMES 256
static float rs_tab[RS_PHASES + 1][RS_TAPS];
static double bessel_i0(double x) { double t = 1, s = 1; for (int k = 1; k < 50; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; }
__attribute__((constructor)) static void rs_init(void) {
    for (int ph = 0; ph <= RS_PHASES; ph++) {
        double frac = (double)ph / RS_PHASES, sum = 0, w[RS_TAPS];
        for (int k = 0; k < RS_TAPS; k++) {
            double x = (k - RS_HALF) - frac;   /* distancia de la muestra fuente a la posición (en muestras fuente) */
            double a = 2 * RS_FC * x, sinc = fabs(a) < 1e-9 ? 1.0 : sin(M_PI * a) / (M_PI * a);
            double r = x / 24.0, win = fabs(r) >= 1 ? 0.0 : bessel_i0(RS_BETA * sqrt(1 - r * r)) / bessel_i0(RS_BETA);
            w[k] = 2 * RS_FC * sinc * win; sum += w[k];
        }
        for (int k = 0; k < RS_TAPS; k++) rs_tab[ph][k] = (float)(w[k] / sum);
    }
}

enum { PL_CONCEAL = 0, PL_LIVE, PL_XFADE };
static struct {   /* solo lo toca el hilo consumidor */
    int state;                    /* empieza en extensión (silencio continuo) hasta tener RESUME_FRAMES */
    double pos;                   /* posición fraccionaria (frames fuente) respecto a rd */
    double lvl_f;                 /* nivel filtrado del anillo */
    float hist[2][CC_P + 1];      /* últimas salidas (anillo de CC_P+1) */
    int hw;                       /* índice de escritura en hist */
    float cs[2][CC_P + 1];        /* instantánea para la extensión: cs[P] = última salida real */
    int ci, cl; float corr[2], cg; int cn;   /* cn: muestras seguidas en extensión */    /* posición en la extensión, corrección de continuidad, ganancia */
    int xf;                       /* muestras de fundido cruzado hechas */
} pl = { .state = PL_CONCEAL, .lvl_f = TARGET_LEVEL, .ci = 1, .cl = CC_P, .cg = 1.0f };

static inline void pl_hist_push(float l, float r) {
    pl.hist[0][pl.hw] = l; pl.hist[1][pl.hw] = r;
    pl.hw = pl.hw == CC_P ? 0 : pl.hw + 1;
}
/* Elige el periodo de la extensión: el retardo L (96..CC_P) que mejor hace coincidir las últimas CC_W muestras con las CC_W
 * anteriores a L (similitud de forma de onda, como SOLA): así la unión de la vuelta apenas necesita corrección. */
#define CC_W 48
#define CC_MINL 96
static int pl_pick_period(void) {
    int best = CC_P; double bs = 1e30;
    for (int L = CC_MINL; L <= CC_P - CC_W; L++) {
        double e = 0;
        for (int i = 0; i < CC_W; i++) {
            int a = CC_P - i, b = a - L;
            double d = (double)(pl.cs[0][a] + pl.cs[1][a]) - (double)(pl.cs[0][b] + pl.cs[1][b]);
            e += d * d;
        }
        if (e < bs) { bs = e; best = L; }
    }
    return best;
}
static void pl_enter_conceal(void) {
    /* cs[0..P] = las últimas P+1 salidas en orden cronológico */
    for (int i = 0; i <= CC_P; i++) { int j = (pl.hw + i) % (CC_P + 1); pl.cs[0][i] = pl.hist[0][j]; pl.cs[1][i] = pl.hist[1][j]; }
    pl.cl = pl_pick_period();
    pl.ci = CC_P - pl.cl + 1; pl.cg = 1.0f; pl.cn = 0;
    /* continuidad: la primera muestra de la extensión es cs[ci] + corr, que debe seguir a cs[P] con la pendiente cs[ci]-cs[ci-1] */
    for (int c = 0; c < 2; c++) pl.corr[c] = pl.cs[c][CC_P] - pl.cs[c][pl.ci - 1];
    pl.state = PL_CONCEAL; pl.xf = 0;
}
static inline void pl_conceal_sample(float *l, float *r) {
    if (pl.cl < CC_MINL || pl.cl > CC_P) { pl.cl = CC_P; pl.ci = 1; }
    if (pl.ci < 1 || pl.ci > CC_P) pl.ci = CC_P - pl.cl + 1;
    *l = (pl.cs[0][pl.ci] + pl.corr[0]) * pl.cg;
    *r = (pl.cs[1][pl.ci] + pl.corr[1]) * pl.cg;
    if (++pl.ci > CC_P) {   /* vuelta al principio del tramo: su predecesor es cs[P-L] */
        pl.ci = CC_P - pl.cl + 1;
        for (int c = 0; c < 2; c++) pl.corr[c] += pl.cs[c][CC_P] - pl.cs[c][pl.ci - 1];
    }
    pl.corr[0] *= 1.0f - 1.0f / 384; pl.corr[1] *= 1.0f - 1.0f / 384;
    /* caída lenta (tau ~3000 muestras) en huecos cortos; si el productor sigue parado (>~100 ms: pausa, interrupción) cae rápido (tau ~20 ms) */
    pl.cg *= pl.cn < 4800 ? 1.0f - 1.0f / 3000 : 1.0f - 1.0f / 960;
    if (pl.cn < 1000000) pl.cn++;
}

static inline int16_t f2s(float x) { return x > 32767.f ? 32767 : x < -32768.f ? -32768 : (int16_t)lrintf(x); }

int spu_pull_rate(int16_t *out, int frames, int rate) {
    if (frames <= 0) return 0;
    if (rate < 8000 || rate > 192000) rate = SPU_RATE;
    uint32_t rd = atomic_load_explicit(&out_rd, memory_order_relaxed);
    uint32_t wr = atomic_load_explicit(&out_wr, memory_order_acquire);
    uint32_t avail = wr - rd;
    if (pl.state == PL_CONCEAL && avail > 4u * PREFILL_FRAMES) {   /* audio viejo acumulado sin consumidor (arranque): salta al nivel objetivo */
        rd += avail - (uint32_t)TARGET_LEVEL; avail = (uint32_t)TARGET_LEVEL;
        atomic_store_explicit(&out_rd, rd, memory_order_release);
    }
    if (pl.state != PL_CONCEAL && avail > OUT_RING / 2) {   /* consumidor parado (interrupción, pausa): todo el anillo es audio viejo */
        rd += avail; avail = 0; pl.pos = 0;   /* se descarta TODO lo viejo; se vuelve con audio nuevo por la extensión + fundido cruzado */
        atomic_store_explicit(&out_rd, rd, memory_order_release);
        pl_enter_conceal();
    }
    uint32_t lvl0 = avail, lvl_min = avail;
    /* relación de reloj: fuente por salida; el filtro lento del nivel mueve +-0,5 % como mucho */
    double alpha = (double)frames / (0.5 * rate); if (alpha > 1) alpha = 1;
    pl.lvl_f += ((double)avail - pl.lvl_f) * alpha;
    double adj = (pl.lvl_f - TARGET_LEVEL) * 5e-6;
    adj = adj > RS_MAX_ADJ ? RS_MAX_ADJ : adj < -RS_MAX_ADJ ? -RS_MAX_ADJ : adj;
    double ratio = (double)SPU_RATE / rate * (1.0 + adj);
    int live = 0, under = 0;
    for (int i = 0; i < frames; i++) {
        int base = (int)pl.pos;
        int can = (int64_t)avail - base > RS_HALF + 1;               /* hay RS_HALF+1 frames por delante de la posición */
        float l = 0, r = 0, cl = 0, cr = 0;
        if (pl.state == PL_LIVE && !can) { under++; pl_enter_conceal(); }
        else if (pl.state == PL_CONCEAL && (int64_t)avail - base >= (int64_t)RESUME_FRAMES) { pl.state = PL_XFADE; pl.xf = 0; }
        else if (pl.state == PL_XFADE && !can) { under++; pl_enter_conceal(); }
        if (pl.state != PL_CONCEAL) {
            double fr = (pl.pos - base) * RS_PHASES;
            int ph = (int)fr; float g = (float)(fr - ph);
            const float *t0 = rs_tab[ph], *t1 = rs_tab[ph + 1];
            uint32_t idx = rd + (uint32_t)base - RS_HALF;
            float al = 0, ar = 0;
            for (int k = 0; k < RS_TAPS; k++) {
                float c = t0[k] + (t1[k] - t0[k]) * g;
                uint32_t q = (idx + (uint32_t)k) & OUT_MASK;
                al += c * out_ring[q * 2]; ar += c * out_ring[q * 2 + 1];
            }
            l = al; r = ar;
            pl.pos += ratio;
            int adv = (int)pl.pos;
            if (adv > 0) { rd += (uint32_t)adv; avail -= (uint32_t)adv; pl.pos -= adv; }
            if (avail < lvl_min) lvl_min = avail;
            live++;
        }
        if (pl.state == PL_CONCEAL || pl.state == PL_XFADE) {
            pl_conceal_sample(&cl, &cr);
            if (pl.state == PL_CONCEAL) { l = cl; r = cr; }
            else {
                float w = (float)pl.xf / XF_FRAMES;
                l = cl * (1 - w) + l * w; r = cr * (1 - w) + r * w;
                if (++pl.xf >= XF_FRAMES) pl.state = PL_LIVE;
            }
        }
        pl_hist_push(l, r);
        out[i * 2] = f2s(l); out[i * 2 + 1] = f2s(r);
    }
    atomic_store_explicit(&out_rd, rd, memory_order_release);
    if (under) atomic_fetch_add_explicit(&st_under, (uint32_t)under, memory_order_relaxed);
    atomic_fetch_add_explicit(&st_cons, (uint64_t)((double)live * ratio), memory_order_relaxed);
    if (live < frames) atomic_fetch_add_explicit(&st_sil, (uint64_t)(frames - live), memory_order_relaxed);
    (void)lvl0;
    if (lvl_min < atomic_load_explicit(&st_min, memory_order_relaxed)) atomic_store_explicit(&st_min, lvl_min, memory_order_relaxed);
    return live;
}
int spu_pull(int16_t *out, int frames) { return spu_pull_rate(out, frames, SPU_RATE); }

void spu_ring_feed(const int16_t *stereo, int frames) {   /* solo pruebas: hace de productor sin la SPU */
    uint32_t wr = atomic_load_explicit(&out_wr, memory_order_relaxed);
    for (int i = 0; i < frames; i++) {
        if (wr - atomic_load_explicit(&out_rd, memory_order_acquire) >= OUT_RING) { atomic_fetch_add_explicit(&st_drop, 1, memory_order_relaxed); continue; }
        out_ring[(wr & OUT_MASK) * 2] = stereo[i * 2]; out_ring[(wr & OUT_MASK) * 2 + 1] = stereo[i * 2 + 1];
        wr++;
    }
    atomic_store_explicit(&out_wr, wr, memory_order_release);
    atomic_fetch_add_explicit(&st_prod, (uint64_t)frames, memory_order_relaxed);
}
void spu_pull_reset(void) {   /* solo pruebas: estado inicial del consumidor y del anillo (un solo hilo) */
    atomic_store(&out_wr, 0); atomic_store(&out_rd, 0);
    memset(out_ring, 0, sizeof out_ring);
    memset(&pl, 0, sizeof pl);
    pl.state = PL_CONCEAL; pl.lvl_f = TARGET_LEVEL; pl.ci = 1; pl.cl = CC_P; pl.cg = 1.0f;
    atomic_store(&st_prod, 0); atomic_store(&st_drop, 0); atomic_store(&st_cons, 0); atomic_store(&st_sil, 0); atomic_store(&st_under, 0);
    atomic_store(&st_min, 0xFFFFFFFFu); atomic_store(&st_max, 0);
}

void spu_audio_stats(SpuAudioStats *s, int reset_minmax) {
    s->produced = atomic_load_explicit(&st_prod, memory_order_relaxed);
    s->consumed = atomic_load_explicit(&st_cons, memory_order_relaxed);
    s->dropped = atomic_load_explicit(&st_drop, memory_order_relaxed);
    s->silence = atomic_load_explicit(&st_sil, memory_order_relaxed);
    s->underruns = atomic_load_explicit(&st_under, memory_order_relaxed);
    s->level = spu_ring_level();
    uint32_t mn = atomic_load_explicit(&st_min, memory_order_relaxed);
    s->level_min = mn == 0xFFFFFFFFu ? s->level : mn;
    s->level_max = atomic_load_explicit(&st_max, memory_order_relaxed);
    if (reset_minmax) {
        atomic_store_explicit(&st_min, 0xFFFFFFFFu, memory_order_relaxed);
        atomic_store_explicit(&st_max, 0, memory_order_relaxed);
    }
}

void spu_advance(int frames) {
    int16_t tmp[512 * 2];
    while (frames > 0) {
        int n = frames > 512 ? 512 : frames;
        spu_render(tmp, n);
        frames -= n;
    }
}

/* ---------------- Registros ---------------- */
static uint16_t reg_read16(uint32_t off) {
    if (off < 0x180) { /* voces */
        int i = off >> 4;
        if ((off & 0xF) == 0xC) return (uint16_t)S.v[i].env.level;
        if ((off & 0xF) == 0xE) return (uint16_t)(S.v[i].rep_addr >> 3);
        return S.regs[off / 2];
    }
    switch (off + 0x1C00) {
    case 0x1D9C: return (uint16_t)S.endx;
    case 0x1D9E: return (uint16_t)(S.endx >> 16);
    case 0x1DAE: { /* SPUSTAT: bits 0-5 reflejan SPUCNT; nunca ocupada */
        uint16_t st = (RG(0x1DAE) & 0x40) | (CNT & 0x3F);
        uint32_t mode = (CNT >> 4) & 3;
        if (mode == 2) st |= 0x100 | 0x80;
        if (mode == 3) st |= 0x200 | 0x80;
        st |= (uint16_t)((S.cap_pos >= 0x100) << 11);
        return st;
    }
    }
    if (off >= 0x200 && off < 0x260) { /* volumen actual por voz */
        int i = (off - 0x200) >> 2;
        return (uint16_t)S.v[i].cur_vol[(off >> 1) & 1];
    }
    if (off == 0x1B8 || off == 0x1BA) return (uint16_t)S.main_cur[(off >> 1) & 1];
    return S.regs[off / 2];
}

static void reg_write16(uint32_t off, uint16_t v) {
    if (off >= 0x400) return;
    S.regs[off / 2] = v;
    if (off < 0x180) {
        if ((off & 0xF) == 0xE) { Voice *vc = &S.v[off >> 4]; vc->rep_addr = (uint32_t)v * 8; vc->ignore_loop = 1; }
        return;
    }
    switch (off + 0x1C00) {
    case 0x1D88: dbg_kon |= v; for (int i = 0; i < 16; i++) if (v & (1u << i)) key_on(i); break;
    case 0x1D8A: dbg_kon |= (uint32_t)v << 16; for (int i = 0; i < 8; i++) if (v & (1u << i)) key_on(16 + i); break;
    case 0x1D8C: dbg_koff |= v; for (int i = 0; i < 16; i++) if (v & (1u << i)) spu_adsr_keyoff(&S.v[i].env); break;
    case 0x1D8E: dbg_koff |= (uint32_t)v << 16; for (int i = 0; i < 8; i++) if (v & (1u << i)) spu_adsr_keyoff(&S.v[16 + i].env); break;
    case 0x1D9C: case 0x1D9E: break; /* ENDX de solo lectura */
    case 0x1DA2: S.rv_cur = (uint32_t)v * 8; break;
    case 0x1DA6: S.xfer_addr = (uint32_t)v * 8; break;
    case 0x1DA8: { /* FIFO de transferencia manual */
        uint32_t a = S.xfer_addr & (SPU_RAM_SIZE - 2);
        memcpy(&S.ram[a], &v, 2);
        irq_check(a, 2);
        S.xfer_addr = (a + 2) & (SPU_RAM_SIZE - 1);
        break;
    }
    case 0x1DAA:
        if (!(v & 0x40)) RG(0x1DAE) &= (uint16_t)~0x40; /* reconocer IRQ */
        if (!(v & 0x8000)) { /* SPU apagada: corta voces */
            for (int i = 0; i < NV; i++) { S.v[i].env.phase = 4; S.v[i].env.level = 0; }
        }
        break;
    }
}

uint32_t spu_io_read(uint32_t addr, int size) {
    uint32_t off = (addr & 0x3FF) & ~1u;
    pthread_mutex_lock(&MX);
    uint32_t v = reg_read16(off);
    if (size == 4) v |= (uint32_t)reg_read16(off + 2) << 16;
    else if (size == 1) v = (addr & 1) ? v >> 8 : v & 0xFF;
    pthread_mutex_unlock(&MX);
    return v;
}

void spu_io_write(uint32_t addr, uint32_t v, int size) {
    uint32_t off = (addr & 0x3FF) & ~1u;
    pthread_mutex_lock(&MX);
    if (size == 4) { reg_write16(off, (uint16_t)v); reg_write16(off + 2, (uint16_t)(v >> 16)); }
    else if (size == 2) reg_write16(off, (uint16_t)v);
    else {
        uint16_t cur = S.regs[off / 2];
        cur = (addr & 1) ? (uint16_t)((cur & 0xFF) | ((v & 0xFF) << 8)) : (uint16_t)((cur & 0xFF00) | (v & 0xFF));
        reg_write16(off, cur);
    }
    pthread_mutex_unlock(&MX);
}

/* ---------------- DMA canal 4 ---------------- */
void spu_dma_write(const uint32_t *src, uint32_t words) {
    pthread_mutex_lock(&MX);
    for (uint32_t i = 0; i < words; i++) {
        uint32_t a = S.xfer_addr & (SPU_RAM_SIZE - 4);
        memcpy(&S.ram[a], &src[i], 4);
        irq_check(a, 4);
        S.xfer_addr = (a + 4) & (SPU_RAM_SIZE - 1);
    }
    pthread_mutex_unlock(&MX);
}

void spu_dma_read(uint32_t *dst, uint32_t words) {
    pthread_mutex_lock(&MX);
    for (uint32_t i = 0; i < words; i++) {
        uint32_t a = S.xfer_addr & (SPU_RAM_SIZE - 4);
        memcpy(&dst[i], &S.ram[a], 4);
        irq_check(a, 4);
        S.xfer_addr = (a + 4) & (SPU_RAM_SIZE - 1);
    }
    pthread_mutex_unlock(&MX);
}

/* ---------------- CD-XA ---------------- */
static void xa_push(int16_t l, int16_t r) {
    if (S.xa_wr - S.xa_rd >= XA_Q) S.xa_rd++; /* cola llena: descarta lo más antiguo */
    S.xa_q[S.xa_wr % XA_Q][0] = l;
    S.xa_q[S.xa_wr % XA_Q][1] = r;
    S.xa_wr++;
}

void spu_xa_reset(void) {
    pthread_mutex_lock(&MX);
    S.xa_rd = S.xa_wr = 0;
    memset(S.xa_hist, 0, sizeof S.xa_hist);
    xa_pos = 0; xa_last[0] = xa_last[1] = 0;
    pthread_mutex_unlock(&MX);
}

/* Sector Form 2: subcabecera [fichero, canal, submodo, codificación] x2, luego 18 grupos de 128 B. */
void spu_feed_xa(const uint8_t *sec, int len) {
    if (len < 8 + 18 * 128) return;
    uint8_t coding = sec[3];
    int stereo = (coding & 3) == 1, half = ((coding >> 2) & 3) == 1, bits8 = ((coding >> 4) & 3) == 1;
    int units = bits8 ? 4 : 8;
    static int16_t pcm[2][18 * 8 * 28];
    int n[2] = { 0, 0 };
    for (int g = 0; g < 18; g++) {
        const uint8_t *grp = sec + 8 + g * 128;
        for (int u = 0; u < units; u++) {
            uint8_t hdr = grp[4 + u];
            int shift = hdr & 0xF, filt = (hdr >> 4) & 3;
            if (shift > 12) shift = 9;
            int ch = stereo ? (u & 1) : 0;
            int32_t *h = S.xa_hist[ch];
            for (int j = 0; j < 28; j++) {
                int32_t s;
                if (bits8) s = (int16_t)(grp[16 + j * 4 + u] << 8) >> shift;
                else s = (int16_t)(((grp[16 + j * 4 + u / 2] >> ((u & 1) * 4)) & 0xF) << 12) >> shift;
                s = clamp16(s + ((h[0] * K_POS[filt] + h[1] * K_NEG[filt] + 32) >> 6));
                h[1] = h[0]; h[0] = s;
                pcm[ch][n[ch]++] = (int16_t)s;
            }
        }
    }
    if (!stereo) memcpy(pcm[1], pcm[0], sizeof(int16_t) * (size_t)n[0]), n[1] = n[0];
    int cnt = n[0] < n[1] ? n[0] : n[1];
    /* remuestreo lineal 37800/18900 -> 44100 */
    uint32_t src_rate = half ? 18900 : 37800;
    uint32_t step = (uint32_t)(((uint64_t)src_rate << 16) / SPU_RATE);
    pthread_mutex_lock(&MX);
    uint32_t pos = xa_pos;
    int16_t *last = xa_last;
    for (; (pos >> 16) < (uint32_t)cnt; pos += step) {
        uint32_t k = pos >> 16, f = pos & 0xFFFF;
        int32_t a0 = k ? pcm[0][k - 1] : last[0], b0 = pcm[0][k];
        int32_t a1 = k ? pcm[1][k - 1] : last[1], b1 = pcm[1][k];
        xa_push((int16_t)(a0 + (((b0 - a0) * (int32_t)f) >> 16)), (int16_t)(a1 + (((b1 - a1) * (int32_t)f) >> 16)));
    }
    xa_pos = pos - ((uint32_t)cnt << 16);
    last[0] = pcm[0][cnt - 1]; last[1] = pcm[1][cnt - 1];
    pthread_mutex_unlock(&MX);
}

/* ---------------- WAV ---------------- */
static void wav_header(FILE *f, uint32_t frames) {
    uint32_t data = frames * 4;
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    uint32_t v = 36 + data; memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4);
    uint16_t s = 1; memcpy(h + 20, &s, 2);
    s = 2; memcpy(h + 22, &s, 2);
    v = SPU_RATE; memcpy(h + 24, &v, 4);
    v = SPU_RATE * 4; memcpy(h + 28, &v, 4);
    s = 4; memcpy(h + 32, &s, 2);
    s = 16; memcpy(h + 34, &s, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}

int spu_dump_wav(const char *path) {
    pthread_mutex_lock(&MX);
    if (S.wav) { wav_header(S.wav, S.wav_frames); fclose(S.wav); }
    S.wav = fopen(path, "wb");
    S.wav_frames = 0;
    if (S.wav) wav_header(S.wav, 0);
    pthread_mutex_unlock(&MX);
    return S.wav ? 0 : -1;
}

void spu_dump_close(void) {
    pthread_mutex_lock(&MX);
    if (S.wav) { wav_header(S.wav, S.wav_frames); fclose(S.wav); S.wav = NULL; }
    pthread_mutex_unlock(&MX);
}

void spu_reset(void) {
    pthread_mutex_lock(&MX);
    FILE *w = S.wav; uint32_t wf = S.wav_frames;
    memset(&S, 0, sizeof S);
    S.wav = w; S.wav_frames = wf;
    xa_pos = 0; xa_last[0] = xa_last[1] = 0;
    for (int i = 0; i < NV; i++) S.v[i].env.phase = 4;
    pthread_mutex_unlock(&MX);
}
