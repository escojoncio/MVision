#pragma once
/* ns_bridge: puente productor (hilo de emulación, ~25 escenas/s) -> consumidor (render a 100 Hz) de la escena nativa.
 * C11 puro (atómicos), sin dependencias del runtime. Contrato de datos: ns_types.h.
 *
 * DISEÑO (SPSC, sin bloqueos, sin asignaciones en caliente)
 *   (El contrato v2 trae un triple búfer mínimo, ns_pub.h; este puente es de 4 ranuras porque el consumidor necesita DOS escenas retenidas a la vez para
 *   interpolar y el productor otra para escribir. Mismo protocolo release/acquire y mismo slot_gen.)
 *   4 ranuras NsScene preasignadas en ns_bridge_create (con las capacidades pedidas). Un único atómico de 12 bits reparte las ranuras:
 *     buzón (la última publicada que el consumidor aún no tomó) | actual | anterior.
 *   El consumidor necesita DOS escenas a la vez (anterior y actual, para interpolar entre escenas del juego) y el productor otra para
 *   escribir: por eso 4 (con 3 un productor rápido pisaría la anterior). El productor nunca espera: si el consumidor no toma, la publicación
 *   nueva sustituye a la del buzón (se cuenta en `dropped`). El consumidor nunca espera: si no hay nada nuevo repite actual/anterior.
 *   Orden de memoria: publicar = CAS con release; tomar = CAS con acquire (las escrituras de la ranura quedan visibles).
 *
 * USO
 *   productor (un solo hilo):   NsScene *s = ns_bridge_begin(b);  ...rellenar s (arrays hasta cap_*, n_* a mano)...  ns_bridge_publish(b, store);
 *                               begin es idempotente mientras no se publique (se reutiliza la misma ranura: abandonar una escena a medias es seguro).
 *   consumidor (un solo hilo):  NsView v; if (ns_bridge_acquire(b, ns_now_ns(), &v)) { NsInterp ip; ns_interp_begin(b, &v, &ip); ... }
 *                               v.cur / v.prev (y lo que cuelga de ellos) valen HASTA LA SIGUIENTE llamada a ns_bridge_acquire.
 *
 * INTERPOLACIÓN
 *   El juego publica ~25 escenas/s con marca de tiempo (NsScene.t_ns, CLOCK_MONOTONIC, ns_now_ns). El consumidor dibuja a 100 Hz en el tiempo
 *   de pantalla T = ahora - retardo (retardo = periodo estimado de las escenas, acotado 20..100 ms): alpha = (T - t_prev) / (t_cur - t_prev) en
 *   [0, 1 + extrapolación] (defecto 0,25). Con alpha 0 se dibuja `prev` y con 1 `cur`. Las instancias se emparejan por NsInstance.key y las piezas por
 *   posición dentro de la instancia (misma malla); lo que no empareja o se teletransporta (> NS_TELEPORT_UNITS) se dibuja tal cual `cur`.
 *   También se interpola to_vr (el origen del mundo VR sigue al ojo del motor: sin ello el mundo "salta" 25 veces por segundo al andar).
 *   Una discontinuidad (cambio de zona/malla, hueco > 400 ms, salto de cámara) desactiva la interpolación (interp = 0, alpha = 1).
 *
 * SEMÁNTICA DE LOS CAMPOS que rellena el puente/enganche (ns_hook.c) y no el extractor: seq, version, t_ns (si 0), game_frame (si 0: fotogramas de juego contados
 * por el enganche), vblank (si 0: hle_frame_count al capturar), to_vr y NsScene.cam cuando hay cámara VR (NS_SV_TOVR_OK). */
#include <stddef.h>
#include <stdint.h>
#include "ns_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NS_SV_TOVR_OK y NS_SV_PLAYER_OK (NsScene.valid) están en ns_types.h. */

#define NS_BRIDGE_SLOTS 4
#define NS_TELEPORT_UNITS 4000.0f   /* salto de traslación de una pieza entre escenas consecutivas que no se interpola (4 m del motor) */
#define NS_DISCONT_NS 400000000ull  /* hueco entre escenas a partir del cual no se interpola */

typedef struct NsBridge NsBridge;

typedef struct {
    uint32_t cap_inst, cap_piece, cap_light, cap_effect;   /* 0 = valor por defecto (512 / 4096 / 256 / 1024) */
} NsBridgeCaps;

typedef struct {
    uint32_t published;     /* escenas publicadas */
    uint32_t dropped;       /* publicadas y sustituidas en el buzón antes de que el consumidor las tomara */
    uint32_t acquired;      /* escenas nuevas tomadas por el consumidor */
    uint32_t polls;         /* llamadas a ns_bridge_acquire */
    double period_ms;       /* periodo estimado entre escenas (consumidor) */
} NsBridgeStats;

/* Reloj monótono en ns (el mismo para t_ns del productor y el `now` del consumidor). */
uint64_t ns_now_ns(void);

/* NULL si no hay memoria. Todo el almacenamiento se reserva aquí. */
NsBridge *ns_bridge_create(const NsBridgeCaps *caps);
void ns_bridge_destroy(NsBridge *b);
/* Solo antes de usar el puente (o desde el consumidor): retardo = periodo * lead_factor (defecto 1,0), extrapolación máxima como fracción del intervalo (defecto 0,25). */
void ns_bridge_set_interp(NsBridge *b, float lead_factor, float max_extrap);
void ns_bridge_stats(const NsBridge *b, NsBridgeStats *s);

/* ---- productor ---- */
/* Ranura libre con la CABECERA de la escena a cero (los arrays de instancias/piezas/luces/efectos se reutilizan SIN limpiar: quien rellena escribe todos los campos) salvo: version, cap_*, punteros de arrays (n_* = 0), units_per_m = 1000. Nunca falla ni espera. */
NsScene *ns_bridge_begin(NsBridge *b);
/* ORDEN: las mallas que la escena referencia deben estar ya completas en el almacén ANTES de llamar a publish (publish es la barrera release; el consumidor
 * adquiere con acquire en ns_bridge_acquire). Publica la escena de begin: seq = anterior + 1, t_ns = ahora si era 0. `store` = almacén de mallas del productor (puntero estable; puede ser NULL). */
void ns_bridge_publish(NsBridge *b, NsMeshStore *store);
/* 1 si hay una escena empezada con begin y aún sin publicar. */
int ns_bridge_pending(const NsBridge *b);
/* La escena empezada (o NULL). */
NsScene *ns_bridge_current(NsBridge *b);

/* ---- consumidor ---- */
typedef struct {
    const NsScene *cur;     /* la más reciente publicada */
    const NsScene *prev;    /* la anterior (NULL si solo ha llegado una) */
    NsMeshStore *store;   /* almacén de cur (no const: ns_store_ack) */
    float alpha;            /* 0..1+extrap entre prev (0) y cur (1); 1 si no hay prev o hay discontinuidad */
    int interp;             /* 1 si hay prev y es continua con cur (se puede interpolar) */
    int fresh;              /* 1 si esta llamada tomó una escena nueva */
    uint32_t cur_gen, prev_gen;   /* NsScene.slot_gen de cada una al tomarla (contrato v2): comprobar que no cambió tras copiar */
} NsView;
/* Devuelve 0 si todavía no hay ninguna escena; 1 en otro caso. `now_ns` en la base de ns_now_ns (se pasa para poder simular el tiempo en pruebas). */
int ns_bridge_acquire(NsBridge *b, uint64_t now_ns, NsView *out);

/* RECONOCIMIENTO DE ÉPOCA (contrato v2): el consumidor llama a ns_store_ack(view.store, escena->mesh_epoch) cuando ya no usa mallas de épocas anteriores
 * (p. ej. tras subir a la GPU las de la escena). El puente no lo hace por él: el almacén es del extractor (ns_types.h, «Entrega»). */

/* ---- decisión nativo/GPU (consumidor) ----
 * La decisión se toma AL DIBUJAR, con el estado real de ese fotograma; el productor de la lista de la GPU no quita nada: marca los batches 3D
 * (VrBatch.skip_if_native: VR_SKIP_WORLD en kind 0, VR_SKIP_FX en kind 1) y pone VrScene.native_3d = 1 si la escena admite el 3D nativo.
 * Uso en el renderer:
 *     NsView v; NsDecision d;
 *     if (ns_bridge_acquire(b, now, &v)) {
 *         NsDecideIn in = { want_native, renderer_ok, meshes_uploaded, scene->native_3d, now, 250000000ull };
 *         ns_native_decide(&v, &in, &d);
 *     } else d = (NsDecision){0};
 *     if (d.native && !draw_native(&v)) ns_native_fail(&d);          // el render nativo cayó a mitad: este mismo fotograma se pinta todo por GPU
 *     for each batch: if (!(batch.skip_if_native & d.skip)) draw(batch);   // d.skip == 0 => se dibujan TODOS los batches
 * Nunca queda un fotograma sin 3D: si algo falta, d.native = 0 y d.skip = 0. */
enum { NS_SKIP_WORLD = 1, NS_SKIP_FX = 2 };   /* = VR_SKIP_WORLD / VR_SKIP_FX (vr_scene.h) */
enum { NS_WHY_OK = 0, NS_WHY_MODE, NS_WHY_RENDERER, NS_WHY_MESHES, NS_WHY_UNMARKED, NS_WHY_NOSCENE, NS_WHY_STALE, NS_WHY_INVALID, NS_WHY_FAILED };
typedef struct {
    int want_native;        /* modo de render pedido (menú / MV_RENDER) */
    int renderer_ok;        /* el renderer nativo está operativo en este fotograma */
    int meshes_ok;          /* las mallas de la escena actual están subidas a la GPU */
    int scene_marked;       /* VrScene.native_3d de la escena de la lista que se va a dibujar */
    uint64_t now_ns;        /* base de ns_now_ns */
    uint64_t max_age_ns;    /* antigüedad máxima de la escena nativa (defecto recomendado 250 ms) */
} NsDecideIn;
typedef struct {
    int native;             /* 1: dibujar la escena nativa */
    uint32_t skip;          /* NS_SKIP_*: batches marcados que NO se dibujan (0 si !native) */
    int why;                /* NS_WHY_*: motivo de native == 0 */
} NsDecision;
/* Bits mínimos de NsScene.valid para dibujar nativo, y los que lo impiden. */
#define NS_NATIVE_NEED (NS_SV_GAME | NS_SV_LISTS_OK | NS_SV_MAP_OK | NS_SV_TOVR_OK | NS_SV_PLAYER_OK)
#define NS_NATIVE_BAD  (NS_SV_MAP_INCOMPLETE | NS_SV_STORE_FULL)
int ns_scene_native_ok(uint32_t valid);
int ns_native_decide(const NsView *v, const NsDecideIn *in, NsDecision *out);
/* El render nativo falló a mitad de fotograma: vuelve a GPU en ese mismo fotograma (native = 0, skip = 0, why = NS_WHY_FAILED). */
void ns_native_fail(NsDecision *d);

/* ---- interpolación (consumidor) ---- */
typedef struct {
    const NsScene *prev, *cur;
    float alpha;
    int enabled;                /* 0: usar siempre cur */
    const int32_t *map;         /* map[i] = índice en prev de la instancia i de cur (por key), -1 si no hay; NULL si !enabled. Memoria del puente: vale hasta la siguiente llamada a ns_interp_begin */
} NsInterp;
void ns_interp_begin(NsBridge *b, const NsView *v, NsInterp *out);
/* Matriz local->mundo interpolada de la pieza k de la instancia i de cur. Devuelve 1 si se interpoló, 0 si es la de cur sin cambio. */
int ns_interp_piece(const NsInterp *ip, uint32_t inst, uint32_t k, NsXf *out);
/* to_vr interpolado (3x4 fila-mayor). */
void ns_interp_to_vr(const NsInterp *ip, float out[12]);
/* Interpolación de transformaciones: traslación lineal, rotación por cuaterniones (slerp) con escala por columna lineal; si la matriz no es una rotación
 * con escala razonable (sombras aplanadas, reflejos) cae a interpolación lineal elemento a elemento. t fuera de [0,1] extrapola. */
void ns_xf_lerp(const NsXf *a, const NsXf *b, float t, NsXf *out);
/* 1 si dos escenas consecutivas se pueden interpolar (misma época de mallas, misma zona, hueco < NS_DISCONT_NS, cámara sin salto, to_vr sin salto). */
int ns_scene_continuous(const NsScene *prev, const NsScene *cur);

#ifdef __cplusplus
}
#endif
