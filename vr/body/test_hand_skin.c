/* Pruebas de vr_hand_asset + vr_hand_skin (mano con skin lineal): integridad del asset (pesos, huesos, piezas, tamaño real), pose de reposo = identidad, cierres monótonos,
 * sin NaN, volumen conservado al cerrar, espejo exacto de la izquierda y puntos de referencia (centro de agarre, yema del índice, nudillos). Sin datos del juego. make -C vr/body test */
#include "vr_hand_skin.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO l.%d: %s ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)
static float P[VRHA_NV][3], N[VRHA_NV][3], P2[VRHA_NV][3], N2[VRHA_NV][3];

static void test_asset(void) {
    int bad_w = 0, bad_j = 0, bad_t = 0;
    for (int i = 0; i < VRHA_NV; i++) {
        float s = 0;
        for (int k = 0; k < 4; k++) { s += vrha_weight[i][k]; if (vrha_weight[i][k] < 0 || (vrha_weight[i][k] > 0 && vrha_joint[i][k] >= VRHA_NJ)) bad_j++; }
        if (fabsf(s - 1.0f) > 1e-4f) bad_w++;
        float nl = sqrtf(N[0][0] * 0 + vrha_nrm[i][0] * vrha_nrm[i][0] + vrha_nrm[i][1] * vrha_nrm[i][1] + vrha_nrm[i][2] * vrha_nrm[i][2]);
        if (fabsf(nl - 1.0f) > 1e-3f) bad_w++;
    }
    CHECK(bad_w == 0 && bad_j == 0, "pesos que no suman 1 / normales no unitarias: %d; huesos inválidos: %d", bad_w, bad_j);
    int tv = 0, tt = 0;
    for (int p = 0; p < VRHA_NP; p++) {
        const VrHaPiece *pc = &vrha_piece[p];
        CHECK(pc->nv <= 256 && pc->nv > 0 && pc->v0 == tv && pc->t0 == tt, "pieza %d: %d vértices, inicio %d/%d", p, pc->nv, pc->v0, pc->t0);
        for (int t = pc->t0; t < pc->t0 + pc->nt; t++) for (int k = 0; k < 3; k++) if (vrha_tri[t][k] >= pc->nv) bad_t++;
        tv += pc->nv; tt += pc->nt;
    }
    CHECK(tv == VRHA_NV && tt == VRHA_NT && bad_t == 0, "piezas: %d/%d vértices %d/%d triángulos, índices fuera %d", tv, VRHA_NV, tt, VRHA_NT, bad_t);
    CHECK(VRHA_NT <= 1500 && VRHA_NT >= 1000, "triángulos %d", VRHA_NT);
    for (int j = 0; j < VRHA_NJ; j++) {
        CHECK(vrha_parent[j] < j && (j == 0) == (vrha_parent[j] < 0), "padre de %s", vrha_name[j]);
        float m[12], e = 0;   /* bind * ibm = identidad */
        for (int i = 0; i < 3; i++) for (int c = 0; c < 4; c++) {
            m[4 * i + c] = vrha_bind[j][4 * i] * vrha_ibm[j][c] + vrha_bind[j][4 * i + 1] * vrha_ibm[j][4 + c] + vrha_bind[j][4 * i + 2] * vrha_ibm[j][8 + c] + (c == 3 ? vrha_bind[j][4 * i + 3] : 0);
            e = fmaxf(e, fabsf(m[4 * i + c] - (c == i ? 1.0f : 0.0f)));
        }
        CHECK(e < 1e-3f, "%s: bind * ibm != I (%g)", vrha_name[j], e);
    }
    /* tamaño real: muñeca a yema del corazón ~190 mm; dedos -Y, palma +X, pulgar +Z */
    float ymin = 1e9f, xs = 0; int np = 0;
    for (int i = 0; i < VRHA_NV; i++) { if (vrha_pos[i][1] < ymin) ymin = vrha_pos[i][1]; }
    CHECK(-ymin > 180.0f && -ymin < 200.0f, "largo muñeca-yema %.1f mm", -ymin);
    for (int i = 0; i < VRHA_NV; i++) if (vrha_pos[i][1] < -40 && vrha_pos[i][1] > -80 && fabsf(vrha_pos[i][2]) < 15) { xs += vrha_nrm[i][0] > 0.7f ? vrha_pos[i][0] : 0; np += vrha_nrm[i][0] > 0.7f; }
    CHECK(np > 0 && xs / (float)np > 10.0f, "palma hacia +X (%.1f)", np ? xs / (float)np : 0.0f);
    CHECK(vrha_bind[VRHS_THUMB_TIP][11] > 50.0f && vrha_bind[VRHS_LITTLE_MC + 4][11] < -30.0f, "pulgar +Z, meñique -Z");
}

static void test_bind_identity(void) {
    float xf[VRHS_J][12], sk[VRHS_J][12];
    const float zero[5] = {0, 0, 0, 0, 0};
    vrhs_pose(zero, 0.0f, 1, xf);
    vrhs_skin_mats(xf, 1, sk);
    float e = 0;
    for (int j = 0; j < VRHS_J; j++) for (int i = 0; i < 12; i++) e = fmaxf(e, fabsf(sk[j][i] - ((i == 0 || i == 5 || i == 10) ? 1.0f : 0.0f) * 1.0f - ((i % 4 == 3) ? 0.0f : 0.0f)));
    CHECK(e < 2e-3f, "reposo: matrices de skin = identidad (%g)", e);
    vrhs_skin(xf, 1, P, N);
    float pe = 0, ne = 0;
    for (int i = 0; i < VRHA_NV; i++) for (int a = 0; a < 3; a++) { pe = fmaxf(pe, fabsf(P[i][a] - vrha_pos[i][a])); ne = fmaxf(ne, fabsf(N[i][a] - vrha_nrm[i][a])); }
    CHECK(pe < 0.05f && ne < 1e-3f, "reposo: vértices %.4f mm, normales %.5f", pe, ne);
}

static float tip_palm(const float c[5], int f) {   /* dedos: distancia de la punta a la muñeca; pulgar: de su punta a la base del meñique (oposición) */
    float xf[VRHS_J][12];
    vrhs_pose(c, 0.0f, 1, xf);
    const int tip = f == 4 ? VRHS_THUMB_TIP : VRHS_INDEX_MC + 5 * f + 4, ref = f == 4 ? VRHS_LITTLE_MC + 1 : VRHS_WRIST;
    float d[3] = {xf[tip][3] - xf[ref][3], xf[tip][7] - xf[ref][7], xf[tip][11] - xf[ref][11]};
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}
static double volume(float (*p)[3]) {   /* volumen (con signo) de la malla abierta respecto a la muñeca: el anillo de la muñeca no se mueve */
    double v = 0;
    for (int q = 0; q < VRHA_NP; q++) {
        const VrHaPiece *pc = &vrha_piece[q];
        for (int t = pc->t0; t < pc->t0 + pc->nt; t++) {
            const float *a = p[pc->v0 + vrha_tri[t][0]], *b = p[pc->v0 + vrha_tri[t][1]], *c = p[pc->v0 + vrha_tri[t][2]];
            v += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6.0;
        }
    }
    return v;
}
static void test_curls(void) {
    /* cierre monótono: la punta de cada dedo se acerca a la muñeca (el pulgar, a la base del meñique) sin alejarse más de 0,5 mm entre pasos, y el puño la deja cerca */
    for (int f = 0; f < 5; f++) {
        float prev = 1e9f, c[5] = {0, 0, 0, 0, 0}, d0 = 0, d1 = 0;
        for (int k = 0; k <= 20; k++) {
            c[f] = k / 20.0f;
            float d = tip_palm(c, f);
            CHECK(isfinite(d) && d <= prev + 0.5f, "dedo %d: cierre no monótono en %.2f (%.1f -> %.1f)", f, c[f], prev, d);
            if (k == 0) d0 = d;
            d1 = d; prev = d;
        }
        CHECK(d1 < d0 - (f == 4 ? 15.0f : 40.0f), "dedo %d: el puño no cierra (%.1f -> %.1f mm)", f, d0, d1);
    }
    /* volumen: abierta vs puño y poses intermedias (los dedos no se adelgazan al cerrar) */
    const float zero[5] = {0, 0, 0, 0, 0};
    float xf[VRHS_J][12];
    vrhs_pose(zero, 0.0f, 1, xf); vrhs_skin(xf, 1, P, NULL);
    const double v0 = volume(P);
    double worst = 0;
    for (int k = 1; k <= 10; k++) {
        float c[5] = {k / 10.0f, k / 10.0f, k / 10.0f, k / 10.0f, k / 10.0f};
        vrhs_pose(c, (k % 3 - 1) * 0.5f, 1, xf); vrhs_skin(xf, 1, P, NULL);
        double r = fabs(volume(P) / v0 - 1.0);
        if (k <= 8 && r > worst) worst = r;
        if (k == 10) CHECK(r < 0.15, "puño: variación de volumen %.1f %% (incluye el contacto de los dedos con la palma)", r * 100);
    }
    CHECK(v0 > 2.0e5 && worst < 0.10, "volumen %.0f mm3, variación máxima hasta cierre 0,8 %.1f %%", v0, worst * 100);
    printf("  volumen de la mano %.0f cm3, variación máxima hasta cierre 0,8: %.1f %%\n", v0 / 1000.0, worst * 100);
}

static void test_mirror_nan(void) {
    const float c[5] = {0.3f, 0.6f, 0.7f, 0.8f, 0.5f};
    float xr[VRHS_J][12], xl[VRHS_J][12];
    vrhs_pose(c, 0.4f, 1, xr); vrhs_pose(c, 0.4f, -1, xl);
    vrhs_skin(xr, 1, P, N); vrhs_skin(xl, -1, P2, N2);
    float e = 0;
    for (int i = 0; i < VRHA_NV; i++) e = fmaxf(e, fabsf(P[i][0] + P2[i][0]) + fabsf(P[i][1] - P2[i][1]) + fabsf(P[i][2] - P2[i][2]) + fabsf(N[i][0] + N2[i][0]));
    CHECK(e < 1e-3f, "la izquierda no es el espejo (%g)", e);
    float g[3], g2[3]; vrhs_grip_center(1, g); vrhs_grip_center(-1, g2);
    CHECK(g[0] > 30.0f && fabsf(g[0] + g2[0]) < 1e-5f && g[1] < -50.0f && g[1] > -90.0f, "centro de agarre (%.1f %.1f %.1f)", g[0], g[1], g[2]);
    float fp[3]; vrhs_fist_point(1, fp);
    CHECK(fp[1] < -80.0f && fp[1] > -140.0f && isfinite(fp[0]), "nudillos del puño (%.1f %.1f %.1f)", fp[0], fp[1], fp[2]);
    const float bad[5] = {NAN, INFINITY, -5.0f, 7.0f, -INFINITY};
    vrhs_pose(bad, NAN, 1, xr); vrhs_skin(xr, 1, P, N);
    int nan = 0;
    for (int i = 0; i < VRHA_NV; i++) for (int a = 0; a < 3; a++) nan += !isfinite(P[i][a]) || !isfinite(N[i][a]);
    CHECK(nan == 0, "entradas no finitas: %d valores no finitos", nan);
    vrhs_pose(NULL, 0.0f, 1, xr);
    CHECK(isfinite(xr[VRHS_INDEX_TIP][3]), "curl NULL");
}

int main(void) {
    test_asset();
    test_bind_identity();
    test_curls();
    test_mirror_nan();
    printf("%s vr_hand_skin: %d comprobaciones, %d fallos\n", fails ? "FALLO" : "OK", checks, fails);
    return fails ? 1 : 0;
}
