#pragma once
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
