/* Pruebas de vr_grip (agarre por arma): con piezas primitivas que hacen de pistola (mango, corredera y gatillo como cajas) se comprueba que el centro de agarre de la mano cae en el
 * agarre que deriva vrhands_derive_grip (< 10 mm) y la punta del índice sobre el gatillo (< 8 mm), en las dos manos y para varios tamaños de manopla; y la coherencia de toda la tabla.
 * Con datos del juego (argv[1] = archivo de stages, argv[2] = stage, por defecto "init") repite la comprobación con el modelo real de la pistola; sin ellos se omite.
 * make -C vr/hands test */
#include "vr_grip.h"
#include "vr_hands.h"
#include "../weapons/wcut.h"
#include "../weapons/wload.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO l.%d: %s ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static float dist(WVec3 a, WVec3 b) { WVec3 d = wv_sub(a, b); return sqrtf(wv_dot(d, d)); }

/* ---- pistola primitiva (mm, marco del modelo: -Z boca, +Y arriba; el origen del modelo NO está en el mango, como en el modelo real) ---- */
static WVertex *box(float x0, float y0, float z0, float x1, float y1, float z1) {
    WVertex *v = calloc(8, sizeof(WVertex));
    for (int i = 0; i < 8; i++) {
        v[i].pos[0] = (i & 1 ? x1 : x0) * 0.001f; v[i].pos[1] = (i & 2 ? y1 : y0) * 0.001f; v[i].pos[2] = (i & 4 ? z1 : z0) * 0.001f;
        v[i].nrm[1] = 1; memset(v[i].rgba, 255, 4);
    }
    return v;
}
static const uint16_t BOXI[36] = {0,1,3, 0,3,2, 4,6,7, 4,7,5, 0,2,6, 0,6,4, 1,5,7, 1,7,3, 2,3,7, 2,7,6, 0,4,5, 0,5,1};
static const char *const NAMES[] = {"frame", "slide", "trigger", NULL};

typedef struct { WeaponDef def; WeaponState st; WMesh m[3]; WPart p[3]; WVec3 trigger_c; } Proxy;
/* Pistola de referencia: mango x +-16, y -100..0, z 0..35 (centro (0, -50, 17.5)); bloque delantero del armazón bajo la corredera; corredera y 0..32; gatillo a 63 mm por delante y 32 por encima
 * del centro del mango: centro (0, -18, -45.5). */
static void proxy_init(Proxy *a) {
    memset(a, 0, sizeof *a);
    WVertex *fv = calloc(16, sizeof(WVertex));
    WVertex *g = box(-16, -100, 0, 16, 0, 35), *f = box(-10, -30, -120, 10, 0, 0);
    memcpy(fv, g, 8 * sizeof(WVertex)); memcpy(fv + 8, f, 8 * sizeof(WVertex)); free(g); free(f);
    uint16_t *fi = malloc(72 * sizeof(uint16_t));
    for (int i = 0; i < 36; i++) { fi[i] = BOXI[i]; fi[36 + i] = (uint16_t)(BOXI[i] + 8); }
    uint16_t *si = malloc(sizeof BOXI), *ti = malloc(sizeof BOXI);
    memcpy(si, BOXI, sizeof BOXI); memcpy(ti, BOXI, sizeof BOXI);
    a->m[0] = (WMesh){NAMES[0], fv, 16, fi, 72, 0};
    a->m[1] = (WMesh){NAMES[1], box(-10, 0, -190, 10, 32, 10), 8, si, 36, 0};
    a->m[2] = (WMesh){NAMES[2], box(-3, -26, -49, 3, -10, -42), 8, ti, 36, 0};
    a->trigger_c = wv(0, -0.018f, -0.0455f);
    for (int i = 0; i < 3; i++) a->p[i] = (WPart){NAMES[i], i ? 0 : -1, {{0, 0, 0}, {0, 0, 0, 1}}, i, WCH_NONE, {0, 0, 0}, 0};
    a->def = (WeaponDef){.id = "proxy", .mesh_names = NAMES, .state_size = sizeof(WeaponState)};
    for (int i = 0; i < WP_COUNT; i++) a->def.points[i] = wpose_identity();
    a->st.def = &a->def;
    a->st.recoil_offset = wpose_identity();
    for (int i = 0; i < W_MAX_HELD; i++) a->st.held[i].mesh = -1;
    weapon_bind(&a->st, a->m, 3, a->p, 3);
}
static void proxy_free(Proxy *a) { for (int i = 0; i < 3; i++) { free((void *)a->m[i].v); free((void *)a->m[i].idx); } }

/* Centro de agarre (m) y punta del índice (m) de una mano construida con unas medidas, con la pose del arma y el gatillo suelto, en el marco de la mano. */
static void hand_points(const VrHand *h, int pose, float trigger, WVec3 *palm, WVec3 *tip) {
    float xf[VRHAND_BONES][12], pc[3], t[3];
    VrHandPoseIn in = {pose, 1.0f, trigger, 0.0f, 0.0f};
    vrhandpose_blend(h, &in, xf);
    vrhandpose_index_tip(h, xf, t);
    vrhand_palm_center(h, pc);
    const float um = vrhand_unit_m(h);
    *palm = wv(pc[0] * um, pc[1] * um, pc[2] * um); *tip = wv(t[0] * um, t[1] * um, t[2] * um);
}

static VrHandDims scaled_dims(float k) {
    VrHandDims e; vrhand_dims_default(&e);
    e.cuff_y *= k; e.tip_y *= k; e.cuff_h[0] *= k; e.cuff_h[1] *= k; e.cuff_c[0] *= k; e.cuff_c[1] *= k;
    return e;
}

static void test_proxy(void) {
    Proxy a; proxy_init(&a);
    WPose g;
    CHECK(vrhands_derive_grip(&a.st, &g) == 0, "derive_grip de la pistola primitiva");
    CHECK(dist(g.p, wv(0, -0.050f, 0.0175f)) < 1e-4f, "agarre derivado (%.4f %.4f %.4f)", g.p.x, g.p.y, g.p.z);
    const VrGripEntry *e = vrgrip_find(0);
    CHECK(e && e->pose == HP_GRIP_PISTOL && e->has_trigger, "entrada de la pistola");
    if (!e) { proxy_free(&a); return; }
    static const float ks[4] = {0.8f, 1.0f, 1.25f, 1.6f};
    for (int ki = 0; ki < 4; ki++) for (int side = 1; side >= -1; side -= 2) {
        VrHandDims d = scaled_dims(ks[ki]);
        VrHand h;
        CHECK(vrhand_build(&h, &d, side) == 0, "build");
        WVec3 palm, tip;
        hand_points(&h, e->pose, 0.0f, &palm, &tip);
        WPose W = vrgrip_weapon_from_hand(g, e->grip, side);     /* arma <- mano */
        WVec3 P = wpose_apply(W, palm), I = wpose_apply(W, tip);
        float dp = dist(P, g.p) * 1000.0f, di = dist(I, a.trigger_c) * 1000.0f;
        CHECK(dp < 10.0f, "escala %.2f lado %d: centro de agarre a %.1f mm del mango derivado", ks[ki], side, dp);
        CHECK(di < 8.0f, "escala %.2f lado %d: punta del índice a %.1f mm del gatillo", ks[ki], side, di);
        /* el punto del gatillo de la tabla (en el marco de empuñadura) coincide con la punta */
        WPose Wh = vrgrip_weapon_from_hand(wpose_identity(), e->grip, side);
        WVec3 Ih = wpose_apply(Wh, tip);
        CHECK(fabsf(Ih.x) < 0.008f && dist(Ih, wv(side < 0 ? -e->trigger.x : e->trigger.x, e->trigger.y, e->trigger.z)) < 0.008f, "escala %.2f lado %d: trigger de la tabla (%.4f %.4f %.4f) vs punta (%.4f %.4f %.4f)",
              ks[ki], side, e->trigger.x, e->trigger.y, e->trigger.z, Ih.x, Ih.y, Ih.z);
        /* con el gatillo a fondo la yema se mueve hacia atrás (+Z del arma) y no más de 40 mm */
        WVec3 palm1, tip1;
        hand_points(&h, e->pose, 1.0f, &palm1, &tip1);
        WVec3 I1 = wpose_apply(W, tip1);
        CHECK(I1.z - I.z > 0.012f && I1.z - I.z < 0.040f, "escala %.2f lado %d: recorrido del gatillo %.1f mm", ks[ki], side, (I1.z - I.z) * 1000.0f);
    }
    /* VRH_GRIP_SOCKET con el zócalo de la tabla (reflejado para la izquierda): mano -> arma por vrgrip_weapon_from_hand, la misma cadena que usa vrhands_weapon_world */
    for (int side = 1; side >= -1; side -= 2) {
        VrHandsConfig c; vrhands_default_config(&c);
        c.grip_mode = VRH_GRIP_SOCKET;
        VrHandsRig *r = vrhands_create(&c);
        vrhands_bind(r, &a.st, NULL, 0, 0);
        CHECK(vrhands_set_socket(r, side < 0 ? vrgrip_mirror(e->grip) : e->grip) == 0, "zócalo lado %d", side);
        VrHandsPose hp = {1, {0.2f, 1.1f, -0.3f}, {0.0f, 0.38268343f, 0.0f, 0.92387953f}};
        WPose H = vrhands_pose_to_w(&hp), W = vrhands_weapon_world(r, &a.st, &hp);
        WPose chain = wpose_mul(W, vrgrip_weapon_from_hand(a.def.points[WP_GRIP], e->grip, side));
        CHECK(dist(chain.p, H.p) < 1e-4f, "lado %d: arma * (agarre * zócalo) = mano (%.6f m)", side, dist(chain.p, H.p));
        vrhands_destroy(r);
    }
    proxy_free(&a);
}

static void test_table(void) {
    CHECK(vrgrip_count() == w_registry_count, "la tabla tiene %d entradas y el registro %d armas", vrgrip_count(), w_registry_count);
    for (int i = 0; i < w_registry_count; i++) {
        const WeaponDef *d = w_registry[i];
        const VrGripEntry *e = vrgrip_find(d->game_weapon_id);
        CHECK(e != NULL, "arma %s (id %d) sin entrada de agarre", d->id, d->game_weapon_id);
        if (!e) continue;
        CHECK(e->has_trigger == vrhandpose_has_trigger(e->pose), "%s: has_trigger no coincide con la pose", d->id);
        CHECK(e->pose >= HP_GRIP_PISTOL && e->pose < HP_COUNT, "%s: pose %d", d->id, e->pose);
        CHECK(e->support_pose == -1 || (e->support_pose >= HP_GRIP_PISTOL && e->support_pose < HP_COUNT && !vrhandpose_has_trigger(e->support_pose)), "%s: pose de apoyo %d", d->id, e->support_pose);
        for (int j = i + 1; j < w_registry_count; j++) CHECK(e != vrgrip_find(w_registry[j]->game_weapon_id), "id repetido");
        /* una mano con apoyo en el arma tiene punto de apoyo propio (WP_SUPPORT); las armas sin apoyo (granadas, colocables) no tienen otra pose */
        if (d->mode == WF_THROWN || d->mode == WF_PLACED) CHECK(e->support_pose == -1 && !e->has_trigger, "%s: lanzables y colocables van a una mano", d->id);
        else CHECK(e->support_pose == HP_GRIP_SUPPORT && e->has_trigger, "%s: arma de dos manos sin pose de apoyo o sin gatillo", d->id);
        /* los zócalos son rotaciones propias (cuaternión unitario) y llevan la palma y el pulgar a donde se supone */
        const WPose *sockets[2] = {&e->grip, &e->support};
        for (int s = 0; s < 2; s++) {
            if (s == 1 && e->support_pose < 0) continue;
            const WPose *p = sockets[s];
            float n = sqrtf(p->q.x * p->q.x + p->q.y * p->q.y + p->q.z * p->q.z + p->q.w * p->q.w);
            CHECK(fabsf(n - 1.0f) < 1e-3f, "%s zócalo %d: |q| = %.5f", d->id, s, n);
            WVec3 fing = wpose_apply((WPose){{0, 0, 0}, p->q}, wv(0, -1, 0)), thumb = wpose_apply((WPose){{0, 0, 0}, p->q}, wv(0, 0, 1)), palmn = wpose_apply((WPose){{0, 0, 0}, p->q}, wv(1, 0, 0));
            if (s == 0) {   /* apretón de manos: dedos hacia la boca (-Z), pulgar arriba (+Y), palma a la izquierda (-X); con guiñada de hasta 20 grados */
                CHECK(fing.z < -0.93f && thumb.y > 0.99f && palmn.x < -0.93f, "%s: orientación de la mano del arma (dedos %.2f %.2f %.2f pulgar %.2f palma %.2f)", d->id, fing.x, fing.y, fing.z, thumb.y, palmn.x);
            } else {        /* apoyo bajo el guardamanos: pulgar hacia la boca, palma hacia arriba, dedos hacia -X para la derecha */
                CHECK(thumb.z < -0.99f && palmn.y > 0.99f && fing.x < -0.99f, "%s: orientación de la mano de apoyo", d->id);
            }
        }
        /* el centro de agarre cae en el origen del marco de empuñadura / apoyo */
        VrHand h;
        vrhand_build(&h, NULL, 1);
        WVec3 palm, tip;
        hand_points(&h, e->pose, 0.0f, &palm, &tip);
        CHECK(dist(wpose_apply(e->grip, palm), wv(0, 0, 0)) < 0.010f, "%s: centro de agarre a %.1f mm del punto de empuñadura", d->id, dist(wpose_apply(e->grip, palm), wv(0, 0, 0)) * 1000.0f);
        if (e->support_pose >= 0) {
            hand_points(&h, e->support_pose, 0.0f, &palm, &tip);
            CHECK(dist(wpose_apply(e->support, palm), wv(0, 0, 0)) < 0.010f, "%s: centro de agarre de apoyo a %.1f mm de WP_SUPPORT", d->id, dist(wpose_apply(e->support, palm), wv(0, 0, 0)) * 1000.0f);
        }
        if (e->has_trigger) {
            hand_points(&h, e->pose, 0.0f, &palm, &tip);
            CHECK(dist(wpose_apply(e->grip, tip), e->trigger) < 0.008f, "%s: trigger de la tabla a %.1f mm de la punta", d->id, dist(wpose_apply(e->grip, tip), e->trigger) * 1000.0f);
        }
    }
    CHECK(vrgrip_find(-1) == NULL && vrgrip_find(1000) == NULL && vrgrip_at(-1) == NULL && vrgrip_at(vrgrip_count()) == NULL && vrgrip_at(0) == vrgrip_find(0), "búsquedas fuera de rango");
    /* espejo: involución, y la mano izquierda (malla espejada) lleva su centro de agarre al mismo punto */
    const VrGripEntry *e = vrgrip_at(1);
    WPose m = vrgrip_mirror(e->grip), mm = vrgrip_mirror(m);
    CHECK(fabsf(mm.p.x - e->grip.p.x) < 1e-7f && fabsf(mm.q.y - e->grip.q.y) < 1e-7f && fabsf(mm.q.z - e->grip.q.z) < 1e-7f && m.p.x == -e->grip.p.x, "espejo no es involución");
}

#ifndef _WIN32
#include <sys/stat.h>
#endif
/* Con datos del juego: pistola real. El agarre del modelo es el derivado (centro de agarre exacto en el mango); el gatillo real solo se informa y falla si está a más de 25 mm. */
static void test_real(const char *path, const char *stage) {
    struct stat sb;
    if (!path || stat(path, &sb) != 0) { printf("  (sin datos del juego%s%s: se omite el modelo real)\n", path ? ": " : "", path ? path : ""); return; }
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  (no se puede leer %s: se omite el modelo real)\n", path); return; }
    size_t n = (size_t)sb.st_size;
    uint8_t *data = malloc(n + 1);
    if (!data || fread(data, 1, n, f) != n) { printf("  (lectura incompleta de %s: se omite)\n", path); fclose(f); free(data); return; }
    fclose(f);
    const uint16_t id = 0x36f6;
    WLoadOpts o; wload_default_opts(&o); o.resident = stage; o.upscale = 2;
    WLoaded m;
    int r = wload_model(data, n, stage, id, &o, &m);
    if (r) { printf("  (wload_model=%d con %s/%s: se omite el modelo real)\n", r, path, stage); free(data); return; }
    const WCutModel *cm = wcut_find(id);
    WCutSet cs;
    r = cm ? wcut_run(&m, cm, &cs) : -1;
    const WeaponDef *def = NULL;
    for (int i = 0; r == 0 && i < w_registry_count; i++) if (cm->weapon && !strcmp(w_registry[i]->id, cm->weapon)) def = w_registry[i];
    if (r || !def) { printf("  (wcut_run=%d: se omite el modelo real)\n", r); wload_free(&m); free(data); return; }
    WeaponState *st = calloc(1, def->state_size);
    st->def = def; def->init(st);
    weapon_bind(st, cs.meshes, cs.nmeshes, cs.parts, cs.nparts);
    WPose g;
    int dr = vrhands_derive_grip(st, &g);
    CHECK(dr == 0, "agarre derivado del modelo real");
    const VrGripEntry *e = vrgrip_find(def->game_weapon_id);
    CHECK(e != NULL, "entrada de la pistola real");
    int it = wcore_part_find(st, "trigger");
    if (dr == 0 && e && it >= 0) {
        int np; weapon_parts(st, &np);
        WPose po[64]; WeaponState tmp = *st; memset(tmp.channel, 0, sizeof tmp.channel);
        int cnt = wcore_part_poses(&tmp, wpose_identity(), po, 64);
        int mi = weapon_part_mesh(st, it), nm; const WMesh *ms = weapon_meshes(st, &nm);
        if (it < cnt && mi >= 0 && mi < nm && ms[mi].nv) {
            WVec3 lo = wv(1e30f, 1e30f, 1e30f), hi = wv(-1e30f, -1e30f, -1e30f);
            for (uint32_t i = 0; i < ms[mi].nv; i++) {
                WVec3 p = wpose_apply(po[it], wv(ms[mi].v[i].pos[0], ms[mi].v[i].pos[1], ms[mi].v[i].pos[2]));
                lo = wv(fminf(lo.x, p.x), fminf(lo.y, p.y), fminf(lo.z, p.z)); hi = wv(fmaxf(hi.x, p.x), fmaxf(hi.y, p.y), fmaxf(hi.z, p.z));
            }
            WVec3 T = wv((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f);
            VrHand h; vrhand_build(&h, NULL, 1);
            WVec3 palm, tip;
            hand_points(&h, e->pose, 0.0f, &palm, &tip);
            WPose W = vrgrip_weapon_from_hand(g, e->grip, 1);
            float dp = dist(wpose_apply(W, palm), g.p) * 1000.0f, di = dist(wpose_apply(W, tip), T) * 1000.0f;
            printf("  modelo real %04x: palma-mango %.1f mm, yema-gatillo %.1f mm (centro de la pieza gatillo)\n", id, dp, di);
            CHECK(dp < 10.0f, "modelo real: centro de agarre a %.1f mm del mango", dp);
            CHECK(di < 25.0f, "modelo real: la yema queda a %.1f mm del gatillo (afinar la tabla de vr_grip.c)", di);
        }
    }
    free(st); wcut_free(&cs); wload_free(&m); free(data);
}

int main(int argc, char **argv) {
    test_proxy();
    test_table();
    test_real(argc > 1 ? argv[1] : NULL, argc > 2 ? argv[2] : "init");
    printf("%s vr_grip: %d comprobaciones, %d fallos\n", fails ? "FALLO" : "OK", checks, fails);
    return fails ? 1 : 0;
}
