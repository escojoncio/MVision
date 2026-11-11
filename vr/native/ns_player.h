#pragma once
/* ns_player: el modelo del jugador dentro de la escena nativa. Pega la capa VR (cuerpo IK, cabeza propia oculta) con las instancias de NsScene,
 * por IDENTIDAD de instancia (NsInstance.key = dirección del conjunto de piezas del motor) y no por heurística sobre la lista de la GPU.
 *
 * Por qué hace falta. El extractor lee la RAM al final de la etapa «pantalla» (docs/escena_nativa.md §10). La capa VR escribe DESPUÉS, en la
 * etapa de transformación: (1) la IK de los brazos sustituye world+screen de las piezas de los brazos del jugador (vrbody_apply; se devuelven a su valor
 * al terminar el dibujado), y (2) la cabeza del jugador se oculta con el modo de visibilidad de su pieza (vrcam_hide_head). Ninguna de las dos
 * cosas está en la escena extraída. Aquí:
 *   - ns_player_bind (en la captura, tras extraer): localiza la instancia del jugador y, por cada pieza del modelo del motor (partes 0..15), qué NsPiece
 *     de la escena la representa (las que tienen la MISMA matriz de mundo EXACTA que la RAM; una parte puede dar varias NsPiece: mallas encadenadas).
 *   - ns_player_hide_part: marca esas piezas NS_PF_HIDDEN (cabeza propia en primera persona).
 *   - ns_player_refresh (después de vrbody_apply): vuelve a leer de la RAM la matriz de mundo de las partes pedidas (brazos) y la copia a sus NsPiece,
 *     de modo que cuerpo, manos y brazos quedan en el mismo espacio del mundo del motor que el resto de la escena (y por tanto bajo la misma to_vr).
 * Sin dependencias del runtime: recibe la RAM como puntero. Disposición del modelo (vr/body/vr_body.h): conjunto de piezas en *(*(0x800ACBB0)),
 * s16 número de piezas en +0x2E, piezas de 0x5C bytes desde +0x48, matriz de mundo en +0x00 de cada una (s16 m[9] con 4096 = 1, s32 t[3] en +0x14). */
#include <stdint.h>
#include "ns_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NS_PLAYER_BODY_PTR 0x800ACBB0u   /* = BA_PLAYER_BODY (vr/bridge/bridge_addrs.h): puntero al objeto del jugador, cuyo primer campo apunta al conjunto de piezas */
#define NS_PLAYER_PARTS 16
#define NS_PLAYER_MAXPC 4                /* NsPiece por parte */
#define NS_PART_HEAD 6
#define NS_PARTS_ARMS ((1u << 2) | (1u << 3) | (1u << 4) | (1u << 7) | (1u << 8) | (1u << 9))   /* brazo/antebrazo/mano derechos e izquierdos */
#define NS_PARTS_BODY (NS_PARTS_ARMS | (1u << 0) | (1u << 1) | (1u << 5) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13) | (1u << 14) | (1u << 15))   /* + pelvis, torso, cuello y piernas (torso_follow) */


typedef struct {
    int bound;                           /* 1 si se encontró la instancia del jugador */
    int inst;                            /* índice en NsScene.inst */
    uint32_t key;                        /* conjunto de piezas del jugador (NsInstance.key) */
    struct { uint8_t n; uint32_t idx[NS_PLAYER_MAXPC]; } part[NS_PLAYER_PARTS];   /* NsPiece (índice global en NsScene.piece) de cada parte */
} NsPlayerMap;

/* Oculta (NS_PF_HIDDEN) las instancias PEGADAS a la cabeza del jugador: objetos cuyo puntero de matriz padre (+0x20) es la matriz de mundo de la pieza NS_PART_HEAD de `key`. Son las cabezas
 * de los objetos equipados (gafas térmicas / de visión nocturna, máscara de gas): el motor retira la cabeza del modelo y cuelga ahí otro objeto con la cabeza equipada. En primera persona esa
 * cabeza rodea el ojo igual que la propia. Devuelve cuántas instancias ocultó. */
int ns_player_hide_head_attached(NsScene *sc, const uint8_t *ram, uint32_t key);

/* Conjunto de piezas del jugador (clave de instancia) leído de la RAM; 0 si no hay jugador / punteros inválidos. */
uint32_t ns_player_key(const uint8_t *ram);
/* Índice de la instancia con esa clave o -1. */
int ns_player_find(const NsScene *sc, uint32_t key);
/* Empareja partes con piezas por matriz de mundo IDÉNTICA (mismos s16/s32 del motor, sin tolerancia); una pieza que coincide con más de una parte es ambigua y no se enlaza a ninguna.
 * Empareja partes con piezas. Devuelve cuántas partes quedaron enlazadas (0 = nada: jugador no está en la escena o su modelo no coincide con la RAM). */
int ns_player_bind(NsPlayerMap *m, const NsScene *sc, const uint8_t *ram, uint32_t key);
/* Marca ocultas las piezas de la parte. Devuelve cuántas. */
int ns_player_hide_part(const NsPlayerMap *m, NsScene *sc, int part);
/* Copia a la escena la matriz de mundo actual de las partes de `part_mask` (bit k = parte k). Devuelve cuántas partes se actualizaron. */
int ns_player_refresh(const NsPlayerMap *m, NsScene *sc, const uint8_t *ram, uint32_t part_mask);

#ifdef __cplusplus
}
#endif
