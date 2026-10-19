/* Gestos de cuerpo -> entrada del juego (C puro, sin dependencias; testeable en Linux).
 *
 * La capa VR no combate ni decide nada: traduce gestos del usuario a los botones que el juego ya espera (medido en el juego real, ver
 * bitacora):
 *  - GOLPEAR y AGARRAR son dos cosas separadas (con las manos libres):
 *    GOLPEAR = gesto físico de golpe (mano rápida hacia delante, ~2-4 m/s durante 0,2-0,4 s) con la mano en PUÑO (agarre/grip cerrado con histéresis; el
 *    movimiento rápido con la mano abierta no golpea): cuadrado (toque) -> el juego da su golpe del combo; la mano del personaje sigue tu puño (IK) y el háptico llega al impactar. NUNCA agarra.
 *    AGARRAR = llevar la mano al cuello del enemigo + apretar el GATILLO: cuadrado MANTENIDO (agarra, tira de él y lo estrangula); cada nuevo
 *    apretón del gatillo mientras sujeta aprieta el cuello (baja el contador de desmayo); soltar = el enemigo cae. Moverse con el stick sujetando
 *    arrastra al enemigo. Con gestos activos y manos libres el cuadrado físico del mando (gatillo dominante / A) NO llega al juego tal cual:
 *    cerca del cuello es agarre, lejos es un golpe de botón (toque de 4 VBlanks + golpe sintético de la mano del personaje, para accesibilidad).
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
enum { VRG_R3 = 1u << 2, VRG_L3 = 1u << 1 };   /* clic del stick (bits del mando del juego, mismos que vr_input) */
enum { VRG_SQUARE = 1u << 15, VRG_CIRCLE = 1u << 13, VRG_TRIANGLE = 1u << 12, VRG_R1 = 1u << 11, VRG_L1 = 1u << 10 };
enum { VRG_EV_NONE = 0, VRG_EV_PUNCH = 1, VRG_EV_GRAB = 2, VRG_EV_SQUEEZE = 3, VRG_EV_RELEASE = 4, VRG_EV_KNOCK = 5, VRG_EV_PEEK = 6, VRG_EV_WALL = 7, VRG_EV_HURT = 8,
       VRG_EV_IMPACT = 9, VRG_EV_JAB = 10 };

typedef struct {
    int enable;              /* 0: el módulo no hace nada (modo botón) */
    int fist_required;       /* 1 (defecto): el golpe solo cuenta con la mano en PUÑO (agarre cerrado con histéresis grip_on/off); 0 = como antes (el puño es indiferente) */
    float fist_haptic;       /* háptico suave al cerrar el puño con las manos libres (0,12); 0 = ninguno */
    int button_jab;          /* 0 (defecto): con manos libres el cuadrado físico (gatillo) fuera de un agarre se DESCARTA (el gatillo solo agarra); 1 = golpe de botón de accesibilidad (toque + golpe sintético de la mano) */
    float item_hold_s;       /* con manos libres los grips NO llegan al juego (solo cierran el puño): mantener el clic del stick de cada mano este tiempo (0,35 s) emite R1 (dominante) / L1 (la otra) = ventana de objetos / armas */
    float punch_dir_cos;     /* el golpe exige que la velocidad vaya al menos este coseno hacia delante de la cabeza O radialmente hacia fuera de ella (0,5 = dentro de 60 grados) */
    float trig_on, trig_off; /* histéresis del gatillo (0,6 / 0,3) */
    int punch, grab, knock, peek;   /* conmutadores por gesto (1 activo) */
    int right_handed;        /* el agarre de la mano dominante es R1 (derecho) o L1 */
    float punch_speed;       /* m/s de la mano para contar como golpe (1,3: la media de 4 muestras recorta el pico real de 2-4 m/s) */
    float knock_speed;       /* m/s para golpear la pared (1,2) */
    float reach_min;         /* alcance horizontal mínimo de la mano respecto a la cabeza en el golpe (0,15 m) */
    float melee_range;       /* distancia máxima al enemigo para golpear (3,0 m); 0 = sin condición */
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
    float body[3], yaw;      /* jugador (mundo) */
    float view_yaw;          /* (informativo) rumbo de la cámara VR */
    /* Cantidades FÍSICAS del usuario (espacio de seguimiento, m): no cambian al andar con el stick ni al girar por pasos, así que la velocidad
     * de la mano no se contamina con la locomoción del personaje. head_fwd = delante de la cabeza en ese espacio (x, z, unitario). */
    float hand_u[2][3];      /* mano en el espacio de seguimiento (de aquí sale la velocidad) */
    float hrel[2][3];        /* mano - cabeza (alcance y dirección radial) */
    float head_fwd[2];
    float head_u[3];         /* cabeza (para asomarse: inclinación lateral respecto a la entrada en la pared) */
    float head[3];           /* cabeza del usuario en el mundo */
    int tracked[2];          /* 0 = izquierda física, 1 = derecha física */
    double hand_time[2];     /* instante (s, reloj monótono) de la pose de cada mano; <= 0: sin marca (se usa dt por llamada). Repetir la marca = misma muestra */
    int weapon_busy;         /* arma VR mostrada o maniobra de recarga en curso (el juego desequipa el arma un instante al recargar) */
    float hand[2][3], grip[2];   /* mano en el mundo (cercanía a enemigos); grip = agarre (puño), NO decide el agarre del enemigo */
    float trig[2];           /* gatillo analógico por mano física */
    int stick_dom, stick_off; /* clic del stick de la mano dominante / de la otra (R3 / L3 del mando del juego) */
    int sq_user;             /* el cuadrado físico del mando está pulsado (gatillo dominante o botón A de la otra mano) */
    int n_en;
    struct { float p[3]; float yaw; int down; int vital; } en[VRG_MAX_EN];
} vrg_in_t;

/* Intento de golpe (una mano con el puño o un movimiento casi rápido): para el registro [gest] del visor. reason es una cadena estática. */
typedef struct {
    int valid, side;
    float speed, fcos, rcos, reach, dist, grip;   /* m/s, coseno hacia delante, coseno radial, alcance (m), distancia al enemigo más cercano (-1 ninguno), agarre */
    int fist;
    const char *reason;      /* "ok", "lento", "dir", "alcance", "sin_enemigo", "lejos", "sin_puno", "enfriamiento", "calma", "ocupado", "manos_ocupadas" */
} vrg_gest_t;

typedef struct {
    uint16_t press;          /* botones a añadir al mando del juego */
    uint16_t mask;           /* botones del usuario a retirar (el grip mientras agarra: no debe abrir menús) */
    float haptic[2];         /* 0..1 por mano física (este VBlank) */
    int event;               /* VRG_EV_* de este VBlank */
    int jab_side;            /* -1 nada; 0/1: iniciar un golpe sintético de esa mano del personaje (botón sin gesto) */
    int grab_side;           /* mano física que sujeta (-1 ninguna) mientras el agarre dura (para pegar las manos al cuello) */
    int grab_en;             /* índice del enemigo sujetado (en `en`) o -1 */
    vrg_gest_t gest;         /* intento de golpe de este VBlank (valid = 0: ninguno) */
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
    int fist;                /* bits: mano física con el puño cerrado (histéresis del agarre) */
    int prev_grip_closed;    /* ahora: gatillo cerrado (con histéresis) */
    int grab_en;
    float fist_mask;         /* s con R1/L1 retirados tras un golpe (el puño cerrado no debe abrir el menú de objetos) */
    float impact_t; int impact_side;      /* s restantes para el háptico de impacto del último golpe y mano que lo dio */
    int vit_prev[VRG_MAX_EN], vit_n;      /* vida de los enemigos en el VBlank anterior (un descenso = impacto) */
    int prev_life, have_life;
    float peek_ref, peek_ax[2]; int peek_ref_ok, prev_wall;
    double tim[2][4]; double clock_s;   /* marcas de las últimas muestras y reloj interno de reserva */
    float settle;            /* s sin golpes tras un salto de pose (recentrado, pérdida de seguimiento) */
    float weapon_hold;       /* s sin manos libres tras haber tenido arma (recarga, cambio de arma) */
    int warm;                /* llamadas tras un reinicio sin iniciar agarres (el grip ya cerrado no es un flanco) */
    int latch; float latch_t;/* R1/L1 retirados hasta soltar el grip tras un agarre */
    float hurt_cool, wall_cool;
    float fist_hap_cool[2];  /* s hasta el próximo háptico de cierre de puño por mano (evita el temblor con el grip en el umbral) */
    float item_t[2];         /* s con el clic de stick mantenido (dominante, otra) */
    vrg_gest_t gpk[2];       /* mejor registro del movimiento en curso por mano (se emite al terminar) */
    float prev_head[3], prev_fwd[2]; int have_head;
} vrg_t;

void vrg_cfg_defaults(vrg_cfg_t *c);
void vrg_init(vrg_t *g);
void vrg_reset(vrg_t *g);                     /* al cambiar de zona / cargar / perder el control */
void vrg_update(vrg_t *g, const vrg_in_t *in, vrg_out_t *out);

#ifdef __cplusplus
}
#endif
#endif
