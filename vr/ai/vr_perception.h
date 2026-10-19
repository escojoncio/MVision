/* vr_perception: percepción enemiga para VR (soldados con IA). Sustituye, mediante parches de función por
 * dirección (rt_patch), la prueba de visión de los soldados de las overlays de zona, ampliando el alcance con
 * sospecha GRADUAL por distancia, y añade el aviso de sospecha ("?") del propio juego.
 *
 *  - SOLO soldados/enemigos con IA. Las cámaras de vigilancia y demás sensores fijos NO pasan por aquí: su
 *    función de detección es otra (ver vr_cameras.h) y no se parchea; el localizador exige la firma completa
 *    de la prueba de visión de soldados y rechaza cualquier otra forma de función.
 *  - Estrategia "envoltorio": la lógica original (cono, línea de visión, caja, oscuridad/camuflaje, humo,
 *    mapa, altura...) se ejecuta TAL CUAL con el alcance temporalmente ampliado (dos escrituras en RAM que se
 *    restauran antes de volver: efecto neto = el de la original). Después se reinterpreta el resultado:
 *       resultado original del juego, por soldado:  0 no ve · 1 ve pero fuera de alcance · 2 ve (alerta crece)
 *    - d <= alcance original L: sin cambios (alerta inmediata original).
 *    - L < d <= Lmax = L*range_mult: un acumulador por soldado crece con el tiempo a la vista (más despacio
 *      cuanto más lejos) y decae al ocultarse. Al cruzar t_suspect se pide la reacción de sospecha del juego
 *      (bit 1 del campo de eventos del soldado: se vuelve y mira al jugador con la "?"); al cruzar t_alert
 *      el resultado pasa a 2 y el juego sigue con su alerta normal ("!", ataque...).
 *    - Periferia del cono (más allá de periph_frac del semiángulo) y d > periph_near*L: solo cuenta si el
 *      jugador se mueve.
 *  El acumulador vive en una tabla propia indexada por la dirección del actor; no se escribe en RAM del juego
 *  nada que la función original no escribiría (campo de resultado) salvo el bit de sospecha en la función de
 *  recogida de eventos, que es la que lo produce en el juego.
 * C99, sin dependencias del runtime (la unión con el runtime está en vr_perception_rt.c). */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* ---- Ajuste ---- */
typedef enum { VRP_ORIGINAL = 0, VRP_VR = 1, VRP_DEMANDING = 2 } VrpPreset;

typedef struct VrpConfig {
    int   enabled;          /* 0: pasa directo a la original (sin tocar nada) */
    float range_mult;       /* alcance máximo / alcance original (>= 1) */
    float rate_pow;         /* caída del ritmo con la distancia: ((Lmax-d)/(Lmax-L))^pow */
    float t_suspect;        /* s de exposición equivalente para la sospecha ("?") */
    float t_alert;          /* s de exposición equivalente para la alerta (resultado 2) */
    float decay;            /* s de acumulador que se pierden por s fuera de la vista */
    float periph_frac;      /* fracción del semiángulo desde la que es periferia (>=1: sin regla) */
    float periph_near;      /* la regla de periferia solo rige para d > periph_near * L */
    float periph_move_rate; /* multiplicador de ritmo en periferia si el jugador se mueve */
    float move_mult;        /* ritmo si el jugador se mueve (andando/corriendo) */
    float crouch_mult;      /* agachado */
    float prone_mult;       /* tumbado */
    float q_cooldown;       /* s mínimos entre dos sospechas del mismo soldado */
    float dt_max;           /* tope del paso de tiempo por evaluación (pausas, cargas) */
    float range_scale;      /* factor del menú «IA: alcance de visión» (1,0 / 1,5 / 2,0): escala el alcance de visión del soldado (el que usa la rutina del
                             * motor para su alerta inmediata) y, con él, la zona de sospecha (range_mult sobre el alcance ya escalado). 0 se trata como 1 */
    int   head_see;         /* 1: la cabeza VR también cuenta como punto visible (se evalúa con la rutina del motor y manda lo más desfavorable) */
    float head_dz;          /* zona muerta del desplazamiento de la cabeza respecto del ancla (unidades del motor): por debajo, la cabeza no se evalúa */
    float head_max;         /* tope del desplazamiento considerado (unidades del motor) */
} VrpConfig;

void vrp_config_preset(VrpConfig *c, VrpPreset p);

/* ---- Modelo (puro, probado con datos sintéticos) ---- */
typedef struct VrpEntry {
    uint32_t actor;         /* dirección del actor (0 = libre) */
    float acc;              /* acumulador (s equivalentes) */
    float last_t;           /* instante (host) de la última evaluación */
    float next_q;           /* instante antes del cual no se vuelve a pedir "?" */
    uint8_t armed;          /* la sospecha puede dispararse (se rearma cuando acc baja de t_suspect/2) */
    uint8_t q_pending;      /* hay una sospecha por entregar a la función de eventos */
    uint8_t last_out;       /* último resultado emitido (0/1/2) */
    uint8_t seen_far;       /* última evaluación: visto en la zona ampliada */
    uint16_t last_dist_q;   /* última distancia (unidades del motor, saturada) */
    uint8_t via;            /* última evaluación que lo vio (resultado 2): 1 cuerpo, 2 cabeza VR, 0 no */
    float seen_t;           /* instante (host) de la última evaluación con resultado 2 */
} VrpEntry;

typedef struct VrpInput {
    int   raw;              /* resultado de la original con alcance ampliado: 0, 1 o 2 */
    float dist;             /* distancia al jugador (unidades del motor) */
    float range;            /* alcance original L (> 0) */
    float off_frac;         /* |ángulo respecto al eje| / semiángulo del cono (0 = centro, 1 = borde) */
    int   moving;           /* el jugador se mueve */
    int   stance;           /* 0 de pie, 1 agachado, 2 tumbado */
    float now;              /* reloj del host en s */
} VrpInput;

/* Ritmo relativo de acumulación a distancia d (1 en L, 0 en Lmax; 0 fuera). */
float vrp_rate(const VrpConfig *c, float d, float L);
/* Un paso de evaluación. Devuelve el resultado a escribir (0/1/2) y actualiza la entrada. */
int vrp_step(const VrpConfig *c, VrpEntry *e, const VrpInput *in);

/* ---- Localización en una overlay ---- */
typedef struct VrpLayout {   /* offsets en el actor del soldado (bytes) */
    const char *id;
    uint16_t o_facedir, o_angle, o_len, o_flag, o_sndis, o_sndir;
} VrpLayout;

typedef struct VrpSites {
    int      found;          /* 1 = prueba de visión localizada y validada */
    uint32_t vision;         /* dirección de la prueba de visión (a0 = actor) */
    const VrpLayout *lay;
    uint32_t eye_global;     /* dirección del alcance global de visión (0 si la variante no lo usa) */
    uint32_t gather;         /* función de recogida de eventos del soldado (0 si no se halló) */
    uint16_t o_events;       /* offset del campo de eventos en el actor */
    uint32_t mode_addr;      /* dirección del modo global del mando de enemigos (0 si no se halló) */
    uint32_t code_lo, code_hi;
} VrpSites;

/* Busca en la ventana de código [lo,hi) de la RAM. Devuelve 1 si localiza la prueba de visión de soldados. */
int vrp_locate(const uint8_t *ram, uint32_t lo, uint32_t hi, VrpSites *out);

/* Direcciones de funciones del EXE principal (edición EU) que forman la firma. */
#define VRP_FN_SUB3    0x80016E5Cu  /* resta de vectores */
#define VRP_FN_DIR     0x80017014u  /* rumbo de un vector */
#define VRP_FN_LEN     0x80016E9Cu  /* longitud 3D */
#define VRP_FN_ADIFF   0x80017190u  /* diferencia absoluta de ángulos */
#define VRP_FN_HAZARD  0x80028D14u  /* línea de visión contra la geometría */
#define VRP_FN_TGTCHK  0x8002ECCCu  /* línea de visión contra blancos (cajas, puertas...) */
/* Variables del EXE principal */
#define VRP_A_PLAYER_STATUS 0x800ACBE0u
#define VRP_PS_MOVE   0x10u
#define VRP_PS_SQUAT  0x20u
#define VRP_PS_GROUND 0x40u
#define VRP_A_PLAYER_POS 0x800ACBA0u   /* posición del personaje que usa la visión (3 x s16: x, y, z; y arriba) */

/* ---- Contexto (tabla de acumuladores + enlace con la RAM) ---- */
typedef struct VrpHost {
    void *user;
    /* Ejecuta el código ORIGINAL de la función `addr` con a0 = arg0 (sin parche). */
    void (*call_orig)(void *user, uint32_t addr, uint32_t arg0);
    /* Opcional. Se llama con enter=1 justo antes de ampliar variables del juego y con enter=0 justo después de
     * restaurarlas: el host debe impedir en medio interrupciones/cambios de hilo (nadie más ve el valor ampliado). */
    void (*guard)(void *user, int enter);
} VrpHost;

#define VRP_MAX_ENTRIES 128
typedef struct VrpStats {
    uint32_t calls, turns, extended_hits, suspects, alerts_far;
    uint32_t head_evals;     /* evaluaciones extra con la cabeza VR */
    uint32_t seen_body, seen_head;   /* evaluaciones con resultado 2: por el cuerpo / solo por la cabeza VR */
} VrpStats;

typedef struct VrpCtx {
    VrpConfig cfg;
    uint8_t *ram;
    VrpHost host;
    VrpSites sites;
    float now;
    int external_motion;     /* el host marca movimiento extra del jugador (cabeza/manos en VR) */
    int head_user;           /* interruptor del usuario para la cabeza VR (defecto 1; se combina con cfg.head_see) */
    int head_valid;          /* hay un desplazamiento de cabeza vigente */
    float head_off[3];       /* desplazamiento mundo (unidades del motor) de la cabeza VR respecto del ojo del personaje */
    float head_t;            /* instante (host) en que se fijó */
    float log_t;             /* instante del último registro [ia] */
    int log_active;          /* el último registro tenía soldados que veían */
    VrpEntry tab[VRP_MAX_ENTRIES];
    VrpStats st;
} VrpCtx;

void vrp_init(VrpCtx *x, uint8_t *ram, const VrpHost *host, VrpPreset p);
void vrp_set_preset(VrpCtx *x, VrpPreset p);
void vrp_set_time(VrpCtx *x, float seconds);          /* una vez por fotograma, desde el gancho de VBlank */
void vrp_set_external_motion(VrpCtx *x, int moving);
void vrp_reset(VrpCtx *x);
/* Cabeza VR: desplazamiento mundo (unidades del motor) de la cabeza respecto del ancla (el ojo del personaje); NULL = sin cabeza válida.
 * Lo calcula el host con vrp_head_offset(). Caduca a los 0,25 s sin renovarse. */
void vrp_set_head(VrpCtx *x, const float off[3]);
void vrp_set_head_enabled(VrpCtx *x, int on);
/* W - ojo = upm * R^T (P + corr - p_ref), con R el giro de la correspondencia usuario<->mundo (vr_headcol/vr_usermap). P = cabeza física (m,
 * espacio del usuario), corr = recorte contra el escenario (m) o NULL. */
void vrp_head_offset(float theta_play_deg, float upm, const float p_ref[3], const float head[3], const float corr[3], float out[3]);
/* Punto que evalúa la rutina de visión con la cabeza: posición del personaje + desplazamiento (con zona muerta y tope); 0 si por debajo de la zona muerta. */
int vrp_head_point(const VrpConfig *c, const int16_t body[3], const float off[3], int16_t out[3]);
/* Registro [ia] como máximo una vez por segundo de host: escribe la línea en buf y devuelve 1 si toca emitirla (algún soldado lo ve ahora o
 * lo veía en el último registro). Cuenta soldados con visión (resultado 2) en el último segundo, por cuerpo y por cabeza VR. */
int vrp_log_tick(VrpCtx *x, char *buf, size_t n);                            /* al cambiar de overlay/zona */
VrpEntry *vrp_entry(VrpCtx *x, uint32_t actor, int create);

/* Cuerpos de los parches (a0 = actor). `vrp_vision` sustituye a la prueba de visión; `vrp_gather` a la recogida
 * de eventos. Requieren x->sites rellenado por vrp_locate. */
void vrp_vision(VrpCtx *x, uint32_t actor);
void vrp_gather(VrpCtx *x, uint32_t actor);
