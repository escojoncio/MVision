/* Prueba de ns_sink: piezas dinámicas hundidas bajo un suelo con sesgo de orden negativo (pies) -> adelanto de orden por pieza. make -f vr/native/extract.mk test-sink */
#include "ns_extract.h"
#include "ns_gpu.h"
#include "ns_sink.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLA %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { NsVert v[8]; NsNorm n[8]; NsFace f[6]; NsMesh m; } Box;
/* caja de semiejes (sx, sy, sz) centrada en el origen local */
static void make_box(Box *b, int sx, int sy, int sz, int bias, uint64_t salt) {
    memset(b, 0, sizeof *b);
    int i = 0;
    for (int ix = 0; ix < 2; ix++) for (int iy = 0; iy < 2; iy++) for (int iz = 0; iz < 2; iz++) {
        b->v[i].x = (int16_t)(ix ? sx : -sx); b->v[i].y = (int16_t)(iy ? sy : -sy); b->v[i].z = (int16_t)(iz ? sz : -sz); b->v[i].weld = 0xFF; i++;
    }
    static const uint8_t quads[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
    for (int f = 0; f < 6; f++) for (int k = 0; k < 4; k++) { b->f[f].v[k] = quads[f][k]; b->f[f].n[k] = quads[f][k]; }
    b->m.n_verts = 8; b->m.n_norms = 8; b->m.n_faces = 6; b->m.verts = b->v; b->m.norms = b->n; b->m.faces = b->f;
    b->m.depth_bias = (int16_t)bias;
    b->m.id = ns_mesh_content_hash(&b->m) ^ salt;
}
/* instancia con su primera pieza en (x, y, z) con la malla dada; `np` piezas en total (las demás sin malla: un cuerpo articulado de prueba lleva NS_SINK_MIN_PIECES) */
static void put(NsScene *sc, uint32_t inst, uint32_t piece, uint32_t np, uint64_t mesh, float x, float y, float z, int flags) {
    NsInstance *in = &sc->inst[inst];
    memset(in, 0, sizeof *in);
    in->key = 0x80100000u + 0x1000u * inst; in->first_piece = piece; in->n_pieces = (uint16_t)np; in->light = -1; in->flags = (uint16_t)flags;
    for (uint32_t k = 0; k < np; k++) {
        NsPiece *p = &sc->piece[piece + k];
        memset(p, 0, sizeof *p);
        float m[12] = {1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z};
        memcpy(p->world.m, m, sizeof m); p->mesh_id = k == 0 ? mesh : 0; p->parent = -1;
    }
}
#define BODY NS_SINK_MIN_PIECES
#define PC(i) (1u + ((i) - 1u) * BODY)     /* primera pieza de la instancia dinámica i (1..) */

int main(void) {
    printf("test_ns_sink: pies hundidos bajo un suelo que el motor ordena por detrás\n");
    NsScene sc; memset(&sc, 0, sizeof sc);
    ns_scene_alloc_exact(&sc, 8, 64, 2, 2);
    NsMeshStore *st = ns_store_create();
    Box floor_b, floor0, foot, wall;
    make_box(&floor_b, 3000, 50, 3000, -750, 1);   /* losa de suelo: cara superior en y = +50 local; sesgo -750 */
    make_box(&floor0, 3000, 50, 3000, 0, 2);       /* misma losa sin sesgo */
    make_box(&foot, 60, 100, 120, 0, 3);           /* «bota»: 20 cm de alto */
    make_box(&wall, 50, 2000, 3000, -750, 4);      /* pared con sesgo negativo: no es un suelo (caras verticales), salvo su tapa */
    ns_store_put(st, &floor_b.m); ns_store_put(st, &floor0.m); ns_store_put(st, &foot.m); ns_store_put(st, &wall.m);
    sc.units_per_m = 1000.0f;
    sc.cam.eye[0] = 0; sc.cam.eye[1] = 4000; sc.cam.eye[2] = -4000;   /* ojo 3 m por encima del suelo, a 4 m */
    /* 0: suelo con sesgo, cara superior en y = 1000 */
    put(&sc, 0, 0, 1, floor_b.m.id, 0, 950, 0, NS_IF_STATIC);
    /* 1: bota hundida 0,2 m (su suela en y = 800, su parte alta en y = 1000): dentro de la planta del suelo */
    put(&sc, 1, PC(1), BODY, foot.m.id, 100, 900, 200, 0);
    /* 2: bota por encima del suelo (suela en y = 1000) */
    put(&sc, 2, PC(2), BODY, foot.m.id, 600, 1100, 200, 0);
    /* 3: objeto muy por debajo (1,5 m bajo la cara: más que el sesgo): es de la planta de abajo, no se toca */
    put(&sc, 3, PC(3), BODY, foot.m.id, -600, -600, 200, 0);
    /* 4: bota hundida pero fuera de la planta del suelo */
    put(&sc, 4, PC(4), BODY, foot.m.id, 5000, 900, 200, 0);
    /* 5: puerta (una pieza) hundida igual que la bota; 6: objeto rígido de varias piezas hundido: no son cuerpos articulados */
    put(&sc, 5, PC(5), 1, foot.m.id, -100, 900, -200, 0);
    put(&sc, 6, PC(5) + 1, BODY, foot.m.id, -400, 900, -200, NS_IF_RIGID);
    sc.n_inst = 7; sc.n_piece = PC(5) + 1 + BODY;
    NsSinkStats ss;
    int n = ns_sink_apply(&sc, st, &ss);
    CHECK(n == 1 && ss.pieces_lifted == 1 && ss.floor_tris >= 2, "una pieza adelantada (%d), suelos %u", n, ss.floor_tris);
    float lift = ns_sink_lift_of(sc.piece[PC(1)].flags);
    {   /* lo justo: hundimiento / sen(elevación) + margen, con el sesgo del motor como tope */
        float dist = sqrtf(100.0f * 100.0f + 3100.0f * 3100.0f + 4200.0f * 4200.0f), sn = 3000.0f / dist, want = 200.0f / sn + 3.0f * NS_LIFT_UNIT;
        CHECK(fabsf(ss.max_sink - 200.0f) < 1.0f && lift >= want && lift <= want + NS_LIFT_UNIT && lift <= 750.0f, "bota hundida %.0f uds: adelanto %.0f (esperado %.0f..%.0f, tope 750)", ss.max_sink, lift, want, want + NS_LIFT_UNIT);
    }
    CHECK(ns_sink_lift_of(sc.piece[PC(2)].flags) == 0 && ns_sink_lift_of(sc.piece[PC(3)].flags) == 0 && ns_sink_lift_of(sc.piece[PC(4)].flags) == 0 && ns_sink_lift_of(sc.piece[0].flags) == 0,
          "ni lo que está encima, ni lo de la planta de abajo, ni lo de fuera de la planta, ni el escenario");
    CHECK(ns_sink_lift_of(sc.piece[PC(5)].flags) == 0 && ns_sink_lift_of(sc.piece[PC(5) + 1].flags) == 0, "una puerta (una pieza) o un objeto rígido hundidos no se adelantan: solo cuerpos articulados");
    CHECK(!(sc.piece[PC(1)].flags & NS_PF_HIDDEN), "no toca las demás marcas");
    /* llega al renderer como raise (unidades de 0,05) y no pisa un sesgo propio mayor */
    NsGpuPiece gp;
    ns_gpu_make_piece(&sc, &sc.inst[1], &sc.piece[PC(1)], 0, 0, 0, -1, 0, &gp);
    CHECK(fabsf(gp.raise * NS_GPU_RAISE_UNIT - lift) < 0.01f, "raise = adelanto / %.2f (%.1f)", (double)NS_GPU_RAISE_UNIT, gp.raise);
    ns_gpu_make_piece(&sc, &sc.inst[1], &sc.piece[PC(1)], 0, 30000, 0, -1, 0, &gp);
    CHECK(gp.raise == 30000.0f, "un sesgo propio mayor se conserva");
    ns_gpu_make_piece(&sc, &sc.inst[2], &sc.piece[PC(2)], 0, 0, 0, -1, 0, &gp);
    CHECK(gp.raise == 0.0f, "pieza sin adelanto: raise 0");
    /* hundida más de NS_SINK_MAX (aunque dentro del sesgo del motor): es otra planta, no un pie */
    sc.piece[PC(1)].world.m[7] = 900 - 400;      /* suela a 0,6 m bajo la cara: toda la bota por debajo de 0,4 m */
    ns_sink_apply(&sc, st, NULL);
    CHECK(ns_sink_lift_of(sc.piece[PC(1)].flags) > 0 && ns_sink_lift_of(sc.piece[PC(1)].flags) < lift + 400.0f, "hundida 0,4-0,6 m: solo cuenta la parte a menos de NS_SINK_MAX (%.0f)", ns_sink_lift_of(sc.piece[PC(1)].flags));
    sc.piece[PC(1)].world.m[7] = 900 - 700;      /* toda la bota a más de 0,6 m bajo la cara */
    ns_sink_apply(&sc, st, NULL);
    CHECK(ns_sink_lift_of(sc.piece[PC(1)].flags) == 0, "a más de NS_SINK_MAX bajo el suelo: sin adelanto");
    sc.piece[PC(1)].world.m[7] = 900;
    ns_sink_apply(&sc, st, NULL);
    /* repetir no acumula; la pieza que deja de estar hundida pierde el adelanto */
    ns_sink_apply(&sc, st, NULL);
    CHECK(ns_sink_lift_of(sc.piece[PC(1)].flags) == lift, "idempotente");
    sc.piece[PC(1)].world.m[7] = 1200;
    ns_sink_apply(&sc, st, NULL);
    CHECK(ns_sink_lift_of(sc.piece[PC(1)].flags) == 0, "al salir del suelo el adelanto se quita");
    sc.piece[PC(1)].world.m[7] = 900;
    /* visto desde debajo del suelo: nada */
    sc.cam.eye[1] = 500;
    CHECK(ns_sink_apply(&sc, st, NULL) == 0, "ojo por debajo del suelo: sin adelanto");
    sc.cam.eye[1] = 4000;
    /* pegado al ojo (cuerpo propio): nunca se acerca a menos de NS_SINK_EYE_KEEP */
    sc.cam.eye[0] = 100; sc.cam.eye[1] = 1150; sc.cam.eye[2] = 200;
    CHECK(ns_sink_apply(&sc, st, NULL) == 0, "pieza a menos de 0,3 m del ojo: sin adelanto");
    sc.cam.eye[0] = 100; sc.cam.eye[1] = 1500; sc.cam.eye[2] = 200;   /* 0,6 m encima */
    ns_sink_apply(&sc, st, NULL);
    CHECK(ns_sink_lift_of(sc.piece[PC(1)].flags) > 0 && ns_sink_lift_of(sc.piece[PC(1)].flags) <= 600.0f - NS_SINK_EYE_KEEP + NS_LIFT_UNIT, "cerca del ojo el adelanto deja 0,3 m (%.0f)", ns_sink_lift_of(sc.piece[PC(1)].flags));
    sc.cam.eye[0] = 0; sc.cam.eye[1] = 4000; sc.cam.eye[2] = -4000;
    /* a ras de suelo el adelanto se acota por el sesgo del motor */
    sc.cam.eye[1] = 1010; sc.cam.eye[2] = -9000;
    ns_sink_apply(&sc, st, NULL);
    CHECK(ns_sink_lift_of(sc.piece[PC(1)].flags) <= 750.0f + NS_LIFT_UNIT && ns_sink_lift_of(sc.piece[PC(1)].flags) >= 700.0f, "a ras de suelo: tope = sesgo del motor (%.0f)", ns_sink_lift_of(sc.piece[PC(1)].flags));
    sc.cam.eye[1] = 4000; sc.cam.eye[2] = -4000;
    /* pieza oculta: no se toca */
    sc.piece[PC(1)].flags |= NS_PF_HIDDEN;
    CHECK(ns_sink_apply(&sc, st, NULL) == 0, "pieza oculta");
    sc.piece[PC(1)].flags &= (uint16_t)~NS_PF_HIDDEN;
    /* suelo SIN sesgo negativo: el motor no lo ordena por detrás -> el búfer de profundidad manda */
    put(&sc, 0, 0, 1, floor0.m.id, 0, 950, 0, NS_IF_STATIC);
    CHECK(ns_sink_apply(&sc, st, &ss) == 0 && ss.floor_tris == 0, "suelo sin sesgo: sin adelanto");
    /* una pared con sesgo negativo no es un suelo: solo cuentan las caras casi horizontales (su tapa y su base) */
    put(&sc, 0, 0, 1, wall.m.id, 0, 0, 0, NS_IF_STATIC);
    ns_sink_apply(&sc, st, &ss);
    CHECK(ss.floor_tris == 4, "pared: solo su tapa y su base (%u triángulos)", ss.floor_tris);
    /* el escenario cambia (otra zona): los suelos se rehacen */
    put(&sc, 0, 0, 1, floor_b.m.id, 0, 950, 0, NS_IF_STATIC);
    CHECK(ns_sink_apply(&sc, st, &ss) == 1 && ss.floor_tris >= 2, "cambio de escenario: suelos rehechos");
    CHECK(ns_sink_apply(NULL, st, NULL) == 0 && ns_sink_apply(&sc, NULL, NULL) == 0, "entradas nulas");
    ns_scene_free(&sc);
    printf("%d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
