#ifndef NS_SCREEN_H
#define NS_SCREEN_H
/* ns_screen: efectos de pantalla completa del renderer nativo (agua, anillo/máscara, fundidos, destello, modos de visión).
 *
 * Reglas de comodidad en VR:
 *  - El TINTE del agua vive en el MUNDO: el sombreador de fragmentos lo aplica a todo fragmento cuya posición en el mundo del motor esté dentro
 *    del volumen (o a todos si la cabeza está dentro). Girar la cabeza no gira nada: sólo cambia qué fragmentos se ven.
 *  - La ONDULACIÓN del agua es una distorsión de la IMAGEN FINAL del ojo (post-proceso por ojo tras la escena nativa, antes de los paneles 2D): desplazamiento
 *    horizontal por fila de pantalla, como el PS1; solo con la cabeza dentro del agua. No toca las texturas ni la profundidad.
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
    float wave_gain;        /* píxeles del ojo (con el ojo a NS_WAVE_REF_W de ancho) de desplazamiento horizontal por fila del motor de amplitud de la ondulación */
    float wave_space;       /* filas del motor por paso del periodo (longitud de onda en pantalla = wave_period * wave_space filas del motor) */
    float bars_gain;        /* 0..1 opacidad de las barras bajo el agua; defecto 0 = sin barras (la viñeta circular dejaba un círculo claro con el resto casi negro y translúcido) */
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
 * out->water_*: [3] = 1 en water_min si hay volumen; water_tint[3] = 1 si la cabeza está dentro. fromvr: inversa de to_vr (3x4 fila-mayor).
 * out->water_wave (solo con la cabeza dentro; si no, todo 0 = sin onda): x = amplitud (px del ojo a NS_WAVE_REF_W de ancho), y = frecuencia (rad por fila del motor), z = fase (rad), w sin uso. */
int ns_screen_build(const NsScreenFx *fx, const float to_vr[12], const float head_vr[3], float dt, const NsScreenCfg *cfg, NsScreenState *st,
                    NsGpuScreen *out, float fromvr[12]);

/* Inversa de una 3x4 afín (fila-mayor). 0 si es singular. */
int ns_screen_invert34(const float m[12], float out[12]);
void ns_screen_xform34(const float m[12], const float p[3], float out[3]);

/* Máscara de visión de la caja como recubrimiento de ojo (misma máscara 160x128 que calcula vrhost; solo se pide con el jugador en la caja). area_w/area_h = imagen mostrada del motor (píxeles),
 * cx_col/cy_row = centro óptico en ella, H = distancia de pantalla del motor, rgb = color del interior cerrado (0..1). Rellena 2 recubrimientos (resta de la máscara y relleno del interior); devuelve 2 o 0 si los datos no valen. */
int ns_screen_frame_build(float area_w, float area_h, float cx_col, float cy_row, float H, const float rgb[3], NsGpuOverlay out[2]);
/* Referencia de CPU del sombreador: resta (0..1) de la máscara en el punto de tangentes (tx, ty) (ty hacia arriba) con muestreo bilineal; 1 fuera de la imagen original. */
float ns_screen_frame_sub(const uint8_t *frame, int fw, int fh, const NsGpuOverlay *ov, float tx, float ty);
/* Aplica el recubrimiento FRAME ov (p[1] = 0 resta, 1 relleno) con la resta `sub` al color b (0..1). */
void ns_screen_frame_ref(const NsGpuOverlay *ov, float sub, const float b[3], float out[3]);

/* ---- referencias de CPU (lo mismo que ns_fs / ns_fs_overlay) ---- */
/* Aplica un recubrimiento al color de fondo b (0..1) en el punto de tangentes (tx, ty); H = distancia de pantalla del motor. Devuelve el color (sin recortar a 0..1 salvo al final). */
/* ydir: componente vertical (mundo VR, Y arriba) del rayo unitario del píxel; solo lo usa SCAN (las líneas están ancladas al mundo, no giran con la cabeza). RING y BARS son
 * viñetas circulares (invariantes al giro de la cabeza sobre su eje), el resto de recubrimientos son uniformes. */
void ns_screen_overlay_ref(const NsGpuOverlay *ov, float tx, float ty, float ydir, const float b[3], float out[3]);
/* Tinte del agua en el mundo: ew = posición del fragmento en el mundo del motor; col *= tinte. Devuelve 1 si el fragmento está afectado. */
int ns_screen_water_ref(const float wmin[4], const float wmax[4], const float wtint[4], const float ew[3], float col[3]);

/* ---- onda de pantalla bajo el agua (post-proceso del ojo; mismo cálculo que vr_fs_wave de vr_scene.metal) ---- */
#define NS_WAVE_REF_W 1888.0f   /* ancho del ojo (px) al que están dadas las amplitudes de water_wave[0] */
/* Desplazamiento horizontal (px del ojo, el muestreo se corre `dx` a la derecha) de la fila del motor `row` para un ojo de eye_w px de ancho: wwave[0] * eye_w / NS_WAVE_REF_W * sin(wwave[1] * row + wwave[2]). */
float ns_screen_wave_dx(const float wwave[4], float row, float eye_w);
/* Fila del motor (desde el centro óptico, positiva hacia abajo) de la fila de pantalla y_px (centro de píxel = y + 0,5): (y_px - cy_px) / fy_px * H, con cy_px = fila del punto principal y fy_px = focal vertical (px). */
float ns_screen_wave_row(float y_px, float cy_px, float fy_px, float H);
/* Referencia de CPU del post-proceso: src/dst RGB8 de w*h (filas de arriba abajo, distintos); cada fila se muestrea desplazada dx en horizontal con interpolación lineal y borde repetido. */
void ns_screen_wave_rgb8(const float wwave[4], const uint8_t *src, uint8_t *dst, int w, int h, float cy_px, float fy_px, float H);

#ifdef __cplusplus
}
#endif
#endif
