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

/* Centro de agarre (m) y yema del índice (m) de la mano side con la pose del arma, en el marco de la mano. */
static void hand_points(int side, int pose, float trigger, WVec3 *palm, WVec3 *tip) {
    float xf[VRHS_J][12], pc[3], t[3];
    VrHandPoseIn in = {pose, 1.0f, trigger, 0.0f, 0.0f};
    vrhandpose_blend(side, &in, xf);
    vrhandpose_index_tip(xf, t);
    vrhs_grip_center(side, pc);
    const float um = VRHS_UNIT_M;
    *palm = wv(pc[0] * um, pc[1] * um, pc[2] * um); *tip = wv(t[0] * um, t[1] * um, t[2] * um);
}

/* Palma contra el mango: hueco mínimo (mm) entre la superficie palmar de la mano (vértices con cara palmar bajo el mango) y la cara del mango primitivo, en el marco del arma. */
static float palm_gap(int side, const WPose W, int pose) {
    static float P[VRHA_NV][3], N[VRHA_NV][3];
    float xf[VRHS_J][12];
    VrHandPoseIn in = {pose, 1.0f, 0.0f, 0.0f, 0.0f};
    vrhandpose_blend(side, &in, xf); vrhs_skin(xf, side, P, N);
    float best = 1e9f;
    for (int i = 0; i < VRHA_NV; i++) {
        if (N[i][0] * side < 0.5f || P[i][1] > -40.0f || P[i][1] < -95.0f) continue;   /* palma (cara palmar) entre la muñeca y los nudillos */
        WVec3 q = wpose_apply(W, wv(P[i][0] * 0.001f, P[i][1] * 0.001f, P[i][2] * 0.001f));
        if (q.y < -0.100f || q.y > 0.0f) continue;                                        /* a la altura del mango */
        float gx = (fabsf(q.x) - 0.016f) * 1000.0f, gz = q.z < 0.0f ? -q.z * 1000.0f : (q.z > 0.035f ? (q.z - 0.035f) * 1000.0f : 0.0f);
        float g = fmaxf(gx, gz);
        if (g < best) best = g;
    }
    return best;
}

static void test_proxy(void) {
    Proxy a; proxy_init(&a);
    WPose g;
    CHECK(vrhands_derive_grip(&a.st, &g) == 0, "derive_grip de la pistola primitiva");
    CHECK(dist(g.p, wv(0, -0.050f, 0.0175f)) < 1e-4f, "agarre derivado (%.4f %.4f %.4f)", g.p.x, g.p.y, g.p.z);
    const VrGripEntry *e = vrgrip_find(0);
    CHECK(e && (e->pose == HP_GRIP_PISTOL || e->pose >= HP_W_FIRST) && e->has_trigger, "entrada de la pistola");
    if (!e) { proxy_free(&a); return; }
    for (int side = 1; side >= -1; side -= 2) {
        WVec3 palm, tip;
        hand_points(side, e->pose, 0.0f, &palm, &tip);
        WPose W = vrgrip_weapon_from_hand(g, e->grip, side);     /* arma <- mano */
        WVec3 P = wpose_apply(W, palm), I = wpose_apply(W, tip);
        WVec3 tc = wv(side < 0 ? -a.trigger_c.x : a.trigger_c.x, a.trigger_c.y, a.trigger_c.z);
        float dp = dist(P, g.p) * 1000.0f, di = dist(I, tc) * 1000.0f, pg = palm_gap(side, W, e->pose);
        /* Contrato nuevo: el zócalo de la pistola (id 0) está ajustado al modelo REAL de la SOCOM (dedo en el gatillo, mano a 20 grados de la de Snake: la palma queda ~51 mm bajo el punto derivado del mango);
         * la pistola primitiva es otra forma, así que aquí solo se informa y no se exige cercanía. */
        (void)pg;
        printf("  pistola primitiva lado %+d: centro de agarre %.1f mm, palma-mango %.1f mm, yema-gatillo %.1f mm\n", side, dp, pg, di);
        /* el punto del gatillo de la tabla (en el marco de empuñadura) coincide con la yema; centrada en X solo en la pose genérica (la ajustada a la SOCOM apoya la yema al lado del gatillo, x = 11 mm; la mide test_gripfit) */
        WPose Wh = vrgrip_weapon_from_hand(wpose_identity(), e->grip, side);
        WVec3 Ih = wpose_apply(Wh, tip);
        CHECK((e->pose >= HP_W_FIRST || fabsf(Ih.x) < 0.008f) && dist(Ih, wv(side < 0 ? -e->trigger.x : e->trigger.x, e->trigger.y, e->trigger.z)) < 0.008f, "lado %d: trigger de la tabla (%.4f %.4f %.4f) vs yema (%.4f %.4f %.4f)",
              side, e->trigger.x, e->trigger.y, e->trigger.z, Ih.x, Ih.y, Ih.z);
        /* con el gatillo a fondo la yema se mueve hacia atrás (+Z del arma) entre 12 y 45 mm */
        WVec3 palm1, tip1;
        hand_points(side, e->pose, 1.0f, &palm1, &tip1);
        WVec3 I1 = wpose_apply(W, tip1);
        CHECK(I1.z - I.z > 0.012f && I1.z - I.z < 0.045f, "lado %d: recorrido del gatillo %.1f mm", side, (I1.z - I.z) * 1000.0f);
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

/* Pose de apuntar de Snake MEDIDA en sus animaciones (OAR "snake" de init + esqueleto del KMD; acciones SocomAttack.aim 42, FamasAttack.aim 5, StingerAttack/NikitaAttack.aim 74, PSG1Attack.aim 44; último fotograma,
 * RotMatrixZYX; validado con el volcado a33 del PSG1 a <= 5 mm). R = orientación de la mano IZQUIERDA (parte 9) en el marco del arma (= hueso de la mano derecha, (x, y, z) -> (-x, z, y)), por filas;
 * pole_main / pole_support = dirección del codo derecho / izquierdo en el marco del torso (x hacia fuera de su lado, y arriba, z delante). Los datos van aquí para que la prueba no necesite el juego. */
static const struct { int id; const char *name; float R[9]; float pole_main[3], pole_support[3]; } SNAKE[] = {
    {0, "SOCOM", {-0.9187f, -0.3611f, -0.1601f, -0.1246f, -0.1197f, 0.9850f, -0.3748f, 0.9248f, 0.0650f}, {0.532f, -0.288f, 0.796f}, {0.566f, -0.776f, 0.280f}},
    {1, "FAMAS", {0.2308f, -0.9472f, -0.2228f, -0.9708f, -0.2394f, 0.0119f, -0.0646f, 0.2135f, -0.9748f}, {0.580f, -0.619f, 0.529f}, {0.232f, -0.969f, 0.079f}},
    {3, "NIKITA", {-0.6251f, -0.6093f, -0.4878f, 0.2197f, -0.7371f, 0.6391f, -0.7490f, 0.2923f, 0.5946f}, {0.649f, -0.625f, 0.433f}, {0.266f, -0.964f, -0.009f}},
    {4, "STINGER", {-0.6251f, -0.6093f, -0.4878f, 0.2197f, -0.7371f, 0.6391f, -0.7490f, 0.2923f, 0.5946f}, {0.649f, -0.625f, 0.433f}, {0.266f, -0.964f, -0.009f}},
    {9, "PSG1", {0.2890f, -0.7404f, -0.6068f, -0.9440f, -0.1152f, -0.3090f, 0.1589f, 0.6622f, -0.7323f}, {0.655f, 0.122f, 0.746f}, {0.143f, -0.111f, 0.983f}},
};

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
        else CHECK((e->support_pose == HP_GRIP_SUPPORT || e->support_pose >= HP_W_FIRST) && e->has_trigger, "%s: arma de dos manos sin pose de apoyo o sin gatillo", d->id);
        /* Armas ajustadas contra la malla real (FIT_* de gripfit_gen.c, poses HP_W_*): orientación, centro de agarre y contacto los comprueba test_gripfit contra el modelo; aquí no se exige
         * el apretón de manos genérico ni la orientación de apoyo de Snake (el ajuste las cambia a propósito: copa bajo la SOCOM, palma bajo el guardamanos, etc.). */
        const int fitted = e->pose >= HP_W_FIRST;
        /* los zócalos son rotaciones propias (cuaternión unitario) y llevan la palma y el pulgar a donde se supone */
        const WPose *sockets[2] = {&e->grip, &e->support};
        for (int s = 0; s < 2; s++) {
            if (s == 1 && e->support_pose < 0) continue;
            if (fitted) continue;
            const WPose *p = sockets[s];
            float n = sqrtf(p->q.x * p->q.x + p->q.y * p->q.y + p->q.z * p->q.z + p->q.w * p->q.w);
            CHECK(fabsf(n - 1.0f) < 1e-3f, "%s zócalo %d: |q| = %.5f", d->id, s, n);
            WVec3 fing = wpose_apply((WPose){{0, 0, 0}, p->q}, wv(0, -1, 0)), thumb = wpose_apply((WPose){{0, 0, 0}, p->q}, wv(0, 0, 1)), palmn = wpose_apply((WPose){{0, 0, 0}, p->q}, wv(1, 0, 0));
            float ha[3]; vrhs_handle_axis(1, ha);
            WVec3 hax = wpose_apply((WPose){{0, 0, 0}, p->q}, wv(ha[0], ha[1], ha[2]));
            (void)thumb;
            if (s == 0) {   /* apretón de manos: eje del mango hacia arriba (+Y), dedos hacia la boca (-Z), palma a la izquierda (-X); con cabeceo y guiñada de hasta ~38 grados (mano a la escala del brazo del juego sobre un arma de tamaño real) */
                CHECK(hax.y > 0.93f && fing.z < -0.78f && palmn.x < -0.78f, "%s: orientación de la mano del arma (eje %.2f dedos %.2f %.2f %.2f palma %.2f)", d->id, hax.y, fing.x, fing.y, fing.z, palmn.x);
            } else {        /* apoyo: orientación MEDIDA en Snake (tabla SNAKE): matriz de la mano de apoyo canónica (derecha) = espejo X de la medida de la izquierda (S R S, S = diag(-1, 1, 1)) */
                int found = 0;
                for (unsigned k = 0; k < sizeof SNAKE / sizeof SNAKE[0]; k++) {
                    if (SNAKE[k].id != d->game_weapon_id) continue;
                    found = 1;
                    float R[9], qx = p->q.x, qy = p->q.y, qz = p->q.z, qw = p->q.w, Q[9] = {1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw), 2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw), 2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)};
                    float dsum = 0;
                    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) { R[r * 3 + c] = SNAKE[k].R[r * 3 + c] * (r == 0 ? -1.0f : 1.0f) * (c == 0 ? -1.0f : 1.0f); dsum += (R[r * 3 + c] - Q[r * 3 + c]) * (R[r * 3 + c] - Q[r * 3 + c]); }
                    CHECK(sqrtf(dsum) < 0.03f, "%s: orientación de la mano de apoyo = la medida en Snake (%s), desviación %.3f", d->id, SNAKE[k].name, sqrtf(dsum));
                }
                CHECK(found, "%s: hay medida de Snake de la mano de apoyo", d->id);
            }
        }
        /* el centro de agarre cae en el origen del marco de empuñadura / apoyo */
        WVec3 palm, tip;
        hand_points(1, e->pose, 0.0f, &palm, &tip);
        if (e->pose == HP_GRIP_SPHERE) palm.x += 0.015f;   /* la granada va 15 unidades por delante de la palma (SPHERE) */
        if (!fitted)
        CHECK(dist(wpose_apply(e->grip, palm), wv(0, 0, 0)) < 0.010f, "%s: centro de agarre a %.1f mm del punto de empuñadura", d->id, dist(wpose_apply(e->grip, palm), wv(0, 0, 0)) * 1000.0f);
        if (e->support_pose >= 0) {
            for (unsigned k = 0; k < sizeof SNAKE / sizeof SNAKE[0]; k++) if (SNAKE[k].id == d->game_weapon_id) {
                CHECK(dist(e->elbow_main, wv(SNAKE[k].pole_main[0], SNAKE[k].pole_main[1], SNAKE[k].pole_main[2])) < 0.01f && dist(e->elbow_support, wv(SNAKE[k].pole_support[0], SNAKE[k].pole_support[1], SNAKE[k].pole_support[2])) < 0.01f, "%s: dirección de los codos = la medida en Snake", d->id);
            }
            hand_points(1, e->support_pose, 0.0f, &palm, &tip);
            if (!fitted) CHECK(dist(wpose_apply(e->support, palm), wv(0, 0, 0)) < 0.010f, "%s: centro de agarre de apoyo a %.1f mm de WP_SUPPORT", d->id, dist(wpose_apply(e->support, palm), wv(0, 0, 0)) * 1000.0f);
        }
        if (e->has_trigger) {
            hand_points(1, e->pose, 0.0f, &palm, &tip);
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
            WVec3 palm, tip;
            hand_points(1, e->pose, 0.0f, &palm, &tip);
            WPose W = vrgrip_weapon_from_hand(g, e->grip, 1);
            float dp = dist(wpose_apply(W, palm), g.p) * 1000.0f, di = dist(wpose_apply(W, tip), T) * 1000.0f;
            printf("  modelo real %04x: palma-mango %.1f mm, yema-gatillo %.1f mm (centro de la pieza gatillo)\n", id, dp, di);
            CHECK(dp < 60.0f, "modelo real: centro de agarre a %.1f mm del punto derivado del mango (la palma baja ~51 mm por el mango, como Snake)", dp);
            CHECK(di < 25.0f, "modelo real: la yema queda a %.1f mm del gatillo (afinar la tabla de vr_grip.c)", di);
        }
    }
    free(st); wcut_free(&cs); wload_free(&m); free(data);
}

/* Con datos del juego: el punto WP_SUPPORT de cada arma de fuego cae sobre su geometría real (la mano de apoyo soldada agarra el arma, no el aire), con la palma hacia arriba y la mano a lo largo del cañón. */
static void test_real_support(const char *path, const char *stage) {
    struct stat sb;
    if (!path || stat(path, &sb) != 0) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t n = (size_t)sb.st_size;
    uint8_t *data = malloc(n + 1);
    if (!data || fread(data, 1, n, f) != n) { fclose(f); free(data); return; }
    fclose(f);
    static const struct { uint16_t id; float max_mm; } W[] = {{0x21b6, 10}, {0x4725, 10}, {0x2d3b, 10}, {0x57f8, 10}, {0x15a9, 10}};      /* pistola, FAMAS, PSG1, Stinger, Nikita: WP_SUPPORT a <= 10 mm de un vértice del modelo real */
    for (unsigned k = 0; k < sizeof W / sizeof W[0]; k++) {
        WLoadOpts o; wload_default_opts(&o); o.resident = stage; o.upscale = 2;
        WLoaded m;
        if (wload_model(data, n, stage, W[k].id, &o, &m)) { printf("  (apoyo real %04x: no se carga el modelo)\n", W[k].id); continue; }
        const WCutModel *cm = wcut_find(W[k].id);
        WCutSet cs;
        const WeaponDef *def = NULL;
        int r = cm ? wcut_run(&m, cm, &cs) : -1;
        for (int i = 0; r == 0 && i < w_registry_count; i++) if (cm->weapon && !strcmp(w_registry[i]->id, cm->weapon)) def = w_registry[i];
        if (r || !def) { printf("  (apoyo real %04x: wcut_run=%d)\n", W[k].id, r); wload_free(&m); continue; }
        WeaponState *st = calloc(1, def->state_size);
        st->def = def; def->init(st);
        weapon_bind(st, cs.meshes, cs.nmeshes, cs.parts, cs.nparts);
        int np; weapon_parts(st, &np);
        WPose po[64]; WeaponState tmp = *st; memset(tmp.channel, 0, sizeof tmp.channel);
        const int cnt = wcore_part_poses(&tmp, wpose_identity(), po, 64);
        int nm;
        const WMesh *ms = weapon_meshes(st, &nm);
        const WVec3 sp = def->points[WP_SUPPORT].p;
        float dmin = 1e9f;
        for (int i = 0; i < cnt && i < np; i++) {
            const int mi = weapon_part_mesh(st, i);
            if (mi < 0 || mi >= nm) continue;
            for (uint32_t v = 0; v < ms[mi].nv; v++) {
                const float d = dist(wpose_apply(po[i], wv(ms[mi].v[v].pos[0], ms[mi].v[v].pos[1], ms[mi].v[v].pos[2])), sp);
                if (d < dmin) dmin = d;
            }
        }
        const VrGripEntry *e = vrgrip_find(def->game_weapon_id);
        printf("  apoyo real %04x (%s): WP_SUPPORT (%.3f, %.3f, %.3f) a %.1f mm de la geometría\n", W[k].id, def->id, sp.x, sp.y, sp.z, dmin * 1000.0f);
        CHECK(e && e->support_pose >= 0, "%s: tiene mano de apoyo en la tabla", def->id);
        CHECK(dmin * 1000.0f < W[k].max_mm, "%s: WP_SUPPORT a %.1f mm de la geometría real (max %.0f)", def->id, dmin * 1000.0f, (double)W[k].max_mm);
        free(st); wcut_free(&cs); wload_free(&m);
    }
    free(data);
}

/* Colocación del objeto del juego en la mano (granadas 2, 7, 8; vr_grip_obj.inc): la relación con la que se ajustaron las poses (gripfit_gen.c / test_gripfit.c: p_obj = Hm * (p_mano + dR) * 0.001 contra la malla
 * wload en marco HELD girado 180 grados sobre Z, HELD = (x, -z, y) del motor) escrita aquí por separado de la tabla. Los demás ids no tienen desplazamiento y no se tocan los argumentos. */
static void test_object_offset(void) {
    static const float Hm[9] = {-1, 0, 0, 0, 0, 1, 0, 1, 0}, dR[3] = {-47.7f, 54.8f, 15.4f};
    const int with[3] = {2, 7, 8}, without[] = {0, 1, 3, 4, 5, 6, 9, -1, 99};
    for (int g = 0; g < 3; g++) {
        float R[9], t[3];
        CHECK(vrgrip_object_offset(with[g], R, t) == 1, "granada %d: tiene desplazamiento de objeto", with[g]);
        const float det = R[0] * (R[4] * R[8] - R[5] * R[7]) - R[1] * (R[3] * R[8] - R[5] * R[6]) + R[2] * (R[3] * R[7] - R[4] * R[6]);
        float oc = 0; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { float d = 0; for (int k = 0; k < 3; k++) d += R[3 * i + k] * R[3 * j + k]; oc += fabsf(d - (i == j)); }
        CHECK(fabsf(det - 1.0f) < 1e-5f && oc < 1e-5f, "granada %d: R es un giro (det %.5f, ortonormalidad %.2g)", with[g], det, oc);
        float worst = 0;
        for (int k = 0; k < 20; k++) {
            const float e[3] = {-100.0f + 15.0f * k, 70.0f - 9.0f * k, -120.0f + 13.0f * k};                     /* punto del modelo (mm del motor) */
            float p[3], q[3], qd[3];
            for (int r = 0; r < 3; r++) p[r] = R[3 * r] * e[0] + R[3 * r + 1] * e[1] + R[3 * r + 2] * e[2] + t[r];   /* en la mano (mm) */
            const float held[3] = {e[0], -e[2], e[1]}, rot[3] = {-held[0], -held[1], held[2]};                  /* malla de test_gripfit (m * 1000) */
            for (int r = 0; r < 3; r++) q[r] = Hm[3 * r] * (p[0] + dR[0]) + Hm[3 * r + 1] * (p[1] + dR[1]) + Hm[3 * r + 2] * (p[2] + dR[2]);
            for (int r = 0; r < 3; r++) qd[r] = q[r] - rot[r];
            worst = fmaxf(worst, sqrtf(qd[0] * qd[0] + qd[1] * qd[1] + qd[2] * qd[2]));
        }
        CHECK(worst < 1e-3f, "granada %d: la tabla es la relación mano-objeto del ajuste (error %.5f mm)", with[g], worst);
    }
    for (unsigned i = 0; i < sizeof without / sizeof without[0]; i++) {
        float R[9], t[3]; for (int k = 0; k < 9; k++) R[k] = 7.0f; for (int k = 0; k < 3; k++) t[k] = 7.0f;
        CHECK(vrgrip_object_offset(without[i], R, t) == 0 && R[0] == 7.0f && R[8] == 7.0f && t[0] == 7.0f && t[2] == 7.0f, "id %d: sin desplazamiento y sin tocar los argumentos", without[i]);
    }
    CHECK(vrgrip_object_offset(2, NULL, NULL) == 1 && vrgrip_object_offset(0, NULL, NULL) == 0, "argumentos nulos");
}

int main(int argc, char **argv) {
    test_proxy();
    test_table();
    test_object_offset();
    test_real(argc > 1 ? argv[1] : NULL, argc > 2 ? argv[2] : "init");
    test_real_support(argc > 1 ? argv[1] : NULL, argc > 2 ? argv[2] : "init");
    printf("%s vr_grip: %d comprobaciones, %d fallos\n", fails ? "FALLO" : "OK", checks, fails);
    return fails ? 1 : 0;
}
