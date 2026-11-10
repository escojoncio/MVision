/* Pruebas de vr_bodylive (sin datos del juego): el jugador anda a 2,5 m/s (100 mm por escena, 25 escenas/s) con el torso fijo respecto al ojo y las piernas oscilando; el
 * renderer dibuja a 90 Hz con la corrección de vr_smooth. Mide, en el espacio del usuario, el diente de sierra del torso respecto a la cabeza y el paso máximo por fotograma
 * de render de las 16 piezas y de una mano propia, con las matrices de la escena tal cual (antes) y con vbl_piece (después). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "vr_smooth.h"
#include "vr_bodylive.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FALLO %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

#define PKEY 0x80123456u
enum { NP = 16, NH = 2, NO = 1 };
static NsInstance inst[3][3];
static NsPiece piece[3][NP + NH + NO];
static NsScene scn[3];
static const float K = 0.001f, PREF[3] = {0.1f, 1.2f, -0.2f};

/* Escena k: ojo en (100 k, 1500, 0); pieza j del jugador en ojo + rel_j(k) con giro sobre Y; piezas 10..15 (piernas) oscilan a 2 Hz; mano propia junto al torso; un enemigo fijo en el mundo */
static void rel(int j, int k, float r[3], float *ang) {
    float ph = 6.2831853f * 2.0f * (float)k * 0.04f + (float)j;
    r[0] = 30.0f * (float)(j % 4) - 40.0f; r[1] = -100.0f * (float)j; r[2] = -60.0f; *ang = 0.2f * (float)j;
    if (j >= 10) { r[0] += 250.0f * sinf(ph); r[1] += 60.0f * cosf(ph); *ang += 0.6f * sinf(ph); }
    if (j >= 2 && j <= 4) r[0] += 200.0f * sinf(0.7f * (float)k + (float)j);   /* brazo derecho: lo lleva la IK del mando, se mueve con la mano del usuario */
}
static void xf_set(NsXf *x, float ang, const float t[3]) {
    float c = cosf(ang), s = sinf(ang);
    float m[12] = {c, 0, s, t[0], 0, 1, 0, t[1], -s, 0, c, t[2]};
    memcpy(x->m, m, sizeof m);
}
static const NsScene *build(int slot, int k) {
    NsScene *sc = &scn[slot];
    memset(sc, 0, sizeof *sc);
    float eye[3] = {100.0f * (float)k, 1500.0f, 0.0f};
    sc->inst = inst[slot]; sc->piece = piece[slot]; sc->n_inst = sc->cap_inst = 3; sc->n_piece = sc->cap_piece = NP + NH + NO;
    sc->inst[0].key = PKEY; sc->inst[0].first_piece = 0; sc->inst[0].n_pieces = NP;
    sc->inst[1].key = NS_HAND_KEY_R; sc->inst[1].first_piece = NP; sc->inst[1].n_pieces = NH;
    sc->inst[2].key = 0x80111111u; sc->inst[2].first_piece = NP + NH; sc->inst[2].n_pieces = NO;
    for (int j = 0; j < NP + NH; j++) {
        float r[3], a, t[3];
        rel(j < NP ? j : 3, k, r, &a);
        if (j >= NP) r[1] -= 40.0f * (float)(j - NP + 1);
        for (int i = 0; i < 3; i++) t[i] = eye[i] + r[i];
        xf_set(&sc->piece[j].world, a, t);
    }
    float te[3] = {5000, 0, 3000}; xf_set(&sc->piece[NP + NH].world, 0.3f, te);
    for (int i = 0; i < 3; i++) { sc->to_vr[4 * i + i] = K; sc->to_vr[4 * i + 3] = PREF[i] - K * eye[i]; }   /* P = p_ref + K (W - ojo) */
    sc->valid = NS_SV_TOVR_OK; sc->t_ns = 1000000000ull + (uint64_t)k * 40000000ull;
    return sc;
}
/* origen de la pieza en el espacio del usuario tras to_vr y la corrección */
static void shown(const NsScene *sc, const NsXf *w, const float m[16], double o[3]) {
    double p[3];
    for (int i = 0; i < 3; i++) p[i] = sc->to_vr[4 * i] * w->m[3] + sc->to_vr[4 * i + 1] * w->m[7] + sc->to_vr[4 * i + 2] * w->m[11] + sc->to_vr[4 * i + 3];
    for (int i = 0; i < 3; i++) o[i] = m[i] * p[0] + m[4 + i] * p[1] + m[8 + i] * p[2] + m[12 + i];
}

int main(void) {
    static VblRing ring; static VblPose pose;
    NsPlayerMap map; memset(&map, 0, sizeof map);
    map.bound = 1; map.key = PKEY;
    for (int q = 0; q < NP; q++) { map.part[q].n = 1; map.part[q].idx[0] = (uint32_t)q; }
    const uint32_t live = (1u << 2) | (1u << 3) | (1u << 4);
    double hand = 0;
    VrSmooth sm; vrsm_init(&sm, NULL);
    const float R[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const NsScene *cur = NULL, *old = NULL;
    int k = 0;
    double saw0 = 0, saw1 = 0, step0 = 0, step1 = 0, alt = 0, prev0[NP + NH][3], prev1[NP + NH][3]; int have = 0, used = 0;
    NsXf x;
    CHECK(vbl_piece(&ring, build(0, 0), 0, 1, 0.5f, (const float[16]){1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, NULL, &x) == 0, "sin poses: no se toca la pieza");
    for (int f = 0; f < 270; f++) {
        double t = f / 90.0;
        while (k * 0.04 <= t) {
            old = cur; cur = build(k % 3, k);
            float org[3] = {100.0f * (float)k, 0, 0};
            VrSmSample s; vrsm_make_sample(&s, org, R, K, PREF, 1.5f, k * 0.04, 0.0, 1, k * 0.04);
            vrsm_push(&sm, &s, t);
            CHECK(vbl_capture(&pose, cur, &map, live, 0, 0, NULL) == NP + NH, "captura: piezas del jugador y de la mano");
            vbl_push(&ring, &pose);
            if (k == 0) CHECK(vbl_piece(&ring, cur, 0, 1, 0.5f, (const float[16]){1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, NULL, &x) == 0, "una sola pose: no se toca la pieza");
            k++;
        }
        VrSmCorr c; vrsm_eval(&sm, t, 0.0, &c);
        if (k < 4) continue;
        CHECK(vbl_piece(&ring, cur, 2, 0, c.alpha, c.m, NULL, &x) == 0, "una instancia que no es del jugador no se toca");
        for (int j = 0; j < NP + NH; j++) {
            double p0[3], p1[3], p2[3];
            uint32_t ii = j < NP ? 0u : 1u, kk = j < NP ? (uint32_t)j : (uint32_t)(j - NP);
            shown(cur, &cur->piece[j].world, c.m, p0);
            int ok = vbl_piece(&ring, cur, ii, kk, c.alpha, c.m, NULL, &x);
            CHECK(ok == 1, "pieza %d: sin matriz", j);
            if (!ok) continue;
            used++;
            shown(cur, &x, c.m, p1);
            NsXf y; CHECK(vbl_piece(&ring, old, ii, kk, c.alpha, c.m, NULL, &y) == 1, "pieza %d con la escena anterior", j);
            shown(old, &y, c.m, p2);
            for (int i = 0; i < 3; i++) alt = fmax(alt, fabs(p2[i] - p1[i]));
            int lv = (j >= 2 && j <= 4) || j >= NP;   /* brazo de la IK y mano propia: siguen al mando */
            if (lv) {   /* en la ÚLTIMA pose (la del mando más reciente), no una escena por detrás: el arma va en tiempo real */
                float r[3], a; rel(j < NP ? j : 3, k - 1, r, &a); if (j >= NP) r[1] -= 40.0f * (float)(j - NP + 1);
                for (int i = 0; i < 3; i++) hand = fmax(hand, fabs(p1[i] - (PREF[i] + K * r[i])));
            } else if (j < 10) {   /* torso, brazo izquierdo y cabeza: fijos respecto al ojo -> deben quedar fijos respecto a la cabeza (p_ref) */
                float r[3], a; rel(j, 0, r, &a);
                for (int i = 0; i < 3; i++) { saw0 = fmax(saw0, fabs(p0[i] - (PREF[i] + K * r[i]))); saw1 = fmax(saw1, fabs(p1[i] - (PREF[i] + K * r[i]))); }
            }
            if (have && !lv) for (int i = 0; i < 3; i++) { step0 = fmax(step0, fabs(p0[i] - prev0[j][i])); step1 = fmax(step1, fabs(p1[i] - prev1[j][i])); }
            memcpy(prev0[j], p0, sizeof p0); memcpy(prev1[j], p1, sizeof p1);
        }
        have = 1;
    }
    printf("cuerpo a 90 Hz (andar a 2,5 m/s): torso respecto a la cabeza, diente de sierra %.1f -> %.2f mm; paso maximo por fotograma de render de las 16 piezas + mano %.1f -> %.1f mm; con la escena anterior %.3f mm\n",
           saw0 * 1000.0, saw1 * 1000.0, step0 * 1000.0, step1 * 1000.0, alt * 1000.0);
    printf("brazo de la IK y mano propia (se mueven hasta 140 mm por escena): desvio respecto a la ultima pose %.2f mm\n", hand * 1000.0);
    CHECK(hand * 1000.0 < 0.5, "brazo de la IK / mano propia: %.1f mm por detras de la ultima pose", hand * 1000.0);
    CHECK(used > 3000, "cobertura (%d)", used);
    CHECK(saw0 * 1000.0 > 90.0, "la referencia sin corregir debe traer el diente de sierra (%.1f mm)", saw0 * 1000.0);
    CHECK(saw1 * 1000.0 < 0.5, "torso: diente de sierra de %.2f mm respecto a la cabeza", saw1 * 1000.0);
    CHECK(step1 * 1000.0 < 50.0 && step1 < step0 / 3.0, "continuidad: paso de %.1f mm por fotograma de render (sin: %.1f)", step1 * 1000.0, step0 * 1000.0);   /* la pierna recorre como mucho ~150 mm por escena = ~42 mm por fotograma */
    CHECK(alt * 1000.0 < 0.5, "dibujando la escena anterior la pieza cae en el mismo sitio (%.3f mm)", alt * 1000.0);
    /* Piezas que siguen al mando: no dependen de la fluidez (m NULL = el renderer no corrige), de que haya dos poses ni de la escena concreta, y las de la mano (y el objeto que cuelga de ella) se mueven con el
     * movimiento rígido del marco de la mano desde la escena hasta ahora, como el arma. */
    {
        static VblRing r1; static VblPose p1;
        static const float I4[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const uint32_t HELD = 0x80111111u;                    /* la instancia 2 de la escena (1 pieza) hace de objeto en la mano derecha */
        VblHand hc[2], now[2];
        memset(hc, 0, sizeof hc); memset(now, 0, sizeof now);
        hc[1].ok = 1; hc[1].p[0] = 0.2f; hc[1].p[1] = 1.0f; hc[1].p[2] = -0.3f; hc[1].q[3] = 1.0f;
        float ang = 0.35f;                                    /* 20 grados sobre Y y 15 cm */
        now[1].ok = 1; now[1].p[0] = 0.2f + 0.10f; now[1].p[1] = 1.0f; now[1].p[2] = -0.3f + 0.11f; now[1].q[1] = sinf(ang / 2); now[1].q[3] = cosf(ang / 2);
        CHECK(vbl_capture(&p1, cur, &map, live, HELD, 1, hc) == NP + NH + NO, "captura con objeto en la mano");
        CHECK(p1.hs[NP + 1] == 2 && p1.hs[NP] == 0 && p1.hs[NP + NH] == 2 && p1.hs[0] == 0 && p1.live[NP + NH] == 1, "solo el marco de la mano (no el anillo) y el objeto siguen a la mano derecha: %d %d %d", p1.hs[NP + 1], p1.hs[NP], p1.hs[NP + NH]);
        vbl_push(&r1, &p1);
        CHECK(r1.n == 1, "una sola pose");
        /* sin compensar (now NULL o sin seguimiento) y sin corrección: la última pose tal cual, aunque haya UNA sola pose y no haya m */
        double q0[3], q1[3];
        const float V[3] = {120.0f, -40.0f, 75.0f};          /* punto local de la pieza */
        struct { uint32_t ii, kk; } pc[3] = {{1, 1}, {2, 0}, {0, 3}};
        const NsXf *wl[3] = {&cur->piece[NP + 1].world, &cur->piece[NP + NH].world, &cur->piece[3].world};
        for (int q = 0; q < 3; q++) {
            NsXf y;
            CHECK(vbl_piece(&r1, cur, pc[q].ii, pc[q].kk, 0.5f, NULL, NULL, &y) == 1, "pieza viva %d sin m, una pose y sin compensar", q);
            double a0[3], a1[3], b0[3], b1[3];
            shown(cur, wl[q], I4, a0); shown(cur, &y, I4, a1);
            for (int i = 0; i < 3; i++) q0[i] = a0[i], q1[i] = a1[i];
            CHECK(fabs(q0[0] - q1[0]) + fabs(q0[1] - q1[1]) + fabs(q0[2] - q1[2]) < 1e-5, "pieza viva %d: sin m queda en su pose de la escena (%.6f)", q, fabs(q0[0] - q1[0]));
            NsXf z; CHECK(vbl_piece(&r1, cur, pc[q].ii, pc[q].kk, 0.5f, NULL, now, &z) == 1, "pieza %d con la mano de ahora", q);
            /* esperado (solo la mano derecha y el objeto): D (Rn Rc^T) aplicada a la pieza en el espacio del usuario; el torso (0, 3) y el anillo no se mueven */
            const float *tv = cur->to_vr;
            float lp[3] = {V[0], V[1], V[2]}, wp[3], up[3], sp[3];
            for (int i = 0; i < 3; i++) wp[i] = wl[q]->m[4 * i] * lp[0] + wl[q]->m[4 * i + 1] * lp[1] + wl[q]->m[4 * i + 2] * lp[2] + wl[q]->m[4 * i + 3];
            for (int i = 0; i < 3; i++) up[i] = tv[4 * i] * wp[0] + tv[4 * i + 1] * wp[1] + tv[4 * i + 2] * wp[2] + tv[4 * i + 3];
            float cs = cosf(ang), sn = sinf(ang), Dm[9] = {cs, 0, sn, 0, 1, 0, -sn, 0, cs}, pd[3];
            for (int i = 0; i < 3; i++) pd[i] = now[1].p[i] - (Dm[3 * i] * hc[1].p[0] + Dm[3 * i + 1] * hc[1].p[1] + Dm[3 * i + 2] * hc[1].p[2]);
            int moved = pc[q].kk == 1 || pc[q].ii == 2;
            for (int i = 0; i < 3; i++) sp[i] = moved ? Dm[3 * i] * up[0] + Dm[3 * i + 1] * up[1] + Dm[3 * i + 2] * up[2] + pd[i] : up[i];
            for (int i = 0; i < 3; i++) {
                float zp = 0; for (int c = 0; c < 3; c++) zp += cur->to_vr[4 * i + c] * (z.m[4 * c] * lp[0] + z.m[4 * c + 1] * lp[1] + z.m[4 * c + 2] * lp[2] + z.m[4 * c + 3]);
                zp += cur->to_vr[4 * i + 3];
                b0[i] = zp; b1[i] = sp[i];
            }
            CHECK(fabs(b0[0] - b1[0]) + fabs(b0[1] - b1[1]) + fabs(b0[2] - b1[2]) < 1e-4, "pieza %d: movimiento rígido de la mano (punto local, error %.5f m)", q, fabs(b0[0] - b1[0]) + fabs(b0[1] - b1[1]) + fabs(b0[2] - b1[2]));
        }
        /* la compensación es la del arma: un salto incoherente (recentrado) no se compensa */
        VblHand far_now[2] = {{0}, {1, {5.0f, 1.0f, 0.0f}, {0, 0, 0, 1}}}; NsXf y, z;
        CHECK(vbl_piece(&r1, cur, 1, 1, 0.5f, NULL, NULL, &y) == 1 && vbl_piece(&r1, cur, 1, 1, 0.5f, NULL, far_now, &z) == 1 && memcmp(&y, &z, sizeof y) == 0, "salto de la mano mayor de 0,35 m: sin compensar");
        VblHand lost[2] = {{0}, {0}};
        CHECK(vbl_piece(&r1, cur, 1, 1, 0.5f, NULL, lost, &z) == 1 && memcmp(&y, &z, sizeof y) == 0, "mano sin seguimiento: sin compensar");
        /* lo que no sigue al mando sigue necesitando fluidez y dos poses */
        CHECK(vbl_piece(&r1, cur, 0, 0, 0.5f, NULL, now, &z) == 0 && vbl_piece(&r1, cur, 0, 0, 0.5f, I4, now, &z) == 0, "torso: sin m o con una sola pose no se toca");
        /* con el renderer corrigiendo (m != identidad) la pieza viva se dibuja en la última pose del mando: m la deja en el sitio de la escena */
        const float c2 = cosf(0.2f), s2 = sinf(0.2f); float mm[16] = {c2, 0, s2, 0, 0, 1, 0, 0, -s2, 0, c2, 0, 0.3f, -0.1f, 0.05f, 1};   /* giro y desplazamiento rígidos */
        NsXf w2; double c0[3], c1[3];
        CHECK(vbl_piece(&r1, cur, 1, 1, 0.5f, mm, NULL, &w2) == 1, "pieza viva con corrección");
        shown(cur, &w2, mm, c1); shown(cur, &cur->piece[NP + 1].world, I4, c0);
        CHECK(fabs(c0[0] - c1[0]) + fabs(c0[1] - c1[1]) + fabs(c0[2] - c1[2]) < 1e-4, "con corrección la pieza viva queda en su pose (%.5f)", fabs(c0[0] - c1[0]));
        /* la escena debe ser una del anillo, aunque no sea la última */
        NsScene farsc = *cur; farsc.t_ns += 7;
        CHECK(vbl_piece(&r1, &farsc, 1, 1, 0.5f, NULL, NULL, &z) == 0, "pieza viva de una escena que no es del anillo: no se toca");
    }
    /* escena que no es ni la última ni la anterior, hueco largo y salto (recentrado): sin matriz */
    NsScene far = *cur; far.t_ns += 123;
    CHECK(vbl_piece(&ring, &far, 0, 1, 0.5f, (const float[16]){1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, NULL, &x) == 0, "escena desconocida");
    VblPose late = pose; late.id += 2 * VBL_GAP_NS; vbl_push(&ring, &late);
    CHECK(ring.n == 1, "hueco largo: se empieza de cero (%d)", ring.n);
    VblPose jump = late; jump.id += 40000000ull; jump.u[0].m[3] += 1000.0f; vbl_push(&ring, &jump);
    CHECK(ring.n == 1, "salto de 1 m: se empieza de cero (%d)", ring.n);
    vbl_push(&ring, NULL);
    CHECK(ring.n == 0, "escena sin jugador: vacio");
    printf(fails ? "FALLOS: %d\n" : "OK: vr_bodylive\n", fails);
    return fails ? 1 : 0;
}
