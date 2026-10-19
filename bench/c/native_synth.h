#pragma once
/* Banco del renderer nativo: escena sintética (generada por código, sin datos del juego) + adaptador a la referencia de CPU (ns_cpuref). */
#include <stddef.h>
#include <stdint.h>
#include "ns_types.h"
#include "ns_cpuref.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Genera el fichero de prueba (cabecera + escena + mallas + VRAM). 0 = bien. */
int ns_synth_write(const char *path);
/* Lo lee, prepara el almacén de mallas y publica la escena por el puente (mv_native_publish). 0 = bien. */
int ns_synth_load(const char *path);
/* VRAM leída del fichero (1024*512 palabras) o NULL. */
const uint16_t *ns_synth_vram(void);
/* Posición de la cabeza (mundo VR, m) desde la que se renderiza el cubemap de comparación. */
void ns_synth_head(float pos[3]);
/* Referencia: renderiza la escena publicada con ns_cpuref (modo float) a un cubemap n x n centrado en la cabeza. Devuelve los triángulos emitidos (<0 = error). */
long ns_synth_ref_cube(int n, float znear, NsRBuf faces[6], NsRStats *st);
/* Triángulos de referencia de la escena publicada (ns_cpuref, posiciones en mundo VR); el llamador los libera con free(). Devuelve cuántos. */
size_t ns_synth_ref_tris(NsRTri **out);
/* Publica la escena de prueba de decals (cuadrilátero rojo + coplanar verde con sesgo de orden). `epoch` = mesh_epoch de la publicación. 0 = bien. */
int ns_synth_publish_decal(uint32_t epoch);
/* Última época reconocida por el consumidor (ns_store_ack): la prueba comprueba que el renderer reconoce tras subir las mallas. */
uint32_t ns_synth_acked(void);
/* Base de cada cara del cubemap respecto a la pose (filas: derecha, abajo, delante en el espacio de la pose), igual que ns_render_cube. */
const float *ns_synth_face_basis(int face);
/* Base de la pose (filas: derecha, abajo, delante) en el mundo VR. */
const float *ns_synth_pose_basis(void);

#ifdef __cplusplus
}
#endif
