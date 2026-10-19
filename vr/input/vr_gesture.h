/* Gestos de cuerpo -> entrada del juego (C puro, sin dependencias; testeable en Linux).
 *
 * La capa VR no combate ni decide nada: traduce gestos del usuario a los botones que el juego ya espera (medido en el juego real, ver
 * bitacora):
 *  - Golpe: cuadrado (toque) sin arma equipada; el juego encadena su combo (puñetazos y patada) con toques repetidos.
 *  - Agarre / estrangulamiento: cuadrado MANTENIDO cerca de un enemigo (agarra, tira de él y lo estrangula); cada nuevo toque de cuadrado
 *    mientras sujeta aprieta el cuello (baja el contador de desmayo del enemigo); soltar = el enemigo cae. Moverse con el stick mientras
 *    sujeta arrastra al enemigo. En el gesto el agarre de la mano (grip) cerrado == cuadrado pulsado, así que mantener = mantener y
 *    apretar/aflojar repetido = toques.
 *  - Pegarse a la pared: lo hace el juego al empujar el stick contra ella (nada que traducir, funciona sentado); asomarse = triángulo
 *    (primera persona asomándose) cuando la cabeza se inclina hacia un lado estando pegado (opcional).
 *  - Golpear la pared: círculo estando pegado, al hacer un golpe rápido de la mano.
 *  - Hápticos: golpe, agarre, apretón, pared, daño recibido.
 * Espacio de entrada: MUNDO del motor en metros (x, y arriba, z), rumbo del jugador yaw en radianes con delante = (sin yaw, cos yaw).
 * Una llamada por VBlank (dt ~ 0,02). */
#ifndef VR_GESTURE_H
#define VR_GESTURE_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VRG_MAX_EN 8
enum { VRG_SQUARE = 1u << 15, VRG_CIRCLE = 1u << 13, VRG_TRIANGLE = 1u << 12, VRG_R1 = 1u << 11, VRG_L1 = 1u << 10 };
enum { VRG_EV_NONE = 0, VRG_EV_PUNCH = 1, VRG_EV_GRAB = 2, VRG_EV_SQUEEZE = 3, VRG_EV_RELEASE = 4, VRG_EV_KNOCK = 5, VRG_EV_PEEK = 6, VRG_EV_WALL = 7, VRG_EV_HURT = 8 };

typedef struct {
    int enable;              /* 0: el módulo no hace nada (modo botón) */
    int punch, grab, knock, peek;   /* conmutadores por gesto (1 activo) */
    int right_handed;        /* el agarre de la mano dominante es R1 (derecho) o L1 */
    float punch_speed;       /* m/s de la mano para contar como golpe (1,8) */
    float knock_speed;       /* m/s para golpear la pared (1,2) */
    float reach_min;         /* alcance horizontal mínimo de la mano respecto a la cabeza en el golpe (0,22 m) */
    float melee_range;       /* distancia máxima al enemigo para golpear (2,5 m); 0 = sin condición */
    float grab_range;        /* distancia máxima cuerpo-enemigo para agarrar (1,3 m) */
    float neck_radius;       /* radio de la mano al cuello/hombro del enemigo (0,45 m) */
    float neck_height;       /* altura del cuello sobre la posición del enemigo (0,45 m, posición del motor = cintura) */
    float grip_on, grip_off; /* histéresis del agarre (0,65 / 0,35) */
    float peek_on, peek_off; /* inclinación lateral de la cabeza para asomarse (0,12 / 0,06 m) */
    float cooldown_s;        /* entre golpes (0,3) */
} vrg_cfg_t;

typedef struct {
    float dt;
    int active;              /* jugador al mando (sin menús, escenas ni pausa) y correspondencia mundo válida */
    int weapon;              /* -1 sin arma: solo entonces hay golpes y agarres */
    int stance;              /* 0 de pie, 1 agachado, 2 tumbado */
    int on_wall, in_box, in_water;
    int holding;             /* el juego está sujetando a un enemigo (acción 7/38/124) */
    int life;                /* vida actual (para el daño recibido) */
    float body[3], yaw;      /* jugador */
    float view_yaw;          /* hacia dónde mira el usuario (rumbo de la cámara VR): define "delante" del golpe y el lado de asomarse */
    float head[3];           /* cabeza del usuario en el mundo */
    int tracked[2];          /* 0 = izquierda física, 1 = derecha física */
    float hand[2][3], grip[2];
    int n_en;
    struct { float p[3]; float yaw; int down; } en[VRG_MAX_EN];
} vrg_in_t;

typedef struct {
    uint16_t press;          /* botones a añadir al mando del juego */
    uint16_t mask;           /* botones del usuario a retirar (el grip mientras agarra: no debe abrir menús) */
    float haptic[2];         /* 0..1 por mano física (este VBlank) */
    int event;               /* VRG_EV_* de este VBlank */
} vrg_out_t;

typedef struct {
    vrg_cfg_t cfg;
    /* interno */
    float pos[2][4][3]; int npos[2];
    float cool;
    int punch_left;          /* VBlanks restantes de cuadrado por golpe */
    int knock_left, peek_on;
    int grab_side;           /* -1 ninguno; mano que sujeta */
    int sq_hold, sq_age;     /* cuadrado por agarre y VBlanks en su estado actual */
    int prev_grip_closed;
    int prev_life, have_life;
    float peek_ref[2]; int peek_ref_ok, prev_wall;
    float sq_prev_out;
} vrg_t;

void vrg_cfg_defaults(vrg_cfg_t *c);
void vrg_init(vrg_t *g);
void vrg_reset(vrg_t *g);                     /* al cambiar de zona / cargar / perder el control */
void vrg_update(vrg_t *g, const vrg_in_t *in, vrg_out_t *out);

#ifdef __cplusplus
}
#endif
#endif
