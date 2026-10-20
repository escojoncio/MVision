/* vr_body: IK de brazos con el modelo original del jugador. Ver vr_body.h para el contrato y la semántica del modelo.
 *
 * Estructura del fichero: 1) matemática (vec3, mat3, cuaterniones); 2) solver de dos huesos; 3) estado y configuración;
 * 4) pegamento sobre la RAM (lee world/screen de las piezas, resuelve, escribe, restaura).
 *
 * Por qué este punto del frame y no otro (datos del motor, verificados con el volcado de RAM):
 *  - world de cada pieza = world del padre x [rotación de la animación | t_i] (etapa 1 del dibujado, que escribe world y screen
 *    de TODAS las piezas del objeto; la lógica del juego lee world de piezas del jugador en el fotograma siguiente: cabeza
 *    (altura de la cabeza para focos y enemigos), mano derecha (origen de los efectos del arma), torso (blanco de
 *    explosivos adhesivos)). Por eso las matrices IK se restauran al terminar el dibujado en vez de dejarlas.
 *  - El motor NO recalcula world desde los ángulos de la animación fuera de esa etapa; sustituirlos exigiría rehacer la etapa 1
 *    entera y ensuciaría la interpolación de la animación. Sustituir las matrices ya compuestas es local y
 *    reversible.
 *  - screen = vista x world (GTE, 4096 = 1): se recalcula aquí con la misma matriz de vista que usó el motor. */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "vr_body.h"
#include "vr_fist.h"
#include "vr_hand_mesh.h"
#include "../bridge/bridge_addrs.h"

#define PI_F 3.14159265358979f
#define DEG (PI_F / 180.0f)

/* ===================================================== 1) matemática ============================================== */
static inline void v_set(float *a, float x, float y, float z) { a[0] = x; a[1] = y; a[2] = z; }
static inline void v_cp(float *a, const float *b) { a[0] = b[0]; a[1] = b[1]; a[2] = b[2]; }
static inline void v_add(float *o, const float *a, const float *b) { o[0] = a[0] + b[0]; o[1] = a[1] + b[1]; o[2] = a[2] + b[2]; }
static inline void v_sub(float *o, const float *a, const float *b) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static inline void v_scale(float *o, const float *a, float s) { o[0] = a[0] * s; o[1] = a[1] * s; o[2] = a[2] * s; }
static inline void v_madd(float *o, const float *a, const float *b, float s) { o[0] = a[0] + b[0] * s; o[1] = a[1] + b[1] * s; o[2] = a[2] + b[2] * s; }
static inline float v_dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline void v_cross(float *o, const float *a, const float *b) {
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x; o[1] = y; o[2] = z;
}
static inline float v_len(const float *a) { return sqrtf(v_dot(a, a)); }
/* Normaliza; devuelve la longitud original. Si es ~0 deja `fallback` (si no es NULL) y devuelve 0. */
static float v_norm(float *o, const float *a, const float *fallback) {
    float n = v_len(a);
    if (n > 1e-9f) { o[0] = a[0] / n; o[1] = a[1] / n; o[2] = a[2] / n; return n; }
    if (fallback) v_cp(o, fallback); else v_set(o, 0, 0, 0);
    return 0;
}
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int all_finite(const float *a, int n) { for (int i = 0; i < n; i++) if (!isfinite(a[i])) return 0; return 1; }

/* M (fila-mayor) x v */
static void m_mulv(float *o, const float *m, const float *v) {
    float x = m[0] * v[0] + m[1] * v[1] + m[2] * v[2], y = m[3] * v[0] + m[4] * v[1] + m[5] * v[2], z = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
    o[0] = x; o[1] = y; o[2] = z;
}
static void m_mulm(float *o, const float *a, const float *b) {
    float r[9];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r[3 * i + j] = a[3 * i] * b[j] + a[3 * i + 1] * b[3 + j] + a[3 * i + 2] * b[6 + j];
    memcpy(o, r, sizeof r);
}
static void m_transpose(float *o, const float *a) {
    float r[9] = {a[0], a[3], a[6], a[1], a[4], a[7], a[2], a[5], a[8]};
    memcpy(o, r, sizeof r);
}
static void m_ident(float *m) { memset(m, 0, 9 * sizeof *m); m[0] = m[4] = m[8] = 1; }
static void m_col(float *c, const float *m, int j) { c[0] = m[j]; c[1] = m[3 + j]; c[2] = m[6 + j]; }
static void m_setcol(float *m, int j, const float *c) { m[j] = c[0]; m[3 + j] = c[1]; m[6 + j] = c[2]; }
/* Rotación de `ang` rad alrededor del eje unitario k (Rodrigues) */
static void m_axis_angle(float *m, const float *k, float ang) {
    float c = cosf(ang), s = sinf(ang), t = 1 - c;
    m[0] = t * k[0] * k[0] + c;        m[1] = t * k[0] * k[1] - s * k[2]; m[2] = t * k[0] * k[2] + s * k[1];
    m[3] = t * k[0] * k[1] + s * k[2]; m[4] = t * k[1] * k[1] + c;        m[5] = t * k[1] * k[2] - s * k[0];
    m[6] = t * k[0] * k[2] - s * k[1]; m[7] = t * k[1] * k[2] + s * k[0]; m[8] = t * k[2] * k[2] + c;
}

void vrbody_orthonormalize(const float m[9], float out[9]) {
    float x[3], y[3], z[3], t[3];
    m_col(x, m, 0); m_col(y, m, 1);
    float ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0};
    if (!all_finite(m, 9)) { m_ident(out); return; }
    v_norm(x, x, ex);
    v_madd(t, y, x, -v_dot(y, x));
    if (v_norm(y, t, NULL) < 1e-6f) {                 /* y degenerado: cualquier perpendicular a x */
        float a[3] = {0, 1, 0}; if (fabsf(x[1]) > 0.9f) v_set(a, 1, 0, 0);
        v_madd(t, a, x, -v_dot(a, x)); v_norm(y, t, ey);
    }
    v_cross(z, x, y);
    m_setcol(out, 0, x); m_setcol(out, 1, y); m_setcol(out, 2, z);
}

void vrbody_quat_to_mat(const float q[4], float m[9]) {
    float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!all_finite(q, 4) || n < 1e-6f) { m_ident(m); return; }
    float x = q[0] / n, y = q[1] / n, z = q[2] / n, w = q[3] / n;
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - z * w);     m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w);     m[7] = 2 * (y * z + x * w);     m[8] = 1 - 2 * (x * x + y * y);
}
void vrbody_mat_to_quat(const float mm[9], float q[4]) {
    float m[9]; vrbody_orthonormalize(mm, m);
    float tr = m[0] + m[4] + m[8];
    if (tr > 0) {
        float s = sqrtf(tr + 1) * 2;
        q[3] = 0.25f * s; q[0] = (m[7] - m[5]) / s; q[1] = (m[2] - m[6]) / s; q[2] = (m[3] - m[1]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        float s = sqrtf(1 + m[0] - m[4] - m[8]) * 2;
        q[3] = (m[7] - m[5]) / s; q[0] = 0.25f * s; q[1] = (m[1] + m[3]) / s; q[2] = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        float s = sqrtf(1 + m[4] - m[0] - m[8]) * 2;
        q[3] = (m[2] - m[6]) / s; q[0] = (m[1] + m[3]) / s; q[1] = 0.25f * s; q[2] = (m[5] + m[7]) / s;
    } else {
        float s = sqrtf(1 + m[8] - m[0] - m[4]) * 2;
        q[3] = (m[3] - m[1]) / s; q[0] = (m[2] + m[6]) / s; q[1] = (m[5] + m[7]) / s; q[2] = 0.25f * s;
    }
    float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n < 1e-9f) { q[0] = q[1] = q[2] = 0; q[3] = 1; return; }
    for (int i = 0; i < 4; i++) q[i] /= n;
}
static void q_mul(float *o, const float *a, const float *b) {   /* a * b */
    float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
          z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}
static void q_conj(float *o, const float *a) { o[0] = -a[0]; o[1] = -a[1]; o[2] = -a[2]; o[3] = a[3]; }
static void q_normalize(float *q) {
    float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n < 1e-9f || !isfinite(n)) { q[0] = q[1] = q[2] = 0; q[3] = 1; return; }
    for (int i = 0; i < 4; i++) q[i] /= n;
}
static void q_slerp(float *o, const float *a, const float *b0, float t) {
    float b[4] = {b0[0], b0[1], b0[2], b0[3]};
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0) { d = -d; for (int i = 0; i < 4; i++) b[i] = -b[i]; }
    float ka, kb;
    if (d > 0.9995f) { ka = 1 - t; kb = t; }
    else { float th = acosf(d), s = sinf(th); ka = sinf((1 - t) * th) / s; kb = sinf(t * th) / s; }
    for (int i = 0; i < 4; i++) o[i] = ka * a[i] + kb * b[i];
    q_normalize(o);
}
/* Ángulo (rad, 0..pi) de un cuaternión */
static float q_angle(const float *q) { return 2 * acosf(clampf(fabsf(q[3]), 0, 1)); }
/* Descompone q = swing * twist con `twist` giro alrededor del eje unitario `ax` (en el marco de partida). */
static void q_swing_twist(const float *q0, const float *ax, float *swing, float *twist) {
    float q[4] = {q0[0], q0[1], q0[2], q0[3]};
    if (q[3] < 0) for (int i = 0; i < 4; i++) q[i] = -q[i];
    float p = q[0] * ax[0] + q[1] * ax[1] + q[2] * ax[2];
    twist[0] = ax[0] * p; twist[1] = ax[1] * p; twist[2] = ax[2] * p; twist[3] = q[3];
    float n = sqrtf(p * p + q[3] * q[3]);
    if (n < 1e-9f) { twist[0] = twist[1] = twist[2] = 0; twist[3] = 1; }
    else for (int i = 0; i < 4; i++) twist[i] /= n;
    float ct[4]; q_conj(ct, twist);
    q_mul(swing, q, ct);
    q_normalize(swing);
}
/* Ángulo con signo (rad, -pi..pi) del twist respecto a `ax` */
static float twist_angle(const float *tw, const float *ax) {
    float s = tw[0] * ax[0] + tw[1] * ax[1] + tw[2] * ax[2];
    return 2 * atan2f(s, tw[3]);
}

/* ======================================================= 2) solver ================================================ */
void vrbody_limits_default(VrArmLimits *l) {
    l->elbow_min_deg = 32; l->reach_margin = 0.999f; l->shoulder_back = -0.80f; l->shoulder_cross = -0.85f;
    l->wrist_swing_deg = 70; l->forearm_share = 0.6f; l->forearm_twist_deg = 85; l->hand_twist_deg = 70;
}

/* Marco de un hueso: R * offset_local = largo * w (w unitario), con el eje x local llevado (lo más cerca posible) a `hint`.
 * R = sum w_i l_i^T con bases ortonormales (l0 = offset, l1 = x local perpendicular, l2 = l0 x l1) y (w, hint perp., w x hint). */
static void bone_frame(float *R, const float *off_local, const float *w, const float *hint, const float *hint_fallback) {
    float l0[3], l1[3], l2[3], t[3];
    float ex[3] = {1, 0, 0}, ez[3] = {0, 0, 1};
    v_norm(l0, off_local, ex);
    v_madd(t, ex, l0, -v_dot(ex, l0));
    if (v_norm(l1, t, NULL) < 0.2f) { v_madd(t, ez, l0, -v_dot(ez, l0)); v_norm(l1, t, ex); }
    v_cross(l2, l0, l1);
    float w1[3], w2[3];
    v_madd(t, hint, w, -v_dot(hint, w));
    if (v_norm(w1, t, NULL) < 1e-4f) { v_madd(t, hint_fallback, w, -v_dot(hint_fallback, w)); if (v_norm(w1, t, NULL) < 1e-4f) { float a[3] = {0, 1, 0}; if (fabsf(w[1]) > 0.9f) v_set(a, 1, 0, 0); v_madd(t, a, w, -v_dot(a, w)); v_norm(w1, t, ex); } }
    v_cross(w2, w, w1);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) R[3 * i + j] = w[i] * l0[j] + w1[i] * l1[j] + w2[i] * l2[j];
}

int vrbody_solve_arm(const VrArmRig *rig, const VrArmLimits *lim, const VrArmIn *in, VrArmOut *out) {
    if (!rig || !lim || !in || !out) return -1;
    if (!isfinite(lim->elbow_min_deg) || !isfinite(lim->reach_margin) || !isfinite(lim->shoulder_back) || !isfinite(lim->shoulder_cross) ||
        !isfinite(lim->wrist_swing_deg) || !isfinite(lim->forearm_share) || !isfinite(lim->forearm_twist_deg) || !isfinite(lim->hand_twist_deg))
        return -1;
    if (!all_finite(in->shoulder, 3) || !all_finite(in->torso, 9) || !all_finite(in->wrist, 3) || !all_finite(in->hand, 9) || !all_finite(in->pole, 3) ||
        !all_finite(rig->elbow_off, 3) || !all_finite(rig->wrist_off, 3) || !isfinite(rig->side))
        return -1;
    const float L1 = v_len(rig->elbow_off), L2 = v_len(rig->wrist_off);
    if (L1 < 10.0f || L2 < 10.0f || L1 > 5000.0f || L2 > 5000.0f) return -1;
    const float side = rig->side >= 0 ? 1.0f : -1.0f;
    float torso[9]; vrbody_orthonormalize(in->torso, torso);
    float TX[3], TY[3], TZ[3];
    m_col(TX, torso, 0); m_col(TY, torso, 1); m_col(TZ, torso, 2);
    unsigned flags = 0;

    const float emin = clampf(lim->elbow_min_deg, 5.0f, 120.0f) * DEG;
    const float dmin = sqrtf(fmaxf(L1 * L1 + L2 * L2 - 2 * L1 * L2 * cosf(emin), 1.0f));
    const float dmax = (L1 + L2) * clampf(lim->reach_margin, 0.5f, 1.0f);

    float S[3]; v_cp(S, in->shoulder);
    float v[3]; v_sub(v, in->wrist, S);
    float d = v_len(v);
    float down[3]; v_scale(down, TY, -1.0f);
    float vh[3]; v_norm(vh, v, down);
    float dd = d < dmin ? dmin : (d > dmax ? dmax : d);
    if (d > dmax) flags |= VRARM_STRETCHED;
    if (d < dmin) flags |= VRARM_TOO_CLOSE;
    float W[3]; v_madd(W, S, vh, dd);

    /* vector polar: perpendicular al brazo; si es paralelo, alternativas del marco del torso */
    float pw[3]; v_norm(pw, in->pole, down);
    float pp[3], t[3];
    v_madd(t, pw, vh, -v_dot(pw, vh));
    if (v_norm(pp, t, NULL) < 0.05f) {
        flags |= VRARM_POLE_FALLBACK;
        float alt[3][3];
        v_scale(alt[0], TX, side); v_scale(alt[1], TZ, -1.0f); v_cp(alt[2], TY);
        int ok = 0;
        for (int k = 0; k < 3 && !ok; k++) { v_madd(t, alt[k], vh, -v_dot(alt[k], vh)); if (v_norm(pp, t, NULL) > 0.2f) ok = 1; }
        if (!ok) { float a[3] = {1, 0, 0}; if (fabsf(vh[0]) > 0.9f) v_set(a, 0, 1, 0); v_madd(t, a, vh, -v_dot(a, vh)); v_norm(pp, t, a); }
    }

    /* codo: ley de los cosenos en el plano (vh, pp) */
    float cosA = clampf((L1 * L1 + dd * dd - L2 * L2) / (2 * L1 * dd), -1.0f, 1.0f);
    float sinA = sqrtf(fmaxf(0.0f, 1 - cosA * cosA));
    float E[3];
    v_madd(E, S, vh, L1 * cosA); v_madd(E, E, pp, L1 * sinA);
    float u[3]; v_sub(u, E, S); v_scale(u, u, 1.0f / L1);

    /* límites del hombro (marco del torso) */
    {
        float c[3] = {v_dot(u, TX) * side, v_dot(u, TY), v_dot(u, TZ)};   /* [0] hacia fuera, [1] arriba, [2] delante */
        float c0[3] = {c[0], c[1], c[2]};
        for (int pass = 0; pass < 2; pass++) {
            int ax_i[2] = {2, 0};
            float lo[2] = {clampf(lim->shoulder_back, -1.0f, 1.0f), clampf(lim->shoulder_cross, -1.0f, 1.0f)};
            for (int k = 0; k < 2; k++) {
                int a = ax_i[k];
                if (c[a] < lo[k]) {
                    c[a] = lo[k];
                    int o1 = (a + 1) % 3, o2 = (a + 2) % 3;
                    float rem = c[o1] * c[o1] + c[o2] * c[o2], want = fmaxf(0.0f, 1 - c[a] * c[a]);
                    if (rem > 1e-9f) { float s = sqrtf(want / rem); c[o1] *= s; c[o2] *= s; } else { c[o1] = 0; c[o2] = 0; c[a] = c[a] < 0 ? -1.0f : 1.0f; }
                }
            }
        }
        if (fabsf(c[0] - c0[0]) + fabsf(c[1] - c0[1]) + fabsf(c[2] - c0[2]) > 1e-4f) {
            flags |= VRARM_SHOULDER_LIMIT;
            float un[3] = {0, 0, 0};
            v_madd(un, un, TX, c[0] * side); v_madd(un, un, TY, c[1]); v_madd(un, un, TZ, c[2]);
            v_norm(u, un, u);
            v_madd(E, S, u, L1);
        }
    }

    /* antebrazo hacia la muñeca alcanzable (largo fijo) */
    float f[3]; v_sub(f, W, E);
    {
        float fb[3]; v_scale(fb, u, -1.0f);
        if (v_norm(f, f, fb) < 1e-6f) v_cp(f, fb);
        /* flexión máxima del codo: el ángulo interior entre -u y f no baja de emin */
        float mu[3]; v_scale(mu, u, -1.0f);
        float ci = clampf(v_dot(mu, f), -1.0f, 1.0f);
        if (acosf(ci) < emin - 1e-4f) {
            float perp[3]; v_madd(perp, f, mu, -ci);
            if (v_norm(perp, perp, NULL) < 1e-6f) { v_cp(perp, pp); v_madd(perp, perp, mu, -v_dot(perp, mu)); v_norm(perp, perp, pp); }
            v_scale(f, mu, cosf(emin)); v_madd(f, f, perp, sinf(emin));
            v_norm(f, f, mu);
            flags |= VRARM_SHOULDER_LIMIT;
        }
    }
    float W2[3]; v_madd(W2, E, f, L2);

    /* bisagra del codo: eje x local del brazo = v x pp (plano polar); se proyecta a cada hueso */
    float h[3]; v_cross(h, vh, pp);
    if (v_norm(h, h, NULL) < 1e-6f) v_scale(h, TX, side);
    float R_up[9], R_fa[9];
    float hx[3]; v_scale(hx, TX, side);
    bone_frame(R_up, rig->elbow_off, u, h, hx);
    bone_frame(R_fa, rig->wrist_off, f, h, hx);

    /* mano: giro (pronación) repartido entre antebrazo y mano; desvío (swing) limitado */
    float ax[3]; v_norm(ax, rig->wrist_off, (float[3]){0, -1, 0});
    float qf[4], qt[4], qrel[4], sw[4], tw[4];
    float hand_t[9]; vrbody_orthonormalize(in->hand, hand_t);
    vrbody_mat_to_quat(R_fa, qf);
    vrbody_mat_to_quat(hand_t, qt);
    float cqf[4]; q_conj(cqf, qf);
    q_mul(qrel, cqf, qt);
    q_swing_twist(qrel, ax, sw, tw);
    float tau = twist_angle(tw, ax);
    float tau_f = clampf(clampf(lim->forearm_share, 0.0f, 1.0f) * tau, -lim->forearm_twist_deg * DEG, lim->forearm_twist_deg * DEG);
    if (fabsf(tau_f) > 1e-6f) { float Rt[9]; m_axis_angle(Rt, f, tau_f); m_mulm(R_fa, Rt, R_fa); }
    vrbody_mat_to_quat(R_fa, qf);
    q_conj(cqf, qf);
    q_mul(qrel, cqf, qt);
    q_swing_twist(qrel, ax, sw, tw);
    float tau_h = twist_angle(tw, ax);
    float tau_hc = clampf(tau_h, -lim->hand_twist_deg * DEG, lim->hand_twist_deg * DEG);
    float sa = q_angle(sw), smax = clampf(lim->wrist_swing_deg, 1.0f, 170.0f) * DEG;
    if (sa > smax) {
        float idq[4] = {0, 0, 0, 1}, sw2[4];
        q_slerp(sw2, idq, sw, smax / sa);
        memcpy(sw, sw2, sizeof sw);
        flags |= VRARM_WRIST_LIMIT;
    }
    if (tau_hc != tau_h) flags |= VRARM_WRIST_LIMIT;
    {
        float twc[4] = {ax[0] * sinf(tau_hc * 0.5f), ax[1] * sinf(tau_hc * 0.5f), ax[2] * sinf(tau_hc * 0.5f), cosf(tau_hc * 0.5f)};
        float qh[4], Mh[9];
        q_mul(qh, sw, twc); q_normalize(qh);
        vrbody_quat_to_mat(qh, Mh);
        m_mulm(out->R_hand, R_fa, Mh);
    }
    memcpy(out->R_upper, R_up, sizeof R_up);
    memcpy(out->R_fore, R_fa, sizeof R_fa);
    v_cp(out->elbow, E); v_cp(out->wrist, W2);
    out->reach = d / (L1 + L2);
    float dw[3]; v_sub(dw, W2, in->wrist);
    out->err_mm = v_len(dw);
    out->flags = flags;
    return 0;
}

/* ============================================ 3) estado y configuración ========================================== */
typedef struct {
    int init;              /* filtro inicializado (con una pose válida) */
    int was_valid;
    float pos[3], q[4];    /* pose filtrada (espacio del usuario) */
    float aq[4]; int aim_ok; /* apuntado filtrado (si el mando lo entrega) */
    float pitch_s; int pitch_init, pitch_miss;   /* cabeceo automático suavizado, y fotogramas seguidos sin valor válido */
    float w;               /* peso de fundido 0..1 */
    float grip_set;        /* agarre pedido por la aplicación */
    float grip_cur;        /* agarre aplicado (suavizado) */
    float att;             /* 0..1: mano pegada al cuello del enemigo (vrbody_set_grab) */
    float jab_t, jab_dir[3]; /* golpe sintético: tiempo transcurrido (< 0 = inactivo) y dirección en el mundo */
} Hand;

typedef struct {
    uint8_t used;          /* 1 si la ranura guarda algo */
    uint32_t addr;         /* dirección de la pieza */
    uint32_t model;        /* model* (identidad de la pieza al guardar) */
    uint8_t raw[0x40];     /* world + screen originales */
    int16_t vis_old;       /* contador de visibilidad (+0x4C) que dejó el motor */
    int vis_forced;        /* 1 si se forzó (estaba a 0: el motor daba la pieza por fuera de pantalla según la pose ANIMADA) */
} Saved;

/* copia de la malla de mano (vértices + normales) que sustituyó el puño, hasta vrbody_restore */
typedef struct {
    uint8_t used;
    uint32_t model, vaddr, naddr;      /* model* (identidad de la pieza) y direcciones de los arrays */
    int nv, nn;
    int16_t v[VRFIST_MAX_V * 4], n[VRFIST_MAX_V * 4];
} FistSave;

/* Calibración compartida entre el hilo de la app (set/get/clear) y el de emulación (apply/calibrate): bloqueo de giro muy corto + instantánea por fotograma. */
static volatile int g_cal_lock;
static void cal_lock(void) { while (__atomic_exchange_n(&g_cal_lock, 1, __ATOMIC_ACQUIRE)) { } }
static void cal_unlock(void) { __atomic_store_n(&g_cal_lock, 0, __ATOMIC_RELEASE); }

static struct {
    VrBodyConfig cfg;
    VrBodyViewFn view_fn;
    VrBodyHook hook;
    Hand hand[2];
    int armed;
    VrBodyStats st;
    VrBodyGrab grab; int grab_age;         /* petición vigente y apply sin refrescarla */
    VrBodyDebug dbg[2];
    /* vista del último apply (para volver del mundo del motor al espacio del usuario en vrbody_forearm_pose) */
    int view_ok;
    float view_eye[3], view_pref[3], view_T[9], view_upm;
    struct { int ok; float q[4]; } cal[2];   /* calibración de manos (sobrevive a vrbody_init) */
    float fore_q[2][4];                /* orientación del marco de agarre (espacio del usuario, sin hand_rot_offset) del último apply */
    float dbg_frame[2][16];            /* marco efectivo de la mano tras la IK (espacio del usuario, m; ver vrbody_debug_hand_frame) */
    int dbg_frame_ok[2];
    /* copia de lo escrito en el último apply, hasta vrbody_restore */
    int pending;
    int pend_n;
    Saved sv[6];
    FistSave fs[2];
    struct { int ok; uint32_t model; float side, grip; float R[9], t[3]; } pf[2];     /* mano y agarre del último apply, para vrbody_prefist; R,t = marco de mundo de la pieza mano tras la IK */
} G;

/* Giro de agarre -> mano alrededor del eje de los dedos (z local de la mano): izquierda +deg, derecha -deg (ver VrBodyConfig.hand_roll_deg). */
static void roll_offset(float deg, int side, float q[4]) {
    float a = (side == 0 ? 1.0f : -1.0f) * clampf(isfinite(deg) ? deg : 0.0f, -180.0f, 180.0f) * DEG * 0.5f;
    q[0] = 0; q[1] = 0; q[2] = sinf(a); q[3] = cosf(a);
}
void vrbody_set_hand_roll(float deg) {
    if (!isfinite(deg)) return;
    G.cfg.hand_roll_deg = clampf(deg, -180.0f, 180.0f);
    for (int s = 0; s < 2; s++) roll_offset(G.cfg.hand_roll_deg, s, G.cfg.hand_rot_offset[s]);
}


/* Cabeceo: giro de `deg` grados sobre el eje X (lateral) del agarre; positivo = el eje de los dedos (-Z) sube hacia +Y. */
static void pitch_quat(float deg, float q[4]) {
    float a = clampf(isfinite(deg) ? deg : 0.0f, -89.0f, 89.0f) * DEG * 0.5f;
    q[0] = sinf(a); q[1] = 0; q[2] = 0; q[3] = cosf(a);
}
static void q_rotv(float *o, const float *q, const float *v) { float m[9]; vrbody_quat_to_mat(q, m); m_mulv(o, m, v); }
void vrbody_set_hand_pitch(float deg) { if (isfinite(deg)) G.cfg.hand_pitch_deg = clampf(deg, -89.0f, 89.0f); }
void vrbody_set_hand_pitch_auto(int on) { G.cfg.hand_pitch_auto = on != 0; }
int vrbody_pitch_from_aim(const float grip_q[4], const float aim_q[4], float *deg) {
    if (!grip_q || !aim_q || !deg || !all_finite(grip_q, 4) || !all_finite(aim_q, 4)) return 0;
    float g[4] = {grip_q[0], grip_q[1], grip_q[2], grip_q[3]}, a[4] = {aim_q[0], aim_q[1], aim_q[2], aim_q[3]}, gc[4], rel[4];
    q_normalize(g); q_normalize(a); q_conj(gc, g); q_mul(rel, gc, a);
    float fwd[3] = {0, 0, -1}, v[3]; q_rotv(v, rel, fwd);              /* el apuntado en el marco del agarre */
    if (!(-v[2] > 0.2f)) return 0;                                    /* apuntado casi perpendicular o contrario a los dedos: inservible */
    float p = atan2f(v[1], -v[2]) / DEG;
    if (!(fabsf(p) <= 80.0f)) return 0;
    *deg = p; return 1;
}
int vrbody_calib_offset(const float grip_q[4], float roll_signed_deg, float out_q[4]) {
    if (!grip_q || !out_q || !all_finite(grip_q, 4) || !isfinite(roll_signed_deg)) return 0;
    float g[4] = {grip_q[0], grip_q[1], grip_q[2], grip_q[3]}; q_normalize(g);
    float G3[9]; vrbody_quat_to_mat(g, G3);
    float f[3] = {-G3[2], -G3[5], -G3[8]};                            /* hacia donde apuntan los dedos del agarre */
    float fh[3] = {f[0], 0, f[2]}; float n = v_len(fh);
    if (!(n > 0.3f)) return 0;                                        /* dedos casi verticales: no hay "delante" */
    v_scale(fh, fh, 1.0f / n);
    float zh[3] = {-fh[0], 0, -fh[2]}, xh[3] = {zh[2], 0, -zh[0]}, yh[3] = {0, 1, 0};
    float H0[9], Rz[9], H[9], Gt[9], O[9];
    m_setcol(H0, 0, xh); m_setcol(H0, 1, yh); m_setcol(H0, 2, zh);
    float a = clampf(roll_signed_deg, -180.0f, 180.0f) * DEG, c_ = cosf(a), s_ = sinf(a);
    float rz[9] = {c_, -s_, 0, s_, c_, 0, 0, 0, 1}; memcpy(Rz, rz, sizeof Rz);
    m_mulm(H, H0, Rz);
    m_transpose(Gt, G3); m_mulm(O, Gt, H);
    vrbody_mat_to_quat(O, out_q);
    return 1;
}
int vrbody_calibrate_hands(void) {
    int n = 0;
    for (int s = 0; s < 2; s++) {
        Hand *h = &G.hand[s];
        if (!h->init || !h->was_valid) continue;
        float q[4];
        if (vrbody_calib_offset(h->q, (s == 0 ? 1.0f : -1.0f) * G.cfg.hand_roll_deg, q)) { cal_lock(); memcpy(G.cal[s].q, q, sizeof q); G.cal[s].ok = 1; cal_unlock(); n++; }
    }
    return n;
}
void vrbody_clear_calibration(void) { cal_lock(); G.cal[0].ok = G.cal[1].ok = 0; cal_unlock(); }
int vrbody_get_calibration(int side, float q[4]) {
    if ((side != 0 && side != 1) || !q) return 0;
    cal_lock(); int ok = G.cal[side].ok; if (ok) memcpy(q, G.cal[side].q, sizeof G.cal[side].q); cal_unlock();
    return ok;
}
void vrbody_set_calibration(int side, const float q[4]) {
    if ((side != 0 && side != 1) || !q || !all_finite(q, 4)) return;
    float t[4] = {q[0], q[1], q[2], q[3]};
    float n2 = t[0] * t[0] + t[1] * t[1] + t[2] * t[2] + t[3] * t[3];
    if (!(n2 >= 0.25f && n2 <= 4.0f)) return;               /* norma fuera de 0,5..2: dato corrupto, no es un cuaternión de rotación */
    q_normalize(t); cal_lock(); memcpy(G.cal[side].q, t, sizeof t); G.cal[side].ok = 1; cal_unlock();
}

void vrbody_config_default(VrBodyConfig *c) {
    memset(c, 0, sizeof *c);
    c->enabled = 1; c->dominant = 1;
    c->part_arm[0][0] = 7; c->part_arm[0][1] = 8; c->part_arm[0][2] = 9;
    c->part_arm[1][0] = 2; c->part_arm[1][1] = 3; c->part_arm[1][2] = 4;
    c->part_torso = 1; c->min_parts = 16; c->force_arm_visible = 1;
    c->grip_to_wrist_mm = 55.0f;
    c->hand_roll_deg = 45.0f;
    for (int s = 0; s < 2; s++) roll_offset(c->hand_roll_deg, s, c->hand_rot_offset[s]);
    c->hand_pitch_deg = -35.0f; c->hand_pitch_auto = 1;
    c->curl_deg = 0.0f; c->armed_grip = 0.85f;
    c->fist_enabled = 1; c->fist_min_grip = 0.03f; vrfist_params_default(&c->fist);
    c->pole[0] = 0.45f; c->pole[1] = -1.0f; c->pole[2] = -0.35f;
    vrbody_limits_default(&c->limits);
    c->jab_s = 0.28f; c->jab_reach_mm = 450.0f; c->grab_blend_s = 0.2f; c->grab_spread_mm = 75.0f; c->grab_roll_deg = 60.0f; c->grab_max_dist_mm = 1100.0f;
    c->smooth_tau_s = 0.02f; c->frame_dt_s = 0.02f; c->blend_s = 0.25f; c->max_hand_jump_m = 3.0f;
}
void vrbody_init(const VrBodyConfig *c) {
    VrBodyViewFn fn = G.view_fn; VrBodyHook hk = G.hook;
    typeof(G.cal) cal; cal_lock(); memcpy(cal, G.cal, sizeof cal); cal_unlock();
    memset(&G, 0, sizeof G);
    G.view_fn = fn; G.hook = hk; cal_lock(); memcpy(G.cal, cal, sizeof cal); cal_unlock();
    if (c) G.cfg = *c; else vrbody_config_default(&G.cfg);
    if (!(G.cfg.frame_dt_s > 1e-4f && G.cfg.frame_dt_s < 1.0f)) G.cfg.frame_dt_s = 0.02f;
    if (!(G.cfg.blend_s >= 0)) G.cfg.blend_s = 0.25f;
    if (!(G.cfg.smooth_tau_s >= 0)) G.cfg.smooth_tau_s = 0;
    if (!(G.cfg.grip_to_wrist_mm >= 0 && G.cfg.grip_to_wrist_mm < 400)) G.cfg.grip_to_wrist_mm = 55.0f;
    for (int s = 0; s < 2; s++) { G.hand[s].grip_set = 0; G.hand[s].grip_cur = 0; G.hand[s].jab_t = -1.0f; }
}
const VrBodyConfig *vrbody_config(void) { return &G.cfg; }
void vrbody_set_view_fn(VrBodyViewFn fn) { G.view_fn = fn; }
void vrbody_set_apply_hook(VrBodyHook fn) { G.hook = fn; }
void vrbody_set_fist_enabled(int on) { G.cfg.fist_enabled = on ? 1 : 0; }

int vrbody_hand_state(int side, float *grip, float R[9], float t[3]) {
    if (side < 0 || side > 1 || !G.pf[side].ok) return 0;
    if (grip) *grip = G.pf[side].grip;
    if (R) memcpy(R, G.pf[side].R, sizeof G.pf[side].R);
    if (t) memcpy(t, G.pf[side].t, sizeof G.pf[side].t);
    return 1;
}

/* Punto de golpe de la mano nueva (vr_hand_mesh): yemas con la mano abierta, nudillos con el puño, en el MUNDO del motor (mm). Mano de dimensiones por defecto (la forma
 * nominal de la mano del personaje); solo se usa para el ataque cuerpo a cuerpo, donde la diferencia de milímetros no importa. */
int vrbody_fist_point(int side, float out[3]) {
    static VrHand hand[2];
    static int built[2];
    if (side < 0 || side > 1 || !out || !G.pf[side].ok) return 0;
    if (!built[side]) { if (vrhand_build(&hand[side], NULL, side == 1 ? 1 : -1) != 0) return 0; built[side] = 1; }
    float xf[VRHAND_BONES][12], p[3];
    vrhand_pose(&hand[side], G.pf[side].grip, xf);
    vrhand_fist_point(&hand[side], xf, p);
    const float *R = G.pf[side].R, *t = G.pf[side].t;
    for (int i = 0; i < 3; i++) out[i] = R[3 * i] * p[0] + R[3 * i + 1] * p[1] + R[3 * i + 2] * p[2] + t[i];
    return all_finite(out, 3);
}

void vrbody_set_grip(int side, float v) { if ((side == 0 || side == 1) && isfinite(v)) G.hand[side].grip_set = clampf(v, 0.0f, 1.0f); }
void vrbody_set_dominant(int side) { G.cfg.dominant = side ? 1 : 0; }
void vrbody_set_armed(int on) { G.armed = on ? 1 : 0; }
void vrbody_set_enabled(int on) { G.cfg.enabled = on ? 1 : 0; }
void vrbody_stats(VrBodyStats *s) { *s = G.st; }
int vrbody_pending(void) { return G.pending; }
void vrbody_set_jab(int side, const float dir[3]) {
    if (side < 0 || side > 1 || !dir || !all_finite(dir, 3)) return;
    Hand *h = &G.hand[side];
    float d[3] = {dir[0], 0, dir[2]};
    if (v_norm(d, d, NULL) < 1e-3f) return;
    v_cp(h->jab_dir, d); h->jab_t = 0.0f;
}
void vrbody_set_grab(const VrBodyGrab *g) {
    if (!g || !g->active || !all_finite(g->neck, 3) || !all_finite(g->left, 3)) { G.grab.active = 0; return; }
    G.grab = *g; G.grab.active = 1; G.grab_age = 0;
}
const VrBodyDebug *vrbody_debug(int side) { return &G.dbg[side ? 1 : 0]; }

/* ================================================ 4) pegamento sobre la RAM ======================================= */
#define RAM_MASK 0x1FFFFFu
#define RAM_SIZE 0x200000u
#define OBJS_NPARTS 0x2E
#define OBJS_VIS   0x32
#define OBJS_FIRST   0x48
#define OBJ_SIZE     0x5C
#define OBJ_WORLD    0x00
#define OBJ_SCREEN   0x20
#define OBJ_MODEL    0x40
#define OBJ_VIS    0x4C
#define MDL_T        0x20
#define MDL_PARENT   0x2C
#define MDL_NF       0x04
#define MDL_NV_OFF   0x34
#define MDL_VOFF     0x38
#define MDL_VIDX     0x3C
#define MDL_NN_OFF   0x40
#define MDL_NOFF     0x44
#define MDL_NIDX     0x48
#define MAX_PARTS    64

static inline int ptr_ok(uint32_t p, uint32_t len) { return (p & 3u) == 0 && (p >> 29) == 4u && (p & RAM_MASK) + len <= RAM_SIZE && (p & RAM_MASK) >= 0x10000u; }
static inline int32_t rd_i32(const uint8_t *r, uint32_t a) { int32_t v; memcpy(&v, r + (a & RAM_MASK & ~3u), 4); return v; }
static inline uint32_t rd_u32(const uint8_t *r, uint32_t a) { return (uint32_t)rd_i32(r, a); }
static inline int16_t rd_s16(const uint8_t *r, uint32_t a) { int16_t v; memcpy(&v, r + (a & RAM_MASK & ~1u), 2); return v; }
static inline void wr_i32(uint8_t *r, uint32_t a, int32_t v) { memcpy(r + (a & RAM_MASK & ~3u), &v, 4); }
static inline void wr_s16(uint8_t *r, uint32_t a, int16_t v) { memcpy(r + (a & RAM_MASK & ~1u), &v, 2); }

typedef struct { float R[9]; float t[3]; } Xf;   /* afín: v' = R v + t */
static void xf_read(const uint8_t *ram, uint32_t a, Xf *x) {
    for (int i = 0; i < 9; i++) x->R[i] = (float)rd_s16(ram, a + 2 * i) / 4096.0f;
    for (int i = 0; i < 3; i++) x->t[i] = (float)rd_i32(ram, a + 0x14 + 4 * i);
}
static int16_t to_s16(float v) { return (int16_t)(v > 32767.0f ? 32767 : (v < -32768.0f ? -32768 : lrintf(v))); }
static int32_t to_i32(float v) { return (int32_t)(v > 2147483000.0f ? 2147483000 : (v < -2147483000.0f ? -2147483000 : lrintf(v))); }
static void xf_write(uint8_t *ram, uint32_t a, const Xf *x) {
    for (int i = 0; i < 9; i++) wr_s16(ram, a + 2 * i, to_s16(x->R[i] * 4096.0f));
    wr_s16(ram, a + 0x12, 0);
    for (int i = 0; i < 3; i++) wr_i32(ram, a + 0x14 + 4 * i, to_i32(x->t[i]));
}
/* a o b: composición; (a o b)(v) = a(b(v)) */
static void xf_comp(Xf *o, const Xf *a, const Xf *b) {
    Xf r;
    m_mulm(r.R, a->R, b->R);
    m_mulv(r.t, a->R, b->t); v_add(r.t, r.t, a->t);
    *o = r;
}
/* Lo que haría la GTE al guardar: la matriz de rotación redondeada a 1/4096 */
static void xf_quant(Xf *x) { for (int i = 0; i < 9; i++) x->R[i] = (float)to_s16(x->R[i] * 4096.0f) / 4096.0f; }

static void smoothstepf(float *w) { float x = clampf(*w, 0.0f, 1.0f); *w = x * x * (3 - 2 * x); }

typedef struct { uint32_t addr, model; int parent; float t[3]; } PartInfo;

/* Datos del modelo de un brazo y comprobación de que es el esperado (jerarquía hombro->codo->muñeca bajo el torso) */
static int read_part(const uint8_t *ram, uint32_t pset, int idx, PartInfo *p) {
    p->addr = pset + OBJS_FIRST + OBJ_SIZE * (uint32_t)idx;
    p->model = rd_u32(ram, p->addr + OBJ_MODEL);
    if (!ptr_ok(p->model, 0x40)) return 0;
    p->parent = rd_i32(ram, p->model + MDL_PARENT);
    for (int i = 0; i < 3; i++) p->t[i] = (float)rd_i32(ram, p->model + MDL_T + 4 * (uint32_t)i);
    return 1;
}

void vrbody_restore(uint8_t *ram) {
    if (!G.pending && !G.fs[0].used && !G.fs[1].used) return;
    G.pending = 0;
    if (!ram) return;
    /* No depende del puntero actual del jugador: si el objeto cambió (otra zona, muerte) pero la pieza guardada sigue
     * siendo la misma (mismo model*), se devuelve igualmente; así la lógica nunca ve la pose IK. */
    for (int i = 0; i < G.pend_n; i++) {
        Saved *s = &G.sv[i];
        if (!s->used) continue;
        /* la pieza debe seguir siendo la misma (mismo model*): si el objeto se destruyó/rehízo no se escribe */
        if (ptr_ok(s->addr, OBJ_SIZE) && rd_u32(ram, s->addr + OBJ_MODEL) == s->model) {
            memcpy(ram + (s->addr & RAM_MASK), s->raw, sizeof s->raw);
            if (s->vis_forced) wr_s16(ram, s->addr + OBJ_VIS, s->vis_old);
        } else G.st.stale_dropped++;
        s->used = 0;
    }
    G.pend_n = 0;
    for (int i = 0; i < 2; i++) {
        FistSave *f = &G.fs[i];
        if (!f->used) continue;
        f->used = 0;
        if (ptr_ok(f->model, 0x58) && rd_u32(ram, f->model + MDL_NV_OFF) == (uint32_t)f->nv && rd_u32(ram, f->model + MDL_VOFF) == f->vaddr && rd_u32(ram, f->model + MDL_NOFF) == f->naddr) {
            memcpy(ram + (f->vaddr & RAM_MASK), f->v, (size_t)f->nv * 8);
            memcpy(ram + (f->naddr & RAM_MASK), f->n, (size_t)f->nn * 8);
        } else G.st.stale_dropped++;
    }
    G.st.restores++;
}

static int hand_valid(const VrHandPose *h) {
    if (!h->valid || !all_finite(h->pos, 3) || !all_finite(h->rot, 4)) return 0;
    float n = h->rot[0] * h->rot[0] + h->rot[1] * h->rot[1] + h->rot[2] * h->rot[2] + h->rot[3] * h->rot[3];
    if (n < 1e-6f) return 0;
    if (fabsf(h->pos[0]) > 1000.0f || fabsf(h->pos[1]) > 1000.0f || fabsf(h->pos[2]) > 1000.0f) return 0;
    return 1;
}

/* Marco de la pieza mano del modelo en el marco de mano de vr_pose.h (columnas = imágenes de los ejes locales).
 * Derecha del personaje (rig.side = -1): dedos -Y_local -> -Z_mano, palma +X_local -> -Y_mano, pulgar +Z_local -> -X_mano.
 * Izquierda: palma -X_local -> -Y_mano, pulgar +Z_local -> +X_mano. */
static void hand_map(float side, float *M) {
    if (side < 0) { const float r[9] = {0, 0, -1, -1, 0, 0, 0, 1, 0}; memcpy(M, r, sizeof r); }
    else          { const float r[9] = {0, 0, 1, 1, 0, 0, 0, 1, 0};  memcpy(M, r, sizeof r); }
}

/* Puño: deforma la malla de la pieza mano (vértices y normales en la RAM del juego) con vr_fist. Guarda los originales para vrbody_restore. */
static int ram_ok(uint32_t p, uint32_t len) { return (p >> 29) == 4u && (p & RAM_MASK) >= 0x10000u && (uint64_t)(p & RAM_MASK) + len <= RAM_SIZE; }
static void fist_apply(uint8_t *ram, int s, uint32_t model, float side_sign, float grip) {
    FistSave *f = &G.fs[s];
    if (f->used || !ptr_ok(model, 0x58)) return;
    uint32_t nf = rd_u32(ram, model + MDL_NF), nv = rd_u32(ram, model + MDL_NV_OFF), nn = rd_u32(ram, model + MDL_NN_OFF);
    uint32_t vo = rd_u32(ram, model + MDL_VOFF), no = rd_u32(ram, model + MDL_NOFF), vi = rd_u32(ram, model + MDL_VIDX), ni = rd_u32(ram, model + MDL_NIDX);
    if (nv < 8 || nv > VRFIST_MAX_V || nn < 1 || nn > VRFIST_MAX_V || nf < 4 || nf > VRFIST_MAX_F || (vo & 1u) || (no & 1u) ||
        !ram_ok(vo, nv * 8) || !ram_ok(no, nn * 8) || !ram_ok(vi, nf * 4) || !ram_ok(ni, nf * 4)) { G.st.fist_skipped++; return; }
    for (int o = 0; o < 2; o++) if (o != s && G.fs[o].used && G.fs[o].vaddr == vo) return;     /* la misma malla ya deformada para la otra mano */
    int16_t v[VRFIST_MAX_V * 4], n[VRFIST_MAX_V * 4], ov[VRFIST_MAX_V * 4], on[VRFIST_MAX_V * 4];
    memcpy(v, ram + (vo & RAM_MASK), nv * 8); memcpy(n, ram + (no & RAM_MASK), nn * 8);
    VrFistMesh m = {(int)nv, (int)nn, (int)nf, v, n, ram + (vi & RAM_MASK), ram + (ni & RAM_MASK)};
    if (vrfist_deform(&m, side_sign, grip, &G.cfg.fist, ov, on) != 0) { G.st.fist_skipped++; return; }
    f->used = 1; f->model = model; f->vaddr = vo; f->naddr = no; f->nv = (int)nv; f->nn = (int)nn;
    memcpy(f->v, v, nv * 8); memcpy(f->n, n, nn * 8);
    memcpy(ram + (vo & RAM_MASK), ov, nv * 8); memcpy(ram + (no & RAM_MASK), on, nn * 8);
    G.st.fist_applies++;
}

/* Puño previo a la captura de la escena nativa: la captura ocurre antes de vrbody_apply, así que la malla de la mano saldría abierta.
 * Se aplica con el agarre y la mano del último vrbody_apply (un fotograma de retraso en el agarre); vrbody_restore lo devuelve. */
void vrbody_prefist(uint8_t *ram) {
    if (!ram || !G.cfg.enabled || !G.cfg.fist_enabled) return;
    for (int s = 0; s < 2; s++) {
        if (!G.pf[s].ok) continue;
        float g = G.pf[s].grip;
        if (g < G.cfg.fist_min_grip) continue;
        fist_apply(ram, s, G.pf[s].model, G.pf[s].side, g);
        G.st.fist_pre++;
    }
}

int vrbody_apply(uint8_t *ram) {
    G.st.applies++;
    if (G.pending) { G.st.stale_dropped++; vrbody_restore(ram); }      /* un apply anterior sin restore: primero devolver sus originales */
    if (!ram) return 0;
    const VrBodyConfig *cfg = &G.cfg;
    if (!cfg->enabled) { G.st.skip_disabled++; return 0; }
    VrBodyView view; memset(&view, 0, sizeof view);
    if (!G.view_fn || !G.view_fn(&view) || !view.active || !all_finite(view.eye, 3) || !all_finite(view.p_ref, 3) || !isfinite(view.theta_deg) ||
        !(view.units_per_m > 1.0f && view.units_per_m < 1e6f)) {
        G.st.skip_noview++;
        for (int s = 0; s < 2; s++) { G.hand[s].w = 0; G.hand[s].was_valid = 0; G.dbg[s].driven = 0; G.pf[s].ok = 0; }
        return 0;
    }

    /* ---- modelo del jugador ---- */
    uint32_t ob = rd_u32(ram, BA_PLAYER_BODY);
    if (!ptr_ok(ob, 8)) { G.st.skip_model++; return 0; }
    uint32_t pset = rd_u32(ram, ob);
    if (!ptr_ok(pset, OBJS_FIRST + OBJ_SIZE)) { G.st.skip_model++; return 0; }
    int nm = rd_s16(ram, pset + OBJS_NPARTS);
    if (nm < cfg->min_parts || nm > MAX_PARTS || !ptr_ok(pset, OBJS_FIRST + OBJ_SIZE * (uint32_t)nm)) { G.st.skip_model++; return 0; }
    if (rd_s16(ram, pset + OBJS_VIS) == 0) { G.st.skip_offscreen++; return 0; }   /* el objeto entero está fuera de pantalla */

    int it = cfg->part_torso;
    if (it < 0 || it >= nm) { G.st.skip_model++; return 0; }
    PartInfo ptor;
    if (!read_part(ram, pset, it, &ptor)) { G.st.skip_model++; return 0; }
    Xf torso_w, torso_s, eye;
    xf_read(ram, ptor.addr + OBJ_WORLD, &torso_w);
    xf_read(ram, ptor.addr + OBJ_SCREEN, &torso_s);

    /* matriz de vista del motor: la del canal (mundo->vista); si el screen del torso no coincide (ajuste vertical activo, etc.)
     * se deduce del propio torso: screen = EYE o world */
    {
        Xf ch; xf_read(ram, BA_VIEW_CHANNEL + BCH_CAM_INV, &ch);
        Xf pr; Xf tw = torso_w; xf_quant(&tw);
        xf_comp(&pr, &ch, &tw);
        float e = 0;
        for (int i = 0; i < 9; i++) e = fmaxf(e, fabsf(pr.R[i] - torso_s.R[i]));
        float et = fmaxf(fabsf(pr.t[0] - torso_s.t[0]), fmaxf(fabsf(pr.t[1] - torso_s.t[1]), fabsf(pr.t[2] - torso_s.t[2])));
        if (e < 0.004f && et < 3.0f) eye = ch;
        else {
            float wr[9]; vrbody_orthonormalize(torso_w.R, wr);
            float wrt[9]; m_transpose(wrt, wr);
            m_mulm(eye.R, torso_s.R, wrt);
            float tt[3]; m_mulv(tt, eye.R, torso_w.t); v_sub(eye.t, torso_s.t, tt);
        }
    }

    if (G.grab.active && ++G.grab_age > 3) G.grab.active = 0;      /* ya no se refresca: soltado */
    /* ---- manos ---- */
    VrHandPose hp[2]; memset(hp, 0, sizeof hp);
    if (vrpose_get_hands(hp) < 0) memset(hp, 0, sizeof hp);
    const float dt = cfg->frame_dt_s;
    int any = 0;
    for (int s = 0; s < 2; s++) {
        Hand *h = &G.hand[s];
        int v = hand_valid(&hp[s]);
        if (cfg->blend_s <= 0) h->w = v ? 1.0f : 0.0f;
        else h->w = clampf(h->w + (v ? dt : -dt) / cfg->blend_s, 0.0f, 1.0f);
        if (v) {
            float q[4] = {hp[s].rot[0], hp[s].rot[1], hp[s].rot[2], hp[s].rot[3]}; q_normalize(q);
            float jump = 0;
            if (h->init) { float d3[3]; v_sub(d3, hp[s].pos, h->pos); jump = v_len(d3); }
            float aqn[4] = {hp[s].aim_rot[0], hp[s].aim_rot[1], hp[s].aim_rot[2], hp[s].aim_rot[3]};
            int aok = hp[s].aim_valid && all_finite(aqn, 4) && (aqn[0] * aqn[0] + aqn[1] * aqn[1] + aqn[2] * aqn[2] + aqn[3] * aqn[3]) > 1e-6f;
            if (aok) q_normalize(aqn);
            if (!h->init || !h->was_valid || (cfg->max_hand_jump_m > 0 && jump > cfg->max_hand_jump_m)) {
                v_cp(h->pos, hp[s].pos); memcpy(h->q, q, sizeof q); h->init = 1;
                if (aok) memcpy(h->aq, aqn, sizeof aqn);
            } else {
                float a = cfg->smooth_tau_s > 0 ? 1 - expf(-dt / cfg->smooth_tau_s) : 1.0f;
                for (int i = 0; i < 3; i++) h->pos[i] += a * (hp[s].pos[i] - h->pos[i]);
                q_slerp(h->q, h->q, q, a);
                if (aok) { if (h->aim_ok) q_slerp(h->aq, h->aq, aqn, a); else memcpy(h->aq, aqn, sizeof aqn); }
            }
            h->aim_ok = aok;
        }
        h->was_valid = v;
        any |= v || h->w > 0;
    }
    if (!any) { G.st.skip_nohands++; for (int s = 0; s < 2; s++) G.dbg[s].driven = 0, G.pf[s].ok = 0; return 0; }

    /* relación usuario -> mundo del motor */
    float th = (view.theta_deg + 180.0f) * DEG, cs = cosf(th), sn = sinf(th);
    const float T[9] = {cs, 0, sn, 0, 1, 0, -sn, 0, cs};
    G.view_ok = 1; v_cp(G.view_eye, view.eye); v_cp(G.view_pref, view.p_ref); memcpy(G.view_T, T, sizeof T); G.view_upm = view.units_per_m;

    typeof(G.cal) cal_snap; cal_lock(); memcpy(cal_snap, G.cal, sizeof cal_snap); cal_unlock();
    int driven = 0, ns = 0;
    for (int s = 0; s < 2; s++) { G.pf[s].ok = 0;
        VrBodyDebug *dbg = &G.dbg[s];
        dbg->driven = 0;
        G.dbg_frame_ok[s] = 0;
        Hand *h = &G.hand[s];
        if (h->w <= 0.0005f) continue;
        int ia = cfg->part_arm[s][0], ie = cfg->part_arm[s][1], iw = cfg->part_arm[s][2];
        if (ia < 0 || ie < 0 || iw < 0 || ia >= nm || ie >= nm || iw >= nm || ia == ie || ie == iw) { G.st.skip_model++; continue; }
        PartInfo pa, pe, pw;
        if (!read_part(ram, pset, ia, &pa) || !read_part(ram, pset, ie, &pe) || !read_part(ram, pset, iw, &pw)) { G.st.skip_model++; continue; }
        if (pa.parent != it || pe.parent != ia || pw.parent != ie) { G.st.skip_model++; continue; }    /* jerarquía esperada */
        VrArmRig rig;
        rig.side = s == 0 ? 1.0f : -1.0f;                  /* lado del personaje: izquierda (s=0) = +X */
        v_cp(rig.elbow_off, pe.t); v_cp(rig.wrist_off, pw.t);
        {   /* el hombro debe estar donde dice el modelo respecto al torso (matrices compuestas, no otro esquema) */
            Xf wa; xf_read(ram, pa.addr + OBJ_WORLD, &wa);
            float ex_[3]; m_mulv(ex_, torso_w.R, pa.t); v_add(ex_, ex_, torso_w.t);
            float dv[3]; v_sub(dv, ex_, wa.t);
            if (v_len(dv) > 40.0f) { G.st.skip_model++; continue; }
        }
        Xf wa, we, ww;
        xf_read(ram, pa.addr + OBJ_WORLD, &wa); xf_read(ram, pe.addr + OBJ_WORLD, &we); xf_read(ram, pw.addr + OBJ_WORLD, &ww);
        float wr_w[9];
        vrbody_orthonormalize(ww.R, wr_w);

        /* objetivo del usuario -> mundo */
        float we_w = h->w; smoothstepf(&we_w);
        float Pm[3]; v_sub(Pm, h->pos, view.p_ref);
        float Pw[3]; m_mulv(Pw, T, Pm); v_scale(Pw, Pw, view.units_per_m); v_add(Pw, Pw, view.eye);
        /* golpe sintético (botón): la mano sale hacia delante y vuelve con el puño cerrado */
        int jabbing = 0;
        if (h->jab_t >= 0) {
            float js = cfg->jab_s > 0.05f ? cfg->jab_s : 0.28f;
            h->jab_t += dt;
            float u = h->jab_t / js;
            if (u >= 1.0f) h->jab_t = -1.0f;
            else {
                float e = u < 0.4f ? u / 0.4f : (1.0f - u) / 0.6f; smoothstepf(&e);
                v_madd(Pw, Pw, h->jab_dir, cfg->jab_reach_mm * e * we_w);
                jabbing = 1; G.st.jab_applies++;
            }
        }
        float Ru[9], Rq[4], qo[4];
        {   /* corrección fija agarre -> mano: calibrada (cuaternión completo) o cabeceo (X del agarre) seguido del alabeo (eje de los dedos) */
            if (cal_snap[s].ok) { memcpy(qo, cal_snap[s].q, sizeof qo); q_normalize(qo); }
            else {
                float pd = cfg->hand_pitch_deg;
                if (cfg->hand_pitch_auto) {
                    /* cabeceo automático: suavizado (constante 0,15 s) y, si el apuntado falta un instante, se mantiene el último valor válido hasta 10 fotogramas */
                    float pa_ = 0;
                    if (h->aim_ok && vrbody_pitch_from_aim(h->q, h->aq, &pa_)) {
                        if (!h->pitch_init) { h->pitch_s = pa_; h->pitch_init = 1; }
                        else h->pitch_s += (1 - expf(-dt / 0.15f)) * (pa_ - h->pitch_s);
                        h->pitch_miss = 0; pd = h->pitch_s;
                    } else if (h->pitch_init && ++h->pitch_miss <= 10) pd = h->pitch_s;
                    else h->pitch_init = 0;
                }
                float qp[4], qr[4]; pitch_quat(pd, qp);
                qr[0] = cfg->hand_rot_offset[s][0]; qr[1] = cfg->hand_rot_offset[s][1]; qr[2] = cfg->hand_rot_offset[s][2]; qr[3] = cfg->hand_rot_offset[s][3]; q_normalize(qr);
                q_mul(qo, qp, qr);
            }
            q_mul(Rq, h->q, qo);
        }
        memcpy(G.fore_q[s], h->q, sizeof h->q);     /* marco del AGARRE (sin hand_rot_offset): el radar de muñeca va en el lado +Y del agarre (arriba con el pulgar arriba), como antes */
        vrbody_quat_to_mat(Rq, Ru);
        float Rw[9]; m_mulm(Rw, T, Ru);                                    /* ejes del marco de mano en el mundo */
        float zc[3]; m_col(zc, Rw, 2);
        float wrist_t[3]; v_madd(wrist_t, Pw, zc, cfg->grip_to_wrist_mm);  /* la muñeca queda hacia atrás de la palma (+Z de la mano) */

        /* agarre de un enemigo: la mano se pega al cuello (transición suave) y vuelve al mando al soltar */
        {
            float want = 0;
            float Nn[3] = {0, 0, 0}, C[3] = {0, 0, 0};
            if (G.grab.active) {
                float side_s = s == 0 ? 1.0f : -1.0f;
                v_madd(C, G.grab.neck, G.grab.left, side_s * cfg->grab_spread_mm);
                float dsh[3]; v_sub(dsh, G.grab.neck, wa.t);
                if (v_len(dsh) <= cfg->grab_max_dist_mm) want = 1;
                v_cp(Nn, G.grab.neck);
            }
            float rate = cfg->grab_blend_s > 1e-3f ? dt / (want > 0 ? cfg->grab_blend_s : cfg->grab_blend_s * 1.25f) : 1.0f;
            h->att = clampf(h->att + (want > 0 ? rate : -rate), 0.0f, 1.0f);
            if (h->att > 0.0005f) {
                float ae = h->att; smoothstepf(&ae);
                if (want <= 0) { v_cp(C, Pw); v_cp(Nn, Pw); }     /* soltando con el enemigo ya lejos: desde donde esté la mano al mando */
                float f[3]; v_sub(f, Nn, C);
                float fl = v_len(f);
                if (fl < 1e-3f) { float fb[3] = {0, 0, 1}; v_cp(f, fb); fl = 1; }
                v_scale(f, f, 1.0f / fl);
                f[1] *= 0.5f; v_norm(f, f, zc);                   /* dedos casi horizontales */
                float z_[3]; v_scale(z_, f, -1.0f);
                float up[3] = {0, 1, 0}, y_[3], t_[3];
                v_madd(t_, up, f, -v_dot(up, f));
                if (v_norm(y_, t_, NULL) < 1e-3f) { float a[3] = {1, 0, 0}; v_cp(y_, a); }
                float x_[3]; v_cross(x_, y_, z_);
                float A[9]; m_setcol(A, 0, x_); m_setcol(A, 1, y_); m_setcol(A, 2, z_);
                float ph = (s == 0 ? 1.0f : -1.0f) * cfg->grab_roll_deg * DEG, cz = cosf(ph), sz = sinf(ph);
                float Rz[9] = {cz, -sz, 0, sz, cz, 0, 0, 0, 1}, Ra[9]; m_mulm(Ra, A, Rz);
                float wr_a[3]; v_madd(wr_a, C, z_, cfg->grip_to_wrist_mm);
                for (int i = 0; i < 3; i++) wrist_t[i] += (wr_a[i] - wrist_t[i]) * ae;
                float qa_[4], qb_[4], qm_[4]; vrbody_mat_to_quat(Rw, qa_); vrbody_mat_to_quat(Ra, qb_); q_slerp(qm_, qa_, qb_, ae); vrbody_quat_to_mat(qm_, Rw);
                G.st.grab_applies++;
            }
        }

        /* agarre */
        int is_dom = (s == (cfg->dominant ? 1 : 0));
        float gset = h->grip_set;
        if (jabbing) gset = 1.0f;
        if (G.armed && is_dom && gset < cfg->armed_grip) gset = cfg->armed_grip;
        h->grip_cur += (gset - h->grip_cur) * (1 - expf(-dt / 0.06f));
        float M[9]; hand_map(rig.side, M);
        float curl[9]; { const float zax[3] = {0, 0, 1}; m_axis_angle(curl, zax, -rig.side * cfg->curl_deg * DEG * h->grip_cur); }
        float Rt[9]; m_mulm(Rt, M, curl); m_mulm(Rt, Rw, Rt);               /* orientación objetivo de la pieza mano */

        /* animación original (referencia del fundido) */
        float Sh[3]; v_cp(Sh, wa.t);
        float Ea[3]; v_cp(Ea, we.t);
        float Wa[3]; v_cp(Wa, ww.t);
        float Tq[9]; vrbody_orthonormalize(torso_w.R, Tq);

        /* fundido: objetivo = mezcla de animación y mano; el IK resuelve siempre una cadena coherente */
        float wt[3]; for (int i = 0; i < 3; i++) wt[i] = Wa[i] + (wrist_t[i] - Wa[i]) * we_w;
        float qa[4], qb[4], qm[4], Rtb[9];
        vrbody_mat_to_quat(wr_w, qa); vrbody_mat_to_quat(Rt, qb); q_slerp(qm, qa, qb, we_w); vrbody_quat_to_mat(qm, Rtb);
        float pole_nat[3];
        { float pl[3] = {cfg->pole[0] * rig.side, cfg->pole[1], cfg->pole[2]}; float tmp[3]; m_mulv(tmp, Tq, pl); v_norm(pole_nat, tmp, NULL); }
        float pole[3]; v_cp(pole, pole_nat);
        {   /* codo de la animación (perpendicular a hombro->muñeca) mezclado con el natural */
            float va[3]; v_sub(va, Wa, Sh); float vah[3]; v_norm(vah, va, NULL);
            float ea[3]; v_sub(ea, Ea, Sh); float pe_[3]; v_madd(pe_, ea, vah, -v_dot(ea, vah));
            float pn[3];
            if (v_norm(pn, pe_, NULL) > 5.0f) for (int i = 0; i < 3; i++) pole[i] = pn[i] * (1 - we_w) + pole_nat[i] * we_w;
        }
        VrArmIn in; v_cp(in.shoulder, Sh); memcpy(in.torso, Tq, sizeof Tq); v_cp(in.wrist, wt); memcpy(in.hand, Rtb, sizeof Rtb); v_cp(in.pole, pole);
        VrArmOut sol;
        if (vrbody_solve_arm(&rig, &cfg->limits, &in, &sol) != 0) { G.st.bad_solve++; continue; }
        if (sol.flags & VRARM_STRETCHED) G.st.stretched++;
        if (sol.flags & VRARM_TOO_CLOSE) G.st.too_close++;
        if (sol.flags & VRARM_SHOULDER_LIMIT) G.st.shoulder_limit++;
        if (sol.flags & VRARM_WRIST_LIMIT) G.st.wrist_limit++;

        /* El fundido con la animación se hace sobre el OBJETIVO (muñeca, orientación de la mano y codo): el IK devuelve siempre
         * una cadena coherente (huesos unidos). Mezclar rotaciones de la animación con posiciones del IK separaría las piezas. */
        const float *Rin[3] = {sol.R_upper, sol.R_fore, sol.R_hand};
        Xf nw[3];
        for (int k = 0; k < 3; k++) memcpy(nw[k].R, Rin[k], sizeof nw[k].R);
        v_cp(nw[0].t, Sh); v_cp(nw[1].t, sol.elbow); v_cp(nw[2].t, sol.wrist);
        for (int k = 0; k < 3; k++) xf_quant(&nw[k]);

        /* guardar y escribir world + screen */
        const PartInfo *pi[3] = {&pa, &pe, &pw};
        for (int k = 0; k < 3; k++) {
            Saved *sv = &G.sv[ns++];
            sv->used = 1; sv->addr = pi[k]->addr; sv->model = pi[k]->model;
            memcpy(sv->raw, ram + (pi[k]->addr & RAM_MASK), sizeof sv->raw);
            /* Brazos siempre dibujados: el motor decide la visibilidad de cada pieza (contador +0x4C = 0: ni se transforma ni genera
             * paquetes) con la pose ANIMADA, no con la de la IK. Si dejó a 0 una pieza que la IK sí lleva a la vista, se le da el valor
             * de la pieza del torso durante esta transformación y se devuelve en vrbody_restore. La cabeza (parte 6) no se toca. */
            sv->vis_old = rd_s16(ram, pi[k]->addr + OBJ_VIS); sv->vis_forced = 0;
            if (cfg->force_arm_visible && sv->vis_old == 0) {
                int16_t tv = rd_s16(ram, ptor.addr + OBJ_VIS);
                if (tv > 0) { wr_s16(ram, pi[k]->addr + OBJ_VIS, tv); sv->vis_forced = 1; G.st.vis_forced++; }
            }
            Xf scr; xf_comp(&scr, &eye, &nw[k]);
            xf_write(ram, pi[k]->addr + OBJ_WORLD, &nw[k]);
            xf_write(ram, pi[k]->addr + OBJ_SCREEN, &scr);
        }
        G.pending = 1; G.pend_n = ns;
        {   /* marco efectivo de la mano en el espacio del usuario (depuración): pieza tras la IK -> convención del agarre, sin el giro de cierre */
            float tr[9]; m_transpose(tr, T);
            float wu[3]; v_sub(wu, sol.wrist, view.eye); m_mulv(wu, tr, wu); v_scale(wu, wu, 1.0f / view.units_per_m); v_add(wu, wu, view.p_ref);
            float Rpu[9]; m_mulm(Rpu, tr, sol.R_hand);
            float Mc[9], Mct[9], Re0[9], Re[9], Qo[9], Qot[9];
            m_mulm(Mc, M, curl); m_transpose(Mct, Mc); m_mulm(Re0, Rpu, Mct);
            vrbody_quat_to_mat(qo, Qo); m_transpose(Qot, Qo); m_mulm(Re, Re0, Qot);      /* sin el giro agarre -> mano: se compara con el marco del agarre */
            float ez[3]; m_col(ez, Re, 2);
            float *f = G.dbg_frame[s];
            for (int j = 0; j < 3; j++) {
                float c[3]; m_col(c, Re, j);
                f[4 * j] = c[0]; f[4 * j + 1] = c[1]; f[4 * j + 2] = c[2]; f[4 * j + 3] = 0;
            }
            for (int j = 0; j < 3; j++) f[12 + j] = wu[j] - ez[j] * (cfg->grip_to_wrist_mm / view.units_per_m);
            f[15] = 1;
            G.dbg_frame_ok[s] = all_finite(f, 16);
        }
        G.pf[s].ok = 1; G.pf[s].model = pw.model; G.pf[s].side = -rig.side; G.pf[s].grip = h->grip_cur * we_w;
        memcpy(G.pf[s].R, nw[2].R, sizeof G.pf[s].R); memcpy(G.pf[s].t, nw[2].t, sizeof G.pf[s].t);
        if (cfg->fist_enabled && h->grip_cur * we_w >= cfg->fist_min_grip) fist_apply(ram, s, pw.model, -rig.side, h->grip_cur * we_w);
        dbg->driven = 1; dbg->w = h->w; dbg->grip = h->grip_cur; v_cp(dbg->target_wrist, wt); memcpy(dbg->target_hand, Rtb, sizeof Rtb); dbg->sol = sol;
        driven++;
    }
    G.st.arms_driven += (uint32_t)driven;
    if (driven && G.hook) G.hook(ram);
    return driven;
}

int vrbody_debug_hand_frame(int side, float m[16]) {
    if (!m || (side != 0 && side != 1) || !G.dbg_frame_ok[side] || !G.dbg[side].driven) return 0;
    memcpy(m, G.dbg_frame[side], 16 * sizeof(float));
    return 1;
}

/* Antebrazo tras la IK, en el espacio del usuario (metros): codo y muñeca resueltos (con los límites aplicados) y la dirección
 * del dorso (eje +Y del marco de mano de vr_pose.h, sin la componente a lo largo del hueso). Solo vale tras un vrbody_apply que
 * moviera ese brazo y con el fundido casi completo; si no, 0 y el llamador usa otra fuente (la pose del mando). */
int vrbody_forearm_pose(int side, VrBodyForearm *out) {
    if (!out || (side != 0 && side != 1) || !G.view_ok) return 0;
    const VrBodyDebug *d = &G.dbg[side];
    if (!d->driven || d->w < 0.3f || !(G.view_upm > 1.0f)) return 0;
    float tr[9]; m_transpose(tr, G.view_T);
    const float *src[2] = {d->sol.elbow, d->sol.wrist};
    float *dst[2] = {out->elbow, out->wrist};
    for (int k = 0; k < 2; k++) {
        float v[3]; v_sub(v, src[k], G.view_eye);
        m_mulv(v, tr, v);
        v_scale(v, v, 1.0f / G.view_upm);
        v_add(dst[k], v, G.view_pref);
        if (!all_finite(dst[k], 3)) return 0;
    }
    float Ru[9], dorsal[3], bone[3], b[3];
    vrbody_quat_to_mat(G.fore_q[side], Ru);
    m_col(dorsal, Ru, 1);
    v_sub(bone, out->wrist, out->elbow);
    if (v_norm(b, bone, NULL) < 1e-4f) return 0;
    float tmp[3]; v_madd(tmp, dorsal, b, -v_dot(dorsal, b));
    if (v_norm(out->dorsal, tmp, NULL) < 1e-3f) return 0;
    return 1;
}
