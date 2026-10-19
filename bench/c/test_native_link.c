/* Prueba de enlace del almacén de mallas: el binario se enlaza SOLO con el extractor real (ns_extract.c + su registro) y debe resolver todos los ns_store_* a
 * definiciones fuertes, sin sustitutos débiles. build_native.sh comprueba además con `nm` que no queda ningún símbolo débil ns_store_*. */
#include "ns_bridge.h"
#include "ns_extract.h"
#include "ns_hook.h"
#include <stdio.h>

void ns_extract_register(void);

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
    NsBridge *b = ns_bridge_create(NULL);
    CHECK(b != NULL);
    ns_hook_init(b);
    CHECK(!ns_hook_has_extractor());
    ns_extract_register();
    CHECK(ns_hook_has_extractor());            /* el registro llamó a ns_hook_set_extractor */
    CHECK(ns_store_is_stub() == 0);
    NsMeshStore *st = ns_store_create();
    CHECK(st != NULL);
    uint32_t e = ns_store_epoch(st);
    CHECK(e >= 1);
    CHECK(ns_store_get(st, 123) == NULL);
    CHECK(ns_store_get_baked(st, 123, 1) == NULL);
    ns_store_ack(st, e);
    ns_store_reset(st);
    CHECK(ns_store_zone_gen(st) >= 2);
    ns_store_destroy(st);
    ns_hook_reset();                           /* llama al reinicio registrado con el almacén real */
    ns_bridge_destroy(b);
    printf(fails ? "FALLOS: %d\n" : "OK: enlace del almacén de mallas\n", fails);
    return fails ? 1 : 0;
}
