#pragma once
/* Enlace de las funciones HLE del SDK en ejecución (modo intérprete, sin código generado).
 *
 * En el modo recompilado las funciones sustituidas por HLE se enlazan por nombre al generar el C
 * (names_eu.json + runtime/hle*.txt). En el modo sin código generado (MV_NOGEN) no hay nada derivado
 * del juego en el binario: se carga un fichero de firmas del SDK (solo hashes enmascarados, ver
 * tools/sig/hlesigs.py y la cabecera de hlebind.c) y se empareja contra el EXE cargado en RAM.
 * Cada función HLE encontrada se registra por dirección: el intérprete, al llegar a esa dirección
 * (llamada, salto o caída), ejecuta hle_<nombre>(c) en lugar del código original. */
#include <stddef.h>
#include <stdint.h>
#include "cpu.h"

/* Tabla nombre -> hle_<nombre>(c), generada de runtime/hle*.txt (hle_table.c; make hle_table). */
typedef struct { const char *name; FuncPtr fn; } HleEntry;
extern const HleEntry hle_table[];
extern const unsigned hle_table_count;

/* Empareja las firmas (JSON en memoria) contra el texto del EXE ya cargado en RAM [text_base, +text_size);
 * entry_pc = pc de la cabecera (inicio de función conocido).
 * Registra cada HLE encontrada y avisa por stderr de las que no aparecen (se queda el código original).
 * Devuelve el nº de HLE enlazadas, o <0 si las firmas no se pueden leer. Sustituye a un enlace anterior. */
int hle_bind(const char *json, size_t len, uint32_t text_base, uint32_t text_size, uint32_t entry_pc);
int hle_bind_file(const char *path, uint32_t text_base, uint32_t text_size, uint32_t entry_pc);
unsigned hle_bind_missing(void);          /* nº de HLE de la tabla que no se pudieron enlazar */

/* Consulta (hilo del emulador). hle_bind_bits: 1 bit por palabra de RAM con una HLE enlazada. */
extern uint8_t hle_bind_bits[PSX_RAM_SIZE / 32];
FuncPtr hle_bind_lookup(uint32_t addr);   /* NULL si no hay HLE en esa dirección */
const char *hle_bind_name(uint32_t addr); /* nombre de la HLE enlazada o NULL */
uint32_t hle_bind_addr(const char *name); /* dirección enlazada de la HLE o 0 */
static inline int hle_bind_is(uint32_t a) {
    uint32_t w = (a & (PSX_RAM_SIZE - 1)) >> 2;
    return (hle_bind_bits[w >> 3] >> (w & 7)) & 1;
}

/* Hash enmascarado de n palabras (mismo que tools/sig/sigs.py); expuesto para las pruebas. */
void hle_sig_hash(const uint32_t *w, unsigned n, uint8_t out[20]);
uint32_t hle_sig_mask(uint32_t w);

/* Ejecuta el código ORIGINAL (interpretado) de una función enlazada por HLE; 0 si no está enlazada. */
int hle_bind_orig(Cpu *c, const char *name);
