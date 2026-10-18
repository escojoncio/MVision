#pragma once
/* E/S de sistema: interrupciones, contadores, DMA, mando, tarjeta de memoria, eventos de BIOS. */
#include <stdint.h>
#include "cpu.h"

/* ---- Reloj virtual y frame ---- */
/* Llamar una vez por VSync (desde hle_VSync, antes de los callbacks de VSync del juego). */
void sys_frame(Cpu *c);
/* 0 = NTSC (60 Hz, 263 líneas), 1 = PAL (50 Hz, 314 líneas). Por defecto PAL (disco EU). */
void sys_set_video(int pal);
uint64_t sys_cycles(void);             /* ciclos de CPU virtuales (33,8688 MHz) */

/* ---- Interrupciones (I_STAT/I_MASK) ---- */
enum { IRQ_VBLANK = 0, IRQ_GPU, IRQ_CD, IRQ_DMA, IRQ_TMR0, IRQ_TMR1, IRQ_TMR2, IRQ_SIO0, IRQ_SIO, IRQ_SPU, IRQ_PIO };
void sys_irq_raise(int bit);
uint32_t sys_irq_stat(void);

/* ---- Eventos de BIOS (bios.c) ---- */
/* Entrega un evento: modo 0x1000 llama a la función MIPS, modo 0x2000 lo deja listo para TestEvent. */
void bios_deliver_event(Cpu *c, uint32_t cls, uint32_t spec);
#define EV_VBLANK 0xF0000001u
#define EV_CARD_SW 0xF0000011u
#define EV_CARD_HW 0xF4000001u
#define EV_RCNT(n) (0xF2000000u + (n))
#define EVS_IOE 0x0004u
#define EVS_ERROR 0x8000u
#define EVS_TIMEOUT 0x0100u
#define EVS_NEWCARD 0x2000u
#define EVS_INTR 0x0002u

/* ---- Mando (pad.c) ----
 * buttons: bit a 1 = pulsado, orden PS1: 0 Select, 1 L3, 2 R3, 3 Start, 4 Arriba, 5 Derecha, 6 Abajo,
 * 7 Izquierda, 8 L2, 9 R2, 10 L1, 11 R1, 12 Triángulo, 13 Círculo, 14 Cruz, 15 Cuadrado.
 * Ejes analógicos 0..255, centro 0x80. */
enum { PAD_NONE = 0, PAD_DIGITAL = 1, PAD_ANALOG = 2 };
void pad_set_type(int port, int type);  /* por defecto: puerto 0 analógico, puerto 1 desconectado */
void pad_set_state(int port, uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);
/* Vibración pedida por el juego: motor pequeño (0/1) y grande (0..255). */
void pad_get_rumble(int port, uint8_t *small, uint8_t *large);
void pad_frame(Cpu *c);
/* Usadas por bios.c (B0:12/13/14). */
void pad_bios_init(Cpu *c, uint32_t b1, uint32_t s1, uint32_t b2, uint32_t s2);
void pad_bios_start(Cpu *c);
void pad_bios_stop(void);

/* ---- Tarjeta de memoria (card.c) ----
 * Imagen cruda de 128 KB (.mcd). Si no existe se crea formateada. Puerto 0 = "bu00:", 1 = "bu10:".
 * Sin card_open, se usa la variable de entorno MV_CARD (puerto 0) en el primer acceso. */
int card_open(int port, const char *path);
void card_close(int port);
/* Llamadas de BIOS de tarjeta / ficheros "buXX:" (desde bios.c). Devuelven v0. */
int card_bios(Cpu *c, uint32_t vec, uint32_t fn);

/* ---- Enrutado de io.c ----
 * 0x1F801810/14 -> gpu_gp0/gpu_read y gpu_gp1/gpu_stat (gpu.c); DMA2 -> gpu_dma2.
 * 0x1F801C00-FFF -> spu_io_read/write (spu.c); DMA4 -> spu_dma_write/read; spu_irq_take -> I_STAT bit 9.
 * 0x1F801820/24 -> mdec_io_read/write (mdec.c); DMA0 -> mdec_dma_in, DMA1 -> mdec_dma_out.
 * 0x1F801800-03 y DMA3: ganchos débiles (el CD va por HLE de librería en cdrom.c). */
uint32_t cd_io_read(uint32_t a, int size);
void cd_io_write(uint32_t a, uint32_t v, int size);
void cd_dma(uint32_t madr, uint32_t bcr, uint32_t chcr);
