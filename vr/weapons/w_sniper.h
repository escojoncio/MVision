/* Rifle de francotirador semiautomático ("sniper"): API propia (extiende el contrato sin tocar weapon.h).
 *
 * MIRA TELESCÓPICA (WAuxView), siempre activa con el arma en la mano (como en los juegos VR: nada de activar por proximidad ni tapar la vista):
 *  - la lente (ocular): eje óptico = normal de la placa "lens" del modelo enlazado; la IMAGEN se dibuja en la abertura interior del extremo trasero del tubo de la mira (contorno de los vértices del cuerpo en el plano de su borde, ver sniper_lens / SniperLens.face; PSG1 real: 44 x 44 mm);
 *    sin modelo enlazado se usa el reposo de la definición. La imagen ampliada es una textura viva sobre ese disco, vista desde cualquier ángulo.
 *  - aux.active = 1; aux.camera = pose de la lente (-Z = eje óptico, no depende de la cabeza) adelantada SNIPER_CAM_AHEAD, más el balanceo;
 *    aux.fov_y según la distancia de pantalla H del original (sniper_fov_y: 2 atan(128 / H), la textura cuadrada cubre el lienzo de 256 px del juego);
 *    target_part = pieza "lens" (uv 0..1); aux.overlay = WO_RETICLE (la retícula original la pinta la app con vr_scope.h).
 *  - ZOOM del original (weapon/rifle.c): H = 320 al empezar; pasados 9 ticks (25 Hz en PAL) sube H += H/3 hasta el límite y
 *    se acerca a él con NearExp4. Límite = distancia al primer muro en el eje (sniper_set_range), 3000 como máximo, y 320 (sin zoom) si es < 1000
 *    (sniper_zoom_limit). No hay control manual de aumentos: el original no lo tiene.
 *  - TIRO: la bala sale siempre del eje óptico (en el plano de la boca) y a lo largo de él, es decir por donde apunta la retícula.
 *  - sniper_lens_visible: el anfitrión puede omitir el segundo render cuando la lente no mira al jugador o está fuera de su campo.
 *  - el canal WCH_SCREEN de la pieza lens vale siempre 1.
 * BALANCEO: medido del original (guiñada ~1,1 grados pico a pico, 0,39/0,58 Hz; cabeceo ~0,7, 0,19/0,39 Hz) + temblor; se reduce con la segunda mano (x0,55) y con la acción
 * del tranquilizante (sniper_set_steady). Se aplica a la cámara de la mira y a la dirección del tiro
 * (siempre: la retícula y la bala se mueven juntas).
 * Controles: WB_STICK (mano del arma) = seguro. */
#pragma once
#include "w_gunc.h"

extern const WeaponDef w_sniper;

enum { SNIPER_PT_BODY, SNIPER_PT_STOCK, SNIPER_PT_TRIGGER, SNIPER_PT_SELECTOR, SNIPER_PT_MAG,
       SNIPER_PT_CHARGE, SNIPER_PT_BOLT, SNIPER_PT_LENS, SNIPER_PT_N };

#define SNIPER_RPM 300.0f
#define SNIPER_MAG 20
#define SNIPER_VIEW_COS    0.17f     /* cos 80 grados: semicampo con el que sniper_lens_visible da la lente por visible */
#define SNIPER_CAM_AHEAD   0.30f     /* m: máximo que la cámara de la mira va adelantada sobre el eje (sin verificar con el modelo real); se reduce si hay un muro cerca, ver sniper_cam_ahead */
#define SNIPER_RIM_R       0.05f     /* m: radio lateral (desde el centro de la placa) en el que se buscan los vértices del borde trasero del tubo de la mira (medido: la abertura del PSG1 tiene ~33 mm de radio al centro de la placa; más allá hay piezas ajenas) */
#define SNIPER_RIM_BACK    0.002f    /* m: hasta cuánto por delante de la placa se admiten vértices del borde */
#define SNIPER_RIM_FWD     0.25f     /* m: hasta cuánto por detrás de la placa (hacia el ojo) */
#define SNIPER_RIM_SLAB    0.001f    /* m: grosor de la losa de vértices que forman el borde (los vértices sueltos de otras piezas cortadas a 1,4 mm del plano quedan fuera) */
#define SNIPER_RIM_BINS    120       /* direcciones (3 grados) del contorno interior */
#define SNIPER_RIM_CAND    2048      /* vértices candidatos como máximo (marco de pila) */
#define SNIPER_FACE_MIN    0.005f    /* m: cara/abertura menor o mayor que esto = malla absurda, se usa el reposo */
#define SNIPER_FACE_MAX    0.30f
#define SNIPER_POLY_MAX    8         /* vértices máximos del polígono de la abertura */
#define SNIPER_ROUND_TOL   0.15f     /* abertura con |ancho - alto| <= 15 % del mayor: se dibuja como disco (w = h = media); si no, rectángulo */
#define SNIPER_LENS_LIFT   0.002f    /* m: el disco de la lente se adelanta hacia el ojo para no solaparse con la placa del modelo */
#define SNIPER_ZOOM_BASE_H 320.0f    /* rifle.c: distancia de pantalla normal */
#define SNIPER_ZOOM_MAX_H  3000.0f   /* rifle.c ScopePos / length */
#define SNIPER_ZOOM_NEAR_H 1000.0f   /* rifle.c: obstáculo más cerca que esto = sin zoom */
#define SNIPER_ZOOM_DELAY  9         /* rifle.c: ticks antes de empezar a subir */
#define SNIPER_TICK_HZ     25.0f     /* tick del juego: 2 VBlank por tick = 25 Hz en PAL (edición del usuario; 30 Hz en NTSC) */
#define SNIPER_VIEW_SIZE   320
#define SNIPER_ZOOM_GAME   9.375f    /* 3000 / 320: aumento del original */
#define SNIPER_SWAY_YAW    0.0078f   /* rad: amplitud por componente (pico ~1,3x) medida del original */
#define SNIPER_SWAY_PITCH  0.0046f

/* Intensidad (0..1) de la acción del tranquilizante del juego: 1 = pulso firme. */
void sniper_set_steady(WeaponState *s, float steady);
/* Ajuste de culata 0 (corta) .. 1 (larga); se aplica suavizado al canal CUSTOM1. */
void sniper_set_stock(WeaponState *s, float len01);

/* Lente de la mira en el marco del arma: pose con -Z = eje óptico (hacia el objetivo) y centro en el centro de la malla de la pieza "lens"; d = menor lado de la cara (m). model = 1 si salió de
 * la malla enlazada (centro, eje = normal de su cara y tamaño); 0 = reposo de la definición, eje -Z y d = 0,04. Devuelve model. */
typedef struct {
    WPose pose; float d; int model;   /* pose = centro de la placa y eje óptico (cámara y bala); d = menor lado de la cara (w, h) */
    WPose face; float w, h; int rim, round;
    int npoly; float poly[SNIPER_POLY_MAX][2];   /* contorno de la abertura: convexo, antihorario, relativo al centro de la cara (x derecha, y arriba, m); 0 = reposo sin modelo */  /* cara donde se dibuja la imagen: centro y orientación (+Z hacia el ojo, -Z = eje) y su caja ancho x alto (m); round = 1: abertura casi cuadrada, se dibuja como disco de diámetro w = h, si no rectángulo w x h. rim = 1: es la abertura interior del extremo trasero del tubo
                                       * (pieza "body", plano de su borde); 0: la placa "lens" (sin tubo hallado) */
} SniperLens;
int sniper_lens(const WeaponState *s, SniperLens *out);
/* 1 si la lente (pose mundo, -Z = eje óptico) da su cara trasera a la cabeza y queda dentro de ~80 grados de su dirección de mirada: si no, nadie puede ver la imagen. */
int sniper_lens_visible(const WPose *lens_w, const WPose *head);
/* Zoom del original (rifle.c): límite de la distancia de pantalla H según el primer impacto del eje (hit en unidades del motor, < 0 = sin impacto en 3000), y un tick de acercamiento. */
float sniper_zoom_limit(float hit);
float sniper_zoom_step(float h, float limit);
float sniper_fov_y(float h);                          /* 2 atan(128 / H) */
float sniper_cam_ahead(float range_m);                /* m que la cámara va adelantada de la lente: min(SNIPER_CAM_AHEAD, range_m - 0,05), nunca < 0; range_m < 0 o NaN (sin muro) = SNIPER_CAM_AHEAD */
void  sniper_set_range(WeaponState *s, float hit, float units_per_m);    /* primer impacto del eje de la mira (unidades del motor, < 0 = ninguno; limita el zoom) y unidades del motor por metro (su distancia en m para la cámara); lo mide el anfitrión antes de update */
int   sniper_lens_world(const WeaponState *s, WPose *out);   /* pose mundo de la lente de la última actualización (sin balanceo); 0 si aún no hubo */

float sniper_zoom(const WeaponState *s);              /* aumentos actuales (H / 320) */
void  sniper_sway(const WeaponState *s, float *yaw, float *pitch);   /* balanceo actual (rad) */

int sniper_casings(const WeaponState *s, WPose *out, int max);
