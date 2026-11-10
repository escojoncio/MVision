#pragma once
/* vr_smooth: fluidez de la locomoción. El juego publica una escena nueva a su ritmo (25-50 por segundo) y el visor dibuja a 90-100 Hz con la ÚLTIMA escena:
 * al andar el mundo avanzaba a saltos (judder) y al girar con el stick la vista llegaba con el retardo de toda la tubería. Este módulo (matemática pura, sin
 * dependencias; hilo de render) calcula por fotograma de render una corrección RÍGIDA del mundo de la escena vigente (giro sobre el eje vertical de la cabeza
 * de referencia + traslación) que lo lleva a donde estaría con el personaje y la vista del instante de presentación.
 *
 * Modelo (ver bitácora). Con la correspondencia mundo -> usuario de vr_scene (VrSceneOpts.user_map_*), un punto del mundo del juego W cae en la escena i en
 *     P_i(W) = c + R_i * K * (W - O_i)        c = p_ref - (0, h_ojo, 0),  K = 1/unidades_por_metro,  O_i = origen de la escena (ojo del motor)
 * y en el instante t de presentación debe caer en  P(W) = c + R(t) * K * (W - O(t)),  R(t) = R(delta) * R_i, delta = giro del stick pedido y aún no visto en la
 * escena (en grados, + izquierda). Entonces  P = Q (P_i - c) + c - Q R_i K (O(t) - O_i),  Q = R(delta): giro de `delta` sobre la vertical que pasa por p_ref
 * más una traslación. Se aplica SOLO a la geometría del mundo (el arma, el radar y las manos van en el espacio del usuario y no se tocan).
 *  - Giro: delta = lead * (giro total pedido ahora - giro total que traía la cámara de la escena): inmediato, por fotograma, sin retardo de tubería.
 *  - Traslación: la posición del ojo del motor B se muestra INTERPOLADA entre las dos últimas escenas, una escena por detrás más el margen -lead_s (el tramo se recorre en el tiempo de juego que
 *    tardó, sin la fluctuación del reloj de la emulación): nunca se pasa de una posición real del juego (sin sobreimpulso al parar ni error al cambiar de dirección), a cambio de
 *    un periodo de escena de retardo SOLO en la locomoción (el giro y la cabeza del usuario no se retrasan). El desajuste al llegar una escena antes o después de lo previsto
 *    se disipa con una exponencial (sin saltos). Cortes (teletransporte, cambio de época, hueco largo) reinician.
 * Reloj: `t` es cualquier reloj monótono en segundos (el mismo para las marcas de las escenas y para vrsm_eval). */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VrSmSample {
    int valid;           /* 1: escena de mundo en espacio de usuario con cámara VR (la corrección tiene sentido); 0: cualquier otra (menú, cine, vídeo) */
    double t_wall;       /* instante (reloj monótono, s) en que el hilo de emulación ensambló la escena */
    double game_s;       /* tiempo de juego de esa escena (VBlank * 0,02 s): monótono y sin fluctuación */
    float B[3];          /* posición del ojo del motor (unidades del juego): origen de la escena (x, z) y su altura */
    float R[9];          /* VrScene.user_R (fila-mayor) */
    float K;             /* user_k * scale = metros por unidad del juego de esa escena */
    float c[3];          /* centro del giro: user_p_ref (donde cae B) */
    double turn_deg;     /* giro total del stick (grados, + izquierda) que ya traía la cámara de la escena, en la escala de vrsm_eval(turn_req) */
    uint32_t epoch;      /* cambia con cada recentrado / recarga del ejecutable: sin continuidad entre épocas distintas */
} VrSmSample;

typedef struct VrSmoothCfg {
    int enabled;         /* 0: corrección siempre identidad (comportamiento anterior) */
    float lead_s;        /* adelanto (s) del reloj con que se recorre el camino entre escenas; negativo = margen por detrás (defecto -0,015: una escena que llega hasta 15 ms tarde no detiene el avance). Nunca se extrapola */
    float blend_s;       /* constante de tiempo con la que se disipa el error de predicción al llegar una escena (defecto 0,045) */
    float turn_lead;     /* 0..1: fracción del giro pendiente que se aplica ya (defecto 1: inmediato) */
    float pll_gain;      /* ganancia de fase del reloj de escenas (defecto 0,15) */
    float cut_units;     /* salto del cuerpo entre dos escenas (unidades del juego, x-z) que se toma por corte, no por movimiento (defecto 2500) */
    float max_gap_s;     /* hueco sin escenas tras el cual se reinicia (defecto 0,5) */
    float max_turn_deg;  /* giro pendiente máximo creíble; mayor = reinicio (defecto 400) */
} VrSmoothCfg;

typedef struct VrSmoothStats {
    uint32_t pushes, resets, evals, active_evals;
    double sum_shift_m, max_shift_m;   /* módulo de la traslación aplicada (m) */
    double max_delta_deg;              /* mayor giro pendiente aplicado */
    double max_err_m;                  /* mayor error de predicción absorbido al llegar una escena (m) */
    double sum_err_m;
    double rho;                        /* segundos de juego por segundo real (la emulación va a tiempo real = 1) */
    double period_s;                   /* periodo entre escenas (s reales) */
} VrSmoothStats;

typedef struct VrSmooth {
    VrSmoothCfg cfg;
    int have;
    VrSmSample cur;
    double ts;               /* instante suavizado (reloj de los `t`) de la escena vigente */
    double rho, period;
    float dB[3], dBp[3];     /* camino de la escena anterior a la vigente y de la ante-anterior a la anterior (unidades del juego) */
    double dur, durp;        /* segundos reales en que se recorre cada uno (los que tardó el juego); 0 = no hay */
    float err[3];            /* error de predicción pendiente de disipar (unidades) */
    double t_err;            /* instante de presentación en que nació err */
    int have_prev_game;
    double rho_g0, rho_w0;   /* ancla de la ventana de medida de rho (tiempo de juego y real) */
    int rho_anchor;
    VrSmoothStats st;
} VrSmooth;

typedef struct VrSmCorr {
    int active;              /* 0: usar la identidad */
    float m[16];             /* corrección 4x4 columna-mayor: P' = m * P (espacio del usuario) */
    float delta_deg;         /* giro aplicado (+ izquierda) */
    float shift_m[3];        /* desplazamiento del cuerpo respecto a la escena (m, espacio del usuario) */
    float alpha;             /* punto del camino que se está mostrando: 0..1 de la escena anterior (0) a la vigente (1); -1..0 de la ante-anterior (-1) a la anterior (0). Lo usa el cuerpo del jugador (mv_vrhost_body_piece) */
} VrSmCorr;

/* Muestra de una escena a partir de su mapa mundo -> usuario (UNICA definición: la usan la lista de la GPU y la escena nativa, cada una con SU registro de cámara; el suavizador
 * debe recibir la muestra de la escena que realmente se dibuja, o la corrige con el movimiento de otro fotograma del motor). origin = origen del mundo VR (unidades del juego),
 * R = user_R, K = metros por unidad, p_ref y eye_h = centro del giro (p_ref - (0, eye_h, 0) rotado por R, como vr_scene). */
void vrsm_make_sample(VrSmSample *out, const float origin[3], const float R[9], float K, const float p_ref[3], float eye_h, double game_s, double turn_deg, uint32_t epoch, double t_wall);
void vrsm_cfg_default(VrSmoothCfg *c);
void vrsm_init(VrSmooth *s, const VrSmoothCfg *c);   /* NULL = valores por defecto */
void vrsm_reset(VrSmooth *s);                         /* olvida la escena vigente (cambio de modo: menú, cine...) */
/* Llamar cuando el renderer toma una escena nueva. `t_now` = reloj actual. */
void vrsm_push(VrSmooth *s, const VrSmSample *sm, double t_now);
/* Corrección para el fotograma que se está codificando. `turn_req_deg` = giro total pedido por el usuario hasta ahora (misma escala que turn_deg). */
void vrsm_eval(VrSmooth *s, double t_now, double turn_req_deg, VrSmCorr *out);
void vrsm_stats(const VrSmooth *s, VrSmoothStats *out);

#ifdef __cplusplus
}
#endif
