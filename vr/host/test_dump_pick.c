/* Prueba de la selección de captura del volcado (vrdump.h): pedido en modo gpu (sin punto de enganche) y en nativo. make -C vr/host test */
#include <stdio.h>
#include "vrdump.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FALLO %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

int main(void) {
    /* pedido: la pareja en gpu no espera un punto de enganche que no existe */
    CHECK(dump_need_init(0, 1) == DN_FRAME, "gpu + pareja: solo el final de fotograma");
    CHECK(dump_need_init(0, 0) == DN_ANY, "gpu simple: la primera que llegue");
    CHECK(dump_need_init(1, 1) == (DN_STAGE | DN_FRAME), "nativo + pareja: enganche y final");
    CHECK(dump_need_init(1, 0) == DN_ANY, "nativo simple");
    /* captura con lista (gancho de fotograma, dump_point = 0) en gpu: debe tomarla, no devolver 0 */
    CHECK(dump_pick_bit(dump_need_init(0, 1), 0) == DN_FRAME, "gpu pareja: la llamada con lista captura el final");
    CHECK(dump_pick_bit(dump_need_init(0, 1), 1) == 0, "gpu pareja: el punto de enganche no captura");
    CHECK(dump_pick_bit(dump_need_init(0, 0), 0) == DN_ANY, "gpu simple con lista");
    /* nativo: comportamiento previo */
    CHECK(dump_pick_bit(DN_STAGE | DN_FRAME, 0) == 0, "nativo pareja: el final espera al enganche");
    CHECK(dump_pick_bit(DN_STAGE | DN_FRAME, 1) == DN_STAGE, "nativo pareja: el enganche primero");
    CHECK(dump_pick_bit(DN_FRAME, 0) == DN_FRAME, "enganche ya hecho: el final");
    CHECK(dump_pick_bit(DN_FRAME, 1) == 0, "enganche ya hecho: nada en el punto");
    CHECK(dump_pick_bit(DN_ANY, 1) == DN_ANY && dump_pick_bit(DN_ANY, 0) == DN_ANY, "simple: cualquiera");
    CHECK(dump_pick_bit(0, 0) == 0 && dump_pick_bit(0, 1) == 0, "sin pedido");
    fprintf(stderr, fails ? "test_dump_pick: %d FALLOS\n" : "test_dump_pick: OK\n", fails);
    return fails ? 1 : 0;
}
