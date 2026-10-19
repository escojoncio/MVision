#pragma once
/* vrhost: pegamento entre el host del runtime (runtime/host.c) y la capa VR (puente RAM, cámara VR, ensamblador de escena).
 * Vive fuera de runtime/ para que los .c de runtime/ no dependan de vr/ (varias herramientas enlazan todo runtime/ tal cual).
 *
 * Uso (app): mv_vrhost_install() una vez, ANTES de mv_host_start. Después:
 *  - el hilo de render llama a mv_host_scene() (RenderRGBridge.h) por frame para recibir las listas de dibujo ya ensambladas;
 *  - el hilo de entrada lee mv_vrhost_game_state() (postura, menú) para alimentar vr_input, publica la pose de la cabeza
 *    con mv_vr_set_head() y los giros con mv_vr_turn() / mv_vr_recenter() (host.h).
 *
 * Qué hace en el hilo de emulación, en cada VBlank (tras publicar el vídeo y aplicar el mando):
 *    bridge_read -> (giros/recentrado pendientes) -> vrcam_frame -> [si hay consumidor de escenas] drena la captura de la GPU,
 *    ensambla una VrScene por lista cerrada (vr_scene_build con el BridgeState del mismo frame) y la copia a memoria propia.
 * Al cargar el EXE (solo MV_NOGEN): parchea el recorte (vrcam_patch_ram) y registra las HLE de la cámara por DIRECCIÓN FIJA
 * (hle_bind_fixed; no hay firmas de la librería para ellas). Si el EXE no coincide con la edición esperada no hay cámara VR.
 * IA de enemigos y cámaras (vr/ai): se instala con las HLE si el EXE es la edición esperada (ver mv_vrhost_set_ai). */
#include <stdint.h>
#include "vr_radar.h"
#include "vr_scene.h"

#ifdef __cplusplus
extern "C" {
#endif

void mv_vrhost_install(void);

/* IA de enemigos y cámaras (vr/ai/vrai.h); cualquier hilo, antes o después de arrancar (se aplica en el siguiente VBlank).
 *   perception: 0 original (el juego tal cual), 1 VR (defecto: visión ampliada con sospecha gradual), 2 exigente
 *   cameras:    1 (defecto) cámaras de vigilancia audibles/visibles de lejos y rompibles con 2 balas (alcance de detección original); 0 original
 *   activity:   1 (defecto) soldados y sensores activos aunque la cámara original no los vea; 0 original
 * El radar de muñeca dibuja el cono original y el ampliado según `perception`. */
void mv_vrhost_set_ai(int perception, int cameras, int activity);
/* Solo para herramientas de prueba (vr/ai/test_ai_run): función llamada en el gancho de VBlank del hilo de emulación tras la capa VR (cpu = Cpu*). */
void mv_vrhost_set_test_hook(void (*cb)(void *cpu, uint32_t frame));

/* Escena VR del siguiente frame (contrato en app/Sources/RenderRGBridge.h). Una por llamada y sin saltarse ninguna;
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
    /* --- presentación (vr_view.h; al final: no rompe a quien solo lea lo anterior) --- */
    int32_t view_mode;   /* VrViewMode con histéresis: 1 = mundo en primera persona, 0 = pantalla de cine */
    int32_t view_reason; /* VRV_R_* del último VBlank (motivo del cine / del mundo) */
    int32_t cinematic, comms, paused, menu_open, stopped;   /* banderas del puente (escena del motor, radio, pausa, ventana de armas/objetos, detenido) */
    int32_t ammo, ammo_max;  /* munición del arma equipada (cargador del juego) y su máximo; -1 si no hay */
    int32_t item;            /* objeto equipado, -1 ninguno */
    int32_t alert_mode;      /* 0 normal, 1 interferencia, 2 evasión, 3 alerta */
    /* --- cinemáticas del motor (vr_view.h, vr_cine.h) --- */
    int32_t view_kind;       /* VRV_K_*: 0 2D, 1 mundo, 2 cinemática del motor (3D estereoscópico), 3 vídeo pregrabado */
    uint32_t cine_shot;      /* plano en curso de la cinemática (sube en cada corte de cámara) */
    uint32_t cine_cuts;      /* cortes de cámara detectados desde que se cargó el ejecutable */
    int32_t cine_cover;      /* fundidos/destellos (recubrimientos a pantalla completa) en la última lista */
} MvVrGameState;
void mv_vrhost_game_state(MvVrGameState *out);

/* Copia el BridgeState del último VBlank (bridge.h). Devuelve 1 si hay uno publicado. Thread-safe. */
struct BridgeState;
/* Radar de muñeca: escena del radar (vr/radar/vr_radar.h) asociada a la ÚLTIMA escena entregada por mv_host_scene (mismos punteros
 * válidos, hasta la siguiente llamada). Devuelve 0 si esa escena no traía radar (el juego lo oculta/apaga/interfiere sin dibujarlo, o
 * cinemática): entonces no se dibuja el quad en el brazo. La escena del HUD ya NO lleva las primitivas del radar. */
int mv_host_radar(VrRadarScene *out);

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
