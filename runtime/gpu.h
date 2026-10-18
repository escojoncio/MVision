#pragma once
/* GPU PS1: rasterizador software de referencia (oráculo del renderer Metal/VR). */
#include <stdint.h>

#define GPU_VRAM_W 1024
#define GPU_VRAM_H 512
extern uint16_t gpu_vram[GPU_VRAM_H][GPU_VRAM_W];

void gpu_reset(void);
/* Puertos: GP0 = 0x1F801810 (escritura), GPUREAD = 0x1F801810 (lectura),
 * GP1 = 0x1F801814 (escritura), GPUSTAT = 0x1F801814 (lectura). */
void gpu_gp0(uint32_t w);
void gpu_gp1(uint32_t w);
uint32_t gpu_read(void);
uint32_t gpu_stat(void);
/* DMA canal 2 (0x1F8010A0 MADR, A4 BCR, A8 CHCR): ejecutar al escribir CHCR con bit 24. */
void gpu_dma2(uint32_t madr, uint32_t bcr, uint32_t chcr);
/* Recorre una lista enlazada de paquetes (modo lista del DMA / DrawOTag). */
void gpu_draw_list(uint32_t addr);

typedef struct {
    int x, y, w, h;      /* área de VRAM mostrada */
    int depth24, interlace, pal, enabled;
} GpuDisplay;
void gpu_get_display(GpuDisplay *d);

/* Contadores por frame (para depurar): primitivas recibidas por clase. */
typedef struct { uint32_t poly, line, rect, fill, copy, upload, download, env, lists; } GpuStats;
extern GpuStats gpu_stats;

/* Vuelca a PPM el área de display (full_vram = 0) o la VRAM completa (1). */
void gpu_dump_ppm(const char *path, int full_vram);
