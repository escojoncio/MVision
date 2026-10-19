/* Validación del modelo de iluminación contra un volcado de RAM (2 MiB, mismo mapa de direcciones que el juego).
 *   ns_lightcheck <ram.bin>
 * Busca los objetos 3D de la escena por su estructura, y para cada uno compara el color por vértice que dejó el
 * motor en los paquetes de dibujo (lo que recibe la GPU) con el que calcula ns_light:
 *   - objetos dinámicos (marca de sombreado): matrices de luz del propio objeto leídas de RAM;
 *   - mapa (color precalculado): se recalcula con la luz global, el ambiente y las luces fijas de la RAM.
 * Direcciones de los globales: obligatorias, por variables de entorno NS_DIR_LUZ, NS_AMBIENTE, NS_LUCES_FIJAS
 * (propias de cada versión; no hay valores por defecto). Salida: coincidencias exactas y error medio por canal (en bytes). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include "ns_light.h"

static uint8_t *ram;
#define RAM_SIZE 0x200000u
static int okp(uint32_t p) { return p >= 0x80010000u && p < 0x80200000u; }
/* Accesores acotados: un acceso que no cabe entero dentro de la RAM devuelve 0 (nunca lee fuera). */
static uint32_t U32(uint32_t a) { uint32_t v = 0; uint32_t o = a & 0x1FFFFF; if (o <= RAM_SIZE - 4) memcpy(&v, ram + o, 4); return v; }
static int32_t S32(uint32_t a) { return (int32_t)U32(a); }
static uint16_t U16(uint32_t a) { uint16_t v = 0; uint32_t o = a & 0x1FFFFF; if (o <= RAM_SIZE - 2) memcpy(&v, ram + o, 2); return v; }
static int16_t S16(uint32_t a) { return (int16_t)U16(a); }
static uint8_t U8(uint32_t a) { return ram[a & 0x1FFFFF]; }
/* Dirección obligatoria por entorno; 0 si falta o es inválida. */
static int envu(const char *n, uint32_t *out) {
    const char *s = getenv(n);
    if (!s || !*s) { fprintf(stderr, "falta la variable %s\n", n); return 0; }
    char *end; errno = 0;
    unsigned long v = strtoul(s, &end, 0);
    if (errno || *end || v > 0xFFFFFFFFul) { fprintf(stderr, "valor inválido en %s\n", n); return 0; }
    *out = (uint32_t)v;
    return 1;
}

static void rd_mat(uint32_t a, int16_t m[3][3]) { for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) m[i][j] = S16(a + (i * 3 + j) * 2); }

typedef struct { long ok, bad; double sad; } Stat;
static void cmp(Stat *s, const uint8_t *got, const uint8_t *exp) {
    int same = 1;
    for (int k = 0; k < 3; k++) { int d = got[k] - exp[k]; if (d) same = 0; s->sad += d < 0 ? -d : d; }
    if (same) s->ok++; else s->bad++;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: %s ram.bin\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    ram = calloc(1, RAM_SIZE + 8);
    size_t got = fread(ram, 1, RAM_SIZE, f); fclose(f);
    if (got < RAM_SIZE) { fprintf(stderr, "volcado corto (%zu)\n", got); return 2; }
    uint32_t a_dir, a_amb, a_fix;
    if (!envu("NS_DIR_LUZ", &a_dir) || !envu("NS_AMBIENTE", &a_amb) || !envu("NS_LUCES_FIJAS", &a_fix)) return 2;

    NsLightEnv env; int16_t LM[3][3], CM[3][3];
    rd_mat(a_dir, LM); rd_mat(a_dir + 32, CM);
    for (int i = 0; i < 3; i++) { env.main_dir[i] = LM[0][i]; env.main_col[i] = CM[i][0]; env.ambient[i] = S16(a_amb + 2 * i); }
    printf("luz global: dir (%d,%d,%d) color x16 (%d,%d,%d) ambiente (%d,%d,%d)\n", env.main_dir[0], env.main_dir[1],
           env.main_dir[2], env.main_col[0], env.main_col[1], env.main_col[2], env.ambient[0], env.ambient[1], env.ambient[2]);

    /* luces fijas: 8 entradas {n, ptr} */
    static NsPointLight pl[8][512]; NsLightList fixed[8]; int nfix = 0;
    for (int i = 0; i < 8; i++) {
        int32_t n = S32(a_fix + i * 8); uint32_t p = U32(a_fix + i * 8 + 4);
        if (n <= 0 || n > 512 || !okp(p)) break;
        for (int k = 0; k < n; k++) {
            uint32_t b = p + k * 16;
            NsPointLight *L = &pl[i][k];
            for (int a = 0; a < 3; a++) { L->pos[a] = S16(b + 2 * a); L->col[a] = U8(b + 12 + a); }
            L->r_range = U16(b + 8); L->e_range = U16(b + 10);
        }
        fixed[nfix].l = pl[i]; fixed[nfix].n = n; nfix++;
    }
    printf("listas de luces fijas: %d\n", nfix);

    Stat dyn = {0}, pre = {0}, prebest = {0};
    int ndyn = 0, nmap = 0, nobj = 0;
    for (uint32_t a = 0x80010000u; a < 0x801FF000u; a += 4) {
        uint32_t d = U32(a + 0x24); int n = U16(a + 0x2E);
        if (!okp(d) || n < 1 || n > 24 || U32(a + 0x48 + 0x40) != d + 0x20 || U32(a + 0x28) > 0x7FF) continue;
        uint32_t flag = U32(a + 0x28);
        nobj++;
        if (flag & 8) {                                   /* dinámico, sombreado en tiempo real */
            uint32_t lp = U32(a + 0x34);
            if (!okp(lp)) continue;
            NsLight L; int16_t Lw[3][3]; rd_mat(lp, Lw); rd_mat(lp + 32, L.cmat);
            for (int i = 0; i < 3; i++) L.bg[i] = ((flag & 0x100) ? S32(lp + 20 + 4 * i) : env.ambient[i]) * 16;
            int used = 0;
            for (int i = 0; i < n; i++) {
                uint32_t o = a + 0x48 + i * 0x5c, chain[8]; int nc = 0;
                for (uint32_t p = o; okp(p) && nc < 8; p = U32(p + 0x48)) chain[nc++] = p;
                int16_t R[3][3]; rd_mat(o, R); ns_light_to_local(Lw, R, L.lmat);
                for (int pi = 0; pi < 2; pi++) {          /* dos búferes de dibujo: el bueno es el último escrito */
                    uint32_t q = U32(o + 0x54 + 4 * pi); if (!okp(q)) continue;
                    Stat s = {0};
                    for (int c = 0; c < nc; c++) {
                        uint32_t mdl = U32(chain[c] + 0x40); int np = U16(chain[c] + 0x52);
                        if (!okp(mdl)) break;
                        int nn = S32(mdl + 0x40); uint32_t norms = U32(mdl + 0x44), nidx = U32(mdl + 0x48);
                        if ((U32(mdl) & 0x10000) || !okp(norms) || !okp(nidx)) { q += 52 * np; continue; }
                        for (int k = 0; k < np; k++, q += 52) {
                            if (!(U32(q) & 0xFFFF)) continue;          /* polígono descartado: sin sombrear */
                            static const int src[4] = {0, 1, 3, 2};
                            for (int sl = 0; sl < 4; sl++) {
                                int ni = U8(nidx + k * 4 + src[sl]); if (ni >= nn) continue;
                                int16_t nv[3] = {S16(norms + ni * 8), S16(norms + ni * 8 + 2), S16(norms + ni * 8 + 4)};
                                uint8_t e[3], g[3]; ns_light_color(&L, nv, e);
                                for (int a2 = 0; a2 < 3; a2++) g[a2] = U8(q + 4 + sl * 12 + a2);
                                cmp(&s, g, e);
                            }
                        }
                    }
                    if (s.ok + s.bad) { dyn.ok += s.ok; dyn.bad += s.bad; dyn.sad += s.sad; used = 1; break; }
                }
            }
            ndyn += used;
        } else if (flag & 2) {                            /* mapa/objeto con color precalculado */
            int16_t Rw[3][3]; int32_t Tw[3]; rd_mat(a, Rw); for (int i = 0; i < 3; i++) Tw[i] = S32(a + 20 + 4 * i);
            Stat best = {0}; int have = 0;
            /* se prueba sin luces y con cada lista; se queda la que mejor reproduce el color guardado */
            for (int cand = -1; cand < nfix; cand++) {
                Stat s = {0};
                for (int i = 0; i < n; i++) {
                    uint32_t o = a + 0x48 + i * 0x5c, mdl = U32(o + 0x40), rg = U32(o + 0x44);
                    if (!okp(mdl) || !okp(rg) || (U32(mdl) & 4)) continue;
                    int nf = S32(mdl + 4), nv = S32(mdl + 0x34), nn = S32(mdl + 0x40);
                    uint32_t verts = U32(mdl + 0x38), vidx = U32(mdl + 0x3C), norms = U32(mdl + 0x44), nidx = U32(mdl + 0x48);
                    if (!okp(verts) || !okp(vidx) || !okp(norms) || !okp(nidx) || nf < 0 || nf > 4096) continue;
                    for (int fv = 0; fv < nf * 4; fv++) {
                        int vi = U8(vidx + fv), ni = U8(nidx + fv);
                        if (vi >= nv || vi >= 126 || ni >= nn) continue;
                        int16_t v[3] = {S16(verts + vi * 8), S16(verts + vi * 8 + 2), S16(verts + vi * 8 + 4)};
                        int16_t nm[3] = {S16(norms + ni * 8), S16(norms + ni * 8 + 2), S16(norms + ni * 8 + 4)};
                        int32_t wp[3]; ns_light_world_pos(Rw, Tw, v, wp);
                        uint8_t e[3], g[3];
                        ns_light_preshade_vertex(&env, cand < 0 ? 0 : &fixed[cand], cand < 0 ? 0 : 1, wp, Rw, nm, e);
                        for (int a2 = 0; a2 < 3; a2++) g[a2] = U8(rg + fv * 4 + a2);
                        cmp(&s, g, e);
                    }
                }
                if (!have || s.ok > best.ok) { best = s; have = 1; }
            }
            pre.ok += best.ok; pre.bad += best.bad; pre.sad += best.sad; nmap++;
            if (best.ok + best.bad > prebest.ok + prebest.bad) prebest = best;
        }
    }
    printf("objetos 3D encontrados: %d (dinamicos con paquetes sombreados: %d, precalculados: %d)\n", nobj, ndyn, nmap);
    long dt = dyn.ok + dyn.bad, pt = pre.ok + pre.bad;
    printf("dinamicos (luz en tiempo real): %ld vertices, exactos %ld (%.2f%%), error medio %.4f bytes/canal\n", dt, dyn.ok,
           dt ? 100.0 * dyn.ok / dt : 0.0, dt ? dyn.sad / (3.0 * dt) : 0.0);
    printf("mapa (precalculado, mejor lista por objeto): %ld vertices, exactos %ld (%.2f%%), error medio %.4f bytes/canal\n", pt,
           pre.ok, pt ? 100.0 * pre.ok / pt : 0.0, pt ? pre.sad / (3.0 * pt) : 0.0);
    printf("objeto precalculado mas grande: %ld vertices, exactos %ld (%.2f%%), error medio %.4f\n", prebest.ok + prebest.bad, prebest.ok,
           (prebest.ok + prebest.bad) ? 100.0 * prebest.ok / (prebest.ok + prebest.bad) : 0.0,
           (prebest.ok + prebest.bad) ? prebest.sad / (3.0 * (prebest.ok + prebest.bad)) : 0.0);
    free(ram);
    return (dt + pt) ? 0 : 1;   /* 1 si no se encontró nada que comparar */
}
