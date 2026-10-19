/* vrai: pegamento de la IA para VR (percepción de soldados, actividad fuera de cámara, cámaras de vigilancia) con el host.
 * Todo corre en el hilo de emulación (ganchos de runtime/host_hooks.h). Usa runtime/rtpatch.h (sustitución de función en
 * ejecución validada por hash); nada se escribe en disco ni en el ejecutable original.
 *
 * Qué hace:
 *  - Soldados: la prueba de visión de cada overlay de zona se envuelve (vr_perception): cono original (alerta inmediata) +
 *    cono ampliado más tenue (sospecha gradual), según el ajuste. El radar de muñeca dibuja el mismo par de conos
 *    (vrai_set_perception_listener).
 *  - Actividad: soldados/sensores dentro del radio de la zona no dependen de que la cámara original los vea (vr_activity).
 *  - Cámaras de vigilancia: detección con su alcance ORIGINAL; se oyen de lejos (curva de sonido natural en la función de
 *    reproducción posicional) y se ven de lejos (la actividad mantiene su estado de recorte), y se rompen con 2 balas
 *    (los explosivos siempre) por la vía del propio juego (vr_cameras).
 *
 * Orden de uso: vrai_on_exe_loaded (EXE ya en RAM) -> vrai_on_bound (HLE enlazadas; instala los parches del ejecutable
 * principal) -> vrai_frame en cada VBlank. Los ajustes se pueden cambiar desde cualquier hilo (se aplican en el siguiente
 * VBlank). Si el ejecutable no es la edición esperada no se instala nada (el juego queda original). */
#pragma once
#include <stdint.h>
#include "vr_perception.h"

struct BridgeState;

typedef struct VraiOptions {
    int perception;   /* VrpPreset: 0 original (sin parches), 1 VR (defecto), 2 exigente */
    int cameras;      /* 1 (defecto): cámaras audibles/visibles de lejos y destruibles con 2 balas; 0: original */
    int activity;     /* 1 (defecto): actividad de soldados y sensores independiente de la cámara original; 0: original */
} VraiOptions;

void vrai_options_default(VraiOptions *o);
void vrai_set_options(const VraiOptions *o);          /* cualquier hilo */
void vrai_get_options(VraiOptions *o);
void vrai_set_perception(int preset);                 /* atajo: solo el ajuste de percepción (cualquier hilo) */
/* Se llama al aplicar un ajuste de percepción y al arrancar (hilo de emulación): el radar copia la configuración activa. */
void vrai_set_perception_listener(void (*fn)(const VrpConfig *cfg));
VrpConfig vrai_perception_config(void);
/* Movimiento del jugador en VR (cabeza/manos) que cuenta como "se mueve" para la regla de periferia. */
void vrai_set_motion(int moving);

/* Menú VR «IA: alcance de visión»: factor 1,0 / 1,5 (defecto) / 2,0 (1..3) sobre el alcance de visión y la zona de sospecha de los SOLDADOS (se aplica dentro de la
 * rutina de visión del motor: alcance del soldado ampliado, con el cono, la línea de visión y las demás reglas originales). No afecta a las cámaras de vigilancia. */
void vrai_set_range_scale(float scale);
float vrai_get_range_scale(void);
/* Los enemigos ven también la cabeza VR (asomarse): interruptor (defecto 1; en el ajuste de percepción «original» nunca) y desplazamiento de la cabeza respecto del
 * ojo del personaje en unidades del motor (vrp_head_offset), una vez por VBlank desde el hilo de emulación; NULL = sin cabeza válida. */
void vrai_set_head_perception(int on);
void vrai_feed_head(const float off[3]);

void vrai_on_exe_loaded(void);                        /* EXE ya en RAM: olvida el estado anterior */
int  vrai_on_bound(void);                             /* 0 = parches del ejecutable instalados; <0 = no (edición distinta o sin hueco) */
void vrai_frame(const struct BridgeState *bs, uint32_t frame);

typedef struct VraiStats {
    int installed;                 /* parches del ejecutable principal instalados */
    int overlay_found;             /* prueba de visión de soldados localizada en la overlay actual */
    uint32_t relocates;            /* veces que se buscó en una overlay nueva */
    uint32_t vision_calls, extended_hits, suspects, alerts_far;
    uint32_t se_calls, se_played, se_dropped;
    int cameras_seen;              /* cámaras de vigilancia en la última tabla */
    int patches;                   /* sustituciones vigentes */
} VraiStats;
void vrai_stats(VraiStats *s);
