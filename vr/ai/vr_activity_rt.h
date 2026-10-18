/* Unión de vr_activity con el runtime (parches de función por dirección). Requiere cpu.h del runtime.
 * Contrato del mecanismo de parches (el mismo que vr_perception_rt.h):
 *   int  rt_patch(uint32_t addr, FuncPtr fn);   registra `fn` en lugar de la función que empieza en `addr` (también
 *                                               para llamadas directas); 0 = ok
 *   void rt_unpatch(uint32_t addr);
 *   void rt_call_orig(Cpu *c, uint32_t addr);   ejecuta el código original de `addr` (sin parche) como una llamada
 * Además se necesita UN gancho a mitad de función (no expresable con rt_patch de función entera):
 *   int  rt_hook_at(uint32_t addr, uint32_t (*fn)(Cpu *c, uint32_t r4, uint32_t r7));
 * semántica: justo ANTES de ejecutar la instrucción de `addr` se llama a fn(c, c->r[4], c->r[7]) y su resultado
 * se escribe en c->r[7]; luego la función sigue. (En vr_activity_ai2_hooks.c se implementa para la copia local.)
 *
 * Parches que registra vra_rt_install:
 *   0x8001c30c  proyección de puntos a pantalla (soldados, M5): llama al original y marca `pad`=1 en los puntos activos
 *   0x80018fc4  (a mitad de función) almacén de bound_mode del paso de límites del canal 3D (M3/M4)
 * No parchea nunca: la variante de un punto (0x8001c564) ni código de overlays.
 */
#pragma once
#include "cpu.h"
#include "vr_activity.h"

#define VRA_EU_POINTCHECK     0x8001c30cu
#define VRA_EU_BOUND_STORE_AT 0x80018fc4u   /* sh $7,0x32($4) en el paso de límites 0x80018c8c */

#define VRA_EU_BOUND_DRAW     0x800188a4u   /* función que construye los paquetes de un bloque (única llamadora: 0x80018fc8) */

/* Instancia global que usan los parches (una por proceso). NULL = parches inertes (comportamiento original). */
void vra_rt_attach(VrActivity *a);
VrActivity *vra_rt_instance(void);
/* Registra los parches (con las funciones rt_patch/rt_hook_at del runtime). 0 = ok. */
int vra_rt_install(void);            /* pointcheck + gancho a mitad de función (requiere rt_hook_at) */
int vra_rt_install_fnpatch(void);    /* pointcheck + parche de entrada de 0x800188a4: solo rt_patch, sin gancho (recomendado) */
void vra_rt_bound_draw(Cpu *c);
void vra_rt_uninstall(void);
/* Los dos parches, expuestos para pruebas y para que el dueño del paso de límites pueda llamarlos él mismo si
 * sustituye esa función entera: */
void vra_rt_pointcheck(Cpu *c);
uint32_t vra_rt_bound_store(Cpu *c, uint32_t objs, uint32_t mode);
/* Contadores de diagnóstico (llamadas y puntos/objetos modificados). */
typedef struct { uint64_t pc_calls, pc_points_changed, bound_calls, bound_changed; } VraRtStats;
VraRtStats vra_rt_stats(void);
