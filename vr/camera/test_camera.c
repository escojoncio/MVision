/* Pruebas de la cámara VR sobre una RAM sintética (no necesitan datos del juego). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_camera.h"
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
    s.valid = BRIDGE_OK_ALL_CORE; s.has_player = 1; s.in_control = 1;
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
    /* salida inmediata al perder el control */
    s.in_control = 0; vrcam_frame(ram, &s);
    CHECK(vrcam_plan(ram, 320, &p) == 0 && !vrcam_active());
    s.in_control = 1; s.has_player = 0;
    for (int i = 0; i < 20; i++) vrcam_frame(ram, &s);
    CHECK(vrcam_plan(ram, 320, &p) == 0);
    s.has_player = 1; s.valid &= ~1u;           /* un grupo del puente sin validar */
    for (int i = 0; i < 20; i++) vrcam_frame(ram, &s);
    CHECK(vrcam_plan(ram, 320, &p) == 0);
    VrCamStats st; vrcam_stats(&st);
    CHECK(st.activations == 1 && st.deactivations >= 1);
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
    vrcam_commit(ram, &p);
    const VrCamRecord *r = vrcam_last_record();
    CHECK(r && r->serial == 1 && r->active && r->eye[0] == 1234 && r->eye[2] == -77 && r->screen == 320);
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
    CHECK(vrcam_patch_ram(a, &p) == 45);       /* 44 inmediatos de recorte + subdivisión; "punto visible" intacto */
    CHECK(memcmp(a + (base & 0x1FFFFF), b + (base & 0x1FFFFF), size) == 0);   /* coincide byte a byte con patch_exe.py */
    /* "punto visible" y subdivisión realmente intactos sin sus opciones */
    { uint32_t w; memcpy(&w, a + (0x8001c440u & 0x1FFFFF), 4); CHECK((int16_t)(w & 0xFFFF) == 192);
      memcpy(&w, a + (0x8001c5e8u & 0x1FFFFF), 4); CHECK((int16_t)(w & 0xFFFF) == 192); }
    const char *ew = getenv("VRCAM_EXE_PATCHED_WIDE");
    if (ew) {
        static uint8_t cw[BRIDGE_RAM_SIZE]; uint32_t b3, s3; VrCamPatch pw = p; pw.wide_points = 1;
        memset(cw, 0, sizeof cw); memset(b, 0, sizeof b);
        CHECK(load_exe(eo, cw, &b3, &s3) && load_exe(ew, b, &base2, &size2) && vrcam_patch_ram(cw, &pw) == 57);
        CHECK(memcmp(cw + (b3 & 0x1FFFFF), b + (base2 & 0x1FFFFF), s3) == 0);
    }
    /* doble aplicación: ya no coincide con lo esperado */
    CHECK(vrcam_patch_ram(a, &p) == -1);
    /* todo o nada: un solo sitio distinto (otra edición del EXE) -> -1 y la RAM queda idéntica, sea cual sea el sitio */
    {
        static const uint32_t bad_sites[] = {0x80018b40u, 0x8001e4b8u, 0x8001e6f4u, 0x8001c604u, 0x8001A11Cu};
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
        pn = p; pn.vfov_deg = 180; pn.engine_h = 2000000000; CHECK(vrcam_patch_ram(a, &pn) == 45);   /* caja limitada a 1020 */
        { uint32_t w2; memcpy(&w2, a + (0x80018b54u & 0x1FFFFF), 4); CHECK((int16_t)(w2 & 0xFFFF) == 1021); }
    }
    memset(a, 0, sizeof a); load_exe(eo, a, &base, &size);
    /* sin subdivisión y con otra caja */
    memset(a, 0, sizeof a); load_exe(eo, a, &base, &size);
    p.no_divide = 0; p.engine_h = 320; p.hfov_deg = 110; p.vfov_deg = 90;
    CHECK(vrcam_patch_ram(a, &p) == 44);
    CHECK(vrcam_patch_ram(a, &p) == -1);
    p.hfov_deg = 10; memset(a, 0, sizeof a); load_exe(eo, a, &base, &size); CHECK(vrcam_patch_ram(a, &p) == -1);   /* parámetros fuera de rango */
}

int main(void) {
    test_angles();
    test_patch();
    test_hide_head();
    test_activation();
    test_engine_h();
    test_delta();
    test_e2e();
    test_records();
    test_robust();
    printf("%d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
