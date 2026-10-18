#pragma once
#include "cpu.h"

/* Hook del host por frame (VSync). Headless: contar/terminar; app: presentar. */
typedef void (*HostFrameHook)(Cpu *c, uint32_t frame);
extern HostFrameHook rt_host_frame;

void hle_invoke(Cpu *c, uint32_t fn, uint32_t arg);
uint32_t hle_frame_count(void);
