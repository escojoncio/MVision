/* Registros de E/S PS1 (0x1F801000-0x1F802FFF): interrupciones, DMA, temporizadores, SIO y
 * reparto hacia GPU/CD/SPU/MDEC mediante ganchos débiles que implementan otros módulos.
 * El reloj es virtual y dirigido por eventos: cada sondeo RT_POLL del código recompilado consume
 * SYS_POLL_CYC ciclos, cada lectura de E/S 64, y el VBlank (50 Hz PAL) y las IRQ de temporizadores se
 * programan en ese tiempo. rt_budget es el número de sondeos hasta el próximo evento; al agotarse
 * rt_poll -> sys_clock_tick procesa lo vencido y la excepción HLE (irq.c) entrega lo pendiente. */
#include <stdio.h>
#include <string.h>
#include "cpu.h"
#include "sys.h"
#include "gpu.h"
#include "spu.h"
#include "mdec.h"
#include "hle.h"
#include "cdrom.h"

#define CPU_HZ 33868800u
#define IO_TICK 64u /* ciclos virtuales por acceso de lectura a E/S */

static uint32_t regs[0x2000 / 4];   /* copia plana para lo que no tiene semántica propia */
static uint64_t clk;                /* ciclos virtuales */
static int pal = 1;
static uint32_t i_stat, i_mask = 0x27Fu; /* VBlank, GPU, CD, DMA, TMR0-2, SPU: el kernel los deja habilitados */
static uint32_t frame_no;
#define SPU_BATCH 32u                 /* muestras por lote de reloj de SPU */
#define SPU_BATCH_CYC (SPU_BATCH * 768u) /* 768 ciclos por muestra a 44100 Hz */
static uint64_t next_spu;             /* ciclo del próximo lote de SPU */
static uint64_t next_vbl;             /* ciclo del próximo VBlank */
static int32_t chunk;                 /* sondeos asignados al último presupuesto */
int32_t rt_budget;

uint64_t sys_cycles(void) { return clk; }
void sys_set_video(int p) { pal = p; }
static uint32_t cycles_per_frame(void) { return pal ? CPU_HZ / 50 : CPU_HZ / 60; }
static uint32_t lines_per_frame(void) { return pal ? 314 : 263; }

void sys_kick(void);
void sys_irq_raise(int bit) { i_stat |= 1u << bit; if (i_stat & i_mask) sys_kick(); }
void sys_irq_ack(int bit) { i_stat &= ~(1u << bit); }
uint32_t sys_irq_stat(void) { return i_stat; }
uint32_t sys_irq_mask(void) { return i_mask; }
void sys_irq_enable(int bit, int on) {
    if (on) { i_mask |= 1u << bit; if (i_stat & i_mask) sys_kick(); }
    else i_mask &= ~(1u << bit);
}
uint32_t sys_vblank_count(void) { return frame_no; }
uint32_t sys_hcount(void) { return (uint32_t)(clk * lines_per_frame() / cycles_per_frame()) & 0xFFFF; }

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

uint32_t sys_dma_flags_take(void) {
    uint32_t f = (dicr >> 24) & 0x7F;
    dicr &= ~(f << 24);
    dicr_update();
    return f;
}

void sys_dma_irq_enable(int ch, int on) {
    if (on) dicr |= (1u << (16 + ch)) | (1u << 23);
    else dicr &= ~(1u << (16 + ch));
    dicr_update();
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

typedef struct {
    uint32_t mode, target; uint64_t base; uint32_t flags;
    uint64_t next_irq;   /* ciclo de la próxima IRQ (~0 = ninguna) */
    uint64_t from;       /* primer tick a considerar para la próxima IRQ */
    int done;            /* modo de un solo disparo ya entregado */
} Timer;
static Timer tm[3] = {{.next_irq = ~0ull}, {.next_irq = ~0ull}, {.next_irq = ~0ull}};

static uint64_t timer_ticks(int n) {
    uint64_t e = clk - tm[n].base;
    uint32_t src = (tm[n].mode >> 8) & 3;
    if (n == 1 && (src & 1)) return e * lines_per_frame() / cycles_per_frame(); /* hblank */
    if (n == 2 && (src & 2)) return e / 8;
    if (n == 0 && (src & 1)) return e * 2 / 11;                                  /* dotclock aprox. */
    return e;
}

/* Menor número de ciclos de CPU e con timer_ticks(e) >= ticks (inversa exacta, en enteros, de timer_ticks):
 * así la IRQ salta en el primer ciclo en que el contador alcanza el valor, sin mezclar ticks y ciclos en coma flotante. */
static uint64_t cycles_for_ticks(int n, uint64_t ticks) {
    uint32_t src = (tm[n].mode >> 8) & 3;
    if (n == 1 && (src & 1)) { uint64_t cpf = cycles_per_frame(), l = lines_per_frame(); return (ticks * cpf + l - 1) / l; }
    if (n == 2 && (src & 2)) return ticks * 8;
    if (n == 0 && (src & 1)) return (ticks * 11 + 1) / 2;
    return ticks;
}

/* Programa la próxima IRQ del temporizador: bit 4 = al llegar al objetivo, bit 5 = al desbordar. */
static void timer_sched(int n) {
    Timer *t = &tm[n];
    t->next_irq = ~0ull;
    if (!(t->mode & 0x30) || t->done) return;
    uint64_t P = (t->mode & 8) ? (uint64_t)(t->target & 0xFFFF) + 1 : 0x10000, best = ~0ull, from = t->from;
    if (t->mode & 0x10) {
        uint64_t tg = t->target & 0xFFFF;
        if (tg < P) { uint64_t k = from > tg ? (from - tg + P - 1) / P : 0; best = k * P + tg; }
    }
    if ((t->mode & 0x20) && !((t->mode & 8) && (t->target & 0xFFFF) < 0xFFFF)) {
        uint64_t m = (from + 0xFFFF) / 0x10000;
        if (m < 1) m = 1;
        if (m * 0x10000 < best) best = m * 0x10000;
    }
    if (best != ~0ull) t->next_irq = t->base + cycles_for_ticks(n, best);
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
    case 0: tm[n].base = clk - cycles_for_ticks(n, v & 0xFFFF); tm[n].from = v & 0xFFFF; break;
    case 4: tm[n].mode = v & 0x3FF; tm[n].base = clk; tm[n].flags = 0; tm[n].from = 0; tm[n].done = 0; break;
    case 8: tm[n].target = v & 0xFFFF; break;
    }
    timer_sched(n);
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
    else if (o == 0x74) { i_mask = v & 0x7FF; if (i_stat & i_mask) sys_kick(); }
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

/* ---------------- reloj dirigido por eventos ---------------- */

/* Vuelca a reloj los sondeos consumidos desde el último re-armado y deja el presupuesto en 0. */
static void account(void) {
    int32_t used = chunk - rt_budget;
    if (used > 0) clk += (uint64_t)used * SYS_POLL_CYC;
    chunk = 0;
    rt_budget = 0;
}

static uint64_t next_event(void) {
    if (!next_vbl) next_vbl = cycles_per_frame();
    if (!next_spu) next_spu = SPU_BATCH_CYC;
    uint64_t e = next_vbl;
    if (next_spu < e) e = next_spu;
    for (int n = 0; n < 3; n++) if (tm[n].next_irq < e) e = tm[n].next_irq;
    return e;
}

static void arm(void) {
    uint64_t e = next_event();
    int64_t d = (int64_t)(e - clk);
    int64_t polls = d <= 0 ? 0 : d / SYS_POLL_CYC;
    if (polls > (1 << 24)) polls = 1 << 24;
    chunk = rt_budget = (int32_t)polls;
}

void sys_kick(void) { account(); }

/* VBlank de hardware: contador, I_STAT, SPU, contadores, mando, CD y gancho de frame del host. */
static void vblank_hw(Cpu *c) {
    frame_no++;
    spu_irq_poll();
    for (int n = 0; n < 3; n++) timer_count(n);
    pad_frame(c);
    sys_irq_raise(IRQ_VBLANK);
    if (cd_busy()) sys_irq_raise(IRQ_CD);
    if (rt_host_frame) rt_host_frame(c, frame_no);
}

static void run_events(Cpu *c) {
    if (!next_vbl) next_vbl = cycles_per_frame();
    if (!next_spu) next_spu = SPU_BATCH_CYC;
    while (clk >= next_vbl) {
        next_vbl += cycles_per_frame();
        vblank_hw(c);
    }
    while (clk >= next_spu) {   /* se pone al día por lotes tras saltos de reloj */
        next_spu += SPU_BATCH_CYC;
        spu_clock((int)SPU_BATCH);
        spu_irq_poll();
    }
    for (int n = 0; n < 3; n++) {
        Timer *t = &tm[n];
        if (t->next_irq > clk) continue;
        sys_irq_raise(IRQ_TMR0 + n);
        if (!(t->mode & 0x40)) t->done = 1;   /* un solo disparo hasta reescribir el modo */
        t->from = timer_ticks(n) + 1;
        timer_sched(n);
    }
}

void sys_clock_tick(Cpu *c) {
    account();
    run_events(c);
    arm();
}

void sys_idle(Cpu *c) {
    account();
    uint64_t e = next_event();
    if (e > clk) clk = e;
    run_events(c);
    arm();
}

void sys_frame(Cpu *c) {
    account();
    clk += cycles_per_frame();
    next_vbl = clk + cycles_per_frame();
    vblank_hw(c);
    run_events(c);
    arm();
}
