/* Herramienta de captura/verificación del renderer de referencia (no forma parte de la app).
 *   capture_test <exe> <frames> <dir_salida> [N_hires=4] [ssaa=2]
 * Arranca el juego con el mismo guion de mando que el headless (círculo cada 97 frames, start en 120/400),
 * activa la captura en modo verificación, aplica CADA lista capturada a un rasterizador de referencia 1x
 * (comparado con la VRAM del oráculo al cerrar el frame) y a uno NxSSAA que solo dibuja en alta resolución
 * en las ventanas de volcado. Escribe PNG: oraculo, ref 1x, diferencia amplificada y ref NxSSAA. Disco: MV_DISC. */
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cdrom.h"
#include "cpu.h"
#include "gpu.h"
#include "hle.h"
#include "sys.h"
#include "ref_raster.h"

extern jmp_buf rt_fatal_jmp;
extern int rt_fatal_armed;

static uint32_t max_frames = 4000;
static const char *outdir;
static RefR *r1, *rn, *rh; /* rh: alta precision (CT_HIPREC=1; con CT_DEPTH=1 tambien profundidad) */
static int scale_n = 4, ssaa_n = 2;
static const uint32_t dump_frames[] = {300, 700, 1100, 1600, 2000, 2600, 3000, 3400, 3800, 3990};
#define NDUMP ((int)(sizeof dump_frames / sizeof *dump_frames))
#define HIRES_WINDOW 3 /* frames previos al volcado que también se dibujan en alta resolución */

/* ---- PNG mínimo (deflate "stored", sin compresión) ---- */
static uint32_t crc_tab[256];
static uint32_t crc32u(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_tab[1])
        for (uint32_t i = 0; i < 256; i++) { uint32_t x = i; for (int k = 0; k < 8; k++) x = x & 1 ? 0xEDB88320u ^ (x >> 1) : x >> 1; crc_tab[i] = x; }
    c = ~c;
    while (n--) c = crc_tab[(c ^ *p++) & 255] ^ (c >> 8);
    return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE *f, const char *t, const uint8_t *d, uint32_t n) {
    uint8_t h[8]; be32(h, n); memcpy(h + 4, t, 4);
    fwrite(h, 1, 8, f); if (n) fwrite(d, 1, n, f);
    uint32_t c = crc32u(0, (const uint8_t *)t, 4); c = crc32u(c, d, n);
    uint8_t e[4]; be32(e, c); fwrite(e, 1, 4, f);
}
static void write_png(const char *path, const uint8_t *rgb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ih[13]; be32(ih, w); be32(ih + 4, h); ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0;
    chunk(f, "IHDR", ih, 13);
    size_t rawn = (size_t)h * (1 + (size_t)w * 3);
    uint8_t *raw = malloc(rawn);
    for (int y = 0; y < h; y++) { raw[(size_t)y * (1 + w * 3)] = 0; memcpy(raw + (size_t)y * (1 + w * 3) + 1, rgb + (size_t)y * w * 3, (size_t)w * 3); }
    size_t nb = (rawn + 65534) / 65535, zn = 2 + rawn + nb * 5 + 4;
    uint8_t *z = malloc(zn), *q = z;
    *q++ = 0x78; *q++ = 1;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < rawn; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < rawn; off += 65535) {
        size_t n = rawn - off < 65535 ? rawn - off : 65535;
        *q++ = off + n >= rawn; *q++ = n; *q++ = n >> 8; *q++ = ~n; *q++ = ~n >> 8;
        memcpy(q, raw + off, n); q += n;
    }
    be32(q, b << 16 | a); q += 4;
    chunk(f, "IDAT", z, (uint32_t)(q - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f); free(raw); free(z);
}

/* ---- imagen del oráculo a partir de la VRAM capturada al cerrar (misma lectura que gpu_dump_ppm) ---- */
static void oracle_image(const uint16_t *vr, const GpuDisplay *d, uint8_t *out) {
    for (int y = 0; y < d->h; y++)
        for (int x = 0; x < d->w; x++) {
            uint8_t *o = out + ((size_t)y * d->w + x) * 3;
            if (d->depth24) {
                const uint8_t *ln = (const uint8_t *)(vr + (size_t)((d->y + y) & 511) * 1024);
                for (int k = 0; k < 3; k++) o[k] = ln[((d->x * 2) + x * 3 + k) & 2047];
            } else {
                uint16_t c = vr[(size_t)((d->y + y) & 511) * 1024 + ((d->x + x) & 1023)];
                o[0] = (uint8_t)((c & 31) << 3 | (c & 31) >> 2);
                o[1] = (uint8_t)(((c >> 5) & 31) << 3 | ((c >> 5) & 31) >> 2);
                o[2] = (uint8_t)(((c >> 10) & 31) << 3 | ((c >> 10) & 31) >> 2);
            }
        }
}

/* ---- estadísticas ---- */
static struct {
    uint32_t lists, vram_bad_lists, hires_bad_lists, flips, vblank_closes;
    uint64_t vram_diff_words, disp_diff_px, disp_px, disp_sum_abs;
    int disp_max_abs;
    uint64_t ops[7];
} S;
static int printed_bad;

/* ---- estadísticas de geometría de alta precisión (CT_HIPREC=1) ---- */
static struct {
    uint64_t polys, polys3d, polys_part, verts, verts_valid, poly_tex3d;
    uint64_t hist[8];  /* max |f - entero| por polígono 3D: <.5, <1, <2, <4, <8, <16, <64, resto */
    double sum_err; uint64_t n_err;
} H;
static void hp_stats(const GpuDrawList *l) {
    for (uint32_t i = 0; i < l->nops; i++) {
        const GpuOp *o = &l->ops[i];
        if (o->type != GPU_OP_POLY) continue;
        H.polys++;
        int nvalid = 0; float mx = 0;
        for (int k = 0; k < o->nv; k++) {
            H.verts++;
            if (!o->v[k].fvalid) continue;
            nvalid++; H.verts_valid++;
            float e = fmaxf(fabsf(o->v[k].fx - o->v[k].x), fabsf(o->v[k].fy - o->v[k].y));
            if (e > mx) mx = e;
            H.sum_err += e; H.n_err++;
        }
        if (nvalid == o->nv) {
            H.polys3d++; if (o->flags & GPU_F_TEXTURED) H.poly_tex3d++;
            int b = mx < .5f ? 0 : mx < 1 ? 1 : mx < 2 ? 2 : mx < 4 ? 3 : mx < 8 ? 4 : mx < 16 ? 5 : mx < 64 ? 6 : 7;
            H.hist[b]++;
        } else if (nvalid) H.polys_part++;
    }
}

static void compare_list(const GpuDrawList *l) {
    S.lists++;
    if (l->reason == GPU_CLOSE_FLIP) S.flips++; else S.vblank_closes++;
    for (uint32_t i = 0; i < l->nops; i++) S.ops[l->ops[i].type]++;
    if (!l->vram_after) return;
    const uint16_t *rv = ref_vram(r1);
    uint64_t nd = 0; int minx = 9999, miny = 9999, maxx = -1, maxy = -1;
    for (int i = 0; i < 1024 * 512; i++)
        if (rv[i] != l->vram_after[i]) {
            nd++;
            int x = i & 1023, y = i >> 10;
            if (x < minx) minx = x; if (x > maxx) maxx = x; if (y < miny) miny = y; if (y > maxy) maxy = y;
        }
    if (nd) {
        S.vram_bad_lists++; S.vram_diff_words += nd;
        if (printed_bad < 12) {
            printed_bad++;
            fprintf(stderr, "[diff] lista %u (frame %u, %u ops): %llu palabras VRAM distintas, bbox (%d,%d)-(%d,%d)\n", l->index,
                    l->host_frame, l->nops, (unsigned long long)nd, minx, miny, maxx, maxy);
        }
    }
    /* display 1x del modelo (ruta de alta resolución con M=1) frente al oráculo */
    int w, h;
    const uint8_t *img = ref_present(r1, &l->disp, &w, &h);
    uint8_t *ora = malloc((size_t)l->disp.w * l->disp.h * 3);
    oracle_image(l->vram_after, &l->disp, ora);
    uint64_t bad = 0;
    for (size_t i = 0; i < (size_t)w * h * 3; i += 3) {
        int m = 0;
        for (int k = 0; k < 3; k++) { int dd = abs((int)img[i + k] - (int)ora[i + k]); if (dd > m) m = dd; S.disp_sum_abs += dd; }
        if (m) { bad++; if (m > S.disp_max_abs) S.disp_max_abs = m; }
    }
    S.disp_px += (uint64_t)w * h; S.disp_diff_px += bad;
    if (bad) S.hires_bad_lists++;
    free(ora);
}

/* ---- metrica de "temblor" (CT_JITTER=1, requiere CT_HIPREC=1) ----
 * Se emparejan poligonos 3D (todos los vertices fvalid) entre listas consecutivas por una clave que NO incluye la posicion
 * (nv, flags, color, UV, CLUT, pagina, modo) + numero de ocurrencia dentro de la lista. Para cada vertice presente en tres listas
 * seguidas (t-1, t, t+1) y que se mueve poco (|d1| <= 6 px, descarta emparejamientos falsos) se mide la segunda diferencia
 * d2 = p(t+1) - 2 p(t) + p(t-1) por eje: una trayectoria suave a velocidad casi constante da d2 ~ 0; el redondeo a entero de la
 * GTE da saltos de +-1 px (d2 de 0, +-1, +-2). Se compara la posicion entera (lo que dibuja el hardware) y la float. */
typedef struct { uint64_t key; int nv; float ix[4], iy[4], fx[4], fy[4]; } JP;
static JP *jf[3]; static size_t jcnt[3], jcap[3];
static struct { uint64_t n_v, n_axis; double s2_int, s2_f; uint64_t big_int, big_f, zero_int, zero_f, moving; double sdev; } J;
static int jcmp(const void *a, const void *b) { uint64_t x = ((const JP *)a)->key, y = ((const JP *)b)->key; return x < y ? -1 : x > y; }
static void jitter_push(const GpuDrawList *l) {
    JP *t = jf[2]; jf[2] = jf[1]; jf[1] = jf[0]; jf[0] = t;
    size_t tn = jcnt[2], tc = jcap[2]; jcnt[2] = jcnt[1]; jcap[2] = jcap[1]; jcnt[1] = jcnt[0]; jcap[1] = jcap[0]; jcnt[0] = 0; jcap[0] = tc; (void)tn;
    static uint32_t *occ; static size_t occ_n;
    if (!occ) { occ_n = 1 << 16; occ = calloc(occ_n, 4); } else memset(occ, 0, occ_n * 4);
    for (uint32_t i = 0; i < l->nops; i++) {
        const GpuOp *o = &l->ops[i];
        if (o->type != GPU_OP_POLY) continue;
        int ok = 1;
        for (int k = 0; k < o->nv; k++) if (!o->v[k].fvalid) ok = 0;
        if (!ok) continue;
        uint64_t h = 1469598103934665603ull;
        #define MIX(x) h = (h ^ (uint64_t)(x)) * 1099511628211ull
        MIX(o->nv); MIX(o->flags); MIX(o->tp); MIX(o->tx); MIX(o->ty); MIX(o->clx); MIX(o->cly); MIX(o->abr);
        for (int k = 0; k < o->nv; k++) { MIX(o->v[k].u); MIX(o->v[k].v); MIX(o->v[k].r); MIX(o->v[k].g); MIX(o->v[k].b); }
        uint32_t *c = &occ[(h >> 7) & (occ_n - 1)]; /* colision de bucket = ocurrencias contadas juntas: aceptable (solo desplaza la numeracion) */
        h = h * 31 + (*c)++;
        if (jcnt[0] == jcap[0]) { jcap[0] = jcap[0] ? jcap[0] * 2 : 4096; jf[0] = realloc(jf[0], jcap[0] * sizeof(JP)); }
        JP *p = &jf[0][jcnt[0]++];
        p->key = h; p->nv = o->nv;
        for (int k = 0; k < o->nv; k++) { p->ix[k] = o->v[k].x - o->dox; p->iy[k] = o->v[k].y - o->doy; p->fx[k] = o->v[k].fx - o->dox; p->fy[k] = o->v[k].fy - o->doy; }
    }
    qsort(jf[0], jcnt[0], sizeof(JP), jcmp);
    if (!jcnt[1] || !jcnt[2]) return;
    /* triples: lista actual = t+1 (jf[0]), jf[1] = t, jf[2] = t-1 */
    for (size_t a = 0; a < jcnt[1]; a++) {
        const JP *m = &jf[1][a];
        JP kx = {m->key}; 
        if (a && jf[1][a - 1].key == m->key) continue; /* clave repetida: ambigua */
        const JP *n = bsearch(&kx, jf[0], jcnt[0], sizeof(JP), jcmp), *pv = bsearch(&kx, jf[2], jcnt[2], sizeof(JP), jcmp);
        if (!n || !pv) continue;
        for (int k = 0; k < m->nv; k++) {
            float di[2] = {m->ix[k] - pv->ix[k], m->iy[k] - pv->iy[k]}, dn[2] = {n->ix[k] - m->ix[k], n->iy[k] - m->iy[k]};
            float df[2] = {m->fx[k] - pv->fx[k], m->fy[k] - pv->fy[k]}, dfn[2] = {n->fx[k] - m->fx[k], n->fy[k] - m->fy[k]};
            if (fabsf(di[0]) > 6 || fabsf(di[1]) > 6 || fabsf(dn[0]) > 6 || fabsf(dn[1]) > 6) continue;
            if (fabsf(df[0]) > 6 || fabsf(df[1]) > 6 || fabsf(dfn[0]) > 6 || fabsf(dfn[1]) > 6) continue;
            /* solo vertices en movimiento real (la camara o el objeto avanza): el float se mueve mas de 0,05 px entre listas */
            if (fabsf(df[0]) + fabsf(df[1]) + fabsf(dfn[0]) + fabsf(dfn[1]) < 0.05f) continue;
            J.n_v++; J.moving++;
            for (int e = 0; e < 2; e++) {
                float d2i = dn[e] - di[e], d2f = dfn[e] - df[e];
                J.n_axis++; J.s2_int += fabsf(d2i); J.s2_f += fabsf(d2f);
                if (fabsf(d2i) >= 1.0f) J.big_int++;
                if (fabsf(d2f) >= 1.0f) J.big_f++;
                if (fabsf(d2i) < 0.5f) J.zero_int++;
                if (fabsf(d2f) < 0.5f) J.zero_f++;
            }
        }
    }
}

static int in_dump_window(uint32_t f) {
    for (int i = 0; i < NDUMP; i++) if (f + HIRES_WINDOW >= dump_frames[i] && f <= dump_frames[i]) return 1;
    return 0;
}

static GpuDisplay last_disp;
static uint16_t *last_vram_after;

static void dump_images(uint32_t f) {
    int w1, h1, wn, hn;
    const uint8_t *a = ref_present(r1, &last_disp, &w1, &h1);
    uint8_t *ora = malloc((size_t)last_disp.w * last_disp.h * 3);
    oracle_image(last_vram_after, &last_disp, ora);
    const uint8_t *b = ref_present(rn, &last_disp, &wn, &hn);
    char p[1024];
    snprintf(p, sizeof p, "%s/oraculo_%05u.png", outdir, f); write_png(p, ora, w1, h1);
    snprintf(p, sizeof p, "%s/ref1x_%05u.png", outdir, f); write_png(p, a, w1, h1);
    snprintf(p, sizeof p, "%s/ref%dx_ssaa%d_%05u.png", outdir, scale_n, ssaa_n, f); write_png(p, b, wn, hn);
    if (rh) {
        int wh, hh;
        const uint8_t *c = ref_present(rh, &last_disp, &wh, &hh);
        snprintf(p, sizeof p, "%s/hiprec%s_%dx_ssaa%d_%05u.png", outdir, getenv("CT_DEPTH") ? "_z" : "", scale_n, ssaa_n, f);
        write_png(p, c, wh, hh);
        /* diferencia normal vs alta precision (amplificada x4) */
        uint8_t *dd = malloc((size_t)wh * hh * 3);
        uint64_t nd = 0;
        for (size_t i = 0; i < (size_t)wh * hh * 3; i++) { int e = abs((int)b[i] - (int)c[i]); nd += e != 0; dd[i] = (uint8_t)(e * 4 > 255 ? 255 : e * 4); }
        snprintf(p, sizeof p, "%s/hpdiff_%dx_ssaa%d_%05u.png", outdir, scale_n, ssaa_n, f); write_png(p, dd, wh, hh);
        fprintf(stderr, "[hp] frame %u: normal vs alta precision: %llu/%llu canales distintos\n", f, (unsigned long long)nd, (unsigned long long)wh * hh * 3);
        free(dd);
    }
    uint8_t *df = malloc((size_t)w1 * h1 * 3);
    int nd = 0;
    for (size_t i = 0; i < (size_t)w1 * h1; i++) {
        int m = 0;
        for (int k = 0; k < 3; k++) { int dd = abs((int)a[i * 3 + k] - (int)ora[i * 3 + k]); if (dd > m) m = dd; }
        df[i * 3] = m ? 255 : 0; df[i * 3 + 1] = df[i * 3 + 2] = 0; nd += m != 0;
    }
    snprintf(p, sizeof p, "%s/diff1x_%05u.png", outdir, f); write_png(p, df, w1, h1);
    fprintf(stderr, "[dump] frame %u: display %dx%d, diff 1x vs oraculo: %d px (%s %dx%d, tiles %d)\n", f, w1, h1, nd, "ref", wn, hn, ref_tiles(rn));
    free(df); free(ora);
}

/* ---- guion de mando (idéntico al del headless: círculo cada 97 frames x6, start en 120 y 400 x6) ---- */
static void script_pad(uint32_t f) {
    static uint16_t last = 0xFFFF;
    uint16_t b = 0;
    if (f >= 97 && f % 97 < 6) b |= 1u << 13;
    if ((f >= 120 && f < 126) || (f >= 400 && f < 406)) b |= 1u << 3;
    if (getenv("CT_GAMEPLAY")) { /* guion largo del headless (bitácora §14): llega a partida */
        if (f >= 211 && f % 211 < 6) b |= 1u << 7;
        if (f >= 151 && f % 151 < 6) b |= 1u << 3;
    }
    if (b != last) { pad_set_state(0, b, 0x80, 0x80, 0x80, 0x80); last = b; }
}

static void on_frame(Cpu *c, uint32_t f) {
    (void)c;
    alarm(30);
    script_pad(f);
    gpu_capture_vblank();
    const GpuDrawList *l;
    int dumped_now = 0;
    while ((l = gpu_capture_frame())) {
        int hi = in_dump_window(f);
        ref_apply(r1, l, 1);
        ref_apply(rn, l, hi);
        if (rh) ref_apply(rh, l, hi);
        compare_list(l);
        if (getenv("CT_HIPREC")) hp_stats(l);
        if (getenv("CT_JITTER")) jitter_push(l);
        last_disp = l->disp;
        if (l->vram_after) {
            if (!last_vram_after) last_vram_after = malloc(1024 * 512 * 2);
            memcpy(last_vram_after, l->vram_after, 1024 * 512 * 2);
        }
        (void)dumped_now;
    }
    for (int i = 0; i < NDUMP; i++)
        if (f == dump_frames[i] && last_vram_after) dump_images(f);
    if (f % 250 == 0)
        fprintf(stderr, "[ct] frame %u: listas %u, VRAM mal %u, display mal %u\n", f, S.lists, S.vram_bad_lists, S.hires_bad_lists);
    if (f >= max_frames) {
        fprintf(stderr, "[ct] FIN %u frames\n", f);
        fprintf(stderr, "[ct] listas=%u (cierre por flip %u, por vblank %u) perdidas=%u\n", S.lists, S.flips, S.vblank_closes, gpu_capture_dropped());
        fprintf(stderr, "[ct] ops: poly=%llu rect=%llu line=%llu fill=%llu vcopy=%llu upload=%llu download=%llu\n",
                (unsigned long long)S.ops[0], (unsigned long long)S.ops[1], (unsigned long long)S.ops[2], (unsigned long long)S.ops[3],
                (unsigned long long)S.ops[4], (unsigned long long)S.ops[5], (unsigned long long)S.ops[6]);
        fprintf(stderr, "[ct] VRAM 1x vs oraculo: %u/%u listas con diferencias, %llu palabras en total\n", S.vram_bad_lists, S.lists,
                (unsigned long long)S.vram_diff_words);
        fprintf(stderr, "[ct] display 1x vs oraculo: %u/%u listas con diferencias, %llu/%llu px (%.6f%%), canal max |d|=%d, media |d|=%.6f\n",
                S.hires_bad_lists, S.lists, (unsigned long long)S.disp_diff_px, (unsigned long long)S.disp_px,
                S.disp_px ? 100.0 * (double)S.disp_diff_px / (double)S.disp_px : 0.0, S.disp_max_abs,
                S.disp_px ? (double)S.disp_sum_abs / (3.0 * (double)S.disp_px) : 0.0);
        if (getenv("CT_JITTER") && J.n_axis)
            fprintf(stderr, "[jitter] %llu vertices en movimiento (%llu ejes) en triples consecutivos: |d2| medio entero=%.4f px float=%.4f px; "
                    "|d2|>=1px: entero %.2f%% float %.2f%%; |d2|<0.5px: entero %.2f%% float %.2f%%\n",
                    (unsigned long long)J.n_v, (unsigned long long)J.n_axis, J.s2_int / (double)J.n_axis, J.s2_f / (double)J.n_axis,
                    100.0 * (double)J.big_int / (double)J.n_axis, 100.0 * (double)J.big_f / (double)J.n_axis,
                    100.0 * (double)J.zero_int / (double)J.n_axis, 100.0 * (double)J.zero_f / (double)J.n_axis);
        if (getenv("CT_HIPREC")) {
            GteHpStats g; gte_hp_stats(&g);
            fprintf(stderr, "[hp] polys=%llu 3D(todos fvalid)=%llu (texturizados %llu) parciales=%llu; vertices %llu/%llu fvalid\n",
                    (unsigned long long)H.polys, (unsigned long long)H.polys3d, (unsigned long long)H.poly_tex3d,
                    (unsigned long long)H.polys_part, (unsigned long long)H.verts_valid, (unsigned long long)H.verts);
            fprintf(stderr, "[hp] max|f-int| por poligono 3D: <.5:%llu <1:%llu <2:%llu <4:%llu <8:%llu <16:%llu <64:%llu mas:%llu; media %.3f px\n",
                    (unsigned long long)H.hist[0], (unsigned long long)H.hist[1], (unsigned long long)H.hist[2], (unsigned long long)H.hist[3],
                    (unsigned long long)H.hist[4], (unsigned long long)H.hist[5], (unsigned long long)H.hist[6], (unsigned long long)H.hist[7],
                    H.n_err ? H.sum_err / (double)H.n_err : 0.0);
            fprintf(stderr, "[hp] tabla GTE: almacenados=%u desalojos=%u omitidos(saturados)=%u consultas=%u aciertos=%u; omitidos: sf0=%u sat=%u div=%u z=%u; max por frame=%u\n", g.stored, g.evict, g.skipped, g.lookups, g.hits, g.skip_sf, g.skip_sat, g.skip_ovf, g.skip_z, g.max_gen_stored);
        }
        exit(0);
    }
}

static void on_alarm(int s) { (void)s; rt_fatal(NULL, "sin VBlank en 30 s"); }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "uso: %s <exe> <frames> <dir_salida> [N=4] [ssaa=2]\n", argv[0]); return 1; }
    const char *exe = argv[1], *disc = getenv("MV_DISC");
    max_frames = (uint32_t)strtoul(argv[2], 0, 0);
    outdir = argv[3];
    if (argc > 4) scale_n = atoi(argv[4]);
    if (argc > 5) ssaa_n = atoi(argv[5]);
    mkdir(outdir, 0755);
    FILE *f = fopen(exe, "rb");
    if (!f) { perror(exe); return 1; }
    static uint8_t buf[EMU_RAM_SIZE + 0x800];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n < 0x800 || memcmp(buf, EXE_MAGIC, 8)) { fprintf(stderr, "ejecutable no válido\n"); return 1; }
    uint32_t pc = rd32(buf + 0x10), gp = rd32(buf + 0x14), t = rd32(buf + 0x18), sz = rd32(buf + 0x1C);
    memcpy(&emu_ram[t & 0x1FFFFF], buf + 0x800, sz);
    if (disc && !cd_open(disc)) { fprintf(stderr, "sin discos\n"); return 1; }
    RefOpts o1 = {1, 1, getenv("CT_R1_HQ") ? REF_HQ : 0}, on = {scale_n, ssaa_n, 0}; /* CT_R1_HQ=1: 1x con muestreo al centro y 8 bits (mide la desviacion de convencion) */
    r1 = ref_create(&o1);
    rn = ref_create(&on);
    if (getenv("CT_HIPREC")) {
        RefOpts oh = on; oh.flags |= REF_FLOAT_VTX | (getenv("CT_DEPTH") ? REF_DEPTH : 0);
        rh = ref_create(&oh);
    }
    Cpu c = {0};
    c.r[28] = gp;
    c.r[29] = c.r[30] = 0x801FFFF0u;
    gte_reset();
    gpu_reset();
    if (getenv("CT_HIPREC")) gpu_capture_hiprec(1);
    gpu_capture_enable(2);
    rt_host_frame = on_frame;
    signal(SIGALRM, on_alarm);
    rt_fatal_armed = 1;
    if (setjmp(rt_fatal_jmp)) return 2;
    alarm(30);
    rt_call(&c, pc);
    return 0;
}
