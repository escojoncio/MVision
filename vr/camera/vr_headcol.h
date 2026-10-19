/* vr_headcol: la cabeza del usuario no puede atravesar el escenario (para no ver a través de paredes).
 *
 * El ojo del personaje del juego (la cámara del motor) es el ANCLA: en el espacio del usuario cae en p_ref y la cabeza física se desplaza respecto de él (asomarse,
 * inclinarse, andar por la habitación). Este módulo limita ese desplazamiento con una esfera de ~13 cm barrida contra los triángulos del mundo cercano:
 *   - los triángulos salen de las escenas VR que ya ensambla el host (clase 3D opaca, espacio del usuario) y se guardan en el MUNDO FIJO del motor (unidades del
 *     motor) con vida limitada, para que lo que sale del cono de dibujo un instante no desaparezca de la colisión y lo que se mueve (enemigos) no deje fantasmas;
 *   - se ignoran los triángulos pequeños pegados a las articulaciones del propio personaje (su cuerpo no es un obstáculo);
 *   - consulta: barrido ancla -> cabeza física con "choca y desliza" (hasta 3 pasos); el resultado es el desplazamiento a SUMAR a la cabeza para dibujar. Nunca
 *     se toca al personaje del juego;
 *   - fundido a negro progresivo (0..1) si la cabeza física empuja más allá del límite (corrección grande) o si, pese al recorte, quedara penetrada.
 * Una esfera ya solapada con un triángulo al empezar no queda atrapada: solo se le prohíbe acercarse más (distancia mínima = la inicial).
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
    float radius_m;         /* radio de la esfera de la cabeza, metros de usuario (def. 0,13) */
    float keep_m;           /* se guardan triángulos dentro de esta distancia al ancla, metros de usuario (def. 2,0) */
    float body_m;           /* un triángulo con sus 3 vértices a menos de esto de una articulación del personaje es su cuerpo y se ignora (def. 0,30) */
    float safe_m;           /* distancia mínima ojo-superficie bajo la cual, aun recortado, hay penetración visible (def. 0,05) */
    float fade_start_m;     /* corrección (cabeza física - cabeza dibujada) a partir de la cual empieza el fundido (def. 0,08) */
    float fade_full_m;      /* corrección con fundido total (def. 0,35) */
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
    float pen_m;            /* penetración residual (m) de la cabeza dibujada contra triángulos que el ancla no solapaba */
    int hit;                /* 1 si el barrido recortó */
    int n_cand;             /* triángulos candidatos del barrido */
} VrHcOut;

void vrhc_config_default(VrHcCfg *c);
void vrhc_init(const VrHcCfg *c);        /* NULL = defecto; vacía el almacén */
const VrHcCfg *vrhc_config(void);
void vrhc_reset(void);                   /* vacía el almacén y el fundido (recentrado, cambio de zona) */

/* Correspondencia de una cámara VR ACTIVA del registro; devuelve m->valid. */
int vrhc_map_from_record(VrHcMap *m, const VrCamRecord *r);

/* Alimenta con la geometría opaca 3D de una escena que ya viene en el espacio del usuario (sc->user_space) construida con `m`.
 * joints = articulaciones del personaje en unidades del motor (nj puede ser 0). Devuelve cuántos triángulos entraron/se refrescaron. */
int vrhc_feed_scene(const VrHcMap *m, const VrScene *sc, const float (*joints)[3], int nj);
/* Lo mismo con una lista plana de triángulos del espacio del usuario (9 floats cada uno, metros). */
int vrhc_feed_tris(const VrHcMap *m, const float *tris, int n, const float (*joints)[3], int nj);

/* Consulta: `m` es la correspondencia de la escena que se está mostrando; head = posición física de la cabeza (m, espacio del usuario); dt = segundos desde la
 * consulta anterior (liberación del fundido). Devuelve 1 si hay corrección o fundido. */
int vrhc_query(const VrHcMap *m, const float head[3], float dt, VrHcOut *out);

/* Triángulos vivos (diagnóstico / pruebas). */
int vrhc_count(void);

/* Matemática pura para pruebas: punto más cercano de un triángulo; devuelve la distancia. */
float vrhc_tri_dist(const float a[3], const float b[3], const float c[3], const float p[3], float closest[3]);

#ifdef __cplusplus
}
#endif
