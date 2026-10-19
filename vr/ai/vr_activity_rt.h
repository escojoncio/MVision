/* Unión de vr_activity con el runtime. Requiere cpu.h y rtpatch.h del runtime (sustitución de función por dirección:
 * rt_patch_fn / rt_unpatch / rt_call_orig, validada por el hash del bloque de código).
 *
 * Parches que registra vra_rt_install_fnpatch (solo del ejecutable principal; nunca de overlays):
 *   0x8001c30c  proyección de puntos a pantalla (soldados): llama al original y marca `pad`=1 en los puntos activos
 *   0x800188a4  construcción de los paquetes de un bloque de objeto 3D: antes de ejecutarla corrige el estado de recorte
 *               (clip_state, s16 @+0x32 del bloque, y el registro r7 que la función ya tiene copiado) de soldados/sensores activos
 * No parchea nunca la variante de un punto (0x8001c564).
 */
#pragma once
#include "cpu.h"
#include "vr_activity.h"

#define VRA_EU_POINTCHECK     0x8001c30cu

#define VRA_EU_CLIP_DRAW     0x800188a4u   /* función que construye los paquetes de un bloque (única llamadora: 0x80018fc8) */
/* Guardas de los dos bloques (nº de palabras y rt_code_hash) de la edición esperada: otra edición no se parchea. */
#define VRA_EU_POINTCHECK_NW   149u
#define VRA_EU_POINTCHECK_HASH 0x1ff0367639872e56ull
#define VRA_EU_CLIP_DRAW_NW    249u
#define VRA_EU_CLIP_DRAW_HASH  0x538a9302bf5e594bull   /* código del ejecutable sin tocar */
#define VRA_EU_CLIP_DRAW_HASH_VRCAM 0xf6fd9952c641334bull /* idem con el parche de recorte de vr/camera ya aplicado (el host siempre lo aplica) */
#define VRA_EU_CLIP_DRAW_HASH_VRCAM_NC 0xa543389b52276e0bull /* idem con ademas el parche sin recorte de vr/camera (0x800188F8) */

/* Instancia global que usan los parches (una por proceso). NULL = parches inertes (comportamiento original). */
void vra_rt_attach(VrActivity *a);
VrActivity *vra_rt_instance(void);
/* Registra los dos parches con sus guardas. 0 = ok, <0 = no se pudo (nada queda parcheado). */
int vra_rt_install_fnpatch(void);
void vra_rt_clip_draw(Cpu *c);
void vra_rt_uninstall(void);
/* Gancho previo del constructor (r4 = bloque, r7 = modo; puede cambiar r7): lo usa vr/camera para separar el estado de recorte de dibujo del de lógica.
 * La sustitución del constructor puede instalarse sola (vra_rt_install_builder, idempotente, 0 = ok); vra_rt_uninstall no la quita. */
void vra_rt_set_pre_builder(void (*fn)(Cpu *));
int vra_rt_install_builder(void);
/* Los dos parches, expuestos para pruebas y para que el dueño del paso de límites pueda llamarlos él mismo si
 * sustituye esa función entera: */
void vra_rt_pointcheck(Cpu *c);
uint32_t vra_rt_clip_store(Cpu *c, uint32_t objs, uint32_t mode);
/* Contadores de diagnóstico (llamadas y puntos/objetos modificados). */
typedef struct { uint64_t pc_calls, pc_points_changed, clip_calls, clip_changed; } VraRtStats;
VraRtStats vra_rt_stats(void);
