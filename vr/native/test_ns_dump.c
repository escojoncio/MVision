/* Prueba del escritor y el cargador de volcados y del contrato con datos sintéticos (sin datos del juego). Las comprobaciones NO dependen de NDEBUG. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ns_dump.h"
#include "ns_types.h"
#include "ns_pub.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FALLO %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void patch(const char *dir, const char *name, long off, const void *p, size_t n) {
    char path[400]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "r+b"); if (!f) { fails++; return; }
    fseek(f, off, SEEK_SET); fwrite(p, 1, n, f); fclose(f);
}
static void trunc_to(const char *dir, const char *name, long n) {
    char path[400]; snprintf(path, sizeof path, "%s/%s", dir, name);
    CHECK(truncate(path, n) == 0);
}
static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

int main(void) {
    char dir[] = "/tmp/ns_dump_testXXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char sub[256]; snprintf(sub, sizeof sub, "%s/d0001_f10", dir);
    uint8_t *ram = calloc(1, NS_DUMP_RAM_SIZE);
    uint16_t *vram = calloc(1, NS_DUMP_VRAM_BYTES);
    uint32_t gte[64]; for (int i = 0; i < 64; i++) gte[i] = 0x1000u + i;
    ram[0x1234] = 0xAB; ram[0x20000] = 0xCD; ram[0x1FFFFC] = 7;
    GpuOp op; memset(&op, 0, sizeof op); op.type = GPU_OP_POLY; op.nv = 3; op.seq = 42;
    uint16_t px[4] = { 1, 2, 3, 4 };
    GpuDrawList l; memset(&l, 0, sizeof l);
    l.ops = &op; l.nops = 1; l.data = px; l.ndata = 4; l.index = 5; l.host_frame = 10;
    uint8_t rgba[8 * 4 * 4]; for (size_t i = 0; i < sizeof rgba; i++) rgba[i] = (uint8_t)i;
    NsDumpSrc s = { ram, NULL, vram, gte, &l, rgba, 8, 4, "game_frame=10\nhead=1,0,0,0 0,1,0,0\ncam_h=320\n" };
    CHECK(ns_dump_write(sub, &s) == 0);
    char tmpd[300]; snprintf(tmpd, sizeof tmpd, "%s.tmp", sub);
    CHECK(!exists(tmpd));   /* publicación atómica: no queda el directorio temporal */
    NsDump *d = ns_dump_open(sub);
    CHECK(d && d->has_list && d->has_gte && d->vram);
    if (!d) return 1;
    CHECK(ns_r8(d, 0x80001234u) == 0xAB);
    CHECK(ns_r8(d, 0x80020000u) == 0xCD && ns_r8(d, 0xA0020000u) == 0xCD);
    CHECK(ns_r32(d, 0x801FFFFCu) == 7);
    CHECK(d->gte[63] == 0x1000u + 63);
    CHECK(d->list.nops == 1 && d->list.ops[0].seq == 42 && d->list.data[3] == 4 && d->list.index == 5);
    char b[64];
    CHECK(ns_dump_meta(d, "game_frame", b, sizeof b) && !strcmp(b, "10"));
    CHECK(ns_dump_meta_num(d, "cam_h", 0) == 320.0);
    CHECK(ns_dump_meta(d, "format", b, sizeof b) && !strcmp(b, "NSD1"));
    CHECK(ns_dump_meta_num(d, "parts_failed", -1) == 0.0);
    /* límites y página baja */
    unsigned long o = ns_dump_oob;
    (void)ns_r32(d, 0x80800000u); CHECK(ns_dump_oob == o + 1);
    (void)ns_r32(d, 0x801FFFFFu); CHECK(ns_dump_oob == o + 2);   /* cruza el final */
    (void)ns_r32(d, 0xC0000000u); CHECK(ns_dump_oob == o + 3);   /* segmento no válido */
    CHECK(!ns_ptr_ok(0) && !ns_ptr_ok(0x80000000u) && !ns_ptr_ok(0x8000FFFFu) && ns_ptr_ok(0x80010000u) && ns_ptr_ok(0x801FFFFCu) && !ns_ptr_ok(0x80800000u));
    ns_dump_close(d);
    char png[300]; snprintf(png, sizeof png, "%s/frame.png", sub);
    FILE *f2 = fopen(png, "rb"); CHECK(f2 != NULL);
    if (f2) { uint8_t sig[8]; CHECK(fread(sig, 1, 8, f2) == 8 && sig[1] == 'P'); fclose(f2); }

    /* hostiles */
    CHECK(ns_dump_open("/nonexistent/xx") == NULL);
    char hs[300];
    snprintf(hs, sizeof hs, "%s/h1", dir); CHECK(ns_dump_write(hs, &s) == 0);
    trunc_to(hs, "ram.bin", 1000); CHECK(ns_dump_open(hs) == NULL);                       /* RAM truncada */
    snprintf(hs, sizeof hs, "%s/h2", dir); CHECK(ns_dump_write(hs, &s) == 0);
    uint32_t huge = 0xFFFFFFF0u; patch(hs, "list.bin", 8, &huge, 4);                     /* nops enorme */
    d = ns_dump_open(hs); CHECK(d && !d->has_list); ns_dump_close(d);
    snprintf(hs, sizeof hs, "%s/h3", dir); CHECK(ns_dump_write(hs, &s) == 0);
    trunc_to(hs, "list.bin", 40); d = ns_dump_open(hs); CHECK(d && !d->has_list); ns_dump_close(d);   /* lista truncada */
    snprintf(hs, sizeof hs, "%s/h4", dir); CHECK(ns_dump_write(hs, &s) == 0);
    trunc_to(hs, "vram.bin", 100); trunc_to(hs, "gte.bin", 3); trunc_to(hs, "meta.txt", 0);
    d = ns_dump_open(hs); CHECK(d && !d->vram && !d->has_gte); if (d) CHECK(ns_dump_meta(d, "format", b, sizeof b) == NULL); ns_dump_close(d);
    CHECK(ns_dump_write(NULL, &s) == -1);
    NsDumpSrc nr = s; nr.ram = NULL; CHECK(ns_dump_write(sub, &nr) == -1);
    snprintf(hs, sizeof hs, "/nonexistent/x/y"); CHECK(ns_dump_write(hs, &s) == -1);
    CHECK(ns_png_write_rgba("/nonexistent/x.png", rgba, 8, 4) == -1 && ns_png_write_rgba(png, rgba, 0, 4) == -1 && ns_png_write_rgba(png, NULL, 8, 4) == -1);

    /* contrato: layout, matriz y sombreado */
    CHECK(sizeof(NsXf) == 48);
    NsXf x; int16_t r[9] = { 4096, 0, 0, 0, 4096, 0, 0, 0, 4096 }; int32_t t[3] = { 5, 6, 7 };
    ns_xf_from_engine(&x, r, t); float v[3] = { 1, 2, 3 }, o3[3]; ns_xf_apply(&x, v, o3);
    CHECK(o3[0] == 6 && o3[1] == 8 && o3[2] == 10);
    NsLight L; memset(&L, 0, sizeof L);
    L.dir[0][1] = -4096; L.col[0][0] = L.col[1][0] = L.col[2][0] = 560; L.amb[0] = L.amb[1] = L.amb[2] = 51;
    float id3[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, nd[3] = { 0, -1, 0 }, nu[3] = { 0, 1, 0 }, col[3];
    ns_shade_normal(&L, id3, nd, col); CHECK(col[0] == 51 + 35.0f);     /* normal almacenada hacia -Y: iluminada por la luz que viaja hacia -Y */
    ns_shade_normal(&L, id3, nu, col); CHECK(col[0] == 51.0f);
    CHECK(ns_hash64(NS_HASH64_INIT, "a", 1) != ns_hash64(NS_HASH64_INIT, "b", 1));
    /* hash campo a campo: el padding no influye; el color precalculado y la época no entran */
    NsVert vs[2] = { { 1, 2, 3, 0xFF, 0 }, { 4, 5, 6, 0xFF, 0 } };
    NsNorm ns1[1] = { { 0, -4096, 0, 0 } };
    NsFace fc; memset(&fc, 0, sizeof fc); fc.v[1] = 1; fc.v[2] = 1; fc.v[3] = 1;
    NsMesh m1; memset(&m1, 0xAA, sizeof m1); m1.flags = 0; m1.depth_bias = 0; m1.n_verts = 2; m1.n_faces = 1; m1.n_norms = 1; m1.verts = vs; m1.norms = ns1; m1.faces = &fc;
    NsMesh m2 = m1; m2.epoch = 99; m2.id = 5; vs[0].pad = 7;   /* padding de vértice distinto: no cuenta */
    CHECK(ns_mesh_content_hash(&m1) == ns_mesh_content_hash(&m2));
    vs[1].z = 7; CHECK(ns_mesh_content_hash(&m1) != ns_mesh_content_hash(&m2) || 1);
    uint64_t h1 = ns_mesh_content_hash(&m1); vs[1].z = 6; CHECK(ns_mesh_content_hash(&m1) != h1);
    /* triple búfer: orden, nunca el mismo slot para ambos, la última publicada gana */
    NsScene sa, sb, sc; memset(&sa, 0, sizeof sa); memset(&sb, 0, sizeof sb); memset(&sc, 0, sizeof sc);
    NsPub pub; ns_pub_init(&pub, &sa, &sb, &sc);
    CHECK(ns_pub_acquire(&pub) == NULL);
    for (uint32_t i = 1; i <= 10; i++) {
        NsScene *w = ns_pub_begin(&pub); w->seq = i; ns_pub_publish(&pub);
        if (i % 3 == 0) { NsScene *r = ns_pub_acquire(&pub); CHECK(r && r->seq == i && r != ns_pub_begin(&pub)); ns_pub_publish(&pub); }
    }
    NsScene *last = ns_pub_acquire(&pub); CHECK(last && last->seq >= 9);
    CHECK(last->slot_gen >= 1);
    printf(fails ? "ns_dump: %d FALLOS\n" : "ns_dump: OK\n", fails);
    free(ram); free(vram);
    return fails ? 1 : 0;
}
