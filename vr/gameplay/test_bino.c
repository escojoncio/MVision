/* Pruebas de vr_bino.h (prismáticos: salto de la espera de scope.c y medida del retardo). RAM sintética con el Work de scope.c tal como lo deja NewScope. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_bino.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define WORK  0x80100000u
#define PREV  0x80100200u   /* vecino anterior de la lista */
#define NEXT  0x80100400u   /* vecino siguiente / centinela */

/* Work recién creado: act/die/nombre puestos por GV_SetNamedActor, hud_delay = 16, flags = 0, enlazado entre PREV y NEXT. */
static uint8_t *mkram(uint32_t w) {
    uint8_t *r = calloc(1, 0x200000);
    vrbino_wr32(r, w, PREV); vrbino_wr32(r, w + 4, NEXT);
    vrbino_wr32(r, w + 8, VRBINO_ACT_FN); vrbino_wr32(r, w + 0xC, VRBINO_DIE_FN); vrbino_wr32(r, w + 0x14, VRBINO_NAME);
    vrbino_wr16(r, w + VRBINO_W_HUD, VRBINO_HUD_DELAY); vrbino_wr32(r, w + VRBINO_W_FLAGS, 0);
    vrbino_wr32(r, PREV + 4, w); vrbino_wr32(r, NEXT, w);
    return r;
}

static void test_skip(void) {
    printf("-- salto de la espera --\n");
    uint8_t *r = mkram(WORK);
    vrbino_reset(); vrbino_set_item(1);
    int w = vrbino_frame(r, 320, 0, 100, 1);
    CHECK(w == 3, "primer fotograma escribe hud_delay y flags (%d)", w);
    CHECK(vrbino_rd16(r, WORK + VRBINO_W_HUD) == 0, "hud_delay = 0");
    CHECK((vrbino_rd32(r, WORK + VRBINO_W_FLAGS) & 3u) == 3u, "flags & 3 == 3 (%x)", vrbino_rd32(r, WORK + VRBINO_W_FLAGS));
    CHECK(vrbino_S.act == WORK && vrbino_S.eq_vbl == 100, "actor encontrado en el fotograma 100");
    w = vrbino_frame(r, 320, 0, 101, 1);
    CHECK(w == 0, "idempotente: nada que escribir después (%d)", w);
    free(r);

    r = mkram(WORK);   /* gafas montadas (0x8000) y una mitad de la rejilla ya lista: se conserva 0x8000 */
    vrbino_wr32(r, WORK + VRBINO_W_FLAGS, 0x8001u); vrbino_wr16(r, WORK + VRBINO_W_HUD, 5);
    vrbino_reset(); vrbino_set_item(1);
    vrbino_frame(r, 320, 0, 1, 1);
    CHECK(vrbino_rd32(r, WORK + VRBINO_W_FLAGS) == 0x8003u && vrbino_rd16(r, WORK + VRBINO_W_HUD) == 0, "conserva 0x8000, hud_delay 5 -> 0 (%x)", vrbino_rd32(r, WORK + VRBINO_W_FLAGS));
    free(r);

    r = mkram(WORK);   /* sin salto (skip = 0): solo mide */
    vrbino_reset(); vrbino_set_item(1);
    CHECK(vrbino_frame(r, 320, 0, 1, 0) == 0 && vrbino_rd16(r, WORK + VRBINO_W_HUD) == 16 && vrbino_rd32(r, WORK + VRBINO_W_FLAGS) == 0, "skip=0 no escribe");
    free(r);
}

static void test_safety(void) {
    printf("-- seguridad: solo actor vivo y valores esperados --\n");
    uint8_t *r = mkram(WORK);
    vrbino_wr32(r, PREV + 4, NEXT);                 /* liberado: el vecino ya no apunta a él */
    vrbino_reset(); vrbino_set_item(1);
    CHECK(vrbino_frame(r, 320, 0, 1, 1) == 0 && vrbino_S.act == 0 && vrbino_rd16(r, WORK + VRBINO_W_HUD) == 16, "bloque liberado (desenlazado): no se toca");
    free(r);

    r = mkram(WORK);
    vrbino_wr32(r, WORK + 0x14, 0x80012a18u);       /* otro nombre */
    vrbino_reset(); vrbino_set_item(1);
    CHECK(vrbino_frame(r, 320, 0, 1, 1) == 0 && vrbino_S.act == 0, "otro nombre de actor: no se toca");
    free(r);

    r = mkram(WORK);
    vrbino_wr16(r, WORK + VRBINO_W_HUD, 999); vrbino_wr32(r, WORK + VRBINO_W_FLAGS, 0x1234u);   /* valores que no son los de la espera */
    vrbino_reset(); vrbino_set_item(1);
    CHECK(vrbino_frame(r, 320, 0, 1, 1) == 0 && vrbino_rd16(r, WORK + VRBINO_W_HUD) == 999 && vrbino_rd32(r, WORK + VRBINO_W_FLAGS) == 0x1234u, "valores inesperados: no se escribe");
    free(r);

    r = mkram(WORK);
    vrbino_reset(); vrbino_set_item(1);
    vrbino_frame(r, 320, 0, 1, 1);
    vrbino_wr32(r, WORK + 8, 0x8001dead);           /* GV_DestroyActor cambia act: ya no es nuestro */
    vrbino_wr16(r, WORK + VRBINO_W_HUD, 7);
    vrbino_frame(r, 320, 0, 2, 1);
    CHECK(vrbino_S.act == 0 && vrbino_rd16(r, WORK + VRBINO_W_HUD) == 7, "actor destruido: se suelta y no se escribe");
    CHECK(vrbino_frame(NULL, 320, 0, 3, 1) == 0, "ram nula");
    free(r);

    r = mkram(WORK);   /* sin actor: la búsqueda no encuentra nada y respeta el intervalo */
    vrbino_wr32(r, WORK + 8, 0);
    vrbino_reset(); vrbino_set_item(1);
    for (uint32_t v = 0; v < 10; v++) vrbino_frame(r, 320, 0, v, 1);
    CHECK(vrbino_S.act == 0, "sin actor: nada");
    free(r);
}

static void test_log(void) {
    printf("-- medida activo / zoom --\n");
    uint8_t *r = mkram(WORK);
    vrbino_reset(); vrbino_set_item(1);
    vrbino_frame(r, 320, 0, 10, 1);
    vrbino_frame(r, 320, 0, 11, 1);
    vrbino_frame(r, 320, 1, 12, 1);
    CHECK(vrbino_S.has_on && vrbino_S.on_vbl == 12, "activo_vbl = 12");
    vrbino_frame(r, 320, 1, 13, 1);
    CHECK(!vrbino_S.has_zoom, "320 = sin zoom");
    vrbino_frame(r, 330, 1, 14, 1);
    CHECK(vrbino_S.has_zoom && vrbino_S.zoom_vbl == 14, "zoom_inicio_vbl = 14");
    vrbino_frame(r, 640, 1, 20, 1);
    CHECK(vrbino_S.zoom_vbl == 14, "solo el primer zoom");
    free(r);
}

static void test_latency(void) {
    printf("-- latencia de O y busqueda con el objeto equipado --\n");
    uint8_t *r = mkram(WORK);
    vrbino_reset(); vrbino_set_item(1);
    vrbino_frame(r, 320, 0, 1, 1);
    CHECK(vrbino_S.act == WORK, "objeto = prismaticos: actor encontrado en el primer fotograma");
    uint8_t *r2 = mkram(WORK);
    vrbino_reset();
    for (uint32_t v = 1; v <= 10; v++) vrbino_frame(r2, 320, 0, v, 1);
    CHECK(vrbino_S.act == 0 && vrbino_rd16(r2, WORK + VRBINO_W_HUD) == 16, "sin los prismaticos equipados no se busca el actor ni se toca");
    vrbino_set_item(1);
    vrbino_frame(r2, 320, 0, 11, 1);
    CHECK(vrbino_S.act == WORK, "al equiparlos se encuentra en el primer fotograma");
    free(r2);

    vrbino_reset(); vrbino_set_item(1); vrbino_frame(r, 320, 1, 10, 1);
    vrbino_pad(10, 320, 0, 2.0f);
    CHECK(!vrbino_B.wait, "sin pulsar: nada que medir");
    vrbino_pad(11, 320, VRBINO_PAD_CIRCLE, 2.0f);
    CHECK(vrbino_B.wait && vrbino_B.btn_vbl == 11 && vrbino_B.base_zoom == 320, "pulsa O en el fotograma 11");
    vrbino_pad(12, 330, VRBINO_PAD_CIRCLE, 2.0f);
    CHECK(vrbino_B.game_seen && vrbino_B.game_vbl == 12, "el zoom del juego cambia en el 12");
    vrbino_pad(13, 340, VRBINO_PAD_CIRCLE, 2.02f);
    CHECK(vrbino_B.wait, "2.02 < 2.0 * 1.017 no se nota");
    vrbino_pad(14, 350, VRBINO_PAD_CIRCLE, 2.06f);
    CHECK(!vrbino_B.wait, "2.06 > 2.034: zoom visible, medida cerrada");
    vrbino_pad(15, 360, VRBINO_PAD_CIRCLE, 2.1f);
    CHECK(!vrbino_B.wait, "mantener O no abre otra medida");
    vrbino_pad(16, 360, 0, 2.1f); vrbino_pad(17, 360, VRBINO_PAD_CIRCLE, 2.1f);
    CHECK(vrbino_B.wait && vrbino_B.btn_vbl == 17, "soltar y pulsar de nuevo: medida nueva");
    for (uint32_t v = 18; v < 18 + VRBINO_WAIT_MAX; v++) vrbino_pad(v, 360, VRBINO_PAD_CIRCLE, 2.1f);
    CHECK(!vrbino_B.wait, "sin cambio: se cierra al agotar la espera");
    free(r);
    vrbino_reset(); vrbino_set_item(1);
    vrbino_pad(1, 320, VRBINO_PAD_CIRCLE, 2.0f);
    CHECK(!vrbino_B.wait, "sin actor no se mide");
}

int main(void) {
    test_skip();
    test_latency();
    test_safety();
    test_log();
    printf("%d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
