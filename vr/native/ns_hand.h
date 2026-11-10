#pragma once
/* ns_hand: manos propias en la escena nativa (primera persona).
 *
 * La mano del personaje del motor es UNA malla rígida (manopla). Aquí se oculta y, en su lugar, se añade a la escena una instancia propia por mano con la mano de vr_hand_skin (mano
 * genérica de WebXR Input Profiles con skin lineal sobre el esqueleto OpenXR de 25 articulaciones, ~1400 triángulos, a la escala del brazo del juego): la pose de los dedos se aplica
 * en la CPU y la malla se reparte en VRHA_NP piezas de <= 256 vértices (NsFace usa índices de 8 bits), con la matriz de la pieza mano del motor (ya con la IK de la capa VR).
 * Las mallas se guardan por pose CUANTIZADA (cierres a pasos de 1/32): mientras los dedos no cambian no se crea ninguna malla nueva aunque la mano se mueva.
 *
 * UNIÓN con el antebrazo del motor: el asset se corta en la muñeca (vrha_cuff = su borde) y ns_hand genera la transición. Pieza 0 = anillo: los vértices del lazo abierto del extremo
 * del antebrazo copiados tal cual, con la matriz del antebrazo (una cara degenerada, no se ve). Pieza 1 = transición, en el marco de la mano: su primer anillo se SUELDA a esos
 * vértices (posiciones idénticas a las del antebrazo), el último son los vértices del borde de la muñeca de la mano (mismas coordenadas, mismo marco) y los intermedios llevan una
 * parte creciente del movimiento del antebrazo respecto a la mano (giro interpolado: giro y flexión repartidos, sin estrangulamiento). Antebrazo + transición + mano = una superficie
 * cerrada. La transición se recalcula en cada fotograma (depende de la postura de la muñeca; ~100 vértices); la topología solo si cambia la malla del antebrazo. Sin antebrazo: sin unión.
 * El antebrazo del motor se sustituye por una copia con el giro REPARTIDO a lo largo (0 en el codo, soldado al brazo; todo en la muñeca): sin retorcer la franja del codo.
 *
 * ESTILO PS1 como el brazo: la cara palmar desde la muñeca hasta la base de los dedos lleva la textura de la palma de la manopla del juego (tpage/clut/UV de su cara, proyectada en plano)
 * y la cara dorsal, también hasta la base de los dedos, la del dorso de la manopla (con su recuadro gris), cada texel en la posición que tiene en la manopla;
 * dedos, pulgar y costados van de un solo color (el texel central de la textura de los dedos de la manopla) con sombreado de la luz del jugador. Sin texturas utilizables, plana.
 * Iluminación, ambiente, niebla y zona: los de la instancia del jugador. Si algo falla (sin manopla enlazada, sin malla, escena sin sitio, almacén lleno) NO se oculta la manopla.
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
#define NS_HAND_PIECES 6                /* piezas por mano como mucho: anillo + transición + VRHA_NP (= 4; comprobado en ns_hand.c). Sin incluir vr_hand_asset.h: ns_adapt y ns_solid incluyen este fichero solo con vr/native */
#define NS_PART_HAND_R 4                 /* partes del modelo del jugador (ns_player) */
#define NS_PART_HAND_L 9
#define NS_PART_FORE_R 3
#define NS_PART_FORE_L 8
#define NS_PART_UPPER_R 2
#define NS_PART_UPPER_L 7
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
    uint32_t no_room;            /* escena sin capacidad para 1 instancia + NS_HAND_PIECES piezas */
    uint32_t put_fail;           /* almacén lleno */
    uint32_t rebuilt;            /* reconstrucciones (cambio de manopla o de zona) */
    uint32_t flat;               /* manos construidas sin ninguna textura utilizable */
    uint32_t last_applied;       /* bit 0 izquierda, bit 1 derecha, en la última llamada */
    uint32_t skinned;            /* poses nuevas calculadas (mallas creadas); las repetidas reutilizan las anteriores */
    uint32_t seams;              /* costuras con el antebrazo (re)calculadas */
    uint32_t fore_twisted;       /* antebrazos sustituidos por la copia con el giro repartido */
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
    int16_t y[4], z[4];                  /* posición de A,B,C,D en el marco de la manopla (largo y ancho de la mano) */
} NsHandTex;
enum { NS_HT_PALM = 0, NS_HT_BACK = 1, NS_HT_FINGER = 2, NS_HT_JOINT = 3 };
/* tex[4] por rol; devuelve cuántos roles encontró con cara propia (el resto se rellena con el más parecido o queda plano). side +1 derecha / -1 izquierda. */
int ns_hand_classify(const NsMesh *glove, int side, NsHandTex tex[4]);


#ifdef __cplusplus
}
#endif
