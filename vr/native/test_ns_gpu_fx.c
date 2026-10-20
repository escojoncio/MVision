/* Pruebas sintéticas de ns_gpu_fx_build (geometría de efectos del renderer nativo). Sin datos del juego. */
#include "ns_gpu.h"
#include "ns_fx_export.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_fail;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static float to_vr[12] = {0.001f, 0, 0, 0, 0, -0.001f, 0, 0, 0, 0, 0.001f, 0};   /* Y arriba del motor = -Y... (cualquier mapa afín sirve) */
static NsCamera cam;
static NsGpuFxParams P;

static void setup(void) {
    memset(&cam, 0, sizeof cam);
    cam.rot[0] = 1; cam.rot[4] = 1; cam.rot[8] = 1;   /* vista = mundo; adelante = +Z */
    cam.h = 320.0f; cam.overscan_y = NS_OVERSCAN_Y;
    memset(&P, 0, sizeof P);
    P.to_vr = to_vr; P.cam = &cam;
    P.head_right[0] = 1; P.head_up[1] = 1;
    P.eyes[0][0] = -0.03f; P.eyes[1][0] = 0.03f; P.n_eyes = 2;
}
static NsEffect quad(float x, float y, float z, float s) {
    NsEffect e; memset(&e, 0, sizeof e);
    e.kind = NS_EK_QUAD; e.nv = 4; e.src_key = 0x80100000u; e.tpage = 0x15; e.clut = 0x3d7e;
    const float d[4][2] = {{-s, -s}, {s, -s}, {-s, s}, {s, s}};
    for (int v = 0; v < 4; v++) { e.p[v][0] = x + d[v][0]; e.p[v][1] = y + d[v][1]; e.p[v][2] = z; for (int c = 0; c < 3; c++) e.rgb[v][c] = 128; e.uv[v][0] = (uint8_t)(v & 1 ? 31 : 0); e.uv[v][1] = (uint8_t)(v >> 1 ? 31 : 0); }
    return e;
}
static int build(NsEffect *fx, uint32_t n, NsGpuFxVert *vb, NsGpuFxRun *runs, uint64_t *keys, NsGpuFxOut *o) {
    return ns_gpu_fx_build(fx, n, &P, vb, 12 * n + 12, runs, n + 1, keys, n + 1, o);
}

static void t_poly(void) {
    NsEffect fx[3];
    fx[0] = quad(0, 0, 4000, 100); fx[0].idx = 1; fx[0].flags = NS_EF_TEXTURED;                       /* opaco texturizado */
    fx[1] = quad(0, 0, 3000, 100); fx[1].idx = 2; fx[1].flags = NS_EF_SEMI; fx[1].abr = 1;             /* semi sin textura */
    fx[2] = quad(0, 0, 2000, 100); fx[2].idx = 3; fx[2].flags = NS_EF_SEMI | NS_EF_TEXTURED; fx[2].abr = 2; fx[2].nv = 3;
    NsGpuFxVert vb[64]; NsGpuFxRun runs[8]; uint64_t keys[8]; NsGpuFxOut o;
    CHECK(build(fx, 3, vb, runs, keys, &o) == 0, "build");
    /* opaco: fx0 (6) + copia del semi texturizado fx2 (3) */
    CHECK(o.n_opaque == 9, "n_opaque=%u", o.n_opaque);
    CHECK(o.n_semi == 6 + 3, "n_semi=%u", o.n_semi);
    CHECK(o.n_keys == 3 && keys[0] == ns_gpu_fx_key(&fx[0]) && keys[1] != keys[0] && keys[2] != keys[1], "claves");
    /* mundo VR: x = 0.001*x */
    CHECK(fabsf(vb[0].p[0] - (-0.1f)) < 1e-5f && fabsf(vb[0].p[2] - 4.0f) < 1e-5f, "posicion VR %f %f", vb[0].p[0], vb[0].p[2]);
    CHECK(vb[0].eyes == 3u, "ambos ojos");
    CHECK((vb[0].clutf >> 16) & NSG_CF_TEX, "bandera de textura");
    /* semi: de atrás adelante respecto a la cabeza (origen): fx1 a 3 m antes que fx2 a 2 m */
    const NsGpuFxVert *s0 = &vb[o.n_opaque];
    CHECK(fabsf(s0[0].p[2] - 3.0f) < 1e-5f, "primero el mas lejano z=%f", s0[0].p[2]);
    CHECK(fabsf(s0[6].p[2] - 2.0f) < 1e-5f, "despues el cercano z=%f", s0[6].p[2]);
    CHECK(o.n_runs == 2 && runs[0].abr == 1 && runs[1].abr == 2 && runs[0].start == o.n_opaque && runs[0].count == 6 && runs[1].start == o.n_opaque + 6 && runs[1].count == 3, "runs por modo de mezcla");
    CHECK(((vb[0].clutf >> 16) >> 2 & 3) == 0 && ((s0[6].clutf >> 16) >> 2 & 3) == 2, "abr en las banderas");
}

static void t_billboard(void) {
    NsEffect e; memset(&e, 0, sizeof e);
    e.kind = NS_EK_BILLBOARD; e.nv = 4; e.flags = NS_EF_TEXTURED | NS_EF_SEMI; e.abr = 1; e.src_key = 0x80200000u; e.tpage = 0x1a; e.clut = 0x3db1;
    e.p[0][0] = 500; e.p[0][1] = -200; e.p[0][2] = 3200;     /* centro */
    e.ref_h = 320.0f; e.box[0] = -16; e.box[1] = -16; e.box[2] = 16; e.box[3] = 16;   /* 32x32 px a z = 3200 -> 320x320 unidades (con y/ys) */
    for (int v = 0; v < 4; v++) { e.uv[v][0] = (uint8_t)(v & 1 ? 63 : 0); e.uv[v][1] = (uint8_t)(v >> 1 ? 63 : 0); for (int c = 0; c < 3; c++) e.rgb[v][c] = 128; }
    NsGpuFxVert vb[64]; NsGpuFxRun runs[4]; uint64_t keys[4]; NsGpuFxOut o;
    CHECK(build(&e, 1, vb, runs, keys, &o) == 0 && o.n_billboard == 1, "billboard");
    const NsGpuFxVert *s = &vb[o.n_opaque];
    /* esquinas 0 (sup-izq),1,2 (inf-izq) = vb[0],[1],[2]; 3 = [5] */
    float w = s[1].p[0] - s[0].p[0], h = s[0].p[1] - s[2].p[1];
    CHECK(fabsf(w - 0.32f) < 1e-4f, "ancho %f", w);
    CHECK(fabsf(fabsf(h) - 0.32f / NS_OVERSCAN_Y) < 1e-4f, "alto %f", h);
    CHECK(fabsf(s[0].p[2] - 3.2f) < 1e-5f && fabsf(s[1].p[2] - 3.2f) < 1e-5f, "plano perpendicular a la vista");
    CHECK(s[0].u == 0 && s[1].u == 63 && s[2].v == 63, "uv de las esquinas");
    /* orientado a la cabeza: con otra cabeza (ejes girados) el cartel gira con ella */
    P.head_right[0] = 0; P.head_right[2] = 1;   /* la cabeza mira hacia -X o +X: su derecha pasa a Z */
    CHECK(build(&e, 1, vb, runs, keys, &o) == 0, "rebuild");
    s = &vb[o.n_opaque];
    CHECK(fabsf(s[1].p[0] - s[0].p[0]) < 1e-5f && fabsf(s[1].p[2] - s[0].p[2]) > 0.3f, "cartel girado con la cabeza");
    P.head_right[0] = 1; P.head_right[2] = 0;
    /* profundidad pequeña: se usa la de referencia en vez de explotar */
    e.p[0][2] = 5.0f;
    CHECK(build(&e, 1, vb, runs, keys, &o) == 0, "z pequena");
    s = &vb[o.n_opaque];
    CHECK(fabsf(s[1].p[0] - s[0].p[0]) < 0.12f, "tamano acotado %f", s[1].p[0] - s[0].p[0]);
}

/* j-render: un cartel de menos de 1 px del motor (destello lejano) se dibuja con 1 px como el hardware, no con 0,2 px; uno mayor no cambia */
static void t_min_px(void) {
    NsEffect e; memset(&e, 0, sizeof e);
    e.kind = NS_EK_BILLBOARD; e.nv = 4; e.flags = NS_EF_TEXTURED | NS_EF_SEMI | NS_EF_WORLD_SIZE; e.abr = 1; e.src_key = 0x80210000u; e.tpage = 0x1a; e.clut = 0x3db1;
    e.p[0][0] = 500; e.p[0][1] = -200; e.p[0][2] = 3200;
    e.box[0] = -1; e.box[1] = -1; e.box[2] = 1; e.box[3] = 1;   /* 2 unidades = 0,2 px a z = 3200 (1 px = 10 unidades) */
    for (int v = 0; v < 4; v++) { e.uv[v][0] = (uint8_t)(v & 1 ? 63 : 0); e.uv[v][1] = (uint8_t)(v >> 1 ? 63 : 0); for (int c = 0; c < 3; c++) e.rgb[v][c] = 128; }
    NsGpuFxVert vb[64]; NsGpuFxRun runs[4]; uint64_t keys[4]; NsGpuFxOut o;
    CHECK(build(&e, 1, vb, runs, keys, &o) == 0 && o.n_billboard == 1, "billboard pequeno");
    const NsGpuFxVert *s = &vb[o.n_opaque];
    float w = s[1].p[0] - s[0].p[0];
    CHECK(fabsf(w - 0.010f) < 2e-4f, "minimo de 1 px: ancho %f (0,01 m)", w);
    e.box[0] = -50; e.box[2] = 50; e.box[1] = -50; e.box[3] = 50;   /* 10 px: no se toca */
    CHECK(build(&e, 1, vb, runs, keys, &o) == 0, "billboard 10px");
    s = &vb[o.n_opaque];
    CHECK(fabsf((s[1].p[0] - s[0].p[0]) - 0.100f) < 1e-4f, "10 px sin cambio: %f", s[1].p[0] - s[0].p[0]);
}

static void t_raise_pull(void) {
    NsGpuFxVert vb[64]; NsGpuFxRun runs[4]; uint64_t keys[4]; NsGpuFxOut o;
    /* sesgo raise: el cuadrilátero se acerca a la cabeza raise * raise_m (escalado desde la cabeza) */
    NsEffect q = quad(0, 0, 4000, 100); q.depth_bias = 200;
    P.raise_m = 0.0005f;   /* 200 -> 0,1 m */
    CHECK(build(&q, 1, vb, runs, keys, &o) == 0, "raise");
    CHECK(fabsf(vb[0].p[2] - 3.9f) < 0.01f, "z con raise %f (4.0 - 0.1)", vb[0].p[2]);
    P.raise_m = 0;
    CHECK(build(&q, 1, vb, runs, keys, &o) == 0 && fabsf(vb[0].p[2] - 4.0f) < 1e-5f, "sin raise_m no cambia");
    /* cartel: board_pull adelanta media extensión sin cambiar la proyección */
    NsEffect e; memset(&e, 0, sizeof e);
    e.kind = NS_EK_BILLBOARD; e.nv = 4; e.flags = NS_EF_TEXTURED | NS_EF_SEMI; e.abr = 1; e.src_key = 0x80200000u; e.tpage = 0x1a;
    e.p[0][0] = 500; e.p[0][1] = -200; e.p[0][2] = 3200; e.ref_h = 320.0f; e.box[0] = -16; e.box[1] = -16; e.box[2] = 16; e.box[3] = 16;
    for (int v = 0; v < 4; v++) for (int c = 0; c < 3; c++) e.rgb[v][c] = 128;
    NsGpuFxVert a[16], b[16];
    CHECK(build(&e, 1, a, runs, keys, &o) == 0, "cartel base");
    const NsGpuFxVert *sa = &a[o.n_opaque];
    float pa[3] = {sa[0].p[0], sa[0].p[1], sa[0].p[2]}, wa = sa[1].p[0] - sa[0].p[0];
    P.board_pull = 1.0f;
    CHECK(build(&e, 1, b, runs, keys, &o) == 0, "cartel adelantado");
    const NsGpuFxVert *sb = &b[o.n_opaque];
    float wb = sb[1].p[0] - sb[0].p[0];
    CHECK(sb[0].p[2] < pa[2] - 0.05f && sb[0].p[2] > 0.69f * pa[2], "mas cerca: z %f -> %f", pa[2], sb[0].p[2]);
    CHECK(fabsf(sb[0].p[0] / sb[0].p[2] - pa[0] / pa[2]) < 1e-4f && fabsf(sb[0].p[1] / sb[0].p[2] - pa[1] / pa[2]) < 1e-4f, "misma direccion desde la cabeza");
    CHECK(fabsf(wb / sb[0].p[2] - wa / pa[2]) < 1e-4f, "mismo tamano angular");
    P.board_pull = 0;
}

static void t_ribbon(void) {
    NsEffect e; memset(&e, 0, sizeof e);
    e.kind = NS_EK_QUAD; e.nv = 2; e.flags = NS_EF_LINE | NS_EF_SEMI; e.abr = 1; e.src_key = 0x80300000u;
    e.p[0][2] = 3000; e.p[1][0] = 2000; e.p[1][2] = 3000; e.width_px = 4.0f; e.ref_h = 0;
    for (int v = 0; v < 2; v++) for (int c = 0; c < 3; c++) e.rgb[v][c] = 200;
    NsGpuFxVert vb[64]; NsGpuFxRun runs[4]; uint64_t keys[4]; NsGpuFxOut o;
    CHECK(build(&e, 1, vb, runs, keys, &o) == 0 && o.n_ribbon == 1 && o.n_semi == 6, "cinta");
    const NsGpuFxVert *s = &vb[o.n_opaque];
    float wid = sqrtf((s[1].p[0] - s[0].p[0]) * (s[1].p[0] - s[0].p[0]) + (s[1].p[1] - s[0].p[1]) * (s[1].p[1] - s[0].p[1]) + (s[1].p[2] - s[0].p[2]) * (s[1].p[2] - s[0].p[2]));
    CHECK(fabsf(wid - 4.0f * 3000.0f / 320.0f * 0.001f) < 1e-4f, "anchura de la cinta %f", wid);
    CHECK(fabsf(s[0].p[0] - s[1].p[0]) < 1e-5f, "perpendicular a la linea");
    CHECK(s[0].col[0] == 200.0f, "color");
    /* degenerada: la cabeza sobre la línea */
    P.head[0] = 1.0f; P.head[1] = 0; P.head[2] = 3.0f;
    CHECK(build(&e, 1, vb, runs, keys, &o) == 0 && o.n_skipped == 1 && o.n_keys == 0 && o.n_semi == 0, "cinta degenerada descartada y no confirmada sk=%u k=%u s=%u", o.n_skipped, o.n_keys, o.n_semi);
    memset(P.head, 0, sizeof P.head);
}

static void t_oneside(void) {
    NsEffect e = quad(0, 0, 4000, 100);
    e.flags = NS_EF_ONESIDE | NS_EF_SEMI; e.abr = 1;
    NsGpuFxVert vb[64]; NsGpuFxRun runs[4]; uint64_t keys[4]; NsGpuFxOut o;
    /* el sentido del paquete (0,1,2) da una normal; la cara se ve desde un lado solo */
    P.eyes[0][2] = 0; P.eyes[1][2] = 0;
    CHECK(build(&e, 1, vb, runs, keys, &o) == 0 && o.n_keys == 1, "una cara");
    uint32_t m1 = o.n_semi ? vb[o.n_opaque].eyes : 0;
    /* ojos al otro lado del plano: la máscara se invierte */
    P.eyes[0][2] = 8.0f; P.eyes[1][2] = 8.0f;
    CHECK(build(&e, 1, vb, runs, keys, &o) == 0 && o.n_keys == 1, "una cara, otro lado");
    uint32_t m2 = o.n_semi ? vb[o.n_opaque].eyes : 0;
    CHECK(m1 != m2 && (m1 == 0 || m1 == 3) && (m2 == 0 || m2 == 3), "caras opuestas m1=%u m2=%u", m1, m2);
    /* un ojo a cada lado: máscara de un solo bit */
    P.eyes[0][2] = 0; P.eyes[1][2] = 8.0f;
    CHECK(build(&e, 1, vb, runs, keys, &o) == 0, "ojos a ambos lados");
    uint32_t m3 = o.n_semi ? vb[o.n_opaque].eyes : 0;
    CHECK(m3 == 1u || m3 == 2u, "un ojo ve la cara m3=%u", m3);
    P.eyes[0][2] = 0; P.eyes[1][2] = 0;
    /* dos caras: siempre ambos ojos */
    e.flags = NS_EF_SEMI;
    CHECK(build(&e, 1, vb, runs, keys, &o) == 0 && vb[o.n_opaque].eyes == 3u, "dos caras");
}

static void t_limits(void) {
    NsGpuFxVert vb[16]; NsGpuFxRun runs[4]; uint64_t keys[4]; NsGpuFxOut o;
    CHECK(ns_gpu_fx_build(NULL, 0, &P, vb, 16, runs, 4, keys, 4, &o) == 0 && o.n_opaque == 0 && o.n_semi == 0, "vacio");
    NsEffect e = quad(0, 0, 3000, 100);
    e.p[2][1] = NAN;
    CHECK(build(&e, 1, vb, runs, keys, &o) == 0 && o.n_skipped == 1 && o.n_keys == 0, "no finito descartado");
    CHECK(ns_gpu_fx_build(&e, 1, NULL, vb, 16, runs, 4, keys, 4, &o) < 0, "sin parametros");
    NsGpuFxParams bad = P; bad.n_eyes = 0;
    CHECK(ns_gpu_fx_build(&e, 1, &bad, vb, 16, runs, 4, keys, 4, &o) < 0, "sin ojos");
    /* capacidad: nunca escribe fuera */
    NsEffect many[10];
    for (int i = 0; i < 10; i++) { many[i] = quad(0, 0, 3000, 50); many[i].idx = (uint16_t)i; many[i].flags = NS_EF_SEMI; }
    NsGpuFxVert small[12]; NsGpuFxRun r2[10]; uint64_t k2[10];
    CHECK(ns_gpu_fx_build(many, 10, &P, small, 12, r2, 10, k2, 10, &o) == 0 && o.n_opaque + o.n_semi <= 12 && o.n_skipped >= 8, "capacidad acotada semi=%u skip=%u", o.n_semi, o.n_skipped);
}

/* Del extractor al empaquetador: ns_fx_export -> ns_gpu_fx_build conserva la identidad (prim, paquete, segmento) */
static void t_export_roundtrip(void) {
    NsFxScene *sc = ns_fx_scene_create(4);
    sc->n_items = 1;
    NsFxItem *it = &sc->items[0];
    memset(it, 0, sizeof *it);
    it->shape = NSFX_SH_QUAD; it->nv = 4; it->prim = 0x8012A000u; it->idx = 7; it->sub = 3; it->flags = NSFX_F_SEMI; it->abr = 1;
    for (int v = 0; v < 4; v++) { it->pos[v][0] = (float)(v & 1) * 100; it->pos[v][1] = 0; it->pos[v][2] = 2000.0f + (float)(v >> 1) * 100; }
    NsEffect fx[2]; int tr;
    CHECK(ns_fx_export(sc, 1, fx, 2, &tr) == 1 && fx[0].idx == 7 && fx[0].sub == 3 && fx[0].src_key == 0x8012A000u, "export con idx y sub");
    CHECK(ns_gpu_fx_key(&fx[0]) == ns_fx_item_key(it), "misma clave que ns_fx_item_key");
    ns_fx_scene_destroy(sc);
}


/* Niebla en los efectos: color *= 1 - clamp((z - inicio)/largo), con z = profundidad de la cámara del juego (la fórmula de las mallas) */
static void t_fog(void) {
    NsFog fog; memset(&fog, 0, sizeof fog);
    fog.active = 1; fog.start = 1000; fog.len = 2000;
    P.fog = &fog;
    NsEffect fx[4];
    fx[0] = quad(0, 0, 500, 50);  fx[0].idx = 1; fx[0].flags = NS_EF_TEXTURED;                                   /* antes del inicio: intacto */
    fx[1] = quad(0, 0, 2000, 50); fx[1].idx = 2; fx[1].flags = NS_EF_TEXTURED;                                   /* mitad: 1 - 0.5 */
    fx[2] = quad(0, 0, 9000, 50); fx[2].idx = 3; fx[2].flags = NS_EF_TEXTURED;                                   /* pasado el final: negro */
    fx[3] = quad(0, 0, 2000, 50); fx[3].idx = 4; fx[3].flags = NS_EF_SEMI | NS_EF_TEXTURED; fx[3].abr = 1;       /* aditivo: se apaga */
    NsGpuFxVert vb[96]; NsGpuFxRun runs[8]; uint64_t keys[8]; NsGpuFxOut o;
    CHECK(build(fx, 4, vb, runs, keys, &o) == 0, "build niebla");
    CHECK(fabsf(vb[0].col[0] - 128.0f) < 1e-3f, "sin niebla antes del inicio %f", vb[0].col[0]);
    CHECK(fabsf(vb[6].col[0] - 64.0f) < 1e-3f && fabsf(vb[6].col[2] - 64.0f) < 1e-3f, "niebla a medias %f", vb[6].col[0]);
    CHECK(fabsf(vb[12].col[1]) < 1e-3f, "niebla total %f", vb[12].col[1]);
    float want = 128.0f * (1.0f - ns_gpu_fog_f(&fog, 2000.0f));
    CHECK(fabsf(vb[6].col[0] - want) < 1e-3f, "misma fórmula que las mallas");
    int found = 0;
    for (uint32_t i = 0; i < o.n_semi; i++) if (fabsf(vb[o.n_opaque + i].col[0] - 64.0f) < 1e-3f) found = 1;
    CHECK(found, "el semitransparente también lleva niebla");
    /* cartel: toda la cara a la profundidad del centro */
    NsEffect b; memset(&b, 0, sizeof b);
    b.kind = NS_EK_BILLBOARD; b.nv = 1; b.src_key = 0x80100000u; b.ref_h = 320; b.p[0][2] = 2000; b.box[0] = -10; b.box[1] = -10; b.box[2] = 10; b.box[3] = 10;
    for (int v = 0; v < 4; v++) for (int c = 0; c < 3; c++) b.rgb[v][c] = 128;
    b.flags = NS_EF_TEXTURED;
    CHECK(build(&b, 1, vb, runs, keys, &o) == 0 && fabsf(vb[0].col[0] - 64.0f) < 1e-3f && fabsf(vb[5].col[0] - 64.0f) < 1e-3f, "cartel con niebla");
    /* sin niebla activa: intacto */
    fog.active = 0;
    CHECK(build(fx, 4, vb, runs, keys, &o) == 0 && fabsf(vb[6].col[0] - 128.0f) < 1e-3f, "niebla inactiva");
    P.fog = NULL;
}

/* Una sola pasada semitransparente: mallas y efectos intercalados por profundidad */
static void t_merge(void) {
    /* mallas: profundidades 9 8 5 5 2 (de atrás adelante), modos 1 1 2 2 1 */
    float md[5] = {9, 8, 5, 5, 2}; uint8_t ma[5] = {1, 1, 2, 2, 1};
    NsGpuFxSemiItem fx[3] = {{8.5f, 100, 6, 1}, {5.0f, 106, 6, 2}, {1.0f, 112, 6, 1}};
    NsGpuSemiRun r[16]; int tr = 0;
    size_t n = ns_gpu_semi_merge(md, ma, 5, fx, 3, r, 16, &tr);
    /* orden esperado: M0(9) F0(8.5) M1(8) M2(5) M3(5) F1(5) M4(2) F2(1) -> M0 | F0 | M1 | M2M3 | F1 | M4 | F2 */
    CHECK(n == 7 && !tr, "7 tramos n=%zu", n);
    CHECK(r[0].kind == 0 && r[0].start == 0 && r[0].count == 1 && r[0].abr == 1, "M0");
    CHECK(r[1].kind == 1 && r[1].start == 100 && r[1].count == 6, "F0 entre medias");
    CHECK(r[2].kind == 0 && r[2].start == 1 && r[2].count == 1, "M1");
    CHECK(r[3].kind == 0 && r[3].start == 2 && r[3].count == 2 && r[3].abr == 2, "M2M3 unidas");
    CHECK(r[4].kind == 1 && r[4].start == 106 && r[4].abr == 2, "F1 detrás de la malla a igual profundidad");
    CHECK(r[5].kind == 0 && r[5].start == 4 && r[6].kind == 1 && r[6].start == 112, "cola");
    /* efectos contiguos del mismo modo se unen */
    NsGpuFxSemiItem f2[2] = {{4, 0, 6, 1}, {3, 6, 6, 1}};
    n = ns_gpu_semi_merge(NULL, NULL, 0, f2, 2, r, 16, &tr);
    CHECK(n == 1 && r[0].count == 12, "efectos unidos");
    /* sin efectos: sólo mallas; sin nada: 0 */
    n = ns_gpu_semi_merge(md, ma, 5, NULL, 0, r, 16, &tr);
    CHECK(n == 3 && r[0].count == 2 && r[1].count == 2 && r[2].count == 1, "sólo mallas");
    CHECK(ns_gpu_semi_merge(NULL, NULL, 0, NULL, 0, r, 16, &tr) == 0 && !tr, "vacío");
    /* tope de tramos */
    n = ns_gpu_semi_merge(md, ma, 5, fx, 3, r, 3, &tr);
    CHECK(n == 3 && tr, "truncado");
    /* integración: ordenación con profundidad de malla + efectos con la misma fila de profundidad */
    NsGpuFxSemiItem items[8]; NsGpuFxVert vb[64]; NsGpuFxRun runs[8]; uint64_t keys[8]; NsGpuFxOut o;
    NsEffect e[3];
    for (int i = 0; i < 3; i++) { e[i] = quad(0, 0, 2000.0f * (float)(i + 1), 50); e[i].idx = (uint8_t)(i + 1); e[i].flags = NS_EF_SEMI; e[i].abr = (uint8_t)(i & 1); }
    P.has_depth_row = 1; P.depth_row[2] = 1.0f;   /* profundidad = z del mundo VR */
    CHECK(ns_gpu_fx_build_ex(e, 3, &P, vb, 64, runs, 8, keys, 8, items, 8, &o) == 0 && o.n_sitems == 3, "items");
    CHECK(items[0].depth > items[1].depth && items[1].depth > items[2].depth && fabsf(items[0].depth - 6.0f) < 1e-4f, "items ordenados de atrás adelante (%f)", items[0].depth);
    CHECK(items[0].start == o.n_opaque && items[1].start == items[0].start + 6, "items contiguos");
    P.has_depth_row = 0;
    /* ordenación de mallas con profundidad de salida */
    float cen[6] = {0, 0, 1, 0, 0, 3}; uint8_t ab[2] = {0, 1};
    NsGpuSemiSrc src = { 7, 0, 2, cen, ab, {0, 0, 1, 0} };
    NsGpuOrder ord[2]; uint8_t abo[2]; float dep[2];
    CHECK(ns_gpu_sort_semi_ex(&src, 1, ord, abo, dep, 2, NULL) == 2 && ord[0].tri == 1 && dep[0] == 3.0f && dep[1] == 1.0f && abo[0] == 1, "sort con profundidad");
}


/* ---- comodidad VR: efectos cerca de la cabeza ---- */
static NsEffect board(float x, float y, float z, float half_px, uint8_t role, uint16_t flags) {
    NsEffect e; memset(&e, 0, sizeof e);
    e.kind = NS_EK_BILLBOARD; e.nv = 4; e.flags = (uint16_t)(NS_EF_TEXTURED | NS_EF_SEMI | flags); e.abr = 1; e.role = role; e.src_key = 0x80300000u + (uint32_t)(z * 7.0f); e.tpage = 0x1a; e.clut = 0x3db1;
    e.p[0][0] = x; e.p[0][1] = y; e.p[0][2] = z; e.ref_h = 320.0f; e.box[0] = -half_px; e.box[1] = -half_px; e.box[2] = half_px; e.box[3] = half_px;
    for (int v = 0; v < 4; v++) for (int c = 0; c < 3; c++) e.rgb[v][c] = 128;
    return e;
}
/* mayor ángulo (grados) entre dos esquinas del primer cuadrilátero de la salida (vértices 0,1,2 y 5), visto desde la cabeza (origen) */
static float out_deg(const NsGpuFxVert *v) {
    const int idx[4] = {0, 1, 2, 5}; float u[4][3], best = 0;
    for (int i = 0; i < 4; i++) { const float *p = v[idx[i]].p; float l = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]); for (int c = 0; c < 3; c++) u[i][c] = p[c] / l; }
    for (int a = 0; a < 4; a++) for (int b = a + 1; b < 4; b++) { float d = u[a][0] * u[b][0] + u[a][1] * u[b][1] + u[a][2] * u[b][2]; if (d > 1) d = 1; float g = acosf(d) * 57.29578f; if (g > best) best = g; }
    return best;
}
static void t_comfort(void) {
    NsGpuFxVert vb[128], ref[128]; NsGpuFxRun runs[16]; uint64_t keys[16]; NsGpuFxOut o;
    /* tope: 0 dentro de la cabeza, X a 0,3 m, 90 a 1 m, sin tope más allá; continuo y creciente */
    CHECK(ns_gpu_fx_comfort_cap_deg(0.10f, 20, 0) == 0.0f && ns_gpu_fx_comfort_cap_deg(0.12f, 20, 0) == 0.0f, "dentro de la cabeza: descartar");
    CHECK(fabsf(ns_gpu_fx_comfort_cap_deg(0.3f, 20, 0) - 20.0f) < 1e-3f && fabsf(ns_gpu_fx_comfort_cap_deg(1.0f - 1e-4f, 20, 0) - 90.0f) < 0.1f && ns_gpu_fx_comfort_cap_deg(1.5f, 20, 0) > 1e8f, "tope 20 / 90 / libre");
    { float prev = -1; int mono = 1; for (float d = 0.121f; d < 1.0f; d += 0.01f) { float c = ns_gpu_fx_comfort_cap_deg(d, 20, 0); if (c < prev - 1e-4f) mono = 0; prev = c; } CHECK(mono, "tope creciente con la distancia"); }
    CHECK(ns_gpu_fx_comfort_cap_deg(0.05f, 0, 0) > 1e8f, "X = 0: desactivado");

    /* desactivado (comfort_deg = 0): el cartel de 320 px a 0,25 m no se toca */
    NsEffect big = board(0, 0, 250, 160, NSFX_ROLE_LIGHT, 0);
    CHECK(build(&big, 1, ref, runs, keys, &o) == 0 && o.n_opaque + o.n_semi > 0 && o.n_shrunk == 0, "sin comodidad");
    float raw = out_deg(&ref[o.n_opaque]);
    CHECK(raw > 45.0f, "el cartel de pantalla completa a 0,25 m ocupa %.0f grados (el motor lo hace asi)", raw);
    CHECK(o.n_near == 1 && o.n_rec == 1 && o.rec[0].role == NSFX_ROLE_LIGHT && o.rec[0].action == NSG_FXN_KEPT && fabsf(o.rec[0].dist_m - 0.25f) < 1e-3f, "registro del cercano: n=%u", o.n_near);

    P.comfort_deg = 20; P.comfort_dmin_m = 0; P.oncam_drop_m = 1.5f;
    /* reducido alrededor del centro: tope respetado, centro igual */
    CHECK(build(&big, 1, vb, runs, keys, &o) == 0 && o.n_shrunk == 1 && o.n_keys == 1, "reducido");
    float cap = ns_gpu_fx_comfort_cap_deg(0.25f, 20, 0), got = out_deg(&vb[o.n_opaque]);
    CHECK(got <= cap * 1.01f && got > cap * 0.8f, "tamano angular %.1f <= tope %.1f (antes %.0f)", got, cap, raw);
    { const NsGpuFxVert *a = &vb[o.n_opaque], *r = &ref[0]; float cx = 0, rx = 0, cz = 0, rz = 0; const int ix[4] = {0, 1, 2, 5};
      NsGpuFxOut o0; build(&big, 1, ref, runs, keys, &o0); r = &ref[o0.n_opaque];
      for (int i = 0; i < 4; i++) { cx += a[ix[i]].p[0] / 4; rx += r[ix[i]].p[0] / 4; cz += a[ix[i]].p[2] / 4; rz += r[ix[i]].p[2] / 4; }
      CHECK(fabsf(cx - rx) < 1e-4f && fabsf(cz - rz) < 1e-4f, "el centro no se mueve (%f %f)", cx - rx, cz - rz); }
    CHECK(o.rec[0].action == NSG_FXN_SHRUNK && o.rec[0].deg_raw > 45.0f && o.rec[0].deg_after <= cap * 1.01f, "registro: reducido %.0f->%.0f", o.rec[0].deg_raw, o.rec[0].deg_after);

    /* nace dentro de la cabeza: descartado pero confirmado (la clave entra: el motor no lo redibuja) */
    NsEffect in = board(0, 0, 50, 8, NSFX_ROLE_SPARK, 0);
    CHECK(build(&in, 1, vb, runs, keys, &o) == 0 && o.n_drop_inside == 1 && o.n_opaque + o.n_semi == 0 && o.n_keys == 1 && keys[0] == ns_gpu_fx_key(&in), "dentro de la cabeza: descartado y confirmado");

    /* pegado a la cámara del motor: descartado a < 1,5 m, se conserva más lejos y si oncam_drop_m = 0 */
    NsEffect fog = board(0, 0, 1000, 40, NSFX_ROLE_VIEW_OVERLAY, NS_EF_ON_CAMERA), far = board(0, 0, 2000, 40, NSFX_ROLE_VIEW_OVERLAY, NS_EF_ON_CAMERA);
    CHECK(build(&fog, 1, vb, runs, keys, &o) == 0 && o.n_drop_oncam == 1 && o.n_opaque + o.n_semi == 0 && o.n_keys == 1, "ON_CAMERA a 1 m: descartado y confirmado");
    CHECK(o.n_rec == 1 && o.rec[0].action == NSG_FXN_DROP_ONCAM && o.rec[0].screen == 1 && o.n_near == 0, "ON_CAMERA descartado queda en el registro aunque no sea < 0,5 m");
    CHECK(build(&far, 1, vb, runs, keys, &o) == 0 && o.n_drop_oncam == 0 && o.n_semi > 0, "ON_CAMERA a 2 m se conserva");
    NsEffect scr = board(0, 0, 2000, 40, NSFX_ROLE_VIEW_OVERLAY, 0);   /* papel de superficie de vista sin la bandera */
    scr.p[0][2] = 1000;
    CHECK(build(&scr, 1, vb, runs, keys, &o) == 0 && o.n_drop_oncam == 1, "papel VIEW_OVERLAY tambien es de pantalla");
    P.oncam_drop_m = 0;
    CHECK(build(&fog, 1, vb, runs, keys, &o) == 0 && o.n_drop_oncam == 0 && o.n_semi > 0, "oncam_drop_m = 0: se conserva");
    P.oncam_drop_m = 1.5f;

    /* lo lejano no se toca: halo de 3 m a 3 m, y un halo grande a 1,2 m (sin tope) */
    NsEffect halo = board(0, 0, 3000, 160, NSFX_ROLE_LIGHT, 0), mid = board(0, 0, 1200, 160, NSFX_ROLE_LIGHT, 0), two[2] = {halo, mid};
    NsGpuFxVert vb2[128];
    P.comfort_deg = 0; CHECK(build(two, 2, ref, runs, keys, &o) == 0, "ref");
    P.comfort_deg = 20; CHECK(build(two, 2, vb2, runs, keys, &o) == 0 && o.n_shrunk == 0 && o.n_drop_inside == 0 && memcmp(ref, vb2, sizeof(NsGpuFxVert) * (o.n_opaque + o.n_semi)) == 0, "a mas de 1 m: identico");

    /* poca profundidad: cartel en píxeles con z < 32 (referencia 1000): el tamaño de mundo se infla, la comodidad lo acota y se cuenta */
    NsEffect zr = board(0, 0, 20, 160, NSFX_ROLE_LIGHT, 0);
    zr.p[0][2] = 20; /* 2 cm: z menor que el mínimo fiable; el centro queda dentro de la cabeza */
    CHECK(build(&zr, 1, vb, runs, keys, &o) == 0 && o.n_zref == 1 && o.n_drop_inside == 1, "z < zmin: cuenta de referencia usada y descartado por estar dentro de la cabeza");
    zr.p[0][2] = 300; zr.p[0][0] = 0; P.px_z_min = 400;   /* cartel de 0,3 m con z < zmin => 1000 en lugar de 300: 3.3x mas grande */
    CHECK(build(&zr, 1, vb, runs, keys, &o) == 0 && o.n_zref == 1 && o.n_shrunk == 1 && out_deg(&vb[o.n_opaque]) <= ns_gpu_fx_comfort_cap_deg(0.3f, 20, 0) * 1.01f, "cartel inflado por la profundidad de referencia queda acotado");
    P.px_z_min = 0;

    /* superficies del entorno (agua, paneles) no son sprites: ni se reducen ni se descartan */
    NsEffect wq = quad(0, 0, 400, 300); wq.role = NSFX_ROLE_WATER_SURFACE; wq.flags = NS_EF_SEMI; wq.abr = 1;
    P.comfort_deg = 0; build(&wq, 1, ref, runs, keys, &o);
    P.comfort_deg = 20; CHECK(build(&wq, 1, vb, runs, keys, &o) == 0 && o.n_shrunk == 0 && memcmp(ref, vb, sizeof(NsGpuFxVert) * (o.n_opaque + o.n_semi)) == 0, "superficie de agua intacta");
    NsEffect pq = quad(0, 0, 400, 300); pq.role = NSFX_ROLE_SPARK; pq.flags = NS_EF_SEMI; pq.abr = 1;
    CHECK(build(&pq, 1, vb, runs, keys, &o) == 0 && o.n_shrunk == 1, "el mismo polígono con papel de chispa si se reduce");

    /* registro: texto con papel, ON_CAMERA, distancia y ángulos; y el acumulador */
    { NsEffect set[3] = {board(0, 0, 250, 160, NSFX_ROLE_LIGHT, 0), board(0, 0, 400, 40, NSFX_ROLE_DROPLET, NS_EF_ON_CAMERA), board(0, 0, 450, 40, NSFX_ROLE_SMOKE, 0)};
      NsGpuFxNearAcc acc; ns_gpu_fx_near_reset(&acc);
      CHECK(build(set, 3, vb, runs, keys, &o) == 0 && o.n_near == 3 && o.n_near_oncam == 1 && o.n_near_screen == 1 && o.n_drop_oncam == 1 && o.n_shrunk == 1, "tres cercanos (<0,5 m: %u) 1 pegado", o.n_near);
      float h0[3] = {0, 1.6f, 0}, h1[3] = {1.0f, 1.6f, 0}, r0[3] = {1, 0, 0}, r1[3] = {0.7071f, 0, 0.7071f};
      ns_gpu_fx_near_accum(&acc, &o, h0, r0, 10.0f);
      ns_gpu_fx_near_accum(&acc, &o, h1, r1, 10.5f);   /* 2 m/s y 90 grados en 0,5 s */
      CHECK(acc.frames == 2 && acc.frames_near == 2 && acc.frames_moving == 1 && acc.frames_moving_near == 1 && fabsf(acc.max_speed_mps - 2.0f) < 0.01f && fabsf(acc.max_turn_dps - 90.0f) < 1.0f, "acumulador: mov=%u v=%.2f w=%.0f", acc.frames_moving, acc.max_speed_mps, acc.max_turn_dps);
      char txt[1536]; size_t n = ns_gpu_fx_near_describe(&acc, txt, sizeof txt);
      CHECK(n > 100 && n < sizeof txt && strstr(txt, "rol=light") && strstr(txt, "rol=droplet ") && strstr(txt, "ON_CAMERA") && strstr(txt, "reducido") && strstr(txt, "descartado_pegado") && strstr(txt, "en_movimiento=1") && !strchr(txt, '\n'), "texto del registro: %s", txt);
      CHECK(ns_gpu_fx_near_describe(&acc, txt, 40) < 40, "descripcion acotada al buffer");
      ns_gpu_fx_near_reset(&acc); CHECK(acc.frames == 0 && acc.n_rec == 0, "reset");
    }
    /* roles: la tabla de nombres coincide con la del extractor */
    for (int k = 0; k < NSFX_ROLE_COUNT; k++) CHECK(strcmp(ns_gpu_fx_role_name(k), ns_fx_role_name(k)) == 0, "papel %d: %s vs %s", k, ns_gpu_fx_role_name(k), ns_fx_role_name(k));
    /* el papel del extractor llega al efecto exportado */
    { NsFxScene *sc = ns_fx_scene_create(0); NsFxItem *it = &sc->items[0]; memset(it, 0, sizeof *it); it->shape = NSFX_SH_BILLBOARD; it->nv = 4; it->role = NSFX_ROLE_VIEW_OVERLAY; it->flags = NSFX_F_VIEWLOCKED | NSFX_F_PXSIZE; sc->n_items = 1; sc->cam.proj_h = 320; sc->cam.y_scale = 1;
      NsEffect fx[1]; int tr = 0; CHECK(ns_fx_export(sc, 1, fx, 1, &tr) == 1 && fx[0].role == NSFX_ROLE_VIEW_OVERLAY && (fx[0].flags & NS_EF_ON_CAMERA), "export con papel y ON_CAMERA");
      ns_fx_scene_destroy(sc); }
}

/* Tamaño en mundo (NS_EF_WORLD_SIZE): no depende de ninguna profundidad; red de seguridad mínima; nada se encoge ni se quita si está bien colocado */
static void t_world_size(void) {
    NsGpuFxVert vb[64]; NsGpuFxRun runs[8]; uint64_t keys[8]; NsGpuFxOut o;
    /* halo de 2 m (semiextensión 1000 unidades) a 8 m delante, 3 cm delante de la cámara y DETRÁS de la cámara: siempre 2 m de lado, nunca 2000 m */
    const float zs[4] = {8000, 400, 30, -9000};
    for (int i = 0; i < 4; i++) {
        NsEffect e = board(0, 0, zs[i], 0, NSFX_ROLE_LIGHT, NS_EF_WORLD_SIZE);
        e.box[0] = -1000; e.box[1] = -1000; e.box[2] = 1000; e.box[3] = 1000;
        e.ref_h = 320;
        CHECK(build(&e, 1, vb, runs, keys, &o) == 0 && o.n_opaque + o.n_semi > 0 && o.n_zref == 0, "halo z=%.0f", zs[i]);
        const NsGpuFxVert *v = &vb[o.n_opaque];
        float w = v[1].p[0] - v[0].p[0], h = fabsf(v[0].p[1] - v[2].p[1]);
        CHECK(fabsf(w - 2.0f) < 1e-4f && fabsf(h - 2.0f) < 1e-4f, "z=%.0f: lado %.3f x %.3f m (2 m en cualquier profundidad)", zs[i], w, h);
    }
    /* centro y orientación a la cabeza intactos, y con ref_h = 0 también vale (no depende de la proyección) */
    { NsEffect e = board(500, -200, 3000, 0, NSFX_ROLE_LIGHT, NS_EF_WORLD_SIZE); e.box[0] = -50; e.box[1] = -40; e.box[2] = 70; e.box[3] = 40; e.ref_h = 0;
      CHECK(build(&e, 1, vb, runs, keys, &o) == 0 && o.n_opaque + o.n_semi > 0, "ref_h = 0 en modo mundo");
      const NsGpuFxVert *v = &vb[o.n_opaque];
      CHECK(fabsf(v[0].p[0] - (0.5f - 0.05f)) < 1e-5f && fabsf(v[1].p[0] - (0.5f + 0.07f)) < 1e-5f && fabsf(v[0].p[1] - 0.24f) < 1e-5f && fabsf(v[2].p[1] - 0.16f) < 1e-5f, "semiextensiones asimétricas en mundo"); }
    /* cinta: anchura en mundo */
    { NsEffect e; memset(&e, 0, sizeof e); e.kind = NS_EK_QUAD; e.nv = 2; e.flags = NS_EF_LINE | NS_EF_WORLD_SIZE | NS_EF_SEMI; e.abr = 1; e.src_key = 0x80400000u; e.width_px = 100; /* 0,1 m */
      e.p[0][0] = 0; e.p[0][1] = 0; e.p[0][2] = -5000; e.p[1][0] = 0; e.p[1][1] = 1000; e.p[1][2] = -5000;   /* detrás de la cámara */
      CHECK(build(&e, 1, vb, runs, keys, &o) == 0 && o.n_opaque + o.n_semi > 0, "cinta");
      const NsGpuFxVert *v = &vb[o.n_opaque];
      float w = sqrtf((v[1].p[0] - v[0].p[0]) * (v[1].p[0] - v[0].p[0]) + (v[1].p[1] - v[0].p[1]) * (v[1].p[1] - v[0].p[1]) + (v[1].p[2] - v[0].p[2]) * (v[1].p[2] - v[0].p[2]));
      CHECK(fabsf(w - 0.1f) < 1e-4f, "anchura de la cinta %.4f m (0,1 en cualquier profundidad)", w); }
    /* red de seguridad: sin comfort_deg ni oncam_drop_m solo se descarta (confirmado) lo que nace a menos de drop_inside_m; un pegado a la cámara a 0,5 m queda tal cual */
    P.comfort_deg = 0; P.oncam_drop_m = 0; P.drop_inside_m = 0.05f;
    NsEffect in = board(0, 0, 30, 0, NSFX_ROLE_SPARK, NS_EF_WORLD_SIZE), ok5 = board(0, 0, 60, 0, NSFX_ROLE_SPARK, NS_EF_WORLD_SIZE);
    in.box[0] = in.box[1] = -10; in.box[2] = in.box[3] = 10; ok5.box[0] = ok5.box[1] = -10; ok5.box[2] = ok5.box[3] = 10; in.ref_h = ok5.ref_h = 320;
    CHECK(build(&in, 1, vb, runs, keys, &o) == 0 && o.n_drop_inside == 1 && o.n_keys == 1 && o.n_opaque + o.n_semi == 0, "a 3 cm: descartado y confirmado");
    CHECK(build(&ok5, 1, vb, runs, keys, &o) == 0 && o.n_drop_inside == 0 && o.n_opaque + o.n_semi > 0, "a 6 cm: se dibuja");
    NsEffect fog = board(0, 0, 500, 0, NSFX_ROLE_VIEW_OVERLAY, NS_EF_ON_CAMERA | NS_EF_WORLD_SIZE); fog.box[0] = fog.box[1] = -400; fog.box[2] = fog.box[3] = 400; fog.ref_h = 320;
    NsGpuFxVert vr[64]; P.drop_inside_m = 0; CHECK(build(&fog, 1, vr, runs, keys, &o) == 0, "ref");
    P.drop_inside_m = 0.05f;
    CHECK(build(&fog, 1, vb, runs, keys, &o) == 0 && o.n_shrunk == 0 && o.n_drop_oncam == 0 && memcmp(vr, vb, sizeof(NsGpuFxVert) * (o.n_opaque + o.n_semi)) == 0 && o.n_opaque + o.n_semi > 0,
          "pegado a la cámara a 0,5 m, colocado como el motor: ni se encoge ni se quita");
}

/* ---- seguimiento: lo pegado a la cabeza se detecta y descarta; lo anclado al mundo no se mueve con la cabeza ---- */
static void head_pose(int t, float head[3], float right[3], float up[3]) {   /* la cabeza gira 3 grados y avanza 5 mm por fotograma */
    float yaw = (float)t * 3.0f * 3.14159265f / 180.0f;
    head[0] = 0.005f * (float)t; head[1] = 1.6f; head[2] = 0.0f;
    right[0] = cosf(yaw); right[1] = 0; right[2] = -sinf(yaw); up[0] = 0; up[1] = 1; up[2] = 0;
}
static void vr_to_engine(const float v[3], float e[3]) { e[0] = v[0] / 0.001f; e[1] = -v[1] / 0.001f; e[2] = v[2] / 0.001f; }
static void t_follow(void) {
    NsGpuFxTrack *trk = ns_gpu_fx_track_create();
    CHECK(trk != NULL, "crear el seguimiento");
    P.track = trk; P.hold_near_m = 0.3f; P.drop_oncam = 1; P.n_eyes = 1;
    enum { N = 11, FR = 70 };
    NsEffect fx[N];
    int drawn_last[N] = {0}, ever_drawn[N] = {0}, drawn_first[N] = {0}, drawn_after_reset = -1;
    float cen[N][3], cen_prev[N][3]; int have_prev[N] = {0};
    float world_dev[N] = {0};
    NsGpuFxFollowAcc acc; ns_gpu_fx_follow_reset(&acc);
    NsGpuFxOut o;
    for (int t = 0; t < FR; t++) {
        float head[3], right[3], up[3], fwd[3];
        head_pose(t, head, right, up);
        fwd[0] = right[1] * up[2] - right[2] * up[1]; fwd[1] = right[2] * up[0] - right[0] * up[2]; fwd[2] = right[0] * up[1] - right[1] * up[0];
        memcpy(P.head, head, sizeof head); memcpy(P.head_right, right, sizeof right); memcpy(P.head_up, up, sizeof up); memcpy(P.eyes[0], head, sizeof head);
        float vr[3], en[3];
        /* 0: de mundo, a 2 m, quieto */
        fx[0] = quad(2000, -1600, 2500, 40); fx[0].src_key = 0x80100000u;
        /* 1: pegado a la cabeza a 0,5 m de frente y grande (20 cm: > 10 grados): el motor lo recoloca cada fotograma en el mismo sitio de la vista */
        for (int a = 0; a < 3; a++) vr[a] = head[a] + 0.10f * right[a] + 0.0f * up[a] + 0.5f * fwd[a];
        vr_to_engine(vr, en); fx[1] = quad(en[0], en[1], en[2], 100); fx[1].src_key = 0x80100100u;
        /* 2: de mundo, a 20 cm de la cabeza al principio (cerca): retenido hasta demostrar que es del mundo */
        {
            float h0[3], r0[3], u0[3];
            head_pose(0, h0, r0, u0);
            float f0[3] = {r0[1] * u0[2] - r0[2] * u0[1], r0[2] * u0[0] - r0[0] * u0[2], r0[0] * u0[1] - r0[1] * u0[0]};
            for (int a = 0; a < 3; a++) vr[a] = h0[a] + 0.2f * f0[a];
            vr_to_engine(vr, en); fx[2] = quad(en[0], en[1], en[2], 20); fx[2].src_key = 0x80100200u;
        }
        /* 3: pegado a la cámara por bandera, no es de pantalla */
        fx[3] = board(0, 0, 3000, 40, NSFX_ROLE_LIGHT, NS_EF_ON_CAMERA | NS_EF_WORLD_SIZE); fx[3].box[0] = fx[3].box[1] = -40; fx[3].box[2] = fx[3].box[3] = 40; fx[3].src_key = 0x80100300u;
        /* 4: (a 0,5 m, grande) pegado a la cámara por bandera y pegado a la cabeza de verdad, pero recubrimiento de pantalla: intencionado, se conserva (y queda en el registro) */
        for (int a = 0; a < 3; a++) vr[a] = head[a] + 0.5f * fwd[a];
        vr_to_engine(vr, en); fx[4] = quad(en[0], en[1], en[2], 100); fx[4].src_key = 0x80100400u; fx[4].role = NSFX_ROLE_VIEW_OVERLAY; fx[4].flags |= NS_EF_ON_CAMERA;
        /* 5: cerca de la cabeza y vagando (colocado cada fotograma a 0,2 m en una dirección distinta del mundo: ni de cabeza ni de mundo) */
        { float ang = (float)t * 1.7f; vr[0] = head[0] + 0.2f * cosf(ang); vr[1] = head[1] + 0.05f * sinf(ang * 0.7f); vr[2] = head[2] + 0.2f * sinf(ang); vr_to_engine(vr, en); fx[5] = quad(en[0], en[1], en[2], 20); fx[5].src_key = 0x80100500u; }
        /* 6: enemigo lejano que camina por su cuenta (2,5 m): legítimo */
        { vr[0] = 1.0f + 0.01f * (float)t; vr[1] = 1.5f; vr[2] = 2.5f; vr_to_engine(vr, en); fx[6] = quad(en[0], en[1], en[2], 40); fx[6].src_key = 0x80100600u; }
        /* 7: pegado a la cabeza a 3 m (cielo que se mueve con el ojo): se detecta aunque esté lejos */
        for (int a = 0; a < 3; a++) vr[a] = head[a] - 0.4f * right[a] + 0.3f * up[a] + 3.0f * fwd[a];
        vr_to_engine(vr, en); fx[7] = quad(en[0], en[1], en[2], 100); fx[7].src_key = 0x80100700u;
        /* 8: M5: partícula pequeña (4 cm, < 10 grados) que va con la cabeza a 0,4 m (vaho): SIGUE pero se conserva */
        for (int a = 0; a < 3; a++) vr[a] = head[a] - 0.05f * right[a] + 0.4f * fwd[a];
        vr_to_engine(vr, en); fx[8] = quad(en[0], en[1], en[2], 20); fx[8].src_key = 0x80100800u;
        /* 9: M6: ON_CAMERA sin papel de pantalla a 0,5 m, quieto en el mundo del motor (no sigue): a menos de 1 m se descarta */
        { float h0[3], r0[3], u0[3]; head_pose(0, h0, r0, u0);
          float f0[3] = {r0[1] * u0[2] - r0[2] * u0[1], r0[2] * u0[0] - r0[0] * u0[2], r0[0] * u0[1] - r0[1] * u0[0]};
          for (int a = 0; a < 3; a++) vr[a] = h0[a] + 0.5f * f0[a];
          vr_to_engine(vr, en); fx[9] = quad(en[0], en[1], en[2], 40); fx[9].src_key = 0x80100900u; fx[9].flags |= NS_EF_ON_CAMERA; }
        /* 10: M6: ON_CAMERA sin papel de pantalla, a 2 m y pegado a la cabeza: SIGUE => se descarta */
        for (int a = 0; a < 3; a++) vr[a] = head[a] + 0.1f * right[a] + 2.0f * fwd[a];
        vr_to_engine(vr, en); fx[10] = quad(en[0], en[1], en[2], 100); fx[10].src_key = 0x80100A00u; fx[10].flags |= NS_EF_ON_CAMERA;
        if (t == 40) ns_gpu_fx_track_request_reset();   /* B6: recentrado: el seguimiento olvida todo antes de procesar este fotograma */
        NsGpuFxVert vb[12 * N + 12]; NsGpuFxRun runs[N + 1]; uint64_t keys[N + 1];
        CHECK(ns_gpu_fx_build(fx, N, &P, vb, 12 * N + 12, runs, N + 1, keys, N + 1, &o) == 0, "build");
        ns_gpu_fx_follow_accum(&acc, &o);
        /* quién se dibuja: los que no están en fol[] */
        int drawn[N], rank = 0;
        for (int i = 0; i < N; i++) {
            drawn[i] = 1;
            for (uint32_t k = 0; k < o.n_fol; k++) if (o.fol[k].src_key == fx[i].src_key && o.fol[k].oncam != 2) drawn[i] = 0;
        }
        for (int i = 0; i < N; i++) {
            if (!drawn[i]) continue;
            const NsGpuFxVert *v = &vb[6 * rank++];
            float c[3] = {0, 0, 0};
            for (int k = 0; k < 6; k++) for (int a = 0; a < 3; a++) c[a] += v[k].p[a] / 6.0f;
            memcpy(cen[i], c, sizeof c);
            if (have_prev[i] && (i == 0 || i == 2)) {
                float dd = 0;
                for (int a = 0; a < 3; a++) dd += (c[a] - cen_prev[i][a]) * (c[a] - cen_prev[i][a]);
                dd = sqrtf(dd);
                if (dd > world_dev[i]) world_dev[i] = dd;
            }
            memcpy(cen_prev[i], c, sizeof c); have_prev[i] = 1;
            ever_drawn[i] = 1;
            if (t == 0) drawn_first[i] = 1;
        }
        if (t == 40) drawn_after_reset = drawn[1];
        if (t == FR - 1) for (int i = 0; i < N; i++) drawn_last[i] = drawn[i];
    }
    CHECK(drawn_last[0] && ever_drawn[0] && world_dev[0] < 1e-4f, "de mundo a 2 m: siempre dibujado y en el MISMO sitio del mundo con la cabeza girando (desviacion %.6f m)", world_dev[0]);
    CHECK(!drawn_last[1], "pegado a la cabeza a 0,5 m y grande: descartado");
    CHECK(drawn_after_reset == 1, "B6: tras pedir el reinicio (recentrado) el fotograma siguiente olvida lo aprendido: se dibuja mientras no se demuestre de nuevo (%d)", drawn_after_reset);
    CHECK(!drawn_first[2] && drawn_last[2] && world_dev[2] < 1e-4f, "cercano de mundo: retenido al principio, luego dibujado y anclado (desviacion %.6f m)", world_dev[2]);
    CHECK(drawn_last[3] && ever_drawn[3], "M6: ON_CAMERA sin papel de pantalla, lejos (3 m) y quieto en el mundo: no basta la bandera, se dibuja");
    CHECK(!drawn_last[9] && !ever_drawn[9], "M6: ON_CAMERA sin papel de pantalla a menos de 1 m: se descarta");
    CHECK(!drawn_last[10], "M6: ON_CAMERA sin papel de pantalla que ademas SIGUE (a 2 m): se descarta");
    CHECK(!drawn_last[8], "j-render: particula pequena que va con la cabeza (< 10 grados): SIGUE confirmado -> se descarta (efecto de espacio de pantalla)");
    CHECK(drawn_last[4] && ever_drawn[4], "recubrimiento de pantalla (VIEW_OVERLAY) ON_CAMERA: se conserva");
    CHECK(!drawn_last[5], "cercano que vaga: descartado");
    CHECK(drawn_last[6] && ever_drawn[6], "enemigo lejano que se mueve solo: se dibuja");
    CHECK(!drawn_last[7], "j-render: pegado a la cabeza a 3 m: SIGUE confirmado -> se descarta (< 30 m)");
    CHECK(o.n_follow >= 3 && o.n_wander == 1 && o.n_tracked == N, "contadores: sigue=%u vaga=%u seguidos=%u", o.n_follow, o.n_wander, o.n_tracked);   /* sigue: 1, 7, 8 y 10; vaga: 5 */
    /* registro del detector: papel, origen, clave */
    int found1 = 0, found7 = 0, foundf = 0, found4 = 0;
    for (uint32_t k = 0; k < acc.n_rec; k++) {
        if (acc.rec[k].f.src_key == 0x80100100u && acc.rec[k].f.state == NSG_FS_FOLLOW && acc.rec[k].f.origin == NSG_ORIGIN_FX) found1 = 1;
        if (acc.rec[k].f.src_key == 0x80100700u) found7 = 1;   /* descartado (SIGUE a < 30 m): se registra */
        if (acc.rec[k].f.src_key == 0x80100900u && acc.rec[k].f.oncam == 1) foundf = 1;
        if (acc.rec[k].f.src_key == 0x80100400u && acc.rec[k].f.oncam == 2 && acc.rec[k].f.role == NSFX_ROLE_VIEW_OVERLAY) found4 = 1;
    }
    CHECK(found1 && found7 && foundf && found4, "el detector registra los descartes: %d %d %d %d", found1, found7, foundf, found4);
    char txt[2048]; size_t n = ns_gpu_fx_follow_describe(&acc, txt, sizeof txt);
    CHECK(n > 0 && strstr(txt, "fx-sigue") && strstr(txt, "origen=fx") && strstr(txt, "sigue") && strstr(txt, "origen=fx"), "texto del registro: %s", txt);
    /* origen relleno: la clave 0xF1... se marca como relleno */
    NsEffect fill = quad(2000, -1600, 2500, 40); fill.src_key = 0xF1000007u;
    { float head[3], right[3], up[3]; head_pose(0, head, right, up); }
    ns_gpu_fx_track_reset(trk);
    int fol_fill = 0;
    for (int t = 0; t < 12; t++) {
        float head[3], right[3], up[3], fwd[3], vr[3], en[3];
        head_pose(t, head, right, up);
        fwd[0] = right[1] * up[2] - right[2] * up[1]; fwd[1] = right[2] * up[0] - right[0] * up[2]; fwd[2] = right[0] * up[1] - right[1] * up[0];
        memcpy(P.head, head, sizeof head); memcpy(P.head_right, right, sizeof right); memcpy(P.head_up, up, sizeof up); memcpy(P.eyes[0], head, sizeof head);
        for (int a = 0; a < 3; a++) vr[a] = head[a] + 0.6f * fwd[a];
        vr_to_engine(vr, en); fill = quad(en[0], en[1], en[2], 100); fill.src_key = 0xF1000007u;
        NsGpuFxVert vb[24]; NsGpuFxRun runs[2]; uint64_t keys[2];
        CHECK(ns_gpu_fx_build(&fill, 1, &P, vb, 24, runs, 2, keys, 2, &o) == 0, "build relleno");
        if (o.n_follow_fill) fol_fill = 1;
    }
    CHECK(fol_fill && o.n_fol >= 1 && o.fol[0].origin == NSG_ORIGIN_FILL, "relleno pegado a la cabeza: detectado con origen relleno");
    ns_gpu_fx_track_destroy(trk);
    P.track = NULL; P.hold_near_m = 0; P.drop_oncam = 0;
}

/* M4: un efecto a 10 cm de la cabeza con la cabeza quieta (no hay forma de distinguir mundo de cabeza):
 *  - vida corta (< 12 fotogramas) a menos de 15 cm y sin demostrar que SIGUE: se dibuja;
 *  - mas viejo: retenido;
 *  - a los ~50 fotogramas sin indicio de SIGUE y quieto en el mundo del motor: MUNDO, se dibuja;
 *  - si luego la cabeza se mueve y resulta que va pegado a ella: vuelve a detectarse (SIGUE) y se descarta. */
static void t_hold_world(void) {
    NsGpuFxTrack *trk = ns_gpu_fx_track_create();
    P.track = trk; P.hold_near_m = 0.15f; P.drop_oncam = 1; P.n_eyes = 1;
    memset(P.head, 0, sizeof P.head); P.head_right[0] = 1; P.head_right[1] = P.head_right[2] = 0; P.head_up[0] = 0; P.head_up[1] = 1; P.head_up[2] = 0;
    memcpy(P.eyes[0], P.head, sizeof P.head);
    int drawn[100]; int follow_seen = 0;
    NsGpuFxOut o;
    for (int t = 0; t < 100; t++) {
        NsEffect e;
        if (t < 60) { e = quad(0, 0, 100, 60); }   /* quieto en el motor: (0, 0, 0,1 m) */
        else {   /* la cabeza gira 4 grados por fotograma; el efecto se recoloca cada fotograma a 10 cm delante (pegado a la cabeza) */
            float yaw = (float)(t - 59) * 4.0f * 3.14159265f / 180.0f, fwd[3] = {sinf(yaw), 0, cosf(yaw)};
            P.head_right[0] = cosf(yaw); P.head_right[1] = 0; P.head_right[2] = -sinf(yaw);
            float vr[3] = {0.1f * fwd[0], 0, 0.1f * fwd[2]}, en[3]; vr_to_engine(vr, en);
            e = quad(en[0], en[1], en[2], 60);
        }
        e.src_key = 0x80100B00u;
        NsGpuFxVert vb[24]; NsGpuFxRun runs[2]; uint64_t keys[2];
        CHECK(ns_gpu_fx_build(&e, 1, &P, vb, 24, runs, 2, keys, 2, &o) == 0, "build");
        drawn[t] = o.n_opaque + o.n_semi > 0;
        if (t >= 60 && o.n_follow) follow_seen = 1;
    }
    int young_ok = 1, held_ok = 1;
    for (int t = 0; t < 12; t++) if (!drawn[t]) young_ok = 0;
    for (int t = 12; t < 49; t++) if (drawn[t]) held_ok = 0;
    CHECK(young_ok, "M4: vida corta (< 12 fotogramas) a menos de 15 cm sin demostrar que sigue: se dibuja");
    CHECK(held_ok, "M4: a partir de 12 fotogramas, sin demostrar que esta en el mundo: retenido");
    CHECK(drawn[50] && drawn[51] && drawn[59], "M4: ~50 fotogramas quieto en el mundo del motor sin indicio de SIGUE: pasa a MUNDO y se dibuja (f49=%d f50=%d)", drawn[49], drawn[50]);
    CHECK(follow_seen && !drawn[99], "M4: si luego va pegado a la cabeza se detecta y se descarta (sigue=%d dibujado=%d)", follow_seen, drawn[99]);
    ns_gpu_fx_track_destroy(trk);
    P.track = NULL; P.hold_near_m = 0; P.drop_oncam = 0;
}

int main(void) {
    setup();
    t_poly();
    setup(); t_billboard(); t_min_px();
    setup(); t_raise_pull();
    setup(); t_ribbon();
    setup(); t_oneside();
    setup(); t_limits();
    setup(); t_fog();
    setup(); t_merge();
    t_export_roundtrip();
    setup(); t_comfort();
    setup(); t_world_size();
    setup(); t_follow();
    setup(); t_hold_world();
    printf("%s: %d comprobaciones, %d fallos\n", g_fail ? "FALLA" : "OK", g_checks, g_fail);
    return g_fail != 0;
}
