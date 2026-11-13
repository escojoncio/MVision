/* Pruebas de vr_hand_pose (poses de la mano nueva a partir de gatillo/agarre/botones): monotonía, autopenetración por pose, zona del gatillo, determinismo, entradas fuera de rango.
 * Sin datos del juego. make -C vr/hands test */
#include "vr_hand_pose.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO l.%d: %s ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *const NAME[HP_COUNT] = {"open", "relax", "fist", "point", "trigger", "grip_pistol", "grip_rifle", "grip_support", "grip_sphere", "grip_placed", "grip_box", "w_socom_r", "w_socom_l", "w_famas_r", "w_famas_l", "w_psg1_r", "w_psg1_l", "w_stinger_r", "w_stinger_l", "w_nikita_r", "w_nikita_l", "w_frag_r", "w_stun_r", "w_chaff_r"};
static const int SIDE[2] = {1, -1};   /* 0 derecha, 1 izquierda */

static int all_finite(const float xf[VRHS_J][12]) {
    for (int k = 0; k < VRHS_J; k++) for (int j = 0; j < 12; j++) if (!isfinite(xf[k][j])) return 0;
    return 1;
}
static VrHandPoseIn in_of(int pose, float grip, float trig, float tb, float fb) { VrHandPoseIn i = {pose, grip, trig, tb, fb}; return i; }

/* Poses con objeto o gesto "destino" desde la mano relajada: todos los cierres crecen con el agarre (HP_OPEN y HP_POINT abren dedos: decrecen). */
static void test_monotonic(void) {
    for (int pose = 0; pose < HP_COUNT; pose++) {
        if (pose == HP_RELAX) continue;
        VrHandCurls prev;
        float sum_prev = -1;
        for (int g = 0; g <= 20; g++) {
            VrHandPoseIn in = in_of(pose, g / 20.0f, 0, 0, 0);
            VrHandCurls c;
            vrhandpose_curls(&in, &c);
            if (g > 0) for (int i = 0; i < 5; i++) {
                float d = c.curl[i] - prev.curl[i];
                int opens = (pose == HP_OPEN) || (pose == HP_POINT && i == 0);   /* destinos con dedos más abiertos que la mano relajada */
                CHECK(opens ? d <= 1e-6f : d >= -1e-6f, "%s: dedo %d no monótono con el agarre en %.2f (%.4f -> %.4f)", NAME[pose], i, g / 20.0f, prev.curl[i], c.curl[i]);
            }
            prev = c;
            (void)sum_prev;
        }
        VrHandPoseIn z = in_of(pose, 0, 0, 0, 0), one = in_of(pose, 1, 0, 0, 0);
        VrHandCurls c0, c1, rel;
        VrHandPoseIn r = in_of(HP_RELAX, 0, 0, 0, 0);
        vrhandpose_curls(&z, &c0); vrhandpose_curls(&one, &c1); vrhandpose_curls(&r, &rel);
        CHECK(memcmp(&c0, &rel, sizeof c0) == 0, "%s: con agarre 0 debe ser la mano relajada", NAME[pose]);
        (void)c1;
    }
    /* gatillo: el cierre del índice crece con el gatillo y la yema retrocede hacia la palma (y del marco de la mano crece) en las poses con gatillo; sin gatillo no se mueve */
    for (int pose = 0; pose < HP_COUNT; pose++) {
        float prev_curl = -1, prev_y = -1e9f;
        for (int t = 0; t <= 20; t++) {
            VrHandPoseIn in = in_of(pose, 1.0f, t / 20.0f, 0, 0);
            VrHandCurls c;
            vrhandpose_curls(&in, &c);
            float xf[VRHS_J][12], tip[3];
            vrhandpose_blend(1, &in, xf);
            vrhandpose_index_tip(xf, tip);
            if (t > 0) {
                if (vrhandpose_has_trigger(pose)) {
                    CHECK(c.curl[0] >= prev_curl - 1e-6f && tip[1] >= prev_y - 1e-3f, "%s: el índice no sigue el gatillo en %.2f (curl %.3f -> %.3f, y %.2f -> %.2f)", NAME[pose], t / 20.0f, prev_curl, c.curl[0], prev_y, tip[1]);
                } else CHECK(c.curl[0] == prev_curl && fabsf(tip[1] - prev_y) < 1e-4f, "%s: sin gatillo el índice se mueve con él (%.3f -> %.3f)", NAME[pose], prev_curl, c.curl[0]);
            }
            prev_curl = c.curl[0]; prev_y = tip[1];
        }
    }
    CHECK(vrhandpose_has_trigger(HP_TRIGGER) && vrhandpose_has_trigger(HP_GRIP_PISTOL) && vrhandpose_has_trigger(HP_GRIP_RIFLE) && !vrhandpose_has_trigger(HP_GRIP_SUPPORT) &&
          !vrhandpose_has_trigger(HP_GRIP_SPHERE) && !vrhandpose_has_trigger(HP_GRIP_PLACED) && !vrhandpose_has_trigger(HP_FIST) && !vrhandpose_has_trigger(-1) && !vrhandpose_has_trigger(HP_COUNT), "has_trigger");
    /* pulgar: el botón lo levanta (cierre y separación monótonos) con o sin objeto */
    for (int pose = 1; pose < HP_COUNT; pose++) {
        VrHandCurls prev;
        for (int b = 0; b <= 10; b++) {
            VrHandPoseIn in = in_of(pose, 1.0f, 0, b / 10.0f, 0);
            VrHandCurls c;
            vrhandpose_curls(&in, &c);
            if (b > 0) CHECK(c.curl[4] <= prev.curl[4] + 1e-6f && c.thumb_spread >= prev.thumb_spread - 1e-6f, "%s: botón del pulgar no monótono en %.1f", NAME[pose], b / 10.0f);
            else for (int i = 0; i < 4; i++) (void)i;
            prev = c;
        }
        VrHandPoseIn a = in_of(pose, 1.0f, 0, 0, 0), bt = in_of(pose, 1.0f, 0, 1, 0);
        VrHandCurls ca, cb;
        vrhandpose_curls(&a, &ca); vrhandpose_curls(&bt, &cb);
        for (int i = 0; i < 4; i++) CHECK(ca.curl[i] == cb.curl[i], "%s: el botón del pulgar mueve el dedo %d", NAME[pose], i);
        CHECK(cb.curl[4] == 0.0f, "%s: pulgar levantado = curl 0", NAME[pose]);
    }
    /* puño: solo sin objeto. Sin objeto (HP_RELAX) el botón de puño cierra todo; con objeto no cambia nada */
    {
        VrHandCurls prev, c;
        for (int b = 0; b <= 10; b++) {
            VrHandPoseIn in = in_of(HP_RELAX, 0, 0, 0, b / 10.0f);
            vrhandpose_curls(&in, &c);
            if (b > 0) for (int i = 0; i < 5; i++) CHECK(c.curl[i] >= prev.curl[i] - 1e-6f, "puño no monótono (dedo %d, botón %.1f)", i, b / 10.0f);
            prev = c;
        }
        VrHandPoseIn f = in_of(HP_RELAX, 0, 0, 0, 1), g1 = in_of(HP_FIST, 1, 0, 0, 0);
        VrHandCurls cf, cg;
        vrhandpose_curls(&f, &cf); vrhandpose_curls(&g1, &cg);
        CHECK(memcmp(&cf, &cg, sizeof cf) == 0, "botón de puño sin objeto = HP_FIST con agarre 1");
        for (int pose = HP_GRIP_PISTOL; pose < HP_COUNT; pose++) {
            VrHandPoseIn a = in_of(pose, 0.6f, 0.3f, 0.2f, 0), bt = in_of(pose, 0.6f, 0.3f, 0.2f, 1);
            VrHandCurls ca, cb;
            vrhandpose_curls(&a, &ca); vrhandpose_curls(&bt, &cb);
            CHECK(memcmp(&ca, &cb, sizeof ca) == 0, "%s: el botón de puño no debe actuar con objeto", NAME[pose]);
        }
    }
}

/* Los umbrales en mm se fijaron con la mano a VRHS_FIT_K = 1,27; escalan con el tamaño de la mano (HS). */
#define HS (VRHS_FIT_K / 1.27f)
/* Sin autopenetración gruesa en ninguna pose: las yemas de dedos vecinos no se funden (centros a > 12 mm) y la punta de cada dedo no entra en la palma (su centro queda por delante
 * de la superficie palmar, x > superficie - 3 mm en la mano derecha) */
static void check_collision(int side, const VrHandPoseIn *in, const char *what) {
    float xf[VRHS_J][12], g[3];
    vrhandpose_blend(SIDE[side], in, xf);
    vrhs_grip_center(SIDE[side], g);
    const float palm_x = SIDE[side] * g[0] - VRHS_HANDLE_R_MM;
    float dmin = 1e9f, worst_in = -1e9f;
    for (int f = 0; f < 3; f++) {
        const float *a = xf[VRHS_INDEX_MC + 5 * f + 3], *b = xf[VRHS_INDEX_MC + 5 * (f + 1) + 3];
        float d = sqrtf((a[3] - b[3]) * (a[3] - b[3]) + (a[7] - b[7]) * (a[7] - b[7]) + (a[11] - b[11]) * (a[11] - b[11]));
        if (d < dmin) dmin = d;
    }
    for (int f = 0; f < 4; f++) {
        const float *t = xf[VRHS_INDEX_MC + 5 * f + 4];
        if (t[7] > -40.0f || t[7] < -110.0f) continue;              /* solo sobre la palma */
        float in_ = palm_x - SIDE[side] * t[3];
        if (in_ > worst_in) worst_in = in_;
    }
    CHECK(dmin > 12.0f * HS && worst_in < 3.0f, "mano %d %s: yemas vecinas a %.1f mm, punta dentro de la palma %.1f mm", side, what, dmin, worst_in);
}
static void test_collision(void) {
    char what[96];
    for (int side = 0; side < 2; side++) for (int pose = 0; pose < HP_COUNT; pose++) {
        for (int g = 0; g <= 10; g++) for (int t = 0; t <= 1; t++) for (int b = 0; b <= 1; b++) {
            VrHandPoseIn in = in_of(pose, g / 10.0f, (float)t, (float)b, 0);
            snprintf(what, sizeof what, "%s agarre %.1f gatillo %d pulgar %d", NAME[pose], g / 10.0, t, b);
            check_collision(side, &in, what);
        }
    }
}

/* Zona del gatillo: con el índice libre (HP_TRIGGER y las poses de arma con el gatillo suelto) la yema queda delante del centro de agarre (-Y), hacia el lado del pulgar (+Z) y por
 * delante de la palma; con el gatillo a fondo retrocede hacia la palma entre 12 y 45 mm. mm respecto al centro de agarre. */
static void test_trigger_zone(void) {
    for (int side = 0; side < 2; side++) {
        const float sg = (float)SIDE[side];
        float pc[3], xf[VRHS_J][12], tip[3], tip1[3];
        vrhs_grip_center(SIDE[side], pc);
        const int poses[3] = {HP_TRIGGER, HP_GRIP_PISTOL, HP_GRIP_RIFLE};
        for (int k = 0; k < 3; k++) {
            VrHandPoseIn in = in_of(poses[k], 1, 0, 0, 0), in1 = in_of(poses[k], 1, 1, 0, 0);
            vrhandpose_blend(SIDE[side], &in, xf); vrhandpose_index_tip(xf, tip);
            vrhandpose_blend(SIDE[side], &in1, xf); vrhandpose_index_tip(xf, tip1);
            float dx = sg * (tip[0] - pc[0]), dy = tip[1] - pc[1], dz = tip[2] - pc[2];
            CHECK(dx > -20.0f * HS && dx < 30.0f * HS && dy < -45.0f * HS && dy > -85.0f * HS && dz > 0.0f && dz < 40.0f * HS, "mano %d %s: yema fuera de la zona del gatillo: (%.1f, %.1f, %.1f) mm", side, NAME[poses[k]], dx, dy, dz);
            float back = tip1[1] - tip[1];
            CHECK(back > 12.0f * HS && back < 45.0f * HS, "mano %d %s: recorrido de la yema con el gatillo %.1f mm (12..45 a la escala 1,27)", side, NAME[poses[k]], back);
        }
    }
}

static void test_deterministic(void) {
    static float sr[VRHA_NV][3], sl[VRHA_NV][3];
    for (int pose = 0; pose < HP_COUNT; pose++) {
        VrHandPoseIn in = in_of(pose, 0.63f, 0.41f, 0.27f, 0.0f);
        float a[VRHS_J][12], b[VRHS_J][12];
        vrhandpose_blend(1, &in, a);
        memset(b, 0x55, sizeof b);
        vrhandpose_blend(1, &in, b);
        CHECK(memcmp(a, b, sizeof a) == 0, "%s: no determinista", NAME[pose]);
        /* la izquierda es el espejo exacto de la derecha */
        float l[VRHS_J][12];
        vrhandpose_blend(-1, &in, l);
        vrhs_skin(a, 1, sr, NULL); vrhs_skin(l, -1, sl, NULL);
        double err = 0;
        for (int i = 0; i < VRHA_NV; i++) {
            double e = fabs(sr[i][0] + sl[i][0]) + fabs(sr[i][1] - sl[i][1]) + fabs(sr[i][2] - sl[i][2]);
            if (e > err) err = e;
        }
        CHECK(err < 1e-3, "%s: la mano izquierda no es el espejo (error %.5f mm)", NAME[pose], err);
    }
}

static void test_robust(void) {
    const float bad[] = {NAN, INFINITY, -INFINITY, -5.0f, 7.0f, 1e30f, -1e30f, 0.5f};
    const int poses[] = {-1, 99, INT_MIN, INT_MAX, HP_COUNT, HP_RELAX, HP_GRIP_PISTOL, HP_FIST};
    for (size_t p = 0; p < sizeof poses / sizeof *poses; p++) for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) for (size_t j = 0; j < sizeof bad / sizeof *bad; j++) {
        VrHandPoseIn in = {poses[p], bad[i], bad[j], bad[(i + j) % 8], bad[(i * 3 + j) % 8]};
        float xf[VRHS_J][12];
        VrHandCurls c;
        vrhandpose_curls(&in, &c);
        int ok = isfinite(c.thumb_spread) && c.thumb_spread >= -1.0f && c.thumb_spread <= 1.0f;
        for (int k = 0; k < 5; k++) ok = ok && isfinite(c.curl[k]) && c.curl[k] >= 0.0f && c.curl[k] <= 1.0f;
        CHECK(ok, "curls fuera de rango con pose %d entradas %g %g", poses[p], bad[i], bad[j]);
        for (int side = 0; side < 2; side++) {
            vrhandpose_blend(SIDE[side], &in, xf);
            CHECK(all_finite(xf), "xf no finito con pose %d entradas %g %g", poses[p], bad[i], bad[j]);
            float tip[3];
            vrhandpose_index_tip(xf, tip);
            CHECK(isfinite(tip[0]) && isfinite(tip[1]) && isfinite(tip[2]), "punta no finita");
        }
    }
    /* se acotan como el valor extremo, NaN = 0, pose inválida = relajada, NULL = relajada */
    float a[VRHS_J][12], b[VRHS_J][12];
    VrHandPoseIn hi = in_of(HP_GRIP_PISTOL, 7, 7, 7, 7), one = in_of(HP_GRIP_PISTOL, 1, 1, 1, 1), nan = in_of(HP_GRIP_PISTOL, NAN, NAN, NAN, NAN), zero = in_of(HP_GRIP_PISTOL, 0, 0, 0, 0);
    vrhandpose_blend(1, &hi, a); vrhandpose_blend(1, &one, b);
    CHECK(memcmp(a, b, sizeof a) == 0, "entradas > 1 deben acotarse a 1");
    vrhandpose_blend(1, &nan, a); vrhandpose_blend(1, &zero, b);
    CHECK(memcmp(a, b, sizeof a) == 0, "NaN debe valer 0");
    VrHandPoseIn badp = in_of(-3, 1, 1, 0, 0), rel = in_of(HP_RELAX, 1, 1, 0, 0);
    vrhandpose_blend(1, &badp, a); vrhandpose_blend(1, &rel, b);
    CHECK(memcmp(a, b, sizeof a) == 0, "pose inválida = sin objeto");
    float r0[VRHS_J][12];
    VrHandPoseIn r = in_of(HP_RELAX, 0, 0, 0, 0);
    vrhandpose_blend(1, NULL, a); vrhandpose_blend(1, &r, r0);
    CHECK(memcmp(a, r0, sizeof a) == 0, "entrada NULL = mano relajada");
}

int main(void) {
    test_monotonic();
    test_collision();
    test_trigger_zone();
    test_deterministic();
    test_robust();
    printf("%s vr_hand_pose: %d comprobaciones, %d fallos\n", fails ? "FALLO" : "OK", checks, fails);
    return fails ? 1 : 0;
}
