#pragma once
/* ns_adapt: adaptador NativeScene (ns_types.h + almacén de mallas) -> triángulos NsRTri del rasterizador de referencia (ns_cpuref.h).
 * Herramienta/validador de escritorio; no forma parte de la app. Hace en CPU lo que hará el renderer nativo en GPU:
 *   - posición de mundo de cada vértice = world_pieza * v_local; los vértices soldados (NsVert.weld) usan la posición transformada del vértice
 *     del padre con la matriz del padre; las soldaduras encadenadas usan la posición ya soldada del padre (costura entre articulaciones, docs §3.5);
 *   - color de esquina: array precalculado (NsBaked de la pieza) tal cual; objetos dinámicos con la luz de la instancia (matrices de RAM) y las normales del
 *     modelo mediante ns_light (exacto al motor, rotando las luces al espacio de la pieza con ns_light_to_local); UNLIT o sin luz: 128 (neutro);
 *   - cuadriláteros con la división del hardware: (A,B,D) y (B,D,C) (A,B,C,D en perímetro); triángulos (C==D): (A,B,C);
 *   - SIN culling por defecto: el rasterizador dibuja ambos anversos (el z-buffer resuelve); `cull_eye` activa el descarte de reversos solo para la vista del juego.
 * Sin VRAM las caras no llevan textura: color de vértice (x exposición) mezclado con un tinte estable por (tpage, clut) para que se vea la estructura
 * de materiales; las caras sin textura quedan sin tinte. Con VRAM (1024x512 u16) se texturiza como el hardware. */
#include "ns_types.h"
#include "ns_cpuref.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint16_t *vram;    /* NULL = sin textura (ver arriba) */
    float exposure;          /* sin VRAM: multiplicador del color de vértice (0 = 1,0) */
    int no_tint;             /* sin VRAM: desactiva el tinte por tpage/clut */
    uint32_t skip_key;       /* instancia a omitir (p. ej. el cuerpo propio al mirar desde la cabeza); 0 = ninguna */
    int no_shade;            /* no calcular luz dinámica (neutro 128) */
    const float *cull_eye;   /* NULL = sin descarte de reversos (panoramas). Si no, posición del ojo del juego en el mundo: se descartan las caras no doble cara que el motor
                              * no dibujaría (regla del anverso del contrato: dot(A - ojo, (B-A)x(D-A)) > 0). Sólo para comparar con el fotograma real. */
    struct NsAdaptSrc **src_out;   /* herramientas: si no es NULL recibe (malloc) un NsAdaptSrc por triángulo de salida, en el mismo orden */
} NsAdaptOpts;

/* Origen de un triángulo de salida (solo diagnóstico): instancia, pieza (índice absoluto en NsScene.piece), cara y mitad del cuadrilátero. */
typedef struct NsAdaptSrc { uint32_t inst, piece, face, half, mesh_id, key, piece_rel, mflags; int32_t vis; } NsAdaptSrc;   /* key = NsInstance.key, piece_rel = índice dentro de la instancia, mflags = NsMesh.flags, vis = NsInstance.vis_state */

typedef struct {
    size_t pieces_in, pieces_drawn;
    size_t drop_hidden_piece, drop_no_mesh, drop_skip_inst;
    size_t faces_in, faces_out_tris, quads, tris;
    size_t drop_backface;            /* caras descartadas por reverso (sólo con cull_eye) */
    size_t drop_degenerate;          /* triángulos con vértices repetidos o área nula/no finita */
    size_t welded_verts;             /* vértices soldados resueltos con el padre */
    size_t faces_baked, faces_dyn, faces_flat;
    size_t faces_textured;
} NsAdaptStats;

/* Devuelve el nº de triángulos; *out se reserva con malloc (liberar con free). 0 y *out = NULL si no hay nada o sin memoria. */
size_t ns_adapt_scene(const NsScene *sc, const NsMeshStore *st, const NsAdaptOpts *opt, NsRTri **out, NsAdaptStats *stats);

#ifdef __cplusplus
}
#endif
