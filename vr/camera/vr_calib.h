/* vr_calib: calibración de altura y escala del usuario (sentado o de pie) al recentrar.
 *
 * La cabeza del usuario al recentrar (p_ref) se ancla al ojo del personaje (la altura física de la cabeza en ese instante
 * pasa a ser la altura del ojo del personaje: sentado o de pie da igual, el suelo del juego queda a la altura de ojos del
 * personaje bajo la cabeza). Además se calcula una ESCALA de correspondencia usuario -> mundo para que el alcance real de
 * los brazos (hombro -> mano, proporcional a la altura del usuario) se corresponda con el del brazo del personaje (IK de dos
 * huesos): s = s0 * ref_eye / user_eye, acotada. s multiplica "unidades del motor por metro" de todo lo que sale del espacio
 * de usuario (traslación de la cabeza, manos, puntería); la separación de los ojos (IPD) NO se escala.
 *
 * Altura del usuario de pie: si se conoce (ajuste), esa; si no, la de la cabeza al recentrar cuando está de pie, o, sentado
 * (cabeza por debajo de `seated_below_m`), la de la cabeza dividida por `seat_ratio` (altura de ojos sentado / de pie). */
#pragma once
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VrCalibCfg {
    float seated_below_m;   /* cabeza por debajo de esta altura al recentrar = sentado (defecto 1,30) */
    float seat_ratio;       /* altura de ojos sentado / de pie (defecto 0,63) */
    float user_eye_m;       /* altura de ojos de pie conocida (m); 0 = estimar */
    float ref_eye_m;        /* altura de ojos de referencia para la que s = s0 (defecto 1,65) */
    float s0;               /* escala para un usuario de altura de referencia (defecto 0,96) */
    float s_min, s_max;     /* límites de la escala (defecto 0,75 y 1,10) */
} VrCalibCfg;

typedef struct VrCalib {
    int seated;
    float head_y_m;         /* altura de la cabeza al recentrar */
    float user_eye_m;       /* altura de ojos de pie usada (conocida o estimada) */
    float scale;            /* escala usuario -> mundo (1 = sin escala) */
} VrCalib;

/* Solo cabecera (static inline): no añade ficheros a los enlaces de cámara/cuerpo/host.
 * head_y: altura de la cabeza al recentrar (m). No finita/fuera de rango -> escala 1, de pie. */


static inline void vrcalib_cfg_default(VrCalibCfg *c) {
    c->seated_below_m = 1.30f; c->seat_ratio = 0.63f; c->user_eye_m = 0.0f;
    c->ref_eye_m = 1.65f; c->s0 = 0.96f; c->s_min = 0.75f; c->s_max = 1.10f;
}

static inline void vrcalib_compute(const VrCalibCfg *cfg, float head_y, VrCalib *o) {
    VrCalibCfg d; if (!cfg) { vrcalib_cfg_default(&d); cfg = &d; }
    o->seated = 0; o->head_y_m = 0.0f; o->user_eye_m = cfg->ref_eye_m; o->scale = 1.0f;
    if (!isfinite(head_y) || head_y < 0.3f || head_y > 2.6f) return;
    o->head_y_m = head_y;
    o->seated = head_y < cfg->seated_below_m;
    float u;
    if (cfg->user_eye_m >= 1.2f && cfg->user_eye_m <= 2.3f) u = cfg->user_eye_m;
    else if (o->seated) u = head_y / (cfg->seat_ratio > 0.3f ? cfg->seat_ratio : 0.63f);
    else u = head_y;
    if (u < 1.35f) u = 1.35f;
    if (u > 2.1f) u = 2.1f;
    o->user_eye_m = u;
    float s = cfg->s0 * cfg->ref_eye_m / u;
    if (s < cfg->s_min) s = cfg->s_min;
    if (s > cfg->s_max) s = cfg->s_max;
    o->scale = s;
}

#ifdef __cplusplus
}
#endif
