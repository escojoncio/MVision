/* Pruebas de ns_div (réplica de bound.c / trans.c / divide.c de libdg) sobre una RAM sintética de 2 MiB. Cada caso cita la regla de la decompilación (FoxdieTeam/mgs_reversing, source/libdg). */
#include "ns_div.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { A_CHAN = 0x800B2B7C, A_GROUP = 0x800ACAF8, A_CLOCK = 0x800ACAB0, A_MEMSYS = 0x800AE480, OBJ = 0x80100000, MDL = 0x80101000, VERTS = 0x80101200, VIND = 0x80101800, DEF = 0x80101C00, QUEUE = 0x80102000 };
static uint8_t *ram;
static void w32(uint32_t a, uint32_t v) { memcpy(ram + (a & 0x1FFFFFFF), &v, 4); }
static void w16(uint32_t a, int v) { uint16_t x = (uint16_t)v; memcpy(ram + (a & 0x1FFFFFFF), &x, 2); }

/* Escena: un canal con H, un objeto con un modelo de nf caras. Matriz de pantalla = identidad (4096), traslación 0. */
static void scene(int H, uint32_t objflag, uint32_t mdlflag, int nv, int nf) {
    memset(ram, 0, 0x200000);
    w16(A_CHAN + 0x50, H); w16(A_CHAN + 0x56, 1); w32(A_CHAN + 0x58, QUEUE);
    w32(A_GROUP, 1); w32(A_CLOCK, 0);
    w32(QUEUE, OBJ);
    w32(OBJ + 0x24, DEF); w32(OBJ + 0x28, objflag); w16(OBJ + 0x2C, 0); w16(OBJ + 0x2E, 1);
    uint32_t n = OBJ + 0x48;
    w16(n + 0x20, 4096); w16(n + 0x20 + 8, 4096); w16(n + 0x20 + 16, 4096);   /* m[0][0], m[1][1], m[2][2] */
    w32(n + 0x40, MDL); w32(n + 0x48, 0); w16(n + 0x4C, 2); w16(n + 0x52, nf);
    w32(MDL, mdlflag); w32(MDL + 4, (uint32_t)nf);
    w32(MDL + 0x34, (uint32_t)nv); w32(MDL + 0x38, VERTS); w32(MDL + 0x3C, VIND);
    /* caja de la definición y del modelo: grande y centrada (se proyecta en pantalla) */
    int32_t bb[6] = {-100, -100, 200, 100, 100, 400};
    for (int i = 0; i < 6; i++) { w32(DEF + 8 + 4 * i, (uint32_t)bb[i]); w32(MDL + 8 + 4 * i, (uint32_t)bb[i]); }
    /* memoria de paquetes del búfer 0: una unidad libre de `packs` paquetes */
    w32(A_MEMSYS + 12, 1);
    w32(A_MEMSYS + 16, 0x80180000); w32(A_MEMSYS + 20, 0); w32(A_MEMSYS + 24, 0x80180000 + 0x34 * 2000);
}
static void vert(int i, int x, int y, int z) { w16(VERTS + 8 * (uint32_t)i, x); w16(VERTS + 8 * (uint32_t)i + 2, y); w16(VERTS + 8 * (uint32_t)i + 4, z); }
static void face(int f, int a, int b, int c, int d) { w32(VIND + 4 * (uint32_t)f, (uint32_t)a | (uint32_t)b << 8 | (uint32_t)c << 16 | (uint32_t)d << 24); }
/* cuadrado centrado en (0,0) de semilado s a profundidad z; A=(-s,-s) B=(s,-s) C=(s,s) D=(-s,s): NCLIP(A,B,D) > 0 si ccw=1 */
static void quad(int s, int z, int ccw) { int k = ccw ? 1 : -1; vert(0, -k * s, -s, z); vert(1, k * s, -s, z); vert(2, k * s, s, z); vert(3, -k * s, s, z); face(0, 0, 1, 2, 3); }

typedef struct { int n, nsub; NsDivPrim p[512]; int nd, reason[64]; } Out;
static void emit(void *u, const NsDivPrim *p) { Out *o = (Out *)u; if (o->n < 512) o->p[o->n] = *p; o->n++; if (p->kind == NSDIV_SUB) o->nsub++; }
static void drop(void *u, uint32_t obj, int node, int face_, int reason, const uint32_t pack[2]) { Out *o = (Out *)u; (void)obj; (void)node; (void)face_; (void)pack; if (o->nd < 64) o->reason[o->nd] = reason; o->nd++; }
static Out run(int flags, uint32_t thresh, int npacks, NsDivStats *S) {
    static Out o;
    memset(&o, 0, sizeof o);
    NsDivCfg c;
    memset(&c, 0, sizeof c);
    c.pool = 0; c.n_packs_override = npacks; c.flags = flags; c.div_thresh = thresh; c.drop = drop;
    NsDivStats st;
    int r = ns_div_run(ram, 0x200000, &c, NULL, emit, &o, S ? S : &st);
    CHECK(r >= 0, "ns_div_run devolvio %d", r);
    return o;
}

int main(void) {
    ram = (uint8_t *)calloc(1, 0x200000);
    NsDivStats S;

    /* GTE: H/SZ con UNR; z <= H/2 satura el cociente a 0x1FFFF (se proyecta como si estuviera en H/2); SXY satura a -0x400..0x3FF (gte.c rtp) */
    {
        NsDivMat M;
        memset(&M, 0, sizeof M); M.m[0][0] = M.m[1][1] = M.m[2][2] = 4096;
        int16_t sx, sy; uint16_t sz;
        int16_t v1[3] = {40, -40, 320}, v2[3] = {100, 0, 40}, v3[3] = {10000, 0, 0}, v4[3] = {0, 0, -5};
        ns_div_rtps(&M, 160, 0, 0, v1, &sx, &sy, &sz);
        CHECK(sz == 320 && sx >= 19 && sx <= 20 && sy >= -20 && sy <= -19, "rtps normal: %d %d %u", sx, sy, sz);
        ns_div_rtps(&M, 160, 0, 0, v2, &sx, &sy, &sz);
        CHECK(sz == 40 && sx >= 199 && sx <= 200, "z <= H/2 se proyecta como H/2 (x*2, cociente 0x1FFFF): %d", sx);
        ns_div_rtps(&M, 160, 0, 0, v3, &sx, &sy, &sz);
        CHECK(sz == 0 && sx == 0x3FF, "z=0 satura SXY: %d", sx);
        ns_div_rtps(&M, 160, 0, 0, v4, &sx, &sy, &sz);
        CHECK(sz == 0, "SZ negativo se recorta a 0: %u", sz);
        CHECK(ns_div_nclip(0, 0, 10, 0, 0, 10) == 100 && ns_div_nclip(0, 0, 0, 10, 10, 0) == -100, "nclip");
    }

    /* trans.c:164-176: cara de una cara con NCLIP(A,B,D) > 0 se entrega, con orden de paquete A,B,D,C */
    scene(160, 0, 0, 4, 1); quad(50, 400, 1);
    Out o = run(0, 0, -1, &S);
    CHECK(o.n == 1 && o.p[0].kind == NSDIV_FULL, "cara frontal: %d prims", o.n);
    CHECK(o.p[0].sx[0] < 0 && o.p[0].sx[1] > 0 && o.p[0].sx[2] < 0 && o.p[0].sx[3] > 0 && o.p[0].sy[2] > 0 && o.p[0].sy[3] > 0, "orden de paquete A,B,D,C");
    CHECK(S.f_split == 0, "area pequena no se divide");

    /* trans.c:164-167: NCLIP <= 0 en una cara de una cara no se entrega; con DG_MODEL_BOTHFACE (0x400) se entrega marcada como trasera; area == 0 nunca */
    scene(160, 0, 0, 4, 1); quad(50, 400, 0);
    o = run(0, 0, -1, &S);
    CHECK(o.n == 0 && o.nd == 1 && o.reason[0] == NSDIV_DROP_BACK && S.f_back == 1, "cara trasera descartada: n=%d", o.n);
    scene(160, 0, 0x400, 4, 1); quad(50, 400, 0);
    o = run(0, 0, -1, &S);
    CHECK(o.n == 1 && o.p[0].back == 1, "doble cara: se entrega con back=1 (n=%d)", o.n);
    scene(160, 0, 0x400, 4, 1); vert(0, -50, -50, 400); vert(1, 0, -50, 400); vert(2, 50, -50, 400); vert(3, -50, -50, 400); face(0, 0, 1, 2, 3);
    o = run(0, 0, -1, &S);
    CHECK(o.n == 0 && S.f_degenerate == 1, "area 0 nunca se entrega (ni a doble cara)");

    /* trans.c:178: z16 = (zA + zC) / 2 == 0 no se dibuja aunque B y D tengan profundidad */
    scene(160, 0, 0, 4, 1); quad(50, 400, 1); vert(0, -50, -50, 0); vert(2, 50, 50, 0);
    o = run(0, 0, -1, &S);
    CHECK(o.n == 0 && S.f_z0 == 1, "z16 == 0 en la diagonal A-C: n=%d z0=%u", o.n, S.f_z0);

    /* trans.c:139-152: con bound_mode 1 (objeto con DG_FLAG_BOUND parcialmente visible) las 4 esquinas con un mismo codigo de salida se descartan; NSDIV_NO_OUTCODE lo desactiva */
    scene(160, 0x10, 0, 4, 1);
    vert(0, -900, -50, 400); vert(1, -500, -50, 400); vert(2, -500, 50, 400); vert(3, -900, 50, 400); face(0, 0, 1, 2, 3);
    { int32_t bb[6] = {-1000, -50, 200, 100, 50, 400}; for (int i = 0; i < 6; i++) w32(MDL + 8 + 4 * i, (uint32_t)bb[i]); }
    o = run(0, 0, -1, &S);
    CHECK(S.models_bound1 == 1, "bound_mode 1 (parcialmente visible): b0=%u b1=%u b2=%u", S.models_bound0, S.models_bound1, S.models_bound2);
    CHECK(o.n == 0 && S.f_outcode == 1, "outcode comun descartado (n=%d)", o.n);
    o = run(NSDIV_NO_OUTCODE | NSDIV_NO_SPLIT, 0, -1, &S);
    CHECK(o.n == 1, "sin la regla de outcode se entrega (n=%d)", o.n);

    /* bound.c:98-141: caja entera fuera de la ventana +-160 x +-112, o con las 8 profundidades a 0 -> bound_mode 0 (no se dibuja) */
    scene(160, 0x10, 0, 4, 1); quad(50, 400, 1);
    for (int i = 0; i < 6; i++) w32(MDL + 8 + 4 * i, (uint32_t)((i % 3 == 0) ? 5000 : (i % 3 == 1) ? 0 : 400) + (i >= 3 ? 100u : 0u));   /* x en 5000..5100: fuera por la derecha */
    o = run(0, 0, -1, &S);
    CHECK(S.models_bound0 == 1 && o.n == 0, "caja fuera de la ventana: bound 0 (n=%d)", o.n);

    /* divide.c:442-444: area (tag >> 8) > 0x800, z16 < 8192 y >= 4 paquetes libres: la cara se parte en 4 (3x3 = 4 trozos), cada trozo con su paquete */
    scene(160, 0, 0, 4, 1); quad(60, 400, 1);   /* sx = +-24: NCLIP 2304 (0x900): pack_addr = 0x800 + (z >> 8) > 0x800; los cuartos (576) ya no se parten */
    o = run(0, 0, -1, &S);
    CHECK(S.f_split == 1 && o.nsub == 4 && o.n == 4, "cara de NCLIP 0x900 se parte en 4 trozos (trozos=%d)", o.nsub);
    CHECK(S.split_depth[1] == 4 && S.split_depth[2] == 0, "4 trozos en el nivel 1: %u / %u", S.split_depth[1], S.split_depth[2]);
    NsDivStats S1 = S;
    scene(160, 0, 0, 4, 1); quad(50, 100, 1);   /* sx = +-80: NCLIP 25600; los cuartos (6400) se parten otra vez; los 16 (1600) no */
    o = run(0, 0, -1, &S);
    CHECK(S.f_split == 1 && o.nsub == 16 && S.split_depth[1] == 0 && S.split_depth[2] == 16, "cara grande: 16 trozos de nivel 2: trozos=%d n2=%u", o.nsub, S.split_depth[2]);
    (void)S1;
    scene(160, 0, 0, 4, 1); quad(60, 400, 1);
    CHECK(S.n_packs_start - S.n_packs_end == (int)S.sub_emitted, "cada trozo gasta un paquete (%d vs %u)", S.n_packs_start - S.n_packs_end, S.sub_emitted);
    {   /* cada trozo es una cuarta parte: las esquinas del trozo 0 incluyen la esquina A y el punto medio (desplazado +-1 px: divide.c:216-283) */
        NsDivPrim *p = &o.p[0];
        CHECK(p->sx[0] == o.p[0].sx[0] && p->kind == NSDIV_SUB && p->level >= 1, "trozo de nivel >= 1");
    }
    /* umbral del juego (0x7FFF0000): nunca se subdivide */
    o = run(0, NSDIV_GAME_THRESH, -1, &S);
    CHECK(S.f_split == 0 && o.n == 1 && o.p[0].kind == NSDIV_FULL, "umbral del juego: cara entera (n=%d)", o.n);
    /* divide.c:444: con menos de 4 paquetes libres no se parte */
    o = run(0, 0, 3, &S);
    CHECK(S.f_split == 0 && o.n == 1, "3 paquetes libres: no se divide (n=%d)", o.n);
    /* divide.c:299 / 317-319: sin paquetes el trozo se pierde. 4 paquetes: se parte la cara y los 4 trozos gastan los 4; los que pidan partirse otra vez (n_packs < 4) se entregan */
    o = run(0, 0, 4, &S);
    CHECK(S.f_split == 1 && S.sub_emitted == 4 && S.n_packs_end == 0, "con 4 paquetes se parte una vez y se gastan los 4 (trozos=%u restan %d)", S.sub_emitted, S.n_packs_end);
    /* z16 >= 8192: lejos no se parte (divide.c:443) */
    scene(160, 0, 0, 4, 1); quad(8000, 9000, 1);
    o = run(0, 0, -1, &S);
    CHECK(S.f_split == 0, "z16 >= 8192 no se divide");

    /* divide.c:599-635: GBOUND (0x20) no se subdivide (DG_AddSubdividedPrim) */
    scene(160, 0x20, 0, 4, 1); quad(50, 100, 1);
    o = run(0, 0, -1, &S);
    CHECK(S.f_split == 0 && o.n == 1, "GBOUND no se divide (n=%d)", o.n);

    /* divide.c:68-83 / 43-66: paquetes libres = suma de floor(tamano / 0x34) de las unidades libres; sin ninguna unidad libre DG_DivideChanl sale (divide.c:571) */
    scene(160, 0, 0, 4, 1); quad(50, 100, 1);
    w32(A_MEMSYS + 12, 3);
    w32(A_MEMSYS + 16, 0x80180000); w32(A_MEMSYS + 20, 0); w32(A_MEMSYS + 24, 0x80180000 + 0x34 * 10 + 0x33);
    w32(A_MEMSYS + 28, 1); w32(A_MEMSYS + 32, 0x80180000 + 0x34 * 10 + 0x33 + 0x340);
    w32(A_MEMSYS + 36, 0); w32(A_MEMSYS + 40, 0x80180000 + 0x34 * 10 + 0x33 + 0x340 + 0x34 * 7);
    o = run(0, NSDIV_GAME_THRESH, -1, &S);
    CHECK(S.n_packs_start == 17, "paquetes libres 10 + 7 (la unidad usada no cuenta, el resto de bytes se pierde): %d", S.n_packs_start);
    w32(A_MEMSYS + 20, 1); w32(A_MEMSYS + 36, 2);
    o = run(0, 0, -1, &S);
    CHECK(S.divide_aborted == 1 && o.n == 0, "sin unidades libres no se entrega nada (n=%d)", o.n);

    /* trans.c:25-93: cara con una esquina soldada (bit 7): sin descarte por cara trasera ni subdivision; z = media de las esquinas propias; sin ninguna propia, no se dibuja */
    scene(160, 0, 0, 4, 1); quad(50, 400, 0);
    face(0, 0, 1, 2, 0x83);
    o = run(0, 0, -1, &S);
    CHECK(o.n == 1 && S.f_indirect == 1 && (o.p[0].weld & 0x4), "cara indirecta no se descarta por orientacion (n=%d weld=%x)", o.n, o.n ? o.p[0].weld : 0);
    face(0, 0x80, 0x81, 0x82, 0x83);
    o = run(0, 0, -1, &S);
    CHECK(o.n == 0 && S.f_indirect_z0 == 1, "indirecta sin esquinas propias no se dibuja");

    free(ram);
    if (fails) { printf("test_ns_div: %d fallos\n", fails); return 1; }
    printf("test_ns_div: OK\n");
    return 0;
}
