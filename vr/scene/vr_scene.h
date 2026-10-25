#pragma once
/* vr_scene: ensamblador de escena estéreo. Convierte una lista de dibujo capturada (GpuDrawList, runtime/gpu.h)
 * más el estado del puente (BridgeState) en geometría 3D en un espacio "mundo VR" (metros, Y arriba, mano
 * derecha) y en primitivas 2D de pantalla original (HUD, texto, menús), listas para que el renderer las pinte por ojo.
 * C99, sin dependencias de plataforma; no llama al runtime (solo lee las estructuras).
 *
 * ---------------------------------------------------------------------------------------------------------
 * Clases de salida (VrBatch.kind)
 *   0 = 3D opaco        triángulos en mundo VR (pos en METROS).
 *   1 = 3D semitransp.  igual; el renderer los pinta después de los opacos, sin escribir profundidad.
 *   3 = filtro de pantalla (ver "Clase 3" abajo): primitivas 2D que actúan sobre la imagen ya dibujada, NO son HUD.
 *   2 = 2D / HUD        triángulos en coordenadas de pantalla original (pos = {x, y, 0}, píxeles enteros con el offset
 *                       de dibujo ya sumado), para la textura del panel HUD. Rects y líneas se convierten a
 *                       triángulos (líneas: cuadrilátero de 1 px, centrado en los píxeles y alargado 0,5 px por extremo). VR_F_2D siempre activo en flags.
 *
 * Qué es 3D (contrato §28): polígonos cuyos vértices son TODOS vvalid (GpuVtx.vvalid: vx/vy/vz en espacio de vista del
 * juego, unidades del motor; incluye vértices saturados, detrás de cámara y derivados). Todo lo demás (rects, líneas,
 * polígonos sin posición de vista: texto, retratos, menús) es 2D. Reglas:
 *   - SAT: se usa vx,vy,vz (nunca x,y enteros, que están saturados).
 *   - BEHIND (vz<=0) en algún vértice: el polígono se recorta (Sutherland-Hodgman, por triángulo del par (0,1,2),(1,2,3)) contra el plano
 *     z = opts.near_units en espacio de vista, interpolando posición de vista, UV y color; cada triángulo da 0, 1 o 2 triángulos (un quad hasta 4).
 *     Los que no traen BEHIND y tienen vz < near_units se descartan (drop_near), como antes.
 *   - AMBIG junto a SAT/BEHIND: se descarta (drop_ambig) salvo cuadrilátero coherente o con un único AMBIG reconstruible por paralelogramo (recon_ambig). AMBIG solo: se acepta.
 *   - DERIVED: se acepta (opts.drop_derived=1 los descarta).
 * Un polígono con solo ALGUNOS vértices vvalid se descarta (opts.partial_to_2d lo manda a 2D, pero entonces aparecerá
 * como trozo de mundo en el HUD).
 *
 * Reconstrucción de parciales (opts.recon_partial): un quad con exactamente 3 vértices de vista limpios (vflags==0) y el 4.º sin posición,
 * pero con coordenadas de pantalla sin saturar (|x-dox-proj_ox| < 1000 px), recupera ese vértice: rayo (px/H, py/H, 1) cortado con el plano de
 * los otros tres. Se exige t>0, plano no paralelo al rayo y z del vértice entre 0,2x y 5x del rango de los otros. VrStats.n_recon los cuenta.
 *
 * Espacio del usuario (opts.user_map_*, ver el campo): el mundo VR sale en los ejes del juego con el ojo del motor bajo el origen; la cabeza del visor
 * vive en el espacio del usuario y la cámara del juego va girada theta_play (VrCamRecord) respecto de él. user_map_on aplica el giro R_y(theta+180)^T,
 * la escala de la calibración y el ancla vertical (el ojo del motor cae en p_ref), con lo que la escena queda donde la ve la cabeza (comprobado contra
 * vrcam_delta); `vr_usermap.h` (vr/camera) rellena los campos desde el registro de cámara.
 * Con engine_near_clip=0 (def.) no se recorta ni descarta contra el plano z=near_units de la cámara del motor: eso solo sirve para la vista del motor,
 * no para una cabeza que mira a otro lado. VrStats.n_unclipped cuenta los polígonos que traían vértices así.
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
 * Clase 3 = filtro de pantalla (contrato para el renderer)
 *   Qué es: primitivas 2D del juego que no dibujan contenido sino que RETOCAN el fotograma ya compuesto: p. ej. bajo el agua el juego
 *   vuelve a dibujar el búfer de pantalla sobre sí mismo en ~260 tiras de 2 px de alto texturizadas en 15 bits (tp=2) leyendo del propio
 *   búfer (desplazadas en vertical con una onda = distorsión) y moduladas por un color (verde = tinte submarino), y los fundidos/destellos
 *   son rectángulos semitransparentes que cubren la pantalla. En el panel HUD serían un recuadro enorme con una copia de la escena: MAL.
 *   Clasificación (solo primitivas 2D, es decir sin vvalid). OPT-IN: opts.screen_filter=0 por defecto (el renderer de app/ ya implementa la clase 3 con un post-proceso por ojo: ignora los batches kind 3 y
 *   aplica VrScene.filter; vr/host lo activa con screen_filter=1; otros consumidores que no lo implementen deben dejarlo en 0). Con 1:
 *     a) copia de pantalla: texturizada con tp>=2 (15 bits, sin CLUT) cuyo rectángulo fuente (tx+u, ty+v, tamaño de la primitiva) solape >= 50 % de su área con el
        área MOSTRADA (src->disp; NO el recorte vigente) y cuyo destino sea de pantalla: una banda fina (alto <= 16) de >= 45 % del ancho de pantalla (las tiras)
        o >= 50 % del área mostrada. Un parche local que copia la pantalla (brillo alrededor de un retrato) queda en la clase 2.
     b) recubrimiento: semitransparente (GPU_F_SEMI), SIN textura (color plano o degradado), que cubre >= opts.filter_min_cover (0,9) del área MOSTRADA
        (fundido, destello, tinte). Un panel pequeño con su propio recorte y fondo semitransparente (minimapa) o un polígono texturizado a pantalla completa
        (contenido) no lo son.
   NO son filtro: el borrado de pantalla (rect opaco negro a pantalla completa, primera operación de cada lista) ni el rect de máscara
 *   (negro, GPU_F_MASK_SET): son mantenimiento de la GPU; NO se emiten (opts.drop_clear=1 por defecto; VrStats.n_clear los cuenta) porque en el panel HUD
 *   (transparente por defecto) dejarían un fondo negro opaco a pantalla completa. Con drop_clear=0 siguen en la clase 2 y el renderer debe ignorarlos.
 *   Datos: los triángulos van en batches kind=3 con el MISMO formato que la clase 2 (pos = píxeles de pantalla original con offset sumado, z=0;
 *   uv = texels de la página de textura tpage; flags con VR_F_2D | VR_F_FILTER | VR_F_RECT si era rect), en orden de dibujo, dentro de cada
 *   época después de la clase 2. Además VrScene.filter resume lo que el renderer necesita sin interpretar 260 tiras:
 *     tint[3]   multiplicador por canal (color de la tira / 128; 1,1,1 si la textura va en crudo): rgb_ojo *= tint (aquí 0,5 / 1 / 0,5 = verde).
 *     dv[], du[]  por fila de pantalla original (índice = y - area_y; area = src->disp): desplazamiento en píxeles original de la fila fuente respecto a
 *                 la de destino (fuente = destino + d). rowcov[i]=1 si esa fila la cubre una tira. Se aplica a la imagen del ojo así: para el
 *                 píxel del ojo, calcular la fila original equivalente r (por su ángulo vertical: r = area_h/2 + tan(ang)*H_motor, H_motor = proj_h del
 *                 motor), tomar d(r) (interpolado) y muestrear la imagen del ojo en el ángulo equivalente a la fila r + d(r).
 *     cover_rgb[3], cover_abr  último recubrimiento semitransparente (color/255 y modo de mezcla original) para pintarlo como capa fija.
     proj_h, cx_col, cy_row  proyección del motor para convertir ángulos <-> filas de la imagen original: H (px por unidad de tangente) y centro óptico (columna, fila) RELATIVOS a area
                 (cx_col = rx - area_x, cy_row = ry - area_y con (rx,ry) el centro del mundo de arriba; por defecto 160, centro de area). Se rellenan siempre
                 (con o sin filtro). Modelo validado en camtest (error medio 0,9 sobre 255): para la fila y del ojo, con cy_img/f_eye = fila del punto principal y focal
                 vertical del ojo en píxeles (tangente = (y-cy_img)/f_eye):  r = cy_row + proj_h*(y-cy_img)/f_eye;  d = dv[r] interpolado entre centros de fila (0 donde
                 rowcov=0);  y_src = cy_img + f_eye*(r+d-cy_row)/proj_h;  out = tint * imagen_sin_filtrar(x, y_src)  (UNA pasada sobre la imagen sin filtrar, NO emular la copia
                 en sitio: re-tintaría las filas con dv<0). du no se aplica (0 en la práctica). El tinte multiplica en el espacio gamma original y se aplica a toda la vista.
 *   El renderer aplica el filtro UNA vez por ojo, como capa fija en pantalla (anclada a la cabeza, no al mundo) DESPUÉS de dibujar 3D opaco y
 *   semitransparente y ANTES de componer el panel HUD (que no se distorsiona). Si filter.active==0 no hay nada que aplicar.
 *   Con opts.screen_filter=0 estas primitivas vuelven a la clase 2 (comportamiento anterior).
 *
 * Clase 3b = objetos equipados (gafas de visión nocturna/térmica, máscara de gas, caja, mira) — misma clase 3, tres piezas nuevas de VrFilter
 *   a) varios recubrimientos: covers_*[] (los 4 últimos, en orden; p. ej. el tinte aditivo de las gafas y un destello/fundido a la vez). El 1.º tinte NV es verde aditivo
 *      (56,120,40), el térmico rojo aditivo (80,0,0); la paleta ya viene convertida por el propio juego (se reescribe la CLUT en VRAM), no hay que hacer nada en 3D.
 *   b) líneas de exploración: rects semitransparentes SIN textura, de >= 90 % del ancho de la imagen mostrada y alto <= 2, repetidos cada `scan_pitch` filas (gafas: 1 fila cada 2, resta 64/255).
 *      No salen en el panel: n_scan/scan_* las describen para repintarlas en la vista (fila original equivalente r = cy_row + proj_h*tan; cobertura = (r - scan_phase) mod pitch < scan_h).
 *   c) máscara de visión (frame): con >= 8 polígonos planos de restar-blanco u opacos negros Y >= 8 polígonos degradados semitransparentes de resta, SIN textura, 2D (la mira
 *      de la máscara de gas, de la caja y de otros visores): todos se emiten a kind 3 y se vuelcan sumados en filter.frame (resta por celda, 160x128). El renderer la aplica en
 *      el espacio angular de la cabeza (celda = fila/columna original equivalentes), con resta total (255) fuera de la imagen original; así el marco rodea TODO el campo del visor en vez
 *      de quedar en el panel flotante.
 *
 * Desplazamiento de proyección por primitiva (OFX/OFY) y centro del mundo
 *   La proyección del motor es x_pantalla = OFX + H*vx/vz. Los sprites de resplandor/halo ponen OFX/OFY en su centro de pantalla y proyectan solo el desplazamiento local
 *   (vx,vy = +-1000 a profundidad vz), así que (vx,vy,vz) NO es una posición de vista del mundo (pegaba el resplandor al centro de la vista). Para cada lista se calcula
 *   una vez el CENTRO DEL MUNDO (rx,ry) = moda de (proj_ox+dox, proj_oy+doy) entre los polígonos 3D OPACOS con proj_h>0 (todos vvalid; si no hay ninguno, entre cualquier
 *   polígono 3D; si tampoco, el primer polígono con proj_h>0) y para CADA vértice de un polígono 3D con proj_h>0 (opaco o semitransparente) la posición de vista es
 *     p.x = vx + (proj_ox+dox-rx)*vz/proj_h,  p.y = vy + (proj_oy+doy-ry)*vz/proj_h,  p.z = vz
 *   (para los polígonos normales proj_ox+dox == rx y no cambia nada). Con proj_h==0 no se corrige. VrStats.n_ofs_fix cuenta los polígonos corregidos.
 *
 * Realimentación (GpuOp.fbk, gpu.h): polígonos/rects 2D texturizados que muestrean UN texel del frame buffer (área de dibujo/visualización) que se
 *   está dibujando. La VRAM del renderer solo recibe eventos (fill/subida/copia), no lo pintado por primitivas, así que leerían un valor viejo.
 *   Aquí salen SIN textura (flags sin TEXTURED/RAW), con color = color de vértice * texel / 128 (crudo: el texel), semitransparentes solo si
 *   GPU_F_SEMI y el bit 15 del texel; texel 0 = transparente => no se emiten (VrStats.n_fb_clear). Solo clases 2D (los 3D con fbk se tratan como antes).
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
 *   clut = (clx/16) | (cly << 6); tpage = (tx/64) | ((ty/256)<<4) | (abr<<5) | (tp<<7)  (formato original GP0(E1h)).
 *   flags = GPU_F_* (gpu.h, bits 0..9) | VR_F_*.
 *   pad = índice en VrScene.states (ventana de textura + recorte de la primitiva).
 */
#include <stdint.h>
#include "gpu.h"
#include "bridge.h"

enum {
    VR_F_FLIPPED = 0x0800, /* se permutaron dos vértices para dejar el frente (cara vista por la cámara del juego) antihorario */
    VR_F_2D = 0x1000,      /* primitiva 2D (pantalla original) */
    VR_F_LINE = 0x2000,    /* era una línea (cuadrilátero de 1 px) */
    VR_F_RECT = 0x4000,    /* era un rect/sprite */
    VR_F_FILTER = 0x8000   /* primitiva de filtro de pantalla (clase 3) */
};

/* Causa/origen de cada triángulo de la salida (VrScene.tri_cause, un byte por triángulo en el orden de VrScene.v): diagnóstico y probador. 0 = ninguna (normal o 2D). */
enum {
    VR_TC_AMBIG = 1,     /* algún vértice con asociación dudosa (GPU_VF_AMBIG) */
    VR_TC_SAT = 2,       /* algún vértice saturado en pantalla (GPU_VF_SAT) */
    VR_TC_BEHIND = 4,    /* algún vértice detrás de la cámara del motor */
    VR_TC_RECON = 8,     /* algún vértice reconstruido (parcial / esquinas de diagonal) */
    VR_TC_DERIVED = 16,  /* algún vértice derivado (esquina de cartel sin GTE) */
    VR_TC_OFS = 32,      /* polígono con desplazamiento de proyección distinto del centro del mundo (sprites, destellos) */
    VR_TC_CLIPPED = 64   /* recortado contra el plano cercano */
};

typedef struct {
    float pos[3];
    float uv[2];
    uint32_t rgba;
    uint16_t clut, tpage;
    uint16_t flags;
    uint16_t pad;          /* índice en VrScene.states */
} VrVtx;

/* VrBatch.skip_if_native. Los mismos valores que NS_SKIP_* (vr/native/ns_bridge.h): d.skip & batch.skip_if_native != 0 => no dibujar. */
enum { VR_SKIP_WORLD = 1, VR_SKIP_FX = 2 };

typedef struct VrBatchTag {
    uint32_t first, count; /* rango en VrScene.v (count múltiplo de 3: lista de triángulos) */
    uint8_t kind;          /* 0 opaco 3D, 1 semitransp 3D, 2 2D/HUD, 3 filtro de pantalla */
    uint8_t blend;         /* abr (0..3) si el batch es semitransparente; 0xFF opaco */
    uint8_t skip_if_native;/* VR_SKIP_*: 0 = siempre se dibuja; VR_SKIP_WORLD (kind 0) / VR_SKIP_FX (kind 1) = el batch NO se dibuja cuando el consumidor pinta ese 3D desde la escena nativa (ns_native_decide) */
    uint8_t pad;
    uint32_t ev_end;       /* aplicar antes los eventos de VRAM de src->ops con índice < ev_end */
    uint32_t src_op;       /* índice en src->ops de la primera primitiva */
} VrBatch;
/* 1 si el batch no se dibuja con la decisión `skip` del consumidor (NsDecision.skip; 0 = dibujarlo todo). */
static inline int vr_batch_skipped(const struct VrBatchTag *b, uint32_t skip) { return (b->skip_if_native & skip) != 0; }

/* Estado por primitiva que no cabe en el vértice (deduplicado). */
typedef struct {
    uint8_t twmx, twmy, twox, twoy; /* ventana de textura E2 (unidades de 8 px); 0 = sin ventana */
    int16_t cx1, cy1, cx2, cy2;     /* recorte vigente (inclusivo) */
} VrState;

/* Causas de descarte de la red de seguridad (VrStats.incoh[]). */
enum { VR_INC_MODEL = 0, VR_INC_EDGE, VR_INC_FAR, VR_INC_STRETCH, VR_INC_N };

typedef struct {
    uint32_t n_ops, n_poly, n_rect, n_line;
    uint32_t n_poly3d, n_poly2d;
    uint32_t n_tris3d, n_tris2d, n_flipped;
    uint32_t drop_partial, drop_near, drop_degenerate, drop_huge, drop_nan;
    uint32_t n_events, n_batches;
    /* --- §28: recorte cercano y descartes por causa (al final: no rompe el layout anterior) --- */
    uint32_t n_poly_behind;   /* polígonos 3D con algún vértice GPU_VF_BEHIND */
    uint32_t n_clipped;       /* triángulos recortados contra el plano cercano */
    uint32_t drop_behind;     /* triángulos enteros detrás del plano cercano (en polígonos con BEHIND) */
    uint32_t drop_ambig;      /* polígonos con AMBIG junto a SAT/BEHIND */
    uint32_t drop_derived;    /* polígonos con vértices DERIVED (solo con opts.drop_derived) */
    uint32_t n_filter_polys, n_filter_rects; /* primitivas 2D de filtro de pantalla mandadas a kind 3 (copias de pantalla) */
    uint32_t n_filter_cover;  /* recubrimientos semitransparentes a pantalla completa mandados a kind 3 */
    uint32_t n_filter_tris;   /* triángulos kind 3 */
    uint32_t n_fb_baked;      /* triángulos 2D de primitivas de realimentación convertidos a color plano (GpuOp.fbk; ver "Realimentación") */
    uint32_t n_fb_clear;      /* primitivas de realimentación con texel transparente (0): descartadas */
    uint32_t n_clear;         /* rects opacos sin textura a pantalla completa (borrado/máscara); no se emiten con opts.drop_clear (def.) */
    uint32_t n_ofs_fix;       /* polígonos 3D cuyo desplazamiento de proyección (OFX/OFY) difería del centro del mundo y se corrigió */
    uint32_t n_ref_group;     /* polígonos del grupo modal (centro del mundo) */
    uint32_t n_filter_scan;   /* rects finos semitransparentes de ancho de pantalla (líneas de exploración de las gafas) mandados a kind 3 */
    uint32_t n_filter_frame;  /* polígonos de la máscara de visión (marco de máscara/caja/mira) mandados a kind 3 y volcados en filter.frame */
    uint32_t n_recon;         /* §49: polígonos de 4 vértices con un vértice sin posición de vista reconstruido (opts.recon_partial) */
    uint32_t n_unclipped;     /* §49: triángulos emitidos con algún vértice por detrás/pegado al ojo del motor (z < near_units) sin recortar (engine_near_clip=0) */
    uint32_t n_filter_ghost;  /* primitivas 2D que mezclan el fotograma anterior (sprite que lee del búfer mostrado durante el dibujo): kind 3, sin efecto en el panel */

    /* --- §56: coherencia de vértices (modelo de la GTE) y red de seguridad --- */
    uint32_t chk_vtx;         /* vértices de polígonos 3D comprobados contra el modelo de la GTE (los derivados/reconstruidos no se comprueban) */
    uint32_t chk_bad[4];      /* ... los que NO reproducen su SXY (más de coh_px): [0] normales, [1] saturados, [2] detrás de la cámara, [3] ambiguos */
    uint32_t drop_incoh;      /* triángulos descartados por la red de seguridad (opts.coherence) */
    uint32_t incoh[VR_INC_N]; /* ... por causa (VR_INC_*) */
    uint32_t n_near_eye;      /* modo gpu (opts.eye_near_m): triángulos descartados por tener un vértice a menos de eye_near_m del ojo del motor */
    uint32_t n_line3d;        /* opts.line3d_px: líneas con los dos extremos de vista emitidas como cuadrilátero 3D (2 triángulos en n_tris3d) */
} VrStats;

#define VR_FILTER_ROWS 512
#define VR_FILTER_COVERS 4
#define VR_FRAME_W 160
#define VR_FRAME_H 128
typedef struct {
    int active;                 /* 1 si hay primitivas de clase 3 */
    int n_strips, n_polys, n_cover;
    int area[4];                /* x, y, w, h de pantalla original (src->disp) */
    float tint[3];              /* multiplicador por canal (def. 1,1,1) */
    int nrows;                  /* filas con tira */
    float dv[VR_FILTER_ROWS], du[VR_FILTER_ROWS];  /* desplazamiento fuente-destino por fila (px original) */
    uint8_t rowcov[VR_FILTER_ROWS];
    float cover_rgb[3]; int cover_abr;  /* último recubrimiento semitransparente; cover_abr<0 si no hay */
    /* proyección del motor (ver "Clase 3"): se rellenan siempre. Defecto 160 / centro de area (160,128 con 320x256). */
    float proj_h;               /* H del motor (px por unidad de tangente) */
    float cx_col, cy_row;       /* centro óptico: columna y fila relativas a area[0], area[1] */
    int proj_ok;                /* 1 si proj_h/cx_col/cy_row salen de polígonos de la lista (centro del mundo); 0 = valores por defecto (la lista no tiene proyección) */
    /* --- objetos equipados (gafas, máscara, caja): ver "Clase 3b" --- */
    int n_covers;               /* recubrimientos semitransparentes en orden de dibujo (los VR_FILTER_COVERS últimos); cover_rgb/cover_abr = el último */
    float covers_rgb[VR_FILTER_COVERS][3]; int covers_abr[VR_FILTER_COVERS];
    int n_scan;                 /* líneas de exploración detectadas (0 = no hay) */
    float scan_rgb[3]; int scan_abr;   /* color/255 y modo de mezcla original de cada línea (gafas: 64/255 y resta) */
    float scan_pitch, scan_phase, scan_h;   /* periodo (filas original), fila de la primera línea mod periodo (relativa a area[1]) y alto de cada línea */
    int scan_after;             /* nº de covers_*[] (de los conservados) dibujados ANTES de las líneas: el orden importa (p. ej. tinte, líneas, y luego el velo gris de la mira) */
    int frame_on;               /* 1 si hay máscara de visión: frame[] válido */
    const uint8_t *frame;       /* frame_w*frame_h bytes: resta (0..255 = 0..1, por canal, en espacio gamma original) que la máscara aplica a cada celda de la imagen mostrada
                                 * (arriba-izquierda primero; la imagen mostrada area[2]x area[3] se divide en frame_w x frame_h celdas). Fuera de la imagen: 255. Misma vida que v/b */
    int frame_w, frame_h;
    float frame_rgb[3];         /* color (gamma, 0..1) que se suma donde la máscara cierra del todo (resta ~255): negro (0,0,0) salvo con la caja equipada (interior de cartón oscuro) */
} VrFilter;

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
    VrFilter filter;          /* resumen de la clase 3 (ver "Clase 3") */
    int user_space;           /* 1 si el mundo de salida está en el espacio del usuario (opts.user_map_on aplicado) */
    float user_R[9], user_k, user_eye_h, user_p_ref[3];   /* la corrección aplicada (copia de opts.user_map_*; válida si user_space): P = R (k diag(1,1,zs) s) + p_ref - (0,eye_h,0) */
    /* --- cinemáticas del motor (vr/host/vr_cine.h; lo rellena el host, vr_scene_build lo deja a cero) --- */
    int32_t cine_active;      /* 1: esta escena es de una cinemática del motor con 3D (presentación en pantalla 3D / dentro de la escena) */
    uint32_t cine_shot;       /* nº de plano (sube en cada corte de cámara y al empezar la cinemática) */
    int32_t cine_cut;         /* 1 si esta escena es el primer fotograma de un plano nuevo */
    int32_t cine_cover;       /* nº de recubrimientos semitransparentes a pantalla completa (fundidos) detectados en la lista */
    const uint8_t *tri_cause; /* VR_TC_* de cada triángulo de v (nv/3 bytes; misma vida que v) */
    /* --- render nativo (lo rellena el host; vr_scene_build lo deja a cero) --- */
    int32_t native_3d;        /* 1: la escena admite el 3D nativo (cámara VR en primera persona, no cinemática, con 3D de mundo): sus batches 3D (kind 0/1) están MARCADOS con skip_if_native pero siguen todos presentes; el que dibuja decide con ns_native_decide (vr/native/ns_bridge.h) cuáles saltar. 0: no marcar nada, dibujar todo */
    const uint32_t *tri_src;  /* índice de la primitiva de la lista (la de `src` de vr_scene_build) de origen de cada triángulo de v (nv/3 entradas; misma vida que v). El host lo usa para ocultar por triángulo lo que el render nativo confirma */
    const uint64_t *tri_fx;   /* (lo rellena el host; vr_scene_build lo deja a NULL) identidad de efecto de cada triángulo: ns_fx_item_key, NSFX_KEY_MESH (1) para mallas, 0 = nada que ocultar. Lo oculta el render nativo solo si lo confirma */
    /* --- destino de cada primitiva y capa 2D fiel (vr_layer2d.h) --- */
    const uint8_t *op_dest;   /* por op de src->ops (n_op_dest = src->nops): 2 = salió al panel 2D (batches kind 2), 3 = filtro de pantalla (kind 3), 0 = no emitida (3D, descartada, evento). Memoria de vr_scene (reutilizada en la siguiente llamada) */
    uint32_t n_op_dest;
    /* Lo rellena el host (vrhost.c) con vl2d_run: RGBA8 NO premultiplicado de la zona mostrada ampliada (hud2d_w x hud2d_h, filas de arriba abajo). NULL = no hay: el renderer
     * dibuja los batches kind 2 como antes. hud2d_full = 1: es el fotograma ENTERO (todas las primitivas sobre el área mostrada, opaco: pantalla de cine), no solo la capa 2D. */
    const uint8_t *hud2d;
    int32_t hud2d_w, hud2d_h, hud2d_full;
    uint32_t hud2d_seq;
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
    /* --- §28 (al final) --- */
    int drop_derived;         /* 1 descarta polígonos con vértices GPU_VF_DERIVED (esquinas de cartel aproximadas); def. 0 = aceptar */
    int screen_filter;        /* 1 clasifica copias de pantalla y recubrimientos como clase 3; 0 (def.) los deja en la 2 */
    float filter_min_cover;   /* fracción mínima del área de dibujo para un recubrimiento semitransparente (def. 0,9) */
    int drop_clear;           /* 1 (def.) no emite los rects opacos sin textura que cubren >= 90 % del área mostrada (borrado de pantalla / máscara): en el panel
                               * HUD lo dejarían opaco. Se cuentan en VrStats.n_clear. 0 = emitirlos en la clase 2 (comportamiento previo) */
    int clip_all_near;        /* 1 recorta contra el plano cercano TODO polígono con algún vz < near_units (def. 0: solo los que traen
                               * GPU_VF_BEHIND; el resto con vz < near_units se descarta como siempre, salida idéntica a la previa) */
    /* --- §49: recorte del motor y reconstrucción (al final: no rompe el layout) --- */
    int engine_near_clip;     /* 1 = comportamiento anterior: recorta/descarta contra el plano z = near_units de la CÁMARA DEL MOTOR (una cabeza VR que mira a otro lado ve esa
                               * geometría). 0 (def.): se emite todo polígono con posiciones de vista válidas, también con vértices detrás o pegados al ojo del motor; el plano cercano
                               * lo recorta el renderer con el de la cabeza. Siguen los límites de arista (max_edge_m) y de área */
    int recon_partial;        /* 1 (def.): polígonos de 4 vértices con exactamente 3 de vista limpios y el 4.º sin posición: el 4.º se reconstruye cortando su rayo de
                               * pantalla con el plano de los otros tres (ver "Reconstrucción de parciales") en vez de descartar el polígono entero */
    int recon_ambig;          /* 1 (def.): un polígono AMBIG junto a SAT/BEHIND ya no se descarta si sus vértices están en un orden de magnitud coherente (ver .c) */
    /* --- correspondencia mundo -> espacio del usuario (cámara VR; al final: ceros = sin cambio) ---
     * Con la cámara VR el mundo del juego no está alineado con el espacio recentrado del usuario: hay un giro (rumbo del personaje
     * menos giro de la cabeza al recentrar + giro del stick), la cabeza del usuario ya no está sobre el suelo del juego (sentado o de pie,
     * el ojo del personaje se ancla a la cabeza) y la escala usuario->mundo puede no ser 1. Si user_map_on, cada vértice (ya en el mundo VR
     * descrito arriba, metros, tras la reflexión de Z si la hubo) se lleva a:  P = R * (k * (F * s) - (0, eye_h_m, 0)) + p_ref
     * con s el vértice, F = diag(1,1,reflexión) (se deshace la reflexión de Z, que el usuario no necesita), R = user_R (fila-mayor, giro
     * mundo -> usuario), k = metros de usuario por metro de mundo (1 / escala usuario->mundo), eye_h_m = altura del ojo del motor sobre el
     * origen YA EN METROS DE USUARIO (= k * altura en m de mundo; el ojo del motor, s = (0,h,0), cae en p_ref) y p_ref = cabeza del usuario
     * al recentrar. Ver vrhost.c (user_map_from_record) y bitácora. */
    int user_map_on;
    float user_R[9];
    float user_k;
    float user_eye_h_m;
    float user_p_ref[3];
    /* --- fotograma anterior (al final) --- */
    int drop_ghost;           /* 1 (def.): un sprite 2D texturizado de 15 bits que lee del búfer alterno (fotograma anterior: estela o fundido encadenado) y cubre >= 20 % de la pantalla
                               * no se emite al panel (kind 3, VrStats.n_filter_ghost): la VRAM del renderer no tiene ese fotograma y mostraría restos de lo último subido. Independiente de screen_filter */

    /* --- §56: coherencia de vértices y red de seguridad (al final: ceros = sin red) --- */
    int coherence;            /* 1 (def.): red de seguridad: se descartan (y se cuentan en VrStats.incoh[]) los triángulos con vértices incoherentes: SXY que no reproduce el modelo de la GTE
                               * (coh_px), arista mayor que coh_edge_m, vértice a más de coh_far_m del ojo del motor o estiramiento extremo (coh_stretch). 0 = solo se cuenta (chk_*) */
    float coh_px;             /* tolerancia (px) del modelo de la GTE; def. 2,5 */
    float coh_edge_m;         /* arista máxima (m del mundo del juego); def. 60 */
    float coh_far_m;          /* distancia máxima de un vértice al ojo del motor (m); def. 150 */
    float coh_stretch;        /* reservado (0 = desactivado) */
    float coh_synth_edge_m;   /* arista máxima (m) de los triángulos con vértices reconstruidos o derivados (no salen de la GTE); def. 20 */
    float coh_flag_edge_m;    /* arista máxima (m) de los triángulos con algún vértice dudoso (ambiguo / saturado / detrás / reconstruido / derivado / desplazamiento de proyección); def. 32 */

    /* --- modo gpu (el 3D de la lista de la GPU con vértices de vista de la GTE; al final: 0 = comportamiento anterior) ---
     * eye_near_m > 0: (1) se descarta todo triángulo 3D pequeño (arista máx < 0,5 m) con TODOS sus vértices a menos de eye_near_m metros del ojo del motor (distancia euclídea en el espacio de vista, en metros del espacio de
     * salida: opts.scale, y opts.user_k si user_map_on); VrStats.n_near_eye. Es lo que pega al ojo (efectos, partes del propio cuerpo) y nunca debe verse proyectado sobre la cara.
     * (2) el recorte contra el plano z = near_units se hace SIEMPRE (como engine_near_clip = 1 con clip forzado): un triángulo parcialmente delante se recorta (n_clipped), uno entero
     * detrás se descarta (drop_behind), y un triángulo que lleva GPU_VF_BEHIND en algún vértice pero no tiene ninguno realmente detrás del plano (dato incoherente) se descarta también (drop_behind). */
    float eye_near_m;
    /* line3d_px > 0: una línea (GPU_OP_LINE) con los DOS extremos de vista fiables (vvalid, sin banderas salvo SAT, y que reproducen su SXY con el modelo de la GTE a coh_px) y proj_h > 0 sale
     * como cuadrilátero 3D (2 triángulos, kind 0/1 según SEMI, sin textura) en vez de al plano 2D: grosor line3d_px píxeles originales a la profundidad de cada extremo, alargado medio
     * grosor en cada extremo (como la línea 2D), recortado contra z = near_units. Si no cumple, sigue yendo al plano 2D. 0 (def.) = siempre 2D. */
    float line3d_px;
} VrSceneOpts;

VrSceneOpts vr_scene_opts_default(void);

/* Ventana de la proyección del motor sobre la imagen mostrada (area[2] x area[3]) en tangentes de vista: columna 0 .. area_w a -cx_col/H .. (area_w-cx_col)/H, fila 0 .. area_h
 * a -cy_row/H .. (area_h-cy_row)/H. La pantalla 3D de las cinemáticas usa exactamente esta ventana para que el 3D registre pixel a pixel con el panel 2D (que cubre area). ok = 0 si
 * el filtro no trae proyección (proj_ok == 0), H o el área no son válidos. */
typedef struct { int ok; float left, right, up, down; } VrScreenWindow;
VrScreenWindow vr_scene_screen_window(const VrFilter *f);

/* Depuración/medición: destino de cada primitiva de la lista. Si se registra un búfer (uno por op de src->ops), vr_scene_build lo rellena. */
enum {
    VR_FATE_NONE = 0,      /* no es un polígono/rect/línea (eventos de VRAM) o no se evaluó */
    VR_FATE_3D = 1,        /* polígono 3D emitido (algún triángulo) */
    VR_FATE_2D = 2,        /* polígono 2D (sin posición de vista), rect o línea */
    VR_FATE_PARTIAL = 3,   /* descartado: solo algunos vértices con posición de vista */
    VR_FATE_AMBIG = 4,     /* descartado: AMBIG junto a SAT/BEHIND */
    VR_FATE_NEAR = 5,      /* descartado: vz < near_units sin BEHIND */
    VR_FATE_BEHIND = 6,    /* descartado: enteros detrás del plano cercano tras recortar */
    VR_FATE_HUGE = 7,      /* descartado: arista mayor que max_edge_m */
    VR_FATE_DEGEN = 8,     /* descartado: área nula */
    VR_FATE_NAN = 9,       /* descartado: coordenadas no finitas */
    VR_FATE_DERIVED = 10,  /* descartado: DERIVED con opts.drop_derived */
    VR_FATE_RECON = 11,    /* emitido tras reconstruir un vértice sin posición o un AMBIG (recon_partial / recon_ambig) */
    VR_FATE_INCOH = 12,    /* descartado por la red de seguridad de coherencia de vértices (opts.coherence) */
    VR_FATE_COUNT
};
void vr_scene_set_fate(uint8_t *buf, uint32_t cap);   /* NULL para desactivar */
/* (pruebas) Rellena buf[k] con el índice de op de origen del triángulo k de VrScene.v (orden de salida). NULL para desactivar. */
void vr_scene_set_tri_src(uint32_t *buf, uint32_t cap);
/* Origen (suelo bajo el jugador, mundo del juego) que usa origin_mode 0; útil para fijar origin_game (modo 1). */
void vr_scene_player_origin(const BridgeState *bs, const VrSceneOpts *o, float org[3]);

/* Ensambla la escena. dl y bs deben seguir vivos mientras se use `out` (bs puede ser NULL: cámara en el origen).
 * `out` apunta a memoria interna reutilizada: válida hasta la siguiente llamada o vr_scene_release(). Devuelve 0 si
 * va bien, <0 error (-1 argumentos, -2 sin memoria). */
int vr_scene_build(const GpuDrawList *dl, const BridgeState *bs, const VrSceneOpts *o, VrScene *out);
void vr_scene_release(void);

/* Utilidad: decodifica el borde de la ventana de estado; devuelve NULL si el índice no existe. */
const VrState *vr_scene_state(const VrScene *s, uint16_t idx);
