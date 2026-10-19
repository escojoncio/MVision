/* vr_camera: la cabeza del usuario como cámara del motor (primera persona) y control del dibujado del juego.
 *
 * Qué hace (detalle, direcciones y validación en bitacora.md y en vr_camera.c):
 *  - Decide cuándo la cámara VR manda: solo con el jugador controlable (puente: has_player && in_control, grupos
 *    BRIDGE_OK_ALL_CORE, con histéresis); nunca en cinemáticas, comunicador, pausa ni menús.
 *  - Con la cámara VR activa, el motor construye SU matriz de cámara (la que alimenta a la GTE y por tanto el
 *    dibujado, el recorte por objeto y la visibilidad de los actores) con el ojo en la cabeza del personaje y la
 *    dirección = yaw/pitch de la cabeza del usuario, no la cámara cenital del juego. El juego sigue en su lógica
 *    normal (el movimiento lo da el mando; el origen angular del mando se ajusta a la mirada).
 *  - La proyección NO se cambia (la H de la GTE sigue siendo la del juego): lo que se amplía es el recorte
 *    (patch_exe.py) y se anula el ajuste vertical de la matriz de pantalla, de modo que las posiciones 3D de vista
 *    que recoge el renderer (GpuVtx.vx/vy/vz) son métricas (mm, X derecha, Y abajo, Z delante) y se re-proyectan
 *    por ojo con la pose real de la cabeza: vrcam_delta() da la transformación vista-del-motor -> vista-de-cabeza.
 *  - Oculta la cabeza del modelo del jugador mientras la cámara VR está activa.
 *
 * Integración con el runtime (parches exactos en el informe del módulo): cinco funciones del motor se sustituyen por
 * hle_vrcam_* (vr_camera_hle.c) y el ejecutable se parchea con patch_exe.py antes de recompilarlo.
 *
 * Convenciones. Mundo del motor: Y ARRIBA, derecho (X x Y = -Z visto como cámara mira a -Z...), unidades ~ 1 mm
 * (el personaje mide ~1700). Ángulo del motor: 4096 = vuelta; yaw 0 = +Z, 1024 = +X, 2048 = -Z, 3072 = -X (giro
 * antihorario visto desde arriba: crece al girar a la izquierda); pitch positivo = mirar ABAJO. Vista de la GTE
 * (lo que ve el renderer): X derecha, Y abajo, Z delante. Pose de la cabeza (mv_vr_get_head): matriz 4x4
 * columna-mayor [m0..m3 = eje X derecha, m4..m7 = Y arriba, m8..m11 = Z atrás (delante = -Z), m12..m14 posición en
 * metros], espacio de juego del usuario (Y arriba); t = marca de tiempo en segundos.
 *
 * No es reentrante ni multihilo: todo se llama desde el hilo del emulador (gancho de frame y funciones del motor
 * sustituidas); la única llamada de otro hilo es la de la fuente de pose (mv_vr_get_head), que ha de ser segura.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fuente de la pose de la cabeza. Devuelve 1 si hay pose válida (m, t rellenados), 0 si no (seguimiento perdido). */
typedef int (*VrCamHeadFn)(float m[16], double *t);

enum { VRCAM_LOCO_HEAD = 0,   /* mando relativo a la mirada (por defecto): delante = hacia donde se mira */
       VRCAM_LOCO_BODY = 1 }; /* mirada = giro del cuerpo + giro relativo de la cabeza; mando relativo al cuerpo */
enum { VRCAM_EYE_AUTO = 0,    /* x,z del centro del cuerpo salvo tumbado (entonces la articulación de la cabeza) */
       VRCAM_EYE_BODY = 1,    /* x,z siempre del centro del cuerpo */
       VRCAM_EYE_JOINT = 2 }; /* x,y,z de la articulación de la cabeza (sigue balanceo y animaciones) */

typedef struct VrCamConfig {
    VrCamHeadFn head;        /* NULL: sin cabeza -> la cámara VR nunca se activa */
    float units_per_m;       /* unidades del motor por metro (1000) */
    float eye_up, eye_fwd;   /* desplazamiento del ojo respecto a la articulación de la cabeza, en unidades (arriba, delante) */
    int loco;                /* VRCAM_LOCO_* */
    int eye_xz;              /* VRCAM_EYE_* */
    int hide_head;           /* 1: ocultar la cabeza del modelo del jugador (defecto) */
    int override_pad;        /* 1: origen del mando según la mirada (defecto) */
    int on_frames;           /* fotogramas consecutivos de juego controlable antes de activar (defecto 8) */
    int engine_h;            /* H del motor = zoom_del_juego * engine_h / 320 (defecto 160; 0 = sin cambio, H = zoom).
                              * Una H menor reduce los vértices saturados (SX/SY a +-1023) y los desbordes de la división de la GTE
                              * (H >= 2 SZ): a H=160 solo saturan los vértices a >81 grados del eje y los de z < 80 mm.
                              * Debe coincidir con --h de patch_exe.py (la caja de recorte se calcula con ella). */
    int keep_overscan;       /* 1: no anular el ajuste vertical de matrices de pantalla (solo para pruebas A/B; defecto 0) */
    int draw_lag;            /* construcciones de cámara entre la que dibuja un envío y la última hecha (0 = la última) */
    float pitch_limit_deg;   /* tope de la elevación que se le da al motor (el resto lo absorbe vrcam_delta); defecto 80 */
} VrCamConfig;

void vrcam_config_default(VrCamConfig *c);
/* Reinicia el estado y fija la configuración (copia). Llamar antes de arrancar el juego. */
void vrcam_init(const VrCamConfig *c);
const VrCamConfig *vrcam_config(void);

/* ---- Gancho de frame (VBlank, CPU parada) ---------------------------------------------------------------------
 * Lee el estado del juego (puente) y decide si la cámara VR manda a partir del próximo frame del motor. `st` es el
 * BridgeState que ya haya leído el host este frame (NULL: se lee aquí). Solo lee la RAM. */
void vrcam_frame(const uint8_t *ram, const BridgeState *st);

/* Pide recentrar (el yaw de mirada vuelve a coincidir con el del cuerpo y la posición actual de la cabeza pasa a ser
 * el origen) en el próximo frame del motor. También ocurre sola al activarse. */
void vrcam_recenter(void);
/* Giro del espacio de juego del usuario (giro por pasos / suave del mando): grados, + = a la izquierda. */
void vrcam_turn(float deg);
/* Tope de la distancia de pantalla (zoom) que el juego pide a la cámara del usuario; 0 = sin tope. Con la mira óptica del francotirador
 * (el juego sube de 320 a 3000) la app pone 320: el aumento va en una vista aparte en el visor del arma y la cabeza no se acerca. */
void vrcam_set_zoom_cap(int cap);
/* 1 si en los últimos VBlanks el juego pidió más aumento que el tope (la mira del juego está activa): solo entonces la máscara de visión es la de la mira. */
int vrcam_zoom_capped(void);
/* 1 si el frame de cámara en curso es VR (decidido al construir la cámara). */
int vrcam_active(void);
/* Yaw de mirada (grados) del último frame VR; origen angular del mando (unidades del motor) con la configuración. */
float vrcam_view_yaw_deg(void);
int vrcam_pad_origin(void);

/* ---- Registro por frame de cámara ------------------------------------------------------------------------------
 * Cada vez que el motor construye la cámara principal se guarda aquí lo necesario para re-proyectar lo que dibuje
 * con ella. Los polígonos de un frame se dibujan (gfx_list) un frame del motor después de su construcción: el
 * registro de una lista capturada es vrcam_draw_record(n-ésimo envío). Los envíos se numeran desde 1. */
typedef struct VrCamRecord {
    uint32_t serial;         /* nº de construcción de cámara (>=1) */
    int active;              /* 1: cámara VR; 0: la del juego (el resto de campos describen la del juego) */
    int32_t eye[3];          /* posición del ojo del motor (mundo, unidades) */
    float M[9];              /* vista -> mundo, fila-mayor M[3*fila+col]; columnas = ejes X (derecha), Y (abajo), Z (delante) */
    int16_t screen;          /* distancia de pantalla H efectiva */
    float yaw_deg, pitch_deg;/* mirada dada al motor (pitch + = abajo) */
    float theta_play_deg;    /* giro espacio de usuario -> mundo (yaw de mirada = theta_play + yaw cabeza) */
    float p_ref[3];          /* posición de la cabeza (m, espacio del usuario) a la que corresponde `eye` */
    float units_per_m;
    float head[16];          /* pose de la cabeza muestreada al construir la cámara (la que el motor usó) */
} VrCamRecord;
const VrCamRecord *vrcam_last_record(void);
uint32_t vrcam_draw_count(void);                         /* envíos de frame al GPU vistos */
/* Registro de la cámara con la que se dibujó la primitiva capturada de número de secuencia `seq` (GpuOp.seq): la del
 * envío al GPU que la emitió (últimos 64 envíos), NULL si no pertenece a ninguno (p.ej. dibujo 2D fuera del envío) o
 * ya caducó. Requiere gpu_capture_seq() (parche runtime_seq.patch). */
const VrCamRecord *vrcam_seq_record(uint32_t seq);

/* Transformación de posiciones de vista del motor a vista de la cabeza actual:  v' = A v + b  (A fila-mayor 3x3).
 * Entrada y salida en el espacio de la GTE (X derecha, Y abajo, Z delante) y unidades del motor. Es la vista del
 * CENTRO de la cabeza; el renderer resta +-IPD/2 (en unidades del motor) a v'.x para cada ojo. `head` = pose actual
 * (mv_vr_get_head). Con r->active == 0 devuelve la identidad (devuelve 0: el renderer no debe re-proyectar). */
typedef struct VrCamDelta { float A[9]; float b[3]; } VrCamDelta;
int vrcam_delta(const VrCamRecord *r, const float head[16], VrCamDelta *out);

/* ---- Núcleo (público para pruebas y para el pegamento del runtime) ------------------------------------------- */
typedef struct VrCamPlan {
    int16_t from[3], to[3];  /* ojo y objetivo para el constructor de matriz del motor */
    int16_t screen;          /* distancia de pantalla a pasar */
    float yaw_deg, pitch_deg, theta_play_deg;
    float head[16];
    float p_ref[3];
} VrCamPlan;
/* Antes de construir la cámara principal. Devuelve 1 y rellena `p` si la cámara VR manda ahora; 0 si no (usar los
 * argumentos originales). Fija el estado "activo" del frame (vrcam_active). `zoom` = distancia de pantalla que pide
 * el juego (320 normal). Solo lee la RAM. */
int vrcam_plan(const uint8_t *ram, int16_t zoom, VrCamPlan *p);
/* Tras construir la matriz (con p o sin él, p == NULL = cámara del juego): lee la matriz resultante y anota el registro. */
void vrcam_commit(const uint8_t *ram, const VrCamPlan *p);
/* Oculta la cabeza del modelo del jugador (partes marcadas como no visibles para las etapas siguientes del frame).
 * Devuelve el nº de partes ocultadas. Escribe en la RAM del juego. */
int vrcam_hide_head(uint8_t *ram);
/* Anota un envío de frame al GPU que emitió las primitivas capturadas con seq en [seq_begin, seq_end) y le asocia el
 * registro de cámara vigente (el último construido, desplazado `draw_lag` construcciones; ver VrCamConfig). */
void vrcam_note_draw(uint32_t seq_begin, uint32_t seq_end);

/* ---- Parche del ejecutable en RAM ------------------------------------------------------------------------------
 * Equivalente en C de patch_exe.py para el modo sin código recompilado (el EXE se carga en RAM desde el disco del
 * usuario y se interpreta): aplica sobre la imagen ya cargada los 44 inmediatos de recorte (+ opcionalmente la
 * subdivisión y los 12 de "punto visible"). Todo o nada: si alguna instrucción no es la esperada (otra edición del EXE) no escribe nada y devuelve -1.
 * Devuelve el nº de puntos escritos. Llamar tras cargar el EXE y antes de ejecutar (o invalidar el código decodificado). */
typedef struct VrCamPatch {
    int engine_h;            /* H del motor con la que se calcula la caja de recorte (debe coincidir con VrCamConfig.engine_h) */
    float hfov_deg, vfov_deg;/* campo TOTAL que debe cubrir el recorte alrededor de la dirección del motor */
    int no_divide;           /* 1: desactiva la subdivisión de polígonos cercanos (recomendado) */
    int wide_points;         /* 1: ensancha también las 3 comprobaciones de "punto visible" (12 sitios; solo pruebas: ver vr_camera.c) */
} VrCamPatch;
void vrcam_patch_default(VrCamPatch *p);                   /* 160, 140, 120, no_divide = 1, wide_points = 0 */
int vrcam_patch_ram(uint8_t *ram, const VrCamPatch *p);

/* Matemática pura (sin estado) */
/* Yaw (grados, antihorario visto desde arriba, + = izquierda) y elevación (grados, + = arriba) de una pose de cabeza. */
void vrcam_head_angles(const float head[16], float *yaw_deg, float *elev_deg);
/* Dirección de mirada del motor (vector unitario mundo) para yaw (grados motor) y pitch (+ abajo). */
void vrcam_dir(float yaw_deg, float pitch_deg, float d[3]);

typedef struct VrCamStats {
    uint32_t builds, vr_builds, head_misses, activations, deactivations, recenters;
} VrCamStats;
void vrcam_stats(VrCamStats *s);

#ifdef __cplusplus
}
#endif
