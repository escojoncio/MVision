/* Prueba de vr_cine: cortes de plano y cámaras estéreo. make -C vr/host test */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "vr_cine.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FALLO %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

static void rot_y(float deg, float M[9]) {   /* ejes de vista (X der, Y abajo, Z delante) girados alrededor del eje vertical del mundo (Y arriba) */
    float c = cosf(deg * 0.0174533f), s = sinf(deg * 0.0174533f);
    /* columnas: X = (c,0,-s), Y = (0,-1,0) (abajo), Z = (s,0,c) */
    float m[9] = {c, 0, s, 0, -1, 0, -s, 0, c};
    memcpy(M, m, sizeof m);
}

int main(void) {
    VrCine s;
    memset(&s, 0, sizeof s);
    vrcine_config_default(&s.cfg);
    vrcine_reset(&s);
    float M[9];
    rot_y(0, M);
    int32_t e[3] = {1000, 1700, 2000};
    CHECK(vrcine_camera(&s, 1, e, M) == 1 && s.shot == 1 && s.cuts == 0, "el primer plano empieza y no cuenta como corte");
    CHECK(s.origin[0] == 1000 && s.origin[2] == 2000, "origen del plano");
    /* travelling: 40 unidades por lista y giro lento */
    int cuts = 0;
    for (int i = 1; i <= 60; i++) { e[0] += 40; rot_y(0.3f * (float)i, M); cuts += vrcine_camera(&s, 1, e, M); }
    CHECK(cuts == 0 && s.shot == 1, "el movimiento continuo no es un corte (%d)", cuts);
    CHECK(s.origin[0] == 1000, "el origen del plano no cambia en el travelling");
    /* corte por posición */
    e[0] += 6000;
    CHECK(vrcine_camera(&s, 1, e, M) == 1 && s.shot == 2 && s.cuts == 1 && s.origin[0] == e[0], "corte por salto de posición");
    /* corte por orientación con la misma posición */
    rot_y(120, M);
    CHECK(vrcine_camera(&s, 1, e, M) == 1 && s.shot == 3 && s.cuts == 2, "corte por giro");
    /* giro rápido pero continuo (10 grados por lista) no es corte */
    int c2 = 0;
    for (int i = 0; i < 20; i++) { rot_y(120.0f + 10.0f * (float)(i + 1), M); c2 += vrcine_camera(&s, 1, e, M); }
    CHECK(c2 == 0, "panorámica rápida sin corte (%d)", c2);
    /* fuera de cinemática se descarta el plano; al volver empieza otro */
    vrcine_camera(&s, 0, e, M);
    CHECK(vrcine_camera(&s, 1, e, M) == 1 && s.shot == 4 && s.cuts == 2, "nueva cinemática: plano nuevo sin contar corte");
    /* NaN = corte */
    float Mn[9]; memcpy(Mn, M, sizeof Mn); Mn[2] = NAN;
    CHECK(vrcine_camera(&s, 1, e, Mn) == 1, "NaN cuenta como corte");

    /* ---- estéreo: cámara del juego en el origen mirando a +Z del motor, 1 unidad = 1 mm ---- */
    float gv[16] = {0};
    /* vista (x der, y abajo, z delante) -> mundo VR: x = 0,001 x, y = -0,001 y, z = -0,001 z (cámara que mira a -Z VR) */
    gv[0] = 0.001f; gv[5] = -0.001f; gv[10] = -0.001f; gv[15] = 1; gv[12] = 0.5f; gv[13] = 1.7f; gv[14] = -0.2f;
    VrCineEye L, R;
    CHECK(vrcine_eye(gv, 0, 0.063f, 3.0f, 0.6f, 0.45f, &L) && vrcine_eye(gv, 1, 0.063f, 3.0f, 0.6f, 0.45f, &R), "ojos");
    /* punto a 3 m delante de la cámara (conv): sin paralaje */
    float p[3] = {0.5f, 1.7f, -3.2f}, ul, vl, ur, vr, d;
    CHECK(vrcine_project(&L, p, &ul, &vl, &d) && vrcine_project(&R, p, &ur, &vr, 0), "proyecta");
    CHECK(fabsf(ul - ur) < 1e-4f && fabsf(ul - 0.5f) < 1e-3f && fabsf(vl - 0.5f) < 1e-3f && fabsf(d - 3.0f) < 1e-3f, "plano de convergencia sin paralaje y centrado: u=%g/%g v=%g d=%g", ul, ur, vl, d);
    float pn[3] = {0.5f, 1.7f, -1.2f}, pf[3] = {0.5f, 1.7f, -30.2f};
    vrcine_project(&L, pn, &ul, &vl, 0); vrcine_project(&R, pn, &ur, &vr, 0);
    CHECK(ul > ur + 1e-3f, "lo cercano sale de la pantalla (paralaje cruzado): %g %g", ul, ur);
    vrcine_project(&L, pf, &ul, &vl, 0); vrcine_project(&R, pf, &ur, &vr, 0);
    CHECK(ul < ur - 1e-4f, "lo lejano queda detrás (paralaje sin cruzar): %g %g", ul, ur);
    /* un punto a la derecha del eje se proyecta a la derecha y arriba del eje arriba */
    float pr[3] = {1.5f, 2.7f, -3.2f};
    vrcine_project(&L, pr, &ul, &vl, 0);
    CHECK(ul > 0.5f && vl < 0.5f, "derecha y arriba: u=%g v=%g", ul, vl);
    /* la cabeza no interviene: la función no recibe pose, así que nada cambia al pedirla otra vez */
    VrCineEye L2; vrcine_eye(gv, 0, 0.063f, 3.0f, 0.6f, 0.45f, &L2);
    CHECK(!memcmp(&L, &L2, sizeof L), "determinista");
    /* separación entre ojos = ipd */
    CHECK(fabsf((R.view[12] - L.view[12]) + 0.063f) < 1e-5f, "los ojos están separados una IPD: %g", R.view[12] - L.view[12]);
    float sing[16] = {0}; sing[15] = 1;
    CHECK(!vrcine_eye(sing, 0, 0.063f, 3.0f, 0.6f, 0.45f, &L), "game_view singular");
    CHECK(!vrcine_eye(gv, 0, 0.063f, 0.0f, 0.6f, 0.45f, &L), "convergencia inválida");
    fprintf(stderr, fails ? "test_cine: %d FALLOS\n" : "test_cine: OK\n", fails);
    return fails ? 1 : 0;
}
