/* nsdiv: valida la réplica del motor (ns_div) contra la lista de la GPU de un volcado. Uso:
 *   nsdiv RAM_DUMP LIST_DUMP [pool] [flags]    pool: -1 reloj del motor (por defecto), -2 el contrario, 0/1 fijo; flags: suma de NSDIV_*
 * Emula Bound/Trans/Divide sobre la RAM de RAM_DUMP y compara las primitivas con los polígonos GT4 de la lista de LIST_DUMP (coordenadas crudas de la GTE:
 * x,y menos el offset de dibujo): exacto (las 4 esquinas, en orden), cercano (cada coordenada a ±1 px) y lo que sobra a cada lado. */
#include "ns_div.h"
#include "ns_dump.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int16_t c[8]; uint16_t obj_i, node, face; uint8_t kind, used; uint32_t pk[2]; } Key;

typedef struct { Key *k; size_t n, cap; } Vec;
static const uint8_t *g_ram;
typedef struct { uint32_t pk[2]; uint8_t reason; } Drop;
static Drop *g_drop; static size_t g_ndrop, g_capdrop;
static void on_drop(void *u, uint32_t obj, int node, int face, int reason, const uint32_t pack[2]) {
    (void)u;
    if (getenv("NSDIV_AUD") && reason == 1) { static int n; uint32_t tag = 0; if (pack[0]) memcpy(&tag, g_ram + (pack[0] & 0x1FFFFFFFu), 4); if (tag && n++ < 12) printf("drop outcode obj %08x node %d face %d pk %08x %08x tag1 %08x\n", obj, node, face, pack[0], pack[1], tag); }
    if (g_ndrop == g_capdrop) { g_capdrop = g_capdrop ? g_capdrop * 2 : 4096; g_drop = (Drop *)realloc(g_drop, g_capdrop * sizeof(Drop)); }
    g_drop[g_ndrop].pk[0] = pack[0]; g_drop[g_ndrop].pk[1] = pack[1]; g_drop[g_ndrop++].reason = (uint8_t)reason;
}
static Vec g_sub;   /* trozos en orden de emisión */
static void emit(void *u, const NsDivPrim *p) {
    Vec *v = (Vec *)u;
    if (v != &g_sub && p->kind == NSDIV_SUB) { Vec *sv = &g_sub; if (sv->n == sv->cap) { sv->cap = sv->cap ? sv->cap * 2 : 4096; sv->k = (Key *)realloc(sv->k, sv->cap * sizeof(Key)); } Key *kk = &sv->k[sv->n++]; memset(kk, 0, sizeof *kk); for (int i = 0; i < 4; i++) { kk->c[2 * i] = p->sx[i]; kk->c[2 * i + 1] = p->sy[i]; } }
    if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 4096; v->k = (Key *)realloc(v->k, v->cap * sizeof(Key)); }
    Key *k = &v->k[v->n++];
    memset(k, 0, sizeof *k);
    for (int i = 0; i < 4; i++) { k->c[2 * i] = p->sx[i]; k->c[2 * i + 1] = p->sy[i]; }
    k->pk[0] = p->pack[0]; k->pk[1] = p->pack[1]; k->node = p->node; k->face = p->face; k->kind = p->kind;
}
static int cmp(const void *a, const void *b) { return memcmp(((const Key *)a)->c, ((const Key *)b)->c, sizeof(int16_t) * 8); }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: nsdiv RAM_DUMP LIST_DUMP [pool] [flags]\n"); return 2; }
    NsDump *rd = ns_dump_open(argv[1]), *ld = ns_dump_open(argv[2]);
    if (!rd || !ld || !ld->has_list) { fprintf(stderr, "nsdiv: volcado ilegible o sin lista\n"); return 1; }
    NsDivCfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.pool = argc > 3 ? atoi(argv[3]) : -1;
    cfg.n_packs_override = -1;
    cfg.flags = argc > 4 ? atoi(argv[4]) : 0;
    cfg.drop = on_drop;
    if (getenv("NSDIV_SCRATCH")) cfg.scratch = rd->scratch;
    Vec e = {0};
    NsDivStats S;
    g_ram = rd->ram;
    int n = ns_div_run(rd->ram, NS_DUMP_RAM_SIZE, &cfg, NULL, emit, &e, &S);
    /* polígonos GT4 de la lista */
    Vec l = {0};
    for (uint32_t i = 0; i < ld->list.nops; i++) {
        const GpuOp *op = &ld->list.ops[i];
        if (op->type != GPU_OP_POLY || op->nv != 4) continue;
        NsDivPrim p;
        memset(&p, 0, sizeof p);
        for (int k = 0; k < 4; k++) { p.sx[k] = (int16_t)(op->v[k].x - op->dox); p.sy[k] = (int16_t)(op->v[k].y - op->doy); }
        emit(&l, &p);
    }
    {   /* Auditoria cara a cara contra el estado REAL del paquete en la RAM (etiqueta 0 = el motor no la dibujo; 0x0c.. = encadenada en la OT): replica x RAM */
        size_t m[4][3] = {{0}};   /* fila: 0 entera, 1 partida, 2 descartada, 3 descartada por outcode/atras; columna: etiqueta 0 / encadenada / otra */
        size_t byr[8][3] = {{0}};
        uint32_t seen = 0;
        for (size_t a = 0; a < e.n; a++) {
            if (e.k[a].kind == NSDIV_SUB || !e.k[a].pk[S.pool]) continue;
            uint32_t pa = e.k[a].pk[S.pool] & 0x1FFFFFFFu, tag;
            if (pa + 4 > NS_DUMP_RAM_SIZE) continue;
            memcpy(&tag, rd->ram + pa, 4); seen++;
            m[0][tag == 0 ? 0 : (tag >> 24) == 0x0c ? 1 : 2]++;
        }
        for (size_t a = 0; a < g_ndrop; a++) {
            if (!g_drop[a].pk[S.pool]) continue;
            uint32_t pa = g_drop[a].pk[S.pool] & 0x1FFFFFFFu, tag;
            if (pa + 4 > NS_DUMP_RAM_SIZE) continue;
            memcpy(&tag, rd->ram + pa, 4);
            byr[g_drop[a].reason][tag == 0 ? 0 : (tag >> 24) == 0x0c ? 1 : 2]++;
        }
        printf("  auditoria de caras (replica x etiqueta del paquete en la RAM: 0 / encadenada / otra):\n    entregada entera : %zu / %zu / %zu\n", m[0][0], m[0][1], m[0][2]);
        static const char *nm[] = {"", "outcode", "cara trasera", "area 0", "z16 0", "indirecta z0"};
        for (int q = 1; q <= 5; q++) printf("    descartada %-12s: %zu / %zu / %zu\n", nm[q], byr[q][0], byr[q][1], byr[q][2]);
        (void)seen;
    }
    /* Trans: caras enteras contra el paquete REAL de la RAM (x0,x1,x2,x3 en +8,+20,+32,+44; solo valen si el paquete no se subdividió ni se descartó) */
    {
        size_t cmpd[2] = {0, 0}, same[2] = {0, 0};
        for (size_t a = 0; a < e.n; a++) {
            if (e.k[a].kind != NSDIV_FULL) continue;
            for (int b = 0; b < 2; b++) {
                uint32_t pa = e.k[a].pk[b];
                if (!pa || (pa & 0x1FFFFFFFu) + 0x34u > NS_DUMP_RAM_SIZE) continue;
                const uint8_t *m = rd->ram + (pa & 0x1FFFFFFFu);
                int16_t v[8];
                static const int off[4] = {8, 20, 32, 44};
                for (int q = 0; q < 4; q++) memcpy(&v[2 * q], m + off[q], 4);
                cmpd[b]++;
                if (!memcmp(v, e.k[a].c, sizeof v)) same[b]++;
            }
        }
        printf("  Trans vs paquetes de la RAM: buf0 %zu/%zu  buf1 %zu/%zu\n", same[0], cmpd[0], same[1], cmpd[1]);
    }
    /* Divide: los trozos contra los paquetes REALES que dejó el motor en la memoria libre del búfer (se reparten en orden desde el inicio de la primera unidad libre) */
    {
        const uint8_t *m = rd->ram;
        uint32_t base = 0x800AE480u + 0x1010u * (uint32_t)S.pool, nu;
        memcpy(&nu, m + (base & 0x1FFFFFFFu) + 12, 4);
        size_t k = 0, same = 0, tot = 0, firstbad = (size_t)-1;
        for (uint32_t u = 0; u < nu && u < 512; u++) {
            uint32_t ua = (base & 0x1FFFFFFFu) + 16 + 8 * u, a0, ap, a1;
            memcpy(&a0, m + ua, 4); memcpy(&ap, m + ua + 4, 4); memcpy(&a1, m + ua + 8, 4);
            if (ap != 0) continue;
            for (uint32_t pa = a0; pa + 0x34u <= a1 && k < g_sub.n; pa += 0x34u, k++) {
                int16_t v[8];
                static const int off[4] = {8, 20, 32, 44};
                if ((pa & 0x1FFFFFFFu) + 0x34u > NS_DUMP_RAM_SIZE) { k = g_sub.n; break; }
                for (int q = 0; q < 4; q++) memcpy(&v[2 * q], m + (pa & 0x1FFFFFFFu) + off[q], 4);
                tot++;
                if (!memcmp(v, g_sub.k[k].c, sizeof v)) same++; else if (firstbad == (size_t)-1) firstbad = k;
            }
        }
        printf("  Divide vs paquetes de la RAM (en orden): %zu/%zu de %zu trozos%s\n", same, tot, g_sub.n, firstbad == (size_t)-1 ? "" : " (primer fallo en el trozo indicado abajo)");
        if (firstbad != (size_t)-1) { printf("  primer fallo %zu:", firstbad); for (int q = 0; q < 8; q++) printf(" %d", g_sub.k[firstbad].c[q]); printf("\n"); }
    }
    qsort(e.k, e.n, sizeof(Key), cmp);
    qsort(l.k, l.n, sizeof(Key), cmp);
    {   /* paquetes que la lista dice haber dibujado (pkt_addr) tal como estan en la RAM: contra la lista y contra la replica */
        size_t same_l = 0, same_e = 0, tot = 0, near_l = 0;
        for (uint32_t i = 0; i < ld->list.nops; i++) {
            const GpuOp *op = &ld->list.ops[i];
            if (op->type != GPU_OP_POLY || op->nv != 4 || !op->pkt_addr) continue;
            if ((op->pkt_addr & 0x1FFFFFFFu) + 0x34u > NS_DUMP_RAM_SIZE) continue;
            const uint8_t *m = rd->ram + (op->pkt_addr & 0x1FFFFFFFu);
            Key kk;
            memset(&kk, 0, sizeof kk);
            static const int off[4] = {8, 20, 32, 44};
            int16_t w[8];
            for (int q = 0; q < 4; q++) { memcpy(&kk.c[2 * q], m + off[q], 4); w[2 * q] = (int16_t)(op->v[q].x - op->dox); w[2 * q + 1] = (int16_t)(op->v[q].y - op->doy); }
            tot++;
            if (!memcmp(kk.c, w, sizeof w)) same_l++;
            else { int ok = 1; for (int q = 0; q < 8; q++) { int d = kk.c[q] - w[q]; ok &= d >= -1 && d <= 1; } near_l += ok; }
            if (bsearch(&kk, e.k, e.n, sizeof(Key), cmp)) same_e++;
        }
        printf("  paquetes de la lista (pkt_addr) tal como estan en la RAM: %zu; = lista exacto %zu, lista a ±1 %zu; = replica exacto %zu\n", tot, same_l, near_l, same_e);
    }
    if (getenv("NSDIV_PKT")) {
        size_t hi = 0, lo = 0, none = 0; uint32_t mn = ~0u, mx = 0;
        for (uint32_t i = 0; i < ld->list.nops; i++) {
            const GpuOp *op = &ld->list.ops[i];
            if (op->type != GPU_OP_POLY || op->nv != 4) continue;
            if (!op->pkt_addr) { none++; continue; }
            uint32_t a = op->pkt_addr & 0x1FFFFFFFu;
            if (a < mn) mn = a; if (a > mx) mx = a;
            if (a >= 0x19c810u && a < 0x1b1000u) hi++; else lo++;
        }
        printf("  pkt_addr GT4: sin=%zu en [0x19c810,0x1b1000)=%zu otros=%zu rango %x..%x\n", none, hi, lo, mn, mx);
    }
    if (getenv("NSDIV_OWN")) {
        size_t own[2] = {0, 0}, other = 0, tot = 0;
        size_t hist[64] = {0};
        for (uint32_t i = 0; i < ld->list.nops; i++) {
            const GpuOp *op = &ld->list.ops[i];
            if (op->type != GPU_OP_POLY || op->nv != 4 || !op->pkt_addr) continue;
            tot++;
            int is = -1;
            for (size_t a = 0; a < e.n && is < 0; a++) { if (e.k[a].kind != NSDIV_FULL) continue; for (int b = 0; b < 2; b++) if (e.k[a].pk[b] == op->pkt_addr) { is = b; break; } }
            if (is >= 0) own[is]++; else { other++; uint32_t pg = ((op->pkt_addr & 0x1FFFFFFFu) - 0x150000u) >> 12; if (pg < 64) hist[pg]++; }
        }
        printf("  paquetes de la lista: %zu; de caras enteras buf0=%zu buf1=%zu; otros=%zu\n  otros por pagina de 4K desde 0x150000:", tot, own[0], own[1], other);
        for (int q = 0; q < 64; q++) if (hist[q]) printf(" %x:%zu", 0x150 + q, hist[q]);
        printf("\n");
    }
    if (getenv("NSDIV_SPLIT")) {
        uint32_t last = 0; size_t nf = 0, z0 = 0;
        for (size_t a = 0; a < e.n; a++) {
            if (e.k[a].kind != NSDIV_SUB || !e.k[a].pk[0] || e.k[a].pk[0] == last) continue;
            last = e.k[a].pk[0]; nf++;
            uint32_t tag; memcpy(&tag, rd->ram + (last & 0x1FFFFFFFu), 4);
            if ((tag & 0xFFFFu) == 0) z0++;
            if (nf < 10) printf("  cara dividida n%u f%u pack %08x etiqueta RAM %08x\n", e.k[a].node, e.k[a].face, last, tag);
        }
        printf("  caras que la replica divide: %zu; su paquete en la RAM con z16==0 (el motor lo anula al dividir): %zu\n", nf, z0);
    }
    {   /* paquetes de la lista cuya direccion cae en una unidad LIBRE de la memoria de paquetes (RAM del volcado): son los trozos de Divide */
        size_t infree = 0, tot = 0;
        for (uint32_t i = 0; i < ld->list.nops; i++) {
            const GpuOp *op = &ld->list.ops[i];
            if (op->type != GPU_OP_POLY || op->nv != 4 || !op->pkt_addr) continue;
            tot++;
            for (int pl = 0; pl < 2; pl++) {
                uint32_t base = (0x800AE480u + 0x1010u * (uint32_t)pl) & 0x1FFFFFFFu, nu;
                memcpy(&nu, rd->ram + base + 12, 4);
                for (uint32_t u = 0; u < nu && u < 512; u++) {
                    uint32_t a0, ap, a1; const uint8_t *m = rd->ram + base + 16 + 8 * u;
                    memcpy(&a0, m, 4); memcpy(&ap, m + 4, 4); memcpy(&a1, m + 8, 4);
                    if (ap == 0 && op->pkt_addr >= a0 && op->pkt_addr < a1) { infree++; pl = 2; break; }
                }
            }
        }
        printf("  paquetes de la lista en unidades libres (trozos de Divide): %zu de %zu\n", infree, tot);
    }
    size_t exact = 0, near = 0;
    size_t i = 0, j = 0;
    while (i < e.n && j < l.n) {
        int c = cmp(&e.k[i], &l.k[j]);
        if (c == 0) { e.k[i].used = l.k[j].used = 1; exact++; i++; j++; } else if (c < 0) i++; else j++;
    }
    for (size_t a = 0; a < e.n; a++) {
        if (e.k[a].used) continue;
        for (size_t b = 0; b < l.n; b++) {
            if (l.k[b].used) continue;
            int ok = 1;
            for (int q = 0; q < 8 && ok; q++) { int d = e.k[a].c[q] - l.k[b].c[q]; ok = d >= -1 && d <= 1; }
            if (ok) { e.k[a].used = l.k[b].used = 2; near++; if (getenv("NSDIV_NEAR") && near < 12) { printf("N:"); for (int q = 0; q < 8; q++) printf(" %d/%d", e.k[a].c[q], l.k[b].c[q]); printf("\n"); } break; }
        }
    }
    if (getenv("NSDIV_DUMP")) {
        for (size_t a = 0; a < e.n; a++) if (!e.k[a].used) { printf("R n%u f%u k%u:", e.k[a].node, e.k[a].face, e.k[a].kind); for (int q = 0; q < 8; q++) printf(" %d", e.k[a].c[q]); printf("\n"); }
        for (size_t b = 0; b < l.n; b++) if (!l.k[b].used) { printf("L:"); for (int q = 0; q < 8; q++) printf(" %d", l.k[b].c[q]); printf("\n"); }
    }
    size_t e_only = 0, l_only = 0, e_full_only = 0, e_sub_only = 0;
    for (size_t a = 0; a < e.n; a++) if (!e.k[a].used) { e_only++; if (e.k[a].kind == NSDIV_SUB) e_sub_only++; else e_full_only++; }
    for (size_t b = 0; b < l.n; b++) if (!l.k[b].used) l_only++;
    printf("%s | lista %s: pool=%d H=%d paquetes=%d->%d | prims motor=%d (enteras=%u subdivididas=%u trozos=%u) lista GT4=%zu | exacto=%zu cercano(±1)=%zu solo_replica=%zu (enteras=%zu trozos=%zu) solo_lista=%zu\n",
           argv[1], argv[2], S.pool, S.h, S.n_packs_start, S.n_packs_end, n, S.f_full, S.f_split, S.sub_emitted, l.n, exact, near, e_only, e_full_only, e_sub_only, l_only);
    printf("  objs=%u dibujados=%u gbound=%u | modelos bound0=%u bound1=%u bound2=%u (bound distinto de la RAM=%u) | descartes: outcode=%u trasera=%u area0=%u z0=%u indirecta_z0=%u | trozos: outcode=%u sinpaquetes=%u niv1=%u niv2=%u niv3=%u abortado=%u | bound: yo0_RAMon=%u yoon_RAM0=%u 1vs2=%u\n",
           S.objs, S.objs_drawn, S.objs_gbound, S.models_bound0, S.models_bound1, S.models_bound2, S.bound_ram_mismatch, S.f_outcode, S.f_back, S.f_degenerate, S.f_z0, S.f_indirect_z0,
           S.sub_outcode, S.sub_nopack, S.split_depth[1], S.split_depth[2], S.split_depth[3], S.divide_aborted, S.bound_mine0_ram_on, S.bound_mine_on_ram0, S.bound_1_vs_2);
    free(e.k); free(l.k);
    ns_dump_close(rd); ns_dump_close(ld);
    return 0;
}
