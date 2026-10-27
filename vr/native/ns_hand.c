/* ns_hand: ver ns_hand.h. */
#include "ns_hand.h"
#include <math.h>
#include <string.h>
#include "ns_extract.h"
#include "vr_hand_skin.h"
#include "vr_hand_pose.h"

#define NS_HAND_QC 32.0f            /* pasos de cuantización de los cierres y de la separación del pulgar */
#define NS_HAND_QK 256.0f           /* pasos de la escala */
typedef struct {
    int valid;
    uint64_t glove_id;           /* malla de la manopla con la que se construyeron las caras */
    int textured;
    uint32_t last_build;         /* fotograma (S.st.frames) de la última construcción */
    uint32_t next_try;           /* no volver a intentar construir antes de este fotograma (tras un fallo) */
    NsFace f[VRHA_NT];           /* caras (índices locales a su pieza, UV de la manopla): no cambian con la pose */
    int have_pose;
    int32_t key[7];              /* pose cuantizada de las mallas actuales: 5 cierres, separación, escala */
    float pos[VRHA_NV][3], nrm[VRHA_NV][3];
    NsVert v[VRHA_NV];
    NsNorm n[VRHA_NV];
    NsMesh mesh[VRHA_NP];
    uint64_t fore_src;           /* antebrazo adelgazado: malla original y escala con que se hizo */
    int32_t fore_k;
    int fore_ok;
    NsVert fv[256];
    NsMesh fmesh;
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
/* Caras de la mano con las texturas de la manopla: el orden de los vértices pasa de antihorario hacia fuera (asset) a la convención del motor (cross(B-A, D-A) interior); la izquierda
 * es un espejo y ya invierte el sentido. Parámetro de textura de cada esquina: la posición en reposo normalizada (palma, dorso y puntas: z y largo; costados: grosor y largo). */
static int build(HandCache *c, int side, const NsMesh *g) {
    NsHandTex tex[4];
    c->textured = ns_hand_classify(g, side, tex) > 0;
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    for (int i = 0; i < VRHA_NV; i++) for (int a = 0; a < 3; a++) { if (vrha_pos[i][a] < lo[a]) lo[a] = vrha_pos[i][a]; if (vrha_pos[i][a] > hi[a]) hi[a] = vrha_pos[i][a]; }
    for (int p = 0; p < VRHA_NP; p++) {
        const VrHaPiece *pc = &vrha_piece[p];
        for (int t = pc->t0; t < pc->t0 + pc->nt; t++) {
            NsFace *f = &c->f[t];
            memset(f, 0, sizeof *f);
            const uint8_t *tr = vrha_tri[t];
            const uint8_t o[4] = {tr[0], side > 0 ? tr[2] : tr[1], side > 0 ? tr[1] : tr[2], side > 0 ? tr[1] : tr[2]};
            const int role = vrha_role[t] & 3;
            const NsHandTex *tx = &tex[role];
            for (int q = 0; q < 4; q++) {
                f->v[q] = o[q]; f->n[q] = o[q];
                if (!tx->ok) continue;
                const float *v = vrha_pos[pc->v0 + o[q]];
                const int ax = role == VRHA_ROLE_SIDE ? 0 : 2;
                float su = (v[ax] - lo[ax]) / (hi[ax] - lo[ax]), sv = (v[1] - lo[1]) / (hi[1] - lo[1]);
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

static int32_t quant(float x, float steps) { return (int32_t)lrintf(x * steps); }

/* Mallas de la pose cuantizada `key` (cierres, separación y escala ya cuantizados): skin, mm * k, normales interiores. */
static void pose_meshes(HandCache *c, int side, const int32_t key[7], const NsMesh *g) {
    float curl[5], xf[VRHS_J][12];
    for (int i = 0; i < 5; i++) curl[i] = (float)key[i] / NS_HAND_QC;
    const float k = (float)key[6] / NS_HAND_QK;
    vrhs_pose(curl, (float)key[5] / NS_HAND_QC, side, xf);
    vrhs_skin(xf, side, c->pos, c->nrm);
    for (int i = 0; i < VRHA_NV; i++) {
        c->v[i].x = (int16_t)ns_clampf(floorf(c->pos[i][0] * k + 0.5f), -32768.0f, 32767.0f);
        c->v[i].y = (int16_t)ns_clampf(floorf(c->pos[i][1] * k + 0.5f), -32768.0f, 32767.0f);
        c->v[i].z = (int16_t)ns_clampf(floorf(c->pos[i][2] * k + 0.5f), -32768.0f, 32767.0f);
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

/* Radio medio del puño del guante (vértices por encima de la muñeca) alrededor del eje Y, mm. */
static float cuff_radius(void) {
    float s = 0.0f; int n = 0;
    for (int i = 0; i < VRHA_NV; i++) if (vrha_pos[i][1] > 10.0f) { s += sqrtf(vrha_pos[i][0] * vrha_pos[i][0] + vrha_pos[i][2] * vrha_pos[i][2]); n++; }
    return n ? s / (float)n : 25.0f;
}
/* Eje del antebrazo: del origen (codo) al centro de los vértices no soldados. 0 si no hay. */
static int fore_axis(const NsMesh *m, float l[3]) {
    float c[3] = {0, 0, 0}; int n = 0;
    for (unsigned i = 0; i < m->n_verts; i++) if (m->verts[i].weld == 0xFF) { c[0] += m->verts[i].x; c[1] += m->verts[i].y; c[2] += m->verts[i].z; n++; }
    float len = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    if (!n || !(len > 1e-3f)) return 0;
    for (int a = 0; a < 3; a++) l[a] = c[a] / len;
    return 1;
}
float ns_hand_fore_slim(const NsMesh *m, float k) {
    float l[3];
    if (!m || !m->verts || !m->n_verts || !fore_axis(m, l)) return 1.0f;
    float tmax = -1e9f;
    for (unsigned i = 0; i < m->n_verts; i++) if (m->verts[i].weld == 0xFF) { float t = l[0] * m->verts[i].x + l[1] * m->verts[i].y + l[2] * m->verts[i].z; if (t > tmax) tmax = t; }
    if (!(tmax > 1.0f)) return 1.0f;
    float rs = 0.0f; int n = 0;
    for (unsigned i = 0; i < m->n_verts; i++) {
        const NsVert *v = &m->verts[i];
        if (v->weld != 0xFF) continue;
        float t = l[0] * v->x + l[1] * v->y + l[2] * v->z;
        if (t < 0.75f * tmax) continue;
        float r[3] = {v->x - t * l[0], v->y - t * l[1], v->z - t * l[2]};
        rs += sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]); n++;
    }
    if (!n || !(rs > 0.0f)) return 1.0f;
    return ns_clampf(1.15f * cuff_radius() * k * (float)n / rs, 0.6f, 1.0f);
}
/* Copia adelgazada del antebrazo (vértices no soldados hacia su eje); 1 = lista en c->fmesh. */
static int fore_mesh(HandCache *c, const NsMesh *m, int32_t kq) {
    if (c->fore_ok && c->fore_src == m->id && c->fore_k == kq) return 1;
    c->fore_ok = 0;
    float l[3];
    if (!m->verts || !m->faces || !m->n_verts || m->n_verts > 256 || !fore_axis(m, l)) return 0;
    const float s = ns_hand_fore_slim(m, (float)kq / NS_HAND_QK);
    if (s >= 0.999f) return 0;
    for (unsigned i = 0; i < m->n_verts; i++) {
        NsVert v = m->verts[i];
        if (v.weld == 0xFF) {
            float p[3] = {v.x, v.y, v.z}, t = l[0] * p[0] + l[1] * p[1] + l[2] * p[2];
            v.x = (int16_t)lrintf(t * l[0] + s * (p[0] - t * l[0])); v.y = (int16_t)lrintf(t * l[1] + s * (p[1] - t * l[1])); v.z = (int16_t)lrintf(t * l[2] + s * (p[2] - t * l[2]));
        }
        c->fv[i] = v;
    }
    c->fmesh = *m;
    c->fmesh.verts = c->fv;
    int16_t mn[3] = {32767, 32767, 32767}, mx[3] = {-32768, -32768, -32768};
    for (unsigned i = 0; i < m->n_verts; i++) { const int16_t q[3] = {c->fv[i].x, c->fv[i].y, c->fv[i].z}; for (int a = 0; a < 3; a++) { if (q[a] < mn[a]) mn[a] = q[a]; if (q[a] > mx[a]) mx[a] = q[a]; } }
    for (int a = 0; a < 3; a++) { c->fmesh.bbox[a] = mn[a]; c->fmesh.bbox[3 + a] = mx[a]; }
    c->fmesh.id = ns_mesh_content_hash(&c->fmesh);
    c->fore_src = m->id; c->fore_k = kq; c->fore_ok = 1;
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
    VrHandPoseIn hpi;                                                    /* mano que la IK no llevó: relajada, sin objeto ni gatillo */
    memset(&hpi, 0, sizeof hpi);
    hpi.item_pose = HP_RELAX;
    if (in->driven[h]) { hpi.item_pose = in->item_pose[h]; hpi.grip = in->grip[h]; hpi.trigger = in->trigger[h]; hpi.thumb_btn = in->thumb[h]; }
    if (hpi.item_pose >= HP_GRIP_PISTOL && hpi.item_pose <= HP_GRIP_BOX) hpi.grip = 1.0f;    /* con un objeto en la mano (HP_GRIP_*) la mano siempre lo rodea, con o sin botón de agarre; el gatillo sigue moviendo el índice */
    VrHandCurls cu;
    vrhandpose_curls(&hpi, &cu);
    const float upm = in->units_per_m > 1.0f && in->units_per_m == in->units_per_m ? in->units_per_m : 1000.0f;
    int32_t pk[7];
    for (int i = 0; i < 5; i++) pk[i] = quant(cu.curl[i], NS_HAND_QC);
    pk[5] = quant(cu.thumb_spread, NS_HAND_QC);
    pk[6] = quant(ns_clampf(upm / 1000.0f, 0.5f, 1.4f), NS_HAND_QK);
    if (!c->have_pose || memcmp(pk, c->key, sizeof pk) != 0) pose_meshes(c, side, pk, g);
    for (int p = 0; p < VRHA_NP; p++) if (ns_store_put(st, &c->mesh[p]) < 0) { S.st.put_fail++; return 0; }
    const NsInstance *pin = &sc->inst[map->inst];
    NsInstance *ni = &sc->inst[sc->n_inst];
    memset(ni, 0, sizeof *ni);
    ni->key = key; ni->first_piece = sc->n_piece; ni->n_pieces = NS_HAND_PIECES;
    ni->flags = (uint16_t)(pin->flags & (NS_IF_LIT | NS_IF_OWN_AMBIENT | NS_IF_BLEND | NS_IF_THERMAL_OK));
    ni->zone_mask = pin->zone_mask; ni->layer = pin->layer; ni->light = pin->light; ni->vis_state = pin->vis_state;
    for (int p = 0; p < NS_HAND_PIECES; p++) {
        NsPiece *q = &sc->piece[sc->n_piece + (uint32_t)p];
        memset(q, 0, sizeof *q);
        q->mesh_id = c->mesh[p].id;
        q->world = gp->world;                                            /* la malla ya va en el marco de la pieza mano (mm * k) */
        q->parent = -1;
        q->flags = gp->flags & NS_PF_HIDDEN;
    }
    sc->n_inst++; sc->n_piece += NS_HAND_PIECES;
    for (int i = 0; i < map->part[part].n; i++) {                                    /* solo las piezas con la malla de la manopla (no otras encadenadas) */
        NsPiece *hp = &sc->piece[map->part[part].idx[i]];
        if (hp->mesh_id == gp->mesh_id) hp->flags |= NS_PF_HIDDEN;
    }
    /* antebrazo adelgazado (si no cabe en el almacén se queda el original) */
    const int fpart = h ? NS_PART_FORE_R : NS_PART_FORE_L;
    if (map->part[fpart].n > 0 && map->part[fpart].idx[0] < sc->n_piece) {
        NsPiece *fp = &sc->piece[map->part[fpart].idx[0]];
        const NsMesh *fm = ns_store_get(st, fp->mesh_id);
        if (fm && fm->id != c->fmesh.id && fore_mesh(c, fm, pk[6]) && ns_store_put(st, &c->fmesh) >= 0) {
            const uint64_t src = fp->mesh_id;
            for (int i = 0; i < map->part[fpart].n; i++) {
                NsPiece *q = map->part[fpart].idx[i] < sc->n_piece ? &sc->piece[map->part[fpart].idx[i]] : NULL;
                if (q && q->mesh_id == src) q->mesh_id = c->fmesh.id;
            }
            S.st.slimmed++;
        }
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
