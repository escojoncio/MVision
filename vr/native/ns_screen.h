#ifndef NS_SCREEN_H
#define NS_SCREEN_H
/* ns_screen: efectos de pantalla completa del renderer nativo (agua, anillo/máscara, fundidos, destello, modos de visión).
 *
 * Reglas de comodidad en VR:
 *  - El TINTE y la ONDULACIÓN del agua viven en el MUNDO: el sombreador de fragmentos los aplica a todo fragmento cuya posición en el mundo del motor esté dentro
 *    del volumen (o a todos si la cabeza está dentro). Girar la cabeza no gira nada: sólo cambia qué fragmentos se ven.
 *  - Los RECUBRIMIENTOS (fundido, destello, modos, anillo, líneas, barras) se calculan en tangentes del ojo (t = (ndc + P2x)/P00), así que su geometría no depende
 *    de la resolución del ojo y siguen a la cabeza como en la pantalla del motor (px = t * H).
 * Este módulo es C puro: construye los uniformes que consume Metal y trae las funciones de referencia con las que las pruebas sintéticas
 * verifican exactamente lo que hace el sombreador. Los valores de color/ganancia son AJUSTES (ver NsScreenCfg). */
#include "ns_types.h"
#include "ns_gpu_types.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float wave_gain;        /* multiplica la amplitud de la ondulación (filas del motor -> texels) */
    float wave_space;       /* unidades del motor por paso del periodo (longitud de onda = wave_period * wave_space) */
    float bars_gain;        /* 0..1 opacidad de las barras bajo el agua */
    float bars_soft;        /* semianchura del borde suave de las barras, en px del motor */
    float scan_gain;        /* intensidad de las líneas de exploración de los modos de visión */
    float scan_period;      /* filas del motor por línea */
    float scan_cover;       /* proporción oscurecida de cada periodo */
    float mode_amount;      /* intensidad del tinte de modo */
    float mode_a_rgb[3], mode_b_rgb[3];   /* color 0..1 del tinte de cada modo */
    float flash_gain;       /* intensidad máxima del destello (VR: menos de 1 para no deslumbrar) */
    float flash_slew;       /* variación máxima por segundo del destello */
    float fade_slew;        /* variación máxima por segundo del fundido (<=0: sin límite) */
    float proj_h;           /* H REAL de la cámara de la escena (NsCamera.h); <= 0: 320 (normal). Los radios y filas del motor se convierten a tangentes con ella */
} NsScreenCfg;
NsScreenCfg ns_screen_cfg_default(void);

/* Estado entre fotogramas: sólo suaviza destello y fundido (limitador de pendiente). */
typedef struct { float flash, fade; int init; } NsScreenState;
void ns_screen_state_reset(NsScreenState *s);

/* Construye los recubrimientos y los parámetros de agua. head_vr: posición de la cabeza en el mundo VR (m). Devuelve cuántos recubrimientos hay.
 * out->water_*: [3] = 1 en water_min si hay volumen; water_tint[3] = 1 si la cabeza está dentro. fromvr: inversa de to_vr (3x4 fila-mayor). */
int ns_screen_build(const NsScreenFx *fx, const float to_vr[12], const float head_vr[3], float dt, const NsScreenCfg *cfg, NsScreenState *st,
                    NsGpuScreen *out, float fromvr[12]);

/* Inversa de una 3x4 afín (fila-mayor). 0 si es singular. */
int ns_screen_invert34(const float m[12], float out[12]);
void ns_screen_xform34(const float m[12], const float p[3], float out[3]);

/* ---- referencias de CPU (lo mismo que ns_fs / ns_fs_overlay) ---- */
/* Aplica un recubrimiento al color de fondo b (0..1) en el punto de tangentes (tx, ty); H = distancia de pantalla del motor. Devuelve el color (sin recortar a 0..1 salvo al final). */
/* ydir: componente vertical (mundo VR, Y arriba) del rayo unitario del píxel; solo lo usa SCAN (las líneas están ancladas al mundo, no giran con la cabeza). RING y BARS son
 * viñetas circulares (invariantes al giro de la cabeza sobre su eje), el resto de recubrimientos son uniformes. */
void ns_screen_overlay_ref(const NsGpuOverlay *ov, float tx, float ty, float ydir, const float b[3], float out[3]);
/* Agua en el mundo: ew = posición del fragmento en el mundo del motor; uv += desplazamiento; col *= tinte. Devuelve 1 si el fragmento está afectado. */
int ns_screen_water_ref(const float wmin[4], const float wmax[4], const float wtint[4], const float wwave[4], const float ew[3], float *du, float *dv, float col[3]);

#ifdef __cplusplus
}
#endif
#endif
