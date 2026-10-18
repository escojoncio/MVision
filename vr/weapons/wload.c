/* wload: ver wload.h. Formato de modelo/textura documentado en tools/wmodel/mdlfmt.py. */
#include "wload.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SEC 2048u
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int32_t  rs32(const uint8_t *p) { return (int32_t)rd32(p); }
static int      rs16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }
static uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }

uint16_t wload_id16(const char *s) {
    uint32_t h = 0;
    for (; *s; s++) h = ((uint8_t)*s + (((h << 5) | (h >> 11)) & 0xFFFF)) & 0xFFFF;
    return (uint16_t)h;
}

void wload_default_opts(WLoadOpts *o) {
    o->frame = W_FRAME_HELD; o->scale = 0.001f; o->smooth_deg = 0.f;
    o->resident = NULL; o->upscale = 1; o->key_black = 0; o->split_parts = 0;
}

/* ---------- contenedor de stage ---------- */
typedef struct { uint32_t id; uint8_t ext; const uint8_t *p; uint32_t n; } Ent;
typedef struct { Ent *e; int n, cap; } EntList;

static int ent_add(EntList *l, uint32_t id, uint8_t ext, const uint8_t *p, uint32_t n) {
    if (l->n == l->cap) {
        int c = l->cap ? l->cap * 2 : 256;
        Ent *x = (Ent *)realloc(l->e, c * sizeof(Ent));
        if (!x) return -3;
        l->e = x; l->cap = c;
    }
    l->e[l->n++] = (Ent){id, ext, p, n};
    return 0;
}

static int find_stage(const uint8_t *d, size_t len, const char *name, uint32_t *sector) {
    if (len < 4) return -1;
    uint32_t tsz = rd32(d);
    for (uint32_t i = 4; i + 12 <= 4 + tsz && i + 12 <= len; i += 12) {
        char nm[9]; memcpy(nm, d + i, 8); nm[8] = 0;
        if (strcmp(nm, name) == 0) { *sector = rd32(d + i + 8); return 0; }
    }
    return -1;
}

/* Rellena la lista con los ficheros del stage: grupo 'c' (id16, ext) y bloques n/r ({id24,tamaño,datos}). */
static int stage_entries(const uint8_t *d, size_t len, const char *name, EntList *l) {
    uint32_t sector;
    if (find_stage(d, len, name, &sector)) return -1;
    size_t o = (size_t)sector * SEC;
    if (o + 12 > len) return -2;
    typedef struct { uint16_t id; uint8_t mode, ext; int32_t size; uint32_t sec; } Tag;
    Tag tags[4096]; int nt = 0;
    size_t p = o + 4;
    for (;;) {
        if (p + 8 > len || nt >= 4096) return -2;
        Tag t; t.id = (uint16_t)rd16(d + p); t.mode = d[p + 2]; t.ext = d[p + 3]; t.size = rs32(d + p + 4); t.sec = 0;
        p += 8;
        if (t.mode == 0) break;
        tags[nt++] = t;
    }
    uint32_t cur = sector + (uint32_t)((p - o + SEC - 1) / SEC);
    for (int i = 0; i < nt;) {
        if (tags[i].mode == 'c') {
            int j = i;
            while (j < nt && tags[j].mode == 'c') j++;
            uint32_t grp = (uint32_t)tags[j - 1].size;
            for (int k = i; k < j; k++) tags[k].sec = cur;
            cur += (grp + SEC - 1) / SEC;
            i = j;
        } else {
            tags[i].sec = cur; cur += ((uint32_t)tags[i].size + SEC - 1) / SEC; i++;
        }
    }
    for (int i = 0; i < nt; i++) {
        Tag *t = &tags[i];
        size_t base = (size_t)t->sec * SEC;
        if (t->mode == 'c') {
            if (t->ext == 0xFF || i + 1 >= nt || tags[i + 1].mode != 'c') continue;
            size_t a = (size_t)t->size, b = (size_t)tags[i + 1].size;
            if (b < a || base + b > len) continue;
            int r = ent_add(l, t->id, t->ext, d + base + a, (uint32_t)(b - a));
            if (r) return r;
        } else if (t->mode == 'n' || t->mode == 'r') {
            size_t q = base, end = base + (uint32_t)t->size;
            if (end > len) continue;
            while (q + 8 <= end) {
                uint32_t id = rd32(d + q), sz = rd32(d + q + 4);
                if (!sz || q + 8 + sz > end) break;
                int r = ent_add(l, id, (uint8_t)(id >> 16), d + q + 8, sz);
                if (r) return r;
                q += 8 + sz;
            }
        }
    }
    return 0;
}

/* ---------- imagen ---------- */
typedef struct { int w, h; uint8_t *rgba; } Img;

static int img_decode(const uint8_t *b, size_t n, int key_black, Img *im) {
    if (n < 128 || b[0] != 10) return -1;
    int enc = b[2], bpp = b[3];
    int w = (int)rd16(b + 8) - (int)rd16(b + 4) + 1, h = (int)rd16(b + 10) - (int)rd16(b + 6) + 1;
    int planes = b[65], bpl = (int)rd16(b + 66);
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || planes < 1) return -1;
    size_t need = (size_t)bpl * planes * h, got = 0, p = 128;
    uint8_t *raw = (uint8_t *)calloc(need + 64, 1);
    if (!raw) return -3;
    while (got < need && p < n) {
        uint8_t c = b[p++];
        if (enc && c >= 0xC0) {
            int r = c & 0x3F; if (p >= n) break;
            uint8_t v = b[p++];
            while (r-- && got < need) raw[got++] = v;
        } else raw[got++] = c;
    }
    uint8_t pal[256][3];
    int np = 16;
    if (bpp == 8 && planes == 1) {
        np = 256;
        if (n >= 769 && b[n - 769] == 12) memcpy(pal, b + n - 768, 768);
        else for (int i = 0; i < 256; i++) pal[i][0] = pal[i][1] = pal[i][2] = (uint8_t)i;
    } else memcpy(pal, b + 16, 48);
    im->w = w; im->h = h;
    im->rgba = (uint8_t *)malloc((size_t)w * h * 4);
    if (!im->rgba) { free(raw); return -3; }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int v = 0;
            if (bpp == 8 && planes == 1) v = raw[(size_t)y * bpl + x];
            else if (bpp == 4 && planes == 1) { uint8_t c = raw[(size_t)y * bpl + (x >> 1)]; v = (x & 1) ? (c & 15) : (c >> 4); }
            else if (bpp == 1)
                for (int pl = 0; pl < planes; pl++) v |= ((raw[(size_t)y * bpl * planes + pl * bpl + (x >> 3)] >> (7 - (x & 7))) & 1) << pl;
            if (v >= np) v = 0;
            uint8_t *o = im->rgba + ((size_t)y * w + x) * 4;
            o[0] = pal[v][0]; o[1] = pal[v][1]; o[2] = pal[v][2];
            o[3] = (key_black && !o[0] && !o[1] && !o[2]) ? 0 : 255;
        }
    free(raw);
    return 0;
}

/* ---------- escalado tipo xBR (variante propia, x2) ---------- */
static int cdist(const uint8_t *a, const uint8_t *b) {
    if (!a[3] || !b[3]) return (a[3] == b[3]) ? 0 : 1000;
    int r = (a[0] + b[0]) >> 1, dr = a[0] - b[0], dg = a[1] - b[1], db = a[2] - b[2];
    /* distancia de color ponderada (aprox. "redmean"), sensible al brillo como YUV */
    return (((512 + r) * dr * dr) >> 8) + 4 * dg * dg + (((767 - r) * db * db) >> 8);
}
static void mix(uint8_t *o, const uint8_t *a, const uint8_t *b, int wa, int wb) {
    int s = wa + wb;
    for (int k = 0; k < 4; k++) o[k] = (uint8_t)((a[k] * wa + b[k] * wb + s / 2) / s);
    if (!a[3] || !b[3]) o[3] = a[3];
}
void wload_scale2x(const uint8_t *src, int w, int h, uint8_t *dst) {
#define PX(x, y) (src + ((size_t)((y) < 0 ? 0 : (y) >= h ? h - 1 : (y)) * w + ((x) < 0 ? 0 : (x) >= w ? w - 1 : (x))) * 4)
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *E = PX(x, y);
            const uint8_t *N[3][3];
            for (int j = 0; j < 3; j++) for (int i = 0; i < 3; i++) N[j][i] = PX(x + i - 1, y + j - 1);
            const uint8_t *B = N[0][1], *D = N[1][0], *F = N[1][2], *H = N[2][1];
            const uint8_t *A = N[0][0], *C = N[0][2], *G = N[2][0], *I = N[2][2];
            /* esquinas: 0 sup-izq, 1 sup-der, 2 inf-izq, 3 inf-der; (v1, v2) vecinos ortogonales, (dg) diagonal,
             * (o1, o2) vecinos exteriores al otro lado de la arista. */
            const uint8_t *v1[4] = {B, B, H, H}, *v2[4] = {D, F, D, F}, *dg[4] = {A, C, G, I};
            const uint8_t *o1[4] = {F, D, F, D}, *o2[4] = {H, H, B, B};
            const uint8_t *w1[4] = {PX(x - 2, y), PX(x + 2, y), PX(x - 2, y), PX(x + 2, y)};
            const uint8_t *w2[4] = {PX(x, y - 2), PX(x, y - 2), PX(x, y + 2), PX(x, y + 2)};
            for (int c = 0; c < 4; c++) {
                uint8_t *out = dst + ((size_t)(2 * y + (c >> 1)) * (2 * w) + 2 * x + (c & 1)) * 4;
                memcpy(out, E, 4);
                if (cdist(v1[c], v2[c]) == 0 && cdist(E, v1[c]) != 0) {
                    /* esquina con dos vecinos iguales distintos de E: candidato a diagonal */
                    int e = cdist(E, dg[c]) + cdist(v1[c], w1[c]) + cdist(v2[c], w2[c]) + 4 * cdist(v1[c], v2[c]);
                    int i = cdist(E, v1[c]) * 4 + cdist(o1[c], dg[c]) + cdist(o2[c], dg[c]);
                    if (e < i) mix(out, E, v1[c], 1, 3);
                } else if (cdist(E, v1[c]) != 0 && cdist(E, v2[c]) != 0) {
                    int e = cdist(E, dg[c]) + cdist(v1[c], w2[c]) + cdist(v2[c], w1[c]) + 4 * cdist(v1[c], v2[c]);
                    int i = cdist(E, v1[c]) + cdist(E, v2[c]) + cdist(o1[c], o2[c]) * 2;
                    if (e * 2 < i) {
                        const uint8_t *px = cdist(E, v1[c]) <= cdist(E, v2[c]) ? v1[c] : v2[c];
                        mix(out, E, px, 3, 1);
                    }
                }
            }
        }
#undef PX
}

static int upscale(Img *im, int factor) {
    while (factor >= 2) {
        uint8_t *n = (uint8_t *)malloc((size_t)im->w * im->h * 16);
        if (!n) return -3;
        wload_scale2x(im->rgba, im->w, im->h, n);
        free(im->rgba); im->rgba = n; im->w *= 2; im->h *= 2; factor /= 2;
    }
    return 0;
}

/* ---------- modelo ---------- */
typedef struct { uint32_t id; Img im; int x, y; } TexSlot;
typedef struct { float p[3]; float n[3]; float uv[2]; int tex; } Corner;

static int find_tex(EntList *lists[2], uint16_t id, const uint8_t **p, uint32_t *n) {
    for (int k = 0; k < 2; k++) {
        if (!lists[k]) continue;
        for (int i = 0; i < lists[k]->n; i++)
            if (lists[k]->e[i].ext == 'p' && (lists[k]->e[i].id & 0xFFFF) == id && lists[k]->e[i].n > 128) {
                *p = lists[k]->e[i].p; *n = lists[k]->e[i].n; return 0;
            }
    }
    return -1;
}

typedef struct { int32_t k[3]; uint32_t i; } Key;
static int key_cmp(const void *x, const void *y) { return memcmp(x, y, 12); }
static int uf_find(int *par, int a) { while (par[a] != a) { par[a] = par[par[a]]; a = par[a]; } return a; }

static void map_frame(const WLoadOpts *o, const float *e, float *w) {
    float s = o->scale;
    if (o->frame == W_FRAME_HELD) { w[0] = e[0] * s; w[1] = -e[2] * s; w[2] = e[1] * s; }
    else { w[0] = e[0] * s; w[1] = e[1] * s; w[2] = e[2] * s; }
}

int wload_model_buf(const uint8_t *dir, size_t dir_len, const char *stage,
                    const uint8_t *k, size_t kn, const WLoadOpts *opt, WLoaded *out) {
    WLoadOpts o; if (opt) o = *opt; else wload_default_opts(&o);
    memset(out, 0, sizeof *out);
    if (kn < 0x20) return -2;
    int nvis = (int)rd32(k), nobj = (int)rd32(k + 4); (void)nvis;
    if (nobj <= 0 || nobj > 512 || 0x20 + (size_t)nobj * 0x58 > kn) return -2;
    int rc = 0;
    TexSlot slots[64]; int nslot = 0;
    EntList ls = {0}, li = {0}; EntList *lists[2] = {NULL, NULL};
    if (stage_entries(dir, dir_len, stage, &ls) == 0) lists[0] = &ls;
    if (o.resident && strcmp(stage, o.resident) && stage_entries(dir, dir_len, o.resident, &li) == 0) lists[1] = &li;
    /* offsets acumulados */
    float (*off)[3] = (float (*)[3])calloc((size_t)nobj, sizeof *off);
    int *parent = (int *)malloc(nobj * sizeof(int));
    uint32_t ntri_max = 0;
    for (int i = 0; i < nobj; i++) {
        const uint8_t *ob = k + 0x20 + (size_t)i * 0x58;
        parent[i] = (int)rs32(ob + 0x2C);
        uint32_t nf = rd32(ob);  nf = rd32(ob + 4);
        ntri_max += nf * 2;
        for (int a = 0; a < 3; a++) off[i][a] = (float)rs32(ob + 0x20 + 4 * a);
        if (parent[i] >= nobj) { rc = -2; goto done; }
    }
    for (int i = 0; i < nobj; i++) {            /* los padres no tienen por qué ir antes: se suma la cadena */
        float acc[3] = {off[i][0], off[i][1], off[i][2]};
        int q = parent[i], g = 0;
        while (q >= 0 && g++ < 64) {
            const uint8_t *pb = k + 0x20 + (size_t)q * 0x58;
            for (int a = 0; a < 3; a++) acc[a] += (float)rs32(pb + 0x20 + 4 * a);
            q = (int)rs32(pb + 0x2C);
        }
        memcpy(off[i], acc, sizeof acc);
    }
    if (ntri_max * 3 > 65535) { rc = -4; goto done; }
    Corner *cn = (Corner *)malloc((size_t)ntri_max * 3 * sizeof(Corner));
    int *ptri = (int *)malloc((size_t)ntri_max * sizeof(int));   /* componente de cada triángulo */
    uint32_t nt = 0, compbase = 0;
    if (!cn || !ptri) { rc = -3; free(cn); free(ptri); goto done; }
    for (int oi = 0; oi < nobj; oi++) {
        const uint8_t *ob = k + 0x20 + (size_t)oi * 0x58;
        uint32_t nf = rd32(ob + 4), nv = rd32(ob + 0x34), vo = rd32(ob + 0x38), io = rd32(ob + 0x3C),
                 nn = rd32(ob + 0x40), no = rd32(ob + 0x44), nio = rd32(ob + 0x48), uvo = rd32(ob + 0x4C), to = rd32(ob + 0x50);
        if (nf > 4000 || nv > 4000 || nn > 4000 || (size_t)vo + nv * 8 > kn || (size_t)io + nf * 4 > kn ||
            (size_t)no + nn * 8 > kn || (size_t)nio + nf * 4 > kn || (size_t)uvo + nf * 8 > kn || (size_t)to + nf * 2 > kn) {
            rc = -2; free(cn); free(ptri); goto done;
        }
        int *par = (int *)malloc((nv + 1) * sizeof(int));
        for (uint32_t v = 0; v <= nv; v++) par[v] = (int)v;
        for (uint32_t f = 0; f < nf; f++) {
            const uint8_t *vi = k + io + 4 * f;
            for (int c = 1; c < 4; c++) if (vi[0] < nv && vi[c] < nv) par[uf_find(par, vi[c])] = uf_find(par, vi[0]);
        }
        for (uint32_t f = 0; f < nf; f++) {
            const uint8_t *vi = k + io + 4 * f, *ni = k + nio + 4 * f, *uv = k + uvo + 8 * f;
            uint16_t tid = (uint16_t)rd16(k + to + 2 * f);
            if (vi[0] >= nv || vi[1] >= nv || vi[2] >= nv || vi[3] >= nv) continue;
            /* textura -> hueco en el atlas */
            int ts = -1;
            for (int s = 0; s < nslot; s++) if (slots[s].id == tid) { ts = s; break; }
            if (ts < 0 && nslot < 64) {
                const uint8_t *pp; uint32_t pn;
                if (find_tex(lists, tid, &pp, &pn) == 0 && img_decode(pp, pn, o.key_black, &slots[nslot].im) == 0) {
                    slots[nslot].id = tid; ts = nslot++;
                    if (o.upscale >= 2 && upscale(&slots[ts].im, o.upscale >= 4 ? 4 : 2)) { rc = -3; free(par); free(cn); free(ptri); goto done; }
                }
            }
            static const int T[2][3] = {{0, 1, 2}, {0, 2, 3}};
            for (int t = 0; t < 2; t++) {
                if (t == 1 && (vi[3] == vi[2] || vi[3] == vi[0])) break;
                Corner *c = cn + (size_t)nt * 3;
                float fn[3] = {0, 0, 0}, sn[3] = {0, 0, 0};
                for (int q = 0; q < 3; q++) {
                    int ci = T[t][q];
                    const uint8_t *vp = k + vo + 8 * vi[ci];
                    float e[3] = {(float)rs16(vp) + off[oi][0], (float)rs16(vp + 2) + off[oi][1], (float)rs16(vp + 4) + off[oi][2]};
                    map_frame(&o, e, c[q].p);
                    uint32_t nidx = ni[ci] & 0x7F;
                    if (nidx < nn) {
                        const uint8_t *np = k + no + 8 * nidx;
                        float ne[3] = {(float)rs16(np), (float)rs16(np + 2), (float)rs16(np + 4)};
                        float l = sqrtf(ne[0] * ne[0] + ne[1] * ne[1] + ne[2] * ne[2]); if (l < 1e-6f) l = 1;
                        for (int a = 0; a < 3; a++) ne[a] /= l;
                        /* normal en el marco de salida (rotación sin escala) */
                        WLoadOpts u = o; u.scale = 1.f; map_frame(&u, ne, c[q].n);
                    } else { c[q].n[0] = c[q].n[1] = 0; c[q].n[2] = 1; }
                    for (int a = 0; a < 3; a++) sn[a] += c[q].n[a];
                    c[q].uv[0] = uv[2 * ci] / 255.f; c[q].uv[1] = uv[2 * ci + 1] / 255.f;
                    c[q].tex = ts;
                }
                float a1[3], b1[3];
                for (int a = 0; a < 3; a++) { a1[a] = c[1].p[a] - c[0].p[a]; b1[a] = c[2].p[a] - c[0].p[a]; }
                fn[0] = a1[1] * b1[2] - a1[2] * b1[1]; fn[1] = a1[2] * b1[0] - a1[0] * b1[2]; fn[2] = a1[0] * b1[1] - a1[1] * b1[0];
                if (fn[0] * sn[0] + fn[1] * sn[1] + fn[2] * sn[2] < 0) { Corner tmp = c[1]; c[1] = c[2]; c[2] = tmp; }
                ptri[nt] = (int)compbase + uf_find(par, vi[0]);
                nt++;
            }
        }
        compbase += nv + 1;
        free(par);
    }
    /* atlas */
    uint32_t aw = 0, ah = 0; int pad = 2 * (o.upscale >= 4 ? 4 : o.upscale >= 2 ? 2 : 1) / 2 + 1;
    if (nslot) {
        for (uint32_t W = 64; W <= 2048; W *= 2) {
            int x = 0, y = 0, rowh = 0, ok = 1; uint32_t maxy = 0;
            for (int s = 0; s < nslot && ok; s++) {
                int w = slots[s].im.w + 2 * pad, h = slots[s].im.h + 2 * pad;
                if (w > (int)W) { ok = 0; break; }
                if (x + w > (int)W) { x = 0; y += rowh; rowh = 0; }
                slots[s].x = x + pad; slots[s].y = y + pad; x += w; if (h > rowh) rowh = h;
                if ((uint32_t)(y + rowh) > maxy) maxy = (uint32_t)(y + rowh);
            }
            if (ok && maxy <= 2048) { aw = W; ah = 1; while (ah < maxy) ah *= 2; break; }
        }
        if (!aw) { rc = -3; free(cn); free(ptri); goto done; }
        out->atlas = (uint8_t *)calloc((size_t)aw * ah, 4);
        if (!out->atlas) { rc = -3; free(cn); free(ptri); goto done; }
        for (int s = 0; s < nslot; s++) {
            Img *im = &slots[s].im;
            for (int y = -pad; y < im->h + pad; y++)
                for (int x = -pad; x < im->w + pad; x++) {      /* gutter por réplica del borde */
                    int sx = x < 0 ? 0 : x >= im->w ? im->w - 1 : x, sy = y < 0 ? 0 : y >= im->h ? im->h - 1 : y;
                    memcpy(out->atlas + ((size_t)(slots[s].y + y) * aw + slots[s].x + x) * 4, im->rgba + ((size_t)sy * im->w + sx) * 4, 4);
                }
        }
        out->atlas_w = aw; out->atlas_h = ah; out->ntex = (uint32_t)nslot;
    }
    /* normales suavizadas por ángulo (sobre posiciones soldadas) */
    if (o.smooth_deg > 0.f && nt) {
        float cosl = cosf(o.smooth_deg * 3.14159265f / 180.f);
        float (*fnorm)[3] = (float (*)[3])malloc((size_t)nt * sizeof *fnorm);
        float (*nn2)[3] = (float (*)[3])malloc((size_t)nt * 3 * sizeof *nn2);
        if (fnorm && nn2) {
            for (uint32_t t = 0; t < nt; t++) {
                Corner *c = cn + (size_t)t * 3; float a[3], b[3], n[3];
                for (int i = 0; i < 3; i++) { a[i] = c[1].p[i] - c[0].p[i]; b[i] = c[2].p[i] - c[0].p[i]; }
                n[0] = a[1] * b[2] - a[2] * b[1]; n[1] = a[2] * b[0] - a[0] * b[2]; n[2] = a[0] * b[1] - a[1] * b[0];
                float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);   /* ponderado por área */
                memcpy(fnorm[t], n, sizeof n); (void)l;
            }
        }
        /* implementación simple O(n * grupo): ordenar por clave de posición */
        Key *keys = (Key *)malloc((size_t)nt * 3 * sizeof(Key));
        if (keys && fnorm && nn2) {
            for (uint32_t i = 0; i < nt * 3; i++) {
                const Corner *c = cn + i;
                for (int a = 0; a < 3; a++) keys[i].k[a] = (int32_t)lroundf(c->p[a] * 20000.f);
                keys[i].i = i;
            }
            qsort(keys, (size_t)nt * 3, sizeof(Key), key_cmp);
            for (uint32_t s = 0; s < nt * 3;) {
                uint32_t e = s + 1;
                while (e < nt * 3 && memcmp(keys[s].k, keys[e].k, 12) == 0) e++;
                for (uint32_t a = s; a < e; a++) {
                    float acc[3] = {0, 0, 0};
                    const float *na = fnorm[keys[a].i / 3];
                    float la = sqrtf(na[0] * na[0] + na[1] * na[1] + na[2] * na[2]) + 1e-12f;
                    for (uint32_t b = s; b < e; b++) {
                        const float *nb = fnorm[keys[b].i / 3];
                        float lb = sqrtf(nb[0] * nb[0] + nb[1] * nb[1] + nb[2] * nb[2]) + 1e-12f;
                        if ((na[0] * nb[0] + na[1] * nb[1] + na[2] * nb[2]) / (la * lb) >= cosl)
                            for (int q = 0; q < 3; q++) acc[q] += nb[q];
                    }
                    memcpy(nn2[keys[a].i], acc, sizeof acc);
                }
                s = e;
            }
            for (uint32_t i = 0; i < nt * 3; i++) {
                float l = sqrtf(nn2[i][0] * nn2[i][0] + nn2[i][1] * nn2[i][1] + nn2[i][2] * nn2[i][2]);
                if (l > 1e-12f) for (int q = 0; q < 3; q++) cn[i].n[q] = nn2[i][q] / l;
            }
        }
        free(keys); free(fnorm); free(nn2);
    }
    /* orden por pieza (componente) y volcado */
    uint32_t *order = (uint32_t *)malloc((size_t)nt * sizeof(uint32_t));
    if (!order) { rc = -3; free(cn); free(ptri); goto done; }
    uint32_t nparts = 1;
    if (o.split_parts) {
        uint32_t *keyc = (uint32_t *)malloc((size_t)nt * sizeof(uint32_t)), *rank = (uint32_t *)calloc(compbase + 1, sizeof(uint32_t));
        for (uint32_t t = 0; t < nt; t++) rank[ptri[t]] = 1;
        nparts = 0;
        for (uint32_t c = 0; c <= compbase; c++) if (rank[c]) rank[c] = ++nparts;
        for (uint32_t t = 0; t < nt; t++) keyc[t] = rank[ptri[t]] - 1;
        uint32_t *cnt = (uint32_t *)calloc(nparts + 1, sizeof(uint32_t));
        for (uint32_t t = 0; t < nt; t++) cnt[keyc[t] + 1]++;
        for (uint32_t p = 0; p < nparts; p++) cnt[p + 1] += cnt[p];
        out->parts = (WLoadPart *)calloc(nparts ? nparts : 1, sizeof(WLoadPart));
        for (uint32_t p = 0; p < nparts; p++) { out->parts[p].first_tri = cnt[p]; out->parts[p].ntri = cnt[p + 1] - cnt[p]; }
        for (uint32_t t = 0; t < nt; t++) order[cnt[keyc[t]]++] = t;
        free(keyc); free(rank); free(cnt);
    } else {
        for (uint32_t t = 0; t < nt; t++) order[t] = t;
        out->parts = (WLoadPart *)calloc(1, sizeof(WLoadPart));
        out->parts[0].first_tri = 0; out->parts[0].ntri = nt;
    }
    out->nparts = nparts;
    out->v = (WVertex *)calloc((size_t)nt * 3 + 1, sizeof(WVertex));
    out->idx = (uint16_t *)malloc(((size_t)nt * 3 + 1) * sizeof(uint16_t));
    if (!out->v || !out->idx) { rc = -3; free(order); free(cn); free(ptri); goto done; }
    out->nv = nt * 3; out->ni = nt * 3;
    float gmin[3] = {1e30f, 1e30f, 1e30f}, gmax[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t p = 0; p < out->nparts; p++) {
        WLoadPart *pp = &out->parts[p];
        float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
        for (uint32_t j = 0; j < pp->ntri; j++) {
            uint32_t t = order[pp->first_tri + j];
            for (int q = 0; q < 3; q++) {
                const Corner *c = cn + (size_t)t * 3 + q;
                uint32_t vi = (pp->first_tri + j) * 3 + q;
                WVertex *w = &out->v[vi];
                memcpy(w->pos, c->p, 12); memcpy(w->nrm, c->n, 12);
                if (c->tex >= 0) {
                    TexSlot *s = &slots[c->tex];
                    w->uv[0] = (s->x + (c->uv[0] * (s->im.w - 1) + 0.5f)) / (float)aw;
                    w->uv[1] = (s->y + (c->uv[1] * (s->im.h - 1) + 0.5f)) / (float)ah;
                } else { w->uv[0] = w->uv[1] = 0; }
                w->rgba[0] = w->rgba[1] = w->rgba[2] = w->rgba[3] = 255;
                out->idx[vi] = (uint16_t)vi;
                for (int a = 0; a < 3; a++) { if (c->p[a] < mn[a]) mn[a] = c->p[a]; if (c->p[a] > mx[a]) mx[a] = c->p[a]; }
            }
        }
        if (!pp->ntri) mn[0] = mn[1] = mn[2] = mx[0] = mx[1] = mx[2] = 0;
        pp->bmin = (WVec3){mn[0], mn[1], mn[2]}; pp->bmax = (WVec3){mx[0], mx[1], mx[2]};
        pp->center = (WVec3){(mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2};
        for (int a = 0; a < 3; a++) { if (mn[a] < gmin[a]) gmin[a] = mn[a]; if (mx[a] > gmax[a]) gmax[a] = mx[a]; }
    }
    out->bmin = (WVec3){gmin[0], gmin[1], gmin[2]}; out->bmax = (WVec3){gmax[0], gmax[1], gmax[2]};
    out->mesh.name = "wload"; out->mesh.v = out->v; out->mesh.nv = out->nv;
    out->mesh.idx = out->idx; out->mesh.ni = out->ni; out->mesh.texture = out->atlas ? 1 : 0;
    free(order); free(cn); free(ptri);
done:
    (void)pad;
    for (int s = 0; s < nslot; s++) free(slots[s].im.rgba);
    free(off); free(parent); free(ls.e); free(li.e);
    if (rc) wload_free(out);
    return rc;
}

int wload_model(const uint8_t *dir, size_t dir_len, const char *stage, uint16_t id, const WLoadOpts *o, WLoaded *out) {
    EntList l = {0};
    int r = stage_entries(dir, dir_len, stage, &l);
    if (r) { free(l.e); memset(out, 0, sizeof *out); return -1; }
    const Ent *f = NULL;
    for (int i = 0; i < l.n; i++) if (l.e[i].ext == 'k' && (l.e[i].id & 0xFFFF) == id) { f = &l.e[i]; break; }
    if (!f) { free(l.e); memset(out, 0, sizeof *out); return -1; }
    r = wload_model_buf(dir, dir_len, stage, f->p, f->n, o, out);
    free(l.e);
    return r;
}

void wload_free(WLoaded *m) {
    free(m->v); free(m->idx); free(m->atlas); free(m->parts);
    memset(m, 0, sizeof *m);
}
