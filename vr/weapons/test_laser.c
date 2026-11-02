/* Pruebas de w_laser (headless): geometría del rayo (origen en la boca, eje de la boca, longitud, grosor, punto del extremo), formato de la malla dinámica y condición de «levantada».
 * Compilar: gcc -std=gnu11 -I. test_laser.c w_laser.c wcore.c -lm */
#include "w_laser.h"
#include "wcore.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static float rnd(void) { return (float)rand() / (float)RAND_MAX; }
static WQuat rand_quat(void) {
    WQuat q = {rnd() - 0.5f, rnd() - 0.5f, rnd() - 0.5f, rnd() - 0.5f};
    float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    q.x /= l; q.y /= l; q.z /= l; q.w /= l;
    return q;
}
static WVec3 vpos(const VrHandsVertex *v) { return wv(v->pos[0], v->pos[1], v->pos[2]); }
static WVec3 centroid(const WLaserMesh *m, int a, int n) {
    WVec3 c = wv(0, 0, 0);
    for (int i = a; i < a + n; i++) c = wv_add(c, vpos(&m->v[i]));
    return wv_scale(c, 1.0f / (float)n);
}
static float dist(WVec3 a, WVec3 b) { return wv_len(wv_sub(a, b)); }

static void t_geometry(void) {
    srand(4242);
    float worst_o = 0, worst_ax = 0, worst_len = 0, worst_tip = 0, worst_w = 0;
    for (int k = 0; k < 500; k++) {
        WPose mz = {{(rnd() - 0.5f) * 4, rnd() * 2, (rnd() - 0.5f) * 4}, rand_quat()};
        float L = 0.1f + rnd() * 14.0f;
        WLaserMesh m;
        CHECK(wlaser_build(&mz, L, &m) == 1, "pose %d válida", k);
        WVec3 f = wquat_rotate(mz.q, wv(0, 0, -1));
        WVec3 c0 = centroid(&m, 0, 4), c1 = centroid(&m, 4, 4);
        float eo = dist(c0, mz.p);                                      /* el rayo parte de la boca */
        WVec3 ax = wv_norm(wv_sub(c1, c0));
        float eax = atan2f(wv_len(wv_cross(ax, f)), wv_dot(ax, f)) * 57.29578f;   /* y sigue el eje de la boca */
        float el = fabsf(dist(c0, c1) - L);
        float proj = -1e30f;                                            /* la punta lejana del punto cae en el extremo del rayo */
        for (int i = 8; i < 14; i++) { float p = wv_dot(wv_sub(vpos(&m.v[i]), mz.p), f); if (p > proj) proj = p; }
        float et = fabsf(proj - L);
        float ew = 0;                                                   /* 2,5 mm de lado: la esquina está a hw*sqrt2 del centro del anillo */
        for (int i = 0; i < 8; i++) ew = fmaxf(ew, fabsf(dist(vpos(&m.v[i]), i < 4 ? c0 : c1) - WLASER_HALF_W * 1.41421356f));
        if (eo > worst_o) worst_o = eo;
        if (eax > worst_ax) worst_ax = eax;
        if (el > worst_len) worst_len = el;
        if (et > worst_tip) worst_tip = et;
        if (ew > worst_w) worst_w = ew;
        for (int i = 0; i < WLASER_NI; i++) CHECK(m.idx[i] < WLASER_NV, "índice %d fuera de rango", i);
        for (int i = 0; i < WLASER_NV; i++) CHECK(m.v[i].rgba[0] == 255 && m.v[i].rgba[1] == 0 && m.v[i].rgba[2] == 0 && m.v[i].rgba[3] == 255, "vértice %d rojo puro", i);
        for (int i = 0; i < WLASER_NV; i++) CHECK(isfinite(m.v[i].pos[0]) && isfinite(m.v[i].pos[1]) && isfinite(m.v[i].pos[2]), "vértice %d finito", i);
    }
    printf("   origen %.2e m, eje %.4f grados, longitud %.2e m, punta %.2e m, grosor %.2e m\n", worst_o, worst_ax, worst_len, worst_tip, worst_w);
    CHECK(worst_o < 1e-5f, "el rayo parte de la boca (%.2e m)", worst_o);
    CHECK(worst_ax < 0.01f, "el rayo sigue el eje de la boca (%.4f grados)", worst_ax);
    CHECK(worst_len < 1e-4f, "longitud pedida (%.2e m)", worst_len);
    CHECK(worst_tip < 1e-4f, "la punta del punto está en el extremo (%.2e m)", worst_tip);
    CHECK(worst_w < 1e-6f, "grosor 2,5 mm (%.2e m)", worst_w);
}

static void t_limits(void) {
    WPose mz = {{0, 1.2f, -0.3f}, {0, 0, 0, 1}};
    WLaserMesh m;
    CHECK(wlaser_build(&mz, 100.0f, &m) == 1 && fabsf(dist(centroid(&m, 4, 4), mz.p) - WLASER_MAX_M) < 1e-4f, "longitud máxima %.0f m", WLASER_MAX_M);
    CHECK(wlaser_build(&mz, WLASER_MIN_M, &m) == 1, "longitud mínima");
    memset(&m, 0x5A, sizeof m);
    CHECK(wlaser_build(&mz, 0.0f, &m) == 0 && wlaser_build(&mz, -1.0f, &m) == 0 && wlaser_build(&mz, NAN, &m) == 0 && wlaser_build(&mz, INFINITY, &m) == 0, "longitud no válida");
    CHECK(((unsigned char *)&m)[0] == 0x5A && ((unsigned char *)&m)[sizeof m - 1] == 0x5A, "pose/longitud no válidas: la salida no se toca");
    CHECK(wlaser_build(NULL, 5.0f, &m) == 0 && wlaser_build(&mz, 5.0f, NULL) == 0, "NULL");
    WPose bad = mz; bad.p.x = NAN; CHECK(wlaser_build(&bad, 5.0f, &m) == 0, "posición NaN");
    bad = mz; bad.q.y = INFINITY; CHECK(wlaser_build(&bad, 5.0f, &m) == 0, "cuaternión infinito");
    bad = mz; bad.q.x = bad.q.y = bad.q.z = bad.q.w = 0; CHECK(wlaser_build(&bad, 5.0f, &m) == 0, "cuaternión nulo");
    bad = mz; bad.q.w = 3.0f; CHECK(wlaser_build(&bad, 5.0f, &m) == 1 && fabsf(dist(centroid(&m, 4, 4), mz.p) - 5.0f) < 1e-4f, "cuaternión no unitario: se normaliza");
    /* orientación identidad: -Z = delante */
    CHECK(wlaser_build(&mz, 2.0f, &m) && fabsf(centroid(&m, 4, 4).z - (mz.p.z - 2.0f)) < 1e-5f && fabsf(centroid(&m, 4, 4).x - mz.p.x) < 1e-5f, "identidad: el rayo va hacia -Z");
    /* punto: tamaño mínimo cerca, creciente con la distancia */
    WLaserMesh a, b;
    wlaser_build(&mz, 0.5f, &a); wlaser_build(&mz, 15.0f, &b);
    float sa = dist(vpos(&a.v[8]), vpos(&a.v[9])), sb = dist(vpos(&b.v[8]), vpos(&b.v[9]));
    CHECK(fabsf(sa - 2 * WLASER_DOT_MIN) < 1e-6f && sb > sa && fabsf(sb - 2 * WLASER_DOT_ANG * 15.0f) < 1e-5f, "punto %.4f m cerca y %.4f m a 15 m", sa, sb);
}

static void t_length(void) {
    CHECK(wlaser_length(-1.0f) == WLASER_MAX_M && wlaser_length(NAN) == WLASER_MAX_M && wlaser_length(INFINITY) == WLASER_MAX_M, "sin impacto / no válida: 15 m");
    CHECK(wlaser_length(WLASER_MAX_M + 5.0f) == WLASER_MAX_M && wlaser_length(WLASER_MAX_M + WLASER_SURF_GAP_M) == WLASER_MAX_M, "impacto más allá del alcance: 15 m");
    CHECK(fabsf(wlaser_length(3.0f) - (3.0f - WLASER_SURF_GAP_M)) < 1e-6f, "impacto a 3 m: el rayo acaba 5 mm antes (%.4f)", wlaser_length(3.0f));
    CHECK(wlaser_length(0.0f) == 0.0f && wlaser_length(WLASER_SURF_GAP_M * 0.5f) == 0.0f, "impacto pegado a la boca: longitud 0 (no se dibuja)");
    float prev = -1.0f; int mono = 1;
    for (int i = 0; i <= 400; i++) { float l = wlaser_length(0.05f * (float)i); if (l < prev || l > WLASER_MAX_M) mono = 0; prev = l; }
    CHECK(mono, "monótona y acotada por 15 m");
    /* la punta del octaedro (extremo del rayo) queda delante de la superficie, no sobre ella */
    WPose mz = {{0, 1.2f, 0}, {0, 0, 0, 1}};
    WLaserMesh m;
    float surf = 2.0f;                                                            /* pared en z = -2 (delante = -Z) */
    CHECK(wlaser_build(&mz, wlaser_length(surf), &m) == 1, "rayo hasta una pared a 2 m");
    float zmin = 1e9f;                                                            /* punto más lejano de todo lo dibujado, en -Z */
    for (int i = 0; i < WLASER_NV; i++) if (m.v[i].pos[2] < zmin) zmin = m.v[i].pos[2];
    CHECK(-zmin <= surf - WLASER_SURF_GAP_M + 1e-5f && -zmin > surf - 0.02f, "todo el rayo y el punto quedan delante de la pared por >= 5 mm: llega a %.4f m", -zmin);
    CHECK(wlaser_build(&mz, wlaser_length(0.03f), &m) == 0, "pared a 3 cm de la boca: nada que dibujar");
}

static void t_mesh(void) {
    WPose mz = {{0, 1.2f, -0.3f}, {0, 0, 0, 1}};
    WLaserMesh m;
    VrHandsMesh o;
    wlaser_build(&mz, 5.0f, &m);
    wlaser_mesh(&m, &o);
    CHECK(o.flags == VRH_MF_DYNAMIC && o.tex_id == 0 && o.mesh_id == WLASER_MESH_ID, "dinámica, sin textura, id propio");
    CHECK(o.verts == m.v && o.idx == m.idx && o.nverts == WLASER_NV && o.nidx == WLASER_NI && o.nidx % 3 == 0, "punteros y conteos");
    CHECK(o.nverts * sizeof(VrHandsVertex) <= 4096, "cabe en el bloque dinámico del renderer (%zu B)", (size_t)o.nverts * sizeof(VrHandsVertex));
    int id = 1;
    for (int i = 0; i < 16; i++) if (o.model[i] != ((i % 5) == 0 ? 1.0f : 0.0f)) id = 0;
    CHECK(id, "modelo = identidad (vértices en el espacio VR)");
    CHECK((WLASER_MESH_ID & 0x80000000u) == 0, "id fuera del espacio del guante");
}

static void t_raised(void) {
    WQuat id = {0, 0, 0, 1};
    WPose p = {{0, 1.2f, 0}, id};
    CHECK(wlaser_raised(&p), "apuntando al frente: levantada");
    for (int deg = -20; deg <= 90; deg += 10) {                                   /* cabeceo + = boca arriba */
        p.q = wquat_axis_angle(wv(1, 0, 0), (float)deg * 0.017453292f);
        CHECK(wlaser_raised(&p), "cabeceo %d grados: levantada", deg);
    }
    p.q = wquat_axis_angle(wv(1, 0, 0), -45.0f * 0.017453292f);
    CHECK(!wlaser_raised(&p), "45 grados por debajo del horizonte: no levantada");
    p.q = wquat_axis_angle(wv(1, 0, 0), -90.0f * 0.017453292f);
    CHECK(!wlaser_raised(&p), "apuntando al suelo: no levantada");
    p.q = wquat_axis_angle(wv(0, 1, 0), 2.0f);                                    /* giro horizontal: sigue levantada */
    CHECK(wlaser_raised(&p), "guiñada: levantada");
    p.q = id; p.p.x = NAN;
    CHECK(!wlaser_raised(&p) && !wlaser_raised(NULL), "pose no válida / NULL: no levantada");
}

int main(void) {
    printf("-- w_laser: geometría --\n"); t_geometry();
    printf("-- w_laser: límites y entradas degeneradas --\n"); t_limits();
    printf("-- w_laser: longitud hasta el impacto --\n"); t_length();
    printf("-- w_laser: malla del frame --\n"); t_mesh();
    printf("-- w_laser: levantada --\n"); t_raised();
    printf(fails ? "FALLO w_laser: %d de %d\n" : "OK w_laser: %d comprobaciones, 0 fallos\n", fails ? fails : checks, checks);
    return fails != 0;
}
