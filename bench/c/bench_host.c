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
#include "vr_smooth.h"
#include <math.h>
#include "ns_bridge.h"

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
static VrVtx g_vbase[MAXV];   /* geometría base (mundo en metros del usuario con el origen del personaje en O = 0, R = I): ver bench_publish_motion */
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
static void batch(uint32_t first, uint8_t kind, uint32_t ev_end) {
    if (g_nv == first || g_nb >= 4) return;
    VrBatch *b = &g_b[g_nb++];
    memset(b, 0, sizeof *b);
    b->first = first; b->count = g_nv - first; b->kind = kind; b->blend = 0xFF; b->ev_end = ev_end;
}

/* ---- VRAM sintética: textura RGB555 de 64x64 en (640,0) con degradado (r = x/2, g = y/2, b = 16) ---- */
static uint16_t g_vram_init[512 * 1024];
static GpuOp g_ops[2];
static uint16_t g_data[256];
static GpuDrawList g_list;

static void texvtx(float x, float y, float z, float u, float v) {
    vtx(x, y, z, rgba(128, 128, 128), 2 /* GPU_F_TEXTURED */);
    VrVtx *t = &g_v[g_nv - 1];
    t->uv[0] = u; t->uv[1] = v;
    t->tpage = (uint16_t)(10 | (0 << 4) | (0 << 5) | (2 << 7));   /* x = 640/64, y = 0, abr 0, 15 bits */
    t->clut = 0;
}

/* Mundo (metros, Y arriba, -Z delante): suelo ajedrezado 20x20 m en y=0 (gris claro/oscuro), cubo ROJO (-1.2, .5, -3), AZUL (1.5, .5, -3),
 * AMARILLO pequeño y cercano en (0, 1.6, -2) justo delante de un cubo VERDE grande y lejano (0, 1.6, -8) (oclusión: el verde se emite DESPUÉS
 * del amarillo, así que si la profundidad falla se vería verde en el centro). HUD 2D: rectángulo magenta 10..60 x 10..40 px en la esquina sup. izq. */
/* Cubo CIANO opcional delante del radar de muñeca (lo coloca el banco tras conocer el marco del radar): el radar lleva profundidad normal y debe quedar tapado. */
static int g_occ_on; static float g_occ[4];
void bench_set_radar_occluder(int on, float x, float y, float z, float size) { g_occ_on = on; g_occ[0] = x; g_occ[1] = y; g_occ[2] = z; g_occ[3] = size; }
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
    /* CIANO delante del panel del HUD: entre la cabeza de la pose B (0.2, 1.6, -0.3) y el rectángulo magenta del panel (-0.386, 1.953, -1.7); el texto/HUD siempre va encima */
    box(-0.093f, 1.776f, -1.0f, 0.12f, 0.12f, 0.12f, rgba(0, 255, 255));
    if (g_occ_on) box(g_occ[0], g_occ[1], g_occ[2], g_occ[3], g_occ[3], g_occ[3], rgba(0, 255, 255));
    batch(first, 0, 0);
    first = g_nv;
    /* cuadro texturizado 2x2 m en x -4..-2, y 1..3, z -5: lee la VRAM tras aplicar los eventos (relleno azul en texels 0..15 x 0..15 y subida verde en 16..31 x 0..15) */
    texvtx(-4, 3, -5, 0, 0);  texvtx(-2, 3, -5, 64, 0);  texvtx(-2, 1, -5, 64, 64);
    texvtx(-4, 3, -5, 0, 0);  texvtx(-2, 1, -5, 64, 64); texvtx(-4, 1, -5, 0, 64);
    batch(first, 0, 2);
    first = g_nv;
    rect2d(10, 10, 60, 40, rgba(255, 0, 255));
    batch(first, 2, 2);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) g_vram_init[(size_t)y * 1024 + 640 + x] = (uint16_t)((x / 2) | ((y / 2) << 5) | (16 << 10));
    memset(g_ops, 0, sizeof g_ops);
    g_ops[0].type = GPU_OP_FILL; g_ops[0].rx = 640; g_ops[0].ry = 0; g_ops[0].rw = 16; g_ops[0].rh = 16; g_ops[0].fill15 = 0x7C00;   /* azul */
    g_ops[1].type = GPU_OP_UPLOAD; g_ops[1].rx = 656; g_ops[1].ry = 0; g_ops[1].rw = 16; g_ops[1].rh = 16; g_ops[1].data_off = 0; g_ops[1].data_n = 256;
    for (int i = 0; i < 256; i++) g_data[i] = 0x03E0;                                                                                 /* verde */
    memset(&g_list, 0, sizeof g_list);
    g_list.index = 0; g_list.disp.w = 320; g_list.disp.h = 240; g_list.disp.enabled = 1;
    g_list.ops = g_ops; g_list.nops = 2; g_list.cap_ops = 2; g_list.data = g_data; g_list.ndata = 256; g_list.cap_data = 256;
    g_list.vram_init = g_vram_init;
    memcpy(g_vbase, g_v, g_nv * sizeof g_v[0]);
    g_scene_loaded = 1;
}
void bench_scene_post(void) { g_scene_pending = g_scene_loaded; }
/* Cámara del juego de la escena (VrScene.game_view: vista del motor en unidades -> mundo VR en metros): a 1,6 m de altura mirando a -Z, 0,001 m por unidad (como en el juego). */
static int g_cine_cam;
void bench_set_cine_camera(int on) { g_cine_cam = on; }

static VrSmooth g_sm;
static VrSmSample g_sample;
static int g_sample_pending, g_sm_ready;
static double g_clock, g_turn_req;
static void sm_ready(void) { if (!g_sm_ready) { vrsm_init(&g_sm, NULL); g_sm_ready = 1; } }

/* ---------- render nativo (banco de extremo a extremo): estado que en la app vive en vrhost.c ---------- */
static int g_rmode = 1, g_nready, g_marks;   /* el banco parte en modo nativo (los casos existentes comprueban que la lista de la GPU no se dibuja); el defecto de la app es gpu */
void mv_vrhost_set_render_mode(int mode) { g_rmode = mode <= 0 ? 0 : mode >= 2 ? 2 : 1; }
int mv_vrhost_render_mode(void) { return g_rmode; }
int mv_vrhost_render_only_native(void) { return g_rmode == 2; }
int mv_vrhost_render_env_locked(void) { return 0; }
int mv_vrhost_native_available(void) { return g_nready; }
void mv_vrhost_native_renderer_ready(int ready) { g_nready = ready != 0; }
struct NsBridge *mv_vrhost_native_bridge(void) { return NULL; }   /* el banco asigna NativeRenderer.bridge a mano (puente de la escena sintética) */
/* La lista de la GPU marca sus batches 3D como vrhost.c (mark_3d) y declara que admite el 3D nativo. */
void bench_set_native_marks(int on) { g_marks = on != 0; }
/* Escena de extremo a extremo: UN batch 3D (kind 0) con un cuadrilátero ROJO puro enorme a 1 m delante de la cabeza (0.3, 1.6, 0.5) mirando a -Z; sin eventos de VRAM, con la
 * VRAM de la escena nativa sintética (`vram`: 1024x512 palabras, NULL = la del banco). Si se dibujara, tapa todo el campo de visión de los dos ojos. */
void bench_make_native_e2e_scene(const uint16_t *vram) {
    bench_make_scene();
    g_nv = 0; g_nb = 0;
    const float x0 = 0.3f - 6.0f, x1 = 0.3f + 6.0f, y0 = 1.6f - 6.0f, y1 = 1.6f + 6.0f, z = -0.5f;
    float a[3] = {x0, y0, z}, b[3] = {x1, y0, z}, c[3] = {x1, y1, z}, d[3] = {x0, y1, z};
    quad3(a, b, c, d, rgba(255, 0, 0));
    batch(0, 0, 0);
    g_list.nops = 0; g_list.ndata = 0;
    if (vram) memcpy(g_vram_init, vram, sizeof g_vram_init);
    memcpy(g_vbase, g_v, g_nv * sizeof g_v[0]);
    g_scene_loaded = 1;
}

int mv_host_scene(VrScene *out) {
    if (!g_scene_pending) return 0;
    g_scene_pending = 0;
    if (g_sample_pending) { sm_ready(); vrsm_push(&g_sm, &g_sample, g_clock); g_sample_pending = 0; }
    memset(out, 0, sizeof *out);
    out->v = g_v; out->nv = g_nv; out->b = g_b; out->nb = g_nb;
    out->has_3d = 1;
    out->src = &g_list;
    out->game_view[0] = out->game_view[5] = out->game_view[10] = out->game_view[15] = 1.0f;
    if (g_cine_cam) {
        out->game_view[0] = 0.001f; out->game_view[5] = -0.001f; out->game_view[10] = -0.001f; out->game_view[13] = 1.6f;
        out->cine_active = 1; out->cine_shot = 1;
    }
    if (g_marks) {
        out->native_3d = 1;
        for (uint32_t i = 0; i < g_nb; i++) g_b[i].skip_if_native = g_b[i].kind == 0 ? VR_SKIP_WORLD : g_b[i].kind == 1 ? VR_SKIP_FX : 0;
    } else {
        for (uint32_t i = 0; i < g_nb; i++) g_b[i].skip_if_native = 0;
    }
    out->scale = 0.001f;
    out->world_valid = 1;
    out->filter.proj_h = 160; out->filter.cx_col = 160; out->filter.cy_row = 128;
    out->filter.cover_abr = -1;
    return 1;
}

/* ---------- ganchos sin contenido (radar, manos/arma, vistas especiales, estado) ---------- */
/* Radar sintético: ventana de 71x54 con fondo verde y un punto rojo central (coordenadas de VRAM). */
static VrVtx g_rv[12];
static VrBatch g_rb[1];
static int g_radar_on, g_weapon_on;
void bench_enable_radar(int on) { g_radar_on = on; }
void bench_enable_weapon(int on) { g_weapon_on = on; }
int mv_host_radar(VrRadarScene *out) {
    if (!g_radar_on) return 0;
    memset(out, 0, sizeof *out);
    uint32_t n = 0;
    uint16_t f = 0x1000;
    float r[2][4] = {{0, 0, 71, 54}, {31, 22, 40, 32}};
    uint32_t col[2] = {rgba(0, 160, 0), rgba(255, 0, 0)};
    for (int i = 0; i < 2; i++) {
        float x0 = r[i][0], y0 = r[i][1], x1 = r[i][2], y1 = r[i][3];
        float q[6][2] = {{x0,y0},{x1,y0},{x1,y1},{x0,y0},{x1,y1},{x0,y1}};
        for (int k = 0; k < 6; k++) {
            VrVtx *v = &g_rv[n++];
            memset(v, 0, sizeof *v);
            v->pos[0] = q[k][0]; v->pos[1] = q[k][1]; v->rgba = col[i]; v->flags = f; v->pad = 0xFFFF;
        }
    }
    memset(&g_rb[0], 0, sizeof g_rb[0]);
    g_rb[0].first = 0; g_rb[0].count = n; g_rb[0].kind = 2; g_rb[0].blend = 0xFF;
    out->present = 1; out->rect[2] = 71; out->rect[3] = 54;
    out->v = g_rv; out->nv = n; out->b = g_rb; out->nb = 1;
    return 1;
}

/* Arma sintética: caja naranja 0,08 x 0,10 x 0,30 m en (0.25, 1.2, -0.6) del espacio recentrado. */
static VrHandsVertex g_wv[24];
static uint16_t g_wi[36];
static VrHandsMesh g_wm;
int mv_host_hands(VrHandsFrame *out) {
    if (!g_weapon_on) return 0;
    static const float N[6][3] = {{0,0,-1},{0,0,1},{-1,0,0},{1,0,0},{0,1,0},{0,-1,0}};
    float hx = 0.04f, hy = 0.05f, hz = 0.15f;
    for (int f = 0; f < 6; f++) {
        for (int k = 0; k < 4; k++) {
            VrHandsVertex *v = &g_wv[f * 4 + k];
            float a = (k == 0 || k == 3) ? -1.f : 1.f, b = (k < 2) ? -1.f : 1.f;
            float p[3];
            if (f < 2) { p[0] = a * hx; p[1] = b * hy; p[2] = (f ? 1 : -1) * hz; }
            else if (f < 4) { p[0] = (f == 3 ? 1 : -1) * hx; p[1] = a * hy; p[2] = b * hz; }
            else { p[0] = a * hx; p[1] = (f == 4 ? 1 : -1) * hy; p[2] = b * hz; }
            memcpy(v->pos, p, sizeof p); memcpy(v->nrm, N[f], sizeof v->nrm);
            v->uv[0] = v->uv[1] = 0;
            v->rgba[0] = 255; v->rgba[1] = 128; v->rgba[2] = 0; v->rgba[3] = 255;
        }
        uint16_t *ix = &g_wi[f * 6];
        ix[0] = f * 4; ix[1] = f * 4 + 1; ix[2] = f * 4 + 2; ix[3] = f * 4; ix[4] = f * 4 + 2; ix[5] = f * 4 + 3;
    }
    memset(&g_wm, 0, sizeof g_wm);
    g_wm.mesh_id = 1; g_wm.tex_id = 0;
    for (int i = 0; i < 16; i++) g_wm.model[i] = (i % 5 == 0) ? 1.f : 0.f;
    g_wm.model[12] = 0.25f; g_wm.model[13] = 1.2f; g_wm.model[14] = -0.6f;
    g_wm.verts = g_wv; g_wm.nverts = 24; g_wm.idx = g_wi; g_wm.nidx = 36;
    memset(out, 0, sizeof *out);
    out->seq = 1; out->nmeshes = 1; out->meshes = &g_wm;
    out->light_dir[1] = 1.f; out->ambient = 0.6f; out->diffuse = 0.4f;
    return 1;
}
void mv_host_hands_done(void) {}
int32_t mv_vrgame_special_view(MvVrSpecialView *out) { (void)out; return 0; }
int32_t mv_vrgame_special_sync(MvVrSpecialView *io) { (void)io; return 0; }   /* sin vista pegada al arma: nada que reajustar */
void mv_vrgame_status(MvVrGameStatus *out) { if (out) memset(out, 0, sizeof *out); if (out) out->weapon = -1; }   /* sin arma VR mostrada (active = 0) */
int32_t mv_vrgame_forearm(int32_t side, MvVrForearm *out) { (void)side; (void)out; return 0; }
int32_t mv_vrgame_hand_matrix(int32_t side, float m[16]) { (void)side; (void)m; return 0; }
int32_t mv_vrgame_menu_hand(void) { return -1; }   /* mano que mantiene abierta la ventana de armas/objetos: el banco no tiene menú del pecho */
/* Calibración de manos, IA de enemigos y manos nuevas: el menú del visor (XRPresent.swift) los llama; el banco no los usa. */
void mv_vrgame_calibrate_hands(float delay_s) { (void)delay_s; }
float mv_vrgame_calibration_countdown(void) { return 0.0f; }
void mv_vrgame_clear_hand_calibration(void) {}
int32_t mv_vrgame_hand_calibration(int32_t side, float q[4]) { (void)side; (void)q; return 0; }
int32_t mv_vrgame_arm_length(int32_t side, float *len_m) { (void)side; (void)len_m; return 0; }
void mv_vrhost_set_ai_range(float scale) { (void)scale; }
void mv_vrhost_set_ai_head(int on) { (void)on; }
void mv_vrhost_set_hands_new(int on) { (void)on; }
int mv_vrhost_hands_new(void) { return 1; }
int mv_vrhost_hands_active(void) { return 1; }
void mv_host_note_render(int kind) { (void)kind; }

/* ---------- fluidez sintética (vr_smooth): el mundo avanza/gira con el personaje y el juego publica escenas a su ritmo ----------
 * Escena i: P_i(W) = R_i * (b - K * O_i)  (c = 0), con b = geometría base, O_i = origen del personaje (unidades del juego, K = 0,001 m/unidad) y R_i = giro del stick que traía
 * la cámara de esa escena. El reloj y el giro pedido los fija el banco por fotograma; mv_vrhost_smooth evalúa la corrección como el host real. */
void bench_smooth_config(int enabled, float lead_s) {
    VrSmoothCfg c;
    vrsm_cfg_default(&c);
    c.enabled = enabled != 0;
    if (lead_s >= 0) c.lead_s = lead_s;
    vrsm_init(&g_sm, &c);
    g_sm_ready = 1; g_sample_pending = 0; g_clock = 0; g_turn_req = 0;
}
void bench_smooth_set_time(double t, double turn_req_deg) { g_clock = t; g_turn_req = turn_req_deg; }
static void rot_y(double a_deg, float R[9]) {
    double a = a_deg * 0.017453292519943295, cs = cos(a), sn = sin(a);
    float r[9] = {(float)cs, 0, (float)-sn, 0, 1, 0, (float)sn, 0, (float)cs};
    memcpy(R, r, sizeof r);
}
void bench_publish_geom(double ox_units, double oz_units, double turn_deg) {
    float R[9];
    rot_y(turn_deg, R);
    const float K = 0.001f;
    for (uint32_t i = 0; i < g_nv; i++) {
        if (g_vbase[i].flags & 0x1000) { g_v[i] = g_vbase[i]; continue; }   /* 2D (HUD): no se mueve */
        double b[3] = {g_vbase[i].pos[0] - K * ox_units, g_vbase[i].pos[1], g_vbase[i].pos[2] - K * oz_units};
        g_v[i] = g_vbase[i];
        for (int r = 0; r < 3; r++) g_v[i].pos[r] = (float)(R[3 * r] * b[0] + R[3 * r + 1] * b[1] + R[3 * r + 2] * b[2]);
    }
    g_scene_pending = g_scene_loaded;
}
void bench_publish_motion(double t_wall, double game_s, double ox_units, double oz_units, double turn_deg) {
    float R[9];
    rot_y(turn_deg, R);
    bench_publish_geom(ox_units, oz_units, turn_deg);
    memset(&g_sample, 0, sizeof g_sample);
    g_sample.valid = 1; g_sample.t_wall = t_wall; g_sample.game_s = game_s;
    g_sample.B[0] = (float)ox_units; g_sample.B[2] = (float)oz_units;
    memcpy(g_sample.R, R, sizeof R);
    g_sample.K = 0.001f; g_sample.turn_deg = turn_deg; g_sample.epoch = 1;
    g_sample_pending = 1;
}
void mv_vrhost_smooth(VrSmCorr *out) { sm_ready(); vrsm_eval(&g_sm, g_clock, g_turn_req, out); }
void mv_vrhost_smooth_stats(VrSmoothStats *s) { sm_ready(); vrsm_stats(&g_sm, s); }
void mv_vrhost_set_smooth(int on) { sm_ready(); g_sm.cfg.enabled = on != 0; }

/* Capa 2D fiel (vr_layer2d): el banco no la usa; el menú del visor solo necesita los símbolos. */
void mv_vrhost_set_hud2d_smooth(int mode) { (void)mode; }
int mv_vrhost_hud2d_smooth(void) { return 1; }
void mv_vrhost_hud2d_stats(int *scale, double *ms) { if (scale) *scale = 0; if (ms) *ms = 0; }
int mv_vrhost_hud2d_acquire(const uint8_t **px, int *w, int *h, int *full, uint32_t *seq) { (void)px; (void)w; (void)h; (void)full; (void)seq; return -1; }
void mv_vrhost_hud2d_release(void) {}
