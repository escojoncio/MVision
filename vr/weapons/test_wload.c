/* Prueba del cargador de modelos originales: ./test_wload <archivo de stages> <stage>  (o variables STAGE_FILE/STAGE_NAME; si no hay datos, se omite). */
#include "wload.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { const char *role; uint16_t id; uint32_t tris; } Exp;
/* Armas/objetos del stage residente stage_name (id = id16 del nombre interno; tris = triángulos esperados). */
static const Exp EXP[] = {
    {"pistola", 0x21b6, 36},       {"pistola_supresor", 0x36f6, 52}, {"fusil_asalto", 0x4725, 70},
    {"rifle_francotirador", 0x2d3b, 106}, {"lanzador_guiado", 0x15a9, 81}, {"misil_guiado", 0x76ab, 66},
    {"lanzador_antiaereo", 0x57f8, 88},   {"misil_antiaereo", 0x9a90, 70}, {"granada", 0x3b88, 36},
    {"granada_humo_aturd", 0x761a, 44},   {"explosivo_adhesivo", 0xf83d, 28}, {"mina_direccional", 0x17a1, 32},
    {"caja_orange", 0x7a64, 40},   {"racion", 0xe2a9, 20},           {"cigarro", 0x2ef8, 12},
};
static int fails;
static const char *stage_name;
#define CHECK(c, ...) do { if (!(c)) { printf("  FALLO: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static int check(const uint8_t *d, size_t n, const Exp *e, const WLoadOpts *o, const char *tag) {
    WLoaded m;
    int r = wload_model(d, n, stage_name, e->id, o, &m);
    CHECK(r == 0, "%s %s: wload_model=%d", e->role, tag, r);
    if (r) return r;
    CHECK(m.ni == m.nv && m.ni == m.mesh.ni, "%s: conteos nv/ni", e->role);
    CHECK(m.ni / 3 == e->tris, "%s %s: triángulos %u != %u", e->role, tag, m.ni / 3, e->tris);
    float dx = m.bmax.x - m.bmin.x, dy = m.bmax.y - m.bmin.y, dz = m.bmax.z - m.bmin.z;
    float big = fmaxf(dx, fmaxf(dy, dz));
    CHECK(big > 0.02f && big < 1.5f, "%s: tamaño %.3f m fuera de rango", e->role, big);
    CHECK(m.atlas && m.atlas_w && m.atlas_h && m.ntex > 0, "%s: sin atlas", e->role);
    for (uint32_t i = 0; i < m.nv; i++) {
        const WVertex *v = &m.v[i];
        float l = sqrtf(v->nrm[0] * v->nrm[0] + v->nrm[1] * v->nrm[1] + v->nrm[2] * v->nrm[2]);
        if (fabsf(l - 1) > 0.01f || v->uv[0] < 0 || v->uv[0] > 1 || v->uv[1] < 0 || v->uv[1] > 1 || m.idx[i] != i) {
            CHECK(0, "%s: vértice %u inválido (|n|=%.3f uv=%.3f,%.3f)", e->role, i, l, v->uv[0], v->uv[1]);
            break;
        }
    }
    uint32_t sum = 0;
    for (uint32_t p = 0; p < m.nparts; p++) sum += m.parts[p].ntri;
    CHECK(sum == m.ni / 3, "%s: piezas suman %u != %u", e->role, sum, m.ni / 3);
    if (o->split_parts) printf("  %-20s piezas=%u\n", e->role, m.nparts);
    wload_free(&m);
    return 0;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : getenv("STAGE_FILE");
    stage_name = argc > 2 ? argv[2] : getenv("STAGE_NAME");
    if (!stage_name) { printf("test_wload: falta nombre de stage, omitido\n"); return 0; }
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) { printf("test_wload: sin archivo de stages, omitido\n"); return 0; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = (uint8_t *)malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { printf("lectura fallida\n"); return 1; }
    fclose(f);
    CHECK(wload_id16("") == 0, "id16 vacío");
    WLoadOpts base; wload_default_opts(&base); base.resident = stage_name;
    WLoadOpts smooth = base; smooth.smooth_deg = 50.f;
    WLoadOpts up4 = base; up4.upscale = 4;
    WLoadOpts split = base; split.split_parts = 1;
    WLoadOpts eng = base; eng.frame = W_FRAME_ENGINE;
    for (unsigned i = 0; i < sizeof EXP / sizeof *EXP; i++) {
        printf("%s\n", EXP[i].role);
        check(d, (size_t)n, &EXP[i], &base, "base");
        check(d, (size_t)n, &EXP[i], &smooth, "suave");
        check(d, (size_t)n, &EXP[i], &up4, "x4");
        check(d, (size_t)n, &EXP[i], &split, "piezas");
        check(d, (size_t)n, &EXP[i], &eng, "motor");
    }
    WLoaded m;
    CHECK(wload_model(d, (size_t)n, stage_name, 0x0001, &base, &m) == -1, "id inexistente debe dar -1");
    CHECK(wload_model(d, (size_t)n, "nope", 0x21b6, &base, &m) == -1, "stage inexistente debe dar -1");
    free(d);
    printf(fails ? "test_wload: %d fallos\n" : "test_wload: OK\n", fails);
    return fails != 0;
}
