#pragma once
/* Sombra de vértices de la GTE por DIRECCIÓN de memoria (como PGXP). El intérprete (interp.c) enlaza cada palabra SXY que el juego
 * guarda con swc2/mfc2+sw con el vértice de vista de alta precisión que la GTE produjo (gte.c), y lo propaga con lw/sw/move/lwc2/mtc2.
 * La GPU (gpu.c) lee la etiqueta de la palabra de cada vértice de la primitiva (gte_hp_resolve_a) sin ambigüedad por valor.
 * Una etiqueta (tag = seq+1 del vértice, 0 = ninguna) solo vale si el valor de 32 bits que hay ahora coincide con la clave del vértice
 * (gte_hp_tag_ok): por eso ni las ALU ni las escrituras que no pasan por el intérprete (DMA, HLE) invalidan explícitamente. Las
 * escrituras parciales/sin etiqueta de la CPU sí la borran (hps_set(a, 0)). Con la alta precisión apagada (hps_on = 0) nada se toca. */
#include <stdint.h>
#include "cpu.h"

#define HPS_RAM_WORDS 0x80000u                 /* 2 MB / 4 */
#define HPS_WORDS (HPS_RAM_WORDS + 0x100u)     /* + scratchpad (1 KB / 4) */
extern int hps_on;
extern uint32_t hps_mem[HPS_WORDS];            /* etiqueta por palabra */
extern uint32_t hps_bm[HPS_WORDS / 32 + 1];    /* bit por palabra con etiqueta: la comprobación barata de cada escritura */

int gte_hp_tag_ok(uint32_t tag, uint32_t v);               /* ¿la etiqueta existe aún y su clave es v? */
uint32_t gte_hp_sxy_tag(int r, uint32_t v);                /* etiqueta del registro de datos r (12..15) de la GTE si vale para v */
void gte_hp_sxy_set(int r, uint32_t tag, uint32_t v);      /* tras mtc2/lwc2 de r (12..15): el valor v lleva esa etiqueta (0 = ninguna) */

static inline int hps_widx(uint32_t a) {
    uint32_t p = a & 0x1FFFFFFFu;
    if (p < 0x800000u) return (int)((p & 0x1FFFFCu) >> 2);
    if (p - 0x1F800000u < 0x400u) return (int)(HPS_RAM_WORDS + ((p - 0x1F800000u) >> 2));
    return -1;
}
/* Etiqueta de la palabra que contiene a (0 si no hay). */
static inline uint32_t hps_get(uint32_t a) {
    int w = hps_widx(a);
    return w >= 0 && (hps_bm[w >> 5] >> (w & 31) & 1u) ? hps_mem[w] : 0;
}
/* Escritura de la CPU en la palabra de a: tag != 0 la etiqueta, 0 la borra (sin tocar la tabla si no había nada). */
static inline void hps_set(uint32_t a, uint32_t tag) {
    int w = hps_widx(a);
    if (w < 0) return;
    if (tag) { hps_mem[w] = tag; hps_bm[w >> 5] |= 1u << (w & 31); }
    else if (hps_bm[w >> 5] >> (w & 31) & 1u) { hps_bm[w >> 5] &= ~(1u << (w & 31)); hps_mem[w] = 0; }
}
