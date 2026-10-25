/* vr_glove: brazo, antebrazo y mano ORIGINALES del personaje (geometría y texturas del juego) como mallas propias del renderer VR (C99, puro: lee una RAM y una VRAM que le
 * pasan, no depende del motor ni del emulador).
 *
 * POR QUÉ. El renderer dibuja el 3D del juego a la cadencia del motor (25 Hz). Para que las manos sigan al mando a 90 Hz se ocultan las piezas del juego (vrbody own_hands) y se
 * dibujan estas mallas, con la IK repetida con el agarre vivo (vr/body: vrbody_arm_resolve).
 *
 * FORMATO DEL MODELO (verificado con volcados reales; igual que vr/native/ns_extract.c decode_model): model+0 banderas (0x10000 = vértices soldados: índices de 7 bits, bit 7 =
 * vértice soldado), +4 nº de caras, +0x2C padre, +0x34 nº de vértices, +0x38 vértices (s16 x, y, z + s16 de soldadura), +0x3C índices de cara A, B, C, D (perímetro; C == D = triángulo),
 * +0x40 nº de normales, +0x44 normales (s16 x3, 4096 = 1), +0x48 índices de normal por cara, +0x4C UV (8 B por cara, 2 por esquina), +0x50 texid u16 por cara. Tabla de texturas
 * 0x800B30E0 (12 B: id u16, +4 tpage, +6 clut, +8 ox, oy, w, h): UV real u' = ((u * (w + 1)) >> 8) + ox. Unidades del modelo: mm del motor (1 m = upm unidades, ~1010,7).
 *
 * MALLA. Una malla por pieza (brazo, antebrazo, mano) y lado. Los vértices de salida son por ESQUINA de cara (UV y normal son por cara en el juego): quad -> triángulos (A,B,D),(B,C,D);
 * triángulo (C == D) -> (A,B,C). Cada pieza produce como mucho VRGLOVE_MAXOUT vértices de salida (36 B * 113 <= 4096: el renderer los enlaza con setVertexBytes). Los vértices
 * cambian en cada fotograma (pose viva, soldaduras, puño): el renderer los trata como DINÁMICOS (VRH_MF_DYNAMIC) y solo cachea los índices por mesh_id.
 * SOLDADURAS. Un vértice marcado (bit 7 del índice) de la pieza toma, en el juego, la posición YA transformada del vértice del padre que indica su campo de soldadura; aquí se
 * cierra dentro del conjunto (antebrazo contra brazo, mano contra antebrazo). El brazo suelda contra el torso, que no es del conjunto: sus vértices quedan donde están.
 * MANO DERECHA. La del juego no es el espejo de la izquierda (pulgar abierto ~39 grados frente a ~22, dedos 20 mm mas cortos y curvados): vrglove_build la construye como ESPEJO X de la izquierda
 * (x cambiada de signo, esquinas A,D,C,B para conservar el winding, mismas UV/texturas) con las soldaduras de su propio antebrazo; si no encaja (otra topologia o >30 unidades de diferencia) queda la original.
 * COLOR. rgba = 255 (brillo ya establecido de las manos); el modelo no trae color por cara.
 * PUÑO. La mano se deforma con vrfist_deform (vr/body) a partir de su propia malla; sin variante de puño en el juego.
 * TEXTURA. Atlas RGBA8 construido con tpage + CLUT de la VRAM del juego (texel 0x0000 = transparente, alfa 0). Cada región (texid) va en su bloque con borde replicado de 1 texel;
 * uv = (ax + (u' - ox) + 0,5) / aw, (ay + (v' - oy) + 0,5) / ah. El atlas se reconstruye si cambia el hash de lo que se leyó (modelos, filas CLUT y texels usados). */
#pragma once
#include <stdint.h>
#include "vr_hands_frame.h"
#include "vr_fist.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VRGLOVE_MAXV 128         /* vértices/normales/caras que se aceptan por pieza (el tope de vr_fist) */
#define VRGLOVE_MAXF 128
#define VRGLOVE_MAXOUT 113       /* vértices de salida por pieza */
#define VRGLOVE_MAXREG 32        /* regiones de textura distintas */
#define VRGLOVE_MAXATLAS_H 4096

typedef struct VrGlovePiece {
    int nv, nn, nf;
    int16_t v[VRGLOVE_MAXV * 4], n[VRGLOVE_MAXV * 4];        /* x, y, z, relleno */
    uint8_t vidx[VRGLOVE_MAXF * 4], nidx[VRGLOVE_MAXF * 4];  /* tal cual (bit 7 = marca) */
    uint8_t weld[VRGLOVE_MAXV];                              /* vértice del padre (dentro del conjunto) o 0xFF */
    uint32_t nout;                                           /* vértices de salida */
    uint8_t out_v[VRGLOVE_MAXOUT], out_n[VRGLOVE_MAXOUT];    /* vértice y normal de origen de cada vértice de salida */
    uint8_t out_u[VRGLOVE_MAXOUT], out_t[VRGLOVE_MAXOUT], out_reg[VRGLOVE_MAXOUT];   /* UV real u', v' (texels de la región) y región del atlas (0 = blanco) */
    float out_uv[VRGLOVE_MAXOUT][2];
    uint32_t nidx_out;
    uint16_t idx[2 * VRGLOVE_MAXOUT];                        /* triángulos */
} VrGlovePiece;

typedef struct VrGlove {
    uint64_t hash;           /* de todo lo leído (la última construcción, válida o no) */
    uint32_t id;             /* identidad de la geometría+atlas construidos (contador global, nunca 0 si valid) */
    int valid;
    VrGlovePiece p[2][3];    /* [lado 0 izquierdo / 1 derecho][brazo, antebrazo, mano] */
    uint8_t *atlas;          /* aw * ah * 4, RGBA8 sin premultiplicar */
    uint32_t aw, ah;
} VrGlove;

VrGlove *vrglove_create(void);
void vrglove_destroy(VrGlove *g);

/* Lee las 6 piezas (models[lado][brazo, antebrazo, mano] = model* del juego; ver vrbody_player_models) de `ram` (2 MB, direcciones KSEG0) y la VRAM (1024 x 512 halfwords) y
 * construye mallas y atlas en g. Si el hash de lo leído es skip_hash no construye el atlas ni marca g como válido (g queda sin definir): el llamador conserva la construcción
 * anterior. Devuelve 1 = construido (g->valid), 0 = hash igual a skip_hash, -1 argumentos, -2 modelo ilegible, -3 pieza con demasiados vértices/caras, -4 atlas imposible. */
int vrglove_build(VrGlove *g, const uint8_t *ram, const uint16_t *vram, const uint32_t models[2][3], uint64_t skip_hash);

/* Pose de las tres piezas de un lado, en el espacio del usuario. org = origen de cada pieza (m), R = su orientación (fila-mayor, columnas = ejes locales), upm = unidades del
 * modelo por metro, grip 0..1 con fist != NULL y grip >= fist_min -> mano en puño. */
typedef struct VrGlovePose {
    float org[3][3];
    float R[3][9];
    float upm;
    float grip;
    const VrFistParams *fist;
    float fist_min;
} VrGlovePose;

/* Vértices de salida en el espacio del usuario (m) + caché del puño deformado (una instancia por lado, no se comparte entre lados). */
typedef struct VrGloveOut {
    VrHandsVertex v[3][VRGLOVE_MAXOUT];
    uint32_t nv[3];
    int fist_valid; uint32_t fist_id; float fist_grip;
    int16_t fv[VRGLOVE_MAXV * 4], fn[VRGLOVE_MAXV * 4];
} VrGloveOut;

/* 0 = bien; -1 = argumentos/pose no finita. Sin estado salvo la caché del puño de `o`. */
int vrglove_pose(const VrGlove *g, int side, const VrGlovePose *p, VrGloveOut *o);

#ifdef __cplusplus
}
#endif
