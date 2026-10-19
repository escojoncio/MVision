#pragma once
/* ns_types: CONTRATO de la escena nativa (renderer nativo). Qué se extrae en cada fotograma de juego de la RAM del motor y cómo se
 * entrega al renderer. C99 puro, sin dependencias de plataforma ni del runtime; el mismo fichero lo ven el extractor (hilo de emulación),
 * el puente a Swift y las herramientas de escritorio. Contexto y origen de cada campo: docs/escena_nativa.md (léelo primero).
 *
 * Idea
 *   El motor del juego mantiene en RAM una LISTA DE OBJETOS por canal (modelos con matriz de mundo, el escenario es una lista de objetos del
 *   mismo tipo) y una lista de PRIMITIVAS de efectos con coordenadas de mundo. Una vez por fotograma de juego, justo después de que el motor
 *   calcule todas las matrices de mundo, el extractor lee esa lista y produce una NsScene: instancias (qué mallas, dónde, con qué luz) +
 *   mallas (cacheables, se envían una vez) + luces + efectos + cámara del juego. El renderer la dibuja con la cámara VR a la frecuencia del visor.
 *   La lógica, la IA y el sonido siguen siendo del juego; el HUD y los menús 2D siguen saliendo de la lista de la GPU (vr_scene, clase 2/3).
 *
 * Unidades y ejes (iguales a los del puente y vr_scene)
 *   Todo va en UNIDADES DEL MOTOR (enteros del juego pasados a float donde se indica) y en el MUNDO DEL MOTOR: Y arriba, 1 unidad = 1 mm
 *   nominal (NsScene.units_per_m = 1000; el personaje mide ~1650-1750). NO se convierte a metros ni a espacio del usuario en el extractor:
 *   NsScene.to_vr (3x4) lleva del mundo del motor al mundo VR con la MISMA regla que vr_scene (escala, espejo Z si hace falta, origen) y se
 *   rellena con vr_scene_opts/vrcam; el renderer aplica to_vr * (world_pieza * v_local) y después la pose de la cabeza (vrcam_delta / usermap).
 *   Ángulos del motor: 4096 = vuelta. Matrices del motor: rotación 3x3 s16 con 4096 = 1,0 (la rotación de mundo de una pieza puede llevar escala:
 *   p. ej. una sombra aplanada pone la escala en la diagonal; se pasa tal cual, como float).
 *
 * Entrega (triple búfer + caché de mallas en GPU)
 *   - Las MALLAS son inmutables y viven en un NsMeshStore propio del productor (arena con punteros estables, solo se añade). Una NsScene sólo
 *     referencia mallas por id (u64) y por tanto es pequeña (unos cientos de instancias): puede perderse o saltarse sin perder datos.
 *     id = ((uint64)dirección_de_la_definición_en_RAM << 32) | hash32 del CONTENIDO resuelto (vértices, normales, caras con UV/tpage/clut,
 *     colores precalculados). Si el motor reescribe una definición (cambio de zona) cambia el hash y por tanto el id: nunca hay que invalidar.
 *   - El consumidor mantiene su caché id -> búferes de GPU; para cada id nuevo llama a ns_store_get(). `mesh_epoch` sube cuando el productor vacía el
 *     almacén (cambio de zona/ejecutable): entonces el consumidor puede soltar toda su caché.
 *   - NsScene es memoria plana (arrays con capacidad) pensada para 3 slots rotativos: productor escribe el libre, el consumidor toma el más reciente
 *     completo; `seq` crece de 1 en 1, `game_frame` es el fotograma del motor. Nada de punteros a la RAM del juego dentro de la escena.
 *
 * Qué NO contiene (y por qué)
 *   - Texturas: salen de la VRAM, que el consumidor ya reconstruye por eventos con la lista de la GPU (vr_scene, VrBatch.ev_end). Aquí sólo
 *     tpage/clut/UV por cara. La paleta térmica/nocturna es un cambio de CLUT en VRAM: no afecta a este contrato.
 *   - Colores finales: se entrega la ENTRADA del sombreado (normales + luces + color precalculado) y el renderer evalúa ns_shade_*, así la luz
 *     responde a la posición real de cada pieza y no a la del último fotograma del motor. La fórmula está fijada y validada (docs §6).
 */
#include <stdint.h>
#include <stddef.h>

#define NS_VERSION 1u

/* ---------------------------------------------------------------------------------------------------------------- transformaciones */
/* 3x4 fila-mayor: v' = R*v + t.  m[0..2] = fila 0 (r00 r01 r02), m[3] = tx, m[4..6] fila 1, m[7] = ty, m[8..10] fila 2, m[11] = tz.
 * Para matrices del motor (s16 con 4096 = 1) el productor ya divide por 4096; la traslación va en unidades del motor. */
typedef struct { float m[12]; } NsXf;

/* ---------------------------------------------------------------------------------------------------------------- malla */
typedef struct {
    int16_t x, y, z;     /* espacio local de la pieza, unidades del motor */
    uint8_t weld;        /* 0xFF = ninguno; si no: índice de vértice del modelo PADRE (NsPiece.parent) cuya posición TRANSFORMADA sustituye a ésta
                          * (costura entre piezas articuladas: el motor copia el vértice ya transformado del padre). El renderer calcula primero los
                          * vértices del padre y luego los welded. Posición y, a falta de más datos, también la normal de color (docs §3.5). */
    uint8_t pad;
} NsVert;

typedef struct { int16_t x, y, z, pad; } NsNorm;   /* normal unitaria con 4096 = 1,0, espacio local */

enum {
    NS_FF_TRI = 1u,      /* triángulo: el motor repite el 3.er vértice (v[2] == v[3]); dibujar (A,B,C) */
    NS_FF_PAINT_OK = 2u  /* la cara tiene color precalculado válido en NsMesh.paint */
};

/* Cara = cuadrilátero A,B,C,D en orden PERÍMETRO (el motor lo guarda así; el hardware lo dibuja en orden Z: triángulos (A,B,D) y (A,D,C) del
 * vértice 0,1,2,3 = A,B,D,C). Triángulos para el renderer: (A,B,C) y (A,C,D). Anverso: nclip(A,B,D) > 0 en la proyección del motor, equivale a
 * dot(A_vista, cross(B-A, D-A)) > 0 con vista X derecha, Y abajo, Z delante (docs §3.4). Si el modelo es BOTHFACE se dibuja a doble cara. */
typedef struct {
    uint8_t v[4];        /* índices de vértice A,B,C,D (7 bits) */
    uint8_t n[4];        /* índices de normal A,B,C,D (7 bits) */
    uint8_t u[4], t[4];  /* texels (0..255) de A,B,C,D DENTRO de la página de textura del tpage (u = columna, t = fila), ya resueltos con el rectángulo de textura */
    uint16_t tpage;      /* formato GP0(E1h): bits 0-3 x/64, 4 y/256, 5-6 modo de mezcla, 7-8 profundidad (0=4b,1=8b,2/3=15b) */
    uint16_t clut;       /* (clx/16) | (cly << 6) */
    uint16_t texid;      /* id de textura del motor (depuración; 0 = sin textura -> cara plana) */
    uint8_t flags;       /* NS_FF_* */
    uint8_t pad;
} NsFace;

enum {
    NS_MF_TRANS = 1u,    /* semitransparente (modo de mezcla en tpage de cada cara) */
    NS_MF_UNLIT = 2u,    /* sin sombreado: color neutro 128 (o paint) */
    NS_MF_BOTHFACE = 4u, /* doble cara */
    NS_MF_INDIRECT = 8u, /* tiene vértices welded */
    NS_MF_PAINT = 16u,   /* usa NsMesh.paint (color precalculado por esquina, p. ej. el escenario) en vez de luz dinámica */
    NS_MF_RAISE_UP = 32u /* raise > 0: se dibuja por delante de otros de su misma profundidad (decals) */
};

typedef struct NsMesh {
    uint64_t id;
    uint32_t hash;       /* hash32 del contenido (parte baja de id) */
    uint32_t flags;      /* NS_MF_* */
    uint32_t model_flags;/* palabra cruda del motor (depuración) */
    uint16_t n_verts, n_faces, n_norms, pad;
    int16_t raise;       /* sesgo de orden de dibujo del motor (unidades de profundidad); 0 normal. Signo + = acercar */
    int16_t bbox[6];     /* min xyz, max xyz locales */
    const NsVert *verts;
    const NsNorm *norms;
    const NsFace *faces;
    const uint32_t *paint; /* NULL o n_faces*4 colores RGBA8 (r | g<<8 | b<<16 | a<<24; 128 = neutro) en orden A,B,C,D; ya incluyen luces fijas del mapa */
} NsMesh;

/* ---------------------------------------------------------------------------------------------------------------- instancias */
typedef struct {
    uint64_t mesh_id;    /* NsMesh.id */
    NsXf world;          /* local -> mundo del motor */
    int16_t parent;      /* índice de pieza (dentro de la misma instancia) cuyos vértices usan los welded de esta malla; -1 */
    uint16_t flags;      /* NS_PF_* */
} NsPiece;
enum { NS_PF_HIDDEN = 1u /* pieza oculta por el juego o por la capa VR (cabeza propia); no dibujar */ };

enum {
    NS_IF_SHADE = 1u,    /* se sombrea con NsLight (si no: neutro/paint) */
    NS_IF_AMBIENT = 2u,  /* ambiente propio (NsLight.amb) en vez del global */
    NS_IF_TRANS = 4u,    /* el objeto entero puede tener piezas semitransparentes (dibujar en el pase de transparentes) */
    NS_IF_STATIC = 8u,   /* escenario (objeto de mapa): no cambia entre fotogramas salvo cambio de grupo */
    NS_IF_THERMAL = 16u, /* admite textura térmica (CLUT alterno en VRAM; informativo) */
    NS_IF_ONEPIECE = 32u,/* todas las piezas comparten la misma matriz */
    NS_IF_ENVMAP = 64u   /* mapa de reflexión (raro; la normal se proyecta sobre la vista) */
};

typedef struct {
    uint32_t key;        /* identidad estable entre fotogramas: dirección del objeto en la RAM del motor (cambia al recrearlo) */
    uint32_t first_piece;
    uint16_t n_pieces;
    uint16_t flags;      /* NS_IF_* */
    uint16_t group;      /* grupo de mapa (máscara de bits); 0 = siempre */
    int16_t chanl;       /* canal del motor (0 = principal) */
    int16_t light;       /* índice en NsScene.lights, -1 si no */
    int16_t bound_mode;  /* modo de visibilidad del último fotograma completo del motor (0 = lo descartó; informativo: el renderer decide por su cámara) */
    uint32_t pad;
} NsInstance;

/* ---------------------------------------------------------------------------------------------------------------- luces */
/* Conjunto de luces tal como las consume el sombreado del motor (docs §6), en MUNDO del motor:
 *   dir[l][0..2] (l=0..2): dirección de la luz l, s16 con 4096 = 1,0; la fila trae ya el factor de atenuación de las puntuales (módulo <= 3072).
 *   col[c][l]: color de la luz l en el canal c (s16; col/16 = aportación 0..255 con intensidad 1,0).
 *   amb[c]: color ambiente 0..255 (el motor lo multiplica por 16 al cargarlo en la GTE).
 * Fórmula (docs §6): I_l = clamp(dot(dir[l]/4096, R*n) , 0, 7.99) ; c = clamp(amb[c] + sum_l col[c][l]/16 * I_l, 0, 255) ; la textura se modula con c/128.
 * `has_dyn` = alguna luz distinta de la principal está activa. */
typedef struct {
    int16_t dir[3][3];
    int16_t col[3][3];
    int16_t amb[3];
    uint16_t flags;      /* bit 0: has_dyn; bit 1: ambiente propio de la instancia */
} NsLight;

/* Niebla / atenuación por profundidad. El motor NO usa el depth-cue de la GTE: sólo hay niebla en las zonas con el actor de niebla, que sustituye
 * las etapas de canal y atenúa hacia NEGRO por profundidad de OBJETO (z de la pieza 0) y de primitiva. Rampa lineal en z de vista (unidades del motor):
 * f = clamp((z - start)/len, 0, 1); color = color*(1-f). `active` = 0 -> sin niebla. */
typedef struct {
    int32_t active;
    float start, len;    /* unidades del motor en profundidad de vista de la cámara del juego */
    float rgb[3];        /* color lejano 0..255 (siempre 0 en la edición analizada) */
} NsFog;

/* ---------------------------------------------------------------------------------------------------------------- efectos */
enum {
    NS_EK_QUAD = 0,      /* polígono/línea con vértices de mundo (pos[]) */
    NS_EK_BILLBOARD = 1  /* sprite cuadrilátero de tamaño en PÍXELES de la proyección del motor alrededor de un punto de mundo */
};
enum {
    NS_EF_SEMI = 1u,     /* semitransparente (modo abr) */
    NS_EF_TEXTURED = 2u,
    NS_EF_GOURAUD = 4u,
    NS_EF_ONESIDE = 8u,  /* sólo anverso */
    NS_EF_LINE = 16u,    /* línea (dibujar como cinta de ancho `px`) */
    NS_EF_ON_CAMERA = 32u/* posiciones en espacio de VISTA del motor (pegado a la cámara): el renderer las pega a la cabeza */
};
typedef struct {
    uint8_t kind;        /* NS_EK_* */
    uint8_t nv;          /* 2 (línea), 3 o 4 vértices (orden de paquete: 0,1,2,3 = Z; cuadrilátero = triángulos (0,1,2) y (1,2,3)) */
    uint16_t flags;      /* NS_EF_* */
    int16_t raise;       /* sesgo de orden */
    uint8_t abr;         /* modo de semitransparencia 0..3 */
    uint8_t pad;
    uint16_t tpage, clut;
    float p[4][3];       /* QUAD: vértices en mundo del motor (ya con la matriz del grupo aplicada); BILLBOARD: p[0] = centro */
    uint8_t rgb[4][3];   /* color por vértice 0..255 (128 = neutro con textura) */
    uint8_t uv[4][2];    /* texels en la página */
    float box[4];        /* BILLBOARD: caja en píxeles del motor respecto al centro proyectado: x0,y0 = (-rect.x,-rect.y), x1,y1 = (w-rect.x, h-rect.y) (y hacia abajo) */
    float ref_h;         /* BILLBOARD: distancia de pantalla H del motor con la que se midieron los píxeles; tamaño de mundo = px * z / ref_h con z = profundidad respecto a la cámara que se use */
    float width_px;      /* LINE: grosor aproximado en píxeles del motor */
    uint32_t src_key;    /* dirección de la primitiva del motor (identidad entre fotogramas) */
} NsEffect;

/* ---------------------------------------------------------------------------------------------------------------- cámara y escena */
typedef struct {
    float eye[3];        /* posición del ojo, mundo del motor */
    float rot[9];        /* vista -> mundo, fila-mayor; columnas = X derecha, Y abajo, Z delante (como VrCamRecord.M) */
    float h;             /* distancia de pantalla H del motor (320 normal) */
    float overscan_y;    /* 58/64: la proyección vertical del motor escala Y por este factor (ya aplicado a eye_inv en el motor) */
    int32_t vr_active;   /* 1 si esta cámara es la que la capa VR le dio al motor (cabeza); 0 la propia del juego */
} NsCamera;

enum {
    NS_SV_GAME = 1u,     /* hay partida/escena 3D con jugador (puente válido) */
    NS_SV_LISTS_OK = 2u, /* las listas de instancias/primitivas se leyeron enteras (si no: truncadas; ver `truncated`) */
    NS_SV_MAP_OK = 4u    /* hay objetos de escenario en la lista */
};

typedef struct NsScene {
    uint32_t version;    /* NS_VERSION */
    uint32_t seq;        /* contador de escenas publicadas (sube 1 en 1) */
    uint32_t game_frame; /* nº de fotograma del motor (hle_frame_count) */
    uint32_t vblank;     /* VBlanks desde el arranque en el momento de la extracción */
    uint64_t t_ns;       /* reloj monótono (ns) al extraer */
    uint32_t mesh_epoch; /* sube cuando el almacén de mallas se vacía */
    uint32_t valid;      /* NS_SV_* */
    uint32_t truncated;  /* bits: 1 instancias, 2 piezas, 4 luces, 8 efectos (se alcanzó la capacidad) */
    uint32_t clock;      /* GV_Clock: búfer doble del motor (0/1) */
    uint32_t group_mask; /* grupos de mapa activos */
    uint16_t area_id; uint16_t pad0;
    float units_per_m;   /* 1000 */
    float to_vr[12];     /* mundo del motor -> mundo VR (3x4 fila-mayor, metros); misma regla que vr_scene (docs §2) */
    NsCamera cam;
    NsFog fog;
    float bg_rgb[3];     /* color de fondo del canal de fondo 0..255 */
    NsLight global_light;/* luz global (el objeto por defecto del motor); lights[] la repite si alguna instancia la usa */

    uint32_t n_inst, cap_inst;     NsInstance *inst;
    uint32_t n_piece, cap_piece;   NsPiece *piece;
    uint32_t n_light, cap_light;   NsLight *lights;
    uint32_t n_effect, cap_effect; NsEffect *effects;
} NsScene;

/* ---------------------------------------------------------------------------------------------------------------- almacén de mallas */
typedef struct NsMeshStore NsMeshStore;   /* opaco: lo implementa el extractor. Lectura thread-safe; sólo el productor añade. */
/* Devuelve la malla o NULL si el id no está (caducó con mesh_epoch). El puntero y sus arrays valen hasta el siguiente vaciado del almacén. */
const NsMesh *ns_store_get(const NsMeshStore *st, uint64_t id);
uint32_t ns_store_epoch(const NsMeshStore *st);

/* ---------------------------------------------------------------------------------------------------------------- utilidades (cabecera, sin dependencias) */
static inline uint32_t ns_hash32(uint32_t h, const void *data, size_t n) {   /* FNV-1a 32 encadenable; h inicial = 2166136261u */
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}
#define NS_HASH_INIT 2166136261u
#define NS_MESH_ID(addr, hash) ((((uint64_t)(uint32_t)(addr)) << 32) | (uint64_t)(uint32_t)(hash))

static inline float ns_clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

/* Matriz del motor (s16[9] fila-mayor, 4096 = 1) + traslación s32[3] -> NsXf. */
static inline void ns_xf_from_engine(NsXf *o, const int16_t r[9], const int32_t t[3]) {
    for (int i = 0; i < 3; i++) {
        o->m[4 * i + 0] = r[3 * i + 0] * (1.0f / 4096.0f);
        o->m[4 * i + 1] = r[3 * i + 1] * (1.0f / 4096.0f);
        o->m[4 * i + 2] = r[3 * i + 2] * (1.0f / 4096.0f);
        o->m[4 * i + 3] = (float)t[i];
    }
}
static inline void ns_xf_apply(const NsXf *x, const float v[3], float o[3]) {
    o[0] = x->m[0] * v[0] + x->m[1] * v[1] + x->m[2] * v[2] + x->m[3];
    o[1] = x->m[4] * v[0] + x->m[5] * v[1] + x->m[6] * v[2] + x->m[7];
    o[2] = x->m[8] * v[0] + x->m[9] * v[1] + x->m[10] * v[2] + x->m[11];
}
/* Sólo rotación (para normales). */
static inline void ns_xf_rot(const NsXf *x, const float v[3], float o[3]) {
    o[0] = x->m[0] * v[0] + x->m[1] * v[1] + x->m[2] * v[2];
    o[1] = x->m[4] * v[0] + x->m[5] * v[1] + x->m[6] * v[2];
    o[2] = x->m[8] * v[0] + x->m[9] * v[1] + x->m[10] * v[2];
}

/* Sombreado por vértice-normal de una cara, EXACTO al del motor (docs §6, validado contra los paquetes de la GPU del motor):
 *   n_local -> normal de mundo = R_pieza * n (sin normalizar: la matriz del motor ya es ~ortonormal; con escala ver docs),
 *   I_l = clamp0( dot(dir_l, nw) / 4096 ) con tope 8,0 (saturación del registro de 16 bits de la GTE: 0x7FFF/4096),
 *   c   = clamp(amb + sum col_l/16 * I_l, 0, 255).   Salida 0..255 por canal; el hardware modula la textura con c/128.
 * `rw` = rotación 3x3 de mundo de la pieza (fila-mayor, 4096 = 1 ya dividido -> floats), n = normal s16/4096 en espacio local. */
static inline void ns_shade_normal(const NsLight *L, const float rw[9], const float n[3], float out_rgb[3]) {
    float nw[3] = { rw[0]*n[0] + rw[1]*n[1] + rw[2]*n[2], rw[3]*n[0] + rw[4]*n[1] + rw[5]*n[2], rw[6]*n[0] + rw[7]*n[1] + rw[8]*n[2] };
    float I[3];
    for (int l = 0; l < 3; l++) {
        float d = (L->dir[l][0] * nw[0] + L->dir[l][1] * nw[1] + L->dir[l][2] * nw[2]) * (1.0f / 4096.0f);
        I[l] = ns_clampf(d, 0.0f, 8.0f);
    }
    for (int c = 0; c < 3; c++) {
        float v = (float)L->amb[c] + (L->col[c][0] * I[0] + L->col[c][1] * I[1] + L->col[c][2] * I[2]) * (1.0f / 16.0f);
        out_rgb[c] = ns_clampf(v, 0.0f, 255.0f);
    }
}
