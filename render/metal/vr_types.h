/* Estructuras compartidas host (C / Swift vía puente) <-> shaders Metal del renderer de la escena VR
 * (vr_scene.metal, vr_vram.metal). Independiente de rg_types.h. Mismo layout en C y en MSL. */
#pragma once
#ifdef __METAL_VERSION__
#include <metal_stdlib>
#endif
#include <simd/simd.h>

/* Constantes de función (MTLFunctionConstantValues): índices */
#define VR_FC_FILTER 0      /* bool: suavizado bilineal de texels (respeta la transparencia 0x0000) */
#define VR_FC_LINEAR_OUT 1  /* bool: el destino es lineal/sRGB: convertir gamma->lineal al escribir */
#define VR_FC_PASS 2        /* uint: 0 todo; 1 solo texels semitransparentes (bit15=1) y sin textura; 2 solo texels opacos (bit15=0) */
#define VR_FC_BLEND 3       /* int: -1 = opaco / mezcla de función fija (no definida = -1); 0..3 = modo de mezcla del original, hecho en el shader en espacio gamma leyendo el destino ([[color(0)]]; vr_blend.h) */

/* VrGpuVtx.flags (el contrato de VrVtx no los definía: se asumen estos bits, == GPU_F_* de gpu.h) */
#define VR_VF_TEXTURED 2
#define VR_VF_RAW 4
#define VR_VF_SEMI 8
#define VR_VF_FLIPX 128   /* rects texturizados: u decrece (== GPU_F_FLIPX) */
#define VR_VF_FLIPY 256
#define VR_VF_RECT 0x4000 /* == VR_F_RECT (vr_scene.h): los FLIP solo valen con este bit */
#define VR_VF_BILLBOARD 0x0400 /* == VR_F_BILLBOARD (vr_scene.h): vr_vs_scene gira el vértice hacia el ojo con VrGpuState.bb */

/* Índices de búfer / textura */
#define VR_BUF_VERTS 0
#define VR_BUF_UNI 1
#define VR_BUF_STATES 2     /* VrGpuState[] (== VrScene.states), indexado por VrGpuVtx.pad; 0xFFFF = sin estado */

/* Espejo de VrVtx (vr/scene/vr_scene.h), 32 bytes.
 * clut: bits 0-5 x/16, 6-14 y (palabra de CLUT del original). tpage: bits 0-3 x/64, 4 y/256, 5-6 abr, 7-8 modo de color
 * (0=4b, 1=8b, 2/3=15b) (palabra de texpage del original). rgba = r | g<<8 | b<<16 | a<<24 (128 = modulación neutra). */
typedef struct {
    float pos[3];
    float uv[2];
    unsigned int rgba;
    unsigned short clut, tpage, flags, pad;
} VrGpuVtx;
typedef char vr_check_vtx_size[(sizeof(VrGpuVtx) == 32) ? 1 : -1];

/* Espejo de VrState (vr_scene.h), 60 bytes: tw = twmx, twmy, twox, twoy (ventana de textura E2, unidades de 8 px); bb = centro C, derecha R, abajo D, delante N (solo con VR_VF_BILLBOARD, si no ceros). */
typedef struct {
    unsigned char tw[4];
    short clip[4];
    float bb[12];
} VrGpuState;
typedef char vr_check_state_size[(sizeof(VrGpuState) == 60) ? 1 : -1];

typedef struct {
    matrix_float4x4 view_proj[2];  /* clip = P * V por ojo (índice = amplification_id; sin amplificación se usa [0]) */
    vector_float4 head;            /* posición de la cabeza en mundo (xyz) */
} VrSceneUniforms;

typedef struct {
    vector_float4 rect;            /* x, y, w, h del área visible en las coordenadas de los vértices 2D */
} VrHudUniforms;

/* Evento de VRAM (derivado de GpuOp). */
typedef struct {
    int x, y, w, h;                /* destino (ya normalizado) */
    int sx, sy;                    /* origen (copia) */
    unsigned int flags;            /* GPU_F_MASK_SET (32) / GPU_F_MASK_CHK (64) */
    unsigned int fill15;           /* fill: RGB555 */
    unsigned int data_off;         /* subida: índice (ushort) en el búfer de datos */
    unsigned int data_n;           /* subida: nº de píxeles recibidos */
} VrVramEvent;
#define VR_MASK_SET 32u
#define VR_MASK_CHK 64u

/* Pase de post-proceso del filtro de pantalla (efecto submarino: onda vertical por fila + tinte + recubrimientos; objetos equipados: líneas de exploración de las
 * gafas y máscara de visión de la máscara/caja). Un pase por ojo con la escena ya resuelta (una sola muestra) como entrada. Modelo (ver "Clase 3" y "Clase 3b" en
 * vr/scene/vr_scene.h): para la fila y del ojo (coordenada continua, centro de píxel = y+0,5) con tangente t = (y - eye.x)/eye.y:  r = cfg.y + cfg.x*t;  d = dv[r]
 * (interpolado entre centros de fila, 0 fuera de las filas con tira);  fila fuente y_src = y + eye.y*d/cfg.x  (== cy_img + f_eye*(r + d - cy_row)/proj_h);
 * salida = tinte * muestra(x, y_src); luego los recubrimientos EN ORDEN, las líneas de exploración y la resta de la máscara de visión. */
#define VR_POST_ROWS 512
#define VR_POST_COVERS 4
#define VR_BUF_POST_DV 1    /* constant float dv[VR_POST_ROWS]: desplazamiento fuente-destino por fila de la imagen original, 0 en las filas sin tira */
typedef struct {
    vector_float4 eye[2];   /* por ojo (amplification/array layer): x = fila del punto principal en píxeles, y = focal vertical en píxeles, z = columna del punto principal, w = focal horizontal */
    vector_float4 tint;     /* xyz = multiplicador por canal (espacio gamma original); w sin uso */
    vector_float4 covers[VR_POST_COVERS];  /* xyz = color/255 del recubrimiento semitransparente; w = modo de mezcla original (0..3) o -1 si no hay (los usados van primero) */
    vector_float4 cfg;      /* x = proj_h del motor, y = cy_row (fila del centro óptico en filas de la imagen original), z = filas con datos (0 = sin onda), w = cx_col (columna) */
    vector_float4 scan;     /* líneas de exploración: x = periodo (filas originales), y = fase, z = alto de cada línea (filas), w = modo de mezcla (0..3) o -1 si no hay */
    vector_float4 scan_rgb; /* xyz = color/255 de cada línea; w = ganancia (1 = fiel al original) */
    vector_float4 misc;     /* x = nº de recubrimientos (de los de covers[]) que se dibujan ANTES de las líneas de exploración (el resto, después); yzw = color que se suma donde la máscara de visión cierra del todo */
    vector_float4 frame;    /* máscara de visión: x,y = ancho y alto de la imagen mostrada del motor (px originales), z = 1 si hay máscara (texture(1)), w sin uso */
    vector_float4 wave;     /* solo vr_fs_wave (render nativo bajo el agua; usa eye[] y cfg.x): x = amplitud (px del ojo a NS_WAVE_REF_W = 1888 de ancho), y = rad por fila del motor, z = fase (rad), w = H del motor (px de la pantalla de 320x240) */
    vector_float4 vfx;      /* solo vr_fs_wave: efectos del juego por procedencia (vr/scene/vr_prov.h): x = intensidad de la viñeta (0 = sin viñeta), y = radio de la zona limpia (alto del ojo / 2 = 1), z = fracción del alto cubierta por las bandas de cinemática (0 = sin bandas), w sin uso */
} VrPostUniforms;
