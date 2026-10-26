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
    uint32_t refresh_parts;  /* partes del jugador cuyas matrices se releen tras la IK (NS_PARTS_ARMS o NS_PARTS_BODY) */
} NsCtxOut;
typedef void (*NsCtxFn)(NsScene *sc, NsCtxOut *out, void *user);

/* Estadísticas (instantánea; cada campo se lee atómicamente, entre campos puede haber un fotograma de diferencia). */
typedef struct {
    uint32_t captures;       /* llamadas a ns_capture aceptadas (fotogramas de juego) */
    uint32_t published;
    uint32_t replaced;       /* capturas que sustituyeron a una pendiente */
    uint32_t extract_fail;
    uint32_t ik_refresh;     /* escenas a las que se copió la pose IK de los brazos */
    uint32_t player_bound;   /* escenas con el jugador enlazado */
    uint32_t ik_expected;    /* escenas en las que se esperaba la pose IK (jugador enlazado y capa VR con brazos) */
    uint32_t ik_missed;      /* de ellas, las que se publicaron SIN la pose IK (no aptas: se pintan por GPU) */
    uint32_t skipped_gate;   /* capturas omitidas por la compuerta (modo gpu / renderer no listo) */
    uint32_t last_valid;     /* NsScene.valid de la última publicada */
    uint64_t last_pub_ns;
    uint32_t ik_idle;        /* de las esperadas sin pose IK, las que la capa VR dejó INACTIVA (sin pose de mano válida: ningún brazo manejado): aptas, con los brazos de la animación del juego */
} NsHookStats;

void ns_hook_init(NsBridge *b);                         /* NULL desengancha */
void ns_hook_reset(void);                               /* cambio de ejecutable: descarta lo pendiente y los contadores, reinicia el almacén del extractor (ns_hook_set_store_reset) y publica una escena vacía (valid = 0) para que el consumidor no use la anterior */
void ns_hook_set_extractor(NsExtractFn fn, void *user, NsMeshStore *store);   /* fn NULL = sustituto */
int ns_hook_has_extractor(void);
void ns_hook_set_context(NsCtxFn fn, void *user);
/* Compuerta (M1): ns_capture solo extrae si la función devuelve distinto de 0 (modo nativo pedido y renderer listo, o un volcado pendiente en el punto de enganche).
 * NULL = siempre. Se llama en el hilo de emulación en cada etapa: debe ser barata. */
typedef int (*NsGateFn)(void *user);
void ns_hook_set_gate(NsGateFn fn, void *user);
/* Cambio de ejecutable: el extractor debe vaciar su almacén y reiniciar la época (los ids y punteros anteriores dejan de valer). NULL = nada. */
typedef void (*NsStoreResetFn)(NsMeshStore *store, void *user);
void ns_hook_set_store_reset(NsStoreResetFn fn, void *user);
/* ¿La capa de brazos corrió en este fotograma sin manejar ningún brazo (mandos sin seguimiento)? Si sí, la ausencia de pose IK no es un fallo: los brazos de la escena son los de la
 * animación del juego (lo que también dibuja la lista de la GPU) y la escena sale apta. Se llama en ns_hook_tick. NULL = toda ausencia de IK esperada es un fallo. */
typedef int (*NsIkIdleFn)(void *user);
void ns_hook_set_ik_idle(NsIkIdleFn fn, void *user);
/* Observador de la escena (hilo de emulación): se llama con la escena pendiente COMPLETA (tras el contexto y la IK) y su almacén de mallas justo antes de publicarla en
 * ns_hook_tick. Solo lectura, no debe tardar (colisión de la cabeza: vr_headcol alimentada con la geometría 360 grados). NULL = nada. */
typedef void (*NsSceneTapFn)(const NsScene *sc, const NsMeshStore *store, void *user);
void ns_hook_set_scene_tap(NsSceneTapFn fn, void *user);
/* Cierre de la escena (hilo de emulación): en ns_hook_tick, con la escena pendiente COMPLETA (tras el contexto y la IK, solo si salió apta: NS_SV_PLAYER_OK), el mapa del
 * jugador enlazado y el almacén, justo antes del observador y de publicar. Puede añadir instancias propias a la escena (manos nuevas: ns_hand_apply). NULL = nada. */
typedef void (*NsFinalizeFn)(NsScene *sc, NsMeshStore *store, const NsPlayerMap *map, void *user);
void ns_hook_set_finalize(NsFinalizeFn fn, void *user);
void ns_hook_set_frame(uint32_t vblank_count);          /* para NsScene.vblank (si el extractor lo dejó a 0) */

void ns_capture(const uint8_t *ram, const uint8_t *scratch);
void ns_hook_post_ik(const uint8_t *ram);
void ns_hook_tick(const uint8_t *ram);

/* 1 si la última escena publicada es apta (ns_scene_native_ok: partida, listas completas, mapa, to_vr, jugador/IK; sin MAP_INCOMPLETE ni STORE_FULL) y tiene menos de max_age_ns.
 * Solo informativo (estadísticas, registro): la decisión de dibujar nativo la toma el consumidor con ns_native_decide. Se puede llamar desde cualquier hilo. */
int ns_hook_native_ready(uint64_t now_ns, uint64_t max_age_ns);
void ns_hook_stats(NsHookStats *s);

#ifdef __cplusplus
}
#endif
