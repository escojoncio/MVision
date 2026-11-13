/* Gestos de cuerpo (C puro, sin dependencias; testeable en Linux).
 *
 * La capa VR decide con el cuerpo y NO simula botones de ataque: sus decisiones (golpe, agarre, apretar) las ejecuta el motor
 * directamente (vr/input/vr_melee). Controles (tabla final en la bitacora):
 *  - R1 (mano dominante) / L1 (la otra) son los botones del PUÑO: puño cerrado mientras se mantienen. No llegan al juego.
 *    El grip analógico solo da la pose de los dedos.
 *  - GOLPE = puño cerrado (botón mantenido) + golpe físico rápido hacia delante (velocidad relativa a la cabeza) -> punch_side.
 *  - AGARRE = PULSAR R1/L1 (flanco) con la mano abierta a <= 0,30 m del cuello o la espalda de un enemigo -> grab_start.
 *    Con el puño ya cerrado (botón mantenido de antes) solo se agarra si la mano entra DESPACIO en la zona por la espalda del enemigo; rápida es un puño. Mantener el botón mantiene el agarre (grab_hold);
 *    cada pulsación nueva o un golpe con el puño estando agarrado = squeeze (apretar y rematar).
 *  - El cuadrado físico y los demás botones pasan al juego como en el original (respaldo de golpe).
 *  - Pared: golpear con el PUÑO cerrado una superficie sólida aproximadamente vertical = toque de nudillos del motor (sonido + ruido que oyen los guardias) -> knock_side;
 *    de pie (no hace falta estar pegado a la pared) o pegado a ella; nunca agachado, tumbado, en la caja ni con un arma o una arrojadiza en la mano. Asomarse = triángulo (opcional).
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
enum { VRG_L2 = 1u << 8, VRG_R2 = 1u << 9, VRG_SQUARE = 1u << 15, VRG_CIRCLE = 1u << 13, VRG_TRIANGLE = 1u << 12, VRG_R1 = 1u << 11, VRG_L1 = 1u << 10 };
enum { VRG_EV_NONE = 0, VRG_EV_PUNCH = 1, VRG_EV_GRAB = 2, VRG_EV_SQUEEZE = 3, VRG_EV_RELEASE = 4, VRG_EV_KNOCK = 5, VRG_EV_PEEK = 6, VRG_EV_WALL = 7, VRG_EV_HURT = 8,
       VRG_EV_IMPACT = 9, VRG_EV_MENU = 10, VRG_EV_THROW = 11 };

typedef struct {
    int enable;              /* 0: el módulo no hace nada (modo botón) */
    int fist_required;       /* 1 (defecto): el golpe solo cuenta con la mano en PUÑO (agarre cerrado con histéresis grip_on/off); 0 = como antes (el puño es indiferente) */
    float fist_haptic;       /* háptico suave al cerrar el puño con las manos libres (0,12); 0 = ninguno */
    float item_hold_s;       /* con manos libres los grips NO llegan al juego (solo cierran el puño): mantener el clic del stick de cada mano este tiempo (0,35 s) emite R1 (dominante) / L1 (la otra) = ventana de objetos / armas */
    float punch_dir_cos;     /* el golpe exige que la velocidad RELATIVA A LA CABEZA vaya al menos este coseno hacia delante Y radialmente hacia fuera de la cabeza (0,5 = dentro de 60 grados): un gesto lateral o girar el cuerpo no valen */
    float trig_on, trig_off; /* histéresis del gatillo (0,6 / 0,3) */
    int punch, grab, knock, peek;   /* conmutadores por gesto (1 activo) */
    int right_handed;        /* el agarre de la mano dominante es R1 (derecho) o L1 */
    float punch_speed;       /* m/s de la mano para contar como golpe (1,3: la media de 4 muestras recorta el pico real de 2-4 m/s) */
    float knock_speed;       /* velocidad mínima al tocar la superficie (0,4 m/s): la de acercamiento a ella Y la de la mano respecto a la cabeza (andar hacia una pared con la mano quieta no cuenta) */
    float knock_touch;       /* distancia mano-superficie que cuenta como tocar (0,08 m) */
    float knock_touch_wall, knock_speed_wall;   /* lo mismo estando pegado a la pared (on_wall): la mano virtual va junto a Snake y el toque se hace a ojo (0,12 m, 0,2 m/s) */
    float knock_ny_max;      /* solo superficies aproximadamente verticales: |componente vertical de la normal| máxima (0,5 = hasta 30 grados de inclinación); el suelo y las tapas no cuentan */
    float knock_cooldown_s;  /* entre toques (0,25 s) */
    float knock_enemy_m;     /* con un enemigo a menos de esto de la mano no es pared sino golpe (1,0 m) */
    float reach_min;         /* alcance horizontal mínimo de la mano respecto a la cabeza en el golpe (0,30 m) */
    float front_cos;         /* el golpe exige un enemigo a tiro DELANTE: coseno mínimo entre el rumbo del jugador y la dirección al enemigo (0,5 = 60 grados) */
    float melee_range;       /* distancia máxima al enemigo para golpear (3,0 m); 0 = sin condición */
    float grab_range;        /* distancia máxima cuerpo-enemigo para agarrar (1,3 m) */
    float neck_radius;       /* radio de la mano al cuello o a la espalda del enemigo para agarrar (0,30 m) */
    float back_dist;         /* la espalda: punto a esta distancia (0,25 m) detrás del enemigo, a 0,2 m de altura */
    float chest_side, chest_down, chest_fwd, chest_radius, chest_exit;   /* zonas de menú (m), UNA POR MANO, fijas respecto a la cabeza en el marco FÍSICO (metros reales, delante = head_fwd, hrel de la mano; sin escala del motor ni rumbo del personaje): la mano izquierda (objetos, L2) a su lado izquierdo y la derecha (armas, R2) al derecho; lateral 0,16 hacia cada lado, 0,26 bajo la cabeza, delante -0,02, radio 0,15 para entrar y chest_exit 0,20 para seguir dentro (histéresis), medidas en el registro del visor con las manos donde el usuario pulsó */
    float chest_hold_s;      /* gatillo de la mano (L2 izquierda, R2 derecha) mantenido sin interrupción dentro de SU zona este tiempo (0,30 s) para abrir la ventana; salir de la zona o soltar lo reinicia. Dentro de la zona el gatillo no llega al juego (mask) */
    float grab_after_punch_s;/* sin agarres tras un golpe durante este tiempo (0,3 s) */
    float neck_height;       /* altura del cuello sobre la posición del enemigo (0,45 m, posición del motor = cintura) */
    float grip_on, grip_off; /* histéresis del agarre (0,65 / 0,35) */
    float peek_on, peek_off; /* inclinación lateral de la cabeza para asomarse (0,12 / 0,06 m) */
    float cooldown_s;        /* entre golpes (0,2) */
    float contact_speed;     /* golpe por CONTACTO: velocidad mínima (relativa a la cabeza) del puño que atraviesa el volumen del enemigo, en cualquier dirección salvo hacia la cabeza (0,8 m/s) */
    float body_radius, body_low, body_high;   /* cápsula del cuerpo del enemigo sobre su posición del motor (cintura): radio 0,28, 0,5 por debajo, 0,65 por encima */
    float throw_speed;       /* lanzamiento: tirón de la mano con el botón pulsado junto al cuerpo del enemigo, >= esta velocidad (1,2 m/s) en grab_delay_s, no hacia el enemigo */
    float throw_reach;       /* distancia máxima de la mano al cuerpo (cápsula) del enemigo para poder lanzar: hombro, cuello o brazo (0,30 m más el radio del cuerpo) */
    float grab_slow_speed;   /* agarre solo si al pulsar (y durante grab_delay_s) la mano va más lenta que esto respecto a la cabeza (0,5 m/s); más rápido es un PUÑO */
    float grab_delay_s;      /* el agarre no llega al motor hasta pasado este margen (0,3 s) sin que la mano se haya lanzado: así un golpe inmediato lo cancela */
    float contact_latch_s;   /* un contacto del puño con el cuerpo sigue valiendo este tiempo (0,15 s): el puño puede cerrarse o acabar el enfriamiento justo después */
    float fist_radius, contact_tol;           /* esfera del puño (0,12) y tolerancia extra (0,05) */
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
    int wall_valid[2];       /* hay consulta de pared para la mano (geometría estática de la escena) */
    float wall_dist[2];      /* distancia de la mano a la superficie más cercana de la geometría estática (m) */
    float wall_ny[2];        /* |componente vertical de la normal| de esa superficie junto a la mano (0 = pared vertical, 1 = suelo/techo) */
    int tracked[2];          /* 0 = izquierda física, 1 = derecha física */
    double hand_time[2];     /* instante (s, reloj monótono) de la pose de cada mano; <= 0: sin marca (se usa dt por llamada). Repetir la marca = misma muestra */
    int throw_hold;          /* arrojadiza VR (granada, aturdidora, chaff) en la mano del arma, sin preparar o preparada: su gatillo la prepara/lanza y NO abre la ventana de armas del pecho */
    int item_scope;          /* prismáticos equipados (objeto IT_Scope, sin arma): en el original ○ es el zoom y no hay golpe, agarre ni toque; aquí tampoco (las ventanas por clic de stick siguen) */
    int weapon_busy;         /* arma VR mostrada o maniobra de recarga en curso (el juego desequipa el arma un instante al recargar) */
    float hand[2][3], grip[2];   /* mano en el mundo (cercanía a enemigos); grip = agarre (puño), NO decide el agarre del enemigo */
    float trig[2];           /* gatillo analógico por mano física */
    int stick_dom, stick_off; /* clic del stick de la mano dominante / de la otra (R3 / L3 del mando del juego) */
    int fist_btn[2];         /* botón del puño pulsado por mano física (R1 de la dominante / L1 de la otra, ya mapeados): ESTO decide puño y agarre; el grip analógico solo es la pose */
    int cap_valid;           /* el anfitrión aporta el contacto del puño con las cápsulas por pieza de los enemigos (vr/gameplay/vr_hit): sustituye al cilindro body_radius */
    int cap_zone[2];         /* con cap_valid: zona que toca el puño de cada mano en este VBlank (0 ninguna, 1 cabeza, 2 cuerpo, 3 extremidad) */
    int n_en;
    struct { float p[3]; float yaw; int down; int vital; int blocked; } en[VRG_MAX_EN];   /* blocked: hay geometría estática entre la cabeza y el enemigo (no se puede golpear a través de una pared) */
} vrg_in_t;

/* Intento de golpe (una mano con el puño o un movimiento casi rápido): para el registro [gest] del visor. reason es una cadena estática. */
typedef struct {
    int valid, side;
    float speed, fcos, rcos, reach, dist, grip;   /* m/s, coseno hacia delante, coseno radial, alcance (m), distancia al enemigo más cercano (-1 ninguno), agarre */
    int fist;
    int contact;             /* el puño atraviesa el volumen del cuerpo de un enemigo (golpe por contacto) */
    const char *reason;      /* "detras" (enemigo a tiro pero no delante), "ok", "lento", "dir", "alcance", "sin_enemigo", "lejos", "sin_puno", "enfriamiento", "calma", "ocupado", "manos_ocupadas" */
} vrg_gest_t;

typedef struct {
    uint16_t press;          /* botones a añadir al mando del juego */
    uint16_t mask;           /* botones del usuario a retirar (el grip mientras agarra: no debe abrir menús) */
    float haptic[2];         /* 0..1 por mano física (este VBlank) */
    int event;               /* VRG_EV_* de este VBlank */
    int punch_side;          /* -1 nada; 0/1: lanzar el golpe del motor (mano física que lo dio) */
    int grab_start;          /* 1: lanzar el agarre del motor este VBlank (mano grab_side, enemigo grab_en) */
    int grab_hold;           /* 1: el botón del puño sigue pulsado durante el agarre (mantiene el agarre del motor) */
    int chest_in[2];         /* la mano física está dentro de SU zona de menú (con histéresis chest_radius/chest_exit) en este VBlank; 0 si el módulo no actúa */
    int menu_open[2];        /* ventana abierta por la mano física (0 objetos/L2, 1 armas/R2) mientras se mantiene el gatillo; vrg_out.press ya lleva L2/R2 */
    int knock_side;          /* -1 nada; 0/1: llamar con los nudillos (acción del motor) con esa mano */
    const char *knock_why;   /* motivo del último intento de toque de este VBlank ("ok", "lento", "mano_quieta", "sin_puno", "postura", "no_vertical", "enfriamiento", "enemigo", "lejos_de_la_pared"...) */
    float knock_speed, knock_dist; int knock_try_side;
    int squeeze;             /* 1: apretar/rematar (nueva pulsación o golpe estando agarrado) */
    const char *grab_why;    /* motivo del último intento de agarre de este VBlank (NULL: ninguno): "ok", "mano_lejos", "cuerpo_lejos", "puno_cerrado", "tras_golpe"... */
    float grab_dist;         /* distancia de la mano al cuello/espalda en ese intento (m) */
    int grab_side_try;
    struct { int valid, opened; float dist, hold; const char *reason; float hf[3]; int rearm; } pecho[2];   /* intento de ventana por mano física (gatillo apretado): distancia a SU zona (m), tiempo mantenido (s), resultado y motivo (cadena estática) para el registro [pecho]; hf = mano respecto a la cabeza en metros reales (derecha, arriba, delante); rearm = 1: apertura bloqueada hasta soltar el gatillo */
    int grab_side;           /* mano física que sujeta (-1 ninguna) mientras el agarre dura (para pegar las manos al cuello) */
    int throw_side;          /* -1 nada; 0/1: lanzamiento (agarre + tirón) con esa mano este VBlank (grab_start también vale 1) */
    float throw_yaw;         /* rumbo del tirón proyectado en el suelo (rad, delante = (sin, cos)) */
    int grab_en;             /* índice del enemigo sujetado (en `en`) o -1 */
    vrg_gest_t gest;         /* intento de golpe de este VBlank (valid = 0: ninguno) */
    int swinging;            /* 1: algún puño cerrado se mueve a velocidad de golpe (un golpe en marcha): la animación del combo del motor espera la próxima petición */
} vrg_out_t;

#define VRG_HOLD_LOST_MAX 3
typedef struct {
    vrg_cfg_t cfg;
    /* interno */
    float pos[2][4][3]; int npos[2];
    float cool;
    float since_punch;       /* s desde el último golpe */
    int near_prev[2];
    int menu_hold, chest_prev[2], chest_live[2];   /* chest_live: intento en curso (gatillo apretado dentro de la zona) */
    int fb[2];               /* bloqueo de disparo por mano física (VRG_FB_*): lo calcula vrg_step_pad; vrg_fire_gate lo aplica al gatillo */
    float fb_last[2];        /* último gatillo físico visto con la mano libre de bloqueo (el que sigue viendo el arma mientras dura) */
    int chest_lock[2];       /* tras abrir la ventana no hay otra apertura hasta soltar el gatillo (con seguimiento): perder y recuperar el seguimiento con el gatillo apretado no es una pulsación */
    int hold_lost[2];        /* VBlanks seguidos sin seguimiento de la mano con la ventana del pecho abierta (vrg_step_pad tolera VRG_HOLD_LOST_MAX antes de soltarla) */
    int trig_dn[2];          /* gatillo de cada mano apretado (histéresis trig_on/trig_off) */
    float chest_cool[2], chest_t[2];
    int peek_on;
    float knock_cool[2], cool_knock; double kn_t[2][3]; float kn_d[2][3]; int kn_n[2], kn_toward[2];
    float knock_log[2];      /* s hasta el próximo registro de "cerca de la pared sin toque" por mano */
    int grab_side;           /* -1 ninguno; mano que sujeta */
    int fist;                /* bits: mano física con el puño cerrado (histéresis del agarre) */
    int grab_en;
    float impact_t; int impact_side;      /* s restantes para el háptico de impacto del último golpe y mano que lo dio */
    int vit_prev[VRG_MAX_EN], vit_n;      /* vida de los enemigos en el VBlank anterior (un descenso = impacto) */
    int prev_life, have_life;
    float peek_ref, peek_ax[2]; int peek_ref_ok, prev_wall;
    double tim[2][4]; double clock_s;   /* marcas de las últimas muestras y reloj interno de reserva */
    float settle;            /* s sin golpes tras un salto de pose (recentrado, pérdida de seguimiento) */
    float weapon_hold;       /* s sin manos libres tras haber tenido arma (recarga, cambio de arma) */
    int warm;                /* llamadas tras un reinicio sin iniciar agarres (el grip ya cerrado no es un flanco) */
    float hurt_cool, wall_cool;
    float gest_tokens;       /* cupo de registros [gest] (10 por segundo) */
    int swing[2];            /* movimiento casi rápido en curso por mano (histéresis del registro) */
    float fist_hap_cool[2];  /* s hasta el próximo háptico de cierre de puño por mano (evita el temblor con el grip en el umbral) */
    float item_t[2];         /* s con el clic de stick mantenido (dominante, otra) */
    vrg_gest_t gpk[2];       /* mejor registro del movimiento en curso por mano (se emite al terminar) */
    float prev_head[3], prev_fwd[2]; int have_head;
    float prev_hand[2][3]; int prev_hand_ok[2];   /* mano (mundo) del VBlank anterior: barrido continuo contra la cápsula del enemigo */
    float contact_t[2];      /* s restantes del contacto recordado por mano */
    int disarm[2];           /* tras un golpe por contacto no vale otro hasta que el puño frene (un mismo movimiento no pega dos veces) */
    float gp_t[2]; int gp_en[2], gp_only[2];   /* gp_only: pulsado junto al cuerpo (no al cuello): solo vale para lanzar */   /* agarre pendiente (s hasta que llegue al motor) y enemigo */
} vrg_t;

void vrg_cfg_defaults(vrg_cfg_t *c);
void vrg_init(vrg_t *g);
void vrg_reset(vrg_t *g);                     /* al cambiar de zona / cargar / perder el control */
void vrg_update(vrg_t *g, const vrg_in_t *in, vrg_out_t *out);

/* Mando que ve el juego en un VBlank (lo que vrbodyg hace alrededor de vrg_update). Entradas que no dependen de la capa: manos y gatillo FÍSICOS, también con el jugador sin el control. */
typedef struct {
    int ctl;                 /* capa de JUEGO VR (jugador al mando); 0 = capa de MENÚ (códec, radio, pausa, ventana de armas/objetos): mando original */
    int menu_open;           /* el juego tiene abierta una ventana (selector de armas/objetos) */
    uint16_t pad;            /* mando del usuario con R2 -> cuadrado ya convertido (capa de juego) */
    uint16_t pad_in;         /* mando del usuario tal cual (capa de menú) */
    int r2_fire;             /* el cuadrado de este VBlank viene del gatillo, no del botón físico */
    int tracked[2]; float trig[2];   /* mano física rastreada y gatillo analógico */
} vrg_pad_t;
/* vrg_update + mando final. La ventana abierta con el gesto del pecho se sostiene con el GATILLO de la mano (histéresis trig_off), NO con el mando entrante ni con la capa:
 * L2 (la otra mano) / R2 (la dominante) va pulsado de forma continua en todos los VBlanks mientras el gatillo siga apretado (cambie la capa o no) y se suelta al soltarlo. */
uint16_t vrg_step_pad(vrg_t *g, const vrg_in_t *in, vrg_out_t *out, const vrg_pad_t *p);

/* Bloqueo de disparo en la zona de menú. Desde que la mano entra en SU zona (mismo umbral que el háptico de aviso: chest_radius para entrar, chest_exit > chest_radius para salir)
 * y mientras tenga la ventana abierta, su gatillo NO es del arma (ni dispara, ni prepara, ni lanza, ni detona); al salir de la zona con el gatillo aún apretado sigue bloqueado hasta soltarlo
 * (trig <= trig_off): hace falta una pulsación nueva. vrg_step_pad lo actualiza una vez por VBlank. */
enum { VRG_FB_NONE = 0, VRG_FB_ZONE = 1, VRG_FB_WINDOW = 2, VRG_FB_RELEASE = 3 };
const char *vrg_fire_block_name(int why);          /* "zona", "ventana", "soltar_gatillo"; "ninguno" con VRG_FB_NONE */
/* Gatillo que debe ver el arma de la mano física `side`: el real, o con bloqueo el último visto antes de empezar (el arma no ve flancos, ni sube ni baja: así una granada preparada no se lanza ni
 * una carga armada se coloca al entrar en la zona). *why = VRG_FB_* (0 libre). */
float vrg_fire_gate(const vrg_t *g, int side, float raw, int *why);

#ifdef __cplusplus
}
#endif
#endif
