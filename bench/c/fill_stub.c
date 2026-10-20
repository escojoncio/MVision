/* Sustituto del relleno por la verdad del motor (vr/native/ns_fill_glue.c) SOLO para los bancos: NativeRenderer.ingest llama a ns_fill_glue_fetch, pero el banco no tiene lista de
 * la GPU ni RAM del motor que rellenar, y enlazar el trabajador real arrastraría el extractor completo (que sustituiría los ns_store_* débiles del puente del banco). Sin relleno. */
#include "ns_fill_glue.h"

void ns_fill_glue_post_ik(const uint8_t *ram) { (void)ram; }
void ns_fill_glue_submit_cam(const uint8_t *ram, const uint8_t *scratch, const GpuOp *ops, uint32_t nops, uint32_t frame, const float *eye, const float *M, int list_vr) {
    (void)ram; (void)scratch; (void)ops; (void)nops; (void)frame; (void)eye; (void)M; (void)list_vr;
}
uint32_t ns_fill_glue_fetch(NsEffect *cap, uint32_t max) { (void)cap; (void)max; return 0; }
size_t ns_fill_glue_poll(char *buf, size_t cap) { (void)buf; (void)cap; return 0; }
void ns_fill_glue_reset(void) {}
