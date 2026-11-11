/* Pruebas de la cámara VR sobre una RAM sintética (no necesitan datos del juego). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "vr_camera.h"
#include "vr_usermap.h"
#include "vr_scene.h"
#ifndef EXE_MAGIC
/* Firma de 8 bytes de la cabecera del formato de ejecutable (construida en hex; se valida con memcmp). */
#define EXE_MAGIC "\x50\x53\x2d\x58\x20\x45\x58\x45"
#endif

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FALLA %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, e) CHECK(fabs((double)(a) - (double)(b)) <= (e))

static uint8_t ram[BRIDGE_RAM_SIZE];
static void w16(uint32_t a, int16_t v) { memcpy(ram + (a & 0x1FFFFF), &v, 2); }
static void w32(uint32_t a, uint32_t v) { memcpy(ram + (a & 0x1FFFFF), &v, 4); }
static int16_t r16(uint32_t a) { int16_t v; memcpy(&v, ram + (a & 0x1FFFFF), 2); return v; }

static float G_yaw, G_elev;      /* grados */
static int G_ok = 1;
static int head(float m[16], double *t) {
    if (!G_ok) return 0;
    float y = G_yaw * 3.14159265f / 180, e = G_elev * 3.14159265f / 180, cy = cosf(y), sy = sinf(y), ce = cosf(e), se = sinf(e);
    float X[3] = {cy, 0, -sy}, Y[3] = {sy * se, ce, cy * se}, Z[3] = {sy * ce, -se, cy * ce};
    for (int i = 0; i < 3; i++) { m[i] = X[i]; m[4 + i] = Y[i]; m[8 + i] = Z[i]; }
    m[3] = m[7] = m[11] = 0; m[12] = 0.1f; m[13] = 1.6f; m[14] = -0.2f; m[15] = 1;
    *t = 0;
    return 1;
}

#define CTL   0x80100000u
#define BODY  0x80110000u
#define OBJS  0x80120000u
#define PART6 (OBJS + 0x48 + 6 * 0x5C)

static void build_world(int yaw_body) {
    memset(ram, 0, sizeof ram);
    w32(BA_PLAYER_CTRL, CTL);
    w16(CTL + BC_MOV, 1000); w16(CTL + BC_MOV + 2, 1029); w16(CTL + BC_MOV + 4, -500);
    w16(CTL + BC_ROT + 2, (int16_t)yaw_body);
    w32(BA_PLAYER_BODY, BODY);
    w32(BODY, OBJS);
    w16(OBJS + 0x2E, 16);
    w32(PART6 + 0x14, 1010 & 0xFFFF); w32(PART6 + 0x18, 1497); w32(PART6 + 0x1C, (uint32_t)-480);
    w16(PART6 + 0x4C, 2);
}
static BridgeState ok_state(void) {
    BridgeState s; memset(&s, 0, sizeof s);
    s.valid = BRIDGE_OK_ALL_CORE; s.has_player = 1; s.in_control = 1; s.cam_ctl = 1;
    return s;
}

static void test_angles(void) {
    float y, e, h[16], t;
    G_yaw = G_elev = 0; double dt; head(h, &dt); vrcam_head_angles(h, &y, &e);
    NEAR(y, 0, 1e-3); NEAR(e, 0, 1e-3);
    for (int yy = -170; yy <= 170; yy += 40) for (int ee = -70; ee <= 70; ee += 35) {
        G_yaw = (float)yy; G_elev = (float)ee; head(h, &dt); vrcam_head_angles(h, &y, &e);
        NEAR(y, yy, 0.05); NEAR(e, ee, 0.05);
    }
    /* casi vertical: la guiñada se recupera por el eje "arriba" de la cabeza */
    for (int yy = -150; yy <= 150; yy += 50) {
        G_yaw = (float)yy; G_elev = 88; head(h, &dt); vrcam_head_angles(h, &y, &e); NEAR(e, 88, 0.05); NEAR(y, yy, 0.6);
        G_elev = -88; head(h, &dt); vrcam_head_angles(h, &y, &e); NEAR(e, -88, 0.05); NEAR(y, yy, 0.6);
    }
    (void)t;
    float d[3];
    vrcam_dir(0, 0, d); NEAR(d[0], 0, 1e-6); NEAR(d[1], 0, 1e-6); NEAR(d[2], 1, 1e-6);
    vrcam_dir(90, 0, d); NEAR(d[0], 1, 1e-6); NEAR(d[2], 0, 1e-5);
    vrcam_dir(180, 0, d); NEAR(d[2], -1, 1e-6);
    vrcam_dir(0, 90, d); NEAR(d[1], -1, 1e-6);   /* pitch + = mirar abajo (Y arriba) */
    vrcam_dir(0, -30, d); NEAR(d[1], 0.5, 1e-6);
}

static void test_hide_head(void) {
    build_world(0);
    CHECK(r16(PART6 + 0x4C) == 2);
    CHECK(vrcam_hide_head(ram) == 1);
    CHECK(r16(PART6 + 0x4C) == 0);
    w16(OBJS + 0x2E, 3);                       /* cuerpo sin cabeza (menos de 7 partes): no toca nada */
    w16(PART6 + 0x4C, 2);
    CHECK(vrcam_hide_head(ram) == 0 && r16(PART6 + 0x4C) == 2);
    w32(BA_PLAYER_BODY, 0x12345);              /* puntero basura */
    CHECK(vrcam_hide_head(ram) == 0);
    w32(BA_PLAYER_BODY, 0x80FFFFFC);           /* al final de la RAM */
    CHECK(vrcam_hide_head(ram) == 0);
}

/* Visor activo (prismáticos / mira: actores scope.c 0x80063D54 o sight.c 0x800726BC en las listas de actores): la cabeza se oculta aunque la cámara VR no esté activa (in_control = 0). */
static void actor_list(int lv, const uint32_t *nodes, const uint32_t *fns, int n) {
    uint32_t base = BA_ACTOR_LISTS + 68u * (uint32_t)lv, endn = base + 32u;
    w32(base + 4, n ? nodes[0] : endn);
    for (int i = 0; i < n; i++) { w32(nodes[i] + 4, i + 1 < n ? nodes[i + 1] : endn); w32(nodes[i] + 8, fns[i]); }
}
static void test_viewer(void) {
    VrCamConfig c; vrcam_config_default(&c); c.head = head;
    vrcam_init(&c); build_world(0);            /* sin plan: vrcam_active() == 0 */
    CHECK(!vrcam_active() && !vrcam_viewer_active(ram) && !vrcam_hide_head_now(ram));
    uint32_t nodes[2] = {0x80170000u, 0x80170100u}, f_other[2] = {0x80050000u, 0x80060000u}, f_scope[2] = {0x80050000u, 0x80063D54u}, f_sight[2] = {0x800726BCu, 0x80050000u};
    actor_list(7, nodes, f_other, 2);
    CHECK(!vrcam_viewer_active(ram) && !vrcam_hide_head_now(ram));
    actor_list(7, nodes, f_scope, 2);
    CHECK(vrcam_viewer_active(ram) && vrcam_hide_head_now(ram));
    actor_list(7, nodes, f_sight, 2);
    CHECK(vrcam_viewer_active(ram) && vrcam_hide_head_now(ram));
    actor_list(7, nodes, f_scope, 0);          /* lista vacía */
    CHECK(!vrcam_viewer_active(ram));
    actor_list(7, nodes, f_scope, 2); w32(nodes[0] + 4, nodes[0]);   /* nodo que se apunta a sí mismo: acotado, sin colgarse */
    CHECK(!vrcam_viewer_active(ram));
    actor_list(7, nodes, f_scope, 2);
    VrCamConfig c2 = c; c2.hide_head = 0; vrcam_init(&c2);
    CHECK(vrcam_viewer_active(ram) && !vrcam_hide_head_now(ram));     /* hide_head = 0 manda */
    vrcam_init(&c);
    CHECK(!vrcam_viewer_active(NULL));
    /* RAM real de volcados (si existen; VRCAM_DUMPS = carpeta): d0006 = prismáticos con in_control = 0 */
    const char *root = getenv("VRCAM_DUMPS"); if (!root) root = "/home/claude/work/adj/Dumps";
    static const struct { const char *dir; int want; } D[] = {
        {"d0006_20261031-213831_stage", 1}, {"d0006_20261031-213831_frame", 1}, {"d0001_20261031-213254_stage", 0}, {"d0002_20261031-213553_stage", 0}, {"d0003_20261031-213615_stage", 0},
        {"d0004_20261031-213738_stage", 0}, {"d0005_20261031-213826_stage", 0}, {"d0007_20261031-214006_stage", 0}};
    static uint8_t real[BRIDGE_RAM_SIZE];
    int loaded = 0;
    for (unsigned i = 0; i < sizeof D / sizeof D[0]; i++) {
        char path[512]; snprintf(path, sizeof path, "%s/%s/ram.bin", root, D[i].dir);
        FILE *f = fopen(path, "rb"); if (!f) continue;
        size_t n = fread(real, 1, sizeof real, f); fclose(f);
        if (n != sizeof real) continue;
        loaded++;
        vrcam_init(&c);                        /* sin plan: cámara VR inactiva (como in_control = 0) */
        CHECK(!vrcam_active());
        CHECK(vrcam_viewer_active(real) == D[i].want);
        CHECK(vrcam_hide_head_now(real) == D[i].want);
    }
    if (!loaded) printf("(volcados reales no encontrados en %s: solo prueba sintética)\n", root);
}

static void test_activation(void) {
    VrCamConfig c; vrcam_config_default(&c); c.head = head;
    vrcam_init(&c); build_world(1024);
    BridgeState s = ok_state();
    VrCamPlan p;
    G_ok = 1; G_yaw = 0; G_elev = 0;
    CHECK(vrcam_plan(ram, 320, &p) == 0);      /* aún no activa */
    for (int i = 0; i < c.on_frames - 1; i++) vrcam_frame(ram, &s);
    CHECK(vrcam_plan(ram, 320, &p) == 0 && !vrcam_active());
    vrcam_frame(ram, &s);
    CHECK(vrcam_plan(ram, 320, &p) == 1 && vrcam_active());
    /* al activar, la mirada coincide con la guiñada del cuerpo (1024) y el ojo está en la cabeza + desplazamiento */
    NEAR(p.yaw_deg, 90, 0.05); NEAR(p.pitch_deg, 0, 0.05);
    NEAR(p.from[1], 1497 + c.eye_up, 1.0);
    /* objetivo: dirección (sin, 0, cos) = +X */
    float dx = p.to[0] - p.from[0], dz = p.to[2] - p.from[2];
    CHECK(dx > 1000 && fabsf(dz) < 3);
    CHECK(p.screen == 160 && vrcam_config()->engine_h == 160);   /* H por defecto del motor */
    CHECK(vrcam_pad_origin() == ((1024 + 2048) & 4095));
    /* giro de cabeza 90 grados a la izquierda: la mirada pasa a 180 */
    G_yaw = 90; vrcam_plan(ram, 320, &p); NEAR(p.yaw_deg, 180, 0.05);
    CHECK(vrcam_pad_origin() == ((2048 + 2048) & 4095));
    /* elevación: cabeza mirando arriba 30 grados -> pitch del motor -30 (mirar abajo es positivo) */
    G_yaw = 0; G_elev = 30; vrcam_plan(ram, 320, &p); NEAR(p.pitch_deg, -30, 0.05); CHECK(p.to[1] > p.from[1]);
    /* el tope de elevación recorta lo que se da al motor */
    G_elev = 88; vrcam_plan(ram, 320, &p); NEAR(p.pitch_deg, -c.pitch_limit_deg, 1e-3);
    /* sin pose (seguimiento perdido): cámara del juego */
    G_ok = 0; CHECK(vrcam_plan(ram, 320, &p) == 0 && !vrcam_active()); G_ok = 1; G_elev = 0;
    /* recentrar: la mirada vuelve a la guiñada del cuerpo con la cabeza donde esté */
    G_yaw = 70; vrcam_recenter(); vrcam_plan(ram, 320, &p); NEAR(p.yaw_deg, 90, 0.05);
    G_yaw = 80; vrcam_plan(ram, 320, &p); NEAR(p.yaw_deg, 100, 0.05);
    /* giro por pasos del espacio del usuario */
    vrcam_turn(30); vrcam_plan(ram, 320, &p); NEAR(p.yaw_deg, 130, 0.05);
    /* ventana de armas/objetos o pausa abiertas (in_control=0, cam_ctl=1): la cabeza sigue llevando la cámara */
    s.in_control = 0; s.cam_ctl = 1; vrcam_frame(ram, &s);
    CHECK(vrcam_plan(ram, 320, &p) != 0 && vrcam_active());
    /* radio recién abierta: la cámara aguanta VRCAM_COMMS_GRACE fotogramas y luego se apaga; sin radio no hay gracia */
    { BridgeState cs = s; cs.in_control = 0; cs.cam_ctl = 0; cs.comms = 1; cs.stopped = 1;
      for (int i = 0; i < VRCAM_COMMS_GRACE; i++) { vrcam_frame(ram, &cs); CHECK(vrcam_plan(ram, 320, &p) != 0 && vrcam_active()); }
      vrcam_frame(ram, &cs); CHECK(vrcam_plan(ram, 320, &p) == 0 && !vrcam_active());
      cs.comms = 0; cs.in_control = 1; cs.cam_ctl = 1; for (int i = 0; i < c.on_frames + 1; i++) vrcam_frame(ram, &cs);
      CHECK(vrcam_plan(ram, 320, &p) != 0); }
    /* salida inmediata al perder el control */
    s.in_control = 0; s.cam_ctl = 0; vrcam_frame(ram, &s);
    CHECK(vrcam_plan(ram, 320, &p) == 0 && !vrcam_active());
    s.in_control = 1; s.cam_ctl = 1; s.has_player = 0;
    for (int i = 0; i < 20; i++) vrcam_frame(ram, &s);
    CHECK(vrcam_plan(ram, 320, &p) == 0);
    s.has_player = 1; s.valid &= ~1u;           /* un grupo del puente sin validar */
    for (int i = 0; i < 20; i++) vrcam_frame(ram, &s);
    CHECK(vrcam_plan(ram, 320, &p) == 0);
    VrCamStats st; vrcam_stats(&st);
    CHECK(st.activations == 2 && st.deactivations >= 1);
    /* hasta que no pasan on_frames cuadros buenos otra vez, no vuelve */
    s.valid = BRIDGE_OK_ALL_CORE;
    for (int i = 0; i < c.on_frames - 1; i++) vrcam_frame(ram, &s);
    CHECK(vrcam_plan(ram, 320, &p) == 0);
    vrcam_frame(ram, &s);
    CHECK(vrcam_plan(ram, 320, &p) == 1);
}

static void test_engine_h(void) {
    VrCamConfig c; vrcam_config_default(&c); c.head = head; c.engine_h = 160;
    vrcam_init(&c); build_world(0);
    BridgeState s = ok_state(); VrCamPlan p;
    for (int i = 0; i < c.on_frames; i++) vrcam_frame(ram, &s);
    CHECK(vrcam_plan(ram, 320, &p) == 1 && p.screen == 160);
    CHECK(vrcam_plan(ram, 640, &p) == 1 && p.screen == 320);     /* un zoom del juego se escala igual */
}

/* La matriz del motor para una mirada (yaw, pitch) de motor (columnas: X derecha, Y abajo, Z delante; mundo con Y arriba, como el
 * que se midió en la RAM: mirando a -Z, la derecha es +X). */
static void engine_matrix(float yaw, float pitch, float M[9]) {
    float d[3]; vrcam_dir(yaw, pitch, d);
    float y = yaw * 3.14159265f / 180;
    float X[3] = {-cosf(y), 0, sinf(y)};
    float Z[3] = {d[0], d[1], d[2]};
    float Y[3] = {Z[1] * X[2] - Z[2] * X[1], Z[2] * X[0] - Z[0] * X[2], Z[0] * X[1] - Z[1] * X[0]};   /* Z x X */
    for (int i = 0; i < 3; i++) { M[3 * i] = X[i]; M[3 * i + 1] = Y[i]; M[3 * i + 2] = Z[i]; }
}

static void test_delta(void) {
    VrCamConfig c; vrcam_config_default(&c); c.head = head;
    vrcam_init(&c);
    /* registro sintético: motor mirando (yaw 90, pitch -20); cabeza con la misma orientación relativa */
    VrCamRecord r; memset(&r, 0, sizeof r);
    r.active = 1; r.units_per_m = 1000;
    engine_matrix(90, -20, r.M);
    G_yaw = 25; G_elev = 20;                                     /* cabeza: yaw 25, elev +20 -> pitch motor -20 */
    double t; head(r.head, &t);
    r.theta_play_deg = 90 - 25; r.yaw_deg = 90; r.pitch_deg = -20;
    memcpy(r.p_ref, (float[3]){r.head[12], r.head[13], r.head[14]}, sizeof r.p_ref);
    VrCamDelta d;
    CHECK(vrcam_delta(&r, r.head, &d));
    /* misma pose: identidad */
    for (int i = 0; i < 9; i++) NEAR(d.A[i], (i % 4 == 0) ? 1 : 0, 2e-3);
    for (int i = 0; i < 3; i++) NEAR(d.b[i], 0, 1e-3);
    /* un punto delante del ojo (en vista del motor) sigue delante */
    float v[3] = {100, -50, 2000}, o[3];
    for (int i = 0; i < 3; i++) o[i] = d.A[3 * i] * v[0] + d.A[3 * i + 1] * v[1] + d.A[3 * i + 2] * v[2] + d.b[i];
    NEAR(o[0], 100, 5); NEAR(o[1], -50, 5); NEAR(o[2], 2000, 5);
    /* mirada horizontal: giro de cabeza de +10 grados a la izquierda: un punto en el eje óptico pasa a verse a la derecha */
    engine_matrix(90, 0, r.M); r.pitch_deg = 0; G_elev = 0; G_yaw = 25; head(r.head, &t);
    memcpy(r.p_ref, (float[3]){r.head[12], r.head[13], r.head[14]}, sizeof r.p_ref);
    G_yaw = 35; float h2[16]; head(h2, &t);
    CHECK(vrcam_delta(&r, h2, &d));
    float p0[3] = {0, 0, 2000};
    for (int i = 0; i < 3; i++) o[i] = d.A[3 * i] * p0[0] + d.A[3 * i + 1] * p0[1] + d.A[3 * i + 2] * p0[2] + d.b[i];
    CHECK(o[0] > 0);
    NEAR(atan2f(o[0], o[2]) * 180 / 3.14159265f, 10, 0.5);
    /* traslación de la cabeza 0,1 m a la derecha (misma orientación): el mundo se desplaza -100 uds en x de la vista */
    G_yaw = 25; head(h2, &t); h2[12] += 0.1f * cosf(25 * 3.14159265f / 180); h2[14] += 0.1f * (-sinf(25 * 3.14159265f / 180));
    CHECK(vrcam_delta(&r, h2, &d));
    for (int i = 0; i < 3; i++) o[i] = d.A[3 * i] * p0[0] + d.A[3 * i + 1] * p0[1] + d.A[3 * i + 2] * p0[2] + d.b[i];
    NEAR(o[0], -100, 3); NEAR(o[1], 0, 3); NEAR(o[2], 2000, 3);
    /* registro inactivo: identidad y devuelve 0 */
    r.active = 0; CHECK(vrcam_delta(&r, h2, &d) == 0);
    NEAR(d.A[0], 1, 1e-6); NEAR(d.b[0], 0, 1e-6);
    CHECK(vrcam_delta(NULL, h2, &d) == 0);
}

/* Extremo a extremo con la matriz que el motor construiría (independiente de vrcam_delta): una dirección fija del mundo, vista con
 * la matriz del motor (limitada a +-pitch_limit), debe quedar tras vrcam_delta exactamente en el eje óptico de la cabeza real,
 * también con la cabeza casi vertical (donde el motor no puede seguirla: lo absorbe el delta). Y A debe ser una rotación. */
static void test_e2e(void) {
    VrCamConfig c; vrcam_config_default(&c); c.head = head; vrcam_init(&c);
    BridgeState s = ok_state(); VrCamPlan p;
    const float ys[] = {0, 33, -120, 170}, es[] = {0, 20, -45, 88, -88, 75, -73};
    for (unsigned iy = 0; iy < 4; iy++) for (unsigned ie = 0; ie < 7; ie++) {
        vrcam_init(&c); build_world(512);
        for (int i = 0; i < c.on_frames; i++) vrcam_frame(ram, &s);
        G_ok = 1; G_yaw = ys[iy]; G_elev = es[ie];
        CHECK(vrcam_plan(ram, 320, &p) == 1);
        float lim = c.pitch_limit_deg, pe = p.pitch_deg;
        CHECK(fabsf(pe) <= lim + 1e-3f);
        if (fabsf(es[ie]) <= lim) NEAR(pe, -es[ie], 0.01);
        float M[9]; engine_matrix(p.yaw_deg, p.pitch_deg, M);
        uint32_t eye = BA_VIEW_CHANNEL + BCH_CAM;
        for (int k = 0; k < 9; k++) w16(eye + 2 * k, (int16_t)lrintf(M[k] * 4096.0f));
        vrcam_commit(ram, &p);
        const VrCamRecord *r = vrcam_last_record();
        float dtrue[3]; vrcam_dir(p.yaw_deg, -es[ie], dtrue);          /* la mirada real de la cabeza, sin limitar, en el mundo del motor */
        float V[3];
        for (int i = 0; i < 3; i++) V[i] = r->M[i] * dtrue[0] + r->M[3 + i] * dtrue[1] + r->M[6 + i] * dtrue[2];   /* M^T d */
        VrCamDelta d; float hn[16]; double t; head(hn, &t);
        CHECK(vrcam_delta(r, hn, &d));
        float o[3];
        for (int i = 0; i < 3; i++) o[i] = d.A[3 * i] * V[0] + d.A[3 * i + 1] * V[1] + d.A[3 * i + 2] * V[2] + d.b[i];
        NEAR(o[0], 0, 4e-3); NEAR(o[1], 0, 4e-3); NEAR(o[2], 1, 4e-3);
        /* A ortogonal con det +1 */
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
            float dot = d.A[3 * i] * d.A[3 * j] + d.A[3 * i + 1] * d.A[3 * j + 1] + d.A[3 * i + 2] * d.A[3 * j + 2];
            NEAR(dot, i == j ? 1 : 0, 2e-3);
        }
        float det = d.A[0] * (d.A[4] * d.A[8] - d.A[5] * d.A[7]) - d.A[1] * (d.A[3] * d.A[8] - d.A[5] * d.A[6]) + d.A[2] * (d.A[3] * d.A[7] - d.A[4] * d.A[6]);
        NEAR(det, 1, 2e-3);
    }
}

static void test_records(void) {
    VrCamConfig c; vrcam_config_default(&c); c.head = head; vrcam_init(&c); build_world(0);
    BridgeState s = ok_state(); VrCamPlan p;
    for (int i = 0; i < c.on_frames; i++) vrcam_frame(ram, &s);
    /* matriz "eye" de la RAM: columnas X, Y, Z, t */
    uint32_t eye = BA_VIEW_CHANNEL + BCH_CAM;
    w16(eye + 0, 4096); w16(eye + 8, 4096); w16(eye + 16, 4096); w16(BA_VIEW_CHANNEL + BCH_SCREEN, 320);
    w32(eye + 0x14, 1234); w32(eye + 0x18, 5); w32(eye + 0x1C, (uint32_t)-77);
    CHECK(vrcam_last_record() == NULL);
    CHECK(vrcam_plan(ram, 320, &p) == 1);
    CHECK(vrcam_turn_total() == 0.0);
    vrcam_turn(12.5f); vrcam_turn(-2.5f); vrcam_turn(NAN);
    NEAR(vrcam_turn_total(), 10.0, 1e-9);   /* fluidez: suma de los giros del stick, sin envolver */
    vrcam_commit(ram, &p);
    const VrCamRecord *r = vrcam_last_record();
    CHECK(r && r->serial == 1 && r->active && r->eye[0] == 1234 && r->eye[2] == -77 && r->screen == 320);
    VrCamStats cst; vrcam_stats(&cst);
    CHECK(r && r->turn_total_deg == 10.0 && r->vbl == (uint32_t)c.on_frames && r->epoch == cst.recenters);
    NEAR(r->M[0], 1, 1e-6); NEAR(r->M[4], 1, 1e-6); NEAR(r->M[8], 1, 1e-6);
    /* envíos: rango de seq -> registro vigente */
    vrcam_note_draw(100, 200);
    CHECK(vrcam_seq_record(150) == r && vrcam_seq_record(100) == r && vrcam_seq_record(199) == r);
    CHECK(vrcam_seq_record(200) == NULL && vrcam_seq_record(99) == NULL);
    /* nueva cámara (la del juego): el siguiente envío se asocia a ella */
    vrcam_commit(ram, NULL);
    vrcam_note_draw(200, 300);
    const VrCamRecord *r2 = vrcam_seq_record(250);
    CHECK(r2 && r2->serial == 2 && !r2->active);
    CHECK(vrcam_seq_record(150) == r);
    CHECK(vrcam_draw_count() == 2);
    /* desfase de un registro */
    VrCamConfig c2 = c; c2.draw_lag = 1; vrcam_init(&c2);
    vrcam_commit(ram, NULL); vrcam_commit(ram, NULL);
    vrcam_note_draw(0, 10);
    const VrCamRecord *rl = vrcam_seq_record(5);
    CHECK(rl && rl->serial == 1);
    /* envuelto del contador de seq */
    vrcam_note_draw(0xFFFFFFF0u, 0x10u);
    CHECK(vrcam_seq_record(0xFFFFFFF8u) != NULL && vrcam_seq_record(4) != NULL && vrcam_seq_record(0x20) == NULL);
    /* el anillo no se desborda con muchos envíos */
    for (int i = 0; i < 500; i++) { vrcam_commit(ram, NULL); vrcam_note_draw(1000 + 10 * i, 1010 + 10 * i); }
    CHECK(vrcam_seq_record(1000 + 10 * 499 + 3) != NULL && vrcam_seq_record(1005) == NULL);
}

static void test_robust(void) {
    VrCamConfig c; vrcam_config_default(&c); vrcam_init(&c);   /* sin cabeza: nunca activa */
    BridgeState s = ok_state(); VrCamPlan p;
    build_world(0);
    for (int i = 0; i < 20; i++) vrcam_frame(ram, &s);
    CHECK(vrcam_plan(ram, 320, &p) == 0);
    c.head = head; vrcam_init(&c);
    for (int i = 0; i < 20; i++) vrcam_frame(ram, &s);
    /* bloque de control con puntero inválido */
    w32(BA_PLAYER_CTRL, 0xDEADBEEF);
    CHECK(vrcam_plan(ram, 320, &p) == 0);
    w32(BA_PLAYER_CTRL, 0x801FFFF0u);                          /* demasiado cerca del final */
    CHECK(vrcam_plan(ram, 320, &p) == 0);
    /* pose degenerada */
    build_world(0);
    G_ok = 1;
    float save = G_yaw; G_yaw = NAN; VrCamPlan q;
    CHECK(vrcam_plan(ram, 320, &q) == 0);
    G_yaw = save;
    vrcam_commit(ram, NULL); vrcam_commit(NULL == NULL ? ram : ram, NULL);
    CHECK(vrcam_seq_record(0) == NULL);
}

static int load_exe(const char *path, uint8_t *img, uint32_t *base, uint32_t *size) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    static uint8_t buf[BRIDGE_RAM_SIZE + 0x800];
    size_t n = fread(buf, 1, sizeof buf, f); fclose(f);
    if (n < 0x800 || memcmp(buf, EXE_MAGIC, 8)) return 0;
    memcpy(base, buf + 0x18, 4); memcpy(size, buf + 0x1C, 4);
    if (*size > n - 0x800 || (*base & 0x1FFFFF) + *size > BRIDGE_RAM_SIZE) return 0;
    memcpy(img + (*base & 0x1FFFFF), buf + 0x800, *size);
    return 1;
}

/* Parche en RAM: sobre RAM vacía (otra edición) no escribe nada; con el EXE real (VRCAM_EXE) y el parcheado por
 * patch_exe.py (VRCAM_EXE_PATCHED) deben coincidir byte a byte. */
static void test_patch(void) {
    static uint8_t a[BRIDGE_RAM_SIZE], b[BRIDGE_RAM_SIZE];
    VrCamPatch p; vrcam_patch_default(&p);
    CHECK(vrcam_patch_ram(a, &p) == -1);
    { size_t nz = 0; for (size_t i = 0; i < sizeof a; i++) nz += a[i] != 0; CHECK(nz == 0); }   /* no escribió nada */
    CHECK(vrcam_patch_ram(NULL, &p) == -1);
    const char *eo = getenv("VRCAM_EXE"), *ep = getenv("VRCAM_EXE_PATCHED");
    if (!eo || !ep) { printf("(sin VRCAM_EXE/VRCAM_EXE_PATCHED: se omite la comparación con el EXE real)\n"); return; }
    uint32_t base, size, base2, size2;
    memset(a, 0, sizeof a); memset(b, 0, sizeof b);
    CHECK(load_exe(eo, a, &base, &size) && load_exe(ep, b, &base2, &size2) && base == base2 && size == size2);
    CHECK(vrcam_patch_ram(a, &p) == 51);       /* 44 inmediatos de recorte + subdivisión + 6 de sin-recorte; "punto visible" intacto */
    CHECK(memcmp(a + (base & 0x1FFFFF), b + (base & 0x1FFFFF), size) == 0);   /* coincide byte a byte con patch_exe.py */
    /* "punto visible" y subdivisión realmente intactos sin sus opciones */
    { uint32_t w; memcpy(&w, a + (0x8001c440u & 0x1FFFFF), 4); CHECK((int16_t)(w & 0xFFFF) == 192);
      memcpy(&w, a + (0x8001c5e8u & 0x1FFFFF), 4); CHECK((int16_t)(w & 0xFFFF) == 192); }
    const char *ew = getenv("VRCAM_EXE_PATCHED_WIDE");
    if (ew) {
        static uint8_t cw[BRIDGE_RAM_SIZE]; uint32_t b3, s3; VrCamPatch pw = p; pw.wide_points = 1;
        memset(cw, 0, sizeof cw); memset(b, 0, sizeof b);
        CHECK(load_exe(eo, cw, &b3, &s3) && load_exe(ew, b, &base2, &size2) && vrcam_patch_ram(cw, &pw) == 63);
        CHECK(memcmp(cw + (b3 & 0x1FFFFF), b + (base2 & 0x1FFFFF), s3) == 0);
    }
    /* doble aplicación: ya no coincide con lo esperado */
    CHECK(vrcam_patch_ram(a, &p) == -1);
    /* todo o nada: un solo sitio distinto (otra edición del EXE) -> -1 y la RAM queda idéntica, sea cual sea el sitio */
    {
        static const uint32_t bad_sites[] = {0x80018b40u, 0x8001e4b8u, 0x8001e6f4u, 0x8001c604u, 0x8001A11Cu, 0x80018D00u, 0x8001E29Cu};
        for (unsigned k = 0; k < sizeof bad_sites / sizeof *bad_sites; k++) {
            memset(a, 0, sizeof a); load_exe(eo, a, &base, &size);
            uint32_t w; memcpy(&w, a + (bad_sites[k] & 0x1FFFFF), 4); w ^= 1u; memcpy(a + (bad_sites[k] & 0x1FFFFF), &w, 4);
            memcpy(b, a, sizeof b);
            VrCamPatch pa = p; pa.wide_points = 1;
            CHECK(vrcam_patch_ram(a, &pa) == -1);
            CHECK(memcmp(a, b, sizeof a) == 0);
        }
        /* caja de otra edición (+-112 en vez de +-160): rechazado */
        memset(a, 0, sizeof a); load_exe(eo, a, &base, &size);
        uint32_t w; memcpy(&w, a + (0x80018b40u & 0x1FFFFF), 4); w = (w & 0xFFFF0000u) | 113u; memcpy(a + (0x80018b40u & 0x1FFFFF), &w, 4);
        CHECK(vrcam_patch_ram(a, &p) == -1);
        /* parámetros no finitos o enormes: rechazados / sin desbordar */
        memset(a, 0, sizeof a); load_exe(eo, a, &base, &size);
        VrCamPatch pn = p; pn.hfov_deg = NAN; CHECK(vrcam_patch_ram(a, &pn) == -1);
        pn = p; pn.vfov_deg = 180; pn.engine_h = 2000000000; CHECK(vrcam_patch_ram(a, &pn) == 51);   /* caja limitada a 1020 */
        { uint32_t w2; memcpy(&w2, a + (0x80018b54u & 0x1FFFFF), 4); CHECK((int16_t)(w2 & 0xFFFF) == 1021); }
    }
    memset(a, 0, sizeof a); load_exe(eo, a, &base, &size);
    /* sin subdivisión y con otra caja */
    memset(a, 0, sizeof a); load_exe(eo, a, &base, &size);
    p.no_divide = 0; p.engine_h = 320; p.hfov_deg = 110; p.vfov_deg = 90;
    CHECK(vrcam_patch_ram(a, &p) == 50);   /* 44 inmediatos + 6 sitios sin recorte */
    memset(a, 0, sizeof a); load_exe(eo, a, &base, &size);
    p.no_cull = 0; CHECK(vrcam_patch_ram(a, &p) == 44); p.no_cull = VRCAM_NOCULL_ALL;
    memset(a, 0, sizeof a); load_exe(eo, a, &base, &size); CHECK(vrcam_patch_ram(a, &p) == 50);
    CHECK(vrcam_patch_ram(a, &p) == -1);
    p.hfov_deg = 10; memset(a, 0, sizeof a); load_exe(eo, a, &base, &size); CHECK(vrcam_patch_ram(a, &p) == -1);   /* parámetros fuera de rango */
}

/* ===== §49: el mundo de vr_scene en el espacio del usuario coincide con vrcam_delta =====
 * Un punto del mundo del motor visto con la cámara del motor (V = M^T (W - E)) se lleva a la vista de la cabeza de presentación de dos maneras:
 *   (a) vrcam_delta (A V + b, mm, X der, Y abajo, Z delante);
 *   (b) vr_scene con opts.user_map_on: posición en el espacio del usuario (m) y luego la cabeza (derecha, arriba, atrás).
 * Deben coincidir para cualquier rumbo del cuerpo (theta), mirada del motor, giro de la cabeza y traslación física. Sin user_map_on (comportamiento
 * anterior) solo coinciden cuando theta + 180 grados es múltiplo de 360. */
static void test_predict(void) {
    /* predicción de mirada: el motor mira predict_s por delante de la cabeza (acotada) y el registro conserva la mirada real para el espacio del usuario */
    for (int mode = 0; mode < 3; mode++) {
        VrCamConfig c; vrcam_config_default(&c); c.head = head;
        if (mode == 0) c.predict_s = 0.0f;
        vrcam_init(&c); build_world(1024);
        BridgeState s = ok_state();
        VrCamPlan p;
        G_ok = 1; G_yaw = 0; G_elev = 0;
        for (int i = 0; i < c.on_frames; i++) vrcam_frame(ram, &s);
        CHECK(vrcam_plan(ram, 320, &p) == 1);
        float rate = mode == 2 ? 40.0f : 1.0f;   /* grados por VBlank (a 50 Hz) */
        for (int i = 0; i < 24; i++) { G_yaw += rate; vrcam_frame(ram, &s); vrcam_plan(ram, 320, &p); }
        float real = 90.0f + G_yaw;               /* 1024 = 90 grados de cuerpo */
        float dy = p.yaw_deg - real; while (dy > 180) dy -= 360; while (dy < -180) dy += 360;
        if (mode == 0) NEAR(dy, 0, 0.05);
        else if (mode == 1) NEAR(dy, 5.0, 0.3);          /* 1 grado/VBlank = 50 grados/s x 0,1 s */
        else NEAR(dy, 35.0, 0.05);                       /* acotada a predict_max_deg */
    }
    VrCamConfig c; vrcam_config_default(&c); c.head = head; c.predict_s = 9.0f;   /* fuera de rango: sin predicción */
    vrcam_init(&c);
    CHECK(vrcam_config()->predict_s == 0.0f);
}

static GpuOp t_ops[8];
static GpuDrawList t_dl;
static void us_scene_point(const VrCamRecord *r, const float V[3], int user_on, float out[3]) {
    memset(t_ops, 0, sizeof t_ops); memset(&t_dl, 0, sizeof t_dl); t_dl.ops = t_ops; t_dl.nops = 1;
    GpuOp *o = &t_ops[0]; o->type = GPU_OP_POLY; o->nv = 3; o->flags = GPU_F_TEXTURED; o->tp = 1;
    /* triángulo con el punto en el vértice 0 y dos auxiliares delante del ojo */
    const float P[3][3] = {{V[0], V[1], V[2]}, {V[0] + 300, V[1], V[2]}, {V[0], V[1] + 300, V[2]}};
    for (int k = 0; k < 3; k++) { o->v[k].vvalid = 1; o->v[k].fvalid = 1; o->v[k].vx = P[k][0]; o->v[k].vy = P[k][1]; o->v[k].vz = P[k][2]; o->v[k].fz = P[k][2]; o->v[k].r = o->v[k].g = o->v[k].b = 100; }
    BridgeState bs; memset(&bs, 0, sizeof bs);
    bs.valid = BRIDGE_OK_ALL_CORE; bs.has_player = 1;
    for (int i = 0; i < 3; i++) { bs.eye_pos[i] = r->eye[i]; for (int j = 0; j < 3; j++) bs.eye_axes[i][j] = (int16_t)lrintf(r->M[3 * i + j] * 4096.0f); }
    bs.player_pos = (BVec3){r->eye[0], r->eye[1], r->eye[2]}; bs.ctl_height = 1104;
    VrSceneOpts so = vr_scene_opts_default();
    so.origin_mode = 1; so.origin_game[0] = (float)r->eye[0]; so.origin_game[1] = (float)r->eye[1] - 1600.0f; so.origin_game[2] = (float)r->eye[2];
    so.near_units = 1.0f; so.max_edge_m = 1000.0f;
    if (user_on) vrcam_scene_user_map(&so, r, so.origin_game[1]);
    VrScene sc;
    CHECK(vr_scene_build(&t_dl, &bs, &so, &sc) == 0 && sc.nv == 3);
    /* el triángulo emitido puede tener los vértices permutados por el frente: game_view (exacta) lleva el punto de vista al espacio de salida */
    for (int r2 = 0; r2 < 3; r2++) out[r2] = sc.game_view[r2] * V[0] + sc.game_view[4 + r2] * V[1] + sc.game_view[8 + r2] * V[2] + sc.game_view[12 + r2];
    vr_scene_release();
}
static void test_userspace(void) {
    VrCamConfig c; vrcam_config_default(&c); c.head = head; c.predict_s = 0; vrcam_init(&c);
    const float thetas[] = {0, 90, 180, 237, 359}, bodyyaw[] = {0, 77, 180};
    const float ys[] = {0, 33, -120}, es[] = {0, 25, -40};
    for (unsigned it = 0; it < 5; it++) for (unsigned iy = 0; iy < 3; iy++) for (unsigned ie = 0; ie < 3; ie++) for (unsigned ib = 0; ib < 3; ib++) {
        /* registro sintético: motor mirando (yaw = theta + yaw cabeza al construir) */
        VrCamRecord r; memset(&r, 0, sizeof r);
        float a_plan = ys[iy];
        r.active = 1; r.units_per_m = 1000; r.theta_play_deg = thetas[it];
        r.yaw_deg = fmodf(thetas[it] + a_plan + 720.0f, 360.0f); r.pitch_deg = -es[ie];
        engine_matrix(r.yaw_deg, r.pitch_deg, r.M);
        r.eye[0] = 12345 + (int)(bodyyaw[ib] * 10); r.eye[1] = 1500; r.eye[2] = -4321;
        G_yaw = a_plan; G_elev = es[ie]; double tt; head(r.head, &tt);
        memcpy(r.p_ref, (float[3]){r.head[12], r.head[13], r.head[14]}, sizeof r.p_ref);
        /* cabeza de presentación: otra orientación y otra posición física */
        G_yaw = a_plan + 25.0f; G_elev = es[ie] - 10.0f; float h[16]; head(h, &tt); h[12] += 0.07f; h[13] -= 0.05f; h[14] += 0.11f;
        VrCamDelta d; CHECK(vrcam_delta(&r, h, &d));
        const float V[3] = {420, -180, 2600};
        float want[3];   /* vista de la cabeza (x der, y arriba, d delante) en metros por vrcam_delta */
        want[0] = (d.A[0] * V[0] + d.A[1] * V[1] + d.A[2] * V[2] + d.b[0]) * 0.001f;
        want[1] = -(d.A[3] * V[0] + d.A[4] * V[1] + d.A[5] * V[2] + d.b[1]) * 0.001f;
        want[2] = (d.A[6] * V[0] + d.A[7] * V[1] + d.A[8] * V[2] + d.b[2]) * 0.001f;
        float U[3]; us_scene_point(&r, V, 1, U);
        float dd[3] = {U[0] - h[12], U[1] - h[13], U[2] - h[14]}, got[3];   /* a la vista de la cabeza: derecha, arriba, atrás */
        got[0] = h[0] * dd[0] + h[1] * dd[1] + h[2] * dd[2];
        got[1] = h[4] * dd[0] + h[5] * dd[1] + h[6] * dd[2];
        got[2] = -(h[8] * dd[0] + h[9] * dd[1] + h[10] * dd[2]);
        /* la altura del ojo del motor (1,6 m sobre el suelo del usuario) coincide con p_ref.y (1,6 m): sin desplazamiento vertical */
        NEAR(got[0], want[0], 0.004); NEAR(got[1], want[1], 0.004); NEAR(got[2], want[2], 0.004);
        /* sin user_map_on: solo coincide si theta + 180 es múltiplo de 360 */
        float U0[3]; us_scene_point(&r, V, 0, U0);
        float d0[3] = {U0[0] - h[12], U0[1] - h[13], U0[2] - h[14]};
        float g0x = h[0] * d0[0] + h[1] * d0[1] + h[2] * d0[2], g0z = -(h[8] * d0[0] + h[9] * d0[1] + h[10] * d0[2]);
        int aligned = fabsf(fmodf(thetas[it] + 180.0f, 360.0f)) < 0.01f;
        if (!aligned) CHECK(fabsf(g0x - want[0]) > 0.05f || fabsf(g0z - want[2]) > 0.05f);
    }
}

/* Copias de la visibilidad dentro de overlays: firma sintética (misma codificación que el ejecutable, otros registros y desplazamientos). */
static void test_scan_overlay(void) {
    static uint8_t ram[0x200000];
    memset(ram, 0, sizeof ram);
    #define W32(a, v) do { uint32_t w_ = (v); memcpy(ram + ((a) & 0x1FFFFFu), &w_, 4); } while (0)
    /* map en 0x800D0000: lhu v0,44(a0); nop; beq v0,zero,+3; and v0,v0,s4; beq v0,zero,D; nop */
    W32(0x800D0000u, 0x9482002Cu); W32(0x800D0004u, 0); W32(0x800D0008u, 0x10400003u); W32(0x800D000Cu, 0x00541024u); W32(0x800D0010u, 0x104000F1u); W32(0x800D0014u, 0);
    /* box en 0x800D0100: andi v0,a2,0x20; beq v0,zero,D; addiu a3,zero,2 */
    W32(0x800D0100u, 0x30C20020u); W32(0x800D0104u, 0x104000EEu); W32(0x800D0108u, 0x24070002u);
    /* part en 0x800D0200: beq s6,zero,X; addu v1,zero,zero; andi v0,s4,0x10; beq v0,zero,D; addiu v1,zero,2 */
    W32(0x800D01F4u, 0x12C000F3u); W32(0x800D01F8u, 0x00001821u); W32(0x800D01FCu, 0x32820010u); W32(0x800D0200u, 0x104000F0u); W32(0x800D0204u, 0x24030002u);
    VrCamOvSite st[8];
    int n = vrcam_scan_overlay(ram, 0x800C4380u, 0x80100000u, st, 8);
    CHECK(n == 3);
    if (n == 3) {
        CHECK(st[0].kind == 0 && st[0].addr == 0x800D0010u && st[0].old == 0x104000F1u && st[0].neu == 0);
        CHECK(st[1].kind == 1 && st[1].addr == 0x800D0104u && st[1].old == 0x104000EEu && st[1].neu == 0x100000EEu);
        CHECK(st[2].kind == 2 && st[2].addr == 0x800D0200u && st[2].old == 0x104000F0u && st[2].neu == 0x100000F0u);
    }
    CHECK(vrcam_scan_overlay(ram, 0x800C4380u, 0x80100000u, st, 2) == 2);          /* respeta el máximo */
    CHECK(vrcam_scan_overlay(ram, 0x800C4380u, 0x80100000u, st, 0) == 0);
    CHECK(vrcam_scan_overlay(NULL, 0x800C4380u, 0x80100000u, st, 8) == 0);
    CHECK(vrcam_scan_overlay(ram, 0x80100000u, 0x800C4380u, st, 8) == 0);          /* rango invertido */
    CHECK(vrcam_scan_overlay(ram, 0x800C4381u, 0x80100000u, st, 8) == 0);          /* sin alinear */
    CHECK(vrcam_scan_overlay(ram, 0x800C4380u, 0x80300000u, st, 8) == 0);          /* fuera de la RAM */
    W32(0x800D0208u, 0); W32(0x800D0108u, 0x24070003u);                             /* box con otra constante: ya no encaja */
    CHECK(vrcam_scan_overlay(ram, 0x800C4380u, 0x80100000u, st, 8) == 2);
    #undef W32
}

/* Amplitud (unidades) de la componente de frecuencia f de x[0..n) muestreada cada dt, tras quitar la recta de mínimos cuadrados. */
static double tone_amp(const double *x, int n, double dt, double f) {
    double sx = 0, sk = 0, skk = 0, sxk = 0;
    for (int k = 0; k < n; k++) { sx += x[k]; sk += k; skk += (double)k * k; sxk += x[k] * k; }
    double b = (n * sxk - sk * sx) / (n * skk - sk * sk), a = (sx - b * sk) / n, c = 0, s = 0;
    for (int k = 0; k < n; k++) { double r = x[k] - a - b * k, w = 6.283185307179586 * f * k * dt; c += r * cos(w); s += r * sin(w); }
    return 2.0 * sqrt(c * c + s * s) / n;
}
static void anchor_start(VrCamConfig *c, BridgeState *s) {
    vrcam_config_default(c); c->head = head; vrcam_init(c); build_world(1024);
    *s = ok_state();
    for (int i = 0; i < c->on_frames; i++) vrcam_frame(ram, s);
    G_ok = 1; G_yaw = 0; G_elev = 0;
}
/* Una construcción de cámara por fotograma de juego (25 por segundo = 2 VBlanks): raíz de control en (x, y, -500), articulación de la cabeza en raíz + (jx, jy, jz). */
static void anchor_tick(const BridgeState *s, int x, int y, int jx, int jy, int jz, uint32_t status, VrCamPlan *p) {
    w16(CTL + BC_MOV, (int16_t)x); w16(CTL + BC_MOV + 2, (int16_t)y);
    w32(BA_PLAYER_STATUS, status);
    w32(PART6 + 0x14, (uint32_t)(x + jx)); w32(PART6 + 0x18, (uint32_t)(y + jy)); w32(PART6 + 0x1C, (uint32_t)(-500 + jz));
    vrcam_frame(ram, s); vrcam_frame(ram, s);
    CHECK(vrcam_plan(ram, 320, p) == 1);
    vrcam_commit(ram, p);
}
/* Ancla del ojo = raíz de locomoción: con la articulación de la cabeza oscilando a 2 Hz (+-40 uds en altura, +-30 de lado y de avance: el balanceo del paso) y el control
 * avanzando en línea recta, el ojo dado al motor avanza en línea recta (componente a 2 Hz < 2 uds = 2 mm). En una cuesta la altura del ojo sube lo mismo que la raíz, sin escalón
 * al empezarla ni al pararse. */
static void test_anchor(void) {
    VrCamConfig c; BridgeState s; VrCamPlan p;
    anchor_start(&c, &s);
    for (int k = 0; k < 25; k++) anchor_tick(&s, 1000, 1029, 10, 468, 20, 0, &p);          /* 1 s quieto */
    NEAR(p.from[1], 1029 + 468 + c.eye_up, 1.5);
    enum { N = 100 };
    static double ex[N], ey[N], ez[N];
    for (int k = 0; k < N; k++) {
        double ph = 6.283185307179586 * 2.0 * k * 0.04;
        anchor_tick(&s, 1000 + 100 * (k + 1), 1029, 10 + (int)lrint(30 * sin(ph + 1.0)), 468 + (int)lrint(40 * sin(ph)), 20 + (int)lrint(30 * sin(ph + 2.0)), BRIDGE_PS_MOVE, &p);
        ex[k] = p.from[0]; ey[k] = p.from[1]; ez[k] = p.from[2];
    }
    double ax = tone_amp(ex + 25, N - 25, 0.04, 2.0), ay = tone_amp(ey + 25, N - 25, 0.04, 2.0), az = tone_amp(ez + 25, N - 25, 0.04, 2.0);
    printf("ancla del ojo, balanceo de 2 Hz de la articulacion (+-40 alto, +-30 lado/avance): componente a 2 Hz en el ojo x=%.2f y=%.2f z=%.2f uds\n", ax, ay, az);
    CHECK(ax < 2.0); CHECK(ay < 2.0); CHECK(az < 2.0);
    NEAR(ex[N - 1] - ex[N - 2], 100, 1.5);                                                   /* el ojo avanza con el control, sin retardo */
    /* cuesta: 40 uds por fotograma hacia arriba mientras avanza, y parada */
    anchor_start(&c, &s);
    for (int k = 0; k < 25; k++) anchor_tick(&s, 1000, 1029, 10, 468, 20, 0, &p);
    int y0 = p.from[1], prev = y0; double worst = 0, worst_stop = 0;
    for (int k = 0; k < 40; k++) {
        anchor_tick(&s, 1000 + 100 * (k + 1), 1029 + 40 * (k + 1), 10, 468 + (k & 1 ? 25 : -25), 20, BRIDGE_PS_MOVE, &p);
        worst = fmax(worst, fabs((double)(p.from[1] - prev) - 40.0)); prev = p.from[1];
    }
    for (int k = 0; k < 25; k++) {
        anchor_tick(&s, 5000, 1029 + 1600, 10, 468, 20, 0, &p);
        worst_stop = fmax(worst_stop, fabs((double)(p.from[1] - prev))); prev = p.from[1];
    }
    printf("cuesta de 40 uds/fotograma: desvio maximo del paso vertical del ojo %.1f uds; paso maximo al pararse %.1f uds\n", worst, worst_stop);
    CHECK(worst <= 3.0); CHECK(worst_stop <= 10.0);
    NEAR(p.from[1], y0 + 1600, 3.0);
}

/* Posturas con ranura propia. Caja de cartón (BRIDGE_PS_BOX): andando, el ojo va a la altura aprendida EN LA CAJA y no a la de pie, y la de pie no se estropea. Derribado o
 * recibiendo un golpe (DOWNED / DAMAGED): aunque la raíz se mueva (retroceso) el ojo sigue a la articulación de la cabeza filtrada, no a una altura de postura. Un salto de la raíz
 * (teletransporte) con el modelo en cualquier pose no cambia la altura aprendida. */
static void test_postures(void) {
    VrCamConfig c; BridgeState s; VrCamPlan p;
    anchor_start(&c, &s);
    for (int k = 0; k < 25; k++) anchor_tick(&s, 1000, 1029, 10, 468, 20, 0, &p);                       /* de pie, quieto: aprende 468 */
    for (int k = 0; k < 25; k++) anchor_tick(&s, 1000, 1029, 10, 200, 20, BRIDGE_PS_BOX, &p);           /* en la caja, quieto: cabeza a 200 sobre la raíz */
    for (int k = 0; k < 25; k++) anchor_tick(&s, 1000 + 60 * (k + 1), 1029, 10, 200 + (k & 1 ? 30 : -30), 20, BRIDGE_PS_BOX | BRIDGE_PS_MOVE, &p);
    printf("caja andando: altura del ojo sobre la raiz %.0f uds (en la caja 200, de pie 468)\n", p.from[1] - c.eye_up - 1029);
    NEAR(p.from[1], 1029 + 200 + c.eye_up, 3.0);
    for (int k = 0; k < 6; k++) anchor_tick(&s, 2500, 1029, 10, 468, 20, 0, &p);                        /* sale de la caja y se queda quieto un momento */
    for (int k = 0; k < 25; k++) anchor_tick(&s, 2500 + 100 * (k + 1), 1029, 10, 468 + (k & 1 ? 30 : -30), 20, BRIDGE_PS_MOVE, &p);
    NEAR(p.from[1], 1029 + 468 + c.eye_up, 3.0);                                                        /* la altura de pie sigue siendo la de pie */
    for (int k = 0; k < 25; k++) anchor_tick(&s, 5000 - 40 * (k + 1), 1029, 10, -700, 20, BRIDGE_PS_DOWNED | BRIDGE_PS_MOVE, &p);   /* derribado y arrastrado: cabeza en el suelo */
    printf("derribado con la raiz en movimiento: altura del ojo sobre la raiz %.0f uds (la cabeza del modelo esta a -700)\n", p.from[1] - c.eye_up - 1029);
    NEAR(p.from[1], 1029 - 700 + c.eye_up, 5.0);
    for (int k = 0; k < 25; k++) anchor_tick(&s, 4000, 1029, 10, 468, 20, 0, &p);                       /* se levanta */
    anchor_tick(&s, -9000, 1029, 10, -300, 20, 0, &p);                                                  /* teletransporte con el modelo a medio animar */
    for (int k = 0; k < 25; k++) anchor_tick(&s, -9000 + 100 * (k + 1), 1029, 10, 468, 20, BRIDGE_PS_MOVE, &p);
    NEAR(p.from[1], 1029 + 468 + c.eye_up, 3.0);                                                        /* el salto no cambió la altura aprendida de pie */
}

/* Paredes de colisión del juego (conductos): el empuje del ojo llega al plan y al registro, y es CONTINUO: una pared que aparece o desaparece de golpe delante del ojo
 * (el ojo va eye_fwd por delante de la raíz que el juego colisiona) lo saca al instante lo justo para la holgura (nunca queda dentro), y al desaparecer el ojo vuelve como mucho a
 * 35 uds por fotograma de juego (< 1 m/s), sin brinco. */
static void test_eye_shift(void) {
    VrCamConfig c; BridgeState s; VrCamPlan p;
    anchor_start(&c, &s);
    uint32_t map = 0x800b8ab8, wr = 0x8015cde4, hdr = 0x80149268, grp = 0x80149284, walls = 0x8014929c, flags = 0x801492c0;
    w32(CTL + 0x2C, map); w32(map + 8, wr); w32(wr, hdr); w32(0x800ACB38, 0);
    w16(hdr + 10, 1); w32(hdr + 16, grp); w16(grp + 2, 1); w32(grp + 8, walls); w32(grp + 20, flags);
    int16_t wl[8] = {1100, -3000, 0, 3000, 1100, 2000, 0, 3000};  /* pared x=1100 delante (mirada +X): la raíz (x=1000) a 100, el ojo libre a 30 -> holgura pedida 100 (la de la raíz): empuje -70 */
    for (int k = 0; k < 8; k++) w16(walls + 2 * k, wl[k]);
    for (int k = 0; k < 10; k++) anchor_tick(&s, 1000, 1029, 10, 468, 20, 0, &p);
    NEAR(p.eye_shift[0], 0, 1e-6); NEAR(p.eye_shift[2], 0, 1e-6); NEAR(p.from[0], 1000 + c.eye_fwd, 1.0);
    double prev = 0, step = 0;
    w32(0x800ACB38, 1);                                       /* la pared aparece de golpe: el ojo sale a la holgura pedida EN ESE MISMO fotograma (nunca queda dentro de la pared) */
    for (int k = 0; k < 30; k++) {
        anchor_tick(&s, 1000, 1029, 10, 468, 20, 0, &p);
        NEAR(p.eye_shift[0], -70, 1.0); NEAR(p.eye_shift[1], 0, 1e-6); NEAR(p.eye_shift[2], 0, 0.5);
        NEAR(p.from[0], 1000 + c.eye_fwd + p.eye_shift[0], 1.0);
    }
    const VrCamRecord *r = vrcam_last_record();
    CHECK(r && r->active);
    if (r) { NEAR(r->eye_shift[0], p.eye_shift[0], 1e-6); NEAR(r->eye_shift[2], p.eye_shift[2], 1e-6); }
    prev = p.eye_shift[0];
    w32(0x800ACB38, 0);                                       /* y desaparece: la retirada sí va filtrada */
    for (int k = 0; k < 30; k++) {
        anchor_tick(&s, 1000, 1029, 10, 468, 20, 0, &p);
        step = fmax(step, fabs(p.eye_shift[0] - prev)); prev = p.eye_shift[0];
        CHECK(p.eye_shift[0] <= 0.5);                        /* sin pasarse de 0 */
    }
    printf("pared delante del ojo que aparece y desaparece (empuje de 70 uds): al entrar, inmediato; al retirarse, paso maximo por fotograma de juego %.1f uds\n", step);
    CHECK(step <= 35.0);
    NEAR(p.eye_shift[0], 0, 1.0); NEAR(p.from[0], 1000 + c.eye_fwd, 1.5);
}


/* Una construcción de cámara por fotograma de juego con el estado del jugador dado; devuelve si la cámara VR se usó (como hle_vrcam_view: plan y commit). */
static int wall_tick(const BridgeState *s, uint32_t status, VrCamPlan *p) {
    w32(BA_PLAYER_STATUS, status);
    vrcam_frame(ram, s); vrcam_frame(ram, s);
    int vr = vrcam_plan(ram, 320, p);
    vrcam_commit(ram, vr ? p : NULL);
    return vr;
}
/* Pegado a la pared: la vista pasa a la del juego (cámara VR apagada a propósito, vrcam_wall_view) con histéresis; el origen angular del mando se conserva. Nunca hay tercera persona propia.
 * Un roce corto, la primera persona del propio juego, la pérdida del control o cfg.wall_view = 0 no la activan. */
static void test_wall_view(void) {
    VrCamConfig c; BridgeState s; VrCamPlan p;
    anchor_start(&c, &s);
    CHECK(c.wall_view == 1);
    for (int k = 0; k < 10; k++) CHECK(wall_tick(&s, 0, &p) == 1);
    const VrCamRecord *r = vrcam_last_record();
    CHECK(vrcam_active() && !vrcam_wall_view() && r && r->active);
    const int org0 = vrcam_pad_origin();
    const float x0 = p.from[0], y0 = p.from[1], z0 = p.from[2];
    /* roce: 12 fotogramas (24 VBlanks < VRCAM_WALL_ON) pegado y se suelta: la vista no cambia (tampoco durante los VBlanks de gracia del parpadeo) y el ojo no se mueve */
    for (int k = 0; k < 12; k++) { CHECK(wall_tick(&s, BRIDGE_PS_WALL, &p) == 1); NEAR(p.from[0], x0, 0.5); NEAR(p.from[1], y0, 0.5); NEAR(p.from[2], z0, 0.5); }
    for (int k = 0; k < 12; k++) { CHECK(wall_tick(&s, 0, &p) == 1); CHECK(!vrcam_wall_view()); }
    /* pegado de verdad, con PS_WALL parpadeando (un fotograma sí y otro no): entra una sola vez y se queda */
    int t_on = -1, flips = 0, prev = 0;
    for (int k = 0; k < 40; k++) {
        int vr = wall_tick(&s, (k & 1) ? 0 : BRIDGE_PS_WALL, &p), w = vrcam_wall_view();
        if (w != prev) { flips++; prev = w; if (w && t_on < 0) t_on = k; }
        CHECK(vr == !w);                                   /* con la vista de pared la cámara VR no se usa (y viceversa) */
    }
    printf("pared: vista del juego tras %d fotogramas (%.2f s) pegado, cambios %d\n", t_on, (t_on + 1) * 0.04f, flips);
    CHECK(t_on >= 11 && t_on <= 14 && flips == 1 && vrcam_wall_view() == 1 && !vrcam_active());
    r = vrcam_last_record();
    CHECK(r && !r->active);
    CHECK(vrcam_pad_origin() == org0);                     /* el mando sigue con el rumbo de cuando se pegó */
    CHECK(vrcam_hide_head_now(ram) == 0);                  /* con la cámara del juego la cabeza se ve */
    /* se despega: sale tras VRCAM_WALL_SEEN + VRCAM_WALL_OFF VBlanks y la cámara VR vuelve tras on_frames */
    int t_off = -1, t_cam = -1;
    for (int k = 0; k < 30; k++) {
        int vr = wall_tick(&s, 0, &p);
        if (t_off < 0 && !vrcam_wall_view()) t_off = k;
        if (t_cam < 0 && vr) t_cam = k;
    }
    printf("pared: vuelve a primera persona %d fotogramas despues de despegarse, camara VR a los %d\n", t_off, t_cam);
    CHECK(t_off >= 0 && (t_off + 1) * 2 >= VRCAM_WALL_SEEN + VRCAM_WALL_OFF - 2 && (t_off + 1) * 2 <= VRCAM_WALL_SEEN + VRCAM_WALL_OFF + 2);
    CHECK(t_cam > t_off && t_cam <= t_off + c.on_frames / 2 + 1 && vrcam_active());
    NEAR(p.from[0], x0, 1.0); NEAR(p.from[2], z0, 1.0);
    /* sin control (escena) en plena vista de pared: se deja al instante */
    for (int k = 0; k < 20; k++) wall_tick(&s, BRIDGE_PS_WALL, &p);
    CHECK(vrcam_wall_view() == 1);
    s.in_control = 0; s.cam_ctl = 0; wall_tick(&s, BRIDGE_PS_WALL, &p);
    CHECK(vrcam_wall_view() == 0);
    s.in_control = 1; s.cam_ctl = 1;
    /* primera persona del propio juego pegado a la pared (asomarse): no es la vista de pared */
    for (int k = 0; k < 30; k++) wall_tick(&s, BRIDGE_PS_WALL | BRIDGE_PS_WATCH, &p);
    CHECK(vrcam_wall_view() == 0 && vrcam_active());
    /* desactivable */
    { VrCamConfig c2; vrcam_config_default(&c2); c2.head = head; c2.wall_view = 0; vrcam_init(&c2); build_world(1024); s = ok_state();
      for (int i = 0; i < c2.on_frames; i++) vrcam_frame(ram, &s);
      for (int k = 0; k < 30; k++) CHECK(wall_tick(&s, BRIDGE_PS_WALL, &p) == 1);
      CHECK(vrcam_wall_view() == 0); }
}

/* Una construcción de cámara por fotograma de juego con la cámara del juego dada (sus banderas y la retrollamada de la cámara de evento) y el estado del jugador. */
static int event_tick(const BridgeState *s, uint32_t cam_flags, uint32_t cam_fn, uint32_t status, VrCamPlan *p) {
    w32(BA_CAMERA + BCAM_FLAG, cam_flags); w32(BA_CAMERA + BCAM_EVENT_FN, cam_fn);
    return wall_tick(s, status, p);
}
/* Cámara de evento del juego con el mando retirado al jugador (panel del ascensor; medido en volcados: banderas de la cámara 0x2, retrollamada 0x800C4E9C, estado 0x20000000, con control):
 * la cámara VR se apaga (vrcam_event_view) y la vista es la del juego con su mando, mientras dure (también con una pausa o una ventana encima). Conductos y gateo (estado 0x4042 / 0x4052,
 * banderas de cámara 0), la cámara de pared (0x10), el mando retirado sin cámara de evento, la cámara de evento sin el mando retirado, un parpadeo y la falta de control (llegada del
 * ascensor: escena) no la activan. */
static void test_event_view(void) {
    enum { PANEL_FN = 0x800C4E9C, PANEL_PS = 0x20000000, DUCT_PS = 0x4042, CRAWL_PS = 0x4052 };
    VrCamConfig c; BridgeState s; VrCamPlan p;
    anchor_start(&c, &s);
    for (int k = 0; k < 10; k++) CHECK(event_tick(&s, 0, 0, 0, &p) == 1);
    CHECK(vrcam_active() && !vrcam_event_view());
    for (int k = 0; k < 30; k++) { CHECK(event_tick(&s, 0, 0, (k & 1) ? CRAWL_PS : DUCT_PS, &p) == 1); CHECK(!vrcam_event_view()); }                /* conductos y gateo: primera persona VR */
    for (int k = 0; k < 30; k++) { CHECK(event_tick(&s, (k & 1) ? 0x10 : 0x20, 0, BRIDGE_PS_WALL, &p) == !vrcam_wall_view()); CHECK(!vrcam_event_view()); }   /* otras banderas de cámara medidas (0x10 pegado a la pared, 0x20) */
    for (int k = 0; k < 20; k++) event_tick(&s, 0, 0, 0, &p);
    CHECK(vrcam_active() && !vrcam_wall_view());
    for (int k = 0; k < 30; k++) { CHECK(event_tick(&s, 0, 0, PANEL_PS, &p) == 1); CHECK(!vrcam_event_view()); }                                    /* mando retirado sin cámara de evento */
    for (int k = 0; k < 30; k++) { CHECK(event_tick(&s, BRIDGE_CAMF_EVENT, PANEL_FN, 0, &p) == 1); CHECK(!vrcam_event_view()); }                   /* cámara de evento con el jugador al mando */
    for (int k = 0; k < 30; k++) { CHECK(event_tick(&s, BRIDGE_CAMF_EVENT, 0, PANEL_PS, &p) == 1); CHECK(!vrcam_event_view()); }                    /* bandera sin retrollamada instalada */
    CHECK(event_tick(&s, BRIDGE_CAMF_EVENT, PANEL_FN, PANEL_PS, &p) == 1 && !vrcam_event_view());                                                    /* un fotograma suelto (2 VBlanks < VRCAM_EVENT_ON) */
    for (int k = 0; k < 10; k++) { CHECK(event_tick(&s, 0, 0, 0, &p) == 1); CHECK(!vrcam_event_view()); }
    /* el panel: entra una sola vez y se queda los 25 s que el usuario estuvo ante él (vb 28900-30145) y el viaje (hasta 30282) */
    int t_on = -1, flips = 0, prev = 0;
    for (int k = 0; k < 690; k++) {
        int vr = event_tick(&s, BRIDGE_CAMF_EVENT, PANEL_FN, PANEL_PS, &p), e = vrcam_event_view();
        if (e != prev) { flips++; prev = e; if (e && t_on < 0) t_on = k; }
        CHECK(vr == !e);                                   /* con la vista del juego la cámara VR no se usa (y viceversa) */
    }
    printf("camara de evento: vista del juego tras %d fotogramas (%.2f s), cambios %d\n", t_on, (t_on + 1) * 0.04f, flips);
    CHECK(t_on >= 0 && (t_on + 1) * 2 <= VRCAM_EVENT_ON + 2 && flips == 1 && vrcam_event_view() == 1 && !vrcam_active() && !vrcam_wall_view());
    const VrCamRecord *r = vrcam_last_record();
    CHECK(r && !r->active);
    /* se sale del panel (el motor quita su cámara; el mando sigue retirado unos fotogramas más): sale tras VRCAM_EVENT_OFF VBlanks y la cámara VR vuelve tras on_frames */
    int t_off = -1, t_cam = -1;
    for (int k = 0; k < 30; k++) {
        int vr = event_tick(&s, 0, 0, k < 8 ? PANEL_PS : 0, &p);
        if (t_off < 0 && !vrcam_event_view()) t_off = k;
        if (t_cam < 0 && vr) t_cam = k;
    }
    printf("camara de evento: vuelve a primera persona %d fotogramas despues de quitarla el motor, camara VR a los %d\n", t_off, t_cam);
    CHECK(t_off >= 0 && (t_off + 1) * 2 >= VRCAM_EVENT_OFF && (t_off + 1) * 2 <= VRCAM_EVENT_OFF + 2);
    CHECK(t_cam > t_off && t_cam <= t_off + c.on_frames / 2 + 1 && vrcam_active());
    /* pausa o ventana de armas/objetos ante el panel (sin control pero cam_ctl): la vista del juego se mantiene y la cámara VR no vuelve */
    for (int k = 0; k < 10; k++) event_tick(&s, BRIDGE_CAMF_EVENT, PANEL_FN, PANEL_PS, &p);
    CHECK(vrcam_event_view() == 1);
    s.in_control = 0;
    for (int k = 0; k < 30; k++) { CHECK(event_tick(&s, BRIDGE_CAMF_EVENT, PANEL_FN, PANEL_PS, &p) == 0); CHECK(vrcam_event_view() == 1); }
    s.in_control = 1;
    /* sin control en plena vista (carga de la planta elegida, escena de llegada): se deja al instante; y sin control no se entra (llegada: cámara de evento con el mando del guion) */
    s.in_control = 0; s.cam_ctl = 0; event_tick(&s, BRIDGE_CAMF_EVENT, PANEL_FN, PANEL_PS, &p);
    CHECK(vrcam_event_view() == 0);
    for (int k = 0; k < 30; k++) { CHECK(event_tick(&s, BRIDGE_CAMF_EVENT, 0x800C4CF4, PANEL_PS, &p) == 0); CHECK(!vrcam_event_view()); }
}

/* Ancla del cuerpo tumbado (gateo, conductos): la articulación de la cabeza queda prone_back por DETRÁS del ojo según el rumbo del CUERPO (los ojos del modelo tumbado van por delante del
 * cuello), no eye_fwd según la mirada; al mirar a los lados el cuerpo no gira alrededor del ojo; la transición no da saltos; el ojo no se mueve; prone_back = 0 deja el ancla de siempre. */
static void test_prone_anchor(void) {
    VrCamConfig c; BridgeState s; VrCamPlan p;
    anchor_start(&c, &s);                                   /* cuerpo y mirada hacia +X */
    CHECK(c.prone_back > c.eye_fwd && c.prone_back <= 400.0f);
    for (int k = 0; k < 25; k++) anchor_tick(&s, 1000, 1029, 10, 468, 20, 0, &p);
    const VrCamRecord *r = vrcam_last_record();
    CHECK(r && r->active);
    if (r) { NEAR(r->head_at[0], p.from[0] - c.eye_fwd, 1.0); NEAR(r->head_at[2], p.from[2], 1.0); NEAR(r->head_at[1], p.from[1] - c.eye_up, 1.0); }
    /* se tumba (misma raíz; la articulación baja): el ancla pasa a prone_back sin saltos; el ojo no se mueve en horizontal */
    const float ex0 = p.from[0], ez0 = p.from[2];
    float prev = r ? p.from[0] - r->head_at[0] : 0, step = 0;
    for (int k = 0; k < 15; k++) {
        anchor_tick(&s, 1000, 1029, 10, 110, 20, BRIDGE_PS_GROUND, &p);
        r = vrcam_last_record();
        float d = p.from[0] - r->head_at[0];
        step = fmaxf(step, fabsf(d - prev)); prev = d;
        NEAR(p.from[0], ex0, 0.5); NEAR(p.from[2], ez0, 0.5);
    }
    printf("tumbado: ancla %.0f uds por detras del ojo (de pie %.0f), paso maximo %.0f uds\n", prev, c.eye_fwd, step);
    NEAR(prev, c.prone_back, 1.0); CHECK(step <= 60.0f);
    NEAR(r->head_at[2], p.from[2], 1.0); NEAR(r->head_at[1], p.from[1] - c.eye_up, 1.0);
    /* mira 90 grados a un lado: el cuerpo sigue detrás del ojo según SU rumbo (no gira alrededor del ojo con la mirada) */
    G_yaw = 90;
    for (int k = 0; k < 5; k++) anchor_tick(&s, 1000, 1029, 10, 110, 20, BRIDGE_PS_GROUND, &p);
    r = vrcam_last_record();
    NEAR(p.from[0] - r->head_at[0], c.prone_back, 2.0);   /* la articulación sigue prone_back por detrás del ojo a lo largo del CUERPO (+X), no de la mirada */
    NEAR(p.from[2] - r->head_at[2], 0.0, 2.0);
    G_yaw = 0;
    /* se levanta: vuelve al ancla de la mirada */
    for (int k = 0; k < 15; k++) anchor_tick(&s, 1000, 1029, 10, 468, 20, 0, &p);
    r = vrcam_last_record();
    NEAR(p.from[0] - r->head_at[0], c.eye_fwd, 1.0);
    /* desactivable */
    { VrCamConfig c2; vrcam_config_default(&c2); c2.head = head; c2.prone_back = 0; vrcam_init(&c2); build_world(1024); s = ok_state();
      for (int i = 0; i < c2.on_frames; i++) vrcam_frame(ram, &s);
      for (int k = 0; k < 15; k++) anchor_tick(&s, 1000, 1029, 10, 110, 20, BRIDGE_PS_GROUND, &p);
      r = vrcam_last_record(); NEAR(p.from[0] - r->head_at[0], c2.eye_fwd, 1.0); }
}

/* Tumbado en un conducto (RAM real de los volcados d0008 / d0009; VRCAM_DUMPS24 = carpeta): con el ojo en la raíz de locomoción el cuerpo se dibuja desplazado hasta que la
 * articulación de la cabeza queda en VrCamRecord.head_at. Ni la cabeza ni el cuello, el torso, la cadera o las piernas quedan por delante del ojo, y la cabeza sigue oculta. */
static void test_prone_dumps(void) {
    const char *root = getenv("VRCAM_DUMPS24"); if (!root) root = "/home/claude/work/adj24/Dumps";
    static const char *D[] = {"d0008_20261103-001918_frame", "d0009_20261103-002117_frame"};
    static uint8_t real[BRIDGE_RAM_SIZE];
    int loaded = 0;
    for (unsigned i = 0; i < sizeof D / sizeof D[0]; i++) {
        char path[512]; snprintf(path, sizeof path, "%s/%s/ram.bin", root, D[i]);
        FILE *f = fopen(path, "rb"); if (!f) continue;
        size_t n = fread(real, 1, sizeof real, f); fclose(f);
        if (n != sizeof real) continue;
        loaded++;
        VrCamConfig c; vrcam_config_default(&c); c.head = head; vrcam_init(&c);
        BridgeState s = ok_state(); VrCamPlan p;
        for (int k = 0; k < c.on_frames; k++) vrcam_frame(real, &s);
        G_ok = 1; G_yaw = 0; G_elev = 0;
        CHECK(vrcam_plan(real, 320, &p) == 1);
        vrcam_commit(real, &p);
        const VrCamRecord *r = vrcam_last_record();
        float j[16][3];
        CHECK(r && r->active && vrcam_model_joints(real, j, 16) == 16);
        if (!r) continue;
        float yr = p.yaw_deg * 3.14159265f / 180.0f, fw[3] = {sinf(yr), 0, cosf(yr)}, body = -1e9f, arms = -1e9f, head6 = 0;
        for (int k = 0; k < 16; k++) {
            float a = 0;
            for (int q = 0; q < 3; q++) a += (j[k][q] + (r->head_at[q] - j[6][q]) - (float)p.from[q]) * fw[q];
            int arm = (k >= 2 && k <= 4) || (k >= 7 && k <= 9);
            if (k == 6) head6 = a;
            if (arm) arms = fmaxf(arms, a); else body = fmaxf(body, a);
        }
        printf("tumbado (%s): empuje de pared %.0f uds; delante del ojo: cabeza %.0f, pieza del cuerpo mas adelantada %.0f, brazos (los lleva la IK) %.0f uds (negativo = detras)\n",
               D[i], hypotf(p.eye_shift[0], p.eye_shift[2]), head6, body, arms);
        NEAR(head6, -c.prone_back, 2.0);                        /* tumbado: la articulación de la cabeza, prone_back por detrás del ojo (cuerpo y mirada alineados en el volcado) */
        CHECK(body <= -c.prone_back + 2.0f);                    /* nada del cuerpo por delante de la cabeza */
        CHECK(vrcam_hide_head_now(real) == 1);
        /* Con las MALLAS del modelo (vértices de cada pieza en el mundo, ya con el ancla): el ojo queda en la cara de la cabeza que se dibujaría (como mucho 60 uds por delante de ella, no
         * 15 cm), y ninguna otra pieza queda a menos de 100 uds del ojo (plano cercano de 0,1 m) ni por delante de él, salvo los antebrazos y las manos apoyados en el suelo. */
        float face = -1e9f, ahead = -1e9f, nearest = 1e9f;
        uint32_t objs; memcpy(&objs, real + (BA_PLAYER_BODY & 0x1FFFFF), 4); memcpy(&objs, real + (objs & 0x1FFFFF), 4);
        for (int k = 0; k < 16; k++) {
            const uint8_t *o = real + ((objs + 0x48 + 0x5C * (uint32_t)k) & 0x1FFFFF);
            int16_t R[9]; int32_t t[3], nv; uint32_t mdl, va;
            memcpy(R, o, sizeof R); memcpy(t, o + 0x14, sizeof t); memcpy(&mdl, o + 0x40, 4);
            memcpy(&nv, real + ((mdl + 0x34) & 0x1FFFFF), 4); memcpy(&va, real + ((mdl + 0x38) & 0x1FFFFF), 4);
            CHECK(nv > 0 && nv <= 256);
            for (int32_t v = 0; v < nv && nv <= 256; v++) {
                int16_t lv[3]; memcpy(lv, real + ((va + 8u * (uint32_t)v) & 0x1FFFFF), sizeof lv);
                float w[3], a = 0;
                for (int q = 0; q < 3; q++) w[q] = (float)(R[3 * q] * lv[0] + R[3 * q + 1] * lv[1] + R[3 * q + 2] * lv[2]) / 4096.0f + (float)t[q] + (r->head_at[q] - j[6][q]) - (float)p.from[q];
                for (int q = 0; q < 3; q++) a += w[q] * fw[q];
                if (k == 6) { face = fmaxf(face, a); continue; }
                nearest = fminf(nearest, sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]));
                if (w[1] > -150.0f) ahead = fmaxf(ahead, a);
            }
        }
        printf("tumbado (%s), mallas: cara de la cabeza %.0f uds respecto al ojo; resto del cuerpo: lo mas adelantado (sin lo apoyado en el suelo) %.0f, lo mas cercano al ojo %.0f uds\n", D[i], face, ahead, nearest);
        CHECK(face <= 0.0f && face >= -60.0f);
        CHECK(ahead <= 0.0f && nearest >= 100.0f);
    }
    if (!loaded) printf("(volcados de gateo no encontrados en %s: sin prueba de tumbado)\n", root);
}

/* Cámara de evento sobre los volcados REALES del visor (build 0d4635f; /tmp/a30/Dumps o VRCAM_DUMPS30): solo el panel del ascensor (d0024: control, banderas de cámara 0x2, retrollamada 0x800C4E9C,
 * estado 0x20000000) la activa; conductos (d0020-d0023), gateo, pared (d0028), la llegada sin control (d0025), cine y fin de partida no. */
static void test_event_dumps(void) {
    const char *root = getenv("VRCAM_DUMPS30"); if (!root) root = "/tmp/a30/Dumps";
    static uint8_t real[BRIDGE_RAM_SIZE];
    static const struct { int n, want; } W[] = {{19, 0}, {20, 0}, {21, 0}, {22, 0}, {23, 0}, {24, 1}, {25, 0}, {26, 0}, {27, 0}, {28, 0}};
    int loaded = 0;
    for (unsigned i = 0; i < sizeof W / sizeof W[0]; i++) {
        char pre[16], path[600]; snprintf(pre, sizeof pre, "d%04d_", W[i].n);
        DIR *d = opendir(root); if (!d) break;
        struct dirent *e; path[0] = 0;
        while ((e = readdir(d)))
            if (!strncmp(e->d_name, pre, 6) && strstr(e->d_name, "_stage") && !strstr(e->d_name, ".tmp")) { snprintf(path, sizeof path, "%s/%s/ram.bin", root, e->d_name); break; }
        closedir(d);
        FILE *f = path[0] ? fopen(path, "rb") : NULL; if (!f) continue;
        size_t n = fread(real, 1, sizeof real, f); fclose(f);
        if (n != sizeof real) continue;
        loaded++;
        VrCamConfig c; vrcam_config_default(&c); c.head = head; vrcam_init(&c);
        BridgeState s; int ev = 0;
        for (int k = 0; k < 60; k++) { bridge_read(real, &s); vrcam_frame(real, &s); vrcam_frame(real, &s); ev = vrcam_event_view(); }
        printf("camara de evento (d%04d): in_control=%d cam_ctl=%d vista del juego=%d (esperado %d)\n", W[i].n, s.in_control, s.cam_ctl, ev, W[i].want);
        CHECK(ev == W[i].want);
    }
    if (!loaded) printf("(volcados del ascensor no encontrados en %s: sin prueba con datos reales)\n", root);
}

int main(void) {
    test_angles();
    test_patch();
    test_scan_overlay();
    test_hide_head();
    test_viewer();
    test_activation();
    test_engine_h();
    test_delta();
    test_e2e();
    test_records();
    test_userspace();
    test_predict();
    test_robust();
    test_anchor();
    test_postures();
    test_eye_shift();
    test_wall_view();
    test_event_view();
    test_prone_anchor();
    test_prone_dumps();
    test_event_dumps();
    printf("%d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
