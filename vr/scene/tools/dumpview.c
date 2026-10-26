/* dumpview: banco de render sin conexión. Reproduce lo que el visor dibuja en modo gpu para un volcado NSD1 (ram/vram/gte/list.bin/meta.txt/frame.png), para verificar arreglos sin build.
 * Uso (desde la raíz del repo):  make -C vr/scene dumpview ARGS="<carpeta del volcado> [-o lamina.png] [--ipd 0.064] [--conv 3] [--filter] [--rects N] [--semi] [--rect-hp Z] [--no-billboard]"
 * En lote:  make -C vr/scene dumpview ARGS="--batch <carpeta> [--sheets]"  procesa todos los volcados de la carpeta (recursivo) y escribe UNA línea por volcado: nombre, motivo (point del meta.txt),
 *   % de píxeles distintos (umbral 24/255) del ojo con ipd 0 frente a frame.png y los 3 tipos de primitiva que más píxeles distintos cubren (porcentaje de esos píxeles; el tipo de un píxel es el de la
 *   última primitiva de la lista cuya huella en pantalla original lo cubre: aproximación por orden de pintor, sin transparencia de texel; "sin_salida" = primitiva que la escena no emite; "fondo" = ninguna).
 *   Sin láminas salvo --sheets (entonces <volcado>/dumpview.png). En lote solo se renderiza el ojo ipd 0.
 *
 * Qué replica (fuente entre paréntesis):
 *   - Ensamblado: vr_scene_build con las MISMAS opciones que vrhost.c build_one en modo gpu (screen_filter, eye_near_m 0,10, line3d_px 1, coplanar_m 0,005, origen bajo el ojo VR,
 *     correspondencia mundo -> usuario con el registro de cámara del meta.txt; keep_late_clear/veil_as_2d si la vista es ENGINE; reensamblado sin filtro si no se conserva).
 *   - Cámara del ojo = pantalla 3D (RenderRG.stereoCamera + XRStereo.eye): ventana REAL de la lista (vr_scene_screen_window: H y OFX/OFY), ojos paralelos separados IPD con la ventana desplazada
 *     ipd/(2*conv), proyección de Z invertida near 0,05 / far 800 (XRMath.projection), textura 1280x960.
 *   - encodeStereoGPU: pase 3D (lotes kind 0/1 en orden; opaco: prueba greaterEqual con escritura; semitransparente: prueba sin escritura con sesgo de profundidad 4/2, mezcla en gamma
 *     en dos pasadas si el lote lleva textura: bit15 mezcla, sin bit15 opaco), onda por fila del agua (vr_post_ysrc) y encodeStereo2D (lotes kind 2 con vr_vs_hud sobre el rect de display);
 *     tinte y recubrimientos de la clase 3 al presentar (screenFx).
 *   - vr_scene.metal vr_fs_scene(_blend): interpolación de uv/color con perspectiva correcta (el shader no usa [[center_no_perspective]]), tpage/clut/ventana de textura planos del primer
 *     vértice, descarte del texel 0, volteo de rects, recorte de la primitiva en el 2D, 5->8 bits (v<<3)|(v>>2), mezcla vr_blend.h.
 * Qué NO replica: MSAA (una muestra por píxel), formato de color real (se guarda en gamma float: sin el redondeo a 8 bits entre pases), pipeline nativo, panel HUD flotante del modo mundo
 *   (en vista WORLD la app NO compone el 2D dentro del 3D: aquí sí, para poder compararlo con frame.png), líneas de exploración y máscara de visión (solo se aplican en el post de la vista
 *   de mundo, no al presentar la pantalla 3D), hardware de recorte de bandas de guarda, radar/arma/cuerpo, cinemática (vrcine) y VRAM por eventos: se usa la VRAM del volcado tal cual
 *   (estado final de la lista = el que ve la app en el pase estéreo, que dibuja todos los lotes con la VRAM de su último grupo).
 * Salida: lámina PNG 3x2: (a) frame.png, (b) ojo izquierdo (ipd, conv), (c) ojo izquierdo con la cabeza +0,10 m lateral y 15 grados a la derecha, (d) diferencias a-b (rojo > 24/255),
 *   (e) b con las primitivas no 3D resaltadas por color, (f) igual que b con ipd 0 (cámara del juego exacta: separa fallos del rasterizador de la paralaje estéreo). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>
#include "cpu.h"
#include "gpu.h"
#include "hle.h"
#include "vr_scene.h"
#include "ns_dump.h"
#include "vr_camera.h"
#include "vr_usermap.h"
#include "vr_view.h"
#include "vr_blend.h"

uint8_t emu_ram[EMU_RAM_SIZE];
uint32_t hle_frame_count(void) { return 0; }
void gte_hp_enable(int on) { (void)on; }
void gte_hp_frame(void) {}
int gte_hp_lookup(int sx, int sy, GpuHpInfo *o) { (void)sx; (void)sy; (void)o; return 0; }
int gte_hp_resolve_a(int n, const int *sx, const int *sy, const uint32_t *a, const uint32_t *w, GpuHpInfo *o, uint8_t *ok) { (void)n; (void)sx; (void)sy; (void)a; (void)w; (void)o; (void)ok; return 0; }
int gte_hp_resolve_addr(uint32_t addr, uint32_t word, GpuHpInfo *o) { (void)addr; (void)word; (void)o; return 0; }
void hle_invoke(Cpu *c, uint32_t fn, uint32_t arg) { (void)c; (void)fn; (void)arg; }
int rt_orig(Cpu *c, const char *name) { (void)c; (void)name; return 0; }
uint8_t mem_r8(Cpu *c, uint32_t a) { (void)c; return emu_ram[a & 0x1FFFFF]; }
uint16_t mem_r16(Cpu *c, uint32_t a) { (void)c; uint16_t v; memcpy(&v, &emu_ram[a & 0x1FFFFE], 2); return v; }
uint32_t mem_r32(Cpu *c, uint32_t a) { (void)c; uint32_t v; memcpy(&v, &emu_ram[a & 0x1FFFFC], 4); return v; }
void mem_w32(Cpu *c, uint32_t a, uint32_t v) { (void)c; memcpy(&emu_ram[a & 0x1FFFFC], &v, 4); }

static int g_quiet;   /* modo lote: el detalle de cada volcado no se imprime, solo su línea de la tabla */
static int dv_printf(const char *f, ...) __attribute__((format(printf, 1, 2)));
static int dv_printf(const char *f, ...) { if (g_quiet) return 0; va_list a; va_start(a, f); int n = vprintf(f, a); va_end(a); return n; }
#define printf dv_printf

#define TW 1280   /* XRPresenter.stereoW/H */
#define TH 960
#define PW 640    /* panel de la lámina */
#define PH 512
#define NEAR_M 0.05
#define FAR_M 800.0

/* ------------------------------------------------------------------------------------------------ lectura de frame.png (RGBA8 con bloques stored, el que escribe ns_dump) */
static uint32_t rbe32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint8_t *png_read_stored(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    if (n < 33 || memcmp(b, "\x89PNG\r\n\x1a\n", 8)) { free(b); return NULL; }
    uint8_t *z = (uint8_t *)malloc((size_t)n); size_t zn = 0;
    int W = 0, H = 0;
    for (long o = 8; o + 12 <= n;) {
        uint32_t len = rbe32(b + o); const uint8_t *t = b + o + 4, *d = b + o + 8;
        if (o + 12 + len > (uint32_t)n) break;
        if (!memcmp(t, "IHDR", 4)) { W = (int)rbe32(d); H = (int)rbe32(d + 4); if (d[8] != 8 || d[9] != 6 || d[12] != 0) { free(b); free(z); return NULL; } }
        else if (!memcmp(t, "IDAT", 4)) { memcpy(z + zn, d, len); zn += len; }
        o += 12 + len;
    }
    free(b);
    size_t rowb = (size_t)W * 4 + 1, need = rowb * (size_t)H;
    uint8_t *raw = (uint8_t *)malloc(need + 16); size_t rn = 0;
    size_t i = 2;
    while (i + 5 <= zn && rn < need) {
        int fin = z[i] & 1, bt = (z[i] >> 1) & 3;
        if (bt != 0) { free(z); free(raw); return NULL; }
        uint32_t L = z[i + 1] | (uint32_t)z[i + 2] << 8;
        i += 5;
        if (i + L > zn || rn + L > need + 16) { free(z); free(raw); return NULL; }
        memcpy(raw + rn, z + i, L); rn += L; i += L;
        if (fin) break;
    }
    free(z);
    if (rn < need) { free(raw); return NULL; }
    uint8_t *out = (uint8_t *)malloc((size_t)W * H * 4);
    for (int y = 0; y < H; y++) {
        const uint8_t *s = raw + (size_t)y * rowb + 1; uint8_t *d = out + (size_t)y * W * 4; int ft = raw[(size_t)y * rowb];
        for (int x = 0; x < W * 4; x++) {
            int a = x >= 4 ? d[x - 4] : 0, up = y ? d[x - W * 4] : 0, c = (x >= 4 && y) ? d[x - W * 4 - 4] : 0, v = s[x];
            switch (ft) {
            case 1: v += a; break;
            case 2: v += up; break;
            case 3: v += (a + up) >> 1; break;
            case 4: { int p = a + up - c, pa = abs(p - a), pb = abs(p - up), pc = abs(p - c); v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? up : c); break; }
            default: break;
            }
            d[x] = (uint8_t)v;
        }
    }
    free(raw);
    *w = W; *h = H;
    return out;
}

/* ------------------------------------------------------------------------------------------------ matrices 4x4 de doble precisión, columna-mayor m[col*4+fila] */
typedef struct { double m[16]; } M4;
static M4 m4_id(void) { M4 r; memset(&r, 0, sizeof r); r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1; return r; }
static M4 m4_mul(M4 a, M4 b) { M4 r; for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) { double s = 0; for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k]; r.m[c * 4 + rr] = s; } return r; }
static M4 m4_trans(double x, double y, double z) { M4 r = m4_id(); r.m[12] = x; r.m[13] = y; r.m[14] = z; return r; }
static M4 m4_diag(double x, double y, double z) { M4 r = m4_id(); r.m[0] = x; r.m[5] = y; r.m[10] = z; return r; }
static M4 m4_roty(double a) { M4 r = m4_id(); double c = cos(a), s = sin(a); r.m[0] = c; r.m[2] = -s; r.m[8] = s; r.m[10] = c; return r; }   /* XRMath.rotationY */
static int m4_inv(M4 a, M4 *out) {
    double aug[4][8];
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) { aug[r][c] = a.m[c * 4 + r]; aug[r][4 + c] = r == c; }
    for (int i = 0; i < 4; i++) {
        int p = i; for (int r = i + 1; r < 4; r++) if (fabs(aug[r][i]) > fabs(aug[p][i])) p = r;
        if (fabs(aug[p][i]) < 1e-18) return -1;
        if (p != i) for (int c = 0; c < 8; c++) { double t = aug[i][c]; aug[i][c] = aug[p][c]; aug[p][c] = t; }
        double d = aug[i][i]; for (int c = 0; c < 8; c++) aug[i][c] /= d;
        for (int r = 0; r < 4; r++) if (r != i) { double f = aug[r][i]; if (f != 0) for (int c = 0; c < 8; c++) aug[r][c] -= f * aug[i][c]; }
    }
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) out->m[c * 4 + r] = aug[r][4 + c];
    return 0;
}
/* XRMath.projection(tangents l,r,u,d; near; far) */
static M4 m4_proj(double l, double r, double u, double d, double n, double f) {
    M4 p; memset(&p, 0, sizeof p);
    double sx = 2 / (l + r), sy = 2 / (u + d), cx = (r - l) / (l + r), cy = (u - d) / (u + d), a = n / (f - n), b = n * f / (f - n);
    p.m[0] = sx; p.m[5] = sy; p.m[8] = cx; p.m[9] = cy; p.m[10] = a; p.m[11] = -1; p.m[14] = b;
    return p;
}

/* ------------------------------------------------------------------------------------------------ "GPU": destino, rasterizador y fragmento (vr_scene.metal) */
typedef struct { float *rgb; float *z; uint8_t *cov; int W, H; } Tgt;
static Tgt tgt_new(int W, int H) {
    Tgt t = {(float *)calloc((size_t)W * H * 3, 4), (float *)calloc((size_t)W * H, 4), (uint8_t *)calloc((size_t)W * H, 1), W, H};
    return t;
}
static void tgt_free(Tgt *t) { free(t->rgb); free(t->z); free(t->cov); }

typedef struct { const uint16_t *vram; int filter; const VrScene *sc; int noz; } Ctx;
static int g_affine;
typedef struct { int ztest, zwrite; float bias, slope; int pass, blend, hud; } Rs;   /* blend -1 = opaco */
typedef struct { double x, y; float z, iw; float a[7]; } SV;                          /* pantalla; a = u, v, r, g, b, ox, oy */

static uint32_t vr_texel(const Ctx *C, uint32_t tpage, uint32_t clut, uint32_t tw, int u, int v) {
    int tx = (int)(tpage & 15u) * 64, ty = (int)((tpage >> 4) & 1u) * 256;
    uint32_t tp = (tpage >> 7) & 3u;
    int clx = (int)(clut & 63u) * 16, cly = (int)((clut >> 6) & 511u);
    u &= 255; v &= 255;
    if (tw != 0u) {
        int mx = (int)(tw & 255u), my = (int)((tw >> 8) & 255u), ox = (int)((tw >> 16) & 255u), oy = (int)((tw >> 24) & 255u);
        u = (u & ~(mx * 8)) | ((ox & mx) * 8);
        v = (v & ~(my * 8)) | ((oy & my) * 8);
    }
    int y = (ty + v) & 511;
    const uint16_t *V = C->vram;
    if (tp == 0u) {
        uint32_t w = V[y * 1024 + ((tx + (u >> 2)) & 1023)];
        uint32_t idx = (w >> (uint32_t)((u & 3) * 4)) & 15u;
        return V[cly * 1024 + ((clx + (int)idx) & 1023)];
    } else if (tp == 1u) {
        uint32_t w = V[y * 1024 + ((tx + (u >> 1)) & 1023)];
        uint32_t idx = (w >> (uint32_t)((u & 1) * 8)) & 255u;
        return V[cly * 1024 + ((clx + (int)idx) & 1023)];
    }
    return V[y * 1024 + ((tx + u) & 1023)];
}
static void vr_expand(uint32_t t, float o[3]) {
    uint32_t r = t & 31u, g = (t >> 5) & 31u, b = (t >> 10) & 31u;
    o[0] = (float)((r << 3) | (r >> 2)) / 255.0f; o[1] = (float)((g << 3) | (g >> 2)) / 255.0f; o[2] = (float)((b << 3) | (b >> 2)) / 255.0f;
}
static float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

/* vr_scene_rgb: devuelve 0 si el fragmento se descarta. col = multiplicador (1,0 = 128). */
static int shade(const Ctx *C, const VrVtx *v0, const VrState *st, const Rs *rs, float u, float vv, const float col[3], float ox, float oy, float out[3]) {
    if (rs->hud && st) {   /* recorte de la primitiva [x1, y1, x2+1, y2+1) */
        if (ox < (float)st->cx1 || oy < (float)st->cy1 || ox >= (float)st->cx2 + 1.0f || oy >= (float)st->cy2 + 1.0f) return 0;
    }
    uint32_t flags = v0->flags, clut = v0->clut, tpage = v0->tpage, tw = 0;
    if (st) tw = (uint32_t)st->twmx | ((uint32_t)st->twmy << 8) | ((uint32_t)st->twox << 16) | ((uint32_t)st->twoy << 24);
    float rgb[3];
    if (flags & GPU_F_TEXTURED) {
        float tex[3];
        float uvx = u, uvy = vv;
        if (flags & VR_F_RECT) { if (flags & GPU_F_FLIPX) uvx += 1.0f; if (flags & GPU_F_FLIPY) uvy += 1.0f; }
        uint32_t tn;
        if (C->filter) {
            float fx = uvx - 0.5f, fy = uvy - 0.5f, flx = floorf(fx), fly = floorf(fy), frx = fx - flx, fry = fy - fly;
            int u0 = (int)flx, v0i = (int)fly;
            uint32_t t[4] = {vr_texel(C, tpage, clut, tw, u0, v0i), vr_texel(C, tpage, clut, tw, u0 + 1, v0i), vr_texel(C, tpage, clut, tw, u0, v0i + 1), vr_texel(C, tpage, clut, tw, u0 + 1, v0i + 1)};
            float w[4] = {(1 - frx) * (1 - fry), frx * (1 - fry), (1 - frx) * fry, frx * fry};
            float acc[3] = {0, 0, 0}, cov = 0;
            for (int i = 0; i < 4; i++) if (t[i] != 0u) { float e[3]; vr_expand(t[i], e); for (int k = 0; k < 3; k++) acc[k] += e[k] * w[i]; cov += w[i]; }
            if (cov < 0.5f) return 0;
            for (int k = 0; k < 3; k++) tex[k] = acc[k] / cov;
            tn = t[(fry >= 0.5f ? 2 : 0) + (frx >= 0.5f ? 1 : 0)];
        } else {
            tn = vr_texel(C, tpage, clut, tw, (int)floorf(uvx), (int)floorf(uvy));
            if (tn == 0u) return 0;
            vr_expand(tn, tex);
        }
        int b15 = (tn & 0x8000u) != 0u;
        if (rs->pass == 1 && !b15) return 0;
        if (rs->pass == 2 && b15) return 0;
        for (int k = 0; k < 3; k++) rgb[k] = (flags & GPU_F_RAW) ? tex[k] : tex[k] * col[k];
    } else {
        if (rs->pass == 2) return 0;
        for (int k = 0; k < 3; k++) rgb[k] = col[k] * (128.0f / 255.0f);
    }
    for (int k = 0; k < 3; k++) out[k] = clamp01(rgb[k]);
    return 1;
}

static int g_dbg, g_dbgx, g_dbgy;
static double edge(double px, double py, double qx, double qy, double x, double y) { return (qx - px) * (y - py) - (qy - py) * (x - px); }
static int topleft(double px, double py, double qx, double qy) { double dx = qx - px, dy = qy - py; return (dy == 0 && dx > 0) || dy < 0; }

/* Un triángulo ya en pantalla (Metal: relleno con la regla superior-izquierda, muestra en el centro del píxel, atributos con perspectiva correcta, proveedor plano = vértice 0). */
static void draw_tri(Tgt *T, const Ctx *C, const VrVtx *pv, const SV s[3], const Rs *rs) {
    SV v[3] = {s[0], s[1], s[2]};
    double area = edge(v[0].x, v[0].y, v[1].x, v[1].y, v[2].x, v[2].y);
    if (!(fabs(area) > 1e-12)) return;
    if (area < 0) { SV t = v[1]; v[1] = v[2]; v[2] = t; area = -area; }   /* orientación interior = E>0 (sin descarte de caras) */
    double minx = fmin(v[0].x, fmin(v[1].x, v[2].x)), maxx = fmax(v[0].x, fmax(v[1].x, v[2].x)), miny = fmin(v[0].y, fmin(v[1].y, v[2].y)), maxy = fmax(v[0].y, fmax(v[1].y, v[2].y));
    int x0 = (int)fmax(0, floor(minx - 0.5)), x1 = (int)fmin(T->W - 1, ceil(maxx + 0.5)), y0 = (int)fmax(0, floor(miny - 0.5)), y1 = (int)fmin(T->H - 1, ceil(maxy + 0.5));
    if (x0 > x1 || y0 > y1) return;
    int tl[3] = {topleft(v[1].x, v[1].y, v[2].x, v[2].y), topleft(v[2].x, v[2].y, v[0].x, v[0].y), topleft(v[0].x, v[0].y, v[1].x, v[1].y)};
    /* plano de profundidad (afín en pantalla) para el sesgo por pendiente */
    float dzdx = 0, dzdy = 0;
    {
        double ax = v[1].x - v[0].x, ay = v[1].y - v[0].y, bx = v[2].x - v[0].x, by = v[2].y - v[0].y, dz1 = v[1].z - v[0].z, dz2 = v[2].z - v[0].z, d = ax * by - ay * bx;
        if (fabs(d) > 1e-12) { dzdx = (float)((dz1 * by - dz2 * ay) / d); dzdy = (float)((dz2 * ax - dz1 * bx) / d); }
    }
    float zmax = fmaxf(v[0].z, fmaxf(v[1].z, v[2].z)), bias = 0;
    if (rs->bias != 0 || rs->slope != 0) {
        int ex; frexpf(zmax > 0 ? zmax : 1e-30f, &ex);
        float r = ldexpf(1.0f, ex - 1 - 23);   /* resolución mínima de un float32 de profundidad en esa magnitud */
        bias = rs->bias * r + rs->slope * fmaxf(fabsf(dzdx), fabsf(dzdy));
    }
    const VrState *st = (pv[0].pad != 0xFFFF) ? vr_scene_state(C->sc, pv[0].pad) : NULL;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            double px = x + 0.5, py = y + 0.5;
            double e0 = edge(v[1].x, v[1].y, v[2].x, v[2].y, px, py), e1 = edge(v[2].x, v[2].y, v[0].x, v[0].y, px, py), e2 = edge(v[0].x, v[0].y, v[1].x, v[1].y, px, py);
            if (!((e0 > 0 || (e0 == 0 && tl[0])) && (e1 > 0 || (e1 == 0 && tl[1])) && (e2 > 0 || (e2 == 0 && tl[2])))) continue;
            double l0 = e0 / area, l1 = e1 / area, l2 = e2 / area;
            float z = (float)(l0 * v[0].z + l1 * v[1].z + l2 * v[2].z);
            size_t pix = (size_t)y * T->W + x;
            if (rs->ztest && !(z + bias >= T->z[pix])) continue;
            double w0 = l0 * (g_affine ? 1.0 : v[0].iw), w1 = l1 * (g_affine ? 1.0 : v[1].iw), w2 = l2 * (g_affine ? 1.0 : v[2].iw), ws = w0 + w1 + w2;
            if (!(ws > 0)) continue;
            float at[7];
            for (int k = 0; k < 7; k++) at[k] = (float)((w0 * v[0].a[k] + w1 * v[1].a[k] + w2 * v[2].a[k]) / ws);
            float col[3] = {at[2], at[3], at[4]}, rgb[3];
            if (!shade(C, &pv[0], st, rs, at[0], at[1], col, at[5], at[6], rgb)) continue;
            float *d = &T->rgb[pix * 3];
            if (g_dbg && x == g_dbgx && y == g_dbgy) printf("    [px %d,%d] tri pass=%d blend=%d z=%.5f uv=(%.2f,%.2f) col=(%.3f,%.3f,%.3f) -> rgb=(%.3f,%.3f,%.3f) fl=%04x tpage=%04x clut=%04x dst=(%.3f,%.3f,%.3f)\n", x, y, rs->pass, rs->blend, z, at[0], at[1], col[0], col[1], col[2], rgb[0], rgb[1], rgb[2], pv[0].flags, pv[0].tpage, pv[0].clut, d[0], d[1], d[2]);
            if (rs->blend >= 0) { for (int k = 0; k < 3; k++) d[k] = vr_blend1(rs->blend, clamp01(d[k]), rgb[k]); }
            else { d[0] = rgb[0]; d[1] = rgb[1]; d[2] = rgb[2]; }
            T->cov[pix] = 1;
            if (rs->zwrite) T->z[pix] = z;
        }
}

/* Vértices de escena (vr_vs_scene) -> recorte contra near/far en espacio de recorte -> pantalla. */
typedef struct { double c[4]; float a[7]; } CVx;
static void vattr(const VrVtx *v, float a[7]) {
    a[0] = v->uv[0]; a[1] = v->uv[1];
    a[2] = (float)(v->rgba & 255u) * (1.0f / 128.0f); a[3] = (float)((v->rgba >> 8) & 255u) * (1.0f / 128.0f); a[4] = (float)((v->rgba >> 16) & 255u) * (1.0f / 128.0f);
    a[5] = v->pos[0]; a[6] = v->pos[1];
}
static void clip_plane(const CVx *in, int n, CVx *out, int *no, int which) {   /* which 0: w - z >= 0 (near, Z invertida); 1: z >= 0 (far) */
    *no = 0;
    for (int i = 0; i < n; i++) {
        const CVx *p = &in[i], *q = &in[(i + 1) % n];
        double dp = which == 0 ? p->c[3] - p->c[2] : p->c[2], dq = which == 0 ? q->c[3] - q->c[2] : q->c[2];
        if (dp >= 0) out[(*no)++] = *p;
        if ((dp >= 0) != (dq >= 0)) {
            double t = dp / (dp - dq); CVx m;
            for (int k = 0; k < 4; k++) m.c[k] = p->c[k] + (q->c[k] - p->c[k]) * t;
            for (int k = 0; k < 7; k++) m.a[k] = (float)(p->a[k] + (q->a[k] - p->a[k]) * t);
            out[(*no)++] = m;
        }
    }
}
static int g_nobb;   /* --no-billboard: sin carteles de cara al ojo (comportamiento anterior), para comparar */
static void draw_scene_tri(Tgt *T, const Ctx *C, const M4 *vp, const VrVtx *v, const Rs *rs) {
    CVx poly[8], tmp[8]; int n = 3, n2;
    double eyeD[3]; int have_eye = -1;   /* vr_vs_scene: el ojo sale de P*V (vr_eye_from_viewproj), solo si hay un vértice VR_F_BILLBOARD */
    for (int i = 0; i < 3; i++) {
        double x = v[i].pos[0], y = v[i].pos[1], z = v[i].pos[2];
        if ((v[i].flags & VR_F_BILLBOARD) && v[i].pad != 0xFFFF) {
            const VrState *bst = vr_scene_state(C->sc, v[i].pad);
            if (have_eye < 0) have_eye = vr_eye_from_viewproj(vp->m, eyeD);
            if (bst && have_eye) { float eyeF[3] = {(float)eyeD[0], (float)eyeD[1], (float)eyeD[2]}, o3[3]; vr_billboard_corner(bst->bb, eyeF, v[i].pos, o3); x = o3[0]; y = o3[1]; z = o3[2]; }
        }
        for (int r = 0; r < 4; r++) poly[i].c[r] = vp->m[r] * x + vp->m[4 + r] * y + vp->m[8 + r] * z + vp->m[12 + r];
        vattr(&v[i], poly[i].a);
    }
    clip_plane(poly, n, tmp, &n2, 0); if (n2 < 3) return;
    clip_plane(tmp, n2, poly, &n, 1); if (n < 3) return;
    SV sv[8];
    for (int i = 0; i < n; i++) {
        double w = poly[i].c[3];
        if (!(w > 0)) return;
        sv[i].x = (poly[i].c[0] / w * 0.5 + 0.5) * T->W; sv[i].y = (0.5 - poly[i].c[1] / w * 0.5) * T->H;
        sv[i].z = (float)(poly[i].c[2] / w); sv[i].iw = (float)(1.0 / w);
        memcpy(sv[i].a, poly[i].a, sizeof sv[i].a);
    }
    for (int t = 1; t + 1 < n; t++) { SV s3[3] = {sv[0], sv[t], sv[t + 1]}; draw_tri(T, C, v, s3, rs); }
}
/* vr_vs_hud: pos.xy en coordenadas de dibujo -> textura (el rect de display la cubre entera) */
static void draw_hud_tri(Tgt *T, const Ctx *C, const float rect[4], const VrVtx *v, const Rs *rs) {
    SV s3[3];
    for (int i = 0; i < 3; i++) {
        s3[i].x = (v[i].pos[0] - rect[0]) / rect[2] * T->W; s3[i].y = (v[i].pos[1] - rect[1]) / rect[3] * T->H;
        s3[i].z = 0; s3[i].iw = 1;
        vattr(&v[i], s3[i].a);
    }
    draw_tri(T, C, v, s3, rs);
}

static int batch_has_tex(const VrScene *sc, const VrBatch *b) {
    for (uint32_t j = 0; j < b->count; j++) if (sc->v[b->first + j].flags & GPU_F_TEXTURED) return 1;
    return 0;
}
/* drawGPU: lotes 3D (kind 0/1) en orden de envío */
static void draw_gpu(Tgt *T, const Ctx *C, const M4 *vp) {
    const VrScene *sc = C->sc;
    for (uint32_t bi = 0; bi < sc->nb; bi++) {
        const VrBatch *b = &sc->b[bi];
        if (b->kind > 1 || b->count < 3) continue;
        uint32_t cnt = b->count - b->count % 3;
        Rs rs; memset(&rs, 0, sizeof rs); rs.ztest = !C->noz; rs.blend = -1;
        if (b->kind == 0) { rs.zwrite = 1; for (uint32_t j = 0; j < cnt; j += 3) draw_scene_tri(T, C, vp, &sc->v[b->first + j], &rs); continue; }
        int hastex = batch_has_tex(sc, b);
        rs.zwrite = 0; rs.bias = 4; rs.slope = 2; rs.blend = b->blend & 3; rs.pass = hastex ? 1 : 0;
        for (uint32_t j = 0; j < cnt; j += 3) draw_scene_tri(T, C, vp, &sc->v[b->first + j], &rs);
        if (hastex) { rs.blend = -1; rs.pass = 2; for (uint32_t j = 0; j < cnt; j += 3) draw_scene_tri(T, C, vp, &sc->v[b->first + j], &rs); }
    }
}
/* encodeStereo2D: lotes kind 2 sobre la textura ya resuelta, sin profundidad */
static void draw_2d(Tgt *T, const Ctx *C, const float rect[4]) {
    const VrScene *sc = C->sc;
    for (uint32_t bi = 0; bi < sc->nb; bi++) {
        const VrBatch *b = &sc->b[bi];
        if (b->kind != 2 || b->count < 3) continue;
        uint32_t cnt = b->count - b->count % 3;
        int semi = b->blend <= 3, hastex = batch_has_tex(sc, b);
        Rs rs; memset(&rs, 0, sizeof rs); rs.hud = 1; rs.blend = -1;
        if (semi) { rs.blend = b->blend & 3; rs.pass = hastex ? 1 : 0; }
        for (uint32_t j = 0; j < cnt; j += 3) draw_hud_tri(T, C, rect, &sc->v[b->first + j], &rs);
        if (semi && hastex) { rs.blend = -1; rs.pass = 2; for (uint32_t j = 0; j < cnt; j += 3) draw_hud_tri(T, C, rect, &sc->v[b->first + j], &rs); }
    }
}

/* Onda por fila del agua (vr_post_ysrc + vr_fs_post, warpOnly): una pasada sobre la escena sin onda. Parámetros por ojo de la proyección. */
static float post_ysrc(float yc, const float eyeXY[2], const float cfg[4], const float *dv) {
    int nrows = (int)cfg[2];
    if (nrows > 0 && eyeXY[1] > 1.0f && cfg[0] > 0.0f) {
        float t = (yc - eyeXY[0]) / eyeXY[1], r = cfg[1] + cfg[0] * t, d = 0;
        if (r >= 0.0f && r < (float)VR_FILTER_ROWS) {
            float rr = r - 0.5f, f = floorf(rr), fr = rr - f;
            int i0 = (int)f, i1 = i0 + 1 < VR_FILTER_ROWS - 1 ? i0 + 1 : VR_FILTER_ROWS - 1;
            if (i0 < 0) i0 = 0;
            d = dv[i0] + (dv[i1] - dv[i0]) * fr;
        }
        return yc + eyeXY[1] * d / cfg[0];
    }
    return yc;
}
static void warp_pass(const Tgt *src, Tgt *dst, const M4 *proj, const float *dv, const float cfg[4]) {
    float eyeXY[2] = {(float)(dst->H * 0.5 * (1 + proj->m[9])), (float)(dst->H * 0.5 * proj->m[5])};
    for (int y = 0; y < dst->H; y++) {
        float ys = post_ysrc((float)y + 0.5f, eyeXY, cfg, dv);
        float ty = ys - 0.5f, fy = floorf(ty), wy = ty - fy; int y0 = (int)fy, y1 = y0 + 1;
        y0 = y0 < 0 ? 0 : (y0 >= src->H ? src->H - 1 : y0); y1 = y1 < 0 ? 0 : (y1 >= src->H ? src->H - 1 : y1);
        for (int x = 0; x < dst->W; x++) {   /* x entero: el centro del píxel cae sobre el centro del texel */
            for (int k = 0; k < 3; k++) dst->rgb[((size_t)y * dst->W + x) * 3 + k] = src->rgb[((size_t)y0 * src->W + x) * 3 + k] * (1 - wy) + src->rgb[((size_t)y1 * src->W + x) * 3 + k] * wy;
            dst->cov[(size_t)y * dst->W + x] = src->cov[(size_t)y0 * src->W + x] | src->cov[(size_t)y1 * src->W + x];
        }
    }
}

/* ------------------------------------------------------------------------------------------------ cámara (RenderRG.stereoCamera / XRStereo.eye) */
typedef struct { M4 view, proj; double t[4]; } EyeCam;
static int eye_cam(const float gv_cm[16], int eye, double ipd, double conv, const VrScreenWindow *w, EyeCam *out) {
    M4 gv; for (int i = 0; i < 16; i++) gv.m[i] = gv_cm[i];
    M4 gi;
    if (m4_inv(gv, &gi)) return -1;
    double tx = 0.5 * (w->left + w->right), ty = 0.5 * (w->up + w->down), dx = 0.5 * (w->left - w->right), dy = 0.5 * (w->up - w->down);
    double s = 0.001, o = eye == 0 ? -0.5 * ipd : 0.5 * ipd, sh = 0.5 * ipd / conv;
    M4 view = m4_mul(m4_trans(-o, 0, 0), m4_mul(m4_diag(s, -s, -s), gi));
    double t[4] = {eye == 0 ? tx - sh : tx + sh, eye == 0 ? tx + sh : tx - sh, ty, ty};
    t[0] += dx; t[1] -= dx; t[2] += dy; t[3] -= dy;
    if (!(t[0] > 0.01 && t[1] > 0.01 && t[2] > 0.01 && t[3] > 0.01)) return -1;
    memcpy(out->t, t, sizeof t);
    out->view = view; out->proj = m4_proj(t[0], t[1], t[2], t[3], NEAR_M, FAR_M);
    return 0;
}
/* Cabeza movida: C' = T(d) * Ry(a) alrededor del ojo * C  (C = ojo -> mundo). dlat = desplazamiento lateral horizontal (m), yaw_deg > 0 = a la izquierda (rotationY). */
static M4 head_moved(M4 view, double dlat, double yaw_deg) {
    M4 C; m4_inv(view, &C);
    double ex = C.m[12], ey = C.m[13], ez = C.m[14], rx = C.m[0], rz = C.m[2], rl = sqrt(rx * rx + rz * rz);
    if (rl > 1e-9) { rx /= rl; rz /= rl; }
    M4 rot = m4_mul(m4_trans(ex, ey, ez), m4_mul(m4_roty(yaw_deg * M_PI / 180.0), m4_trans(-ex, -ey, -ez)));
    M4 Cn = m4_mul(m4_trans(rx * dlat, 0, rz * dlat), m4_mul(rot, C));
    M4 vn; m4_inv(Cn, &vn);
    return vn;
}

/* ------------------------------------------------------------------------------------------------ remuestreo y lámina */
static void box_resample(const float *src, int sw, int sh, float *dst, int dw, int dh) {   /* media por área (reducción) o vecino más próximo (ampliación) */
    double fx = (double)sw / dw, fy = (double)sh / dh;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            double ax = x * fx, bx = (x + 1) * fx, ay = y * fy, by = (y + 1) * fy, acc[3] = {0, 0, 0}, wt = 0;
            if (fx <= 1 && fy <= 1) { int sx = (int)fmin(sw - 1, floor((x + 0.5) * fx)), sy = (int)fmin(sh - 1, floor((y + 0.5) * fy)); for (int k = 0; k < 3; k++) dst[((size_t)y * dw + x) * 3 + k] = src[((size_t)sy * sw + sx) * 3 + k]; continue; }
            for (int sy = (int)floor(ay); sy < (int)ceil(by) && sy < sh; sy++) {
                double wy = fmin(by, sy + 1) - fmax(ay, sy);
                for (int sx = (int)floor(ax); sx < (int)ceil(bx) && sx < sw; sx++) {
                    double w = wy * (fmin(bx, sx + 1) - fmax(ax, sx));
                    for (int k = 0; k < 3; k++) acc[k] += w * src[((size_t)sy * sw + sx) * 3 + k];
                    wt += w;
                }
            }
            for (int k = 0; k < 3; k++) dst[((size_t)y * dw + x) * 3 + k] = (float)(wt > 0 ? acc[k] / wt : 0);
        }
}
static uint8_t q8(float v) { int i = (int)lrintf(clamp01(v) * 255.0f); return (uint8_t)i; }

/* máscara de huella de una primitiva (clase 1 sprite, 2 tile, 3 poly2d, 4 line, 5 kind 3) en un lienzo de wxh px con `sc` px por px original, relativo al origen (ox, oy) del display */
static void fill_tri_mask(uint8_t *m, int W, int H, const double p[3][2], uint8_t val) {
    double minx = fmin(p[0][0], fmin(p[1][0], p[2][0])), maxx = fmax(p[0][0], fmax(p[1][0], p[2][0])), miny = fmin(p[0][1], fmin(p[1][1], p[2][1])), maxy = fmax(p[0][1], fmax(p[1][1], p[2][1]));
    double d = (p[1][1] - p[2][1]) * (p[0][0] - p[2][0]) + (p[2][0] - p[1][0]) * (p[0][1] - p[2][1]);
    if (fabs(d) < 1e-9) return;
    for (int y = (int)fmax(0, floor(miny)); y <= (int)fmin(H - 1, ceil(maxy)); y++)
        for (int x = (int)fmax(0, floor(minx)); x <= (int)fmin(W - 1, ceil(maxx)); x++) {
            double px = x + 0.5, py = y + 0.5;
            double a = ((p[1][1] - p[2][1]) * (px - p[2][0]) + (p[2][0] - p[1][0]) * (py - p[2][1])) / d, b = ((p[2][1] - p[0][1]) * (px - p[2][0]) + (p[0][0] - p[2][0]) * (py - p[2][1])) / d, c = 1 - a - b;
            if (a >= -1e-6 && b >= -1e-6 && c >= -1e-6) m[y * W + x] = val;
        }
}
enum { PC_NONE = 0, PC_SPRITE, PC_TILE, PC_POLY2D, PC_LINE, PC_FILTER };
static void op_footprint(uint8_t *m, int W, int H, double sc, const GpuDrawList *l, const GpuOp *op, uint8_t cls) {
    double ox = l->disp.x, oy = l->disp.y;
#define PT(X, Y) {((X) - ox) * sc, ((Y) - oy) * sc}
    if (op->type == GPU_OP_RECT) {
        double x0 = op->v[0].x, y0 = op->v[0].y, x1 = x0 + op->rw, y1 = y0 + op->rh;
        double a[3][2] = {PT(x0, y0), PT(x1, y0), PT(x0, y1)}, b[3][2] = {PT(x1, y0), PT(x1, y1), PT(x0, y1)};
        fill_tri_mask(m, W, H, a, cls); fill_tri_mask(m, W, H, b, cls);
    } else if (op->type == GPU_OP_POLY) {
        static const int idx[2][3] = {{0, 1, 2}, {1, 2, 3}};
        for (int q = 0; q < (op->nv == 4 ? 2 : 1); q++) {
            double a[3][2] = {PT(op->v[idx[q][0]].x, op->v[idx[q][0]].y), PT(op->v[idx[q][1]].x, op->v[idx[q][1]].y), PT(op->v[idx[q][2]].x, op->v[idx[q][2]].y)};
            fill_tri_mask(m, W, H, a, cls);
        }
    } else if (op->type == GPU_OP_LINE) {
        double x0 = (op->v[0].x - ox) * sc, y0 = (op->v[0].y - oy) * sc, x1 = (op->v[1].x - ox) * sc, y1 = (op->v[1].y - oy) * sc;
        int n = (int)fmax(fabs(x1 - x0), fabs(y1 - y0)) + 1;
        for (int i = 0; i <= n; i++) { int x = (int)floor(x0 + (x1 - x0) * i / n), y = (int)floor(y0 + (y1 - y0) * i / n); if (x >= 0 && y >= 0 && x < W && y < H) m[y * W + x] = cls; }
    }
#undef PT
}

/* ------------------------------------------------------------------------------------------------ render de un ojo completo (pantalla 3D en modo gpu) */
typedef struct { Tgt img; EyeCam cam; } EyeImg;
static int g_noz, g_semi;
static double g_rect_hp_z;   /* --rect-hp Z: simula la captura de vista en los RECT/sprites (los volcados anteriores no la traen) */
static int render_eye(const VrScene *sc, const uint16_t *vram, int filter, int eye, double ipd, double conv, int moved, const VrScreenWindow *w, const float rect[4], EyeImg *out) {
    if (eye_cam(sc->game_view, eye, ipd, conv, w, &out->cam)) return -1;
    if (moved) out->cam.view = head_moved(out->cam.view, 0.10, -15.0);
    M4 vp = m4_mul(out->cam.proj, out->cam.view);
    Ctx C = {vram, filter, sc, g_noz};
    Tgt scene = tgt_new(TW, TH);
    draw_gpu(&scene, &C, &vp);
    const VrFilter *f = &sc->filter;
    int warp = 0; float dv[VR_FILTER_ROWS]; memset(dv, 0, sizeof dv);
    if (f->active) {
        for (int i = 0; i < VR_FILTER_ROWS; i++) if (f->rowcov[i] && isfinite(f->dv[i]) && fabsf(f->dv[i]) < 64) { dv[i] = f->dv[i]; if (dv[i] != 0) warp = 1; }
    }
    if (warp) {
        float cfg[4] = {isfinite(f->proj_h) && f->proj_h > 1 ? f->proj_h : 160.0f, isfinite(f->cy_row) ? f->cy_row : 128.0f, (float)(f->nrows > 0 ? f->nrows : 1), isfinite(f->cx_col) ? f->cx_col : 160.0f};
        Tgt dst = tgt_new(TW, TH);
        warp_pass(&scene, &dst, &out->cam.proj, dv, cfg);
        tgt_free(&scene); scene = dst;
    }
    draw_2d(&scene, &C, rect);
    /* screenFx al presentar: tinte y recubrimientos (gamma) */
    if (f->active) {
        float tint[3]; for (int k = 0; k < 3; k++) tint[k] = isfinite(f->tint[k]) ? fminf(fmaxf(f->tint[k], 0), 4) : 1;
        float cv[4][4]; int nc = 0;
        for (int i = 0; i < f->n_covers && i < VR_FILTER_COVERS; i++) if (f->covers_abr[i] >= 0) { for (int k = 0; k < 3; k++) cv[nc][k] = clamp01(f->covers_rgb[i][k]); cv[nc][3] = (float)(f->covers_abr[i] & 3); nc++; }
        if (nc == 0 && f->cover_abr >= 0) { for (int k = 0; k < 3; k++) cv[0][k] = clamp01(f->cover_rgb[k]); cv[0][3] = (float)(f->cover_abr & 3); nc = 1; }
        if (nc || tint[0] != 1 || tint[1] != 1 || tint[2] != 1)
            for (size_t i = 0; i < (size_t)TW * TH; i++) {
                float *c = &scene.rgb[i * 3];
                for (int k = 0; k < 3; k++) c[k] = clamp01(c[k] * tint[k]);
                for (int q = 0; q < nc; q++) for (int k = 0; k < 3; k++) c[k] = vr_blend1((int)cv[q][3], c[k], cv[q][k]);
            }
    }
    out->img = scene;
    return 0;
}

static void put_panel(uint8_t *sheet, int SW, int px, int py, const float *rgb) {   /* rgb PWxPH float */
    for (int y = 0; y < PH; y++) for (int x = 0; x < PW; x++) {
        uint8_t *d = &sheet[((size_t)(py + y) * SW + px + x) * 4];
        d[0] = q8(rgb[((size_t)y * PW + x) * 3]); d[1] = q8(rgb[((size_t)y * PW + x) * 3 + 1]); d[2] = q8(rgb[((size_t)y * PW + x) * 3 + 2]); d[3] = 255;
    }
}

/* tipo de la primitiva que cubre un píxel (modo lote) */
enum { BK_FONDO = 0, BK_POLY3D, BK_POLY2D, BK_SPRITE, BK_TILE, BK_CARTEL3D, BK_LINEA3D, BK_LINEA2D, BK_FILTRO, BK_SINSALIDA, BK_N };
static const char *const bk_name[BK_N] = {"fondo", "poly3d", "poly2d", "sprite", "tile", "cartel3d", "linea3d", "linea2d", "filtro", "sin_salida"};
typedef struct { char motivo[48]; double dif_f; int ntop, top[3]; double top_pct[3]; } BatchRow;   /* dif_f = % de píxeles distintos a-f (ipd 0) */
typedef struct { double ipd, conv; int filter, maxrects, probe_x, probe_y, batch, sheets; const char *outp; } DvOpts;

/* Procesa un volcado. row != NULL = modo lote (rellena la fila; sin lámina salvo O->sheets; solo el ojo ipd 0). 0 = bien. */
static int run_dump(const char *dir, const DvOpts *O, BatchRow *row) {
    double ipd = O->ipd, conv = O->conv; int filter = O->filter, maxrects = O->maxrects, probe_x = O->probe_x, probe_y = O->probe_y;
    const char *outp = O->outp;
    const int full = !row || O->sheets;   /* ojos b y c y lámina */
    int ret = 0;
    NsDump *d = NULL; uint8_t *fate = NULL, *ref = NULL, *fp_m = NULL, *diffmap = NULL, *cm = NULL, *sheet = NULL;
    float *ref_f = NULL, *b_d = NULL, *f_d = NULL, *p = NULL, *pb = NULL;
    EyeImg b_, c_, f_;
    memset(&b_, 0, sizeof b_); memset(&c_, 0, sizeof c_); memset(&f_, 0, sizeof f_);
    if (row) memset(row, 0, sizeof *row);
    d = ns_dump_open(dir);
    if (!d) { fprintf(stderr, "no se pudo abrir %s\n", dir); return 2; }
    if (row) { char mb[48]; const char *pt = ns_dump_meta(d, "point", mb, sizeof mb); snprintf(row->motivo, sizeof row->motivo, "%s", pt ? pt : "?"); }
    if (!d->has_list || !d->vram) { fprintf(stderr, "%s: sin list.bin/vram.bin: sin lista no hay posiciones de vista (GTE) de los vertices y el 3D no se puede reconstruir desde ram.bin\n", dir); ret = 2; goto done; }
    GpuDrawList l = d->list; l.vram_start = d->vram; l.vram_end = NULL;
    uint32_t nops = l.nops;

    /* ---- registro de cámara y puente a partir del meta.txt (lo que vrhost.c build_one saca de list_record / bs) */
    VrCamRecord rec; memset(&rec, 0, sizeof rec);
    float M[9], eye[3], pref[3] = {0, 0, 0};
    int have_rec = ns_dump_meta_floats(d, "rec_M", M, 9) == 9 && ns_dump_meta_floats(d, "rec_eye", eye, 3) == 3;
    BridgeState bs; memset(&bs, 0, sizeof bs);
    VrSceneOpts o = vr_scene_opts_default();
    o.screen_filter = 1;                                                              /* on_start */
    o.eye_near_m = 0.10f; o.line3d_px = 1.0f; o.coplanar_m = 0.005f; o.rect3d = 1; o.billboard = !g_nobb;       /* modo gpu (build_one) */
    int view_kind = (int)ns_dump_meta_num(d, "view_mode", 1);
    if (view_kind == VRV_K_ENGINE) { o.keep_late_clear = 1; o.veil_as_2d = 1; }
    int rec_active = 0;
    if (have_rec) {
        ns_dump_meta_floats(d, "rec_p_ref", pref, 3);
        rec.serial = 1; rec.active = rec_active = (int)ns_dump_meta_num(d, "rec_active", 0) != 0;
        for (int i = 0; i < 3; i++) rec.eye[i] = (int32_t)lrintf(eye[i]);
        memcpy(rec.M, M, sizeof M); memcpy(rec.p_ref, pref, sizeof pref);
        rec.theta_play_deg = (float)ns_dump_meta_num(d, "rec_theta", 0); rec.units_per_m = (float)ns_dump_meta_num(d, "rec_units_per_m", 1000);
        bs.valid = BRIDGE_OK_EYE;
        for (int i = 0; i < 3; i++) { bs.eye_pos[i] = rec.eye[i]; for (int j = 0; j < 3; j++) bs.eye_axes[i][j] = (int16_t)lrintf(M[3 * i + j] * 4096.0f); }
        if ((int)ns_dump_meta_num(d, "game", 0)) {   /* has_player + núcleo del puente: world_valid y origen bajo el jugador */
            bs.valid = BRIDGE_OK_ALL_CORE; bs.has_player = 1;
            bs.player_pos.x = (int16_t)ns_dump_meta_num(d, "player_x", 0); bs.player_pos.y = (int16_t)ns_dump_meta_num(d, "player_y", 0); bs.player_pos.z = (int16_t)ns_dump_meta_num(d, "player_z", 0);
        }
        if (rec_active) {
            float org[3]; vr_scene_player_origin(&bs, &o, org);
            o.origin_mode = 1; o.origin_game[0] = (float)rec.eye[0]; o.origin_game[1] = org[1]; o.origin_game[2] = (float)rec.eye[2];
            vrcam_scene_user_map(&o, &rec, org[1]);
        }
    }
    if (g_rect_hp_z > 0) {   /* a cada sprite texturizado de tamaño de sprite (<= 64 px) le da la vista de un punto a profundidad Z con la proyección del primer polígono 3D de la lista (H, OFX/OFY): como si su XY viniera de un RTPS */
        const GpuOp *pr = NULL;
        for (uint32_t i = 0; i < nops && !pr; i++) if (l.ops[i].type == GPU_OP_POLY && l.ops[i].proj_h > 0 && l.ops[i].v[0].vvalid) pr = &l.ops[i];
        uint32_t nsim = 0;
        for (uint32_t i = 0; pr && i < nops; i++) {
            GpuOp *r = &l.ops[i];
            if (r->type != GPU_OP_RECT || !(r->flags & GPU_F_TEXTURED) || r->rw > 64 || r->rh > 64) continue;
            r->proj_h = pr->proj_h; r->proj_ox = pr->proj_ox; r->proj_oy = pr->proj_oy;
            r->v[0].vvalid = 1; r->v[0].vflags = 0; r->v[0].vz = (float)g_rect_hp_z;
            r->v[0].vx = (float)(r->v[0].x - r->dox - pr->proj_ox) * (float)g_rect_hp_z / pr->proj_h; r->v[0].vy = (float)(r->v[0].y - r->doy - pr->proj_oy) * (float)g_rect_hp_z / pr->proj_h;
            nsim++;
        }
        printf("--rect-hp %.0f: %u sprites con vista simulada (H=%.0f OFX/OFY=%.0f,%.0f)\n", g_rect_hp_z, nsim, pr ? pr->proj_h : 0.0f, pr ? pr->proj_ox : 0.0f, pr ? pr->proj_oy : 0.0f);
    }
    fate = calloc(nops + 1, 1);
    vr_scene_set_fate(fate, nops);
    VrScene sc;
    int rc = vr_scene_build(&l, &bs, &o, &sc);
    if (rc == 0 && o.screen_filter && sc.filter.active && sc.filter.n_strips == 0 && sc.filter.n_polys == 0 &&
        !vrhost_keep_filter(0, 0, sc.stats.n_tris3d >= 12, sc.has_3d, sc.world_valid, sc.filter.n_scan, sc.filter.frame_on, sc.filter.n_strips)) {
        VrSceneOpts o2 = o; o2.screen_filter = 0;
        memset(fate, 0, nops + 1);
        rc = vr_scene_build(&l, &bs, &o2, &sc);
    }
    if (rc) { fprintf(stderr, "vr_scene_build fallo (%d)\n", rc); ret = 2; goto done; }
    VrScreenWindow win; { VrFilter ff = sc.filter; win = vr_scene_screen_window(&ff); }
    float rect[4] = {(float)l.disp.x, (float)l.disp.y, l.disp.w > 0 ? (float)l.disp.w : 320.0f, l.disp.h > 0 ? (float)l.disp.h : 240.0f};
    printf("== %s\n", dir);
    printf("vista=%d rec_active=%d has_3d=%d world_valid=%d user_space=%d | disp %d,%d %dx%d | ventana L=%.4f R=%.4f U=%.4f D=%.4f ok=%d proj_h=%.1f | lotes: %u (v=%u)\n", view_kind, rec_active, sc.has_3d, sc.world_valid, sc.user_space,
           l.disp.x, l.disp.y, l.disp.w, l.disp.h, win.left, win.right, win.up, win.down, win.ok, sc.filter.proj_h, sc.nb, sc.nv);
    if (!win.ok) { fprintf(stderr, "sin ventana de proyeccion en esta lista: la app no dibuja un estereo nuevo con 3D (stereoWindow nil)\n"); if (sc.has_3d) { ret = 3; goto done; } win.left = win.right = 0.5f; win.up = win.down = 0.4f; }

    /* ---- recuento de primitivas */
    uint32_t n_p3 = 0, n_phpdrop = 0, n_p2 = 0, n_sprite = 0, n_tile = 0, n_l3 = 0, n_l2 = 0, n_ev = 0, n_k2 = 0, n_k3 = 0, n_gone = 0;
    for (uint32_t i = 0; i < nops; i++) {
        const GpuOp *op = &l.ops[i]; uint8_t dest = (sc.op_dest && i < sc.n_op_dest) ? sc.op_dest[i] : 0;
        if (op->type == GPU_OP_POLY) {
            int allhp = 1; for (int k = 0; k < op->nv; k++) if (!op->v[k].vvalid) allhp = 0;
            if (fate[i] == VR_FATE_3D || fate[i] == VR_FATE_RECON) n_p3++; else if (allhp) n_phpdrop++; else n_p2++;
        } else if (op->type == GPU_OP_RECT) { if (op->flags & GPU_F_TEXTURED) n_sprite++; else n_tile++; }
        else if (op->type == GPU_OP_LINE) { if (fate[i] == VR_FATE_3D) n_l3++; else n_l2++; }
        else n_ev++;
        if (dest == 2) n_k2++; else if (dest == 3) n_k3++;
        if (op->type != GPU_OP_FILL && op->type != GPU_OP_VCOPY && op->type != GPU_OP_UPLOAD && op->type != GPU_OP_DOWNLOAD && dest == 0 && fate[i] != VR_FATE_3D && fate[i] != VR_FATE_RECON) n_gone++;
    }
    {   /* la GPU original no dibuja un triángulo con extensión > 1023 en X o > 511 en Y (vr_scene lo aplica a todo polígono, 2D o 3D, y a las líneas): recuento independiente sobre la lista */
        uint32_t nbig = 0, nbig_op = 0, nfull = 0; static const int idx[2][3] = {{0, 1, 2}, {1, 2, 3}};
        for (uint32_t i = 0; i < nops; i++) {
            const GpuOp *op = &l.ops[i]; if (op->type != GPU_OP_POLY) continue;
            int any = 0, cnt = 0, ntri = op->nv == 4 ? 2 : 1;
            for (int q = 0; q < ntri; q++) {
                int x0 = 1 << 20, x1 = -(1 << 20), y0 = 1 << 20, y1 = -(1 << 20);
                for (int k = 0; k < 3; k++) { const GpuVtx *a = &op->v[idx[q][k]]; if (a->x < x0) x0 = a->x; if (a->x > x1) x1 = a->x; if (a->y < y0) y0 = a->y; if (a->y > y1) y1 = a->y; }
                int big = x1 - x0 > 1023 || y1 - y0 > 511; nbig += big; any |= big; cnt += big;
            }
            nbig_op += any; nfull += cnt == ntri;
        }
        printf("GPU original: triangulos descartados por tamano (> 1023x511) en la lista: %u de %u polys (%u enteros) | vr_scene n_hw_cull=%u (polys+lineas)\n", nbig, nbig_op, nfull, sc.stats.n_hw_cull);
        printf("RECT/sprites: total=%u con vista de la GTE (n_rect_hp)=%u emitidos como cartel 3D (n_rect3d)=%u | cuadrilateros de cara al ojo (n_billboard)=%u\n", sc.stats.n_rect, sc.stats.n_rect_hp, sc.stats.n_rect3d, sc.stats.n_billboard);
    }
    if (g_semi) {
        printf("semitransparentes (abr 0 (B+F)/2, 1 B+F aditivo, 2 B-F, 3 B+F/4):\n");
        for (uint32_t i = 0; i < nops; i++) {
            const GpuOp *op = &l.ops[i]; if (!(op->flags & GPU_F_SEMI) || (op->type != GPU_OP_POLY && op->type != GPU_OP_RECT && op->type != GPU_OP_LINE)) continue;
            int nvv = op->type == GPU_OP_POLY ? op->nv : op->type == GPU_OP_LINE ? 2 : 1, hp = 0, x0 = 1 << 20, y0 = 1 << 20, x1 = -(1 << 20), y1 = -(1 << 20);
            for (int k = 0; k < nvv; k++) { hp += op->v[k].vvalid; if (op->v[k].x < x0) x0 = op->v[k].x; if (op->v[k].x > x1) x1 = op->v[k].x; if (op->v[k].y < y0) y0 = op->v[k].y; if (op->v[k].y > y1) y1 = op->v[k].y; }
            if (op->type == GPU_OP_RECT) { x1 = x0 + op->rw; y1 = y0 + op->rh; }
            printf("  #%u %s abr=%u %s tp=%u tpage=(%u,%u) clut=(%u,%u) box=(%d,%d)-(%d,%d) rgb0=%u,%u,%u hp=%d/%d fate=%u dest=%u\n", i, op->type == GPU_OP_POLY ? (op->nv == 4 ? "QUAD" : "TRI ") : op->type == GPU_OP_RECT ? "RECT" : "LINE", op->abr,
                   (op->flags & GPU_F_TEXTURED) ? "tex" : "flat", op->tp, op->tx, op->ty, op->clx, op->cly, x0 - l.disp.x, y0 - l.disp.y, x1 - l.disp.x, y1 - l.disp.y, op->v[0].r, op->v[0].g, op->v[0].b, hp, nvv, fate[i], (sc.op_dest && i < sc.n_op_dest) ? sc.op_dest[i] : 0);
            int dub = hp < nvv; for (int k = 0; k < nvv; k++) dub |= op->v[k].vflags != 0;
            if (dub && op->type == GPU_OP_POLY) { printf("      H=%.0f ofs=(%.0f,%.0f) v:", op->proj_h, op->proj_ox, op->proj_oy); for (int k = 0; k < nvv; k++) printf(" [(%d,%d) %s view=(%.0f,%.0f,%.0f) fl=%u]", op->v[k].x - op->dox, op->v[k].y - op->doy, op->v[k].vvalid ? "hp" : "--", op->v[k].vx, op->v[k].vy, op->v[k].vz, op->v[k].vflags); printf("\n"); }
        }
    }
    uint32_t tris3d = 0, tris2d = 0, tris3 = 0;
    for (uint32_t b = 0; b < sc.nb; b++) { uint32_t t = sc.b[b].count / 3; if (sc.b[b].kind <= 1) tris3d += t; else if (sc.b[b].kind == 2) tris2d += t; else tris3 += t; }
    printf("prims: ops=%u | POLY3D(con hp, emitidos)=%u POLY con hp descartados=%u POLY2D(sin hp)=%u | RECT/sprite(tex)=%u TILE(sin tex)=%u | LINE 3D=%u LINE 2D=%u | eventos VRAM=%u\n", nops, n_p3, n_phpdrop, n_p2, n_sprite, n_tile, n_l3, n_l2, n_ev);
    printf("destino: kind2(panel 2D)=%u prims (%u tris) | kind3(filtro)=%u prims (%u tris) | 3D=%u tris | sin salida (descartadas)=%u | filtro activo=%d strips=%d covers=%d tint=%.2f,%.2f,%.2f\n", n_k2, tris2d, n_k3, tris3, tris3d, n_gone,
           sc.filter.active, sc.filter.n_strips, sc.filter.n_covers, sc.filter.tint[0], sc.filter.tint[1], sc.filter.tint[2]);
    {   /* RECT/sprites: posición y plano */
        printf("RECT/sprites (disp-relativo; plano: kind2 = panel 2D compuesto sobre el 3D en la ventana de la lista, texel = %.2f x %.2f px de textura estereo):\n", (double)TW / rect[2], (double)TH / rect[3]);
        int shown = 0;
        for (uint32_t i = 0; i < nops; i++) {
            const GpuOp *op = &l.ops[i]; if (op->type != GPU_OP_RECT) continue;
            uint8_t dest = (sc.op_dest && i < sc.n_op_dest) ? sc.op_dest[i] : 0;
            if (shown++ >= maxrects) continue;
            printf("  #%u %s pos=(%d,%d) %dx%d uv=(%u,%u) tp=%u clut=(%u,%u) %s%s -> %s | tex-px=(%.0f,%.0f)\n", i, (op->flags & GPU_F_TEXTURED) ? "sprite" : "TILE  ", op->v[0].x - l.disp.x, op->v[0].y - l.disp.y, op->rw, op->rh,
                   op->v[0].u, op->v[0].v, op->tp, op->clx, op->cly, (op->flags & GPU_F_SEMI) ? "semi" : "opaco", (op->flags & GPU_F_RAW) ? "+raw" : "",
                   dest == 2 ? "kind2 (plano 2D)" : dest == 3 ? "kind3 (filtro)" : fate[i] == VR_FATE_3D ? "3D (cartel con vista)" : "SIN SALIDA (descartado)", (op->v[0].x - l.disp.x) * (double)TW / rect[2], (op->v[0].y - l.disp.y) * (double)TH / rect[3]);
        }
        if (shown > maxrects) printf("  ... %d mas (--rects N)\n", shown - maxrects);
    }

    /* ---- renders */
    if ((full && (render_eye(&sc, d->vram, filter, 0, ipd, conv, 0, &win, rect, &b_) || render_eye(&sc, d->vram, filter, 0, ipd, conv, 1, &win, rect, &c_))) || render_eye(&sc, d->vram, filter, 0, 0.0, conv, 0, &win, rect, &f_)) {
        fprintf(stderr, "camara estereo invalida (game_view no valido)\n"); ret = 3; goto done;
    }
    if (probe_x >= 0) {   /* traza de los fragmentos que escriben el píxel de la textura estéreo bajo el píxel original, ojo ipd 0 */
        g_dbgx = (int)((probe_x + 0.5) * TW / rect[2]); g_dbgy = (int)((probe_y + 0.5) * TH / rect[3]); g_dbg = 1;
        EyeImg t_; memset(&t_, 0, sizeof t_);
        printf("traza ipd 0 en textura (%d,%d):\n", g_dbgx, g_dbgy);
        render_eye(&sc, d->vram, filter, 0, 0.0, conv, 0, &win, rect, &t_);
        g_dbg = 0; tgt_free(&t_.img);
    }
    int rw = 0, rh = 0; char fp[700]; snprintf(fp, sizeof fp, "%s/frame.png", dir);
    ref = png_read_stored(fp, &rw, &rh);
    if (!ref) { fprintf(stderr, "sin frame.png legible\n"); ret = 3; goto done; }
    int DW = rw, DH = rh;   /* dominio de comparación = imagen original */
    ref_f = malloc((size_t)DW * DH * 3 * 4); b_d = full ? malloc((size_t)DW * DH * 3 * 4) : NULL; f_d = malloc((size_t)DW * DH * 3 * 4);
    for (int i = 0; i < DW * DH; i++) for (int k = 0; k < 3; k++) ref_f[i * 3 + k] = ref[i * 4 + k] / 255.0f;
    if (full) box_resample(b_.img.rgb, TW, TH, b_d, DW, DH);
    box_resample(f_.img.rgb, TW, TH, f_d, DW, DH);
    /* huella no 3D en el dominio de comparación */
    fp_m = calloc((size_t)DW * DH, 1);
    for (uint32_t i = 0; i < nops; i++) {
        const GpuOp *op = &l.ops[i]; uint8_t dest = (sc.op_dest && i < sc.n_op_dest) ? sc.op_dest[i] : 0;
        if (dest != 2 && dest != 3) continue;
        uint8_t cls = dest == 3 ? PC_FILTER : op->type == GPU_OP_RECT ? ((op->flags & GPU_F_TEXTURED) ? PC_SPRITE : PC_TILE) : op->type == GPU_OP_LINE ? PC_LINE : PC_POLY2D;
        op_footprint(fp_m, DW, DH, 1.0, &l, op, cls);
    }
    long nd_b = 0, nd_f = 0, nd_c = 0, in_fp = 0, hole = 0, other = 0, hole_b = 0, hole_c = 0;
    diffmap = malloc((size_t)DW * DH);
    long cnt_bk[BK_N] = {0};
    if (row) {   /* tipo de la última primitiva que cubre cada píxel (orden de la lista) */
        cm = calloc((size_t)DW * DH, 1);
        for (uint32_t i = 0; i < nops; i++) {
            const GpuOp *op = &l.ops[i]; uint8_t dest = (sc.op_dest && i < sc.n_op_dest) ? sc.op_dest[i] : 0, k;
            if (op->type != GPU_OP_POLY && op->type != GPU_OP_RECT && op->type != GPU_OP_LINE) continue;
            int is3d = fate[i] == VR_FATE_3D || fate[i] == VR_FATE_RECON;
            if (dest == 0 && !is3d) k = BK_SINSALIDA;
            else if (dest == 3) k = BK_FILTRO;
            else if (op->type == GPU_OP_POLY) k = is3d ? BK_POLY3D : BK_POLY2D;
            else if (op->type == GPU_OP_RECT) k = is3d ? BK_CARTEL3D : (op->flags & GPU_F_TEXTURED) ? BK_SPRITE : BK_TILE;
            else k = is3d ? BK_LINEA3D : BK_LINEA2D;
            op_footprint(cm, DW, DH, 1.0, &l, op, k);
        }
    }
    for (int i = 0; i < DW * DH; i++) {
        int mb = 0, mf = 0;
        for (int k = 0; k < 3; k++) {
            int a = ref[i * 4 + k], ff = q8(f_d[i * 3 + k]);
            if (full) { int bb = q8(b_d[i * 3 + k]); mb = abs(a - bb) > mb ? abs(a - bb) : mb; }
            mf = abs(a - ff) > mf ? abs(a - ff) : mf;
        }
        diffmap[i] = (uint8_t)mb;
        if (mb > 24) {
            nd_b++;
            if (fp_m[i]) in_fp++; else { int sx = (int)((i % DW + 0.5) * TW / DW), sy = (int)((i / DW + 0.5) * TH / DH); if (!b_.img.cov[(size_t)sy * TW + sx]) hole++; else other++; }
        }
        if (mf > 24) { nd_f++; if (cm) cnt_bk[cm[i]]++; }
    }
    if (full) for (size_t i = 0; i < (size_t)TW * TH; i++) { hole_b += !b_.img.cov[i]; hole_c += !c_.img.cov[i]; }
    if (row) {
        row->dif_f = 100.0 * nd_f / ((double)DW * DH);
        for (int t = 0; t < 3; t++) {
            int best = -1;
            for (int k = 0; k < BK_N; k++) if (cnt_bk[k] > 0 && (best < 0 || cnt_bk[k] > cnt_bk[best])) best = k;
            if (best < 0) break;
            row->top[row->ntop] = best; row->top_pct[row->ntop++] = 100.0 * cnt_bk[best] / nd_f; cnt_bk[best] = 0;
        }
    }
    (void)nd_c;
    if (full) {   /* brillo medio por canal: a (original) frente a f (ipd 0) y b */
        double ma[3] = {0, 0, 0}, mf[3] = {0, 0, 0}, mb[3] = {0, 0, 0};
        for (int i = 0; i < DW * DH; i++) for (int k = 0; k < 3; k++) { ma[k] += ref[i * 4 + k]; mf[k] += q8(f_d[i * 3 + k]); mb[k] += q8(b_d[i * 3 + k]); }
        for (int k = 0; k < 3; k++) { ma[k] /= DW * DH; mf[k] /= DW * DH; mb[k] /= DW * DH; }
        printf("brillo medio RGB: a=%.1f,%.1f,%.1f  b=%.1f,%.1f,%.1f  f=%.1f,%.1f,%.1f\n", ma[0], ma[1], ma[2], mb[0], mb[1], mb[2], mf[0], mf[1], mf[2]);
    }
    printf("dif a-b (ojo izq. ipd %.3f, umbral 24/255): %.2f%% de %dx%d px | de ellos: bajo huella no-3D %.1f%%, hueco sin dibujar %.1f%%, resto (3D/otras) %.1f%%\n", ipd, 100.0 * nd_b / (DW * DH), DW, DH,
           nd_b ? 100.0 * in_fp / nd_b : 0, nd_b ? 100.0 * hole / nd_b : 0, nd_b ? 100.0 * other / nd_b : 0);
    printf("dif a-f (ipd 0, camara del juego exacta): %.2f%% | huecos sin dibujar (fondo negro): b=%.2f%% c(cabeza movida)=%.2f%%\n", 100.0 * nd_f / (DW * DH), 100.0 * hole_b / ((double)TW * TH), 100.0 * hole_c / ((double)TW * TH));

    /* ---- lámina 3x2 */
    if (!full) goto done;
    int G = 4, SW = 3 * PW + 2 * G, SH = 2 * PH + G;
    sheet = calloc((size_t)SW * SH * 4, 1);
    for (size_t i = 0; i < (size_t)SW * SH; i++) { sheet[i * 4] = sheet[i * 4 + 1] = sheet[i * 4 + 2] = 40; sheet[i * 4 + 3] = 255; }
    p = malloc((size_t)PW * PH * 3 * 4); pb = malloc((size_t)PW * PH * 3 * 4);
    box_resample(ref_f, DW, DH, p, PW, PH); put_panel(sheet, SW, 0, 0, p);                                               /* a */
    box_resample(b_.img.rgb, TW, TH, pb, PW, PH); put_panel(sheet, SW, PW + G, 0, pb);                                  /* b */
    box_resample(c_.img.rgb, TW, TH, p, PW, PH); put_panel(sheet, SW, 2 * (PW + G), 0, p);                              /* c */
    {   /* d */
        float *dd = malloc((size_t)PW * PH * 3 * 4);
        for (int y = 0; y < PH; y++) for (int x = 0; x < PW; x++) {
            int sx = x * DW / PW, sy = y * DH / PH; int df = diffmap[sy * DW + sx];
            float *q = &dd[((size_t)y * PW + x) * 3];
            if (df > 24) { q[0] = 1; q[1] = q[2] = 0.25f * (1.0f - (float)(df - 24) / 231.0f); } else { q[0] = q[1] = q[2] = fminf(1.0f, df / 24.0f * 0.45f); }
        }
        put_panel(sheet, SW, 0, PH + G, dd); free(dd);
    }
    {   /* e */
        float *e = malloc((size_t)PW * PH * 3 * 4); memcpy(e, pb, (size_t)PW * PH * 3 * 4);
        for (size_t i = 0; i < (size_t)PW * PH * 3; i++) e[i] *= 0.45f;
        uint8_t *m = calloc((size_t)PW * PH, 1);
        for (uint32_t i = 0; i < nops; i++) {
            const GpuOp *op = &l.ops[i]; uint8_t dest = (sc.op_dest && i < sc.n_op_dest) ? sc.op_dest[i] : 0;
            if (dest != 2 && dest != 3) continue;
            uint8_t cls = dest == 3 ? PC_FILTER : op->type == GPU_OP_RECT ? ((op->flags & GPU_F_TEXTURED) ? PC_SPRITE : PC_TILE) : op->type == GPU_OP_LINE ? PC_LINE : PC_POLY2D;
            op_footprint(m, PW, PH, (double)PW / DW, &l, op, cls);
        }
        static const float col[6][3] = {{0, 0, 0}, {1, 0.1f, 0.1f}, {1, 0.9f, 0.1f}, {0.1f, 0.9f, 1}, {0.2f, 1, 0.2f}, {1, 0.2f, 1}};
        long cnt[6] = {0};
        for (int i = 0; i < PW * PH; i++) if (m[i]) { cnt[m[i]]++; for (int k = 0; k < 3; k++) e[i * 3 + k] = 0.45f * e[i * 3 + k] + 0.55f * col[m[i]][k] * (pb[i * 3 + k] * 0.7f + 0.3f); }
        printf("lamina (e): rojo=sprite(RECT tex) amarillo=TILE cian=POLY 2D verde=LINE 2D magenta=kind3(filtro) | px (x0.25 de panel): %ld %ld %ld %ld %ld\n", cnt[1], cnt[2], cnt[3], cnt[4], cnt[5]);
        put_panel(sheet, SW, PW + G, PH + G, e); free(e); free(m);
    }
    box_resample(f_.img.rgb, TW, TH, p, PW, PH); put_panel(sheet, SW, 2 * (PW + G), PH + G, p);                         /* f */
    if (probe_x >= 0) {   /* primitivas de la lista que cubren el píxel (x,y) de la imagen original, en orden de dibujo, y los valores de a / b / f */
        int pi = probe_y * DW + probe_x;
        printf("probe (%d,%d): a=%d,%d,%d b=%d,%d,%d f=%d,%d,%d\n", probe_x, probe_y, ref[pi * 4], ref[pi * 4 + 1], ref[pi * 4 + 2], q8(b_d[pi * 3]), q8(b_d[pi * 3 + 1]), q8(b_d[pi * 3 + 2]), q8(f_d[pi * 3]), q8(f_d[pi * 3 + 1]), q8(f_d[pi * 3 + 2]));
        for (uint32_t i = 0; i < nops; i++) {
            const GpuOp *op = &l.ops[i]; uint8_t mm[1] = {0};
            if (op->type != GPU_OP_POLY && op->type != GPU_OP_RECT) continue;
            uint8_t *m = calloc((size_t)DW * DH, 1); op_footprint(m, DW, DH, 1.0, &l, op, 1); mm[0] = m[pi]; free(m);
            if (!mm[0]) continue;
            printf("  #%u %s nv=%d fl=%04x abr=%u tp=%u tpage=(%u,%u) clut=(%u,%u) fate=%u dest=%u rgb0=%u,%u,%u uv0=%u,%u hp=%d\n", i, op->type == GPU_OP_POLY ? "POLY" : "RECT", op->nv, op->flags, op->abr, op->tp, op->tx, op->ty, op->clx, op->cly,
                   fate[i], (sc.op_dest && i < sc.n_op_dest) ? sc.op_dest[i] : 0, op->v[0].r, op->v[0].g, op->v[0].b, op->v[0].u, op->v[0].v, op->v[0].vvalid);
            if (op->type == GPU_OP_POLY) { printf("      v:"); for (int k = 0; k < op->nv; k++) printf(" (%d,%d vz=%.0f fl=%u)", op->v[k].x - l.disp.x, op->v[k].y - l.disp.y, op->v[k].vz, op->v[k].vflags); printf("\n"); }
        }
    }
    {
        char sp[700]; if (row) snprintf(sp, sizeof sp, "%s/dumpview.png", dir);
        const char *wp = row ? sp : outp;
        if (ns_png_write_rgba(wp, sheet, SW, SH)) { fprintf(stderr, "no se pudo escribir %s\n", wp); ret = 2; goto done; }
        printf("lamina: %s (%dx%d) | fila 1: a=frame.png b=ojo izq. c=cabeza +0,10 m y 15 grados dcha. | fila 2: d=dif (rojo>24) e=no-3D resaltado f=ipd 0\n", wp, SW, SH);
    }
done:
    tgt_free(&b_.img); tgt_free(&c_.img); tgt_free(&f_.img);
    free(ref); free(ref_f); free(b_d); free(f_d); free(fp_m); free(diffmap); free(cm); free(sheet); free(p); free(pb);
    vr_scene_set_fate(NULL, 0); free(fate);
    if (d) ns_dump_close(d);
    return ret;
}

#undef printf   /* main escribe la tabla directamente */
/* ------------------------------------------------------------------------------------------------ modo lote */
static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int has_file(const char *dir, const char *name) { char q[1100]; struct stat st; snprintf(q, sizeof q, "%s/%s", dir, name); return stat(q, &st) == 0; }
typedef struct { char **v; int n, cap; } PathList;
static void plist_add(PathList *L, const char *p) {
    if (L->n == L->cap) { L->cap = L->cap ? L->cap * 2 : 64; L->v = realloc(L->v, (size_t)L->cap * sizeof *L->v); }
    L->v[L->n++] = strdup(p);
}
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
/* volcados (carpetas con ram.bin) bajo `dir`, recursivo; no entra en un volcado ni en los `.tmp` de una escritura sin terminar */
static void scan_dumps(const char *dir, PathList *L) {
    if (has_file(dir, "ram.bin")) { plist_add(L, dir); return; }
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *e;
    while ((e = readdir(dp))) {
        size_t n = strlen(e->d_name);
        if (e->d_name[0] == '.' || (n > 4 && !strcmp(e->d_name + n - 4, ".tmp"))) continue;
        char q[1100]; snprintf(q, sizeof q, "%s/%s", dir, e->d_name);
        if (is_dir(q)) scan_dumps(q, L);
    }
    closedir(dp);
}

int main(int argc, char **argv) {
    const char *dir = NULL, *batch = NULL;
    DvOpts O = {0.064, 3.0, 0, 80, -1, -1, 0, 0, "dumpview.png"};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) O.outp = argv[++i];
        else if (!strcmp(argv[i], "--ipd") && i + 1 < argc) O.ipd = atof(argv[++i]);
        else if (!strcmp(argv[i], "--conv") && i + 1 < argc) O.conv = atof(argv[++i]);
        else if (!strcmp(argv[i], "--rects") && i + 1 < argc) O.maxrects = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--filter")) O.filter = 1;
        else if (!strcmp(argv[i], "--batch") && i + 1 < argc) batch = argv[++i];   /* una línea por volcado de la carpeta (recursivo) */
        else if (!strcmp(argv[i], "--sheets")) O.sheets = 1;   /* con --batch: además la lámina de cada volcado en <volcado>/dumpview.png */
        else if (!strcmp(argv[i], "--affine")) g_affine = 1;   /* diagnóstico: uv/color afines en pantalla como la PS1 (el shader de la app es perspectiva-correcto) */
        else if (!strcmp(argv[i], "--semi")) g_semi = 1;   /* diagnóstico: lista las primitivas semitransparentes (brillos, esferas) con su abr, textura, vista y destino */
        else if (!strcmp(argv[i], "--rect-hp") && i + 1 < argc) g_rect_hp_z = atof(argv[++i]);
        else if (!strcmp(argv[i], "--no-billboard")) g_nobb = 1;
        else if (!strcmp(argv[i], "--noz")) g_noz = 1;   /* diagnóstico (RenderRG.diagNoZ): sin prueba de profundidad, orden de pintor de la lista */
        else if (!strcmp(argv[i], "--probe") && i + 1 < argc) sscanf(argv[++i], "%d,%d", &O.probe_x, &O.probe_y);
        else if (argv[i][0] != '-') dir = argv[i];
    }
    if (batch) {
        PathList L = {0};
        scan_dumps(batch, &L);
        if (!L.n) { fprintf(stderr, "sin volcados (carpetas con ram.bin) en %s\n", batch); return 2; }
        qsort(L.v, (size_t)L.n, sizeof *L.v, cmp_str);
        O.batch = 1;
        size_t bl = strlen(batch); int errs = 0;
        printf("%-44s | %-24s | %-9s | %s\n", "volcado", "motivo", "dif ipd0", "tipos de primitiva bajo los píxeles distintos (% de ellos)");
        for (int i = 0; i < L.n; i++) {
            const char *nm = L.v[i] + (strncmp(L.v[i], batch, bl) == 0 ? bl : 0);
            while (*nm == '/') nm++;
            BatchRow r;
            g_quiet = 1;
            int rc = run_dump(L.v[i], &O, &r);
            g_quiet = 0;
            if (rc) { errs++; printf("%-44s | %-24s | ERROR (código %d, ver stderr)\n", *nm ? nm : L.v[i], r.motivo[0] ? r.motivo : "?", rc); }
            else {
                char tops[200] = ""; size_t tn = 0;
                for (int t = 0; t < r.ntop; t++) tn += (size_t)snprintf(tops + tn, sizeof tops - tn, "%s%s %.0f%%", t ? ", " : "", bk_name[r.top[t]], r.top_pct[t]);
                printf("%-44s | %-24s | %8.2f%% | %s\n", *nm ? nm : L.v[i], r.motivo, r.dif_f, r.ntop ? tops : "-");
            }
            fflush(stdout);
        }
        printf("%d volcados, %d con error\n", L.n, errs);
        for (int i = 0; i < L.n; i++) free(L.v[i]);
        free(L.v);
        return errs ? 1 : 0;
    }
    if (!dir) { fprintf(stderr, "uso: dumpview <carpeta del volcado> [-o lamina.png] [--ipd m] [--conv m] [--filter] [--noz] [--affine] [--rects N] [--probe x,y]\n       dumpview --batch <carpeta> [--sheets]\n"); return 2; }
    return run_dump(dir, &O, NULL);
}
