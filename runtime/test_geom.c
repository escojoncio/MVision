/* Pruebas de la geometría de alta precisión (sombra flotante de la GTE, gte.c) y de su enlace con la captura de GPU.
 * Compilar a mano (el Makefile no la incluye): clang -I. -O2 -o ../build/test_geom <RT sin headless/test_/nogen> test_geom.c -lm
 * Comprueba: (1) la alta precisión NO cambia nada del resultado entero (regs de datos/control tras cada comando, 50k comandos
 * aleatorios con y sin ella); (2) la posición float coincide con la fórmula cerrada; (3) trayectoria suave: con la cámara
 * moviéndose despacio la posición float tiene segunda diferencia ~0 mientras la entera salta +-1 px; (4) envejecimiento por frame,
 * ambigüedad por (sx,sy) y saturación; (5) enlace gpu_capture_hiprec -> GpuVtx.fvalid/fx/fy/fz/vx/vy/vz y offset de dibujo. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "cpu.h"
#include "gpu.h"

const FuncEntry rt_func_table[] = {{0, 0}};
const unsigned rt_func_count = 0;
const FuncName rt_func_names[] = {{0xFFFFFFFFu, ""}};
const unsigned rt_func_name_count = 0;
const uint32_t rt_entry = 0;

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FALLO %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static Cpu cpu;
static uint32_t rs = 12345;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8 ^ rs << 9; }

static uint64_t regs_hash(void) {
    uint64_t h = 1469598103934665603ull;
    for (int r = 0; r < 32; r++) {
        h = (h ^ gte_mfc2(&cpu, r)) * 1099511628211ull;
        h = (h ^ gte_cfc2(&cpu, r)) * 1099511628211ull;
    }
    return h;
}

/* Rotación identidad (1.0 = 0x1000) y proyección típica. */
static void setup_cam(int tx, int ty, int tz, int h, int ofx, int ofy) {
    gte_ctc2(&cpu, 0, 0x1000);            /* R11 */
    gte_ctc2(&cpu, 1, 0);                 /* R13,R21 */
    gte_ctc2(&cpu, 2, 0x1000);            /* R22,R23 -> R22=0x1000 */
    gte_ctc2(&cpu, 3, 0);
    gte_ctc2(&cpu, 4, 0x1000);            /* R33 */
    gte_ctc2(&cpu, 5, (uint32_t)tx); gte_ctc2(&cpu, 6, (uint32_t)ty); gte_ctc2(&cpu, 7, (uint32_t)tz);
    gte_ctc2(&cpu, 24, (uint32_t)ofx << 16); gte_ctc2(&cpu, 25, (uint32_t)ofy << 16);
    gte_ctc2(&cpu, 26, (uint32_t)h);
}
static void load_v0(int x, int y, int z) {
    gte_mtc2(&cpu, 0, (uint32_t)(x & 0xFFFF) | ((uint32_t)y << 16));
    gte_mtc2(&cpu, 1, (uint32_t)(int16_t)z);
}
#define RTPS 0x0180001u
#define RTPT 0x0280030u

static void t_exact_unchanged(void) {
    /* misma secuencia aleatoria con la alta precisión apagada y encendida: los registros deben ser idénticos */
    uint64_t hh[2];
    for (int pass = 0; pass < 2; pass++) {
        gte_reset(); gte_hp_enable(pass);
        rs = 777;
        uint64_t acc = 1469598103934665603ull;
        for (int i = 0; i < 50000; i++) {
            if (i % 50 == 0) {
                for (int r = 0; r < 5; r++) gte_ctc2(&cpu, r, rnd());
                for (int r = 5; r < 8; r++) gte_ctc2(&cpu, r, (rnd() & 0xFFFF) - 0x8000);
                gte_ctc2(&cpu, 24, rnd()); gte_ctc2(&cpu, 25, rnd()); gte_ctc2(&cpu, 26, rnd() & 0xFFFF);
                gte_ctc2(&cpu, 27, rnd()); gte_ctc2(&cpu, 28, rnd());
            }
            for (int r = 0; r < 6; r++) gte_mtc2(&cpu, r, rnd());
            uint32_t cmd = (rnd() & 1) ? RTPT : RTPS;
            if (rnd() & 4) cmd &= ~(1u << 19); /* sf=0 también */
            if (rnd() & 8) cmd |= 1u << 10;    /* lm */
            gte_cmd(&cpu, cmd);
            acc = (acc ^ regs_hash()) * 1099511628211ull;
            if (i % 1000 == 999) gte_hp_frame();
        }
        hh[pass] = acc;
    }
    CHECK(hh[0] == hh[1], "la alta precision altera el resultado entero (0x%llx vs 0x%llx)", (unsigned long long)hh[0], (unsigned long long)hh[1]);
    gte_hp_enable(0);
}

static void t_formula(void) {
    gte_reset(); gte_hp_enable(1);
    setup_cam(100, -50, 1000, 500, 160, 120);
    load_v0(300, 200, 1000);
    gte_cmd(&cpu, RTPS);
    uint32_t sxy = gte_mfc2(&cpu, 15);
    int sx = (int16_t)(sxy & 0xFFFF), sy = (int16_t)(sxy >> 16);
    GpuHpInfo h;
    CHECK(gte_hp_lookup(sx, sy, &h) == 1, "no hay entrada para (%d,%d)", sx, sy);
    /* vista = (400,150,2000) -> x = 160 + 500*400/2000 = 260, y = 120 + 500*150/2000 = 157.5 */
    CHECK(fabsf(h.vx - 400) < 1e-3 && fabsf(h.vy - 150) < 1e-3 && fabsf(h.vz - 2000) < 1e-3, "vista %f %f %f", h.vx, h.vy, h.vz);
    CHECK(fabsf(h.fx - 260.0f) < 1e-3 && fabsf(h.fy - 157.5f) < 1e-3, "pantalla %f %f", h.fx, h.fy);
    CHECK(h.fz == 2000.0f && h.h == 500.0f && h.ofx == 160.0f && h.ofy == 120.0f, "proyeccion");
    CHECK(sx == 260 && (sy == 157 || sy == 158), "entero %d %d", sx, sy);
    /* Z no entero: la GTE trunca SZ, el float conserva la z real */
    gte_ctc2(&cpu, 7, 0);
    gte_ctc2(&cpu, 0, 0x1000 + 0x800); /* R11 = 1.5: vista.x = 1.5*300 + TR.x(100) */
    load_v0(300, 0, 777);
    gte_cmd(&cpu, RTPS);
    sxy = gte_mfc2(&cpu, 15); sx = (int16_t)(sxy & 0xFFFF); sy = (int16_t)(sxy >> 16);
    CHECK(gte_hp_lookup(sx, sy, &h) == 1 && fabsf(h.vx - 550) < 1e-3, "R11 1.5 + TR.x 100: vx=%f", h.vx);
    gte_hp_enable(0);
}

static void t_smooth(void) {
    gte_reset(); gte_hp_enable(1);
    /* cámara que avanza 1 unidad por frame (0,15 px/frame para H=300, z=2000): el entero salta, el float es lineal */
    double xi[64], xf[64];
    int n = 60;
    for (int f = 0; f < n; f++) {
        setup_cam(f, 0, 2000, 300, 160, 120);
        load_v0(500, 0, 0);
        gte_mtc2(&cpu, 1, 0);
        gte_cmd(&cpu, RTPS);
        uint32_t sxy = gte_mfc2(&cpu, 15);
        int sx = (int16_t)(sxy & 0xFFFF), sy = (int16_t)(sxy >> 16);
        GpuHpInfo h;
        CHECK(gte_hp_lookup(sx, sy, &h) == 1, "frame %d sin entrada", f);
        xi[f] = sx; xf[f] = h.fx;
        gte_hp_frame();
    }
    double mi = 0, mf = 0; int big = 0;
    for (int f = 1; f + 1 < n; f++) {
        double di = fabs(xi[f + 1] - 2 * xi[f] + xi[f - 1]), df = fabs(xf[f + 1] - 2 * xf[f] + xf[f - 1]);
        if (di > mi) mi = di;
        if (df > mf) mf = df;
        if (di >= 1.0) big++;
    }
    printf("[geom] suavidad: max|d2| entero=%.3f px (%d de %d saltos >=1 px), float=%.6f px\n", mi, big, n - 2, mf);
    CHECK(mf < 1e-3, "el float no es suave: %f", mf);
    CHECK(mi >= 1.0 && big >= 4, "el entero deberia saltar (max d2 %f, saltos %d)", mi, big);
    gte_hp_enable(0);
}

static void t_table(void) {
    gte_reset(); gte_hp_enable(1);
    GpuHpInfo h;
    setup_cam(0, 0, 0, 500, 160, 120);
    /* dos vértices en el mismo píxel con distinta z: gana el último */
    load_v0(0, 0, 1000); gte_cmd(&cpu, RTPS);
    load_v0(0, 0, 2000); gte_cmd(&cpu, RTPS);
    CHECK(gte_hp_lookup(160, 120, &h) && h.fz == 2000.0f, "gana el ultimo (fz=%f)", h.fz);
    /* envejecimiento: sobrevive un cierre de frame, no dos */
    gte_hp_frame(); CHECK(gte_hp_lookup(160, 120, &h) == 1, "debe sobrevivir 1 cierre");
    gte_hp_frame(); CHECK(gte_hp_lookup(160, 120, &h) == 0, "debe caducar a los 2 cierres");
    /* saturación: SXY fuera de +-1024 no se registra */
    gte_hp_frame();
    GteHpStats s0, s1; gte_hp_stats(&s0);
    setup_cam(0, 0, 0, 500, 160, 120);
    load_v0(30000, 0, 600); gte_cmd(&cpu, RTPS);
    gte_hp_stats(&s1);
    CHECK(s1.stored == s0.stored && s1.skip_sat == s0.skip_sat + 1, "vertice saturado registrado");
    /* detrás de la cámara / división desbordada */
    load_v0(10, 10, 0); gte_cmd(&cpu, RTPS);
    gte_hp_stats(&s0);
    CHECK(s0.skip_ovf >= 1, "division desbordada no omitida");
    /* sf=0: no se registra */
    gte_hp_frame(); gte_hp_frame();
    uint32_t b4 = s0.stored;
    load_v0(10, 10, 1000); gte_cmd(&cpu, RTPS & ~(1u << 19));
    gte_hp_stats(&s1);
    CHECK(s1.stored == b4 && s1.skip_sf >= 1, "sf=0 registrado");
    /* capacidad: muchos vértices distintos en un frame, la tasa de pérdida debe ser baja */
    gte_reset(); gte_hp_enable(1); setup_cam(0, 0, 0, 500, 512, 256);
    int total = 0, found = 0;
    for (int i = 0; i < 12000; i++) {
        int x = (int)(rnd() % 1000) - 500, y = (int)(rnd() % 400) - 200, z = 700 + (int)(rnd() % 3000);
        load_v0(x, y, z); gte_cmd(&cpu, RTPS);
        total++;
    }
    gte_hp_stats(&s1);
    found = (int)(s1.stored - s1.evict);
    printf("[geom] tabla: %d vertices aleatorios en un frame: almacenados=%u desalojos=%u (%.2f%%)\n", total, s1.stored, s1.evict, 100.0 * s1.evict / s1.stored);
    CHECK(s1.evict * 20 < s1.stored, "demasiados desalojos");
    (void)found;
    gte_hp_enable(0);
}

/* Enlace con la captura: un triángulo con SXY de la GTE sale con fvalid y offset de dibujo bien tratado. */
static void t_capture(void) {
    gte_reset(); gpu_reset(); gte_hp_enable(0);
    gpu_capture_enable(1); gpu_capture_hiprec(1);
    setup_cam(0, 0, 3000, 400, 160, 120);
    uint32_t sxyv[3];
    int vx[3] = {-300, 300, 0}, vy[3] = {-100, -100, 250};
    for (int i = 0; i < 3; i++) {
        load_v0(vx[i], vy[i], 0); gte_cmd(&cpu, RTPS);
        sxyv[i] = gte_mfc2(&cpu, 15);
    }
    gpu_gp0(0xE5000000u | (320 & 0x7FF) | (5 << 11)); /* offset de dibujo (320,5) */
    gpu_gp0(0x20FFFFFFu);
    for (int i = 0; i < 3; i++) gpu_gp0(sxyv[i]);     /* polígono plano con las coordenadas crudas de la GTE */
    /* polígono 2D (HUD) con coordenadas que no salen de la GTE */
    gpu_gp0(0x2000FF00u); gpu_gp0(0x00100010u); gpu_gp0(0x00100050u); gpu_gp0(0x00500010u);
    gpu_gp1(0x05000000u | 0); gpu_gp1(0x05000000u | 100); /* cambio de direccion de display: cierra el frame */
    const GpuDrawList *l = gpu_capture_frame();
    CHECK(l != NULL && l->nops >= 2, "sin lista");
    if (l && l->nops >= 2) {
        const GpuOp *o = &l->ops[l->nops - 2];
        CHECK(o->type == GPU_OP_POLY && o->v[0].fvalid && o->v[1].fvalid && o->v[2].fvalid, "poligono 3D sin fvalid");
        CHECK(o->dox == 320 && o->doy == 5, "offset de dibujo no registrado");
        for (int i = 0; i < 3; i++) {
            float ex = 160.0f + 400.0f * vx[i] / 3000.0f + 320.0f, ey = 120.0f + 400.0f * vy[i] / 3000.0f + 5.0f;
            CHECK(fabsf(o->v[i].fx - ex) < 1e-2 && fabsf(o->v[i].fy - ey) < 1e-2, "v%d fx,fy=%f,%f esperado %f,%f", i, o->v[i].fx, o->v[i].fy, ex, ey);
            CHECK(o->v[i].fz == 3000.0f && fabsf(o->v[i].vx - vx[i]) < 1e-3 && fabsf(o->v[i].vy - vy[i]) < 1e-3, "vista v%d", i);
            /* el entero es floor(float) salvo el error de la division UNR (< ~1 px) */
            CHECK(fabsf(o->v[i].fx - o->v[i].x) <= 2.0f && fabsf(o->v[i].fy - o->v[i].y) <= 2.0f, "float lejos del entero");
        }
        CHECK(o->proj_h == 400.0f && o->proj_ox == 160.0f && o->proj_oy == 120.0f, "proyeccion del op");
        const GpuOp *p = &l->ops[l->nops - 1];
        CHECK(p->type == GPU_OP_POLY && !p->v[0].fvalid && !p->v[1].fvalid && !p->v[2].fvalid && p->proj_h == 0.0f, "el 2D no debe llevar float");
    }
    gpu_capture_hiprec(0); gpu_capture_enable(0);
}

int main(void) {
    t_exact_unchanged();
    t_formula();
    t_smooth();
    t_table();
    t_capture();
    if (fails) { printf("test_geom: %d FALLOS\n", fails); return 1; }
    printf("test_geom: todas las pruebas OK\n");
    return 0;
}
