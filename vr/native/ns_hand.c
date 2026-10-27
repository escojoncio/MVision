/* ns_hand: ver ns_hand.h. */
#include "ns_hand.h"
#include <math.h>
#include <string.h>
#include "ns_extract.h"
#include "vr_hand_skin.h"
#include "vr_hand_pose.h"

_Static_assert(NS_HAND_PIECES == VRHA_NP + 2, "NS_HAND_PIECES = anillo + transición + piezas de la mano");

#define NS_HAND_QC 32.0f            /* pasos de cuantización de los cierres y de la separación del pulgar */
#define NS_HAND_LR 5                /* anillos de la transición antebrazo -> mano (0 = extremo del antebrazo, NS_HAND_LR - 1 = borde de la muñeca de la mano) */
#define NS_HAND_FMAX 16             /* vértices del extremo del antebrazo como mucho */
#define NS_HAND_LV (NS_HAND_FMAX + (NS_HAND_LR - 1) * VRHA_NCUFF)
#define NS_HAND_LT (2 * (NS_HAND_LR - 2) * VRHA_NCUFF + NS_HAND_FMAX + VRHA_NCUFF)
_Static_assert(NS_HAND_LV <= 256, "la transición cabe en una malla");
typedef struct {
    int valid;
    uint64_t glove_id;           /* malla de la manopla con la que se construyeron las caras */
    int textured;
    uint32_t last_build;         /* fotograma (S.st.frames) de la última construcción */
    uint32_t next_try;           /* no volver a intentar construir antes de este fotograma (tras un fallo) */
    NsFace f[VRHA_NT];           /* caras (índices locales a su pieza, UV de la manopla): no cambian con la pose */
    NsHandTex plain;             /* texel del color del guante (caras sin la textura de la palma) */
    /* transición con el antebrazo del motor (ver ns_hand.h): anillo = los vértices del extremo del antebrazo (copia exacta, en su marco); malla de la transición en el marco de la mano */
    int seam;
    uint64_t seam_fore;          /* malla del antebrazo y muñeca (en su marco, enteros) con que se calculó */
    int32_t seam_d[3];
    int nf;                      /* vértices del extremo del antebrazo */
    float poly[NS_HAND_FMAX][3]; /* esos vértices en el marco de la mano en reposo (local del antebrazo - muñeca) */
    float slot_p[VRHA_NCUFF][3]; /* punto del contorno del antebrazo en la dirección de cada vértice del borde de la muñeca (reposo) */
    int nlt;
    uint8_t lt[NS_HAND_LT][3];   /* triángulos de la transición (índices de la malla de la transición) */
    NsVert rv[NS_HAND_FMAX];
    NsNorm rn[1];
    NsFace rf[1];
    NsMesh ring;
    NsVert lv[NS_HAND_LV];
    NsNorm ln[NS_HAND_LV];
    NsFace lf[NS_HAND_LT];
    NsMesh loft;
    int have_pose;
    int32_t key[6];              /* pose cuantizada de las mallas actuales: 5 cierres y separación */
    float pos[VRHA_NV][3], nrm[VRHA_NV][3];
    NsVert v[VRHA_NV];
    NsNorm n[VRHA_NV];
    NsMesh mesh[VRHA_NP];
} HandCache;
#define NS_HAND_REBUILD_FRAMES 30u   /* como mucho una (re)construcción cada N fotogramas: una manopla que cambie de id a cada fotograma no hace crecer el almacén */

static struct { HandCache c[2]; NsHandStats st; } S;   /* índice 0 = izquierda, 1 = derecha */

void ns_hand_reset(void) { memset(&S, 0, sizeof S); }
void ns_hand_stats(NsHandStats *o) { if (o) *o = S.st; }

/* ---------------------------------------------------------------------------------------------------------------- clasificación de texturas */
static void face_info(const NsMesh *m, const NsFace *f, float c[3], float out[3], float *area) {
    float p[4][3];
    for (int k = 0; k < 4; k++) { const NsVert *v = &m->verts[f->v[k] < m->n_verts ? f->v[k] : 0]; p[k][0] = v->x; p[k][1] = v->y; p[k][2] = v->z; }
    for (int a = 0; a < 3; a++) c[a] = 0.25f * (p[0][a] + p[1][a] + p[2][a] + p[3][a]);
    float e1[3], e2[3], n[3];
    for (int a = 0; a < 3; a++) { e1[a] = p[1][a] - p[0][a]; e2[a] = p[3][a] - p[0][a]; }
    n[0] = e1[1] * e2[2] - e1[2] * e2[1]; n[1] = e1[2] * e2[0] - e1[0] * e2[2]; n[2] = e1[0] * e2[1] - e1[1] * e2[0];
    float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    *area = 0.5f * l;
    for (int a = 0; a < 3; a++) out[a] = l > 1e-6f ? -n[a] / l : 0.0f;   /* cross(B-A, D-A) es INTERIOR: la normal hacia fuera es la opuesta */
}

int ns_hand_classify(const NsMesh *g, int side, NsHandTex tex[4]) {
    memset(tex, 0, 4 * sizeof tex[0]);
    if (!g || !g->verts || !g->faces || !g->n_faces || !g->n_verts) return 0;
    float best[4] = {0, 0, 0, 0};
    float ymin = 1e9f, ymax = -1e9f;
    for (unsigned i = 0; i < g->n_verts; i++) { if (g->verts[i].y < ymin) ymin = g->verts[i].y; if (g->verts[i].y > ymax) ymax = g->verts[i].y; }
    float yfing = ymin + 0.5f * (ymax - ymin);          /* mitad de dedos (hacia -Y) */
    for (unsigned i = 0; i < g->n_faces; i++) {
        const NsFace *f = &g->faces[i];
        if (!f->texid) continue;
        float c[3], o[3], a;
        face_info(g, f, c, o, &a);
        if (a < 1.0f) continue;
        float px = o[0] * (float)side;                  /* >0: mira hacia la palma */
        int role = -1;
        float score = a;
        if (o[1] < -0.6f && c[1] < yfing) role = NS_HT_JOINT;           /* extremo de los dedos */
        else if (o[1] > 0.6f) continue;                                  /* extremo de la muñeca: no se ve */
        else if (px > 0.5f) role = NS_HT_PALM;
        else if (px < -0.5f) role = NS_HT_BACK;
        else role = NS_HT_FINGER;                                        /* costados */
        if (score > best[role]) {
            best[role] = score;
            NsHandTex *t = &tex[role];
            t->ok = 1; t->tpage = f->tpage; t->clut = f->clut; t->texid = f->texid;
            memcpy(t->u, f->u, 4); memcpy(t->t, f->t, 4);
        }
    }
    int found = 0;
    for (int r = 0; r < 4; r++) found += tex[r].ok;
    if (!found) return 0;
    /* los que faltan toman el más parecido */
    static const int pref[4][3] = {{NS_HT_BACK, NS_HT_FINGER, NS_HT_JOINT}, {NS_HT_PALM, NS_HT_FINGER, NS_HT_JOINT}, {NS_HT_BACK, NS_HT_PALM, NS_HT_JOINT}, {NS_HT_FINGER, NS_HT_BACK, NS_HT_PALM}};
    NsHandTex src[4];
    memcpy(src, tex, sizeof src);
    for (int r = 0; r < 4; r++) if (!src[r].ok) for (int k = 0; k < 3; k++) if (src[pref[r][k]].ok) { tex[r] = src[pref[r][k]]; tex[r].ok = 2; break; }   /* 2 = prestada */
    return found;
}

/* Interpolación bilineal en el cuadrilátero A,B,C,D de texels con (s,t) 0..255 (A->B por s, A->D por t); con un recorte del 4 % para no leer vecinos. */
static void map_uv(const NsHandTex *tx, uint8_t su, uint8_t sv, uint8_t *u, uint8_t *t) {
    float s = 0.04f + 0.92f * (float)su / 255.0f, v = 0.04f + 0.92f * (float)sv / 255.0f;
    float w[4] = {(1 - s) * (1 - v), s * (1 - v), s * v, (1 - s) * v};
    float uu = 0, tt = 0;
    for (int k = 0; k < 4; k++) { uu += w[k] * tx->u[k]; tt += w[k] * tx->t[k]; }
    *u = (uint8_t)ns_clampf(floorf(uu + 0.5f), 0.0f, 255.0f); *t = (uint8_t)ns_clampf(floorf(tt + 0.5f), 0.0f, 255.0f);
}

/* ---------------------------------------------------------------------------------------------------------------- construcción */
static int dominant_joint(int pv0, const uint8_t *tr) {
    float w[VRHA_NJ] = {0};
    for (int k = 0; k < 3; k++) for (int q = 0; q < 4; q++) w[vrha_joint[pv0 + tr[k]][q]] += vrha_weight[pv0 + tr[k]][q];
    int d = 0;
    for (int j = 1; j < VRHA_NJ; j++) if (w[j] > w[d]) d = j;
    return d;
}
/* Caras de la mano. Orden de los vértices: de antihorario hacia fuera (asset) a la convención del motor (cross(B-A, D-A) interior); la izquierda es un espejo y ya invierte el sentido.
 * Estilo PS1 como el brazo: la cara PALMAR desde la muñeca hasta la base de los dedos (muñeca y metacarpos) lleva la textura de la palma de la manopla del juego, proyectada en plano
 * (ancho de la palma -> u, largo -> v); el resto (dedos, pulgar, dorso, costados) es de un solo color: las cuatro esquinas en el texel central de la textura de los dedos de la manopla (si no, la del dorso)
 * (el color del guante), sombreado por la luz. */
static int build(HandCache *c, int side, const NsMesh *g) {
    NsHandTex tex[4];
    c->textured = ns_hand_classify(g, side, tex) > 0;
    const NsHandTex *plain = tex[NS_HT_FINGER].ok ? &tex[NS_HT_FINGER] : tex[NS_HT_BACK].ok ? &tex[NS_HT_BACK] : &tex[NS_HT_PALM];   /* los dedos de la manopla: el color del guante */
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};    /* caja de la palma (vértices de las caras texturizadas) */
    for (int p = 0; p < VRHA_NP; p++) for (int t = vrha_piece[p].t0; t < vrha_piece[p].t0 + vrha_piece[p].nt; t++) {
        const int dj = dominant_joint(vrha_piece[p].v0, vrha_tri[t]);
        if (vrha_role[t] != VRHA_ROLE_PALM || !(dj == 0 || (dj >= VRHS_INDEX_MC && (dj - VRHS_INDEX_MC) % 5 == 0))) continue;
        for (int k = 0; k < 3; k++) for (int a = 0; a < 3; a++) { const float v = vrha_pos[vrha_piece[p].v0 + vrha_tri[t][k]][a]; if (v < lo[a]) lo[a] = v; if (v > hi[a]) hi[a] = v; }
    }
    for (int p = 0; p < VRHA_NP; p++) {
        const VrHaPiece *pc = &vrha_piece[p];
        for (int t = pc->t0; t < pc->t0 + pc->nt; t++) {
            NsFace *f = &c->f[t];
            memset(f, 0, sizeof *f);
            const uint8_t *tr = vrha_tri[t];
            const uint8_t o[4] = {tr[0], side > 0 ? tr[2] : tr[1], side > 0 ? tr[1] : tr[2], side > 0 ? tr[1] : tr[2]};
            const int dj = dominant_joint(pc->v0, tr);
            const int front = vrha_role[t] == VRHA_ROLE_PALM && (dj == 0 || (dj >= VRHS_INDEX_MC && (dj - VRHS_INDEX_MC) % 5 == 0));
            const NsHandTex *tx = front ? &tex[NS_HT_PALM] : plain;
            for (int q = 0; q < 4; q++) {
                f->v[q] = o[q]; f->n[q] = o[q];
                if (!tx->ok) continue;
                if (!front) { map_uv(tx, 128, 128, &f->u[q], &f->t[q]); continue; }
                const float *v = vrha_pos[pc->v0 + o[q]];
                const float su = (v[2] - lo[2]) / (hi[2] - lo[2]), sv = (v[1] - lo[1]) / (hi[1] - lo[1]);
                map_uv(tx, (uint8_t)ns_clampf(su * 255.0f + 0.5f, 0.0f, 255.0f), (uint8_t)ns_clampf(sv * 255.0f + 0.5f, 0.0f, 255.0f), &f->u[q], &f->t[q]);
            }
            f->flags = NS_FF_TRI;
            if (tx->ok) { f->tpage = tx->tpage; f->clut = tx->clut; f->texid = tx->texid; }
        }
    }
    c->plain = *plain;
    c->glove_id = g->id;
    c->have_pose = 0;
    c->seam = 0;
    c->valid = 1;
    return 1;
}

/* Transición con el antebrazo: topología (cuando cambia la malla del antebrazo o la muñeca `d` en su marco; en reposo los ejes de antebrazo y mano coinciden).
 *   anillo 0  = el lazo abierto del extremo del antebrazo `fm` (aristas de una sola cara entre vértices no soldados): MISMOS vértices, copiados tal cual a la malla del anillo (pieza 0, con la
 *               matriz del antebrazo) y soldados desde la transición -> posiciones idénticas a las del antebrazo;
 *   anillos 1..NS_HAND_LR-1 = una ranura por vértice del borde de la muñeca de la mano (vrha_cuff, en orden); el último ES ese borde (mismas coordenadas enteras, mismo marco).
 * Banda 0 -> 1: triangulación por ángulo entre los dos lazos (sin vértices en T); bandas siguientes: cuadriláteros. El sentido de cada triángulo se fija con la geometría de reposo. */
static float wrapf(float a) { while (a < 0.0f) a += 6.2831853f; while (a >= 6.2831853f) a -= 6.2831853f; return a; }
static int build_seam(HandCache *c, int side, const NsMesh *fm, const int32_t d[3]) {
    c->seam = 0;
    if (!fm || !fm->verts || !fm->faces || !fm->n_verts || fm->n_verts > 256 || fm->n_faces > 256) return 0;
    /* aristas de una sola cara entre vértices no soldados */
    static uint8_t ea[1024], eb[1024]; static uint16_t ec[1024];
    int ne = 0;
    for (unsigned f = 0; f < fm->n_faces; f++) {
        const NsFace *F = &fm->faces[f];
        const int tri = (F->flags & NS_FF_TRI) || F->v[2] == F->v[3];
        const uint8_t loop[4] = {F->v[0], F->v[1], F->v[2], F->v[3]};
        const int n = tri ? 3 : 4;
        for (int k = 0; k < n; k++) {
            uint8_t a = loop[k], b = loop[(k + 1) % n];
            if (a == b || a >= fm->n_verts || b >= fm->n_verts) continue;
            if (a > b) { uint8_t t = a; a = b; b = t; }
            int e = 0;
            while (e < ne && !(ea[e] == a && eb[e] == b)) e++;
            if (e == ne) { if (ne >= 1024) return 0; ea[ne] = a; eb[ne] = b; ec[ne] = 0; ne++; }
            ec[e]++;
        }
    }
    int lp[NS_HAND_FMAX], nl = 0, best_n = 0, best[NS_HAND_FMAX];
    float best_t = -1e30f;
    static uint8_t used[1024];
    memset(used, 0, sizeof used);
    for (int e0 = 0; e0 < ne; e0++) {   /* cada lazo de aristas abiertas entre vértices no soldados; se queda el más alejado del codo */
        if (used[e0] || ec[e0] != 1 || fm->verts[ea[e0]].weld != 0xFF || fm->verts[eb[e0]].weld != 0xFF) continue;
        nl = 0; lp[nl++] = ea[e0]; used[e0] = 1;
        int cur = eb[e0], ok = 1;
        while (cur != lp[0]) {
            if (nl >= NS_HAND_FMAX) { ok = 0; break; }
            lp[nl++] = cur;
            int nx = -1;
            for (int e = 0; e < ne; e++) if (!used[e] && ec[e] == 1 && (ea[e] == cur || eb[e] == cur) && fm->verts[ea[e]].weld == 0xFF && fm->verts[eb[e]].weld == 0xFF) { nx = e; break; }
            if (nx < 0) { ok = 0; break; }
            used[nx] = 1; cur = ea[nx] == cur ? eb[nx] : ea[nx];
        }
        if (!ok || nl < 3) continue;
        float t = 0;
        for (int i = 0; i < nl; i++) { const NsVert *v = &fm->verts[lp[i]]; t += sqrtf((float)v->x * v->x + (float)v->y * v->y + (float)v->z * v->z); }
        if (t / (float)nl > best_t) { best_t = t / (float)nl; best_n = nl; memcpy(best, lp, sizeof lp); }
    }
    if (best_n < 3) return 0;
    /* borde de la muñeca de la mano en reposo */
    float xf[VRHS_J][12];
    const float zero[5] = {0, 0, 0, 0, 0};
    vrhs_pose(zero, 0.0f, side, xf);
    vrhs_skin(xf, side, c->pos, NULL);
    const int K = VRHA_NCUFF, nf = best_n;
    float cp[2] = {0, 0}, cc[2] = {0, 0}, ap = 0, ac = 0;
    for (int i = 0; i < nf; i++) {
        const NsVert *v = &fm->verts[best[i]];
        c->poly[i][0] = (float)(v->x - d[0]); c->poly[i][1] = (float)(v->y - d[1]); c->poly[i][2] = (float)(v->z - d[2]);
        cp[0] += c->poly[i][0] / (float)nf; cp[1] += c->poly[i][2] / (float)nf;
    }
    for (int j = 0; j < K; j++) { cc[0] += c->pos[vrha_cuff[j]][0] / (float)K; cc[1] += c->pos[vrha_cuff[j]][2] / (float)K; }
    for (int i = 0; i < nf; i++) { const float *a = c->poly[i], *b = c->poly[(i + 1) % nf]; ap += (a[0] - cp[0]) * (b[2] - cp[1]) - (b[0] - cp[0]) * (a[2] - cp[1]); }
    for (int j = 0; j < K; j++) { const float *a = c->pos[vrha_cuff[j]], *b = c->pos[vrha_cuff[(j + 1) % K]]; ac += (a[0] - cc[0]) * (b[2] - cc[1]) - (b[0] - cc[0]) * (a[2] - cc[1]); }
    if (ap < 0) for (int i = 0; i < nf / 2; i++) {   /* mismo sentido de giro que el borde de la mano */
        int t = best[i]; best[i] = best[nf - 1 - i]; best[nf - 1 - i] = t;
        for (int a = 0; a < 3; a++) { float u = c->poly[i][a]; c->poly[i][a] = c->poly[nf - 1 - i][a]; c->poly[nf - 1 - i][a] = u; }
    }
    const float dirc = ac < 0 ? -1.0f : 1.0f;   /* el borde de la mano gira en sentido dirc (en xz); el anillo se ordenó en sentido positivo: se recorre con dirc */
    float al[NS_HAND_FMAX], be[VRHA_NCUFF];
    for (int i = 0; i < nf; i++) al[i] = atan2f(c->poly[i][2] - cp[1], c->poly[i][0] - cp[0]);
    for (int j = 0; j < K; j++) be[j] = atan2f(c->pos[vrha_cuff[j]][2] - cc[1], c->pos[vrha_cuff[j]][0] - cc[0]);
    /* punto del contorno del antebrazo en la dirección de cada vértice del borde */
    for (int j = 0; j < K; j++) {
        const float dx = cosf(be[j]), dz = sinf(be[j]);
        float bt = 1e30f; float P[3] = {cp[0], c->poly[0][1], cp[1]};
        for (int e = 0; e < nf; e++) {
            const float *A = c->poly[e], *B = c->poly[(e + 1) % nf];
            const float ex = B[0] - A[0], ez = B[2] - A[2], den = dx * ez - dz * ex;
            if (fabsf(den) < 1e-6f) continue;
            const float ax = A[0] - cp[0], az = A[2] - cp[1], tr = (ax * ez - az * ex) / den, u = (ax * dz - az * dx) / den;
            if (tr > 0.0f && u >= -1e-4f && u <= 1.0001f && tr < bt) { bt = tr; P[0] = A[0] + u * ex; P[1] = A[1] + u * (B[1] - A[1]); P[2] = A[2] + u * ez; }
        }
        memcpy(c->slot_p[j], P, sizeof P);
    }
    /* banda 0: fusión por ángulo de los dos lazos, recorridos en el sentido del borde de la mano (dirc) a partir del vértice del anillo más cercano a la ranura 0 */
    int i0 = 0;
    float bd = 1e9f;
    for (int i = 0; i < nf; i++) { float dd = fabsf(remainderf(al[i] - be[0], 6.2831853f)); if (dd < bd) { bd = dd; i0 = i; } }
    const int sgn_ring = dirc > 0 ? 1 : nf - 1;     /* paso en el anillo (ordenado en sentido positivo) */
    int nt = 0, ia = 0, jb = 0;
    #define RI(i) ((i0 + (i) * sgn_ring) % nf)
    #define BI(j) (nf + ((j) % K))
    while (ia < nf || jb < K) {
        const float na = ia < nf ? wrapf(dirc * (al[RI(ia + 1)] - be[0])) + (ia + 1 == nf ? 6.2831853f : 0.0f) : 1e9f;
        const float nb = jb < K ? wrapf(dirc * (be[(jb + 1) % K] - be[0])) + (jb + 1 == K ? 6.2831853f : 0.0f) : 1e9f;
        if (ia < nf && (jb >= K || na < nb)) { c->lt[nt][0] = (uint8_t)RI(ia); c->lt[nt][1] = (uint8_t)BI(jb); c->lt[nt][2] = (uint8_t)RI(ia + 1); ia++; }
        else { c->lt[nt][0] = (uint8_t)RI(ia); c->lt[nt][1] = (uint8_t)BI(jb); c->lt[nt][2] = (uint8_t)BI(jb + 1); jb++; }
        nt++;
    }
    #undef RI
    #undef BI
    for (int r = 1; r < NS_HAND_LR - 1; r++) for (int j = 0; j < K; j++) {
        const int a = nf + (r - 1) * K + j, b = nf + (r - 1) * K + (j + 1) % K, a2 = a + K, b2 = b + K;
        c->lt[nt][0] = (uint8_t)a; c->lt[nt][1] = (uint8_t)b; c->lt[nt][2] = (uint8_t)b2; nt++;
        c->lt[nt][0] = (uint8_t)a; c->lt[nt][1] = (uint8_t)b2; c->lt[nt][2] = (uint8_t)a2; nt++;
    }
    c->nlt = nt;
    /* sentido: normal (cross(B-A, C-A)) hacia DENTRO, como el motor, con la geometría de reposo */
    float rest[NS_HAND_LV][3], rs[NS_HAND_LV];
    for (int i = 0; i < nf; i++) { memcpy(rest[i], c->poly[i], sizeof rest[i]); rs[i] = 1.0f; }
    for (int r = 1; r < NS_HAND_LR; r++) {
        const float s = 1.0f - (float)r / (float)(NS_HAND_LR - 1);
        for (int j = 0; j < K; j++) { const int v = nf + (r - 1) * K + j; for (int a = 0; a < 3; a++) rest[v][a] = (1.0f - s) * c->pos[vrha_cuff[j]][a] + s * c->slot_p[j][a]; rs[v] = s; }
    }
    for (int t = 0; t < nt; t++) {
        const float *A = rest[c->lt[t][0]], *B = rest[c->lt[t][1]], *C = rest[c->lt[t][2]];
        const float e1[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]}, e2[3] = {C[0] - A[0], C[1] - A[1], C[2] - A[2]};
        const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const float s = (rs[c->lt[t][0]] + rs[c->lt[t][1]] + rs[c->lt[t][2]]) / 3.0f;
        const float ox = (A[0] + B[0] + C[0]) / 3.0f - ((1.0f - s) * cc[0] + s * cp[0]), oz = (A[2] + B[2] + C[2]) / 3.0f - ((1.0f - s) * cc[1] + s * cp[1]);
        if (n[0] * ox + n[2] * oz > 0.0f) { uint8_t u = c->lt[t][1]; c->lt[t][1] = c->lt[t][2]; c->lt[t][2] = u; }
    }
    /* anillo (pieza 0): los vértices del extremo del antebrazo tal cual */
    for (int i = 0; i < nf; i++) { c->rv[i] = fm->verts[best[i]]; c->rv[i].pad = 0; }
    memset(&c->ring, 0, sizeof c->ring);
    memset(c->rf, 0, sizeof c->rf);
    c->rf[0].flags = NS_FF_TRI;                 /* una cara degenerada: el anillo no se dibuja, solo presta sus vértices a las soldaduras */
    c->rn[0].x = 0; c->rn[0].y = 4096; c->rn[0].z = 0; c->rn[0].pad = 0;
    c->ring.n_verts = (uint16_t)nf; c->ring.n_norms = 1; c->ring.n_faces = 1;
    c->ring.verts = c->rv; c->ring.norms = c->rn; c->ring.faces = c->rf;
    for (int a = 0; a < 3; a++) { c->ring.bbox[a] = 32767; c->ring.bbox[3 + a] = -32768; }
    for (int i = 0; i < nf; i++) {
        const int16_t q[3] = {c->rv[i].x, c->rv[i].y, c->rv[i].z};
        for (int a = 0; a < 3; a++) { if (q[a] < c->ring.bbox[a]) c->ring.bbox[a] = q[a]; if (q[a] > c->ring.bbox[3 + a]) c->ring.bbox[3 + a] = q[a]; }
    }
    c->ring.id = ns_mesh_content_hash(&c->ring);
    /* caras de la transición: color del guante */
    for (int t = 0; t < nt; t++) {
        NsFace *F = &c->lf[t];
        memset(F, 0, sizeof *F);
        F->v[0] = F->n[0] = c->lt[t][0]; F->v[1] = F->n[1] = c->lt[t][1]; F->v[2] = F->n[2] = c->lt[t][2]; F->v[3] = F->n[3] = c->lt[t][2];
        F->flags = NS_FF_TRI;
        if (c->plain.ok) { for (int q = 0; q < 4; q++) map_uv(&c->plain, 128, 128, &F->u[q], &F->t[q]); F->tpage = c->plain.tpage; F->clut = c->plain.clut; F->texid = c->plain.texid; }
    }
    c->nf = nf;
    c->seam_fore = fm->id; memcpy(c->seam_d, d, sizeof c->seam_d);
    c->seam = 1;
    return 1;
}

static void quat_of(const float m[9], float q[4]) {
    const float tr = m[0] + m[4] + m[8];
    if (tr > 0.0f) { float s = sqrtf(tr + 1.0f) * 2.0f; q[3] = 0.25f * s; q[0] = (m[7] - m[5]) / s; q[1] = (m[2] - m[6]) / s; q[2] = (m[3] - m[1]) / s; }
    else if (m[0] > m[4] && m[0] > m[8]) { float s = sqrtf(1.0f + m[0] - m[4] - m[8]) * 2.0f; q[3] = (m[7] - m[5]) / s; q[0] = 0.25f * s; q[1] = (m[1] + m[3]) / s; q[2] = (m[2] + m[6]) / s; }
    else if (m[4] > m[8]) { float s = sqrtf(1.0f + m[4] - m[0] - m[8]) * 2.0f; q[3] = (m[2] - m[6]) / s; q[0] = (m[1] + m[3]) / s; q[1] = 0.25f * s; q[2] = (m[5] + m[7]) / s; }
    else { float s = sqrtf(1.0f + m[8] - m[0] - m[4]) * 2.0f; q[3] = (m[3] - m[1]) / s; q[0] = (m[2] + m[6]) / s; q[1] = (m[5] + m[7]) / s; q[2] = 0.25f * s; }
}
static void mat_of(const float q[4], float m[9]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y), 2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x), 2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)};
    memcpy(m, r, sizeof r);
}
/* Malla de la transición en el marco de la mano para la posición relativa actual antebrazo -> mano: el anillo s (1 = antebrazo, 0 = mano) lleva la parte s del movimiento del antebrazo
 * respecto a la mano (giro interpolado por cuaternión: el giro y la flexión se reparten a lo largo de la transición, sin estrangularse; escala del estirado interpolada). */
static void loft_mesh(HandCache *c, const NsXf *F, const NsXf *H, const NsMesh *g) {
    const float *f = F->m, *h = H->m;
    float A[9], b[3], Q[9], S[9], q[4], Ad[3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) A[3 * i + j] = h[i] * f[j] + h[4 + i] * f[4 + j] + h[8 + i] * f[8 + j];   /* R_H^T R_F */
    for (int i = 0; i < 3; i++) b[i] = h[i] * (f[3] - h[3]) + h[4 + i] * (f[7] - h[7]) + h[8 + i] * (f[11] - h[11]);
    {   /* parte de giro de A (Gram-Schmidt por columnas) */
        float c0[3] = {A[0], A[3], A[6]}, c1[3] = {A[1], A[4], A[7]}, c2[3];
        float l0 = sqrtf(c0[0] * c0[0] + c0[1] * c0[1] + c0[2] * c0[2]); for (int a = 0; a < 3; a++) c0[a] /= l0;
        float dp = c0[0] * c1[0] + c0[1] * c1[1] + c0[2] * c1[2]; for (int a = 0; a < 3; a++) c1[a] -= dp * c0[a];
        float l1 = sqrtf(c1[0] * c1[0] + c1[1] * c1[1] + c1[2] * c1[2]); for (int a = 0; a < 3; a++) c1[a] /= l1;
        c2[0] = c0[1] * c1[2] - c0[2] * c1[1]; c2[1] = c0[2] * c1[0] - c0[0] * c1[2]; c2[2] = c0[0] * c1[1] - c0[1] * c1[0];
        for (int a = 0; a < 3; a++) { Q[3 * a] = c0[a]; Q[3 * a + 1] = c1[a]; Q[3 * a + 2] = c2[a]; }
    }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) S[3 * i + j] = Q[i] * A[j] + Q[3 + i] * A[3 + j] + Q[6 + i] * A[6 + j];   /* Q^T A */
    quat_of(Q, q);
    if (q[3] < 0) for (int a = 0; a < 4; a++) q[a] = -q[a];
    const float dd[3] = {(float)c->seam_d[0], (float)c->seam_d[1], (float)c->seam_d[2]};
    for (int i = 0; i < 3; i++) Ad[i] = A[3 * i] * dd[0] + A[3 * i + 1] * dd[1] + A[3 * i + 2] * dd[2] + b[i];
    const int nf = c->nf, K = VRHA_NCUFF;
    float P[NS_HAND_LV][3];
    for (int i = 0; i < nf; i++) {   /* anillo 0: soldado al anillo del antebrazo; sus propias coordenadas son su imagen en el marco de la mano */
        const float *x = c->poly[i];
        for (int a = 0; a < 3; a++) P[i][a] = A[3 * a] * x[0] + A[3 * a + 1] * x[1] + A[3 * a + 2] * x[2] + Ad[a];
    }
    for (int r = 1; r < NS_HAND_LR; r++) {
        const float s = 1.0f - (float)r / (float)(NS_HAND_LR - 1);
        const float ang = 2.0f * acosf(q[3] > 1.0f ? 1.0f : q[3]), sn = sinf(0.5f * ang);
        float qs[4] = {0, 0, 0, 1}, Rs[9], M[9];
        if (sn > 1e-6f) { const float hs = 0.5f * ang * s; for (int a = 0; a < 3; a++) qs[a] = q[a] / sn * sinf(hs); qs[3] = cosf(hs); }
        mat_of(qs, Rs);
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { float m = 0; for (int k = 0; k < 3; k++) m += Rs[3 * i + k] * ((k == j ? 1.0f : 0.0f) + s * (S[3 * k + j] - (k == j ? 1.0f : 0.0f))); M[3 * i + j] = m; }
        for (int j = 0; j < K; j++) {
            const int v = nf + (r - 1) * K + j;
            if (r == NS_HAND_LR - 1) { const NsVert *cv = &c->v[vrha_cuff[j]]; P[v][0] = cv->x; P[v][1] = cv->y; P[v][2] = cv->z; continue; }   /* el borde de la mano tal cual */
            const NsVert *cv = &c->v[vrha_cuff[j]];
            const float x[3] = {(1.0f - s) * cv->x + s * c->slot_p[j][0], (1.0f - s) * cv->y + s * c->slot_p[j][1], (1.0f - s) * cv->z + s * c->slot_p[j][2]};
            for (int a = 0; a < 3; a++) P[v][a] = M[3 * a] * x[0] + M[3 * a + 1] * x[1] + M[3 * a + 2] * x[2] + s * Ad[a];
        }
    }
    const int nv = nf + (NS_HAND_LR - 1) * K;
    int16_t mn[3] = {32767, 32767, 32767}, mx[3] = {-32768, -32768, -32768};
    for (int v = 0; v < nv; v++) {
        NsVert *o = &c->lv[v];
        o->x = (int16_t)ns_clampf(floorf(P[v][0] + 0.5f), -32768.0f, 32767.0f); o->y = (int16_t)ns_clampf(floorf(P[v][1] + 0.5f), -32768.0f, 32767.0f); o->z = (int16_t)ns_clampf(floorf(P[v][2] + 0.5f), -32768.0f, 32767.0f);
        o->weld = (uint8_t)(v < nf ? v : 0xFF); o->pad = 0;
        const int16_t qv[3] = {o->x, o->y, o->z};
        for (int a = 0; a < 3; a++) { if (qv[a] < mn[a]) mn[a] = qv[a]; if (qv[a] > mx[a]) mx[a] = qv[a]; }
        /* normal: hacia fuera desde el centro de su anillo (se guarda interior, como el motor) */
        float cen[3] = {0, 0, 0};
        const int r0 = v < nf ? 0 : 1 + (v - nf) / K, n0 = r0 ? K : nf, b0 = r0 ? nf + (r0 - 1) * K : 0;
        for (int k = 0; k < n0; k++) for (int a = 0; a < 3; a++) cen[a] += P[b0 + k][a] / (float)n0;
        float nn[3] = {P[v][0] - cen[0], P[v][1] - cen[1], P[v][2] - cen[2]}, nl = sqrtf(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
        if (!(nl > 1e-6f)) { nn[0] = 1; nn[1] = nn[2] = 0; nl = 1; }
        c->ln[v].x = (int16_t)lrintf(-nn[0] / nl * 4096.0f); c->ln[v].y = (int16_t)lrintf(-nn[1] / nl * 4096.0f); c->ln[v].z = (int16_t)lrintf(-nn[2] / nl * 4096.0f); c->ln[v].pad = 0;
    }
    NsMesh *m = &c->loft;
    memset(m, 0, sizeof *m);
    m->flags = (g->flags & (NS_MF_BLEND | NS_MF_UNLIT | NS_MF_TWOSIDED)) | NS_MF_WELDED;
    m->model_flags = g->model_flags;
    m->depth_bias = g->depth_bias;
    if (m->depth_bias > 0) m->flags |= NS_MF_BIAS_UP;
    m->n_verts = (uint16_t)nv; m->n_norms = (uint16_t)nv; m->n_faces = (uint16_t)c->nlt;
    for (int a = 0; a < 3; a++) { m->bbox[a] = mn[a]; m->bbox[3 + a] = mx[a]; }
    m->verts = c->lv; m->norms = c->ln; m->faces = c->lf;
    m->id = ns_mesh_content_hash(m);
}

/* Mallas de la pose cuantizada `key` (cierres y separación ya cuantizados): skin, normales interiores. */
static void pose_meshes(HandCache *c, int side, const int32_t key[6], const NsMesh *g) {
    float curl[5], xf[VRHS_J][12];
    for (int i = 0; i < 5; i++) curl[i] = (float)key[i] / NS_HAND_QC;
    vrhs_pose(curl, (float)key[5] / NS_HAND_QC, side, xf);
    vrhs_skin(xf, side, c->pos, c->nrm);
    for (int i = 0; i < VRHA_NV; i++) {
        c->v[i].x = (int16_t)ns_clampf(floorf(c->pos[i][0] + 0.5f), -32768.0f, 32767.0f);
        c->v[i].y = (int16_t)ns_clampf(floorf(c->pos[i][1] + 0.5f), -32768.0f, 32767.0f);
        c->v[i].z = (int16_t)ns_clampf(floorf(c->pos[i][2] + 0.5f), -32768.0f, 32767.0f);
        c->v[i].weld = 0xFF; c->v[i].pad = 0;
        c->n[i].x = (int16_t)lrintf(-c->nrm[i][0] * 4096.0f); c->n[i].y = (int16_t)lrintf(-c->nrm[i][1] * 4096.0f); c->n[i].z = (int16_t)lrintf(-c->nrm[i][2] * 4096.0f); c->n[i].pad = 0;
    }
    for (int p = 0; p < VRHA_NP; p++) {
        const VrHaPiece *pc = &vrha_piece[p];
        NsMesh *m = &c->mesh[p];
        memset(m, 0, sizeof *m);
        int16_t mn[3] = {32767, 32767, 32767}, mx[3] = {-32768, -32768, -32768};
        for (int i = pc->v0; i < pc->v0 + pc->nv; i++) {
            const int16_t q[3] = {c->v[i].x, c->v[i].y, c->v[i].z};
            for (int a = 0; a < 3; a++) { if (q[a] < mn[a]) mn[a] = q[a]; if (q[a] > mx[a]) mx[a] = q[a]; }
        }
        m->flags = g->flags & (NS_MF_BLEND | NS_MF_UNLIT | NS_MF_TWOSIDED);
        m->model_flags = g->model_flags;
        m->depth_bias = g->depth_bias;
        if (m->depth_bias > 0) m->flags |= NS_MF_BIAS_UP;
        m->n_verts = pc->nv; m->n_norms = pc->nv; m->n_faces = pc->nt;
        for (int a = 0; a < 3; a++) { m->bbox[a] = mn[a]; m->bbox[3 + a] = mx[a]; }
        m->verts = &c->v[pc->v0]; m->norms = &c->n[pc->v0]; m->faces = &c->f[pc->t0];
        m->id = ns_mesh_content_hash(m);
    }
    memcpy(c->key, key, sizeof c->key);
    c->have_pose = 1;
    S.st.skinned++;
}

static int32_t quant(float x, float steps) { return (int32_t)lrintf(x * steps); }

/* Muñeca (origen de la pieza mano) en el marco del antebrazo, en enteros: inv(world_antebrazo) * t_mano (el estirado del brazo no la cambia). 0 si la matriz no se invierte. */
static int wrist_in_fore(const NsXf *f, const NsXf *h, int32_t d[3]) {
    const float *m = f->m;
    const float a = m[0], b = m[1], c_ = m[2], e = m[4], ff = m[5], g = m[6], hh = m[8], i = m[9], j = m[10];
    const float det = a * (ff * j - g * i) - b * (e * j - g * hh) + c_ * (e * i - ff * hh);
    if (!(fabsf(det) > 1e-9f)) return 0;
    const float v[3] = {h->m[3] - m[3], h->m[7] - m[7], h->m[11] - m[11]};
    const float inv[9] = {(ff * j - g * i) / det, (c_ * i - b * j) / det, (b * g - c_ * ff) / det, (g * hh - e * j) / det, (a * j - c_ * hh) / det, (c_ * e - a * g) / det,
                          (e * i - ff * hh) / det, (b * hh - a * i) / det, (a * ff - b * e) / det};
    for (int r = 0; r < 3; r++) {
        const float x = inv[3 * r] * v[0] + inv[3 * r + 1] * v[1] + inv[3 * r + 2] * v[2];
        if (!(fabsf(x) < 30000.0f)) return 0;
        d[r] = (int32_t)lrintf(x);
    }
    return 1;
}

static int one_hand(NsScene *sc, NsMeshStore *st, const NsPlayerMap *map, const NsHandIn *in, int h /* 0 izq, 1 der */) {
    const int part = h ? NS_PART_HAND_R : NS_PART_HAND_L, side = h ? 1 : -1;
    const uint32_t key = h ? NS_HAND_KEY_R : NS_HAND_KEY_L;
    if (map->part[part].n == 0 || map->inst < 0 || (uint32_t)map->inst >= sc->n_inst) { S.st.no_part++; return 0; }
    for (uint32_t i = 0; i < sc->n_inst; i++) if (sc->inst[i].key == key) return 0;      /* ya añadida (no debería pasar) */
    for (int i = 0; i < map->part[part].n; i++) if (map->part[part].idx[i] >= sc->n_piece) { S.st.no_part++; return 0; }
    const NsPiece *gp = &sc->piece[map->part[part].idx[0]];
    const NsMesh *g = ns_store_get(st, gp->mesh_id);
    if (!g || !g->n_verts || !g->n_faces || !g->verts || !g->faces) { S.st.no_mesh++; return 0; }
    if (sc->n_inst + 1 > sc->cap_inst || sc->n_piece + NS_HAND_PIECES > sc->cap_piece) { S.st.no_room++; return 0; }
    HandCache *c = &S.c[h];
    if (!c->valid || c->glove_id != g->id) {
        if (!c->valid && S.st.frames < c->next_try) { S.st.no_mesh++; return 0; }                       /* fallo reciente: no reintentar todavía */
        if (!c->valid || S.st.frames - c->last_build >= NS_HAND_REBUILD_FRAMES) {                        /* manopla distinta: reconstruir como mucho cada N fotogramas */
            if (!build(c, side, g)) { c->valid = 0; c->next_try = S.st.frames + NS_HAND_REBUILD_FRAMES; S.st.no_mesh++; return 0; }
            c->last_build = S.st.frames;
            S.st.rebuilt++;
            if (!c->textured) S.st.flat++;
        }                                                                                                 /* si no, se sigue con las caras anteriores */
    }
    /* costura con el antebrazo del motor (si está en la escena) */
    const int fpart = h ? NS_PART_FORE_R : NS_PART_FORE_L;
    const NsPiece *fp = (map->part[fpart].n > 0 && map->part[fpart].idx[0] < sc->n_piece) ? &sc->piece[map->part[fpart].idx[0]] : NULL;
    const NsMesh *fm = fp ? ns_store_get(st, fp->mesh_id) : NULL;
    int32_t d[3] = {0, 0, 0};
    const int want_seam = fm && wrist_in_fore(&fp->world, &gp->world, d);
    if (want_seam ? (!c->seam || c->seam_fore != fm->id || memcmp(d, c->seam_d, sizeof d) != 0) : c->seam) {
        if (!want_seam || !build_seam(c, side, fm, d)) c->seam = 0;
        S.st.seams++;
    }
    VrHandPoseIn hpi;                                                    /* mano que la IK no llevó: relajada, sin objeto ni gatillo */
    memset(&hpi, 0, sizeof hpi);
    hpi.item_pose = HP_RELAX;
    if (in->driven[h]) { hpi.item_pose = in->item_pose[h]; hpi.grip = in->grip[h]; hpi.trigger = in->trigger[h]; hpi.thumb_btn = in->thumb[h]; }
    if (hpi.item_pose >= HP_GRIP_PISTOL && hpi.item_pose <= HP_GRIP_BOX) hpi.grip = 1.0f;    /* con un objeto en la mano (HP_GRIP_*) la mano siempre lo rodea, con o sin botón de agarre; el gatillo sigue moviendo el índice */
    VrHandCurls cu;
    vrhandpose_curls(&hpi, &cu);
    int32_t pk[6];
    for (int i = 0; i < 5; i++) pk[i] = quant(cu.curl[i], NS_HAND_QC);
    pk[5] = quant(cu.thumb_spread, NS_HAND_QC);
    if (!c->have_pose || memcmp(pk, c->key, sizeof pk) != 0) pose_meshes(c, side, pk, g);
    if (c->seam) {
        loft_mesh(c, &fp->world, &gp->world, g);
        if (ns_store_put(st, &c->ring) < 0 || ns_store_put(st, &c->loft) < 0) { S.st.put_fail++; return 0; }
    }
    for (int p = 0; p < VRHA_NP; p++) if (ns_store_put(st, &c->mesh[p]) < 0) { S.st.put_fail++; return 0; }
    const NsInstance *pin = &sc->inst[map->inst];
    NsInstance *ni = &sc->inst[sc->n_inst];
    const int base = c->seam ? 2 : 0;                                    /* pieza 0 = anillo del antebrazo, 1 = transición (con costura) */
    memset(ni, 0, sizeof *ni);
    ni->key = key; ni->first_piece = sc->n_piece; ni->n_pieces = (uint16_t)(VRHA_NP + base);
    ni->flags = (uint16_t)(pin->flags & (NS_IF_LIT | NS_IF_OWN_AMBIENT | NS_IF_BLEND | NS_IF_THERMAL_OK));
    ni->zone_mask = pin->zone_mask; ni->layer = pin->layer; ni->light = pin->light; ni->vis_state = pin->vis_state;
    if (base) {
        NsPiece *q = &sc->piece[sc->n_piece];
        memset(q, 0, sizeof *q);
        q->mesh_id = c->ring.id; q->world = fp->world; q->parent = -1; q->flags = gp->flags & NS_PF_HIDDEN;
        q = &sc->piece[sc->n_piece + 1];
        memset(q, 0, sizeof *q);
        q->mesh_id = c->loft.id; q->world = gp->world; q->parent = 0; q->flags = gp->flags & NS_PF_HIDDEN;
    }
    for (int p = 0; p < VRHA_NP; p++) {
        NsPiece *q = &sc->piece[sc->n_piece + (uint32_t)(base + p)];
        memset(q, 0, sizeof *q);
        q->mesh_id = c->mesh[p].id;
        q->world = gp->world;                                            /* la malla va en el marco de la pieza mano (unidades del motor) */
        q->parent = -1;
        q->flags = gp->flags & NS_PF_HIDDEN;
    }
    sc->n_inst++; sc->n_piece += (uint32_t)(VRHA_NP + base);
    for (int i = 0; i < map->part[part].n; i++) {                                    /* solo las piezas con la malla de la manopla (no otras encadenadas) */
        NsPiece *hp = &sc->piece[map->part[part].idx[i]];
        if (hp->mesh_id == gp->mesh_id) hp->flags |= NS_PF_HIDDEN;
    }
    S.st.applied++;
    return 1;
}

int ns_hand_apply(NsScene *sc, NsMeshStore *st, const NsPlayerMap *map, const NsHandIn *in) {
    if (!sc || !st || !map || !in || !in->enabled || !map->bound) return 0;
    S.st.frames++;
    int n = 0;
    uint32_t bits = 0;
    for (int h = 0; h < 2; h++) if (one_hand(sc, st, map, in, h)) { n++; bits |= 1u << h; }
    S.st.last_applied = bits;
    return n;
}
