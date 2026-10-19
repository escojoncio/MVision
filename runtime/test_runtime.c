/* Pruebas unitarias del runtime (sin datos del juego). */
#include <stdio.h>
#include <stdint.h>
#include "cpu.h"
#include <string.h>
#include "gte_vectors.h"
#include "gpu.h"
#include "hle.h"

const FuncEntry rt_func_table[] = {{0, 0}};
const unsigned rt_func_count = 0;
const FuncName rt_func_names[] = {{0xFFFFFFFFu, ""}};
const unsigned rt_func_name_count = 0;
const uint32_t rt_entry = 0;

static int fails;
#define EQ(a, b) do { unsigned long long _a = (a), _b = (b); \
    if (_a != _b) { printf("FALLO %s:%d %s = 0x%llx, esperado 0x%llx\n", __FILE__, __LINE__, #a, _a, _b); fails++; } } while (0)

static void t_mem(Cpu *c) {
    mem_w32(c, 0x80001000, 0x44332211);
    mem_w32(c, 0x80001004, 0x88776655);
    EQ(mem_r32(c, 0x00001000), 0x44332211);         /* espejo KUSEG */
    EQ(mem_r32(c, 0xA0601000), 0x44332211);         /* espejo KSEG1 + 6 MB */
    EQ(mem_r16(c, 0x80001002), 0x4433);
    /* lwr/lwl para leer 0x80001001 no alineado = 0x55443322 */
    uint32_t v = mem_lwr(c, 0x80001001, 0xAAAAAAAA);
    v = mem_lwl(c, 0x80001004, v);
    EQ(v, 0x55443322);
    /* swr/swl para escribir 0xDDCCBBAA en 0x80001001 */
    mem_swr(c, 0x80001001, 0xDDCCBBAA);
    mem_swl(c, 0x80001004, 0xDDCCBBAA);
    EQ(mem_r32(c, 0x80001000), 0xCCBBAA11);
    EQ(mem_r32(c, 0x80001004), 0x887766DD);
    mem_w8(c, 0x1F800010, 0x7F);
    EQ(mem_r8(c, 0x1F800010), 0x7F);
}

static void t_div(Cpu *c) {
    rt_div(c, (uint32_t)-7, 2); EQ(c->lo, (uint32_t)-3); EQ(c->hi, (uint32_t)-1);
    rt_div(c, 5, 0); EQ(c->lo, 0xFFFFFFFF); EQ(c->hi, 5);
    rt_div(c, (uint32_t)-5, 0); EQ(c->lo, 1);
    rt_div(c, 0x80000000u, (uint32_t)-1); EQ(c->lo, 0x80000000u); EQ(c->hi, 0);
    rt_divu(c, 7, 0); EQ(c->lo, 0xFFFFFFFF); EQ(c->hi, 7);
}

static void t_gte(Cpu *c) {
    gte_reset();
    gte_ctc2(c, 0, 0x1000); gte_ctc2(c, 1, 0); gte_ctc2(c, 2, 0x1000); gte_ctc2(c, 3, 0); gte_ctc2(c, 4, 0x1000);
    gte_ctc2(c, 5, 0); gte_ctc2(c, 6, 0); gte_ctc2(c, 7, 1000);
    gte_ctc2(c, 24, 0); gte_ctc2(c, 25, 0); gte_ctc2(c, 26, 1000);
    gte_mtc2(c, 0, (50u << 16) | 100); gte_mtc2(c, 1, 0);
    gte_cmd(c, 0x00180001); /* RTPS sf=1 */
    EQ(gte_mfc2(c, 19), 1000);                     /* SZ3 */
    EQ(gte_mfc2(c, 14), (50u << 16) | 100);         /* SXY2 */
    EQ(gte_cfc2(c, 31), 0);                         /* FLAG */
    EQ(gte_cfc2(c, 26), 1000);                      /* H se lee con signo */
    /* Proyección con división: H=500, Z=1000 -> 0.5 */
    gte_ctc2(c, 26, 500);
    gte_cmd(c, 0x00180001);
    EQ(gte_mfc2(c, 14), (25u << 16) | 50);
    /* Desbordamiento de división: Z pequeño */
    gte_ctc2(c, 7, 100); gte_ctc2(c, 26, 1000);
    gte_cmd(c, 0x00180001);
    EQ((gte_cfc2(c, 31) >> 17) & 1, 1);
    EQ(gte_cfc2(c, 31) >> 31, 1);
    /* NCLIP: triángulo CCW (0,0) (10,0) (0,10) -> 100 */
    gte_mtc2(c, 12, 0); gte_mtc2(c, 13, 10); gte_mtc2(c, 14, 10u << 16);
    gte_cmd(c, 0x00000006);
    EQ(gte_mfc2(c, 24), 100);
    /* AVSZ3 */
    gte_mtc2(c, 17, 300); gte_mtc2(c, 18, 600); gte_mtc2(c, 19, 900);
    gte_ctc2(c, 29, 0x555);                         /* ~1/3 en 1.12 */
    gte_cmd(c, 0x0000002D);
    EQ(gte_mfc2(c, 7), (0x555 * 1800) >> 12);
    /* LZCS/LZCR */
    gte_mtc2(c, 30, 0x00F00000); EQ(gte_mfc2(c, 31), 8);
    gte_mtc2(c, 30, 0xFFFF0000); EQ(gte_mfc2(c, 31), 16);
    /* IRGB -> IR */
    gte_mtc2(c, 28, 0x7FFF); EQ(gte_mfc2(c, 9), 0xF80); EQ(gte_mfc2(c, 29), 0x7FFF);
    /* SQR */
    gte_mtc2(c, 9, 0x10); gte_mtc2(c, 10, (uint32_t)-0x20); gte_mtc2(c, 11, 0x7FFF);
    gte_cmd(c, 0x00000428); /* sf=0, lm=1 */
    EQ(gte_mfc2(c, 9), 0x100); EQ(gte_mfc2(c, 10), 0x400); EQ(gte_mfc2(c, 11), 0x7FFF);
    EQ((gte_cfc2(c, 31) >> 22) & 1, 1);            /* IR3 saturado */
}

/* ---- vectores diferenciales de la GTE ----
 * Cada caso se reconstruye de forma determinista desde (cmd, semilla): se rellenan los registros de
 * datos y control con valores pseudoaleatorios (xorshift64), se ejecuta el comando y se compara un hash
 * FNV-1a de los 64 registros de salida (datos vía mfc2 y control vía cfc2, incluido FLAG), más FLAG y
 * MAC0 en claro para facilitar el diagnóstico. Los valores esperados (gte_vectors.h) son una instantánea
 * del comportamiento verificado de gte.c: detectan regresiones. */
static uint64_t gv_s;
static uint32_t gv_rnd(void) { gv_s ^= gv_s << 13; gv_s ^= gv_s >> 7; gv_s ^= gv_s << 17; return (uint32_t)(gv_s >> 16); }
static uint32_t gv_gen(int mode) {
    uint32_t r = gv_rnd();
    static const uint32_t ext[16] = {0, 1, 0xFFFFFFFFu, 0x7FFF, 0x8000, 0xFFFF8000u, 0x7FFFFFFFu, 0x80000000u,
                                     0xFFFF, 0x10000, 0x1000, 0xFFFFF000u, 0x7FFF0000u, 0x80000000u, 0x8000, 0x0FFF};
    switch (mode) {
    case 0: return r;
    case 1: return (uint32_t)(int32_t)(int16_t)r;
    case 2: return ext[r & 15];
    case 3: return (uint32_t)((int32_t)(r % 0x2001) - 0x1000);
    default: return (uint32_t)((int32_t)(r % 65536) - 32768) * ((gv_rnd() & 1) ? 0x10001u : 1u);
    }
}
static uint32_t gv_run(Cpu *c, uint32_t cmd, uint32_t seed, uint32_t *flag, uint32_t *mac0) {
    gv_s = 88172645463325252ull ^ ((uint64_t)seed * 0x9E3779B97F4A7C15ull);
    gte_reset();
    static const int modes[8] = {3, 3, 3, 1, 4, 0, 2, 3};
    int mode = modes[seed & 7];
    for (int r = 0; r < 31; r++) gte_ctc2(c, r, gv_gen((gv_rnd() & 3) == 0 ? (int)(gv_rnd() % 5) : mode));
    for (int r = 0; r < 31; r++) {
        if (r == 15 || r == 28 || r == 29) continue;
        gte_mtc2(c, r, gv_gen((gv_rnd() & 3) == 0 ? (int)(gv_rnd() % 5) : mode));
    }
    if (gv_rnd() & 1) gte_mtc2(c, 28, gv_rnd());
    if (gv_rnd() & 1) {
        uint32_t hv = gv_rnd() % 0x2000, zr = 16 + gv_rnd() % 4, zv = gv_rnd() % 0x3000;   /* orden fijo */
        gte_ctc2(c, 26, hv); gte_mtc2(c, (int)zr, zv);
    }
    gte_cmd(c, cmd);
    uint32_t h = 2166136261u;
    for (int r = 0; r < 64; r++) {
        uint32_t v = r < 32 ? gte_mfc2(c, r) : gte_cfc2(c, r - 32);
        for (int b = 0; b < 4; b++) { h ^= (v >> (8 * b)) & 0xFF; h *= 16777619u; }
    }
    *flag = gte_cfc2(c, 31);
    *mac0 = gte_mfc2(c, 24);
    return h;
}

static void t_gte_vec(Cpu *c) {
    unsigned n = sizeof gte_vectors / sizeof gte_vectors[0];
    int malos = 0;
    for (unsigned i = 0; i < n; i++) {
        uint32_t flag, mac0;
        uint32_t h = gv_run(c, gte_vectors[i].cmd, gte_vectors[i].seed, &flag, &mac0);
        if (flag != gte_vectors[i].flag || mac0 != gte_vectors[i].mac0 || h != gte_vectors[i].hash) {
            if (malos++ < 8)
                printf("FALLO gte vector %u cmd=%08x: flag=%08x/%08x mac0=%08x/%08x hash=%08x/%08x\n", i,
                       gte_vectors[i].cmd, flag, gte_vectors[i].flag, mac0, gte_vectors[i].mac0, h,
                       gte_vectors[i].hash);
        }
    }
    EQ(malos, 0);
}

/* Los registros de 16 bits ignoran la mitad alta al escribirlos (p.ej. lwc2 de una palabra con dos
 * halfwords): los cálculos no deben verla. */
static void t_gte_regs16(Cpu *c) {
    gte_reset();
    gte_lwc2(c, 9, 0x00010003);                    /* IR1 = 3 aunque la mitad alta valga 1 */
    gte_lwc2(c, 10, 0xFFFF8000u);                  /* IR2 = -32768 */
    gte_lwc2(c, 11, 0x7FFF0004);                   /* IR3 = 4 */
    EQ(gte_mfc2(c, 9), 3); EQ(gte_mfc2(c, 10), 0xFFFF8000u); EQ(gte_mfc2(c, 11), 4);
    gte_cmd(c, 0x00000028);                        /* SQR sf=0 */
    EQ(gte_mfc2(c, 25), 9); EQ(gte_mfc2(c, 26), 0x40000000u); EQ(gte_mfc2(c, 27), 16);
    gte_lwc2(c, 8, 0x12341000);                    /* IR0 = 0x1000 */
    gte_lwc2(c, 19, 0xABCD0100);                   /* SZ3 = 0x100 sin signo */
    EQ(gte_mfc2(c, 8), 0x1000); EQ(gte_mfc2(c, 19), 0x100);
    gte_lwc2(c, 7, 0xFFFF8000u); EQ(gte_mfc2(c, 7), 0x8000);   /* OTZ sin signo */
    gte_lwc2(c, 1, 0xFFFF8000u); EQ(gte_mfc2(c, 1), 0xFFFF8000u); /* VZ0 con signo */
    gte_lwc2(c, 16, 0x7FFF0010);                   /* SZ0 */
    gte_lwc2(c, 17, 0x00010020); gte_lwc2(c, 18, 0x00010030);
    gte_ctc2(c, 29, 0x1000);
    gte_cmd(c, 0x0000002D);                        /* AVSZ3 con SZ1..3 = 0x20,0x30,0x100 */
    EQ(gte_mfc2(c, 7), 0x20 + 0x30 + 0x100);
}

/* Captura de realimentación: primitivas texturizadas de 1 texel que leen el frame buffer (GpuOp.fbk/fbt, gpu.h). */
static uint32_t pxy(int x, int y) { return (uint32_t)(x & 0xFFFF) | (uint32_t)(y & 0xFFFF) << 16; }
static void fb_quad(uint32_t cmd, uint32_t col, int x0, int y0, int x1, int y1, int u1, int v1, uint32_t clut, uint32_t tpage) {
    gpu_gp0(cmd << 24 | col); gpu_gp0(pxy(x0, y0)); gpu_gp0(clut << 16);
    gpu_gp0(pxy(x1, y0)); gpu_gp0(tpage << 16 | (uint32_t)u1);
    gpu_gp0(pxy(x0, y1)); gpu_gp0((uint32_t)v1 << 8);
    gpu_gp0(pxy(x1, y1)); gpu_gp0((uint32_t)v1 << 8 | (uint32_t)u1);
}
static uint32_t fb_hash(void) { uint32_t h = 2166136261u; for (int y = 0; y < 512; y++) for (int x = 0; x < 1024; x++) { h ^= gpu_vram[y][x]; h *= 16777619u; } return h; }
/* Dibuja la secuencia de la prueba; con cap=1 devuelve las ops en `out` (copiadas). */
static uint32_t fb_seq(int cap, GpuOp *out, uint32_t *nout) {
    gpu_reset();
    memset(gpu_vram, 0, sizeof gpu_vram);
    if (cap) gpu_capture_enable(1);
    gpu_gp0(0xE3000000); gpu_gp0(0xE4000000 | 319 | 239 << 10); gpu_gp0(0xE5000000);
    gpu_vram[0][0] = 0xC3E0;                 /* texel 15 bits en el frame buffer: bit15=1, g=31, b=16 */
    gpu_vram[0][1] = 0x0000;                 /* transparente */
    gpu_vram[0][2] = 0x2108;                 /* texel distinto */
    gpu_vram[0][512] = 0x0033;               /* 4 bits, página x=512 (fuera del frame buffer): índice 3 en u=0 */
    gpu_vram[10][3] = 0x7C00;                /* CLUT (0,10) dentro del frame buffer */
    gpu_vram[0][704] = 0x6666;               /* textura legítima fuera del frame buffer (no transparente: si se resolviera por error saldría fbk=1) */
    gpu_vram[0][704 + 1] = 0x6666;
    gpu_vram[30][2] = 0x4321;                /* CLUT de 8 bits (0,30) dentro del frame buffer */
    gpu_vram[0][8] = gpu_vram[1][8] = 0x5555; /* destino de la ventana de textura */
    gpu_vram[0][3] = gpu_vram[0][4] = 0x7777; /* para el sprite volteado */
    /* 0: 15 bits, quad 40x8 con uv 0..1 -> texel (0,0) en el frame buffer; SEMI, modulado por color 0x80 80 40 */
    fb_quad(0x2E, 0x408080, 80, 100, 120, 108, 1, 1, 0, 0x100 | 0x20);
    /* 1: 4 bits, texel fuera del frame buffer pero CLUT dentro (clut x=0,y=10 -> 0x000 | 10<<6) */
    fb_quad(0x2C, 0x808080, 80, 120, 120, 128, 1, 1, 10 << 6, 8 | 0x00 | 0 << 7);
    /* 2: 15 bits, texel 0 (transparente) en el frame buffer: u=1 y uv 1..1 (todos los vértices en (1,0)) */
    gpu_gp0(0x2C000000 | 0x808080); gpu_gp0(pxy(80, 140)); gpu_gp0(1); gpu_gp0(pxy(120, 140)); gpu_gp0(0x100 << 16 | 1);
    gpu_gp0(pxy(80, 148)); gpu_gp0(1); gpu_gp0(pxy(120, 148)); gpu_gp0(1);
    /* 3: 15 bits, textura fuera del frame buffer (tpage x=704 = 11*64) 1 texel: NO se resuelve */
    gpu_gp0(0x2C808080); gpu_gp0(pxy(80, 160)); gpu_gp0(0); gpu_gp0(pxy(120, 160)); gpu_gp0((0x100u | 11) << 16 | 0);
    gpu_gp0(pxy(80, 168)); gpu_gp0(0); gpu_gp0(pxy(120, 168)); gpu_gp0(0);
    /* 4: rect 2x1 con dos texels distintos en el frame buffer (u 0..1: 0xC3E0 y 0x0000): no uniforme */
    gpu_gp0(0xE1000100); gpu_gp0(0x64808080); gpu_gp0(pxy(200, 100)); gpu_gp0(0); gpu_gp0(pxy(2, 1) );
    /* 5: rect 1x1 en el frame buffer con raw (0x6D = rect 1x1 textura cruda) en u=2 (0x2108) */
    gpu_gp0(0x6D808080); gpu_gp0(pxy(210, 100)); gpu_gp0(2);
    /* 6: poligono con uv 0..8 en el frame buffer: no es candidato (cubre mas de 2 texels) */
    fb_quad(0x2C, 0x808080, 80, 180, 120, 188, 8, 8, 0, 0x100);
    /* 7: 8 bits, página fuera (x=704) con índice 2 en u=0, CLUT (0,30) en el frame buffer */
    gpu_vram[0][704] = 0x6602;
    fb_quad(0x2C, 0x808080, 80, 200, 120, 208, 0, 0, 30 << 6, 0x80 | 11);
    /* 8: ventana de textura: máscara x total, desplazamiento 1 (u siempre 8); sin ella u=0,1 darían dos valores */
    gpu_gp0(0xE2000000 | 31 | 1 << 10);
    fb_quad(0x2C, 0x808080, 80, 210, 120, 218, 1, 1, 0, 0x100);
    gpu_gp0(0xE2000000);
    /* 9: sprite 2x1 volteado en x, u=4 -> texels 4,3 (ambos 0x7777); sin volteo serían 4,5 (distintos) */
    gpu_gp0(0xE1000100 | 1 << 12); gpu_gp0(0x64808080); gpu_gp0(pxy(200, 120)); gpu_gp0(4); gpu_gp0(pxy(2, 1));
    gpu_gp0(0xE1000100);
    /* 10: el polígono cubre su propio texel (0,0): los píxeles posteriores leerían lo ya dibujado -> no se resuelve */
    fb_quad(0x2C, 0x808080, 0, 0, 8, 8, 1, 1, 0, 0x100);
    gpu_gp1(0x05000000); gpu_gp1(0x05000000 | 100);  /* cierra el frame */
    uint32_t h = fb_hash();
    if (cap) {
        const GpuDrawList *l = gpu_capture_frame();
        if (l) { *nout = l->nops < 16 ? l->nops : 16; memcpy(out, l->ops, *nout * sizeof *out); } else *nout = 0;
        gpu_capture_enable(0);
    }
    return h;
}
static void t_gpu_feedback(void) {
    GpuOp ops[16]; uint32_t n = 0;
    uint32_t h_on = fb_seq(1, ops, &n), h_off = fb_seq(0, NULL, NULL);
    EQ(h_on, h_off);                         /* la sonda no altera el oráculo */
    EQ(n, 11);
    EQ(ops[0].fbk, 1); EQ(ops[0].fbt, 0xC3E0); EQ(ops[0].flags & GPU_F_SEMI, GPU_F_SEMI);
    EQ(ops[1].fbk, 1); EQ(ops[1].fbt, 0x7C00);   /* 4 bits: índice 3 de la página fuera, CLUT en el frame buffer */
    EQ(ops[2].fbk, 1); EQ(ops[2].fbt, 0);        /* transparente */
    EQ(ops[3].fbk, 0);                           /* texel legítimo fuera del frame buffer */
    EQ(ops[4].fbk, 0);                           /* rect 2x1 con 2 valores: no uniforme */
    EQ(ops[5].fbk, 1); EQ(ops[5].fbt, 0x2108);   /* rect 1x1 */
    EQ(ops[6].fbk, 0);                           /* cubre 8 texels */
    EQ(ops[7].fbk, 1); EQ(ops[7].fbt, 0x4321);   /* 8 bits: índice 2, CLUT en el frame buffer */
    EQ(ops[8].fbk, 1); EQ(ops[8].fbt, 0x5555);   /* ventana de textura */
    EQ(ops[9].fbk, 1); EQ(ops[9].fbt, 0x7777);   /* volteo */
    EQ(ops[10].fbk, 0);                          /* se pisa su propia textura */
}

/* Presentación del vídeo: la réplica de PutDispEnv fija el rango horizontal y la lectura del marco respeta lo que muestra el
 * hardware (regresión del códec: modo de 384 con rango de 318 columnas; antes se publicaban 368 con 48 de basura de la VRAM). */
void hle_gfx_put_disp(Cpu *c);
static void put_disp(Cpu *c, int dw, int dh, int sx, int sy, int sw, int sh, int rgb24) {
    uint32_t e = 0x3000;
    int16_t v[8] = {0, 0, (int16_t)dw, (int16_t)dh, (int16_t)sx, (int16_t)sy, (int16_t)sw, (int16_t)sh};
    for (int i = 0; i < 8; i++) mem_w16(c, e + 2u * (unsigned)i, (uint16_t)v[i]);
    mem_w8(c, e + 16, 0); mem_w8(c, e + 17, (uint8_t)rgb24);
    c->r[4] = e;
    hle_gfx_put_disp(c);
}
static uint8_t disp_img[1024 * 512 * 4];
static void t_display(void) {
    Cpu c = {0};
    GpuDisplay d;
    gpu_reset();
    memset(gpu_vram, 0, sizeof gpu_vram);
    gpu_gp1(0x03000000);   /* display encendido */
    /* juego normal: 320x256, rango completo -> marco = área, sin márgenes; rango 0x270..0xC70 y 35..291 */
    put_disp(&c, 320, 256, 0, 16, 256, 256, 0);
    gpu_get_display(&d);
    EQ(d.w, 320); EQ(d.aw, 320); EQ(d.fw, 320); EQ(d.pad_l, 0); EQ(d.h, 256); EQ(d.mode & 0x4F, 0x09);
    EQ(d.hx1, 0x270); EQ(d.hx2, 0xC70); EQ(d.vy1, 35); EQ(d.vy2, 291);
    /* modo de 256 y de 512/640: anchos nominales con screen.w=256 */
    put_disp(&c, 256, 240, 0, 0, 0, 0, 0);
    gpu_get_display(&d); EQ(d.w, 256); EQ(d.aw, 256); EQ(d.fw, 256); EQ(d.pad_l, 0); EQ(d.h, 240);    /* screen.h = 0 -> 240 */
    put_disp(&c, 512, 256, 0, 16, 256, 256, 0);
    gpu_get_display(&d); EQ(d.w, 512); EQ(d.aw, 512); EQ(d.fw, 512);
    put_disp(&c, 640, 256, 0, 16, 256, 256, 0);
    gpu_get_display(&d); EQ(d.w, 640); EQ(d.aw, 640); EQ(d.fw, 640);
    /* radio/códec: disp 384 con screen (26,16,212,256): visibles 318 columnas desplazadas 26 en un marco de 384 */
    put_disp(&c, 384, 256, 26, 16, 212, 256, 0);
    gpu_get_display(&d);
    EQ(d.mode & 0x40, 0x40); EQ(d.w, 384); EQ(d.aw, 318); EQ(d.fw, 384); EQ(d.pad_l, 26); EQ(d.h, 256);
    EQ(d.hx1, 0x230 + 26 * 7); EQ(d.hx2, 0x230 + 26 * 7 + (2688 * 212 >> 8));
    for (int y = 0; y < 256; y++) for (int x = 0; x < 400; x++) gpu_vram[y][x] = (uint16_t)(0x1F);   /* rojo */
    gpu_vram[0][0] = 0x03E0; gpu_vram[0][317] = 0x7C00; gpu_vram[0][318] = 0x7FFF; gpu_vram[0][367] = 0x7FFF; gpu_vram[1][320] = 0x7FFF;
    EQ(gpu_present_rgba(&d, NULL, disp_img, 1024 * 512), 384 * 256);
    const uint8_t *r0 = disp_img;
    #define PIX(x, y) (r0 + 4 * ((y) * 384 + (x)))
    EQ(PIX(0, 0)[0], 0); EQ(PIX(25, 0)[0], 0); EQ(PIX(25, 0)[3], 255);          /* margen izquierdo negro */
    EQ(PIX(26, 0)[1], 255); EQ(PIX(26, 0)[0], 0);                               /* VRAM x=0 (verde) en la columna 26 */
    EQ(PIX(27, 0)[0], 255);                                                     /* VRAM x=1 (rojo) */
    EQ(PIX(26 + 317, 0)[2], 255); EQ(PIX(26 + 317, 0)[0], 0);                   /* última columna visible: VRAM x=317 (azul) */
    EQ(PIX(26 + 318, 0)[0], 0); EQ(PIX(26 + 318, 0)[1], 0); EQ(PIX(26 + 318, 0)[2], 0);   /* x=318 (blanco en VRAM) NO se ve */
    EQ(PIX(383, 0)[0], 0); EQ(PIX(26 + 320, 1)[0], 0);                          /* nada de basura a la derecha */
    #undef PIX
    /* pantalla apagada: marco negro */
    gpu_gp1(0x03000001); gpu_get_display(&d);
    EQ(gpu_present_rgba(&d, NULL, disp_img, 1024 * 512), 384 * 256);
    EQ(disp_img[4 * 27], 0); EQ(disp_img[4 * 27 + 3], 255);
    /* 24 bits: 3 bytes por píxel empaquetados desde disp.x*2 */
    gpu_gp1(0x03000000);
    put_disp(&c, 256, 240, 0, 0, 0, 0, 1);
    gpu_get_display(&d);
    EQ(d.depth24, 1); EQ(d.aw, 256);
    uint8_t *ln = (uint8_t *)gpu_vram[0];
    for (int i = 0; i < 9; i++) ln[i] = (uint8_t)(10 + i);
    gpu_present_rgba(&d, NULL, disp_img, 1024 * 512);
    EQ(disp_img[0], 10); EQ(disp_img[1], 11); EQ(disp_img[2], 12); EQ(disp_img[4], 13); EQ(disp_img[8 + 2], 18);
    /* tras gpu_reset el rango por defecto da 256 columnas */
    gpu_reset(); gpu_get_display(&d); EQ(d.w, 256); EQ(d.aw, 256);
    /* screen.w = 255 (vídeos del juego): 2550 ciclos / 8 = 318,75 -> 319 columnas visibles de 320, la última a medias recortada */
    put_disp(&c, 320, 256, 0, 16, 255, 256, 0);
    gpu_get_display(&d); EQ(d.w, 320); EQ(d.aw, 319); EQ(d.fw, 320); EQ(d.pad_l, 0);
}

int main(void) {
    Cpu c = {0};
    t_mem(&c);
    t_div(&c);
    t_gte(&c);
    t_gte_regs16(&c);
    t_gte_vec(&c);
    t_gpu_feedback();
    t_display();
    printf(fails ? "%d fallos\n" : "runtime: todas las pruebas OK\n", fails);
    return fails != 0;
}
