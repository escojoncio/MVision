/* Rifle de francotirador semiautomático ("sniper"): API propia (extiende el contrato sin tocar weapon.h).
 *
 * MIRA TELESCÓPICA (WAuxView):
 *  - la mira se activa cuando el ojo derecho (head + 0,032 m a la derecha) está a menos de
 *    SNIPER_SCOPE_ON_R de WP_EYE y dentro de un cono de 60 grados detrás del ocular; se desactiva a
 *    más de SNIPER_SCOPE_OFF_R (histéresis). WeaponState.aux.active refleja ese estado.
 *  - aux.camera = WP_SCREEN (objetivo) orientada como el arma (con retroceso) más el balanceo;
 *    aux.fov_y según aumentos (2 atan(128 / (320 z)): la textura cuadrada cubre el lienzo de 256 px del juego); target_part = pieza "lens" (uv 0..1); aux.overlay = WO_RETICLE | SNIPER_OV_VIGNETTE.
 *  - la app renderiza la escena a aux.width x aux.height, llama a sniper_draw_overlay() sobre el
 *    RGBA (R | G<<8 | B<<16 | A<<24, u32 little-endian) y lo sube como textura de la pieza "lens".
 *  - el canal WCH_SCREEN de la pieza lens vale 0..1 (apertura del iris de la mira).
 * BALANCEO: medido del original (guiñada ~1,1 grados pico a pico, 0,39/0,58 Hz; cabeceo ~0,7, 0,19/0,39 Hz) + temblor; se reduce con la segunda mano (x0,55) y con la acción
 * del tranquilizante (sniper_set_steady). Se aplica a la cámara de la mira y a la dirección del tiro
 * mientras se mira por ella.
 * Controles: WB_STICK (mano del arma) = seguro; WB_A (mano del arma) = cambiar aumentos. */
#pragma once
#include "w_gunc.h"

extern const WeaponDef w_sniper;

enum { SNIPER_PT_BODY, SNIPER_PT_STOCK, SNIPER_PT_TRIGGER, SNIPER_PT_SELECTOR, SNIPER_PT_MAG,
       SNIPER_PT_CHARGE, SNIPER_PT_BOLT, SNIPER_PT_LENS, SNIPER_PT_N };

#define SNIPER_RPM 300.0f
#define SNIPER_MAG 20
#define SNIPER_SCOPE_ON_R  0.10f
#define SNIPER_SCOPE_OFF_R 0.14f
#define SNIPER_BASE_FOV    1.0472f   /* 60 grados: vista sin aumentos del juego */
#define SNIPER_OV_VIGNETTE 0x100u    /* bit propio de aux.overlay (fuera de WO_*) */
#define SNIPER_VIEW_SIZE   320
#define SNIPER_ZOOM_GAME   9.375f    /* 3000 / 320: aumento del original */
#define SNIPER_SWAY_YAW    0.0078f   /* rad: amplitud por componente (pico ~1,3x) medida del original */
#define SNIPER_SWAY_PITCH  0.0046f

/* Intensidad (0..1) de la acción del tranquilizante del juego: 1 = pulso firme. */
void sniper_set_steady(WeaponState *s, float steady);
/* Ajuste de culata 0 (corta) .. 1 (larga); se aplica suavizado al canal CUSTOM1. */
void sniper_set_stock(WeaponState *s, float len01);

int   sniper_scoped(const WeaponState *s);            /* 1 si la vista auxiliar está activa */
float sniper_zoom(const WeaponState *s);              /* aumentos actuales */
void  sniper_sway(const WeaponState *s, float *yaw, float *pitch);   /* balanceo actual (rad) */
void  sniper_draw_overlay(const WeaponState *s, uint32_t *rgba, int w, int h);

int sniper_loose_mag(const WeaponState *s, WPose *out);
int sniper_held_mag(const WeaponState *s, const WInput *in, WPose *out);
int sniper_casings(const WeaponState *s, WPose *out, int max);
