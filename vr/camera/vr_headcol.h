/* vr_headcol: la cabeza del usuario no puede atravesar el escenario (para no ver a través de paredes).
 *
 * El ojo del personaje del juego (la cámara del motor) es el ANCLA: en el espacio del usuario cae en p_ref y la cabeza física se desplaza respecto de él (asomarse,
 * inclinarse, andar por la habitación). Este módulo limita ese desplazamiento con una esfera de 20 cm barrida contra los triángulos del mundo cercano:
 *   - FUENTE PRINCIPAL: la escena NATIVA (completa en 360 grados: escenario y objetos dinámicos sólidos, también lo que está detrás o a los lados de la cámara del
 *     juego), ya en el mundo del motor (vrhc_feed_world; la reemplaza entera cada escena, sin fantasmas);
 *   - FUENTE COMPLEMENTARIA: las escenas VR de la lista de la GPU (clase 3D opaca, espacio del usuario), guardadas en el MUNDO FIJO del motor con vida limitada. Es
 *     la única fuente cuando no hay escena nativa (modo gpu, escena no apta), y se usa siempre además de la nativa (unión: nunca menos protección);
 *   - se ignoran los triángulos pequeños pegados a las articulaciones del propio personaje (su cuerpo no es un obstáculo);
 *   - consulta: (1) barrido ancla -> cabeza física con "choca y desliza" (hasta 3 pasos); (2) EXPULSIÓN: si la posición resultante quedara a menos de face_m de un
 *     sólido (el ancla ya estaba pegada a la pared, o un objeto se movió encima), se empuja hacia fuera por la normal de contacto (hasta push_max_m), de modo que el plano
 *     cercano (10 cm) nunca corta una superficie; (3) fundido a negro INMEDIATO (sube de golpe, se libera con fade_release_s) si la cabeza física atraviesa un sólido
 *     (el segmento ancla -> cabeza cruza una superficie), si tras la expulsión sigue a menos de safe_m de un sólido, si la expulsión fue enorme, si empuja más allá del
 *     límite (corrección grande) o si faltan candidatos (desbordamiento: ante la duda, negro, nunca ver a través). El resultado es el desplazamiento a SUMAR a la
 *     cabeza para dibujar. Nunca se toca al personaje del juego.
 * C99 puro; no es thread-safe (el host protege con un candado). Sin dependencias del runtime. */
#pragma once
#include <stdint.h>
#include "vr_scene.h"
#include "vr_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VrHcCfg {
    int enabled;            /* 1 (def.) */
    float radius_m;         /* radio de la esfera de la cabeza del barrido, metros de usuario (def. 0,20: con el plano cercano a 0,1 m la cara nunca corta una pared) */
    float keep_m;           /* se guardan triángulos dentro de esta distancia al ancla, metros de usuario (def. 2,0) */
    float body_m;           /* un triángulo con sus 3 vértices a menos de esto de una articulación del personaje es su cuerpo y se ignora (def. 0,30) */
    float safe_m;           /* distancia mínima ojo-superficie que la expulsión debe garantizar; por debajo, fundido (def. 0,12 = plano cercano 0,1 m + margen) */
    float face_m;           /* distancia que la expulsión intenta dejar entre el ojo y cualquier sólido (def. 0,18; entre safe_m y radius_m) */
    float push_max_m;       /* expulsión máxima por consulta (def. 0,60) */
    float through_m;        /* radio del segmento ancla -> cabeza para detectar que la cabeza física cruza una superficie (def. 0,03) */
    float fade_start_m;     /* corrección (cabeza física - cabeza dibujada) a partir de la cual empieza el fundido (def. 0,10) */
    float fade_full_m;      /* corrección con fundido total (def. 0,30) */
    float fade_release_s;   /* tiempo en deshacer un fundido completo (def. 0,30) */
    int ttl;                /* escenas alimentadas durante las que vive un triángulo (def. 100 = 2 s a 50 por segundo) */
    float step_frac;        /* paso del barrido como fracción del radio (def. 0,25) */
} VrHcCfg;

/* Correspondencia espacio del usuario <-> mundo fijo del motor de UNA cámara: W = eye + upm * R^T (P - p_ref). */
typedef struct VrHcMap {
    int valid;
    float eye[3];           /* ojo del motor (unidades del motor) */
    float R[9];             /* giro mundo -> usuario (fila-mayor), el de VrSceneOpts.user_R */
    float upm;              /* unidades del motor por metro de usuario */
    float p_ref[3];         /* cabeza del usuario al recentrar (m) */
} VrHcMap;

typedef struct VrHcOut {
    float corr[3];          /* desplazamiento (m, espacio del usuario) a sumar a la cabeza física para dibujar */
    float fade;             /* 0..1: negro progresivo (ya suavizado con la liberación) */
    float corr_len;         /* |corr| */
    float pen_m;            /* penetración residual (m): cuánto falta para safe_m tras la expulsión (0 = ninguna) */
    float clear_m;          /* distancia final (m) de la cabeza dibujada al sólido más cercano (9 si no hay candidatos) */
    float pushed_m;         /* expulsión aplicada (m) */
    float through;          /* 0..1: la cabeza física atraviesa una superficie (segmento ancla -> cabeza) */
    int hit;                /* 1 si el barrido recortó */
    int n_cand;             /* triángulos candidatos del barrido */
    int native_live;        /* 1 si la escena nativa alimenta la colisión (fresca); 0 = solo la lista de la GPU */
    int overflow;           /* 1 si hubo más candidatos de los que caben (fundido total por seguridad) */
} VrHcOut;

void vrhc_config_default(VrHcCfg *c);
void vrhc_init(const VrHcCfg *c);        /* NULL = defecto; vacía el almacén */
const VrHcCfg *vrhc_config(void);
void vrhc_reset(void);                   /* vacía el almacén y el fundido (recentrado, cambio de zona) */

/* Correspondencia de una cámara VR ACTIVA del registro; devuelve m->valid. */
int vrhc_map_from_record(VrHcMap *m, const VrCamRecord *r);

/* Geometría sólida de la escena NATIVA, ya en el MUNDO del motor (9 floats por triángulo, unidades del motor). REEMPLAZA la lista nativa anterior (sin fantasmas).
 * Devuelve cuántos guardó (tope HC_NAT_MAX; si n lo supera o `truncated`, la consulta funde a negro por seguridad). Caduca sola si no se refresca (1 s de consultas). */
#define HC_NAT_MAX 32768
int vrhc_feed_world(const float *tris, int n, int truncated);   /* truncated = 1: la lista no recoge todo lo cercano (desbordó el búfer del recolector) */
int vrhc_native_count(void);

/* Alimenta con la geometría opaca 3D de una escena que ya viene en el espacio del usuario (sc->user_space) construida con `m`.
 * joints = articulaciones del personaje en unidades del motor (nj puede ser 0). Devuelve cuántos triángulos entraron/se refrescaron. */
int vrhc_feed_scene(const VrHcMap *m, const VrScene *sc, const float (*joints)[3], int nj);
/* Lo mismo con una lista plana de triángulos del espacio del usuario (9 floats cada uno, metros). */
int vrhc_feed_tris(const VrHcMap *m, const float *tris, int n, const float (*joints)[3], int nj);

/* Consulta: `m` es la correspondencia de la escena que se está mostrando; head = posición física de la cabeza (m, espacio del usuario); dt = segundos desde la
 * consulta anterior (liberación del fundido). Devuelve 1 si hay corrección o fundido. */
int vrhc_query(const VrHcMap *m, const float head[3], float dt, VrHcOut *out);

/* Distancia (unidades del motor) de un punto del mundo fijo del motor a la superficie estática más cercana a menos de max_units; -1 si no hay. */
float vrhc_nearest_world(const float p[3], float max_units);

/* Primer impacto del rayo o + t*d (mundo fijo del motor, unidades del motor; d se normaliza) con la geometría sólida guardada, t en (0, max_units]. Fuente: la escena nativa si está
 * fresca (suelo, techo, paredes, puertas y objetos sólidos en 360 grados, solo lo cercano al ancla: ver keep_m); si no, la lista de la GPU. -1 = sin impacto / entradas no finitas.
 * Solo lectura del almacén (el que llama sostiene el candado del host). Sirve al láser de la pistola (vr/weapons/w_laser.h). */
float vrhc_raycast_world(const float o[3], const float d[3], float max_units);

/* Triángulos vivos (diagnóstico / pruebas). */
int vrhc_count(void);

/* Matemática pura para pruebas: punto más cercano de un triángulo; devuelve la distancia. */
float vrhc_tri_dist(const float a[3], const float b[3], const float c[3], const float p[3], float closest[3]);

#ifdef __cplusplus
}
#endif
