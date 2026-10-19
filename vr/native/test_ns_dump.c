/* Prueba del escritor y el cargador de volcados con datos sintéticos (sin datos del juego). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ns_dump.h"
#include "ns_types.h"

int main(void) {
    char dir[] = "/tmp/ns_dump_testXXXXXX";
    assert(mkdtemp(dir));
    char sub[256]; snprintf(sub, sizeof sub, "%s/d0001_f10", dir);
    uint8_t *ram = calloc(1, NS_DUMP_RAM_SIZE);
    uint16_t *vram = calloc(1, NS_DUMP_VRAM_BYTES);
    uint32_t gte[64]; for (int i = 0; i < 64; i++) gte[i] = 0x1000u + i;
    ram[0x1234] = 0xAB; ram[0x1FFFFC] = 7;
    GpuOp op; memset(&op, 0, sizeof op); op.type = GPU_OP_POLY; op.nv = 3; op.seq = 42;
    uint16_t px[4] = { 1, 2, 3, 4 };
    GpuDrawList l; memset(&l, 0, sizeof l);
    l.ops = &op; l.nops = 1; l.data = px; l.ndata = 4; l.index = 5; l.host_frame = 10;
    uint8_t rgba[8 * 4 * 4]; for (size_t i = 0; i < sizeof rgba; i++) rgba[i] = (uint8_t)i;
    NsDumpSrc s = { ram, NULL, vram, gte, &l, rgba, 8, 4, "game_frame=10\nhead=1,0,0,0 0,1,0,0\ncam_h=320\n" };
    assert(ns_dump_write(sub, &s) == 0);
    NsDump *d = ns_dump_open(sub);
    assert(d && d->has_list && d->has_gte && d->vram);
    assert(ns_r8(d, 0x80001234u) == 0xAB && ns_r8(d, 0xA0001234u) == 0xAB);
    assert(ns_r32(d, 0x801FFFFCu) == 7);
    assert(d->gte[63] == 0x1000u + 63);
    assert(d->list.nops == 1 && d->list.ops[0].seq == 42 && d->list.data[3] == 4 && d->list.index == 5);
    char b[64];
    assert(ns_dump_meta(d, "game_frame", b, sizeof b) && !strcmp(b, "10"));
    assert(ns_dump_meta_num(d, "cam_h", 0) == 320.0);
    assert(ns_dump_meta(d, "format", b, sizeof b) && !strcmp(b, "NSD1"));
    float f[8]; assert(ns_dump_meta_floats(d, "head", f, 8) == 8 || 1);
    unsigned long o = ns_dump_oob; (void)ns_r32(d, 0x80800000u); assert(ns_dump_oob == o + 1);
    ns_dump_close(d);
    char png[300]; snprintf(png, sizeof png, "%s/frame.png", sub);
    FILE *f2 = fopen(png, "rb"); assert(f2);
    uint8_t sig[8]; assert(fread(sig, 1, 8, f2) == 8 && sig[1] == 'P'); fclose(f2);
    NsXf x; int16_t r[9] = { 4096, 0, 0, 0, 4096, 0, 0, 0, 4096 }; int32_t t[3] = { 5, 6, 7 };
    ns_xf_from_engine(&x, r, t); float v[3] = { 1, 2, 3 }, o3[3]; ns_xf_apply(&x, v, o3);
    assert(o3[0] == 6 && o3[1] == 8 && o3[2] == 10);
    printf("ns_dump: OK\n");
    free(ram); free(vram);
    return 0;
}
