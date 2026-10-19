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
    int32_t weapon;          /* arma VR mostrada (índice del puente), -1 ninguna */
    int32_t mag_rounds, chambered, mag_inserted;   /* arma VR */
    int32_t game_mag, game_reserve;                 /* munición del juego */
    int32_t reload_state;    /* VRR_* de vr_reload.h */
    uint32_t shots;          /* disparos VR desde el arranque */
    uint32_t throws, places, dets, rejects;   /* lanzamientos de granada, colocaciones, detonaciones y eventos rechazados (sin munición, fuera de alcance) */
    uint32_t arms_driven;    /* brazos movidos por la IK desde el arranque */
} MvVrGameStatus;
void mv_vrgame_status(MvVrGameStatus *out);

/* ---- uso interno (vrhost.c) ---- */
struct BridgeState;
void vrgame_reset(void);                       /* al cargar el EXE */
void vrgame_set_enabled(int body_ok, int fire_ok);   /* HLE enlazadas con éxito (sin ellas no se mueven los brazos / no se dispara) */
uint16_t vrgame_pad(uint16_t pad, const struct BridgeState *bs, uint8_t *ram);   /* una vez por VBlank, antes de aplicar el mando */
/* Pruebas (bodyrun): función llamada al principio de vrgame_pad en cada VBlank (hilo de emulación) con el nº de VBlank desde que se instaló,
 * el mando que pidió la app y el puente del VBlank anterior; devuelve el mando a usar. Dentro puede llamar a mv_vrgame_set_hand,
 * mv_vr_set_head, etc.: así las manos sintéticas van sincronizadas con el VBlank y la prueba es determinista. NULL la quita. */
typedef uint16_t (*VrGameDriver)(uint32_t vbl, uint16_t pad, const struct BridgeState *bs);
void vrgame_set_driver(VrGameDriver f);
/* Gestos de cuerpo (vr/input/vr_gesture): golpe, agarre/estrangulamiento, golpear la pared, asomarse. mask: bit 1 golpe, 2 agarre,
 * 4 pared, 8 asomarse (defecto todos). Sin arma equipada y con el jugador al mando; el botón del juego sigue funcionando siempre. */
void mv_vrgame_set_gestures(int32_t enable, int32_t mask);
void vrbodyg_reset(void);
unsigned vrbodyg_event_count(int ev);                                  /* VRG_EV_* acumulados (pruebas) */
uint16_t vrbodyg_pad(uint16_t pad, const struct BridgeState *bs, int right_handed);
int vrgame_hand_raw(int side, float p[3], float *grip);                /* pose del agarre (espacio usuario, m) y grip; 1 si hay seguimiento */
void vrgame_haptic(int side, float strength);                          /* pulso háptico (0..1) a la mano física */
void vrgame_frame_end(void);                   /* una vez por VBlank, tras el puente: publica el antebrazo y el estado */

#ifdef __cplusplus
}
#endif
