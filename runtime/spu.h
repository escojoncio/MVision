#pragma once
/* SPU emulada a nivel de registros (0x1F801C00-0x1F801FFF), RAM de sonido de 512 KB,
 * 24 voces ADPCM con ADSR, reverb y entrada CD-XA. Salida estéreo 44100 Hz. */
#include <stdint.h>

#define SPU_RAM_SIZE 0x80000u
#define SPU_RATE     44100

void spu_reset(void);

/* Registros. Implementación fuerte de los ganchos que llama io.c. */
uint32_t spu_io_read(uint32_t addr, int size);
void spu_io_write(uint32_t addr, uint32_t v, int size);

/* DMA canal 4 (lo dispara io.c al escribir CHCR). Palabras de 32 bits en memoria host. */
void spu_dma_write(const uint32_t *src, uint32_t words); /* RAM principal -> SPU */
void spu_dma_read(uint32_t *dst, uint32_t words);        /* SPU -> RAM principal */

/* IRQ9 de la SPU: devuelve 1 (y lo consume) si saltó desde la última consulta.
 * El dueño del controlador de interrupciones debe fijar I_STAT bit 9. */
int spu_irq_take(void);
/* Gancho opcional llamado en cuanto salta el IRQ (desde el hilo que renderiza). */
extern void (*spu_irq_hook)(void);

/* Genera `frames` muestras estéreo intercaladas (L,R) a 44100 Hz y avanza la SPU. */
int spu_render(int16_t *out_stereo, int frames);
/* Avanza la SPU descartando la salida (o grabándola en el WAV). */
void spu_advance(int frames);

/* Sector CD-XA de audio: `sec` apunta a la subcabecera (offset 16 del sector Mode 2 bruto,
 * 8 bytes de subcabecera + 2304 de datos); len >= 8 + 2304. Se decodifica y encola. */
void spu_feed_xa(const uint8_t *sec, int len);
void spu_xa_reset(void);

/* Volcado WAV (headless). Abre el fichero; todo lo renderizado se escribe hasta spu_dump_close. */
int spu_dump_wav(const char *path);
void spu_dump_close(void);

/* Acceso directo para pruebas/depuración. */
uint8_t *spu_ram(void);
/* Decodifica un bloque ADPCM de 16 bytes a 28 muestras; hist[0]=s-1, hist[1]=s-2 (se actualiza). */
void spu_adpcm_block(const uint8_t *blk, int16_t out[28], int32_t hist[2]);
/* Envolvente: estado interno de una voz expuesto para pruebas. */
typedef struct {
    int phase;      /* 0 ataque, 1 decaimiento, 2 sostenido, 3 relajación, 4 apagada */
    int32_t level;  /* 0..0x7FFF */
    int32_t counter;
    uint16_t lo, hi; /* registros ADSR */
} SpuAdsr;
void spu_adsr_keyon(SpuAdsr *e);
void spu_adsr_keyoff(SpuAdsr *e);
void spu_adsr_tick(SpuAdsr *e);
