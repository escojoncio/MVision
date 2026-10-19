/* Pruebas de vr_cameras (parte 2): tabla, daño, rayo, sonido, barrido. Datos sintéticos; con `build/ov` (overlays
 * extraídas, locales) además comprueba las firmas contra el código real. Uso: test_cameras [dir_overlays]. */
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_ai_scan.h"
#include "vr_cameras.h"

static int nfail, nok;
#define CHECK(c, ...) do { if (c) nok++; else { nfail++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t RAM[VAI_RAM_SIZE];
#define LISTS 0x800ADDA8u
#define TARGETS 0x800B7660u
#define PMAP 0x800ACB9Cu
#define STATUS 0x800AC570u

/* ---- construcción de RAM sintética ---- */
static uint32_t ACT_A = 0x800D0000u, ACT_B = 0x800D0400u, ACT_NOISE = 0x800D0800u;
static const uint32_t NW_A = 40, NW_B = 70;

static void put_func(uint32_t a, uint32_t nwords, uint32_t seed) {
    vai_w32(RAM, a, 0x27BDFFE0u);   /* addiu sp,sp,-32 */
    for (uint32_t i = 1; i < nwords - 1; i++) vai_w32(RAM, a + 4 * i, 0x24020000u + ((seed * 2654435761u + i * 40503u) & 0x7FFF) + (i & 7) * 0x04000000u * 0);
    vai_w32(RAM, a + 4 * (nwords - 1), 0x03E00008u);
    vai_w32(RAM, a + 4 * nwords, 0x27BDFFE8u);     /* siguiente prólogo: delimita la función */
}

static void lists_reset(void) {
    for (int lv = 0; lv < 9; lv++) {
        uint32_t b = LISTS + 68u * lv, e = b + 32;
        vai_w32(RAM, b, 0); vai_w32(RAM, b + 4, e); vai_w32(RAM, b + 8, 0);
        vai_w32(RAM, e, b); vai_w32(RAM, e + 4, 0); vai_w32(RAM, e + 8, 0);
    }
}
static uint32_t list_tail[9];
static void list_add(int lv, uint32_t node, uint32_t act) {
    uint32_t b = LISTS + 68u * lv, e = b + 32;
    uint32_t prev = vai_r32(RAM, e);
    vai_w32(RAM, node, prev); vai_w32(RAM, node + 4, e); vai_w32(RAM, node + 8, act);
    vai_w32(RAM, prev + 4, node); vai_w32(RAM, e, node);
    (void)list_tail;
}
static void tgt_set(int i, uint16_t cls, int16_t cx, int16_t cy, int16_t cz, int16_t hx, int16_t hy, int16_t hz) {
    uint32_t t = TARGETS + 72u * i;
    vai_w16(RAM, t, (int16_t)cls); vai_w16(RAM, t + 2, 2); vai_w16(RAM, t + 6, 0);
    vai_w16(RAM, t + 8, cx); vai_w16(RAM, t + 10, cy); vai_w16(RAM, t + 12, cz);
    vai_w16(RAM, t + 0x10, hx); vai_w16(RAM, t + 0x12, hy); vai_w16(RAM, t + 0x14, hz);
    vai_w16(RAM, t + 0x28, 0); vai_w16(RAM, t + 0x3e, 0);
}
/* actor familia CAMERA: offsets del bloque de trabajo (ver vr_cameras.c) */
static void cam_make(uint32_t a, uint32_t act, int ti, int16_t x, int16_t y, int16_t z, int16_t yaw, int16_t pitch,
                     uint16_t range, uint16_t half, uint16_t turnmax, int active, int mode, uint32_t mapidx) {
    (void)act;
    uint32_t mapst = 0x80180000u + (a & 0xFFFCu);   /* una estructura MAP por actor */
    vai_w32(RAM, mapst, mapidx);
    vai_w16(RAM, a + 0x20, x); vai_w16(RAM, a + 0x22, y); vai_w16(RAM, a + 0x24, z);
    vai_w16(RAM, a + 0x28, pitch); vai_w16(RAM, a + 0x2a, yaw);
    vai_w32(RAM, a + 0x4c, mapst);
    vai_w32(RAM, a + 0x1CC, TARGETS + 72u * ti);
    vai_w32(RAM, a + 0x1F0, 0);
    vai_w16(RAM, a + 0x284, (int16_t)active); vai_w16(RAM, a + 0x28A, 1);
    vai_w32(RAM, a + 0x1E0, (uint32_t)mode); vai_w32(RAM, a + 0x28C, 0xFFFFFFFFu);
    vai_w16(RAM, a + 0x27E, (int16_t)range); vai_w16(RAM, a + 0x280, (int16_t)half); vai_w16(RAM, a + 0x282, (int16_t)turnmax);
    vai_w16(RAM, a + 0x1C0, 0); vai_w16(RAM, a + 0x1C2, yaw);
    tgt_set(ti, 0x15, x, y, z, 300, 300, 300);
}

static void world_init(void) {
    vrc_reset_all();
    memset(RAM, 0, sizeof RAM);
    lists_reset();
    put_func(ACT_A, NW_A, 1);
    put_func(ACT_B, NW_B, 2);
    put_func(ACT_NOISE, 50, 3);
    vrc_add_signature(VRC_FAM_CAMERA, NW_A, vai_shape_hash(RAM, ACT_A, ACT_A + 4 * NW_A));
    vrc_add_signature(VRC_FAM_GUN, NW_B, vai_shape_hash(RAM, ACT_B, ACT_B + 4 * NW_B));
    vai_w32(RAM, PMAP, 0x3);
}

/* ---- pruebas ---- */
static void t_curve(void) {
    VrcSoundCfg c; vrc_sound_default(&c);
    VrcSoundCfg o = c; o.mode = VRC_SND_ORIGINAL;
    for (int d = 0; d <= 12000; d += 37) {
        int want = d < 9500 ? 63 - d / 150 : 0;
        CHECK(vrc_sound_curve(&o, (float)d, NULL) == want, "original d=%d", d);
    }
    float g, prev = 2.f;
    for (float d = 0; d < 40000; d += 53) {
        int v = vrc_sound_curve(&c, d, &g);
        CHECK(g <= prev + 1e-6f && g >= 0.f && g <= 1.f, "monótona d=%.0f g=%f", d, g);
        CHECK(v >= 0 && v <= 63, "rango vol");
        if (d < c.max_dist && g > 0) CHECK(v >= 1, "audible => vol>=1 d=%.0f", d);
        if (d >= c.max_dist) CHECK(v == 0 && g == 0.f, "silencio tras max");
        prev = g;
    }
    CHECK(vrc_sound_curve(&c, 0, &g) == 63 && g == 1.f, "pleno en 0");
    CHECK(vrc_sound_curve(&c, c.ref_dist, &g) == 63, "pleno en ref");
    /* natural llega más lejos que el original, con la misma cercanía */
    CHECK(vrc_sound_curve(&c, 9400, NULL) >= vrc_sound_curve(&o, 9400, NULL), "nunca más bajo que el original");
    for (float d = 0; d < 9500; d += 41) CHECK(vrc_sound_curve(&c, d, NULL) >= vrc_sound_curve(&o, d, NULL), "natural >= original d=%.0f", d);
    CHECK(vrc_sound_curve(&c, 20000, NULL) > 0 && vrc_sound_curve(&o, 20000, NULL) == 0, "a 20000 solo natural");
    /* ley inversa: al doblar la distancia (fuera del fundido) la ganancia se divide por ~2 */
    float g1, g2; vrc_sound_curve(&c, 10000, &g1); vrc_sound_curve(&c, 20000, &g2);
    CHECK(fabsf(g1 / g2 - 2.f) < 0.01f, "inversa: %f %f", g1, g2);
    CHECK(vrc_sound_curve(&c, NAN, &g) == 63, "NaN -> 0");
    CHECK(vrc_sound_curve(&c, -5.f, &g) == 63, "negativa");
    CHECK(vrc_sound_curve(&c, INFINITY, &g) == 0, "inf");
    CHECK(vrc_sound_curve(&o, INFINITY, &g) == 0 && vrc_sound_curve(&o, 3e9f, &g) == 0 && vrc_sound_curve(&o, NAN, &g) == 63, "original: inf/enorme/NaN sin UB");
    /* sin fundido y con corte */
    c.fade_frac = 0; c.max_dist = 10000; vrc_sound_set(&c);
    VrcSoundCfg q; vrc_sound_get(&q);
    CHECK(vrc_sound_curve(&q, 9999, NULL) >= 1 && vrc_sound_curve(&q, 10000, NULL) == 0, "corte seco");
    /* saneado de configuración */
    VrcSoundCfg bad = c; bad.ref_dist = NAN; bad.max_dist = -1; bad.rolloff = -2; bad.fade_frac = 7; bad.gain = NAN; bad.min_vol = 99; bad.n_se = 77;
    vrc_sound_set(&bad); vrc_sound_get(&q);
    CHECK(q.ref_dist >= 1 && q.max_dist > q.ref_dist && q.rolloff >= 0 && q.fade_frac <= 1 && q.gain >= 0 && q.min_vol <= 63 && q.n_se <= 8, "saneado");
    CHECK(vrc_sound_curve(&q, 100, NULL) >= 0, "curva tras saneado");
}

static void t_scan(void) {
    world_init();
    uint32_t A = 0x80120000u, B = 0x80121000u, N = 0x80122000u;
    cam_make(A, ACT_A, 0, 1000, 2000, 3000, 1024, 100, 8000, 256, 200, 1, 0, 1);
    cam_make(B, ACT_A, 1, -500, 1500, 200, 3000, 0, 5000, 128, 0, 0, 1, 4);   /* apagada, otro mapa */
    list_add(4, A, ACT_A); list_add(4, B, ACT_A);
    list_add(5, N, ACT_NOISE);                                                /* actor que no es cámara */
    VrcTable t; memset(&t, 0, sizeof t);
    CHECK(vrc_scan(RAM, &t) == 2, "2 cámaras (n=%d)", t.n);
    const VrcCamera *a = vrc_find(&t, A), *b = vrc_find(&t, B);
    CHECK(a && b && !vrc_find(&t, N) && !vrc_find(&t, 0x1234), "búsqueda");
    if (a && b) {
        CHECK(a->family == VRC_FAM_CAMERA && a->pos[0] == 1000 && a->pos[1] == 2000 && a->pos[2] == 3000, "posición");
        CHECK(a->rot[1] == 1024 && a->rot[0] == 100 && a->range == 8000 && a->half == 256, "orientación y cono");
        CHECK(a->status == VRC_ST_PATROL && a->alive && a->active && a->in_player_map && a->map_bit == 1, "estado A");
        CHECK(a->target == TARGETS && a->box_half[0] == 300 && a->box_center[2] == 3000, "caja de impacto");
        CHECK(a->proc == -1 && a->sound_on, "script/sonido");
        CHECK(b->status == VRC_ST_INACTIVE && !b->active && !b->in_player_map && b->map_bit == 4, "estado B (apagada, otro mapa)");
    }
    /* estados por modo */
    vai_w32(RAM, A + 0x1E0, 1); vrc_scan(RAM, &t); CHECK(vrc_find(&t, A)->status == VRC_ST_ALERT, "alerta");
    vai_w32(RAM, A + 0x1E0, 2); vrc_scan(RAM, &t); CHECK(vrc_find(&t, A)->status == VRC_ST_SEARCH, "búsqueda");
    vai_w32(RAM, A + 0x1E0, 3); vrc_scan(RAM, &t); CHECK(vrc_find(&t, A)->status == VRC_ST_JAMMED, "interferida");
    vai_w32(RAM, A + 0x1E0, 9); vrc_scan(RAM, &t); CHECK(vrc_find(&t, A)->status == VRC_ST_PATROL, "modo fuera de rango -> patrulla");
    vai_w32(RAM, A + 0x1F0, 5); vrc_scan(RAM, &t); CHECK(vrc_find(&t, A)->status == VRC_ST_DYING && !vrc_find(&t, A)->alive, "muriendo");
    /* puntero de blanco inválido */
    vai_w32(RAM, A + 0x1CC, 0x80300000u); vai_w32(RAM, A + 0x1F0, 0); vrc_scan(RAM, &t);
    CHECK(vrc_find(&t, A) && vrc_find(&t, A)->target == 0, "blanco inválido -> 0");
    vai_w32(RAM, A + 0x1CC, TARGETS + 5); vrc_scan(RAM, &t);
    CHECK(vrc_find(&t, A)->target == 0, "blanco desalineado -> 0");
    /* tabla llena */
    world_init();
    for (int i = 0; i < 40; i++) { uint32_t n = 0x80130000u + 0x400u * i; cam_make(n, ACT_A, i % 60, (int16_t)i, 0, 0, 0, 0, 100, 10, 0, 1, 0, 1); list_add(3, n, ACT_A); }
    CHECK(vrc_scan(RAM, &t) == VRC_MAX_CAMERAS, "tope de tabla");
    /* lista corrupta: bucle y prev incoherente */
    world_init();
    cam_make(A, ACT_A, 0, 1, 2, 3, 0, 0, 100, 10, 0, 1, 0, 1); list_add(2, A, ACT_A);
    vai_w32(RAM, A + 4, A);                                   /* se apunta a sí mismo */
    CHECK(vrc_scan(RAM, &t) <= 1, "bucle no cuelga");
    world_init();
    cam_make(A, ACT_A, 0, 1, 2, 3, 0, 0, 100, 10, 0, 1, 0, 1); list_add(2, A, ACT_A);
    vai_w32(RAM, A, 0x80140000u);                             /* prev incoherente */
    CHECK(vrc_scan(RAM, &t) == 0, "prev incoherente se descarta");
    CHECK(vrc_scan(NULL, &t) == 0 && vrc_scan(RAM, NULL) == 0, "NULL");
}

static void t_cone(void) {
    world_init();
    uint32_t A = 0x80120000u;
    cam_make(A, ACT_A, 0, 0, 1000, 0, 0, 200, 4000, 300, 0, 1, 0, 1);   /* mira a +Z, algo hacia abajo */
    list_add(4, A, ACT_A);
    VrcTable t; vrc_scan(RAM, &t);
    const VrcCamera *c = vrc_find(&t, A);
    float f[3]; vrc_forward(c, f);
    CHECK(fabsf(f[0]) < 1e-5f && f[2] > 0.9f && f[1] < 0.f, "dirección: +Z y hacia abajo (%f %f %f)", f[0], f[1], f[2]);
    float len = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]); CHECK(fabsf(len - 1.f) < 1e-5f, "unitario");
    /* dirección del cabeceo 200 (17,6°): punto en esa dirección a 2000 */
    int16_t p[3] = {0, (int16_t)(1000 + lrintf(f[1] * 2000)), (int16_t)lrintf(f[2] * 2000)};
    CHECK(vrc_in_cone(c, p), "dentro (eje)");
    int16_t far_[3] = {0, 1000, 5000}; CHECK(!vrc_in_cone(c, far_), "fuera de alcance");
    int16_t side[3] = {3000, 1000, 100}; CHECK(!vrc_in_cone(c, side), "fuera de ángulo");
    int16_t back[3] = {0, 1000, -2000}; CHECK(!vrc_in_cone(c, back), "detrás");
    int16_t edge[3] = {(int16_t)(1000 * tanf(250.f * 6.2831853f / 4096.f)), (int16_t)(1000 + lrintf(f[1] * 1000)), 1000};
    CHECK(vrc_in_cone(c, edge), "cerca del borde dentro");
    CHECK(!vrc_in_cone(NULL, p) && !vrc_in_cone(c, NULL), "NULL");
}

static void t_hit(void) {
    world_init();
    uint32_t A = 0x80120000u, N = 0x80122000u, D = 0x80123000u;
    cam_make(A, ACT_A, 0, 100, 200, 300, 0, 0, 4000, 256, 0, 1, 0, 1);
    cam_make(D, ACT_A, 1, 100, 200, 300, 0, 0, 4000, 256, 0, 1, 0, 1);
    vai_w32(RAM, D + 0x1F0, 7);
    list_add(4, A, ACT_A); list_add(4, D, ACT_A); list_add(5, N, ACT_NOISE);
    int16_t pt[3] = {100, 200, 300};
    CHECK(vr_cameras_hit(RAM, N, pt, 64) == VRC_HIT_BAD, "no es cámara");
    CHECK(vr_cameras_hit(RAM, 0x1234, pt, 64) == VRC_HIT_BAD, "puntero basura");
    CHECK(vr_cameras_hit(NULL, A, pt, 64) == VRC_HIT_BAD && vr_cameras_hit(RAM, A, pt, 0) == VRC_HIT_BAD && vr_cameras_hit(RAM, A, pt, -4) == VRC_HIT_BAD, "args");
    CHECK(vr_cameras_hit(RAM, D, pt, 64) == VRC_HIT_IGNORED, "muriendo: ignorada");
    /* política VR: 2 balas (bullet_hits = 2 por defecto) */
    CHECK(vr_cameras_hit(RAM, A, pt, 64) == VRC_HIT_DAMAGED, "1ª bala: dañada");
    CHECK(vai_r16(RAM, TARGETS + 6) == 0 && vai_r16(RAM, TARGETS + 0x3e) == 0, "daño parcial no toca el blanco");
    CHECK(vr_cameras_hit(RAM, A, pt, 64) == VRC_HIT_DESTROYED, "2ª bala: destruida");
    CHECK((vai_r16(RAM, TARGETS + 6) & 4) && vai_r16(RAM, TARGETS + 0x3e) == 2 && vai_r16(RAM, TARGETS + 0x28) == 64, "blanco marcado como el juego: bit 4, modo 2, daño");
    /* explosivo: inmediato y sin tocar el resto del blanco */
    world_init();
    cam_make(A, ACT_A, 0, 100, 200, 300, 0, 0, 4000, 256, 0, 1, 0, 1); list_add(4, A, ACT_A);
    vai_w16(RAM, TARGETS + 0x24, 0x1234); vai_w16(RAM, TARGETS + 0x26, 77);
    CHECK(vr_cameras_hit_ex(RAM, A, pt, 1, VRC_DMG_EXPLOSIVE) == VRC_HIT_DESTROYED, "explosivo destruye");
    CHECK(vai_r16(RAM, TARGETS + 0x24) == 0x1234 && vai_r16(RAM, TARGETS + 0x26) == 77 && vai_r16(RAM, TARGETS) == 0x15, "no toca otros campos");
    /* política original: balas ignoradas */
    VrcConfig cfg; vrc_config_get(&cfg); cfg.bullets_destroy = 0; vrc_config_set(&cfg);
    CHECK(vr_cameras_hit(RAM, A, pt, 9999) == VRC_HIT_IGNORED, "original: bala ignorada");
    cfg.bullets_destroy = 1; cfg.bullet_hits = 1; vrc_config_set(&cfg);
    CHECK(vr_cameras_hit(RAM, A, pt, 1) == VRC_HIT_DESTROYED, "bullet_hits 1: de un golpe");
    cfg.bullet_hits = 3; vrc_config_set(&cfg);
    vrc_reset();
    CHECK(vr_cameras_hit(RAM, A, pt, 1) == VRC_HIT_DAMAGED && vr_cameras_hit(RAM, A, pt, 1) == VRC_HIT_DAMAGED && vr_cameras_hit(RAM, A, pt, 1) == VRC_HIT_DESTROYED, "bullet_hits 3: 2 dañan, la 3ª destruye");
    VrcConfig dflt; vrc_config_default(&dflt);
    CHECK(dflt.bullets_destroy == 1 && dflt.bullet_hits == 2 && dflt.cosmetic_sweep == 0, "valores por defecto: balas rompen con 2, barrido apagado");
    cfg.bullet_hits = -5; vrc_config_set(&cfg); vrc_config_get(&dflt); CHECK(dflt.bullet_hits == 1, "bullet_hits saneado");
    vai_w16(RAM, TARGETS + 0x28, 32767);
    CHECK(vr_cameras_hit_ex(RAM, A, pt, 2147483647, VRC_DMG_EXPLOSIVE) == VRC_HIT_DESTROYED && vai_r16(RAM, TARGETS + 0x28) == 32767, "daño INT_MAX sin desbordar");
    CHECK(vr_cameras_hit_ex(RAM, A, pt, 5, (VrcDamageKind)7) == VRC_HIT_BAD, "tipo inválido");
    /* clase no dañable (cámara sencilla en patrulla) */
    world_init();
    cam_make(A, ACT_A, 0, 1, 2, 3, 0, 0, 100, 10, 0, 1, 0, 1); list_add(4, A, ACT_A);
    vai_w16(RAM, TARGETS, 1);
    vrc_config_get(&cfg); cfg.ignore_class = 0; vrc_config_set(&cfg);
    CHECK(vr_cameras_hit_ex(RAM, A, pt, 500, VRC_DMG_EXPLOSIVE) == VRC_HIT_IGNORED, "respeta la clase si se pide");
    cfg.ignore_class = 1; vrc_config_set(&cfg);
    CHECK(vr_cameras_hit_ex(RAM, A, pt, 500, VRC_DMG_EXPLOSIVE) == VRC_HIT_DESTROYED, "ignora la clase por defecto");
    vai_w16(RAM, TARGETS, 0);
    CHECK(vr_cameras_hit_ex(RAM, A, pt, 500, VRC_DMG_EXPLOSIVE) == VRC_HIT_IGNORED, "blanco retirado: ignorado");
    /* saturación del daño acumulado */
    vai_w16(RAM, TARGETS, 0x15); vai_w16(RAM, TARGETS + 0x28, 32000);
    CHECK(vr_cameras_hit_ex(RAM, A, pt, 2000000000, VRC_DMG_EXPLOSIVE) == VRC_HIT_DESTROYED && vai_r16(RAM, TARGETS + 0x28) == 32767, "saturación");
}

static void t_frame_bullets(void) {
    world_init();
    uint32_t A = 0x80120000u;
    cam_make(A, ACT_A, 0, 100, 200, 300, 0, 0, 4000, 256, 0, 1, 0, 1); list_add(4, A, ACT_A);
    VrcTable t;
    uint32_t T = TARGETS;
    /* impacto de bala del juego: bit 4 + modo 1 */
    for (int shot = 1; shot <= 2; shot++) {
        vai_w16(RAM, T + 6, 4); vai_w16(RAM, T + 0x3e, 1); vai_w16(RAM, T + 0x28, 5);
        vrc_frame(RAM, &t);
        if (shot < 2) CHECK(vai_r16(RAM, T + 6) == 0 && vai_r16(RAM, T + 0x28) == 0, "tiro %d: se limpia como haría la cámara", shot);
        else CHECK((vai_r16(RAM, T + 6) & 4) && vai_r16(RAM, T + 0x3e) == 2, "tiro 2: pasa a modo 2 y la cámara la destruye");
    }
    CHECK(vrc_table() && vrc_table()->n == 1 && vrc_table()->cam[0].hits == 2, "acumulado visible en la tabla (%d)", vrc_table() ? vrc_table()->cam[0].hits : -1);
    /* sin recogida: no se toca */
    world_init();
    cam_make(A, ACT_A, 0, 100, 200, 300, 0, 0, 4000, 256, 0, 1, 0, 1); list_add(4, A, ACT_A);
    VrcConfig cfg; vrc_config_get(&cfg); cfg.pickup_game_bullets = 0; vrc_config_set(&cfg);
    vai_w16(RAM, T + 6, 4); vai_w16(RAM, T + 0x3e, 1);
    vrc_frame(RAM, &t);
    CHECK(vai_r16(RAM, T + 6) == 4 && vai_r16(RAM, T + 0x3e) == 1, "recogida desactivada: intacto");
    /* originales (bullets_destroy=0): no se modifica nada */
    cfg.pickup_game_bullets = 1; cfg.bullets_destroy = 0; vrc_config_set(&cfg);
    vrc_frame(RAM, &t);
    CHECK(vai_r16(RAM, T + 6) == 4 && vai_r16(RAM, T + 0x3e) == 1, "política original: intacto");
    /* explosión (modo 2) no se toca */
    cfg.bullets_destroy = 1; vrc_config_set(&cfg);
    vai_w16(RAM, T + 0x3e, 2); vrc_frame(RAM, &t);
    CHECK(vai_r16(RAM, T + 0x3e) == 2 && vai_r16(RAM, T + 6) == 4, "modo 2 se respeta");
    /* cámara que desaparece: olvida el acumulado */
    vai_w16(RAM, T + 0x3e, 1); vrc_frame(RAM, &t);
    lists_reset(); vrc_frame(RAM, &t);
    CHECK(t.n == 0, "sin cámaras");
    list_add(4, A, ACT_A); vrc_frame(RAM, &t);
    CHECK(vrc_find(&t, A) && vrc_find(&t, A)->hits == 0, "acumulado olvidado tras desaparecer");
}

static void t_sweep(void) {
    world_init();
    uint32_t A = 0x80120000u;
    cam_make(A, ACT_A, 0, 100, 200, 300, 1000, 0, 4000, 256, 120, 0 /* apagada */, 0, 1); list_add(4, A, ACT_A);
    VrcTable t; int ymin = 9999, ymax = -9999, flips = 0, last = 0;
    vai_w16(RAM, A + 0x6e, 1000);
    for (int f = 0; f < 300; f++) vrc_frame(RAM, &t);
    CHECK(vai_r16(RAM, A + 0x6e) == 1000, "por defecto el barrido no escribe en la RAM del juego");
    { VrcConfig c0; vrc_config_get(&c0); c0.cosmetic_sweep = 1; vrc_config_set(&c0); }
    for (int f = 0; f < 600; f++) {
        vrc_frame(RAM, &t);
        int y = vai_r16(RAM, A + 0x6e);
        int d = ((y - 1000 + 2048) & 4095) - 2048;
        if (d < ymin) ymin = d;
        if (d > ymax) ymax = d;
        int dir = d > last ? 1 : (d < last ? -1 : 0);
        static int pd; if (dir && pd && dir != pd) flips++;
        if (dir) pd = dir;
        last = d;
    }
    CHECK(ymax >= 100 && ymax <= 128 && ymin <= -100 && ymin >= -128, "barrido acotado por el giro máximo (%d..%d)", ymin, ymax);
    CHECK(flips >= 2, "invierte el sentido (%d)", flips);
    /* al reactivarla el script se restaura el giro original (la detección no ve un rumbo alterado) */
    vai_w16(RAM, A + 0x284, 1); vrc_frame(RAM, &t);
    CHECK(vai_r16(RAM, A + 0x6e) == 1000, "reactivada: giro original restaurado (%d)", vai_r16(RAM, A + 0x6e));
    /* activa: no escribe nada */
    vai_w16(RAM, A + 0x6e, 777);
    for (int f = 0; f < 200; f++) vrc_frame(RAM, &t);
    CHECK(vai_r16(RAM, A + 0x6e) == 777, "activa: no se toca");
    /* muerta: no escribe */
    vai_w16(RAM, A + 0x284, 0); vai_w32(RAM, A + 0x1F0, 3);
    for (int f = 0; f < 200; f++) vrc_frame(RAM, &t);
    CHECK(vai_r16(RAM, A + 0x6e) == 777, "muerta: no se toca");
    vai_w32(RAM, A + 0x1F0, 0);
    /* desactivado por configuración */
    VrcConfig cfg; vrc_config_get(&cfg); cfg.cosmetic_sweep = 0; vrc_config_set(&cfg);
    for (int f = 0; f < 200; f++) vrc_frame(RAM, &t);
    CHECK(vai_r16(RAM, A + 0x6e) == 777, "desactivado");
    /* giro máx < 8 (cámara fija): no gira; distancia limitada */
    cfg.cosmetic_sweep = 1; vrc_config_set(&cfg);
    vai_w16(RAM, A + 0x282, 4);
    for (int f = 0; f < 200; f++) vrc_frame(RAM, &t);
    CHECK(vai_r16(RAM, A + 0x6e) == 777, "cámara fija no gira");
    vai_w16(RAM, A + 0x282, 120);
    cfg.sweep_max_dist = 500; vrc_config_set(&cfg);
    int32_t far_[3] = {100000, 0, 0}; vrc_set_listener(far_, 0, 1);
    for (int f = 0; f < 200; f++) vrc_frame(RAM, &t);
    CHECK(vai_r16(RAM, A + 0x6e) == 777, "oyente lejos: no gira");
    int32_t near_[3] = {100, 200, 400}; vrc_set_listener(near_, 0, 1);
    for (int f = 0; f < 200; f++) vrc_frame(RAM, &t);
    CHECK(vai_r16(RAM, A + 0x6e) != 777, "oyente cerca: gira");
}

static void t_ray(void) {
    world_init();
    uint32_t A = 0x80120000u, B = 0x80121000u;
    cam_make(A, ACT_A, 0, 0, 0, 5000, 0, 0, 4000, 256, 0, 1, 0, 1);
    cam_make(B, ACT_A, 1, 0, 0, 9000, 0, 0, 4000, 256, 0, 1, 0, 1);
    list_add(4, A, ACT_A); list_add(4, B, ACT_A);
    VrcTable t; vrc_scan(RAM, &t);
    float o[3] = {0, 0, 0}, d[3] = {0, 0, 1}, tt = 0; int16_t h[3];
    int i = vrc_raycast(&t, o, d, 20000, &tt, h);
    CHECK(i >= 0 && t.cam[i].actor == A && fabsf(tt - 4700.f) < 1.f && h[2] == 4700, "el más cercano (t=%f)", tt);
    float d2[3] = {0, 0, 5}; CHECK(vrc_raycast(&t, o, d2, 20000, &tt, h) == i, "dirección no normalizada");
    CHECK(vrc_raycast(&t, o, d, 4000, &tt, h) < 0, "fuera de alcance");
    float d3[3] = {0, 1, 0}; CHECK(vrc_raycast(&t, o, d3, 20000, &tt, h) < 0, "ortogonal");
    float o4[3] = {400, 0, 0}; CHECK(vrc_raycast(&t, o4, d, 20000, &tt, h) < 0, "fuera de la caja en x");
    float o5[3] = {0, 0, 5000}; CHECK(vrc_raycast(&t, o5, d, 20000, &tt, h) == i && tt == 0.f, "origen dentro");
    float dz[3] = {0, 0, 0}; CHECK(vrc_raycast(&t, o, dz, 20000, &tt, h) < 0, "dirección nula");
    float onan[3] = {NAN, 0, 0}; CHECK(vrc_raycast(&t, onan, d, 20000, &tt, h) < 0, "origen NaN");
    CHECK(vrc_raycast(NULL, o, d, 1, &tt, h) < 0 && vrc_raycast(&t, NULL, d, 1, &tt, h) < 0 && vrc_raycast(&t, o, d, NAN, &tt, h) < 0, "NULL/NaN");
    vai_w32(RAM, A + 0x1F0, 1); vrc_scan(RAM, &t);
    i = vrc_raycast(&t, o, d, 20000, &tt, h);
    CHECK(i >= 0 && t.cam[i].actor == B, "la muerta no cuenta");
}

static void t_sound(void) {
    world_init();
    uint32_t A = 0x80120000u;
    cam_make(A, ACT_A, 0, 4000, 500, 0, 0, 0, 4000, 256, 0, 1, 0, 1); list_add(4, A, ACT_A);
    VrcTable t; vrc_frame(RAM, &t);
    int32_t ear[3] = {0, 500, 0}; vrc_set_listener(ear, 0, 1);
    uint32_t cmd = 0;
    /* posición del emisor en RAM (en la misma que usa el juego: control.mov del actor) */
    uint32_t pos = A + 0x20;
    CHECK(vrc_se_intercept(RAM, pos, 94, &cmd) == VRC_SE_PLAY, "scan se reproduce");
    CHECK((cmd & 0xff) == 94 && ((cmd >> 8) & 0xff) > 0 && ((cmd >> 8) & 0xff) <= 63, "comando: id y volumen (cmd=%08x)", cmd);
    CHECK(vrc_se_intercept(RAM, pos, 17, &cmd) == VRC_SE_PASS, "otro sonido: pasa");
    CHECK(vrc_se_intercept(RAM, 0, 94, &cmd) == VRC_SE_PASS, "sin posición: pasa");
    CHECK(vrc_se_intercept(NULL, pos, 94, &cmd) == VRC_SE_PASS, "ram NULL");
    VrcSoundEvent ev[8]; int n = vrc_sound_events(ev, 8);
    CHECK(n == 1 && ev[0].se == 94 && ev[0].pos[0] == 4000 && ev[0].pos[1] == 500 && ev[0].pos[2] == 0, "evento con posición 3D del emisor");
    CHECK(ev[0].cam_index == 0 && ev[0].actor == A && fabsf(ev[0].dist - 4000.f) < 1e-3f && ev[0].played, "asociado a la cámara, distancia");
    CHECK(ev[0].vol_orig == 63 - 4000 / 150 && ev[0].vol == ev[0].vol_orig, "vol = max(natural, original) = original a 4000 (%d)", ev[0].vol);
    CHECK(fabsf(ev[0].gain - (float)ev[0].vol_orig / 63.f) < 1e-3f, "ganancia continua");
    CHECK(vrc_sound_events(ev, 8) == 0, "vaciado");
    /* lejos: el original ya no suena, el natural sí; muy lejos silencio */
    vai_w16(RAM, A + 0x20, 12000);
    CHECK(vrc_se_intercept(RAM, pos, 95, &cmd) == VRC_SE_PLAY, "a 12000 suena (original: no)");
    n = vrc_sound_events(ev, 8); CHECK(n == 1 && ev[0].vol_orig == 0 && ev[0].vol >= 1, "original 0, natural >0");
    vai_w16(RAM, A + 0x20, 32000);
    CHECK(vrc_se_intercept(RAM, pos, 95, &cmd) == VRC_SE_DROP, "a 32000 silencio");
    n = vrc_sound_events(ev, 8); CHECK(n == 1 && !ev[0].played && ev[0].pos[0] == 32000, "evento sin reproducir pero con posición");
    /* paneo: fuente a la derecha / izquierda / delante */
    vai_w16(RAM, A + 0x20, 3000);
    vrc_se_intercept(RAM, pos, 94, &cmd); int p_right = (int)((cmd >> 16) & 0xff);
    vai_w16(RAM, A + 0x20, -3000);
    vrc_se_intercept(RAM, pos, 94, &cmd); int p_left = (int)((cmd >> 16) & 0xff);
    vai_w16(RAM, A + 0x20, 0); vai_w16(RAM, A + 0x24, 3000);
    vrc_se_intercept(RAM, pos, 94, &cmd); int p_front = (int)((cmd >> 16) & 0xff);
    CHECK(p_right != p_left && (p_right + p_left == 255 || abs(p_right + p_left - 255) <= 2), "pan simétrico (%d %d)", p_right, p_left);
    CHECK(p_front <= 2 || p_front >= 253, "delante: pan ~0 (%d)", p_front);
    vrc_sound_events(ev, 8);
    /* el rumbo del oyente gira el paneo */
    int32_t ear2[3] = {0, 500, 0}; vrc_set_listener(ear2, 1024, 1);
    vai_w16(RAM, A + 0x20, 0); vai_w16(RAM, A + 0x24, 3000);
    vrc_se_intercept(RAM, pos, 94, &cmd); int p_turn = (int)((cmd >> 16) & 0xff);
    CHECK(abs(p_turn - p_front) > 20 && p_turn != p_front, "girar la cabeza cambia el paneo (%d vs %d)", p_turn, p_front);
    vrc_sound_events(ev, 8);
    /* fin de partida/demo: silencio como el original */
    vai_w32(RAM, STATUS, 0x80000000u);
    CHECK(vrc_se_intercept(RAM, pos, 94, &cmd) == VRC_SE_DROP, "demo: silencio");
    vai_w32(RAM, STATUS, 0x04000000u);
    CHECK(vrc_se_intercept(RAM, pos, 94, &cmd) == VRC_SE_DROP, "fin de partida: silencio");
    vai_w32(RAM, STATUS, 0);
    vrc_sound_events(ev, 8);
    /* modo ORIGINAL: pasa, pero registra el emisor */
    VrcSoundCfg sc; vrc_sound_get(&sc); sc.mode = VRC_SND_ORIGINAL; vrc_sound_set(&sc);
    CHECK(vrc_se_intercept(RAM, pos, 94, &cmd) == VRC_SE_PASS, "original: pasa");
    n = vrc_sound_events(ev, 8); CHECK(n == 1 && ev[0].pos[2] == 3000, "original: registra el emisor");
    /* anillo */
    for (int i = 0; i < 100; i++) vrc_se_intercept(RAM, pos, 94, &cmd);
    n = vrc_sound_events(ev, 8); CHECK(n == 8, "máx respetado");
    n = vrc_sound_events(NULL, 1000); CHECK(n <= VRC_MAX_SND_EVENTS, "drena todo con out NULL");
    CHECK(vrc_sound_events(ev, 0) == 0 && vrc_sound_events(ev, -3) == 0, "max<=0");
    /* otros ids configurables */
    sc.mode = VRC_SND_NATURAL; sc.n_se = 1; sc.se[0] = 17; vrc_sound_set(&sc);
    CHECK(vrc_se_intercept(RAM, pos, 94, &cmd) == VRC_SE_PASS && vrc_se_intercept(RAM, pos, 17, &cmd) != VRC_SE_PASS, "lista de ids configurable");
    /* puntero de posición inválido */
    CHECK(vrc_se_intercept(RAM, 0x1234, 17, &cmd) == VRC_SE_PASS && vrc_se_intercept(RAM, 0x80200000u, 17, &cmd) == VRC_SE_PASS, "pos inválida: pasa");
}

static void t_listener_fallback(void) {
    world_init();
    uint32_t A = 0x80120000u;
    cam_make(A, ACT_A, 0, 3000, 0, 0, 0, 0, 4000, 256, 0, 1, 0, 1); list_add(4, A, ACT_A);
    VrcTable t; vrc_frame(RAM, &t);
    /* sin oyente VR: tercera persona -> posición del jugador */
    vai_w16(RAM, 0x800ACBA0u, 0); vai_w16(RAM, 0x800ACBA2u, 0); vai_w16(RAM, 0x800ACBA4u, 0);
    uint32_t cmd; VrcSoundEvent ev[2];
    vrc_se_intercept(RAM, A + 0x20, 94, &cmd);
    CHECK(vrc_sound_events(ev, 2) == 1 && fabsf(ev[0].dist - 3000.f) < 1e-3f, "tercera persona: mide desde el jugador");
    /* primera persona -> ojo del juego: t = (1000,0,0) */
    vai_w16(RAM, 0x800B8968u + 0x22, 1);
    vai_w32(RAM, 0x800B2B7Cu + 0x30 + 0x14, 1000);
    vrc_se_intercept(RAM, A + 0x20, 94, &cmd);
    CHECK(vrc_sound_events(ev, 2) == 1 && fabsf(ev[0].dist - 2000.f) < 1e-3f, "primera persona: mide desde el ojo del juego (%f)", ev[0].dist);
    /* oyente VR manda sobre todo */
    int32_t e[3] = {2000, 0, 0}; vrc_set_listener(e, 0, 1);
    vrc_se_intercept(RAM, A + 0x20, 94, &cmd);
    CHECK(vrc_sound_events(ev, 2) == 1 && fabsf(ev[0].dist - 1000.f) < 1e-3f, "oyente VR");
    vrc_set_listener(e, 0, 0);
    vrc_se_intercept(RAM, A + 0x20, 94, &cmd);
    CHECK(vrc_sound_events(ev, 2) == 1 && fabsf(ev[0].dist - 2000.f) < 1e-3f, "oyente VR retirado");
}

/* Fuzz: RAM aleatoria con listas coherentes y estructuras corruptas: nada debe salirse de la RAM ni colgar. */
static void t_fuzz(void) {
    unsigned s = 12345;
    for (int it = 0; it < 300; it++) {
        world_init();
        for (uint32_t a = 0x80120000u; a < 0x80124000u; a += 4) { s = s * 1664525u + 1013904223u; vai_w32(RAM, a, s); }
        for (int k = 0; k < 6; k++) {
            uint32_t n = 0x80120000u + 0x400u * (uint32_t)k;
            s = s * 1664525u + 1013904223u;
            cam_make(n, (s & 1) ? ACT_A : ACT_B, k, (int16_t)s, (int16_t)(s >> 8), (int16_t)(s >> 16), (int16_t)s, (int16_t)(s >> 4), (uint16_t)s, (uint16_t)(s >> 3), (uint16_t)s, s & 1, (int)(s % 5), s);
            if (s & 0x100) vai_w32(RAM, n + 0x1CC, s);
            list_add((int)(s % 9), n, (s & 1) ? ACT_A : ACT_B);
        }
        VrcTable t;
        vrc_frame(RAM, &t);
        for (int k = 0; k < t.n; k++) { int16_t p[3] = {1, 2, 3}; vr_cameras_hit(RAM, t.cam[k].actor, p, (int)(s % 300) - 20); vr_cameras_hit_ex(RAM, t.cam[k].actor, p, 70, VRC_DMG_EXPLOSIVE); vrc_in_cone(&t.cam[k], p); }
        uint32_t cmd;
        for (int k = 0; k < 20; k++) { s = s * 1664525u + 1013904223u; vrc_se_intercept(RAM, 0x80120000u + (s & 0x3FFF), 94 + (s % 3) * 7, &cmd); }
        float o[3] = {0, 0, 0}, d[3] = {1, 1, 1}; vrc_raycast(&t, o, d, 1e9f, NULL, NULL);
        VrcSoundEvent ev[4]; vrc_sound_events(ev, 4);
    }
    CHECK(1, "fuzz");
}

/* ---- Con las overlays reales (si existen) ---- */
static uint8_t *slurp(const char *path, long *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)*n + 1);
    if (!b || fread(b, 1, (size_t)*n, f) != (size_t)*n) { fclose(f); free(b); return NULL; }
    fclose(f); return b;
}
static const struct { const char *ov; uint32_t act; VrcFamily fam; } REAL[] = {
    {"s01a", 0x800d77a0u, VRC_FAM_CAMERA}, {"s02a", 0x800d6becu, VRC_FAM_CAMERA}, {"s02b", 0x800d6e28u, VRC_FAM_CAMERA},
    {"s02c", 0x800d6be0u, VRC_FAM_CAMERA}, {"s02d", 0x800d6de4u, VRC_FAM_CAMERA}, {"s02e", 0x800d6be0u, VRC_FAM_CAMERA},
    {"s06a", 0x800d6ac0u, VRC_FAM_CAMERA}, {"s16c", 0x800d4558u, VRC_FAM_CAMERA}, {"s16d", 0x800d4974u, VRC_FAM_CAMERA},
    {"d16e", 0x800c5a98u, VRC_FAM_CAMERA}, {"vr09", 0x800d5368u, VRC_FAM_CAMERA}, {"vr10", 0x800d5490u, VRC_FAM_CAMERA},
    {"s11a", 0x800c66d0u, VRC_FAM_SIMPLE}, {"s19a", 0x800c6fc0u, VRC_FAM_SIMPLE},
    {"s04c", 0x800c9464u, VRC_FAM_GUN}, {"s04br", 0x800cb690u, VRC_FAM_GUN}, {"s10ar", 0x800d34e4u, VRC_FAM_GUN}, {"s12b", 0x800c8eacu, VRC_FAM_GUN}, {"s12c", 0x800c8ea0u, VRC_FAM_GUN},
    {"s03a", 0x800cd2f4u, VRC_FAM_GUN}, {"s03c", 0x800d24f4u, VRC_FAM_GUN}, {"s03e", 0x800c93a4u, VRC_FAM_GUN}, {"s04b", 0x800cb690u, VRC_FAM_GUN},
    {"s05a", 0x800cac74u, VRC_FAM_GUN}, {"s08a", 0x800cbd78u, VRC_FAM_GUN}, {"s10a", 0x800d34e4u, VRC_FAM_GUN}, {"s11c", 0x800c8664u, VRC_FAM_GUN},
    {"s12a", 0x800cb960u, VRC_FAM_GUN}, {"s13a", 0x800db804u, VRC_FAM_GUN}, {"s14e", 0x800d1c24u, VRC_FAM_GUN}, {"s15b", 0x800c6d10u, VRC_FAM_GUN}, {"s16a", 0x800c7b60u, VRC_FAM_GUN},
};
static void t_real(const char *dir) {
    int fp = 0, found = 0, ovs = 0;
    DIR *d = opendir(dir);
    if (!d) { printf("  (sin %s: parte con overlays reales omitida)\n", dir); return; }
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t L = strlen(de->d_name);
        if (L < 5 || strcmp(de->d_name + L - 4, ".bin")) continue;
        char path[1024], name[64];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        snprintf(name, sizeof name, "%.*s", (int)(L - 4), de->d_name);
        long n; uint8_t *b = slurp(path, &n);
        if (!b || n > 0x100000) { free(b); continue; }
        vrc_reset_all(); memset(RAM, 0, sizeof RAM);
        memcpy(RAM + (0x800C4380u & 0x1FFFFF), b, (size_t)n);
        free(b); ovs++;
        int expect_fam = -1; uint32_t expect_act = 0;
        for (unsigned i = 0; i < sizeof REAL / sizeof *REAL; i++) if (!strcmp(REAL[i].ov, name)) { expect_fam = REAL[i].fam; expect_act = REAL[i].act; }
        /* cada prólogo de función de la overlay: solo el actor conocido puede clasificarse como cámara */
        for (uint32_t a = 0x800C4380u; a < 0x800C4380u + (uint32_t)n; a += 4) {
            if (!vai_is_prologue(vai_r32(RAM, a))) continue;
            /* los actores empiezan en el prólogo o 1-2 palabras antes: probar las tres */
            for (uint32_t k = 0; k < 3; k++) {
                uint32_t act = a - 4 * k;
                if (act < 0x800C4380u) continue;
                uint32_t actor = 0x80120000u;
                vai_w32(RAM, actor + 8, act);
                int f = vrc_classify_actor(RAM, actor);
                if (f >= 0) {
                    if (act == expect_act && f == expect_fam) found++;
                    else { fp++; printf("  falso positivo en %s: %08x (fam %d)\n", name, act, f); }
                }
            }
        }
    }
    closedir(d);
    CHECK(fp == 0, "sin falsos positivos en %d overlays", ovs);
    CHECK(ovs == 0 || found == (int)(sizeof REAL / sizeof *REAL) || ovs < 90, "todos los actores conocidos clasificados (%d de %d, overlays %d)", found, (int)(sizeof REAL / sizeof *REAL), ovs);
    printf("  overlays reales: %d, actores de cámara clasificados %d/%d, falsos positivos %d\n", ovs, found, (int)(sizeof REAL / sizeof *REAL), fp);
}

int main(int argc, char **argv) {
    t_curve(); t_scan(); t_cone(); t_hit(); t_frame_bullets(); t_sweep(); t_ray(); t_sound(); t_listener_fallback(); t_fuzz();
    t_real(argc > 1 ? argv[1] : "../../build/ov");
    printf("test_cameras: %d correctas, %d fallos\n", nok, nfail);
    return nfail ? 1 : 0;
}
