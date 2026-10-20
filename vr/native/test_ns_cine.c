/* Regresión de la pantalla 3D de las cinemáticas (sintética, sin datos del juego): la cadena de la app (ns_cinema_to_vr -> vista de ojo -> proyección -> píxel, con el descarte por detrás del
 * sombreador, ns_gpu_tri_back) debe dar la MISMA imagen que la cámara directa del motor con su regla de caras, y fallar si la orientación es incorrecta.
 * Escena: suelo (anverso hacia arriba) y TECHO de una sola cara (anverso hacia abajo) con texturas de colores distintas. Cámaras fuera de la sala (encima del techo, mirando abajo: sin descarte el techo
 * tapa el suelo = el defecto «veo un techo») y dentro (debajo del techo, mirando arriba). Medida: correlación por bloques de la luminancia y centro de masas. */
#include "ns_cinema.h"
#include "ns_cinema_ref.h"
#include "ns_cpuref.h"
#include "ns_gpu.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_ok, g_fail;
#define CHECK(c, ...) do { if (c) g_ok++; else { g_fail++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MAXT 4096
static NsRTri g_t[MAXT];
static NsAdaptSrc g_s[MAXT];
static size_t g_n;

/* triángulo con el sentido pedido: dir = +1 si el producto vectorial (p1-p0)x(p2-p0) debe apuntar a +Y, -1 a -Y */
static void add_tri(const float a[3], const float b[3], const float c[3], int dir, uint8_t r, uint8_t g, uint8_t bl, uint32_t mflags) {
    const float *p[3] = { a, b, c };
    float e1[3], e2[3];
    for (int k = 0; k < 3; k++) { e1[k] = b[k] - a[k]; e2[k] = c[k] - a[k]; }
    const float ny = e1[2] * e2[0] - e1[0] * e2[2];
    if ((ny > 0) != (dir > 0)) { p[1] = c; p[2] = b; }
    NsRTri *t = &g_t[g_n];
    memset(t, 0, sizeof *t);
    for (int k = 0; k < 3; k++) { memcpy(t->v[k].p, p[k], 12); t->v[k].r = r; t->v[k].g = g; t->v[k].b = bl; }
    memset(&g_s[g_n], 0, sizeof g_s[g_n]);
    g_s[g_n].mflags = mflags;
    g_n++;
}
/* plano horizontal y = h, de x,z en [-L,L], N x N baldosas con un patrón de color por baldosa; dir = hacia dónde mira el anverso */
static void plane(float h, float L, int N, int dir, int pat, uint32_t mflags) {
    for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) {
        float x0 = -L + 2 * L * (float)i / (float)N, x1 = -L + 2 * L * (float)(i + 1) / (float)N, z0 = -L + 2 * L * (float)j / (float)N, z1 = -L + 2 * L * (float)(j + 1) / (float)N;
        const float A[3] = { x0, h, z0 }, B[3] = { x1, h, z0 }, C[3] = { x1, h, z1 }, D[3] = { x0, h, z1 };
        unsigned hsh = (unsigned)(i * 7919 + j * 104729 + pat * 13007);
        uint8_t r = (uint8_t)(40 + (hsh * 2654435761u >> 24) % 200), g = (uint8_t)(40 + ((hsh * 40503u) >> 8) % 200), b = (uint8_t)(40 + ((hsh * 69069u) >> 10) % 200);
        add_tri(A, B, D, dir, r, g, b, mflags);
        add_tri(B, C, D, dir, r, g, b, mflags);
    }
}

/* cámara del motor: rot (vista -> mundo, fila-mayor) desde yaw/pitch/roll en un mundo con Y arriba; columnas = X der, Y abajo, Z delante */
static void make_cam(NsCamera *c, const float eye[3], float yaw, float pitch, float roll) {
    memset(c, 0, sizeof *c);
    const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch), cr = cosf(roll), sr = sinf(roll);
    float f[3] = { sy * cp, sp, cy * cp };                      /* delante (pitch > 0 = mirar arriba) */
    float r0[3] = { cy, 0, -sy };                                /* derecha horizontal */
    float d0[3] = { f[1] * r0[2] - f[2] * r0[1], f[2] * r0[0] - f[0] * r0[2], f[0] * r0[1] - f[1] * r0[0] };   /* f x r (abajo con Y arriba) */
    float nd = sqrtf(d0[0] * d0[0] + d0[1] * d0[1] + d0[2] * d0[2]);
    for (int k = 0; k < 3; k++) d0[k] /= nd;
    float X[3], Y[3];
    for (int k = 0; k < 3; k++) { X[k] = cr * r0[k] + sr * d0[k]; Y[k] = -sr * r0[k] + cr * d0[k]; }
    for (int r = 0; r < 3; r++) { c->rot[r * 3 + 0] = X[r]; c->rot[r * 3 + 1] = Y[r]; c->rot[r * 3 + 2] = f[r]; c->eye[r] = eye[r]; }
    c->h = 260; c->overscan_y = NS_OVERSCAN_Y;
}

typedef struct { double corr, dcom; } Sc;
static void render_direct(const NsCamera *cam, const NsRTri *tr, size_t n, int w, int h, uint8_t *rgb) {
    float A[9], t[3]; ns_cine_engine_view(cam, A, t);
    NsRCam c; ns_cam_game(&c, A, t, cam->h, 160, 120, 320, 240, w, h); c.znear = 1;
    NsRScene rs = { tr, n, NULL };
    NsRBuf b; ns_buf_alloc(&b, w, h); ns_buf_clear(&b, 0, 0, 0);
    ns_render_view(&rs, &c, 0, &b, NULL); ns_buf_to_rgb8(&b, rgb, 0, 0); ns_buf_free(&b);
}
static int render_app(const NsCamera *cam, const float gv[16], int eye, const NsRTri *tr, size_t n, int w, int h, uint8_t *rgb) {
    NsRTri *tt = malloc(n * sizeof *tt); memcpy(tt, tr, n * sizeof *tt);
    NsRCam c;
    int ok = ns_cine_app_chain(cam, gv, eye, 0.02f, 3.0f, 0, 0, tt, n, w, h, &c);
    if (ok) {
        NsRScene rs = { tt, n, NULL };
        NsRBuf b; ns_buf_alloc(&b, w, h); ns_buf_clear(&b, 0, 0, 0);
        ns_render_view(&rs, &c, 0, &b, NULL); ns_buf_to_rgb8(&b, rgb, 0, 0); ns_buf_free(&b);
    }
    free(tt);
    return ok;
}

static void test_case(const char *name, const float eye[3], float yaw, float pitch, float roll, int expect_ceiling_hidden) {
    NsCamera cam; make_cam(&cam, eye, yaw, pitch, roll);
    const int w = 160, h = 120;
    uint8_t *ref = malloc((size_t)w * h * 3), *img = malloc((size_t)w * h * 3);
    /* motor: con su regla de caras (descarte de una sola cara por detrás desde el ojo); la app: la geometría entera + descarte del sombreador */
    NsRTri *eng = malloc(sizeof g_t); NsAdaptSrc *es = malloc(sizeof g_s);
    memcpy(eng, g_t, sizeof g_t); memcpy(es, g_s, sizeof g_s);
    size_t ne = ns_cine_cull_back(eng, es, g_n, eye);
    render_direct(&cam, eng, ne, w, h, ref);
    NsRTri *app = malloc(sizeof g_t); NsAdaptSrc *as = malloc(sizeof g_s);
    memcpy(app, g_t, sizeof g_t); memcpy(as, g_s, sizeof g_s);
    size_t na = ns_cine_cull_back(app, as, g_n, eye);
    CHECK(na == ne, "%s: el descarte de la app (%zu) = el del motor (%zu)", name, na, ne);
    if (expect_ceiling_hidden) CHECK(ne < g_n, "%s: el escenario tiene caras por detrás que descartar", name);
    for (int k = 0; k < NS_CINE_NGV; k++) for (int e = 0; e < 2; e++) {
        float gv[16]; ns_cine_test_gv(&cam, k, gv);
        int ok = render_app(&cam, gv, e, app, na, w, h, img);
        CHECK(ok, "%s gv%d ojo%d: cadena válida", name, k, e);
        if (!ok) continue;
        NsCineScore s; ns_cine_score(ref, img, w, h, &s);
        CHECK(s.block_corr > 0.9 && s.com_dist < 0.02, "%s gv%d ojo%d: correlación por bloques %.3f (>0.9) y distancia del centro de masas %.4f (<0.02)", name, k, e, s.block_corr, s.com_dist);
    }
    if (expect_ceiling_hidden) {
        /* sin el descarte (doble cara, como antes) el techo tapa la escena: la medida lo detecta */
        float gv[16]; ns_cine_test_gv(&cam, 2, gv);
        render_app(&cam, gv, 0, g_t, g_n, w, h, img);
        NsCineScore s; ns_cine_score(ref, img, w, h, &s);
        CHECK(s.block_corr < 0.5, "%s: sin descarte debe fallar la medida (corr=%.3f)", name, s.block_corr);
    }
    free(ref); free(img); free(eng); free(es); free(app); free(as);
}

int main(void) {
    /* suelo y techo (de una sola cara) a 4 m; un techo de doble cara más alto para comprobar que no se descarta */
    plane(0, 6000, 12, -1, 1, 0);        /* suelo: anverso hacia arriba (producto vectorial hacia -Y en el sentido de la regla: se ve desde arriba) */
    plane(4000, 6000, 12, +1, 2, 0);     /* techo: anverso hacia abajo */
    {   /* regla: anverso = dot(p0 - ojo, n) > 0; el suelo se ve desde arriba (ojo con y > 0) si n apunta a -Y */
        const float eye_up[3] = { 0, 9000, 0 }, eye_in[3] = { 0, 2000, 0 };
        CHECK(ns_gpu_tri_back(g_t[0].v[0].p, g_t[0].v[1].p, g_t[0].v[2].p, eye_up) == 0, "suelo visto desde arriba: anverso");
        CHECK(ns_gpu_tri_back(g_t[0].v[0].p, g_t[0].v[1].p, g_t[0].v[2].p, eye_in) == 0, "suelo visto desde dentro: anverso");
        const size_t ci = g_n - 2;
        CHECK(ns_gpu_tri_back(g_t[ci].v[0].p, g_t[ci].v[1].p, g_t[ci].v[2].p, eye_up) == 1, "techo visto desde arriba: reverso (no se dibuja)");
        CHECK(ns_gpu_tri_back(g_t[ci].v[0].p, g_t[ci].v[1].p, g_t[ci].v[2].p, eye_in) == 0, "techo visto desde dentro: anverso");
    }
    const float up[3] = { 300, 9000, -500 }, in[3] = { -200, 1500, 800 };
    test_case("fuera mirando abajo", up, 0.4f, -1.2f, 0.0f, 1);
    test_case("fuera, giro y alabeo", up, 2.0f, -0.9f, 0.5f, 1);
    test_case("dentro mirando al techo", in, -0.7f, 0.9f, 0.0f, 0);
    test_case("dentro, horizontal", in, 1.3f, 0.0f, 0.0f, 0);
    {   /* un modelo de doble cara no se descarta nunca */
        size_t n0 = g_n;
        plane(8000, 3000, 4, +1, 3, NS_MF_TWOSIDED);
        const float eye_up[3] = { 0, 9000, 0 };
        NsRTri *tr = malloc(sizeof g_t); NsAdaptSrc *sr = malloc(sizeof g_s);
        memcpy(tr, g_t, sizeof g_t); memcpy(sr, g_s, sizeof g_s);
        size_t kept = ns_cine_cull_back(tr, sr, g_n, eye_up);
        size_t two = 0; for (size_t i = 0; i < kept; i++) if (sr[i].mflags & NS_MF_TWOSIDED) two++;
        CHECK(two == g_n - n0, "doble cara: se conservan todas (%zu de %zu)", two, g_n - n0);
        free(tr); free(sr);
    }
    printf("test_ns_cine: %d comprobaciones, %d fallos\n", g_ok + g_fail, g_fail);
    return g_fail != 0;
}
