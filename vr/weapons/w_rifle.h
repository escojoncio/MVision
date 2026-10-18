/* Fusil de asalto bullpup ("rifle"): API propia (extiende el contrato sin tocar weapon.h).
 * Automático ~950 rpm, 25 balas, cargador tras la empuñadura, palanca de carga bajo el asa.
 * Lógica de disparo/recarga común en w_gunc.c. Selector auto/semi con WB_STICK de la mano del arma.
 *
 * Render: la pieza "mag" (canal WCH_MAG) solo se dibuja si WeaponState.mag_inserted; el cargador
 * suelto/en la mano se dibuja con la malla de la pieza "mag" en las poses de rifle_loose_mag/rifle_held_mag
 * (origen de la malla en su centro; +Y hacia las balas); las vainas con la malla "casing" (definida por el cargador de modelos). Las piezas son
 * datos: se asocian por nombre a las partes del modelo original (ver tabla en bitacora/informe). */
#pragma once
#include "w_gunc.h"

extern const WeaponDef w_rifle;

enum { RIFLE_PT_BODY, RIFLE_PT_TRIGGER, RIFLE_PT_SELECTOR, RIFLE_PT_MAG, RIFLE_PT_CHARGE, RIFLE_PT_BOLT, RIFLE_PT_N };

#define RIFLE_RPM 950.0f
#define RIFLE_MAG 25

int rifle_loose_mag(const WeaponState *s, WPose *out);
int rifle_held_mag(const WeaponState *s, const WInput *in, WPose *out);
int rifle_casings(const WeaponState *s, WPose *out, int max);
int rifle_is_semi(const WeaponState *s);
