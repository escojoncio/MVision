/* Registro del extractor real en el enganche (ns_hook): vrhost llama a ns_extract_register() en mv_vrhost_install (tiene una definición débil vacía; ésta la sustituye).
 * Crea el almacén de mallas, registra la extracción de la RAM y el reinicio del almacén al cambiar de ejecutable. Hilo: el de instalación; después el enganche
 * llama a ns_extract_ex en el hilo de emulación (único productor). */
#include "ns_extract.h"
#include "ns_hook.h"

static NsMeshStore *g_store;

static int glue_extract(const uint8_t *ram, const uint8_t *scratch, NsScene *out, void *user) {
    return ns_extract_ex(out, (NsMeshStore *)user, ram, NS_RAM_SIZE, scratch, scratch ? 1024u : 0u, NULL, NULL);
}
static void glue_reset(NsMeshStore *st, void *user) { (void)user; ns_store_reset(st); }

void ns_extract_register(void) {
    if (!g_store) g_store = ns_store_create();
    if (!g_store) return;
    ns_hook_set_extractor(glue_extract, g_store, g_store);
    ns_hook_set_store_reset(glue_reset, NULL);
}
