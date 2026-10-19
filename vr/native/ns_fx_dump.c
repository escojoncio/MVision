/* ns_fx_dump: herramienta de escritorio. Lee una RAM de 2 MB (volcado local, NO va al repo) y muestra la escena de efectos extraída.
 *   ns_fx_dump ram.bin [--items] [--buf 0|1] [--ysc F]
 * Salida: estado de la cámara, efectos de pantalla, estadísticas (incluida la comprobación de reproyección contra las coordenadas de pantalla
 * de los paquetes del motor) y, con --items, un elemento por línea. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ns_fx.h"

int main(int argc, char **argv) {
    const char *path = NULL; int items = 0; NsFxOpts o = ns_fx_opts_default();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--items")) items = 1;
        else if (!strcmp(argv[i], "--buf") && i + 1 < argc) o.buffer = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ysc") && i + 1 < argc) o.y_scale = (float)atof(argv[++i]);
        else path = argv[i];
    }
    if (!path) { fprintf(stderr, "uso: %s ram.bin [--items] [--buf 0|1] [--ysc F]\n", argv[0]); return 2; }
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); return 2; }
    uint8_t *ram = (uint8_t *)malloc(NSFX_RAM_SIZE);
    if (!ram || fread(ram, 1, NSFX_RAM_SIZE, f) != NSFX_RAM_SIZE) { fprintf(stderr, "se esperaban %u bytes\n", NSFX_RAM_SIZE); return 2; }
    fclose(f);
    NsFxScene *s = ns_fx_scene_create(0);
    int r = ns_fx_extract(ram, &o, s);
    printf("extract=%d cam.valid=%d H=%.0f det_neg=%d buffer=%d map=0x%X\n", r, s->cam.valid, s->cam.proj_h, s->cam.det_neg, s->buffer, s->cur_map);
    const NsFxScreen *sc = &s->screen;
    printf("pantalla: status=0x%X nvg=%d term=%d agua(present=%d active=%d tinte=%.2f,%.2f,%.2f) gas(present=%d active=%d inten=%.2f) fundido(active=%d blanco=%d cantidad=%.2f)\n",
           sc->game_status, sc->nvg, sc->thermal, sc->water.present, sc->water.active, sc->water.tint[0], sc->water.tint[1], sc->water.tint[2], sc->gas.present, sc->gas.active, sc->gas.intensity,
           sc->fade.active, sc->fade.white, sc->fade.amount);
    if (sc->water.present) printf("  agua caja (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f) fase=%u\n", sc->water.box_min[0], sc->water.box_min[1], sc->water.box_min[2], sc->water.box_max[0], sc->water.box_max[1], sc->water.box_max[2], sc->water.phase);
    printf("prims=%u sortonly=%u invisibles=%u otro_mapa=%u malas=%u libres=%u desbordadas=%u elementos=%u (cull motor=%u) sombras_obj=%u\n", s->stats.n_prims, s->stats.n_sortonly, s->stats.n_invisible,
           s->stats.n_wrong_map, s->stats.n_bad, s->stats.n_free, s->stats.n_overflow, s->n_items, s->stats.n_culled, s->n_shadow_objs);
    printf("formas: billboard=%u quad=%u tri=%u cinta=%u\n", s->stats.by_shape[1], s->stats.by_shape[2], s->stats.by_shape[3], s->stats.by_shape[4]);
    printf("papeles:"); for (int k = 1; k < NSFX_ROLE_COUNT; k++) if (s->stats.by_role[k]) printf(" %s=%u", ns_fx_role_name(k), s->stats.by_role[k]); printf("\n");
    printf("reproyeccion: n=%u media=%.2f px max=%.1f px mal(>3px)=%u\n", s->stats.chk_n, s->stats.chk_n ? s->stats.chk_sum_px / s->stats.chk_n : 0.0, s->stats.chk_max_px, s->stats.chk_bad);
    {   /* cobertura: todo paquete que el motor emitió (z != 0) de una primitiva visible del mapa actual debe tener al menos un elemento en la lista */
        uint32_t emitted = 0, covered = 0, missing_packs = 0;
        #define RD16(a) ((uint32_t)(ram[(a) & 0x1FFFFF] | (ram[((a) & 0x1FFFFF) + 1] << 8)))
        #define RD32(a) (RD16(a) | (RD16((a) + 2) << 16))
        uint32_t ch = NSFX_A_CHANL; int qs = (int16_t)RD16(ch + 0x52), pi = (int16_t)RD16(ch + 0x54); uint32_t q = RD32(ch + 0x58);
        if (r == 0 && q >= 0x80000000u && q < 0x80200000u && pi >= 0 && qs >= pi && qs <= 1024) for (int i = pi; i < qs; i++) {
            uint32_t p = RD32(q + 4u * (uint32_t)i); if (p < 0x80000000u || p >= 0x80200000u) continue;
            uint32_t type = RD32(p + 0x24), grp = RD16(p + 0x28); int n = (int16_t)RD16(p + 0x2A), psz = (int16_t)RD16(p + 0x30);
            uint32_t pk = RD32(p + 0x40 + 4u * (uint32_t)s->buffer);
            if ((type & 0x900) || (grp && !(grp & s->cur_map)) || (type & 0x1F) == 23 || n <= 0 || psz <= 0 || pk < 0x80000000u || pk >= 0x80200000u) continue;
            for (int j = 0; j < n; j++) {
                if (!RD16(pk + (uint32_t)(j * psz))) continue;
                emitted++;
                int found = 0; for (uint32_t k = 0; k < s->n_items && !found; k++) found = s->items[k].prim == p && s->items[k].idx == j;
                if (found) covered++; else missing_packs++;
            }
        }
        printf("cobertura: paquetes emitidos por el motor=%u en la lista=%u sin elemento=%u\n", emitted, covered, missing_packs);
        if (missing_packs && getenv("NSFX_STRICT")) return 3;
    }
    if (items) for (uint32_t i = 0; i < s->n_items; i++) {
        const NsFxItem *it = &s->items[i];
        printf("%4u %-9s %-13s k=%-2u fl=0x%03X abr=%u sz=%-6d raise=%-5d w=(%.0f,%.0f,%.0f) off=(%.1f,%.1f,%.1f,%.1f) prim=%08X/%u\n", i, ns_fx_shape_name(it->shape), ns_fx_role_name(it->role), it->kind, it->flags, it->abr, it->sz,
               it->raise, it->pos[0][0], it->pos[0][1], it->pos[0][2], it->off[0], it->off[1], it->off[2], it->off[3], it->prim, it->idx);
    }
    ns_fx_scene_destroy(s); free(ram);
    return r < 0 ? 1 : 0;
}
