#pragma once
/* Parcheo en tiempo de ejecución del código ya cargado en RAM (EXE y overlays), sin tocar nunca el disco ni ficheros.
 * Dos mecanismos genéricos, ambos validados por el hash del bloque de código y reversibles:
 *
 *  1. Sustitución de función (solo intérprete): rt_patch_fn(addr, fn, guard) hace que toda entrada a `addr` (llamada, salto o
 *     caída) ejecute fn(c) en lugar del código. fn recibe la CPU de la llamada (a0 = r[4]...) y devuelve como la función
 *     original (el intérprete continúa en r[31]). Dentro de fn, rt_call_orig(c, addr) ejecuta el código original.
 *     Si hay `guard`, se comprueba el hash del bloque en el registro Y en CADA entrada: si el código de esa dirección cambió
 *     (otra overlay cargada encima, otra edición) el parche se desactiva solo y la entrada ejecuta el código real; el dueño
 *     lo ve con rt_patch_stale(). Con la misma overlay recargada (mismo hash) el parche sigue vigente.
 *     Un parche no puede coincidir con una HLE enlazada (hlebind) en la misma dirección.
 *
 *  2. Parche de palabras de código: rt_patch_code(addr, n, esperadas, nuevas, &id) escribe `nuevas` en RAM solo si las
 *     `n` palabras actuales coinciden con `esperadas` (todo o nada), invalida la caché del intérprete y guarda el original;
 *     rt_unpatch_code(id) lo restaura (solo si el bloque sigue siendo el parcheado; si ya lo pisó otra carga no toca nada).
 *
 * Todo corre en el hilo de emulación (ganchos de host_hooks.h o dentro de funciones HLE/parche). */
#include <stddef.h>
#include <stdint.h>
#include "cpu.h"

/* Hash FNV-1a de 64 bits de `nwords` palabras de RAM desde `addr` (dirección del juego), little-endian. 0 si no cabe en la RAM. */
uint64_t rt_code_hash(uint32_t addr, unsigned nwords);

typedef struct RtPatchGuard {
    unsigned nwords;        /* palabras del bloque a validar (1..4096) desde la dirección del parche */
    uint64_t hash;          /* rt_code_hash esperado */
} RtPatchGuard;

#define RT_PATCH_MAX 24
enum { RT_PATCH_OK = 0, RT_PATCH_BAD_ADDR = -1, RT_PATCH_FULL = -2, RT_PATCH_HASH = -3, RT_PATCH_BUSY = -4 };

int  rt_patch_fn(uint32_t addr, FuncPtr fn, const RtPatchGuard *guard);
int  rt_patch(uint32_t addr, FuncPtr fn);         /* sin guarda: el que llama ya validó la función (p. ej. por forma) */
void rt_unpatch(uint32_t addr);
void rt_unpatch_all(void);
int  rt_patched(uint32_t addr);                   /* 1 si hay un parche de función vigente en addr */
int  rt_patch_stale(uint32_t addr);               /* 1 si el parche existe pero su guarda dejó de coincidir (inerte) */
unsigned rt_patch_count(void);
/* Ejecuta el código ORIGINAL de la función de `addr` (sin parche ni HLE en la primera entrada) como una llamada normal. */
void rt_call_orig(Cpu *c, uint32_t addr);

/* Parche de palabras. `id` >= 0 en éxito. */
int  rt_patch_code(uint32_t addr, unsigned n, const uint32_t *expect, const uint32_t *neu, int *id);
int  rt_unpatch_code(int id);                     /* 0 = restaurado, 1 = la zona ya era otra (no se toca), <0 id inválido */
void rt_unpatch_code_all(void);
unsigned rt_patch_code_count(void);

/* Consulta rápida del intérprete (1 bit por palabra de RAM con parche de función). */
extern uint8_t rt_patch_bits[PSX_RAM_SIZE / 32];
static inline int rt_patch_is(uint32_t a) {
    uint32_t w = (a & (PSX_RAM_SIZE - 1)) >> 2;
    return (rt_patch_bits[w >> 3] >> (w & 7)) & 1;
}
FuncPtr rt_patch_lookup(uint32_t addr);           /* hilo del emulador; NULL si no hay (devuelve la entrada que comprueba la guarda) */

/* Intérprete: olvida el bloque especial (sustitución) de `addr` y los bloques de código que lo cubren. */
void interp_forget(uint32_t addr);
