#pragma once
/* vr_view: decide, una vez por VBlank, QUÉ se le enseña al usuario en el visor: el mundo en primera persona (cámara VR del juego,
 * escena 3D en estéreo + HUD en paneles) o la PANTALLA DE CINE (el fotograma completo que dibuja el juego, en una pantalla virtual
 * grande, para todo lo que no es control en primera persona: título, menús, radio, cargas, vídeos, cinemáticas del motor, fin de
 * partida, pausa). C99 puro, sin dependencias de plataforma; lo prueba test_view.c.
 *
 * Clasificación del cine (ver `VRV_K_*`): fmv / motor. Una escena 3D del motor es SIEMPRE la pantalla estereoscópica (render nativo): ya no hay respaldo con la imagen de
 * la VRAM ni medida de cobertura que cambie de clase.
 *
 * Política (ver bitácora, "Presentación en VR"):
 *   - MUNDO si hay partida (puente completo + jugador), la cámara VR del juego está activa y, o bien el jugador controla a su personaje, o bien
 *     hay una ventana 2D del juego encima (armas/objetos = VRV_R_SELECTOR, pausa = VRV_R_PAUSE_WORLD) sin otra causa (escena, radio, detenido, muerte):
 *     el mundo sigue en primera persona y la ventana se ve como panel flotante (la cámara VR no se apaga: vr/camera usa BridgeState.cam_ctl).
 *     Sin cámara VR activa, el motor dibuja con SU cámara, cenital, y mezclarla con el mundo del usuario sería un salto de 8 m de altura.
 *   - La radio (códec) gana a la cinemática: es 2D del juego aunque el motor la abra durante una escena -> pantalla de cine plana, nunca la pantalla 3D.
 *   - Todo lo demás es CINE. Si la cámara VR no está disponible (edición del ejecutable no reconocida), siempre CINE: el juego se
 *     ve igualmente, en pantalla grande.
 *   - Histéresis en fotogramas de VBlank (50 Hz) para que un parpadeo de una bandera del juego no haga saltar la vista:
 *     a CINE tras `to_cine` fotogramas seguidos de motivo de cine; a MUNDO tras `to_world` fotogramas seguidos de motivo de mundo.
 *   - Viniendo de una vista nativa (mundo o pantalla 3D) no se enseña en la pantalla plana un fotograma 3D rasterizado por la PS1 (baja resolución, sin estéreo): (1) mientras el motor no
 *     dibuja (cargas, cambios de zona: `frozen`) la vista entera se congela (VRV_R_FROZEN) y el render nativo repite su última escena dibujada; (2) si la clase pedida es 2D pero la última
 *     lista del juego trae 3D (`list3d`: arranque de una escena o de la vista de pared, cuyas primeras listas aún salen con la cámara VR; zona recién cargada sin jugador) la vista entera
 *     se retiene (VRV_R_HOLD) como mucho hold_max fotogramas. En los dos casos se retienen MODO y clase: con modo cine la app solo deja la pantalla plana si la clase es la pantalla 3D.
 *     La radio, la pausa, las ventanas, el juego detenido y la muerte sí son 2D aunque haya 3D detrás.
 *   - Fundido a negro (`VrView.black`, lo aplica la app con su velo): mientras dura VRV_R_FROZEN la escena nativa retenida NO se puede enseñar (el juego reescribe por debajo la VRAM de la que
 *     el render nativo lee texturas y paletas: se veía deformarse durante los 0,5-4,8 s de cada carga). El negro empieza en el primer fotograma sin dibujo del motor viniendo de una vista
 *     nativa y dura hasta que hay destino: (a) vista nativa (mundo, con escena apta `native_ok`, o pantalla 3D) con `fresh` >= black_fresh escenas nuevas y el motor dibujando black_settle fotogramas seguidos
 *     (un hueco de 2 fotogramas en mitad del desmontaje no lo levanta); (b) la radio, el juego detenido o un vídeo: enseguida; (c) otra pantalla 2D estable black_2d fotogramas (hold_max
 *     si su lista trae 3D de PS1). En los tres casos solo cuando la vista VIGENTE (modo y clase con histéresis) ya es ese destino: levantarlo antes enseñaría, mientras la app baja el velo,
 *     la vista retenida o la pantalla plana. Topes: black_nat_max esperando escenas, black_max en total. No cambia modo, clase ni motivo. La radio, la pausa y el selector nunca lo
 *     provocan; desde una pantalla 2D (fin de partida -> continuar, título) tampoco: no hay vista nativa que tapar.
 *   - Pegado a la pared (`wall`) con control: CINE con la pantalla 3D del motor (su cámara de pared, que enseña lo que la primera persona no puede) y el mando original.
 *   - Cámara de evento del juego con el mando retirado al jugador (`event`: panel del ascensor) con control: lo mismo que la pared (cámara VR apagada a propósito, pantalla 3D del motor
 *     con su cámara y el mando original), mientras dure; la carga de la planta elegida entra en el fundido a negro y la escena de llegada es una cinemática.
 *   - Se incluye el motivo (VRV_R_*) para el registro y para que la interfaz distinga "el mundo se está pidiendo" de "cine por cinemática". */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { VRV_CINEMA = 0, VRV_WORLD = 1 } VrViewMode;

enum {
    VRV_R_WORLD = 0,       /* mundo: partida, con control y cámara VR */
    VRV_R_NO_GAME,         /* sin partida: título, selección, cargas, vídeos de arranque, cambio de zona */
    VRV_R_CINEMATIC,       /* escena del motor (demo / control retirado) */
    VRV_R_COMMS,           /* comunicador (radio) abierto */
    VRV_R_STOPPED,         /* juego detenido: película, pantalla de resultado, ... */
    VRV_R_PAUSED,          /* pausa */
    VRV_R_MENU,            /* ventana de armas / objetos del juego (el motor dibuja con su cámara mientras está abierta) */
    VRV_R_LOST_CONTROL,    /* sin control sin otra causa conocida (fin de partida, transiciones) y sin mundo nativo apto con cámara VR (con él es VRV_R_WORLD: ascensor, gateo) */
    VRV_R_NO_CAM,          /* con control pero la cámara VR no está (aún) activa o no existe */
    VRV_R_DEAD,            /* el jugador ha muerto (vida a cero / fin de partida): el motor enseña su escena de muerte con su propia cámara */
    VRV_R_SELECTOR,        /* ventana de armas/objetos con la cámara VR activa: MUNDO en primera persona + ventana como panel flotante */
    VRV_R_PAUSE_WORLD,     /* pausa con la cámara VR activa: igual que el selector */
    VRV_R_PENDING,         /* transición: el jugador ya tiene el control (fin de radio / de escena) pero la cámara VR aún no está: vista congelada, la app muestra negro, NUNCA cine plano */
    VRV_R_WALL,            /* pegado a la pared con control: la cámara VR se apaga a propósito (vrcam_wall_view) y se ve el juego con SU cámara de pared en la pantalla 3D, con el mando original */
    VRV_R_FROZEN,          /* el motor no está dibujando (carga / cambio de zona: su contador de fotogramas sin dibujo > 0): en pantalla queda su último fotograma; viniendo de una vista nativa, la
                            * vista (modo y clase) se congela con la última escena nativa, nunca se enseña ese fotograma de la VRAM en la pantalla plana; la app la tapa en negro (VrView.black) */
    VRV_R_HOLD,            /* la lista trae 3D de PS1 sin cámara del motor reconocida y se venía de una vista nativa: vista (modo y clase) retenida un tiempo acotado en vez de la pantalla plana */
    VRV_R_EVENT            /* cámara de evento del juego con el mando retirado al jugador y con control (panel del ascensor): la cámara VR se apaga a propósito (vrcam_event_view) y se ve el juego con SU
                            * cámara en la pantalla 3D, con el mando original. Al final: los valores anteriores no cambian (la app compara algunos por número) */
};

typedef struct {
    uint8_t game;          /* puente completo + jugador en escena */
    uint8_t in_control;
    uint8_t cam_active;    /* la cámara del motor es la de la cabeza del usuario */
    uint8_t cinematic, comms, paused, menu_open, stopped;
    /* --- clasificación de lo que NO es mundo (al final: no rompe inicializaciones posicionales) --- */
    uint8_t scene3d;       /* la última lista capturada trae geometría 3D sustancial con cámara del motor válida */
    uint8_t dead;          /* vida a cero (o fin de partida) con partida en curso: la cámara del motor muestra la muerte en tercera persona */
    uint8_t video;         /* la última lista es un fotograma de vídeo pregrabado (subidas a VRAM que cubren la zona mostrada, sin 3D) */
    uint8_t mdec;          /* decodificador de vídeo (MDEC) activo en los últimos fotogramas: hay un FMV reproduciéndose (más fiable que `video`; gana al 3D) */
    uint8_t native_ok;     /* la última escena nativa publicada es apta y reciente (ns_hook_native_ready): el mundo se puede dibujar con el render nativo (no basta el puente: la escena debe existir y valer) */
    uint8_t wall;          /* pegado a la pared con la vista del juego pedida (vrcam_wall_view) */
    uint8_t frozen;        /* el motor no dibuja (contador de fotogramas sin dibujo del motor > 0): lo que hay en la pantalla del juego es un fotograma viejo */
    uint8_t list3d;        /* la última lista capturada trae geometría 3D sustancial, con la cámara que sea (scene3d exige la del motor): la pantalla del juego es un render 3D de PS1 */
    uint8_t fresh;         /* escenas nativas dibujables (partida, escenario y listas enteras) publicadas desde que el motor volvió a dibujar; satura en 255. Solo lo mira el fundido a negro */
    uint8_t event;         /* cámara de evento del juego con el mando retirado y la vista del juego pedida (vrcam_event_view) */
} VrViewIn;

/* Qué es lo que se ve (con histéresis propia, ver VrView.kind):
 *   WORLD   primera persona (= VRV_WORLD)
 *   ENGINE  cinemática o conversación hecha con el motor en tiempo real: hay 3D en la escena y la cámara la lleva el guion (sin partida
 *           controlable). Se presenta en 3D estereoscópico (pantalla 3D) en vez de en la pantalla de cine plana.
 *   VIDEO   vídeo pregrabado (fotogramas descomprimidos subidos a VRAM): siempre pantalla de cine
 *   UI      menús, títulos, cargas, códec/radio, pausa, ventanas de armas/objetos, resultados: 2D, pantalla de cine */
enum { VRV_K_UI = 0, VRV_K_WORLD, VRV_K_ENGINE, VRV_K_VIDEO };

typedef struct {
    int mode;              /* VrViewMode vigente (con histéresis) */
    int reason;            /* motivo vigente (VRV_R_*) del último fotograma (sin histéresis) */
    int want;              /* modo pedido por el último fotograma */
    int run;               /* fotogramas seguidos pidiendo `want` distinto de `mode` */
    int to_cine, to_world; /* umbrales de histéresis, en fotogramas */
    uint32_t switches;
    /* --- clasificación (VRV_K_*) --- */
    int kind;              /* vigente */
    int want_kind;         /* pedido por el último fotograma */
    int kind_run;          /* fotogramas seguidos pidiendo `want_kind` distinto de `kind` */
    int to_engine, from_engine, to_other;   /* umbrales de histéresis (fotogramas): entrar en ENGINE, salir de ENGINE, resto de cambios */
    uint32_t kind_switches;
    /* --- registro --- */
    int prev_reason;       /* motivo del fotograma anterior */
    int to_comms;          /* fotogramas de radio seguidos antes de pasar de mundo a la pantalla de cine (el códec debe estar ya dibujado) */
    int pend_max;          /* fotogramas máximos de VRV_R_PENDING seguidos; pasados, sin cámara VR = cine (cámara no disponible) */
    int pend_run;
    int cam_seen;          /* la cámara VR ha estado activa alguna vez (si no, no hay transición que esperar: edición no reconocida) */
    unsigned changed;      /* tras vrview_step: VRV_CH_* de lo que cambió en ese fotograma (0 = nada) */
    int frozen_max;        /* fotogramas máximos seguidos de VRV_R_FROZEN (tope de seguridad: pasado, clasificación normal) */
    int frozen_run;
    int hold_max;          /* fotogramas máximos seguidos de VRV_R_HOLD (pasado, clasificación normal: pantalla plana) */
    int hold_run;
    /* --- fundido a negro (ver la política) --- */
    int black;             /* 1 = la app debe tener la vista en negro (funde rápido al subir y al bajar) */
    int black_pre;         /* fotogramas seguidos con el motor sin dibujar antes de entrar en negro (las paradas cortas al empezar escenas no rewriten la VRAM) */
    int black_run;         /* fotogramas seguidos en negro */
    int black_draw;        /* de ellos, los últimos seguidos con el motor dibujando */
    int black_nat;         /* de ellos, los últimos seguidos con destino nativo (esperando escenas nuevas) */
    int black_fresh;       /* escenas nuevas completas exigidas para levantar el negro hacia una vista nativa */
    int black_settle;      /* fotogramas seguidos con el motor dibujando exigidos para lo mismo */
    int black_2d;          /* fotogramas con el motor dibujando una pantalla 2D (que no es ventana del juego ni vídeo) antes de levantar el negro */
    int black_nat_max;     /* fotogramas máximos con destino nativo sin escenas nuevas */
    int black_max;         /* tope total de fotogramas en negro */
} VrView;

enum { VRV_CH_MODE = 1, VRV_CH_KIND = 2, VRV_CH_REASON = 4, VRV_CH_BLACK = 8 };

/* Línea de registro para un fotograma con cambios (view.changed != 0): "cambio: [modo] [clase] [motivo] [negro] modo=... clase=... motivo=... | banderas ... negro=0|1 nuevas=N". Devuelve la longitud escrita. */
int vrview_describe(const VrView *v, const VrViewIn *in, char *buf, int n);

void vrview_reset(VrView *v);
/* Motivo para unas entradas dadas (función pura, sin histéresis). */
int vrview_reason(const VrViewIn *in);
/* Un fotograma. Devuelve el modo vigente. */
int vrview_step(VrView *v, const VrViewIn *in);
/* Clase de lo que se ve para unas entradas dadas (función pura, sin histéresis). */
int vrview_kind_of(const VrViewIn *in);
const char *vrview_kind_name(int kind);
/* Línea de registro «[cine] tipo=fmv|motor motivo=...» para el tipo VIGENTE (v->kind); tipos que no son de cine (mundo / 2D) → «tipo=2d|mundo». Devuelve la longitud. */
int vrview_cine_describe(const VrView *v, const VrViewIn *in, char *buf, int n);
/* Detección de fotograma de vídeo por subidas a VRAM: área (media-palabras) de la subida (x,y,w,h) recortada al rectángulo mostrado (dx,dy,dw,dh; dw en píxeles del modo) y
 * veredicto con la suma de varias subidas: 1 si cubren >= 60 % del área mostrada (24 bits: 3/2 media-palabras por píxel). Vale para un rectángulo entero y para tiras de 16 px. */
long long vrview_upload_area(int dx, int dy, int dw, int dh, int depth24, int x, int y, int w, int h);
int vrview_uploads_cover(long long sum, int dw, int dh, int depth24);
/* Nombre corto del tipo de cine: fmv / motor / 2d / mundo. */
const char *vrview_cine_type(int kind);
const char *vrview_reason_name(int reason);

/* ¿Se conserva el recubrimiento de pantalla (clase 3b: alarma roja, destellos, fundidos, gafas, marco de máscara) como post-proceso por ojo en vez de pasar al panel 2D? (función pura; vrhost.c)
 * Nativo: gafas, marco o mundo; una cinemática del motor con 3D (scene3d) lo manda al panel. Modo gpu: el mundo (has_3d y world_valid), la cinemática del motor con 3D y las tiras de agua
 * (sin panel) lo conservan, y el renderer lo aplica con RenderRG.planPost; el resto va al panel. */
static inline int vrhost_keep_filter(int native, int cine_cand, int scene3d, int has_3d, int world_valid, int n_scan, int frame_on, int n_strips) {
    int world = has_3d && world_valid;
    if (!native) return n_strips > 0 || world || (cine_cand && scene3d);
    return (n_scan > 0 || frame_on || world) && !(cine_cand && scene3d);
}

/* ¿Corre la emulación sin límite de ritmo (y con el audio descartado: runtime/host.h mv_host_set_load_boost)? Solo en una carga del motor: su contador de fotogramas sin dibujo (`undraw`,
 * BA_UNDRAW_COUNT) está en la cuenta atrás de la petición de carga (parte de 0x7FFF0000; un guion que oculta unos fotogramas pone valores pequeños y no cuenta), la vista lleva en negro
 * VRHOST_BOOST_DELAY fotogramas (el fundido de la app, 0,15 s, ha terminado: nada de lo acelerado se ve) y no hay vídeo. Acotado a VRHOST_BOOST_MAX fotogramas en negro. El tiempo virtual
 * del juego no cambia (mismos VBlanks, mismo ritmo de disco por VBlank): solo dura menos en el reloj de pared. Función pura; vrhost.c. */
#define VRHOST_LOAD_UNDRAW 0x01000000
#define VRHOST_BOOST_DELAY 10
#define VRHOST_BOOST_MAX 3000
static inline int vrhost_load_boost(int black, int black_run, int32_t undraw, int mdec) {
    return black && black_run >= VRHOST_BOOST_DELAY && black_run < VRHOST_BOOST_MAX && undraw >= VRHOST_LOAD_UNDRAW && !mdec;
}

/* ¿Se quitan de la capa 2D los polígonos 3D sin vista (opts.drop_world2d de vr_scene.h)? Solo con render nativo, que dibuja el 3D: el mundo (también con la ventana del selector encima: son rects 2D y el cielo / las
 * estrellas sin vista que el juego sigue dibujando no deben llegar al panel HUD, donde son un cuadrado opaco ante la cara; sí se conserva con la pausa, sin volcado que lo respalde) y la cinemática del motor
 * (pantalla 3D: encima solo va 2D de verdad, nunca trozos de la imagen de PS1). Menús y vídeo son 2D enteros. Función pura; vrhost.c. */
static inline int vrhost_drop_world2d(int native, int kind, int reason) {
    return native && ((kind == VRV_K_WORLD && reason != VRV_R_PAUSE_WORLD) || kind == VRV_K_ENGINE);
}

/* ¿Pinta el render nativo AHORA el efecto de pantalla `bit` (NS_SFX_FADE, NS_SFX_GAS) que el juego dibuja con paquetes propios (fadeio.c, gas_efct.c)? Solo entonces esas primitivas salen del
 * panel 2D (vr_prov.h, clase FX_SCREEN). Exige: renderer nativo preparado; última escena nativa publicada APTA (ns_scene_native_ok: con una no apta el renderer repite la última válida con los
 * efectos de pantalla de AQUELLA, no los de ahora) y con efectos (NS_SV_FX) que traen ese bit (`sfx` = sus NsScreenFx.flags, 0 sin NS_SV_FX); y reciente (`age_vb` sincronismos desde que se
 * publicó: sin escenas nuevas el renderer también repite). Si falta algo, el efecto sigue en el panel: un fundido a negro no se pierde. Función pura; vrhost.c. */
#define VRHOST_SFX_MAX_AGE 8u
static inline int vrhost_native_sfx(int renderer_ready, int scene_fit, uint32_t sfx, uint32_t age_vb, uint32_t bit) {
    return renderer_ready && scene_fit && age_vb <= VRHOST_SFX_MAX_AGE && (sfx & bit) != 0;
}

/* ¿Se aplica la máscara de visión (marco con la abertura de la caja) como recubrimiento de ojo del render nativo? Solo en nativo, con el jugador en la caja (BRIDGE_PS_BOX) y una máscara en la lista (frame_on). Función pura; vrhost.c. */
static inline int vrhost_box_mask(int native, int in_box, int frame_on) { return native && in_box && frame_on; }
/* Anillo de pantalla de gas_efct.c dentro de un conducto: el guion de la zona (s01a, s02a: sin gas) lo enciende al ENTRAR en la zona del conducto y lo apaga al salir (mensajes ON/OFF
 * al actor 0xB455, junto con el cambio de sonido ambiente), con el jugador en la 1.ª persona de gateo del juego (estado INTRUDE): borde negro -> gris 64 en resta = el oscurecimiento de
 * bordes de esa cámara 2D. Con la cámara VR sería una viñeta a pantalla completa pegada a la cabeza: no se dibuja. Fuera de INTRUDE, o con otro modo de mezcla (un anillo de gas de
 * verdad), se queda. `intrude` = BRIDGE_PS_INTRUDE del estado del jugador; `abr` = modo de mezcla del anillo (2 = resta). */
static inline int vrhost_duct_ring(int vr_active, int intrude, int abr) { return vr_active && intrude && abr == 2; }
/* Banderas de efectos de pantalla (NS_SFX_*) de la última escena publicada tal como las ve prov_covered. `has_fx` = escena con NS_SV_FX; `gas_intensity` = intensidad del anillo (0 = sin anillo); `duct_cut` = el cierre
 * de la escena recortó el anillo del conducto (vrhost_duct_ring): el nativo ya no lo pinta PERO tampoco debe verse el de gas_efct.c en el panel 2D (marco 4:3), así que cuenta como cubierto (NS_SFX_GAS). */
static inline uint32_t vrhost_scene_sfx(int has_fx, uint32_t flags, float gas_intensity, int duct_cut) {
    const uint32_t gas = 2u;   /* NS_SFX_GAS (ns_types.h; este archivo no lo incluye) */
    if (!has_fx) return 0;
    if (gas_intensity <= 0.0f) return flags & ~gas;
    return duct_cut ? flags | gas : flags;
}

/* ¿El 2D (lotes kind 2) de la lista actual va DENTRO de las texturas de la pantalla 3D en vez de en el panel aparte (hp2 de XRPresent)? (función pura; RenderRG.stereo2DInside)
 * Solo en modo gpu, si hay una imagen 3D sobre la que componer (la lista trae 3D o hay una base 3D guardada) y el estéreo compuso de verdad esa lista (composed_in_stereo = 0 si
 * falló la textura o la cámara y se cayó al cine plano): si no, el 2D se ve por el panel como antes. */
static inline int vrhost_stereo2d_inside(int gpu, int list_has_3d_or_base, int composed_in_stereo) {
    return gpu && list_has_3d_or_base && composed_in_stereo;
}

#ifdef __cplusplus
}
#endif
