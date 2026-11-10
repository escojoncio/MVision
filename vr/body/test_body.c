/* Pruebas de vr_body: solver (propiedades geométricas con miles de casos aleatorios), matemática, y el pegamento sobre
 * una RAM sintética con la estructura del modelo del jugador (solo escribe las 6 piezas, restaura byte a byte, fundidos,
 * robustez ante RAM corrupta). No necesita datos del juego. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_body.h"
#include "vr_hand_skin.h"
#include "test_fist_mesh.h"
#include "../bridge/bridge_addrs.h"

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; if (g_fail < 40) { printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }
static float frand(float lo, float hi) { return lo + (hi - lo) * ((rnd() & 0xFFFFFF) / 16777216.0f); }

static float dotv(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float lenv(const float *a) { return sqrtf(dotv(a, a)); }
static void mv(float *o, const float *m, const float *v) { o[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2]; o[1] = m[3] * v[0] + m[4] * v[1] + m[5] * v[2]; o[2] = m[6] * v[0] + m[7] * v[1] + m[8] * v[2]; }
static void mm(float *o, const float *a, const float *b) {
    float r[9];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r[3 * i + j] = a[3 * i] * b[j] + a[3 * i + 1] * b[3 + j] + a[3 * i + 2] * b[6 + j];
    memcpy(o, r, sizeof r);
}
static float det3(const float *m) { return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]); }
static float ortho_err(const float *m) {
    float t[9] = {m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]}, p[9]; mm(p, t, m);
    float e = 0; for (int i = 0; i < 9; i++) e = fmaxf(e, fabsf(p[i] - (i % 4 == 0 ? 1.0f : 0.0f)));
    return e;
}
static void rand_rot(float *m, float maxang) {
    float k[3] = {frand(-1, 1), frand(-1, 1), frand(-1, 1)}; float n = lenv(k); if (n < 1e-3f) { k[0] = 1; n = 1; }
    for (int i = 0; i < 3; i++) k[i] /= n;
    float a = frand(-maxang, maxang), c = cosf(a), s = sinf(a), t = 1 - c;
    float r[9] = {t * k[0] * k[0] + c, t * k[0] * k[1] - s * k[2], t * k[0] * k[2] + s * k[1], t * k[0] * k[1] + s * k[2], t * k[1] * k[1] + c, t * k[1] * k[2] - s * k[0],
                  t * k[0] * k[2] - s * k[1], t * k[1] * k[2] + s * k[0], t * k[2] * k[2] + c};
    memcpy(m, r, sizeof r);
}

/* ================================================== solver ================================================== */
static const float ELB[3] = {0, -260, -50}, WRI[3] = {0, -249, 47};

static void test_math(void) {
    for (int i = 0; i < 2000; i++) {
        float R[9]; rand_rot(R, 3.14159f);
        float q[4], R2[9]; vrbody_mat_to_quat(R, q); vrbody_quat_to_mat(q, R2);
        float e = 0; for (int k = 0; k < 9; k++) e = fmaxf(e, fabsf(R[k] - R2[k]));
        CHECK(e < 2e-5f, "quat<->mat %g", e);
        CHECK(fabsf(det3(R2) - 1) < 1e-4f, "det");
    }
    float bad[4] = {NAN, 0, 0, 1}, m[9]; vrbody_quat_to_mat(bad, m);
    CHECK(m[0] == 1 && m[4] == 1 && m[8] == 1 && m[1] == 0, "quat NaN -> identidad");
    float z[4] = {0, 0, 0, 0}; vrbody_quat_to_mat(z, m); CHECK(m[0] == 1, "quat 0 -> identidad");
    float sk[9] = {1, 0.3f, 0, 0.1f, 2, 0, 0, 0, 0.5f}, o[9]; vrbody_orthonormalize(sk, o);
    CHECK(ortho_err(o) < 1e-5f && fabsf(det3(o) - 1) < 1e-4f, "orthonormalize");
    float nanm[9] = {NAN, 0, 0, 0, 1, 0, 0, 0, 1}; vrbody_orthonormalize(nanm, o); CHECK(o[0] == 1 && o[4] == 1, "orthonormalize NaN");
}

static void check_solution(const VrArmRig *rig, const VrArmLimits *lim, const VrArmIn *in, const VrArmOut *o, int *n_stretch, int *n_close) {
    const float L1 = lenv(rig->elbow_off), L2 = lenv(rig->wrist_off);
    float d[3];
    for (int i = 0; i < 3; i++) d[i] = o->elbow[i] - in->shoulder[i];
    const float sc = o->stretch;      /* huesos escalados a lo largo de su eje cuando el objetivo está fuera de alcance */
    CHECK(sc >= 1.0f && sc <= 2.5001f, "estirado fuera de rango: %g", sc);
    CHECK(fabsf(lenv(d) - L1 * sc) < 0.02f * sc, "hueso 1: %g vs %g", lenv(d), L1 * sc);
    float d2[3]; for (int i = 0; i < 3; i++) d2[i] = o->wrist[i] - o->elbow[i];
    CHECK(fabsf(lenv(d2) - L2 * sc) < 0.02f * sc, "hueso 2: %g vs %g", lenv(d2), L2 * sc);
    CHECK(ortho_err(o->R_upper) < 1e-4f && fabsf(det3(o->R_upper) - 1) < 1e-4f, "R_upper no ortonormal");
    CHECK(ortho_err(o->R_fore) < 1e-4f && fabsf(det3(o->R_fore) - 1) < 1e-4f, "R_fore no ortonormal");
    CHECK(ortho_err(o->R_hand) < 1e-4f && fabsf(det3(o->R_hand) - 1) < 1e-4f, "R_hand no ortonormal");
    float t[3]; mv(t, o->R_upper, rig->elbow_off);
    CHECK(fabsf(t[0] * sc - d[0]) < 0.02f * sc && fabsf(t[1] * sc - d[1]) < 0.02f * sc && fabsf(t[2] * sc - d[2]) < 0.02f * sc, "R_upper no lleva el codo a su sitio");
    mv(t, o->R_fore, rig->wrist_off);
    CHECK(fabsf(t[0] * sc - d2[0]) < 0.02f * sc && fabsf(t[1] * sc - d2[1]) < 0.02f * sc && fabsf(t[2] * sc - d2[2]) < 0.02f * sc, "R_fore no lleva la muñeca a su sitio");
    /* la mano lleva SIEMPRE la orientación pedida: el giro que no cabe en el antebrazo lo absorbe la mano */
    float ht[9]; vrbody_orthonormalize(in->hand, ht);
    float he = 0; for (int i = 0; i < 9; i++) he = fmaxf(he, fabsf(ht[i] - o->R_hand[i]));
    CHECK(he < 2e-3f, "R_hand no es la orientación pedida: %g", he);
    /* alcance */
    float v[3]; for (int i = 0; i < 3; i++) v[i] = in->wrist[i] - in->shoulder[i];
    float dist = lenv(v);
    float emin = lim->elbow_min_deg * 3.14159265f / 180;
    float dmin = sqrtf(L1 * L1 + L2 * L2 - 2 * L1 * L2 * cosf(emin)), dmax = (L1 + L2) * lim->reach_margin;
    float wv[3]; for (int i = 0; i < 3; i++) wv[i] = o->wrist[i] - in->shoulder[i];
    if (dist > dmax) {
        (*n_stretch)++;
        CHECK(o->flags & VRARM_STRETCHED, "falta STRETCHED");
        /* estirado: los huesos se alargan y la muñeca queda EXACTAMENTE en el objetivo (hasta 4 veces el alcance) */
        if (dist <= 2.5f * dmax) {
            if (sc >= 1.1f) CHECK(o->err_mm < 0.05f, "estirado: muñeca separada del objetivo %g mm", o->err_mm);
            CHECK(fabsf(sc - dist / dmax) < 1e-3f * sc, "factor de estirado %g vs %g", sc, dist / dmax);
            if (sc >= 1.1f) CHECK(lenv(wv) > dmax, "estirado: alcance %g", lenv(wv));
        }
    } else if (dist >= dmin) {
        if (!(o->flags & VRARM_SHOULDER_LIMIT)) CHECK(o->err_mm < 0.03f, "no alcanzó el objetivo alcanzable: %g (flags %u)", o->err_mm, o->flags);
    } else {
        (*n_close)++;
        CHECK(o->flags & VRARM_TOO_CLOSE, "falta TOO_CLOSE");
    }
    /* codo: ángulo interior >= mínimo */
    float a[3], b[3]; for (int i = 0; i < 3; i++) { a[i] = in->shoulder[i] - o->elbow[i]; b[i] = o->wrist[i] - o->elbow[i]; }
    float ang = acosf(fmaxf(-1, fminf(1, dotv(a, b) / (lenv(a) * lenv(b)))));
    CHECK(ang >= emin - 0.002f, "ángulo del codo %g < %g", ang * 57.29578f, lim->elbow_min_deg);
    /* hombro: componentes del brazo superior en el marco del torso */
    float u[3]; for (int i = 0; i < 3; i++) u[i] = -a[i] / lenv(a);
    float X[3] = {in->torso[0], in->torso[3], in->torso[6]}, Z[3] = {in->torso[2], in->torso[5], in->torso[8]};
    float out_c = dotv(u, X) * rig->side, fwd = dotv(u, Z);
    if (sc <= 1.0f)   /* estirado: el brazo apunta al objetivo sin límite de hombro */
    CHECK(fwd >= lim->shoulder_back - 2e-3f, "hombro hacia atrás %g < %g", fwd, lim->shoulder_back);
    if (sc <= 1.0f)
    CHECK(out_c >= lim->shoulder_cross - 2e-3f, "hombro cruzando %g < %g", out_c, lim->shoulder_cross);
}

static void test_solver(void) {
    VrArmLimits lim; vrbody_limits_default(&lim);
    int n_stretch = 0, n_close = 0, n_lim = 0, n_flag_wrist = 0, solved = 0;
    for (int it = 0; it < 30000; it++) {
        VrArmRig rig = {(it & 1) ? 1.0f : -1.0f, {ELB[0], ELB[1], ELB[2]}, {WRI[0], WRI[1], WRI[2]}};
        VrArmIn in; memset(&in, 0, sizeof in);
        rand_rot(in.torso, 0.5f);
        float tw[3] = {rig.side * 190, 260, -29}, off[3]; mv(off, in.torso, tw);
        for (int i = 0; i < 3; i++) { in.shoulder[i] = frand(-8000, 8000) + off[i]; }
        float r = (it % 10 == 0) ? frand(0, 120) : frand(0, 1100), dir[3] = {frand(-1, 1), frand(-1, 1), frand(-1, 1)}, dl = lenv(dir);
        if (dl < 1e-3f) continue;
        for (int i = 0; i < 3; i++) in.wrist[i] = in.shoulder[i] + dir[i] / dl * r;
        rand_rot(in.hand, 3.14159f);
        float pole[3] = {frand(-1, 1) * rig.side, -1, frand(-0.6f, 0.2f)}; memcpy(in.pole, pole, sizeof pole);
        if (it % 97 == 0) { for (int i = 0; i < 3; i++) in.pole[i] = (in.wrist[i] - in.shoulder[i]); }   /* polar paralelo al brazo */
        VrArmOut o; memset(&o, 0, sizeof o);
        int rc = vrbody_solve_arm(&rig, &lim, &in, &o);
        CHECK(rc == 0, "solve rc=%d", rc);
        if (rc) continue;
        solved++;
        if (o.flags & VRARM_SHOULDER_LIMIT) n_lim++;
        if (o.flags & VRARM_WRIST_LIMIT) n_flag_wrist++;
        check_solution(&rig, &lim, &in, &o, &n_stretch, &n_close);
    }
    CHECK(n_stretch > 1000 && n_close > 100 && n_lim > 50 && n_flag_wrist > 10, "cobertura de casos: estirados %d cerca %d hombro %d muñeca %d", n_stretch, n_close, n_lim, n_flag_wrist);
    printf("  solver: %d casos, estirados %d, demasiado cerca %d, límite de hombro %d, de muñeca %d\n", solved, n_stretch, n_close, n_lim, n_flag_wrist);
}

static void test_solver_nonfinite(void) {
    VrArmLimits lim; vrbody_limits_default(&lim);
    VrArmRig rig = {-1, {ELB[0], ELB[1], ELB[2]}, {WRI[0], WRI[1], WRI[2]}};
    VrArmIn in; memset(&in, 0, sizeof in);
    in.torso[0] = in.torso[4] = in.torso[8] = 1; in.hand[0] = in.hand[4] = in.hand[8] = 1;
    in.pole[1] = -1; in.wrist[2] = 300;
    VrArmOut o, ref; memset(&o, 0x5A, sizeof o); memcpy(&ref, &o, sizeof o);
    CHECK(vrbody_solve_arm(&rig, &lim, &in, &o) == 0, "caso normal");
    float *fields[] = {in.shoulder, in.torso, in.wrist, in.hand, in.pole};
    int lens[] = {3, 9, 3, 9, 3};
    for (int f = 0; f < 5; f++) for (int k = 0; k < lens[f]; k++) for (int w = 0; w < 2; w++) {
        float save = fields[f][k]; fields[f][k] = w ? INFINITY : NAN;
        memset(&o, 0x5A, sizeof o);
        CHECK(vrbody_solve_arm(&rig, &lim, &in, &o) == -1, "no finito debe fallar");
        CHECK(memcmp(&o, &ref, sizeof o) == 0, "out tocado con entrada no finita");
        fields[f][k] = save;
    }
    VrArmRig bad = rig; bad.elbow_off[0] = bad.elbow_off[1] = bad.elbow_off[2] = 0;
    CHECK(vrbody_solve_arm(&bad, &lim, &in, &o) == -1, "hueso de longitud 0");
    CHECK(vrbody_solve_arm(NULL, &lim, &in, &o) == -1 && vrbody_solve_arm(&rig, NULL, &in, &o) == -1 && vrbody_solve_arm(&rig, &lim, NULL, &o) == -1 && vrbody_solve_arm(&rig, &lim, &in, NULL) == -1, "NULL");
    /* objetivo exactamente en el hombro, polar degenerado, torso sin ortonormalizar */
    in.wrist[0] = in.wrist[1] = in.wrist[2] = 0; in.pole[0] = in.pole[1] = in.pole[2] = 0;
    for (int i = 0; i < 9; i++) in.torso[i] *= 1.003f;
    CHECK(vrbody_solve_arm(&rig, &lim, &in, &o) == 0 && isfinite(o.elbow[0]) && isfinite(o.R_hand[0]), "objetivo en el hombro con polar nulo");
    int s = 0, c = 0; check_solution(&rig, &lim, &in, &o, &s, &c);
}

/* Estirado continuo: cruzar el alcance máximo no da saltos y la muñeca siempre llega al objetivo */
static void test_stretch_continuous(void) {
    VrArmLimits lim; vrbody_limits_default(&lim);
    const float L1 = lenv(ELB), L2 = lenv(WRI), dmax = (L1 + L2) * lim.reach_margin;
    for (int side = -1; side <= 1; side += 2) {
        VrArmRig rig = {(float)side, {ELB[0], ELB[1], ELB[2]}, {WRI[0], WRI[1], WRI[2]}};
        VrArmIn in; memset(&in, 0, sizeof in);
        in.torso[0] = in.torso[4] = in.torso[8] = 1;
        in.shoulder[0] = side * 190; in.shoulder[1] = 260; in.shoulder[2] = -29;
        in.pole[0] = 0.45f * side; in.pole[1] = -1; in.pole[2] = -0.35f;
        float dir[3] = {0.1f * side, 0.2f, 0.97f}; float dl = lenv(dir); for (int i = 0; i < 3; i++) dir[i] /= dl;
        VrArmOut prev; memset(&prev, 0, sizeof prev); int have = 0;
        for (float k = 1.0f; k <= 2.001f; k += 0.002f) {
            for (int i = 0; i < 3; i++) in.wrist[i] = in.shoulder[i] + dir[i] * dmax * k;
            in.has_prev = have; if (have) { in.tau_prev = prev.tau; in.tau_f_prev = prev.tau_f; in.swivel_prev = prev.swivel; }   /* secuencia: el codo es continuo con el cálculo anterior */
            VrArmOut o; memset(&o, 0, sizeof o);
            CHECK(vrbody_solve_arm(&rig, &lim, &in, &o) == 0, "solve");
            if (k >= 1.1f) CHECK(o.err_mm < 0.05f, "k=%g: muñeca separada del objetivo %g mm", k, o.err_mm);   /* entre 1.0 y 1.1 el límite de hombro se desvanece */
            CHECK(fabsf(o.stretch - (k > 1 ? k : 1.0f)) < 1e-3f, "k=%g: estirado %g", k, o.stretch);
            if (have) {
                float de = sqrtf((o.elbow[0] - prev.elbow[0]) * (o.elbow[0] - prev.elbow[0]) + (o.elbow[1] - prev.elbow[1]) * (o.elbow[1] - prev.elbow[1]) + (o.elbow[2] - prev.elbow[2]) * (o.elbow[2] - prev.elbow[2]));
                CHECK(de < 8.0f, "k=%g: salto del codo %g mm", k, de);
            }
            prev = o; have = 1;
        }
        /* a ambos lados del alcance máximo el codo apenas se mueve */
        VrArmOut oa, ob;
        for (int i = 0; i < 3; i++) in.wrist[i] = in.shoulder[i] + dir[i] * dmax * 0.9999f;
        CHECK(vrbody_solve_arm(&rig, &lim, &in, &oa) == 0, "solve");
        for (int i = 0; i < 3; i++) in.wrist[i] = in.shoulder[i] + dir[i] * dmax * 1.0001f;
        CHECK(vrbody_solve_arm(&rig, &lim, &in, &ob) == 0, "solve");
        CHECK(fabsf(oa.elbow[0] - ob.elbow[0]) + fabsf(oa.elbow[1] - ob.elbow[1]) + fabsf(oa.elbow[2] - ob.elbow[2]) < 1.5f, "salto del codo al cruzar el alcance");
    }
}


static float rot_angle(const float *A, const float *B) {      /* ángulo de A^T B */
    float t = 0; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) t += A[3 * i + j] * B[3 * i + j];
    return acosf(fmaxf(-1, fminf(1, (t - 1) * 0.5f)));
}

/* vrbody_stretch_bone: lo largo del hueso se escala, lo perpendicular no */
static void test_stretch_bone(void) {
    float R[9]; rand_rot(R, 3.0f);
    float off[3] = {ELB[0], ELB[1], ELB[2]}, a[3], b[3], p[3] = {off[1], -off[0], 0}, pa[3], pb[3];
    mv(a, R, off); mv(pa, R, p);
    float R2[9]; memcpy(R2, R, sizeof R);
    vrbody_stretch_bone(R2, off, 2.0f);
    mv(b, R2, off); mv(pb, R2, p);
    CHECK(fabsf(lenv(b) - 2 * lenv(a)) < 1e-2f, "el hueso no se alarga: %g vs %g", lenv(b), 2 * lenv(a));
    CHECK(fabsf(pb[0] - pa[0]) + fabsf(pb[1] - pa[1]) + fabsf(pb[2] - pa[2]) < 1e-3f, "lo perpendicular cambió");
    float R3[9]; memcpy(R3, R, sizeof R); vrbody_stretch_bone(R3, off, 1.0f);
    CHECK(memcmp(R3, R, sizeof R) == 0, "s = 1 no debe tocar la matriz");
}

/* La ruta de la mano viva (vrbody_arm_resolve) entrega las matrices ya alargadas: la malla llega al hueso siguiente */
static void test_resolve_stretch(void) {
    VrBodyArmLive a; memset(&a, 0, sizeof a);
    vrbody_limits_default(&a.lim);
    a.rig.side = 1; for (int i = 0; i < 3; i++) { a.rig.elbow_off[i] = ELB[i]; a.rig.wrist_off[i] = WRI[i]; }
    a.upm = 1000;
    a.shoulder[0] = 0.19f; a.shoulder[1] = 0.26f; a.shoulder[2] = -0.029f;
    a.torso[0] = a.torso[4] = a.torso[8] = 1; a.hand[0] = a.hand[4] = a.hand[8] = 1;
    a.pole[0] = 0.4f; a.pole[1] = -0.9f; a.pole[2] = -0.3f;
    a.wrist[0] = 0.19f; a.wrist[1] = 0.26f; a.wrist[2] = 0.971f;       /* a 1 m del hombro: muy lejos del alcance (0,51 m) */
    memcpy(a.grip_p, a.wrist, sizeof a.wrist); a.grip_q[3] = 1; a.fade = 1;
    float q[4] = {0, 0, 0, 1};
    VrBodyArmPose o; CHECK(vrbody_arm_resolve(&a, a.grip_p, q, &o) == 0, "resolve");
    CHECK(o.stretch > 1.5f && o.stretch <= 2.5001f, "factor de estirado %g", o.stretch);
    float t[3], d[3];
    mv(t, o.R_upper, a.rig.elbow_off); for (int i = 0; i < 3; i++) d[i] = (o.elbow[i] - o.shoulder[i]) * 1000;
    CHECK(fabsf(t[0] - d[0]) + fabsf(t[1] - d[1]) + fabsf(t[2] - d[2]) < 0.5f, "R_upper no alcanza el codo: %g,%g,%g vs %g,%g,%g", t[0], t[1], t[2], d[0], d[1], d[2]);
    mv(t, o.R_fore, a.rig.wrist_off); for (int i = 0; i < 3; i++) d[i] = (o.wrist[i] - o.elbow[i]) * 1000;
    CHECK(fabsf(t[0] - d[0]) + fabsf(t[1] - d[1]) + fabsf(t[2] - d[2]) < 0.5f, "R_fore no alcanza la muñeca");
    VrBodyArmPose o2; a.wrist[2] = 0.45f; memcpy(a.grip_p, a.wrist, sizeof a.wrist);
    CHECK(vrbody_arm_resolve(&a, a.grip_p, q, &o2) == 0 && fabsf(o2.stretch - 1.0f) < 1e-6f, "alcanzable: sin estirado");
}

/* El límite de hombro se desvanece con el estirado: sin saltos del codo al cruzar el alcance, también con objetivos que activan el límite */
static void test_shoulder_blend(void) {
    VrArmLimits lim; vrbody_limits_default(&lim);
    const float dmax = (lenv(ELB) + lenv(WRI)) * lim.reach_margin;
    int limited = 0;
    for (int it = 0; it < 60; it++) {
        VrArmRig rig = {(it & 1) ? 1.0f : -1.0f, {ELB[0], ELB[1], ELB[2]}, {WRI[0], WRI[1], WRI[2]}};
        VrArmIn in; memset(&in, 0, sizeof in);
        in.torso[0] = in.torso[4] = in.torso[8] = 1; in.hand[0] = in.hand[4] = in.hand[8] = 1;
        in.shoulder[0] = rig.side * 190; in.shoulder[1] = 260; in.shoulder[2] = -29;
        in.pole[0] = 0.45f * rig.side; in.pole[1] = -1; in.pole[2] = -0.35f;
        float dir[3] = {frand(-1, 0.2f) * rig.side, frand(-0.5f, 0.5f), frand(-1, -0.2f)}, dl = lenv(dir);     /* hacia atrás / cruzando el pecho */
        for (int i = 0; i < 3; i++) dir[i] /= dl;
        VrArmOut prev; memset(&prev, 0, sizeof prev); int have = 0; unsigned fl0 = 0;
        for (float k = 0.97f; k <= 1.25f; k += 0.002f) {
            for (int i = 0; i < 3; i++) in.wrist[i] = in.shoulder[i] + dir[i] * dmax * k;
            in.has_prev = have; if (have) { in.tau_prev = prev.tau; in.tau_f_prev = prev.tau_f; in.swivel_prev = prev.swivel; }   /* secuencia: el codo es continuo con el cálculo anterior */
            VrArmOut o; memset(&o, 0, sizeof o);
            CHECK(vrbody_solve_arm(&rig, &lim, &in, &o) == 0, "solve");
            if (!have) fl0 = o.flags;
            if (have && k >= 1.0f) {
                float de = 0; for (int i = 0; i < 3; i++) de += (o.elbow[i] - prev.elbow[i]) * (o.elbow[i] - prev.elbow[i]);
                CHECK(sqrtf(de) < 12.0f, "it %d k=%g: salto del codo %g mm", it, k, sqrtf(de));
            }
            prev = o; have = 1;
        }
        if (fl0 & VRARM_SHOULDER_LIMIT) limited++;
    }
    CHECK(limited > 5, "cobertura: casos con límite de hombro %d", limited);
}

/* El giro del antebrazo es continuo al pasar la mano por +-180 grados respecto al antebrazo; la mano sigue exacta */
static void test_twist_continuity(void) {
    VrArmLimits lim; vrbody_limits_default(&lim);
    lim.swivel_max_deg = 0;   /* solo el giro del antebrazo (el codo, en test_wrist_continuity) */
    VrArmRig rig = {1.0f, {ELB[0], ELB[1], ELB[2]}, {WRI[0], WRI[1], WRI[2]}};
    VrArmIn in; memset(&in, 0, sizeof in);
    in.torso[0] = in.torso[4] = in.torso[8] = 1;
    in.shoulder[0] = 190; in.shoulder[1] = 260; in.shoulder[2] = -29;
    in.wrist[0] = 190; in.wrist[1] = 260; in.wrist[2] = 300;
    in.pole[0] = 0.45f; in.pole[1] = -1; in.pole[2] = -0.35f;
    VrArmOut prev; memset(&prev, 0, sizeof prev); int have = 0; float jump_max = 0, tf_max = 0;
    for (int a = 0; a <= 400; a += 2) {
        float h[9], ax[3] = {0, 0, 1}, c = cosf(a * 0.0174532925f), s_ = sinf(a * 0.0174532925f);
        (void)ax;
        float Rz[9] = {c, -s_, 0, s_, c, 0, 0, 0, 1}, R0[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        mm(h, R0, Rz); memcpy(in.hand, h, sizeof h);      /* giro de la mano sobre su eje Z (el antebrazo apunta a +Z) */
        VrArmOut o; memset(&o, 0, sizeof o);
        in.has_prev = have; if (have) { in.tau_prev = prev.tau; in.tau_f_prev = prev.tau_f; in.swivel_prev = prev.swivel; }
        CHECK(vrbody_solve_arm(&rig, &lim, &in, &o) == 0, "solve");
        float he = 0; for (int i = 0; i < 9; i++) he = fmaxf(he, fabsf(o.R_hand[i] - h[i]));
        CHECK(he < 2e-3f, "mano no exacta: %g", he);
        if (have) {
            float j = rot_angle(prev.R_fore, o.R_fore) * 57.29578f;
            if (j > jump_max) jump_max = j;
            float dtf = fabsf(remainderf(o.tau_f - prev.tau_f, 6.2831853f)) * 57.29578f; if (dtf > tf_max) tf_max = dtf;
        }
        prev = o; have = 1;
    }
    CHECK(jump_max < 8.0f, "el antebrazo salta %g grados entre pasos de 2 grados", jump_max);
    CHECK(tf_max <= 8.0f, "giro del antebrazo por paso %g > 8 grados (sigue a la mano)", tf_max);
}

/* El brazo es natural: con la mano delante del pecho el codo cuelga (por debajo de la recta hombro-muñeca) y hacia fuera */
static void test_natural_elbow(void) {
    VrArmLimits lim; vrbody_limits_default(&lim);
    for (int side = -1; side <= 1; side += 2) {
        VrArmRig rig = {(float)side, {ELB[0], ELB[1], ELB[2]}, {WRI[0], WRI[1], WRI[2]}};
        VrArmIn in; memset(&in, 0, sizeof in);
        in.torso[0] = in.torso[4] = in.torso[8] = 1;
        in.shoulder[0] = side * 190; in.shoulder[1] = 260; in.shoulder[2] = -29;
        /* varios destinos delante / altos / bajos */
        float tg[][3] = {{0, 100, 450}, {side * 150, 300, 480}, {0, 500, 300}, {side * 400, 150, 200}, {side * 100, -100, 300}, {0, 0, 350}};
        for (unsigned k = 0; k < sizeof tg / sizeof *tg; k++) {
            for (int i = 0; i < 3; i++) in.wrist[i] = tg[k][i];
            float pol[3] = {0.45f * side, -1, -0.35f}; memcpy(in.pole, pol, sizeof pol);
            {   /* mano natural: dedos (-Y local) en la dirección hombro -> muñeca, palma hacia dentro (pulgar arriba) */
                float z[3] = {in.wrist[0] - in.shoulder[0], in.wrist[1] - in.shoulder[1], in.wrist[2] - in.shoulder[2]}, zl = lenv(z), up[3] = {0, 1, 0}, x[3], y[3];
                for (int i = 0; i < 3; i++) z[i] /= zl;
                y[0] = up[0] - z[0] * dotv(up, z); y[1] = up[1] - z[1] * dotv(up, z); y[2] = up[2] - z[2] * dotv(up, z); float yl = lenv(y); for (int i = 0; i < 3; i++) y[i] /= yl;
                x[0] = -z[1] * y[2] + z[2] * y[1]; x[1] = -z[2] * y[0] + z[0] * y[2]; x[2] = -z[0] * y[1] + z[1] * y[0];   /* x = (-z) x y: base derecha con Y local = -z, Z local = y */
                float H[9] = {x[0], -z[0], y[0], x[1], -z[1], y[1], x[2], -z[2], y[2]}; memcpy(in.hand, H, sizeof H);
            }
            VrArmOut o; CHECK(vrbody_solve_arm(&rig, &lim, &in, &o) == 0, "solve");
            /* el codo no puede quedar por encima del hombro cuando la mano está a la altura del pecho o más baja */
            if (tg[k][1] < 300) CHECK(o.elbow[1] < in.shoulder[1] + 5, "codo por encima del hombro (lado %d, caso %u): %g", side, k, o.elbow[1]);
            /* el codo queda del lado de fuera o por debajo de la recta */
            float mid[3] = {(in.shoulder[0] + o.wrist[0]) / 2, (in.shoulder[1] + o.wrist[1]) / 2, (in.shoulder[2] + o.wrist[2]) / 2};
            CHECK(o.elbow[1] <= mid[1] + 1.0f, "codo por encima de la recta (lado %d, caso %u)", side, k);
            /* flexión hacia delante: la muñeca queda por delante del codo si la mano está delante */
            if (tg[k][2] > 300) CHECK(o.wrist[2] > o.elbow[2], "antebrazo no va hacia delante");
        }
    }
}

/* Continuidad: perturbaciones pequeñas de la entrada no producen saltos grandes (fuera de las singularidades obvias) */
static void test_continuity(void) {
    VrArmLimits lim; vrbody_limits_default(&lim);
    int bad = 0, tot = 0;
    for (int it = 0; it < 4000; it++) {
        VrArmRig rig = {(it & 1) ? 1.0f : -1.0f, {ELB[0], ELB[1], ELB[2]}, {WRI[0], WRI[1], WRI[2]}};
        VrArmIn a; memset(&a, 0, sizeof a);
        a.torso[0] = a.torso[4] = a.torso[8] = 1;
        a.shoulder[0] = rig.side * 190; a.shoulder[1] = 260; a.shoulder[2] = -29;
        float dir[3] = {frand(-1, 1), frand(-1, 1), frand(0, 1)}, dl = lenv(dir), r = frand(200, 520);
        if (dl < 0.2f) continue;
        for (int i = 0; i < 3; i++) a.wrist[i] = a.shoulder[i] + dir[i] / dl * r;
        rand_rot(a.hand, 1.0f);
        a.pole[0] = 0.45f * rig.side; a.pole[1] = -1; a.pole[2] = -0.35f;
        VrArmIn b = a; for (int i = 0; i < 3; i++) b.wrist[i] += frand(-0.5f, 0.5f);
        float dr[9]; rand_rot(dr, 0.002f); mm(b.hand, dr, a.hand);
        VrArmOut oa, ob; if (vrbody_solve_arm(&rig, &lim, &a, &oa) || vrbody_solve_arm(&rig, &lim, &b, &ob)) continue;
        tot++;
        float de = sqrtf((oa.elbow[0] - ob.elbow[0]) * (oa.elbow[0] - ob.elbow[0]) + (oa.elbow[1] - ob.elbow[1]) * (oa.elbow[1] - ob.elbow[1]) + (oa.elbow[2] - ob.elbow[2]) * (oa.elbow[2] - ob.elbow[2]));
        if (de > 6.0f) bad++;
    }
    CHECK(bad * 50 < tot, "continuidad del codo: %d de %d saltos > 6 mm", bad, tot);
    printf("  continuidad: %d de %d con salto >6 mm\n", bad, tot);
}


/* Muñeca y antebrazo continuos con mandos en poses aleatorias: el antebrazo acaba en la muñeca (< 1 mm), su giro sigue al de la mano (giro residual mano/antebrazo sobre el eje
 * del antebrazo < 10 grados) y, con la mano en una orientación alcanzable, la flexión y la desviación de la muñeca quedan dentro de los límites anatómicos (el codo gira para ello). */
static float twist_about(const float *Rf, const float *Rh, const float *ax) {
    float Rt[9] = {Rf[0], Rf[3], Rf[6], Rf[1], Rf[4], Rf[7], Rf[2], Rf[5], Rf[8]}, rel[9], q[4];
    mm(rel, Rt, Rh); vrbody_mat_to_quat(rel, q);
    if (q[3] < 0) for (int i = 0; i < 4; i++) q[i] = -q[i];
    return 2.0f * atan2f(q[0] * ax[0] + q[1] * ax[1] + q[2] * ax[2], q[3]) * 57.29578f;
}
static void wrist_angles(const VrArmOut *o, float side, float *flex, float *dev) {
    float f[3] = {o->wrist[0] - o->elbow[0], o->wrist[1] - o->elbow[1], o->wrist[2] - o->elbow[2]}, l = lenv(f), fl[3];
    for (int i = 0; i < 3; i++) f[i] /= l;
    const float *R = o->R_hand;
    fl[0] = R[0] * f[0] + R[3] * f[1] + R[6] * f[2]; fl[1] = R[1] * f[0] + R[4] * f[1] + R[7] * f[2]; fl[2] = R[2] * f[0] + R[5] * f[1] + R[8] * f[2];
    const float palm = -side;   /* rig.side +1 = brazo izquierdo: palma hacia -X local */
    *flex = atan2f(-fl[0] * palm, -fl[1]) * 57.29578f;
    *dev = atan2f(-fl[2], -fl[1]) * 57.29578f;
}
static void test_wrist_continuity(void) {
    VrArmLimits lim; vrbody_limits_default(&lim);
    float worst_gap = 0, worst_roll = 0; int n = 0, in_lim = 0, nat = 0;
    for (int seq = 0; seq < 40; seq++) {
        VrArmRig rig = {(seq & 1) ? 1.0f : -1.0f, {ELB[0], ELB[1], ELB[2]}, {WRI[0], WRI[1], WRI[2]}};
        VrArmIn in; memset(&in, 0, sizeof in);
        in.torso[0] = in.torso[4] = in.torso[8] = 1;
        in.shoulder[0] = rig.side * 190; in.shoulder[1] = 260; in.shoulder[2] = -29;
        in.pole[0] = 0.45f * rig.side; in.pole[1] = -1; in.pole[2] = -0.35f;
        const int natural = seq < 20;
        float dir[3] = {frand(-0.6f, 0.6f), frand(-0.6f, 0.5f), 1}, dl = lenv(dir), r = frand(250, 480);
        for (int i = 0; i < 3; i++) in.wrist[i] = in.shoulder[i] + dir[i] / dl * r;
        float H[9];
        if (natural) {   /* dedos (-Y local) hacia delante en la dirección del brazo, giro y desvío aleatorios */
            float z[3] = {dir[0] / dl, dir[1] / dl, dir[2] / dl}, up[3] = {0, 1, 0}, x[3], y[3];
            x[0] = up[1] * z[2] - up[2] * z[1]; x[1] = up[2] * z[0] - up[0] * z[2]; x[2] = up[0] * z[1] - up[1] * z[0]; float xl = lenv(x); for (int i = 0; i < 3; i++) x[i] /= xl;
            y[0] = z[1] * x[2] - z[2] * x[1]; y[1] = z[2] * x[0] - z[0] * x[2]; y[2] = z[0] * x[1] - z[1] * x[0];
            float B[9] = {x[0], -z[0], y[0], x[1], -z[1], y[1], x[2], -z[2], y[2]};   /* local X = x, local -Y = z (dedos delante), local Z = y */
            float tw[9], rr[9]; float a = frand(-1.6f, 1.6f), c = cosf(a), s_ = sinf(a);
            float Ry[9] = {c, 0, s_, 0, 1, 0, -s_, 0, c};   /* giro sobre el eje de los dedos (Y local) */
            mm(tw, B, Ry); rand_rot(rr, 0.6f); mm(H, rr, tw);
        } else rand_rot(H, 3.14159f);
        memcpy(in.hand, H, sizeof H);
        VrArmOut prev; memset(&prev, 0, sizeof prev); int have = 0;
        for (int it = 0; it < 150; it++) {
            float dr[9], h2[9]; rand_rot(dr, 0.06f); mm(h2, dr, in.hand); vrbody_orthonormalize(h2, in.hand);
            for (int i = 0; i < 3; i++) in.wrist[i] += frand(-6, 6);
            in.has_prev = have; if (have) { in.tau_prev = prev.tau; in.tau_f_prev = prev.tau_f; in.swivel_prev = prev.swivel; }
            VrArmOut o;
            if (vrbody_solve_arm(&rig, &lim, &in, &o) != 0) { CHECK(0, "solve"); continue; }
            float off[3], ax[3], e[3];
            mv(off, o.R_fore, rig.wrist_off);
            for (int i = 0; i < 3; i++) e[i] = o.elbow[i] + off[i] * o.stretch - o.wrist[i];
            float gap = lenv(e); if (gap > worst_gap) worst_gap = gap;
            float wl = lenv(rig.wrist_off); for (int i = 0; i < 3; i++) ax[i] = rig.wrist_off[i] / wl;
            float roll = fabsf(twist_about(o.R_fore, o.R_hand, ax)); if (roll > worst_roll) worst_roll = roll;
            if (natural && !(o.flags & (VRARM_STRETCHED | VRARM_TOO_CLOSE))) {
                float fx, dv; wrist_angles(&o, rig.side, &fx, &dv);
                nat++;
                if (fx <= lim.wrist_flex_deg + 5 && fx >= -lim.wrist_ext_deg - 5 && dv <= lim.wrist_rad_deg + 5 && dv >= -lim.wrist_uln_deg - 5) in_lim++;
            }
            prev = o; have = 1; n++;
        }
    }
    CHECK(worst_gap < 1.0f, "el antebrazo no llega a la muñeca: %.3f mm", worst_gap);
    CHECK(worst_roll < 10.0f, "giro residual mano/antebrazo %.1f grados", worst_roll);
    CHECK(in_lim >= nat * 9 / 10, "muñeca dentro de los límites en %d de %d poses naturales", in_lim, nat);
    printf("  muñeca: %d poses, hueco máx %.3f mm, giro residual máx %.1f grados, dentro de límites %d de %d\n", n, worst_gap, worst_roll, in_lim, nat);
}

/* =========================================== pegamento sobre RAM sintética =========================================== */
#define RAMSZ 0x200000
static uint8_t RAM[RAMSZ], RAM0[RAMSZ];

int vrpose_get_hands(VrHandPose out[2]);       /* definición fuerte de prueba (sustituye a la débil) */
static VrHandPose g_hands[2];
int vrpose_get_hands(VrHandPose out[2]) { memcpy(out, g_hands, sizeof g_hands); return g_hands[0].valid + g_hands[1].valid; }
static VrBodyView g_view;
static int g_view_ok;
static int view_fn(VrBodyView *v) { if (!g_view_ok) return 0; *v = g_view; return 1; }
/* Configuración de las pruebas anteriores al torso que sigue la mirada: el torso original no se toca (el nuevo comportamiento tiene sus propias pruebas). */
static void legacy_cfg(VrBodyConfig *c) { vrbody_config_default(c); c->torso_follow = 0; c->near_hide_m = c->near_hide_vtx_m = 0; }

static void w16(uint32_t a, int16_t v) { memcpy(RAM + (a & 0x1FFFFF), &v, 2); }
static void w32(uint32_t a, int32_t v) { memcpy(RAM + (a & 0x1FFFFF), &v, 4); }
static int32_t r32(uint32_t a) { int32_t v; memcpy(&v, RAM + (a & 0x1FFFFF), 4); return v; }
static int16_t r16(uint32_t a) { int16_t v; memcpy(&v, RAM + (a & 0x1FFFFF), 2); return v; }

#define OB   0x8016cb44u
#define PARTSET 0x8016d518u
#define PSET_DEF  0x8013e7f8u
#define PMODELS 0x8013e818u     /* 16 modelos de 0x3C (solo lo que lee vr_body) */
static const int PARENT[16] = {-1, 0, 1, 2, 3, 1, 5, 1, 7, 8, 0, 10, 11, 0, 13, 14};
static const int TOFF[16][3] = {{0, 0, 0}, {0, 5, 0}, {-190, 260, -29}, {0, -260, -50}, {0, -249, 47}, {0, 305, -55}, {0, 95, 59}, {190, 260, -29},
                                {0, -260, -50}, {0, -249, 47}, {-90, -255, -10}, {0, -311, 37}, {0, -508, -47}, {90, -253, -11}, {0, -312, 39}, {0, -508, -47}};
static uint32_t part_addr(int i) { return PARTSET + 0x48 + 0x5C * (uint32_t)i; }

static void store_xf(uint32_t a, const float *R, const float *t) {
    for (int i = 0; i < 9; i++) w16(a + 2 * i, (int16_t)lrintf(R[i] * 4096));
    for (int i = 0; i < 3; i++) w32(a + 0x14 + 4 * i, (int32_t)lrintf(t[i]));
}
static void load_xf(uint32_t a, float *R, float *t) {
    for (int i = 0; i < 9; i++) R[i] = r16(a + 2 * i) / 4096.0f;
    for (int i = 0; i < 3; i++) t[i] = (float)r32(a + 0x14 + 4 * i);
}
static float EYE_R[9], EYE_t[3];

/* Crea el modelo sintético: FK de rotaciones aleatorias pequeñas (brazos colgando) y matrices screen = EYE o world */
static void build_body(int seed) {
    rs = 0x1234567ull + (uint64_t)seed * 7919;
    memset(RAM, 0, sizeof RAM);
    w32(BA_PLAYER_BODY, (int32_t)OB);
    w32(OB, (int32_t)PARTSET);
    w16(PARTSET + 0x2E, 16);
    w16(PARTSET + 0x32, 2);
    w32(PARTSET + 0x24, (int32_t)PSET_DEF);
    float Rw[16][9], tw[16][3];
    float yaw = frand(-3.14f, 3.14f), cy = cosf(yaw), sy = sinf(yaw);
    float Rroot[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy};
    float troot[3] = {frand(-6000, 6000), 1049, frand(-3000, 3000)};
    rand_rot(EYE_R, 1.0f); EYE_t[0] = frand(-4000, 4000); EYE_t[1] = frand(-4000, 4000); EYE_t[2] = frand(-4000, 4000);
    for (int i = 0; i < 16; i++) {
        uint32_t mdl = PMODELS + 0x3C * (uint32_t)i;
        w32(part_addr(i) + 0x40, (int32_t)mdl);
        w32(mdl + 0x20, TOFF[i][0]); w32(mdl + 0x24, TOFF[i][1]); w32(mdl + 0x28, TOFF[i][2]);
        w32(mdl + 0x2C, PARENT[i]);
        w16(part_addr(i) + 0x4C, 2);
        float Rl[9]; rand_rot(Rl, i == 3 || i == 8 ? 0.7f : 0.25f);
        const float *Rp = PARENT[i] < 0 ? Rroot : Rw[PARENT[i]], *tp = PARENT[i] < 0 ? troot : tw[PARENT[i]];
        float tl[3] = {(float)TOFF[i][0], (float)TOFF[i][1], (float)TOFF[i][2]};
        mm(Rw[i], Rp, Rl);
        mv(tw[i], Rp, tl); for (int k = 0; k < 3; k++) tw[i][k] += tp[k];
        store_xf(part_addr(i), Rw[i], tw[i]);
        /* screen = EYE o world (con las rotaciones ya redondeadas) */
        float Rq[9], tq[3]; load_xf(part_addr(i), Rq, tq);
        float Rs[9], ts[3]; mm(Rs, EYE_R, Rq); mv(ts, EYE_R, tq); for (int k = 0; k < 3; k++) ts[k] += EYE_t[k];
        store_xf(part_addr(i) + 0x20, Rs, ts);
    }
    /* matriz de vista del canal (mundo->vista del canal) con la misma redondeada que la usada */
    float Rc[9]; for (int i = 0; i < 9; i++) { w16(BA_VIEW_CHANNEL + BCH_CAM_INV + 2 * i, (int16_t)lrintf(EYE_R[i] * 4096)); Rc[i] = r16(BA_VIEW_CHANNEL + BCH_CAM_INV + 2 * i) / 4096.0f; }
    (void)Rc;
    for (int k = 0; k < 3; k++) w32(BA_VIEW_CHANNEL + BCH_CAM_INV + 0x14 + 4 * k, (int32_t)lrintf(EYE_t[k]));
    /* recalcula screen con los R de la matriz de vista redondeados, para que coincida exactamente */
    float Re[9]; for (int i = 0; i < 9; i++) Re[i] = r16(BA_VIEW_CHANNEL + BCH_CAM_INV + 2 * i) / 4096.0f;
    for (int i = 0; i < 16; i++) {
        float Rq[9], tq[3]; load_xf(part_addr(i), Rq, tq);
        float Rs[9], ts[3]; mm(Rs, Re, Rq); mv(ts, Re, tq); for (int k = 0; k < 3; k++) ts[k] += (float)r32(BA_VIEW_CHANNEL + BCH_CAM_INV + 0x14 + 4 * k);
        store_xf(part_addr(i) + 0x20, Rs, ts);
    }
    memset(g_hands, 0, sizeof g_hands);
    g_view_ok = 1; memset(&g_view, 0, sizeof g_view);
    g_view.active = 1; g_view.units_per_m = 1000; g_view.theta_deg = frand(0, 360);
    g_view.eye[0] = troot[0]; g_view.eye[1] = troot[1] + 700; g_view.eye[2] = troot[2];
    g_view.p_ref[0] = 0.1f; g_view.p_ref[1] = 1.6f; g_view.p_ref[2] = -0.2f;
    memcpy(RAM0, RAM, sizeof RAM);
}

static void set_hand(int s, float x, float y, float z, float yaw) {
    g_hands[s].valid = 1;
    g_hands[s].pos[0] = x; g_hands[s].pos[1] = y; g_hands[s].pos[2] = z;
    float h = yaw * 0.5f; g_hands[s].rot[0] = 0; g_hands[s].rot[1] = sinf(h); g_hands[s].rot[2] = 0; g_hands[s].rot[3] = cosf(h);
}
static int is_arm_part(int i) { return i == 2 || i == 3 || i == 4 || i == 7 || i == 8 || i == 9; }

static void test_glue(void) {
    VrBodyConfig cfg; legacy_cfg(&cfg); cfg.hand_pitch_deg = 0;
    vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
    /* a) sin manos: no toca nada */
    build_body(1);
    CHECK(vrbody_apply(RAM) == 0, "sin manos");
    CHECK(memcmp(RAM, RAM0, sizeof RAM) == 0, "sin manos: RAM tocada");
    vrbody_restore(RAM);
    CHECK(memcmp(RAM, RAM0, sizeof RAM) == 0, "restore sin nada pendiente");

    /* b) dos manos: solo cambian las 6 piezas de los brazos (64 B cada una); restore devuelve byte a byte */
    for (int seed = 0; seed < 40; seed++) {
        build_body(seed + 10);
        vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
        for (int f = 0; f < 30; f++) {     /* deja que el fundido llegue a 1 */
            set_hand(0, -0.25f + 0.002f * f, 1.3f, -0.4f, 0.3f); set_hand(1, 0.30f, 1.25f, -0.45f - 0.001f * f, -0.2f);
            memcpy(RAM, RAM0, sizeof RAM);
            int n = vrbody_apply(RAM);
            CHECK(n == 2 || f < 2, "brazos modificados %d (f=%d)", n, f);
            if (n) {
                int diff_outside = 0;
                for (uint32_t a = 0; a < RAMSZ; a++) {
                    if (RAM[a] == RAM0[a]) continue;
                    int inside = 0;
                    for (int p = 0; p < 16 && !inside; p++) if (is_arm_part(p) && a >= (part_addr(p) & 0x1FFFFF) && a < (part_addr(p) & 0x1FFFFF) + 0x40) inside = 1;
                    for (int p = 0; p < 16 && !inside; p++) if (a >= ((PMODELS + 0x3C * (uint32_t)p) & 0x1FFFFF) && a < ((PMODELS + 0x3C * (uint32_t)p) & 0x1FFFFF) + 4) inside = 1;   /* model[0]: MF_TWO_SIDED */
                    if (!inside) diff_outside++;
                }
                CHECK(diff_outside == 0, "bytes fuera de las 6 piezas modificados: %d", diff_outside);
                /* conectividad: cada hueso lleva su origen al siguiente (a 1,8 mm: enteros de t + cuantización de 1/4096 por 265 mm) */
                for (int arm = 0; arm < 2; arm++) {
                    int pi[3] = {arm ? 2 : 7, arm ? 3 : 8, arm ? 4 : 9};
                    float R[3][9], t[3][3];
                    for (int k = 0; k < 3; k++) load_xf(part_addr(pi[k]), R[k], t[k]);
                    int bone_stretched[2] = {0, 0};   /* hueso alargado por el estirado: su matriz ya no es una rotación pura */
                    for (int k = 0; k < 2; k++) {
                        float o[3], tl[3] = {(float)TOFF[pi[k + 1]][0], (float)TOFF[pi[k + 1]][1], (float)TOFF[pi[k + 1]][2]};
                        { float ro[3]; mv(ro, R[k], tl); bone_stretched[k] = lenv(ro) > 1.0005f * lenv(tl); }
                        mv(o, R[k], tl); for (int c = 0; c < 3; c++) o[c] += t[k][c];
                        float d[3] = {o[0] - t[k + 1][0], o[1] - t[k + 1][1], o[2] - t[k + 1][2]};
                        CHECK(lenv(d) < 1.8f, "hueso %d del brazo %d desconectado %g mm", k, arm, lenv(d));
                    }
                    CHECK((bone_stretched[0] || ortho_err(R[0]) < 3e-3f) && (bone_stretched[1] || ortho_err(R[1]) < 3e-3f) && ortho_err(R[2]) < 3e-3f, "matrices no ortonormales (%d %g, %d %g, %g)", bone_stretched[0], ortho_err(R[0]), bone_stretched[1], ortho_err(R[1]), ortho_err(R[2]));
                    /* el hombro no se mueve (fijo al torso animado) */
                    float Ro[9], to[3]; load_xf(part_addr(pi[0]) , Ro, to);
                    int32_t a0 = r32(part_addr(pi[0]) + 0x14), b0 = (int32_t)((uint32_t)(RAM0[(part_addr(pi[0]) & 0x1FFFFF) + 0x14] | RAM0[(part_addr(pi[0]) & 0x1FFFFF) + 0x15] << 8 | RAM0[(part_addr(pi[0]) & 0x1FFFFF) + 0x16] << 16 | (uint32_t)RAM0[(part_addr(pi[0]) & 0x1FFFFF) + 0x17] << 24));
                    CHECK(a0 == b0, "hombro movido");
                    /* screen = EYE o world, con redondeo */
                    for (int k = 0; k < 3; k++) {
                        float Rs[9], ts[3]; load_xf(part_addr(pi[k]) + 0x20, Rs, ts);
                        float Re[9], te[3]; load_xf(BA_VIEW_CHANNEL + BCH_CAM_INV, Re, te);
                        float Rx[9], tx[3]; mm(Rx, Re, R[k]); mv(tx, Re, t[k]); for (int c = 0; c < 3; c++) tx[c] += te[c];
                        float e = 0; for (int c = 0; c < 9; c++) e = fmaxf(e, fabsf(Rx[c] - Rs[c]));
                        float et = fmaxf(fabsf(tx[0] - ts[0]), fmaxf(fabsf(tx[1] - ts[1]), fabsf(tx[2] - ts[2])));
                        CHECK(e < 2.5e-4f && et < 2.0f, "screen != vista o world (%g, %g mm)", e, et);
                    }
                }
                vrbody_restore(RAM);
                CHECK(memcmp(RAM, RAM0, sizeof RAM) == 0, "restore no devuelve la RAM original (semilla %d, frame %d)", seed, f);
                /* idempotente */
                vrbody_restore(RAM);
                CHECK(memcmp(RAM, RAM0, sizeof RAM) == 0, "restore doble");
            }
        }
    }

    /* c) la mano del modelo llega a la pose del usuario (alcanzable): el centro de la palma coincide con pos */
    build_body(77);
    vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
    {
        float shoulderR[3];     /* hombro derecho en mundo */
        float Rr[9], tr[3]; load_xf(part_addr(2), Rr, tr); memcpy(shoulderR, tr, sizeof tr);
        /* posición del usuario cuyo mundo es a 300 mm delante y 100 abajo del hombro */
        float th = (g_view.theta_deg + 180) * 3.14159265f / 180, cs = cosf(th), sn = sinf(th);
        float T[9] = {cs, 0, sn, 0, 1, 0, -sn, 0, cs}, Tt[9] = {cs, 0, -sn, 0, 1, 0, sn, 0, cs};
        float Rt1[9], tt1[3]; load_xf(part_addr(1), Rt1, tt1);
        float want[3]; for (int k = 0; k < 3; k++) want[k] = shoulderR[k] + Rt1[3 * k + 2] * 350.0f - Rt1[3 * k + 1] * 100.0f;   /* 350 mm delante y 100 mm bajo el hombro, en el marco del torso */
        (void)T;
        float dw[3] = {want[0] - g_view.eye[0], want[1] - g_view.eye[1], want[2] - g_view.eye[2]};
        float pu[3]; mv(pu, Tt, dw);
        for (int k = 0; k < 3; k++) pu[k] = pu[k] / 1000 + g_view.p_ref[k];
        for (int f = 0; f < 40; f++) { set_hand(1, pu[0], pu[1], pu[2], 0); g_hands[1].rot[1] = 0; g_hands[1].rot[3] = 1; memcpy(RAM, RAM0, sizeof RAM); vrbody_apply(RAM); vrbody_restore(RAM); }
        memcpy(RAM, RAM0, sizeof RAM);
        set_hand(1, pu[0], pu[1], pu[2], 0); vrbody_apply(RAM);
        const VrBodyDebug *d = vrbody_debug(1);
        CHECK(d->driven, "brazo derecho conducido");
        float tw[3]; for (int k = 0; k < 3; k++) tw[k] = d->sol.wrist[k];
        /* centro de agarre de la mano nueva (a tamaño real) con la orientación de la pieza mano */
        float Rh[9], th_[3], gc[3], go[3]; load_xf(part_addr(4), Rh, th_); vrhs_grip_center(1, gc);
        mv(go, Rh, gc);
        float palm[3]; for (int k = 0; k < 3; k++) palm[k] = tw[k] + go[k];
        float e[3] = {palm[0] - want[0], palm[1] - want[1], palm[2] - want[2]};
        printf("  alcance: error muñeca->palma %.2f mm (reach %.2f, flags %u)\n", lenv(e), d->sol.reach, d->sol.flags);
        CHECK(lenv(e) < 1.0f || (d->sol.flags & VRARM_STRETCHED), "la mano no llega a la pose pedida");
        vrbody_restore(RAM);
    }

    /* d) fuera de alcance: brazo extendido y huesos alargados; la muñeca llega al objetivo */
    build_body(78);
    vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
    for (int f = 0; f < 40; f++) {
        set_hand(0, -0.25f, 1.3f, -1.8f, 0); set_hand(1, 0.3f, 1.3f, -1.8f, 0);
        memcpy(RAM, RAM0, sizeof RAM); vrbody_apply(RAM);
        if (f == 39) {
            for (int s = 0; s < 2; s++) {
                const VrBodyDebug *d = vrbody_debug(s);
                CHECK(d->sol.flags & VRARM_STRETCHED, "lejos: debería estirarse");
                float S[3] = {(float)r32(part_addr(s ? 2 : 7) + 0x14), (float)r32(part_addr(s ? 2 : 7) + 0x18), (float)r32(part_addr(s ? 2 : 7) + 0x1C)};
                float dd[3] = {d->sol.wrist[0] - S[0], d->sol.wrist[1] - S[1], d->sol.wrist[2] - S[2]};
                CHECK(d->sol.stretch > 1.0f && (d->sol.err_mm < 1.0f || d->sol.stretch > 2.499f), "brazo estirado: factor %g, error %g mm", d->sol.stretch, d->sol.err_mm);
                CHECK(lenv(dd) > (264.75f + 253.4f), "brazo no alargado: %g", lenv(dd));
            }
            VrBodyStats bs_; vrbody_stats(&bs_);
            CHECK(bs_.stretched > 0 && bs_.stretch_max > 1.0f && bs_.stretch_sum >= (float)bs_.stretched, "contadores de estirado: n=%u max=%g suma=%g", bs_.stretched, bs_.stretch_max, bs_.stretch_sum);
        }
        vrbody_restore(RAM);
    }

    /* e) fundido: al perder la mano el brazo vuelve a la animación en ~0,25 s sin saltos bruscos */
    build_body(79);
    vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
    float prev[3] = {0, 0, 0}; int have_prev = 0; float maxstep = 0; int last_driven = -1;
    for (int f = 0; f < 80; f++) {
        if (f < 40) { set_hand(1, 0.3f, 1.3f, -0.6f, 0); set_hand(0, -0.3f, 1.3f, -0.6f, 0); } else memset(g_hands, 0, sizeof g_hands);
        memcpy(RAM, RAM0, sizeof RAM); int n = vrbody_apply(RAM);
        if (n) { last_driven = f; const VrBodyDebug *d = vrbody_debug(1); if (have_prev) { float st[3] = {d->sol.wrist[0] - prev[0], d->sol.wrist[1] - prev[1], d->sol.wrist[2] - prev[2]}; if (f >= 3) maxstep = fmaxf(maxstep, lenv(st)); } memcpy(prev, d->sol.wrist, sizeof prev); have_prev = 1; }
        vrbody_restore(RAM);
        CHECK(memcmp(RAM, RAM0, sizeof RAM) == 0, "restore tras fundido");
    }
    CHECK(last_driven >= 45 && last_driven <= 60, "el fundido de salida dura %d frames", last_driven - 40);
    CHECK(maxstep < 120.0f, "salto de la muñeca por frame en el fundido %g mm", maxstep);
    {   /* sin manos: nada más que hacer */
        memcpy(RAM, RAM0, sizeof RAM); CHECK(vrbody_apply(RAM) == 0, "sin manos tras el fundido");
    }

    /* f) sin vista VR, deshabilitado, objeto fuera de pantalla, modelo distinto */
    build_body(80); vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
    set_hand(0, -0.2f, 1.3f, -0.4f, 0); set_hand(1, 0.2f, 1.3f, -0.4f, 0);
    g_view_ok = 0; CHECK(vrbody_apply(RAM) == 0 && memcmp(RAM, RAM0, sizeof RAM) == 0, "sin vista");
    g_view_ok = 1; g_view.active = 0; CHECK(vrbody_apply(RAM) == 0 && memcmp(RAM, RAM0, sizeof RAM) == 0, "vista inactiva"); g_view.active = 1;
    vrbody_set_enabled(0); CHECK(vrbody_apply(RAM) == 0 && memcmp(RAM, RAM0, sizeof RAM) == 0, "deshabilitado"); vrbody_set_enabled(1);
    w16(PARTSET + 0x32, 0); memcpy(RAM0, RAM, sizeof RAM); CHECK(vrbody_apply(RAM) == 0 && memcmp(RAM, RAM0, sizeof RAM) == 0, "objeto fuera de pantalla"); w16(PARTSET + 0x32, 2);
    w16(PARTSET + 0x2E, 12); memcpy(RAM0, RAM, sizeof RAM); CHECK(vrbody_apply(RAM) == 0 && memcmp(RAM, RAM0, sizeof RAM) == 0, "modelo con 12 piezas"); w16(PARTSET + 0x2E, 16);
    w32(PMODELS + 0x3C * 3 + 0x2C, 7); memcpy(RAM0, RAM, sizeof RAM);
    { int n = vrbody_apply(RAM); CHECK(n <= 1, "jerarquía distinta: %d brazos", n); vrbody_restore(RAM); CHECK(memcmp(RAM, RAM0, sizeof RAM) == 0, "restore con jerarquía distinta"); }

    /* g) apply doble sin restore: el segundo devuelve primero los originales; restore con el modelo cambiado no escribe */
    build_body(81); vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
    for (int f = 0; f < 30; f++) { set_hand(0, -0.2f, 1.3f, -0.4f, 0); set_hand(1, 0.2f, 1.3f, -0.4f, 0); memcpy(RAM, RAM0, sizeof RAM); vrbody_apply(RAM); vrbody_restore(RAM); }
    memcpy(RAM, RAM0, sizeof RAM);
    vrbody_apply(RAM); vrbody_apply(RAM); vrbody_restore(RAM);
    CHECK(memcmp(RAM, RAM0, sizeof RAM) == 0, "apply doble + restore");
    vrbody_apply(RAM);
    w32(part_addr(3) + 0x40, (int32_t)(PMODELS + 0x3C * 9));    /* "el objeto se rehizo" */
    uint8_t keep[0x40]; memcpy(keep, RAM + (part_addr(3) & 0x1FFFFF), 0x40);
    vrbody_restore(RAM);
    CHECK(memcmp(RAM + (part_addr(3) & 0x1FFFFF), keep, 0x40) == 0, "restore escribió una pieza cuyo modelo cambió");
    CHECK(!vrbody_pending(), "pendiente tras restore");
    {   VrBodyStats st; vrbody_stats(&st); CHECK(st.stale_dropped >= 1, "pieza cambiada no descartada"); }

    /* g2) el puntero del jugador cambia entre apply y restore (zona nueva, muerte): la pieza se devuelve igualmente */
    build_body(83); vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
    for (int f = 0; f < 30; f++) { set_hand(0, -0.2f, 1.3f, -0.4f, 0); set_hand(1, 0.2f, 1.3f, -0.4f, 0); memcpy(RAM, RAM0, sizeof RAM); vrbody_apply(RAM); vrbody_restore(RAM); }
    memcpy(RAM, RAM0, sizeof RAM);
    CHECK(vrbody_apply(RAM) == 2 && memcmp(RAM, RAM0, sizeof RAM) != 0, "apply antes del cambio de objeto");
    w32(BA_PLAYER_BODY, 0);
    vrbody_restore(RAM);
    w32(BA_PLAYER_BODY, (int32_t)OB);
    CHECK(memcmp(RAM, RAM0, sizeof RAM) == 0, "restore con el puntero del jugador a 0 deja la pose IK");

    /* h) entradas de mano no válidas */
    build_body(82); vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
    g_hands[0].valid = 1; g_hands[0].pos[0] = NAN; g_hands[0].rot[3] = 1;
    g_hands[1].valid = 1; g_hands[1].pos[0] = 0; g_hands[1].rot[0] = g_hands[1].rot[1] = g_hands[1].rot[2] = g_hands[1].rot[3] = 0;
    CHECK(vrbody_apply(RAM) == 0 && memcmp(RAM, RAM0, sizeof RAM) == 0, "poses no válidas");
    g_hands[0].valid = 1; g_hands[0].pos[0] = 0; g_hands[0].pos[1] = 5000; g_hands[0].rot[3] = 1;
    CHECK(vrbody_apply(RAM) == 0 && memcmp(RAM, RAM0, sizeof RAM) == 0, "pose absurda (5 km)");
}


/* El marco de la mano del modelo sigue el marco de vr_pose.h: dedos (-Y local) = -Z de la mano, palma (derecha +X local /
 * izquierda -X local) = -Y de la mano, pulgar (+Z local) = -X (derecha) / +X (izquierda). Comprobado sobre las matrices escritas
 * en la RAM, con orientaciones de mano distintas, sin usar hand_map. */
/* mode 0: pose = marco de vr_pose.h (sin cabeceo); 1: mando de seguimiento con apuntado (agarre inclinado 35 grados, cabeceo automático); 2: sin apuntado (cabeceo fijo -35);
 * 3: calibrado con vrbody_calibrate_hands; 4: como 1 con el apuntado ausente 3 fotogramas antes de medir (se mantiene el último cabeceo); 5: apuntado ausente desde el fotograma 20 con cabeceo fijo 0 (tras 10 fotogramas vuelve al fijo: dedos 35 grados arriba). En 1..3 el marco esperado de la mano es el del apuntado horizontal (guiñada de la pose) girado el alabeo. */
static void hand_axes_run(float roll_deg, int mode) {
    VrBodyConfig cfg; legacy_cfg(&cfg); if (mode == 0 || mode == 5) cfg.hand_pitch_deg = 0;
    int tested = 0;
    for (int seed = 0; seed < 30; seed++) {
        build_body(200 + seed);
        vrbody_init(&cfg); vrbody_set_view_fn(view_fn); vrbody_set_hand_roll(roll_deg);
        float th = (g_view.theta_deg + 180) * 3.14159265f / 180, cs = cosf(th), sn = sinf(th);
        float T[9] = {cs, 0, sn, 0, 1, 0, -sn, 0, cs};
        float yaw = (float)(seed % 5 - 2) * 0.25f;
        for (int f = 0; f < 40; f++) {
            for (int s = 0; s < 2; s++) {      /* a 330 mm delante y 120 bajo cada hombro, en el marco del torso */
                float Rt1[9], tt1[3], sh[3], Rs_[9]; load_xf(part_addr(1), Rt1, tt1); load_xf(part_addr(s ? 2 : 7), Rs_, sh);
                float want[3]; for (int k = 0; k < 3; k++) want[k] = sh[k] + Rt1[3 * k + 2] * 330.0f - Rt1[3 * k + 1] * 120.0f;
                float dw[3] = {want[0] - g_view.eye[0], want[1] - g_view.eye[1], want[2] - g_view.eye[2]};
                float Tt[9] = {T[0], T[3], T[6], T[1], T[4], T[7], T[2], T[5], T[8]}, pu[3]; mv(pu, Tt, dw);
                set_hand(s, pu[0] / 1000 + g_view.p_ref[0], pu[1] / 1000 + g_view.p_ref[1], pu[2] / 1000 + g_view.p_ref[2], yaw);
                if (mode) {     /* agarre = guiñada * Rx(35 grados) (medido en el mando de seguimiento); apuntado = guiñada */
                    float h2 = yaw * 0.5f, a2 = 35.0f * 3.14159265f / 180 * 0.5f;
                    float qy[4] = {0, sinf(h2), 0, cosf(h2)}, qx[4] = {sinf(a2), 0, 0, cosf(a2)}, qg[4];
                    qg[0] = qy[3] * qx[0] + qy[0] * qx[3] + qy[1] * qx[2] - qy[2] * qx[1]; qg[1] = qy[3] * qx[1] - qy[0] * qx[2] + qy[1] * qx[3] + qy[2] * qx[0];
                    qg[2] = qy[3] * qx[2] + qy[0] * qx[1] - qy[1] * qx[0] + qy[2] * qx[3]; qg[3] = qy[3] * qx[3] - qy[0] * qx[0] - qy[1] * qx[1] - qy[2] * qx[2];
                    memcpy(g_hands[s].rot, qg, sizeof qg);
                    { int av = mode == 1 || mode == 3 || (mode == 4 && !(f >= 36 && f <= 38)) || (mode == 5 && f < 20); g_hands[s].aim_valid = av; if (av) memcpy(g_hands[s].aim_rot, qy, sizeof qy); }
                    if (mode == 3 && f > 20 && 0) {}
                }
                /* la posición de la pose es el centro de la palma: la muñeca queda 55 mm atrás; el objetivo sigue alcanzable */
            }
            memcpy(RAM, RAM0, sizeof RAM);
            if (mode == 3 && f == 10) { vrbody_set_hand_pitch(0); CHECK(vrbody_calibrate_hands() == 2, "calibrar manos"); for (int s = 0; s < 2; s++) g_hands[s].aim_valid = 0; }
            int n = vrbody_apply(RAM);
            if (f == 39 && n == 2) {
                float Ru[9]; { float q[4] = {0, sinf(yaw / 2), 0, cosf(yaw / 2)}; vrbody_quat_to_mat(q, Ru); }
                for (int s = 0; s < 2; s++) {
                    const VrBodyDebug *d = vrbody_debug(s);
                    /* marco de la mano pedido = agarre x giro sobre el eje de los dedos (izquierda +roll, derecha -roll) */
                    float ph = (s ? -roll_deg : roll_deg) * 3.14159265f / 180, cz = cosf(ph), sz = sinf(ph);
                    float Rz[9] = {cz, -sz, 0, sz, cz, 0, 0, 0, 1}, Rh[9], Rw[9]; mm(Rh, Ru, Rz); mm(Rw, T, Rh);
                    float fing[3] = {-Rw[2], -Rw[5], -Rw[8]}, up[3] = {Rw[1], Rw[4], Rw[7]}, xm[3] = {Rw[0], Rw[3], Rw[6]};
                    if (d->sol.flags & (VRARM_SHOULDER_LIMIT | VRARM_STRETCHED)) continue;
                    float R[9], t[3]; load_xf(part_addr(s ? 4 : 9), R, t);
                    float lx[3] = {R[0], R[3], R[6]}, ly[3] = {R[1], R[4], R[7]}, lz[3] = {R[2], R[5], R[8]};
                    float palm[3], thumb[3];
                    for (int k = 0; k < 3; k++) { palm[k] = (s ? lx[k] : -lx[k]); thumb[k] = lz[k]; }
                    float fl[3] = {-ly[0], -ly[1], -ly[2]};
                    float pw[3] = {-up[0], -up[1], -up[2]}, tm[3] = {s ? -xm[0] : xm[0], s ? -xm[1] : xm[1], s ? -xm[2] : xm[2]};
                    if (mode == 5) { CHECK(fabsf(fl[1] - 0.5736f) < 0.04f, "vuelta al cabeceo fijo: %g", fl[1]); tested++; continue; }
                    CHECK(dotv(fl, fing) > 0.995f, "dedos de la mano %d: %g", s, dotv(fl, fing));
                    CHECK(dotv(palm, pw) > 0.995f, "palma de la mano %d: %g", s, dotv(palm, pw));
                    CHECK(dotv(thumb, tm) > 0.995f, "pulgar de la mano %d: %g", s, dotv(thumb, tm));
                    if (mode == 0) {   /* marco de depuración: mismos ejes que la pose pedida (alcanzable, sin límites) */
                        float df[16]; int ok = vrbody_debug_hand_frame(s, df);
                        CHECK(ok, "marco de depuración de la mano %d no disponible", s);
                        if (ok) {
                            /* el marco se da en el espacio del usuario: se compara con Ru (la pose pedida), no con su versión en el mundo */
                            float dz[3] = {df[8], df[9], df[10]}, dy[3] = {df[4], df[5], df[6]}, uz[3] = {Ru[2], Ru[5], Ru[8]}, uy[3] = {Ru[1], Ru[4], Ru[7]};
                            CHECK(dotv(dz, uz) > 0.995f, "marco depuración: -Z a los dedos (mano %d): %g", s, dotv(dz, uz));
                            CHECK(dotv(dy, uy) > 0.995f, "marco depuración: +Y al dorso (mano %d): %g", s, dotv(dy, uy));
                        }
                    }
                    tested++;
                }
            }
            vrbody_restore(RAM);
        }
    }
    CHECK(tested >= 20, "pocas manos comprobadas: %d", tested);
    vrbody_clear_calibration();
}

/* Mano que empuña un arma: su corrección agarre -> mano es la FIJA (calibración o cabeceo fijo + alabeo), igual a vrbody_hand_offset, y NO sigue el apuntado del mando (cabeceo automático).
 * El arma cuelga de esa misma corrección (vr/host/vrgame.c), así que si la mano armada siguiera el apuntado, la mano y el arma se separarían. Mismo montaje que hand_axes_run (modo 1). */
static void armed_hand_run(int armed, int aim_on, float tilt_deg, float fl_out[2][3]) {
    VrBodyConfig cfg; legacy_cfg(&cfg);
    build_body(200);
    vrbody_init(&cfg); vrbody_set_view_fn(view_fn); vrbody_set_armed(armed); vrbody_set_dominant(1);
    float th = (g_view.theta_deg + 180) * 3.14159265f / 180, cs = cosf(th), sn = sinf(th);
    float T[9] = {cs, 0, sn, 0, 1, 0, -sn, 0, cs};
    for (int f = 0; f < 40; f++) {
        for (int s = 0; s < 2; s++) {
            float Rt1[9], tt1[3], sh[3], Rs_[9]; load_xf(part_addr(1), Rt1, tt1); load_xf(part_addr(s ? 2 : 7), Rs_, sh);
            float want[3]; for (int k = 0; k < 3; k++) want[k] = sh[k] + Rt1[3 * k + 2] * 330.0f - Rt1[3 * k + 1] * 120.0f;
            float dw[3] = {want[0] - g_view.eye[0], want[1] - g_view.eye[1], want[2] - g_view.eye[2]};
            float Tt[9] = {T[0], T[3], T[6], T[1], T[4], T[7], T[2], T[5], T[8]}, pu[3]; mv(pu, Tt, dw);
            set_hand(s, pu[0] / 1000 + g_view.p_ref[0], pu[1] / 1000 + g_view.p_ref[1], pu[2] / 1000 + g_view.p_ref[2], 0.0f);
            float a2 = 35.0f * 3.14159265f / 180 * 0.5f, b2 = tilt_deg * 3.14159265f / 180 * 0.5f;
            float qg[4] = {sinf(a2), 0, 0, cosf(a2)}, qa[4] = {sinf(b2), 0, 0, cosf(b2)};      /* agarre = Rx(35); apuntado = Rx(tilt): cabeceo automático = tilt - 35 */
            memcpy(g_hands[s].rot, qg, sizeof qg);
            g_hands[s].aim_valid = aim_on; if (aim_on) memcpy(g_hands[s].aim_rot, qa, sizeof qa);
        }
        memcpy(RAM, RAM0, sizeof RAM);
        int n = vrbody_apply(RAM);
        if (f == 39 && n == 2) for (int s = 0; s < 2; s++) {
            float R[9], t[3]; load_xf(part_addr(s ? 4 : 9), R, t);
            fl_out[s][0] = -R[1]; fl_out[s][1] = -R[4]; fl_out[s][2] = -R[7];      /* dedos = -Y local de la pieza mano */
        }
        vrbody_restore(RAM);
    }
    vrbody_set_armed(0);
}
static void test_armed_hand_fixed_offset(void) {
    float a[2][3] = {{0}}, b[2][3] = {{0}}, c[2][3] = {{0}};
    armed_hand_run(1, 1, 5.0f, a);        /* armado, apuntado inclinado */
    armed_hand_run(1, 0, 0.0f, b);        /* armado, sin apuntado (cabeceo fijo) */
    armed_hand_run(0, 1, 5.0f, c);        /* desarmado, apuntado inclinado (cabeceo automático) */
    CHECK(dotv(a[1], b[1]) > 0.9999f, "mano armada (derecha): ignora el apuntado, mismo cabeceo que sin apuntado: %g", dotv(a[1], b[1]));
    CHECK(dotv(a[1], c[1]) < 0.9990f, "la derecha desarmada sí sigue el apuntado (control): %g", dotv(a[1], c[1]));
    CHECK(dotv(a[0], c[0]) > 0.9999f, "la mano NO dominante conserva el cabeceo automático aunque la otra empuñe: %g", dotv(a[0], c[0]));
    /* vrbody_hand_offset = lo que vrbody_apply aplica a la mano armada: giro de cabeceo fijo y alabeo (o la calibración) */
    VrBodyConfig cfg; legacy_cfg(&cfg); vrbody_init(&cfg);
    float q[4], qp[4], qr[4], e[4], ang = cfg.hand_pitch_deg * 3.14159265f / 180 * 0.5f;
    qp[0] = sinf(ang); qp[1] = qp[2] = 0; qp[3] = cosf(ang);
    memcpy(qr, cfg.hand_rot_offset[1], sizeof qr);
    e[0] = qp[3] * qr[0] + qp[0] * qr[3] + qp[1] * qr[2] - qp[2] * qr[1]; e[1] = qp[3] * qr[1] - qp[0] * qr[2] + qp[1] * qr[3] + qp[2] * qr[0];
    e[2] = qp[3] * qr[2] + qp[0] * qr[1] - qp[1] * qr[0] + qp[2] * qr[3]; e[3] = qp[3] * qr[3] - qp[0] * qr[0] - qp[1] * qr[1] - qp[2] * qr[2];
    CHECK(vrbody_hand_offset(1, q) == 0 && fabsf(fabsf(q[0] * e[0] + q[1] * e[1] + q[2] * e[2] + q[3] * e[3]) - 1.0f) < 1e-5f, "vrbody_hand_offset = cabeceo fijo * alabeo");
    float cal[4] = {0, 0.3826834f, 0, 0.9238795f};
    vrbody_set_calibration(1, cal);
    CHECK(vrbody_hand_offset(1, q) == 1 && fabsf(q[1] - 0.3826834f) < 1e-5f, "con calibración devuelve la calibrada");
    CHECK(!vrbody_hand_offset(2, q) && !vrbody_hand_offset(1, NULL), "lado o destino inválidos");
    vrbody_clear_calibration();
}
static void test_hand_axes(void) {
    hand_axes_run(0.0f, 0);      /* sin giro: marco de vr_pose.h tal cual */
    hand_axes_run(90.0f, 0);
    hand_axes_run(45.0f, 0);     /* defecto */
    for (int mode = 1; mode <= 5; mode++) { hand_axes_run(45.0f, mode); hand_axes_run(0.0f, mode); }
}

/* Valor por defecto (45 grados): con el marco de agarre alineado con el mundo la mano del personaje queda girada 45 grados sobre el eje de los dedos
 * (palma hacia dentro y abajo, dorso hacia fuera y arriba), no plana "sobre las piernas" ni en apretón de manos. */
static void test_default_roll(void) {
    VrBodyConfig cfg; vrbody_config_default(&cfg); cfg.hand_pitch_deg = 0;
    CHECK(fabsf(cfg.hand_roll_deg - 45.0f) < 1e-4f, "roll por defecto");
    float sn = sinf(45.0f * 3.14159265f / 180), cs = cosf(45.0f * 3.14159265f / 180);
    for (int s = 0; s < 2; s++) {
        float q[4] = {cfg.hand_rot_offset[s][0], cfg.hand_rot_offset[s][1], cfg.hand_rot_offset[s][2], cfg.hand_rot_offset[s][3]}, M[9];
        vrbody_quat_to_mat(q, M);   /* hand = grip * M: columnas de la mano en el marco del agarre */
        float dors[3] = {M[1], M[4], M[7]}, xh[3] = {M[0], M[3], M[6]};
        CHECK(fabsf(dors[0] - (s ? 1.0f : -1.0f) * sn) < 1e-4f && fabsf(dors[1] - cs) < 1e-4f, "dorso mano %d: %g %g", s, dors[0], dors[1]);
        CHECK(fabsf((s ? -xh[1] : xh[1]) - sn) < 1e-4f, "pulgar mano %d: %g", s, xh[1]);
    }
}


/* API de cabeceo/calibración (pura): relación agarre/apuntado del mando de seguimiento, calibración, persistencia y entradas inválidas */
static void qmul_(float *o, const float *a, const float *b) {
    float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
          z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}
static void qeul_(float *q, float yaw, float pitch, float roll) {   /* guiñada (Y) * cabeceo (X) * alabeo (Z), grados */
    float d = 3.14159265f / 360.0f, qy[4] = {0, sinf(yaw * d), 0, cosf(yaw * d)}, qx[4] = {sinf(pitch * d), 0, 0, cosf(pitch * d)}, qz[4] = {0, 0, sinf(roll * d), cosf(roll * d)}, t[4];
    qmul_(t, qy, qx); qmul_(q, t, qz);
}
static void test_hand_pitch_api(void) {
    float deg;
    for (int i = 0; i < 200; i++) {                      /* apuntado cualquiera; agarre = apuntado * Rx(35): el cabeceo calculado es -35 */
        float a[4], x35[4], g[4];
        qeul_(a, (float)(i * 37 % 360), (float)(i * 11 % 120 - 60), (float)(i * 7 % 40 - 20));
        qeul_(x35, 0, 35.0f, 0); qmul_(g, a, x35);
        CHECK(vrbody_pitch_from_aim(g, a, &deg) && fabsf(deg + 35.0f) < 0.05f, "cabeceo del mando: %g", deg);
        qeul_(x35, 0, -20.0f, 0); qmul_(g, a, x35);
        CHECK(vrbody_pitch_from_aim(g, a, &deg) && fabsf(deg - 20.0f) < 0.05f, "cabeceo de otro mando: %g", deg);
    }
    float id[4] = {0, 0, 0, 1}, nan4[4] = {NAN, 0, 0, 1}, back[4];
    qeul_(back, 0, 100.0f, 0);
    CHECK(!vrbody_pitch_from_aim(NULL, id, &deg) && !vrbody_pitch_from_aim(id, nan4, &deg) && !vrbody_pitch_from_aim(id, id, NULL), "entradas inválidas");
    CHECK(!vrbody_pitch_from_aim(id, back, &deg), "apuntado casi perpendicular a los dedos");
    /* calibración pura: los dedos del resultado son horizontales (±1 grado) con cualquier agarre y el dorso queda a `roll` del cenit */
    for (int i = 0; i < 300; i++) {
        float g[4], o[4], h[4], M[9];
        qeul_(g, (float)(i * 53 % 360), (float)(i * 13 % 100 - 50), (float)(i * 29 % 120 - 60));
        float roll = (i & 1) ? 45.0f : -45.0f;
        int ok = vrbody_calib_offset(g, roll, o);
        float gm[9]; vrbody_quat_to_mat(g, gm);
        float fh = sqrtf(gm[2] * gm[2] + gm[8] * gm[8]);
        CHECK(ok == (fh > 0.3f), "calibrable según el rumbo de los dedos");
        if (!ok) continue;
        qmul_(h, g, o); vrbody_quat_to_mat(h, M);
        CHECK(fabsf(M[5]) < 0.02f, "dedos horizontales: %g", M[5]);                          /* y de -Z */
        float dors = M[4];                                                                     /* componente vertical del dorso (+Y) = cos(roll) */
        CHECK(fabsf(dors - cosf(roll * 3.14159265f / 180)) < 0.02f, "dorso: %g", dors);
        float yaw_g = atan2f(-gm[2], -gm[8]), yaw_h = atan2f(-M[2], -M[8]);                   /* mismo rumbo */
        CHECK(fabsf(yaw_g - yaw_h) < 0.02f, "rumbo conservado");
    }
    { float o[4], v[4] = {0, 0, 0, 1}; float up[4]; qeul_(up, 0, -88.0f, 0);   /* dedos casi verticales hacia arriba */
      CHECK(!vrbody_calib_offset(up, 45.0f, o) && !vrbody_calib_offset(v, NAN, o) && !vrbody_calib_offset(NULL, 0, o), "calibración inválida"); }
    /* persistencia: set/get/clear, y sobrevive a vrbody_init */
    { VrBodyConfig cfg; vrbody_config_default(&cfg); vrbody_init(&cfg); float q[4]; vrbody_clear_calibration();
      CHECK(!vrbody_get_calibration(0, q) && !vrbody_get_calibration(2, q) && !vrbody_get_calibration(1, NULL), "sin calibración");
      float c[4]; qeul_(c, 0, -35.0f, 20.0f); vrbody_set_calibration(1, c);
      float bad[4] = {NAN, 0, 0, 1}, zero[4] = {0, 0, 0, 0}; vrbody_set_calibration(0, bad); vrbody_set_calibration(0, zero); vrbody_set_calibration(5, c);
      CHECK(!vrbody_get_calibration(0, q) && vrbody_get_calibration(1, q) && fabsf(q[0] - c[0]) < 1e-5f, "set/get");
      float big[4] = {3, 0, 0, 1}, small[4] = {0.1f, 0, 0, 0.1f}, edge[4] = {0, 0, 0, 1.9f}; vrbody_set_calibration(0, big); CHECK(!vrbody_get_calibration(0, q), "norma > 2 rechazada");
      vrbody_set_calibration(0, small); CHECK(!vrbody_get_calibration(0, q), "norma < 0,5 rechazada"); vrbody_set_calibration(0, edge); CHECK(vrbody_get_calibration(0, q) && fabsf(q[3] - 1.0f) < 1e-5f, "norma 1,9 aceptada y normalizada");
      vrbody_init(&cfg); CHECK(vrbody_get_calibration(1, q), "la calibración sobrevive a vrbody_init");
      CHECK(vrbody_calibrate_hands() == 0, "sin manos seguidas no se calibra nada");
      vrbody_clear_calibration(); CHECK(!vrbody_get_calibration(1, q), "borrada"); }
    { VrBodyConfig cfg; vrbody_config_default(&cfg);
      CHECK(fabsf(cfg.hand_pitch_deg + 35.0f) < 1e-4f && cfg.hand_pitch_auto == 1, "cabeceo por defecto -35 y automático");
      vrbody_init(&cfg); vrbody_set_hand_pitch(1000.0f); CHECK(vrbody_config()->hand_pitch_deg <= 89.0f, "cabeceo acotado"); vrbody_set_hand_pitch(NAN);
      vrbody_set_hand_pitch_auto(0); CHECK(vrbody_config()->hand_pitch_auto == 0, "auto apagado"); }
}

/* Golpe sintético (botón sin gesto) y agarre de enemigo: la MANO del personaje se mueve (no solo el codo) y vuelve al mando. */
static void hands_at_rest(const float T[9]) {
    for (int s = 0; s < 2; s++) {      /* a 250 mm delante y 120 bajo cada hombro, en el marco del torso */
        float Rt1[9], tt1[3], sh[3], Rs_[9]; load_xf(part_addr(1), Rt1, tt1); load_xf(part_addr(s ? 2 : 7), Rs_, sh);
        float want[3]; for (int k = 0; k < 3; k++) want[k] = sh[k] + Rt1[3 * k + 2] * 250.0f - Rt1[3 * k + 1] * 120.0f;
        float dw[3] = {want[0] - g_view.eye[0], want[1] - g_view.eye[1], want[2] - g_view.eye[2]};
        float Tt[9] = {T[0], T[3], T[6], T[1], T[4], T[7], T[2], T[5], T[8]}, pu[3]; mv(pu, Tt, dw);
        set_hand(s, pu[0] / 1000 + g_view.p_ref[0], pu[1] / 1000 + g_view.p_ref[1], pu[2] / 1000 + g_view.p_ref[2], 0.0f);
    }
}
static void wrist_pos(int s, float w[3]) { float R[9]; load_xf(part_addr(s ? 4 : 9), R, w); }
static void test_jab_and_grab(void) {
    VrBodyConfig cfg; legacy_cfg(&cfg); cfg.hand_pitch_deg = 0;
    for (int seed = 0; seed < 12; seed++) {
        build_body(300 + seed);
        vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
        float th = (g_view.theta_deg + 180) * 3.14159265f / 180, cs = cosf(th), sn = sinf(th);
        float T[9] = {cs, 0, sn, 0, 1, 0, -sn, 0, cs};
        float Rt1[9], tt1[3]; load_xf(part_addr(1), Rt1, tt1);
        float fw[3] = {Rt1[2], 0, Rt1[8]}; float fl = lenv(fw); fw[0] /= fl; fw[2] /= fl;
        float lf[3] = {fw[2], 0, -fw[0]};                      /* izquierda del personaje mirando hacia fw */
        float rest[2][3];
        for (int f = 0; f < 40; f++) { hands_at_rest(T); memcpy(RAM, RAM0, sizeof RAM); vrbody_apply(RAM); if (f == 39) { wrist_pos(0, rest[0]); wrist_pos(1, rest[1]); } vrbody_restore(RAM); }
        /* a) golpe sintético de la mano derecha del personaje: avanza y vuelve; la otra mano no se mueve */
        vrbody_set_jab(1, fw);
        float maxfwd = 0, maxl = 0, endd = 0;
        for (int f = 0; f < 40; f++) {
            hands_at_rest(T); memcpy(RAM, RAM0, sizeof RAM); vrbody_apply(RAM);
            float w1[3], w0[3]; wrist_pos(1, w1); wrist_pos(0, w0);
            float d[3] = {w1[0] - rest[1][0], w1[1] - rest[1][1], w1[2] - rest[1][2]};
            float fwd = d[0] * fw[0] + d[2] * fw[2]; if (fwd > maxfwd) maxfwd = fwd;
            float dl[3] = {w0[0] - rest[0][0], w0[1] - rest[0][1], w0[2] - rest[0][2]}; if (lenv(dl) > maxl) maxl = lenv(dl);
            if (f == 39) endd = lenv(d);
            vrbody_restore(RAM);
        }
        CHECK(maxfwd > 150.0f, "el golpe sintético no mueve la mano: %g mm (semilla %d)", maxfwd, seed);
        CHECK(maxl < 3.0f, "la otra mano se movió %g mm", maxl);
        CHECK(endd < 3.0f, "la mano no vuelve tras el golpe: %g mm", endd);
        VrBodyStats st; vrbody_stats(&st); CHECK(st.jab_applies > 5, "jab_applies %u", st.jab_applies);
        /* b) agarre: las dos manos van al cuello (a ambos lados) y vuelven al soltar */
        float Sh0[3], Sh1[3], Rq[9]; load_xf(part_addr(7), Rq, Sh0); load_xf(part_addr(2), Rq, Sh1);
        VrBodyGrab gr; memset(&gr, 0, sizeof gr); gr.active = 1;
        for (int k = 0; k < 3; k++) { gr.neck[k] = 0.5f * (Sh0[k] + Sh1[k]) + fw[k] * 450.0f; gr.left[k] = lf[k]; }
        gr.neck[1] += 0;
        float dmin[2] = {1e9f, 1e9f};
        for (int f = 0; f < 40; f++) {
            hands_at_rest(T); vrbody_set_grab(&gr); memcpy(RAM, RAM0, sizeof RAM); vrbody_apply(RAM);
            if (f == 39)
                for (int s = 0; s < 2; s++) {
                    float w[3]; wrist_pos(s, w);
                    float C[3]; for (int k = 0; k < 3; k++) C[k] = gr.neck[k] + (s == 0 ? 1.0f : -1.0f) * lf[k] * cfg.grab_spread_mm;
                    float d[3] = {w[0] - C[0], w[1] - C[1], w[2] - C[2]}; dmin[s] = lenv(d);
                }
            vrbody_restore(RAM);
        }
        for (int s = 0; s < 2; s++) CHECK(dmin[s] < 140.0f, "la mano %d no llega al cuello: %g mm (semilla %d)", s, dmin[s], seed);
        vrbody_stats(&st); CHECK(st.grab_applies > 20, "grab_applies %u", st.grab_applies);
        /* soltar (sin refrescar): vuelven al mando */
        for (int f = 0; f < 60; f++) { hands_at_rest(T); memcpy(RAM, RAM0, sizeof RAM); vrbody_apply(RAM); vrbody_restore(RAM); }
        hands_at_rest(T); memcpy(RAM, RAM0, sizeof RAM); vrbody_apply(RAM);
        for (int s = 0; s < 2; s++) { float w[3]; wrist_pos(s, w); float d[3] = {w[0] - rest[s][0], w[1] - rest[s][1], w[2] - rest[s][2]}; CHECK(lenv(d) < 4.0f, "la mano %d no vuelve al mando: %g mm", s, lenv(d)); }
        vrbody_restore(RAM);
    }
}

/* Brazos siempre dibujados: si el motor dejó a 0 el contador de visibilidad (+0x4C) de piezas del brazo, la IK lo fuerza durante la
 * transformación y restore lo devuelve; la cabeza (parte 6, oculta a propósito) y el resto no se tocan. */
static void test_visibility(void) {
    for (int force = 0; force < 2; force++) {
        VrBodyConfig cfg; legacy_cfg(&cfg); cfg.hand_pitch_deg = 0; cfg.force_arm_visible = force;
        vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
        build_body(77);
        static const int ARMS[6] = {2, 3, 4, 7, 8, 9};
        for (int f = 0; f < 30; f++) {
            set_hand(0, -0.25f, 1.3f, -0.4f, 0.3f); set_hand(1, 0.30f, 1.25f, -0.45f, -0.2f);
            memcpy(RAM, RAM0, sizeof RAM);
            for (int k = 0; k < 6; k++) w16(part_addr(ARMS[k]) + 0x4C, 0);
            w16(part_addr(6) + 0x4C, 0);
            int n = vrbody_apply(RAM);
            if (n == 2 && f > 20) {
                for (int k = 0; k < 6; k++) CHECK((r16(part_addr(ARMS[k]) + 0x4C) != 0) == (force != 0), "visibilidad forzada de la pieza %d con force=%d: %d", ARMS[k], force, r16(part_addr(ARMS[k]) + 0x4C));
                CHECK(r16(part_addr(6) + 0x4C) == 0, "la cabeza no debe tocarse");
                CHECK(r16(part_addr(10) + 0x4C) == 2, "las piernas no deben tocarse");
            }
            vrbody_restore(RAM);
            for (int k = 0; k < 6; k++) CHECK(r16(part_addr(ARMS[k]) + 0x4C) == 0, "restore devuelve la visibilidad (pieza %d)", ARMS[k]);
        }
    }
}

/* Espejo: el brazo derecho resuelto con la escena reflejada en X del torso da el espejo del izquierdo. */
static void test_mirror(void) {
    VrArmLimits lim; vrbody_limits_default(&lim);
    const float Mx[9] = {-1, 0, 0, 0, 1, 0, 0, 0, 1};
    int tested = 0, bad = 0;
    for (int it = 0; it < 4000; it++) {
        VrArmRig L = {1.0f, {ELB[0], ELB[1], ELB[2]}, {WRI[0], WRI[1], WRI[2]}}, Rr = L; Rr.side = -1.0f;
        VrArmIn a; memset(&a, 0, sizeof a);
        a.torso[0] = a.torso[4] = a.torso[8] = 1;
        a.shoulder[0] = 190; a.shoulder[1] = 260; a.shoulder[2] = -29;
        float dir[3] = {frand(-1, 1), frand(-1, 1), frand(-0.3f, 1)}, dl = lenv(dir), r = frand(100, 560);
        if (dl < 0.2f) continue;
        for (int i = 0; i < 3; i++) a.wrist[i] = a.shoulder[i] + dir[i] / dl * r;
        rand_rot(a.hand, 2.0f);
        a.pole[0] = 0.45f; a.pole[1] = -1; a.pole[2] = -0.35f;
        VrArmIn b = a;                                  /* reflejo en x (plano del torso) */
        b.shoulder[0] = -a.shoulder[0]; b.wrist[0] = -a.wrist[0]; b.pole[0] = -a.pole[0];
        float t1[9]; mm(t1, Mx, a.hand); mm(b.hand, t1, Mx);
        VrArmOut oa, ob; if (vrbody_solve_arm(&L, &lim, &a, &oa) || vrbody_solve_arm(&Rr, &lim, &b, &ob)) continue;
        tested++;
        float e = fabsf(oa.elbow[0] + ob.elbow[0]) + fabsf(oa.elbow[1] - ob.elbow[1]) + fabsf(oa.elbow[2] - ob.elbow[2]);
        e += fabsf(oa.wrist[0] + ob.wrist[0]) + fabsf(oa.wrist[1] - ob.wrist[1]) + fabsf(oa.wrist[2] - ob.wrist[2]);
        float Rm[9], u[9]; mm(u, Mx, oa.R_hand); mm(Rm, u, Mx);
        float eh = 0; for (int i = 0; i < 9; i++) eh = fmaxf(eh, fabsf(Rm[i] - ob.R_hand[i]));
        if (e > 0.05f || eh > 1e-3f || oa.flags != ob.flags) bad++;
    }
    CHECK(tested > 3000 && bad * 1000 <= tested, "simetría izquierda/derecha: %d de %d no son espejo", bad, tested);   /* el giro del codo (búsqueda en rejilla) puede deshacer un empate distinto en casos sueltos */
}

/* Cuerpo completo: los 16 modelos pasan a doble cara (bit 0x400 de model[0]) durante el dibujado y restore devuelve sus banderas. */
static void test_two_sided(void) {
    VrBodyConfig cfg; legacy_cfg(&cfg); cfg.hand_pitch_deg = 0;
    build_body(401); vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
    for (int i = 0; i < 16; i++) w32(PMODELS + 0x3C * (uint32_t)i, (i & 1) ? 0x1 : 0x10001);
    memcpy(RAM0, RAM, sizeof RAM);
    set_hand(0, -0.25f, 1.3f, -0.4f, 0.3f); set_hand(1, 0.30f, 1.25f, -0.45f, -0.2f);
    for (int f = 0; f < 5; f++) {
        memcpy(RAM, RAM0, sizeof RAM);
        vrbody_apply(RAM);
        for (int i = 0; i < 16; i++) CHECK((r32(PMODELS + 0x3C * (uint32_t)i) & 0x400) != 0 && (r32(PMODELS + 0x3C * (uint32_t)i) & ~0x400) == ((i & 1) ? 0x1 : 0x10001), "modelo %d sin doble cara tras apply: %x", i, r32(PMODELS + 0x3C * (uint32_t)i));
        vrbody_restore(RAM);
        for (int i = 0; i < 16; i++) CHECK(r32(PMODELS + 0x3C * (uint32_t)i) == ((i & 1) ? 0x1 : 0x10001), "modelo %d no restaurado: %x", i, r32(PMODELS + 0x3C * (uint32_t)i));
        CHECK(memcmp(RAM, RAM0, sizeof RAM) == 0, "doble cara: restore no devuelve la RAM");
    }
    { VrBodyStats st; vrbody_stats(&st); CHECK(st.two_sided > 0, "stats: modelos de doble cara no contados"); }
    /* sin manos no se toca nada */
    memset(g_hands, 0, sizeof g_hands); vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
    memcpy(RAM, RAM0, sizeof RAM); vrbody_apply(RAM); CHECK(memcmp(RAM, RAM0, sizeof RAM) == 0, "doble cara sin manos: RAM tocada");
    /* el juego ya tenía el bit: se conserva tras restore; dos piezas con el mismo modelo no corrompen la restauración */
    set_hand(0, -0.25f, 1.3f, -0.4f, 0.3f); set_hand(1, 0.30f, 1.25f, -0.45f, -0.2f);
    w32(PMODELS, 0x10401); w32(part_addr(12) + 0x40, (int32_t)(PMODELS + 0x3C * 11)); memcpy(RAM0, RAM, sizeof RAM);
    vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
    for (int f = 0; f < 3; f++) { memcpy(RAM, RAM0, sizeof RAM); vrbody_apply(RAM); vrbody_restore(RAM); CHECK(memcmp(RAM, RAM0, sizeof RAM) == 0, "doble cara: bit previo / modelo compartido (f=%d)", f); }
    /* si el juego cambió la bandera entre apply y restore, no se pisa */
    memcpy(RAM, RAM0, sizeof RAM); vrbody_apply(RAM); w32(PMODELS + 0x3C * 3, 0x777); vrbody_restore(RAM);
    CHECK(r32(PMODELS + 0x3C * 3) == 0x777, "restore pisó una bandera cambiada por el juego");
}

/* own_hands: las piezas de los brazos movidas se ocultan (+0x4C = 0) y restore las devuelve; sin la bandera no se ocultan. */
static void test_own_hands(void) {
    static const int ARMS[6] = {2, 3, 4, 7, 8, 9};
    for (int own = 0; own < 2; own++) {
        VrBodyConfig cfg; legacy_cfg(&cfg); cfg.hand_pitch_deg = 0; cfg.own_hands = own;
        build_body(402); vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
        for (int f = 0; f < 30; f++) {
            set_hand(0, -0.25f, 1.3f, -0.4f, 0.3f); set_hand(1, 0.30f, 1.25f, -0.45f, -0.2f);
            memcpy(RAM, RAM0, sizeof RAM);
            int n = vrbody_apply(RAM);
            if (n == 2) {
                for (int k = 0; k < 6; k++) CHECK(r16(part_addr(ARMS[k]) + 0x4C) == (own ? 0 : 2), "own_hands=%d: visibilidad de la pieza %d = %d", own, ARMS[k], r16(part_addr(ARMS[k]) + 0x4C));
                for (int i = 0; i < 16; i++) if (!is_arm_part(i)) CHECK(r16(part_addr(i) + 0x4C) == 2, "own_hands: pieza %d tocada", i);
            }
            vrbody_restore(RAM);
            for (int k = 0; k < 6; k++) CHECK(r16(part_addr(ARMS[k]) + 0x4C) == 2, "restore devuelve la visibilidad (pieza %d, own=%d)", ARMS[k], own);
            CHECK(memcmp(RAM, RAM0, sizeof RAM) == 0, "own_hands: restore no devuelve la RAM");
        }
        { VrBodyStats st; vrbody_stats(&st); CHECK(own ? st.hidden_pieces > 0 : st.hidden_pieces == 0, "stats: piezas ocultas=%u con own_hands=%d", st.hidden_pieces, own); }
    }
}

/* own_hands: solo se ocultan las tres piezas del lado que la IK movió; el lado sin seguimiento conserva la animación (y la visibilidad) del juego; restore las devuelve; sin manos no se toca nada. */
static void test_own_hands_both(void) {
    static const int ARMS[6] = {2, 3, 4, 7, 8, 9};
    for (int only = 0; only < 3; only++) {      /* 0: solo la izquierda, 1: solo la derecha, 2: ninguna */
        VrBodyConfig cfg; legacy_cfg(&cfg); cfg.hand_pitch_deg = 0; cfg.own_hands = 1;
        build_body(410 + only); vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
        for (int f = 0; f < 20; f++) {
            memset(g_hands, 0, sizeof g_hands);
            if (only == 0) set_hand(0, -0.25f, 1.3f, -0.4f, 0.3f);
            if (only == 1) set_hand(1, 0.30f, 1.25f, -0.45f, -0.2f);
            memcpy(RAM, RAM0, sizeof RAM);
            int n = vrbody_apply(RAM);
            if (only < 2 && f > 10) CHECK(n == 1, "own_hands: un solo brazo movido (n=%d)", n);
            if (n == 1) {      /* un lado movido: sus 3 piezas ocultas, las 3 del otro lado intactas */
                int hid_a = 0, hid_b = 0;
                for (int k = 0; k < 3; k++) { hid_a += r16(part_addr(ARMS[k]) + 0x4C) == 0; hid_b += r16(part_addr(ARMS[k + 3]) + 0x4C) == 0; }
                CHECK((hid_a == 3 && hid_b == 0) || (hid_a == 0 && hid_b == 3), "own_hands (solo=%d): solo el lado movido se oculta (a=%d b=%d)", only, hid_a, hid_b);
            }
            if (only == 2) for (int k = 0; k < 6; k++) CHECK(r16(part_addr(ARMS[k]) + 0x4C) == 2, "own_hands (sin manos): pieza %d oculta", ARMS[k]);
            for (int i = 0; i < 16; i++) if (!is_arm_part(i)) CHECK(r16(part_addr(i) + 0x4C) == 2, "own_hands: pieza %d tocada", i);
            CHECK(only == 2 ? !vrbody_ram_dirty() : vrbody_ram_dirty(), "own_hands: ram_dirty tras apply (solo=%d)", only);
            vrbody_restore(RAM);
            CHECK(!vrbody_ram_dirty() && memcmp(RAM, RAM0, sizeof RAM) == 0, "own_hands: restore devuelve la RAM (solo=%d)", only);
        }
    }
    /* vrbody_set_own_hands en caliente */
    VrBodyConfig cfg; legacy_cfg(&cfg); cfg.hand_pitch_deg = 0;
    build_body(413); vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
    set_hand(0, -0.25f, 1.3f, -0.4f, 0.3f);
    vrbody_apply(RAM); CHECK(r16(part_addr(2) + 0x4C) == 2, "sin own_hands no se oculta"); vrbody_restore(RAM);
    vrbody_set_own_hands(1);
    vrbody_apply(RAM); CHECK((r16(part_addr(2) + 0x4C) == 0) != (r16(part_addr(9) + 0x4C) == 0), "vrbody_set_own_hands(1) oculta solo el lado movido"); vrbody_restore(RAM);
    vrbody_set_own_hands(0);
    vrbody_apply(RAM); CHECK(r16(part_addr(2) + 0x4C) == 2, "vrbody_set_own_hands(0)"); vrbody_restore(RAM);
}

/* vrbody_player_models y vrbody_arm_live / vrbody_arm_resolve: la IK con el agarre simulado reproduce el apply; con el agarre desplazado mueve muñeca y mano rígidamente. */
static void test_arm_live(void) {
    uint32_t md[2][3];
    build_body(420);
    CHECK(vrbody_player_models(RAM, md) == 1, "player_models");
    static const int PI_[2][3] = {{7, 8, 9}, {2, 3, 4}};
    for (int s = 0; s < 2; s++) for (int k = 0; k < 3; k++) CHECK(md[s][k] == PMODELS + 0x3C * (uint32_t)PI_[s][k], "modelo lado %d pieza %d: %x", s, k, md[s][k]);
    int16_t keep = r16(PARTSET + 0x2E); w16(PARTSET + 0x2E, 3); CHECK(vrbody_player_models(RAM, md) == 0, "player_models con pocas piezas"); w16(PARTSET + 0x2E, keep);
    int tested = 0, shifted = 0, rotated = 0;
    for (int seed = 0; seed < 20; seed++) {
        VrBodyConfig cfg; vrbody_config_default(&cfg); cfg.hand_pitch_deg = 0;
        build_body(430 + seed); vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
        for (int f = 0; f < 40; f++) {
            set_hand(0, -0.25f + 0.002f * f, 1.3f, -0.4f, 0.3f); set_hand(1, 0.30f, 1.25f, -0.45f - 0.001f * f, -0.2f);
            memcpy(RAM, RAM0, sizeof RAM); vrbody_apply(RAM);
            if (f < 39) vrbody_restore(RAM);
        }
        for (int s = 0; s < 2; s++) {
            VrBodyArmLive a; VrBodyForearm fa; VrBodyArmPose o;
            CHECK(vrbody_arm_live(s, &a) == 1 && vrbody_forearm_pose(s, &fa) == 1, "arm_live disponible (lado %d)", s);
            CHECK(a.fade > 0.99f && a.upm == 1000.0f && a.rig.side == (s == 0 ? 1.0f : -1.0f), "arm_live: fundido/escala/lado");
            CHECK(vrbody_arm_resolve(&a, a.grip_p, a.grip_q, &o) == 0, "resolve con el agarre simulado");
            float de = 0, dw = 0;
            for (int j = 0; j < 3; j++) { de = fmaxf(de, fabsf(o.elbow[j] - fa.elbow[j])); dw = fmaxf(dw, fabsf(o.wrist[j] - fa.wrist[j])); }
            CHECK(de < 1e-3f && dw < 1e-3f, "resolve (agarre simulado) = apply: codo %g m, muñeca %g m (lado %d)", de, dw, s);
            tested++;
            if ((o.flags & ~(unsigned)VRARM_WRIST_LIMIT) == 0) {      /* el límite de la muñeca no mueve la muñeca */
                float lp[3] = {a.grip_p[0] + 0.01f, a.grip_p[1] - 0.005f, a.grip_p[2] + 0.008f};
                VrBodyArmPose o2;
                CHECK(vrbody_arm_resolve(&a, lp, a.grip_q, &o2) == 0, "resolve desplazado");
                if ((o2.flags & ~(unsigned)VRARM_WRIST_LIMIT) == 0 && o2.err_mm < 0.5f) {
                    float dx[3] = {0.01f, -0.005f, 0.008f}; float e = 0;
                    for (int j = 0; j < 3; j++) e = fmaxf(e, fabsf((o2.wrist[j] - o.wrist[j]) - dx[j]));
                    CHECK(e < 2e-3f, "la muñeca sigue al agarre vivo: error %g m", e);
                    CHECK(fabsf(o2.shoulder[0] - o.shoulder[0]) + fabsf(o2.shoulder[1] - o.shoulder[1]) + fabsf(o2.shoulder[2] - o.shoulder[2]) < 1e-6f, "el hombro no se mueve");
                    shifted++;
                }
                /* giro de 8 grados del agarre vivo en torno al propio agarre: la muñeca gira rígidamente alrededor del punto del agarre */
                float h = 8.0f * 0.0174532925f * 0.5f, qy[4] = {0, sinf(h), 0, cosf(h)}, ql[4], ct[3] = {0, 0, 0};
                (void)ct;
                { const float *b = a.grip_q;     /* ql = qy * grip_q */
                  ql[0] = qy[3] * b[0] + qy[0] * b[3] + qy[1] * b[2] - qy[2] * b[1]; ql[1] = qy[3] * b[1] - qy[0] * b[2] + qy[1] * b[3] + qy[2] * b[0];
                  ql[2] = qy[3] * b[2] + qy[0] * b[1] - qy[1] * b[0] + qy[2] * b[3]; ql[3] = qy[3] * b[3] - qy[0] * b[0] - qy[1] * b[1] - qy[2] * b[2]; }
                VrBodyArmPose o3;
                CHECK(vrbody_arm_resolve(&a, a.grip_p, ql, &o3) == 0, "resolve girado");
                if ((o3.flags & ~(unsigned)VRARM_WRIST_LIMIT) == 0 && o3.err_mm < 0.5f) {
                    float c = cosf(8.0f * 0.0174532925f), sn = sinf(8.0f * 0.0174532925f);
                    float d[3] = {a.wrist[0] - a.grip_p[0], a.wrist[1] - a.grip_p[1], a.wrist[2] - a.grip_p[2]};
                    float want[3] = {a.grip_p[0] + c * d[0] + sn * d[2], a.grip_p[1] + d[1], a.grip_p[2] - sn * d[0] + c * d[2]};
                    float e = 0; for (int j = 0; j < 3; j++) e = fmaxf(e, fabsf(o3.wrist[j] - want[j]));
                    CHECK(e < 2e-3f, "la muñeca gira con el agarre vivo: error %g m", e);
                    rotated++;
                }
            }
        }
        vrbody_restore(RAM);
    }
    CHECK(tested == 40 && shifted > 10 && rotated > 10, "arm_live: cobertura (%d, desplazados %d, girados %d)", tested, shifted, rotated);
    float nan3[3] = {NAN, 0, 0}, q[4] = {0, 0, 0, 1}; VrBodyArmLive a0; VrBodyArmPose o0; memset(&a0, 0, sizeof a0);
    CHECK(vrbody_arm_resolve(&a0, nan3, q, &o0) == -1 && vrbody_arm_resolve(NULL, q, q, &o0) == -1, "resolve: entrada no válida");
    CHECK(vrbody_arm_live(2, &a0) == 0 && vrbody_arm_live(0, NULL) == 0, "arm_live: lado/puntero inválidos");
}

/* torso_follow: el torso hereda la pelvis del reposo (rumbo distinto de la mirada); con la mirada real como guiñada del torso los hombros quedan
 * donde las manos naturales los esperan. */
static float yaw_of(uint32_t part) { float R[9], t[3]; load_xf(part_addr((int)part), R, t); return atan2f(R[2], R[8]) * 57.2957795f; }
static float wrap180t(float a) { while (a > 180) a -= 360; while (a < -180) a += 360; return a; }
static struct { float pel_err, leg_rel_err, leg_dist_err; } TR;   /* pelvis: error de guiñada; piernas: giro relativo y distancia a la pelvis respecto al juego */
static void torso_run(int seed, int follow, float off_deg, float *yaw_err, float *yawY, float *dP5, float reach[2], float err[2], int *same_after) {
    VrBodyConfig cfg; vrbody_config_default(&cfg); cfg.hand_pitch_deg = 0; cfg.torso_follow = follow; cfg.torso_hand_share = 0;
    build_body(seed); vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
    const float P0 = yaw_of(0);
    float gaze = wrap180t(P0 + off_deg);
    g_view.gaze_yaw_deg = gaze;
    /* esperado en reposo: la pelvis solo sale de la zona muerta; el pecho se tuerce una fracción del desvío hacia la mirada */
    float expP = P0;
    if (fabsf(off_deg) > cfg.torso_deadzone_deg) expP = wrap180t(gaze - (off_deg > 0 ? 1.0f : -1.0f) * cfg.torso_deadzone_deg);
    float tw_ = cfg.torso_chest_share * wrap180t(gaze - expP);
    tw_ = tw_ > cfg.torso_chest_max_deg ? cfg.torso_chest_max_deg : (tw_ < -cfg.torso_chest_max_deg ? -cfg.torso_chest_max_deg : tw_);
    const float expC = wrap180t(expP + tw_);
    const float leg_rel0 = wrap180t(yaw_of(10) - P0);
    float lt0[3], l0[3], R_[9]; load_xf(part_addr(10), R_, lt0); load_xf(part_addr(0), R_, l0);
    const float leg_d0 = sqrtf((lt0[0] - l0[0]) * (lt0[0] - l0[0]) + (lt0[1] - l0[1]) * (lt0[1] - l0[1]) + (lt0[2] - l0[2]) * (lt0[2] - l0[2]));
    float gr = (follow ? expC : gaze) * 3.14159265f / 180, c = cosf(gr), s = sinf(gr);
    float Rn[9] = {c, 0, s, 0, 1, 0, -s, 0, c};
    float R5[9], P5[3]; load_xf(part_addr(5), R5, P5);
    float n5[3] = {0, 305, -55}, rn5[3]; mv(rn5, Rn, n5);
    float t1[3] = {P5[0] - rn5[0], P5[1] - rn5[1], P5[2] - rn5[2]};
    float th = (g_view.theta_deg + 180) * 3.14159265f / 180, cs = cosf(th), sn = sinf(th);
    float T[9] = {cs, 0, sn, 0, 1, 0, -sn, 0, cs}, Tt[9] = {cs, 0, -sn, 0, 1, 0, sn, 0, cs};
    for (int f = 0; f < 160; f++) {
        for (int sd = 0; sd < 2; sd++) {     /* muñeca a 480 mm delante y 100 bajo el hombro del torso corregido (alcance 94 %) */
            float so[3], sh[3], want[3], dw[3], pu[3];
            so[0] = TOFF[sd ? 2 : 7][0]; so[1] = TOFF[sd ? 2 : 7][1]; so[2] = TOFF[sd ? 2 : 7][2];
            mv(sh, Rn, so);
            for (int k = 0; k < 3; k++) { sh[k] += t1[k]; want[k] = sh[k] + Rn[3 * k + 2] * 480.0f - Rn[3 * k + 1] * 100.0f; }
            {   /* centro de agarre: muñeca + R_pieza * vrhs_grip_center * k (sin cabeceo; alabeo de 45 grados, izquierda +, derecha -) */
                float a = (sd ? -45.0f : 45.0f) * 3.14159265f / 180, Rz[9] = {cosf(a), -sinf(a), 0, sinf(a), cosf(a), 0, 0, 0, 1}, Rw[9];
                mm(Rw, T, Rz);
                const float Mr[9] = {0, 0, -1, -1, 0, 0, 0, 1, 0}, Ml[9] = {0, 0, 1, 1, 0, 0, 0, 1, 0};
                float gc[3], gh[3], go[3]; vrhs_grip_center(sd ? 1 : -1, gc);
                        mv(gh, sd ? Mr : Ml, gc); mv(go, Rw, gh);
                for (int k = 0; k < 3; k++) want[k] += go[k];
            }
            for (int k = 0; k < 3; k++) dw[k] = want[k] - g_view.eye[k];
            mv(pu, Tt, dw);
            set_hand(sd, pu[0] / 1000 + g_view.p_ref[0], pu[1] / 1000 + g_view.p_ref[1], pu[2] / 1000 + g_view.p_ref[2], 0);
        }
        memcpy(RAM, RAM0, sizeof RAM);
        vrbody_apply(RAM);
        if (f == 159) {
            float Rt[9], tt[3], Rp[9], Pp[3]; load_xf(part_addr(1), Rt, tt); load_xf(part_addr(5), Rp, Pp);
            *yaw_err = wrap180t(yaw_of(1) - (follow ? expC : gaze));
            { float lt[3], l[3], R2_[9]; load_xf(part_addr(10), R2_, lt); load_xf(part_addr(0), R2_, l);
              TR.pel_err = wrap180t(yaw_of(0) - expP); TR.leg_rel_err = wrap180t(yaw_of(10) - yaw_of(0) - leg_rel0);
              TR.leg_dist_err = sqrtf((lt[0] - l[0]) * (lt[0] - l[0]) + (lt[1] - l[1]) * (lt[1] - l[1]) + (lt[2] - l[2]) * (lt[2] - l[2])) - leg_d0; } *yawY = Rt[4];
            *dP5 = sqrtf((Pp[0] - P5[0]) * (Pp[0] - P5[0]) + (Pp[1] - P5[1]) * (Pp[1] - P5[1]) + (Pp[2] - P5[2]) * (Pp[2] - P5[2]));
            for (int sd = 0; sd < 2; sd++) { reach[sd] = vrbody_debug(sd)->sol.reach; err[sd] = vrbody_debug(sd)->sol.err_mm + (vrbody_debug(sd)->sol.stretch - 1.0f) * 517.6f; }   /* lo que falta de alcance: hoy se compensa estirando el brazo */
        }
        vrbody_restore(RAM);
    }
    *same_after = memcmp(RAM, RAM0, sizeof RAM) == 0;
}
static void test_torso_follow(void) {
    int bad_off = 0;
    for (int seed = 0; seed < 8; seed++) {
        float ye, yy, dp, re[2], er[2]; int same;
        torso_run(500 + seed, 0, 50.0f, &ye, &yy, &dp, re, er, &same);
        CHECK(er[1] > 80.0f, "torso_follow=0: el brazo derecho debería fallar (semilla %d): %g mm", seed, er[1]);
        CHECK(same, "torso_follow=0: restore");
        CHECK(fabsf(ye) > 20.0f, "torso_follow=0 no debe girar el torso: %g", ye);
        torso_run(500 + seed, 1, 50.0f, &ye, &yy, &dp, re, er, &same);
        CHECK(fabsf(ye) <= 1.0f, "guiñada del pecho esperada (pelvis en el borde de la zona muerta + torsión): error %g grados (semilla %d)", ye, seed);
        CHECK(fabsf(TR.pel_err) <= 1.0f, "guiñada de la pelvis: error %g grados (semilla %d)", TR.pel_err, seed);
        CHECK(fabsf(TR.leg_rel_err) <= 1.0f && fabsf(TR.leg_dist_err) <= 1.5f, "las piernas no giran rígidas con la pelvis: giro %g, distancia %g mm (semilla %d)", TR.leg_rel_err, TR.leg_dist_err, seed);
        CHECK(yy > 0.99f, "torso sin inclinación: Y.y = %g", yy);
        CHECK(dp < 2.0f, "el cuello se movió %g mm", dp);
        CHECK(fabsf(re[1] - re[0]) < 0.1f, "alcance asimétrico: %g vs %g", re[0], re[1]);
        CHECK(er[0] < 20.0f && er[1] < 20.0f, "error de brazo con torso_follow=1: %g / %g mm (semilla %d)", er[0], er[1], seed);
        CHECK(same, "torso_follow=1: restore devuelve la RAM (incluida la pieza 1)");
        (void)bad_off;
    }
    /* zona muerta de la pelvis: dentro de ella (10, 20, -30 grados) la pelvis y las piernas no se mueven y solo el pecho se tuerce; fuera sigue a la mirada con la zona de margen */
    { float offs[] = {10.0f, 20.0f, -30.0f, 70.0f, -90.0f};
      for (unsigned k = 0; k < sizeof offs / sizeof *offs; k++) {
        float ye, yy, dp, re[2], er[2]; int same;
        torso_run(520 + (int)k, 1, offs[k], &ye, &yy, &dp, re, er, &same);
        CHECK(fabsf(ye) <= 1.0f && same, "gaze %+g: pecho error %g", offs[k], ye);
        CHECK(fabsf(TR.pel_err) <= 1.0f, "gaze %+g: pelvis error %g", offs[k], TR.pel_err);
        CHECK(fabsf(TR.leg_rel_err) <= 1.0f && fabsf(TR.leg_dist_err) <= 1.5f, "gaze %+g: piernas no rígidas con la pelvis (%g, %g)", offs[k], TR.leg_rel_err, TR.leg_dist_err);
      } }
    /* inclinado más de 35 grados (agachado/tumbado): no se toca */
    { VrBodyConfig cfg; vrbody_config_default(&cfg); cfg.hand_pitch_deg = 0;
      build_body(530); vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
      float R[9], t[3]; load_xf(part_addr(1), R, t);
      float Rx[9] = {1, 0, 0, 0, cosf(1.05f), -sinf(1.05f), 0, sinf(1.05f), cosf(1.05f)}, Rn[9]; mm(Rn, R, Rx);
      float Re[9]; for (int i = 0; i < 9; i++) Re[i] = r16(BA_VIEW_CHANNEL + BCH_CAM_INV + 2 * i) / 4096.0f;
      float Rs[9], ts[3]; mm(Rs, Re, Rn); mv(ts, Re, t); for (int k = 0; k < 3; k++) ts[k] += (float)r32(BA_VIEW_CHANNEL + BCH_CAM_INV + 0x14 + 4 * k);
      store_xf(part_addr(1), Rn, t); store_xf(part_addr(1) + 0x20, Rs, ts); memcpy(RAM0, RAM, sizeof RAM);
      g_view.gaze_yaw_deg = wrap180t(yaw_of(1) + 90.0f);
      set_hand(0, -0.25f, 1.3f, -0.4f, 0); set_hand(1, 0.30f, 1.25f, -0.45f, 0);
      VrBodyStats a, b; vrbody_stats(&a);
      for (int f = 0; f < 5; f++) { memcpy(RAM, RAM0, sizeof RAM); vrbody_apply(RAM); CHECK(memcmp(RAM + (part_addr(1) & 0x1FFFFF), RAM0 + (part_addr(1) & 0x1FFFFF), 0x40) == 0, "torso inclinado 60 grados: no debe tocarse"); vrbody_restore(RAM); }
      vrbody_stats(&b); CHECK(b.torso_applies == a.torso_applies, "torso inclinado: torso_applies"); }
}


/* Cuerpo anclado a la cabeza del usuario (VrBodyView.anchor / head_at): la articulación de la cabeza del modelo sube, baja y se ladea a cada paso (animación de andar); el cuerpo
 * entero se dibuja desplazado para que esa articulación quede SIEMPRE en head_at (bajo el ojo): el torso no hereda el balanceo. restore devuelve la RAM. */
static void test_head_anchor(void) {
    VrBodyConfig cfg; legacy_cfg(&cfg); build_body(902); vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
    float R6[9], t6[3]; load_xf(part_addr(6), R6, t6);
    const float at[3] = {t6[0] + 35.0f, t6[1] - 20.0f, t6[2] - 410.0f};
    float worst = 0, rigid = 0, bob_seen = 0;
    for (int k = 0; k < 12; k++) {
        float bob[3] = {30.0f * sinf(0.5f * k + 1.0f), 40.0f * sinf(0.5f * k), 30.0f * sinf(0.5f * k + 2.0f)}, t0[16][3], R[9];
        for (int i = 1; i < 10; i++) { float t[3]; load_xf(part_addr(i), R, t); for (int j = 0; j < 3; j++) t[j] += bob[j]; store_xf(part_addr(i), R, t); }   /* torso, brazos, cuello y cabeza con el balanceo */
        memcpy(RAM0, RAM, sizeof RAM);
        for (int i = 0; i < 16; i++) load_xf(part_addr(i), R, t0[i]);
        g_view.anchor = 1; for (int j = 0; j < 3; j++) g_view.head_at[j] = at[j];
        vrbody_apply(RAM);
        float t1[3]; load_xf(part_addr(6), R, t1);
        for (int j = 0; j < 3; j++) { worst = fmaxf(worst, fabsf(t1[j] - at[j])); bob_seen = fmaxf(bob_seen, fabsf(t0[6][j] - t6[j])); }
        for (int i = 0; i < 16; i++) { float t[3]; load_xf(part_addr(i), R, t); for (int j = 0; j < 3; j++) rigid = fmaxf(rigid, fabsf((t[j] - t0[i][j]) - (at[j] - t0[6][j]))); }
        vrbody_restore(RAM);
        CHECK(memcmp(RAM, RAM0, sizeof RAM) == 0, "ancla: restore devuelve la RAM");
        for (int i = 1; i < 10; i++) { float t[3]; load_xf(part_addr(i), R, t); for (int j = 0; j < 3; j++) t[j] -= bob[j]; store_xf(part_addr(i), R, t); }
    }
    printf("cuerpo anclado: balanceo de la cabeza del modelo %.0f uds -> desvio de la cabeza dibujada respecto al ojo %.2f uds (cuerpo rigido: %.2f)\n", bob_seen, worst, rigid);
    CHECK(bob_seen > 25.0f, "ancla: la prueba debe traer balanceo (%g)", bob_seen);
    CHECK(worst <= 1.0f, "ancla: la cabeza del modelo debe quedar en head_at (error %g)", worst);
    CHECK(rigid <= 1.0f, "ancla: las 16 piezas se desplazan lo mismo (error %g)", rigid);
    g_view.anchor = 0; memset(g_view.head_at, 0, sizeof g_view.head_at);
}

/* Gateo bajo geometría baja: la cámara aparta el ojo del modelo (VrBodyView.shift); todo el cuerpo del jugador se dibuja desplazado igual, para que no quede
 * por delante del ojo. Sin empuje (shift 0 / no finito) no cambia nada. restore devuelve la RAM. */
static void place_hands_at(float yaw_deg, float spread_deg);
static void test_eye_shift(void) {
    const float S[3] = {-120.0f, 0.0f, 200.0f};
    /* sin manos: solo el desplazamiento (world += S, screen += R_ojo * S, rotaciones intactas) */
    { VrBodyConfig cfg; legacy_cfg(&cfg); build_body(900); vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
      float Re[9], dS[3]; for (int i = 0; i < 9; i++) Re[i] = r16(BA_VIEW_CHANNEL + BCH_CAM_INV + 2 * i) / 4096.0f;
      mv(dS, Re, S);
      float R0[16][9], t0[16][3], Rs0[16][9], ts0[16][3];
      for (int i = 0; i < 16; i++) { load_xf(part_addr(i), R0[i], t0[i]); load_xf(part_addr(i) + 0x20, Rs0[i], ts0[i]); }
      for (int k = 0; k < 3; k++) g_view.shift[k] = S[k];
      vrbody_apply(RAM);
      int bad = 0; float worst = 0;
      for (int i = 0; i < 16; i++) {
          float R1[9], t1[9], Rs1[9], ts1[3];
          load_xf(part_addr(i), R1, t1); load_xf(part_addr(i) + 0x20, Rs1, ts1);
          for (int j = 0; j < 9; j++) if (R1[j] != R0[i][j] || Rs1[j] != Rs0[i][j]) bad++;
          for (int j = 0; j < 3; j++) { worst = fmaxf(worst, fabsf(t1[j] - (t0[i][j] + S[j]))); worst = fmaxf(worst, fabsf(ts1[j] - (ts0[i][j] + dS[j]))); }
      }
      CHECK(bad == 0, "shift: las rotaciones no deben cambiar (%d)", bad);
      CHECK(worst <= 1.0f, "shift: las 16 piezas deben moverse S en world y R_ojo*S en screen (error %g)", worst);
      vrbody_restore(RAM);
      CHECK(memcmp(RAM, RAM0, sizeof RAM) == 0, "shift: restore devuelve la RAM"); }
    /* shift no finito o cero: la RAM no cambia */
    { VrBodyConfig cfg; legacy_cfg(&cfg); build_body(901); vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
      g_view.shift[0] = NAN; g_view.shift[2] = 50.0f; vrbody_apply(RAM);
      CHECK(memcmp(RAM, RAM0, sizeof RAM) == 0, "shift no finito: no se toca nada");
      vrbody_restore(RAM);
      memset(g_view.shift, 0, sizeof g_view.shift); vrbody_apply(RAM);
      CHECK(memcmp(RAM, RAM0, sizeof RAM) == 0, "shift 0 sin manos: no se toca nada"); vrbody_restore(RAM); }
    /* con manos y torso_follow: pelvis, cuello y piernas siguen el desplazamiento, y que crezca de frame en frame (entrada al conducto) no se confunde con marcha: la pelvis no gira */
    { VrBodyConfig cfg; vrbody_config_default(&cfg); cfg.hand_pitch_deg = 0; cfg.near_hide_m = cfg.near_hide_vtx_m = 0;
      build_body(902); vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
      const float yaw0 = yaw_of(0), ya = yaw0 * 0.0174532925f, Sf[3] = {cosf(ya) * 230.0f, 0.0f, -sinf(ya) * 230.0f};   /* lateral a la mirada */
      g_view.gaze_yaw_deg = yaw0; place_hands_at(yaw0, 15.0f);
      float R_[9], t0[3], t4[3], t5[3], t10[3];
      load_xf(part_addr(0), R_, t0); load_xf(part_addr(4), R_, t4); load_xf(part_addr(5), R_, t5); load_xf(part_addr(10), R_, t10);
      for (int f = 0; f < 12; f++) {
          float k = (float)(f + 1) / 12.0f;
          for (int j = 0; j < 3; j++) g_view.shift[j] = Sf[j] * k;
          memcpy(RAM, RAM0, sizeof RAM); vrbody_apply(RAM);
          if (f == 11) {
              float tq[3], d;
              load_xf(part_addr(0), R_, tq);
              d = fmaxf(fabsf(tq[0] - (t0[0] + Sf[0])), fmaxf(fabsf(tq[1] - t0[1]), fabsf(tq[2] - (t0[2] + Sf[2]))));
              CHECK(d <= 1.0f, "shift+follow: pelvis desplazada S (error %g)", d);
              load_xf(part_addr(5), R_, tq);
              d = fmaxf(fabsf(tq[0] - (t5[0] + Sf[0])), fmaxf(fabsf(tq[1] - t5[1]), fabsf(tq[2] - (t5[2] + Sf[2]))));
              CHECK(d <= 1.0f, "shift+follow: cuello desplazado S (error %g)", d);
              load_xf(part_addr(10), R_, tq);
              d = fmaxf(fabsf(tq[0] - (t10[0] + Sf[0])), fmaxf(fabsf(tq[1] - t10[1]), fabsf(tq[2] - (t10[2] + Sf[2]))));
              CHECK(d <= 2.0f, "shift+follow: pierna desplazada S (error %g)", d);
              CHECK(fabsf(wrap180t(yaw_of(0) - yaw0)) <= 1.0f, "shift creciente: la pelvis no debe girar por el desplazamiento (%g grados)", wrap180t(yaw_of(0) - yaw0));
              float Ra[9], ta[3];   /* la mano de la animación para colgar lo que lleva el juego (otro actor, sin el desplazamiento del ojo): la de la RAM original */
              CHECK(vrbody_hand_anim(1, Ra, ta), "shift: mano derecha manejada");
              d = fmaxf(fabsf(ta[0] - t4[0]), fmaxf(fabsf(ta[1] - t4[1]), fabsf(ta[2] - t4[2])));
              CHECK(d <= 1.0f, "shift: vrbody_hand_anim sin el desplazamiento del ojo (error %g)", d);
          }
          vrbody_restore(RAM);
          CHECK(memcmp(RAM, RAM0, sizeof RAM) == 0, "shift+follow: restore devuelve la RAM (frame %d)", f);
      } }
}

/* Marcha y manos: la cadera del juego avanza en una dirección => la pelvis se alinea con ella (de espaldas, con la contraria) sin salir de la zona muerta de la mirada;
 * las manos delante de la mirada tuercen un poco el pecho */
static void place_hands_at(float yaw_deg, float spread_deg) {     /* ambas manos a 450 mm del ojo en el rumbo yaw +- spread */
    float th = (g_view.theta_deg + 180) * 3.14159265f / 180, cs = cosf(th), sn = sinf(th);
    float Tt[9] = {cs, 0, -sn, 0, 1, 0, sn, 0, cs};
    for (int sd = 0; sd < 2; sd++) {
        float y = (yaw_deg + (sd ? -spread_deg : spread_deg)) * 0.0174532925f, dw[3] = {sinf(y) * 450.0f, -100.0f, cosf(y) * 450.0f}, pu[3];
        mv(pu, Tt, dw);
        set_hand(sd, pu[0] / 1000 + g_view.p_ref[0], pu[1] / 1000 + g_view.p_ref[1], pu[2] / 1000 + g_view.p_ref[2], 0);
    }
}
static void pelvis_run(int seed, float gaze_off, int walk, float head_off, float hands_off, float *pel, float *chest, float *gaze_out, float *head_out) {
    VrBodyConfig cfg; vrbody_config_default(&cfg); cfg.hand_pitch_deg = 0;
    build_body(seed); vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
    const float P0 = yaw_of(0), gaze = wrap180t(P0 + gaze_off), H = wrap180t(P0 + head_off);
    g_view.gaze_yaw_deg = gaze;
    float R0[9], pos[3]; load_xf(part_addr(0), R0, pos);
    for (int f = 0; f < 240; f++) {
        place_hands_at(gaze + hands_off, 25.0f);
        memcpy(RAM, RAM0, sizeof RAM);
        if (walk) { pos[0] += sinf(H * 0.0174532925f) * 60.0f; pos[2] += cosf(H * 0.0174532925f) * 60.0f; store_xf(part_addr(0), R0, pos); }   /* 60 mm por 0,02 s = 3 m/s */
        vrbody_apply(RAM);
        if (f == 239) { *pel = yaw_of(0); *chest = yaw_of(1); }
        vrbody_restore(RAM);
    }
    *gaze_out = gaze; *head_out = H;
}
static void test_pelvis_walk(void) {
    float pel, ch, gz, hd;
    /* parado, mirada dentro de la zona muerta: la pelvis no se mueve; el pecho se tuerce la mitad del desvío */
    pelvis_run(600, 25.0f, 0, 0, 0, &pel, &ch, &gz, &hd);
    CHECK(fabsf(wrap180t(pel - (gz - 25.0f))) < 1.0f, "parado: la pelvis se movió (%g)", wrap180t(pel - (gz - 25.0f)));
    CHECK(fabsf(wrap180t(ch - (gz - 12.5f))) < 1.5f, "parado: torsión del pecho %g", wrap180t(ch - (gz - 25.0f)));
    /* andando 30 grados a un lado de la mirada: la pelvis se alinea con la marcha */
    pelvis_run(601, 0, 1, 30.0f, 0, &pel, &ch, &gz, &hd);
    CHECK(fabsf(wrap180t(pel - hd)) < 2.0f, "andando: pelvis %g grados fuera de la marcha", wrap180t(pel - hd));
    CHECK(fabsf(wrap180t(ch - (hd + (gz - hd) * 0.5f))) < 2.0f, "andando: el pecho queda entre la marcha y la mirada: %g", wrap180t(ch - (hd + (gz - hd) * 0.5f)));
    /* andando de espaldas (marcha opuesta a la mirada): la pelvis mira a la mirada, no a la marcha */
    pelvis_run(602, 0, 1, 180.0f, 0, &pel, &ch, &gz, &hd);
    CHECK(fabsf(wrap180t(pel - gz)) < 2.0f, "de espaldas: la pelvis debe seguir mirando a la mirada: %g", wrap180t(pel - gz));
    /* andando de lado a 90 grados (fuera de la zona muerta de 40): la pelvis se queda en el borde de la zona */
    pelvis_run(603, 0, 1, 90.0f, 0, &pel, &ch, &gz, &hd);
    CHECK(fabsf(wrap180t(pel - gz)) < 41.5f && fabsf(wrap180t(pel - gz)) > 35.0f, "de lado: la pelvis debe quedar en el borde de la zona muerta: %g", wrap180t(pel - gz));
    /* manos delante, 30 grados a un lado de la mirada: el pecho se tuerce torso_hand_share * share de 30 grados */
    pelvis_run(604, 0, 0, 0, 30.0f, &pel, &ch, &gz, &hd);
    { float exp = 0.3f * 30.0f * 0.5f, got = wrap180t(ch - pel);
      CHECK(fabsf(got - exp) < 1.5f, "manos: torsión del pecho %g, esperada %g", got, exp); }
}


/* dt medido: la velocidad de marcha se calcula con el tiempo emulado entre applies (vrbody_set_dt), no con frame_dt_s fijo */
static float walk_yaw_with_dt(float dt, int use_dt) {
    VrBodyConfig cfg; vrbody_config_default(&cfg); cfg.hand_pitch_deg = 0; cfg.torso_hand_share = 0;
    build_body(620); vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
    const float P0 = yaw_of(0); g_view.gaze_yaw_deg = P0;
    float R0[9], pos[3]; load_xf(part_addr(0), R0, pos);
    float res = 0;
    for (int f = 0; f < 240; f++) {
        place_hands_at(P0, 25.0f);
        memcpy(RAM, RAM0, sizeof RAM);
        pos[0] += 10.0f; store_xf(part_addr(0), R0, pos);          /* 10 mm por frame hacia +X (rumbo 90) */
        if (use_dt) vrbody_set_dt(dt);
        vrbody_apply(RAM);
        if (f == 239) res = wrap180t(yaw_of(0) - P0);
        vrbody_restore(RAM);
    }
    vrbody_set_dt(0);
    return res;
}
static void test_dt_measured(void) {
    float a = walk_yaw_with_dt(0, 0);         /* 10 mm / 0,02 s = 0,5 m/s: andando => la pelvis se alinea con +X (acotada a la zona muerta) */
    float b = walk_yaw_with_dt(0.05f, 1);     /* 10 mm / 0,05 s = 0,2 m/s: parado => la pelvis no se mueve */
    float c = walk_yaw_with_dt(0.005f, 1);    /* 2 m/s */
    CHECK(fabsf(a) > 20.0f, "dt por defecto: debería andar (giro %g)", a);
    CHECK(fabsf(b) < 1.0f, "dt = 0,05 s: 0,2 m/s no es marcha (giro %g)", b);
    CHECK(fabsf(c) > 20.0f, "dt = 5 ms: debería andar (giro %g)", c);
}
/* Tras un hueco en el que la pelvis estuvo fuera de rango (inclinada), la primera posición válida no cuenta como desplazamiento (falso «andando») */
static void test_pelvis_gap(void) {
    VrBodyConfig cfg; vrbody_config_default(&cfg); cfg.hand_pitch_deg = 0; cfg.torso_hand_share = 0;
    build_body(630); vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
    const float P0 = yaw_of(0); g_view.gaze_yaw_deg = P0;
    float R0[9], pos[3]; load_xf(part_addr(0), R0, pos);
    float Rx[9] = {1, 0, 0, 0, cosf(1.05f), -sinf(1.05f), 0, sinf(1.05f), cosf(1.05f)}, Rt[9]; mm(Rt, R0, Rx);
    for (int f = 0; f < 4; f++) {
        place_hands_at(P0, 25.0f);
        memcpy(RAM, RAM0, sizeof RAM); store_xf(part_addr(0), f == 2 ? Rt : R0, pos);
        if (f == 3) pos[0] += 100.0f, store_xf(part_addr(0), R0, pos);
        vrbody_apply(RAM);
        if (f == 3) CHECK(fabsf(wrap180t(yaw_of(0) - P0)) < 0.5f, "hueco con la pelvis inclinada: falso andando (giro %g)", wrap180t(yaw_of(0) - P0));
        vrbody_restore(RAM);
    }
}

/* RAM corrupta: no debe caerse ni escribir fuera */
static void test_fuzz(void) {
    VrBodyConfig cfg; legacy_cfg(&cfg); cfg.hand_pitch_deg = 0;
    build_body(90); vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
    set_hand(0, -0.2f, 1.3f, -0.4f, 0); set_hand(1, 0.2f, 1.3f, -0.4f, 0);
    for (int it = 0; it < 3000; it++) {
        memcpy(RAM, RAM0, sizeof RAM);
        int nc = 1 + (int)(rnd() % 12);
        for (int k = 0; k < nc; k++) {
            uint32_t a;
            switch (rnd() % 5) {
            case 0: a = BA_PLAYER_BODY; break;
            case 1: a = OB; break;
            case 2: a = PARTSET + (rnd() % 0x60); break;
            case 3: a = part_addr((int)(rnd() % 16)) + (rnd() % 0x5C); break;
            default: a = PMODELS + (rnd() % (0x3C * 16)); break;
            }
            uint32_t v = (rnd() & 1) ? rnd() : (0x80000000u | ((rnd() % 0x200000) & ~3u));
            if (rnd() % 7 == 0) v = 0xFFFFFFFFu;
            w32(a & ~3u, (int32_t)v);
        }
        for (int f = 0; f < 3; f++) { vrbody_apply(RAM); vrbody_restore(RAM); }
    }
    /* RAM totalmente aleatoria */
    for (int it = 0; it < 300; it++) {
        for (uint32_t i = 0; i < RAMSZ; i += 4) { uint32_t v = rnd(); memcpy(RAM + i, &v, 4); }
        w32(BA_PLAYER_BODY, (int32_t)OB); w32(OB, (int32_t)PARTSET); w16(PARTSET + 0x2E, 16);
        vrbody_apply(RAM); vrbody_restore(RAM);
    }
    CHECK(!vrbody_pending(), "fuzz: nada pendiente al final");
}

/* Puño en la RAM: la malla de la mano (vértices + normales) se deforma al cerrar el agarre y vuelve byte a byte tras el dibujado. */
static const uint32_t FM_BASE[2] = {0x80190000u, 0x801A0000u};     /* malla de la mano izquierda (pieza 9) y derecha (pieza 4) */
static void install_hand_mesh(int s, int mirror) {
    SynthHand h; synth_hand(&h, mirror);
    uint32_t b = FM_BASE[s], mdl = PMODELS + 0x3C * (uint32_t)(s ? 4 : 9);
    memcpy(RAM + (b & 0x1FFFFF), h.v, (size_t)h.nv * 8);
    memcpy(RAM + ((b + 0x400) & 0x1FFFFF), h.vidx, (size_t)h.nf * 4);
    memcpy(RAM + ((b + 0x500) & 0x1FFFFF), h.n, (size_t)h.nn * 8);
    memcpy(RAM + ((b + 0x900) & 0x1FFFFF), h.nidx, (size_t)h.nf * 4);
    w32(mdl + 0x04, h.nf); w32(mdl + 0x34, h.nv); w32(mdl + 0x38, (int32_t)b); w32(mdl + 0x3C, (int32_t)(b + 0x400));
    w32(mdl + 0x40, h.nn); w32(mdl + 0x44, (int32_t)(b + 0x500)); w32(mdl + 0x48, (int32_t)(b + 0x900));
}
static int mesh_diff(int s, int nv) {      /* nº de vértices distintos entre RAM y RAM0 */
    int d = 0; const int16_t *a = (const int16_t *)(RAM + (FM_BASE[s] & 0x1FFFFF)), *b = (const int16_t *)(RAM0 + (FM_BASE[s] & 0x1FFFFF));
    for (int i = 0; i < nv; i++) d += a[4 * i] != b[4 * i] || a[4 * i + 1] != b[4 * i + 1] || a[4 * i + 2] != b[4 * i + 2];
    return d;
}
static void run_fist_frames(float grip, int frames, int *driven_last) {
    static uint8_t before[RAMSZ];
    for (int f = 0; f < frames; f++) {
        set_hand(0, -0.25f, 1.3f, -0.4f, 0.3f); set_hand(1, 0.30f, 1.25f, -0.45f, -0.2f);
        vrbody_set_grip(0, grip); vrbody_set_grip(1, grip);
        memcpy(RAM, RAM0, RAMSZ); memcpy(before, RAM, RAMSZ);
        *driven_last = vrbody_apply(RAM);
        if (f == frames - 1) return;                      /* el último apply se deja sin restaurar para inspeccionarlo */
        vrbody_restore(RAM);
        CHECK(memcmp(RAM, before, RAMSZ) == 0, "restore no devuelve la RAM (incluida la malla) byte a byte (f=%d)", f);
    }
}
static void test_fist_glue(void) {
    VrBodyStats st0, st1;
    int driven = 0;
    VrBodyConfig cfg; legacy_cfg(&cfg); cfg.hand_pitch_deg = 0;
    vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
    build_body(31); install_hand_mesh(0, 1); install_hand_mesh(1, 0); memcpy(RAM0, RAM, RAMSZ);
    /* mano abierta: la malla no se toca */
    vrbody_stats(&st0);
    run_fist_frames(0.0f, 40, &driven);
    CHECK(driven == 2 && mesh_diff(0, 28) == 0 && mesh_diff(1, 28) == 0, "con el agarre abierto la malla no debe tocarse");
    vrbody_restore(RAM);
    vrbody_stats(&st1); CHECK(st1.fist_applies == st0.fist_applies, "sin agarre no hay puño");
    /* agarre cerrado: las dos mallas se deforman y vuelven exactas */
    run_fist_frames(1.0f, 60, &driven);
    const int nv = 8 + 8 + 8;   /* 24 vértices de la malla sintética */
    CHECK(driven == 2, "dos brazos");
    CHECK(mesh_diff(0, nv) >= 6 && mesh_diff(1, nv) >= 6, "la malla de ambas manos debe deformarse: %d y %d vértices", mesh_diff(0, nv), mesh_diff(1, nv));
    for (int i = 0; i < 8; i++) {          /* la palma (muñeca y nudillos) no se mueve */
        const int16_t *a = (const int16_t *)(RAM + (FM_BASE[1] & 0x1FFFFF)), *b = (const int16_t *)(RAM0 + (FM_BASE[1] & 0x1FFFFF));
        CHECK(a[4 * i] == b[4 * i] && a[4 * i + 1] == b[4 * i + 1], "palma movida (%d)", i);
    }
    CHECK(vrbody_pending(), "pendiente");
    vrbody_restore(RAM);
    CHECK(memcmp(RAM, RAM0, RAMSZ) == 0, "tras restaurar, la RAM (malla incluida) es la original");
    vrbody_stats(&st1); CHECK(st1.fist_applies >= st0.fist_applies + 2, "puños aplicados: %u", st1.fist_applies - st0.fist_applies);
    /* cierre progresivo: con medio agarre la deformación es menor que con el agarre completo */
    {
        int16_t full[24 * 4], half[24 * 4];
        run_fist_frames(1.0f, 60, &driven); memcpy(full, RAM + (FM_BASE[1] & 0x1FFFFF), sizeof full); vrbody_restore(RAM);
        vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
        run_fist_frames(0.5f, 60, &driven); memcpy(half, RAM + (FM_BASE[1] & 0x1FFFFF), sizeof half); vrbody_restore(RAM);
        const int16_t *r = (const int16_t *)(RAM0 + (FM_BASE[1] & 0x1FFFFF));
        float dfull = 0, dhalf = 0;
        for (int i = 8; i < 16; i++) { dfull += fabsf((float)(full[4 * i + 1] - r[4 * i + 1])); dhalf += fabsf((float)(half[4 * i + 1] - r[4 * i + 1])); }
        CHECK(dhalf > 0 && dhalf < dfull, "cierre progresivo: medio %.0f, completo %.0f", dhalf, dfull);
    }
    /* puño previo a la captura nativa: tras un apply con agarre, prefist deja la malla cerrada ANTES del siguiente apply y restore la devuelve aunque no haya apply */
    { vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
      run_fist_frames(1.0f, 60, &driven); vrbody_restore(RAM);
      CHECK(memcmp(RAM, RAM0, RAMSZ) == 0, "prefist: RAM original antes");
      vrbody_prefist(RAM);
      CHECK(mesh_diff(0, nv) >= 6 && mesh_diff(1, nv) >= 6, "prefist: la malla debe llegar cerrada a la captura");
      vrbody_restore(RAM);
      CHECK(memcmp(RAM, RAM0, RAMSZ) == 0, "prefist: restore sin apply devuelve la malla"); }
    /* desactivado por configuración */
    { VrBodyConfig c2 = cfg; c2.fist_enabled = 0; vrbody_init(&c2); vrbody_set_view_fn(view_fn);
      run_fist_frames(1.0f, 40, &driven); CHECK(mesh_diff(0, nv) == 0 && mesh_diff(1, nv) == 0, "fist_enabled = 0: malla rígida"); vrbody_restore(RAM); }
    /* vrbody_set_fist_enabled(0) en caliente (las manos nuevas hacen el puño) y estado de la mano para el golpe */
    { vrbody_init(&cfg); vrbody_set_view_fn(view_fn); vrbody_set_fist_suppressed(1);
      run_fist_frames(1.0f, 60, &driven);
      CHECK(mesh_diff(0, nv) == 0 && mesh_diff(1, nv) == 0 && vrbody_config()->fist_enabled == 1, "vrbody_set_fist_suppressed(1): malla rígida y el ajuste del usuario intacto");
      float g = -1, R[9], t[3], fp1[3], fp0[3];
      CHECK(vrbody_hand_state(1, &g, R, t) && vrbody_hand_state(0, NULL, NULL, NULL) && g > 0.9f, "estado de la mano derecha tras el apply (agarre %.2f)", g);
      float det = R[0] * (R[4] * R[8] - R[5] * R[7]) - R[1] * (R[3] * R[8] - R[5] * R[6]) + R[2] * (R[3] * R[7] - R[4] * R[6]);
      CHECK(fabsf(det - 1.0f) < 0.01f, "rotación de la mano: det %.4f", det);
      CHECK(!vrbody_hand_state(2, &g, R, t) && !vrbody_hand_state(-1, &g, R, t), "lado inválido");
      CHECK(vrbody_fist_point(1, fp1), "punto de golpe con puño");
      float d1 = sqrtf((fp1[0] - t[0]) * (fp1[0] - t[0]) + (fp1[1] - t[1]) * (fp1[1] - t[1]) + (fp1[2] - t[2]) * (fp1[2] - t[2]));
      vrbody_restore(RAM);
      {   /* marco de la mano en la animación del juego (lo que usa el arma que cuelga de la mano): el de la RAM original, no el de la IK */
          float Ra[9], ta[3], Ro[9], to[3], dt_ = 0, dr = 0, di = 0;
          load_xf(part_addr(4), Ro, to);
          CHECK(vrbody_hand_anim(1, Ra, ta) && vrbody_hand_anim(0, NULL, NULL) && !vrbody_hand_anim(2, Ra, ta) && !vrbody_hand_anim(-1, Ra, ta), "vrbody_hand_anim: lados");
          for (int i = 0; i < 3; i++) { dt_ = fmaxf(dt_, fabsf(ta[i] - to[i])); di = fmaxf(di, fabsf(ta[i] - t[i])); }
          for (int i = 0; i < 9; i++) dr = fmaxf(dr, fabsf(Ra[i] - Ro[i]));
          CHECK(dt_ < 0.01f && dr < 0.01f, "vrbody_hand_anim: coincide con la animación de la RAM original (t %.3f, R %.4f)", dt_, dr);
          CHECK(di > 1.0f, "vrbody_hand_anim: distinto del marco tras la IK (%.1f mm)", di);
      }
      run_fist_frames(0.0f, 60, &driven);
      vrbody_hand_state(1, NULL, NULL, t);
      CHECK(vrbody_fist_point(1, fp0), "punto de golpe con la mano abierta");
      float d0 = sqrtf((fp0[0] - t[0]) * (fp0[0] - t[0]) + (fp0[1] - t[1]) * (fp0[1] - t[1]) + (fp0[2] - t[2]) * (fp0[2] - t[2]));
      CHECK(d1 > 40.0f && d1 < 140.0f && fabsf(d0 - d1) < 0.01f && sqrtf((fp1[0]-fp0[0])*(fp1[0]-fp0[0])+(fp1[1]-fp0[1])*(fp1[1]-fp0[1])+(fp1[2]-fp0[2])*(fp1[2]-fp0[2])) < 5.0f, "punto de golpe constante (nudillos %.1f mm con puño, %.1f abierta)", d1, d0);
      vrbody_restore(RAM);
      vrbody_set_armed(1); run_fist_frames(0.0f, 60, &driven);       /* con arma ya no se fuerza un puño: la pose de la mano la decide vr_hand_pose */
      float ga = -1; vrbody_hand_state(1, &ga, NULL, NULL);
      CHECK(ga < 0.05f, "mano dominante armada con el agarre a 0: sin puño forzado (%.2f)", ga);
      vrbody_set_armed(0);
      float nope[3];
      vrbody_restore(RAM);
      vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
      CHECK(!vrbody_fist_point(1, nope) && !vrbody_fist_point(2, nope) && !vrbody_fist_point(1, NULL), "sin apply no hay punto");
      vrbody_init(&cfg); vrbody_set_view_fn(view_fn); vrbody_set_fist_suppressed(0); }
    /* malla no reconocida (recuentos absurdos): se deja rígida y se cuenta */
    { vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
      w32(PMODELS + 0x3C * 4 + 0x34, 500); memcpy(RAM0, RAM, RAMSZ);
      vrbody_stats(&st0); run_fist_frames(1.0f, 40, &driven); vrbody_stats(&st1);
      CHECK(mesh_diff(1, nv) == 0 && st1.fist_skipped > st0.fist_skipped, "malla no válida: rígida y contada"); vrbody_restore(RAM);
      w32(PMODELS + 0x3C * 4 + 0x34, 24); memcpy(RAM0, RAM, RAMSZ); }
    /* si el modelo cambió entre el apply y el restore, no se escribe sobre la malla nueva */
    { vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
      run_fist_frames(1.0f, 60, &driven);
      w32(PMODELS + 0x3C * 4 + 0x38, (int32_t)(FM_BASE[1] + 0x40));      /* otro array de vértices */
      vrbody_stats(&st0); vrbody_restore(RAM); vrbody_stats(&st1);
      CHECK(st1.stale_dropped > st0.stale_dropped, "restore con la malla cambiada no escribe");
      w32(PMODELS + 0x3C * 4 + 0x38, (int32_t)FM_BASE[1]); }
}

/* Cuerpo por delante al atacar: las piezas (salvo piernas/pies 10-15) con el centro delante del ojo (mirada horizontal) y a < near_hide_m, o con algún vértice a < near_hide_vtx_m, se ocultan
 * (+0x4C = 0) durante el fotograma y restore las devuelve; las piernas nunca; contador near_hidden. */
static void test_near_hide(void) {
    static const uint32_t VERTS = 0x80100000u;
    for (int sc = 0; sc < 8; sc++) {
        VrBodyConfig cfg; legacy_cfg(&cfg); cfg.hand_pitch_deg = 0; cfg.near_hide_m = 0.30f; cfg.near_hide_vtx_m = 0.12f;
        build_body(500 + sc);
        float R[9], t[3];
        int ref = sc == 3 ? 12 : sc == 4 || sc == 5 ? 5 : 1;       /* pieza junto a la que se pone el ojo */
        load_xf(part_addr(ref), R, t);
        float yaw = sc == 6 ? 90.0f : 0.0f, gx = sinf(yaw * 3.14159265f / 180.0f), gz = cosf(yaw * 3.14159265f / 180.0f);   /* mirada horizontal: 0 = +Z, + hacia +X */
        float back = sc == 1 ? 400.0f : sc == 2 ? -200.0f : 200.0f;    /* mm de ojo detrás de la pieza (negativo: delante de ella) */
        g_view.gaze_yaw_deg = yaw; g_view.third = sc == 7;      /* escenario 7 = como el 0 pero con la cámara en tercera persona: el cuerpo no se oculta */
        g_view.eye[0] = t[0] - gx * back; g_view.eye[1] = t[1]; g_view.eye[2] = t[2] - gz * back;
        if (sc == 4 || sc == 5) {      /* cuello con dos vértices: A en su origen y B a 4 m (el centro queda lejos del ojo); el ojo a 100 mm (sc 4) o 150 mm (sc 5) de A */
            uint32_t mdl = PMODELS + 0x3C * 5u;
            w32(mdl + 0x34, 2); w32(mdl + 0x38, (int32_t)VERTS);
            w16(VERTS + 0, 0); w16(VERTS + 2, 0); w16(VERTS + 4, 0); w16(VERTS + 6, 0);
            w16(VERTS + 8, 4000); w16(VERTS + 10, 0); w16(VERTS + 12, 0); w16(VERTS + 14, 0);
            g_view.eye[0] = t[0] + (sc == 4 ? 100.0f : 150.0f); g_view.eye[1] = t[1]; g_view.eye[2] = t[2];
        }
        memcpy(RAM0, RAM, sizeof RAM);
        vrbody_init(&cfg); vrbody_set_view_fn(view_fn);
        memset(g_hands, 0, sizeof g_hands);
        vrbody_apply(RAM);
        int hid[16], nh = 0;
        for (int i = 0; i < 16; i++) { hid[i] = r16(part_addr(i) + 0x4C) == 0; nh += hid[i]; }
        for (int i = 10; i < 16; i++) CHECK(!hid[i], "escenario %d: la pierna/pie %d no debe ocultarse", sc, i);
        if (sc == 0 || sc == 6) CHECK(hid[1], "escenario %d: torso a 0,20 m delante del ojo: oculto", sc);
        if (sc == 7) CHECK(nh == 0, "escenario 7: tercera persona: ninguna pieza se oculta por cercanía al ojo de primera persona (%d)", nh);
        if (sc == 1) CHECK(!hid[1], "escenario 1: torso a 0,40 m: no se oculta");
        if (sc == 2) CHECK(!hid[1], "escenario 2: torso detrás del ojo: no se oculta");
        if (sc == 4) CHECK(hid[5], "escenario 4: vértice a 0,10 m oculta el cuello");
        if (sc == 5) CHECK(!hid[5], "escenario 5: vértice a 0,15 m no oculta el cuello (centro lejos)");
        VrBodyStats st; vrbody_stats(&st);
        CHECK(st.near_hidden == (uint32_t)nh, "escenario %d: near_hidden=%u ocultas=%d", sc, st.near_hidden, nh);
        CHECK(!!vrbody_ram_dirty() == (nh > 0), "escenario %d: ram_dirty=%d con %d ocultas", sc, vrbody_ram_dirty(), nh);
        vrbody_restore(RAM);
        CHECK(memcmp(RAM, RAM0, sizeof RAM) == 0, "escenario %d: restore no devuelve la RAM", sc);
        cfg.near_hide_m = 0; cfg.near_hide_vtx_m = 0; vrbody_init(&cfg); vrbody_set_view_fn(view_fn);   /* desactivado: nada */
        vrbody_apply(RAM); vrbody_restore(RAM);
        CHECK(memcmp(RAM, RAM0, sizeof RAM) == 0, "escenario %d: near_hide = 0 no debe tocar nada", sc);
    }
}

int main(void) {
    test_math();
    test_solver();
    test_solver_nonfinite();
    test_natural_elbow();
    test_stretch_continuous(); test_stretch_bone(); test_resolve_stretch(); test_shoulder_blend(); test_twist_continuity(); test_wrist_continuity();
    test_continuity();
    test_glue();
    test_hand_axes(); test_armed_hand_fixed_offset(); test_hand_pitch_api(); test_default_roll(); test_jab_and_grab();
    test_visibility();
    test_two_sided(); test_own_hands(); test_own_hands_both(); test_arm_live(); test_torso_follow(); test_eye_shift(); test_head_anchor(); test_pelvis_walk(); test_dt_measured(); test_pelvis_gap();
    test_fist_glue();
    test_near_hide();
    test_mirror();
    test_fuzz();
    VrBodyStats st; vrbody_stats(&st);
    printf("  stats: applies %u, brazos %u, restores %u, estirados %u, hombro %u, muñeca %u\n", st.applies, st.arms_driven, st.restores, st.stretched, st.shoulder_limit, st.wrist_limit);
    if (g_fail) { printf("FALLO: %d de %d comprobaciones\n", g_fail, g_checks); return 1; }
    printf("OK: %d comprobaciones\n", g_checks);
    return 0;
}
