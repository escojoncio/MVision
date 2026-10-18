#pragma once
#include "cpu.h"

/* Hook del host por frame (VSync). Headless: contar/terminar; app: presentar. */
typedef void (*HostFrameHook)(Cpu *c, uint32_t frame);
extern HostFrameHook rt_host_frame;

void hle_invoke(Cpu *c, uint32_t fn, uint32_t arg);
/* Pila para callbacks de eventos (hle_invoke/CD): región de kernel propia, como la del manejador de excepciones del
 * BIOS real; dentro de otro callback sigue por debajo del sp actual. Antes se usaba sp-0x400 del hilo en curso, que
 * con pilas de hilo de 0x400 B caía en la pila de OTRO hilo y pisaba sus marcos (lo que el código recompilado
 * toleraba porque retorna por la pila de C, pero el intérprete sigue la ra de memoria). */
uint32_t hle_cb_sp(uint32_t cur_sp);
void hle_cb_done(void);   /* cierra el callback abierto por hle_cb_sp */
uint32_t hle_frame_count(void);
