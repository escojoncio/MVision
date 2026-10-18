/* wload: cargador en tiempo de ejecución de modelos del motor original, leídos del archivo de stages del usuario.
 * C99 sin dependencias de plataforma. Los datos NO viajan con la app: se leen del disco del usuario.
 *
 * Uso típico:
 *   WLoadOpts o; wload_default_opts(&o);
 *   WLoaded m;   if (wload_model(dir, dir_len, stage, id, &o, &m) == 0) { ... m.mesh ...; wload_free(&m); }
 *
 * Marco de salida (opts.frame):
 *   W_FRAME_HELD   (por defecto) +Y arriba, -Z hacia delante (boca del arma), +X derecha; metros.
 *                  Mapeo desde el marco del motor: (x, y, z) -> (x, -z, y), pensado para los modelos de
 *                  arma/objeto, cuyo eje largo es Y del motor con la boca hacia -Y.
 *   W_FRAME_ENGINE marco del motor sin rotar (+Y arriba), solo escalado.
 * Triángulos con orientación antihoraria hacia fuera (se corrige con las normales guardadas del modelo).
 * UV: v=0 es la fila superior del atlas. Un único atlas RGBA8 por modelo (todas las texturas empaquetadas).
 */
#pragma once
#include "weapon.h"

typedef enum { W_FRAME_HELD = 0, W_FRAME_ENGINE = 1 } WFrame;

typedef struct {
    WFrame frame;
    float scale;          /* metros por unidad del motor (defecto 0.001) */
    float smooth_deg;     /* >0: normales suavizadas entre caras con ángulo menor que este (grados); 0 = las del modelo */
    int   upscale;        /* factor de escalado de textura: 1 (nada), 2 o 4 (filtro tipo xBR, desactivable) */
    int   key_black;      /* 1: el negro puro de la paleta es transparente (alpha 0) */
    const char *resident; /* stage con recursos compartidos donde buscar texturas (NULL = ninguno); lo pasa la app */
    int   split_parts;    /* 1: separa en piezas por componentes conexas (para articular) */
} WLoadOpts;

typedef struct {          /* pieza = rango contiguo de triángulos del índice */
    uint32_t first_tri, ntri;
    WVec3 center, bmin, bmax;   /* en el marco de salida, metros */
} WLoadPart;

typedef struct {
    WMesh mesh;           /* mesh.v / mesh.idx apuntan a los buffers de abajo; mesh.texture = 1 si hay atlas */
    WVertex *v; uint16_t *idx;
    uint32_t nv, ni;
    uint8_t *atlas;       /* RGBA8, atlas_w * atlas_h (NULL si el modelo no tiene texturas) */
    uint32_t atlas_w, atlas_h;
    uint32_t ntex;        /* texturas empaquetadas */
    WLoadPart *parts; uint32_t nparts;   /* 1 pieza si split_parts == 0 */
    WVec3 bmin, bmax;
} WLoaded;

void wload_default_opts(WLoadOpts *o);

/* Carga el modelo 'id' (id16 del nombre) del stage 'stage' del archivo de stages en memoria (dir, dir_len).
 * Busca el modelo en la caché y en los bloques n/r del stage; las texturas, en el stage.
 * Devuelve 0 si va bien; <0: -1 stage/modelo no encontrado, -2 formato, -3 memoria, -4 demasiados vértices (>65535). */
int  wload_model(const uint8_t *dir, size_t dir_len, const char *stage, uint16_t id,
                 const WLoadOpts *o, WLoaded *out);
/* Igual pero con el modelo ya extraído (buffer de modelo) y buscando texturas en 'stage' y resident. */
int  wload_model_buf(const uint8_t *dir, size_t dir_len, const char *stage,
                     const uint8_t *mdl, size_t mdl_len, const WLoadOpts *o, WLoaded *out);
void wload_free(WLoaded *m);

/* Utilidades. */
uint16_t wload_id16(const char *name);   /* hash de nombre -> id de modelo/textura */
/* Escala x2 con filtro de arista tipo xBR (variante propia); src/dst RGBA8, dst = (2w)x(2h). */
void wload_scale2x(const uint8_t *src, int w, int h, uint8_t *dst);
