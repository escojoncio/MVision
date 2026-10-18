#pragma once
/* vr_scene: ensamblador de escena estéreo. Convierte una lista de dibujo capturada (GpuDrawList, runtime/gpu.h)
 * más el estado del puente (BridgeState) en geometría 3D en un espacio "mundo VR" (metros, Y arriba, mano
 * derecha) y en primitivas 2D de pantalla PS1 (HUD, texto, menús), listas para que el renderer las pinte por ojo.
 * C99, sin dependencias de plataforma; no llama al runtime (solo lee las estructuras).
 *
 * ---------------------------------------------------------------------------------------------------------
 * Clases de salida (VrBatch.kind)
 *   0 = 3D opaco        triángulos en mundo VR (pos en METROS).
 *   1 = 3D semitransp.  igual; el renderer los pinta después de los opacos, sin escribir profundidad.
 *   2 = 2D / HUD        triángulos en coordenadas de pantalla PS1 (pos = {x, y, 0}, píxeles enteros con el offset
 *                       de dibujo ya sumado), para la textura del panel HUD. Rects y líneas se convierten a
 *                       triángulos (líneas: cuadrilátero de 1 px, centrado en los píxeles y alargado 0,5 px por extremo). VR_F_2D siempre activo en flags.
 *
 * Qué es 3D: polígonos cuyos vértices traen TODOS datos de vista (GpuVtx.fvalid, vx/vy/vz en espacio de vista del
 * juego, unidades del motor). Todo lo demás (rects, líneas, polígonos sin float: texto, retratos, menús) es 2D.
 * Un polígono con solo ALGUNOS vértices fvalid (recortado por el juego) se descarta (opts.partial_to_2d lo
 * manda a 2D, pero entonces aparecerá como trozo de mundo en el HUD).
 *
 * ---------------------------------------------------------------------------------------------------------
 * Espacio mundo VR y escala (ver vr_scene.c, "Escala")
 *   v_vr = S * Mmap * ( R_eye * v_vista + eye_pos - origen_juego )
 *   - R_eye (3x3, 1.12) y eye_pos: matriz vista->mundo del motor (BridgeState.eye_axes / eye_pos), el inverso de lo
 *     que usa la GTE. Lleva el vértice del espacio de vista al MUNDO DEL JUEGO (unidades del motor, Y arriba).
 *   - S = opts.scale (metros por unidad del juego; defecto 0,001: 1 unidad = 1 mm).
 *   - Mmap: identidad (mundo del juego ya es Y arriba) o espejo en Z si el mundo del juego es zurdo
 *     (opts.mirror_z: -1 automático por det(R_eye)). Se conserva siempre un sistema de mano derecha.
 *   - origen_juego: con origin_mode 0 (defecto) el jugador (player_pos con y rebajada en feet_below_pos hasta los
 *     pies) => el jugador está en x=z=0 y los pies en y=0 EN CADA FOTOGRAMA. Para un mundo FIJO entre fotogramas
 *     usar origin_mode 1 y pasar el mismo origin_game siempre; o acumular VrScene.origin_game (el origen usado,
 *     en unidades del juego) para mantener la plataforma del usuario: pos_mundo_fijo = pos_vr + origin_game*S.
 *   Los triángulos salen con el frente (la cara que vio la cámara del juego) en sentido antihorario: si hace falta se
 *   permutan dos vértices y se marca VR_F_FLIPPED (solo indica la permutación; en un quad un triángulo de cada dos la
 *   necesita por la paridad del orden Z, no implica doble cara). El renderer puede culling o dibujar a doble cara.
 *
 * Orden y VRAM
 *   - Los vértices de cada clase mantienen el orden de dibujo original. Los batches se emiten por "épocas": una
 *     época es el tramo de la lista entre dos eventos de VRAM (GPU_OP_FILL / GPU_OP_VCOPY / GPU_OP_UPLOAD). Dentro
 *     de cada época: clase 0, clase 1, clase 2; cada batch corta además al cambiar el modo de mezcla.
 *   - El consumidor recorre b[0..nb) EN ORDEN. Antes de dibujar un batch debe haber aplicado a su VRAM todos los
 *     eventos de src->ops con índice < batch.ev_end (los aplica incrementalmente; ev_end no decrece).
 *     Tras el último batch, aplicar los eventos restantes de la lista (hasta src->nops) para quedar en sincronía.
 *   - VrBatch.src_op: índice en src->ops de la primera primitiva del batch (depuración / estado extra).
 *
 * Campos de VrVtx
 *   uv: texels 0..255 de la página (floats, sin ventana de textura; si la primitiva tiene ventana E2 está en
 *       VrScene.states[v.pad]). Para rects con flip, u decrece (u1 = u - w).
 *   rgba: r | g<<8 | b<<16 | a<<24 (a = 255). Color de vértice (planos: el de la primitiva).
 *   clut = (clx/16) | (cly << 6); tpage = (tx/64) | ((ty/256)<<4) | (abr<<5) | (tp<<7)  (formato PS1 GP0(E1h)).
 *   flags = GPU_F_* (gpu.h, bits 0..9) | VR_F_*.
 *   pad = índice en VrScene.states (ventana de textura + recorte de la primitiva).
 */
#include <stdint.h>
#include "gpu.h"
#include "bridge.h"

enum {
    VR_F_FLIPPED = 0x0800, /* se permutaron dos vértices para dejar el frente (cara vista por la cámara del juego) antihorario */
    VR_F_2D = 0x1000,      /* primitiva 2D (pantalla PS1) */
    VR_F_LINE = 0x2000,    /* era una línea (cuadrilátero de 1 px) */
    VR_F_RECT = 0x4000     /* era un rect/sprite */
};

typedef struct {
    float pos[3];
    float uv[2];
    uint32_t rgba;
    uint16_t clut, tpage;
    uint16_t flags;
    uint16_t pad;          /* índice en VrScene.states */
} VrVtx;

typedef struct {
    uint32_t first, count; /* rango en VrScene.v (count múltiplo de 3: lista de triángulos) */
    uint8_t kind;          /* 0 opaco 3D, 1 semitransp 3D, 2 2D/HUD */
    uint8_t blend;         /* abr (0..3) si el batch es semitransparente; 0xFF opaco */
    uint16_t pad;
    uint32_t ev_end;       /* aplicar antes los eventos de VRAM de src->ops con índice < ev_end */
    uint32_t src_op;       /* índice en src->ops de la primera primitiva */
} VrBatch;

/* Estado por primitiva que no cabe en el vértice (deduplicado). */
typedef struct {
    uint8_t twmx, twmy, twox, twoy; /* ventana de textura E2 (unidades de 8 px); 0 = sin ventana */
    int16_t cx1, cy1, cx2, cy2;     /* recorte vigente (inclusivo) */
} VrState;

typedef struct {
    uint32_t n_ops, n_poly, n_rect, n_line;
    uint32_t n_poly3d, n_poly2d;
    uint32_t n_tris3d, n_tris2d, n_flipped;
    uint32_t drop_partial, drop_near, drop_degenerate, drop_huge, drop_nan;
    uint32_t n_events, n_batches;
} VrStats;

typedef struct {
    const VrVtx *v; uint32_t nv;
    const VrBatch *b; uint32_t nb;
    const GpuDrawList *src;   /* para eventos de VRAM en orden */
    float game_view[16];      /* cámara del juego -> mundo VR; COLUMNA-mayor (m[col*4+fila]); vértice de vista en
                               * unidades del juego (x der, y abajo, z adelante) -> metros. Traslación = ojo del juego. */
    int has_3d;
    /* --- ampliación (al final: no rompe a quien solo lea lo anterior) --- */
    const VrState *states; uint32_t nstates;
    float eye_vr[3];          /* posición de la cámara del juego en mundo VR (m) */
    float origin_game[3];     /* origen usado (unidades del juego, mundo del juego) */
    float scale;              /* metros por unidad usados */
    int world_valid;          /* 1 si la transformación vino del puente Y es fiable (EYE + has_player + BRIDGE_OK_ALL_CORE); 0 = no fiarse para mover
                               * el mundo (título/carga/zona). Con EYE válido pero world_valid 0 la matriz se aplica igualmente; sin EYE, cámara en el origen. */
    VrStats stats;
} VrScene;

typedef struct {
    float scale;              /* m por unidad del juego (def. 0.001) */
    float near_units;         /* descarta si algún vz < near_units (def. 24) */
    float max_edge_m;         /* descarta si alguna arista 3D supera esto en m (def. 120) */
    float min_area_m2;        /* descarta triángulos 3D de área menor (def. 1e-9) */
    int partial_to_2d;        /* 0 descartar polígonos parcialmente 3D (def.), 1 mandarlos a 2D */
    int origin_mode;          /* 0 jugador por fotograma (def.), 1 origin_game fijo */
    float origin_game[3];     /* modo 1 */
    float feet_below_pos;     /* unidades que están los pies por debajo de player_pos.y; <0 = auto (def.; ver "Escala" en .c) */
    int mirror_z;             /* -1 auto (def.), 0 no, 1 sí */
    int eye_normalize;        /* 1 (def.): vista->mundo = inversa exacta de eye_axes^T (la GTE usa esa matriz con escala ~1,002 por eje); 0 = eye_axes crudo */
    int eye_from_view_t;      /* 1: ojo = -R*view_t (traslación de la GTE) en vez de eye_pos (def. 0; ver bitácora) */
    int drop_2d_all;          /* 1 no emitir 2D (def. 0) */
} VrSceneOpts;

VrSceneOpts vr_scene_opts_default(void);
/* Origen (suelo bajo el jugador, mundo del juego) que usa origin_mode 0; útil para fijar origin_game (modo 1). */
void vr_scene_player_origin(const BridgeState *bs, const VrSceneOpts *o, float org[3]);

/* Ensambla la escena. dl y bs deben seguir vivos mientras se use `out` (bs puede ser NULL: cámara en el origen).
 * `out` apunta a memoria interna reutilizada: válida hasta la siguiente llamada o vr_scene_release(). Devuelve 0 si
 * va bien, <0 error (-1 argumentos, -2 sin memoria). */
int vr_scene_build(const GpuDrawList *dl, const BridgeState *bs, const VrSceneOpts *o, VrScene *out);
void vr_scene_release(void);

/* Utilidad: decodifica el borde de la ventana de estado; devuelve NULL si el índice no existe. */
const VrState *vr_scene_state(const VrScene *s, uint16_t idx);
