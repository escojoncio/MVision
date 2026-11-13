/* Pruebas de la mira del francotirador (siempre activa): lente tomada del modelo, visibilidad de la lente, zoom/FOV del original (weapon/rifle.c) y
 * eje de la mira = rayo del disparo siempre. Compilar: cc -I. test_sniper_scope.c w_sniper.c w_gunc.c wcore.c -lm */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "w_sniper.h"
#include "wload.h"
#include "wcut.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FALLO %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define NEAR(a, b, e) (fabsf((a) - (b)) <= (e))

typedef struct { int fired; WPose muz; } Br;
static Br B;
static int b_res(int id) { (void)id; return 100; }
static void b_take(int id, int n) { (void)id; (void)n; }
static void b_fire(int id, WPose p) { (void)id; B.fired++; B.muz = p; }
static void b_hap(int h, float k, float t) { (void)h; (void)k; (void)t; }
static void b_snd(int id, int e) { (void)id; (void)e; }
static WGameBridge BR = { .ammo_reserve = b_res, .ammo_take = b_take, .fire = b_fire, .haptic = b_hap, .sound = b_snd };

static WInput mk(void) {
    WInput in; memset(&in, 0, sizeof in);
    in.head.q.w = 1; in.head.p = (WVec3){0, 1.7f, 0};
    for (int i = 0; i < 2; i++) in.hand[i].q.w = 1;
    in.hand[0].p = (WVec3){0.2f, 1.3f, -0.4f};
    in.hand[1].p = (WVec3){-0.2f, 1.2f, -0.6f};
    in.dt = 1.0f / 90;
    return in;
}
static WeaponState *mkw(void) { WeaponState *s = calloc(1, w_sniper.state_size); s->def = &w_sniper; w_sniper.init(s); w_sniper.make_ready(s, 5); return s; }   /* cargada, como la deja el anfitrión */
static void step(WeaponState *s, WInput *in, int n) { for (int i = 0; i < n; i++) s->def->update(s, in, WR_MANUAL, &BR); }
static WVec3 fwd_of(WQuat q) { return wquat_rotate(q, wv(0, 0, -1)); }

/* ---- lente tomada del modelo ---- */
enum { MESH_LENS = 7, N_MESH = 9 };
static WVertex qv[4];
static uint16_t qi[6];
static WMesh meshes[N_MESH];
static WPart parts[SNIPER_PT_N];

static void quad(float hw, float hh, float tilt_deg, int flip) {
    float c = cosf(tilt_deg * 0.017453292f), sn = sinf(tilt_deg * 0.017453292f);
    const float xy[4][2] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
    memset(qv, 0, sizeof qv);
    for (int i = 0; i < 4; i++) { qv[i].pos[0] = xy[i][0]; qv[i].pos[1] = xy[i][1] * c; qv[i].pos[2] = xy[i][1] * sn; }
    const uint16_t a[6] = {0, 1, 2, 0, 2, 3}, b[6] = {0, 2, 1, 0, 3, 2};
    memcpy(qi, flip ? b : a, sizeof qi);
    memset(meshes, 0, sizeof meshes);
    meshes[MESH_LENS] = (WMesh){ .name = "lens", .v = qv, .nv = 4, .idx = qi, .ni = 6 };
}
static WeaponState *mkbound(WVec3 rest) {
    WeaponState *s = mkw();
    memcpy(parts, w_sniper.parts, sizeof parts);
    parts[SNIPER_PT_LENS].rest.p = rest;
    weapon_bind(s, meshes, N_MESH, parts, SNIPER_PT_N);
    return s;
}

static void test_lens_model(void) {
    SniperLens L;
    /* sin enlazar: reposo de la definición, eje -Z */
    WeaponState *s0 = mkw();
    CHECK(sniper_lens(s0, &L) == 0 && !L.model);
    CHECK(NEAR(L.pose.p.y, 0.1475f, 1e-6f) && NEAR(L.pose.p.z, 0.1002f, 1e-6f) && NEAR(L.d, 0.04f, 1e-6f));
    free(s0);
    /* placa de 50 x 32 mm en (0, 0,130, 0,074): el centro y el diámetro salen del modelo, no de las constantes */
    for (int flip = 0; flip < 2; flip++) {
        quad(0.025f, 0.016f, 0.0f, flip);
        WeaponState *s = mkbound(wv(0.001f, 0.130f, 0.074f));
        CHECK(sniper_lens(s, &L) == 1 && L.model);
        CHECK(NEAR(L.pose.p.x, 0.001f, 1e-6f) && NEAR(L.pose.p.y, 0.130f, 1e-6f) && NEAR(L.pose.p.z, 0.074f, 1e-6f));
        WVec3 a = fwd_of(L.pose.q);
        CHECK(NEAR(a.x, 0, 1e-5f) && NEAR(a.y, 0, 1e-5f) && NEAR(a.z, -1, 1e-5f));   /* con cualquier sentido de giro de los triángulos */
        CHECK(NEAR(L.d, 0.032f, 1e-6f));
        free(s);
    }
    /* centro de la malla desplazado respecto al pivote: se suma */
    quad(0.025f, 0.016f, 0.0f, 0);
    for (int i = 0; i < 4; i++) qv[i].pos[1] += 0.004f;
    WeaponState *s = mkbound(wv(0, 0.130f, 0.074f));
    CHECK(sniper_lens(s, &L) == 1 && NEAR(L.pose.p.y, 0.134f, 1e-6f));
    free(s);
    /* placa inclinada 5 grados: el eje la sigue; 25 grados (no es una cara trasera): -Z */
    quad(0.025f, 0.016f, 5.0f, 0);
    s = mkbound(wv(0, 0.130f, 0.074f));
    sniper_lens(s, &L);
    WVec3 a = fwd_of(L.pose.q), e = wv(0, sinf(5.0f * 0.017453292f), -cosf(5.0f * 0.017453292f));
    CHECK(wv_len(wv_sub(a, e)) < 1e-4f);
    free(s);
    quad(0.025f, 0.016f, 25.0f, 0);
    s = mkbound(wv(0, 0.130f, 0.074f));
    sniper_lens(s, &L);
    a = fwd_of(L.pose.q);
    CHECK(NEAR(a.z, -1, 1e-5f));
    free(s);
    /* malla vacía / tamaño absurdo: sin romper; sin límite 30-60 mm: una placa de 20 x 10 mm da 20 x 10 */
    quad(0.2f, 0.2f, 0.0f, 0);                                       /* 400 mm: absurdo, reposo */
    s = mkbound(wv(0, 0.130f, 0.074f));
    CHECK(sniper_lens(s, &L) == 0 && !L.model && NEAR(L.w, 0.04f, 1e-6f));
    free(s);
    quad(0.010f, 0.005f, 0.0f, 0);
    s = mkbound(wv(0, 0.130f, 0.074f));
    CHECK(sniper_lens(s, &L) == 1 && NEAR(L.w, 0.020f, 1e-6f) && NEAR(L.h, 0.010f, 1e-6f) && NEAR(L.d, 0.010f, 1e-6f) && !L.rim);
    free(s);
    quad(0.2f, 0.2f, 0.0f, 0);
    s = mkbound(wv(0, 0.130f, 0.074f));
    meshes[MESH_LENS].nv = 0;
    CHECK(sniper_lens(s, &L) == 0 && !L.model && NEAR(L.pose.p.y, 0.130f, 1e-6f));
    free(s);
}

/* ---- abertura trasera del tubo de la mira (cuerpo): la imagen llena su contorno interior ---- */
enum { MAXBV = 64 };
static WVertex bv[MAXBV];
static uint16_t bi_[3];
static int nbv;
static void bvert(float x, float y, float z) { bv[nbv].pos[0] = x; bv[nbv].pos[1] = y; bv[nbv].pos[2] = z; nbv++; }
/* tubo de n lados centrado en (0, cy): anillo exterior ro y anillo interior ri a zr (el interior a zr + dz_in), fondo del tubo a zbot; decoy = cara trasera de una culata por debajo del eje */
static void tube(int n, float phase_deg, float ro, float ri, float cy, float zr, float dz_in, float zbot, int decoy) {
    nbv = 0;
    for (int k = 0; k < n; k++) {
        float a = (phase_deg + 360.0f * k / n) * 0.017453292f;
        bvert(ro * cosf(a), cy + ro * sinf(a), zr);
        bvert(ri * cosf(a), cy + ri * sinf(a), zr + dz_in);
        bvert(ri * cosf(a), cy + ri * sinf(a), zbot);
    }
    if (decoy) {   /* caja de culata: por debajo y detrás del eje (no rodea el centro) */
        bvert(-0.03f, cy - 0.025f, 0.20f); bvert(0.03f, cy - 0.025f, 0.20f);
        bvert(0.03f, cy - 0.075f, 0.20f); bvert(-0.03f, cy - 0.075f, 0.20f);
    }
    memset(meshes, 0, sizeof meshes);
    meshes[MESH_LENS] = (WMesh){ .name = "lens", .v = qv, .nv = 4, .idx = qi, .ni = 6 };
    meshes[0] = (WMesh){ .name = "body", .v = bv, .nv = (uint32_t)nbv, .idx = bi_, .ni = 3 };
}
static void test_rim(void) {
    SniperLens L;
    const float cy = 0.1475f, zl = 0.1002f;
    /* octógono (vértices a 22,5 grados): anillo interior 0,8 mm por delante del borde exterior; plano del borde 40 mm detrás de la placa; culata como señuelo más atrás */
    quad(0.025f, 0.016f, 0.0f, 0);
    tube(8, 22.5f, 0.034f, 0.026f, cy, zl + 0.040f, -0.0008f, zl - 0.030f, 1);
    WeaponState *s = mkbound(wv(0, cy, zl));
    CHECK(sniper_lens(s, &L) == 1 && L.model && L.rim == 1 && L.npoly == 8);
    float half = 0.026f * cosf(22.5f * 0.017453292f);              /* semilado del octógono interior */
    CHECK(NEAR(L.w, 2 * half, 1e-3f) && NEAR(L.h, 2 * half, 1e-3f));                 /* contorno interior, no la placa de 50 x 32 ni un disco acotado */
    CHECK(NEAR(L.face.p.x, 0, 1e-3f) && NEAR(L.face.p.y, cy, 1e-3f) && NEAR(L.face.p.z, zl + 0.040f, 1e-3f));   /* centro en el plano del borde */
    WVec3 a = fwd_of(L.face.q);
    CHECK(NEAR(a.z, -1, 1e-5f));
    CHECK(NEAR(L.pose.p.y, cy, 1e-6f) && NEAR(L.pose.p.z, zl, 1e-6f));              /* cámara y bala siguen saliendo de la placa */
    free(s);
    /* anillo de 16 lados centrado 5 mm más alto que la placa: el centro sale del contorno */
    tube(16, 0.0f, 0.040f, 0.030f, cy + 0.005f, zl + 0.020f, 0.0f, zl - 0.030f, 1);
    s = mkbound(wv(0, cy, zl));
    CHECK(sniper_lens(s, &L) == 1 && L.rim == 1);
    CHECK(NEAR(L.w, 0.060f, 1e-3f) && NEAR(L.h, 0.060f, 1e-3f) && NEAR(L.face.p.y, cy + 0.005f, 1e-3f) && NEAR(L.face.p.x, 0, 1e-3f));
    free(s);
    /* sin tubo que rodee el centro (solo la culata): se usa la placa */
    nbv = 0;
    bvert(-0.03f, cy - 0.025f, 0.20f); bvert(0.03f, cy - 0.025f, 0.20f); bvert(0.03f, cy - 0.075f, 0.20f); bvert(-0.03f, cy - 0.075f, 0.20f);
    meshes[0] = (WMesh){ .name = "body", .v = bv, .nv = 4, .idx = bi_, .ni = 3 };
    s = mkbound(wv(0, cy, zl));
    CHECK(sniper_lens(s, &L) == 1 && L.rim == 0 && NEAR(L.w, 0.050f, 1e-6f) && NEAR(L.h, 0.032f, 1e-6f) && !L.round && NEAR(L.face.p.z, zl, 1e-6f));
    free(s);
}

/* ---- modelo REAL del PSG1 (solo si hay datos del juego: argumento, STAGE_FILE, /tmp/gamedata/STAGE.DIR, gamedata/stages.bin; si no, se omite) ---- */
static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = (uint8_t *)malloc(l > 0 ? (size_t)l : 1);
    if (!d || fread(d, 1, (size_t)l, f) != (size_t)l) { free(d); fclose(f); return NULL; }
    fclose(f); *n = (size_t)l; return d;
}
static void test_real(const char *arg) {
    const char *cands[5] = { arg, getenv("STAGE_FILE"), "/tmp/gamedata/STAGE.DIR", "gamedata/stages.bin", "../../gamedata/stages.bin" };
    uint8_t *d = NULL; size_t n = 0;
    for (int i = 0; i < 5 && !d; i++) if (cands[i]) d = slurp(cands[i], &n);
    if (!d) { printf("test_sniper_scope: sin datos del juego: parte del modelo real omitida\n"); return; }
    const char *stage = getenv("STAGE_NAME"); if (!stage) stage = "init";
    const WCutModel *cm = wcut_find(0x2d3b);
    WLoadOpts o; wload_default_opts(&o); o.resident = stage;
    WLoaded m; WCutSet cs;
    int r = wload_model(d, n, stage, 0x2d3b, &o, &m);
    CHECK(r == 0 && cm);
    if (r || !cm) { free(d); return; }
    r = wcut_run(&m, cm, &cs);
    CHECK(r == 0);
    if (r) { wload_free(&m); free(d); return; }
    WeaponState *s = mkw();
    weapon_bind(s, cs.meshes, cs.nmeshes, cs.parts, cs.nparts);
    SniperLens L;
    CHECK(sniper_lens(s, &L) == 1 && L.model && L.rim == 1);
    printf("sniper (modelo real): abertura %.1f x %.1f mm, centro (%.1f, %.1f, %.1f) mm, placa lens en (%.1f, %.1f, %.1f) mm\n", L.w * 1e3f, L.h * 1e3f, L.face.p.x * 1e3f, L.face.p.y * 1e3f, L.face.p.z * 1e3f, L.pose.p.x * 1e3f, L.pose.p.y * 1e3f, L.pose.p.z * 1e3f);
    /* los vértices del borde trasero de la mira (plano de la cara, dentro de 6 cm del centro) deben quedar dentro de la caja de la abertura y tocar sus cuatro lados: la imagen coincide con el contorno */
    int np; const WPart *pt = weapon_parts(s, &np);
    (void)pt;
    int nm; const WMesh *ms = weapon_meshes(s, &nm);
    int bi = wcore_part_find(s, "body"), mb = bi >= 0 ? weapon_part_mesh(s, bi) : -1;
    CHECK(mb >= 0 && mb < nm);
    if (mb >= 0 && mb < nm) {
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f; int cnt = 0;
        for (uint32_t k = 0; k < ms[mb].nv; k++) {
            const float *p = ms[mb].v[k].pos;
            if (fabsf(p[2] - L.face.p.z) > 0.001f || fabsf(p[0] - L.face.p.x) > 0.05f || fabsf(p[1] - L.face.p.y) > 0.05f) continue;
            cnt++;
            if (p[0] < x0) x0 = p[0];
            if (p[0] > x1) x1 = p[0];
            if (p[1] < y0) y0 = p[1];
            if (p[1] > y1) y1 = p[1];
        }
        CHECK(cnt >= 4);
        CHECK(NEAR(x0, L.face.p.x - L.w * 0.5f, 1e-3f) && NEAR(x1, L.face.p.x + L.w * 0.5f, 1e-3f) && NEAR(y0, L.face.p.y - L.h * 0.5f, 1e-3f) && NEAR(y1, L.face.p.y + L.h * 0.5f, 1e-3f));
    }
    /* medido en el original: la cara trasera es un octógono achatado de ~44 x 44 mm (la placa "lens" son solo sus 2 triángulos inferiores) a 74 mm detrás del origen del arma, normal -Z */
    CHECK(L.round == 1 && NEAR(L.w, 0.044f, 1.5e-3f) && NEAR(L.h, 0.044f, 1.5e-3f) && NEAR(L.face.p.z, 0.074f, 1.5e-3f) && NEAR(L.face.p.x, 0.0f, 1e-3f));
    /* polígono = hexágono medido (mm, respecto al centro (0,140)): (±10,+22), (±22,+3), (±18,-22); cada vértice dentro de 1 mm */
    CHECK(L.npoly == 6);
    { const float ex[6][2] = { {10, 22}, {-10, 22}, {-22, 3}, {-18, -22}, {18, -22}, {22, 3} };
      for (int e = 0; e < 6; e++) { int ok = 0; for (int i = 0; i < L.npoly; i++) if (NEAR(L.poly[i][0] * 1e3f, ex[e][0], 1.0f) && NEAR(L.poly[i][1] * 1e3f, ex[e][1], 1.0f)) ok = 1; CHECK(ok); }
      float ar = 0; for (int i = 0; i < L.npoly; i++) { int j = (i + 1) % L.npoly; ar += L.poly[i][0] * L.poly[j][1] - L.poly[j][0] * L.poly[i][1]; }
      CHECK(ar > 0); }                                                /* antihorario */
    WVec3 a = fwd_of(L.face.q);
    CHECK(NEAR(a.z, -1, 1e-4f));
    free(s); wcut_free(&cs); wload_free(&m); free(d);
}

/* ---- visibilidad de la lente (solo decide si merece la pena el segundo render) ---- */
static void test_visible(void) {
    WPose lens = wpose_identity(), head = wpose_identity();
    lens.p = wv(0, 1.5f, 0);                                          /* -Z = eje óptico: su cara trasera mira a +Z */
    head.p = wv(0, 1.6f, 0.4f);                                       /* detrás de la lente, mirando a -Z */
    CHECK(sniper_lens_visible(&lens, &head) == 1);
    head.p = wv(0, 1.6f, 3.0f);                                       /* lejos: sigue visible (la imagen existe a cualquier distancia) */
    CHECK(sniper_lens_visible(&lens, &head) == 1);
    head.p = wv(0.6f, 1.6f, 0.4f);                                    /* en ángulo (~56 grados de la normal): se ve */
    head.q = wquat_axis_angle(wv(0, 1, 0), 0.9f);
    CHECK(sniper_lens_visible(&lens, &head) == 1);
    head.p = wv(0, 1.6f, -0.4f); head.q = wquat_identity();           /* delante de la lente: se ve la parte de delante, no la imagen */
    CHECK(sniper_lens_visible(&lens, &head) == 0);
    head.p = wv(0, 1.6f, -0.4f);
    head.q = wquat_axis_angle(wv(0, 1, 0), 3.14159265f);              /* delante de la lente y mirándola de frente (dentro del campo): sigue sin verse la imagen, es la cara de delante */
    CHECK(sniper_lens_visible(&lens, &head) == 0);
    head.p = wv(0, 1.6f, 0.4f);
    head.q = wquat_axis_angle(wv(0, 1, 0), 3.14159265f);              /* mira hacia otro lado: fuera del campo */
    CHECK(sniper_lens_visible(&lens, &head) == 0);
    head.q = wquat_axis_angle(wv(0, 1, 0), 1.2f);                     /* 69 grados de giro: aún dentro de los 80 */
    CHECK(sniper_lens_visible(&lens, &head) == 1);
    head.q = wquat_axis_angle(wv(0, 1, 0), 1.6f);                     /* 92 grados: fuera */
    CHECK(sniper_lens_visible(&lens, &head) == 0);
    head.p = lens.p;                                                  /* coincidentes: visible */
    head.q = wquat_identity();
    CHECK(sniper_lens_visible(&lens, &head) == 1);
    head.p = wv(NAN, NAN, NAN);                                       /* NaN: no */
    CHECK(sniper_lens_visible(&lens, &head) == 0);
    /* sigue la orientación de la lente: girada 90 grados su cara trasera mira a +X */
    lens.q = wquat_axis_angle(wv(0, 1, 0), 1.5707963f);
    head.p = wv(0.5f, 1.5f, 0); head.q = wquat_axis_angle(wv(0, 1, 0), 1.5707963f);
    CHECK(sniper_lens_visible(&lens, &head) == 1);
}

/* ---- zoom / FOV del original ---- */
static void test_zoom(void) {
    /* rifle.c: largo 3000; el primer impacto lo recorta; < 1000 = 320 */
    CHECK(sniper_zoom_limit(-1) == 3000.0f);
    CHECK(sniper_zoom_limit(5000) == 3000.0f);
    CHECK(sniper_zoom_limit(3000) == 3000.0f);
    CHECK(sniper_zoom_limit(2000) == 2000.0f);
    CHECK(sniper_zoom_limit(1000) == 1000.0f);
    CHECK(sniper_zoom_limit(999) == 320.0f);
    CHECK(sniper_zoom_limit(0) == 320.0f);
    /* subida H += H/3 sin pasarse; NearExp4 hacia abajo y salto a +-4 */
    CHECK(NEAR(sniper_zoom_step(320, 3000), 320 + 320 / 3.0f, 1e-3f));
    CHECK(sniper_zoom_step(2900, 3000) == 3000.0f);
    CHECK(NEAR(sniper_zoom_step(3000, 1000), 2500, 1e-3f));
    CHECK(sniper_zoom_step(1002, 1000) == 1000.0f);
    CHECK(sniper_zoom_step(1000, 1000) == 1000.0f);
    float h = 320; int n = 0;
    while (h < 3000 && n < 100) { h = sniper_zoom_step(h, 3000); n++; }
    CHECK(h == 3000.0f && n >= 7 && n <= 9);
    CHECK(NEAR(sniper_fov_y(3000), 2 * atanf(128.0f / 3000.0f), 1e-6f));     /* 4,89 grados */
    CHECK(NEAR(sniper_fov_y(320), 2 * atanf(0.4f), 1e-6f));
    /* cámara adelantada: 0,30 m, menos si el muro está cerca (range_m - 0,05), nunca detrás de la lente */
    CHECK(NEAR(sniper_cam_ahead(-1), SNIPER_CAM_AHEAD, 1e-6f) && NEAR(sniper_cam_ahead(3.0f), SNIPER_CAM_AHEAD, 1e-6f));
    CHECK(NEAR(sniper_cam_ahead(1.0f), SNIPER_CAM_AHEAD, 1e-6f) && NEAR(sniper_cam_ahead(0.35f), 0.30f, 1e-6f));
    CHECK(NEAR(sniper_cam_ahead(0.30f), 0.25f, 1e-6f) && NEAR(sniper_cam_ahead(0.12f), 0.07f, 1e-6f));
    CHECK(sniper_cam_ahead(0.05f) == 0.0f && sniper_cam_ahead(0) == 0.0f && NEAR(sniper_cam_ahead(NAN), SNIPER_CAM_AHEAD, 1e-6f));
    CHECK(NEAR(3000.0f / SNIPER_ZOOM_BASE_H, SNIPER_ZOOM_GAME, 1e-6f));

    /* en el arma: siempre activa, sin acercar el ojo; el límite viene de sniper_set_range */
    WeaponState *s = mkw(); WInput in = mk();
    WPose lw;
    CHECK(sniper_lens_world(s, &lw) == 0);                           /* aún no hubo actualización */
    step(s, &in, 2);
    CHECK(sniper_lens_world(s, &lw) == 1);
    free(s);
    /* ritmo del original: 25 ticks/s (2 VBlank PAL) y 9 ticks de espera; a 90 Hz un tick cada 3,6 cuadros: 9 ticks hechos = 33 cuadros, el 10.º en el 36 */
    s = mkw(); in = mk();
    step(s, &in, 32);
    CHECK(NEAR(sniper_zoom(s), 1.0f, 1e-6f));                         /* 8 ticks */
    step(s, &in, 3);
    CHECK(NEAR(sniper_zoom(s), 1.0f, 1e-6f));                         /* 35 cuadros = 9 ticks: aún espera */
    step(s, &in, 2);
    CHECK(NEAR(sniper_zoom(s), 1.0f + 1.0f / 3.0f, 1e-4f));           /* 37 cuadros = 10 ticks: primer H += H/3 */
    step(s, &in, 7);                                                  /* 44 cuadros = 12 ticks: dos subidas más */
    CHECK(NEAR(sniper_zoom(s), (1.0f + 1.0f / 3.0f) * (4.0f / 3.0f) * (4.0f / 3.0f), 1e-3f));
    free(s);
    s = mkw(); in = mk();
    step(s, &in, 2);
    CHECK(s->aux.active == 1 && s->channel[WCH_SCREEN] == 1.0f && (s->aux.overlay & WO_RETICLE) && s->aux.target_part == SNIPER_PT_LENS);
    CHECK(NEAR(sniper_zoom(s), 1.0f, 1e-6f));                         /* aún dentro de la espera */
    sniper_set_range(s, 2000, 1000.0f);
    step(s, &in, 120);                                               /* con el arma a la cadera y la cabeza lejos del ocular: igual */
    CHECK(NEAR(sniper_zoom(s) * SNIPER_ZOOM_BASE_H, 2000.0f, 1e-2f));
    CHECK(NEAR(s->aux.fov_y, sniper_fov_y(2000), 1e-6f));
    sniper_set_range(s, 600, 1000.0f);                                        /* pared a menos de 1000: sin zoom */
    step(s, &in, 120);
    CHECK(NEAR(sniper_zoom(s), 1.0f, 1e-3f));
    sniper_set_range(s, -1, 1000.0f);
    step(s, &in, 120);
    CHECK(sniper_zoom(s) == SNIPER_ZOOM_GAME);
    CHECK(NEAR(s->aux.fov_y, 2 * atanf(128.0f / 3000.0f), 1e-6f));
    {   /* la cámara va SNIPER_CAM_AHEAD por delante de la lente sobre su eje, y se acerca a ella si hay un muro cerca */
        SniperLens L; sniper_lens(s, &L);
        WPose lens = wpose_mul(wcore_weapon_world(s, &in), L.pose);
        WVec3 d = wv_sub(s->aux.camera.p, lens.p);
        CHECK(NEAR(wv_len(d), SNIPER_CAM_AHEAD, 1e-3f) && wv_dot(d, fwd_of(lens.q)) > 0.29f);
        sniper_set_range(s, 200, 1000.0f);
        step(s, &in, 2);
        d = wv_sub(s->aux.camera.p, lens.p);
        CHECK(NEAR(wv_len(d), 0.15f, 1e-3f));
        sniper_set_range(s, 400, 2000.0f);                       /* otra escala (2000 unidades/m): 0,2 m -> la cámara a 0,15 m; el límite del zoom sigue en unidades del motor */
        step(s, &in, 2);
        CHECK(NEAR(wv_len(wv_sub(s->aux.camera.p, lens.p)), 0.15f, 1e-3f));
        sniper_set_range(s, 30, 1000.0f);
        step(s, &in, 2);
        CHECK(wv_len(wv_sub(s->aux.camera.p, lens.p)) < 1e-3f);
        sniper_set_range(s, -1, 1000.0f);
        step(s, &in, 2);
    }
    /* la cámara no depende de la cabeza: mover la cabeza no la cambia */
    WPose c0 = s->aux.camera;
    in.head.p = wv(5, 3, -4); in.head.q = wquat_axis_angle(wv(0, 1, 0), 2.0f);
    step(s, &in, 120);
    CHECK(wv_len(wv_sub(s->aux.camera.p, c0.p)) < 1e-3f && NEAR(sniper_zoom(s), SNIPER_ZOOM_GAME, 1e-6f));
    /* A no cambia aumentos */
    in.buttons[0] = WB_A; step(s, &in, 1); in.buttons[0] = 0;
    CHECK(sniper_zoom(s) == SNIPER_ZOOM_GAME);
    free(s);
}

/* ---- el eje de la mira es el rayo del disparo ---- */
static float line_dist(WVec3 p, WVec3 o, WVec3 d) {   /* distancia del punto p a la recta o + t d (d unitario) */
    WVec3 v = wv_sub(p, o);
    return wv_len(wv_sub(v, wv_scale(d, wv_dot(v, d))));
}
static void fire_once(WeaponState *s, WInput *in) {
    int f0 = B.fired;
    in->trigger[0] = 0; step(s, in, 30);
    in->trigger[0] = 1;
    for (int i = 0; i < 90 && B.fired == f0; i++) step(s, in, 1);
    in->trigger[0] = 0;
}
static void test_fire_axis(float tilt_deg) {
    int bound = tilt_deg >= 0;
    WeaponState *s;
    if (bound) { quad(0.025f, 0.016f, tilt_deg, 0); s = mkbound(wv(0.0f, 0.130f, 0.074f)); }
    else s = mkw();
    WInput in = mk();
    sniper_set_steady(s, 1.0f); in.support_held = 1;           /* dos manos y tranquilizante: balanceo mínimo */
    step(s, &in, 3);
    /* la cabeza está lejos del ocular (arma a la cadera): la bala sale igualmente por el eje de la mira, no por la boca */
    memset(&B, 0, sizeof B);
    fire_once(s, &in);
    CHECK(B.fired == 1);
    WPose cam = s->aux.camera;                                        /* eje de la retícula: cámara de la mira */
    WVec3 cf = fwd_of(cam.q), ff = fwd_of(B.muz.q);
    float ang = acosf(wcore_clampf(wv_dot(cf, ff), -1.0f, 1.0f));
    /* a 50 m del disparo, el punto de la retícula y el de la bala coinciden (sin paralaje de la boca, 5-7 cm por debajo del eje) */
    WVec3 pf = wv_add(B.muz.p, wv_scale(ff, 50.0f));
    printf("sniper: placa %.0f grados (-1 = sin enlazar): giro disparo/retícula %.3f grados, bala a %.1f mm de la línea de visión a 50 m\n", tilt_deg, ang * 57.29578f, line_dist(pf, cam.p, cf) * 1000.0f);
    CHECK(ang < 0.0035f);                                             /* < 0,2 grados: solo difiere el retroceso entre disparo y cámara */
    CHECK(line_dist(pf, cam.p, cf) < 0.02f);
    CHECK(line_dist(B.muz.p, cam.p, cf) < 0.01f);
    /* con el cañón (boca) la bala habría pasado por debajo de la retícula */
    WPose mz = wpose_mul(wcore_weapon_world(s, &in), w_sniper.points[WP_MUZZLE]);
    CHECK(line_dist(wv_add(mz.p, wv_scale(cf, 50.0f)), cam.p, cf) > 0.04f);
    /* acercar la cabeza al ocular no cambia nada del tiro */
    SniperLens L; sniper_lens(s, &L);
    WPose lens = wpose_mul(wcore_weapon_world(s, &in), L.pose);
    in.head.p = wv_add(lens.p, wquat_rotate(lens.q, wv(0, 0, 0.08f)));
    memset(&B, 0, sizeof B);
    fire_once(s, &in);
    CHECK(B.fired == 1);
    cam = s->aux.camera; cf = fwd_of(cam.q); ff = fwd_of(B.muz.q);
    CHECK(wv_dot(cf, ff) > 0.99999f && line_dist(B.muz.p, cam.p, cf) < 0.01f);
    free(s);
}

int main(int argc, char **argv) {
    test_lens_model();
    test_rim();
    test_real(argc > 1 ? argv[1] : NULL);

    test_visible();
    test_zoom();
    test_fire_axis(-1.0f);     /* sin enlazar */
    test_fire_axis(0.0f);      /* con modelo: placa recta */
    test_fire_axis(5.0f);      /* con modelo: placa inclinada, el eje la sigue */
    printf(fails ? "RESULTADO: %d FALLOS\n" : "RESULTADO: todo OK\n", fails);
    return fails != 0;
}
