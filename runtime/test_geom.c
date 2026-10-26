/* Pruebas de la geometría de alta precisión (sombra flotante de la GTE, gte.c) y de su enlace con la captura de GPU.
 * Compilar a mano (el Makefile no la incluye): clang -I. -O2 -o ../build/test_geom <RT sin headless/test_/nogen> test_geom.c -lm
 * Comprueba: (1) la alta precisión NO cambia nada del resultado entero (regs de datos/control tras cada comando, 50k comandos
 * aleatorios con y sin ella); (2) la posición float coincide con la fórmula cerrada; (3) trayectoria suave: con la cámara
 * moviéndose despacio la posición float tiene segunda diferencia ~0 mientras la entera salta +-1 px; (4) envejecimiento por frame,
 * ambigüedad por (sx,sy); (5) enlace gpu_capture_hiprec -> GpuVtx.fvalid/fx/fy/fz/vx/vy/vz y offset de dibujo; (6) vértices saturados / detrás de la cámara: vvalid + flags y asociación por secuencia en bordes y esquinas compartidos. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "cpu.h"
#include "gpu.h"
#include "hps.h"

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
    /* saturación: SXY fuera de +-1024 NO entra en la tabla de normales (fvalid) pero sí en el anillo con flag (vvalid) */
    gte_hp_frame();
    GteHpStats s0, s1; gte_hp_stats(&s0);
    setup_cam(0, 0, 0, 500, 160, 120);
    load_v0(30000, 0, 600); gte_cmd(&cpu, RTPS);
    gte_hp_stats(&s1);
    CHECK(s1.flagged == s0.flagged + 1 && s1.skip_sat == s0.skip_sat + 1, "vertice saturado no registrado con flag");
    CHECK(gte_hp_lookup(0x3FF, 120, &h) == 0, "gte_hp_lookup no debe devolver saturados");
    /* detrás de la cámara / división desbordada */
    load_v0(10, 10, 0); gte_cmd(&cpu, RTPS);
    gte_hp_stats(&s0);
    CHECK(s0.skip_ovf >= 1, "division desbordada no contada");
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


/* Resolución de primitivas con vértices saturados / detrás de la cámara (asociación por secuencia). */
static uint32_t rtps_sxy(int x, int y, int z) {
    load_v0(x, y, z); gte_cmd(&cpu, RTPS);
    return gte_mfc2(&cpu, 15);
}
static void sxy_split(uint32_t w, int *sx, int *sy) { *sx = (int16_t)(w & 0xFFFF); *sy = (int16_t)(w >> 16); }
/* RTPT de 3 vértices: devuelve los 3 SXY de la FIFO (regs 12,13,14 = SXY0..2) */
static void rtpt3(const int (*v)[3], int sx[3], int sy[3]) {
    gte_mtc2(&cpu, 0, (uint32_t)(v[0][0] & 0xFFFF) | ((uint32_t)v[0][1] << 16)); gte_mtc2(&cpu, 1, (uint32_t)(int16_t)v[0][2]);
    gte_mtc2(&cpu, 2, (uint32_t)(v[1][0] & 0xFFFF) | ((uint32_t)v[1][1] << 16)); gte_mtc2(&cpu, 3, (uint32_t)(int16_t)v[1][2]);
    gte_mtc2(&cpu, 4, (uint32_t)(v[2][0] & 0xFFFF) | ((uint32_t)v[2][1] << 16)); gte_mtc2(&cpu, 5, (uint32_t)(int16_t)v[2][2]);
    gte_cmd(&cpu, RTPT);
    for (int i = 0; i < 3; i++) sxy_split(gte_mfc2(&cpu, 12 + i), &sx[i], &sy[i]);
}

static void t_saturated(void) {
    gte_reset(); gte_hp_enable(1);
    setup_cam(0, 0, 0, 500, 160, 120);
    int sx[4], sy[4]; GpuHpInfo o[4]; uint8_t ok[4];
    /* (1) fuera de pantalla por la derecha: sx satura a 0x3FF, sy normal */
    sxy_split(rtps_sxy(30000, 60, 600), &sx[0], &sy[0]);
    CHECK(sx[0] == 0x3FF, "esperaba sx saturado, %d", sx[0]);
    CHECK(gte_hp_resolve(1, sx, sy, o, ok) == 1 && ok[0], "saturado no resuelto");
    CHECK(o[0].flags == GPU_VF_SAT && fabsf(o[0].vx - 30000) < 1e-2 && fabsf(o[0].vy - 60) < 1e-2 && o[0].vz == 600.0f, "vista del saturado %f %f %f flags %d", o[0].vx, o[0].vy, o[0].vz, o[0].flags);
    /* fx sin saturar = 160 + 500*30000/600 = 25160 */
    CHECK(fabsf(o[0].fx - 25160.0f) < 1.0f && fabsf(o[0].fy - 170.0f) < 1e-2, "proyeccion sin saturar %f %f", o[0].fx, o[0].fy);
    /* (2) detrás de la cámara: z < 0 */
    int a, b; sxy_split(rtps_sxy(10, 10, -500), &a, &b);
    CHECK(gte_hp_resolve(1, &a, &b, o, ok) == 1 && (o[0].flags & GPU_VF_BEHIND) && o[0].vz < 0 && fabsf(o[0].vx - 10) < 1e-3 && fabsf(o[0].vz + 500) < 1e-3,
          "detras de la camara: flags %d vz %f", o[0].flags, o[0].vz);
    /* (3) varios saturados en el mismo borde (x=0x3FF), cada uno con su sy distinto: cada clave es única */
    int ksx[8], ksy[8];
    for (int k = 0; k < 8; k++) sxy_split(rtps_sxy(20000 + 1000 * k, 60 * k - 200, 600), &ksx[k], &ksy[k]);
    for (int k = 0; k < 8; k++) {
        GpuHpInfo r; uint8_t g;
        CHECK(gte_hp_resolve(1, &ksx[k], &ksy[k], &r, &g) == 1 && fabsf(r.vx - (20000 + 1000 * k)) < 1e-2 && fabsf(r.vy - (60 * k - 200)) < 1e-2,
              "borde compartido k=%d vx=%f", k, r.vx);
    }
    /* (4) esquina compartida (0x3FF,0x3FF): 6 triángulos con v0 normal (ancla) y v1,v2 saturados en la esquina; la ancla (seq)
     * decide a qué triángulo pertenecen v1,v2. Posiciones de vista distintas por triángulo. */
    gte_hp_frame(); gte_hp_frame();
    int tsx[6][3], tsy[6][3], tv[6][3][3];
    for (int t = 0; t < 6; t++) {
        int bx = 12000 + 2000 * t;
        int v[3][3] = {{-300 + 60 * t, -100 + 30 * t, 2000 + 50 * t}, {bx, bx, 600 + t}, {bx + 40, bx + 70, 600 + t}};
        memcpy(tv[t], v, sizeof v);
        rtpt3((const int (*)[3])v, tsx[t], tsy[t]);
        CHECK(tsx[t][1] == 0x3FF && tsy[t][1] == 0x3FF && tsx[t][2] == 0x3FF && tsy[t][2] == 0x3FF, "no son esquina t=%d", t);
    }
    int good = 0;
    for (int t = 0; t < 6; t++) {
        GpuHpInfo r[3]; uint8_t g[3];
        gte_hp_resolve(3, tsx[t], tsy[t], r, g);
        int okt = g[0] && g[1] && g[2] && r[0].flags == 0 && (r[1].flags & GPU_VF_SAT) && (r[2].flags & GPU_VF_SAT);
        for (int i = 0; okt && i < 3; i++) okt = fabsf(r[i].vx - tv[t][i][0]) < 1e-2 && fabsf(r[i].vy - tv[t][i][1]) < 1e-2 && fabsf(r[i].vz - tv[t][i][2]) < 1e-2;
        good += okt;
    }
    CHECK(good == 6, "esquina compartida con ancla: %d de 6 triangulos bien asociados", good);
    /* (5) sin ancla: los tres vértices en la misma esquina; debe devolver un triple coherente (los 3 de un mismo RTPT), no mezclar */
    {
        GpuHpInfo r[3]; uint8_t g[3];
        int ex[3] = {0x3FF, 0x3FF, 0x3FF}, ey[3] = {0x3FF, 0x3FF, 0x3FF};
        for (int t = 0; t < 4; t++) {
            int vv[3][3] = {{12000 + 500 * t, 12000, 700 + t}, {12100 + 500 * t, 12100, 700 + t}, {12200 + 500 * t, 12000, 700 + t}};
            int qx[3], qy[3]; rtpt3((const int (*)[3])vv, qx, qy);
        }
        gte_hp_resolve(3, ex, ey, r, g);
        uint32_t lo = r[0].seq, hi = lo;
        for (int i = 1; i < 3; i++) { if (r[i].seq < lo) lo = r[i].seq; if (r[i].seq > hi) hi = r[i].seq; }
        CHECK(g[0] && g[1] && g[2] && hi - lo == 2 && r[0].seq != r[1].seq && r[1].seq != r[2].seq, "sin ancla: triple incoherente (span seq %u)", hi - lo);
        CHECK((r[0].flags & GPU_VF_AMBIG) && (r[1].flags & GPU_VF_AMBIG) && (r[2].flags & GPU_VF_AMBIG), "sin ancla debe marcarse AMBIG");
    }
    /* (6) quad: RTPT(3) + RTPS(1); v3 saturado en un borde con la otra coordenada única */
    {
        int vv[3][3] = {{-200, -100, 1500}, {200, -100, 1500}, {-200, 100, 1500}};
        int qx[4], qy[4]; rtpt3((const int (*)[3])vv, qx, qy);
        sxy_split(rtps_sxy(-30000, 100, 900), &qx[3], &qy[3]);
        CHECK(qx[3] == -0x400, "borde izquierdo %d", qx[3]);
        GpuHpInfo r[4]; uint8_t g[4];
        CHECK(gte_hp_resolve(4, qx, qy, r, g) == 4, "quad no resuelto");
        CHECK(r[3].flags == GPU_VF_SAT && fabsf(r[3].vx + 30000) < 1e-2 && r[0].flags == 0 && fabsf(r[0].vx + 200) < 1e-3, "quad con vertice saturado");
    }
    /* (7) sin ancla y muchos candidatos (tope HP_MAXC): 4 vértices en la misma esquina -> candidatos distintos, todos AMBIG */
    {
        gte_hp_frame(); gte_hp_frame();
        for (int t = 0; t < 100; t++) rtps_sxy(15000 + 10 * t, 15000 + 10 * t, 700);
        int ex[4] = {0x3FF, 0x3FF, 0x3FF, 0x3FF}, ey[4] = {0x3FF, 0x3FF, 0x3FF, 0x3FF};
        GpuHpInfo r[4]; uint8_t g[4];
        CHECK(gte_hp_resolve(4, ex, ey, r, g) == 4, "esquina con 100 candidatos");
        for (int i = 0; i < 4; i++) for (int j = i + 1; j < 4; j++) CHECK(r[i].seq != r[j].seq, "mismo candidato en dos vertices (%d,%d)", i, j);
        for (int i = 0; i < 4; i++) CHECK(r[i].flags & GPU_VF_AMBIG, "esquina sin ancla debe ser AMBIG");
        /* 3 vértices con el mismo SXY y solo 2 candidatos: el tercero repite candidato pero queda marcado AMBIG */
        gte_hp_frame(); gte_hp_frame();
        rtps_sxy(15000, 15000, 700); rtps_sxy(26000, 26000, 900);
        CHECK(gte_hp_resolve(3, ex, ey, r, g) == 3 && (r[0].flags & r[1].flags & r[2].flags & GPU_VF_AMBIG), "3 vertices / 2 candidatos sin AMBIG");
    }
    gte_hp_enable(0);
}

/* Quad con dos vértices saturados en la misma esquina y decoys más cercanos en la secuencia: la elección por secuencia/cercanía falla, pero solo UNA combinación de
 * candidatos hace el cuadrilátero coplanar y con orden de tira válido (v1/v3 intercambiados también es coplanar, pero cruza). */
static void t_coplanar_combo(void) {
    gte_reset(); gte_hp_enable(1);
    setup_cam(0, 0, 0, 500, 160, 120);
    const int v0[3] = {-200, -100, 1500}, v1[3] = {26000, 21000, 1800}, v2[3] = {-100, 300, 1100};
    const int v3[3] = {v1[0] + v2[0] - v0[0], v1[1] + v2[1] - v0[1], v1[2] + v2[2] - v0[2]};
    int sx[4], sy[4];
    sxy_split(rtps_sxy(v0[0], v0[1], v0[2]), &sx[0], &sy[0]);
    rtps_sxy(30000, 31000, 600); rtps_sxy(28000, 30000, 650);        /* decoys pegados al ancla v0 en la secuencia */
    sxy_split(rtps_sxy(v1[0], v1[1], v1[2]), &sx[1], &sy[1]);
    rtps_sxy(29000, 33000, 700);
    sxy_split(rtps_sxy(v2[0], v2[1], v2[2]), &sx[2], &sy[2]);
    rtps_sxy(31000, 29000, 620); rtps_sxy(27000, 32000, 640);        /* decoys pegados al ancla v2 */
    sxy_split(rtps_sxy(v3[0], v3[1], v3[2]), &sx[3], &sy[3]);
    CHECK(sx[1] == 0x3FF && sy[1] == 0x3FF && sx[3] == 0x3FF && sy[3] == 0x3FF && sx[0] < 0x3FF && sx[2] < 0x3FF, "v1,v3 no saturan en la esquina");
    GpuHpInfo r[4]; uint8_t g[4];
    CHECK(gte_hp_resolve(4, sx, sy, r, g) == 4, "quad no resuelto");
    CHECK(fabsf(r[1].vx - v1[0]) < 1e-2 && fabsf(r[1].vy - v1[1]) < 1e-2 && fabsf(r[1].vz - v1[2]) < 1e-2, "v1 no es el verdadero (%f %f %f)", r[1].vx, r[1].vy, r[1].vz);
    CHECK(fabsf(r[3].vx - v3[0]) < 1e-2 && fabsf(r[3].vy - v3[1]) < 1e-2 && fabsf(r[3].vz - v3[2]) < 1e-2, "v3 no es el verdadero (%f %f %f)", r[3].vx, r[3].vy, r[3].vz);
    CHECK(!(r[1].flags & GPU_VF_AMBIG) && !(r[3].flags & GPU_VF_AMBIG), "la combinacion coplanar unica no debe quedar AMBIG (flags %d %d)", r[1].flags, r[3].flags);
    /* sin combinacion coplanar (el v3 verdadero no se transformó): se conserva la elección por secuencia, AMBIG */
    gte_hp_frame(); gte_hp_frame();
    sxy_split(rtps_sxy(v0[0], v0[1], v0[2]), &sx[0], &sy[0]);
    rtps_sxy(30000, 31000, 600); rtps_sxy(28000, 30000, 650);
    sxy_split(rtps_sxy(v1[0], v1[1], v1[2]), &sx[1], &sy[1]);
    sxy_split(rtps_sxy(v2[0], v2[1], v2[2]), &sx[2], &sy[2]);
    rtps_sxy(27000, 32000, 640);
    CHECK(gte_hp_resolve(4, sx, sy, r, g) == 4 && g[3], "sin combinacion coplanar: debe devolver algo");
    gte_hp_enable(0);
}

/* Captura: polígono con un vértice saturado y otro detrás de la cámara -> vvalid en todos, fvalid solo en los normales. */
static void t_capture_sat(void) {
    gte_reset(); gpu_reset(); gte_hp_enable(0);
    gpu_capture_enable(1); gpu_capture_hiprec(1);
    setup_cam(0, 0, 3000, 400, 160, 120);
    int vx[3] = {-300, 30000, 0}, vy[3] = {-100, -100, 250}, vz[3] = {0, 0, -9000};
    uint32_t sxyv[3];
    for (int i = 0; i < 3; i++) { load_v0(vx[i], vy[i], vz[i]); gte_cmd(&cpu, RTPS); sxyv[i] = gte_mfc2(&cpu, 15); }
    gpu_gp0(0x20FFFFFFu);
    for (int i = 0; i < 3; i++) gpu_gp0(sxyv[i]);
    gpu_gp1(0x05000000u | 0); gpu_gp1(0x05000000u | 100);
    const GpuDrawList *l = gpu_capture_frame();
    CHECK(l != NULL && l->nops >= 1, "sin lista");
    if (l && l->nops >= 1) {
        const GpuOp *o = &l->ops[l->nops - 1];
        CHECK(o->v[0].vvalid && o->v[0].fvalid && o->v[0].vflags == 0, "v0 normal");
        CHECK(o->v[1].vvalid && !o->v[1].fvalid && o->v[1].vflags == GPU_VF_SAT && fabsf(o->v[1].vx - 30000) < 1e-2, "v1 saturado: vvalid=%d fvalid=%d flags=%d vx=%f", o->v[1].vvalid, o->v[1].fvalid, o->v[1].vflags, o->v[1].vx);
        CHECK(o->v[2].vvalid && !o->v[2].fvalid && (o->v[2].vflags & GPU_VF_BEHIND) && o->v[2].vz < 0, "v2 detras: vvalid=%d vflags=%d vz=%f", o->v[2].vvalid, o->v[2].vflags, o->v[2].vz);
        CHECK(o->proj_h == 400.0f, "proj_h");
    }
    gpu_capture_hiprec(0); gpu_capture_enable(0);
}

/* La proyección (H, OFX, OFY) de una primitiva sale de un vértice fiable, no del último resuelto: si el último es un candidato con banderas (SAT/AMBIG) de otra proyección, no debe ganar. */
static void t_proj_trusted(void) {
    gte_reset(); gpu_reset(); gte_hp_enable(0);
    gpu_capture_enable(1); gpu_capture_hiprec(1);
    setup_cam(0, 0, 0, 500, 160, 120);
    int vv[3][3] = {{-200, -100, 1500}, {200, -100, 1500}, {-200, 100, 1500}};
    int qx[4], qy[4]; rtpt3((const int (*)[3])vv, qx, qy);
    /* v3: candidatos saturados en la esquina (0x3FF,0x3FF) con OTRA proyección (OFX=500, OFY=300) */
    setup_cam(0, 0, 0, 500, 500, 300);
    rtps_sxy(30000, 30000, 600); rtps_sxy(31000, 32000, 700);
    qx[3] = 0x3FF; qy[3] = 0x3FF;
    gpu_gp0(0x28FFFFFFu);
    for (int i = 0; i < 4; i++) gpu_gp0((uint32_t)(qx[i] & 0xFFFF) | ((uint32_t)qy[i] << 16));
    gpu_gp1(0x05000000u | 0); gpu_gp1(0x05000000u | 100);
    const GpuDrawList *l = gpu_capture_frame();
    CHECK(l != NULL && l->nops >= 1, "sin lista");
    if (l && l->nops >= 1) {
        const GpuOp *o = &l->ops[l->nops - 1];
        CHECK(o->nv == 4 && o->v[3].vvalid && o->v[3].vflags != 0, "v3 debia salir con banderas (vflags %d)", o->v[3].vflags);
        CHECK(o->proj_h == 500.0f && o->proj_ox == 160.0f && o->proj_oy == 120.0f, "proyeccion del op de un vertice AMBIG: ox=%f oy=%f", o->proj_ox, o->proj_oy);
    }
    gpu_capture_hiprec(0); gpu_capture_enable(0);
}

/* Sprite rectangular: el juego proyecta dos esquinas diagonales y construye las otras dos en pantalla. */
static void t_sprite(void) {
    gte_reset(); gpu_reset(); gte_hp_enable(0);
    gpu_capture_enable(1); gpu_capture_hiprec(1);
    setup_cam(0, 0, 0, 400, 160, 120);
    uint32_t a = rtps_sxy(-300, -200, 2000), b = rtps_sxy(300, 200, 2000);
    int ax, ay, bx, by; sxy_split(a, &ax, &ay); sxy_split(b, &bx, &by);
    gpu_gp0(0x2C808080u); /* quad texturizado */
    gpu_gp0(a); gpu_gp0(0);
    gpu_gp0((uint32_t)(bx & 0xFFFF) | ((uint32_t)ay << 16)); gpu_gp0(0);
    gpu_gp0((uint32_t)(ax & 0xFFFF) | ((uint32_t)by << 16)); gpu_gp0(0);
    gpu_gp0(b); gpu_gp0(0);
    gpu_gp1(0x05000000u | 0); gpu_gp1(0x05000000u | 100);
    const GpuDrawList *l = gpu_capture_frame();
    CHECK(l != NULL && l->nops >= 1, "sin lista");
    if (l && l->nops >= 1) {
        const GpuOp *o = &l->ops[l->nops - 1];
        CHECK(o->nv == 4 && o->v[0].fvalid && o->v[3].fvalid && !o->v[1].fvalid && !o->v[2].fvalid, "validez del sprite");
        CHECK(o->v[1].vvalid && o->v[2].vvalid && o->v[1].vflags == GPU_VF_DERIVED && o->v[2].vflags == GPU_VF_DERIVED, "esquinas derivadas");
        CHECK(fabsf(o->v[1].vx - 300) < 1.0f && fabsf(o->v[1].vy + 200) < 1.0f && fabsf(o->v[1].vz - 2000) < 1e-3f, "derivada v1 (%f %f %f)", o->v[1].vx, o->v[1].vy, o->v[1].vz);
        CHECK(fabsf(o->v[2].vx + 300) < 1.0f && fabsf(o->v[2].vy - 200) < 1.0f, "derivada v2 (%f %f)", o->v[2].vx, o->v[2].vy);
    }
    gpu_capture_hiprec(0); gpu_capture_enable(0);
}

/* RECT/sprite cuya palabra XY el juego copió de un SXY de la GTE: la captura lo enlaza SOLO por dirección de memoria (hps.h); un RECT con el mismo valor sin etiqueta (HUD) no se asocia por valor. */
static void t_rect_hp(void) {
    gte_reset(); gpu_reset(); gte_hp_enable(0);
    gpu_capture_enable(1); gpu_capture_hiprec(1);
    setup_cam(0, 0, 3000, 400, 160, 120);
    uint32_t a = rtps_sxy(300, -150, 0);                 /* SXY2 de un punto del mundo; hps_on lo etiqueta al guardarlo */
    uint32_t tag = gte_hp_sxy_tag(15, a);
    CHECK(tag != 0, "la GTE no dio etiqueta al SXY");
    /* lista de 3 paquetes en RAM: (1) RECT variable con el XY etiquetado; (2) el mismo valor SIN etiqueta; (3) RECT 8x8 con XY etiquetado y tamaño fijo */
    uint32_t base = 0x00100000u, w[3][4] = {{0x60808080u, a, (8u << 16) | 16u, 0}, {0x60808080u, a, (8u << 16) | 16u, 0}, {0x78808080u, a, 0, 0}};
    int nw[3] = {3, 3, 2};
    uint32_t at = base;
    for (int k = 0; k < 3; k++) {
        uint32_t hdr = ((uint32_t)nw[k] << 24) | (k == 2 ? 0x00FFFFFFu : ((at + 4 + 4 * nw[k]) & 0x1FFFFC));
        memcpy(&emu_ram[at], &hdr, 4);
        for (int i = 0; i < nw[k]; i++) memcpy(&emu_ram[at + 4 + 4 * i], &w[k][i], 4);
        if (k != 1) hps_set(at + 8, tag);                  /* palabra XY (índice 1 del paquete) */
        at += 4 + 4 * (uint32_t)nw[k];
    }
    gpu_gp0(0xE5000000u | (320 & 0x7FF) | (5 << 11));    /* offset de dibujo (320,5) */
    gpu_draw_list(base);
    gpu_gp1(0x05000000u | 0); gpu_gp1(0x05000000u | 100);
    const GpuDrawList *l = gpu_capture_frame();
    CHECK(l != NULL && l->nops >= 3, "sin lista de rects");
    if (l && l->nops >= 3) {
        const GpuOp *r[3] = {&l->ops[l->nops - 3], &l->ops[l->nops - 2], &l->ops[l->nops - 1]};
        int sx, sy; sxy_split(a, &sx, &sy);
        CHECK(r[0]->type == GPU_OP_RECT && r[0]->v[0].vvalid && r[0]->v[0].vflags == 0, "rect etiquetado sin vista");
        CHECK(fabsf(r[0]->v[0].vx - 300) < 1e-3f && fabsf(r[0]->v[0].vy + 150) < 1e-3f && fabsf(r[0]->v[0].vz - 3000) < 1e-3f, "vista del rect (%f %f %f)", r[0]->v[0].vx, r[0]->v[0].vy, r[0]->v[0].vz);
        CHECK(r[0]->v[0].x == sx + 320 && r[0]->v[0].y == sy + 5 && r[0]->rw == 16 && r[0]->rh == 8, "x,y del rect (offset de dibujo sumado) %d,%d", r[0]->v[0].x, r[0]->v[0].y);
        CHECK(r[0]->proj_h == 400.0f && r[0]->proj_ox == 160.0f && r[0]->proj_oy == 120.0f, "proyeccion del rect");
        CHECK(!r[1]->v[0].vvalid && r[1]->proj_h == 0.0f, "mismo valor sin etiqueta: no se asocia por valor");
        CHECK(r[2]->type == GPU_OP_RECT && r[2]->v[0].vvalid && r[2]->rw == 16 && r[2]->rh == 16, "rect de tamano fijo etiquetado");
    }
    gpu_capture_hiprec(0); gpu_capture_enable(0);
}

int main(void) {
    t_exact_unchanged();
    t_formula();
    t_smooth();
    t_table();
    t_capture();
    t_saturated();
    t_coplanar_combo();
    t_capture_sat();
    t_proj_trusted();
    t_sprite();
    t_rect_hp();
    if (fails) { printf("test_geom: %d FALLOS\n", fails); return 1; }
    printf("test_geom: todas las pruebas OK\n");
    return 0;
}
