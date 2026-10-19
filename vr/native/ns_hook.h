#pragma once
/* ns_hook: orquesta la captura de la escena nativa en el hilo de emulación y la publica en el puente (ns_bridge.h). Sin dependencias del runtime:
 * ns_hook_rt.c lo engancha a la etapa «pantalla» del motor; las pruebas lo ejercitan con una RAM falsa.
 *
 * Flujo por fotograma de juego (todo en el hilo de emulación):
 *   1. ns_capture(ram, scratch)       tras la etapa «pantalla» (matrices de mundo calculadas): extrae la escena a una ranura libre del puente
 *                                     (extractor registrado, o el sustituto que solo rellena la cabecera), rellena contexto (to_vr, cámara, jugador) y
 *                                     enlaza el modelo del jugador (ns_player), ocultando su cabeza si procede. NO publica todavía.
 *   2. ns_hook_post_ik(ram)           opcional, tras la IK de los brazos de la capa VR: copia las matrices de los brazos a la escena pendiente.
 *   3. ns_hook_tick(ram)              en el siguiente sincronismo vertical: publica la escena pendiente (release; el consumidor la toma sin bloquear a nadie).
 * Si la etapa corre más de una vez antes del sincronismo la escena pendiente se REEMPLAZA (se cuenta en `replaced`). */
#include <stdint.h>
#include "ns_bridge.h"
#include "ns_player.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Extractor (lo implementa ns_extract). Rellena `out` (ya con capacidades y cabecera a cero, ver ns_bridge_begin) leyendo la RAM; las mallas nuevas
 * deben quedar en su almacén ANTES de volver. Devuelve 0 si fue bien, <0 si falló (la escena no se publica). Debe poner `valid` (NS_SV_*) con verdad. */
typedef int (*NsExtractFn)(const uint8_t *ram, const uint8_t *scratch, NsScene *out, void *user);

/* Contexto del anfitrión tras extraer: to_vr, cámara, zona (rellena directamente `sc`), y qué hacer con el jugador. */
typedef struct {
    uint32_t player_key;     /* conjunto de piezas del jugador (ns_player_key) o 0 */
    int hide_head;           /* 1: ocultar la cabeza propia (primera persona) */
    uint32_t refresh_parts;  /* partes del jugador cuyas matrices se releen tras la IK (NS_PARTS_ARMS) */
} NsCtxOut;
typedef void (*NsCtxFn)(NsScene *sc, NsCtxOut *out, void *user);

typedef struct {
    uint32_t captures;       /* llamadas a ns_capture aceptadas (fotogramas de juego) */
    uint32_t published;
    uint32_t replaced;       /* capturas que sustituyeron a una pendiente */
    uint32_t extract_fail;
    uint32_t ik_refresh;     /* escenas a las que se copió la pose IK de los brazos */
    uint32_t player_bound;   /* escenas con el jugador enlazado */
    uint32_t last_valid;     /* NsScene.valid de la última publicada */
    uint64_t last_pub_ns;
} NsHookStats;

void ns_hook_init(NsBridge *b);                         /* NULL desengancha */
void ns_hook_reset(void);                               /* cambio de ejecutable: descarta lo pendiente y los contadores */
void ns_hook_set_extractor(NsExtractFn fn, void *user, const NsMeshStore *store);   /* fn NULL = sustituto */
int ns_hook_has_extractor(void);
void ns_hook_set_context(NsCtxFn fn, void *user);
void ns_hook_set_frame(uint32_t vblank_count);          /* para NsScene.vblank (si el extractor lo dejó a 0) */

void ns_capture(const uint8_t *ram, const uint8_t *scratch);
void ns_hook_post_ik(const uint8_t *ram);
void ns_hook_tick(const uint8_t *ram);

/* 1 si la última escena publicada es de partida con listas completas y mapa VR (to_vr) y tiene menos de max_age_ns. */
int ns_hook_native_ready(uint64_t now_ns, uint64_t max_age_ns);
void ns_hook_stats(NsHookStats *s);

#ifdef __cplusplus
}
#endif
