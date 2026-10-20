/* Pruebas de vr_hand_mesh (mano articulada propia): topología, orientación, espejo, volumen, colisión del puño y medidas. Sin datos del juego. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_hand_mesh.h"
#include "test_fist_mesh.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: %s ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static VrHand R, Lh;

static void cross3(const double *a, const double *b, double *o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static double dot3(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void sub3(const int16_t *a, const int16_t *b, double *o) { for (int i = 0; i < 3; i++) o[i] = (double)a[i] - (double)b[i]; }

/* cross(B-A, D-A) de una cara */
static void face_cross(const VrHandBoneMesh *m, int f, double *n) {
    const uint8_t *q = m->f[f];
    double e1[3], e2[3];
    sub3(m->v[q[1]], m->v[q[0]], e1); sub3(m->v[q[3]], m->v[q[0]], e2);
    cross3(e1, e2, n);
}
static double tri_area2(const int16_t *a, const int16_t *b, const int16_t *c) {
    double e1[3], e2[3], n[3]; sub3(b, a, e1); sub3(c, a, e2); cross3(e1, e2, n);
    return sqrt(dot3(n, n));
}

static void test_counts_and_topology(const VrHand *h, const char *name) {
    int tris = vrhand_tri_count(h);
    CHECK(tris >= 300 && tris <= 600, "%s: triángulos %d fuera de 300..600", name, tris);
    for (int k = 0; k < VRHAND_BONES; k++) {
        const VrHandBoneMesh *m = &h->mesh[k];
        CHECK(m->n_verts >= 8 && m->n_verts <= VRHAND_MAX_V && m->n_faces >= 8 && m->n_faces <= VRHAND_MAX_F, "%s hueso %d: nv=%d nf=%d", name, k, m->n_verts, m->n_faces);
        int used[VRHAND_MAX_V] = {0};
        /* aristas de perímetro: cada arista dirigida (a->b) aparece una vez y su inversa (b->a) otra: malla cerrada, orientable y consistente */
        int dir[VRHAND_MAX_V][VRHAND_MAX_V];
        memset(dir, 0, sizeof dir);
        for (int f = 0; f < m->n_faces; f++) {
            const uint8_t *q = m->f[f];
            for (int c = 0; c < 4; c++) {
                CHECK(q[c] < m->n_verts, "%s hueso %d cara %d: índice fuera de rango", name, k, f);
                used[q[c]] = 1;
            }
            CHECK(q[0] != q[1] && q[1] != q[2] && q[2] != q[3] && q[3] != q[0] && q[0] != q[2] && q[1] != q[3], "%s hueso %d cara %d: vértices repetidos", name, k, f);
            /* sin triángulos degenerados: (A,B,D) y (B,C,D), área mínima 4 mm2 (el cuadrilátero más fino mide ~20 mm2) */
            CHECK(tri_area2(m->v[q[0]], m->v[q[1]], m->v[q[3]]) > 8.0, "%s hueso %d cara %d: triángulo ABD degenerado", name, k, f);
            CHECK(tri_area2(m->v[q[1]], m->v[q[2]], m->v[q[3]]) > 8.0, "%s hueso %d cara %d: triángulo BCD degenerado", name, k, f);
            for (int c = 0; c < 4; c++) dir[q[c]][q[(c + 1) & 3]]++;
        }
        for (int i = 0; i < m->n_verts; i++) {
            CHECK(used[i], "%s hueso %d: vértice %d huérfano", name, k, i);
            for (int j = 0; j < m->n_verts; j++) {
                CHECK(dir[i][j] <= 1, "%s hueso %d: arista %d->%d repetida", name, k, i, j);
                CHECK(dir[i][j] == dir[j][i], "%s hueso %d: arista %d-%d abierta o con sentido inconsistente (%d/%d)", name, k, i, j, dir[i][j], dir[j][i]);
            }
        }
        /* orientación: cross(B-A, D-A) hacia el interior (respecto al centroide) y la normal por vértice también */
        double c[3] = {0, 0, 0};
        for (int i = 0; i < m->n_verts; i++) for (int a = 0; a < 3; a++) c[a] += m->v[i][a];
        for (int a = 0; a < 3; a++) c[a] /= m->n_verts;
        for (int f = 0; f < m->n_faces; f++) {
            double n[3]; face_cross(m, f, n);
            double fc[3] = {0, 0, 0};
            for (int q = 0; q < 4; q++) for (int a = 0; a < 3; a++) fc[a] += 0.25 * m->v[m->f[f][q]][a];
            double o[3] = {fc[0] - c[0], fc[1] - c[1], fc[2] - c[2]};
            CHECK(dot3(n, o) < 0.0, "%s hueso %d cara %d: normal geométrica hacia fuera", name, k, f);
        }
        for (int i = 0; i < m->n_verts; i++) {
            double n[3] = {m->n[i][0], m->n[i][1], m->n[i][2]}, len = sqrt(dot3(n, n));
            CHECK(fabs(len - 4096.0) < 0.01 * 4096.0 + 2.0, "%s hueso %d vértice %d: |normal| = %.1f", name, k, i, len);
            double o[3] = {m->v[i][0] - c[0], m->v[i][1] - c[1], m->v[i][2] - c[2]};
            CHECK(dot3(n, o) <= 0.0, "%s hueso %d vértice %d: normal por vértice hacia fuera", name, k, i);
        }
    }
}

static void apply(const float *x, const double *p, double *o) {
    for (int i = 0; i < 3; i++) o[i] = x[4 * i] * p[0] + x[4 * i + 1] * p[1] + x[4 * i + 2] * p[2] + x[4 * i + 3];
}

/* volumen con signo total (tetraedros desde el origen) de todas las mallas en el marco de la mano, en valor absoluto por hueso */
static double posed_volume(const VrHand *h, float xf[VRHAND_BONES][12], double *per_bone) {
    double tot = 0;
    for (int k = 0; k < VRHAND_BONES; k++) {
        const VrHandBoneMesh *m = &h->mesh[k];
        double v = 0;
        for (int f = 0; f < m->n_faces; f++) {
            const uint8_t *q = m->f[f];
            int tri[2][3] = {{q[0], q[1], q[3]}, {q[1], q[2], q[3]}};
            for (int t = 0; t < 2; t++) {
                double P[3][3];
                for (int c = 0; c < 3; c++) { double p[3] = {m->v[tri[t][c]][0], m->v[tri[t][c]][1], m->v[tri[t][c]][2]}; apply(xf[k], p, P[c]); }
                double cr[3]; cross3(P[1], P[2], cr);
                v += dot3(P[0], cr) / 6.0;
            }
        }
        if (per_bone) per_bone[k] = fabs(v);
        tot += fabs(v);
    }
    return tot;
}

static void test_volume_constant(void) {
    float xf[VRHAND_BONES][12];
    double v0[VRHAND_BONES], v1[VRHAND_BONES];
    vrhand_pose(&R, 0.0f, xf);
    double t0 = posed_volume(&R, xf, v0);
    CHECK(t0 > 80000.0 && t0 < 400000.0, "volumen de la mano abierta: %.0f mm3", t0);
    for (int i = 0; i <= 10; i++) {
        vrhand_pose(&R, i / 10.0f, xf);
        double t1 = posed_volume(&R, xf, v1);
        CHECK(fabs(t1 - t0) < 1e-4 * t0, "volumen total no constante en grip %.1f: %.1f vs %.1f", i / 10.0, t1, t0);
        for (int k = 0; k < VRHAND_BONES; k++) CHECK(fabs(v1[k] - v0[k]) < 1e-4 * v0[k] + 1e-3, "volumen del hueso %d no constante en grip %.1f (%.1f vs %.1f)", k, i / 10.0, v1[k], v0[k]);
    }
    /* los huesos son rígidos: las distancias entre vértices del mismo hueso se conservan (grosor de los dedos) */
    vrhand_pose(&R, 1.0f, xf);
    for (int k = 1; k < VRHAND_BONES; k++) {
        const VrHandBoneMesh *m = &R.mesh[k];
        double maxd = 0;
        for (int i = 0; i < m->n_verts; i++) for (int j = i + 1; j < m->n_verts; j++) {
            double a[3] = {m->v[i][0], m->v[i][1], m->v[i][2]}, b[3] = {m->v[j][0], m->v[j][1], m->v[j][2]}, pa[3], pb[3];
            apply(xf[k], a, pa); apply(xf[k], b, pb);
            double d0 = sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
            double d1 = sqrt((pa[0] - pb[0]) * (pa[0] - pb[0]) + (pa[1] - pb[1]) * (pa[1] - pb[1]) + (pa[2] - pb[2]) * (pa[2] - pb[2]));
            if (fabs(d1 - d0) > maxd) maxd = fabs(d1 - d0);
        }
        CHECK(maxd < 1e-3, "hueso %d deformado: %.5f mm", k, maxd);
    }
    /* el grosor (diámetro mínimo plano a plano de la sección) de cada falange: igual abierta y cerrada y razonable (>= 11 mm en dedos, >= 14 en el pulgar) */
    for (int k = 1; k < VRHAND_BONES; k++) CHECK(R.bone[k].radius >= 5.5f, "hueso %d demasiado fino: radio %.1f", k, R.bone[k].radius);
}

static void test_mirror(void) {
    float xr[VRHAND_BONES][12], xl[VRHAND_BONES][12];
    for (int k = 0; k < VRHAND_BONES; k++) {
        const VrHandBoneMesh *a = &R.mesh[k], *b = &Lh.mesh[k];
        CHECK(a->n_verts == b->n_verts && a->n_faces == b->n_faces, "hueso %d: tamaños distintos", k);
        for (int i = 0; i < a->n_verts; i++) {
            CHECK(b->v[i][0] == -a->v[i][0] && b->v[i][1] == a->v[i][1] && b->v[i][2] == a->v[i][2], "hueso %d vértice %d: espejo inexacto", k, i);
            CHECK(b->n[i][0] == -a->n[i][0] && b->n[i][1] == a->n[i][1] && b->n[i][2] == a->n[i][2], "hueso %d normal %d: espejo inexacto", k, i);
        }
        for (int f = 0; f < a->n_faces; f++) {
            const int ord[4] = {0, 3, 2, 1};
            for (int q = 0; q < 4; q++) CHECK(b->f[f][q] == a->f[f][ord[q]], "hueso %d cara %d: orden del espejo", k, f);
            CHECK(a->role[f] == b->role[f], "hueso %d cara %d: rol", k, f);
        }
    }
    for (int g = 0; g <= 8; g++) {
        float grip = g / 8.0f;
        vrhand_pose(&R, grip, xr); vrhand_pose(&Lh, grip, xl);
        double err = 0;
        for (int k = 0; k < VRHAND_BONES; k++) {
            /* los vértices de cada hueso, ya en el marco de la mano, deben ser el reflejo exacto */
            for (int i = 0; i < R.mesh[k].n_verts; i++) {
                double a[3] = {R.mesh[k].v[i][0], R.mesh[k].v[i][1], R.mesh[k].v[i][2]}, b[3] = {Lh.mesh[k].v[i][0], Lh.mesh[k].v[i][1], Lh.mesh[k].v[i][2]}, pa[3], pb[3];
                apply(xr[k], a, pa); apply(xl[k], b, pb);
                double e = fabs(pa[0] + pb[0]) + fabs(pa[1] - pb[1]) + fabs(pa[2] - pb[2]);
                if (e > err) err = e;
            }
        }
        CHECK(err < 1e-3, "espejo de la pose (grip %.2f): error %.6f mm", grip, err);
        /* determinante +1 de las rotaciones de la izquierda (el espejo se conjuga, no se aplica una reflexión a la pose) */
        for (int k = 0; k < VRHAND_BONES; k++) {
            const float *x = xl[k];
            double d = x[0] * (x[5] * x[10] - x[6] * x[9]) - x[1] * (x[4] * x[10] - x[6] * x[8]) + x[2] * (x[4] * x[9] - x[5] * x[8]);
            CHECK(fabs(d - 1.0) < 1e-4, "determinante del hueso %d: %.5f", k, d);
        }
    }
    float pr[3], pl[3];
    vrhand_palm_center(&R, pr); vrhand_palm_center(&Lh, pl);
    CHECK(fabsf(pr[0] + pl[0]) < 1e-3f && fabsf(pr[1] - pl[1]) < 1e-3f && fabsf(pr[2] - pl[2]) < 1e-3f, "centro de la palma no espejado");
    vrhand_pose(&R, 1.0f, xr); vrhand_pose(&Lh, 1.0f, xl);
    vrhand_fist_point(&R, xr, pr); vrhand_fist_point(&Lh, xl, pl);
    CHECK(fabsf(pr[0] + pl[0]) < 1e-3f && fabsf(pr[1] - pl[1]) < 1e-3f && fabsf(pr[2] - pl[2]) < 1e-3f, "punto de golpe no espejado: (%.2f %.2f %.2f) / (%.2f %.2f %.2f)", pr[0], pr[1], pr[2], pl[0], pl[1], pl[2]);
}

static void test_fist(void) {
    float xf[VRHAND_BONES][12];
    VrHandCollision c;
    const VrHand *hs[2] = {&R, &Lh};
    for (int s = 0; s < 2; s++) {
        const VrHand *h = hs[s];
        vrhand_pose(h, 1.0f, xf);
        vrhand_collision(h, xf, &c);
        /* "grave" = más de 4 mm (~40 % del radio de una falange) */
        CHECK(c.finger_finger < 0.5f, "mano %d puño: dedo-dedo %.2f mm", s, c.finger_finger);
        CHECK(c.finger_palm < 3.0f, "mano %d puño: dedo-palma %.2f mm", s, c.finger_palm);
        CHECK(c.thumb_finger < 4.0f, "mano %d puño: pulgar-dedos %.2f mm", s, c.thumb_finger);
        CHECK(c.thumb_palm < 3.0f, "mano %d puño: pulgar-palma %.2f mm", s, c.thumb_palm);
        /* y es de verdad un puño: la yema del corazón llega a la palma y el pulgar queda por FUERA de índice y corazón */
        CHECK(c.tip_palm_gap < 4.0f, "mano %d puño: la yema no llega a la palma (hueco %.2f mm)", s, c.tip_palm_gap);
        CHECK(c.thumb_over > 0.0f, "mano %d puño: el pulgar debe quedar por fuera (sobresale %.2f mm)", s, c.thumb_over);
        /* abierta: separada, sin penetraciones */
        vrhand_pose(h, 0.0f, xf);
        vrhand_collision(h, xf, &c);
        CHECK(c.finger_finger < 0.0f && c.finger_palm < 0.0f && c.thumb_finger < 0.0f && c.thumb_palm < 3.0f, "mano %d abierta: ff %.2f fp %.2f tf %.2f tp %.2f", s, c.finger_finger, c.finger_palm, c.thumb_finger, c.thumb_palm);
        /* en transición: el pulgar cierra después de los dedos y las penetraciones se quedan en lo visualmente tolerable */
        for (int g = 1; g < 20; g++) {
            float grip = g / 20.0f;
            vrhand_pose(h, grip, xf);
            vrhand_collision(h, xf, &c);
            CHECK(c.finger_finger < 0.5f && c.finger_palm < 3.0f, "mano %d grip %.2f: ff %.2f fp %.2f", s, grip, c.finger_finger, c.finger_palm);
            CHECK(c.thumb_finger < 8.0f && c.thumb_palm < 3.0f, "mano %d grip %.2f: pulgar tf %.2f tp %.2f", s, grip, c.thumb_finger, c.thumb_palm);
        }
    }
    /* monótona: la yema del corazón se acerca a la palma con el agarre y el punto de golpe avanza al cerrar sin saltos */
    float prev_gap = 1e9f, prev_fp[3] = {0, 0, 0};
    for (int g = 0; g <= 20; g++) {
        float grip = g / 20.0f;
        vrhand_pose(&R, grip, xf);
        vrhand_collision(&R, xf, &c);
        CHECK(c.tip_palm_gap <= prev_gap + 1e-3f, "yema no monótona en grip %.2f: %.2f > %.2f", grip, c.tip_palm_gap, prev_gap);
        prev_gap = c.tip_palm_gap;
        float fp[3];
        vrhand_fist_point(&R, xf, fp);
        if (g > 0) { float d = sqrtf((fp[0] - prev_fp[0]) * (fp[0] - prev_fp[0]) + (fp[1] - prev_fp[1]) * (fp[1] - prev_fp[1]) + (fp[2] - prev_fp[2]) * (fp[2] - prev_fp[2])); CHECK(d < 14.0f, "salto del punto de golpe en grip %.2f: %.1f mm", grip, d); }
        memcpy(prev_fp, fp, sizeof fp);
    }
    /* punto de golpe: abierta = yemas (cerca de la punta), puño = nudillos (a una distancia razonable de la muñeca, por delante de la palma) */
    float f0[3], f1[3];
    vrhand_pose(&R, 0.0f, xf); vrhand_fist_point(&R, xf, f0);
    vrhand_pose(&R, 1.0f, xf); vrhand_fist_point(&R, xf, f1);
    CHECK(f0[1] < R.dims.tip_y + 14.0f && f0[1] > R.dims.tip_y - 3.0f, "yemas abiertas en y=%.1f (punta %.1f)", f0[1], R.dims.tip_y);
    CHECK(f1[1] < -55.0f && f1[1] > -90.0f, "nudillos del puño en y=%.1f", f1[1]);
    /* curl por dedo: un solo dedo cerrado no mueve a los demás */
    float cur[5] = {0, 0, 0, 0, 0}, xo[VRHAND_BONES][12], xi[VRHAND_BONES][12];
    vrhand_pose_curls(&R, cur, xo);
    cur[1] = 1.0f; vrhand_pose_curls(&R, cur, xi);
    for (int k = 1; k < VRHAND_BONES; k++) {
        int moved = 0;
        for (int i = 0; i < 12; i++) if (fabsf(xo[k][i] - xi[k][i]) > 1e-5f) moved = 1;
        CHECK(moved == (k >= 4 && k <= 6), "curl del corazón: hueso %d movido=%d", k, moved);
    }
}

static void test_dims(void) {
    VrHandDims d;
    vrhand_dims_default(&d);
    VrHand h;
    CHECK(vrhand_build(&h, &d, 1) == 0, "build por defecto");
    float xf[VRHAND_BONES][12];
    vrhand_pose(&h, 0.0f, xf);
    /* abierta: la punta queda a +-8 % del largo medido */
    double ymin = 1e9, ymax = -1e9;
    for (int k = 0; k < VRHAND_BONES; k++) for (int i = 0; i < h.mesh[k].n_verts; i++) {
        double p[3] = {h.mesh[k].v[i][0], h.mesh[k].v[i][1], h.mesh[k].v[i][2]}, q[3];
        apply(xf[k], p, q);
        if (q[1] < ymin) ymin = q[1];
        if (q[1] > ymax) ymax = q[1];
    }
    double L = d.cuff_y - d.tip_y;
    CHECK(fabs(ymin - d.tip_y) < 0.08 * L, "punta abierta y=%.1f (medida %.1f)", ymin, d.tip_y);
    CHECK(ymax > d.cuff_y && ymax < d.cuff_y + 0.1 * L, "arranque del puño y=%.1f (plano %.1f)", ymax, d.cuff_y);
    /* el puño de la mano queda dentro del de la manga (se mete en ella), sin sobresalir más del 3 % */
    double xmin = 1e9, xmax = -1e9, zmin = 1e9, zmax = -1e9;
    for (int i = 0; i < h.mesh[0].n_verts; i++) if (h.mesh[0].v[i][1] >= d.cuff_y) {
        if (h.mesh[0].v[i][0] < xmin) xmin = h.mesh[0].v[i][0];
        if (h.mesh[0].v[i][0] > xmax) xmax = h.mesh[0].v[i][0];
        if (h.mesh[0].v[i][2] < zmin) zmin = h.mesh[0].v[i][2];
        if (h.mesh[0].v[i][2] > zmax) zmax = h.mesh[0].v[i][2];
    }
    CHECK(xmax - xmin <= 2 * d.cuff_h[0] * 1.03 && zmax - zmin <= 2 * d.cuff_h[1] * 1.03 && xmax - xmin > 1.5 * d.cuff_h[0], "puño de la mano %.1f x %.1f vs manga %.1f x %.1f", xmax - xmin, zmax - zmin, 2 * d.cuff_h[0], 2 * d.cuff_h[1]);
    /* ajuste a la manopla sintética: medidas razonables y espejo coherente */
    SynthHand sh, sl;
    synth_hand(&sh, 0); synth_hand(&sl, 1);
    VrHandDims dr, dl;
    int okr = vrhand_dims_from_verts(sh.v, sh.nv, 4, 1, &dr), okl = vrhand_dims_from_verts(sl.v, sl.nv, 4, -1, &dl);
    CHECK(okr && okl, "medidas de la manopla sintética");
    CHECK(fabsf(dr.cuff_y - 20.0f) < 0.5f && fabsf(dr.tip_y + 125.0f) < 0.5f, "medidas sintéticas: y %.1f / %.1f", dr.cuff_y, dr.tip_y);
    CHECK(fabsf(dr.cuff_c[0] - dl.cuff_c[0]) < 0.01f && fabsf(dr.cuff_h[0] - dl.cuff_h[0]) < 0.01f && fabsf(dr.cuff_h[1] - dl.cuff_h[1]) < 0.01f, "las medidas de la izquierda se miden en espejo");
    VrHandDims bad;
    int16_t zero[8 * 4];
    memset(zero, 0, sizeof zero);
    CHECK(vrhand_dims_from_verts(zero, 8, 4, 1, &bad) == 0 && bad.tip_y == d.tip_y, "malla inservible -> medidas por defecto");
    CHECK(vrhand_dims_from_verts(NULL, 0, 4, 1, &bad) == 0, "NULL");
    /* escalas distintas: cualquier medida razonable construye una mano cerrada y válida */
    for (int s = 0; s < 5; s++) {
        VrHandDims e = d;
        float k = 0.7f + 0.2f * (float)s;
        e.cuff_y *= k; e.tip_y *= k; e.cuff_h[0] *= k; e.cuff_h[1] *= k; e.cuff_c[0] *= k; e.cuff_c[1] *= k;
        VrHand hh;
        CHECK(vrhand_build(&hh, &e, s & 1 ? -1 : 1) == 0, "build escala %.1f", k);
        int t = vrhand_tri_count(&hh);
        CHECK(t >= 300 && t <= 600, "escala %.1f: triángulos %d", k, t);
        if (k >= 0.7f && k <= 1.5f) {
            float x2[VRHAND_BONES][12]; VrHandCollision cc;
            vrhand_pose(&hh, 1.0f, x2); vrhand_collision(&hh, x2, &cc);
            CHECK(cc.finger_finger < 1.0f && cc.finger_palm < 4.0f, "escala %.1f: puño ff %.2f fp %.2f", k, cc.finger_finger, cc.finger_palm);
        }
    }
}

static void test_robust(void) {
    float xf[VRHAND_BONES][12], xr[VRHAND_BONES][12];
    const float bad[] = {NAN, INFINITY, -INFINITY, -5.0f, 7.0f};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        vrhand_pose(&R, bad[i], xf);
        int fin = 1;
        for (int k = 0; k < VRHAND_BONES; k++) for (int j = 0; j < 12; j++) if (!isfinite(xf[k][j])) fin = 0;
        CHECK(fin, "pose con grip %f no finita", bad[i]);
    }
    vrhand_pose(&R, 0.0f, xr);
    vrhand_pose(&R, NAN, xf);
    CHECK(memcmp(xf, xr, sizeof xf) == 0, "NaN debe dar la mano abierta");
    vrhand_pose(&R, 1.0f, xr); vrhand_pose(&R, 7.0f, xf);
    CHECK(memcmp(xf, xr, sizeof xf) == 0, "grip > 1 se acota");
    float cur[5] = {NAN, 2.0f, -1.0f, INFINITY, 0.5f};
    vrhand_pose_curls(&R, cur, xf);
    int fin = 1;
    for (int k = 0; k < VRHAND_BONES; k++) for (int j = 0; j < 12; j++) if (!isfinite(xf[k][j])) fin = 0;
    CHECK(fin, "curls no finitos");
    VrHand a, b;
    vrhand_build(&a, NULL, 1); vrhand_build(&b, NULL, 1);
    CHECK(memcmp(&a, &b, sizeof a) == 0, "construcción no determinista");
    VrHandDims nd; vrhand_dims_default(&nd); nd.tip_y = NAN;
    CHECK(vrhand_build(&a, &nd, 1) == 0 && vrhand_tri_count(&a) >= 300, "dims con NaN -> por defecto");
    CHECK(vrhand_build(NULL, NULL, 1) < 0, "build NULL");
}

int main(void) {
    vrhand_build(&R, NULL, 1);
    vrhand_build(&Lh, NULL, -1);
    printf("mano: %d triángulos, %d+%d vértices máx por hueso\n", vrhand_tri_count(&R), VRHAND_MAX_V, VRHAND_MAX_F);
    test_counts_and_topology(&R, "derecha");
    test_counts_and_topology(&Lh, "izquierda");
    test_volume_constant();
    test_mirror();
    test_fist();
    test_dims();
    test_robust();
    printf("%d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
