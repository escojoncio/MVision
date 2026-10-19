/* vr_heading: el rumbo del PERSONAJE del juego es la referencia del jugador (cuerpo completo en primera persona).
 *
 *  - El giro con el stick derecho (suave o por pasos; lo calcula vr/input) rota el rumbo del personaje en la RAM del juego
 *    (campo de rotación del bloque de control, el mismo que lee el movimiento, el apuntado y el dibujado del cuerpo) y a la
 *    vez el espacio de usuario de la cámara (vrcam_turn): la vista = rumbo del personaje + giro físico de la cabeza.
 *  - El torso sigue a la cabeza con margen: parado, el personaje solo rota cuando la cabeza se desvía más de `margin_deg`
 *    de su rumbo (se arrastra el rumbo hasta quedar en el margen, a `follow_dps`); si la cabeza se queda girada más de
 *    `relax_s` segundos, el rumbo se relaja despacio hacia la mirada (`relax_dps`) hasta `relax_deg`. Andando, el propio
 *    juego orienta al personaje hacia la dirección del mando (el origen del mando es la mirada), así que aquí no se escribe
 *    (salvo el giro del stick, que es inmediato).
 *
 * Convenciones: ángulos en grados con el sentido del motor (+ = izquierda, antihorario visto desde arriba); 4096 = vuelta.
 * Se llama desde el hilo del emulador en el gancho de VBlank (CPU parada), tras vrcam_frame. Escribe SOLO el yaw del bloque de
 * control del jugador. Sin dependencias del runtime: opera sobre un puntero a la RAM de 2 MB. */
#pragma once
#include <stdint.h>
#include "bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VrHeadingConfig {
    int enabled;          /* 1 (defecto); 0: el giro del stick solo rota la vista (comportamiento anterior) */
    float margin_deg;     /* desvío cabeza-torso que se tolera antes de arrastrar al personaje (defecto 45) */
    float follow_dps;     /* velocidad con la que el torso alcanza el margen (defecto 300 grados/s) */
    float relax_deg;      /* desvío al que se relaja el torso si la cabeza se queda girada (defecto 12; 0 = no relajar) */
    float relax_s;        /* tiempo con la cabeza quieta y girada antes de relajar (defecto 1,2 s) */
    float relax_dps;      /* velocidad de la relajación (defecto 45 grados/s) */
    float frame_dt_s;     /* duración de un VBlank del motor (0,02) */
} VrHeadingConfig;

void vrhead_config_default(VrHeadingConfig *c);
void vrhead_init(const VrHeadingConfig *c);              /* reinicia el estado */
const VrHeadingConfig *vrhead_config(void);
/* Configuración que usará vrhead_init(NULL) (el host la aplica al cargar el ejecutable). NULL = la de defecto. */
void vrhead_set_default(const VrHeadingConfig *c);

/* Giro pedido por el mando (grados, + = izquierda): rota la vista (vrcam_turn) y, en el próximo vrhead_frame, el personaje. */
void vrhead_turn(float deg);
/* Gancho de VBlank. `walking` = el personaje se mueve por orden del mando. Devuelve 1 si escribió el rumbo. Sin cámara VR
 * activa (vrcam_active) no hace nada. */
int vrhead_frame(uint8_t *ram, const BridgeState *st, int walking);
/* Rumbo del personaje (grados 0..360) tal como lo leyó el último vrhead_frame; desvío cabeza-torso (-180..180, + = la vista
 * está a la izquierda del personaje). */
float vrhead_body_yaw(void);
float vrhead_offset_deg(void);

/* Matemática pura (para pruebas): paso del seguimiento. off = vista - rumbo (grados, envuelto); devuelve el giro del
 * rumbo (grados) para este paso. `still_s` = tiempo que lleva la cabeza quieta. */
float vrhead_follow_step(const VrHeadingConfig *c, float off, float still_s, float dt);

#ifdef __cplusplus
}
#endif
