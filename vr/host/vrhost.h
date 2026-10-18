#pragma once
/* vrhost: pegamento entre el host del runtime (runtime/host.c) y la capa VR (puente RAM, cámara VR, ensamblador de escena).
 * Vive fuera de runtime/ para que los .c de runtime/ no dependan de vr/ (varias herramientas enlazan todo runtime/ tal cual).
 *
 * Uso (app): mv_vrhost_install() una vez, ANTES de mv_host_start. Después:
 *  - el hilo de render llama a mv_host_scene() (RenderPS1Bridge.h) por frame para recibir las listas de dibujo ya ensambladas;
 *  - el hilo de entrada lee mv_vrhost_game_state() (postura, menú) para alimentar vr_input, publica la pose de la cabeza
 *    con mv_vr_set_head() y los giros con mv_vr_turn() / mv_vr_recenter() (host.h).
 *
 * Qué hace en el hilo de emulación, en cada VBlank (tras publicar el vídeo y aplicar el mando):
 *    bridge_read -> (giros/recentrado pendientes) -> vrcam_frame -> [si hay consumidor de escenas] drena la captura de la GPU,
 *    ensambla una VrScene por lista cerrada (vr_scene_build con el BridgeState del mismo frame) y la copia a memoria propia.
 * Al cargar el EXE (solo MV_NOGEN): parchea el recorte (vrcam_patch_ram) y registra las HLE de la cámara por DIRECCIÓN FIJA
 * (hle_bind_fixed; no hay firmas de la librería para ellas). Si el EXE no coincide con la edición esperada no hay cámara VR.
 * La IA de vr/ai NO está integrada (requiere un mecanismo rt_patch que aún no existe en el runtime). */
#include <stdint.h>
#include "vr_scene.h"

#ifdef __cplusplus
extern "C" {
#endif

void mv_vrhost_install(void);

/* Escena VR del siguiente frame (contrato en app/Sources/RenderPS1Bridge.h). Una por llamada y sin saltarse ninguna;
 * los punteros de `out` valen hasta la siguiente llamada. Llamarla es lo que activa la captura de la GPU: sin llamadas durante
 * ~1,5 s se apaga (ventana plana) y al volver la primera lista trae la VRAM completa (vram_init). Thread-safe. */
extern int mv_host_scene(VrScene *out);

/* Resumen del último frame del puente para Swift (sin importar el BridgeState entero). */
typedef struct {
    int32_t game;        /* 1 si hay jugador en escena con el puente completo (partida) */
    int32_t in_control;  /* el jugador controla a su personaje */
    int32_t posture;     /* -1 desconocida, 0 de pie, 1 agachado, 2 tumbado */
    int32_t menu;        /* 1 = fuera de control (título, carga, pausa, menú, comunicador, escena): sin locomoción, stick -> cruceta */
    int32_t cam_active;  /* 1 si la cámara del motor es la de la cabeza del usuario */
    int32_t first_person;
    int32_t weapon;      /* -1 ninguna, 0..9 */
    int32_t life, life_max;
} MvVrGameState;
void mv_vrhost_game_state(MvVrGameState *out);

/* Copia el BridgeState del último VBlank (bridge.h). Devuelve 1 si hay uno publicado. Thread-safe. */
struct BridgeState;
int mv_host_bridge(struct BridgeState *out);

typedef struct {
    uint32_t built;          /* escenas ensambladas */
    uint32_t delivered;      /* entregadas por mv_host_scene */
    uint32_t has3d, world_valid;
    uint32_t full_waits;     /* VBlanks en que la cola propia estaba llena y las listas esperaron en la captura */
    uint32_t cap_resyncs;    /* veces que se (re)encendió la captura por llegada de un consumidor */
    uint32_t cam_rec_found, cam_rec_missing; /* listas con/sin registro de cámara asociado por secuencia (solo las que tienen polígonos con posición de vista) */
    uint32_t cam_rec_differs; /* de las halladas: cuántas traen una cámara distinta de la del puente en ese VBlank */
    double build_ms_avg, build_ms_max;
    int cam_ok;              /* 1 si el EXE coincidió con la edición esperada (parche + HLE de cámara) */
} MvVrHostStats;
void mv_vrhost_stats(MvVrHostStats *s);

#ifdef __cplusplus
}
#endif
