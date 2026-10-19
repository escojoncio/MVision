#pragma once
/* ns_pub: triple búfer SIN BLOQUEOS para publicar NsScene del productor (hilo de emulación) al consumidor (hilo de render). C11 (stdatomic).
 * Garantías (ver ns_types.h, «Entrega»):
 *   - El productor escribe SIEMPRE en un slot que nadie lee; el consumidor lee SIEMPRE un slot que nadie escribe. Ninguno espera al otro.
 *   - ns_pub_publish() es un intercambio release/acquire: todo lo que el productor escribió antes (el slot y las mallas añadidas al almacén) es visible para el
 *     consumidor cuando ns_pub_acquire() devuelve ese slot. El productor debe añadir las mallas ANTES de publicar.
 *   - Cada slot lleva un contador de generación (NsScene.slot_gen) que ns_pub_begin() sube al empezar a escribirlo.
 * Uso:  productor: s = ns_pub_begin(&p); ...rellenar *s (seq, mallas ya añadidas)...; ns_pub_publish(&p);
 *       consumidor: s = ns_pub_acquire(&p); (s es estable hasta la siguiente llamada; NULL si aún no hay ninguna escena)
 */
#include <stdatomic.h>
#include <stddef.h>
#include "ns_types.h"

typedef struct {
    NsScene *slot[3];
    _Atomic uint32_t mid;      /* índice del slot intermedio | 4 si es nuevo (no visto por el consumidor) */
    int back, front;           /* back: del productor; front: del consumidor */
    int have;                  /* el consumidor ya tomó alguna */
} NsPub;

static inline void ns_pub_init(NsPub *p, NsScene *a, NsScene *b, NsScene *c) {
    p->slot[0] = a; p->slot[1] = b; p->slot[2] = c;
    p->back = 0; p->front = 1; p->have = 0;
    atomic_init(&p->mid, 2u);
}
/* Productor: slot libre para escribir. Sube slot_gen. */
static inline NsScene *ns_pub_begin(NsPub *p) {
    NsScene *s = p->slot[p->back];
    s->slot_gen++;
    return s;
}
/* Productor: publica lo escrito (release). */
static inline void ns_pub_publish(NsPub *p) {
    uint32_t old = atomic_exchange_explicit(&p->mid, (uint32_t)p->back | 4u, memory_order_acq_rel);
    p->back = (int)(old & 3u);
}
/* Consumidor: la escena más reciente publicada (acquire), o la que ya tenía si no hay nueva. NULL si nunca se publicó. */
static inline NsScene *ns_pub_acquire(NsPub *p) {
    if (atomic_load_explicit(&p->mid, memory_order_relaxed) & 4u) {
        uint32_t old = atomic_exchange_explicit(&p->mid, (uint32_t)p->front, memory_order_acq_rel);
        p->front = (int)(old & 3u);
        p->have = 1;
    }
    return p->have ? p->slot[p->front] : NULL;
}
