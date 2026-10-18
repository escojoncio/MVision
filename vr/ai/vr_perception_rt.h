/* Unión de vr_perception con el runtime (parches de función por dirección). Requiere cpu.h del runtime.
 * Contrato esperado del mecanismo de parches (vr/camera):
 *   int  rt_patch(uint32_t addr, FuncPtr fn);   registra `fn` en lugar de la función que empieza en `addr`, también
 *                                               para llamadas directas dentro de la overlay; 0 = ok
 *   void rt_unpatch(uint32_t addr);
 *   void rt_call_orig(Cpu *c, uint32_t addr);   ejecuta el código original de `addr` (sin parche) como una llamada
 * `fn` recibe el estado de CPU de la llamada (a0 = r[4]) y vuelve como lo haría la función. */
#pragma once
#include "vr_perception.h"

void vrp_rt_init(VrpPreset p);                       /* una vez, tras iniciar el runtime */
void vrp_rt_set_preset(VrpPreset p);                 /* ajuste de la app: original / VR / exigente */
void vrp_rt_set_gather(int on);                      /* 0: no parchea la recogida de eventos (sin aviso de sospecha '?') */
void vrp_rt_set_external_motion(int moving);         /* movimiento del jugador en VR (cabeza/manos) */
/* La overlay activa cambió (coincidencia por hash): [lo,hi) = su ventana de código. Quita los parches anteriores,
 * localiza la prueba de visión de soldados y, si el ajuste no es el original, registra los parches (con el original
 * no se parchea nada; vrp_rt_set_preset los pone/quita al vuelo). Devuelve 1 si la localizó. */
int  vrp_rt_overlay(uint32_t code_lo, uint32_t code_hi);
void vrp_rt_overlay_gone(void);                      /* overlay invalidada/descargada */
void vrp_rt_frame(float seconds);                    /* una vez por fotograma (VBlank): reloj del modelo */
const VrpCtx *vrp_rt_ctx(void);
