/* Fusil de asalto bullpup ("rifle"): API propia (extiende el contrato sin tocar weapon.h).
 * Automático 750 rpm (la del juego en PAL), 25 balas, cargador tras la empuñadura.
 * Lógica de disparo común en w_gunc.c. Selector auto/semi con WB_STICK de la mano del arma. Sin recarga VR: las balas son un reflejo del cargador del juego (w_gunc.h).
 *
 * Render: la pieza "mag" (canal WCH_MAG) solo se dibuja si WeaponState.mag_inserted; las vainas con la malla "casing" (definida por el cargador de modelos). Las piezas son
 * datos: se asocian por nombre a las partes del modelo original (ver tabla en bitacora/informe). */
#pragma once
#include "w_gunc.h"

extern const WeaponDef w_rifle;

enum { RIFLE_PT_BODY, RIFLE_PT_TRIGGER, RIFLE_PT_SELECTOR, RIFLE_PT_MAG, RIFLE_PT_CHARGE, RIFLE_PT_BOLT, RIFLE_PT_N };

#define RIFLE_RPM 750.0f    /* una bala cada 2 ticks del juego (famas.c, 25 ticks/s PAL): el arma VR no se seca antes que el cargador del juego */
#define RIFLE_MAG 25

int rifle_casings(const WeaponState *s, WPose *out, int max);
int rifle_is_semi(const WeaponState *s);
