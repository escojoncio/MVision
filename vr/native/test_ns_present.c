/* Pruebas sintéticas: presentación del mundo siempre nativo (repetir el último válido / fundir a negro) y cinemáticas con el render nativo desde la cámara del juego. */
#include "ns_present.h"
#include "ns_cinema.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_ok, g_fail;
#define CHECK(c, ...) do { if (c) g_ok++; else { g_fail++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define MS 1000000ull

static NsPresentIn good(uint64_t now) { NsPresentIn i = { 1, 1, 1, 1, 1, now - 5 * MS, now }; return i; }

static void test_present(void) {
    NsPresentState st; ns_present_reset(&st);
    NsPresentCfg cfg = ns_present_cfg_default();
    NsPresentOut o;
    NsPresentIn bad;
    /* sin ningún fotograma válido todavía: negro (nunca una ruta de GPU) */
    memset(&bad, 0, sizeof bad); bad.now_ns = 1000 * MS;
    CHECK(ns_present_step(&st, &cfg, &bad, &o) == NS_PR_BLACK && o.fade == 0.0f, "sin historia: negro");
    /* válido */
    NsPresentIn g = good(2000 * MS);
    CHECK(ns_present_step(&st, &cfg, &g, &o) == NS_PR_CURRENT && o.fade == 1.0f && o.why == 0, "válido: actual");
    /* deja de ser válido: se repite el último válido */
    NsPresentIn b = good(2100 * MS); b.meshes_ok = 0;
    CHECK(ns_present_step(&st, &cfg, &b, &o) == NS_PR_HOLD && o.fade == 1.0f && (o.why & NS_PRW_MESHES), "mallas ausentes: repetir");
    b = good(2200 * MS); b.scene_fit = 0;
    CHECK(ns_present_step(&st, &cfg, &b, &o) == NS_PR_HOLD && (o.why & NS_PRW_UNFIT), "escena no apta: repetir");
    b = good(2300 * MS); b.renderer_ok = 0;
    CHECK(ns_present_step(&st, &cfg, &b, &o) == NS_PR_HOLD && (o.why & NS_PRW_NO_RENDERER), "renderer sin listo: repetir");
    b = good(2400 * MS); b.have_scene = 0;
    CHECK(ns_present_step(&st, &cfg, &b, &o) == NS_PR_HOLD && (o.why & NS_PRW_NO_SCENE), "sin escena: repetir");
    /* la incoherencia con la lista y una escena vieja solo se REGISTRAN: se sigue dibujando la actual */
    NsPresentIn inc = good(2500 * MS); inc.coherent = 0;
    CHECK(ns_present_step(&st, &cfg, &inc, &o) == NS_PR_CURRENT && (o.why & NS_PRW_INCOHERENT) && o.fade == 1.0f, "incoherente: solo registro");
    NsPresentIn old = good(3000 * MS); old.scene_t_ns = 3000 * MS - 800 * MS;
    CHECK(ns_present_step(&st, &cfg, &old, &o) == NS_PR_CURRENT && (o.why & NS_PRW_STALE), "vieja: solo registro");
    /* fallo prolongado: repetir hasta hold_ns, luego fundido suave y negro */
    uint64_t t0 = 3000 * MS;
    float prev = 1.0f; int monotone = 1;
    for (uint64_t dt = 50; dt < 12000; dt += 50) {
        NsPresentIn x = good(t0 + dt * MS); x.meshes_ok = 0;
        int m = ns_present_step(&st, &cfg, &x, &o);
        CHECK(m == NS_PR_HOLD || m == NS_PR_BLACK, "modo %d", m);
        if (o.fade > prev + 1e-6f) monotone = 0;
        prev = o.fade;
        if (dt <= 9300) CHECK(o.mode == NS_PR_HOLD && o.fade == 1.0f, "dentro de la espera dt=%llu", (unsigned long long)dt);
        if (dt > 9300 && dt < 10000) CHECK(o.mode == NS_PR_HOLD && o.fade > 0.0f && o.fade < 1.0f, "fundiendo dt=%llu fade=%f", (unsigned long long)dt, o.fade);
        if (dt >= 10000) CHECK(o.mode == NS_PR_BLACK && o.fade == 0.0f, "negro dt=%llu", (unsigned long long)dt);
    }
    CHECK(monotone, "el fundido solo baja");
    /* vuelve a ser válido: imagen entera de nuevo, sin saltos de estado */
    NsPresentIn r = good(t0 + 14000 * MS);
    CHECK(ns_present_step(&st, &cfg, &r, &o) == NS_PR_CURRENT && o.fade == 1.0f, "recupera");
    CHECK(st.n_current > 0 && st.n_hold > 0 && st.n_black > 0, "contadores");
}

/* Barrido exhaustivo de entradas: el resultado SIEMPRE es actual, repetir o negro; jamás otra cosa (no existe «GPU»). */
static void test_exhaustive(void) {
    NsPresentCfg cfg = ns_present_cfg_default();
    int bad = 0, n = 0;
    for (int mask = 0; mask < 32; mask++) {
        for (int hist = 0; hist < 2; hist++) {
            for (uint64_t el = 0; el < 4000; el += 97) {
                NsPresentState st; ns_present_reset(&st);
                NsPresentOut o;
                if (hist) { NsPresentIn g = good(10000 * MS); ns_present_step(&st, &cfg, &g, &o); }
                NsPresentIn in; in.renderer_ok = mask & 1; in.have_scene = (mask >> 1) & 1; in.scene_fit = (mask >> 2) & 1; in.meshes_ok = (mask >> 3) & 1; in.coherent = (mask >> 4) & 1;
                in.now_ns = 10000 * MS + el * MS; in.scene_t_ns = in.now_ns - 20 * MS;
                int m = ns_present_step(&st, &cfg, &in, &o);
                n++;
                if (m != NS_PR_CURRENT && m != NS_PR_HOLD && m != NS_PR_BLACK) bad++;
                if (o.fade < 0.0f || o.fade > 1.0f || o.mode != m) bad++;
                if (!hist && m != NS_PR_CURRENT && m != NS_PR_BLACK) bad++;      /* sin historia nunca se repite nada */
                if (m == NS_PR_CURRENT && ((mask & 15) != 15)) bad++;           /* actual solo con todo listo */
            }
        }
    }
    CHECK(bad == 0 && n > 2000, "barrido: %d combinaciones, %d erróneas", n, bad);
    CHECK(ns_present_step(NULL, NULL, NULL, NULL) == NS_PR_BLACK, "todo NULL: negro");
}

/* ---- cinemáticas ---- */
static void mat_vec(const float m[16], const float p[3], float o[4]) {
    for (int r = 0; r < 4; r++) o[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r];
}
static void xf34(const float m[12], const float p[3], float o[3]) { for (int r = 0; r < 3; r++) o[r] = m[4 * r] * p[0] + m[4 * r + 1] * p[1] + m[4 * r + 2] * p[2] + m[4 * r + 3]; }

static void test_cinema(void) {
    /* cámara del motor con un giro arbitrario (yaw 30 grados, cabeceo 10) y su posición; gv = escala 0,001 con orientación y traslación cualesquiera */
    NsCamera cam; memset(&cam, 0, sizeof cam);
    float cy = cosf(0.52f), sy = sinf(0.52f), cp = cosf(0.17f), sp = sinf(0.17f);
    /* columnas = X der, Y abajo, Z delante en el mundo del motor (rotación propia) */
    float ax[3] = { cy, 0, -sy }, az0[3] = { sy, 0, cy };
    float az[3] = { az0[0] * cp, -sp, az0[2] * cp };
    float ay[3] = { az[1] * ax[2] - az[2] * ax[1], az[2] * ax[0] - az[0] * ax[2], az[0] * ax[1] - az[1] * ax[0] };   /* z x x = y (según el convenio, solo importa que sea ortonormal) */
    for (int r = 0; r < 3; r++) { cam.rot[r * 3 + 0] = ax[r]; cam.rot[r * 3 + 1] = ay[r]; cam.rot[r * 3 + 2] = az[r]; }
    cam.eye[0] = 1500; cam.eye[1] = -800; cam.eye[2] = 4200; cam.h = 320; cam.overscan_y = NS_OVERSCAN_Y;
    /* game_view: vista (unidades) -> VR (m): giro Y de 90 grados, escala 0,001, traslación (2, 1.5, -3) */
    float gv[16] = {0};
    gv[0] = 0; gv[2] = -0.001f; gv[1] = 0;            /* columna 0: x de la vista -> -z VR */
    gv[5] = -0.001f;                                   /* columna 1: y de la vista (abajo) -> -y VR (arriba) */
    gv[8] = 0.001f;                                    /* columna 2: z de la vista (adelante) -> +x VR */
    gv[12] = 2; gv[13] = 1.5f; gv[14] = -3; gv[15] = 1;
    float tv[12];
    CHECK(ns_cinema_to_vr(&cam, gv, tv) == 1, "to_vr");
    /* un punto en el mundo del motor delante de la cámara: view = S R^T (p - eye); H = 320 */
    const float H = cam.h;
    const float tests[4][2] = { { 0, 0 }, { 60, -40 }, { -120, 80 }, { 150, 100 } };   /* píxeles del motor respecto al centro (x der, y abajo) */
    const float conv = 3.0f, ipd = 0.064f, tanx = 160.0f / H * 1.0f, tany = 120.0f / H;
    for (int k = 0; k < 4; k++) {
        /* punto en el plano de convergencia: z de la vista = conv / 0.001 unidades. Sus píxeles de motor son (px, py) -> vx = px*z/H, vy = py*z/(H*ys) */
        float z = conv / 0.001f;
        float v[3] = { tests[k][0] * z / H, tests[k][1] * z / (H * NS_OVERSCAN_Y), z };
        float p[3];
        for (int r = 0; r < 3; r++) p[r] = cam.eye[r] + cam.rot[r * 3 + 0] * v[0] + cam.rot[r * 3 + 1] * v[1] + cam.rot[r * 3 + 2] * v[2];
        float w[3]; xf34(tv, p, w);
        for (int eye = 0; eye < 2; eye++) {
            float view[16], tan4[4], P[16];
            CHECK(ns_cinema_eye(gv, eye, ipd, conv, tanx, tany, view, tan4) == 1, "eye");
            ns_cinema_projection(tan4, 0.05f, 800.0f, P);
            float e[4], c[4];
            mat_vec(view, w, e);
            for (int r = 0; r < 4; r++) c[r] = P[r] * e[0] + P[4 + r] * e[1] + P[8 + r] * e[2] + P[12 + r] * e[3];
            float ndcx = c[0] / c[3], ndcy = c[1] / c[3];
            /* pantalla del motor: 320x240 -> ndc x = px/160, ndc y = -py/120 (GL arriba) */
            CHECK(fabsf(ndcx - tests[k][0] / 160.0f) < 2e-3f && fabsf(ndcy + tests[k][1] / 120.0f) < 2e-3f,
                  "ojo %d punto %d: ndc (%f, %f) esperado (%f, %f)", eye, k, ndcx, ndcy, tests[k][0] / 160.0f, -tests[k][1] / 120.0f);
            CHECK(c[3] > 0, "delante");
        }
    }
    /* H REAL y centro OFX/OFY de la escena (no los 320/160/120 fijos): ventana descentrada, el píxel X = OFX + x_rel cae en ndc (X-160)/160 */
    {
        NsCamera c2 = cam; c2.h = 256.0f; c2.ofx = 180.0f; c2.ofy = 100.0f;
        float tx2, ty2, sh[4];
        CHECK(ns_cinema_window(&c2, &tx2, &ty2, sh) == 1 && fabsf(tx2 - 160.0f / 256.0f) < 1e-6f && fabsf(ty2 - 120.0f / 256.0f) < 1e-6f, "ventana con H real");
        CHECK(fabsf(sh[0] - 20.0f / 256.0f) < 1e-6f && fabsf(sh[1] + 20.0f / 256.0f) < 1e-6f && fabsf(sh[2] + 20.0f / 256.0f) < 1e-6f && fabsf(sh[3] - 20.0f / 256.0f) < 1e-6f, "desplazamiento OFX/OFY");
        NsCamera c3 = cam; c3.ofx = 0; c3.ofy = 0;
        CHECK(ns_cinema_window(&c3, &tx2, &ty2, sh) == 1 && sh[0] == 0 && sh[2] == 0, "OFX/OFY desconocidos = centro");
        c3.h = 0; CHECK(ns_cinema_window(&c3, &tx2, &ty2, sh) == 0, "H inválida rechazada");
        ns_cinema_window(&c2, &tx2, &ty2, sh);
        for (int k = 0; k < 4; k++) {
            float z = conv / 0.001f, H2 = c2.h;
            float v[3] = { tests[k][0] * z / H2, tests[k][1] * z / (H2 * NS_OVERSCAN_Y), z }, p[3];
            for (int r = 0; r < 3; r++) p[r] = cam.eye[r] + cam.rot[r * 3 + 0] * v[0] + cam.rot[r * 3 + 1] * v[1] + cam.rot[r * 3 + 2] * v[2];
            float w[3]; xf34(tv, p, w);
            float view[16], tan4[4], P[16], e[4], c[4];
            ns_cinema_eye(gv, 0, ipd, conv, tx2, ty2, view, tan4);
            for (int i = 0; i < 4; i++) tan4[i] += sh[i];
            ns_cinema_projection(tan4, 0.05f, 800.0f, P);
            mat_vec(view, w, e);
            for (int r = 0; r < 4; r++) c[r] = P[r] * e[0] + P[4 + r] * e[1] + P[8 + r] * e[2] + P[12 + r] * e[3];
            float X = c2.ofx + tests[k][0], Y = c2.ofy + tests[k][1];
            CHECK(fabsf(c[0] / c[3] - (X - 160.0f) / 160.0f) < 2e-3f && fabsf(c[1] / c[3] + (Y - 120.0f) / 120.0f) < 2e-3f, "ventana descentrada punto %d", k);
        }
    }
    /* el punto fuera del plano de convergencia tiene paralaje entre ojos; la altura es idéntica */
    float zf = 9.0f / 0.001f, vf[3] = { 0, 0, zf }, pf[3];
    for (int r = 0; r < 3; r++) pf[r] = cam.eye[r] + cam.rot[r * 3 + 2] * vf[2];
    float wf[3]; xf34(tv, pf, wf);
    float nd[2][2];
    for (int eye = 0; eye < 2; eye++) {
        float view[16], tan4[4], P[16], e[4], c[4];
        ns_cinema_eye(gv, eye, ipd, conv, tanx, tany, view, tan4);
        ns_cinema_projection(tan4, 0.05f, 800.0f, P);
        mat_vec(view, wf, e);
        for (int r = 0; r < 4; r++) c[r] = P[r] * e[0] + P[4 + r] * e[1] + P[8 + r] * e[2] + P[12 + r] * e[3];
        nd[eye][0] = c[0] / c[3]; nd[eye][1] = c[1] / c[3];
    }
    CHECK(fabsf(nd[0][1] - nd[1][1]) < 1e-5f && nd[0][0] != nd[1][0], "paralaje horizontal, misma altura");
    CHECK(nd[0][0] < nd[1][0], "lo lejano queda detrás de la pantalla (ojo izquierdo más a la izquierda) %f %f", nd[0][0], nd[1][0]);
    /* sin cámara válida o con datos no finitos: rechaza */
    float bad[16]; memcpy(bad, gv, sizeof bad); bad[3] = NAN;
    float o12[12], v16[16], t4[4];
    CHECK(ns_cinema_to_vr(&cam, bad, o12) == 0 && ns_cinema_eye(bad, 0, ipd, conv, tanx, tany, v16, t4) == 0, "NaN rechazado");
    float sing[16] = {0};
    CHECK(ns_cinema_to_vr(&cam, sing, o12) == 0, "singular rechazado");
    CHECK(ns_cinema_eye(gv, 0, -0.01f, conv, tanx, tany, v16, t4) == 0 && ns_cinema_eye(gv, 0, NAN, conv, tanx, tany, v16, t4) == 0, "ipd inválido");
    /* pantalla de cine 3D (mono): IPD 0 es válido y ambos ojos dan exactamente la misma vista y tangentes */
    {
        float vl[16], vr[16], tl[4], tr[4];
        CHECK(ns_cinema_eye(gv, 0, 0.0f, conv, tanx, tany, vl, tl) == 1 && ns_cinema_eye(gv, 1, 0.0f, conv, tanx, tany, vr, tr) == 1, "ipd 0 válido (mono)");
        CHECK(memcmp(vl, vr, sizeof vl) == 0 && memcmp(tl, tr, sizeof tl) == 0 && tl[0] == tanx && tl[1] == tanx, "mono: ojos idénticos");
    }
}

/* ---- ventana estéreo de la pantalla 3D: separación y convergencia según la escena, con el paralaje acotado ---- */
static float ndc_x(const float gv[16], int eye, float b, float conv, float tx, float ty, float zm) {   /* punto en el eje de la cámara a zm metros */
    float view[16], tan4[4], P[16], e[4], c[4];
    const float w[3] = { gv[12], gv[13], gv[14] - zm };
    ns_cinema_eye(gv, eye, b, conv, tx, ty, view, tan4);
    ns_cinema_projection(tan4, 0.05f, 800.0f, P);
    mat_vec(view, w, e);
    for (int r = 0; r < 4; r++) c[r] = P[r] * e[0] + P[4 + r] * e[1] + P[8 + r] * e[2] + P[12 + r] * e[3];
    return c[0] / c[3];
}

static void test_stereo(void) {
    float gv[16] = {0};
    gv[0] = 0.001f; gv[5] = -0.001f; gv[10] = -0.001f; gv[15] = 1; gv[12] = 0.5f; gv[13] = 1.7f; gv[14] = -0.2f;   /* cámara del juego mirando a -Z VR */
    const float ipd = 0.063f, RAD = 57.29578f;
    const float hs[3] = { 320.0f, 1500.0f, 180.0f };                       /* normal (53 grados), teleobjetivo (12), angular (83) */
    const float dz[5][2] = { { 0.6f, 1.0f }, { 1.2f, 2.0f }, { 4.0f, 10.0f }, { 0.0f, 0.0f }, { 0.2f, 3.0f } };   /* (cercano, sujeto) m; 0 = desconocido */
    const float scr[3][2] = { { 3.0f, 2.5f }, { 3.0f, 8.0f }, { 8.0f, 2.0f } };   /* pantalla: ancho, distancia (m) */
    for (int s = 0; s < 3; s++) for (int h = 0; h < 3; h++) for (int k = 0; k < 5; k++) {
        const float W = scr[s][0], D = scr[s][1], ts = W / (2 * D), tx = 160.0f / hs[h], ty = 120.0f / hs[h];
        const float lim_pos = fminf(1.0f / RAD, ipd / D), lim_neg = 1.25f / RAD;
        NsCinemaStereo st;
        CHECK(ns_cinema_stereo(ipd, dz[k][0], dz[k][1], tx, ts, D, &st) == 1, "estéreo válido");
        CHECK(st.b > 0.0f && st.b <= ipd + 1e-7f, "separación en (0, IPD real]: %g", st.b);
        CHECK(dz[k][1] > 0 ? fabsf(st.conv - dz[k][1]) < 1e-5f : fabsf(st.conv - 3.0f) < 1e-5f, "convergencia en el sujeto: %g", st.conv);
        /* plano de convergencia: el mismo píxel en ambos ojos */
        float d0 = ndc_x(gv, 1, st.b, st.conv, tx, ty, st.conv) - ndc_x(gv, 0, st.b, st.conv, tx, ty, st.conv);
        CHECK(fabsf(d0) * 640.0f < 0.02f, "sujeto sin paralaje: %g px", d0 * 640.0f);
        /* infinito: paralaje positivo dentro del límite y sin divergencia (desplazamiento en la pantalla <= IPD real) */
        float di = ndc_x(gv, 1, st.b, st.conv, tx, ty, 700.0f) - ndc_x(gv, 0, st.b, st.conv, tx, ty, 700.0f);
        CHECK(di > 0 && di * ts <= lim_pos * 1.01f, "infinito: %g grados (límite %g) W=%g D=%g H=%g caso %d", di * ts * RAD, lim_pos * RAD, W, D, hs[h], k);
        CHECK(di * 0.5f * W <= ipd * 1.01f, "sin divergencia: %g m en pantalla", di * 0.5f * W);
        CHECK(fabsf(st.par_max - di * ts * RAD) < 0.02f, "par_max = paralaje medido: %g / %g", st.par_max, di * ts * RAD);
        /* lo más cercano: paralaje negativo dentro del límite */
        if (dz[k][0] > 0) {
            float dn = ndc_x(gv, 1, st.b, st.conv, tx, ty, dz[k][0]) - ndc_x(gv, 0, st.b, st.conv, tx, ty, dz[k][0]);
            CHECK(dn < 0 && dn * ts >= -lim_neg * 1.01f, "cercano: %g grados (límite -%g)", dn * ts * RAD, lim_neg * RAD);
            CHECK(fabsf(st.par_min - dn * ts * RAD) < 0.02f, "par_min = paralaje medido: %g / %g", st.par_min, dn * ts * RAD);
        } else CHECK(st.par_min == 0.0f, "sin cercano conocido: par_min 0");
        /* simetría izquierda/derecha y marco idéntico (la misma ventana de 2*tx de ancho en ambos ojos: la misma textura sobre el mismo recuadro) */
        float vl[16], vr[16], tl[4], tr[4];
        ns_cinema_eye(gv, 0, st.b, st.conv, tx, ty, vl, tl); ns_cinema_eye(gv, 1, st.b, st.conv, tx, ty, vr, tr);
        CHECK(tl[0] == tr[1] && tl[1] == tr[0] && tl[2] == tr[2] && tl[3] == tr[3], "frustos simétricos");
        CHECK(fabsf(tl[0] + tl[1] - 2 * tx) < 1e-6f && fabsf(tr[0] + tr[1] - 2 * tx) < 1e-6f, "marco idéntico");
        CHECK(fabsf((vl[12] - vr[12]) - st.b) < 1e-6f, "cámaras separadas b: %g / %g", vl[12] - vr[12], st.b);
        float xl = ndc_x(gv, 0, st.b, st.conv, tx, ty, 700.0f), xr = ndc_x(gv, 1, st.b, st.conv, tx, ty, 700.0f);
        CHECK(fabsf(xl + xr) < 1e-5f, "disparidad repartida por igual entre los ojos");
    }
    /* los parámetros anteriores (b = IPD, C = 3 m fijos) incumplían los límites: primer plano a 0,8 m con H = 320 y teleobjetivo (H = 1500) en el infinito, pantalla 3 m a 2,5 m */
    {
        float dn = ndc_x(gv, 1, ipd, 3.0f, 0.5f, 0.375f, 0.8f) - ndc_x(gv, 0, ipd, 3.0f, 0.5f, 0.375f, 0.8f);
        float di = ndc_x(gv, 1, ipd, 3.0f, 160.0f / 1500.0f, 120.0f / 1500.0f, 700.0f) - ndc_x(gv, 0, ipd, 3.0f, 160.0f / 1500.0f, 120.0f / 1500.0f, 700.0f);
        CHECK(dn * 0.6f * RAD < -3.0f && di * 1.5f > 4.0f * ipd, "referencia del fallo: %g grados cruzados, %g m de divergencia", dn * 0.6f * RAD, di * 1.5f);
    }
    NsCinemaStereo st;
    CHECK(ns_cinema_stereo(0.0f, 1.0f, 2.0f, 0.5f, 0.6f, 2.5f, &st) == 1 && st.b == 0.0f && st.par_max == 0.0f && st.par_min == 0.0f, "mono: separación 0");
    CHECK(ns_cinema_stereo(ipd, 3.0f, 2.0f, 0.5f, 0.6f, 2.5f, &st) == 1 && st.par_min == 0.0f, "cercano por detrás del sujeto: no cuenta");
    CHECK(ns_cinema_stereo(ipd, 0.0f, 0.05f, 0.5f, 0.6f, 2.5f, &st) == 1 && st.conv >= 0.3f, "convergencia mínima");
    CHECK(ns_cinema_stereo(-1.0f, 1, 2, 0.5f, 0.6f, 2.5f, &st) == 0 && ns_cinema_stereo(NAN, 1, 2, 0.5f, 0.6f, 2.5f, &st) == 0 && ns_cinema_stereo(ipd, 1, 2, 0.0f, 0.6f, 2.5f, &st) == 0
          && ns_cinema_stereo(ipd, 1, 2, 0.5f, 0.0f, 2.5f, &st) == 0 && ns_cinema_stereo(ipd, 1, 2, 0.5f, 0.6f, 0.0f, &st) == 0 && ns_cinema_stereo(ipd, 1, 2, 0.5f, 0.6f, 2.5f, NULL) == 0, "datos inválidos rechazados");

    /* profundidades de la escena: piezas dinámicas visibles dentro de la ventana, desde la cámara del juego */
    NsScene sc; memset(&sc, 0, sizeof sc);
    NsInstance in[2]; NsPiece pc[8]; memset(in, 0, sizeof in); memset(pc, 0, sizeof pc);
    sc.inst = in; sc.piece = pc; sc.cap_inst = 2; sc.cap_piece = 8;
    sc.cam.rot[0] = sc.cam.rot[4] = sc.cam.rot[8] = 1; sc.cam.h = 320; sc.cam.overscan_y = NS_OVERSCAN_Y;
    sc.cam.eye[0] = 1000; sc.cam.eye[1] = -500; sc.cam.eye[2] = 2000;
#define PIECE(i, x, y, z) do { pc[i].world.m[3] = sc.cam.eye[0] + (x); pc[i].world.m[7] = sc.cam.eye[1] + (y); pc[i].world.m[11] = sc.cam.eye[2] + (z); } while (0)
    in[0].first_piece = 0; in[0].n_pieces = 1; in[0].flags = NS_IF_STATIC; PIECE(0, 0, 0, 500);   /* escenario: no cuenta */
    in[1].first_piece = 1; in[1].n_pieces = 6;
    PIECE(1, 0, 0, 2000); PIECE(2, 0, 0, 4000);
    PIECE(3, 0, 0, 300); pc[3].flags = NS_PF_HIDDEN;      /* oculta */
    PIECE(4, 5000, 0, 2000);                             /* fuera de la ventana */
    PIECE(5, 0, 0, -1000);                               /* detrás */
    PIECE(6, 0, 3000, 2000);                             /* fuera por abajo */
    sc.n_inst = 2; sc.n_piece = 7;
    float zn = -1, zs = -1;
    CHECK(ns_cinema_depth(&sc, &zn, &zs) == 1 && fabsf(zs - 8.0f / 3.0f) < 1e-3f && fabsf(zn - 1.75f) < 1e-3f, "sujeto = media armónica, cercano = mínimo - 0,25 m: %g %g", zs, zn);
    PIECE(2, 900, 0, 6000);                               /* hacia el borde (x/z = 0,15 de 0,5) pesa menos que lo centrado */
    CHECK(ns_cinema_depth(&sc, &zn, &zs) == 1 && zs > 2.0f && zs < 3.0f * 0.95f, "el centro pesa más: %g", zs);
    in[1].n_pieces = 9;                                   /* instancia fuera de la tabla: se ignora */
    CHECK(ns_cinema_depth(&sc, &zn, &zs) == 0 && zn == 0.0f && zs == 0.0f, "sin piezas dinámicas a la vista: 0");
    in[1].n_pieces = 6; sc.cam.h = 0;
    CHECK(ns_cinema_depth(&sc, &zn, &zs) == 0 && ns_cinema_depth(NULL, &zn, &zs) == 0, "cámara inválida");
#undef PIECE
}

int main(void) {
    test_present(); test_exhaustive(); test_cinema(); test_stereo();
    printf("%s: %d comprobaciones, %d fallos\n", g_fail ? "FALLO" : "OK", g_ok + g_fail, g_fail);
    return g_fail ? 1 : 0;
}
