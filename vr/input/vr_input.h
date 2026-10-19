/* Entrada VR -> mando original + locomoción + confort. C puro, sin dependencias (testeable en Linux).
 *
 * Convenciones:
 *  - Espacio VR: Y arriba, mano derecha, "adelante" por defecto = -Z. Cuaterniones como float[4] = x,y,z,w
 *    (mismo orden que simd_quatf.vector). Unidades en metros / radianes.
 *  - Rumbo (heading) h: dirección (x,z) = (sin h, -cos h). h=0 mira a -Z, h>0 gira hacia +X (a la derecha).
 *  - Sticks de entrada: -1..1, Y positivo = arriba/adelante. Salida original: 0..255, centro 0x80, Y positivo = abajo.
 *  - Botones original activo-alto (bits de bitacora §14).
 *
 * Uso por fotograma: rellenar vr_input_frame_t, llamar vr_input_update() y enviar vr_pad_t a mv_host_pad().
 * La capa de cámara usa vr_input_body_yaw() (se SUMA al rumbo de la cabeza) y vr_input_vignette() el renderer. */
#ifndef VR_INPUT_H
#define VR_INPUT_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bits del mando original (activo-alto). */
enum {
    VRI_PAD_SELECT = 1u << 0, VRI_PAD_L3 = 1u << 1, VRI_PAD_R3 = 1u << 2, VRI_PAD_START = 1u << 3,
    VRI_PAD_UP = 1u << 4, VRI_PAD_RIGHT = 1u << 5, VRI_PAD_DOWN = 1u << 6, VRI_PAD_LEFT = 1u << 7,
    VRI_PAD_L2 = 1u << 8, VRI_PAD_R2 = 1u << 9, VRI_PAD_L1 = 1u << 10, VRI_PAD_R1 = 1u << 11,
    VRI_PAD_TRIANGLE = 1u << 12, VRI_PAD_CIRCLE = 1u << 13, VRI_PAD_CROSS = 1u << 14, VRI_PAD_SQUARE = 1u << 15
};

/* Botones digitales de un mando Sense (los analógicos van aparte). */
enum { VRI_H_LOW = 1u << 0,    /* cara inferior (A en el derecho, X en el izquierdo) */
       VRI_H_UP = 1u << 1,     /* cara superior (B / Y) */
       VRI_H_STICK = 1u << 2,  /* pulsación del stick */
       VRI_H_MENU = 1u << 3 }; /* Options (derecho) / Create (izquierdo) */

typedef enum { VRI_MODE_SENSE = 0, VRI_MODE_PAD = 1 } vri_mode_t;
typedef enum { VRI_LOCO_HEAD = 0, VRI_LOCO_HAND = 1 } vri_loco_t;
typedef enum { VRI_TURN_SNAP = 0, VRI_TURN_SMOOTH = 1, VRI_TURN_OFF = 2 } vri_turn_t;
typedef enum { VRI_STAND = 0, VRI_CROUCH = 1, VRI_PRONE = 2 } vri_posture_t;
/* Postura física (altura de la cabeza). El botón del juego (cruz) SIEMPRE funciona, con cualquier modo: esto es un añadido.
 *  OFF  (defecto): la capa nunca pulsa el botón de postura; la postura deseada sigue a la del juego.
 *  DOWN: agacharse/tumbarse físicamente baja la postura del juego (un toque por escalón) pero levantarse nunca la sube, y
 *        nunca se actúa si el juego ya está igual de bajo o más (el botón manual manda).
 *  FULL: como antes, sigue también las subidas (de pie físico = de pie en el juego). */
typedef enum { VRI_POSTURE_OFF = 0, VRI_POSTURE_DOWN = 1, VRI_POSTURE_FULL = 2 } vri_posture_mode_t;

typedef struct {
    bool tracked;          /* pose válida */
    float pos[3];
    float quat[4];         /* x,y,z,w */
    float stick_x, stick_y;/* -1..1, Y arriba */
    float trigger, grip;   /* 0..1 */
    uint32_t buttons;      /* VRI_H_* */
} vri_hand_t;

typedef struct {
    float dt;                       /* segundos desde el fotograma anterior */
    bool head_valid;
    float head_pos[3];
    float head_quat[4];
    vri_hand_t hand[2];             /* 0 = izquierdo, 1 = derecho (físicos) */
    /* Modo PAD (DualSense / extendido): ya mapeado a bits original como hoy, sticks -1..1 con Y arriba. */
    uint16_t pad_buttons;
    float pad_lx, pad_ly, pad_rx, pad_ry;
    /* Retroalimentación del juego (puente RAM); -1 = desconocida. */
    int game_posture;               /* 0 de pie, 1 agachado, 2 tumbado */
    bool menu;                      /* pausa / menú / comunicador: sin locomoción ni agacharse, stick -> cruceta */
} vr_input_frame_t;

typedef struct {
    uint16_t buttons;
    uint8_t lx, ly, rx, ry;         /* 0..255, centro 0x80 */
} vr_pad_t;

/* Mapa de botones del modo Sense (máscara de bits original; 0 = sin acción). "dom" = mano dominante. */
typedef struct {
    uint16_t trig_dom, trig_off, grip_dom, grip_off;
    uint16_t low_dom, up_dom, low_off, up_off;
    uint16_t stick_dom, stick_off, menu_dom, menu_off;
    /* Acorde de la mano no dominante: agarre + gatillo apretados a la vez (con el agarre aún no emitido) = esta máscara
     * (R2 por defecto); no emite ni L1 ni L2. 0 = sin acorde. Solo en juego (no en menús). */
    uint16_t chord_off;
    /* En menús (jugador fuera de control: título, menús, comunicador, escenas, pausa) los botones de las dos manos pasan por
     * estas máscaras en vez de las del juego: gatillo y cara baja = confirmar (+ avanzar texto/saltar escena), cara alta = cancelar. */
    uint16_t menu_trig, menu_low, menu_up;
} vri_map_t;

typedef struct {
    vri_mode_t mode;
    int dominant_hand;       /* 1 derecha (defecto), 0 izquierda: espeja roles (stick de mover/girar y botones) */
    vri_loco_t loco_ref;     /* locomoción relativa a cabeza o a la mano del stick de mover */
    vri_turn_t turn_mode;
    float snap_deg;          /* 30 o 45 (cualquier valor 5..180) */
    float smooth_dps;        /* velocidad de giro suave a tope, grados/s */
    float vignette;          /* intensidad máxima 0..1 (0 = desactivada) */
    float stick_deadzone;    /* radial, 0..0.9 */
    float walk_scale;        /* 0.1..1: tope del módulo del stick de mover */
    bool pass_right_stick;   /* modo PAD: no usar el stick derecho para girar, pasarlo al juego */
    bool allow_prone;
    int posture_mode;        /* vri_posture_mode_t */
    float crouch_drop_m;     /* caída de cabeza para agacharse (~0.35) */
    float prone_drop_m;      /* caída para tumbarse (~0.80) */
    float hysteresis_m;      /* 0.06 */
    uint16_t crouch_btn;     /* botón del juego que agacha/tumba (toques) */
    float grip_delay_s;      /* mano no dominante con acorde: el agarre solo emite L1 tras este tiempo (0 = inmediato) */
    float grip_tap_s;        /* un agarre más corto que grip_delay_s emite un toque de esta duración al soltar */
    vri_map_t map;
} vri_cfg_t;

typedef struct {
    vri_cfg_t cfg;
    /* estado interno (no tocar) */
    float body_yaw;
    float calib_h;
    float vig, snap_pulse, omega, lin;
    int turn_armed, turn_dir;
    uint32_t snap_count;
    int desired, prev_desired;
    int crouch_phase, retries, taps_left;
    float crouch_timer;
    bool need_calib, crouch_pending, hold_trig[2], hold_grip[2];
    float game_up;
    bool menu_dpad[4];
    float grip_t[2], grip_tap[2];
    bool grip_prev[2], grip_out[2], grip_chord[2];
    vr_pad_t last;
} vr_input_t;

void vr_input_cfg_defaults(vri_cfg_t *cfg);
void vr_input_init(vr_input_t *s);                       /* configuración por defecto */
void vr_input_set_cfg(vr_input_t *s, const vri_cfg_t *cfg); /* sanea valores fuera de rango */
void vr_input_update(vr_input_t *s, const vr_input_frame_t *f, vr_pad_t *out);
/* Sin estado, para la ventana (sin juego conocido): botones de los dos Sense con el mapa de MENÚS y los agarres sin retardo;
 * el stick de cada mano (el de mayor desvío) pasa a cruceta. Misma tabla que vr_input_update en modo Sense con menu=true. */
uint16_t vr_input_sense_plain(const vri_cfg_t *cfg, const vri_hand_t hand[2]);

/* Cámara: yaw del "cuerpo" en radianes (-pi..pi], se suma al rumbo de la cabeza. Gira alrededor de la cabeza
 * (la capa de cámara debe compensar el desplazamiento XZ para que la cabeza no se mueva al girar). */
float vr_input_body_yaw(const vr_input_t *s);
void vr_input_reset_body_yaw(vr_input_t *s, float yaw);  /* p. ej. al cambiar de sala, o alinear con el juego */
uint32_t vr_input_snap_count(const vr_input_t *s);       /* incrementa en cada giro por pasos (para fundido breve) */
float vr_input_vignette(const vr_input_t *s);            /* 0..1 para el renderer */

/* Agacharse físico. */
void vr_input_recalibrate(vr_input_t *s);                /* toma la altura de cabeza del próximo fotograma como "de pie" */
void vr_input_set_height(vr_input_t *s, float head_y_m); /* altura calibrada explícita (ajuste persistido) */
float vr_input_height(const vr_input_t *s);
int vr_input_posture(const vr_input_t *s);               /* postura deseada por la cabeza: vri_posture_t */

/* Rumbo del mundo (mismo espacio que cabeza+cuerpo) al que corresponde "stick arriba" del juego. */
void vr_input_set_game_up(vr_input_t *s, float heading_rad);

/* Utilidades públicas (útiles a la capa de cámara y a los tests). */
float vr_input_quat_heading(const float q[4]);           /* rumbo horizontal del vector adelante (-Z local) */
uint8_t vr_input_axis_u8(float v);                       /* -1..1 -> 0..255, 0 -> 0x80 */

#ifdef __cplusplus
}
#endif
#endif
