/* Pruebas del rumbo del personaje (vr_heading) y de la calibración de altura/escala (vr_calib) sobre una RAM sintética. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_calib.h"
#include "vr_camera.h"
#include "vr_heading.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FALLA %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, e) do { checks++; if (fabs((double)(a) - (double)(b)) > (e)) { fails++; printf("FALLA %s:%d: %s=%g esperado %g\n", __FILE__, __LINE__, #a, (double)(a), (double)(b)); } } while (0)

static uint8_t ram[BRIDGE_RAM_SIZE];
static void w16(uint32_t a, int16_t v) { memcpy(ram + (a & 0x1FFFFF), &v, 2); }
static void w32(uint32_t a, uint32_t v) { memcpy(ram + (a & 0x1FFFFF), &v, 4); }
static int16_t r16(uint32_t a) { int16_t v; memcpy(&v, ram + (a & 0x1FFFFF), 2); return v; }

static float G_yaw, G_hy = 1.6f;
static int head(float m[16], double *t) {
    float y = G_yaw * 3.14159265f / 180, c = cosf(y), s = sinf(y);
    float X[3] = {c, 0, -s}, Y[3] = {0, 1, 0}, Z[3] = {s, 0, c};
    for (int i = 0; i < 3; i++) { m[i] = X[i]; m[4 + i] = Y[i]; m[8 + i] = Z[i]; }
    m[3] = m[7] = m[11] = 0; m[12] = 0.05f; m[13] = G_hy; m[14] = -0.1f; m[15] = 1; *t = 0;
    return 1;
}
#define CTL   0x80100000u
#define BODY  0x80110000u
#define OBJS  0x80120000u
#define PART6 (OBJS + 0x48 + 6 * 0x5C)
static void world(int yaw_units) {
    memset(ram, 0, sizeof ram);
    w32(BA_PLAYER_CTRL, CTL);
    w16(CTL + BC_MOV, 1000); w16(CTL + BC_MOV + 2, 1029); w16(CTL + BC_MOV + 4, -500);
    w16(CTL + BC_ROT + 2, (int16_t)yaw_units); w16(CTL + 0x2A, (int16_t)yaw_units); w16(CTL + 0x4E, (int16_t)yaw_units);
    w32(BA_PLAYER_BODY, BODY); w32(BODY, OBJS); w16(OBJS + 0x2E, 16);
    w32(PART6 + 0x14, 1010); w32(PART6 + 0x18, 1497); w32(PART6 + 0x1C, (uint32_t)-480); w16(PART6 + 0x4C, 2);
}
static BridgeState st;
static void setup(int yaw_units, int calibrate) {
    G_yaw = 0;
    world(yaw_units);
    VrCamConfig c; vrcam_config_default(&c); c.head = head; c.calibrate = calibrate; vrcam_init(&c);
    vrhead_init(NULL);
    memset(&st, 0, sizeof st);
    st.valid = BRIDGE_OK_ALL_CORE; st.has_player = 1; st.in_control = 1; st.cam_ctl = 1; st.ctl_addr = CTL;
    for (int i = 0; i < 10; i++) vrcam_frame(ram, &st);
}
static float body_deg(void) { return (float)((uint16_t)r16(CTL + BC_ROT + 2) & 4095u) * (360.0f / 4096.0f); }
static float wrap180(float a) { a = fmodf(a + 180.0f, 360.0f); if (a < 0) a += 360.0f; return a - 180.0f; }
/* un VBlank con la cámara: construye la cámara (fija la mirada) y llama al gancho de rumbo */
static int tick(int walking) {          /* orden real: gancho de VBlank (giro + rumbo) y luego el motor construye la cámara */
    int r = vrhead_frame(ram, &st, walking);
    VrCamPlan p; vrcam_plan(ram, 320, &p); vrcam_commit(ram, &p);
    return r;
}

static void test_turn(void) {
    setup(2048, 0);
    tick(0);
    float v0 = vrcam_view_yaw_deg(), b0 = body_deg();
    NEAR(wrap180(v0 - b0), 0, 0.2);                  /* tras recentrar la mirada coincide con el rumbo del personaje */
    /* giro por pasos del stick: +30 (izquierda) rota la vista y el personaje a la vez */
    vrhead_turn(30.0f);
    CHECK(tick(0) == 1);
    NEAR(wrap180(vrcam_view_yaw_deg() - v0), 30, 0.05);
    NEAR(wrap180(body_deg() - b0), 30, 0.1);
    NEAR(wrap180(vrcam_view_yaw_deg() - body_deg()), 0, 0.2);
    CHECK(r16(CTL + 0x2A) == r16(CTL + BC_ROT + 2) && r16(CTL + 0x4E) == r16(CTL + BC_ROT + 2));   /* los tres campos de rumbo juntos */
    CHECK(tick(0) == 0);                              /* sin más giro no se escribe */
    /* giro suave: 60 grados en 100 VBlanks de 0,6 */
    for (int i = 0; i < 100; i++) { vrhead_turn(0.6f); tick(0); }
    NEAR(wrap180(vrcam_view_yaw_deg() - v0), 90, 0.5);
    NEAR(wrap180(body_deg() - b0), 90, 1.0);          /* el resto fraccionario no se pierde */
    /* negativo y vuelta por 0/360 */
    vrhead_turn(-200.0f); tick(0);
    NEAR(wrap180(body_deg() - b0), 90 - 200, 1.0);
    /* desactivado: solo la vista */
    VrHeadingConfig hc; vrhead_config_default(&hc); hc.enabled = 0; vrhead_init(&hc);
    float bb = body_deg();
    vrhead_turn(40.0f); tick(0);
    NEAR(body_deg(), bb, 1e-3);
}

static void test_torso(void) {
    setup(1024, 0);
    tick(0);
    float b0 = body_deg();
    /* cabeza girada 30 grados (dentro del margen de 45): el torso no se mueve, ni siquiera tras mucho tiempo si es poco desvío */
    G_yaw = 30; for (int i = 0; i < 400; i++) tick(0);
    NEAR(wrap180(body_deg() - b0), 0, 0.1 + 18.5);   /* relajación a 12 grados de desvío */
    /* relajación: parado con la cabeza girada 30 grados, el torso se acerca hasta dejar 12 grados */
    NEAR(wrap180(vrcam_view_yaw_deg() - body_deg()), 12, 0.6);
    setup(1024, 0); tick(0); b0 = body_deg();
    G_yaw = 30; for (int i = 0; i < 30; i++) tick(0);      /* < 1,2 s quieto: sin relajar todavía */
    NEAR(wrap180(body_deg() - b0), 0, 0.1);
    /* cabeza a 100 grados: el torso se arrastra hasta dejarla a 45 grados de desvío y no más */
    G_yaw = 100; int n = 0;
    for (int i = 0; i < 30; i++) { n += tick(0); }
    CHECK(n > 0);
    NEAR(wrap180(vrcam_view_yaw_deg() - body_deg()), 45, 0.6);
    /* velocidad máxima: 300 grados/s * 0,02 = 6 grados por VBlank */
    setup(1024, 0); tick(0); G_yaw = 150; float prev = body_deg(), mx = 0;
    for (int i = 0; i < 30; i++) { tick(0); float d = fabsf(wrap180(body_deg() - prev)); if (d > mx) mx = d; prev = body_deg(); }
    CHECK(mx <= 6.1f && mx > 5.0f);
    /* andando no se escribe (el juego orienta al personaje) */
    setup(1024, 0); tick(0); b0 = body_deg(); G_yaw = 120;
    for (int i = 0; i < 20; i++) tick(1);
    NEAR(body_deg(), b0, 1e-3);
    /* sentido: cabeza a la derecha (-) arrastra el torso a la derecha */
    setup(1024, 0); tick(0); b0 = body_deg(); G_yaw = -100; for (int i = 0; i < 30; i++) tick(0);
    CHECK(wrap180(body_deg() - b0) < -40);
    NEAR(wrap180(vrcam_view_yaw_deg() - body_deg()), -45, 0.6);
    G_yaw = 0;
}

static void test_guards(void) {
    setup(500, 0);
    tick(0);
    uint8_t save[BRIDGE_RAM_SIZE]; memcpy(save, ram, sizeof ram);
    BridgeState bad = st; bad.ctl_addr = 0;
    vrhead_turn(20.0f);
    CHECK(vrhead_frame(ram, &bad, 0) == 0);
    bad = st; bad.valid &= ~BRIDGE_OK_PLAYER_CTL;
    CHECK(vrhead_frame(ram, &bad, 0) == 0);
    bad = st; bad.ctl_addr = 0x80000010u;        /* fuera de la zona válida */
    CHECK(vrhead_frame(ram, &bad, 0) == 0);
    CHECK(memcmp(save, ram, sizeof ram) == 0);   /* no escribió nada */
    CHECK(vrhead_frame(NULL, &st, 0) == 0 && vrhead_frame(ram, NULL, 0) == 0);
    vrhead_turn(NAN); vrhead_turn(INFINITY);
    VrHeadingConfig c = {0}; c.margin_deg = NAN; c.frame_dt_s = -1; vrhead_init(&c);      /* configuración basura -> valores por defecto razonables */
    CHECK(vrhead_config()->margin_deg == 45.0f && vrhead_config()->follow_dps > 0 && vrhead_config()->frame_dt_s > 0);
}

static void test_calib(void) {
    VrCalibCfg c; vrcalib_cfg_default(&c);
    VrCalib k;
    vrcalib_compute(&c, 1.04f, &k);                 /* sentado, como en el visor */
    CHECK(k.seated); NEAR(k.user_eye_m, 1.04f / 0.63f, 1e-4); NEAR(k.scale, 0.96f * 1.65f / (1.04f / 0.63f), 1e-4);
    vrcalib_compute(&c, 1.65f, &k);
    CHECK(!k.seated); NEAR(k.scale, 0.96f, 1e-4);
    vrcalib_compute(&c, 1.90f, &k);
    NEAR(k.scale, 0.96f * 1.65f / 1.90f, 1e-4);
    vrcalib_compute(&c, 2.5f, &k); CHECK(k.scale >= c.s_min && k.scale <= c.s_max);
    vrcalib_compute(&c, NAN, &k); NEAR(k.scale, 1.0f, 1e-6); CHECK(!k.seated);
    vrcalib_compute(&c, -1.0f, &k); NEAR(k.scale, 1.0f, 1e-6);
    c.user_eye_m = 1.80f; vrcalib_compute(&c, 1.04f, &k);   /* altura conocida: manda sobre la estimación (sentado) */
    CHECK(k.seated); NEAR(k.user_eye_m, 1.80f, 1e-6); NEAR(k.scale, 0.96f * 1.65f / 1.80f, 1e-4);
    c.user_eye_m = 0.2f; vrcalib_compute(&c, 1.65f, &k);    /* altura absurda: se ignora */
    NEAR(k.user_eye_m, 1.65f, 1e-4);

    /* integrado en la cámara: la escala sale en el registro (units_per_m) y recalibra al recentrar */
    G_hy = 1.04f; setup(2048, 1);
    VrCamPlan p; CHECK(vrcam_plan(ram, 320, &p)); vrcam_commit(ram, &p);
    const VrCalib *cal = vrcam_calib();
    CHECK(cal->seated); NEAR(cal->head_y_m, 1.04f, 1e-5);
    const VrCamRecord *r = vrcam_last_record();
    NEAR(r->units_per_m, 1000.0f * cal->scale, 1e-2);
    NEAR(r->p_ref[1], 1.04f, 1e-5);
    G_hy = 1.65f; vrcam_recenter(); vrcam_plan(ram, 320, &p); vrcam_commit(ram, &p);
    CHECK(!vrcam_calib()->seated); NEAR(vrcam_last_record()->units_per_m, 960.0f, 1e-2);
    vrcam_set_user_eye_height(1.80f); vrcam_plan(ram, 320, &p); vrcam_commit(ram, &p);
    NEAR(vrcam_last_record()->units_per_m, 1000.0f * 0.96f * 1.65f / 1.80f, 1e-2);
    /* sin calibrar: units_per_m intacto */
    G_hy = 1.04f; setup(2048, 0); vrcam_plan(ram, 320, &p); vrcam_commit(ram, &p);
    NEAR(vrcam_last_record()->units_per_m, 1000.0f, 1e-3);
    G_hy = 1.6f;
}

static void test_step(void) {
    VrHeadingConfig c; vrhead_config_default(&c);
    NEAR(vrhead_follow_step(&c, 30, 5, 0.02f), 0, 1e-6 + 0.9);          /* relajación: sentido y tope 0,9 grados */
    CHECK(vrhead_follow_step(&c, 30, 5, 0.02f) > 0 && vrhead_follow_step(&c, -30, 5, 0.02f) < 0);
    NEAR(vrhead_follow_step(&c, 30, 0.5f, 0.02f), 0, 1e-9);             /* aún no relaja */
    NEAR(vrhead_follow_step(&c, 10, 9, 0.02f), 0, 1e-9);                /* ya dentro de relax_deg */
    NEAR(vrhead_follow_step(&c, 46, 0, 0.02f), 1, 1e-6);                /* justo fuera del margen: 1 grado */
    NEAR(vrhead_follow_step(&c, -170, 0, 0.02f), -6, 1e-6);             /* tope 6 grados por VBlank */
}

static void test_walk(void) {
    /* andando: el rumbo del personaje se adelanta al deseado por el juego (+0x4E), con tope, sin pasarse y sin pisar el deseado */
    setup(2048, 0); tick(0);
    float b0 = body_deg();
    int16_t goal = (int16_t)(2048 + 1024);                    /* 90 grados a la izquierda del rumbo actual */
    w16(CTL + 0x4E, goal);
    st.moving = 1; st.on_wall = 0; st.first_person = 0; st.knocking = 0;
    float prev = b0, maxstep = 0;
    for (int i = 0; i < 4; i++) { tick(1); float b = body_deg(); float d = wrap180(b - prev); if (fabsf(d) > maxstep) maxstep = fabsf(d); prev = b; }
    NEAR(wrap180(body_deg() - b0), 4 * 10.8f, 0.5);           /* 540 grados/s a 0,02 s = 10,8 por VBlank */
    NEAR(maxstep, 10.8f, 0.2);
    CHECK(r16(CTL + 0x4E) == goal);                           /* el deseado del juego no se toca */
    for (int i = 0; i < 20; i++) tick(1);
    NEAR(wrap180(body_deg() - b0), 90, 0.2);                  /* llega y no se pasa */
    CHECK(tick(1) == 0);                                       /* ya alineado: no escribe */
    /* pegado a pared / primera persona / golpeando / sin control: no actúa */
    int flags[3] = {0, 1, 2};
    for (int k = 0; k < 4; k++) {
        setup(2048, 0); tick(0);
        w16(CTL + 0x4E, goal);
        st.moving = 1; st.on_wall = k == 0; st.first_person = k == 1; st.knocking = k == 2; st.in_control = k == 3 ? 0 : 1;
        (void)flags;
        float b = body_deg();
        int r = vrhead_frame(ram, &st, 1);
        CHECK(r == 0 && fabsf(wrap180(body_deg() - b)) < 0.1f);
    }
    st.in_control = 1; st.cam_ctl = 1;
    /* sin andar: el seguimiento normal (margen) manda, el deseado se ignora */
    setup(2048, 0); tick(0);
    w16(CTL + 0x4E, goal);
    st.moving = 0;
    CHECK(tick(0) == 0);
    /* desactivado (walk_dps = 0) */
    VrHeadingConfig hc; vrhead_config_default(&hc); hc.walk_dps = 0; vrhead_init(&hc);
    setup(2048, 0); vrhead_init(&hc); tick(0);
    w16(CTL + 0x4E, goal); st.moving = 1;
    CHECK(tick(1) == 0);
    /* un giro del stick mientras anda sigue escribiendo los tres campos */
    vrhead_init(NULL); setup(2048, 0); tick(0);
    st.moving = 1; w16(CTL + 0x4E, 2048);
    vrhead_turn(30.0f);
    CHECK(tick(1) == 1 && r16(CTL + 0x4E) == r16(CTL + BC_ROT + 2) && r16(CTL + 0x2A) == r16(CTL + BC_ROT + 2));
    /* paso puro */
    VrHeadingConfig c; vrhead_config_default(&c);
    NEAR(vrhead_walk_step(&c, 0.5f, 0.02f), 0, 1e-9);
    NEAR(vrhead_walk_step(&c, 5.0f, 0.02f), 5.0f, 1e-6);
    NEAR(vrhead_walk_step(&c, -170.0f, 0.02f), -10.8f, 1e-5);
    NEAR(vrhead_walk_step(&c, NAN, 0.02f), 0, 1e-9);
}

static void test_eye(void) {
    /* AUTO: x,z del ojo en la posición de control (raíz de locomoción), no en la articulación de la cabeza del modelo, y sin adelanto */
    setup(2048, 0);
    VrCamPlan p;
    G_yaw = 0;
    vrcam_plan(ram, 320, &p);
    CHECK(abs(p.from[0] - 1000) <= 71);                       /* x del control (1000 +- eye_fwd) */
    NEAR(p.from[1], 1497 + vrcam_config()->eye_up, 1.0);
    int x0 = p.from[0];
    w32(PART6 + 0x14, 1060);                                  /* la animación movió la articulación 50 unidades: el ojo no se mueve */
    vrcam_frame(ram, &st); vrcam_plan(ram, 320, &p);
    NEAR(p.from[0] - x0, 0, 1.0);
    w16(CTL + BC_MOV, 1050);                                  /* el control avanzó 50 unidades: el ojo también, sin adelanto */
    vrcam_frame(ram, &st); vrcam_plan(ram, 320, &p);
    NEAR(p.from[0] - x0, 50, 1.0);
    /* JOINT: x,z de la articulación */
    VrCamConfig c = *vrcam_config(); c.eye_xz = VRCAM_EYE_JOINT; vrcam_init(&c);
    for (int i = 0; i < 10; i++) vrcam_frame(ram, &st);
    vrcam_plan(ram, 320, &p);
    NEAR(p.from[0] - x0, 60, 1.0);
}

int main(void) {
    test_turn(); test_torso(); test_guards(); test_calib(); test_step(); test_walk(); test_eye();
    printf("test_heading: %d comprobaciones, %d fallos\n", checks, fails);
    return fails != 0;
}
