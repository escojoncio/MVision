#pragma once
#include <stdint.h>
/* Selección de captura del volcado de vrhost.c (funciones puras, probadas en test_dump_pick.c).
 * Capturas pendientes del pedido en curso: DN_ANY = la primera que llegue (pedido simple); DN_STAGE + DN_FRAME = pareja del mismo fotograma de juego
 * (primero el punto de enganche, justo tras la captura nativa; luego el final del fotograma). */
enum { DN_STAGE = 1, DN_FRAME = 2, DN_ANY = 4 };

/* Capturas que pide un pedido. El modo gpu no tiene punto de enganche (no hay captura nativa): la pareja se reduce al final de fotograma, que sale con la lista. */
static inline int dump_need_init(int native, int pair) {
    return pair ? (native ? (DN_STAGE | DN_FRAME) : DN_FRAME) : DN_ANY;
}

/* Qué captura toca ahora (0 = ninguna). `point` = 1 en el punto de enganche, 0 en el fin de fotograma. */
static inline int dump_pick_bit(int need, int point) {
    if (point) return (need & DN_STAGE) ? DN_STAGE : (need & DN_ANY);
    if (need & DN_ANY) return DN_ANY;
    if ((need & DN_FRAME) && !(need & DN_STAGE)) return DN_FRAME;
    return 0;
}

/* Autovolcado (vrhost.c autodump_note): decisión pura de si una lista anómala o una vista tapada justifica un volcado automático (etiqueta point=auto_<motivo>).
 * Límites: AD_MAX_N por sesión, AD_MAX_PER_WHY por motivo y AD_GAP_S s entre dos intentos (aunque el pedido no llegue a hacerse). Umbrales por lista (valores iniciales, a ajustar con los registros [autodump]). */
enum { AD_MAX_N = 8, AD_MAX_PER_WHY = 2 };
#define AD_GAP_S 5.0
#define AD_T_HWCULL 40u      /* triángulos que la GPU de PS1 no dibujaría (VrStats.n_hw_cull) */
#define AD_T_INCOH 10u       /* triángulos descartados por la red de coherencia (VrStats.drop_incoh) */
#define AD_T_QUAD 10u        /* cuadriláteros 3D no coherentes (VrStats.n_quad_noncoh) */
#define AD_T_LONG 10u        /* triángulos 3D con alguna arista > 10 m */
#define AD_T_COVER 0.60f     /* fracción de la ventana tapada por triángulos pegados al ojo (< 0,15 m) */
enum { AD_NONE = 0, AD_NEAR_COVER, AD_BLACK_COVER, AD_HWCULL, AD_INCOH, AD_QUAD, AD_LONG, AD_N };
typedef struct { uint32_t n_hw_cull, drop_incoh, quad_noncoh, long_edge; float near_cover; int black_cover; int world; } AutoDumpIn;
typedef struct { int n; double last_s; int by[AD_N]; } AutoDumpState;   /* n = volcados ya pedidos; last_s = instante del último intento (< 0: ninguno); by[AD_*] = pedidos por motivo */
static inline const char *autodump_name(int why) {
    static const char *nm[AD_N] = {"ninguno", "vista_tapada", "negro_en_mundo", "hw_cull", "incoherentes", "quads_no_coherentes", "aristas_largas"};
    return why > 0 && why < AD_N ? nm[why] : nm[0];
}
/* Motivo (AD_*) por el que pedir un volcado ahora, o AD_NONE. Primero las vistas tapadas (solo en vista mundo), luego las anomalías de la lista. Un motivo agotado (AD_MAX_PER_WHY) cede el turno al siguiente. */
static inline int autodump_decide(const AutoDumpState *st, const AutoDumpIn *in, double now_s) {
    if (st->n >= AD_MAX_N || (st->last_s >= 0 && now_s - st->last_s < AD_GAP_S)) return AD_NONE;
    if (in->world && in->near_cover > AD_T_COVER && st->by[AD_NEAR_COVER] < AD_MAX_PER_WHY) return AD_NEAR_COVER;
    if (in->world && in->black_cover && st->by[AD_BLACK_COVER] < AD_MAX_PER_WHY) return AD_BLACK_COVER;
    if (in->n_hw_cull >= AD_T_HWCULL && st->by[AD_HWCULL] < AD_MAX_PER_WHY) return AD_HWCULL;
    if (in->drop_incoh >= AD_T_INCOH && st->by[AD_INCOH] < AD_MAX_PER_WHY) return AD_INCOH;
    if (in->quad_noncoh >= AD_T_QUAD && st->by[AD_QUAD] < AD_MAX_PER_WHY) return AD_QUAD;
    if (in->long_edge >= AD_T_LONG && st->by[AD_LONG] < AD_MAX_PER_WHY) return AD_LONG;
    return AD_NONE;
}
