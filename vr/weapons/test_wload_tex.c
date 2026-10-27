/* Cobertura de texturas del cargador de modelos (wload) con un archivo de stages y un modelo SINTÉTICOS (sin datos del juego): una malla sin textura sale blanca (color de vértice 255), así que el
 * cargador debe decir cuántas texturas referencian las caras, cuántas no se encontraron y cuántas son de 8 bits sin paleta (CLUT perdido -> rampa de grises).
 * Con datos reales (gamedata/stages.bin) comprueba además que las armas y granadas del juego cargan con todas sus texturas y paleta (sin ellos se omite esa parte). */
#include "wload.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FALLO l.%d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define SEC 2048u
static void w16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { w16(p, v & 0xFFFF); w16(p + 2, v >> 16); }

/* PCX de 4x4: 8 bits con paleta al final (pal = 1) o sin ella (pal = 0), sin RLE. color = valor del índice 1 (el resto 0). */
static size_t make_pcx(uint8_t *o, int pal, uint8_t color) {
    memset(o, 0, 128);
    o[0] = 10; o[2] = 0; o[3] = 8;
    w16(o + 8, 3); w16(o + 10, 3);          /* xmax, ymax: 4x4 */
    o[65] = 1; w16(o + 66, 4);
    size_t n = 128;
    for (int i = 0; i < 16; i++) o[n++] = 1;
    if (pal) {
        o[n++] = 12;
        for (int i = 0; i < 256; i++) { o[n++] = i == 1 ? color : 0; o[n++] = i == 1 ? color / 2 : 0; o[n++] = 0; }
    }
    return n;
}

/* Archivo de stages con un stage "init" cuyo bloque 'n' lleva las texturas pedidas (id16, con paleta, color); tid 0 al final = fin. */
typedef struct { uint16_t id; int pal; uint8_t color; } TexDef;
static uint8_t *make_archive(const TexDef *t, size_t *len) {
    static uint8_t blk[8192];
    size_t bn = 0;
    for (; t && t->id; t++) {
        uint8_t pcx[1024];
        size_t n = make_pcx(pcx, t->pal, t->color);
        w32(blk + bn, (uint32_t)t->id | ((uint32_t)'p' << 16)); w32(blk + bn + 4, (uint32_t)n);
        memcpy(blk + bn + 8, pcx, n); bn += 8 + n;
    }
    size_t total = (size_t)SEC * 3 + ((bn + SEC - 1) / SEC) * SEC;
    uint8_t *d = (uint8_t *)calloc(1, total);
    w32(d, 12);                                           /* tabla de stages: 1 entrada */
    memcpy(d + 4, "init", 4); w32(d + 12, 1);             /* nombre (8) + sector */
    uint8_t *st = d + SEC;
    st[4] = 0; st[5] = 0; st[6] = 'n'; st[7] = 0; w32(st + 8, (uint32_t)bn);   /* etiqueta n: id 0, modo 'n', tamaño del bloque */
    /* la etiqueta de cierre (modo 0) ya está a cero */
    memcpy(d + 2 * SEC, blk, bn);
    *len = total;
    return d;
}

/* Modelo 'k' de 1 objeto: 2 caras (cuadriláteros) con las texturas ta y tb (0 = sin textura). */
static size_t make_model(uint8_t *k, uint16_t ta, uint16_t tb) {
    memset(k, 0, 512);
    w32(k, 1); w32(k + 4, 1);                             /* nvis, nobj */
    uint8_t *ob = k + 0x20;
    w32(ob + 0x2C, 0xFFFFFFFFu);                          /* sin padre */
    const uint32_t nf = 2, nv = 8, nn = 1, vo = 0x100, io = 0x140, no = 0x150, nio = 0x160, uvo = 0x170, to = 0x190;
    w32(ob + 4, nf); w32(ob + 0x34, nv); w32(ob + 0x38, vo); w32(ob + 0x3C, io);
    w32(ob + 0x40, nn); w32(ob + 0x44, no); w32(ob + 0x48, nio); w32(ob + 0x4C, uvo); w32(ob + 0x50, to);
    static const int16_t X[4] = {0, 100, 100, 0}, Y[4] = {0, 0, 100, 100};
    for (int f = 0; f < 2; f++) for (int i = 0; i < 4; i++) {
        uint8_t *v = k + vo + 8 * (4 * f + i);
        w16(v, (uint16_t)(X[i] + 200 * f)); w16(v + 2, (uint16_t)Y[i]); w16(v + 4, 0);
        k[io + 4 * f + i] = (uint8_t)(4 * f + i);
        k[nio + 4 * f + i] = 0;
        k[uvo + 8 * f + 2 * i] = (uint8_t)(X[i] ? 255 : 0); k[uvo + 8 * f + 2 * i + 1] = (uint8_t)(Y[i] ? 255 : 0);
    }
    w16(k + no + 4, 0x1000);                              /* una normal (0, 0, 4096) */
    w16(k + to, ta); w16(k + to + 2, tb);
    return 0x200;
}

static int load(const TexDef *t, uint16_t ta, uint16_t tb, WLoaded *m) {
    size_t len; uint8_t *d = make_archive(t, &len);
    uint8_t k[0x200]; size_t kn = make_model(k, ta, tb);
    WLoadOpts o; wload_default_opts(&o);
    int rc = wload_model_buf(d, len, "init", k, kn, &o, m);
    free(d);
    return rc;
}

static void synthetic(void) {
    WLoaded m;
    const TexDef both[] = {{0x1111, 1, 200}, {0x2222, 1, 90}, {0, 0, 0}};
    CHECK(load(both, 0x1111, 0x2222, &m) == 0, "modelo sintético con dos texturas");
    CHECK(m.ni == 12 && m.atlas && m.mesh.texture == 1, "con texturas: atlas y mesh.texture (tri %u)", m.ni / 3);
    CHECK(m.ntex == 2 && m.ntex_ref == 2 && m.ntex_missing == 0 && m.ntex_nopal == 0, "todas encontradas y con paleta: ref %u falta %u sin_paleta %u", m.ntex_ref, m.ntex_missing, m.ntex_nopal);
    int white = 0;                                        /* el atlas lleva el color de la paleta (no blanco ni gris) */
    for (uint32_t i = 0; i < m.atlas_w * m.atlas_h; i++) if (m.atlas[4 * i] == 200 && m.atlas[4 * i + 1] == 100 && m.atlas[4 * i + 2] == 0) white++;
    CHECK(white > 0, "el atlas conserva los colores de la paleta (CLUT) del fichero");
    wload_free(&m);

    const TexDef only_a[] = {{0x1111, 1, 200}, {0, 0, 0}};
    CHECK(load(only_a, 0x1111, 0x2222, &m) == 0, "una textura ausente");
    CHECK(m.atlas && m.ntex == 1 && m.ntex_ref == 2 && m.ntex_missing == 1 && m.first_missing == 0x2222, "una ausente: ref %u falta %u (id %04x)", m.ntex_ref, m.ntex_missing, m.first_missing);
    wload_free(&m);

    const TexDef none[] = {{0, 0, 0}};
    CHECK(load(none, 0x1111, 0x2222, &m) == 0, "sin ninguna textura");
    CHECK(!m.atlas && m.mesh.texture == 0 && m.ntex_missing == 2 && m.ntex_ref == 2, "sin texturas: sin atlas (la malla saldría blanca) y 2 ausentes (%u)", m.ntex_missing);
    wload_free(&m);

    const TexDef nopal[] = {{0x1111, 0, 200}, {0x2222, 1, 90}, {0, 0, 0}};
    CHECK(load(nopal, 0x1111, 0x2222, &m) == 0, "textura sin paleta");
    CHECK(m.ntex == 2 && m.ntex_missing == 0 && m.ntex_nopal == 1, "8 bits sin paleta detectado (sin_paleta %u)", m.ntex_nopal);
    wload_free(&m);

    CHECK(load(only_a, 0x1111, 0, &m) == 0, "una cara sin textura (id 0)");
    CHECK(m.ntex_ref == 1 && m.ntex_missing == 0, "el id 0 no cuenta como ausente (ref %u falta %u)", m.ntex_ref, m.ntex_missing);
    wload_free(&m);

    CHECK(load(both, 0x1111, 0x1111, &m) == 0 && m.ntex == 1 && m.ntex_ref == 1, "la misma textura en las dos caras cuenta una vez (%u)", m.ntex_ref);
    wload_free(&m);
}

/* Datos reales: armas y granadas con todas sus texturas y paleta. */
static void real(void) {
    const char *cands[2] = {"gamedata/stages.bin", "../../gamedata/stages.bin"};
    FILE *f = NULL;
    for (int i = 0; i < 2 && !f; i++) f = fopen(cands[i], "rb");
    if (!f) { printf("  datos reales: sin gamedata/stages.bin, omitido\n"); return; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = (uint8_t *)malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { printf("  lectura fallida\n"); fails++; fclose(f); free(d); return; }
    fclose(f);
    static const struct { const char *name; uint16_t id; } W[] = {
        {"pistola", 0x36f6}, {"pistola sin supresor", 0x21b6}, {"fusil", 0x4725}, {"francotirador", 0x2d3b}, {"guiado", 0x15a9}, {"antiaereo", 0x57f8},
        {"granada (frag)", 0x3b88}, {"granada cilindrica (aturdidora/humo)", 0x761a}, {"adhesivo", 0xf83d}, {"mina", 0x17a1},
    };
    WLoadOpts o; wload_default_opts(&o); o.resident = "init";
    for (unsigned i = 0; i < sizeof W / sizeof *W; i++) {
        WLoaded m;
        int rc = wload_model(d, (size_t)n, "init", W[i].id, &o, &m);
        CHECK(rc == 0, "%s: wload_model=%d", W[i].name, rc);
        if (rc) continue;
        printf("  %-38s atlas %ux%u texturas %u/%u ausentes %u sin_paleta %u%s\n", W[i].name, m.atlas_w, m.atlas_h, m.ntex, m.ntex_ref, m.ntex_missing, m.ntex_nopal, m.ntex_missing ? " (!)" : "");
        CHECK(m.atlas && m.mesh.texture == 1, "%s: sin atlas (saldría blanca)", W[i].name);
        CHECK(m.ntex_missing == 0, "%s: %u texturas sin encontrar (primera %04x): sus caras salen de un color plano", W[i].name, m.ntex_missing, m.first_missing);
        CHECK(m.ntex_nopal == 0, "%s: %u texturas de 8 bits sin paleta: salen en grises/blanco", W[i].name, m.ntex_nopal);
        wload_free(&m);
    }
    free(d);
}

int main(void) {
    synthetic();
    real();
    printf(fails ? "test_wload_tex: %d fallos de %d\n" : "test_wload_tex: OK (%d comprobaciones)\n", fails ? fails : checks, checks);
    return fails != 0;
}
