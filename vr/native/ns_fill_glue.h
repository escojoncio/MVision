#pragma once
/* ns_fill_glue: relleno por la verdad del motor en la app (ver ns_fill.h). Un hilo trabajador (un solo trabajo a la vez, sin cola) recibe la instantánea de la RAM y la lista
 * de la GPU de cada fotograma, ejecuta la sesión (ns_fill_session_run) y deja los efectos de relleno listos; el renderer los recoge con ns_fill_glue_fetch (UN fotograma de retraso
 * como mucho: el relleno persiste anclado al mundo, así que el retraso no se nota). MV_FILL=0 lo apaga; MV_FILL_N = 1 de cada N listas (def. 1). */
#include <stdint.h>
#include "gpu.h"
#include "ns_types.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Hilo de emulación, tras vrbody_apply: instantánea de la RAM con la pose IK (solo si el trabajador está libre). */
void ns_fill_glue_post_ik(const uint8_t *ram);
/* Hilo de emulación, al cerrarse una lista: eye/M = cámara con la que se emitió (NULL = la de la RAM); list_vr = VrCamRecord.active. Barato (copia ~2 MiB) y no bloquea. */
void ns_fill_glue_submit_cam(const uint8_t *ram, const uint8_t *scratch, const GpuOp *ops, uint32_t nops, uint32_t frame, const float *eye, const float *M, int list_vr);
/* Cualquier hilo (renderer): copia en cap[0..max) los efectos de relleno vigentes (NS_EK_QUAD en el mundo del motor, src_key = 0xF1000000|seq) y devuelve cuántos. */
uint32_t ns_fill_glue_fetch(NsEffect *cap, uint32_t max);
/* Texto de registro (~1 por segundo) del trabajador; devuelve su longitud, 0 si no hay nada nuevo. */
size_t ns_fill_glue_poll(char *buf, size_t cap);
/* Cualquier hilo: última medida de COBERTURA de la lista de la GPU por el nativo + el relleno (lo que se ve dentro de la vista del juego): fotograma de la lista y % (0..100), sin pose. 0 si no hay
 * ninguna (relleno apagado o aún sin escena 3D). La decisión motor/respaldo de la vista (vrhost -> vr_view) la usa en vez de la del nativo solo (ns_cover_glue_last). */
int ns_fill_glue_last(uint32_t *frame, int *pct);
/* 1 si la última medida dice que el escenario nativo no casa con la lista (nativo < 10 %, nativo + relleno >= 80 %): el renderer no dibuja las instancias estáticas del nativo en la pantalla 3D (el relleno ya lo cubre). */
int ns_fill_glue_scenery_stale(void);
/* Vacía la persistencia (cambio de zona conocido). */
void ns_fill_glue_reset(void);
#ifdef __cplusplus
}
#endif
