#pragma once
/* vr_view: decide, una vez por VBlank, QUÉ se le enseña al usuario en el visor: el mundo en primera persona (cámara VR del juego,
 * escena 3D en estéreo + HUD en paneles) o la PANTALLA DE CINE (el fotograma completo que dibuja el juego, en una pantalla virtual
 * grande, para todo lo que no es control en primera persona: título, menús, radio, cargas, vídeos, cinemáticas del motor, fin de
 * partida, pausa). C99 puro, sin dependencias de plataforma; lo prueba test_view.c.
 *
 * Clasificación del cine (ver `VRV_K_*`): fmv / motor / respaldo. La pantalla estereoscópica (motor) solo se usa si el render nativo representa de verdad lo
 * que el motor dibuja (cobertura medida; sin medir: solo con partida); si no, el respaldo enseña el fotograma emulado entero (área de pantalla de la VRAM).
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
    VRV_R_LOST_CONTROL,    /* sin control sin otra causa conocida (fin de partida, transiciones) */
    VRV_R_NO_CAM,          /* con control pero la cámara VR no está (aún) activa o no existe */
    VRV_R_DEAD,            /* el jugador ha muerto (vida a cero / fin de partida): el motor enseña su escena de muerte con su propia cámara */
    VRV_R_SELECTOR,        /* ventana de armas/objetos con la cámara VR activa: MUNDO en primera persona + ventana como panel flotante */
    VRV_R_PAUSE_WORLD,     /* pausa con la cámara VR activa: igual que el selector */
    VRV_R_PENDING          /* transición: el jugador ya tiene el control (fin de radio / de escena) pero la cámara VR aún no está: vista congelada, la app muestra negro, NUNCA cine plano */
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
    uint8_t cover1;        /* cobertura de la lista de la GPU por lo que se VE dentro de la vista del juego, en % + 1 (0 = sin medida vigente): escena nativa + relleno global (ns_fill) si hay relleno, si no el nativo solo */
    uint8_t cover_new;     /* 1 en el fotograma en que llega una medida nueva en `cover1` */
    uint8_t cover_src;     /* de dónde viene `cover1` (solo registro): 0 ninguna, 1 nativo solo, 2 nativo + relleno */
} VrViewIn;

/* Qué es lo que se ve (con histéresis propia, ver VrView.kind):
 *   WORLD   primera persona (= VRV_WORLD)
 *   ENGINE  cinemática o conversación hecha con el motor en tiempo real: hay 3D en la escena y la cámara la lleva el guion (sin partida
 *           controlable). Se presenta en 3D estereoscópico (pantalla 3D) en vez de en la pantalla de cine plana.
 *   VIDEO   vídeo pregrabado (fotogramas descomprimidos subidos a VRAM): siempre pantalla de cine
 *   UI      menús, títulos, cargas, códec/radio, pausa, ventanas de armas/objetos, resultados: 2D, pantalla de cine */
enum { VRV_K_UI = 0, VRV_K_WORLD, VRV_K_ENGINE, VRV_K_VIDEO, VRV_K_BACKUP };
/* VRV_K_BACKUP: escena 3D del motor en la que el render nativo NO es fiel (extractor sin escena apta en ese modo: introducciones, demos sin partida, cobertura baja):
 *   se enseña el fotograma emulado completo (área de pantalla de la VRAM) en la pantalla de cine plana, escalado con calidad. La lista de la GPU no se dibuja en 3D jamás. */

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
    /* --- fidelidad del render nativo en escenas del motor (con histéresis por medidas) --- */
    int cov_state;         /* 0 sin medidas (por defecto: de fiar solo con partida), 1 fiel, 2 no fiel */
    uint8_t cov_src;       /* fuente (VrViewIn.cover_src) de las medidas de la ventana: si cambia, la ventana se reinicia (no se mezclan nativo y nativo+relleno) */
    unsigned cov_bad_mask, cov_good_mask;   /* últimas `cov_win` medidas: bit a 1 = por debajo de cov_lo / por encima de cov_hi */
    int cov_idle;          /* fotogramas seguidos sin 3D (pasados cov_forget se olvidan las medidas) */
    int cov_hi, cov_lo;    /* umbrales de cobertura (%): >= hi medida buena, < lo medida mala, entre medias cuenta como ninguna */
    int cov_win, cov_need_bad, cov_need_good, cov_forget;   /* ventana de medidas, malas para perder la fiabilidad, buenas para recuperarla, fotogramas sin 3D para olvidar */
    int cam_seen;          /* la cámara VR ha estado activa alguna vez (si no, no hay transición que esperar: edición no reconocida) */
    unsigned changed;      /* tras vrview_step: VRV_CH_* de lo que cambió en ese fotograma (0 = nada) */
} VrView;

enum { VRV_CH_MODE = 1, VRV_CH_KIND = 2, VRV_CH_REASON = 4 };

/* Línea de registro para un fotograma con cambios (view.changed != 0): "modo=a->b clase=... motivo=... | banderas". Devuelve la longitud escrita. */
int vrview_describe(const VrView *v, const VrViewIn *in, char *buf, int n);

void vrview_reset(VrView *v);
/* Motivo para unas entradas dadas (función pura, sin histéresis). */
int vrview_reason(const VrViewIn *in);
/* Un fotograma. Devuelve el modo vigente. */
int vrview_step(VrView *v, const VrViewIn *in);
/* Clase de lo que se ve para unas entradas dadas (función pura, sin histéresis). */
int vrview_kind_of(const VrViewIn *in);
const char *vrview_kind_name(int kind);
/* Fiabilidad del render nativo para una escena del motor con estas entradas (función pura): 1 = fiel. Sin medida vigente: solo con partida (puente completo + jugador). */
int vrview_native_trusted(const VrViewIn *in);
/* Línea de registro «[cine] tipo=fmv|motor|respaldo motivo=...» para el tipo VIGENTE (v->kind); tipos que no son de cine (mundo / 2D) → «tipo=2d|mundo». Devuelve la longitud. */
int vrview_cine_describe(const VrView *v, const VrViewIn *in, char *buf, int n);
/* Detección de fotograma de vídeo por subidas a VRAM: área (media-palabras) de la subida (x,y,w,h) recortada al rectángulo mostrado (dx,dy,dw,dh; dw en píxeles del modo) y
 * veredicto con la suma de varias subidas: 1 si cubren >= 60 % del área mostrada (24 bits: 3/2 media-palabras por píxel). Vale para un rectángulo entero y para tiras de 16 px. */
long long vrview_upload_area(int dx, int dy, int dw, int dh, int depth24, int x, int y, int w, int h);
int vrview_uploads_cover(long long sum, int dw, int dh, int depth24);
/* Nombre corto del tipo de cine: fmv / motor / respaldo / 2d / mundo. */
const char *vrview_cine_type(int kind);
const char *vrview_reason_name(int reason);

/* Entrega a la vista la medida de cobertura vigente (rellena in->cover1 / cover_new / cover_src). La cobertura que decide motor / respaldo es la de lo que el usuario VE dentro de la
 * vista del juego: escena nativa + relleno global (ns_fill) cuando hay relleno (has_fill); si no, la del nativo solo (has_nat). vb = fotograma actual; una medida de hace más de 150 fotogramas
 * (3 s) ya no vale. El relleno mide casi cada fotograma, el nativo solo cada 25: para que la ventana de vrview_step siga representando ~4 s, solo se acepta como NUEVA una medida a >= 25
 * fotogramas de la anterior aceptada (la primera de una escena, al instante). *seen = fotograma de la última medida aceptada (estado del llamador). Devuelve 1 si hay medida vigente. */
#define VRV_COVER_SPACING 25u
int vrview_cover_feed(VrViewIn *in, uint32_t *seen, uint32_t vb, int has_fill, uint32_t fill_frame, int fill_pct, int has_nat, uint32_t nat_frame, int nat_pct);

#ifdef __cplusplus
}
#endif
