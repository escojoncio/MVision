/* ns_hand: ver ns_hand.h. */
#include "ns_hand.h"
#include <math.h>
#include <string.h>
#include "ns_extract.h"
#include "vr_hand_skin.h"
#include "vr_hand_pose.h"

#define NS_HAND_QC 32.0f            /* pasos de cuantización de los cierres y de la separación del pulgar */
#define NS_HAND_RIM 24              /* sectores del borde del puño soldados al anillo del antebrazo */
#define NS_HAND_TAPER 30.0f         /* el puño se deforma hacia el antebrazo desde esta distancia bajo la muñeca (unidades del motor) */
typedef struct {
    int valid;
    uint64_t glove_id;           /* malla de la manopla con la que se construyeron las caras */
    int textured;
    uint32_t last_build;         /* fotograma (S.st.frames) de la última construcción */
    uint32_t next_try;           /* no volver a intentar construir antes de este fotograma (tras un fallo) */
    NsFace f[VRHA_NT];           /* caras (índices locales a su pieza, UV de la manopla): no cambian con la pose */
    /* costura con el antebrazo del motor: anillo (malla propia en el marco del antebrazo) al que se sueldan los vértices del borde del puño; el resto del puño se deforma hacia él */
    int seam;
    uint64_t seam_fore;          /* malla del antebrazo y muñeca (en su marco, enteros) con que se calculó */
    int32_t seam_d[3];
    float disp[VRHA_NV][3];      /* desplazamiento del puño en el marco de la mano */
    uint8_t weld[VRHA_NV];       /* 0xFF o índice en el anillo */
    NsVert rv[NS_HAND_RIM];
    NsNorm rn[1];
    NsFace rf[1];
    NsMesh ring;
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
    c->glove_id = g->id;
    c->have_pose = 0;
    c->valid = 1;
    return 1;
}

/* Costura: anillo final del antebrazo `fm` (vértices no soldados más alejados del codo), con la muñeca en `d` (marco del antebrazo, en reposo los ejes de antebrazo y mano coinciden).
 * Borde del puño = en cada sector angular alrededor del centro del puño, el vértice más alto (hacia el codo). Se suelda (NsVert.weld) al punto del contorno del anillo del antebrazo en esa
 * dirección y el resto del puño se desplaza hacia el contorno con un peso que crece de la muñeca (0) al borde (1): sin hueco, sin cuello y sin escalón, y el borde sigue al antebrazo
 * (giro incluido). 1 = costura lista. */
static int build_seam(HandCache *c, int side, const NsMesh *fm, const int32_t d[3]) {
    c->seam = 0;
    memset(c->disp, 0, sizeof c->disp);
    memset(c->weld, 0xFF, sizeof c->weld);
    if (!fm || !fm->verts || !fm->n_verts || fm->n_verts > 256) return 0;
    /* anillo del antebrazo en el marco de la mano */
    float l[3] = {0, 0, 0}; int nn = 0;
    for (unsigned i = 0; i < fm->n_verts; i++) if (fm->verts[i].weld == 0xFF) { l[0] += fm->verts[i].x; l[1] += fm->verts[i].y; l[2] += fm->verts[i].z; nn++; }
    float ll = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
    if (nn < 3 || !(ll > 1.0f)) return 0;
    for (int a = 0; a < 3; a++) l[a] /= ll;
    float tmax = -1e9f;
    for (unsigned i = 0; i < fm->n_verts; i++) if (fm->verts[i].weld == 0xFF) { const float t = l[0] * fm->verts[i].x + l[1] * fm->verts[i].y + l[2] * fm->verts[i].z; if (t > tmax) tmax = t; }
    float H[16][3], cr[2] = {0, 0}, yr = 0; int nh = 0;
    for (unsigned i = 0; i < fm->n_verts && nh < 16; i++) {
        const NsVert *v = &fm->verts[i];
        if (v->weld != 0xFF || l[0] * v->x + l[1] * v->y + l[2] * v->z < tmax - 25.0f) continue;
        H[nh][0] = (float)(v->x - d[0]); H[nh][1] = (float)(v->y - d[1]); H[nh][2] = (float)(v->z - d[2]);
        cr[0] += H[nh][0]; cr[1] += H[nh][2]; yr += H[nh][1]; nh++;
    }
    if (nh < 3) return 0;
    cr[0] /= (float)nh; cr[1] /= (float)nh; yr /= (float)nh;
    float ang[16];
    for (int i = 0; i < nh; i++) ang[i] = atan2f(H[i][2] - cr[1], H[i][0] - cr[0]);
    for (int i = 1; i < nh; i++) for (int j = i; j > 0 && ang[j] < ang[j - 1]; j--) {   /* contorno ordenado por ángulo */
        float ta = ang[j]; ang[j] = ang[j - 1]; ang[j - 1] = ta;
        for (int a = 0; a < 3; a++) { float tv = H[j][a]; H[j][a] = H[j - 1][a]; H[j - 1][a] = tv; }
    }
    /* puño de la mano en reposo */
    float xf[VRHS_J][12];
    const float zero[5] = {0, 0, 0, 0, 0};
    vrhs_pose(zero, 0.0f, side, xf);
    vrhs_skin(xf, side, c->pos, NULL);
    float ch[2] = {0, 0}; int nc = 0;
    for (int i = 0; i < VRHA_NV; i++) if (c->pos[i][1] > 0.0f && vrha_joint[i][0] == VRHS_WRIST) { ch[0] += c->pos[i][0]; ch[1] += c->pos[i][2]; nc++; }
    if (nc < 8) return 0;
    ch[0] /= (float)nc; ch[1] /= (float)nc;
    int top[NS_HAND_RIM];
    for (int b = 0; b < NS_HAND_RIM; b++) top[b] = -1;
    int bin[VRHA_NV];
    const float ylo = -NS_HAND_TAPER;                                    /* el puño se deforma desde un poco por debajo de la muñeca: transición larga, sin escalón */
    for (int i = 0; i < VRHA_NV; i++) {
        bin[i] = -1;
        if (c->pos[i][1] <= ylo) continue;
        float th = atan2f(c->pos[i][2] - ch[1], c->pos[i][0] - ch[0]);
        int b = (int)floorf((th + 3.14159265f) / 6.2831853f * (float)NS_HAND_RIM);
        b = b < 0 ? 0 : (b >= NS_HAND_RIM ? NS_HAND_RIM - 1 : b);
        bin[i] = b;
        if (c->pos[i][1] > 0.0f && vrha_joint[i][0] == VRHS_WRIST && (top[b] < 0 || c->pos[i][1] > c->pos[top[b]][1])) top[b] = i;
    }
    float rimd[NS_HAND_RIM][3], rimy[NS_HAND_RIM];
    int nr = 0;
    for (int b = 0; b < NS_HAND_RIM; b++) {
        rimy[b] = 0; rimd[b][0] = rimd[b][1] = rimd[b][2] = 0;
        if (top[b] < 0) continue;
        const float *p = c->pos[top[b]];
        /* contorno del antebrazo en la dirección del vértice (rayo desde el centro del anillo) */
        const float th = atan2f(p[2] - ch[1], p[0] - ch[0]), dx = cosf(th), dz = sinf(th);
        float T[3] = {cr[0], yr, cr[1]}, best = 1e9f;
        for (int e = 0; e < nh; e++) {
            const float *A = H[e], *B = H[(e + 1) % nh];
            const float ex = B[0] - A[0], ez = B[2] - A[2], den = dx * ez - dz * ex;
            if (fabsf(den) < 1e-6f) continue;
            const float ax = A[0] - cr[0], az = A[2] - cr[1];
            const float tr = (ax * ez - az * ex) / den, u = (ax * dz - az * dx) / den;
            if (tr > 0.0f && u >= -1e-4f && u <= 1.0001f && tr < best) { best = tr; T[0] = A[0] + u * ex; T[1] = A[1] + u * (B[1] - A[1]); T[2] = A[2] + u * ez; }
        }
        if (best > 1e8f) continue;
        for (int a = 0; a < 3; a++) rimd[b][a] = T[a] - p[a];
        rimy[b] = p[1];
        c->rv[nr].x = (int16_t)lrintf(T[0] + (float)d[0]); c->rv[nr].y = (int16_t)lrintf(T[1] + (float)d[1]); c->rv[nr].z = (int16_t)lrintf(T[2] + (float)d[2]);
        c->rv[nr].weld = 0xFF; c->rv[nr].pad = 0;
        for (int i = 0; i < VRHA_NV; i++)   /* el vértice del borde y sus duplicados en otras piezas */
            if (fabsf(c->pos[i][0] - p[0]) < 0.01f && fabsf(c->pos[i][1] - p[1]) < 0.01f && fabsf(c->pos[i][2] - p[2]) < 0.01f) c->weld[i] = (uint8_t)nr;
        nr++;
    }
    if (nr < 3) { memset(c->weld, 0xFF, sizeof c->weld); return 0; }
    for (int i = 0; i < VRHA_NV; i++) {
        const int b = bin[i];
        if (b < 0 || rimy[b] <= 0.0f) continue;
        float t = ns_clampf((c->pos[i][1] - ylo) / (rimy[b] - ylo), 0.0f, 1.0f), w0 = 0.0f;
        for (int q = 0; q < 4; q++) if (vrha_joint[i][q] == VRHS_WRIST) w0 += vrha_weight[i][q];
        t = t * t * (3.0f - 2.0f * t) * w0;
        if (c->weld[i] != 0xFF) t = 1.0f;
        for (int a = 0; a < 3; a++) c->disp[i][a] = rimd[b][a] * t;
    }
    memset(&c->ring, 0, sizeof c->ring);
    memset(c->rf, 0, sizeof c->rf);
    c->rf[0].flags = NS_FF_TRI;                 /* una cara degenerada: el anillo no se dibuja, solo presta sus vértices a las soldaduras */
    c->rn[0].x = 0; c->rn[0].y = 4096; c->rn[0].z = 0; c->rn[0].pad = 0;
    c->ring.n_verts = (uint16_t)nr; c->ring.n_norms = 1; c->ring.n_faces = 1;
    c->ring.verts = c->rv; c->ring.norms = c->rn; c->ring.faces = c->rf;
    for (int a = 0; a < 3; a++) { c->ring.bbox[a] = 32767; c->ring.bbox[3 + a] = -32768; }
    for (int i = 0; i < nr; i++) {
        const int16_t q[3] = {c->rv[i].x, c->rv[i].y, c->rv[i].z};
        for (int a = 0; a < 3; a++) { if (q[a] < c->ring.bbox[a]) c->ring.bbox[a] = q[a]; if (q[a] > c->ring.bbox[3 + a]) c->ring.bbox[3 + a] = q[a]; }
    }
    c->ring.id = ns_mesh_content_hash(&c->ring);
    c->seam_fore = fm->id; memcpy(c->seam_d, d, sizeof c->seam_d);
    c->seam = 1;
    return 1;
}

/* Mallas de la pose cuantizada `key` (cierres y separación ya cuantizados): skin + costura del puño, normales interiores, soldaduras del borde. */
static void pose_meshes(HandCache *c, int side, const int32_t key[6], const NsMesh *g) {
    float curl[5], xf[VRHS_J][12];
    for (int i = 0; i < 5; i++) curl[i] = (float)key[i] / NS_HAND_QC;
    vrhs_pose(curl, (float)key[5] / NS_HAND_QC, side, xf);
    vrhs_skin(xf, side, c->pos, c->nrm);
    for (int i = 0; i < VRHA_NV; i++) {
        c->v[i].x = (int16_t)ns_clampf(floorf(c->pos[i][0] + c->disp[i][0] + 0.5f), -32768.0f, 32767.0f);
        c->v[i].y = (int16_t)ns_clampf(floorf(c->pos[i][1] + c->disp[i][1] + 0.5f), -32768.0f, 32767.0f);
        c->v[i].z = (int16_t)ns_clampf(floorf(c->pos[i][2] + c->disp[i][2] + 0.5f), -32768.0f, 32767.0f);
        c->v[i].weld = c->seam ? c->weld[i] : 0xFF; c->v[i].pad = 0;
        c->n[i].x = (int16_t)lrintf(-c->nrm[i][0] * 4096.0f); c->n[i].y = (int16_t)lrintf(-c->nrm[i][1] * 4096.0f); c->n[i].z = (int16_t)lrintf(-c->nrm[i][2] * 4096.0f); c->n[i].pad = 0;
    }
    for (int p = 0; p < VRHA_NP; p++) {
        const VrHaPiece *pc = &vrha_piece[p];
        NsMesh *m = &c->mesh[p];
        memset(m, 0, sizeof *m);
        int16_t mn[3] = {32767, 32767, 32767}, mx[3] = {-32768, -32768, -32768};
        int nw = 0;
        for (int i = pc->v0; i < pc->v0 + pc->nv; i++) {
            const int16_t q[3] = {c->v[i].x, c->v[i].y, c->v[i].z};
            for (int a = 0; a < 3; a++) { if (q[a] < mn[a]) mn[a] = q[a]; if (q[a] > mx[a]) mx[a] = q[a]; }
            nw += c->v[i].weld != 0xFF;
        }
        m->flags = g->flags & (NS_MF_BLEND | NS_MF_UNLIT | NS_MF_TWOSIDED);
        if (nw) m->flags |= NS_MF_WELDED;
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
        if (!want_seam || !build_seam(c, side, fm, d)) { c->seam = 0; memset(c->disp, 0, sizeof c->disp); }
        c->have_pose = 0;
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
    if (c->seam && ns_store_put(st, &c->ring) < 0) { S.st.put_fail++; return 0; }
    for (int p = 0; p < VRHA_NP; p++) if (ns_store_put(st, &c->mesh[p]) < 0) { S.st.put_fail++; return 0; }
    const NsInstance *pin = &sc->inst[map->inst];
    NsInstance *ni = &sc->inst[sc->n_inst];
    const int base = c->seam ? 1 : 0;                                    /* pieza 0 = anillo del antebrazo (con costura) */
    memset(ni, 0, sizeof *ni);
    ni->key = key; ni->first_piece = sc->n_piece; ni->n_pieces = (uint16_t)(VRHA_NP + base);
    ni->flags = (uint16_t)(pin->flags & (NS_IF_LIT | NS_IF_OWN_AMBIENT | NS_IF_BLEND | NS_IF_THERMAL_OK));
    ni->zone_mask = pin->zone_mask; ni->layer = pin->layer; ni->light = pin->light; ni->vis_state = pin->vis_state;
    if (base) {
        NsPiece *q = &sc->piece[sc->n_piece];
        memset(q, 0, sizeof *q);
        q->mesh_id = c->ring.id; q->world = fp->world; q->parent = -1; q->flags = gp->flags & NS_PF_HIDDEN;
    }
    for (int p = 0; p < VRHA_NP; p++) {
        NsPiece *q = &sc->piece[sc->n_piece + (uint32_t)(base + p)];
        memset(q, 0, sizeof *q);
        q->mesh_id = c->mesh[p].id;
        q->world = gp->world;                                            /* la malla va en el marco de la pieza mano (unidades del motor) */
        q->parent = (int16_t)((c->mesh[p].flags & NS_MF_WELDED) ? 0 : -1);
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
