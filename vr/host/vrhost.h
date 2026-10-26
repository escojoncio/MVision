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
/* Menú VR «IA: alcance de visión» (1,0 / 1,5 / 2,0; defecto 1,5) y «los enemigos ven la cabeza VR» (defecto 1). Cualquier hilo; no afectan a las cámaras de vigilancia. */
void mv_vrhost_set_ai_range(float scale);
void mv_vrhost_set_ai_head(int on);
/* Menú VR «Manos: nuevas / originales» (defecto nuevas = 1): manos articuladas propias en el render nativo en primera persona; 0 = la manopla del motor con el puño deformado.
 * Cualquier hilo. Las cinemáticas y la cámara del juego conservan siempre las manos originales. */
void mv_vrhost_set_hands_new(int on);
int mv_vrhost_hands_new(void);
int mv_vrhost_hands_active(void);   /* ajuste activado Y render nativo listo */
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
/* Pareja de volcados del MISMO fotograma de juego en un solo gesto: el del punto de enganche (justo tras la captura nativa, point=stage en el meta; sin lista de la GPU)
 * a dir_stage y, a continuación, el del final de fotograma (point=frame, con la última lista) a dir_frame. Mismas reglas y mismo resultado que mv_vrhost_request_dump
 * (el resultado es el peor de los dos). Sin el enganche nativo instalado se hace un único volcado, a dir_frame. */
int mv_vrhost_request_dump_pair(const char *dir_stage, const char *dir_frame);
/* Carpeta (ya existente) donde el anfitrión deja los autovolcados (auto<NN>_<motivo>/, meta point=auto_<motivo>; ver vrdump.h autodump_decide). Vacía/NULL = sin autovolcado (ni registro). */
void mv_vrhost_set_autodump_dir(const char *dir);
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
    int32_t view_kind;       /* VRV_K_*: 0 2D, 1 mundo, 2 cinemática del motor (3D estereoscópico), 3 vídeo pregrabado, 4 respaldo (3D del motor sin render nativo fiel: imagen de la VRAM en la pantalla de cine) */
    uint32_t cine_shot;      /* plano en curso de la cinemática (sube en cada corte de cámara) */
    uint32_t cine_cuts;      /* cortes de cámara detectados desde que se cargó el ejecutable */
    int32_t cine_cover;      /* fundidos/destellos (recubrimientos a pantalla completa) en la última lista */
    /* --- render nativo --- */
    int32_t render_req;      /* modo pedido: 0 = gpu (defecto), 1 = nativo */
    int32_t native_avail;    /* 1 si el modo nativo se puede usar (extractor registrado, enganche instalado y renderer Metal listo) */
} MvVrGameState;
void mv_vrhost_game_state(MvVrGameState *out);

/* ---- Render ----
 * Dos modos (cualquier hilo). gpu (DEFECTO): el 3D del mundo (y de las cinemáticas del motor, desde la cámara del juego) sale de la lista de la GPU del propio juego con los vértices de
 * vista de la GTE en alta precisión (estilo PGXP, vr_scene.h); el render nativo no captura ni dibuja. Las primitivas sin vértices de vista (HUD, textos, créditos, fundidos y destellos a
 * pantalla completa) van a la capa 2D (panel) a su distancia fija, nunca proyectadas junto al ojo. nativo (MV_RENDER=nativo): la escena que el extractor lee de la RAM (vr/native, puente
 * ns_bridge); si en un fotograma no está lista se repite el último válido o se funde a negro (ns_present.h) y la lista de la GPU solo alimenta la capa 2D, los eventos de VRAM y las marcas.
 * Elección: MV_RENDER=gpu|nativo en el entorno (gana y fija el modo) o mv_vrhost_set_render_mode. «Solo nativo» ya no existe. */
void mv_vrhost_set_render_mode(int mode);        /* 0 = gpu, otro valor = nativo (sin efecto si MV_RENDER fija el modo) */
int mv_vrhost_render_mode(void);                 /* 0 = gpu, 1 = nativo */
int mv_vrhost_render_only_native(void);          /* siempre 0 */
int mv_vrhost_render_env_locked(void);           /* 1 si MV_RENDER fija el modo */
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
float mv_vrhost_wall_distance(const float p_mm[3], float max_mm);   /* mm hasta la superficie estática más cercana (golpe en la pared); -1 ninguna */
int mv_vrhost_head_limit(const float head[16], float dt, float corr[3], float *fade);
/* Activa/desactiva el límite (defecto: activo; MV_HEADCOL=0 en el entorno lo apaga al arrancar) y fija el radio de la esfera en metros (0 = no cambiar). */
void mv_vrhost_set_headcol(int enable, float radius_m);
/* Capa 2D fiel y en alta resolución (vr/scene/vr_layer2d.h): suavizado de las texturas 2D, 0 original / 1 nítido / 2 suave (defecto 1). MV_HUD2D=0 la apaga (el renderer vuelve al shader); MV_HUD2D_SCALE=1..6 fija la escala interna (defecto 4, baja sola si la lista tarda > 12 ms). Cualquier hilo. */
void mv_vrhost_set_hud2d_smooth(int mode);
int mv_vrhost_hud2d_smooth(void);
void mv_vrhost_hud2d_stats(int *scale, double *ms);
/* Capa 2D fiel (hilo trabajador): -1 sin capa utilizable (usar el shader), 0 la misma de antes, 1 nueva (px RGBA8 w x h válidos hasta mv_vrhost_hud2d_release; full = fotograma entero). Hilo de render. */
int mv_vrhost_hud2d_acquire(const uint8_t **px, int *w, int *h, int *full, uint32_t *seq);
void mv_vrhost_hud2d_release(void);
/* Modo (VL_PANEL=0 / VL_FULL=1 / VL_PANEL_VERIFIED=2) y etiqueta de vista ((view_kind << 8) | view_reason) de la capa adquirida; solo entre acquire (>= 0) y release. 1 si hay capa. Hilo de render. */
int mv_vrhost_hud2d_layer(int *mode, int *view);
/* Entre mv_vrhost_hud2d_acquire (>= 0) y mv_vrhost_hud2d_release: rect UV [u0, v0, u1, v1] de la ventana de menú (selector sobre el mundo) dentro de la capa adquirida; 0 si no hay (usar la capa entera). Hilo de render. */
int mv_vrhost_hud2d_menu_rect(float uv[4]);
/* ¿Vale una capa (mode, layer_view) para la vista vigente cur_view? Con overlay (ventana 2D del menú sobre el mundo) solo VL_PANEL; siempre la misma clase de vista (view_kind). */
int mv_vrhost_hud2d_accept(int mode, int layer_view, int cur_view, int overlay);

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
