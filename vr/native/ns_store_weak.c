/* Definiciones DÉBILES del almacén de mallas (ns_types.h, «almacén de mallas») para que la app enlace antes de que exista el extractor: sin mallas el renderer
 * nativo no dibuja nada. El almacén real (extractor) las define fuertes y sustituye a éstas. */
#include "ns_types.h"

__attribute__((weak)) const NsMesh *ns_store_get(const NsMeshStore *st, uint64_t id) { (void)st; (void)id; return NULL; }
__attribute__((weak)) const NsBaked *ns_store_get_baked(const NsMeshStore *st, uint64_t mesh_id, uint32_t ver) { (void)st; (void)mesh_id; (void)ver; return NULL; }
__attribute__((weak)) uint32_t ns_store_epoch(const NsMeshStore *st) { (void)st; return 0; }
__attribute__((weak)) void ns_store_ack(NsMeshStore *st, uint32_t epoch) { (void)st; (void)epoch; }
