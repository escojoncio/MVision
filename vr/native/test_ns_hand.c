/* Pruebas de ns_hand (manos propias con skin en la escena nativa) con una escena, una manopla y un antebrazo SINTÉTICOS (cajas, sin datos del juego). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ns_hand.h"
#include "ns_extract.h"
#include "ns_solid.h"
#include "ns_gpu.h"
#include "vr_hand_skin.h"
#include "vr_hand_pose.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: %s ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- manopla sintética: caja x in [-30,30], y in [-125,20], z in [-35,35]; palma a +X*side; cada cara con su texid y su rectángulo ---- */
typedef struct { NsVert v[8]; NsNorm n[8]; NsFace f[6]; NsMesh m; } Glove;
static void add_quad(Glove *g, int fi, const int idx[4], const float out[3], int texid, uint16_t tpage, uint16_t clut, const uint8_t u[4], const uint8_t t[4]) {
    NsFace *f = &g->f[fi];
    memset(f, 0, sizeof *f);
    int o[4] = {idx[0], idx[1], idx[2], idx[3]};
    float p[4][3];
    for (int k = 0; k < 4; k++) { p[k][0] = g->v[o[k]].x; p[k][1] = g->v[o[k]].y; p[k][2] = g->v[o[k]].z; }
    float e1[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]}, e2[3] = {p[3][0] - p[0][0], p[3][1] - p[0][1], p[3][2] - p[0][2]};
    float c[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    if (c[0] * out[0] + c[1] * out[1] + c[2] * out[2] > 0) { int s = o[1]; o[1] = o[3]; o[3] = s; }   /* cross(B-A, D-A) debe ser INTERIOR */
    for (int k = 0; k < 4; k++) { f->v[k] = (uint8_t)o[k]; f->n[k] = (uint8_t)o[k]; f->u[k] = u[k]; f->t[k] = t[k]; }
    f->tpage = tpage; f->clut = clut; f->texid = (uint16_t)texid;
}
static void make_glove(Glove *g, int side, int textured, uint64_t salt) {
    memset(g, 0, sizeof *g);
    int i = 0;
    for (int ix = 0; ix < 2; ix++) for (int iy = 0; iy < 2; iy++) for (int iz = 0; iz < 2; iz++) {
        g->v[i].x = (int16_t)(ix ? 30 : -30); g->v[i].y = (int16_t)(iy ? 20 : -125); g->v[i].z = (int16_t)(iz ? 35 : -35); g->v[i].weld = 0xFF;
        float l = sqrtf(3.0f); g->n[i].x = (int16_t)(-(ix ? 1 : -1) * 4096 / l); g->n[i].y = (int16_t)(-(iy ? 1 : -1) * 4096 / l); g->n[i].z = (int16_t)(-(iz ? 1 : -1) * 4096 / l);
        i++;
    }
    /* vértice (ix,iy,iz) = 4*ix + 2*iy + iz */
    const uint8_t uA[4] = {8, 56, 56, 8}, tA[4] = {8, 8, 56, 56};
    const uint8_t uB[4] = {72, 120, 120, 72};
    const uint8_t uC[4] = {136, 184, 184, 136};
    const uint8_t uD[4] = {200, 248, 248, 200};
    int xp[4] = {4, 5, 7, 6}, xm[4] = {0, 1, 3, 2}, ym[4] = {0, 1, 5, 4}, yp[4] = {2, 3, 7, 6}, zp[4] = {1, 3, 7, 5}, zm[4] = {0, 2, 6, 4};
    float ox[3] = {1, 0, 0}, mx[3] = {-1, 0, 0}, my[3] = {0, -1, 0}, py[3] = {0, 1, 0}, pz[3] = {0, 0, 1}, mz[3] = {0, 0, -1};
    float sx[3] = {(float)side, 0, 0}, sxm[3] = {(float)-side, 0, 0};
    (void)ox; (void)mx;
    int *palm = side > 0 ? xp : xm, *back = side > 0 ? xm : xp;
    add_quad(g, 0, palm, sx, textured ? 1 : 0, 0x0019, 0x7C10, uA, tA);
    add_quad(g, 1, back, sxm, textured ? 2 : 0, 0x001A, 0x7C20, uB, tA);
    add_quad(g, 2, ym, my, textured ? 3 : 0, 0x001B, 0x7C30, uC, tA);
    add_quad(g, 3, yp, py, 0, 0, 0, uD, tA);
    add_quad(g, 4, zp, pz, textured ? 4 : 0, 0x001C, 0x7C40, uD, tA);
    add_quad(g, 5, zm, mz, textured ? 4 : 0, 0x001C, 0x7C40, uD, tA);
    NsMesh *m = &g->m;
    m->n_verts = 8; m->n_norms = 8; m->n_faces = 6; m->verts = g->v; m->norms = g->n; m->faces = g->f;
    m->flags = NS_MF_TWOSIDED; m->model_flags = 0x1234; m->depth_bias = 0;
    int16_t bb[6] = {-30, -125, -35, 30, 20, 35}; memcpy(m->bbox, bb, sizeof bb);
    m->id = ns_mesh_content_hash(m) ^ salt;   /* distintos ids para distintas manoplas */
}

typedef struct { NsScene sc; NsMeshStore *st; NsPlayerMap map; Glove gr, gl; NsMesh dummy; NsVert dv[4]; NsFace df[1]; NsNorm dn[1]; NsMesh fore; NsVert fv[8]; NsFace ff[4]; NsNorm fn[1]; } World;

static void xf_ident_t(NsXf *x, float tx, float ty, float tz, float yaw) {
    memset(x, 0, sizeof *x);
    float c = cosf(yaw), s = sinf(yaw);
    x->m[0] = c; x->m[2] = s; x->m[5] = 1; x->m[8] = -s; x->m[10] = c; x->m[3] = tx; x->m[7] = ty; x->m[11] = tz;
}
static int world_init(World *w, uint32_t cap_inst, uint32_t cap_piece, int textured) {
    memset(w, 0, sizeof *w);
    w->st = ns_store_create();
    if (!w->st) return 0;
    if (ns_scene_alloc_exact(&w->sc, cap_inst, cap_piece, 8, 8) != 0) return 0;
    make_glove(&w->gr, 1, textured, 0x1111); make_glove(&w->gl, -1, textured, 0x2222);
    ns_store_put(w->st, &w->gr.m); ns_store_put(w->st, &w->gl.m);
    /* malla ficticia del resto del cuerpo */
    w->dv[0] = (NsVert){0, 0, 0, 0xFF, 0}; w->dv[1] = (NsVert){10, 0, 0, 0xFF, 0}; w->dv[2] = (NsVert){10, 10, 0, 0xFF, 0}; w->dv[3] = (NsVert){0, 10, 0, 0xFF, 0};
    w->dn[0] = (NsNorm){0, 0, 4096, 0};
    memset(w->df, 0, sizeof w->df); w->df[0].v[1] = 1; w->df[0].v[2] = 2; w->df[0].v[3] = 3;
    w->dummy.n_verts = 4; w->dummy.n_norms = 1; w->dummy.n_faces = 1; w->dummy.verts = w->dv; w->dummy.norms = w->dn; w->dummy.faces = w->df;
    w->dummy.id = ns_mesh_content_hash(&w->dummy);
    ns_store_put(w->st, &w->dummy);
    /* antebrazo: anillo del codo soldado (y = 0) y anillo de la muñeca (y = -230) de 50 mm de radio, como el del juego */
    for (int i = 0; i < 8; i++) {
        float a = 1.5707963f * (float)(i & 3);
        w->fv[i].x = (int16_t)lrintf(50.0f * cosf(a)); w->fv[i].z = (int16_t)lrintf(50.0f * sinf(a)); w->fv[i].y = (int16_t)(i < 4 ? 0 : -230); w->fv[i].weld = (uint8_t)(i < 4 ? i : 0xFF);
    }
    w->fn[0] = (NsNorm){0, 0, 4096, 0};
    memset(w->ff, 0, sizeof w->ff);
    for (int k = 0; k < 4; k++) { w->ff[k].v[0] = (uint8_t)k; w->ff[k].v[1] = (uint8_t)((k + 1) & 3); w->ff[k].v[2] = (uint8_t)(4 + ((k + 1) & 3)); w->ff[k].v[3] = (uint8_t)(4 + k); }
    w->fore.n_verts = 8; w->fore.n_norms = 1; w->fore.n_faces = 4; w->fore.verts = w->fv; w->fore.norms = w->fn; w->fore.faces = w->ff; w->fore.flags = NS_MF_WELDED;
    w->fore.id = ns_mesh_content_hash(&w->fore);
    ns_store_put(w->st, &w->fore);
    NsScene *s = &w->sc;
    s->lights[0].amb[0] = 40; s->n_light = 1;
    s->n_inst = 2;
    s->inst[0].key = 0x80130000u; s->inst[0].first_piece = 0; s->inst[0].n_pieces = 1; s->inst[0].flags = NS_IF_STATIC; s->inst[0].light = -1;
    s->piece[0].mesh_id = w->dummy.id; s->piece[0].parent = -1; xf_ident_t(&s->piece[0].world, 0, 0, 0, 0);
    s->inst[1].key = 0x80120000u; s->inst[1].first_piece = 1; s->inst[1].n_pieces = 16; s->inst[1].flags = NS_IF_LIT | NS_IF_OWN_AMBIENT | NS_IF_RIGID;
    s->inst[1].zone_mask = 0x0005; s->inst[1].layer = 0; s->inst[1].light = 0; s->inst[1].vis_state = 3;
    for (int k = 0; k < 16; k++) {
        NsPiece *p = &s->piece[1 + k];
        p->mesh_id = k == NS_PART_HAND_R ? w->gr.m.id : k == NS_PART_HAND_L ? w->gl.m.id : (k == NS_PART_FORE_R || k == NS_PART_FORE_L) ? w->fore.id : w->dummy.id;
        p->parent = -1;
        xf_ident_t(&p->world, 100.0f * k, 1000.0f + k, -50.0f * k, 0.1f * k);
    }
    s->n_piece = 17;
    w->map.bound = 1; w->map.inst = 1; w->map.key = s->inst[1].key;
    for (int k = 0; k < 16; k++) { w->map.part[k].n = 1; w->map.part[k].idx[0] = 1 + (uint32_t)k; }
    return 1;
}
static void world_free(World *w) { ns_scene_free(&w->sc); ns_store_destroy(w->st); }

static double mat_err(const NsXf *a, const NsXf *b) { double e = 0; for (int i = 0; i < 12; i++) e += fabs((double)a->m[i] - b->m[i]); return e; }

static void test_classify(void) {
    Glove g;
    for (int side = -1; side <= 1; side += 2) {
        make_glove(&g, side, 1, 0);
        NsHandTex tx[4];
        int n = ns_hand_classify(&g.m, side, tx);
        CHECK(n == 4, "lado %d: roles encontrados %d", side, n);
        CHECK(tx[NS_HT_PALM].ok == 1 && tx[NS_HT_PALM].texid == 1 && tx[NS_HT_PALM].tpage == 0x19, "lado %d: palma texid=%d", side, tx[NS_HT_PALM].texid);
        CHECK(tx[NS_HT_BACK].ok == 1 && tx[NS_HT_BACK].texid == 2, "lado %d: dorso texid=%d", side, tx[NS_HT_BACK].texid);
        CHECK(tx[NS_HT_JOINT].ok == 1 && tx[NS_HT_JOINT].texid == 3, "lado %d: extremo texid=%d", side, tx[NS_HT_JOINT].texid);
        CHECK(tx[NS_HT_FINGER].ok == 1 && tx[NS_HT_FINGER].texid == 4, "lado %d: dedo texid=%d", side, tx[NS_HT_FINGER].texid);
    }
    /* sin texturas: nada; sólo la palma: el resto la toma prestada */
    make_glove(&g, 1, 0, 0);
    NsHandTex tx[4];
    CHECK(ns_hand_classify(&g.m, 1, tx) == 0 && !tx[0].ok && !tx[1].ok && !tx[2].ok && !tx[3].ok, "manopla plana: sin texturas");
    make_glove(&g, 1, 1, 0);
    g.f[1].texid = g.f[2].texid = g.f[4].texid = g.f[5].texid = 0;
    CHECK(ns_hand_classify(&g.m, 1, tx) == 1, "solo palma");
    for (int r = 0; r < 4; r++) CHECK(tx[r].ok && tx[r].texid == 1, "rol %d debe tomar la palma prestada (ok=%d texid=%d)", r, tx[r].ok, tx[r].texid);
    CHECK(ns_hand_classify(NULL, 1, tx) == 0, "NULL");
    NsMesh empty; memset(&empty, 0, sizeof empty);
    CHECK(ns_hand_classify(&empty, 1, tx) == 0, "vacía");
}

static void test_apply(void) {
    World w;
    CHECK(world_init(&w, 8, 64, 1), "mundo");
    ns_hand_reset();
    NsScene *s = &w.sc;
    NsHandIn in; memset(&in, 0, sizeof in);
    in.enabled = 1; in.grip[0] = 1.0f; in.grip[1] = 0.0f; in.driven[0] = 1; in.driven[1] = 1; in.item_pose[0] = in.item_pose[1] = HP_RELAX;
    NsPiece before[17]; memcpy(before, s->piece, sizeof before);
    NsXf rw = s->piece[1 + NS_PART_HAND_R].world, lw = s->piece[1 + NS_PART_HAND_L].world;
    in.units_per_m = 900.0f;
    int n = ns_hand_apply(s, w.st, &w.map, &in);
    const uint32_t NP = NS_HAND_PIECES;
    CHECK(n == 2, "se ponen las 2 manos (%d)", n);
    CHECK(s->n_inst == 4 && s->n_piece == 17 + 2 * NP, "instancias=%u piezas=%u", s->n_inst, s->n_piece);
    CHECK(s->piece[1 + NS_PART_HAND_R].flags & NS_PF_HIDDEN && s->piece[1 + NS_PART_HAND_L].flags & NS_PF_HIDDEN, "las manoplas quedan ocultas");
    int others = 0;
    for (int k = 0; k < 16; k++) if (k != NS_PART_HAND_R && k != NS_PART_HAND_L && (s->piece[1 + k].flags & NS_PF_HIDDEN)) others++;
    CHECK(others == 0, "el resto del cuerpo no se oculta (%d)", others);
    CHECK(memcmp(&before[0], &s->piece[0], sizeof before[0]) == 0, "el escenario no cambia");
    const NsInstance *ir = NULL, *il = NULL;
    for (uint32_t i = 0; i < s->n_inst; i++) { if (s->inst[i].key == NS_HAND_KEY_R) ir = &s->inst[i]; if (s->inst[i].key == NS_HAND_KEY_L) il = &s->inst[i]; }
    CHECK(ir && il && ir != il, "instancias de las manos");
    if (ir && il) {
        CHECK(ir->n_pieces == NP && il->n_pieces == NP && il->first_piece == 17 && ir->first_piece == 17 + NP, "reparto de piezas");
        CHECK(ir->light == 0 && ir->zone_mask == 5 && ir->vis_state == 3 && ir->layer == 0, "luz/zona/estado copiados del jugador");
        CHECK((ir->flags & (NS_IF_LIT | NS_IF_OWN_AMBIENT)) == (NS_IF_LIT | NS_IF_OWN_AMBIENT) && !(ir->flags & (NS_IF_RIGID | NS_IF_STATIC)), "flags: se hereda la luz y no RIGID/STATIC (%u)", ir->flags);
        /* vértices = skin de la pose (derecha agarre 0, izquierda agarre 1, sin objeto) * 0,9 (escala), matrices = la de la manopla */
        static float pr[VRHA_NV][3], pl[VRHA_NV][3];
        float xr[VRHS_J][12], xl[VRHS_J][12];
        {   /* misma cuantización que ns_hand (cierres a 1/32) */
            VrHandPoseIn a0 = {HP_RELAX, 0.0f, 0.0f, 0.0f, 0.0f}, a1 = {HP_RELAX, 1.0f, 0.0f, 0.0f, 0.0f};
            VrHandCurls c0, c1; vrhandpose_curls(&a0, &c0); vrhandpose_curls(&a1, &c1);
            for (int i = 0; i < 5; i++) { c0.curl[i] = roundf(c0.curl[i] * 32.0f) / 32.0f; c1.curl[i] = roundf(c1.curl[i] * 32.0f) / 32.0f; }
            vrhs_pose(c0.curl, roundf(c0.thumb_spread * 32.0f) / 32.0f, 1, xr); vrhs_pose(c1.curl, roundf(c1.thumb_spread * 32.0f) / 32.0f, -1, xl);
        }
        vrhs_skin(xr, 1, pr, NULL); vrhs_skin(xl, -1, pl, NULL);
        const float KQ = roundf(0.9f * 256.0f) / 256.0f;   /* escala cuantizada como en ns_hand */
        float verr = 0; int ntri = 0, okc = 1, wind = 0, wind_n = 0;
        for (uint32_t k = 0; k < NP; k++) {
            const NsPiece *qr = &s->piece[ir->first_piece + k], *ql = &s->piece[il->first_piece + k];
            const NsMesh *mr = ns_store_get(w.st, qr->mesh_id), *ml = ns_store_get(w.st, ql->mesh_id);
            CHECK(mr && ml, "pieza %u: malla en el almacén", k);
            if (!mr || !ml) continue;
            CHECK(ns_mesh_content_hash(mr) == mr->id && ns_mesh_content_hash(ml) == ml->id, "pieza %u: id = hash del contenido", k);
            CHECK(mr->flags & NS_MF_TWOSIDED && mr->model_flags == 0x1234 && !(mr->flags & NS_MF_WELDED), "pieza %u: banderas de la manopla", k);
            CHECK(qr->parent == -1 && ql->parent == -1 && qr->baked_ver == 0 && !(qr->flags & NS_PF_HIDDEN), "pieza %u", k);
            CHECK(mat_err(&qr->world, &rw) < 1e-6 && mat_err(&ql->world, &lw) < 1e-6, "pieza %u: matriz de la manopla", k);
            CHECK(mr->n_verts == vrha_piece[k].nv && mr->n_verts <= 256 && mr->n_faces == vrha_piece[k].nt, "pieza %u: %u vértices %u caras", k, mr->n_verts, mr->n_faces);
            for (uint32_t i = 0; i < mr->n_verts; i++) {
                const float *e = pr[vrha_piece[k].v0 + i], *f = pl[vrha_piece[k].v0 + i];
                verr = fmaxf(verr, fmaxf(fabsf(mr->verts[i].x - e[0] * KQ), fmaxf(fabsf(mr->verts[i].y - e[1] * KQ), fabsf(mr->verts[i].z - e[2] * KQ))));
                verr = fmaxf(verr, fmaxf(fabsf(ml->verts[i].x - f[0] * KQ), fmaxf(fabsf(ml->verts[i].y - f[1] * KQ), fabsf(ml->verts[i].z - f[2] * KQ))));
            }
            for (uint32_t f = 0; f < 2u * mr->n_faces; f++) {
                const NsMesh *mm_ = f < mr->n_faces ? mr : ml;
                const NsFace *F = &mm_->faces[f % mr->n_faces];
                if (!(F->flags & NS_FF_TRI) || F->v[2] != F->v[3]) okc = 0;
                for (int q = 0; q < 4; q++) if (F->v[q] >= mr->n_verts || F->n[q] != F->v[q]) okc = 0;
                /* convención del motor (las dos manos): cross(B-A, D-A) hacia el INTERIOR, como la normal almacenada */
                const NsVert *A = &mm_->verts[F->v[0]], *B = &mm_->verts[F->v[1]], *D = &mm_->verts[F->v[3]];
                float e1[3] = {B->x - A->x, B->y - A->y, B->z - A->z}, e2[3] = {D->x - A->x, D->y - A->y, D->z - A->z};
                float c[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                const NsNorm *nn = &mm_->norms[F->n[0]];
                float d = c[0] * nn->x + c[1] * nn->y + c[2] * nn->z;
                if (fabsf(d) > 1e-3f) { wind_n++; wind += d > 0; }
            }
            ntri += (int)ns_gpu_tri_count(mr);
            NsGpuVert gv[256]; static NsGpuCorner gc[3 * VRHA_NT];
            ns_gpu_pack(mr, NULL, gv, gc, 0, NULL, NULL);
            for (uint32_t c = 0; c < 3 * ns_gpu_tri_count(mr); c++) if (gc[c].v >= mr->n_verts) okc = 0;
        }
        CHECK(verr <= 0.51f, "vértices = skin de la pose * escala (error %.2f)", verr);
        CHECK(okc, "caras triangulares con índices válidos");
        CHECK(wind_n > 0 && wind * 100 >= wind_n * 95, "sentido de las caras (dos manos): %d de %d con la normal interior", wind, wind_n);
        CHECK(ntri == VRHA_NT, "triángulos por mano: %d", ntri);
        /* las caras llevan texturas de la manopla, dentro del rectángulo de su cara de referencia */
        int tex_ok = 0, flat = 0, in_rect = 1;
        for (uint32_t k = 0; k < NP; k++) {
            const NsMesh *pm = ns_store_get(w.st, s->piece[ir->first_piece + k].mesh_id);
            for (uint32_t f = 0; pm && f < pm->n_faces; f++) {
                int id = pm->faces[f].texid;
                if (id) tex_ok++; else flat++;
                uint8_t lo = id == 1 ? 8 : id == 2 ? 72 : id == 3 ? 136 : 200, hi = (uint8_t)(lo + 48);
                for (int q = 0; q < 4; q++) if (pm->faces[f].u[q] < lo || pm->faces[f].u[q] > hi || pm->faces[f].t[q] < 8 || pm->faces[f].t[q] > 56) in_rect = 0;
            }
        }
        CHECK(tex_ok > 0 && flat == 0 && in_rect, "texturas de la manopla (%d, planas %d, en el rectángulo %d)", tex_ok, flat, in_rect);
        /* antebrazo adelgazado: malla sustituida, soldados intactos, anillo de la muñeca más cerca del eje */
        const NsPiece *fr = &s->piece[1 + NS_PART_FORE_R];
        const NsMesh *fm = ns_store_get(w.st, fr->mesh_id);
        CHECK(fr->mesh_id != w.fore.id && fm && fm->n_verts == 8, "antebrazo sustituido");
        if (fm) {
            int weld_same = 1; float r = 0;
            for (int i = 0; i < 4; i++) if (memcmp(&fm->verts[i], &w.fv[i], sizeof w.fv[i])) weld_same = 0;
            for (int i = 4; i < 8; i++) r += sqrtf((float)fm->verts[i].x * fm->verts[i].x + (float)fm->verts[i].z * fm->verts[i].z) / 4.0f;
            float sl = ns_hand_fore_slim(&w.fore, 0.9f);
            CHECK(weld_same && sl >= 0.6f && sl < 0.95f && r < 50.0f * sl + 3.0f && r > 50.0f * sl - 6.0f, "antebrazo: soldados %d, factor %.2f, radio de la muñeca %.1f", weld_same, sl, r);
        }
    }
    /* determinista: otra escena igual da los mismos ids y matrices */
    World w2;
    world_init(&w2, 8, 64, 1);
    ns_hand_apply(&w2.sc, w2.st, &w2.map, &in);
    CHECK(w2.sc.n_piece == s->n_piece, "determinismo: piezas");
    int same = 1;
    for (uint32_t i = 17; i < s->n_piece && i < w2.sc.n_piece; i++) if (s->piece[i].mesh_id != w2.sc.piece[i].mesh_id || mat_err(&s->piece[i].world, &w2.sc.piece[i].world) > 1e-6) same = 0;
    CHECK(same, "determinismo: ids y matrices");
    world_free(&w2);
    /* mano que la capa VR no llevó: relajada (agarre 0) aunque llegue un agarre */
    World w3;
    world_init(&w3, 8, 64, 1);
    NsHandIn in3 = in; in3.driven[0] = 0; in3.driven[1] = 0; in3.grip[0] = 1; in3.grip[1] = 1;
    ns_hand_apply(&w3.sc, w3.st, &w3.map, &in3);
    NsHandIn in4 = in; in4.driven[0] = 1; in4.driven[1] = 1; in4.grip[0] = 0; in4.grip[1] = 0;
    World w4;
    world_init(&w4, 8, 64, 1);
    ns_hand_apply(&w4.sc, w4.st, &w4.map, &in4);
    same = w3.sc.n_piece == w4.sc.n_piece;
    for (uint32_t i = 17; same && i < w3.sc.n_piece; i++) if (mat_err(&w3.sc.piece[i].world, &w4.sc.piece[i].world) > 1e-6) same = 0;
    CHECK(same, "sin brazo manejado = agarre 0");
    world_free(&w3); world_free(&w4);
    /* ns_solid omite las manos como al cuerpo (no son geometría del mundo) */
    NsSolidOpts so; memset(&so, 0, sizeof so);
    static float tris[9 * 4096];
    NsSolidStats ss;
    size_t nt = ns_solid_collect(s, w.st, &so, tris, NULL, 4096, &ss);
    CHECK(ss.inst_skipped == 2, "ns_solid omite las 2 instancias de las manos (%u)", ss.inst_skipped);
    uint32_t ni = s->n_inst, np = s->n_piece;
    s->n_inst -= 2; s->n_piece -= 2 * NS_HAND_PIECES;
    size_t nt0 = ns_solid_collect(s, w.st, &so, tris, NULL, 4096, &ss);
    s->n_inst = ni; s->n_piece = np;
    CHECK(nt == nt0 && nt > 0, "la geometría sólida no cambia con las manos (%zu vs %zu)", nt, nt0);
    world_free(&w);
}

static void test_guards(void) {
    World w;
    NsHandStats st;
    NsHandIn in; memset(&in, 0, sizeof in);
    in.enabled = 1; in.driven[0] = in.driven[1] = 1; in.grip[0] = in.grip[1] = 0.5f; in.item_pose[0] = in.item_pose[1] = HP_RELAX;
    /* desactivado: nada */
    world_init(&w, 8, 64, 1); ns_hand_reset();
    NsHandIn off = in; off.enabled = 0;
    CHECK(ns_hand_apply(&w.sc, w.st, &w.map, &off) == 0 && w.sc.n_inst == 2 && w.sc.n_piece == 17 && !(w.sc.piece[5].flags & NS_PF_HIDDEN), "desactivado: la escena no cambia");
    CHECK(ns_hand_apply(NULL, w.st, &w.map, &in) == 0 && ns_hand_apply(&w.sc, NULL, &w.map, &in) == 0 && ns_hand_apply(&w.sc, w.st, NULL, &in) == 0 && ns_hand_apply(&w.sc, w.st, &w.map, NULL) == 0, "NULL");
    NsPlayerMap unb = w.map; unb.bound = 0;
    CHECK(ns_hand_apply(&w.sc, w.st, &unb, &in) == 0, "jugador no enlazado");
    world_free(&w);
    /* sin capacidad de instancias o de piezas: no se oculta nada */
    world_init(&w, 2, 64, 1); ns_hand_reset();
    CHECK(ns_hand_apply(&w.sc, w.st, &w.map, &in) == 0 && w.sc.n_inst == 2 && !(w.sc.piece[1 + NS_PART_HAND_R].flags & NS_PF_HIDDEN), "sin sitio para instancias: manoplas visibles");
    ns_hand_stats(&st);
    CHECK(st.no_room == 2, "no_room=%u", st.no_room);
    world_free(&w);
    world_init(&w, 8, 17 + NS_HAND_PIECES, 1); ns_hand_reset();   /* cabe UNA mano y la otra no */
    int n = ns_hand_apply(&w.sc, w.st, &w.map, &in);
    CHECK(n == 1 && w.sc.n_piece == 17 + NS_HAND_PIECES && w.sc.n_inst == 3, "cabe una mano (%d)", n);
    int hidden = ((w.sc.piece[1 + NS_PART_HAND_R].flags & NS_PF_HIDDEN) != 0) + ((w.sc.piece[1 + NS_PART_HAND_L].flags & NS_PF_HIDDEN) != 0);
    CHECK(hidden == 1, "solo se oculta la manopla de la mano que se pudo poner (%d)", hidden);
    world_free(&w);
    /* manopla ausente del almacén o parte no enlazada */
    world_init(&w, 8, 64, 1); ns_hand_reset();
    w.sc.piece[1 + NS_PART_HAND_R].mesh_id = 0xDEADBEEFull;
    w.map.part[NS_PART_HAND_L].n = 0;
    CHECK(ns_hand_apply(&w.sc, w.st, &w.map, &in) == 0 && w.sc.n_inst == 2, "sin malla / sin parte: no se pone nada");
    ns_hand_stats(&st);
    CHECK(st.no_mesh == 1 && st.no_part == 1, "no_mesh=%u no_part=%u", st.no_mesh, st.no_part);
    CHECK(!(w.sc.piece[1 + NS_PART_HAND_R].flags & NS_PF_HIDDEN), "la manopla sigue visible");
    world_free(&w);
    /* índices de pieza fuera de la escena */
    world_init(&w, 8, 64, 1); ns_hand_reset();
    w.map.part[NS_PART_HAND_R].idx[0] = 9999;
    w.map.inst = 77;
    CHECK(ns_hand_apply(&w.sc, w.st, &w.map, &in) == 0, "mapa hostil");
    world_free(&w);
    /* parte con pieza encadenada: solo se oculta la de la malla de la manopla */
    world_init(&w, 8, 64, 1); ns_hand_reset();
    w.map.part[NS_PART_HAND_R].n = 2; w.map.part[NS_PART_HAND_R].idx[1] = 0;      /* la segunda pieza es otra malla (el escenario) */
    w.sc.piece[0].flags = 0;
    ns_hand_apply(&w.sc, w.st, &w.map, &in);
    CHECK((w.sc.piece[1 + NS_PART_HAND_R].flags & NS_PF_HIDDEN) && !(w.sc.piece[0].flags & NS_PF_HIDDEN), "solo se oculta la pieza con la malla de la manopla");
    world_free(&w);
    /* una pieza de la manopla ya oculta por el juego: las nuevas también */
    world_init(&w, 8, 64, 1); ns_hand_reset();
    w.sc.piece[1 + NS_PART_HAND_R].flags = NS_PF_HIDDEN;
    ns_hand_apply(&w.sc, w.st, &w.map, &in);
    int hid = 0;
    for (uint32_t i = 0; i < w.sc.n_inst; i++) if (w.sc.inst[i].key == NS_HAND_KEY_R) for (int k = 0; k < NS_HAND_PIECES; k++) hid += (w.sc.piece[w.sc.inst[i].first_piece + (uint32_t)k].flags & NS_PF_HIDDEN) != 0;
    CHECK(hid == NS_HAND_PIECES, "manopla oculta por el juego = mano nueva oculta (%d)", hid);
    world_free(&w);
}

static void test_flat_and_epoch(void) {
    World w;
    NsHandStats st;
    NsHandIn in; memset(&in, 0, sizeof in);
    in.enabled = 1; in.driven[0] = in.driven[1] = 1; in.item_pose[0] = in.item_pose[1] = HP_RELAX;
    /* manopla sin texturas: manos planas */
    world_init(&w, 8, 64, 0); ns_hand_reset();
    CHECK(ns_hand_apply(&w.sc, w.st, &w.map, &in) == 2, "manos planas");
    ns_hand_stats(&st);
    CHECK(st.flat == 2, "flat=%u", st.flat);
    const NsMesh *m = ns_store_get(w.st, w.sc.piece[w.sc.inst[w.sc.n_inst - 1].first_piece].mesh_id);
    int tex = 0;
    for (uint32_t f = 0; m && f < m->n_faces; f++) tex += m->faces[f].texid != 0 || m->faces[f].tpage != 0;
    CHECK(m && tex == 0, "sin textura: caras planas (%d)", tex);
    world_free(&w);
    /* épocas del almacén: tras abrir una nueva las mallas se ofrecen otra vez */
    world_init(&w, 8, 128, 1); ns_hand_reset();
    ns_hand_apply(&w.sc, w.st, &w.map, &in);
    uint64_t id0 = w.sc.piece[w.sc.inst[w.sc.n_inst - 1].first_piece].mesh_id;
    const NsMesh *m1 = ns_store_get(w.st, id0);
    CHECK(m1 && m1->epoch == 1, "época 1");
    CHECK(ns_store_new_epoch(w.st) == 1 && ns_store_epoch(w.st) == 2, "época nueva");
    /* nueva escena (la manopla y el muñeco también deben reponerse como haría el extractor) */
    ns_store_put(w.st, &w.gr.m); ns_store_put(w.st, &w.gl.m);
    w.sc.n_inst = 2; w.sc.n_piece = 17;
    w.sc.piece[1 + NS_PART_HAND_R].flags = 0; w.sc.piece[1 + NS_PART_HAND_L].flags = 0;
    CHECK(ns_hand_apply(&w.sc, w.st, &w.map, &in) == 2, "tras la época nueva");
    const NsMesh *m2 = ns_store_get(w.st, id0);
    CHECK(m2 && m2->epoch == 2, "la malla de la mano se reofrece en la época nueva (época %u)", m2 ? m2->epoch : 0);
    ns_store_ack(w.st, 2);
    ns_store_put(w.st, &w.gr.m);
    ns_store_put(w.st, &w.dummy);
    CHECK(ns_store_get(w.st, id0) != NULL, "tras el ack sigue disponible");
    ns_hand_stats(&st);
    CHECK(st.rebuilt == 2, "no se reconstruye por cambiar de época (%u)", st.rebuilt);
    /* otra manopla (otro id): una reconstrucción como mucho cada 30 fotogramas, mientras tanto se siguen usando las mallas anteriores */
    Glove g2; make_glove(&g2, 1, 1, 0x9999);
    ns_store_put(w.st, &g2.m);
    uint32_t bytes0 = (uint32_t)ns_store_bytes(w.st);
    for (int f = 0; f < 10; f++) {      /* la manopla cambia de id a cada fotograma */
        Glove gf; make_glove(&gf, 1, 1, 0xA000u + (uint64_t)f); ns_store_put(w.st, &gf.m);
        w.sc.n_inst = 2; w.sc.n_piece = 17;
        w.sc.piece[1 + NS_PART_HAND_R].flags = 0; w.sc.piece[1 + NS_PART_HAND_L].flags = 0;
        w.sc.piece[1 + NS_PART_HAND_R].mesh_id = gf.m.id;
        ns_hand_apply(&w.sc, w.st, &w.map, &in);
    }
    ns_hand_stats(&st);
    CHECK(st.rebuilt == 2, "id inestable: sin reconstrucciones nuevas (%u)", st.rebuilt);
    CHECK(ns_store_bytes(w.st) < bytes0 + 12000, "el almacén no crece con la manopla inestable (%zu -> %zu)", (size_t)bytes0, ns_store_bytes(w.st));
    for (int f = 0; f < 30; f++) {
        w.sc.n_inst = 2; w.sc.n_piece = 17;
        w.sc.piece[1 + NS_PART_HAND_R].flags = 0; w.sc.piece[1 + NS_PART_HAND_L].flags = 0;
        w.sc.piece[1 + NS_PART_HAND_R].mesh_id = g2.m.id;
        ns_hand_apply(&w.sc, w.st, &w.map, &in);
    }
    ns_hand_stats(&st);
    CHECK(st.rebuilt == 3, "pasados 30 fotogramas, reconstrucción (%u)", st.rebuilt);
    world_free(&w);
    /* almacén lleno: no se pone ni se oculta nada */
    world_init(&w, 8, 64, 1); ns_hand_reset();
    ns_store_set_limit(w.st, ns_store_bytes(w.st));
    CHECK(ns_hand_apply(&w.sc, w.st, &w.map, &in) == 0 && w.sc.n_inst == 2 && w.sc.n_piece == 17, "almacén lleno: la escena no cambia");
    CHECK(!(w.sc.piece[1 + NS_PART_HAND_R].flags & NS_PF_HIDDEN) && !(w.sc.piece[1 + NS_PART_HAND_L].flags & NS_PF_HIDDEN), "almacén lleno: manoplas visibles");
    ns_hand_stats(&st);
    CHECK(st.put_fail == 2, "put_fail=%u", st.put_fail);
    world_free(&w);
}

static void test_store_put(void) {
    NsMeshStore *st = ns_store_create();
    Glove g; make_glove(&g, 1, 1, 0);
    CHECK(ns_store_put(st, &g.m) == 1 && ns_store_put(st, &g.m) == 0, "añadir y repetir");
    const NsMesh *m = ns_store_get(st, g.m.id);
    CHECK(m && m->verts != g.v && m->n_faces == 6 && memcmp(m->faces, g.f, sizeof g.f) == 0, "copia propia");
    NsMesh bad = g.m; bad.verts = NULL;
    CHECK(ns_store_put(st, &bad) == -1 && ns_store_put(st, NULL) == -1 && ns_store_put(NULL, &g.m) == -1, "inválidas");
    bad = g.m; bad.n_faces = 0;
    CHECK(ns_store_put(st, &bad) == -1, "sin caras");
    ns_store_destroy(st);
}

/* Pose mezclada (vr_hand_pose): objeto en la mano, gatillo que cierra el índice solo si la pose lleva gatillo, pulgar; la mano que la IK no llevó ignora todo. Se compara la malla de la pieza que
 * contiene cada dedo (pieza 0 = palma; las piezas se agrupan por dedo). */
typedef struct { uint64_t id[VRHA_NP]; } Ids;
static void apply_pose(Ids *o, int pose, float grip, float trig, float thumb, int driven) {
    World w;
    world_init(&w, 8, 64, 1); ns_hand_reset();
    NsHandIn in; memset(&in, 0, sizeof in);
    in.enabled = 1; in.driven[0] = in.driven[1] = driven; in.item_pose[0] = in.item_pose[1] = pose;
    in.grip[0] = in.grip[1] = grip; in.trigger[0] = in.trigger[1] = trig; in.thumb[0] = in.thumb[1] = thumb;
    ns_hand_apply(&w.sc, w.st, &w.map, &in);
    memset(o, 0, sizeof *o);
    for (uint32_t i = 0; i < w.sc.n_inst; i++) if (w.sc.inst[i].key == NS_HAND_KEY_R) for (int k = 0; k < VRHA_NP; k++) o->id[k] = w.sc.piece[w.sc.inst[i].first_piece + (uint32_t)k].mesh_id;
    world_free(&w);
}
static int n_diff(const Ids *a, const Ids *b) { int n = 0; for (int k = 0; k < VRHA_NP; k++) n += a->id[k] != b->id[k]; return n; }
static void test_pose_blend(void) {
    Ids a, b, c, d, h0, h1, t0, t1, u, r0;
    apply_pose(&a, HP_GRIP_PISTOL, 1.0f, 0.0f, 0.0f, 1);
    apply_pose(&b, HP_GRIP_PISTOL, 1.0f, 1.0f, 0.0f, 1);
    CHECK(n_diff(&a, &b) >= 1 && n_diff(&a, &b) < VRHA_NP, "pistola: el gatillo cambia solo las piezas del índice (%d)", n_diff(&a, &b));
    apply_pose(&c, HP_RELAX, 1.0f, 0.0f, 0.0f, 1);
    apply_pose(&d, HP_RELAX, 1.0f, 1.0f, 0.0f, 1);
    CHECK(n_diff(&c, &d) == 0, "sin objeto el gatillo no mueve la mano");
    CHECK(n_diff(&a, &c) > 0, "pistola y puño sin objeto difieren");
    apply_pose(&h0, HP_GRIP_PISTOL, 0.0f, 0.0f, 0.0f, 1);
    CHECK(n_diff(&h0, &a) == 0, "pistola en la mano sin botón de agarre = con agarre a fondo");
    apply_pose(&h1, HP_RELAX, 0.0f, 0.0f, 0.0f, 1);
    CHECK(n_diff(&h1, &h0) > 0, "sin objeto y sin agarre la mano sigue relajada, no empuña");
    apply_pose(&h1, HP_GRIP_PISTOL, 0.0f, 1.0f, 0.0f, 1);
    CHECK(n_diff(&h0, &h1) > 0, "sin botón de agarre el gatillo aún cierra el índice");
    apply_pose(&t0, HP_GRIP_PISTOL, 1.0f, 0.0f, 0.0f, 1);
    apply_pose(&t1, HP_GRIP_PISTOL, 1.0f, 0.0f, 1.0f, 1);
    CHECK(n_diff(&t0, &t1) >= 1, "botón del pulgar: cambia la mano (%d)", n_diff(&t0, &t1));
    apply_pose(&u, HP_GRIP_PISTOL, 1.0f, 1.0f, 1.0f, 0);
    apply_pose(&r0, HP_RELAX, 0.0f, 0.0f, 0.0f, 1);
    CHECK(n_diff(&u, &r0) == 0, "sin brazo manejado: mano relajada");
    /* caché: la misma pose no crea mallas nuevas; un gatillo que varía poco (mismo paso de cuantización) tampoco */
    World w; world_init(&w, 8, 128, 1); ns_hand_reset();
    NsHandIn in; memset(&in, 0, sizeof in);
    in.enabled = 1; in.driven[0] = in.driven[1] = 1; in.item_pose[0] = in.item_pose[1] = HP_GRIP_PISTOL; in.grip[0] = in.grip[1] = 1;
    NsHandStats st;
    for (int f = 0; f < 20; f++) {
        w.sc.n_inst = 2; w.sc.n_piece = 17; w.sc.piece[1 + NS_PART_HAND_R].flags = 0; w.sc.piece[1 + NS_PART_HAND_L].flags = 0;
        w.sc.piece[1 + NS_PART_FORE_R].mesh_id = w.fore.id; w.sc.piece[1 + NS_PART_FORE_L].mesh_id = w.fore.id;
        in.trigger[0] = in.trigger[1] = 0.2f + 0.0005f * (float)f;
        ns_hand_apply(&w.sc, w.st, &w.map, &in);
    }
    ns_hand_stats(&st);
    CHECK(st.skinned == 2 && st.slimmed == 40, "caché de poses: %u poses calculadas, %u antebrazos", st.skinned, st.slimmed);
    world_free(&w);
}

int main(void) {
    test_store_put();
    test_classify();
    test_apply();
    test_guards();
    test_flat_and_epoch();
    test_pose_blend();
    printf("%s ns_hand: %d comprobaciones, %d fallos\n", fails ? "FALLO" : "OK", checks, fails);
    return fails ? 1 : 0;
}
