/* Pruebas de E/S de sistema: registros, DMA OTC, temporizadores, eventos, mando y tarjeta.
 *   make GEN=<dir> OUT=<dir> test_sys  (ver Makefile) o compilar a mano con el runtime. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cpu.h"
#include "sys.h"
#include "fiber.h"
#include "spu.h"

const FuncEntry rt_func_table[] = {{0, 0}};
const unsigned rt_func_count = 0;
const FuncName rt_func_names[] = {{0xFFFFFFFFu, ""}};
const unsigned rt_func_name_count = 0;
const uint32_t rt_entry = 0;

void hle_ctl_init_direct(Cpu *c);
void hle_ctl_start(Cpu *c);
void hle_ctl_state(Cpu *c);

static int fails;
#define EQ(a, b) do { unsigned long long _a = (a), _b = (b); \
    if (_a != _b) { printf("FALLO %s:%d %s = 0x%llx, esperado 0x%llx\n", __FILE__, __LINE__, #a, _a, _b); fails++; } } while (0)

static uint32_t bios(Cpu *c, uint32_t vec, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; c->r[7] = a3; c->r[9] = fn;
    bios_hle(c, vec);
    return c->r[2];
}

static void put_str(Cpu *c, uint32_t a, const char *s) {
    do mem_w8(c, a++, (uint8_t)*s); while (*s++);
}

static void t_irq(Cpu *c) {
    mem_w32(c, 0x1F801074, 0xFFFF);
    EQ(mem_r32(c, 0x1F801074), 0x7FF);
    sys_frame(c);
    EQ(mem_r32(c, 0x1F801070) & 1, 1);
    mem_w32(c, 0x1F801070, ~1u);                 /* reconocer VBLANK */
    EQ(mem_r32(c, 0x1F801070) & 1, 0);
}

static void t_timer(Cpu *c) {
    mem_w32(c, 0x1F801104, 0);                   /* contador 0, reloj de sistema */
    uint32_t a = mem_r32(c, 0x1F801100);
    uint32_t b = mem_r32(c, 0x1F801100);
    EQ(b != a, 1);                               /* avanza con cada acceso */
    mem_w32(c, 0x1F801114, 0x100);               /* contador 1, hblank */
    sys_frame(c);
    uint32_t h = mem_r32(c, 0x1F801110);
    EQ(h >= 300 && h < 330, 1);                  /* ~314 líneas PAL */
}

static void t_dma_otc(Cpu *c) {
    mem_w32(c, 0x1F8010E0, 0x80002000 + 4 * 3); /* final de una OT de 4 entradas */
    mem_w32(c, 0x1F8010E4, 4);
    mem_w32(c, 0x1F8010E8, 0x11000002);
    EQ(mem_r32(c, 0x8000200C), 0x2008);
    EQ(mem_r32(c, 0x80002004), 0x2000);
    EQ(mem_r32(c, 0x80002000), 0x00FFFFFF);
    EQ(mem_r32(c, 0x1F8010E8) & (1u << 24), 0);
}

static void t_events(Cpu *c) {
    uint32_t ev = bios(c, 0xB0, 0x08, EV_RCNT(3), 2, 0x2000, 0);
    EQ(ev >> 24, 0xF1);
    EQ(bios(c, 0xB0, 0x0B, ev, 0, 0, 0), 0);     /* sin activar */
    bios_deliver_event(c, EV_RCNT(3), 2);
    EQ(bios(c, 0xB0, 0x0B, ev, 0, 0, 0), 0);     /* desactivado: no se registra */
    bios(c, 0xB0, 0x0C, ev, 0, 0, 0);
    bios(c, 0xB0, 0x07, EV_RCNT(3), 2, 0, 0);    /* entregar evento */
    EQ(bios(c, 0xB0, 0x0B, ev, 0, 0, 0), 1);
    EQ(bios(c, 0xB0, 0x0B, ev, 0, 0, 0), 0);     /* consultar evento consume */
    bios(c, 0xB0, 0x09, ev, 0, 0, 0);
    uint32_t k = bios(c, 0xB0, 0x00, 64, 0, 0, 0);
    EQ(k != 0, 1);
}

static void t_pad(Cpu *c) {
    c->r[4] = 0x80003000; c->r[5] = 0x80003100;
    hle_ctl_init_direct(c);
    hle_ctl_start(c);
    pad_set_state(0, 0x4008, 0x10, 0x20, 0x30, 0x40); /* Cruz + Start */
    pad_frame(c);
    EQ(mem_r8(c, 0x80003000), 0x00);
    EQ(mem_r8(c, 0x80003001), 0x73);
    EQ(mem_r16(c, 0x80003002), (uint16_t)~0x4008);
    EQ(mem_r8(c, 0x80003004), 0x30);
    EQ(mem_r8(c, 0x80003007), 0x20);
    EQ(mem_r8(c, 0x80003100), 0xFF);              /* puerto 2 vacío */
    c->r[4] = 0; hle_ctl_state(c); EQ(c->r[2], 6);
    c->r[4] = 0x10; hle_ctl_state(c); EQ(c->r[2], 0);
    pad_set_type(0, PAD_DIGITAL); pad_frame(c);
    EQ(mem_r8(c, 0x80003001), 0x41);
    pad_set_type(0, PAD_ANALOG);
    /* modo BIOS */
    bios(c, 0xB0, 0x12, 0x80003200, 34, 0x80003300, 34);
    bios(c, 0xB0, 0x13, 0, 0, 0, 0);
    EQ(mem_r8(c, 0x80003201), 0x73);
}

static void t_card(Cpu *c) {
    char path[] = "/tmp/mv_test_mc_XXXXXX";
    int fd = mkstemp(path); close(fd); unlink(path);
    EQ(card_open(0, path), 0);
    uint32_t ev_sw = bios(c, 0xB0, 0x08, EV_MC_SW, EVS_IOE, 0x2000, 0);
    bios(c, 0xB0, 0x0C, ev_sw, 0, 0, 0);
    bios(c, 0xA0, 0xAB, 0x00, 0, 0, 0);          /* info de tarjeta: tarjeta nueva -> NEWCARD, no IOE */
    EQ(bios(c, 0xB0, 0x0B, ev_sw, 0, 0, 0), 0);
    bios(c, 0xB0, 0x50, 0x00, 0, 0, 0);          /* marcar tarjeta no nueva */
    bios(c, 0xA0, 0xAB, 0x00, 0, 0, 0);
    EQ(bios(c, 0xB0, 0x0B, ev_sw, 0, 0, 0), 1);

    put_str(c, 0x80004000, "bu00:PRUEBA01");
    uint32_t f = bios(c, 0xB0, 0x32, 0x80004000, (1u << 16) | 0x200, 0, 0);
    EQ((int32_t)f >= 2, 1);
    bios(c, 0xB0, 0x36, f, 0, 0, 0);
    f = bios(c, 0xB0, 0x32, 0x80004000, 0x8002, 0, 0);
    for (int i = 0; i < 256; i++) mem_w8(c, 0x80005000 + (uint32_t)i, (uint8_t)i);
    EQ(bios(c, 0xB0, 0x35, f, 0x80005000, 256, 0), 256);
    EQ(bios(c, 0xB0, 0x0B, ev_sw, 0, 0, 0), 1);  /* evento de fin asíncrono */
    bios(c, 0xB0, 0x36, f, 0, 0, 0);
    f = bios(c, 0xB0, 0x32, 0x80004000, 0x0001, 0, 0);
    EQ(bios(c, 0xB0, 0x34, f, 0x80006000, 128, 0), 128);
    EQ(mem_r8(c, 0x8000607F), 0x7F);
    bios(c, 0xB0, 0x36, f, 0, 0, 0);

    put_str(c, 0x80004100, "bu00:*");
    uint32_t de = bios(c, 0xB0, 0x42, 0x80004100, 0x80007000, 0, 0);
    EQ(de, 0x80007000);
    EQ(mem_r8(c, 0x80007000), 'P');
    EQ(mem_r32(c, 0x80007018), 8192);
    EQ(bios(c, 0xB0, 0x43, 0x80007000, 0, 0, 0), 0);

    /* sondeo de escritura (sector 0x3F, búfer NULL): confirma con evento pero NO copia RAM[0..127] sobre el sector 63 */
    {
        uint8_t hdr[128], now[128];
        bios(c, 0xB0, 0x4F, 0x00, 0x3F, 0x80008000, 0);               /* leer sector 63 */
        for (int j = 0; j < 128; j++) hdr[j] = mem_r8(c, 0x80008000u + (uint32_t)j);
        EQ(hdr[0], 'M'); EQ(hdr[1], 'C');                               /* copia de la cabecera */
        for (int j = 0; j < 128; j++) mem_w8(c, (uint32_t)j, 0xEE);   /* RAM baja distinta de la cabecera */
        bios(c, 0xB0, 0x0C, ev_sw, 0, 0, 0);
        EQ(bios(c, 0xB0, 0x4E, 0x00, 0x3F, 0, 0), 1);
        EQ(bios(c, 0xB0, 0x0B, ev_sw, 0, 0, 0), 1);                    /* el evento de fin se entrega igual */
        bios(c, 0xB0, 0x4F, 0x00, 0x3F, 0x80008100, 0);
        for (int j = 0; j < 128; j++) now[j] = mem_r8(c, 0x80008100u + (uint32_t)j);
        EQ(memcmp(hdr, now, 128), 0);
        EQ(bios(c, 0xB0, 0x4E, 0x00, 0x3F, 0x80000000, 0), 1);         /* mismo sondeo por KSEG0 */
        bios(c, 0xB0, 0x4F, 0x00, 0x3F, 0x80008100, 0);
        for (int j = 0; j < 128; j++) now[j] = mem_r8(c, 0x80008100u + (uint32_t)j);
        EQ(memcmp(hdr, now, 128), 0);
        for (int j = 0; j < 128; j++) mem_w8(c, 0x80008200u + (uint32_t)j, 0x5A);   /* escritura legítima con búfer real: sí se copia */
        EQ(bios(c, 0xB0, 0x4E, 0x00, 0x3F, 0x80008200, 0), 1);
        bios(c, 0xB0, 0x4F, 0x00, 0x3F, 0x80008100, 0);
        EQ(mem_r8(c, 0x80008100), 0x5A);
        EQ(mem_r8(c, 0x8000817F), 0x5A);
        for (int j = 0; j < 128; j++) mem_w8(c, 0x80008200u + (uint32_t)j, hdr[j]);   /* restaurar la cabecera */
        bios(c, 0xB0, 0x4E, 0x00, 0x3F, 0x80008200, 0);
        for (int j = 0; j < 128; j++) mem_w8(c, (uint32_t)j, 0);
    }

    /* persistencia: reabrir la imagen */
    card_close(0);
    card_open(0, path);
    de = bios(c, 0xB0, 0x42, 0x80004100, 0x80007000, 0, 0);
    EQ(de, 0x80007000);
    EQ(bios(c, 0xB0, 0x45, 0x80004000, 0, 0, 0), 1); /* erase */
    EQ(bios(c, 0xB0, 0x42, 0x80004100, 0x80007000, 0, 0), 0);
    FILE *fp = fopen(path, "rb");
    fseek(fp, 0, SEEK_END); EQ(ftell(fp), 0x20000); fclose(fp);
    card_close(0);
    unlink(path);
    bios(c, 0xB0, 0x09, ev_sw, 0, 0, 0);
}


/* Ticks del contador transcurridos tras e ciclos (mismas fuentes de reloj que el modelo de io.c). */
static uint64_t tm_ticks(int n, uint32_t src, uint64_t e) {
    if (n == 1 && (src & 1)) return e * 314 / (33868800u / 50);
    if (n == 2 && (src & 2)) return e / 8;
    if (n == 0 && (src & 1)) return e * 2 / 11;
    return e;
}

/* Avanza el reloj hasta que la IRQ del contador n se levante; devuelve los ciclos transcurridos desde t0. */
static uint64_t tm_wait(Cpu *c, int n, uint64_t t0) {
    uint32_t bit = 1u << (IRQ_TMR0 + n);
    for (int i = 0; i < 400000 && !(sys_irq_stat() & bit); i++) sys_idle(c);
    EQ((sys_irq_stat() & bit) != 0, 1);
    mem_w32(c, 0x1F801070, ~bit);
    return sys_cycles() - t0;
}

/* La IRQ debe saltar en el primer ciclo en que el contador alcanza el valor (ni antes ni después). */
static void t_timer_modes(Cpu *c) {
    static const uint32_t flags[] = {0x10, 0x20, 0x38};
    const uint32_t tg = 100;
    for (int n = 0; n < 3; n++)
        for (int src = 0; src < 4; src++)
            for (int f = 0; f < 3; f++)
                for (int rep = 0; rep < 2; rep++) {
                    uint32_t o = 0x1F801100u + 0x10u * (uint32_t)n, mode = flags[f] | ((uint32_t)src << 8) | (rep ? 0x40u : 0);
                    mem_w32(c, o + 4, 0);
                    mem_w32(c, 0x1F801070, ~(1u << (IRQ_TMR0 + n)));
                    mem_w32(c, o + 8, tg);
                    uint64_t t0 = sys_cycles();
                    mem_w32(c, o + 4, mode);
                    int reset = (mode & 8) != 0;
                    uint64_t period = reset ? tg + 1 : 0x10000, want = (mode & 0x10) ? tg : 0x10000;
                    if (!(mode & 0x10) && reset) want = 0x10000;   /* solo desbordamiento con reset en objetivo: no llega a 0xFFFF */
                    uint64_t e = tm_wait(c, n, t0);
                    if (!(mode & 0x10) && reset) continue;          /* caso sin IRQ prevista: el bucle de espera ya falló si no la hubo */
                    if (tm_ticks(n, (uint32_t)src, e) < want || tm_ticks(n, (uint32_t)src, e - 1) >= want) {
                        printf("FALLO timer n=%d src=%d modo=0x%x: IRQ a %llu ciclos (ticks %llu, esperado %llu)\n", n, src, (unsigned)mode,
                               (unsigned long long)e, (unsigned long long)tm_ticks(n, (uint32_t)src, e), (unsigned long long)want);
                        fails++;
                    }
                    if (rep) {
                        uint64_t want2 = (mode & 0x10) ? want + period : 0x20000;
                        uint64_t e2 = tm_wait(c, n, t0);
                        if (tm_ticks(n, (uint32_t)src, e2) < want2 || tm_ticks(n, (uint32_t)src, e2 - 1) >= want2) {
                            printf("FALLO timer repetido n=%d src=%d modo=0x%x: 2.ª IRQ a ticks %llu, esperado %llu\n", n, src, (unsigned)mode,
                                   (unsigned long long)tm_ticks(n, (uint32_t)src, e2), (unsigned long long)want2);
                            fails++;
                        }
                    }
                    mem_w32(c, o + 4, 0);
                    mem_w32(c, 0x1F801070, 0);
                }
}

/* IRQ sin manejador ni cola: se reconoce sin montar la excepción. */
extern unsigned long irq_exceptions;
static void t_irq_sin_manejador(Cpu *c) {
    unsigned long antes = irq_exceptions;
    sys_irq_enable(IRQ_GPU, 1);
    sys_irq_raise(IRQ_GPU);
    rt_irq_check(c);
    EQ(sys_irq_stat() & (1u << IRQ_GPU), 0);
    EQ(irq_exceptions, antes);
}

/* Codificación mínima de MIPS para el intérprete. */
#define LUI(rt, i)     (0x3C000000u | (rt) << 16 | (i))
#define ORI(rt, rs, i) (0x34000000u | (rs) << 21 | (rt) << 16 | (i))
#define ADDIU(rt, rs, i) (0x24000000u | (rs) << 21 | (rt) << 16 | ((i) & 0xFFFF))
#define LW(rt, b, o)   (0x8C000000u | (b) << 21 | (rt) << 16 | ((o) & 0xFFFF))
#define SW(rt, b, o)   (0xAC000000u | (b) << 21 | (rt) << 16 | ((o) & 0xFFFF))
#define SLTIU(rt, rs, i) (0x2C000000u | (rs) << 21 | (rt) << 16 | (i))
#define BNE(rs, rt, o) (0x14000000u | (rs) << 21 | (rt) << 16 | ((o) & 0xFFFF))
#define OR(rd, rs, rt) ((rs) << 21 | (rt) << 16 | (rd) << 11 | 0x25u)
#define JR(rs)         ((rs) << 21 | 8u)
#define JALR(rs)       ((rs) << 21 | 31u << 11 | 9u)

/* cambiar de hilo desde un manejador de frame_wait (dentro de la excepción): es diferido y no deja el estado de
 * excepción bloqueado. El hilo 0 gira hasta que el contador (que incrementa el hilo 1) llega a N; el
 * manejador manda al hilo 1 mientras no, y de vuelta al 0 después. */
static void t_cambio_hilo_en_excepcion(Cpu *c) {
    enum { N = 1000, CNT = 0x80020000, H = 0x80010000, A = 0x80010200, B = 0x80010100 };
    uint32_t h[] = {
        ADDIU(29, 29, -8), SW(31, 29, 0),
        LUI(8, 0x8002), LW(11, 8, 0), SLTIU(11, 11, N),     /* t3 = contador < N */
        LUI(4, 0xFF00), OR(4, 4, 11),                       /* a0 = hilo 1 si t3, hilo 0 si no */
        ORI(10, 0, 0xB0), JALR(10), ORI(9, 0, 0x10),        /* cambiar de hilo */
        LW(31, 29, 0), ADDIU(29, 29, 8), JR(31), 0};
    uint32_t b[] = {LUI(8, 0x8002), LW(9, 8, 0), ADDIU(9, 9, 1), SW(9, 8, 0), BNE(0, 9, -4 & 0xFFFF), 0};
    uint32_t a[] = {LUI(8, 0x8002), LW(9, 8, 0), SLTIU(10, 9, N), BNE(10, 0, -3), 0, JR(31), 0};
    for (unsigned i = 0; i < sizeof h / 4; i++) mem_w32(c, H + 4 * i, h[i]);
    for (unsigned i = 0; i < sizeof b / 4; i++) mem_w32(c, B + 4 * i, b[i]);
    for (unsigned i = 0; i < sizeof a / 4; i++) mem_w32(c, A + 4 * i, a[i]);
    mem_w32(c, CNT, 0);
    uint32_t th = bios(c, 0xB0, 0x0E, B, 0x801F0000, 0, 0);
    EQ(th, 0xFF000001u);
    unsigned long antes = irq_exceptions;
    irq_set_vsync_callback(H);
    c->r[29] = 0x801FFF00;
    rt_call(c, A);                                          /* vuelve solo si el hilo 0 recuperó el control */
    irq_set_vsync_callback(0);
    EQ(mem_r32(c, CNT) >= N, 1);
    EQ(irq_exceptions - antes >= 2, 1);                     /* una excepción en cada hilo */
    EQ(fiber_current(), 0xFF000000u);
    /* TCB del hilo interrumpido: SR con IEc/IM2 y CAUSE con IP2. */
    uint32_t tcb = mem_r32(c, 0x80000110);
    EQ(mem_r32(c, tcb + 0x94) & 0x401, 0x401);
    EQ(mem_r32(c, tcb + 0x98) & 0x400, 0x400);
}

/* El IRQ por dirección de la SPU debe saltar con el reloj virtual (sin hilo de audio). */
static void t_spu_clock(Cpu *c) {
    uint8_t *ram = spu_ram();
    memset(&ram[0x1000], 0, 512);
    ram[0x1000 + 1] = 0x07;                      /* flags: inicio de bucle */
    ram[0x1000 + 16 + 1] = 0x03;
    spu_io_write(0x1F801DAA, 0xC000, 2);         /* SPU activa, sin IRQ */
    spu_io_write(0x1F801D80, 0x3FFF, 2);
    spu_io_write(0x1F801D82, 0x3FFF, 2);
    spu_io_write(0x1F801C00, 0x3FFF, 2);
    spu_io_write(0x1F801C02, 0x3FFF, 2);
    spu_io_write(0x1F801C04, 0x1000, 2);         /* pitch = 44100 Hz */
    spu_io_write(0x1F801C06, 0x1000 / 8, 2);     /* dirección inicial */
    spu_io_write(0x1F801C08, 0x00FF, 2);
    spu_io_write(0x1F801C0A, 0x0000, 2);
    spu_io_write(0x1F801DA4, 0x1000 / 8, 2);     /* dirección de IRQ */
    spu_io_write(0x1F801D88, 1, 2);              /* key-on voz 0 */
    int hits = 0;
    for (int k = 0; k < 3; k++) {
        spu_io_write(0x1F801DAA, 0xC000, 2);     /* apaga IRQ: limpia el flag */
        mem_w32(c, 0x1F801070, ~(1u << IRQ_SPU));
        spu_io_write(0x1F801DAA, 0xC040, 2);     /* enciende IRQ */
        EQ((spu_io_read(0x1F801DAE, 2) & 0x40), 0);
        sys_frame(c);
        if (mem_r32(c, 0x1F801070) & (1u << IRQ_SPU)) hits++;
    }
    EQ(hits, 3);
    int16_t o[64 * 2];                           /* estéreo: 2 muestras por trama */
    EQ(spu_pull(o, 64), 64);
}

/* Anillo SPSC de salida de la SPU: infraejecución con fundido y silencio hasta el prefill, sin repetir; desbordamiento descarta lo nuevo. */
static void t_spu_ring(void) {
    int16_t o[512 * 2];
    SpuAudioStats s0, s1;
    int n, it = 0;
    do { n = spu_pull(o, 512); } while (n == 512 && ++it < 100);   /* vacía hasta la infraejecución */
    EQ(n < 512, 1);
    spu_audio_stats(&s1, 0);
    EQ(s1.underruns >= 1, 1);
    for (int i = n; i < 512; i++) { /* tras la cola de fundido solo hay ceros */
        if (i - n >= 128) { EQ(o[i * 2], 0); EQ(o[i * 2 + 1], 0); }
    }
    spu_clock(32 * 10);                          /* 320 < prefill (1764): sigue en silencio y NO consume */
    EQ(spu_pull(o, 64), 0);
    EQ(spu_ring_level(), 320);
    for (int k = 0; k < 6; k++) spu_clock(256);  /* 1856 >= prefill */
    EQ(spu_pull(o, 64), 64);                     /* reanuda */
    spu_audio_stats(&s0, 0);
    for (int k = 0; k < 40; k++) spu_clock(512); /* 20480 > 16384: desborda */
    spu_audio_stats(&s1, 0);
    EQ(spu_ring_level(), 16384);
    EQ(s1.dropped > s0.dropped, 1);
    EQ(s1.produced - s0.produced, 20480);
}

int main(void) {
    Cpu c = {0};
    c.r[29] = 0x801FFF00;
    t_irq(&c);
    t_timer(&c);
    t_dma_otc(&c);
    t_events(&c);
    t_pad(&c);
    t_card(&c);
    t_timer_modes(&c);
    t_spu_clock(&c);
    t_spu_ring();
    t_irq_sin_manejador(&c);
    t_cambio_hilo_en_excepcion(&c);
    printf(fails ? "test_sys: %d FALLOS\n" : "test_sys: OK\n", fails);
    return fails != 0;
}
