#pragma once
/* MDEC (descompresor de macrobloques) por software, a nivel de registros:
 * 0x1F801820 comando/parámetros (escritura) y FIFO de salida (lectura),
 * 0x1F801824 control (escritura) y estado (lectura). DMA0 = entrada, DMA1 = salida.
 * El reproductor de vídeo del juego vive en una overlay y programa el hardware directamente. */
#include <stdint.h>

void mdec_reset(void);
uint32_t mdec_io_read(uint32_t addr, int size);
void mdec_io_write(uint32_t addr, uint32_t v, int size);
/* DMA0: palabras de RAM principal -> puerto de comando/datos. */
void mdec_dma_in(const uint32_t *src, uint32_t words);
/* DMA1: FIFO de salida -> RAM principal. Devuelve palabras realmente copiadas. */
uint32_t mdec_dma_out(uint32_t *dst, uint32_t words);

/* Núcleo reutilizable (pruebas/herramientas). */
void mdec_set_default_tables(void); /* tablas típicas: cuantización MPEG por defecto y escala DCT */
