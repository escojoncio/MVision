#pragma once
/* ns_fill: RELLENO POR LA VERDAD DEL MOTOR. Regla global (vale para cualquier escena, sin casos por zona):
 *   - La lista de la GPU de un fotograma contiene TODO lo que el motor dibuja en su vista, con la posición exacta (GpuVtx.vx/vy/vz = posición de vista de alta
 *     precisión de la GTE por vértice, vvalid).
 *   - La escena nativa leída de la RAM aporta lo que hay fuera de esa vista (360 grados) y lo que coincide con la lista.
 *   - Cada triángulo 3D de la lista SIN pareja en la escena nativa se dibuja en el mundo nativo: se desproyecta con la cámara con la que se emitió la lista
 *     (mundo = ojo + A^-1 * vista, la inversa EXACTA del mapa mundo->vista que usa ns_cover), con la misma textura, CLUT, UV, color y mezcla de la primitiva.
 *     Salida: NsEffect (NS_EK_QUAD en el MUNDO del motor), que el renderer añade a NsScene.effects: pasa por ns_gpu_fx_build (to_vr, semitransparentes
 *     ordenados junto a las mallas) sin ninguna ruta nueva en la GPU.
 *
 * Qué se rellena y qué no (por triángulo; los cuadriláteros son 2 triángulos (0,1,2) y (1,2,3)):
 *   cubierto por el nativo  MATCH (esquinas dentro de tol(z)), SPLIT (otra diagonal de la misma cara), SUBDIV (sobre la superficie nativa con la misma textura)
 *                           -> no se rellena. MATCH con otra página/CLUT (textura nativa equivocada) -> se rellena con sesgo hacia la cabeza (la lista manda).
 *   POSE                    la misma textura+UV existe en un objeto DINÁMICO cercano (pieza/animación/IK distinta): jugador (cuerpo propio y manos que sustituye la VR)
 *                           u otro objeto móvil que el nativo ya dibuja en su pose -> no se rellena (se cuenta aparte; no entra en la cobertura).
 *   resto                   AUSENTE (nada parecido), OTRA_TEXTURA (superficie nativa con otra textura), PARCIAL, SPRITE (rectángulo a z constante sin superficie) -> se rellena.
 *   sin posición fiable     vértices BEHIND (vz <= 0), AMBIG junto a SAT, aristas desproporcionadas (red de seguridad), texel único transparente -> no se rellena (contado).
 *   2D                      polígonos sin vvalid (HUD, texto): fuera (van a la capa 2D).
 *   efectos                 las operaciones que ns_fx ya cubre (carteles, ondas...) las excluye el llamador con skip_op (ns_fill_run.c lo hace).
 * Identidad: cada relleno lleva una identidad estable (NsEffect.src_key = 0xF1000000 | id) que hereda al reencontrarse (misma firma de textura y sitio de mundo a <= 48 u, o sitio de VISTA a <= view_match u);
 *   el renderer la usa para detectar lo que sigue a la cabeza (ns_gpu_fx_build con seguimiento). La operación de la lista de origen queda en la caja box[0] (bits de un uint32) solo para herramientas.
 * Persistencia: lo rellenado se ancla al MUNDO y se conserva keep_frames fotogramas tras verse por última vez (para que al girar la cabeza fuera de la vista del
 * motor no desaparezca de golpe); se retira de inmediato si su sitio está dentro de la vista actual y el motor ya no lo dibuja, o si la cámara salta (corte).
 *
 * Coste: índice espacial del nativo en el MUNDO (rejilla de esquinas + rejilla de superficies + cubos por textura de los dinámicos), reconstruido por llamada
 * (O(triángulos nativos)); cada triángulo de la lista hace consultas O(1). Medidas en tools/nsfill.c y en bitacora.md.
 * Sin dependencias de plataforma (C11, libm). Un solo hilo por NsFill (el trabajador de ns_fill_glue.c). */
#include <stddef.h>
#include <stdint.h>
#include "gpu.h"
#include "ns_adapt.h"
#include "ns_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Cámara con la que se emitió la lista: ojo en el mundo del motor y vista->mundo fila-mayor (columnas = X derecha, Y abajo, Z delante), como VrCamRecord.M /
 * NsCamera.rot. El mapa mundo->vista es A = diag(1,ys,1) R^T (e = A (p - ojo)); el relleno desproyecta con A^-1 (exacta, también con escala en R). Sin ys la vista de la lista
 * (que sí lo lleva en Y) y las matrices de la RAM discrepan un 3 % en Y: lo que ns_cover llama «pose» en lejanías con la cámara del juego. */
typedef struct { float eye[3]; float R[9]; float ys; } NsFillCam;   /* ys: factor vertical de la vista de la GTE frente a las matrices de la RAM (NS_OVERSCAN_Y = 66/64 con la cámara propia del juego, 1 con la de la VR; 0 = 1) */

typedef struct {
    float tol_base, tol_z;     /* tolerancia de esquina: tol_base + tol_z * z de vista (def. 20 y 0,004; tope 200), como ns_cover */
    float surf_tol;            /* margen extra de «sobre la superficie» (def. 80) */
    float pose_radius;         /* distancia máxima del centro de un triángulo de la lista a su pareja dinámica de igual textura+UV para llamarlo POSE (def. 2500 = 2,5 m) */
    float max_edge;            /* arista máxima de un triángulo a rellenar, unidades del motor (def. 40000); mayor = «explosión» de vértices, se descarta */
    float max_edge_flagged;    /* idem con vértices SAT/AMBIG/reconstruidos (def. 6000) */
    float max_dist;            /* distancia máxima al ojo de un vértice (def. 400000) */
    int screen_w, screen_h;    /* pantalla del motor (def. 320x256) para decidir si un sitio está «dentro de la vista actual» */
    uint32_t player_key;       /* NsInstance.key del jugador (ns_player_key); 0 = ninguno. Sus triángulos sin pareja son POSE (la VR los sustituye) */
    int pose_fill;             /* 0 (def.): POSE no se rellena; 1: los POSE de objetos que NO son el jugador sí (cuando el nativo los tiene en otra pose desactualizada) */
    int fix_texture;           /* 1 (def.): MATCH con página/CLUT distintos se rellena con sesgo */
    int fill_partial_semi;     /* 0 (def.): PARCIAL semitransparente no se rellena (doble mezcla sobre lo ya cubierto); 1: sí */
    uint32_t keep_frames;      /* persistencia fuera de la vista (def. 15; 0 = sin persistencia) */
    uint32_t max_out;          /* tope de efectos de salida (def. 6000) */
    float cut_dist, cut_cos;   /* corte de cámara: ojo movido más de cut_dist (def. 3000) o eje Z girado con coseno < cut_cos (def. 0,7) -> se vacía la persistencia */
    int bias;                  /* NsEffect.depth_bias de lo que corrige texturas / parcial (def. 2) */
    float near_skip;           /* un triángulo pequeño (ver near_edge_max) cuyo punto MÁS CERCANO queda a menos de near_skip unidades del ojo (def. 250 = 25 cm) no se rellena: es la envoltura del propio jugador / geometría pegada al plano de la cámara, que desproyectada
                                * queda rígida a la cabeza (0 = sin regla) */
    float near_edge_max;       /* near_skip solo omite triángulos pequeños: arista máxima < near_edge_max (def. 500 = 50 cm; 0 = sin límite). Las paredes grandes nunca se omiten por cercanas */
    float view_match;          /* radio (unidades de vista) con el que un relleno nuevo hereda la identidad de uno visto hace poco con la MISMA firma en la misma posición de VISTA aunque su sitio en el mundo haya cambiado: es lo que
                                * delata a lo pegado a la cámara (def. 24; 0 = solo por mundo) */
    void *dbg;                 /* FILE* opcional (herramientas): una línea por triángulo que se rellena o se descarta, con su causa */
} NsFillCfg;
void ns_fill_cfg_default(NsFillCfg *c);

/* Clases por triángulo de la lista (NsFillStats.cls). */
enum {
    NF_MATCH = 0, NF_SPLIT, NF_SUBDIV, NF_LOCKED,            /* cubiertos por el nativo (LOCKED: pegado a la cámara, casa con la cámara alternativa) */
    NF_POSE_PLAYER, NF_POSE_OTHER,                           /* no se rellenan: jugador / otro objeto dinámico */
    NF_FILL_ABSENT, NF_FILL_OTHERTEX, NF_FILL_PARTIAL, NF_FILL_SPRITE, NF_FILL_TEXFIX,   /* se rellenan */
    NF_SKIP_BEHIND, NF_SKIP_AMBIG, NF_SKIP_INCOH, NF_SKIP_EDGE, NF_SKIP_FAR, NF_SKIP_NAN, NF_SKIP_FB, NF_SKIP_PARTIAL_SEMI, NF_SKIP_NEAR, NF_SKIP_CAP,   /* sin posición fiable / vacío / tope */
    NF_N
};
extern const char *const ns_fill_cls_name[NF_N];

typedef struct {
    uint32_t ops, polys3d, no_view, recon;        /* operaciones / polígonos con vista / sin vista (2D) / reconstruidos (un vértice sin vista o esquinas diagonales) */
    uint32_t tris, degenerate, skipped_ops;       /* triángulos 3D comparados / degenerados omitidos / operaciones excluidas por skip_op (efectos) */
    uint32_t cls[NF_N];
    uint32_t filled_tris;                         /* triángulos nuevos rellenados este fotograma */
    uint32_t persisted, retired_view, retired_age, retired_cut;   /* conservados de fotogramas anteriores / retirados por estar en vista sin aparecer / por edad / por corte */
    uint32_t n_out;                               /* efectos devueltos (nuevos + persistentes) */
    uint32_t nat_tris, nat_dyn;
    float cover_pct;                              /* % de los triángulos 3D (sin POSE) cubiertos por nativo + relleno: (MATCH+SPLIT+SUBDIV+rellenados) / (tris - POSE) */
    float nat_pct;                                /* lo mismo contando solo el nativo */
    double ms_index, ms_match, ms_persist, ms_total;
    int cam_src;                                  /* solo en la sesión completa: cámara con la que se desproyectó: 0 la de la lista, 1 la de la RAM, 2 la de la RAM porque la lista no trajo cámara (sin registro de cámara válido) */
    double ms_extract, ms_adapt, ms_fx;           /* solo en la sesión completa (ns_fill_run.c): extracción de la RAM, adaptación a triángulos de mundo, clasificación de efectos; ya sumados a ms_total */
} NsFillStats;

typedef struct NsFill NsFill;
NsFill *ns_fill_create(void);
void ns_fill_destroy(NsFill *f);
/* Vacía la persistencia (cambio de zona, corte de cámara conocido). */
void ns_fill_reset(NsFill *f);

/* Escena nativa en el MUNDO: triángulos de ns_adapt_scene (con src_out) y, opcionalmente, la escena para saber qué instancias son estáticas (NS_IF_STATIC). */
typedef struct { const NsRTri *tris; const NsAdaptSrc *src; size_t n; const NsScene *sc; } NsFillNative;

/* Un fotograma: casa la lista con el nativo, rellena lo que falta, actualiza la persistencia. skip_op (nops bytes, != 0 = excluir la operación; puede ser NULL).
 * alt (puede ser NULL) = la OTRA cámara disponible (la de la RAM si cam es la de la lista, o al revés): lo que no casa con cam pero sí con alt está pegado a la cámara del motor
 * (cielo, fondos que se mueven con el ojo; el nativo lo tiene un fotograma desplazado) y cuenta como cubierto (NF_LOCKED), no se rellena.
 * frame = nº de fotograma del motor (crece). 0 = bien. El resultado se lee con ns_fill_effects hasta la siguiente llamada. */
int ns_fill_run(NsFill *f, const NsFillNative *nat, const GpuOp *ops, uint32_t nops, const uint8_t *skip_op, const NsFillCam *cam, const NsFillCam *alt, const NsFillCfg *cfg, uint32_t frame, NsFillStats *st);
const NsEffect *ns_fill_effects(const NsFill *f, uint32_t *n);

/* Tasa de casamiento (MATCH) de una muestra de la lista con una cámara candidata: sirve para elegir entre la cámara de la lista y la de la RAM cuando no coinciden
 * (cinemáticas con registro de cámara inactivo). Devuelve [0,1] o -1 si no hay muestra. */
float ns_fill_probe(NsFill *f, const NsFillNative *nat, const GpuOp *ops, uint32_t nops, const uint8_t *skip_op, const NsFillCam *cam, const NsFillCfg *cfg);

/* Mapa mundo->vista y su inversa para una cámara (A fila-mayor 3x3). 0 = bien (A invertible). */
int ns_fill_cam_maps(const NsFillCam *cam, double A[9], double Ai[9]);

/* ---- sesión completa (ns_fill_run.c): instantánea de RAM + lista -> efectos ---- */
typedef struct NsFillSession NsFillSession;
NsFillSession *ns_fill_session_create(void);
void ns_fill_session_destroy(NsFillSession *s);
typedef struct {
    const float *list_eye, *list_M;   /* cámara con la que se emitió la lista (ojo mundo y vista->mundo fila-mayor); NULL = la de la RAM */
    int list_vr;                      /* 1: la cámara de la lista es la de la capa VR (VrCamRecord.active): su vista no lleva el factor 66/64 en Y (ys = 1); 0: es la del juego */
    int no_auto_cam;                  /* 0 (def.): si hay cámara de la lista y de la RAM, usa la que case mejor (ns_fill_probe); 1: siempre la de la lista */
    uint32_t player_key;              /* 0 = leerlo de la RAM (ns_player_key) */
    int keep_native;                  /* herramientas: conserva la escena nativa adaptada de la última ejecución (ns_fill_session_native) */
} NsFillRunOpts;
/* Extrae la escena de la RAM, la adapta, excluye lo que cubre ns_fx, rellena. 0 = bien. cam_used (opcional): 0 la de la lista, 1 la de la RAM. */
int ns_fill_session_run(NsFillSession *s, const uint8_t *ram, const uint8_t *scratch, const GpuOp *ops, uint32_t nops, const NsFillRunOpts *ro, const NsFillCfg *cfg,
                        uint32_t frame, NsFillStats *st, int *cam_used);
const NsEffect *ns_fill_session_effects(const NsFillSession *s, uint32_t *n);
/* generación de zona y época del extractor de la última ejecución (cambian al cambiar de zona/escena); -1 si no hay sesión */
int ns_fill_session_zone(const NsFillSession *s, uint32_t *zone_gen, uint32_t *epoch);
NsFill *ns_fill_session_fill(NsFillSession *s);
/* Herramientas (con keep_native): escena nativa adaptada de la última ejecución, máscara de operaciones excluidas (efectos) y cámara usada. Válidos hasta la siguiente ejecución. */
int ns_fill_session_native(const NsFillSession *s, NsFillNative *nat, const uint8_t **skip, NsFillCam *cam);
size_t ns_fill_format(const NsFillStats *st, uint32_t frame, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
