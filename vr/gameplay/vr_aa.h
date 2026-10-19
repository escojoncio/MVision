/* vr_aa: apuntado del lanzador antiaéreo (arma 4) con el buscador del PROPIO juego.
 *
 * Idea. El juego ya resuelve el fijado de blanco: mientras el jugador mantiene el botón de ataque con ese arma
 * (primera persona con mira), un actor "buscador" proyecta cada blanco enemigo del reparto de blancos con la matriz
 * mundo->vista del canal 3D principal, elige el más cercano al jugador cuyo centro cae en un cono estrecho alrededor
 * del eje de la vista, lo sigue (puntero persistente leído en vivo por el misil) y dibuja su marco de fijación en el
 * HUD. Aquí NO se reimplementa nada de eso: solo se sustituye la ORIENTACIÓN de la vista con la que el buscador
 * hace sus cuentas (HLE de vr_fire_hle.c: durante el paso del buscador se escribe en el canal 3D la matriz del visor
 * del lanzador VR; al terminar se restaura la del juego y se recarga la GTE). El misil sale por la boca del arma VR
 * (vr_fire) y persigue el puntero de blanco que dejó el buscador, igual que en el juego.
 *
 * Gesto. Para que el buscador exista hay que estar en el estado "apuntando" del juego (botón de ataque mantenido).
 * vraa_gesture() decide cuándo el jugador "mira por el visor" (lanzador llevado al ojo, con histéresis); mientras
 * dure, el anfitrión pide a vr_fire el modo mantener (VrFireIn.raise/.ready), de modo que el juego levanta el arma,
 * crea la mira y el buscador. El gatillo VR suelta el botón (flanco de disparo de vr_fire) y el juego lanza el misil.
 * Sin gesto (arma a la cadera) el gatillo dispara como antes (disparo directo, el juego no fija blanco).
 *
 * Unidades/ejes: las de vr_fire.h (usuario en metros; mundo del motor 1 unidad = 1 mm). Todo se llama desde el hilo
 * del emulador. No es reentrante.
 */
#pragma once
#include <stdint.h>
#include "vr_fire.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- gesto del visor ---- */
typedef struct VrAaCfg {
    float enter_m;       /* distancia ojo-visor para entrar (m) (defecto 0,20) */
    float leave_m;       /* distancia para salir (m) (defecto 0,30) */
    float axis_deg;      /* ángulo máximo entre el eje del tubo y la mirada para entrar (defecto 40) */
    float sight_back_m;  /* el ojo apoya a esta distancia por detrás del punto del visor, sobre el eje (defecto 0,10) */
    int   on_vbl;        /* VBlanks seguidos cumpliendo la condición antes de entrar (defecto 3) */
    float h_scale;       /* H original del juego / H del motor con la cámara VR (= 320 / engine_h; 1 sin cámara VR): el visor
                          * usa la H del juego para que su campo y la ventana del buscador sean los del original */
} VrAaCfg;
void vraa_cfg_default(VrAaCfg *c);
void vraa_init(const VrAaCfg *c);
const VrAaCfg *vraa_config(void);

/* Una vez por VBlank. head: pose de la cabeza (espacio de usuario); sight: pose del punto del visor del lanzador
 * (WP_EYE en mundo de usuario; delante = -Z local). equipped: el lanzador VR está empuñado y puede apuntar.
 * Devuelve 1 si el jugador mira por el visor (con histéresis). */
int vraa_gesture(const WPose *head, const WPose *sight, int equipped);
int vraa_aiming(void);

/* ---- vista del visor para el buscador del juego ---- */
/* Fija la vista de este VBlank: sight en espacio de usuario + correspondencia usuario->motor. on = 0 la anula. */
void vraa_set_scope(int on, const VrFireMap *map, const WPose *sight);
/* Matriz mundo->vista (la que carga la GTE) del visor: m = filas ejes X (derecha), Y (abajo), Z (delante) de la vista en
 * mundo del motor (1.12), t = -R * ojo (unidades). Devuelve 0 si no hay vista válida. */
int vraa_view_matrix(const VrFireMap *map, const WPose *sight, VrFireMat *out);
/* H (distancia de pantalla) con la que el buscador proyecta: la del motor ahora * h_scale. */
int vraa_view_focal(int engine_focal);
void vraa_note_h(float h);   /* el HLE anota la H usada (para vraa_sight) */
/* Para el sustituto HLE: matriz a poner en el canal 3D durante el paso del buscador (0 = dejar la del juego). */
int vraa_take_view(VrFireMat *out);

/* ---- estado del buscador del juego (solo lectura de la RAM tras cada paso) ---- */
typedef struct VrAaLock {
    int      alive;       /* el buscador se ha ejecutado en los últimos VBlanks */
    uint32_t actor;       /* dirección (invitado) del actor del buscador */
    uint32_t target;      /* blanco seguido ahora (0 = ninguno); es lo que lee el misil */
    int      frames;      /* fotogramas seguidos con ese mismo blanco */
    int      lost;        /* fotogramas seguidos sin candidato desde que lo perdió (el juego lo suelta a los 24) */
    int      candidates;  /* candidatos dentro de pantalla en el último paso */
    int      converged;   /* el marco ya terminó de cerrarse (>= LOCK_BOX_FRAMES fotogramas) */
    int      steps;       /* pasos del buscador desde vraa_init (para comprobar que el HLE corre) */
    int      swapped;     /* pasos hechos con la vista del visor VR */
} VrAaLock;
enum { VRAA_BOX_FRAMES = 12,      /* el marco de fijación del juego tarda 12 fotogramas en cerrarse */
       VRAA_LOSE_FRAMES = 24 };   /* sin candidato, el juego suelta el blanco a los 24 fotogramas */
void vraa_lock_info(VrAaLock *out);
/* Los sustitutos HLE llaman a esto tras cada paso del buscador. */
void vraa_after_step(const uint8_t *ram, uint32_t actor, int swapped);
/* El anfitrión llama una vez por VBlank (antes de enviar el mando): marca "vivo" y caduca si el buscador dejó de correr. */
void vraa_tick(void);

/* Puntero de blanco persistente del juego (el que lee el misil). */
uint32_t vraa_game_target(const uint8_t *ram);

/* ---- presentación del visor (contrato con la app) ---- */
/* El HUD del juego (mira y marco de fijación) lo dibuja el propio juego en el panel del HUD, en coordenadas de pantalla del
 * juego (320 x 256, centro en 160,128). Con el visor activo la app puede colocar ese panel como visor óptico del lanzador:
 * vraa_sight_anchor() da su pose (WP_EYE del arma, cara hacia el ojo) y el tamaño angular que debe cubrir. */
typedef struct VrAaSight {
    int   active;          /* el visor está activo (gesto + buscador vivo) */
    WPose pose;            /* pose del visor en el espacio de usuario (delante = -Z local = hacia el blanco) */
    float half_fov_deg;    /* semiángulo horizontal que cubre el panel del HUD completo (el de la proyección del juego) */
    int   locked;          /* hay blanco seguido */
    int   converged;       /* el marco ya está cerrado */
} VrAaSight;
void vraa_sight(VrAaSight *out);

#ifdef __cplusplus
}
#endif
