/* Pruebas de vr_hand_pose (poses de la mano nueva a partir de gatillo/agarre/botones): monotonía, autopenetración por pose, zona del gatillo, determinismo, entradas fuera de rango.
 * Sin datos del juego. make -C vr/hands test */
#include "vr_hand_pose.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO l.%d: %s ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *const NAME[HP_COUNT] = {"open", "relax", "fist", "point", "trigger", "grip_pistol", "grip_rifle", "grip_support", "grip_sphere", "grip_placed", "grip_box"};
static VrHand H[2];   /* 0 derecha, 1 izquierda */

static int all_finite(const float xf[VRHAND_BONES][12]) {
    for (int k = 0; k < VRHAND_BONES; k++) for (int j = 0; j < 12; j++) if (!isfinite(xf[k][j])) return 0;
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
            float xf[VRHAND_BONES][12], tip[3];
            vrhandpose_blend(&H[0], &in, xf);
            vrhandpose_index_tip(&H[0], xf, tip);
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

/* Autopenetración por pose (cápsulas de vrhand_collision), en los extremos y en el camino: mismos umbrales que el puño de test_hand_mesh.c */
static void check_collision(int side, const VrHandPoseIn *in, const char *what) {
    float xf[VRHAND_BONES][12];
    VrHandCollision c;
    vrhandpose_blend(&H[side], in, xf);
    vrhand_collision(&H[side], xf, &c);
    CHECK(c.finger_finger < 0.5f && c.finger_palm < 3.0f && c.thumb_finger < 4.0f && c.thumb_palm < 3.0f, "mano %d %s: ff %.2f fp %.2f tf %.2f tp %.2f", side, what, c.finger_finger, c.finger_palm, c.thumb_finger, c.thumb_palm);
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
    VrHandPoseIn f = in_of(HP_RELAX, 0, 0, 0, 1);
    check_collision(0, &f, "puño sin objeto"); check_collision(1, &f, "puño sin objeto");
    /* el puño sigue siendo un puño: la yema del corazón llega a la palma y el pulgar queda por fuera */
    float xf[VRHAND_BONES][12];
    VrHandCollision c;
    vrhandpose_blend(&H[0], &f, xf); vrhand_collision(&H[0], xf, &c);
    CHECK(c.tip_palm_gap < 4.0f && c.thumb_over > 0.0f, "puño: hueco %.2f, pulgar fuera %.2f", c.tip_palm_gap, c.thumb_over);
}

/* Zona del gatillo: con el índice libre (HP_TRIGGER, gatillo suelto) la yema queda delante del centro de agarre (-Y), hacia el lado del pulgar (+Z) y por delante de la palma (+X en la derecha);
 * con el gatillo a fondo retrocede hacia la palma. Medidas en mm respecto al centro de agarre (la mano es proporcional a su largo: se pasa a mm con vrhand_unit_m). */
static void test_trigger_zone(void) {
    for (int side = 0; side < 2; side++) {
        const float sg = side ? -1.0f : 1.0f, um = vrhand_unit_m(&H[side]) * 1000.0f;
        float pc[3], xf[VRHAND_BONES][12], tip[3], tip1[3];
        vrhand_palm_center(&H[side], pc);
        VrHandPoseIn rest = in_of(HP_TRIGGER, 1, 0, 0, 0), pulled = in_of(HP_TRIGGER, 1, 1, 0, 0);
        vrhandpose_blend(&H[side], &rest, xf); vrhandpose_index_tip(&H[side], xf, tip);
        vrhandpose_blend(&H[side], &pulled, xf); vrhandpose_index_tip(&H[side], xf, tip1);
        float dx = sg * (tip[0] - pc[0]) * um, dy = (tip[1] - pc[1]) * um, dz = (tip[2] - pc[2]) * um;
        CHECK(dx > 0.0f && dx < 35.0f && dy < -45.0f && dy > -80.0f && dz > 20.0f && dz < 45.0f, "mano %d: yema en HP_TRIGGER fuera de la zona del gatillo: (%.1f, %.1f, %.1f) mm", side, dx, dy, dz);
        float dy1 = (tip1[1] - pc[1]) * um;
        CHECK(dy1 > dy + 30.0f, "mano %d: con el gatillo a fondo la yema debe retroceder >30 mm (%.1f -> %.1f)", side, dy, dy1);
        /* las poses de arma con gatillo (soltado) colocan la yema en la misma zona */
        for (int pose = HP_GRIP_PISTOL; pose <= HP_GRIP_RIFLE; pose++) {
            VrHandPoseIn in = in_of(pose, 1, 0, 0, 0);
            vrhandpose_blend(&H[side], &in, xf); vrhandpose_index_tip(&H[side], xf, tip);
            dx = sg * (tip[0] - pc[0]) * um; dy = (tip[1] - pc[1]) * um; dz = (tip[2] - pc[2]) * um;
            CHECK(dx > 0.0f && dx < 35.0f && dy < -45.0f && dy > -80.0f && dz > 20.0f && dz < 45.0f, "mano %d %s: yema fuera de la zona del gatillo: (%.1f, %.1f, %.1f) mm", side, NAME[pose], dx, dy, dz);
            VrHandPoseIn in1 = in_of(pose, 1, 1, 0, 0);
            vrhandpose_blend(&H[side], &in1, xf); vrhandpose_index_tip(&H[side], xf, tip1);
            float back = ((tip1[1] - pc[1]) * um) - dy;
            CHECK(back > 12.0f && back < 40.0f, "mano %d %s: recorrido de la yema con el gatillo %.1f mm (12..40)", side, NAME[pose], back);
        }
    }
}

static void test_deterministic(void) {
    for (int pose = 0; pose < HP_COUNT; pose++) {
        VrHandPoseIn in = in_of(pose, 0.63f, 0.41f, 0.27f, 0.0f);
        float a[VRHAND_BONES][12], b[VRHAND_BONES][12];
        vrhandpose_blend(&H[0], &in, a);
        memset(b, 0x55, sizeof b);
        vrhandpose_blend(&H[0], &in, b);
        CHECK(memcmp(a, b, sizeof a) == 0, "%s: no determinista", NAME[pose]);
        VrHand h2;
        vrhand_build(&h2, NULL, 1);
        vrhandpose_blend(&h2, &in, b);
        CHECK(memcmp(a, b, sizeof a) == 0, "%s: depende de la instancia de la mano", NAME[pose]);
        /* la izquierda es el espejo exacto de la derecha */
        float l[VRHAND_BONES][12];
        vrhandpose_blend(&H[1], &in, l);
        static float sr[VRHAND_BONES][VRHAND_MAX_V][3], sl[VRHAND_BONES][VRHAND_MAX_V][3];
        vrhand_skin(&H[0], a, sr); vrhand_skin(&H[1], l, sl);
        double err = 0;
        for (int k = 0; k < VRHAND_BONES; k++) for (int i = 0; i < H[0].mesh[k].n_verts; i++) {
            double e = fabs(sr[k][i][0] + sl[k][i][0]) + fabs(sr[k][i][1] - sl[k][i][1]) + fabs(sr[k][i][2] - sl[k][i][2]);
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
        float xf[VRHAND_BONES][12];
        VrHandCurls c;
        vrhandpose_curls(&in, &c);
        int ok = isfinite(c.thumb_spread) && c.thumb_spread >= -1.0f && c.thumb_spread <= 1.0f;
        for (int k = 0; k < 5; k++) ok = ok && isfinite(c.curl[k]) && c.curl[k] >= 0.0f && c.curl[k] <= 1.0f;
        CHECK(ok, "curls fuera de rango con pose %d entradas %g %g", poses[p], bad[i], bad[j]);
        for (int side = 0; side < 2; side++) {
            vrhandpose_blend(&H[side], &in, xf);
            CHECK(all_finite(xf), "xf no finito con pose %d entradas %g %g", poses[p], bad[i], bad[j]);
            float tip[3];
            vrhandpose_index_tip(&H[side], xf, tip);
            CHECK(isfinite(tip[0]) && isfinite(tip[1]) && isfinite(tip[2]), "punta no finita");
            VrHandCollision col;
            vrhand_collision(&H[side], xf, &col);
            CHECK(isfinite(col.finger_finger) && isfinite(col.thumb_finger), "colisión no finita");
        }
    }
    /* se acotan como el valor extremo, NaN = 0, pose inválida = relajada, NULL = relajada */
    float a[VRHAND_BONES][12], b[VRHAND_BONES][12];
    VrHandPoseIn hi = in_of(HP_GRIP_PISTOL, 7, 7, 7, 7), one = in_of(HP_GRIP_PISTOL, 1, 1, 1, 1), nan = in_of(HP_GRIP_PISTOL, NAN, NAN, NAN, NAN), zero = in_of(HP_GRIP_PISTOL, 0, 0, 0, 0);
    vrhandpose_blend(&H[0], &hi, a); vrhandpose_blend(&H[0], &one, b);
    CHECK(memcmp(a, b, sizeof a) == 0, "entradas > 1 deben acotarse a 1");
    vrhandpose_blend(&H[0], &nan, a); vrhandpose_blend(&H[0], &zero, b);
    CHECK(memcmp(a, b, sizeof a) == 0, "NaN debe valer 0");
    VrHandPoseIn badp = in_of(-3, 1, 1, 0, 0), rel = in_of(HP_RELAX, 1, 1, 0, 0);
    vrhandpose_blend(&H[0], &badp, a); vrhandpose_blend(&H[0], &rel, b);
    CHECK(memcmp(a, b, sizeof a) == 0, "pose inválida = sin objeto");
    float r0[VRHAND_BONES][12];
    VrHandPoseIn r = in_of(HP_RELAX, 0, 0, 0, 0);
    vrhandpose_blend(&H[0], NULL, a); vrhandpose_blend(&H[0], &r, r0);
    CHECK(memcmp(a, r0, sizeof a) == 0, "entrada NULL = mano relajada");
}

int main(void) {
    vrhand_build(&H[0], NULL, 1); vrhand_build(&H[1], NULL, -1);
    test_monotonic();
    test_collision();
    test_trigger_zone();
    test_deterministic();
    test_robust();
    printf("%s vr_hand_pose: %d comprobaciones, %d fallos\n", fails ? "FALLO" : "OK", checks, fails);
    return fails ? 1 : 0;
}
