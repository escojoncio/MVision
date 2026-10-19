/* nssolid: geometría sólida de la escena nativa de un volcado y análisis de huecos del escenario (aristas libres) para la colisión de la cabeza.
 * Uso: nssolid DIR_VOLCADO [-v]. Imprime: estadísticas del extractor, nº de triángulos sólidos (escenario / dinámicos / semitransparentes excluidos) y el análisis de
 * aristas del escenario: aristas compartidas por 2 caras (cerradas), por 3 o más (no variedad), y aristas de borde (1 sola cara) clasificadas en «contacto» (sus dos extremos
 * y su punto medio tocan otra superficie: costura entre mallas) o «libres» (hay un hueco por donde se ve al otro lado). NO ejecuta el juego. */
#include "../ns_dump.h"
#include "../ns_extract.h"
#include "../ns_solid.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int64_t x, y, z; } Q;
static uint64_t hq(int64_t x, int64_t y, int64_t z) { uint64_t h = (uint64_t)x * 0x9E3779B97F4A7C15ull ^ ((uint64_t)y + 0x7F4A7C15ull) * 0xC2B2AE3D27D4EB4Full ^ ((uint64_t)z + 0x1234567ull) * 0x165667B19E3779F9ull; h ^= h >> 29; return h ? h : 1; }

/* tabla de vértices soldados por posición (rejilla de TOL) */
typedef struct { uint64_t k; int64_t x, y, z; uint32_t id; } VE;
static VE *vt; static size_t vcap, vn;
static float TOL = 2.0f;
static uint32_t vid(const float p[3]) {
    int64_t x = llroundf(p[0] / TOL), y = llroundf(p[1] / TOL), z = llroundf(p[2] / TOL);
    uint64_t k = hq(x, y, z);
    size_t i = k & (vcap - 1);
    while (vt[i].k) { if (vt[i].k == k && vt[i].x == x && vt[i].y == y && vt[i].z == z) return vt[i].id; i = (i + 1) & (vcap - 1); }
    vt[i].k = k; vt[i].x = x; vt[i].y = y; vt[i].z = z; vt[i].id = (uint32_t)vn++;
    return vt[i].id;
}
typedef struct { uint32_t a, b; uint32_t cnt; uint32_t tri; float pa[3], pb[3]; } ED;
static ED *et; static size_t ecap, en;
static ED *edge(uint32_t a, uint32_t b, const float pa[3], const float pb[3], uint32_t tri) {
    if (a > b) { uint32_t t = a; a = b; b = t; const float *q = pa; pa = pb; pb = q; }
    uint64_t k = ((uint64_t)a << 32 | b) * 0x9E3779B97F4A7C15ull; k ^= k >> 31;
    size_t i = k & (ecap - 1);
    while (et[i].cnt) { if (et[i].a == a && et[i].b == b) { et[i].cnt++; return &et[i]; } i = (i + 1) & (ecap - 1); }
    et[i].a = a; et[i].b = b; et[i].cnt = 1; et[i].tri = tri; memcpy(et[i].pa, pa, 12); memcpy(et[i].pb, pb, 12); en++;
    return &et[i];
}

/* distancia punto-triángulo */
static void sub(const float a[3], const float b[3], float o[3]) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float tri_dist(const float *T, const float p[3]) {
    const float *a = T, *b = T + 3, *c = T + 6;
    float ab[3], ac[3], ap[3], q[3];
    sub(b, a, ab); sub(c, a, ac); sub(p, a, ap);
    float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) memcpy(q, a, 12);
    else {
        float bp[3]; sub(p, b, bp); float d3 = dot(ab, bp), d4 = dot(ac, bp);
        if (d3 >= 0 && d4 <= d3) memcpy(q, b, 12);
        else {
            float vc = d1 * d4 - d3 * d2;
            if (vc <= 0 && d1 >= 0 && d3 <= 0) { float t = d1 / (d1 - d3); for (int i = 0; i < 3; i++) q[i] = a[i] + t * ab[i]; }
            else {
                float cp[3]; sub(p, c, cp); float d5 = dot(ab, cp), d6 = dot(ac, cp);
                if (d6 >= 0 && d5 <= d6) memcpy(q, c, 12);
                else {
                    float vb = d5 * d2 - d1 * d6;
                    if (vb <= 0 && d2 >= 0 && d6 <= 0) { float t = d2 / (d2 - d6); for (int i = 0; i < 3; i++) q[i] = a[i] + t * ac[i]; }
                    else {
                        float va = d3 * d6 - d5 * d4;
                        if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) { float t = (d4 - d3) / ((d4 - d3) + (d5 - d6)); for (int i = 0; i < 3; i++) q[i] = b[i] + t * (c[i] - b[i]); }
                        else { float den = 1.0f / (va + vb + vc), v = vb * den, w = vc * den; for (int i = 0; i < 3; i++) q[i] = a[i] + ab[i] * v + ac[i] * w; }
                    }
                }
            }
        }
    }
    float d[3]; sub(p, q, d); return sqrtf(dot(d, d));
}

/* rejilla uniforme de triángulos (celdas de CELL unidades) */
#define CELL 512.0f
typedef struct { uint64_t k; uint32_t first, n; } GC;
static GC *gt; static size_t gcap; static uint32_t *glist; static size_t gl_n;
static const float *TR; static size_t NT;
static size_t gfind(int64_t x, int64_t y, int64_t z) { uint64_t k = hq(x, y, z); size_t i = k & (gcap - 1); while (gt[i].k && gt[i].k != k) i = (i + 1) & (gcap - 1); return i; }
static int touches(const float p[3], uint32_t skip_tri, float tol) {
    int64_t cx = (int64_t)floorf(p[0] / CELL), cy = (int64_t)floorf(p[1] / CELL), cz = (int64_t)floorf(p[2] / CELL);
    for (int64_t x = cx - 1; x <= cx + 1; x++) for (int64_t y = cy - 1; y <= cy + 1; y++) for (int64_t z = cz - 1; z <= cz + 1; z++) {
        size_t i = gfind(x, y, z);
        if (!gt[i].k) continue;
        for (uint32_t j = 0; j < gt[i].n; j++) {
            uint32_t t = glist[gt[i].first + j];
            if (t == skip_tri) continue;
            if (tri_dist(TR + 9 * (size_t)t, p) <= tol) return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: nssolid DIR [-v] [tol]\n"); return 2; }
    int verbose = argc > 2 && !strcmp(argv[2], "-v");
    if (argc > 3) TOL = (float)atof(argv[3]);
    NsDump *d = ns_dump_open(argv[1]);
    if (!d) { fprintf(stderr, "no se pudo abrir %s\n", argv[1]); return 1; }
    NsScene sc; memset(&sc, 0, sizeof sc);
    if (ns_scene_alloc(&sc, 1, 1, 1, 0)) return 1;
    NsMeshStore *st = ns_store_create();
    NsExtractStats es; memset(&es, 0, sizeof es);
    int rc = ns_extract_ex(&sc, st, d->ram, NS_RAM_SIZE, d->scratch, 1024u, NULL, &es);
    printf("[extractor] rc=%d valid=0x%x instancias=%u (escenario %u) piezas=%u ocultas=%u sin_modelo(drop_bad_model)=%u almacen_lleno=%u truncado=0x%x descartadas inst/pieza=%u/%u\n",
           rc, sc.valid, sc.n_inst, es.n_static, sc.n_piece, es.n_hidden_pieces, es.drop_bad_model, es.store_full, sc.truncated, sc.n_dropped_inst, sc.n_dropped_piece);
    size_t cap = 1u << 20;
    float *tris = malloc(cap * 36); NsSolidSrc *src = malloc(cap * sizeof *src);
    NsSolidOpts o; memset(&o, 0, sizeof o);
    o.include_blend = getenv("NSSOLID_BLEND") != NULL;
    NsSolidStats S;
    size_t n = ns_solid_collect(&sc, st, &o, tris, src, cap, &S);
    o.include_blend = 1;
    NsSolidStats S2; float *t2 = malloc(cap * 36);
    size_t n2 = ns_solid_collect(&sc, st, &o, t2, NULL, cap, &S2);
    free(t2);
    printf("[solido] triangulos=%zu (escenario %u, dinamicos %u) | semitransparentes excluidos: piezas=%u triangulos=%zu | piezas=%u ocultas=%u malla_ausente=%u desbordados=%u\n",
           n, S.tris_static, S.tris_dyn, S.pieces_blend, n2 - n, S.pieces, S.pieces_hidden, S.pieces_no_mesh, S.overflow);
    /* alcance: triángulos del escenario por octante respecto del ojo del juego (360 grados) */
    {
        float *E = sc.cam.eye; unsigned oct[8] = {0}; unsigned behind = 0, ns = 0;
        for (size_t i = 0; i < n; i++) {
            if (!src[i].is_static) continue;
            float cx = (tris[9 * i] + tris[9 * i + 3] + tris[9 * i + 6]) / 3 - E[0], cy = (tris[9 * i + 1] + tris[9 * i + 4] + tris[9 * i + 7]) / 3 - E[1], cz = (tris[9 * i + 2] + tris[9 * i + 5] + tris[9 * i + 8]) / 3 - E[2];
            if (cx * cx + cy * cy + cz * cz > 4000.0f * 4000.0f) continue;
            const float *R = sc.cam.rot; float vz = R[2] * cx + R[5] * cy + R[8] * cz;   /* z de vista = delante */
            ns++; if (vz < 0) behind++;
            oct[(cx > 0) | ((cy > 0) << 1) | ((cz > 0) << 2)]++;
        }
        printf("[alcance] escenario a menos de 4 m del ojo: %u triangulos, %u (%.1f %%) DETRAS de la camara del juego (la lista de la GPU no los trae)\n", ns, behind, ns ? 100.0 * behind / ns : 0.0);
    }
    /* rayos desde el ojo del juego y desde puntos de la habitación (cabeza física: ±0,6 m): fracción que ESCAPA sin tocar nada = lugares por donde se vería al otro lado */
    {
        const float *E = sc.cam.eye;
        const float off[9][3] = {{0, 0, 0}, {600, 0, 0}, {-600, 0, 0}, {0, 0, 600}, {0, 0, -600}, {0, 400, 0}, {0, -400, 0}, {400, 0, 400}, {-400, 0, -400}};
        const int NR = 6000;
        unsigned esc_all = 0, tot = 0, esc_static = 0;
        unsigned esc_oct[8] = {0}, esc_up = 0, esc_down = 0; float esc_sum[3] = {0, 0, 0}; unsigned elev[6] = {0};
        float first_esc[3][3]; int nfe = 0;
        for (int pi = 0; pi < 9; pi++) {
            float O[3] = {E[0] + off[pi][0], E[1] + off[pi][1], E[2] + off[pi][2]};
            for (int r = 0; r < NR; r++) {   /* espiral de Fibonacci sobre la esfera */
                float z = 1.0f - 2.0f * ((float)r + 0.5f) / (float)NR, rr = sqrtf(1.0f - z * z), ph = 2.399963f * (float)r;
                float D[3] = {rr * cosf(ph), z, rr * sinf(ph)};
                int hit_any = 0, hit_static = 0;
                for (size_t i = 0; i < n && !(hit_any && hit_static); i++) {
                    const float *a = tris + 9 * i, *b = a + 3, *c2 = a + 6;
                    float e1[3], e2[3]; sub(b, a, e1); sub(c2, a, e2);
                    float pv[3] = {D[1] * e2[2] - D[2] * e2[1], D[2] * e2[0] - D[0] * e2[2], D[0] * e2[1] - D[1] * e2[0]};
                    float det = dot(e1, pv);
                    if (fabsf(det) < 1e-9f) continue;
                    float inv = 1.0f / det, tv[3]; sub(O, a, tv);
                    float u = dot(tv, pv) * inv; if (u < -1e-4f || u > 1.0001f) continue;
                    float qv[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0]};
                    float v = dot(D, qv) * inv; if (v < -1e-4f || u + v > 1.0002f) continue;
                    float t = dot(e2, qv) * inv; if (t <= 1.0f) continue;
                    hit_any = 1; if (src[i].is_static) hit_static = 1;
                }
                tot++;
                if (!hit_any) { esc_all++; esc_oct[(D[0] > 0) | ((D[1] > 0) << 1) | ((D[2] > 0) << 2)]++; if (D[1] > 0) esc_up++; else esc_down++; if (nfe < 3) { memcpy(first_esc[nfe], D, 12); nfe++; } for (int q = 0; q < 3; q++) esc_sum[q] += D[q]; { int e = (int)((D[1] + 1.0f) * 3.0f); if (e > 5) e = 5; elev[e]++; } }
                if (!hit_static) esc_static++;
            }
        }
        printf("[rayos] %u rayos (9 puntos x %d direcciones, desde el ojo del juego y a +-0,6 m / +-0,4 m): escapan sin tocar NADA = %u (%.2f %%): hacia +y=%u hacia -y=%u | solo con el escenario escapan %u (%.2f %%)\n",
               tot, NR, esc_all, 100.0 * esc_all / tot, esc_up, esc_down, esc_static, 100.0 * esc_static / tot);
        printf("[rayos] direccion media de escape (%.2f, %.2f, %.2f); por altura de y=-1..1 en 6 tramos: %u %u %u %u %u %u\n", esc_all ? esc_sum[0] / esc_all : 0, esc_all ? esc_sum[1] / esc_all : 0, esc_all ? esc_sum[2] / esc_all : 0, elev[0], elev[1], elev[2], elev[3], elev[4], elev[5]);
        printf("[rayos] escapes por octante (x+,y+,z+ bits 0,1,2): %u %u %u %u %u %u %u %u\n", esc_oct[0], esc_oct[1], esc_oct[2], esc_oct[3], esc_oct[4], esc_oct[5], esc_oct[6], esc_oct[7]);
    }
    /* aristas del escenario */
    size_t nst = 0;
    float *sT = malloc(cap * 36); uint32_t *sIdx = malloc(cap * 4);
    for (size_t i = 0; i < n; i++) if (src[i].is_static) { memcpy(sT + 9 * nst, tris + 9 * i, 36); sIdx[nst] = (uint32_t)i; nst++; }
    TR = sT; NT = nst;
    vcap = 1; while (vcap < nst * 6 + 16) vcap <<= 1; vt = calloc(vcap, sizeof *vt);
    ecap = 1; while (ecap < nst * 6 + 16) ecap <<= 1; et = calloc(ecap, sizeof *et);
    for (size_t t = 0; t < nst; t++) {
        uint32_t v[3]; for (int k = 0; k < 3; k++) v[k] = vid(sT + 9 * t + 3 * k);
        for (int k = 0; k < 3; k++) edge(v[k], v[(k + 1) % 3], sT + 9 * t + 3 * k, sT + 9 * t + 3 * ((k + 1) % 3), (uint32_t)t);
    }
    /* rejilla de triángulos por celdas que solapan su caja */
    size_t inc = 0;   /* incidencias (triángulo, celda) para dimensionar la tabla */
    for (size_t t = 0; t < nst; t++) {
        float mn[3], mx[3];
        for (int a = 0; a < 3; a++) { mn[a] = fminf(sT[9 * t + a], fminf(sT[9 * t + 3 + a], sT[9 * t + 6 + a])); mx[a] = fmaxf(sT[9 * t + a], fmaxf(sT[9 * t + 3 + a], sT[9 * t + 6 + a])); }
        int64_t nc = ((int64_t)floorf(mx[0] / CELL) - (int64_t)floorf(mn[0] / CELL) + 1) * ((int64_t)floorf(mx[1] / CELL) - (int64_t)floorf(mn[1] / CELL) + 1) * ((int64_t)floorf(mx[2] / CELL) - (int64_t)floorf(mn[2] / CELL) + 1);
        if (nc <= 4096) inc += (size_t)nc;
    }
    gcap = 1; while (gcap < inc * 2 + 16) gcap <<= 1; gt = calloc(gcap, sizeof *gt);
    glist = malloc((inc + 1) * 4);
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) { size_t acc = 0; for (size_t i = 0; i < gcap; i++) if (gt[i].k) { gt[i].first = (uint32_t)acc; acc += gt[i].n; gt[i].n = 0; } glist = realloc(glist, (acc + 1) * 4); gl_n = acc; }
        for (size_t t = 0; t < nst; t++) {
            float mn[3], mx[3];
            for (int a = 0; a < 3; a++) { mn[a] = fminf(sT[9 * t + a], fminf(sT[9 * t + 3 + a], sT[9 * t + 6 + a])); mx[a] = fmaxf(sT[9 * t + a], fmaxf(sT[9 * t + 3 + a], sT[9 * t + 6 + a])); }
            int64_t x0 = (int64_t)floorf(mn[0] / CELL), x1 = (int64_t)floorf(mx[0] / CELL), y0 = (int64_t)floorf(mn[1] / CELL), y1 = (int64_t)floorf(mx[1] / CELL), z0 = (int64_t)floorf(mn[2] / CELL), z1 = (int64_t)floorf(mx[2] / CELL);
            if ((x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > 4096) continue;   /* triángulo gigante: se salta (no hay tantos) */
            for (int64_t x = x0; x <= x1; x++) for (int64_t y = y0; y <= y1; y++) for (int64_t z = z0; z <= z1; z++) {
                uint64_t k = hq(x, y, z); size_t i = k & (gcap - 1);
                while (gt[i].k && gt[i].k != k) i = (i + 1) & (gcap - 1);
                if (!gt[i].k) { gt[i].k = k; }
                if (pass == 1) glist[gt[i].first + gt[i].n] = (uint32_t)t;
                gt[i].n++;
            }
        }
        if (pass == 0) { /* gt[i].n cuenta */ }
    }
    size_t shared2 = 0, shared3 = 0, bnd = 0, bnd_contact = 0, bnd_free = 0;
    double len_free = 0, len_bnd = 0;
    size_t hist[4] = {0}, free_near = 0, bnd_near = 0;   /* libres por longitud: <100, <500, <2000, >=2000 unidades */
    typedef struct { float len; float a[3], b[3]; uint32_t key; uint16_t piece; uint32_t mesh; } FE;
    FE worst[12]; int nw = 0;
    for (size_t i = 0; i < ecap; i++) {
        if (!et[i].cnt) continue;
        if (et[i].cnt == 2) { shared2++; continue; }
        if (et[i].cnt > 2) { shared3++; continue; }
        bnd++;
        float emid[3] = {(et[i].pa[0] + et[i].pb[0]) / 2 - sc.cam.eye[0], (et[i].pa[1] + et[i].pb[1]) / 2 - sc.cam.eye[1], (et[i].pa[2] + et[i].pb[2]) / 2 - sc.cam.eye[2]};
        int near4 = emid[0] * emid[0] + emid[1] * emid[1] + emid[2] * emid[2] < 4000.0f * 4000.0f;
        bnd_near += near4;
        float dd[3]; sub(et[i].pb, et[i].pa, dd); float len = sqrtf(dot(dd, dd));
        len_bnd += len;
        float mid[3] = {(et[i].pa[0] + et[i].pb[0]) / 2, (et[i].pa[1] + et[i].pb[1]) / 2, (et[i].pa[2] + et[i].pb[2]) / 2};
        float tolc = TOL * 2.0f;
        int ct = touches(et[i].pa, et[i].tri, tolc) && touches(et[i].pb, et[i].tri, tolc) && touches(mid, et[i].tri, tolc);
        if (ct) { bnd_contact++; continue; }
        bnd_free++; len_free += len; free_near += near4;
        hist[len < 100 ? 0 : len < 500 ? 1 : len < 2000 ? 2 : 3]++;
        const NsSolidSrc *s = &src[sIdx[et[i].tri]];
        FE f = {len, {et[i].pa[0], et[i].pa[1], et[i].pa[2]}, {et[i].pb[0], et[i].pb[1], et[i].pb[2]}, s->key, s->piece_rel, s->mesh32};
        int pos = nw < 12 ? nw++ : 11;
        if (pos == 11 && nw == 12 && worst[11].len >= len) continue;
        worst[pos] = f;
        for (int j = pos; j > 0 && worst[j].len > worst[j - 1].len; j--) { FE t = worst[j]; worst[j] = worst[j - 1]; worst[j - 1] = t; }
    }
    printf("[aristas] escenario: triangulos=%zu vertices_soldados=%zu aristas=%zu | compartidas_por_2=%zu (%.1f %%) compartidas_por_3+=%zu | de_borde(1 cara)=%zu (%.1f %%, %.0f unidades)\n",
           nst, vn, en, shared2, en ? 100.0 * shared2 / en : 0.0, shared3, bnd, en ? 100.0 * bnd / en : 0.0, len_bnd);
    printf("[aristas] de borde: en_contacto_con_otra_superficie=%zu (costura entre mallas) | LIBRES=%zu (%.0f unidades de longitud; <10cm=%zu, 10-50cm=%zu, 0,5-2m=%zu, >=2m=%zu)\n",
           bnd_contact, bnd_free, len_free, hist[0], hist[1], hist[2], hist[3]);
    printf("[aristas] a menos de 4 m del ojo: de_borde=%zu, LIBRES=%zu\n", bnd_near, free_near);
    for (int i = 0; i < nw && (verbose || i < 6); i++)
        printf("[aristas]   libre %d: len=%.0f de (%.0f,%.0f,%.0f) a (%.0f,%.0f,%.0f) objeto=%08x pieza=%u malla=%08x\n", i + 1, worst[i].len, worst[i].a[0], worst[i].a[1], worst[i].a[2], worst[i].b[0], worst[i].b[1], worst[i].b[2], worst[i].key, worst[i].piece, worst[i].mesh);
    return 0;
}
