/* vr_melee: golpe y agarre del cuerpo VR ejecutados DIRECTAMENTE por la lógica del motor (C puro sobre la RAM del invitado;
 * testeable con RAM falsa). Sin botones sintéticos: se sustituye el punto donde el motor decide la acción del jugador a partir
 * de la entrada y se arranca el estado de la acción (la misma rutina que lanzaría el botón), y la comprobación de impacto y la
 * búsqueda de presa usan como origen el puño VR real.
 *
 * Puntos (direcciones neutras de la edición europea; vr_melee_rt.c los engancha con rt_patch_fn):
 *   0x800513D8  decisión de acción del jugador por la entrada (se llama cada fotograma en reposo/marcha). Gancho: si hay petición
 *               de golpe, arranca el estado de golpe (0x80059CD0) con la bandera de estado 4; si hay petición de agarre, arranca
 *               el estado de presa (0x80059D44). Sin petición: el original (el botón sigue funcionando).
 *   0x80051538  llamar a la pared: con petición de toque, entra directamente en el estado de nudillos (bandera 0x100 + rutina 0x800551C0),
 *               que es lo que hace el original con el botón junto a una pared (solo en los estados pegado a la pared, tiempo >= 8).
 *   0x80059D44  estado de presa (tiempo 0 = búsqueda de presa en una caja). 0x80051108 es la búsqueda; llama a 0x8002DF1C (mover
 *               el blanco del jugador): ahí se sustituye el centro de la caja por la mano VR.
 *   0x80050FD4  comprobación de impacto del golpe (origen + alcance); también termina en 0x8002DF1C: centro = puño VR.
 *   0x8005A2A4  rutina por fotograma del golpe: una petición nueva de golpe durante la animación marca la continuación del combo
 *               (bit 0x20 del acumulado de entrada del estado, que el motor mira en los tiempos 6 y 13: golpe, golpe, patada).
 *   0x8005A5C8  rutina por fotograma de la presa: mantener (puño pulsado), apretar (pulsación nueva) y soltar, con la misma
 *               lógica que el original pero con las decisiones del cuerpo VR; el cuadrado físico sigue valiendo.
 *   0x8005A6D4  rutina del apretón: pulsaciones nuevas durante el apretón cuentan igual que en el original (contador de 10).
 *   0x8005A100  arrastre: mientras el puño VR siga pulsado el motor ve la presa mantenida.
 * Desplazamientos del actor del jugador medidos en volcados de RAM (ver test_vr_melee.c y la bitácora). */
#ifndef VR_MELEE_H
#define VR_MELEE_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VRM_RAM_SIZE 0x200000u
/* funciones del motor */
#define VRM_FN_ACTION_IN  0x800513D8u
#define VRM_FN_KNOCK_CK   0x80051538u   /* comprobación de «llamar a la pared» en los estados pegado a la pared (actor, tiempo) */
#define VRM_ST_KNOCK      0x800551C0u   /* estado de llamar con los nudillos */
#define VRM_FN_START      0x8004F618u   /* arrancar estado: (actor, rutina) */
#define VRM_FN_PUNCH      0x80059CD0u   /* estado de golpe */
#define VRM_FN_GRAB       0x80059D44u   /* estado de presa */
#define VRM_FN_HIT        0x80050FD4u   /* comprobación de impacto (actor, golpe*, modo) */
#define VRM_FN_GRAB_SCAN  0x80051108u   /* búsqueda de presa */
#define VRM_FN_MOVE_TGT   0x8002DF1Cu   /* mover blanco (blanco, centro*) */
#define VRM_FN_PUNCH_TICK 0x8005A2A4u
#define VRM_FN_HOLD_TICK  0x8005A5C8u
#define VRM_FN_SQZ_TICK   0x8005A6D4u
#define VRM_FN_DRAG       0x8005A100u
#define VRM_FN_HOLD_END   0x8004F7F0u   /* (actor, 2) */
#define VRM_ST_HOLD       0x8005A1F8u   /* estado: sujetando sin apretar (final) */
#define VRM_ST_SQUEEZE    0x80059FB4u   /* estado: apretón */
#define VRM_ST_KILL       0x8005A04Cu   /* estado: remate */
#define VRM_ST_DRAG       0x8005A100u
#define VRM_G_MOVEDIR     0x800ACD34u   /* s32 dirección de marcha pedida (-1 ninguna) */
#define VRM_G_PSTATUS     0x800ACBE0u   /* u32 banderas de estado del jugador */
#define VRM_TARGETS       0x800B7660u   /* 64 blancos de 72 B */
#define VRM_PLAYER_CTRL   0x800ACB84u
/* actor del jugador */
#define VRM_W_F910      2316
#define VRM_W_FLAGS     2196
#define VRM_W_TARGET    2204    /* blanco propio (72 B): centro +8, caja +16 */
#define VRM_W_PTARGET   2276    /* presa */
#define VRM_W_FRAMES    2304    /* fotogramas desde el último apretón */
#define VRM_W_WPN       2328
#define VRM_W_KNOCK_FN  2492
#define VRM_W_KNOCK_T   2496
#define VRM_W_TBL       2332
#define VRM_W_PAD       2476    /* puntero a la entrada: u16 estado +0, u16 pulsado +2 */
#define VRM_W_ACTPACK   2480
#define VRM_W_ANIMFN    2484
#define VRM_W_ANIMT     2488
#define VRM_W_UPD       2500
#define VRM_W_LOCAL     2612
#define VRM_W_SQZCNT    2640
#define VRM_PAD_ACT     0x80    /* bit de la acción de agarre/estrangular en la entrada del motor (solo para el respaldo físico) */

typedef struct VrmEng {
    uint8_t *ram;                                     /* RAM del invitado (2 MiB) */
    void *user;
    void (*call)(void *user, uint32_t fn, uint32_t a0, uint32_t a1);   /* ejecuta una función del motor */
} VrmEng;

typedef struct {
    int punch_side;           /* -1 nada; 0/1: petición de golpe */
    int grab_start, grab_side;/* petición de agarre y mano que lo pide */
    int knock_side;           /* -1 nada; 0/1: toque en la pared */
    int hold;                 /* el botón del puño sigue pulsado */
    int squeeze;              /* apretar/rematar */
    int fist_ok[2];           /* puño con seguimiento */
    int fist_closed[2];       /* botón del puño pulsado */
    float fist[2][3];         /* puño VR en el mundo del motor (mm) */
} VrmFrame;

#define VRM_HIST 24             /* ~0,4 s de muestras del puño: el estado de golpe del motor comprueba el impacto varios fotogramas después del gesto */
#define VRM_FIST_BOX_MM 180     /* semitamaño mínimo (mm) de la caja del jugador mientras dura la comprobación de impacto: puño ~120 mm + tolerancia */
#define VRM_SNAP_MM 450         /* si el puño pasa a menos de esto (mm) de la caja del enemigo, el reintento centra el origen en su cuerpo */
typedef struct Vrm {
    int enable;
    int ttl_frames;           /* vida de una petición (VBlanks, 8) */
    void (*log)(const char *line);
    /* peticiones */
    int p_punch, p_punch_side, p_punch_ttl;
    int p_grab, p_grab_side, p_grab_ttl;
    int p_knock, p_knock_side, p_knock_ttl;
    int hold, squeeze_pend;
    int fist_ok[2], fist_closed[2];
    float fist[2][3];
    float hist[2][VRM_HIST][3]; unsigned char hist_cl[2][VRM_HIST]; int hist_n[2], hist_i[2];
    /* contexto de los ganchos */
    int ctx_hit, ctx_grab, ctx_ok; uint32_t ctx_target; float ctx_origin[3], ctx_fist[3];
    int stage;                /* etapa del golpe en curso (1..3) */
    int retried;              /* la comprobación de impacto en curso es el reintento centrado en el cuerpo */
    int16_t box_save[3]; int box_saved;   /* caja original del blanco del jugador (se ensancha mientras dura la comprobación) */
    uint32_t box_work;
    int grab_owned;           /* la presa en curso la inició el cuerpo VR */
    int in_punch;             /* el estado de golpe está en curso por una petición VR */
    /* contadores */
    unsigned n_punch, n_combo, n_hit, n_miss, n_retry, n_retry_ok, n_nobody, n_grab, n_grab_rejected, n_squeeze, n_expired, n_knock;
} Vrm;

void vrm_init(Vrm *v);
void vrm_reset(Vrm *v);
/* una vez por VBlank, antes de que corra el fotograma del juego: caduca peticiones sin atender (registra el motivo) y anota las nuevas */
void vrm_frame(Vrm *v, const VrmEng *e, const VrmFrame *f);

/* ganchos (cada uno devuelve 1 si ha resuelto la llamada y no hay que ejecutar el original) */
int  vrm_knock_check(Vrm *v, const VrmEng *e, uint32_t work, uint32_t time);       /* 0x80051538: 1 = resuelto (no ejecutar el original) */
int  vrm_action_input(Vrm *v, const VrmEng *e, uint32_t work);                  /* 0x800513D8: v0 = 1 si arrancó una acción */
void vrm_pre_grab(Vrm *v, const VrmEng *e, uint32_t work, uint32_t time);       /* 0x80059D44 antes del original */
void vrm_post_grab(Vrm *v, const VrmEng *e, uint32_t work, uint32_t time);
void vrm_pre_hit(Vrm *v, const VrmEng *e, uint32_t work);                       /* 0x80050FD4 */
int  vrm_post_hit(Vrm *v, const VrmEng *e, uint32_t work, uint32_t result);   /* 1 = repetir la llamada original (reintento centrado en el cuerpo); el contexto ya está preparado */
int  vrm_move_target(Vrm *v, const VrmEng *e, uint32_t target, uint32_t vec);   /* 0x8002DF1C: 1 si cambió el centro */
void vrm_punch_tick(Vrm *v, const VrmEng *e, uint32_t work, uint32_t time);     /* 0x8005A2A4 antes del original */
int  vrm_hold_tick(Vrm *v, const VrmEng *e, uint32_t work);                     /* 0x8005A5C8: 1 = resuelto */
int  vrm_squeeze_tick(Vrm *v, const VrmEng *e, uint32_t work, uint32_t time);   /* 0x8005A6D4: 1 = remate (no ejecutar el resto) */
void vrm_pre_drag(Vrm *v, const VrmEng *e, uint32_t work, uint16_t *saved);     /* 0x8005A100 */
void vrm_post_drag(Vrm *v, const VrmEng *e, uint32_t work, uint16_t saved);

#ifdef __cplusplus
}
#endif
#endif
