/* wcut: herramienta de "corte" en ejecución. Separa un modelo cargado (WLoaded, una sola pieza rígida) en
 * piezas articulables (cargador, supresor, corredera, palanca, tapa...) SIN modelar geometría nueva: cada pieza
 * sale de los triángulos y la textura originales; donde el original no tiene una pieza propia se extrae el
 * volumen correspondiente del cuerpo y se cierra con "tapas" (polígonos de contorno de corte, triangulados) para
 * que la pieza tenga grosor y el hueco que deja no se vea vacío.
 *
 * Los datos del juego NO viajan con la app: aquí solo hay el algoritmo y (en wcut_tables.c) regiones paramétricas
 * pequeñas (cajas orientadas, máscaras de componente, rangos de UV); nunca vértices ni texturas.
 *
 * Marco de trabajo: el de wload en W_FRAME_HELD, CORREGIDO (ver WCutModel.fix): +Y arriba, -Z hacia la boca,
 * +X derecha; metros; triángulos antihorarios hacia fuera con normales hacia fuera. Las coordenadas de las tablas
 * se escriben en milímetros con las macros de abajo.
 *
 * Nota sobre wload.h: allí se promete "antihorario hacia fuera", pero lo MEDIDO en la salida actual de wload
 * (W_FRAME_HELD) es lo contrario (orden horario hacia fuera, normales hacia dentro, arma boca abajo). El cortador
 * lo corrige al ingerir con WCutModel.fix; además normaliza la orientación por componente conexa (volumen con
 * signo) y por triángulo (orden frente a normales guardadas), así que un wload futuro ya corregido no se
 * "corrige dos veces": solo habría que quitar los flags de las tablas (UPRIGHT sí es una rotación y no se
 * detecta sola; el test comprueba volumen con signo >= 0 y normales coherentes en cada pieza).
 *
 * Uso típico:
 *   WLoaded m; wload_model(dir, len, stage, id, &opts, &m);        // opts.frame = W_FRAME_HELD
 *   WCutSet s;  if (wcut_run(&m, wcut_find(id), &s) == 0) {
 *       weapon_bind(state, s.meshes, s.nmeshes, s.parts, s.nparts); // las meshes siguen el orden de mesh_names
 *       ... s.atlas / s.atlas_w / s.atlas_h es la textura (copia propia); wcut_free(&s) al terminar
 *   }
 * Cada mesh va EXPRESADA RELATIVA A SU PIVOTE; la WPart correspondiente lleva rest.p = pivote - pivote del padre
 * (el cuerpo tiene pivote en el origen del modelo), así que con rest + canal la pieza queda en su sitio. */
#pragma once
#include "wload.h"
#include <math.h>

/* ---- unidades de las tablas ---- */
#define WCUT_MM(v)  ((v) * 0.001f)
#define WCUT_DEG(v) ((v) * 0.017453292519943295f)
#define W_AUTO      NAN   /* en un pivote: usar el centro de la caja envolvente de la pieza en ese eje */

/* Caja orientada: centro, semiextensiones y giro XYZ (R = Rz*Ry*Rx). Una semiextensión grande = sin límite en ese eje. */
typedef struct { WVec3 c, h, rot; } WCutBox;
#define WCUT_BOX(cx, cy, cz, hx, hy, hz, rxd, ryd, rzd) \
    { {WCUT_MM(cx), WCUT_MM(cy), WCUT_MM(cz)}, {WCUT_MM(hx), WCUT_MM(hy), WCUT_MM(hz)}, \
      {WCUT_DEG(rxd), WCUT_DEG(ryd), WCUT_DEG(rzd)} }

/* Modo de aplicar la caja:
 *  WCUT_CLIP     recorta geométricamente (parte los triángulos que cruzan, UV/normales interpoladas) y tapa;
 *  WCUT_WHOLE    toma solo los triángulos con los 3 vértices dentro (sin partir; para piezas ya separadas);
 *  WCUT_CENTROID toma los triángulos cuyo centroide cae dentro. */
typedef enum { WCUT_CLIP = 0, WCUT_WHOLE = 1, WCUT_CENTROID = 2 } WCutMode;

typedef struct { float u0, v0, u1, v1; } WCutUV;     /* rectángulo en UV normalizada del atlas (v=0 arriba) */
/* Rectángulo en texels de un atlas de w x h (sin escalar). */
#define WCUT_UVR(x0, y0, x1, y1, w, h) { (x0) / (float)(w), (y0) / (float)(h), (x1) / (float)(w), (y1) / (float)(h) }

/* Selección = caja Y componentes Y rango de UV (los criterios ausentes no filtran). */
typedef struct {
    int has_box; WCutBox box; WCutMode mode;
    uint32_t comps;       /* máscara de componentes conexas del modelo (bit i = i-ésima por orden de 1.er triángulo); 0 = todas */
    int has_uv; WCutUV uv;/* solo triángulos cuyo centroide UV cae dentro */
} WCutSel;

enum {
    WCUT_COPY      = 1,   /* duplica: el origen conserva su geometría (cargador de repuesto, anilla, cartucho) */
    WCUT_CAP_OPEN  = 2,   /* además tapa los contornos abiertos de la pieza que no tienen pareja en el origen */
    WCUT_NO_HOLE   = 4,   /* no rellena el hueco que deja en el origen */
    WCUT_NO_CAP    = 8    /* no tapa la pieza */
};

/* Generador de cargador (pistolas): la pieza extraída es solo la base (franja de piel original del fondo del mango).
 * Sobre su cara superior (el plano de corte, normal +Y) se construye un cargador completo derivado del propio modelo:
 *  - sección = contorno de la base desplazado hacia dentro 'wall_frac' del ancho de la base (grosor de pared); el cuerpo
 *    es ese contorno cizallado a lo largo de 'dir' (la inclinación del mango: las paredes del mango no son verticales);
 *  - altura: el cargador sube hasta la cara inferior de la pieza 'slide' menos 'clear' (holgura) menos lo que la bala
 *    asoma por encima de los labios: el último 'lip_frac' de la altura los labios se estrechan en x ('lip_keep');
 *  - arriba, ranura de alimentación (recorte rectangular de profundidad 'rec_frac' * radio de la vaina, con fondo) y un
 *    cartucho en ella; su calibre sale del ánima del cañón generado ('bore': radio del ánima = radio de la bala; vaina =
 *    'case_frac' veces el radio; longitudes en diámetros de vaina). Es volumen cerrado: la base y el cargador forman una
 *    sola cáscara y el cartucho otra, solapada con ella. Materiales: bloques del atlas del propio modelo. */
typedef struct {
    int on;
    const char *slide, *bore;
    WVec3 dir;                              /* eje del cargador hacia arriba (unitario) */
    float clear, wall_frac, lip_frac, lip_keep, rec_frac, case_frac, case_len, bullet_len;
    WCutUV mat_body, mat_case, mat_bullet;
} WCutMag;

/* Generador de cañón: tubo hueco (con ánima) bajo una pieza de referencia. La cara frontal (z mínima) de 'ref' se
 * perfora con un canal cilíndrico del radio exterior (la pieza de referencia gana un orificio con fondo oscuro) y el
 * cañón, un poco más fino, queda dentro con la boca a ras de esa cara. El radio sale de la sección frontal de 'ref':
 * radio = r_frac * (semilado menor); ánima = bore_frac * radio del cañón. */
typedef struct {
    int on;
    const char *ref;                 /* pieza que ya existe (regla anterior) */
    float axis_y;                    /* altura del eje (x = centro de la sección frontal) */
    float r_frac, bore_frac;
    float length, bore_depth, gap;   /* m: longitud desde la boca hacia +Z; profundidad del ánima; holgura radial */
    int segs;
} WCutTube;

typedef struct {
    const char *name;     /* nombre de la pieza = entrada de mesh_names del arma */
    const char *src;      /* pieza de la que se extrae; NULL = el cuerpo (lo que queda) */
    WCutSel sel;
    unsigned flags;
    WCutUV cap_piece;     /* material de la tapa de la pieza (zona del atlas; rectángulo degenerado = color plano) */
    WCutUV cap_hole;      /* material del relleno del hueco (p.ej. interior oscuro del pozo) */
    WVec3 pivot;          /* en el marco del modelo; componentes W_AUTO = centro de la caja envolvente */
    const char *parent;   /* pieza padre para rest (NULL = raíz) */
    WChannel channel; WVec3 axis; int rotates;   /* como WPart */
    const char *note;     /* decisión de diseño (documentación) */
    WCutMag mag;          /* on: cargador generado sobre la pieza extraída (ver arriba) */
    WCutTube tube;        /* on: la regla genera un cañón en lugar de seleccionar triángulos */
} WCutRule;

/* Correcciones sobre lo que entrega wload (medido sobre los modelos reales):
 *  WCUT_FIX_UPRIGHT  las armas salen boca abajo en W_FRAME_HELD (el lomo/mira hacia -Y): se giran 180 grados
 *                    sobre Z (x,y -> -x,-y), giro propio (no espeja);
 *  WCUT_FIX_WINDING  los triángulos salen con el orden horario hacia fuera y las normales hacia dentro: se
 *                    invierte el orden (v1<->v2) y se niegan las normales. */
enum { WCUT_FIX_UPRIGHT = 1, WCUT_FIX_WINDING = 2 };

/* Caja envolvente aproximada del modelo ya corregido (tolerancia de comprobación: WCUT_BB_TOL). */
typedef struct { WVec3 lo, hi; } WCutBounds;
#define WCUT_BB(x0, y0, z0, x1, y1, z1) { {WCUT_MM(x0), WCUT_MM(y0), WCUT_MM(z0)}, {WCUT_MM(x1), WCUT_MM(y1), WCUT_MM(z1)} }
#define WCUT_BB_TOL 0.003f   /* m: margen de la comprobación de caja (las tablas la redondean al mm) */

/* Sellado de una pieza resultante: sus contornos abiertos (aristas sin gemela: la boca hueca de un cañon, el hueco del
 * original que ocupaba otra superficie) se tapan con un parche del material 'mat' (bloque del atlas). Se aplica tras todas
 * las reglas, sobre la pieza ya cortada; no modifica ninguna otra. */
typedef struct { const char *piece; WCutUV mat; const char *note; } WCutSeal;

typedef struct {
    uint16_t id;          /* id16 del modelo */
    const char *tag;      /* rol neutro: "pist", "rifle"... */
    int fix;              /* WCUT_FIX_*: correcciones al formato de salida actual de wload (ver arriba) */
    const char *body;     /* nombre de la pieza que queda del cuerpo */
    WVec3 body_pivot;
    const char *const *names;       /* orden de salida (mesh_names del arma), NULL-terminado */
    const WCutRule *rules; int nrules;
    const char *note;
    const char *weapon;   /* id del arma del registro cuyos nombres/canales/ejes debe seguir (NULL = ninguna); lo comprueba el test */
    uint32_t src_tris;    /* nº de triángulos esperado del modelo; 0 = no comprobar. Si no coincide: wcut_run devuelve -4 */
    WCutBounds bounds;    /* caja envolvente esperada (aprox.); ignorada si src_tris == 0 */
    const WCutSeal *seals; int nseals;
} WCutModel;

/* Origen de cada triángulo de una pieza. */
enum { WCUT_TRI_SKIN = 0,   /* piel original (o recortada) del modelo */
       WCUT_TRI_CAP  = 1,   /* tapa de la propia pieza (material cap_piece) */
       WCUT_TRI_FILL = 2 }; /* relleno de un hueco dejado por otra pieza (material cap_hole) */

typedef struct {
    char name[24];
    WVertex *v; uint16_t *idx;      /* triángulos sin indexar: nv = ni; posiciones en el marco del modelo (NO relativas) */
    uint32_t nv, ni;
    WVec3 pivot, bmin, bmax;
    uint32_t ncap;                  /* triángulos de tapa incluidos (kind != WCUT_TRI_SKIN) */
    uint8_t *kind;                  /* por triángulo (ni/3 entradas): WCUT_TRI_* */
    int copy;                       /* 1 si es un duplicado (no cuenta para la suma de volúmenes) */
    double gen_vol;                 /* m3 netos de geometría generada (cañón, cargador, canal): volumen total - gen_vol = volumen original */
} WCutPiece;

typedef struct {
    WCutPiece *piece; int npiece;   /* en el orden de WCutModel.names (huecos vacíos incluidos), luego las extra */
    WMesh *meshes; int nmeshes;     /* paralelas a piece; vértices RELATIVOS al pivote; nv == 0 = hueco vacío */
    WVertex **mv; uint16_t **mi;    /* buffers de las meshes (propios) */
    WPart *parts; int nparts;       /* una por regla + cuerpo; mesh = índice en meshes */
    uint8_t *atlas; uint32_t atlas_w, atlas_h;   /* copia del atlas del modelo */
    uint32_t src_tris;
} WCutSet;

/* Tabla de regiones por modelo (wcut_tables.c). */
const WCutModel *wcut_find(uint16_t id);
int wcut_model_count(void);
const WCutModel *wcut_model_at(int i);

/* Corta 'm' según 'model'. 0 = bien. Errores (siempre deja 'out' a cero, así que wcut_free es seguro):
 *   -1 argumentos o modelo cargado inválido (idx NULL, ni != nv, ni % 3, nv < 3, índice >= nv, posiciones/normales/UV no
 *      finitas, coordenadas > 10 km) o tabla inválida;
 *   -2 regla con origen desconocido;
 *   -3 memoria o presupuesto agotado (tope global de triángulos 4*src+4096, empujes y trabajo acotados);
 *   -4 el modelo no es el que describe la tabla (nº de triángulos o caja envolvente distintos);
 *   -5 una pieza supera los 65535 vértices (índices de 16 bits);
 *   -6 una regla usa máscara de componentes y el modelo tiene más de 32;
 *   -7 una tapa no se pudo triangular (contornos anidados incoherentes).
 * 'm' no se modifica. Las correcciones de model->fix se aplican al ingerir (las piezas salen ya corregidas). */
int  wcut_run(const WLoaded *m, const WCutModel *model, WCutSet *out);
void wcut_free(WCutSet *s);              /* seguro tras un error y con un WCutSet a cero */
const WCutPiece *wcut_piece(const WCutSet *s, const char *name);
int  wcut_piece_index(const WCutSet *s, const char *name);

/* Utilidades de comprobación. */
uint32_t wcut_open_edges(const WCutPiece *p);       /* aristas dirigidas sin pareja (0 = pieza cerrada) */
double   wcut_volume(const WCutPiece *p);           /* volumen firmado (m3) por divergencia; vale para piezas cerradas */
uint32_t wcut_degenerate(const WCutPiece *p);       /* triángulos de área ~0 */
