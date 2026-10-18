/* Estructuras compartidas entre el host (C/C++/Swift vía puente) y los shaders Metal (ps1_gpu.metal).
 * Diseño en render/README.md. Todo con campos de 4 bytes: mismo layout en C y en MSL (sin float2/float3 empaquetados). */
#pragma once
#ifdef __METAL_VERSION__
#include <metal_stdlib>
typedef unsigned int ps1_u32;
typedef int ps1_i32;
#else
#include <stdint.h>
typedef uint32_t ps1_u32;
typedef int32_t ps1_i32;
#endif

/* Constantes de función (MTLFunctionConstantValues): índices */
#define PS1_FC_EXACT5 0     /* bool: canal de 5 bits + tramado + mezcla en 5 bits (idéntico al oráculo) */
#define PS1_FC_PERSAMPLE 1  /* bool: sombrear por muestra (MSAA); requiere sample_id */
#define PS1_FC_MS 2         /* bool: el objetivo de alta resolución es multimuestra */

/* Banderas = GPU_F_* de runtime/gpu.h (mismos valores) */
#define PS1_F_GOURAUD 1
#define PS1_F_TEXTURED 2
#define PS1_F_RAW 4
#define PS1_F_SEMI 8
#define PS1_F_DITHER 16
#define PS1_F_MASK_SET 32
#define PS1_F_MASK_CHK 64
/* Solo host (no existe en GpuOp.flags): todos los vértices de la primitiva traen geometría de alta precisión
 * (GpuVtx.fvalid), con |fx-x| y |fy-y| <= 3 px (REF_F_TOL de ref_raster.c: descarta coincidencias espurias y vértices saturados),
 * finitos y fz > 0 -> clase de profundidad 3D (PS1Vertex.iz > 0). Las demás son 2D (iz = 0). */
#define PS1_F_HP3D 1024

/* Un vértice ya expandido a lista de triángulos (cuadrángulo = (0,1,2),(1,2,3); sprite = 2 triángulos con
 * u,v de partida +0 / +w (ver README: u0+1 y u0+1-w si hay volteo); línea = cinta de 1 px). 36 bytes.
 * Alta precisión: el flujo de la pasada de ALTA RESOLUCIÓN lleva x,y = fx,fy (float, offset sumado) e iz = 1/fz si la primitiva
 * es 3D (todos sus vértices fvalid); el flujo de la pasada NATIVA 1x exacta lleva siempre x,y enteros e iz = 0. */
typedef struct {
    float x, y;       /* coordenadas de VRAM nativas (offset de dibujo sumado; fx/fy si fvalid y pasada hires) */
    float u, v;       /* 0..255 sin normalizar */
    float r, g, b;    /* 0..255 */
    float iz;         /* 1/z de vista (GpuVtx.fz) si es 3D en la pasada hires; 0 = 2D / afín (comportamiento de siempre) */
    ps1_u32 prim;     /* índice en la tabla de estados */
} PS1Vertex;
#ifndef __METAL_VERSION__
_Static_assert(sizeof(PS1Vertex) == 36, "PS1Vertex debe medir 36 bytes (9 palabras de 4) como en MSL");
#endif

/* Estado de una primitiva (tabla indexada por PS1Vertex.prim). 32 bytes. */
typedef struct {
    ps1_u32 flags;    /* PS1_F_* */
    ps1_u32 modes;    /* abr | tp << 2 */
    ps1_u32 tpage;    /* tx | ty << 16 (halfwords, filas) */
    ps1_u32 clut;     /* clx | cly << 16 */
    ps1_u32 window;   /* twmx | twmy << 8 | twox << 16 | twoy << 24 */
    ps1_u32 clip1;    /* cx1 | cy1 << 16 */
    ps1_u32 clip2;    /* cx2 | cy2 << 16 (inclusivo) */
    ps1_u32 pad;
} PS1Prim;

/* Parámetros por dibujo/evento.
 * Profundidad (solo hires con hiprec=1): adjunto Depth32Float, borrado a 0 al inicio de cada lista, profundidad = 1/z (mayor =
 * más cerca; el vértice pone clip.z = 1, clip.w = z, así ndc.z = 1/z y la interpolación en pantalla es lineal, que es la correcta
 * para 1/z). Tres MTLDepthStencilState según la clase de la primitiva (mismo reparto que REF_DEPTH de ref_raster.c):
 *   3D opaca     (PS1_F_HP3D, sin PS1_F_SEMI): compare greaterEqual, escribe;
 *   3D semitransp. (PS1_F_HP3D y PS1_F_SEMI):  compare greaterEqual, NO escribe;
 *   2D           (resto): compare always, escribe (clip.z = 0 => borra la profundidad: lo 2D opaco pasa a ser lo más cercano,
 *                como el orden de pintor del juego). Una 2D semitransparente: always, sin escribir.
 * Calcomanías coplanares: setDepthBias(65536, 0, 0) en las dos clases 3D (en Depth32Float el sesgo es 2^(exp-23) por unidad, es
 * decir ~1/128 relativo, el REF_DEPTH.depth_bias por defecto de ref_raster.c). */
typedef struct {
    ps1_u32 scale;    /* M = muestras lineales por píxel nativo en el objetivo de alta resolución */
    ps1_u32 samples;  /* muestras por píxel del objetivo (1 o 4) */
    ps1_u32 corner;   /* 1: muestreo en la ESQUINA como el oráculo (pasada nativa 1x): el vértice suma +0,5 px; 0: centro */
    ps1_u32 hiprec;   /* 1: usar PS1Vertex.iz (perspectiva + profundidad); 0 (siempre en la pasada nativa): afín, w = 1 */
} PS1Frame;

/* Evento de VRAM: relleno / subida / copia (rectángulo destino en px nativos). */
typedef struct {
    ps1_i32 x, y, w, h;      /* destino */
    ps1_i32 sx, sy;          /* origen (copia) */
    ps1_u32 flags;           /* PS1_F_MASK_SET / PS1_F_MASK_CHK */
    ps1_u32 color;           /* fill: RGB de 24 bits (r | g<<8 | b<<16) */
    ps1_u32 fill15;          /* fill: RGB555 */
    ps1_u32 data_off;        /* subida: índice (uint16) en el búfer de datos */
    ps1_u32 data_n;          /* subida: nº de píxeles recibidos (GpuOp.data_n; < w*h si la subida se abortó) */
    ps1_u32 pad1;
} PS1Event;

/* Presentación del área de display. */
typedef struct {
    ps1_i32 x, y, w, h;      /* display en px nativos */
    ps1_u32 scale, ssaa;     /* N de salida; K lineal de supermuestreo (hires = N*K), 1 si el objetivo es MSAA */
    ps1_u32 samples;         /* muestras del objetivo (MSAA) */
    ps1_u32 depth24;
} PS1Present;
