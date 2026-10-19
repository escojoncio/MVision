/* Pruebas de vr_body: solver (propiedades geométricas con miles de casos aleatorios), matemática, y el pegamento sobre
 * una RAM sintética con la estructura del modelo del jugador (solo escribe las 6 piezas, restaura byte a byte, fundidos,
 * robustez ante RAM corrupta). No necesita datos del juego. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_body.h"
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
    CHECK(fabsf(lenv(d) - L1) < 0.02f, "hueso 1: %g vs %g", lenv(d), L1);
    float d2[3]; for (int i = 0; i < 3; i++) d2[i] = o->wrist[i] - o->elbow[i];
    CHECK(fabsf(lenv(d2) - L2) < 0.02f, "hueso 2: %g vs %g", lenv(d2), L2);
    CHECK(ortho_err(o->R_upper) < 1e-4f && fabsf(det3(o->R_upper) - 1) < 1e-4f, "R_upper no ortonormal");
    CHECK(ortho_err(o->R_fore) < 1e-4f && fabsf(det3(o->R_fore) - 1) < 1e-4f, "R_fore no ortonormal");
    CHECK(ortho_err(o->R_hand) < 1e-4f && fabsf(det3(o->R_hand) - 1) < 1e-4f, "R_hand no ortonormal");
    float t[3]; mv(t, o->R_upper, rig->elbow_off);
    CHECK(fabsf(t[0] - d[0]) < 0.02f && fabsf(t[1] - d[1]) < 0.02f && fabsf(t[2] - d[2]) < 0.02f, "R_upper no lleva el codo a su sitio");
    mv(t, o->R_fore, rig->wrist_off);
    CHECK(fabsf(t[0] - d2[0]) < 0.02f && fabsf(t[1] - d2[1]) < 0.02f && fabsf(t[2] - d2[2]) < 0.02f, "R_fore no lleva la muñeca a su sitio");
    /* alcance */
    float v[3]; for (int i = 0; i < 3; i++) v[i] = in->wrist[i] - in->shoulder[i];
    float dist = lenv(v);
    float emin = lim->elbow_min_deg * 3.14159265f / 180;
    float dmin = sqrtf(L1 * L1 + L2 * L2 - 2 * L1 * L2 * cosf(emin)), dmax = (L1 + L2) * lim->reach_margin;
    float wv[3]; for (int i = 0; i < 3; i++) wv[i] = o->wrist[i] - in->shoulder[i];
    if (dist > dmax) {
        (*n_stretch)++;
        CHECK(o->flags & VRARM_STRETCHED, "falta STRETCHED");
        /* estirado: sin alargar huesos; la mano en la dirección del objetivo, a lo sumo dmax */
        CHECK(lenv(wv) <= ((o->flags & VRARM_SHOULDER_LIMIT) ? L1 + L2 : dmax) + 0.05f, "alcance %g > %g", lenv(wv), dmax);
        if (!(o->flags & VRARM_SHOULDER_LIMIT)) {
            float c = dotv(wv, v) / (lenv(wv) * dist);
            CHECK(c > 0.9999f, "estirado fuera de la línea al objetivo %g", c);
            CHECK(o->err_mm >= dist - dmax - 0.2f && o->err_mm <= dist - dmax + 0.2f, "err_mm %g vs %g", o->err_mm, dist - dmax);
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
    CHECK(fwd >= lim->shoulder_back - 2e-3f, "hombro hacia atrás %g < %g", fwd, lim->shoulder_back);
    CHECK(out_c >= lim->shoulder_cross - 2e-3f, "hombro cruzando %g < %g", out_c, lim->shoulder_cross);
    /* muñeca: desvío de la mano respecto al antebrazo */
    float Rrel[9], Tf[9] = {o->R_fore[0], o->R_fore[3], o->R_fore[6], o->R_fore[1], o->R_fore[4], o->R_fore[7], o->R_fore[2], o->R_fore[5], o->R_fore[8]};
    mm(Rrel, Tf, o->R_hand);
    float ax[3] = {WRI[0] / L2, WRI[1] / L2, WRI[2] / L2}, rv[3]; mv(rv, Rrel, ax);
    float sw = acosf(fmaxf(-1, fminf(1, dotv(rv, ax))));
    CHECK(sw <= lim->wrist_swing_deg * 3.14159265f / 180 + 0.003f, "desvío de muñeca %g", sw * 57.29578f);
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
    CHECK(n_stretch > 1000 && n_close > 100 && n_lim > 50 && n_flag_wrist > 100, "cobertura de casos: estirados %d cerca %d hombro %d muñeca %d", n_stretch, n_close, n_lim, n_flag_wrist);
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

/* El brazo es natural: con la mano delante del pecho el codo cuelga (por debajo de la recta hombro-muñeca) y hacia fuera */
static void test_natural_elbow(void) {
    VrArmLimits lim; vrbody_limits_default(&lim);
    for (int side = -1; side <= 1; side += 2) {
        VrArmRig rig = {(float)side, {ELB[0], ELB[1], ELB[2]}, {WRI[0], WRI[1], WRI[2]}};
        VrArmIn in; memset(&in, 0, sizeof in);
        in.torso[0] = in.torso[4] = in.torso[8] = 1; in.hand[0] = in.hand[4] = in.hand[8] = 1;
        in.shoulder[0] = side * 190; in.shoulder[1] = 260; in.shoulder[2] = -29;
        /* varios destinos delante / altos / bajos */
        float tg[][3] = {{0, 100, 450}, {side * 150, 300, 480}, {0, 500, 300}, {side * 400, 150, 200}, {side * 100, -100, 300}, {0, 0, 350}};
        for (unsigned k = 0; k < sizeof tg / sizeof *tg; k++) {
            for (int i = 0; i < 3; i++) in.wrist[i] = tg[k][i];
            float pol[3] = {0.45f * side, -1, -0.35f}; memcpy(in.pole, pol, sizeof pol);
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

/* =========================================== pegamento sobre RAM sintética =========================================== */
#define RAMSZ 0x200000
static uint8_t RAM[RAMSZ], RAM0[RAMSZ];

int vrpose_get_hands(VrHandPose out[2]);       /* definición fuerte de prueba (sustituye a la débil) */
static VrHandPose g_hands[2];
int vrpose_get_hands(VrHandPose out[2]) { memcpy(out, g_hands, sizeof g_hands); return g_hands[0].valid + g_hands[1].valid; }
static VrBodyView g_view;
static int g_view_ok;
static int view_fn(VrBodyView *v) { if (!g_view_ok) return 0; *v = g_view; return 1; }

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
    VrBodyConfig cfg; vrbody_config_default(&cfg); cfg.hand_pitch_deg = 0;
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
                    if (!inside) diff_outside++;
                }
                CHECK(diff_outside == 0, "bytes fuera de las 6 piezas modificados: %d", diff_outside);
                /* conectividad: cada hueso lleva su origen al siguiente (a 1,8 mm: enteros de t + cuantización de 1/4096 por 265 mm) */
                for (int arm = 0; arm < 2; arm++) {
                    int pi[3] = {arm ? 2 : 7, arm ? 3 : 8, arm ? 4 : 9};
                    float R[3][9], t[3][3];
                    for (int k = 0; k < 3; k++) load_xf(part_addr(pi[k]), R[k], t[k]);
                    for (int k = 0; k < 2; k++) {
                        float o[3], tl[3] = {(float)TOFF[pi[k + 1]][0], (float)TOFF[pi[k + 1]][1], (float)TOFF[pi[k + 1]][2]};
                        mv(o, R[k], tl); for (int c = 0; c < 3; c++) o[c] += t[k][c];
                        float d[3] = {o[0] - t[k + 1][0], o[1] - t[k + 1][1], o[2] - t[k + 1][2]};
                        CHECK(lenv(d) < 1.8f, "hueso %d del brazo %d desconectado %g mm", k, arm, lenv(d));
                    }
                    CHECK(ortho_err(R[0]) < 3e-3f && ortho_err(R[1]) < 3e-3f && ortho_err(R[2]) < 3e-3f, "matrices no ortonormales");
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
        float zc[3]; /* el marco de mano para rot identidad en usuario: columna Z en mundo = T * (0,0,1) */
        zc[0] = T[2]; zc[1] = T[5]; zc[2] = T[8];
        float palm[3]; for (int k = 0; k < 3; k++) palm[k] = tw[k] - zc[k] * 55.0f;
        float e[3] = {palm[0] - want[0], palm[1] - want[1], palm[2] - want[2]};
        printf("  alcance: error muñeca->palma %.2f mm (reach %.2f, flags %u)\n", lenv(e), d->sol.reach, d->sol.flags);
        CHECK(lenv(e) < 1.0f || (d->sol.flags & VRARM_STRETCHED), "la mano no llega a la pose pedida");
        vrbody_restore(RAM);
    }

    /* d) fuera de alcance: brazo extendido sin alargar huesos */
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
                CHECK(lenv(dd) <= (264.75f + 253.4f) + 0.1f, "brazo alargado: %g", lenv(dd));
            }
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
/* mode 0: pose = marco de vr_pose.h (sin cabeceo); 1: mando Sense con apuntado (agarre inclinado 35 grados, cabeceo automático); 2: Sense sin apuntado (cabeceo fijo -35);
 * 3: Sense calibrado con vrbody_calibrate_hands. En 1..3 el marco esperado de la mano es el del apuntado horizontal (guiñada de la pose) girado el alabeo. */
static void hand_axes_run(float roll_deg, int mode) {
    VrBodyConfig cfg; vrbody_config_default(&cfg); if (mode == 0) cfg.hand_pitch_deg = 0;
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
                if (mode) {     /* agarre = guiñada * Rx(35 grados) (medido en el Sense); apuntado = guiñada */
                    float h2 = yaw * 0.5f, a2 = 35.0f * 3.14159265f / 180 * 0.5f;
                    float qy[4] = {0, sinf(h2), 0, cosf(h2)}, qx[4] = {sinf(a2), 0, 0, cosf(a2)}, qg[4];
                    qg[0] = qy[3] * qx[0] + qy[0] * qx[3] + qy[1] * qx[2] - qy[2] * qx[1]; qg[1] = qy[3] * qx[1] - qy[0] * qx[2] + qy[1] * qx[3] + qy[2] * qx[0];
                    qg[2] = qy[3] * qx[2] + qy[0] * qx[1] - qy[1] * qx[0] + qy[2] * qx[3]; qg[3] = qy[3] * qx[3] - qy[0] * qx[0] - qy[1] * qx[1] - qy[2] * qx[2];
                    memcpy(g_hands[s].rot, qg, sizeof qg);
                    if (mode == 1 || mode == 3) { g_hands[s].aim_valid = 1; memcpy(g_hands[s].aim_rot, qy, sizeof qy); }
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
                    if (d->sol.flags & (VRARM_WRIST_LIMIT | VRARM_SHOULDER_LIMIT | VRARM_STRETCHED)) continue;
                    float R[9], t[3]; load_xf(part_addr(s ? 4 : 9), R, t);
                    float lx[3] = {R[0], R[3], R[6]}, ly[3] = {R[1], R[4], R[7]}, lz[3] = {R[2], R[5], R[8]};
                    float palm[3], thumb[3];
                    for (int k = 0; k < 3; k++) { palm[k] = (s ? lx[k] : -lx[k]); thumb[k] = lz[k]; }
                    float fl[3] = {-ly[0], -ly[1], -ly[2]};
                    float pw[3] = {-up[0], -up[1], -up[2]}, tm[3] = {s ? -xm[0] : xm[0], s ? -xm[1] : xm[1], s ? -xm[2] : xm[2]};
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
static void test_hand_axes(void) {
    hand_axes_run(0.0f, 0);      /* sin giro: marco de vr_pose.h tal cual */
    hand_axes_run(90.0f, 0);
    hand_axes_run(45.0f, 0);     /* defecto */
    for (int mode = 1; mode <= 3; mode++) { hand_axes_run(45.0f, mode); hand_axes_run(0.0f, mode); }
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


/* API de cabeceo/calibración (pura): relación agarre/apuntado del Sense, calibración, persistencia y entradas inválidas */
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
        CHECK(vrbody_pitch_from_aim(g, a, &deg) && fabsf(deg + 35.0f) < 0.05f, "cabeceo del Sense: %g", deg);
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
    VrBodyConfig cfg; vrbody_config_default(&cfg); cfg.hand_pitch_deg = 0;
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
        VrBodyConfig cfg; vrbody_config_default(&cfg); cfg.hand_pitch_deg = 0; cfg.force_arm_visible = force;
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
    CHECK(tested > 3000 && bad == 0, "simetría izquierda/derecha: %d de %d no son espejo", bad, tested);
}

/* RAM corrupta: no debe caerse ni escribir fuera */
static void test_fuzz(void) {
    VrBodyConfig cfg; vrbody_config_default(&cfg); cfg.hand_pitch_deg = 0;
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
    VrBodyConfig cfg; vrbody_config_default(&cfg); cfg.hand_pitch_deg = 0;
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
    /* desactivado por configuración */
    { VrBodyConfig c2 = cfg; c2.fist_enabled = 0; vrbody_init(&c2); vrbody_set_view_fn(view_fn);
      run_fist_frames(1.0f, 40, &driven); CHECK(mesh_diff(0, nv) == 0 && mesh_diff(1, nv) == 0, "fist_enabled = 0: malla rígida"); vrbody_restore(RAM); }
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

int main(void) {
    test_math();
    test_solver();
    test_solver_nonfinite();
    test_natural_elbow();
    test_continuity();
    test_glue();
    test_hand_axes(); test_hand_pitch_api(); test_default_roll(); test_jab_and_grab();
    test_visibility();
    test_fist_glue();
    test_mirror();
    test_fuzz();
    VrBodyStats st; vrbody_stats(&st);
    printf("  stats: applies %u, brazos %u, restores %u, estirados %u, hombro %u, muñeca %u\n", st.applies, st.arms_driven, st.restores, st.stretched, st.shoulder_limit, st.wrist_limit);
    if (g_fail) { printf("FALLO: %d de %d comprobaciones\n", g_fail, g_checks); return 1; }
    printf("OK: %d comprobaciones\n", g_checks);
    return 0;
}
