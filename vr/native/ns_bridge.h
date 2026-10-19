#pragma once
/* ns_bridge: capa fina sobre ns_pub.h (triple búfer sin bloqueos del contrato v2) con la API que usa el renderer Metal. Las 3 NsScene las reserva
 * mv_native_init; las mallas NO viajan aquí: van por el almacén (NsMeshStore, ns_store_get / ns_store_get_baked / ns_store_ack de ns_types.h).
 *
 * Productor:   mv_native_init() una vez; por fotograma de juego: (1) añadir al almacén TODAS las mallas y colores que la escena referencia;
 *              (2) sc = mv_native_begin() (escena libre con n_* = 0, size/version/slot_gen y arrays con sus cap_*); (3) rellenar sc (no pasar de las
 *              capacidades: marcar `truncated`); (4) mv_native_publish(sc, store) (release; asigna seq 1, 2, 3...).
 * Consumidor:  sc = mv_native_acquire(&store): la escena más reciente (acquire), estable hasta la SIGUIENTE llamada a mv_native_acquire (no hay que soltarla;
 *              mv_native_release es un vacío por compatibilidad). NULL si aún no hay ninguna o si size/version no coinciden con los del consumidor.
 *              Tras subir a la GPU las mallas de la época de la escena, mv_native_ack(store, sc->mesh_epoch) deja que el productor libere la época anterior.
 *              mv_native_seq() dice si hay una escena nueva sin tomarla.
 * Un productor y un consumidor concurrentes; el consumidor es UN solo hilo. Accesores ns_scene_* para Swift (los arrays fijos de C llegan como tuplas). */
#include <stdint.h>
#include "ns_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reserva las 3 escenas con esas capacidades (0 = las mínimas del contrato NS_MIN_*: 256 instancias, 2048 piezas, 256 luces, 4096 efectos). 0 = bien, -1 = sin memoria.
 * Volver a llamarla libera lo anterior (el llamador garantiza que nadie usa escenas). */
int mv_native_init(uint32_t cap_inst, uint32_t cap_piece, uint32_t cap_light, uint32_t cap_effect);
void mv_native_shutdown(void);

/* Productor */
NsScene *mv_native_begin(void);
void mv_native_publish(NsScene *sc, const NsMeshStore *store);

/* Consumidor */
const NsScene *mv_native_acquire(const NsMeshStore **store);
void mv_native_release(const NsScene *sc);
void mv_native_ack(const NsMeshStore *store, uint32_t epoch);
uint32_t mv_native_seq(void);

/* Accesores para Swift */
static inline const float *ns_scene_to_vr(const NsScene *s) { return s->to_vr; }
static inline const float *ns_scene_cam_rot(const NsScene *s) { return s->cam.rot; }
static inline const float *ns_scene_cam_eye(const NsScene *s) { return s->cam.eye; }
static inline const float *ns_xf_m(const NsXf *x) { return x->m; }
static inline const int16_t *ns_light_dir(const NsLight *l) { return &l->dir[0][0]; }
static inline const int16_t *ns_light_col(const NsLight *l) { return &l->col[0][0]; }

#ifdef __cplusplus
}
#endif
