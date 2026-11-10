#pragma once
/* ns_fx: efectos y primitivas del motor que NO son mallas de modelo, leídos de la RAM (renderer nativo).
 *
 * Qué hace
 *  1. ns_fx_extract: recorre la cola de primitivas del canal 3D principal (sprites y carteles, destellos, partículas, ondas, trazadores, haces,
 *     cintas, sombras...) y deja UNA lista de elementos en espacio de MUNDO del motor (Y arriba): carteles con su tamaño en el mundo, cintas
 *     (líneas con anchura), cuadriláteros/triángulos de mundo (con su sentido de cara) y, aparte, el estado de los efectos de PANTALLA COMPLETA
 *     que el renderer nativo hace como post-proceso (bajo el agua, anillo, fundido, modos de visión).
 *  2. ns_fx_classify: dada la lista de dibujo de la GPU del mismo fotograma, etiqueta cada primitiva con una CLASE CANDIDATA.
 *
 * Contrato de cobertura CONFIRMADA (importante)
 *  - Lo único que ns_fx garantiza es 2D/EVENT/MAINT: se queda en la GPU. Todo lo demás (MESH, MESH_GUESS, FX, FX_PACK, SCREEN) es solo CANDIDATO a
 *    estar cubierto por el renderer nativo. El renderer indica con `confirmed[i]` qué operaciones ha dibujado de verdad (malla/efecto/post-proceso
 *    equivalente) y `ns_fx_gpu_mask` devuelve lo que debe seguir saliendo por la GPU: todo lo no confirmado. Peor caso aceptado: un doble dibujo
 *    cosmético; nunca debe desaparecer HUD, menús, radio ni textos. Ante la duda, 2D.
 *  - Los efectos de pantalla solo se reconocen si el estado de la RAM los justifica (fundido, modos de visión, agua, anillo activos).
 *
 * Contrato de tipos
 *  - Solo lectura de la RAM (2 MB, little-endian). Sin dependencias de plataforma. Las posiciones de `NsFxItem` están en el MUNDO del motor
 *    (Y arriba; zurdo si `NsFxCam.det_neg`). `ns_fx_to_vr` aplica la misma correspondencia que vr_scene.
 *  - Los carteles y cintas se orientan UNA vez por fotograma con los ejes y la posición de la CABEZA (centro de los ojos); el descarte de caras
 *    de los polígonos de una cara (`ns_fx_item_faces`) sí se evalúa por ojo.
 *  - Se extraen TODAS las primitivas visibles del mapa actual, también las que la cámara original no veía (`NSFX_F_ENGINE_CULLED`, y
 *    `NSFX_F_LOW_CONF` cuando el motor no dejó profundidad en el paquete).
 *  - Detalles: docs/efectos_nativos.md.
 */
#include <stdint.h>
#include "gpu.h"

#define NSFX_RAM_SIZE 0x200000u

/* Direcciones de la RAM del motor (ver docs/efectos_nativos.md §1). */
#define NSFX_A_CHANL        0x800B2B7Cu  /* canal 3D principal (cola de objetos y primitivas, matrices de cámara) */
#define NSFX_A_CLOCK        0x800BCAB0u  /* contador de fotogramas: bit 0 = búfer de paquetes que se está generando */
#define NSFX_A_MAP          0x800ACAF8u  /* máscara de mapa actual (grupos visibles) */
#define NSFX_A_STATUS       0x800AC570u  /* banderas globales de estado (bits 0..3: ver NsFxScreen) */
#define NSFX_A_ACTOR_LISTS  0x800ADDA8u  /* 9 listas de actores x 68 B */

enum { NSFX_MAX_ITEMS_DEFAULT = 4096, NSFX_MAX_SHADOW_OBJS = 64 };

/* Forma del elemento. */
enum {
    NSFX_SH_NONE = 0,
    NSFX_SH_BILLBOARD = 1,  /* rectángulo que mira a la cámara: centro `pos[0]`, esquinas = centro + der*off[0|2] + abajo*off[1|3] (unidades de vista) */
    NSFX_SH_QUAD = 2,       /* cuadrilátero del mundo, vértices `pos[0..3]` en orden de paquete (triángulos 0,1,2 y 1,2,3) */
    NSFX_SH_TRI = 3,        /* triángulo del mundo, `pos[0..2]` */
    NSFX_SH_RIBBON = 4      /* segmento `pos[0]`-`pos[1]` con anchura `width[0|1]` (unidades del mundo en cada extremo): cinta que mira a la cabeza */
};

/* Papel del efecto (heurístico y solo informativo: por el actor dueño si se encuentra, si no por forma/mezcla). */
enum {
    NSFX_ROLE_NONE = 0,
    NSFX_ROLE_LIGHT,         /* halos y destellos aditivos */
    NSFX_ROLE_BUBBLE,
    NSFX_ROLE_SMOKE,
    NSFX_ROLE_SPARK,
    NSFX_ROLE_DROPLET,
    NSFX_ROLE_SPLASH,
    NSFX_ROLE_RIPPLE,        /* ondas sobre una superficie */
    NSFX_ROLE_TRACER,
    NSFX_ROLE_BEAM,          /* haces y líneas largas */
    NSFX_ROLE_DECAL,         /* marcas planas sobre el suelo */
    NSFX_ROLE_SHADOW,        /* sombra sencilla sobre el suelo */
    NSFX_ROLE_SHADOW_OBJ,    /* sombra de objeto de malla de un cuadrado (se extrae aquí y se pide a las mallas que la omitan) */
    NSFX_ROLE_PLASMA,
    NSFX_ROLE_SURFACE,       /* paneles y superficies planas animadas */
    NSFX_ROLE_WATER_SURFACE,
    NSFX_ROLE_AMBIENT,       /* partículas ambientales */
    NSFX_ROLE_CONE,          /* conos de luz */
    NSFX_ROLE_TRAIL,
    NSFX_ROLE_STRIP,
    NSFX_ROLE_PARTICLE,
    NSFX_ROLE_SPRITE_ANIM,
    NSFX_ROLE_VIEW_OVERLAY,  /* sprite pegado a la cámara */
    NSFX_ROLE_COUNT
};
/* Códigos de actor que no son papel de primitiva (tabla de actores; ver NsFxActorEntry). */
enum { NSFX_AK_WATER = 100, NSFX_AK_GAS = 101, NSFX_AK_FADE = 102 };

/* Banderas de NsFxItem.flags. */
enum {
    NSFX_F_SEMI = 1,           /* semitransparente (código del paquete bit 1). Texturizado: solo texels con bit 15 */
    NSFX_F_TEXTURED = 2,
    NSFX_F_GOURAUD = 4,
    NSFX_F_RAW = 8,            /* textura sin modular por el color */
    NSFX_F_ONESIDE = 16,       /* una sola cara: ver `fn` */
    NSFX_F_VIEWLOCKED = 32,    /* pegado a la cámara del motor (`vpos` en espacio de vista; `pos` es el equivalente en mundo con la cámara actual) */
    NSFX_F_ENGINE_CULLED = 64, /* el motor no lo emitió este fotograma (profundidad 0 en el paquete) */
    NSFX_F_PXSIZE = 128,       /* tamaño original en PÍXELES: off = px * z / H a la profundidad original (o a la de referencia si no es fiable) */
    NSFX_F_ABR_UNKNOWN = 256,  /* semitransparente SIN modo de mezcla en el paquete: `abr` = 1 supuesto; ns_fx_resolve_items lo toma de la GPU */
    NSFX_F_FROM_OBJ = 512,     /* viene de un objeto de malla (sombra) y no de una primitiva */
    NSFX_F_TPAGE_UNKNOWN = 1024, /* sprite texturizado: la página de textura no está en el paquete; ns_fx_resolve_items la toma de la GPU */
    NSFX_F_LOW_CONF = 2048,    /* sin profundidad en el paquete (sz == 0): posiciones de mundo válidas, pero ni la reproyección ni la casación con la GPU son fiables */
    NSFX_F_NOSIZE = 8192,      /* cartel de tamaño en píxeles que el motor no emitió este fotograma y del que no hay tamaño en mundo aprendido: no se exporta (nunca se inventa una profundidad) */
    NSFX_F_LEARNED = 4096      /* la página de textura y/o el modo de mezcla no estaban en el paquete y se tomaron de una lista de la GPU anterior (ns_fx_learn) o de la última sombra emitida */
};

typedef struct {
    uint32_t prim;        /* dirección de la primitiva (o del objeto de malla si NSFX_F_FROM_OBJ) */
    uint16_t idx;         /* índice del paquete dentro de la primitiva */
    uint16_t sub;         /* segmento dentro del paquete (polilíneas) */
    uint8_t shape;        /* NSFX_SH_* */
    uint8_t role;         /* NSFX_ROLE_* */
    uint8_t kind;         /* clase de paquete del motor (0..23, ver docs) */
    uint8_t abr;          /* modo de mezcla 0 (B+F)/2, 1 B+F, 2 B-F, 3 B+F/4 (solo con NSFX_F_SEMI) */
    uint16_t flags;       /* NSFX_F_* */
    uint16_t gflags;      /* GPU_F_* equivalentes (GOURAUD, TEXTURED, RAW, SEMI) */
    uint16_t tpage;       /* palabra tpage del paquete (formato GP0(E1h) sin ventana); 0 sin textura */
    uint16_t clut;        /* palabra CLUT del paquete (clx/16 | cly<<6); 0 sin textura */
    uint8_t tp;           /* modo de color de la textura: 0 4 bits, 1 8 bits, 2/3 15 bits */
    uint8_t nv;           /* vértices con datos válidos en pos/uv/rgb (cartel: 4 esquinas de textura) */
    uint8_t uv[4][2];     /* u,v de las 4 esquinas (cartel: 0 sup-izq, 1 sup-der, 2 inf-izq, 3 inf-der) */
    uint8_t rgb[4][3];    /* color por esquina (128 = neutro); sin gouraud todas iguales */
    int32_t sz;           /* profundidad de vista del motor del vértice 0 / centro en este fotograma */
    int16_t raise;        /* sesgo de orden de dibujo del motor */
    float pos[4][3];      /* mundo del motor */
    float vpos[4][3];     /* espacio de vista del motor (x der, y abajo, z adelante) */
    float off[4];         /* cartel: x_izq, y_sup, x_der, y_inf en unidades de vista (y hacia abajo) respecto al centro */
    float box_px[4];      /* cartel: la misma caja en píxeles del motor (izq, sup, der, inf) respecto al punto proyectado; PXSIZE: los píxeles originales */
    float width[2];       /* cinta: anchura en unidades del mundo en cada extremo */
    float fn[3];          /* una cara: normal normalizada tal que el motor lo dibuja si dot(fn, p - ojo) > 0 */
    /* datos del paquete del motor para casarlo con la lista de la GPU sin volver a la RAM */
    int16_t sxy[4][2];    /* x,y de pantalla del paquete (sin offset de dibujo) */
    uint8_t pcls;         /* 0 línea, 1 rectángulo (sprite), 2 polígono */
    uint8_t pnv;          /* vértices que ve la GPU (línea 2, rectángulo 1, polígono 3/4) */
    uint8_t pu0, pv0;     /* u,v del vértice 0 del paquete */
    uint16_t sw, sh;      /* rectángulos: tamaño en píxeles */
    uint8_t ptp;          /* modo de color del tpage del paquete (255 = no consta) */
} NsFxItem;

/* Estado de la cámara del motor en este fotograma. */
typedef struct {
    float inv[3][3];      /* mundo -> vista (1.12 pasado a float) */
    float inv_t[3];
    float axes[3][3];     /* vista -> mundo (inversa exacta): columna c = eje c de la vista (x der, y abajo, z adelante) en el mundo */
    float eye[3];         /* posición del ojo del motor en el mundo */
    float proj_h;         /* distancia de pantalla (focal en píxeles) */
    float y_scale;        /* factor vertical del motor sobre la proyección de posiciones (NsFxTuning.y_scale) */
    int det_neg;          /* 1 si el mundo es zurdo (det de la rotación de vista < 0): espejo en Z al pasar a VR */
    int valid;
} NsFxCam;

/* --- efectos de pantalla completa (post-proceso) cuyo estado se lee de la RAM --- */
typedef struct {
    int active;           /* el actor existe y ha reservado sus paquetes (el ojo del motor está en el volumen) */
    int present;
    float box_min[3], box_max[3];   /* volumen en el mundo (la cara superior es la superficie) */
    float tint[3];        /* multiplicador por canal (color/128) */
    uint32_t phase;       /* contador de fotogramas de la onda */
    int wave_rows;        /* amplitud de la onda en filas (NsFxTuning) */
    int wave_period;      /* periodo de la onda en pasos */
    int bar_rows;         /* barras negras arriba y abajo en filas */
    uint32_t actor;
} NsFxWater;

typedef struct {
    int active;           /* anillo dibujándose */
    int present;
    float intensity;      /* 0..1 (rampa de entrada/salida), recortada */
    float inner_px;       /* radio horizontal interior en píxeles */
    float outer_px;       /* radio horizontal exterior (NsFxTuning) */
    float aspect;         /* radio vertical / horizontal, recortado a 0..2 */
    float col_inner[3], col_outer[3];   /* color 0..1 del borde interior/exterior, con intensidad y pulso, recortado */
    int abr;              /* modo de mezcla 0..3 */
    float pulse_amp;      /* amplitud relativa del pulso */
    uint32_t actor;
} NsFxGas;

typedef struct {
    int active;           /* cantidad > 0 */
    int white;            /* 1 a blanco (suma), 0 a negro (resta) */
    int fade_in;          /* 1 si va de cubierto a descubierto */
    float amount;         /* cobertura actual 0..1 (0 = imagen normal) */
    uint32_t actor;
} NsFxFade;

typedef struct {
    NsFxWater water;
    NsFxGas gas;
    NsFxFade fade;
    uint32_t status;      /* palabra de estado global cruda (NSFX_A_STATUS) */
    int mode_a, mode_b;   /* bits 2 y 3: modos de visión con recoloreado de CLUT y recubrimiento de pantalla */
    int flash;            /* bit 1: destello de pantalla */
    int aux;              /* bit 0 */
} NsFxScreen;

/* Tabla de ajustes: constantes del motor con descripción genérica. Los valores por defecto están en ns_fx_tuning_default. */
typedef struct {
    float y_scale;        /* factor vertical de la proyección de posiciones */
    float line_px;        /* grosor de las líneas de color plano, en píxeles */
    float line_tex_px;    /* grosor de las líneas texturizadas, en píxeles */
    float shadow_half;    /* semilado del cuadrado base de los objetos de sombra, antes de escalar */
    float px_z_min;       /* SIN USO (antes: profundidad mínima fiable de un tamaño en píxeles). Los tamaños se convierten con la z real del motor */
    float px_z_ref;       /* SIN USO (antes: profundidad de referencia). Un cartel sin emisión usa su tamaño en mundo aprendido o no se exporta */
    float ring_outer_px;  /* radio horizontal exterior del anillo */
    int ring_segments;    /* polígonos del anillo */
    int wave_rows, wave_period, bar_rows;   /* ondulación y barras bajo el agua */
    uint32_t idx_max;     /* tope global de entradas del índice de coincidencias (al desbordar, todo 2D) */
    int chain_max;        /* tope de la cadena hash en una búsqueda (al superarlo en una inserción, todo 2D) */
} NsFxTuning;
NsFxTuning ns_fx_tuning_default(void);

typedef struct {
    NsFxCam cam;
    NsFxScreen screen;
    NsFxItem *items; uint32_t n_items, cap_items;
    uint32_t shadow_objs[NSFX_MAX_SHADOW_OBJS]; uint32_t n_shadow_objs;   /* objetos de malla ya extraídos aquí: el renderer de mallas debe OMITIRLOS */
    int buffer;           /* búfer de paquetes usado (0/1) */
    uint32_t cur_map;
    /* tabla (primitiva, paquete) -> primer elemento, precalculada en la extracción */
    uint64_t *pm_key; int32_t *pm_val; uint32_t pm_mask;
    struct {
        uint32_t n_prims;         /* primitivas en la cola */
        uint32_t n_sortonly;      /* de ordenación 2D (HUD/menús): no se extraen */
        uint32_t n_invisible;
        uint32_t n_wrong_map;
        uint32_t n_bad;           /* punteros o contadores incoherentes */
        uint32_t n_free;          /* primitivas de formato libre: no se interpretan (sus paquetes se indexan como 2D) */
        uint32_t n_overflow;      /* elementos que no cupieron */
        uint32_t by_shape[5];
        uint32_t by_role[NSFX_ROLE_COUNT];
        uint32_t n_culled;        /* NSFX_F_ENGINE_CULLED */
        uint32_t chk_n; double chk_sum_px; float chk_max_px; uint32_t chk_bad;   /* reproyección contra las coordenadas de pantalla de los paquetes */
    } stats;
} NsFxScene;

typedef struct {
    int buffer;           /* 0/1 fuerza el búfer de paquetes; -1 (def.) elige el que reproyecta mejor (empate: bit 0 de NSFX_A_CLOCK) */
    NsFxTuning tune;
    int owner_roles;      /* 1 (def.): busca el actor dueño de cada primitiva para fijar `role` */
    int include_viewlocked;   /* 1 (def.) */
    int include_engine_culled;/* 1 (def.) */
    int check_reproj;     /* 1 (def.): calcula stats.chk_* */
} NsFxOpts;

NsFxOpts ns_fx_opts_default(void);

/* Tabla de actores inyectable: hash FNV-1a 32 del nombre del actor -> papel (NSFX_ROLE_*) o NSFX_AK_*. La tabla de producción se instala por defecto;
 * ns_fx_set_actor_table(NULL, 0) la restaura. Las pruebas instalan una con nombres neutros. */
typedef struct { uint32_t hash; int code; } NsFxActorEntry;
void ns_fx_set_actor_table(const NsFxActorEntry *t, uint32_t n);
uint32_t ns_fx_name_hash(const char *s);

NsFxScene *ns_fx_scene_create(uint32_t cap_items);
void ns_fx_scene_destroy(NsFxScene *s);
/* Extrae la escena de efectos de una RAM de 2 MB. Devuelve 0; -1 si faltan argumentos; -2 si la cámara/cola no es coherente (escena vacía, cam.valid = 0). */
int ns_fx_extract(const uint8_t *ram, const NsFxOpts *opt, NsFxScene *out);

/* GARANTÍA de cobertura nativa de un paquete del motor. `pkt_addr` = dirección de la palabra-etiqueta del paquete (GpuOp.pkt_addr), en cualquiera de los dos búferes de paquetes de su
 * primitiva; `sc` = escena extraída de ESTA `ram`. Devuelve 1 solo si el paquete es de una primitiva de la cola del canal 3D y `sc` tiene un elemento de ese paquete que se exporta al
 * renderer (ns_fx_export). 0 en todo lo demás: paquetes que no son de la cola (HUD, efectos de pantalla con paquetes propios), primitivas que ns_fx no extrae (solo ordenación, invisibles,
 * de otro mapa, incoherentes, ya retiradas) y sprites texturizados sin página de textura conocida. Un elemento que el motor ya no emite en esta RAM (NSFX_F_ENGINE_CULLED, tenga tamaño o no)
 * cuenta: la lista de la GPU es de un fotograma anterior y el motor ya dejó de dibujarlo. */
int ns_fx_covers_packet(const uint8_t *ram, const NsFxScene *sc, uint32_t pkt_addr);

/* ---- correspondencia mundo del motor -> VR (misma que vr_scene) ---- */
typedef struct {
    float scale;              /* metros por unidad (def. 0,001) */
    float origin[3];          /* unidades del motor */
    int mirror_z;             /* -1 auto (por NsFxCam.det_neg), 0 no, 1 sí */
    int user_map_on;          /* P = R*(k*S*(w-origin)) - R[:,1]*eye_h + p_ref, S = scale con el espejo en Z (como vr_scene) */
    float user_R[9], user_k, user_eye_h_m, user_p_ref[3];
} NsFxXform;
NsFxXform ns_fx_xform_default(void);
void ns_fx_to_vr(const NsFxXform *x, const NsFxCam *cam, const float w[3], float out[3]);
void ns_fx_dir_to_vr(const NsFxXform *x, const NsFxCam *cam, const float d[3], float out[3]);
float ns_fx_len_scale(const NsFxXform *x);

/* Esquinas VR de un cartel orientado a la CABEZA: head_right/head_up = ejes de la cabeza en el espacio VR de salida (unitarios, ortogonales); se
 * orienta una vez por fotograma, no por ojo. Orden de esquinas = uv[0..3]. */
void ns_fx_billboard_corners(const NsFxItem *it, const NsFxXform *x, const NsFxCam *cam, const float head_right[3], const float head_up[3], float out[4][3]);
/* Cinta como tira de 4 vértices (0 = p0-lado, 1 = p0+lado, 2 = p1-lado, 3 = p1+lado) que mira a la cabeza `head_vr` (centro de los ojos). 0 si degenerada. */
int ns_fx_ribbon_corners(const NsFxItem *it, const NsFxXform *x, const NsFxCam *cam, const float head_vr[3], float out[4][3]);
/* 1 si el motor dibujaría el elemento de una cara visto desde `eye_w` (mundo del motor; se evalúa por ojo); los de dos caras devuelven 1. */
int ns_fx_item_faces(const NsFxItem *it, const float eye_w[3]);

/* ---- clasificación de las primitivas de la lista de la GPU (CANDIDATAS; ver contrato arriba) ---- */
enum {
    NSFX_OP_EVENT = 0,     /* no dibuja (fill/copia/subida/descarga de VRAM): no se toca */
    NSFX_OP_2D,            /* HUD, menús, textos, radio... y todo lo que no se pueda justificar: sigue saliendo de la GPU */
    NSFX_OP_MESH,          /* CANDIDATO: coincide exactamente con un paquete emitido de un objeto de malla */
    NSFX_OP_MESH_GUESS,    /* CANDIDATO: polígono con posición de vista limpia (sin AMBIG/SAT/BEHIND) y sin paquete que coincida */
    NSFX_OP_FX,            /* CANDIDATO: coincide exactamente con un elemento de NsFxScene.items */
    NSFX_OP_FX_PACK,       /* CANDIDATO: coincide con un paquete de una primitiva de efectos no extraída (invisible/otro mapa) y tiene posición de vista limpia */
    NSFX_OP_SCREEN,        /* CANDIDATO: efecto de pantalla completa justificado por el estado de la RAM (ver NSFX_SC_*) */
    NSFX_OP_MAINT          /* reservado: no lo produce ns_fx (todo lo no reconocido es 2D) */
};
/* Subtipo de NSFX_OP_SCREEN y condición de la RAM que lo justifica. */
enum {
    NSFX_SC_NONE = 0,
    NSFX_SC_COPY,          /* relee el área de dibujo vigente: solo con agua activa */
    NSFX_SC_COVER,         /* recubrimiento semitransparente a pantalla completa: solo con fundido, destello o modo de visión activos */
    NSFX_SC_BARS,          /* barras opacas arriba/abajo de ancho de pantalla: solo con agua activa */
    NSFX_SC_SCAN,          /* líneas finas de ancho de pantalla: solo con modo de visión activo */
    NSFX_SC_RING           /* exactamente `ring_segments` polígonos degradados semitransparentes con simetría radial: solo con anillo activo */
};
typedef struct {
    uint32_t n_ops, n_event, n_2d, n_mesh, n_mesh_guess, n_fx, n_fx_pack, n_screen, n_maint;
    uint32_t sub[8];          /* por NSFX_SC_* */
    uint32_t n_fx_items_hit;  /* elementos distintos de la escena que casaron con alguna primitiva */
    uint32_t n_fx_items_vis;  /* elementos que el motor emitió con fiabilidad (ni ENGINE_CULLED ni LOW_CONF) */
    uint32_t n_fx_items_missing; /* ... de los cuales ninguna primitiva de la lista casó */
    uint32_t idx_overflow;    /* 1 si el índice se desbordó: todo lo que no es evento salió como 2D */
} NsFxClsStats;
/* cls (n_ops bytes, NSFX_OP_*) y, opcionales, sub (n_ops bytes, NSFX_SC_*) e item (n_ops int32: índice del elemento casado o -1).
 * `ram` hace falta para indexar los paquetes de objetos de malla y de las primitivas no extraídas; con NULL solo se casa con `sc->items` y no
 * se produce MESH (lo demás sigue siendo válido: los elementos llevan los datos de su paquete). Devuelve 0 o <0. */
int ns_fx_classify(const uint8_t *ram, const NsFxScene *sc, const GpuDrawList *dl, uint8_t *cls, uint8_t *sub, int32_t *item, NsFxClsStats *st);
/* Cobertura confirmada: gpu[i] = 1 si la operación debe seguir saliendo por la GPU. Lo es siempre que NO sea candidata (2D, EVENT, MAINT) o el renderer
 * no la haya confirmado (confirmed[i] == 0; confirmed == NULL = nada confirmado). Los eventos de VRAM siempre se aplican a la VRAM. Devuelve cuántas
 * operaciones salen por la GPU. */
uint32_t ns_fx_gpu_mask(const uint8_t *cls, const uint8_t *confirmed, uint32_t n, uint8_t *gpu);

/* APRENDIZAJE entre fotogramas (global, sin bloqueos: seguro entre hilos). Los sprites y las primitivas planas semitransparentes no traen en su paquete la página de
 * textura ni el modo de mezcla (los fija un comando de modo de dibujo aparte): sin ellos el renderer los dibujaría con la página 0. Tras clasificar una lista, ns_fx_learn
 * guarda por primitiva lo que la GPU usó para los elementos casados con esos datos desconocidos; ns_fx_extract lo aplica a los siguientes fotogramas
 * (NSFX_F_LEARNED). Devuelve cuántos elementos aprendió. */
uint32_t ns_fx_learn(const NsFxScene *sc, const GpuDrawList *dl, const int32_t *item);
void ns_fx_learn_reset(void);

/* Tras classify: rellena de las primitivas de la GPU casadas (item[i] = índice o -1) lo que el paquete no trae (página de textura, modo de mezcla).
 * Devuelve cuántos elementos se completaron. */
int ns_fx_resolve_items(NsFxScene *sc, const GpuDrawList *dl, const int32_t *item);
/* ---- casación TOLERANTE (animación entre fotogramas) ----
 * La lista de la GPU y la RAM de un mismo instante no tienen por qué ser del mismo paquete del motor: la RAM ya puede traer los paquetes del fotograma siguiente
 * (ondas, anillos, partículas se mueven unos píxeles) y la casación exacta de ns_fx_classify los deja como 2D. Esta segunda pasada reclasifica como FX las
 * operaciones que siguen siendo 2D, son polígonos 3D con posición de vista en TODOS los vértices (el HUD y los textos nunca la tienen) y coinciden con un elemento
 * cuadrilátero/triángulo de la escena por firma exacta (página de textura sin modo de mezcla, CLUT, semitransparencia, u/v del vértice 0, nº de vértices) y por
 * posición: los vértices de la operación (vista del motor de ESA lista) y los del elemento llevados a esa misma vista, a menos de 40 + 0,03 z unidades.
 * `lc` = cámara con la que se emitió la lista (NULL = la de la RAM: el elemento usa su `vpos`). Solo toca operaciones con cls == NSFX_OP_2D o NSFX_OP_MESH_GUESS (sin paquete exacto); devuelve cuántas cambió. */
typedef struct {
    float eye[3];         /* ojo del motor en el mundo */
    float M[9];           /* vista -> mundo, fila-mayor; columnas = der, abajo, adelante (como NsCamera.rot / VrCamRecord.M) */
} NsFxListCam;
uint32_t ns_fx_classify_near(const NsFxScene *sc, const NsFxListCam *lc, const GpuDrawList *dl, uint8_t *cls, int32_t *item);
/* Identidad estable de un elemento entre fotogramas (primitiva, paquete, segmento): 64 bits no nulos. La usa el renderer para confirmar lo dibujado. */
uint64_t ns_fx_item_key(const NsFxItem *it);
#define NSFX_KEY_MESH 1ull   /* clave especial de las operaciones MESH (paquete exacto de un objeto de malla) */

const char *ns_fx_role_name(int role);
const char *ns_fx_shape_name(int shape);
