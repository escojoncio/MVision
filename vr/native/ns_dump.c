/* ns_dump: cargador de volcados (ver ns_dump.h). SOLO escritorio (herramientas y pruebas). */
#include "ns_dump.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned long ns_dump_oob;

#define SLURP_MAX (16u << 20)   /* tope de cualquier fichero del volcado */
static uint8_t *slurp(const char *dir, const char *name, size_t *n) {
    char p[640];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || (unsigned long)sz > SLURP_MAX) { fclose(f); return NULL; }
    uint8_t *b = (uint8_t *)malloc((size_t)sz + 1);
    if (!b) { fclose(f); return NULL; }
    size_t r = fread(b, 1, (size_t)sz, f);
    fclose(f);
    if (r != (size_t)sz) { free(b); return NULL; }
    b[sz] = 0;
    if (n) *n = (size_t)sz;
    return b;
}

NsDump *ns_dump_open(const char *dir) {
    NsDump *d = (NsDump *)calloc(1, sizeof *d);
    if (!d) return NULL;
    snprintf(d->dir, sizeof d->dir, "%s", dir);
    size_t n = 0;
    d->ram = slurp(dir, "ram.bin", &n);
    if (!d->ram || n != NS_DUMP_RAM_SIZE) { ns_dump_close(d); return NULL; }
    uint8_t *sp = slurp(dir, "scratch.bin", &n);
    if (sp && n == 1024) memcpy(d->scratch, sp, 1024);
    free(sp);
    uint8_t *vr = slurp(dir, "vram.bin", &n);
    if (vr && n == NS_DUMP_VRAM_BYTES) d->vram = (uint16_t *)vr; else free(vr);
    uint8_t *g = slurp(dir, "gte.bin", &n);
    if (g && n == 256) { memcpy(d->gte, g, 256); d->has_gte = 1; }
    free(g);
    d->meta = (char *)slurp(dir, "meta.txt", NULL);
    uint8_t *lb = slurp(dir, "list.bin", &n);
    if (lb && n >= 4 * 8) {
        uint32_t h[7]; memcpy(h, lb, sizeof h);
        uint32_t szd; memcpy(&szd, lb + sizeof h, 4);
        size_t off = sizeof h + 4;
        if (h[0] == 0x314C534Eu && h[1] == sizeof(GpuOp) && szd == sizeof(GpuDisplay) && h[2] <= (SLURP_MAX / sizeof(GpuOp)) && h[3] <= (SLURP_MAX / 2) &&
            n == off + sizeof(GpuDisplay) + (size_t)h[2] * sizeof(GpuOp) + (size_t)h[3] * 2) {
            GpuDrawList *l = &d->list;
            l->nops = h[2]; l->ndata = h[3]; l->index = h[4]; l->host_frame = h[5]; l->reason = (int)h[6];
            memcpy(&l->disp, lb + off, sizeof l->disp); off += sizeof l->disp;
            l->ops = (GpuOp *)malloc((size_t)(l->nops ? l->nops : 1) * sizeof(GpuOp));
            l->data = (uint16_t *)malloc((size_t)(l->ndata ? l->ndata : 1) * 2);
            if (l->ops && l->data) {
                memcpy(l->ops, lb + off, (size_t)l->nops * sizeof(GpuOp)); off += (size_t)l->nops * sizeof(GpuOp);
                memcpy(l->data, lb + off, (size_t)l->ndata * 2);
                l->cap_ops = l->nops; l->cap_data = l->ndata;
                d->has_list = 1;
            } else { free(l->ops); free(l->data); memset(l, 0, sizeof *l); }
        }
    }
    free(lb);
    return d;
}

void ns_dump_close(NsDump *d) {
    if (!d) return;
    free(d->ram); free(d->vram); free(d->meta); free(d->list.ops); free(d->list.data);
    free(d);
}

const char *ns_dump_meta(const NsDump *d, const char *key, char *buf, size_t cap) {
    if (!d || !d->meta || !key || !buf || !cap) return NULL;
    size_t kl = strlen(key);
    const char *p = d->meta;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t ll = e ? (size_t)(e - p) : strlen(p);
        if (ll > kl && !strncmp(p, key, kl) && p[kl] == '=') {
            size_t vl = ll - kl - 1;
            if (vl >= cap) vl = cap - 1;
            memcpy(buf, p + kl + 1, vl); buf[vl] = 0;
            return buf;
        }
        if (!e) break;
        p = e + 1;
    }
    return NULL;
}
double ns_dump_meta_num(const NsDump *d, const char *key, double dflt) {
    char b[64];
    return ns_dump_meta(d, key, b, sizeof b) ? atof(b) : dflt;
}
int ns_dump_meta_floats(const NsDump *d, const char *key, float *out, int n) {
    char b[512];
    if (!ns_dump_meta(d, key, b, sizeof b)) return 0;
    int k = 0;
    char *p = b;
    while (k < n) {
        char *q;
        double v = strtod(p, &q);
        if (q == p) break;
        out[k++] = (float)v;
        p = q;
        while (*p == ',' || *p == ' ') p++;
    }
    return k;
}

/* Puntero RAM válido del motor: segmentos de usuario, cacheado y no cacheado, dentro de los 2 MiB (con espejos hasta 8 MiB) y FUERA de los primeros 64 KiB (vectores y área reservada
 * del sistema: ningún objeto del juego vive ahí; un 0 o un valor pequeño es un puntero nulo o un entero). */
int ns_ptr_ok(uint32_t a) {
    uint32_t seg = a >> 29;
    uint32_t o = a & 0x1FFFFFFFu;
    return (seg == 0 || seg == 4 || seg == 5) && o < 0x00800000u && (o & 0x1FFFFFu) >= 0x10000u;
}
static const uint8_t *at(const NsDump *d, uint32_t a, uint32_t n) {
    uint32_t seg = a >> 29, o = a & 0x1FFFFFFFu;
    if (!d || !d->ram || !(seg == 0 || seg == 4 || seg == 5) || o >= 0x00800000u || n == 0 || n > NS_DUMP_RAM_SIZE) { ns_dump_oob++; return NULL; }
    o &= 0x1FFFFFu;
    if (o + n > NS_DUMP_RAM_SIZE) { ns_dump_oob++; return NULL; }
    return d->ram + o;
}
uint32_t ns_r32(const NsDump *d, uint32_t a) { const uint8_t *p = at(d, a, 4); uint32_t v = 0; if (p) memcpy(&v, p, 4); return v; }
uint16_t ns_r16(const NsDump *d, uint32_t a) { const uint8_t *p = at(d, a, 2); uint16_t v = 0; if (p) memcpy(&v, p, 2); return v; }
int16_t ns_s16(const NsDump *d, uint32_t a) { return (int16_t)ns_r16(d, a); }
int32_t ns_s32(const NsDump *d, uint32_t a) { return (int32_t)ns_r32(d, a); }
uint8_t ns_r8(const NsDump *d, uint32_t a) { const uint8_t *p = at(d, a, 1); return p ? *p : 0; }
