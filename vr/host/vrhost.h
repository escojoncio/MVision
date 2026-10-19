#pragma once
/* vrhost: pegamento entre el host del runtime (runtime/host.c) y la capa VR (puente RAM, cámara VR, ensamblador de escena).
 * Vive fuera de runtime/ para que los .c de runtime/ no dependan de vr/ (varias herramientas enlazan todo runtime/ tal cual).
 *
 * Uso (app): mv_vrhost_install() una vez, ANTES de mv_host_start. Después:
 *  - el hilo de render llama a mv_host_scene() (RenderRGBridge.h) por frame para recibir las listas de dibujo ya ensambladas;
 *  - el hilo de entrada lee mv_vrhost_game_state() (postura, menú) para alimentar vr_input, publica la pose de la cabeza
 *    con mv_vr_set_head() y los giros con mv_vr_turn() / mv_vr_recenter() (host.h).
 *
 * Qué hace en el hilo de emulación, en cada VBlank (tras publicar el vídeo y aplicar el mando):
 *    bridge_read -> (giros/recentrado pendientes) -> vrcam_frame -> [si hay consumidor de escenas] drena la captura de la GPU,
 *    ensambla una VrScene por lista cerrada (vr_scene_build con el BridgeState del mismo frame) y la copia a memoria propia.
 * Al cargar el EXE (solo MV_NOGEN): parchea el recorte (vrcam_patch_ram) y registra las HLE de la cámara por DIRECCIÓN FIJA
 * (hle_bind_fixed; no hay firmas de la librería para ellas). Si el EXE no coincide con la edición esperada no hay cámara VR.
 * IA de enemigos y cámaras (vr/ai): se instala con las HLE si el EXE es la edición esperada (ver mv_vrhost_set_ai). */
#include <stdint.h>
#include "vr_radar.h"
#include "vr_scene.h"
#include "vr_smooth.h"

#ifdef __cplusplus
extern "C" {
#endif

void mv_vrhost_install(void);

/* IA de enemigos y cámaras (vr/ai/vrai.h); cualquier hilo, antes o después de arrancar (se aplica en el siguiente VBlank).
 *   perception: 0 original (el juego tal cual), 1 VR (defecto: visión ampliada con sospecha gradual), 2 exigente
 *   cameras:    1 (defecto) cámaras de vigilancia audibles/visibles de lejos y rompibles con 2 balas (alcance de detección original); 0 original
 *   activity:   1 (defecto) soldados y sensores activos aunque la cámara original no los vea; 0 original
 * El radar de muñeca dibuja el cono original y el ampliado según `perception`. */
void mv_vrhost_set_ai(int perception, int cameras, int activity);
/* Solo para herramientas de prueba (vr/ai/test_ai_run): función llamada en el gancho de VBlank del hilo de emulación tras la capa VR (cpu = Cpu*). */
void mv_vrhost_set_test_hook(void (*cb)(void *cpu, uint32_t frame));
/* Toma de pruebas: se llama en el hilo de emulación justo tras ensamblar cada escena (antes de copiarla al slot), con la lista, el puente y la escena. NULL = sin toma. */
void mv_vrhost_set_scene_tap(void (*cb)(const GpuDrawList *dl, const BridgeState *bs, const VrScene *sc, uint32_t frame));

/* Solo pruebas (vr/tools/vrprobe/probe2d): se llama en el hilo de emulación con cada lista capturada completa (`full`), la lista sin las primitivas del radar
 * que se ensambló (`hud`), la escena resultante (memoria interna de vr_scene: valida solo durante la llamada), las opciones y el puente con la cámara de la lista. */
typedef void (*MvVrListTap)(const GpuDrawList *full, const GpuDrawList *hud, const VrScene *sc, const VrSceneOpts *o, const BridgeState *bs);
void mv_vrhost_set_list_tap(MvVrListTap cb);

/* Escena VR del siguiente frame (contrato en app/Sources/RenderRGBridge.h). Una por llamada y sin saltarse ninguna;
 * los punteros de `out` valen hasta la siguiente llamada. Llamarla es lo que activa la captura de la GPU: sin llamadas durante
 * ~1,5 s se apaga (ventana plana) y al volver la primera lista trae la VRAM completa (vram_init). Thread-safe. */
extern int mv_host_scene(VrScene *out);

/* Volcado de un fotograma para desarrollo (vr/native/ns_dump.h). Cualquier hilo (no bloquea). `dir` = directorio a crear (el padre debe existir; se copia la
 * ruta). El hilo de emulación sólo COPIA (RAM, memoria rápida, VRAM, coprocesador de geometría, lista, meta) en el siguiente fin de fotograma, con la última lista construida si la hay y sin ella si no
 * (sin consumidor, cola llena o sin lista nueva); la PNG y el disco los hace un hilo trabajador propio. Devuelve 0 si se aceptó, -1 si ya hay uno en curso,
 * la ruta es inválida o falta memoria. */
int mv_vrhost_request_dump(const char *dir);
/* Resultado del último volcado: 0 = en curso o ninguno, 1 = escrito, 2 = escrito con partes accesorias fallidas, -1 = falló. Se consume al leerlo. */
int mv_vrhost_dump_result(void);
/* Anula un pedido que aún no llegó a copiarse (timeout del llamador). Si ya se copió, termina de escribirse. */
void mv_vrhost_cancel_dump(void);

/* Resumen del último frame del puente para Swift (sin importar el BridgeState entero). */
typedef struct {
    int32_t game;        /* 1 si hay jugador en escena con el puente completo (partida) */
    int32_t in_control;  /* el jugador controla a su personaje */
    int32_t posture;     /* -1 desconocida, 0 de pie, 1 agachado, 2 tumbado */
    int32_t menu;        /* 1 = fuera de control (título, carga, pausa, menú, comunicador, escena): sin locomoción, stick -> cruceta */
    int32_t cam_active;  /* 1 si la cámara del motor es la de la cabeza del usuario */
    int32_t first_person;
    int32_t weapon;      /* -1 ninguna, 0..9 */
    int32_t life, life_max;
    /* --- presentación (vr_view.h; al final: no rompe a quien solo lea lo anterior) --- */
    int32_t view_mode;   /* VrViewMode con histéresis: 1 = mundo en primera persona, 0 = pantalla de cine */
    int32_t view_reason; /* VRV_R_* del último VBlank (motivo del cine / del mundo) */
    int32_t cinematic, comms, paused, menu_open, stopped;   /* banderas del puente (escena del motor, radio, pausa, ventana de armas/objetos, detenido) */
    int32_t ammo, ammo_max;  /* munición del arma equipada (cargador del juego) y su máximo; -1 si no hay */
    int32_t item;            /* objeto equipado, -1 ninguno */
    int32_t alert_mode;      /* 0 normal, 1 interferencia, 2 evasión, 3 alerta */
    /* --- cinemáticas del motor (vr_view.h, vr_cine.h) --- */
    int32_t view_kind;       /* VRV_K_*: 0 2D, 1 mundo, 2 cinemática del motor (3D estereoscópico), 3 vídeo pregrabado */
    uint32_t cine_shot;      /* plano en curso de la cinemática (sube en cada corte de cámara) */
    uint32_t cine_cuts;      /* cortes de cámara detectados desde que se cargó el ejecutable */
    int32_t cine_cover;      /* fundidos/destellos (recubrimientos a pantalla completa) en la última lista */
    /* --- render nativo --- */
    int32_t render_req;      /* modo pedido: 0 = GPU (reconstrucción desde la lista de dibujo), 1 = nativo */
    int32_t native_avail;    /* 1 si el modo nativo se puede usar (extractor registrado, enganche instalado y renderer Metal listo) */
} MvVrGameState;
void mv_vrhost_game_state(MvVrGameState *out);

/* ---- Modo de render (nativo | gpu) ----
 * gpu: el 3D se reconstruye de la lista de dibujo de la GPU (comportamiento histórico). nativo: el 3D sale de la escena que el extractor lee de la RAM
 * (vr/native, puente ns_bridge) y la capa 2D (HUD, menús, radio, textos, radar y HUD de muñeca) sigue saliendo de la lista de la GPU. Las cinemáticas
 * del motor y todo lo que no tenga cámara VR se dibujan SIEMPRE como pantalla estereoscópica con la cámara del juego (desde la lista).
 * Elección: variable de entorno MV_RENDER=nativo|gpu (gana y fija el modo) o mv_vrhost_set_render_mode (menú VR). Defecto: gpu (MV_RENDER_DEFAULT_NATIVE en vrhost.c
 * pasará a 1 cuando extractor y renderer estén integrados). El modo nativo solo se activa de verdad si mv_vrhost_native_available() y llega una escena válida;
 * si no, se sigue con la lista de la GPU (nunca pantalla vacía). Cualquier hilo.
 * QUIÉN DECIDE: el que DIBUJA, en cada fotograma, con su estado real. La lista de la GPU llega entera; si la escena admite el 3D nativo trae VrScene.native_3d = 1 y los
 * batches 3D marcados (VrBatch.skip_if_native). El render hace  ns_bridge_acquire(mv_vrhost_native_bridge(), ns_now_ns(), &view)  y
 * ns_native_decide(&view, &(NsDecideIn){mv_vrhost_render_mode(), renderer_ok, mallas_subidas, scene.native_3d, ahora, 250 ms}, &d)  (vr/native/ns_bridge.h); si d.native pinta la
 * escena nativa y salta los batches con (b.skip_if_native & d.skip); si el dibujo nativo falla a mitad llama a ns_native_fail(&d) y pinta TODOS los batches en ese mismo fotograma.
 * No hay estado global "esta escena es nativa": solo el de cada escena (native_3d) y el de la decisión. */
void mv_vrhost_set_render_mode(int native);
int mv_vrhost_render_mode(void);                 /* modo pedido (0/1) */
int mv_vrhost_render_env_locked(void);           /* 1 si MV_RENDER fija el modo (el menú no puede cambiarlo) */
int mv_vrhost_native_available(void);
void mv_vrhost_native_renderer_ready(int ready); /* el renderer Metal nativo avisa de que puede dibujar el puente */
struct NsBridge;
struct NsBridge *mv_vrhost_native_bridge(void);  /* puente de escenas nativas (consumidor: render a 100 Hz). Nunca NULL tras mv_vrhost_install */

/* Copia el BridgeState del último VBlank (bridge.h). Devuelve 1 si hay uno publicado. Thread-safe. */
struct BridgeState;
/* Radar de muñeca: escena del radar (vr/radar/vr_radar.h) asociada a la ÚLTIMA escena entregada por mv_host_scene (mismos punteros
 * válidos, hasta la siguiente llamada). Devuelve 0 si esa escena no traía radar (el juego lo oculta/apaga/interfiere sin dibujarlo, o
 * cinemática): entonces no se dibuja el quad en el brazo. La escena del HUD ya NO lleva las primitivas del radar. */
int mv_host_radar(VrRadarScene *out);

int mv_host_bridge(struct BridgeState *out);

/* Límite de la cabeza contra el escenario (vr/camera/vr_headcol.h). Hilo de render, una vez por fotograma: `head` = pose física de la cabeza en el espacio recentrado
 * (16 floats en columnas, como mv_vr_set_head), dt = segundos desde la llamada anterior. Devuelve 1 si hay recorte o fundido; corr = desplazamiento (m, mismo espacio)
 * que hay que SUMAR a la posición de la cabeza para dibujar el mundo (la esfera de la cabeza no atraviesa paredes; nunca se mueve al personaje del juego);
 * fade = 0..1 de negro progresivo si la cabeza empuja más allá del límite. Sin escena VR activa devuelve 0 y corr = 0. */
int mv_vrhost_head_limit(const float head[16], float dt, float corr[3], float *fade);
/* Activa/desactiva el límite (defecto: activo; MV_HEADCOL=0 en el entorno lo apaga al arrancar) y fija el radio de la esfera en metros (0 = no cambiar). */
void mv_vrhost_set_headcol(int enable, float radius_m);

typedef struct {
    uint32_t built;          /* escenas ensambladas */
    uint32_t delivered;      /* entregadas por mv_host_scene */
    uint32_t has3d, world_valid;
    uint32_t full_waits;     /* VBlanks en que la cola propia estaba llena y las listas esperaron en la captura */
    uint32_t cap_resyncs;    /* veces que se (re)encendió la captura por llegada de un consumidor */
    uint32_t cam_rec_found, cam_rec_missing; /* listas con/sin registro de cámara asociado por secuencia (solo las que tienen polígonos con posición de vista) */
    uint32_t cam_rec_differs; /* de las halladas: cuántas traen una cámara distinta de la del puente en ese VBlank */
    double build_ms_avg, build_ms_max;
    int cam_ok;              /* 1 si el EXE coincidió con la edición esperada (parche + HLE de cámara) */
} MvVrHostStats;
void mv_vrhost_stats(MvVrHostStats *s);

/* Fluidez de la locomoción (vr_smooth.h; ver bitácora). El juego publica escenas a 25-50 por segundo y el visor dibuja a 90-100 Hz: por cada fotograma de render
 * esta llamada da una corrección RÍGIDA del mundo de la escena vigente (giro del stick pedido y aún no visto en la escena + traslación del personaje extrapolada con
 * sus marcas de tiempo de juego), en el espacio del usuario: P' = m * P. Se aplica SOLO a la geometría del mundo (vistas del pase de escena); el arma, las manos, el
 * radar y el HUD no se tocan. `active` = 0 -> identidad. Thread-safe; llamar una vez por fotograma de render (hilo de render), también si no llegó escena nueva. */
void mv_vrhost_smooth(VrSmCorr *out);
void mv_vrhost_set_smooth(int on);                    /* 1 (defecto; MV_SMOOTH=0 al arrancar lo apaga) / 0: siempre identidad */
void mv_vrhost_smooth_stats(VrSmoothStats *s);
/* Solo pruebas headless: reloj monótono (s) con el que se marcan las escenas y se evalúa la corrección (NULL = CLOCK_MONOTONIC). */
void mv_vrhost_set_clock(double (*fn)(void));

#ifdef __cplusplus
}
#endif
