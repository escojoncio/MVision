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
    CHECK(c0.enabled && c0.radius_m >= 0.18f && c0.radius_m <= 0.22f, "radio por defecto 18-22 cm (%.3f)", c0.radius_m);
    CHECK(c0.safe_m > 0.1f && c0.face_m >= c0.safe_m && c0.face_m <= c0.radius_m, "safe %.3f <= face %.3f <= radio %.3f; safe > plano cercano 0,1 m", c0.safe_m, c0.face_m, c0.radius_m);
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
    /* se asoma 0,2 m hacia la pared: aún libre (0,2 de radio -> límite en x = 0,4) */
    head[0] = 0.1f + 0.2f; vrhc_query(&m, head, 0.02f, &o);
    CHECK(!o.hit && o.corr_len < 1e-4f, "0,2 m sin contacto (hit=%d corr=%.4f)", o.hit, o.corr_len);
    /* 0,6 m: atraviesa; se recorta en x = 0,6 - radio */
    head[0] = 0.1f + 0.6f; vrhc_query(&m, head, 0.02f, &o);
    CHECK(o.hit, "choca");
    CHECK(NEAR(head[0] + o.corr[0], 0.6f - vrhc_config()->radius_m, 0.005f), "posición recortada x=%.4f esperado 0.40", head[0] + o.corr[0]);
    CHECK(NEAR(o.corr[1], 0, 0.003f) && NEAR(o.corr[2], 0, 0.003f), "recorte solo en x (%.4f %.4f)", o.corr[1], o.corr[2]);
    CHECK(o.fade > 0.99f && o.through > 0.99f, "la cabeza física atraviesa la pared: negro INMEDIATO en la misma consulta (fade=%.3f through=%.3f)", o.fade, o.through);
    /* sin atravesar, pero empujando 8 cm más allá del límite: recorta sin negro */
    vrhc_reset(); feed_wall(&m, 0.6f, 1);
    head[0] = 0.4f + 0.08f; vrhc_query(&m, head, 0.02f, &o);
    CHECK(o.hit && o.fade < 0.5f && NEAR(head[0] + o.corr[0], 0.4f, 0.005f), "empuje corto: recorte sin negro (fade=%.3f x=%.3f)", o.fade, head[0] + o.corr[0]);
    head[0] = 0.1f + 1.0f; vrhc_query(&m, head, 0.02f, &o);
    /* mucho más allá: fundido total */
    head[0] = 0.1f + 1.5f; vrhc_query(&m, head, 0.02f, &o);
    CHECK(o.fade > 0.99f, "fundido total %.3f", o.fade);
    CHECK(head[0] + o.corr[0] < 0.6f - 0.18f, "no atraviesa nunca (x=%.3f)", head[0] + o.corr[0]);
    /* liberación progresiva */
    head[0] = 0.1f;
    float f0 = o.fade; vrhc_query(&m, head, 0.1f, &o);
    CHECK(o.fade < f0 && o.fade > 0, "liberación gradual %.3f -> %.3f", f0, o.fade);
    for (int i = 0; i < 20; i++) vrhc_query(&m, head, 0.1f, &o);
    CHECK(o.fade == 0 && o.corr_len < 1e-4f, "tras soltar vuelve a 0");
    /* deslizamiento: oblicuo (+x hacia la pared, +z libre): z avanza, x se queda en el límite */
    head[0] = 0.1f + 0.8f; head[2] = -0.2f + 0.5f; vrhc_query(&m, head, 0.02f, &o);
    CHECK(o.hit && NEAR(head[0] + o.corr[0], 0.40f, 0.01f) && NEAR(head[2] + o.corr[2], head[2], 0.02f), "desliza por la pared (x=%.3f z=%.3f)", head[0] + o.corr[0], head[2] + o.corr[2]);
    /* de un lado a otro de una pared fina de un salto: no se atraviesa */
    head[0] = 0.1f + 3.0f; head[2] = -0.2f; vrhc_query(&m, head, 0.02f, &o);
    CHECK(head[0] + o.corr[0] < 0.6f, "no hay túnel con un salto de 3 m");
}

static void test_overlap_start(void) {
    vrhc_init(NULL);
    float eye[3] = {0, 1500, 0}, pref[3] = {0, 1.6f, 0};
    VrHcMap m = mk_map(-180.0f, eye, pref);
    const VrHcCfg *cf = vrhc_config();
    feed_wall(&m, 0.08f, 1);                    /* pared a 8 cm del ancla (< safe): el ojo del juego está pegado */
    VrHcOut o; float head[3] = {0, 1.6f, 0};
    vrhc_query(&m, head, 0.02f, &o);
    CHECK(o.pushed_m > 0.05f && o.clear_m >= cf->face_m - 0.005f, "se expulsa hasta face_m (clear=%.3f pushed=%.3f)", o.clear_m, o.pushed_m);
    CHECK(head[0] + o.corr[0] < 0.08f - cf->face_m + 0.006f, "la cámara se aparta de la pared (x=%.3f)", head[0] + o.corr[0]);
    CHECK(o.fade < 0.01f, "expulsión resuelta: sin negro (fade=%.3f)", o.fade);
    head[0] = -0.3f; vrhc_query(&m, head, 0.02f, &o);
    CHECK(o.clear_m >= cf->face_m - 0.005f && o.fade < 0.01f, "alejarse es libre");
    head[0] = 0.04f; vrhc_query(&m, head, 0.02f, &o);
    CHECK(o.clear_m >= cf->face_m - 0.005f && head[0] + o.corr[0] < 0.08f - cf->face_m + 0.006f, "no se acerca más (x=%.4f clear=%.3f)", head[0] + o.corr[0], o.clear_m);
    head[1] = 2.0f; head[2] = 0.5f; head[0] = 0; vrhc_query(&m, head, 0.02f, &o);
    CHECK(o.clear_m >= cf->face_m - 0.005f, "moverse en paralelo a la pared pegada: la cara nunca baja de face_m (clear=%.3f)", o.clear_m);
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
        CHECK(o.hit && NEAR(head[0] + o.corr[0], xw - vrhc_config()->radius_m, 0.006f), "mapa girado %d: x=%.4f esperado %.4f", k, head[0] + o.corr[0], xw - vrhc_config()->radius_m);
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
    CHECK(o.hit && h2[0] + o.corr[0] <= 0.2f - vrhc_config()->face_m + 0.006f, "recortado antes de la pared (x=%.3f)", h2[0] + o.corr[0]);
    CHECK(o.pen_m < 0.005f && o.clear_m >= vrhc_config()->face_m - 0.006f, "tras recortar no queda penetración (pen=%.4f clear=%.3f)", o.pen_m, o.clear_m);
}


/* ---- sólidos de prueba en el espacio del usuario (mapa identidad: R = I, ojo (5000,1500,-2000), p_ref (0.1,1.6,-0.2); 1 m = 1000 unidades) ---- */
static void quad_user(const VrHcMap *m, int axis, float sign, float dist, float half, const float c0[3], float *t18) {
    /* plano perpendicular a `axis` a `dist` (con signo) de c0, cuadrado de lado 2*half */
    (void)m;
    int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
    float p[4][3];
    for (int k = 0; k < 4; k++) {
        memcpy(p[k], c0, 12);
        p[k][axis] = c0[axis] + sign * dist;
        p[k][a1] = c0[a1] + ((k == 0 || k == 3) ? -half : half);
        p[k][a2] = c0[a2] + ((k == 0 || k == 1) ? -half : half);
    }
    int idx[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++) memcpy(t18 + 3 * i, p[idx[i]], 12);
}
static void to_world_id(const VrHcMap *m, const float p[3], float w[3]) {
    for (int i = 0; i < 3; i++) w[i] = m->eye[i] + m->upm * (p[i] - m->p_ref[i]);
}

static void test_six_directions(void) {
    float eye[3] = {5000, 1500, -2000}, pref[3] = {0.1f, 1.6f, -0.2f};
    VrHcMap m = mk_map(-180.0f, eye, pref);
    const char *nm[6] = {"+x", "-x", "+y (techo)", "-y (suelo)", "+z", "-z"};
    for (int d = 0; d < 6; d++) {
        for (int src = 0; src < 2; src++) {      /* 0 = lista de la GPU (espacio del usuario), 1 = escena nativa (mundo) */
            vrhc_init(NULL);
            const VrHcCfg *cf = vrhc_config();
            int axis = d / 2; float sign = (d & 1) ? -1.0f : 1.0f;
            float t[18], tw[18];
            quad_user(&m, axis, sign, 0.6f, 3.0f, pref, t);
            if (src == 0) vrhc_feed_tris(&m, t, 2, NULL, 0);
            else { for (int v = 0; v < 6; v++) to_world_id(&m, t + 3 * v, tw + 3 * v); vrhc_feed_world(tw, 2, 0); }
            VrHcOut o;
            float head[3] = {pref[0], pref[1], pref[2]};
            vrhc_query(&m, head, 0.011f, &o);
            CHECK(o.corr_len < 1e-3f && o.fade == 0, "%s src%d: en reposo libre", nm[d], src);
            /* 3 empujes: hasta tocar, más allá y a través (1,5 m) */
            float reach[3] = {0.55f, 0.75f, 1.5f};
            for (int s = 0; s < 3; s++) {
                vrhc_reset();
                if (src == 0) vrhc_feed_tris(&m, t, 2, NULL, 0);
                else vrhc_feed_world(tw, 2, 0);
                memcpy(head, pref, 12); head[axis] += sign * reach[s];
                vrhc_query(&m, head, 0.011f, &o);
                float fin = head[axis] + o.corr[axis];
                float dist_wall = 0.6f - sign * (fin - pref[axis]);   /* distancia del centro de la cabeza al plano */
                CHECK(dist_wall >= cf->radius_m - 0.006f, "%s src%d empuje %.2f: la cabeza queda a %.3f m del plano (>= radio %.2f)", nm[d], src, reach[s], dist_wall, cf->radius_m);
                CHECK(o.clear_m >= cf->face_m - 0.006f, "%s src%d empuje %.2f: cara a %.3f m de un sólido (>= %.2f)", nm[d], src, reach[s], o.clear_m, cf->face_m);
                if (s == 2) CHECK(o.fade > 0.99f, "%s src%d: atravesar la pared funde a negro de inmediato (%.3f)", nm[d], src, o.fade);
            }
        }
    }
}

static void test_native_360(void) {
    /* La lista de la GPU solo ve DELANTE; la escena nativa trae además lo de ATRÁS: sin la nativa la cabeza atravesaría la pared trasera. */
    float eye[3] = {5000, 1500, -2000}, pref[3] = {0.1f, 1.6f, -0.2f};
    VrHcMap m = mk_map(-180.0f, eye, pref);
    vrhc_init(NULL);
    float front[18], back[18], bw[18];
    quad_user(&m, 0, 1.0f, 0.6f, 3.0f, pref, front);
    quad_user(&m, 0, -1.0f, 0.6f, 3.0f, pref, back);
    vrhc_feed_tris(&m, front, 2, NULL, 0);
    VrHcOut o; float head[3] = {pref[0] - 0.9f, pref[1], pref[2]};
    vrhc_query(&m, head, 0.011f, &o);
    CHECK(!o.native_live && head[0] + o.corr[0] < pref[0] - 0.7f, "solo GPU: la pared de atrás no existe (atraviesa x=%.3f) -- el hueco que cubre la escena nativa", head[0] + o.corr[0]);
    for (int v = 0; v < 6; v++) to_world_id(&m, back + 3 * v, bw + 3 * v);
    CHECK(vrhc_feed_world(bw, 2, 0) == 2 && vrhc_native_count() == 2, "alimenta la lista nativa");
    vrhc_query(&m, head, 0.011f, &o);
    CHECK(o.native_live && head[0] + o.corr[0] >= pref[0] - 0.6f + vrhc_config()->radius_m - 0.006f, "con la nativa la pared de atrás frena (x=%.3f)", head[0] + o.corr[0]);
    CHECK(o.fade > 0.99f, "y funde a negro (%.3f)", o.fade);
    /* la lista nativa se REEMPLAZA (sin fantasmas) */
    vrhc_reset(); vrhc_feed_tris(&m, front, 2, NULL, 0);
    vrhc_feed_world(bw, 2, 0);
    float far_[9] = {9000, 0, 9000, 9000, 10, 9000, 9100, 0, 9000};
    vrhc_feed_world(far_, 1, 0);
    head[0] = pref[0] - 0.9f; vrhc_query(&m, head, 0.011f, &o);
    CHECK(o.clear_m > 1.0f && head[0] + o.corr[0] < pref[0] - 0.7f, "la lista nativa nueva sustituye a la anterior");
    /* caduca sin refrescar (la nativa deja de llegar: modo gpu, escena no apta): vuelve a la lista de la GPU */
    vrhc_feed_world(bw, 2, 0);
    for (int i = 0; i < 120; i++) vrhc_query(&m, (float[3]){pref[0], pref[1], pref[2]}, 0.011f, &o);
    CHECK(!o.native_live, "la lista nativa caduca a ~1 s sin refresco");
}

static void test_fade_and_release(void) {
    float eye[3] = {5000, 1500, -2000}, pref[3] = {0.1f, 1.6f, -0.2f};
    VrHcMap m = mk_map(-180.0f, eye, pref);
    vrhc_init(NULL);
    float t[18]; quad_user(&m, 2, 1.0f, 0.5f, 3.0f, pref, t);
    vrhc_feed_tris(&m, t, 2, NULL, 0);
    VrHcOut o; float head[3] = {pref[0], pref[1], pref[2]};
    vrhc_query(&m, head, 0.011f, &o);
    CHECK(o.fade == 0, "libre");
    head[2] = pref[2] + 0.65f; vrhc_query(&m, head, 0.011f, &o);   /* un solo fotograma de entrada en el sólido */
    CHECK(o.fade > 0.99f, "negro inmediato al entrar en el sólido (%.3f)", o.fade);
    head[2] = pref[2]; vrhc_query(&m, head, 0.011f, &o);
    CHECK(o.fade > 0.9f, "al salir se mantiene un instante (liberación, no parpadeo): %.3f", o.fade);
    for (int i = 0; i < 40; i++) vrhc_query(&m, head, 0.011f, &o);
    CHECK(o.fade == 0, "y se libera del todo (%.3f)", o.fade);
    /* hueco de seguridad: más candidatos de los que caben -> negro, nunca ver a través */
    vrhc_init(NULL);
    int n = HC_NAT_MAX;
    float *w = malloc(sizeof(float) * 9 * (size_t)(n + 10));
    for (int i = 0; i < n + 10; i++) { float x = 5000 + (float)(i % 100), y = 1500 + (float)(i / 100 % 100) * 0.5f; float tt[9] = {x, y, -2000, x + 1, y, -2000, x, y + 1, -2000}; memcpy(w + 9 * i, tt, 36); }
    vrhc_feed_world(w, n + 10, 0);
    free(w);
    memcpy(head, pref, 12); vrhc_query(&m, head, 0.011f, &o);
    CHECK(o.overflow && o.fade > 0.99f, "desbordamiento de la lista nativa: negro (ov=%d fade=%.2f)", o.overflow, o.fade);
    /* lista truncada por el recolector (el búfer se llenó): ante la duda, negro */
    vrhc_init(NULL);
    float one[9] = {5000, 1500, -1000, 5001, 1500, -1000, 5000, 1501, -1000};
    vrhc_feed_world(one, 1, 1);
    memcpy(head, pref, 12); vrhc_query(&m, head, 0.011f, &o);
    CHECK(o.overflow && o.fade > 0.99f, "lista truncada: negro (ov=%d fade=%.2f)", o.overflow, o.fade);
    vrhc_feed_world(one, 1, 0);
    for (int i = 0; i < 40; i++) vrhc_query(&m, head, 0.011f, &o);
    CHECK(!o.overflow && o.fade == 0, "lista completa: sin negro");
}

static void test_min_face_distance(void) {
    /* habitación cerrada de 2 x 2.6 x 2 m (6 paredes); la cabeza se mueve al azar 3000 pasos con saltos grandes: la cara nunca baja de la distancia mínima ni sale */
    float eye[3] = {5000, 1500, -2000}, pref[3] = {0.1f, 1.6f, -0.2f};
    VrHcMap m = mk_map(-180.0f, eye, pref);
    for (int pass = 0; pass < 2; pass++) {
        vrhc_init(NULL);
        const VrHcCfg *cf = vrhc_config();
        float c0[3] = {pref[0], pref[1], pref[2]};
        float room[6][18]; float tw[18];
        float half[6] = {1.0f, 1.0f, 1.3f, 1.3f, 1.0f, 1.0f};
        for (int d = 0; d < 6; d++) {
            quad_user(&m, d / 2, (d & 1) ? -1.0f : 1.0f, half[d], 2.0f, c0, room[d]);
            if (pass == 0) vrhc_feed_tris(&m, room[d], 2, NULL, 0);
        }
        if (pass == 1) {   /* misma habitación pero pegada a una pared: el ancla a 5 cm de la pared +x */
            for (int d = 0; d < 6; d++) { quad_user(&m, d / 2, (d & 1) ? -1.0f : 1.0f, d == 0 ? 0.05f : half[d], 2.0f, c0, room[d]); }
        }
        float *all = malloc(sizeof(float) * 18 * 6);
        for (int d = 0; d < 6; d++) for (int v = 0; v < 6; v++) to_world_id(&m, room[d] + 3 * v, all + 18 * d + 3 * v);
        vrhc_reset();
        vrhc_feed_world(all, 12, 0);
        (void)tw;
        srand(123 + pass);
        VrHcOut o;
        float worst = 9.0f; int outside = 0, fade_bad = 0;
        for (int i = 0; i < 3000; i++) {
            float head[3] = {c0[0] + ((rand() % 4000) - 2000) / 1000.0f, c0[1] + ((rand() % 4000) - 2000) / 1000.0f, c0[2] + ((rand() % 4000) - 2000) / 1000.0f};
            if (i % 4 == 0) vrhc_feed_world(all, 12, 0);   /* la escena nativa llega cada pocos fotogramas */
            vrhc_query(&m, head, 0.011f, &o);
            float f[3] = {head[0] + o.corr[0] - c0[0], head[1] + o.corr[1] - c0[1], head[2] + o.corr[2] - c0[2]};
            if (o.clear_m < worst) worst = o.clear_m;
            if (f[0] > (pass ? 0.05f : 1.0f) || f[0] < -1.0f || f[1] > 1.3f || f[1] < -1.3f || fabsf(f[2]) > 1.0f) outside++;
            if (o.clear_m < cf->safe_m && o.fade < 0.5f) fade_bad++;
        }
        free(all);
        CHECK(worst >= cf->face_m - 0.01f, "pase %d: distancia mínima de la cara en 3000 pasos = %.3f m (>= %.2f)", pass, worst, cf->face_m);
        CHECK(outside == 0, "pase %d: la cabeza dibujada nunca sale de la habitación (%d fuera)", pass, outside);
        CHECK(fade_bad == 0, "pase %d: nunca a menos de safe sin negro", pass);
    }
    /* pasillo estrecho (2 x 13 cm): la cara no puede quedar a salvo -> negro, nunca ver a través */
    vrhc_init(NULL);
    float t1[18], t2[18];
    quad_user(&m, 0, 1.0f, 0.10f, 3.0f, pref, t1); quad_user(&m, 0, -1.0f, 0.10f, 3.0f, pref, t2);
    vrhc_feed_tris(&m, t1, 2, NULL, 0); vrhc_feed_tris(&m, t2, 2, NULL, 0);
    VrHcOut o; float head[3] = {pref[0], pref[1], pref[2]};
    vrhc_query(&m, head, 0.011f, &o);
    CHECK(o.clear_m < vrhc_config()->safe_m && o.fade > 0.5f, "pasillo de 20 cm: imposible mantener la distancia -> negro (clear=%.3f fade=%.2f)", o.clear_m, o.fade);
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
    test_basics(); test_wall(); test_overlap_start(); test_rotated_map(); test_body_and_ttl(); test_scene_feed(); test_penetration_fade(); test_six_directions(); test_native_360(); test_fade_and_release(); test_min_face_distance(); test_perf();
    printf("test_headcol: %d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
