#pragma once
/* vrgame: pegamento de jugabilidad VR entre la app, el motor emulado y los módulos de vr/ (cuerpo, radar, manos/arma, disparo y recarga).
 *
 * Hilos:
 *  - HILO DE RENDER / ENTRADA (Swift): publica las manos (mv_vrgame_set_hand), lee el antebrazo para el radar, los hápticos y el estado.
 *  - HILO DE EMULACIÓN: el gancho de mando (vrgame_pad, cada VBlank, justo antes de aplicar el mando al juego) simula el arma VR,
 *    traduce disparo y recarga al mando del juego y publica las mallas del arma; el motor llama a vrpose_get_hands (IK de los brazos)
 *    en la etapa de dibujado.
 *  - HILO DE CARGA (propio): lee los modelos del arma del disco del usuario (archivo de stages) la primera vez que hacen falta.
 *
 * Espacio de las poses: el mismo que la cabeza de mv_vr_set_head (host.h): metros, Y arriba, -Z delante, origen RECENTRADO. La pose
 * que se publica es la del MANDO tal cual la da ARKit (ya multiplicada por la inversa del recentrado); aquí se convierte al marco de
 * la mano de vr/body (centro de la palma, -Z hacia los dedos, +Y al dorso) con un desplazamiento configurable. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- entrada (hilo de render) ---- */
typedef struct {
    int32_t valid;           /* 0 = mando sin seguimiento */
    float pos[3];            /* m, espacio recentrado */
    float rot[4];            /* cuaternión x, y, z, w */
    float trigger, grip;     /* 0..1 */
    int32_t low, up, stick;  /* cara inferior (A/X), superior (B/Y), clic del stick */
} MvVrHand;
/* side: 0 = mando izquierdo, 1 = derecho (físicos). Llamar cada fotograma. */
void mv_vrgame_set_hand(int32_t side, const MvVrHand *h);
void mv_vrgame_clear_hands(void);   /* sin mandos rastreados (mando normal, sin Sense, fuera del espacio inmersivo) */
/* right_handed: mano del arma (1 derecha); reload_mode: 0 manual, 1 simplificada, 2 automática (WReloadMode). */
void mv_vrgame_set_options(int32_t right_handed, int32_t reload_mode);
/* Marco del agarre respecto al del mando: p (m, en el marco del mando) y cuaternión x,y,z,w. Defecto identidad. */
void mv_vrgame_set_grip_offset(const float p[3], const float q[4]);

/* ---- salida (hilo de render) ---- */
typedef struct { float elbow[3], wrist[3], dorsal[3]; } MvVrForearm;   /* mismo layout que VrRadarArm */
/* Antebrazo tras la IK del último VBlank (espacio recentrado, m). 1 = válido; 0 = el llamador usa el mando. */
int32_t mv_vrgame_forearm(int32_t side, MvVrForearm *out);
/* Pose del agarre (marco de vr/body) como matriz 4x4 columna-mayor; 1 si el mando está rastreado. */
int32_t mv_vrgame_hand_matrix(int32_t side, float m[16]);
/* Hápticos pendientes del arma (0..1 por mando físico); se consumen (una lectura = un pulso). */
void mv_vrgame_take_haptics(float *left, float *right);

typedef struct {
    int32_t loader;          /* 0 sin pedir, 1 cargando, 2 listo, 3 fallo */
    int32_t active;          /* arma VR mostrada y mandando */
    int32_t mag_rounds, chambered, mag_inserted;   /* arma VR */
    int32_t game_mag, game_reserve;                 /* munición del juego */
    int32_t reload_state;    /* VRR_* de vr_reload.h */
    uint32_t shots;          /* disparos VR desde el arranque */
    uint32_t arms_driven;    /* brazos movidos por la IK desde el arranque */
} MvVrGameStatus;
void mv_vrgame_status(MvVrGameStatus *out);

/* ---- uso interno (vrhost.c) ---- */
struct BridgeState;
void vrgame_reset(void);                       /* al cargar el EXE */
void vrgame_set_enabled(int body_ok, int fire_ok);   /* HLE enlazadas con éxito (sin ellas no se mueven los brazos / no se dispara) */
uint16_t vrgame_pad(uint16_t pad, const struct BridgeState *bs, uint8_t *ram);   /* una vez por VBlank, antes de aplicar el mando */
void vrgame_frame_end(void);                   /* una vez por VBlank, tras el puente: publica el antebrazo y el estado */

#ifdef __cplusplus
}
#endif
