#pragma once
/* ns_hand: manos articuladas propias en la escena nativa (primera persona).
 *
 * La mano del personaje del motor es UNA malla rígida (manopla). Aquí se oculta y, en su lugar, se añaden a la escena dos instancias propias (una por mano) con las mallas
 * de vr_hand_mesh: 16 huesos rígidos = 16 NsPiece por mano, con el anillo proximal de cada hueso soldado (NsVert.weld, NsPiece.parent = hueso padre) al distal del padre: sin huecos en las articulaciones. Cada pieza lleva la matriz de la pieza mano del motor (ya con la IK de la capa VR, mismo espacio que el resto
 * de la escena y por tanto bajo la misma to_vr) compuesta con la pose del hueso para el agarre. Iluminación, ambiente, niebla y zona: los de la instancia del jugador.
 *
 * Texturas: sin datos propios. Cada cara de la mano nueva toma tpage/clut/texid y un trozo del rectángulo UV de una cara de la manopla del motor (se clasifican por
 * orientación en palma, dorso, dedo y yema/articulación). Si no hay ninguna cara con textura utilizable, la mano sale plana (sombreada por la luz del jugador).
 * Si algo falla (sin manopla enlazada, sin malla, escena sin sitio, almacén lleno) NO se oculta la manopla: se sigue viendo la del motor.
 *
 * Todo en el hilo de emulación (se llama desde el cierre de ns_hook_tick). */
#include <stdint.h>
#include "ns_types.h"
#include "ns_player.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NS_HAND_KEY_R 0x4D480001u        /* NsInstance.key de las manos propias: no es una dirección de RAM del motor (0x80......), no choca con ninguna instancia */
#define NS_HAND_KEY_L 0x4D480002u
#define NS_HAND_BONES 16
#define NS_PART_HAND_R 4                 /* partes del modelo del jugador (ns_player) */
#define NS_PART_HAND_L 9
static inline int ns_key_is_hand(uint32_t k) { return k == NS_HAND_KEY_R || k == NS_HAND_KEY_L; }   /* ns_solid y ns_adapt las omiten como al cuerpo propio */

typedef struct {
    int enabled;                 /* 0: no tocar la escena (manos originales) */
    float grip[2];               /* agarre efectivo 0..1, índice 0 = izquierda, 1 = derecha (como vrbody_set_grip) */
    int driven[2];               /* la capa VR llevó ese brazo en este fotograma (si no, mano relajada en la pose de la animación del juego) */
    int item_pose[2];            /* HP_* de vr_hand_pose.h: pose del objeto en la mano (HP_RELAX = sin objeto; ojo: el cero de un NsHandIn a cero es HP_OPEN) */
    float trigger[2];            /* gatillo analógico 0..1: solo cierra el índice si la pose lleva gatillo (vrhandpose_has_trigger) */
    float thumb[2];              /* pulgar sobre un botón de cara 0..1 */
} NsHandIn;

typedef struct {
    uint32_t frames;             /* llamadas con la función activa */
    uint32_t applied;            /* manos añadidas */
    uint32_t no_part;            /* manopla no enlazada en la escena */
    uint32_t no_mesh;            /* malla de la manopla ausente del almacén o inservible */
    uint32_t no_room;            /* escena sin capacidad para 1 instancia + 16 piezas */
    uint32_t put_fail;           /* almacén lleno */
    uint32_t rebuilt;            /* reconstrucciones (cambio de manopla o de zona) */
    uint32_t flat;               /* manos construidas sin ninguna textura utilizable */
    uint32_t last_applied;       /* bit 0 izquierda, bit 1 derecha, en la última llamada */
} NsHandStats;

/* Añade las manos nuevas a la escena `sc` y oculta las manopla del motor. Devuelve cuántas manos se pusieron (0..2). map: jugador enlazado (ns_player_bind). */
int ns_hand_apply(NsScene *sc, NsMeshStore *store, const NsPlayerMap *map, const NsHandIn *in);
void ns_hand_reset(void);                 /* olvida las mallas construidas (cambio de ejecutable) y los contadores */
void ns_hand_stats(NsHandStats *out);

/* Clasificación de texturas de la manopla (visible para las pruebas). */
typedef struct {
    int ok;                              /* hay cara de referencia */
    uint16_t tpage, clut, texid;
    uint8_t u[4], t[4];                  /* rectángulo A,B,C,D en texels */
} NsHandTex;
enum { NS_HT_PALM = 0, NS_HT_BACK = 1, NS_HT_FINGER = 2, NS_HT_JOINT = 3 };
/* tex[4] por rol; devuelve cuántos roles encontró con cara propia (el resto se rellena con el más parecido o queda plano). side +1 derecha / -1 izquierda. */
int ns_hand_classify(const NsMesh *glove, int side, NsHandTex tex[4]);

#ifdef __cplusplus
}
#endif
