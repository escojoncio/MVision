#pragma once
#include "cpu.h"
uint32_t fiber_open(Cpu *c, uint32_t pc, uint32_t sp, uint32_t gp);
uint32_t fiber_close(Cpu *c, uint32_t h);
uint32_t fiber_change(Cpu *c, uint32_t h);
uint32_t fiber_current(void);
