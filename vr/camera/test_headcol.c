/* Pruebas de vr_headcol (sin datos del juego). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_headcol.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define NEAR(a, b, e) (fabsf((a) - (b)) <= (e))

static VrHcMap mk_map(float theta_play_deg, const float eye[3], const float pref[3]) {
    VrHcMap m; VrCamRecord r; memset(&r, 0, sizeof r);
    r.active = 1; r.theta_play_deg = theta_play_deg; r.units_per_m = 1000.0f;
    for (int i = 0; i < 3; i++) { r.eye[i] = (int32_t)eye[i]; r.p_ref[i] = pref[i]; }
    vrhc_map_from_record(&m, &r);
    return m;
}
/* pared vertical (dos triángulos) en el plano x = xw, z en [-1,1], y en [0,3] (espacio del usuario, m) */
static int feed_wall(const VrHcMap *m, float xw, int times) {
    float t[18] = {xw, 0, -1, xw, 3, -1, xw, 3, 1,   xw, 0, -1, xw, 3, 1, xw, 0, 1};
    int k = 0;
    for (int i = 0; i < times; i++) k = vrhc_feed_tris(m, t, 2, NULL, 0);
    return k;
}

static void test_basics(void) {
    float a[3] = {0, 0, 0}, b[3] = {1, 0, 0}, c[3] = {0, 1, 0}, p[3] = {0.25f, 0.25f, 2}, q[3];
    CHECK(NEAR(vrhc_tri_dist(a, b, c, p, q), 2.0f, 1e-5f), "cara");
    float p2[3] = {2, 0, 0};
    CHECK(NEAR(vrhc_tri_dist(a, b, c, p2, q), 1.0f, 1e-5f) && NEAR(q[0], 1, 1e-5f), "vértice");
    float p3[3] = {1, 1, 0};
    CHECK(NEAR(vrhc_tri_dist(a, b, c, p3, q), sqrtf(0.5f), 1e-5f), "arista");
    VrHcCfg c0; vrhc_config_default(&c0);
    CHECK(c0.enabled && c0.radius_m >= 0.12f && c0.radius_m <= 0.15f, "radio por defecto 12-15 cm");
}

static void test_wall(void) {
    vrhc_init(NULL);
    float eye[3] = {5000, 1500, -2000}, pref[3] = {0.1f, 1.6f, -0.2f};
    VrHcMap m = mk_map(-180.0f, eye, pref);   /* theta+180 = 0: R = identidad */
    CHECK(m.valid && NEAR(m.R[0], 1, 1e-5f), "mapa");
    feed_wall(&m, 0.6f, 1);                     /* pared a 0,5 m del ancla (p_ref.x = 0,1) */
    VrHcOut o;
    float head[3] = {0.1f, 1.6f, -0.2f};
    vrhc_query(&m, head, 0.02f, &o);
    CHECK(!o.hit && o.corr_len < 1e-4f && o.fade == 0, "en reposo no hay corrección (corr=%.4f fade=%.2f)", o.corr_len, o.fade);
    /* se asoma 0,2 m hacia la pared: aún libre (0,13 de radio -> límite en x = 0,47) */
    head[0] = 0.1f + 0.2f; vrhc_query(&m, head, 0.02f, &o);
    CHECK(!o.hit && o.corr_len < 1e-4f, "0,2 m sin contacto (hit=%d corr=%.4f)", o.hit, o.corr_len);
    /* 0,6 m: atraviesa; se recorta en x = 0,6 - 0,13 */
    head[0] = 0.1f + 0.6f; vrhc_query(&m, head, 0.02f, &o);
    CHECK(o.hit, "choca");
    CHECK(NEAR(head[0] + o.corr[0], 0.6f - 0.13f, 0.005f), "posición recortada x=%.4f esperado 0.47", head[0] + o.corr[0]);
    CHECK(NEAR(o.corr[1], 0, 0.003f) && NEAR(o.corr[2], 0, 0.003f), "recorte solo en x (%.4f %.4f)", o.corr[1], o.corr[2]);
    CHECK(o.fade > 0.0f && o.fade < 1.0f, "fundido parcial %.3f", o.fade);
    /* mucho más allá: fundido total */
    head[0] = 0.1f + 1.5f; vrhc_query(&m, head, 0.02f, &o);
    CHECK(o.fade > 0.99f, "fundido total %.3f", o.fade);
    CHECK(head[0] + o.corr[0] < 0.6f - 0.12f, "no atraviesa nunca (x=%.3f)", head[0] + o.corr[0]);
    /* liberación progresiva */
    head[0] = 0.1f;
    float f0 = o.fade; vrhc_query(&m, head, 0.1f, &o);
    CHECK(o.fade < f0 && o.fade > 0, "liberación gradual %.3f -> %.3f", f0, o.fade);
    for (int i = 0; i < 20; i++) vrhc_query(&m, head, 0.1f, &o);
    CHECK(o.fade == 0 && o.corr_len < 1e-4f, "tras soltar vuelve a 0");
    /* deslizamiento: oblicuo (+x hacia la pared, +z libre): z avanza, x se queda en el límite */
    head[0] = 0.1f + 0.8f; head[2] = -0.2f + 0.5f; vrhc_query(&m, head, 0.02f, &o);
    CHECK(o.hit && NEAR(head[0] + o.corr[0], 0.47f, 0.01f) && NEAR(head[2] + o.corr[2], head[2], 0.02f), "desliza por la pared (x=%.3f z=%.3f)", head[0] + o.corr[0], head[2] + o.corr[2]);
    /* de un lado a otro de una pared fina de un salto: no se atraviesa */
    head[0] = 0.1f + 3.0f; head[2] = -0.2f; vrhc_query(&m, head, 0.02f, &o);
    CHECK(head[0] + o.corr[0] < 0.6f, "no hay túnel con un salto de 3 m");
}

static void test_overlap_start(void) {
    vrhc_init(NULL);
    float eye[3] = {0, 1500, 0}, pref[3] = {0, 1.6f, 0};
    VrHcMap m = mk_map(-180.0f, eye, pref);
    feed_wall(&m, 0.08f, 1);                    /* pared a 8 cm del ancla (< radio 13 cm): solapado desde el principio */
    VrHcOut o; float head[3] = {0, 1.6f, 0};
    vrhc_query(&m, head, 0.02f, &o);
    CHECK(!o.hit && o.corr_len < 1e-4f, "no empuja al estar solapado al empezar (corr=%.4f)", o.corr_len);
    head[0] = -0.3f; vrhc_query(&m, head, 0.02f, &o);
    CHECK(!o.hit && o.corr_len < 1e-4f, "alejarse es libre");
    head[0] = 0.04f; vrhc_query(&m, head, 0.02f, &o);
    CHECK(o.hit && head[0] + o.corr[0] <= 0.005f, "no se acerca más (x=%.4f)", head[0] + o.corr[0]);
    head[1] = 2.0f; head[2] = 0.5f; head[0] = 0; vrhc_query(&m, head, 0.02f, &o);
    CHECK(!o.hit, "moverse en paralelo a la pared solapada es libre");
}

static void test_rotated_map(void) {
    /* mismo escenario con otro giro/ojo: la corrección en espacio del usuario no cambia */
    for (int k = 0; k < 4; k++) {
        vrhc_init(NULL);
        float eye[3] = {-3000.0f + 900.0f * k, 1400, 2500.0f - 300.0f * k}, pref[3] = {0.3f * k, 1.55f, -0.4f};
        VrHcMap m = mk_map(37.0f + 83.0f * k, eye, pref);
        float t[18];
        float xw = pref[0] + 0.5f;
        float tt[18] = {xw, 0, pref[2] - 1, xw, 3, pref[2] - 1, xw, 3, pref[2] + 1,   xw, 0, pref[2] - 1, xw, 3, pref[2] + 1, xw, 0, pref[2] + 1};
        memcpy(t, tt, sizeof t);
        vrhc_feed_tris(&m, t, 2, NULL, 0);
        VrHcOut o; float head[3] = {pref[0] + 0.9f, pref[1], pref[2]};
        vrhc_query(&m, head, 0.02f, &o);
        CHECK(o.hit && NEAR(head[0] + o.corr[0], xw - 0.13f, 0.006f), "mapa girado %d: x=%.4f esperado %.4f", k, head[0] + o.corr[0], xw - 0.13f);
    }
}

static void test_body_and_ttl(void) {
    vrhc_init(NULL);
    float eye[3] = {0, 1500, 0}, pref[3] = {0, 1.6f, 0};
    VrHcMap m = mk_map(-180.0f, eye, pref);
    /* triángulo pequeño pegado a una articulación (cuerpo): se ignora; la misma geometría lejos de ellas, no */
    float joints[2][3] = {{0, 1300, 0}, {0, 1000, 0}};
    float t[9] = {0.25f, 1.5f, 0, 0.25f, 1.3f, 0.1f, 0.25f, 1.3f, -0.1f};
    float jw[2][3];
    /* las articulaciones están en unidades del motor (mundo): ojo (0,1500,0) <-> usuario (0,1.6,0): y_mundo = 1500 + 1000*(y-1.6) */
    for (int i = 0; i < 2; i++) { jw[i][0] = joints[i][0]; jw[i][1] = joints[i][1]; jw[i][2] = joints[i][2]; }
    int k = vrhc_feed_tris(&m, t, 1, (const float (*)[3])jw, 2);
    CHECK(k == 0 && vrhc_count() == 0, "triángulo del propio cuerpo ignorado (k=%d)", k);
    k = vrhc_feed_tris(&m, t, 1, NULL, 0);
    CHECK(k == 1 && vrhc_count() == 1, "sin articulaciones entra");
    /* vida: se refresca con la misma geometría, caduca sin alimentar */
    const VrHcCfg *c = vrhc_config();
    int ttl = c->ttl;
    float far_[9] = {10, 0, 10, 10, 1, 10, 11, 0, 10};
    for (int i = 0; i < ttl - 2; i++) vrhc_feed_tris(&m, t, 1, NULL, 0), vrhc_feed_tris(&m, far_, 1, NULL, 0);
    CHECK(vrhc_count() == 1, "refrescado sigue vivo (lejano descartado), n=%d", vrhc_count());
    for (int i = 0; i < ttl + 1; i++) vrhc_feed_tris(&m, far_, 1, NULL, 0);
    CHECK(vrhc_count() == 0, "caduca (n=%d)", vrhc_count());
    /* sin mapa válido o con vrhc desactivado no hace nada */
    VrHcMap bad; memset(&bad, 0, sizeof bad);
    VrHcOut o; float head[3] = {1, 1.6f, 0};
    CHECK(vrhc_feed_tris(&bad, t, 1, NULL, 0) == 0, "mapa inválido");
    vrhc_query(&bad, head, 0.02f, &o);
    CHECK(o.corr_len == 0, "consulta sin mapa: sin corrección");
}

static void test_scene_feed(void) {
    vrhc_init(NULL);
    float eye[3] = {0, 1500, 0}, pref[3] = {0, 1.6f, 0};
    VrHcMap m = mk_map(-180.0f, eye, pref);
    VrVtx v[12]; memset(v, 0, sizeof v);
    float wall[9] = {0.6f, 0, -1, 0.6f, 3, -1, 0.6f, 3, 1};
    for (int i = 0; i < 3; i++) { memcpy(v[i].pos, wall + 3 * i, 12); memcpy(v[3 + i].pos, wall + 3 * i, 12); memcpy(v[6 + i].pos, wall + 3 * i, 12); memcpy(v[9 + i].pos, wall + 3 * i, 12); v[9 + i].flags = VR_F_2D; }
    VrBatch b[4]; memset(b, 0, sizeof b);
    b[0].first = 0; b[0].count = 3; b[0].kind = 1;        /* semitransparente: no cuenta */
    b[1].first = 3; b[1].count = 3; b[1].kind = 2;        /* HUD: no cuenta */
    b[2].first = 6; b[2].count = 3; b[2].kind = 0;        /* opaco 3D: sí */
    b[3].first = 9; b[3].count = 3; b[3].kind = 0;        /* con VR_F_2D: no */
    VrScene sc; memset(&sc, 0, sizeof sc);
    sc.v = v; sc.nv = 12; sc.b = b; sc.nb = 4; sc.user_space = 1;
    int k = vrhc_feed_scene(&m, &sc, NULL, 0);
    CHECK(k == 1 && vrhc_count() == 1, "solo el opaco 3D (k=%d n=%d)", k, vrhc_count());
    sc.user_space = 0;
    CHECK(vrhc_feed_scene(&m, &sc, NULL, 0) == 0, "escena fuera del espacio del usuario: ignorada");
}

static void test_penetration_fade(void) {
    /* la geometría aparece DESPUÉS de que la cabeza ya está dentro (alimentación tardía): no hay escape por barrido, pero el fundido sube */
    vrhc_init(NULL);
    float eye[3] = {0, 1500, 0}, pref[3] = {0, 1.6f, 0};
    VrHcMap m = mk_map(-180.0f, eye, pref);
    feed_wall(&m, -0.02f, 1);                  /* pared a 2 cm DETRÁS del ancla (x<0); la cabeza se queda en x = 0,03 (1 cm al otro lado del plano... no: 5 cm) */
    VrHcOut o; float head[3] = {0.03f, 1.6f, 0};
    vrhc_query(&m, head, 0.02f, &o);
    CHECK(o.pen_m >= 0.0f && o.fade >= 0.0f, "consulta tardía estable");
    /* cabeza dibujada a 1 cm de una pared que el ancla tenía a 20 cm: penetración visible -> fundido */
    vrhc_init(NULL);
    VrHcMap m2 = mk_map(-180.0f, eye, pref);
    feed_wall(&m2, 0.2f, 1);
    float h2[3] = {0.2f - 0.01f, 1.6f, 0};
    float save = vrhc_config()->radius_m;
    (void)save;
    vrhc_query(&m2, h2, 0.02f, &o);
    CHECK(o.hit && h2[0] + o.corr[0] <= 0.2f - 0.12f, "recortado antes de la pared (x=%.3f)", h2[0] + o.corr[0]);
    CHECK(o.pen_m < 0.005f, "tras recortar no queda penetración (pen=%.4f)", o.pen_m);
}

static void test_perf(void) {
    vrhc_init(NULL);
    float eye[3] = {0, 1500, 0}, pref[3] = {0, 1.6f, 0};
    VrHcMap m = mk_map(-180.0f, eye, pref);
    /* 3000 triángulos aleatorios alrededor */
    srand(7);
    float *t = malloc(sizeof(float) * 9 * 3000);
    for (int i = 0; i < 3000; i++) {
        float cx = (rand() % 4000 - 2000) / 1000.0f, cy = (rand() % 3000) / 1000.0f, cz = (rand() % 4000 - 2000) / 1000.0f;
        for (int v = 0; v < 3; v++) { t[9 * i + 3 * v] = cx + (rand() % 400 - 200) / 1000.0f; t[9 * i + 3 * v + 1] = cy + (rand() % 400 - 200) / 1000.0f; t[9 * i + 3 * v + 2] = cz + (rand() % 400 - 200) / 1000.0f; }
    }
    vrhc_feed_tris(&m, t, 3000, NULL, 0);
    CHECK(vrhc_count() > 1000, "almacén con muchos triángulos (%d)", vrhc_count());
    VrHcOut o; float head[3] = {1.0f, 1.6f, 0.5f};
    for (int i = 0; i < 200; i++) { head[0] = 0.5f + 0.005f * i; vrhc_query(&m, head, 0.011f, &o); }
    CHECK(isfinite(o.corr_len), "corrección finita");
    free(t);
}

int main(void) {
    test_basics(); test_wall(); test_overlap_start(); test_rotated_map(); test_body_and_ttl(); test_scene_feed(); test_penetration_fade(); test_perf();
    printf("test_headcol: %d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
