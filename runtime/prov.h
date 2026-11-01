#pragma once
/* Procedencia (provenance) de las primitivas de la GPU: qué rutina del juego escribió cada palabra de RAM.
 *  - Cada llamada indirecta (jalr / rt_jump / rt_call) abre un ÁMBITO: rt_prov_cur = id interno de (función destino, nombre del actor): las instancias del mismo tipo comparten id.
 *    El nodo (ProvEntry.actor_node = el primero visto con esa clave) es a0 si parece un nodo de actor (RAM, con su +8 = la función llamada); su nombre de depuración es la cadena de +20.
 *  - Toda escritura del juego a RAM (mem_w8/16/32 y las rutas rápidas del intérprete) anota el ámbito vigente en rt_prov_mem (por palabra).
 *    Las escrituras del HLE en C (memcpy, DMA) no pasan por ahí y quedan con 0 (sin procedencia).
 *  - gpu_draw_list lee rt_prov_mem de la palabra-etiqueta de cada paquete de la OT (quién lo encadenó) y la guarda en GpuOp.prov.
 * Solo funciona con rt_prov_on = 1 (rt_prov_set); apagado, el coste es un `if` por escritura/llamada indirecta.
 * El intérprete es PLANO (jal/jr no recursan en C): sus llamadas indirectas anotan (ret, sp, ámbito anterior) en una pila sombra por hilo
 * y el `jr` que vuelve a esa dirección con el mismo sp la cierra (rt_prov_push / rt_prov_pop). */
#include <stdint.h>

#define PROV_MAX 1024        /* ids 1..PROV_MAX-1; 0 = sin procedencia */
#define PROV_STK 64          /* profundidad de la pila sombra por hilo */
#define PROV_THREADS 32      /* >= MAX_TH de fiber.c */

typedef struct { uint16_t id; uint32_t fn; uint32_t actor_node; char name[20]; } ProvEntry;

extern int rt_prov_on;
extern uint16_t rt_prov_cur;                 /* ámbito vigente */
extern uint16_t rt_prov_mem[0x80000];        /* por palabra de RAM (2 MB / 4) */
extern ProvEntry rt_prov_tab[PROV_MAX];      /* índice = id */
extern unsigned rt_prov_n;                   /* mayor id asignado */
extern unsigned long rt_prov_full;           /* internados rechazados por tabla llena (devuelven id 0) */
extern unsigned long rt_prov_ovf;            /* llamadas del intérprete sin anotar por pila sombra llena */

void rt_prov_set(int on);                    /* enciende/apaga; pone el ámbito a 0 */
void rt_prov_reset(void);                    /* vacía la tabla, la sombra de RAM y las pilas (pruebas / nueva partida) */
uint16_t rt_prov_enter(uint32_t tgt, uint32_t a0);   /* abre el ámbito de la llamada a tgt (a0 = argumento): devuelve el anterior */
static inline void rt_prov_leave(uint16_t old) { rt_prov_cur = old; }
/* intérprete plano */
void rt_prov_push(uint32_t tgt, uint32_t a0, uint32_t ret, uint32_t sp);
void rt_prov_pop(uint32_t sp, uint32_t target);     /* en cada jr: cierra la llamada si target/sp coinciden */
void rt_prov_unwind(uint32_t sp);                   /* longjmp: descarta las llamadas con sp <= sp y repone el ámbito */
/* hilos (fiber.c) */
void rt_prov_thread_init(unsigned t);
void rt_prov_switch(unsigned from, unsigned to);
