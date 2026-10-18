/* vr_ai_scan: utilidades comunes de la capa de IA VR para leer la RAM emulada (2 MB, KSEG0) y localizar
 * funciones MIPS de las overlays de zona por firma estructural. Sin dependencias del runtime. C99. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define VAI_RAM_SIZE 0x200000u

/* Acceso a RAM: direcciones enmascaradas a 2 MB (KUSEG/KSEG0/KSEG1 equivalentes), little-endian. */
uint32_t vai_r32(const uint8_t *ram, uint32_t a);
int16_t  vai_r16(const uint8_t *ram, uint32_t a);
void     vai_w16(uint8_t *ram, uint32_t a, int16_t v);
void     vai_w32(uint8_t *ram, uint32_t a, uint32_t v);
void     vai_w8(uint8_t *ram, uint32_t a, uint8_t v);
uint8_t  vai_r8(const uint8_t *ram, uint32_t a);

/* Instrucciones */
uint32_t vai_jal(uint32_t target);               /* codificación de `jal target` */
int vai_is_prologue(uint32_t w);                 /* addiu sp,sp,-N */
int vai_is_jal(uint32_t w);
uint32_t vai_jal_target(uint32_t w);             /* destino absoluto en KSEG0 */

/* Límites de una función que contiene la dirección `inside` dentro de la ventana de código [lo,hi):
 * inicio = prólogo anterior más cercano; fin = siguiente prólogo (sin la cola de ceros). 0 si no cabe. */
int vai_func_bounds(const uint8_t *ram, uint32_t inside, uint32_t lo, uint32_t hi, uint32_t *start, uint32_t *end);

/* Hash FNV-1a 64 de la "forma" de la función: solo opcodes (y funct en tipo R), sin registros ni inmediatos.
 * Identifica la variante lógica de una función aunque cambien las constantes/offsets de su overlay. */
uint64_t vai_shape_hash(const uint8_t *ram, uint32_t start, uint32_t end);

/* Cuenta ocurrencias de una palabra en [start,end) */
int vai_count_word(const uint8_t *ram, uint32_t start, uint32_t end, uint32_t w);
