#pragma once
/* Enlace opcional con vr/ai: convierte la configuración de percepción activa (VrpConfig) en la que usa el radar.
 * Requiere -I../ai (solo cabecera; vr_perception.h no depende del runtime). Llamar tras cada cambio de preset
 * (vrp_set_preset / vrp_config_preset). El preset ORIGINAL (enabled = 0 o range_mult <= 1) no dibuja nada extra. */
#include "vr_perception.h"
#include "vr_radar.h"

static inline void vrradar_set_vrp(const VrpConfig *c) {
    VrRadarPerc p;
    p.enabled = c ? c->enabled : 0;
    p.range_mult = c ? c->range_mult : 1.0f;
    p.periph_frac = c ? c->periph_frac : 1.0f;
    p.periph_near = c ? c->periph_near : 0.5f;
    vrradar_set_perception(&p);
}
