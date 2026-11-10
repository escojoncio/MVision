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

typedef struct { NsScene sc; NsMeshStore *st; NsPlayerMap map; Glove gr, gl; NsMesh dummy; NsVert dv[4]; NsFace df[1]; NsNorm dn[1]; NsMesh fore; NsVert fv[12]; NsFace ff[8]; NsNorm fn[1]; NsMesh upper; NsVert uv[8]; NsFace uf[4]; } World;

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
    /* antebrazo: anillo del codo soldado al brazo (0..3, y = 0), de la muñeca (4..7, y = -230) e intermedio (8..11, y = -80), 50 mm de radio, como el del juego */
    for (int i = 0; i < 12; i++) {
        float a = 1.5707963f * (float)(i & 3);
        const float y = i < 4 ? 0.0f : i < 8 ? -230.0f : -80.0f;   /* anillos centrados en el eje codo -> muñeca (0, -249, 47) */
        w->fv[i].x = (int16_t)lrintf(50.0f * cosf(a)); w->fv[i].z = (int16_t)lrintf(50.0f * sinf(a) - y * 47.0f / 249.0f); w->fv[i].y = (int16_t)y; w->fv[i].weld = (uint8_t)(i < 4 ? i : 0xFF);
    }
    w->fn[0] = (NsNorm){0, 0, 4096, 0};
    memset(w->ff, 0, sizeof w->ff);
    for (int k = 0; k < 4; k++) {
        w->ff[k].v[0] = (uint8_t)k; w->ff[k].v[1] = (uint8_t)((k + 1) & 3); w->ff[k].v[2] = (uint8_t)(8 + ((k + 1) & 3)); w->ff[k].v[3] = (uint8_t)(8 + k);
        w->ff[4 + k].v[0] = (uint8_t)(8 + k); w->ff[4 + k].v[1] = (uint8_t)(8 + ((k + 1) & 3)); w->ff[4 + k].v[2] = (uint8_t)(4 + ((k + 1) & 3)); w->ff[4 + k].v[3] = (uint8_t)(4 + k);
    }
    w->fore.n_verts = 12; w->fore.n_norms = 1; w->fore.n_faces = 8; w->fore.verts = w->fv; w->fore.norms = w->fn; w->fore.faces = w->ff; w->fore.flags = NS_MF_WELDED;
    w->fore.id = ns_mesh_content_hash(&w->fore);
    ns_store_put(w->st, &w->fore);
    /* brazo: anillo del codo (0..3) donde se sueldan los del antebrazo (codo en (0, -260, -50) de su marco) y anillo del hombro (4..7, abierto) */
    for (int i = 0; i < 8; i++) {
        float a = 1.5707963f * (float)(i & 3), r = i < 4 ? 50.0f : 60.0f;
        w->uv[i].x = (int16_t)lrintf(r * cosf(a)); w->uv[i].z = (int16_t)lrintf(r * sinf(a) + (i < 4 ? -50.0f : 0.0f)); w->uv[i].y = (int16_t)(i < 4 ? -260 : 0); w->uv[i].weld = 0xFF;
    }
    memset(w->uf, 0, sizeof w->uf);
    for (int k = 0; k < 4; k++) { w->uf[k].v[0] = (uint8_t)(4 + k); w->uf[k].v[1] = (uint8_t)(4 + ((k + 1) & 3)); w->uf[k].v[2] = (uint8_t)((k + 1) & 3); w->uf[k].v[3] = (uint8_t)k; }
    w->upper.n_verts = 8; w->upper.n_norms = 1; w->upper.n_faces = 4; w->upper.verts = w->uv; w->upper.norms = w->fn; w->upper.faces = w->uf;
    w->upper.id = ns_mesh_content_hash(&w->upper);
    ns_store_put(w->st, &w->upper);
    NsScene *s = &w->sc;
    s->lights[0].amb[0] = 40; s->lights[0].amb[1] = 90; s->lights[0].amb[2] = 20; s->n_light = 1;   /* ambiente y luces con tinte (verdoso / azulado) para comprobar la luz neutra de las manos */
    s->lights[0].col[0][0] = 400; s->lights[0].col[1][0] = 900; s->lights[0].col[2][0] = 200; s->lights[0].col[0][1] = 100; s->lights[0].col[1][1] = 100; s->lights[0].col[2][1] = 800;
    s->n_inst = 2;
    s->inst[0].key = 0x80130000u; s->inst[0].first_piece = 0; s->inst[0].n_pieces = 1; s->inst[0].flags = NS_IF_STATIC; s->inst[0].light = -1;
    s->piece[0].mesh_id = w->dummy.id; s->piece[0].parent = -1; xf_ident_t(&s->piece[0].world, 0, 0, 0, 0);
    s->inst[1].key = 0x80120000u; s->inst[1].first_piece = 1; s->inst[1].n_pieces = 16; s->inst[1].flags = NS_IF_LIT | NS_IF_OWN_AMBIENT | NS_IF_RIGID;
    s->inst[1].zone_mask = 0x0005; s->inst[1].layer = 0; s->inst[1].light = 0; s->inst[1].vis_state = 3;
    for (int k = 0; k < 16; k++) {
        NsPiece *p = &s->piece[1 + k];
        p->mesh_id = k == NS_PART_HAND_R ? w->gr.m.id : k == NS_PART_HAND_L ? w->gl.m.id : (k == NS_PART_FORE_R || k == NS_PART_FORE_L) ? w->fore.id :
                     (k == NS_PART_UPPER_R || k == NS_PART_UPPER_L) ? w->upper.id : w->dummy.id;
        p->parent = (int16_t)(k == NS_PART_FORE_R ? NS_PART_UPPER_R : k == NS_PART_FORE_L ? NS_PART_UPPER_L : -1);   /* el antebrazo se suelda al brazo */
        xf_ident_t(&p->world, 100.0f * k, 1000.0f + k, -50.0f * k, 0.1f * k);
    }
    for (int sd = 0; sd < 2; sd++) {   /* la mano cuelga del antebrazo como en el modelo: muñeca en (0, -249, 47) de su marco, mismos ejes */
        const NsXf *f = &s->piece[1 + (sd ? NS_PART_FORE_R : NS_PART_FORE_L)].world;
        NsXf *hw = &s->piece[1 + (sd ? NS_PART_HAND_R : NS_PART_HAND_L)].world;
        *hw = *f;
        for (int i = 0; i < 3; i++) hw->m[4 * i + 3] = f->m[4 * i + 3] + f->m[4 * i + 1] * -249.0f + f->m[4 * i + 2] * 47.0f;
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
        CHECK(ir->zone_mask == 5 && ir->vis_state == 3 && ir->layer == 0, "zona/estado copiados del jugador");
        /* luz neutra: luz propia nueva (la del jugador no se toca), cada luz y el ambiente con los tres canales iguales y la luminancia de los originales */
        const NsLight *pl0 = &s->lights[0];
        CHECK(ir->light > 0 && il->light > 0 && ir->light != il->light && (uint32_t)ir->light < s->n_light && (uint32_t)il->light < s->n_light && s->n_light == 3, "luz propia de cada mano (%d, %d, n=%u)", ir->light, il->light, s->n_light);
        if (ir->light > 0 && (uint32_t)ir->light < s->n_light) {
            const NsLight *nl = &s->lights[ir->light];
            const int ay = (77 * pl0->amb[0] + 150 * pl0->amb[1] + 29 * pl0->amb[2] + 128) >> 8;
            int neutral = nl->amb[0] == nl->amb[1] && nl->amb[1] == nl->amb[2] && nl->amb[0] == ay && ay > 0 && ay != pl0->amb[0] && ay != pl0->amb[1];
            for (int l = 0; l < 3; l++) {
                const int y = (77 * pl0->col[0][l] + 150 * pl0->col[1][l] + 29 * pl0->col[2][l] + 128) >> 8;
                neutral &= nl->col[0][l] == y && nl->col[1][l] == y && nl->col[2][l] == y;
                neutral &= memcmp(nl->dir[l], pl0->dir[l], sizeof nl->dir[l]) == 0;
            }
            CHECK(neutral && pl0->amb[1] == 90 && pl0->col[1][0] == 900, "luz de la mano: solo luminancia, sin tinte (amb %d %d %d)", nl->amb[0], nl->amb[1], nl->amb[2]);
        }
        CHECK((ir->flags & (NS_IF_LIT | NS_IF_OWN_AMBIENT)) == (NS_IF_LIT | NS_IF_OWN_AMBIENT) && !(ir->flags & (NS_IF_RIGID | NS_IF_STATIC)), "flags: se hereda la luz y no RIGID/STATIC (%u)", ir->flags);
        /* vértices = skin de la pose (derecha agarre 0, izquierda agarre 1, sin objeto) fuera del puño; matrices = la de la manopla; pieza 0 = anillo de la costura */
        static float pr[VRHA_NV][3], pl[VRHA_NV][3];
        float xr[VRHS_J][12], xl[VRHS_J][12];
        {   /* misma cuantización que ns_hand (cierres a 1/32) */
            VrHandPoseIn a0 = {HP_RELAX, 0.0f, 0.0f, 0.0f, 0.0f}, a1 = {HP_RELAX, 1.0f, 0.0f, 0.0f, 0.0f};
            VrHandCurls c0, c1; vrhandpose_curls(&a0, &c0); vrhandpose_curls(&a1, &c1);
            for (int i = 0; i < 5; i++) { c0.curl[i] = roundf(c0.curl[i] * 32.0f) / 32.0f; c1.curl[i] = roundf(c1.curl[i] * 32.0f) / 32.0f; }
            vrhs_pose(c0.curl, roundf(c0.thumb_spread * 32.0f) / 32.0f, 1, xr); vrhs_pose(c1.curl, roundf(c1.thumb_spread * 32.0f) / 32.0f, -1, xl);
        }
        vrhs_skin(xr, 1, pr, NULL); vrhs_skin(xl, -1, pl, NULL);
        float verr = 0; int ntri = 0, okc = 1, wind = 0, wind_n = 0;
        for (uint32_t k = 0; k < NP; k++) {
            if (k < 2) continue;   /* anillo y transición: se comprueban aparte */
            const NsPiece *qr = &s->piece[ir->first_piece + k], *ql = &s->piece[il->first_piece + k];
            const uint32_t hk = k - 2;
            const NsMesh *mr = ns_store_get(w.st, qr->mesh_id), *ml = ns_store_get(w.st, ql->mesh_id);
            CHECK(mr && ml, "pieza %u: malla en el almacén", k);
            if (!mr || !ml) continue;
            CHECK(ns_mesh_content_hash(mr) == mr->id && ns_mesh_content_hash(ml) == ml->id, "pieza %u: id = hash del contenido", k);
            CHECK(mr->flags & NS_MF_TWOSIDED && mr->model_flags == 0x1234, "pieza %u: banderas de la manopla", k);
            CHECK(qr->parent == ((mr->flags & NS_MF_WELDED) ? 0 : -1) && ql->parent == ((ml->flags & NS_MF_WELDED) ? 0 : -1) && qr->baked_ver == 0 && !(qr->flags & NS_PF_HIDDEN), "pieza %u: padre = anillo si lleva soldaduras", k);
            CHECK(mat_err(&qr->world, &rw) < 1e-6 && mat_err(&ql->world, &lw) < 1e-6, "pieza %u: matriz de la manopla", k);
            CHECK(mr->n_verts == vrha_piece[hk].nv && mr->n_verts <= 256 && mr->n_faces == vrha_piece[hk].nt, "pieza %u: %u vértices %u caras", k, mr->n_verts, mr->n_faces);
            for (uint32_t i = 0; i < mr->n_verts; i++) {
                const float *e = pr[vrha_piece[hk].v0 + i], *f = pl[vrha_piece[hk].v0 + i];
                if (e[1] > -32.0f || f[1] > -32.0f) continue;   /* el puño se deforma hacia el antebrazo */
                verr = fmaxf(verr, fmaxf(fabsf(mr->verts[i].x - e[0]), fmaxf(fabsf(mr->verts[i].y - e[1]), fabsf(mr->verts[i].z - e[2]))));
                verr = fmaxf(verr, fmaxf(fabsf(ml->verts[i].x - f[0]), fmaxf(fabsf(ml->verts[i].y - f[1]), fabsf(ml->verts[i].z - f[2]))));
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
            if (k >= 2) ntri += (int)ns_gpu_tri_count(mr);
            NsGpuVert gv[256]; static NsGpuCorner gc[3 * VRHA_NT];
            ns_gpu_pack(mr, NULL, gv, gc, 0, NULL, NULL);
            for (uint32_t c = 0; c < 3 * ns_gpu_tri_count(mr); c++) if (gc[c].v >= mr->n_verts) okc = 0;
        }
        CHECK(verr <= 0.51f, "vértices = skin de la pose * escala (error %.2f)", verr);
        CHECK(okc, "caras triangulares con índices válidos");
        CHECK(wind_n > 0 && wind * 100 >= wind_n * 95, "sentido de las caras (dos manos): %d de %d con la normal interior", wind, wind_n);
        CHECK(ntri == VRHA_NT, "triángulos por mano: %d", ntri);
        /* unión: anillo = los 4 vértices del extremo del antebrazo (copia exacta, matriz del antebrazo); la transición se suelda a ellos (en reposo caen donde el anillo los pone) */
        const NsPiece *rp = &s->piece[ir->first_piece];
        const NsMesh *rm = ns_store_get(w.st, rp->mesh_id);
        int same = rm && rm->n_verts == 4;
        for (uint32_t i = 0; same && i < 4; i++) { int found = 0; for (int j = 4; j < 8; j++) found |= rm->verts[i].x == w.fv[j].x && rm->verts[i].y == w.fv[j].y && rm->verts[i].z == w.fv[j].z; same &= found; }
        CHECK(same && mat_err(&rp->world, &s->piece[1 + NS_PART_FORE_R].world) < 1e-6, "anillo = vértices del extremo del antebrazo (%u) con su matriz", rm ? rm->n_verts : 0);
        float wgap = 0; int nwv = 0;
        for (uint32_t k = 1; rm && k < 2; k++) {
            const NsPiece *qr = &s->piece[ir->first_piece + k];
            const NsMesh *mr = ns_store_get(w.st, qr->mesh_id);
            for (uint32_t i = 0; mr && i < mr->n_verts; i++) {
                const NsVert *v = &mr->verts[i];
                if (v->weld == 0xFF) continue;
                nwv++;
                if (v->weld >= rm->n_verts) { wgap = 1e9f; continue; }
                float a[3] = {v->x, v->y, v->z}, b[3] = {rm->verts[v->weld].x, rm->verts[v->weld].y, rm->verts[v->weld].z}, pa[3], pb[3];
                ns_xf_apply(&qr->world, a, pa); ns_xf_apply(&rp->world, b, pb);
                wgap = fmaxf(wgap, sqrtf((pa[0] - pb[0]) * (pa[0] - pb[0]) + (pa[1] - pb[1]) * (pa[1] - pb[1]) + (pa[2] - pb[2]) * (pa[2] - pb[2])));
            }
        }
        CHECK(rm && nwv == (int)rm->n_verts && wgap < 1.0f, "transición soldada al antebrazo: %d vértices, hueco en reposo %.2f", nwv, wgap);
        /* las caras llevan la textura propia del guante (ns_hand_glove_block), dentro de la textura (u 128..191, t 0..31): el dorso (u < 176), la tela (u >= 176, t < 16) y, la transición con
         * el antebrazo (pieza 1), la zona del puño (u 184, t = 16 + 3 * anillo) */
        int tex_ok = 0, bad = 0, dors = 0, cloth = 0;
        for (uint32_t k = 1; k < NP; k++) {
            const NsMesh *pm = ns_store_get(w.st, s->piece[ir->first_piece + k].mesh_id);
            for (uint32_t f = 0; pm && f < pm->n_faces; f++) {
                const NsFace *F = &pm->faces[f];
                if (F->texid == NS_GLOVE_TEXID && F->tpage == NS_GLOVE_TPAGE && F->clut == NS_GLOVE_CLUT) tex_ok++; else bad++;
                int all_dors = 1;
                for (int q = 0; q < 4; q++) {
                    if (F->u[q] < NS_GLOVE_U0 || F->u[q] > NS_GLOVE_U0 + 63 || F->t[q] > 31) bad++;
                    if (F->u[q] >= NS_GLOVE_U0 + 48) { all_dors = 0; if (k == 1 ? (F->u[q] != 184 || F->t[q] < 16 || (F->t[q] - 16) % 3 != 0) : F->t[q] > 15) bad++; }
                }
                if (k >= 2) { if (all_dors) dors++; else cloth++; }
            }
        }
        CHECK(tex_ok > 0 && bad == 0 && dors > 50 && cloth > dors, "texturas propias del guante (%d caras, mal %d, dorso %d, tela %d)", tex_ok, bad, dors, cloth);
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
    /* sin sitio para la luz neutra: las manos se ponen igual con la luz del jugador */
    world_init(&w, 8, 64, 1); ns_hand_reset();
    w.sc.n_light = w.sc.cap_light;
    n = ns_hand_apply(&w.sc, w.st, &w.map, &in);
    CHECK(n == 2 && w.sc.n_light == w.sc.cap_light && w.sc.inst[2].light == 0 && w.sc.inst[3].light == 0, "sin sitio para luces: se usa la del jugador (%d)", n);
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
    const NsMesh *m = ns_store_get(w.st, w.sc.piece[w.sc.inst[w.sc.n_inst - 1].first_piece + 1].mesh_id);
    int tex = 0;
    for (uint32_t f = 0; m && f < m->n_faces; f++) tex += m->faces[f].texid != 0 || m->faces[f].tpage != 0;
    CHECK(m && tex == 0, "sin textura: caras planas (%d)", tex);
    world_free(&w);
    /* épocas del almacén: tras abrir una nueva las mallas se ofrecen otra vez */
    world_init(&w, 8, 128, 1); ns_hand_reset();
    ns_hand_apply(&w.sc, w.st, &w.map, &in);
    uint64_t id0 = w.sc.piece[w.sc.inst[w.sc.n_inst - 1].first_piece + 1].mesh_id;
    const NsMesh *m1 = ns_store_get(w.st, id0);
    CHECK(m1 && m1->epoch == 1, "época 1");
    CHECK(ns_store_new_epoch(w.st) == 1 && ns_store_epoch(w.st) == 2, "época nueva");
    /* nueva escena (la manopla y el muñeco también deben reponerse como haría el extractor) */
    ns_store_put(w.st, &w.gr.m); ns_store_put(w.st, &w.gl.m); ns_store_put(w.st, &w.fore); ns_store_put(w.st, &w.upper);
    w.sc.n_inst = 2; w.sc.n_piece = 17;
    w.sc.piece[1 + NS_PART_HAND_R].flags = 0; w.sc.piece[1 + NS_PART_HAND_L].flags = 0;
    CHECK(ns_hand_apply(&w.sc, w.st, &w.map, &in) == 2, "tras la época nueva");
    const NsMesh *m2 = ns_store_get(w.st, id0);
    CHECK(m2 && m2->epoch == 2, "la malla de la mano se reofrece en la época nueva (época %u)", m2 ? m2->epoch : 0);
    ns_store_ack(w.st, 2);
    ns_store_put(w.st, &w.gr.m); ns_store_put(w.st, &w.fore); ns_store_put(w.st, &w.upper);
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
    for (uint32_t i = 0; i < w.sc.n_inst; i++) if (w.sc.inst[i].key == NS_HAND_KEY_R) for (int k = 0; k < VRHA_NP; k++) o->id[k] = w.sc.piece[w.sc.inst[i].first_piece + w.sc.inst[i].n_pieces - VRHA_NP + (uint32_t)k].mesh_id;
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
        w.sc.piece[1 + NS_PART_FORE_R].mesh_id = w.fore.id; w.sc.piece[1 + NS_PART_FORE_L].mesh_id = w.fore.id;   /* el extractor repone cada fotograma la malla del motor */
        in.trigger[0] = in.trigger[1] = 0.2f + 0.0005f * (float)f;
        ns_hand_apply(&w.sc, w.st, &w.map, &in);
    }
    ns_hand_stats(&st);
    CHECK(st.skinned == 2 && st.seams == 2, "caché de poses: %u poses calculadas, %u costuras", st.skinned, st.seams);
    world_free(&w);
}


/* Brazo + antebrazo + transición + mano: UNA superficie cerrada. En >= 300 poses aleatorias (codo doblado 0..140 grados y antebrazo girado +-180 sobre su eje, como lo deja la IK; muñeca
 * dentro de sus límites: giro +-100 grados, flexión 80 / extensión 70, desviación radial 20 / cubital 30) se juntan los triángulos del brazo, del antebrazo y de la instancia de la mano
 * (soldaduras resueltas con la matriz del padre, como la GPU) y se cuentan las aristas: las únicas abiertas son las del hombro (4), ninguna la comparten más de dos triángulos y el giro no
 * estrecha el antebrazo (área >= 75 % de la de reposo). */
typedef struct { float p[3]; int piece; } WV;
static void rot_axis_m(float *R, const float a[3], float deg) {
    float c = cosf(deg * 0.0174533f), s_ = sinf(deg * 0.0174533f), t = 1 - c, x = a[0], y = a[1], z = a[2];
    float r[9] = {t * x * x + c, t * x * y - s_ * z, t * x * z + s_ * y, t * x * y + s_ * z, t * y * y + c, t * y * z - s_ * x, t * x * z - s_ * y, t * y * z + s_ * x, t * z * z + c};
    memcpy(R, r, sizeof r);
}
static void m3mul(float *o, const float *a, const float *b) { float r[9]; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r[3 * i + j] = a[3 * i] * b[j] + a[3 * i + 1] * b[3 + j] + a[3 * i + 2] * b[6 + j]; memcpy(o, r, sizeof r); }
static int collect(const NsScene *sc, const NsMeshStore *st, uint32_t inst, int only_piece, WV *v, int *nv, int (*tri)[3], int *nt, int cap_v, int cap_t) {
    const NsInstance *in = &sc->inst[inst];
    for (uint32_t k = 0; k < in->n_pieces; k++) {
        if (only_piece >= 0 && (int)k != only_piece) continue;
        const NsPiece *p = &sc->piece[in->first_piece + k];
        const NsMesh *m = ns_store_get(st, p->mesh_id);
        if (!m || (p->flags & NS_PF_HIDDEN)) continue;
        int base = *nv;
        if (base + (int)m->n_verts > cap_v) return 0;
        for (uint32_t i = 0; i < m->n_verts; i++) {
            const NsVert *q = &m->verts[i];
            float a[3] = {q->x, q->y, q->z};
            const NsXf *w = &p->world;
            if (q->weld != 0xFF && p->parent >= 0 && (m->flags & NS_MF_WELDED)) {
                const NsPiece *pp = &sc->piece[in->first_piece + (uint32_t)p->parent];
                const NsMesh *pm = ns_store_get(st, pp->mesh_id);
                if (pm && q->weld < pm->n_verts) { a[0] = pm->verts[q->weld].x; a[1] = pm->verts[q->weld].y; a[2] = pm->verts[q->weld].z; w = &pp->world; }
            }
            ns_xf_apply(w, a, v[*nv].p); v[*nv].piece = (int)(in->first_piece + k);
            /* anillos intermedios de la transición (pieza 1 de la mano: 4 del antebrazo + 3 anillos intermedios + borde): vértices propios, nunca compartidos */
            if (ns_key_is_hand(in->key) && k == 1 && i >= 4 && i + VRHA_NCUFF < m->n_verts) v[*nv].piece = -1 - (int)i;
            (*nv)++;
        }
        for (uint32_t f = 0; f < m->n_faces; f++) {
            const NsFace *F = &m->faces[f];
            const int quad = !(F->flags & NS_FF_TRI);
            const int tt[2][3] = {{F->v[0], F->v[1], F->v[3]}, {F->v[1], F->v[2], F->v[3]}};
            for (int h = 0; h < (quad ? 2 : 1); h++) {
                if (*nt >= cap_t) return 0;
                const int *t3 = quad ? tt[h] : (int[3]){F->v[0], F->v[1], F->v[2]};
                for (int c = 0; c < 3; c++) tri[*nt][c] = base + t3[c];
                (*nt)++;
            }
        }
    }
    return 1;
}
static int cmp_u64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static void test_watertight(void) {
    static WV v[4096]; static int tri[8192][3], id[4096]; static uint64_t e[3 * 8192];
    uint32_t seed = 12345; int bad_open = 0, bad_nm = 0, poses = 0, worst_open = 0, bad_twist = 0;
    const float l[3] = {0.0f, -249.0f / 253.4f, 47.0f / 253.4f};
    for (int it = 0; it < 320; it++) {
        World w; world_init(&w, 8, 64, 1); ns_hand_reset();
        #define RNDF(a, b) ((a) + ((b) - (a)) * (float)((seed = seed * 1664525u + 1013904223u) >> 8) / 16777216.0f)
        float Ra[9], Rb[9], Rc[9], R[9], Rf[9], ax[3] = {RNDF(-1, 1), RNDF(-1, 1), RNDF(-1, 1)}, al = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
        for (int i = 0; i < 3; i++) ax[i] /= al;
        float Ru[9], Rd[9], Re[9];
        rot_axis_m(Ru, ax, RNDF(-180, 180));                                       /* brazo en cualquier orientación */
        const float zx[3] = {0, 0, 1}, xx[3] = {1, 0, 0}, yl[3] = {0, 1, 0};
        rot_axis_m(Rd, xx, RNDF(-140, 0)); rot_axis_m(Re, yl, RNDF(-180, 180)); m3mul(Rf, Ru, Rd); m3mul(Rf, Rf, Re);   /* antebrazo: codo doblado y girado sobre su eje (la IK lo gira con la mano) */
        rot_axis_m(Ra, l, RNDF(-100, 100)); rot_axis_m(Rb, zx, RNDF(-70, 80)); rot_axis_m(Rc, xx, RNDF(-30, 20));
        m3mul(R, Ra, Rb); m3mul(R, R, Rc);                                         /* mano respecto al antebrazo: giro, flexión, desviación */
        NsXf *U = &w.sc.piece[1 + NS_PART_UPPER_R].world, *F = &w.sc.piece[1 + NS_PART_FORE_R].world, *H = &w.sc.piece[1 + NS_PART_HAND_R].world;
        float RH[9]; m3mul(RH, Rf, R);
        for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) { U->m[4 * i + j] = Ru[3 * i + j]; F->m[4 * i + j] = Rf[3 * i + j]; H->m[4 * i + j] = RH[3 * i + j]; } U->m[4 * i + 3] = RNDF(-3000, 3000); }
        for (int i = 0; i < 3; i++) F->m[4 * i + 3] = U->m[4 * i + 3] + Ru[3 * i + 1] * -260.0f + Ru[3 * i + 2] * -50.0f;
        for (int i = 0; i < 3; i++) H->m[4 * i + 3] = F->m[4 * i + 3] + Rf[3 * i + 1] * -249.0f + Rf[3 * i + 2] * 47.0f;
        #undef RNDF
        NsHandIn in; memset(&in, 0, sizeof in);
        in.enabled = 1; in.driven[1] = 1; in.item_pose[1] = (it & 1) ? HP_GRIP_PISTOL : HP_RELAX; in.grip[1] = (float)(it % 5) / 4.0f;
        if (ns_hand_apply(&w.sc, w.st, &w.map, &in) < 1) { bad_open++; world_free(&w); continue; }
        int nv = 0, nt = 0, ok = 1;
        ok &= collect(&w.sc, w.st, 1, NS_PART_UPPER_R, v, &nv, tri, &nt, 4096, 8192);
        const int fv0 = nv;
        ok &= collect(&w.sc, w.st, 1, NS_PART_FORE_R, v, &nv, tri, &nt, 4096, 8192);
        {   /* el giro del antebrazo respecto al brazo se reparte entre sus dos franjas (codo -> anillo intermedio -> muñeca): ninguna lleva más del 70 % (sin repartir, la del codo lo lleva todo) */
            const float ll = sqrtf(249.0f * 249.0f + 47.0f * 47.0f), L[3] = {0, -249.0f / ll, 47.0f / ll};
            float loc[12][3];
            for (int i = 0; i < 12; i++) {   /* vértices del antebrazo (soldados resueltos) de vuelta a su marco */
                const float *wp = v[fv0 + i].p; float dv[3] = {wp[0] - F->m[3], wp[1] - F->m[7], wp[2] - F->m[11]};
                for (int a = 0; a < 3; a++) loc[i][a] = F->m[a] * dv[0] + F->m[4 + a] * dv[1] + F->m[8 + a] * dv[2];
            }
            /* giro del antebrazo respecto al brazo sobre su eje (separado de la flexión del codo) */
            float Rr[9], q[4], Rt[9] = {Ru[0], Ru[3], Ru[6], Ru[1], Ru[4], Ru[7], Ru[2], Ru[5], Ru[8]};
            m3mul(Rr, Rt, Rf);
            { const float tr = Rr[0] + Rr[4] + Rr[8]; q[3] = 0.5f * sqrtf(fmaxf(0.0f, 1.0f + tr)); const float k4 = q[3] > 1e-4f ? 0.25f / q[3] : 0.0f; q[0] = (Rr[7] - Rr[5]) * k4; q[1] = (Rr[2] - Rr[6]) * k4; q[2] = (Rr[3] - Rr[1]) * k4; }
            const float tau = 2.0f * atan2f(q[0] * L[0] + q[1] * L[1] + q[2] * L[2], q[3]) * 57.29578f;
            float worst = 0;
            for (int k = 0; k < 4 && q[3] > 0.05f; k++) {   /* franja intermedio -> muñeca, en el marco del antebrazo; la del codo lleva el resto */
                const float *p0 = loc[8 + k], *p1 = loc[4 + k];
                const float t0 = p0[0] * L[0] + p0[1] * L[1] + p0[2] * L[2], t1 = p1[0] * L[0] + p1[1] * L[1] + p1[2] * L[2];
                const float u[3] = {p0[0] - t0 * L[0], p0[1] - t0 * L[1], p0[2] - t0 * L[2]}, qq[3] = {p1[0] - t1 * L[0], p1[1] - t1 * L[1], p1[2] - t1 * L[2]};
                const float cr[3] = {u[1] * qq[2] - u[2] * qq[1], u[2] * qq[0] - u[0] * qq[2], u[0] * qq[1] - u[1] * qq[0]};
                const float b2 = atan2f(cr[0] * L[0] + cr[1] * L[1] + cr[2] * L[2], u[0] * qq[0] + u[1] * qq[1] + u[2] * qq[2]) * 57.29578f;
                /* la franja del codo lleva tau - b2 (sin repartir: b2 = 0, todo el giro en la franja del codo) */
                const float wb = fmaxf(fabsf(tau - b2), fabsf(b2));
                if (wb > 0.7f * fabsf(tau) + 3.0f && wb > worst) worst = wb;
            }
            if (worst > 0) bad_twist++;
        }
        for (uint32_t i = 2; i < w.sc.n_inst; i++) if (w.sc.inst[i].key == NS_HAND_KEY_R) ok &= collect(&w.sc, w.st, i, -1, v, &nv, tri, &nt, 4096, 8192);
        CHECK(ok, "capacidad de la prueba");
        /* vértices que coinciden (misma posición, de mallas distintas: soldados o compartidos entre piezas; dentro de una malla cada vértice es distinto) */
        for (int i = 0; i < nv; i++) { id[i] = i; for (int j = 0; j < i; j++) { if (v[j].piece == v[i].piece || v[i].piece < 0 || v[j].piece < 0) continue; float d0 = v[i].p[0] - v[j].p[0], d1 = v[i].p[1] - v[j].p[1], d2 = v[i].p[2] - v[j].p[2]; if (d0 * d0 + d1 * d1 + d2 * d2 < 1e-6f) { id[i] = id[j]; break; } } }
        int ne = 0;
        for (int t = 0; t < nt; t++) {
            int a = id[tri[t][0]], b = id[tri[t][1]], c = id[tri[t][2]];
            if (a == b || b == c || a == c) continue;   /* degenerado (anillo de soldadura) */
            const int ed[3][2] = {{a, b}, {b, c}, {c, a}};
            for (int k = 0; k < 3; k++) { uint32_t x = (uint32_t)(ed[k][0] < ed[k][1] ? ed[k][0] : ed[k][1]), y = (uint32_t)(ed[k][0] < ed[k][1] ? ed[k][1] : ed[k][0]); e[ne++] = (uint64_t)x << 32 | y; }
        }
        qsort(e, (size_t)ne, sizeof e[0], cmp_u64);
        int open = 0, nm = 0;
        for (int i = 0; i < ne;) { int j = i; while (j < ne && e[j] == e[i]) j++; if (j - i == 1) open++; else if (j - i > 2) { nm++; if (getenv("NSH_DBG")) { int a = (int)(e[i] >> 32), b = (int)(e[i] & 0xFFFFFFFFu); printf("  pose %d arista %d-%d x%d: (%.2f %.2f %.2f) (%.2f %.2f %.2f)\n", it, a, b, j - i, v[a].p[0], v[a].p[1], v[a].p[2], v[b].p[0], v[b].p[1], v[b].p[2]); } } i = j; }
        if (open != 4) bad_open++;
        if (nm) bad_nm++;
        if (open > worst_open) worst_open = open;
        poses++;
        world_free(&w);
    }
    CHECK(poses >= 300 && bad_open == 0 && bad_nm == 0, "brazo+antebrazo+mano cerrados: %d de %d poses con aristas abiertas (peor %d; se esperan 4, el hombro), %d con aristas de >2 triángulos", bad_open, poses, worst_open, bad_nm);
    CHECK(bad_twist == 0, "giro del antebrazo concentrado en una franja (> 70 %%) en %d de %d poses", bad_twist, poses);
    printf("  brazo+antebrazo+mano: %d poses, aristas abiertas fuera del hombro en %d, no variedad en %d (peor %d abiertas), giro concentrado en una franja en %d\n", poses, bad_open, bad_nm, worst_open, bad_twist);
}


/* Revisión: el anillo y la transición se ocultan si el antebrazo está oculto; la copia girada del antebrazo lleva una normal por esquina, unitaria y girada con su vértice. */
static void test_seam_details(void) {
    World w; world_init(&w, 8, 64, 1); ns_hand_reset();
    NsXf *U = &w.sc.piece[1 + NS_PART_UPPER_R].world, *F = &w.sc.piece[1 + NS_PART_FORE_R].world, *H = &w.sc.piece[1 + NS_PART_HAND_R].world;
    /* brazo identidad; antebrazo en el codo girado 90 grados sobre su eje (y local) */
    memset(U, 0, sizeof *U); U->m[0] = U->m[5] = U->m[10] = 1;
    memset(F, 0, sizeof *F); F->m[2] = 1; F->m[5] = 1; F->m[8] = -1; F->m[7] = -260; F->m[11] = -50;
    *H = *F; for (int i = 0; i < 3; i++) H->m[4 * i + 3] = F->m[4 * i + 3] + F->m[4 * i + 1] * -249.0f + F->m[4 * i + 2] * 47.0f;
    w.sc.piece[1 + NS_PART_FORE_R].flags = NS_PF_HIDDEN;
    NsHandIn in; memset(&in, 0, sizeof in); in.enabled = 1; in.driven[1] = 1; in.item_pose[1] = HP_RELAX;
    CHECK(ns_hand_apply(&w.sc, w.st, &w.map, &in) >= 1, "mano puesta");
    const NsInstance *ir = NULL;
    for (uint32_t i = 0; i < w.sc.n_inst; i++) if (w.sc.inst[i].key == NS_HAND_KEY_R) ir = &w.sc.inst[i];
    CHECK(ir && ir->n_pieces == NS_HAND_PIECES && (w.sc.piece[ir->first_piece].flags & NS_PF_HIDDEN) && (w.sc.piece[ir->first_piece + 1].flags & NS_PF_HIDDEN) &&
          !(w.sc.piece[ir->first_piece + 2].flags & NS_PF_HIDDEN), "antebrazo oculto: anillo y transición ocultos, la mano no");
    const NsMesh *fm = ns_store_get(w.st, w.sc.piece[1 + NS_PART_FORE_R].mesh_id);
    int nok = fm && fm->id != w.fore.id && fm->n_norms == 4u * fm->n_faces, rotated = 0;
    for (uint32_t f = 0; nok && f < fm->n_faces; f++) for (int k = 0; k < 4; k++) {
        const NsNorm *n = &fm->norms[fm->faces[f].n[k]];
        const float l = sqrtf((float)n->x * n->x + (float)n->y * n->y + (float)n->z * n->z) / 4096.0f;
        if (fm->faces[f].n[k] != 4 * f + (uint32_t)k || fabsf(l - 1.0f) > 0.01f) nok = 0;
        if (n->x != 0 || n->y != 0 || n->z != 4096) rotated = 1;
    }
    CHECK(nok && rotated, "antebrazo girado: una normal unitaria por esquina, giradas con su vértice (ok %d, giradas %d)", nok, rotated);
    world_free(&w);
}

/* ---- ayudas: caras de la mano en el orden de vrha_tri, zona de cada triángulo, aplicar manoplas ---- */
static int hand_faces(World *w, uint32_t key, NsFace out[VRHA_NT]) {   /* caras de la mano en el orden de vrha_tri; 0 si falta algo */
    const NsInstance *hi = NULL;
    for (uint32_t i = 0; i < w->sc.n_inst; i++) if (w->sc.inst[i].key == key) hi = &w->sc.inst[i];
    if (!hi || hi->n_pieces != NS_HAND_PIECES) return 0;
    for (int p = 0; p < VRHA_NP; p++) {
        const NsMesh *m = ns_store_get(w->st, w->sc.piece[hi->first_piece + 2 + (uint32_t)p].mesh_id);
        if (!m || m->n_faces != vrha_piece[p].nt) return 0;
        memcpy(&out[vrha_piece[p].t0], m->faces, vrha_piece[p].nt * sizeof(NsFace));
    }
    return 1;
}
static int tri_zone(int p, int t) {   /* 0 = palma (muñeca y metacarpos), 1 = dorso (muñeca y metacarpos), 2 = resto (dedos, pulgar, costados) */
    float wj[VRHA_NJ] = {0};
    for (int k = 0; k < 3; k++) for (int q = 0; q < 4; q++) wj[vrha_joint[vrha_piece[p].v0 + vrha_tri[t][k]][q]] += vrha_weight[vrha_piece[p].v0 + vrha_tri[t][k]][q];
    int d = 0;
    for (int j = 1; j < VRHA_NJ; j++) if (wj[j] > wj[d]) d = j;
    if (!(d == 0 || (d >= VRHS_INDEX_MC && (d - VRHS_INDEX_MC) % 5 == 0))) return 2;
    return vrha_role[t] == VRHA_ROLE_PALM ? 0 : vrha_role[t] == VRHA_ROLE_BACK ? 1 : 2;
}
static int apply_gloves(World *w, Glove *gr, Glove *gl) {
    ns_hand_reset();
    ns_store_put(w->st, &gr->m); ns_store_put(w->st, &gl->m);
    w->sc.n_inst = 2; w->sc.n_piece = 17;
    w->sc.piece[1 + NS_PART_HAND_R].flags = 0; w->sc.piece[1 + NS_PART_HAND_L].flags = 0;
    w->sc.piece[1 + NS_PART_HAND_R].mesh_id = gr->m.id; w->sc.piece[1 + NS_PART_HAND_L].mesh_id = gl->m.id;
    NsHandIn in; memset(&in, 0, sizeof in);
    in.enabled = 1; in.driven[0] = in.driven[1] = 1; in.item_pose[0] = in.item_pose[1] = HP_RELAX;
    return ns_hand_apply(&w->sc, w->st, &w->map, &in);
}
/* ---- textura propia del guante: bloque de VRAM (paleta, parche del dorso) y caras del dorso, la palma y los dedos ---- */
static int glove_texel(const uint16_t *vram, int tx, int ty) {   /* suma de componentes de 5 bits del texel (tx, ty) de la textura, a través de su CLUT */
    const uint16_t w = vram[(NS_GLOVE_VY + ty) * 1024 + NS_GLOVE_VX + tx / 4];
    const uint16_t c = vram[NS_GLOVE_VY * 1024 + NS_GLOVE_VX + 16 + ((w >> (4 * (tx & 3))) & 15)];
    return (c & 31) + ((c >> 5) & 31) + ((c >> 10) & 31);
}
static void test_glove_texture(void) {
    static uint16_t vram[1024 * 512];
    const uint16_t *b0 = ns_hand_glove_block();
    ns_hand_glove_blit(vram);
    CHECK(b0 && b0 == ns_hand_glove_block(), "bloque de la textura");
    int same = 1, outside = 0;
    for (int y = 0; y < NS_GLOVE_VH; y++) for (int x = 0; x < NS_GLOVE_VW; x++) same &= vram[(NS_GLOVE_VY + y) * 1024 + NS_GLOVE_VX + x] == b0[y * NS_GLOVE_VW + x];
    for (int i = 0; i < 1024 * 512; i++) { const int x = i % 1024, y = i / 1024; if (!(x >= NS_GLOVE_VX && x < NS_GLOVE_VX + NS_GLOVE_VW && y >= NS_GLOVE_VY && y < NS_GLOVE_VY + NS_GLOVE_VH)) outside += vram[i] != 0; }
    CHECK(same && outside == 0, "ns_hand_glove_blit escribe solo su zona de la VRAM (fuera: %d)", outside);
    CHECK(NS_GLOVE_TPAGE == 15 && NS_GLOVE_CLUT == (NS_GLOVE_VY << 6 | (NS_GLOVE_VX + 16) / 16) && NS_GLOVE_U0 == (NS_GLOVE_VX - 15 * 64) * 4, "tpage, clut y u0 coherentes con la zona de VRAM");
    /* paleta: <= 12 colores, todos distintos de 0 (el texel 0 es transparente); los texels usan solo esos índices */
    int ncol = 0, zero = 0;
    for (int i = 0; i < 16; i++) { const uint16_t c = b0[16 + i]; zero += c == 0; int dup = 0; for (int j = 0; j < i; j++) dup |= b0[16 + j] == c; ncol += !dup; }
    CHECK(zero == 0 && ncol <= 12, "paleta: %d colores, %d a cero", ncol, zero);
    int used[16] = {0}, nused = 0;
    for (int ty = 0; ty < 32; ty++) for (int tx = 0; tx < 64; tx++) used[(b0[ty * NS_GLOVE_VW + tx / 4] >> (4 * (tx & 3))) & 15] = 1;
    for (int i = 0; i < 16; i++) nused += used[i];
    CHECK(nused >= 8 && nused <= 12, "índices usados: %d", nused);
    /* parche del dorso (zona de 48x32 texels): los texels claros (suma de 5 bits >= 35, sin el borde oscuro) forman un rectángulo casi lleno (esquinas redondeadas) y GRANDE: >= 65 % del ancho
     * (31 de 48), >= 40 % del largo (13 de 32), >= 25 % de la zona del dorso, y más claro que el guante; el parche pequeño de antes (~16x9) no pasa */
    int n = 0, x0 = 99, x1 = -1, y0 = 99, y1 = -1;
    for (int ty = 0; ty < 32; ty++) for (int tx = 0; tx < 48; tx++) if (glove_texel(vram, tx, ty) >= 35) { n++; if (tx < x0) x0 = tx; if (tx > x1) x1 = tx; if (ty < y0) y0 = ty; if (ty > y1) y1 = ty; }
    const int area = (x1 - x0 + 1) * (y1 - y0 + 1);
    CHECK(n > 0 && x1 - x0 + 1 >= 31 && x1 - x0 + 1 <= 40 && y1 - y0 + 1 >= 13 && y1 - y0 + 1 <= 20 && n * 4 >= 48 * 32 && n * 100 >= area * 85 && n < area, "parche gris: %d texels en %dx%d", n, x1 - x0 + 1, y1 - y0 + 1);
    /* nada claro fuera de la placa: todo texel de la textura (dorso, tela, puño) fuera de la placa (caja de sus texels claros + 2 texels de borde) tiene una luminancia mínima (suma de 5 bits <= 15 de 93) */
    int maxout = 0, nlight = 0;
    for (int ty = 0; ty < 32; ty++) for (int tx = 0; tx < 64; tx++) {
        if (tx >= x0 - 2 && tx <= x1 + 2 && ty >= y0 - 2 && ty <= y1 + 2) continue;
        const int v = glove_texel(vram, tx, ty);
        if (v > maxout) maxout = v;
        nlight += v > 15;
    }
    CHECK(nlight == 0, "texels claros fuera de la placa: %d (máximo %d, límite 15)", nlight, maxout);
    int tinted = 0;
    for (int i = 0; i < 16; i++) { const uint16_t c = b0[16 + i]; tinted += (c & 31) != ((c >> 5) & 31) || (c & 31) != ((c >> 10) & 31); }
    CHECK(tinted == 0, "paleta sin tinte: %d colores con R, G, B distintos", tinted);
    CHECK(glove_texel(vram, (x0 + x1) / 2, (y0 + y1) / 2) >= 2 * glove_texel(vram, 0, 0), "parche más claro que el guante");
    /* caras: dorso por posición de vértice (mismo texel en las dos manos; pinky u bajo, pulgar u alto), el resto en la zona de tela */
    World w;
    CHECK(world_init(&w, 8, 64, 1), "mundo");
    static NsFace fr[VRHA_NT], fl[VRHA_NT];
    CHECK(apply_gloves(&w, &w.gr, &w.gl) == 2 && hand_faces(&w, NS_HAND_KEY_R, fr) && hand_faces(&w, NS_HAND_KEY_L, fl), "manos");
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    for (int p = 0; p < VRHA_NP; p++) for (int t = vrha_piece[p].t0; t < vrha_piece[p].t0 + vrha_piece[p].nt; t++) {
        if (tri_zone(p, t) != 1) continue;
        for (int k = 0; k < 3; k++) for (int a = 0; a < 3; a++) { const float v = vrha_pos[vrha_piece[p].v0 + vrha_tri[t][k]][a]; if (v < lo[a]) lo[a] = v; if (v > hi[a]) hi[a] = v; }
    }
    for (int side = 1; side >= -1; side -= 2) {
        const NsFace *f = side > 0 ? fr : fl;
        int nback = 0, bad = 0, varied = 0, cloth = 0, cloth_bad = 0;
        for (int p = 0; p < VRHA_NP; p++) for (int t = vrha_piece[p].t0; t < vrha_piece[p].t0 + vrha_piece[p].nt; t++) {
            const NsFace *F = &f[t];
            if (F->texid != NS_GLOVE_TEXID || F->tpage != NS_GLOVE_TPAGE || F->clut != NS_GLOVE_CLUT) { bad++; continue; }
            if (tri_zone(p, t) != 1) {
                cloth++;
                for (int q = 0; q < 4; q++) if (F->u[q] < NS_GLOVE_U0 + 48 || F->u[q] > NS_GLOVE_U0 + 63 || F->t[q] > 15) cloth_bad++;
                continue;
            }
            nback++;
            if (F->u[0] != F->u[1] || F->u[1] != F->u[2] || F->t[0] != F->t[1] || F->t[1] != F->t[2]) varied++;
            for (int q = 0; q < 4; q++) {
                const float *v = vrha_pos[vrha_piece[p].v0 + F->v[q]];
                const float su = (v[2] - lo[2]) / (hi[2] - lo[2]) * 47.0f, sv = (v[1] - lo[1]) / (hi[1] - lo[1]) * 31.0f;
                if (fabsf((float)(F->u[q] - NS_GLOVE_U0) - su) > 0.51f || fabsf((float)F->t[q] - sv) > 0.51f) bad++;
            }
        }
        CHECK(bad == 0 && nback > 50 && varied > nback / 2, "lado %d: dorso con la zona del dorso por posición (mal %d, caras %d, proyectadas %d)", side, bad, nback, varied);
        CHECK(cloth > nback && cloth_bad == 0, "lado %d: palma, dedos y costados en la zona de tela (%d caras, mal %d)", side, cloth, cloth_bad);
    }
    static int ur[VRHA_NV], ul[VRHA_NV], tr_[VRHA_NV], tl_[VRHA_NV];
    for (int i = 0; i < VRHA_NV; i++) ur[i] = ul[i] = tr_[i] = tl_[i] = -1;
    for (int p = 0; p < VRHA_NP; p++) for (int t = vrha_piece[p].t0; t < vrha_piece[p].t0 + vrha_piece[p].nt; t++) {
        if (tri_zone(p, t) != 1) continue;
        for (int q = 0; q < 3; q++) {
            const int vr = vrha_piece[p].v0 + fr[t].v[q], vl = vrha_piece[p].v0 + fl[t].v[q];
            ur[vr] = fr[t].u[q]; tr_[vr] = fr[t].t[q]; ul[vl] = fl[t].u[q]; tl_[vl] = fl[t].t[q];
        }
    }
    int nv = 0, diff = 0, side_bad = 0;
    for (int i = 0; i < VRHA_NV; i++) {
        if (ur[i] < 0) continue;
        nv++;
        if (ur[i] != ul[i] || tr_[i] != tl_[i]) diff++;
        const float sz = (vrha_pos[i][2] - lo[2]) / (hi[2] - lo[2]);
        if ((sz > 0.75f && ur[i] < NS_GLOVE_U0 + 35) || (sz < 0.25f && ur[i] > NS_GLOVE_U0 + 12)) side_bad++;
    }
    CHECK(nv > 50 && diff == 0, "dorso: mismo texel en las dos manos para el mismo vértice (%d vértices, %d distintos)", nv, diff);
    CHECK(side_bad == 0, "dorso: meñique (z bajo) en u bajo, pulgar (z alto) en u alto (%d mal)", side_bad);
    world_free(&w);
}

int main(void) {
    test_store_put();
    test_classify();
    test_apply();
    test_guards();
    test_flat_and_epoch();
    test_pose_blend();
    test_watertight();
    test_seam_details();
    test_glove_texture();
    printf("%s ns_hand: %d comprobaciones, %d fallos\n", fails ? "FALLO" : "OK", checks, fails);
    return fails ? 1 : 0;
}
