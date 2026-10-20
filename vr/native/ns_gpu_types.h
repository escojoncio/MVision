/* ns_gpu_types: estructuras compartidas entre el empaquetador en C (ns_gpu.c), el renderer Metal (app/Sources/Native/NativeRenderer.swift) y los
 * shaders (app/Sources/Native/NativeShaders.metal). Mismo layout en C y en MSL: solo escalares y arrays de escalares (nada de vectores en las
 * estructuras que viajan por búfer), para que el tamaño y los desplazamientos sean idénticos en los tres lados. Las comprobaciones de tamaño
 * están al final (C) y en NativeRenderer.init (Swift). */
#ifndef NS_GPU_TYPES_H
#define NS_GPU_TYPES_H

#ifdef __METAL_VERSION__
#include <metal_stdlib>
#include <simd/simd.h>
typedef unsigned int ns_u32;
typedef unsigned short ns_u16;
typedef unsigned char ns_u8;
#else
#include <stdint.h>
typedef uint32_t ns_u32;
typedef uint16_t ns_u16;
typedef uint8_t ns_u8;
#if defined(__APPLE__)
#include <simd/simd.h>
#define NS_GPU_HAVE_SIMD 1
#endif
#endif
#ifdef __METAL_VERSION__
#define NS_GPU_HAVE_SIMD 1
#endif

/* Índices de búfer / constantes de función */
#define NSG_BUF_VERTS   0
#define NSG_BUF_CORNERS 1
#define NSG_BUF_PIECES  2
#define NSG_BUF_UNI     3
#define NSG_BUF_PIECEIX 4    /* (en desuso: el índice de pieza de los opacos por pieza es el instance_id = baseInstance de la llamada) */
#define NSG_BUF_ORDER   4    /* pase de semitransparentes: NsGpuOrder[] ya ordenado de atrás adelante */
#define NSG_BUF_FX      0    /* efectos (ns_vs_fx*): NsGpuFxVert[] (usa el índice 0; no se mezcla con las mallas en una misma llamada) */
#define NSG_FC_PASS     10   /* uint: 0 opaco, 1 semitransparente */
#define NSG_FC_LINEAR   11   /* bool: destino lineal/sRGB (convertir gamma -> lineal al escribir) */
#define NSG_FC_FLAT     12   /* bool: malla sin caras texturizadas ni semitransparentes: el fragmento no lee la VRAM ni descarta (early-z completo) */

/* Marcas de esquina */
#define NSG_CF_TEX    1u     /* cara texturizada */
#define NSG_CF_SEMI   2u     /* malla semitransparente */
#define NSG_CF_ABR(f) (((f) >> 2) & 3u)   /* bits 2-3: modo de mezcla */
#define NSG_CF_PAINT  16u    /* color precalculado válido en .paint */

/* Marcas de pieza */
#define NSG_PF_SHADE  1u     /* se sombrea con la luz de la pieza (si la esquina no lleva color precalculado) */
#define NSG_PF_VFOG   2u     /* niebla por vértice (escenario); si no, `fog` por pieza (objeto) */
#define NSG_PF_DRAW   4u     /* se dibuja (malla presente y pieza visible) */

#define NSG_NO_WELD 0xFFu
#define NSG_INTENSITY_MAX 7.99975586f   /* = NS_INTENSITY_MAX (0x7FFF/4096) de ns_types.h */

/* Vértice local (unidades del motor). weld: NSG_NO_WELD o índice de vértice del modelo PADRE (NsPiece.parent) cuya posición transformada lo sustituye. */
typedef struct { float p[3]; ns_u32 weld; } NsGpuVert;                                              /* 16 B */

/* Esquina de triángulo (3 por triángulo; un cuadrilátero = 2 triángulos (A,B,D) y (B,C,D): diagonal B-D como el hardware). */
typedef struct {
    float n[3];          /* normal local unitaria */
    ns_u32 v;            /* índice ABSOLUTO del vértice en el pool de vértices */
    ns_u32 paint;        /* color precalculado RGBA8 (r | g<<8 | b<<16), 128 = neutro; solo con NSG_CF_PAINT */
    ns_u8 u, t;          /* texels 0..255 */
    ns_u16 tpage;
    ns_u16 clut;
    ns_u16 flags;        /* NSG_CF_* */
} NsGpuCorner;                                                                                      /* 28 B */

/* Pieza dibujable (una por NsPiece de la escena, en el mismo orden). */
typedef struct {
    float w[12];         /* local -> mundo del motor, 3x4 fila-mayor (NsXf) */
    float l[9];          /* direcciones de las 3 luces en espacio LOCAL de la pieza (ya /4096) */
    float k[12];         /* por canal c (R,G,B): k[4c+0..2] = col[c][luz]/16, k[4c+3] = ambiente */
    ns_u32 vert_base;    /* primer vértice de su malla en el pool */
    int parent;          /* índice de pieza padre (global) para los vértices welded; -1 */
    ns_u32 flags;        /* NSG_PF_* */
    float fog;           /* multiplicador de niebla del objeto (1 = sin niebla) */
    float raise;         /* sesgo de orden de la malla (unidades del motor) */
    ns_u32 parent_nverts;/* nº de vértices de la malla del padre (acota los welded) */
    ns_u32 pad[2];
} NsGpuPiece;                                                                                       /* 164 B */

/* Entrada de la lista ordenada de semitransparentes: una por triángulo. */
typedef struct { ns_u32 piece; ns_u32 tri; } NsGpuOrder;                                            /* tri = índice ABSOLUTO de triángulo (esquina = 3*tri + k) */

/* Vértice de un efecto (billboards, cintas, cuadriláteros y triángulos de mundo), ya en el MUNDO VR (m): el empaquetador en C (ns_gpu_fx_build) orienta los
 * carteles y cintas a la cabeza una vez por fotograma. Color/texel como NsGpuCorner (la misma ns_fs). `eyes`: bit i = se dibuja para el ojo i (polígonos de una
 * cara: el test se hace por ojo en CPU). */
typedef struct {
    float p[3];
    float u, v;          /* texels 0..255 */
    float col[3];        /* 0..255, 128 = neutro */
    ns_u32 tpage;
    ns_u32 clutf;        /* clut | NSG_CF_* << 16 */
    ns_u32 eyes;
} NsGpuFxVert;                                                                                      /* 44 B */

#ifdef NS_GPU_HAVE_SIMD
typedef struct {
    matrix_float4x4 view_proj[2];   /* clip = P * V por ojo (índice = amplification_id; sin amplificación se usa [0]) */
    vector_float4 eye[2];           /* posición del ojo en el mundo VR (m) */
    vector_float4 tovr[3];          /* mundo del motor -> mundo VR, filas de la 3x4 (xyz, traslación en w) */
    vector_float4 fog_fwd;          /* xyz = adelante de la cámara del juego (mundo del motor); w = -dot(adelante, ojo): z_vista = dot(xyz, p) + w */
    vector_float4 fog_par;          /* x = inicio, y = longitud (unidades del motor), z = 1 si hay niebla, w = metros VR por unidad de `raise` */
    vector_float4 tint;             /* xyz = factor de color por canal aplicado al color de vértice (1,1,1 = sin teñir; fundido a negro del último fotograma válido) */
    /* agua (ns_screen): el tinte y la ondulación viven en el MUNDO. w de water_min = 1 si hay volumen de agua; w de water_tint = 1 si la cabeza está dentro (se tiñe todo) */
    vector_float4 water_min, water_max, water_tint;
    vector_float4 water_wave;       /* x = amplitud (texels), y = frecuencia espacial (rad por unidad del motor), z = fase (rad) */
    vector_float4 fromvr[3];        /* mundo VR -> mundo del motor, filas de la 3x4 (xyz, traslación en w) */
    vector_float4 misc;             /* x = índice de ojo del pase cuando NO hay amplificación (0/1: máscara de caras de los efectos) */
} NsGpuUniforms;
#endif

/* ---- Recubrimientos de pantalla del ojo (ns_screen): fundido, destello, modos de visión, anillo, líneas de exploración y barras. Una pasada de pantalla completa cada uno. ---- */
#define NSG_MAX_OVERLAYS 8
#define NSG_BUF_OVERLAY 0    /* ns_fs_overlay: NsGpuOverlayU en setFragmentBytes(index 0) */
enum { NSG_OV_NONE = 0, NSG_OV_COVER = 1, NSG_OV_RING = 2, NSG_OV_SCAN = 3, NSG_OV_BARS = 4 };
typedef struct {
    ns_u32 kind;         /* NSG_OV_* */
    ns_u32 abr;          /* modo de mezcla original 0 (B+F)/2, 1 B+F, 2 B-F, 3 B+F/4 */
    float amount;        /* COVER/SCAN/BARS: intensidad 0..1 */
    float rgb[3];        /* color 0..1 (RING: borde interior) */
    float rgb2[3];       /* RING: borde exterior */
    float p[4];          /* RING: radio interior px, radio exterior px, (sin uso: viñeta circular), H REAL del motor. SCAN: periodo en filas del motor, fase, proporción cubierta, H. BARS: filas del motor de la viñeta y suavizado, H */
} NsGpuOverlay;                                                                                     /* 52 B */
/* Parámetros del ojo para pasar de NDC a tangentes: t = (ndc + (P20, P21)) / (P00, P11). eye[i] = {P00, P11, P20, P21}. */
/* up0/up1: eje Y del mundo VR expresado en el espacio de vista del ojo (columna 1 de la matriz de vista; w sin uso): SCAN ancla sus líneas a la elevación del rayo en el mundo. */
typedef struct { NsGpuOverlay ov; float eye0[4], eye1[4]; float up0[4], up1[4]; } NsGpuOverlayU;     /* 116 B */
typedef struct {
    float water_min[4], water_max[4], water_tint[4], water_wave[4];   /* ver NsGpuUniforms */
    ns_u32 n_overlays, pad0, pad1, pad2;
    NsGpuOverlay ov[NSG_MAX_OVERLAYS];                                 /* en orden de dibujo */
} NsGpuScreen;

#ifndef __METAL_VERSION__
typedef char ns_gpu_chk_overlay[(sizeof(NsGpuOverlay) == 52) ? 1 : -1];
typedef char ns_gpu_chk_vert[(sizeof(NsGpuVert) == 16) ? 1 : -1];
typedef char ns_gpu_chk_corner[(sizeof(NsGpuCorner) == 28) ? 1 : -1];
typedef char ns_gpu_chk_piece[(sizeof(NsGpuPiece) == 164) ? 1 : -1];
typedef char ns_gpu_chk_order[(sizeof(NsGpuOrder) == 8) ? 1 : -1];
typedef char ns_gpu_chk_fxvert[(sizeof(NsGpuFxVert) == 44) ? 1 : -1];
#endif

#endif
