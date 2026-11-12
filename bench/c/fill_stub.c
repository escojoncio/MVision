/* Sustituto del relleno por la verdad del motor (vr/native/ns_fill_glue.c) SOLO para los bancos (NativeRenderer ya no llama a ns_fill_glue_fetch; se conservan los símbolos): el banco no tiene lista de
 * la GPU ni RAM del motor que rellenar, y enlazar el trabajador real arrastraría el extractor completo (que sustituiría los ns_store_* débiles del puente del banco). Sin relleno. */
#include "ns_fill_glue.h"
#include "ns_solid.h"

void ns_fill_glue_post_ik(const uint8_t *ram) { (void)ram; }
void ns_fill_glue_submit_cam(const uint8_t *ram, const uint8_t *scratch, const GpuOp *ops, uint32_t nops, uint32_t frame, const float *eye, const float *M, int list_vr) {
    (void)ram; (void)scratch; (void)ops; (void)nops; (void)frame; (void)eye; (void)M; (void)list_vr;
}
uint32_t ns_fill_glue_fetch(NsEffect *cap, uint32_t max) { (void)cap; (void)max; return 0; }
size_t ns_fill_glue_poll(char *buf, size_t cap) { (void)buf; (void)cap; return 0; }
void ns_fill_glue_reset(void) {}
void ns_fill_glue_set_mode(int draw) { (void)draw; }
int ns_fill_glue_scenery_stale(void) { return 0; }

/* Cuerpo del jugador a la cadencia del renderer (vr/host/vrhost.c, que los bancos no enlazan): sin anfitrión, la matriz de la escena tal cual. */
int32_t mv_vrhost_body_piece(const void *sc, uint32_t inst, uint32_t k, void *out_xf) { (void)sc; (void)inst; (void)k; (void)out_xf; return 0; }

/* Cota del paralaje de la pantalla 3D (vr/native/ns_solid.c, que los bancos no enlazan: arrastraría el extractor): 0 = desconocida, la ventana estéreo solo mira las piezas dinámicas. */
float ns_solid_view_near(const NsScene *sc, const NsMeshStore *st) { (void)sc; (void)st; return 0.0f; }
