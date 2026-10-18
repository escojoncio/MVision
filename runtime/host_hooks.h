#pragma once
/* Puntos de enganche del host (solo C, para la capa VR de vr/host/vrhost.c; host.h es lo que ve Swift).
 * Se instalan con mv_host_set_hooks ANTES de mv_host_start. Todos se llaman desde el hilo de emulación. Existen para que
 * los .c de runtime/ no dependan de vr/ (varias herramientas enlazan todo runtime/ tal cual). */
#include <stdint.h>
#include "cpu.h"

typedef struct MvHostHooks {
    /* EXE ya copiado a la RAM (texto en [text_base, text_base+text_size)) y antes de enlazar las HLE: puede parchearlo
     * (debe llamar a rt_code_invalidate si lo hace) y declarar HLE de dirección fija (hle_bind_declare_fixed). */
    void (*exe_loaded)(uint8_t *ram, uint32_t text_base, uint32_t text_size);
    /* Tras enlazar las HLE por firmas (hle_bind_fixed ya puede usarse). */
    void (*bound)(void);
    /* Tras gte_reset/gpu_reset, justo antes de ejecutar el EXE. */
    void (*start)(void);
    /* En cada VBlank (CPU parada), tras aplicar el mando y publicar el vídeo y antes del ritmo/pausa. */
    void (*frame)(Cpu *c, uint32_t frame);
} MvHostHooks;
void mv_host_set_hooks(const MvHostHooks *h);   /* copia; NULL los quita */

/* Peticiones de otros hilos para la capa de cámara (host_vr.c): giro acumulado (grados, + = izquierda) y recentrado; la
 * función "take" devuelve lo pendiente y lo borra (hilo de emulación). */
float mv_vr_take_turn(void);
int mv_vr_take_recenter(void);
/* Segundos desde la última pose de cabeza publicada (mv_vr_set_head); < 0 si no hay. */
double mv_vr_head_age(void);
