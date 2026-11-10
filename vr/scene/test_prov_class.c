/* Prueba sintética de vr_prov (clasificación de primitivas 2D por procedencia, sin datos del juego). make -C vr/scene test */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_prov.h"
#include "vr_scene.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FALLO %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static GpuOp ops[256];
static GpuDrawList dl;
static ProvEntry tab[64];

/* ids de la tabla sintética */
enum { I_CINEMA = 1, I_MOTSE, I_TELOP, I_FAMA, I_SCOPE, I_SIGHT, I_MENUMAN, I_CB, I_OTHER, I_EFFECT, I_SCOPE_FN, I_CINEMA_FN, I_WIN, I_X, I_SORT, I_SORT2, I_GAS, I_FADE, I_LAST };

static void set_e(int id, const char *name, uint32_t fn) {
    tab[id].id = (uint16_t)id; tab[id].fn = fn; tab[id].actor_node = name && name[0] ? 0x800be4e0u : 0;
    memset(tab[id].name, 0, sizeof tab[id].name);
    if (name) strncpy(tab[id].name, name, sizeof tab[id].name - 1);
}
static void reset(void) {
    memset(ops, 0, sizeof ops); memset(&dl, 0, sizeof dl); memset(tab, 0, sizeof tab);
    dl.ops = ops; dl.disp.x = 0; dl.disp.y = 0; dl.disp.w = 320; dl.disp.h = 240;
    set_e(I_CINEMA, "cinema.c", 0x800e1050u); set_e(I_MOTSE, "motse.c", 0x80050000u); set_e(I_TELOP, "telop.c", 0x80051000u); set_e(I_FAMA, "famaslit.c", 0x80052000u);
    set_e(I_SCOPE, "scope.c", 0x80063d54u); set_e(I_SIGHT, "sight.c", 0x800726bcu); set_e(I_MENUMAN, "menuman.c", 0x800394f8u);
    set_e(I_CB, "", 0x8003dc5cu);           /* callback del HUD de la tabla de menuman */
    set_e(I_OTHER, "", 0x80012345u);        /* función cualquiera */
    set_e(I_EFFECT, "o2_damge.c", 0x80060000u);
    set_e(I_SCOPE_FN, "", 0x80063d54u);     /* scope.c reconocido solo por la dirección de su rutina */
    set_e(I_CINEMA_FN, "", 0x800e1050u);
    set_e(I_WIN, "", 0x8003c360u);          /* callback de la ventana del selector (de la tabla) */
    set_e(I_X, "", 0x80099999u);
    set_e(I_SORT, "", 0x8001da18u);         /* etapa de ordenación del canal principal: encadena paquetes que escribieron otros */
    set_e(I_SORT2, "", 0x80018304u);        /* ordenación de canales (llama a las etapas) */
    set_e(I_GAS, "gas_efct.c", 0x800de1f0u);   /* anillo del gas: efecto de pantalla con paquetes propios */
    set_e(I_FADE, "fadeio.c", 0x800e15e8u);    /* fundido */
    dl.prov = tab; dl.nprov = I_LAST;
}
static GpuOp *add(int type, int prov) { GpuOp *o = &ops[dl.nops++]; o->type = (uint8_t)type; o->prov = (uint16_t)prov; return o; }
static GpuOp *rect(int prov, int x, int y, int w, int h, uint16_t flags) {
    GpuOp *o = add(GPU_OP_RECT, prov);
    o->v[0].x = (int16_t)x; o->v[0].y = (int16_t)y; o->rw = (int16_t)w; o->rh = (int16_t)h; o->flags = flags;
    o->cx1 = 0; o->cy1 = 0; o->cx2 = 319; o->cy2 = 255;
    return o;
}
static int cls_of(const VrProvLearn *S, const GpuOp *o) { return vpc_class_op(S, &dl, o); }

static void test_rules(void) {
    reset();
    int ids[] = {I_CINEMA, I_MOTSE, I_TELOP, I_FAMA, I_SCOPE, I_SIGHT, I_MENUMAN, I_CB, I_OTHER, I_EFFECT, I_SCOPE_FN, I_CINEMA_FN, I_GAS, I_FADE};
    int unt[] = {VPC_FX_LETTERBOX, VPC_FX_VIGNETTE, VPC_FULL, VPC_FULL, VPC_SCOPE_FRAME, VPC_SCOPE_FRAME, VPC_HUD_WORLD, VPC_HUD_WORLD, VPC_UNKNOWN, VPC_UNKNOWN, VPC_SCOPE_FRAME, VPC_FX_LETTERBOX, VPC_FX_SCREEN, VPC_FX_SCREEN};
    int tex[] = {VPC_FX_LETTERBOX, VPC_FX_VIGNETTE, VPC_FULL, VPC_FULL, VPC_SCOPE_TEXT, VPC_SCOPE_TEXT, VPC_HUD_WORLD, VPC_HUD_WORLD, VPC_UNKNOWN, VPC_UNKNOWN, VPC_SCOPE_TEXT, VPC_FX_LETTERBOX, VPC_FX_SCREEN, VPC_FX_SCREEN};
    for (size_t k = 0; k < sizeof ids / sizeof *ids; k++) {
        GpuOp *a = rect(ids[k], 0, 0, 8, 8, 0), *b = rect(ids[k], 0, 0, 8, 8, GPU_F_TEXTURED);
        CHECK(cls_of(NULL, a) == unt[k], "regla id %d sin textura: %s (esperado %s)", ids[k], vpc_class_name(cls_of(NULL, a)), vpc_class_name(unt[k]));
        CHECK(cls_of(NULL, b) == tex[k], "regla id %d texturado: %s (esperado %s)", ids[k], vpc_class_name(cls_of(NULL, b)), vpc_class_name(tex[k]));
    }
    GpuOp *z = rect(0, 0, 0, 8, 8, 0);
    CHECK(cls_of(NULL, z) == VPC_UNKNOWN, "sin procedencia: UNKNOWN");
    z->prov2 = I_CINEMA;
    CHECK(cls_of(NULL, z) == VPC_FX_LETTERBOX, "prov == 0 usa prov2");
    GpuOp *hi = rect(I_LAST + 5, 0, 0, 8, 8, 0);
    CHECK(cls_of(NULL, hi) == VPC_UNKNOWN, "id fuera de la tabla: UNKNOWN");
    GpuDrawList nol = dl; nol.prov = NULL; nol.nprov = 0;
    CHECK(vpc_class_op(NULL, &nol, &ops[0]) == VPC_UNKNOWN, "lista sin tabla: UNKNOWN");
    CHECK(vpc_class_op(NULL, NULL, &ops[0]) == VPC_UNKNOWN, "sin lista: UNKNOWN");
    CHECK(vpc_layer_of(VPC_HUD_WORLD) == VPC_L_HUD && vpc_layer_of(VPC_SCOPE_TEXT) == VPC_L_HUD && vpc_layer_of(VPC_UNKNOWN) == VPC_L_HUD, "HUD_WORLD, SCOPE_TEXT y UNKNOWN van a la capa HUD");
    CHECK(vpc_layer_of(VPC_WINDOW) == VPC_L_WINDOW && vpc_layer_of(VPC_FULL) == VPC_L_FULL, "WINDOW y FULL tienen su capa");
    CHECK(vpc_layer_of(VPC_FX_VIGNETTE) == -1 && vpc_layer_of(VPC_FX_LETTERBOX) == -1 && vpc_layer_of(VPC_SCOPE_FRAME) == -1 && vpc_layer_of(99) == -1, "FX_* y SCOPE_FRAME no van al 2D");
}

/* una lista con los ids dados (una primitiva por id) */
static void list_of(const int *ids, int n) {
    dl.nops = 0; memset(ops, 0, sizeof ops);
    for (int i = 0; i < n; i++) rect(ids[i], 4 * i, 100, 3, 3, 0);
}

static void test_window_learning(void) {
    reset();
    VrProvLearn S; vpc_learn_reset(&S);
    const int closed[] = {I_MENUMAN, I_CB}, open[] = {I_MENUMAN, I_CB, I_WIN};
    /* 40 listas con la ventana cerrada: menuman y el callback emiten siempre; el id X emite una vez (lista 5) */
    for (int k = 0; k < 40; k++) {
        if (k == 5) { const int c2[] = {I_MENUMAN, I_CB, I_X}; list_of(c2, 3); } else list_of(closed, 2);
        vpc_learn_note(&S, &dl, VPC_ST_CLOSED);
    }
    GpuOp w; memset(&w, 0, sizeof w); w.type = GPU_OP_RECT; w.prov = I_WIN;
    GpuOp x = w; x.prov = I_X;
    GpuOp m = w; m.prov = I_MENUMAN;
    GpuOp c = w; c.prov = I_CB;
    CHECK(cls_of(&S, &w) == VPC_HUD_WORLD, "antes de abrir, el callback de la ventana (de la tabla de menuman) es HUD_WORLD por defecto");
    list_of(open, 3); vpc_learn_note(&S, &dl, VPC_ST_OPEN);
    CHECK(cls_of(&S, &w) == VPC_WINDOW, "el id que solo emite con la ventana abierta pasa a WINDOW (%s)", vpc_class_name(cls_of(&S, &w)));
    CHECK(cls_of(&S, &m) == VPC_HUD_WORLD && cls_of(&S, &c) == VPC_HUD_WORLD, "lo que emite siempre sigue siendo HUD_WORLD");
    CHECK(cls_of(&S, &x) == VPC_UNKNOWN, "el id sin emitir con la ventana abierta no cambia (UNKNOWN)");
    /* otra vista: no se aprende ni se cuenta */
    { uint32_t n0 = S.nclosed, t0 = S.tick; list_of(open, 3); vpc_learn_note(&S, &dl, VPC_ST_OTHER); CHECK(S.nclosed == n0 && S.tick == t0, "vista distinta del mundo: la lista no se anota"); }
    /* el id emite con la ventana abierta tras >= 30 listas sin emitir (X emitió en la lista 5 de 40): se aprende */
    { const int o2[] = {I_MENUMAN, I_CB, I_X}; list_of(o2, 3); vpc_learn_note(&S, &dl, VPC_ST_OPEN); CHECK(cls_of(&S, &x) == VPC_WINDOW, "X llevaba 35 listas cerradas sin emitir: WINDOW"); }
    /* el cierre con animación (pocas listas con el id emitiendo con la ventana cerrada) no descarta */
    for (int k = 0; k < VPC_LEAK_LISTS - 1; k++) { list_of(open, 3); vpc_learn_note(&S, &dl, VPC_ST_CLOSED); }
    CHECK(cls_of(&S, &w) == VPC_WINDOW, "%d listas de cierre con animación no descartan", VPC_LEAK_LISTS - 1);
    /* ... pero emitir de forma sostenida con la ventana cerrada era HUD: se descarta para siempre */
    list_of(open, 3); vpc_learn_note(&S, &dl, VPC_ST_CLOSED);
    CHECK(cls_of(&S, &w) == VPC_HUD_WORLD && S.n_demoted >= 1, "emisión sostenida con la ventana cerrada: descartado (HUD_WORLD)");
    for (int k = 0; k < 40; k++) { list_of(closed, 2); vpc_learn_note(&S, &dl, VPC_ST_CLOSED); }
    list_of(open, 3); vpc_learn_note(&S, &dl, VPC_ST_OPEN);
    CHECK(cls_of(&S, &w) == VPC_HUD_WORLD, "un id descartado no se vuelve a aprender");
    /* se conserva la sesión: X sigue siendo WINDOW tras cerrar y reabrir */
    CHECK(cls_of(&S, &x) == VPC_WINDOW, "lo aprendido se conserva toda la sesión");

    /* sin observación suficiente (abre antes de VPC_QUIET_LISTS listas de mundo): no se aprende nada */
    vpc_learn_reset(&S);
    for (int k = 0; k < VPC_QUIET_LISTS - 1; k++) { list_of(closed, 2); vpc_learn_note(&S, &dl, VPC_ST_CLOSED); }
    list_of(open, 3); vpc_learn_note(&S, &dl, VPC_ST_OPEN);
    CHECK(cls_of(&S, &w) == VPC_HUD_WORLD, "con menos de %d listas de mundo vistas no se aprende", VPC_QUIET_LISTS);
    list_of(closed, 2); vpc_learn_note(&S, &dl, VPC_ST_CLOSED);
    list_of(open, 3); vpc_learn_note(&S, &dl, VPC_ST_OPEN);
    CHECK(cls_of(&S, &w) == VPC_WINDOW, "con %d listas vistas y el id nunca emitido: WINDOW", VPC_QUIET_LISTS);

    /* las clases FX / FULL / SCOPE nunca se aprenden como ventana */
    vpc_learn_reset(&S);
    for (int k = 0; k < 40; k++) { list_of(closed, 2); vpc_learn_note(&S, &dl, VPC_ST_CLOSED); }
    { const int fx[] = {I_MENUMAN, I_CINEMA, I_SCOPE, I_TELOP, I_MOTSE, I_OTHER}; list_of(fx, 6); vpc_learn_note(&S, &dl, VPC_ST_OPEN);
      CHECK(cls_of(&S, &ops[1]) == VPC_FX_LETTERBOX && cls_of(&S, &ops[2]) == VPC_SCOPE_FRAME && cls_of(&S, &ops[3]) == VPC_FULL && cls_of(&S, &ops[4]) == VPC_FX_VIGNETTE, "FX / FULL / SCOPE no se aprenden como ventana");
      CHECK(cls_of(&S, &ops[5]) == VPC_WINDOW, "un id desconocido que solo emite con la ventana abierta también se aprende"); }
    /* NULL y listas sin tabla: sin fallos */
    vpc_learn_note(NULL, &dl, VPC_ST_OPEN); vpc_learn_note(&S, NULL, VPC_ST_OPEN);
    { GpuDrawList nol = dl; nol.prov = NULL; nol.nprov = 0; vpc_learn_note(&S, &nol, VPC_ST_OPEN); vpc_learn_note(&S, &nol, VPC_ST_CLOSED); }
    CHECK(1, "ok");
}

/* Paquete encadenado por una rutina de ordenación (prov = ordenación): la procedencia es la de quien lo escribió (prov2); sin escritor con regla es del MUNDO (WORLD); con ámbito normal no cambia. */
static void test_chain(void) {
    reset();
    CHECK(vpc_is_chain(&tab[I_SORT]) && vpc_is_chain(&tab[I_SORT2]) && !vpc_is_chain(&tab[I_MENUMAN]) && !vpc_is_chain(&tab[I_OTHER]) && !vpc_is_chain(NULL), "vpc_is_chain: solo las rutinas de ordenación");
    GpuOp *a = rect(I_SORT, 0, 0, 320, 32, 0); a->prov2 = I_CINEMA;
    CHECK(cls_of(NULL, a) == VPC_FX_LETTERBOX, "encadenado por la ordenación, escritor cinema.c: FX_LETTERBOX (%s)", vpc_class_name(cls_of(NULL, a)));
    GpuOp *b = rect(I_SORT2, 0, 0, 8, 8, 0); b->prov2 = I_MENUMAN;
    CHECK(cls_of(NULL, b) == VPC_HUD_WORLD, "encadenado por 0x80018304, escritor menuman.c: HUD_WORLD (%s)", vpc_class_name(cls_of(NULL, b)));
    GpuOp *c = rect(I_SORT, 0, 0, 8, 8, GPU_F_TEXTURED); c->prov2 = I_SCOPE;
    CHECK(cls_of(NULL, c) == VPC_SCOPE_TEXT, "escritor scope.c texturado: SCOPE_TEXT (%s)", vpc_class_name(cls_of(NULL, c)));
    GpuOp *d = rect(I_SORT, 0, 0, 8, 8, 0);
    CHECK(cls_of(NULL, d) == VPC_WORLD, "ordenación sin escritor conocido: WORLD (%s)", vpc_class_name(cls_of(NULL, d)));
    GpuOp *e = rect(I_SORT, 0, 0, 8, 8, 0); e->prov2 = I_SORT2;
    CHECK(cls_of(NULL, e) == VPC_WORLD, "escritor que también es una rutina de ordenación: WORLD (%s)", vpc_class_name(cls_of(NULL, e)));
    GpuOp *f = rect(I_SORT, 0, 0, 8, 8, 0); f->prov2 = I_OTHER;
    CHECK(cls_of(NULL, f) == VPC_WORLD, "escritor de otra función (sin regla): WORLD (%s)", vpc_class_name(cls_of(NULL, f)));
    GpuOp *g = rect(I_MENUMAN, 0, 0, 8, 8, 0); g->prov2 = I_CINEMA;
    CHECK(cls_of(NULL, g) == VPC_HUD_WORLD, "ámbito normal: manda quien encadenó, prov2 no cambia nada (%s)", vpc_class_name(cls_of(NULL, g)));
    GpuOp *h = rect(0, 0, 0, 8, 8, 0); h->prov2 = I_CINEMA;
    CHECK(cls_of(NULL, h) == VPC_FX_LETTERBOX, "prov == 0 sigue usando prov2");
    GpuOp *k = rect(I_SORT, 0, 0, 8, 8, 0); k->prov2 = (uint16_t)(I_LAST + 5);
    CHECK(cls_of(NULL, k) == VPC_WORLD, "escritor fuera de la tabla: WORLD (%s)", vpc_class_name(cls_of(NULL, k)));
    GpuOp *m = rect(I_SORT, 0, 0, 8, 8, 0); m->prov2 = I_EFFECT;
    CHECK(cls_of(NULL, m) == VPC_WORLD, "escritor actor de efectos sin regla (o2_damge.c): WORLD (%s)", vpc_class_name(cls_of(NULL, m)));
    GpuOp *n = rect(I_SORT, 0, 0, 320, 240, GPU_F_SEMI); n->prov2 = I_FADE;
    CHECK(cls_of(NULL, n) == VPC_FX_SCREEN, "escritor fadeio.c: FX_SCREEN aunque lo encadene la ordenación (%s)", vpc_class_name(cls_of(NULL, n)));
    CHECK(vpc_layer_of(VPC_FX_SCREEN) == -1, "FX_SCREEN no tiene capa 2D");
    CHECK(vpc_layer_of(VPC_WORLD) == VPC_L_HUD, "WORLD que vr_scene no descartó (sin drop_world2d: selector / pausa): al HUD, como antes");
    CHECK(!strcmp(vpc_class_name(VPC_WORLD), "WORLD") && !strcmp(vpc_class_name(VPC_FX_SCREEN), "FX_SCREEN"), "nombres de las clases nuevas");
    { GpuDrawList nol = dl; nol.prov = NULL; nol.nprov = 0; CHECK(vpc_class_op(NULL, &nol, d) == VPC_UNKNOWN, "sin tabla de procedencia: UNKNOWN (HUD), como antes"); }

    /* aprendizaje de la ventana por escritor: el id de la ordenación no se aprende; el escritor que solo emite con la ventana abierta sí */
    reset();
    VrProvLearn S; vpc_learn_reset(&S);
    for (int i = 0; i < 40; i++) {
        dl.nops = 0; memset(ops, 0, sizeof ops);
        rect(I_SORT, 0, 100, 3, 3, 0)->prov2 = I_MENUMAN;
        rect(I_SORT, 4, 100, 3, 3, 0);   /* sin escritor */
        vpc_learn_note(&S, &dl, VPC_ST_CLOSED);
    }
    dl.nops = 0; memset(ops, 0, sizeof ops);
    rect(I_SORT, 0, 100, 3, 3, 0)->prov2 = I_MENUMAN;
    rect(I_SORT, 4, 100, 3, 3, 0)->prov2 = I_WIN;
    rect(I_SORT, 8, 100, 3, 3, 0);
    vpc_learn_note(&S, &dl, VPC_ST_OPEN);
    CHECK(cls_of(&S, &ops[0]) == VPC_HUD_WORLD, "escritor que emite siempre: HUD_WORLD (%s)", vpc_class_name(cls_of(&S, &ops[0])));
    CHECK(cls_of(&S, &ops[1]) == VPC_WINDOW, "escritor que solo emite con la ventana abierta: WINDOW, aunque lo encadene la ordenación (%s)", vpc_class_name(cls_of(&S, &ops[1])));
    CHECK(cls_of(&S, &ops[2]) == VPC_WORLD, "sin escritor no se aprende como ventana (%s)", vpc_class_name(cls_of(&S, &ops[2])));

    /* efectos: bandas de cinemática encadenadas por la ordenación */
    VrFx fx;
    reset();
    rect(I_SORT, 0, 0, 320, 32, 0)->prov2 = I_CINEMA; rect(I_SORT, 0, 208, 320, 32, 0)->prov2 = I_CINEMA;
    vpc_fx_extract(NULL, &dl, 0, &fx);
    CHECK(fabsf(fx.letterbox - 64.0f / 240.0f) < 1e-4f, "bandas de cinema.c encadenadas por la ordenación: letterbox %.4f", (double)fx.letterbox);
}

static void test_unknown_and_split(void) {
    reset();
    /* NSL1: ninguna primitiva con procedencia ni tabla */
    GpuDrawList nsl1 = dl; nsl1.prov = NULL; nsl1.nprov = 0;
    GpuOp nops[8]; memset(nops, 0, sizeof nops);
    for (int i = 0; i < 8; i++) { nops[i].type = (uint8_t)(i & 1 ? GPU_OP_POLY : GPU_OP_RECT); nops[i].nv = 3; }
    nsl1.ops = nops; nsl1.nops = 8;
    uint8_t cls[8], od[8] = {2, 2, 2, 2, 2, 2, 2, 2};
    for (int i = 0; i < 8; i++) cls[i] = vpc_op_class_cb(&nsl1, (uint32_t)i, NULL);
    uint32_t cnt[VPC_N], lc[VPC_L_N];
    uint8_t *out[VPC_L_N]; uint8_t b0[8], b1[8], b2[8]; out[0] = b0; out[1] = b1; out[2] = b2;
    vpc_tally(od, cls, 8, cnt);
    CHECK(cnt[VPC_UNKNOWN] == 8, "NSL1: todo UNKNOWN (%u)", cnt[VPC_UNKNOWN]);
    vpc_split_dest(od, cls, 8, out, lc);
    CHECK(lc[VPC_L_HUD] == 8 && lc[VPC_L_WINDOW] == 0 && lc[VPC_L_FULL] == 0, "NSL1: todo a la capa HUD, como antes (%u %u %u)", lc[0], lc[1], lc[2]);
    { int same = 1; for (int i = 0; i < 8; i++) same &= b0[i] == 2 && b1[i] == 0 && b2[i] == 0; CHECK(same, "NSL1: op_dest de la capa HUD = el de la lista"); }

    /* reparto por clase; op_dest 0 (3D / descartada) y 3 (filtro) no van a ninguna capa */
    uint8_t c2[8] = {VPC_HUD_WORLD, VPC_WINDOW, VPC_FULL, VPC_FX_VIGNETTE, VPC_FX_LETTERBOX, VPC_SCOPE_FRAME, VPC_SCOPE_TEXT, VPC_UNKNOWN};
    uint8_t d2[8] = {2, 2, 2, 2, 2, 2, 2, 2};
    vpc_split_dest(d2, c2, 8, out, lc);
    CHECK(lc[VPC_L_HUD] == 3 && lc[VPC_L_WINDOW] == 1 && lc[VPC_L_FULL] == 1, "reparto: HUD 3 (HUD_WORLD, SCOPE_TEXT, UNKNOWN), ventana 1, completa 1 (%u %u %u)", lc[0], lc[1], lc[2]);
    CHECK(b0[0] == 2 && b0[6] == 2 && b0[7] == 2 && b0[1] == 0 && b0[3] == 0 && b0[4] == 0 && b0[5] == 0 && b1[1] == 2 && b2[2] == 2, "reparto: cada primitiva en su capa; FX_* y SCOPE_FRAME en ninguna");
    uint8_t d3[8] = {0, 3, 2, 0, 3, 2, 2, 2};
    vpc_split_dest(d3, c2, 8, out, lc);
    CHECK(lc[VPC_L_HUD] == 2 && lc[VPC_L_WINDOW] == 0 && lc[VPC_L_FULL] == 1, "op_dest 0 y 3 no se reparten (%u %u %u)", lc[0], lc[1], lc[2]);
    vpc_split_dest(NULL, c2, 8, out, lc);
    CHECK(lc[0] == 0 && lc[1] == 0 && lc[2] == 0, "sin op_dest: nada");
    uint8_t c4[8] = {VPC_FX_SCREEN, VPC_WORLD, VPC_HUD_WORLD, VPC_FX_SCREEN, VPC_WORLD, VPC_UNKNOWN, VPC_WINDOW, VPC_FULL};
    vpc_split_dest(d2, c4, 8, out, lc);
    CHECK(lc[VPC_L_HUD] == 4 && lc[VPC_L_WINDOW] == 1 && lc[VPC_L_FULL] == 1 && b0[0] == 0 && b0[3] == 0 && b1[0] == 0 && b1[3] == 0 && b2[0] == 0 && b2[3] == 0 && b0[1] == 2 && b0[4] == 2,
          "FX_SCREEN (gas, fundido) no va a ninguna capa del panel; WORLD que llega al reparto, al HUD (%u %u %u)", lc[0], lc[1], lc[2]);
    vpc_tally(d2, c4, 8, cnt);
    CHECK(cnt[VPC_FX_SCREEN] == 2 && cnt[VPC_WORLD] == 2 && cnt[VPC_HUD_WORLD] == 1 && cnt[VPC_UNKNOWN] == 1, "recuento por clase con las clases nuevas");
}

/* vr_scene_build rellena op_cls con el gancho de VrSceneOpts */
static void test_scene_hook(void) {
    reset();
    rect(I_MENUMAN, 10, 10, 20, 8, 0);                    /* 0: HUD */
    rect(I_CINEMA, 0, 0, 320, 30, 0);                     /* 1: banda */
    rect(I_TELOP, 40, 100, 100, 12, GPU_F_TEXTURED);      /* 2: créditos */
    rect(0, 50, 50, 8, 8, 0);                             /* 3: sin procedencia */
    VrProvLearn S; vpc_learn_reset(&S);
    VrSceneOpts o = vr_scene_opts_default();
    o.drop_clear = 0;
    VrScene s;
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.op_dest && s.op_cls == NULL, "sin gancho: op_cls NULL");
    o.op_class = vpc_op_class_cb; o.op_class_ctx = &S;
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.op_dest && s.op_cls && s.n_op_dest == 4, "con gancho: op_cls");
    if (s.op_dest && s.op_cls) {
        CHECK(s.op_dest[0] == 2 && s.op_dest[1] == 2 && s.op_dest[2] == 2 && s.op_dest[3] == 2, "las 4 primitivas van al panel (%u %u %u %u)", s.op_dest[0], s.op_dest[1], s.op_dest[2], s.op_dest[3]);
        CHECK(s.op_cls[0] == VPC_HUD_WORLD && s.op_cls[1] == VPC_FX_LETTERBOX && s.op_cls[2] == VPC_FULL && s.op_cls[3] == VPC_UNKNOWN,
              "clases en vr_scene: %u %u %u %u", s.op_cls[0], s.op_cls[1], s.op_cls[2], s.op_cls[3]);
        uint8_t b0[4], b1[4], b2[4]; uint8_t *out[VPC_L_N] = {b0, b1, b2}; uint32_t lc[VPC_L_N];
        vpc_split_dest(s.op_dest, s.op_cls, s.n_op_dest, out, lc);
        CHECK(lc[VPC_L_HUD] == 2 && lc[VPC_L_FULL] == 1 && lc[VPC_L_WINDOW] == 0, "reparto de la escena: %u %u %u", lc[0], lc[1], lc[2]);
    }
    vr_scene_release();
}

/* Primitivas del MUNDO por procedencia (clase WORLD: las encadenó la ordenación del canal 3D y su escritor no tiene regla): con drop_world2d (el nativo dibuja el mundo y sus efectos) no van al
 * panel 2D (VR_FATE_WORLD2D), sean RECT (nieve), LINE (chaff) o POLY posteriores al último 3D. El HUD, los créditos, las bandas y la ventana no cambian, tampoco si los encadena la ordenación. */
static void test_scene_world(void) {
    reset();
    rect(I_SORT, 100, 60, 4, 4, 0);                                   /* 0: copo de nieve (sprite del canal 3D, sin escritor conocido) */
    { GpuOp *o = add(GPU_OP_LINE, I_SORT); o->v[0].x = 20; o->v[0].y = 30; o->v[1].x = 60; o->v[1].y = 44; o->v[0].r = o->v[1].r = 200; o->cx2 = 319; o->cy2 = 255; o->prov2 = I_EFFECT; }   /* 1: línea de un efecto */
    { GpuOp *o = add(GPU_OP_POLY, I_SORT2); o->nv = 3; o->v[0].x = 10; o->v[0].y = 10; o->v[1].x = 30; o->v[1].y = 10; o->v[2].x = 10; o->v[2].y = 30; o->cx2 = 319; o->cy2 = 255; }      /* 2: polígono sin vista, encadenado */
    rect(I_MENUMAN, 10, 200, 20, 8, 0);                               /* 3: HUD */
    rect(I_SORT, 40, 200, 20, 8, 0)->prov2 = I_MENUMAN;               /* 4: HUD encadenado por la ordenación */
    rect(I_SORT, 40, 100, 100, 12, GPU_F_TEXTURED)->prov2 = I_TELOP;  /* 5: créditos encadenados por la ordenación */
    rect(I_SORT, 0, 0, 320, 30, GPU_F_SEMI)->prov2 = I_CINEMA;        /* 6: banda de cinemática encadenada */
    rect(0, 50, 50, 8, 8, 0);                                         /* 7: sin procedencia */
    rect(I_GAS, 60, 60, 40, 40, GPU_F_SEMI);                          /* 8: gas_efct.c */
    rect(I_OTHER, 70, 150, 8, 8, 0);                                  /* 9: otra función, no encadenada */
    static uint8_t fate[16];
    vr_scene_set_fate(fate, sizeof fate);
    VrSceneOpts o = vr_scene_opts_default();
    o.drop_clear = 0; o.drop_world2d = 1; o.op_class = vpc_op_class_cb; o.op_class_ctx = NULL;
    VrScene s;
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.op_dest && s.op_cls && s.n_op_dest == 10, "escena con drop_world2d y gancho");
    if (s.op_dest && s.op_cls) {
        CHECK(s.op_dest[0] == 0 && fate[0] == VR_FATE_WORLD2D, "RECT encadenado por la ordenación: mundo, no panel (dest %u fate %u)", s.op_dest[0], fate[0]);
        CHECK(s.op_dest[1] == 0 && fate[1] == VR_FATE_WORLD2D, "LINE encadenada por la ordenación: mundo, no panel (dest %u fate %u)", s.op_dest[1], fate[1]);
        CHECK(s.op_dest[2] == 0 && fate[2] == VR_FATE_WORLD2D, "POLY sin vista encadenado (0x80018304), fuera del pase del mundo: mundo, no panel (dest %u fate %u)", s.op_dest[2], fate[2]);
        CHECK(s.op_dest[3] == 2 && s.op_cls[3] == VPC_HUD_WORLD && s.op_dest[4] == 2 && s.op_cls[4] == VPC_HUD_WORLD, "menuman.c (directo y encadenado): HUD, sin cambios (%u %u)", s.op_dest[3], s.op_dest[4]);
        CHECK(s.op_dest[5] == 2 && s.op_cls[5] == VPC_FULL, "telop.c encadenado: FULL, sin cambios (%u)", s.op_dest[5]);
        CHECK(s.op_dest[6] == 2 && s.op_cls[6] == VPC_FX_LETTERBOX, "cinema.c encadenado: FX_LETTERBOX, sin cambios (%u)", s.op_dest[6]);
        CHECK(s.op_dest[7] == 2 && s.op_cls[7] == VPC_UNKNOWN && s.op_dest[9] == 2 && s.op_cls[9] == VPC_UNKNOWN, "sin procedencia / otra función no encadenada: UNKNOWN al HUD, sin cambios (%u %u)", s.op_dest[7], s.op_dest[9]);
        CHECK(s.op_dest[8] == 2 && s.op_cls[8] == VPC_FX_SCREEN, "gas_efct.c: sale de vr_scene con la clase FX_SCREEN (%u %u)", s.op_dest[8], s.op_cls[8]);
        uint8_t b0[10], b1[10], b2[10]; uint8_t *out[VPC_L_N] = {b0, b1, b2}; uint32_t lc[VPC_L_N];
        vpc_split_dest(s.op_dest, s.op_cls, s.n_op_dest, out, lc);
        CHECK(lc[VPC_L_HUD] == 4 && lc[VPC_L_FULL] == 1 && lc[VPC_L_WINDOW] == 0 && b0[8] == 0 && b1[8] == 0 && b2[8] == 0 && b0[0] == 0 && b0[1] == 0 && b0[2] == 0,
              "panel: HUD 4 (menuman x2, sin procedencia, otra función), créditos 1; ni el gas (sin capa) ni los efectos del mundo (no salen de vr_scene) (%u %u %u)", lc[0], lc[1], lc[2]);
    }
    /* ventana aprendida cuyo escritor encadena la ordenación: sigue siendo WINDOW y va al panel */
    {
        VrProvLearn S; vpc_learn_reset(&S); S.win[I_WIN] = 1;
        GpuOp keep = ops[0]; ops[0].prov2 = I_WIN;
        o.op_class_ctx = &S;
        CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.op_dest && s.op_cls && s.op_dest[0] == 2 && s.op_cls[0] == VPC_WINDOW, "ventana aprendida encadenada por la ordenación: WINDOW al panel, sin cambios");
        ops[0] = keep; o.op_class_ctx = NULL;
    }
    /* sin drop_world2d (menús, vídeo, ventana del selector / pausa): todo va al panel como antes (clase WORLD: capa HUD) */
    o.drop_world2d = 0;
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.op_dest && s.op_cls && s.op_dest[0] == 2 && s.op_dest[1] == 2 && s.op_dest[2] == 2 && s.op_cls[0] == VPC_WORLD && s.op_cls[1] == VPC_WORLD && s.op_cls[2] == VPC_WORLD,
          "sin drop_world2d: vr_scene no descarta nada; clase WORLD");
    if (s.op_dest && s.op_cls) {
        uint8_t b0[10], b1[10], b2[10]; uint8_t *out[VPC_L_N] = {b0, b1, b2}; uint32_t lc[VPC_L_N];
        vpc_split_dest(s.op_dest, s.op_cls, s.n_op_dest, out, lc);
        CHECK(lc[VPC_L_HUD] == 7 && b0[0] == 2 && b0[1] == 2 && b0[2] == 2 && b0[8] == 0, "sin drop_world2d: las primitivas del canal 3D siguen en la capa HUD, como antes; el gas no (%u)", lc[0]);
    }
    /* sin gancho de procedencia: como antes */
    o.drop_world2d = 1; o.op_class = NULL;
    CHECK(vr_scene_build(&dl, NULL, &o, &s) == 0 && s.op_dest && s.op_dest[0] == 2 && s.op_dest[1] == 2 && s.op_dest[2] == 2, "sin gancho: RECT / LINE / POLY posterior al 3D van al panel, como antes");
    vr_scene_set_fate(NULL, 0);
    vr_scene_release();
}

static GpuOp *quad(int prov, const float x[4], const float y[4], const int c[4]) {
    GpuOp *o = add(GPU_OP_POLY, prov);
    o->nv = 4; o->flags = (uint16_t)(GPU_F_GOURAUD | GPU_F_SEMI | GPU_F_QUAD); o->abr = 2;
    for (int i = 0; i < 4; i++) { o->v[i].x = (int16_t)lrintf(x[i]); o->v[i].y = (int16_t)lrintf(y[i]); o->v[i].r = o->v[i].g = o->v[i].b = (uint8_t)c[i]; }
    return o;
}

static void test_fx(void) {
    VrFx fx;
    /* ---- bandas de cinemática: fracción cubierta del alto mostrado, en coordenadas relativas a la zona mostrada (doble búfer) ---- */
    reset();
    rect(I_CINEMA, 0, 0, 320, 32, 0); rect(I_CINEMA, 0, 208, 320, 32, 0);
    vpc_fx_extract(NULL, &dl, 0, &fx);
    CHECK(fabsf(fx.letterbox - 64.0f / 240.0f) < 1e-4f, "letterbox 64/240 = %.4f", (double)fx.letterbox);
    CHECK(fx.vignette == 0 && fx.scope_on == 0 && fx.scope_zoom == 1.0f, "solo hay bandas: sin viñeta ni mira");
    reset(); dl.disp.y = 240;   /* búfer inferior: las coordenadas llevan el origen de la zona mostrada */
    rect(I_CINEMA, 0, 240, 320, 40, 0); rect(I_CINEMA, 0, 240, 320, 20, 0); rect(I_CINEMA, 0, 440, 320, 40, 0);
    vpc_fx_extract(NULL, &dl, 0, &fx);
    CHECK(fabsf(fx.letterbox - 80.0f / 240.0f) < 1e-4f, "letterbox con origen y = 240 y bandas solapadas (la unión, no la suma): %.4f", (double)fx.letterbox);
    reset();
    rect(I_CINEMA, 0, -20, 320, 52, 0);   /* sale de la zona mostrada: se recorta */
    vpc_fx_extract(NULL, &dl, 0, &fx);
    CHECK(fabsf(fx.letterbox - 32.0f / 240.0f) < 1e-4f, "letterbox recortado a la zona mostrada: %.4f", (double)fx.letterbox);
    reset();
    rect(I_CINEMA, 0, 0, 320, 240, 0);   /* pantalla entera: no es una banda */
    vpc_fx_extract(NULL, &dl, 0, &fx);
    CHECK(fx.letterbox <= 0.8f, "cobertura total acotada (%.3f)", (double)fx.letterbox);
    reset();
    rect(I_MENUMAN, 0, 0, 320, 32, 0);
    vpc_fx_extract(NULL, &dl, 0, &fx);
    CHECK(fx.letterbox == 0, "un rectángulo de otro actor no es banda");

    /* ---- viñeta: abanico de 14 cuadriláteros G4 semitransparentes (abr 2) alrededor de (160,120): interior de color 0 a radio 60, exterior de color 96 a radio 110 ---- */
    reset();
    for (int i = 0; i < 14; i++) {
        float a0 = 6.2831853f * (float)i / 14.0f, a1 = 6.2831853f * (float)(i + 1) / 14.0f;
        float x[4] = {160 + 60 * cosf(a0), 160 + 60 * cosf(a1), 160 + 110 * cosf(a0), 160 + 110 * cosf(a1)};
        float y[4] = {120 + 60 * sinf(a0), 120 + 60 * sinf(a1), 120 + 110 * sinf(a0), 120 + 110 * sinf(a1)};
        int c[4] = {0, 0, 96, 96};
        quad(I_MOTSE, x, y, c);
    }
    vpc_fx_extract(NULL, &dl, 0, &fx);
    CHECK(fabsf(fx.vignette - 96.0f / 255.0f) < 1e-4f, "viñeta intensidad 96/255: %.4f", (double)fx.vignette);
    CHECK(fabsf(fx.vign_radius - 0.5f) < 0.03f, "viñeta radio limpio 60/120 = 0,5: %.4f", (double)fx.vign_radius);
    CHECK(fx.letterbox == 0 && fx.scope_on == 0, "solo viñeta");
    /* abanico con el vértice central (triángulos): radio 0 */
    reset();
    for (int i = 0; i < 14; i++) {
        float a0 = 6.2831853f * (float)i / 14.0f, a1 = 6.2831853f * (float)(i + 1) / 14.0f;
        GpuOp *o = add(GPU_OP_POLY, I_MOTSE); o->nv = 3; o->flags = GPU_F_GOURAUD | GPU_F_SEMI; o->abr = 2;
        o->v[0].x = 160; o->v[0].y = 120; o->v[1].x = (int16_t)lrintf(160 + 130 * cosf(a0)); o->v[1].y = (int16_t)lrintf(120 + 130 * sinf(a0));
        o->v[2].x = (int16_t)lrintf(160 + 130 * cosf(a1)); o->v[2].y = (int16_t)lrintf(120 + 130 * sinf(a1));
        o->v[1].r = o->v[2].r = 200; o->v[1].g = o->v[2].g = 200; o->v[1].b = o->v[2].b = 200;
    }
    vpc_fx_extract(NULL, &dl, 0, &fx);
    CHECK(fabsf(fx.vignette - 200.0f / 255.0f) < 1e-4f && fx.vign_radius < 0.03f, "abanico desde el centro: intensidad %.3f radio %.3f", (double)fx.vignette, (double)fx.vign_radius);
    /* color de los vértices fuera de [0,255] no existe (uint8); intensidad acotada a 1 */
    reset();
    { float x[4] = {0, 320, 0, 320}, y[4] = {0, 0, 240, 240}; int c[4] = {255, 255, 255, 255}; quad(I_MOTSE, x, y, c); }
    vpc_fx_extract(NULL, &dl, 0, &fx);
    CHECK(fx.vignette == 1.0f, "intensidad máxima 1");

    /* ---- mira (scope.c / sight.c): presencia y aumento candidato = pantalla H efectiva / 160 (> 1) ---- */
    reset();
    rect(I_SCOPE, 0, 0, 30, 30, 0); rect(I_SIGHT, 100, 100, 8, 8, GPU_F_TEXTURED);
    vpc_fx_extract(NULL, &dl, 480, &fx);
    CHECK(fx.scope_on == 1 && fabsf(fx.scope_zoom - 3.0f) < 1e-4f, "mira: activa, aumento 480/160 = %.3f", (double)fx.scope_zoom);
    vpc_fx_extract(NULL, &dl, 160, &fx);
    CHECK(fx.scope_on == 1 && fx.scope_zoom == 1.0f, "H = 160: sin aumento (%.3f)", (double)fx.scope_zoom);
    vpc_fx_extract(NULL, &dl, 100, &fx);
    CHECK(fx.scope_on == 1 && fx.scope_zoom == 1.0f, "H < 160: aumento 1");
    vpc_fx_extract(NULL, &dl, 0, &fx);
    CHECK(fx.scope_on == 1 && fx.scope_zoom == 1.0f, "sin cámara de la lista: aumento 1");
    vpc_fx_extract(NULL, &dl, 20000, &fx);
    CHECK(fx.scope_on == 1 && fx.scope_zoom <= 24.0f && fx.scope_zoom >= 1.0f, "aumento acotado (%.2f)", (double)fx.scope_zoom);
    reset();
    rect(I_MENUMAN, 0, 0, 30, 30, 0);
    vpc_fx_extract(NULL, &dl, 480, &fx);
    CHECK(fx.scope_on == 0 && fx.scope_zoom == 1.0f, "sin scope.c no hay mira (aunque la pantalla H sea alta)");

    /* ---- sin procedencia (NSL1) o sin tabla: nada, sin fallos ---- */
    reset(); dl.prov = NULL; dl.nprov = 0;
    rect(0, 0, 0, 320, 30, 0); rect(5, 0, 0, 10, 10, 0);
    vpc_fx_extract(NULL, &dl, 480, &fx);
    CHECK(fx.letterbox == 0 && fx.vignette == 0 && fx.scope_on == 0 && fx.scope_zoom == 1.0f, "sin procedencia: sin efectos");
    vpc_fx_extract(NULL, NULL, 0, &fx);
    CHECK(fx.letterbox == 0 && fx.scope_on == 0, "lista NULL");
    vpc_fx_extract(NULL, &dl, 0, NULL);
    CHECK(1, "fx NULL");

    /* ---- retención ---- */
    VrFxHold H; memset(&H, 0, sizeof H); H.fx.scope_zoom = 1.0f;
    VrFx on; memset(&on, 0, sizeof on); on.letterbox = 0.25f; on.vignette = 0.4f; on.vign_radius = 0.5f; on.scope_on = 1; on.scope_zoom = 3.0f;
    VrFx off; memset(&off, 0, sizeof off); off.scope_zoom = 1.0f;
    vpc_fx_hold(&H, &on);
    CHECK(H.fx.letterbox == 0.25f && H.fx.vignette == 0.4f && H.fx.scope_on == 1 && H.fx.scope_zoom == 3.0f, "hold: el efecto entra");
    for (int k = 0; k < VPC_FX_HOLD; k++) vpc_fx_hold(&H, &off);
    CHECK(H.fx.letterbox == 0.25f && H.fx.vignette == 0.4f && H.fx.scope_on == 1 && H.fx.scope_zoom == 3.0f, "hold: se mantiene %d listas", VPC_FX_HOLD);
    vpc_fx_hold(&H, &off);
    CHECK(H.fx.letterbox == 0 && H.fx.vignette == 0 && H.fx.vign_radius == 0 && H.fx.scope_on == 0 && H.fx.scope_zoom == 1.0f, "hold: se apaga tras %d listas sin aparecer", VPC_FX_HOLD);
    vpc_fx_hold(&H, &on); vpc_fx_hold(&H, &off); vpc_fx_hold(&H, &on);
    CHECK(H.age_v == 0 && H.age_l == 0 && H.age_s == 0, "hold: reaparecer reinicia la cuenta");
}

int main(void) {
    test_rules();
    test_window_learning();
    test_chain();
    test_unknown_and_split();
    test_scene_hook();
    test_scene_world();
    test_fx();
    if (fails) { fprintf(stderr, "test_prov_class: %d fallos de %d\n", fails, checks); return 1; }
    printf("test_prov_class: OK (%d comprobaciones)\n", checks);
    return 0;
}
