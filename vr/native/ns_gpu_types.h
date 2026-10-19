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

/* Esquina de triángulo (3 por triángulo; un cuadrilátero = 2 triángulos (A,B,C) y (A,C,D)). */
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

#ifdef NS_GPU_HAVE_SIMD
typedef struct {
    matrix_float4x4 view_proj[2];   /* clip = P * V por ojo (índice = amplification_id; sin amplificación se usa [0]) */
    vector_float4 eye[2];           /* posición del ojo en el mundo VR (m) */
    vector_float4 tovr[3];          /* mundo del motor -> mundo VR, filas de la 3x4 (xyz, traslación en w) */
    vector_float4 fog_fwd;          /* xyz = adelante de la cámara del juego (mundo del motor); w = -dot(adelante, ojo): z_vista = dot(xyz, p) + w */
    vector_float4 fog_par;          /* x = inicio, y = longitud (unidades del motor), z = 1 si hay niebla, w = metros VR por unidad de `raise` */
    vector_float4 tint;             /* xyz = factor de color por canal aplicado al color de vértice (1,1,1 = sin teñir; modo de depuración «solo nativo») */
} NsGpuUniforms;
#endif

#ifndef __METAL_VERSION__
typedef char ns_gpu_chk_vert[(sizeof(NsGpuVert) == 16) ? 1 : -1];
typedef char ns_gpu_chk_corner[(sizeof(NsGpuCorner) == 28) ? 1 : -1];
typedef char ns_gpu_chk_piece[(sizeof(NsGpuPiece) == 164) ? 1 : -1];
typedef char ns_gpu_chk_order[(sizeof(NsGpuOrder) == 8) ? 1 : -1];
#endif

#endif
