#pragma once
/* ns_div: réplica EXACTA, en C, de las etapas de polígonos del motor (libdg de la decompilación pública FoxdieTeam/mgs_reversing: bound.c, trans.c, divide.c, sort.c) para la pantalla 3D
 * con la cámara del propio juego. Lee la RAM del motor (cola del canal principal, objetos, modelos, matrices de PANTALLA +0x20, memoria de paquetes) y devuelve, cara por cara, lo que el
 * motor entrega a la GPU: la cara entera o sus trozos subdivididos, con las mismas coordenadas de pantalla (GTE entera: UNR, saturaciones) y los mismos descartes. Reglas replicadas:
 *   bound.c   BoundObjs/BoundCheck: el objeto o modelo cuya caja (8 esquinas proyectadas) cae entera fuera de ±160 x ±112, o con las 8 profundidades a 0, no se dibuja; GBOUND decide con la caja de la definición.
 *   trans.c   DG_WriteObjVertices: descarte por código de salida común de las 4 esquinas (solo objetos «parciales» no GBOUND), cara de una sola cara con NCLIP(A,B,D) <= 0 y toda cara con NCLIP == 0
 *             (no se dibujan), profundidad z = (zA + zC) / 2 == 0 (no se dibuja), caras con esquinas soldadas (bit 7: «indirectas») sin descarte ni subdivisión y con z = media de las esquinas propias.
 *   divide.c  DG_InitRVector / DG_SubDivideRVectors: una cara se parte en 4 (hasta 3 niveles: 64 trozos) si (tag >> 8) > 0x800, z < 8192 (60000 con H > 1000) y quedan >= 4 paquetes; el trozo cuyas 4 esquinas
 *             comparten código de salida (o profundidad 0) se descarta; el trozo se vuelve a partir si |NCLIP| > 0x800 (nivel <= 2); sin paquetes libres el trozo se pierde; los puntos medios del borde se mueven ±1 px.
 * No es el pipeline de la app: es la verdad contra la que se mide (ns_div.c) y la fuente de las caras que la app no puede reproducir con su sombreador por triángulo. C99, sin dependencias. */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { NSDIV_FULL = 0, NSDIV_SUB = 1 };

/* Lo MEDIDO en los volcados del juego (32 volcados de fotograma, ver informe), que difiere de la decompilación: divide_mem->field_14 vale 0x7FFF0000 en la memoria rápida (n_packs == paquetes libres
 * en 32/32: no se subdivide nunca) y el descarte por código de salida de trans.c:139-152 no actúa (6603 caras con las 4 esquinas al mismo lado están encadenadas en la OT, 5 contraejemplos de 20 889). */
#define NSDIV_GAME_THRESH 0x7FFF0000u

typedef struct {
    uint32_t obj;          /* dirección (KSEG0) del DG_OBJS */
    uint16_t model;        /* índice del modelo base dentro de objs->objs[] */
    uint16_t node;         /* índice del modelo (base o extensión) dentro de objs->objs[] */
    uint16_t face;         /* cara dentro de ese modelo */
    uint8_t kind;          /* NSDIV_FULL (la cara) / NSDIV_SUB (trozo) */
    uint8_t level;         /* 0 la cara; 1..3 nivel de subdivisión del trozo */
    uint32_t pack[2];      /* dirección del paquete de la cara en cada búfer (objs->packs[b] + 0x34 * n); 0 = sin paquete. Los trozos comparten el de su cara */
    uint8_t nv;            /* 4 */
    uint8_t pad;
    int16_t sx[4], sy[4];  /* pantalla de la GTE (centrada en 0), orden de paquete: A, B, D, C */
    uint16_t sz[4];
    int16_t p[4][3];       /* posición LOCAL (espacio del modelo) de cada esquina; las soldadas llevan la de su vértice propio (sin sentido) */
    uint8_t weld;          /* bit k: la esquina k (orden de paquete) es soldada al padre */
    uint8_t u[4], v[4], rgb[4][3];   /* atributos (los de la cara; los trozos los interpolan como el motor: (a + b) / 2) */
    int16_t raise;         /* obj->raise */
    uint8_t trans;         /* el modelo es translúcido */
    uint8_t back;          /* la cara se ve por detrás (NCLIP < 0) y es de doble cara */
} NsDivPrim;

/* Atributos de la cara `face` del modelo `node` del objeto `obj`, esquina k en orden de paquete (A, B, D, C). NULL = ceros. */
typedef void (*NsDivAttrFn)(void *user, uint32_t obj, int node, int face, int k, uint8_t *u, uint8_t *v, uint8_t rgb[3]);
/* Cada primitiva final, en el orden de proceso del motor. */
typedef void (*NsDivEmitFn)(void *user, const NsDivPrim *p);

/* Cara que el motor NO entrega a la GPU (reason = NSDIV_DROP_*). `pack` = paquetes de la cara en cada búfer (como NsDivPrim.pack). */
enum { NSDIV_DROP_OUTCODE = 1, NSDIV_DROP_BACK = 2, NSDIV_DROP_AREA0 = 3, NSDIV_DROP_Z0 = 4, NSDIV_DROP_INDIRECT_Z0 = 5, NSDIV_DROP_SPLIT_LOST = 6 };
typedef void (*NsDivDropFn)(void *user, uint32_t obj, int node, int face, int reason, const uint32_t pack[2]);

typedef struct {
    NsDivDropFn drop;      /* o NULL */
    const uint8_t *scratch;/* memoria rápida de 1 KiB del motor (o NULL): si trae divide_mem->field_14 (+0x14) distinto de 0 se usa como umbral de subdivisión; ver abajo */
    uint32_t div_thresh;   /* umbral de area de subdivision (divide.c:577 fija 0x800); 0 = el de `scratch` si lo trae y no es 0, si no 0x800 */
    int pool;              /* búfer (0/1) de la memoria de paquetes que reparte DG_InitDividePacks; -1 = reloj de búfer del motor; -2 = el contrario */
    int h_override;        /* >0: H en lugar del de la cámara del canal */
    int n_packs_override;  /* >=0: paquetes libres en lugar de los de la RAM */
    uint32_t only_obj;     /* != 0: solo ese objeto (diagnóstico) */
    int flags;
} NsDivCfg;
enum { NSDIV_NO_SPLIT = 1, NSDIV_NO_BUDGET = 2 /* sin tope de paquetes */, NSDIV_NO_NUDGE = 4, NSDIV_IGNORE_RAM_BOUND = 8 /* usar el bound_mode de la RAM en lugar de calcularlo */, NSDIV_NO_OUTCODE = 16 /* no descartar por código de salida común (trans.c:139-152) */ };

#define NSDIV_GAME_FLAGS  NSDIV_NO_OUTCODE   /* (tras el enum de banderas) */

typedef struct {
    uint32_t objs, objs_drawn, objs_gbound;        /* objetos de la cola / con alguna parte dibujada / GBOUND */
    uint32_t models_bound0, models_bound1, models_bound2;
    uint32_t faces, f_outcode, f_back, f_degenerate, f_z0, f_indirect, f_indirect_z0;
    uint32_t f_full, f_split;                      /* caras entregadas enteras / subdivididas */
    uint32_t sub_emitted, sub_outcode, sub_nopack; /* trozos entregados / descartados por código de salida / perdidos por falta de paquetes */
    uint32_t split_depth[4];                       /* trozos por nivel */
    int n_packs_start, n_packs_end, pool, h;
    uint32_t thresh;                               /* umbral de subdivisión usado */
    uint32_t divide_aborted;                       /* 1: DG_InitDividePacks sin ninguna unidad libre -> DG_DivideChanl sale sin meter paquetes (divide.c:571) */
    uint32_t bound_mine0_ram_on, bound_mine_on_ram0, bound_1_vs_2;   /* desglose: yo 0 / RAM encendido; yo encendido / RAM 0; 1 contra 2 */
    uint32_t bound_ram_mismatch;                   /* modelos cuyo bound_mode calculado difiere del de la RAM (diagnóstico) */
} NsDivStats;

/* Ejecuta las etapas Bound/Trans/Divide sobre la RAM (ram_size >= 2 MiB). Devuelve el nº de primitivas entregadas, o -1 si el canal no es legible. */
int ns_div_run(const uint8_t *ram, size_t ram_size, const NsDivCfg *cfg, NsDivAttrFn attr, NsDivEmitFn emit, void *user, NsDivStats *stats);

/* ---- Piezas sueltas (para las pruebas) ---- */
typedef struct { int16_t m[3][3]; int32_t t[3]; } NsDivMat;
/* RTPS de la GTE: proyección entera con división UNR y saturaciones. Devuelve 1 si el vértice es utilizable (siempre). */
void ns_div_rtps(const NsDivMat *M, int H, int ofx, int ofy, const int16_t v[3], int16_t *sx, int16_t *sy, uint16_t *sz);
/* NCLIP: doble área con signo de (0,1,2) en pantalla. */
int32_t ns_div_nclip(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2);

#ifdef __cplusplus
}
#endif
