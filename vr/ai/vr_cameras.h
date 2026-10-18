/* vr_cameras: PUNTO DE ENTRADA reservado para el comportamiento VR de las cámaras de vigilancia y demás
 * sensores fijos (cámaras con arma, ...). Las cámaras NO usan el modelo de vr_perception (acumulador por
 * distancia, periferia por movimiento): tienen su propia función de detección y su propia lógica (barrido,
 * cono, alarma) y hoy se dejan intactas (VRC_ORIGINAL, sin parches).
 *
 * Esta unidad solo LOCALIZA sus funciones de detección en una overlay y lee sus parámetros originales, para
 * que el ajuste VR de las cámaras (original / VR) se defina aparte sin tocar vr_perception.
 *
 * Lo que hace la detección original de una cámara (por familia, ver `VrcFamily`):
 *  - sale sin ver si el jugador lleva el objeto de camuflaje o no comparte mapa con la cámara;
 *  - ángulo: |rumbo al jugador - rumbo de la cámara| y lo mismo en cabeceo, ambos <= semiángulo `half`;
 *  - alcance: distancia 3D al jugador <= `range` (sin ampliar nada por debajo);
 *  - línea de visión contra la geometría (si el flag de la instancia lo pide);
 *  - dentro de la caja de cartón y quieto: solo "ve" si cambió de posición/orientación respecto al fotograma
 *    anterior (movimiento), nunca si se queda inmóvil;
 *  - escribe en el actor un indicador "detectado" y, en la lógica de la cámara, eso dispara la alarma.
 * `range` y `half` los fija el script de cada instancia (no son constantes del código).
 * C99, sin dependencias del runtime. */
#pragma once
#include <stdint.h>

typedef enum { VRC_ORIGINAL = 0, VRC_VR = 1 } VrcMode;   /* reservado: VRC_VR aún sin comportamiento */

typedef enum {
    VRC_FAM_CAMERA = 0,   /* cámara de vigilancia: range u16 @+638, half u16 @+640, detectado s32 @+476 */
    VRC_FAM_GUN = 1,      /* cámara/torreta con arma: range s32 @+868, half s32 @+872 */
    VRC_FAM_SIMPLE = 2    /* sensor sencillo: range u16 @+842, half u16 @+844 */
} VrcFamily;

typedef struct VrcSite {
    VrcFamily family;
    uint32_t detect;        /* dirección de la función de detección (a0 = actor) */
    uint16_t o_range, o_half;
    uint8_t range_is_32;    /* 1 si range/half son s32, 0 si u16 */
    uint16_t o_detected;    /* offset del indicador "detectado" (0 si no se conoce) */
} VrcSite;

/* Localiza las funciones de detección de cámaras/sensores en [lo,hi). Devuelve cuántas halló (<= max). */
int vrc_locate(const uint8_t *ram, uint32_t lo, uint32_t hi, VrcSite *out, int max);
/* Lee el alcance y el semiángulo originales (unidades del motor; ángulo 4096 = vuelta) de un actor. */
void vrc_read_params(const uint8_t *ram, const VrcSite *s, uint32_t actor, int32_t *range, int32_t *half);
/* 1 si `addr` es una función de detección de cámara/sensor de los sitios dados (vr_perception no debe parchearla). */
int vrc_is_sensor_func(const VrcSite *s, int n, uint32_t addr);

/* =====================================================================================================
 * PARTE 2: tabla de cámaras activas, daño (romperlas), sonido (oírlas de lejos), visibilidad.
 *
 * Todo esto trabaja sobre la RAM del juego desde el gancho de fotograma (CPU parada, igual que el puente
 * vr/bridge) y NO toca la lógica de detección: alcance, cono, línea de visión, mapa, camuflaje y caja son los
 * originales. Familias: VRC_FAM_CAMERA (vigilancia, la más común), VRC_FAM_SIMPLE (vigilancia "sencilla",
 * solo en dos zonas) y VRC_FAM_GUN (cámara con arma). Se identifican por la forma (hash de opcodes) de la
 * función de actualización del actor, no por nombres.
 *
 * Unidades: las del motor (enteros s16); ángulos 4096 = vuelta; rumbo 0 = +Z, 1024 = +X; cabeceo > 0 = mira
 * hacia abajo; Y hacia arriba. Un actor se identifica por su dirección (el nodo de la lista de actores es el
 * inicio del bloque de trabajo; la posición/orientación del actor están en actor+0x20 y actor+0x28).
 * ===================================================================================================== */
#include <stddef.h>

#define VRC_MAX_CAMERAS 32
#define VRC_MAX_SND_EVENTS 32

typedef enum {
    VRC_ST_PATROL = 0,    /* barriendo (LED verde) */
    VRC_ST_ALERT = 1,     /* ha visto al jugador y lo sigue (LED rojo) */
    VRC_ST_SEARCH = 2,    /* alerta general: busca al azar (LED amarillo) */
    VRC_ST_JAMMED = 3,    /* interferida por el objeto antirradar (LED amarillo) */
    VRC_ST_DYING = 4,     /* destruida: explosión y humo en curso, desaparece en ~40 fotogramas */
    VRC_ST_INACTIVE = 5   /* el script la apagó (mensajes de entrada/salida de sala) o nace apagada */
} VrcStatus;

typedef struct VrcCamera {
    uint32_t actor;          /* dirección del actor */
    VrcFamily family;
    uint32_t act;            /* función de actualización del actor */
    uint32_t control;        /* actor + 0x20 */
    uint32_t target;         /* blanco (caja de impacto del juego, 72 B) o 0 si el puntero no es válido */
    int16_t pos[3];          /* origen del cono (posición del actor, actor+0x20): desde aquí mide la detección original */
    int16_t rot[3];          /* rot: [0] cabeceo (>0 mira abajo), [1] rumbo, [2] 0 */
    int32_t range;           /* alcance original del cono (instancia) */
    int32_t half;            /* semiángulo original (cada eje por separado: pirámide, no cono circular) */
    uint32_t map_bit;        /* bit(s) de mapa del actor */
    VrcStatus status;
    uint8_t alive;           /* 0 si está muriendo */
    uint8_t active;          /* 1 si su lógica de barrido/detección está encendida */
    uint8_t sound_on;        /* el script permite sus sonidos */
    uint8_t in_player_map;   /* comparte mapa con el jugador (condición de la detección original) */
    int32_t proc;            /* script que se ejecuta al destruirla (-1 ninguno) */
    int32_t hits;            /* nº de impactos de bala acumulados (política VR) */
    int16_t box_center[3];   /* caja de impacto del juego (centro y semitamaño del blanco) */
    int16_t box_half[3];
    uint32_t alert_frame;    /* último fotograma en que pasó a ALERT (0 nunca) */
} VrcCamera;

typedef struct VrcTable {
    uint32_t frame;          /* contador de vrc_frame */
    int n;
    VrcCamera cam[VRC_MAX_CAMERAS];
} VrcTable;

/* Recorre las 9 listas de actores y rellena la tabla (sin escribir en RAM). Devuelve el nº de cámaras. */
int vrc_scan(const uint8_t *ram, VrcTable *t);
const VrcCamera *vrc_find(const VrcTable *t, uint32_t actor);
/* Familia de un actor por la forma de su función de actualización; -1 si no es una cámara/sensor conocido. */
int vrc_classify_actor(const uint8_t *ram, uint32_t actor);
/* Añade una firma de función de actualización (nº de palabras y hash de opcodes, ver vai_shape_hash) para otra
 * edición del juego o una variante. Devuelve 0 si la tabla (4 ranuras) está llena. */
int vrc_add_signature(VrcFamily fam, uint32_t nwords, uint64_t shape);
/* Prueba angular + alcance del cono original sobre un punto (SIN línea de visión ni mapa). Para dibujar el
 * cono / depurar; la detección real sigue siendo la del juego. */
int vrc_in_cone(const VrcCamera *c, const int16_t pt[3]);
/* Vector unidad de la dirección de mirada en ejes del motor (x,y,z; y hacia arriba). */
void vrc_forward(const VrcCamera *c, float out[3]);

/* ---- Política (daño / visibilidad) ---- */
typedef enum { VRC_DMG_BULLET = 0, VRC_DMG_EXPLOSIVE = 1 } VrcDamageKind;
typedef enum { VRC_HIT_BAD = -1, VRC_HIT_IGNORED = 0, VRC_HIT_DAMAGED = 1, VRC_HIT_DESTROYED = 2 } VrcHitResult;

typedef struct VrcConfig {
    /* daño */
    int bullets_destroy;     /* CAMBIO RESPECTO AL ORIGINAL (donde las balas no las dañan). 1 (por defecto): las balas las
                              * rompen tras `bullet_hits` impactos; 0: comportamiento original. Los explosivos destruyen siempre. */
    int bullet_hits;         /* nº de impactos de bala que la destruyen (por defecto 2; mínimo 1), sea cual sea el daño de cada uno */
    int ignore_class;        /* 1 (por defecto): aplica el daño aunque el blanco esté en "no dañable" (cámara sencilla en patrulla) */
    int pickup_game_bullets; /* 1: recoge los impactos de las balas propias del juego (a_mode 1) del blanco */
    /* visibilidad / animación */
    int cosmetic_sweep;      /* 1: si el script apagó una cámara viva, sigue girando. DESACTIVADO por defecto: escribe el giro
                              * objetivo del actor (que mueve su rumbo y por tanto el cono de detección al reactivarse). Si se
                              * activa, se restaura el giro original al terminar el barrido. */
    int32_t sweep_max_dist;  /* solo si el oyente está más cerca de esto (0 = sin límite) */
} VrcConfig;

void vrc_config_default(VrcConfig *c);
void vrc_config_get(VrcConfig *c);
void vrc_config_set(const VrcConfig *c);
void vrc_reset(void);        /* olvida acumuladores, eventos, tabla y estado de barrido (llamar al cambiar de zona) */
void vrc_reset_all(void);    /* además: configuración por defecto, firmas añadidas y oyente (pruebas) */

/* Llamar UNA vez por fotograma (gancho de fotograma): vrc_scan + recogida de impactos de balas del juego +
 * barrido cosmético + limpieza de estado de cámaras que ya no existen. Devuelve el nº de cámaras. */
int vrc_frame(uint8_t *ram, VrcTable *t);
const VrcTable *vrc_table(void);   /* última tabla de vrc_frame (NULL antes de la primera) */

/* ---- Daño ----
 * Aplica sobre el blanco de la cámara exactamente lo que la función de daño del juego (la que aplica un
 * impacto a un blanco enemigo) anotaría en una víctima: bit de daño, modo de arma 2 y daño acumulado. La
 * propia rutina de actualización de la cámara hace el resto (explosión pequeña, chispas, humo, script de
 * destrucción, retirada del actor): no se inventa ningún efecto. Los explosivos destruyen siempre; las balas
 * solo con `bullets_destroy` y al acumular `bullet_hits` impactos (cada llamada = 1 impacto, sea cual sea `damage`).
 * `punto`: punto de impacto en el mundo (para el aviso al integrador; no se usa para decidir). `damage` > 0.
 * Llamar desde el gancho de fotograma, nunca con la CPU emulada en marcha. */
int vr_cameras_hit(uint8_t *ram, uint32_t actor, const int16_t punto[3], int damage);
int vr_cameras_hit_ex(uint8_t *ram, uint32_t actor, const int16_t punto[3], int damage, VrcDamageKind kind);
/* Rayo contra las cajas de impacto del juego de las cámaras vivas (origen/dirección en unidades del motor).
 * Devuelve el índice de tabla del primer impacto (<0 ninguno) y la distancia en `t_out` y el punto en `hit`. */
int vrc_raycast(const VrcTable *t, const float origin[3], const float dir[3], float max_len, float *t_out, int16_t hit[3]);

/* ---- Sonido ---- */
typedef enum { VRC_SND_ORIGINAL = 0, VRC_SND_NATURAL = 1 } VrcSndMode;
typedef struct VrcSoundCfg {
    VrcSndMode mode;
    float ref_dist;     /* hasta aquí, volumen pleno (unidades del motor) */
    float rolloff;      /* exponente de la ley (1 = inversa: -6 dB al doblar la distancia) */
    float max_dist;     /* a partir de aquí, silencio */
    float fade_frac;    /* fracción final de max_dist en la que se funde a 0 (evita el corte seco) */
    float gain;         /* multiplicador global */
    int   min_vol;      /* mínimo (1..63) que se envía mientras haya algo audible: la cuantización de 6 bits no lo borra */
    int   n_se;         /* sonidos de cámara afectados */
    uint8_t se[8];
} VrcSoundCfg;
void vrc_sound_default(VrcSoundCfg *c);
void vrc_sound_get(VrcSoundCfg *c);
void vrc_sound_set(const VrcSoundCfg *c);
/* Volumen (0..63) y ganancia (0..1) de la curva para una distancia. En ORIGINAL replica el cálculo del juego
 * (cámara en primera persona): 63 - d/150 hasta 9500. */
int vrc_sound_curve(const VrcSoundCfg *c, float dist, float *gain_out);
/* Oyente: cabeza VR (unidades del motor; rumbo 4096 = vuelta). Sin llamarla (o con valid=0) se usa la cámara del
 * juego en primera persona y la posición del jugador en el resto, como el original. */
void vrc_set_listener(const int32_t pos[3], int yaw, int valid);

typedef struct VrcSoundEvent {
    uint32_t seq;         /* nº de orden (creciente) */
    uint32_t frame;
    uint16_t se;          /* identificador del sonido */
    int16_t pos[3];       /* POSICIÓN 3D DEL EMISOR (mundo, unidades del motor): para el audio binaural */
    int32_t cam_index;    /* índice en la última tabla (<0 si no se asoció a una cámara) */
    uint32_t actor;       /* actor emisor (0 si no se asoció) */
    float dist;           /* distancia oyente-emisor */
    float gain;           /* ganancia continua (0..1) de la curva activa; el audio binaural usa ésta, no `vol` */
    uint8_t vol, pan;     /* lo que se envía al reproductor estéreo del juego (0 = no se reproduce) */
    uint8_t vol_orig;     /* lo que habría enviado el juego original */
    uint8_t played;       /* 1 si se envió al reproductor */
} VrcSoundEvent;
/* Saca (y vacía) los eventos pendientes; devuelve cuántos copió. */
int vrc_sound_events(VrcSoundEvent *out, int max);

/* Intercepta una llamada de reproducción posicional (la función de reproducir sonido en una posición del
 * mundo). `pos_ptr` = puntero RAM a la posición (3 x s16) del emisor (0 = sin posición), `se` = id. Devuelve:
 *   VRC_SE_PASS  no es un sonido de cámara (o modo ORIGINAL): ejecutar la función original tal cual;
 *   VRC_SE_DROP  no reproducir (silencio por la curva, o fin de partida/demo como el original);
 *   VRC_SE_PLAY  reproducir enviando *cmd = (pan<<16 | vol<<8 | id) al reproductor con modo asíncrono (0).
 * En modo ORIGINAL registra el evento (posición del emisor) y devuelve PASS. */
enum { VRC_SE_PASS = 0, VRC_SE_DROP = 1, VRC_SE_PLAY = 2 };
int vrc_se_intercept(const uint8_t *ram, uint32_t pos_ptr, uint32_t se, uint32_t *cmd);
