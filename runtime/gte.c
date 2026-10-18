/* GTE (COP2) exacta en enteros, para validación diferencial contra emulador de referencia.
 * La GTE en float para VR irá aparte; esta es la fuente de verdad del comportamiento original.
 * Semántica según la documentación pública del hardware (MAC 44 bits, saturaciones, flags, UNR). */
#include <string.h>
#include "cpu.h"

static int32_t D[32];   /* datos */
static int32_t C[32];   /* control */
static uint32_t flag;

void gte_reset(void) { memset(D, 0, sizeof D); memset(C, 0, sizeof C); flag = 0; }

/* --- acceso a campos --- */
#define LO16(x) ((int16_t)((x) & 0xFFFF))
#define HI16(x) ((int16_t)((uint32_t)(x) >> 16))
static int16_t VX(int v) { return LO16(D[v * 2]); }
static int16_t VY(int v) { return HI16(D[v * 2]); }
static int16_t VZ(int v) { return LO16(D[v * 2 + 1]); }
#define IR0 D[8]
#define IR(i) D[8 + (i)]
#define MAC0 D[24]
#define MAC(i) D[24 + (i)]
#define SXY(i) D[12 + (i)]
#define SZ(i) D[16 + (i)]
#define RGBC ((uint32_t)D[6])

/* Matrices 3x3 en control 0-4 (RT), 8-12 (LLM), 16-20 (LCM). */
static int16_t mat(int base, int r, int col) {
    int idx = r * 3 + col;            /* 0..8 */
    int32_t w = C[base + idx / 2];
    return (idx & 1) ? HI16(w) : LO16(w);
}
#define TR(i) C[5 + (i)]
#define BK(i) C[13 + (i)]
#define FC(i) C[21 + (i)]
#define OFX C[24]
#define OFY C[25]
#define H ((uint16_t)C[26])
#define DQA ((int16_t)C[27])
#define DQB C[28]
#define ZSF3 ((int16_t)C[29])
#define ZSF4 ((int16_t)C[30])

/* --- comprobaciones --- */
static int64_t A(int i, int64_t v) {
    if (v > 0x7FFFFFFFFFFLL) flag |= 1u << (31 - i);
    else if (v < -0x80000000000LL) flag |= 1u << (28 - i);
    return (v << 20) >> 20; /* signo a 44 bits */
}
static int32_t setmac(int i, int64_t v, int sh) {
    A(i, v);
    MAC(i) = (int32_t)(v >> sh);
    return MAC(i);
}
static int32_t lim_ir(int i, int32_t v, int lm) {
    int32_t lo = lm ? 0 : -0x8000;
    if (v < lo) { flag |= 1u << (25 - i); return lo; }
    if (v > 0x7FFF) { flag |= 1u << (25 - i); return 0x7FFF; }
    return v;
}
static void mac_to_ir(int lm) {
    for (int i = 1; i <= 3; i++) IR(i) = lim_ir(i, MAC(i), lm);
}
static void F(int64_t v) {
    if (v > 0x7FFFFFFFLL) flag |= 1u << 16;
    else if (v < -0x80000000LL) flag |= 1u << 15;
}
static int32_t lim_c(int i, int32_t v) {     /* i=0 R, 1 G, 2 B */
    if (v < 0) { flag |= 1u << (21 - i); return 0; }
    if (v > 255) { flag |= 1u << (21 - i); return 255; }
    return v;
}
static int32_t lim_d(int64_t v) {
    if (v < 0) { flag |= 1u << 18; return 0; }
    if (v > 0xFFFF) { flag |= 1u << 18; return 0xFFFF; }
    return (int32_t)v;
}

static void push_color(void) {
    uint32_t col = (uint32_t)lim_c(0, MAC(1) >> 4) | ((uint32_t)lim_c(1, MAC(2) >> 4) << 8) |
                   ((uint32_t)lim_c(2, MAC(3) >> 4) << 16) | (RGBC & 0xFF000000u);
    D[20] = D[21]; D[21] = D[22]; D[22] = (int32_t)col;
}
static void push_sz(int32_t z) { SZ(0) = SZ(1); SZ(1) = SZ(2); SZ(2) = SZ(3); SZ(3) = z; }
static void push_sxy(int32_t x, int32_t y) {
    SXY(0) = SXY(1); SXY(1) = SXY(2);
    SXY(2) = (int32_t)((uint32_t)(x & 0xFFFF) | ((uint32_t)y << 16));
}

/* --- división UNR --- */
static uint8_t unr_table[257];
static int unr_ready;
static void unr_init(void) {
    for (int i = 0; i < 257; i++) {
        int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
        unr_table[i] = (uint8_t)(v < 0 ? 0 : v);
    }
    unr_ready = 1;
}
static uint32_t gte_divide(uint32_t h, uint32_t sz3) {
    if (h >= sz3 * 2) { flag |= 1u << 17; return 0x1FFFF; }
    if (!unr_ready) unr_init();
    int z = __builtin_clz(sz3 & 0xFFFF) - 16;
    uint64_t n = (uint64_t)h << z;
    uint32_t d = sz3 << z;
    uint32_t u = unr_table[(d - 0x7FC0) >> 7] + 0x101;
    d = (0x2000080 - d * u) >> 8;
    d = (0x0000080 + d * u) >> 8;
    uint64_t r = (n * d + 0x8000) >> 16;
    return r > 0x1FFFF ? 0x1FFFF : (uint32_t)r;
}

/* --- RTPS núcleo --- */
static void rtp(int v, int sf, int lm, int last) {
    int sh = sf ? 12 : 0;
    int64_t m3 = 0;
    for (int i = 1; i <= 3; i++) {
        int64_t s = (int64_t)TR(i - 1) << 12;
        s = A(i, s + (int64_t)mat(0, i - 1, 0) * VX(v));
        s = A(i, s + (int64_t)mat(0, i - 1, 1) * VY(v));
        s = A(i, s + (int64_t)mat(0, i - 1, 2) * VZ(v));
        MAC(i) = (int32_t)(s >> sh);
        if (i == 3) m3 = s;
    }
    IR(1) = lim_ir(1, MAC(1), lm);
    IR(2) = lim_ir(2, MAC(2), lm);
    /* IR3: la flag se calcula sobre MAC3>>12 aunque sf=0 */
    {
        int32_t v3 = MAC(3), lo = lm ? 0 : -0x8000;
        int32_t f3 = (int32_t)(m3 >> 12);
        if (f3 < -0x8000 || f3 > 0x7FFF) flag |= 1u << 22;
        IR(3) = v3 < lo ? lo : v3 > 0x7FFF ? 0x7FFF : v3;
    }
    push_sz(lim_d(m3 >> 12));
    uint32_t n = gte_divide(H, (uint32_t)SZ(3));
    int64_t x = (int64_t)n * IR(1) + OFX; F(x);
    int64_t y = (int64_t)n * IR(2) + OFY; F(y);
    int32_t sx = (int32_t)(x >> 16), sy = (int32_t)(y >> 16);
    if (sx < -0x400) { sx = -0x400; flag |= 1u << 14; } else if (sx > 0x3FF) { sx = 0x3FF; flag |= 1u << 14; }
    if (sy < -0x400) { sy = -0x400; flag |= 1u << 13; } else if (sy > 0x3FF) { sy = 0x3FF; flag |= 1u << 13; }
    push_sxy(sx, sy);
    if (last) {
        int64_t q = (int64_t)n * DQA + DQB; F(q);
        MAC0 = (int32_t)q;
        int32_t i0 = (int32_t)(q >> 12);
        if (i0 < 0) { i0 = 0; flag |= 1u << 12; } else if (i0 > 0x1000) { i0 = 0x1000; flag |= 1u << 12; }
        IR0 = i0;
    }
}

/* --- multiplicación matriz·vector + traslación (MVMVA y color) --- */
static void mvmva_core(int mx, const int32_t vec[3], int cv, int sh, int lm) {
    int16_t M[3][3];
    if (mx < 3) {
        static const int bases[3] = {0, 8, 16};
        for (int r = 0; r < 3; r++) for (int k = 0; k < 3; k++) M[r][k] = mat(bases[mx], r, k);
    } else { /* matriz basura del hardware */
        int16_t R = (int16_t)((RGBC & 0xFF) << 4);
        M[0][0] = (int16_t)-R; M[0][1] = R; M[0][2] = (int16_t)IR0;
        M[1][0] = M[1][1] = M[1][2] = mat(0, 0, 2);
        M[2][0] = M[2][1] = M[2][2] = mat(0, 1, 1);
    }
    int32_t T[3] = {0, 0, 0};
    if (cv == 0) for (int i = 0; i < 3; i++) T[i] = TR(i);
    else if (cv == 1) for (int i = 0; i < 3; i++) T[i] = BK(i);
    else if (cv == 2) for (int i = 0; i < 3; i++) T[i] = FC(i);
    for (int i = 1; i <= 3; i++) {
        int64_t t = (int64_t)T[i - 1] << 12;
        if (cv == 2) {
            /* fallo del hardware: la columna 0 solo afecta a las flags */
            int64_t f = A(i, t + (int64_t)M[i - 1][0] * vec[0]);
            lim_ir(i, (int32_t)(f >> sh), 0);
            t = 0;
        } else {
            t = A(i, t + (int64_t)M[i - 1][0] * vec[0]);
        }
        t = A(i, t + (int64_t)M[i - 1][1] * vec[1]);
        t = A(i, t + (int64_t)M[i - 1][2] * vec[2]);
        MAC(i) = (int32_t)(t >> sh);
    }
    mac_to_ir(lm);
}

static void vec_of(int v, int32_t out[3]) {
    if (v < 3) { out[0] = VX(v); out[1] = VY(v); out[2] = VZ(v); }
    else { out[0] = IR(1); out[1] = IR(2); out[2] = IR(3); }
}
static void ir_vec(int32_t o[3]) { o[0] = IR(1); o[1] = IR(2); o[2] = IR(3); }

static uint32_t R8(void) { return RGBC & 0xFF; }
static uint32_t G8(void) { return (RGBC >> 8) & 0xFF; }
static uint32_t B8(void) { return (RGBC >> 16) & 0xFF; }

/* Interpolación hacia el color lejano (FC) con IR0. Entrada: MAC1-3 ya cargados (sin desplazar). */
static void interp_fc(int sh, int lm) {
    int32_t m[3] = {MAC(1), MAC(2), MAC(3)};
    for (int i = 1; i <= 3; i++) {
        int64_t d = ((int64_t)FC(i - 1) << 12) - m[i - 1];
        A(i, d);
        IR(i) = lim_ir(i, (int32_t)(d >> sh), 0);
    }
    for (int i = 1; i <= 3; i++) setmac(i, (int64_t)IR(i) * IR0 + m[i - 1], sh);
    push_color();
    mac_to_ir(lm);
}

/* Luz: IR = lim(LLM·V), luego IR = lim(BK + LCM·IR). */
static void light(int v, int sh, int lm) {
    int32_t a[3]; vec_of(v, a);
    mvmva_core(1, a, 3, sh, lm);
    ir_vec(a);
    mvmva_core(2, a, 1, sh, lm);
}
static void color_mul(void) { /* MAC = [R*IR1, G*IR2, B*IR3] << 4 */
    setmac(1, ((int64_t)R8() * IR(1)) << 4, 0);
    setmac(2, ((int64_t)G8() * IR(2)) << 4, 0);
    setmac(3, ((int64_t)B8() * IR(3)) << 4, 0);
}
static void finish_shift(int sh, int lm) { /* MAC = MAC >> sh, push, IR = lim */
    for (int i = 1; i <= 3; i++) MAC(i) = MAC(i) >> sh;
    push_color();
    mac_to_ir(lm);
}

static void ncs(int v, int sh, int lm) { light(v, sh, lm); push_color(); }
static void ncc(int v, int sh, int lm) { light(v, sh, lm); color_mul(); finish_shift(sh, lm); }
static void ncd(int v, int sh, int lm) { light(v, sh, lm); color_mul(); interp_fc(sh, lm); }

void gte_cmd(Cpu *c, uint32_t cmd) {
    (void)c;
    int sf = (cmd >> 19) & 1, sh = sf ? 12 : 0, lm = (cmd >> 10) & 1;
    int mx = (cmd >> 17) & 3, v = (cmd >> 15) & 3, cv = (cmd >> 13) & 3;
    flag = 0;
    switch (cmd & 0x3F) {
    case 0x01: rtp(0, sf, lm, 1); break;                                   /* RTPS */
    case 0x30: rtp(0, sf, lm, 0); rtp(1, sf, lm, 0); rtp(2, sf, lm, 1); break; /* RTPT */
    case 0x06: { /* NCLIP */
        int64_t x0 = LO16(SXY(0)), y0 = HI16(SXY(0)), x1 = LO16(SXY(1)), y1 = HI16(SXY(1));
        int64_t x2 = LO16(SXY(2)), y2 = HI16(SXY(2));
        int64_t m = x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1;
        F(m); MAC0 = (int32_t)m; break;
    }
    case 0x0C: { /* OP */
        int64_t d1 = mat(0, 0, 0), d2 = mat(0, 1, 1), d3 = mat(0, 2, 2);
        setmac(1, IR(3) * d2 - IR(2) * d3, sh);
        setmac(2, IR(1) * d3 - IR(3) * d1, sh);
        setmac(3, IR(2) * d1 - IR(1) * d2, sh);
        mac_to_ir(lm); break;
    }
    case 0x10: /* DPCS */
        setmac(1, (int64_t)R8() << 16, 0); setmac(2, (int64_t)G8() << 16, 0); setmac(3, (int64_t)B8() << 16, 0);
        interp_fc(sh, lm); break;
    case 0x2A: /* DPCT */
        for (int k = 0; k < 3; k++) {
            uint32_t col = (uint32_t)D[20];
            setmac(1, (int64_t)(col & 0xFF) << 16, 0);
            setmac(2, (int64_t)((col >> 8) & 0xFF) << 16, 0);
            setmac(3, (int64_t)((col >> 16) & 0xFF) << 16, 0);
            interp_fc(sh, lm);
        }
        break;
    case 0x11: /* INTPL */
        for (int i = 1; i <= 3; i++) setmac(i, (int64_t)IR(i) << 12, 0);
        interp_fc(sh, lm); break;
    case 0x29: color_mul(); interp_fc(sh, lm); break;                       /* DCPL */
    case 0x12: { int32_t a[3]; vec_of(v, a); mvmva_core(mx, a, cv, sh, lm); break; } /* MVMVA */
    case 0x13: ncd(0, sh, lm); break;                                       /* NCDS */
    case 0x16: ncd(0, sh, lm); ncd(1, sh, lm); ncd(2, sh, lm); break;      /* NCDT */
    case 0x1B: ncc(0, sh, lm); break;                                       /* NCCS */
    case 0x3F: ncc(0, sh, lm); ncc(1, sh, lm); ncc(2, sh, lm); break;      /* NCCT */
    case 0x1E: ncs(0, sh, lm); break;                                       /* NCS */
    case 0x20: ncs(0, sh, lm); ncs(1, sh, lm); ncs(2, sh, lm); break;      /* NCT */
    case 0x14: { /* CDP */
        int32_t a[3]; ir_vec(a); mvmva_core(2, a, 1, sh, lm);
        color_mul(); interp_fc(sh, lm); break;
    }
    case 0x1C: { /* CC */
        int32_t a[3]; ir_vec(a); mvmva_core(2, a, 1, sh, lm);
        color_mul(); finish_shift(sh, lm); break;
    }
    case 0x28: /* SQR */
        for (int i = 1; i <= 3; i++) setmac(i, (int64_t)IR(i) * IR(i), sh);
        mac_to_ir(lm); break;
    case 0x2D: { /* AVSZ3 */
        int64_t m = (int64_t)ZSF3 * (SZ(1) + SZ(2) + SZ(3)); F(m); MAC0 = (int32_t)m;
        D[7] = lim_d(m >> 12); break;
    }
    case 0x2E: { /* AVSZ4 */
        int64_t m = (int64_t)ZSF4 * (SZ(0) + SZ(1) + SZ(2) + SZ(3)); F(m); MAC0 = (int32_t)m;
        D[7] = lim_d(m >> 12); break;
    }
    case 0x3D: /* GPF */
        for (int i = 1; i <= 3; i++) setmac(i, (int64_t)IR(i) * IR0, sh);
        push_color(); mac_to_ir(lm); break;
    case 0x3E: /* GPL */
        for (int i = 1; i <= 3; i++) setmac(i, ((int64_t)MAC(i) << sh) + (int64_t)IR(i) * IR0, sh);
        push_color(); mac_to_ir(lm); break;
    default: break; /* comandos no documentados: sin efecto */
    }
    if (flag & 0x7F87E000u) flag |= 0x80000000u;
    C[31] = (int32_t)flag;
}

/* --- transferencias de registros --- */
static int32_t sat5(int32_t v) { v >>= 7; return v < 0 ? 0 : v > 0x1F ? 0x1F : v; }

uint32_t gte_mfc2(Cpu *c, int r) {
    (void)c;
    switch (r) {
    case 1: case 3: case 5: case 8: case 9: case 10: case 11: return (uint32_t)(int32_t)(int16_t)D[r];
    case 7: case 16: case 17: case 18: case 19: return (uint32_t)D[r] & 0xFFFF;
    case 15: return (uint32_t)SXY(2);
    case 28: case 29: return (uint32_t)(sat5(IR(1)) | (sat5(IR(2)) << 5) | (sat5(IR(3)) << 10));
    default: return (uint32_t)D[r];
    }
}

void gte_mtc2(Cpu *c, int r, uint32_t v) {
    (void)c;
    switch (r) {
    case 15: push_sxy(LO16(v), HI16(v)); break;
    case 28:
        D[28] = (int32_t)(v & 0x7FFF);
        IR(1) = (int32_t)((v & 0x1F) << 7);
        IR(2) = (int32_t)(((v >> 5) & 0x1F) << 7);
        IR(3) = (int32_t)(((v >> 10) & 0x1F) << 7);
        break;
    case 29: case 31: break; /* solo lectura */
    case 30: {
        D[30] = (int32_t)v;
        uint32_t x = (int32_t)v < 0 ? ~v : v;
        D[31] = x ? __builtin_clz(x) : 32;
        break;
    }
    default: D[r] = (int32_t)v; break;
    }
}

uint32_t gte_cfc2(Cpu *c, int r) {
    (void)c;
    switch (r) {
    case 4: case 12: case 20: case 26: case 27: case 29: case 30: return (uint32_t)(int32_t)(int16_t)C[r];
    default: return (uint32_t)C[r];
    }
}

void gte_ctc2(Cpu *c, int r, uint32_t v) {
    (void)c;
    if (r == 31) {
        flag = v & 0x7FFFF000u;
        if (flag & 0x7F87E000u) flag |= 0x80000000u;
        C[31] = (int32_t)flag;
        return;
    }
    C[r] = (int32_t)v;
}

void gte_lwc2(Cpu *c, int r, uint32_t v) { gte_mtc2(c, r, v); }
uint32_t gte_swc2(Cpu *c, int r) { return gte_mfc2(c, r); }
