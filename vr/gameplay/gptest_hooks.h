/* Ganchos de la herramienta gptest (implementados en gptest_hooks.c según lo que se pruebe). */
#pragma once
#include <stdint.h>
#include "bridge.h"
void gptest_pre_bind(void);
int gptest_init(uint8_t *ram);
void gptest_frame(uint32_t f, int t, const BridgeState *bs, uint16_t *pad);
void gptest_finish(void);
/* Línea de guion que gptest no reconoce (la herramienta concreta la interpreta); se llama al cargar el guion. */
void gptest_cmd(const char *line);
