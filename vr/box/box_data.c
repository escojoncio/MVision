/* box_data: ver box_data.h. */
#include "box_data.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SEC 2048u
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static int      rs16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

/* ---------------- contenedor de stage ---------------- */
int box_stage_find(const uint8_t *d, size_t len, const char *name, uint16_t id16, uint8_t type,
                   const uint8_t **out, size_t *outn) {
    if (!d || len < 16 || !name) return -1;
    uint32_t tsz = rd32(d), sector = 0;
    int found = 0;
    for (uint32_t i = 4; i + 12 <= 4 + tsz && i + 12 <= len; i += 12) {
        char nm[9]; memcpy(nm, d + i, 8); nm[8] = 0;
        if (strcmp(nm, name) == 0) { sector = rd32(d + i + 8); found = 1; break; }
    }
    if (!found) return -1;
    size_t o = (size_t)sector * SEC;
    if (o + 12 > len) return -2;
    typedef struct { uint16_t id; uint8_t mode, ext; int32_t size; uint32_t sec; } Tag;
    static const int MAXT = 2048;
    Tag *tags = (Tag *)malloc(sizeof(Tag) * (size_t)MAXT);
    if (!tags) return -2;
    int nt = 0, rc = -1;
    size_t p = o + 4;
    for (;;) {
        if (p + 8 > len || nt >= MAXT) { rc = -2; goto done; }
        Tag t; t.id = (uint16_t)rd16(d + p); t.mode = d[p + 2]; t.ext = d[p + 3]; t.size = (int32_t)rd32(d + p + 4); t.sec = 0;
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
            if (t->id == id16 && t->ext == type) { *out = d + base + a; *outn = b - a; rc = 0; goto done; }
        } else if (t->mode == 'n' || t->mode == 'r') {
            size_t q = base, end = base + (uint32_t)t->size;
            if (end > len) continue;
            while (q + 8 <= end) {
                uint32_t id = rd32(d + q), sz = rd32(d + q + 4);
                if (!sz || q + 8 + sz > end) break;
                if ((id & 0xFFFFu) == id16 && (uint8_t)(id >> 16) == type) { *out = d + q + 8; *outn = sz; rc = 0; goto done; }
                q += 8 + sz;
            }
        }
    }
done:
    free(tags);
    return rc;
}

/* ---------------- imagen ---------------- */
int box_tex_decode(const uint8_t *b, size_t n, int *ow, int *oh, uint8_t **orgba) {
    if (n < 128 || b[0] != 10) return -1;
    int enc = b[2], bpp = b[3];
    int w = (int)rd16(b + 8) - (int)rd16(b + 4) + 1, h = (int)rd16(b + 10) - (int)rd16(b + 6) + 1;
    int planes = b[65], bpl = (int)rd16(b + 66);
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || planes < 1 || planes > 8 || bpl <= 0) return -1;
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
    memset(pal, 0, sizeof pal);
    int np = 16;
    if (bpp == 8 && planes == 1) {
        np = 256;
        if (n >= 769 && b[n - 769] == 12) memcpy(pal, b + n - 768, 768);
        else for (int i = 0; i < 256; i++) pal[i][0] = pal[i][1] = pal[i][2] = (uint8_t)i;
    } else memcpy(pal, b + 16, 48);
    uint8_t *rgba = (uint8_t *)malloc((size_t)w * h * 4);
    if (!rgba) { free(raw); return -3; }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int v = 0;
            if (bpp == 8 && planes == 1) v = raw[(size_t)y * bpl + x];
            else if (bpp == 4 && planes == 1) { uint8_t c = raw[(size_t)y * bpl + (x >> 1)]; v = (x & 1) ? (c & 15) : (c >> 4); }
            else if (bpp == 1)
                for (int pl = 0; pl < planes; pl++) v |= ((raw[(size_t)y * bpl * planes + pl * bpl + (x >> 3)] >> (7 - (x & 7))) & 1) << pl;
            if (v >= np) v = 0;
            uint8_t *o = rgba + ((size_t)y * w + x) * 4;
            o[0] = pal[v][0]; o[1] = pal[v][1]; o[2] = pal[v][2]; o[3] = 255;
        }
    free(raw);
    *ow = w; *oh = h; *orgba = rgba;
    return 0;
}

/* ---------------- máscara ---------------- */
typedef struct { float x, y, sx, sy; } RawV;

static int get_rec(const uint8_t *b, size_t n, uint32_t idxo, uint32_t reco, int id, int k, float *sx, float *sy) {
    *sx = *sy = 0.f;
    size_t ip = (size_t)idxo + (size_t)(id - 1) * 4 + (size_t)k;
    if (ip >= n) return -1;
    int ri = b[ip];
    if (ri == 0) return 0;
    size_t rp = (size_t)reco + (size_t)(ri - 1) * 4;
    if (rp + 4 > n) return -1;
    int lo = b[rp], hi = b[rp + 1], dx = (int8_t)b[rp + 2], dy = (int8_t)b[rp + 3];
    *sx = (float)(((lo - hi) * dx) / 4);     /* división entera del original */
    *sy = (float)(((lo - hi) * dy) / 4);
    return 0;
}

static int cmp_ang(const void *pa, const void *pb) {
    const float *a = (const float *)pa, *b = (const float *)pb;
    return a[0] < b[0] ? -1 : (a[0] > b[0] ? 1 : 0);
}

int box_sight_parse(const uint8_t *b, size_t n, BoxSight *out) {
    memset(out, 0, sizeof *out);
    if (!b || n < 24) return -1;
    uint32_t dlen = rd16(b);
    int nprims = b[3];
    uint32_t eo = rd32(b + 4), po = rd32(b + 8), io = rd32(b + 12), ro = rd32(b + 16);
    if (nprims < 1 || (size_t)eo + (size_t)nprims * 4 > n || (size_t)po + dlen > n ||
        (size_t)io + (size_t)nprims * 4 > n || ro > n) return -2;

    /* primera pasada: lienzo y polígonos planos */
    typedef struct { RawV v[4]; } F4;
    F4 *f4 = (F4 *)calloc((size_t)nprims, sizeof(F4));
    if (!f4) return -2;
    int nf4 = 0, rc = 0;
    float maxx = 0, maxy = 0;
    for (int i = 0; i < nprims; i++) {
        int id = b[eo + 4 * i];
        uint32_t off = rd16(b + eo + 4 * i + 2);
        size_t p = (size_t)po + off;
        if (id < 1 || id > nprims || off + 8 > dlen || p + 8 > n) { rc = -2; goto fail; }
        int code = b[p + 7];
        if (code == 0x28) {
            if (off + 24 > dlen) { rc = -2; goto fail; }
            F4 *q = &f4[nf4];
            for (int k = 0; k < 4; k++) {
                q->v[k].x = (float)rs16(b + p + 8 + 4 * k);
                q->v[k].y = (float)rs16(b + p + 10 + 4 * k);
                if (get_rec(b, n, io, ro, id, k, &q->v[k].sx, &q->v[k].sy)) { rc = -2; goto fail; }
                if (q->v[k].x > maxx) maxx = q->v[k].x;
                if (q->v[k].y > maxy) maxy = q->v[k].y;
            }
            nf4++;
        } else if ((code & 0xFC) == 0x38) {
            if (off + 36 > dlen) { rc = -2; goto fail; }
            if (out->nband >= BOX_MAX_BAND) continue;
            BoxBandQuad *q = &out->band[out->nband++];
            for (int k = 0; k < 4; k++) {
                size_t xp = p + 8 + 8 * (size_t)k;         /* xy0 en +8, xy1 en +16, ... (el color k va delante) */
                q->v[k].x = (float)rs16(b + xp);
                q->v[k].y = (float)rs16(b + xp + 2);
                if (get_rec(b, n, io, ro, id, k, &q->v[k].sx, &q->v[k].sy)) { rc = -2; goto fail; }
                const uint8_t *c = (k == 0) ? b + p + 4 : b + p + 12 + 8 * (size_t)(k - 1);
                int mn = c[0] < c[1] ? c[0] : c[1]; if (c[2] < mn) mn = c[2];
                q->a[k] = (float)mn / 255.f;
            }
        }
    }
    if (maxx < 16 || maxy < 16) { rc = -3; goto fail; }
    out->canvas_w = (int)maxx; out->canvas_h = (int)maxy;

    /* contorno del hueco: vértices de los planos que no tocan el borde del lienzo, sin repetidos */
    {
        float ang[BOX_MAX_OUT * 2][2];   /* ángulo, índice */
        BoxVtx tmp[BOX_MAX_OUT];
        int nt = 0;
        for (int i = 0; i < nf4; i++)
            for (int k = 0; k < 4; k++) {
                RawV *r = &f4[i].v[k];
                if (r->x <= 0 || r->y <= 0 || r->x >= maxx || r->y >= maxy) continue;
                int dup = 0;
                for (int j = 0; j < nt; j++) if (tmp[j].x == r->x && tmp[j].y == r->y) { dup = 1; break; }
                if (dup) continue;
                if (nt >= BOX_MAX_OUT) { rc = -3; goto fail; }
                tmp[nt++] = (BoxVtx){r->x, r->y, r->sx, r->sy};
            }
        if (nt < 8) { rc = -3; goto fail; }
        float mx = 0, my = 0;
        for (int i = 0; i < nt; i++) { mx += tmp[i].x; my += tmp[i].y; }
        mx /= (float)nt; my /= (float)nt;
        for (int i = 0; i < nt; i++) { ang[i][0] = atan2f(tmp[i].y - my, tmp[i].x - mx); ang[i][1] = (float)i; }
        /* qsort sobre pares (ángulo, índice) */
        qsort(ang, (size_t)nt, sizeof ang[0], cmp_ang);
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (int i = 0; i < nt; i++) {
            out->out[i] = tmp[(int)ang[i][1]];
            if (out->out[i].x < x0) x0 = out->out[i].x;
            if (out->out[i].x > x1) x1 = out->out[i].x;
            if (out->out[i].y < y0) y0 = out->out[i].y;
            if (out->out[i].y > y1) y1 = out->out[i].y;
        }
        out->nout = nt;
        out->cx = 0.5f * (x0 + x1); out->cy = 0.5f * (y0 + y1);
    }
    out->from_disc = 1;
    free(f4);
    return 0;
fail:
    free(f4);
    memset(out, 0, sizeof *out);
    return rc;
}

/* Rectángulo redondeado: 4 arcos de 'seg' tramos; banda interior = mismo contorno encogido 'band' px con alfa 0 -> 1. */
void box_sight_fallback(BoxSight *s, int cw, int ch, float cx, float cy, float w, float h, float r, float band) {
    memset(s, 0, sizeof *s);
    s->canvas_w = cw; s->canvas_h = ch; s->cx = cx; s->cy = cy;
    float hw = 0.5f * w, hh = 0.5f * h;
    if (r > hh) r = hh;
    if (r > hw) r = hw;
    const int seg = 8;
    int n = 0;
    /* recorrido ordenado por ángulo (y abajo, ángulo creciente): cuadrantes abajo-dcha, abajo-izq, arriba-izq, arriba-dcha */
    static const float start[4] = {0.f, 1.57079633f, 3.14159265f, 4.71238898f};
    static const float ox[4] = {1, -1, -1, 1}, oy[4] = {1, 1, -1, -1};
    for (int c = 0; c < 4 && n + seg + 1 <= BOX_MAX_OUT; c++)
        for (int i = 0; i <= seg; i++) {
            float a = start[c] + 1.57079633f * (float)i / (float)seg;
            float x = cx + ox[c] * (hw - r) + r * cosf(a), y = cy + oy[c] * (hh - r) + r * sinf(a);
            s->out[n++] = (BoxVtx){x, y, 0.f, 0.f};
        }
    s->nout = n;
    /* desplazamiento de apertura: el hueco cerrado es la mitad (aprox.) y algo más bajo */
    for (int i = 0; i < n; i++) {
        s->out[i].sx = -(s->out[i].x - cx) * 0.5f;
        s->out[i].sy = -(s->out[i].y - cy) * 0.45f + 10.f;
    }
    /* banda: anillo entre el contorno (alfa 1) y el contorno encogido (alfa 0) */
    for (int i = 0; i < n && s->nband < BOX_MAX_BAND; i++) {
        int j = (i + 1) % n;
        BoxBandQuad *q = &s->band[s->nband++];
        const BoxVtx *a = &s->out[i], *b = &s->out[j];
        BoxVtx ai = *a, bi = *b;
        float la = hypotf(a->x - cx, a->y - cy), lb = hypotf(b->x - cx, b->y - cy);
        ai.x = a->x - (a->x - cx) / (la > 1 ? la : 1) * band; ai.y = a->y - (a->y - cy) / (la > 1 ? la : 1) * band;
        bi.x = b->x - (b->x - cx) / (lb > 1 ? lb : 1) * band; bi.y = b->y - (b->y - cy) / (lb > 1 ? lb : 1) * band;
        q->v[0] = ai; q->a[0] = 0.f;
        q->v[1] = *a; q->a[1] = 1.f;
        q->v[2] = bi; q->a[2] = 0.f;
        q->v[3] = *b; q->a[3] = 1.f;
    }
    s->from_disc = 0;
}
