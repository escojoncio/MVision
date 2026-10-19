#pragma once
/* ns_fx: efectos y primitivas del motor que NO son mallas de modelo, leídos de la RAM del juego (renderer nativo).
 *
 * Qué hace
 *  1. ns_fx_extract: recorre la cola de primitivas del canal 3D principal (sprites/billboards, destellos de lámparas, humo, burbujas,
 *     salpicaduras, ondas, chispas, sangre, trazadores, láseres y cintas, sombras sueltas y de personajes, huellas, plasma, letreros,
 *     nieve/chaff...) y deja UNA lista de elementos en espacio de MUNDO del motor (unidades del juego, Y arriba): billboards con su
 *     tamaño en el mundo (el renderer los orienta a la cámara de CADA OJO), cintas (líneas con anchura), cuadriláteros/triángulos
 *     del mundo (con su sentido de cara para el descarte) y, aparte, el estado de los efectos de PANTALLA COMPLETA que el renderer
 *     nativo tiene que hacer como post-proceso (bajo el agua, anillo de gas, fundido a negro/blanco, gafas...).
 *  2. ns_fx_classify: dada la lista de dibujo de la GPU del mismo fotograma, marca cada primitiva como ya cubierta por el renderer nativo
 *     (malla / efecto de mundo), efecto de pantalla (post-proceso), 2D de verdad (HUD, menús, textos, radio) o mantenimiento de la GPU
 *     (borrados, máscaras, eventos de VRAM), para que el HUD 2D siga saliendo de la GPU sin duplicar nada.
 *
 * Contrato
 *  - Solo lectura de la RAM (2 MB, original, little-endian). Sin dependencias de plataforma. El contrato de tipos del renderer nativo
 *    (`ns_types.h`) aún no existe: aquí se usan tipos propios `NsFx*` (adaptar la salida en una sola función es trivial).
 *  - Las posiciones de `NsFxItem` están en el MUNDO del motor (unidades del juego, ~1 mm; eje Y hacia ARRIBA; el mundo es zurdo si
 *    `NsFxCam.det_neg`). `ns_fx_to_vr` aplica la misma correspondencia que vr_scene (escala, origen, espejo Z, espacio del usuario).
 *  - Los elementos "pegados a la cámara" (VIEWLOCKED) traen además sus coordenadas en espacio de vista del motor (`vpos`).
 *  - Se extraen TODAS las primitivas visibles (no invisibles) del mapa actual, también las que la cámara original no veía: el
 *    motor las recorta por pantalla pero la cabeza VR mira a otro sitio. `NSFX_F_ENGINE_CULLED` marca las que el motor no emitió en
 *    ese fotograma (detrás de la cámara original, cara trasera de una de una sola cara...) para depuración.
 *  - Detalles de formato, coordenadas por tipo de efecto y animación: docs/efectos_nativos.md.
 */
#include <stdint.h>
#include "gpu.h"

#define NSFX_RAM_SIZE 0x200000u

/* Direcciones del EXE principal (edición europea). Ver docs/efectos_nativos.md §1. */
#define NSFX_A_CHANL        0x800B2B7Cu  /* canal 3D principal (cola de objetos y primitivas, matrices de cámara) */
#define NSFX_A_CLOCK        0x800BCAB0u  /* contador de fotogramas: bit 0 = búfer de paquetes que se está generando */
#define NSFX_A_MAP          0x800BCAF8u  /* máscara de mapa actual (grupos visibles) */
#define NSFX_A_GAME_STATUS  0x800AC570u  /* banderas globales (NVG 4, térmicas 8, aturdido 2, chaff 1...) */
#define NSFX_A_ACTOR_LISTS  0x800ADDA8u  /* 9 listas de actores x 68 B */

enum { NSFX_MAX_ITEMS_DEFAULT = 4096, NSFX_MAX_SHADOW_OBJS = 32 };

/* Forma del elemento. */
enum {
    NSFX_SH_NONE = 0,
    NSFX_SH_BILLBOARD = 1,  /* rectángulo que mira a la cámara: centro `pos[0]`, esquinas = centro + der*off[0|2] + abajo*off[1|3] (unidades de vista) */
    NSFX_SH_QUAD = 2,       /* cuadrilátero del mundo, vértices `pos[0..3]` en orden de paquete (triángulos 0,1,2 y 1,2,3) */
    NSFX_SH_TRI = 3,        /* triángulo del mundo, `pos[0..2]` */
    NSFX_SH_RIBBON = 4      /* segmento `pos[0]`-`pos[1]` con anchura `width[0|1]` (unidades del mundo en cada extremo): cinta que mira a la cámara */
};

/* Papel del efecto (heurístico: por el actor dueño si se encuentra, si no por forma/mezcla). */
enum {
    NSFX_ROLE_NONE = 0,
    NSFX_ROLE_GLOW,          /* halos de lámparas, destellos, LED de cámaras, brasa, ojos */
    NSFX_ROLE_BUBBLE,
    NSFX_ROLE_SMOKE,         /* humo, llamas, polvo */
    NSFX_ROLE_SPARK,
    NSFX_ROLE_BLOOD,
    NSFX_ROLE_SPLASH,        /* salpicaduras, cascadas, círculos de viento */
    NSFX_ROLE_RIPPLE,        /* ondas en el agua */
    NSFX_ROLE_TRACER,        /* balas, misiles, estelas de proyectil */
    NSFX_ROLE_LASER,         /* láser de mira, rayos infrarrojos, líneas de visión */
    NSFX_ROLE_FOOTPRINT,
    NSFX_ROLE_SHADOW,        /* sombra sencilla sobre el suelo (objetos, aterrizaje, demos) */
    NSFX_ROLE_SHADOW_CHAR,   /* sombra de personaje (objeto de malla de un cuadrado; ns_fx lo extrae y pide a las mallas que lo omitan) */
    NSFX_ROLE_PLASMA,
    NSFX_ROLE_SURFACE,       /* letreros, paneles, pantallas, puertas/cristales animados */
    NSFX_ROLE_WATER_SURFACE,
    NSFX_ROLE_WEATHER,       /* nieve, chaff */
    NSFX_ROLE_INSECT,
    NSFX_ROLE_SEARCHLIGHT,   /* cono de luz de focos y linternas de patrulla */
    NSFX_ROLE_TRAIL,         /* estela de katana, cables */
    NSFX_ROLE_CLOTH,
    NSFX_ROLE_PARTICLE,
    NSFX_ROLE_SPRITE_ANIM,
    NSFX_ROLE_VIEW_OVERLAY,  /* sprite pegado a la cámara (destello de granada aturdidora...) */
    NSFX_ROLE_COUNT
};

/* Banderas de NsFxItem.flags. */
enum {
    NSFX_F_SEMI = 1,           /* semitransparente (código del paquete bit 1). Texturizado: solo texels con bit 15 */
    NSFX_F_TEXTURED = 2,
    NSFX_F_GOURAUD = 4,
    NSFX_F_RAW = 8,            /* textura sin modular por el color */
    NSFX_F_ONESIDE = 16,       /* una sola cara: ver `fn` */
    NSFX_F_VIEWLOCKED = 32,    /* pegado a la cámara del motor (posiciones en espacio de vista; `pos` es el equivalente en mundo con la cámara actual) */
    NSFX_F_ENGINE_CULLED = 64, /* el motor no lo emitió este fotograma (profundidad 0 en el paquete) */
    NSFX_F_PXSIZE = 128,       /* billboard cuyo tamaño original era en PÍXELES (puntos, nieve): off = px * z / H, convertido al mundo a la profundidad original */
    NSFX_F_ABR_UNKNOWN = 256,  /* semitransparente SIN textura: el modo de mezcla no está en el paquete (lo fija un cambio de modo de dibujo aparte); `abr` = 1 supuesto */
    NSFX_F_FROM_OBJ = 512,     /* viene de un objeto de malla (sombra de personaje) y no de una primitiva */
    NSFX_F_TPAGE_UNKNOWN = 1024 /* sprite texturizado: la página de textura la fija un cambio de modo de dibujo aparte (no está en el paquete); ns_fx_resolve_items la toma de la primitiva de la GPU que casa */
};

typedef struct {
    uint32_t prim;        /* dirección del DG_PRIM (o del objeto de malla si NSFX_F_FROM_OBJ) */
    uint16_t idx;         /* índice del paquete dentro de la primitiva */
    uint16_t sub;         /* segmento dentro del paquete (cintas de polilínea) */
    uint8_t shape;        /* NSFX_SH_* */
    uint8_t role;         /* NSFX_ROLE_* */
    uint8_t kind;         /* tipo de paquete del motor (0..23, ver docs) */
    uint8_t abr;          /* modo de mezcla 0 (B+F)/2, 1 B+F, 2 B-F, 3 B+F/4 (solo con NSFX_F_SEMI) */
    uint16_t flags;       /* NSFX_F_* */
    uint16_t gflags;      /* GPU_F_* equivalentes (GOURAUD, TEXTURED, RAW, SEMI) para reutilizar el camino de VrVtx */
    uint16_t tpage;       /* palabra tpage del paquete (formato GP0(E1h) sin ventana); 0 sin textura */
    uint16_t clut;        /* palabra CLUT del paquete (clx/16 | cly<<6); 0 sin textura */
    uint8_t tp;           /* modo de color de la textura: 0 4 bits, 1 8 bits, 2/3 15 bits */
    uint8_t nv;           /* vértices con datos válidos en pos/uv/rgb (billboard: 4 esquinas de textura) */
    uint8_t uv[4][2];     /* u,v de las 4 esquinas en orden de paquete (billboard: 0 sup-izq, 1 sup-der, 2 inf-izq, 3 inf-der) */
    uint8_t rgb[4][3];    /* color por esquina (modulación 128 = neutro); sin gouraud todas iguales */
    int32_t sz;           /* profundidad de vista del motor del vértice 0 / centro en este fotograma */
    int16_t raise;        /* sesgo de orden de dibujo del motor: el elemento se ordenaba como si estuviese `raise` unidades más cerca */
    float pos[4][3];      /* mundo del motor */
    float vpos[4][3];     /* espacio de vista del motor (x der, y abajo, z adelante) */
    float off[4];         /* billboard: x_izq, y_sup, x_der, y_inf en unidades de vista (y hacia abajo) respecto al centro */
    float width[2];       /* cinta: anchura en unidades del mundo en cada extremo */
    float fn[3];          /* una cara: normal normalizada tal que el motor lo dibuja si dot(fn, p - ojo) > 0 */
} NsFxItem;

/* Estado de la cámara del motor en este fotograma. */
typedef struct {
    float inv[3][3];      /* mundo -> vista (la de la GTE, 1.12 pasado a float) */
    float inv_t[3];
    float axes[3][3];     /* vista -> mundo (inversa exacta): columna c = eje c de la vista (x der, y abajo, z adelante) en el mundo */
    float eye[3];         /* posición del ojo del motor en el mundo */
    float proj_h;         /* distancia de pantalla (focal en píxeles) */
    float y_scale;        /* factor vertical del motor sobre la proyección de posiciones (66/64 en la edición europea) */
    int det_neg;          /* 1 si el mundo es zurdo (det de la rotación de vista < 0): espejo en Z al pasar a VR */
    int valid;
} NsFxCam;

/* --- efectos de pantalla completa (post-proceso) cuyo estado se lee de la RAM --- */
typedef struct {
    int active;           /* el actor de vista submarina existe y ha reservado sus paquetes (el ojo del motor está en el volumen) */
    int present;          /* hay actor (aunque ahora no actúe) */
    float box_min[3], box_max[3];   /* volumen de agua en el mundo (mín/máx por eje; la cara superior es la superficie) */
    float tint[3];        /* multiplicador por canal (color/128) */
    uint32_t phase;       /* contador de fotogramas de la onda */
    int wave_rows;        /* amplitud de la onda en filas originales (+-4) */
    int wave_period;      /* periodo de la onda en pasos (32), cada paso = 2 filas originales */
    int bar_rows;         /* barras negras arriba y abajo en filas originales (6) */
    uint32_t actor;
} NsFxWater;

typedef struct {
    int active;           /* hay anillo de gas dibujándose */
    int present;
    float intensity;      /* 0..1 (rampa de entrada/salida) */
    float inner_px;       /* radio horizontal interior en píxeles originales */
    float outer_px;       /* radio horizontal exterior (230) */
    float aspect;         /* radio vertical / horizontal (f64/4096) */
    float col_inner[3], col_outer[3];   /* color 0..1 del borde interior/exterior, ya con intensidad y pulso */
    int abr;              /* modo de mezcla */
    float pulse_amp;      /* amplitud relativa del pulso (f50/4096) */
    uint32_t actor;
} NsFxGas;

typedef struct {
    int active;
    int white;            /* 1 fundido a blanco (suma), 0 a negro (resta) */
    int fade_in;          /* 1 si va de cubierto a descubierto */
    float amount;         /* cobertura actual 0..1 (0 = imagen normal, 1 = todo negro/blanco) */
    uint32_t actor;
} NsFxFade;

typedef struct {
    NsFxWater water;
    NsFxGas gas;
    NsFxFade fade;
    uint32_t game_status; /* BA_GAME_STATUS crudo */
    int nvg, thermal;     /* gafas de visión nocturna / térmicas puestas */
    int stun, chaff;
} NsFxScreen;

typedef struct {
    NsFxCam cam;
    NsFxScreen screen;
    NsFxItem *items; uint32_t n_items, cap_items;
    uint32_t shadow_objs[NSFX_MAX_SHADOW_OBJS]; uint32_t n_shadow_objs;   /* DG_OBJS (objetos de malla) ya extraídos aquí: el renderer de mallas debe OMITIRLOS */
    int buffer;           /* búfer de paquetes usado (0/1) */
    uint32_t cur_map;
    struct {
        uint32_t n_prims;         /* primitivas en la cola */
        uint32_t n_sortonly;      /* de ordenación 2D (HUD/menús): no se extraen */
        uint32_t n_invisible;
        uint32_t n_wrong_map;
        uint32_t n_bad;           /* punteros o contadores incoherentes */
        uint32_t n_free;          /* primitivas de formato libre (callback propio): su paquete no se interpreta, se extraen solo si el motor las proyectó como polígono/sprite estándar */
        uint32_t n_overflow;      /* elementos que no cupieron */
        uint32_t by_shape[5];
        uint32_t by_role[NSFX_ROLE_COUNT];
        uint32_t n_culled;        /* NSFX_F_ENGINE_CULLED */
        /* comprobación de reproyección contra las coordenadas de pantalla de los paquetes del motor (vértices de polígonos del mundo y esquinas de billboards) */
        uint32_t chk_n; double chk_sum_px; float chk_max_px; uint32_t chk_bad;
    } stats;
} NsFxScene;

typedef struct {
    int buffer;           /* 0/1 fuerza el búfer de paquetes; -1 (def.) elige el que reproyecta mejor con la cámara actual (empate: bit 0 de NSFX_A_CLOCK) */
    float y_scale;        /* factor vertical del motor (def. 66/64) */
    int owner_roles;      /* 1 (def.): busca el actor dueño de cada primitiva para fijar `role` por su nombre */
    int include_viewlocked;   /* 1 (def.) */
    int include_engine_culled;/* 1 (def.) */
    int check_reproj;     /* 1 (def.): calcula stats.chk_* */
} NsFxOpts;

NsFxOpts ns_fx_opts_default(void);
/* Reserva/libera la escena (capacidad de elementos; 0 = NSFX_MAX_ITEMS_DEFAULT). */
NsFxScene *ns_fx_scene_create(uint32_t cap_items);
void ns_fx_scene_destroy(NsFxScene *s);
/* Extrae la escena de efectos de una RAM de 2 MB. Devuelve 0 si va bien; <0 si faltan argumentos (-1) o la cámara/cola no es coherente (-2: la escena
 * queda vacía, cam.valid = 0; típico en título/cargas, donde la cola puede estar sin inicializar). */
int ns_fx_extract(const uint8_t *ram, const NsFxOpts *opt, NsFxScene *out);

/* ---- correspondencia mundo del motor -> VR (misma que vr_scene; ver vr_scene.h "Espacio mundo VR y escala") ---- */
typedef struct {
    float scale;              /* metros por unidad (def. 0,001) */
    float origin[3];          /* unidades del juego */
    int mirror_z;             /* -1 auto (por NsFxCam.det_neg), 0 no, 1 sí */
    int user_map_on;          /* P = R*(k*S*(w-origin)) - R[:,1]*eye_h + p_ref, S = scale con el espejo en Z (como vr_scene) */
    float user_R[9], user_k, user_eye_h_m, user_p_ref[3];
} NsFxXform;
NsFxXform ns_fx_xform_default(void);
void ns_fx_to_vr(const NsFxXform *x, const NsFxCam *cam, const float w[3], float out[3]);
/* Dirección (sin traslación) y factor de escala lineal total (metros VR por unidad del juego). */
void ns_fx_dir_to_vr(const NsFxXform *x, const NsFxCam *cam, const float d[3], float out[3]);
float ns_fx_len_scale(const NsFxXform *x);

/* Esquinas VR de un billboard: der_vr/arriba_vr = ejes de la cabeza/ojo en el espacio VR de salida (unitarios, ortogonales). Orden de esquinas = uv[0..3]. */
void ns_fx_billboard_corners(const NsFxItem *it, const NsFxXform *x, const NsFxCam *cam, const float right_vr[3], const float up_vr[3], float out[4][3]);
/* Cinta como tira de 4 vértices (0 =p0-lado, 1 = p0+lado, 2 = p1-lado, 3 = p1+lado) que mira al ojo `eye_vr`. Devuelve 0 si es degenerada. */
int ns_fx_ribbon_corners(const NsFxItem *it, const NsFxXform *x, const NsFxCam *cam, const float eye_vr[3], float out[4][3]);
/* 1 si el motor dibujaría el elemento de una cara visto desde `eye_w` (mundo del motor); los de dos caras devuelven 1. */
int ns_fx_item_faces(const NsFxItem *it, const float eye_w[3]);

/* ---- clasificación de las primitivas de la lista de la GPU ---- */
enum {
    NSFX_OP_EVENT = 0,     /* no dibuja (fill/copia/subida/descarga de VRAM): no se toca */
    NSFX_OP_2D,            /* HUD, menús, textos, radio, retratos, radar...: sigue saliendo de la GPU */
    NSFX_OP_MESH,          /* paquete de objeto de malla (coincide con un paquete del motor): lo cubre el renderer de mallas */
    NSFX_OP_MESH_GUESS,    /* 3D (posición de vista conocida) sin paquete que coincida: subdividido por el motor / malla: lo cubre el renderer de mallas */
    NSFX_OP_FX,            /* coincide con un elemento de NsFxScene.items: lo cubre ns_fx */
    NSFX_OP_FX_PACK,       /* coincide con un paquete de una primitiva de efectos que no se extrae (p.ej. invisible o de otro mapa): no dibujar */
    NSFX_OP_SCREEN,        /* efecto de pantalla completa (post-proceso): copia de pantalla, recubrimiento, líneas, marco, anillo, barras */
    NSFX_OP_MAINT          /* borrado/máscara de la GPU: no dibujar */
};
/* Subtipo de NSFX_OP_SCREEN. */
enum {
    NSFX_SC_NONE = 0,
    NSFX_SC_COPY,          /* relee el búfer de pantalla (tiras de agua, enfoque) */
    NSFX_SC_COVER,         /* recubrimiento semitransparente a pantalla completa (fundido, destello, tinte de gafas) */
    NSFX_SC_BARS,          /* barras negras opacas arriba/abajo (cine, agua) */
    NSFX_SC_SCAN,          /* líneas de exploración finas de ancho de pantalla */
    NSFX_SC_FRAME,         /* marco de máscara/gafas/caja (muchos polígonos planos o degradados) */
    NSFX_SC_RING,          /* anillo/viñeta degradada semitransparente alrededor del centro (gas) */
    NSFX_SC_GHOST          /* sprite que mezcla el fotograma anterior */
};
typedef struct {
    uint32_t n_ops, n_event, n_2d, n_mesh, n_mesh_guess, n_fx, n_fx_pack, n_screen, n_maint;
    uint32_t sub[8];          /* por NSFX_SC_* */
    uint32_t n_fx_items_hit;  /* elementos distintos de la escena que casaron con alguna primitiva */
    uint32_t n_fx_items_vis;  /* elementos de la escena que el motor emitió (no ENGINE_CULLED) y están dentro del área mostrada */
    uint32_t n_fx_items_missing; /* ... de los cuales ninguna primitiva de la lista casó: efectos que el motor emitió y no estarán (¿lista de otro fotograma?) */
} NsFxClsStats;
/* cls (n_ops bytes, NSFX_OP_*) y, opcionales, sub (n_ops bytes, NSFX_SC_*) e item (n_ops int32: índice del elemento casado o -1). Devuelve 0 o <0.
 * `ram` hace falta para indexar los paquetes de objetos de malla y de las primitivas descartadas; con NULL solo se casa con `sc->items`. */
int ns_fx_classify(const uint8_t *ram, const NsFxScene *sc, const GpuDrawList *dl, uint8_t *cls, uint8_t *sub, int32_t *item, NsFxClsStats *st);
/* Tras classify: rellena de las primitivas de la GPU casadas (item[i] = índice de elemento o -1) lo que el paquete no trae (página de textura, modo de
 * mezcla de sprites y rectángulos sin textura). Devuelve cuántos elementos se completaron. */
int ns_fx_resolve_items(NsFxScene *sc, const GpuDrawList *dl, const int32_t *item);
const char *ns_fx_role_name(int role);
const char *ns_fx_shape_name(int shape);
