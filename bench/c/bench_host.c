/* Banco de render (CI, macOS): sustituye al host del runtime con datos SINTÉTICOS generados aquí. Ningún dato del juego.
 * Implementa lo que consume el renderer Swift: fotograma de la pantalla de cine, escena VR (suelo, cubos, sprite 2D del HUD),
 * y los ganchos de radar / manos / vistas especiales (vacíos: ver bench_set_* para activarlos). */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "host.h"
#include "vrhost.h"
#include "vrgame.h"
#include "vr_hands_frame.h"
#include "vr_scene.h"

/* ---------- fotograma 2D (pantalla de cine): patrón de prueba 320x240 ---------- */
static uint8_t g_fb[320 * 240 * 4];
static int g_fb_seq = 0;

static void px(int x, int y, int r, int g, int b) {
    if (x < 0 || y < 0 || x >= 320 || y >= 240) return;
    uint8_t *p = g_fb + ((size_t)y * 320 + x) * 4;
    p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)b; p[3] = 255;
}
static void rect(int x0, int y0, int x1, int y1, int r, int g, int b) {
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) px(x, y, r, g, b);
}

/* Esquinas: rojo (arriba izq.), verde (arriba der.), azul (abajo izq.), amarillo (abajo der.); cruz blanca central; rejilla gris cada 20 px;
 * una "F" asimétrica en el centro-izquierda (detecta espejos y giros). */
void bench_make_pattern(void) {
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++) {
            int grid = (x % 20 == 0) || (y % 20 == 0);
            int v = grid ? 90 : 28;
            px(x, y, v, v, v);
        }
    rect(0, 0, 40, 40, 255, 0, 0);
    rect(280, 0, 320, 40, 0, 255, 0);
    rect(0, 200, 40, 240, 0, 0, 255);
    rect(280, 200, 320, 240, 255, 255, 0);
    rect(156, 20, 164, 220, 255, 255, 255);
    rect(20, 116, 300, 124, 255, 255, 255);
    /* F cian en x 60..100, y 80..160 */
    rect(60, 80, 68, 160, 0, 255, 255);
    rect(60, 80, 100, 88, 0, 255, 255);
    rect(60, 116, 90, 124, 0, 255, 255);
    g_fb_seq = 1;
}

uint32_t mv_host_framebuffer(uint8_t *dst, size_t dst_size, int *w, int *h, uint32_t last_seq) {
    if (g_fb_seq == 0 || (uint32_t)g_fb_seq == last_seq || dst_size < sizeof g_fb) return 0;
    memcpy(dst, g_fb, sizeof g_fb);
    if (w) *w = 320;
    if (h) *h = 240;
    return (uint32_t)g_fb_seq;
}
/* el banco puede pedir "fotograma nuevo" (mismo contenido) para probar el anillo de texturas */
void bench_touch_frame(void) { if (g_fb_seq) g_fb_seq++; }

/* ---------- escena VR sintética ---------- */
#define MAXV 20000
static VrVtx g_v[MAXV];
static uint32_t g_nv;
static VrBatch g_b[4];
static uint32_t g_nb;
static int g_scene_pending;
static int g_scene_loaded;

static uint32_t rgba(int r, int g, int b) { return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | (255u << 24); }

static void vtx(float x, float y, float z, uint32_t c, uint16_t flags) {
    if (g_nv >= MAXV) return;
    VrVtx *v = &g_v[g_nv++];
    memset(v, 0, sizeof *v);
    v->pos[0] = x; v->pos[1] = y; v->pos[2] = z;
    v->rgba = c; v->flags = flags; v->pad = 0xFFFF;
}
static void tri3(const float *a, const float *b, const float *c, uint32_t col) {
    vtx(a[0], a[1], a[2], col, 0); vtx(b[0], b[1], b[2], col, 0); vtx(c[0], c[1], c[2], col, 0);
}
static void quad3(const float *a, const float *b, const float *c, const float *d, uint32_t col) {
    tri3(a, b, c, col); tri3(a, c, d, col);
}
static void box(float cx, float cy, float cz, float sx, float sy, float sz, uint32_t col) {
    float x0 = cx - sx / 2, x1 = cx + sx / 2, y0 = cy - sy / 2, y1 = cy + sy / 2, z0 = cz - sz / 2, z1 = cz + sz / 2;
    float p[8][3] = {{x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},{x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1}};
    static const int f[6][4] = {{0,1,2,3},{5,4,7,6},{4,0,3,7},{1,5,6,2},{3,2,6,7},{4,5,1,0}};
    for (int i = 0; i < 6; i++) quad3(p[f[i][0]], p[f[i][1]], p[f[i][2]], p[f[i][3]], col);
}
static void rect2d(float x0, float y0, float x1, float y1, uint32_t col) {
    uint16_t f = 0x1000;   /* VR_F_2D */
    vtx(x0, y0, 0, col, f); vtx(x1, y0, 0, col, f); vtx(x1, y1, 0, col, f);
    vtx(x0, y0, 0, col, f); vtx(x1, y1, 0, col, f); vtx(x0, y1, 0, col, f);
}
static void batch(uint32_t first, uint8_t kind) {
    if (g_nv == first || g_nb >= 4) return;
    VrBatch *b = &g_b[g_nb++];
    memset(b, 0, sizeof *b);
    b->first = first; b->count = g_nv - first; b->kind = kind; b->blend = 0xFF;
}

/* Mundo (metros, Y arriba, -Z delante): suelo ajedrezado 20x20 m en y=0 (gris claro/oscuro), cubo ROJO (-1.2, .5, -3), AZUL (1.5, .5, -3),
 * AMARILLO pequeño y cercano en (0, 1.6, -2) justo delante de un cubo VERDE grande y lejano (0, 1.6, -8) (oclusión: el verde se emite DESPUÉS
 * del amarillo, así que si la profundidad falla se vería verde en el centro). HUD 2D: rectángulo magenta 10..60 x 10..40 px en la esquina sup. izq. */
void bench_make_scene(void) {
    g_nv = 0; g_nb = 0;
    uint32_t first = 0;
    for (int i = -10; i < 10; i++)
        for (int j = -10; j < 10; j++) {
            uint32_t c = ((i + j) & 1) ? rgba(150, 150, 150) : rgba(70, 70, 70);
            float a[3] = {(float)i, 0, (float)j}, b[3] = {(float)i + 1, 0, (float)j}, cc[3] = {(float)i + 1, 0, (float)j + 1}, d[3] = {(float)i, 0, (float)j + 1};
            quad3(a, b, cc, d, c);
        }
    box(-1.2f, 0.5f, -3.0f, 1.0f, 1.0f, 1.0f, rgba(255, 0, 0));
    box(1.5f, 0.5f, -3.0f, 1.0f, 1.0f, 1.0f, rgba(0, 0, 255));
    box(0.0f, 1.6f, -2.0f, 0.5f, 0.5f, 0.5f, rgba(255, 255, 0));
    box(0.0f, 1.6f, -8.0f, 3.0f, 3.0f, 1.0f, rgba(0, 255, 0));
    batch(first, 0);
    first = g_nv;
    rect2d(10, 10, 60, 40, rgba(255, 0, 255));
    batch(first, 2);
    g_scene_loaded = 1;
}
void bench_scene_post(void) { g_scene_pending = g_scene_loaded; }

int mv_host_scene(VrScene *out) {
    if (!g_scene_pending) return 0;
    g_scene_pending = 0;
    memset(out, 0, sizeof *out);
    out->v = g_v; out->nv = g_nv; out->b = g_b; out->nb = g_nb;
    out->has_3d = 1;
    out->game_view[0] = out->game_view[5] = out->game_view[10] = out->game_view[15] = 1.0f;
    out->scale = 0.001f;
    out->world_valid = 1;
    out->filter.proj_h = 160; out->filter.cx_col = 160; out->filter.cy_row = 128;
    out->filter.cover_abr = -1;
    return 1;
}

/* ---------- ganchos sin contenido (radar, manos/arma, vistas especiales, estado) ---------- */
int mv_host_radar(VrRadarScene *out) { (void)out; return 0; }
int mv_host_hands(VrHandsFrame *out) { (void)out; return 0; }
void mv_host_hands_done(void) {}
int32_t mv_vrgame_special_view(MvVrSpecialView *out) { (void)out; return 0; }
int32_t mv_vrgame_forearm(int32_t side, MvVrForearm *out) { (void)side; (void)out; return 0; }
int32_t mv_vrgame_hand_matrix(int32_t side, float m[16]) { (void)side; (void)m; return 0; }
void mv_host_note_render(int kind) { (void)kind; }
