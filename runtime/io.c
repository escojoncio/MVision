/* Registros de E/S PS1 (0x1F801000-0x1F802FFF): interrupciones, DMA, temporizadores, SIO y
 * reparto hacia GPU/CD/SPU/MDEC mediante ganchos débiles que implementan otros módulos.
 * El reloj es virtual: avanza un frame entero en sys_frame y un poco en cada lectura de E/S,
 * para que los bucles de sondeo sobre temporizadores terminen. */
#include <stdio.h>
#include <string.h>
#include "cpu.h"
#include "sys.h"
#include "gpu.h"
#include "spu.h"
#include "mdec.h"

#define CPU_HZ 33868800u
#define IO_TICK 64u /* ciclos virtuales por acceso de lectura a E/S */

static uint32_t regs[0x2000 / 4];   /* copia plana para lo que no tiene semántica propia */
static uint64_t clk;                /* ciclos virtuales */
static int pal = 1;
static uint32_t i_stat, i_mask;
static uint32_t frame_no;

uint64_t sys_cycles(void) { return clk; }
void sys_set_video(int p) { pal = p; }
static uint32_t cycles_per_frame(void) { return pal ? CPU_HZ / 50 : CPU_HZ / 60; }
static uint32_t lines_per_frame(void) { return pal ? 314 : 263; }

void sys_irq_raise(int bit) { i_stat |= 1u << bit; }
uint32_t sys_irq_stat(void) { return i_stat; }

static inline uint32_t off_of(uint32_t a) { return (a & 0x1FFFFFFFu) - PSX_IO; }

static uint32_t plain_read(uint32_t a, int size) {
    uint32_t o = off_of(a);
    uint32_t v = regs[(o & ~3u) / 4] >> ((o & 3) * 8);
    return size == 4 ? v : size == 2 ? (v & 0xFFFF) : (v & 0xFF);
}

static void plain_write(uint32_t a, uint32_t v, int size) {
    uint32_t o = off_of(a), *r = &regs[(o & ~3u) / 4];
    if (size == 4) { *r = v; return; }
    int sh = (o & 3) * 8;
    uint32_t m = (size == 2 ? 0xFFFFu : 0xFFu) << sh;
    *r = (*r & ~m) | ((v << sh) & m);
}

/* ---------------- subsistemas ----------------
 * GPU (gpu.c), SPU (spu.c) y MDEC (mdec.c) se enlazan directamente. El CD se sustituye por HLE de
 * la librería (cdrom.c); sus registros quedan como gancho débil con un estado "libre". */

static uint32_t gpu_regs_read(uint32_t o) { return o == 0x814 ? gpu_stat() : gpu_read(); }
static void gpu_regs_write(uint32_t o, uint32_t v) { if (o == 0x814) gpu_gp1(v); else gpu_gp0(v); }

__attribute__((weak)) uint32_t cd_io_read(uint32_t a, int size) {
    if ((a & 3) == 0) return 0x18; /* FIFO de parámetros vacía, sin ocupado */
    return plain_read(a, size);
}
__attribute__((weak)) void cd_io_write(uint32_t a, uint32_t v, int size) { plain_write(a, v, size); }
__attribute__((weak)) void cd_dma(uint32_t m, uint32_t b, uint32_t ch) { (void)m; (void)b; (void)ch; }

static uint32_t dma_words(uint32_t bcr, uint32_t chcr) {
    if (((chcr >> 9) & 3) == 0) return bcr & 0xFFFF ? bcr & 0xFFFF : 0x10000;
    return (bcr & 0xFFFF) * (bcr >> 16);
}

/* Puntero host a RAM con recorte al final de los 2 MB. */
static uint32_t *ram_words(uint32_t madr, uint32_t *n) {
    uint32_t a = madr & 0x1FFFFC, max = (PSX_RAM_SIZE - a) / 4;
    if (*n > max) *n = max;
    return (uint32_t *)&psx_ram[a];
}

static void spu_dma_ch(uint32_t m, uint32_t b, uint32_t ch) {
    uint32_t n = dma_words(b, ch);
    uint32_t *p = ram_words(m, &n);
    if (ch & 1) spu_dma_write(p, n); else spu_dma_read(p, n);
}

static void mdec_dma0(uint32_t m, uint32_t b, uint32_t ch) {
    uint32_t n = dma_words(b, ch);
    mdec_dma_in(ram_words(m, &n), n);
}

static void mdec_dma1(uint32_t m, uint32_t b, uint32_t ch) {
    uint32_t n = dma_words(b, ch);
    mdec_dma_out(ram_words(m, &n), n);
}

/* IRQ9 de la SPU hacia I_STAT. */
static void spu_irq_poll(void) { if (spu_irq_take()) sys_irq_raise(IRQ_SPU); }

/* ---------------- DMA ---------------- */

static uint32_t dma_madr[7], dma_bcr[7], dma_chcr[7], dpcr = 0x07654321u, dicr;

static void dicr_update(void) {
    uint32_t flags = (dicr >> 24) & 0x7F, en = (dicr >> 16) & 0x7F;
    int master = ((dicr >> 15) & 1) || (((dicr >> 23) & 1) && (flags & en));
    int was = (dicr >> 31) & 1;
    dicr = (dicr & 0x7FFFFFFFu) | ((uint32_t)master << 31);
    if (master && !was) sys_irq_raise(IRQ_DMA);
}

/* Canal 6: limpia una tabla de orden (lista enlazada hacia atrás, último = 0xFFFFFF). */
static void dma_otc(uint32_t madr, uint32_t bcr) {
    uint32_t n = bcr & 0xFFFF ? bcr & 0xFFFF : 0x10000;
    uint32_t a = madr & 0x1FFFFC;
    for (uint32_t i = 0; i < n; i++, a = (a - 4) & 0x1FFFFC) {
        uint32_t v = i == n - 1 ? 0x00FFFFFFu : ((a - 4) & 0x1FFFFC);
        memcpy(&psx_ram[a], &v, 4);
    }
}

static void dma_start(int ch) {
    uint32_t m = dma_madr[ch] & 0xFFFFFF, b = dma_bcr[ch], c = dma_chcr[ch];
    switch (ch) {
    case 0: mdec_dma0(m, b, c); break;
    case 1: mdec_dma1(m, b, c); break;
    case 2: gpu_dma2(m, b, c); break;
    case 3: cd_dma(m, b, c); break;
    case 4: spu_dma_ch(m, b, c); spu_irq_poll(); break;
    case 6: dma_otc(m, b); break;
    default: break;
    }
    dma_chcr[ch] &= ~((1u << 24) | (1u << 28));
    if (dicr & (1u << (16 + ch))) { dicr |= 1u << (24 + ch); }
    dicr_update();
}

static uint32_t dma_read(uint32_t o) {
    if (o == 0xF0) return dpcr;
    if (o == 0xF4) return dicr;
    int ch = (int)((o - 0x80) >> 4);
    switch (o & 0xC) {
    case 0: return dma_madr[ch];
    case 4: return dma_bcr[ch];
    case 8: return dma_chcr[ch];
    }
    return 0;
}

static void dma_write(uint32_t o, uint32_t v) {
    if (o == 0xF0) { dpcr = v; return; }
    if (o == 0xF4) {
        /* bits 24-30 se reconocen escribiendo 1 */
        dicr = (dicr & 0xFF000000u & ~(v & 0x7F000000u)) | (v & 0x00FF803Fu);
        dicr_update();
        return;
    }
    int ch = (int)((o - 0x80) >> 4);
    switch (o & 0xC) {
    case 0: dma_madr[ch] = v & 0xFFFFFF; break;
    case 4: dma_bcr[ch] = v; break;
    case 8:
        dma_chcr[ch] = v;
        if (v & (1u << 24)) dma_start(ch);
        break;
    }
}

/* ---------------- temporizadores (root counters) ---------------- */

typedef struct { uint32_t mode, target; uint64_t base; uint32_t flags; } Timer;
static Timer tm[3];

static uint64_t timer_ticks(int n) {
    uint64_t e = clk - tm[n].base;
    uint32_t src = (tm[n].mode >> 8) & 3;
    if (n == 1 && (src & 1)) return e * lines_per_frame() / cycles_per_frame(); /* hblank */
    if (n == 2 && (src & 2)) return e / 8;
    if (n == 0 && (src & 1)) return e * 2 / 11;                                  /* dotclock aprox. */
    return e;
}

static uint32_t timer_count(int n) {
    uint64_t t = timer_ticks(n);
    uint32_t lim = (tm[n].mode & 8) ? (tm[n].target & 0xFFFF) + 1 : 0x10000;
    if (lim == 1) lim = 0x10000;
    if (t >= lim) tm[n].flags |= (tm[n].mode & 8) ? (1u << 11) : (1u << 12);
    return (uint32_t)(t % lim);
}

static uint32_t timer_read(uint32_t o) {
    int n = (int)((o - 0x100) >> 4);
    if (n > 2) return 0;
    switch (o & 0xC) {
    case 0: return timer_count(n);
    case 4: {
        timer_count(n);
        uint32_t v = (tm[n].mode & 0x3FF) | (1u << 10) | tm[n].flags;
        tm[n].flags = 0; /* bits 11/12 se borran al leer */
        return v;
    }
    case 8: return tm[n].target;
    }
    return 0;
}

static void timer_write(uint32_t o, uint32_t v) {
    int n = (int)((o - 0x100) >> 4);
    if (n > 2) return;
    switch (o & 0xC) {
    case 0: tm[n].base = clk - (v & 0xFFFF); break; /* aprox.: válido para fuente de reloj de sistema */
    case 4: tm[n].mode = v & 0x3FF; tm[n].base = clk; tm[n].flags = 0; break;
    case 8: tm[n].target = v & 0xFFFF; break;
    }
}

/* ---------------- SIO0 (mando/tarjeta por registros) ----------------
 * El juego usa las librerías (sustituidas por HLE); si algo escribe directamente, se responde
 * como puerto sin dispositivo (0xFF) y el bus siempre listo, para no colgar bucles de espera. */
static uint32_t sio_ctrl, sio_mode, sio_baud, sio_rx = 0xFF, sio_rx_full;

static uint32_t sio_read(uint32_t o, int size) {
    switch (o) {
    case 0x40: { uint32_t v = sio_rx; sio_rx_full = 0; sio_rx = 0xFF; return size == 1 ? v : v | 0xFFFFFF00u; }
    case 0x44: return 0x5u | (sio_rx_full ? 2u : 0u) | 0x80u; /* TX listo, RX, /ACK alto */
    case 0x48: return sio_mode;
    case 0x4A: return sio_ctrl;
    case 0x4E: return sio_baud;
    }
    return 0;
}

static void sio_write(uint32_t o, uint32_t v) {
    switch (o) {
    case 0x40: sio_rx = 0xFF; sio_rx_full = 1; break; /* sin dispositivo */
    case 0x48: sio_mode = v; break;
    case 0x4A: sio_ctrl = v & ~0x50u; if (v & 0x40) { sio_rx_full = 0; } break;
    case 0x4E: sio_baud = v; break;
    }
}

/* ---------------- detección de sondeo infinito ---------------- */
static uint32_t poll_addr, poll_n;

static void poll_check(uint32_t a, uint32_t v) {
    static uint32_t last_v;
    if (a == poll_addr && v == last_v) {
        if (++poll_n == 2000000u)
            fprintf(stderr, "[io] posible espera infinita: 0x%08x devuelve siempre 0x%08x\n", a, v);
    } else { poll_addr = a; poll_n = 0; last_v = v; }
}

/* ---------------- enrutado ---------------- */

uint32_t io_read(uint32_t a, int size) {
    uint32_t o = off_of(a), v;
    clk += IO_TICK;
    if (o >= 0x40 && o < 0x50) v = sio_read(o, size);
    else if (o == 0x70) { spu_irq_poll(); v = i_stat; }
    else if (o == 0x72) v = 0;
    else if (o == 0x74) v = i_mask;
    else if (o >= 0x80 && o < 0x100) v = dma_read(o & ~3u) >> ((o & 3) * 8);
    else if (o >= 0x100 && o < 0x130) v = timer_read(o & ~3u) >> ((o & 3) * 8);
    else if (o >= 0x800 && o < 0x804) v = cd_io_read(a, size);
    else if (o >= 0x810 && o < 0x818) v = gpu_regs_read(o & ~3u);
    else if (o >= 0x820 && o < 0x828) v = mdec_io_read(a, size);
    else if (o >= 0xC00 && o < 0x1000) { v = spu_io_read(a, size); spu_irq_poll(); }
    else v = plain_read(a, size);
    if (size == 1) v &= 0xFF; else if (size == 2) v &= 0xFFFF;
    poll_check(a, v);
    return v;
}

void io_write(uint32_t a, uint32_t v, int size) {
    uint32_t o = off_of(a);
    poll_addr = 0;
    if (o >= 0x40 && o < 0x50) sio_write(o, v);
    else if (o == 0x70) i_stat &= v;
    else if (o == 0x74) i_mask = v & 0x7FF;
    else if (o >= 0x80 && o < 0x100) {
        if (size != 4) { /* parcial: combinar con el valor actual */
            uint32_t cur = dma_read(o & ~3u); int sh = (o & 3) * 8;
            uint32_t m = (size == 2 ? 0xFFFFu : 0xFFu) << sh;
            v = (cur & ~m) | ((v << sh) & m);
        }
        dma_write(o & ~3u, v);
    }
    else if (o >= 0x100 && o < 0x130) timer_write(o & ~3u, v);
    else if (o >= 0x800 && o < 0x804) cd_io_write(a, v, size);
    else if (o >= 0x810 && o < 0x818) gpu_regs_write(o & ~3u, v);
    else if (o >= 0x820 && o < 0x828) mdec_io_write(a, v, size);
    else if (o >= 0xC00 && o < 0x1000) { spu_io_write(a, v, size); spu_irq_poll(); }
    else plain_write(a, v, size);
}

/* ---------------- frame ---------------- */

void sys_frame(Cpu *c) {
    frame_no++;
    clk += cycles_per_frame();
    sys_irq_raise(IRQ_VBLANK);
    spu_irq_poll();
    for (int n = 0; n < 3; n++) timer_count(n);
    bios_deliver_event(c, EV_RCNT(3), EVS_INTR);  /* evento de VBLANK por contador 3 */
    pad_frame(c);
}
