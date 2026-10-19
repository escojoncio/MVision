/* Sustitutos débiles de las dos funciones de vr/body que llama vr_camera_hle.c (vrbody_apply / vrbody_restore): las herramientas
 * que enlazan la capa de cámara sin la del cuerpo (camtest) siguen enlazando y no tocan nada. Con vr/body enlazado, las suyas
 * (fuertes) mandan. */
#include <stdint.h>

__attribute__((weak)) int vrbody_apply(uint8_t *ram) { (void)ram; return 0; }
__attribute__((weak)) void vrbody_restore(uint8_t *ram) { (void)ram; }
