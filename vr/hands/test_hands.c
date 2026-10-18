/* Pruebas sintéticas de vr_hands (sin datos del juego): agarre derivado, transformaciones de piezas animadas, objetos sueltos,
 * ids estables, anillo de frames, buzón sim->render, entradas inválidas. make -C vr/hands test / test-san */
#include "vr_hands.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("  FALLO l.%d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-4f)

/* ---- arma sintética: cajas (en mm, marco del modelo) ---- */
static WVertex *box_v(float x0, float y0, float z0, float x1, float y1, float z1, float px, float py, float pz, uint32_t *nv) {
    WVertex *v = calloc(8, sizeof(WVertex));
    for (int i = 0; i < 8; i++) {
        v[i].pos[0] = ((i & 1 ? x1 : x0) - px) * 0.001f; v[i].pos[1] = ((i & 2 ? y1 : y0) - py) * 0.001f; v[i].pos[2] = ((i & 4 ? z1 : z0) - pz) * 0.001f;
        v[i].nrm[1] = 1; v[i].uv[0] = (i & 1) ? 1.f : 0.f; v[i].uv[1] = (i & 2) ? 1.f : 0.f; memset(v[i].rgba, 255, 4);
    }
    *nv = 8; return v;
}
static const uint16_t BOXI[36] = {0,1,3, 0,3,2, 4,6,7, 4,7,5, 0,2,6, 0,6,4, 1,5,7, 1,7,3, 2,3,7, 2,7,6, 0,4,5, 0,5,1};
static const char *const NAMES[] = {"frame", "slide", "trigger", "mag", NULL};

typedef struct { WeaponDef def; WeaponState *st; WMesh m[4]; WPart p[4]; int with_trigger; } Syn;
static void syn_init(Syn *a, int with_trigger) {
    memset(a, 0, sizeof *a);
    a->with_trigger = with_trigger;
    uint32_t nv;
    /* pivotes: frame (0,0,0); slide (0,46,-40); trigger (0,6,-90); mag (0,0,-35) */
    float piv[4][3] = {{0, 0, 0}, {0, 46, -40}, {0, 6, -90}, {0, 0, -35}};
    float bx[4][6] = {{-10, -30, -100, 10, 45, -35}, {-10, 46, -200, 10, 70, -40}, {-3, 0, -95, 3, 12, -86}, {-10, -36, -100, 10, 0, -35}};
    for (int i = 0; i < 4; i++) {
        WVertex *v = box_v(bx[i][0], bx[i][1], bx[i][2], bx[i][3], bx[i][4], bx[i][5], piv[i][0], piv[i][1], piv[i][2], &nv);
        uint16_t *ix = malloc(sizeof BOXI); memcpy(ix, BOXI, sizeof BOXI);
        a->m[i] = (WMesh){NAMES[i], v, 8, ix, 36, 1};
    }
    a->p[0] = (WPart){"frame", -1, {{0, 0, 0}, {0, 0, 0, 1}}, 0, WCH_NONE, {0, 0, 0}, 0};
    a->p[1] = (WPart){"slide", 0, {{0, 0.046f, -0.040f}, {0, 0, 0, 1}}, 1, WCH_SLIDE, {0, 0, 1}, 0};
    a->p[2] = (WPart){"trigger", 0, {{0, 0.006f, -0.090f}, {0, 0, 0, 1}}, 2, WCH_TRIGGER, {-0.35f, 0, 0}, 1};
    a->p[3] = (WPart){"mag", 0, {{0, 0, -0.035f}, {0, 0, 0, 1}}, 3, WCH_MAG, {0, -1, 0}, 0};
    a->def = (WeaponDef){.id = "syn", .mesh_names = NAMES, .state_size = sizeof(WeaponState)};
    for (int i = 0; i < WP_COUNT; i++) a->def.points[i] = wpose_identity();
    a->st = calloc(1, sizeof(WeaponState));
    a->st->def = &a->def;
    a->st->recoil_offset = wpose_identity();
    for (int i = 0; i < W_MAX_HELD; i++) a->st->held[i].mesh = -1;
    weapon_bind(a->st, a->m, 4, a->p, with_trigger ? 4 : 4);
    if (!with_trigger) a->p[2].name = "gatillo";     /* sin pieza "trigger": no se puede derivar el agarre */
}
static void syn_free(Syn *a) { for (int i = 0; i < 4; i++) { free((void *)a->m[i].v); free((void *)a->m[i].idx); } free(a->st); }

static VrHandsPose hp(float x, float y, float z, float qx, float qy, float qz, float qw) {
    VrHandsPose p = {1, {x, y, z}, {qx, qy, qz, qw}}; return p;
}
static WVec3 xf(const float *m, WVec3 v) {
    return wv(m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12], m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13], m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14]);
}
static const VrHandsMesh *find_mesh(const VrHandsFrame *f, uint32_t flags, int nth, const void *verts) {
    int k = 0;
    for (int i = 0; i < f->nmeshes; i++) if (f->meshes[i].flags == flags && f->meshes[i].verts == verts && k++ == nth) return &f->meshes[i];
    return NULL;
}

static void t_grip_and_transforms(void) {
    Syn a; syn_init(&a, 1);
    VrHandsRig *r = vrhands_create(NULL);
    uint8_t atlas[4 * 4 * 4] = {0};
    CHECK(vrhands_bind(r, a.st, atlas, 4, 4) == 0, "bind");
    WPose g; CHECK(vrhands_derive_grip(a.st, &g) == 0, "derive_grip");
    /* región: vértices del frame con y <= 46 (fondo de la corredera) y z > -86 (detrás del gatillo): solo la cara trasera z=-35 */
    CHECK(NEAR(g.p.x, 0) && NEAR(g.p.y, 0.0075f) && NEAR(g.p.z, -0.035f), "grip derivado (%.4f %.4f %.4f)", g.p.x, g.p.y, g.p.z);
    VrHandsPose h = hp(0.2f, 1.3f, -0.4f, 0, 0, 0, 1);
    VrHandsFrame f;
    int n = vrhands_build(r, a.st, &h, &f);
    CHECK(n == 4 && f.nmeshes == 4, "nmeshes %d", n);
    const VrHandsMesh *fr = find_mesh(&f, 0, 0, a.m[0].v), *sl = find_mesh(&f, 0, 0, a.m[1].v), *tr = find_mesh(&f, 0, 0, a.m[2].v);
    CHECK(fr && sl && tr, "piezas");
    if (fr) {   /* el punto de agarre del modelo cae sobre la mano */
        WVec3 q = xf(fr->model, g.p);
        CHECK(NEAR(q.x, 0.2f) && NEAR(q.y, 1.3f) && NEAR(q.z, -0.4f), "agarre en la mano (%.4f %.4f %.4f)", q.x, q.y, q.z);
    }
    float slide0 = sl ? sl->model[14] : 0;
    CHECK(f.ntex == 1 && f.tex[0].rgba == atlas && f.tex[0].w == 4 && fr && fr->tex_id == f.tex[0].id, "textura");
    /* corredera atrás 20 mm (+Z) */
    a.st->channel[WCH_SLIDE] = 0.02f;
    vrhands_build(r, a.st, &h, &f);
    sl = find_mesh(&f, 0, 0, a.m[1].v);
    CHECK(sl && NEAR(sl->model[14] - slide0, 0.02f), "corredera +20 mm (%.5f)", sl ? sl->model[14] - slide0 : 0);
    /* gatillo: rotación sobre X */
    a.st->channel[WCH_TRIGGER] = 1.0f;
    vrhands_build(r, a.st, &h, &f);
    tr = find_mesh(&f, 0, 0, a.m[2].v);
    CHECK(tr && NEAR(tr->model[6], sinf(-0.35f)) && NEAR(tr->model[5], cosf(0.35f)), "gatillo rota (%.4f)", tr ? tr->model[6] : 0);
    /* cargador oculto a 50 m: se omite */
    a.st->channel[WCH_MAG] = 50.0f;
    n = vrhands_build(r, a.st, &h, &f);
    CHECK(n == 3 && !find_mesh(&f, 0, 0, a.m[3].v), "cargador apartado omitido (n=%d)", n);
    a.st->channel[WCH_MAG] = 0.03f;
    n = vrhands_build(r, a.st, &h, &f);
    const VrHandsMesh *mg = find_mesh(&f, 0, 0, a.m[3].v);
    CHECK(n == 4 && mg, "cargador extraído");
    /* objeto suelto: cargador en la otra mano */
    a.st->held[0].active = 1; a.st->held[0].mesh = 3; a.st->held[0].world = (WPose){{-0.1f, 1.0f, -0.3f}, {0, 0, 0, 1}};
    n = vrhands_build(r, a.st, &h, &f);
    const VrHandsMesh *hm = find_mesh(&f, VRH_MF_HELD, 0, a.m[3].v);
    CHECK(n == 5 && hm && NEAR(hm->model[12], -0.1f) && NEAR(hm->model[13], 1.0f) && hm->mesh_id == mg->mesh_id, "suelto (n=%d)", n);
    a.st->held[0].active = 0;
    /* mano girada 90 grados en Y (boca hacia -X): el eje -Z del arma apunta a -X */
    float s2 = sqrtf(0.5f);
    VrHandsPose h2 = hp(0, 1, 0, 0, s2, 0, s2);
    vrhands_build(r, a.st, &h2, &f);
    sl = find_mesh(&f, 0, 0, a.m[1].v);
    CHECK(sl && NEAR(sl->model[8], 1.0f) && NEAR(sl->model[10], 0.0f) && NEAR(sl->model[0], 0.0f) && NEAR(sl->model[2], -1.0f),
          "giro de mano (%.3f %.3f %.3f %.3f)", sl ? sl->model[0] : 0, sl ? sl->model[2] : 0, sl ? sl->model[8] : 0, sl ? sl->model[10] : 0);
    /* retroceso del estado: desplaza todo el arma */
    a.st->recoil_offset.p = wv(0, 0, 0.02f);
    vrhands_build(r, a.st, &h, &f);
    fr = find_mesh(&f, 0, 0, a.m[0].v);
    CHECK(fr && NEAR(xf(fr->model, g.p).z, -0.4f + 0.02f), "retroceso");
    vrhands_destroy(r); syn_free(&a);
}

static void t_modes_scale_pitch(void) {
    Syn a; syn_init(&a, 1);
    VrHandsConfig c; vrhands_default_config(&c);
    c.grip_mode = VRH_GRIP_DEF;
    VrHandsRig *r = vrhands_create(&c);
    vrhands_bind(r, a.st, NULL, 0, 0);
    VrHandsPose h = hp(0, 1, 0, 0, 0, 0, 1); VrHandsFrame f;
    vrhands_build(r, a.st, &h, &f);
    const VrHandsMesh *fr = find_mesh(&f, 0, 0, a.m[0].v);
    CHECK(fr && NEAR(fr->model[12], 0) && NEAR(fr->model[14], 0) && fr->tex_id == 0 && f.ntex == 0, "modo DEF (origen del modelo en la mano), sin atlas");
    vrhands_destroy(r);
    /* def con agarre propio: AUTO lo respeta */
    a.def.points[WP_GRIP] = (WPose){{0.01f, 0.02f, -0.05f}, {0, 0, 0, 1}};
    c.grip_mode = VRH_GRIP_AUTO; r = vrhands_create(&c); vrhands_bind(r, a.st, NULL, 0, 0);
    WPose g = vrhands_grip(r, a.st);
    CHECK(NEAR(g.p.z, -0.05f), "AUTO respeta def");
    vrhands_destroy(r);
    /* manual + escala + pitch */
    c.grip_mode = VRH_GRIP_MANUAL; c.grip_manual = wpose_identity(); c.scale = 0.001f; c.grip_pitch_deg = 90.0f;
    r = vrhands_create(&c); vrhands_bind(r, a.st, NULL, 0, 0);
    vrhands_build(r, a.st, &h, &f);
    fr = find_mesh(&f, 0, 0, a.m[0].v);
    /* pitch +90 sobre X: -Z del arma -> +Y; con escala 0.001 las columnas miden 1e-3 */
    CHECK(fr && NEAR(fr->model[0], 0.001f) && NEAR(fr->model[10], 0.0f) && NEAR(fr->model[9], -0.001f) && NEAR(fr->model[6], 0.001f), "escala y pitch (%.5f %.5f)", fr ? fr->model[10] : 0, fr ? fr->model[9] : 0);
    vrhands_destroy(r); syn_free(&a);
    /* sin gatillo: derive falla y AUTO cae a def (identidad) */
    Syn b; syn_init(&b, 0);
    WPose g2; CHECK(vrhands_derive_grip(b.st, &g2) == -1, "derive sin gatillo");
    VrHandsRig *r2 = vrhands_create(NULL); vrhands_bind(r2, b.st, NULL, 0, 0);
    { WPose gg = vrhands_grip(r2, b.st); CHECK(NEAR(gg.p.x, 0) && NEAR(gg.p.z, 0), "AUTO sin gatillo cae a def"); }
    vrhands_destroy(r2); syn_free(&b);
}

static void t_ids_ring_mailbox(void) {
    Syn a; syn_init(&a, 1);
    VrHandsRig *r = vrhands_create(NULL);
    vrhands_bind(r, a.st, NULL, 0, 0);
    VrHandsPose h = hp(0, 1, -0.3f, 0, 0, 0, 1);
    VrHandsFrame f1, f2, f3, f4;
    vrhands_build(r, a.st, &h, &f1);
    uint32_t id0 = f1.meshes[0].mesh_id; float m0 = f1.meshes[0].model[12];
    h.pos[0] = 0.5f; vrhands_build(r, a.st, &h, &f2);
    h.pos[0] = 0.7f; vrhands_build(r, a.st, &h, &f3);
    CHECK(f1.meshes != f2.meshes && f2.meshes != f3.meshes && f1.meshes != f3.meshes, "anillo distinto");
    CHECK(f1.meshes[0].mesh_id == id0 && f1.meshes[0].model[12] == m0, "frame viejo intacto tras 2 builds");
    CHECK(f2.meshes[0].mesh_id == id0 && f3.meshes[0].mesh_id == id0, "id estable entre frames");
    CHECK(f3.seq == f1.seq + 2, "seq");
    vrhands_build(r, a.st, &h, &f4);
    CHECK(f4.meshes == f1.meshes, "anillo de 3");
    /* re-enlazar cambia ids */
    vrhands_bind(r, a.st, NULL, 0, 0);
    vrhands_build(r, a.st, &h, &f4);
    CHECK(f4.meshes[0].mesh_id != id0, "rebind cambia id");
    /* ids distintos entre piezas */
    CHECK(f4.meshes[0].mesh_id != f4.meshes[1].mesh_id, "ids por pieza");
    /* buzón */
    VrHandsFrame out;
    vrhands_post_clear();
    CHECK(mv_host_hands(&out) == 0 && out.nmeshes == 0, "buzón vacío");
    vrhands_post(&f4);
    CHECK(mv_host_hands(&out) == 1 && out.nmeshes == f4.nmeshes && out.meshes != f4.meshes && out.meshes[1].mesh_id == f4.meshes[1].mesh_id, "buzón entrega copia");
    const VrHandsMesh *p1 = out.meshes;
    h.pos[0] = 0.1f; vrhands_build(r, a.st, &h, &f1); vrhands_post(&f1);
    CHECK(p1[0].model[12] != f1.meshes[0].model[12], "lo leído no se pisa por la publicación nueva");
    for (int i = 0; i < 10; i++) { h.pos[0] += 0.01f; vrhands_build(r, a.st, &h, &f2); vrhands_post(&f2); }
    CHECK(fabsf(p1[0].model[12] - (-0.0f)) < 10.0f && p1[0].mesh_id == f4.meshes[0].mesh_id, "el búfer en lectura sigue intacto");
    CHECK(mv_host_hands(&out) == 1 && fabsf(out.meshes[0].model[12] - f2.meshes[0].model[12]) < 1e-6f, "último publicado");
    /* frame vacío publicado => 0 */
    VrHandsPose bad = hp(0, 0, 0, 0, 0, 0, 1); bad.valid = 0;
    vrhands_build(r, a.st, &bad, &f3); vrhands_post(&f3);
    CHECK(mv_host_hands(&out) == 0, "vacío");
    vrhands_destroy(r); syn_free(&a);
}

static void t_busy_scale_limits(void) {
    Syn a; syn_init(&a, 1);
    VrHandsRig *r = vrhands_create(NULL); vrhands_bind(r, a.st, NULL, 0, 0);
    VrHandsPose h = hp(0, 1, -0.3f, 0, 0, 0, 1); VrHandsFrame f, out;
    mv_host_hands_done();     /* (el test anterior dejó una lectura pendiente) */
    vrhands_build(r, a.st, &h, &f); vrhands_post(&f);
    CHECK(!vrhands_render_busy(), "sin lectura pendiente");
    CHECK(mv_host_hands(&out) == 1 && vrhands_render_busy(), "leyendo => busy");
    vrhands_post_clear();
    CHECK(vrhands_render_busy(), "tras clear sigue busy hasta done (el render aún usa los punteros)");
    mv_host_hands_done();
    CHECK(!vrhands_render_busy(), "done libera");
    CHECK(mv_host_hands(&out) == 0 && !vrhands_render_busy(), "tras clear no hay frame y no queda busy");
    /* post con listas NULL pero contadores > 0: no revienta */
    VrHandsFrame bad; memset(&bad, 0, sizeof bad); bad.nmeshes = 5; bad.ntex = 2;
    vrhands_post(&bad);
    CHECK(mv_host_hands(&out) == 0, "post con meshes NULL = vacío");
    mv_host_hands_done();
    vrhands_destroy(r); syn_free(&a);
    /* agarre derivado con escala: modelo en mm (x1000) y scale 0.001 => mismo agarre en metros */
    Syn b; syn_init(&b, 1);
    WPose g1; vrhands_derive_grip(b.st, &g1);
    for (int i = 0; i < 4; i++) { WVertex *v = (WVertex *)b.m[i].v; for (uint32_t k = 0; k < b.m[i].nv; k++) for (int q = 0; q < 3; q++) v[k].pos[q] *= 1000.0f; }
    VrHandsConfig c; vrhands_default_config(&c); c.scale = 0.001f;
    VrHandsRig *r2 = vrhands_create(&c); vrhands_bind(r2, b.st, NULL, 0, 0);
    WPose g2 = vrhands_grip(r2, b.st);
    CHECK(NEAR(g2.p.x, g1.p.x) && NEAR(g2.p.y, g1.p.y) && NEAR(g2.p.z, g1.p.z), "derive con scale (%.4f %.4f %.4f)", g2.p.x, g2.p.y, g2.p.z);
    vrhands_destroy(r2); syn_free(&b);
    /* más de 64 mallas: ids colisionarían => bind rechazado */
    Syn d; syn_init(&d, 1);
    WMesh *many = calloc(65, sizeof(WMesh)); for (int i = 0; i < 65; i++) many[i] = d.m[i & 3];
    WeaponState *w = calloc(1, sizeof(WeaponState)); w->def = &d.def; weapon_bind(w, many, 65, d.p, 4);
    VrHandsRig *r3 = vrhands_create(NULL);
    CHECK(vrhands_bind(r3, w, NULL, 0, 0) == -2, "65 mallas rechazado");
    vrhands_destroy(r3); free(w); free(many); syn_free(&d);
}

static void t_invalid(void) {
    Syn a; syn_init(&a, 1);
    VrHandsRig *r = vrhands_create(NULL);
    VrHandsFrame f;
    VrHandsPose h = hp(0, 1, 0, 0, 0, 0, 1);
    CHECK(vrhands_build(r, a.st, &h, &f) == 0 && f.nmeshes == 0, "sin enlazar");
    vrhands_bind(r, a.st, NULL, 0, 0);
    VrHandsPose inv = h; inv.valid = 0;
    CHECK(vrhands_build(r, a.st, &inv, &f) == 0, "mano sin seguimiento");
    CHECK(vrhands_build(r, a.st, NULL, &f) == 0, "mano NULL");
    VrHandsPose nan = h; nan.pos[1] = NAN;
    CHECK(vrhands_build(r, a.st, &nan, &f) == 0, "pos NaN");
    VrHandsPose zq = hp(0, 1, 0, 0, 0, 0, 0);
    CHECK(vrhands_build(r, a.st, &zq, &f) == 0, "cuaternión nulo");
    CHECK(vrhands_build(NULL, a.st, &h, &f) == 0, "rig NULL");
    CHECK(vrhands_build(r, a.st, &h, NULL) == 0, "out NULL");
    /* canal NaN o infinito: la pieza se omite, el resto sigue */
    a.st->channel[WCH_SLIDE] = NAN; a.st->channel[WCH_MAG] = INFINITY;
    int n = vrhands_build(r, a.st, &h, &f);
    CHECK(n == 2, "canales no finitos omiten piezas (n=%d)", n);
    a.st->channel[WCH_SLIDE] = 0; a.st->channel[WCH_MAG] = 0;
    /* suelto con pose NaN o mesh fuera de rango */
    a.st->held[0].active = 1; a.st->held[0].mesh = 3; a.st->held[0].world = (WPose){{NAN, 0, 0}, {0, 0, 0, 1}};
    a.st->held[1].active = 1; a.st->held[1].mesh = 99; a.st->held[1].world = wpose_identity();
    a.st->held[2].active = 1; a.st->held[2].mesh = -1;
    n = vrhands_build(r, a.st, &h, &f);
    CHECK(n == 4, "sueltos inválidos omitidos (n=%d)", n);
    /* estado a cero (recoil_offset con q = 0) */
    WeaponState *z = calloc(1, sizeof(WeaponState)); z->def = &a.def; weapon_bind(z, a.m, 4, a.p, 4);
    vrhands_bind(r, z, NULL, 0, 0);
    n = vrhands_build(r, z, &h, &f);
    CHECK(n == 4, "estado a cero (recoil q=0) n=%d", n);
    free(z);
    /* malla con índice fuera de rango: bind rechazado */
    uint16_t bad[36]; memcpy(bad, BOXI, sizeof bad); bad[5] = 9;
    uint16_t *keep = (uint16_t *)a.m[1].idx; a.m[1].idx = bad;
    WeaponState *w = calloc(1, sizeof(WeaponState)); w->def = &a.def; weapon_bind(w, a.m, 4, a.p, 4);
    CHECK(vrhands_bind(r, w, NULL, 0, 0) == -2, "índice fuera de rango rechazado");
    a.m[1].idx = keep; free(w);
    CHECK(vrhands_bind(r, a.st, (uint8_t *)"x", 0, 4) == -1, "atlas de 0 px");
    /* tope de enlaces */
    WeaponState *ws[VRH_MAX_BOUND + 1]; int rc = 0;
    for (int i = 0; i <= VRH_MAX_BOUND; i++) { ws[i] = calloc(1, sizeof(WeaponState)); ws[i]->def = &a.def; weapon_bind(ws[i], a.m, 4, a.p, 4); rc = vrhands_bind(r, ws[i], NULL, 0, 0); }
    CHECK(rc == -3, "demasiados enlaces");
    for (int i = 0; i <= VRH_MAX_BOUND; i++) { vrhands_unbind(r, ws[i]); free(ws[i]); }
    vrhands_unbind(r, a.st);
    CHECK(vrhands_build(r, a.st, &h, &f) == 0, "tras unbind");
    vrhands_destroy(r); syn_free(&a);
}

static void t_fuzz(void) {
    Syn a; syn_init(&a, 1);
    VrHandsRig *r = vrhands_create(NULL); vrhands_bind(r, a.st, NULL, 0, 0);
    uint32_t seed = 12345;
    VrHandsFrame f;
    for (int it = 0; it < 5000; it++) {
        for (int c = 0; c < WCH_COUNT; c++) a.st->channel[c] = wcore_rand(&seed) * 60.0f;
        for (int k = 0; k < W_MAX_HELD; k++) { a.st->held[k].active = (wcore_rand(&seed) > 0); a.st->held[k].mesh = (int)(wcore_rand(&seed) * 6); a.st->held[k].world = (WPose){{wcore_rand(&seed) * 3, wcore_rand(&seed) * 3, wcore_rand(&seed) * 3}, wquat_normalize((WQuat){wcore_rand(&seed), wcore_rand(&seed), wcore_rand(&seed), 1})}; }
        VrHandsPose h = hp(wcore_rand(&seed), wcore_rand(&seed) + 1, wcore_rand(&seed), wcore_rand(&seed), wcore_rand(&seed), wcore_rand(&seed), wcore_rand(&seed) + 1.5f);
        int n = vrhands_build(r, a.st, &h, &f);
        CHECK(n >= 0 && n <= VRH_MAX_MESHES, "fuzz n=%d", n);
        for (int i = 0; i < n; i++) for (int k = 0; k < 16; k++) if (!isfinite(f.meshes[i].model[k])) { CHECK(0, "no finito"); it = 99999; break; }
        vrhands_post(&f);
        VrHandsFrame o; mv_host_hands(&o);
    }
    vrhands_destroy(r); syn_free(&a);
}

int main(void) {
    t_grip_and_transforms();
    t_modes_scale_pitch();
    t_ids_ring_mailbox();
    t_busy_scale_limits();
    t_invalid();
    t_fuzz();
    if (fails) { printf("test_hands: %d FALLOS\n", fails); return 1; }
    printf("test_hands: OK\n");
    return 0;
}
