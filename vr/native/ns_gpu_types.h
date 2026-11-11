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
#define NSG_CF_ONESIDED 32u  /* cara de una sola cara (modelo sin NS_MF_TWOSIDED): con NsGpuUniforms.cull_eye.w = 1 el sombreador la descarta vista por detrás (regla del anverso del motor) */

/* Marcas de pieza */
#define NSG_PF_SHADE  1u     /* se sombrea con la luz de la pieza (si la esquina no lleva color precalculado) */
#define NSG_PF_VFOG   2u     /* niebla por vértice (escenario); si no, `fog` por pieza (objeto) */
#define NSG_PF_DRAW   4u     /* se dibuja (malla presente y pieza visible) */
#define NSG_PF_NET    8u     /* red de seguridad de vértices activa (piezas de objetos dinámicos, no del escenario): el shader colapsa los triángulos con vértices lejos de la pieza o aristas desproporcionadas */
#define NSG_PF_NOWELD 16u    /* ignorar las soldaduras de esta pieza (la costura con el padre quedó inconsistente este fotograma): sus vértices usan su propia posición local */
#define NSG_PF_IRCLUT 32u    /* visión térmica: las caras texturizadas de la pieza usan la CLUT fija NSG_IR_CLUT en vez de la suya (objetos con textura térmica, ver ns_gpu_make_piece) */
#define NSG_IR_CLUT 0x3FFFu  /* CLUT térmica del motor: x = 1008, y = 255 de la VRAM */

#define NSG_NO_WELD 0xFFu
#define NSG_WELD_DEPTH 4     /* soldaduras encadenadas que se siguen (hijo -> padre -> abuelo ...): el adaptador de la referencia llega a 8, en los datos reales no pasa de 1 */

/* Red de seguridad contra «explosiones de vértices» (triángulos estirados hasta lejos). Solo piezas NSG_PF_NET. Todas las distancias en metros de mundo VR (m por unidad = norma
 * de la 1.ª columna de to_vr): un triángulo se colapsa si (a) algún vértice queda a más de NSG_NET_FAR_RATIO * (radio de reposo de la malla) + NSG_NET_SLACK_M del origen de su pieza,
 * o (b) alguna arista mide más de NSG_NET_EDGE_RATIO * (la misma arista en espacio local) + NSG_NET_SLACK_M. En datos reales sin fallo: 0 triángulos (ver test_ns_net.c). */
#define NSG_NET_FAR_RATIO  1.5f
#define NSG_NET_EDGE_RATIO 2.0f
#define NSG_NET_SLACK_M    1.0f
#define NSG_NET_WELD_GAP_M 1.0f   /* una soldadura cuyo vértice del padre cae a más de esto de la posición propia del vértice => NSG_PF_NOWELD */
#define NSG_NET_MAT_SCALE  2.75f  /* una columna de la matriz de pieza con norma mayor => matriz corrupta (a medio actualizar): la pieza no se dibuja. Por encima del estirado máximo de los brazos de la IK
                                   * (2,5 en vr_body.c, por la escala ~1,003 de las matrices del motor): con 1,5 un brazo estirado más de la mitad dejaba de dibujarse */
#define NSG_INTENSITY_MAX 7.99975586f   /* = NS_INTENSITY_MAX (0x7FFF/4096) de ns_types.h */

/* Reversos de las caras de una sola cara cuando NO se descartan (mundo en primera persona: doble cara). Muchas paredes son dos hojas de una cara en el MISMO plano, espalda con espalda (una por
 * sala o por grupo de mapa); el motor solo dibuja la que mira a la cámara. Con doble cara y búfer de profundidad las dos empatan y la pared parpadea entre sus dos texturas (o se queda negra).
 * El reverso se dibuja igual (tapa donde no hay anverso) pero con la profundidad de recorte multiplicada por NSG_BACK_Z (Z invertida: un 0,05 % más lejos), así que pierde siempre el empate.
 * Réplica en C: ns_gpu_back_z (ns_gpu.c). */
#define NSG_BACK_Z (1.0f - 1.0f / 2048.0f)
/* Tamaño máximo de un triángulo en la pantalla del motor: la GPU original no dibuja el que mide más de 1023 px de ancho o 511 de alto (coordenadas de la GTE saturadas a -1024..1023). Con la
 * cámara del juego (pantalla 3D) una rejilla o un suelo pegados a la cámara no existen en su imagen. Réplica en C: ns_gpu_tri_oversize (ns_gpu.c). */
#define NSG_HW_MAX_W 1023.0f
#define NSG_HW_MAX_H 511.0f
#define NSG_HW_SXY_MIN (-1024.0f)
#define NSG_HW_SXY_MAX 1023.0f

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
    float rest_r;        /* radio de reposo de la malla (máx. |v| local, unidades del motor): referencia de la red de seguridad (0 = sin dato: la red no actúa) */
    ns_u32 pad;
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

/* Valores de NsGpuUniforms.cull_eye.w (ver ns_gpu_cull_applies). */
#define NSG_CULL_NONE 0
#define NSG_CULL_ALL  1
#define NSG_CULL_SEMI 2

#ifdef NS_GPU_HAVE_SIMD
typedef struct {
    matrix_float4x4 view_proj[2];   /* clip = P * V por ojo (índice = amplification_id; sin amplificación se usa [0]) */
    vector_float4 eye[2];           /* posición del ojo en el mundo VR (m) */
    vector_float4 tovr[3];          /* mundo del motor -> mundo VR, filas de la 3x4 (xyz, traslación en w) */
    vector_float4 fog_fwd;          /* xyz = adelante de la cámara del juego (mundo del motor); w = -dot(adelante, ojo): z_vista = dot(xyz, p) + w */
    vector_float4 fog_par;          /* x = inicio, y = longitud (unidades del motor), z = 1 si hay niebla, w = metros VR por unidad de `raise` */
    vector_float4 tint;             /* xyz = factor de color por canal aplicado al color de vértice (1,1,1 = sin teñir; fundido a negro del último fotograma válido) */
    /* agua (ns_screen): el tinte vive en el MUNDO (la ondulación es un post-proceso de pantalla, ver NsGpuScreen.water_wave). w de water_min = 1 si hay volumen de agua; w de water_tint = 1 si la cabeza está dentro (se tiñe todo) */
    vector_float4 water_min, water_max, water_tint;
    vector_float4 fromvr[3];        /* mundo VR -> mundo del motor, filas de la 3x4 (xyz, traslación en w) */
    vector_float4 misc;             /* x = índice de ojo del pase cuando NO hay amplificación (0/1: máscara de caras de los efectos); y = descarte cercano del motor: z de vista de la cámara del juego (unidades del motor) por debajo de la cual un triángulo con algún vértice ahí no se dibuja (0 = sin descarte) */
    vector_float4 cull_eye;         /* xyz = ojo de la cámara del JUEGO en el mundo del motor; w = NSG_CULL_ALL: descartar las caras NSG_CF_ONESIDED vistas por detrás (cinemáticas: la cámara queda fuera de las salas y el motor no dibuja los techos desde arriba); NSG_CULL_SEMI: solo las de mallas semitransparentes (cabeza bajo el agua: el motor no dibuja la superficie del agua vista desde abajo); 0 = doble cara (primera persona) */
    vector_float4 cam_x, cam_y;     /* cámara del JUEGO en el mundo del motor: xyz = eje X (derecha) / Y (abajo) de la vista; w = píxeles de la pantalla del motor por unidad de tangente (H; H por el factor vertical). Solo se usan con misc.y > 0 (tamaño máximo del hardware, NSG_HW_MAX_*) */
} NsGpuUniforms;
#endif

/* ---- Recubrimientos de pantalla del ojo (ns_screen): fundido, destello, modos de visión, anillo, líneas de exploración y barras. Una pasada de pantalla completa cada uno. ---- */
#define NSG_MAX_OVERLAYS 8
#define NSG_BUF_OVERLAY 0    /* ns_fs_overlay: NsGpuOverlayU en setFragmentBytes(index 0) */
enum { NSG_OV_NONE = 0, NSG_OV_COVER = 1, NSG_OV_RING = 2, NSG_OV_SCAN = 3, NSG_OV_BARS = 4, NSG_OV_FRAME = 5 };
/* NSG_OV_FRAME: máscara de visión de la caja (resta por celda de una textura R8 de 160x128 anclada a la cabeza). p[0] = columna del centro óptico en la imagen del motor, p[1] = 0 resta / 1 relleno del interior cerrado (rgb),
 * p[3] = H del motor; rgb2 = {ancho, alto, fila del centro} de la imagen mostrada en píxeles del motor. */
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
    float water_min[4], water_max[4], water_tint[4];                 /* ver NsGpuUniforms */
    float water_wave[4];                                              /* onda de pantalla (post-proceso del ojo, solo con la cabeza dentro; todo 0 = sin onda): x = amplitud (px del ojo a 1888 de ancho), y = rad por fila del motor, z = fase (rad) */
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
