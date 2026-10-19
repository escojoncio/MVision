#pragma once
/* vr_view: decide, una vez por VBlank, QUÉ se le enseña al usuario en el visor: el mundo en primera persona (cámara VR del juego,
 * escena 3D en estéreo + HUD en paneles) o la PANTALLA DE CINE (el fotograma completo que dibuja el juego, en una pantalla virtual
 * grande, para todo lo que no es control en primera persona: título, menús, códec, cargas, vídeos, cinemáticas del motor, fin de
 * partida, pausa). C99 puro, sin dependencias de plataforma; lo prueba test_view.c.
 *
 * Política (ver bitácora, "Presentación en VR"):
 *   - MUNDO solo si hay partida (puente completo + jugador), el jugador controla a su personaje, y la cámara VR del juego está
 *     activa (si no, el motor dibuja con SU cámara, cenital, y mezclarla con el mundo del usuario sería un salto de 8 m de altura).
 *   - Todo lo demás es CINE. Si la cámara VR no está disponible (edición del ejecutable no reconocida), siempre CINE: el juego se
 *     ve igualmente, en pantalla grande.
 *   - Histéresis en fotogramas de VBlank (50 Hz) para que un parpadeo de una bandera del juego no haga saltar la vista:
 *     a CINE tras `to_cine` fotogramas seguidos de motivo de cine; a MUNDO tras `to_world` fotogramas seguidos de motivo de mundo.
 *   - Se incluye el motivo (VRV_R_*) para el registro y para que la interfaz distinga "el mundo se está pidiendo" de "cine por cinemática". */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { VRV_CINEMA = 0, VRV_WORLD = 1 } VrViewMode;

enum {
    VRV_R_WORLD = 0,       /* mundo: partida, con control y cámara VR */
    VRV_R_NO_GAME,         /* sin partida: título, selección, cargas, vídeos de arranque, cambio de zona */
    VRV_R_CINEMATIC,       /* escena del motor (demo / control retirado) */
    VRV_R_COMMS,           /* comunicador (códec) abierto */
    VRV_R_STOPPED,         /* juego detenido: película, pantalla de resultado, ... */
    VRV_R_PAUSED,          /* pausa */
    VRV_R_MENU,            /* ventana de armas / objetos del juego (el motor dibuja con su cámara mientras está abierta) */
    VRV_R_LOST_CONTROL,    /* sin control sin otra causa conocida (fin de partida, transiciones) */
    VRV_R_NO_CAM           /* con control pero la cámara VR no está (aún) activa o no existe */
};

typedef struct {
    uint8_t game;          /* puente completo + jugador en escena */
    uint8_t in_control;
    uint8_t cam_active;    /* la cámara del motor es la de la cabeza del usuario */
    uint8_t cinematic, comms, paused, menu_open, stopped;
} VrViewIn;

typedef struct {
    int mode;              /* VrViewMode vigente (con histéresis) */
    int reason;            /* motivo vigente (VRV_R_*) del último fotograma (sin histéresis) */
    int want;              /* modo pedido por el último fotograma */
    int run;               /* fotogramas seguidos pidiendo `want` distinto de `mode` */
    int to_cine, to_world; /* umbrales de histéresis, en fotogramas */
    uint32_t switches;
} VrView;

void vrview_reset(VrView *v);
/* Motivo para unas entradas dadas (función pura, sin histéresis). */
int vrview_reason(const VrViewIn *in);
/* Un fotograma. Devuelve el modo vigente. */
int vrview_step(VrView *v, const VrViewIn *in);
const char *vrview_reason_name(int reason);

#ifdef __cplusplus
}
#endif
