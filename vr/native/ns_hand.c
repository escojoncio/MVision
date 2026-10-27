/* ns_hand: ver ns_hand.h. */
#include "ns_hand.h"
#include <math.h>
#include <string.h>
#include "ns_extract.h"
#include "vr_hand_mesh.h"
#include "vr_hand_pose.h"

typedef struct {
    int valid;
    uint64_t glove_id;           /* malla de la manopla con la que se construyó */
    VrHand hand;
    NsVert v[NS_HAND_BONES][VRHAND_MAX_V];
    NsNorm n[NS_HAND_BONES][VRHAND_MAX_V];
    NsFace f[NS_HAND_BONES][VRHAND_MAX_F];
    NsMesh mesh[NS_HAND_BONES];
    int textured;
    uint32_t last_build;         /* fotograma (S.st.frames) de la última construcción */
    uint32_t next_try;           /* no volver a intentar construir antes de este fotograma (tras un fallo) */
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
static int build(HandCache *c, int side, const NsMesh *g) {
    VrHandDims d;
    vrhand_dims_from_verts(&g->verts[0].x, g->n_verts, (int)(sizeof(NsVert) / sizeof(int16_t)), side, &d);   /* 0 = por defecto: sigue valiendo */
    if (vrhand_build(&c->hand, &d, side) != 0) return 0;
    NsHandTex tex[4];
    c->textured = ns_hand_classify(g, side, tex) > 0;
    for (int k = 0; k < NS_HAND_BONES; k++) {
        const VrHandBoneMesh *b = &c->hand.mesh[k];
        NsMesh *m = &c->mesh[k];
        memset(m, 0, sizeof *m);
        int16_t mn[3] = {32767, 32767, 32767}, mx[3] = {-32768, -32768, -32768};
        int nweld = 0;
        for (int i = 0; i < b->n_verts; i++) {
            c->v[k][i].x = b->v[i][0]; c->v[k][i].y = b->v[i][1]; c->v[k][i].z = b->v[i][2]; c->v[k][i].weld = b->weld[i]; c->v[k][i].pad = 0; if (b->weld[i] != VRHAND_NO_WELD) nweld++;
            c->n[k][i].x = b->n[i][0]; c->n[k][i].y = b->n[i][1]; c->n[k][i].z = b->n[i][2]; c->n[k][i].pad = 0;
            for (int a = 0; a < 3; a++) { if (b->v[i][a] < mn[a]) mn[a] = b->v[i][a]; if (b->v[i][a] > mx[a]) mx[a] = b->v[i][a]; }
        }
        for (int i = 0; i < b->n_faces; i++) {
            NsFace *f = &c->f[k][i];
            memset(f, 0, sizeof *f);
            const NsHandTex *tx = &tex[b->role[i] < 4 ? b->role[i] : 0];
            for (int q = 0; q < 4; q++) {
                f->v[q] = b->f[i][q]; f->n[q] = b->f[i][q];
                if (tx->ok) map_uv(tx, b->su[i][q], b->sv[i][q], &f->u[q], &f->t[q]);
            }
            if (tx->ok) { f->tpage = tx->tpage; f->clut = tx->clut; f->texid = tx->texid; }
        }
        m->flags = g->flags & (NS_MF_BLEND | NS_MF_UNLIT | NS_MF_TWOSIDED);
        if (nweld) m->flags |= NS_MF_WELDED;                              /* anillo proximal soldado al distal del hueso padre (NsPiece.parent) */
        m->model_flags = g->model_flags;
        m->depth_bias = g->depth_bias;
        if (m->depth_bias > 0) m->flags |= NS_MF_BIAS_UP;
        m->n_verts = b->n_verts; m->n_norms = b->n_verts; m->n_faces = b->n_faces;
        for (int a = 0; a < 3; a++) { m->bbox[a] = mn[a]; m->bbox[3 + a] = mx[a]; }
        m->verts = c->v[k]; m->norms = c->n[k]; m->faces = c->f[k];
        m->id = ns_mesh_content_hash(m);
    }
    c->glove_id = g->id;
    c->valid = 1;
    return 1;
}

static void xf_mul(NsXf *o, const NsXf *a, const float b[12]) {   /* o = a * b (b: 3x4 fila-mayor hueso -> mano) */
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) o->m[4 * i + j] = a->m[4 * i] * b[j] + a->m[4 * i + 1] * b[4 + j] + a->m[4 * i + 2] * b[8 + j];
        o->m[4 * i + 3] = a->m[4 * i] * b[3] + a->m[4 * i + 1] * b[7] + a->m[4 * i + 2] * b[11] + a->m[4 * i + 3];
    }
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
    if (sc->n_inst + 1 > sc->cap_inst || sc->n_piece + NS_HAND_BONES > sc->cap_piece) { S.st.no_room++; return 0; }
    HandCache *c = &S.c[h];
    if (!c->valid || c->glove_id != g->id) {
        if (!c->valid && S.st.frames < c->next_try) { S.st.no_mesh++; return 0; }                       /* fallo reciente: no reintentar todavía */
        if (!c->valid || S.st.frames - c->last_build >= NS_HAND_REBUILD_FRAMES) {                        /* manopla distinta: reconstruir como mucho cada N fotogramas */
            if (!build(c, side, g)) { c->valid = 0; c->next_try = S.st.frames + NS_HAND_REBUILD_FRAMES; S.st.no_mesh++; return 0; }
            c->last_build = S.st.frames;
            S.st.rebuilt++;
            if (!c->textured) S.st.flat++;
        }                                                                                                 /* si no, se sigue con las mallas anteriores */
    }
    for (int k = 0; k < NS_HAND_BONES; k++) if (ns_store_put(st, &c->mesh[k]) < 0) { S.st.put_fail++; return 0; }
    float xf[NS_HAND_BONES][12];
    VrHandPoseIn hpi;                                                    /* mano que la IK no llevó: relajada, sin objeto ni gatillo */
    memset(&hpi, 0, sizeof hpi);
    hpi.item_pose = HP_RELAX;
    if (in->driven[h]) { hpi.item_pose = in->item_pose[h]; hpi.grip = in->grip[h]; hpi.trigger = in->trigger[h]; hpi.thumb_btn = in->thumb[h]; }
    if (hpi.item_pose >= HP_GRIP_PISTOL && hpi.item_pose <= HP_GRIP_BOX) hpi.grip = 1.0f;    /* con un objeto en la mano (HP_GRIP_*) la mano siempre lo rodea, con o sin botón de agarre; el gatillo sigue moviendo el índice */
    vrhandpose_blend(&c->hand, &hpi, xf);
    const NsInstance *pin = &sc->inst[map->inst];
    NsInstance *ni = &sc->inst[sc->n_inst];
    memset(ni, 0, sizeof *ni);
    ni->key = key; ni->first_piece = sc->n_piece; ni->n_pieces = NS_HAND_BONES;
    ni->flags = (uint16_t)(pin->flags & (NS_IF_LIT | NS_IF_OWN_AMBIENT | NS_IF_BLEND | NS_IF_THERMAL_OK));
    ni->zone_mask = pin->zone_mask; ni->layer = pin->layer; ni->light = pin->light; ni->vis_state = pin->vis_state;
    for (int k = 0; k < NS_HAND_BONES; k++) {
        NsPiece *p = &sc->piece[sc->n_piece + (uint32_t)k];
        memset(p, 0, sizeof *p);
        p->mesh_id = c->mesh[k].id;
        xf_mul(&p->world, &gp->world, xf[k]);
        p->parent = (int16_t)c->hand.bone[k].parent;                   /* los vértices soldados toman la posición del hueso padre (índice dentro de la instancia) */
        p->flags = gp->flags & NS_PF_HIDDEN;
    }
    sc->n_inst++; sc->n_piece += NS_HAND_BONES;
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
