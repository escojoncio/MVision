#pragma once
/* vr_prov: clasifica las primitivas 2D por PROCEDENCIA (runtime/prov.h: GpuOp.prov / GpuDrawList.prov) y decide a qué capa 2D van. Sustituye a las heurísticas por forma
 * (rect del selector, filtro de créditos por par de rectángulos, bandas, regiones del HUD). C99 puro, sin dependencias de plataforma; lo prueba test_prov_class.c.
 *
 *  Clases (VPC_*), por actor (nombre de depuración de la entrada de procedencia, nodo+20) o por función (fn) de la rutina que encadenó la primitiva:
 *    HUD_WORLD     HUD de partida (menuman.c y sus llamadas de componentes: vida, radar, cajas, textos); por defecto de lo que viene de menuman
 *    WINDOW        lo que solo emite mientras la ventana del selector / la pausa está abierta (aprendido en ejecución, ver VrProvLearn)
 *    FULL          créditos (telop.c, famaslit.c): panel plano de vista completa
 *    FX_VIGNETTE   motse.c: viñeta. No va al 2D: efecto nativo por ojo (VrFx.vignette / vign_radius)
 *    FX_LETTERBOX  cinema.c: bandas de cinemática. No va al 2D: efecto nativo (VrFx.letterbox)
 *    SCOPE_FRAME   scope.c / sight.c sin textura en una lista SIN prismáticos (sight.c sin scope.c: mira de PSG-1 / lanzador / Nikita): marco de la mira. No va al 2D: vista nativa de zoom (VrFx.scope_on / scope_zoom)
 *    SCOPE_TEXT    scope.c / sight.c texturado: textos de la mira; van al HUD
 *    SCOPE_BINO    scope.c / sight.c sin textura en una lista CON prismáticos (hay primitivas de scope.c: el objeto SCOPE; VrProvLearn.bino): el marco del juego (rejilla, retícula, bandas, tinte verdoso a pantalla completa).
 *                  Va a la capa HUD (que el visor pinta sobre la vista aumentada de los prismáticos, VrFx.binocular) y vr_scene no lo trata como recubrimiento del filtro de pantalla
 *    UNKNOWN       sin procedencia o de otro actor/función: va al HUD (como antes) y se cuenta
 *    FX_SCREEN     gas_efct.c (anillo del gas) y fadeio.c (fundido): efectos de pantalla con paquetes propios que el renderer nativo hace de la RAM (ns_fx: NsFxGas / NsFxFade). No van al 2D,
 *                  pero SOLO mientras el anfitrión garantice que el nativo pinta ese efecto ahora (VrProvLearn.covered); si no, son UNKNOWN y siguen en el HUD (un fundido no se pierde)
 *    WORLD         encadenada por la ordenación del canal 3D (vpc_is_chain), con un escritor sin regla (o sin escritor) y con GARANTÍA de cobertura nativa de su paquete (VrProvLearn.covered: el
 *                  paquete tiene un elemento que el renderer nativo dibuja; nieve, chaff, humo...). Con VrSceneOpts.drop_world2d vr_scene no la emite al panel (VR_FATE_WORLD2D). Sin
 *                  drop_world2d (ventana del selector / pausa) sale al panel y va al HUD, como antes. Sin esa garantía (primitivas de pantalla de actores sin regla, fondo de cielo) es UNKNOWN
 *  Capas 2D (VPC_L_*): HUD (HUD_WORLD, SCOPE_TEXT, SCOPE_BINO, UNKNOWN, WORLD), WINDOW, FULL; las clases FX_* y SCOPE_FRAME no tienen capa (-1).
 *  Sin procedencia (volcados NSL1, rt_prov apagado) todo es UNKNOWN y por tanto HUD: el comportamiento de antes.
 *  Id efectivo de una primitiva: el de quien encadenó el paquete (GpuOp.prov) salvo que sea una rutina de ordenación (vpc_is_chain): entonces el de quien rellenó el comando (GpuOp.prov2), o 0 si no consta o es otra rutina de ordenación. */
#include <stdint.h>
#include "gpu.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { VPC_HUD_WORLD = 0, VPC_WINDOW, VPC_FULL, VPC_FX_VIGNETTE, VPC_FX_LETTERBOX, VPC_SCOPE_FRAME, VPC_SCOPE_TEXT, VPC_UNKNOWN, VPC_FX_SCREEN, VPC_WORLD, VPC_SCOPE_BINO, VPC_N };   /* HUD_WORLD = 0: memset a cero = todo HUD */
enum { VPC_L_HUD = 0, VPC_L_WINDOW, VPC_L_FULL, VPC_L_N };
const char *vpc_class_name(int cls);
int vpc_layer_of(int cls);   /* VPC_L_* o -1 (no va al 2D) */

/* Efectos de pantalla que salen de las primitivas clasificadas (los publica el host a Swift dentro de MvVrGameState.fx; unidades: alto de la zona mostrada). */
typedef struct {
    float vignette;      /* intensidad 0..1 (color máximo de los vértices de motse.c / 255) */
    float vign_radius;   /* radio de la zona limpia en alturas de media pantalla (distancia mínima de un vértice al centro del abanico / (alto/2)) */
    float letterbox;     /* fracción 0..1 del alto cubierta por las bandas de cinemática (cinema.c) */
    int32_t scope_on;    /* hay primitivas de scope.c / sight.c */
    float scope_zoom;    /* aumento >= 1: distancia de pantalla H efectiva de la cámara de la lista / 160 si > 1; si no, 1 */
    int32_t binocular;   /* scope_on con prismáticos (scope.c en la lista, VrProvLearn.bino): el marco del juego va en la capa HUD y el visor lo pinta sobre una vista aumentada; sin él (sight.c solo) es la mira de un arma */
} VrFx;

/* ---- reglas (vr_prov_rules.c) ---- */
/* Clase base de una primitiva por su entrada de procedencia (sin aprendizaje ni encadenado): FX_*, FULL, SCOPE_*, HUD_WORLD (menuman) o UNKNOWN. e == NULL -> UNKNOWN. */
int vpc_rule(const ProvEntry *e, const GpuOp *o);
/* ¿Es la entrada scope.c (rutina 0x80063d54), el actor del objeto SCOPE (prismáticos)? sight.c solo (sin scope.c) es la mira de otras armas. e == NULL -> 0. */
int vpc_is_binocular(const ProvEntry *e);
int vpc_is_mosaic(const ProvEntry *e);     /* mosaico de censura del juego (mosaic.c): celdas 2D que muestrean el propio fotograma */
/* Efecto de pantalla de la entrada: VPC_COV_GAS (gas_efct.c), VPC_COV_FADE (fadeio.c) o -1 (no es de la clase FX_SCREEN; e == NULL -> -1). */
int vpc_screen_kind(const ProvEntry *e);
/* ¿Es la entrada una rutina de ordenación/encadenado (0x8001da18, 0x80018304)? Encadena paquetes que escribieron otros actores: su id no dice quién dibuja. e == NULL -> 0. */
int vpc_is_chain(const ProvEntry *e);

/* ---- aprendizaje de la clase WINDOW ----
 * Un id de procedencia (rutina de componente del HUD o desconocida) es WINDOW si no emitió ninguna primitiva durante al menos VPC_QUIET_LISTS listas de mundo con la ventana cerrada
 * antes de emitir con la ventana abierta (selector o pausa); se conserva toda la sesión. Un id aprendido que vuelve a emitir con la ventana cerrada VPC_LEAK_LISTS listas (el cierre
 * con animación no cuenta) se descarta para siempre (era HUD): así un componente raro del HUD no desaparece del HUD. Las clases FX / FULL / SCOPE nunca se aprenden como ventana. */
#define VPC_QUIET_LISTS 30
#define VPC_LEAK_LISTS 20
enum { VPC_COV_PACKET = 0, VPC_COV_GAS, VPC_COV_FADE, VPC_COV_MOSAIC, VPC_COV_FEEDBACK };   /* qué se pregunta a VrProvLearn.covered (MOSAIC: el mosaico de censura de esta lista, que el anfitrión convierte en un parche del mundo: vr/host/vr_mosaic.h) */
/* VPC_COV_FEEDBACK: los efectos de realimentación de pantalla de esta lista (vr_feedback.h: sprites que muestrean un frame buffer) los reproduce el nativo sobre su imagen: salen del panel 2D. */
enum { VPC_ST_OTHER = 0, VPC_ST_CLOSED, VPC_ST_OPEN };   /* estado de la vista de la lista: mundo con ventana cerrada / abierta / otra vista (no se aprende) */
typedef struct {
    uint8_t win[PROV_MAX];             /* 0 sin decidir, 1 WINDOW, 2 descartado */
    uint16_t leak[PROV_MAX];           /* listas con la ventana cerrada en que emitió un id aprendido */
    uint32_t last_closed[PROV_MAX];    /* nclosed de la última lista cerrada en que emitió (0 = nunca) */
    uint32_t seen[PROV_MAX];           /* marca de la lista en que se contó el id (una vez por lista) */
    uint32_t tick, nclosed;            /* listas anotadas / listas anotadas con la ventana cerrada */
    uint32_t n_learned, n_demoted;     /* ids aprendidos / descartados (registro) */
    /* Área mostrada {x, y, w, h} al cerrar la última lista anotada (draw_cur, = su búfer de dibujo) y la anterior (draw_prev, = lo que estaba en pantalla mientras se dibujaba la última:
     * «el otro búfer» de vr_feedback.h); w = 0 = sin dato. draw_idx = índice de la última lista anotada (una lista anotada dos veces no desplaza la pareja). */
    int draw_prev[4], draw_cur[4];
    uint32_t draw_idx; int draw_have;
    int bino;                          /* la última lista anotada (vpc_learn_note, con cualquier estado) trae primitivas de scope.c: prismáticos (SCOPE_BINO) */
    /* Garantía de cobertura nativa (la fija el anfitrión; vpc_learn_reset la borra; NULL = sin garantía: nada es WORLD ni FX_SCREEN). what = VPC_COV_PACKET: ¿el paquete del motor en pkt_addr
     * (GpuOp.pkt_addr) tiene un elemento que el renderer nativo dibuja (ns_fx_covers_packet)? VPC_COV_GAS / VPC_COV_FADE: ¿pinta el nativo ahora el anillo / el fundido? (pkt_addr sin uso) */
    int (*covered)(void *ctx, int what, uint32_t pkt_addr);
    void *covered_ctx;
} VrProvLearn;
void vpc_learn_reset(VrProvLearn *S);
/* Anota la lista `l` ANTES de ensamblarla (así sus ids ya están clasificados). state: VPC_ST_*. Con cualquier estado (también VPC_ST_OTHER) fija S->bino de esa lista. */
void vpc_learn_note(VrProvLearn *S, const GpuDrawList *l, int state);

/* Id efectivo de la primitiva o (ver arriba); 0 = no consta. No comprueba que esté dentro de la tabla. */
unsigned vpc_op_id(const GpuDrawList *l, const GpuOp *o);
/* Clase de la primitiva o (de la lista l): reglas + aprendizaje (S puede ser NULL) + garantía de cobertura nativa (S->covered): sin ella FX_SCREEN es UNKNOWN; con ella, WORLD si la encadenó la
 * ordenación del canal 3D, su escritor no tiene regla ni es una ventana aprendida y su paquete está cubierto. */
int vpc_class_op(const VrProvLearn *S, const GpuDrawList *l, const GpuOp *o);
/* Gancho para VrSceneOpts.op_class (ctx = VrProvLearn *, puede ser NULL). */
uint8_t vpc_op_class_cb(const GpuDrawList *l, uint32_t i, void *ctx);

/* Reparte op_dest (VrScene.op_dest: 2 = al panel 2D) por capas: out[k][i] = 2 si op_dest[i] == 2 y la clase de op_cls[i] va a la capa k, 0 si no; cnt[k] = primitivas de la capa. Cada out[k] tiene n bytes. */
void vpc_split_dest(const uint8_t *op_dest, const uint8_t *op_cls, uint32_t n, uint8_t *const out[VPC_L_N], uint32_t cnt[VPC_L_N]);
/* Cuenta las primitivas del panel (op_dest == 2) por clase. */
void vpc_tally(const uint8_t *op_dest, const uint8_t *op_cls, uint32_t n, uint32_t cnt[VPC_N]);

/* ---- efectos ---- */
/* Extrae los efectos de las primitivas de la lista (independiente de op_dest: las bandas de cinemática pueden haberse descartado del 2D por otra vía). rec_screen = distancia de pantalla
 * efectiva de la cámara de la lista (VrCamRecord.screen; 0 = no hay). */
void vpc_fx_extract(const VrProvLearn *S, const GpuDrawList *l, int rec_screen, VrFx *fx);
/* Mantiene cada efecto VPC_FX_HOLD listas tras su última aparición (una lista sin las primitivas no apaga el efecto de golpe). */
#define VPC_FX_HOLD 4
typedef struct { VrFx fx; uint8_t age_v, age_l, age_s; } VrFxHold;
void vpc_fx_hold(VrFxHold *H, const VrFx *now);

#ifdef __cplusplus
}
#endif
