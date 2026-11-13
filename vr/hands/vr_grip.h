/* vr_grip: cómo agarra cada arma la mano nueva (vr/body/vr_hand_skin + vr_hand_pose). Tabla por game_weapon_id (vr/weapons/weapon.h).
 *
 * Cada entrada da: la pose de la mano del arma y de la mano de apoyo (HP_*), el zócalo (socket) arma <- mano, el punto del gatillo y el zócalo de la mano de apoyo.
 *
 * MARCOS. Marco de la mano = el de vr_hand_skin (dedos -Y, palma +X en la mano DERECHA, pulgar +Z), aquí en METROS con 1 unidad de la mano = 1 mm (unidades * VRHS_UNIT_M; la mano va a la escala del brazo del juego). Marco del arma =
 * el del modelo (-Z = boca, +Y arriba, +X derecha). Los zócalos son de la mano derecha canónica; para la izquierda vrgrip_mirror() (espejo en X, como la propia malla).
 *   grip     WPose que lleva puntos de la mano (m) al marco de EMPUÑADURA: ejes del arma, origen en el punto de empuñadura (el que devuelve vrhands_grip: def->points[WP_GRIP] o el derivado
 *            del modelo). El centro de agarre de la mano (vrhs_grip_center) cae en ese origen. Arma en mundo por la mano: W = mano_mundo * inv(grip) (en el modelo: vrhands_grip * grip).
 *   trigger  punta del índice con el gatillo suelto (HP_GRIP_PISTOL / HP_GRIP_RIFLE con trigger 0), en el marco de empuñadura (m). Con el gatillo a fondo la yema retrocede ~25 mm.
 *   support  igual que grip, para la mano de apoyo, respecto al punto WP_SUPPORT del arma (la palma agarra un cilindro cuyo eje es el del cañón).
 * La mano gira con respecto al arma como en un apretón de manos (eje del mango vrhs_handle_axis hacia arriba, dedos hacia delante, palma a la izquierda); en las armas con gatillo además
 * -17 grados de cabeceo y -35 de guiñada para que la yema del índice caiga sobre el gatillo. Sin geometría real del arma (no hay datos del juego aquí): el gatillo está a ~63 mm por delante y
 * ~32 mm por encima de la empuñadura; test_grip.c los comprueba con piezas primitivas y, si hay datos, con el modelo real. */
#pragma once
#include "vr_hand_pose.h"
#include "../weapons/weapon.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int game_weapon_id;
    int pose;             /* HP_*: mano del arma */
    int support_pose;     /* HP_*: mano de apoyo; -1 = el arma no se sujeta con la otra mano */
    int has_trigger;      /* el índice sigue el gatillo analógico (vrhandpose_has_trigger(pose)) */
    WPose grip;           /* marco de la mano (m) -> marco de empuñadura */
    WVec3 trigger;        /* yema del índice con el gatillo suelto, en el marco de empuñadura (m); solo si has_trigger */
    WPose support;        /* marco de la mano de apoyo (m) -> marco del punto WP_SUPPORT */
    WVec3 elbow_main;     /* dirección del codo del brazo que empuña (marco del torso: x hacia fuera de su lado, y arriba, z delante; medida en Snake, ver vr_grip.c); (0,0,0) = sin dato */
    WVec3 elbow_support;  /* igual, brazo de apoyo */
    WQuat turn;           /* giro local de la mano que empuña respecto al mando (marco de vr_hand_skin; vrbody_set_hand_turn): grip ya lo incluye, así que el arma conserva la orientación
                           * respecto al mando que tendría sin él. (0, 0, 0, 0) o (0, 0, 0, 1) = sin giro */
} VrGripEntry;

const VrGripEntry *vrgrip_find(int game_weapon_id);   /* NULL si el arma no está en la tabla */
int vrgrip_count(void);
const VrGripEntry *vrgrip_at(int i);                  /* 0..count-1; NULL fuera de rango */

/* Colocación del objeto del juego respecto a la mano (granadas 2, 7, 8): pose del marco local del modelo (unidades del motor = mm) en el marco de la mano derecha canónica (mm): p_mano = R * p_modelo + t, R fila-mayor
 * 3x3. Es la relación con la que se ajustaron las poses de granada (gripfit_gen.c / test_gripfit.c; ver vr_grip_obj.inc). 1 = hay desplazamiento (R y t rellenados, pueden ser NULL); 0 = el id no lo tiene y conserva la
 * relación mano-objeto del juego. Para ns_wphide_carry_off. */
int vrgrip_object_offset(int game_weapon_id, float R[9], float t[3]);

/* Zócalo de la mano izquierda a partir del de la derecha: refleja X (posición x -> -x; cuaternión (x, y, z, w) -> (x, -y, -z, w)). Es una involución. */
WPose vrgrip_mirror(WPose p);
/* Arma <- mano: handle = pose del punto de empuñadura (o de apoyo) en el modelo, socket = e->grip o e->support, side +1 derecha / -1 izquierda. */
WPose vrgrip_weapon_from_hand(WPose handle, WPose socket, int side);

#ifdef __cplusplus
}
#endif
