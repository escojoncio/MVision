#pragma once
/* ns_hand: manos propias en la escena nativa (primera persona).
 *
 * La mano del personaje del motor es UNA malla rígida (manopla). Aquí se oculta y, en su lugar, se añaden a la escena dos instancias propias (una por mano) con la mano de
 * vr_hand_skin (mano genérica de WebXR Input Profiles, skin lineal sobre el esqueleto OpenXR de 25 articulaciones, ~1400 triángulos a tamaño real): la pose de los dedos se aplica en la
 * CPU y la malla resultante se reparte en VRHA_NP piezas de <= 256 vértices (NsFace usa índices de 8 bits), todas con la matriz de la pieza mano del motor (ya con la IK de la capa VR,
 * mismo espacio que el resto de la escena). Las mallas se guardan por pose CUANTIZADA (cierres a pasos de 1/32, escala a pasos de 1/256): mientras los dedos no cambian no se crea
 * ninguna malla nueva, aunque la mano se mueva (solo cambia la matriz). Iluminación, ambiente, niebla y zona: los de la instancia del jugador.
 *
 * Tamaño real: la mano mide ~190 mm de la muñeca a la yema del corazón; en unidades del motor se dibuja a mm * units_per_m / 1000 (NsHandIn.units_per_m, la escala calibrada).
 * Antebrazo: el del motor (pieza NS_PART_FORE_*) es mucho más grueso que el puño del guante; mientras la mano nueva está puesta se sustituye su malla por una copia ADELGAZADA
 * (vértices no soldados escalados hacia su eje hasta un radio ~15 % mayor que el del puño del guante, factor 0,6..1); el extremo del codo va soldado al brazo y no cambia.
 *
 * Texturas: sin datos propios. Cada triángulo toma tpage/clut/texid y un trozo del rectángulo UV de una cara de la manopla del motor según su rol (palma/yemas, dorso, costados,
 * puntas; clasificadas por orientación en la manopla); la posición del vértice en reposo, normalizada, elige el punto del rectángulo. Si no hay ninguna cara con textura utilizable,
 * la mano sale plana (sombreada por la luz del jugador). Si algo falla (sin manopla enlazada, sin malla, escena sin sitio, almacén lleno) NO se oculta la manopla.
 *
 * Todo en el hilo de emulación (se llama desde el cierre de ns_hook_tick). */
#include <stdint.h>
#include "ns_types.h"
#include "ns_player.h"
#include "vr_hand_asset.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NS_HAND_KEY_R 0x4D480001u        /* NsInstance.key de las manos propias: no es una dirección de RAM del motor (0x80......), no choca con ninguna instancia */
#define NS_HAND_KEY_L 0x4D480002u
#define NS_HAND_PIECES VRHA_NP          /* piezas por mano */
#define NS_PART_HAND_R 4                 /* partes del modelo del jugador (ns_player) */
#define NS_PART_HAND_L 9
#define NS_PART_FORE_R 3
#define NS_PART_FORE_L 8
static inline int ns_key_is_hand(uint32_t k) { return k == NS_HAND_KEY_R || k == NS_HAND_KEY_L; }   /* ns_solid y ns_adapt las omiten como al cuerpo propio */

typedef struct {
    int enabled;                 /* 0: no tocar la escena (manos originales) */
    float grip[2];               /* agarre efectivo 0..1, índice 0 = izquierda, 1 = derecha (como vrbody_set_grip) */
    int driven[2];               /* la capa VR llevó ese brazo en este fotograma (si no, mano relajada en la pose de la animación del juego) */
    int item_pose[2];            /* HP_* de vr_hand_pose.h: pose del objeto en la mano (HP_RELAX = sin objeto; ojo: el cero de un NsHandIn a cero es HP_OPEN) */
    float trigger[2];            /* gatillo analógico 0..1: solo cierra el índice si la pose lleva gatillo (vrhandpose_has_trigger) */
    float thumb[2];              /* pulgar sobre un botón de cara 0..1 */
    float units_per_m;           /* unidades del motor por metro real (escala calibrada de la cámara VR); <= 0 = 1000 */
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
    uint32_t slimmed;            /* antebrazos adelgazados */
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
/* Factor de adelgazamiento del antebrazo (0,6..1) para una malla de antebrazo y la escala de la mano (k = units_per_m / 1000); 1 = no hace falta. Visible para las pruebas. */
float ns_hand_fore_slim(const NsMesh *fore, float k);

#ifdef __cplusplus
}
#endif
