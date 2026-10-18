/* Contrato plano (C puro, sin dependencias) entre la capa de armas/manos (vr_hands.c) y el renderer Metal
 * (app/Sources/RenderWeapon.swift). Es lo único que necesita el puente Swift: añadir `#include "vr_hands_frame.h"`
 * al Bridging-Header. Unidades: metros; mundo VR = el mismo de la cabeza y de las matrices `views` del renderer
 * (Y arriba, -Z delante).
 *
 * Qué entrega cada frame (VrHandsFrame): una lista de mallas a dibujar. Cada malla es una pieza del arma
 * (corredera, cargador, supresor...) o un objeto suelto (cargador en la mano libre, vaina expulsada...), con su
 * transformación 4x4 local->mundo, sus vértices/índices (propiedad del modelo enlazado: estables mientras el arma
 * esté enlazada) y el id de su textura.
 *
 * Caché en el renderer:
 *   - mesh_id identifica la geometría (vértices+índices): el renderer la sube UNA vez (la geometría no cambia
 *     entre frames; solo cambia `model`). Un id nunca se reutiliza para otra geometría (contador global).
 *   - tex_id identifica el atlas RGBA8 (ya escalado): se sube una vez. 0 = sin textura (color de vértice).
 *   - Los ids que dejan de aparecer pueden descartarse pasados unos frames.
 *
 * Vida de los punteros: `meshes`/`tex` y los datos de vértices valen hasta que el productor publique otro frame
 * (mv_host_hands copia a un búfer propio del consumidor, ver vr_hands.h). Las mallas deben seguir vivas hasta que
 * el renderer haya consumido el último frame en el que aparecen. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pose de mano en el espacio VR. rot = cuaternión (x, y, z, w). valid = 0: mano sin seguimiento. */
typedef struct { int32_t valid; float pos[3]; float rot[4]; } VrHandsPose;

/* Vértice: 36 bytes, sin relleno (idéntico en memoria a WVertex de vr/weapons/weapon.h). En MSL:
 * packed_float3 pos; packed_float3 nrm; packed_float2 uv; uchar4 rgba (255 = neutro). uv: v = 0 es la fila superior. */
typedef struct { float pos[3]; float nrm[3]; float uv[2]; uint8_t rgba[4]; } VrHandsVertex;
typedef char vrhands_check_vertex_size[(sizeof(VrHandsVertex) == 36) ? 1 : -1];

enum { VRH_MF_HELD = 1 };   /* VrHandsMesh.flags: objeto suelto (no es pieza del arma) */

typedef struct {
    uint32_t mesh_id;
    uint32_t tex_id;                 /* 0 = sin textura */
    float model[16];                 /* columna-mayor (simd_float4x4): local -> mundo VR; incluye la escala unidades->metros */
    const VrHandsVertex *verts; uint32_t nverts;
    const uint16_t *idx; uint32_t nidx;     /* triángulos (lista) */
    uint32_t flags;                  /* VRH_MF_* */
} VrHandsMesh;

typedef struct {
    uint32_t id;
    uint32_t w, h;
    const uint8_t *rgba;             /* w*h*4, RGBA8 no premultiplicado, gamma (valores del juego) */
} VrHandsTex;

typedef struct {
    uint32_t seq;                    /* sube en cada frame publicado (el renderer puede detectar repetición) */
    int32_t nmeshes; const VrHandsMesh *meshes;
    int32_t ntex; const VrHandsTex *tex;
    /* iluminación suave estilo juego: luz direccional fija en el mundo (dirección HACIA la luz, unitaria) */
    float light_dir[3];
    float ambient, diffuse;          /* color = textura * vértice * (ambient + diffuse * lambert_suave) */
} VrHandsFrame;

/* Lo implementa el integrador en host (o se usa el buzón de vr_hands.c, ver vrhands_post). Devuelve 1 si hay algo que
 * dibujar en `out` (nmeshes > 0), 0 si no. Se llama desde el hilo de render una vez por frame. */
int mv_host_hands(VrHandsFrame *out);
/* El renderer lo llama cuando termina de usar el frame de la última mv_host_hands (también vale si esta devolvió 0). Hasta
 * entonces, `out` y todo lo que referencia (vértices, índices, atlas) debe seguir vivo. Implementación débil en vr_hands.c. */
void mv_host_hands_done(void);

#ifdef __cplusplus
}
#endif
