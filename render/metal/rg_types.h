/* Estructuras compartidas entre el host (C/C++/Swift vía puente) y los shaders Metal (rg_gpu.metal).
 * Diseño en render/README.md. Todo con campos de 4 bytes: mismo layout en C y en MSL (sin float2/float3 empaquetados). */
#pragma once
#ifdef __METAL_VERSION__
#include <metal_stdlib>
typedef unsigned int rg_u32;
typedef int rg_i32;
#else
#include <stdint.h>
typedef uint32_t rg_u32;
typedef int32_t rg_i32;
#endif

/* Constantes de función (MTLFunctionConstantValues): índices */
#define RG_FC_EXACT5 0     /* bool: canal de 5 bits + tramado + mezcla en 5 bits (idéntico al oráculo) */
#define RG_FC_PERSAMPLE 1  /* bool: sombrear por muestra (MSAA); requiere sample_id */
#define RG_FC_MS 2         /* bool: el objetivo de alta resolución es multimuestra */

/* Banderas = GPU_F_* de runtime/gpu.h (mismos valores) */
#define RG_F_GOURAUD 1
#define RG_F_TEXTURED 2
#define RG_F_RAW 4
#define RG_F_SEMI 8
#define RG_F_DITHER 16
#define RG_F_MASK_SET 32
#define RG_F_MASK_CHK 64
/* Solo host (no existe en GpuOp.flags): todos los vértices de la primitiva traen geometría de alta precisión
 * (GpuVtx.fvalid), con |fx-x| y |fy-y| <= 3 px (REF_F_TOL de ref_raster.c: descarta coincidencias espurias y vértices saturados),
 * finitos y fz > 0 -> clase de profundidad 3D (RGVertex.iz > 0). Las demás son 2D (iz = 0). */
#define RG_F_HP3D 1024

/* Un vértice ya expandido a lista de triángulos (cuadrángulo = (0,1,2),(1,2,3); sprite = 2 triángulos con
 * u,v de partida +0 / +w (ver README: u0+1 y u0+1-w si hay volteo); línea = cinta de 1 px). 36 bytes.
 * Alta precisión: el flujo de la pasada de ALTA RESOLUCIÓN lleva x,y = fx,fy (float, offset sumado) e iz = 1/fz si la primitiva
 * es 3D (todos sus vértices fvalid); el flujo de la pasada NATIVA 1x exacta lleva siempre x,y enteros e iz = 0. */
typedef struct {
    float x, y;       /* coordenadas de VRAM nativas (offset de dibujo sumado; fx/fy si fvalid y pasada hires) */
    float u, v;       /* 0..255 sin normalizar */
    float r, g, b;    /* 0..255 */
    float iz;         /* 1/z de vista (GpuVtx.fz) si es 3D en la pasada hires; 0 = 2D / afín (comportamiento de siempre) */
    rg_u32 prim;     /* índice en la tabla de estados */
} RGVertex;
#ifndef __METAL_VERSION__
_Static_assert(sizeof(RGVertex) == 36, "RGVertex debe medir 36 bytes (9 palabras de 4) como en MSL");
#endif

/* Estado de una primitiva (tabla indexada por RGVertex.prim). 32 bytes. */
typedef struct {
    rg_u32 flags;    /* RG_F_* */
    rg_u32 modes;    /* abr | tp << 2 */
    rg_u32 tpage;    /* tx | ty << 16 (halfwords, filas) */
    rg_u32 clut;     /* clx | cly << 16 */
    rg_u32 window;   /* twmx | twmy << 8 | twox << 16 | twoy << 24 */
    rg_u32 clip1;    /* cx1 | cy1 << 16 */
    rg_u32 clip2;    /* cx2 | cy2 << 16 (inclusivo) */
    rg_u32 pad;
} RGPrim;

/* Parámetros por dibujo/evento.
 * Profundidad (solo hires con hiprec=1): adjunto Depth32Float, borrado a 0 al inicio de cada lista, profundidad = 1/z (mayor =
 * más cerca; el vértice pone clip.z = 1, clip.w = z, así ndc.z = 1/z y la interpolación en pantalla es lineal, que es la correcta
 * para 1/z). Tres MTLDepthStencilState según la clase de la primitiva (mismo reparto que REF_DEPTH de ref_raster.c):
 *   3D opaca     (RG_F_HP3D, sin RG_F_SEMI): compare greaterEqual, escribe;
 *   3D semitransp. (RG_F_HP3D y RG_F_SEMI):  compare greaterEqual, NO escribe;
 *   2D           (resto): compare always, escribe (clip.z = 0 => borra la profundidad: lo 2D opaco pasa a ser lo más cercano,
 *                como el orden de pintor del juego). Una 2D semitransparente: always, sin escribir.
 * Calcomanías coplanares: setDepthBias(65536, 0, 0) en las dos clases 3D (en Depth32Float el sesgo es 2^(exp-23) por unidad, es
 * decir ~1/128 relativo, el REF_DEPTH.depth_bias por defecto de ref_raster.c). */
typedef struct {
    rg_u32 scale;    /* M = muestras lineales por píxel nativo en el objetivo de alta resolución */
    rg_u32 samples;  /* muestras por píxel del objetivo (1 o 4) */
    rg_u32 corner;   /* 1: muestreo en la ESQUINA como el oráculo (pasada nativa 1x): el vértice suma +0,5 px; 0: centro */
    rg_u32 hiprec;   /* 1: usar RGVertex.iz (perspectiva + profundidad); 0 (siempre en la pasada nativa): afín, w = 1 */
} RGFrame;

/* Evento de VRAM: relleno / subida / copia (rectángulo destino en px nativos). */
typedef struct {
    rg_i32 x, y, w, h;      /* destino */
    rg_i32 sx, sy;          /* origen (copia) */
    rg_u32 flags;           /* RG_F_MASK_SET / RG_F_MASK_CHK */
    rg_u32 color;           /* fill: RGB de 24 bits (r | g<<8 | b<<16) */
    rg_u32 fill15;          /* fill: RGB555 */
    rg_u32 data_off;        /* subida: índice (uint16) en el búfer de datos */
    rg_u32 data_n;          /* subida: nº de píxeles recibidos (GpuOp.data_n; < w*h si la subida se abortó) */
    rg_u32 pad1;
} RGEvent;

/* Presentación del área de display. */
typedef struct {
    rg_i32 x, y, w, h;      /* display en px nativos */
    rg_u32 scale, ssaa;     /* N de salida; K lineal de supermuestreo (hires = N*K), 1 si el objetivo es MSAA */
    rg_u32 samples;         /* muestras del objetivo (MSAA) */
    rg_u32 depth24;
} RGPresent;
